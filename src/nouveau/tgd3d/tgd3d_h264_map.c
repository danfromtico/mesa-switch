/* SPDX-License-Identifier: MIT */
#include "tgd3d_h264_map.h"

#include <string.h>

static const uint8_t default_4x4_intra[16] = {
    6, 13, 13, 20, 20, 20, 28, 28, 28, 28, 32, 32, 32, 32, 36, 36,
};
static const uint8_t default_4x4_inter[16] = {
    10, 14, 14, 20, 20, 20, 24, 24, 24, 24, 27, 27, 27, 27, 30, 30,
};
static const uint8_t default_8x8_intra[64] = {
    6, 10, 10, 13, 11, 13, 16, 16, 18, 18, 18, 23, 23, 23, 25, 25,
    25, 25, 27, 27, 27, 27, 29, 29, 29, 29, 30, 30, 30, 30, 32, 32,
    32, 32, 33, 33, 33, 33, 34, 34, 34, 34, 35, 35, 35, 35, 36, 36,
    36, 36, 37, 37, 37, 37, 38, 38, 38, 38, 39, 39, 39, 39, 40, 40,
};
static const uint8_t default_8x8_inter[64] = {
    9, 13, 13, 15, 13, 15, 17, 17, 19, 19, 19, 21, 21, 21, 22, 22,
    22, 22, 24, 24, 24, 24, 25, 25, 25, 25, 26, 26, 26, 26, 27, 27,
    27, 27, 28, 28, 28, 28, 29, 29, 29, 29, 30, 30, 30, 30, 31, 31,
    31, 31, 32, 32, 32, 32, 33, 33, 33, 33, 34, 34, 34, 34, 35, 35,
};

static void set_err(char *err, size_t size, const char *text)
{
    size_t n;
    if (!err || !size)
        return;
    n = strlen(text);
    if (n >= size)
        n = size - 1;
    memcpy(err, text, n);
    err[n] = '\0';
}

void tgd3d_h264_default_scaling(uint8_t s4[6][16], uint8_t s8[2][64])
{
    unsigned i;
    for (i = 0; i < 6; i++)
        memcpy(s4[i], i < 3 ? default_4x4_intra : default_4x4_inter, 16);
    memcpy(s8[0], default_8x8_intra, 64);
    memcpy(s8[1], default_8x8_inter, 64);
}

