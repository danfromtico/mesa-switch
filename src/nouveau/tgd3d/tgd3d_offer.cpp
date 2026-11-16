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

uint64_t resource_last_use(const Resource *res)
{
   return res->kind == ResKind::Image ? res->storage.lastUse :
      std::max(res->sliceLastUse, res->sliceLastWrite);
}

HRESULT prepare_resource_residency(Device *dev)
{
   for (Resource *res = dev->resources; res; res = res->next) {
      if (resource_last_use(res) <= dev->submitted) continue;
      HRESULT hr = runtime_make_resident(dev, resource_runtime(res));
      if (FAILED(hr)) return hr;
   }
   return S_OK;
}

HRESULT trim_memory(Device *dev)
{
   if (FAILED(dev->lost)) return dev->lost;
   retire(dev); // only the actual completed queue value retires commands/slices
   if (FAILED(dev->lost)) return dev->lost;
   upload_trim(dev);
   for (auto pool : dev->freePools) dev->vk.DestroyDescriptorPool(dev->vkdev, pool, nullptr);
   dev->freePools.clear();
   if (!dev->freeCommands.empty()) {
      dev->vk.FreeCommandBuffers(dev->vkdev, dev->cmdPool,
         static_cast<uint32_t>(dev->freeCommands.size()), dev->freeCommands.data());
      dev->freeCommands.clear();
   }
   return dev->lost;
}

HRESULT trim_residency_for_allocation(Device *dev, RuntimeResource *exclude,
                                     uint64_t requested, uint64_t *released)
{
   *released = 0;
   HRESULT hr = trim_memory(dev);
   if (FAILED(hr)) return hr;
   const uint64_t done = dev->completed();
   if (FAILED(dev->lost)) return dev->lost;
   /* A zero estimate does not mean unlimited budget. Exhaust eligible idle
    * backing before the single CantTrimFurther attempt. No render wait or
    * submission occurs here, including on resource-create/DDI callback paths.
    */
   const bool exhaust = requested == 0;
   for (Resource *res = dev->resources; res; res = res->next) {
      RuntimeResource *owner = resource_runtime(res);
      if (!owner || owner == exclude || res->primary || resource_last_use(res) > done) continue;
      bool mapped = res->bufferMapped;
      for (const auto &map : res->maps) mapped |= map.mapped;
      if (mapped) continue;
      uint64_t bytes = 0, remaining = 0;
      hr = runtime_evict(owner, &bytes, &remaining);
      if (FAILED(hr)) return residency_result(dev, hr);
      *released += bytes;
      requested = std::max(requested - std::min(requested, bytes), remaining);
      if (!exhaust && !requested) break;
   }
   return S_OK;
}

HRESULT APIENTRY TrimResidencySet(DXGI_DDI_ARG_TRIMRESIDENCYSET *args)
{
   if (!args || !args->hDevice || args->TrimFlags.Reserved ||
       (args->TrimFlags.PeriodicTrim && args->TrimFlags.RestartPeriodicTrim)) return E_INVALIDARG;
   auto *dev = reinterpret_cast<Device *>(args->hDevice);
   if (FAILED(dev->lost)) return dev->lost;
   /* Use the runtime's serialized DDI entry. Do not dispatch allocation
    * callbacks or access this list from a driver-created worker thread. */
   const uint64_t done = dev->completed();
   if (FAILED(dev->lost)) return dev->lost;
   const uint64_t cutoff = dev->periodicTrimSequence;
   if (args->TrimFlags.PeriodicTrim || args->TrimFlags.RestartPeriodicTrim)
      dev->periodicTrimSequence = done;
   HRESULT hr = trim_memory(dev);
   if (FAILED(hr)) return hr;
   uint64_t budget = args->TrimFlags.TrimToBudget ? args->NumBytesToTrim : 0;
   for (Resource *res = dev->resources; res; res = res->next) {
      const uint64_t use = resource_last_use(res);
      bool mapped = res->bufferMapped;
      for (const auto &map : res->maps) mapped |= map.mapped;
      if (mapped || use > done || res->primary) continue;
      if (!(args->TrimFlags.PeriodicTrim && use <= cutoff) && !budget) continue;
      uint64_t bytes = 0, remaining = 0;
      hr = runtime_evict(resource_runtime(res), &bytes, &remaining);
      if (FAILED(hr)) return residency_result(dev, hr);
      budget = std::max(budget - std::min(budget, bytes), remaining);
   }
   /* Live/in-flight and scanout backing can exceed a requested trim amount.
    * Preserve it; never pretend that a render fence completed to meet budget. */
   return S_OK;
}

} // namespace tgd3d
