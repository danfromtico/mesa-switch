/* SPDX-License-Identifier: MIT */
#include "adapter.h"
#include "allocation.h"
#include "panel_snapshot.h"
#include "../platform/t214_resources.h"

typedef struct _FLIP_ACTION {
    TG_GPU_PANEL *Panel;
    TG_GPU_PANEL_FLIP Flip;
    NTSTATUS Status;
} FLIP_ACTION;

static bool matches(const TG_GPU_PANEL_FLIP *flip, const struct tg_t214_dc_scanout *active)
{
    return flip->Address == active->address &&
        (flip->Visible != FALSE) == active->visible;
}

BOOLEAN TgGpuPanelReadLatched(TG_GPU_PANEL *panel, struct tg_t214_dc_scanout *active)
{
    if (!tg_t214_dc_latched_surface(&panel->Dc, active))
        return FALSE;
    if (panel->Pending.Address && matches(&panel->Pending, active)) {
        panel->Displayed = panel->Pending;
        panel->Pending = (TG_GPU_PANEL_FLIP){0};
    } else if (!matches(&panel->Displayed, active)) {
        /* Unknown/external state must not inherit a completed PresentId. */
        panel->Displayed = (TG_GPU_PANEL_FLIP){.Address = active->address,
            .Visible = active->visible};
    }
    return TRUE;
}

static BOOLEAN program_flip(PVOID context)
{
    FLIP_ACTION *action = context;
    TG_GPU_PANEL *panel = action->Panel;
    if (!panel->Dc.owned || panel->Stopping) {
        action->Status = STATUS_DEVICE_NOT_READY;
        return TRUE;
    }
    struct tg_t214_dc_scanout active;
    const BOOLEAN latched = TgGpuPanelReadLatched(panel, &active);
    if (panel->Pending.Address) {
        action->Status = STATUS_DEVICE_BUSY;
        return TRUE;
    }
    TG_GPU_PANEL_FLIP flip = action->Flip;
    if (!flip.Address) {
        /* A disabled MPO plane supplies no allocation. Preserve its binding
         * so source visibility can re-enable the primary without another flip. */
        if (flip.Visible || !latched) {
            action->Status = STATUS_INVALID_PARAMETER;
            return TRUE;
        }
        flip.Address = active.address;
    }
    flip.Visible = flip.Visible && panel->SourceVisible && panel->TargetActive;
    if (flip.Immediate && latched && matches(&flip, &active) &&
        panel->Displayed.PresentId==flip.PresentId) {
        action->Status=STATUS_SUCCESS;
        return TRUE;
    }
    const struct tg_t214_dc_scanout surface = {flip.Address, flip.Visible != FALSE};
    enum tg_t214_dc_result result = tg_t214_dc_queue_surface(&panel->Dc, &surface,
        flip.Immediate != FALSE);
    if (result == TG_T214_DC_OK) {
        panel->Pending = flip;
        action->Flip = flip; /* Confirm the effective visibility after queuing. */
        action->Status = flip.Immediate ? STATUS_PENDING : STATUS_SUCCESS;
    } else {
        action->Status = result == TG_T214_DC_BUSY ? STATUS_DEVICE_BUSY : STATUS_INVALID_PARAMETER;
    }
    return TRUE;
}

static BOOLEAN confirm_flip(PVOID context)
{
    FLIP_ACTION *action=context;
    struct tg_t214_dc_scanout active;
    action->Status=STATUS_PENDING;
    if (TgGpuPanelReadLatched(action->Panel, &active) && matches(&action->Flip, &active)) {
        action->Panel->Displayed=action->Flip;
        action->Panel->Pending=(TG_GPU_PANEL_FLIP){0};
        action->Status=STATUS_SUCCESS;
    }
    return TRUE;
}

static NTSTATUS flip(TG_GPU_ADAPTER *adapter, TG_GPU_PANEL_FLIP scanout, BOOLEAN mode_change)
{
    FLIP_ACTION action = {&adapter->Panel, scanout, STATUS_UNSUCCESSFUL};
    /* A modeset has no scheduled flip receipt to retire the previous primary.
     * Keep the PASSIVE-level call alive until its address and layout are active.
     * Ordinary MMIO flips retain their asynchronous VSync notification path. */
    if (mode_change && KeGetCurrentIrql() != PASSIVE_LEVEL)
        return STATUS_INVALID_PARAMETER;
    /* DxgKrnl calls the fast flip path in the interrupt synchronization
     * class. Legacy modeset and pre-present calls can arrive at lower IRQL.
     */
    if (KeGetCurrentIrql() > DISPATCH_LEVEL) {
        (VOID)program_flip(&action);
        return action.Status;
    }
    NTSTATUS status = TgGpuPanelSynchronize(adapter, program_flip, &action);
    if (!NT_SUCCESS(status))return status;
    if (!mode_change && (!scanout.Immediate || action.Status!=STATUS_PENDING))
        return action.Status;
    BOOLEAN queued = action.Status == STATUS_SUCCESS || action.Status == STATUS_PENDING;
    if (!queued && (!mode_change || action.Status != STATUS_DEVICE_BUSY))
        return action.Status;
    /* Immediate MPO requests are recalled at PASSIVE before queuing. Keep
     * their DDI lifetime until the active bank confirms completion. Address
     * changes usually take one scanline; layout/visibility changes require VBlank.
     * A modeset also drains a previously queued flip before replacing it. Its
     * Immediate bit remains clear, so even address-only modesets use VBlank. */
    const ULONGLONG deadline=KeQueryInterruptTime()+500000;
    LARGE_INTEGER interval={.QuadPart=-10000};
    for (UINT i=0;KeQueryInterruptTime()<deadline;++i) {
        if(i<100)KeStallExecutionProcessor(1);
        else (VOID)KeDelayExecutionThread(KernelMode,FALSE,&interval);
        status=TgGpuPanelSynchronize(adapter,queued ? confirm_flip : program_flip,&action);
        if(!NT_SUCCESS(status))return status;
        if (queued) {
            if(action.Status==STATUS_SUCCESS)return STATUS_SUCCESS;
        } else if (action.Status == STATUS_SUCCESS || action.Status == STATUS_PENDING) {
            queued = TRUE; /* Queuing is not a latch/completion receipt. */
        } else if (action.Status != STATUS_DEVICE_BUSY) {
            return action.Status;
        }
    }
    return mode_change ? STATUS_IO_TIMEOUT : STATUS_INVALID_PARAMETER;
}

