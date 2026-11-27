/* SPDX-License-Identifier: MIT */
#ifndef TG_GPU_PANEL_H
#define TG_GPU_PANEL_H

#include <ntddk.h>
#include <dispmprt.h>
#include "../platform/t214_dc.h"

typedef struct _TG_GPU_PANEL_FLIP {
    UINT64 Address;
    ULONGLONG PresentId;
    BOOLEAN Visible, Immediate;
} TG_GPU_PANEL_FLIP;

typedef struct _TG_GPU_PANEL {
    struct tg_t214_dc Dc;
    DXGK_DISPLAY_INFORMATION Boot;
    TG_GPU_PANEL_FLIP Pending, Displayed;
    D3DKMDT_VIDPN_PRESENT_PATH_ROTATION PathRotation;
    volatile PVOID PostCpuAddress;
    UINT SystemDisplayWidth, SystemDisplayHeight;
    BOOLEAN Stopping, NotifyVBlank, NotifyMpoVBlank, SourceVisible, TargetActive;
    /* Valid from the one-child StartDevice result until successful StopDevice. */
    BOOLEAN ChildReported;
} TG_GPU_PANEL;

struct _TG_GPU_ADAPTER;
extern const D3DKMDT_VIDEO_SIGNAL_INFO TgGpuPanelSignal;
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS TgGpuPanelStart(struct _TG_GPU_ADAPTER *adapter);
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS TgGpuPanelStop(struct _TG_GPU_ADAPTER *adapter);
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS TgGpuPanelSetVisibility(struct _TG_GPU_ADAPTER *adapter, BOOLEAN visible);
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS TgGpuPanelSetTargetActive(struct _TG_GPU_ADAPTER *adapter, BOOLEAN active);
_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS TgGpuPanelSynchronize(struct _TG_GPU_ADAPTER *adapter, PKSYNCHRONIZE_ROUTINE routine,
    PVOID context);
/* Caller holds display interrupt synchronization. A completed flip must match
 * the full active fetch layout, not just its physical address. */
BOOLEAN TgGpuPanelReadLatched(TG_GPU_PANEL *, struct tg_t214_dc_scanout *);
void TgGpuRegisterPanelLifecycle(DRIVER_INITIALIZATION_DATA *ddis);
void TgGpuRegisterPanelVidPn(DRIVER_INITIALIZATION_DATA *ddis);
void TgGpuRegisterPanelScanout(DRIVER_INITIALIZATION_DATA *ddis);

#endif
