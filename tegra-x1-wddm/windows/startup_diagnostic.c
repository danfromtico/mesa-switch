/* SPDX-License-Identifier: MIT */
#include "adapter.h"

void TgGpuRecordStartup(TG_GPU_ADAPTER *adapter,enum tg_gpu_startup_stage stage,NTSTATUS status)
{
    /* A level-zero callback can sample the stage and status as one value. */
    if (adapter) {
        const ULONGLONG snapshot=((ULONGLONG)stage<<32)|(ULONG)status;
        InterlockedExchange64(&adapter->StartupDiagnostic,(LONG64)snapshot);
    }
}

NTSTATUS APIENTRY TgGpuDdiCollectDiagnosticInfo(PDEVICE_OBJECT physical,
    DXGKARG_COLLECTDIAGNOSTICINFO *args)
{
    static const CHAR add_bucket[]="tegragpurender_adddevice";
    static const CHAR start_bucket[]="tegragpurender_startdevice";
    static const CHAR with_adapter[]="adapter_context_available";
    static const CHAR without_adapter[]="adapter_context_unavailable";
    _Static_assert(sizeof(add_bucket)<=sizeof(args->BucketingString),"AddDevice bucket");
    _Static_assert(sizeof(start_bucket)<=sizeof(args->BucketingString),"StartDevice bucket");
    _Static_assert(sizeof(with_adapter)<=sizeof(args->DescriptionString),"Available adapter");
    _Static_assert(sizeof(without_adapter)<=sizeof(args->DescriptionString),"Unavailable adapter");
    PAGED_CODE();

    if (!args)
        return STATUS_INVALID_PARAMETER;
    args->BufferSizeOut=0;
    RtlZeroMemory(args->BucketingString,sizeof(args->BucketingString));
    RtlZeroMemory(args->DescriptionString,sizeof(args->DescriptionString));
    if (args->Type!=DXGK_DI_ADDDEVICE && args->Type!=DXGK_DI_STARTDEVICE)
        return STATUS_NOT_SUPPORTED;
    if (!args->pBuffer && args->BufferSizeIn)
        return STATUS_INVALID_PARAMETER;
    if (args->BufferSizeIn<sizeof(TG_GPU_STARTUP_DIAGNOSTIC))
        return STATUS_BUFFER_TOO_SMALL;

    /* Stage and StartStatus are meaningful only when AdapterAvailable is set. */
    TG_GPU_STARTUP_DIAGNOSTIC record={.Version=1,.Type=args->Type};
    TG_GPU_ADAPTER *adapter=args->hAdapter;
    if (physical && adapter && adapter->PhysicalDevice==physical) {
        const ULONGLONG snapshot=InterlockedCompareExchange64(&adapter->StartupDiagnostic,0,0);
        record.AdapterAvailable=TRUE;
        record.Stage=(ULONG)(snapshot>>32);
        record.StartStatus=(NTSTATUS)(ULONG)snapshot;
    }
    if (args->Type==DXGK_DI_ADDDEVICE)
        RtlCopyMemory(args->BucketingString,add_bucket,sizeof(add_bucket));
    else
        RtlCopyMemory(args->BucketingString,start_bucket,sizeof(start_bucket));
    if (record.AdapterAvailable)
        RtlCopyMemory(args->DescriptionString,with_adapter,sizeof(with_adapter));
    else
        RtlCopyMemory(args->DescriptionString,without_adapter,sizeof(without_adapter));
    RtlCopyMemory(args->pBuffer,&record,sizeof(record));
    args->BufferSizeOut=sizeof(record);
    const TG_GPU_STARTUP_DIAGNOSTIC_EVENT event={
        .Snapshot=record,.BufferSizeIn=args->BufferSizeIn,.BufferSizeOut=args->BufferSizeOut
    };
    TgGpuTraceStartupDiagnostic(&event);
    return STATUS_SUCCESS;
}
