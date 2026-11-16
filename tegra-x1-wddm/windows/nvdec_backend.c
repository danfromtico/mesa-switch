/* SPDX-License-Identifier: MIT */
#include "nvdec_backend.h"
#include "../platform/t214_nvdec.h"

#include <ntddk.h>

struct tg_nvdec_win {
    volatile UINT32 *nvdec;
    volatile UINT32 *host1x;
    volatile UINT32 *pmc;
    volatile UINT32 *car;
    volatile UINT32 *mc;
    struct tg_nvdec_platform_io pio;
    struct tg_nvdec_platform platform;
    struct tg_nvdec_hal hal;
    struct tg_nvdec engine;
    struct tg_nvdec_dma push;
    struct tg_nvdec_dma surfaces[TG_NVDEC_MAX_SURFACES];
    UINT32 width, height, pitch, aligned_height, frame_size;
    BOOLEAN surfaces_ready;
    BOOLEAN firmware_ready;
};

static UINT32 win_read32(void *ctx, UINT32 offset)
{
    struct tg_nvdec_win *w = ctx;
    return READ_REGISTER_ULONG((PULONG)w->nvdec + offset / 4u);
}

static void win_write32(void *ctx, UINT32 offset, UINT32 value)
{
    struct tg_nvdec_win *w = ctx;
    WRITE_REGISTER_ULONG((PULONG)w->nvdec + offset / 4u, value);
}

static void win_delay(void *ctx, UINT32 us)
{
    UNREFERENCED_PARAMETER(ctx);
    KeStallExecutionProcessor(us);
}

static bool win_dma_alloc(void *ctx, SIZE_T size, struct tg_nvdec_dma *out)
{
    PHYSICAL_ADDRESS low, high, skip;
    void *p;
    PHYSICAL_ADDRESS pa;

    UNREFERENCED_PARAMETER(ctx);
    low.QuadPart = 0;
    high.QuadPart = 0xffffffffull;   /* Current encoder admits only low32-bit addresses. */
    skip.QuadPart = 0;
    p = MmAllocateContiguousMemorySpecifyCache(size, low, high, skip, MmNonCached);
    if (!p)
        return false;
    pa = MmGetPhysicalAddress(p);
    if (pa.QuadPart <= 0 || (ULONGLONG)pa.QuadPart > 0xffffffffull ||
        size > 0x100000000ull - (ULONGLONG)pa.QuadPart) {
        MmFreeContiguousMemory(p);
        return false;
    }
    RtlZeroMemory(p, size);
    out->cpu = p;
    out->bus = pa.QuadPart;
    out->size = size;
    return true;
}

static void win_dma_free(void *ctx, struct tg_nvdec_dma *dma)
{
    UNREFERENCED_PARAMETER(ctx);
    if (dma->cpu)
        MmFreeContiguousMemory(dma->cpu);
    RtlZeroMemory(dma, sizeof(*dma));
}

static UINT32 pio_read32(void *ctx, enum tg_nvdec_aperture ap, UINT32 offset)
{
    struct tg_nvdec_win *w = ctx;
    switch (ap) {
    case TG_NVDEC_AP_NVDEC: return READ_REGISTER_ULONG((PULONG)w->nvdec + offset / 4u);
    case TG_NVDEC_AP_HOST1X: return READ_REGISTER_ULONG((PULONG)w->host1x + offset / 4u);
    case TG_NVDEC_AP_PMC: return READ_REGISTER_ULONG((PULONG)w->pmc + offset / 4u);
    case TG_NVDEC_AP_CAR: return READ_REGISTER_ULONG((PULONG)w->car + offset / 4u);
    case TG_NVDEC_AP_MC: return READ_REGISTER_ULONG((PULONG)w->mc + offset / 4u);
    }
    return 0;
}

static void pio_write32(void *ctx, enum tg_nvdec_aperture ap, UINT32 offset,
                        UINT32 value)
{
    struct tg_nvdec_win *w = ctx;
    switch (ap) {
    case TG_NVDEC_AP_NVDEC: WRITE_REGISTER_ULONG((PULONG)w->nvdec + offset / 4u, value); break;
    case TG_NVDEC_AP_HOST1X: WRITE_REGISTER_ULONG((PULONG)w->host1x + offset / 4u, value); break;
    case TG_NVDEC_AP_PMC: WRITE_REGISTER_ULONG((PULONG)w->pmc + offset / 4u, value); break;
    case TG_NVDEC_AP_CAR: WRITE_REGISTER_ULONG((PULONG)w->car + offset / 4u, value); break;
    case TG_NVDEC_AP_MC: WRITE_REGISTER_ULONG((PULONG)w->mc + offset / 4u, value); break;
    }
}

