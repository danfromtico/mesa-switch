/* SPDX-License-Identifier: MIT
 *
 * Tegra210 NVDEC platform bring-up and Host1x channel submission.
 *
 * Retained delivery implementation, not connected to the production render
 * driver. NVJPG power dependency, the T210B01 MBIST exception, shared clock
 * leases and channel/SMMU ownership still require integration before use.
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
    uint32_t (*read32)(void *ctx, enum tg_nvdec_aperture ap, uint32_t offset);
    void (*write32)(void *ctx, enum tg_nvdec_aperture ap, uint32_t offset, uint32_t value);
    void (*delay_us)(void *ctx, uint32_t microseconds);
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
};

struct tg_nvdec_platform {
    bool powered;
    bool nvdec_clock;
    bool host1x_clock;
    bool source_saved;
    bool quiesced;
    bool channel_started;
    bool power_transition;
    uint32_t saved_source;
    uint32_t saved_gate_y;
    uint32_t saved_gate_l;
    uint32_t saved_pmc_status;
    uint32_t saved_mc_flush;
    uint32_t channel;
    uint32_t syncpt;
};

void tg_nvdec_platform_zero(struct tg_nvdec_platform *p);
enum tg_nvdec_platform_result tg_nvdec_platform_init(
    const struct tg_nvdec_platform_io *io, struct tg_nvdec_platform *p,
    uint32_t channel, uint32_t syncpt);
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
