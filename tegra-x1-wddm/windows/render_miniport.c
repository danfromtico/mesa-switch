/* SPDX-License-Identifier: MIT */
#include "adapter.h"
#include "allocation.h"
#include "reset_backend.h"
#include "submit_ddi.h"
#ifdef TG_GPU_NATIVE_TEST_VERSION
#include "native_start_test.h"
#endif

static VOID record_status(PCWSTR name, NTSTATUS status)
{
    ULONG value=(ULONG)status;
    (VOID)RtlWriteRegistryValue(RTL_REGISTRY_SERVICES,L"tegragpurender\\Parameters",
                              name,REG_DWORD,&value,sizeof(value));
}

static NTSTATUS start_device(PVOID context,DXGK_START_INFO *start,
    DXGKRNL_INTERFACE *dxgk,ULONG *sources,ULONG *children)
{
    TG_GPU_ADAPTER *a=context;
    TgGpuRecordStartup(a,TgGpuStartupEntering,STATUS_PENDING);
    /* Startup samples only. A closed gate or failed start must not leave an
     * old successful measurement looking like this activation's result.
     */
    record_status(L"BootGpuTargetKhz",(NTSTATUS)TG_GPU_RENDER_KHZ);
    record_status(L"BootGpuMeasuredKhz",0);
    record_status(L"BootGpuRailUv",0);
#ifdef TG_GPU_NATIVE_TEST_VERSION
    record_status(L"HardwareStatus",(NTSTATUS)MAXULONG);
    record_status(L"PanelStatus",(NTSTATUS)MAXULONG);
    NTSTATUS permit=TgGpuConsumeNativeStartPermit();
    record_status(L"BootPermitStatus",permit);
    NTSTATUS gate=NT_SUCCESS(permit)?STATUS_SUCCESS:STATUS_DEVICE_NOT_READY;
    record_status(L"StartGateStatus",gate);
    TgGpuRecordStartup(a,TgGpuStartupPermit,gate);
    if(!NT_SUCCESS(gate))return gate;
#endif
    NTSTATUS status=TgGpuDdiStartDevice(context,start,dxgk,sources,children);
    record_status(L"ResourceStatus",status);
    TgGpuRecordStartup(a,TgGpuStartupResources,status);
    if (!NT_SUCCESS(status)) return status;

    /* Ordinary bring-up builds exercise paging. The explicitly gated test
     * package also exposes the native NVK submission path after startup. */
#ifdef TG_GPU_NATIVE_TEST_VERSION
    a->Engine.PagingOnly=FALSE;
#else
    a->Engine.PagingOnly=TRUE;
#endif
    status=TgGpuStartPlatformAdapter(a);
    record_status(L"HardwareStatus",status);
    TgGpuRecordStartup(a,TgGpuStartupHardware,status);
    if(NT_SUCCESS(status)) {
        record_status(L"BootGpuMeasuredKhz",(NTSTATUS)a->Hardware.Clock.measured_khz);
        record_status(L"BootGpuRailUv",(NTSTATUS)a->Hardware.Clock.rail_uv);
    }
#ifdef TG_GPU_PANEL_WDDM
    if (!NT_SUCCESS(status)) return status;
    status=TgGpuPanelStart(a);
    record_status(L"PanelStatus",status);
    TgGpuRecordStartup(a,TgGpuStartupPanel,status);
    if (!NT_SUCCESS(status)) {
        NTSTATUS cleanup=TgGpuPanelStop(a);
        if (NT_SUCCESS(cleanup)) cleanup=TgGpuDdiStopDevice(a);
        if (!NT_SUCCESS(cleanup)) {
            TgGpuRecordStartup(a,TgGpuStartupPanel,cleanup);
            return cleanup;
        }
        return status;
    }
    *sources=*children=1;
#endif
    if(status==STATUS_SUCCESS)TgNvdecAdapterInitialize(a);
    if (NT_SUCCESS(status))
        TgGpuRecordStartup(a,TgGpuStartupComplete,status);
    return status;
}

