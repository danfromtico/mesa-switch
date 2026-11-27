/* SPDX-License-Identifier: MIT
 *
 * Host tests for the NVDEC H.264 picture setup, method stream and the
 * self-contained Annex B parser.  These are command/parse controls; they do
 * not claim a hardware decode.
 */
#include "nvdec.h"
#include "nvdec_parse.h"
#include "nvidia/video/nvdec_drv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

static uint8_t *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    uint8_t *p;
    long n;

    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) {
        fclose(f);
        return NULL;
    }
    p = malloc((size_t)n);
    if (!p) {
        fclose(f);
        return NULL;
    }
    if (fread(p, 1, (size_t)n, f) != (size_t)n) {
        free(p);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *size = (size_t)n;
    return p;
}

static void make_fake_surfaces(struct tg_nvdec_surface s[TG_NVDEC_MAX_SURFACES])
{
    uint32_t i;
    memset(s, 0, sizeof(struct tg_nvdec_surface) * TG_NVDEC_MAX_SURFACES);
    for (i = 0; i < TG_NVDEC_MAX_SURFACES; i++) {
        s[i].valid = true;
        s[i].slot = i;
        s[i].luma_bus = 0x10000000ull + (uint64_t)i * 0x00200000ull;
        s[i].chroma_bus = s[i].luma_bus + 0x00100000ull;
    }
}

static void test_config(void)
{
    struct tg_nvdec_h264_pic pic;
    uint8_t config[4096];
    const nvdec_h264_pic_s *c = (const nvdec_h264_pic_s *)config;
    size_t used;

    memset(&pic, 0, sizeof(pic));
    pic.width = 174;
    pic.height = 142;
    pic.coded_width = 176;
    pic.coded_height = 144;
    pic.pitch_luma = 256;
    pic.pitch_chroma = 256;
    pic.chroma_format_idc = 1;
    pic.bit_depth_luma = 8;
    pic.bit_depth_chroma = 8;
    pic.log2_max_frame_num_minus4 = 4;
    pic.log2_max_pic_order_cnt_lsb_minus4 = 4;
    pic.pic_order_cnt_type = 0;
    pic.frame_mbs_only_flag = true;
    pic.direct_8x8_inference_flag = true;
    pic.constrained_intra_pred_flag = false;
    pic.ref_pic_flag = true;
    pic.entropy_coding_mode_flag = false;
    pic.pic_order_present_flag = false;
    pic.deblocking_filter_control_present_flag = true;
    pic.lossless_ipred8x8_filter_enable = true;
    pic.frame_num = 3;
    pic.curr_slot = 2;
    pic.curr_top_poc = 12;
    pic.curr_bottom_poc = 12;
    pic.num_ref_idx_l0_active_minus1 = 1;
    pic.dpb_count = 2;
    pic.dpb[0].slot = 0;
    pic.dpb[0].frame_num = 1;
    pic.dpb[0].top_poc = 4;
    pic.dpb[0].bottom_poc = 4;
    pic.dpb[1].slot = 1;
    pic.dpb[1].frame_num = 2;
    pic.dpb[1].top_poc = 8;
    pic.dpb[1].bottom_poc = 8;

    used = tg_nvdec_build_pic_config(config, sizeof(config), &pic, 1234, 2, 1024, 32768);
    CHECK(used == tg_nvdec_pic_config_size());
    CHECK(used == sizeof(nvdec_h264_pic_s));
    CHECK(c->PicWidthInMbs == 11);
    CHECK(c->FrameHeightInMbs == 9);
    CHECK(c->stream_len == 1234);
    CHECK(c->slice_count == 2);
    CHECK(c->pitch_luma == 256);
    CHECK(c->pitch_chroma == 256);
    CHECK(c->CurrPicIdx == 2);
    CHECK(c->CurrColIdx == 2);
    CHECK(c->frame_num == 3);
    CHECK(c->CurrFieldOrderCnt[0] == 12);
    CHECK(c->CurrFieldOrderCnt[1] == 12);
    CHECK(c->dpb[0].index == 0 && c->dpb[1].index == 1);
    CHECK(c->dpb[0].state == 3 && c->dpb[1].state == 3);
    CHECK(c->dpb[0].FrameIdx == 1 && c->dpb[1].FrameIdx == 2);
    CHECK(c->HistBufferSize == 1024);
    CHECK(c->mbhist_buffer_size == 32768);
    CHECK(c->tileFormat == 0);
    CHECK(c->output_memory_layout == 0);

    /* Unsupported and inconsistent pictures are refused. */
    CHECK(tg_nvdec_build_pic_config(config, sizeof(config), &pic, 1, 1, 1, 1) == used);
    {
        struct tg_nvdec_h264_pic bad = pic;
        bad.chroma_format_idc = 2;
        CHECK(tg_nvdec_build_pic_config(config, sizeof(config), &bad, 1, 1, 1, 1) == 0);
        bad = pic;
        bad.bit_depth_luma = 10;
        CHECK(tg_nvdec_build_pic_config(config, sizeof(config), &bad, 1, 1, 1, 1) == 0);
        bad = pic;
        bad.curr_slot = TG_NVDEC_MAX_SURFACES;
        CHECK(tg_nvdec_build_pic_config(config, sizeof(config), &bad, 1, 1, 1, 1) == 0);
        bad = pic;
        bad.dpb_count = TG_NVDEC_MAX_DPB + 1;
        CHECK(tg_nvdec_build_pic_config(config, sizeof(config), &bad, 1, 1, 1, 1) == 0);
        bad = pic;
        bad.dpb[0].slot = 99;
        CHECK(tg_nvdec_build_pic_config(config, sizeof(config), &bad, 1, 1, 1, 1) == 0);
    }
    CHECK(tg_nvdec_build_pic_config(config, 4, &pic, 1, 1, 1, 1) == 0);
}

