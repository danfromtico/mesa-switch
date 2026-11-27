/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2014-2018 NVIDIA CORPORATION. All rights reserved.
 * Copyright 2026 ruje0504-2
 * C1 NA PLL startup, bypass relock and counter follow Switchroot nvgpu
 * clk_gm20b.c (MIT). T210B01 HighOPT characterization coefficients reflect
 * captured hardware table NAPLL En - p4v2-AggressiveHighOPT. See NOTICE.
 */
#include "t214_clock.h"
#include "t214_emc_gpu_floor.h"
#include "../core/identity.h"

#define GPU 0x57000000u
#define CFG 0x137000u
#define COEFF 0x137004u
#define CFG2 0x13700cu
#define CFG3 0x137018u
#define DVFS0 0x137010u
#define DVFS1 0x137014u
#define DVFS2 0x132820u
#define VCO 0x137100u
#define OUT 0x137250u
#define BYPASS 0x137340u
#define SLOW 0x020160u
#define THERM 0x020798u
#define SVOP 0x021944u
#define SVOP_OVERRIDE 0x021948u
#define COUNT_CFG 0x134124u
#define COUNT_VAL 0x134128u
#define ENABLE 1u
#define IDDQ 2u
#define SYNC 4u
#define OUT_VCODIV_MASK 0x00003f00u
#define OUT_VCODIV_BY1  0x00000000u
#define OUT_VCODIV_BY2  (2u << 8)

static const uint32_t clock_regs[TG_GPU_CLOCK_SAVED_REGS] = {
    CFG, COEFF, CFG2, CFG3, DVFS0, DVFS1, DVFS2, VCO, OUT, BYPASS,
    SVOP, SVOP_OVERRIDE, SLOW
};
/* Ignore read-only lock/calibration/testout bits when restoring control. */
static const uint32_t clock_masks[TG_GPU_CLOCK_SAVED_REGS] = {
    0x17, 0x3fffff, 0xff0000ff, 0x00ff01ff, 0x00007f7f,
    0x7fff0000, 0x0001007f, 1, 0x80003f3f, 1, 3, 1, 0x003f0000
};

static uint32_t clk_read(const struct tg_gpu_power_io *io, uint32_t reg)
{ return io->read32(io->context, GPU + reg); }

static void clk_write(const struct tg_gpu_power_io *io, uint32_t reg, uint32_t val)
{
    io->write32(io->context, GPU + reg, val);
    (void)clk_read(io, reg);
}

static void clk_update(const struct tg_gpu_power_io *io, uint32_t reg,
                       uint32_t mask, uint32_t val)
{ clk_write(io, reg, (clk_read(io, reg) & ~mask) | (val & mask)); }

static void clk_select(const struct tg_gpu_power_io *io, uint32_t vco)
{
    /* Switchroot masks throttling only across the mux update. */
    uint32_t saved = clk_read(io, THERM);
    clk_write(io, THERM, 0);
    clk_update(io, VCO, 1, vco);
    clk_write(io, THERM, saved);
}

static void clk_write_vcodiv_double(const struct tg_gpu_power_io *io,
    uint32_t val)
{
    /* Switchroot clk_program_gpc_pll (gm20b/clk_gm20b.c): intentional 2nd
     * back-to-back write to assure linear divider operation, then readback.
     */
    io->write32(io->context, GPU + OUT, val);
    io->write32(io->context, GPU + OUT, val);
    (void)clk_read(io, OUT);
}

static bool program_pll_under_bypass(const struct tg_gpu_power_io *io,
    uint32_t coeff, uint32_t dfs_coeff, int32_t ext_cal);

static int32_t div_nearest(int32_t n, int32_t d)
{ return n < 0 ? -((-n + d / 2) / d) : (n + d / 2) / d; }

struct tg_gpu_hiopt_entry {
    uint32_t khz;
    uint8_t n;
    uint8_t pl;
    int32_t c0, c1, c2, c3, c4, c5;
};

/* T210B01 GPUB01_NA_CVB_TABLE_HIOPT (gspd_id=3), speedo_scale=100,
 * thermal_scale=150, voltage_scale=1000. All (N, PL) pairs keep C1 VCO
 * (38.4 MHz * N) inside 1300..2600 MHz with SDM=-4096 (integer N).
 */
