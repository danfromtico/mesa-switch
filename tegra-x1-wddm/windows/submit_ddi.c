/* SPDX-License-Identifier: MIT */
#include "submit_ddi.h"
#include "work_queue.h"
#include "hardware.h"
#include "present.h"

static NTSTATUS finish_submit(TG_GPU_CONTEXT_ENGINE *e,
    const DXGKARG_SUBMITCOMMANDVIRTUAL *a,TG_GPU_WORK_QUEUE *q,NTSTATUS result)
{
    KIRQL irql;
    KeAcquireSpinLock(&e->Lock,&irql);
    /* A malformed DMA/private payload still carries a scheduler fence. Its
     * ordered rejection marker must retire after the preceding packets, and
     * an intervening adapter reset must account for that same fence too. */
    e->LastSubmittedFence=a->SubmissionFenceId;
    e->SubmittedFenceValid=TRUE;
    KeReleaseSpinLock(&e->Lock,irql);
    /* Publish the scheduler fence before releasing the submission gate;
     * a newer caller must not be overwritten by this older return path. */
    if(q)(VOID)KeReleaseMutex(&q->DispatchGate,FALSE);
    KeAcquireSpinLock(&e->Lock,&irql);--e->Creating;KeReleaseSpinLock(&e->Lock,irql);
    return result;
}

NTSTATUS APIENTRY TgGpuDdiSubmitCommandVirtual(HANDLE adapter,const DXGKARG_SUBMITCOMMANDVIRTUAL *a)
{
    TG_GPU_CONTEXT_ENGINE *e=adapter;TG_GPU_QUEUE_ENTRY *reserved;TG_GPU_HARDWARE *h;
    TG_GPU_WORK_QUEUE *q;TG_GPU_COMMAND_SNAPSHOT *snapshot=NULL;
    NTSTATUS status;BOOLEAN used=FALSE;KIRQL irql;
    PAGED_CODE();
    if(!e || !a || !e->Dxgk.DeviceHandle || a->NodeOrdinal || a->EngineOrdinal)
        return STATUS_INVALID_PARAMETER;
    KeAcquireSpinLock(&e->Lock,&irql);++e->Creating;q=e->QueueOwner;KeReleaseSpinLock(&e->Lock,irql);
    if(q) {
        status=KeWaitForSingleObject(&q->DispatchGate,Executive,KernelMode,FALSE,NULL);
        if(status!=STATUS_SUCCESS) {
            TgGpuTraceExecutionState(e,TgGpuExecutionSubmitFailure,status,0,a);
            (VOID)TgGpuRequestEngineRecoveryAt(e,NT_SUCCESS(status)?STATUS_DEVICE_BUSY:status,
                TgGpuRecoverySubmitGate,a);
            return finish_submit(e,a,NULL,STATUS_SUCCESS);
        }
    }
    /* Reserve before inspecting the submission. Allocation failure is an
     * internal admission failure, not a malformed-packet diagnosis. Accept
     * into recovery without touching commands or reporting completion. */
    reserved=ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(*reserved),TG_GPU_QUEUE_TAG);
    h=e->HardwareOwner;
    if(!reserved)status=STATUS_NO_MEMORY;
    else if(e->Resetting || e->RecoveryPending || !q || q->Engine!=e ||
       !h || h->Engine!=e || !h->Ready || !h->Power.owned || !h->Power.ready)
        status=STATUS_DEVICE_NOT_READY;
    else {
        BOOLEAN application=!a->Flags.Paging && !a->Flags.ContextSwitch &&
            !a->Flags.NullRendering && a->DmaBufferSize;
        status=STATUS_SUCCESS;
        if (application) {
            if (TgGpuIsGdiContext(e,a->hContext) || (a->Flags.Present && !e->PagingOnly)) {
                status=TgGpuCapturePresentSnapshot(e,a,&snapshot);
            } else if (!e->PagingOnly) {
                struct tg_gpu_command_va va;
                status=TgGpuGetCommandVa(e,a->hContext,&va);
                if (status==STATUS_SUCCESS)
                    status=TgGpuCaptureCommandSnapshot(a,&va,&snapshot);
            }
        }
        if(status==STATUS_SUCCESS || status==STATUS_INVALID_PARAMETER)
            status=TgGpuQueueSubmitOrReject(q,a,reserved,snapshot,&used);
    }
    /* Only a successful application admission owns the captured bytes. An
     * ordered rejection marker can own Reserved without owning Snapshot. */
    if(snapshot && status!=STATUS_SUCCESS)TgGpuFreeCommandSnapshot(snapshot);
    if(reserved && !used)ExFreePoolWithTag(reserved,TG_GPU_QUEUE_TAG);
    if(status!=STATUS_SUCCESS)TgGpuTraceExecutionState(e,TgGpuExecutionSubmitFailure,status,0,a);
    if(status==STATUS_INVALID_PARAMETER && used) {
        NTSTATUS drain=TgGpuDrainRejectedIdle(q);
        if(!NT_SUCCESS(drain))(VOID)TgGpuRequestEngineRecoveryAt(e,drain,TgGpuRecoveryRejectDrain,a);
    } else if(status!=STATUS_SUCCESS)(VOID)TgGpuRequestEngineRecoveryAt(e,status,TgGpuRecoverySubmit,a);
    /* Busy, OOM, lost root and transport failures must never escape this DDI:
     * Microsoft permits only SUCCESS or malformed INVALID_PARAMETER here. */
    return finish_submit(e,a,q,status==STATUS_INVALID_PARAMETER && used?
        STATUS_INVALID_PARAMETER:STATUS_SUCCESS);
}
NTSTATUS APIENTRY TgGpuDdiPreemptCommand(HANDLE adapter,const DXGKARG_PREEMPTCOMMAND *a)
{
    TG_GPU_CONTEXT_ENGINE *e=adapter;TG_GPU_WORK_QUEUE *q;TG_GPU_QUEUE_ENTRY *entry=NULL;
    KIRQL irql;NTSTATUS status;
    /* Nonpageable DISPATCH_LEVEL callback: no dispatcher mutex or GPU waits.
     * WDDM requires SUCCESS even on an internal failure; request recovery
     * instead of inventing a preempt completion or returning a bugcheck code. */
    if(!e)return STATUS_SUCCESS;
    KeAcquireSpinLock(&e->Lock,&irql);++e->Creating;q=e->QueueOwner;
    status=e->Stopping?STATUS_DEVICE_NOT_READY:STATUS_SUCCESS;KeReleaseSpinLock(&e->Lock,irql);
    if(!a || a->NodeOrdinal || a->EngineOrdinal || a->Flags.Value)status=STATUS_INVALID_PARAMETER;
    if(NT_SUCCESS(status)) {
        entry=ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(*entry),TG_GPU_QUEUE_TAG);
        status=entry?TgGpuQueuePreempt(q,a,entry):STATUS_NO_MEMORY;
        if(entry && !NT_SUCCESS(status))ExFreePoolWithTag(entry,TG_GPU_QUEUE_TAG);
    }
    TgGpuTraceExecutionState(e,TgGpuExecutionPreempt,status,a?a->PreemptionFenceId:0,NULL);
    if(!NT_SUCCESS(status))(VOID)TgGpuRequestEngineRecoveryAt(e,status,TgGpuRecoveryPreempt,NULL);
    KeAcquireSpinLock(&e->Lock,&irql);--e->Creating;KeReleaseSpinLock(&e->Lock,irql);
    return STATUS_SUCCESS;
}
void TgGpuRegisterSubmissionDdi(DRIVER_INITIALIZATION_DATA *d)
{
    d->DxgkDdiSubmitCommandVirtual=TgGpuDdiSubmitCommandVirtual;
    d->DxgkDdiPreemptCommand=TgGpuDdiPreemptCommand;
}

