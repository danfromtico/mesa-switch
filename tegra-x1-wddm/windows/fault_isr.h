/* SPDX-License-Identifier: MIT */
#ifndef TG_GPU_FAULT_ISR_H
#define TG_GPU_FAULT_ISR_H

#include <ntddk.h>
#include <dispmprt.h>
#include "../core/fault.h"
#include "../platform/t214_power.h"
#include "diagnostic_events.h"

/* GM20B NV_PMC_INTR_0 / NV_PMC_INTR_MSK_0 stall fault engines:
 * bit 8 = PFIFO (MMU_FAULT, DROPPED_MMU_FAULT, PBDMA, SCHED/BIND/CHSW/LB/FB_FLUSH)
 * bit 28 = PBUS (PRI timeout / squash / FECS error)
 * bit 30 = PPRIV_RING (PRI ring error)
 */
#define TG_GPU_MC_STALL_PFIFO     0x00000100u
#define TG_GPU_MC_STALL_PBUS      0x10000000u
#define TG_GPU_MC_STALL_PRIV_RING 0x40000000u
#define TG_GPU_MC_STALL_MASK      (TG_GPU_MC_STALL_PFIFO | TG_GPU_MC_STALL_PBUS | TG_GPU_MC_STALL_PRIV_RING)

#define TG_GPU_FAULT_SLOTS        4u
#define TG_GPU_FAULT_MAX_MMU_IDS  32u

typedef struct _TG_GPU_MMU_FAULT_ENTRY {
    ULONG Id;
    ULONG Inst;
    ULONG AddrLo;
    ULONG AddrHi;
    ULONG Info;
} TG_GPU_MMU_FAULT_ENTRY;

/* Per-epoch immutable top-half slot + separate PASSIVE capture slot (B07/B08). */
typedef struct _TG_GPU_FAULT_SLOT {
    volatile LONG SlotEpoch;
    TG_GPU_FAULT_ISR_EVENT TopHalf;
    TG_GPU_FAULT_ISR_EVENT Details;
    ULONG MmuEntryCount;
    TG_GPU_MMU_FAULT_ENTRY MmuEntries[TG_GPU_FAULT_MAX_MMU_IDS];
    ULONG PrivSysErr[4]; /* 0x122120..0x12212c: ADR, WRDAT, INFO, CODE */
    ULONG PrivGpcErr[4]; /* 0x128120..0x12812c: ADR, WRDAT, INFO, CODE */
    ULONG PrivValidMask; /* bit0=SYS (STATUS0 bit8), bit1=GPC0 (STATUS1 bit0) */
} TG_GPU_FAULT_SLOT;

typedef struct _TG_GPU_FAULT_ISR {
    volatile PULONG GpuRegs;
    volatile PULONG LicRegs;
    ULONG SavedLicEnable;
    ULONG SavedLicClass;
    ULONG SavedFifoEnable;
    ULONG SavedPbdmaEnable;
    ULONG SavedPmcMask;
    ULONG SavedPmcEnable;
    ULONG PendingRestore;
    BOOLEAN LicSaved;
    BOOLEAN GpuSaved;
    volatile LONG Armed;
    volatile LONG Faulted;
    volatile LONG GpuUnreachable;
    volatile LONG DpcPending;
    volatile LONG Epoch;
    volatile LONG ActiveEpoch; /* Nonzero ONLY while an armed running epoch is active (AB06). */
    volatile LONG FaultEpoch;
    /* Resident nonpaged diagnostic counters (survive across epochs).
     * IsrProbeEnterCount increments BEFORE the first GPU PRI read so a hung
     * read leaves IsrProbeEnterCount > IsrProbeDoneCount. */
    volatile LONG IsrProbeEnterCount;
    volatile LONG IsrProbeDoneCount;
    volatile LONG IsrEnterCount;
    volatile LONG IsrMaskCount;
    volatile LONG IsrUnreachableCount;
    volatile LONG DpcCount;
    volatile LONG CaptureCount;
    TG_GPU_FAULT_SLOT Slots[TG_GPU_FAULT_SLOTS];
} TG_GPU_FAULT_ISR;

_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS TgGpuArmFaultIsr(TG_GPU_FAULT_ISR *Isr, const DXGKRNL_INTERFACE *Dxgk,
                          PVOID GpuRegisters, PVOID LicRegisters);

/* Synchronizes with DIRQL to mask SPI 157 and detach MMIO pointers.
 * Atomically reports whether the ISR was armed and/or had a latched fault
 * inside the same DIRQL synchronization callback. On synchronization or
 * hardware restore failure, pointers/saved masks are retained for retry. */
