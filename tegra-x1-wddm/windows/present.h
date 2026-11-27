/* SPDX-License-Identifier: MIT */
#ifndef TG_GPU_PRESENT_H
#define TG_GPU_PRESENT_H
#include "context.h"
#include "command_snapshot.h"
DXGKDDI_PRESENT TgGpuDdiPresent;
NTSTATUS TgGpuCapturePresentSnapshot(TG_GPU_CONTEXT_ENGINE *,
    const DXGKARG_SUBMITCOMMANDVIRTUAL *,TG_GPU_COMMAND_SNAPSHOT **);
#endif
