/* SPDX-License-Identifier: MIT
 *
 * DXGI DDI: presentation, display mode, identity rotation and blits.
 *
 * Present submits whatever is recorded, queues the fence wait on the runtime
 * context and hands the allocation to the runtime callback. It does not wait
 * for the GPU: the kernel keeps the flip behind the rendering it depends on,
 * and the scheduled panel flip latches it at vertical blank.
 */
#include "tgd3d.h"

namespace tgd3d {

static inline Device *DD(DXGI_DDI_HDEVICE h) { return reinterpret_cast<Device *>(h); }
static inline Resource *DR(DXGI_DDI_HRESOURCE h) { return reinterpret_cast<Resource *>(h); }

static HRESULT APIENTRY Present(DXGI_DDI_ARG_PRESENT *args)
{
   if (!args || !args->hDevice || !args->hSurfaceToPresent)
      return E_INVALIDARG;
   Device *dev = DD(args->hDevice);
   Resource *source = DR(args->hSurfaceToPresent);
   Resource *destination = args->hDstResource ? DR(args->hDstResource) : nullptr;
   if (source->dev != dev || (destination && destination->dev != dev))
      return E_INVALIDARG;
   /* DXGI selects the presentation model behind pDXGIContext. Windowed
    * shared surfaces arrive as Blt although nothing is copied here: the
    * runtime and kernel own destination selection. */
   const bool flip = args->Flags.Flip;
   HRESULT hr = S_OK;
   if (flip == (bool)args->Flags.Blt || args->Flags.PreferRight || args->Flags.TemporaryMono ||
       args->SrcSubResourceIndex || (destination && args->DstSubResourceIndex) ||
       (unsigned)args->FlipInterval > DXGI_DDI_FLIP_INTERVAL_FOUR ||
       !dev->hContext || !dev->dxgi || !dev->dxgi->pfnPresentCb)
      hr = DXGI_ERROR_UNSUPPORTED;
   RuntimeResource *src = source->storage.runtime;
   RuntimeResource *dst = destination ? destination->storage.runtime : nullptr;
   if (SUCCEEDED(hr) && (!src || (destination && !dst)))
      hr = DXGI_ERROR_UNSUPPORTED;
   if (SUCCEEDED(hr) && flip && (!runtime_is_primary(src) || (dst && !runtime_is_primary(dst))))
      hr = DXGI_ERROR_UNSUPPORTED;
   if (SUCCEEDED(hr) && (!runtime_allocation(src) || (dst && !runtime_allocation(dst))))
      hr = D3DDDIERR_DEVICEREMOVED;
   uint64_t qpc[4] = {qpc_now(), 0, 0, 0};
   if (SUCCEEDED(hr)) {
      hr = flush(dev, false);
      qpc[1] = qpc_now();
   }
   if (SUCCEEDED(hr)) {
      hr = flush(dev, true);
      qpc[2] = qpc_now();
   }
   const uint64_t backpressure = dev->stats.backpressureNs;
   dev->stats.backpressureNs = 0;
   if (SUCCEEDED(hr)) {
      DXGIDDICB_PRESENT present = {};
      present.hSrcAllocation = runtime_allocation(src);
      present.hDstAllocation = dst ? runtime_allocation(dst) : 0;
      present.hContext = dev->hContext;
      present.pDXGIContext = args->pDXGIContext;
      /* The panel path never tears and never runs an immediate-present
       * loop: every present waits for at least one real vertical blank. */
      present.SyncIntervalOverrideValid = TRUE;
      present.SyncIntervalOverride = args->FlipInterval == DXGI_DDI_FLIP_INTERVAL_IMMEDIATE ?
         DXGI_DDI_FLIP_INTERVAL_ONE : args->FlipInterval;
      hr = dev->dxgi->pfnPresentCb(dev->hRTDevice.handle, &present);
      qpc[3] = qpc_now();
      if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == D3DDDIERR_DEVICEREMOVED ||
          hr == DXGI_ERROR_DEVICE_RESET)
         dev->lost = hr;
   }
   const uint64_t completed = dev->completed();
   const uint64_t value[6] = {args->Flags.Value, (uint64_t)args->FlipInterval,
                              dev->submitted - completed, backpressure, 0, 0};
   trace_timing(TGD3D_TIMING_PRESENT, hr, dev->submitted, completed, qpc, value);
   return hr;
}

