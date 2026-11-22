/* SPDX-License-Identifier: MIT */
#include "work_queue.h"

static BOOLEAN notify(PVOID context)
{
    TG_GPU_CONTEXT_ENGINE *e=context;
    DXGKARGCB_NOTIFY_INTERRUPT_DATA data={.InterruptType=DXGK_INTERRUPT_GPU_ENGINE_TIMEOUT};
    data.GpuEngineTimeout.NodeOrdinal=data.GpuEngineTimeout.EngineOrdinal=0;
    e->RecoveryNotified=TRUE;
    e->Dxgk.DxgkCbNotifyInterrupt(e->Dxgk.DeviceHandle,&data);
    (VOID)e->Dxgk.DxgkCbQueueDpc(e->Dxgk.DeviceHandle);return TRUE;
}
NTSTATUS TgGpuRequestEngineRecovery(TG_GPU_CONTEXT_ENGINE *e,NTSTATUS reason)
{
    return TgGpuRequestEngineRecoveryAt(e,reason,TgGpuRecoveryUnspecified,NULL);
}
NTSTATUS TgGpuRequestEngineRecoveryAt(TG_GPU_CONTEXT_ENGINE *e,NTSTATUS reason,
    enum tg_gpu_recovery_source source,const DXGKARG_SUBMITCOMMANDVIRTUAL *submit)
{
    TG_GPU_WORK_QUEUE *q;KIRQL irql,queue_irql;BOOLEAN invoked=FALSE,send=FALSE;NTSTATUS status;
    TG_GPU_RECOVERY_EVENT record={0};BOOLEAN first=FALSE;
    if(!e || NT_SUCCESS(reason) || !e->Dxgk.DeviceHandle || !e->Dxgk.DxgkCbSynchronizeExecution ||
       !e->Dxgk.DxgkCbNotifyInterrupt || !e->Dxgk.DxgkCbQueueDpc)return STATUS_INVALID_PARAMETER;
    /* Adapter/queue lifetime is serialized against stop by the caller. No
     * register access, completion fence, context release or recovery reset is
     * performed here; the actual WDDM TDR callbacks remain responsible. */
    q=e->QueueOwner;
    if(q){KeAcquireSpinLock(&q->Lock,&queue_irql);q->Paused=TRUE;}
    KeAcquireSpinLock(&e->Lock,&irql);
    if(e->Stopping || e->Resetting)status=STATUS_DEVICE_NOT_READY;
    else if(e->RecoveryNotified)status=STATUS_SUCCESS;
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
        e->RecoverySending=TRUE;send=TRUE;status=STATUS_SUCCESS;
    }
    KeReleaseSpinLock(&e->Lock,irql);
    if(q)KeReleaseSpinLock(&q->Lock,queue_irql);
    if(first)TgGpuTraceRecovery(&record);
    if(!send)return status;
    status=e->Dxgk.DxgkCbSynchronizeExecution(e->Dxgk.DeviceHandle,notify,e,0,&invoked);
    KeAcquireSpinLock(&e->Lock,&irql);e->RecoverySending=FALSE;
    if(e->RecoveryNotified)status=STATUS_SUCCESS;
    else if(NT_SUCCESS(status))status=STATUS_UNSUCCESSFUL;
    KeReleaseSpinLock(&e->Lock,irql);return status;
}
