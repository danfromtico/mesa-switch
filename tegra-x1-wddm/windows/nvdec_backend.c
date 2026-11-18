/* SPDX-License-Identifier: MIT */
#include "nvdec_backend.h"
#include "../platform/t214_nvdec.h"

#include <ntddk.h>

struct tg_nvdec_win {
    DXGKRNL_INTERFACE dxgk;
    TG_NVDEC_MEMORY memory;
    struct tg_nvdec_platform_io pio;
    struct tg_nvdec_platform_owner owner;
    UINT32 channel, syncpt;
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
    return w->pio.read32(w->pio.ctx, TG_NVDEC_AP_NVDEC, offset);
}

static void win_write32(void *ctx, UINT32 offset, UINT32 value)
{
    struct tg_nvdec_win *w = ctx;
    w->pio.write32(w->pio.ctx, TG_NVDEC_AP_NVDEC, offset, value);
}

static void win_delay(void *ctx, UINT32 us)
{
    UNREFERENCED_PARAMETER(ctx);
    KeStallExecutionProcessor(us);
}

static bool win_dma_alloc(void *ctx, SIZE_T size, struct tg_nvdec_dma *out)
{
    struct tg_nvdec_win *w = ctx;
    return TgNvdecMemoryAllocate(&w->memory, size, out) != FALSE;
}

static void win_dma_free(void *ctx, struct tg_nvdec_dma *dma)
{
    struct tg_nvdec_win *w = ctx;
    TgNvdecMemoryRetire(&w->memory, dma);
}

static void win_dma_sync(void *ctx, const struct tg_nvdec_dma *dma, bool to_device)
{
    UNREFERENCED_PARAMETER(ctx);
    UNREFERENCED_PARAMETER(dma);
    UNREFERENCED_PARAMETER(to_device);
    /* Private allocations use an uncached CPU mapping. Order CPU writes
     * before publication and device output reads after verified completion. */
    KeMemoryBarrier();
}

