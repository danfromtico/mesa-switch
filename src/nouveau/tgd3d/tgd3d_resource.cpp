/* SPDX-License-Identifier: MIT
 *
 * Resources, views, CPU access and GPU copies.
 */
#include "tgd3d.h"
#include "../nvdec/nvdec.h"
#include "vulkan/util/vk_internal_exts.h"

#include <string.h>

namespace tgd3d {

HRESULT resource_result(VkResult result)
{
   switch (result) {
   case VK_SUCCESS: return S_OK;
   case VK_ERROR_OUT_OF_HOST_MEMORY:
   case VK_ERROR_OUT_OF_DEVICE_MEMORY:
   case VK_ERROR_MEMORY_MAP_FAILED: return E_OUTOFMEMORY;
   case VK_ERROR_DEVICE_LOST: return D3DDDIERR_DEVICEREMOVED;
   case VK_ERROR_FORMAT_NOT_SUPPORTED: return DXGI_ERROR_UNSUPPORTED;
   default: return E_FAIL;
   }
}

static inline View *V(void *p) { return static_cast<View *>(p); }

static bool empty_box(const D3D10_DDI_BOX *box)
{
   /* D3D defines inverted intervals as empty too. Test signed endpoints
    * before subtraction can turn them into a huge unsigned copy extent. */
   return box && (box->left >= box->right || box->top >= box->bottom ||
                  box->front >= box->back);
}

static uint32_t block_dim(const FormatInfo *info) { return (info->flags & FMT_BC) ? 4 : 1; }

static uint32_t row_bytes(const FormatInfo *info, uint32_t width)
{
   const uint32_t block = block_dim(info);
   return ((width + block - 1) / block) * info->bytes;
}

static uint32_t row_count(const FormatInfo *info, uint32_t height)
{
   if (info->flags & FMT_NV12) return height + height / 2;
   const uint32_t block = block_dim(info);
   return (height + block - 1) / block;
}

static VkImageAspectFlags copy_aspect(const Resource *res)
{
   /* Depth-stencil data is copied through its depth aspect. */
   return (res->aspects & VK_IMAGE_ASPECT_DEPTH_BIT) ? VK_IMAGE_ASPECT_DEPTH_BIT :
          (res->aspects & VK_IMAGE_ASPECT_STENCIL_BIT) ? VK_IMAGE_ASPECT_STENCIL_BIT :
          VK_IMAGE_ASPECT_COLOR_BIT;
}

/* ------------------------------------------------------------------ */
/* Image storage                                                       */

static bool family_formats(const Device *dev, DXGI_FORMAT format, std::vector<VkFormat> *list)
{
   const FormatInfo *info = format_info(format);
   if (!info || !info->family)
      return false;
   for (unsigned f = 1; f < 100; f++) {
      const FormatInfo *other = format_info((DXGI_FORMAT)f);
      if (!other || other->family != info->family || other->vk == VK_FORMAT_UNDEFINED ||
          (other->flags & (FMT_DEPTH | FMT_STENCIL)))
         continue;
      /* A fully typed resource only ever gains its sRGB or linear twin. */
      if (!(info->flags & FMT_TYPELESS) && other->vk != info->vk &&
          !((other->flags ^ info->flags) & FMT_SRGB))
         continue;
      bool seen = false;
      for (VkFormat v : *list)
         seen |= v == other->vk;
      if (!seen)
         list->push_back(other->vk);
   }
   (void)dev;
   return list->size() > 1;
}

HRESULT image_create(Device *dev, Resource *res)
{
   const FormatInfo *info = format_info(res->format);
   if (!info || res->vkFormat == VK_FORMAT_UNDEFINED)
      return DXGI_ERROR_UNSUPPORTED;
   if ((info->flags & FMT_R_AS_ALPHA) && !dev->hasA8 &&
       (res->bind & D3D10_DDI_BIND_RENDER_TARGET))
      return DXGI_ERROR_UNSUPPORTED;
   const bool depth = res->aspects & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
   VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
   ici.imageType = res->imageType;
   ici.format = res->vkFormat;
   ici.extent = {res->width, res->height, res->depth};
   ici.mipLevels = res->mips;
   ici.arrayLayers = res->arraySize;
   ici.samples = VK_SAMPLE_COUNT_1_BIT;
   ici.tiling = VK_IMAGE_TILING_OPTIMAL;
   ici.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
   /* DXGI may ask for a rotated presentation copy even when the application's
    * back buffer has no public SRV binding. The copy samples it internally. */
   if (res->bind & (D3D10_DDI_BIND_SHADER_RESOURCE | D3D10_DDI_BIND_RENDER_TARGET |
                    D3D10_DDI_BIND_PRESENT))
      ici.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
   if (res->bind & D3D11_DDI_BIND_UNORDERED_ACCESS)
      ici.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
   if (res->bind & D3D10_DDI_BIND_RENDER_TARGET)
      ici.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
   if (res->bind & D3D10_DDI_BIND_DEPTH_STENCIL)
      ici.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
   ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
   ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
   if (res->tiledShared)
      ici.flags |= VK_IMAGE_CREATE_ALIAS_BIT;
   if (res->cube)
      ici.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
   if (res->imageType == VK_IMAGE_TYPE_3D && (res->bind & D3D10_DDI_BIND_RENDER_TARGET))
      ici.flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;

   std::vector<VkFormat> formats;
   VkImageFormatListCreateInfo list = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
   const bool reinterpretable = !depth &&
      ((info->flags & FMT_TYPELESS) ||
       ((res->bind & (D3D10_DDI_BIND_RENDER_TARGET | D3D10_DDI_BIND_PRESENT)) &&
        (info->family == 7 || info->family == 19 || info->family == 20)));
   if (reinterpretable && family_formats(dev, res->format, &formats)) {
      ici.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
      list.viewFormatCount = (uint32_t)formats.size();
      list.pViewFormats = formats.data();
      ici.pNext = &list;
   }

   struct wsi_image_create_info layout = {};
   VkExternalMemoryImageCreateInfo external = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
   RuntimeResource *runtime = res->storage.runtime;
   if (runtime && !res->tiledShared) {
      /* Linear storage with the allocation's own pitch. The image is laid
       * over memory another owner may already have rendered. */
      ici.tiling = VK_IMAGE_TILING_LINEAR;
      ici.initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
      if (dev->nvk) {
         layout.sType = VK_STRUCTURE_TYPE_WSI_IMAGE_CREATE_INFO_MESA;
         layout.pNext = ici.pNext;
         layout.scanout = res->primary;
         layout.win32_row_pitch = res->pitch;
         ici.pNext = &layout;
      } else {
         external.pNext = ici.pNext;
         external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
         ici.pNext = &external;
      }
   }
   trace(TGD3D_TRACE_RESOURCE_PHASE, E_PENDING, 1, (uintptr_t)res);
   HRESULT hr = resource_result(dev->vk.CreateImage(dev->vkdev, &ici, nullptr, &res->storage.image));
   trace(TGD3D_TRACE_RESOURCE_PHASE, hr, 1, (uintptr_t)res);
   if (FAILED(hr))
      return hr;
   res->storage.aspects = res->aspects;
   res->storage.layoutReady = false;
   res->storage.preinitialized = runtime != nullptr && !res->tiledShared;

   if (res->tiledShared) {
      VkMemoryRequirements req;
      dev->vk.GetImageMemoryRequirements(dev->vkdev, res->storage.image, &req);
      hr = runtime ? runtime_shared_tiled_check(dev, runtime, ici, req) :
         runtime_shared_tiled_create(dev, res, ici, req, &res->storage.runtime);
      if (SUCCEEDED(hr))
         hr = runtime_bind_image(dev, res->storage.runtime, res->storage.image, &res->storage.memory,
                                 &res->storage.writtenAllocation);
      if (SUCCEEDED(hr)) {
         res->storage.externalShared = true;
         /* The private runtime owner imports the producer's actual NVK
          * backing. Its contents are in GENERAL after external release;
          * transitioning an opened optimal image from UNDEFINED would lose
          * them. The first use acquires external ownership instead. */
         res->storage.layoutReady = res->opened;
      }
   } else if (runtime) {
      {
         /* Verify both the private-pitch ICD and the generic control driver.
          * A successful CreateImage alone does not prove shared addressing. */
         VkImageSubresource sub = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
         VkSubresourceLayout got;
         dev->vk.GetImageSubresourceLayout(dev->vkdev, res->storage.image, &sub, &got);
         if (got.rowPitch != res->pitch || got.offset != 0)
            hr = DXGI_ERROR_UNSUPPORTED;
      }
      if (SUCCEEDED(hr)) {
         trace(TGD3D_TRACE_RESOURCE_PHASE, E_PENDING, 4, (uintptr_t)res);
         hr = runtime_bind_image(dev, runtime, res->storage.image, &res->storage.memory,
                                 &res->storage.writtenAllocation);
         trace(TGD3D_TRACE_RESOURCE_PHASE, hr, 4, (uintptr_t)res);
      }
   } else {
      VkMemoryRequirements req;
      dev->vk.GetImageMemoryRequirements(dev->vkdev, res->storage.image, &req);
      if (runtime_private_allocations(dev) && res->hRT.handle) {
         /* Keep optimal tiling and its per-plane PTE kinds. Only backing
          * ownership changes; this is not a linear/shared image import. */
         hr = runtime_private_create(dev, res->hRT, req.size, &res->storage.runtime);
         if (SUCCEEDED(hr))
            hr = runtime_bind_image(dev, res->storage.runtime, res->storage.image, &res->storage.memory,
                                    &res->storage.writtenAllocation);
      } else {
         VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
         alloc.allocationSize = req.size;
         alloc.memoryTypeIndex = dev->adapter->vk.findMemoryType(
            req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
         if (alloc.memoryTypeIndex == UINT32_MAX)
            alloc.memoryTypeIndex = dev->adapter->vk.findMemoryType(req.memoryTypeBits, 0);
         trace(TGD3D_TRACE_RESOURCE_PHASE, E_PENDING, 2, (uintptr_t)res);
         hr = alloc.memoryTypeIndex == UINT32_MAX ? DXGI_ERROR_UNSUPPORTED :
            resource_result(dev->vk.AllocateMemory(dev->vkdev, &alloc, nullptr, &res->storage.memory));
         trace(TGD3D_TRACE_RESOURCE_PHASE, hr, 2, (uintptr_t)res);
         if (SUCCEEDED(hr)) {
            trace(TGD3D_TRACE_RESOURCE_PHASE, E_PENDING, 3, (uintptr_t)res);
            hr = resource_result(dev->vk.BindImageMemory(dev->vkdev, res->storage.image, res->storage.memory, 0));
            trace(TGD3D_TRACE_RESOURCE_PHASE, hr, 3, (uintptr_t)res);
         }
      }
   }
   if (FAILED(hr)) {
      dev->vk.DestroyImage(dev->vkdev, res->storage.image, nullptr);
      res->storage.image = VK_NULL_HANDLE;
      if (res->storage.memory)
         dev->vk.FreeMemory(dev->vkdev, res->storage.memory, nullptr);
      res->storage.memory = VK_NULL_HANDLE;
   }
   return hr;
}

void storage_destroy(Device *dev, ImageStorage &storage, bool immediate)
{
   const uint64_t seq = immediate ? 0 : storage.lastUse;
   if (seq > dev->submitted && SUCCEEDED(dev->lost) &&
       FAILED(runtime_make_resident(dev, storage.runtime)))
      TG_ERROR(dev, DXGI_ERROR_DEVICE_REMOVED);
   /* Rotation leaves cached views in other logical resources. Retire all
    * views of this backing before its image, including already-deferred ones. */
   const bool ready = immediate || sequence_done(dev, seq);
   for (auto it = dev->deferred.begin(); it != dev->deferred.end();) {
      if (!storage.image || it->type != DeferredType::ImageView || it->image != storage.image) {
         ++it;
      } else if (ready) {
         dev->vk.DestroyImageView(dev->vkdev, (VkImageView)it->handle, nullptr);
         it = dev->deferred.erase(it);
      } else {
         it->seq = seq;
         ++it;
      }
   }
   auto retire_view = [&](VkImageView handle) {
      if (ready) dev->vk.DestroyImageView(dev->vkdev, handle, nullptr);
      else defer(dev, DeferredType::ImageView, (uint64_t)handle, seq, storage.image);
   };
   for (auto &view : storage.blitViews) {
      if (view.handle) retire_view(view.handle);
      view = {};
   }
   for (View *view : dev->views) {
      for (auto &entry : view->cache) {
         if (entry.view && entry.image == storage.image) {
            retire_view(entry.view);
            entry = {};
         }
      }
   }
   if (ready) {
      /* The image and its memory go first: freeing the memory is what
       * releases this device's GPU mapping of a runtime allocation. */
      if (storage.image)
         dev->vk.DestroyImage(dev->vkdev, storage.image, nullptr);
      if (storage.memory)
         dev->vk.FreeMemory(dev->vkdev, storage.memory, nullptr);
      if (storage.runtime) {
         HRESULT hr = runtime_destroy(storage.runtime);
         if (FAILED(hr))
            TG_ERROR(dev, hr);
      }
   } else {
      defer(dev, DeferredType::Image, (uint64_t)storage.image, seq);
      defer(dev, DeferredType::Memory, (uint64_t)storage.memory, seq);
      defer(dev, DeferredType::Runtime, (uint64_t)(uintptr_t)storage.runtime, seq);
   }
   storage = ImageStorage();
}

/* ------------------------------------------------------------------ */
/* Texel transfer helpers                                              */

static void copy_rows(uint8_t *dst, uint32_t dst_pitch, const uint8_t *src, uint32_t src_pitch,
                      uint32_t bytes, uint32_t rows)
{
   if (dst_pitch == src_pitch && bytes == dst_pitch) {
      memcpy(dst, src, (size_t)bytes * rows);
      return;
   }
   for (uint32_t y = 0; y < rows; y++)
      memcpy(dst + (size_t)y * dst_pitch, src + (size_t)y * src_pitch, bytes);
}

static VkImageSubresourceLayers layers_of(const Resource *res, UINT sub, VkImageAspectFlags aspect)
{
   VkImageSubresourceLayers l;
   l.aspectMask = aspect;
   l.mipLevel = sub % res->mips;
   l.baseArrayLayer = sub / res->mips;
   l.layerCount = 1;
   return l;
}

/* Uploads a box of texels to an image through a transient host slice. */
static HRESULT upload_texels(Device *dev, Resource *res, UINT sub, const D3D10_DDI_BOX *box,
                             const void *data, UINT row_pitch, UINT depth_pitch)
{
   /* Packed D3D depth/stencil upload needs plane unpacking. Do not silently
    * upload only depth and report that the stencil bytes were preserved. */
   if ((res->aspects & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) ==
       (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))
      return DXGI_ERROR_UNSUPPORTED;
   const FormatInfo *info = format_info(res->format);
   const UINT mip = sub % res->mips;
   D3D10_DDI_BOX full = {0, 0, 0, (LONG)res->mipWidth(mip), (LONG)res->mipHeight(mip),
                         (LONG)res->mipDepth(mip)};
   if (!box)
      box = &full;
   const uint32_t width = box->right - box->left, height = box->bottom - box->top;
   const uint32_t depth = box->back - box->front;
   if (!width || !height || !depth)
      return S_OK;
   const uint32_t bytes = row_bytes(info, width), rows = row_count(info, height);
   const VkDeviceSize total = (VkDeviceSize)bytes * rows * depth;
   Slice slice;
   HRESULT allocation = slice_alloc(dev, total, 16, &slice);
   if (FAILED(allocation))
      return allocation;
   for (uint32_t z = 0; z < depth; z++)
      copy_rows(slice.cpu() + (size_t)z * bytes * rows, bytes,
                static_cast<const uint8_t *>(data) + (size_t)z * depth_pitch, row_pitch, bytes, rows);
   VkCommandBuffer cmd = cmd_begin(dev, &res->storage);
   if (!cmd) {
      slice_release(dev, slice, 0);
      return FAILED(dev->lost) ? dev->lost : E_OUTOFMEMORY;
   }
   image_use(dev, res->storage, true);
   VkBufferImageCopy region = {};
   region.bufferOffset = slice.offset;
   region.imageSubresource = layers_of(res, sub, copy_aspect(res));
   region.imageOffset = {box->left, box->top, box->front};
   region.imageExtent = {width, height, depth};
   dev->vk.CmdCopyBufferToImage(cmd, slice.buffer(), res->storage.image, VK_IMAGE_LAYOUT_GENERAL,
                                1, &region);
   dev->stats.uploadBytes += total;
   slice_release(dev, slice, dev->recordingSeq());
   return S_OK;
}

static void staging_layout(Resource *res, const FormatInfo *info)
{
   VkDeviceSize offset = 0;
   res->layout.resize(res->subresources());
   for (UINT layer = 0; layer < res->arraySize; layer++) {
      for (UINT mip = 0; mip < res->mips; mip++) {
         SubLayout &l = res->layout[mip + layer * res->mips];
         l.width = res->mipWidth(mip);
         l.height = res->mipHeight(mip);
         l.depth = res->mipDepth(mip);
         l.rowPitch = (row_bytes(info, l.width) + 3) & ~3u;
         l.depthPitch = l.rowPitch * row_count(info, l.height);
         l.offset = offset;
         offset += ((VkDeviceSize)l.depthPitch * l.depth + 15) & ~VkDeviceSize(15);
      }
   }
   res->size = offset;
}

/* ------------------------------------------------------------------ */
/* Create / open / destroy                                             */

static SIZE_T APIENTRY CalcPrivateResourceSize(D3D10DDI_HDEVICE, const D3D10DDIARG_CREATERESOURCE *)
{
   return sizeof(Resource);
}

static SIZE_T APIENTRY CalcPrivateOpenedResourceSize(D3D10DDI_HDEVICE,
                                                     const D3D10DDIARG_OPENRESOURCE *)
{
   return sizeof(Resource);
}

static HRESULT init_image_fields(Device *dev, Resource *res)
{
   VkComponentMapping swizzle;
   const FormatInfo *info = format_info(res->format);
   if (!info)
      return DXGI_ERROR_UNSUPPORTED;
   res->vkFormat = resource_vk_format(dev, res->format, res->bind);
   if (res->vkFormat == VK_FORMAT_UNDEFINED)
      return DXGI_ERROR_UNSUPPORTED;
   if (info->depth != VK_FORMAT_UNDEFINED && res->vkFormat == info->depth) {
      res->aspects = VK_IMAGE_ASPECT_DEPTH_BIT;
      if (info->depth == VK_FORMAT_D24_UNORM_S8_UINT || info->depth == VK_FORMAT_D32_SFLOAT_S8_UINT)
         res->aspects |= VK_IMAGE_ASPECT_STENCIL_BIT;
   } else {
      res->aspects = VK_IMAGE_ASPECT_COLOR_BIT;
   }
   (void)swizzle;
   return S_OK;
}

static void resource_untrack(Device *dev, Resource *res)
{
   for (Resource **link = &dev->resources; *link; link = &(*link)->next) {
      if (*link == res) { *link = res->next; res->next = nullptr; return; }
   }
}

void resource_creation_error(Device *dev, HRESULT hr, DXGI_DDI_PRIMARY_DESC *primary)
{
   if (FAILED(dev->lost)) hr = dev->lost;
   if (hr == D3DDDIERR_OUTOFVIDEOMEMORY) hr = E_OUTOFMEMORY;
   if (primary && (hr == DXGI_ERROR_UNSUPPORTED || hr == DXGI_DDI_ERR_UNSUPPORTED)) {
      /* This is the DDI scanout opt-out, not DXGI_ERROR_UNSUPPORTED from the
       * public API. The latter is a critical runtime error, not a fallback.
       * Only a failed primary creation can request the DXGI proxy path. */
      primary->DriverFlags = DXGI_DDI_PRIMARY_DRIVER_FLAG_NO_SCANOUT;
      TG_ERROR(dev, DXGI_DDI_ERR_UNSUPPORTED);
   } else if (hr == E_OUTOFMEMORY) {
      TG_ERROR(dev, hr);
   } else {
      /* Ordinary Create/OpenResource permits OOM and device removal. An
       * unsupported/invalid internal request cannot become a successful
       * resource or a fake OOM; retain its original reason in the trace and
       * report that this device cannot continue through the allowed DDI code. */
      RemovalOnlyErrorScope scope(dev);
      TG_ERROR(dev, hr);
   }
}

static void retire_resource_image(Device *dev, ImageStorage &storage)
{
   const bool runtime_lifetime = runtime_requires_resource_lifetime(storage.runtime);
   if (runtime_lifetime && !sequence_done(dev, storage.lastUse)) {
      end_rendering(dev);
      if (FAILED(wait_sequence(dev, storage.lastUse, TGD3D_WAIT_DESTROY))) {
         /* An unsubmitted recording can contain this image. Never submit
          * it after the resource's synchronous destruction has unwound. */
         if (SUCCEEDED(dev->lost)) TG_ERROR(dev, DXGI_ERROR_DEVICE_REMOVED);
         if (cleanup_idle(dev)) {
            storage_destroy(dev, storage, true);
            return;
         }
         park_image(dev, storage);
         return;
      }
   }
   storage_destroy(dev, storage, runtime_lifetime);
}

static void resource_fail(Device *dev, Resource *res, HRESULT hr,
                          DXGI_DDI_PRIMARY_DESC *primary = nullptr)
{
   if (res->slice)
      slice_release(dev, res->slice, std::max(res->sliceLastUse, res->sliceLastWrite));
   slice_release(dev, res->soCounter, res->soCounterLastUse);
   if (res->storage.image || res->storage.runtime)
      retire_resource_image(dev, res->storage);
   resource_untrack(dev, res);
   res->~Resource();
   resource_creation_error(dev, hr, primary);
}

static HRESULT create_resource(Device *dev, const D3D10DDIARG_CREATERESOURCE *desc,
                                Resource *res)
{
   if (FAILED(dev->lost))
      return dev->lost;
   res->dim = desc->ResourceDimension;
   res->usage = desc->Usage;
   res->bind = desc->BindFlags;
   res->mapFlags = desc->MapFlags;
   res->misc = desc->MiscFlags;
   res->format = desc->Format;
   res->mips = desc->MipLevels ? desc->MipLevels : 1;
   res->arraySize = desc->ArraySize ? desc->ArraySize : 1;
   res->width = desc->pMipInfoList[0].TexelWidth;
   res->height = desc->pMipInfoList[0].TexelHeight ? desc->pMipInfoList[0].TexelHeight : 1;
   res->depth = desc->pMipInfoList[0].TexelDepth ? desc->pMipInfoList[0].TexelDepth : 1;
   trace(TGD3D_TRACE_RESOURCE, E_PENDING, (uint64_t)res->dim << 32 | res->bind,
         (uint64_t)res->width << 32 | res->height);

   if (desc->SampleDesc.Count > 1)
      return DXGI_ERROR_UNSUPPORTED;   /* no multisampling is advertised */

   HRESULT hr = S_OK;
   if (res->dim == D3D10DDIRESOURCE_BUFFER) {
      res->kind = ResKind::Buffer;
      res->size = res->width;
      hr = resource_slice_alloc(res, &res->slice);
      if (SUCCEEDED(hr) && dev->caps && dev->caps->transformFeedback && (res->bind & D3D10_DDI_BIND_STREAM_OUTPUT)) {
         hr = slice_alloc(dev, sizeof(UINT), 4, &res->soCounter);
         if (SUCCEEDED(hr)) memset(res->soCounter.cpu(), 0, sizeof(UINT));
      }
      if (SUCCEEDED(hr) && desc->pInitialDataUP && desc->pInitialDataUP[0].pSysMem)
         memcpy(res->slice.cpu(), desc->pInitialDataUP[0].pSysMem, res->size);
      return hr;
   }

   const FormatInfo *info = format_info(res->format);
   if (!info)
      return DXGI_ERROR_UNSUPPORTED;
   if (info->flags & FMT_NV12) {
      /* NVDEC's real tiled DPB is owned by the KMD. The public output is a
       * distinct linear NV12 allocation populated only after completion.
       * Array slices have independent Y/UV extents and retain normal WDDM
       * backing, CPU map, copy and retirement ownership. */
      if (!video_device_supported(dev) || res->dim != D3D10DDIRESOURCE_TEXTURE2D ||
          res->mips != 1 || res->arraySize > TG_NVDEC_MAX_SURFACES ||
          !tg_nvdec_output_size_supported(res->width, res->height) ||
          res->depth != 1 || res->misc || desc->pPrimaryDesc || desc->SampleDesc.Quality ||
          (res->usage == D3D10_DDI_USAGE_STAGING ? res->bind != 0 :
           res->usage != D3D10_DDI_USAGE_DEFAULT || res->bind != D3D11_DDI_BIND_DECODER || res->mapFlags))
         return DXGI_ERROR_UNSUPPORTED;
      res->kind = ResKind::Staging;
      res->maps.resize(res->arraySize);
      staging_layout(res, info);
      hr = resource_slice_alloc(res, &res->slice);
      if (FAILED(hr)) return hr;
      for (UINT sub = 0; desc->pInitialDataUP && sub < res->arraySize; ++sub) {
         const auto &up = desc->pInitialDataUP[sub];
         if (!up.pSysMem) continue;
         if (up.SysMemPitch < res->width) return E_INVALIDARG;
         const auto &l = res->layout[sub];
         copy_rows(res->slice.cpu() + l.offset, l.rowPitch,
                   static_cast<const uint8_t *>(up.pSysMem), up.SysMemPitch,
                   res->width, res->height * 3 / 2);
      }
      return S_OK;
   }
   res->imageType = res->dim == D3D10DDIRESOURCE_TEXTURE1D ? VK_IMAGE_TYPE_1D :
                    res->dim == D3D10DDIRESOURCE_TEXTURE3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
   res->cube = res->dim == D3D10DDIRESOURCE_TEXTURECUBE;
   const bool linearRgb = res->format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                          res->format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                          (dev->interfaceVersion == D3D9_RUNTIME_INTERFACE && res->format == DXGI_FORMAT_B8G8R8X8_UNORM);
   res->tiledShared = !desc->pPrimaryDesc && (res->misc & (D3D10_DDI_RESOURCE_MISC_SHARED |
      D3DWDDM2_0DDI_RESOURCE_MISC_DISPLAYABLE_SURFACE)) && (res->arraySize != 1 || !linearRgb);
   if (res->tiledShared) {
      if (desc->SampleDesc.Count != 1 || desc->SampleDesc.Quality)
         return DXGI_ERROR_UNSUPPORTED;
      hr = runtime_shared_tiled_validate(dev, res);
      if (FAILED(hr)) return hr;
   }
   res->maps.resize(res->subresources());

   if (res->usage == D3D10_DDI_USAGE_STAGING && !desc->pPrimaryDesc &&
       !(res->misc & D3D10_DDI_RESOURCE_MISC_SHARED)) {
      res->kind = ResKind::Staging;
      staging_layout(res, info);
      hr = resource_slice_alloc(res, &res->slice);
      if (FAILED(hr))
         return hr;
      for (UINT sub = 0; desc->pInitialDataUP && sub < res->subresources(); sub++) {
         const auto &up = desc->pInitialDataUP[sub];
         const SubLayout &l = res->layout[sub];
         if (!up.pSysMem)
            continue;
         for (uint32_t z = 0; z < l.depth; z++)
            copy_rows(res->slice.cpu() + l.offset + (size_t)z * l.depthPitch, l.rowPitch,
                      static_cast<const uint8_t *>(up.pSysMem) + (size_t)z * up.SysMemSlicePitch,
                      up.SysMemPitch, row_bytes(info, l.width), row_count(info, l.height));
      }
      return S_OK;
   }

   res->kind = ResKind::Image;
   hr = init_image_fields(dev, res);
   if (SUCCEEDED(hr) && desc->pPrimaryDesc) {
      UINT width = 0, height = 0, pitch = 0;
      hr = runtime_primary_create(dev, desc, res->hRT, &res->storage.runtime, &width, &height, &pitch);
      res->primary = true;
      res->pitch = pitch;
   } else if (SUCCEEDED(hr) && (res->misc & (D3D10_DDI_RESOURCE_MISC_SHARED |
             D3DWDDM2_0DDI_RESOURCE_MISC_DISPLAYABLE_SURFACE)) && !res->tiledShared) {
      hr = runtime_shared_create(dev, desc, res->hRT, &res->storage.runtime, &res->pitch);
   } else if (SUCCEEDED(hr) && !res->tiledShared && (res->misc & ~(D3D10_DDI_RESOURCE_AUTO_GEN_MIP_MAP |
                                            D3D10_DDI_RESOURCE_MISC_DISCARD_ON_PRESENT))) {
      /* Keyed-mutex, GDI-compatible and NT-handle sharing need DDI levels
       * this driver does not negotiate. */
      hr = DXGI_ERROR_UNSUPPORTED;
   }
   if (SUCCEEDED(hr))
      hr = image_create(dev, res);
   for (UINT sub = 0; SUCCEEDED(hr) && desc->pInitialDataUP && sub < res->subresources(); sub++) {
      const auto &up = desc->pInitialDataUP[sub];
      if (up.pSysMem)
         hr = upload_texels(dev, res, sub, nullptr, up.pSysMem, up.SysMemPitch, up.SysMemSlicePitch);
   }
   /* DriverFlags is output-only. An accepted primary supports direct scanout;
    * do not retain NO_SCANOUT from a reused descriptor and redirect DXGI to
    * copy-style presentation. Failed creation still returns its original error.
    */
   if (SUCCEEDED(hr) && res->primary)
      desc->pPrimaryDesc->DriverFlags = 0;
   return hr;
}

void create_resource_with_stride(D3D10DDI_HDEVICE hDevice, const D3D10DDIARG_CREATERESOURCE *desc,
                                    D3D10DDI_HRESOURCE hResource, D3D10DDI_HRTRESOURCE hRT, UINT stride)
{
   Device *dev = D(hDevice);
   Resource *res = new (hResource.pDrvPrivate) Resource();
   res->dev = dev;
   res->hRT = hRT;
   res->structureStride = stride;
   res->next = dev->resources;
   dev->resources = res;
   trace(TGD3D_TRACE_RESOURCE_PHASE, E_PENDING, 0, (uintptr_t)res);
   HRESULT hr = create_resource(dev, desc, res);
   if (FAILED(hr))
      resource_fail(dev, res, hr,
         (desc->BindFlags & D3D10_DDI_BIND_PRESENT) ? desc->pPrimaryDesc : nullptr);
   trace(TGD3D_TRACE_RESOURCE_PHASE, hr, 0, (uintptr_t)res);
}

static void APIENTRY CreateResource(D3D10DDI_HDEVICE h, const D3D10DDIARG_CREATERESOURCE *desc,
                                    D3D10DDI_HRESOURCE resource, D3D10DDI_HRTRESOURCE rt)
{
   create_resource_with_stride(h, desc, resource, rt, 0);
}

static void APIENTRY OpenResource(D3D10DDI_HDEVICE hDevice, const D3D10DDIARG_OPENRESOURCE *args,
                                  D3D10DDI_HRESOURCE hResource, D3D10DDI_HRTRESOURCE hRT)
{
   Device *dev = D(hDevice);
   Resource *res = new (hResource.pDrvPrivate) Resource();
   res->dev = dev;
   res->hRT = hRT;
   res->next = dev->resources;
   dev->resources = res;
   res->opened = true;
   RuntimeSharedDescription d = {};
   HRESULT hr = FAILED(dev->lost) ? dev->lost :
      runtime_shared_open(dev, args, hRT, &res->storage.runtime, &d);
   trace(TGD3D_TRACE_OPEN_RESOURCE, hr, (uint64_t)d.width << 32 | d.height, d.pitch);
   if (SUCCEEDED(hr)) {
      res->primary = runtime_is_primary(res->storage.runtime);
      res->dim = D3D10DDIRESOURCE_TEXTURE2D;
      res->kind = ResKind::Image;
      res->usage = d.usage;
      res->bind = d.bind_flags;
      res->misc = D3D10_DDI_RESOURCE_MISC_SHARED;
      if (runtime_is_displayable(res->storage.runtime))
         res->misc |= D3DWDDM2_0DDI_RESOURCE_MISC_DISPLAYABLE_SURFACE;
      res->format = d.format;
      res->width = d.width;
      res->height = d.height;
      res->pitch = d.pitch;
      res->tiledShared = runtime_is_tiled_shared(res->storage.runtime);
      res->arraySize = runtime_shared_layers(res->storage.runtime);
      res->maps.resize(res->arraySize);
      hr = init_image_fields(dev, res);
   }
   if (SUCCEEDED(hr))
      hr = image_create(dev, res);
   /* The runtime does not destroy a resource whose open failed. */
   if (FAILED(hr))
      resource_fail(dev, res, hr);
}

RuntimeResource *resource_runtime(const Resource *res)
{
   return res->kind == ResKind::Image ? res->storage.runtime :
      res->slice ? res->slice.chunk->runtime : nullptr;
}

void resource_unbind(Device *dev, Resource *res)
{
   for (auto &target : dev->soTargets)
      if (target == res) target = nullptr;
   for (auto &uav : dev->computeUav)
      if (uav && uav->res == res) uav = nullptr;
   for (auto &uav : dev->graphicsUav)
      if (uav && uav->res == res) { uav = nullptr; dev->dirty |= DIRTY_GRAPHICS_UAVS; }
   for (auto &stage : dev->stage)
      for (auto &srv : stage.srv)
         if (srv && srv->res == res) {
            srv = nullptr;
            dev->dirty |= DIRTY_TEXTURES | DIRTY_TEXEL_BUFFERS;
         }
   for (auto &rtv : dev->rtv)
      if (rtv && rtv->res == res) {
         end_rendering(dev); rtv = nullptr; dev->dirty = DIRTY_ALL;
      }
   if (dev->dsv && dev->dsv->res == res) {
      end_rendering(dev); dev->dsv = nullptr; dev->dirty = DIRTY_ALL;
   }
   for (auto &stage : dev->stage)
      for (UINT i = 0; i < D3D_CB_SLOTS; i++)
         if (stage.cb[i] == res) {
            stage.cb[i] = nullptr;
            stage.cbDirty |= 1u << i;
         }
   for (auto &vb : dev->vb)
      if (vb.buffer == res) {
         vb.buffer = nullptr;
         dev->dirty |= DIRTY_VERTEX_BUFFERS;
      }
   if (dev->indexBuffer == res) {
      dev->indexBuffer = nullptr;
      dev->dirty |= DIRTY_INDEX_BUFFER;
   }
}

static void APIENTRY DestroyResource(D3D10DDI_HDEVICE hDevice, D3D10DDI_HRESOURCE hResource)
{
   Device *dev = D(hDevice);
   RemovalOnlyErrorScope errors(dev);
   Resource *res = R(hResource);
   flush_pending_clears(dev, nullptr);
   resource_unbind(dev, res);
   for (auto &map : res->maps)
      if (map.upload)
         slice_release(dev, map.upload, 0);
   if (res->slice)
      slice_release(dev, res->slice, res->sliceLastUse > res->sliceLastWrite ?
                                        res->sliceLastUse : res->sliceLastWrite);
   slice_release(dev, res->soCounter, res->soCounterLastUse);
   if (res->storage.image || res->storage.runtime)
      retire_resource_image(dev, res->storage);
   resource_untrack(dev, res);
   res->~Resource();
}

/* ------------------------------------------------------------------ */
/* Views                                                               */

static VkImageViewType arrayed_type(VkImageViewType type, bool arrayed)
{
   switch (type) {
   case VK_IMAGE_VIEW_TYPE_1D:
   case VK_IMAGE_VIEW_TYPE_1D_ARRAY:
      return arrayed ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D;
   case VK_IMAGE_VIEW_TYPE_2D:
   case VK_IMAGE_VIEW_TYPE_2D_ARRAY:
      return arrayed ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
   default:
      return type;
   }
}

VkImageView view_get(View *view, bool arrayed)
{
   Resource *res = view->res;
   const VkImage image = res->storage.image;
   const bool shaderView = view->type == ViewType::SRV || view->type == ViewType::UAV;
   const bool key = shaderView ? arrayed :
      view->info.viewType == VK_IMAGE_VIEW_TYPE_2D_ARRAY;
   Device *dev = view->dev;
   View::Entry *slot = nullptr;
   for (auto &entry : view->cache) {
      if (entry.view && entry.image == image && entry.arrayed == key)
         return entry.view;
      if (!entry.view && !slot)
         slot = &entry;
   }
   if (!slot) {
      /* More backings than slots: drop the oldest entry once the queue has
       * passed every use of it. */
      slot = &view->cache[0];
      defer(dev, DeferredType::ImageView, (uint64_t)slot->view, dev->lastSeq(), slot->image);
      for (size_t i = 0; i + 1 < sizeof(view->cache) / sizeof(view->cache[0]); i++)
         view->cache[i] = view->cache[i + 1];
      slot = &view->cache[sizeof(view->cache) / sizeof(view->cache[0]) - 1];
      *slot = {};
   }
   VkImageViewCreateInfo info = view->info;
   info.image = image;
   if (shaderView) {
      info.viewType = arrayed_type(info.viewType, arrayed);
      if (!arrayed && info.viewType != VK_IMAGE_VIEW_TYPE_CUBE &&
          info.viewType != VK_IMAGE_VIEW_TYPE_3D)
         info.subresourceRange.layerCount = 1;
   }
   VkImageView handle = VK_NULL_HANDLE;
   const VkResult result = dev->vk.CreateImageView(dev->vkdev, &info, nullptr, &handle);
   if (result != VK_SUCCESS) {
      TG_ERROR(dev, resource_result(result));
      return VK_NULL_HANDLE;
   }
   *slot = {image, handle, key};
   return handle;
}

VkBufferView view_get_buffer(View *view)
{
   Resource *res = view->res;
   Device *dev = view->dev;
   const VkBuffer buffer = res->slice.buffer();
   const VkDeviceSize offset = res->slice.offset + view->bufferOffset;
   if (view->bufferView && view->bufferViewBuffer == buffer && view->bufferViewOffset == offset)
      return view->bufferView;
   defer(dev, DeferredType::BufferView, (uint64_t)view->bufferView, dev->lastSeq());
   view->bufferView = VK_NULL_HANDLE;
   VkBufferViewCreateInfo info = {VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO};
   info.buffer = buffer;
   info.format = view->bufferFormat;
   info.offset = offset;
   info.range = view->bufferRange;
   const VkResult result = dev->vk.CreateBufferView(dev->vkdev, &info, nullptr, &view->bufferView);
   if (result != VK_SUCCESS) {
      TG_ERROR(dev, resource_result(result));
      return VK_NULL_HANDLE;
   }
   view->bufferViewBuffer = buffer;
   view->bufferViewOffset = offset;
   return view->bufferView;
}

static View *view_init(Device *dev, void *memory, Resource *res, ViewType type, DXGI_FORMAT format)
{
   View *view = new (memory) View();
   view->dev = dev;
   view->res = res;
   view->type = type;
   view->format = format;
   const FormatInfo *info = format_info(format);
   view->formatFlags = info ? info->flags : 0;
   view->next = res->views;
   res->views = view;
   dev->views.push_back(view);
   return view;
}

static void view_destroy(Device *dev, View *view)
{
   for (auto &uav : dev->computeUav)
      if (uav == view) uav = nullptr;
   for (auto &uav : dev->graphicsUav)
      if (uav == view) { uav = nullptr; dev->dirty |= DIRTY_GRAPHICS_UAVS; }
   if (view->counter) slice_release(dev, view->counter, dev->lastSeq());
   flush_pending_clears(dev, nullptr);
   end_rendering(dev);
   for (auto &stage : dev->stage)
      for (auto &srv : stage.srv)
         if (srv == view) {
            srv = nullptr;
            dev->dirty |= DIRTY_TEXTURES | DIRTY_TEXEL_BUFFERS;
         }
   for (auto &rtv : dev->rtv) {
      if (rtv == view) {
         rtv = nullptr;
         dev->dirty |= DIRTY_BLEND;
      }
   }
   if (dev->dsv == view) {
      dev->dsv = nullptr;
      dev->dirty |= DIRTY_DEPTH_STENCIL | DIRTY_BLEND;
   }
   const uint64_t seq = dev->lastSeq();
   for (auto &entry : view->cache)
      defer(dev, DeferredType::ImageView, (uint64_t)entry.view, seq, entry.image);
   defer(dev, DeferredType::BufferView, (uint64_t)view->bufferView, seq);
   for (View **link = &view->res->views; *link; link = &(*link)->next) {
      if (*link == view) {
         *link = view->next;
         break;
      }
   }
   for (size_t i = 0; i < dev->views.size(); i++) {
      if (dev->views[i] == view) {
         dev->views.erase(dev->views.begin() + i);
         break;
      }
   }
   view->~View();
}

static bool view_image_info(Device *dev, View *view, VkImageViewType type, uint32_t mip,
                            uint32_t mips, uint32_t layer, uint32_t layers)
{
   Resource *res = view->res;
   VkFormat format;
   VkImageAspectFlags aspect;
   VkComponentMapping swizzle;
   if (res->kind != ResKind::Image ||
       !view_vk_format(dev, res->format, res->bind, view->format, &format, &aspect, &swizzle))
      return false;
   if (mip >= res->mips || !mips || !layers)
      return false;
   if (mips == ~0u)
      mips = res->mips - mip;
   if (mips > res->mips - mip)
      return false;
   /* A 2D attachment view of a 3D image selects depth slices; a 3D SRV
    * still has one Vulkan array layer. Reject before subtracting/shifting. */
   const UINT available = res->imageType == VK_IMAGE_TYPE_3D && type != VK_IMAGE_VIEW_TYPE_3D ?
                          res->mipDepth(mip) : res->arraySize;
   if (layer >= available || layers > available - layer)
      return false;
   view->info = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
   view->info.viewType = type;
   view->info.format = format;
   view->info.components = view->type == ViewType::SRV ? swizzle : VkComponentMapping{};
   view->info.subresourceRange = {aspect, mip, mips, layer, layers};
   view->mip = mip;
   view->layers = layers;
   view->width = res->mipWidth(mip);
   view->height = res->mipHeight(mip);
   return true;
}

/* Untyped D3D views address a range in bytes/structure indices. The shader
 * converter uses an R32_UINT texel buffer for both, including atomic accesses. */
static HRESULT buffer_view(Device *dev, View *view, UINT first, UINT elements, bool raw)
{
   Resource *res = view->res;
   if (res->kind != ResKind::Buffer || !elements) return E_INVALIDARG;
   const FormatInfo *info = format_info(view->format);
   UINT stride;
   if (raw) {
      if (!(res->misc & D3D11_DDI_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS) || view->format != DXGI_FORMAT_R32_TYPELESS)
         return E_INVALIDARG;
      stride = 4;
      view->bufferFormat = VK_FORMAT_R32_UINT;
   } else if (res->structureStride) {
      if (view->format != DXGI_FORMAT_UNKNOWN) return E_INVALIDARG;
      stride = res->structureStride;
      view->bufferFormat = VK_FORMAT_R32_UINT;
   } else {
      if (!info || (info->flags & (FMT_TYPELESS | FMT_BC | FMT_DEPTH | FMT_STENCIL | FMT_NO_VIEW)))
         return E_INVALIDARG;
      stride = info->bytes;
      view->bufferFormat = vertex_vk_format(view->format);
      if (view->bufferFormat == VK_FORMAT_UNDEFINED) return DXGI_ERROR_UNSUPPORTED;
   }
   view->bufferOffset = VkDeviceSize(first) * stride;
   view->bufferRange = VkDeviceSize(elements) * stride;
   if (view->bufferOffset > res->size || view->bufferRange > res->size - view->bufferOffset)
      return E_INVALIDARG;
   VkFormatProperties props{};
   dev->adapter->vk.vk.GetPhysicalDeviceFormatProperties(dev->adapter->vk.pdev, view->bufferFormat, &props);
   const bool storage = view->type == ViewType::UAV;
   const UINT texel = raw || res->structureStride ? 4 : stride;
   VkDeviceSize alignment = storage ? dev->caps->storageTexelAlignment : dev->caps->uniformTexelAlignment;
   if (storage ? dev->caps->storageSingleTexel : dev->caps->uniformSingleTexel)
      alignment = std::min<VkDeviceSize>(alignment, texel % 3 == 0 ? texel / 3 : texel);
   if (!alignment || (res->slice.offset + view->bufferOffset) % alignment ||
       view->bufferRange / texel > dev->caps->maxTexelBufferElements ||
       !(props.bufferFeatures & (storage ? VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT :
                                          VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT)))
      return DXGI_ERROR_UNSUPPORTED;
   return S_OK;
}

void create_buffer_srv11(Device *dev, Resource *res, void *memory, UINT first, UINT count,
                         DXGI_FORMAT format, UINT flags)
{
   if (!res || res->dev != dev || !memory || !(res->bind & D3D10_DDI_BIND_SHADER_RESOURCE) ||
       (flags & ~D3D11_DDI_BUFFEREX_SRV_FLAG_RAW)) { TG_ERROR(dev, E_INVALIDARG); return; }
   View *view = view_init(dev, memory, res, ViewType::SRV, format);
   HRESULT hr = buffer_view(dev, view, first, count, flags & D3D11_DDI_BUFFEREX_SRV_FLAG_RAW);
   if (FAILED(hr)) { view_destroy(dev, view); TG_ERROR(dev, hr); }
}

void APIENTRY CreateUav11(D3D10DDI_HDEVICE h, const D3D11DDIARG_CREATEUNORDEREDACCESSVIEW *in,
                          D3D11DDI_HUNORDEREDACCESSVIEW out, D3D11DDI_HRTUNORDEREDACCESSVIEW)
{
   Device *dev = D(h);
   Resource *res = in ? R(in->hDrvResource) : nullptr;
   if (!res || !out.pDrvPrivate || res->dev != dev || !(res->bind & D3D11_DDI_BIND_UNORDERED_ACCESS)) {
      TG_ERROR(dev, E_INVALIDARG); return;
   }
   View *v = view_init(dev, out.pDrvPrivate, res, ViewType::UAV, in->Format);
   HRESULT hr = E_INVALIDARG;
   if (in->ResourceDimension == D3D10DDIRESOURCE_BUFFER) {
      const UINT flags = in->Buffer.Flags;
      const UINT counterBits = D3D11_DDI_BUFFER_UAV_FLAG_APPEND | D3D11_DDI_BUFFER_UAV_FLAG_COUNTER;
      if (!(flags & ~(D3D11_DDI_BUFFER_UAV_FLAG_RAW | counterBits)) &&
          (!(flags & counterBits) || (res->structureStride && !(flags & D3D11_DDI_BUFFER_UAV_FLAG_RAW) &&
                                       (flags & counterBits) != counterBits))) {
         hr = buffer_view(dev, v, in->Buffer.FirstElement, in->Buffer.NumElements, flags & D3D11_DDI_BUFFER_UAV_FLAG_RAW);
         if (SUCCEEDED(hr) && (flags & counterBits))
            hr = slice_alloc(dev, 4, dev->caps->counterAlignment, &v->counter);
      }
   } else if (res->kind == ResKind::Image) {
      bool ok = false;
      switch (in->ResourceDimension) {
      case D3D10DDIRESOURCE_TEXTURE1D:
         ok = res->imageType == VK_IMAGE_TYPE_1D && view_image_info(dev, v, VK_IMAGE_VIEW_TYPE_1D,
            in->Tex1D.MipSlice, 1, in->Tex1D.FirstArraySlice, in->Tex1D.ArraySize);
         break;
      case D3D10DDIRESOURCE_TEXTURE2D:
         ok = res->imageType == VK_IMAGE_TYPE_2D && view_image_info(dev, v, VK_IMAGE_VIEW_TYPE_2D,
            in->Tex2D.MipSlice, 1, in->Tex2D.FirstArraySlice, in->Tex2D.ArraySize);
         break;
      case D3D10DDIRESOURCE_TEXTURE3D:
         if (res->imageType == VK_IMAGE_TYPE_3D && in->Tex3D.MipSlice < res->mips &&
             !in->Tex3D.FirstW && (in->Tex3D.WSize == ~0u || in->Tex3D.WSize == res->mipDepth(in->Tex3D.MipSlice)))
            ok = view_image_info(dev, v, VK_IMAGE_VIEW_TYPE_3D, in->Tex3D.MipSlice, 1, 0, 1);
         break;
      default: break;
      }
      if (ok) {
         VkFormatProperties props{};
         dev->adapter->vk.vk.GetPhysicalDeviceFormatProperties(dev->adapter->vk.pdev, v->info.format, &props);
         hr = (props.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) ? S_OK : DXGI_ERROR_UNSUPPORTED;
      }
   }
   if (FAILED(hr)) { view_destroy(dev, v); TG_ERROR(dev, hr); }
}

void APIENTRY DestroyUav11(D3D10DDI_HDEVICE h, D3D11DDI_HUNORDEREDACCESSVIEW view)
{
   view_destroy(D(h), static_cast<View *>(view.pDrvPrivate));
}

static SIZE_T APIENTRY CalcPrivateShaderResourceViewSize(D3D10DDI_HDEVICE,
                                                         const D3D10DDIARG_CREATESHADERRESOURCEVIEW *)
{
   return sizeof(View);
}

static void APIENTRY CreateShaderResourceView(D3D10DDI_HDEVICE hDevice,
                                              const D3D10DDIARG_CREATESHADERRESOURCEVIEW *args,
                                              D3D10DDI_HSHADERRESOURCEVIEW hView,
                                              D3D10DDI_HRTSHADERRESOURCEVIEW)
{
   Device *dev = D(hDevice);
   Resource *res = R(args->hDrvResource);
   View *view = view_init(dev, hView.pDrvPrivate, res, ViewType::SRV, args->Format);
   bool ok = false;
   switch (args->ResourceDimension) {
   case D3D10DDIRESOURCE_BUFFER: {
      const FormatInfo *info = format_info(args->Format);
      view->bufferFormat = vertex_vk_format(args->Format);
      if (res->kind == ResKind::Buffer && info && view->bufferFormat != VK_FORMAT_UNDEFINED) {
         view->bufferOffset = (VkDeviceSize)args->Buffer.FirstElement * info->bytes;
         view->bufferRange = (VkDeviceSize)args->Buffer.NumElements * info->bytes;
         ok = view->bufferOffset + view->bufferRange <= res->size;
      }
      break;
   }
   case D3D10DDIRESOURCE_TEXTURE1D:
      ok = view_image_info(dev, view, VK_IMAGE_VIEW_TYPE_1D, args->Tex1D.MostDetailedMip,
                           args->Tex1D.MipLevels, args->Tex1D.FirstArraySlice, args->Tex1D.ArraySize);
      break;
   case D3D10DDIRESOURCE_TEXTURE2D:
      ok = view_image_info(dev, view, VK_IMAGE_VIEW_TYPE_2D, args->Tex2D.MostDetailedMip,
                           args->Tex2D.MipLevels, args->Tex2D.FirstArraySlice, args->Tex2D.ArraySize);
      break;
   case D3D10DDIRESOURCE_TEXTURE3D:
      ok = view_image_info(dev, view, VK_IMAGE_VIEW_TYPE_3D, args->Tex3D.MostDetailedMip,
                           args->Tex3D.MipLevels, 0, 1);
      break;
   case D3D10DDIRESOURCE_TEXTURECUBE:
      ok = view_image_info(dev, view, VK_IMAGE_VIEW_TYPE_CUBE, args->TexCube.MostDetailedMip,
                           args->TexCube.MipLevels, 0, 6);
      break;
   default:
      break;
   }
   if (!ok) {
      view_destroy(dev, view);
      TG_ERROR(dev, E_INVALIDARG);
   }
}

static void APIENTRY DestroyShaderResourceView(D3D10DDI_HDEVICE hDevice,
                                               D3D10DDI_HSHADERRESOURCEVIEW hView)
{
   view_destroy(D(hDevice), V(hView.pDrvPrivate));
}

static SIZE_T APIENTRY CalcPrivateRenderTargetViewSize(D3D10DDI_HDEVICE,
                                                       const D3D10DDIARG_CREATERENDERTARGETVIEW *)
{
   return sizeof(View);
}

static void APIENTRY CreateRenderTargetView(D3D10DDI_HDEVICE hDevice,
                                            const D3D10DDIARG_CREATERENDERTARGETVIEW *args,
                                            D3D10DDI_HRENDERTARGETVIEW hView,
                                            D3D10DDI_HRTRENDERTARGETVIEW)
{
   Device *dev = D(hDevice);
   Resource *res = R(args->hDrvResource);
   View *view = view_init(dev, hView.pDrvPrivate, res, ViewType::RTV, args->Format);
   bool ok = false;
   switch (args->ResourceDimension) {
   case D3D10DDIRESOURCE_TEXTURE1D:
      ok = view_image_info(dev, view, args->Tex1D.ArraySize > 1 ? VK_IMAGE_VIEW_TYPE_1D_ARRAY :
                           VK_IMAGE_VIEW_TYPE_1D, args->Tex1D.MipSlice, 1,
                           args->Tex1D.FirstArraySlice, args->Tex1D.ArraySize);
      break;
   case D3D10DDIRESOURCE_TEXTURE2D:
      ok = view_image_info(dev, view, args->Tex2D.ArraySize > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY :
                           VK_IMAGE_VIEW_TYPE_2D, args->Tex2D.MipSlice, 1,
                           args->Tex2D.FirstArraySlice, args->Tex2D.ArraySize);
      break;
   case D3D10DDIRESOURCE_TEXTURECUBE:
      ok = view_image_info(dev, view, args->TexCube.ArraySize > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY :
                           VK_IMAGE_VIEW_TYPE_2D, args->TexCube.MipSlice, 1,
                           args->TexCube.FirstArraySlice, args->TexCube.ArraySize);
      break;
   case D3D10DDIRESOURCE_TEXTURE3D: {
      /* Depth slices of a volume are rendered as layers of a 2D array view. */
      if (args->Tex3D.MipSlice >= res->mips)
         break;
      const UINT depth = res->mipDepth(args->Tex3D.MipSlice);
      if (args->Tex3D.FirstW >= depth)
         break;
      const UINT count = args->Tex3D.WSize == ~0u ? depth - args->Tex3D.FirstW : args->Tex3D.WSize;
      ok = view_image_info(dev, view, count > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D,
                           args->Tex3D.MipSlice, 1, args->Tex3D.FirstW, count);
      break;
   }
   default:
      break;   /* buffer render targets are not implemented */
   }
   if (ok && (view->formatFlags & (FMT_DEPTH | FMT_STENCIL)))
      ok = false;
   if (!ok) {
      view_destroy(dev, view);
      TG_ERROR(dev, args->ResourceDimension == D3D10DDIRESOURCE_BUFFER ? E_NOTIMPL : E_INVALIDARG);
   }
}

static void APIENTRY DestroyRenderTargetView(D3D10DDI_HDEVICE hDevice, D3D10DDI_HRENDERTARGETVIEW hView)
{
   view_destroy(D(hDevice), V(hView.pDrvPrivate));
}

static SIZE_T APIENTRY CalcPrivateDepthStencilViewSize(D3D10DDI_HDEVICE,
                                                       const D3D10DDIARG_CREATEDEPTHSTENCILVIEW *)
{
   return sizeof(View);
}

void create_depth_stencil_view(Device *dev, const D3D10DDIARG_CREATEDEPTHSTENCILVIEW *args,
                               void *memory, UINT flags)
{
   Resource *res = R(args->hDrvResource);
   if (!memory || !res || res->dev != dev) { TG_ERROR(dev, E_INVALIDARG); return; }
   View *view = view_init(dev, memory, res, ViewType::DSV, args->Format);
   view->dsvFlags = flags;
   bool ok = false;
   switch (args->ResourceDimension) {
   case D3D10DDIRESOURCE_TEXTURE1D:
      ok = view_image_info(dev, view, args->Tex1D.ArraySize > 1 ? VK_IMAGE_VIEW_TYPE_1D_ARRAY :
                           VK_IMAGE_VIEW_TYPE_1D, args->Tex1D.MipSlice, 1,
                           args->Tex1D.FirstArraySlice, args->Tex1D.ArraySize);
      break;
   case D3D10DDIRESOURCE_TEXTURE2D:
      ok = view_image_info(dev, view, args->Tex2D.ArraySize > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY :
                           VK_IMAGE_VIEW_TYPE_2D, args->Tex2D.MipSlice, 1,
                           args->Tex2D.FirstArraySlice, args->Tex2D.ArraySize);
      break;
   case D3D10DDIRESOURCE_TEXTURECUBE:
      ok = view_image_info(dev, view, args->TexCube.ArraySize > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY :
                           VK_IMAGE_VIEW_TYPE_2D, args->TexCube.MipSlice, 1,
                           args->TexCube.FirstArraySlice, args->TexCube.ArraySize);
      break;
   default:
      break;
   }
   if (ok) {
      /* The attachment view always covers every aspect of the image. */
      view->info.subresourceRange.aspectMask = res->aspects;
      if (!(res->aspects & VK_IMAGE_ASPECT_DEPTH_BIT))
         ok = false;
      view->formatFlags = FMT_DEPTH | ((res->aspects & VK_IMAGE_ASPECT_STENCIL_BIT) ? FMT_STENCIL : 0);
   }
   if (!ok) {
      view_destroy(dev, view);
      TG_ERROR(dev, E_INVALIDARG);
   }
}

static void APIENTRY CreateDepthStencilView(D3D10DDI_HDEVICE hDevice,
                                            const D3D10DDIARG_CREATEDEPTHSTENCILVIEW *args,
                                            D3D10DDI_HDEPTHSTENCILVIEW hView,
                                            D3D10DDI_HRTDEPTHSTENCILVIEW)
{
   create_depth_stencil_view(D(hDevice), args, hView.pDrvPrivate, 0);
}

static void APIENTRY DestroyDepthStencilView(D3D10DDI_HDEVICE hDevice, D3D10DDI_HDEPTHSTENCILVIEW hView)
{
   view_destroy(D(hDevice), V(hView.pDrvPrivate));
}

/* ------------------------------------------------------------------ */
/* CPU access                                                          */

static void buffer_renamed(Device *dev, Resource *res)
{
   if (res->bind & D3D10_DDI_BIND_VERTEX_BUFFER)
      dev->dirty |= DIRTY_VERTEX_BUFFERS;
   if (res->bind & D3D10_DDI_BIND_INDEX_BUFFER)
      dev->dirty |= DIRTY_INDEX_BUFFER;
   if (res->bind & D3D10_DDI_BIND_CONSTANT_BUFFER) {
      for (auto &stage : dev->stage)
         for (unsigned i = 0; i < D3D_CB_SLOTS; i++)
            if (stage.cb[i] == res)
               stage.cbDirty |= 1u << i;
   }
   if (res->bind & D3D10_DDI_BIND_SHADER_RESOURCE)
      dev->dirty |= DIRTY_TEXEL_BUFFERS;
   if (res->bind & D3D11_DDI_BIND_UNORDERED_ACCESS)
      dev->dirty |= DIRTY_GRAPHICS_UAVS;
}

/* Gives the buffer storage that no recorded or queued command references. */
static HRESULT buffer_discard(Device *dev, Resource *res)
{
   const uint64_t last = res->sliceLastUse > res->sliceLastWrite ? res->sliceLastUse :
                                                                   res->sliceLastWrite;
   if (!last || sequence_done(dev, last))
      return S_OK;
   Slice fresh;
   HRESULT hr = resource_slice_alloc(res, &fresh);
   if (FAILED(hr))
      return hr;
   slice_release(dev, res->slice, last);
   res->slice = fresh;
   res->sliceLastUse = res->sliceLastWrite = 0;
   buffer_renamed(dev, res);
   return S_OK;
}

static HRESULT wait_for_cpu(Device *dev, uint64_t seq, UINT flags)
{
   if (!seq || sequence_done(dev, seq))
      return S_OK;
   if (flags & D3D10_DDI_MAP_FLAG_DONOTWAIT) {
      /* The work must at least be on the queue or the caller polls forever. */
      if (seq > dev->submitted) {
         HRESULT hr = flush(dev, false);
         if (FAILED(hr))
            return hr;
      }
      return sequence_done(dev, seq) ? S_OK : DXGI_DDI_ERR_WASSTILLDRAWING;
   }
   return wait_sequence(dev, seq, TGD3D_WAIT_READBACK);
}

static void APIENTRY ResourceMap(D3D10DDI_HDEVICE hDevice, D3D10DDI_HRESOURCE hResource, UINT sub,
                                 D3D10_DDI_MAP type, UINT flags, D3D10DDI_MAPPED_SUBRESOURCE *out)
{
   Device *dev = D(hDevice);
   Resource *res = R(hResource);
   out->pData = nullptr;
   out->RowPitch = out->DepthPitch = 0;
   if (runtime_is_offered(resource_runtime(res))) { TG_ERROR(dev, DXGI_ERROR_INVALID_CALL); return; }
   const bool reads = type == D3D10_DDI_MAP_READ || type == D3D10_DDI_MAP_READWRITE;
   const bool writes = type != D3D10_DDI_MAP_READ;
   HRESULT hr = S_OK;

   if (res->kind == ResKind::Buffer) {
      if (type == D3D10_DDI_MAP_WRITE_DISCARD) {
         hr = buffer_discard(dev, res);
      } else if (type != D3D10_DDI_MAP_WRITE_NOOVERWRITE) {
         hr = wait_for_cpu(dev, reads ? res->sliceLastWrite : 0, flags);
         if (SUCCEEDED(hr) && writes)
            hr = wait_for_cpu(dev, res->sliceLastUse, flags);
      }
      if (FAILED(hr)) {
         TG_ERROR(dev, hr);
         return;
      }
      hr = slice_map(dev, res->slice);
      if (FAILED(hr)) { TG_ERROR(dev, hr); return; }
      out->pData = res->slice.cpu();
      out->RowPitch = out->DepthPitch = (UINT)res->size;
      res->bufferMapped = true;
      return;
   }

   if (sub >= res->maps.size()) {
      TG_ERROR(dev, E_INVALIDARG);
      return;
   }
   const FormatInfo *info = format_info(res->format);
   MapState &map = res->maps[sub];
   if (res->kind == ResKind::Staging) {
      hr = wait_for_cpu(dev, reads ? res->sliceLastWrite : 0, flags);
      if (SUCCEEDED(hr) && writes)
         hr = wait_for_cpu(dev, res->sliceLastUse, flags);
      if (FAILED(hr)) {
         TG_ERROR(dev, hr);
         return;
      }
      hr = slice_map(dev, res->slice);
      if (FAILED(hr)) { TG_ERROR(dev, hr); return; }
      const SubLayout &l = res->layout[sub];
      out->pData = res->slice.cpu() + l.offset;
      out->RowPitch = l.rowPitch;
      out->DepthPitch = l.depthPitch;
      map.mapped = true;
      map.write = writes;
      if (reads)
         dev->stats.readbackBytes += (uint64_t)l.depthPitch * l.depth;
      return;
   }

   /* Dynamic textures are write-only: the application fills a host slice
    * that Unmap copies into the image. */
   if (res->aspects & VK_IMAGE_ASPECT_STENCIL_BIT) {
      TG_ERROR(dev, DXGI_ERROR_UNSUPPORTED);
      return;
   }
   if (type != D3D10_DDI_MAP_WRITE_DISCARD || map.mapped) {
      TG_ERROR(dev, E_INVALIDARG);
      return;
   }
   const UINT mip = sub % res->mips;
   const uint32_t pitch = (row_bytes(info, res->mipWidth(mip)) + 3) & ~3u;
   const uint32_t slice_pitch = pitch * row_count(info, res->mipHeight(mip));
   hr = slice_alloc(dev, (VkDeviceSize)slice_pitch * res->mipDepth(mip), 16, &map.upload);
   if (FAILED(hr)) {
      TG_ERROR(dev, hr);
      return;
   }
   map.mapped = true;
   map.write = true;
   out->pData = map.upload.cpu();
   out->RowPitch = pitch;
   out->DepthPitch = slice_pitch;
}

static void APIENTRY ResourceUnmap(D3D10DDI_HDEVICE hDevice, D3D10DDI_HRESOURCE hResource, UINT sub)
{
   Device *dev = D(hDevice);
   Resource *res = R(hResource);
   if (res->kind == ResKind::Buffer) { res->bufferMapped = false; return; }
   if (sub >= res->maps.size())
      return;
   MapState &map = res->maps[sub];
   if (!map.mapped)
      return;
   map.mapped = false;
   if (res->kind != ResKind::Image || !map.upload)
      return;
   const FormatInfo *info = format_info(res->format);
   const UINT mip = sub % res->mips;
   const uint32_t pitch = (row_bytes(info, res->mipWidth(mip)) + 3) & ~3u;
   VkCommandBuffer cmd = cmd_begin(dev, &res->storage);
   if (!cmd) {
      slice_release(dev, map.upload, 0);
      map.upload = Slice();
      return;
   }
   image_use(dev, res->storage, true);
   VkBufferImageCopy region = {};
   region.bufferOffset = map.upload.offset;
   region.bufferRowLength = pitch / info->bytes * block_dim(info);
   region.imageSubresource = layers_of(res, sub, copy_aspect(res));
   region.imageExtent = {res->mipWidth(mip), res->mipHeight(mip), res->mipDepth(mip)};
   dev->vk.CmdCopyBufferToImage(cmd, map.upload.buffer(), res->storage.image,
                                VK_IMAGE_LAYOUT_GENERAL, 1, &region);
   dev->stats.uploadBytes += map.upload.size;
   slice_release(dev, map.upload, dev->recordingSeq());
   map.upload = Slice();
}

static BOOL APIENTRY ResourceIsStagingBusy(D3D10DDI_HDEVICE hDevice, D3D10DDI_HRESOURCE hResource)
{
   Device *dev = D(hDevice);
   Resource *res = R(hResource);
   const uint64_t last = res->sliceLastUse > res->sliceLastWrite ? res->sliceLastUse :
                                                                   res->sliceLastWrite;
   return last && !sequence_done(dev, last);
}

static void APIENTRY ResourceUpdateSubresourceUP(D3D10DDI_HDEVICE hDevice, D3D10DDI_HRESOURCE hDst,
                                                 UINT sub, const D3D10_DDI_BOX *box,
                                                 const VOID *data, UINT row_pitch, UINT depth_pitch)
{
   if (empty_box(box))
      return;
   Device *dev = D(hDevice);
   Resource *res = R(hDst);
   HRESULT hr = S_OK;
   if (runtime_is_offered(resource_runtime(res))) { TG_ERROR(dev, DXGI_ERROR_INVALID_CALL); return; }
   if (res->kind == ResKind::Buffer) {
      const VkDeviceSize begin = box ? (VkDeviceSize)box->left : 0;
      const VkDeviceSize end = box ? (VkDeviceSize)box->right : res->size;
      if (begin > end || end > res->size) {
         TG_ERROR(dev, E_INVALIDARG);
         return;
      }
      const bool whole = begin == 0 && end == res->size;
      const uint64_t last = res->sliceLastUse > res->sliceLastWrite ? res->sliceLastUse :
                                                                      res->sliceLastWrite;
      if (last && !sequence_done(dev, last)) {
         /* Recorded draws still read the old contents. Take new storage and
          * carry over the bytes this update does not replace. */
         Slice old = res->slice;
         Slice fresh;
         hr = resource_slice_alloc(res, &fresh);
         if (FAILED(hr)) {
            TG_ERROR(dev, hr);
            return;
         }
         if (!whole) {
            if (res->sliceLastWrite && !sequence_done(dev, res->sliceLastWrite))
               hr = wait_sequence(dev, res->sliceLastWrite, TGD3D_WAIT_READBACK);
            if (SUCCEEDED(hr)) hr = slice_map(dev, old);
            if (FAILED(hr)) {
               slice_release(dev, fresh, 0);
               TG_ERROR(dev, hr);
               return;
            }
            memcpy(fresh.cpu(), old.cpu(), res->size);
         }
         slice_release(dev, old, last);
         res->slice = fresh;
         res->sliceLastUse = res->sliceLastWrite = 0;
         buffer_renamed(dev, res);
      }
      hr = slice_map(dev, res->slice);
      if (FAILED(hr)) { TG_ERROR(dev, hr); return; }
      memcpy(res->slice.cpu() + begin, data, end - begin);
      dev->stats.uploadBytes += end - begin;
      if (FAILED(hr))
         TG_ERROR(dev, hr);
      return;
   }
   if (sub >= res->subresources()) {
      TG_ERROR(dev, E_INVALIDARG);
      return;
   }
   if (res->kind == ResKind::Staging) {
      const FormatInfo *info = format_info(res->format);
      const SubLayout &l = res->layout[sub];
      D3D10_DDI_BOX full = {0, 0, 0, (LONG)l.width, (LONG)l.height, (LONG)l.depth};
      if (!box)
         box = &full;
      hr = wait_sequence(dev, res->sliceLastUse > res->sliceLastWrite ? res->sliceLastUse :
                                                                         res->sliceLastWrite,
                         TGD3D_WAIT_READBACK);
      if (SUCCEEDED(hr)) hr = slice_map(dev, res->slice);
      if (FAILED(hr)) {
         TG_ERROR(dev, hr);
         return;
      }
      const uint32_t block = block_dim(info);
      if (info->flags & FMT_NV12) {
         if (res->slice.chunk->videoWrites || box->front || box->back != 1 ||
             ((box->left | box->top | box->right | box->bottom) & 1) ||
             row_pitch < (UINT)(box->right - box->left)) {
            TG_ERROR(dev, E_INVALIDARG); return;
         }
         const uint32_t width = box->right - box->left, height = box->bottom - box->top;
         uint8_t *target = res->slice.cpu() + l.offset;
         copy_rows(target + (size_t)box->top * l.rowPitch + box->left, l.rowPitch,
                   static_cast<const uint8_t *>(data), row_pitch, width, height);
         copy_rows(target + (size_t)(l.height + box->top / 2) * l.rowPitch + box->left, l.rowPitch,
                   static_cast<const uint8_t *>(data) + (size_t)row_pitch * height, row_pitch, width, height / 2);
         return;
      }
      for (LONG z = box->front; z < box->back; z++)
         copy_rows(res->slice.cpu() + l.offset + (size_t)z * l.depthPitch +
                      (size_t)(box->top / block) * l.rowPitch + (size_t)(box->left / block) * info->bytes,
                   l.rowPitch,
                   static_cast<const uint8_t *>(data) + (size_t)(z - box->front) * depth_pitch,
                   row_pitch, row_bytes(info, box->right - box->left),
                   row_count(info, box->bottom - box->top));
      return;
   }
   hr = upload_texels(dev, res, sub, box, data, row_pitch, depth_pitch);
   if (FAILED(hr))
      TG_ERROR(dev, hr);
}

/* ------------------------------------------------------------------ */
/* GPU copies                                                          */

static VkBufferImageCopy staging_region(const Resource *staging, UINT sub, uint32_t x, uint32_t y,
                                        uint32_t z, const Resource *image, UINT image_sub)
{
   const FormatInfo *info = format_info(staging->format);
   const SubLayout &l = staging->layout[sub];
   const uint32_t block = block_dim(info);
   VkBufferImageCopy region = {};
   region.bufferOffset = staging->slice.offset + l.offset + (VkDeviceSize)z * l.depthPitch +
                         (VkDeviceSize)(y / block) * l.rowPitch + (VkDeviceSize)(x / block) * info->bytes;
   region.bufferRowLength = l.rowPitch / info->bytes * block;
   region.bufferImageHeight = ((l.height + block - 1) / block) * block;
   region.imageSubresource = layers_of(image, image_sub, copy_aspect(image));
   return region;
}

static bool copy_region(Device *dev, Resource *dst, UINT dst_sub, UINT dx, UINT dy, UINT dz,
                        Resource *src, UINT src_sub, const D3D10_DDI_BOX *box)
{
   if (empty_box(box))
      return true;
   if (FAILED(dev->lost)) return false;
   if (runtime_is_offered(resource_runtime(src)) || runtime_is_offered(resource_runtime(dst))) {
      TG_ERROR(dev, DXGI_ERROR_INVALID_CALL); return false;
   }
   HRESULT hr = S_OK;
   if (dst->kind == ResKind::Buffer && src->kind == ResKind::Buffer) {
      const VkDeviceSize begin = box ? (VkDeviceSize)box->left : 0;
      const VkDeviceSize end = box ? (VkDeviceSize)box->right : src->size;
      if (begin >= end || end > src->size || dx + (end - begin) > dst->size)
         return false;
      const bool same_buffer = src->slice.buffer() == dst->slice.buffer();
      const VkDeviceSize from = src->slice.offset + begin, to = dst->slice.offset + dx;
      if (same_buffer && from == to) return true;
      const bool overlap = same_buffer && from < to + (end - begin) && to < from + (end - begin);
      Slice scratch;
      if (overlap) {
         hr = slice_alloc(dev, end - begin, 256, &scratch);
         if (FAILED(hr)) { TG_ERROR(dev, hr); return false; }
      }
      VkCommandBuffer cmd = cmd_begin(dev);
      if (!cmd) { slice_release(dev, scratch, 0); return false; }
      barrier_for_read(dev, dev->writeSerial);
      VkBufferCopy region = {src->slice.offset + begin, dst->slice.offset + dx, end - begin};
      if (overlap) {
         /* Vulkan forbids overlapping source/destination bytes in one copy.
          * Capture the source before writing any part of the destination. */
         VkBufferCopy first = {from, scratch.offset, region.size};
         dev->vk.CmdCopyBuffer(dev->cmd, src->slice.buffer(), scratch.buffer(), 1, &first);
         buffer_written(dev);
         barrier_for_read(dev, dev->bufferWriteSerial);
         region.srcOffset = scratch.offset;
         dev->vk.CmdCopyBuffer(dev->cmd, scratch.buffer(), dst->slice.buffer(), 1, &region);
         slice_release(dev, scratch, dev->recordingSeq());
      } else {
         dev->vk.CmdCopyBuffer(dev->cmd, src->slice.buffer(), dst->slice.buffer(), 1, &region);
      }
      src->sliceLastUse = dst->sliceLastUse = dst->sliceLastWrite = dev->recordingSeq();
      buffer_written(dev);
      dev->stats.copyBytes += end - begin;
      return true;
   }
   if (dst->kind == ResKind::Buffer || src->kind == ResKind::Buffer ||
       dst_sub >= dst->subresources() || src_sub >= src->subresources()) {
      TG_ERROR(dev, E_INVALIDARG);
      return false;
   }
   const UINT src_mip = src_sub % src->mips;
   D3D10_DDI_BOX full = {0, 0, 0, (LONG)src->mipWidth(src_mip), (LONG)src->mipHeight(src_mip),
                         (LONG)src->mipDepth(src_mip)};
   if (!box)
      box = &full;
   const UINT dst_mip = dst_sub % dst->mips;
   if (box->left < 0 || box->top < 0 || box->front < 0 ||
       (UINT)box->right > src->mipWidth(src_mip) ||
       (UINT)box->bottom > src->mipHeight(src_mip) ||
       (UINT)box->back > src->mipDepth(src_mip) ||
       dx > dst->mipWidth(dst_mip) || dy > dst->mipHeight(dst_mip) || dz > dst->mipDepth(dst_mip) ||
       (UINT)(box->right - box->left) > dst->mipWidth(dst_mip) - dx ||
       (UINT)(box->bottom - box->top) > dst->mipHeight(dst_mip) - dy ||
       (UINT)(box->back - box->front) > dst->mipDepth(dst_mip) - dz) {
      TG_ERROR(dev, E_INVALIDARG); return false;
   }
   const VkExtent3D extent = {(uint32_t)(box->right - box->left), (uint32_t)(box->bottom - box->top),
                              (uint32_t)(box->back - box->front)};
   if (!extent.width || !extent.height || !extent.depth)
      return true;
   const FormatInfo *info = format_info(src->format);
   const bool same_sub = src == dst && src_sub == dst_sub;
   if (same_sub && dx == (UINT)box->left && dy == (UINT)box->top && dz == (UINT)box->front)
      return true;
   const bool overlap = same_sub && dx < (UINT)box->right && (UINT)box->left < dx + extent.width &&
      dy < (UINT)box->bottom && (UINT)box->top < dy + extent.height &&
      dz < (UINT)box->back && (UINT)box->front < dz + extent.depth;

   if (overlap && src->kind == ResKind::Image) {
      Resource scratch;
      scratch.dev = dev; scratch.kind = ResKind::Image; scratch.dim = src->dim;
      scratch.format = src->format; scratch.vkFormat = src->vkFormat;
      scratch.imageType = src->imageType; scratch.aspects = src->aspects;
      scratch.width = extent.width; scratch.height = extent.height; scratch.depth = extent.depth;
      hr = image_create(dev, &scratch);
      if (FAILED(hr)) { TG_ERROR(dev, hr); return false; }
      const bool copied = copy_region(dev, &scratch, 0, 0, 0, 0, src, src_sub, box) &&
         SUCCEEDED(dev->lost) && copy_region(dev, dst, dst_sub, dx, dy, dz, &scratch, 0, nullptr);
      /* Both copies use the ordinary image barriers and the same real
       * completion sequence. No CPU wait or early backing release. */
      storage_destroy(dev, scratch.storage, false);
      return copied;
   }

   if ((src->kind == ResKind::Staging || dst->kind == ResKind::Staging) &&
       ((src->aspects | dst->aspects) & VK_IMAGE_ASPECT_STENCIL_BIT)) {
      TG_ERROR(dev, DXGI_ERROR_UNSUPPORTED);
      return false;
   }

   if (dst->kind == ResKind::Staging && src->kind == ResKind::Staging) {
      if ((src->slice && src->slice.chunk->videoWrites) ||
          (dst->slice && dst->slice.chunk->videoWrites)) {
         TG_ERROR(dev, E_INVALIDARG); return false;
      }
      /* Both sides are host memory; order the copy after the GPU. */
      hr = wait_sequence(dev, src->sliceLastWrite, TGD3D_WAIT_READBACK);
      if (SUCCEEDED(hr))
         hr = wait_sequence(dev, dst->sliceLastUse > dst->sliceLastWrite ? dst->sliceLastUse :
                                                                          dst->sliceLastWrite,
                            TGD3D_WAIT_READBACK);
      if (SUCCEEDED(hr)) hr = slice_map(dev, src->slice);
      if (SUCCEEDED(hr)) hr = slice_map(dev, dst->slice);
      if (FAILED(hr)) {
         TG_ERROR(dev, hr);
         return false;
      }
      const SubLayout &sl = src->layout[src_sub], &dl = dst->layout[dst_sub];
      if ((info->flags & FMT_NV12) || dst->format == DXGI_FORMAT_NV12) {
         if (src->format != DXGI_FORMAT_NV12 || dst->format != DXGI_FORMAT_NV12 ||
             ((box->left | box->top | dx | dy | extent.width | extent.height) & 1u) ||
             box->front || dz || extent.depth != 1) {
            TG_ERROR(dev, E_INVALIDARG); return false;
         }
         const uint8_t *y = src->slice.cpu() + sl.offset + (size_t)box->top * sl.rowPitch + box->left;
         const uint8_t *uv = src->slice.cpu() + sl.offset + (size_t)(sl.height + box->top / 2) * sl.rowPitch + box->left;
         uint32_t source_pitch = sl.rowPitch;
         uint8_t *saved = nullptr;
         if (overlap) {
            const size_t ybytes = (size_t)extent.width * extent.height;
            saved = new (std::nothrow) uint8_t[ybytes + ybytes / 2];
            if (!saved) { TG_ERROR(dev, E_OUTOFMEMORY); return false; }
            copy_rows(saved, extent.width, y, sl.rowPitch, extent.width, extent.height);
            copy_rows(saved + ybytes, extent.width, uv, sl.rowPitch, extent.width, extent.height / 2);
            y = saved; uv = saved + ybytes; source_pitch = extent.width;
         }
         uint8_t *target = dst->slice.cpu() + dl.offset;
         copy_rows(target + (size_t)dy * dl.rowPitch + dx, dl.rowPitch,
                   y, source_pitch, extent.width, extent.height);
         copy_rows(target + (size_t)(dl.height + dy / 2) * dl.rowPitch + dx, dl.rowPitch,
                   uv, source_pitch, extent.width, extent.height / 2);
         delete[] saved;
         dev->stats.copyBytes += (uint64_t)extent.width * extent.height * 3 / 2;
         return true;
      }
      const uint32_t block = block_dim(info);
      const uint32_t bytes = row_bytes(info, extent.width), rows = row_count(info, extent.height);
      uint8_t *saved = nullptr;
      if (overlap) {
         saved = new (std::nothrow) uint8_t[(size_t)bytes * rows * extent.depth];
         if (!saved) { TG_ERROR(dev, E_OUTOFMEMORY); return false; }
         for (uint32_t z = 0; z < extent.depth; ++z)
            copy_rows(saved + (size_t)z * bytes * rows, bytes,
               src->slice.cpu() + sl.offset + (size_t)(box->front + z) * sl.depthPitch +
                  (size_t)(box->top / block) * sl.rowPitch + (size_t)(box->left / block) * info->bytes,
               sl.rowPitch, bytes, rows);
      }
      for (uint32_t z = 0; z < extent.depth; z++)
         copy_rows(dst->slice.cpu() + dl.offset + (size_t)(dz + z) * dl.depthPitch +
                      (size_t)(dy / block) * dl.rowPitch + (size_t)(dx / block) * info->bytes,
                   dl.rowPitch,
                   saved ? saved + (size_t)z * bytes * rows :
                   src->slice.cpu() + sl.offset + (size_t)(box->front + z) * sl.depthPitch +
                      (size_t)(box->top / block) * sl.rowPitch + (size_t)(box->left / block) * info->bytes,
                   saved ? bytes : sl.rowPitch, bytes, rows);
      delete[] saved;
      return true;
   }

   VkCommandBuffer cmd = cmd_begin(dev, dst->kind == ResKind::Image ? &dst->storage : nullptr);
   if (!cmd) return false;
   if (src->kind == ResKind::Image) {
      image_use(dev, src->storage, false);
      barrier_for_read(dev, src->storage.writeSerial);
   } else {
      barrier_for_read(dev, dev->writeSerial);
      src->sliceLastUse = dev->recordingSeq();
   }
   if (dst->kind == ResKind::Image)
      image_use(dev, dst->storage, true);
   cmd = dev->cmd;
   const uint64_t bytes = (uint64_t)row_bytes(info, extent.width) * row_count(info, extent.height) *
                          extent.depth;
   if (dst->kind == ResKind::Image && src->kind == ResKind::Image) {
      VkImageCopy region = {};
      region.srcSubresource = layers_of(src, src_sub, copy_aspect(src));
      region.srcOffset = {box->left, box->top, box->front};
      region.dstSubresource = layers_of(dst, dst_sub, copy_aspect(dst));
      region.dstOffset = {(int32_t)dx, (int32_t)dy, (int32_t)dz};
      region.extent = extent;
      /* Depth-stencil images copy both aspects together. */
      if (src->aspects == dst->aspects)
         region.srcSubresource.aspectMask = region.dstSubresource.aspectMask = src->aspects;
      dev->vk.CmdCopyImage(cmd, src->storage.image, VK_IMAGE_LAYOUT_GENERAL, dst->storage.image,
                           VK_IMAGE_LAYOUT_GENERAL, 1, &region);
      dev->stats.copyBytes += bytes;
   } else if (dst->kind == ResKind::Staging) {
      VkBufferImageCopy region = staging_region(dst, dst_sub, dx, dy, dz, src, src_sub);
      region.imageOffset = {box->left, box->top, box->front};
      region.imageExtent = extent;
      dev->vk.CmdCopyImageToBuffer(cmd, src->storage.image, VK_IMAGE_LAYOUT_GENERAL,
                                   dst->slice.buffer(), 1, &region);
      dst->sliceLastUse = dst->sliceLastWrite = dev->recordingSeq();
      buffer_written(dev);
      dev->stats.copyBytes += bytes;
   } else {
      VkBufferImageCopy region = staging_region(src, src_sub, box->left, box->top, box->front,
                                                dst, dst_sub);
      region.imageOffset = {(int32_t)dx, (int32_t)dy, (int32_t)dz};
      region.imageExtent = extent;
      dev->vk.CmdCopyBufferToImage(cmd, src->slice.buffer(), dst->storage.image,
                                   VK_IMAGE_LAYOUT_GENERAL, 1, &region);
      dev->stats.uploadBytes += bytes;
   }
   return true;
}

static void APIENTRY ResourceCopyRegion(D3D10DDI_HDEVICE hDevice, D3D10DDI_HRESOURCE hDst,
                                        UINT dst_sub, UINT dx, UINT dy, UINT dz,
                                        D3D10DDI_HRESOURCE hSrc, UINT src_sub,
                                        const D3D10_DDI_BOX *box)
{
   copy_region(D(hDevice), R(hDst), dst_sub, dx, dy, dz, R(hSrc), src_sub, box);
}

static void APIENTRY ResourceCopy(D3D10DDI_HDEVICE hDevice, D3D10DDI_HRESOURCE hDst,
                                  D3D10DDI_HRESOURCE hSrc)
{
   Device *dev = D(hDevice);
   Resource *dst = R(hDst), *src = R(hSrc);
   if (dst->kind == ResKind::Buffer) {
      copy_region(dev, dst, 0, 0, 0, 0, src, 0, nullptr);
      return;
   }
   const UINT count = dst->subresources() < src->subresources() ? dst->subresources() :
                                                                  src->subresources();
   for (UINT sub = 0; sub < count; sub++)
      copy_region(dev, dst, sub, 0, 0, 0, src, sub, nullptr);
}

static void APIENTRY ResourceResolveSubresource(D3D10DDI_HDEVICE hDevice, D3D10DDI_HRESOURCE, UINT,
                                                D3D10DDI_HRESOURCE, UINT, DXGI_FORMAT)
{
   /* Multisampled resources cannot be created, so there is nothing to resolve. */
   TG_ERROR(D(hDevice), DXGI_ERROR_UNSUPPORTED);
}

static void APIENTRY GenMips(D3D10DDI_HDEVICE hDevice, D3D10DDI_HSHADERRESOURCEVIEW hView)
{
   Device *dev = D(hDevice);
   View *view = V(hView.pDrvPrivate);
   Resource *res = view->res;
   if (res->kind != ResKind::Image)
      return;
   const VkImageSubresourceRange range = view->info.subresourceRange;
   if (range.levelCount < 2)
      return;
   VkCommandBuffer cmd = cmd_begin(dev, &res->storage);
   if (!cmd) return;
   image_use(dev, res->storage, true);
   cmd = dev->cmd;
   const uint32_t layers = view->info.viewType == VK_IMAGE_VIEW_TYPE_CUBE ? 6 :
      view->info.viewType == VK_IMAGE_VIEW_TYPE_3D ? 1 : range.layerCount;
   for (uint32_t i = 1; i < range.levelCount; i++) {
      const uint32_t src = range.baseMipLevel + i - 1, dst = src + 1;
      /* Each level reads the level written just before it. */
      barrier_for_read(dev, ++dev->writeSerial);
      cmd = dev->cmd;
      VkImageBlit blit = {};
      blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, src, range.baseArrayLayer, layers};
      blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, dst, range.baseArrayLayer, layers};
      blit.srcOffsets[1] = {(int32_t)res->mipWidth(src), (int32_t)res->mipHeight(src),
                            (int32_t)res->mipDepth(src)};
      blit.dstOffsets[1] = {(int32_t)res->mipWidth(dst), (int32_t)res->mipHeight(dst),
                            (int32_t)res->mipDepth(dst)};
      dev->vk.CmdBlitImage(cmd, res->storage.image, VK_IMAGE_LAYOUT_GENERAL, res->storage.image,
                           VK_IMAGE_LAYOUT_GENERAL, 1, &blit, VK_FILTER_LINEAR);
      dev->stats.copyBytes += (uint64_t)res->mipWidth(dst) * res->mipHeight(dst) * 4;
   }
   res->storage.writeSerial = ++dev->writeSerial;
}

