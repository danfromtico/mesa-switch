/* SPDX-License-Identifier: MIT
 *
 * Host control for the DXVA/D3D11VA H.264 -> NVDEC mapping and Annex B
 * assembly.  It exercises the same conversion code the UMD video DDI uses.
 */
#include "tgd3d_h264_map.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

static void base_720(struct tgd3d_h264_pic_in *in)
{
    memset(in, 0, sizeof(*in));
    in->width_mbs_minus1 = 79;   /* 1280 */
    in->height_mbs_minus1 = 44;  /* 720 */
    in->num_ref_frames = 2;
    in->curr_slot = 2;
    in->flags = TGD3D_H264_FRAME_MBS_ONLY | TGD3D_H264_REF |
                TGD3D_H264_DIRECT_8X8 | TGD3D_H264_DEBLOCK |
                (1u << TGD3D_H264_CHROMA_SHIFT);
    in->log2_max_frame_num_minus4 = 4;
    in->log2_max_pic_order_cnt_lsb_minus4 = 4;
    in->pic_order_cnt_type = 0;
    in->frame_num = 5;
    in->curr_poc[0] = 10;
    in->curr_poc[1] = 10;
    in->num_ref_l0_minus1 = 1;
    in->num_ref_l1_minus1 = 0;
    in->refs[0].slot = 0;
    in->refs[0].frame_num = 3;
    in->refs[0].top_poc = 6;
    in->refs[0].bottom_poc = 6;
    in->refs[1].slot = 1;
    in->refs[1].frame_num = 4;
    in->refs[1].top_poc = 8;
    in->refs[1].bottom_poc = 8;
    in->refs[1].flags = TGD3D_H264_REF_LONG_TERM;
}

