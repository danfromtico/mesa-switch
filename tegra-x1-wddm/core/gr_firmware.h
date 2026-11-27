/* SPDX-License-Identifier: MIT */
#ifndef TG_GPU_GR_FIRMWARE_H
#define TG_GPU_GR_FIRMWARE_H
#include <stddef.h>
#include <stdint.h>

enum tg_gpu_gr_firmware_result {
    TG_GPU_GR_FW_OK, TG_GPU_GR_FW_ARGUMENT, TG_GPU_GR_FW_FORMAT,
    TG_GPU_GR_FW_BOOT_VERSION, TG_GPU_GR_FW_SIZE, TG_GPU_GR_FW_ADDRESS
};
enum tg_gpu_gr_region_id {
    TG_GPU_GR_FECS_DATA, TG_GPU_GR_FECS_CODE,
    TG_GPU_GR_GPCCS_DATA, TG_GPU_GR_GPCCS_CODE,
    TG_GPU_GR_BUNDLE, TG_GPU_GR_CONTEXT, TG_GPU_GR_NONCONTEXT, TG_GPU_GR_METHOD,
    TG_GPU_GR_REGION_COUNT = 36
};
struct tg_gpu_gr_blob { const uint8_t *data; size_t size; };
/* Borrowed views: retain the immutable input until the caller has consumed it.
 * NETB payload offsets can be unaligned. Do not cast them to uint32_t arrays.
 */
struct tg_gpu_gr_netlist {
    uint32_t version, major, buffer_size, ctxsw_base, number;
    struct tg_gpu_gr_blob region[TG_GPU_GR_REGION_COUNT];
};
struct tg_gpu_gr_segment { uint32_t offset, size; };
struct tg_gpu_gr_falcon_image {
    struct tg_gpu_gr_segment boot, code, data;
    uint32_t imem_offset, entry, boot_signature;
};
struct tg_gpu_gr_image {
    uint32_t size;
    struct tg_gpu_gr_falcon_image falcon[2]; /* FECS, GPCCS */
};
/* CPU-owned, immutable input. Execution copies only the packed surface into
 * its bounded GPU arena; the NETB views remain valid until bootstrap returns.
 */
struct tg_gpu_acr_files;
struct tg_gpu_gr_payload {
    struct tg_gpu_gr_netlist netlist;
    struct tg_gpu_gr_image image;
    const uint8_t *surface;
    const struct tg_gpu_acr_files *acr;
};
enum tg_gpu_gr_firmware_result tg_gpu_gr_netlist_parse(
    struct tg_gpu_gr_blob input, struct tg_gpu_gr_netlist *out);
/* Match Switchroot's DMA-bootstrap surface: FECS boot/code/data followed by
 * GPCCS boot/code/data; each segment starts at a 256-byte boundary. With NULL/0
 * storage this validates and calculates the allocation size. Otherwise the
 * caller supplies separate writable storage, disjoint from the input files.
 * All validation precedes writes. Successful packing is NOT firmware startup.
 */
enum tg_gpu_gr_firmware_result tg_gpu_gr_image_prepare(
    struct tg_gpu_gr_blob netlist, const struct tg_gpu_gr_blob boot[2],
    void *storage, size_t capacity, struct tg_gpu_gr_image *out);
/* Encode the T21x bootstrap DMEM header for a mapped, caller-owned GPU VA.
 * Checksums select the upstream header ABI; they are not authentication.
 * Loading/binding, secure ACR bootstrap and the ready handshake are separate.
 */
enum tg_gpu_gr_firmware_result tg_gpu_gr_boot_header(
    const struct tg_gpu_gr_image *image, unsigned falcon, uint64_t gpu_va,
    uint32_t words[17], uint32_t *count);
#endif
