/* SPDX-License-Identifier: MIT */
#include "adapter.h"
#include "allocation.h"
#include "../platform/t214_resources.h"

const D3DKMDT_VIDEO_SIGNAL_INFO TgGpuPanelSignal = {
    .VideoStandard = D3DKMDT_VSS_OTHER,
    .TotalSize = {1000, 1300},
    .ActiveSize = {T214_DC_PANEL_WIDTH, T214_DC_PANEL_HEIGHT},
    .VSyncFreq = {60, 1},
    .HSyncFreq = {78000, 1},
    .PixelRate = 78000000,
    .AdditionalSignalInfo = {.ScanLineOrdering = D3DDDI_VSSLO_PROGRESSIVE},
};

static uint32_t read_register(void *registers, uint32_t word)
{
    return READ_REGISTER_ULONG((PULONG)registers + word);
}

static void write_register(void *registers, uint32_t word, uint32_t value)
{
    WRITE_REGISTER_ULONG((PULONG)registers + word, value);
}

typedef struct _PANEL_ACTION {
    TG_GPU_ADAPTER *Adapter;
    enum tg_t214_dc_result Result;
    BOOLEAN Visible, TargetGate;
} PANEL_ACTION;

NTSTATUS TgGpuPanelSynchronize(TG_GPU_ADAPTER *adapter, PKSYNCHRONIZE_ROUTINE routine,
    PVOID context)
{
    const DXGKRNL_INTERFACE *dxgk = &adapter->Engine.Dxgk;
    BOOLEAN invoked = FALSE;
    NTSTATUS status = dxgk->DxgkCbSynchronizeExecution(dxgk->DeviceHandle,
        routine, context, 0, &invoked);
    return NT_SUCCESS(status) && !invoked ? STATUS_UNSUCCESSFUL : status;
}

static BOOLEAN acquire_dc(PVOID context)
{
    PANEL_ACTION *action = context;
    TG_GPU_ADAPTER *adapter = action->Adapter;
    const struct tg_t214_dc_io io = {adapter->Resources.DisplayRegisters,
        read_register, write_register};
    action->Result = tg_t214_dc_acquire(&adapter->Panel.Dc, &io);
    return TRUE;
}

NTSTATUS TgGpuPanelStart(TG_GPU_ADAPTER *adapter)
{
    PAGED_CODE();
    if (!adapter)
        return STATUS_INVALID_PARAMETER;
    if (adapter->Panel.Dc.owned)
        return STATUS_INVALID_DEVICE_STATE;
    RtlZeroMemory(&adapter->Panel, sizeof(adapter->Panel));
    if (!adapter->Resources.PanelAvailable || !adapter->Resources.DisplayRegisters)
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    const DXGKRNL_INTERFACE *dxgk = &adapter->Engine.Dxgk;
    if (!dxgk->DeviceHandle || !dxgk->DxgkCbAcquirePostDisplayOwnership ||
        !dxgk->DxgkCbSynchronizeExecution)
        return STATUS_INVALID_DEVICE_STATE;
    NTSTATUS status = TgGpuCreateScanoutSegment(dxgk, &adapter->Tables);
    if (!NT_SUCCESS(status))
        return status;
    DXGK_DISPLAY_INFORMATION boot = {0};
    status = dxgk->DxgkCbAcquirePostDisplayOwnership(dxgk->DeviceHandle, &boot);
    if (!NT_SUCCESS(status))
        return status;
    if (boot.Width != T214_DC_PANEL_WIDTH || boot.Height != T214_DC_PANEL_HEIGHT ||
        boot.Pitch != T214_DC_PANEL_PITCH || boot.PhysicAddress.QuadPart != T214_GPU_POST_FB_BASE ||
        (boot.ColorFormat != D3DDDIFMT_X8R8G8B8 && boot.ColorFormat != D3DDDIFMT_A8R8G8B8))
        return STATUS_DEVICE_CONFIGURATION_ERROR;

    PANEL_ACTION action = {.Adapter = adapter};
    status = TgGpuPanelSynchronize(adapter, acquire_dc, &action);
    if (!NT_SUCCESS(status))
        return status;
    if (action.Result != TG_T214_DC_OK)
        return action.Result == TG_T214_DC_BUSY ? STATUS_DEVICE_BUSY :
            STATUS_DEVICE_CONFIGURATION_ERROR;
    if (boot.TargetId == D3DDDI_ID_UNINITIALIZED)
        boot.TargetId = 0;
    adapter->Panel.Boot = boot;
    adapter->Panel.PathRotation = D3DKMDT_VPPR_IDENTITY;
    adapter->Panel.SourceVisible = TRUE;
    adapter->Panel.TargetActive = TRUE;
    adapter->Panel.Displayed = (TG_GPU_PANEL_FLIP){
        .Address = (UINT64)boot.PhysicAddress.QuadPart,
        .Visible = TRUE,
    };
    return STATUS_SUCCESS;
}