static const struct tg_gpu_hiopt_entry hiopt_table[TG_GPU_HIOPT_OPP_COUNT] = {
    {  76800u, 36u, 9u, 590000,     0,    0,     0,      0,      0 },
    { 153600u, 40u, 5u, 590000,     0,    0,     0,      0,      0 },
    { 230400u, 36u, 3u, 590000,     0,    0,     0,      0,      0 },
    { 307200u, 48u, 3u, 590000,     0,    0,     0,      0,      0 },
    { 384000u, 40u, 2u, 590000,     0,    0,     0,      0,      0 },
    { 460800u, 48u, 2u, 590000,     0,    0,     0,      0,      0 },
    { 537600u, 56u, 2u, 590000,     0,    0,     0,      0,      0 },
    { 614400u, 64u, 2u, 590000,     0,    0,     0,      0,      0 },
    { 691200u, 36u, 1u, 838712, -7304, -552,  1785, -56250,   -450 },
    { 768000u, 40u, 1u, 880210, -7955, -584,     0, -42735,   8775 },
    { 844800u, 44u, 1u, 926398, -8892, -602,  -900,  -5760, -20925 },
    { 921600u, 48u, 1u, 970060, -10108, -614, -2685, 22620, -2925 },
    { 998400u, 52u, 1u,1065665, -16075, -497, -2685, 48195,  2025 },
    {1075200u, 56u, 1u,1132576, -16093, -648,     0, 16155,  9000 },
    {1152000u, 60u, 1u,1180029, -14534, -830,     0, 22035, 24750 },
    {1228800u, 64u, 1u,1248293, -16383, -859,     0, 55830, 70425 },
};

static uint32_t pll_coeff_for_khz(uint32_t target_khz)
{
    uint32_t i;
    for (i = 0; i < TG_GPU_HIOPT_OPP_COUNT; ++i) {
        if (hiopt_table[i].khz == target_khz)
            return 1u | ((uint32_t)hiopt_table[i].n << 8) | ((uint32_t)hiopt_table[i].pl << 16);
    }
    if (target_khz == TG_GPU_FIXED_KHZ || target_khz == TG_GPU_OVERCLOCK_KHZ)
        return 0x00010001u | ((target_khz / 19200u) << 8);
    return 0;
}

uint32_t tg_gpu_clock_hiopt_freq_khz(uint32_t index)
{
    return index < TG_GPU_HIOPT_OPP_COUNT ? hiopt_table[index].khz : 0;
}

uint32_t tg_gpu_clock_hiopt_index(uint32_t target_khz)
{
    uint32_t i;
    for (i = 0; i < TG_GPU_HIOPT_OPP_COUNT; ++i)
        if (hiopt_table[i].khz == target_khz) return i;
    return UINT32_MAX;
}

uint32_t tg_gpu_clock_thermal_range(int32_t temp_c, uint32_t prev_range)
{
    static const int32_t trips[TG_GPU_THERMAL_RANGES - 1u] = { 20, 30, 50, 70, 90 };
    uint32_t r = 0;
    while (r < TG_GPU_THERMAL_RANGES - 1u && temp_c > trips[r])
        ++r;
    if (prev_range < TG_GPU_THERMAL_RANGES && r < prev_range) {
        /* 1 C downward hysteresis across thermal trip boundaries. */
        while (r < prev_range && temp_c > trips[r] - 1)
            ++r;
    }
    return r;
}

uint32_t tg_gpu_clock_emc_floor_uv(uint32_t gpu_speedo, uint32_t emc_khz)
{ return tg_gpu_emc_floor_policy_uv(gpu_speedo, emc_khz); }

static uint32_t round_cvb_mv_5mv(int32_t scaled_mv)
{
    /* MAX77812 alignment: offset_uv=250000, step_uv=5000, voltage_scale=1000. */
    int64_t scaled_uv = (int64_t)scaled_mv * 1000;
    int64_t offset = 250000LL * 1000LL;
    int64_t step = 5000LL * 1000LL;
    int64_t rem = scaled_uv > offset ? (scaled_uv - offset) : 0;
    int64_t steps = (rem + step - 1) / step;
    return (uint32_t)((steps * 5000LL + 250000LL) / 1000LL);
}