static void test_map(void)
{
    struct tgd3d_h264_pic_in in;
    struct tg_nvdec_h264_pic pic;
    char err[160] = {0};

    {
        char tiny[3] = {'L', 'x', 'R'};
        CHECK(tgd3d_h264_map(NULL, 1280, 720, &pic, tiny + 1, 1) ==
              TGD3D_H264_MAP_BUFFER);
        CHECK(tiny[0] == 'L' && tiny[1] == 0 && tiny[2] == 'R');
        base_720(&in);
        CHECK(tgd3d_h264_map(&in, 1280, 720, NULL, NULL, 0) ==
              TGD3D_H264_MAP_BUFFER);
    }
    base_720(&in);
    CHECK(tgd3d_h264_map(&in, 1280, 720, &pic, err, sizeof(err)) == TGD3D_H264_MAP_OK);
    CHECK(pic.width == 1280 && pic.height == 720);
    CHECK(pic.coded_width == 1280 && pic.coded_height == 720);
    CHECK(pic.chroma_format_idc == 1 && pic.bit_depth_luma == 8);
    CHECK(pic.curr_slot == 2 && pic.frame_num == 5);
    CHECK(pic.curr_top_poc == 10 && pic.curr_bottom_poc == 10);
    CHECK(pic.dpb_count == 2);
    CHECK(pic.dpb[0].slot == 0 && pic.dpb[1].slot == 1);
    CHECK(!pic.dpb[0].is_long_term && pic.dpb[1].is_long_term);
    CHECK(pic.frame_mbs_only_flag && pic.direct_8x8_inference_flag);
    CHECK(pic.ref_pic_flag && pic.entropy_coding_mode_flag == false);
    CHECK(pic.deblocking_filter_control_present_flag);
    CHECK(pic.scaling4x4[0][0] == 6);

    in.have_scaling = 1;
    memset(in.scaling4x4, 16, sizeof(in.scaling4x4));
    memset(in.scaling8x8, 16, sizeof(in.scaling8x8));
    CHECK(tgd3d_h264_map(&in, 1280, 720, &pic, err, sizeof(err)) == TGD3D_H264_MAP_OK);
    CHECK(pic.scaling4x4[0][0] == 16 && pic.scaling8x8[0][0] == 16);

    base_720(&in);
    in.width_mbs_minus1 = 239;   /* 3840 */
    in.height_mbs_minus1 = 134;  /* 2160 */
    CHECK(tgd3d_h264_map(&in, 3840, 2160, &pic, err, sizeof(err)) ==
          TGD3D_H264_MAP_UNSUPPORTED);
    CHECK(err[0] != 0);

    base_720(&in);
    in.bit_depth_luma_minus8 = 2;
    CHECK(tgd3d_h264_map(&in, 1280, 720, &pic, err, sizeof(err)) ==
          TGD3D_H264_MAP_UNSUPPORTED);
    base_720(&in);
    in.flags = (in.flags & ~TGD3D_H264_CHROMA_MASK) |
               (2u << TGD3D_H264_CHROMA_SHIFT);
    CHECK(tgd3d_h264_map(&in, 1280, 720, &pic, err, sizeof(err)) ==
          TGD3D_H264_MAP_UNSUPPORTED);
    base_720(&in);
    in.flags |= TGD3D_H264_FIELD;
    CHECK(tgd3d_h264_map(&in, 1280, 720, &pic, err, sizeof(err)) ==
          TGD3D_H264_MAP_UNSUPPORTED);
    base_720(&in);
    in.curr_slot = TG_NVDEC_MAX_SURFACES;
    CHECK(tgd3d_h264_map(&in, 1280, 720, &pic, err, sizeof(err)) ==
          TGD3D_H264_MAP_DPB);
    base_720(&in);
    in.refs[1].slot = in.curr_slot;
    CHECK(tgd3d_h264_map(&in, 1280, 720, &pic, err, sizeof(err)) ==
          TGD3D_H264_MAP_DPB);
    base_720(&in);
    in.refs[1].slot = in.refs[0].slot;
    CHECK(tgd3d_h264_map(&in, 1280, 720, &pic, err, sizeof(err)) ==
          TGD3D_H264_MAP_DPB);
    base_720(&in);
    CHECK(tgd3d_h264_map(&in, 1278, 718, &pic, err, sizeof(err)) ==
          TGD3D_H264_MAP_OK);
    CHECK(pic.width == 1278 && pic.height == 718);
    base_720(&in);
    CHECK(tgd3d_h264_map(&in, 1279, 720, &pic, err, sizeof(err)) ==
          TGD3D_H264_MAP_DIMENSIONS);
}

static void test_assemble(void)
{
    static const uint8_t bitstream[] = {
        0, 0, 1, 0x65, 0xaa,
        0, 0, 0, 1, 0x41, 0xbb, 0xcc,
        0x65, 0xdd,
    };
    const struct tgd3d_h264_slice_in slices[3] = {
        { 0, 5 }, { 5, 7 }, { 12, 2 },
    };
    uint8_t out[128];
    uint32_t offsets[4] = {0};
    uint32_t size = 0, out_slices = 0;
    static const uint8_t expected[] = {
        0, 0, 1, 0x65, 0xaa,
        0, 0, 1, 0x41, 0xbb, 0xcc,
        0, 0, 1, 0x65, 0xdd,
        0, 0, 1, 0x0b, 0, 0, 0, 0,
        0, 0, 1, 0x0b, 0, 0, 0, 0,
    };

    CHECK(tgd3d_h264_assemble_bitstream(bitstream, sizeof(bitstream), slices, 3,
                                        out, sizeof(out), &size, offsets,
                                        &out_slices) == TGD3D_H264_MAP_OK);
    CHECK(out_slices == 3);
    CHECK(size == sizeof(expected));
    CHECK(memcmp(out, expected, size) == 0);
    CHECK(offsets[0] == 0 && offsets[1] == 5 && offsets[2] == 11);
    CHECK(offsets[3] == size);

    CHECK(tgd3d_h264_assemble_bitstream(bitstream, sizeof(bitstream), slices, 3,
                                        out, 10, &size, offsets,
                                        &out_slices) == TGD3D_H264_MAP_BUFFER);
    {
        const struct tgd3d_h264_slice_in bad[1] = { { 0, 6 } };
        CHECK(tgd3d_h264_assemble_bitstream(bitstream, 5, bad, 1, out,
                                            sizeof(out), &size, offsets,
                                            &out_slices) == TGD3D_H264_MAP_SLICE);
    }
    CHECK(tgd3d_h264_assemble_bitstream(bitstream, sizeof(bitstream), slices, 0,
                                        out, sizeof(out), &size, offsets,
                                        &out_slices) == TGD3D_H264_MAP_SLICE);
    CHECK(tgd3d_h264_assemble_bitstream(NULL, 0, slices, 1, out, sizeof(out),
                                        &size, offsets, &out_slices) ==
          TGD3D_H264_MAP_SLICE);
}

