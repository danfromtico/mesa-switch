/* SPDX-License-Identifier: MIT
 *
 * Host model tests for T210 NVDEC power/clock/reset and Host1x channel
 * programming.  The model only checks register sequencing and the real
 * syncpoint wait path; it is not a hardware decode result.
 */
#include "t214_nvdec.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

static struct pmodel m;

struct pmodel {
    uint32_t pmc[0x1000 / 4];
    uint32_t car[0x1000 / 4];
    uint32_t mc[0x1000 / 4];
    uint32_t host1x[0x40000 / 4];
    uint32_t nvdec[0x40000 / 4];
    uint32_t *push;
    uint32_t push_size;
    uint32_t reset_set, reset_clr;
    uint32_t mbist[4];
    unsigned mbist_count;
    bool hang;
    bool mc_hang;
    bool power_hang;
    bool submit_seen;
    uint32_t submit_words;
    uint64_t submit_bus;
};

static uint32_t *regs(struct pmodel *m, enum tg_nvdec_aperture ap)
{
    switch (ap) {
    case TG_NVDEC_AP_NVDEC: return m->nvdec;
    case TG_NVDEC_AP_HOST1X: return m->host1x;
    case TG_NVDEC_AP_PMC: return m->pmc;
    case TG_NVDEC_AP_CAR: return m->car;
    case TG_NVDEC_AP_MC: return m->mc;
    }
    return NULL;
}

static uint32_t model_read(void *ctx, enum tg_nvdec_aperture ap, uint32_t offset)
{
    struct pmodel *m = ctx;
    uint32_t *r = regs(m, ap);
    if (!r || offset >= (ap == TG_NVDEC_AP_HOST1X ? 0x40000u :
                         ap == TG_NVDEC_AP_NVDEC ? 0x40000u : 0x1000u))
        return 0;
    return r[offset / 4];
}

static void model_write(void *ctx, enum tg_nvdec_aperture ap, uint32_t offset,
                        uint32_t value)
{
    struct pmodel *m = ctx;
    uint32_t *r = regs(m, ap);
    if (!r)
        return;
    r[offset / 4] = value;
    if (ap == TG_NVDEC_AP_PMC && offset == 0x30) {
        if (m->power_hang) return;
        if (value & 0x100u) {
            m->pmc[0x30 / 4] &= ~0x100u;
            const uint32_t id = value & 0x1fu;
            if (m->pmc[0x38 / 4] & (1u << id))
                m->pmc[0x38 / 4] &= ~(1u << id);
            else
                m->pmc[0x38 / 4] |= 1u << id;
        }
    } else if (ap == TG_NVDEC_AP_CAR && offset == 0x320) {
        m->car[0x010 / 4] |= value;
    } else if (ap == TG_NVDEC_AP_CAR && offset == 0x29c) {
        m->car[0x298 / 4] |= value;
    } else if (ap == TG_NVDEC_AP_CAR && offset == 0x2a0) {
        m->car[0x298 / 4] &= ~value;
    } else if (ap == TG_NVDEC_AP_CAR && offset == 0x2a8) {
        m->reset_set |= value;
        m->car[0x2a4 / 4] |= value;
    } else if (ap == TG_NVDEC_AP_CAR && offset == 0x2ac) {
        m->reset_clr |= value;
        m->car[0x2a4 / 4] &= ~value;
    } else if (ap == TG_NVDEC_AP_MC && offset == 0x970) {
        if (!m->mc_hang) m->mc[0x974 / 4] = value;
    } else if (ap == TG_NVDEC_AP_HOST1X && offset == 0x21b0) {
        for (unsigned ch = 0; ch < 12; ++ch)
            if (value & (1u << ch)) m->host1x[ch * 0x4000 / 4] |= 1u << 11;
    } else if (ap == TG_NVDEC_AP_CAR && offset == 0x554) {
        if (m->mbist_count < 4)
            m->mbist[m->mbist_count++] = value;
    } else if (ap == TG_NVDEC_AP_HOST1X && (offset & 0x3fffu) == 0x18u &&
               value == 16u && m->push) {
        m->submit_seen = true;
        m->submit_words = m->push[1] & 0xffffu;
        m->submit_bus = m->push[2];
        if (!m->hang) {
            const uint32_t ch = offset / 0x4000u;
            const uint32_t syncpt = 31u;
            (void)ch;
            m->host1x[(0x2100u + 0xf80u + syncpt * 4u) / 4u] = 1u;
        }
    }
}

