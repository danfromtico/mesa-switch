/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2014-2019, NVIDIA CORPORATION. All rights reserved.
 * Copyright (C) 2010 Google, Inc.
 * Copyright (C) 2011-2021 NVIDIA Corporation. All rights reserved.
 * Copyright 2026 ruje0504-2
 * Power ordering follows Switchroot nvgpu 1ae0167d, platform_gk20a_tegra.c
 * (NVIDIA, GPL-2.0); MC drain follows Switchroot's tegra_mc_flush().
 * MAX77812 M1 is the B01 GPU rail, as documented by the Switchroot B01 DT
 * and Hekate bdk/power/max77812.h (CTCaer, GPL-2.0).
 */
#include "t214_power.h"

#define CAR_BASE        0x60006000u
#define CAR_PLLP        (CAR_BASE + 0x0a0u)
#define CAR_CLOCK_X     (CAR_BASE + 0x280u)
#define CAR_CLOCK_SET   (CAR_BASE + 0x284u)
#define CAR_CLOCK_CLR   (CAR_BASE + 0x288u)
#define CAR_RESET_X     (CAR_BASE + 0x28cu)
#define CAR_RESET_SET   (CAR_BASE + 0x290u)
#define CAR_RESET_CLR   (CAR_BASE + 0x294u)
#define CAR_PLLP_OUT5   (CAR_BASE + 0x67cu)
#define GPU_GATE        (1u << 24) /* Tegra clock/reset 184 */
#define GPU_REF         (1u << 29) /* Tegra clock 189 */
#define GPU_CLOCKS      (GPU_GATE | GPU_REF)
#define OUT5_MASK       0xff070000u
/* Switchroot clk-tegra-periph.c uses TEGRA_DIVIDER_FIXED for OUT5.
 * clk-divider.c sets its override at (divider shift 24 - 6) = bit 18.
 * Ratio 2 gives 408 MHz * 2 / (2 + 2) = 204 MHz; retain enable/reset.
 */
#define OUT5_204MHZ     0x02070000u
#define PMC_GPU_CLAMP   0x7000e6d4u
#define MC_FLUSH        0x70019970u
#define MC_FLUSH_STATUS 0x70019974u
#define MC_GPU_BIT      (1u << 2) /* MC client 34 in the second bank */
#define PMIC_ENABLE     0x06u
#define PMIC_M1_VOLTAGE 0x23u
#define PMIC_M1_ENABLE 0x01u
#define M1_800MV        110u /* 250 mV + 110 * 5 mV; B01 DT init voltage */

static void update32(const struct tg_gpu_power_io *io, uint32_t address,
                     uint32_t mask, uint32_t value)
{
    uint32_t old = io->read32(io->context, address);
    io->write32(io->context, address, (old & ~mask) | (value & mask));
    (void)io->read32(io->context, address);
}

static bool write_pmic_checked(const struct tg_gpu_power_io *io, uint8_t address,
                               uint8_t reg, uint8_t value)
{
    uint8_t readback;
    return io->write_pmic(io->context, address, reg, value) &&
           io->read_pmic(io->context, address, reg, &readback) && readback == value;
}

static bool set_m1_enable(const struct tg_gpu_power_io *io,
                          struct tg_gpu_power *state, bool enable)
{
    uint8_t value;
    if (!io->read_pmic(io->context, state->pmic_address, PMIC_ENABLE, &value))
        return false;
    value = (uint8_t)((value & ~PMIC_M1_ENABLE) | (enable ? PMIC_M1_ENABLE : 0));
    return write_pmic_checked(io, state->pmic_address, PMIC_ENABLE, value);
}