uint32_t tg_gpu_clock_hiopt_uv(uint32_t speedo, uint32_t target,
    uint32_t thermal_range, uint32_t emc_khz)
{
    static const int32_t vts_trips[TG_GPU_THERMAL_RANGES] = { -25, 20, 30, 50, 70, 90 };
    static const uint32_t vts_vmin_mv[TG_GPU_THERMAL_RANGES] = { 800u, 590u, 590u, 590u, 590u, 590u };
    uint32_t opp = tg_gpu_clock_hiopt_index(target);
    uint32_t i, j, row_mv = 0, uv, emc_uv;
    uint32_t ceiling_uv = target > 844800u ? TG_GPU_HIOPT_OC_MAX_UV : TG_GPU_SKU83_MAX_UV;
    if (!speedo || speedo > 5000u || opp == UINT32_MAX || thermal_range >= TG_GPU_THERMAL_RANGES)
        return UINT32_MAX;
    for (i = 0; i <= opp; ++i) {
        const struct tg_gpu_hiopt_entry *e = &hiopt_table[i];
        int32_t base = div_nearest(e->c2 * (int32_t)speedo, 100);
        uint32_t range_mv[TG_GPU_THERMAL_RANGES];
        base = div_nearest((base + e->c1) * (int32_t)speedo, 100) + e->c0;
        for (j = 0; j < TG_GPU_THERMAL_RANGES; ++j) {
            int32_t t = vts_trips[j];
            int32_t t_term = div_nearest(e->c3 * (int32_t)speedo, 100) + e->c4 +
                             div_nearest(e->c5 * t, 150);
            int32_t mvj = base + div_nearest(t_term * t, 150);
            uint32_t rounded = round_cvb_mv_5mv(mvj);
            if (rounded < vts_vmin_mv[j]) rounded = vts_vmin_mv[j];
            /* Upstream excludes an OPP if ANY temperature range exceeds
             * its rail ceiling, not just the currently selected range. */
            if (rounded * 1000u > ceiling_uv) return UINT32_MAX;
            range_mv[j] = rounded;
            if (j && range_mv[j - 1u] < rounded)
                range_mv[j - 1u] = rounded;
        }
        if (range_mv[thermal_range] > row_mv)
            row_mv = range_mv[thermal_range];
    }
    uv = row_mv * 1000u;
    emc_uv = tg_gpu_clock_emc_floor_uv(speedo, emc_khz);
    if (uv < emc_uv) uv = emc_uv;
    return uv <= ceiling_uv ? uv : UINT32_MAX;
}

uint32_t tg_gpu_clock_required_uv(uint32_t speedo, uint32_t target)
{
    int32_t base, peak = 0, t, c0, c1, c2, c4, c5;
    if (!speedo || speedo > 5000 ||
        (target != TG_GPU_FIXED_KHZ && target != TG_GPU_LOW_POWER_KHZ &&
         target != TG_GPU_OVERCLOCK_KHZ))
        return UINT32_MAX;
    /* The non-SLT 460.8 MHz CVB entry is 610 mV, independent of speedo and
     * temperature. Keep the same 800 mV fixed-profile floor as above it.
     */
    if (target == TG_GPU_LOW_POWER_KHZ) return 800000u;
    /* GPUB01_NA_CVB_TABLE at 768000: c0=891575,c1=-4409,c2=-584,
     * c3=0,c4=-2849,c5=39, speedo_scale=100, thermal_scale=10.
     * 729.6 MHz uses the next higher characterized frequency envelope.
     */
    if (target == TG_GPU_OVERCLOCK_KHZ) {
        c0 = 1277303; c1 = -11675; c2 = -859; c4 = 3722; c5 = 313;
    } else {
        c0 = 891575; c1 = -4409; c2 = -584; c4 = -2849; c5 = 39;
    }
    base = div_nearest(c2 * (int32_t)speedo, 100) + c1;
    base = div_nearest(base * (int32_t)speedo, 100) + c0;
    for (t = -40; t <= 105; ++t) {
        int32_t uv = base + div_nearest((div_nearest(c5 * t, 10) + c4) * t, 10);
        if (uv > peak) peak = uv;
    }
    if (peak < 800000) peak = 800000; /* Switchroot default VTS floor. */
    return ((uint32_t)peak + 4999u) / 5000u * 5000u;
}