static void model_delay(void *ctx, uint32_t us)
{
    (void)ctx;
    (void)us;
}

static const struct tg_nvdec_platform_io model_io = {
    .ctx = &m,
    .read32 = model_read,
    .write32 = model_write,
    .delay_us = model_delay,
};

static void test_power_and_submit(void)
{
    struct tg_nvdec_platform p;
    uint32_t gather[8] = { 0 };
    uint32_t push[16 * 1024 + 2] = { 0 };
    uint32_t expected = 0, observed = 0;
    enum tg_nvdec_platform_result r;

    memset(&m, 0, sizeof(m));
    m.push = push;
    m.push_size = 16u * 1024u;
    r = tg_nvdec_platform_init(&model_io, &p, 11, 31);
    CHECK(r == TG_NVDEC_PLATFORM_OK);
    CHECK(p.powered && p.nvdec_clock && p.host1x_clock);
    CHECK(m.pmc[0x38 / 4] & (1u << 25));
    CHECK(m.car[0x298 / 4] & (1u << 2));
    CHECK((m.car[0x010 / 4] & (1u << 28)) != 0);
    CHECK((m.car[0x698 / 4] & 0xe0000000u) == (4u << 29));
    CHECK(m.reset_set & (1u << 2));
    CHECK(m.reset_clr & (1u << 2));
    CHECK(m.mbist_count == 2);
    CHECK((m.mbist[0] & ((1u << 9) | (1u << 31))) == ((1u << 9) | (1u << 31)));
    CHECK(m.mbist[1] == 0);

    gather[0] = 0x10100002u;
    gather[1] = 0x80u;
    gather[2] = 3u;
    gather[3] = 0x20000001u;
    gather[4] = 31u | (1u << 8);
    r = tg_nvdec_host1x_submit(&model_io, &p, gather, 5, 0x90000000ull,
                               push, 0x80000000ull, m.push_size,
                               100, &expected, &observed);
    CHECK(r == TG_NVDEC_PLATFORM_OK);
    CHECK(expected == 1 && observed == 1);
    CHECK(m.submit_seen);
    CHECK(m.submit_words == 5);
    CHECK(m.submit_bus == 0x90000000ull);
    CHECK(push[0] == 0x3c00u);
    CHECK(push[1] == (0x60000000u | 5u));
    CHECK(push[3] == 0x20000000u);
    CHECK(push[(m.push_size / 4)] == 0x50000000u);

    /* Timeout must stop the channel and report the observed value. */
    m.hang = true;
    m.submit_seen = false;
    r = tg_nvdec_host1x_submit(&model_io, &p, gather, 5, 0x90000000ull,
                               push, 0x80000000ull, m.push_size,
                               2, &expected, &observed);
    CHECK(r == TG_NVDEC_PLATFORM_TIMEOUT);
    CHECK(observed == 1);
    CHECK(m.host1x[(11u * 0x4000u + 0x24u) / 4u] == 1u);

    /* Addresses above 4 GiB are rejected before touching the channel. */
    r = tg_nvdec_host1x_submit(&model_io, &p, gather, 5, 0x190000000ull,
                               push, 0x80000000ull, m.push_size,
                               100, &expected, &observed);
    CHECK(r == TG_NVDEC_PLATFORM_ARGUMENT);
    r = tg_nvdec_host1x_submit(&model_io, &p, gather, 5, 0x90000000ull,
                               push, 0x180000000ull, m.push_size,
                               100, &expected, &observed);
    CHECK(r == TG_NVDEC_PLATFORM_ARGUMENT);
    CHECK(tg_nvdec_host1x_submit(NULL, &p, gather, 5, 0, push, 0x80000000ull,
                                 m.push_size, 1, &expected, &observed) ==
          TG_NVDEC_PLATFORM_ARGUMENT);

    /* Another client changes a shared clock bit after our initial snapshot. */
    m.car[0x298 / 4] |= 1u << 9;
    r = tg_nvdec_platform_shutdown(&model_io, &p);
    CHECK(r == TG_NVDEC_PLATFORM_OK);
    CHECK(!(m.pmc[0x38 / 4] & (1u << 25)));
    CHECK(!(m.car[0x298 / 4] & (1u << 2)));
    CHECK(m.car[0x298 / 4] & (1u << 9));
    CHECK(m.car[0x698 / 4] == 0);
    CHECK(!p.powered && !p.nvdec_clock);
}

