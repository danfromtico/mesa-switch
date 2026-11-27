/* SPDX-License-Identifier: MIT */
#ifndef TG_GPU_WINDOWS_HARDWARE_H
#define TG_GPU_WINDOWS_HARDWARE_H
#include "context.h"
#include "dvfs.h"
#include "../core/execute.h"

typedef struct _TG_GPU_HARDWARE {
    TG_GPU_CONTEXT_ENGINE *Engine;
    struct tg_gpu_power_io Io;
    struct tg_gpu_power Power;
    struct tg_gpu_clock Clock;
    struct tg_gpu_execution Execution;
    TG_GPU_PRIVATE_MEMORY Boot,Work;
    struct tg_gpu_status Status;
    TG_GPU_DVFS Dvfs;
    BOOLEAN Ready,GpuOwned,Isolated;
} TG_GPU_HARDWARE;

/* PASSIVE, serialized cold start. The trusted platform supplies its declared
 * resource I/O and PMIC-owner bridge, preflights temperature, and keeps the I/O
 * context alive through stop. Firmware source files can be freed on return.
 * Any partial start retains tracking: stop the queue, then call StopHardware
 * even on failure. This function schedules no application work.
 */
NTSTATUS TgGpuStartHardware(TG_GPU_HARDWARE *,TG_GPU_CONTEXT_ENGINE *,
    const struct tg_gpu_power_io *,UCHAR PmicAddress,const struct tg_gpu_gr_payload *);
/* Same quiescence/isolation gate as StopHardware, but retains all private
 * backing and the hardware owner through the OS's TDR cleanup interval. */
NTSTATUS TgGpuIsolateHardware(TG_GPU_HARDWARE *);
/* Stop/detach the queue first; live work must retire/reset before this call.
 * Failure retains private memory and the engine owner, including failures
 * after power isolation when a Dxgkrnl free needs retry.
 */
NTSTATUS TgGpuStopHardware(TG_GPU_HARDWARE *);
#endif
