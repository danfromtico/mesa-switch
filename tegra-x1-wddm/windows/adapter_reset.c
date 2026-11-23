/* SPDX-License-Identifier: MIT */
#include "adapter.h"
#include "reset_backend.h"
#include "gr_firmware_io.h"

/* Caller holds Engine.Lock; event publication always follows lock release. */
static VOID tdr_snapshot_locked(TG_GPU_CONTEXT_ENGINE *e,TG_GPU_TDR_EVENT *record)
{
    record->Flags|=TgGpuTdrEngineSnapshot;
    record->LastSubmittedFence=e->LastSubmittedFence;
    record->LastCompletedFence=e->LastCompletedFence;
    if(e->SubmittedFenceValid)record->Flags|=TgGpuTdrSubmittedValid;
    if(e->AdapterIdentityReady){record->AdapterLuid=e->AdapterLuid;record->Flags|=TgGpuTdrIdentityValid;}
    if(e->RecoveryPending)record->Flags|=TgGpuTdrRecoveryPending;
    if(e->Resetting)record->Flags|=TgGpuTdrResetting;
}
static VOID trace_adapter_tdr(TG_GPU_CONTEXT_ENGINE *e,ULONG operation,ULONG phase,NTSTATUS status)
{
    KIRQL irql;
    if(!TgGpuTraceTdrEnabled())return;
    TG_GPU_TDR_EVENT record={.Version=1,.Operation=operation,.Phase=phase,
        .Status=status,.Engine=(ULONGLONG)(ULONG_PTR)e};
    KeAcquireSpinLock(&e->Lock,&irql);tdr_snapshot_locked(e,&record);KeReleaseSpinLock(&e->Lock,irql);
    TgGpuTraceTdr(&record);
}