enum tgd3d_h264_map_result tgd3d_h264_map(
    const struct tgd3d_h264_pic_in *in, uint32_t sample_width,
    uint32_t sample_height, struct tg_nvdec_h264_pic *out,
    char *err, size_t err_size)
{
    uint32_t coded_width, coded_height, i, n = 0;
    uint32_t chroma;

    if (!in || !out) {
        set_err(err, err_size, "missing H.264 mapping input");
        return TGD3D_H264_MAP_BUFFER;
    }
    chroma = (in->flags & TGD3D_H264_CHROMA_MASK) >> TGD3D_H264_CHROMA_SHIFT;
    memset(out, 0, sizeof(*out));
    coded_width = ((uint32_t)in->width_mbs_minus1 + 1u) * 16u;
    coded_height = ((uint32_t)in->height_mbs_minus1 + 1u) * 16u;
    if (!sample_width || !sample_height || sample_width > coded_width ||
        sample_height > coded_height || (sample_width & 1u) ||
        (sample_height & 1u)) {
        set_err(err, err_size, "invalid or odd visible dimensions");
        return TGD3D_H264_MAP_DIMENSIONS;
    }
    if (!tg_nvdec_stage1_supported(sample_width, sample_height, coded_width,
                                   coded_height, chroma, 8u)) {
        set_err(err, err_size, "unsupported H.264 sample size, coded size or chroma format");
        return TGD3D_H264_MAP_UNSUPPORTED;
    }
    if (in->bit_depth_luma_minus8 || in->bit_depth_chroma_minus8 ||
        (in->flags & TGD3D_H264_FIELD) || (in->flags & TGD3D_H264_MBAFF) ||
        !(in->flags & TGD3D_H264_FRAME_MBS_ONLY) ||
        (in->pic_order_cnt_type != 0u && in->pic_order_cnt_type != 2u) ||
        in->num_ref_frames > TG_NVDEC_MAX_DPB) {
        set_err(err, err_size, "interlaced, >8-bit, or unsupported POC/DPB");
        return TGD3D_H264_MAP_UNSUPPORTED;
    }
    if (in->curr_slot >= TG_NVDEC_MAX_SURFACES) {
        set_err(err, err_size, "current output slot is out of range");
        return TGD3D_H264_MAP_DPB;
    }

    out->width = sample_width;
    out->height = sample_height;
    out->coded_width = coded_width;
    out->coded_height = coded_height;
    out->chroma_format_idc = 1;
    out->bit_depth_luma = 8;
    out->bit_depth_chroma = 8;
    out->log2_max_frame_num_minus4 = in->log2_max_frame_num_minus4;
    out->pic_order_cnt_type = in->pic_order_cnt_type;
    out->log2_max_pic_order_cnt_lsb_minus4 = in->log2_max_pic_order_cnt_lsb_minus4;
    out->delta_pic_order_always_zero_flag = in->delta_pic_order_always_zero != 0;
    out->frame_mbs_only_flag = true;
    out->direct_8x8_inference_flag = (in->flags & TGD3D_H264_DIRECT_8X8) != 0;
    out->constrained_intra_pred_flag = (in->flags & TGD3D_H264_CONSTRAINED) != 0;
    out->ref_pic_flag = (in->flags & TGD3D_H264_REF) != 0;
    out->entropy_coding_mode_flag = (in->flags & TGD3D_H264_ENTROPY) != 0;
    out->pic_order_present_flag = (in->flags & TGD3D_H264_PIC_ORDER_PRESENT) != 0;
    out->weighted_pred_flag = (in->flags & TGD3D_H264_WEIGHTED_PRED) != 0;
    out->weighted_bipred_idc = (in->flags & TGD3D_H264_WEIGHTED_BIPRED_MASK) >>
                               TGD3D_H264_WEIGHTED_BIPRED_SHIFT;
    out->pic_init_qp_minus26 = in->pic_init_qp_minus26;
    out->chroma_qp_index_offset = in->chroma_qp_index_offset;
    out->second_chroma_qp_index_offset = in->second_chroma_qp_index_offset;
    out->deblocking_filter_control_present_flag = (in->flags & TGD3D_H264_DEBLOCK) != 0;
    out->redundant_pic_cnt_present_flag = (in->flags & TGD3D_H264_REDUNDANT) != 0;
    out->transform_8x8_mode_flag = (in->flags & TGD3D_H264_TRANSFORM_8X8) != 0;
    out->frame_num = in->frame_num;
    out->curr_slot = in->curr_slot;
    out->curr_top_poc = in->curr_poc[0];
    out->curr_bottom_poc = in->curr_poc[1];
    out->num_ref_idx_l0_active_minus1 = in->num_ref_l0_minus1;
    out->num_ref_idx_l1_active_minus1 = in->num_ref_l1_minus1;
    out->lossless_ipred8x8_filter_enable = true;
    if (in->have_scaling) {
        memcpy(out->scaling4x4, in->scaling4x4, sizeof(out->scaling4x4));
        memcpy(out->scaling8x8, in->scaling8x8, sizeof(out->scaling8x8));
    } else {
        tgd3d_h264_default_scaling(out->scaling4x4, out->scaling8x8);
    }

    for (i = 0; i < in->num_ref_frames; i++) {
        const struct tgd3d_h264_ref_in *r = &in->refs[i];
        struct tg_nvdec_h264_ref *d;
        uint32_t j;
        if (r->slot >= TG_NVDEC_MAX_SURFACES || r->slot == in->curr_slot) {
            set_err(err, err_size, "reference slot is invalid or equals current");
            return TGD3D_H264_MAP_DPB;
        }
        for (j = 0; j < n; j++)
            if (out->dpb[j].slot == r->slot) {
                set_err(err, err_size, "duplicate reference slot");
                return TGD3D_H264_MAP_DPB;
            }
        d = &out->dpb[n++];
        d->slot = r->slot;
        d->frame_num = r->frame_num;
        d->top_poc = r->top_poc;
        d->bottom_poc = r->bottom_poc;
        d->is_long_term = (r->flags & TGD3D_H264_REF_LONG_TERM) != 0;
        d->is_field = (r->flags & TGD3D_H264_REF_FIELD) != 0;
        d->not_existing = (r->flags & TGD3D_H264_REF_NOT_EXISTING) != 0;
    }
    out->dpb_count = (uint8_t)n;
    return TGD3D_H264_MAP_OK;
}

