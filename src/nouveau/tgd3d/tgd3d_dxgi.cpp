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
static HRESULT APIENTRY Blt1(DXGI_DDI_ARG_BLT1 *args);

static HRESULT present_result(Device *dev, HRESULT hr)
{
   if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == D3DDDIERR_DEVICEREMOVED ||
       hr == DXGI_ERROR_DEVICE_RESET)
      dev->lost = hr;
   return hr;
}

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
       args->Flags.AllowTearing || args->Flags.AllowFlexibleRefresh ||
       args->Flags.NoScanoutTransform || args->Flags.Reserved ||
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
      present_result(dev, hr);
   }
   const uint64_t completed = dev->completed();
   const uint64_t value[6] = {args->Flags.Value, (uint64_t)args->FlipInterval,
                              dev->submitted - completed, backpressure, 0, 0};
   trace_timing(TGD3D_TIMING_PRESENT, hr, dev->submitted, completed, qpc, value);
   return hr;
}

static bool inside(const RECT &rect, UINT width, UINT height)
{
   return rect.left >= 0 && rect.top >= 0 && rect.right >= rect.left &&
          rect.bottom >= rect.top && (UINT)rect.right <= width && (UINT)rect.bottom <= height;
}

static bool full_rect(const RECT &rect, UINT width, UINT height)
{
   return !rect.left && !rect.top && rect.right == (LONG)width && rect.bottom == (LONG)height;
}

/* Scanout is restricted to the exact linear primary layouts understood by
 * the KMD. A private VkImage or a shared texture is not automatically a primary. */
static bool scanout_resource(Device *dev, const Resource *res)
{
   return res && res->dev == dev && res->kind == ResKind::Image &&
          res->imageType == VK_IMAGE_TYPE_2D && res->storage.image &&
          res->mips == 1 && res->arraySize == 1 && res->depth == 1 &&
          runtime_is_primary(res->storage.runtime) && runtime_allocation(res->storage.runtime) &&
          (res->format == DXGI_FORMAT_B8G8R8A8_UNORM || res->format == DXGI_FORMAT_B8G8R8X8_UNORM) &&
          ((res->width == 720 && res->height == 1280) ||
           (res->width == 1280 && res->height == 720)) && res->pitch == res->width * 4;
}

bool direct_flip_compatible(Device *dev, const Resource *source, const Resource *target)
{
   return dev && SUCCEEDED(dev->lost) && scanout_resource(dev, source) &&
          scanout_resource(dev, target) && source->width == target->width &&
          source->height == target->height && source->pitch == target->pitch &&
          source->format == target->format &&
          runtime_primary_compatible(source->storage.runtime, target->storage.runtime);
}

static HRESULT APIENTRY Present1(DXGI_DDI_ARG_PRESENT1 *args)
{
   if (!args || !args->hDevice || args->Reserved || args->SurfacesToPresent != 1 ||
       !args->phSurfacesToPresent || !args->phSurfacesToPresent[0].hSurface ||
       (args->DirtyRects && !args->pDirtyRects))
      return E_INVALIDARG;
   const auto &source = args->phSurfacesToPresent[0];
   const Resource *res = DR(source.hSurface);
   if (res->dev != DD(args->hDevice) || source.SubResourceIndex >= res->subresources())
      return E_INVALIDARG;
   const UINT mip = source.SubResourceIndex % res->mips;
   for (UINT i = 0; i < args->DirtyRects; ++i)
      if (!inside(args->pDirtyRects[i], res->mipWidth(mip), res->mipHeight(mip)))
         return E_INVALIDARG;
   /* PresentCb consumes the opaque runtime token, including dirty rectangles.
    * Do not recreate the token or issue one callback per rectangle. */
   DXGI_DDI_ARG_PRESENT present = {};
   present.hDevice = args->hDevice;
   present.hSurfaceToPresent = source.hSurface;
   present.SrcSubResourceIndex = source.SubResourceIndex;
   present.hDstResource = args->hDstResource;
   present.DstSubResourceIndex = args->DstSubResourceIndex;
   present.pDXGIContext = args->pDXGIContext;
   present.Flags = args->Flags;
   present.FlipInterval = args->FlipInterval;
   /* BackBufferMultiplicity belongs to WDDM 2.0, not this 1.3 table. */
   return Present(&present);
}

