/* SPDX-License-Identifier: MIT */
#include "tgd3d_legacy9.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <new>

namespace tgd3d::legacy9 {

// These fixed-function textures cannot be render targets. Their CPU copy is
// authoritative, so independent levels can be locked without mapping an image
// that earlier draws still reference. Uploads are ordered on the graphics queue.
static HRESULT upload(Surface *surface, UINT subresource, const RECT &area)
{
   const auto &level = surface->texture[subresource];
   LegacyDevice *dev = surface->owner;
   const BYTE *source = level.bytes + size_t(area.top) * level.pitch + size_t(area.left) * 4;
   const D3D10_DDI_BOX box{area.left, area.top, 0, area.right, area.bottom, 1};
   dev->graphics.pfnResourceUpdateSubresourceUP(dev->handle(), surface->resource(), subresource,
                                               &box, source, level.pitch, 0);
   return dev->error;
}

// Unlock exposes CPU writes before the GPU upload can succeed. Retain the
// dirty rectangle across a transient allocation failure and retry before use.
static HRESULT upload_pending(Surface *surface, UINT subresource)
{
   auto &level = surface->texture[subresource];
   if (IsRectEmpty(&level.pendingUpload)) return S_OK;
   HRESULT hr = upload(surface, subresource, level.pendingUpload);
   if (SUCCEEDED(hr)) level.pendingUpload = {};
   return hr;
}

HRESULT prepare_texture(Surface *surface)
{
   if (surface->lockedLevels) return D3DERR_INVALIDCALL;
   if (!surface->needsTextureUpload) return S_OK;
   for (UINT i = 0; i < surface->levels; ++i) {
      HRESULT hr = upload_pending(surface, i);
      if (FAILED(hr)) return hr;
   }
   surface->needsTextureUpload = false;
   return S_OK;
}

HRESULT create_texture(LegacyDevice *dev, D3DDDIARG_CREATERESOURCE *args, bool displayable)
{
   D3DDDI_RESOURCEFLAGS allowed{};
   allowed.CpuOptimized = allowed.Texture = allowed.Dynamic = allowed.HintStatic = allowed.WriteOnly = allowed.NotLockable = 1;
   if (displayable || (args->Flags.Value & ~allowed.Value) ||
       args->MultisampleType != D3DDDIMULTISAMPLE_NONE || args->MultisampleQuality)
      return D3DERR_NOTAVAILABLE;
   const bool system = args->Pool == D3DDDIPOOL_SYSTEMMEM || args->Pool == D3DDDIPOOL_STAGINGMEM;
   if (!system && args->Pool != D3DDDIPOOL_VIDEOMEMORY && args->Pool != D3DDDIPOOL_LOCALVIDMEM &&
       args->Pool != D3DDDIPOOL_NONLOCALVIDMEM)
      return D3DERR_NOTAVAILABLE;
   DXGI_FORMAT format;
   switch (args->Format) {
   case D3DDDIFMT_A8R8G8B8: format = DXGI_FORMAT_B8G8R8A8_UNORM; break;
   case D3DDDIFMT_X8R8G8B8: format = DXGI_FORMAT_B8G8R8X8_UNORM; break;
   default: return D3DERR_NOTAVAILABLE;
   }
   const UINT width = args->pSurfList[0].Width, height = args->pSurfList[0].Height;
   const UINT limit = dev->slot.state->caps->maxImageDimension2D;
   if (!width || !height || width > limit || height > limit || width > UINT_MAX / 4 || height > INT_MAX)
      return E_INVALIDARG;
   UINT maxLevels = 1;
   for (UINT size = std::max(width, height); size > 1; size >>= 1)
      ++maxLevels;
   if (!args->MipLevels || args->MipLevels != args->SurfCount || args->MipLevels > maxLevels)
      return E_INVALIDARG;

   dev->begin();
   if (FAILED(dev->error)) return dev->error;
   std::unique_ptr<Surface> surface(new (std::nothrow) Surface{});
   if (!surface) return E_OUTOFMEMORY;
   surface->owner = dev;
   surface->width = width;
   surface->height = height;
   surface->levels = args->MipLevels;
   surface->format = format;
   surface->lockable = !args->Flags.NotLockable;
   surface->system = system;
   surface->texture.reset(new (std::nothrow) TextureLevel[surface->levels]());
   if (!surface->texture) return E_OUTOFMEMORY;

   std::array<D3D10DDI_MIPINFO, 32> mips{};
   std::array<D3D10_DDIARG_SUBRESOURCE_UP, 32> initial{};
   for (UINT i = 0; i < surface->levels; ++i) {
      const auto &src = args->pSurfList[i];
      auto &level = surface->texture[i];
      level.width = std::max(1u, width >> i);
      level.height = std::max(1u, height >> i);
      level.pitch = level.width * 4;
      if (src.Width != level.width || src.Height != level.height || src.Depth > 1 ||
          (src.pSysMem && src.SysMemPitch < level.pitch))
         return E_INVALIDARG;
      const uint64_t size = uint64_t(level.pitch) * level.height;
      if (size > SIZE_MAX) return E_OUTOFMEMORY;
      if (src.pSysMem) {
         const uint64_t last = uint64_t(src.SysMemPitch) * (level.height - 1) + level.pitch - 1;
         if (last > UINTPTR_MAX - reinterpret_cast<uintptr_t>(src.pSysMem)) return E_INVALIDARG;
      }
      if (args->Pool == D3DDDIPOOL_SYSTEMMEM && src.pSysMem) {
         level.pitch = src.SysMemPitch;
         level.bytes = static_cast<BYTE *>(const_cast<void *>(src.pSysMem));
      } else {
         level.owned.reset(new (std::nothrow) BYTE[size_t(size)]{});
         if (!level.owned) return E_OUTOFMEMORY;
         level.bytes = level.owned.get();
         if (src.pSysMem) {
            const auto *input = static_cast<const BYTE *>(src.pSysMem);
            for (UINT y = 0; y < level.height; ++y)
               std::memcpy(level.bytes + size_t(y) * level.pitch, input + size_t(y) * src.SysMemPitch, level.pitch);
         }
      }
      mips[i] = {level.width, level.height, 1, level.width, level.height, 1};
      initial[i] = {level.bytes, level.pitch, 0};
   }
   if (!system) {
      D3D10DDIARG_CREATERESOURCE desc{};
      desc.ResourceDimension = D3D10DDIRESOURCE_TEXTURE2D;
      desc.Format = format;
      desc.Usage = D3D10_DDI_USAGE_DEFAULT;
      desc.BindFlags = D3D10_DDI_BIND_SHADER_RESOURCE;
      desc.SampleDesc.Count = desc.ArraySize = 1;
      desc.MipLevels = surface->levels;
      desc.pMipInfoList = mips.data();
      desc.pInitialDataUP = initial.data();
      dev->graphics.pfnCreateResource(dev->handle(), &desc, surface->resource(), {});
      if (FAILED(dev->error)) return dev->error;
      D3D10DDIARG_CREATESHADERRESOURCEVIEW srv{};
      srv.hDrvResource = surface->resource();
      srv.Format = format;
      srv.ResourceDimension = D3D10DDIRESOURCE_TEXTURE2D;
      srv.Tex2D.MipLevels = surface->levels;
      srv.Tex2D.ArraySize = 1;
      dev->graphics.pfnCreateShaderResourceView(dev->handle(), &srv, {surface->view}, {});
      if (FAILED(dev->error)) {
         dev->graphics.pfnDestroyResource(dev->handle(), surface->resource());
         return dev->error;
      }
   }
   surface->next = dev->surfaces;
   dev->surfaces = surface.get();
   args->hResource = surface.release();
   return S_OK;
}

HRESULT APIENTRY update_subresource(HANDLE handle, const D3DDDIARG_UPDATESUBRESOURCEUP *args)
{
   auto *dev = static_cast<LegacyDevice *>(handle);
   if (!dev || !args || !args->hResource) return E_INVALIDARG;
   D3DDDIARG_COPYFLAGS allowed{};
   allowed.BoxValid = allowed.Discard = allowed.NoOverwrite = 1;
   if ((args->Flags.Value & ~allowed.Value) || (args->Flags.Discard && args->Flags.NoOverwrite))
      return E_INVALIDARG;
   auto *surface = static_cast<Surface *>(args->hResource);
   if (surface->owner != dev || surface->locked) return D3DERR_INVALIDCALL;
   if (surface->isDepth()) return D3DERR_NOTAVAILABLE;
   const UINT sub = args->SubResourceIndex;
   if (sub >= (surface->texture ? surface->levels : 1)) return E_INVALIDARG;
   auto *level = surface->texture ? &surface->texture[sub] : nullptr;
   if (level && level->locked) return D3DERR_INVALIDCALL;
   dev->begin();
   if (FAILED(dev->error)) return dev->error;

   const bool buffer = surface->isBuffer();
   const UINT width = level ? level->width : surface->width;
   const UINT height = level ? level->height : surface->height;
   const D3DDDIBOX full{0, 0, width, height, 0, 1};
   const auto &box = args->Flags.BoxValid ? args->DstBox : full;
   if (box.Left > box.Right || box.Right > width || box.Top > box.Bottom || box.Bottom > height ||
       box.Front > box.Back || box.Back > 1) return E_INVALIDARG;
   if (box.Left == box.Right || box.Top == box.Bottom || box.Front == box.Back) return S_OK;
   if (!args->pSysMemUP) return E_INVALIDARG;

   const UINT rowBytes = (box.Right - box.Left) * (buffer ? 1 : 4);
   const UINT rows = box.Bottom - box.Top;
   UINT srcPitch = rows == 1 ? rowBytes : args->RowPitch;
   if (srcPitch < rowBytes) return E_INVALIDARG;
   const uint64_t srcBytes = uint64_t(rows - 1) * srcPitch + rowBytes;
   const BYTE *src = static_cast<const BYTE *>(args->pSysMemUP);
   const uintptr_t srcAddress = reinterpret_cast<uintptr_t>(src);
   if (srcBytes - 1 > UINTPTR_MAX - srcAddress) return E_INVALIDARG;
   if (buffer) {
      std::memmove(surface->bufferData() + box.Left, src, rowBytes);
      return S_OK;
   }

   BYTE *dst = level ? level->bytes : surface->external;
   const UINT dstPitch = level ? level->pitch : surface->externalPitch;
   std::unique_ptr<BYTE[]> snapshot;
   if (dst) {
      dst += size_t(box.Top) * dstPitch + size_t(box.Left) * 4;
      const uintptr_t dstAddress = reinterpret_cast<uintptr_t>(dst);
      const uint64_t dstBytes = uint64_t(rows - 1) * dstPitch + rowBytes;
      // External SYSTEMMEM surfaces may be initialized from overlapping rows
      // with a different pitch. Snapshot before issuing any GPU or CPU write.
      if (srcAddress <= dstAddress + (dstBytes - 1) && dstAddress <= srcAddress + (srcBytes - 1)) {
         if (uint64_t(rows) * rowBytes > SIZE_MAX) return E_OUTOFMEMORY;
         snapshot.reset(new (std::nothrow) BYTE[size_t(rows) * rowBytes]);
         if (!snapshot) return E_OUTOFMEMORY;
         for (UINT y = 0; y < rows; ++y)
            std::memcpy(snapshot.get() + size_t(y) * rowBytes, src + size_t(y) * srcPitch, rowBytes);
         src = snapshot.get();
         srcPitch = rowBytes;
      }
   }
   // Standalone staging surfaces have a D3D10 resource. System-memory
   // texture levels are CPU-only. Commit a CPU mirror after GPU acceptance.
   if (!level || !surface->system) {
      const D3D10_DDI_BOX dest{LONG(box.Left), LONG(box.Top), 0, LONG(box.Right), LONG(box.Bottom), 1};
      dev->graphics.pfnResourceUpdateSubresourceUP(dev->handle(), surface->resource(), sub, &dest,
                                                  src, srcPitch, 0);
      if (FAILED(dev->error)) return dev->error;
      surface->shadowCurrent = false;
   }
   if (dst)
      for (UINT y = 0; y < rows; ++y)
         std::memcpy(dst + size_t(y) * dstPitch, src + size_t(y) * srcPitch, rowBytes);
   return S_OK;
}

HRESULT APIENTRY texture_blt(HANDLE h, const D3DDDIARG_TEXBLT1 *args)
{
   auto *dev = static_cast<LegacyDevice *>(h);
   if (!dev || !args || !args->hSrcResource || !args->hDstResource || args->CopyFlags > D3DDDI_COPY_DISCARD)
      return E_INVALIDARG;
   auto *src = static_cast<Surface *>(args->hSrcResource);
   auto *dst = static_cast<Surface *>(args->hDstResource);
   if (src->owner != dev || dst->owner != dev || !src->texture || !dst->texture || dst->system ||
       src == dst || src->format != dst->format || args->CubeMapFace) return D3DERR_INVALIDCALL;
   if (FAILED(dev->slot.state->lost)) return dev->slot.state->lost;
   UINT first = 0;
   while (first < src->levels && (src->texture[first].width != dst->width ||
                                  src->texture[first].height != dst->height))
      ++first;
   if (first == src->levels) return D3DERR_INVALIDCALL;
   RECT area = args->SrcRect;
   if (area.left < 0 || area.top < 0 || area.left >= area.right || area.top >= area.bottom ||
       UINT(area.right) > src->width || UINT(area.bottom) > src->height ||
       args->DstPoint.x < 0 || args->DstPoint.y < 0) return E_INVALIDARG;
   // Each nonempty source region contributes at least its last texel in
   // smaller NPOT mips, even when the last row or column rounded away.
   auto sourceRect = [&](UINT mip) {
      const auto &level = src->texture[mip];
      const UINT right = std::min(level.width, 1u + ((UINT(area.right) - 1) >> mip));
      const UINT bottom = std::min(level.height, 1u + ((UINT(area.bottom) - 1) >> mip));
      return RECT{LONG(std::min(UINT(area.left) >> mip, right - 1)),
                  LONG(std::min(UINT(area.top) >> mip, bottom - 1)), LONG(right), LONG(bottom)};
   };
   const auto top = sourceRect(first);
   const UINT startX = UINT(args->DstPoint.x), startY = UINT(args->DstPoint.y);
   if (startX >= dst->width || startY >= dst->height ||
       UINT(top.right - top.left) > dst->width - startX || UINT(top.bottom - top.top) > dst->height - startY)
      return D3DERR_INVALIDCALL;

   const UINT count = std::min(src->levels - first, dst->levels);
   for (UINT i = 0; i < count; ++i)
      if (src->texture[first + i].locked || dst->texture[i].locked) return D3DERR_INVALIDCALL;
   dev->begin();
   for (UINT i = 0; i < count; ++i) {
      const auto &from = src->texture[first + i];
      const auto &to = dst->texture[i];
      const RECT r = sourceRect(first + i);
      const UINT x = std::min(startX >> i, to.width - 1);
      const UINT y = std::min(startY >> i, to.height - 1);
      // Independently rounded source and destination origins can leave one
      // fewer texel at the destination edge than in the projected source.
      const UINT width = std::min(UINT(r.right - r.left), to.width - x);
      const UINT height = std::min(UINT(r.bottom - r.top), to.height - y);
      D3DDDIARG_UPDATESUBRESOURCEUP update{};
      update.hResource = dst;
      update.SubResourceIndex = i;
      update.DstBox = {x, y, x + width, y + height, 0, 1};
      update.Flags.BoxValid = 1;
      update.pSysMemUP = from.bytes + size_t(r.top) * from.pitch + size_t(r.left) * 4;
      update.RowPitch = from.pitch;
      HRESULT hr = update_subresource(dev, &update);
      if (FAILED(hr)) return hr;
   }
   return S_OK;
}

HRESULT lock_texture(Surface *surface, D3DDDIARG_LOCK *args)
{
   if (!surface->lockable || args->SubResourceIndex >= surface->levels) return D3DERR_INVALIDCALL;
   if (FAILED(surface->owner->slot.state->lost)) return surface->owner->slot.state->lost;
   auto &level = surface->texture[args->SubResourceIndex];
   if (level.locked) return D3DERR_INVALIDCALL;
   D3DDDI_LOCKFLAGS allowed{};
   allowed.AreaValid = allowed.ReadOnly = allowed.WriteOnly = allowed.Discard = allowed.DoNotWait = 1;
   allowed.NotifyOnly = surface->system && !level.owned;
   if ((args->Flags.Value & ~allowed.Value) || (args->Flags.ReadOnly && (args->Flags.Discard || args->Flags.WriteOnly)))
      return E_INVALIDARG;
   const RECT area = args->Flags.AreaValid ? args->Area : RECT{0, 0, LONG(level.width), LONG(level.height)};
   if (area.left < 0 || area.top < 0 || area.right <= area.left || area.bottom <= area.top ||
       UINT(area.right) > level.width || UINT(area.bottom) > level.height)
      return E_INVALIDARG;
   args->Pitch = level.pitch;
   args->SlicePitch = 0;
   args->pSurfData = level.bytes + size_t(area.top) * level.pitch + size_t(area.left) * 4;
   level.area = area;
   level.write = !args->Flags.ReadOnly;
   level.locked = true;
   ++surface->lockedLevels;
   return S_OK;
}

HRESULT unlock_texture(Surface *surface, const D3DDDIARG_UNLOCK *args)
{
   if (args->SubResourceIndex >= surface->levels) return E_INVALIDARG;
   auto &level = surface->texture[args->SubResourceIndex];
   D3DDDI_UNLOCKFLAGS allowed{};
   allowed.NotifyOnly = surface->system && !level.owned;
   if (!level.locked || (args->Flags.Value & ~allowed.Value)) return E_INVALIDARG;
   surface->owner->begin();
   level.locked = false;
   --surface->lockedLevels;
   if (surface->system || !level.write || FAILED(surface->owner->error)) return surface->owner->error;
   RECT dirty{};
   UnionRect(&dirty, &level.pendingUpload, &level.area);
   level.pendingUpload = dirty;
   const HRESULT hr = upload_pending(surface, args->SubResourceIndex);
   if (FAILED(hr)) surface->needsTextureUpload = true;
   return hr;
}
} // namespace tgd3d::legacy9
