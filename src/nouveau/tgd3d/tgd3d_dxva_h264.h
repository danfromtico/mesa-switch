/* SPDX-License-Identifier: MIT
 *
 * Minimal, layout-stable mirror of the DXVA H.264 decode structures that the
 * D3D11VA DDI receives.  The WDK dxva.h is avoided here because it conflicts
 * with the D3D10/11 UMD headers in C++; only the fields the NVDEC mapping
 * needs and the full public picture-buffer footprint are declared.
 */
#ifndef TGD3D_DXVA_H264_H
#define TGD3D_DXVA_H264_H

#include <stdint.h>

/* Reserve index 127: a long-term reference to it would be the 0xff invalid entry. */
#define TGD3D_DXVA_H264_SURFACES 127u

#ifdef _MSC_VER
#pragma pack(push, 1)
#endif
typedef struct tgd3d_dxva_pic_entry {
    uint8_t bPicEntry;   /* Index7Bits in bits 0..6, AssociatedFlag in bit 7 */
} tgd3d_dxva_pic_entry;
#ifdef _MSC_VER
#pragma pack(pop)
#endif

/* Prefix of DXVA_PicParams_H264 through Reserved8BitsB.  The application's
 * buffer is larger (slice-group map); only this prefix is consumed. */
typedef struct tgd3d_dxva_pic_h264 {
    uint16_t wFrameWidthInMbsMinus1;
    uint16_t wFrameHeightInMbsMinus1;
    tgd3d_dxva_pic_entry CurrPic;
    uint8_t num_ref_frames;
    union {
        struct {
            uint16_t field_pic_flag : 1;
            uint16_t MbaffFrameFlag : 1;
            uint16_t residual_colour_transform_flag : 1;
            uint16_t sp_for_switch_flag : 1;
            uint16_t chroma_format_idc : 2;
            uint16_t RefPicFlag : 1;
            uint16_t constrained_intra_pred_flag : 1;
            uint16_t weighted_pred_flag : 1;
            uint16_t weighted_bipred_idc : 2;
            uint16_t MbsConsecutiveFlag : 1;
            uint16_t frame_mbs_only_flag : 1;
            uint16_t transform_8x8_mode_flag : 1;
            uint16_t MinLumaBipredSize8x8Flag : 1;
            uint16_t IntraPicFlag : 1;
        };
        uint16_t wBitFields;
    };
    uint8_t bit_depth_luma_minus8;
    uint8_t bit_depth_chroma_minus8;
    uint16_t Reserved16Bits;
    uint32_t StatusReportFeedbackNumber;
    tgd3d_dxva_pic_entry RefFrameList[16];
    int32_t CurrFieldOrderCnt[2];
    int32_t FieldOrderCntList[16][2];
    int8_t pic_init_qs_minus26;
    int8_t chroma_qp_index_offset;
    int8_t second_chroma_qp_index_offset;
    uint8_t ContinuationFlag;
    int8_t pic_init_qp_minus26;
    uint8_t num_ref_idx_l0_active_minus1;
    uint8_t num_ref_idx_l1_active_minus1;
    uint8_t Reserved8BitsA;
    uint16_t FrameNumList[16];
    uint32_t UsedForReferenceFlags;
    uint16_t NonExistingFrameFlags;
    uint16_t frame_num;
    uint8_t log2_max_frame_num_minus4;
    uint8_t pic_order_cnt_type;
    uint8_t log2_max_pic_order_cnt_lsb_minus4;
    uint8_t delta_pic_order_always_zero_flag;
    uint8_t direct_8x8_inference_flag;
    uint8_t entropy_coding_mode_flag;
    uint8_t pic_order_present_flag;
    uint8_t num_slice_groups_minus1;
    uint8_t slice_group_map_type;
    uint8_t deblocking_filter_control_present_flag;
    uint8_t redundant_pic_cnt_present_flag;
    uint8_t Reserved8BitsB;
} tgd3d_dxva_pic_h264;

/* The application can submit the complete DXVA_PicParams_H264 even when
 * the active PPS contains no slice groups. */
typedef struct tgd3d_dxva_pic_h264_buffer {
    tgd3d_dxva_pic_h264 picture;
    uint16_t slice_group_change_rate_minus1;
    uint8_t SliceGroupMap[810];
} tgd3d_dxva_pic_h264_buffer;

#pragma pack(push, 1)
typedef struct tgd3d_dxva_slice_h264_short {
    uint32_t BSNALunitDataLocation;
    uint32_t SliceBytesInBuffer;
    uint16_t wBadSliceChopping;
} tgd3d_dxva_slice_h264_short;
#pragma pack(pop)

typedef struct tgd3d_dxva_qmatrix_h264 {
    uint8_t bScalingLists4x4[6][16];
    uint8_t bScalingLists8x8[2][64];
} tgd3d_dxva_qmatrix_h264;

static inline uint32_t tgd3d_dxva_entry_index(const tgd3d_dxva_pic_entry *e)
{
    return e->bPicEntry & 0x7fu;
}

static inline uint32_t tgd3d_dxva_entry_flag(const tgd3d_dxva_pic_entry *e)
{
    return e->bPicEntry >> 7;
}

#endif
