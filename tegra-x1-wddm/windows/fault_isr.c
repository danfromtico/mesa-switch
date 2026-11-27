/* SPDX-License-Identifier: MIT
 * Minimal DIRQL-safe GM20B Stall fault ISR + single-owner PASSIVE fault capture.
 * Register offsets follow Switchroot nvgpu 1ae0167d gk20a/fifo_gk20a.c,
 * common/mc/mc_gm20b.c, and common/priv_ring/priv_ring_gm20b.c.
 */
#include "fault_isr.h"
#include "control.h"
#include "paging.h"
#include "../platform/t214_resources.h"

#define GPU_PMC_INTR_0          0x000100u
#define GPU_PMC_INTR_EN_0       0x000140u
#define GPU_PMC_ENABLE          0x000200u
#define GPU_PMC_INTR_MSK_0      0x000640u
#define GPU_PBUS_INTR_0         0x001100u
#define GPU_PFIFO_INTR_0        0x002100u
#define GPU_PFIFO_INTR_EN_0     0x002140u
#define GPU_PFIFO_MMU_FAULT_ID  0x00259cu
#define GPU_PFIFO_MMU_FAULT_BASE 0x002800u
#define GPU_PTIMER_PRI_SAVE0    0x009084u
#define GPU_PTIMER_PRI_SAVE1    0x009088u
#define GPU_PTIMER_PRI_FECS     0x00908cu
#define GPU_PBDMA0_INTR_0       0x040108u
#define GPU_PBDMA0_INTR_EN_0    0x04010cu
#define GPU_PBDMA0_INTR_1       0x040148u
#define GPU_PRIV_RING_STATUS0   0x120058u
#define GPU_PRIV_RING_STATUS1   0x12005cu
/* NVIDIA GM20B hw_pri_ringstation_{sys,gpc}_gm20b.h (Switchroot 1ae0167d):
 * SYS ringstation error block is 0x122120..0x12212c; GPC0 is 0x128120..0x12812c. */
#define GPU_PRIV_SYS_ERR_ADR    0x122120u
#define GPU_PRIV_SYS_ERR_WRDAT  0x122124u
#define GPU_PRIV_SYS_ERR_INFO   0x122128u
#define GPU_PRIV_SYS_ERR_CODE   0x12212cu
#define GPU_PRIV_GPC_ERR_ADR    0x128120u
#define GPU_PRIV_GPC_ERR_WRDAT  0x128124u
#define GPU_PRIV_GPC_ERR_INFO   0x128128u
#define GPU_PRIV_GPC_ERR_CODE   0x12812cu
#define GPU_PGRAPH_INTR         0x400100u

#define PFIFO_PBDMA_SUMMARY     0x20000000u
#define PFIFO_RUNLIST_EVENT     0x40000000u
#define PBDMA_ENGINE_EVENT      0x01000000u

#define LIC_FIR_OFFSET          0u
#define LIC_FIR_CLR_OFFSET      8u
#define LIC_IER_OFFSET          12u
#define LIC_IER_SET_OFFSET      16u
#define LIC_IER_CLR_OFFSET      20u
#define LIC_CLASS_OFFSET        24u

/* Per-register pending restore bits (B09): distinguish unfinished arm_sync
 * rollback of GPU leaf/master masks from normal disarm PMC disable. */
#define TG_RESTORE_LIC_CLASS    0x01u
#define TG_RESTORE_LIC_IER      0x02u
#define TG_RESTORE_LIC_ALL      (TG_RESTORE_LIC_CLASS | TG_RESTORE_LIC_IER)
#define TG_RESTORE_FIFO_EN      0x04u
#define TG_RESTORE_PBDMA_EN     0x08u
#define TG_RESTORE_PMC_MASK     0x10u
#define TG_RESTORE_PMC_EN       0x20u
#define TG_RESTORE_GPU_ROLLBACK (TG_RESTORE_FIFO_EN | TG_RESTORE_PBDMA_EN | TG_RESTORE_PMC_MASK | TG_RESTORE_PMC_EN)
#define TG_RESTORE_PMC_DISABLE  0x40u
#define TG_RESTORE_GPU_ALL      (TG_RESTORE_GPU_ROLLBACK | TG_RESTORE_PMC_DISABLE)

static __forceinline ULONG reg_read(volatile PULONG base, ULONG byte_offset)
{
    ULONG v = READ_REGISTER_ULONG((PULONG)((PUCHAR)base + byte_offset));
    TgGpuDataSyncBarrier();
    return v;
}

static __forceinline VOID reg_write(volatile PULONG base, ULONG byte_offset, ULONG value)
{
    /* WDK WRITE_REGISTER_ULONG on ARM64 already issues a leading DSB. */
    WRITE_REGISTER_ULONG((PULONG)((PUCHAR)base + byte_offset), value);
    TgGpuDataSyncBarrier();
}

