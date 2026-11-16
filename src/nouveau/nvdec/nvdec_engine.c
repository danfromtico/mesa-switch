/* SPDX-License-Identifier: MIT
 * Copyright © 2024 Collabora, Ltd and Red Hat, Inc. (workspace sizing)
 *
 * NVDEC job submission, completion, timeout and cancellation.
 *
 * Every submit uses a DMA-backed Host1x gather buffer.  Completion is the
 * Host1x syncpoint increment written by the NVDEC channel at the end of that
 * gather; there is no CPU-written fence and no software decode fallback.
 */
#include "nvdec.h"

#include <string.h>

#include "nvidia/video/nvdec_drv.h"
#undef ALIGN_UP
#undef NVDEC_ALIGN

#define NVDEC_CMD_BYTES      4096u
#define NVDEC_STATUS_BYTES   4096u
#define NVDEC_ARENA_OVERHEAD (NVDEC_CMD_BYTES + NVDEC_STATUS_BYTES + \
                              64u * 1024u)
#define NVDEC_SUBMIT_TIMEOUT_MS 2000u

static size_t align_up(size_t v, size_t a)
{
    return (v + a - 1u) & ~(a - 1u);
}

static bool arena_reserve(struct tg_nvdec *nvdec, size_t size)
{
    struct tg_nvdec_dma dma;

    if (nvdec->arena.cpu && nvdec->arena.size >= size)
        return true;
    memset(&dma, 0, sizeof(dma));
    if (!nvdec->hal->dma_alloc(nvdec->hal->ctx, size, &dma) ||
        !dma.cpu || dma.size < size)
        return false;
    if (nvdec->arena.cpu)
        nvdec->hal->dma_free(nvdec->hal->ctx, &nvdec->arena);
    nvdec->arena = dma;
    nvdec->arena_used = 0;
    return true;
}

static void *arena_take(struct tg_nvdec *nvdec, size_t bytes, size_t alignment)
{
    size_t offset = align_up(nvdec->arena_used, alignment);

    if (offset > nvdec->arena.size || bytes > nvdec->arena.size - offset)
        return NULL;
    nvdec->arena_used = offset + bytes;
    return (uint8_t *)nvdec->arena.cpu + offset;
}

static uint64_t arena_bus(struct tg_nvdec *nvdec, const void *cpu)
{
    return nvdec->arena.bus + (uint64_t)((const uint8_t *)cpu -
                                        (const uint8_t *)nvdec->arena.cpu);
}

/* Progressive H.264 sizing follows Mesa NVK h264.rs at
 * 684c1b339bddbed6f11af93161da1c1bba77edb1 (MIT). This decoder uses the
 * surface index directly for CurrColIdx/col_idx, including sparse slots,
 * so reserve all 17 indices rather than the current reference count.
 * validate_pic has bounded both nonzero coded dimensions to 4096. */
static void scratch_sizes(const struct tg_nvdec_h264_pic *pic,
                          uint32_t *coloc, uint32_t *history, uint32_t *mbhist)
{
    uint32_t width_mbs = pic->coded_width / 16u;
    uint32_t height_mbs = pic->coded_height / 16u;
    size_t stride = align_up(align_up(height_mbs, 2u) * width_mbs * 64u - 63u,
                             256u);
    *coloc = (uint32_t)stride * TG_NVDEC_MAX_SURFACES;
    *history = (uint32_t)align_up(width_mbs * 0x300u, 512u);
    *mbhist = (uint32_t)align_up(width_mbs * 104u, 256u);
}

static bool scratch_reserve(struct tg_nvdec *nvdec, size_t bytes)
{
    struct tg_nvdec_dma dma = {0};

    if (nvdec->scratch.cpu)
        return nvdec->scratch.size >= bytes;
    if (!nvdec->hal->dma_alloc(nvdec->hal->ctx, bytes, &dma))
        return false;
    if (!dma.cpu || dma.size < bytes) {
        nvdec->hal->dma_free(nvdec->hal->ctx, &dma);
        return false;
    }
    memset(dma.cpu, 0, bytes);
    if (nvdec->hal->dma_sync)
        nvdec->hal->dma_sync(nvdec->hal->ctx, &dma, true);
    nvdec->scratch = dma;
    return true;
}

static enum tg_nvdec_result validate_pic(const struct tg_nvdec_h264_job *job)
{
    const struct tg_nvdec_h264_pic *p = &job->pic;
    uint32_t i, j;