static bool storage_matches(const TG_GPU_PRIMARY_ALLOCATION_REQUEST *allocation)
{
    return allocation->Rotation == D3DDDI_ROTATION_IDENTITY &&
        allocation->Width == T214_DC_PANEL_WIDTH && allocation->Height == T214_DC_PANEL_HEIGHT &&
        allocation->Pitch == T214_DC_PANEL_PITCH;
}

static NTSTATUS APIENTRY set_primary(HANDLE context, const DXGKARG_SETVIDPNSOURCEADDRESS *args)
{
    TG_GPU_ADAPTER *adapter = context;
    if (!adapter || !args || args->VidPnSourceId != 0)
        return STATUS_INVALID_PARAMETER;
    if (args->Flags.FlipImmediate || args->Flags.FlipStereo || args->ContextCount > 1)
        return STATUS_NOT_SUPPORTED;
    TG_GPU_PANEL_FLIP scanout = {.Address = T214_GPU_POST_FB_BASE, .Visible = TRUE};
    if (args->hAllocation) {
        if (args->PrimarySegment != TG_GPU_SCANOUT_SEGMENT)
            return STATUS_NOT_SUPPORTED;
        TG_GPU_PRIMARY_ALLOCATION_REQUEST allocation;
        NTSTATUS status = TgGpuGetScanoutSurface(&adapter->Engine, args->hAllocation, &allocation);
        if (!NT_SUCCESS(status))
            return status;
        if (!storage_matches(&allocation))
            return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE_MODE;
        status = TgGpuResolveScanoutAddress(&adapter->Tables,
            (UINT64)args->PrimaryAddress.QuadPart, T214_GPU_POST_FB_SIZE, &scanout.Address);
        if (!NT_SUCCESS(status))
            return status;
    } else if (!args->Flags.ModeChange) {
        return STATUS_INVALID_PARAMETER;
    }
    return flip(adapter, scanout, args->Flags.ModeChange != 0);
}

static bool full_surface(const RECT *rect, UINT width, UINT height)
{
    return rect->left == 0 && rect->top == 0 &&
        rect->right == (LONG)width && rect->bottom == (LONG)height;
}

static bool native_plane(const DXGK_MULTIPLANE_OVERLAY_ATTRIBUTES3 *plane,
    const TG_GPU_PRIMARY_ALLOCATION_REQUEST *allocation)
{
    /* The UMD has already rotated pixels into portrait storage. */
    return plane->Rotation == D3DDDI_ROTATION_IDENTITY && storage_matches(allocation) &&
        !plane->Flags.Value &&
        full_surface(&plane->SrcRect, allocation->Width, allocation->Height) &&
        full_surface(&plane->DstRect, T214_DC_PANEL_WIDTH, T214_DC_PANEL_HEIGHT) &&
        (full_surface(&plane->ClipRect, T214_DC_PANEL_WIDTH, T214_DC_PANEL_HEIGHT) ||
            (!plane->ClipRect.left && !plane->ClipRect.top &&
             !plane->ClipRect.right && !plane->ClipRect.bottom)) &&
        !plane->Blend.Value &&
        plane->ColorSpaceType == D3DDDI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
}

static NTSTATUS APIENTRY set_primary_mpo(HANDLE context,
    DXGKARG_SETVIDPNSOURCEADDRESSWITHMULTIPLANEOVERLAY3 *args)
{
    TG_GPU_ADAPTER *adapter = context;
    if (!adapter || !args || args->VidPnSourceId != 0 || args->PlaneCount > 1 ||
        (args->PlaneCount && (!args->ppPlanes || !args->ppPlanes[0])))
        return STATUS_INVALID_PARAMETER;
    args->OutputFlags.Value = 0;
    if (args->InputFlags.FlipStereo || args->pPostComposition || args->pHDRMetaData)
        return STATUS_NOT_SUPPORTED;
    if (!args->PlaneCount)
        return STATUS_SUCCESS; /* Omitted planes keep their existing state. */

    DXGK_MULTIPLANE_OVERLAY_PLANE3 *plane = args->ppPlanes[0];
    plane->OutputFlags.Value = 0;
    if (plane->LayerIndex != 0 || plane->InputFlags.FlipImmediateNoTearing)
        return STATUS_INVALID_PARAMETER;
    TG_GPU_PANEL_FLIP scanout = {
        .PresentId = plane->PresentId,
        .Visible = plane->InputFlags.Enabled != 0,
        .Immediate = plane->InputFlags.FlipImmediate != 0,
    };
    if (scanout.Immediate && KeGetCurrentIrql()!=PASSIVE_LEVEL) {
        args->OutputFlags.PrePresentNeeded=1;
        return STATUS_RETRY;
    }
    if (scanout.Visible) {
        if (plane->ContextCount != 1 ||
            !plane->ppContextData || !plane->ppContextData[0])
            return STATUS_INVALID_PARAMETER;
        const DXGK_PRIMARYCONTEXTDATA *primary = plane->ppContextData[0];
        if (!primary->hAllocation || primary->SegmentId != TG_GPU_SCANOUT_SEGMENT)
            return STATUS_NOT_SUPPORTED;
        TG_GPU_PRIMARY_ALLOCATION_REQUEST allocation;
        NTSTATUS status = TgGpuGetScanoutSurface(&adapter->Engine, primary->hAllocation, &allocation);
        if (!NT_SUCCESS(status))
            return status;
        if (!native_plane(&plane->PlaneAttributes, &allocation))
            return STATUS_INVALID_PARAMETER;
        status = TgGpuResolveScanoutAddress(&adapter->Tables,
            (UINT64)primary->SegmentAddress.QuadPart, T214_GPU_POST_FB_SIZE, &scanout.Address);
        if (!NT_SUCCESS(status))
            return status;
    }
    NTSTATUS status=flip(adapter, scanout, FALSE);
    return status==STATUS_DEVICE_BUSY?STATUS_RETRY:status;
}

