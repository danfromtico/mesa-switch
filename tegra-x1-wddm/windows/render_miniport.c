/* SPDX-License-Identifier: MIT */
#include "adapter.h"
#include "allocation.h"
#include "query.h"
#include "reset_backend.h"
#include "submit_ddi.h"
#ifdef TG_GPU_NATIVE_TEST_VERSION
#include "native_start_test.h"
#endif
#ifdef TG_GPU_PAGING_DIAGNOSTICS
#include "paging_dispatch.h"
#include "diagnostic_events.h"

static volatile LONG g_PagingFailureRecorded;
static volatile LONG g_PagingDirectoryRecorded;

#ifdef TG_GPU_PAGING_STOP_PROBE
#define TG_GPU_PAGING_PROBE_BUGCHECK 0xe0535751u
enum { PagingProbeInactive, PagingProbeArmed, PagingProbeCaptured };
static volatile LONG g_PagingProbeState;
#endif

static NTSTATUS APIENTRY diagnostic_paging_buffer(HANDLE adapter, DXGKARG_BUILDPAGINGBUFFER *args)
{
    TG_GPU_PAGING_REJECTION rejection = {0};
    NTSTATUS status = TgGpuBuildPagingRequest(adapter, args, &rejection);
    if (NT_SUCCESS(status) && args && args->Operation == DXGK_OPERATION_UPDATE_PAGE_TABLE &&
        args->UpdatePageTable.PageTableLevel == 1 &&
        args->UpdatePageTable.UpdateMode == DXGK_PAGETABLEUPDATE_GPU_PHYSICAL &&
        args->UpdatePageTable.DriverProtection &&
        InterlockedCompareExchange(&g_PagingDirectoryRecorded, 1, 0) == 0) {
        const TG_GPU_PAGING_REJECTION_EVENT event = {
            .Version = 1, .Operation = args->Operation, .Status = status,
            .PageTableLevel = 1, .UpdateMode = DXGK_PAGETABLEUPDATE_GPU_PHYSICAL,
            .HasAllocation = args->UpdatePageTable.hAllocation != NULL,
            .UpdateFieldsValid = 1, .Detail = args->UpdatePageTable.DriverProtection,
        };
        TgGpuTracePagingDirectory(&event);
    }
    if (status == STATUS_NOT_SUPPORTED && args &&
        InterlockedCompareExchange(&g_PagingFailureRecorded, 1, 0) == 0) {
        TG_GPU_PAGING_REJECTION_EVENT event = {
            .Version = 1, .Operation = args->Operation, .Status = status,
            .Site = rejection.Site, .Detail = rejection.Detail,
        };
        if (args->Operation == DXGK_OPERATION_UPDATE_PAGE_TABLE) {
            event.PageTableLevel = args->UpdatePageTable.PageTableLevel;
            event.UpdateMode = args->UpdatePageTable.UpdateMode;
            event.HasAllocation = args->UpdatePageTable.hAllocation != NULL;
            event.UpdateFieldsValid = 1;
        }
        /* No per-success trace, disk write or GPU register access. Recording
         * needs an enabled ETW session; it is not a durable on-disk log. */
        TgGpuTracePagingRejection(&event);
#ifdef TG_GPU_PAGING_STOP_PROBE
        if (InterlockedCompareExchange(&g_PagingProbeState, PagingProbeCaptured,
                                       PagingProbeArmed) != PagingProbeArmed)
            return status;
        /* V2 parameter 3: site[31:0], level[39:32], mode[47:40],
         * allocation-present[48]. Parameter 4 retains the full detail. */
        ULONG_PTR context = (ULONG)rejection.Site;
        if (args->Operation == DXGK_OPERATION_UPDATE_PAGE_TABLE) {
            const DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE *u = &args->UpdatePageTable;
            context |= ((ULONG_PTR)u->PageTableLevel & 0xffu) << 32;
            context |= ((ULONG_PTR)u->UpdateMode & 0xffu) << 40;
            context |= (ULONG_PTR)(u->hAllocation != NULL) << 48;
        }
        KeBugCheckEx(TG_GPU_PAGING_PROBE_BUGCHECK, (ULONG)args->Operation,
                     (ULONG)status, context, rejection.Detail);
#endif
    }
    return status;
}
#endif

static DXGKDDI_STOP_DEVICE *g_BaseStopDevice;
static DXGKDDI_STOP_DEVICE_AND_RELEASE_POST_DISPLAY_OWNERSHIP *g_BaseStopAndRelease;

static VOID record_status(PCWSTR name, NTSTATUS status)
{
    ULONG value=(ULONG)status;
    (VOID)RtlWriteRegistryValue(RTL_REGISTRY_SERVICES,L"tegragpurender\\Parameters",
                              name,REG_DWORD,&value,sizeof(value));
}

