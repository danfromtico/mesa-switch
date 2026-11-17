/* SPDX-License-Identifier: MIT */
#include "nvdec_parse.h"

#include <string.h>

#define PARSE_MAX_FRAMES 64u

struct br {
    const uint8_t *p;
    size_t bits;
    size_t pos;
    bool ok;
};

static void br_init(struct br *b, const uint8_t *p, size_t bytes)
{
    b->p = p;
    b->bits = bytes * 8u;
    b->pos = 0;
    b->ok = true;
}

static uint32_t br_u(struct br *b, unsigned n)
{
    uint32_t v = 0;
    unsigned i;

    if (!b->ok || n > 32u || b->pos + n > b->bits) {
        b->ok = false;
        return 0;
    }
    for (i = 0; i < n; i++) {
        v = (v << 1) | ((b->p[b->pos >> 3] >> (7u - (b->pos & 7u))) & 1u);
        b->pos++;
    }
    return v;
}

static uint32_t br_ue(struct br *b)
{
    unsigned zeros = 0;
    uint32_t suffix;

    while (b->ok && zeros < 32u && br_u(b, 1) == 0)
        zeros++;
    if (!b->ok || zeros >= 32u) {
        b->ok = false;
        return 0;
    }
    suffix = br_u(b, zeros);
    return ((1u << zeros) - 1u) + suffix;
}

static int32_t br_se(struct br *b)
{
    const uint32_t u = br_ue(b);
    return (u & 1u) ? (int32_t)((u + 1u) >> 1) : -(int32_t)(u >> 1);
}

/* H.264 more_rbsp_data(): ignore the rbsp_stop_one_bit and zero padding. */
static bool br_more_rbsp(const struct br *b)
{
    size_t last_one;

    if (!b->ok || !b->bits)
        return false;
    last_one = b->bits;
    while (last_one > 0) {
        const size_t bit = last_one - 1;
        if ((b->p[bit >> 3] >> (7u - (bit & 7u))) & 1u)
            break;
        last_one--;
    }
    if (!last_one)
        return false;
    return b->pos < last_one - 1u;
}

static size_t rbsp_from_nal(const uint8_t *nal, size_t bytes,
                            uint8_t *out, size_t capacity)
{
    size_t i, n = 0;
    unsigned zeros = 0;

    for (i = 0; i < bytes && n < capacity; i++) {
        if (zeros >= 2 && nal[i] == 0x03) {
            zeros = 0;
            continue;
        }
        out[n++] = nal[i];
        if (nal[i] == 0)
            zeros++;
        else
            zeros = 0;
    }
    return n;
}

/* Find the next Annex B NAL unit.  Handles both 3-byte and 4-byte start
 * codes, including the zero_byte form. */
static bool next_nal(const uint8_t *data, size_t size, size_t *pos,
                     const uint8_t **nal, size_t *nal_size)
{
    size_t i = *pos;
    size_t payload = 0;

    while (i + 3 <= size) {
        if (data[i] == 0 && data[i + 1] == 0) {
            if (data[i + 2] == 1) {
                payload = i + 3;
                break;
            }
            if (i + 4 <= size && data[i + 2] == 0 && data[i + 3] == 1) {
                payload = i + 4;
                break;
            }
        }
        i++;
    }
    if (!payload)
        return false;

    i = payload;
    while (i + 3 <= size) {
        if (data[i] == 0 && data[i + 1] == 0 &&
            (data[i + 2] == 1 ||
             (data[i + 2] == 0 && i + 4 <= size && data[i + 3] == 1)))
            break;
        i++;
    }
    /* No following start code: the remaining one or two bytes still belong
     * to this NAL. They can contain CABAC payload, not disposable padding. */
    if (size - i < 3)
        i = size;
    *nal = data + payload;
    *nal_size = i - payload;
    *pos = i;
    return *nal_size > 0;
}