static bool scaling_to_raster(uint8_t *dst, const uint8_t *src, int side)
{
    unsigned index = 0;
    for (int diagonal = 0; diagonal < 2 * side - 1; diagonal++) {
        const int first = diagonal < side ? 0 : diagonal - side + 1;
        const int last = diagonal < side ? diagonal : side - 1;
        for (int i = 0; i <= last - first; i++) {
            const int row = (diagonal & 1) ? first + i : last - i;
            const int col = diagonal - row;
            if (!src[index])
                return false;
            dst[row * side + col] = src[index++];
        }
    }
    return true;
}

enum tgd3d_h264_map_result tgd3d_h264_map_dxva(
    const tgd3d_dxva_pic_h264 *p, const tgd3d_dxva_qmatrix_h264 *matrix,
    uint32_t width, uint32_t height, struct tg_nvdec_h264_pic *out,
    char *err, size_t err_size)
{
    struct tgd3d_h264_pic_in in = {0};
    if (!p || !matrix || !out) {
        set_err(err, err_size, "picture and scaling matrix buffers are required");
        return TGD3D_H264_MAP_BUFFER;
    }
    /* Off-host VLD needs the fields following ContinuationFlag. A zero flag
     * permits a host-parsed picture to omit them or leave them undefined. */
    if (p->ContinuationFlag != 1) {
        set_err(err, err_size, "DXVA off-host picture parameters are absent");
        return TGD3D_H264_MAP_UNSUPPORTED;
    }
    if (p->num_ref_frames > TG_NVDEC_MAX_DPB ||
        p->num_slice_groups_minus1 || p->residual_colour_transform_flag ||
        p->sp_for_switch_flag || tgd3d_dxva_entry_flag(&p->CurrPic)) {
        set_err(err, err_size, "unsupported DXVA picture configuration");
        return TGD3D_H264_MAP_UNSUPPORTED;
    }
    in.width_mbs_minus1 = p->wFrameWidthInMbsMinus1;
    in.height_mbs_minus1 = p->wFrameHeightInMbsMinus1;
    in.curr_slot = tgd3d_dxva_entry_index(&p->CurrPic);
    if (p->frame_mbs_only_flag) in.flags |= TGD3D_H264_FRAME_MBS_ONLY;
    if (p->MbaffFrameFlag) in.flags |= TGD3D_H264_MBAFF;
    in.flags |= (p->chroma_format_idc & 3u) << TGD3D_H264_CHROMA_SHIFT;
    if (p->RefPicFlag) in.flags |= TGD3D_H264_REF;
    if (p->constrained_intra_pred_flag) in.flags |= TGD3D_H264_CONSTRAINED;
    if (p->weighted_pred_flag) in.flags |= TGD3D_H264_WEIGHTED_PRED;
    in.flags |= (p->weighted_bipred_idc & 3u) << TGD3D_H264_WEIGHTED_BIPRED_SHIFT;
    if (p->transform_8x8_mode_flag) in.flags |= TGD3D_H264_TRANSFORM_8X8;
    if (p->entropy_coding_mode_flag) in.flags |= TGD3D_H264_ENTROPY;
    if (p->pic_order_present_flag) in.flags |= TGD3D_H264_PIC_ORDER_PRESENT;
    if (p->deblocking_filter_control_present_flag) in.flags |= TGD3D_H264_DEBLOCK;
    if (p->redundant_pic_cnt_present_flag) in.flags |= TGD3D_H264_REDUNDANT;
    if (p->direct_8x8_inference_flag) in.flags |= TGD3D_H264_DIRECT_8X8;
    if (p->field_pic_flag) in.flags |= TGD3D_H264_FIELD;
    in.bit_depth_luma_minus8 = p->bit_depth_luma_minus8;
    in.bit_depth_chroma_minus8 = p->bit_depth_chroma_minus8;
    in.curr_poc[0] = p->CurrFieldOrderCnt[0];
    in.curr_poc[1] = p->CurrFieldOrderCnt[1];
    in.pic_init_qp_minus26 = p->pic_init_qp_minus26;
    in.chroma_qp_index_offset = p->chroma_qp_index_offset;
    in.second_chroma_qp_index_offset = p->second_chroma_qp_index_offset;
    in.num_ref_l0_minus1 = p->num_ref_idx_l0_active_minus1;
    in.num_ref_l1_minus1 = p->num_ref_idx_l1_active_minus1;
    in.frame_num = p->frame_num;
    in.log2_max_frame_num_minus4 = p->log2_max_frame_num_minus4;
    in.pic_order_cnt_type = p->pic_order_cnt_type;
    in.log2_max_pic_order_cnt_lsb_minus4 = p->log2_max_pic_order_cnt_lsb_minus4;
    in.delta_pic_order_always_zero = p->delta_pic_order_always_zero_flag;

    /* num_ref_frames is an SPS maximum. IDR commonly has a nonzero maximum
     * but no references. Walk all entries and retain their paired metadata. */
    for (unsigned i = 0; i < TG_NVDEC_MAX_DPB; i++) {
        const uint32_t used = (p->UsedForReferenceFlags >> (2u * i)) & 3u;
        const uint32_t slot = tgd3d_dxva_entry_index(&p->RefFrameList[i]);
        if (!used)
            continue;
        if (slot >= TG_NVDEC_MAX_SURFACES) {
            set_err(err, err_size, "used DXVA reference has no valid surface");
            return TGD3D_H264_MAP_DPB;
        }
        if (used != 3u || (p->NonExistingFrameFlags & (1u << i))) {
            set_err(err, err_size, "field-only or nonexisting references are not enabled");
            return TGD3D_H264_MAP_UNSUPPORTED;
        }
        struct tgd3d_h264_ref_in *r = &in.refs[in.num_ref_frames++];
        r->slot = (uint8_t)slot;
        r->frame_num = p->FrameNumList[i];
        r->top_poc = p->FieldOrderCntList[i][0];
        r->bottom_poc = p->FieldOrderCntList[i][1];
        if (tgd3d_dxva_entry_flag(&p->RefFrameList[i]))
            r->flags |= TGD3D_H264_REF_LONG_TERM;
        /* Both UsedForReferenceFlags bits denote a frame, not a field. */
    }
    if (in.num_ref_frames > p->num_ref_frames) {
        set_err(err, err_size, "active reference count exceeds SPS maximum");
        return TGD3D_H264_MAP_DPB;
    }
    in.have_scaling = 1;
    for (unsigned i = 0; i < 6; i++)
        if (!scaling_to_raster(in.scaling4x4[i], matrix->bScalingLists4x4[i], 4)) {
            set_err(err, err_size, "zero DXVA 4x4 scaling weight");
            return TGD3D_H264_MAP_BUFFER;
        }
    for (unsigned i = 0; i < 2; i++)
        if (!scaling_to_raster(in.scaling8x8[i], matrix->bScalingLists8x8[i], 8)) {
            set_err(err, err_size, "zero DXVA 8x8 scaling weight");
            return TGD3D_H264_MAP_BUFFER;
        }
    return tgd3d_h264_map(&in, width, height, out, err, err_size);
}