    if (!p->width || !p->height || !p->coded_width || !p->coded_height)
        return TG_NVDEC_ERR_DIMENSIONS;
    if (p->coded_width > 4096u || p->coded_height > 4096u ||
        (p->coded_width & 15u) || (p->coded_height & 15u))
        return TG_NVDEC_ERR_DIMENSIONS;
    if (p->width > p->coded_width || p->height > p->coded_height)
        return TG_NVDEC_ERR_DIMENSIONS;
    if (p->chroma_format_idc != 1u || p->bit_depth_luma != 8u ||
        p->bit_depth_chroma != 8u)
        return TG_NVDEC_ERR_UNSUPPORTED;
    if (p->field_pic_flag)
        return TG_NVDEC_ERR_UNSUPPORTED;   /* field decoding is not enabled */
    if (!p->frame_mbs_only_flag || p->mb_adaptive_frame_field_flag)
        return TG_NVDEC_ERR_UNSUPPORTED;
    if (p->pic_order_cnt_type != 0u && p->pic_order_cnt_type != 2u)
        return TG_NVDEC_ERR_UNSUPPORTED;
    if (p->pitch_luma < p->width || p->pitch_chroma < p->width)
        return TG_NVDEC_ERR_SURFACE;
    if (p->curr_slot >= TG_NVDEC_MAX_SURFACES ||
        !job->surfaces[p->curr_slot].valid)
        return TG_NVDEC_ERR_SURFACE;
    if (p->dpb_count > TG_NVDEC_MAX_DPB)
        return TG_NVDEC_ERR_DPB;

    for (i = 0; i < p->dpb_count; i++) {
        const struct tg_nvdec_h264_ref *r = &p->dpb[i];
        if (r->slot >= TG_NVDEC_MAX_SURFACES ||
            !job->surfaces[r->slot].valid)
            return TG_NVDEC_ERR_DPB;
        if (r->slot == p->curr_slot)
            return TG_NVDEC_ERR_DPB;
    }
    for (i = 0; i < TG_NVDEC_MAX_SURFACES; i++) {
        if (!job->surfaces[i].valid)
            continue;
        if (job->surfaces[i].slot != i)
            return TG_NVDEC_ERR_SURFACE;
        if (!job->surfaces[i].luma_bus || !job->surfaces[i].chroma_bus)
            return TG_NVDEC_ERR_SURFACE;
        for (j = i + 1; j < TG_NVDEC_MAX_SURFACES; j++) {
            if (job->surfaces[j].valid &&
                job->surfaces[j].luma_bus == job->surfaces[i].luma_bus)
                return TG_NVDEC_ERR_SURFACE;
        }
    }
    return TG_NVDEC_OK;
}

static enum tg_nvdec_result validate_slices(const struct tg_nvdec_h264_job *job)
{
    uint32_t i;

    if (!job->bitstream || !job->bitstream_size || !job->slice_offsets ||
        !job->slice_count || job->slice_count > TG_NVDEC_MAX_SLICES)
        return TG_NVDEC_ERR_SLICE;
    if (job->slice_offsets[0] != 0)
        return TG_NVDEC_ERR_SLICE;
    for (i = 0; i < job->slice_count; i++) {
        if (job->slice_offsets[i] >= job->bitstream_size ||
            job->slice_offsets[i + 1] <= job->slice_offsets[i] ||
            job->slice_offsets[i + 1] > job->bitstream_size)
            return TG_NVDEC_ERR_SLICE;
    }
    /* The final offset may equal the end of the slice buffer. */
    if (job->slice_offsets[job->slice_count] > job->bitstream_size)
        return TG_NVDEC_ERR_SLICE;
    return TG_NVDEC_OK;
}

enum tg_nvdec_result tg_nvdec_submit_h264(struct tg_nvdec *nvdec,
                                          const struct tg_nvdec_h264_job *job,
                                          struct tg_nvdec_status *status)
{
    struct tg_nvdec_cmd_buffers bufs;
    void *cmd;
    void *config, *slice_offsets, *status_buf;
    uint8_t *bitstream;
    size_t need, config_size;
    uint32_t coloc_bytes, history_bytes, mbhist_bytes;
    uint32_t words, observed = 0, syncpt_value = 0;
    enum tg_nvdec_result r;

