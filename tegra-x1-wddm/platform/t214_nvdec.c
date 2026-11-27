/* SPDX-License-Identifier: MIT
 *
 * T210B01 inherited NVDEC power, clock, reset and Host1x submission.
 *
 * Register contract:
 *  - PMC NVDEC/NVJPG ids 25/26, CLAMP_STATUS 0x2c, PWRGATE_TOGGLE 0x30,
 *    PWRGATE_STATUS 0x38. This path only reads these inherited states.
 *  - CAR CLK_OUT_ENB_Y 0x298 bit 2, CLK_SOURCE_NVDEC 0x698 mux PLLP (value
 *    4 in mux_pllc2_c_c3_pllp_plla1_clkm), RST_DEV_Y_SET/CLR 0x2a8/0x2ac.
 *    Linux v6.8 drivers/clk/tegra/{clk.h,clk-tegra-periph.c}.
 *  - T210 Host1x05: channel registers at 0x4000*channel, sync registers at
 *    0x2100, SYNCPT(id) at 0x2100+0xf80+4*id.  Linux drivers/gpu/host1x.
 * Noble powergate-t21x.c skips the SLCG/MBIST workaround on T210B01 and
 * retains a reference for already-powered partitions, including NVJPG.
 */
#include "t214_nvdec.h"

#include <string.h>

#define PMC_PWRGATE_TOGGLE    0x30u
#define PMC_CLAMP_STATUS      0x2cu
#define PMC_PWRGATE_STATUS    0x38u
#define PMC_TOGGLE_START      (1u << 8)
#define PMC_NVDEC             25u
#define PMC_NVJPG             26u
#define PMC_VIDEO_BITS        ((1u << PMC_NVDEC) | (1u << PMC_NVJPG))

#define CAR_CLK_OUT_ENB_Y     0x298u
#define CAR_CLK_OUT_ENB_Y_SET 0x29cu
#define CAR_CLK_OUT_ENB_Y_CLR 0x2a0u
#define CAR_RST_DEV_Y_SET     0x2a8u
#define CAR_RST_DEV_Y_CLR     0x2acu
#define CAR_RST_DEV_Y         0x2a4u
#define CAR_CLK_SOURCE_NVDEC  0x698u
#define CAR_NVDEC_BIT         (1u << 2)
#define CAR_NVJPG_BIT         (1u << 3)
#define CAR_NVDEC_MUX_PLLP    (4u << 29)
#define CAR_NVDEC_SOURCE      (CAR_NVDEC_MUX_PLLP | 2u) /* U7.1:408MHz ->204MHz */
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
#define HOST1X_CHANNEL_DMAGET   0x1cu
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

static bool valid_io(const struct tg_nvdec_platform_io *io)
{
    return io && io->read32 && io->write32 && io->delay_us;
}

bool tg_nvdec_platform_direct_dma(const struct tg_nvdec_platform_io *io)
{
    uint32_t hc, dec;
    if (!io || !io->read32) return false;
    hc = r32(io, TG_NVDEC_AP_MC, 0x250u);
    dec = r32(io, TG_NVDEC_AP_MC, 0xab4u);
    if (hc == UINT32_MAX || dec == UINT32_MAX) return false;
    /* A clear SWGROUP ASID_ENABLE leaves the client in bypass regardless
     * of the per-client translation-enable words. T210 ATF initializes
     * those words to all ones; they are not an MMIO failure sentinel.
     * Noble's __smmu_client_set_hwgrp_default() disables a group through
     * its ASID register alone. Never change an existing address space.
     * NVJPG performs no DMA in this path; its clock/reset stay inactive. */
    return !((hc | dec) & (1u << 31));
}