static void default_scaling(uint8_t s4[6][16], uint8_t s8[2][64])
{
    static const uint8_t i4[16] = {
        6, 13, 13, 20, 20, 20, 28, 28, 28, 28, 32, 32, 32, 32, 36, 36,
    };
    static const uint8_t p4[16] = {
        10, 14, 14, 20, 20, 20, 24, 24, 24, 24, 27, 27, 27, 27, 30, 30,
    };
    static const uint8_t i8[64] = {
        6, 10, 10, 13, 11, 13, 16, 16, 18, 18, 18, 23, 23, 23, 25, 25,
        25, 25, 27, 27, 27, 27, 29, 29, 29, 29, 30, 30, 30, 30, 32, 32,
        32, 32, 33, 33, 33, 33, 34, 34, 34, 34, 35, 35, 35, 35, 36, 36,
        36, 36, 37, 37, 37, 37, 38, 38, 38, 38, 39, 39, 39, 39, 40, 40,
    };
    static const uint8_t p8[64] = {
        9, 13, 13, 15, 13, 15, 17, 17, 19, 19, 19, 21, 21, 21, 22, 22,
        22, 22, 24, 24, 24, 24, 25, 25, 25, 25, 26, 26, 26, 26, 27, 27,
        27, 27, 28, 28, 28, 28, 29, 29, 29, 29, 30, 30, 30, 30, 31, 31,
        31, 31, 32, 32, 32, 32, 33, 33, 33, 33, 34, 34, 34, 34, 35, 35,
    };
    unsigned i;

    for (i = 0; i < 6; i++)
        memcpy(s4[i], i < 3 ? i4 : p4, 16);
    memcpy(s8[0], i8, 64);
    memcpy(s8[1], p8, 64);
}

static bool parse_sps(const uint8_t *nal, size_t bytes, struct tg_nvdec_sps *sps)
{
    uint8_t rbsp[4096];
    struct br b;
    uint32_t id;
    size_t n;

    if (bytes < 4)
        return false;
    memset(sps, 0, sizeof(*sps));
    default_scaling(sps->scaling4x4, sps->scaling8x8);
    sps->profile_idc = nal[1];
    sps->level_idc = nal[3];
    n = rbsp_from_nal(nal + 4, bytes - 4, rbsp, sizeof(rbsp));
    br_init(&b, rbsp, n);

    id = br_ue(&b);
    if (!b.ok || id >= TG_NVDEC_MAX_SPS)
        return false;

    sps->chroma_format_idc = 1;
    sps->bit_depth_luma = 8;
    sps->bit_depth_chroma = 8;
    if (sps->profile_idc == 100 || sps->profile_idc == 110 ||
        sps->profile_idc == 122 || sps->profile_idc == 244 ||
        sps->profile_idc == 44 || sps->profile_idc == 83 ||
        sps->profile_idc == 86 || sps->profile_idc == 118 ||
        sps->profile_idc == 128 || sps->profile_idc == 138 ||
        sps->profile_idc == 139 || sps->profile_idc == 134 ||
        sps->profile_idc == 135) {
        sps->chroma_format_idc = br_ue(&b);
        if (sps->chroma_format_idc == 3)
            (void)br_u(&b, 1);
        sps->bit_depth_luma = br_ue(&b) + 8u;
        sps->bit_depth_chroma = br_ue(&b) + 8u;
        (void)br_u(&b, 1);       /* qpprime_y_zero_transform_bypass_flag */
        if (br_u(&b, 1))         /* seq_scaling_matrix_present_flag */
            return false;
    }

    sps->log2_max_frame_num_minus4 = br_ue(&b);
    sps->pic_order_cnt_type = br_ue(&b);
    if (sps->pic_order_cnt_type == 0) {
        sps->log2_max_pic_order_cnt_lsb_minus4 = br_ue(&b);
    } else if (sps->pic_order_cnt_type == 1) {
        return false;
    }
    sps->max_num_ref_frames = br_ue(&b);
    (void)br_u(&b, 1);           /* gaps_in_frame_num_value_allowed_flag */
    sps->pic_width_in_mbs_minus1 = br_ue(&b);
    sps->pic_height_in_map_units_minus1 = br_ue(&b);
    sps->frame_mbs_only_flag = br_u(&b, 1) != 0;
    if (!sps->frame_mbs_only_flag)
        sps->mb_adaptive_frame_field_flag = br_u(&b, 1) != 0;
    sps->direct_8x8_inference_flag = br_u(&b, 1) != 0;
    if (br_u(&b, 1)) {           /* frame_cropping_flag */
        sps->crop_left = br_ue(&b);
        sps->crop_right = br_ue(&b);
        sps->crop_top = br_ue(&b);
        sps->crop_bottom = br_ue(&b);
    }
    (void)br_u(&b, 1);           /* vui_parameters_present_flag */
    if (!b.ok)
        return false;

    if (sps->log2_max_frame_num_minus4 > 12 ||
        sps->log2_max_pic_order_cnt_lsb_minus4 > 12 ||
        sps->pic_width_in_mbs_minus1 > 255 ||
        sps->pic_height_in_map_units_minus1 > 255 ||
        !sps->frame_mbs_only_flag || sps->mb_adaptive_frame_field_flag)
        return false;

    sps->coded_width = (sps->pic_width_in_mbs_minus1 + 1u) * 16u;
    sps->coded_height = (sps->pic_height_in_map_units_minus1 + 1u) * 16u;
    {
        const uint32_t sub_width = sps->chroma_format_idc == 1 ? 2u : 1u;
        const uint32_t sub_height = sps->chroma_format_idc == 1 ? 2u : 1u;
        const uint32_t crop_x = sub_width * (sps->crop_left + sps->crop_right);
        const uint32_t crop_y = sub_height * (sps->crop_top + sps->crop_bottom);
        if (crop_x >= sps->coded_width || crop_y >= sps->coded_height)
            return false;
        sps->width = sps->coded_width - crop_x;
        sps->height = sps->coded_height - crop_y;
    }
    sps->id = id;
    sps->valid = true;
    return true;
}