static BOOLEAN restore_lic_pending(TG_GPU_FAULT_ISR *isr, volatile PULONG lic)
{
    ULONG pending = isr->PendingRestore & TG_RESTORE_LIC_ALL;
    if (!isr->LicSaved && !pending)
        return TRUE;
    if (!lic)
        return FALSE;
    if (!pending)
        pending = TG_RESTORE_LIC_ALL;
    /* Temporarily masking LIC_IER bit 29 via IER_CLR_OFFSET obligates restoring
     * SavedLicEnable again before declaring LIC restore complete (B09). */
    reg_write(lic, LIC_IER_CLR_OFFSET, T214_GPU_LIC_STALL);
    reg_write(lic, LIC_FIR_CLR_OFFSET, T214_GPU_LIC_STALL);
    pending |= TG_RESTORE_LIC_IER;
    if (pending & TG_RESTORE_LIC_CLASS) {
        const ULONG cls = reg_read(lic, LIC_CLASS_OFFSET);
        if (cls != MAXULONG) {
            const ULONG want_cls = (cls & ~T214_GPU_LIC_STALL) | (isr->SavedLicClass & T214_GPU_LIC_STALL);
            reg_write(lic, LIC_CLASS_OFFSET, want_cls);
            const ULONG cls_after = reg_read(lic, LIC_CLASS_OFFSET);
            if (cls_after != MAXULONG && ((cls_after ^ isr->SavedLicClass) & T214_GPU_LIC_STALL) == 0u)
                pending &= ~TG_RESTORE_LIC_CLASS;
        }
    }
    if (pending & TG_RESTORE_LIC_IER) {
        if (isr->SavedLicEnable & T214_GPU_LIC_STALL)
            reg_write(lic, LIC_IER_SET_OFFSET, T214_GPU_LIC_STALL);
        else
            reg_write(lic, LIC_IER_CLR_OFFSET, T214_GPU_LIC_STALL);
        const ULONG fir_after = reg_read(lic, LIC_FIR_OFFSET);
        const ULONG ier_after = reg_read(lic, LIC_IER_OFFSET);
        /* LIC_IER is a complete 32-bit enable bitmap where 0xffffffff is valid;
         * verify live LIC aperture via FIR bit 29 cleared above + LIC_CLASS. */
        if ((fir_after & T214_GPU_LIC_STALL) == 0u &&
            ((ier_after ^ isr->SavedLicEnable) & T214_GPU_LIC_STALL) == 0u &&
            !(pending & TG_RESTORE_LIC_CLASS))
            pending &= ~TG_RESTORE_LIC_IER;
    }
    if (pending == 0u) {
        const ULONG fir_final = reg_read(lic, LIC_FIR_OFFSET);
        const ULONG cls_final = reg_read(lic, LIC_CLASS_OFFSET);
        const ULONG ier_final = reg_read(lic, LIC_IER_OFFSET);
        if (cls_final == MAXULONG || ((cls_final ^ isr->SavedLicClass) & T214_GPU_LIC_STALL) != 0u)
            pending |= TG_RESTORE_LIC_CLASS;
        if ((fir_final & T214_GPU_LIC_STALL) != 0u ||
            ((ier_final ^ isr->SavedLicEnable) & T214_GPU_LIC_STALL) != 0u)
            pending |= TG_RESTORE_LIC_IER;
    }
    isr->PendingRestore = (isr->PendingRestore & ~TG_RESTORE_LIC_ALL) | pending;
    if (pending == 0u) {
        isr->LicSaved = FALSE;
        return TRUE;
    }
    isr->LicSaved = TRUE;
    return FALSE;
}

static BOOLEAN restore_gpu_pending(TG_GPU_FAULT_ISR *isr, volatile PULONG gpu)
{
    ULONG pending = isr->PendingRestore & TG_RESTORE_GPU_ALL;
    if (!isr->GpuSaved && !pending)
        return TRUE;
    if (!gpu)
        return FALSE;
    if (pending & TG_RESTORE_GPU_ROLLBACK) {
        /* Unfinished arm_sync rollback: retry each unverified leaf/master mask
         * individually before restoring SavedPmcEnable (B09). */
        reg_write(gpu, GPU_PMC_INTR_EN_0, 0u);
        const ULONG en_probe = reg_read(gpu, GPU_PMC_INTR_EN_0);
        if (pending & TG_RESTORE_PMC_MASK) {
            reg_write(gpu, GPU_PMC_INTR_MSK_0, isr->SavedPmcMask);
            const ULONG msk_after = reg_read(gpu, GPU_PMC_INTR_MSK_0);
            /* NV_PMC_INTR_MSK_0 is a complete 32-bit bitmap whose physical GM20B
             * reset value is ~0u (0xffffffff); check PRI liveness via 0x140. */
            if (en_probe != MAXULONG && msk_after == isr->SavedPmcMask)
                pending &= ~TG_RESTORE_PMC_MASK;
        }
        if (pending & TG_RESTORE_FIFO_EN) {
            reg_write(gpu, GPU_PFIFO_INTR_EN_0, isr->SavedFifoEnable);
            const ULONG fifo_after = reg_read(gpu, GPU_PFIFO_INTR_EN_0);
            if (fifo_after != MAXULONG && fifo_after == isr->SavedFifoEnable)
                pending &= ~TG_RESTORE_FIFO_EN;
        }
        if (pending & TG_RESTORE_PBDMA_EN) {
            reg_write(gpu, GPU_PBDMA0_INTR_EN_0, isr->SavedPbdmaEnable);
            const ULONG pbdma_after = reg_read(gpu, GPU_PBDMA0_INTR_EN_0);
            if (pbdma_after != MAXULONG && pbdma_after == isr->SavedPbdmaEnable)
                pending &= ~TG_RESTORE_PBDMA_EN;
        }
        if (!(pending & (TG_RESTORE_PMC_MASK | TG_RESTORE_FIFO_EN | TG_RESTORE_PBDMA_EN)) &&
            (pending & TG_RESTORE_PMC_EN)) {
            if (isr->SavedPmcEnable)
                reg_write(gpu, GPU_PMC_INTR_EN_0, isr->SavedPmcEnable);
            const ULONG en_after = reg_read(gpu, GPU_PMC_INTR_EN_0);
            if (en_after != MAXULONG && (en_after & 3u) == (isr->SavedPmcEnable & 3u))
                pending &= ~TG_RESTORE_PMC_EN;
        }
        pending &= ~TG_RESTORE_PMC_DISABLE;
    } else {
        /* Normal disarm of an armed epoch: disable PMC stall output and verify. */
        reg_write(gpu, GPU_PMC_INTR_EN_0, 0u);
        const ULONG en_after = reg_read(gpu, GPU_PMC_INTR_EN_0);
        if (en_after != MAXULONG && (en_after & 3u) == 0u)
            pending &= ~TG_RESTORE_PMC_DISABLE;
        else
            pending |= TG_RESTORE_PMC_DISABLE;
    }
    isr->PendingRestore = (isr->PendingRestore & ~TG_RESTORE_GPU_ALL) | pending;
    if (pending == 0u) {
        isr->GpuSaved = FALSE;
        return TRUE;
    }
    isr->GpuSaved = TRUE;
    return FALSE;
}

typedef struct _ISR_SYNC_ARGS {
    TG_GPU_FAULT_ISR *Isr;
    PVOID GpuRegisters;
    PVOID LicRegisters;
    BOOLEAN GpuPowered;
    BOOLEAN WasArmed;
    BOOLEAN HadFault;
    BOOLEAN Ok;
} ISR_SYNC_ARGS;