static void test_dxva_boundary(void)
{
    uint8_t owners[TG_NVDEC_MAX_SURFACES] = {[1] = 2};
    tgd3d_dxva_pic_h264 p = {0};
    tgd3d_dxva_qmatrix_h264 matrix;
    struct tg_nvdec_h264_pic out;
    char error[160];
    memset(&matrix, 16, sizeof(matrix));
    p.wFrameWidthInMbsMinus1 = 79;
    p.wFrameHeightInMbsMinus1 = 44;
    p.CurrPic.bPicEntry = 16;
    p.wBitFields = 0x1050; /* frame_mbs_only, RefPicFlag, 4:2:0 */
    p.num_ref_frames = 16; /* SPS maximum; IDR has zero active entries */
    memset(p.RefFrameList, 0xff, sizeof(p.RefFrameList));
    CHECK(tgd3d_h264_map_dxva(&p, &matrix, owners, 1280, 720, &out, error, sizeof(error)) ==
          TGD3D_H264_MAP_UNSUPPORTED);
    p.ContinuationFlag = 1;
    CHECK(tgd3d_h264_map_dxva(&p, &matrix, owners, 1280, 720, &out, error, sizeof(error)) ==
          TGD3D_H264_MAP_OK);
    CHECK(out.dpb_count == 0 && out.curr_slot == 16 && out.ref_pic_flag);
    CHECK(out.scaling4x4[0][0] == 16);

    /* A valid reference may occur after unused entries. Its picture metadata
     * remains paired with that entry rather than with the compacted index. */
    p.RefFrameList[7].bPicEntry = 0x81;
    p.UsedForReferenceFlags = 3u << 14;
    p.FrameNumList[7] = 21;
    p.FieldOrderCntList[7][0] = 36;
    p.FieldOrderCntList[7][1] = 38;
    CHECK(tgd3d_h264_map_dxva(&p, &matrix, owners, 1280, 720, &out, error, sizeof(error)) ==
          TGD3D_H264_MAP_OK);
    CHECK(out.dpb_count == 1 && out.dpb[0].slot == 1);
    CHECK(out.dpb[0].frame_num == 21 && out.dpb[0].top_poc == 36 &&
          out.dpb[0].bottom_poc == 38);
    CHECK(out.dpb[0].is_long_term && !out.dpb[0].is_field);

    p.UsedForReferenceFlags = 1u << 14;
    CHECK(tgd3d_h264_map_dxva(&p, &matrix, owners, 1280, 720, &out, NULL, 0) ==
          TGD3D_H264_MAP_UNSUPPORTED);
    p.UsedForReferenceFlags = 3u << 14;
    p.NonExistingFrameFlags = 1u << 7;
    CHECK(tgd3d_h264_map_dxva(&p, &matrix, owners, 1280, 720, &out, NULL, 0) ==
          TGD3D_H264_MAP_UNSUPPORTED);
    p.NonExistingFrameFlags = 0;
    p.RefFrameList[7].bPicEntry = 0xff;
    CHECK(tgd3d_h264_map_dxva(&p, &matrix, owners, 1280, 720, &out, NULL, 0) ==
          TGD3D_H264_MAP_DPB);
    p.UsedForReferenceFlags = 0;
    CHECK(tgd3d_h264_map_dxva(&p, NULL, owners, 1280, 720, &out, NULL, 0) ==
          TGD3D_H264_MAP_BUFFER);

    /* Standard AVC zig-zag sequence numbered1..N must reach raster layout.
     * These expected raster rows are independent of the conversion loop. */
    for (unsigned i = 0; i < 16; i++) matrix.bScalingLists4x4[0][i] = (uint8_t)(i + 1);
    for (unsigned i = 0; i < 64; i++) matrix.bScalingLists8x8[0][i] = (uint8_t)(i + 1);
    CHECK(tgd3d_h264_map_dxva(&p, &matrix, owners, 1280, 720, &out, NULL, 0) ==
          TGD3D_H264_MAP_OK);
    const uint8_t raster4[16] = {1,2,6,7,3,5,8,13,4,9,12,14,10,11,15,16};
    const uint8_t raster8_first[8] = {1,2,6,7,15,16,28,29};
    const uint8_t raster8_last[8] = {36,37,49,50,58,59,63,64};
    CHECK(memcmp(out.scaling4x4[0], raster4, sizeof(raster4)) == 0);
    CHECK(memcmp(out.scaling8x8[0], raster8_first, sizeof(raster8_first)) == 0);
    CHECK(memcmp(out.scaling8x8[0] + 56, raster8_last, sizeof(raster8_last)) == 0);
    matrix.bScalingLists8x8[0][32] = 0;
    CHECK(tgd3d_h264_map_dxva(&p, &matrix, owners, 1280, 720, &out, NULL, 0) ==
          TGD3D_H264_MAP_BUFFER);
}