static BOOLEAN restore_dc(PVOID context)
{
    PANEL_ACTION *action = context;
    TG_GPU_PANEL *panel = &action->Adapter->Panel;
    struct tg_t214_dc *dc = &panel->Dc;
    panel->Stopping = TRUE;
    tg_t214_dc_set_vblank(dc, false);
    struct tg_t214_dc_scanout scanout;
    action->Result = TG_T214_DC_BUSY;
    if (tg_t214_dc_latched_surface(dc, &scanout)) {
        if (scanout.address == T214_GPU_POST_FB_BASE && scanout.visible) {
            action->Result = tg_t214_dc_release(dc);
        } else {
            enum tg_t214_dc_result result = tg_t214_dc_queue_scanout(dc, T214_GPU_POST_FB_BASE, true);
            if (result != TG_T214_DC_OK)
                action->Result = result;
        }
    }
    return TRUE;
}

static BOOLEAN set_visibility(PVOID context)
{
    PANEL_ACTION *action = context;
    struct tg_t214_dc *dc = &action->Adapter->Panel.Dc;
    TG_GPU_PANEL *panel = &action->Adapter->Panel;
    struct tg_t214_dc_scanout active;
    action->Result = TG_T214_DC_BUSY;
    /* A modeset/target-power update is not a source-visibility request.
     * Preserve the other gate across either callback ordering. */
    if (action->TargetGate)
        panel->TargetActive = action->Visible != FALSE;
    else
        panel->SourceVisible = action->Visible != FALSE;
    const BOOLEAN visible = panel->SourceVisible && panel->TargetActive;
    if (!TgGpuPanelReadLatched(panel, &active))
        return TRUE;
    if (active.visible == (visible != FALSE)) {
        action->Result = TG_T214_DC_OK;
        return TRUE;
    }
    /* Visibility starts/stops the assigned primary; it does not assign POST.
     * Disabled Window A performs no fetch. VidMm owns residency and supplies
     * address updates when the primary backing changes, including while hidden.
     * Only the stop/POST-handoff path deliberately replaces this binding. */
    active.visible = visible != FALSE;
    enum tg_t214_dc_result result = tg_t214_dc_queue_surface(dc, &active, false);
    if (result == TG_T214_DC_OK) {
        /* This is the same binding and completed PresentId, with only its
         * enable bit changing. Preserve that receipt after the visibility
         * update latches instead of classifying our own update as external. */
        panel->Pending = panel->Displayed;
        panel->Pending.Visible = visible;
        panel->Pending.Immediate = FALSE;
    } else {
        action->Result = result;
    }
    return TRUE;
}

static NTSTATUS wait_for_panel(PANEL_ACTION *action, PKSYNCHRONIZE_ROUTINE routine)
{
    const ULONGLONG deadline = KeQueryInterruptTime() + 5000000;
    LARGE_INTEGER delay = {.QuadPart = -10000};
    do {
        NTSTATUS status = TgGpuPanelSynchronize(action->Adapter, routine, action);
        if (!NT_SUCCESS(status))
            return status;
        if (action->Result == TG_T214_DC_OK)
            return STATUS_SUCCESS;
        if (action->Result != TG_T214_DC_BUSY)
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        (VOID)KeDelayExecutionThread(KernelMode, FALSE, &delay);
    } while (KeQueryInterruptTime() < deadline);
    return STATUS_IO_TIMEOUT;
}