static bool parse_pps(const uint8_t *nal, size_t bytes, struct tg_nvdec_pps *pps)
{
    uint8_t rbsp[1024];
    struct br b;
    uint32_t id;
    size_t n;

    if (bytes < 2)
        return false;
    memset(pps, 0, sizeof(*pps));
    default_scaling(pps->scaling4x4, pps->scaling8x8);
    n = rbsp_from_nal(nal + 1, bytes - 1, rbsp, sizeof(rbsp));
    br_init(&b, rbsp, n);

    id = br_ue(&b);
    if (!b.ok || id >= TG_NVDEC_MAX_PPS)
        return false;
    pps->id = id;
    pps->sps_id = br_ue(&b);
    pps->entropy_coding_mode_flag = br_u(&b, 1) != 0;
    pps->bottom_field_pic_order_in_frame_present_flag = br_u(&b, 1) != 0;
    if (br_ue(&b) != 0)
        return false;            /* slice groups are not implemented */
    pps->num_ref_idx_l0_default_active_minus1 = br_ue(&b);
    pps->num_ref_idx_l1_default_active_minus1 = br_ue(&b);
    pps->weighted_pred_flag = br_u(&b, 1) != 0;
    pps->weighted_bipred_idc = br_u(&b, 2);
    pps->pic_init_qp_minus26 = br_se(&b);
    (void)br_se(&b);             /* pic_init_qs_minus26 */
    pps->chroma_qp_index_offset = br_se(&b);
    pps->deblocking_filter_control_present_flag = br_u(&b, 1) != 0;
    pps->constrained_intra_pred_flag = br_u(&b, 1) != 0;
    pps->redundant_pic_cnt_present_flag = br_u(&b, 1) != 0;
    if (br_more_rbsp(&b)) {
        pps->transform_8x8_mode_flag = br_u(&b, 1) != 0;
        if (br_u(&b, 1))
            return false;
        pps->second_chroma_qp_index_offset = br_se(&b);
    } else {
        pps->second_chroma_qp_index_offset = pps->chroma_qp_index_offset;
    }
    if (!b.ok || pps->sps_id >= TG_NVDEC_MAX_SPS)
        return false;
    pps->valid = true;
    return true;
}

struct slice_header {
    uint32_t first_mb;
    uint32_t slice_type;
    uint32_t pps_id;
    uint32_t frame_num;
    bool field_pic_flag;
    bool bottom_field_flag;
    uint32_t idr_pic_id;
    int32_t pic_order_cnt_lsb;
    int32_t delta_pic_order_cnt_bottom;
    bool direct_spatial_mv_pred_flag;
    bool num_ref_idx_active_override_flag;
    uint32_t num_ref_idx_l0_active_minus1;
    uint32_t num_ref_idx_l1_active_minus1;
    bool adaptive_ref_pic_marking_mode_flag;
    bool no_output_of_prior_pics_flag;
    bool long_term_reference_flag;
    uint32_t cabac_init_idc;
    int32_t slice_qp_delta;
    uint32_t disable_deblocking_filter_idc;
    int32_t slice_alpha_c0_offset_div2;
    int32_t slice_beta_offset_div2;
    bool idr;
    uint32_t nal_ref_idc;
};

static bool skip_ref_pic_list_modification(struct br *b, uint32_t slice_type)
{
    if (slice_type % 5 == 2 || slice_type % 5 == 4)
        return true;
    if (br_u(b, 1)) {
        for (;;) {
            const uint32_t idc = br_ue(b);
            if (idc == 3)
                break;
            if (idc > 2)
                return false;
            (void)br_ue(b);
        }
    }
    if (slice_type % 5 == 1) {
        if (br_u(b, 1)) {
            for (;;) {
                const uint32_t idc = br_ue(b);
                if (idc == 3)
                    break;
                if (idc > 2)
                    return false;
                (void)br_ue(b);
            }
        }
    }
    return b->ok;
}

