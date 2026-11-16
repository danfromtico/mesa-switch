/* SPDX-License-Identifier: MIT */
#ifndef VK_WSI_WIN32_MEMORY_H
#define VK_WSI_WIN32_MEMORY_H

#include <vulkan/vulkan_core.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* Mesa-internal import of a runtime-owned WDDM allocation. These are not
 * Win32 external-memory handles and must never be passed to ShareObjects or
 * used with another device's paging queue. The owner retains the allocation
 * and executes each operation on the device that created/opened it.
 *
 * GPUVA reservations are process-wide. map_gpu maps the owner's allocation
 * into the supplied reservation and waits for its paging fence before
 * returning success. The owner records the whole reservation, releases it
 * before deallocating the runtime resource, and makes a later free_gpu a no-op
 * for a range already released. WDDM automatically releases allocation GPUVAs;
 * cached importer objects must not free a subsequently reused address.
 *
 * retain/release protect the owner context. The D3D frontend must finish GPU
 * use before destroying its runtime resource; importing does not change the
 * runtime's CreateResource/DestroyResource lifetime contract.
 */
#define WSI_WIN32_MEMORY_VERSION 1u
#define WSI_WIN32_MEMORY_DISPLAYABLE_VERSION 2u
#define WSI_WIN32_MEMORY_RUNTIME_VERSION 3u
struct wsi_win32_memory {
   uint32_t version;
   uint32_t allocation;
   uint64_t adapter_luid;
   uint64_t size;
   bool read_only;
   void *context;
   void (*retain)(void *context);
   void (*release)(void *context);
   VkResult (*map_gpu)(void *context, uint64_t address, uint64_t offset,
                       uint64_t size, uint8_t kind, bool read_only,
                       uint64_t reservation_base, uint64_t reservation_size);
   VkResult (*free_gpu)(void *context, uint64_t reservation_base, uint64_t reservation_size);
   VkResult (*map_cpu)(void *context, void **address);
   void (*unmap_cpu)(void *context);

   /* Version 2 only: actual kernel resource returned by Allocate/OpenResource,
    * never hRTResource. Version 1 ends before this member. Version 2 requires
    * an importing-device write reference and positive acknowledgement. */
   uint32_t kernel_resource;
   uint32_t reserved;
   /* Version 3: writes use a context on this runtime device, without
    * exporting/reopening the allocation. kernel_resource must be zero. */
   void *runtime_device;
};

static inline bool
wsi_win32_memory_copy(struct wsi_win32_memory *dst, const struct wsi_win32_memory *src)
{
   memset(dst, 0, sizeof(*dst));
   if (!src || (src->version != WSI_WIN32_MEMORY_VERSION &&
                src->version != WSI_WIN32_MEMORY_DISPLAYABLE_VERSION &&
                src->version != WSI_WIN32_MEMORY_RUNTIME_VERSION))
      return false;
   /* Old UMDs provide only the v1 prefix. Do not read the new tail. */
   memcpy(dst, src, src->version == WSI_WIN32_MEMORY_VERSION ?
          offsetof(struct wsi_win32_memory, kernel_resource) :
          src->version == WSI_WIN32_MEMORY_DISPLAYABLE_VERSION ?
          offsetof(struct wsi_win32_memory, runtime_device) : sizeof(*dst));
   if (src->version == WSI_WIN32_MEMORY_RUNTIME_VERSION)
      return dst->runtime_device && !dst->kernel_resource && !dst->reserved;
   return src->version == WSI_WIN32_MEMORY_VERSION ||
          (dst->kernel_resource && !dst->reserved);
}

/* A runtime allocation has one stored format. The only other way to view it
 * is the sRGB encoding of the same bytes, which Direct3D applications use
 * routinely for render-target and shader views of UNORM swap-chain buffers.
 */
static inline VkFormat
wsi_win32_srgb_twin(VkFormat format)
{
   switch (format) {
   case VK_FORMAT_B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_SRGB;
   case VK_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_SRGB;
   default: return VK_FORMAT_UNDEFINED;
   }
}

/* Create flags a driver accepts for an image over a runtime allocation:
 * none, or MUTABLE_FORMAT with a view-format list naming only the stored
 * format and its sRGB twin. The importer asks for the latter so that such a
 * view never forces it to replace the image, which would silently detach
 * the resource from the allocation other owners read.
 */
static inline bool
wsi_win32_image_flags_supported(VkImageCreateFlags flags, VkFormat format,
                                const VkImageFormatListCreateInfo *views)
{
   if (!flags)
      return true;
   const VkFormat twin = wsi_win32_srgb_twin(format);
   if (flags != VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT || twin == VK_FORMAT_UNDEFINED ||
       !views || !views->viewFormatCount || views->viewFormatCount > 2 ||
       !views->pViewFormats)
      return false;
   for (uint32_t i = 0; i < views->viewFormatCount; i++) {
      if (views->pViewFormats[i] != format && views->pViewFormats[i] != twin)
         return false;
   }
   return true;
}

#endif