static uint32_t strip_start_code(const uint8_t *p, uint32_t size)
{
    if (size >= 4 && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1)
        return 4;
    if (size >= 3 && p[0] == 0 && p[1] == 0 && p[2] == 1)
        return 3;
    return 0;
}

enum tgd3d_h264_map_result tgd3d_h264_assemble_bitstream(
    const uint8_t *bitstream, size_t bitstream_size,
    const struct tgd3d_h264_slice_in *slices, uint32_t slice_count,
    uint8_t *out, size_t out_capacity, uint32_t *out_size,
    uint32_t *offsets, uint32_t *out_slices)
{
    static const uint8_t eos[16] = {
        0, 0, 1, 0x0b, 0, 0, 0, 0,
        0, 0, 1, 0x0b, 0, 0, 0, 0,
    };
    size_t used = 0;
    uint32_t i;

    if (!bitstream || !slices || !out || !out_size || !offsets ||
        !slice_count || slice_count > TG_NVDEC_MAX_SLICES)
        return TGD3D_H264_MAP_SLICE;
    for (i = 0; i < slice_count; i++) {
        uint32_t offset = slices[i].offset;
        uint32_t size = slices[i].size;
        uint32_t skip;
        if (!size || (size_t)offset + size > bitstream_size)
            return TGD3D_H264_MAP_SLICE;
        if (offsets)
            offsets[i] = (uint32_t)used;
        skip = strip_start_code(bitstream + offset, size);
        offset += skip;
        size -= skip;
        if (used + 3u + size + sizeof(eos) > out_capacity)
            return TGD3D_H264_MAP_BUFFER;
        out[used++] = 0;
        out[used++] = 0;
        out[used++] = 1;
        memcpy(out + used, bitstream + offset, size);
        used += size;
    }
    if (used + sizeof(eos) > out_capacity)
        return TGD3D_H264_MAP_BUFFER;
    memcpy(out + used, eos, sizeof(eos));
    used += sizeof(eos);
    offsets[slice_count] = (uint32_t)used;
    *out_size = (uint32_t)used;
    if (out_slices)
        *out_slices = slice_count;
    return TGD3D_H264_MAP_OK;
}
