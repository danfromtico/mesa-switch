/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef TEGRA_GPU_T214_POWER_H
#define TEGRA_GPU_T214_POWER_H

#include <stdbool.h>
#include <stdint.h>

/* The caller serializes shared CAR/PMC/MC and I2C5 accesses. This initial
 * interface acquires an idle, reset, clamped GPU only. It does not change
 * CPU/DDR voltage, PLLP itself, GPCPLL, memory mappings or GPU firmware.
 */
struct tg_gpu_power_io {
    void *context;
    uint32_t (*read32)(void *context, uint32_t address);
    void (*write32)(void *context, uint32_t address, uint32_t value);
    bool (*read_pmic)(void *context, uint8_t address, uint8_t reg, uint8_t *value);
    bool (*write_pmic)(void *context, uint8_t address, uint8_t reg, uint8_t value);
    void (*delay_us)(void *context, uint32_t delay);
    /* Called only after MC drain, clamp and reset have isolated the GPU.
     * A shared rail owner may retain M1 for EMC instead of restoring it. */
    bool (*release_pmic)(void *context, uint8_t address,
                         uint8_t original_enable, uint8_t original_voltage);
};

enum tg_gpu_power_result {
    TG_GPU_POWER_OK,
    TG_GPU_POWER_UNSUPPORTED_BOARD,
    TG_GPU_POWER_BUSY,
    TG_GPU_POWER_PMIC_ERROR,
    TG_GPU_POWER_CLOCK_ERROR,
    TG_GPU_POWER_RESET_ERROR,
    TG_GPU_POWER_MC_TIMEOUT,
    TG_GPU_POWER_BAD_ARGUMENT
};

enum tg_gpu_power_stage {
    TG_GPU_STAGE_SNAPSHOT,
    TG_GPU_STAGE_RAIL,
    TG_GPU_STAGE_CLOCKS,
    TG_GPU_STAGE_RESET,
    TG_GPU_STAGE_MC,
    TG_GPU_STAGE_READY
};

struct tg_gpu_power {
    bool owned;
    bool ready;
    uint8_t pmic_address;
    uint8_t original_enable;
    uint8_t original_voltage;
    uint32_t original_clocks;
    uint32_t original_pllp_out5;
    uint32_t original_mc_flush;
    enum tg_gpu_power_stage stage;
};

/* Zero-initialize state before the first call. Even a failed power_up may
 * own partially changed resources; call power_down before releasing them.
 * A failed MC drain keeps ownership, so live GPU memory cannot be reclaimed.
 */
enum tg_gpu_power_result tg_gpu_power_up(const struct tg_gpu_power_io *io,
                                        uint8_t pmic_address,
                                        struct tg_gpu_power *state);
enum tg_gpu_power_result tg_gpu_power_down(const struct tg_gpu_power_io *io,
                                          struct tg_gpu_power *state);
/* Channel fetch must already be stopped. Successful drain holds the GPU
 * MC client in reset while the caller stops its PLL before power_down.
 */
enum tg_gpu_power_result tg_gpu_power_drain(const struct tg_gpu_power_io *io,
                                           const struct tg_gpu_power *state);

#endif
