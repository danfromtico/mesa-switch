/* SPDX-License-Identifier: MIT */
#ifndef TEGRA_GPU_ALLOCATION_ABI_H
#define TEGRA_GPU_ALLOCATION_ABI_H
#include <stdint.h>

#define TG_GPU_ALLOCATION_ABI 1u
#define TG_GPU_ALLOCATION_CPU_VISIBLE 1u
#define TG_GPU_ALLOCATION_DISPLAY_PRIMARY 2u
/* Shared KMD/UMD description of GPU storage. DISPLAY_PRIMARY selects the
 * fixed native scanout format and contiguous VidMm segment. Neither request
 * supplies an address. GPU VA alignment is chosen when mapping the allocation.
 * Host-visible backing is initially write-combined; cached/coherent memory
 * types are not advertised by this request.
 */
struct tg_gpu_allocation_request {
    uint32_t version, flags;
    uint64_t bytes;
};
_Static_assert(sizeof(struct tg_gpu_allocation_request)==16,"allocation ABI");
#endif