    if (status)
        memset(status, 0, sizeof(*status));
    if (!nvdec || !nvdec->hal || !job)
        return TG_NVDEC_ERR_ARGUMENT;
    if (!nvdec->firmware_loaded)
        return tg_nvdec_set_error(nvdec, TG_NVDEC_ERR_FIRMWARE_MISSING,
                                  "firmware is not loaded");
    if (!nvdec->booted)
        return tg_nvdec_set_error(nvdec, TG_NVDEC_ERR_STATE,
                                  "Falcon is not booted");
    if (nvdec->in_flight)
        return tg_nvdec_set_error(nvdec, TG_NVDEC_ERR_STATE,
                                  "a decode is already in flight");
    if ((r = validate_pic(job)) != TG_NVDEC_OK)
        return tg_nvdec_set_error(nvdec, r, "H.264 picture parameters are invalid");
    if ((r = validate_slices(job)) != TG_NVDEC_OK)
        return tg_nvdec_set_error(nvdec, r, "slice data or offsets are invalid");
    if (nvdec->scratch.cpu &&
        (nvdec->scratch_coded_width != job->pic.coded_width ||
         nvdec->scratch_coded_height != job->pic.coded_height))
        return tg_nvdec_set_error(nvdec, TG_NVDEC_ERR_STATE,
                                  "coded size change requires decoder reset");
    if (job->bitstream_size > 64u * 1024u * 1024u)
        return tg_nvdec_set_error(nvdec, TG_NVDEC_ERR_SLICE,
                                  "bitstream is larger than the 64 MiB limit");

    config_size = tg_nvdec_pic_config_size();
    need = align_up(NVDEC_ARENA_OVERHEAD + job->bitstream_size +
                    (job->slice_count + 1u) * 4u, 4096);
    if (!arena_reserve(nvdec, need))
        return tg_nvdec_set_error(nvdec, TG_NVDEC_ERR_NO_MEMORY,
                                  "cannot allocate the NVDEC command arena");
    scratch_sizes(&job->pic, &coloc_bytes, &history_bytes, &mbhist_bytes);
    if (!scratch_reserve(nvdec, (size_t)coloc_bytes + history_bytes + mbhist_bytes))
        return tg_nvdec_set_error(nvdec, TG_NVDEC_ERR_NO_MEMORY,
                                  "cannot allocate the NVDEC session workspace");
    nvdec->scratch_coded_width = job->pic.coded_width;
    nvdec->scratch_coded_height = job->pic.coded_height;
    nvdec->arena_used = 0;

    cmd = arena_take(nvdec, NVDEC_CMD_BYTES, 256);
    config = arena_take(nvdec, config_size, 256);
    bitstream = arena_take(nvdec, job->bitstream_size, 256);
    slice_offsets = arena_take(nvdec, (job->slice_count + 1u) * 4u, 256);
    status_buf = arena_take(nvdec, NVDEC_STATUS_BYTES, 256);
    if (!cmd || !config || !bitstream || !slice_offsets || !status_buf)
        return tg_nvdec_set_error(nvdec, TG_NVDEC_ERR_NO_MEMORY,
                                  "NVDEC arena layout overflow");

    if (!tg_nvdec_build_pic_config(config, config_size, &job->pic,
                                   (uint32_t)job->bitstream_size,
                                   job->slice_count,
                                   history_bytes / 256u,
                                   mbhist_bytes))
        return tg_nvdec_set_error(nvdec, TG_NVDEC_ERR_ARGUMENT,
                                  "NVIDIA H.264 picture setup rejected the picture");
    memcpy(bitstream, job->bitstream, job->bitstream_size);
    memcpy(slice_offsets, job->slice_offsets, (job->slice_count + 1u) * 4u);
    memset(status_buf, 0, NVDEC_STATUS_BYTES);

    memset(&bufs, 0, sizeof(bufs));
    bufs.config = arena_bus(nvdec, config);
    bufs.bitstream = arena_bus(nvdec, bitstream);
    bufs.slice_offsets = arena_bus(nvdec, slice_offsets);
    bufs.coloc = nvdec->scratch.bus;
    bufs.history = nvdec->scratch.bus + coloc_bytes;
    bufs.mbhist = bufs.history + history_bytes;
    bufs.status = arena_bus(nvdec, status_buf);
    bufs.stream_len = (uint32_t)job->bitstream_size;
    bufs.slice_count = job->slice_count;
    bufs.history_size_256 = history_bytes / 256u;
    bufs.mbhist_size = mbhist_bytes;
    bufs.picture_index = nvdec->picture_index;
    bufs.syncpt_id = nvdec->syncpt_id;
    for (uint32_t i = 0; i < TG_NVDEC_MAX_SURFACES; i++) {
        bufs.surfaces[i].valid = job->surfaces[i].valid;
        bufs.surfaces[i].luma = job->surfaces[i].luma_bus;
        bufs.surfaces[i].chroma = job->surfaces[i].chroma_bus;
    }
    words = (uint32_t)tg_nvdec_build_h264_cmd(cmd, NVDEC_CMD_BYTES / 4u, &bufs);
    if (!words)
        return tg_nvdec_set_error(nvdec, TG_NVDEC_ERR_ARGUMENT,
                                  "NVDEC command buffer rejected the job");