static NTSTATUS APIENTRY render_legacy(HANDLE context, DXGKARG_RENDER *args)
{
    PAGED_CODE();
    if (!context || !args)
        return STATUS_INVALID_PARAMETER;
    /* GM20B command submission uses VidMm GPU VAs and the virtual queue. */
    return STATUS_NOT_SUPPORTED;
}

static NTSTATUS APIENTRY patch_legacy(HANDLE adapter, const DXGKARG_PATCH *args)
{
    UNREFERENCED_PARAMETER(adapter);
    UNREFERENCED_PARAMETER(args);
    PAGED_CODE();
    /* The virtual path emits no physical relocations. SubmitCommand contains
     * any unexpected physical work before it can reach the hardware.
     */
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY submit_legacy(HANDLE adapter, const DXGKARG_SUBMITCOMMAND *args)
{
    TG_GPU_CONTEXT_ENGINE *e=adapter;KIRQL irql;
    /* This DISPATCH_LEVEL DDI has no failure return. An unexpected physical
     * submission enters the existing recovery path without retiring a fence.
     */
    if(e && args) {
        KeAcquireSpinLock(&e->Lock,&irql);
        e->LastSubmittedFence=args->SubmissionFenceId;e->SubmittedFenceValid=TRUE;
        KeReleaseSpinLock(&e->Lock,irql);
    }
    (VOID)TgGpuRequestEngineRecoveryAt(e,STATUS_NOT_SUPPORTED,TgGpuRecoveryLegacySubmit,NULL);
    return STATUS_SUCCESS;
}

void TgGpuRegisterDisplaySubmissionDdis(DRIVER_INITIALIZATION_DATA *d)
{
    d->DxgkDdiRender = render_legacy;
    d->DxgkDdiPatch = patch_legacy;
    d->DxgkDdiSubmitCommand = submit_legacy;
    d->DxgkDdiPresent = TgGpuDdiPresent;
}