enum tg_gpu_clock_result tg_gpu_clock_start(const struct tg_gpu_power_io *io,
    const struct tg_gpu_power *power, uint32_t target, struct tg_gpu_clock *c)
{
    uint32_t i, fuse, coeff, value, required_uv, mv;
    uint8_t rail, enabled;
    struct tg_gpu_identity id;
    if (!io || !power || !c || !io->read32 || !io->write32 ||
        !io->read_pmic || !io->write_pmic || !io->delay_us || !power->ready || c->owned ||
        (target != TG_GPU_FIXED_KHZ && target != TG_GPU_LOW_POWER_KHZ &&
         target != TG_GPU_OVERCLOCK_KHZ) ||
        !tg_gpu_identify(clk_read(io, 0), &id) || id.chipset != TG_GPU_CHIP_GM20B_B)
        return TG_GPU_CLOCK_BAD_STATE;
    c->stage = 0;
    /* Freshly reset GPU only. Existing clock owners are never displaced. */
    if ((clk_read(io, CFG) & ENABLE) || (clk_read(io, VCO) & 1))
        return TG_GPU_CLOCK_BAD_STATE;
    c->gpu_speedo = io->read32(io->context, 0x7000f930u);
    required_uv = tg_gpu_clock_required_uv(c->gpu_speedo, target);
    /* Conservative B01 table limit. Do not overvolt a part whose calculated
     * envelope would need more than 1050 mV. Only the GPU M1 rail changes.
     */
    if (required_uv > 1050000u) return TG_GPU_CLOCK_RAIL_ERROR;
    if (!io->read_pmic(io->context, power->pmic_address, 0x23, &rail) ||
        !io->read_pmic(io->context, power->pmic_address, 0x06, &enabled) ||
        !(enabled & 1))
        return TG_GPU_CLOCK_RAIL_ERROR;
    /* PLL is still disabled; program voltage before frequency. power_down
     * restores the pre-acquisition voltage after clocks/reset are stopped.
     */
    rail = (uint8_t)((required_uv - 250000u) / 5000u);
    if (!io->write_pmic(io->context, power->pmic_address, 0x23, rail) ||
        !io->read_pmic(io->context, power->pmic_address, 0x23, &enabled) || enabled != rail)
        return TG_GPU_CLOCK_RAIL_ERROR;
    io->delay_us(io->context, 5000);
    c->rail_uv = required_uv;
    c->target_khz = target;
    mv = required_uv / 1000u;
    for (i = 0; i < TG_GPU_CLOCK_SAVED_REGS; ++i)
        c->saved[i] = clk_read(io, clock_regs[i]);
    c->owned = true;
    c->running = false;
    c->stage = 1;
    clk_select(io, 0);
    clk_update(io, OUT, 0x80003f3f, 0x80000000);
    clk_update(io, BYPASS, 1, 0);
    if (!(clk_read(io, SVOP) & 3)) {
        clk_update(io, SVOP, 3, 2);
        clk_update(io, SVOP_OVERRIDE, 1, 1);
    }
    /* Switchroot disables idle slowdown during clock setup, not only while
     * measuring. Keep the fixed profile through command execution and restore
     * this field with the other controls when relinquishing the GPU.
     */
    clk_update(io, SLOW, 0x003f0000, 0);
    if (clk_read(io, SLOW) & 0x003f0000)
        return TG_GPU_CLOCK_READBACK_ERROR;

    /* NA mode is required on B01. Use C1 VCO_CTRL/DFS_CTRL, not B1. */
    clk_update(io, DVFS1, 0x2fff0000, 0x2c000000);
    clk_update(io, CFG3, 0x1ff, 0x0f);
    fuse = io->read32(io->context, 0x7000fb04u); /* FUSE + 0x100 + 0x204 */
    c->adc_slope_uv = (int32_t)(((fuse >> 24) & 0x3f) * 1000 + ((fuse >> 14) & 0x3ff));
    c->adc_intercept_uv = (int32_t)(((fuse >> 4) & 0x3ff) * 1000 + (fuse & 0xf) * 100);
    if (!(fuse >> 30) || !c->adc_slope_uv || !c->adc_intercept_uv) {
        c->adc_slope_uv = 0;
        c->adc_intercept_uv = 0;
    } else {
        clk_update(io, DVFS1, 1u << 30, 1u << 30);
    }
    clk_update(io, CFG, IDDQ, 0);
    io->delay_us(io->context, 5);
    clk_update(io, CFG2, 0xff000000, 0x04000000);
    clk_update(io, CFG3, 0x00ff0000, 0x00050000);
    c->stage = 2;
    if (!c->adc_slope_uv) {
        clk_update(io, DVFS1, 1u << 30, 1u << 30);
        clk_update(io, CFG, ENABLE, ENABLE);
        for (i = 0; i < 5; ++i) {
            if (clk_read(io, DVFS1) & (1u << 31)) break;
            io->delay_us(io->context, 1);
        }
        value = (clk_read(io, CFG3) >> 24) & 0x7f;
        clk_update(io, CFG, ENABLE, 0);
        if (i == 5) return TG_GPU_CLOCK_CALIBRATION_ERROR;
        c->adc_slope_uv = 10000;
        c->adc_intercept_uv = (int32_t)required_uv - (int32_t)value * 10000;
    }
    c->external_calibration = div_nearest((int32_t)required_uv - c->adc_intercept_uv, c->adc_slope_uv);
    if (c->external_calibration <= -64 || c->external_calibration >= 64)
        return TG_GPU_CLOCK_CALIBRATION_ERROR;

    c->stage = 3;
    /* C1 coefficient round((-172550 * 0.8 + 195374) / 1000) = 57.
     * DET_MAX=0 preserves the upper frequency bound if the rail rises.
     */
    value = (uint32_t)div_nearest(div_nearest(-172550 * (int32_t)mv, 1000) + 195374, 1000);
    if (value > 127) return TG_GPU_CLOCK_CALIBRATION_ERROR;
    /* SDM=-4096 (top byte f0) makes effective N equal integer N. C1's
     * VCO must stay in 1.3..2.6 GHz: 460.8 MHz therefore uses N=48, P=2,
     * VCO=1843.2 MHz, not N=24/P=1 with an out-of-range 921.6 MHz VCO.
     * The original 729.6/1228.8 MHz profiles retain N=38/64, P=1.
     * P is the divisor itself (clk_gm20b.h nvgpu_pl_to_div, 1ae0167d).
     */
    coeff = target == TG_GPU_LOW_POWER_KHZ ? 0x00023001u :
        0x00010001u | ((target / 19200u) << 8);
    if (!program_pll_under_bypass(io, coeff, value, c->external_calibration))
        return TG_GPU_CLOCK_READBACK_ERROR;
    c->stage = 4;
    c->running = true;
    return tg_gpu_clock_measure(io, c);
}

