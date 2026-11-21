/*
 * Copyright © 2022 Collabora Ltd. and Red Hat Inc.
 * SPDX-License-Identifier: MIT
 */
#include "nvk_wsi.h"
#include "nvk_instance.h"
#include "nvk_image.h"
#include "nvk_device_memory.h"
#include "nvkmd/nvkmd.h"
#include "wsi_common.h"

#ifdef __SWITCH__
#include "nvk_switch_wsi.h"
#include "nvkmd/switch/nvkmd_switch.h"
#include "util/format/u_format.h"
#include "util/macros.h"
#include "vk_device.h"
#include "vk_fence.h"
#include "vk_semaphore.h"
#include "vk_sync.h"
#endif

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
nvk_wsi_proc_addr(VkPhysicalDevice physicalDevice, const char *pName)
{
   VK_FROM_HANDLE(nvk_physical_device, pdev, physicalDevice);
#ifdef __SWITCH__
   /* Internal WSI helpers must bypass the application's Vulkan
    * version/extension gates. Try the unchecked runtime resolver, then the
    * public loaderless resolver for remaining symbols.
    */
   PFN_vkVoidFunction func =
      vk_instance_get_proc_addr_unchecked(pdev->vk.instance, pName);
   if (func != NULL)
      return func;

   return vkGetInstanceProcAddr(nvk_instance_to_handle(
                                   (struct nvk_instance *)
                                   nvk_physical_device_instance(pdev)),
                                pName);
#else
   return vk_instance_get_proc_addr_unchecked(pdev->vk.instance, pName);
#endif
}

VkResult
nvk_init_wsi(struct nvk_physical_device *pdev)
{
   VkResult result;

   struct wsi_device_options wsi_options = {
      .sw_device = false
   };
   result = wsi_device_init(&pdev->wsi_device,
                            nvk_physical_device_to_handle(pdev),
                            nvk_wsi_proc_addr, &pdev->vk.instance->alloc,
                            nvkmd_pdev_get_drm_primary_fd(pdev->nvkmd),
                            &nvk_physical_device_instance(pdev)->drirc.options,
                            &wsi_options);
   if (result != VK_SUCCESS)
      return result;

   pdev->wsi_device.supports_scanout = false;
#ifdef VK_USE_PLATFORM_WIN32_KHR
   /* NVK exposes no D3D12 command queue, so the Win32 WSI cannot use the
    * DXGI/DirectComposition direct-flip entry and presents through the GDI
    * path instead: one GPU staging copy moves each finished frame into a
    * host-visible linear image (the WaitForFences in queue_present stays),
    * and GDI submits the mapped pixels. Do not declare DXGI support or drop
    * that GPU wait to fake a zero-readback path. GDI itself does not
    * throttle the producer even with FIFO selected, so enable a 60 Hz
    * producer cap whose deadline phase-locks to the measured compositor
    * cadence (DwmFlush) instead of free-running. */
   pdev->wsi_device.win32.cpu_present_fps = 60;
#endif
   pdev->wsi_device.supports_modifiers =
      pdev->vk.supported_extensions.table.EXT_image_drm_format_modifier;

   pdev->vk.wsi_device = &pdev->wsi_device;

   return result;
}

void
nvk_finish_wsi(struct nvk_physical_device *pdev)
{
   pdev->vk.wsi_device = NULL;
   wsi_device_finish(&pdev->wsi_device, &pdev->vk.instance->alloc);
}

#ifdef __SWITCH__
VkResult
nvk_switch_get_scanout_layout(VkImage _image, VkDeviceMemory _memory,
                              struct nvk_switch_scanout_layout *out)
{
   VK_FROM_HANDLE(nvk_image, image, _image);
   VK_FROM_HANDLE(nvk_device_memory, mem, _memory);

   if (image == NULL || mem == NULL)
      return VK_ERROR_FEATURE_NOT_PRESENT;

   if (image->disjoint || image->plane_count != 1)
      return VK_ERROR_FEATURE_NOT_PRESENT;

