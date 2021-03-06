/*********************************************************
 * Copyright (C) 1998-2017 VMware, Inc. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation version 2 and no later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 *
 *********************************************************/

/*
 * task.c --
 *
 *      Task initialization and switching routines between the host
 *      and the monitor.
 *
 *      A task switch:
 *          -saves the EFLAGS,CR0,CR2,CR4, and IDT
 *          -jumps to code on the shared page
 *              which saves the registers, GDT and CR3
 *              which then restores the registers, GDT and CR3
 *          -restores the IDT,CR0,CR2,CR4 and EFLAGS
 *
 *      This file is pretty much independent of the host OS.
 *
 */

#ifdef linux
/* Must come before any kernel header file --hpreg */
#   include "driver-config.h"
#   include <linux/string.h> /* memset() in the kernel */

#   define EXPORT_SYMTAB
#   if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 12, 0)
#      define LINUX_GDT_IS_RO
#   endif
#else
#   include <string.h>
#endif

#include "vmware.h"
#include "modulecall.h"
#include "vmx86.h"
#include "task.h"
#include "vm_asm.h"
#include "cpuid.h"
#include "hostif.h"
/* On Linux, must come before any inclusion of asm/page.h --hpreg */
#include "hostKernel.h"
#include "comport.h"
#include "crossgdt.h"
#include "x86svm.h"
#include "x86vt.h"
#include "x86vtinstr.h"
#include "apic.h"
#include "x86perfctr.h"
#include "x86paging_common.h"
#include "x86paging_64.h"
#include "memtrack.h"

#ifdef LINUX_GDT_IS_RO
#   include <asm/desc.h>
#   define default_rw_gdt get_current_gdt_rw()
#else
#   define default_rw_gdt NULL
#endif

#if defined(_WIN64)
#   include "x86.h"
#   include "vmmon-asm-x86-64.h"
#   define USE_TEMPORARY_GDT 1
#else
/* It is OK to set this to 1 on 64-bit Linux/Mac OS for testing. */
#   define USE_TEMPORARY_GDT 0
#endif

#define TS_ASSERT(t) do { \
   DEBUG_ONLY(if (!(t)) TaskAssertFail(__LINE__);)  \
} while (0)

static CrossGDT *crossGDT = NULL;
static MPN crossGDTMPNs[CROSSGDT_NUMPAGES];
static Selector kernelStackSegment = 0;
static uint32 dummyLVT;
static Atomic_uint64 hvRootPage[MAX_PCPUS];
static Atomic_Ptr tmpGDT[MAX_PCPUS];
static Bool pebsAvailable = FALSE;
static Bool ptAvailable = FALSE;

#define L1_FLAGS      (PTE_RW | PTE_A | PTE_D | PTE_P)
#define L2_FLAGS      (PTE_RW | PTE_A | PTE_D)
#define L3_FLAGS      (PTE_RW | PTE_A | PTE_D)
#define L4_FLAGS      (PTE_RW | PTE_A | PTE_D)

#define CANONICAL_MASK MASK64(36)

/*
 * Index of the PTE in the given PT level when translating the given
 * LPN. This index is global, i.e. it considers the full page table
 * hierarchy, not just the containing page table.
 */
#define PTE_GLOBAL_INDEX(_lpn, _lvl) (((_lpn) & CANONICAL_MASK) >> ((_lvl) - 1) * 9)

/*
 * Index of the PTE in the page table at the given PT level when
 * translating the given LPN.
 */
#define PTE_INDEX(_lpn, _lvl) (PTE_GLOBAL_INDEX(_lpn, _lvl) & PT_OFF_MASK)


/*
 *-----------------------------------------------------------------------------
 *
 * TaskAllocHVRootPage --
 *
 *      Allocate and initialize an HV root page.  Upon success, race to be
 *      the first to store the allocated MPN in '*slot'.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      When the call returns, '*slot' contains the MPN of an HV root page if
 *      a thread succeeded, or INVALID_MPN if all threads failed.
 *
 *-----------------------------------------------------------------------------
 */

static void
TaskAllocHVRootPage(Atomic_uint64 *slot) // IN/OUT
{
   uint32 *content;
   uint64 vmxBasicMSR;
   MPN mpn;
   static const MPN invalidMPN = INVALID_MPN;

   ASSERT(slot != NULL);

   /* Allocate the page contents. */
   content = HostIF_AllocKernelMem(PAGE_SIZE, TRUE);
   if (content == NULL) {
      Warning("%s: Failed to allocate content.\n", __FUNCTION__);
      return;
   }

   /*
    * On VMX-capable hardware, write the VMCS revision identifier at the
    * beginning of the HV root page.  On SVM-capable hardware, the HV root
    * page is just initialized to zeroes.
    */
   memset(content, 0, PAGE_SIZE);
   if (HostIF_SafeRDMSR(MSR_VMX_BASIC, &vmxBasicMSR) == 0) {
      *content = LODWORD(vmxBasicMSR);
   }

   /* Allocate the HV root page. */
   mpn = HostIF_AllocMachinePage();

   if (mpn != INVALID_MPN) {
      /*
       * Store the MPN of the HV root page. This is done atomically, so if
       * several threads concurrently race and call TaskAllocHVRootPage() with
       * the same 'slot', only the first one to pass this finish line will win.
       */
      if (HostIF_WriteMachinePage(mpn, PtrToVA64(content)) != 0 ||
          !Atomic_CMPXCHG64(slot, invalidMPN, mpn)) {
         /*
          * Either we couldn't set up the page or this thread lost the race.
          * We must free its HV root page.
          */
         Warning("%s: Failed to setup page mpn=%llx.\n",
                 __FUNCTION__, (long long unsigned)mpn);
         HostIF_FreeMachinePage(mpn);
      }
   } else {
      Warning("%s: Failed to allocate page.\n", __FUNCTION__);
   }

   HostIF_FreeKernelMem(content);
}


/*
 *-----------------------------------------------------------------------------
 *
 * TaskGetHVRootPage --
 *
 *      Lazily allocate an HV root page, and return its MPN.
 *
 * Results:
 *      On success: The MPN of the HV root page.
 *      On failure: INVALID_MPN.
 *
 * Side effects:
 *      Might allocate memory, and transition '*slot' from
 *      INVALID_MPN to a valid MPN.
 *
 *-----------------------------------------------------------------------------
 */

static MPN
TaskGetHVRootPage(Atomic_uint64 *slot) // IN/OUT
{
   MPN mpn = Atomic_Read64(slot);

   if (mpn != INVALID_MPN) {
      return mpn;
   }

   TaskAllocHVRootPage(slot);

   return Atomic_Read64(slot);
}


/*
 *-----------------------------------------------------------------------------
 *
 * Task_GetHVRootPageForPCPU --
 *
 *      Lazily allocate the HV root page for a pCPU, and return its MPN.
 *      This is used for the VMXON region on Intel/VIA hardware and the
 *      host save area on AMD hardware.
 *
 * Results:
 *      On success: The MPN of the HV root page.
 *      On failure: INVALID_MPN.
 *
 * Side effects:
 *      Might allocate memory, and transition 'hvRootPage[pCPU]' from
 *      INVALID_MPN to a valid MPN.
 *
 *-----------------------------------------------------------------------------
 */

MPN
Task_GetHVRootPageForPCPU(uint32 pCPU) // IN
{
   ASSERT(pCPU < ARRAYSIZE(hvRootPage));

   return TaskGetHVRootPage(&hvRootPage[pCPU]);
}


/*
 *-----------------------------------------------------------------------------
 *
 * TaskAllocGDT --
 *
 *      Allocate a GDT. Upon success, race to be the first to store its base in
 *      '*slot'.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      When the call returns, '*slot' contains the base of a GDT if a thread
 *      succeeded, or NULL if all threads failed.
 *
 *-----------------------------------------------------------------------------
 */

