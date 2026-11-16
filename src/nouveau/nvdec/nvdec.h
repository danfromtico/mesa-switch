/* SPDX-License-Identifier: MIT
 *
 * Tegra X1 (T210/T210B01) NVDEC H.264 decode backend.
 *
 * The register/Falcon contract is taken from the upstream NVIDIA "drm/tegra:
 * Add NVDEC driver" series (Mikko Perttunen, 2021-09-16, v6) and the NVDEC
 * class programming sequence from NVIDIA's MIT-licensed vaapi-tegra-driver.
 * The H.264 picture structure is NVIDIA's open-gpu-doc nvdec_drv.h (MIT).
 *
 * This module is deliberately free of OS and Vulkan dependencies so that the
 * same code can run against the Windows kernel HAL and against a register
 * model in the host tests.  Nothing here writes a completion value: the only
 * completion source is the Host1x syncpoint advanced by the NVDEC engine.
 */
#ifndef TGD3D_NVDEC_H
#define TGD3D_NVDEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TG_NVDEC_MAX_DPB       16u
#define TG_NVDEC_MAX_SURFACES  17u
#define TG_NVDEC_MAX_SLICES    256u
#define TG_NVDEC_MAX_SPS       32u
#define TG_NVDEC_MAX_PPS       256u
#define TG_NVDEC_SHA256_SIZE   32u

/* Stage-1 admission policy: only 720p 8-bit 4:2:0 progressive H.264 is
 * enabled.  The core can construct larger pictures, but no caller may
 * advertise or accept a larger mode until that mode has its own validation. */
static inline bool tg_nvdec_stage1_supported(uint32_t width, uint32_t height,
                                             uint32_t coded_width,
                                             uint32_t coded_height,
                                             uint32_t chroma_format_idc,
                                             uint32_t bit_depth_luma)
{
    return width >= 16u && height >= 16u && width <= 1280u && height <= 720u &&
           coded_width <= 1280u && coded_height <= 720u &&
           chroma_format_idc == 1u && bit_depth_luma == 8u;
}

/* NVDEC register aperture on Tegra210. */
#define TG_NVDEC_REG_BASE      0x54480000u
#define TG_NVDEC_REG_SIZE      0x00040000u
/* Host1x register aperture and the T210 NVDEC Host1x channel class. */
#define TG_HOST1X_REG_BASE     0x50000000u
#define TG_HOST1X_REG_SIZE     0x00040000u
#define TG_HOST1X_CLASS_NVDEC  0xf0u
#define TG_NVDEC_SYNCPT_SHIFT  8u

struct tg_nvdec_dma {
    void *cpu;
    uint64_t bus;
    size_t size;
};

enum tg_nvdec_result {
    TG_NVDEC_OK = 0,
    TG_NVDEC_ERR_ARGUMENT,
    TG_NVDEC_ERR_NO_HAL,
    TG_NVDEC_ERR_NO_MEMORY,
    TG_NVDEC_ERR_FIRMWARE_MISSING,
    TG_NVDEC_ERR_FIRMWARE_MAGIC,
    TG_NVDEC_ERR_FIRMWARE_VERSION,
    TG_NVDEC_ERR_FIRMWARE_SIZE,
    TG_NVDEC_ERR_FIRMWARE_SECTIONS,
    TG_NVDEC_ERR_FIRMWARE_HASH,
    TG_NVDEC_ERR_FIRMWARE_DMA,
    TG_NVDEC_ERR_DMA_SCRUB,
    TG_NVDEC_ERR_DMA_TIMEOUT,
    TG_NVDEC_ERR_BOOT_TIMEOUT,
    TG_NVDEC_ERR_BOOT_HALT,
    TG_NVDEC_ERR_SUBMIT,
    TG_NVDEC_ERR_TIMEOUT,
    TG_NVDEC_ERR_CANCELLED,
    TG_NVDEC_ERR_ENGINE,
    TG_NVDEC_ERR_UNSUPPORTED,
    TG_NVDEC_ERR_DIMENSIONS,
    TG_NVDEC_ERR_SURFACE,
    TG_NVDEC_ERR_SLICE,
    TG_NVDEC_ERR_DPB,
    TG_NVDEC_ERR_STATE,
};

/* Only the NVDEC aperture is accessed through read32/write32.  Host1x
 * channel programming, DMA assignment and the completion wait live in
 * submit(), because they are platform policy.  submit() must consume exactly
 * `words` gather words and return only after the Host1x syncpoint reaches
 * `syncpt_value`; it must never advance the syncpoint from the CPU. */
