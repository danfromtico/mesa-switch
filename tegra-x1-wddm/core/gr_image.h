/* SPDX-License-Identifier: MIT */
#ifndef TG_GPU_GR_IMAGE_H
#define TG_GPU_GR_IMAGE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TG_GPU_GR_IMAGE_HEADER_BYTES 256u
#define TG_GPU_GR_IMAGE_MAX_BYTES 0x20000u
#define TG_GPU_GR_ACCESS_MAP_BYTES 0x80000u
struct tg_gpu_gr_patch { uint32_t address, value; };
struct tg_gpu_gr_image_config {
    uint64_t access_map_va, patch_va;
    size_t patch_bytes;
    uint32_t patch_count, context_id;
};
enum tg_gpu_gr_image_result {
    TG_GPU_GR_IMAGE_OK, TG_GPU_GR_IMAGE_ARGUMENT,
    TG_GPU_GR_IMAGE_FORMAT, TG_GPU_GR_IMAGE_ADDRESS
};
/* Trusted KMD-owned storage only. The caller owns GPU mappings/lifetime and
 * synchronization; these byte builders perform no MMIO or fence writes.
 * The golden image is immutable and must not overlap the output. All input
 * validation precedes writes. This is not validation of application commands.
 */
enum tg_gpu_gr_image_result tg_gpu_gr_image_build(void *output, size_t capacity,
    const void *golden, size_t image_bytes, const struct tg_gpu_gr_image_config *);
/* GM20B's vendor whitelist; every other privilege-map bit is zero. */
bool tg_gpu_gr_access_map_build(void *output, size_t capacity);
/* GPU patch records are little-endian register/value pairs. */
bool tg_gpu_gr_patch_build(void *output, size_t capacity,
    const struct tg_gpu_gr_patch *records, uint32_t count);
#endif
