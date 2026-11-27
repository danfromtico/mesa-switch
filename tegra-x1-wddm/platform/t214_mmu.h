/* SPDX-License-Identifier: MIT */
#ifndef TEGRA_GPU_T214_MMU_H
#define TEGRA_GPU_T214_MMU_H
#include "t214_power.h"
enum tg_gpu_tlb_result { TG_GPU_TLB_OK, TG_GPU_TLB_ARGUMENT, TG_GPU_TLB_NOT_READY,
                         TG_GPU_TLB_NO_SPACE, TG_GPU_TLB_TIMEOUT, TG_GPU_TLB_IO_ERROR,
                         TG_GPU_TLB_FAULT };
/* Arbitrated hardware commit guard (V5-01/B04/B11/N3):
 * - abort() checks latched fault/epoch before and after each register access;
 * - acquire() arbitrates software commit ownership at DIRQL with zero MMIO;
 * - write32() issues an individual MMIO write at caller IRQL (outside DIRQL),
 *   recording adjacent register and enter/done breadcrumbs around the access. */
typedef bool (*tg_gpu_abort_fn)(void *context);
typedef bool (*tg_gpu_commit_acquire_fn)(void *context);
typedef void (*tg_gpu_commit_write32_fn)(void *context,
    const struct tg_gpu_power_io *io,
    uint32_t reg, uint32_t val, bool final_step);
struct tg_gpu_commit_guard {
    tg_gpu_abort_fn abort;
    tg_gpu_commit_acquire_fn acquire;
    tg_gpu_commit_write32_fn write32;
    void *context;
};
/* Caller owns GPU power, serializes the invalidate FIFO and has made all
 * page-table writes visible. A timeout is a failure; do not recycle pages.
 */
enum tg_gpu_tlb_result tg_gpu_tlb_invalidate(const struct tg_gpu_power_io *io,
    const struct tg_gpu_power *power, uint64_t root_device_address);
enum tg_gpu_tlb_result tg_gpu_tlb_invalidate_checked(const struct tg_gpu_power_io *io,
    const struct tg_gpu_power *power, uint64_t root_device_address,
    const struct tg_gpu_commit_guard *guard);
#endif
