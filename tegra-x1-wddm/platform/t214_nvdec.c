/* SPDX-License-Identifier: MIT
 *
 * Tegra210/T210B01 NVDEC power, clock, reset and Host1x submission.
 *
 * Register contract:
 *  - PMC powergate id 25, PWRGATE_TOGGLE 0x30, PWRGATE_STATUS 0x38,
 *    REMOVE_CLAMPING 0x34 (Linux drivers/soc/tegra/pmc.c, T210 table).
 *  - CAR CLK_OUT_ENB_Y 0x298 bit 2, CLK_SOURCE_NVDEC 0x698 mux PLLP (value
 *    4 in mux_pllc2_c_c3_pllp_plla1_clkm), RST_DEV_Y_SET/CLR 0x2a8/0x2ac.
 *    Linux v6.8 drivers/clk/tegra/{clk.h,clk-tegra-periph.c}.
 *  - T210 Host1x05: channel registers at 0x4000*channel, sync registers at
 *    0x2100, SYNCPT(id) at 0x2100+0xf80+4*id.  Linux drivers/gpu/host1x.
 * The MBIST workaround is the T210 generic one at CAR 0x554 bits 9/31.
 */
#include "t214_nvdec.h"

#include <string.h>

#define PMC_PWRGATE_TOGGLE    0x30u
#define PMC_REMOVE_CLAMPING   0x34u
#define PMC_PWRGATE_STATUS    0x38u
#define PMC_TOGGLE_START      (1u << 8)
#define PMC_NVDEC             25u

#define CAR_CLK_OUT_ENB_L     0x010u
#define CAR_CLK_OUT_ENB_Y     0x298u
#define CAR_CLK_OUT_ENB_L_SET 0x320u
#define CAR_CLK_OUT_ENB_Y_SET 0x29cu
#define CAR_CLK_OUT_ENB_Y_CLR 0x2a0u
#define CAR_RST_DEV_Y_SET     0x2a8u
#define CAR_RST_DEV_Y_CLR     0x2acu
#define CAR_RST_DEV_Y         0x2a4u
#define CAR_CLK_SOURCE_NVDEC  0x698u
#define CAR_LVL2_CLK_GATE_OVRE 0x554u
#define CAR_HOST1X_BIT        (1u << 28)
#define CAR_NVDEC_BIT         (1u << 2)
#define CAR_NVDEC_MUX_PLLP    (4u << 29)
#define CAR_NVDEC_MUX_MASK    0xe00000ffu

#define HOST1X_CHANNEL_STRIDE 0x4000u
#define HOST1X_SYNC_BASE      0x2100u
#define HOST1X_SYNCPT_BASE    (HOST1X_SYNC_BASE + 0xf80u)
#define HOST1X_CMDPROC_STOP   (HOST1X_SYNC_BASE + 0xacu)
#define HOST1X_CH_TEARDOWN    (HOST1X_SYNC_BASE + 0xb0u)
#define HOST1X_CFEMPTY        (1u << 11)
#define MC_FLUSH             0x970u
#define MC_FLUSH_STATUS      0x974u
#define MC_NVDEC_BIT         (1u << 5)
#define HOST1X_CHANNEL_DMASTART 0x14u
#define HOST1X_CHANNEL_DMAPUT   0x18u
#define HOST1X_CHANNEL_DMAEND   0x20u
#define HOST1X_CHANNEL_DMACTRL  0x24u
#define HOST1X_CHANNEL_CHANNELCTRL 0x98u
#define HOST1X_DMACTRL_STOP     1u
#define HOST1X_DMACTRL_GETRST   2u
#define HOST1X_DMACTRL_INITGET  4u
#define HOST1X_CHANNELCTRL_FILTER (1u << 2)
#define HOST1X_PUSH_RESTART     0x50000000u
#define HOST1X_PUSH_NOP         0x20000000u

#define HOST1X_CLASS_NVDEC 0xf0u
#define HOST1X_OP_SETCLASS(class, offset, mask) \
    (((0u) << 28) | ((offset) << 16) | ((class) << 6) | (mask))