struct tg_nvdec_hal {
    void *ctx;
    uint32_t (*read32)(void *ctx, uint32_t offset);
    void (*write32)(void *ctx, uint32_t offset, uint32_t value);
    void (*delay_us)(void *ctx, uint32_t microseconds);
    bool (*dma_alloc)(void *ctx, size_t size, struct tg_nvdec_dma *out);
    void (*dma_free)(void *ctx, struct tg_nvdec_dma *dma);
    /* Flush CPU writes before the engine reads the buffer, and invalidate
     * before the CPU reads engine output.  Optional for coherent memory. */
    void (*dma_sync)(void *ctx, const struct tg_nvdec_dma *dma, bool to_device);
    /* The HAL reads the channel's current syncpoint, arms exactly one
     * increment, and waits for the engine to advance it.  expected receives
     * that target and observed the value read on return.  No CPU write to the
     * syncpoint is permitted. */
    enum tg_nvdec_result (*submit)(void *ctx, const struct tg_nvdec_dma *gather,
                                   uint32_t words, uint32_t syncpt_id,
                                   uint32_t timeout_ms, uint32_t *expected,
                                   uint32_t *observed);
    /* Return true only after Host1x and NVDEC can no longer access any DMA
     * allocation owned by this context. A successful quiesce invalidates
     * the Falcon boot state; false requires retaining all allocations. */
    bool (*cancel)(void *ctx);
};

struct tg_nvdec_surface {
    bool valid;
    uint32_t slot;          /* 0..16, the DPB output slot */
    uint64_t luma_bus;      /* NV12 Y plane */
    uint64_t chroma_bus;    /* NV12 interleaved CbCr plane */
};

struct tg_nvdec_h264_ref {
    uint32_t slot;          /* output surface slot of the reference */
    uint32_t frame_num;     /* short-term FrameNum, or long-term index */
    int32_t top_poc;
    int32_t bottom_poc;
    bool is_long_term;
    bool not_existing;
    bool is_field;
};

/* H.264 picture parameters consumed by NVDEC's H.264 picture setup.  This
 * mirrors the subset of DXVA_PicParams_H264 / VAPictureParameterBufferH264
 * that the engine needs; the D3D11VA layer maps its named structures here. */
struct tg_nvdec_h264_pic {
    uint32_t width;              /* visible, cropped width */
    uint32_t height;             /* visible, cropped height */
    uint32_t coded_width;        /* PicWidthInMbs * 16 */
    uint32_t coded_height;       /* FrameHeightInMbs * 16 */
    uint32_t pitch_luma;
    uint32_t pitch_chroma;

    uint32_t chroma_format_idc;  /* must be 1 (4:2:0) */
    uint32_t bit_depth_luma;     /* must be 8 */
    uint32_t bit_depth_chroma;   /* must be 8 */
    uint32_t log2_max_frame_num_minus4;
    uint32_t pic_order_cnt_type; /* 0 or 2 */
    uint32_t log2_max_pic_order_cnt_lsb_minus4;
    bool delta_pic_order_always_zero_flag;
    bool frame_mbs_only_flag;
    bool mb_adaptive_frame_field_flag;
    bool direct_8x8_inference_flag;
    bool constrained_intra_pred_flag;
    bool ref_pic_flag;
    bool entropy_coding_mode_flag;
    bool pic_order_present_flag;
    bool weighted_pred_flag;
    uint32_t weighted_bipred_idc;
    int32_t pic_init_qp_minus26;
    int32_t chroma_qp_index_offset;
    int32_t second_chroma_qp_index_offset;
    bool deblocking_filter_control_present_flag;
    bool redundant_pic_cnt_present_flag;
    bool transform_8x8_mode_flag;
    bool field_pic_flag;
    bool bottom_field_flag;
    bool lossless_ipred8x8_filter_enable;
    bool qpprime_y_zero_transform_bypass_flag;

    uint32_t frame_num;
    uint32_t curr_slot;          /* output slot of this picture */
    int32_t curr_top_poc;
    int32_t curr_bottom_poc;
    uint32_t num_ref_idx_l0_active_minus1;
    uint32_t num_ref_idx_l1_active_minus1;

    uint8_t scaling4x4[6][16];   /* raster order */
    uint8_t scaling8x8[2][64];   /* raster order */

    uint8_t dpb_count;
    struct tg_nvdec_h264_ref dpb[TG_NVDEC_MAX_DPB];
};