static bool drain_mc(const struct tg_gpu_power_io *io)
{
    unsigned int attempt, sample;
    uint32_t request;
    update32(io, MC_FLUSH, MC_GPU_BIT, MC_GPU_BIT);
    request = io->read32(io->context, MC_FLUSH);
    if (request == UINT32_MAX || !(request & MC_GPU_BIT)) return false;
    for (attempt = 0; attempt < 100000; ++attempt) {
        uint32_t value;
        io->delay_us(io->context, 10);
        value = io->read32(io->context, MC_FLUSH_STATUS);
        if (value == UINT32_MAX) return false;
        if (!(value & MC_GPU_BIT))
            continue;
        for (sample = 0; sample < 5; ++sample)
            if (io->read32(io->context, MC_FLUSH_STATUS) != value)
                break;
        if (sample == 5)
            return true;
    }
    return false;
}

enum tg_gpu_power_result
tg_gpu_power_up(const struct tg_gpu_power_io *io, uint8_t pmic_address,
                struct tg_gpu_power *state)
{
    uint32_t pllp;
    if (!io || !state || !io->read32 || !io->write32 || !io->read_pmic ||
        !io->write_pmic || !io->delay_us || (pmic_address != 0x31 && pmic_address != 0x33))
        return TG_GPU_POWER_BAD_ARGUMENT;
    if (state->owned)
        return TG_GPU_POWER_BUSY;
    state->stage = TG_GPU_STAGE_SNAPSHOT;
    if ((io->read32(io->context, 0x70000804u) & 0xfff0u) != 0x2120u)
        return TG_GPU_POWER_UNSUPPORTED_BOARD;
    /* Do not reset or borrow an adapter already running somebody's channel. */
    if (!(io->read32(io->context, CAR_RESET_X) & GPU_GATE) ||
        !(io->read32(io->context, PMC_GPU_CLAMP) & 1u))
        return TG_GPU_POWER_BUSY;
    pllp = io->read32(io->context, CAR_PLLP);
    if ((pllp & ((1u << 31) | (1u << 30) | (1u << 27))) != ((1u << 30) | (1u << 27)))
        return TG_GPU_POWER_CLOCK_ERROR;
    state->pmic_address = pmic_address;
    if (!io->read_pmic(io->context, pmic_address, PMIC_ENABLE, &state->original_enable) ||
        !io->read_pmic(io->context, pmic_address, PMIC_M1_VOLTAGE, &state->original_voltage))
        return TG_GPU_POWER_PMIC_ERROR;
    state->original_clocks = io->read32(io->context, CAR_CLOCK_X);
    state->original_pllp_out5 = io->read32(io->context, CAR_PLLP_OUT5);
    state->original_mc_flush = io->read32(io->context, MC_FLUSH);
    /* Hekate can leave OUT5 enabled at its reset divider (0x00030000).
     * Switchroot's gk20a_tegra_get_clocks() calls clk_set_rate(204 MHz)
     * before using this GPU power clock. Set our output below while the
     * GPU is still reset/clamped, rather than requiring Linux to have run.
     */
    state->owned = true;
    state->ready = false;
    state->stage = TG_GPU_STAGE_RAIL;
    if (!write_pmic_checked(io, pmic_address, PMIC_M1_VOLTAGE, M1_800MV) ||
        !set_m1_enable(io, state, true))
        return TG_GPU_POWER_PMIC_ERROR;
    io->delay_us(io->context, 5000);

    state->stage = TG_GPU_STAGE_CLOCKS;
    update32(io, CAR_PLLP_OUT5, OUT5_MASK, OUT5_204MHZ);
    io->delay_us(io->context, 2); /* clk_frac_div_set_rate() fence_udelay */
    if ((io->read32(io->context, CAR_PLLP_OUT5) & OUT5_MASK) != OUT5_204MHZ)
        return TG_GPU_POWER_CLOCK_ERROR;
    io->write32(io->context, CAR_CLOCK_SET, GPU_CLOCKS);
    io->delay_us(io->context, 10);
    if ((io->read32(io->context, CAR_CLOCK_X) & GPU_CLOCKS) != GPU_CLOCKS)
        return TG_GPU_POWER_CLOCK_ERROR;