#define HOST1X_OP_GATHER(count) ((6u << 28) | (count))

static uint32_t r32(const struct tg_nvdec_platform_io *io,
                    enum tg_nvdec_aperture ap, uint32_t offset)
{
    return io->read32(io->ctx, ap, offset);
}

static void w32(const struct tg_nvdec_platform_io *io, enum tg_nvdec_aperture ap,
                uint32_t offset, uint32_t value)
{
    io->write32(io->ctx, ap, offset, value);
    (void)r32(io, ap, offset);   /* posting read for CAR/PMC ordering */
}

static bool poll_bit(const struct tg_nvdec_platform_io *io,
                     enum tg_nvdec_aperture ap, uint32_t offset,
                     uint32_t mask, bool set, uint32_t timeout_ms)
{
    uint32_t elapsed = 0;
    const uint32_t step = 10;

    for (;;) {
        const uint32_t value = r32(io, ap, offset);
        if (value == UINT32_MAX)
            return false;
        if (((value & mask) != 0) == set)
            return true;
        if (elapsed >= timeout_ms * 1000u)
            return false;
        io->delay_us(io->ctx, step);
        elapsed += step;
    }
}

static uint32_t channel_base(const struct tg_nvdec_platform *p)
{
    return p->channel * HOST1X_CHANNEL_STRIDE;
}

static void channel_stop(const struct tg_nvdec_platform_io *io,
                         const struct tg_nvdec_platform *p)
{
    if (p->channel >= 12)
        return;
    w32(io, TG_NVDEC_AP_HOST1X, channel_base(p) + HOST1X_CHANNEL_DMACTRL,
        HOST1X_DMACTRL_STOP);
}

void tg_nvdec_platform_zero(struct tg_nvdec_platform *p)
{
    if (p)
        memset(p, 0, sizeof(*p));
}

