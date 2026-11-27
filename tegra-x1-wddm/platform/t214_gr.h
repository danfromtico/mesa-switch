/* SPDX-License-Identifier: MIT */
#ifndef TG_GPU_T214_GR_H
#define TG_GPU_T214_GR_H
#include "t214_power.h"
#include "t214_frontend.h"
#include "t214_pmu.h"
#include "../core/gr_firmware.h"
enum tg_gpu_gr_result {
    TG_GPU_GR_OK, TG_GPU_GR_ARGUMENT, TG_GPU_GR_SECURITY_REQUIRED,
    TG_GPU_GR_REGISTER, TG_GPU_GR_SCRUB_TIMEOUT, TG_GPU_GR_ARB_TIMEOUT,
    TG_GPU_GR_DMA_TIMEOUT, TG_GPU_GR_READY_TIMEOUT, TG_GPU_GR_PRI_FAULT,
    TG_GPU_GR_FIRMWARE_FORMAT, TG_GPU_GR_IDLE_TIMEOUT, TG_GPU_GR_CONTEXT_TIMEOUT
};
struct tg_gpu_gr_status {
    uint32_t stage, result, priv_security, last_register, last_value;
    uint32_t fecs_hwcfg, gpccs_hwcfg, fecs_mailbox, gr_interrupt;
    uint32_t current_context, context_bytes, zcull_bytes, pm_bytes;
    uint32_t fecs_host_interrupt, fecs_ctxsw_status, fecs_error_mailbox, new_context;
};
/* Read while GR is owned and clocked, including after channel submission.
 * A pre-submission ready reply is not a snapshot of a later channel failure. */
void tg_gpu_gr_snapshot(const struct tg_gpu_power_io *io, struct tg_gpu_gr_status *status);
bool tg_gpu_gr_snapshot_checked(const struct tg_gpu_power_io *io,
    struct tg_gpu_gr_status *status, const struct tg_gpu_commit_guard *guard);
/* Non-secure Switchroot DMA bootstrap. Caller owns a cold GR with its reset/
 * access preamble complete, a valid instance/PDB and an immutable image mapped
 * read-only at gpu_va. No runnable channels may exist. This call never changes
 * security fuses, starts a secure Falcon or frees DMA memory. On any return,
 * buffers stay owned until the caller drains/resets/powers down the whole GPU.
 * A firmware-ready handshake is not a graphics context or shader-execution pass.
 */
enum tg_gpu_gr_result tg_gpu_gr_boot(const struct tg_gpu_power_io *io,
    const struct tg_gpu_power *power, struct tg_gpu_frontend_status *frontend,
    const struct tg_gpu_gr_netlist *netlist, const struct tg_gpu_gr_image *image,
    uint64_t gpu_va, uint64_t instance_physical, struct tg_gpu_gr_status *status);
/* Cold secure GM20B path: ACR has authenticated/loaded FECS, PMU RTOS has
 * acknowledged WPR. Never overwrite FECS code or weaken its security mode.
 * GPCCS uses the non-secure loader selected by Switchroot's gm20b fuse policy.
 */
enum tg_gpu_gr_result tg_gpu_gr_boot_secure(const struct tg_gpu_power_io *io,
    const struct tg_gpu_power *power, struct tg_gpu_frontend_status *frontend,
    const struct tg_gpu_gr_netlist *netlist, const struct tg_gpu_gr_image *image,
    uint64_t gpu_va, uint64_t instance_physical,
    const struct tg_gpu_acr_hw_status *acr, const struct tg_gpu_pmu_status *pmu,
    struct tg_gpu_gr_status *status);
/* Mirror the initialized FB translation/debug policy into GPCs after FECS
 * context-state discovery, before channel admission. This configures routing;
 * it does not allocate or save a graphics context.
 */
enum tg_gpu_gr_result tg_gpu_gr_gpc_mmu(const struct tg_gpu_power_io *io,
    const struct tg_gpu_power *power, struct tg_gpu_frontend_status *frontend,
    struct tg_gpu_gr_status *status);
#endif
