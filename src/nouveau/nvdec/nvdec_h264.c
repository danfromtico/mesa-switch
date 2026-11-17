/* SPDX-License-Identifier: MIT
 *
 * NVDEC (class C5B0) H.264 picture setup and Host1x method stream.
 *
 * The method sequence follows NVIDIA's vaapi-tegra-driver (MIT): application
 * id, control params, picture setup, input bitstream, slice offsets, the 17
 * picture surfaces, colocated/history/MB-history buffers, status buffer,
 * picture index and EXECUTE, followed by an INCR_SYNCPT in the same channel
 * class.  Addresses are never patched by the CPU after submission.
 */
#include "nvdec.h"

#include <string.h>

/* NVIDIA's structure definitions are MIT licensed and self-contained. */
#include "nvidia/video/nvdec_drv.h"
#undef ALIGN_UP
#undef NVDEC_ALIGN

#define NVC5B0_SET_APPLICATION_ID          0x00000200u
#define NVC5B0_SET_APPLICATION_ID_H264     0x00000003u
#define NVC5B0_SET_CONTROL_PARAMS          0x00000400u
#define NVC5B0_SET_CONTROL_PARAMS_H264     0x00000003u
#define NVC5B0_SET_CONTROL_PARAMS_GPTIMER  (1u << 4)
#define NVC5B0_SET_CONTROL_PARAMS_ERRCON   (1u << 6)
#define NVC5B0_SET_DRV_PIC_SETUP_OFFSET    0x00000404u
#define NVC5B0_SET_IN_BUF_BASE_OFFSET      0x00000408u
#define NVC5B0_SET_PICTURE_INDEX           0x0000040cu
#define NVC5B0_SET_SLICE_OFFSETS_BUF_OFFSET 0x00000410u
#define NVC5B0_SET_COLOC_DATA_OFFSET       0x00000414u
#define NVC5B0_SET_HISTORY_OFFSET          0x00000418u
#define NVC5B0_SET_NVDEC_STATUS_OFFSET     0x00000424u
#define NVC5B0_SET_PICTURE_LUMA_OFFSET0    0x00000430u
#define NVC5B0_SET_PICTURE_CHROMA_OFFSET0  0x00000474u
#define NVC5B0_H264_SET_MBHIST_BUF_OFFSET  0x00000500u
#define NVC5B0_EXECUTE                     0x00000300u
#define NVC5B0_EXECUTE_AWAKEN              (1u << 8)

#define NVDEC_UCLASS_METHOD_OFFSET         0x10u
#define NVDEC_UCLASS_METHOD_DATA           0x11u
#define NVDEC_UCLASS_INCR_SYNCPT           0x00u

static uint32_t host1x_incr(unsigned offset, unsigned count)
{
    return (1u << 28) | (offset << 16) | count;
}

static uint32_t host1x_nonincr(unsigned offset, unsigned count)
{
    return (2u << 28) | (offset << 16) | count;
}

static bool method_address_valid(uint64_t address)
{
    /* C5B0 OFFSET methods encode addresses in 256-byte units. */
    return address && !(address & 255u) && (address >> 8) <= UINT32_MAX;
}

size_t tg_nvdec_pic_config_size(void)
{
    return sizeof(nvdec_h264_pic_s);
}

