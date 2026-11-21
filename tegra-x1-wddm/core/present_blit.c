/* SPDX-License-Identifier: MIT */
#include "present_blit.h"
#include "gm20b_mmu.h"
#include "nouveau/headers/drf.h"
#include "nouveau/headers/nvidia/classes/clb06f.h"
#include "nouveau/headers/nvidia/classes/clb0b5.h"

static bool rect_valid(struct tg_gpu_blit_rect r, uint32_t width, uint32_t height)
{
    return r.left >= 0 && r.top >= 0 && r.right > r.left && r.bottom > r.top &&
        (uint32_t)r.right <= width && (uint32_t)r.bottom <= height;
}

static bool surface_valid(struct tg_gpu_blit_surface s)
{
    return s.width && s.height && s.width <= INT32_MAX && s.height <= INT32_MAX &&
        (uint64_t)s.width * 4 <= s.pitch && !(s.pitch & 3) &&
        (uint64_t)s.pitch * s.height < TG_GPU_VA_LIMIT;
}

bool tg_gpu_present_blit_init(struct tg_gpu_present_blit *p,
    struct tg_gpu_blit_surface source, struct tg_gpu_blit_surface destination,
    struct tg_gpu_blit_rect src, struct tg_gpu_blit_rect dst, uint32_t rotation)
{
    if (!p || !surface_valid(source) || !surface_valid(destination) ||
        (rotation != 0 && rotation != 90 && rotation != 180 && rotation != 270))
        return false;
    const bool quarter = rotation == 90 || rotation == 270;
    const uint32_t width = quarter ? destination.height : destination.width;
    const uint32_t height = quarter ? destination.width : destination.height;
    if (!rect_valid(src, source.width, source.height) || !rect_valid(dst, width, height) ||
        src.right - src.left != dst.right - dst.left ||
        src.bottom - src.top != dst.bottom - dst.top)
        return false; /* No stretching is advertised by this GDI path. */
    *p = (struct tg_gpu_present_blit){source, destination, src, dst, rotation, width, height};
    return true;
}

uint32_t tg_gpu_present_blit_stripes(const struct tg_gpu_present_blit *p,
    struct tg_gpu_blit_rect r)
{
    if (!p || !rect_valid(r, p->logical_width, p->logical_height) ||
        r.left < p->destination_rect.left || r.top < p->destination_rect.top ||
        r.right > p->destination_rect.right || r.bottom > p->destination_rect.bottom)
        return 0;
    return p->rotation == 90 ? (uint32_t)(r.right - r.left) :
        p->rotation == 270 ? (uint32_t)(r.bottom - r.top) : 1;
}

bool tg_gpu_present_blit_copy(const struct tg_gpu_present_blit *p,
    struct tg_gpu_blit_rect r, uint32_t stripe, struct tg_gpu_present_copy *copy)
{
    if (!copy || stripe >= tg_gpu_present_blit_stripes(p, r) || p->rotation == 180)
        return false;
    uint32_t sx = p->source_rect.left + (r.left - p->destination_rect.left);
    uint32_t sy = p->source_rect.top + (r.top - p->destination_rect.top);
    uint32_t dx = r.left, dy = r.top;
    *copy = (struct tg_gpu_present_copy){.source_pitch = p->source.pitch,
        .destination_pitch = p->destination.pitch, .row_bytes = (uint32_t)(r.right - r.left) * 4,
        .lines = r.bottom - r.top};
    if (p->rotation == 90) {
        sx += stripe;
        dx = r.top;
        dy = p->logical_width - 1 - (r.left + stripe);
        copy->destination_pitch = 4;
        copy->row_bytes = 4;
    } else if (p->rotation == 270) {
        sy += stripe;
        dx = p->logical_height - 1 - (r.top + stripe);
        dy = r.left;
        copy->source_pitch = 4;
        copy->row_bytes = 4;
        copy->lines = r.right - r.left;
    }
    copy->source_offset = (uint64_t)sy * p->source.pitch + (uint64_t)sx * 4;
    copy->destination_offset = (uint64_t)dy * p->destination.pitch + (uint64_t)dx * 4;
    return true;
}

bool tg_gpu_present_blit_halfturn(const struct tg_gpu_present_blit *p,
    struct tg_gpu_blit_rect r, struct tg_gpu_present_halfturn *out)
{
    if (!out || !tg_gpu_present_blit_stripes(p, r) || p->rotation != 180)
        return false;
    *out = (struct tg_gpu_present_halfturn){p->source, p->destination,
        p->source_rect.left + r.right - p->destination_rect.left - 1,
        p->source_rect.top + r.bottom - p->destination_rect.top - 1,
        p->logical_width - r.right, p->logical_height - r.bottom,
        r.right - r.left, r.bottom - r.top};
    return true;
}

