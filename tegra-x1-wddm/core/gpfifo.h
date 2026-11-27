/* SPDX-License-Identifier: MIT */
#ifndef TEGRA_GPU_GPFIFO_H
#define TEGRA_GPU_GPFIFO_H

#include <stdbool.h>
#include <stdint.h>

struct tg_gpu_gpfifo_entry {
    uint32_t low;
    uint32_t high;
};

/* Encode a main pushbuffer entry, unprivileged and unconditional. No CPU
 * pointer is accepted: va must name resident GPU command memory. Publishing
 * to a live ring, its memory barriers, GET/PUT and retirement are separate.
 */
bool tg_gpu_build_gpfifo(uint64_t va, uint32_t words, bool no_prefetch,
                         struct tg_gpu_gpfifo_entry *entry);

#endif