static bool skip_dec_ref_pic_marking(struct br *b, bool idr, uint32_t nal_ref_idc,
                                     bool *adaptive, bool *no_output, bool *long_term)
{
    if (!nal_ref_idc)
        return true;
    if (idr) {
        *no_output = br_u(b, 1) != 0;
        *long_term = br_u(b, 1) != 0;
        return b->ok;
    }
    *adaptive = br_u(b, 1) != 0;
    if (*adaptive) {
        for (;;) {
            const uint32_t op = br_ue(b);
            if (op == 0)
                break;
            if (op == 1 || op == 2 || op == 4 || op == 6) {
                (void)br_ue(b);
            } else if (op == 3) {
                (void)br_ue(b);
                (void)br_ue(b);
            } else if (op != 5) {
                return false;
            }
        }
    }
    return b->ok;
}

static bool parse_slice(const uint8_t *nal, size_t bytes,
                        const struct tg_nvdec_sps *sps,
                        const struct tg_nvdec_pps *pps,
                        struct slice_header *sh)
{
    uint8_t rbsp[2048];
    struct br b;
    size_t n;

    if (bytes < 2)
        return false;
    memset(sh, 0, sizeof(*sh));
    sh->idr = (nal[0] & 0x1f) == 5;
    sh->nal_ref_idc = (nal[0] >> 5) & 3u;
    n = rbsp_from_nal(nal + 1, bytes - 1, rbsp, sizeof(rbsp));
    br_init(&b, rbsp, n);

    sh->first_mb = br_ue(&b);
    sh->slice_type = br_ue(&b);
    sh->pps_id = br_ue(&b);
    if (!b.ok || sh->pps_id != pps->id || sh->slice_type > 9u)
        return false;
    if (sh->slice_type % 5 == 3 || sh->slice_type % 5 == 4)
        return false;
    sh->frame_num = br_u(&b, sps->log2_max_frame_num_minus4 + 4u);
    if (!sps->frame_mbs_only_flag) {
        sh->field_pic_flag = br_u(&b, 1) != 0;
        if (sh->field_pic_flag)
            sh->bottom_field_flag = br_u(&b, 1) != 0;
    }
    if (sh->field_pic_flag)
        return false;            /* field pictures are not implemented */
    if (sh->idr)
        sh->idr_pic_id = br_ue(&b);
    if (sps->pic_order_cnt_type == 0) {
        sh->pic_order_cnt_lsb = (int32_t)br_u(&b,
            sps->log2_max_pic_order_cnt_lsb_minus4 + 4u);
        if (pps->bottom_field_pic_order_in_frame_present_flag)
            sh->delta_pic_order_cnt_bottom = br_se(&b);
    }
    if (pps->redundant_pic_cnt_present_flag)
        (void)br_ue(&b);
    if (sh->slice_type % 5 == 1)
        sh->direct_spatial_mv_pred_flag = br_u(&b, 1) != 0;
    if (sh->slice_type % 5 == 0 || sh->slice_type % 5 == 1) {
        sh->num_ref_idx_active_override_flag = br_u(&b, 1) != 0;
        if (sh->num_ref_idx_active_override_flag) {
            sh->num_ref_idx_l0_active_minus1 = br_ue(&b);
            if (sh->slice_type % 5 == 1)
                sh->num_ref_idx_l1_active_minus1 = br_ue(&b);
        } else {
            sh->num_ref_idx_l0_active_minus1 = pps->num_ref_idx_l0_default_active_minus1;
            sh->num_ref_idx_l1_active_minus1 = pps->num_ref_idx_l1_default_active_minus1;
        }
    }
    if (!skip_ref_pic_list_modification(&b, sh->slice_type))
        return false;
    if (pps->weighted_pred_flag && sh->slice_type % 5 == 0)
        return false;
    if (pps->weighted_bipred_idc == 1 && sh->slice_type % 5 == 1)
        return false;
    if (!skip_dec_ref_pic_marking(&b, sh->idr, sh->nal_ref_idc,
                                  &sh->adaptive_ref_pic_marking_mode_flag,
                                  &sh->no_output_of_prior_pics_flag,
                                  &sh->long_term_reference_flag))
        return false;
    if (pps->entropy_coding_mode_flag && sh->slice_type % 5 != 2)
        sh->cabac_init_idc = br_ue(&b);
    sh->slice_qp_delta = br_se(&b);
    if (pps->deblocking_filter_control_present_flag) {
        sh->disable_deblocking_filter_idc = br_ue(&b);
        if (sh->disable_deblocking_filter_idc > 2)
            return false;
        if (sh->disable_deblocking_filter_idc != 1) {
            sh->slice_alpha_c0_offset_div2 = br_se(&b);
            sh->slice_beta_offset_div2 = br_se(&b);
        }
    }
    return b.ok;
}