bool tg_nvdec_platform_clocks_supported(uint32_t pllp, uint32_t source,
                                      uint32_t core_boot_floor_uv)
{
    const uint32_t enabled_locked = (1u << 30) | (1u << 27);
    if (core_boot_floor_uv < 637500u || core_boot_floor_uv > 1050000u ||
        core_boot_floor_uv % 12500u || pllp == UINT32_MAX || source == UINT32_MAX ||
        (pllp & (enabled_locked | (1u << 31))) != enabled_locked ||
        (source & ~0xe00000ffu)) return false;
    /* PLLP without OVERRIDE uses its fixed hardware rate. With OVERRIDE,
     * OUT0 is the VCO; DIVP applies to OUT2, not this parent. */
    if (pllp & (1u << 28)) {
        const uint32_t m = pllp & 31u, n = (pllp >> 8) & 1023u;
        if (!m || 38400u * n != 408000u * m) return false;
    }
    switch (source >> 29) {
    /* T210 selects tegra_clk_host1x_9, whose sparse mux table maps PLLP to4
     * and CLK_M to5. Value3 is PLLC4_OUT2, outside this owner's contract. */
    case 4:
        /* Normal B01 process0 admits204MHz at650mV; SLT process0 at625mV.
         * Use their intersection, rather than the temporary EMC voltage.
         * DIV=3 observed on Windows is163.2MHz; keep that inherited rate. */
        return (source & 255u) >= (core_boot_floor_uv >= 650000u ? 2u : 6u);
    case 5: return true; /* CLK_M <=38.4MHz, even before its local divider */
    default: return false; /* no ownership claim over another PLL */
    }
}

/* The caller serializes this shared MC word against graphics reset/power
 * transitions. Only NVDEC's client bit may change. */
static bool set_mc_flush(const struct tg_nvdec_platform_io *io, bool set)
{
    uint32_t value = r32(io, TG_NVDEC_AP_MC, MC_FLUSH);
    if (value == UINT32_MAX)
        return false;
    value = (value & ~MC_NVDEC_BIT) | (set ? MC_NVDEC_BIT : 0);
    w32(io, TG_NVDEC_AP_MC, MC_FLUSH, value);
    value = r32(io, TG_NVDEC_AP_MC, MC_FLUSH);
    return value != UINT32_MAX && !!(value & MC_NVDEC_BIT) == set;
}

static bool wait_mc_drained(const struct tg_nvdec_platform_io *io)
{
    uint32_t consecutive = 0;
    /* Require stable acknowledgement, following the existing GPU MC drain
     * policy. A single transient assertion cannot retire DMA backing. */
    for (uint32_t us = 0; us <= 100000u; us += 10u) {
        const uint32_t value = r32(io, TG_NVDEC_AP_MC, MC_FLUSH_STATUS);
        if (value == UINT32_MAX)
            return false;
        consecutive = value & MC_NVDEC_BIT ? consecutive + 1u : 0u;
        if (consecutive == 5u)
            return true;
        io->delay_us(io->ctx, 10);
    }
    return false;
}

