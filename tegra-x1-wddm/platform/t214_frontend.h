/* SPDX-License-Identifier: MIT */
#ifndef TEGRA_GPU_T214_FRONTEND_H
#define TEGRA_GPU_T214_FRONTEND_H
#include <stdint.h>

struct tg_gpu_power_io;
struct tg_gpu_power;
struct tg_gpu_commit_guard;
enum tg_gpu_frontend_result {
    TG_GPU_FE_OK, TG_GPU_FE_ARGUMENT, TG_GPU_FE_REGISTER,
    TG_GPU_FE_LTC, TG_GPU_FE_FLUSH_TIMEOUT, TG_GPU_FE_PRI_FAULT,
    TG_GPU_FE_L2_TIMEOUT, TG_GPU_FE_TOPOLOGY, TG_GPU_FE_FAULT
};
/* Fixed-size diagnostic appended to the control ABI; overwritten per run. */
struct tg_gpu_frontend_status {
    uint32_t stage, result, ltc_count, fb_flushes;
    uint32_t bus_interrupt, pri_status0, pri_status1;
    uint32_t pri_error_address, pri_error_code;
    uint32_t l2_flushes, l2_invalidations;
    uint32_t gr_enable, gr_fifo_access, gr_gate_ctrl[2];
};
/* Non-destructive PBUS-gated read-only sample of PTIMER PRI timeout latches
 * (Switchroot nvgpu 1ae0167d gk20a_bus_isr -> gk20a_ptimer_isr: 0x9084,
 * 0x9088, and 0x908c when save0 bit 31 FECS_TGT is set). Kept separate from
 * tg_gpu_frontend_status to preserve TG_GPU_CONTROL_VERSION ABI; never clears
 * 0x9084/0x9088 during cold start or observation.
 */
struct tg_gpu_pri_timeout_snapshot {
    uint32_t bus_interrupt;
    uint32_t save0;
    uint32_t save1;
    uint32_t fecs_errcode;
    uint32_t valid;
    uint32_t fecs_valid;
};

/* Cold, exclusively owned GPU only. Order: start -> GPC clock -> FIFO reset
 * -> fifo -> memory -> MMU/BAR1 binding -> flush -> runnable channel.
 * These controls are retired by the caller's full GPU reset/power-down.
 */
enum tg_gpu_frontend_result tg_gpu_frontend_start(const struct tg_gpu_power_io *,
    const struct tg_gpu_power *, struct tg_gpu_frontend_status *);
enum tg_gpu_frontend_result tg_gpu_frontend_fifo(const struct tg_gpu_power_io *,
    const struct tg_gpu_power *, struct tg_gpu_frontend_status *);
enum tg_gpu_frontend_result tg_gpu_frontend_memory(const struct tg_gpu_power_io *,
    const struct tg_gpu_power *, struct tg_gpu_frontend_status *);
enum tg_gpu_frontend_result tg_gpu_frontend_flush(const struct tg_gpu_power_io *,
    const struct tg_gpu_power *, struct tg_gpu_frontend_status *);
enum tg_gpu_frontend_result tg_gpu_frontend_flush_checked(const struct tg_gpu_power_io *,
    const struct tg_gpu_power *, struct tg_gpu_frontend_status *,
    const struct tg_gpu_commit_guard *);
/* Cold GR reset/access preamble, after MMU/BAR1 and before any channel bind.
 * No Falcon is started; GR firmware/context/non-context setup remains separate.
 * The caller owns the whole GPU and must reset/power it down on every exit.
 */
enum tg_gpu_frontend_result tg_gpu_frontend_gr_prepare(const struct tg_gpu_power_io *,
    const struct tg_gpu_power *, struct tg_gpu_frontend_status *);
/* Release the T214 copy engine sharing runlist zero before channel admission.
 * TOP must identify CE2/reset 21 on that runlist. Caller resets owned_mask
 * before draining/freeing any backing, including on a partial failure. */
enum tg_gpu_frontend_result tg_gpu_frontend_ce_prepare(const struct tg_gpu_power_io *,
    const struct tg_gpu_power *, struct tg_gpu_frontend_status *, uint32_t *owned_mask);
/* FB flush only orders writes into L2. Drain dirty lines to DRAM, then
 * invalidate clean lines before comparing CPU and BAR1 aliases. The caller
 * serializes these operations and owns the entire GPU during this probe.
 */
enum tg_gpu_frontend_result tg_gpu_frontend_sync_memory(const struct tg_gpu_power_io *,
    const struct tg_gpu_power *, struct tg_gpu_frontend_status *);
enum tg_gpu_frontend_result tg_gpu_frontend_sync_memory_checked(const struct tg_gpu_power_io *,
    const struct tg_gpu_power *, struct tg_gpu_frontend_status *,
    const struct tg_gpu_commit_guard *);
void tg_gpu_frontend_snapshot(const struct tg_gpu_power_io *,
    struct tg_gpu_frontend_status *);
void tg_gpu_frontend_snapshot_checked(const struct tg_gpu_power_io *,
    struct tg_gpu_frontend_status *, const struct tg_gpu_commit_guard *);
void tg_gpu_frontend_pri_timeout_snapshot(const struct tg_gpu_power_io *,
    uint32_t bus_interrupt, struct tg_gpu_pri_timeout_snapshot *);
void tg_gpu_frontend_pri_timeout_snapshot_checked(const struct tg_gpu_power_io *,
    uint32_t bus_interrupt, struct tg_gpu_pri_timeout_snapshot *,
    const struct tg_gpu_commit_guard *);
#endif