struct decoded_frame {
    bool valid;
    bool ref;
    bool long_term;
    uint32_t slot;
    uint32_t frame_num;
    uint32_t long_term_idx;
    int32_t top_poc;
    int32_t bottom_poc;
};

struct parse_ctx {
    struct tg_nvdec_sps sps[TG_NVDEC_MAX_SPS];
    struct tg_nvdec_pps pps[TG_NVDEC_MAX_PPS];
    struct decoded_frame dpb[TG_NVDEC_MAX_SURFACES];
    uint32_t next_slot;
    bool poc_valid;
    int32_t prev_msb;
    int32_t prev_lsb;
    uint32_t prev_frame_num;
    uint32_t prev_frame_num_offset;
};

struct frame_build {
    struct slice_header sh;
    uint32_t pps_id;
    uint32_t sps_id;
    uint32_t first_index;       /* index into frames[] */
    uint32_t slice_count;
    uint32_t bitstream_offset;
    uint32_t slice_offsets[TG_NVDEC_MAX_SLICES + 1];
};

static void put_err(char *err, size_t size, const char *text)
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

static uint32_t alloc_slot(struct parse_ctx *ctx)
{
    uint32_t i;

    for (i = 0; i < TG_NVDEC_MAX_SURFACES; i++) {
        const uint32_t slot = (ctx->next_slot + i) % TG_NVDEC_MAX_SURFACES;
        bool used = false;
        uint32_t j;
        for (j = 0; j < TG_NVDEC_MAX_SURFACES; j++) {
            if (ctx->dpb[j].valid && ctx->dpb[j].ref && ctx->dpb[j].slot == slot) {
                used = true;
                break;
            }
        }
        if (!used) {
            ctx->next_slot = (slot + 1u) % TG_NVDEC_MAX_SURFACES;
            return slot;
        }
    }
    return TG_NVDEC_MAX_SURFACES;
}

static int32_t compute_poc(struct parse_ctx *ctx, const struct tg_nvdec_sps *sps,
                           const struct tg_nvdec_pps *pps,
                           const struct slice_header *sh, int32_t *bottom)
{
    if (sps->pic_order_cnt_type == 2) {
        uint32_t frame_num_offset;
        int32_t poc;

        if (sh->idr) {
            frame_num_offset = 0;
            ctx->prev_frame_num = 0;
            ctx->prev_frame_num_offset = 0;
        } else if (sh->frame_num < ctx->prev_frame_num) {
            frame_num_offset = ctx->prev_frame_num_offset +
                               (1u << (sps->log2_max_frame_num_minus4 + 4u));
        } else {
            frame_num_offset = ctx->prev_frame_num_offset;
        }
        poc = (int32_t)(2u * (frame_num_offset + sh->frame_num)) -
              (sh->nal_ref_idc ? 0 : 1);
        *bottom = poc;
        if (sh->nal_ref_idc) {
            ctx->prev_frame_num = sh->frame_num;
            ctx->prev_frame_num_offset = frame_num_offset;
            ctx->poc_valid = true;
        }
        return poc;
    }

    {
        const uint32_t max_lsb = 1u << (sps->log2_max_pic_order_cnt_lsb_minus4 + 4u);
        int32_t msb;
        int32_t top;

        if (sh->idr) {
            msb = 0;
            ctx->prev_msb = 0;
            ctx->prev_lsb = 0;
            ctx->poc_valid = false;
        } else if (!ctx->poc_valid) {
            msb = 0;
        } else if (sh->pic_order_cnt_lsb < ctx->prev_lsb &&
                   ctx->prev_lsb - sh->pic_order_cnt_lsb >= (int32_t)(max_lsb / 2u)) {
            msb = ctx->prev_msb + (int32_t)max_lsb;
        } else if (sh->pic_order_cnt_lsb > ctx->prev_lsb &&
                   sh->pic_order_cnt_lsb - ctx->prev_lsb > (int32_t)(max_lsb / 2u)) {
            msb = ctx->prev_msb - (int32_t)max_lsb;
        } else {
            msb = ctx->prev_msb;
        }
        top = msb + sh->pic_order_cnt_lsb;
        *bottom = top;
        if (pps->bottom_field_pic_order_in_frame_present_flag)
            *bottom += sh->delta_pic_order_cnt_bottom;
        if (sh->nal_ref_idc) {
            ctx->prev_msb = msb;
            ctx->prev_lsb = sh->pic_order_cnt_lsb;
            ctx->poc_valid = true;
        }
        return top;
    }
}

