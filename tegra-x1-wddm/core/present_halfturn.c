/* SPDX-License-Identifier: MIT AND Zlib
 * Adapted, restricted 180-degree copy using deko3d's Blit2DEngine setup and
 * corner-origin pixel centres, revision 373bfd43d8b2d805fa2eb7d406dbb823032df958.
 * Copyright (C) 2018-2020 fincs. See ../NOTICE and ../LICENSES/Zlib.txt.
 * Unlike the general upstream blitter, only linear BGRA/XRGB and unit negative
 * derivatives are admitted. No shader, scratch allocation, or CPU copy.
 */
#include "present_halfturn.h"
#include "gm20b_mmu.h"
#include "nouveau/headers/drf.h"
#include "nouveau/headers/nvidia/classes/clb06f.h"
#include "nouveau/headers/nvidia/classes/cl902d.h"

static uint32_t header(uint32_t method, uint32_t count)
{
    return NVDEF(NVB06F, DMA_INCR, OPCODE, VALUE) |
        NVVAL(NVB06F, DMA_INCR, SUBCHANNEL, 3) |
        NVVAL(NVB06F, DMA_INCR, ADDRESS, method >> 2) |
        NVVAL(NVB06F, DMA_INCR, COUNT, count);
}

static bool surface_valid(struct tg_gpu_blit_surface s, uint64_t va)
{
    const uint64_t bytes = (uint64_t)s.pitch * s.height;
    return s.width && s.height && s.width <= 16384 && s.height <= 16384 &&
        (uint64_t)s.width * 4 <= s.pitch && !(s.pitch & 31) && va && !(va & 255) &&
        va < TG_GPU_VA_LIMIT && bytes <= TG_GPU_VA_LIMIT - va;
}

uint32_t tg_gpu_encode_present_halfturn(uint32_t *words, uint32_t capacity,
    uint64_t source, uint64_t destination, const struct tg_gpu_present_halfturn *b, bool opaque)
{
    if (!words || !b || capacity < TG_GPU_PRESENT_HALFTURN_WORDS ||
        !surface_valid(b->source, source) || !surface_valid(b->destination, destination) ||
        !b->width || !b->height || b->source_x >= b->source.width || b->source_y >= b->source.height ||
        b->width > b->source_x + 1 || b->height > b->source_y + 1 ||
        b->destination_x >= b->destination.width || b->destination_y >= b->destination.height ||
        b->width > b->destination.width - b->destination_x ||
        b->height > b->destination.height - b->destination_y ||
        (source < destination + (uint64_t)b->destination.pitch * b->destination.height &&
         destination < source + (uint64_t)b->source.pitch * b->source.height))
        return 0;
    uint32_t *p = words;
#define M(method, value) do { *p++ = header(NV902D_##method, 1); *p++ = (value); } while (0)
    *p++ = header(0, 1); *p++ = FERMI_TWOD_A;
    *p++ = header(NV902D_SET_DST_FORMAT, 2);
    *p++ = NV902D_SET_DST_FORMAT_V_A8R8G8B8; *p++ = NV902D_SET_DST_MEMORY_LAYOUT_V_PITCH;
    *p++ = header(NV902D_SET_DST_PITCH, 5);
    *p++ = b->destination.pitch; *p++ = b->destination.width; *p++ = b->destination.height;
    *p++ = (uint32_t)(destination >> 32); *p++ = (uint32_t)destination;
    *p++ = header(NV902D_SET_SRC_FORMAT, 2);
    *p++ = opaque ? NV902D_SET_SRC_FORMAT_V_X8R8G8B8 : NV902D_SET_SRC_FORMAT_V_A8R8G8B8;
    *p++ = NV902D_SET_SRC_MEMORY_LAYOUT_V_PITCH;
    *p++ = header(NV902D_SET_SRC_PITCH, 5);
    *p++ = b->source.pitch; *p++ = b->source.width; *p++ = b->source.height;
    *p++ = (uint32_t)(source >> 32); *p++ = (uint32_t)source;
    M(SET_RENDER_ENABLE_C, NV902D_SET_RENDER_ENABLE_C_MODE_TRUE);
    M(SET_CLIP_ENABLE, 0); M(SET_COLOR_KEY_ENABLE, 0);
    M(SET_OPERATION, NV902D_SET_OPERATION_V_SRCCOPY);
    M(SET_COMPRESSION, 0); M(SET_DST_COLOR_RENDER_TO_ZETA_SURFACE, 0);
    M(SET_RENDER_ENABLE_OVERRIDE, NV902D_SET_RENDER_ENABLE_OVERRIDE_MODE_USE_RENDER_ENABLE);
    M(SET_PIXELS_FROM_MEMORY_DIRECTION, 0);
    M(SET_PIXELS_FROM_MEMORY_CORRAL_SIZE, 0x3f);
    M(SET_PIXELS_FROM_MEMORY_BLOCK_SHAPE, NV902D_SET_PIXELS_FROM_MEMORY_BLOCK_SHAPE_V_AUTO);
    M(SET_PIXELS_FROM_MEMORY_SAFE_OVERLAP, 0);
    M(SET_PIXELS_FROM_MEMORY_SAMPLE_MODE, NV902D_SET_PIXELS_FROM_MEMORY_SAMPLE_MODE_ORIGIN_CORNER);
#undef M
    *p++ = header(NV902D_SET_PIXELS_FROM_MEMORY_DST_X0, 12);
    *p++ = b->destination_x; *p++ = b->destination_y; *p++ = b->width; *p++ = b->height;
    /* Signed 32.32 derivatives -1, -1. Start at the last source pixel's
     * centre (x + 0.5, y + 0.5), as deko3d's FlipX|FlipY does. */
    *p++ = 0; *p++ = UINT32_MAX; *p++ = 0; *p++ = UINT32_MAX;
    *p++ = 0x80000000; *p++ = b->source_x; *p++ = 0x80000000; *p++ = b->source_y;
    return (uint32_t)(p - words);
}

bool tg_gpu_decode_present_halfturn(const uint32_t *words, size_t count,
    uint64_t *source, uint64_t *destination, struct tg_gpu_present_halfturn *out)
{
    if (!words || count < TG_GPU_PRESENT_HALFTURN_WORDS || !source || !destination || !out)
        return false;
    const struct tg_gpu_present_halfturn b = {
        {words[16], words[17], words[15]}, {words[7], words[8], words[6]},
        words[54], words[56], words[45], words[46], words[47], words[48]
    };
    const uint64_t src = ((uint64_t)words[18] << 32) | words[19];
    const uint64_t dst = ((uint64_t)words[9] << 32) | words[10];
    uint32_t expected[TG_GPU_PRESENT_HALFTURN_WORDS];
    if (tg_gpu_encode_present_halfturn(expected, TG_GPU_PRESENT_HALFTURN_WORDS, src, dst, &b,
        words[12] == NV902D_SET_SRC_FORMAT_V_X8R8G8B8) != TG_GPU_PRESENT_HALFTURN_WORDS)
        return false;
    /* Do not enable arbitrary 902D methods. Every state, derivative, packet
     * boundary and format is part of this closed operation. */
    for (unsigned i = 0; i < TG_GPU_PRESENT_HALFTURN_WORDS; ++i)
        if (words[i] != expected[i]) return false;
    *source = src; *destination = dst; *out = b;
    return true;
}
