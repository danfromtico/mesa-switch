/* SPDX-License-Identifier: MIT */
#include "work_queue.h"

typedef struct _RECOVERY_NOTIFY_CTX {
    TG_GPU_CONTEXT_ENGINE *Engine;
    LONG FaultEpoch;
} RECOVERY_NOTIFY_CTX;

static BOOLEAN notify(PVOID context)
{
    const RECOVERY_NOTIFY_CTX *ctx=context;
    TG_GPU_CONTEXT_ENGINE *e=ctx->Engine;
    if(e->Stopping || e->Resetting ||
       (ctx->FaultEpoch && !TgGpuFaultEpochMatches(&e->FaultIsr,ctx->FaultEpoch)))
        return TRUE;
    DXGKARGCB_NOTIFY_INTERRUPT_DATA data={.InterruptType=DXGK_INTERRUPT_GPU_ENGINE_TIMEOUT};
    data.GpuEngineTimeout.NodeOrdinal=data.GpuEngineTimeout.EngineOrdinal=0;
    e->RecoveryNotified=TRUE;
    e->Dxgk.DxgkCbNotifyInterrupt(e->Dxgk.DeviceHandle,&data);
    (VOID)e->Dxgk.DxgkCbQueueDpc(e->Dxgk.DeviceHandle);return TRUE;
}
NTSTATUS TgGpuRequestEngineRecovery(TG_GPU_CONTEXT_ENGINE *e,NTSTATUS reason)
{
    return TgGpuRequestEngineRecoveryForEpoch(e,reason,TgGpuRecoveryUnspecified,NULL,0);
}
NTSTATUS TgGpuRequestEngineRecoveryAt(TG_GPU_CONTEXT_ENGINE *e,NTSTATUS reason,
    enum tg_gpu_recovery_source source,const DXGKARG_SUBMITCOMMANDVIRTUAL *submit)
{
    return TgGpuRequestEngineRecoveryForEpoch(e,reason,source,submit,0);
}
NTSTATUS TgGpuRequestEngineRecoveryForEpoch(TG_GPU_CONTEXT_ENGINE *e,NTSTATUS reason,
    enum tg_gpu_recovery_source source,const DXGKARG_SUBMITCOMMANDVIRTUAL *submit,LONG fault_epoch)
{
    TG_GPU_WORK_QUEUE *q;KIRQL irql,queue_irql;BOOLEAN invoked=FALSE,send=FALSE;NTSTATUS status;
    TG_GPU_RECOVERY_EVENT record={0};BOOLEAN first=FALSE;ULONG my_token=0;
    if(!e || NT_SUCCESS(reason) || !e->Dxgk.DeviceHandle || !e->Dxgk.DxgkCbSynchronizeExecution ||
       !e->Dxgk.DxgkCbNotifyInterrupt || !e->Dxgk.DxgkCbQueueDpc)return STATUS_INVALID_PARAMETER;
    /* Adapter/queue lifetime is serialized against stop by the caller. No
     * register access, completion fence, context release or recovery reset is
     * performed here; the actual WDDM TDR callbacks remain responsible.
     * Check Stopping/Resetting and fault_epoch BEFORE pausing the queue so a
     * stale DPC from a prior epoch can never pause a freshly restarted queue. */
    q=e->QueueOwner;
    if(q)KeAcquireSpinLock(&q->Lock,&queue_irql);
    KeAcquireSpinLock(&e->Lock,&irql);
    if(e->Stopping || e->Resetting ||
       (fault_epoch && !TgGpuFaultEpochMatches(&e->FaultIsr,fault_epoch)))
        status=STATUS_DEVICE_NOT_READY;
    else {
        if(q)q->Paused=TRUE;
        if(e->RecoveryNotified)status=STATUS_SUCCESS;
        else if(e->RecoverySending)status=STATUS_PENDING;
        else {
            if(!e->RecoveryPending){
                e->RecoveryPending=TRUE;e->RecoveryStatus=reason;first=TRUE;
                record=(TG_GPU_RECOVERY_EVENT){.Version=1,.Source=source,
                    .AdapterLuid=e->AdapterLuid,.Status=reason,
                    .LastCompletedFence=e->LastCompletedFence,.GraphicsGeneration=e->GraphicsGeneration,
                    .CachedFifo=e->Channel.fifo_interrupt,.CachedMmu=e->Channel.mmu_fault,
                    .CachedPbdma=e->Channel.pbdma_interrupt,.CachedPbdma1=e->Channel.pbdma_interrupt_1};
                if(submit){
                    record.Flags=submit->Flags.Value;record.Context=(UINT64)(ULONG_PTR)submit->hContext;
                    record.DmaBytes=submit->DmaBufferSize;record.PrivateBytes=submit->DmaBufferPrivateDataSize;
                    record.UmdPrivateBytes=submit->DmaBufferUmdPrivateDataSize;
                    record.SubmissionFence=submit->SubmissionFenceId;
                }
                TgGpuCacheRecovery(&e->DebugCache,&record);
            }
            my_token=++e->RecoverySendToken;
            if(!my_token)my_token=++e->RecoverySendToken;
            e->RecoveryActiveToken=my_token;
            e->RecoverySending=TRUE;send=TRUE;status=STATUS_SUCCESS;
        }
    }
    KeReleaseSpinLock(&e->Lock,irql);
    if(q)KeReleaseSpinLock(&q->Lock,queue_irql);
    if(first)TgGpuTraceRecovery(&record);
    if(!send)return status;
    RECOVERY_NOTIFY_CTX ctx={.Engine=e,.FaultEpoch=fault_epoch};
    status=e->Dxgk.DxgkCbSynchronizeExecution(e->Dxgk.DeviceHandle,notify,&ctx,0,&invoked);
    KeAcquireSpinLock(&e->Lock,&irql);
    /* Only release RecoverySending if this request still owns the active token
     * so a stale caller across restart never clears a newer sender's flag (B07). */
    if(e->RecoveryActiveToken==my_token) {
        e->RecoverySending=FALSE;
        e->RecoveryActiveToken=0;
    }
    if(e->RecoveryNotified)status=STATUS_SUCCESS;
    else if(NT_SUCCESS(status))
        status=(e->Stopping || e->Resetting ||
                (fault_epoch && !TgGpuFaultEpochMatches(&e->FaultIsr,fault_epoch))) ?
               STATUS_DEVICE_NOT_READY : STATUS_UNSUCCESSFUL;
    KeReleaseSpinLock(&e->Lock,irql);return status;
}
