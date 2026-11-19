/* SPDX-License-Identifier: MIT
 * Resource creation failures through the real DDI, without GPU submission.
 */
#include "tgd3d.h"
#include <cstdio>
#include <cstdlib>
using namespace tgd3d;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%u: %s\n", __FILE__, __LINE__, #x); std::exit(1); } } while (0)

static unsigned failureStep, calls, liveImages, liveBuffers, liveMemory, notifications;
static VkDeviceSize bufferSize;
static VkResult failureResult;
static HRESULT reported;

static void init(Device *dev, Adapter *adapter)
{
   calls = liveImages = liveBuffers = liveMemory = notifications = 0;
   reported = S_OK;
   dev->adapter = adapter; dev->caps = &adapter->vk.caps;
   dev->vkdev = (VkDevice)(uintptr_t)1;
   adapter->vk.caps.memory.memoryTypeCount = 1;
   adapter->vk.caps.memory.memoryTypes[0].propertyFlags =
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
   dev->um.pfnSetErrorCb = [](D3D10DDI_HRTCORELAYER, HRESULT hr) APIENTRY {
      CHECK(!liveImages && !liveBuffers && !liveMemory);
      reported = hr; ++notifications;
   };
   dev->vk.CreateImage = [](VkDevice, const VkImageCreateInfo *, const VkAllocationCallbacks *, VkImage *out) VKAPI_CALL {
      ++calls; if (failureStep == 1) return failureResult;
      *out = (VkImage)(uintptr_t)2; ++liveImages; return VK_SUCCESS;
   };
   dev->vk.CreateBuffer = [](VkDevice, const VkBufferCreateInfo *info, const VkAllocationCallbacks *, VkBuffer *out) VKAPI_CALL {
      ++calls; if (failureStep == 1) return failureResult;
      bufferSize = info->size; *out = (VkBuffer)(uintptr_t)3; ++liveBuffers; return VK_SUCCESS;
   };
   dev->vk.GetImageMemoryRequirements = [](VkDevice, VkImage, VkMemoryRequirements *out) VKAPI_CALL { *out = {16384, 4096, 1}; };
   dev->vk.GetBufferMemoryRequirements = [](VkDevice, VkBuffer, VkMemoryRequirements *out) VKAPI_CALL { *out = {bufferSize, 4096, 1}; };
   dev->vk.AllocateMemory = [](VkDevice, const VkMemoryAllocateInfo *, const VkAllocationCallbacks *, VkDeviceMemory *out) VKAPI_CALL {
      ++calls; if (failureStep == 2) return failureResult;
      *out = (VkDeviceMemory)(uintptr_t)4; ++liveMemory; return VK_SUCCESS;
   };
   dev->vk.BindImageMemory = [](VkDevice, VkImage, VkDeviceMemory, VkDeviceSize) VKAPI_CALL {
      ++calls; return failureStep == 3 ? failureResult : VK_SUCCESS;
   };
   dev->vk.BindBufferMemory = [](VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize) VKAPI_CALL {
      ++calls; return failureStep == 3 ? failureResult : VK_SUCCESS;
   };
   dev->vk.MapMemory = [](VkDevice, VkDeviceMemory, VkDeviceSize, VkDeviceSize, VkMemoryMapFlags, void **) VKAPI_CALL {
      ++calls; CHECK(failureStep == 4); return failureResult;
   };
   dev->vk.DestroyImage = [](VkDevice, VkImage, const VkAllocationCallbacks *) VKAPI_CALL { CHECK(liveImages == 1); --liveImages; };
   dev->vk.DestroyBuffer = [](VkDevice, VkBuffer, const VkAllocationCallbacks *) VKAPI_CALL { CHECK(liveBuffers == 1); --liveBuffers; };
   dev->vk.FreeMemory = [](VkDevice, VkDeviceMemory, const VkAllocationCallbacks *) VKAPI_CALL { CHECK(liveMemory == 1); --liveMemory; };
}

static void dynamic_unmap_failure_contract()
{
   for (bool dedicated : {false, true}) {
      Adapter adapter; Device dev; init(&dev, &adapter);
      D3D10DDI_DEVICEFUNCS table = {}; resource_fill_table(&table);
      uint8_t mappedByte = 0;
      Chunk *chunk = new Chunk;
      chunk->buffer = (VkBuffer)(uintptr_t)3;
      chunk->memory = (VkDeviceMemory)(uintptr_t)4;
      chunk->cpu = &mappedByte;
      chunk->size = chunk->used = 512;
      chunk->dedicated = dedicated;
      chunk->live = dedicated ? 1 : 2; /* pooled case has an unrelated live slice */
      liveBuffers = liveMemory = 1;
      dev.chunks.push_back(chunk);
      if (!dedicated) dev.currentChunk = chunk;
      dev.vk.UnmapMemory = [](VkDevice, VkDeviceMemory memory) VKAPI_CALL {
         CHECK(memory == (VkDeviceMemory)(uintptr_t)4 && liveMemory == 1);
      };
      Resource res;
      res.dev = &dev; res.kind = ResKind::Image;
      res.format = DXGI_FORMAT_R8G8B8A8_UNORM;
      res.width = res.height = 8; res.depth = res.mips = 1;
      res.maps.resize(1);
      res.maps[0].mapped = res.maps[0].write = true;
      res.maps[0].upload = {chunk, 0, 256};
      /* A successful Map preceded the device loss; Unmap must still release
       * its CPU-only upload. No command or completion is fabricated. */
      dev.lost = DXGI_ERROR_DEVICE_REMOVED;
      table.pfnDynamicResourceUnmap({&dev}, {&res}, 0);
      CHECK(!res.maps[0].mapped && !res.maps[0].upload && dev.deferredSlices.empty());
      CHECK(dev.cmd == VK_NULL_HANDLE && !dev.cmdHasWork && !notifications);
      table.pfnDynamicResourceUnmap({&dev}, {&res}, 0); /* no double release */
      if (dedicated) {
         CHECK(dev.chunks.empty() && !liveBuffers && !liveMemory);
      } else {
         CHECK(chunk->live == 1 && chunk->used == 512 && liveBuffers == 1 && liveMemory == 1);
         slice_release(&dev, {chunk, 256, 256}, 0);
         CHECK(!chunk->live && !chunk->used);
      }
      upload_destroy(&dev);
      CHECK(dev.chunks.empty() && !liveBuffers && !liveMemory);
   }
   std::puts("PASS: failed dynamic-texture Unmap releases its upload once, preserves other live slices and submits no GPU work (CPU control)");
}

