/* SPDX-License-Identifier: MIT */
#include "adapter.h"
#include "diagnostic_events.h"

typedef struct _PANEL_PATH {
    const DXGK_VIDPN_INTERFACE *VidPn;
    const DXGK_VIDPNTOPOLOGY_INTERFACE *Topology;
    D3DKMDT_HVIDPNTOPOLOGY Handle;
    const D3DKMDT_VIDPN_PRESENT_PATH *Path;
} PANEL_PATH;

enum panel_mode_state {
    PanelModeUnpinned, PanelModeNative, PanelModeUnsupported
};

/* The WDK exposes DXGK_CONNECTION_STATUS values only to C++ sources. */
enum { TgGpuLinkConfigurationSucceeded = 14 };
static volatile LONG64 timing_change_id;

/* The firmware drives the panel over a fixed RGB888 DSI link. */
static const D3DKMDT_WIRE_FORMAT_AND_PREFERENCE panel_wire_format = {
    .Rgb = D3DKMDT_BITS_PER_COMPONENT_08,
};

static NTSTATUS acquire_path(TG_GPU_ADAPTER *adapter, D3DKMDT_HVIDPN vidpn, PANEL_PATH *path)
{
    if (!vidpn)
        return STATUS_SUCCESS;
    const DXGKRNL_INTERFACE *dxgk = &adapter->Engine.Dxgk;
    if (!dxgk->DxgkCbQueryVidPnInterface)
        return STATUS_DEVICE_NOT_READY;
    NTSTATUS status = dxgk->DxgkCbQueryVidPnInterface(vidpn,
        DXGK_VIDPN_INTERFACE_VERSION_V1, &path->VidPn);
    if (!NT_SUCCESS(status))
        return status;
    status = path->VidPn->pfnGetTopology(vidpn, &path->Handle, &path->Topology);
    if (!NT_SUCCESS(status))
        return status;
    SIZE_T count = 0;
    status = path->Topology->pfnGetNumPaths(path->Handle, &count);
    if (!NT_SUCCESS(status) || count == 0)
        return status;
    if (count != 1)
        return STATUS_GRAPHICS_INVALID_VIDPN_TOPOLOGY;
    return path->Topology->pfnAcquireFirstPathInfo(path->Handle, &path->Path);
}

static bool supported_path(const TG_GPU_PANEL *panel, const D3DKMDT_VIDPN_PRESENT_PATH *path)
{
    const D3DKMDT_VIDPN_PRESENT_PATH_TRANSFORMATION *x = &path->ContentTransformation;
    return path->VidPnSourceId == 0 && path->VidPnTargetId == panel->Boot.TargetId &&
        (x->Scaling == D3DKMDT_VPPS_IDENTITY || x->Scaling == D3DKMDT_VPPS_UNPINNED ||
         x->Scaling == D3DKMDT_VPPS_NOTSPECIFIED || x->Scaling == D3DKMDT_VPPS_UNINITIALIZED) &&
        ((x->Rotation >= D3DKMDT_VPPR_IDENTITY && x->Rotation <= D3DKMDT_VPPR_ROTATE270) ||
         x->Rotation == D3DKMDT_VPPR_UNPINNED ||
         x->Rotation == D3DKMDT_VPPR_NOTSPECIFIED || x->Rotation == D3DKMDT_VPPR_UNINITIALIZED);
}

static D3DKMDT_VIDPN_PRESENT_PATH_ROTATION path_rotation(const D3DKMDT_VIDPN_PRESENT_PATH *path)
{
    const D3DKMDT_VIDPN_PRESENT_PATH_ROTATION rotation=path->ContentTransformation.Rotation;
    return rotation>=D3DKMDT_VPPR_IDENTITY && rotation<=D3DKMDT_VPPR_ROTATE270 ?
        rotation : D3DKMDT_VPPR_IDENTITY;
}

static bool same_size(D3DKMDT_2DREGION a, D3DKMDT_2DREGION b)
{
    return a.cx == b.cx && a.cy == b.cy;
}

static bool fixed_rotation(D3DKMDT_VIDPN_PRESENT_PATH_ROTATION rotation)
{
    return rotation >= D3DKMDT_VPPR_IDENTITY && rotation <= D3DKMDT_VPPR_ROTATE270;
}