static HRESULT APIENTRY GetGammaCaps(DXGI_DDI_ARG_GET_GAMMA_CONTROL_CAPS *args)
{
   /* Describes the ramp shape DXGI may hand to the kernel; whether a ramp
    * is applied is decided by the display miniport. */
   DXGI_GAMMA_CONTROL_CAPABILITIES *caps = args->pGammaCapabilities;
   caps->ScaleAndOffsetSupported = FALSE;
   caps->MinConvertedValue = 0.0f;
   caps->MaxConvertedValue = 1.0f;
   caps->NumGammaControlPoints = 17;
   for (UINT i = 0; i < caps->NumGammaControlPoints; i++)
      caps->ControlPointPositions[i] = (float)i / (float)(caps->NumGammaControlPoints - 1);
   return S_OK;
}

static HRESULT APIENTRY SetDisplayMode(DXGI_DDI_ARG_SETDISPLAYMODE *args)
{
   if (!args || !args->hDevice || !args->hResource)
      return E_INVALIDARG;
   Device *dev = DD(args->hDevice);
   Resource *res = DR(args->hResource);
#ifdef TGD3D_RED_PRIMARY_CONTROL
   /* The modeset test must execute even when DXGI never reaches rotated Blt.
    * Wait for the real clear before handing this same allocation to runtime;
    * this diagnostic wait is not a replacement for production GPU fences. */
   HRESULT hr = red_primary_clear(dev, res, args->SubResourceIndex);
   if (SUCCEEDED(hr)) hr = flush(dev, false);
   if (SUCCEEDED(hr)) hr = wait_sequence(dev, dev->submitted, TGD3D_WAIT_SYNC_CONTROL);
   trace(TGD3D_TRACE_RED_PRIMARY, hr,
         UINT64_C(1) << 32 | runtime_allocation(res->storage.runtime), dev->submitted);
#else
   /* SetDisplayModeCb selects a scanout allocation synchronously; it is not
    * queued behind the runtime context's GPU wait like an ordinary Present.
    * Finish the submitted NVK work before handing the primary to runtime. */
   HRESULT hr = flush(dev, false);
   if (SUCCEEDED(hr))
      hr = wait_sequence(dev, dev->submitted, TGD3D_WAIT_MODE_CHANGE);
#endif
   if (SUCCEEDED(hr))
      hr = runtime_set_display_mode(res->storage.runtime, dev, args->SubResourceIndex);
   trace(TGD3D_TRACE_SET_DISPLAY_MODE, hr, runtime_allocation(res->storage.runtime),
         args->SubResourceIndex);
   return hr;
}

static HRESULT APIENTRY SetResourcePriority(DXGI_DDI_ARG_SETRESOURCEPRIORITY *args)
{
   if (!args || !args->hDevice || !args->hResource)
      return E_INVALIDARG;
   Device *dev = DD(args->hDevice);
   Resource *res = DR(args->hResource);
   if (res->dev != dev)
      return E_INVALIDARG;
   const D3DKMT_HANDLE allocation = runtime_allocation(res->storage.runtime);
   /* Private NVK storage holds a residency reference for its entire life. */
   if (!res->storage.runtime)
      return S_OK;
   if (!allocation)
      return D3DDDIERR_DEVICEREMOVED;
   if (!dev->kt.pfnSetPriorityCb)
      return DXGI_ERROR_UNSUPPORTED;
   D3DDDICB_SETPRIORITY priority = {};
   priority.NumAllocations = 1;
   priority.HandleList = &allocation;
   priority.pPriorities = &args->Priority;
   return dev->kt.pfnSetPriorityCb(dev->hRTDevice.handle, &priority);
}

