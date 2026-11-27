/* SPDX-License-Identifier: MIT */
#include "paging_dma.h"
#include "gm20b_mmu.h"
#include "nouveau/headers/drf.h"
#include "nouveau/headers/nvidia/classes/clb06f.h"
#include "nouveau/headers/nvidia/classes/clb0b5.h"

/* NVK uses subchannel 4 and 128 KiB linear rows on Maxwell. */
#define CE_SUBCHANNEL 4u
#define ROW_BYTES (128u * 1024u)

bool tg_gpu_paging_dma_valid(const struct tg_gpu_paging_dma *op)
{
    if (!op || !op->bytes || op->bytes > TG_GPU_PAGING_DMA_MAX_BYTES ||
        !op->destination || op->destination >= TG_GPU_VA_LIMIT ||
        op->bytes > TG_GPU_VA_LIMIT - op->destination)
        return false;
    if (op->fill)
        return !op->source && !((op->bytes | op->destination) & 3);
    if (op->pattern || !op->source || op->source >= TG_GPU_VA_LIMIT ||
        op->bytes > TG_GPU_VA_LIMIT - op->source)
        return false;
    return op->destination + op->bytes <= op->source ||
           op->source + op->bytes <= op->destination;
}

static uint32_t header(uint32_t method, uint32_t count)
{
    return NVDEF(NVB06F, DMA_INCR, OPCODE, VALUE) |
        NVVAL(NVB06F, DMA_INCR, SUBCHANNEL, CE_SUBCHANNEL) |
        NVVAL(NVB06F, DMA_INCR, ADDRESS, method >> 2) |
        NVVAL(NVB06F, DMA_INCR, COUNT, count);
}

uint32_t tg_gpu_encode_paging_dma(uint32_t words[TG_GPU_PAGING_DMA_MAX_WORDS],
    const struct tg_gpu_paging_dma *op)
{
    if (!words || !tg_gpu_paging_dma_valid(op))
        return 0;

    uint32_t *p = words;
    uint64_t source = op->source, destination = op->destination, left = op->bytes;
    uint32_t launch = NVDEF(NVB0B5, LAUNCH_DMA, DATA_TRANSFER_TYPE, PIPELINED) |
        NVDEF(NVB0B5, LAUNCH_DMA, FLUSH_ENABLE, TRUE) |
        NVDEF(NVB0B5, LAUNCH_DMA, SRC_MEMORY_LAYOUT, PITCH) |
        NVDEF(NVB0B5, LAUNCH_DMA, DST_MEMORY_LAYOUT, PITCH);
    *p++ = header(0, 1);
    *p++ = MAXWELL_DMA_COPY_A;
    if (op->fill) {
        *p++ = header(NVB0B5_SET_REMAP_CONST_A, 3);
        *p++ = op->pattern;
        *p++ = 0; /* constant B is unused */
        *p++ = NVDEF(NVB0B5, SET_REMAP_COMPONENTS, DST_X, CONST_A) |
            NVDEF(NVB0B5, SET_REMAP_COMPONENTS, DST_Y, CONST_A) |
            NVDEF(NVB0B5, SET_REMAP_COMPONENTS, DST_Z, CONST_A) |
            NVDEF(NVB0B5, SET_REMAP_COMPONENTS, DST_W, CONST_A) |
            NVDEF(NVB0B5, SET_REMAP_COMPONENTS, COMPONENT_SIZE, FOUR);
        launch |= NVDEF(NVB0B5, LAUNCH_DMA, REMAP_ENABLE, TRUE);
    }
    while (left) {
        uint32_t row = left < ROW_BYTES ? (uint32_t)left : ROW_BYTES;
        uint32_t lines = (uint32_t)(left / row);
        uint64_t consumed = (uint64_t)row * lines;
        *p++ = header(NVB0B5_OFFSET_IN_UPPER, 8);
        *p++ = (uint32_t)(source >> 32);
        *p++ = (uint32_t)source;
        *p++ = (uint32_t)(destination >> 32);
        *p++ = (uint32_t)destination;
        *p++ = row; /* pitch in bytes */
        *p++ = row;
        *p++ = op->fill ? row / 4 : row;
        *p++ = lines;
        *p++ = header(NVB0B5_LAUNCH_DMA, 1);
        *p++ = launch | (lines > 1 ? NVDEF(NVB0B5, LAUNCH_DMA, MULTI_LINE_ENABLE, TRUE) : 0);
        if (!op->fill) source += consumed;
        destination += consumed;
        left -= consumed;
    }
    return (uint32_t)(p - words);
}