   const struct nvk_image_plane *plane = &image->planes[0];

   /* 5b.A's dedicated-alloc gate requires pte_kind != 0 and a dedicated
    * binding; enforce the same invariant here so any mismatch falls back
    * to the CPU-blit WSI path instead of feeding garbage to the compositor.
    */
   if (plane->nil.pte_kind == 0)
      return VK_ERROR_FEATURE_NOT_PRESENT;

   if (plane->nil.num_levels == 0)
      return VK_ERROR_FEATURE_NOT_PRESENT;

   const struct nil_image_level *lvl0 = &plane->nil.levels[0];

   /* GM20B 2D block-linear: x_log2/z_log2 are always 0, y_log2 is the one
    * the compositor cares about (maps 1:1 to NvGraphicBuffer.block_height_log2).
    */
   if (lvl0->tiling.x_log2 != 0 || lvl0->tiling.z_log2 != 0)
      return VK_ERROR_FEATURE_NOT_PRESENT;

   const struct util_format_description *fmt_desc =
      util_format_description(plane->nil.format.p_format);
   const uint32_t block_size_B = fmt_desc->block.bits / 8;
   if (block_size_B == 0 || (lvl0->row_stride_B % block_size_B) != 0)
      return VK_ERROR_FEATURE_NOT_PRESENT;

   out->nvmap_id = nvkmd_switch_mem_get_nvmap_id(mem->mem);
   out->offset_B = (uint32_t)lvl0->offset_B;
   out->size_B = (uint32_t)plane->nil.size_B;
   out->row_stride_B = lvl0->row_stride_B;
   out->row_stride_px = lvl0->row_stride_B / block_size_B;
   out->block_height_log2 = lvl0->tiling.y_log2;
   out->pte_kind = plane->nil.pte_kind;

   return VK_SUCCESS;
}

bool
nvk_switch_fence_peek_nvmultifence(VkFence _fence, NvMultiFence *out)
{
   VK_FROM_HANDLE(vk_fence, fence, _fence);
   if (fence == NULL)
      return false;

   struct vk_sync *sync = vk_fence_get_active_sync(fence);
   return nvkmd_switch_sync_peek_nvmultifence(sync, out);
}

bool
nvk_switch_fence_peek_nvfence(VkFence _fence, NvFence *out)
{
   NvMultiFence mf;
   if (!nvk_switch_fence_peek_nvmultifence(_fence, &mf))
      return false;

   if (mf.num_fences != 1)
      return false;

   if (out != NULL)
      *out = mf.fences[0];

   return true;
}

static const struct vk_sync_type *
nvk_switch_binary_sync_type(struct vk_device *device)
{
   if (device == NULL || device->physical->supported_sync_types == NULL)
      return NULL;

   for (const struct vk_sync_type *const *t =
        device->physical->supported_sync_types; *t != NULL; t++) {
      if (((*t)->features & VK_SYNC_FEATURE_BINARY) &&
          nvkmd_switch_sync_is_nvfence(*t))
         return *t;
   }

   return NULL;
}

static uint32_t
nvk_switch_multifence_valid_count(const NvMultiFence *fence)
{
   if (fence == NULL)
      return 0;

   const uint32_t fence_count = MIN2(fence->num_fences, ARRAY_SIZE(fence->fences));
   uint32_t valid_count = 0;
   for (uint32_t i = 0; i < fence_count; i++) {
      if ((int32_t)fence->fences[i].id >= 0)
         valid_count++;
   }

   return valid_count;
}

static VkResult
nvk_switch_create_sync_from_nvmultifence(struct vk_device *device,
                                         const NvMultiFence *fence,
                                         struct vk_sync **sync_out)
{
   if (device == NULL || fence == NULL ||
       nvk_switch_multifence_valid_count(fence) == 0)
      return VK_ERROR_FEATURE_NOT_PRESENT;

   const struct vk_sync_type *sync_type = nvk_switch_binary_sync_type(device);
   if (sync_type == NULL)
      return VK_ERROR_FEATURE_NOT_PRESENT;

