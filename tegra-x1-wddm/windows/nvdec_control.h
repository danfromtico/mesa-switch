/* SPDX-License-Identifier: MIT
 *
 * Private bring-up ABI between the SwitchWOA NVDEC acceptance tool and the
 * TegraGpuProbe kernel driver.  This is not a WDDM/UMD contract and must not
 * be advertised as D3D11VA support.  Only 1280x720 8-bit 4:2:0 progressive
 * H.264 is accepted by the current backend.
 */
#ifndef TGD3D_NVDEC_CONTROL_H
#define TGD3D_NVDEC_CONTROL_H

#include <stdint.h>
#include "../../src/nouveau/nvdec/nvdec.h"

#define TG_NVDEC_CONTROL_VERSION 1u
#define TG_NVDEC_ERROR_SIZE 160u

#define TG_GPU_IOCTL_NVDEC_LOAD      0x0022a420u
#define TG_GPU_IOCTL_NVDEC_CONFIGURE 0x0022a424u
#define TG_GPU_IOCTL_NVDEC_DECODE    0x0022a428u
#define TG_GPU_IOCTL_NVDEC_STATUS    0x0022a42cu
#define TG_GPU_IOCTL_NVDEC_RESET     0x0022a430u

struct tg_nvdec_load_request {
    uint32_t version;
    uint32_t size;
    uint8_t sha256[TG_NVDEC_SHA256_SIZE];
    /* firmware bytes follow */
};

struct tg_nvdec_load_result {
    uint32_t version;
    uint32_t result;
    uint32_t firmware_bytes;
    uint32_t booted;
    uint8_t sha256[TG_NVDEC_SHA256_SIZE];
    char error[TG_NVDEC_ERROR_SIZE];
};

struct tg_nvdec_config_request {
    uint32_t version;
    uint32_t width;
    uint32_t height;
};

struct tg_nvdec_config_result {
    uint32_t version;
    uint32_t result;
    uint32_t pitch;
    uint32_t aligned_height;
    uint32_t frame_size;
    char error[TG_NVDEC_ERROR_SIZE];
};

struct tg_nvdec_decode_request {
    uint32_t version;
    uint32_t bitstream_size;
    uint32_t slice_count;
    uint32_t output_size;
    struct tg_nvdec_h264_pic pic;
    /* bitstream_size bytes follow, then (slice_count + 1) uint32 offsets */
};

struct tg_nvdec_decode_result {
    uint32_t version;
    uint32_t result;
    uint32_t mbs_correctly_decoded;
    uint32_t mbs_in_error;
    uint32_t cycles;
    uint32_t error_status;
    uint32_t slice_header_error_code;
    uint32_t expected_syncpoint;
    uint32_t observed_syncpoint;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t aligned_height;
    uint32_t frame_size;
    uint32_t bytes_written;
    char error[TG_NVDEC_ERROR_SIZE];
    /* NV12 bytes follow when result == 0 */
};

struct tg_nvdec_status_result {
    uint32_t version;
    uint32_t created;
    uint32_t firmware_ready;
    uint32_t surfaces_ready;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t aligned_height;
    uint32_t frame_size;
    char error[TG_NVDEC_ERROR_SIZE];
};

#endif
