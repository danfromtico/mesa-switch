/* SPDX-License-Identifier: MIT
 * Execute recorded copy commands on CPU storage. These controls catch illegal
 * Vulkan overlap, missing scratch dependencies and premature retirement;
 * they do not run a GPU or prove WDDM/physical display behavior.
 */
#include "tgd3d.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace tgd3d;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%u: %s\n", __FILE__, __LINE__, #x); std::exit(1); } } while (0)

namespace {
struct Memory { std::vector<uint8_t> bytes; };
struct Buffer { size_t size; Memory *memory = nullptr; };
struct Image { UINT width, height, depth; std::vector<uint8_t> bytes; };
struct Copy {
   Image *srcImage = nullptr, *dstImage = nullptr;
   Buffer *srcBuffer = nullptr, *dstBuffer = nullptr;
   VkImageCopy image = {};
   VkBufferCopy buffer = {};
   unsigned barrier;
};
static std::vector<Copy> copies;
static unsigned images, buffers, memories, barriers, waits, errors;
static uint64_t completed;
static HRESULT error;
static unsigned failStage;
template<typename T, typename H> T *object(H handle) { return (T *)(uintptr_t)handle; }
template<typename H, typename T> H handle(T *object) { return (H)(uintptr_t)object; }

static void init(Device &dev, Adapter &adapter)
{
   images = buffers = memories = barriers = waits = errors = 0;
   completed = 6; error = S_OK; failStage = 0; copies.clear();
   dev.adapter = &adapter; dev.caps = &adapter.vk.caps;
   dev.vkdev = (VkDevice)(uintptr_t)1; dev.cmd = (VkCommandBuffer)(uintptr_t)2;
   dev.fence = (VkSemaphore)(uintptr_t)3; dev.submitted = completed;
   adapter.vk.caps.memory.memoryTypeCount = 1;
   adapter.vk.caps.memory.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
   dev.um.pfnSetErrorCb = [](D3D10DDI_HRTCORELAYER, HRESULT hr) { error = hr; ++errors; };
   dev.vk.GetSemaphoreCounterValue = [](VkDevice, VkSemaphore, uint64_t *out) { *out = completed; return VK_SUCCESS; };
   dev.vk.WaitSemaphores = [](VkDevice, const VkSemaphoreWaitInfo *, uint64_t) { ++waits; return VK_TIMEOUT; };
   dev.vk.CreateImage = [](VkDevice, const VkImageCreateInfo *in, const VkAllocationCallbacks *, VkImage *out) {
      if (failStage == 1) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
      CHECK(in->mipLevels == 1 && in->arrayLayers == 1 && in->samples == VK_SAMPLE_COUNT_1_BIT);
      auto image = new Image{in->extent.width, in->extent.height, in->extent.depth, {}};
      image->bytes.resize((size_t)image->width * image->height * image->depth * 4);
      *out = handle<VkImage>(image); ++images; return VK_SUCCESS;
   };
   dev.vk.GetImageMemoryRequirements = [](VkDevice, VkImage h, VkMemoryRequirements *out) {
      *out = {object<Image>(h)->bytes.size(), 256, 1};
   };
   dev.vk.CreateBuffer = [](VkDevice, const VkBufferCreateInfo *in, const VkAllocationCallbacks *, VkBuffer *out) {
      if (failStage == 5) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
      CHECK(in->usage & VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
      *out = handle<VkBuffer>(new Buffer{(size_t)in->size}); ++buffers; return VK_SUCCESS;
   };
   dev.vk.GetBufferMemoryRequirements = [](VkDevice, VkBuffer h, VkMemoryRequirements *out) {
      *out = {object<Buffer>(h)->size, 256, 1};
   };
   dev.vk.AllocateMemory = [](VkDevice, const VkMemoryAllocateInfo *in, const VkAllocationCallbacks *, VkDeviceMemory *out) {
      if (failStage == 2) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
      auto memory = new Memory; memory->bytes.resize(in->allocationSize);
      *out = handle<VkDeviceMemory>(memory); ++memories; return VK_SUCCESS;
   };
   dev.vk.BindImageMemory = [](VkDevice, VkImage, VkDeviceMemory, VkDeviceSize) {
      return failStage == 3 ? VK_ERROR_OUT_OF_DEVICE_MEMORY : VK_SUCCESS;
   };
   dev.vk.BindBufferMemory = [](VkDevice, VkBuffer b, VkDeviceMemory m, VkDeviceSize offset) {
      if (failStage == 6) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
      CHECK(!offset); object<Buffer>(b)->memory = object<Memory>(m); return VK_SUCCESS;
   };
   dev.vk.MapMemory = [](VkDevice, VkDeviceMemory h, VkDeviceSize, VkDeviceSize, VkMemoryMapFlags, void **out) {
      if (failStage == 7) return VK_ERROR_MEMORY_MAP_FAILED;
      *out = object<Memory>(h)->bytes.data(); return VK_SUCCESS;
   };
   dev.vk.UnmapMemory = [](VkDevice, VkDeviceMemory) {};
   dev.vk.AllocateCommandBuffers = [](VkDevice, const VkCommandBufferAllocateInfo *, VkCommandBuffer *out) {
      CHECK(failStage == 4); *out = VK_NULL_HANDLE; return VK_ERROR_OUT_OF_HOST_MEMORY;
   };
   dev.vk.DestroyImage = [](VkDevice, VkImage h, const VkAllocationCallbacks *) { CHECK(images); --images; delete object<Image>(h); };
   dev.vk.DestroyBuffer = [](VkDevice, VkBuffer h, const VkAllocationCallbacks *) { CHECK(buffers); --buffers; delete object<Buffer>(h); };
   dev.vk.FreeMemory = [](VkDevice, VkDeviceMemory h, const VkAllocationCallbacks *) { CHECK(memories); --memories; delete object<Memory>(h); };
   dev.vk.CmdPipelineBarrier2 = [](VkCommandBuffer, const VkDependencyInfo *in) {
      if (in->memoryBarrierCount) {
         CHECK(in->pMemoryBarriers[0].srcAccessMask & VK_ACCESS_2_MEMORY_WRITE_BIT);
         CHECK(in->pMemoryBarriers[0].dstAccessMask & VK_ACCESS_2_MEMORY_READ_BIT);
         ++barriers;
      }
   };
   dev.vk.CmdCopyBuffer = [](VkCommandBuffer, VkBuffer src, VkBuffer dst, UINT count, const VkBufferCopy *r) {
      CHECK(count == 1);
      if (src == dst) CHECK(r->srcOffset + r->size <= r->dstOffset || r->dstOffset + r->size <= r->srcOffset);
      Copy c; c.srcBuffer = object<Buffer>(src); c.dstBuffer = object<Buffer>(dst);
      c.buffer = *r; c.barrier = barriers; copies.push_back(c);
   };
   dev.vk.CmdCopyImage = [](VkCommandBuffer, VkImage src, VkImageLayout sl, VkImage dst,
                          VkImageLayout dl, UINT count, const VkImageCopy *r) {
      CHECK(sl == VK_IMAGE_LAYOUT_GENERAL && dl == sl && count == 1);
      if (src == dst) {
         CHECK(r->srcSubresource.mipLevel != r->dstSubresource.mipLevel ||
            r->srcOffset.x + r->extent.width <= (UINT)r->dstOffset.x ||
            r->dstOffset.x + r->extent.width <= (UINT)r->srcOffset.x ||
            r->srcOffset.y + r->extent.height <= (UINT)r->dstOffset.y ||
            r->dstOffset.y + r->extent.height <= (UINT)r->srcOffset.y ||
            r->srcOffset.z + r->extent.depth <= (UINT)r->dstOffset.z ||
            r->dstOffset.z + r->extent.depth <= (UINT)r->srcOffset.z);
      }
      Copy c; c.srcImage = object<Image>(src); c.dstImage = object<Image>(dst);
      c.image = *r; c.barrier = barriers; copies.push_back(c);
   };
   dev.vk.CmdBlitImage = [](VkCommandBuffer, VkImage src, VkImageLayout sl, VkImage dst,
                           VkImageLayout dl, UINT count, const VkImageBlit *b, VkFilter filter) {
      CHECK(src != dst && sl == VK_IMAGE_LAYOUT_GENERAL && dl == sl && count == 1);
      CHECK(filter == VK_FILTER_NEAREST && b->srcOffsets[1].x - b->srcOffsets[0].x ==
            b->dstOffsets[1].x - b->dstOffsets[0].x && b->srcOffsets[1].y - b->srcOffsets[0].y ==
            b->dstOffsets[1].y - b->dstOffsets[0].y);
      Copy c; c.srcImage = object<Image>(src); c.dstImage = object<Image>(dst);
      c.image.srcSubresource = b->srcSubresource; c.image.dstSubresource = b->dstSubresource;
      c.image.srcOffset = b->srcOffsets[0]; c.image.dstOffset = b->dstOffsets[0];
      c.image.extent = {(UINT)(b->srcOffsets[1].x - b->srcOffsets[0].x),
                         (UINT)(b->srcOffsets[1].y - b->srcOffsets[0].y), 1};
      c.barrier = barriers; copies.push_back(c);
   };
}

static void execute(Device &dev)
{
   CHECK(!waits && !errors);
   if (copies.size() == 2) CHECK(copies[1].barrier > copies[0].barrier);
   for (const auto &c : copies) {
      if (c.srcBuffer) {
         std::memcpy(c.dstBuffer->memory->bytes.data() + c.buffer.dstOffset,
                     c.srcBuffer->memory->bytes.data() + c.buffer.srcOffset, c.buffer.size);
      } else {
         const auto &r = c.image;
         for (UINT z = 0; z < r.extent.depth; ++z)
            for (UINT y = 0; y < r.extent.height; ++y) {
               size_t src = (((size_t)r.srcOffset.z + z) * c.srcImage->height + r.srcOffset.y + y) * c.srcImage->width + r.srcOffset.x;
               size_t dst = (((size_t)r.dstOffset.z + z) * c.dstImage->height + r.dstOffset.y + y) * c.dstImage->width + r.dstOffset.x;
               std::memcpy(c.dstImage->bytes.data() + dst * 4, c.srcImage->bytes.data() + src * 4, r.extent.width * 4);
            }
      }
   }
   dev.submitted = dev.recordingSeq(); completed = dev.submitted;
   retire(&dev); copies.clear();
}

static void fill(std::vector<uint8_t> &bytes)
{ for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = (i * 37 + i / 11) & 255; }

static void image_copy(UINT depth, UINT sx, UINT sy, UINT dx, UINT dy)
{
   Adapter adapter; Device dev; init(dev, adapter);
   D3DWDDM1_3DDI_DEVICEFUNCS t; device_fill_table13(&t);
   Resource r; r.dev = &dev; r.kind = ResKind::Image;
   r.dim = depth == 1 ? D3D10DDIRESOURCE_TEXTURE2D : D3D10DDIRESOURCE_TEXTURE3D;
   r.width = 13; r.height = 9; r.depth = depth;
   r.format = DXGI_FORMAT_B8G8R8A8_UNORM; r.vkFormat = VK_FORMAT_B8G8R8A8_UNORM;
   r.imageType = depth == 1 ? VK_IMAGE_TYPE_2D : VK_IMAGE_TYPE_3D; r.aspects = VK_IMAGE_ASPECT_COLOR_BIT;
   CHECK(image_create(&dev, &r) == S_OK);
   auto image = object<Image>(r.storage.image); fill(image->bytes);
   auto expected = image->bytes, original = expected;
   const UINT sz = 0, dz = depth == 1 ? 0 : 1, slices = depth == 1 ? 1 : 2;
   D3D10_DDI_BOX box = {(LONG)sx, (LONG)sy, (LONG)sz, (LONG)sx + 8, (LONG)sy + 5, (LONG)slices};
   for (UINT z = 0; z < slices; ++z) for (UINT y = 0; y < 5; ++y) for (UINT x = 0; x < 8; ++x)
      for (UINT c = 0; c < 4; ++c)
         expected[(((size_t)(dz + z) * 9 + dy + y) * 13 + dx + x) * 4 + c] =
            original[(((size_t)z * 9 + sy + y) * 13 + sx + x) * 4 + c];
   t.pfnResourceCopyRegion({&dev}, {&r}, 0, dx, dy, dz, {&r}, 0, &box, 0);
   CHECK(copies.size() == 2 && images == 2 && memories == 2 && dev.deferred.size() == 2);
   retire(&dev); CHECK(images == 2 && memories == 2); /* no completion yet */
   execute(dev); CHECK(image->bytes == expected && images == 1 && memories == 1);
   /* Exact self-copy is a no-op; invalid coordinates cannot reach Vulkan. */
   t.pfnResourceCopyRegion({&dev}, {&r}, 0, sx, sy, sz, {&r}, 0, &box, 0);
   CHECK(copies.empty());
   box.left = -1;
   t.pfnResourceCopyRegion({&dev}, {&r}, 0, 0, 0, 0, {&r}, 0, &box, 0);
   CHECK(errors == 1 && error == D3DDDIERR_DEVICEREMOVED && FAILED(dev.lost) && copies.empty());
   box.left = sx;
   for (UINT stage : {1u, 2u, 3u, 4u}) {
      dev.lost = S_OK;  // Inject each failure independently.
      errors = 0; failStage = stage;
      if (stage == 4) dev.cmd = VK_NULL_HANDLE;
      t.pfnResourceCopyRegion({&dev}, {&r}, 0, dx, dy, dz, {&r}, 0, &box, 0);
      CHECK(errors == 1 && error == D3DDDIERR_DEVICEREMOVED && FAILED(dev.lost));
      CHECK(copies.empty() && images == 1 && memories == 1 && image->bytes == expected);
      CHECK(dev.deferred.empty());
   }
   storage_destroy(&dev, r.storage, true); CHECK(!images && !memories && !waits);
}

static void dxgi_self_blt(UINT sx, UINT sy, UINT dx, UINT dy)
{
   Adapter adapter; Device dev; init(dev, adapter);
   DXGI1_4_DDI_BASE_FUNCTIONS t; dxgi_fill_presentation14(&t);
   Resource r; r.dev = &dev; r.kind = ResKind::Image; r.dim = D3D10DDIRESOURCE_TEXTURE2D;
   r.width = 13; r.height = 9; r.format = DXGI_FORMAT_B8G8R8A8_UNORM;
   r.vkFormat = VK_FORMAT_B8G8R8A8_UNORM; r.aspects = VK_IMAGE_ASPECT_COLOR_BIT;
   CHECK(image_create(&dev, &r) == S_OK);
   auto image = object<Image>(r.storage.image); fill(image->bytes);
   auto expected = image->bytes; const auto original = expected;
   for (UINT y = 0; y < 5; ++y) for (UINT x = 0; x < 8; ++x) for (UINT c = 0; c < 4; ++c)
      expected[((dy + y) * 13 + dx + x) * 4 + c] = original[((sy + y) * 13 + sx + x) * 4 + c];
   DXGI_DDI_ARG_BLT1 args = {};
   args.hDevice = (DXGI_DDI_HDEVICE)&dev; args.hSrcResource = args.hDstResource = (DXGI_DDI_HRESOURCE)&r;
   args.SrcLeft = sx; args.SrcTop = sy; args.SrcRight = sx + 8; args.SrcBottom = sy + 5;
   args.DstLeft = dx; args.DstTop = dy; args.DstRight = dx + 8; args.DstBottom = dy + 5;
   args.Rotate = DXGI_DDI_MODE_ROTATION_IDENTITY;
   CHECK(t.pfnBlt1(&args) == S_OK);
   CHECK(copies.size() == 2 && images == 2 && memories == 2 && dev.deferred.size() == 2);
   retire(&dev); CHECK(images == 2 && memories == 2);
   execute(dev); CHECK(image->bytes == expected && images == 1 && memories == 1);
   // Fail after the source snapshot has already been recorded, at rotated
   // Blt pipeline allocation. The target must remain intact and the scratch
   // must survive until that first copy completes, despite the returned OOM.
   r.bind = D3D10_DDI_BIND_RENDER_TARGET;
   args.Rotate = DXGI_DDI_MODE_ROTATION_ROTATE180;
   dev.vk.CreatePipelineLayout = [](VkDevice, const VkPipelineLayoutCreateInfo *,
                                   const VkAllocationCallbacks *, VkPipelineLayout *) {
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   };
   CHECK(t.pfnBlt1(&args) == E_OUTOFMEMORY);
   CHECK(errors == 1 && error == E_OUTOFMEMORY && SUCCEEDED(dev.lost));
   errors = 0; // the injected failure was reported; execution below checks retirement
   CHECK(copies.size() == 1 && images == 2 && memories == 2 && image->bytes == expected);
   retire(&dev); CHECK(images == 2 && memories == 2);
   execute(dev); CHECK(images == 1 && memories == 1 && image->bytes == expected);
   args.Rotate = DXGI_DDI_MODE_ROTATION_IDENTITY; r.bind = 0;
   for (UINT stage : {1u, 2u, 3u, 4u}) {
      failStage = stage; errors = 0;
      if (stage == 4) dev.cmd = VK_NULL_HANDLE;
      const HRESULT hr = t.pfnBlt1(&args);
      CHECK(FAILED(hr) && (stage == 4 || hr == E_OUTOFMEMORY));
      CHECK(copies.empty() && images == 1 && memories == 1 && image->bytes == expected && dev.deferred.empty());
   }
   storage_destroy(&dev, r.storage, true); CHECK(!images && !memories && !waits);
}

static void buffer_copy(UINT from, UINT to)
{
   Adapter adapter; Device dev; init(dev, adapter);
   D3DWDDM1_3DDI_DEVICEFUNCS t; device_fill_table13(&t);
   Resource r; r.dev = &dev; r.size = r.width = 128;
   CHECK(slice_alloc(&dev, r.size, 256, &r.slice) == S_OK);
   for (UINT i = 0; i < r.size; ++i) r.slice.cpu()[i] = (i * 17 + 3) & 255;
   std::vector<uint8_t> expected(r.slice.cpu(), r.slice.cpu() + r.size);
   const auto original = expected;
   for (UINT i = 0; i < 72; ++i) expected[to + i] = original[from + i];
   D3D10_DDI_BOX box = {(LONG)from, 0, 0, (LONG)from + 72, 1, 1};
   t.pfnResourceCopyRegion({&dev}, {&r}, 0, to, 0, 0, {&r}, 0, &box, 0);
   CHECK(copies.size() == 2 && r.slice.chunk->live == 2 && dev.deferredSlices.size() == 1);
   retire(&dev); CHECK(r.slice.chunk->live == 2);
   execute(dev); CHECK(r.slice.chunk->live == 1 && !std::memcmp(r.slice.cpu(), expected.data(), r.size));
   slice_release(&dev, r.slice, r.sliceLastUse); upload_destroy(&dev);
   CHECK(!buffers && !memories && !waits);
}

static void staging_copy()
{
   Adapter adapter; Device dev; init(dev, adapter);
   D3DWDDM1_3DDI_DEVICEFUNCS t; device_fill_table13(&t);
   Resource r; r.dev = &dev; r.kind = ResKind::Staging; r.width = 13; r.height = 9; r.depth = 3;
   r.dim = D3D10DDIRESOURCE_TEXTURE3D;
   r.format = DXGI_FORMAT_B8G8R8A8_UNORM; r.aspects = VK_IMAGE_ASPECT_COLOR_BIT;
   r.size = 64 * 9 * 3; r.layout.push_back({0, 64, 64 * 9, 13, 9, 3});
   CHECK(slice_alloc(&dev, r.size, 256, &r.slice) == S_OK);
   std::vector<uint8_t> original(r.size); fill(original);
   std::memcpy(r.slice.cpu(), original.data(), r.size); auto expected = original;
   for (UINT z = 0; z < 2; ++z) for (UINT y = 0; y < 5; ++y) for (UINT x = 0; x < 8 * 4; ++x)
      expected[(z + 1) * 64 * 9 + (y + 2) * 64 + 3 * 4 + x] = original[z * 64 * 9 + (y + 1) * 64 + 4 + x];
   D3D10_DDI_BOX box = {1, 1, 0, 9, 6, 2};
   t.pfnResourceCopyRegion({&dev}, {&r}, 0, 3, 2, 1, {&r}, 0, &box, 0);
   CHECK(!errors && !waits && copies.empty() && !std::memcmp(r.slice.cpu(), expected.data(), r.size));
   slice_release(&dev, r.slice, 0); upload_destroy(&dev); CHECK(!buffers && !memories);
}

static void buffer_scratch_failures()
{
   for (UINT stage : {5u, 2u, 6u, 7u, 4u}) {
      Adapter adapter; Device dev; init(dev, adapter);
      D3DWDDM1_3DDI_DEVICEFUNCS t; device_fill_table13(&t);
      Resource r; r.dev = &dev; r.size = r.width = 128;
      CHECK(slice_alloc(&dev, r.size, 256, &r.slice) == S_OK);
      std::vector<uint8_t> original(r.size); fill(original);
      std::memcpy(r.slice.cpu(), original.data(), r.size);
      /* Force a new scratch chunk for allocation failures. For the command
       * failure, exercise releasing a scratch slice in the live source chunk. */
      if (stage != 4) dev.currentChunk->used = dev.currentChunk->size;
      else dev.cmd = VK_NULL_HANDLE;
      failStage = stage;
      D3D10_DDI_BOX box = {4, 0, 0, 76, 1, 1};
      t.pfnResourceCopyRegion({&dev}, {&r}, 0, 12, 0, 0, {&r}, 0, &box, 0);
      CHECK(errors == 1 && error == D3DDDIERR_DEVICEREMOVED && FAILED(dev.lost));
      CHECK(copies.empty() && buffers == 1 && memories == 1 && !waits &&
            r.slice.chunk->live == 1 && dev.deferredSlices.empty());
      CHECK(!std::memcmp(r.slice.cpu(), original.data(), r.size));
      slice_release(&dev, r.slice, 0); upload_destroy(&dev); CHECK(!buffers && !memories);
   }
}

static void discard_hint()
{
   Adapter adapter; Device dev; init(dev, adapter);
   D3DWDDM1_3DDI_DEVICEFUNCS t; device_fill_table13(&t);
   Resource r; r.dev = &dev; r.kind = ResKind::Image; r.storage.lastUse = 7;
   r.storage.layoutReady = r.storage.preinitialized = true; r.storage.writeSerial = 11;
   View v; v.dev = &dev; v.res = &r; v.type = ViewType::RTV;
   dev.rtv[0] = &v; dev.writeSerial = 11;
   D3D10_DDI_RECT rects[] = {{1, 2, 9, 8}, {0, 0, 0, 0}};
   t.pfnDiscard({&dev}, D3D10DDI_HT_RESOURCE, &r, nullptr, 0);
   t.pfnDiscard({&dev}, D3D10DDI_HT_RENDERTARGETVIEW, &v, rects, 2);
   CHECK(!errors && !waits && copies.empty() && dev.deferred.empty() && dev.rtv[0] == &v &&
         dev.submitted == 6 && completed == 6 && r.storage.lastUse == 7 && r.storage.writeSerial == 11 &&
         r.storage.layoutReady && r.storage.preinitialized);
}
}

void overlap_contracts()
{
   dxgi_self_blt(1, 1, 3, 2); dxgi_self_blt(3, 2, 1, 1);
   buffer_copy(4, 12); buffer_copy(12, 4);
   image_copy(1, 1, 1, 3, 2); image_copy(1, 3, 2, 1, 1); image_copy(3, 1, 1, 3, 2);
   staging_copy(); buffer_scratch_failures(); discard_hint();
   std::puts("PASS: overlapping buffer/image/staging copies preserve snapshot pixels and outside bytes, use legal scratch transfers/barriers, retire only after completion; Discard preserves in-flight ownership (CPU control)");
}
