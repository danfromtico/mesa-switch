/* SPDX-License-Identifier: MIT
 *
 * Self-contained H.264 Annex B parser for the standalone NVDEC acceptance
 * tool.  It covers 8-bit 4:2:0 progressive frame decoding (Baseline, Main and
 * High), I/P/B pictures, multiple reference frames, cropping and multi-slice
 * pictures.  Interlaced, 4:2:2/4:4:4, >8-bit, slice groups, MVC/SVC and
 * POC type 1 are rejected explicitly instead of being guessed at.
 */
#ifndef TGD3D_NVDEC_PARSE_H
#define TGD3D_NVDEC_PARSE_H

#include "nvdec.h"

#ifdef __cplusplus
extern "C" {
#endif

struct tg_nvdec_sps {
    bool valid;
    uint32_t id;
    uint32_t profile_idc;
    uint32_t level_idc;
    uint32_t chroma_format_idc;
    uint32_t bit_depth_luma;
    uint32_t bit_depth_chroma;
    uint32_t log2_max_frame_num_minus4;
    uint32_t pic_order_cnt_type;
    uint32_t log2_max_pic_order_cnt_lsb_minus4;
    bool delta_pic_order_always_zero_flag;
    uint32_t max_num_ref_frames;
    bool frame_mbs_only_flag;
    bool mb_adaptive_frame_field_flag;
    bool direct_8x8_inference_flag;
    uint32_t pic_width_in_mbs_minus1;
    uint32_t pic_height_in_map_units_minus1;
    uint32_t crop_left, crop_right, crop_top, crop_bottom;
    uint32_t width, height;
    uint32_t coded_width, coded_height;
    bool scaling_present;
    uint8_t scaling4x4[6][16];
    uint8_t scaling8x8[2][64];
};

struct tg_nvdec_pps {
    bool valid;
    uint32_t id;
    uint32_t sps_id;
    bool entropy_coding_mode_flag;
    bool bottom_field_pic_order_in_frame_present_flag;
    uint32_t num_ref_idx_l0_default_active_minus1;
    uint32_t num_ref_idx_l1_default_active_minus1;
    bool weighted_pred_flag;
    uint32_t weighted_bipred_idc;
    int32_t pic_init_qp_minus26;
    int32_t chroma_qp_index_offset;
    bool deblocking_filter_control_present_flag;
    bool constrained_intra_pred_flag;
    bool redundant_pic_cnt_present_flag;
    bool transform_8x8_mode_flag;
    int32_t second_chroma_qp_index_offset;
    bool scaling_present;
    uint8_t scaling4x4[6][16];
    uint8_t scaling8x8[2][64];
};

struct tg_nvdec_parsed_frame {
    struct tg_nvdec_h264_pic pic;
    uint32_t bitstream_offset;   /* into the caller's slice_data buffer */
    uint32_t bitstream_size;
    uint32_t slice_count;
    uint32_t slice_offsets[TG_NVDEC_MAX_SLICES + 1];
    bool idr;
    bool reference;
};

struct tg_nvdec_stream_info {
    uint32_t width, height;
    uint32_t coded_width, coded_height;
    uint32_t profile_idc, level_idc;
    uint32_t chroma_format_idc, bit_depth_luma, bit_depth_chroma;
    uint32_t max_num_ref_frames;
    uint32_t frame_count;
    uint32_t total_slices;
    uint32_t slice_data_bytes;
    bool has_b_frame;
    bool supported;
    char reason[160];
};

/* Parse a complete Annex B stream.  Frames are written to frames[0..
 * max_frames); slice payloads (with start codes) are written to slice_data.
 * Returns the number of frames, or 0 with `err` filled in. */
uint32_t tg_nvdec_parse_annexb(const uint8_t *data, size_t size,
                               uint8_t *slice_data, size_t slice_capacity,
                               struct tg_nvdec_parsed_frame *frames,
                               uint32_t max_frames,
                               struct tg_nvdec_stream_info *info,
                               char *err, size_t err_size);

#ifdef __cplusplus
}
#endif

#endif
