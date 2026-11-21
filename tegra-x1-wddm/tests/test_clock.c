/* SPDX-License-Identifier: MIT */
#include "../platform/t214_clock.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

enum model_event_kind {
    EV_PMIC_WRITE = 1,
    EV_PLL_BYPASS = 2,
    EV_PLL_SELECT = 3,
    EV_COUNTER    = 4,
};

struct model_event {
    enum model_event_kind kind;
    uint32_t value;
};

enum out_trace_kind {
    TR_OUT_READ  = 1,
    TR_OUT_WRITE = 2,
    TR_VCO_WRITE = 3,
    TR_DELAY_US  = 4,
};

struct out_trace_entry {
    enum out_trace_kind kind;
    uint32_t value;
};

static uint32_t registers[0x140000 / 4];
static bool counter_bad, calibration_timeout, slowdown_stuck, live_mode, pmic_fail;
static bool pmic_fail_after_write, out_restore_stuck, trace_out;
static uint32_t pmic_read_failures;
static uint32_t fail_counter_for_khz;
static uint32_t gpu_speedo_fuse;
static uint32_t adc_fuse;
static uint32_t writes;
static uint8_t rail;
static uint32_t target_khz;
static struct model_event events[64];
static uint32_t event_count;
static struct out_trace_entry out_trace[128];
static uint32_t out_trace_count;

static void push_event(enum model_event_kind kind, uint32_t value)
{
    if (event_count < sizeof(events) / sizeof(events[0]))
        events[event_count++] = (struct model_event){ kind, value };
}

static void push_out_trace(enum out_trace_kind kind, uint32_t value)
{
    if (trace_out && out_trace_count < sizeof(out_trace) / sizeof(out_trace[0]))
        out_trace[out_trace_count++] = (struct out_trace_entry){ kind, value };
}

static uint32_t read_reg(void *ctx, uint32_t address)
{
    (void)ctx;
    if (address == 0x7000f930) return gpu_speedo_fuse;
    if (address == 0x7000fb04) return adc_fuse;
    assert(address >= 0x57000000 && address < 0x57140000);
    if (address == 0x57137250)
        push_out_trace(TR_OUT_READ, registers[0x137250 / 4]);
    return registers[(address - 0x57000000) / 4];
}

static void write_reg(void *ctx, uint32_t address, uint32_t value)
{
    (void)ctx;
    assert(address >= 0x57000000 && address < 0x57140000);
    ++writes;
    if (address == 0x57020160 && slowdown_stuck && !(value & 0x003f0000)) return;
    if (address == 0x57137250) {
        push_out_trace(TR_OUT_WRITE, value);
        if (out_restore_stuck && (registers[0x137250 / 4] & 0x3f00u) != 0u &&
            (value & 0x3f00u) == 0u)
            return;
    }
    if (address == 0x57137100)
        push_out_trace(TR_VCO_WRITE, value & 1u);
    if (address == 0x57137100 && live_mode) {
        if ((value & 1u) == 0)
            push_event(EV_PLL_BYPASS, 0);
        else {
            uint32_t coeff = registers[0x137004 / 4];
            uint32_t m = coeff & 0xff, n = (coeff >> 8) & 0xff, p = (coeff >> 16) & 0x3f;
            assert(m == 1 && p);
            push_event(EV_PLL_SELECT, 38400u * n / m / p / 2u);
        }
    }
    registers[(address - 0x57000000) / 4] = value;
    if (address == 0x57137000 && (value & 1) && !calibration_timeout) {
        registers[0x137014 / 4] |= 1u << 31;
        registers[0x137018 / 4] |= 20u << 24;
    }
    if (address == 0x57134124 && value == 0x00110320) {
        uint32_t coeff = registers[0x137004 / 4];
        uint32_t m = coeff & 0xff, n = (coeff >> 8) & 0xff, p = (coeff >> 16) & 0x3f;
        uint32_t gpc_khz;
        assert(m == 1 && p);
        uint32_t vco = 38400u * n / m;
        gpc_khz = vco / p / 2;
        /* Counter observes GPCCLK (half the PLL), not GPC2CLK. */
        assert(!(registers[0x20160 / 4] & 0x003f0000));
        assert((registers[0x137250 / 4] & 0x3f00u) == 0u);
        assert(vco >= 1300000 && vco <= 2600000);
        if (!live_mode) {
            assert(gpc_khz == target_khz);
            assert((registers[0x132820 / 4] & 0x7f) == 20);
        }
        assert((registers[0x13700c / 4] & 0xff) == 0xf0);
        assert((registers[0x137014 / 4] & 0x2fff0000) == 0x2e010000);
        if (live_mode) push_event(EV_COUNTER, gpc_khz);
        registers[0x134128 / 4] = (counter_bad || gpc_khz == fail_counter_for_khz) ?
            100 : gpc_khz / 48;
    }
}