size_t tg_nvdec_build_pic_config(void *out, size_t bytes,
                                 const struct tg_nvdec_h264_pic *pic,
                                 uint32_t stream_len, uint32_t slice_count,
                                 uint32_t history_size_256, uint32_t mbhist_size)
{
    nvdec_h264_pic_s *c = out;
    uint32_t i;

    if (!out || !pic || bytes < sizeof(*c))
        return 0;
    if (!pic->coded_width || !pic->coded_height ||
        pic->coded_width > 4096u || pic->coded_height > 4096u ||
        pic->chroma_format_idc != 1u ||
        pic->bit_depth_luma != 8u || pic->bit_depth_chroma != 8u)
        return 0;
    if (pic->dpb_count > TG_NVDEC_MAX_DPB)
        return 0;

    memset(c, 0, sizeof(*c));
    c->stream_len = stream_len;
    c->slice_count = slice_count;
    c->mbhist_buffer_size = mbhist_size;
    c->log2_max_pic_order_cnt_lsb_minus4 = (int)pic->log2_max_pic_order_cnt_lsb_minus4;
    c->delta_pic_order_always_zero_flag = pic->delta_pic_order_always_zero_flag;
    c->frame_mbs_only_flag = pic->frame_mbs_only_flag;
    c->PicWidthInMbs = (int)(pic->coded_width / 16u);
    c->FrameHeightInMbs = (int)(pic->coded_height / 16u);
    c->tileFormat = 0;
    c->gob_height = 0;
    c->entropy_coding_mode_flag = pic->entropy_coding_mode_flag;
    c->pic_order_present_flag = pic->pic_order_present_flag;
    c->num_ref_idx_l0_active_minus1 = (int)pic->num_ref_idx_l0_active_minus1;
    c->num_ref_idx_l1_active_minus1 = (int)pic->num_ref_idx_l1_active_minus1;
    c->deblocking_filter_control_present_flag = pic->deblocking_filter_control_present_flag;
    c->redundant_pic_cnt_present_flag = pic->redundant_pic_cnt_present_flag;
    c->transform_8x8_mode_flag = pic->transform_8x8_mode_flag;
    c->pitch_luma = pic->pitch_luma;
    c->pitch_chroma = pic->pitch_chroma;
    c->luma_top_offset = 0;
    c->luma_bot_offset = 0;
    c->luma_frame_offset = 0;
    c->chroma_top_offset = 0;
    c->chroma_bot_offset = 0;
    c->chroma_frame_offset = 0;
    c->HistBufferSize = history_size_256;
    c->MbaffFrameFlag = pic->mb_adaptive_frame_field_flag;
    c->direct_8x8_inference_flag = pic->direct_8x8_inference_flag;
    c->weighted_pred_flag = pic->weighted_pred_flag;
    c->constrained_intra_pred_flag = pic->constrained_intra_pred_flag;
    c->ref_pic_flag = pic->ref_pic_flag;
    c->field_pic_flag = pic->field_pic_flag;
    c->bottom_field_flag = pic->bottom_field_flag;
    c->log2_max_frame_num_minus4 = pic->log2_max_frame_num_minus4 & 0xfu;
    c->chroma_format_idc = pic->chroma_format_idc & 0x3u;
    c->pic_order_cnt_type = pic->pic_order_cnt_type & 0x3u;
    c->pic_init_qp_minus26 = pic->pic_init_qp_minus26;
    c->chroma_qp_index_offset = pic->chroma_qp_index_offset;
    c->second_chroma_qp_index_offset = pic->second_chroma_qp_index_offset;
    c->weighted_bipred_idc = pic->weighted_bipred_idc & 0x3u;
    if (pic->curr_slot >= TG_NVDEC_MAX_SURFACES)
        return 0;
    c->CurrPicIdx = pic->curr_slot;
    c->CurrColIdx = pic->curr_slot;
    c->frame_num = pic->frame_num;
    c->frame_surfaces = 0;
    c->output_memory_layout = 0;
    c->CurrFieldOrderCnt[0] = pic->curr_top_poc;
    c->CurrFieldOrderCnt[1] = pic->curr_bottom_poc;
    c->lossless_ipred8x8_filter_enable = pic->lossless_ipred8x8_filter_enable;
    c->qpprime_y_zero_transform_bypass_flag = pic->qpprime_y_zero_transform_bypass_flag;
    memcpy(c->WeightScale, pic->scaling4x4, sizeof(pic->scaling4x4));
    memcpy(c->WeightScale8x8, pic->scaling8x8, sizeof(pic->scaling8x8));

    for (i = 0; i < pic->dpb_count; i++) {
        const struct tg_nvdec_h264_ref *r = &pic->dpb[i];
        nvdec_dpb_entry_s *d = &c->dpb[i];

        if (r->slot >= TG_NVDEC_MAX_SURFACES)
            return 0;
        d->index = r->slot;
        d->col_idx = r->slot;
        d->state = 3;   /* frame picture: top and bottom both used */
        d->is_long_term = r->is_long_term;
        d->not_existing = r->not_existing;
        d->is_field = r->is_field;
        d->top_field_marking = r->is_long_term ? 2 : 1;
        d->bottom_field_marking = r->is_long_term ? 2 : 1;
        d->output_memory_layout = 0;
        d->FieldOrderCnt[0] = (uint32_t)r->top_poc;
        d->FieldOrderCnt[1] = (uint32_t)r->bottom_poc;
        d->FrameIdx = (int)r->frame_num;
    }
    return sizeof(*c);
}