NTSTATUS APIENTRY TgGpuDdiResetFromTimeout(HANDLE adapter)
{
    TG_GPU_ADAPTER *a=adapter;NTSTATUS status;
    PAGED_CODE();
    if(!a || !a->EngineReady || (a->State!=TgGpuAdapterReady && a->State!=TgGpuAdapterResetting))
        return STATUS_INVALID_DEVICE_STATE;
    trace_adapter_tdr(&a->Engine,TgGpuTdrResetAdapter,0,0);
    a->State=TgGpuAdapterResetting;
    status=TgNvdecAdapterStop(a);
    if(status!=STATUS_SUCCESS) {
        trace_adapter_tdr(&a->Engine,TgGpuTdrResetAdapter,1,status);return status;
    }
    status=TgGpuResetBackend(&a->Engine,&a->Queue,&a->Hardware,&a->Worker);
    if(status==STATUS_SUCCESS) {
        KIRQL irql;
        /* This is the scheduler's adapter-wide reset boundary. Suspend and
         * generic cleanup share ResetBackend but must not retire its fences.
         * Use the most recent scheduler submission, including ordered
         * format rejection markers and wraparound;
         * preemption identifiers belong to a separate sequence. */
        KeAcquireSpinLock(&a->Engine.Lock,&irql);
        if(a->Engine.SubmittedFenceValid)
            a->Engine.LastCompletedFence=a->Engine.LastSubmittedFence;
        a->Engine.EngineQueryFence=a->Engine.LastCompletedFence;
        KeReleaseSpinLock(&a->Engine.Lock,irql);
    }
    a->QueueReady=a->Queue.Engine==&a->Engine;
    trace_adapter_tdr(&a->Engine,TgGpuTdrResetAdapter,1,status);return status;
}
NTSTATUS TgGpuRestartAdapterHardware(TG_GPU_ADAPTER *a)
{
    TG_GPU_FIRMWARE firmware={0};
    struct tg_gpu_power_io io;UCHAR address=0;NTSTATUS status;
    PAGED_CODE();
    if(!a || !a->EngineReady || !a->Engine.Resetting ||
       !a->Platform.Resources || !a->Hardware.Isolated)return STATUS_INVALID_DEVICE_STATE;
    /* The OS has completed its cleanup interval. Only now load firmware,
     * preflight the platform and perform a new cold hardware initialization. */
    status=TgGpuPlatformPreflight(&a->Platform,&address);
    if(NT_SUCCESS(status))status=TgGpuFirmwareLoad(&firmware);
    if(NT_SUCCESS(status)) {
        io=TgGpuPlatformIo(&a->Platform);
        status=TgGpuRestartBackend(&a->Engine,&a->Queue,&a->Hardware,&a->Worker,
            &io,address,&firmware.payload);
        if(NT_SUCCESS(status) && !NT_SUCCESS(a->Platform.IoStatus))status=a->Platform.IoStatus;
    }
    TgGpuFirmwareFree(&firmware);a->QueueReady=a->Queue.Engine==&a->Engine;
    if(NT_SUCCESS(status)) {
        TgGpuDvfsAttach(&a->Hardware,&a->Platform,&a->Queue);
        a->State=TgGpuAdapterReady;
    } else if(!a->Hardware.Power.owned) {
        TgGpuPlatformAbandonRail(&a->Platform,address);
    }
    return status;
}
NTSTATUS APIENTRY TgGpuDdiRestartFromTimeout(HANDLE adapter)
{
    TG_GPU_ADAPTER *a=adapter;
    if (!a || a->State!=TgGpuAdapterResetting) return STATUS_INVALID_DEVICE_STATE;
    trace_adapter_tdr(&a->Engine,TgGpuTdrRestartAdapter,0,0);
    NTSTATUS status=TgGpuRestartAdapterHardware(a);
    trace_adapter_tdr(&a->Engine,TgGpuTdrRestartAdapter,1,status);return status;
}
static BOOLEAN engine_idle(TG_GPU_ADAPTER *a,UINT *fence,BOOLEAN *responsive,TG_GPU_TDR_EVENT *record)
{
    KIRQL irql,engine_irql;
    TG_GPU_WORK_QUEUE *q=&a->Queue;
    TG_GPU_CONTEXT_ENGINE *e=&a->Engine;
    if(responsive)*responsive=FALSE;
    if (a->State!=TgGpuAdapterReady || !a->QueueReady) return FALSE;
    KeAcquireSpinLock(&q->Lock,&irql);
    KeAcquireSpinLock(&e->Lock,&engine_irql);
    BOOLEAN healthy=!q->Paused && NT_SUCCESS(q->Failure) &&
        !e->RecoveryPending && !e->Resetting;
    BOOLEAN idle=!q->Servicing &&
        !q->Current.Engine && IsListEmpty(&q->Pending);
    *fence=e->LastCompletedFence;
    if(record) {
        tdr_snapshot_locked(e,record);
        if(healthy)record->Flags|=TgGpuTdrHealthy;
        if(idle)record->Flags|=TgGpuTdrIdle;
        if(q->Paused)record->Flags|=TgGpuTdrQueuePaused;
        if(!NT_SUCCESS(q->Failure))record->Flags|=TgGpuTdrQueueFailure;
        record->QueueFailure=q->Failure;
        if(q->Servicing)record->Flags|=TgGpuTdrQueueServicing;
        if(!IsListEmpty(&q->Pending))record->Flags|=TgGpuTdrQueuePending;
        if(responsive){record->QuerySampleFence=e->EngineQueryFence;record->Flags|=TgGpuTdrQuerySampleValid;}
    }
    if(responsive) {
        /* Busy is not the same as unresponsive. Report an actual completed
         * packet since the previous sample, or a healthy empty queue. The
         * first admission after idle starts a fresh observation interval.
         * No observed completion is not a claim that the GPU is dead. */
        *responsive=healthy && (idle || *fence!=e->EngineQueryFence);
        e->EngineQueryFence=*fence;
    }
    KeReleaseSpinLock(&e->Lock,engine_irql);
    KeReleaseSpinLock(&q->Lock,irql);
    return healthy && idle;
}
static NTSTATUS APIENTRY query_group(HANDLE adapter,DXGKARG_QUERYDEPENDENTENGINEGROUP *a)
{
    if (!adapter || !a || a->NodeOrdinal || a->EngineOrdinal) return STATUS_INVALID_PARAMETER;
    a->DependentNodeOrdinalMask=1;
    return STATUS_SUCCESS;
}
static NTSTATUS APIENTRY query_engine(HANDLE adapter,DXGKARG_QUERYENGINESTATUS *a)
{
    UINT fence;BOOLEAN responsive;
    if (!adapter || !a || a->NodeOrdinal || a->EngineOrdinal) return STATUS_INVALID_PARAMETER;
    TG_GPU_ADAPTER *d=adapter;BOOLEAN trace=TgGpuTraceTdrEnabled();
    TG_GPU_TDR_EVENT record={.Version=1,.Operation=TgGpuTdrQueryEngine,.Phase=1,
        .Engine=(ULONGLONG)(ULONG_PTR)&d->Engine,.NodeOrdinal=a->NodeOrdinal,.EngineOrdinal=a->EngineOrdinal};
    (VOID)engine_idle(d,&fence,&responsive,trace?&record:NULL);
    a->EngineStatus.Value=responsive?1:0;
    if(trace){record.Result=a->EngineStatus.Value;record.Flags|=TgGpuTdrResultValid;TgGpuTraceTdr(&record);}
    return STATUS_SUCCESS;
}
static NTSTATUS APIENTRY reset_engine(HANDLE adapter,DXGKARG_RESETENGINE *a)
{
    UINT fence;
    if (!adapter || !a || a->NodeOrdinal || a->EngineOrdinal) return STATUS_INVALID_PARAMETER;
    /* The queue may have drained before the timeout callback arrived.
     * Otherwise GR and CE share the runlist and need the full adapter reset. */
    TG_GPU_ADAPTER *d=adapter;BOOLEAN trace=TgGpuTraceTdrEnabled();
    TG_GPU_TDR_EVENT record={.Version=1,.Operation=TgGpuTdrResetEngine,.Phase=1,
        .Engine=(ULONGLONG)(ULONG_PTR)&d->Engine,.NodeOrdinal=a->NodeOrdinal,.EngineOrdinal=a->EngineOrdinal};
    NTSTATUS status=engine_idle(d,&fence,NULL,trace?&record:NULL)?STATUS_SUCCESS:STATUS_UNSUCCESSFUL;
    if(status==STATUS_SUCCESS){a->LastAbortedFenceId=fence;record.Result=fence;record.Flags|=TgGpuTdrResultValid;}
    if(trace){record.Status=status;TgGpuTraceTdr(&record);}
    return status;
}
static NTSTATUS APIENTRY collect_debug_info(HANDLE adapter,const DXGKARG_COLLECTDBGINFO *a)
{
    TG_GPU_ADAPTER *d=adapter;
    if(!d || !a)return STATUS_UNSUCCESSFUL;
    /* May run during a bugcheck at undefined IRQL. All data is resident:
     * no queue/context dereference, lock wait, allocation, ETW or MMIO here.
     * LastCompletedFence is an independent aligned observation; cached
     * boundaries include their own fence/generation and validity flags. */
    return TgGpuCopyDebugReport(&d->Engine.DebugCache,d->Engine.LastCompletedFence,
        a->Reason,a->pBuffer,a->BufferSize);
}
void TgGpuRegisterResetDdis(DRIVER_INITIALIZATION_DATA *d)
{
    d->DxgkDdiResetFromTimeout=TgGpuDdiResetFromTimeout;
    d->DxgkDdiRestartFromTimeout=TgGpuDdiRestartFromTimeout;
    d->DxgkDdiQueryDependentEngineGroup=query_group;
    d->DxgkDdiQueryEngineStatus=query_engine;
    d->DxgkDdiResetEngine=reset_engine;
    d->DxgkDdiCollectDbgInfo=collect_debug_info;
}
