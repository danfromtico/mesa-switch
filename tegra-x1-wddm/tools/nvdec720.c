/* SPDX-License-Identifier: MIT
 *
 * Standalone Windows ARM64 acceptance tool for the stage-1 T210 NVDEC
 * H.264 backend.  It is intentionally limited to 1280x720, 8-bit, 4:2:0
 * progressive H.264. A real KMT device on the native NVK adapter owns the
 * coexisting decoder session. No software decoding fallback is used.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nvdec_control.h"
#include "nvdec_parse.h"
#include "nvdec_native.h"

#define STAGE1_MAX_WIDTH  1280u
#define STAGE1_MAX_HEIGHT 720u
#define MAX_FRAMES 64u
#define MAX_SLICES TG_NVDEC_MAX_SLICES

struct hash_record {
    int32_t poc;
    uint8_t hash[TG_NVDEC_SHA256_SIZE];
};

static void usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s --firmware <nvdec.bin> --input <stream.h264> "
        "--reference <hashes.txt> [--dump <dir>]\n"
        "reference lines are: <display-index> <sha256-of-planar-YUV420p>\n"
        "stage 1 accepts only 1280x720 8-bit 4:2:0 progressive H.264\n", argv0);
}

static uint8_t *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    uint8_t *p;
    long n;

    if (!f) {
        fprintf(stderr, "cannot open %s\n", path);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    n = ftell(f);
    if (n <= 0 || fseek(f, 0, SEEK_SET) != 0) {
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

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static size_t load_reference(const char *path, uint8_t hashes[][TG_NVDEC_SHA256_SIZE],
                             size_t max)
{
    FILE *f = fopen(path, "r");
    char line[256];
    size_t n = 0;

    if (!f) {
        fprintf(stderr, "cannot open reference %s\n", path);
        return 0;
    }
    while (fgets(line, sizeof(line), f) && n < max) {
        char *p = line;
        char *end;
        long index;
        size_t i;
        index = strtol(p, &end, 10);
        if (end == p)
            continue;
        p = end;
        while (*p == ' ' || *p == '\t') p++;
        if (strlen(p) < 64)
            continue;
        for (i = 0; i < 32; i++) {
            const int hi = hex_value(p[i * 2]);
            const int lo = hex_value(p[i * 2 + 1]);
            if (hi < 0 || lo < 0)
                break;
            hashes[n][i] = (uint8_t)((hi << 4) | lo);
        }
        if (i != 32) {
            fprintf(stderr, "reference line %zu is not a SHA-256\n", n + 1);
            fclose(f);
            return 0;
        }
        (void)index;
        n++;
    }
    fclose(f);
    return n;
}

static int hash_nv12_visible(const uint8_t *nv12, uint32_t width, uint32_t height,
                             uint32_t pitch, uint32_t aligned_height,
                             uint8_t out[TG_NVDEC_SHA256_SIZE])
{
    const uint32_t chroma_w = width / 2;
    const uint32_t chroma_h = height / 2;
    const size_t y_size = (size_t)width * height;
    const size_t c_size = (size_t)chroma_w * chroma_h;
    uint8_t *planar;
    uint32_t y, x;
    const uint8_t *uv;

    if (!width || !height || (width & 1u) || (height & 1u) ||
        pitch < width || aligned_height < height)
        return 0;
    planar = malloc(y_size + 2u * c_size);
    if (!planar)
        return 0;
    for (y = 0; y < height; y++)
        memcpy(planar + (size_t)y * width, nv12 + (size_t)y * pitch, width);
    uv = nv12 + (size_t)pitch * aligned_height;
    for (y = 0; y < chroma_h; y++) {
        const uint8_t *row = uv + (size_t)y * pitch;
        for (x = 0; x < chroma_w; x++) {
            planar[y_size + (size_t)y * chroma_w + x] = row[x * 2];
            planar[y_size + c_size + (size_t)y * chroma_w + x] = row[x * 2 + 1];
        }
    }
    tg_nvdec_sha256(planar, y_size + 2u * c_size, out);
    free(planar);
    return 1;
}

static int compare_u32(const void *a, const void *b)
{
    const struct hash_record *x = a;
    const struct hash_record *y = b;
    return (x->poc > y->poc) - (x->poc < y->poc);
}

static void print_hash(const uint8_t h[TG_NVDEC_SHA256_SIZE])
{
    size_t i;
    for (i = 0; i < TG_NVDEC_SHA256_SIZE; i++)
        printf("%02x", h[i]);
}

int main(int argc, char **argv)
{
    /* Preserve the last actual API boundary if the first native call hangs
     * or Windows stops before normal CRT teardown can flush redirected I/O. */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    const char *firmware_path = NULL, *input_path = NULL, *reference_path = NULL;
    const char *dump_dir = NULL;
    uint8_t *firmware = NULL, *stream = NULL, *slice = NULL;
    uint8_t reference[64][TG_NVDEC_SHA256_SIZE];
    struct tg_nvdec_parsed_frame *frames = NULL;
    struct tg_nvdec_stream_info info;
    struct hash_record records[MAX_FRAMES];
    char err[200] = {0};
    size_t firmware_size = 0, stream_size = 0, reference_count = 0;
    uint32_t frame_count = 0, i;
    uint32_t pitch = 0, aligned_height = 0, frame_size = 0;
    struct tg_nvdec_native device = {0};
    uint8_t fw_sha[TG_NVDEC_SHA256_SIZE];
    LONG transport_status = 0;
    int exit_code = 0;

    for (i = 1; i < (uint32_t)argc; i++) {
        if (!strcmp(argv[i], "--firmware") && i + 1 < (uint32_t)argc)
            firmware_path = argv[++i];
        else if (!strcmp(argv[i], "--input") && i + 1 < (uint32_t)argc)
            input_path = argv[++i];
        else if (!strcmp(argv[i], "--reference") && i + 1 < (uint32_t)argc)
            reference_path = argv[++i];
        else if (!strcmp(argv[i], "--dump") && i + 1 < (uint32_t)argc)
            dump_dir = argv[++i];
        else {
            usage(argv[0]);
            return 2;
        }
    }
    if (!firmware_path || !input_path || !reference_path) {
        usage(argv[0]);
        return 2;
    }

    firmware = read_file(firmware_path, &firmware_size);
    stream = read_file(input_path, &stream_size);
    if (!firmware || !stream) {
        exit_code = 3;
        goto out;
    }
    if (firmware_size < 64 || firmware_size > 1024u * 1024u) {
        fprintf(stderr, "firmware size %zu is out of range\n", firmware_size);
        exit_code = 3;
        goto out;
    }
    tg_nvdec_sha256(firmware, firmware_size, fw_sha);

    slice = malloc(32u * 1024u * 1024u);
    frames = calloc(MAX_FRAMES, sizeof(*frames));
    if (!slice || !frames) {
        fprintf(stderr, "out of memory\n");
        exit_code = 3;
        goto out;
    }
    frame_count = tg_nvdec_parse_annexb(stream, stream_size, slice,
                                        32u * 1024u * 1024u, frames, MAX_FRAMES,
                                        &info, err, sizeof(err));
    if (!frame_count) {
        fprintf(stderr, "H.264 parse rejected: %s\n", err);
        exit_code = 4;
        goto out;
    }
    if (!tg_nvdec_stage1_supported(info.width, info.height, info.coded_width,
                                   info.coded_height, info.chroma_format_idc,
                                   info.bit_depth_luma)) {
        fprintf(stderr, "REJECTED: stage 1 is limited to 1280x720 8-bit 4:2:0 "
                "progressive H.264; stream is %ux%u coded %ux%u\n",
                info.width, info.height, info.coded_width, info.coded_height);
        exit_code = 4;
        goto out;
    }
    if (info.bit_depth_chroma != 8 || !info.supported) {
        fprintf(stderr, "REJECTED: stage 1 requires 8-bit 4:2:0 progressive H.264\n");
        exit_code = 4;
        goto out;
    }
    if ((info.width & 1u) || (info.height & 1u)) {
        fprintf(stderr, "REJECTED: odd visible dimensions are not supported\n");
        exit_code = 4;
        goto out;
    }
    reference_count = load_reference(reference_path, reference, 64);
    if (!reference_count) {
        fprintf(stderr, "reference hash file is empty or invalid\n");
        exit_code = 3;
        goto out;
    }
    if (reference_count != frame_count) {
        fprintf(stderr, "reference has %zu frames but stream has %u\n",
                reference_count, frame_count);
        exit_code = 3;
        goto out;
    }

    printf("NVDEC stage-1 tool: %ux%u coded %ux%u profile %u level %u refs %u frames %u\n",
           info.width, info.height, info.coded_width, info.coded_height,
           info.profile_idc, info.level_idc, info.max_num_ref_frames, frame_count);
    printf("firmware: %zu bytes sha256 ", firmware_size);
    print_hash(fw_sha);
    printf("\n");

    struct tg_kmt_scan scan = {0};
    transport_status = tg_gpu_kmt_scan_system(&scan);
    if (transport_status != 0 || scan.matched != 1 || scan.native_ready != 1) {
        fprintf(stderr, "HARDWARE_PENDING: native adapter scan status=0x%08lx matched=%u ready=%u\n",
                (unsigned long)transport_status, scan.matched, scan.native_ready);
        exit_code = 5;
        goto out;
    }
    printf("native NVK LUID=%08x%08x\n", (uint32_t)scan.first_ready.luid_high, scan.first_ready.luid_low);
    transport_status = tg_nvdec_native_open(&device, &scan.first_ready);
    if (transport_status != 0) {
        fprintf(stderr, "native KMT device open failed: 0x%08lx\n", (unsigned long)transport_status);
        exit_code = 5; goto out;
    }

    {
        struct tg_nvdec_load_request *req;
        struct tg_nvdec_load_result res;
        size_t in_size = sizeof(*req) + firmware_size;
        req = calloc(1, in_size);
        if (!req) {
            exit_code = 3;
            goto out;
        }
        req->version = TG_NVDEC_CONTROL_VERSION;
        req->size = (uint32_t)firmware_size;
        memcpy(req->sha256, fw_sha, sizeof(fw_sha));
        memcpy((uint8_t *)req + sizeof(*req), firmware, firmware_size);
        memset(&res, 0, sizeof(res));
        transport_status = tg_nvdec_native_call(&device, TG_NVDEC_ESCAPE_LOAD, req,
                                                (uint32_t)in_size, &res, sizeof(res));
        if (transport_status != 0) {
            fprintf(stderr, "NVDEC_LOAD failed: 0x%08lx %s\n", (unsigned long)transport_status, res.error);
            free(req);
            exit_code = 5;
            goto out;
        }
        free(req);
        if (res.result != 0 || !res.booted) {
            fprintf(stderr, "NVDEC firmware/Falcon load failed: 0x%08x %s\n",
                    res.result, res.error);
            exit_code = 7;
            goto out;
        }
        printf("firmware loaded, Falcon booted\n");
    }

    {
        struct tg_nvdec_config_request req;
        struct tg_nvdec_config_result res;
        req.version = TG_NVDEC_CONTROL_VERSION;
        req.width = info.coded_width;
        req.height = info.coded_height;
        memset(&res, 0, sizeof(res));
        transport_status = tg_nvdec_native_call(&device, TG_NVDEC_ESCAPE_CONFIGURE, &req,
                                                sizeof(req), &res, sizeof(res));
        if (transport_status != 0 ||
            res.result != 0) {
            fprintf(stderr, "NVDEC_CONFIGURE failed: 0x%08x %s\n",
                    res.result, res.error);
            exit_code = 7;
            goto out;
        }
        pitch = res.pitch;
        aligned_height = res.aligned_height;
        frame_size = res.frame_size;
        printf("surfaces: pitch %u aligned height %u frame %u bytes\n",
               pitch, aligned_height, frame_size);
    }

    for (i = 0; i < frame_count; i++) {
        struct tg_nvdec_parsed_frame *f = &frames[i];
        const uint32_t offsets_size = (f->slice_count + 1u) * sizeof(uint32_t);
        const size_t in_size = sizeof(struct tg_nvdec_decode_request) +
                               f->bitstream_size + offsets_size;
        const size_t out_size = sizeof(struct tg_nvdec_decode_result) + frame_size;
        struct tg_nvdec_decode_request *req;
        struct tg_nvdec_decode_result *res;
        uint8_t *out;
        uint8_t hash[TG_NVDEC_SHA256_SIZE];
        const uint32_t expected_mbs =
            (f->pic.coded_width / 16u) * (f->pic.coded_height / 16u);

        req = calloc(1, in_size);
        out = calloc(1, out_size);
        if (!req || !out) {
            free(req);
            free(out);
            fprintf(stderr, "out of memory for frame %u\n", i);
            exit_code = 3;
            goto out;
        }
        req->version = TG_NVDEC_CONTROL_VERSION;
        req->bitstream_size = f->bitstream_size;
        req->slice_count = f->slice_count;
        req->output_size = frame_size;
        req->pic = f->pic;
        req->pic.pitch_luma = pitch;
        req->pic.pitch_chroma = pitch;
        memcpy((uint8_t *)req + sizeof(*req), slice + f->bitstream_offset,
               f->bitstream_size);
        memcpy((uint8_t *)req + sizeof(*req) + f->bitstream_size,
               f->slice_offsets, offsets_size);

        transport_status = tg_nvdec_native_call(&device, TG_NVDEC_ESCAPE_DECODE, req,
                                                (uint32_t)in_size, out, (uint32_t)out_size);
        res = (struct tg_nvdec_decode_result *)out;
        if (transport_status != 0) {
            /* A delivered escape can fail inside the driver and still carry
             * its diagnostic response. Keep that evidence before CLOSE. A
             * delivery/protocol failure leaves the calloc'd response zero. */
            fprintf(stderr,
                    "frame %u NVDEC_DECODE failed: 0x%08lx response_v=%u "
                    "result=0x%08x sync=%u/%u bytes=%u mbs=%u/%u "
                    "mb_errors=%u error=0x%08x slice_error=0x%08x cycles=%u detail=%.*s "
                    "request=%u response=%u bitstream=%u slices=%u slot=%u\n",
                    i, (unsigned long)transport_status, res->version,
                    res->result, res->expected_syncpoint, res->observed_syncpoint,
                    res->bytes_written, res->mbs_correctly_decoded, expected_mbs,
                    res->mbs_in_error, res->error_status, res->slice_header_error_code,
                    res->cycles, (int)sizeof(res->error), res->error,
                    (uint32_t)in_size, (uint32_t)out_size, f->bitstream_size,
                    f->slice_count, f->pic.curr_slot);
            free(req);
            free(out);
            exit_code = 5;
            goto out;
        }
        if (res->result != 0 || res->error_status || res->mbs_in_error ||
            res->mbs_correctly_decoded != expected_mbs ||
            res->expected_syncpoint != res->observed_syncpoint ||
            res->bytes_written != frame_size) {
            fprintf(stderr,
                "frame %u NVDEC failure: result=0x%08x mbs=%u/%u err=%u "
                "slice_err=%u sync=%u/%u bytes=%u %s\n",
                i, res->result, res->mbs_correctly_decoded, expected_mbs,
                res->error_status, res->slice_header_error_code,
                res->expected_syncpoint, res->observed_syncpoint,
                res->bytes_written, res->error);
            free(req);
            free(out);
            exit_code = 7;
            goto out;
        }
        if (!hash_nv12_visible(out + sizeof(*res), info.width, info.height,
                               pitch, aligned_height, hash)) {
            fprintf(stderr, "frame %u NV12 layout rejected\n", i);
            free(req);
            free(out);
            exit_code = 7;
            goto out;
        }
        records[i].poc = f->pic.curr_top_poc;
        memcpy(records[i].hash, hash, sizeof(hash));
        printf("frame %u slot %u poc %d slices %u sync %u/%u mbs %u sha256 ",
               i, f->pic.curr_slot, f->pic.curr_top_poc, f->slice_count,
               res->expected_syncpoint, res->observed_syncpoint,
               res->mbs_correctly_decoded);
        print_hash(hash);
        printf("\n");
        if (dump_dir) {
            char path[512];
            FILE *df;
            snprintf(path, sizeof(path), "%s\\nvdec_poc%d_%u.nv12", dump_dir,
                     f->pic.curr_top_poc, i);
            df = fopen(path, "wb");
            if (df) {
                fwrite(out + sizeof(*res), 1, frame_size, df);
                fclose(df);
            }
        }
        free(req);
        free(out);
    }

    qsort(records, frame_count, sizeof(records[0]), compare_u32);
    for (i = 0; i < frame_count; i++) {
        if (memcmp(records[i].hash, reference[i], TG_NVDEC_SHA256_SIZE) != 0) {
            fprintf(stderr, "pixel mismatch at display order %u (poc %d): expected ",
                    i, records[i].poc);
            print_hash((const uint8_t *)reference[i]);
            fprintf(stderr, " got ");
            print_hash(records[i].hash);
            fprintf(stderr, "\n");
            exit_code = 6;
            goto out;
        }
    }
out:
    transport_status = tg_nvdec_native_close(&device);
    if (transport_status != 0) {
        fprintf(stderr, "NVDEC close failed: 0x%08lx; cleanup not accepted\n", (unsigned long)transport_status);
        exit_code = 8;
    }
    if (!exit_code) {
        printf("NVDEC=used (real Host1x syncpoint per frame)\n");
        printf("RESULT=PASS all %u frames matched the software reference; session retired\n", frame_count);
    }
    free(firmware);
    free(stream);
    free(slice);
    free(frames);
    return exit_code;
}
