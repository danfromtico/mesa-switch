/* SPDX-License-Identifier: MIT */
#include "context.h"
#include "paging_buffer.h"

struct paging_fault_guard {
    TG_GPU_CONTEXT_ENGINE *engine;
    LONG epoch;
};

static bool paging_abort_cb(void *ctx)
{
    const struct paging_fault_guard *g = ctx;
    return !TgGpuFaultEpochHealthy(&g->engine->FaultIsr, g->epoch);
}

static bool paging_acquire_cb(void *ctx)
{
    struct paging_fault_guard *g = ctx;
    TG_GPU_CONTEXT_ENGINE *e = g->engine;
    return TgGpuFaultIsrAcquireCommit(&e->FaultIsr, &e->Dxgk, g->epoch,
        &e->DiagCommitSeq, &e->DiagCommitPermitCount, &e->DiagCommitStep,
        &e->DiagLastMmioReg) != FALSE;
}

static void paging_write32_cb(void *ctx, const struct tg_gpu_power_io *io,
    uint32_t reg, uint32_t val, bool final_step)
{
    struct paging_fault_guard *g = ctx;
    TG_GPU_CONTEXT_ENGINE *e = g->engine;
    TgGpuFaultIsrTrackedWrite32(io, reg, val, final_step ? TRUE : FALSE,
        &e->DiagCommitSeq, &e->DiagLastMmioReg, &e->DiagMmioEnterCount,
        &e->DiagMmioDoneCount, &e->DiagCommitStep);
}

static BOOLEAN valid_range(const TG_GPU_CONTEXT_WORK *w,UINT64 address,UINT64 bytes)
{
    return address < w->Root.VaLimit && bytes <= w->Root.VaLimit - address &&
        (address + bytes <= w->InternalVa || address >= w->InternalVa + TG_GPU_INTERNAL_COVERAGE);
}

static NTSTATUS start_dma(TG_GPU_CONTEXT_ENGINE *e,TG_GPU_CONTEXT_WORK *w,
    const TG_GPU_PAGING_DMA_STEP *step,const struct tg_gpu_power_io *io,const struct tg_gpu_power *power)
{
    const struct tg_gpu_paging_dma *op = &step->Operation;
    UINT32 packet[TG_GPU_PAGING_DMA_MAX_WORDS];
    if (!w->Context || !valid_range(w,op->destination,op->bytes) ||
        (!op->fill && !valid_range(w,op->source,op->bytes)))
        return STATUS_INVALID_ADDRESS;
    UINT words = tg_gpu_encode_paging_dma(packet,op);
    if (!words)
        return STATUS_INVALID_PARAMETER;

    w->PagingDmaBytes = (UINT)step->RecordBytes;
    w->DmaAddress = w->InternalVa + TG_GPU_COMMAND_CAPTURE;
    w->DmaBytes = words * sizeof(*packet);
    NTSTATUS status = TgGpuActivateContextWork(e,w,NULL);
    if (status != STATUS_SUCCESS) return status;
    status = TgGpuPrepareContextWork(e,w);
    if (status != STATUS_SUCCESS) return status;

    UCHAR *staging = (UCHAR *)e->Commands.CpuAddress + TG_GPU_COMMAND_CAPTURE;
    TgGpuDataSyncBarrier();
    RtlZeroMemory(staging,TG_GPU_COMMAND_CAPTURE_BYTES);
    RtlCopyMemory(staging,packet,w->DmaBytes);
    TgGpuDataSyncBarrier();
    return TgGpuSubmitContextWork(e,w,io,power);
}

static NTSTATUS finish_dma(TG_GPU_CONTEXT_ENGINE *e,TG_GPU_CONTEXT_WORK *w,
    const struct tg_gpu_power_io *io,const struct tg_gpu_power *power)
{
    NTSTATUS status = TgGpuObserveContextWork(e,w,io,power);
    if (status != STATUS_SUCCESS) return status;
    status = TgGpuQuiesceContextWork(e,w,io,power);
    if (status != STATUS_SUCCESS) return status;
    status = TgGpuObserveContextWork(e,w,io,power);
    if (status != STATUS_SUCCESS) return status;

    /* Observed GPU and CPU-visible markers after retirement, not a software
     * completion claim. At most one record per context/hardware generation. */
    TgGpuTracePagingCompletion(e,w);

    /* Only a retired and CPU-visible marker advances this sub-operation.
     * Keep the context reference and OS fence until the entire DMA buffer
     * completes, including subsequent PTE updates and TLB invalidations. */
    w->PagingCursor += w->PagingDmaBytes;
    w->PagingDmaBytes = 0;
    w->Expected = 0;
    w->Prepared = w->Submitted = w->Attempted = w->Retired = FALSE;
    w->GpuCompleted = w->CpuCompleted = FALSE;
    return STATUS_SUCCESS;
}

