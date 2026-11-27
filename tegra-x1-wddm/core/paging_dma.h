/* SPDX-License-Identifier: MIT */
#ifndef TG_GPU_PAGING_DMA_H
#define TG_GPU_PAGING_DMA_H
#include <stdbool.h>
#include <stdint.h>

/* One WDDM operation uses at most two CE launches (whole rows and tail). */
#define TG_GPU_PAGING_DMA_MAX_WORDS 32u
#define TG_GPU_PAGING_DMA_MAX_BYTES (UINT64_C(512) * 1024 * 1024)
struct tg_gpu_paging_dma {
    uint64_t source, destination, bytes;
    uint32_t pattern;
    bool fill;
};

bool tg_gpu_paging_dma_valid(const struct tg_gpu_paging_dma *);
/* Virtual, linear GM20B B0B5 copy/fill. Caller owns the system root and
 * backing and appends its WFI/Reference packet through the common channel.
 */
uint32_t tg_gpu_encode_paging_dma(uint32_t words[TG_GPU_PAGING_DMA_MAX_WORDS],
    const struct tg_gpu_paging_dma *);
#endif
