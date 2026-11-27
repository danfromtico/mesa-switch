/* SPDX-License-Identifier: MIT */
#ifndef TG_GPU_RESET_BACKEND_H
#define TG_GPU_RESET_BACKEND_H
#include "worker.h"
/* Serialized PASSIVE lifetime, shared by the future adapter's global TDR DDIs
 * and the probe. Reset joins/detaches execution and proves isolation without
 * freeing its private backing. No GPU access is allowed during OS cleanup.
 * Restart owns cold initialization, preserves the graphics epoch and opens
 * admission only after the worker starts. Old contexts/roots stay lost.
 */
NTSTATUS TgGpuResetBackend(TG_GPU_CONTEXT_ENGINE *,TG_GPU_WORK_QUEUE *,TG_GPU_HARDWARE *,TG_GPU_WORKER *);
/* With the OS scheduler idle, retain its root tables across a power cycle. */
NTSTATUS TgGpuSuspendBackend(TG_GPU_CONTEXT_ENGINE *,TG_GPU_WORK_QUEUE *,TG_GPU_HARDWARE *,TG_GPU_WORKER *);
/* One-way terminal teardown: bounded waits, no recovery handoff and no D0 wake. */
NTSTATUS TgGpuShutdownBackend(TG_GPU_CONTEXT_ENGINE *,TG_GPU_WORK_QUEUE *,TG_GPU_HARDWARE *,TG_GPU_WORKER *);
NTSTATUS TgGpuRestartBackend(TG_GPU_CONTEXT_ENGINE *,TG_GPU_WORK_QUEUE *,TG_GPU_HARDWARE *,TG_GPU_WORKER *,
    const struct tg_gpu_power_io *,UCHAR,const struct tg_gpu_gr_payload *);
/* Atomic restart variant: validates platform IoStatus and arms the stall fault
 * ISR while holding DispatchGate BEFORE starting the worker or opening admission.
 * When PreserveRoots is TRUE (suspended PagingOnly D0 wake), rollback uses
 * TgGpuSuspendBackend first so non-system roots survive a failed wake. */
NTSTATUS TgGpuRestartBackendEx(TG_GPU_CONTEXT_ENGINE *,TG_GPU_WORK_QUEUE *,TG_GPU_HARDWARE *,TG_GPU_WORKER *,
    const struct tg_gpu_power_io *,UCHAR,const struct tg_gpu_gr_payload *,
    const NTSTATUS *IoStatus,PVOID GpuRegisters,PVOID LicRegisters,BOOLEAN PreserveRoots);
#endif
