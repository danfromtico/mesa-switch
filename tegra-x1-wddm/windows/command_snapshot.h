/* SPDX-License-Identifier: MIT */
#ifndef TG_GPU_COMMAND_SNAPSHOT_H
#define TG_GPU_COMMAND_SNAPSHOT_H
#include <ntddk.h>
#include <dispmprt.h>
#include "../core/command_stream.h"
#include "submit_abi.h"
typedef struct _TG_GPU_COMMAND_SNAPSHOT {
    UINT Bytes;
    BOOLEAN HasIndirect;
    struct tg_gpu_submit_indirect Indirect;
    struct tg_gpu_command_info Info;
    UINT Words[];
} TG_GPU_COMMAND_SNAPSHOT;
NTSTATUS TgGpuCaptureCommandSnapshot(const DXGKARG_SUBMITCOMMANDVIRTUAL *,
    const struct tg_gpu_command_va *,TG_GPU_COMMAND_SNAPSHOT **);
VOID TgGpuFreeCommandSnapshot(TG_GPU_COMMAND_SNAPSHOT *);
#endif