static NTSTATUS set_power(PVOID context,ULONG uid,DEVICE_POWER_STATE state,POWER_ACTION action)
{
    TG_GPU_ADAPTER *a=context;
    UNREFERENCED_PARAMETER(action);
    PAGED_CODE();
    if (uid!=DISPLAY_ADAPTER_HW_ID || !a->EngineReady) return STATUS_SUCCESS;
    if (state==PowerDeviceD0) {
        return a->State==TgGpuAdapterSuspended ? TgGpuRestartAdapterHardware(a) : STATUS_SUCCESS;
    }
    if (a->State!=TgGpuAdapterReady && a->State!=TgGpuAdapterSuspended) return STATUS_SUCCESS;
    if (a->State==TgGpuAdapterSuspended && a->Hardware.Isolated) return STATUS_SUCCESS;
    NTSTATUS status=TgNvdecAdapterStop(a);
    if(status!=STATUS_SUCCESS)return status;
    status=TgGpuSuspendBackend(&a->Engine,&a->Queue,&a->Hardware,&a->Worker);
    if (a->Engine.Resetting) {
        a->State=TgGpuAdapterSuspended;
        a->QueueReady=a->Queue.Engine==&a->Engine;
    }
    return status;
}

static BOOLEAN interrupt(PVOID context,ULONG message)
{
    UNREFERENCED_PARAMETER(context);
    UNREFERENCED_PARAMETER(message);
    /* The cold backend leaves hardware sources masked. The worker reports
     * verified completions through DxgkCbSynchronizeExecution and NotifyDpc. */
    return FALSE;
}

static NTSTATUS query_children(PVOID context,PDXGK_CHILD_DESCRIPTOR children,ULONG bytes)
{
    UNREFERENCED_PARAMETER(context);
    if (!children || bytes<sizeof(*children)) return STATUS_BUFFER_TOO_SMALL;
    RtlZeroMemory(children,sizeof(*children)); /* terminator; no display outputs */
    return STATUS_SUCCESS;
}

static NTSTATUS query_child_status(PVOID context,PDXGK_CHILD_STATUS child,BOOLEAN nondestructive)
{
    UNREFERENCED_PARAMETER(context);
    UNREFERENCED_PARAMETER(child);
    UNREFERENCED_PARAMETER(nondestructive);
    return STATUS_INVALID_PARAMETER; /* zero child UIDs */
}

static NTSTATUS query_device_descriptor(PVOID context,ULONG uid,PDXGK_DEVICE_DESCRIPTOR descriptor)
{
    UNREFERENCED_PARAMETER(context);
    UNREFERENCED_PARAMETER(uid);
    UNREFERENCED_PARAMETER(descriptor);
    return STATUS_GRAPHICS_CHILD_DESCRIPTOR_NOT_SUPPORTED;
}

static VOID reset_device(PVOID context)
{
    /* VGA reset has no work on this adapter. GPU recovery uses the TDR DDIs. */
    UNREFERENCED_PARAMETER(context);
}

static NTSTATUS get_node(HANDLE adapter,UINT node,DXGKARG_GETNODEMETADATA *metadata)
{
    if (!adapter || node || !metadata) return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(metadata,sizeof(*metadata));
    metadata->EngineType=DXGK_ENGINE_TYPE_3D;
    metadata->GpuMmuSupported=TRUE;
    return STATUS_SUCCESS;
}

static NTSTATUS dispatch_io(PVOID context,ULONG source,PVIDEO_REQUEST_PACKET request)
{
    UNREFERENCED_PARAMETER(context);
    UNREFERENCED_PARAMETER(source);
    UNREFERENCED_PARAMETER(request);
    return STATUS_NOT_SUPPORTED;
}

static NTSTATUS query_interface(PVOID context,PQUERY_INTERFACE request)
{
    UNREFERENCED_PARAMETER(context);
    UNREFERENCED_PARAMETER(request);
    return STATUS_NOT_SUPPORTED;
}

static NTSTATUS APIENTRY standard_allocation(HANDLE adapter,DXGKARG_GETSTANDARDALLOCATIONDRIVERDATA *args)
{
    return TgGpuDdiGetStandardAllocationDriverData(adapter,args);
}