static void test_command(void)
{
    struct tg_nvdec_cmd_buffers b;
    uint32_t cmd[256];
    size_t n;

    memset(&b, 0, sizeof(b));
    b.config = 0x80001000;
    b.bitstream = 0x80002000;
    b.slice_offsets = 0x80003000;
    b.coloc = 0x80004000;
    b.history = 0x80100000;
    b.mbhist = 0x80140000;
    b.status = 0x80148000;
    b.stream_len = 1234;
    b.slice_count = 2;
    b.history_size_256 = 1024;
    b.mbhist_size = 32768;
    b.picture_index = 9;
    b.syncpt_id = 5;
    b.surfaces[0].valid = true;
    b.surfaces[0].luma = 0x90000000;
    b.surfaces[0].chroma = 0x90010000;
    b.surfaces[3].valid = true;
    b.surfaces[3].luma = 0x90020000;
    b.surfaces[3].chroma = 0x90030000;

    n = tg_nvdec_build_h264_cmd(cmd, 256, &b);
    CHECK(n > 0);
    CHECK(n == 47);
    CHECK(cmd[0] == ((1u << 28) | (0x10u << 16) | 2u));
    CHECK(cmd[1] == 0x200u / 4u);
    CHECK(cmd[2] == 3);
    CHECK(cmd[3] == ((1u << 28) | (0x10u << 16) | 2u));
    CHECK(cmd[4] == 0x400u / 4u);
    CHECK(cmd[5] == 0x53);
    CHECK(cmd[6] == ((1u << 28) | (0x10u << 16) | 2u));
    CHECK(cmd[7] == 0x404u / 4u);
    CHECK(cmd[8] == (0x80001000u >> 8));
    CHECK(cmd[9] == ((1u << 28) | (0x10u << 16) | 2u));
    CHECK(cmd[10] == 0x408u / 4u);
    CHECK(cmd[11] == (0x80002000u >> 8));
    CHECK(cmd[12] == ((1u << 28) | (0x10u << 16) | 2u));
    CHECK(cmd[13] == 0x410u / 4u);
    CHECK(cmd[14] == (0x80003000u >> 8));
    /* Only slots 0 and 3 have surfaces: four surface methods. */
    CHECK(cmd[15] == ((1u << 28) | (0x10u << 16) | 2u));
    CHECK(cmd[16] == 0x430u / 4u);
    CHECK(cmd[17] == (0x90000000u >> 8));
    CHECK(cmd[18] == ((1u << 28) | (0x10u << 16) | 2u));
    CHECK(cmd[19] == 0x474u / 4u);
    CHECK(cmd[20] == (0x90010000u >> 8));
    CHECK(cmd[21] == ((1u << 28) | (0x10u << 16) | 2u));
    CHECK(cmd[22] == 0x43cu / 4u);
    CHECK(cmd[23] == (0x90020000u >> 8));
    CHECK(cmd[24] == ((1u << 28) | (0x10u << 16) | 2u));
    CHECK(cmd[25] == 0x480u / 4u);
    CHECK(cmd[26] == (0x90030000u >> 8));
    CHECK(cmd[30] == ((1u << 28) | (0x10u << 16) | 2u));
    CHECK(cmd[31] == 0x418u / 4u);
    CHECK(cmd[32] == (0x80100000u >> 8));
    CHECK(cmd[36] == ((1u << 28) | (0x10u << 16) | 2u));
    CHECK(cmd[37] == 0x424u / 4u);
    CHECK(cmd[38] == (0x80148000u >> 8));
    CHECK(cmd[39] == ((1u << 28) | (0x10u << 16) | 2u));
    CHECK(cmd[40] == 0x40cu / 4u);
    CHECK(cmd[41] == 9);
    CHECK(cmd[42] == ((1u << 28) | (0x10u << 16) | 2u));
    CHECK(cmd[43] == 0x300u / 4u);
    CHECK(cmd[44] == 0x100);
    CHECK(cmd[45] == ((2u << 28) | 1u));
    CHECK(cmd[46] == (5u | (1u << 8)));
    CHECK(cmd[n - 2] == ((2u << 28) | 1u));
    CHECK(cmd[n - 1] == (5u | (1u << 8)));

    /* Relocations in the NVIDIA Tegra backend use shift=8, not byte units.
     * Reject values which would be truncated by this hardware encoding. */
    {
        struct tg_nvdec_cmd_buffers bad = b;
        bad.config |= 1;
        CHECK(tg_nvdec_build_h264_cmd(cmd, 256, &bad) == 0);
        bad = b;
        bad.surfaces[0].chroma |= 128;
        CHECK(tg_nvdec_build_h264_cmd(cmd, 256, &bad) == 0);
        bad = b;
        bad.status = UINT64_C(1) << 40;
        CHECK(tg_nvdec_build_h264_cmd(cmd, 256, &bad) == 0);
    }
    CHECK(tg_nvdec_build_h264_cmd(cmd, 10, &b) == 0);
    b.status = 0;
    CHECK(tg_nvdec_build_h264_cmd(cmd, 256, &b) == 0);
    b.status = 0x80148000;
    b.slice_count = 0;
    CHECK(tg_nvdec_build_h264_cmd(cmd, 256, &b) == 0);
    b.slice_count = 2;
    /* T210 has192 counters with an8-bit UCLASS index. In particular the
     * production reservation191 must survive encoding without truncation. */
    const uint32_t valid_syncpts[] = {0u,31u,32u,191u};
    for (size_t s=0;s<sizeof(valid_syncpts)/sizeof(valid_syncpts[0]);++s) {
        b.syncpt_id=valid_syncpts[s];
        CHECK(tg_nvdec_build_h264_cmd(cmd, 256, &b) == n);
        CHECK(cmd[n-2] == 0x20000001u && cmd[n-1] == (0x100u|valid_syncpts[s]));
    }
    const uint32_t invalid_syncpts[] = {192u,255u,256u,UINT32_MAX};
    for (size_t s=0;s<sizeof(invalid_syncpts)/sizeof(invalid_syncpts[0]);++s) {
        b.syncpt_id=invalid_syncpts[s];
        CHECK(tg_nvdec_build_h264_cmd(cmd, 256, &b) == 0);
    }
}

