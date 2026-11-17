/* SPDX-License-Identifier: MIT
 *
 * Tegra210 NVDEC platform bring-up and Host1x channel submission.
 *
 * The B01 path borrows the firmware's powered, unclamped, idle partitions.
 * It never toggles inherited NVDEC/NVJPG power or applies the T210 MBIST WAR.
 * Production wiring must supply a real clock owner and reserved channel /
 * syncpoint, plus verified PnP/DMA ownership; a CPU fixture is not that owner.
 * Host1x global reset is outside this module's ownership. Completion must
 * come from the Host1x syncpoint advanced by the NVDEC channel.
 */
#ifndef TGD3D_T214_NVDEC_H
#define TGD3D_T214_NVDEC_H

#include <stdbool.h>
#include <stdint.h>

enum tg_nvdec_aperture {
    TG_NVDEC_AP_NVDEC = 0,
    TG_NVDEC_AP_HOST1X,
    TG_NVDEC_AP_PMC,
    TG_NVDEC_AP_CAR,
    TG_NVDEC_AP_MC,
};

struct tg_nvdec_platform_io {
    void *ctx;
    /* Ordered register accesses, including CPU writes to DMA backing before
     * the DMAPUT publication. The parent maps only its assigned PnP views. */
    uint32_t (*read32)(void *ctx, enum tg_nvdec_aperture ap, uint32_t offset);
    void (*write32)(void *ctx, enum tg_nvdec_aperture ap, uint32_t offset, uint32_t value);
    void (*delay_us)(void *ctx, uint32_t microseconds);
};

struct tg_nvdec_platform_owner {
    void *ctx;
    uint32_t hidrev;
    uint32_t pllp_base; /* real locked PLLP readback from its clock owner */
    /* Acquire pins the shared Host1x clock and the retained 408 MHz PLLP source.
     * Release must be safe after any acquire attempt (including failure),
     * and must retain its ownership record on failure. It cannot gate another
     * consumer's clock. Both callbacks are serialized by the parent owner. */
    bool (*acquire_host1x)(void *ctx);
    bool (*release_host1x)(void *ctx);
    bool (*host1x_live)(void *ctx); /* production admission; no provider I/O */
};

enum tg_nvdec_platform_result {
    TG_NVDEC_PLATFORM_OK = 0,
    TG_NVDEC_PLATFORM_ARGUMENT,
    TG_NVDEC_PLATFORM_POWER,
    TG_NVDEC_PLATFORM_CLOCK,
    TG_NVDEC_PLATFORM_RESET,
    TG_NVDEC_PLATFORM_MBIST,
    TG_NVDEC_PLATFORM_TIMEOUT,
    TG_NVDEC_PLATFORM_SUBMIT,
    TG_NVDEC_PLATFORM_OWNERSHIP,
    TG_NVDEC_PLATFORM_UNSUPPORTED,
};

struct tg_nvdec_platform {
    bool captured;
    bool active;
    bool nvdec_touched;
    bool nvdec_clock;
    bool host1x_clock;
    bool host1x_lease;
    bool quiesced;
    bool channel_started;
    struct tg_nvdec_platform_owner owner;
    uint32_t saved_source;
    uint32_t saved_gate_y;
    uint32_t saved_reset_y;
    uint32_t saved_pmc_status;
    uint32_t saved_clamp_status;
    uint32_t saved_mc_flush;
    uint32_t channel;
    uint32_t syncpt;
};

void tg_nvdec_platform_zero(struct tg_nvdec_platform *p);
/* Live read of the two DMA clients used by this backend. Admit only the
 * explicitly untranslated route; do not disable somebody else's SMMU map.
 * This check is not an allocation or evidence of successful device DMA. */
bool tg_nvdec_platform_direct_dma(const struct tg_nvdec_platform_io *io);
/* Fixed408MHz PLLP and inherited Host1x <=204MHz. The existing core owner
 * must retain at least650mV for rates above102MHz;637.5mV covers lower rates.
 * This intersects normal/SLT B01 limits and keeps NVDEC at204MHz. No writes. */
bool tg_nvdec_platform_clocks_supported(uint32_t pllp_base, uint32_t host_source,
                                      uint32_t core_boot_floor_uv);
/* p must be freshly zeroed or successfully shut down. The caller holds the
 * adapter's lifecycle/MC lock and reserves channel/syncpt until shutdown
 * succeeds. A failed init may retain ownership: call shutdown, never zero it.
 * Only the validated B01 inherited-idle state is admitted; cold partition
 * power sequencing requires a separate, serialized PMC owner. */
enum tg_nvdec_platform_result tg_nvdec_platform_init(
    const struct tg_nvdec_platform_io *io, struct tg_nvdec_platform *p,
    uint32_t channel, uint32_t syncpt,
    const struct tg_nvdec_platform_owner *owner);
enum tg_nvdec_platform_result tg_nvdec_platform_shutdown(
    const struct tg_nvdec_platform_io *io, struct tg_nvdec_platform *p);
/* Stop only the owned channel, reset NVDEC and drain its MC client. */
enum tg_nvdec_platform_result tg_nvdec_platform_quiesce(
    const struct tg_nvdec_platform_io *io, struct tg_nvdec_platform *p);

/* Program the channel and submit one gather.  `push` must be a DMA buffer of
 * at least push_size+4 bytes whose bus range is below 4 GiB; `gather_bus` likewise.
 * expected receives the syncpoint target and observed the value read when
 * the wait returned.  The CPU never increments the syncpoint. */
enum tg_nvdec_platform_result tg_nvdec_host1x_submit(
    const struct tg_nvdec_platform_io *io, struct tg_nvdec_platform *p,
    const uint32_t *gather, uint32_t words, uint64_t gather_bus,
    void *push, uint64_t push_bus, uint32_t push_size,
    uint32_t timeout_ms, uint32_t *expected, uint32_t *observed);
enum tg_nvdec_platform_result tg_nvdec_host1x_stop(
    const struct tg_nvdec_platform_io *io, struct tg_nvdec_platform *p);

#endif