#ifdef TG_GPU_PANEL_WDDM
static VOID map_post_display(TG_GPU_ADAPTER *a)
{
    if (!a || a->Panel.PostCpuAddress)
        return;
    PHYSICAL_ADDRESS pa = {.QuadPart = T214_GPU_POST_FB_BASE};
    a->Panel.PostCpuAddress = MmMapIoSpaceEx(
        pa, T214_GPU_POST_FB_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    record_status(L"PostDisplayMapStatus",
        a->Panel.PostCpuAddress ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES);
}
#endif

static VOID unmap_post_display(TG_GPU_ADAPTER *a)
{
    if (!a || !a->Panel.PostCpuAddress)
        return;
    PVOID mapping = (PVOID)a->Panel.PostCpuAddress;
    a->Panel.PostCpuAddress = NULL;
    MmUnmapIoSpace(mapping, T214_GPU_POST_FB_SIZE);
}

static NTSTATUS APIENTRY miniport_stop_device(PVOID context)
{
    TG_GPU_ADAPTER *a = context;
    NTSTATUS status = g_BaseStopDevice ? g_BaseStopDevice(context) : STATUS_INVALID_PARAMETER;
    if (NT_SUCCESS(status) && a)
        unmap_post_display(a);
    return status;
}

static NTSTATUS APIENTRY miniport_stop_and_release(PVOID context, D3DDDI_VIDEO_PRESENT_TARGET_ID target,
    PDXGK_DISPLAY_INFORMATION info)
{
    TG_GPU_ADAPTER *a = context;
    NTSTATUS status = g_BaseStopAndRelease ? g_BaseStopAndRelease(context, target, info) : STATUS_NOT_SUPPORTED;
    if (NT_SUCCESS(status) && a)
        unmap_post_display(a);
    return status;
}

static NTSTATUS start_device(PVOID context,DXGK_START_INFO *start,
    DXGKRNL_INTERFACE *dxgk,ULONG *sources,ULONG *children)
{
    TG_GPU_ADAPTER *a=context;
    TgGpuRecordStartup(a,TgGpuStartupEntering,STATUS_PENDING);
#ifdef TG_GPU_PAGING_DIAGNOSTICS
    InterlockedExchange(&g_PagingFailureRecorded, 0);
    InterlockedExchange(&g_PagingDirectoryRecorded, 0);
    record_status(L"PagingDiagnosticsVersion", 1);
    record_status(L"PagingStopProbeArmed", 0);
#ifdef TG_GPU_PAGING_STOP_PROBE
    InterlockedExchange(&g_PagingProbeState, PagingProbeInactive);
#endif
#endif
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
    record_status(L"HardwareStage",(NTSTATUS)a->Hardware.Status.stage);
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
    map_post_display(a);
    *sources=*children=1;
    a->Panel.ChildReported=TRUE;
#endif
    if(status==STATUS_SUCCESS)TgNvdecAdapterInitialize(a);
    if (NT_SUCCESS(status)) {
        TgGpuRecordStartup(a,TgGpuStartupComplete,status);
#ifdef TG_GPU_PAGING_STOP_PROBE
        if (InterlockedCompareExchange(&g_PagingProbeState, PagingProbeArmed,
                                       PagingProbeInactive) == PagingProbeInactive)
            record_status(L"PagingStopProbeArmed", (NTSTATUS)TG_GPU_PAGING_PROBE_BUGCHECK);
#endif
    }
    return status;
}

static NTSTATUS set_power(PVOID context,ULONG uid,DEVICE_POWER_STATE state,POWER_ACTION action)
{
    PAGED_CODE();
    return TgGpuSetAdapterPowerState(context,uid,state,action);
}

static BOOLEAN interrupt(PVOID context,ULONG message)
{
    TG_GPU_ADAPTER *a=context;
    if(!a || message)return FALSE;
    return TgGpuServiceFaultIsr(&a->Engine.FaultIsr,&a->Engine.Dxgk);
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

static VOID unload(VOID)
{
    (VOID)TgGpuRetryUnresolvedRailRelease();
    TgGpuSetPlatformDriverObject(NULL);
    TgGpuTraceShutdown();
}

DRIVER_INITIALIZE DriverEntry;
NTSTATUS DriverEntry(PDRIVER_OBJECT object,PUNICODE_STRING path)
{
    TgGpuSetPlatformDriverObject(object);
    record_status(L"InitializeStatus",STATUS_PENDING);
    record_status(L"TraceStatus",TgGpuTraceInitialize());
    DRIVER_INITIALIZATION_DATA d={0};
    d.Version=DXGKDDI_INTERFACE_VERSION;
    TgGpuRegisterAdapterDdis(&d);
#ifdef TG_GPU_PAGING_DIAGNOSTICS
    d.DxgkDdiBuildPagingBuffer=diagnostic_paging_buffer;
#endif
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
    d.DxgkDdiQueryAdapterInfo=TgGpuDdiQueryDisplayAdapterInfo;
    TgGpuRegisterPanelLifecycle(&d);
    TgGpuRegisterDisplaySubmissionDdis(&d);
#else
    d.DxgkDdiQueryCurrentFence=NULL; /* MCDM uses scheduler fence notifications. */
#endif
    g_BaseStopDevice=d.DxgkDdiStopDevice;
    d.DxgkDdiStopDevice=miniport_stop_device;
    if(d.DxgkDdiStopDeviceAndReleasePostDisplayOwnership) {
        g_BaseStopAndRelease=d.DxgkDdiStopDeviceAndReleasePostDisplayOwnership;
        d.DxgkDdiStopDeviceAndReleasePostDisplayOwnership=miniport_stop_and_release;
    }
    TgNvdecRegisterEscape(&d); /* preserve the existing panel escape */
    NTSTATUS status=DxgkInitialize(object,path,&d);
    if(!NT_SUCCESS(status)){TgGpuSetPlatformDriverObject(NULL);TgGpuTraceShutdown();}
    record_status(L"InitializeStatus",status);
    return status;
}