enum tg_nvdec_platform_result tg_nvdec_platform_init(
    const struct tg_nvdec_platform_io *io, struct tg_nvdec_platform *p,
    uint32_t channel, uint32_t syncpt)
{
    uint32_t status, gate, source;

    if (!io || !p || !io->read32 || !io->write32 || !io->delay_us)
        return TG_NVDEC_PLATFORM_ARGUMENT;
    if (channel >= 12 || syncpt >= 192)
        return TG_NVDEC_PLATFORM_ARGUMENT;
    tg_nvdec_platform_zero(p);
    p->channel = channel;
    p->syncpt = syncpt;

    /* Snapshot the state we are allowed to restore. */
    p->saved_pmc_status = r32(io, TG_NVDEC_AP_PMC, PMC_PWRGATE_STATUS);
    /* An already powered partition belongs to another owner. */
    if (p->saved_pmc_status == UINT32_MAX ||
        (p->saved_pmc_status & (1u << PMC_NVDEC)))
        return TG_NVDEC_PLATFORM_POWER;
    p->saved_mc_flush = r32(io, TG_NVDEC_AP_MC, MC_FLUSH);
    if (p->saved_mc_flush == UINT32_MAX)
        return TG_NVDEC_PLATFORM_ARGUMENT;
    p->saved_gate_y = r32(io, TG_NVDEC_AP_CAR, CAR_CLK_OUT_ENB_Y);
    p->saved_gate_l = r32(io, TG_NVDEC_AP_CAR, CAR_CLK_OUT_ENB_L);
    source = r32(io, TG_NVDEC_AP_CAR, CAR_CLK_SOURCE_NVDEC);
    p->saved_source = source;
    p->source_saved = true;

    /* Legacy gate handling: production integration must use the clock owner's
     * lease. Do not reset the shared Host1x domain here. */
    if (!(p->saved_gate_l & CAR_HOST1X_BIT)) {
        w32(io, TG_NVDEC_AP_CAR, CAR_CLK_OUT_ENB_L_SET, CAR_HOST1X_BIT);
        io->delay_us(io->ctx, 10);
    }
    p->host1x_clock = true;

    /* Assert NVDEC reset before changing its clock or power. */
    w32(io, TG_NVDEC_AP_CAR, CAR_RST_DEV_Y_SET, CAR_NVDEC_BIT);
    io->delay_us(io->ctx, 10);

    status = r32(io, TG_NVDEC_AP_PMC, PMC_PWRGATE_STATUS);
    if (!(status & (1u << PMC_NVDEC))) {
        if (!poll_bit(io, TG_NVDEC_AP_PMC, PMC_PWRGATE_TOGGLE,
                      PMC_TOGGLE_START, false, 100))
            return TG_NVDEC_PLATFORM_POWER;
        p->power_transition = true;
        w32(io, TG_NVDEC_AP_PMC, PMC_PWRGATE_TOGGLE,
            PMC_TOGGLE_START | PMC_NVDEC);
        if (!poll_bit(io, TG_NVDEC_AP_PMC, PMC_PWRGATE_STATUS,
                      1u << PMC_NVDEC, true, 100))
            goto fail_power;
    }
    p->powered = true;
    p->power_transition = false;
    io->delay_us(io->ctx, 10);

    /* PLLP/1 is always available and below the NVDEC limit. */
    w32(io, TG_NVDEC_AP_CAR, CAR_CLK_SOURCE_NVDEC,
        (source & ~CAR_NVDEC_MUX_MASK) | CAR_NVDEC_MUX_PLLP);
    gate = r32(io, TG_NVDEC_AP_CAR, CAR_CLK_OUT_ENB_Y);
    if (!(gate & CAR_NVDEC_BIT))
        w32(io, TG_NVDEC_AP_CAR, CAR_CLK_OUT_ENB_Y_SET, CAR_NVDEC_BIT);
    p->nvdec_clock = true;
    io->delay_us(io->ctx, 10);

    w32(io, TG_NVDEC_AP_PMC, PMC_REMOVE_CLAMPING, 1u << PMC_NVDEC);
    io->delay_us(io->ctx, 10);
    w32(io, TG_NVDEC_AP_MC, MC_FLUSH,
        r32(io, TG_NVDEC_AP_MC, MC_FLUSH) & ~MC_NVDEC_BIT);
    w32(io, TG_NVDEC_AP_CAR, CAR_RST_DEV_Y_CLR, CAR_NVDEC_BIT);
    io->delay_us(io->ctx, 10);

    /* T210 MBIST workaround for the NVDEC partition. */
    {
        const uint32_t v = r32(io, TG_NVDEC_AP_CAR, CAR_LVL2_CLK_GATE_OVRE);
        w32(io, TG_NVDEC_AP_CAR, CAR_LVL2_CLK_GATE_OVRE,
            v | (1u << 9) | (1u << 31));
        io->delay_us(io->ctx, 1);
        w32(io, TG_NVDEC_AP_CAR, CAR_LVL2_CLK_GATE_OVRE, v);
        io->delay_us(io->ctx, 1);
    }

    /* Leave the channel stopped until the first gather. */
    channel_stop(io, p);
    p->channel_started = true;
    return TG_NVDEC_PLATFORM_OK;

fail_power:
    /* Keep ownership of an unacknowledged power command. Retrying the toggle
     * here can turn an already-off partition back on during cleanup. */
    return TG_NVDEC_PLATFORM_POWER;
}