static BOOLEAN arm_sync(PVOID context)
{
    ISR_SYNC_ARGS *args = context;
    TG_GPU_FAULT_ISR *isr = args->Isr;
    volatile PULONG gpu = (volatile PULONG)args->GpuRegisters;
    volatile PULONG lic = (volatile PULONG)args->LicRegisters;

    InterlockedExchange(&isr->Armed, 0);
    InterlockedExchange(&isr->ActiveEpoch, 0);
    InterlockedExchange(&isr->Faulted, 0);
    InterlockedExchange(&isr->GpuUnreachable, 0);
    InterlockedExchange(&isr->DpcPending, 0);
    InterlockedExchange(&isr->FaultEpoch, 0);

    /* Validate GPU and LIC apertures before touching any shared LIC or GPU mask.
     * Physical GM20B reset leaves NV_PMC_INTR_MSK_0 (0x640) at ~0u (0xffffffff),
     * and LIC_IER is also a complete 32-bit bitmap; validate liveness via
     * LIC_FIR / LIC_CLASS and GPU 0x140 / 0x2140 / 0x4010c instead. */
    const ULONG lic_fir_before = reg_read(lic, LIC_FIR_OFFSET);
    const ULONG lic_ier_before = reg_read(lic, LIC_IER_OFFSET);
    const ULONG lic_cls_before = reg_read(lic, LIC_CLASS_OFFSET);
    const ULONG fifo_en = reg_read(gpu, GPU_PFIFO_INTR_EN_0);
    const ULONG pbdma_en = reg_read(gpu, GPU_PBDMA0_INTR_EN_0);
    const ULONG pmc_msk = reg_read(gpu, GPU_PMC_INTR_MSK_0);
    const ULONG pmc_en = reg_read(gpu, GPU_PMC_INTR_EN_0);
    if (lic_fir_before == MAXULONG || lic_cls_before == MAXULONG ||
        fifo_en == MAXULONG || pbdma_en == MAXULONG || pmc_en == MAXULONG) {
        args->Ok = FALSE;
        return TRUE;
    }

    if (!isr->LicSaved) {
        isr->SavedLicEnable = lic_ier_before;
        isr->SavedLicClass = lic_cls_before;
        isr->LicSaved = TRUE;
    }
    if (!isr->GpuSaved) {
        isr->SavedFifoEnable = fifo_en;
        isr->SavedPbdmaEnable = pbdma_en;
        isr->SavedPmcMask = pmc_msk;
        isr->SavedPmcEnable = pmc_en;
        isr->GpuSaved = TRUE;
    }

    /* Disable stall output while configuring masks. */
    reg_write(gpu, GPU_PMC_INTR_EN_0, 0u);
    (VOID)reg_read(gpu, GPU_PMC_INTR_EN_0);

    /* Keep only true FIFO fault bits on the stall tree (drop RUNLIST_EVENT and
     * lone PBDMA ENGINE_EVENT notifications so normal submits never storm).
     */
    const ULONG want_pbdma_en = pbdma_en & ~PBDMA_ENGINE_EVENT;
    if (pbdma_en & PBDMA_ENGINE_EVENT) {
        reg_write(gpu, GPU_PBDMA0_INTR_EN_0, want_pbdma_en);
        if (reg_read(gpu, GPU_PBDMA0_INTR_0) == PBDMA_ENGINE_EVENT &&
            !reg_read(gpu, GPU_PBDMA0_INTR_1) && !reg_read(gpu, GPU_PFIFO_MMU_FAULT_ID)) {
            reg_write(gpu, GPU_PBDMA0_INTR_0, PBDMA_ENGINE_EVENT);
            reg_write(gpu, GPU_PFIFO_INTR_0, PFIFO_PBDMA_SUMMARY);
        }
    }
    const ULONG want_fifo_en = (fifo_en & ~PFIFO_RUNLIST_EVENT) | TG_GPU_FIFO_ERROR_MASK;
    reg_write(gpu, GPU_PFIFO_INTR_0, PFIFO_RUNLIST_EVENT);
    reg_write(gpu, GPU_PFIFO_INTR_EN_0, want_fifo_en);
    reg_write(gpu, GPU_PMC_INTR_MSK_0, TG_GPU_MC_STALL_MASK);

    /* Route GIC SPI 157 (LIC bank 4 bit 29) as IRQ and enable it. */
    reg_write(lic, LIC_CLASS_OFFSET, lic_cls_before & ~T214_GPU_LIC_STALL);
    reg_write(lic, LIC_FIR_CLR_OFFSET, T214_GPU_LIC_STALL);
    reg_write(lic, LIC_IER_SET_OFFSET, T214_GPU_LIC_STALL);
    reg_write(gpu, GPU_PMC_INTR_EN_0, 1u);

    /* Strict readback verification of leaf enables, master mask/enable and LIC. */
    const ULONG fifo_after = reg_read(gpu, GPU_PFIFO_INTR_EN_0);
    const ULONG pbdma_after = reg_read(gpu, GPU_PBDMA0_INTR_EN_0);
    const ULONG msk_after = reg_read(gpu, GPU_PMC_INTR_MSK_0);
    const ULONG en_after = reg_read(gpu, GPU_PMC_INTR_EN_0);
    const ULONG lic_fir_after = reg_read(lic, LIC_FIR_OFFSET);
    const ULONG lic_ier_after = reg_read(lic, LIC_IER_OFFSET);
    const ULONG lic_cls_after = reg_read(lic, LIC_CLASS_OFFSET);
    if (fifo_after == MAXULONG || (fifo_after & (TG_GPU_FIFO_ERROR_MASK | PFIFO_RUNLIST_EVENT)) != TG_GPU_FIFO_ERROR_MASK ||
        pbdma_after == MAXULONG || (pbdma_after & PBDMA_ENGINE_EVENT) != 0u ||
        msk_after != TG_GPU_MC_STALL_MASK || (en_after & 3u) != 1u ||
        (lic_fir_after & T214_GPU_LIC_STALL) != 0u || !(lic_ier_after & T214_GPU_LIC_STALL) ||
        lic_cls_after == MAXULONG || (lic_cls_after & T214_GPU_LIC_STALL)) {
        /* Reverse-order rollback of all registers modified above (B09).
         * Track each unverified leaf/master/LIC register in PendingRestore so a
         * subsequent DisarmFaultIsr retry restores the exact saved masks rather
         * than merely clearing PMC_INTR_EN_0. */
        isr->PendingRestore = TG_RESTORE_LIC_ALL | TG_RESTORE_GPU_ROLLBACK;
        const BOOLEAN lic_ok = restore_lic_pending(isr, lic);
        const BOOLEAN gpu_ok = restore_gpu_pending(isr, gpu);
        isr->GpuRegs = gpu_ok ? NULL : gpu;
        isr->LicRegs = lic_ok ? NULL : lic;
        args->Ok = FALSE;
        return TRUE;
    }

    isr->PendingRestore = TG_RESTORE_LIC_ALL | TG_RESTORE_PMC_DISABLE;
    LONG epoch = InterlockedIncrement(&isr->Epoch);
    if (!epoch)
        epoch = InterlockedIncrement(&isr->Epoch);
    TG_GPU_FAULT_SLOT *slot = &isr->Slots[(ULONG)epoch & (TG_GPU_FAULT_SLOTS - 1u)];
    RtlZeroMemory(slot, sizeof(*slot));
    slot->SlotEpoch = epoch;
    isr->GpuRegs = gpu;
    isr->LicRegs = lic;
    TgGpuDataSyncBarrier();
    InterlockedExchange(&isr->ActiveEpoch, epoch);
    InterlockedExchange(&isr->Armed, 1);
    args->Ok = TRUE;
    return TRUE;
}