static bool mpo_plane(Device *dev, const DXGI_DDI_PRESENT_MULTIPLANE_OVERLAY &plane)
{
   if (plane.LayerIndex || (plane.Enabled != TRUE && plane.Enabled != FALSE))
      return false;
   /* Disabled planes have no allocation. Their other attributes are ignored. */
   if (!plane.Enabled)
      return true;
   const Resource *res = DR(plane.hResource);
   if (!scanout_resource(dev, res) || plane.SubResourceIndex)
      return false;
   const auto &a = plane.PlaneAttributes;
   const bool quarter = a.Rotation == DXGI_DDI_MODE_ROTATION_ROTATE90 ||
                        a.Rotation == DXGI_DDI_MODE_ROTATION_ROTATE270;
   if (a.Rotation < DXGI_DDI_MODE_ROTATION_IDENTITY ||
       a.Rotation > DXGI_DDI_MODE_ROTATION_ROTATE270 ||
       res->width != (quarter ? 1280u : 720u) || res->height != (quarter ? 720u : 1280u) ||
       a.Flags || !full_rect(a.SrcRect, res->width, res->height) ||
       !full_rect(a.DstRect, 720, 1280) ||
       !(full_rect(a.ClipRect, 720, 1280) ||
         (!a.ClipRect.left && !a.ClipRect.top && !a.ClipRect.right && !a.ClipRect.bottom)) ||
       a.Blend != DXGI_DDI_MULTIPLANE_OVERLAY_BLEND_OPAQUE ||
       a.VideoFrameFormat != DXGI_DDI_MULIIPLANE_OVERLAY_VIDEO_FRAME_FORMAT_PROGRESSIVE ||
       a.YCbCrFlags || a.StereoFormat != DXGI_DDI_MULTIPLANE_OVERLAY_STEREO_FORMAT_MONO ||
       a.StereoFlipMode != DXGI_DDI_MULTIPLANE_OVERLAY_STEREO_FLIP_NONE)
      return false;
   /* Dirty rectangles are only an optimization. The complete allocation is
    * scanned out; do not dereference or reject a plane because of this hint.
    * The runtime's token retains the original rectangles without alteration. */
   return true;
}

static bool mpo_flags(const DXGI_DDI_PRESENT_FLAGS &flags, DXGI_DDI_FLIP_INTERVAL_TYPE interval)
{
   return flags.Flip && !flags.Blt && !flags.PreferRight && !flags.TemporaryMono &&
          !flags.AllowTearing && !flags.AllowFlexibleRefresh && !flags.NoScanoutTransform &&
          !flags.Reserved && interval >= DXGI_DDI_FLIP_INTERVAL_ONE &&
          interval <= DXGI_DDI_FLIP_INTERVAL_FOUR;
}