static void enforce_sliding_window(struct parse_ctx *ctx, uint32_t max_refs)
{
    uint32_t i, count = 0;

    for (i = 0; i < TG_NVDEC_MAX_SURFACES; i++)
        if (ctx->dpb[i].valid && ctx->dpb[i].ref && !ctx->dpb[i].long_term)
            count++;
    while (count > max_refs) {
        uint32_t victim = TG_NVDEC_MAX_SURFACES;
        uint32_t j;
        for (j = 0; j < TG_NVDEC_MAX_SURFACES; j++) {
            if (!ctx->dpb[j].valid || !ctx->dpb[j].ref || ctx->dpb[j].long_term)
                continue;
            if (victim == TG_NVDEC_MAX_SURFACES ||
                ctx->dpb[j].frame_num < ctx->dpb[victim].frame_num)
                victim = j;
        }
        if (victim == TG_NVDEC_MAX_SURFACES)
            break;
        ctx->dpb[victim].valid = false;
        count--;
    }
}

static bool fill_picture(struct parse_ctx *ctx, const struct tg_nvdec_sps *sps,
                         const struct tg_nvdec_pps *pps, const struct slice_header *sh,
                         struct tg_nvdec_parsed_frame *frame, uint32_t slot)
{
    struct tg_nvdec_h264_pic *p = &frame->pic;
    uint32_t i, refs = 0;
    int32_t bottom = 0;
    const int32_t top = compute_poc(ctx, sps, pps, sh, &bottom);

    if (sh->idr) {
        for (i = 0; i < TG_NVDEC_MAX_SURFACES; i++)
            ctx->dpb[i].valid = false;
    }

    memset(p, 0, sizeof(*p));
    p->width = sps->width;
    p->height = sps->height;
    p->coded_width = sps->coded_width;
    p->coded_height = sps->coded_height;
    p->pitch_luma = sps->coded_width;
    p->pitch_chroma = sps->coded_width;
    p->chroma_format_idc = sps->chroma_format_idc;
    p->bit_depth_luma = sps->bit_depth_luma;
    p->bit_depth_chroma = sps->bit_depth_chroma;
    p->log2_max_frame_num_minus4 = sps->log2_max_frame_num_minus4;
    p->pic_order_cnt_type = sps->pic_order_cnt_type;
    p->log2_max_pic_order_cnt_lsb_minus4 = sps->log2_max_pic_order_cnt_lsb_minus4;
    p->delta_pic_order_always_zero_flag = sps->delta_pic_order_always_zero_flag;
    p->frame_mbs_only_flag = sps->frame_mbs_only_flag;
    p->mb_adaptive_frame_field_flag = sps->mb_adaptive_frame_field_flag;
    p->direct_8x8_inference_flag = sps->direct_8x8_inference_flag;
    p->constrained_intra_pred_flag = pps->constrained_intra_pred_flag;
    p->ref_pic_flag = sh->nal_ref_idc != 0;
    p->entropy_coding_mode_flag = pps->entropy_coding_mode_flag;
    p->pic_order_present_flag = pps->bottom_field_pic_order_in_frame_present_flag;
    p->weighted_pred_flag = pps->weighted_pred_flag;
    p->weighted_bipred_idc = pps->weighted_bipred_idc;
    p->pic_init_qp_minus26 = pps->pic_init_qp_minus26;
    p->chroma_qp_index_offset = pps->chroma_qp_index_offset;
    p->second_chroma_qp_index_offset = pps->second_chroma_qp_index_offset;
    p->deblocking_filter_control_present_flag = pps->deblocking_filter_control_present_flag;
    p->redundant_pic_cnt_present_flag = pps->redundant_pic_cnt_present_flag;
    p->transform_8x8_mode_flag = pps->transform_8x8_mode_flag;
    p->field_pic_flag = sh->field_pic_flag;
    p->bottom_field_flag = sh->bottom_field_flag;
    p->lossless_ipred8x8_filter_enable = true;
    p->frame_num = sh->frame_num;
    p->curr_slot = slot;
    p->curr_top_poc = top;
    p->curr_bottom_poc = bottom;
    p->num_ref_idx_l0_active_minus1 = sh->num_ref_idx_l0_active_minus1;
    p->num_ref_idx_l1_active_minus1 = sh->num_ref_idx_l1_active_minus1;
    memcpy(p->scaling4x4, pps->scaling4x4, sizeof(p->scaling4x4));
    memcpy(p->scaling8x8, pps->scaling8x8, sizeof(p->scaling8x8));