static bool read_pmic(void *ctx, uint8_t address, uint8_t reg, uint8_t *value)
{
    (void)ctx;
    assert(address == 0x33);
    if (reg == 0x23 && pmic_read_failures) {
        --pmic_read_failures;
        return false;
    }
    *value = reg == 0x23 ? rail : 0x41;
    return true;
}

static bool write_pmic(void *ctx, uint8_t addr, uint8_t reg, uint8_t value)
{
    (void)ctx;
    assert(addr == 0x33 && reg == 0x23);
    if (!live_mode)
        assert(!(registers[0x137000 / 4] & 1));
    if (pmic_fail)
        return false;
    rail = value;
    if (live_mode)
        push_event(EV_PMIC_WRITE, 250000u + (uint32_t)value * 5000u);
    return !pmic_fail_after_write;
}

static void delay(void *ctx, uint32_t us)
{
    (void)ctx;
    push_out_trace(TR_DELAY_US, us);
}

static void reset(void)
{
    memset(registers, 0, sizeof(registers));
    registers[0] = 0x12e000b1;
    registers[0x137000 / 4] = 2;
    registers[0x20798 / 4] = 0x105;
    registers[0x20160 / 4] = 0x00220000;
    gpu_speedo_fuse = 1400;
    adc_fuse = 0;
    rail = 110;
    target_khz = TG_GPU_FIXED_KHZ;
    counter_bad = calibration_timeout = slowdown_stuck = live_mode = pmic_fail = false;
    pmic_fail_after_write = out_restore_stuck = trace_out = false;
    pmic_read_failures = 0;
    fail_counter_for_khz = 0;
    writes = 0;
    event_count = 0;
    out_trace_count = 0;
}

static void test_hiopt_characterization(void)
{
    assert(tg_gpu_clock_emc_floor_uv(1583u,2233000u) == 600000u);
    assert(tg_gpu_clock_emc_floor_uv(1584u,2233000u) == 590000u);
    assert(tg_gpu_clock_emc_floor_uv(1694u,2466000u) == 630000u);
    assert(tg_gpu_clock_emc_floor_uv(1695u,2466000u) == 590000u);
    assert(tg_gpu_clock_emc_floor_uv(1583u,3200000u) == 760000u);
    assert(tg_gpu_clock_emc_floor_uv(1659u,3200000u) == 740000u);
    assert(tg_gpu_clock_emc_floor_uv(1700u,3200000u) == 720000u);
    assert(tg_gpu_clock_emc_floor_uv(0u,1600000u) == UINT32_MAX);
    assert(tg_gpu_clock_emc_floor_uv(1659u,3200001u) == UINT32_MAX);
    /* Reference Switch Lite fused GPU Speedo = 1659, captured Noble 4.9.140-l4t
     * NAPLL En - p4v2-AggressiveHighOPT (gspd_id=3) table.
     */
    static const uint32_t expected_freqs[TG_GPU_HIOPT_OPP_COUNT] = {
        76800u, 153600u, 230400u, 307200u, 384000u, 460800u,
        537600u, 614400u, 691200u, 768000u, 844800u, 921600u,
        998400u, 1075200u, 1152000u, 1228800u
    };
    /* >=921.6 MHz: independent fixed-upstream HighOPT CVB calculation,
     * not part of the captured DT-limited 844.8 MHz hardware table. */
    static const uint32_t expected_high_uv[6][TG_GPU_THERMAL_RANGES] = {
        {800000,615000,610000,605000,600000,595000},
        {800000,635000,630000,630000,625000,620000},
        {800000,665000,665000,665000,670000,670000},
        {800000,695000,695000,700000,705000,705000},
        {800000,720000,725000,730000,735000,735000},
        {800000,755000,770000,785000,800000,800000},
    };
    uint32_t i, r;
    for (i = 0; i < TG_GPU_HIOPT_OPP_COUNT; ++i) {
        uint32_t khz = tg_gpu_clock_hiopt_freq_khz(i);
        assert(khz == expected_freqs[i]);
        assert(tg_gpu_clock_hiopt_index(khz) == i);
        for (r = 0; r < TG_GPU_THERMAL_RANGES; ++r) {
            uint32_t uv_1600 = tg_gpu_clock_hiopt_uv(1659u, khz, r, 1600000u);
            uint32_t uv_2500 = tg_gpu_clock_hiopt_uv(1659u, khz, r, 2500000u);
            if (r == 0) {
                assert(uv_1600 == 800000u && uv_2500 == 800000u);
            } else if (i < 10u) {
                assert(uv_1600 == 590000u);
                assert(uv_2500 == 630000u);
            } else {
                assert(uv_1600 == expected_high_uv[i-10u][r]);
                assert(uv_2500 == (uv_1600 > 630000u ? uv_1600 : 630000u));
            }
        }
    }
    assert(tg_gpu_clock_hiopt_freq_khz(TG_GPU_HIOPT_OPP_COUNT) == 0);
    assert(tg_gpu_clock_hiopt_index(123456u) == UINT32_MAX);
    assert(tg_gpu_clock_hiopt_uv(0, 460800u, 1, 1600000u) == UINT32_MAX);
    assert(tg_gpu_clock_hiopt_uv(1659u, 460800u, 6, 1600000u) == UINT32_MAX);

    /* Thermal range boundaries + 1 C downward cooldown hysteresis. */
    r = tg_gpu_clock_thermal_range(20, TG_GPU_THERMAL_RANGES);
    assert(r == 0);
    r = tg_gpu_clock_thermal_range(21, r);
    assert(r == 1);
    r = tg_gpu_clock_thermal_range(20, r);
    assert(r == 1); /* held by 1 C hysteresis */
    r = tg_gpu_clock_thermal_range(19, r);
    assert(r == 0);
    assert(tg_gpu_clock_thermal_range(91, 4) == 5);
    assert(tg_gpu_clock_thermal_range(90, 5) == 5);
    assert(tg_gpu_clock_thermal_range(89, 5) == 4);
}

