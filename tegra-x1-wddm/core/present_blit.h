/* SPDX-License-Identifier: MIT */
#ifndef TG_GPU_PRESENT_BLIT_H
#define TG_GPU_PRESENT_BLIT_H
#include <stdbool.h>
#include <stdint.h>
#include "present_halfturn.h"

struct tg_gpu_blit_rect { int32_t left, top, right, bottom; };
struct tg_gpu_present_blit {
    struct tg_gpu_blit_surface source, destination;
    struct tg_gpu_blit_rect source_rect, destination_rect;
    uint32_t rotation, logical_width, logical_height;
};
struct tg_gpu_present_copy {
    uint64_t source_offset, destination_offset;
    uint32_t source_pitch, destination_pitch, row_bytes, lines;
};

/* Degrees follow the WDDM counter-clockwise convention. A quarter-turn is
 * a set of positive-stride CE column copies, not a CPU pixel rotation. */
bool tg_gpu_present_blit_init(struct tg_gpu_present_blit *,
    struct tg_gpu_blit_surface, struct tg_gpu_blit_surface,
    struct tg_gpu_blit_rect, struct tg_gpu_blit_rect, uint32_t rotation);
uint32_t tg_gpu_present_blit_stripes(const struct tg_gpu_present_blit *,
    struct tg_gpu_blit_rect);
bool tg_gpu_present_blit_copy(const struct tg_gpu_present_blit *,
    struct tg_gpu_blit_rect, uint32_t stripe, struct tg_gpu_present_copy *);
bool tg_gpu_present_blit_halfturn(const struct tg_gpu_present_blit *,
    struct tg_gpu_blit_rect, struct tg_gpu_present_halfturn *);
#define TG_GPU_PRESENT_COPY_MAX_WORDS 17u
uint32_t tg_gpu_encode_present_copy(uint32_t *, uint32_t capacity_words,
    uint64_t source, uint64_t destination, const struct tg_gpu_present_copy *, bool opaque,
    bool cpu_readback);
#endif