static enum panel_mode_state source_layout(const D3DKMDT_VIDPN_SOURCE_MODE *mode)
{
    /* VidPN mode enumeration describes the unrotated source surface. CCD's
     * rotated desktop extent is not a second monitor/source mode. Requiring
     * a landscape source for quarter turns removes 90/270 from the OS mode
     * list. A DXGI primary independently supplies its actual storage layout
     * to allocation.c and panel_scanout.c; do not change that allocation here. */
    const D3DKMDT_GRAPHICS_RENDERING_FORMAT *g = &mode->Format.Graphics;
    if (mode->Type != D3DKMDT_RMT_GRAPHICS ||
        !same_size(g->PrimSurfSize, g->VisibleRegionSize) ||
        (g->PixelFormat != D3DDDIFMT_X8R8G8B8 && g->PixelFormat != D3DDDIFMT_A8R8G8B8) ||
        g->PixelValueAccessMode != D3DKMDT_PVAM_DIRECT)
        return PanelModeUnsupported;
    if (g->PrimSurfSize.cx == T214_DC_PANEL_WIDTH && g->PrimSurfSize.cy == T214_DC_PANEL_HEIGHT &&
        g->Stride == T214_DC_PANEL_PITCH)
        return PanelModeNative;
    return PanelModeUnsupported;
}

static bool is_native_target(const D3DKMDT_VIDEO_SIGNAL_INFO *signal)
{
    return same_size(signal->ActiveSize, TgGpuPanelSignal.ActiveSize) &&
        same_size(signal->TotalSize, TgGpuPanelSignal.TotalSize) &&
        signal->PixelRate == TgGpuPanelSignal.PixelRate &&
        signal->VSyncFreq.Denominator != 0 &&
        signal->VSyncFreq.Numerator == 60ull * signal->VSyncFreq.Denominator &&
        signal->AdditionalSignalInfo.ScanLineOrdering == D3DDDI_VSSLO_PROGRESSIVE;
}

static NTSTATUS pinned_source(const DXGK_VIDPN_INTERFACE *api, D3DKMDT_HVIDPN vidpn,
    enum panel_mode_state *state)
{
    const DXGK_VIDPNSOURCEMODESET_INTERFACE *modes;
    D3DKMDT_HVIDPNSOURCEMODESET set;
    NTSTATUS status = api->pfnAcquireSourceModeSet(vidpn, 0, &set, &modes);
    if (!NT_SUCCESS(status))
        return status;
    const D3DKMDT_VIDPN_SOURCE_MODE *mode = NULL;
    status = modes->pfnAcquirePinnedModeInfo(set, &mode);
    if (status == STATUS_GRAPHICS_MODE_NOT_PINNED || (NT_SUCCESS(status) && !mode)) {
        *state = PanelModeUnpinned;
        status = STATUS_SUCCESS;
    } else if (NT_SUCCESS(status)) {
        *state = source_layout(mode);
        status = modes->pfnReleaseModeInfo(set, mode);
    }
    NTSTATUS release = api->pfnReleaseSourceModeSet(vidpn, set);
    return NT_SUCCESS(status) ? release : status;
}

static NTSTATUS pinned_target(const DXGK_VIDPN_INTERFACE *api, D3DKMDT_HVIDPN vidpn,
    D3DDDI_VIDEO_PRESENT_TARGET_ID target, enum panel_mode_state *state)
{
    const DXGK_VIDPNTARGETMODESET_INTERFACE *modes;
    D3DKMDT_HVIDPNTARGETMODESET set;
    NTSTATUS status = api->pfnAcquireTargetModeSet(vidpn, target, &set, &modes);
    if (!NT_SUCCESS(status))
        return status;
    const D3DKMDT_VIDPN_TARGET_MODE *mode = NULL;
    status = modes->pfnAcquirePinnedModeInfo(set, &mode);
    if (status == STATUS_GRAPHICS_MODE_NOT_PINNED || (NT_SUCCESS(status) && !mode)) {
        *state = PanelModeUnpinned;
        status = STATUS_SUCCESS;
    } else if (NT_SUCCESS(status)) {
        *state = is_native_target(&mode->VideoSignalInfo) ? PanelModeNative : PanelModeUnsupported;
        status = modes->pfnReleaseModeInfo(set, mode);
    }
    NTSTATUS release = api->pfnReleaseTargetModeSet(vidpn, set);
    return NT_SUCCESS(status) ? release : status;
}