    if (nvdec->hal->dma_sync)
        nvdec->hal->dma_sync(nvdec->hal->ctx, &nvdec->arena, true);
    nvdec->stats.submits++;
    nvdec->in_flight = true;
    nvdec->last_status.mbs_correctly_decoded = 0;
    nvdec->last_status.mbs_in_error = 0;
    nvdec->last_status.error_status = 0;
    nvdec->last_status.slice_header_error_code = 0;
    nvdec->last_status.cycles = 0;

    {
        struct tg_nvdec_dma gather;
        gather.cpu = cmd;
        gather.bus = arena_bus(nvdec, cmd);
        gather.size = NVDEC_CMD_BYTES;
        r = nvdec->hal->submit(nvdec->hal->ctx, &gather, words, nvdec->syncpt_id,
                               NVDEC_SUBMIT_TIMEOUT_MS, &syncpt_value, &observed);
    }
    nvdec->stats.last_syncpoint = syncpt_value;
    nvdec->stats.last_observed = observed;

    if (r == TG_NVDEC_OK && syncpt_value != observed)
        r = TG_NVDEC_ERR_SUBMIT;
    if (r != TG_NVDEC_OK) {
        if (r == TG_NVDEC_ERR_TIMEOUT)
            nvdec->stats.timeouts++;
        else if (r == TG_NVDEC_ERR_CANCELLED)
            nvdec->stats.cancelled++;
        else
            nvdec->stats.failures++;
        (void)tg_nvdec_cancel(nvdec);
        return tg_nvdec_set_error(nvdec, r,
            r == TG_NVDEC_ERR_TIMEOUT ? "NVDEC did not advance its syncpoint in time" :
            r == TG_NVDEC_ERR_CANCELLED ? "NVDEC decode was cancelled" :
                                          "Host1x rejected the NVDEC submission");
    }
    nvdec->in_flight = false;

    if (nvdec->hal->dma_sync)
        nvdec->hal->dma_sync(nvdec->hal->ctx, &nvdec->arena, false);

    {
        const nvdec_status_s *st = status_buf;
        struct tg_nvdec_status out;
        out.mbs_correctly_decoded = st->mbs_correctly_decoded;
        out.mbs_in_error = st->mbs_in_error;
        out.cycles = st->cycle_count;
        out.error_status = st->error_status;
        out.slice_header_error_code = st->slice_header_error_code;
        nvdec->last_status = out;
        if (status)
            *status = out;
        if (out.error_status || out.mbs_in_error || out.slice_header_error_code ||
            out.mbs_correctly_decoded !=
                (job->pic.coded_width / 16u) * (job->pic.coded_height / 16u)) {
            nvdec->stats.failures++;
            nvdec->stats.completions++;
            return tg_nvdec_set_error(nvdec, TG_NVDEC_ERR_ENGINE,
                                      "NVDEC completed with a decode error");
        }
    }

    nvdec->stats.completions++;
    nvdec->picture_index++;
    return TG_NVDEC_OK;
}

enum tg_nvdec_result tg_nvdec_cancel(struct tg_nvdec *nvdec)
{
    if (!nvdec || !nvdec->hal)
        return TG_NVDEC_ERR_NO_HAL;
    if (!nvdec->in_flight && !nvdec->booted)
        return TG_NVDEC_OK;
    if (nvdec->hal->cancel && nvdec->hal->cancel(nvdec->hal->ctx)) {
        if (nvdec->in_flight)
            nvdec->stats.cancelled++;
        nvdec->in_flight = false;
        nvdec->booted = false;
        return TG_NVDEC_OK;
    }
    return tg_nvdec_set_error(nvdec, TG_NVDEC_ERR_SUBMIT,
                              "could not stop the NVDEC channel");
}