NTSTATUS TgGpuPanelStop(TG_GPU_ADAPTER *adapter)
{
    PAGED_CODE();
    if (!adapter)
        return STATUS_INVALID_PARAMETER;
    if (!adapter->Panel.Dc.owned)
        return STATUS_SUCCESS;
    PANEL_ACTION action = {.Adapter = adapter};
    return wait_for_panel(&action, restore_dc);
}

NTSTATUS TgGpuPanelSetVisibility(TG_GPU_ADAPTER *adapter, BOOLEAN visible)
{
    PAGED_CODE();
    if (!adapter->Panel.Dc.owned)
        return STATUS_DEVICE_NOT_READY;
    PANEL_ACTION action = {.Adapter = adapter, .Visible = visible};
    return wait_for_panel(&action, set_visibility);
}

NTSTATUS TgGpuPanelSetTargetActive(TG_GPU_ADAPTER *adapter, BOOLEAN active)
{
    PAGED_CODE();
    if (!adapter->Panel.Dc.owned)
        return STATUS_DEVICE_NOT_READY;
    PANEL_ACTION action = {.Adapter = adapter, .Visible = active, .TargetGate = TRUE};
    return wait_for_panel(&action, set_visibility);
}

static NTSTATUS APIENTRY stop_device(PVOID context)
{
    NTSTATUS status = TgGpuPanelStop(context);
    return NT_SUCCESS(status) ? TgGpuDdiStopDevice(context) : status;
}

static NTSTATUS APIENTRY remove_device(PVOID context)
{
    NTSTATUS status = TgGpuPanelStop(context);
    return NT_SUCCESS(status) ? TgGpuDdiRemoveDevice(context) : status;
}

static NTSTATUS stop_and_release(PVOID context, D3DDDI_VIDEO_PRESENT_TARGET_ID target,
    PDXGK_DISPLAY_INFORMATION info)
{
    TG_GPU_ADAPTER *adapter = context;
    if (!adapter || !info)
        return STATUS_INVALID_PARAMETER;
    if (!adapter->Panel.Boot.Width || target != adapter->Panel.Boot.TargetId)
        return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_TARGET;
    const DXGK_DISPLAY_INFORMATION boot = adapter->Panel.Boot;
    NTSTATUS status = stop_device(context);
    if (NT_SUCCESS(status))
        *info = boot;
    return status;
}

static NTSTATUS query_children(PVOID context, PDXGK_CHILD_DESCRIPTOR children, ULONG bytes)
{
    TG_GPU_ADAPTER *adapter = context;
    if (!adapter || !children)
        return STATUS_INVALID_PARAMETER;
    if (!adapter->Panel.ChildReported)
        return STATUS_DEVICE_NOT_READY;
    /* One reported child requires its descriptor and a zero terminator. */
    if (bytes < 2 * sizeof(*children))
        return STATUS_BUFFER_TOO_SMALL;
    RtlZeroMemory(children, 2 * sizeof(*children));
    children[0].ChildDeviceType = TypeVideoOutput;
    children[0].ChildCapabilities.HpdAwareness = HpdAwarenessAlwaysConnected;
    children[0].ChildCapabilities.Type.VideoOutput.InterfaceTechnology = D3DKMDT_VOT_INTERNAL;
    children[0].ChildCapabilities.Type.VideoOutput.MonitorOrientationAwareness = D3DKMDT_MOA_NONE;
    children[0].ChildUid = adapter->Panel.Boot.TargetId;
    children[0].AcpiUid = adapter->Panel.Boot.AcpiId;
    return STATUS_SUCCESS;
}

static NTSTATUS query_child_status(PVOID context, PDXGK_CHILD_STATUS status, BOOLEAN nondestructive)
{
    TG_GPU_ADAPTER *adapter = context;
    UNREFERENCED_PARAMETER(nondestructive);
    if (!adapter || !status)
        return STATUS_INVALID_PARAMETER;
    if (!adapter->Panel.ChildReported)
        return STATUS_DEVICE_NOT_READY;
    if (status->ChildUid != adapter->Panel.Boot.TargetId)
        return STATUS_INVALID_PARAMETER;
    if (status->Type != StatusConnection)
        return STATUS_NOT_SUPPORTED;
    status->HotPlug.Connected = TRUE;
    return STATUS_SUCCESS;
}