static void test_full_oc_ladder(void)
{
    struct tg_gpu_power p = {.owned=true,.ready=true,.pmic_address=0x33};
    struct tg_gpu_power_io io = {NULL,read_reg,write_reg,read_pmic,write_pmic,delay, NULL};
    struct tg_gpu_clock c = {0};
    reset(); gpu_speedo_fuse=1659u; target_khz=460800u;
    assert(tg_gpu_clock_start(&io,&p,460800u,&c)==TG_GPU_CLOCK_OK);
    live_mode=true;
    for (uint32_t i=0; i<TG_GPU_HIOPT_OPP_COUNT; ++i) {
        uint32_t khz=tg_gpu_clock_hiopt_freq_khz(i);
        uint32_t uv=tg_gpu_clock_hiopt_uv(1659u,khz,2u,1600000u);
        event_count=0;
        assert(tg_gpu_clock_set_rate(&io,&p,khz,uv,&c)==TG_GPU_CLOCK_OK);
        assert(c.measured_khz==khz && c.rail_uv==uv);
    }
    event_count=0;
    assert(tg_gpu_clock_set_rate(&io,&p,76800u,590000u,&c)==TG_GPU_CLOCK_OK);
    assert(events[0].kind==EV_PLL_BYPASS && events[event_count-1].kind==EV_PMIC_WRITE);
    assert(tg_gpu_clock_stop(&io,&c)==TG_GPU_CLOCK_OK);
}