static HRESULT APIENTRY PresentMultiplaneOverlay(DXGI_DDI_ARG_PRESENTMULTIPLANEOVERLAY *args)
{
   if (!args || !args->hDevice || args->VidPnSourceId || args->Reserved ||
       args->PresentPlaneCount != 1 || !args->pPresentPlanes)
      return E_INVALIDARG;
   Device *dev = DD(args->hDevice);
   if (FAILED(dev->lost))
      return dev->lost;
   const auto &plane = args->pPresentPlanes[0];
   /* This callback version cannot override interval zero. Immediate/tearing
    * and independent flip are not advertised by the paired display driver. */
   if (!mpo_flags(args->Flags, args->FlipInterval) || !mpo_plane(dev, plane) ||
       !dev->hContext || !dev->dxgi || !dev->dxgi->pfnPresentMultiplaneOverlayCb)
      return DXGI_ERROR_UNSUPPORTED;
   HRESULT hr = flush(dev, true);
   /* Retirement reads the completion counter and can discover device loss
    * after QueueSubmit/gate succeeded. Never present after that failure. */
   if (FAILED(hr) || FAILED(dev->lost))
      return FAILED(hr) ? hr : dev->lost;
   DXGIDDICB_PRESENT_MULTIPLANE_OVERLAY present = {};
   present.pDXGIContext = args->pDXGIContext;
   present.hContext = dev->hContext;
   present.AllocationInfoCount = 1;
   if (plane.Enabled)
      present.AllocationInfo[0].PresentAllocation = runtime_allocation(DR(plane.hResource)->storage.runtime);
   /* The token carries rotation and the plane attributes unchanged. No blit,
    * CPU fence wait or synthetic completion belongs in this handoff. */
   return present_result(dev, dev->dxgi->pfnPresentMultiplaneOverlayCb(dev->hRTDevice.handle, &present));
}

static bool mpo_plane1(Device *dev, const DXGI_DDI_PRESENT_MULTIPLANE_OVERLAY1 &plane)
{
   /* The 1.4 attributes replaced YCbCrFlags with ColorSpace and added tails.
    * Convert named fields; the two structure layouts are not interchangeable. */
   if (plane.Enabled && (plane.PlaneAttributes.ColorSpace != D3DDDI_COLOR_SPACE_RGB_FULL_G22_NONE_P709 ||
                         plane.PlaneAttributes.ColorKey))
      return false;
   DXGI_DDI_PRESENT_MULTIPLANE_OVERLAY old = {};
   old.LayerIndex = plane.LayerIndex;
   old.Enabled = plane.Enabled;
   if (!plane.Enabled) return mpo_plane(dev, old);
   old.hResource = plane.hResource;
   old.SubResourceIndex = plane.SubResourceIndex;
   const auto &a = plane.PlaneAttributes;
   old.PlaneAttributes.Flags = a.Flags;
   old.PlaneAttributes.SrcRect = a.SrcRect;
   old.PlaneAttributes.DstRect = a.DstRect;
   old.PlaneAttributes.ClipRect = a.ClipRect;
   old.PlaneAttributes.Rotation = a.Rotation;
   old.PlaneAttributes.Blend = a.Blend;
   old.PlaneAttributes.VideoFrameFormat = a.VideoFrameFormat;
   old.PlaneAttributes.StereoFormat = a.StereoFormat;
   old.PlaneAttributes.StereoFlipMode = a.StereoFlipMode;
   return mpo_plane(dev, old);
}

