/* SPDX-License-Identifier: MIT */
#ifndef TG_GPU_GR_CLEAR_H
#define TG_GPU_GR_CLEAR_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TG_GPU_CLEAR_WIDTH 16u
#define TG_GPU_CLEAR_HEIGHT 16u
#define TG_GPU_CLEAR_PITCH 128u
#define TG_GPU_CLEAR_FORMAT 0xd5u /* A8B8G8R8 UNORM */
#define TG_GPU_CLEAR_BYTES 4096u
#define TG_GPU_CLEAR_OFFSET 0x190000u
#define TG_GPU_CLEAR_SENTINEL 0xdeadbeefu
#define TG_GPU_CLEAR_BACKGROUND 0xffff0000u /* RGBA: blue */
#define TG_GPU_CLEAR_FOREGROUND 0xff0000ffu /* RGBA: red */
#define TG_GPU_CLEAR_MAX_WORDS 128u
struct tg_gpu_clear_status {
    uint32_t checked, width, height, pitch, format, pixels;
    uint32_t errors, guard_errors, background, foreground, image_hash;
};
/* Fixed owned 16x16 pitch target. First binds B197 and clears blue;
 * second clears its central 8x8 red rectangle. Both end in a Mesa 3D report.
 * These implement the corresponding NVK surface/clear methods, without a
 * shader or application command stream. No CPU success pixels are written.
 */
bool tg_gpu_build_clear_packet(uint32_t *commands,size_t capacity_words,
    uint64_t image_va,uint64_t report_va,uint32_t report_value,bool first,
    uint32_t *word_count);
/* Read only after the caller proves engine completion and memory visibility. */
bool tg_gpu_check_clear_pixels(const volatile uint32_t *pixels,size_t bytes,
                              struct tg_gpu_clear_status *);
#endif