enum tg_gpu_clock_result tg_gpu_clock_measure(const struct tg_gpu_power_io *io,
    struct tg_gpu_clock *c)
{
    uint32_t slow, counter_cfg, first, second;
    if (!c->owned || !c->running) return TG_GPU_CLOCK_BAD_STATE;
    slow = clk_read(io, SLOW);
    counter_cfg = clk_read(io, COUNT_CFG);
    clk_update(io, SLOW, 0x3f0000, 0);
    clk_write(io, COUNT_CFG, 0x01000000);
    clk_write(io, COUNT_CFG, 0x00110000 | 800);
    io->delay_us(io->context, 200);
    first = clk_read(io, COUNT_VAL) & 0xfffff;
    io->delay_us(io->context, 100);
    second = clk_read(io, COUNT_VAL) & 0xfffff;
    clk_write(io, COUNT_CFG, counter_cfg);
    clk_write(io, SLOW, slow);
    c->counter = second;
    c->measured_khz = (uint32_t)((uint64_t)second * TG_GPU_REFERENCE_KHZ / 800);
    /* Counter's resolution is 48 kHz; tolerate 0.5% including oscillator
     * error. This is a live counter check, not a report of programmed N.
     */
    if (!first || first != second || c->measured_khz < c->target_khz * 995u / 1000u ||
        c->measured_khz > c->target_khz * 1005u / 1000u)
        return TG_GPU_CLOCK_COUNTER_ERROR;
    c->stage = 5;
    return TG_GPU_CLOCK_OK;
}