static NTSTATUS add_source(const DXGK_VIDPN_INTERFACE *api, D3DKMDT_HVIDPN vidpn,
    D3DDDIFORMAT format)
{
    const DXGK_VIDPNSOURCEMODESET_INTERFACE *modes;
    D3DKMDT_HVIDPNSOURCEMODESET set;
    NTSTATUS status = api->pfnCreateNewSourceModeSet(vidpn, 0, &set, &modes);
    if (!NT_SUCCESS(status))
        return status;
    /* CDD enumerates A8R8G8B8 for a 32-bit GDI desktop. The firmware may
     * hand over X8R8G8B8, which the panel can scan with the same pitch. */
    const D3DDDIFORMAT formats[] = {format, D3DDDIFMT_A8R8G8B8};
    const UINT count = format == D3DDDIFMT_A8R8G8B8 ? 1 : 2;
    for (UINT i = 0; i < count; ++i) {
        D3DKMDT_VIDPN_SOURCE_MODE *mode;
        status = modes->pfnCreateNewModeInfo(set, &mode);
        if (!NT_SUCCESS(status))
            break;
        mode->Type = D3DKMDT_RMT_GRAPHICS;
        mode->Format.Graphics = (D3DKMDT_GRAPHICS_RENDERING_FORMAT){
            .PrimSurfSize = TgGpuPanelSignal.ActiveSize,
            .VisibleRegionSize = TgGpuPanelSignal.ActiveSize,
            .Stride = T214_DC_PANEL_PITCH,
            .PixelFormat = formats[i],
            .ColorBasis = D3DKMDT_CB_SRGB,
            .PixelValueAccessMode = D3DKMDT_PVAM_DIRECT,
        };
        status = modes->pfnAddMode(set, mode);
        if (!NT_SUCCESS(status)) {
            (VOID)modes->pfnReleaseModeInfo(set, mode);
            break;
        }
    }
    if (NT_SUCCESS(status)) {
        status = api->pfnAssignSourceModeSet(vidpn, 0, set);
        /* Assign consumes the set after validating its arguments. */
        if (status != STATUS_GRAPHICS_INVALID_VIDPN &&
            status != STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE &&
            status != STATUS_GRAPHICS_INVALID_VIDPN_SOURCEMODESET)
            return status;
    }
    (VOID)api->pfnReleaseSourceModeSet(vidpn, set);
    return status;
}

static NTSTATUS add_target(const DXGK_VIDPN_INTERFACE *api, D3DKMDT_HVIDPN vidpn,
    D3DDDI_VIDEO_PRESENT_TARGET_ID target)
{
    const DXGK_VIDPNTARGETMODESET_INTERFACE *modes;
    D3DKMDT_HVIDPNTARGETMODESET set;
    NTSTATUS status = api->pfnCreateNewTargetModeSet(vidpn, target, &set, &modes);
    if (!NT_SUCCESS(status))
        return status;
    D3DKMDT_VIDPN_TARGET_MODE *mode;
    status = modes->pfnCreateNewModeInfo(set, &mode);
    if (NT_SUCCESS(status)) {
        mode->VideoSignalInfo = TgGpuPanelSignal;
        mode->WireFormatAndPreference = panel_wire_format;
        mode->Preference = D3DKMDT_MP_PREFERRED;
        status = modes->pfnAddMode(set, mode);
        if (!NT_SUCCESS(status))
            (VOID)modes->pfnReleaseModeInfo(set, mode);
    }
    if (NT_SUCCESS(status)) {
        status = api->pfnAssignTargetModeSet(vidpn, target, set);
        if (status != STATUS_GRAPHICS_INVALID_VIDPN &&
            status != STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_TARGET &&
            status != STATUS_GRAPHICS_INVALID_VIDPN_TARGETMODESET)
            return status;
    }
    (VOID)api->pfnReleaseTargetModeSet(vidpn, set);
    return status;
}

