/* SPDX-License-Identifier: MIT
 * Shader objects bypass Vulkan pipeline caches. Persist their opaque binaries
 * through Mesa's existing Win32 blob cache; never share a live VkShader handle.
 */
#include "tgd3d.h"
#include "util/disk_cache.h"
#include "util/hex.h"
#include "util/mesa-blake3.h"

#include <malloc.h>
#include <stdio.h>

namespace tgd3d {

disk_cache *create_shader_cache(const VkPhysicalDeviceShaderObjectPropertiesEXT &properties)
{
   char uuid[VK_UUID_SIZE * 2 + 1], driver[96];
   mesa_bytes_to_hex(uuid, properties.shaderBinaryUUID, VK_UUID_SIZE);
   /* This version describes tgd3d's descriptor/push-constant layout, not
    * Vulkan's opaque format. Bump it when that layout's semantics change. */
   snprintf(driver, sizeof(driver), "tgd3d-layout-1-%s-%u", uuid, properties.shaderBinaryVersion);
   return disk_cache_create_custom("tgd3d", driver, 0, "tgd3d-shader-objects", 128u * 1024 * 1024);
}

VkResult create_shader_cached(Device *dev, const VkShaderCreateInfoEXT &info, VkShaderEXT *shader,
                              const Shader *compute)
{
   disk_cache *cache = dev->adapter->vk.shaderCache;
   if (!cache)
      return dev->vk.CreateShadersEXT(dev->vkdev, 1, &info, nullptr, shader);

   /* Hash SPIR-V after reflection/conversion so all float controls, IO
    * classes, dual-source outputs and resource mappings enter the key.
    * Layout handles are device-local and must never be hashed or persisted. */
   assert(info.codeType == VK_SHADER_CODE_TYPE_SPIRV_EXT && !info.pNext &&
          !info.pSpecializationInfo && !info.pushConstantRangeCount &&
          info.setLayoutCount == (compute ? 1u : dev->caps->graphicsSetCount()) && !strcmp(info.pName, "main"));
   const uint32_t state[] = {info.flags, info.stage, info.nextStage,
      info.setLayoutCount, dev->caps->cbSlots, D3D_SRV_SLOTS, D3D_SAMPLER_SLOTS, SET1_STRIDE};
   blake3_hasher hash;
   _mesa_blake3_init(&hash);
   _mesa_blake3_update(&hash, state, sizeof(state));
   if (!compute && dev->caps->tessellationShader) {
      const uint32_t stages = VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT | VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
      _mesa_blake3_update(&hash, &stages, sizeof(stages));
   }
   if (compute) {
      for (const ShaderBinding &b : compute->bindings) {
         const uint32_t layout[] = {b.binding, static_cast<uint32_t>(b.type)};
         _mesa_blake3_update(&hash, layout, sizeof(layout));
      }
   } else if (dev->caps->graphicsUavSlots) {
      // Version two shares buffer and counter descriptors among graphics stages.
      const uint32_t layout[] = {2, dev->caps->graphicsUavSlots, dev->caps->graphicsUavStages};
      _mesa_blake3_update(&hash, layout, sizeof(layout));
   }
   _mesa_blake3_update(&hash, info.pCode, info.codeSize);
   blake3_hash digest;
   _mesa_blake3_final(&hash, digest);
   cache_key key;
   disk_cache_compute_key(cache, digest, sizeof(digest), key);

   constexpr size_t maxBinary = 16u * 1024 * 1024;
   size_t size = 0;
   void *stored = disk_cache_get(cache, key, &size);
   if (stored && size && size <= maxBinary) {
      /* Vulkan requires 16-byte alignment for both binary input and output. */
      void *aligned = _aligned_malloc(size, 16);
      if (aligned) {
         memcpy(aligned, stored, size);
         VkShaderCreateInfoEXT binary = info;
         binary.codeType = VK_SHADER_CODE_TYPE_BINARY_EXT;
         binary.codeSize = size;
         binary.pCode = aligned;
         VkResult result = dev->vk.CreateShadersEXT(dev->vkdev, 1, &binary, nullptr, shader);
         _aligned_free(aligned);
         free(stored);
         if (result == VK_SUCCESS) {
            trace(TGD3D_TRACE_SHADER_CACHE, S_OK, 1, size);
            return result;
         }
         /* Only an incompatible binary is a cache miss. OOM/device loss
          * must reach the existing DDI error path without another compile. */
         if (result != VK_INCOMPATIBLE_SHADER_BINARY_EXT)
            return result;
         disk_cache_remove(cache, key);
         stored = nullptr;
      }
   }
   free(stored);
   trace(TGD3D_TRACE_SHADER_CACHE, S_OK, 0, 0);

   VkResult result = dev->vk.CreateShadersEXT(dev->vkdev, 1, &info, nullptr, shader);
   if (result != VK_SUCCESS)
      return result;
   size = 0;
   VkResult exported = dev->vk.GetShaderBinaryDataEXT(dev->vkdev, *shader, &size, nullptr);
   if (exported == VK_SUCCESS && size && size <= maxBinary) {
      const size_t capacity = size;
      void *binary = _aligned_malloc(capacity, 16);
      if (binary) {
         exported = dev->vk.GetShaderBinaryDataEXT(dev->vkdev, *shader, &size, binary);
         if (exported == VK_SUCCESS && size && size <= capacity) {
            /* The cache copies the bytes before queueing its low-priority
             * writer. Neither a VkDevice nor a runtime callback is retained. */
            disk_cache_put(cache, key, binary, size, nullptr);
            trace(TGD3D_TRACE_SHADER_CACHE, S_OK, 2, size);
         }
         _aligned_free(binary);
      }
   }
   if (exported == VK_ERROR_DEVICE_LOST) {
      /* The new object has never been submitted. Cache export is optional,
       * but discovering device loss must not turn into successful creation. */
      dev->vk.DestroyShaderEXT(dev->vkdev, *shader, nullptr);
      *shader = VK_NULL_HANDLE;
      return exported;
   }
   return result;
}

}