enum tg_nvdec_platform_result tg_nvdec_platform_init(
    const struct tg_nvdec_platform_io *io, struct tg_nvdec_platform *p,
    uint32_t channel, uint32_t syncpt,
    const struct tg_nvdec_platform_owner *owner)
{
    uint32_t power, clamp, toggle, gate, reset, source, flush, value, ctrl;
    const uint32_t video_clocks = CAR_NVDEC_BIT | CAR_NVJPG_BIT;

    if (!valid_io(io) || !p || !owner || !owner->acquire_host1x ||
        !owner->release_host1x || channel >= 12 || syncpt >= 192)
        return TG_NVDEC_PLATFORM_ARGUMENT;
    if (p->captured || p->host1x_lease || p->nvdec_touched)
        return TG_NVDEC_PLATFORM_OWNERSHIP;
    if ((owner->hidrev & 0xfff0u) != 0x2120u)
        return TG_NVDEC_PLATFORM_UNSUPPORTED;
    if ((owner->pllp_base & ((1u << 31) | (1u << 30) | (1u << 27))) !=
        ((1u << 30) | (1u << 27)))
        return TG_NVDEC_PLATFORM_CLOCK;

    /* Already powered is not evidence of a foreign owner. Admit only a
     * powered, unclamped, reset and clock-gated firmware handoff, with both
     * NVDEC and its NVJPG dependency retained. Other states require their
     * actual owner; do not toggle PMC or force resets to manufacture them. */
    power = r32(io, TG_NVDEC_AP_PMC, PMC_PWRGATE_STATUS);
    clamp = r32(io, TG_NVDEC_AP_PMC, PMC_CLAMP_STATUS);
    toggle = r32(io, TG_NVDEC_AP_PMC, PMC_PWRGATE_TOGGLE);
    if (power == UINT32_MAX || clamp == UINT32_MAX || toggle == UINT32_MAX ||
        (power & PMC_VIDEO_BITS) != PMC_VIDEO_BITS ||
        (clamp & PMC_VIDEO_BITS) || (toggle & PMC_TOGGLE_START))
        return TG_NVDEC_PLATFORM_POWER;
    gate = r32(io, TG_NVDEC_AP_CAR, CAR_CLK_OUT_ENB_Y);
    reset = r32(io, TG_NVDEC_AP_CAR, CAR_RST_DEV_Y);
    source = r32(io, TG_NVDEC_AP_CAR, CAR_CLK_SOURCE_NVDEC);
    flush = r32(io, TG_NVDEC_AP_MC, MC_FLUSH);
    if (gate == UINT32_MAX || reset == UINT32_MAX || source == UINT32_MAX ||
        flush == UINT32_MAX)
        return TG_NVDEC_PLATFORM_ARGUMENT;
    if ((gate & video_clocks) || (reset & video_clocks) != video_clocks)
        return TG_NVDEC_PLATFORM_OWNERSHIP;

    p->owner = *owner;
    p->channel = channel;
    p->syncpt = syncpt;
    p->saved_pmc_status = power;
    p->saved_clamp_status = clamp;
    p->saved_gate_y = gate;
    p->saved_reset_y = reset;
    p->saved_source = source;
    p->saved_mc_flush = flush;
    p->captured = true;
    /* A failed provider request can have taken a reference. Its idempotent
     * release must finish before we discard the callback/context. */
    p->host1x_lease = true;
    if (!owner->acquire_host1x(owner->ctx))
        return TG_NVDEC_PLATFORM_CLOCK;
    p->host1x_clock = true;

    value = r32(io, TG_NVDEC_AP_HOST1X, channel_base(p));
    ctrl = r32(io, TG_NVDEC_AP_HOST1X, channel_base(p) + HOST1X_CHANNEL_DMACTRL);
    /* The caller has exclusively reserved a fresh software channel; the
     * video engines above are still reset/gated. Require stopped DMA and an
     * empty FIFO, not equality of inherited pointers to an unknown old ring.
     * First submit programs and verifies our ring before starting DMA. This
     * admission never retires a previous submission or signals completion. */
    if (value == UINT32_MAX || ctrl == UINT32_MAX ||
        !(value & HOST1X_CFEMPTY) || (ctrl & 7u) != HOST1X_DMACTRL_STOP)
        return TG_NVDEC_PLATFORM_OWNERSHIP;
    channel_stop(io, p);
    p->channel_started = true;

    /* Reset was asserted by the handoff and remains asserted while changing
     * the local source/gate. Host1x reset and CAR L are never programmed here. */
    p->nvdec_touched = true;
    w32(io, TG_NVDEC_AP_CAR, CAR_CLK_SOURCE_NVDEC,
        (source & ~CAR_NVDEC_MUX_MASK) | CAR_NVDEC_SOURCE);
    value = r32(io, TG_NVDEC_AP_CAR, CAR_CLK_SOURCE_NVDEC);
    if (value == UINT32_MAX || (value & CAR_NVDEC_MUX_MASK) != CAR_NVDEC_SOURCE)
        return TG_NVDEC_PLATFORM_CLOCK;
    w32(io, TG_NVDEC_AP_CAR, CAR_CLK_OUT_ENB_Y_SET, CAR_NVDEC_BIT);
    p->nvdec_clock = true;
    if (!poll_bit(io, TG_NVDEC_AP_CAR, CAR_CLK_OUT_ENB_Y, CAR_NVDEC_BIT, true, 100))
        return TG_NVDEC_PLATFORM_CLOCK;
    io->delay_us(io->ctx, 10);
    if (!set_mc_flush(io, false))
        return TG_NVDEC_PLATFORM_RESET;
    w32(io, TG_NVDEC_AP_CAR, CAR_RST_DEV_Y_CLR, CAR_NVDEC_BIT);
    if (!poll_bit(io, TG_NVDEC_AP_CAR, CAR_RST_DEV_Y, CAR_NVDEC_BIT, false, 100))
        return TG_NVDEC_PLATFORM_RESET;
    io->delay_us(io->ctx, 10);
    /* T210B01 has no T210 SLCG/MBIST workaround. NVJPG is only a retained
     * power dependency here, not an engine whose reset/clock we may change. */
    p->active = true;
    return TG_NVDEC_PLATFORM_OK;
}

