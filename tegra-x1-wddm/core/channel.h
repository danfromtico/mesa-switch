/* SPDX-License-Identifier: MIT */
#ifndef TEGRA_GPU_CHANNEL_H
#define TEGRA_GPU_CHANNEL_H
#include "../platform/t214_power.h"
#include "../platform/t214_mmu.h"
#include "../platform/t214_frontend.h"

enum tg_gpu_channel_result {
    TG_GPU_CHANNEL_OK, TG_GPU_CHANNEL_ARGUMENT, TG_GPU_CHANNEL_NOT_READY,
    TG_GPU_CHANNEL_BUSY, TG_GPU_CHANNEL_TLB, TG_GPU_CHANNEL_IO,
    TG_GPU_CHANNEL_TIMEOUT, TG_GPU_CHANNEL_FAULT, TG_GPU_CHANNEL_FLUSH
};
struct tg_gpu_channel {
    const void *owner;
    const struct tg_gpu_power *power;
    uint64_t root;
    bool referenced, bound, submitted;
    uint32_t userd_offset, last_put;
    uint32_t stage, last_register, last_value;
    uint32_t fifo_interrupt, mmu_fault, pbdma_interrupt, pbdma_interrupt_1;
    struct tg_gpu_frontend_status frontend;
    struct tg_gpu_pri_timeout_snapshot pri_timeout;
};
/* Sequential read-only reset-time samples, not an atomic GPU snapshot.
 * flags: 1=enabled FIFO, 2=matching PBDMA slot before/after, 4=USERD alias,
 * 8=GR enabled. PBDMA words are attributable only with flag 2. */
struct tg_gpu_channel_snapshot {
    uint32_t flags,mc_enable,pbdma_enable,pbdma_before,pbdma_after;
    uint32_t userd_get,userd_put,userd_reference;
    uint32_t gp_get,gp_put,gp_fetch,pb_get_lo,pb_get_hi,pb_fetch_lo,pb_fetch_hi;
    uint32_t pb_header,method0,data0,engine0,engine1,gr_interrupt,gr_current;
};
void tg_gpu_channel_snapshot(const struct tg_gpu_power_io *,
    const struct tg_gpu_power *,const void *owner,const struct tg_gpu_channel *,
    struct tg_gpu_channel_snapshot *);
void tg_gpu_channel_snapshot_checked(const struct tg_gpu_power_io *,
    const struct tg_gpu_power *,const void *owner,const struct tg_gpu_channel *,
    struct tg_gpu_channel_snapshot *,const struct tg_gpu_commit_guard *);
/* Hot channel path after exclusive cold GPU/FIFO/LTC/MMU initialization and
 * clock setup. One bare channel/runlist/PBDMA (zero). Command and RAMIN bytes
 * must already be published; caller owns CPU/MMIO ordering and residency.
 * Bind starts with an empty GPFIFO. It does not publish GP_PUT: callers must
 * notify through the owned USERD BAR1 alias after the runlist is runnable.
 * The caller maps that alias and programs FIFO_BAR1_BASE before binding.
 */
typedef tg_gpu_abort_fn tg_gpu_channel_abort_fn;
enum tg_gpu_channel_result tg_gpu_channel_bind(const struct tg_gpu_power_io *,
    const struct tg_gpu_power *, uint64_t root, uint64_t instance, uint64_t runlist,
    const void *owner, struct tg_gpu_channel *);
enum tg_gpu_channel_result tg_gpu_channel_bind_checked(const struct tg_gpu_power_io *,
    const struct tg_gpu_power *, uint64_t root, uint64_t instance, uint64_t runlist,
    const void *owner, struct tg_gpu_channel *, const struct tg_gpu_commit_guard *);
/* One non-wrapping batch in the fixed 512-entry ring. Only increasing indices
 * 1..511 are accepted. The USERD page must match the active BAR1 snoop base.
 * A posted-write/readback failure may already have started DMA; retain owner
 * and all backing until retirement/reset, even when publication returns error.
 */
enum tg_gpu_channel_result tg_gpu_channel_publish(const struct tg_gpu_power_io *,
    const struct tg_gpu_power *, uint32_t userd_bar1_offset, uint32_t gp_put,
    const void *owner, struct tg_gpu_channel *);
enum tg_gpu_channel_result tg_gpu_channel_publish_checked(const struct tg_gpu_power_io *,
    const struct tg_gpu_power *, uint32_t userd_bar1_offset, uint32_t gp_put,
    const void *owner, struct tg_gpu_channel *, const struct tg_gpu_commit_guard *);
/* WDDM's two-entry batch: USER workload plus KERNEL completion packet. */
enum tg_gpu_channel_result tg_gpu_channel_submit(const struct tg_gpu_power_io *,
    const struct tg_gpu_power *, uint64_t root, uint64_t instance, uint64_t runlist,
    uint32_t userd_bar1_offset, const void *owner, struct tg_gpu_channel *);
/* Multi-entry WDDM batch (2..4 GPFIFO entries): USER prefix, optional USER
 * indirect segment and continuation suffix, plus KERNEL completion packet.
 */
enum tg_gpu_channel_result tg_gpu_channel_submit_entries(const struct tg_gpu_power_io *,
    const struct tg_gpu_power *, uint64_t root, uint64_t instance, uint64_t runlist,
    uint32_t userd_bar1_offset, uint32_t gp_put, const void *owner,
    struct tg_gpu_channel *);
enum tg_gpu_channel_result tg_gpu_channel_submit_entries_checked(const struct tg_gpu_power_io *,
    const struct tg_gpu_power *, uint64_t root, uint64_t instance, uint64_t runlist,
    uint32_t userd_bar1_offset, uint32_t gp_put, const void *owner,
    struct tg_gpu_channel *, const struct tg_gpu_commit_guard *);
/* Success means preempt/GR unload/runlist/unbind/FB flush/TLB invalidation
 * finished. GR unload is checked only when MC reports the GR engine enabled.
 * Failure retains referenced/owner: do not recycle any backing until retry
 * succeeds or the owning GPU has been drained and fully reset/powered down.
 */
enum tg_gpu_channel_result tg_gpu_channel_retire(const struct tg_gpu_power_io *,
    const struct tg_gpu_power *, const void *owner, struct tg_gpu_channel *);
enum tg_gpu_channel_result tg_gpu_channel_retire_checked(const struct tg_gpu_power_io *,
    const struct tg_gpu_power *, const void *owner, struct tg_gpu_channel *,
    const struct tg_gpu_commit_guard *);
/* Recovery precursor, not a release proof. Follow with MC drain and the
 * platform's full reset/power-down before dropping referenced/owner.
 */
void tg_gpu_channel_disable(const struct tg_gpu_power_io *, const struct tg_gpu_power *);
#endif