static bool program_pll_under_bypass(const struct tg_gpu_power_io *io,
    uint32_t coeff, uint32_t dfs_coeff, int32_t ext_cal)
{
    uint32_t old_pl = (clk_read(io, COEFF) >> 16) & 0x3fu;
    uint32_t new_pl = (coeff >> 16) & 0x3fu;
    bool pll_on_vco = (clk_read(io, CFG) & ENABLE) != 0u &&
                      (clk_read(io, VCO) & 1u) != 0u;
    uint32_t out;

    /* Switchroot clk_program_gpc_pll (gm20b/clk_gm20b.c): split FO-to-bypass
     * and bypass-to-FO jumps in half by setting OUT VCODIV (0x137250[13:8])
     * to 1:2 whenever either active old PL or target new PL is < 2 (1:1).
     * Direct 1:1 <=> 1:2 transition is not truly glitchless. Because this KMD
     * always bypass-relocks without NDIV sliding, PL=1 -> PL=1 changes also
     * cross bypass and therefore engage this divider transition. On cold start
     * (PLL off and already on bypass), only new_pl < 2 triggers the split.
     */
    if (new_pl < 2u || (pll_on_vco && old_pl < 2u)) {
        out = (clk_read(io, OUT) & ~OUT_VCODIV_MASK) | OUT_VCODIV_BY2;
        clk_write_vcodiv_double(io, out);
        io->delay_us(io->context, 2);
    }

    clk_select(io, 0);
    clk_update(io, CFG, SYNC, 0);
    clk_update(io, CFG, ENABLE, 0);
    clk_update(io, DVFS2, 1u << 16, 1u << 16);
    clk_update(io, DVFS0, 0x7f7f, dfs_coeff);
    io->delay_us(io->context, 1);
    clk_update(io, DVFS2, 1u << 16, 0);
    clk_update(io, DVFS2, 0x7f, (uint32_t)ext_cal);
    io->delay_us(io->context, 1);
    clk_update(io, DVFS1, 0x0fff0000, 0x0e010000);
    clk_update(io, CFG2, 0xff, 0xf0);
    clk_update(io, COEFF, 0x3fffff, coeff);
    clk_update(io, CFG, ENABLE, ENABLE);
    io->delay_us(io->context, 40); /* NA mode has no usable LOCK bit. */
    clk_update(io, CFG, SYNC, SYNC);
    clk_select(io, 1);

    /* Restore OUT VCODIV to 1:1 after returning to VCO (clk_program_gpc_pll):
     * wait 2 us at half rate, then double-write and read back OUT.
     */
    out = clk_read(io, OUT);
    if ((out & OUT_VCODIV_MASK) != OUT_VCODIV_BY1) {
        out = (out & ~OUT_VCODIV_MASK) | OUT_VCODIV_BY1;
        io->delay_us(io->context, 2);
        clk_write_vcodiv_double(io, out);
    }

    return (clk_read(io, COEFF) & 0x3fffff) == coeff &&
           (clk_read(io, CFG) & (ENABLE | SYNC | IDDQ)) == (ENABLE | SYNC) &&
           (clk_read(io, VCO) & 1) != 0 &&
           (clk_read(io, CFG2) & 0xff) == 0xf0 &&
           (clk_read(io, OUT) & OUT_VCODIV_MASK) == OUT_VCODIV_BY1;
}

static bool write_gpu_rail_checked(const struct tg_gpu_power_io *io,
    uint8_t pmic_address, uint32_t target_uv, uint32_t settle_us)
{
    uint8_t enabled = 0, readback = 0;
    uint8_t code = (uint8_t)((target_uv - 250000u) / 5000u);
    if (!io->read_pmic(io->context, pmic_address, 0x06, &enabled) || !(enabled & 1))
        return false;
    if (!io->write_pmic(io->context, pmic_address, 0x23, code) ||
        !io->read_pmic(io->context, pmic_address, 0x23, &readback) || readback != code)
        return false;
    io->delay_us(io->context, settle_us);
    return true;
}

static enum tg_gpu_clock_result reconcile_rail_error(const struct tg_gpu_power_io *io,
    uint8_t address, uint32_t minimum_uv, struct tg_gpu_clock *c)
{
    uint8_t enabled = 0, code = 0;
    /* An I2C write may have reached the regulator even when its completion or
     * readback failed. Never let a later F-up rely on the old cached voltage. */
    if (io->read_pmic(io->context, address, 0x06, &enabled) && (enabled & 1u) &&
        io->read_pmic(io->context, address, 0x23, &code)) {
        uint32_t actual_uv = 250000u + (uint32_t)code * 5000u;
        if (actual_uv >= minimum_uv && actual_uv <= 1050000u) {
            c->rail_uv = actual_uv;
            io->delay_us(io->context, 1000u);
            return TG_GPU_CLOCK_RAIL_ERROR;
        }
    }
    /* An unverified rail is not a usable clock state. The worker must stop
     * submitting and take its existing device-failure/reset path. */
    c->rail_uv = 0;
    c->running = false;
    return TG_GPU_CLOCK_RAIL_ERROR;
}