enum tg_nvdec_platform_result tg_nvdec_platform_shutdown(
    const struct tg_nvdec_platform_io *io, struct tg_nvdec_platform *p)
{
    enum tg_nvdec_platform_result result;
    uint32_t value;
    if (!valid_io(io) || !p)
        return TG_NVDEC_PLATFORM_ARGUMENT;
    if (p->nvdec_touched) {
        result = tg_nvdec_platform_quiesce(io, p);
        if (result != TG_NVDEC_PLATFORM_OK)
            return result;
        w32(io, TG_NVDEC_AP_CAR, CAR_CLK_OUT_ENB_Y_CLR, CAR_NVDEC_BIT);
        if (!poll_bit(io, TG_NVDEC_AP_CAR, CAR_CLK_OUT_ENB_Y, CAR_NVDEC_BIT, false, 100))
            return TG_NVDEC_PLATFORM_CLOCK;
        p->nvdec_clock = false;
        w32(io, TG_NVDEC_AP_CAR, CAR_CLK_SOURCE_NVDEC, p->saved_source);
        value = r32(io, TG_NVDEC_AP_CAR, CAR_CLK_SOURCE_NVDEC);
        if (value == UINT32_MAX ||
            (value & CAR_NVDEC_MUX_MASK) != (p->saved_source & CAR_NVDEC_MUX_MASK))
            return TG_NVDEC_PLATFORM_CLOCK;
        if (!set_mc_flush(io, (p->saved_mc_flush & MC_NVDEC_BIT) != 0))
            return TG_NVDEC_PLATFORM_RESET;
        /* Reset remains asserted, matching the admitted initial state.
         * Never turn off either inherited partition or restore whole banks. */
        p->nvdec_touched = false;
        p->active = false;
    }
    if (p->channel_started) {
        /* A timed-out ring can leave GET behind PUT even after teardown.
         * With DMA stopped and quiescence proved, clear its backing addresses
         * and initialize GET from the empty PUT before releasing the lease.
         * This is channel retirement, not a submission or syncpoint signal. */
        if (!p->quiesced || !p->host1x_clock)
            return TG_NVDEC_PLATFORM_OWNERSHIP;
        channel_stop(io, p);
        w32(io, TG_NVDEC_AP_HOST1X, channel_base(p) + HOST1X_CHANNEL_DMAPUT, 0);
        w32(io, TG_NVDEC_AP_HOST1X, channel_base(p) + HOST1X_CHANNEL_DMASTART, 0);
        w32(io, TG_NVDEC_AP_HOST1X, channel_base(p) + HOST1X_CHANNEL_DMAEND, 0);
        w32(io, TG_NVDEC_AP_HOST1X, channel_base(p) + HOST1X_CHANNEL_DMACTRL,
            HOST1X_DMACTRL_STOP | HOST1X_DMACTRL_GETRST | HOST1X_DMACTRL_INITGET);
        w32(io, TG_NVDEC_AP_HOST1X, channel_base(p) + HOST1X_CHANNEL_DMACTRL,
            HOST1X_DMACTRL_STOP);
        value = r32(io, TG_NVDEC_AP_HOST1X, channel_base(p) + HOST1X_CHANNEL_DMACTRL);
        if (value == UINT32_MAX || !(value & HOST1X_DMACTRL_STOP) ||
            r32(io, TG_NVDEC_AP_HOST1X, channel_base(p) + HOST1X_CHANNEL_DMAPUT) != 0 ||
            r32(io, TG_NVDEC_AP_HOST1X, channel_base(p) + HOST1X_CHANNEL_DMAGET) != 0)
            return TG_NVDEC_PLATFORM_SUBMIT;
        p->channel_started = false;
    }
    if (p->host1x_lease) {
        if (!p->owner.release_host1x || !p->owner.release_host1x(p->owner.ctx))
            return TG_NVDEC_PLATFORM_CLOCK;
        p->host1x_lease = false;
    }
    tg_nvdec_platform_zero(p);
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
    if (!valid_io(io) || !p || p->channel >= 12)
        return TG_NVDEC_PLATFORM_ARGUMENT;
    if (!p->nvdec_touched || p->quiesced)
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
    if (!set_mc_flush(io, true))
        return TG_NVDEC_PLATFORM_RESET;
    if (!wait_mc_drained(io) ||
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

    if (!valid_io(io) || !p || !gather || !words || words > 0xffffu || !push ||
        push_size < 16u || (push_size & 3u) || (gather_bus & 3u) ||
        (push_bus & 15u) || (gather_bus >> 32) || (push_bus >> 32) ||
        (uint64_t)words * 4u > UINT64_C(0x100000000) - gather_bus ||
        (uint64_t)push_size + 4u > UINT32_MAX ||
        (uint64_t)push_size + 4u > UINT64_C(0x100000000) - push_bus ||
        !expected || !observed)
        return TG_NVDEC_PLATFORM_ARGUMENT;
    if (!p->active || !p->host1x_clock || !p->nvdec_clock || p->quiesced ||
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
    /* DMAEND is relative to DMASTART, including the wrap word. The old
     * upstream absolute-address interpretation was corrected by Linux
     * 0e43b8da154a95f4369da4068a43ad9d700f4cea; it could permit prefetch
     * beyond the mapped push buffer. */
    w32(io, TG_NVDEC_AP_HOST1X, base + HOST1X_CHANNEL_DMAEND,
        push_size + 4u);
    w32(io, TG_NVDEC_AP_HOST1X, base + HOST1X_CHANNEL_DMACTRL,
        HOST1X_DMACTRL_STOP | HOST1X_DMACTRL_GETRST | HOST1X_DMACTRL_INITGET);
    value = r32(io, TG_NVDEC_AP_HOST1X, base + HOST1X_CHANNEL_DMACTRL);
    if (value == UINT32_MAX || !(value & HOST1X_DMACTRL_STOP) ||
        r32(io, TG_NVDEC_AP_HOST1X, base + HOST1X_CHANNEL_DMASTART) != (uint32_t)push_bus ||
        r32(io, TG_NVDEC_AP_HOST1X, base + HOST1X_CHANNEL_DMAEND) != push_size + 4u ||
        r32(io, TG_NVDEC_AP_HOST1X, base + HOST1X_CHANNEL_DMAPUT) != 0 ||
        r32(io, TG_NVDEC_AP_HOST1X, base + HOST1X_CHANNEL_DMAGET) != 0)
        return TG_NVDEC_PLATFORM_SUBMIT;
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
