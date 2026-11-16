/* SPDX-License-Identifier: MIT
 * DXGI resource offer/reclaim. Kernel allocations, GPU completion and paging
 * residency have distinct ownership; none of them is inferred from S_OK alone.
 */
#include "tgd3d.h"
#include <memory>

namespace tgd3d {

static HRESULT residency_result(Device *dev, HRESULT hr)
{
   if (hr == D3DDDIERR_DEVICEREMOVED || hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
      dev->lost = hr;
   return hr;
}

static HRESULT owners(Device *dev, const DXGI_DDI_HRESOURCE *resources, UINT count,
                       bool offered, RuntimeResource **out)
{
   if (count && !resources) return E_INVALIDARG;
   for (UINT i = 0; i < count; ++i) {
      const auto *res = reinterpret_cast<const Resource *>(resources[i]);
      if (!res || res->dev != dev || res->usage == D3D10_DDI_USAGE_IMMUTABLE || res->bufferMapped)
         return E_INVALIDARG;
      for (const auto &map : res->maps) if (map.mapped) return E_INVALIDARG;
      /* Offering shared resources requires keyed-mutex ownership. The current
       * device cannot create keyed-mutex resources and must not offer an
       * ordinary shared surface that another process may still be accessing. */
      if (res->misc & D3D10_DDI_RESOURCE_MISC_SHARED) return E_INVALIDARG;
      out[i] = resource_runtime(res);
      if (!out[i]) return DXGI_ERROR_UNSUPPORTED;
      if (runtime_is_offered(out[i]) != offered) return E_INVALIDARG;
   }
   return S_OK;
}

static HRESULT APIENTRY OfferResources(DXGI_DDI_ARG_OFFERRESOURCES *args)
{
   if (!args || !args->hDevice || args->Priority < D3DDDI_OFFER_PRIORITY_LOW ||
       args->Priority > D3DDDI_OFFER_PRIORITY_HIGH) return E_INVALIDARG;
   auto *dev = reinterpret_cast<Device *>(args->hDevice);
   if (FAILED(dev->lost)) return dev->lost;
   if (!args->Resources) return S_OK;
   if (!dev->nvk || !dev->kt.pfnOfferAllocationsCb) return DXGI_ERROR_UNSUPPORTED;
   std::unique_ptr<RuntimeResource *[]> list(new (std::nothrow) RuntimeResource *[args->Resources]);
   if (!list) return E_OUTOFMEMORY;
   HRESULT hr = owners(dev, args->pResources, args->Resources, false, list.get());
   if (FAILED(hr)) return hr;
   hr = flush(dev, true); // real NVK submission -> runtime GPU wait; no CPU render-fence completion
   if (FAILED(hr) || FAILED(dev->lost)) return FAILED(hr) ? hr : dev->lost;
   for (UINT i = 0; i < args->Resources; ++i) {
      auto *res = reinterpret_cast<Resource *>(args->pResources[i]);
      if (res->slice) slice_unmap(dev, res->slice);
   }
   hr = runtime_offer(dev, list.get(), args->Resources, args->Priority);
   if (SUCCEEDED(hr))
      for (UINT i = 0; i < args->Resources; ++i)
         resource_unbind(dev, reinterpret_cast<Resource *>(args->pResources[i]));
   /* On failure the resource remains usable. Its next CPU access remaps via
    * slice_map; no stale mapping survives a dropped Lock2 mapping. */
   return residency_result(dev, hr);
}

static HRESULT APIENTRY ReclaimResources(DXGI_DDI_ARG_RECLAIMRESOURCES *args)
{
   if (!args || !args->hDevice) return E_INVALIDARG;
   auto *dev = reinterpret_cast<Device *>(args->hDevice);
   if (FAILED(dev->lost)) return dev->lost;
   if (!args->Resources) return S_OK;
   if (!dev->nvk) return DXGI_ERROR_UNSUPPORTED;
   std::unique_ptr<RuntimeResource *[]> list(new (std::nothrow) RuntimeResource *[args->Resources]);
   if (!list) return E_OUTOFMEMORY;
   HRESULT hr = owners(dev, args->pResources, args->Resources, true, list.get());
   if (SUCCEEDED(hr)) hr = runtime_reclaim(dev, list.get(), args->Resources, args->pDiscarded);
   /* Successful reclaim proves the paging fence, not render-fence completion.
    * Buffers/staging surfaces reacquire their CPU mapping only on CPU access. */
   return residency_result(dev, hr);
}

void dxgi_fill_residency13(DXGI1_3_DDI_BASE_FUNCTIONS *table)
{
   table->pfnOfferResources = OfferResources;
   table->pfnReclaimResources = ReclaimResources;
}

} // namespace tgd3d