static void test_live_dvfs_ordering_and_rollback(void)
{
    struct tg_gpu_power p = {.owned = true, .ready = true, .pmic_address = 0x33};
    struct tg_gpu_power_io io = {NULL, read_reg, write_reg, read_pmic, write_pmic, delay, NULL};
    struct tg_gpu_clock c = {0};

    reset();
    gpu_speedo_fuse = 1659u;
    target_khz = TG_GPU_LOW_POWER_KHZ;
    assert(tg_gpu_clock_start(&io, &p, target_khz, &c) == TG_GPU_CLOCK_OK);
    assert(c.gpu_speedo == 1659u && c.target_khz == 460800u && c.rail_uv == 800000u);

    /* 1. Downscale 460.8 MHz / 800 mV -> 153.6 MHz / 590 mV:
     *    PLL bypass -> relock 153.6 MHz -> counter check -> PMIC V-down last.
     */
    live_mode = true;
    event_count = 0;
    assert(tg_gpu_clock_set_rate(&io, &p, 153600u, 590000u, &c) == TG_GPU_CLOCK_OK);
    assert(c.target_khz == 153600u && c.measured_khz == 153600u && c.rail_uv == 590000u);
    assert(rail == (590000u - 250000u) / 5000u);
    assert(event_count == 4);
    assert(events[0].kind == EV_PLL_BYPASS);
    assert(events[1].kind == EV_PLL_SELECT && events[1].value == 153600u);
    assert(events[2].kind == EV_COUNTER && events[2].value == 153600u);
    assert(events[3].kind == EV_PMIC_WRITE && events[3].value == 590000u);

    /* 2. Upscale 153.6 MHz / 590 mV -> 844.8 MHz / 615 mV:
     *    PMIC V-up first -> PLL bypass -> relock 844.8 MHz -> counter check.
     */
    event_count = 0;
    assert(tg_gpu_clock_set_rate(&io, &p, 844800u, 615000u, &c) == TG_GPU_CLOCK_OK);
    assert(c.target_khz == 844800u && c.measured_khz == 844800u && c.rail_uv == 615000u);
    assert(event_count == 4);
    assert(events[0].kind == EV_PMIC_WRITE && events[0].value == 615000u);
    assert(events[1].kind == EV_PLL_BYPASS);
    assert(events[2].kind == EV_PLL_SELECT && events[2].value == 844800u);
    assert(events[3].kind == EV_COUNTER && events[3].value == 844800u);

    /* 3. PMIC failure before upscale: PLL untouched, clock remains at 844.8 MHz. */
    assert(tg_gpu_clock_set_rate(&io, &p, 460800u, 590000u, &c) == TG_GPU_CLOCK_OK);
    event_count = 0;
    pmic_fail = true;
    assert(tg_gpu_clock_set_rate(&io, &p, 844800u, 615000u, &c) == TG_GPU_CLOCK_RAIL_ERROR);
    assert(event_count == 0 && c.running && c.target_khz == 460800u && c.rail_uv == 590000u);
    pmic_fail = false;

    /* 4. Upscale counter failure: V-up happens first, target 844.8 MHz fails
     *    counter, PLL rolls back to 460.8 MHz and keeps rail at 615 mV (never lowered).
     */
    event_count = 0;
    fail_counter_for_khz = 844800u;
    assert(tg_gpu_clock_set_rate(&io, &p, 844800u, 615000u, &c) == TG_GPU_CLOCK_COUNTER_ERROR);
    assert(c.running && c.target_khz == 460800u && c.measured_khz == 460800u);
    assert(c.rail_uv == 615000u && rail == (615000u - 250000u) / 5000u);
    fail_counter_for_khz = 0;

    /* 5. Downscale counter failure: rail is NEVER lowered when F-down fails,
     *    and PLL rolls back to 460.8 MHz.
     */
    event_count = 0;
    fail_counter_for_khz = 153600u;
    assert(tg_gpu_clock_set_rate(&io, &p, 153600u, 590000u, &c) == TG_GPU_CLOCK_COUNTER_ERROR);
    assert(c.running && c.target_khz == 460800u && c.measured_khz == 460800u);
    assert(c.rail_uv == 615000u && rail == (615000u - 250000u) / 5000u);
    for (uint32_t i = 0; i < event_count; ++i)
        assert(events[i].kind != EV_PMIC_WRITE);
    fail_counter_for_khz = 0;

    /* 6. Unrecoverable double counter failure marks clock->running = false. */
    counter_bad = true;
    assert(tg_gpu_clock_set_rate(&io, &p, 153600u, 590000u, &c) == TG_GPU_CLOCK_COUNTER_ERROR);
    assert(!c.running);
    counter_bad = false;

    live_mode = false;
    assert(tg_gpu_clock_stop(&io, &c) == TG_GPU_CLOCK_OK);
    assert(registers[0x20160 / 4] == 0x00220000);
}