static void test_quiesce_failures(void)
{
    struct tg_nvdec_platform p;
    memset(&m, 0, sizeof(m));
    CHECK(tg_nvdec_platform_init(&model_io, &p, 11, 31) == TG_NVDEC_PLATFORM_OK);
    m.mc_hang = true;
    CHECK(tg_nvdec_platform_quiesce(&model_io, &p) == TG_NVDEC_PLATFORM_TIMEOUT);
    CHECK(p.powered && !p.quiesced);
    CHECK(tg_nvdec_platform_shutdown(&model_io, &p) == TG_NVDEC_PLATFORM_TIMEOUT);
    CHECK(m.pmc[0x38 / 4] & (1u << 25));
    m.mc_hang = false;
    CHECK(tg_nvdec_platform_shutdown(&model_io, &p) == TG_NVDEC_PLATFORM_OK);
    CHECK(!p.powered);

    memset(&m, 0, sizeof(m));
    m.power_hang = true;
    CHECK(tg_nvdec_platform_init(&model_io, &p, 11, 31) == TG_NVDEC_PLATFORM_POWER);
    CHECK(p.power_transition);
    CHECK(tg_nvdec_platform_shutdown(&model_io, &p) == TG_NVDEC_PLATFORM_POWER);
    CHECK(p.power_transition);
    /* A delayed response confirms the original command did not power on. */
    m.pmc[0x30 / 4] = 0;
    m.power_hang = false;
    CHECK(tg_nvdec_platform_shutdown(&model_io, &p) == TG_NVDEC_PLATFORM_OK);
    CHECK(!p.powered && !p.power_transition);
    CHECK(!(m.pmc[0x38 / 4] & (1u << 25)));
}

static void test_arguments(void)
{
    struct tg_nvdec_platform p;
    CHECK(tg_nvdec_platform_init(NULL, &p, 0, 0) == TG_NVDEC_PLATFORM_ARGUMENT);
    CHECK(tg_nvdec_platform_init(&model_io, NULL, 0, 0) == TG_NVDEC_PLATFORM_ARGUMENT);
    CHECK(tg_nvdec_platform_init(&model_io, &p, 12, 0) == TG_NVDEC_PLATFORM_ARGUMENT);
    CHECK(tg_nvdec_platform_init(&model_io, &p, 0, 192) == TG_NVDEC_PLATFORM_ARGUMENT);
    CHECK(tg_nvdec_platform_shutdown(NULL, &p) == TG_NVDEC_PLATFORM_ARGUMENT);
}

int main(void)
{
    test_power_and_submit();
    test_arguments();
    test_quiesce_failures();
    if (failures) {
        fprintf(stderr, "T210 NVDEC platform model: %d failure(s)\n", failures);
        return 1;
    }
    printf("T210 NVDEC power/clock/reset and Host1x syncpoint model: PASS (model only)\n");
    return 0;
}
