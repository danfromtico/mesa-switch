/* SPDX-License-Identifier: MIT
 *
 * Host model tests for the NVDEC firmware loader, Falcon boot sequence,
 * Host1x method stream, syncpoint completion, error status and timeout paths.
 * This is a register/DMA model, not a hardware decode result.
 */
#include "nvdec.h"
#include "nvidia/video/nvdec_drv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MODEL_DMA_BUS 0x80000000ull
#define MODEL_MEM_SIZE (16u * 1024u * 1024u)
#define MODEL_REG_WORDS (TG_NVDEC_REG_SIZE / 4u)

struct model {
    uint32_t regs[MODEL_REG_WORDS];
    uint8_t *mem;
    size_t mem_used;
    uint32_t syncpt;
    uint32_t fw_bus;
    uint32_t fw_size;
    uint32_t trfbase;
    uint32_t trf_chunks;
    uint32_t trf_bytes[64];
    uint32_t status_bus;
    uint32_t scratch_bus[3];
    uint32_t app_id;
    uint32_t control;
    uint32_t picture_index;
    uint32_t surface_methods;
    uint32_t cmds[512];
    uint32_t cmd_words;
    uint32_t last_expected;
    uint32_t last_observed;
    bool fail_dma;
    bool fail_boot;
    bool fail_submit;
    bool fail_status;
    bool timeout;
    bool cancel_called;
    bool fail_cancel;
    bool fail_alloc;
    uint32_t frees;
};

static int failures;
#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

static void *dma_bus_to_cpu(struct model *m, uint64_t bus)
{
    if (bus < MODEL_DMA_BUS || bus >= MODEL_DMA_BUS + m->mem_used)
        return NULL;
    return m->mem + (size_t)(bus - MODEL_DMA_BUS);
}

static bool model_dma_alloc(void *ctx, size_t size, struct tg_nvdec_dma *out)
{
    struct model *m = ctx;
    size_t aligned = (size + 255u) & ~255u;

    if (m->fail_alloc || m->mem_used + aligned > MODEL_MEM_SIZE)
        return false;
    out->cpu = m->mem + m->mem_used;
    out->bus = MODEL_DMA_BUS + m->mem_used;
    out->size = size;
    m->mem_used += aligned;
    return true;
}

static void model_dma_free(void *ctx, struct tg_nvdec_dma *dma)
{
    struct model *m = ctx;
    m->frees++;
    memset(dma, 0, sizeof(*dma));
}

static uint32_t model_read32(void *ctx, uint32_t offset)
{
    struct model *m = ctx;
    if (offset >= TG_NVDEC_REG_SIZE)
        return 0xffffffffu;
    return m->regs[offset / 4u];
}

static void model_write32(void *ctx, uint32_t offset, uint32_t value)
{
    struct model *m = ctx;

    if (offset >= TG_NVDEC_REG_SIZE)
        return;
    m->regs[offset / 4u] = value;
    if (offset == 0x111cu) {
        if (m->trf_chunks < 64)
            m->trf_bytes[m->trf_chunks] = value;
        m->trf_chunks++;
    } else if (offset == 0x1110u) {
        m->trfbase = value;
    } else if (offset == 0x1118u) {
        if (!m->fail_dma)
            m->regs[offset / 4u] |= 2u;   /* IDLE */
    } else if (offset == 0x1100u) {
        if (value & 2u) {                  /* STARTCPU */
            m->regs[offset / 4u] = 0;      /* STARTCPU is self-clearing */
            if (!m->fail_boot)
                m->regs[0x104c / 4u] = 0;  /* FALCON_IDLESTATE */
        }
    } else if (offset == 0x110cu) {
        if (m->fail_dma)
            m->regs[offset / 4u] |= 6u;    /* scrub never finishes */
    }
}

static void model_delay(void *ctx, uint32_t us)
{
    (void)ctx;
    (void)us;
}