bool tg_nvdec_nv12_storage_layout(uint32_t pitch, uint32_t height,
                                 struct tg_nvdec_nv12_storage *layout)
{
    if (!layout || !pitch || (pitch & 63u) || !height || (height & 15u))
        return false;
    const uint64_t luma = (uint64_t)pitch * height;
    const uint64_t chroma = (uint64_t)pitch * ((height / 2u + 15u) & ~15u);
    if (chroma > SIZE_MAX || luma > SIZE_MAX - chroma)
        return false;
    *layout = (struct tg_nvdec_nv12_storage){
        (size_t)luma, (size_t)(luma + (uint64_t)pitch * (height / 2u)),
        (size_t)(luma + chroma)
    };
    return true;
}

static void readback_tbl_plane(uint8_t *dst, const uint8_t *src,
                               uint32_t pitch, uint32_t rows)
{
    /* TegraColor GOB byte ordering is also documented by NIL's tiling.rs
     * and CopyGOBTegra (MIT). A tile is64 bytes wide and16 rows high. Copy
     * its16-byte atoms; no alignment assumption is made for the destination. */
    for (uint32_t tile_y = 0; tile_y < rows; tile_y += 16u) {
        const uint32_t count = rows - tile_y < 16u ? rows - tile_y : 16u;
        for (uint32_t tile_x = 0; tile_x < pitch; tile_x += 64u) {
            const uint8_t *tile = src + (size_t)tile_y * pitch +
                                   (size_t)tile_x * 16u;
            for (uint32_t y = 0; y < count; ++y) {
                uint8_t *line = dst + (size_t)(tile_y + y) * pitch + tile_x;
                const size_t row = (y / 8u) * 512u + ((y & 7u) / 2u) * 64u +
                                    (y & 1u) * 16u;
                memcpy(line +  0u, tile + row +   0u, 16u);
                memcpy(line + 16u, tile + row +  32u, 16u);
                memcpy(line + 32u, tile + row + 256u, 16u);
                memcpy(line + 48u, tile + row + 288u, 16u);
            }
        }
    }
}

bool tg_nvdec_nv12_readback(void *dst, size_t dst_size,
                           const void *src, size_t src_size,
                           uint32_t pitch, uint32_t height)
{
    struct tg_nvdec_nv12_storage layout;
    if (!dst || !src || !tg_nvdec_nv12_storage_layout(pitch, height, &layout) ||
        dst_size < layout.linear_bytes || src_size < layout.tiled_bytes)
        return false;
    const uintptr_t d = (uintptr_t)dst, s = (uintptr_t)src;
    if (layout.linear_bytes > UINTPTR_MAX - d || layout.tiled_bytes > UINTPTR_MAX - s ||
        (d < s + layout.tiled_bytes && s < d + layout.linear_bytes))
        return false;
    readback_tbl_plane(dst, src, pitch, height);
    readback_tbl_plane((uint8_t *)dst + layout.chroma_offset,
                       (const uint8_t *)src + layout.chroma_offset,
                       pitch, height / 2u);
    return true;
}

