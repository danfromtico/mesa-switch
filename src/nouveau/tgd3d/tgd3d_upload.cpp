/* SPDX-License-Identifier: MIT
 *
 * Host-visible buffer storage. D3D buffers and CPU-side texel uploads are
 * slices of a few large Vulkan buffers. A discard map or a full update takes
 * a fresh slice instead of waiting for the GPU or copying on it: draws that
 * were already recorded keep reading the old slice, which returns to its
 * chunk once the queue fence passes its last use.
 */
#include "tgd3d.h"

namespace tgd3d {

enum : VkDeviceSize {
   CHUNK_SIZE = 1024 * 1024,
   DEDICATED_THRESHOLD = CHUNK_SIZE / 4,
   MAX_IDLE_CHUNKS = 4,
};

static HRESULT chunk_create(Device *dev, VkDeviceSize size, bool dedicated, Chunk **out,
                             D3D10DDI_HRTRESOURCE resource = {})
{
   *out = nullptr;
   Chunk *chunk = new (std::nothrow) Chunk();
   if (!chunk)
      return E_OUTOFMEMORY;
   VkBufferCreateInfo info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
   info.size = size;
   info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT |
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT |
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
   if (dev->caps && dev->caps->transformFeedback)
      info.usage |= VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT | VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_COUNTER_BUFFER_BIT_EXT;
   info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
   HRESULT hr = resource_result(dev->vk.CreateBuffer(dev->vkdev, &info, nullptr, &chunk->buffer));
   if (FAILED(hr)) {
      delete chunk;
      return hr;
   }
   VkMemoryRequirements req;
   dev->vk.GetBufferMemoryRequirements(dev->vkdev, chunk->buffer, &req);
   VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
   alloc.allocationSize = req.size;
   alloc.memoryTypeIndex = dev->adapter->vk.findMemoryType(
      req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
   void *cpu = nullptr;
   if (resource.handle) {
      hr = runtime_private_create(dev, resource, req.size, &chunk->runtime);
      if (SUCCEEDED(hr)) hr = runtime_bind_buffer(dev, chunk->runtime, chunk->buffer, &chunk->memory);
   } else {
      hr = alloc.memoryTypeIndex == UINT32_MAX ? DXGI_ERROR_UNSUPPORTED :
         resource_result(dev->vk.AllocateMemory(dev->vkdev, &alloc, nullptr, &chunk->memory));
      if (SUCCEEDED(hr))
         hr = resource_result(dev->vk.BindBufferMemory(dev->vkdev, chunk->buffer, chunk->memory, 0));
   }
   if (SUCCEEDED(hr))
      hr = resource_result(dev->vk.MapMemory(dev->vkdev, chunk->memory, 0, VK_WHOLE_SIZE, 0, &cpu));
   if (FAILED(hr)) {
      dev->vk.DestroyBuffer(dev->vkdev, chunk->buffer, nullptr);
      if (chunk->memory)
         dev->vk.FreeMemory(dev->vkdev, chunk->memory, nullptr);
      if (chunk->runtime) runtime_destroy(chunk->runtime);
      delete chunk;
      return hr;
   }
   chunk->cpu = static_cast<uint8_t *>(cpu);
   chunk->size = size;
   chunk->dedicated = dedicated;
   dev->chunks.push_back(chunk);
   *out = chunk;
   return S_OK;
}

static void chunk_destroy(Device *dev, Chunk *chunk)
{
   for (size_t i = 0; i < dev->chunks.size(); i++) {
      if (dev->chunks[i] == chunk) {
         dev->chunks.erase(dev->chunks.begin() + i);
         break;
      }
   }
   if (dev->currentChunk == chunk)
      dev->currentChunk = nullptr;
   dev->vk.DestroyBuffer(dev->vkdev, chunk->buffer, nullptr);
   dev->vk.FreeMemory(dev->vkdev, chunk->memory, nullptr);
   if (chunk->runtime) {
      HRESULT hr = runtime_destroy(chunk->runtime);
      if (FAILED(hr)) TG_ERROR(dev, hr);
   }
   delete chunk;
}

HRESULT resource_slice_alloc(Resource *res, Slice *out)
{
   Device *dev = res->dev;
   if (!runtime_private_allocations(dev)) return slice_alloc(dev, res->size, 256, out);
   *out = {};
   if (FAILED(dev->lost)) return dev->lost;
   if (!res->hRT.handle || !res->size || res->size > TG_GPU_MEMORY_BUDGET_BYTES)
      return E_INVALIDARG;
   /* An offered application buffer must not discard unrelated slices of an
    * upload pool. Driver-internal uploads retain the existing pooled path. */
   Chunk *chunk = nullptr;
   HRESULT hr = chunk_create(dev, (res->size + 4095) & ~VkDeviceSize(4095), true, &chunk, res->hRT);
   if (FAILED(hr)) return hr;
   chunk->live = 1;
   chunk->used = res->size;
   *out = {chunk, 0, res->size};
   return S_OK;
}

HRESULT slice_map(Device *dev, Slice slice)
{
   if (!slice) return E_INVALIDARG;
   if (FAILED(dev->lost)) return dev->lost;
   if (runtime_is_offered(slice.chunk->runtime)) return DXGI_ERROR_INVALID_CALL;
   if (slice.chunk->cpu) return S_OK;
   void *cpu = nullptr;
   HRESULT hr = resource_result(dev->vk.MapMemory(dev->vkdev, slice.chunk->memory,
                                                  0, VK_WHOLE_SIZE, 0, &cpu));
   if (SUCCEEDED(hr) && !cpu) {
      dev->vk.UnmapMemory(dev->vkdev, slice.chunk->memory);
      return E_FAIL;
   }
   if (SUCCEEDED(hr)) slice.chunk->cpu = static_cast<uint8_t *>(cpu);
   return hr;
}

void slice_unmap(Device *dev, Slice slice)
{
   if (slice && slice.chunk->cpu && !slice.chunk->videoWrites) {
      dev->vk.UnmapMemory(dev->vkdev, slice.chunk->memory);
      slice.chunk->cpu = nullptr;
   }
}

HRESULT slice_alloc(Device *dev, VkDeviceSize size, VkDeviceSize alignment, Slice *out)
{
   *out = {};
   if (FAILED(dev->lost))
      return dev->lost;
   if (!size)
      size = 1;
   if (alignment < 16)
      alignment = 16;
   if (size >= DEDICATED_THRESHOLD) {
      Chunk *chunk = nullptr;
      HRESULT hr = chunk_create(dev, (size + 4095) & ~VkDeviceSize(4095), true, &chunk);
      if (FAILED(hr))
         return hr;
      chunk->live = 1;
      chunk->used = size;
      out->chunk = chunk;
      out->size = size;
      return S_OK;
   }
   for (int attempt = 0; attempt < 2; attempt++) {
      Chunk *chunk = dev->currentChunk;
      if (chunk) {
         const VkDeviceSize offset = (chunk->used + alignment - 1) & ~(alignment - 1);
         if (offset + size <= chunk->size) {
            chunk->used = offset + size;
            chunk->live++;
            out->chunk = chunk;
            out->offset = offset;
            out->size = size;
            return S_OK;
         }
      }
      /* Prefer a chunk every slice of which has been returned. */
      dev->currentChunk = nullptr;
      for (Chunk *idle : dev->chunks) {
         if (!idle->dedicated && !idle->live && idle != chunk) {
            idle->used = 0;
            dev->currentChunk = idle;
            break;
         }
      }
      if (!dev->currentChunk) {
         HRESULT hr = chunk_create(dev, CHUNK_SIZE, false, &dev->currentChunk);
         if (FAILED(hr))
            return hr;
      }
   }
   return E_OUTOFMEMORY;
}

void slice_release(Device *dev, Slice slice, uint64_t lastUse)
{
   if (!slice)
      return;
   if (lastUse > dev->submitted && SUCCEEDED(dev->lost) &&
       FAILED(runtime_make_resident(dev, slice.chunk->runtime)))
      TG_ERROR(dev, DXGI_ERROR_DEVICE_REMOVED);
   if (lastUse && !sequence_done(dev, lastUse)) {
      dev->deferredSlices.push_back({slice, lastUse});
      return;
   }
   Chunk *chunk = slice.chunk;
   if (--chunk->live)
      return;
   if (chunk->dedicated) {
      chunk_destroy(dev, chunk);
      return;
   }
   if (chunk == dev->currentChunk) {
      chunk->used = 0;
      return;
   }
   size_t idle = 0;
   for (Chunk *other : dev->chunks)
      idle += !other->dedicated && !other->live;
   if (idle > MAX_IDLE_CHUNKS)
      chunk_destroy(dev, chunk);
}

void upload_destroy(Device *dev)
{
   dev->deferredSlices.clear();
   while (!dev->chunks.empty())
      chunk_destroy(dev, dev->chunks.back());
}

void upload_trim(Device *dev)
{
   /* live includes slices still in the retirement queue. Keep the current
    * upload reserve so Trim does not discard state needed to keep operating. */
   for (size_t i = 0; i < dev->chunks.size();) {
      Chunk *chunk = dev->chunks[i];
      if (!chunk->live && chunk != dev->currentChunk) chunk_destroy(dev, chunk);
      else ++i;
   }
}

}