static NTSTATUS APIENTRY is_supported(HANDLE context, DXGKARG_ISSUPPORTEDVIDPN *args)
{
    TG_GPU_ADAPTER *adapter = context;
    PAGED_CODE();
    if (!adapter || !args)
        return STATUS_INVALID_PARAMETER;
    args->IsVidPnSupported = FALSE;
    if (!adapter->Panel.Dc.owned)
        return STATUS_DEVICE_NOT_READY;
    PANEL_PATH path = {0};
    NTSTATUS status = acquire_path(adapter, args->hDesiredVidPn, &path);
    if (status == STATUS_GRAPHICS_INVALID_VIDPN_TOPOLOGY)
        return STATUS_SUCCESS;
    if (!NT_SUCCESS(status))
        return status;
    if (!path.Path) {
        args->IsVidPnSupported = TRUE; /* No output is a valid topology. */
        return STATUS_SUCCESS;
    }

    if (supported_path(&adapter->Panel, path.Path)) {
        enum panel_mode_state source, target;
        status = pinned_source(path.VidPn, args->hDesiredVidPn, &source);
        if (NT_SUCCESS(status))
            status = pinned_target(path.VidPn, args->hDesiredVidPn, path.Path->VidPnTargetId, &target);
        if (NT_SUCCESS(status))
            args->IsVidPnSupported = source != PanelModeUnsupported &&
                target != PanelModeUnsupported;
    }
    NTSTATUS release = path.Topology->pfnReleasePathInfo(path.Handle, path.Path);
    return NT_SUCCESS(status) ? release : status;
}