    state->stage = TG_GPU_STAGE_RESET;
    io->write32(io->context, CAR_RESET_SET, GPU_GATE);
    io->delay_us(io->context, 10);
    update32(io, PMC_GPU_CLAMP, 1u, 0);
    io->delay_us(io->context, 10);
    io->write32(io->context, CAR_CLOCK_CLR, GPU_GATE);
    io->write32(io->context, CAR_RESET_CLR, GPU_GATE);
    io->write32(io->context, CAR_CLOCK_SET, GPU_GATE);
    if ((io->read32(io->context, CAR_RESET_X) & GPU_GATE) ||
        (io->read32(io->context, PMC_GPU_CLAMP) & 1u) ||
        (io->read32(io->context, CAR_CLOCK_X) & GPU_CLOCKS) != GPU_CLOCKS)
        return TG_GPU_POWER_RESET_ERROR;

    state->stage = TG_GPU_STAGE_MC;
    if (!drain_mc(io))
        return TG_GPU_POWER_MC_TIMEOUT;
    io->delay_us(io->context, 10);
    update32(io, MC_FLUSH, MC_GPU_BIT, 0);
    io->delay_us(io->context, 10);
    state->ready = true;
    state->stage = TG_GPU_STAGE_READY;
    return TG_GPU_POWER_OK;
}

enum tg_gpu_power_result
tg_gpu_power_drain(const struct tg_gpu_power_io *io, const struct tg_gpu_power *state)
{
    if (!state->owned || !state->ready) return TG_GPU_POWER_BAD_ARGUMENT;
    return drain_mc(io) ? TG_GPU_POWER_OK : TG_GPU_POWER_MC_TIMEOUT;
}

enum tg_gpu_power_result
tg_gpu_power_down(const struct tg_gpu_power_io *io, struct tg_gpu_power *state)
{
    bool ok;
    uint32_t clamp, reset;
    if (!state->owned)
        return TG_GPU_POWER_OK;
    if (state->stage >= TG_GPU_STAGE_MC && !drain_mc(io))
        return TG_GPU_POWER_MC_TIMEOUT;
    update32(io, PMC_GPU_CLAMP, 1u, 1u);
    io->delay_us(io->context, 10);
    clamp = io->read32(io->context, PMC_GPU_CLAMP);
    if (clamp == UINT32_MAX || !(clamp & 1u)) return TG_GPU_POWER_RESET_ERROR;
    io->write32(io->context, CAR_RESET_SET, GPU_GATE);
    io->delay_us(io->context, 10);
    reset = io->read32(io->context, CAR_RESET_X);
    if (reset == UINT32_MAX || !(reset & GPU_GATE)) return TG_GPU_POWER_RESET_ERROR;
    /* Keep MC blocked and backing owned until clamp + reset are observed.
     * A posted write alone cannot justify freeing memory used by GPU DMA. */
    io->write32(io->context, CAR_CLOCK_CLR, GPU_CLOCKS & ~state->original_clocks);
    io->write32(io->context, CAR_CLOCK_SET, GPU_CLOCKS & state->original_clocks);
    update32(io, CAR_PLLP_OUT5, OUT5_MASK, state->original_pllp_out5);
    update32(io, MC_FLUSH, MC_GPU_BIT, state->original_mc_flush);
    if (io->release_pmic) {
        ok = io->release_pmic(io->context, state->pmic_address,
                              state->original_enable, state->original_voltage);
    } else {
        /* Read-modify-write EN_CTRL so M3 (DDR), M4 (CPU), and other bits survive. */
        ok = set_m1_enable(io, state, (state->original_enable & PMIC_M1_ENABLE) != 0);
        ok = write_pmic_checked(io, state->pmic_address, PMIC_M1_VOLTAGE,
                                state->original_voltage) && ok;
    }
    io->delay_us(io->context, 5000);
    state->ready = false;
    if (!ok)
        return TG_GPU_POWER_PMIC_ERROR;
    state->owned = false;
    return TG_GPU_POWER_OK;
}
