/* SPDX-License-Identifier: MIT */
#include "reset_backend.h"

static NTSTATUS isolate_backend(TG_GPU_CONTEXT_ENGINE *e,TG_GPU_WORK_QUEUE *q,
    TG_GPU_HARDWARE *h,TG_GPU_WORKER *w,BOOLEAN reset)
{
    KIRQL irql,engine_irql;NTSTATUS status;LIST_ENTRY *link;
    PAGED_CODE();
    if(!e || !q || !h || !w || !e->Commands.Handle ||
       (h->Engine && h->Engine!=e) || (q->Engine && q->Engine!=e) ||
       (w->Thread && (w->Queue!=q || w->Hardware!=h)))return STATUS_INVALID_PARAMETER;
    KeAcquireSpinLock(&q->Lock,&irql);
    if(!reset && q->Engine && (q->Current.Engine || !IsListEmpty(&q->Pending))) {
        KeReleaseSpinLock(&q->Lock,irql);
        return STATUS_DEVICE_BUSY;
    }
    q->Paused=TRUE;
    KeAcquireSpinLock(&e->Lock,&engine_irql);e->Resetting=TRUE;
    if(reset && !e->RecoveryPending){e->RecoveryPending=TRUE;e->RecoveryStatus=STATUS_IO_TIMEOUT;}
    KeReleaseSpinLock(&e->Lock,engine_irql);KeReleaseSpinLock(&q->Lock,irql);
    /* Never join while holding DispatchGate: a waking worker may be waiting
     * for it. Resetting also suppresses a late worker recovery notification. */
    status=TgGpuStopWorker(w);if(!NT_SUCCESS(status))return status;
    if(q->Engine) {
        status=KeWaitForSingleObject(&q->DispatchGate,Executive,KernelMode,FALSE,NULL);
        if(status!=STATUS_SUCCESS)return status;
        if(reset)TgGpuTraceResetWork(q,&h->Io,&h->Power);
        status=TgGpuResetWorkQueue(q,&h->Io,&h->Power,&h->Clock);
        if(NT_SUCCESS(status))status=TgGpuStopWorkQueue(q);
        (VOID)KeReleaseMutex(&q->DispatchGate,FALSE);
        if(!NT_SUCCESS(status))return status;
    }
    if(!h->Engine)return h->Isolated?STATUS_SUCCESS:STATUS_DEVICE_NOT_READY;
    status=TgGpuIsolateHardware(h);if(!NT_SUCCESS(status))return status;
    KeAcquireSpinLock(&e->Lock,&irql);
    for(link=e->Contexts.Flink;link!=&e->Contexts;link=link->Flink) {
        TG_GPU_CONTEXT *c=CONTAINING_RECORD(link,TG_GPU_CONTEXT,Link);
        const BOOLEAN system_paging=c->Flags.SystemContext && c->Flags.VirtualAddressing &&
            !c->Flags.GdiContext && !c->Closing && !c->References &&
            c->Device && !c->Device->Closing && c->Device->Process &&
            !c->Device->Process->Closing && c->Device->Process->Flags.SystemProcess;
        if(reset && !system_paging) {
            /* VidMm can evict the roots during TDR cleanup. */
            c->Root.Valid=FALSE;c->RootStatus=STATUS_DEVICE_NOT_READY;
        } else {
            /* The system paging device survives adapter recovery. Its root
             * is the authoritative OS binding, updated by SetRootPageTable
             * when moved/resized; never revive a failed binding from a saved
             * descriptor. Resetting/RecoveryPending still block execution
             * through OS cleanup and cold startup. Only its private image
             * must be rebuilt from the new hardware generation's template.
             * Suspend retains its existing paging-only behavior. */
            c->GraphicsGeneration=0;
        }
    }
    KeReleaseSpinLock(&e->Lock,irql);return STATUS_SUCCESS;
}

NTSTATUS TgGpuResetBackend(TG_GPU_CONTEXT_ENGINE *e,TG_GPU_WORK_QUEUE *q,
    TG_GPU_HARDWARE *h,TG_GPU_WORKER *w)
{
    TgGpuTraceExecutionState(e,TgGpuExecutionResetBegin,STATUS_PENDING,0,NULL);
    NTSTATUS status=isolate_backend(e,q,h,w,TRUE);
    TgGpuTraceExecutionState(e,TgGpuExecutionResetEnd,status,0,NULL);
    return status;
}

NTSTATUS TgGpuSuspendBackend(TG_GPU_CONTEXT_ENGINE *e,TG_GPU_WORK_QUEUE *q,
    TG_GPU_HARDWARE *h,TG_GPU_WORKER *w)
{
    if (!e || !e->PagingOnly) return STATUS_NOT_SUPPORTED;
    return isolate_backend(e,q,h,w,FALSE);
}
NTSTATUS TgGpuRestartBackend(TG_GPU_CONTEXT_ENGINE *e,TG_GPU_WORK_QUEUE *q,
    TG_GPU_HARDWARE *h,TG_GPU_WORKER *w,const struct tg_gpu_power_io *io,
    UCHAR address,const struct tg_gpu_gr_payload *firmware)
{
    struct tg_gpu_power_io next_io;KIRQL irql,engine_irql;NTSTATUS status,cleanup;
    PAGED_CODE();
    if(!e || !q || !h || !w || !io || !firmware || !e->Resetting ||
       !h->Isolated || h->Power.owned || w->Thread || q->Engine ||
       (h->Engine && h->Engine!=e))return STATUS_INVALID_DEVICE_STATE;
    if(h->Status.restore_result)return STATUS_DEVICE_DATA_ERROR;
    next_io=*io; /* io may point into h, which cold StartHardware clears. */
    status=TgGpuStopHardware(h);if(!NT_SUCCESS(status))return status;
    status=TgGpuFreePrivateMemory(&e->Dxgk,&e->Graphics.Memory);
    if(!NT_SUCCESS(status))return status;
    e->Graphics.ImageBytes=0;
    /* Isolation and detached queue/worker are prerequisites for reusing the
     * private command backing. This never initializes an expected fence. */
    RtlZeroMemory(e->Commands.CpuAddress,e->Commands.Bytes);KeMemoryBarrier();
    RtlZeroMemory(&e->Channel,sizeof(e->Channel));
    status=TgGpuStartHardware(h,e,&next_io,address,firmware);
    if(!NT_SUCCESS(status))goto fail;
    status=TgGpuInitializeWorkQueue(q,e);if(!NT_SUCCESS(status))goto fail;
    status=KeWaitForSingleObject(&q->DispatchGate,Executive,KernelMode,FALSE,NULL);
    if(status!=STATUS_SUCCESS)goto fail;
    /* Normal submission takes this mutex; DISPATCH preemption is rejected
     * by RecoveryPending until the thread has actually been created. */
    status=TgGpuStartWorker(w,q,h);
    if(NT_SUCCESS(status)) {
        KeAcquireSpinLock(&q->Lock,&irql);KeAcquireSpinLock(&e->Lock,&engine_irql);
        e->RecoveryPending=e->RecoveryNotified=e->RecoverySending=e->Resetting=FALSE;
        e->RecoveryStatus=STATUS_SUCCESS;
        KeReleaseSpinLock(&e->Lock,engine_irql);KeReleaseSpinLock(&q->Lock,irql);
    }
    (VOID)KeReleaseMutex(&q->DispatchGate,FALSE);
    if(NT_SUCCESS(status))return status;
fail:
    cleanup=TgGpuResetBackend(e,q,h,w);
    return NT_SUCCESS(cleanup)?status:cleanup;
}