/* Independent byte-coordinate reference for Tegra's16Bx2 sectors, two
 * GOBs high. This is a layout fixture, never an NVDEC completion substitute. */
static size_t tbl_address(uint32_t pitch, uint32_t x, uint32_t y)
{
    return (size_t)(y / 16u) * pitch * 16u + (size_t)(x / 64u) * 1024u +
           ((y & 8u) << 6) + ((x & 32u) << 3) + ((y & 6u) << 5) +
           ((x & 16u) << 1) + ((y & 1u) << 4) + (x & 15u);
}

static void test_nv12_layout(void)
{
    struct tg_nvdec_nv12_storage l;
    CHECK(tbl_address(1280,0,0)==0 && tbl_address(1280,16,0)==32);
    CHECK(tbl_address(1280,32,0)==256 && tbl_address(1280,0,1)==16);
    CHECK(tbl_address(1280,0,8)==512 && tbl_address(1280,64,0)==1024);
    CHECK(tbl_address(1280,0,16)==20480 && tbl_address(1280,1279,359)==470527);
    CHECK(tg_nvdec_nv12_storage_layout(1280,720,&l));
    CHECK(l.chroma_offset==921600 && l.linear_bytes==1382400 && l.tiled_bytes==1392640);
    CHECK(l.chroma_offset+tbl_address(1280,1279,359) >= l.linear_bytes);
    CHECK(tg_nvdec_nv12_storage_layout(2048,1088,&l));
    CHECK(l.chroma_offset==2228224 && l.linear_bytes==3342336 && l.tiled_bytes==3342336);
    CHECK(!tg_nvdec_nv12_storage_layout(0,720,&l));
    CHECK(!tg_nvdec_nv12_storage_layout(1279,720,&l));
    CHECK(!tg_nvdec_nv12_storage_layout(1280,719,&l));
    CHECK(!tg_nvdec_nv12_storage_layout(1280,720,NULL));
    CHECK(!tg_nvdec_nv12_storage_layout(0xffffffc0u,0xfffffff0u,&l));
    const uint32_t cases[][2]={{64,16},{256,32},{768,368},{1280,720},{2048,1088}};
    for (size_t c=0;c<sizeof(cases)/sizeof(cases[0]);++c) {
        const uint32_t pitch=cases[c][0], height=cases[c][1];
        CHECK(tg_nvdec_nv12_storage_layout(pitch,height,&l));
        uint8_t *source=malloc(l.tiled_bytes+2), *dest=malloc(l.linear_bytes+2);
        uint8_t *reference=malloc(l.linear_bytes);
        CHECK(source && dest && reference);
        if (!source || !dest || !reference) { free(source);free(dest);free(reference);continue; }
        memset(source,0xcd,l.tiled_bytes+2);memset(dest,0xa5,l.linear_bytes+2);
        for (uint32_t plane=0;plane<2;++plane) {
            const uint32_t rows=plane?height/2u:height;
            const size_t offset=plane?l.chroma_offset:0;
            for (uint32_t y=0;y<rows;++y) for (uint32_t x=0;x<pitch;++x) {
                const uint8_t value=(uint8_t)(x*17u+y*29u+(x/32u)*43u+(y/16u)*71u+plane*113u);
                reference[offset+(size_t)y*pitch+x]=value;
                source[1+offset+tbl_address(pitch,x,y)]=value;
            }
        }
        /* The former direct linear memcpy cannot represent this surface. */
        CHECK(memcmp(reference,source+1,l.linear_bytes)!=0);
        CHECK(!tg_nvdec_nv12_readback(dest+1,l.linear_bytes-1,source+1,l.tiled_bytes,pitch,height));
        CHECK(!tg_nvdec_nv12_readback(dest+1,l.linear_bytes,source+1,l.tiled_bytes-1,pitch,height));
        CHECK(dest[1]==0xa5 && dest[l.linear_bytes]==0xa5);
        CHECK(!tg_nvdec_nv12_readback(source+1,l.linear_bytes,source+1,l.tiled_bytes,pitch,height));
        CHECK(tg_nvdec_nv12_readback(dest+1,l.linear_bytes,source+1,l.tiled_bytes,pitch,height));
        CHECK(!memcmp(dest+1,reference,l.linear_bytes));
        CHECK(dest[0]==0xa5 && dest[l.linear_bytes+1]==0xa5);
        CHECK(source[0]==0xcd && source[l.tiled_bytes+1]==0xcd);
        free(source);free(dest);free(reference);
    }
}