static void test_1080(void)
{
    struct tgd3d_h264_pic_in in;
    struct tg_nvdec_h264_pic pic;
    char err[160] = {0};

    base_720(&in);
    in.width_mbs_minus1 = 119;   /* 1920 */
    in.height_mbs_minus1 = 67;   /* 1088 coded for 1080 visible */
    in.num_ref_frames = 4;
    in.curr_slot = 4;
    in.refs[2].slot = 2;
    in.refs[2].frame_num = 2;
    in.refs[2].top_poc = 4;
    in.refs[2].bottom_poc = 4;
    in.refs[3].slot = 3;
    in.refs[3].frame_num = 3;
    in.refs[3].top_poc = 6;
    in.refs[3].bottom_poc = 6;
    CHECK(tgd3d_h264_map(&in, 1920, 1080, &pic, err, sizeof(err)) ==
          TGD3D_H264_MAP_OK);
    CHECK(pic.width == 1920 && pic.height == 1080);
    CHECK(pic.coded_width == 1920 && pic.coded_height == 1088);
    CHECK(pic.dpb_count == 4 && pic.curr_slot == 4);
    CHECK(tgd3d_h264_map(&in, 1920, 1088, &pic, err, sizeof(err)) ==
          TGD3D_H264_MAP_OK);
    CHECK(pic.height == 1088 && pic.coded_height == 1088);
    CHECK(!tg_nvdec_stage1_supported(1920, 1080, 1920, 1072, 1, 8));
    CHECK(!tg_nvdec_stage1_supported(1920, 1080, 1920, 1088, 1, 10));
    CHECK(tg_nvdec_stage1_supported(1920, 1080, 1920, 1088, 1, 8));
    CHECK(!tg_nvdec_stage1_supported(3840, 2160, 3840, 2160, 1, 8));
}

