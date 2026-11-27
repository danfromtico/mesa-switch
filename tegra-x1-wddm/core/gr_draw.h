/* SPDX-License-Identifier: MIT */
#ifndef TG_GPU_GR_DRAW_H
#define TG_GPU_GR_DRAW_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "draw_sm53.h"

#define TG_GPU_DRAW_OFFSET 0x1a0000u
#define TG_GPU_DRAW_BYTES 8192u
#define TG_GPU_DRAW_MAX_WORDS 512u
struct tg_gpu_draw_status {
    uint32_t requested, checked, sm, vs_bytes, fs_bytes, vs_gprs, fs_gprs, code_hash;
};
/* Copy only NAK-produced code/header data into two owned read-only GPU pages.
 * Header starts at 0x30, so its following 80-byte SPH aligns code to 0x80.
 * No expected pixel or report value is written into the target by this code.
 */
bool tg_gpu_prepare_draw_code(void *code,size_t bytes,struct tg_gpu_draw_status *);
bool tg_gpu_check_draw_code(const volatile uint32_t *code,size_t bytes);
/* Requires the first (blue) clear packet's color target, then emits a vertex-ID
 * triangle with a central 8x8 scissor and constant-red fragment shader.
 * Fixed diagnostic methods derived from NVK, not an application command stream
 * or a Vulkan API pass. The second packet contains no CLEAR_SURFACE method.
 */
bool tg_gpu_build_draw_packet(uint32_t *commands,size_t capacity_words,
    uint64_t code_va,uint64_t report_va,uint32_t report_value,uint32_t *word_count);
#endif