    {
        struct decoded_frame *ordered[TG_NVDEC_MAX_SURFACES];
        uint32_t n = 0;

        for (i = 0; i < TG_NVDEC_MAX_SURFACES; i++) {
            if (ctx->dpb[i].valid && ctx->dpb[i].ref &&
                ctx->dpb[i].slot != slot)
                ordered[n++] = &ctx->dpb[i];
        }
        for (i = 0; i < n; i++) {
            uint32_t j;
            for (j = i + 1; j < n; j++) {
                if (ordered[j]->top_poc > ordered[i]->top_poc) {
                    struct decoded_frame *tmp = ordered[i];
                    ordered[i] = ordered[j];
                    ordered[j] = tmp;
                }
            }
        }
        for (i = 0; i < n && refs < TG_NVDEC_MAX_DPB; i++) {
            struct tg_nvdec_h264_ref *r = &p->dpb[refs++];
            r->slot = ordered[i]->slot;
            r->frame_num = ordered[i]->long_term ? ordered[i]->long_term_idx
                                                 : ordered[i]->frame_num;
            r->top_poc = ordered[i]->top_poc;
            r->bottom_poc = ordered[i]->bottom_poc;
            r->is_long_term = ordered[i]->long_term;
            r->not_existing = false;
            r->is_field = false;
        }
        p->dpb_count = (uint8_t)refs;
    }

    if (sh->nal_ref_idc) {
        struct decoded_frame *d = NULL;
        for (i = 0; i < TG_NVDEC_MAX_SURFACES; i++) {
            if (!ctx->dpb[i].valid) {
                d = &ctx->dpb[i];
                break;
            }
        }
        if (!d)
            return false;
        d->valid = true;
        d->ref = true;
        d->long_term = sh->long_term_reference_flag;
        d->slot = slot;
        d->frame_num = sh->frame_num;
        d->long_term_idx = 0;
        d->top_poc = top;
        d->bottom_poc = bottom;
    }
    enforce_sliding_window(ctx, sps->max_num_ref_frames);
    return true;
}

static bool finalize_frame(struct parse_ctx *ctx, struct frame_build *b,
                           struct tg_nvdec_parsed_frame *frame,
                           uint8_t *slice_data, size_t *used, size_t capacity,
                           char *err, size_t err_size)
{
    static const uint8_t eos[16] = {
        0, 0, 1, 0x0b, 0, 0, 0, 0,
        0, 0, 1, 0x0b, 0, 0, 0, 0,
    };
    uint32_t slot;

    if (*used + sizeof(eos) > capacity) {
        put_err(err, err_size, "slice buffer is too small for the end marker");
        return false;
    }
    memcpy(slice_data + *used, eos, sizeof(eos));
    *used += sizeof(eos);
    b->slice_offsets[b->slice_count] = (uint32_t)(*used - b->bitstream_offset);
    frame->bitstream_offset = b->bitstream_offset;
    frame->bitstream_size = (uint32_t)(*used - b->bitstream_offset);
    frame->slice_count = b->slice_count;
    memcpy(frame->slice_offsets, b->slice_offsets, sizeof(frame->slice_offsets));
    frame->idr = b->sh.idr;
    frame->reference = b->sh.nal_ref_idc != 0;

    slot = alloc_slot(ctx);
    if (slot >= TG_NVDEC_MAX_SURFACES)
        return false;
    return fill_picture(ctx, &ctx->sps[b->sps_id], &ctx->pps[b->pps_id],
                        &b->sh, frame, slot);
}