static void pio_delay(void *ctx, UINT32 us)
{
    UNREFERENCED_PARAMETER(ctx);
    KeStallExecutionProcessor(us);
}

static enum tg_nvdec_result win_submit(void *ctx, const struct tg_nvdec_dma *gather,
                                       UINT32 words, UINT32 syncpt_id,
                                       UINT32 timeout_ms, UINT32 *expected,
                                       UINT32 *observed)
{
    struct tg_nvdec_win *w = ctx;
    enum tg_nvdec_platform_result r;

    r = tg_nvdec_host1x_submit(&w->pio, &w->platform, gather->cpu, words,
                               gather->bus, w->push.cpu, w->push.bus,
                               64u * 1024u, timeout_ms,
                               expected, observed);
    switch (r) {
    case TG_NVDEC_PLATFORM_OK: return TG_NVDEC_OK;
    case TG_NVDEC_PLATFORM_TIMEOUT: return TG_NVDEC_ERR_TIMEOUT;
    default: return TG_NVDEC_ERR_SUBMIT;
    }
}

static bool win_cancel(void *ctx)
{
    struct tg_nvdec_win *w = ctx;
    return tg_nvdec_host1x_stop(&w->pio, &w->platform) == TG_NVDEC_PLATFORM_OK;
}

static void release_surfaces(struct tg_nvdec_win *w)
{
    UINT32 i;
    for (i = 0; i < TG_NVDEC_MAX_SURFACES; i++)
        win_dma_free(w, &w->surfaces[i]);
    w->surfaces_ready = FALSE;
}

NTSTATUS TgNvdecReleaseSurfaces(struct tg_nvdec_win *w)
{
    if (!w) return STATUS_INVALID_PARAMETER;
    if (w->engine.in_flight) return STATUS_DEVICE_BUSY;
    release_surfaces(w);
    return STATUS_SUCCESS;
}

NTSTATUS TgNvdecCreate(volatile UINT32 *nvdec_regs,
                                   volatile UINT32 *host1x_regs,
                                   volatile UINT32 *pmc_regs,
                                   volatile UINT32 *car_regs,
                                   volatile UINT32 *mc_regs,
                                   struct tg_nvdec_win **result)
{
    struct tg_nvdec_win *w;

    if (!result || *result || !nvdec_regs || !host1x_regs ||
        !pmc_regs || !car_regs || !mc_regs)
        return STATUS_INVALID_PARAMETER;
    w = ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*w), 'cvdN');
    if (!w)
        return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(w, sizeof(*w));
    w->nvdec = nvdec_regs;
    w->host1x = host1x_regs;
    w->pmc = pmc_regs;
    w->car = car_regs;
    w->mc = mc_regs;
    *result = w;
    w->pio.ctx = w;
    w->pio.read32 = pio_read32;
    w->pio.write32 = pio_write32;
    w->pio.delay_us = pio_delay;
    w->hal.ctx = w;
    w->hal.read32 = win_read32;
    w->hal.write32 = win_write32;
    w->hal.delay_us = win_delay;
    w->hal.dma_alloc = win_dma_alloc;
    w->hal.dma_free = win_dma_free;
    w->hal.submit = win_submit;
    w->hal.cancel = win_cancel;
    tg_nvdec_init(&w->engine, &w->hal, 31);

    if (tg_nvdec_platform_init(&w->pio, &w->platform, 11, 31) !=
        TG_NVDEC_PLATFORM_OK)
        goto fail;
    if (!win_dma_alloc(w, 64u * 1024u + 16u, &w->push))
        goto fail_platform;
    return STATUS_SUCCESS;

fail_platform:
fail:
    if (!NT_SUCCESS(TgNvdecDestroy(w)))
        return STATUS_DEVICE_BUSY; /* caller retains object and MMIO ownership */
    *result = NULL;
    return STATUS_DEVICE_NOT_READY;
}

NTSTATUS TgNvdecDestroy(struct tg_nvdec_win *w)
{
    if (!w)
        return STATUS_SUCCESS;
    if (tg_nvdec_close(&w->engine) != TG_NVDEC_OK)
        return STATUS_DEVICE_BUSY;
    if (tg_nvdec_platform_shutdown(&w->pio, &w->platform) != TG_NVDEC_PLATFORM_OK)
        return STATUS_DEVICE_BUSY;
    release_surfaces(w);
    win_dma_free(w, &w->push);
    ExFreePoolWithTag(w, 'cvdN');
    return STATUS_SUCCESS;
}