static HRESULT APIENTRY PresentMultiplaneOverlay1(DXGI_DDI_ARG_PRESENTMULTIPLANEOVERLAY1 *args)
{
   if (!args || !args->hDevice || args->VidPnSourceId ||
       args->PresentPlaneCount != 1 || !args->pPresentPlanes)
      return E_INVALIDARG;
   Device *dev = DD(args->hDevice);
   if (FAILED(dev->lost)) return dev->lost;
   const auto &plane = args->pPresentPlanes[0];
   if (dev->interfaceVersion < D3DWDDM2_0_DDI_INTERFACE_VERSION ||
       !mpo_flags(args->Flags, args->FlipInterval) || !mpo_plane1(dev, plane) ||
       !dev->hContext || !dev->dxgi || !dev->dxgi->pfnPresentMultiplaneOverlay1Cb)
      return DXGI_ERROR_UNSUPPORTED;
   HRESULT hr = flush(dev, true);
   if (FAILED(hr) || FAILED(dev->lost)) return FAILED(hr) ? hr : dev->lost;

   HANDLE context = dev->hContext;
   D3DKMT_HANDLE allocation = plane.Enabled ?
      runtime_allocation(DR(plane.hResource)->storage.runtime) : 0;
   DXGIDDI_MULTIPLANE_OVERLAY_PLANE_INFO info = {};
   info.ContextCount = 1;
   info.pContextList = &context;
   info.pAllocationList = &allocation;
   /* WDDM2.0/2.1 callbacks consume the prefix through pDriverPrivateData.
    * The matching 22621 runtime's PreWDDM2_2 entry reads these same offsets.
    * Do not depend on the later SyncIntervalOverride tail: intervals 1..4
    * are preserved in the opaque token, and interval zero is not admitted. */
   DXGIDDI_MULTIPLANE_OVERLAY_PLANE_INFO *planes[] = {&info};
   DXGIDDICB_PRESENT_MULTIPLANE_OVERLAY1 present = {};
   present.pDXGIContext = args->pDXGIContext;
   present.PresentPlaneCount = 1;
   present.ppPresentPlanes = planes;
   /* Submit -> runtime GPU wait -> same-thread callback. Neither the shared
    * allocation nor its rendering fence is retired by accepting the Present.
    * Keep the context even for a disabled plane: runtime needs it to submit
    * the disable operation when no enabled allocation supplies a context. */
   return present_result(dev, dev->dxgi->pfnPresentMultiplaneOverlay1Cb(dev->hRTDevice.handle, &present));
}

static HRESULT APIENTRY CheckMultiplaneOverlayColorSpaceSupport(
   DXGI_DDI_ARG_CHECKMULTIPLANEOVERLAYCOLORSPACESUPPORT *args)
{
   if (!args) return E_INVALIDARG;
   args->Supported = FALSE;
   if (!args->hDevice || args->VidPnSourceId) return E_INVALIDARG;
   if (FAILED(DD(args->hDevice)->lost)) return DD(args->hDevice)->lost;
   args->Supported = args->ColorSpace == D3DDDI_COLOR_SPACE_RGB_FULL_G22_NONE_P709 &&
      (args->Format == DXGI_FORMAT_B8G8R8A8_UNORM || args->Format == DXGI_FORMAT_B8G8R8X8_UNORM);
   return S_OK;
}

static HRESULT APIENTRY GetMultiplaneOverlayCaps(DXGI_DDI_ARG_GETMULTIPLANEOVERLAYCAPS *args)
{
   if (!args || !args->hDevice || args->VidPnSourceId)
      return E_INVALIDARG;
   if (FAILED(DD(args->hDevice)->lost))
      return DD(args->hDevice)->lost;
   args->MultiplaneOverlayCaps = {1, 1};
   return S_OK;
}

static HRESULT APIENTRY GetMultiplaneOverlayGroupCaps(DXGI_DDI_ARG_GETMULTIPLANEOVERLAYGROUPCAPS *args)
{
   if (!args || !args->hDevice || args->VidPnSourceId || args->GroupIndex)
      return E_INVALIDARG;
   if (FAILED(DD(args->hDevice)->lost))
      return DD(args->hDevice)->lost;
   args->MultiplaneOverlayGroupCaps = {1, 1.0f, 1.0f,
      DXGI_DDI_MULTIPLANE_OVERLAY_FEATURE_CAPS_RGB |
      DXGI_DDI_MULTIPLANE_OVERLAY_FEATURE_CAPS_ROTATION_WITHOUT_INDEPENDENT_FLIP, 0};
   return S_OK;
}

static HRESULT APIENTRY CheckPresentDurationSupport(DXGI_DDI_ARG_CHECKPRESENTDURATIONSUPPORT *args)
{
   if (!args || !args->hDevice || args->VidPnSourceId)
      return E_INVALIDARG;
   if (FAILED(DD(args->hDevice)->lost))
      return DD(args->hDevice)->lost;
   /* Fixed panel timing; no seamless refresh-rate switching is supported. */
   args->ClosestSmallerDuration = args->ClosestLargerDuration = 0;
   return S_OK;
}