uint32_t tg_nvdec_parse_annexb(const uint8_t *data, size_t size,
                               uint8_t *slice_data, size_t slice_capacity,
                               struct tg_nvdec_parsed_frame *frames,
                               uint32_t max_frames,
                               struct tg_nvdec_stream_info *info,
                               char *err, size_t err_size)
{
    struct parse_ctx ctx;
    struct frame_build build;
    bool have_build = false;
    size_t pos = 0, used = 0;
    uint32_t frame_count = 0;
    bool saw_b = false;
    uint32_t i;

    if (info)
        memset(info, 0, sizeof(*info));
    if (!data || !size || !slice_data || !frames || !max_frames) {
        put_err(err, err_size, "invalid parser arguments");
        return 0;
    }
    if (max_frames > PARSE_MAX_FRAMES)
        max_frames = PARSE_MAX_FRAMES;
    memset(&ctx, 0, sizeof(ctx));
    memset(&build, 0, sizeof(build));

    while (pos + 3 <= size) {
        const uint8_t *nal = NULL;
        size_t nal_size = 0;
        uint32_t nal_type;

        if (!next_nal(data, size, &pos, &nal, &nal_size))
            break;
        if (nal_size < 2) {
            put_err(err, err_size, "truncated NAL unit");
            return 0;
        }
        nal_type = nal[0] & 0x1f;
        if (nal_type == 7) {
            struct tg_nvdec_sps sps;
            if (!parse_sps(nal, nal_size, &sps)) {
                put_err(err, err_size, "unsupported or malformed SPS (profile/scaling/POC/field)");
                return 0;
            }
            ctx.sps[sps.id] = sps;
            continue;
        }
        if (nal_type == 8) {
            struct tg_nvdec_pps pps;
            if (!parse_pps(nal, nal_size, &pps)) {
                put_err(err, err_size, "unsupported or malformed PPS (slice groups/scaling)");
                return 0;
            }
            ctx.pps[pps.id] = pps;
            continue;
        }
        if (nal_type != 1 && nal_type != 2 && nal_type != 5)
            continue;

        {
            uint8_t rbsp[64];
            struct br b;
            size_t n = rbsp_from_nal(nal + 1, nal_size - 1, rbsp, sizeof(rbsp));
            uint32_t pps_id;
            const struct tg_nvdec_pps *pps;
            const struct tg_nvdec_sps *sps;
            struct slice_header sh;

            br_init(&b, rbsp, n);
            (void)br_ue(&b);
            (void)br_ue(&b);
            pps_id = br_ue(&b);
            if (!b.ok || pps_id >= TG_NVDEC_MAX_PPS || !ctx.pps[pps_id].valid) {
                put_err(err, err_size, "slice references a missing PPS");
                return 0;
            }
            pps = &ctx.pps[pps_id];
            sps = &ctx.sps[pps->sps_id];
            if (!sps->valid) {
                put_err(err, err_size, "PPS references a missing SPS");
                return 0;
            }
            if (!parse_slice(nal, nal_size, sps, pps, &sh)) {
                put_err(err, err_size, "unsupported or malformed slice header");
                return 0;
            }

            if (sh.slice_type % 5 == 1)
                saw_b = true;
            if (sh.first_mb == 0) {
                if (have_build) {
                    if (!finalize_frame(&ctx, &build, &frames[build.first_index],
                                        slice_data, &used, slice_capacity,
                                        err, err_size)) {
                        if (err && !err[0])
                            put_err(err, err_size, "reference/DPB state overflow");
                        return 0;
                    }
                }
                if (frame_count >= max_frames) {
                    put_err(err, err_size, "frame array is too small");
                    return 0;
                }
                memset(&build, 0, sizeof(build));
                build.sh = sh;
                build.pps_id = pps_id;
                build.sps_id = pps->sps_id;
                build.first_index = frame_count++;
                build.bitstream_offset = (uint32_t)used;
                have_build = true;
            } else if (!have_build) {
                put_err(err, err_size, "slice without a first slice");
                return 0;
            } else if (sh.frame_num != build.sh.frame_num ||
                       sh.pps_id != build.sh.pps_id ||
                       sh.slice_type % 5 != build.sh.slice_type % 5) {
                put_err(err, err_size, "multi-slice picture header mismatch");
                return 0;
            }
            if (build.slice_count >= TG_NVDEC_MAX_SLICES) {
                put_err(err, err_size, "too many slices in one picture");
                return 0;
            }
            if (used + 3u + nal_size > slice_capacity) {
                put_err(err, err_size, "slice buffer is too small");
                return 0;
            }
            build.slice_offsets[build.slice_count++] = (uint32_t)(used - build.bitstream_offset);
            slice_data[used++] = 0;
            slice_data[used++] = 0;
            slice_data[used++] = 1;
            memcpy(slice_data + used, nal, nal_size);
            used += nal_size;
        }
    }

    if (!have_build) {
        put_err(err, err_size, "no H.264 pictures found");
        return 0;
    }
    if (!finalize_frame(&ctx, &build, &frames[build.first_index],
                        slice_data, &used, slice_capacity, err, err_size)) {
        if (err && !err[0])
            put_err(err, err_size, "reference/DPB state overflow");
        return 0;
    }

    if (info) {
        const struct tg_nvdec_parsed_frame *f = &frames[0];
        const struct tg_nvdec_pps *pps = &ctx.pps[build.pps_id];
        const struct tg_nvdec_sps *sps = &ctx.sps[pps->sps_id];
        info->width = f->pic.width;
        info->height = f->pic.height;
        info->coded_width = f->pic.coded_width;
        info->coded_height = f->pic.coded_height;
        info->profile_idc = sps->profile_idc;
        info->level_idc = sps->level_idc;
        info->chroma_format_idc = f->pic.chroma_format_idc;
        info->bit_depth_luma = f->pic.bit_depth_luma;
        info->bit_depth_chroma = f->pic.bit_depth_chroma;
        info->max_num_ref_frames = sps->max_num_ref_frames;
        info->frame_count = frame_count;
        info->slice_data_bytes = (uint32_t)used;
        for (i = 0; i < frame_count; i++)
            info->total_slices += frames[i].slice_count;
        info->has_b_frame = saw_b;
        info->supported = true;
    }
    return frame_count;
}