static NTSTATUS APIENTRY calibrate_clock(HANDLE adapter,UINT32 node,UINT32 engine,
    DXGKARG_CALIBRATEGPUCLOCK *args)
{
    UNREFERENCED_PARAMETER(adapter);
    UNREFERENCED_PARAMETER(node);
    UNREFERENCED_PARAMETER(engine);
    UNREFERENCED_PARAMETER(args);
    return STATUS_NOT_SUPPORTED;
}

static NTSTATUS APIENTRY format_history(HANDLE context,DXGKARG_FORMATHISTORYBUFFER *args)
{
    UNREFERENCED_PARAMETER(context);
    UNREFERENCED_PARAMETER(args);
    return STATUS_NOT_SUPPORTED;
}

static VOID APIENTRY set_stable_power(HANDLE adapter,const DXGKARG_SETSTABLEPOWERSTATE *args)
{
    TG_GPU_ADAPTER *a=adapter;
    if(!a || !args)return;
    TgGpuDvfsSetStablePower(&a->Hardware.Dvfs,args->Enabled!=FALSE);
    if(a->QueueReady)(VOID)KeSetEvent(&a->Queue.Wake,IO_NO_INCREMENT,FALSE);
}

static NTSTATUS APIENTRY set_vm_data(HANDLE adapter,const DXGKARG_SETVIRTUALMACHINEDATA *args)
{
    UNREFERENCED_PARAMETER(adapter);
    UNREFERENCED_PARAMETER(args);
    return STATUS_NOT_SUPPORTED;
}

static VOID unload(VOID) {TgGpuSetPlatformDriverObject(NULL);TgGpuTraceShutdown();}

DRIVER_INITIALIZE DriverEntry;
NTSTATUS DriverEntry(PDRIVER_OBJECT object,PUNICODE_STRING path)
{
    TgGpuSetPlatformDriverObject(object);
    record_status(L"InitializeStatus",STATUS_PENDING);
    record_status(L"TraceStatus",TgGpuTraceInitialize());
    DRIVER_INITIALIZATION_DATA d={0};
    d.Version=DXGKDDI_INTERFACE_VERSION;
    TgGpuRegisterAdapterDdis(&d);
    d.DxgkDdiCollectDiagnosticInfo=TgGpuDdiCollectDiagnosticInfo;
    d.DxgkDdiStartDevice=start_device;
    d.DxgkDdiSetPowerState=set_power;
    d.DxgkDdiInterruptRoutine=interrupt;
    d.DxgkDdiQueryChildRelations=query_children;
    d.DxgkDdiQueryChildStatus=query_child_status;
    d.DxgkDdiQueryDeviceDescriptor=query_device_descriptor;
    d.DxgkDdiResetDevice=reset_device;
    d.DxgkDdiGetNodeMetadata=get_node;
    d.DxgkDdiDispatchIoRequest=dispatch_io;
    d.DxgkDdiQueryInterface=query_interface;
    d.DxgkDdiUnload=unload;
    d.DxgkDdiGetStandardAllocationDriverData=standard_allocation;
    d.DxgkDdiCalibrateGpuClock=calibrate_clock;
    d.DxgkDdiFormatHistoryBuffer=format_history;
    d.DxgkDdiSetStablePowerState=set_stable_power;
    d.DxgkDdiSetVirtualMachineData=set_vm_data;
#ifdef TG_GPU_PANEL_WDDM
    TgGpuRegisterPanelLifecycle(&d);
    TgGpuRegisterDisplaySubmissionDdis(&d);
#else
    d.DxgkDdiQueryCurrentFence=NULL; /* MCDM uses scheduler fence notifications. */
#endif
    TgNvdecRegisterEscape(&d); /* preserve the existing panel escape */
    NTSTATUS status=DxgkInitialize(object,path,&d);
    if(!NT_SUCCESS(status)){TgGpuSetPlatformDriverObject(NULL);TgGpuTraceShutdown();}
    record_status(L"InitializeStatus",status);
    return status;
}
