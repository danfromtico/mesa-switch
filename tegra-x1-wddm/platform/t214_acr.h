/* SPDX-License-Identifier: MIT */
#ifndef TEGRA_GPU_T214_ACR_H
#define TEGRA_GPU_T214_ACR_H
#include "t214_power.h"
#include "../core/acr_firmware.h"

enum tg_gpu_acr_hw_result {
    TG_GPU_ACR_HW_OK, TG_GPU_ACR_HW_ARGUMENT, TG_GPU_ACR_HW_REGISTER,
    TG_GPU_ACR_HW_SECURITY, TG_GPU_ACR_HW_WPR, TG_GPU_ACR_HW_IDLE_TIMEOUT,
    TG_GPU_ACR_HW_RESET, TG_GPU_ACR_HW_SCRUB_TIMEOUT, TG_GPU_ACR_HW_MEMORY,
    TG_GPU_ACR_HW_HALT_TIMEOUT, TG_GPU_ACR_HW_AUTHENTICATION
};
struct tg_gpu_acr_hw_status {
    uint32_t stage, result, security, hwcfg, imem_size, dmem_size;
    uint32_t mc_enable, mailbox0, mailbox1, cpuctl, sctl;
    uint32_t last_register, last_value, reset_confirmed;
    uint32_t scp_status;
    uint64_t wpr_base, wpr_size;
};
/* Cold, exclusively owned GPU with CPU interrupt outputs disabled. Discover
 * existing WPR through its selector (never alter the carveout), reset PMU and
 * wait for scrub/idle. The returned WPR/DMEM values feed tg_gpu_acr_pack().
 * No firmware runs in this phase. Every exit still requires full GPU cleanup.
 */
enum tg_gpu_acr_hw_result tg_gpu_acr_hw_prepare(const struct tg_gpu_power_io *,
    const struct tg_gpu_power *, struct tg_gpu_acr_hw_status *);
/* The caller packs the authenticated vendor image into owned backing, maps it
 * into the supplied private PMU instance and completes CPU/GPU visibility
 * before calling. PMU executes its vendor HS bootloader and ACR; neither a
 * successful HALT/mailbox nor this API starts the PMU RTOS or proves GR work.
 * Keep all backing until PMU is stopped AND the enclosing GPU is reset/drained.
 */
enum tg_gpu_acr_hw_result tg_gpu_acr_hw_boot(const struct tg_gpu_power_io *,
    const struct tg_gpu_power *, const struct tg_gpu_acr_image *,
    const uint32_t boot_descriptor[19], uint64_t instance_physical,
    struct tg_gpu_acr_hw_status *);
bool tg_gpu_acr_hw_stop(const struct tg_gpu_power_io *,
    const struct tg_gpu_power *, struct tg_gpu_acr_hw_status *);
#endif
