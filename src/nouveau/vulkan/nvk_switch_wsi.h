/*
 * Copyright © 2026 Mesa Switch port contributors
 * SPDX-License-Identifier: MIT
 *
 * Minimal NVK-side shim for the libnx-backed WSI. This header is
 * deliberately kept free of NVK internals so that wsi_common_switch.c,
 * which lives in src/vulkan/wsi/ and only has src/ in its include path,
 * can consume it via "nouveau/vulkan/nvk_switch_wsi.h".
 */
#ifndef NVK_SWITCH_WSI_H
#define NVK_SWITCH_WSI_H 1

#ifdef __SWITCH__

#include <stdbool.h>
#include <stdint.h>

#include <switch.h>
#include <vulkan/vulkan_core.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Horizon scanout layout: a single-plane, non-disjoint, block-linear image
 * with dedicated NvMap-backed memory.
 */
struct nvk_switch_scanout_layout {
   /* Numeric NvMap ID owning the pixels.  The memory object retains the
    * underlying map for at least as long as this image remains bound.
    */
   uint32_t nvmap_id;

   /* Offset of the image inside the NvMap and total byte size of the
    * image (nil plane size, already block-aligned).
    */
   uint32_t offset_B;
   uint32_t size_B;

   /* Row stride of level 0 in bytes and in pixels.
    * NvGraphicBuffer.stride is pixels; planes[0].pitch is bytes.
    */
   uint32_t row_stride_B;
   uint32_t row_stride_px;

   /* Block height (tile Y) as log2(GOBs). Must match what NVK fed into
    * nil; the compositor uses this to deswizzle scanout reads.
    */
   uint8_t block_height_log2;

   /* Page-table kind. Must match the MMU PTE kind the driver programmed
    * or compositor scanout will read garbage. For BGRA8/RGBA8 on GM20B
    * this is NvKind_Generic_16BX2.
    */
   uint8_t pte_kind;
};

/* Requires a bound image/memory pair from the same device: single-plane,
 * non-disjoint, tiled image with dedicated Switch memory. Returns
 * VK_ERROR_FEATURE_NOT_PRESENT for unsupported pairs.
 */
VkResult nvk_switch_get_scanout_layout(VkImage image,
                                       VkDeviceMemory memory,
                                       struct nvk_switch_scanout_layout *out);

VkResult nvk_switch_allocate_shared_memory(
   VkDevice device,
   const VkMemoryAllocateInfo *allocate_info,
   const VkAllocationCallbacks *allocator,
   uint32_t nvmap_id,
   VkDeviceMemory *memory_out);

bool nvk_switch_export_memory(VkDeviceMemory memory,
                              uint32_t *nvmap_id_out,
                              void **reference_out);

void nvk_switch_release_memory_reference(void *reference);

/* Export an installed native VkFence payload. If absent or CPU-only,
 * return false; presentation must wait on the Vulkan fence instead.
 */
bool nvk_switch_fence_peek_nvmultifence(VkFence fence, NvMultiFence *out);
bool nvk_switch_fence_peek_nvfence(VkFence fence, NvFence *out);

/* Import a native release fence as a temporary Vulkan acquire payload.
 * VK_ERROR_FEATURE_NOT_PRESENT requires a CPU wait and dummy sync
 * fallback.
 */
VkResult nvk_switch_semaphore_import_nvfence(VkDevice device,
                                             VkSemaphore semaphore,
                                             const NvFence *fence);
VkResult nvk_switch_semaphore_import_nvmultifence(VkDevice device,
                                                  VkSemaphore semaphore,
                                                  const NvMultiFence *fence);
VkResult nvk_switch_fence_import_nvfence(VkDevice device,
                                         VkFence fence,
                                         const NvFence *fence_in);
VkResult nvk_switch_fence_import_nvmultifence(VkDevice device,
                                              VkFence fence,
                                              const NvMultiFence *fence_in);

#ifdef __cplusplus
}
#endif

#endif /* __SWITCH__ */

#endif /* NVK_SWITCH_WSI_H */