static NTSTATUS APIENTRY check_mpo(HANDLE context, DXGKARG_CHECKMULTIPLANEOVERLAYSUPPORT3 *args)
{
    PAGED_CODE();
    if (!context || !args || (args->PlaneCount && !args->ppPlanes))
        return STATUS_INVALID_PARAMETER;
    args->ReturnInfo.Value = 0;
    args->Supported = FALSE;
    if (args->PostCompositionCount || args->PlaneCount > 1)
        return STATUS_SUCCESS;
    if (!args->PlaneCount) {
        args->Supported = TRUE;
        return STATUS_SUCCESS;
    }
    const DXGK_MULTIPLANE_OVERLAY_PLANE_WITH_SOURCE2 *plane = args->ppPlanes[0];
    if (plane && plane->hAllocation && !plane->LayerIndex && !plane->VidPnSourceId) {
        TG_GPU_ADAPTER *adapter = context;
        TG_GPU_PRIMARY_ALLOCATION_REQUEST allocation;
        if (NT_SUCCESS(TgGpuGetScanoutSurface(&adapter->Engine, plane->hAllocation, &allocation)))
            args->Supported = native_plane(&plane->PlaneAttributes, &allocation);
    }
    return STATUS_SUCCESS;
}

typedef struct _VBLANK_ACTION {
    TG_GPU_PANEL *Panel;
    DXGK_INTERRUPT_TYPE Type;
    BOOLEAN Enable;
} VBLANK_ACTION;

static BOOLEAN control_vblank(PVOID context)
{
    const VBLANK_ACTION *action = context;
    TG_GPU_PANEL *panel = action->Panel;
    if (action->Type == DXGK_INTERRUPT_CRTC_VSYNC)
        panel->NotifyVBlank = action->Enable;
    else
        panel->NotifyMpoVBlank = action->Enable;
    tg_t214_dc_set_vblank(&panel->Dc, !panel->Stopping &&
        (panel->NotifyVBlank || panel->NotifyMpoVBlank));
    return TRUE;
}

static NTSTATUS APIENTRY control_interrupt(HANDLE context, DXGK_INTERRUPT_TYPE type, BOOLEAN enable)
{
    TG_GPU_ADAPTER *adapter = context;
    PAGED_CODE();
    if (!adapter)
        return STATUS_INVALID_PARAMETER;
    if (type != DXGK_INTERRUPT_CRTC_VSYNC &&
        type != DXGK_INTERRUPT_CRTC_VSYNC_WITH_MULTIPLANE_OVERLAY2)
        return STATUS_NOT_SUPPORTED;
    if (!adapter->Panel.Dc.owned)
        return enable ? STATUS_DEVICE_NOT_READY : STATUS_SUCCESS;
    VBLANK_ACTION action = {&adapter->Panel, type, enable};
    return TgGpuPanelSynchronize(adapter, control_vblank, &action);
}

static BOOLEAN panel_interrupt(PVOID context, ULONG message)
{
    TG_GPU_ADAPTER *adapter = context;
    TG_GPU_PANEL *panel = &adapter->Panel;
    if (message)
        return FALSE;

    /* Acknowledge Display A VBlank first so a hung GPU PRI read can never
     * starve or delay 60 Hz scanout interrupts. */
    const BOOLEAN vblank = tg_t214_dc_ack_vblank(&panel->Dc);
    if (vblank) {
        struct tg_t214_dc_scanout active;
        (VOID)TgGpuPanelReadLatched(panel, &active);

        const DXGKRNL_INTERFACE *dxgk = &adapter->Engine.Dxgk;
        if (panel->NotifyMpoVBlank) {
            DXGK_MULTIPLANE_OVERLAY_VSYNC_INFO2 plane = {.PresentId = panel->Displayed.PresentId};
            DXGKARGCB_NOTIFY_INTERRUPT_DATA data = {
                .InterruptType = DXGK_INTERRUPT_CRTC_VSYNC_WITH_MULTIPLANE_OVERLAY2,
                .CrtcVsyncWithMultiPlaneOverlay2 = {
                    .VidPnTargetId = panel->Boot.TargetId,
                    .PhysicalAdapterMask = 1,
                    .MultiPlaneOverlayVsyncInfoCount = plane.PresentId ? 1 : 0,
                    .pMultiPlaneOverlayVsyncInfo = plane.PresentId ? &plane : NULL,
                },
                /* This is a real VBlank receipt, not an HSync-only completion.
                 * Mark the supplied physical-adapter mask as valid. */
                .Flags = {.ValidPhysicalAdapterMask = 1},
            };
            dxgk->DxgkCbNotifyInterrupt(dxgk->DeviceHandle, &data);
        }
        if (panel->NotifyVBlank) {
            const UINT64 displayed = panel->Displayed.Address;
            DXGKARGCB_NOTIFY_INTERRUPT_DATA data = {
                .InterruptType = DXGK_INTERRUPT_CRTC_VSYNC,
                .CrtcVsync = {
                    .VidPnTargetId = panel->Boot.TargetId,
                    .PhysicalAddress = {.QuadPart = (LONGLONG)displayed},
                },
            };
            dxgk->DxgkCbNotifyInterrupt(dxgk->DeviceHandle, &data);
        }
        if (panel->NotifyMpoVBlank || panel->NotifyVBlank)
            (VOID)dxgk->DxgkCbQueueDpc(dxgk->DeviceHandle);
        /* Display A VBlank (GSI 105) and GM20B Stall (GSI 189) are independent
         * level-high GIC SPIs (B03). Return immediately after handling VBlank
         * so 60 Hz scanout never probes GPU PRI in DIRQL; if GSI 189 is also
         * asserted, GIC re-delivers immediately after EOI. */
        return TRUE;
    }

    return TgGpuServiceFaultIsr(&adapter->Engine.FaultIsr, &adapter->Engine.Dxgk);
}

