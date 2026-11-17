/* SPDX-License-Identifier: MIT */
/* CPU-only ownership/failure model, never proof of hardware DMA or clocks. */
#include "t214_nvdec.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; \
} } while (0)

#define VIDEO_POWER (3u << 25)
#define VIDEO_CLOCK (3u << 2)
#define NVDEC_FLUSH (1u << 5)
#define CHANNEL 11u
#define CH_BASE (CHANNEL * 0x4000u)
#define SYNCPT 31u
#define SYNC_OFFSET (0x2100u + 0xf80u + SYNCPT * 4u)

static struct model {
    uint32_t pmc[0x1000/4], car[0x1000/4], mc[0x1000/4];
    uint32_t host[0x40000/4], nvdec[0x40000/4];
    uint32_t *push;
    uint32_t push_bytes;
    unsigned writes, pmc_writes, host_writes, mbist_writes, global_clock_writes;
    unsigned acquire_calls, release_calls, status_reads;
    uint32_t drop_host_write;
    bool held, acquire_fail, release_fail, source_fail, gate_clear_fail;
    bool reset_fail, mc_hang, mc_pulse, submit_hang, get_reset_fail;
} m;

static uint32_t *regs(enum tg_nvdec_aperture ap)
{
    switch (ap) {
    case TG_NVDEC_AP_NVDEC: return m.nvdec;
    case TG_NVDEC_AP_HOST1X: return m.host;
    case TG_NVDEC_AP_PMC: return m.pmc;
    case TG_NVDEC_AP_CAR: return m.car;
    case TG_NVDEC_AP_MC: return m.mc;
    }
    return NULL;
}

static uint32_t model_read(void *ctx, enum tg_nvdec_aperture ap, uint32_t off)
{
    CHECK(ctx == &m);
    if (ap == TG_NVDEC_AP_HOST1X) CHECK(m.held);
    if (ap == TG_NVDEC_AP_MC && off == 0x974) {
        ++m.status_reads;
        if (m.mc_pulse) return m.status_reads % 4 ? NVDEC_FLUSH : 0;
    }
    uint32_t *r = regs(ap);
    CHECK(r && off < (ap <= TG_NVDEC_AP_HOST1X ? 0x40000u : 0x1000u));
    return r ? r[off/4] : UINT32_MAX;
}

static void model_write(void *ctx, enum tg_nvdec_aperture ap, uint32_t off, uint32_t v)
{
    CHECK(ctx == &m); ++m.writes;
    uint32_t *r = regs(ap);
    CHECK(r != NULL);
    if (!r) return;
    if (ap == TG_NVDEC_AP_PMC) { ++m.pmc_writes; CHECK(false); }
    if (ap == TG_NVDEC_AP_CAR && off == 0x554) { ++m.mbist_writes; CHECK(false); }
    if (ap == TG_NVDEC_AP_CAR && (off == 0x010 || off == 0x320 || off == 0x324)) {
        ++m.global_clock_writes; CHECK(false);
    }
    if (ap == TG_NVDEC_AP_CAR && off == 0x698 && m.source_fail) return;
    if (ap == TG_NVDEC_AP_HOST1X) {
        ++m.host_writes; CHECK(m.held); CHECK(off != SYNC_OFFSET);
        if (off == 0x21ac) CHECK(!((r[off/4] ^ v) & ~(1u << CHANNEL)));
        else if (off == 0x21b0) CHECK(v == (1u << CHANNEL));
        else CHECK(off == CH_BASE+0x14u || off == CH_BASE+0x18u ||
                   off == CH_BASE+0x20u || off == CH_BASE+0x24u || off == CH_BASE+0x98u);
        if (off == m.drop_host_write) return;
    }
    r[off/4] = v;
    if (ap == TG_NVDEC_AP_CAR && off == 0x29c) m.car[0x298/4] |= v;
    if (ap == TG_NVDEC_AP_CAR && off == 0x2a0 && !m.gate_clear_fail) m.car[0x298/4] &= ~v;
    if (ap == TG_NVDEC_AP_CAR && off == 0x2a8 && !m.reset_fail) m.car[0x2a4/4] |= v;
    if (ap == TG_NVDEC_AP_CAR && off == 0x2ac) m.car[0x2a4/4] &= ~v;
    if (ap == TG_NVDEC_AP_MC && off == 0x970 && !m.mc_hang) m.mc[0x974/4] = v;
    if (ap == TG_NVDEC_AP_HOST1X && off == 0x21b0 && (v & (1u << CHANNEL)))
        m.host[CH_BASE/4] |= 1u << 11;
    if (ap == TG_NVDEC_AP_HOST1X && off == CH_BASE + 0x24 &&
        (v & 7u) == 7u && !m.get_reset_fail)
        m.host[(CH_BASE+0x1c)/4] = m.host[(CH_BASE+0x18)/4];
    if (ap == TG_NVDEC_AP_HOST1X && off == CH_BASE + 0x18 && v == 16u) {
        uint64_t start = m.host[(CH_BASE+0x14)/4];
        uint64_t end = m.host[(CH_BASE+0x20)/4];
        /* END bounds the offset/prefetch space, not an absolute address.
         * The modeled allocation must cover it before completion is modeled. */
        if (end < v || end > m.push_bytes ||
            start + end > UINT64_C(0x100000000)) { CHECK(false); return; }
        CHECK(m.push && m.push[3] == 0x20000000u);
        if (!m.submit_hang) ++m.host[SYNC_OFFSET/4];
    }
}