void dxgi_fill_presentation12(DXGI1_2_DDI_BASE_FUNCTIONS *table, bool has_mpo)
{
   DXGI1_1_DDI_BASE_FUNCTIONS old = {};
   dxgi_fill_table(&old, true);
   table->pfnPresent = old.pfnPresent;
   table->pfnGetGammaCaps = old.pfnGetGammaCaps;
   table->pfnSetDisplayMode = old.pfnSetDisplayMode;
   table->pfnSetResourcePriority = old.pfnSetResourcePriority;
   table->pfnQueryResourceResidency = old.pfnQueryResourceResidency;
   table->pfnRotateResourceIdentities = old.pfnRotateResourceIdentities;
   table->pfnBlt = old.pfnBlt;
   table->pfnResolveSharedResource = old.pfnResolveSharedResource;
   table->pfnBlt1 = Blt1;
   dxgi_fill_residency12(table);
   /* Older Win8 revisions can lack the MPO tail. Follow the negotiated
    * macro, not the build number alone. Video opt-in claims no overlays. */
   if (has_mpo) {
      table->pfnGetMultiplaneOverlayCaps = nullptr;
      table->pfnGetMultiplaneOverlayFilterRange = nullptr;
      table->pfnCheckMultiplaneOverlaySupport = nullptr;
      table->pfnPresentMultiplaneOverlay = nullptr;
   }
}

void dxgi_fill_presentation13(DXGI1_3_DDI_BASE_FUNCTIONS *table)
{
   *table = {};
   DXGI1_1_DDI_BASE_FUNCTIONS old = {};
   dxgi_fill_table(&old, true);
   /* The prefix is shared semantically, not by casting the longer table. */
   table->pfnPresent = old.pfnPresent;
   table->pfnGetGammaCaps = old.pfnGetGammaCaps;
   table->pfnSetDisplayMode = old.pfnSetDisplayMode;
   table->pfnSetResourcePriority = old.pfnSetResourcePriority;
   table->pfnQueryResourceResidency = old.pfnQueryResourceResidency;
   table->pfnRotateResourceIdentities = old.pfnRotateResourceIdentities;
   table->pfnBlt = old.pfnBlt;
   table->pfnResolveSharedResource = old.pfnResolveSharedResource;
   dxgi_fill_residency13(table);
   table->pfnBlt1 = Blt1;
   table->pfnPresent1 = Present1;
   table->pfnPresentMultiplaneOverlay = PresentMultiplaneOverlay;
   table->pfnGetMultiplaneOverlayCaps = GetMultiplaneOverlayCaps;
   table->pfnGetMultiplaneOverlayGroupCaps = GetMultiplaneOverlayGroupCaps;
   table->pfnCheckPresentDurationSupport = CheckPresentDurationSupport;
   table->pfnReserved1 = table->pfnReserved2 = nullptr;
}