static void test_parser(const char *path)
{
    char tiny[3] = {'L', 'x', 'R'};
    CHECK(tg_nvdec_parse_annexb(NULL, 0, NULL, 0, NULL, 0,
                                NULL, tiny + 1, 1) == 0);
    CHECK(tiny[0] == 'L' && tiny[1] == 0 && tiny[2] == 'R');
    uint8_t *data, *slice;
    size_t size = 0;
    struct tg_nvdec_parsed_frame *frames;
    struct tg_nvdec_stream_info info;
    char err[160] = {0};
    uint32_t count, i;
    struct tg_nvdec_surface surfaces[TG_NVDEC_MAX_SURFACES];
    struct tg_nvdec_h264_job job;
    struct tg_nvdec_cmd_buffers b;
    uint32_t cmd[256];
    size_t words;

    data = read_file(path, &size);
    CHECK(data != NULL);
    if (!data)
        return;
    slice = malloc(8 * 1024 * 1024);
    frames = calloc(64, sizeof(*frames));
    CHECK(slice != NULL && frames != NULL);
    if (!slice || !frames) {
        free(slice);
        free(frames);
        free(data);
        return;
    }
    count = tg_nvdec_parse_annexb(data, size, slice, 8 * 1024 * 1024,
                                  frames, 64, &info, err, sizeof(err));
    CHECK(count == 6);
    CHECK(err[0] == 0);
    CHECK(info.supported);
    CHECK(info.width == 1280 && info.height == 720);
    CHECK(info.coded_width == 1280 && info.coded_height == 720);
    CHECK(info.profile_idc == 77 && info.level_idc == 31);
    CHECK(info.chroma_format_idc == 1);
    CHECK(info.bit_depth_luma == 8 && info.bit_depth_chroma == 8);
    CHECK(info.max_num_ref_frames == 2);
    CHECK(info.frame_count == 6);
    CHECK(info.total_slices == 6);
    CHECK(info.has_b_frame);
    CHECK(info.slice_data_bytes > 0 && info.slice_data_bytes <= size + 6 * 16);

    /* The final slice has no following start code. Preserve its entire
     * payload, including the last bytes before EOF, ahead of our EOS marker. */
    size_t last_payload = 0;
    for (size_t n=size;n>=3;--n) {
        if (!data[n-3] && !data[n-2] && data[n-1]==1) { last_payload=n;break; }
    }
    CHECK(last_payload && count);
    if (last_payload && count) {
        const struct tg_nvdec_parsed_frame *last=&frames[count-1];
        CHECK(last->slice_count==1);
        CHECK(last->bitstream_size==3u+size-last_payload+16u);
        CHECK(!memcmp(slice+last->bitstream_offset+3u,data+last_payload,size-last_payload));
    }

    CHECK(frames[0].idr && frames[0].reference && frames[0].slice_count == 1);
    CHECK(frames[0].pic.curr_slot == 0);
    CHECK(frames[0].pic.curr_top_poc == 0);
    CHECK(frames[0].pic.dpb_count == 0);
    CHECK(frames[1].pic.dpb_count == 1 && frames[1].pic.dpb[0].slot == 0);
    CHECK(frames[2].pic.dpb_count == 2);
    CHECK(frames[3].pic.curr_top_poc == 8);
    CHECK(frames[4].pic.curr_top_poc == 6);
    CHECK(frames[5].pic.curr_top_poc == 10);
    CHECK(frames[3].pic.dpb_count == 2 && frames[4].pic.dpb_count == 2);
    CHECK(frames[1].reference && frames[2].reference && frames[3].reference && !frames[4].reference);
    CHECK(frames[0].pic.pitch_luma == 1280);
    CHECK(tg_nvdec_stage1_supported(1280, 720, 1280, 720, 1, 8));
    CHECK(!tg_nvdec_stage1_supported(3840, 2160, 3840, 2160, 1, 8));
    CHECK(!tg_nvdec_stage1_supported(1280, 720, 1280, 720, 2, 8));
    for (i = 0; i < count; i++) {
        bool flat4 = true, flat8 = true;
        for (unsigned list=0;list<6;++list) for (unsigned k=0;k<16;++k)
            if (frames[i].pic.scaling4x4[list][k]!=16) flat4=false;
        for (unsigned list=0;list<2;++list) for (unsigned k=0;k<64;++k)
            if (frames[i].pic.scaling8x8[list][k]!=16) flat8=false;
        CHECK(flat4 && flat8); /* Main profile, no explicit scaling matrices. */
        CHECK(frames[i].slice_offsets[0] == 0);
        CHECK(frames[i].slice_offsets[frames[i].slice_count] == frames[i].bitstream_size);
        CHECK(frames[i].bitstream_offset + frames[i].bitstream_size <= info.slice_data_bytes);
        CHECK(slice[frames[i].bitstream_offset] == 0 &&
              slice[frames[i].bitstream_offset + 1] == 0 &&
              slice[frames[i].bitstream_offset + 2] == 1);
    }

    /* A parsed picture must produce a complete command stream with the
     * caller-provided surface pitch. */
    make_fake_surfaces(surfaces);
    memset(&job, 0, sizeof(job));
    job.bitstream = slice + frames[0].bitstream_offset;
    job.bitstream_size = frames[0].bitstream_size;
    job.slice_offsets = frames[0].slice_offsets;
    job.slice_count = frames[0].slice_count;
    job.pic = frames[0].pic;
    job.pic.pitch_luma = 256;
    job.pic.pitch_chroma = 256;
    for (i = 0; i < TG_NVDEC_MAX_SURFACES; i++)
        job.surfaces[i] = surfaces[i];
    memset(&b, 0, sizeof(b));
    b.config = 0x80001000;
    b.bitstream = 0x80002000;
    b.slice_offsets = 0x80003000;
    b.coloc = 0x80004000;
    b.history = 0x80100000;
    b.mbhist = 0x80140000;
    b.status = 0x80148000;
    b.stream_len = job.bitstream_size;
    b.slice_count = job.slice_count;
    b.history_size_256 = 1024;
    b.mbhist_size = 32768;
    b.syncpt_id = 1;
    for (i = 0; i < TG_NVDEC_MAX_SURFACES; i++) {
        b.surfaces[i].valid = true;
        b.surfaces[i].luma = surfaces[i].luma_bus;
        b.surfaces[i].chroma = surfaces[i].chroma_bus;
    }
    words = tg_nvdec_build_h264_cmd(cmd, 256, &b);
    CHECK(words == 137);

    /* Corrupt the stream: the parser must fail cleanly, not fabricate a
     * picture. */
    {
        uint8_t *bad = malloc(size);
        char bad_err[160] = {0};
        CHECK(bad != NULL);
        if (bad) {
            memcpy(bad, data, size);
            bad[0] = 0xff;
            bad[1] = 0xff;
            bad[2] = 0xff;
            CHECK(tg_nvdec_parse_annexb(bad, size, slice, 8 * 1024 * 1024,
                                        frames, 64, &info, bad_err, sizeof(bad_err)) == 0);
            CHECK(bad_err[0] != 0);
            free(bad);
        }
    }

    free(slice);
    free(frames);
    free(data);
}