static void test_larger_output_pool(void)
{
    uint8_t owners[TG_NVDEC_MAX_SURFACES] = {0};
    tgd3d_dxva_qmatrix_h264 matrix;
    struct tg_nvdec_h264_pic out;
    memset(&matrix, 16, sizeof(matrix));
    tgd3d_dxva_pic_h264 base = {0};
    base.wFrameWidthInMbsMinus1 = 79;
    base.wFrameHeightInMbsMinus1 = 44;
    base.ContinuationFlag = 1;
    base.wBitFields = 0x1050;
    base.num_ref_frames = 16;
    /* Keep 16 references live while a 20-slice client pool rotates. Check
     * each mapping against the actual decoded picture retained in that slot. */
    for (unsigned frame = 0; frame < 400; frame++) {
        tgd3d_dxva_pic_h264 pic = base;
        unsigned logical = frame % 20;
        unsigned refs = frame < TG_NVDEC_MAX_DPB ? frame : TG_NVDEC_MAX_DPB;
        pic.CurrPic.bPicEntry = logical;
        memset(pic.RefFrameList, 0xff, sizeof(pic.RefFrameList));
        for (unsigned i = 0; i < refs; i++) {
            pic.RefFrameList[i].bPicEntry = (frame - 1 - i) % 20;
            if (i == 0) pic.RefFrameList[i].bPicEntry |= 0x80;
            pic.UsedForReferenceFlags |= 3u << (2 * i);
        }
        bool ok = tgd3d_h264_map_dxva(&pic, &matrix, owners, 1280, 720, &out, NULL, 0) == TGD3D_H264_MAP_OK;
        CHECK(ok);
        if (!ok) return;
        unsigned target = out.curr_slot;
        CHECK(target < TG_NVDEC_MAX_SURFACES);
        for (unsigned i = 0; i < refs; i++) {
            unsigned slot = out.dpb[i].slot;
            CHECK(slot != target);
            CHECK(slot < TG_NVDEC_MAX_SURFACES && owners[slot] == 1 + (frame - 1 - i) % 20);
            CHECK(out.dpb[i].is_long_term == (i == 0));
        }
        if (target < TG_NVDEC_MAX_SURFACES)
            owners[target] = logical + 1;
    }
    tgd3d_dxva_pic_h264 pic = base;
    pic.CurrPic.bPicEntry = 63;
    pic.RefFrameList[15].bPicEntry = 61;
    pic.UsedForReferenceFlags = 3u << 30;
    CHECK(tgd3d_h264_map_dxva(&pic, &matrix, owners, 1280, 720, &out, NULL, 0) == TGD3D_H264_MAP_DPB);
    pic.RefFrameList[15].bPicEntry = 63;
    CHECK(tgd3d_h264_map_dxva(&pic, &matrix, owners, 1280, 720, &out, NULL, 0) == TGD3D_H264_MAP_DPB);
    pic.RefFrameList[15].bPicEntry = 0xff;
    CHECK(tgd3d_h264_map_dxva(&pic, &matrix, owners, 1280, 720, &out, NULL, 0) == TGD3D_H264_MAP_DPB);
    pic.UsedForReferenceFlags = 0;
    pic.CurrPic.bPicEntry = 0x80;
    CHECK(tgd3d_h264_map_dxva(&pic, &matrix, owners, 1280, 720, &out, NULL, 0) == TGD3D_H264_MAP_UNSUPPORTED);
    pic.CurrPic.bPicEntry = 127;
    CHECK(tgd3d_h264_map_dxva(&pic, &matrix, owners, 1280, 720, &out, NULL, 0) == TGD3D_H264_MAP_DPB);
    pic.CurrPic.bPicEntry = 126;
    CHECK(tgd3d_h264_map_dxva(&pic, &matrix, owners, 1280, 720, &out, NULL, 0) == TGD3D_H264_MAP_OK);
}

int main(void)
{
    test_map();
    test_1080();
    test_assemble();
    test_dxva_boundary();
    test_larger_output_pool();
    if (failures) {
        fprintf(stderr, "tgd3d H.264 map control: %d failure(s)\n", failures);
        return 1;
    }
    printf("tgd3d DXVA->NVDEC H.264 map and Annex B assembly: PASS (CPU control only)\n");
    return 0;
}