static NTSTATUS APIENTRY get_scanline(HANDLE context, DXGKARG_GETSCANLINE *args)
{
    const TG_GPU_ADAPTER *adapter = context;
    PAGED_CODE();
    if (!adapter || !args)
        return STATUS_INVALID_PARAMETER;
    if (args->VidPnTargetId != adapter->Panel.Boot.TargetId)
        return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_TARGET;
    /* VBlank IRQs prove frame boundaries. They do not read a raster counter. */
    return STATUS_NOT_SUPPORTED;
}

/* With zero hardware pointer caps, Windows composites the software cursor.
 * Keep the display DDI pair available for a hardware-pointer hide request.
 */
static NTSTATUS APIENTRY set_pointer_position(HANDLE context, const DXGKARG_SETPOINTERPOSITION *args)
{
    PAGED_CODE();
    if (!context || !args)
        return STATUS_INVALID_PARAMETER;
    if (args->VidPnSourceId != 0)
        return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE;
    return args->Flags.Visible ? STATUS_UNSUCCESSFUL : STATUS_SUCCESS;
}

static NTSTATUS APIENTRY set_pointer_shape(HANDLE context, const DXGKARG_SETPOINTERSHAPE *args)
{
    PAGED_CODE();
    if (!context || !args)
        return STATUS_INVALID_PARAMETER;
    if (args->VidPnSourceId != 0)
        return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE;
    return STATUS_NOT_IMPLEMENTED;
}

typedef struct _SNAPSHOT_ACTION {
    TG_GPU_ADAPTER *Adapter;
    struct tg_gpu_panel_snapshot Data;
    NTSTATUS Status;
} SNAPSHOT_ACTION;

static BOOLEAN snapshot_panel(PVOID context)
{
    SNAPSHOT_ACTION *action = context;
    TG_GPU_ADAPTER *adapter = action->Adapter;
    TG_GPU_PANEL *panel = &adapter->Panel;
    struct tg_gpu_panel_snapshot *s = &action->Data;
    if (!panel->Dc.owned || panel->Stopping) {
        action->Status = STATUS_DEVICE_NOT_READY;
        return TRUE;
    }
    const struct tg_t214_dc_io *io = &panel->Dc.io;
    s->luid_low = adapter->Engine.AdapterLuid.LowPart;
    s->luid_high = adapter->Engine.AdapterLuid.HighPart;
    s->path_rotation = panel->PathRotation;
    s->source_visible = panel->SourceVisible;
    s->target_active = panel->TargetActive;
    s->pending_address = panel->Pending.Address;
    s->pending_present = panel->Pending.PresentId;
    s->displayed_address = panel->Displayed.Address;
    s->displayed_present = panel->Displayed.PresentId;
    s->state_control_before = io->read32(io->context, T214_DC_STATE_CONTROL);
    const UINT access = io->read32(io->context, T214_DC_STATE_ACCESS);
    io->write32(io->context, T214_DC_STATE_ACCESS, access | T214_DC_READ_ACTIVE);
    s->address = (UINT64)io->read32(io->context, T214_DC_WIN_A_ADDR) |
        (UINT64)io->read32(io->context, T214_DC_WIN_A_ADDR_HI) << 32;
    s->options = io->read32(io->context, T214_DC_WIN_A_OPTIONS);
    s->pitch = io->read32(io->context, T214_DC_WIN_A_STRIDE) & 0xffffu;
    s->h_offset = io->read32(io->context, T214_DC_WIN_A_H_OFFSET);
    s->v_offset = io->read32(io->context, T214_DC_WIN_A_V_OFFSET);
    s->surface_kind = io->read32(io->context, T214_DC_WIN_A_SURFACE_KIND);
    s->output_size = io->read32(io->context, T214_DC_WIN_A_SIZE);
    s->prescaled_size = io->read32(io->context, T214_DC_WIN_A_PRESCALED);
    s->dda = io->read32(io->context, T214_DC_WIN_A_DDA);
    s->panel_active = io->read32(io->context, T214_DC_ACTIVE_SIZE);
    io->write32(io->context, T214_DC_STATE_ACCESS, access);
    (void)io->read32(io->context, T214_DC_STATE_ACCESS);
    s->interrupt_status = io->read32(io->context, T214_DC_INT_STATUS);
    s->underflow = io->read32(io->context, T214_DC_WIN_A_UNDERFLOW);
    /* Never acknowledge an interrupt, advance Pending or trigger an activation.
     * The display synchronization lock prevents other CPU writers; ACT_REQ
     * brackets catch an in-flight hardware bank transition. */
    s->state_control_after = io->read32(io->context, T214_DC_STATE_CONTROL);
    if (!((s->state_control_before | s->state_control_after) &
          (T214_DC_GENERAL_ACT_REQ | T214_DC_WIN_A_ACT_REQ)))
        s->flags |= TG_GPU_PANEL_SNAPSHOT_STABLE;
    if (!(s->options & T214_DC_WIN_TRANSFORM_BITS) && s->pitch == T214_DC_PANEL_PITCH &&
        !s->h_offset && !s->v_offset && !s->surface_kind) {
        s->rotation = 0; /* fixed native portrait scanout */
        s->width = T214_DC_PANEL_WIDTH;
        s->height = T214_DC_PANEL_HEIGHT;
        s->flags |= TG_GPU_PANEL_SNAPSHOT_LAYOUT;
    }
    const TG_GPU_PRIVATE_MEMORY *memory = &adapter->Tables.Scanout;
    const UINT64 base = (UINT64)memory->SystemPhysicalAddress.QuadPart;
    if ((s->flags & (TG_GPU_PANEL_SNAPSHOT_STABLE | TG_GPU_PANEL_SNAPSHOT_LAYOUT)) ==
            (TG_GPU_PANEL_SNAPSHOT_STABLE | TG_GPU_PANEL_SNAPSHOT_LAYOUT) &&
        memory->Handle && memory->CpuAddress && memory->Bytes >= T214_GPU_POST_FB_SIZE &&
        s->address >= base && !(s->address & (PAGE_SIZE - 1)) &&
        s->address - base <= memory->Bytes - T214_GPU_POST_FB_SIZE) {
        const volatile UCHAR *pixels = (volatile UCHAR *)memory->CpuAddress + (s->address - base);
        for (UINT i = 0; i < 4; ++i) {
            s->pixels[i].x = s->width * ((i & 1) ? 3 : 1) / 4;
            s->pixels[i].y = s->height * ((i & 2) ? 3 : 1) / 4;
            s->pixels[i].bgra = *(const volatile UINT32 *)(pixels +
                s->pixels[i].y * s->pitch + s->pixels[i].x * 4);
        }
        s->flags |= TG_GPU_PANEL_SNAPSHOT_PIXELS;
    }
    action->Status = STATUS_SUCCESS;
    return TRUE;
}

