/* SPDX-License-Identifier: MIT */
#ifndef TG_GPU_ACR_FIRMWARE_H
#define TG_GPU_ACR_FIRMWARE_H
#include "gr_firmware.h"
#include <stdbool.h>

enum tg_gpu_acr_result {
    TG_GPU_ACR_OK, TG_GPU_ACR_ARGUMENT, TG_GPU_ACR_FORMAT,
    TG_GPU_ACR_SIZE, TG_GPU_ACR_SIGNATURE_PROFILE, TG_GPU_ACR_ADDRESS
};
struct tg_gpu_acr_files {
    struct tg_gpu_gr_blob hs, hs_boot, pmu, pmu_desc, pmu_sig, fecs_sig;
    const struct tg_gpu_gr_payload *gr;
};
struct tg_gpu_acr_lsf {
    struct tg_gpu_gr_blob image, signature;
    uint32_t id, boot_size, boot_imem, app_start, app_size;
    uint32_t code_offset, code_size, data_offset, data_size, entry;
    uint32_t ucode_size, full_data_size;
    uint32_t lsb_offset, image_offset, loader_offset;
};
struct tg_gpu_acr_image {
    struct tg_gpu_gr_blob hs, boot, hs_signature;
    uint32_t signature_offset, nonsec_offset, nonsec_size;
    uint32_t secure_offset, secure_size, dmem_offset, dmem_size;
    uint32_t boot_tag, boot_dmem, pmu_version;
    uint32_t boot_offset, wpr_offset, wpr_size, size;
    struct tg_gpu_acr_lsf lsf[2]; /* Switchroot prepend order: FECS, PMU. */
};
struct tg_gpu_acr_addresses {
    uint64_t gpu_va, physical, wpr_base, wpr_size;
    uint32_t pmu_dmem_size, pmu_args_size;
};
/* Parse the GM20B/T21x signed-firmware format and plan the staging image.
 * Views borrow the immutable input until packing finishes. This checks format
 * and signature availability, NOT authenticity: the PMU HS bootloader and ACR
 * must authenticate the unchanged vendor payload on the physical GPU.
 */
enum tg_gpu_acr_result tg_gpu_acr_prepare(const struct tg_gpu_acr_files *files,
    bool debug_signatures, struct tg_gpu_acr_image *image);
/* Pack into separate driver-owned storage. Addresses come from owned backing,
 * its GPU mapping, and the hardware-reported reserved WPR carveout. No MMIO or
 * CPU access to WPR occurs here. The PMU boot descriptor contains 19 LE words.
 * This only prepares a blob; it does not start ACR or report firmware success.
 */
enum tg_gpu_acr_result tg_gpu_acr_pack(const struct tg_gpu_acr_image *image,
    const struct tg_gpu_acr_addresses *addresses, void *storage, size_t capacity,
    uint32_t boot_descriptor[19]);
#endif