static enum tg_nvdec_result win_submit(void *ctx, const struct tg_nvdec_dma *gather,
                                       UINT32 words, UINT32 syncpt_id,
                                       UINT32 timeout_ms, UINT32 *expected,
                                       UINT32 *observed)
{
    struct tg_nvdec_win *w = ctx;
    enum tg_nvdec_platform_result r;

    if (syncpt_id != w->syncpt || !w->owner.host1x_live(w->owner.ctx) ||
        !tg_nvdec_platform_direct_dma(&w->pio))
        return TG_NVDEC_ERR_ARGUMENT;
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

static NTSTATUS release_surfaces(struct tg_nvdec_win *w)
{
    UINT32 i;
    for (i = 0; i < TG_NVDEC_MAX_SURFACES; i++)
        win_dma_free(w, &w->surfaces[i]);
    w->surfaces_ready = FALSE;
    return TgNvdecMemoryRetryRetired(&w->memory);
}

NTSTATUS TgNvdecReleaseSurfaces(struct tg_nvdec_win *w)
{
    if (!w) return STATUS_INVALID_PARAMETER;
    if (w->engine.in_flight) return STATUS_DEVICE_BUSY;
    return release_surfaces(w);
}

NTSTATUS TgNvdecCreate(const DXGKRNL_INTERFACE *dxgk,
                       const struct tg_nvdec_platform_io *io,
                       const struct tg_nvdec_platform_owner *owner,
                       UINT32 channel, UINT32 syncpt,
                       struct tg_nvdec_win **result)
{
    struct tg_nvdec_win *w;
    NTSTATUS status;

    if (!dxgk || !result || *result || !io || !io->read32 || !io->write32 || !io->delay_us ||
        !owner || !owner->acquire_host1x || !owner->release_host1x || !owner->host1x_live ||
        channel >= 12 || syncpt >= 192)
        return STATUS_INVALID_PARAMETER;
    if (!tg_nvdec_platform_direct_dma(io)) return STATUS_NOT_SUPPORTED;
    w = ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*w), 'cvdN');
    if (!w)
        return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(w, sizeof(*w));
    w->dxgk = *dxgk;
    status = TgNvdecMemoryInit(&w->memory, &w->dxgk);
    if (!NT_SUCCESS(status)) { ExFreePoolWithTag(w, 'cvdN'); return status; }
    w->pio = *io;
    w->owner = *owner;
    w->channel = channel;
    w->syncpt = syncpt;
    *result = w;
    w->hal.ctx = w;
    w->hal.read32 = win_read32;
    w->hal.write32 = win_write32;
    w->hal.delay_us = win_delay;
    w->hal.dma_alloc = win_dma_alloc;
    w->hal.dma_free = win_dma_free;
    w->hal.dma_sync = win_dma_sync;
    w->hal.submit = win_submit;
    w->hal.cancel = win_cancel;
    tg_nvdec_init(&w->engine, &w->hal, w->syncpt);

    if (tg_nvdec_platform_init(&w->pio, &w->platform, w->channel, w->syncpt, &w->owner) !=
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

NTSTATUS TgNvdecQuiesce(struct tg_nvdec_win *w)
{
    if (!w) return STATUS_SUCCESS;
    /* Also covers initialized-but-not-booted hardware, which the codec's
     * in_flight/booted flags alone do not prove quiescent. */
    if (tg_nvdec_platform_quiesce(&w->pio, &w->platform) != TG_NVDEC_PLATFORM_OK)
        return STATUS_DEVICE_BUSY;
    if (tg_nvdec_cancel(&w->engine) != TG_NVDEC_OK) return STATUS_DEVICE_BUSY;
    w->firmware_ready = FALSE;
    return STATUS_SUCCESS;
}

NTSTATUS TgNvdecDestroy(struct tg_nvdec_win *w)
{
    if (!w)
        return STATUS_SUCCESS;
    if (tg_nvdec_close(&w->engine) != TG_NVDEC_OK)
        return STATUS_DEVICE_BUSY;
    if (tg_nvdec_platform_shutdown(&w->pio, &w->platform) != TG_NVDEC_PLATFORM_OK)
        return STATUS_DEVICE_BUSY;
    (VOID)release_surfaces(w);
    win_dma_free(w, &w->push);
    if (!NT_SUCCESS(TgNvdecMemoryFinish(&w->memory)))
        return STATUS_DEVICE_BUSY;
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
    if (!w->owner.host1x_live(w->owner.ctx)) return STATUS_DEVICE_NOT_READY;
    w->firmware_ready = FALSE;
    if (tg_nvdec_close(&w->engine) != TG_NVDEC_OK)
        return STATUS_DEVICE_BUSY;
    if (w->platform.quiesced &&
        tg_nvdec_platform_shutdown(&w->pio, &w->platform) != TG_NVDEC_PLATFORM_OK)
        return STATUS_DEVICE_BUSY;
    if (!tg_nvdec_platform_direct_dma(&w->pio)) return STATUS_NOT_SUPPORTED;
    if (!w->platform.active &&
        tg_nvdec_platform_init(&w->pio, &w->platform, w->channel, w->syncpt, &w->owner) != TG_NVDEC_PLATFORM_OK)
        return STATUS_DEVICE_NOT_READY;
    if (!w->push.cpu && !win_dma_alloc(w, 64u * 1024u + 16u, &w->push))
        return STATUS_INSUFFICIENT_RESOURCES;
    tg_nvdec_init(&w->engine, &w->hal, w->syncpt);
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
    struct tg_nvdec_nv12_storage layout;

    if (!w || !pitch || !aligned_height || !frame_size)
        return STATUS_INVALID_PARAMETER;
    if (w->engine.in_flight) return STATUS_DEVICE_BUSY;
    if (!TgNvdecFirmwareReady(w)) return STATUS_DEVICE_NOT_READY;
    p = (width + 255u) & ~255u;
    ah = (height + 15u) & ~15u;
    if (p < width || ah < height)
        return STATUS_INVALID_PARAMETER;
    if (!tg_nvdec_stage1_supported(width, height, width, ah, 1, 8))
        return STATUS_NOT_SUPPORTED;
    if (!tg_nvdec_nv12_storage_layout(p, ah, &layout) ||
        layout.linear_bytes > MAXULONG)
        return STATUS_INVALID_PARAMETER;

    NTSTATUS released = release_surfaces(w);
    if (!NT_SUCCESS(released)) return released;
    for (i = 0; i < TG_NVDEC_MAX_SURFACES; i++) {
        if (!win_dma_alloc(w, layout.tiled_bytes, &w->surfaces[i])) {
            (VOID)release_surfaces(w);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }
    w->width = width;
    w->height = height;
    w->pitch = p;
    w->aligned_height = ah;
    w->frame_size = (UINT32)layout.linear_bytes;
    w->surfaces_ready = TRUE;
    *pitch = p;
    *aligned_height = ah;
    *frame_size = w->frame_size;
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
    if (!tg_nvdec_stage1_supported(pic->width, pic->height, pic->coded_width,
                                   pic->coded_height, pic->chroma_format_idc, pic->bit_depth_luma) ||
        pic->width > w->width || pic->height > w->height ||
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
    /* The engine wrote TBL, not pitch-linear NV12. Completion was verified
     * above; retain the surface and clock lease throughout the readback. */
    if (!tg_nvdec_nv12_readback(output, output_size, w->surfaces[slot].cpu,
                               w->surfaces[slot].size, w->pitch, w->aligned_height)) {
        (VOID)tg_nvdec_set_error(&w->engine, TG_NVDEC_ERR_SURFACE,
                                 "NV12 block-linear readback bounds are invalid");
        return STATUS_INVALID_PARAMETER;
    }
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
    tg_nvdec_init(&w->engine, &w->hal, w->syncpt);
    NTSTATUS released = release_surfaces(w);
    if (!NT_SUCCESS(released)) return released;
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