static NTSTATUS APIENTRY panel_escape(HANDLE context, const DXGKARG_ESCAPE *args)
{
    PAGED_CODE();
    /* HardwareAccess requests WDDM's second-level adapter serialization.
     * Debug privilege is also required: the active desktop is not necessarily
     * owned by the caller's process or interactive session. */
    if (!context || !args || args->Flags.Value != 1 || args->hDevice || args->hContext ||
        !args->pPrivateDriverData || args->PrivateDriverDataSize != sizeof(struct tg_gpu_panel_snapshot))
        return STATUS_INVALID_PARAMETER;
    if (!SeSinglePrivilegeCheck(RtlConvertLongToLuid(SE_DEBUG_PRIVILEGE), ExGetPreviousMode()))
        return STATUS_PRIVILEGE_NOT_HELD;
    struct tg_gpu_panel_snapshot request;
    RtlCopyMemory(&request, args->pPrivateDriverData, sizeof(request));
    if (request.magic != TG_GPU_PANEL_SNAPSHOT_MAGIC || request.version != TG_GPU_PANEL_SNAPSHOT_VERSION ||
        request.size != sizeof(request))
        return STATUS_GRAPHICS_DRIVER_MISMATCH;
    SNAPSHOT_ACTION action = {.Adapter = context, .Data = {
        .magic = TG_GPU_PANEL_SNAPSHOT_MAGIC, .version = TG_GPU_PANEL_SNAPSHOT_VERSION,
        .size = sizeof(struct tg_gpu_panel_snapshot)}, .Status = STATUS_UNSUCCESSFUL};
    NTSTATUS status = TgGpuPanelSynchronize(context, snapshot_panel, &action);
    if (NT_SUCCESS(status)) status = action.Status;
    if (NT_SUCCESS(status)) RtlCopyMemory(args->pPrivateDriverData, &action.Data, sizeof(action.Data));
    return status;
}

static BOOLEAN dc_scanout_mode_valid(const TG_GPU_PANEL *panel)
{
    if (!panel->Dc.owned || panel->Stopping || !panel->TargetActive || !panel->PostCpuAddress)
        return FALSE;
    const struct tg_t214_dc_io *io = &panel->Dc.io;
    const ULONG cmd = io->read32(io->context, T214_DC_COMMAND);
    const ULONG out = io->read32(io->context, T214_DC_OUTPUT_OPTIONS);
    const ULONG active_sz = io->read32(io->context, T214_DC_ACTIVE_SIZE);
    const ULONG sync_w = io->read32(io->context, T214_DC_SYNC_WIDTH);
    const ULONG back_p = io->read32(io->context, T214_DC_BACK_PORCH);
    const ULONG front_p = io->read32(io->context, T214_DC_FRONT_PORCH);
    if (cmd == MAXULONG || (cmd & T214_DC_MODE_MASK) != T214_DC_MODE_CONTINUOUS ||
        out == MAXULONG || !(out & T214_DC_DSI_ENABLE) ||
        active_sz != T214_DC_PANEL_ACTIVE ||
        sync_w != T214_DC_PANEL_SYNC ||
        back_p != T214_DC_PANEL_BACK ||
        front_p != T214_DC_PANEL_FRONT)
        return FALSE;

    const ULONG access = io->read32(io->context, T214_DC_STATE_ACCESS);
    io->write32(io->context, T214_DC_STATE_ACCESS, access | T214_DC_READ_ACTIVE);
    const ULONG win_size = io->read32(io->context, T214_DC_WIN_A_SIZE);
    const ULONG win_presc = io->read32(io->context, T214_DC_WIN_A_PRESCALED);
    const ULONG win_color = io->read32(io->context, T214_DC_WIN_A_COLOR);
    const ULONG win_dda = io->read32(io->context, T214_DC_WIN_A_DDA);
    const ULONG win_pos = io->read32(io->context, T214_DC_WIN_A_POSITION);
    const ULONG win_kind = io->read32(io->context, T214_DC_WIN_A_SURFACE_KIND);
    io->write32(io->context, T214_DC_STATE_ACCESS, access);
    (void)io->read32(io->context, T214_DC_STATE_ACCESS);

    return win_size == T214_DC_PANEL_ACTIVE &&
        win_presc == T214_DC_PANEL_PRESCALED &&
        win_color == T214_DC_COLOR_BGRA8888 &&
        win_dda == T214_DC_DDA_IDENTITY &&
        win_pos == 0u &&
        win_kind == 0u;
}