static BOOLEAN disarm_sync(PVOID context)
{
    ISR_SYNC_ARGS *args = context;
    TG_GPU_FAULT_ISR *isr = args->Isr;
    volatile PULONG gpu = isr->GpuRegs;
    volatile PULONG lic = isr->LicRegs;
    BOOLEAN gpu_ok = TRUE;

    args->WasArmed = (InterlockedCompareExchange(&isr->Armed, 0, 0) != 0);
    args->HadFault = (InterlockedCompareExchange(&isr->Faulted, 0, 0) != 0);
    InterlockedExchange(&isr->Armed, 0);
    InterlockedExchange(&isr->ActiveEpoch, 0);
    InterlockedExchange(&isr->DpcPending, 0);

    /* Do NOT re-probe GPU PMC if a prior ISR already observed 0xffffffff (B03).
     * For a previously reachable powered GPU, retry any unfinished arm_sync
     * rollback or normal PMC disable and require valid readbacks (B09). */
    const BOOLEAN gpu_unreachable = (InterlockedCompareExchange(&isr->GpuUnreachable, 0, 0) != 0);
    if (!args->GpuPowered || gpu_unreachable) {
        /* Cold hardware reset or power-off has superseded GPU mask restore. */
        isr->PendingRestore &= ~TG_RESTORE_GPU_ALL;
        isr->GpuSaved = FALSE;
    } else if (gpu || isr->GpuSaved || (isr->PendingRestore & TG_RESTORE_GPU_ALL)) {
        gpu_ok = restore_gpu_pending(isr, gpu);
    }
    const BOOLEAN lic_ok = restore_lic_pending(isr, lic);

    /* Advance epoch so any DPC already queued from the retiring epoch drops. */
    LONG epoch = InterlockedIncrement(&isr->Epoch);
    if (!epoch)
        (VOID)InterlockedIncrement(&isr->Epoch);
    if (gpu_ok)
        isr->GpuRegs = NULL;
    if (lic_ok)
        isr->LicRegs = NULL;
    args->Ok = gpu_ok && lic_ok && (isr->PendingRestore == 0u);
    return TRUE;
}

NTSTATUS TgGpuArmFaultIsr(TG_GPU_FAULT_ISR *Isr, const DXGKRNL_INTERFACE *Dxgk,
                          PVOID GpuRegisters, PVOID LicRegisters)
{
    BOOLEAN invoked = FALSE;
    ISR_SYNC_ARGS args = {Isr, GpuRegisters, LicRegisters, TRUE, FALSE, FALSE, FALSE};
    if (!Isr || !Dxgk || !Dxgk->DeviceHandle || !Dxgk->DxgkCbSynchronizeExecution ||
        !GpuRegisters || !LicRegisters)
        return STATUS_INVALID_PARAMETER;
    NTSTATUS status = Dxgk->DxgkCbSynchronizeExecution(Dxgk->DeviceHandle, arm_sync, &args, 0, &invoked);
    if (!NT_SUCCESS(status))
        return status;
    return invoked && args.Ok ? STATUS_SUCCESS : STATUS_DEVICE_DATA_ERROR;
}