enum tg_nvdec_platform_result tg_nvdec_platform_shutdown(
    const struct tg_nvdec_platform_io *io, struct tg_nvdec_platform *p)
{
    if (!io || !p)
        return TG_NVDEC_PLATFORM_ARGUMENT;
    if (p->power_transition) {
        uint32_t state;
        if (!poll_bit(io, TG_NVDEC_AP_PMC, PMC_PWRGATE_TOGGLE,
                      PMC_TOGGLE_START, false, 100))
            return TG_NVDEC_PLATFORM_POWER;
        state = r32(io, TG_NVDEC_AP_PMC, PMC_PWRGATE_STATUS);
        if (state == UINT32_MAX)
            return TG_NVDEC_PLATFORM_POWER;
        p->powered = (state & (1u << PMC_NVDEC)) != 0;
        p->power_transition = false;
    }
    if (!p->powered)
        return TG_NVDEC_PLATFORM_OK;

    if (tg_nvdec_platform_quiesce(io, p) != TG_NVDEC_PLATFORM_OK)
        return TG_NVDEC_PLATFORM_TIMEOUT;
    w32(io, TG_NVDEC_AP_CAR,
        (p->saved_gate_y & CAR_NVDEC_BIT) ? CAR_CLK_OUT_ENB_Y_SET : CAR_CLK_OUT_ENB_Y_CLR,
        CAR_NVDEC_BIT);
    p->nvdec_clock = false;
    io->delay_us(io->ctx, 10);
    w32(io, TG_NVDEC_AP_PMC, PMC_PWRGATE_TOGGLE,
        PMC_TOGGLE_START | PMC_NVDEC);
    if (!poll_bit(io, TG_NVDEC_AP_PMC, PMC_PWRGATE_STATUS,
                  1u << PMC_NVDEC, false, 100))
        return TG_NVDEC_PLATFORM_POWER;
    if (p->source_saved)
        w32(io, TG_NVDEC_AP_CAR, CAR_CLK_SOURCE_NVDEC, p->saved_source);
    w32(io, TG_NVDEC_AP_MC, MC_FLUSH,
        (r32(io, TG_NVDEC_AP_MC, MC_FLUSH) & ~MC_NVDEC_BIT) |
        (p->saved_mc_flush & MC_NVDEC_BIT));
    p->powered = false;
    p->channel_started = false;
    return TG_NVDEC_PLATFORM_OK;
}

enum tg_nvdec_platform_result tg_nvdec_host1x_stop(
    const struct tg_nvdec_platform_io *io, struct tg_nvdec_platform *p)
{
    return tg_nvdec_platform_quiesce(io, p);
}

enum tg_nvdec_platform_result tg_nvdec_platform_quiesce(
    const struct tg_nvdec_platform_io *io, struct tg_nvdec_platform *p)
{
    uint32_t stop;
    if (!io || !p || !io->read32 || !io->write32 || !io->delay_us || p->channel >= 12)
        return TG_NVDEC_PLATFORM_ARGUMENT;
    if (!p->powered || p->quiesced)
        return TG_NVDEC_PLATFORM_OK;
    if (p->channel_started) {
        stop = r32(io, TG_NVDEC_AP_HOST1X, HOST1X_CMDPROC_STOP);
        if (stop == UINT32_MAX)
            return TG_NVDEC_PLATFORM_SUBMIT;
        w32(io, TG_NVDEC_AP_HOST1X, HOST1X_CMDPROC_STOP, stop | (1u << p->channel));
        channel_stop(io, p);
        w32(io, TG_NVDEC_AP_HOST1X, HOST1X_CH_TEARDOWN, 1u << p->channel);
    }
    w32(io, TG_NVDEC_AP_CAR, CAR_RST_DEV_Y_SET, CAR_NVDEC_BIT);
    if (!poll_bit(io, TG_NVDEC_AP_CAR, CAR_RST_DEV_Y, CAR_NVDEC_BIT, true, 100))
        return TG_NVDEC_PLATFORM_RESET;
    w32(io, TG_NVDEC_AP_MC, MC_FLUSH,
        r32(io, TG_NVDEC_AP_MC, MC_FLUSH) | MC_NVDEC_BIT);
    if (!poll_bit(io, TG_NVDEC_AP_MC, MC_FLUSH_STATUS, MC_NVDEC_BIT, true, 100) ||
        (p->channel_started &&
         !poll_bit(io, TG_NVDEC_AP_HOST1X, channel_base(p), HOST1X_CFEMPTY, true, 100)))
        return TG_NVDEC_PLATFORM_TIMEOUT;
    /* No syncpoint is advanced here. Reset/drain establishes memory safety,
     * while the failed decode remains failed. */
    p->quiesced = true;
    return TG_NVDEC_PLATFORM_OK;
}