void dxgi_fill_presentation14(DXGI1_4_DDI_BASE_FUNCTIONS *table)
{
   *table = {};
   DXGI1_3_DDI_BASE_FUNCTIONS old = {};
   dxgi_fill_presentation13(&old);
#define SAME(name) table->name = old.name
   SAME(pfnPresent); SAME(pfnGetGammaCaps); SAME(pfnSetDisplayMode);
   SAME(pfnSetResourcePriority); SAME(pfnQueryResourceResidency);
   SAME(pfnRotateResourceIdentities); SAME(pfnBlt); SAME(pfnResolveSharedResource);
   SAME(pfnBlt1); SAME(pfnOfferResources); SAME(pfnReclaimResources);
   SAME(pfnGetMultiplaneOverlayCaps); SAME(pfnGetMultiplaneOverlayGroupCaps);
   SAME(pfnPresentMultiplaneOverlay); SAME(pfnPresent1); SAME(pfnCheckPresentDurationSupport);
#undef SAME
   table->pfnPresentMultiplaneOverlay1 = PresentMultiplaneOverlay1;
   table->pfnCheckMultiplaneOverlayColorSpaceSupport = CheckMultiplaneOverlayColorSpaceSupport;
   table->pfnTrimResidencySet = TrimResidencySet;
   /* Paired with typed WDDM2 device/context and residency negotiation.
    * A 1.3 table cannot supply the 1.4 MPO1 slot
    * tested by DWM's D3D11 runtime for single-plane MPO3 conversion. */
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
   if (res->dev != dev || args->SubResourceIndex)
      return E_INVALIDARG;
   if (!runtime_is_primary(res->storage.runtime))
      return DXGI_ERROR_UNSUPPORTED;
   if (!runtime_allocation(res->storage.runtime))
      return D3DDDIERR_DEVICEREMOVED;
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
   RuntimeResource *owner = resource_runtime(res);
   const D3DKMT_HANDLE allocation = runtime_allocation(owner);
   /* Private NVK storage holds a residency reference for its entire life. */
   if (!owner)
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
      RuntimeResource *owner = resource_runtime(res);
      if (!owner)
         continue;
      const D3DKMT_HANDLE allocation = runtime_allocation(owner);
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

static HRESULT blt(DXGI_DDI_ARG_BLT *args, const RECT *source)
{
   if (!args || !args->hDevice || !args->hDstResource || !args->hSrcResource)
      return E_INVALIDARG;
   Device *dev = DD(args->hDevice);
   if (FAILED(dev->lost)) return dev->lost;
   Resource *dst = DR(args->hDstResource), *src = DR(args->hSrcResource);
   HRESULT hr = S_OK;
   if (dst->dev != dev || src->dev != dev ||
       dst->kind != ResKind::Image || src->kind != ResKind::Image ||
       args->DstSubresource >= dst->subresources() || args->SrcSubresource >= src->subresources() ||
       args->DstRight <= args->DstLeft || args->DstBottom <= args->DstTop ||
       args->DstRight > dst->mipWidth(args->DstSubresource % dst->mips) ||
       args->DstBottom > dst->mipHeight(args->DstSubresource % dst->mips))
      hr = E_INVALIDARG;
   else if (source && (!inside(*source, src->mipWidth(args->SrcSubresource % src->mips),
                                           src->mipHeight(args->SrcSubresource % src->mips)) ||
                       source->left == source->right || source->top == source->bottom))
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
      if (src->storage.image == dst->storage.image) {
         /* Blt1 also permits a same-surface copy. Snapshot the complete source
          * rectangle before any destination write, including scaled/rotated
          * copies. ResourceCopyRegion's overlap handling alone does not cover
          * this DXGI entry point. This is not the display rotation policy. */
         const FormatInfo *format = format_info(src->format);
         if (src->imageType != VK_IMAGE_TYPE_2D || dst->imageType != VK_IMAGE_TYPE_2D ||
             !src->storage.image || !format || (format->flags & FMT_BC) ||
             runtime_is_offered(resource_runtime(src)) || runtime_is_offered(resource_runtime(dst)))
            return DXGI_ERROR_UNSUPPORTED;
         const UINT mip = args->SrcSubresource % src->mips;
         const RECT region = source ? *source :
            RECT{0, 0, (LONG)src->mipWidth(mip), (LONG)src->mipHeight(mip)};
         Resource saved;
         saved.dev = dev; saved.kind = ResKind::Image; saved.dim = D3D10DDIRESOURCE_TEXTURE2D;
         saved.format = src->format; saved.vkFormat = src->vkFormat;
         saved.aspects = VK_IMAGE_ASPECT_COLOR_BIT;
         saved.width = region.right - region.left; saved.height = region.bottom - region.top;
         saved.bind = D3D10_DDI_BIND_SHADER_RESOURCE;
         hr = image_create(dev, &saved);
         if (FAILED(hr)) return hr;
         VkCommandBuffer cmd = cmd_begin(dev, &saved.storage);
         if (cmd) {
            image_use(dev, src->storage, false);
            barrier_for_read(dev, src->storage.writeSerial);
            image_use(dev, saved.storage, true);
            VkImageCopy copy = {};
            copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, args->SrcSubresource / src->mips, 1};
            copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.srcOffset = {region.left, region.top, 0};
            copy.extent = {saved.width, saved.height, 1};
            dev->vk.CmdCopyImage(dev->cmd, src->storage.image, VK_IMAGE_LAYOUT_GENERAL,
                                 saved.storage.image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
            dev->stats.copyBytes += (uint64_t)saved.width * saved.height * format->bytes;
            DXGI_DDI_ARG_BLT next = *args;
            next.hSrcResource = (DXGI_DDI_HRESOURCE)&saved;
            next.SrcSubresource = 0;
            hr = blt(&next, nullptr);
         } else {
            hr = FAILED(dev->lost) ? dev->lost : E_OUTOFMEMORY;
         }
         /* On any later failure, the saved source may already be in flight.
          * Its image/views/memory follow the recorded GPU sequence as usual. */
         storage_destroy(dev, saved.storage, false);
         return hr;
      }
      if (args->Rotate >= DXGI_DDI_MODE_ROTATION_ROTATE90) {
         hr = rotated_blit(dev, args, source);
         trace(TGD3D_TRACE_BLT, hr, args->Flags.Value, args->Rotate);
         return hr;
      }
      const UINT src_mip = args->SrcSubresource % src->mips;
      const UINT dst_mip = args->DstSubresource % dst->mips;
      const int32_t sx = source ? source->left : 0, sy = source ? source->top : 0;
      const int32_t sw = source ? source->right - sx : (int32_t)src->mipWidth(src_mip);
      const int32_t sh = source ? source->bottom - sy : (int32_t)src->mipHeight(src_mip);
      const int32_t dw = (int32_t)(args->DstRight - args->DstLeft);
      const int32_t dh = (int32_t)(args->DstBottom - args->DstTop);
      VkCommandBuffer cmd = cmd_begin(dev, &dst->storage);
      if (!cmd)
         return FAILED(dev->lost) ? dev->lost : E_OUTOFMEMORY;
      image_use(dev, src->storage, false);
      barrier_for_read(dev, src->storage.writeSerial);
      image_use(dev, dst->storage, true);
      cmd = dev->cmd;
      VkImageBlit blit = {};
      blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, src_mip, args->SrcSubresource / src->mips, 1};
      blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, dst_mip, args->DstSubresource / dst->mips, 1};
      blit.srcOffsets[0] = {sx, sy, 0};
      blit.srcOffsets[1] = {sx + sw, sy + sh, 1};
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

static HRESULT APIENTRY Blt(DXGI_DDI_ARG_BLT *args)
{
   return blt(args, nullptr);
}

static HRESULT APIENTRY Blt1(DXGI_DDI_ARG_BLT1 *args)
{
   if (!args || args->SrcLeft > INT32_MAX || args->SrcTop > INT32_MAX ||
       args->SrcRight > INT32_MAX || args->SrcBottom > INT32_MAX)
      return E_INVALIDARG;
   DXGI_DDI_ARG_BLT copy = {};
   copy.hDevice = args->hDevice; copy.hDstResource = args->hDstResource;
   copy.DstSubresource = args->DstSubresource;
   copy.DstLeft = args->DstLeft; copy.DstTop = args->DstTop;
   copy.DstRight = args->DstRight; copy.DstBottom = args->DstBottom;
   copy.hSrcResource = args->hSrcResource; copy.SrcSubresource = args->SrcSubresource;
   copy.Flags = args->Flags; copy.Rotate = args->Rotate;
   const RECT source = {(LONG)args->SrcLeft, (LONG)args->SrcTop,
                         (LONG)args->SrcRight, (LONG)args->SrcBottom};
   return blt(&copy, &source);
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
