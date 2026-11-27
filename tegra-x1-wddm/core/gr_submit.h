/* SPDX-License-Identifier: MIT */
#ifndef TG_GPU_GR_SUBMIT_H
#define TG_GPU_GR_SUBMIT_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TG_GPU_GR_ENGINE_CLASS 0xb197u
#define TG_GPU_GR_ENGINE_FIRST_WORDS 10u
#define TG_GPU_GR_ENGINE_NEXT_WORDS 8u
/* Fixed, KMD-owned workload: optionally bind Maxwell B on subchannel zero,
 * issue NO_OPERATION/WAIT_FOR_IDLE and the unchanged Mesa 3D report packet.
 * The caller owns a ready GR image, mappings and synchronization. These
 * packets exercise the graphics engine, not a shader or a Vulkan draw.
 */
bool tg_gpu_build_gr_engine_packet(uint32_t *output,size_t capacity_words,
    uint64_t report_va,uint32_t value,bool bind_object);
#endif