NTSTATUS TgGpuDisarmFaultIsrEx(TG_GPU_FAULT_ISR *Isr, const DXGKRNL_INTERFACE *Dxgk,
                               BOOLEAN GpuPowered, BOOLEAN *WasArmed, BOOLEAN *HadFault)
{
    BOOLEAN invoked = FALSE;
    ISR_SYNC_ARGS args = {Isr, NULL, NULL, GpuPowered, FALSE, FALSE, FALSE};
    if (WasArmed) *WasArmed = FALSE;
    if (HadFault) *HadFault = FALSE;
    if (!Isr)
        return STATUS_INVALID_PARAMETER;
    if (!Isr->GpuRegs && !Isr->LicRegs && !InterlockedCompareExchange(&Isr->Armed, 0, 0) &&
        !Isr->LicSaved && !Isr->GpuSaved && !Isr->PendingRestore) {
        if (HadFault) *HadFault = TgGpuIsFaultLatched(Isr);
        return STATUS_SUCCESS;
    }
    if (!Dxgk || !Dxgk->DeviceHandle || !Dxgk->DxgkCbSynchronizeExecution)
        return STATUS_INVALID_DEVICE_STATE;
    NTSTATUS status = Dxgk->DxgkCbSynchronizeExecution(Dxgk->DeviceHandle, disarm_sync, &args, 0, &invoked);
    if (WasArmed) *WasArmed = args.WasArmed;
    if (HadFault) *HadFault = args.HadFault;
    if (!NT_SUCCESS(status))
        return status;
    return invoked && args.Ok ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

NTSTATUS TgGpuDisarmFaultIsr(TG_GPU_FAULT_ISR *Isr, const DXGKRNL_INTERFACE *Dxgk,
                             BOOLEAN GpuPowered)
{
    return TgGpuDisarmFaultIsrEx(Isr, Dxgk, GpuPowered, NULL, NULL);
}

BOOLEAN TgGpuServiceFaultIsr(TG_GPU_FAULT_ISR *Isr, const DXGKRNL_INTERFACE *Dxgk)
{
    if (!Isr || !InterlockedCompareExchange(&Isr->Armed, 0, 0))
        return FALSE;

    volatile PULONG gpu = Isr->GpuRegs;
    volatile PULONG lic = Isr->LicRegs;
    if (!gpu || !lic)
        return FALSE;

    /* Resident breadcrumb BEFORE the first GPU PRI read (B03/N3): if the PRI
     * read hangs on AXI/MSELECT, IsrProbeEnterCount == IsrProbeDoneCount + 1. */
    (VOID)InterlockedIncrement(&Isr->IsrProbeEnterCount);
    const ULONG mc_intr0 = reg_read(gpu, GPU_PMC_INTR_0);
    (VOID)InterlockedIncrement(&Isr->IsrProbeDoneCount);

    const BOOLEAN unreachable = (mc_intr0 == MAXULONG);
    if (!unreachable && !(mc_intr0 & TG_GPU_MC_STALL_MASK))
        return FALSE;

    /* Latch software Faulted BEFORE masking hardware so concurrent Worker paths
     * observe the fault immediately even if the mask write stalls. */
    const LONG epoch = InterlockedCompareExchange(&Isr->Epoch, 0, 0);
    const ULONG enter = (ULONG)InterlockedIncrement(&Isr->IsrEnterCount);
    if (unreachable) {
        InterlockedExchange(&Isr->GpuUnreachable, 1);
        (VOID)InterlockedIncrement(&Isr->IsrUnreachableCount);
    }
    InterlockedExchange(&Isr->Armed, 0);
    InterlockedExchange(&Isr->ActiveEpoch, 0);
    InterlockedExchange(&Isr->FaultEpoch, epoch);
    InterlockedExchange(&Isr->Faulted, 1);

    /* Minimal top-half mask-only: disable level-triggered stall IRQ line. */
    if (!unreachable) {
        reg_write(gpu, GPU_PMC_INTR_EN_0, 0u);
        (VOID)reg_read(gpu, GPU_PMC_INTR_EN_0);
    }
    reg_write(lic, LIC_IER_CLR_OFFSET, T214_GPU_LIC_STALL);
    reg_write(lic, LIC_FIR_CLR_OFFSET, T214_GPU_LIC_STALL);
    (VOID)InterlockedIncrement(&Isr->IsrMaskCount);

    TG_GPU_FAULT_SLOT *slot = &Isr->Slots[(ULONG)epoch & (TG_GPU_FAULT_SLOTS - 1u)];
    slot->TopHalf = (TG_GPU_FAULT_ISR_EVENT){
        .Version = 2u,
        .Flags = unreachable ? TgGpuFaultFlagUnreachable : TgGpuFaultFlagStallIrq,
        .Epoch = (ULONG)epoch,
        .McIntr0 = mc_intr0,
        .FaultId = MAXULONG,
        .IsrCount = enter,
        .DpcCount = (ULONG)InterlockedCompareExchange(&Isr->DpcCount, 0, 0),
        .Phase = TgGpuFaultPhaseTopHalf,
    };
    TgGpuDataSyncBarrier();
    InterlockedExchange(&Isr->DpcPending, 1);
    if (Dxgk && Dxgk->DxgkCbQueueDpc)
        (VOID)Dxgk->DxgkCbQueueDpc(Dxgk->DeviceHandle);
    return TRUE;
}

BOOLEAN TgGpuIsFaultLatched(const TG_GPU_FAULT_ISR *Isr)
{
    if (!Isr)
        return FALSE;
    return InterlockedCompareExchange((volatile LONG *)&Isr->Faulted, 0, 0) != 0;
}

LONG TgGpuCurrentFaultEpoch(const TG_GPU_FAULT_ISR *Isr)
{
    if (!Isr)
        return 0;
    return InterlockedCompareExchange((volatile LONG *)&Isr->Epoch, 0, 0);
}

BOOLEAN TgGpuFaultEpochMatches(const TG_GPU_FAULT_ISR *Isr, LONG ExpectedEpoch)
{
    if (!Isr || !ExpectedEpoch)
        return FALSE;
    return InterlockedCompareExchange((volatile LONG *)&Isr->Epoch, 0, 0) == ExpectedEpoch;
}

BOOLEAN TgGpuFaultEpochHealthy(const TG_GPU_FAULT_ISR *Isr, LONG ExpectedEpoch)
{
    if (!Isr)
        return TRUE;
    if (TgGpuIsFaultLatched(Isr))
        return FALSE;
    const LONG cur = TgGpuCurrentFaultEpoch(Isr);
    const LONG active = InterlockedCompareExchange((volatile LONG *)&Isr->ActiveEpoch, 0, 0);
    /* Allow pure offline unit-test engines that have never armed FaultIsr. */
    if (cur == 0 && active == 0 && ExpectedEpoch == 0)
        return TRUE;
    /* Once FaultIsr has ever been armed, ONLY an active running epoch (never a
     * disarmed/retiring window) may admit or commit hardware work (AB06). */
    if (active == 0 || cur != active)
        return FALSE;
    if (ExpectedEpoch && ExpectedEpoch != active)
        return FALSE;
    return TRUE;
}

BOOLEAN TgGpuConsumeFaultDpc(TG_GPU_FAULT_ISR *Isr, TG_GPU_FAULT_ISR_EVENT *Event)
{
    if (!Isr || !Event)
        return FALSE;
    if (InterlockedCompareExchange(&Isr->DpcPending, 0, 1) != 1)
        return FALSE;
    TgGpuDataSyncBarrier();
    const LONG fault_epoch = InterlockedCompareExchange(&Isr->FaultEpoch, 0, 0);
    const LONG before_epoch = InterlockedCompareExchange(&Isr->Epoch, 0, 0);
    if (!fault_epoch || fault_epoch != before_epoch)
        return FALSE;

    const TG_GPU_FAULT_SLOT *slot = &Isr->Slots[(ULONG)fault_epoch & (TG_GPU_FAULT_SLOTS - 1u)];
    TG_GPU_FAULT_ISR_EVENT copy = slot->TopHalf;
    TgGpuDataSyncBarrier();
    /* Re-verify epoch AFTER copying the immutable TopHalf slot (B07). */
    if (InterlockedCompareExchange(&Isr->Epoch, 0, 0) != fault_epoch ||
        slot->SlotEpoch != fault_epoch || copy.Epoch != (ULONG)fault_epoch)
        return FALSE;

    copy.DpcCount = (ULONG)InterlockedIncrement(&Isr->DpcCount);
    *Event = copy;
    return TRUE;
}

BOOLEAN TgGpuReadFaultIsr(const TG_GPU_FAULT_ISR *Isr, struct tg_gpu_fault_status *Faults)
{
    if (!TgGpuIsFaultLatched(Isr))
        return FALSE;
    TgGpuDataSyncBarrier();
    const LONG fault_epoch = InterlockedCompareExchange((volatile LONG *)&Isr->FaultEpoch, 0, 0);
    if (Faults) {
        *Faults = (struct tg_gpu_fault_status){0};
        if (fault_epoch) {
            const TG_GPU_FAULT_SLOT *slot = &Isr->Slots[(ULONG)fault_epoch & (TG_GPU_FAULT_SLOTS - 1u)];
            if (slot->SlotEpoch == fault_epoch &&
                (slot->Details.Flags & TgGpuFaultFlagDetailsCaptured)) {
                Faults->fifo = slot->Details.FifoIntr;
                Faults->mmu = slot->Details.MmuFaultMask;
                Faults->pbdma = slot->Details.Pbdma0;
                Faults->pbdma1 = slot->Details.Pbdma1;
            }
        }
    }
    return TRUE;
}

BOOLEAN TgGpuCaptureFaultDetails(TG_GPU_FAULT_ISR *Isr, const struct tg_gpu_power_io *Io,
                                 const struct tg_gpu_power *Power,
                                 TG_GPU_FAULT_ISR_EVENT *Event)
{
    if (!Isr || !Io || !Io->read32 || !Power || !Power->owned || !Power->ready)
        return FALSE;
    if (!TgGpuIsFaultLatched(Isr))
        return FALSE;
    TgGpuDataSyncBarrier();
    const LONG fault_epoch = InterlockedCompareExchange(&Isr->FaultEpoch, 0, 0);
    if (!fault_epoch)
        return FALSE;
    TG_GPU_FAULT_SLOT *slot = &Isr->Slots[(ULONG)fault_epoch & (TG_GPU_FAULT_SLOTS - 1u)];
    if (slot->SlotEpoch != fault_epoch)
        return FALSE;
    if (slot->Details.Flags & TgGpuFaultFlagDetailsCaptured) {
        if (Event) *Event = slot->Details;
        return TRUE;
    }
    if (slot->TopHalf.Flags & TgGpuFaultFlagUnreachable) {
        slot->Details = slot->TopHalf;
        slot->Details.Phase = TgGpuFaultPhasePassiveCapture;
        if (Event) *Event = slot->Details;
        return TRUE;
    }

    const ULONG base = 0x57000000u;
    const ULONG mc_intr0 = Io->read32(Io->context, base + GPU_PMC_INTR_0);
    TG_GPU_FAULT_ISR_EVENT captured = slot->TopHalf;
    captured.Phase = TgGpuFaultPhasePassiveCapture;
    if (mc_intr0 == MAXULONG) {
        InterlockedExchange(&Isr->GpuUnreachable, 1);
        captured.Flags |= TgGpuFaultFlagUnreachable;
        slot->Details = captured;
        TgGpuDataSyncBarrier();
        if (Event) *Event = captured;
        return TRUE;
    }

    const ULONG fifo = Io->read32(Io->context, base + GPU_PFIFO_INTR_0);
    const ULONG mmu = Io->read32(Io->context, base + GPU_PFIFO_MMU_FAULT_ID);
    const ULONG pbdma0 = Io->read32(Io->context, base + GPU_PBDMA0_INTR_0);
    const ULONG pbdma1 = Io->read32(Io->context, base + GPU_PBDMA0_INTR_1);
    const ULONG bus = Io->read32(Io->context, base + GPU_PBUS_INTR_0);
    const ULONG pri0 = Io->read32(Io->context, base + GPU_PRIV_RING_STATUS0);
    const ULONG pri1 = Io->read32(Io->context, base + GPU_PRIV_RING_STATUS1);
    const ULONG pmc_en = Io->read32(Io->context, base + GPU_PMC_ENABLE);
    const ULONG gr_intr = (pmc_en != MAXULONG && (pmc_en & 0x1000u)) ?
        Io->read32(Io->context, base + GPU_PGRAPH_INTR) : 0u;

    /* B08: Capture ALL set MMU fault IDs (4 words each) into per-epoch slot. */
    ULONG fault_count = 0;
    slot->MmuEntryCount = 0;
    if (mmu && mmu != MAXULONG) {
        for (ULONG id = 0; id < TG_GPU_FAULT_MAX_MMU_IDS; ++id) {
            if (mmu & (1u << id)) {
                const ULONG unit = base + GPU_PFIFO_MMU_FAULT_BASE + id * 16u;
                TG_GPU_MMU_FAULT_ENTRY *entry = &slot->MmuEntries[fault_count];
                entry->Id = id;
                entry->Inst = Io->read32(Io->context, unit);
                entry->AddrLo = Io->read32(Io->context, unit + 4u);
                entry->AddrHi = Io->read32(Io->context, unit + 8u);
                entry->Info = Io->read32(Io->context, unit + 12u);
                ++fault_count;
            }
        }
        slot->MmuEntryCount = fault_count;
    }

    /* B08: Capture SYS (0x122120..12c on STATUS0 bit 8) and GPC0 (0x128120..12c
     * on STATUS1 bit 0) ringstation error blocks independently per source bit. */
    slot->PrivValidMask = 0;
    if (pri0 != MAXULONG && (pri0 & 0x100u)) {
        const ULONG s0 = Io->read32(Io->context, base + GPU_PRIV_SYS_ERR_ADR);
        const ULONG s1 = Io->read32(Io->context, base + GPU_PRIV_SYS_ERR_WRDAT);
        const ULONG s2 = Io->read32(Io->context, base + GPU_PRIV_SYS_ERR_INFO);
        const ULONG s3 = Io->read32(Io->context, base + GPU_PRIV_SYS_ERR_CODE);
        if (s0 != MAXULONG && s1 != MAXULONG && s2 != MAXULONG && s3 != MAXULONG) {
            slot->PrivSysErr[0] = s0; slot->PrivSysErr[1] = s1;
            slot->PrivSysErr[2] = s2; slot->PrivSysErr[3] = s3;
            slot->PrivValidMask |= 1u;
        }
    }
    if (pri1 != MAXULONG && (pri1 & 0x1u)) {
        const ULONG g0 = Io->read32(Io->context, base + GPU_PRIV_GPC_ERR_ADR);
        const ULONG g1 = Io->read32(Io->context, base + GPU_PRIV_GPC_ERR_WRDAT);
        const ULONG g2 = Io->read32(Io->context, base + GPU_PRIV_GPC_ERR_INFO);
        const ULONG g3 = Io->read32(Io->context, base + GPU_PRIV_GPC_ERR_CODE);
        if (g0 != MAXULONG && g1 != MAXULONG && g2 != MAXULONG && g3 != MAXULONG) {
            slot->PrivGpcErr[0] = g0; slot->PrivGpcErr[1] = g1;
            slot->PrivGpcErr[2] = g2; slot->PrivGpcErr[3] = g3;
            slot->PrivValidMask |= 2u;
        }
    }

    /* B08: Reject 0xffffffff on PTIMER registers instead of marking them valid! */
    ULONG pri_save0 = 0, pri_save1 = 0, pri_fecs = 0;
    BOOLEAN pri_timeout_valid = FALSE, pri_fecs_valid = FALSE, pri_unreachable = FALSE;
    if (bus != MAXULONG && (bus & 0xeu)) {
        pri_save0 = Io->read32(Io->context, base + GPU_PTIMER_PRI_SAVE0);
        pri_save1 = Io->read32(Io->context, base + GPU_PTIMER_PRI_SAVE1);
        if (pri_save0 == MAXULONG || pri_save1 == MAXULONG) {
            pri_unreachable = TRUE;
        } else {
            pri_timeout_valid = TRUE;
            if (pri_save0 & 0x80000000u) {
                pri_fecs = Io->read32(Io->context, base + GPU_PTIMER_PRI_FECS);
                if (pri_fecs == MAXULONG)
                    pri_unreachable = TRUE;
                else
                    pri_fecs_valid = TRUE;
            }
        }
    }

    ULONG flags = captured.Flags | TgGpuFaultFlagDetailsCaptured;
    if (fault_count > 1u) flags |= TgGpuFaultFlagMultiMmu;
    if (pri_timeout_valid) flags |= TgGpuFaultFlagPriTimeout;
    if (pri_fecs_valid) flags |= TgGpuFaultFlagFecsError;
    if (pri_unreachable) flags |= TgGpuFaultFlagUnreachable;

    captured.Flags = flags;
    if (!captured.McIntr0) captured.McIntr0 = mc_intr0;
    captured.FifoIntr = fifo;
    captured.MmuFaultMask = mmu;
    captured.MmuFaultCount = fault_count;
    if (fault_count > 0u) {
        captured.FaultId = slot->MmuEntries[0].Id;
        captured.FaultInst = slot->MmuEntries[0].Inst;
        captured.FaultAddrLo = slot->MmuEntries[0].AddrLo;
        captured.FaultAddrHi = slot->MmuEntries[0].AddrHi;
        captured.FaultInfo = slot->MmuEntries[0].Info;
    } else {
        captured.FaultId = MAXULONG;
    }
    captured.Pbdma0 = pbdma0;
    captured.Pbdma1 = pbdma1;
    captured.BusIntr = bus;
    captured.PriStatus0 = pri0;
    captured.PriStatus1 = pri1;
    captured.GrIntr = gr_intr;
    captured.PriSave0 = pri_save0;
    captured.PriSave1 = pri_save1;
    captured.PriFecs = pri_fecs;
    captured.DpcCount = (ULONG)InterlockedCompareExchange(&Isr->DpcCount, 0, 0);

    /* Publish into separate Details slot atomically (never mutating TopHalf). */
    slot->Details = captured;
    (VOID)InterlockedIncrement(&Isr->CaptureCount);
    TgGpuDataSyncBarrier();
    if (Event) *Event = captured;
    return TRUE;
}

ULONG TgGpuGetCapturedMmuEntryCount(const TG_GPU_FAULT_ISR *Isr, LONG FaultEpoch)
{
    if (!Isr || !FaultEpoch)
        return 0u;
    const TG_GPU_FAULT_SLOT *slot = &Isr->Slots[(ULONG)FaultEpoch & (TG_GPU_FAULT_SLOTS - 1u)];
    return (slot->SlotEpoch == FaultEpoch && (slot->Details.Flags & TgGpuFaultFlagDetailsCaptured)) ?
        slot->MmuEntryCount : 0u;
}

BOOLEAN TgGpuGetCapturedMmuEntry(const TG_GPU_FAULT_ISR *Isr, LONG FaultEpoch,
                                 ULONG Index, TG_GPU_MMU_FAULT_ENTRY *Entry)
{
    if (!Isr || !FaultEpoch || !Entry)
        return FALSE;
    const TG_GPU_FAULT_SLOT *slot = &Isr->Slots[(ULONG)FaultEpoch & (TG_GPU_FAULT_SLOTS - 1u)];
    if (slot->SlotEpoch != FaultEpoch || !(slot->Details.Flags & TgGpuFaultFlagDetailsCaptured) ||
        Index >= slot->MmuEntryCount)
        return FALSE;
    *Entry = slot->MmuEntries[Index];
    return TRUE;
}

BOOLEAN TgGpuGetCapturedPrivRing(const TG_GPU_FAULT_ISR *Isr, LONG FaultEpoch,
                                 ULONG *ValidMask, ULONG SysErr[4], ULONG GpcErr[4])
{
    if (ValidMask) *ValidMask = 0u;
    if (!Isr || !FaultEpoch)
        return FALSE;
    const TG_GPU_FAULT_SLOT *slot = &Isr->Slots[(ULONG)FaultEpoch & (TG_GPU_FAULT_SLOTS - 1u)];
    if (slot->SlotEpoch != FaultEpoch || !(slot->Details.Flags & TgGpuFaultFlagDetailsCaptured))
        return FALSE;
    if (ValidMask) *ValidMask = slot->PrivValidMask;
    if (SysErr) RtlCopyMemory(SysErr, slot->PrivSysErr, sizeof(slot->PrivSysErr));
    if (GpcErr) RtlCopyMemory(GpcErr, slot->PrivGpcErr, sizeof(slot->PrivGpcErr));
    return slot->PrivValidMask != 0u;
}

typedef struct _COMMIT_ACQUIRE_ARGS {
    const TG_GPU_FAULT_ISR *Isr;
    LONG ExpectedEpoch;
    BOOLEAN Permitted;
} COMMIT_ACQUIRE_ARGS;

static BOOLEAN dirql_acquire_commit_sync(PVOID context)
{
    COMMIT_ACQUIRE_ARGS *args = context;
    /* Pure resident memory check under DIRQL synchronization: ZERO MMIO or DSB
     * barrier on device registers inside the interrupt spinlock (V5-01). */
    args->Permitted = TgGpuFaultEpochHealthy(args->Isr, args->ExpectedEpoch);
    return TRUE;
}

static __forceinline VOID begin_commit_seq(volatile LONG *seq)
{
    if (!seq)
        return;
    const ULONG cur = (ULONG)InterlockedCompareExchange(seq, 0, 0);
    (VOID)InterlockedExchange(seq, (LONG)((cur & ~1u) + 1u));
}

static __forceinline VOID end_commit_seq(volatile LONG *seq)
{
    if (!seq)
        return;
    ULONG next = ((ULONG)InterlockedCompareExchange(seq, 0, 0) & ~1u) + 2u;
    if (!next)
        next = 2u;
    (VOID)InterlockedExchange(seq, (LONG)next);
}

BOOLEAN TgGpuFaultIsrAcquireCommit(const TG_GPU_FAULT_ISR *Isr,
                                   const DXGKRNL_INTERFACE *Dxgk,
                                   LONG ExpectedEpoch,
                                   volatile LONG *DiagCommitSeq,
                                   volatile LONG *DiagPermitCount,
                                   volatile LONG *DiagCommitStep,
                                   volatile LONG *DiagLastMmioReg)
{
    BOOLEAN permitted = FALSE;
    const BOOLEAN ever_armed = Isr &&
        (TgGpuCurrentFaultEpoch(Isr) != 0 ||
         InterlockedCompareExchange((volatile LONG *)&Isr->ActiveEpoch, 0, 0) != 0 ||
         TgGpuIsFaultLatched(Isr));
    if (ever_armed) {
        if (Dxgk && Dxgk->DeviceHandle && Dxgk->DxgkCbSynchronizeExecution) {
            BOOLEAN invoked = FALSE;
            COMMIT_ACQUIRE_ARGS args = {Isr, ExpectedEpoch, FALSE};
            NTSTATUS status = Dxgk->DxgkCbSynchronizeExecution(
                Dxgk->DeviceHandle, dirql_acquire_commit_sync, &args, 0, &invoked);
            permitted = NT_SUCCESS(status) && invoked && args.Permitted;
        }
    } else if (!Isr || (ExpectedEpoch == 0 && TgGpuFaultEpochHealthy(Isr, 0))) {
        permitted = TRUE;
    }

    if (permitted) {
        begin_commit_seq(DiagCommitSeq);
        if (DiagLastMmioReg)
            InterlockedExchange(DiagLastMmioReg, 0);
        if (DiagCommitStep)
            InterlockedExchange(DiagCommitStep, 1);
        if (DiagPermitCount)
            (VOID)InterlockedIncrement(DiagPermitCount);
        end_commit_seq(DiagCommitSeq);
    }
    return permitted;
}

VOID TgGpuFaultIsrTrackedWrite32(const struct tg_gpu_power_io *Io,
                                 ULONG Reg, ULONG Val, BOOLEAN FinalStep,
                                 volatile LONG *DiagCommitSeq,
                                 volatile LONG *DiagLastMmioReg,
                                 volatile LONG *DiagMmioEnterCount,
                                 volatile LONG *DiagMmioDoneCount,
                                 volatile LONG *DiagCommitStep)
{
    if (!Io || !Io->write32)
        return;
    /* Publish coherent pre-write (mmio_in_flight) and post-write phase tuples
     * without holding an odd sequence across the hardware access itself (N3). */
    begin_commit_seq(DiagCommitSeq);
    if (DiagLastMmioReg)
        InterlockedExchange(DiagLastMmioReg, (LONG)Reg);
    if (DiagMmioEnterCount)
        (VOID)InterlockedIncrement(DiagMmioEnterCount);
    end_commit_seq(DiagCommitSeq);

    Io->write32(Io->context, Reg, Val);

    begin_commit_seq(DiagCommitSeq);
    if (DiagCommitStep)
        InterlockedExchange(DiagCommitStep, FinalStep ? 3 : 2);
    if (DiagMmioDoneCount)
        (VOID)InterlockedIncrement(DiagMmioDoneCount);
    end_commit_seq(DiagCommitSeq);
}