static NTSTATUS map_result(enum tg_nvdec_result r)
{
    switch (r) {
    case TG_NVDEC_OK: return STATUS_SUCCESS;
    case TG_NVDEC_ERR_NO_MEMORY: return STATUS_INSUFFICIENT_RESOURCES;
    case TG_NVDEC_ERR_TIMEOUT: return STATUS_IO_TIMEOUT;
    case TG_NVDEC_ERR_UNSUPPORTED: return STATUS_NOT_SUPPORTED;
    case TG_NVDEC_ERR_ARGUMENT:
    case TG_NVDEC_ERR_DIMENSIONS:
    case TG_NVDEC_ERR_SURFACE:
    case TG_NVDEC_ERR_SLICE:
    case TG_NVDEC_ERR_DPB:
    case TG_NVDEC_ERR_STATE:
    case TG_NVDEC_ERR_FIRMWARE_HASH:
        return STATUS_INVALID_PARAMETER;
    case TG_NVDEC_ERR_NO_HAL: return STATUS_DEVICE_NOT_READY;
    case TG_NVDEC_ERR_CANCELLED: return STATUS_CANCELLED;
    default: return STATUS_DEVICE_DATA_ERROR;
    }
}

BOOLEAN TgNvdecFirmwareValid(const void *data, SIZE_T size,
                             const UINT8 expected_sha256[TG_NVDEC_SHA256_SIZE])
{
    static const UINT8 pinned[TG_NVDEC_SHA256_SIZE] = {
        0x7b,0x5a,0xc5,0xad,0x66,0xda,0xd4,0x7e,
        0x77,0xf2,0xa0,0x56,0x99,0x1e,0x42,0x2d,
        0xd2,0x63,0x0f,0xa1,0x7e,0x0a,0x57,0xa5,
        0x87,0x4a,0xf4,0x92,0xef,0xa0,0xf9,0x09,
    };
    UINT8 digest[TG_NVDEC_SHA256_SIZE];
    if (!data || size != 128000u || !expected_sha256 ||
        RtlCompareMemory(expected_sha256, pinned, sizeof(pinned)) != sizeof(pinned))
        return FALSE;
    tg_nvdec_sha256(data, size, digest);
    return RtlCompareMemory(digest, pinned, sizeof(pinned)) == sizeof(pinned);
}

NTSTATUS TgNvdecLoadFirmware(struct tg_nvdec_win *w, const void *data,
                             SIZE_T size,
                             const UINT8 expected_sha256[TG_NVDEC_SHA256_SIZE])
{
    enum tg_nvdec_result r;

    if (!w || !TgNvdecFirmwareValid(data, size, expected_sha256))
        return STATUS_INVALID_PARAMETER;
    w->firmware_ready = FALSE;
    if (tg_nvdec_close(&w->engine) != TG_NVDEC_OK)
        return STATUS_DEVICE_BUSY;
    if (w->platform.quiesced &&
        tg_nvdec_platform_shutdown(&w->pio, &w->platform) != TG_NVDEC_PLATFORM_OK)
        return STATUS_DEVICE_BUSY;
    if (!w->platform.powered &&
        tg_nvdec_platform_init(&w->pio, &w->platform, 11, 31) != TG_NVDEC_PLATFORM_OK)
        return STATUS_DEVICE_NOT_READY;
    if (!w->push.cpu && !win_dma_alloc(w, 64u * 1024u + 16u, &w->push))
        return STATUS_INSUFFICIENT_RESOURCES;
    tg_nvdec_init(&w->engine, &w->hal, 31);
    r = tg_nvdec_load_firmware(&w->engine, data, size, expected_sha256);
    if (r != TG_NVDEC_OK)
        return map_result(r);
    r = tg_nvdec_boot(&w->engine);
    if (r != TG_NVDEC_OK)
        return map_result(r);
    w->firmware_ready = TRUE;
    return STATUS_SUCCESS;
}