static void test_partial_pmic_failure(void)
{
    struct tg_gpu_power p = {.owned = true, .ready = true, .pmic_address = 0x33};
    struct tg_gpu_power_io io = {NULL, read_reg, write_reg, read_pmic, write_pmic, delay, NULL};
    struct tg_gpu_clock c = {0};
    reset();
    gpu_speedo_fuse = 1659u;
    target_khz = TG_GPU_LOW_POWER_KHZ;
    assert(tg_gpu_clock_start(&io, &p, target_khz, &c) == TG_GPU_CLOCK_OK);
    live_mode = true;

    /* The lowering write succeeds, but its first readback fails. Reconcile
     * the real 590 mV rail; the next boost must raise voltage before PLL. */
    pmic_read_failures = 1;
    assert(tg_gpu_clock_set_rate(&io, &p, 153600u, 590000u, &c) == TG_GPU_CLOCK_RAIL_ERROR);
    assert(c.running && c.target_khz == 153600u && c.rail_uv == 590000u);
    event_count = 0;
    assert(tg_gpu_clock_set_rate(&io, &p, 844800u, 615000u, &c) == TG_GPU_CLOCK_OK);
    assert(events[0].kind == EV_PMIC_WRITE && events[0].value == 615000u);

    /* The bridge itself can fail AFTER applying the write. */
    pmic_fail_after_write = true;
    assert(tg_gpu_clock_set_rate(&io, &p, 153600u, 590000u, &c) == TG_GPU_CLOCK_RAIL_ERROR);
    assert(c.running && c.rail_uv == 590000u);
    pmic_fail_after_write = false;
    assert(tg_gpu_clock_set_rate(&io, &p, 844800u, 615000u, &c) == TG_GPU_CLOCK_OK);

    /* If readback remains unavailable, do not continue submitting or boost
     * using an assumed voltage. Recovery must reinitialize the clock. */
    pmic_read_failures = 2;
    assert(tg_gpu_clock_set_rate(&io, &p, 153600u, 590000u, &c) == TG_GPU_CLOCK_RAIL_ERROR);
    assert(!c.running && c.rail_uv == 0);
    event_count = 0;
    assert(tg_gpu_clock_set_rate(&io, &p, 844800u, 615000u, &c) == TG_GPU_CLOCK_BAD_STATE);
    assert(event_count == 0);
    live_mode = false;
    assert(tg_gpu_clock_stop(&io, &c) == TG_GPU_CLOCK_OK);
}

static void assert_vcodiv_split_sequence(uint32_t start_idx, uint32_t base_out)
{
    uint32_t by2 = (base_out & ~0x3f00u) | (2u << 8);
    uint32_t by1 = base_out & ~0x3f00u;
    uint32_t i = start_idx;
    /* Entry: read OUT -> back-to-back double-write vcodiv(2) -> readback -> 2 us
     * -> bypass (SEL_VCO=0) -> NA delays (1, 1, 40 us) -> VCO (SEL_VCO=1).
     */
    assert(i + 16 < out_trace_count);
    assert(out_trace[i].kind == TR_OUT_READ && out_trace[i].value == by1); ++i;
    assert(out_trace[i].kind == TR_OUT_WRITE && out_trace[i].value == by2); ++i;
    assert(out_trace[i].kind == TR_OUT_WRITE && out_trace[i].value == by2); ++i;
    assert(out_trace[i].kind == TR_OUT_READ && out_trace[i].value == by2); ++i;
    assert(out_trace[i].kind == TR_DELAY_US && out_trace[i].value == 2u); ++i;
    assert(out_trace[i].kind == TR_VCO_WRITE && out_trace[i].value == 0u); ++i;
    assert(out_trace[i].kind == TR_DELAY_US && out_trace[i].value == 1u); ++i;
    assert(out_trace[i].kind == TR_DELAY_US && out_trace[i].value == 1u); ++i;
    assert(out_trace[i].kind == TR_DELAY_US && out_trace[i].value == 40u); ++i;
    assert(out_trace[i].kind == TR_VCO_WRITE && out_trace[i].value == 1u); ++i;
    /* Restore: read OUT (still 1:2) -> 2 us -> double-write vcodiv(0) -> readback
     * -> final verification readback (vcodiv == 0).
     */
    assert(out_trace[i].kind == TR_OUT_READ && out_trace[i].value == by2); ++i;
    assert(out_trace[i].kind == TR_DELAY_US && out_trace[i].value == 2u); ++i;
    assert(out_trace[i].kind == TR_OUT_WRITE && out_trace[i].value == by1); ++i;
    assert(out_trace[i].kind == TR_OUT_WRITE && out_trace[i].value == by1); ++i;
    assert(out_trace[i].kind == TR_OUT_READ && out_trace[i].value == by1); ++i;
    assert(out_trace[i].kind == TR_OUT_READ && out_trace[i].value == by1);
}

static void assert_no_vcodiv_split_writes(uint32_t start_idx, uint32_t end_idx)
{
    for (uint32_t i = start_idx; i < end_idx; ++i) {
        assert(out_trace[i].kind != TR_OUT_WRITE);
        assert(!(out_trace[i].kind == TR_DELAY_US && out_trace[i].value == 2u));
    }
}

