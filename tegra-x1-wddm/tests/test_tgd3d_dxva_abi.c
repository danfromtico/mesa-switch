/* SPDX-License-Identifier: MIT
 * Compile with the public Windows SDK dxva.h, not a test replacement.
 * This validates the serialized ABI mirror; it does not run a GPU decoder.
 */
#include <windows.h>
#include <dxva.h>
#include <stddef.h>
#include "tgd3d_dxva_h264.h"

#define CHECK_OFFSET(field) _Static_assert( \
    offsetof(tgd3d_dxva_pic_h264, field) == offsetof(DXVA_PicParams_H264, field), #field)
CHECK_OFFSET(wFrameWidthInMbsMinus1);
CHECK_OFFSET(wFrameHeightInMbsMinus1);
CHECK_OFFSET(CurrPic);
CHECK_OFFSET(num_ref_frames);
CHECK_OFFSET(wBitFields);
CHECK_OFFSET(bit_depth_luma_minus8);
CHECK_OFFSET(bit_depth_chroma_minus8);
CHECK_OFFSET(Reserved16Bits);
CHECK_OFFSET(StatusReportFeedbackNumber);
CHECK_OFFSET(RefFrameList);
CHECK_OFFSET(CurrFieldOrderCnt);
CHECK_OFFSET(FieldOrderCntList);
CHECK_OFFSET(pic_init_qs_minus26);
CHECK_OFFSET(chroma_qp_index_offset);
CHECK_OFFSET(second_chroma_qp_index_offset);
CHECK_OFFSET(ContinuationFlag);
CHECK_OFFSET(pic_init_qp_minus26);
CHECK_OFFSET(num_ref_idx_l0_active_minus1);
CHECK_OFFSET(num_ref_idx_l1_active_minus1);
CHECK_OFFSET(Reserved8BitsA);
CHECK_OFFSET(FrameNumList);
CHECK_OFFSET(UsedForReferenceFlags);
CHECK_OFFSET(NonExistingFrameFlags);
CHECK_OFFSET(frame_num);
CHECK_OFFSET(log2_max_frame_num_minus4);
CHECK_OFFSET(pic_order_cnt_type);
CHECK_OFFSET(log2_max_pic_order_cnt_lsb_minus4);
CHECK_OFFSET(delta_pic_order_always_zero_flag);
CHECK_OFFSET(direct_8x8_inference_flag);
CHECK_OFFSET(entropy_coding_mode_flag);
CHECK_OFFSET(pic_order_present_flag);
CHECK_OFFSET(num_slice_groups_minus1);
CHECK_OFFSET(slice_group_map_type);
CHECK_OFFSET(deblocking_filter_control_present_flag);
CHECK_OFFSET(redundant_pic_cnt_present_flag);
CHECK_OFFSET(Reserved8BitsB);
_Static_assert(sizeof(tgd3d_dxva_pic_h264) ==
               offsetof(DXVA_PicParams_H264, slice_group_change_rate_minus1), "picture prefix");
_Static_assert(sizeof(tgd3d_dxva_pic_h264_buffer) == sizeof(DXVA_PicParams_H264),
               "complete picture parameters");
_Static_assert(offsetof(tgd3d_dxva_pic_h264_buffer, SliceGroupMap) ==
               offsetof(DXVA_PicParams_H264, SliceGroupMap), "slice group map");
_Static_assert(sizeof(tgd3d_dxva_pic_entry) == sizeof(DXVA_PicEntry_H264), "picture entry");
_Static_assert(sizeof(tgd3d_dxva_slice_h264_short) == sizeof(DXVA_Slice_H264_Short), "short slice");
_Static_assert(offsetof(tgd3d_dxva_slice_h264_short, wBadSliceChopping) ==
               offsetof(DXVA_Slice_H264_Short, wBadSliceChopping), "short slice fields");
_Static_assert(sizeof(tgd3d_dxva_qmatrix_h264) == sizeof(DXVA_Qmatrix_H264), "matrix");