static enum tg_nvdec_result model_submit(void *ctx, const struct tg_nvdec_dma *gather,
                                         uint32_t words, uint32_t syncpt_id,
                                         uint32_t timeout_ms, uint32_t *expected,
                                         uint32_t *observed)
{
    struct model *m = ctx;
    const uint32_t *cmd = gather->cpu;
    uint32_t i = 0;
    bool saw_execute = false;
    bool saw_syncpt = false;

    (void)timeout_ms;
    if (!cmd || !words || words > 512)
        return TG_NVDEC_ERR_SUBMIT;
    memcpy(m->cmds, cmd, words * 4u);
    m->cmd_words = words;
    m->surface_methods = 0;
    while (i < words) {
        const uint32_t op = cmd[i++];
        if ((op >> 28) == 1 && ((op >> 16) & 0xfffu) == 0x10u && (op & 0xffffu) == 2u) {
            uint32_t method, data;
            if (i + 1 >= words)
                return TG_NVDEC_ERR_SUBMIT;
            method = cmd[i++];
            data = cmd[i++];
            if (method == 0x200u / 4u)
                m->app_id = data;
            else if (method == 0x400u / 4u)
                m->control = data;
            else if (method == 0x424u / 4u)
                m->status_bus = data << 8;
            else if (method == 0x414u / 4u)
                m->scratch_bus[0] = data << 8;
            else if (method == 0x418u / 4u)
                m->scratch_bus[1] = data << 8;
            else if (method == 0x500u / 4u)
                m->scratch_bus[2] = data << 8;
            else if (method == 0x40cu / 4u)
                m->picture_index = data;
            else if (method >= 0x430u / 4u && method <= 0x4b4u / 4u)
                m->surface_methods++;
            else if (method == 0x300u / 4u)
                saw_execute = true;
        } else if ((op >> 28) == 2 && ((op >> 16) & 0xfffu) == 0u && (op & 0xffffu) == 1u) {
            const uint32_t value = cmd[i++];
            if ((value & 0xffu) != syncpt_id || ((value >> 8) & 0xffu) != 1u)
                return TG_NVDEC_ERR_SUBMIT;
            saw_syncpt = true;
        } else {
            return TG_NVDEC_ERR_SUBMIT;
        }
    }
    if (!saw_execute || !saw_syncpt || m->app_id != 3u ||
        !(m->control & 3u) || !m->status_bus)
        return TG_NVDEC_ERR_SUBMIT;

    if (m->fail_submit)
        return TG_NVDEC_ERR_SUBMIT;
    if (m->timeout) {
        *expected = m->syncpt + 1u;
        *observed = m->syncpt;
        return TG_NVDEC_ERR_TIMEOUT;
    }

    *expected = m->syncpt + 1u;
    m->syncpt = *expected;
    *observed = m->syncpt;
    m->last_expected = *expected;
    m->last_observed = *observed;

    {
        nvdec_status_s *st = dma_bus_to_cpu(m, m->status_bus);
        if (!st)
            return TG_NVDEC_ERR_SUBMIT;
        memset(st, 0, sizeof(*st));
        if (m->fail_status) {
            st->error_status = 1;
            st->mbs_in_error = 1;
        } else {
            /* Read the picture setup from the DMA arena so the test also
             * proves the config buffer is reachable by the model. */
            const nvdec_h264_pic_s *pic = NULL;
            uint32_t j;
            for (j = 0; j + 2 < m->cmd_words; j++) {
                if (m->cmds[j] == ((1u << 28) | (0x10u << 16) | 2u) &&
                    m->cmds[j + 1] == (0x404u >> 2)) {
                    pic = dma_bus_to_cpu(m, (uint64_t)m->cmds[j + 2] << 8);
                    break;
                }
            }
            if (!pic)
                return TG_NVDEC_ERR_SUBMIT;
            st->mbs_correctly_decoded = (uint32_t)pic->PicWidthInMbs *
                                        (uint32_t)pic->FrameHeightInMbs;
        }
    }
    return TG_NVDEC_OK;
}

static bool model_cancel(void *ctx)
{
    struct model *m = ctx;
    m->cancel_called = true;
    return !m->fail_cancel;
}