static void test_parser_1080(const char *path)
{
    uint8_t *data, *slice;
    struct tg_nvdec_parsed_frame *frames;
    struct tg_nvdec_stream_info info;
    char err[160] = {0};
    size_t size = 0;
    uint32_t count;

    data = read_file(path, &size);
    CHECK(data != NULL);
    if (!data)
        return;
    slice = malloc(16 * 1024 * 1024);
    frames = calloc(64, sizeof(*frames));
    CHECK(slice != NULL && frames != NULL);
    if (!slice || !frames) {
        free(slice);
        free(frames);
        free(data);
        return;
    }
    count = tg_nvdec_parse_annexb(data, size, slice, 16 * 1024 * 1024,
                                  frames, 64, &info, err, sizeof(err));
    CHECK(count == 6);
    CHECK(err[0] == 0);
    CHECK(info.supported);
    CHECK(info.width == 1920 && info.height == 1080);
    CHECK(info.coded_width == 1920 && info.coded_height == 1088);
    CHECK(info.profile_idc == 77 && info.level_idc == 40);
    CHECK(info.chroma_format_idc == 1);
    CHECK(info.bit_depth_luma == 8 && info.bit_depth_chroma == 8);
    CHECK(info.max_num_ref_frames == 4);
    CHECK(info.frame_count == 6 && info.total_slices == 6);
    CHECK(info.has_b_frame);
    CHECK(frames[0].idr && frames[0].pic.curr_top_poc == 0);
    CHECK(frames[1].pic.curr_top_poc == 6);
    CHECK(frames[2].pic.curr_top_poc == 2);
    CHECK(frames[3].pic.curr_top_poc == 4);
    CHECK(frames[4].pic.curr_top_poc == 10);
    CHECK(frames[5].pic.curr_top_poc == 8);
    CHECK(frames[3].pic.dpb_count == 3 && frames[5].pic.dpb_count == 4);
    CHECK(frames[0].pic.pitch_luma == 1920);
    CHECK(tg_nvdec_stage1_supported(1920, 1080, 1920, 1088, 1, 8));
    CHECK(!tg_nvdec_stage1_supported(3840, 2160, 3840, 2160, 1, 8));
    free(slice);
    free(frames);
    free(data);
}

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: %s <720p-sample.h264> [1080p-sample.h264]\n", argv[0]);
        return 2;
    }
    test_config();
    test_command();
    test_nv12_layout();
    test_parser(argv[1]);
    if (argc == 3) test_parser_1080(argv[2]);
    if (failures) {
        fprintf(stderr, "NVDEC H.264 control: %d failure(s)\n", failures);
        return 1;
    }
    printf("NVDEC H.264 config/method/parser: PASS (control only, no hardware)\n");
    return 0;
}