static NTSTATUS query_device_descriptor(PVOID context, ULONG uid, PDXGK_DEVICE_DESCRIPTOR descriptor)
{
    const TG_GPU_ADAPTER *adapter = context;
    PAGED_CODE();
    if (!adapter || !descriptor)
        return STATUS_INVALID_PARAMETER;
    if (!adapter->Panel.ChildReported)
        return STATUS_DEVICE_NOT_READY;
    if (uid != adapter->Panel.Boot.TargetId)
        return STATUS_INVALID_PARAMETER;
    /* The internal DSI panel supplies its mode through RecommendMonitorModes. */
    return STATUS_MONITOR_NO_DESCRIPTOR;
}

static NTSTATUS APIENTRY recommend_monitor_modes(HANDLE context, const DXGKARG_RECOMMENDMONITORMODES *args)
{
    TG_GPU_ADAPTER *adapter = context;
    PAGED_CODE();
    if (!adapter || !args || !args->pMonitorSourceModeSetInterface)
        return STATUS_INVALID_PARAMETER;
    if (!adapter->Panel.ChildReported || args->VideoPresentTargetId != adapter->Panel.Boot.TargetId)
        return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_TARGET;
    const DXGK_MONITORSOURCEMODESET_INTERFACE *modes = args->pMonitorSourceModeSetInterface;
    D3DKMDT_MONITOR_SOURCE_MODE *mode;
    NTSTATUS status = modes->pfnCreateNewModeInfo(args->hMonitorSourceModeSet, &mode);
    if (!NT_SUCCESS(status))
        return status;
    mode->VideoSignalInfo = TgGpuPanelSignal;
    mode->ColorBasis = D3DKMDT_CB_SRGB;
    mode->ColorCoeffDynamicRanges = (D3DKMDT_COLOR_COEFF_DYNAMIC_RANGES){8, 8, 8, 0};
    mode->Origin = D3DKMDT_MCO_DRIVER;
    mode->Preference = D3DKMDT_MP_PREFERRED;
    status = modes->pfnAddMode(args->hMonitorSourceModeSet, mode);
    if (!NT_SUCCESS(status))
        (VOID)modes->pfnReleaseModeInfo(args->hMonitorSourceModeSet, mode);
    return status == STATUS_GRAPHICS_MODE_ALREADY_IN_MODESET ? STATUS_SUCCESS : status;
}

static NTSTATUS APIENTRY update_monitor_link_info(HANDLE context, DXGKARG_UPDATEMONITORLINKINFO *args)
{
    const TG_GPU_ADAPTER *adapter = context;
    PAGED_CODE();
    if (!adapter || !args)
        return STATUS_INVALID_PARAMETER;
    if (!adapter->Panel.ChildReported || args->VideoPresentTargetId != adapter->Panel.Boot.TargetId)
        return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_TARGET;

    /* The inherited DSI link runs fixed-rate SDR without link dithering. */
    args->MonitorLinkInfo.Capabilities.Value = 0;
    args->MonitorLinkInfo.DitheringSupport = (D3DKMDT_WIRE_FORMAT_AND_PREFERENCE){0};
    return STATUS_SUCCESS;
}


void TgGpuRegisterPanelLifecycle(DRIVER_INITIALIZATION_DATA *ddis)
{
    ddis->DxgkDdiStopDevice = stop_device;
    ddis->DxgkDdiRemoveDevice = remove_device;
    ddis->DxgkDdiStopDeviceAndReleasePostDisplayOwnership = stop_and_release;
    ddis->DxgkDdiQueryChildRelations = query_children;
    ddis->DxgkDdiQueryChildStatus = query_child_status;
    ddis->DxgkDdiQueryDeviceDescriptor = query_device_descriptor;
    ddis->DxgkDdiRecommendMonitorModes = recommend_monitor_modes;
    ddis->DxgkDdiUpdateMonitorLinkInfo = update_monitor_link_info;
    ddis->DxgkDdiGetStandardAllocationDriverData = TgGpuDdiGetStandardAllocationDriverData;
    TgGpuRegisterPanelVidPn(ddis);
    TgGpuRegisterPanelScanout(ddis);
}
