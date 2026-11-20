/* SPDX-License-Identifier: MIT */
#include "tgd3d_legacy9.h"
#include <cstddef>
#include <cstring>
#include <memory>

using namespace tgd3d;
using namespace tgd3d::legacy9;

namespace {

void APIENTRY set_error(D3D10DDI_HRTCORELAYER core, HRESULT hr)
{
   auto *dev = static_cast<LegacyDevice *>(core.handle);
   if (SUCCEEDED(dev->error)) dev->error = hr;
}

bool valid_rect(const Surface *surface, const RECT &r)
{
   return r.left >= 0 && r.top >= 0 && r.right > r.left && r.bottom > r.top &&
          static_cast<UINT>(r.right) <= surface->width && static_cast<UINT>(r.bottom) <= surface->height;
}

HRESULT allocate(Surface *res, D3D10DDI_HRESOURCE storage, HANDLE runtime, bool staging,
                 const D3D10_DDIARG_SUBRESOURCE_UP *initial, bool shared = false, bool displayable = false)
{
   LegacyDevice *dev = res->owner;
   D3D10DDI_MIPINFO mip = {res->width, res->height, 1, res->width, res->height, 1};
   D3D10DDIARG_CREATERESOURCE desc{};
   desc.pMipInfoList = &mip;
   desc.pInitialDataUP = initial;
   desc.ResourceDimension = D3D10DDIRESOURCE_TEXTURE2D;
   desc.Usage = staging ? D3D10_DDI_USAGE_STAGING : D3D10_DDI_USAGE_DEFAULT;
   desc.BindFlags = !staging && res->target ? D3D10_DDI_BIND_RENDER_TARGET : 0;
   if (!staging && (shared || displayable || (res->target && runtime))) {
      // A standalone D3D9 render target may become the source of a windowed
      // Present. Keep its real allocation associated with the runtime handle.
      // The runtime controls whether that resource can be opened by another process.
      desc.MiscFlags = displayable ? D3DWDDM2_0DDI_RESOURCE_MISC_DISPLAYABLE_SURFACE : D3D10_DDI_RESOURCE_MISC_SHARED;
      if (res->target) desc.BindFlags |= D3D10_DDI_BIND_PRESENT;
   }
   desc.MapFlags = staging ? D3D10_DDI_CPU_ACCESS_READ | D3D10_DDI_CPU_ACCESS_WRITE : 0;
   desc.Format = res->format;
   desc.SampleDesc.Count = desc.MipLevels = desc.ArraySize = 1;
   dev->graphics.pfnCreateResource(dev->handle(), &desc, storage, {runtime});
   return dev->error;
}

HRESULT finish_surface(std::unique_ptr<Surface> &res, HANDLE &output)
{
   LegacyDevice *dev = res->owner;
   if (res->target) {
      D3D10DDIARG_CREATERENDERTARGETVIEW view{};
      view.hDrvResource = res->resource();
      view.Format = res->format;
      view.ResourceDimension = D3D10DDIRESOURCE_TEXTURE2D;
      view.Tex2D.ArraySize = 1;
      dev->graphics.pfnCreateRenderTargetView(dev->handle(), &view, res->rtv(), {});
      if (FAILED(dev->error)) {
         HRESULT hr = dev->error;
         dev->graphics.pfnDestroyResource(dev->handle(), res->resource());
         return hr;
      }
   }
   res->next = dev->surfaces;
   dev->surfaces = res.get();
   output = res.release();
   return S_OK;
}

// The transformed-vertex path consumes the source bytes on the CPU and uploads
// the converted vertices for each draw. No submitted command retains this store.
HRESULT create_buffer(LegacyDevice *dev, D3DDDIARG_CREATERESOURCE *args, bool displayable)
{
   const auto &desc = args->pSurfList[0];
   UINT indexSize = 0;
   if (args->Flags.IndexBuffer) {
      if (args->Flags.VertexBuffer || (args->Format != D3DDDIFMT_INDEX16 && args->Format != D3DDDIFMT_INDEX32))
         return E_INVALIDARG;
      indexSize = args->Format == D3DDDIFMT_INDEX16 ? 2 : 4;
   } else if (!args->Flags.VertexBuffer || args->Format != D3DDDIFMT_VERTEXDATA) {
      return E_INVALIDARG;
   }
   if (!desc.Width || desc.Height > 1 || desc.Depth > 1 || args->SurfCount != 1 || args->MipLevels > 1)
      return E_INVALIDARG;
   D3DDDI_RESOURCEFLAGS supported{};
   supported.IndexBuffer = supported.VertexBuffer = supported.Dynamic = supported.HintStatic = supported.WriteOnly = 1;
   supported.CpuOptimized = supported.MightDrawFromLocked = supported.NotLockable = 1;
   if (displayable || (args->Flags.Value & ~supported.Value) ||
       args->MultisampleType != D3DDDIMULTISAMPLE_NONE || args->MultisampleQuality)
      return D3DERR_NOTAVAILABLE;
   const bool system = args->Pool == D3DDDIPOOL_SYSTEMMEM || args->Pool == D3DDDIPOOL_STAGINGMEM;
   if (!system && args->Pool != D3DDDIPOOL_VIDEOMEMORY && args->Pool != D3DDDIPOOL_LOCALVIDMEM &&
       args->Pool != D3DDDIPOOL_NONLOCALVIDMEM) return D3DERR_NOTAVAILABLE;
   if (desc.pSysMem && desc.Width > UINTPTR_MAX - reinterpret_cast<uintptr_t>(desc.pSysMem))
      return E_INVALIDARG;
   dev->begin();
   if (FAILED(dev->error)) return dev->error;
   std::unique_ptr<Surface> res(new (std::nothrow) Surface{});
   if (!res) return E_OUTOFMEMORY;
   res->owner = dev;
   res->format = DXGI_FORMAT_UNKNOWN;
   res->indexSize = indexSize;
   res->width = desc.Width;
   res->height = 1;
   res->system = system;
   res->lockable = !args->Flags.NotLockable;
   res->mightDrawLocked = args->Flags.MightDrawFromLocked;
   if (args->Pool == D3DDDIPOOL_SYSTEMMEM && desc.pSysMem) {
      res->external = static_cast<BYTE *>(const_cast<void *>(desc.pSysMem));
   } else {
      res->ownedBytes.reset(new (std::nothrow) BYTE[desc.Width]());
      if (!res->ownedBytes) return E_OUTOFMEMORY;
      if (desc.pSysMem) std::memcpy(res->ownedBytes.get(), desc.pSysMem, desc.Width);
   }
   return finish_surface(res, args->hResource);
}

HRESULT create_surface(HANDLE h, D3DDDIARG_CREATERESOURCE *args, bool displayable)
{
   if (!h || !args || !args->pSurfList || !args->SurfCount) return E_INVALIDARG;
   auto *dev = static_cast<LegacyDevice *>(h);
   if (args->Flags.Texture)
      return create_texture(dev, args, displayable);
   if (args->Flags.VertexBuffer || args->Flags.IndexBuffer || args->Format == D3DDDIFMT_VERTEXDATA ||
       args->Format == D3DDDIFMT_INDEX16 || args->Format == D3DDDIFMT_INDEX32)
      return create_buffer(dev, args, displayable);
   const auto &first = args->pSurfList[0];
   const UINT limit = dev->slot.state->caps->maxImageDimension2D;
   if (!first.Width || !first.Height || first.Width > limit || first.Height > limit ||
       first.Width > UINT_MAX / 4 || first.Width > INT_MAX || first.Height > INT_MAX)
      return E_INVALIDARG;
   if (args->SurfCount != 1 || args->MipLevels > 1 || first.Depth > 1 ||
       args->MultisampleType != D3DDDIMULTISAMPLE_NONE || args->MultisampleQuality)
      return D3DERR_NOTAVAILABLE;
   D3DDDI_RESOURCEFLAGS supported{};
   supported.RenderTarget = supported.HintStatic = supported.WriteOnly = supported.NotLockable = 1;
   supported.DiscardRenderTarget = supported.CpuOptimized = 1;
   supported.SharedResource = 1;
   if (args->Flags.Value & ~supported.Value) return D3DERR_NOTAVAILABLE;
   DXGI_FORMAT format;
   switch (args->Format) {
   case D3DDDIFMT_X8R8G8B8: format = DXGI_FORMAT_B8G8R8X8_UNORM; break;
   case D3DDDIFMT_A8R8G8B8: format = DXGI_FORMAT_B8G8R8A8_UNORM; break;
   default: return D3DERR_NOTAVAILABLE;
   }
   const bool system = args->Pool == D3DDDIPOOL_SYSTEMMEM || args->Pool == D3DDDIPOOL_STAGINGMEM;
   if ((!system && args->Pool != D3DDDIPOOL_VIDEOMEMORY && args->Pool != D3DDDIPOOL_LOCALVIDMEM &&
        args->Pool != D3DDDIPOOL_NONLOCALVIDMEM) || (system && (args->Flags.RenderTarget || args->Flags.SharedResource || displayable)))
      return D3DERR_NOTAVAILABLE;
   if ((args->Flags.SharedResource || displayable) && !args->hResource) return E_INVALIDARG;
   if (first.pSysMem && (first.SysMemPitch < first.Width * 4 ||
       uint64_t(first.SysMemPitch) * (first.Height - 1) + first.Width * 4 >
          UINTPTR_MAX - reinterpret_cast<uintptr_t>(first.pSysMem))) return E_INVALIDARG;
   std::unique_ptr<Surface> res(new (std::nothrow) Surface{});
   if (!res) return E_OUTOFMEMORY;
   res->owner = dev;
   res->width = first.Width;
   res->height = first.Height;
   res->format = format;
   res->system = system;
   res->target = args->Flags.RenderTarget != 0;
   res->externallyWritable = args->Flags.SharedResource || displayable;
   res->lockable = !args->Flags.NotLockable;
   if (args->Pool == D3DDDIPOOL_SYSTEMMEM && first.pSysMem) {
      res->external = static_cast<BYTE *>(const_cast<void *>(first.pSysMem));
      res->externalPitch = first.SysMemPitch;
   }
   D3D10_DDIARG_SUBRESOURCE_UP initial = {const_cast<void *>(first.pSysMem), first.SysMemPitch, first.SysMemSlicePitch};
   dev->begin();
   if (FAILED(dev->error)) return dev->error;
   HRESULT hr = allocate(res.get(), res->resource(), args->hResource, system, first.pSysMem ? &initial : nullptr,
                         args->Flags.SharedResource, displayable);
   if (FAILED(hr)) return hr;
   return finish_surface(res, args->hResource);
}

HRESULT APIENTRY create_resource2(HANDLE h, D3DDDIARG_CREATERESOURCE2 *args)
{
   if (!args) return E_INVALIDARG;
   D3DDDI_RESOURCEFLAGS2 supported{};
   supported.IsDisplayable = 1;
   if (args->Flags.CaptureBuffer) return E_INVALIDARG;
   if (args->Flags2.Value & ~supported.Value) return D3DERR_NOTAVAILABLE;
   D3DDDIARG_CREATERESOURCE common{};
   common.Format=args->Format; common.Pool=args->Pool; common.MultisampleType=args->MultisampleType;
   common.MultisampleQuality=args->MultisampleQuality; common.pSurfList=args->pSurfList;
   common.SurfCount=args->SurfCount; common.MipLevels=args->MipLevels; common.Fvf=args->Fvf;
   common.VidPnSourceId=args->VidPnSourceId; common.RefreshRate=args->RefreshRate;
   common.hResource=args->hResource; common.Flags=args->Flags; common.Rotation=args->Rotation;
   HRESULT hr=create_surface(h,&common,args->Flags2.IsDisplayable);
   if (SUCCEEDED(hr)) args->hResource=common.hResource;
   return hr;
}

HRESULT APIENTRY open_resource(HANDLE h, D3DDDIARG_OPENRESOURCE *args)
{
   if (!h || !args || !args->hResource || !args->hKMResource || !args->pOpenAllocationInfo2) return E_INVALIDARG;
   if (args->NumAllocations != 1 || args->Flags.Value) return D3DERR_NOTAVAILABLE;
   auto *dev = static_cast<LegacyDevice *>(h);
   std::unique_ptr<Surface> res(new (std::nothrow) Surface{});
   if (!res) return E_OUTOFMEMORY;
   res->owner = dev;
   res->externallyWritable = true;
   dev->begin();
   if (FAILED(dev->error)) return dev->error;
   D3D10DDIARG_OPENRESOURCE open{};
   open.NumAllocations = 1;
   open.pOpenAllocationInfo2 = args->pOpenAllocationInfo2;
   open.hKMResource.handle = args->hKMResource;
   open.pPrivateDriverData = args->pPrivateDriverData;
   open.PrivateDriverDataSize = args->PrivateDriverDataSize;
   dev->graphics.pfnOpenResource(dev->handle(), &open, res->resource(), {args->hResource});
   if (FAILED(dev->error)) return dev->error;
   auto *image = R(res->resource());
   if (image->mips != 1 || image->arraySize != 1 ||
       (image->format != DXGI_FORMAT_B8G8R8A8_UNORM && image->format != DXGI_FORMAT_B8G8R8X8_UNORM)) {
      dev->graphics.pfnDestroyResource(dev->handle(), res->resource());
      return D3DERR_NOTAVAILABLE;
   }
   res->width = image->width;
   res->height = image->height;
   res->format = image->format;
   res->target = (image->bind & D3D10_DDI_BIND_RENDER_TARGET) != 0;
   return finish_surface(res, args->hResource);
}

HRESULT APIENTRY set_target(HANDLE h, const D3DDDIARG_SETRENDERTARGET *args)
{
   if (!h || !args || args->SubResourceIndex || args->RenderTargetIndex) return E_INVALIDARG;
   auto *dev = static_cast<LegacyDevice *>(h);
   auto *res = static_cast<Surface *>(args->hRenderTarget);
   if (res && (res->owner != dev || !res->target || res->locked)) return E_INVALIDARG;
   dev->begin();
   D3D10DDI_HRENDERTARGETVIEW view = res ? res->rtv() : D3D10DDI_HRENDERTARGETVIEW{};
   dev->graphics.pfnSetRenderTargets(dev->handle(), &view, 1, 0, {});
   if (SUCCEEDED(dev->error)) {
      dev->target = res;
      draw_target_changed(dev);
   }
   return dev->error;
}

HRESULT color_clear(Surface *surface, D3DCOLOR argb, UINT count, const RECT *rects)
{
   if (!surface || !surface->target || surface->locked || (count && !rects)) return E_INVALIDARG;
   LegacyDevice *dev = surface->owner;
   FLOAT color[] = {FLOAT((argb >> 16) & 255) / 255, FLOAT((argb >> 8) & 255) / 255,
                    FLOAT(argb & 255) / 255, FLOAT(argb >> 24) / 255};
   dev->begin();
   if (FAILED(dev->error)) return dev->error;
   if (!count) {
      dev->graphics.pfnClearRenderTargetView(dev->handle(), surface->rtv(), color);
   } else {
      for (UINT i = 0; i < count; ++i) {
         D3D10_DDI_RECT rect = {rects[i].left, rects[i].top, rects[i].right, rects[i].bottom};
         ClearView11(dev->handle(), D3D10DDI_HT_RENDERTARGETVIEW, surface->view, color, &rect, 1);
         if (FAILED(dev->error)) break;
      }
   }
   surface->shadowCurrent = false;
   return dev->error;
}

HRESULT APIENTRY clear(HANDLE h, const D3DDDIARG_CLEAR *args, UINT count, const RECT *rects)
{
   if (!h || !args) return E_INVALIDARG;
   // D3DCLEAR_COMPUTERECTS from d3dhal.h is absent in the MinGW DDI headers.
   constexpr DWORD computeRects = 0x00000008;
   if ((args->Flags & ~computeRects) != D3DCLEAR_TARGET) return D3DERR_NOTAVAILABLE;
   auto *dev = static_cast<LegacyDevice *>(h);
   if (!dev->target || dev->target->locked || (count && !rects)) return E_INVALIDARG;
   if (FAILED(dev->slot.state->lost)) return dev->slot.state->lost;
   for (UINT i = 0; i < count; ++i)
      if (rects[i].left > rects[i].right || rects[i].top > rects[i].bottom) return E_INVALIDARG;
   if (!(args->Flags & computeRects))
      return count ? color_clear(dev->target, args->FillColor, count, rects) : S_OK;

   const RECT bounds = clear_bounds(dev);
   if (IsRectEmpty(&bounds)) return S_OK;
   if (!count) {
      const RECT target = dev->target->bounds();
      if (EqualRect(&bounds, &target))
         return color_clear(dev->target, args->FillColor, 0, nullptr);
      return color_clear(dev->target, args->FillColor, 1, &bounds);
   }
   for (UINT i = 0; i < count; ++i) {
      RECT clipped;
      if (IntersectRect(&clipped, &bounds, &rects[i])) {
         HRESULT hr = color_clear(dev->target, args->FillColor, 1, &clipped);
         if (FAILED(hr)) return hr;
      }
   }
   return S_OK;
}

HRESULT APIENTRY color_fill(HANDLE h, const D3DDDIARG_COLORFILL *args)
{
   if (!h || !args || !args->hResource || args->SubResourceIndex) return E_INVALIDARG;
   if (args->Flags.Value) return D3DERR_NOTAVAILABLE;
   auto *res = static_cast<Surface *>(args->hResource);
   if (res->owner != h) return E_INVALIDARG;
   return color_clear(res, args->Color, 1, &args->DstRect);
}

HRESULT external_rows(Surface *res, const RECT &r, bool to_external)
{
   LegacyDevice *dev = res->owner;
   D3D10DDI_MAPPED_SUBRESOURCE map{};
   dev->graphics.pfnResourceMap(dev->handle(), res->resource(), 0,
      to_external ? D3D10_DDI_MAP_READ : D3D10_DDI_MAP_WRITE, 0, &map);
   if (FAILED(dev->error)) return dev->error;
   const size_t width = size_t(r.right - r.left) * 4;
   for (LONG y = r.top; y < r.bottom; ++y) {
      BYTE *external = res->external + size_t(y) * res->externalPitch + size_t(r.left) * 4;
      BYTE *buffer = static_cast<BYTE *>(map.pData) + size_t(y) * map.RowPitch + size_t(r.left) * 4;
      if (to_external) std::memcpy(external, buffer, width);
      else std::memcpy(buffer, external, width);
   }
   dev->graphics.pfnResourceUnmap(dev->handle(), res->resource(), 0);
   return dev->error;
}

HRESULT APIENTRY blt(HANDLE h, const D3DDDIARG_BLT *args)
{
   if (!h || !args || !args->hSrcResource || !args->hDstResource ||
       args->SrcSubResourceIndex || args->DstSubResourceIndex) return E_INVALIDARG;
   auto *dev = static_cast<LegacyDevice *>(h);
   auto *src = static_cast<Surface *>(args->hSrcResource);
   auto *dst = static_cast<Surface *>(args->hDstResource);
   if (src->texture || dst->texture || src->isBuffer() || dst->isBuffer()) return D3DERR_NOTAVAILABLE;
   const RECT &a = args->SrcRect, &b = args->DstRect;
   if (src->owner != dev || dst->owner != dev || src->locked || dst->locked ||
       !valid_rect(src, a) || !valid_rect(dst, b)) return E_INVALIDARG;
   D3DDDI_BLTFLAGS allowed{}; allowed.Point = 1;
   if ((args->Flags.Value & ~allowed.Value) || src->format != dst->format || src == dst ||
       a.right-a.left != b.right-b.left || a.bottom-a.top != b.bottom-b.top) return D3DERR_NOTAVAILABLE;
   dev->begin();
   if (FAILED(dev->error)) return dev->error;
   if (src->external && FAILED(external_rows(src, a, false))) return dev->error;
   D3D10_DDI_BOX box = {a.left, a.top, 0, a.right, a.bottom, 1};
   dev->graphics.pfnResourceCopyRegion(dev->handle(),dst->resource(),0,b.left,b.top,0,src->resource(),0,&box);
   dst->shadowCurrent = false;
   if (SUCCEEDED(dev->error) && dst->external) return external_rows(dst,b,true);
   return dev->error;
}

HRESULT APIENTRY lock_surface(HANDLE h, D3DDDIARG_LOCK *args)
{
   if (!h || !args || !args->hResource) return E_INVALIDARG;
   auto *dev = static_cast<LegacyDevice *>(h);
   auto *res = static_cast<Surface *>(args->hResource);
   if (res->owner == dev && res->texture) return lock_texture(res, args);
   if (args->SubResourceIndex) return E_INVALIDARG;
   if (res->owner != dev || res->locked) return E_INVALIDARG;
   if (!res->lockable) return D3DERR_INVALIDCALL;
   if (res->isBuffer()) {
      D3DDDI_LOCKFLAGS allowed{};
      allowed.RangeValid = allowed.ReadOnly = allowed.WriteOnly = allowed.Discard = allowed.NoOverwrite = 1;
      allowed.MightDrawFromLocked = allowed.NotifyOnly = allowed.DoNotWait = 1;
      if ((args->Flags.Value & ~allowed.Value) || (args->Flags.NotifyOnly && !res->external) ||
          (args->Flags.MightDrawFromLocked && !res->mightDrawLocked) ||
          (args->Flags.ReadOnly && (args->Flags.WriteOnly || args->Flags.Discard || args->Flags.NoOverwrite)) ||
          (args->Flags.Discard && args->Flags.NoOverwrite)) return E_INVALIDARG;
      const UINT start = args->Flags.RangeValid ? args->Range.Offset : 0;
      if (start >= res->width || (args->Flags.RangeValid && args->Range.Size > res->width - start))
         return E_INVALIDARG;
      dev->begin();
      if (FAILED(dev->error)) return dev->error;
      args->pSurfData = res->bufferData() + start;
      args->Pitch = args->SlicePitch = res->width;
      res->locked = true;
      res->allowLockedDraw = args->Flags.MightDrawFromLocked;
      return S_OK;
   }
   D3DDDI_LOCKFLAGS allowed{}; allowed.ReadOnly=allowed.WriteOnly=allowed.Discard=allowed.AreaValid=allowed.DoNotWait=1;
   if ((args->Flags.Value & ~allowed.Value) || (args->Flags.ReadOnly && (args->Flags.WriteOnly || args->Flags.Discard)))
      return D3DERR_NOTAVAILABLE;
   const RECT r = args->Flags.AreaValid ? args->Area : res->bounds();
   if (!valid_rect(res,r)) return E_INVALIDARG;
   dev->begin();
   if (FAILED(dev->error)) return dev->error;
   D3D10DDI_MAPPED_SUBRESOURCE map{};
   if (res->external) {
      map.pData=res->external; map.RowPitch=res->externalPitch;
   } else {
      if (!res->system && !res->hasShadow) {
         HRESULT hr=allocate(res,res->staging(),nullptr,true,nullptr);
         if (FAILED(hr)) return hr;
         res->hasShadow=true;
      }
      const bool read=!args->Flags.WriteOnly && !args->Flags.Discard;
      if (read && !res->system && (!res->shadowCurrent || res->externallyWritable)) {
         dev->graphics.pfnResourceCopy(dev->handle(),res->staging(),res->resource());
         if (FAILED(dev->error)) return dev->error;
         res->shadowCurrent=true;
      }
      const auto mode=args->Flags.ReadOnly ? D3D10_DDI_MAP_READ : read ? D3D10_DDI_MAP_READWRITE : D3D10_DDI_MAP_WRITE;
      dev->graphics.pfnResourceMap(dev->handle(),res->staging(),0,mode,
                                  args->Flags.DoNotWait ? D3D10_DDI_MAP_FLAG_DONOTWAIT : 0,&map);
      if (FAILED(dev->error)) return dev->error==DXGI_ERROR_WAS_STILL_DRAWING ? D3DERR_WASSTILLDRAWING : dev->error;
   }
   args->pSurfData=static_cast<BYTE *>(map.pData)+size_t(r.top)*map.RowPitch+size_t(r.left)*4;
   args->Pitch=map.RowPitch; args->SlicePitch=map.DepthPitch;
   res->locked=true; res->write=!args->Flags.ReadOnly; res->lockedArea=r;
   return S_OK;
}

HRESULT APIENTRY unlock_surface(HANDLE h, const D3DDDIARG_UNLOCK *args)
{
   if (!h || !args || !args->hResource) return E_INVALIDARG;
   auto *dev=static_cast<LegacyDevice *>(h);
   auto *res=static_cast<Surface *>(args->hResource);
   if (res->owner == dev && res->texture) return unlock_texture(res, args);
   if (args->SubResourceIndex) return E_INVALIDARG;
   if (res->owner!=dev || !res->locked) return E_INVALIDARG;
   if (res->isBuffer()) {
      D3DDDI_UNLOCKFLAGS allowed{};
      allowed.NotifyOnly = res->external != nullptr;
      if (args->Flags.Value & ~allowed.Value) return E_INVALIDARG;
      res->locked = res->allowLockedDraw = false;
      return dev->slot.state->lost;
   }
   if (args->Flags.Value) return E_INVALIDARG;
   dev->begin();
   if (!res->external) dev->graphics.pfnResourceUnmap(dev->handle(),res->staging(),0);
   if (SUCCEEDED(dev->error) && !res->system && res->write) {
      const RECT &r=res->lockedArea;
      D3D10_DDI_BOX box={r.left,r.top,0,r.right,r.bottom,1};
      dev->graphics.pfnResourceCopyRegion(dev->handle(),res->resource(),0,r.left,r.top,0,res->staging(),0,&box);
      res->shadowCurrent=false;
   }
   res->locked=false;
   return dev->error;
}

HRESULT APIENTRY destroy_resource(HANDLE h, HANDLE resource)
{
   if (!h || !resource) return E_INVALIDARG;
   auto *dev=static_cast<LegacyDevice *>(h);
   auto *res=static_cast<Surface *>(resource);
   if (res->owner!=dev) return E_INVALIDARG;
   dev->begin();
   if (dev->target==res) {
      dev->graphics.pfnSetRenderTargets(dev->handle(),nullptr,0,1,{});
      dev->target=nullptr;
   }
   if (res->target) dev->graphics.pfnDestroyRenderTargetView(dev->handle(),res->rtv());
   if (res->hasShadow) dev->graphics.pfnDestroyResource(dev->handle(),res->staging());
   if (dev->vertexStream == res) dev->vertexStream = nullptr;
   if (dev->indexStream == res) dev->indexStream = nullptr;
   if (dev->sampled == res) dev->sampled = nullptr;
   if (res->texture && !res->system)
      dev->graphics.pfnDestroyShaderResourceView(dev->handle(), {res->view});
   if (!res->isBuffer() && !(res->system && res->texture)) dev->graphics.pfnDestroyResource(dev->handle(),res->resource());
   Surface **prev=&dev->surfaces;
   while (*prev!=res) prev=&(*prev)->next;
   *prev=res->next;
   delete res;
   return dev->error;
}

HRESULT APIENTRY buffer_blt(HANDLE h, const D3DDDIARG_BUFFERBLT1 *args)
{
   if (!h || !args || !args->hDstResource || !args->hSrcResource || args->CopyFlags > D3DDDI_COPY_DISCARD)
      return E_INVALIDARG;
   auto *dev = static_cast<LegacyDevice *>(h);
   auto *src = static_cast<Surface *>(args->hSrcResource);
   auto *dst = static_cast<Surface *>(args->hDstResource);
   if (src->owner != dev || dst->owner != dev || !src->isBuffer() || !dst->isBuffer() ||
       src->locked || dst->locked) return D3DERR_INVALIDCALL;
   const UINT begin = args->SrcRange.Offset, size = args->SrcRange.Size;
   if (begin > src->width || size > src->width - begin ||
       args->Offset > dst->width || size > dst->width - args->Offset) return E_INVALIDARG;
   if (FAILED(dev->slot.state->lost)) return dev->slot.state->lost;
   if (size) std::memmove(dst->bufferData() + args->Offset, src->bufferData() + begin, size);
   return S_OK;
}

HRESULT APIENTRY volume_blt(HANDLE h, const D3DDDIARG_VOLUMEBLT1 *args)
{
   if (!h || !args || !args->hSrcResource || !args->hDstResource) return E_INVALIDARG;
   // Volume textures are rejected at creation and D3DPTEXTURECAPS_VOLUMEMAP is clear.
   return D3DERR_NOTAVAILABLE;
}

HRESULT APIENTRY flush_device(HANDLE h)
{
   if (!h) return E_INVALIDARG;
   return flush(static_cast<LegacyDevice *>(h)->slot.state,true);
}

HRESULT APIENTRY flush1(HANDLE h, UINT flags)
{
   if (!h || (flags & ~UINT(D3DDDI_TRIM_MEMORY))) return E_INVALIDARG;
   HRESULT hr = flush_device(h);
   if (FAILED(hr) || !(flags & D3DDDI_TRIM_MEMORY)) return hr;
   return trim_memory(static_cast<LegacyDevice *>(h)->slot.state);
}

void APIENTRY check_counter_info(HANDLE, D3DDDIARG_COUNTER_INFO *info)
{
   if (info) *info = {};
}

HRESULT APIENTRY check_counter(HANDLE h, D3DDDIQUERYTYPE, D3DDDI_COUNTER_TYPE *, UINT *,
                               LPSTR, UINT *, LPSTR, UINT *, LPSTR, UINT *)
{
   if (!h) return E_INVALIDARG;
   HRESULT hr = static_cast<LegacyDevice *>(h)->slot.state->lost;
   return FAILED(hr) ? hr : D3DERR_NOTAVAILABLE;
}

HRESULT APIENTRY set_marker_mode(HANDLE h, D3DDDI_MARKERTYPE type, UINT flags)
{
   if (!h || (flags & ~D3DDDI_SETMARKERMODE_CUSTOMDRIVEREVENTS)) return E_INVALIDARG;
   HRESULT hr = static_cast<LegacyDevice *>(h)->slot.state->lost;
   if (FAILED(hr)) return hr;
   // The Switch KMD does not provide GPU history buffers. Its matching UMD
   // marker capability is NONE; profile timestamps cannot be generated.
   return type == D3DDDIMT_NONE && flags == 0 ? S_OK : D3DERR_NOTAVAILABLE;
}

HRESULT APIENTRY set_marker(HANDLE h)
{
   if (!h) return E_INVALIDARG;
   return static_cast<LegacyDevice *>(h)->slot.state->lost;
}

HRESULT APIENTRY present(HANDLE h, const D3DDDIARG_PRESENT *arg)
{
   if (!h || !arg || arg->SrcSubResourceIndex || arg->DstSubResourceIndex) return E_INVALIDARG;
   auto *dev = static_cast<LegacyDevice *>(h);
   Device *device = dev->slot.state;
   if (FAILED(device->lost)) return device->lost;
   D3DDDI_PRESENTFLAGS allowed{};
   allowed.Blt = allowed.Flip = allowed.ColorFill = 1;
   if ((arg->Flags.Value & ~allowed.Value) || (arg->Flags.Blt + arg->Flags.Flip + arg->Flags.ColorFill != 1) ||
       (arg->Flags.Flip && UINT(arg->FlipInterval) > D3DDDI_FLIPINTERVAL_FOUR)) return D3DERR_NOTAVAILABLE;
   auto *src = static_cast<Surface *>(arg->hSrcResource);
   auto *dst = static_cast<Surface *>(arg->hDstResource);
   if (bool(src) == bool(arg->Flags.ColorFill) || (src && src == dst)) return E_INVALIDARG;
   if (!dev->present || !device->hContext) return D3DERR_NOTAVAILABLE;
   D3DDDICB_PRESENT cb{};
   auto resolve = [&](Surface *surface, D3DKMT_HANDLE &allocation) -> HRESULT {
      if (!surface) return S_OK;
      if (surface->owner != dev || surface->locked || surface->texture || surface->isBuffer()) return D3DERR_INVALIDCALL;
      if (surface->system) return D3DERR_NOTAVAILABLE;
      RuntimeResource *owner = R(surface->resource())->storage.runtime;
      if (arg->Flags.Flip && !runtime_is_primary(owner) && !runtime_is_displayable(owner)) return D3DERR_NOTAVAILABLE;
      allocation = runtime_allocation(owner);
      return allocation ? S_OK : D3DERR_NOTAVAILABLE;
   };
   HRESULT hr = resolve(src, cb.hSrcAllocation);
   if (SUCCEEDED(hr)) hr = resolve(dst, cb.hDstAllocation);
   // Flush orders the callback on hContext behind all preceding NVK writes,
   // including clears still pending for this render target.
   if (SUCCEEDED(hr)) hr = flush(device, true);
   if (SUCCEEDED(hr)) {
      cb.hContext = device->hContext;
      hr = dev->present(device->hRTDevice.handle, &cb);
      if (SUCCEEDED(hr) && dst) dst->shadowCurrent = false;
      if (hr == D3DDDIERR_DEVICEREMOVED || hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
         device->lost = hr;
   }
   trace(TGD3D_TRACE_LEGACY9, hr, (UINT64_C(5) << 32) | arg->Flags.Value,
         (uint64_t(cb.hSrcAllocation) << 32) | cb.hDstAllocation);
   return hr;
}

HRESULT APIENTRY present1(HANDLE h, D3DDDIARG_PRESENT1 *arg)
{
   if (!h || !arg || arg->Reserved || (arg->DirtyRects && !arg->pDirtyRects)) return E_INVALIDARG;
   if (arg->SrcResources > 1) return D3DERR_NOTAVAILABLE;
   if ((arg->SrcResources && !arg->phSrcResources) || (!arg->SrcResources && !arg->Flags.ColorFill)) return E_INVALIDARG;
   D3DDDIARG_PRESENT one{};
   if (arg->SrcResources) {
      one.hSrcResource = arg->phSrcResources[0].hResource;
      one.SrcSubResourceIndex = arg->phSrcResources[0].SubResourceIndex;
   }
   const auto *src = static_cast<const Surface *>(one.hSrcResource);
   for (UINT i = 0; i < arg->DirtyRects; ++i) {
      const RECT &r = arg->pDirtyRects[i];
      if (!src || src->owner != h || r.left < 0 || r.top < 0 || r.right < r.left || r.bottom < r.top ||
          UINT(r.right) > src->width || UINT(r.bottom) > src->height) return E_INVALIDARG;
   }
   one.hDstResource = arg->hDstResource;
   one.DstSubResourceIndex = arg->DstSubResourceIndex;
   one.Flags = arg->Flags;
   one.FlipInterval = arg->FlipInterval;
   // The D3D9 runtime retains its dirty rectangles and flip interval. Its
   // WDDM 2.0 present callback takes the resolved allocation handles.
   return present(h, &one);
}

HRESULT APIENTRY destroy_device(HANDLE h)
{
   if (!h) return E_INVALIDARG;
   auto *dev=static_cast<LegacyDevice *>(h);
   destroy_draw_state(dev);
   while (dev->surfaces) destroy_resource(dev,dev->surfaces);
   release_device(&dev->slot);
   delete dev;
   return S_OK;
}
}

HRESULT tgd3d::legacy9_create_device(Adapter *adapter, D3DDDIARG_CREATEDEVICE *args)
{
   if (!adapter || !args || !args->hDevice || !args->pCallbacks || !args->pDeviceFuncs) return E_INVALIDARG;
   if (args->Interface != D3D9_RUNTIME_INTERFACE || args->Version < D3D_UMD_INTERFACE_VERSION_WDDM2_0)
      return D3DERR_NOTAVAILABLE;
   std::unique_ptr<LegacyDevice> legacy(new (std::nothrow) LegacyDevice{});
   if (!legacy) return E_OUTOFMEMORY;
   try { legacy->slot.state=new Device(); } catch (const std::bad_alloc &) { return E_OUTOFMEMORY; }
   Device *dev=legacy->slot.state;
   dev->adapter=adapter; dev->caps=&adapter->vk.caps; dev->nvk=adapter->vk.nvk;
   dev->hRTDevice={args->hDevice}; dev->hRTCoreLayer={legacy.get()}; dev->um.pfnSetErrorCb=set_error;
   dev->interfaceVersion=args->Interface;
   legacy->present=args->pCallbacks->pfnPresentCb;
   runtime_copy_callbacks(&dev->kt,args->pCallbacks);
   HRESULT hr=initialize_graphics_device(dev,dev->nvk,true);
   if (FAILED(hr)) { release_device(&legacy->slot); return hr; }
   resource_fill_table(&legacy->graphics);
   state_fill_table(&legacy->graphics);
   draw_fill_table(&legacy->graphics);
   shader_fill_table(&legacy->graphics);
   auto *table=args->pDeviceFuncs;
   std::memset(table, 0, offsetof(D3DDDI_DEVICEFUNCS, pfnTrimResidencySet) + sizeof(table->pfnTrimResidencySet));
   // WDDM 1.3+ requires the new entry points and null legacy counterparts.
   table->pfnCreateResource2 = create_resource2;
   table->pfnBufBlt1 = buffer_blt;
   table->pfnTexBlt1 = texture_blt;
   table->pfnVolBlt1 = volume_blt;
   table->pfnOpenResource=open_resource; table->pfnPresent=present;
   // Keep the optional WDDM 1.3 callback group complete when exposing Present1.
   // The companion DXGI Present1 is installed by dxgi_fill_presentation13.
   table->pfnPresent1 = present1;
   table->pfnFlush1 = flush1;
   table->pfnUpdateSubresourceUP = update_subresource;
   table->pfnCheckCounterInfo = check_counter_info;
   table->pfnCheckCounter = check_counter;
   table->pfnSetMarkerMode = set_marker_mode;
   table->pfnSetMarker = set_marker;
   table->pfnDestroyResource=destroy_resource; table->pfnSetRenderTarget=set_target;
   table->pfnBlt=blt; table->pfnClear=clear; table->pfnColorFill=color_fill;
   table->pfnLock=lock_surface; table->pfnUnlock=unlock_surface; table->pfnFlush=flush_device;
   table->pfnDestroyDevice=destroy_device;
   install_draw_functions(table);
   args->hDevice=legacy.release();
   return S_OK;
}