NTSTATUS TgGpuExecutePagingWork(TG_GPU_CONTEXT_ENGINE *e,TG_GPU_CONTEXT_WORK *w,
    const void *buffer,SIZE_T bytes,const struct tg_gpu_power_io *io,const struct tg_gpu_power *power)
{
    KIRQL irql;
    NTSTATUS status;
    if (!e || !w || w->Engine!=e || !w->Paging || !buffer || bytes!=w->PagingBytes)
        return STATUS_INVALID_PARAMETER;
    if (TgGpuIsFaultLatched(&e->FaultIsr))
        return STATUS_DEVICE_DATA_ERROR;

    KeAcquireSpinLock(&e->Lock,&irql);
    if (w->CompletionReady) {
        status = e->Notification==w ? STATUS_SUCCESS : STATUS_INVALID_DEVICE_STATE;
        KeReleaseSpinLock(&e->Lock,irql);
        return status;
    }
    if (!w->PagingStarted) {
        if (e->Active || e->Notification || e->Channel.referenced) {
            KeReleaseSpinLock(&e->Lock,irql);
            return STATUS_PENDING;
        }
        e->Active = w;
        w->PagingStarted = TRUE;
        w->FaultEpoch = TgGpuCurrentFaultEpoch(&e->FaultIsr);
        KeReleaseSpinLock(&e->Lock,irql);
        if (!TgGpuFaultEpochHealthy(&e->FaultIsr,w->FaultEpoch))
            return STATUS_DEVICE_DATA_ERROR;
        struct paging_fault_guard sync_guard_ctx = {.engine = e, .epoch = w->FaultEpoch};
        const struct tg_gpu_commit_guard sync_guard = {
            .abort = paging_abort_cb, .acquire = paging_acquire_cb,
            .write32 = paging_write32_cb, .context = &sync_guard_ctx
        };
        struct tg_gpu_frontend_status sync = {0};
        if (tg_gpu_frontend_sync_memory_checked(io,power,&sync,&sync_guard)!=TG_GPU_FE_OK)
            return STATUS_DEVICE_DATA_ERROR;
    } else {
        BOOLEAN active = e->Active==w;
        KeReleaseSpinLock(&e->Lock,irql);
        if (!active) return STATUS_INVALID_DEVICE_STATE;
    }

    if (w->PagingDmaBytes) {
        status = finish_dma(e,w,io,power);
        if (status != STATUS_SUCCESS) return status;
    }

    if (!TgGpuFaultEpochHealthy(&e->FaultIsr,w->FaultEpoch))
        return STATUS_DEVICE_DATA_ERROR;
    SIZE_T completed;
    TG_GPU_PAGING_DMA_STEP step = {0};
    struct paging_fault_guard guard_ctx = {.engine = e, .epoch = w->FaultEpoch};
    const struct tg_gpu_commit_guard guard = {
        .abort = paging_abort_cb, .acquire = paging_acquire_cb,
        .write32 = paging_write32_cb, .context = &guard_ctx
    };
    status = TgGpuExecutePagingBufferChecked(e->Tables,(const UCHAR *)buffer+w->PagingCursor,
        bytes-w->PagingCursor,TRUE,io,power,&completed,&step,
        &guard,&e->DiagLastPagingKind);
    w->PagingCursor += (UINT)completed;
    if (status != STATUS_SUCCESS) return status;
    if (!TgGpuFaultEpochHealthy(&e->FaultIsr,w->FaultEpoch))
        return STATUS_DEVICE_DATA_ERROR;
    if (step.RecordBytes) {
        status = start_dma(e,w,&step,io,power);
        return status==STATUS_SUCCESS ? STATUS_PENDING : status;
    }

    KeAcquireSpinLock(&e->Lock,&irql);
    if (w->Context) {
        --w->Context->References;
        w->Context = NULL;
    }
    e->Active = NULL;
    e->Notification = w;
    w->CompletionReady = TRUE;
    KeReleaseSpinLock(&e->Lock,irql);
    return STATUS_SUCCESS;
}
