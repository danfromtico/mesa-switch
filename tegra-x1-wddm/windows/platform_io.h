/* SPDX-License-Identifier: MIT */
#ifndef TG_GPU_PLATFORM_IO_H
#define TG_GPU_PLATFORM_IO_H
#include "resources.h"
#include "pmic.h"
#include "../platform/t214_power.h"
typedef struct _TG_GPU_PLATFORM {
    const TG_GPU_RESOURCES *Resources; /* borrowed through hardware retirement */
    KSPIN_LOCK HandleLock;
    HANDLE Bridge;
    PVOID FileObject;
    PVOID OwnerDevice;
    PVOID PnpNotify;
    ULONG BridgeEpoch;
    NTSTATUS IoStatus;
    BOOLEAN EmcRailOwner,RailClaimed,QueryRemove,NeedReclaim,Initialized,Opening,Closing;
} TG_GPU_PLATFORM;

/* Open/Close and requests have a serialized adapter lifetime. PnP callbacks
 * can run concurrently; HandleLock and BridgeEpoch protect publication.
 * DriverObject registration enables TargetDeviceChange PnP unbind and
 * IoSetCompletionRoutineEx unload-safe IRP completions on the adapter FDO. */
VOID TgGpuSetPlatformDriverObject(PVOID DriverObject);
NTSTATUS TgGpuOpenPlatform(TG_GPU_PLATFORM *,const TG_GPU_RESOURCES *);
NTSTATUS TgGpuClosePlatform(TG_GPU_PLATFORM *);
NTSTATUS TgGpuPlatformRequest(TG_GPU_PLATFORM *,struct tg_gpu_pmic_request *);
/* Optional capability/telemetry request: does not latch IoStatus when an older
 * SwitchFan build returns STATUS_INVALID_PARAMETER or STATUS_NOT_SUPPORTED. */
NTSTATUS TgGpuPlatformTryRequest(TG_GPU_PLATFORM *,struct tg_gpu_pmic_request *);
/* Optional long-lived kernel request. Returns referenced real objects; the
 * caller releases both references. Failure does not poison display IoStatus. */
NTSTATUS TgGpuReferencePlatformBridge(TG_GPU_PLATFORM *,PFILE_OBJECT *,PDEVICE_OBJECT *);
NTSTATUS TgGpuPlatformPreflight(TG_GPU_PLATFORM *,UCHAR *PmicAddress);
VOID TgGpuPlatformAbandonRail(TG_GPU_PLATFORM *,UCHAR PmicAddress);
struct tg_gpu_power_io TgGpuPlatformIo(TG_GPU_PLATFORM *);
#endif