static HRESULT APIENTRY QueryResourceResidency(DXGI_DDI_ARG_QUERYRESOURCERESIDENCY *args)
{
   if (!args || !args->hDevice || (args->Resources && (!args->pResources || !args->pStatus)))
      return E_INVALIDARG;
   Device *dev = DD(args->hDevice);
   for (SIZE_T i = 0; i < args->Resources; i++) {
      Resource *res = DR(args->pResources[i]);
      if (!res || res->dev != dev)
         return E_INVALIDARG;
      args->pStatus[i] = DXGI_DDI_RESIDENCY_FULLY_RESIDENT;
      if (!res->storage.runtime)
         continue;
      const D3DKMT_HANDLE allocation = runtime_allocation(res->storage.runtime);
      if (!allocation)
         return D3DDDIERR_DEVICEREMOVED;
      if (!dev->kt.pfnQueryResidencyCb)
         return DXGI_ERROR_UNSUPPORTED;
      D3DDDI_RESIDENCYSTATUS status = D3DDDI_RESIDENCYSTATUS_NOTRESIDENT;
      D3DDDICB_QUERYRESIDENCY query = {};
      query.NumAllocations = 1;
      query.HandleList = &allocation;
      query.pResidencyStatus = &status;
      HRESULT hr = dev->kt.pfnQueryResidencyCb(dev->hRTDevice.handle, &query);
      if (FAILED(hr))
         return hr;
      args->pStatus[i] = status == D3DDDI_RESIDENCYSTATUS_NOTRESIDENT ?
         DXGI_DDI_RESIDENCY_EVICTED_TO_DISK : status == D3DDDI_RESIDENCYSTATUS_RESIDENTINSHAREDMEMORY ?
         DXGI_DDI_RESIDENCY_RESIDENT_IN_SHARED_MEMORY : DXGI_DDI_RESIDENCY_FULLY_RESIDENT;
   }
   return S_OK;
}

/* Moves storage between the logical buffers of a flip chain. Pure
 * bookkeeping: no pixels move and nothing waits for the GPU. */
static HRESULT APIENTRY RotateResourceIdentities(DXGI_DDI_ARG_ROTATE_RESOURCE_IDENTITIES *args)
{
   if (!args || !args->hDevice || (args->Resources && !args->pResources))
      return E_INVALIDARG;
   const UINT count = args->Resources;
   if (count <= 1)
      return S_OK;
   Device *dev = DD(args->hDevice);
   HRESULT hr = S_OK;
   Resource *first = DR(args->pResources[0]);
   for (UINT i = 0; i < count && SUCCEEDED(hr); i++) {
      Resource *r = DR(args->pResources[i]);
      if (!r || r->dev != dev || r->kind != ResKind::Image || !r->storage.image)
         hr = E_INVALIDARG;
      else if (r->format != first->format || r->width != first->width ||
               r->height != first->height || r->mips != first->mips ||
               r->arraySize != first->arraySize || r->bind != first->bind ||
               r->pitch != first->pitch ||
               !r->storage.runtime != !first->storage.runtime)
         hr = DXGI_ERROR_UNSUPPORTED;
      for (UINT j = 0; j < i && SUCCEEDED(hr); j++)
         if (DR(args->pResources[j]) == r)
            hr = E_INVALIDARG;
   }
   if (SUCCEEDED(hr)) {
      /* Recorded clears and the open render pass name views, and a view
       * follows its resource to the new storage. */
      flush_pending_clears(dev, nullptr);
      end_rendering(dev);
      const ImageStorage head = first->storage;
      for (UINT i = 0; i + 1 < count; i++)
         DR(args->pResources[i])->storage = DR(args->pResources[i + 1])->storage;
      DR(args->pResources[count - 1])->storage = head;
      for (UINT i = 0; i < count; i++) {
         Resource *r = DR(args->pResources[i]);
         if (r->storage.runtime)
            runtime_rebind(r->storage.runtime, r->hRT);
      }
      dev->dirty |= DIRTY_TEXTURES;
   }
   trace(TGD3D_TRACE_ROTATE, hr, count, 0);
   return hr;
}