_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS TgGpuDisarmFaultIsrEx(TG_GPU_FAULT_ISR *Isr, const DXGKRNL_INTERFACE *Dxgk,
                               BOOLEAN GpuPowered, BOOLEAN *WasArmed, BOOLEAN *HadFault);

_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS TgGpuDisarmFaultIsr(TG_GPU_FAULT_ISR *Isr, const DXGKRNL_INTERFACE *Dxgk,
                             BOOLEAN GpuPowered);

/* Minimal DIRQL top-half: increments IsrProbeEnterCount before the first GPU PRI
 * read, checks NV_PMC_INTR_0, latches Faulted & epoch into an immutable per-epoch
 * slot, masks GPU/LIC stall IRQ only, and queues a DPC. */
BOOLEAN TgGpuServiceFaultIsr(TG_GPU_FAULT_ISR *Isr, const DXGKRNL_INTERFACE *Dxgk);

/* Fast lock-free epoch/latch helpers for worker/submit/paging/TDR/NotifyFence. */
BOOLEAN TgGpuIsFaultLatched(const TG_GPU_FAULT_ISR *Isr);
LONG TgGpuCurrentFaultEpoch(const TG_GPU_FAULT_ISR *Isr);
BOOLEAN TgGpuFaultEpochMatches(const TG_GPU_FAULT_ISR *Isr, LONG ExpectedEpoch);
BOOLEAN TgGpuFaultEpochHealthy(const TG_GPU_FAULT_ISR *Isr, LONG ExpectedEpoch);

/* Consumes a pending DPC notification only if its fault epoch still matches
 * the active epoch both before AND after copying the immutable TopHalf slot. */
BOOLEAN TgGpuConsumeFaultDpc(TG_GPU_FAULT_ISR *Isr, TG_GPU_FAULT_ISR_EVENT *Event);

/* Returns only raw captured hardware fault words from the current fault slot. */
BOOLEAN TgGpuReadFaultIsr(const TG_GPU_FAULT_ISR *Isr, struct tg_gpu_fault_status *Faults);

/* Single-owner PASSIVE capture called after the worker is stopped and before
 * cold hardware reset. Populates the separate Details slot (never mutating
 * TopHalf), captures ALL set MMU fault IDs, PTIMER (rejecting 0xffffffff),
 * and SYS/GPC PRIV_RING station error registers. */
BOOLEAN TgGpuCaptureFaultDetails(TG_GPU_FAULT_ISR *Isr, const struct tg_gpu_power_io *Io,
                                 const struct tg_gpu_power *Power,
                                 TG_GPU_FAULT_ISR_EVENT *Event);

/* Read additional MMU fault entries and PRIV_RING station records captured by TgGpuCaptureFaultDetails. */
ULONG TgGpuGetCapturedMmuEntryCount(const TG_GPU_FAULT_ISR *Isr, LONG FaultEpoch);
BOOLEAN TgGpuGetCapturedMmuEntry(const TG_GPU_FAULT_ISR *Isr, LONG FaultEpoch,
                                 ULONG Index, TG_GPU_MMU_FAULT_ENTRY *Entry);
BOOLEAN TgGpuGetCapturedPrivRing(const TG_GPU_FAULT_ISR *Isr, LONG FaultEpoch,
                                 ULONG *ValidMask, ULONG SysErr[4], ULONG GpcErr[4]);

/* Pure-software DIRQL commit permission gate (V5-01/B04/B11/N3): runs mutually
 * exclusively with TgGpuServiceFaultIsr via DxgkCbSynchronizeExecution and
 * verifies TgGpuFaultEpochHealthy with ZERO hardware MMIO inside DIRQL. */
_IRQL_requires_max_(DISPATCH_LEVEL)
BOOLEAN TgGpuFaultIsrAcquireCommit(const TG_GPU_FAULT_ISR *Isr,
                                   const DXGKRNL_INTERFACE *Dxgk,
                                   LONG ExpectedEpoch,
                                   volatile LONG *DiagCommitSeq,
                                   volatile LONG *DiagPermitCount,
                                   volatile LONG *DiagCommitStep,
                                   volatile LONG *DiagLastMmioReg);

/* Tracked caller-IRQL MMIO write helper (V5-01/N3): executes outside DIRQL and
 * records exact register plus adjacent enter/done counters around Io->write32. */
VOID TgGpuFaultIsrTrackedWrite32(const struct tg_gpu_power_io *Io,
                                 ULONG Reg, ULONG Val, BOOLEAN FinalStep,
                                 volatile LONG *DiagCommitSeq,
                                 volatile LONG *DiagLastMmioReg,
                                 volatile LONG *DiagMmioEnterCount,
                                 volatile LONG *DiagMmioDoneCount,
                                 volatile LONG *DiagCommitStep);

#endif
