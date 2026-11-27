/* SPDX-License-Identifier: MIT */
#ifndef TG_GPU_COPY_STREAM_H
#define TG_GPU_COPY_STREAM_H
#include "command_stream.h"

struct tg_gpu_copy_state {
    uint32_t registers[24];
    uint32_t written;
};

bool tg_gpu_copy_method(struct tg_gpu_copy_state *, uint32_t method, uint32_t value,
                        const struct tg_gpu_command_va *, uint64_t *max_end);
#endif