enum tg_nvdec_platform_result tg_nvdec_host1x_submit(
    const struct tg_nvdec_platform_io *io, struct tg_nvdec_platform *p,
    const uint32_t *gather, uint32_t words, uint64_t gather_bus,
    void *push, uint64_t push_bus, uint32_t push_size,
    uint32_t timeout_ms, uint32_t *expected, uint32_t *observed)
{
    uint32_t *pb = push;
    uint32_t base, value;
    uint32_t elapsed = 0;
    const uint32_t step = 10;

    if (!io || !p || !gather || !words || words > 0xffffu || !push ||
        push_size < 16u || (push_size & 3u) || (gather_bus & 3u) ||
        (push_bus & 15u) || (gather_bus >> 32) || (push_bus >> 32) ||
        (uint64_t)words * 4u > UINT64_C(0x100000000) - gather_bus ||
        (uint64_t)push_size + 4u > UINT64_C(0x100000000) - push_bus ||
        !expected || !observed)
        return TG_NVDEC_PLATFORM_ARGUMENT;
    if (!p->powered || !p->nvdec_clock || p->quiesced ||
        p->channel >= 12 || p->syncpt >= 192 || !timeout_ms || timeout_ms > 5000u)
        return TG_NVDEC_PLATFORM_ARGUMENT;

    base = channel_base(p);
    channel_stop(io, p);

    /* This encoder currently uses only low32-bit gather/push addresses. */
    pb[0] = HOST1X_OP_SETCLASS(HOST1X_CLASS_NVDEC, 0, 0);
    pb[1] = HOST1X_OP_GATHER(words);
    pb[2] = (uint32_t)gather_bus;
    /* A RESTART here would jump back into the published gather repeatedly.
     * The ring wrap instruction belongs only beyond its usable capacity. */
    pb[3] = HOST1X_PUSH_NOP;
    ((uint32_t *)((uint8_t *)push + push_size))[0] = HOST1X_PUSH_RESTART;

    value = r32(io, TG_NVDEC_AP_HOST1X, base + HOST1X_CHANNEL_CHANNELCTRL);
    w32(io, TG_NVDEC_AP_HOST1X, base + HOST1X_CHANNEL_CHANNELCTRL,
        value | HOST1X_CHANNELCTRL_FILTER);

    w32(io, TG_NVDEC_AP_HOST1X, base + HOST1X_CHANNEL_DMASTART,
        (uint32_t)push_bus);
    w32(io, TG_NVDEC_AP_HOST1X, base + HOST1X_CHANNEL_DMAPUT, 0);
    w32(io, TG_NVDEC_AP_HOST1X, base + HOST1X_CHANNEL_DMAEND,
        push_size + 4u);
    w32(io, TG_NVDEC_AP_HOST1X, base + HOST1X_CHANNEL_DMACTRL,
        HOST1X_DMACTRL_STOP | HOST1X_DMACTRL_GETRST | HOST1X_DMACTRL_INITGET);
    value = r32(io, TG_NVDEC_AP_HOST1X, HOST1X_CMDPROC_STOP);
    if (value == UINT32_MAX)
        return TG_NVDEC_PLATFORM_SUBMIT;
    w32(io, TG_NVDEC_AP_HOST1X, HOST1X_CMDPROC_STOP, value & ~(1u << p->channel));
    w32(io, TG_NVDEC_AP_HOST1X, base + HOST1X_CHANNEL_DMACTRL, 0);
    *expected = r32(io, TG_NVDEC_AP_HOST1X,
                    HOST1X_SYNCPT_BASE + 4u * p->syncpt) + 1u;
    w32(io, TG_NVDEC_AP_HOST1X, base + HOST1X_CHANNEL_DMAPUT, 16u);

    for (;;) {
        value = r32(io, TG_NVDEC_AP_HOST1X,
                    HOST1X_SYNCPT_BASE + 4u * p->syncpt);
        if ((int32_t)(value - *expected) >= 0)
            break;
        if (elapsed >= timeout_ms * 1000u) {
            channel_stop(io, p);
            *observed = value;
            return TG_NVDEC_PLATFORM_TIMEOUT;
        }
        io->delay_us(io->ctx, step);
        elapsed += step;
    }
    channel_stop(io, p);
    *observed = value;
    return TG_NVDEC_PLATFORM_OK;
}