static uint32_t header(uint32_t method, uint32_t count)
{
    return NVDEF(NVB06F, DMA_INCR, OPCODE, VALUE) |
        NVVAL(NVB06F, DMA_INCR, SUBCHANNEL, 4) |
        NVVAL(NVB06F, DMA_INCR, ADDRESS, method >> 2) |
        NVVAL(NVB06F, DMA_INCR, COUNT, count);
}

uint32_t tg_gpu_encode_present_copy(uint32_t *words, uint32_t capacity,
    uint64_t source, uint64_t destination, const struct tg_gpu_present_copy *copy, bool opaque,
    bool cpu_readback)
{
    const uint32_t count = opaque ? 17 : 13;
    if (!words || !copy || capacity < count || !copy->row_bytes || !copy->lines ||
        (copy->row_bytes & 3) || copy->source_pitch < copy->row_bytes ||
        copy->destination_pitch < copy->row_bytes ||
        source >= TG_GPU_VA_LIMIT || destination >= TG_GPU_VA_LIMIT)
        return 0;
    const uint64_t from = source + copy->source_offset, to = destination + copy->destination_offset;
    const uint64_t source_bytes = (uint64_t)(copy->lines - 1) * copy->source_pitch + copy->row_bytes;
    const uint64_t destination_bytes = (uint64_t)(copy->lines - 1) * copy->destination_pitch + copy->row_bytes;
    if (from < source || to < destination || !from || !to || from >= TG_GPU_VA_LIMIT ||
        to >= TG_GPU_VA_LIMIT || source_bytes > TG_GPU_VA_LIMIT - from ||
        destination_bytes > TG_GPU_VA_LIMIT - to)
        return 0;
    uint32_t *p = words;
    *p++ = header(0, 1); *p++ = MAXWELL_DMA_COPY_A;
    if (opaque) {
        *p++ = header(NVB0B5_SET_REMAP_CONST_A, 3); *p++ = 0xff; *p++ = 0;
        *p++ = NVDEF(NVB0B5, SET_REMAP_COMPONENTS, DST_X, SRC_X) |
            NVDEF(NVB0B5, SET_REMAP_COMPONENTS, DST_Y, SRC_Y) |
            NVDEF(NVB0B5, SET_REMAP_COMPONENTS, DST_Z, SRC_Z) |
            NVDEF(NVB0B5, SET_REMAP_COMPONENTS, DST_W, CONST_A) |
            NVDEF(NVB0B5, SET_REMAP_COMPONENTS, COMPONENT_SIZE, ONE) |
            NVDEF(NVB0B5, SET_REMAP_COMPONENTS, NUM_SRC_COMPONENTS, FOUR) |
            NVDEF(NVB0B5, SET_REMAP_COMPONENTS, NUM_DST_COMPONENTS, FOUR);
    }
    *p++ = header(NVB0B5_OFFSET_IN_UPPER, 8);
    *p++ = (uint32_t)(from >> 32); *p++ = (uint32_t)from;
    *p++ = (uint32_t)(to >> 32); *p++ = (uint32_t)to;
    *p++ = copy->source_pitch; *p++ = copy->destination_pitch;
    *p++ = opaque ? copy->row_bytes / 4 : copy->row_bytes; *p++ = copy->lines;
    *p++ = header(NVB0B5_LAUNCH_DMA, 1);
    /* A CPU lock may follow the scheduler fence for a staging surface. */
    *p++ = (cpu_readback ? NVDEF(NVB0B5, LAUNCH_DMA, DATA_TRANSFER_TYPE, NON_PIPELINED) :
                          NVDEF(NVB0B5, LAUNCH_DMA, DATA_TRANSFER_TYPE, PIPELINED)) |
        NVDEF(NVB0B5, LAUNCH_DMA, FLUSH_ENABLE, TRUE) |
        NVDEF(NVB0B5, LAUNCH_DMA, SRC_MEMORY_LAYOUT, PITCH) |
        NVDEF(NVB0B5, LAUNCH_DMA, DST_MEMORY_LAYOUT, PITCH) |
        NVDEF(NVB0B5, LAUNCH_DMA, MULTI_LINE_ENABLE, TRUE) |
        (opaque ? NVDEF(NVB0B5, LAUNCH_DMA, REMAP_ENABLE, TRUE) : 0);
    return (uint32_t)(p - words);
}