static HRESULT APIENTRY Blt(DXGI_DDI_ARG_BLT *args)
{
   if (!args || !args->hDevice || !args->hDstResource || !args->hSrcResource)
      return E_INVALIDARG;
   Device *dev = DD(args->hDevice);
   Resource *dst = DR(args->hDstResource), *src = DR(args->hSrcResource);
   HRESULT hr = S_OK;
   if (dst->dev != dev || src->dev != dev ||
       dst->kind != ResKind::Image || src->kind != ResKind::Image ||
       args->DstSubresource >= dst->subresources() || args->SrcSubresource >= src->subresources() ||
       args->DstRight <= args->DstLeft || args->DstBottom <= args->DstTop ||
       args->DstRight > dst->mipWidth(args->DstSubresource % dst->mips) ||
       args->DstBottom > dst->mipHeight(args->DstSubresource % dst->mips))
      hr = E_INVALIDARG;
   else if (args->Flags.Resolve || args->Flags.Reserved)
      hr = DXGI_ERROR_UNSUPPORTED;
   else if (args->Rotate != DXGI_DDI_MODE_ROTATION_IDENTITY &&
            args->Rotate != DXGI_DDI_MODE_ROTATION_UNSPECIFIED &&
            (args->Rotate < DXGI_DDI_MODE_ROTATION_ROTATE90 ||
             args->Rotate > DXGI_DDI_MODE_ROTATION_ROTATE270))
      hr = E_INVALIDARG;
   else if ((src->aspects | dst->aspects) != VK_IMAGE_ASPECT_COLOR_BIT)
      hr = DXGI_ERROR_UNSUPPORTED;
   if (SUCCEEDED(hr)) {
      if (args->Rotate >= DXGI_DDI_MODE_ROTATION_ROTATE90) {
         hr = rotated_blit(dev, args);
         trace(TGD3D_TRACE_BLT, hr, args->Flags.Value, args->Rotate);
         return hr;
      }
      const UINT src_mip = args->SrcSubresource % src->mips;
      const UINT dst_mip = args->DstSubresource % dst->mips;
      const int32_t sw = (int32_t)src->mipWidth(src_mip), sh = (int32_t)src->mipHeight(src_mip);
      const int32_t dw = (int32_t)(args->DstRight - args->DstLeft);
      const int32_t dh = (int32_t)(args->DstBottom - args->DstTop);
      VkCommandBuffer cmd = cmd_begin(dev);
      if (!cmd)
         return FAILED(dev->lost) ? dev->lost : E_OUTOFMEMORY;
      image_use(dev, src->storage, false);
      barrier_for_read(dev, src->storage.writeSerial);
      image_use(dev, dst->storage, true);
      cmd = dev->cmd;
      VkImageBlit blit = {};
      blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, src_mip, args->SrcSubresource / src->mips, 1};
      blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, dst_mip, args->DstSubresource / dst->mips, 1};
      blit.srcOffsets[1] = {sw, sh, 1};
      blit.dstOffsets[0] = {(int32_t)args->DstLeft, (int32_t)args->DstTop, 0};
      blit.dstOffsets[1] = {(int32_t)args->DstRight, (int32_t)args->DstBottom, 1};
      dev->vk.CmdBlitImage(cmd, src->storage.image, VK_IMAGE_LAYOUT_GENERAL, dst->storage.image,
                           VK_IMAGE_LAYOUT_GENERAL, 1, &blit,
                           (sw != dw || sh != dh) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
      dev->stats.copyBytes += (uint64_t)dw * dh * 4;
   }
   trace(TGD3D_TRACE_BLT, hr, args->Flags.Value, args->Rotate);
   return hr;
}

static HRESULT APIENTRY ResolveSharedResource(DXGI_DDI_ARG_RESOLVESHAREDRESOURCE *args)
{
   if (!args || !args->hDevice)
      return E_INVALIDARG;
   /* Ownership of a shared surface is changing: everything recorded for it
    * goes to the queue, and the runtime's next signal waits for it. */
   return flush(DD(args->hDevice), true);
}

void dxgi_fill_table(DXGI1_1_DDI_BASE_FUNCTIONS *t, bool has_resolve)
{
   t->pfnPresent = Present;
   t->pfnGetGammaCaps = GetGammaCaps;
   t->pfnSetDisplayMode = SetDisplayMode;
   t->pfnSetResourcePriority = SetResourcePriority;
   t->pfnQueryResourceResidency = QueryResourceResidency;
   t->pfnRotateResourceIdentities = RotateResourceIdentities;
   t->pfnBlt = Blt;
   if (has_resolve)
      t->pfnResolveSharedResource = ResolveSharedResource;
}

}