/* Bounded HIGH_LEVEL-safe GPU cancellation/isolation via SoC MC/PMC/CAR (N2):
 * preserves Display A resources, never takes spinlocks or issues blind BAR0 PRI
 * reads on a wedged GPU, and fails if in-flight GPU work cannot be isolated. */
static NTSTATUS isolate_gpu_for_system_display(TG_GPU_ADAPTER *adapter)
{
    InterlockedExchange(&adapter->Engine.FaultIsr.Armed, 0);
    InterlockedExchange(&adapter->Engine.FaultIsr.ActiveEpoch, 0);
    adapter->Engine.Stopping = TRUE;
    if (adapter->QueueReady)
        adapter->Queue.Paused = TRUE;
    if (adapter->Resources.LicMapping &&
        adapter->Resources.Lic.u.Memory.Length >= T214_GPU_LIC_SIZE) {
        volatile PULONG lic = (volatile PULONG)adapter->Resources.LicMapping;
        WRITE_REGISTER_ULONG((PULONG)((PUCHAR)lic + 20u), T214_GPU_LIC_STALL);
        WRITE_REGISTER_ULONG((PULONG)((PUCHAR)lic + 8u), T214_GPU_LIC_STALL);
        TgGpuDataSyncBarrier();
    }

    const BOOLEAN queue_busy = adapter->QueueReady &&
        (adapter->Queue.Current.Engine != NULL ||
         (adapter->Queue.Pending.Flink != NULL && !IsListEmpty(&adapter->Queue.Pending)));
    const BOOLEAN work_in_flight = adapter->Engine.Active != NULL ||
        adapter->Engine.Notification != NULL ||
        adapter->Engine.Channel.referenced ||
        adapter->Engine.Channel.submitted ||
        queue_busy;
    const BOOLEAN gpu_live = (adapter->Hardware.Power.owned || adapter->Hardware.Power.ready) &&
        !adapter->Hardware.Isolated;
    if (!gpu_live && !work_in_flight)
        return STATUS_SUCCESS;

    volatile PULONG clamp_reg = (adapter->Resources.Mapping[TgGpuClamp] &&
        adapter->Resources.Memory[TgGpuClamp].u.Memory.Length >= 4u) ?
        (volatile PULONG)adapter->Resources.Mapping[TgGpuClamp] : NULL;
    volatile PUCHAR car_base = (adapter->Resources.Mapping[TgGpuCarX] &&
        adapter->Resources.Memory[TgGpuCarX].u.Memory.Length >= 0x18u) ?
        (volatile PUCHAR)adapter->Resources.Mapping[TgGpuCarX] : NULL;

    /* Already isolated prior to SystemDisplayEnable: verify hardware clamp/reset
     * if mapped, or accept software-isolated state when power is already down. */
    if (adapter->Hardware.Isolated && !adapter->Hardware.Power.ready) {
        if (clamp_reg && car_base) {
            const ULONG c = READ_REGISTER_ULONG((PULONG)clamp_reg);
            const ULONG r = READ_REGISTER_ULONG((PULONG)(car_base + 0x0cu));
            TgGpuDataSyncBarrier();
            if (c == MAXULONG || r == MAXULONG || !(c & 1u) || !(r & (1u << 24)))
                return STATUS_DEVICE_DATA_ERROR;
        } else if (adapter->Hardware.Power.owned) {
            return STATUS_DEVICE_NOT_READY;
        }
        return STATUS_SUCCESS;
    }

    volatile PULONG mc_flush = (adapter->Resources.Mapping[TgGpuMcFlush] &&
        adapter->Resources.Memory[TgGpuMcFlush].u.Memory.Length >= 8u) ?
        (volatile PULONG)adapter->Resources.Mapping[TgGpuMcFlush] : NULL;
    if (!clamp_reg || !car_base || !mc_flush)
        return STATUS_DEVICE_NOT_READY;

    /* Block and drain GPU DMA clients at Tegra MC without disturbing Display A.
     * Follow NVIDIA Tegra MC (tegra_mc_flush) and tg_gpu_power_drain: require
     * initial bit 2 sample plus 5 consecutive identical follow-up reads so a
     * transient 1->0 status pulse can never bypass isolation (N2). */
    {
        volatile PULONG mc_status = (volatile PULONG)((PUCHAR)mc_flush + 4u);
        const ULONG old_flush = READ_REGISTER_ULONG((PULONG)mc_flush);
        if (old_flush == MAXULONG)
            return STATUS_DEVICE_DATA_ERROR;
        WRITE_REGISTER_ULONG((PULONG)mc_flush, old_flush | (1u << 2));
        TgGpuDataSyncBarrier();
        const ULONG req_flush = READ_REGISTER_ULONG((PULONG)mc_flush);
        TgGpuDataSyncBarrier();
        if (req_flush == MAXULONG || !(req_flush & (1u << 2)))
            return STATUS_DEVICE_DATA_ERROR;
        BOOLEAN drained = FALSE;
        for (UINT i = 0; i < 5000u; ++i) {
            const ULONG st = READ_REGISTER_ULONG((PULONG)mc_status);
            TgGpuDataSyncBarrier();
            if (st == MAXULONG)
                return STATUS_DEVICE_DATA_ERROR;
            if (st & (1u << 2)) {
                UINT sample;
                for (sample = 0u; sample < 5u; ++sample) {
                    const ULONG check = READ_REGISTER_ULONG((PULONG)mc_status);
                    TgGpuDataSyncBarrier();
                    if (check == MAXULONG)
                        return STATUS_DEVICE_DATA_ERROR;
                    if (check != st)
                        break;
                }
                if (sample == 5u) {
                    drained = TRUE;
                    break;
                }
            }
            KeStallExecutionProcessor(1);
        }
        if (!drained)
            return STATUS_IO_TIMEOUT;
    }

    /* Assert PMC_GPU_CLAMP, CAR_RESET_SET (GPU_GATE), and CAR_CLOCK_CLR. */
    const ULONG old_clamp = READ_REGISTER_ULONG((PULONG)clamp_reg);
    if (old_clamp == MAXULONG)
        return STATUS_DEVICE_DATA_ERROR;
    WRITE_REGISTER_ULONG((PULONG)clamp_reg, old_clamp | 1u);
    TgGpuDataSyncBarrier();
    for (UINT i = 0; i < 10u; ++i)
        KeStallExecutionProcessor(1);
    const ULONG clamp_after = READ_REGISTER_ULONG((PULONG)clamp_reg);
    TgGpuDataSyncBarrier();
    if (clamp_after == MAXULONG || !(clamp_after & 1u))
        return STATUS_DEVICE_DATA_ERROR;

    WRITE_REGISTER_ULONG((PULONG)(car_base + 0x10u), 1u << 24);
    TgGpuDataSyncBarrier();
    for (UINT i = 0; i < 10u; ++i)
        KeStallExecutionProcessor(1);
    const ULONG reset_after = READ_REGISTER_ULONG((PULONG)(car_base + 0x0cu));
    TgGpuDataSyncBarrier();
    if (reset_after == MAXULONG || !(reset_after & (1u << 24)))
        return STATUS_DEVICE_DATA_ERROR;

    WRITE_REGISTER_ULONG((PULONG)(car_base + 0x08u), (1u << 24) | (1u << 29));
    TgGpuDataSyncBarrier();

    adapter->Hardware.Power.ready = false;
    adapter->Hardware.Isolated = TRUE;
    adapter->Platform.PreservedGpuIsolated = TRUE;
    InterlockedExchange(&adapter->Engine.FaultIsr.GpuUnreachable, 1);
    return STATUS_SUCCESS;
}