static void
TaskAllocGDT(Atomic_Ptr *slot) // IN/OUT
{
   Descriptor *base;

   ASSERT(slot);

   /* Allocate a GDT. */
   base = HostIF_AllocKernelMem(0x10000 /* Maximal GDT size */, TRUE);
   if (!base) {
      Warning("%s: Failed to allocate temporary GDT.\n", __FUNCTION__);
      return;
   }

   /*
    * Store the base of the GDT. This is done atomically, so if several threads
    * concurrently race and call TaskAllocGDT() with the same 'slot', only the
    * first one to pass this finish line will win.
    */

   if (Atomic_ReadIfEqualWritePtr(slot, NULL, base)) {
      /* This thread lost the race. It must free its GDT. */
      HostIF_FreeKernelMem(base);
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * TaskGetGDT --
 *
 *      Lazily allocate a GDT, and return its base.
 *
 * Results:
 *      On success: The base of the GDT.
 *      On failure: NULL.
 *
 * Side effects:
 *      Might allocate memory, and transition '*slot' from NULL to a valid
 *      base.
 *
 *-----------------------------------------------------------------------------
 */

static Descriptor *
TaskGetGDT(Atomic_Ptr *slot) // IN/OUT
{
   Descriptor *base = Atomic_ReadPtr(slot);

   if (base) {
      return base;
   }

   TaskAllocGDT(slot);

   return Atomic_ReadPtr(slot);
}


/*
 *-----------------------------------------------------------------------------
 *
 * Task_GetTmpGDT --
 *
 *      Lazily allocate the temporary GDT for a pCPU, and return its base.
 *
 * Results:
 *      On success: The base of the temporary GDT.
 *      On failure: NULL.
 *
 * Side effects:
 *      Might allocate memory, and transition 'tmpGDT[pCPU]' from NULL to a
 *      valid base.
 *
 *-----------------------------------------------------------------------------
 */

Descriptor *
Task_GetTmpGDT(uint32 pCPU) // IN
{
   ASSERT(pCPU < ARRAYSIZE(tmpGDT));

   return TaskGetGDT(&tmpGDT[pCPU]);
}


/*
 *-----------------------------------------------------------------------------
 *
 * TaskFreeHVRootPages --
 *
 *      Free all HV root pages (allocated by TaskAllocHVRootPage), if any.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

static void
TaskFreeHVRootPages(void)
{
   MPN mpn;
   unsigned i;

   for (i = 0; i < ARRAYSIZE(hvRootPage); i++) {
      mpn = Atomic_Read64(&hvRootPage[i]);
      if (mpn != INVALID_MPN) {
         HostIF_FreeMachinePage(mpn);
      }
   }
}


#ifdef VMX86_DEBUG
/*
 *-----------------------------------------------------------------------------
 *
 * TaskAssertFail --
 *
 *      Output line number to comport and crash.
 *
 *-----------------------------------------------------------------------------
 */

static void
TaskAssertFail(int line)
{
   CP_PutStr("TaskAssertFail*: ");
   CP_PutDec(line);
   CP_PutCrLf();
   SET_CR3(0);
}


#endif
/*
 *-----------------------------------------------------------------------------
 *
 * TaskSaveGDT64 --
 *
 *      Save the current GDT in the caller-supplied struct.
 *
 * Results:
 *      *hostGDT64 = copy of the processor's GDT.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

static INLINE void
TaskSaveGDT64(DTR64 *hostGDT64)  // OUT
{
   hostGDT64->offset = 0;
   _Get_GDT((DTR *)hostGDT64);
}


/*
 *-----------------------------------------------------------------------------
 *
 * TaskSaveIDT64 --
 *
 *      Save the current IDT in the caller-supplied struct.
 *
 * Results:
 *      *hostIDT64 = copy of the processor's IDT.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

static INLINE void
TaskSaveIDT64(DTR64 *hostIDT64)  // OUT
{
   hostIDT64->offset = 0;
   _Get_IDT((DTR *)hostIDT64);
}


/*
 *-----------------------------------------------------------------------------
 *
 * TaskLoadIDT64 --
 *
 *      Load the current IDT from the caller-supplied struct.
 *
 * Results:
 *      Processor's IDT = *hostIDT64.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

static INLINE void
TaskLoadIDT64(DTR64 *hostIDT64)  // IN
{
   _Set_IDT((DTR *)hostIDT64);
}


/*
 *-----------------------------------------------------------------------------
 *
 * TaskCopyGDT64 --
 *
 *      Copy the given GDT contents to the caller-supplied buffer.
 *
 *      This routine assumes the caller has already verified there is enough
 *      room in the output buffer.
 *
 * Results:
 *      *out = copy of the processor's GDT contents.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

static INLINE void
TaskCopyGDT64(DTR64 *hostGDT64,  // IN  GDT to be copied from
              Descriptor *out)   // OUT where to copy contents to
{
   memcpy(out,
          (void *)HOST_KERNEL_LA_2_VA((LA)hostGDT64->offset),
          hostGDT64->limit + 1);
}


/*
 *-----------------------------------------------------------------------------
 *
 * Task_Terminate --
 *
 *      Called at driver unload time.  Undoes whatever Task_Initialize did.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Release temporary GDT memory.
 *
 *-----------------------------------------------------------------------------
 */

void
Task_Terminate(void)
{
   TaskFreeHVRootPages();

   if (crossGDT != NULL) {
      HostIF_FreeKernelPages(ARRAYSIZE(crossGDTMPNs), crossGDT);
      crossGDT = NULL;
   }

   if (USE_TEMPORARY_GDT) {
      unsigned i;

      for (i = 0; i < ARRAYSIZE(tmpGDT); i++) {
         Descriptor *base = Atomic_ReadPtr(&tmpGDT[i]);

         if (base) {
            HostIF_FreeKernelMem(base);
         }
      }
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * Task_Initialize --
 *
 *      Called at driver load time to initialize module's static data.
 *
 * Results:
 *      TRUE iff initialization successful.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

Bool
Task_Initialize(void)
{
   unsigned i;

   ASSERT_ON_COMPILE(sizeof (Atomic_uint64) == sizeof (MPN));
   for (i = 0; i < ARRAYSIZE(hvRootPage); i++) {
      Atomic_Write64(&hvRootPage[i], INVALID_MPN);
   }
   if (USE_TEMPORARY_GDT) {
      for (i = 0; i < ARRAYSIZE(tmpGDT); i++) {
         Atomic_WritePtr(&tmpGDT[i], NULL);
      }
   }

   /*
    * The worldswitch code doesn't work with a zero stack segment
    * because it temporarily restores the data segments to the stack
    * segment.  So here we make sure we have a non-zero kernel
    * read/write flat data segment.
    */

   kernelStackSegment = GET_SS();
   if (kernelStackSegment == 0) {
      DTR hostGDTR;

      GET_GDT(hostGDTR);
      for (kernelStackSegment = 8;
           kernelStackSegment + 7 <= hostGDTR.limit;
           kernelStackSegment += 8) {
         uint64 gdte = *(uint64 *)(hostGDTR.offset + kernelStackSegment);

         if ((gdte & 0xFFCFFEFFFFFFFFFFULL) == 0x00CF92000000FFFFULL) {
            goto gotnzss;
         }
      }
      Warning("%s: no non-null flat kernel data GDT segment\n",
              __FUNCTION__);

      return FALSE;
gotnzss:;
   }
   if ((kernelStackSegment == 0) || ((kernelStackSegment & 7) != 0)) {
           Warning("Task_Initialize: unsupported SS %04x\n",
                   kernelStackSegment);
         return FALSE;
   }

   /*
    * Check if PEBS is supported.  For simplicity we assume there will not
    * be mixed CPU models.  According to the Intel SDM, PEBS is supported if:
    *
    * IA32_MISC_ENABLE.EMON_AVAILABE (bit 7) is set and
    * IA32_MISC_ENABLE.PEBS_UNAVAILABE (bit 12) is clear.
    */

   pebsAvailable = PerfCtr_PEBSAvailable();
   /*
    * Check if PT is supported.  For simplicity we assume there will not
    * be mixed CPU models.
    */
   ptAvailable = PerfCtr_PTAvailable();
   return TRUE;
}


/*
 *-----------------------------------------------------------------------------
 *
 * TaskRestoreHostGDTTRLDT --
 *
 *
 * Results:
 *      The host's GDT is copied (or partially copied) to the
 *      dynamically allocated temporary GDT.
 *
 *      The TR is restored using the temporary GDT then the host's real GDT is
 *      restored.  Finally, the host LDT is restored.
 *
 * Notes:
 *      An OS which checks critical data structures, such as the GDT,
 *      can fail when this module changes the TSS busy bit in the host
 *      GDT.  To avoid this problem, we use a sparse copy of the host
 *      GDT to perform the manipulation of the TSS busy bit.
 *
 *      See PR 68144.
 *
 *-----------------------------------------------------------------------------
 */

static INLINE_SINGLE_CALLER void
TaskRestoreHostGDTTRLDT(Descriptor *tempGDTBase,
                        DTR64 hostGDT64,
                        Selector ldt,
                        Selector cs,
                        Selector tr)
{
   TS_ASSERT(tr != 0);
   TS_ASSERT((tr & 7) == 0);

   if (USE_TEMPORARY_GDT) {
      DTR64 tempGDT64;

      /*
       * Set up a temporary GDT so that the TSS 'busy bit' can be
       * changed without affecting the host's data structures.
       */

      const VA hostGDTVA  = HOST_KERNEL_LA_2_VA(hostGDT64.offset);
      const unsigned size = sizeof(Descriptor);
      const Selector ss   = SELECTOR_CLEAR_RPL(GET_SS());

      ASSERT(hostGDTVA == HOST_KERNEL_LA_2_VA(hostGDT64.offset));

      ASSERT(SELECTOR_RPL(cs) == 0 && SELECTOR_TABLE(cs) == 0);
      ASSERT(SELECTOR_RPL(ss) == 0 && SELECTOR_TABLE(ss) == 0);

      /*
       * Copy code and data segments so they remain valid in case of NMI.
       * Worldswitch code returns with DS==ES==SS so we don't have to set
       * up DS,ES explicitly.
       */

      ASSERT(SELECTOR_CLEAR_RPL(GET_DS()) == ss);
      ASSERT(SELECTOR_CLEAR_RPL(GET_ES()) == ss);
      tempGDTBase[cs / size]     = *(Descriptor *)(hostGDTVA + cs);
      tempGDTBase[ss / size]     = *(Descriptor *)(hostGDTVA + ss);

      /*
       * TR descriptors use two entries (64-bits wide) in 64-bit mode.
       */

      tempGDTBase[tr / size]     = *(Descriptor *)(hostGDTVA + tr);
      tempGDTBase[tr / size + 1] = *(Descriptor *)(hostGDTVA + tr + size);

      /*
       * Clear the 'task busy' bit so we can reload TR.
       */

      if (Desc_Type(&tempGDTBase[tr / size]) == TASK_DESC_BUSY) {
         Desc_SetType(&tempGDTBase[tr / size], TASK_DESC);
      }

      /*
       * Restore the TR using the temp GDT then restore the host's real GDT
       * then host LDT.
       */

      tempGDT64.limit  = hostGDT64.limit;
      tempGDT64.offset = HOST_KERNEL_VA_2_LA((VA)tempGDTBase);
      _Set_GDT((DTR *)&tempGDT64);
      SET_TR(tr);
      _Set_GDT((DTR *)&hostGDT64);
      SET_LDT(ldt);
   } else {
      Descriptor *desc;

      /*
       * The host isn't picky about the TR entry.  So clear the TSS<busy> bit
       * in the host GDT, then restore host GDT and TR, then LDT.
       */

      desc = (Descriptor *)((VA)HOST_KERNEL_LA_2_VA(hostGDT64.offset + tr));
#ifdef LINUX_GDT_IS_RO
      /*
       * If GDT is read-only, we must always load TR from alternative gdt,
       * otherwise CPU gets page fault when marking TR busy.
       */
      {
         DTR64 rwGDT64;

         rwGDT64.offset = (unsigned long)tempGDTBase;
         rwGDT64.limit = hostGDT64.limit;
         Desc_SetType((Descriptor *)((unsigned long)tempGDTBase + tr), TASK_DESC);
         _Set_GDT((DTR *)&rwGDT64);
         SET_TR(tr);
         _Set_GDT((DTR *)&hostGDT64);
      }
#else
      if (Desc_Type(desc) == TASK_DESC_BUSY) {
         Desc_SetType(desc, TASK_DESC);
      }
      _Set_GDT((DTR *)&hostGDT64);
      SET_TR(tr);
#endif
      SET_LDT(ldt);
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * TaskSwitchPTPAllocPage --
 *
 *      Allocates and zeroes a page to be used as part of a page table patch.
 *      Adds a corresponding entry to the PTP memtracker.
 *
 * Results:
 *      On success, a pointer to the page's starting address. NULL on
 *      failure.
 *
 *-----------------------------------------------------------------------------
 */

static void *
TaskSwitchPTPAllocPage(VMDriver *vm)
{
   MPN mpn;
   MemTrackEntry *entry;
   uint8 *ptr;

   ptr = HostIF_AllocKernelPages(1, &mpn);
   if (ptr == NULL) {
      return NULL;
   }
   memset(ptr, 0, PAGE_SIZE);
   entry = MemTrack_Add(vm->ptpTracker, VA_2_VPN(PtrToVA64(ptr)), mpn);

   if (entry == NULL) {
      HostIF_FreeKernelPages(1, ptr);
      return NULL;
   }

   return ptr;
}


/*
 *-----------------------------------------------------------------------------
 *
 * TaskSwitchPTPLookupVPN --
 *
 *      Looks up the MPN for the given PTP page VPN by querying the PTP
 *      memtracker.
 *
 * Results:
 *      The VPN's corresponding MPN.
 *
 *-----------------------------------------------------------------------------
 */

static MPN
TaskSwitchPTPLookupVPN(VMDriver *vm, VPN vpn)
{
   MemTrackEntry *entry = MemTrack_LookupVPN(vm->ptpTracker, vpn);

   ASSERT(entry != NULL && entry->mpn != 0);
   return entry->mpn;
}


/*
 *-----------------------------------------------------------------------------
 *
 * TaskSwitchPTPPageFree --
 *
 *      Frees the PTP page matching the VPN stored by the given
 *      MemTrackEntry. This function is used as a callback by
 *      MemTrack_Cleanup().
 *
 * Results:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

static void
TaskSwitchPTPPageFree(void *unused, MemTrackEntry *entry)
{
   ASSERT(entry->mpn != 0);
   HostIF_FreeKernelPages(1, VA64ToPtr(VPN_2_VA(entry->vpn)));
}


/*
 *-----------------------------------------------------------------------------
 *
 * Task_SwitchPTPPageCleanup --
 *
 *      Frees all PTP pages allocated for the given VM and frees the
 *      PTP MemTracker.
 *
 * Results:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

void
Task_SwitchPTPPageCleanup(VMDriver *vm)
{
   MemTrack_Cleanup(vm->ptpTracker, TaskSwitchPTPPageFree, NULL);
   vm->ptpTracker = NULL;
}


/*
 *-----------------------------------------------------------------------------
 *
 * TaskSetCrossGDTEntry --
 *
 *      Fill in a crossGDT entry from the given descriptor. If the entry has
 *      already been initialized, it is compared to the given descriptor. Any
 *      discrepancy is logged and an error is returned.
 *
 * Results:
 *      TRUE on success, FALSE otherwise.
 *
 * Side effects:
 *      crossGDT entry is initialized, if not already.
 *
 *-----------------------------------------------------------------------------
 */

static Bool
TaskSetCrossGDTEntry(uint16 index, Descriptor d)
{
   Bool res = FALSE;

   ASSERT(HostIF_GlobalLockIsHeld());
   ASSERT(Desc_Present(&d));

   if (index >= ARRAYSIZE(crossGDT->gdtes)) {
      Warning("%s: index %u too big\n", __FUNCTION__, index);
   } else if (!CROSSGDT_TESTINDEXMASK(index)) {
      Warning("%s: index %u not in CROSSGDT_PAGEMASK %x\n", __FUNCTION__,
              index, CROSSGDT_PAGEMASK);
   } else if (!Desc_Present(crossGDT->gdtes + index)) {
      crossGDT->gdtes[index] = d;
      res = TRUE;
   } else if (Desc_EqualIgnoreAccessed(crossGDT->gdtes + index, &d)) {
      res = TRUE;
   } else {
      Warning("%s: entry 0x%X mismatch\n", __FUNCTION__, index);
      Warning("%s:   crossGDT %16.16" FMT64 "X\n", __FUNCTION__,
              *(uint64 *)(crossGDT->gdtes + index));
      Warning("%s:   template %16.16" FMT64 "X\n", __FUNCTION__, *(uint64 *)&d);
   }

   return res;
}


/*
 *-----------------------------------------------------------------------------
 *
 * TaskSetCrossGDTVMM --
 *
 *      Initialize/compare the VMM portion of the crossGDT.
 *
 * Results:
 *      TRUE if success, FALSE otherwise.
 *
 * Side effects:
 *      VMM portion of the crossGDT is initialized, if not already.
 *
 *-----------------------------------------------------------------------------
 */

static Bool
TaskSetCrossGDTVMM(BSVMM_GDTInit *gdt)
{
   uint8 i;

   for (i = 0; i < ARRAYSIZE(gdt->entries); i++) {
      Descriptor d;
      if (gdt->entries[i].present == 1) {
         Desc_SetDescriptor(&d, gdt->entries[i].base,
                                gdt->entries[i].limit,
                                gdt->entries[i].type,
                                gdt->entries[i].S,
                                gdt->entries[i].DPL,
                                gdt->entries[i].present,
                                gdt->entries[i].DB,
                                gdt->entries[i].gran);
         d.longmode = gdt->entries[i].longmode;

         if (!TaskSetCrossGDTEntry(gdt->entries[i].index, d)) {
            return FALSE;
         }
      }
   }
   return TRUE;
}


/*
 *-----------------------------------------------------------------------------
 *
 * TaskSetCrossGDTHost --
 *
 *      Initializes the host portion of the crossGDT by copying it directly from
 *      the host kernel's GDT. We assume that all the host segments we
 *      will ever need are below CROSSGDT_HOSTLIMIT.
 *
 * Results:
 *      TRUE on success, FALSE otherwise.
 *
 * Side effects:
 *      The host portion of the crossGDT is initialized.
 *
 *-----------------------------------------------------------------------------
 */

static void
TaskSetCrossGDTHost(void)
{
   DTR64 hostGDT;
   /*
    * All copied host segment descriptors will come from the first page of
    * the host kernel GDT.
    */
   ASSERT(HostIF_GlobalLockIsHeld());
   ASSERT_ON_COMPILE(CROSSGDT_HOSTLIMIT <= PAGE_SIZE / sizeof(Descriptor));
   ASSERT_ON_COMPILE(CROSSGDT_PAGEMASK & 1);

   TaskSaveGDT64(&hostGDT);
   if (hostGDT.limit > CROSSGDT_HOSTLIMIT * sizeof(Descriptor) - 1) {
      hostGDT.limit = CROSSGDT_HOSTLIMIT * sizeof(Descriptor) - 1;
   }
   TaskCopyGDT64(&hostGDT, crossGDT->gdtes);
}


/*
 *-----------------------------------------------------------------------------
 *
 * Task_CreateCrossGDT --
 *
 *      Allocate and initialize the crossGDT, if not already done. A single
 *      crossGDT is shared by all VMs managed by this driver.
 *
 * Results:
 *      TRUE if success, FALSE otherwise.
 *
 * Side effects:
 *      crossGDT static vars set up if not already.
 *
 *-----------------------------------------------------------------------------
 */

Bool
Task_CreateCrossGDT(BSVMM_GDTInit *gdt)
{
   HostIF_GlobalLock(2);

   if (crossGDT == NULL) {
      /*
       * The crossGDT has not yet been created. This must be the first
       * VM that this driver powers on.
       */
      ASSERT_ON_COMPILE(sizeof *crossGDT == CROSSGDT_NUMPAGES * PAGE_SIZE);
      crossGDT = HostIF_AllocKernelPages(ARRAYSIZE(crossGDTMPNs),
                                         crossGDTMPNs);
      if (crossGDT == NULL) {
         HostIF_GlobalUnlock(2);
         Warning("%s: unable to allocate crossGDT\n", __FUNCTION__);

         return FALSE;
      }
      memset(crossGDT, 0, sizeof *crossGDT);
      TaskSetCrossGDTHost();
   }
   if (!TaskSetCrossGDTVMM(gdt)) {
      HostIF_GlobalUnlock(2);
      return FALSE;
   }

   HostIF_GlobalUnlock(2);

   return TRUE;
}


/*
 *-----------------------------------------------------------------------------
 *
 * TaskFixupHostSwitchIDTE --
 *
 *      Fixes up a 64-bit host IDT entry in the crosspage, given the
 *      host kernel CS and the crosspage's code section address on the
 *      host.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

static void
TaskFixupHostSwitchIDTE(Gate64 *entry, Selector cs, VMCrossPageCode *cpCode)
{
   VA64 handlerVA;
   DEBUG_ONLY(const VA64 codeBase = offsetof(VMCrossPageCode, codeBlock);)

   ASSERT_ON_COMPILE(sizeof *entry == 16);

   ASSERT(entry->segment      == 0 && entry->ist          == 0 &&
          entry->reserved0    == 0 && entry->type         == INTER_GATE &&
          entry->DPL          == 0 && entry->present      == 0 &&
          entry->offset_16_31 == 0 && entry->offset_32_63 == 0 &&
          entry->reserved1    == 0);

   /*
    * entry->offset_0_15 is initialized in crosspage.S with an offset relative
    * to the crosspage's code section. Here, we transform it into an absolute
    * address.
    */
   ASSERT(entry->offset_0_15 >= codeBase &&
          entry->offset_0_15 < codeBase + sizeof cpCode->codeBlock);

   handlerVA = PtrToVA64(cpCode) + entry->offset_0_15;

   entry->offset_0_15  = LOWORD(handlerVA);
   entry->segment      = cs;
   entry->present      = 1;
   entry->offset_16_31 = HIWORD(handlerVA);
   entry->offset_32_63 = HIDWORD(handlerVA);
}


/*
 *-----------------------------------------------------------------------------
 *
 *  TaskFixupHostSwitchIDT --
 *
 *      Fixes up the contents of the host-context switch IDT and IDTR,
 *      which were partially initialized at compile-time. At this point,
 *      we have the necessary runtime information needed to fully
 *      initialize them: the crosspage's address in the host and the
 *      host kernel's CS.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

static void
TaskFixupHostSwitchIDT(VMCrossPage *crosspage)
{
   VMCrossPageData *cpData = &crosspage->crosspageData;
   VMCrossPageCode *cpCode = &crosspage->crosspageCode;
   const Selector   cs     = cpData->hostInitial64CS;
   Gate64          *idt    = (Gate64 *)&cpData->switchHostIDT;

   ASSERT(cpData->switchHostIDTR.limit == sizeof cpData->switchHostIDT - 1);
   ASSERT(cpData->switchHostIDTR.offset == 0);
   cpData->switchHostIDTR.offset = PtrToVA64(&cpData->switchHostIDT);

   TaskFixupHostSwitchIDTE(&idt[EXC_DB ], cs, cpCode);
   TaskFixupHostSwitchIDTE(&idt[EXC_NMI], cs, cpCode);
   TaskFixupHostSwitchIDTE(&idt[EXC_UD ], cs, cpCode);
   TaskFixupHostSwitchIDTE(&idt[EXC_MC ], cs, cpCode);
}


/*
 *-----------------------------------------------------------------------------
 *
 * TaskFixupPatchPT --
 * TaskFixupPatchPTE --
 *
 *      These functions recursively walk the specified page table patch
 *      subtree and convert all entries containing a 'VA | flags' into
 *      their actual PTE values at that level.
 *
 *      When 'PTE_P' is not present in an entry, the value is actually
 *      a VA.  All other flag values, besides 'PTE_P' may be set in
 *      the entry; the flag values are preserved during conversion to
 *      a PTE (and 'PTE_P' is included).
 *
 *      Level 1 entries are always entered as true PTE values.  Since
 *      they always have PTE_P set, they never need fixing up.
 *
 * Results:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

static void
TaskFixupPatchPT(VMDriver *vm,
                 VM_PDPTE *ptPage,
                 unsigned  ptLevel);

static void
TaskFixupPatchPTE(VMDriver *vm,
                  VM_PDPTE *pte,
                  unsigned  ptLevel)
{
   const VM_PDPTE entry   = *pte;
   const VM_PDPTE paEntry = VM_PAGE_BASE(entry);
   const uint32   flags   = entry & (PAGE_SIZE - 1);

   ASSERT_ON_COMPILE(sizeof(VM_PDPTE) == sizeof(uint64));
   ASSERT_ON_COMPILE(sizeof(PT_L4E)   == sizeof(VM_PDPTE));
   ASSERT_ON_COMPILE(sizeof(PT_L3E)   == sizeof(VM_PDPTE));
   ASSERT_ON_COMPILE(sizeof(PT_L2E)   == sizeof(VM_PDPTE));
   ASSERT_ON_COMPILE(sizeof(PT_L1E)   == sizeof(VM_PDPTE));
   ASSERT(PTP_LEVEL_L1 <= ptLevel && ptLevel <= PTP_LEVEL_L4);

   if (ptLevel == 1) {
      ASSERT(PTE_PRESENT(flags)); /* Already in PTE form. */
   } else if (paEntry != 0) {
      MPN mpn;
      VM_PDPTE *nextPt = (VM_PDPTE *)(uintptr_t)paEntry;

      ASSERT(!PTE_PRESENT(flags)); /* Not in PTE form. */
      mpn = TaskSwitchPTPLookupVPN(vm, VA_2_VPN(PtrToVA64(nextPt)));

      *pte = LM_MAKE_PTE(mpn, 0, flags | PTE_P);

      TaskFixupPatchPT(vm, nextPt, ptLevel - 1);
   }
}

static void
TaskFixupPatchPT(VMDriver *vm,
                 VM_PDPTE *ptPage,
                 unsigned  ptLevel)
{
   unsigned i;

   ASSERT(PTP_LEVEL_L1 <= ptLevel && ptLevel <= PTP_LEVEL_L4);

   for (i = 0; i < PAGE_SIZE / sizeof(VM_PDPTE); i++) {
      VM_PDPTE *pte = &ptPage[i];
      if (*pte != 0) {
         TaskFixupPatchPTE(vm, pte, ptLevel);
      }
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * TaskFixupPTPatches --
 *
 *    Fixes up the PTEs for the entire page table hierarchy rooted at
 *    each PTP. It translates PTEs from the 'VA | flags' intermediate
 *    format to the actual PTE format.
 *
 * Results:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

static void
TaskFixupPTPatches(VMDriver *vm, VMCrossPageData *cpData)
{
   unsigned i;

   for (i = 0; i < MAX_SWITCH_PT_PATCHES; i++) {
      VMMPageTablePatch *patch = &cpData->vmmPTP[i];
      if (patch->level == PTP_EMPTY) {
         break;
      }
      TaskFixupPatchPTE(vm, &patch->pte, patch->level);
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * TaskSavePTPatch --
 *
 *      This function saves a page table patch entry.  The patch
 *      entries are used during worldswitch to ensure that the cross
 *      page and portions of the host GDT are mapped at all times (for
 *      NMI-safe worldswitches).
 *
 *      The code searches for an empty slot rather than using a static
 *      local index (incremented after use) because it is run once on
 *      each VCPU.  Each VCPU has a distinct 'crosspage', and thus a
 *      distinct patch table.
 *
 * Results:
 *      Saved patch entry.
 *
 *-----------------------------------------------------------------------------
 */

static VMMPageTablePatch *
TaskSavePTPatch(VMCrossPageData *crosspage,
                LPN64            lpn,
                unsigned         level,
                VM_PDPTE         pte)
{
   unsigned i;
   const unsigned pteIdx     = PTE_INDEX(lpn, level);
   const uint64 pteGlobalIdx = PTE_GLOBAL_INDEX(lpn, level);

   ASSERT(PTP_LEVEL_L1 <= level && level <= PTP_LEVEL_L4);

   for (i = 0; i < MAX_SWITCH_PT_PATCHES; ++i) {
      ASSERT(crosspage->vmmPTP[i].level != level || /* No duplicates. */
             crosspage->vmmPTP[i].pteGlobalIdx != pteGlobalIdx ||
             crosspage->vmmPTP[i].pteIdx != pteIdx);

      if (crosspage->vmmPTP[i].level == PTP_EMPTY) {
         crosspage->vmmPTP[i].level = level;
         crosspage->vmmPTP[i].pteIdx = pteIdx;
         crosspage->vmmPTP[i].pte   = pte;
         crosspage->vmmPTP[i].pteGlobalIdx = pteGlobalIdx;

         return &crosspage->vmmPTP[i];
      }
   }
   Panic("Internal error: PTP table is full");
}


/*
 *-----------------------------------------------------------------------------
 *
 * TaskSearchPTPatch --
 *
 *    Looks for an entry in the PT patch table.
 *
 * Results:
 *      Pointer to the PTP entry, or NULL if not found.
 *
 *-----------------------------------------------------------------------------
 */

static VMMPageTablePatch *
TaskSearchPTPatch(VMCrossPageData *cpData,
                  LPN64            lpn,
                  unsigned         level)
{
   unsigned i;

   ASSERT(PTP_LEVEL_L1 <= level && level <= PTP_LEVEL_L4);

   for (i = 0; i < MAX_SWITCH_PT_PATCHES; i++) {
      VMMPageTablePatch *patch = &cpData->vmmPTP[i];
      if (patch->level == level &&
          patch->pteGlobalIdx == PTE_GLOBAL_INDEX(lpn, level)) {

         return patch;
      }
   }
   return NULL;
}

/*
 *-----------------------------------------------------------------------------
 *
 * TaskCreatePTPatch --
 *
 *      Creates a page table patch for BackToHost to apply to the
 *      monitor's page table before switching to the host. These patches
 *      map the crosspage and crossGDT at their host addresses so there
 *      is common ground during the switch.
 *
 *      A patch is essentially a PTE-rooted page table subtree. The
 *      location of the root PTE is determined by calculating the lowest
 *      page table that is shared with the monitor in the translation
 *      path for the LPN associated with the patch. We disallow patches
 *      inside the monitor address space.
 *
 *      At creation time, the patch's PTEs are initialized to a 'VA | flags'
 *      intermediary representation, where VA is the virtual address of
 *      a page, and 'flags' are the PTE flags. This allows easy
 *      traversal of the structure during patch creation. PTEs are fixed
 *      up later on after all patches have been created.
 *
 * Results:
 *      TRUE if the patch was created successfully, FALSE otherwise.
 *
 * Side effects:
 *      Patch is created and saved to the crosspage's PTP table.
 *
 *-----------------------------------------------------------------------------
 */

static Bool
TaskCreatePTPatch(VMDriver        *vm,
                  VMCrossPageData *cpData,
                  LPN64            monStartLPN,
                  LPN64            monEndLPN,
                  LPN64            lpn,
                  MPN              mpn,
                  uint16          *numPages)
{
   const PT_L1E       l1pte    = LM_MAKE_PTE(mpn, 0, L1_FLAGS);
   const int          flags[4] = {L1_FLAGS, L2_FLAGS, L3_FLAGS, L4_FLAGS};
   VM_PDPTE          *pte;
   unsigned           level;
   VMMPageTablePatch *patch;

   ASSERT_ON_COMPILE(sizeof(PT_L4E) == sizeof(VM_PDPTE));
   ASSERT_ON_COMPILE(sizeof(PT_L3E) == sizeof(VM_PDPTE));
   ASSERT_ON_COMPILE(sizeof(PT_L2E) == sizeof(VM_PDPTE));
   ASSERT_ON_COMPILE(sizeof(PT_L1E) == sizeof(VM_PDPTE));

   /*
    * Find the level of the first non-VMM PTE in the LPN's translation
    * path. This will be the level in which the patch's root PTE will be
    * placed.
    */
   for (level = 4; level > 0; level--) {
      uint64 lpnIdx = PTE_GLOBAL_INDEX(lpn, level);
      if (lpnIdx < PTE_GLOBAL_INDEX(monStartLPN, level) ||
          lpnIdx > PTE_GLOBAL_INDEX(monEndLPN, level)) {
         break;
      }
   }

   /*
    * Ensure that we actually found something. If not (i.e. the l4e, l3e,
    * l2e and l1e are all VMM entries), the LPN overlaps with the VMM's
    * address space, so we return an error.
    */
   if (level == 0) {
      Warning("%s: cannot create page table patch because LPN 0x%" FMT64 "x "
              "overlaps with the VMM address space.\n", __FUNCTION__, lpn);
      return FALSE;
   }

   patch = TaskSearchPTPatch(cpData, lpn, level);
   if (patch == NULL) {
      patch = TaskSavePTPatch(cpData, lpn, level, 0); /* PTE filled later */
   }

   /*
    * Traverse the patch-rooted page table subtree until we reach L1.
    * Allocate new page tables as needed.
    */
   pte = &patch->pte;
   while (level > 1) {
      VM_PDPTE *page;
      ASSERT(!PTE_PRESENT(*pte & (PAGE_SIZE - 1))); /* Pre-fixup entry. */
      page = (VM_PDPTE *)VM_PAGE_BASE((uintptr_t)*pte);
      if (page == NULL) {
         page = TaskSwitchPTPAllocPage(vm);
         if (page == NULL) {
            Warning("%s: failure allocating memory for page table patch.\n",
                    __FUNCTION__);
            return FALSE;
         }
         *numPages += 1;
         *pte = (VM_PDPTE)((uintptr_t)page | flags[level - 1]); /* VA | flags */
      }
      level -= 1;
      pte = (VM_PDPTE *)&page[PTE_INDEX(lpn, level)];
   }

   ASSERT(level == 1 && *pte == 0);
   ASSERT_ON_COMPILE(PTE_PRESENT(L1_FLAGS));
   *pte = l1pte;

   return TRUE;
}


/*
 *-----------------------------------------------------------------------------
 *
 * TaskCreatePTPatches
 *
 *      Creates VMM mappings for the crosspage and crossGDT that match
 *      those from the vmmon address space, so that they are mapped at
 *      a common linear address on both address spaces. Such mappings
 *      are created in the form of patches that are applied to/removed
 *      from the VMM's page tables.
 *
 * Results:
 *      TRUE if all patches were created successfully, FALSE otherwise.
 *
 *-----------------------------------------------------------------------------
 */

static Bool
TaskCreatePTPatches(VMDriver    *vm,
                    VMCrossPage *crosspage,
                    LPN64        monStartLPN,
                    LPN64        monEndLPN,
                    uint16      *numPages)
{
   VMCrossPageData * const cpData = &crosspage->crosspageData;
   LPN64                   xgCPN  = LA_2_LPN(cpData->vmm64CrossGDTLA);
   LPN64                   xpCPN  = LA_2_LPN(cpData->vmm64CrossPageLA);
   MPN                     xpMPN  = MA_2_MPN(cpData->crosspageMA);
   int                     i;

   /*
    * Set up patches that the BackToHost code will use to map the
    * crosspage and crossGDT at their crossover addresses (HKLA for
    * the host).
    */
   if (!TaskCreatePTPatch(vm, cpData, monStartLPN, monEndLPN, xpCPN, xpMPN,
                          numPages)) {
      return FALSE;
   }

   for (i = 0; CROSSGDT_PAGEMASK >> i; i++) {
      if ((CROSSGDT_PAGEMASK >> i) & 1) {
         /*
          * Set up patches for just first and last pages of cross-gdt
          * due to the selected value of CROSSGDT_PAGEMASK.
          */
         if (!TaskCreatePTPatch(vm, cpData, monStartLPN, monEndLPN, xgCPN + i,
                                crossGDTMPNs[i], numPages)) {
            return FALSE;
         }
      }
   }
   TaskFixupPTPatches(vm, cpData);

   return TRUE;
}


/*
 *-----------------------------------------------------------------------------
 *
 * Task_InitCrosspage  --
 *
 *    Initialize the crosspage used to switch to the monitor task.
 *
 * Results:
 *    0 on success
 *    != 0 on failure
 *
 * Side effects:
 *    None
 *
 *-----------------------------------------------------------------------------
 */

int
Task_InitCrosspage(VMDriver *vm,          // IN
                   InitBlock *initParams) // IN/OUT: Initial params from the
                                          //         VM
{
   Vcpuid vcpuid;
   uint16 numPTPPages = 0;

   if (crossGDT == NULL) {
      return 1;
   }

   initParams->crossGDTHKLA = HOST_KERNEL_VA_2_LA((VA)crossGDT);
   ASSERT_ON_COMPILE(sizeof initParams->crossGDTMPNs == sizeof crossGDTMPNs);
   memcpy(initParams->crossGDTMPNs, crossGDTMPNs, sizeof crossGDTMPNs);

   for (vcpuid = 0; vcpuid < initParams->numVCPUs;  vcpuid++) {
      VA64         crossPageUserAddr = initParams->crosspage[vcpuid];
      VMCrossPage *p                 = HostIF_MapCrossPage(vm, crossPageUserAddr);
      MPN          crossPageMPN;

      if (p == NULL) {
         return 1;
      }

      if (HostIF_LookupUserMPN(vm, crossPageUserAddr, &crossPageMPN) !=
          PAGE_LOOKUP_SUCCESS ||
          crossPageMPN == 0) {
         return 1;
      }

      {
         /* The version of the crosspage must be the first four
          * bytes of the crosspage.  See the declaration
          * of VMCrossPage in modulecall.h.
          */

         ASSERT_ON_COMPILE(offsetof(VMCrossPage, version) == 0);
         ASSERT_ON_COMPILE(sizeof(p->version) == sizeof(uint32));

         /* p->version is VMX's version; CROSSPAGE_VERSION is vmmon's. */
         if (p->version != CROSSPAGE_VERSION) {
            Warning("%s: crosspage version mismatch: vmmon claims %#x, must "
                    "match vmx version of %#x.\n", __FUNCTION__,
                    (int)CROSSPAGE_VERSION, p->version);
            return 1;
         }
      }
      {
         /* The following constants are the size and offset of the
          * VMCrossPage->crosspage_size field as defined by the
          * vmm/vmx.
          */

         ASSERT_ON_COMPILE(offsetof(VMCrossPage, crosspage_size) ==
                           sizeof(uint32));
         ASSERT_ON_COMPILE(sizeof(p->crosspage_size) == sizeof(uint32));

         if (p->crosspage_size != sizeof(VMCrossPage)) {
            Warning("%s: crosspage size mismatch: vmmon claims %#x bytes, "
                    "must match vmm size of %#x bytes.\n", __FUNCTION__,
                    (unsigned)sizeof(VMCrossPage), p->crosspage_size);
            return 1;
         }
      }

      if (crossPageMPN > MA_2_MPN(0xFFFFFFFF)) {
         Warning("%s*: crossPageMPN 0x%016" FMT64 "x invalid\n", __FUNCTION__,
                 crossPageMPN);
         return 1;
      }
      if (!pseudoTSC.initialized) {
         Warning("%s*: PseudoTSC has not been initialized\n", __FUNCTION__);
         return 1;
      }
      p->crosspageData.crosspageMA = (uint32)MPN_2_MA(crossPageMPN);
      p->crosspageData.hostCrossPageLA = (LA64)(uintptr_t)p;
      p->crosspageData.vmm64CrossPageLA = p->crosspageData.hostCrossPageLA;
      p->crosspageData.vmm64CrossGDTLA = HOST_KERNEL_VA_2_LA((VA)crossGDT);

      HostIF_VMLock(vm, 39);
      if (!TaskCreatePTPatches(vm, p,
                               initParams->monStartLPN,
                               initParams->monEndLPN,
                               &numPTPPages)) {
         HostIF_VMUnlock(vm, 39);
         Warning("%s: Could not create page table patches for VCPU %d\n",
                 __FUNCTION__, vcpuid);
         return 1;
      }
      HostIF_VMUnlock(vm, 39);
      /*
       * Pass our kernel code segment numbers back to MonitorPlatformInit.
       * They have to be in the GDT so they will be valid when the crossGDT is
       * active.
       */

      p->crosspageData.hostInitial64CS = GET_CS();
      TS_ASSERT(SELECTOR_RPL  (p->crosspageData.hostInitial64CS) == 0 &&
                SELECTOR_TABLE(p->crosspageData.hostInitial64CS) == 0);

      p->crosspageData.moduleCallInterrupted = FALSE;
      VersionedAtomic_BeginWrite(&p->crosspageData.pseudoTSCConv.vers);
      p->crosspageData.pseudoTSCConv.p.mult  = 1;
      p->crosspageData.pseudoTSCConv.p.shift = 0;
      p->crosspageData.pseudoTSCConv.p.add   = 0;
      VersionedAtomic_EndWrite(&p->crosspageData.pseudoTSCConv.vers);
      p->crosspageData.worldSwitchPTSC       = Vmx86_GetPseudoTSC();
      p->crosspageData.timerIntrTS           = MAX_ABSOLUTE_TS;
      p->crosspageData.hstTimerExpiry        = MAX_ABSOLUTE_TS;
      p->crosspageData.monTimerExpiry        = MAX_ABSOLUTE_TS;
      vm->crosspage[vcpuid]                  = p;

      TaskFixupHostSwitchIDT(p);
   }
   /*
    * Report back to the VMX the number of pages allocated for this VM's
    * page table patches.
    */
   initParams->numPTPPages = numPTPPages;

   return 0;
}


/*
 *-----------------------------------------------------------------------------
 *
 *      Disable and restore APIC NMI delivery.
 *
 *-----------------------------------------------------------------------------
 */

static Bool
TaskDisableNMIDelivery(const APICDescriptor *desc, // IN
                       int regNum)                 // IN
{
   uint32 reg = APIC_Read(desc, regNum);

   if (APIC_LVT_DELVMODE(reg) == APIC_LVT_DELVMODE_NMI &&
       !APIC_LVT_ISMASKED(reg)) {
      APIC_Write(desc, regNum, reg | APIC_LVT_MASK);
      // Force completion of masking (was bug 78470).
      dummyLVT = APIC_Read(desc, regNum);
      return TRUE;
   }

   return FALSE;
}


static void
TaskDisableNMI(const APICDescriptor *desc, // IN
               Bool *lint0NMI,             // OUT
               Bool *lint1NMI,             // OUT
               Bool *pcNMI,                // OUT
               Bool *thermalNMI)           // OUT
{
   if (desc->base || desc->isX2) {
      *lint0NMI = TaskDisableNMIDelivery(desc, APICR_LVT0);
      *lint1NMI = TaskDisableNMIDelivery(desc, APICR_LVT1);
      *pcNMI = TaskDisableNMIDelivery(desc, APICR_PCLVT);

      /*
       * The LVT thermal monitor register was introduced
       * in Pentium 4 and Xeon processors.
       */

      if (APIC_MaxLVT(desc) >= 5) {
         *thermalNMI = TaskDisableNMIDelivery(desc, APICR_THERMLVT);
      } else {
         *thermalNMI = FALSE;
      }
   } else {
      *lint0NMI = FALSE;
      *lint1NMI = FALSE;
      *pcNMI = FALSE;
      *thermalNMI = FALSE;
   }
}


static void
TaskRestoreNMIDelivery(const APICDescriptor *desc, // IN
                       Bool restore,               // IN
                       int regNum)                 // IN
{
   if (restore) {
      uint32 reg = APIC_Read(desc, regNum);

      APIC_Write(desc, regNum, reg & ~APIC_LVT_MASK);
   }
}


static void
TaskRestoreNMI(const APICDescriptor *desc, // IN
               Bool lint0NMI,              // IN
               Bool lint1NMI,              // IN
               Bool pcNMI,                 // IN
               Bool thermalNMI)            // IN
{
   TaskRestoreNMIDelivery(desc, lint0NMI, APICR_LVT0);
   TaskRestoreNMIDelivery(desc, lint1NMI, APICR_LVT1);
   TaskRestoreNMIDelivery(desc, pcNMI, APICR_PCLVT);
   TaskRestoreNMIDelivery(desc, thermalNMI, APICR_THERMLVT);
}


/*
 *-----------------------------------------------------------------------------
 *
 * TaskEnableTF --
 *
 *     Turn on EFLAGS<TF>.
 *
 * Results:
 *     None.
 *
 * Side effects:
 *     Trace trapping enabled.
 *
 *-----------------------------------------------------------------------------
 */

static INLINE void
TaskEnableTF(void)
{
#if defined(__GNUC__)
   asm volatile ("pushfq ; orb $1,1(%rsp) ; popfq");
#elif defined(_MSC_VER)
   __writeeflags(__readeflags() | EFLAGS_TF);
#else
#error no compiler support for setting TF
#endif
}


/*
 *-----------------------------------------------------------------------------
 *
 * TaskDisableTF --
 *
 *     Turn off EFLAGS<TF>.
 *
 * Results:
 *     None.
 *
 * Side effects:
 *     Trace trapping disabled.
 *
 *-----------------------------------------------------------------------------
 */

static INLINE void
TaskDisableTF(void)
{
#if defined(__GNUC__)
   asm volatile ("pushfq ; andb $~1,1(%rsp) ; popfq");
#elif defined(_MSC_VER)
   __writeeflags(__readeflags() & ~EFLAGS_TF);
#else
#error no compiler support for clearing TF
#endif
}


static INLINE Bool
TaskGotException(const VMCrossPage *crosspage, unsigned exc)
{
   return crosspage->crosspageData.wsException[exc];
}


static INLINE void
TaskSetException(VMCrossPage *crosspage, unsigned exc, Bool v)
{
   crosspage->crosspageData.wsException[exc] = v;
}


/*
 *-----------------------------------------------------------------------------
 *
 * TaskSaveDebugRegisters --
 *
 *      Save debug registers in the host context area of the crosspage.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      crosspage->hostDR[*] = some filled with debug register contents
 *               hostDRSaved = bits set for those we wrote to hostDR[*] array
 *                hostDRInHW = bits set indicating which hardware DR contents
 *                             still match what the host wants
 *      hardware DR7<GD> = 0
 *      hardware DR7<bp enables> = 0
 *
 *-----------------------------------------------------------------------------
 */

static INLINE_SINGLE_CALLER void
TaskSaveDebugRegisters(VMCrossPage *crosspage)
{
   Bool saveGotDB;

#define SAVE_DR(n)                                      \
   do {                                                 \
      uintptr_t drReg;                                  \
      GET_DR##n(drReg);                                 \
      crosspage->crosspageData.hostDR[n] = drReg;       \
   } while (0)

   /* Hardware contains the host's %dr7, %dr6, %dr3, %dr2, %dr1, %dr0 */
   crosspage->crosspageData.hostDRInHW = ((1 << 7) | (1 << 6) |
                                          (1 << 3) | (1 << 2) |
                                          (1 << 1) | (1 << 0));

   /*
    * Save DR7 since we need to disable debug breakpoints during the world
    * switch code.  We will get a #DB if DR7<GD> is set, but the
    * SwitchDBHandler simply IRETs after setting crosspage gotDB flag.
    */

   saveGotDB = TaskGotException(crosspage, EXC_DB);
   TaskSetException(crosspage, EXC_DB, FALSE);
   COMPILER_MEM_BARRIER();      /* Prevent hoisting #UD-raising instructions. */
   SAVE_DR(7);

   /*
    * In all cases, DR7 shouldn't have the GD bit set.
    */

   TS_ASSERT(!(crosspage->crosspageData.hostDR[7] & DR7_GD));

   /*
    * Save DR6 in order to accommodate the ICEBP instruction and other stuff
    * that can modify DR6 bits (trace traps, task switch traps, any others?).
    */

   SAVE_DR(6);

   /*
    * It may be that DR7 had the GD bit set, in which case the
    * crosspage exception[EXC_DB] flag would have just been set and
    * DR6<BD> will be set.  If so, fix the saved values to look like
    * they were when DR7<GD> was set (before we tripped the #DB), so
    * they'll get restored to what they were.  Then make sure
    * breakpoints are disabled during switch.
    *
    * Note that I am assuming DR6_BD was clear before the #DB and so
    * I'm clearing it here.  If it was set, we will end up restoring
    * it cleared, but there's no way to tell.  Someone suggested that
    * ICEBP would tell us but it may also clear DR6<3:0>.
    *
    * SAVE_DR(6) can raise #DB.
    */

   if (TaskGotException(crosspage, EXC_DB) &&
       (crosspage->crosspageData.hostDR[6] & DR6_BD)) {
      crosspage->crosspageData.hostDR[6] -= DR6_BD;
      crosspage->crosspageData.hostDR[7] |= DR7_GD;
      SET_DR7(DR7_DEFAULT);

      /* HW: %dr7 and %dr6 are the guest, %dr3, %dr2, %dr1, %dr0 are host */
      crosspage->crosspageData.hostDRInHW = ((1 << 3) | (1 << 2) |
                                             (1 << 1) | (1 << 0));
   }

   /*
    * No GD bit, check for enabled breakpoints.  Disable them as they may
    * coincidentally trip during the switch.
    */

   else if (crosspage->crosspageData.hostDR[7] & DR7_ENABLED) {
      SET_DR7(DR7_DEFAULT);          // no #DB here, just simple set
      /* HW: %dr7 = guest, %dr6, %dr3, %dr2, %dr1, %dr0 = host */
      crosspage->crosspageData.hostDRInHW = ((1 << 6) | (1 << 3) | (1 << 2) |
                                             (1 << 1) | (1 << 0));
   }

   TaskSetException(crosspage, EXC_DB, saveGotDB);

   /*
    * hostDR[6,7] have host contents in them now.
    */

   crosspage->crosspageData.hostDRSaved = 0xC0;
#undef SAVE_DR
}


/*
 *-----------------------------------------------------------------------------
 *
 * TaskRestoreDebugRegisters --
 *
 *      Put the debug registers back the way they were when
 *      TaskSaveDebugRegisters was called.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Debug registers restored from values saved in the crosspage.
 *
 *-----------------------------------------------------------------------------
 */

static INLINE_SINGLE_CALLER void
TaskRestoreDebugRegisters(VMCrossPageData *crosspage)
{
#define RESTORE_DR(n)                                                 \
   if ((crosspage->hostDRInHW & (1 << n)) == 0) {                     \
      /* Guest value for register 'n' in hardware. */                 \
      const uintptr_t drReg = (uintptr_t)crosspage->hostDR[n];        \
      if (!(crosspage->shadowDRInHW & (1 << n)) ||                    \
          (drReg != SHADOW_DR(crosspage, n))) {                       \
         SET_DR##n(drReg);                                            \
      }                                                               \
   }

   RESTORE_DR(0);
   RESTORE_DR(1);
   RESTORE_DR(2);
   RESTORE_DR(3);
   RESTORE_DR(6);

   /*
    * DR7 must be restored last in case DR7<GD> is set.
    */
   RESTORE_DR(7);
#undef RESTORE_DR
}


/*
 *-----------------------------------------------------------------------------
 *
 * TaskUpdateLatestPTSC --
 *
 *      Record the per-VM latest visible PTSC value, and indicate that
 *      this thread is no longer running in the VMM.  See
 *      TaskUpdatePTSCParameters.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      May update the latest PTSC value and the PTSC offset reference count.
 *
 *-----------------------------------------------------------------------------
 */

static INLINE_SINGLE_CALLER void
TaskUpdateLatestPTSC(VMDriver *vm, VMCrossPageData *crosspage)
{
   if (Vmx86_HwTSCsSynced()) {
      uint64 latest;
      /*
       * Determine a conservative estimate for the last PTSC value the
       * VMM may have used.  We can't just use
       * crosspage->worldSwitchPTSC since some callees of BackToHost
       * will compute their own PTSC (or infer a PTSC value from the
       * TSC).
       */
      uint64 ptsc = RDTSC() + crosspage->pseudoTSCConv.p.add;
      do {
         latest = Atomic_Read64(&vm->ptscLatest);
         if (ptsc <= latest) {
            break;
         }
      } while (!Atomic_CMPXCHG64(&vm->ptscLatest, latest, ptsc));
      /* After updating the latest PTSC, decrement the reference count. */
      Atomic_Dec32((Atomic_uint32 *)&vm->ptscOffsetInfo.inVMMCnt);
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * TaskUpdatePTSCParameters --
 *
 *      If the PTSC is behind where it should be, based on the host's
 *      uptime, then adjust the PTSC parameters.  PR 118376.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      May update the PTSC parameters.
 *
 *-----------------------------------------------------------------------------
 */

static INLINE_SINGLE_CALLER void
TaskUpdatePTSCParameters(VMDriver *vm,
                         VMCrossPageData *crosspage,
                         Vcpuid vcpuid)
{
   uint64 tsc, ptsc;

   ASSERT_NO_INTERRUPTS();
   ASSERT_ON_COMPILE(sizeof(vm->ptscOffsetInfo) == sizeof(Atomic_uint64));
   ptsc = Vmx86_GetPseudoTSC();
   /*
    * Use unsigned comparison to test ptsc inside the interval:
    *   [worldSwitchPTSC, worldSwitchPTSC + largeDelta)
    * where largeDelta is choosen to be much larger than the normal time
    * between worldswitches, but not so large that we'd miss a jump due
    * to TSC reset.
    */
   if (UNLIKELY((uint64)(ptsc - crosspage->worldSwitchPTSC) >
                Vmx86_GetPseudoTSCHz() * 4096)) {
      /*
       * If the PTSC went backwards since we last left the monitor, then either:
       *  a) TSC is unsynchronized across cores.
       *  b) TSC was reset (probably due to host stand by or hibernate).
       *  c) khzEstimate was incorrect (too low).
       *  d) the host's reference clock is too low resolution.
       *  e) the host's reference clock is broken.
       *
       * We handle case (a) and (b) by switch PTSC over to using the
       * reference clock as the basis for pseudo TSC.
       *
       * For case (c), ideally we'd want to get khzEstimate correct in
       * the first place.  Using the reference clock for pseudo TSC is
       * just a backup if all else failed.  It will prevent PTSC from
       * drifting from real time over the long run.  Additionally, we
       * could try to adopt the mult/shift of pseudoTSCConv to make PTSC
       * run at the (incorrect) TSC kHz estimate, so that PTSC
       * progresses at the correct rate over the short term (while in
       * the monitor).
       *
       * We don't do anything for case (e).  If we see it happen, we
       * could try to pin the value returned by HostIF_ReadUptime to
       * some sane range to help compensate.
       */
      if (Vmx86_SetPseudoTSCUseRefClock()) {
         ptsc = Vmx86_GetPseudoTSC();
      }

      /*
       * For case (d), check for PTSC between (worldSwitchPTSC - Hz) and
       * worldSwitchPTSC.  That is, if ptsc is still behind
       * worldSwitchPTSC (even after ensuring the PTSC is based on the
       * reference clock), but by less than a second, assume that the
       * reference clock is too low of resolution, and nudge PTSC
       * forward to ensure it doesn't go backwards on this VCPU.  If we
       * are more than a second behind, then we assume that the
       * reference clock was stepped (or broken) and we just stay in
       * sync with it.
       */
      if ((uint64)(crosspage->worldSwitchPTSC - ptsc) <
          Vmx86_GetPseudoTSCHz()) {
         ptsc = crosspage->worldSwitchPTSC;
      }
   }

   /*
    * While running in the monitor, we can't read the reference
    * clock, which is implemented by the host OS.  So, offset from
    * the current pseudoTSC value using the TSC in order to provide
    * high resolution PTSC while in the monitor.  The RDTSC below
    * must be executed on the same pcpu that the vmm vcpu thread will
    * run on (in case of out of sync TSCs).  This is guaranteed since
    * we are on the on-ramp into the monitor with interrupts
    * disabled.
    */
   tsc = RDTSC();
   if (Vmx86_HwTSCsSynced()) {
      /*
       * When the TSCs are synchronized, make Pseudo TSC synchronized
       * as well.  To ensure this, all vcpu threads of a VM that are
       * simultaneously running their VMMs need to use the same exact
       * offset.  This global offset can be updated only when no
       * threads are running in the VMM.  In the case of synchronized
       * TSCs, updating the offset only when all threads are outside
       * the VMM is okay in terms of keeping VMMs' PTSC close to real
       * time because the TSCs stop only when all cores enter a deep
       * sleep state (otherwise the TSCs wouldn't be in sync to begin
       * with).
       */
      PseudoTSCOffsetInfo old, new;
      do {
         old = vm->ptscOffsetInfo;
         new = old;
         if (new.inVMMCnt == 0) {
            int64 ptscOffset;
            if (Vmx86_PseudoTSCUsesRefClock()) {
               /* Must read ptscLatest after reading ptscOffsetInfo. */
               uint64 latest = Atomic_Read64(&vm->ptscLatest);
               if (UNLIKELY(ptsc < latest)) {
                  /*
                   * The Vmx86_GetPseudoTSC call above occurred before
                   * some other vcpu thread exited the monitor; need to
                   * bump forward.
                   */
                  ptsc = latest;
               }
               ptscOffset = ptsc - tsc;
            } else {
               ptscOffset = Vmx86_GetPseudoTSCOffset();
            }
            /*
             * Since inVMMCnt is zero, it is safe to update our entry in
             * ptscOffsets -- no other thread will try to read it until
             * the inVMMCnt > 0.
             */
            vm->ptscOffsets[vcpuid] = ptscOffset;
            /* Try to use this thread's offset as the global offset. */
            new.vcpuid = vcpuid;
         }
         new.inVMMCnt++;
      } while (!Atomic_CMPXCHG64((Atomic_uint64 *)&vm->ptscOffsetInfo,
                                 *(uint64 *)&old, *(uint64 *)&new));
      /* Use the designated global offset as this thread's offset. */
      VersionedAtomic_BeginWrite(&crosspage->pseudoTSCConv.vers);
      crosspage->pseudoTSCConv.p.add   = vm->ptscOffsets[new.vcpuid];
      VersionedAtomic_EndWrite(&crosspage->pseudoTSCConv.vers);
      /*
       * Need to derive the worldSwitchPTSC value from TSC since the
       * PTSC, when calculated from TSC, may drift from the reference
       * clock over the short term.
       */
      ptsc = tsc + crosspage->pseudoTSCConv.p.add;
   } else {
      VersionedAtomic_BeginWrite(&crosspage->pseudoTSCConv.vers);
      crosspage->pseudoTSCConv.p.add   = ptsc - tsc;
      VersionedAtomic_EndWrite(&crosspage->pseudoTSCConv.vers);
   }
   /* Cache PTSC value for BackToHost. */
   crosspage->worldSwitchPTSC = ptsc;
}


/*
 *-----------------------------------------------------------------------------
 *
 * TaskSwitchToMonitor --
 *
 *      Wrapper that calls code to switch from the host to the monitor.
 *
 *      The basic idea is to do a (*(crosspage->hostToVmm))(crosspage)
 *      but it's complicated because we must have a common call format
 *      between GCC and MSC.
 *
 *      Since we have complete control over what GCC does with asm volatile,
 *      this amounts to having GCC do exactly what MSC does.
 *      For 64-bit hosts, we pass the parameter in RCX.
 *
 *      For 64-bit GCC, the callee is expected to preserve
 *      RBX,RBP,RSP,R12..R15, whereas MSC expects the callee to preserve
 *      RBX,RSI,RDI,RBP,RSP,R12..R15.  So for simplicity, we have the
 *      worldswitch code save RBX,RSI,RDI,RBP,RSP,R12..R15.
 *
 * From an email with Petr regarding gcc's handling of the stdcall
 * attribute for x86-64:
 *
 *    As far as I can tell, for x86_64 there is only one calling
 *    convention:
 *       On GCC rdi/rsi/rdx/rcx/r8d/r9d for <= 6 arguments,
 *       others always on stack, caller always adjusts stack.
 *
 *       On MSC it is rcx/rdx/r8d/r9d for <= 4 arguments, rest on
 *       stack.  When more than 4 arguments are passed, spill space is
 *       reserved on the stack for the register arguments.  Argument
 *       5 is accessed at (5 * 8)(rsp).
 *
 * Side effects:
 *      The monitor does many things, but it's irrelevant to this code.  The
 *      worldswitch should eventually return here with the host state intact.
 *
 *-----------------------------------------------------------------------------
 */

static INLINE_SINGLE_CALLER void
TaskSwitchToMonitor(VMCrossPage *crosspage)
{
   const uint8 *codePtr = ((uint8 *)&crosspage->crosspageCode +
                           crosspage->crosspageCode.offsets.hostToVmm);

#if defined(__GNUC__)
   /*
    * Pass the crosspage pointer in RCX just like 64-bit MSC does.
    * Tell GCC that the worldswitch preserves RBX,RSI,RDI,RBP,RSP,
    * R12..R15 just like the MSC 64-bit calling convention.
    */

   {
      uint64 raxGetsWiped, rcxGetsWiped;

      __asm__ __volatile__("call *%%rax"
                           : "=a" (raxGetsWiped),
                             "=c" (rcxGetsWiped)
                           : "0" (codePtr),
                             "1" (crosspage)
                           : "rdx", "r8", "r9", "r10", "r11", "cc", "memory");
   }
#elif defined(_MSC_VER)
   /*
    * The 64-bit calling convention is to pass the argument in RCX and that
    * the called function must preserve RBX,RSI,RDI,RBP,RSP,R12..R15.
    */
#pragma warning(suppress: 4055) // Cast of data pointer to function pointer.
   (*(void (*)(VMCrossPage *))codePtr)(crosspage);
#else
#error No compiler defined for TaskSwitchToMonitor
#endif
}


static void
TaskTestCrossPageExceptionHandlers(VMCrossPage *crosspage)
{
   static Bool testSwitchNMI = TRUE; /* test only first time through */

   /*
    * Test the DB,NMI,MCE handlers to make sure they can set the
    * flags.  This is calling the handlers in switchNMI.S.
    */

   if (vmx86_debug && testSwitchNMI) {
      Bool gotSave;

      testSwitchNMI = FALSE;

      /*
       * RAISE_INTERRUPT calls Switch{32,64}DBHandler in switchNMI.S
       * (depending on host bitsize).
       */

      gotSave = TaskGotException(crosspage, EXC_DB);
      TaskSetException(crosspage, EXC_DB, FALSE);
      RAISE_INTERRUPT(1);
      TS_ASSERT(TaskGotException(crosspage, EXC_DB));
      TaskSetException(crosspage, EXC_DB, gotSave);

      /*
       * RAISE_INTERRUPT calls Switch{32,64}NMIHandler in switchNMI.S
       * (depending on host bitsize).
       */
      gotSave = TaskGotException(crosspage, EXC_NMI);
      TaskSetException(crosspage, EXC_NMI, FALSE);
      RAISE_INTERRUPT(EXC_NMI);
      TS_ASSERT(TaskGotException(crosspage, EXC_NMI));

#if defined(__GNUC__)
      /*
       * Test the LRETQ in the 64-bit mini NMI handler to make sure
       * it works with any 16-byte offset of the stack pointer.
       * The INT 2 calls Switch64NMIHandler in switchNMI.S.
       */
      {
         uint64 v1, v2;

         asm volatile ("\n"
                       "        movl    $16, %%ecx      \n"
                       "1000:                           \n"
                       "        decq    %%rsp           \n"
                       "        movb    $0xDB, (%%rsp)  \n"
                       "        int     $2              \n"
                       "        loop    1000b           \n"
                       "        popq    %%rcx           \n"
                       "        popq    %%rax           \n"
                       : "=a" (v1), "=c" (v2));

         /*
          * Ensure nothing overwritten just above where it is
          * allowed to, because the decq rsp/movb 0xDBs pushed 16
          * of them one byte at a time.
          */

         TS_ASSERT(v1 == 0xDBDBDBDBDBDBDBDBULL);
         TS_ASSERT(v2 == 0xDBDBDBDBDBDBDBDBULL);
      }
#endif
      TaskSetException(crosspage, EXC_NMI, gotSave);

      /*
       * RAISE_INTERRUPT calls Switch{32,64}MCEHandler in switchNMI.S
       * (depending on host bitsize).
       */

      gotSave = TaskGotException(crosspage, EXC_MC);
      TaskSetException(crosspage, EXC_MC, FALSE);
      RAISE_INTERRUPT(EXC_MC);
      TS_ASSERT(TaskGotException(crosspage, EXC_MC));
      TaskSetException(crosspage, EXC_MC, gotSave);
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * TaskShouldRetryWorldSwitch --
 *
 *      Returns whether or not we should retry the world switch.
 *
 *      It is possible that the gotNMI and/or gotMCE was detected when
 *      switching in the host->monitor direction, in which case the
 *      retryWorldSwitch flag will be set.  If such is the case, we
 *      want to immediately loop back to the monitor as that is what
 *      it is expecting us to do.
 *
 *-----------------------------------------------------------------------------
 */

static INLINE Bool
TaskShouldRetryWorldSwitch(VMCrossPage *crosspage)
{
   Bool result = crosspage->crosspageData.retryWorldSwitch;
   crosspage->crosspageData.retryWorldSwitch = FALSE;
   return result;
}


/*
 *-----------------------------------------------------------------------------
 *
 * Task_Switch --
 *
 *      Switches from the host context into the monitor context and
 *      then receives control when the monitor returns to the
 *      host.
 *
 *      Think of it as a coroutine switch that changes not only the
 *      registers, but also the address space and all the hardware
 *      state.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Jump to the monitor. Has no direct effect on the host-visible
 *      state except that it might generate an interrupt.
 *
 *-----------------------------------------------------------------------------
 */

void
Task_Switch(VMDriver *vm,  // IN
            Vcpuid vcpuid) // IN
{
   uintptr_t   flags;
   uint64      fs64  = 0;
   uint64      gs64  = 0;
   uint64      kgs64 = 0;
   uint64      ptMSR = 0;
   uint64      pebsMSR = 0;
   DTR64       hostGDT64, hostIDT64;
   Selector    cs, ds, es, fs, gs, ss;
   Selector    hostTR;
   Selector    hostLDT;
   Bool lint0NMI;
   Bool lint1NMI;
   Bool pcNMI;
   Bool thermalNMI;
   VMCrossPage *crosspage = vm->crosspage[vcpuid];
   uint32 pCPU;
   MPN hvRootMPN;
   Descriptor *tempGDTBase;

   ASSERT_ON_COMPILE(sizeof(VMCrossPage) == PAGE_SIZE);
   TaskDisableNMI(&vm->hostAPIC, &lint0NMI, &lint1NMI, &pcNMI, &thermalNMI);
   SAVE_FLAGS(flags);
   CLEAR_INTERRUPTS();

   pCPU = HostIF_GetCurrentPCPU();
   ASSERT(pCPU < ARRAYSIZE(hvRootPage) && pCPU < ARRAYSIZE(tmpGDT));

   hvRootMPN = Atomic_Read64(&hvRootPage[pCPU]);
   tempGDTBase = USE_TEMPORARY_GDT ? Atomic_ReadPtr(&tmpGDT[pCPU])
                                   : default_rw_gdt;

   /*
    * We can't allocate memory with interrupts disabled on all hosts
    * so we dummy up a modulecall to do it before we start in on the
    * world switch.  We must be careful not to overwrite the
    * crosspages arguments when doing this though, see bug 820257.
    */
   if (hvRootMPN == INVALID_MPN) {
      crosspage->crosspageData.userCallType = MODULECALL_USERCALL_NONE;
      crosspage->crosspageData.moduleCallType = MODULECALL_ALLOC_VMX_PAGE;
      crosspage->crosspageData.pcpuNum = pCPU;
   } else if (USE_TEMPORARY_GDT && tempGDTBase == NULL) {
      crosspage->crosspageData.userCallType = MODULECALL_USERCALL_NONE;
      crosspage->crosspageData.moduleCallType = MODULECALL_ALLOC_TMP_GDT;
      crosspage->crosspageData.pcpuNum = pCPU;
   } else {
      do {
         uintptr_t cr0reg, cr2reg, cr3reg, cr4reg;
         uint64 efer     = ~0ULL;
         Bool needVMXOFF = FALSE;
         MA foreignVMCS  = ~0ULL;
         MA foreignHSAVE = ~0ULL;

         vm->currentHostCpu[vcpuid] = pCPU;

         TaskUpdatePTSCParameters(vm, &crosspage->crosspageData, vcpuid);

         /*
          * Disable PEBS if it is supported and enabled.  Do this while on the
          * hosts IDT - PR 848701.
          */
         if (pebsAvailable) {
            pebsMSR = __GET_MSR(IA32_MSR_PEBS_ENABLE);
            if (pebsMSR != 0) {
               __SET_MSR(IA32_MSR_PEBS_ENABLE, 0);
            }
         }

         /*
          * The same as PEBS disable PT(Processor Trace) if it is supported and
          * enabled.
          */
         if (ptAvailable) {
            ptMSR = __GET_MSR(MSR_RTIT_CTL);
            if ((ptMSR & MSR_RTIT_CTL_TRACE_EN) != 0) {
               __SET_MSR(MSR_RTIT_CTL, ptMSR & ~MSR_RTIT_CTL_TRACE_EN);
            }
         }

         /*
          * Save the host's standard IDT and set up an IDT that only
          * has space for all the hardware exceptions (though only a
          * few are handled).
          */

         TaskSaveIDT64(&hostIDT64);
         TaskLoadIDT64(&crosspage->crosspageData.switchHostIDTR);
         TaskTestCrossPageExceptionHandlers(crosspage);

         if (CPUID_GetVendor() == CPUID_VENDOR_INTEL) {
            /*
             * Ensure that VMX is enabled and locked in the feature control MSR,
             * so that we can set CR4.VMXE to activate VMX.
             */
            uint64 bits = MSR_FEATCTL_LOCK | MSR_FEATCTL_VMXE;
            uint64 featCtl = __GET_MSR(MSR_FEATCTL);
            if ((featCtl & bits) != bits) {
               if ((featCtl & MSR_FEATCTL_LOCK) != 0) {
                  Panic("Intel VT-x is disabled and locked on CPU %d\n", pCPU);
               }
               __SET_MSR(MSR_FEATCTL, featCtl | bits);
            }
         }

         /*
          * Save CR state.  The monitor deals with EFER.
          */

         GET_CR2(cr2reg);
         GET_CR0(cr0reg);
         GET_CR4(cr4reg);
         GET_CR3(cr3reg);
         crosspage->crosspageData.hostCR3 = cr3reg;

         /*
          * Any reserved bits in CR0 must be preserved when we switch
          * to the VMM. [See PR 291004.]  (On the other hand, Intel
          * recommends that we clear any reserved CR4 bits.)
          */
         crosspage->crosspageData.wsCR0 &= ~CR0_RESERVED;
         crosspage->crosspageData.wsCR0 |= (cr0reg & CR0_RESERVED);

         /*
          * CR4.VMXE must be enabled to support VMX in the monitor.
          */
         if (CPUID_GetVendor() == CPUID_VENDOR_INTEL) {
            crosspage->crosspageData.wsCR4 |= CR4_VMXE;
         }

         /*
          * The world-switch CR4.MCE and CR4.PCIDE should always reflect the
          * host's values.  CR4.PCIDE will be cleared once we're in the monitor,
          * running on a CR3 with a PCID field of 0.
          */
         crosspage->crosspageData.wsCR4 =
            (crosspage->crosspageData.wsCR4 & ~(CR4_MCE | CR4_PCIDE)) |
            (cr4reg & (CR4_MCE | CR4_PCIDE));

         /*
          * The world-switch should never have global pages enabled.  Therefore,
          * switching to the monitor's CR4 ensures that global pages are
          * flushed.
          */
         ASSERT((crosspage->crosspageData.wsCR4 & CR4_PGE) == 0);

         /*
          * Load the world-switch CR0 and CR4.  We can't load the monitor's
          * CR3 yet, because the current code isn't mapped into the
          * monitor's address space.
          */
         SET_CR0((uintptr_t)crosspage->crosspageData.wsCR0);
         SET_CR4((uintptr_t)crosspage->crosspageData.wsCR4);

         TaskSaveDebugRegisters(crosspage);

         TaskSaveGDT64(&hostGDT64);

         if (CPUID_GetVendor() == CPUID_VENDOR_INTEL) {
            MA vmxonRegion = MPN_2_MA(hvRootMPN);
            VMXStatus status = VMXON_2_STATUS(&vmxonRegion);
            if (status == VMX_Success) {
               needVMXOFF = TRUE;
            } else {
               VMPTRST(&foreignVMCS);
            }
         }

         if (CPUID_GetVendor() == CPUID_VENDOR_AMD) {
            efer = __GET_MSR(MSR_EFER);
            if ((efer & MSR_EFER_SVME) == 0) {
               __SET_MSR(MSR_EFER, efer | MSR_EFER_SVME);
            }
            foreignHSAVE = __GET_MSR(MSR_VM_HSAVE_PA);
            __SET_MSR(MSR_VM_HSAVE_PA, MPN_2_MA(hvRootMPN));
         }

         /*
          * If NMI stress testing enabled, set EFLAGS<TF>.  This will
          * make sure there is a valid IDT, GDT, stack, etc. at every
          * instruction boundary during the switch.
          */
         if (WS_INTR_STRESS) {
            TaskEnableTF();
         }

         /*
          * GS and FS are saved outside of the TaskSwitchToMonitor() code to
          *
          * 1) minimize the amount of code handled there, and
          *
          * 2) prevent us from faulting if they happen to be in the LDT
          *    (since the LDT is saved and restored here too).
          *
          * Also, the 32-bit Mac OS running in legacy mode has
          * CS, DS, ES, SS in the LDT!
          */
         cs = GET_CS();
         ss = GET_SS();
#if defined __APPLE__
         /*
          * The 64-bit Mac OS kernel leaks segment selectors from
          * other threads into 64-bit threads.  When the selectors
          * reference a foreign thread's LDT, we may not be able to
          * reload them using our thread's LDT.  So, let's just clear
          * them instead of trying to preserve them.  [PR 467140]
          */
         ds = 0;
         es = 0;
         fs = 0;
         gs = 0;
#else
         ds = GET_DS();
         es = GET_ES();
         fs = GET_FS();
         gs = GET_GS();
#endif
         GET_LDT(hostLDT);
         GET_TR(hostTR);

         kgs64 = GET_KernelGS64();
         gs64  = GET_GS64();
         fs64  = GET_FS64();

         /*
          * Make sure stack segment is non-zero so worldswitch can use it
          * to temporarily restore DS,ES on return.
          */
         if (ss == 0) {
            SET_SS(kernelStackSegment);
         }

         TS_ASSERT(SELECTOR_TABLE(cs) == SELECTOR_GDT);
         TS_ASSERT(SELECTOR_TABLE(ds) == SELECTOR_GDT);
         TS_ASSERT(SELECTOR_TABLE(ss) == SELECTOR_GDT);

         DEBUG_ONLY(crosspage->crosspageData.tinyStack[0] = 0xDEADBEEF;)
         /* Running in host context prior to TaskSwitchToMonitor() */
         TaskSwitchToMonitor(crosspage);
         /* Running in host context after to TaskSwitchToMonitor() */

         TS_ASSERT(crosspage->crosspageData.tinyStack[0] == 0xDEADBEEF);

         /*
          * Temporarily disable single-step stress as VMX/VMCS change code
          * ASSERTS on RFLAGS content without allowing TF/RF to be set.
          */
         if (WS_INTR_STRESS) {
            TaskDisableTF();
         }

         if (needVMXOFF) {
            VMXOFF();
         } else if (foreignVMCS != ~0ULL) {
            VMPTRLD_UNCHECKED(&foreignVMCS);
         }

         if (WS_INTR_STRESS) {
            TaskEnableTF();
         }

         if (CPUID_GetVendor() == CPUID_VENDOR_AMD) {
            __SET_MSR(MSR_VM_HSAVE_PA, foreignHSAVE);
            if ((efer & MSR_EFER_SVME) == 0) {
               __SET_MSR(MSR_EFER, efer);
            }
         }

         /*
          * Restore CR state.
          * CR3 should already have been restored.  CR0 and CR4 have to
          * be restored if the world switch values do not match the host's.
          * CR2 always has to be restored.  CR8 never has to be restored.
          */
         SET_CR2(cr2reg);
         if (crosspage->crosspageData.wsCR0 != cr0reg) {
            SET_CR0(cr0reg);
         }
         if (crosspage->crosspageData.wsCR4 != cr4reg) {
            SET_CR4(cr4reg);
         } else if ((cr4reg & CR4_PCIDE) != 0) {
            /*
             * Flush PCID 0.
             */
            ASSERT((cr4reg & CR4_PGE) == 0);
            SET_CR4(cr4reg | CR4_PGE);
            SET_CR4(cr4reg);
         }
         if (vmx86_debug) {
            uintptr_t cr;
            GET_CR0(cr);
            ASSERT(cr == cr0reg);
            GET_CR4(cr);
            ASSERT(cr == cr4reg);
            GET_CR3(cr);
            ASSERT(cr == cr3reg);
         }

         /*
          * TaskSwitchToMonitor() returns with GDT = crossGDT so switch back to
          * the host GDT here.  We will also restore host TR as the task busy
          * bit needs to be fiddled with.  Also restore host LDT while we're
          * at it.
          */
         TaskRestoreHostGDTTRLDT(tempGDTBase, hostGDT64,
                                 hostLDT, cs, hostTR);

         SET_DS(ds);
         SET_ES(es);

         /*
          * First, restore %fs and %gs from the in-memory descriptor tables,
          * and then overwrite the bases in the descriptor cache with the
          * saved 64-bit values.
          */

         SET_FS(fs);
         SET_GS(gs);
         SET_FS64(fs64);
         SET_GS64(gs64);
         SET_KernelGS64(kgs64);

         /* Restore debug registers and host's IDT; turn off stress test. */
         if (WS_INTR_STRESS) {
            TaskDisableTF();
         }

         TaskRestoreDebugRegisters(&crosspage->crosspageData);

         ASSERT_NO_INTERRUPTS();

         /*
          * Restore standard host interrupt table and re-enable PEBS afterwards
          * iff we disabled it.
          */

         TaskLoadIDT64(&hostIDT64);

         if (pebsMSR != 0) {
            __SET_MSR(IA32_MSR_PEBS_ENABLE, pebsMSR);
         }

         if ((ptMSR & MSR_RTIT_CTL_TRACE_EN) != 0) {
            __SET_MSR(MSR_RTIT_CTL, ptMSR);
         }

         TaskUpdateLatestPTSC(vm, &crosspage->crosspageData);
         vm->currentHostCpu[vcpuid] = INVALID_PCPU;

         /*
          * If an #NMI or #MCE was logged while switching, re-raise such an
          * interrupt or exception for the host to consume.  Handlers preserve
          * NMI-blocking (when not stress-testing or changing VIP/VIP) by using
          * synthetic irets instead of real irets.  By this point, if an NMI
          * was received during switching, NMIs should still be blocked.
          *
          * When stress testing, NMIs are almost guaranteed to be synthetic, so
          * no NMI is raised.
          *
          * If a #UD was logged while switching, warn accordingly rather than
          * raising a new exception as this would likely panic the host kernel.
          */

         if (UNLIKELY(TaskGotException(crosspage, EXC_NMI))) {
            TaskSetException(crosspage, EXC_NMI, FALSE);
            if (!WS_INTR_STRESS) {
               RAISE_INTERRUPT(EXC_NMI);
            }
         }

         if (UNLIKELY(TaskGotException(crosspage, EXC_MC))) {
            TaskSetException(crosspage, EXC_MC, FALSE);
            if (vmx86_debug) {
               CP_PutStr("Task_Switch: forwarding MCE to host\n");
            }
            RAISE_INTERRUPT(EXC_MC);
         }
         if (UNLIKELY(TaskGotException(crosspage, EXC_UD))) {
            Warning("#UD occurred on switch back to host; dumping core");
         }
         /*
          * The NMI/MCE checks above are special cases for interrupts
          * received during worldswitch.  Here is the more generic case
          * of forwarding NMIs received while executing the VMM/guest.
          */
         if (crosspage->crosspageData.moduleCallType == MODULECALL_INTR &&
             crosspage->crosspageData.args[0] == EXC_NMI) {
            /*
             * If VMM was interrupted by an NMI, do the INT 2 so the
             * host will handle it, but then return immediately to the
             * VMM in case the VMM was in the middle of a critical
             * region.  E.g. the NMI may have interrupted the VMM while
             * an interrupt was in service, before the VMM or host has
             * done the EOI.
             */
            RAISE_INTERRUPT(EXC_NMI);
            crosspage->crosspageData.retryWorldSwitch = TRUE;
         }
      } while (UNLIKELY(TaskShouldRetryWorldSwitch(crosspage)));
   }

   if (crosspage->crosspageData.moduleCallType == MODULECALL_INTR) {
      /*
       * Note we must do the RAISE_INTERRUPT before ever enabling
       * interrupts or bad things have happened (might want to know exactly
       * what bad things btw).
       */
#ifdef _WIN64
      if (crosspage->crosspageData.args[0] <= 0xFF &&
          (crosspage->crosspageData.args[0] >= 0x14 ||
           crosspage->crosspageData.args[0] == EXC_MC)) {
         RAISE_INTERRUPT((unsigned char)crosspage->crosspageData.args[0]);
      } else {
         Warning("%s: Received Unexpected Interrupt: 0x%"FMT64"X\n",
                 __FUNCTION__, crosspage->crosspageData.args[0]);
         Panic("Received Unexpected Interrupt: 0x%"FMT64"X\n",
               crosspage->crosspageData.args[0]);
      }
#else
      /*
       * Note2 RAISE_INTERRUPT() only takes a constant and hence with switch
       * statement.
       */
#define IRQ_INT(_x) case _x: RAISE_INTERRUPT(_x); break
#define IRQ_INT2(_x) IRQ_INT(_x); IRQ_INT(_x + 1)
#define IRQ_INT4(_x) IRQ_INT2(_x); IRQ_INT2(_x + 2)
#define IRQ_INT8(_x) IRQ_INT4(_x); IRQ_INT4(_x + 4)
#define IRQ_INT16(_x) IRQ_INT8(_x); IRQ_INT8(_x + 8)
#define IRQ_INT32(_x) IRQ_INT16(_x); IRQ_INT16(_x + 16)

      switch (crosspage->crosspageData.args[0]) {
         // These are the general IO interrupts
         // It would be nice to generate this dynamically, but see Note2 above.

         /*
          * Pass Machine Check Exception (Interrupt 0x12) to the host.
          * See bug #45286 for details.
          */
         IRQ_INT(EXC_MC);

         /*
          * pass the reserved vectors (20-31) as well. amd64 windows
          * generates these.
          */

         IRQ_INT8(0x14);
         IRQ_INT4(0x1c);

         IRQ_INT32(0x20);
         IRQ_INT32(0x40);
         IRQ_INT32(0x60);
         IRQ_INT32(0x80);
         IRQ_INT32(0xa0);
         IRQ_INT32(0xc0);
         IRQ_INT32(0xe0);

      default:
         /*
          * XXXX nt running on a 2 processor machine we hit this Panic
          * with int 0xD1 0x61 ...
          */

         Warning("%s: Received Unexpected Interrupt: 0x%"FMT64"X\n",
                 __FUNCTION__, crosspage->crosspageData.args[0]);
         Panic("Received Unexpected Interrupt: 0x%"FMT64"X\n",
               crosspage->crosspageData.args[0]);
      }
#endif
   }

   RESTORE_FLAGS(flags);
   TaskRestoreNMI(&vm->hostAPIC, lint0NMI, lint1NMI, pcNMI, thermalNMI);
}