size_t tg_nvdec_build_h264_cmd(uint32_t *out, size_t words,
                               const struct tg_nvdec_cmd_buffers *bufs)
{
    size_t i = 0;
    uint32_t s;

    if (!out || !bufs || !bufs->config || !bufs->bitstream ||
        !bufs->slice_offsets || !bufs->coloc || !bufs->history ||
        !bufs->mbhist || !bufs->status || !bufs->stream_len ||
        !bufs->slice_count || bufs->slice_count > TG_NVDEC_MAX_SLICES ||
        /* T210 Host1x05 has192 counters; UCLASS INCR_SYNCPT carries an8-bit
         * index below the condition byte. The earlier32-counter limit
         * rejected the adapter's reserved counter191 before submission. */
        bufs->syncpt_id >= 192u)
        return 0;
    if (!method_address_valid(bufs->config) ||
        !method_address_valid(bufs->bitstream) ||
        !method_address_valid(bufs->slice_offsets) ||
        !method_address_valid(bufs->coloc) ||
        !method_address_valid(bufs->history) ||
        !method_address_valid(bufs->mbhist) ||
        !method_address_valid(bufs->status))
        return 0;
    for (s = 0; s < TG_NVDEC_MAX_SURFACES; s++) {
        if (bufs->surfaces[s].valid &&
            (!method_address_valid(bufs->surfaces[s].luma) ||
             !method_address_valid(bufs->surfaces[s].chroma)))
            return 0;
    }

#define EMIT(word) do { if (i >= words) return 0; out[i++] = (word); } while (0)
#define METHOD(name, value) do { \
        EMIT(host1x_incr(NVDEC_UCLASS_METHOD_OFFSET, 2)); \
        EMIT((uint32_t)(name) >> 2); \
        EMIT((uint32_t)(value)); \
    } while (0)

    METHOD(NVC5B0_SET_APPLICATION_ID, NVC5B0_SET_APPLICATION_ID_H264);
    METHOD(NVC5B0_SET_CONTROL_PARAMS,
           NVC5B0_SET_CONTROL_PARAMS_H264 |
           NVC5B0_SET_CONTROL_PARAMS_GPTIMER |
           NVC5B0_SET_CONTROL_PARAMS_ERRCON);
    METHOD(NVC5B0_SET_DRV_PIC_SETUP_OFFSET, bufs->config >> 8);
    METHOD(NVC5B0_SET_IN_BUF_BASE_OFFSET, bufs->bitstream >> 8);
    METHOD(NVC5B0_SET_SLICE_OFFSETS_BUF_OFFSET, bufs->slice_offsets >> 8);

    for (s = 0; s < TG_NVDEC_MAX_SURFACES; s++) {
        if (!bufs->surfaces[s].valid)
            continue;
        METHOD(NVC5B0_SET_PICTURE_LUMA_OFFSET0 + 4u * s, bufs->surfaces[s].luma >> 8);
        METHOD(NVC5B0_SET_PICTURE_CHROMA_OFFSET0 + 4u * s, bufs->surfaces[s].chroma >> 8);
    }

    METHOD(NVC5B0_SET_COLOC_DATA_OFFSET, bufs->coloc >> 8);
    METHOD(NVC5B0_SET_HISTORY_OFFSET, bufs->history >> 8);
    METHOD(NVC5B0_H264_SET_MBHIST_BUF_OFFSET, bufs->mbhist >> 8);
    METHOD(NVC5B0_SET_NVDEC_STATUS_OFFSET, bufs->status >> 8);
    METHOD(NVC5B0_SET_PICTURE_INDEX, bufs->picture_index);
    METHOD(NVC5B0_EXECUTE, NVC5B0_EXECUTE_AWAKEN);

    /* Increment this channel's syncpoint only after the engine consumed the
     * EXECUTE method.  The CPU never performs this write. */
    EMIT(host1x_nonincr(NVDEC_UCLASS_INCR_SYNCPT, 1));
    EMIT(bufs->syncpt_id | (1u << TG_NVDEC_SYNCPT_SHIFT));

#undef METHOD
#undef EMIT
    return i;
}