struct tg_nvdec_h264_job {
    const uint8_t *bitstream;       /* Annex B, start codes included */
    size_t bitstream_size;
    const uint32_t *slice_offsets;  /* slice_count + 1 entries */
    uint32_t slice_count;
    struct tg_nvdec_h264_pic pic;
    struct tg_nvdec_surface surfaces[TG_NVDEC_MAX_SURFACES];
};

struct tg_nvdec_status {
    uint32_t mbs_correctly_decoded;
    uint32_t mbs_in_error;
    uint32_t cycles;
    uint32_t error_status;
    uint32_t slice_header_error_code;
};

struct tg_nvdec_stats {
    uint64_t submits;
    uint64_t completions;
    uint64_t timeouts;
    uint64_t cancelled;
    uint64_t failures;
    uint64_t firmware_bytes;
    uint32_t fw_version;
    uint32_t last_syncpoint;
    uint32_t last_observed;
};

struct tg_nvdec {
    const struct tg_nvdec_hal *hal;
    uint32_t syncpt_id;
    bool firmware_loaded;
    bool booted;
    bool in_flight;

    struct tg_nvdec_dma firmware;
    struct tg_nvdec_dma arena;
    /* Decoder-session state. Reference motion/history data must not move
     * with per-picture bitstream size or command-arena growth. */
    struct tg_nvdec_dma scratch;
    uint32_t scratch_coded_width, scratch_coded_height;
    size_t arena_used;

    uint32_t fw_version;
    uint8_t fw_sha256[TG_NVDEC_SHA256_SIZE];

    uint32_t picture_index;
    struct tg_nvdec_stats stats;
    struct tg_nvdec_status last_status;
    uint32_t last_regs[8];
    char error[224];
};

/* Command-buffer inputs.  All addresses are bus addresses the NVDEC engine
 * may read or write.  The builder is pure so the host tests can compare the
 * complete method stream without a device. */
struct tg_nvdec_cmd_buffers {
    uint64_t config;
    uint64_t bitstream;
    uint64_t slice_offsets;
    uint64_t coloc;
    uint64_t history;
    uint64_t mbhist;
    uint64_t status;
    uint32_t stream_len;
    uint32_t slice_count;
    uint32_t history_size_256;
    uint32_t mbhist_size;
    uint32_t picture_index;
    uint32_t syncpt_id;
    struct {
        bool valid;
        uint64_t luma;
        uint64_t chroma;
    } surfaces[TG_NVDEC_MAX_SURFACES];
};

void tg_nvdec_init(struct tg_nvdec *nvdec, const struct tg_nvdec_hal *hal,
                   uint32_t syncpt_id);
enum tg_nvdec_result tg_nvdec_load_firmware(struct tg_nvdec *nvdec,
                                            const void *data, size_t size,
                                            const uint8_t expected_sha256[TG_NVDEC_SHA256_SIZE]);
enum tg_nvdec_result tg_nvdec_boot(struct tg_nvdec *nvdec);
enum tg_nvdec_result tg_nvdec_submit_h264(struct tg_nvdec *nvdec,
                                          const struct tg_nvdec_h264_job *job,
                                          struct tg_nvdec_status *status);
enum tg_nvdec_result tg_nvdec_cancel(struct tg_nvdec *nvdec);
/* On failure all DMA storage and ownership are retained for recovery. */
enum tg_nvdec_result tg_nvdec_close(struct tg_nvdec *nvdec);
enum tg_nvdec_result tg_nvdec_set_error(struct tg_nvdec *nvdec,
                                        enum tg_nvdec_result result,
                                        const char *what);
const char *tg_nvdec_result_name(enum tg_nvdec_result result);

/* Pure builders.  Return the number of 32-bit words written, or 0 when the
 * output buffer is too small or the descriptor is invalid. */
size_t tg_nvdec_build_h264_cmd(uint32_t *out, size_t words,
                               const struct tg_nvdec_cmd_buffers *bufs);
size_t tg_nvdec_build_pic_config(void *out, size_t bytes,
                                 const struct tg_nvdec_h264_pic *pic,
                                 uint32_t stream_len, uint32_t slice_count,
                                 uint32_t history_size_256, uint32_t mbhist_size);

/* The config struct is NVIDIA's; expose its size to callers without making
 * them include the large NVIDIA header. */
size_t tg_nvdec_pic_config_size(void);

/* SHA-256 of one buffer.  Used to pin the firmware blob before it is parsed
 * and executed. */
void tg_nvdec_sha256(const void *data, size_t size, uint8_t out[TG_NVDEC_SHA256_SIZE]);

#ifdef __cplusplus
}
#endif

#endif