static const struct tg_nvdec_hal model_hal = {
    .read32 = model_read32,
    .write32 = model_write32,
    .delay_us = model_delay,
    .dma_alloc = model_dma_alloc,
    .dma_free = model_dma_free,
    .submit = model_submit,
    .cancel = model_cancel,
};

static struct model *model_new(void)
{
    struct model *m = calloc(1, sizeof(*m));
    if (!m)
        return NULL;
    m->mem = calloc(1, MODEL_MEM_SIZE);
    if (!m->mem) {
        free(m);
        return NULL;
    }
    m->regs[0x104c / 4u] = 1;   /* IDLESTATE: not booted */
    m->regs[0x1118 / 4u] = 2;   /* DMATRFCMD IDLE */
    return m;
}

static void model_free(struct model *m)
{
    free(m->mem);
    free(m);
}

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
    if (n < 0) {
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

static void make_job(struct tg_nvdec_h264_job *job, uint8_t *bits,
                     uint32_t *offsets, struct tg_nvdec_surface *surfaces)
{
    memset(job, 0, sizeof(*job));
    memset(bits, 0, 64);
    bits[0] = 0; bits[1] = 0; bits[2] = 1; bits[3] = 0x65;
    offsets[0] = 0;
    offsets[1] = 64;
    job->bitstream = bits;
    job->bitstream_size = 64;
    job->slice_offsets = offsets;
    job->slice_count = 1;
    job->pic.width = 176;
    job->pic.height = 144;
    job->pic.coded_width = 176;
    job->pic.coded_height = 144;
    job->pic.pitch_luma = 256;
    job->pic.pitch_chroma = 256;
    job->pic.chroma_format_idc = 1;
    job->pic.bit_depth_luma = 8;
    job->pic.bit_depth_chroma = 8;
    job->pic.log2_max_frame_num_minus4 = 4;
    job->pic.log2_max_pic_order_cnt_lsb_minus4 = 4;
    job->pic.frame_mbs_only_flag = true;
    job->pic.direct_8x8_inference_flag = true;
    job->pic.lossless_ipred8x8_filter_enable = true;
    job->pic.curr_slot = 0;
    job->pic.frame_num = 0;
    job->pic.curr_top_poc = 0;
    job->pic.curr_bottom_poc = 0;
    job->pic.dpb_count = 0;
    job->pic.pic_order_cnt_type = 0;

    memset(surfaces, 0, sizeof(struct tg_nvdec_surface) * TG_NVDEC_MAX_SURFACES);
    surfaces[0].valid = true;
    surfaces[0].slot = 0;
    surfaces[0].luma_bus = 0x90000000ull;
    surfaces[0].chroma_bus = 0x90010000ull;
    job->surfaces[0] = surfaces[0];
}

static int test_sha(void)
{
    static const char *vectors[] = {
        "", "abc", "The quick brown fox jumps over the lazy dog",
    };
    static const char *digests[] = {
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592",
    };
    unsigned i;

    for (i = 0; i < 3; i++) {
        uint8_t out[32];
        char hex[65];
        unsigned j;
        tg_nvdec_sha256(vectors[i], strlen(vectors[i]), out);
        for (j = 0; j < 32; j++)
            snprintf(hex + j * 2, 3, "%02x", out[j]);
        CHECK(strcmp(hex, digests[i]) == 0);
    }
    return 0;
}

static int test_firmware(const char *fw_path)
{
    struct model *m = model_new();
    struct tg_nvdec nvdec;
    struct tg_nvdec_hal hal = model_hal;
    size_t size = 0;
    uint8_t *fw;
    uint8_t sha[32];
    uint8_t bad[32];

    CHECK(m != NULL);
    fw = read_file(fw_path, &size);
    CHECK(fw != NULL);
    if (!m || !fw) {
        model_free(m);
        free(fw);
        return 1;
    }
    tg_nvdec_sha256(fw, size, sha);
    hal.ctx = m;
    tg_nvdec_init(&nvdec, &hal, 7);

    /* Wrong magic/version/size/hash/sections must fail before DMA. */
    {
        uint8_t *copy = malloc(size);
        CHECK(copy != NULL);
        if (copy) {
            memcpy(copy, fw, size);
            copy[0] ^= 0xff;
            CHECK(tg_nvdec_load_firmware(&nvdec, copy, size, NULL) ==
                  TG_NVDEC_ERR_FIRMWARE_MAGIC);
            memcpy(copy, fw, size);
            copy[4] = 2;
            CHECK(tg_nvdec_load_firmware(&nvdec, copy, size, NULL) ==
                  TG_NVDEC_ERR_FIRMWARE_VERSION);
            memcpy(copy, fw, size);
            copy[8] = 0xff; copy[9] = 0xff; copy[10] = 0xff; copy[11] = 0xff;
            CHECK(tg_nvdec_load_firmware(&nvdec, copy, size, NULL) ==
                  TG_NVDEC_ERR_FIRMWARE_SIZE);
            memcpy(copy, fw, size);
            memcpy(bad, sha, sizeof(bad));
            bad[0] ^= 1;
            CHECK(tg_nvdec_load_firmware(&nvdec, copy, size, bad) ==
                  TG_NVDEC_ERR_FIRMWARE_HASH);
            memcpy(copy, fw, size);
            copy[0x100 + 4] = 1;   /* code_size not 256-aligned */
            CHECK(tg_nvdec_load_firmware(&nvdec, copy, size, NULL) ==
                  TG_NVDEC_ERR_FIRMWARE_SECTIONS);
            free(copy);
        }
    }
    CHECK(m->mem_used == 0);   /* rejected images never allocate DMA */

    CHECK(tg_nvdec_load_firmware(&nvdec, fw, size, sha) == TG_NVDEC_OK);
    CHECK(nvdec.firmware_loaded);
    CHECK(memcmp(nvdec.fw_sha256, sha, sizeof(sha)) == 0);
    CHECK(nvdec.stats.firmware_bytes == size);
    CHECK(nvdec.firmware.bus == MODEL_DMA_BUS);
    CHECK(tg_nvdec_boot(&nvdec) == TG_NVDEC_OK);
    CHECK(nvdec.booted);
    CHECK(m->trf_chunks == 7);   /* 0x300 code + 0x400 data, 256B chunks */
    CHECK(m->trfbase == (uint32_t)((nvdec.firmware.bus + 0x200u) >> 8));
    /* The base already includes os_data_offset. FBOFFS is relative to it. */
    for (uint32_t i = 0; i < 4; ++i)
        CHECK(m->trf_bytes[i] == 0x1ee00u + i * 256u);
    for (uint32_t i = 0; i < 3; ++i)
        CHECK(m->trf_bytes[i + 4] == i * 256u);
    CHECK(m->regs[0x1100 / 4u] == 0);   /* CPUCTL: STARTCPU cleared */

    tg_nvdec_close(&nvdec);
    free(fw);
    model_free(m);
    return 0;
}

static int test_boot_timeout(const char *fw_path)
{
    struct model *m = model_new();
    struct tg_nvdec nvdec;
    struct tg_nvdec_hal hal = model_hal;
    size_t size = 0;
    uint8_t *fw;
    uint8_t sha[32];

    CHECK(m != NULL);
    fw = read_file(fw_path, &size);
    CHECK(fw != NULL);
    if (!m || !fw) {
        model_free(m);
        free(fw);
        return 1;
    }
    tg_nvdec_sha256(fw, size, sha);
    m->fail_boot = true;
    hal.ctx = m;
    tg_nvdec_init(&nvdec, &hal, 0);
    CHECK(tg_nvdec_load_firmware(&nvdec, fw, size, sha) == TG_NVDEC_OK);
    CHECK(tg_nvdec_boot(&nvdec) == TG_NVDEC_ERR_BOOT_TIMEOUT);
    CHECK(!nvdec.booted);
    tg_nvdec_close(&nvdec);
    free(fw);
    model_free(m);
    return 0;
}

static int test_submit(const char *fw_path)
{
    struct model *m = model_new();
    struct tg_nvdec nvdec;
    struct tg_nvdec_hal hal = model_hal;
    struct tg_nvdec_h264_job job;
    struct tg_nvdec_status status;
    struct tg_nvdec_surface surfaces[TG_NVDEC_MAX_SURFACES];
    uint8_t bits[64];
    uint32_t offsets[2];
    size_t size = 0;
    uint8_t *fw;
    uint8_t sha[32];

    CHECK(m != NULL);
    fw = read_file(fw_path, &size);
    CHECK(fw != NULL);
    if (!m || !fw) {
        model_free(m);
        free(fw);
        return 1;
    }
    tg_nvdec_sha256(fw, size, sha);
    hal.ctx = m;
    tg_nvdec_init(&nvdec, &hal, 3);
    make_job(&job, bits, offsets, surfaces);
    CHECK(tg_nvdec_submit_h264(&nvdec, &job, &status) == TG_NVDEC_ERR_FIRMWARE_MISSING);
    CHECK(tg_nvdec_load_firmware(&nvdec, fw, size, sha) == TG_NVDEC_OK);
    CHECK(tg_nvdec_submit_h264(&nvdec, &job, &status) == TG_NVDEC_ERR_STATE);
    CHECK(tg_nvdec_boot(&nvdec) == TG_NVDEC_OK);
    {
        enum tg_nvdec_result rr = tg_nvdec_submit_h264(&nvdec, &job, &status);
        if (rr != TG_NVDEC_OK) {
            unsigned q;
            fprintf(stderr, "submit result=%s error=%s words=%u app=%x ctl=%x st=%x pi=%x\n",
                    tg_nvdec_result_name(rr), nvdec.error, m->cmd_words, m->app_id,
                    m->control, m->status_bus, m->picture_index);
            for (q = 0; q + 2 < m->cmd_words && q < 18; q += 3)
                fprintf(stderr, "  %08x %08x %08x\n", m->cmds[q], m->cmds[q+1], m->cmds[q+2]);
            for (q = m->cmd_words > 4 ? m->cmd_words - 4 : 0; q < m->cmd_words; q++)
                fprintf(stderr, "  tail %08x\n", m->cmds[q]);
        }
        CHECK(rr == TG_NVDEC_OK);
    }
    CHECK(status.mbs_correctly_decoded == 99);
    CHECK(status.error_status == 0);
    CHECK(nvdec.stats.submits == 1 && nvdec.stats.completions == 1);
    CHECK(nvdec.stats.last_syncpoint == 1 && nvdec.stats.last_observed == 1);
    CHECK(nvdec.picture_index == 1);
    CHECK(m->cmd_words > 20);
    CHECK(m->app_id == 3);
    CHECK(m->control == 0x53);
    CHECK(m->surface_methods == 2);
    CHECK(m->cmds[0] == ((1u << 28) | (0x10u << 16) | 2u));
    CHECK(m->cmds[1] == 0x200u / 4u);
    CHECK(m->cmds[2] == 3);
    CHECK(m->cmds[m->cmd_words - 2] == ((2u << 28) | 1u));
    CHECK(m->cmds[m->cmd_words - 1] == (3u | (1u << 8)));

    /* A second submit must use the next syncpoint value. */
    CHECK(tg_nvdec_submit_h264(&nvdec, &job, &status) == TG_NVDEC_OK);
    CHECK(nvdec.stats.last_syncpoint == 2);
    CHECK(nvdec.picture_index == 2);

    /* Reference-frame workspaces must survive changes in packet size,
     * including command-arena reallocation and failed growth. Model the
     * engine's previous-frame state with sentinels, not a fake decode. */
    {
        static const size_t packet_sizes[] = {1024, 128 * 1024, 64};
        uint32_t scratch_bus[3];
        uint8_t *packet = calloc(1, 128 * 1024);
        CHECK(packet != NULL);
        memcpy(scratch_bus, m->scratch_bus, sizeof(scratch_bus));
        for (uint32_t i = 0; i < 3; i++) {
            void *cpu = dma_bus_to_cpu(m, scratch_bus[i]);
            CHECK(cpu != NULL);
            if (cpu)
                memset(cpu, (int)(0x30 + i), 64);
        }
        if (packet) {
            for (uint32_t n = 0; n < 3; n++) {
                job.bitstream = packet;
                job.bitstream_size = packet_sizes[n];
                offsets[1] = (uint32_t)job.bitstream_size;
                if (n == 1) {
                    void *arena_cpu = nvdec.arena.cpu;
                    uint64_t submits = nvdec.stats.submits;
                    m->fail_alloc = true;
                    CHECK(tg_nvdec_submit_h264(&nvdec, &job, &status) ==
                          TG_NVDEC_ERR_NO_MEMORY);
                    CHECK(nvdec.arena.cpu == arena_cpu);
                    CHECK(nvdec.stats.submits == submits);
                    m->fail_alloc = false;
                }
                CHECK(tg_nvdec_submit_h264(&nvdec, &job, &status) == TG_NVDEC_OK);
                for (uint32_t i = 0; i < 3; i++) {
                    const uint8_t *cpu = dma_bus_to_cpu(m, m->scratch_bus[i]);
                    CHECK(m->scratch_bus[i] == scratch_bus[i]);
                    CHECK(cpu != NULL);
                    if (cpu)
                        for (uint32_t j = 0; j < 64; j++)
                            CHECK(cpu[j] == 0x30 + i);
                }
            }
            free(packet);
        }
        job.bitstream = bits;
        job.bitstream_size = sizeof(bits);
        offsets[1] = sizeof(bits);
    }

    /* Bad slice offsets and aliased surfaces are rejected before submit. */
    nvdec.stats.submits = 0;
    offsets[0] = 4;
    CHECK(tg_nvdec_submit_h264(&nvdec, &job, &status) == TG_NVDEC_ERR_SLICE);
    CHECK(nvdec.stats.submits == 0);
    offsets[0] = 0;
    job.surfaces[1] = job.surfaces[0];
    job.surfaces[1].slot = 1;
    job.surfaces[1].valid = true;
    job.pic.dpb_count = 1;
    job.pic.dpb[0].slot = 1;
    CHECK(tg_nvdec_submit_h264(&nvdec, &job, &status) == TG_NVDEC_ERR_SURFACE);
    job.surfaces[1].valid = false;
    job.pic.dpb_count = 0;

    /* Engine error status is surfaced, not converted to success. */
    m->fail_status = true;
    CHECK(tg_nvdec_submit_h264(&nvdec, &job, &status) == TG_NVDEC_ERR_ENGINE);
    CHECK(status.error_status == 1 && status.mbs_in_error == 1);
    CHECK(nvdec.stats.failures == 1);
    m->fail_status = false;

    /* Timeout leaves the device usable only after cancel and is reported. */
    m->timeout = true;
    CHECK(tg_nvdec_submit_h264(&nvdec, &job, &status) == TG_NVDEC_ERR_TIMEOUT);
    CHECK(nvdec.stats.timeouts == 1);
    CHECK(m->cancel_called);
    m->timeout = false;
    CHECK(tg_nvdec_submit_h264(&nvdec, &job, &status) == TG_NVDEC_ERR_STATE);
    CHECK(tg_nvdec_boot(&nvdec) == TG_NVDEC_OK);
    CHECK(tg_nvdec_submit_h264(&nvdec, &job, &status) == TG_NVDEC_OK);

    /* An unsupported configuration is rejected, not silently decoded. */
    job.pic.bit_depth_luma = 10;
    CHECK(tg_nvdec_submit_h264(&nvdec, &job, &status) == TG_NVDEC_ERR_UNSUPPORTED);
    job.pic.bit_depth_luma = 8;
    job.pic.chroma_format_idc = 2;
    CHECK(tg_nvdec_submit_h264(&nvdec, &job, &status) == TG_NVDEC_ERR_UNSUPPORTED);
    job.pic.chroma_format_idc = 1;
    job.pic.width = 177;
    CHECK(tg_nvdec_submit_h264(&nvdec, &job, &status) == TG_NVDEC_ERR_DIMENSIONS);
    job.pic.width = 176;

    /* Failed cancellation must pin every DMA allocation and prevent reuse.
     * No fake completion or successful destructor is accepted. */
    {
        void *firmware_cpu = nvdec.firmware.cpu;
        void *arena_cpu = nvdec.arena.cpu;
        void *scratch_cpu = nvdec.scratch.cpu;
        uint32_t frees = m->frees;
        m->timeout = true;
        m->fail_cancel = true;
        CHECK(tg_nvdec_submit_h264(&nvdec, &job, &status) == TG_NVDEC_ERR_TIMEOUT);
        CHECK(nvdec.in_flight);
        CHECK(tg_nvdec_close(&nvdec) == TG_NVDEC_ERR_SUBMIT);
        CHECK(m->frees == frees);
        CHECK(nvdec.firmware.cpu == firmware_cpu && nvdec.arena.cpu == arena_cpu);
        CHECK(nvdec.scratch.cpu == scratch_cpu);
        CHECK(tg_nvdec_load_firmware(&nvdec, fw, size, sha) == TG_NVDEC_ERR_STATE);
        CHECK(tg_nvdec_submit_h264(&nvdec, &job, &status) == TG_NVDEC_ERR_STATE);
        m->fail_cancel = false;
        CHECK(tg_nvdec_cancel(&nvdec) == TG_NVDEC_OK);
        CHECK(!nvdec.in_flight && !nvdec.booted);
        CHECK(tg_nvdec_close(&nvdec) == TG_NVDEC_OK);
        CHECK(m->frees == frees + 3);
        CHECK(!nvdec.scratch.cpu && !nvdec.picture_index);
    }
    free(fw);
    model_free(m);
    return 0;
}

static int test_bad_firmware_paths(void)
{
    struct tg_nvdec nvdec;
    struct tg_nvdec_hal hal = model_hal;
    struct model *m = model_new();
    uint8_t tiny[4] = { 0xde, 0x10, 0, 0 };

    CHECK(m != NULL);
    if (!m)
        return 1;
    hal.ctx = m;
    tg_nvdec_init(&nvdec, &hal, 0);
    CHECK(tg_nvdec_load_firmware(&nvdec, NULL, 0, NULL) ==
          TG_NVDEC_ERR_FIRMWARE_MISSING);
    CHECK(tg_nvdec_load_firmware(&nvdec, tiny, sizeof(tiny), NULL) ==
          TG_NVDEC_ERR_FIRMWARE_MISSING);
    CHECK(tg_nvdec_boot(&nvdec) == TG_NVDEC_ERR_FIRMWARE_MISSING);
    CHECK(tg_nvdec_submit_h264(&nvdec, NULL, NULL) == TG_NVDEC_ERR_ARGUMENT);
    tg_nvdec_close(&nvdec);
    model_free(m);
    return 0;
}

static int test_workspace_capacity(const char *fw_path)
{
    struct model *m = model_new();
    struct tg_nvdec nvdec;
    struct tg_nvdec_hal hal = model_hal;
    struct tg_nvdec_h264_job job;
    struct tg_nvdec_status status;
    struct tg_nvdec_surface surfaces[TG_NVDEC_MAX_SURFACES];
    uint8_t bits[64], sha[32];
    uint32_t offsets[2];
    size_t size = 0;
    uint8_t *fw = read_file(fw_path, &size);

    CHECK(m && fw);
    if (!m || !fw) {
        if (m) model_free(m);
        free(fw);
        return 1;
    }
    hal.ctx = m;
    tg_nvdec_init(&nvdec, &hal, 7);
    tg_nvdec_sha256(fw, size, sha);
    CHECK(tg_nvdec_load_firmware(&nvdec, fw, size, sha) == TG_NVDEC_OK);
    CHECK(tg_nvdec_boot(&nvdec) == TG_NVDEC_OK);
    make_job(&job, bits, offsets, surfaces);
    job.pic.width = job.pic.coded_width = 1280;
    job.pic.height = job.pic.coded_height = 720;
    job.pic.pitch_luma = job.pic.pitch_chroma = 1280;
    job.pic.curr_slot = 16;
    job.pic.dpb_count = 16;
    for (uint32_t i = 0; i < TG_NVDEC_MAX_SURFACES; i++) {
        job.surfaces[i].valid = true;
        job.surfaces[i].slot = i;
        job.surfaces[i].luma_bus = 0x90000000ull + i * 0x200000ull;
        job.surfaces[i].chroma_bus = job.surfaces[i].luma_bus + 0x100000u;
        if (i < 16) job.pic.dpb[i].slot = i;
    }
    CHECK(tg_nvdec_submit_h264(&nvdec, &job, &status) == TG_NVDEC_OK);
    CHECK(status.mbs_correctly_decoded == 3600);
    /* Pinned upstream progressive H.264 layout: 80 x 45 macroblocks,
     * even-height coloc stride 235520 bytes, all indices 0..16 admitted.
     * Independent expected extents catch the former fixed 512KiB overlap.
     * This checks method-address/layout logic, not firmware execution. */
    CHECK(m->scratch_bus[1] - m->scratch_bus[0] == 4003840u);
    CHECK(m->scratch_bus[2] - m->scratch_bus[1] == 61440u);
    CHECK(nvdec.scratch.size == 4073728u);
    CHECK((uint64_t)m->scratch_bus[2] + 8448u ==
          nvdec.scratch.bus + nvdec.scratch.size);
    {
        uint64_t submits = nvdec.stats.submits;
        uint32_t frees = m->frees;
        size_t used = m->mem_used;
        struct tg_nvdec_dma saved = nvdec.scratch;
        /* Changing the geometry would reinterpret retained reference-MV
         * strides. Require an explicit close/reset before another size. */
        job.pic.width = job.pic.coded_width = 1264;
        CHECK(tg_nvdec_submit_h264(&nvdec, &job, &status) == TG_NVDEC_ERR_STATE);
        CHECK(nvdec.stats.submits == submits && m->mem_used == used && m->frees == frees);
        CHECK(nvdec.scratch.cpu == saved.cpu && nvdec.scratch.bus == saved.bus);
        job.pic.width = job.pic.coded_width = 1280;
        CHECK(tg_nvdec_submit_h264(&nvdec, &job, &status) == TG_NVDEC_OK);
        m->fail_cancel = true;
        CHECK(tg_nvdec_close(&nvdec) == TG_NVDEC_ERR_SUBMIT);
        CHECK(nvdec.scratch.cpu == saved.cpu && m->frees == frees);
        m->fail_cancel = false;
    }
    CHECK(tg_nvdec_close(&nvdec) == TG_NVDEC_OK);
    CHECK(tg_nvdec_load_firmware(&nvdec, fw, size, sha) == TG_NVDEC_OK);
    CHECK(tg_nvdec_boot(&nvdec) == TG_NVDEC_OK);
    job.pic.width = job.pic.coded_width = 640;
    job.pic.height = 360;
    job.pic.coded_height = 368;
    CHECK(tg_nvdec_submit_h264(&nvdec, &job, &status) == TG_NVDEC_OK);
    CHECK(nvdec.scratch.size == 1079552u);
    CHECK(tg_nvdec_close(&nvdec) == TG_NVDEC_OK);
    free(fw);
    model_free(m);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <nvidia/tegra210/nvdec.bin>\n", argv[0]);
        return 2;
    }
    test_sha();
    test_firmware(argv[1]);
    test_boot_timeout(argv[1]);
    test_submit(argv[1]);
    test_workspace_capacity(argv[1]);
    test_bad_firmware_paths();
    if (failures) {
        fprintf(stderr, "NVDEC host model: %d failure(s)\n", failures);
        return 1;
    }
    printf("NVDEC firmware/Falcon/Host1x model: PASS (model only, no hardware)\n");
    return 0;
}
