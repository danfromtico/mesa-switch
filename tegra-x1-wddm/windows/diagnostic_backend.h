/* SPDX-License-Identifier: MIT */
#ifndef TG_GPU_DIAGNOSTIC_BACKEND_H
#define TG_GPU_DIAGNOSTIC_BACKEND_H
#include "hardware.h"

typedef struct _TG_GPU_DIAGNOSTIC_BACKEND TG_GPU_DIAGNOSTIC_BACKEND;
/* Opt-in probe only. Uses real GPU backing, shared packed contexts/queue and
 * PsCreateSystemThread. Diagnostic callbacks replace VidMm/VidSch, and no
 * display adapter, user command stream or Vulkan API is registered here.
 * Keep State and platform resources until Stop succeeds, including failures.
 */
NTSTATUS TgGpuRunDiagnosticBackend(TG_GPU_DIAGNOSTIC_BACKEND **State,
    const struct tg_gpu_power_io *,UCHAR,const struct tg_gpu_gr_payload *,
    UINT Seed,struct tg_gpu_status *);
NTSTATUS TgGpuStopDiagnosticBackend(TG_GPU_DIAGNOSTIC_BACKEND **State,
    struct tg_gpu_status *);
#endif
