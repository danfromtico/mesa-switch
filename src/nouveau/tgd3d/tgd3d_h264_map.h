/* SPDX-License-Identifier: MIT
 *
 * D3D11VA/DXVA H.264 -> NVDEC H.264 picture mapping and Annex B slice
 * assembly.  This is the portable conversion layer used by tgd3d_video.cpp;
 * it has no D3D or OS dependency so it can be checked on the host.  Only the
 * supported 8-bit 4:2:0 progressive dimensions are admitted.
 */
#ifndef TGD3D_H264_MAP_H
#define TGD3D_H264_MAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../nvdec/nvdec.h"
#include "tgd3d_dxva_h264.h"

#ifdef __cplusplus
extern "C" {
#endif

enum tgd3d_h264_flag {
    TGD3D_H264_FIELD            = 1u << 0,
    TGD3D_H264_MBAFF            = 1u << 1,
    TGD3D_H264_CHROMA_SHIFT     = 2,
    TGD3D_H264_CHROMA_MASK      = 3u << 2,
    TGD3D_H264_REF              = 1u << 4,
    TGD3D_H264_CONSTRAINED      = 1u << 5,
    TGD3D_H264_WEIGHTED_PRED    = 1u << 6,
    TGD3D_H264_WEIGHTED_BIPRED_SHIFT = 7,
    TGD3D_H264_WEIGHTED_BIPRED_MASK  = 3u << 7,
    TGD3D_H264_FRAME_MBS_ONLY   = 1u << 9,
    TGD3D_H264_TRANSFORM_8X8    = 1u << 10,
    TGD3D_H264_ENTROPY          = 1u << 11,
    TGD3D_H264_PIC_ORDER_PRESENT = 1u << 12,
    TGD3D_H264_DEBLOCK          = 1u << 13,
    TGD3D_H264_REDUNDANT        = 1u << 14,
    TGD3D_H264_DIRECT_8X8       = 1u << 15,
};

enum tgd3d_h264_ref_flag {
    TGD3D_H264_REF_LONG_TERM    = 1u << 0,
    TGD3D_H264_REF_FIELD        = 1u << 1,
    TGD3D_H264_REF_NOT_EXISTING = 1u << 2,
};

struct tgd3d_h264_ref_in {
    uint8_t slot;
    uint8_t flags;
    uint16_t frame_num;
    int32_t top_poc;
    int32_t bottom_poc;
};

struct tgd3d_h264_pic_in {
    uint16_t width_mbs_minus1;
    uint16_t height_mbs_minus1;
    uint8_t num_ref_frames; /* normalized active entries, not the DXVA SPS maximum */
    uint8_t curr_slot;
    uint16_t flags;
    uint8_t bit_depth_luma_minus8;
    uint8_t bit_depth_chroma_minus8;
    int32_t curr_poc[2];
    int8_t pic_init_qp_minus26;
    int8_t chroma_qp_index_offset;
    int8_t second_chroma_qp_index_offset;
    uint8_t num_ref_l0_minus1;
    uint8_t num_ref_l1_minus1;
    uint16_t frame_num;
    uint8_t log2_max_frame_num_minus4;
    uint8_t pic_order_cnt_type;
    uint8_t log2_max_pic_order_cnt_lsb_minus4;
    uint8_t delta_pic_order_always_zero;
    uint8_t have_scaling;
    uint8_t scaling4x4[6][16];
    uint8_t scaling8x8[2][64];
    struct tgd3d_h264_ref_in refs[TG_NVDEC_MAX_DPB];
};

struct tgd3d_h264_slice_in {
    uint32_t offset;
    uint32_t size;
};

enum tgd3d_h264_map_result {
    TGD3D_H264_MAP_OK = 0,
    TGD3D_H264_MAP_UNSUPPORTED,
    TGD3D_H264_MAP_DIMENSIONS,
    TGD3D_H264_MAP_DPB,
    TGD3D_H264_MAP_SLICE,
    TGD3D_H264_MAP_BUFFER,
};

enum tgd3d_h264_map_result tgd3d_h264_map(
    const struct tgd3d_h264_pic_in *in, uint32_t sample_width,
    uint32_t sample_height, struct tg_nvdec_h264_pic *out,
    char *err, size_t err_size);

/* DXVA public bitstream-buffer ABI. The host decoder has already resolved
 * scaling-list defaults; the matrix buffer is in scan order. Each hardware
 * slot owner is its completed reference picture index plus one, or zero. */
enum tgd3d_h264_map_result tgd3d_h264_map_dxva(
    const tgd3d_dxva_pic_h264 *pic, const tgd3d_dxva_qmatrix_h264 *matrix,
    const uint8_t owners[TG_NVDEC_MAX_SURFACES],
    uint32_t sample_width, uint32_t sample_height,
    struct tg_nvdec_h264_pic *out, char *err, size_t err_size);

enum tgd3d_h264_map_result tgd3d_h264_assemble_bitstream(
    const uint8_t *bitstream, size_t bitstream_size,
    const struct tgd3d_h264_slice_in *slices, uint32_t slice_count,
    uint8_t *out, size_t out_capacity, uint32_t *out_size,
    uint32_t *offsets, uint32_t *out_slices);

void tgd3d_h264_default_scaling(uint8_t s4[6][16], uint8_t s8[2][64]);

#ifdef __cplusplus
}
#endif

#endif
