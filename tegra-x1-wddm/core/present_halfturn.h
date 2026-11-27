/* SPDX-License-Identifier: MIT */
#ifndef TG_GPU_PRESENT_HALFTURN_H
#define TG_GPU_PRESENT_HALFTURN_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct tg_gpu_blit_surface { uint32_t width, height, pitch; };
struct tg_gpu_present_halfturn {
    struct tg_gpu_blit_surface source, destination;
    uint32_t source_x, source_y, destination_x, destination_y, width, height;
};
/* One complete, self-contained FERMI_TWOD_A operation on subchannel 3. */
#define TG_GPU_PRESENT_HALFTURN_WORDS 57u
uint32_t tg_gpu_encode_present_halfturn(uint32_t *, uint32_t capacity_words,
    uint64_t source, uint64_t destination, const struct tg_gpu_present_halfturn *, bool opaque);
bool tg_gpu_decode_present_halfturn(const uint32_t *, size_t word_count,
    uint64_t *source, uint64_t *destination, struct tg_gpu_present_halfturn *);
#endif