/* BugCheck / shutdown display takeover: runs at arbitrary IRQL (including
 * HIGH_LEVEL) with no locks, allocations, waits, or blind GPU BAR0 MMIO.
 * Cancels/isolates in-flight GPU work via SoC MC/PMC/CAR and switches Display A
 * Window A directly onto the pre-mapped linear POST framebuffer. */
static NTSTATUS APIENTRY system_display_enable(PVOID context, D3DDDI_VIDEO_PRESENT_TARGET_ID target,
    PDXGKARG_SYSTEM_DISPLAY_ENABLE_FLAGS flags, UINT *width, UINT *height, D3DDDIFORMAT *format)
{
    TG_GPU_ADAPTER *adapter = context;
    UNREFERENCED_PARAMETER(flags);
    if (!adapter || !width || !height || !format)
        return STATUS_INVALID_PARAMETER;
    TG_GPU_PANEL *panel = &adapter->Panel;
    if (!panel->Dc.owned || panel->Stopping || !panel->TargetActive || !panel->PostCpuAddress)
        return STATUS_DEVICE_NOT_READY;
    if (target != D3DDDI_ID_UNINITIALIZED && target != panel->Boot.TargetId)
        return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_TARGET;
    if (!dc_scanout_mode_valid(panel))
        return STATUS_DEVICE_NOT_READY;

    NTSTATUS iso_status = isolate_gpu_for_system_display(adapter);
    if (!NT_SUCCESS(iso_status))
        return iso_status;

    const struct tg_t214_dc_io *io = &panel->Dc.io;

    volatile ULONG *fb = (volatile ULONG *)panel->PostCpuAddress;
    const SIZE_T words = T214_GPU_POST_FB_SIZE / sizeof(ULONG);
    for (SIZE_T i = 0; i < words; ++i)
        fb[i] = 0u;
    TgGpuDataSyncBarrier();

    const ULONG options = io->read32(io->context, T214_DC_WIN_A_OPTIONS);
    const BOOLEAN same_layout = !(options & T214_DC_WIN_TRANSFORM_BITS) &&
        (io->read32(io->context, T214_DC_WIN_A_STRIDE) & 0xffffu) == T214_DC_PANEL_PITCH &&
        io->read32(io->context, T214_DC_WIN_A_H_OFFSET) == 0u &&
        io->read32(io->context, T214_DC_WIN_A_V_OFFSET) == 0u;
    /* Only an address-only switch on an already-visible native surface may use
     * HBlank (HCOUNTER); visibility or layout changes must latch at VBlank. */
    const BOOLEAN hcounter = same_layout && ((options & T214_DC_WIN_ENABLE) != 0u);
    const ULONG act = io->read32(io->context, T214_DC_REG_ACT_CONTROL);
    const ULONG next_act = (act & ~T214_DC_WIN_A_HCOUNTER) |
        (hcounter ? T214_DC_WIN_A_HCOUNTER : 0u);
    if (next_act != act)
        io->write32(io->context, T214_DC_REG_ACT_CONTROL, next_act);
    io->write32(io->context, T214_DC_WIN_A_OPTIONS,
                (options & ~T214_DC_WIN_TRANSFORM_BITS) | T214_DC_WIN_ENABLE);
    io->write32(io->context, T214_DC_WIN_A_STRIDE,
                (io->read32(io->context, T214_DC_WIN_A_STRIDE) & 0xffff0000u) | T214_DC_PANEL_PITCH);
    io->write32(io->context, T214_DC_WIN_A_H_OFFSET, 0u);
    io->write32(io->context, T214_DC_WIN_A_V_OFFSET, 0u);
    io->write32(io->context, T214_DC_WIN_A_ADDR, T214_GPU_POST_FB_BASE);
    const ULONG group = T214_DC_GENERAL_ACT_REQ | T214_DC_WIN_A_ACT_REQ;
    io->write32(io->context, T214_DC_STATE_CONTROL, group << 8);
    (void)io->read32(io->context, T214_DC_STATE_CONTROL);
    io->write32(io->context, T214_DC_STATE_CONTROL, group);
    (void)io->read32(io->context, T214_DC_STATE_CONTROL);
    for (UINT i = 0; i < 25000u; ++i) {
        if (!(io->read32(io->context, T214_DC_STATE_CONTROL) & group))
            break;
        KeStallExecutionProcessor(1);
    }

    /* Strictly verify that ACT_REQ cleared, the active bank latched the visible
     * POST framebuffer, AND Display A mode, timings, active geometry and target
     * state remain valid at completion (N2 / M3 / T3). */
    struct tg_t214_dc_scanout active = {0};
    if ((io->read32(io->context, T214_DC_STATE_CONTROL) & group) ||
        !tg_t214_dc_latched_surface(&panel->Dc, &active) ||
        active.address != T214_GPU_POST_FB_BASE || !active.visible)
        return STATUS_IO_TIMEOUT;
    if (!dc_scanout_mode_valid(panel))
        return STATUS_DEVICE_NOT_READY;

    if (panel->PathRotation == D3DKMDT_VPPR_IDENTITY ||
        panel->PathRotation == D3DKMDT_VPPR_ROTATE180) {
        panel->SystemDisplayWidth = T214_DC_PANEL_WIDTH;
        panel->SystemDisplayHeight = T214_DC_PANEL_HEIGHT;
    } else {
        panel->SystemDisplayWidth = T214_DC_PANEL_HEIGHT;
        panel->SystemDisplayHeight = T214_DC_PANEL_WIDTH;
    }
    *width = panel->SystemDisplayWidth;
    *height = panel->SystemDisplayHeight;
    *format = D3DDDIFMT_A8R8G8B8;
    return STATUS_SUCCESS;
}