static NTSTATUS APIENTRY query_hardware_capability(HANDLE context, DXGKARG_QUERYVIDPNHWCAPABILITY *args)
{
    const TG_GPU_ADAPTER *adapter = context;
    PAGED_CODE();
    if (!adapter || !args || !args->hFunctionalVidPn)
        return STATUS_INVALID_PARAMETER;
    if (!adapter->Panel.Dc.owned)
        return STATUS_DEVICE_NOT_READY;
    if (args->SourceId != 0)
        return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE;
    if (args->TargetId != adapter->Panel.Boot.TargetId)
        return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_TARGET;
    /* The UMD performs a GPU copy before fixed portrait scanout. */
    args->VidPnHWCaps = (D3DKMDT_VIDPN_HW_CAPABILITY){.DriverRotation = 1};
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY enumerate_modes(HANDLE context, const DXGKARG_ENUMVIDPNCOFUNCMODALITY *args)
{
    TG_GPU_ADAPTER *adapter = context;
    PAGED_CODE();
    if (!adapter || !args)
        return STATUS_INVALID_PARAMETER;
    if (!adapter->Panel.Dc.owned)
        return STATUS_DEVICE_NOT_READY;
    D3DKMDT_HVIDPN vidpn = args->hConstrainingVidPn;
    PANEL_PATH path = {0};
    NTSTATUS status = acquire_path(adapter, vidpn, &path);
    if (!NT_SUCCESS(status) || !path.Path)
        return status;
    D3DDDI_VIDEO_PRESENT_TARGET_ID target = path.Path->VidPnTargetId;
    if (!supported_path(&adapter->Panel, path.Path)) {
        status = STATUS_GRAPHICS_INVALID_VIDPN_TOPOLOGY;
        goto done;
    }

    enum panel_mode_state source_mode, target_mode;
    status = pinned_source(path.VidPn, vidpn, &source_mode);
    if (NT_SUCCESS(status))
        status = pinned_target(path.VidPn, vidpn, target, &target_mode);
    if (!NT_SUCCESS(status))
        goto done;
    const D3DKMDT_VIDPN_PRESENT_PATH_ROTATION rotation = path.Path->ContentTransformation.Rotation;
    if (source_mode == PanelModeUnsupported || target_mode == PanelModeUnsupported) {
        status = STATUS_GRAPHICS_INVALID_VIDPN_TOPOLOGY;
        goto done;
    }

    const bool source_pivot = args->EnumPivotType == D3DKMDT_EPT_VIDPNSOURCE &&
        args->EnumPivot.VidPnSourceId == 0;
    const bool target_pivot = args->EnumPivotType == D3DKMDT_EPT_VIDPNTARGET &&
        args->EnumPivot.VidPnTargetId == target;
    if (source_mode == PanelModeUnpinned && !source_pivot)
        status = add_source(path.VidPn, vidpn, adapter->Panel.Boot.ColorFormat);
    if (NT_SUCCESS(status) && target_mode == PanelModeUnpinned && !target_pivot)
        status = add_target(path.VidPn, vidpn, target);
    if (!NT_SUCCESS(status))
        goto done;

    D3DKMDT_VIDPN_PRESENT_PATH update = *path.Path;
    const bool path_pivot = (args->EnumPivotType == D3DKMDT_EPT_SCALING ||
        args->EnumPivotType == D3DKMDT_EPT_ROTATION) &&
        args->EnumPivot.VidPnSourceId == 0 && args->EnumPivot.VidPnTargetId == target;
    if (args->EnumPivotType != D3DKMDT_EPT_SCALING || !path_pivot)
        update.ContentTransformation.ScalingSupport =
            (D3DKMDT_VIDPN_PRESENT_PATH_SCALING_SUPPORT){.Identity = 1};
    if (!fixed_rotation(rotation) &&
        (args->EnumPivotType != D3DKMDT_EPT_ROTATION || !path_pivot)) {
        update.ContentTransformation.RotationSupport =
            (D3DKMDT_VIDPN_PRESENT_PATH_ROTATION_SUPPORT){.Identity = 1,
                .Rotate90 = 1, .Rotate180 = 1, .Rotate270 = 1, .Offset0 = 1};
    }
    status = path.Topology->pfnUpdatePathSupportInfo(path.Handle, &update);
done:
    {
        NTSTATUS release = path.Topology->pfnReleasePathInfo(path.Handle, path.Path);
        return NT_SUCCESS(status) ? release : status;
    }
}

static NTSTATUS validate_commit(TG_GPU_ADAPTER *adapter, D3DKMDT_HVIDPN vidpn,
    bool inherited, bool *attached, D3DKMDT_VIDPN_PRESENT_PATH_ROTATION *rotation)
{
    PANEL_PATH path = {0};
    NTSTATUS status = acquire_path(adapter, vidpn, &path);
    if (!NT_SUCCESS(status))
        return status;
    *attached = path.Path != NULL;
    if (rotation)
        *rotation = *attached ? path_rotation(path.Path) : D3DKMDT_VPPR_IDENTITY;
    if (!*attached)
        return STATUS_SUCCESS;

    if (!supported_path(&adapter->Panel, path.Path)) {
        status = STATUS_GRAPHICS_INVALID_VIDPN_TOPOLOGY;
    } else {
        enum panel_mode_state source, target;
        status = pinned_source(path.VidPn, vidpn, &source);
        if (NT_SUCCESS(status))
            status = pinned_target(path.VidPn, vidpn, path.Path->VidPnTargetId, &target);
        if (NT_SUCCESS(status) && (source == PanelModeUnsupported ||
            target == PanelModeUnsupported ||
            (!inherited && (source == PanelModeUnpinned || target != PanelModeNative))))
            status = STATUS_GRAPHICS_INVALID_VIDPN_TOPOLOGY;
    }
    NTSTATUS release = path.Topology->pfnReleasePathInfo(path.Handle, path.Path);
    return NT_SUCCESS(status) ? release : status;
}

static NTSTATUS APIENTRY recommend_functional(HANDLE context,
    const DXGKARG_RECOMMENDFUNCTIONALVIDPN *args)
{
    PAGED_CODE();
    if (!context || !args)
        return STATUS_INVALID_PARAMETER;
    /* DxgKrnl builds the initial topology from the single panel and the
     * preferred modes enumerated by the child and cofunctional DDIs.
     */
    return STATUS_GRAPHICS_NO_RECOMMENDED_FUNCTIONAL_VIDPN;
}

static NTSTATUS commit_vidpn_impl(HANDLE context, const DXGKARG_COMMITVIDPN *args)
{
    TG_GPU_ADAPTER *adapter = context;
    PAGED_CODE();
    if (!adapter || !args)
        return STATUS_INVALID_PARAMETER;
    if (args->AffectedVidPnSourceId != 0 && args->AffectedVidPnSourceId != D3DDDI_ID_ALL)
        return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE;
    bool attached;
    D3DKMDT_VIDPN_PRESENT_PATH_ROTATION rotation;
    NTSTATUS status = validate_commit(adapter, args->hFunctionalVidPn, false, &attached, &rotation);
    if (!NT_SUCCESS(status))
        return status;
    /* DSI timings do not rotate. SetVidPnSourceAddress programs the bound
     * primary's dimensions/pitch/orientation atomically, before or after this
     * modeset. Never reinterpret a previous primary using the new path angle. */
    status = TgGpuPanelSetTargetActive(adapter, attached && !args->Flags.PathPoweredOff);
    if (NT_SUCCESS(status))
        adapter->Panel.PathRotation = rotation;
    return status;
}

static NTSTATUS set_timings_impl(HANDLE context, DXGKARG_SETTIMINGSFROMVIDPN *args)
{
    TG_GPU_ADAPTER *adapter = context;
    PAGED_CODE();
    if (!adapter || !args || !args->pResultsFlags || args->SetFlags.Value || args->PathCount > 1 ||
        (args->PathCount && !args->pSetTimingPathInfo))
        return STATUS_INVALID_PARAMETER;
    args->pResultsFlags->Value = 0;
    if (!args->PathCount)
        return STATUS_SUCCESS;
    DXGK_SET_TIMING_PATH_INFO *path = args->pSetTimingPathInfo;
    if (path->VidPnTargetId != adapter->Panel.Boot.TargetId)
        return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_TARGET;
    if (path->Input.SyncLockGroup || path->Input.SyncLockStyle)
        return STATUS_NOT_SUPPORTED;

    bool attached = false;
    D3DKMDT_VIDPN_PRESENT_PATH_ROTATION rotation = adapter->Panel.PathRotation;
    if (path->Input.Active) {
        D3DKMDT_WIRE_FORMAT_AND_PREFERENCE selected = path->SelectedWireFormat;
        selected.Preference = 0; /* The OS reserves preference in this callback. */
        if (!path->Input.PreserveInherited && (selected.Value != panel_wire_format.Value ||
            path->OutputWireColorSpace != D3DDDI_OUTPUT_WIRE_COLOR_SPACE_G22_P709))
            return STATUS_INVALID_PARAMETER;

        NTSTATUS status = validate_commit(adapter, args->hFunctionalVidPn,
            path->Input.PreserveInherited != 0, &attached, &rotation);
        if (!NT_SUCCESS(status))
            return status;
        if (!attached && !path->Input.PreserveInherited)
            return STATUS_GRAPHICS_INVALID_VIDPN_TOPOLOGY;
    }
    NTSTATUS status = TgGpuPanelSetTargetActive(adapter, path->Input.Active != 0);
    if (NT_SUCCESS(status)) {
        /* This modeset replaces CommitVidPn. Remember the accepted path for
         * later UpdateActiveVidPnPresentPath calls, without reinterpreting
         * the current scanout. PreserveInherited may supply no new path. */
        if (attached)
            adapter->Panel.PathRotation = rotation;
        path->OutputFlags = 0;
        /* SetTimings completes synchronously. DxgKrnl requires a nonzero
         * change ID and a link-configuration result even when the DDI itself
         * returns success. One sequence keeps IDs unique across targets. */
        path->TargetState = (DXGK_CONNECTION_CHANGE){
            .ConnectionChangeId = (ULONGLONG)InterlockedIncrement64(&timing_change_id),
            .TargetId = path->VidPnTargetId,
            .ConnectionStatus = TgGpuLinkConfigurationSucceeded,
        };
        /* NONE / SEAMLESS / NONE; these UINT8 enum values are C++-only too. */
        path->DiagnosticInfo = 0x00ffffff;
    }
    return status;
}

static NTSTATUS source_visibility_impl(HANDLE context,
    const DXGKARG_SETVIDPNSOURCEVISIBILITY *args)
{
    PAGED_CODE();
    if (!context || !args)
        return STATUS_INVALID_PARAMETER;
    if (args->VidPnSourceId != 0)
        return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE;
    return TgGpuPanelSetVisibility(context, args->Visible);
}

static NTSTATUS update_active_path_impl(HANDLE context,
    const DXGKARG_UPDATEACTIVEVIDPNPRESENTPATH *args)
{
    TG_GPU_ADAPTER *adapter = context;
    PAGED_CODE();
    if (!adapter || !args)
        return STATUS_INVALID_PARAMETER;
    const D3DKMDT_VIDPN_PRESENT_PATH *path = &args->VidPnPresentPathInfo;
    if (!supported_path(&adapter->Panel, path))
        return STATUS_GRAPHICS_INVALID_VIDPN_TOPOLOGY;
    if (path->GammaRamp.Type != D3DDDI_GAMMARAMP_DEFAULT &&
        path->GammaRamp.Type != D3DDDI_GAMMARAMP_UNINITIALIZED)
        return STATUS_NOT_SUPPORTED;
    if (!adapter->Panel.Dc.owned)
        return STATUS_DEVICE_NOT_READY;
    const D3DKMDT_VIDPN_PRESENT_PATH_ROTATION rotation=path->ContentTransformation.Rotation;
    if (!fixed_rotation(rotation))
        return STATUS_GRAPHICS_PATH_CONTENT_GEOMETRY_TRANSFORMATION_NOT_SUPPORTED;
    /* All scanout allocations have the same portrait layout. A smooth path
     * change updates the angle consumed by future Rotate Blts; it neither
     * reinterprets old pixels nor disables output. DWM/GPU completion and
     * the subsequent actual primary latch still control frame retirement. */
    adapter->Panel.PathRotation=rotation;
    return STATUS_SUCCESS;
}

static void trace_display(const TG_GPU_ADAPTER *adapter, ULONG operation, ULONG phase,
    NTSTATUS status, ULONG before, ULONG path, ULONG rotation, ULONG flags)
{
    if (!adapter) return;
    const TG_GPU_DISPLAY_EVENT event={.Version=1,.Operation=operation,
        .AdapterLuid=adapter->Engine.AdapterLuid,.Status=status,.Phase=phase,
        .PathId=path,.RequestedRotation=rotation,.BeforeRotation=before,
        .AfterRotation=adapter->Panel.PathRotation,
        .SourceVisible=adapter->Panel.SourceVisible,.TargetActive=adapter->Panel.TargetActive,.Flags=flags};
    TgGpuTraceDisplay(&event);
}

/* Pair every return, including validation failures, with its real entry.
 * No topology queries, MMIO, disk writes or extra GPU commands for tracing. */
#define DISPLAY_WRAPPER(name, type, operation, path_id, rotation, input_flags) \
static NTSTATUS APIENTRY name(HANDLE context, type *args) \
{ \
    const TG_GPU_ADAPTER *adapter=context; \
    const ULONG before=adapter?adapter->Panel.PathRotation:0; \
    const ULONG path=args?(path_id):0, angle=args?(rotation):0, flags=args?(input_flags):0; \
    trace_display(adapter,operation,0,STATUS_PENDING,before,path,angle,flags); \
    NTSTATUS status=name##_impl(context,args); \
    trace_display(adapter,operation,1,status,before,path,angle,flags); \
    return status; \
}
DISPLAY_WRAPPER(commit_vidpn,const DXGKARG_COMMITVIDPN,TgGpuDisplayCommit,
    args->AffectedVidPnSourceId,0,args->Flags.PathPowerTransition | (args->Flags.PathPoweredOff<<1))
DISPLAY_WRAPPER(set_timings,DXGKARG_SETTIMINGSFROMVIDPN,TgGpuDisplaySetTimings,
    args->PathCount && args->pSetTimingPathInfo ? args->pSetTimingPathInfo[0].VidPnTargetId : 0,
    0,args->SetFlags.Value)
DISPLAY_WRAPPER(update_active_path,const DXGKARG_UPDATEACTIVEVIDPNPRESENTPATH,TgGpuDisplayUpdatePath,
    args->VidPnPresentPathInfo.VidPnSourceId,args->VidPnPresentPathInfo.ContentTransformation.Rotation,
    args->VidPnPresentPathInfo.ContentTransformation.Scaling)
DISPLAY_WRAPPER(source_visibility,const DXGKARG_SETVIDPNSOURCEVISIBILITY,TgGpuDisplayVisibility,
    args->VidPnSourceId,0,args->Visible)
#undef DISPLAY_WRAPPER

void TgGpuRegisterPanelVidPn(DRIVER_INITIALIZATION_DATA *ddis)
{
    ddis->DxgkDdiIsSupportedVidPn = is_supported;
    ddis->DxgkDdiQueryVidPnHWCapability = query_hardware_capability;
    ddis->DxgkDdiEnumVidPnCofuncModality = enumerate_modes;
    ddis->DxgkDdiRecommendFunctionalVidPn = recommend_functional;
    ddis->DxgkDdiCommitVidPn = commit_vidpn;
    ddis->DxgkDdiSetTimingsFromVidPn = set_timings;
    ddis->DxgkDdiSetVidPnSourceVisibility = source_visibility;
    ddis->DxgkDdiUpdateActiveVidPnPresentPath = update_active_path;
}
