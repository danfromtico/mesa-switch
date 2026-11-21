/* SPDX-License-Identifier: MIT */
#include "adapter.h"
#include "reset_backend.h"
#include "gr_firmware_io.h"

NTSTATUS APIENTRY TgGpuDdiResetFromTimeout(HANDLE adapter)
{
    TG_GPU_ADAPTER *a=adapter;NTSTATUS status;
    PAGED_CODE();
    if(!a || !a->EngineReady || (a->State!=TgGpuAdapterReady && a->State!=TgGpuAdapterResetting))
        return STATUS_INVALID_DEVICE_STATE;
    a->State=TgGpuAdapterResetting;
    status=TgNvdecAdapterStop(a);
    if(status!=STATUS_SUCCESS)return status;
    status=TgGpuResetBackend(&a->Engine,&a->Queue,&a->Hardware,&a->Worker);
    a->QueueReady=a->Queue.Engine==&a->Engine;return status;
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
    return TgGpuRestartAdapterHardware(a);
}
static BOOLEAN engine_idle(TG_GPU_ADAPTER *a,UINT *fence)
{
    KIRQL irql,engine_irql;
    TG_GPU_WORK_QUEUE *q=&a->Queue;
    TG_GPU_CONTEXT_ENGINE *e=&a->Engine;
    if (a->State!=TgGpuAdapterReady || !a->QueueReady) return FALSE;
    KeAcquireSpinLock(&q->Lock,&irql);
    KeAcquireSpinLock(&e->Lock,&engine_irql);
    BOOLEAN idle=!e->RecoveryPending && !e->Resetting && !q->Servicing &&
        !q->Current.Engine && IsListEmpty(&q->Pending);
    *fence=e->LastCompletedFence;
    KeReleaseSpinLock(&e->Lock,engine_irql);
    KeReleaseSpinLock(&q->Lock,irql);
    return idle;
}
static NTSTATUS APIENTRY query_group(HANDLE adapter,DXGKARG_QUERYDEPENDENTENGINEGROUP *a)
{
    if (!adapter || !a || a->NodeOrdinal || a->EngineOrdinal) return STATUS_INVALID_PARAMETER;
    a->DependentNodeOrdinalMask=1;
    return STATUS_SUCCESS;
}
static NTSTATUS APIENTRY query_engine(HANDLE adapter,DXGKARG_QUERYENGINESTATUS *a)
{
    UINT fence;
    if (!adapter || !a || a->NodeOrdinal || a->EngineOrdinal) return STATUS_INVALID_PARAMETER;
    a->EngineStatus.Value=engine_idle(adapter,&fence)?1:0;
    return STATUS_SUCCESS;
}
static NTSTATUS APIENTRY reset_engine(HANDLE adapter,DXGKARG_RESETENGINE *a)
{
    UINT fence;
    if (!adapter || !a || a->NodeOrdinal || a->EngineOrdinal) return STATUS_INVALID_PARAMETER;
    /* The queue may have drained before the timeout callback arrived.
     * Otherwise GR and CE share the runlist and need the full adapter reset. */
    if (!engine_idle(adapter,&fence)) return STATUS_UNSUCCESSFUL;
    a->LastAbortedFenceId=fence;
    return STATUS_SUCCESS;
}
static NTSTATUS APIENTRY collect_debug_info(HANDLE adapter,const DXGKARG_COLLECTDBGINFO *a)
{
    const TG_GPU_ADAPTER *d=adapter;
    if (!d || !a || !a->pBuffer || a->BufferSize<sizeof(UINT)) return STATUS_INVALID_PARAMETER;
    UINT fence=d->Engine.LastCompletedFence;
    RtlCopyMemory(a->pBuffer,&fence,sizeof(fence));
    return STATUS_SUCCESS;
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