static void model_delay(void *ctx, uint32_t us) { CHECK(ctx == &m && us <= 100u); }
static bool acquire_clock(void *ctx)
{
    CHECK(ctx == &m && !m.held); ++m.acquire_calls;
    /* Include failure after the provider took a reference. */
    m.held = true;
    return !m.acquire_fail;
}
static bool release_clock(void *ctx)
{
    CHECK(ctx == &m && m.held); ++m.release_calls;
    CHECK(!(m.car[0x298/4] & (1u << 2)));
    CHECK(m.car[0x2a4/4] & (1u << 2));
    if (m.release_fail) return false;
    m.held = false;
    return true;
}
static bool clock_live(void *ctx) { CHECK(ctx == &m); return m.held; }
static const struct tg_nvdec_platform_io io = {&m, model_read, model_write, model_delay};
static const struct tg_nvdec_platform_owner owner = {
    &m, 0x00012127u, 0x48005504u, acquire_clock, release_clock, clock_live
};
static void prepare(void)
{
    memset(&m, 0, sizeof(m));
    /* Power/CAR from resource preflight and STOP/CFEMPTY from native admission.
     * These model values do not establish that a new ring executes on hardware. */
    m.pmc[0x38/4] = 0x3ffecf4d;
    m.car[0x298/4] = 0x342;
    m.car[0x2a4/4] = 0x0fe9d09d;
    m.car[0x698/4] = 0xe0000000;
    m.car[0x69c/4] = 0xe0000000;
    m.mc[0x970/4] = m.mc[0x974/4] = 4;
    m.host[CH_BASE/4] = 1u << 11;
    m.host[(CH_BASE+0x24)/4] = 1u;
}
static enum tg_nvdec_platform_result start(struct tg_nvdec_platform *p)
{ return tg_nvdec_platform_init(&io, p, CHANNEL, SYNCPT, &owner); }
static void retained_boot_state(void)
{
    CHECK((m.pmc[0x38/4] & VIDEO_POWER) == VIDEO_POWER);
    CHECK(!(m.pmc[0x2c/4] & VIDEO_POWER));
    CHECK(!(m.car[0x298/4] & VIDEO_CLOCK));
    CHECK((m.car[0x2a4/4] & VIDEO_CLOCK) == VIDEO_CLOCK);
    CHECK(m.car[0x698/4] == 0xe0000000 && m.car[0x69c/4] == 0xe0000000);
    CHECK(!m.pmc_writes && !m.mbist_writes && !m.global_clock_writes);
}
static void test_inherited_and_submit(void)
{
    struct tg_nvdec_platform p = {0};
    uint32_t gather[8] = {0}, push[16 * 1024 + 2] = {0}, expected = 0, observed = 0;
    prepare(); m.push = push; m.push_bytes = 16u * 1024u + 4u;
    /* Actual first-use snapshot: no software queue exists yet. The stopped
     * channel's old pointers are not completion evidence for our new ring. */
    m.host[(CH_BASE+0x14)/4] = 0x108e00a8u;
    m.host[(CH_BASE+0x18)/4] = 0;
    m.host[(CH_BASE+0x1c)/4] = 0x00121010u;
    m.host[(CH_BASE+0x20)/4] = 0xa800a800u;
    /* Preserve the unrelated stopped channel observed on the native board. */
    m.host[0]=0x80100840u; m.host[0x18/4]=0; m.host[0x1c/4]=0x21d84cf0u; m.host[0x24/4]=1;
    m.host[0x21ac/4]=1u; /* another channel's command-processor stop bit */
    CHECK(start(&p) == TG_NVDEC_PLATFORM_OK);
    CHECK(p.active && p.nvdec_clock && p.host1x_clock && p.host1x_lease);
    CHECK(m.host[(CH_BASE+0x1c)/4] == 0x00121010u && !m.host[SYNC_OFFSET/4]);
    CHECK(m.car[0x298/4] & (1u << 2));
    CHECK(m.car[0x698/4] == 0x80000002u); /* PLLP OUT0 / 2 */
    CHECK(!(m.car[0x2a4/4] & (1u << 2)) && (m.car[0x2a4/4] & (1u << 3)));
    const unsigned writes = m.writes;
    CHECK(start(&p) == TG_NVDEC_PLATFORM_OWNERSHIP && m.writes == writes && m.acquire_calls == 1);
    CHECK(tg_nvdec_host1x_submit(&io, &p, gather, 5, 0x90000000,
          push, 0x80000000, 16u * 1024u, 100, &expected, &observed) == TG_NVDEC_PLATFORM_OK);
    CHECK(expected == 1 && observed == 1 && push[0] == 0x3c00 && push[1] == 0x60000005);
    CHECK(m.host[(CH_BASE+0x20)/4] == 0x4004u);
    CHECK(push[3] == 0x20000000 && push[4096] == 0x50000000);
    m.submit_hang = true;
    CHECK(tg_nvdec_host1x_submit(&io, &p, gather, 5, 0x90000000,
          push, 0x80000000, 16u * 1024u, 2, &expected, &observed) == TG_NVDEC_PLATFORM_TIMEOUT);
    CHECK(observed == 1 && m.host[(CH_BASE+0x24)/4] == 1);
    CHECK(tg_nvdec_host1x_submit(&io, &p, gather, 5, 0x190000000ULL,
          push, 0x80000000, 16u * 1024u, 2, &expected, &observed) == TG_NVDEC_PLATFORM_ARGUMENT);
    CHECK(tg_nvdec_host1x_submit(&io, &p, gather, 5, 0x90000000,
          push, 0x180000000ULL, 16u * 1024u, 2, &expected, &observed) == TG_NVDEC_PLATFORM_ARGUMENT);
    /* Independent clients changed unrelated bits after our snapshot. */
    m.car[0x298/4] |= 1u << 15; m.mc[0x970/4] |= 1u << 12;
    CHECK(tg_nvdec_platform_shutdown(&io, &p) == TG_NVDEC_PLATFORM_OK);
    CHECK(!p.captured && !p.active && !m.held && m.release_calls == 1);
    CHECK(m.car[0x298/4] & (1u << 15)); CHECK(m.mc[0x970/4] == (4 | (1u << 12)));
    retained_boot_state(); CHECK(m.status_reads >= 5);
    const unsigned stopped_writes = m.writes;
    CHECK(tg_nvdec_platform_shutdown(&io, &p) == TG_NVDEC_PLATFORM_OK && m.writes == stopped_writes);
    /* A retired timed-out ring must not prevent the same reserved channel
     * from being acquired for a later decoder session. */
    CHECK(start(&p) == TG_NVDEC_PLATFORM_OK);
    CHECK(tg_nvdec_platform_shutdown(&io, &p) == TG_NVDEC_PLATFORM_OK);
    CHECK(m.host[0]==0x80100840u && !m.host[0x18/4] && m.host[0x1c/4]==0x21d84cf0u && m.host[0x24/4]==1);
    CHECK(m.host[0x21ac/4] & 1u);
    retained_boot_state();
}
static void test_refuse_foreign_or_unknown_state(void)
{
    for (unsigned bad = 0; bad < 15; ++bad) {
        struct tg_nvdec_platform p = {0}; prepare();
        if (bad == 0) m.pmc[0x38/4] &= ~(1u << 25);
        if (bad == 1) m.pmc[0x38/4] &= ~(1u << 26);
        if (bad == 2) m.pmc[0x2c/4] = 1u << 25;
        if (bad == 3) m.pmc[0x2c/4] = 1u << 26;
        if (bad == 4) m.pmc[0x30/4] = 1u << 8;
        if (bad == 5) m.car[0x298/4] |= 1u << 2;
        if (bad == 6) m.car[0x298/4] |= 1u << 3;
        if (bad == 7) m.car[0x2a4/4] &= ~(1u << 2);
        if (bad == 8) m.car[0x2a4/4] &= ~(1u << 3);
        if (bad == 9) m.car[0x698/4] = UINT32_MAX;
        if (bad == 10) m.host[CH_BASE/4] = 0;
        if (bad == 11) m.host[(CH_BASE+0x24)/4] = 0; /* running, even GET==PUT */
        if (bad == 12) m.host[(CH_BASE+0x24)/4] = UINT32_MAX;
        if (bad == 13) m.host[(CH_BASE+0x24)/4] = 5; /* init in progress */
        if (bad == 14) m.host[CH_BASE/4] = UINT32_MAX;
        CHECK(start(&p) != TG_NVDEC_PLATFORM_OK);
        CHECK(!m.writes && !p.active);
        CHECK(tg_nvdec_platform_shutdown(&io, &p) == TG_NVDEC_PLATFORM_OK && !m.held);
        CHECK(m.acquire_calls == (bad >= 10 ? 1u : 0u));
    }
    struct tg_nvdec_platform p = {0}; struct tg_nvdec_platform_owner invalid = owner;
    prepare(); invalid.hidrev = 0x2110;
    CHECK(tg_nvdec_platform_init(&io, &p, CHANNEL, SYNCPT, &invalid) == TG_NVDEC_PLATFORM_UNSUPPORTED);
    invalid = owner; invalid.pllp_base = 0;
    CHECK(tg_nvdec_platform_init(&io, &p, CHANNEL, SYNCPT, &invalid) == TG_NVDEC_PLATFORM_CLOCK);
    invalid = owner; invalid.acquire_host1x = NULL;
    CHECK(tg_nvdec_platform_init(&io, &p, CHANNEL, SYNCPT, &invalid) == TG_NVDEC_PLATFORM_ARGUMENT);
    CHECK(!m.writes && !m.acquire_calls);
}
static void test_fresh_ring_readback_failure(void)
{
    const uint32_t dropped[] = {0, CH_BASE+0x14u, CH_BASE+0x20u};
    for (unsigned fail = 0; fail < sizeof(dropped)/sizeof(dropped[0]); ++fail) {
        struct tg_nvdec_platform p = {0};
        uint32_t gather[8] = {0}, push[16] = {0}, expected = 123, observed = 456;
        prepare(); m.push = push; m.push_bytes = sizeof(push);
        m.host[(CH_BASE+0x1c)/4] = 0x00121010u;
        CHECK(start(&p) == TG_NVDEC_PLATFORM_OK);
        m.get_reset_fail = fail == 0;
        m.drop_host_write = dropped[fail];
        CHECK(tg_nvdec_host1x_submit(&io, &p, gather, 5, 0x90000000u,
              push, 0x80000000u, 16u, 2, &expected, &observed) == TG_NVDEC_PLATFORM_SUBMIT);
        CHECK((m.host[(CH_BASE+0x24)/4] & 1u) && !m.host[(CH_BASE+0x18)/4]);
        CHECK(!m.host[SYNC_OFFSET/4] && expected == 123 && observed == 456);
        CHECK(p.active && p.host1x_lease && m.held && !m.release_calls);
        m.get_reset_fail = false; m.drop_host_write = 0;
        m.mc_hang = true;
        CHECK(tg_nvdec_platform_shutdown(&io, &p) == TG_NVDEC_PLATFORM_TIMEOUT);
        CHECK(p.captured && p.host1x_lease && m.held && !m.release_calls);
        m.mc_hang = false;
        CHECK(tg_nvdec_platform_shutdown(&io, &p) == TG_NVDEC_PLATFORM_OK);
        retained_boot_state();
    }
}
static void test_push_address_boundary(void)
{
    struct tg_nvdec_platform p = {0};
    uint32_t gather[8] = {0}, push[16] = {0}, expected = 0, observed = 0;
    prepare(); m.push = push; m.push_bytes = sizeof(push);
    CHECK(start(&p) == TG_NVDEC_PLATFORM_OK);
    unsigned writes = m.writes;
    /* The physical span may end exactly at2^32, but not cross it. END
     * itself is the small relative offset and must not wrap to zero. */
    CHECK(tg_nvdec_host1x_submit(&io, &p, gather, 5, 0x90000000,
          push, 0xffffffe0u, 32, 1, &expected, &observed) == TG_NVDEC_PLATFORM_ARGUMENT);
    CHECK(tg_nvdec_host1x_submit(&io, &p, gather, 5, 0x90000000,
          push, 0, 0xfffffffcu, 1, &expected, &observed) == TG_NVDEC_PLATFORM_ARGUMENT);
    CHECK(m.writes == writes && !expected && !observed);
    CHECK(tg_nvdec_host1x_submit(&io, &p, gather, 5, 0x90000000,
          push, 0xffffffe0u, 28, 1, &expected, &observed) == TG_NVDEC_PLATFORM_OK);
    CHECK(m.host[(CH_BASE+0x14)/4] == 0xffffffe0u && m.host[(CH_BASE+0x20)/4] == 32u);
    CHECK(tg_nvdec_platform_shutdown(&io, &p) == TG_NVDEC_PLATFORM_OK);
    retained_boot_state();
}
static void test_failed_cleanup_retains_ownership(void)
{
    struct tg_nvdec_platform p = {0};
    prepare(); m.acquire_fail = true; m.release_fail = true;
    CHECK(start(&p) == TG_NVDEC_PLATFORM_CLOCK && p.host1x_lease && m.held && !m.writes);
    CHECK(tg_nvdec_platform_shutdown(&io, &p) == TG_NVDEC_PLATFORM_CLOCK && p.captured);
    m.release_fail = false;
    CHECK(tg_nvdec_platform_shutdown(&io, &p) == TG_NVDEC_PLATFORM_OK && !p.captured && !m.held);
    retained_boot_state();

    prepare(); m.source_fail = true;
    CHECK(start(&p) == TG_NVDEC_PLATFORM_CLOCK && p.nvdec_touched && m.held);
    m.source_fail = false;
    CHECK(tg_nvdec_platform_shutdown(&io, &p) == TG_NVDEC_PLATFORM_OK); retained_boot_state();

    for (unsigned fail = 0; fail < 6; ++fail) {
        prepare(); CHECK(start(&p) == TG_NVDEC_PLATFORM_OK);
        if (fail == 0) m.mc_hang = true;
        if (fail == 1) m.mc_pulse = true;
        if (fail == 2) m.reset_fail = true;
        if (fail == 3) m.gate_clear_fail = true;
        if (fail == 4) m.release_fail = true;
        if (fail == 5) { m.get_reset_fail = true; m.host[(CH_BASE+0x1c)/4] = 8; }
        CHECK(tg_nvdec_platform_shutdown(&io, &p) != TG_NVDEC_PLATFORM_OK);
        CHECK(p.captured && p.host1x_lease && m.held);
        CHECK(m.release_calls == (fail == 4 ? 1u : 0u));
        CHECK(!m.pmc_writes && !m.mbist_writes && !m.global_clock_writes);
        m.mc_hang = m.mc_pulse = m.reset_fail = m.gate_clear_fail = m.release_fail = m.get_reset_fail = false;
        CHECK(tg_nvdec_platform_shutdown(&io, &p) == TG_NVDEC_PLATFORM_OK && !m.held);
        retained_boot_state();
    }
}
static void test_direct_route_is_not_assumed(void)
{
    prepare();
    CHECK(tg_nvdec_platform_direct_dma(&io));
    /* T210 ATF enables every client; an unassigned SWGROUP still bypasses.
     * Match the real boot snapshot without pretending CONFIG was readable. */
    m.mc[0x10/4] = m.mc[0x228/4] = m.mc[0x22c/4] = m.mc[0x234/4] = UINT32_MAX;
    CHECK(tg_nvdec_platform_direct_dma(&io));
    const uint32_t offsets[] = {0x250, 0xab4};
    for (unsigned enabled = 0; enabled < 2; ++enabled) {
        m.mc[0x228/4] = m.mc[0x22c/4] = m.mc[0x234/4] = enabled ? UINT32_MAX : 0;
        for (unsigned i = 0; i < sizeof(offsets)/sizeof(offsets[0]); ++i) {
            m.mc[offsets[i]/4] = 0x01020304; /* saved ASIDs, enable clear */
            CHECK(tg_nvdec_platform_direct_dma(&io));
            m.mc[offsets[i]/4] |= 1u << 31;
            CHECK(!tg_nvdec_platform_direct_dma(&io));
            m.mc[offsets[i]/4] = UINT32_MAX;
            CHECK(!tg_nvdec_platform_direct_dma(&io));
            m.mc[offsets[i]/4] = 0;
        }
    }
    /* NVJPG does not execute DMA for H.264. Its dependency is power-only. */
    m.mc[0x234/4] = 3u << 30; m.mc[0xac0/4] = 1u << 31;
    CHECK(tg_nvdec_platform_direct_dma(&io) && !m.writes);
    CHECK(!tg_nvdec_platform_direct_dma(NULL));
}
static void test_clock_admission(void)
{
    /* The actual Windows route needs an enforced floor, not a temporary high
     * EMC voltage. No clock/voltage write is introduced by this admission. */
    CHECK(tg_nvdec_platform_clocks_supported(0x48115408u,0x80000003u,650000u));
    CHECK(tg_nvdec_platform_clocks_supported(0x48115408u,0x80000002u,650000u));
    CHECK(!tg_nvdec_platform_clocks_supported(0x48115408u,0x80000003u,637500u));
    CHECK(!tg_nvdec_platform_clocks_supported(0x48115408u,0x80000001u,1050000u));
    const uint32_t bad_floor[]={0u,600000u,637499u,649999u,1062500u,UINT32_MAX};
    for (unsigned i=0;i<sizeof(bad_floor)/sizeof(bad_floor[0]);++i)
        CHECK(!tg_nvdec_platform_clocks_supported(0x48115408u,0x80000008u,bad_floor[i]));
    CHECK(tg_nvdec_platform_clocks_supported(0x48115408u,0x80000006u,637500u));
    CHECK(tg_nvdec_platform_clocks_supported(0x48115408u,0x80000008u,637500u));
    CHECK(tg_nvdec_platform_clocks_supported(0x48115408u,0xa0000000u,637500u));
    CHECK(!tg_nvdec_platform_clocks_supported(0x48115408u,0x80000005u,637500u));
    CHECK(!tg_nvdec_platform_clocks_supported(0x48115408u,0x80000000u,637500u));
    CHECK(!tg_nvdec_platform_clocks_supported(0x48115408u,0x60000006u,637500u));
    CHECK(!tg_nvdec_platform_clocks_supported(0x48115408u,0xe0000000u,637500u));
    CHECK(!tg_nvdec_platform_clocks_supported(0x48115408u,0x80000106u,637500u));
    CHECK(!tg_nvdec_platform_clocks_supported(UINT32_MAX,0x80000006u,637500u));
    CHECK(!tg_nvdec_platform_clocks_supported(0x48115408u,UINT32_MAX,637500u));
    CHECK(!tg_nvdec_platform_clocks_supported(0x40115408u,0x80000006u,637500u));
    CHECK(!tg_nvdec_platform_clocks_supported(0xc8115408u,0x80000006u,637500u));
    CHECK(tg_nvdec_platform_clocks_supported(0x58005508u,0x80000006u,637500u));
    CHECK(!tg_nvdec_platform_clocks_supported(0x58005408u,0x80000006u,637500u));
    CHECK(!tg_nvdec_platform_clocks_supported(0x58005500u,0x80000006u,637500u));
}
int main(void)
{
    test_inherited_and_submit();
    test_push_address_boundary();
    test_fresh_ring_readback_failure();
    test_refuse_foreign_or_unknown_state();
    test_failed_cleanup_retains_ownership();
    test_direct_route_is_not_assumed();
    test_clock_admission();
    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    puts("PASS: B01 inherited power, shared-clock lease, failure retention and Host1x completion (CPU model only)");
    return 0;
}