static void test_vcodiv_pl1_transitions(void)
{
    struct tg_gpu_power p = {.owned = true, .ready = true, .pmic_address = 0x33};
    struct tg_gpu_power_io io = {NULL, read_reg, write_reg, read_pmic, write_pmic, delay, NULL};
    struct tg_gpu_clock c = {0};
    uint32_t idx;

    /* 1. Cold start at PL=2 (460.8 MHz) with reset COEFF=0: only the initial
     *    OUT setup write (0x80000000), zero vcodiv(2) split writes.
     */
    reset();
    gpu_speedo_fuse = 1659u;
    target_khz = TG_GPU_LOW_POWER_KHZ;
    trace_out = true;
    out_trace_count = 0;
    assert(tg_gpu_clock_start(&io, &p, target_khz, &c) == TG_GPU_CLOCK_OK);
    {
        uint32_t out_writes = 0;
        for (uint32_t i = 0; i < out_trace_count; ++i) {
            if (out_trace[i].kind == TR_OUT_WRITE) {
                assert(out_trace[i].value == 0x80000000u);
                ++out_writes;
            }
            assert(!(out_trace[i].kind == TR_DELAY_US && out_trace[i].value == 2u));
        }
        assert(out_writes == 1u);
    }

    /* 2. Live PL>=2 -> PL>=2 control (460.8 [PL=2] -> 153.6 [PL=5], and
     *    153.6 [PL=5] -> 614.4 [PL=2]): zero OUT writes and no 2 us delays.
     */
    live_mode = true;
    out_trace_count = 0;
    assert(tg_gpu_clock_set_rate(&io, &p, 153600u, 590000u, &c) == TG_GPU_CLOCK_OK);
    assert_no_vcodiv_split_writes(0, out_trace_count);
    out_trace_count = 0;
    assert(tg_gpu_clock_set_rate(&io, &p, 614400u, 590000u, &c) == TG_GPU_CLOCK_OK);
    assert_no_vcodiv_split_writes(0, out_trace_count);

    /* 3. Live PL>=2 -> PL<2 (614.4 [PL=2] -> 691.2 [PL=1]): must double-write
     *    vcodiv(2), read back, delay 2 us, relock under bypass, wait 2 us,
     *    double-write vcodiv(0), read back, and preserve non-VCODIV bits.
     */
    registers[0x137250 / 4] = 0x80000015u; /* non-zero BYPDIV must stay intact */
    out_trace_count = 0;
    assert(tg_gpu_clock_set_rate(&io, &p, 691200u, 590000u, &c) == TG_GPU_CLOCK_OK);
    assert_vcodiv_split_sequence(0, 0x80000015u);
    assert(registers[0x137250 / 4] == 0x80000015u);

    /* 4. Live PL<2 -> PL<2 same-PL under bypass (691.2 [PL=1] -> 1228.8 [PL=1],
     *    then voltage-only NA update at 1228.8 [PL=1]): KMD has no NDIV slide,
     *    so both bypass-relocks must engage and restore vcodiv(2).
     */
    registers[0x137250 / 4] = 0x80000000u;
    out_trace_count = 0;
    assert(tg_gpu_clock_set_rate(&io, &p, 1228800u, 770000u, &c) == TG_GPU_CLOCK_OK);
    /* First entry in trace is PMIC V-up 1000 us settle delay, then split seq. */
    assert(out_trace[0].kind == TR_DELAY_US && out_trace[0].value == 1000u);
    assert_vcodiv_split_sequence(1u, 0x80000000u);
    out_trace_count = 0;
    assert(tg_gpu_clock_set_rate(&io, &p, 1228800u, 800000u, &c) == TG_GPU_CLOCK_OK);
    assert(out_trace[0].kind == TR_DELAY_US && out_trace[0].value == 1000u);
    assert_vcodiv_split_sequence(1u, 0x80000000u);

    /* 5. Live PL<2 -> PL>=2 (1228.8 [PL=1] -> 460.8 [PL=2]): must split entry
     *    from PL=1 to bypass and restore 1:1 after relocking at PL=2.
     */
    out_trace_count = 0;
    assert(tg_gpu_clock_set_rate(&io, &p, 460800u, 590000u, &c) == TG_GPU_CLOCK_OK);
    assert_vcodiv_split_sequence(0u, 0x80000000u);
    assert(registers[0x137250 / 4] == 0x80000000u);

    /* 6. Counter failure rollback across PL=2 -> PL=1 -> PL=2: both forward
     *    attempt and rollback execute vcodiv(2) split + restore, leaving 1:1.
     */
    out_trace_count = 0;
    fail_counter_for_khz = 844800u;
    assert(tg_gpu_clock_set_rate(&io, &p, 844800u, 615000u, &c) == TG_GPU_CLOCK_COUNTER_ERROR);
    fail_counter_for_khz = 0;
    assert(c.running && c.target_khz == 460800u && c.rail_uv == 615000u);
    assert(registers[0x137250 / 4] == 0x80000000u);
    /* PMIC 1000 us -> forward split (16 entries) -> counter (200, 100 us) -> rollback split. */
    assert(out_trace[0].kind == TR_DELAY_US && out_trace[0].value == 1000u);
    assert_vcodiv_split_sequence(1u, 0x80000000u);
    assert(out_trace[17].kind == TR_DELAY_US && out_trace[17].value == 200u);
    assert(out_trace[18].kind == TR_DELAY_US && out_trace[18].value == 100u);
    assert_vcodiv_split_sequence(19u, 0x80000000u);

    /* 7. Counter failure rollback staying in PL>=2 (460.8 -> 153.6 -> 460.8):
     *    neither forward attempt nor rollback touches OUT.
     */
    out_trace_count = 0;
    fail_counter_for_khz = 153600u;
    assert(tg_gpu_clock_set_rate(&io, &p, 153600u, 590000u, &c) == TG_GPU_CLOCK_COUNTER_ERROR);
    fail_counter_for_khz = 0;
    assert(c.running && c.target_khz == 460800u && c.rail_uv == 615000u);
    assert_no_vcodiv_split_writes(0, out_trace_count);

    /* 8. Stuck OUT restore: fails readback, rollback fails readback -> running=false,
     *    rail is never lowered, and stop restores saved OUT once cleared.
     */
    out_restore_stuck = true;
    assert(tg_gpu_clock_set_rate(&io, &p, 844800u, 615000u, &c) == TG_GPU_CLOCK_READBACK_ERROR);
    assert(!c.running && c.rail_uv == 615000u);
    assert((registers[0x137250 / 4] & 0x3f00u) == (2u << 8));
    out_restore_stuck = false;
    live_mode = false;
    trace_out = false;
    assert(tg_gpu_clock_stop(&io, &c) == TG_GPU_CLOCK_OK);
    assert((registers[0x137250 / 4] & 0x3f00u) == 0u);

    /* 9. Cold start at PL=1 (729.6 MHz & 1228.8 MHz): initial OUT=0x80000000,
     *    then full vcodiv(2) split + 2 us + bypass-to-VCO + 2 us + restore 1:1.
     */
    for (uint32_t k = 0; k < 2u; ++k) {
        reset();
        memset(&c, 0, sizeof(c));
        gpu_speedo_fuse = 1659u;
        target_khz = k == 0u ? TG_GPU_FIXED_KHZ : TG_GPU_OVERCLOCK_KHZ;
        trace_out = true;
        out_trace_count = 0;
        assert(tg_gpu_clock_start(&io, &p, target_khz, &c) == TG_GPU_CLOCK_OK);
        idx = 0;
        while (idx < out_trace_count &&
               !(out_trace[idx].kind == TR_OUT_WRITE && out_trace[idx].value == 0x80000200u))
            ++idx;
        assert(idx > 0 && idx < out_trace_count);
        assert_vcodiv_split_sequence(idx - 1u, 0x80000000u);
        assert(registers[0x137250 / 4] == 0x80000000u);
        trace_out = false;
        assert(tg_gpu_clock_stop(&io, &c) == TG_GPU_CLOCK_OK);
    }
}