enum tg_gpu_clock_result tg_gpu_clock_set_rate(const struct tg_gpu_power_io *io,
    const struct tg_gpu_power *power, uint32_t target, uint32_t required_uv,
    struct tg_gpu_clock *c)
{
    uint32_t coeff, dfs_coeff, mv, old_khz, old_uv, old_coeff, old_dvfs0;
    int32_t ext_cal, old_cal;
    enum tg_gpu_clock_result res;
    if (!io || !power || !c || !io->read32 || !io->write32 || !io->read_pmic ||
        !io->write_pmic || !io->delay_us || !power->owned || !power->ready ||
        !c->owned || !c->running || !c->adc_slope_uv)
        return TG_GPU_CLOCK_BAD_STATE;
    coeff = pll_coeff_for_khz(target);
    if (!coeff) return TG_GPU_CLOCK_BAD_STATE;
    if (required_uv < TG_GPU_HIOPT_VMIN_UV || required_uv > 1050000u ||
        (required_uv - 250000u) % 5000u != 0)
        return TG_GPU_CLOCK_RAIL_ERROR;
    ext_cal = div_nearest((int32_t)required_uv - c->adc_intercept_uv, c->adc_slope_uv);
    mv = required_uv / 1000u;
    dfs_coeff = (uint32_t)div_nearest(div_nearest(-172550 * (int32_t)mv, 1000) + 195374, 1000);
    if (ext_cal <= -64 || ext_cal >= 64 || dfs_coeff > 127u)
        return TG_GPU_CLOCK_CALIBRATION_ERROR;
    if (target == c->target_khz && required_uv == c->rail_uv)
        return TG_GPU_CLOCK_OK;

    old_khz = c->target_khz;
    old_uv = c->rail_uv;
    old_cal = c->external_calibration;
    old_coeff = clk_read(io, COEFF) & 0x3fffff;
    old_dvfs0 = clk_read(io, DVFS0) & 0x7f7f;

    /* 1. Voltage UP first (with PMIC readback + ramp delay) before frequency UP. */
    if (required_uv > old_uv) {
        if (!write_gpu_rail_checked(io, power->pmic_address, required_uv, 1000u))
            return reconcile_rail_error(io, power->pmic_address, old_uv, c);
        c->rail_uv = required_uv;
    }

    /* 2. Reprogram NA GPCPLL under bypass and verify live GPCCLK counter. */
    if (!program_pll_under_bypass(io, coeff, dfs_coeff, ext_cal)) {
        res = TG_GPU_CLOCK_READBACK_ERROR;
    } else {
        c->target_khz = target;
        c->external_calibration = ext_cal;
        res = tg_gpu_clock_measure(io, c);
    }
    if (res != TG_GPU_CLOCK_OK) {
        /* Roll back PLL to the prior verified clock without lowering voltage. */
        c->target_khz = old_khz;
        c->external_calibration = old_cal;
        if (!program_pll_under_bypass(io, old_coeff, old_dvfs0, old_cal) ||
            tg_gpu_clock_measure(io, c) != TG_GPU_CLOCK_OK)
            c->running = false;
        return res;
    }

    /* 3. Voltage DOWN only AFTER register readback and live counter confirm F-down. */
    if (required_uv < old_uv) {
        if (!write_gpu_rail_checked(io, power->pmic_address, required_uv, 200u))
            return reconcile_rail_error(io, power->pmic_address, required_uv, c);
        c->rail_uv = required_uv;
    }
    return TG_GPU_CLOCK_OK;
}

enum tg_gpu_clock_result tg_gpu_clock_stop(const struct tg_gpu_power_io *io,
    struct tg_gpu_clock *c)
{
    uint32_t i;
    bool ok = true;
    if (!c->owned) return TG_GPU_CLOCK_OK;
    clk_select(io, 0);
    clk_update(io, CFG, SYNC, 0);
    clk_update(io, CFG, ENABLE, 0);
    c->running = false;
    /* Restore CFG last, so the original IDDQ state follows other registers. */
    for (i = TG_GPU_CLOCK_SAVED_REGS; i-- > 0; ) {
        clk_update(io, clock_regs[i], clock_masks[i], c->saved[i]);
        if ((clk_read(io, clock_regs[i]) & clock_masks[i]) !=
            (c->saved[i] & clock_masks[i])) ok = false;
    }
    if (!ok) return TG_GPU_CLOCK_READBACK_ERROR;
    c->owned = false;
    return TG_GPU_CLOCK_OK;
}