static void APIENTRY ResourceReadAfterWriteHazard(D3D10DDI_HDEVICE, D3D10DDI_HRESOURCE)
{
   /* Ordering between a write and a later read is tracked per storage. */
}

static void APIENTRY ShaderResourceViewReadAfterWriteHazard(D3D10DDI_HDEVICE,
                                                            D3D10DDI_HSHADERRESOURCEVIEW,
                                                            D3D10DDI_HRESOURCE)
{
}

void resource_fill_table(D3D10DDI_DEVICEFUNCS *t)
{
   t->pfnCalcPrivateResourceSize = CalcPrivateResourceSize;
   t->pfnCalcPrivateOpenedResourceSize = CalcPrivateOpenedResourceSize;
   t->pfnCreateResource = CreateResource;
   t->pfnOpenResource = OpenResource;
   t->pfnDestroyResource = DestroyResource;
   t->pfnCalcPrivateShaderResourceViewSize = CalcPrivateShaderResourceViewSize;
   t->pfnCreateShaderResourceView = CreateShaderResourceView;
   t->pfnDestroyShaderResourceView = DestroyShaderResourceView;
   t->pfnCalcPrivateRenderTargetViewSize = CalcPrivateRenderTargetViewSize;
   t->pfnCreateRenderTargetView = CreateRenderTargetView;
   t->pfnDestroyRenderTargetView = DestroyRenderTargetView;
   t->pfnCalcPrivateDepthStencilViewSize = CalcPrivateDepthStencilViewSize;
   t->pfnCreateDepthStencilView = CreateDepthStencilView;
   t->pfnDestroyDepthStencilView = DestroyDepthStencilView;
   t->pfnResourceMap = ResourceMap;
   t->pfnResourceUnmap = ResourceUnmap;
   t->pfnDynamicIABufferMapNoOverwrite = ResourceMap;
   t->pfnDynamicIABufferMapDiscard = ResourceMap;
   t->pfnDynamicIABufferUnmap = ResourceUnmap;
   t->pfnDynamicConstantBufferMapDiscard = ResourceMap;
   t->pfnDynamicConstantBufferUnmap = ResourceUnmap;
   t->pfnDynamicResourceMapDiscard = ResourceMap;
   t->pfnDynamicResourceUnmap = ResourceUnmap;
   t->pfnStagingResourceMap = ResourceMap;
   t->pfnStagingResourceUnmap = ResourceUnmap;
   t->pfnResourceIsStagingBusy = ResourceIsStagingBusy;
   t->pfnResourceUpdateSubresourceUP = ResourceUpdateSubresourceUP;
   t->pfnDefaultConstantBufferUpdateSubresourceUP = ResourceUpdateSubresourceUP;
   t->pfnResourceCopy = ResourceCopy;
   t->pfnResourceCopyRegion = ResourceCopyRegion;
   t->pfnResourceResolveSubresource = ResourceResolveSubresource;
   t->pfnGenMips = GenMips;
   t->pfnResourceReadAfterWriteHazard = ResourceReadAfterWriteHazard;
   t->pfnShaderResourceViewReadAfterWriteHazard = ShaderResourceViewReadAfterWriteHazard;
}

}
