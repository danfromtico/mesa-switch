/* SPDX-License-Identifier: MIT */
#ifndef TEGRA_GPU_T214_CLOCK_H
#define TEGRA_GPU_T214_CLOCK_H

#include "t214_power.h"

/* Fixed bring-up profile, not a governor. GPCCLK = 38.4 MHz * 38 / 2. */
#define TG_GPU_FIXED_KHZ 729600u
#define TG_GPU_LOW_POWER_KHZ 460800u
#define TG_GPU_OVERCLOCK_KHZ 1228800u
#define TG_GPU_REFERENCE_KHZ 38400u
#define TG_GPU_CLOCK_SAVED_REGS 13u

/* T210B01 NAPLL En - p4v2-AggressiveHighOPT (gspd_id=3) OPP ladder. */
#define TG_GPU_HIOPT_OPP_COUNT 16u
#define TG_GPU_HIOPT_MIN_KHZ   76800u
#define TG_GPU_HIOPT_IDLE_KHZ   76800u
#define TG_GPU_HIOPT_SAFE_KHZ  460800u
#define TG_GPU_HIOPT_DOCK_KHZ  768000u
#define TG_GPU_HIOPT_MAX_KHZ  1228800u
#define TG_GPU_THERMAL_RANGES  6u
#define TG_GPU_SKU83_MAX_UV    800000u
#define TG_GPU_HIOPT_OC_MAX_UV 900000u /* Switchroot HighOPT characterization limit */
#define TG_GPU_HIOPT_VMIN_UV   590000u

/* The low-power control is an explicit build choice, not a live governor. */
#ifndef TG_GPU_RENDER_KHZ
#define TG_GPU_RENDER_KHZ TG_GPU_FIXED_KHZ
#endif
#if TG_GPU_RENDER_KHZ != TG_GPU_FIXED_KHZ && TG_GPU_RENDER_KHZ != TG_GPU_LOW_POWER_KHZ
#error Unsupported render clock profile
#endif

enum tg_gpu_clock_result {
    TG_GPU_CLOCK_OK, TG_GPU_CLOCK_BAD_STATE, TG_GPU_CLOCK_RAIL_ERROR,
    TG_GPU_CLOCK_CALIBRATION_ERROR, TG_GPU_CLOCK_READBACK_ERROR,
    TG_GPU_CLOCK_COUNTER_ERROR
};

struct tg_gpu_clock {
    bool owned;
    bool running;
    uint32_t saved[TG_GPU_CLOCK_SAVED_REGS];
    uint32_t measured_khz;
    uint32_t target_khz;
    uint32_t rail_uv;
    uint32_t counter;
    uint32_t stage;
    int32_t adc_slope_uv;
    int32_t adc_intercept_uv;
    int32_t external_calibration;
    uint32_t gpu_speedo;
};

/* Requires an idle GPU just acquired by power_up, no live DMA and exclusive
 * register ownership. Zero state first. Uses the C1 NA PLL path, as required
 * by Switchroot on T210B01. Only GPU M1 voltage is set, before PLL startup;
 * power_down restores it after the clock is stopped. CPU/DDR are untouched.
 * Stop channels and drain their writes before calling clock_stop.
 */
enum tg_gpu_clock_result tg_gpu_clock_start(const struct tg_gpu_power_io *io,
    const struct tg_gpu_power *power, uint32_t target_khz, struct tg_gpu_clock *clock);
enum tg_gpu_clock_result tg_gpu_clock_measure(const struct tg_gpu_power_io *io,
    struct tg_gpu_clock *clock);
/* Quiescent live C1 NA GPCPLL transition. Caller must hold exclusive dispatch
 * ownership with all channels retired/idle. Enforces V-up + PMIC readback +
 * ramp delay before F-up, and F-down + register/counter verification before
 * V-down, rolling back PLL state without lowering rail voltage on failure.
 */
enum tg_gpu_clock_result tg_gpu_clock_set_rate(const struct tg_gpu_power_io *io,
    const struct tg_gpu_power *power, uint32_t target_khz, uint32_t required_uv,
    struct tg_gpu_clock *clock);
enum tg_gpu_clock_result tg_gpu_clock_stop(const struct tg_gpu_power_io *io,
    struct tg_gpu_clock *clock);

/* Conservative non-SLT 460.8/768/1228.8 MHz CVB envelopes, covering -40..105 C,
 * rounded to the MAX77812 5 mV step. Non-default profiles are opt-in.
 */
uint32_t tg_gpu_clock_required_uv(uint32_t gpu_speedo, uint32_t target_khz);

/* Captured T210B01 NAPLL En - p4v2-AggressiveHighOPT (gspd_id=3) helpers:
 *   thermal_range 0..5 corresponds to <=20, 20..30, 30..50, 50..70, 70..90, >90 C
 *   with 1 C downward hysteresis and vdd-gpu(emc) floor.
 */
uint32_t tg_gpu_clock_hiopt_freq_khz(uint32_t index);
uint32_t tg_gpu_clock_hiopt_index(uint32_t target_khz);
uint32_t tg_gpu_clock_thermal_range(int32_t temp_c, uint32_t prev_range);
uint32_t tg_gpu_clock_emc_floor_uv(uint32_t gpu_speedo, uint32_t emc_khz);
uint32_t tg_gpu_clock_hiopt_uv(uint32_t gpu_speedo, uint32_t target_khz,
    uint32_t thermal_range, uint32_t emc_khz);

#endif
