/* SPDX-License-Identifier: MIT */
#ifndef TG_GPU_ADAPTER_H
#define TG_GPU_ADAPTER_H
#include "context.h"
#include "work_queue.h"
#include "resources.h"
#include "hardware.h"
#include "platform_io.h"
#include "worker.h"
#include "panel.h"
#include "nvdec_adapter.h"
#include "diagnostic_events.h"

/* DWM and shell processes share this VidMm-managed page-table segment.
 * The initial 16-table (2 MiB) pool fills during desktop device creation;
 * .150 ETW records full residency followed by failed Segment 1 reservations.
 * Keep space for 128 tables while application storage remains dynamic under
 * the unchanged 512 MiB resource cap. This is MMU backing, not application VRAM.
 */
#define TG_GPU_ADAPTER_TABLE_BYTES (128u*TG_GPU_TABLE_BYTES)
enum tg_gpu_adapter_state { TgGpuAdapterAdded,TgGpuAdapterStarting,TgGpuAdapterReady,
    TgGpuAdapterStopping,TgGpuAdapterResetting,TgGpuAdapterSuspended };
typedef struct _TG_GPU_ADAPTER {
    /* All implemented allocation/context/query DDIs accept this same handle. */
    TG_GPU_CONTEXT_ENGINE Engine;
    PDEVICE_OBJECT PhysicalDevice;
    TG_GPU_RESOURCES Resources;
    TG_GPU_TABLE_SEGMENT Tables;
    TG_GPU_WORK_QUEUE Queue;
    TG_GPU_HARDWARE Hardware;
    TG_GPU_PLATFORM Platform;
    TG_GPU_WORKER Worker;
    TG_GPU_PANEL Panel;
    TG_NVDEC_ADAPTER Video;
    volatile LONG64 StartupDiagnostic;
    enum tg_gpu_adapter_state State;
    BOOLEAN EngineReady,QueueReady;
    /* A terminal power transition retires native roots and forbids D0 wake. */
    BOOLEAN ShutdownFinal;
} TG_GPU_ADAPTER;
_Static_assert(FIELD_OFFSET(TG_GPU_ADAPTER,Engine)==0,"adapter DDI handle layout");
_Static_assert(FIELD_OFFSET(TG_GPU_ADAPTER,StartupDiagnostic)%8==0,
    "startup diagnostic snapshot alignment");

DXGKDDI_ADD_DEVICE TgGpuDdiAddDevice;
DXGKDDI_START_DEVICE TgGpuDdiStartDevice;
DXGKDDI_STOP_DEVICE TgGpuDdiStopDevice;
DXGKDDI_REMOVE_DEVICE TgGpuDdiRemoveDevice;
DXGKDDI_RESETFROMTIMEOUT TgGpuDdiResetFromTimeout;
DXGKDDI_RESTARTFROMTIMEOUT TgGpuDdiRestartFromTimeout;
DXGKDDI_COLLECTDIAGNOSTICINFO TgGpuDdiCollectDiagnosticInfo;
void TgGpuRecordStartup(TG_GPU_ADAPTER *,enum tg_gpu_startup_stage,NTSTATUS);
NTSTATUS TgGpuRestartAdapterHardware(TG_GPU_ADAPTER *);
NTSTATUS TgGpuSetAdapterPowerState(TG_GPU_ADAPTER *,ULONG DeviceUid,
    DEVICE_POWER_STATE PowerState,POWER_ACTION Action);
void TgGpuRegisterResetDdis(DRIVER_INITIALIZATION_DATA *);
/* The platform provider starts after StartDevice acquires resources. */
NTSTATUS TgGpuStartAdapterHardware(TG_GPU_ADAPTER *,const struct tg_gpu_power_io *,
    UCHAR PmicAddress,const struct tg_gpu_gr_payload *);
/* Platform, firmware and worker hookup after resource acquisition. */
NTSTATUS TgGpuStartPlatformAdapter(TG_GPU_ADAPTER *);
/* Populate shared callbacks. The miniport adds render-only lifecycle DDIs. */
void TgGpuRegisterAdapterDdis(DRIVER_INITIALIZATION_DATA *);
#endif