int main(void)
{
    struct tg_gpu_power p = {.owned = true, .ready = true, .pmic_address = 0x33};
    struct tg_gpu_power_io io = {NULL, read_reg, write_reg, read_pmic, write_pmic, delay, NULL};
    struct tg_gpu_clock c = {0};
    assert(tg_gpu_clock_required_uv(0, TG_GPU_FIXED_KHZ) == UINT32_MAX);
    assert(tg_gpu_clock_required_uv(5001, TG_GPU_FIXED_KHZ) == UINT32_MAX);
    assert(tg_gpu_clock_required_uv(1400, TG_GPU_FIXED_KHZ) == 800000);
    assert(tg_gpu_clock_required_uv(100, TG_GPU_FIXED_KHZ) > 800000);
    assert(tg_gpu_clock_required_uv(1659, TG_GPU_FIXED_KHZ) == 800000);
    assert(tg_gpu_clock_required_uv(1659, TG_GPU_LOW_POWER_KHZ) == 800000);
    assert(tg_gpu_clock_required_uv(0, TG_GPU_LOW_POWER_KHZ) == UINT32_MAX);
    reset();
    assert(tg_gpu_clock_start(&io, &p, target_khz, &c) == TG_GPU_CLOCK_OK);
    assert(c.measured_khz == TG_GPU_FIXED_KHZ && c.counter == 15200);
    assert(c.external_calibration == 20 && c.stage == 5);
    assert(registers[0x20798 / 4] == 0x105);
    assert(registers[0x20160 / 4] == 0); /* persists beyond the counter sample */
    assert(tg_gpu_clock_stop(&io, &c) == TG_GPU_CLOCK_OK);
    assert(!c.owned && !c.running && registers[0x137000 / 4] == 2);
    assert(registers[0x20160 / 4] == 0x00220000);
    assert((registers[0x132820 / 4] & 0x1007f) == 0);
    reset(); memset(&c, 0, sizeof(c));
    /* Fused slope 10mV, intercept 600mV gives the same 800mV calibration. */
    adc_fuse = (1u << 30) | (10u << 24) | (600u << 4);
    assert(tg_gpu_clock_start(&io, &p, target_khz, &c) == TG_GPU_CLOCK_OK);
    assert(tg_gpu_clock_stop(&io, &c) == TG_GPU_CLOCK_OK);
    assert(registers[0x20160 / 4] == 0x00220000);
    reset(); memset(&c, 0, sizeof(c)); slowdown_stuck=true;
    assert(tg_gpu_clock_start(&io, &p, target_khz, &c) == TG_GPU_CLOCK_READBACK_ERROR);
    assert(c.owned && !c.running);
    assert(tg_gpu_clock_stop(&io, &c) == TG_GPU_CLOCK_OK);
    assert(registers[0x20160 / 4] == 0x00220000);
    reset(); memset(&c, 0, sizeof(c)); counter_bad = true;
    assert(tg_gpu_clock_start(&io, &p, target_khz, &c) == TG_GPU_CLOCK_COUNTER_ERROR);
    assert(c.owned);
    assert(tg_gpu_clock_stop(&io, &c) == TG_GPU_CLOCK_OK);
    reset(); memset(&c, 0, sizeof(c)); calibration_timeout = true;
    assert(tg_gpu_clock_start(&io, &p, target_khz, &c) == TG_GPU_CLOCK_CALIBRATION_ERROR);
    assert(tg_gpu_clock_stop(&io, &c) == TG_GPU_CLOCK_OK);
    reset(); memset(&c, 0, sizeof(c)); registers[0x137000 / 4] = 1;
    assert(tg_gpu_clock_start(&io, &p, target_khz, &c) == TG_GPU_CLOCK_BAD_STATE);
    assert(!writes && !c.owned);
    reset(); memset(&c, 0, sizeof(c)); target_khz = TG_GPU_OVERCLOCK_KHZ;
    assert(tg_gpu_clock_start(&io, &p, target_khz, &c) == TG_GPU_CLOCK_OK);
    assert(c.target_khz == 1228800 && c.measured_khz == 1228800);
    assert(c.rail_uv > 800000 && c.rail_uv <= 1050000);
    assert(rail == (c.rail_uv - 250000) / 5000);
    assert(tg_gpu_clock_stop(&io, &c) == TG_GPU_CLOCK_OK);
    assert(tg_gpu_clock_start(&io, &p, 1234567, &c) == TG_GPU_CLOCK_BAD_STATE);
    reset(); memset(&c, 0, sizeof(c)); target_khz = TG_GPU_LOW_POWER_KHZ;
    assert(tg_gpu_clock_start(&io, &p, target_khz, &c) == TG_GPU_CLOCK_OK);
    assert((registers[0x137004 / 4] & 0x3fffff) == 0x23001u);
    assert(c.target_khz == 460800 && c.measured_khz == 460800 && c.counter == 9600);
    assert(c.rail_uv == 800000 && rail == 110);
    assert(tg_gpu_clock_stop(&io, &c) == TG_GPU_CLOCK_OK);
    assert((registers[0x137004 / 4] & 0x3fffff) == 0);
    reset(); memset(&c, 0, sizeof(c)); target_khz = TG_GPU_LOW_POWER_KHZ; counter_bad = true;
    assert(tg_gpu_clock_start(&io, &p, target_khz, &c) == TG_GPU_CLOCK_COUNTER_ERROR);
    assert(c.measured_khz != c.target_khz && c.owned);
    assert(tg_gpu_clock_stop(&io, &c) == TG_GPU_CLOCK_OK);
    test_hiopt_characterization();
    test_full_oc_ladder();
    test_live_dvfs_ordering_and_rollback();
    test_partial_pmic_failure();
    test_vcodiv_pl1_transitions();
    puts("T214 C1 NA clock ordering, HighOPT CVB, live DVFS, VCODIV split and unwind tests passed (host model).");
    return 0;
}