static VOID APIENTRY system_display_write(PVOID context, PVOID source, UINT width, UINT height,
    UINT stride, UINT pos_x, UINT pos_y)
{
    TG_GPU_ADAPTER *adapter = context;
    if (!adapter || !source || !width || !height)
        return;
    TG_GPU_PANEL *panel = &adapter->Panel;
    if (!panel->PostCpuAddress)
        return;
    const BOOLEAN portrait = (panel->PathRotation == D3DKMDT_VPPR_IDENTITY ||
                              panel->PathRotation == D3DKMDT_VPPR_ROTATE180);
    const UINT logical_w = panel->SystemDisplayWidth ? panel->SystemDisplayWidth :
        (portrait ? T214_DC_PANEL_WIDTH : T214_DC_PANEL_HEIGHT);
    const UINT logical_h = panel->SystemDisplayHeight ? panel->SystemDisplayHeight :
        (portrait ? T214_DC_PANEL_HEIGHT : T214_DC_PANEL_WIDTH);
    if (pos_x >= logical_w || pos_y >= logical_h)
        return;
    const UINT max_w = (width > logical_w - pos_x) ? (logical_w - pos_x) : width;
    const UINT max_h = (height > logical_h - pos_y) ? (logical_h - pos_y) : height;
    const D3DKMDT_VIDPN_PRESENT_PATH_ROTATION rot = panel->PathRotation;
    const UCHAR *src = (const UCHAR *)source;
    volatile ULONG *dst = (volatile ULONG *)panel->PostCpuAddress;

    for (UINT y = 0; y < max_h; ++y) {
        const ULONG *row = (const ULONG *)(src + (SIZE_T)y * stride);
        const UINT ly = pos_y + y;
        for (UINT x = 0; x < max_w; ++x) {
            const UINT lx = pos_x + x;
            UINT px, py;
            if (rot == D3DKMDT_VPPR_IDENTITY) {
                px = lx;
                py = ly;
            } else if (rot == D3DKMDT_VPPR_ROTATE180) {
                px = (T214_DC_PANEL_WIDTH - 1u) - lx;
                py = (T214_DC_PANEL_HEIGHT - 1u) - ly;
            } else if (rot == D3DKMDT_VPPR_ROTATE270) {
                px = ly;
                py = (T214_DC_PANEL_HEIGHT - 1u) - lx;
            } else {
                px = (T214_DC_PANEL_WIDTH - 1u) - ly;
                py = lx;
            }
            dst[(SIZE_T)py * T214_DC_PANEL_WIDTH + px] = row[x];
        }
    }
    TgGpuDataSyncBarrier();
}

void TgGpuRegisterPanelScanout(DRIVER_INITIALIZATION_DATA *ddis)
{
    ddis->DxgkDdiInterruptRoutine = panel_interrupt;
    ddis->DxgkDdiControlInterrupt = control_interrupt;
    ddis->DxgkDdiGetScanLine = get_scanline;
    ddis->DxgkDdiSetPointerPosition = set_pointer_position;
    ddis->DxgkDdiSetPointerShape = set_pointer_shape;
    ddis->DxgkDdiSetVidPnSourceAddress = set_primary;
    ddis->DxgkDdiSetVidPnSourceAddressWithMultiPlaneOverlay3 = set_primary_mpo;
    ddis->DxgkDdiCheckMultiPlaneOverlaySupport3 = check_mpo;
    ddis->DxgkDdiEscape = panel_escape;
    ddis->DxgkDdiSystemDisplayEnable = system_display_enable;
    ddis->DxgkDdiSystemDisplayWrite = system_display_write;
}
