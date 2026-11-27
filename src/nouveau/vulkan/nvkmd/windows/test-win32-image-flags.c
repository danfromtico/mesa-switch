/* SPDX-License-Identifier: MIT */
/* Host model of the create flags allowed on images over runtime allocations.
 * Zink builds the request and NVK validates it with this same rule. */
#include "vk_wsi_win32_memory.h"
#include <stdio.h>

#define CHECK(test) do { if (!(test)) { printf("FAIL line %d\n", __LINE__); return __LINE__; } } while (0)

static bool
allowed(VkImageCreateFlags flags, VkFormat format, uint32_t count, const VkFormat *formats)
{
   const VkImageFormatListCreateInfo list = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
      .viewFormatCount = count, .pViewFormats = formats,
   };
   return wsi_win32_image_flags_supported(flags, format, formats ? &list : NULL);
}

int main(void)
{
   const VkImageCreateFlags mutable = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
   const VkFormat bgra = VK_FORMAT_B8G8R8A8_UNORM, rgba = VK_FORMAT_R8G8B8A8_UNORM;
   CHECK(wsi_win32_srgb_twin(bgra) == VK_FORMAT_B8G8R8A8_SRGB);
   CHECK(wsi_win32_srgb_twin(rgba) == VK_FORMAT_R8G8B8A8_SRGB);
   CHECK(wsi_win32_srgb_twin(VK_FORMAT_B8G8R8A8_SRGB) == VK_FORMAT_UNDEFINED);
   CHECK(wsi_win32_srgb_twin(VK_FORMAT_A2B10G10R10_UNORM_PACK32) == VK_FORMAT_UNDEFINED);

   /* The request Zink makes, and the earlier flagless one. */
   const VkFormat pair[2] = {bgra, VK_FORMAT_B8G8R8A8_SRGB};
   CHECK(allowed(mutable, bgra, 2, pair));
   CHECK(allowed(0, bgra, 0, NULL) && allowed(0, bgra, 2, pair));
   const VkFormat reversed[2] = {VK_FORMAT_R8G8B8A8_SRGB, rgba};
   CHECK(allowed(mutable, rgba, 2, reversed) && allowed(mutable, rgba, 1, reversed));

   /* Mutable without a list would allow any compatible format. */
   CHECK(!allowed(mutable, bgra, 0, NULL) && !allowed(mutable, bgra, 0, pair));
   /* Another channel order or class is a different image, not a view. */
   const VkFormat swapped[2] = {bgra, VK_FORMAT_R8G8B8A8_SRGB};
   CHECK(!allowed(mutable, bgra, 2, swapped));
   const VkFormat integer[2] = {bgra, VK_FORMAT_B8G8R8A8_UINT};
   CHECK(!allowed(mutable, bgra, 2, integer));
   const VkFormat three[3] = {bgra, VK_FORMAT_B8G8R8A8_SRGB, bgra};
   CHECK(!allowed(mutable, bgra, 3, three));
   /* A format without a twin has nothing to declare. */
   const VkFormat packed[1] = {VK_FORMAT_A2B10G10R10_UNORM_PACK32};
   CHECK(!allowed(mutable, VK_FORMAT_A2B10G10R10_UNORM_PACK32, 1, packed));
   /* Every other create flag stays rejected, alone or combined. */
   CHECK(!allowed(VK_IMAGE_CREATE_EXTENDED_USAGE_BIT, bgra, 2, pair));
   CHECK(!allowed(mutable | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT, bgra, 2, pair));
   CHECK(!allowed(VK_IMAGE_CREATE_SPARSE_BINDING_BIT, bgra, 2, pair));
   CHECK(!allowed(mutable | VK_IMAGE_CREATE_ALIAS_BIT, bgra, 2, pair));
   const VkImageFormatListCreateInfo null_formats = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO, .viewFormatCount = 2,
   };
   CHECK(!wsi_win32_image_flags_supported(mutable, bgra, &null_formats));
   puts("PASS: runtime-allocation image create flags (host model)");
   return 0;
}