void resource_creation_contracts()
{
   /* Private image, pooled buffer, dedicated buffer and staging texture. */
   for (unsigned kind = 0; kind < 4; ++kind) {
      const bool buffer = kind == 1 || kind == 2;
      for (unsigned step = 1; step <= (kind ? 4u : 3u); ++step) {
         for (VkResult error : {VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                               VK_ERROR_MEMORY_MAP_FAILED, VK_ERROR_FORMAT_NOT_SUPPORTED, VK_ERROR_UNKNOWN}) {
            if (error == VK_ERROR_MEMORY_MAP_FAILED && step != 4) continue;
            Adapter adapter; Device dev; init(&dev, &adapter);
            failureStep = step; failureResult = error;
            D3D10DDI_MIPINFO mip = {};
            mip.TexelWidth = kind == 2 ? 300000 : 64; mip.TexelHeight = buffer ? 1 : 64; mip.TexelDepth = 1;
            D3D10DDIARG_CREATERESOURCE desc = {};
            desc.pMipInfoList = &mip; desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
            desc.ResourceDimension = buffer ? D3D10DDIRESOURCE_BUFFER : D3D10DDIRESOURCE_TEXTURE2D;
            desc.Format = buffer ? DXGI_FORMAT_UNKNOWN : DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.Usage = kind == 3 ? D3D10_DDI_USAGE_STAGING : D3D10_DDI_USAGE_DEFAULT;
            desc.BindFlags = buffer ? D3D10_DDI_BIND_VERTEX_BUFFER : kind == 3 ? 0 : D3D10_DDI_BIND_RENDER_TARGET;
            alignas(Resource) unsigned char storage[sizeof(Resource)];
            D3D10DDI_DEVICEFUNCS table = {}; resource_fill_table(&table);
            table.pfnCreateResource({&dev}, &desc, {storage}, {});
            const bool recoverable = error == VK_ERROR_OUT_OF_HOST_MEMORY ||
               error == VK_ERROR_OUT_OF_DEVICE_MEMORY || error == VK_ERROR_MEMORY_MAP_FAILED;
            CHECK(reported == (recoverable ? E_OUTOFMEMORY : D3DDDIERR_DEVICEREMOVED) && notifications == 1);
            CHECK(SUCCEEDED(dev.lost) == recoverable);
            CHECK(!dev.resources);
            CHECK(!liveImages && !liveBuffers && !liveMemory && dev.chunks.empty() && !dev.currentChunk);
            /* A removed device rejects another create before allocation. */
            const unsigned previous = calls;
            dev.lost = D3DDDIERR_DEVICEREMOVED;
            table.pfnCreateResource({&dev}, &desc, {storage}, {});
            CHECK(reported == D3DDDIERR_DEVICEREMOVED && notifications == 2 && calls == previous);
         }
      }
   }
   /* Lazy view creation also preserves internal failures instead of
    * labeling every result as memory exhaustion. */
   for (VkResult error : {VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY, VK_ERROR_UNKNOWN}) {
      Adapter adapter; Device dev; init(&dev, &adapter); failureResult = error;
      dev.vk.CreateImageView = [](VkDevice, const VkImageViewCreateInfo *, const VkAllocationCallbacks *, VkImageView *) VKAPI_CALL { return failureResult; };
      dev.vk.CreateBufferView = [](VkDevice, const VkBufferViewCreateInfo *, const VkAllocationCallbacks *, VkBufferView *) VKAPI_CALL { return failureResult; };
      Resource res; res.dev = &dev; res.storage.image = (VkImage)(uintptr_t)2;
      View view; view.dev = &dev; view.res = &res; view.type = ViewType::SRV;
      view.info = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      view.info.viewType = VK_IMAGE_VIEW_TYPE_2D; view.info.format = VK_FORMAT_R8G8B8A8_UNORM;
      view.info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      CHECK(view_get(&view) == VK_NULL_HANDLE);
      CHECK(reported == (error == VK_ERROR_UNKNOWN ? E_FAIL : E_OUTOFMEMORY));
      Chunk chunk; chunk.buffer = (VkBuffer)(uintptr_t)3; res.slice.chunk = &chunk;
      view.bufferFormat = VK_FORMAT_R32_UINT; view.bufferRange = 64;
      CHECK(view_get_buffer(&view) == VK_NULL_HANDLE && notifications == 2);
      CHECK(reported == (error == VK_ERROR_UNKNOWN ? E_FAIL : E_OUTOFMEMORY));
   }
   dynamic_unmap_failure_contract();
   std::puts("PASS: image/buffer/staging allocation and map failures report OOM after cleanup; removed devices allocate nothing; view errors retain their class (CPU control)");
}