   VkResult result = vk_sync_create(device, sync_type,
                                    0 /* flags */, 0 /* initial_value */,
                                    sync_out);
   if (result != VK_SUCCESS)
      return result;

   nvkmd_switch_sync_import_nvmultifence(*sync_out, fence);
   return VK_SUCCESS;
}

static VkResult
nvk_switch_create_sync_from_nvfence(struct vk_device *device,
                                    const NvFence *fence,
                                    struct vk_sync **sync_out)
{
   NvMultiFence mf;
   if (fence == NULL || (int32_t)fence->id < 0)
      return VK_ERROR_FEATURE_NOT_PRESENT;

   nvMultiFenceCreate(&mf, fence);
   return nvk_switch_create_sync_from_nvmultifence(device, &mf, sync_out);
}

VkResult
nvk_switch_semaphore_import_nvfence(VkDevice _device,
                                    VkSemaphore _semaphore,
                                    const NvFence *fence)
{
   VK_FROM_HANDLE(vk_device, device, _device);
   VK_FROM_HANDLE(vk_semaphore, semaphore, _semaphore);

   if (device == NULL || semaphore == NULL ||
       semaphore->type != VK_SEMAPHORE_TYPE_BINARY)
      return VK_ERROR_FEATURE_NOT_PRESENT;

   vk_semaphore_reset_temporary(device, semaphore);

   struct vk_sync *temporary = NULL;
   VkResult result =
      nvk_switch_create_sync_from_nvfence(device, fence, &temporary);
   if (result != VK_SUCCESS)
      return result;

   semaphore->temporary = temporary;
   return VK_SUCCESS;
}

VkResult
nvk_switch_semaphore_import_nvmultifence(VkDevice _device,
                                         VkSemaphore _semaphore,
                                         const NvMultiFence *fence)
{
   VK_FROM_HANDLE(vk_device, device, _device);
   VK_FROM_HANDLE(vk_semaphore, semaphore, _semaphore);

   if (device == NULL || semaphore == NULL ||
       semaphore->type != VK_SEMAPHORE_TYPE_BINARY)
      return VK_ERROR_FEATURE_NOT_PRESENT;

   vk_semaphore_reset_temporary(device, semaphore);

   struct vk_sync *temporary = NULL;
   VkResult result =
      nvk_switch_create_sync_from_nvmultifence(device, fence, &temporary);
   if (result != VK_SUCCESS)
      return result;

   semaphore->temporary = temporary;
   return VK_SUCCESS;
}

VkResult
nvk_switch_fence_import_nvfence(VkDevice _device,
                                VkFence _fence,
                                const NvFence *fence_in)
{
   VK_FROM_HANDLE(vk_device, device, _device);
   VK_FROM_HANDLE(vk_fence, fence, _fence);

   if (device == NULL || fence == NULL)
      return VK_ERROR_FEATURE_NOT_PRESENT;

   vk_fence_reset_temporary(device, fence);

   struct vk_sync *temporary = NULL;
   VkResult result =
      nvk_switch_create_sync_from_nvfence(device, fence_in, &temporary);
   if (result != VK_SUCCESS)
      return result;

   fence->temporary = temporary;
   return VK_SUCCESS;
}

VkResult
nvk_switch_fence_import_nvmultifence(VkDevice _device,
                                     VkFence _fence,
                                     const NvMultiFence *fence_in)
{
   VK_FROM_HANDLE(vk_device, device, _device);
   VK_FROM_HANDLE(vk_fence, fence, _fence);

   if (device == NULL || fence == NULL)
      return VK_ERROR_FEATURE_NOT_PRESENT;

   vk_fence_reset_temporary(device, fence);

   struct vk_sync *temporary = NULL;
   VkResult result =
      nvk_switch_create_sync_from_nvmultifence(device, fence_in, &temporary);
   if (result != VK_SUCCESS)
      return result;

   fence->temporary = temporary;
   return VK_SUCCESS;
}
#endif
