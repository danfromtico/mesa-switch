/* SPDX-License-Identifier: MIT */
#ifndef TG_GPU_PLATFORM_IO_H
#define TG_GPU_PLATFORM_IO_H
#include "resources.h"
#include "pmic.h"
#include "../platform/t214_power.h"
typedef struct _TG_GPU_PLATFORM {
    const TG_GPU_RESOURCES *Resources; /* borrowed through hardware retirement */
    HANDLE Bridge;
    NTSTATUS IoStatus;
} TG_GPU_PLATFORM;

/* PASSIVE, serialized bridge lifetime; mappings remain owned by the caller.
 * Kernel handle to the existing SwitchFan PMIC owner, not a new SPB target.
 */
NTSTATUS TgGpuOpenPlatform(TG_GPU_PLATFORM *,const TG_GPU_RESOURCES *);
NTSTATUS TgGpuClosePlatform(TG_GPU_PLATFORM *);
NTSTATUS TgGpuPlatformRequest(TG_GPU_PLATFORM *,struct tg_gpu_pmic_request *);
/* Optional capability/telemetry request: does not latch IoStatus when an older
 * SwitchFan build returns STATUS_INVALID_PARAMETER or STATUS_NOT_SUPPORTED. */
NTSTATUS TgGpuPlatformTryRequest(TG_GPU_PLATFORM *,struct tg_gpu_pmic_request *);
NTSTATUS TgGpuPlatformPreflight(TG_GPU_PLATFORM *,UCHAR *PmicAddress);
/* Mapped-register reads/writes can run in the ISR. PMIC/PLLP queries and
 * delays that sleep are PASSIVE worker operations, as in the diagnostic.
 */
struct tg_gpu_power_io TgGpuPlatformIo(TG_GPU_PLATFORM *);
#endif