NTSTATUS TgNvdecConfigure(struct tg_nvdec_win *w, UINT32 width, UINT32 height,
                          UINT32 *pitch, UINT32 *aligned_height,
                          UINT32 *frame_size)
{
    UINT32 i, p, ah;
    SIZE_T bytes;

    if (!w || !pitch || !aligned_height || !frame_size)
        return STATUS_INVALID_PARAMETER;
    if (w->engine.in_flight) return STATUS_DEVICE_BUSY;
    if (!TgNvdecFirmwareReady(w)) return STATUS_DEVICE_NOT_READY;
    /* Stage 1 is deliberately limited to 720p 8-bit 4:2:0. */
    if (!tg_nvdec_stage1_supported(width, height, width, height, 1, 8))
        return STATUS_NOT_SUPPORTED;
    p = (width + 255u) & ~255u;
    ah = (height + 15u) & ~15u;
    if (p < width || ah < height)
        return STATUS_INVALID_PARAMETER;
    bytes = (SIZE_T)p * ah * 3u / 2u;
    if (!bytes)
        return STATUS_INVALID_PARAMETER;

    release_surfaces(w);
    for (i = 0; i < TG_NVDEC_MAX_SURFACES; i++) {
        if (!win_dma_alloc(w, bytes, &w->surfaces[i])) {
            release_surfaces(w);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }
    w->width = width;
    w->height = height;
    w->pitch = p;
    w->aligned_height = ah;
    w->frame_size = (UINT32)bytes;
    w->surfaces_ready = TRUE;
    *pitch = p;
    *aligned_height = ah;
    *frame_size = (UINT32)bytes;
    return STATUS_SUCCESS;
}

NTSTATUS TgNvdecDecode(struct tg_nvdec_win *w,
                       const struct tg_nvdec_h264_pic *pic,
                       const UINT8 *bitstream, UINT32 bitstream_size,
                       const UINT32 *slice_offsets, UINT32 slice_count,
                       void *output, SIZE_T output_size,
                       struct tg_nvdec_status *status,
                       UINT32 *expected, UINT32 *observed)
{
    struct tg_nvdec_h264_job job;
    struct tg_nvdec_status st = {0};
    enum tg_nvdec_result r;
    UINT32 i, slot;

    if (!w || !pic || !bitstream || !slice_offsets || !slice_count ||
        !output || !status || !expected || !observed)
        return STATUS_INVALID_PARAMETER;
    if (!w->surfaces_ready || !w->firmware_ready)
        return STATUS_DEVICE_NOT_READY;
    if (output_size < w->frame_size)
        return STATUS_BUFFER_TOO_SMALL;
    if (pic->width > w->width || pic->height > w->height ||
        pic->coded_width > w->pitch || pic->coded_height > w->aligned_height)
        return STATUS_NOT_SUPPORTED;
    slot = pic->curr_slot;
    if (slot >= TG_NVDEC_MAX_SURFACES)
        return STATUS_INVALID_PARAMETER;

    RtlZeroMemory(&job, sizeof(job));
    job.pic = *pic;
    job.pic.pitch_luma = w->pitch;
    job.pic.pitch_chroma = w->pitch;
    job.bitstream = bitstream;
    job.bitstream_size = bitstream_size;
    job.slice_offsets = slice_offsets;
    job.slice_count = slice_count;
    for (i = 0; i < TG_NVDEC_MAX_SURFACES; i++) {
        job.surfaces[i].valid = TRUE;
        job.surfaces[i].slot = i;
        job.surfaces[i].luma_bus = w->surfaces[i].bus;
        job.surfaces[i].chroma_bus = w->surfaces[i].bus +
                                     (UINT64)w->pitch * w->aligned_height;
    }

    r = tg_nvdec_submit_h264(&w->engine, &job, &st);
    *status = st;
    *expected = w->engine.stats.last_syncpoint;
    *observed = w->engine.stats.last_observed;
    if (r != TG_NVDEC_OK)
        return map_result(r);
    RtlCopyMemory(output, w->surfaces[slot].cpu, w->frame_size);
    return STATUS_SUCCESS;
}

NTSTATUS TgNvdecReset(struct tg_nvdec_win *w)
{
    if (!w)
        return STATUS_INVALID_PARAMETER;
    w->firmware_ready = FALSE;
    if (tg_nvdec_close(&w->engine) != TG_NVDEC_OK ||
        tg_nvdec_platform_shutdown(&w->pio, &w->platform) != TG_NVDEC_PLATFORM_OK)
        return STATUS_DEVICE_BUSY;
    tg_nvdec_init(&w->engine, &w->hal, 31);
    release_surfaces(w);
    w->width = w->height = w->pitch = w->aligned_height = w->frame_size = 0;
    return STATUS_SUCCESS;
}

const char *TgNvdecLastError(struct tg_nvdec_win *w)
{
    return w ? w->engine.error : "NVDEC backend is absent";
}

BOOLEAN TgNvdecFirmwareReady(struct tg_nvdec_win *w)
{
    return w && w->firmware_ready && w->engine.booted;
}

BOOLEAN TgNvdecSurfacesReady(struct tg_nvdec_win *w)
{
    return w && w->surfaces_ready;
}

void TgNvdecGetConfig(struct tg_nvdec_win *w, UINT32 *width, UINT32 *height,
                      UINT32 *pitch, UINT32 *aligned_height, UINT32 *frame_size)
{
    if (!w)
        return;
    if (width) *width = w->width;
    if (height) *height = w->height;
    if (pitch) *pitch = w->pitch;
    if (aligned_height) *aligned_height = w->aligned_height;
    if (frame_size) *frame_size = w->frame_size;
}
