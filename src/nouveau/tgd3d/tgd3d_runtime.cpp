/* SPDX-License-Identifier: MIT
 *
 * Runtime-owned WDDM allocations: primaries, shared surfaces and surfaces
 * opened from another device. The D3D runtime owns the allocation; this
 * driver makes it resident and maps it into the process GPU address space on
 * the runtime's device, and the Vulkan image is bound to that same backing.
 * No runtime handle is cast to a KMT device or NT handle, and no path here
 * copies pixels: a second owner always sees what the first one rendered.
 *
 * The allocation and callback contracts follow the earlier, hardware-tested
 * owner implementation (tegra-x1-wddm/docs/D3D_RUNTIME_SHARING.md); one
 * paging queue now serves a whole device instead of one per resource.
 */
#include "tgd3d.h"
#include "nouveau/vulkan/nvkmd/windows/allocation_abi.h"
#include "vulkan/util/vk_internal_exts.h"
#include "vulkan/util/vk_wsi_win32_memory.h"

#include <stdlib.h>
#include <string.h>
#include <memory>

namespace tgd3d {

/* Version 1's 72-byte linear description remains unchanged. Version 2 is a
 * same-NVK optimal-image contract, never a physical address or NT handle.
 * Array stride and PTE kinds are selected by the matching NVK image creator;
 * the producer's exact creation/requirements and driver identity are checked
 * again by each importer before binding its own runtime allocation handle. */
struct SharedTiledMetadata {
   tg_gpu_resource_request storage;
   uint32_t magic, version;
   RuntimeSharedDescription description;
   uint32_t array_size, vk_format, image_flags, image_usage;
   uint64_t image_bytes, alignment;
   uint32_t memory_types, driver_version;
   uint8_t device_uuid[VK_UUID_SIZE], driver_uuid[VK_UUID_SIZE];
   uint64_t adapter_luid;
   uint32_t reserved[2];
};
static_assert(sizeof(SharedTiledMetadata) == 152, "optimal shared-image metadata ABI");
static constexpr uint32_t shared_magic = 0x52534754; /* TGSR */

struct Mapping {
   uint64_t address, size;
   bool freed;
   Mapping *next;
};

struct RuntimeResource {
   wsi_win32_memory memory;
   LONG refs;
   SRWLOCK lock;
   HANDLE device, runtime_resource;
   D3DDDI_DEVICECALLBACKS callbacks;
   D3DKMT_HANDLE queue, fence;
   const volatile UINT64 *fence_value;
   HRESULT error, cleanup_error;
   bool cpu_locked, dead, associated, primary;
   bool device_owned, offered, reclaim_pending, discarded;
   bool tiled_shared;
   bool residency_requested, resident;
   uint64_t residency_fence;
   SharedTiledMetadata tiled_layout;
   uint64_t reclaim_fence;
   tg_gpu_primary_allocation_request primary_layout;
   void *cpu;
   Mapping *mappings;
};

/* arg0: stage in the high half, allocation handle in the low half. */
static HRESULT rt_trace(RuntimeResource *p, unsigned stage, HRESULT hr, uint64_t value = 0)
{
   trace(TGD3D_TRACE_RUNTIME_MEMORY, hr, (uint64_t)stage << 32 | p->memory.allocation, value);
   return hr;
}

static VkResult vk_result(RuntimeResource *p, HRESULT hr)
{
   if (SUCCEEDED(hr))
      return VK_SUCCESS;
   p->error = hr;
   if (hr == E_OUTOFMEMORY)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   if (hr == D3DDDIERR_OUTOFVIDEOMEMORY)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   if (hr == D3DDDIERR_DEVICEREMOVED || hr == DXGI_ERROR_DEVICE_REMOVED ||
       hr == DXGI_ERROR_DEVICE_RESET)
      return VK_ERROR_DEVICE_LOST;
   return VK_ERROR_UNKNOWN;
}

static HRESULT wait_paging(const D3DDDI_DEVICECALLBACKS &cb, HANDLE device, D3DKMT_HANDLE fence,
                           const volatile UINT64 *current, UINT64 value)
{
   if (!value || (current && *current >= value))
      return S_OK;
   D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMCPU wait = {};
   wait.ObjectCount = 1;
   wait.ObjectHandleArray = &fence;
   wait.FenceValueArray = &value;
   return cb.pfnWaitForSynchronizationObjectFromCpuCb(device, &wait);
}

static void owner_retain(void *context)
{
   InterlockedIncrement(&static_cast<RuntimeResource *>(context)->refs);
}

static void owner_release(void *context)
{
   auto p = static_cast<RuntimeResource *>(context);
   if (!InterlockedDecrement(&p->refs)) {
      while (p->mappings) {
         Mapping *next = p->mappings->next;
         free(p->mappings);
         p->mappings = next;
      }
      free(p);
   }
}

static HRESULT free_address(RuntimeResource *p, uint64_t address, uint64_t bytes)
{
   D3DDDICB_FREEGPUVIRTUALADDRESS args = {};
   args.BaseAddress = address;
   args.Size = bytes;
   return rt_trace(p, 5, p->callbacks.pfnFreeGpuVirtualAddressCb(p->device, &args), bytes);
}

static VkResult owner_free_gpu(void *context, uint64_t address, uint64_t bytes)
{
   auto p = static_cast<RuntimeResource *>(context);
   AcquireSRWLockExclusive(&p->lock);
   Mapping **link = &p->mappings;
   while (*link && ((*link)->address != address || (*link)->size != bytes))
      link = &(*link)->next;
   Mapping *mapping = *link;
   HRESULT hr = S_OK;
   /* Deallocating the runtime resource already released its addresses, and
    * they may have been handed out again. Never free a stale range. */
   if ((!mapping || !mapping->freed) && !p->dead)
      hr = free_address(p, address, bytes);
   if (FAILED(hr))
      p->cleanup_error = hr;
   if (mapping) {
      *link = mapping->next;
      free(mapping);
   }
   VkResult status = vk_result(p, hr);
   ReleaseSRWLockExclusive(&p->lock);
   return status;
}

static VkResult owner_map_gpu(void *context, uint64_t address, uint64_t offset, uint64_t bytes,
                              uint8_t kind, bool read_only, uint64_t reserved,
                              uint64_t reserved_size)
{
   auto p = static_cast<RuntimeResource *>(context);
   AcquireSRWLockExclusive(&p->lock);
   HRESULT hr = E_INVALIDARG;
   if (p->dead) {
      hr = D3DDDIERR_DEVICEREMOVED;
   } else if (!p->offered && bytes && !((address | offset | bytes | reserved | reserved_size) & 4095) &&
              /* Linear primaries/shared surfaces require pitch PTEs. Private
               * images and validated version-2 shared images use NVK's exact
               * block-linear kind when the image is bound to this backing. */
              (!kind || p->device_owned || p->tiled_shared) &&
              offset <= p->memory.size && bytes <= p->memory.size - offset &&
              address >= reserved && address - reserved <= reserved_size &&
              bytes <= reserved_size - (address - reserved)) {
      Mapping *mapping = p->mappings;
      while (mapping && (mapping->address != reserved || mapping->size != reserved_size))
         mapping = mapping->next;
      if (!mapping) {
         mapping = static_cast<Mapping *>(calloc(1, sizeof(*mapping)));
         if (!mapping) {
            vk_result(p, E_OUTOFMEMORY);
            ReleaseSRWLockExclusive(&p->lock);
            return VK_ERROR_OUT_OF_HOST_MEMORY;
         }
         mapping->address = reserved;
         mapping->size = reserved_size;
         mapping->next = p->mappings;
         p->mappings = mapping;
      }
      D3DDDI_MAPGPUVIRTUALADDRESS map = {};
      map.hPagingQueue = p->queue;
      map.BaseAddress = address;
      map.hAllocation = p->memory.allocation;
      map.OffsetInPages = offset / 4096;
      map.SizeInPages = bytes / 4096;
      map.Protection.Write = !read_only;
      map.DriverProtection = kind; // same PTE-kind contract as NVK's direct KMT allocator
      hr = rt_trace(p, 4, p->callbacks.pfnMapGpuVirtualAddressCb(p->device, &map), bytes);
      /* WDDM queues page-table updates with E_PENDING despite its failure
       * bit; the mapping is usable only after the paging fence. */
      if (SUCCEEDED(hr) || hr == E_PENDING) {
         if (map.VirtualAddress != address)
            hr = E_FAIL;
         else
            hr = rt_trace(p, 3, wait_paging(p->callbacks, p->device, p->fence, p->fence_value,
                                            map.PagingFenceValue), map.PagingFenceValue);
      }
   }
   VkResult status = vk_result(p, hr);
   ReleaseSRWLockExclusive(&p->lock);
   return status;
}

static HRESULT lock_cpu(RuntimeResource *p, void **address)
{
   if (p->dead)
      return D3DDDIERR_DEVICEREMOVED;
   if (p->offered)
      return DXGI_ERROR_INVALID_CALL;
   if (!p->cpu_locked) {
      D3DDDICB_LOCK2 lock = {};
      lock.hAllocation = p->memory.allocation;
      HRESULT hr = rt_trace(p, 7, p->callbacks.pfnLock2Cb(p->device, &lock));
      if (FAILED(hr))
         return hr;
      if (!lock.pData)
         return E_FAIL;
      p->cpu = lock.pData;
      p->cpu_locked = true;
   }
   *address = p->cpu;
   return S_OK;
}

static VkResult owner_map_cpu(void *context, void **address)
{
   auto p = static_cast<RuntimeResource *>(context);
   AcquireSRWLockExclusive(&p->lock);
   VkResult status = vk_result(p, lock_cpu(p, address));
   ReleaseSRWLockExclusive(&p->lock);
   return status;
}

static void owner_unmap_cpu(void *context)
{
   /* The lock is kept until the resource is destroyed: Lock2 pins nothing
    * on this adapter and the mapping is reused by the next reader. */
   (void)context;
}

HRESULT runtime_destroy(RuntimeResource *p)
{
   if (!p)
      return S_OK;
   AcquireSRWLockExclusive(&p->lock);
   HRESULT hr = p->cleanup_error;
   p->dead = true;
   /* Order matters: release this device's GPU addresses, drop the CPU lock,
    * and only then return the allocation to the runtime. */
   for (Mapping *mapping = p->mappings; mapping; mapping = mapping->next) {
      if (!mapping->freed) {
         HRESULT next = free_address(p, mapping->address, mapping->size);
         if (SUCCEEDED(hr))
            hr = next;
         mapping->freed = true;
      }
   }
   if (p->cpu_locked) {
      D3DDDICB_UNLOCK2 unlock = {};
      unlock.hAllocation = p->memory.allocation;
      HRESULT next = rt_trace(p, 8, p->callbacks.pfnUnlock2Cb(p->device, &unlock));
      if (SUCCEEDED(hr))
         hr = next;
      p->cpu_locked = false;
      p->cpu = nullptr;
   }
   if (p->associated) {
      D3DDDICB_DEALLOCATE deallocate = {};
      if (p->device_owned) {
         // A renamed buffer must not release another live allocation of the same resource.
         deallocate.NumAllocations = 1;
         deallocate.HandleList = &p->memory.allocation;
      } else {
         deallocate.hResource = p->runtime_resource;
      }
      HRESULT next = rt_trace(p, 6, p->callbacks.pfnDeallocateCb(p->device, &deallocate));
      if (SUCCEEDED(hr))
         hr = next;
      p->memory.allocation = 0;
      p->associated = false;
   }
   ReleaseSRWLockExclusive(&p->lock);
   owner_release(p);
   return hr;
}

void runtime_copy_callbacks(D3DDDI_DEVICECALLBACKS *out, const D3DDDI_DEVICECALLBACKS *in)
{
   *out = {};
   /* This GPU-VA driver requires WDDM2 runtime callbacks. A newer SDK can
    * append fields beyond that producer's table: sizeof(*in) is not its
    * negotiated byte length. Read only the callbacks actually used below. */
#define COPY(name) out->name = in->name
   COPY(pfnAllocateCb);
   COPY(pfnDeallocateCb);
   COPY(pfnSetPriorityCb);
   COPY(pfnQueryResidencyCb);
   COPY(pfnSetDisplayModeCb);
   COPY(pfnDestroyContextCb);
   COPY(pfnDestroySynchronizationObjectCb);
   COPY(pfnSetDisplayPrivateDriverFormatCb);
   COPY(pfnCreateSynchronizationObject2Cb);
   COPY(pfnMakeResidentCb);
   COPY(pfnEvictCb);
   COPY(pfnWaitForSynchronizationObjectFromCpuCb);
   COPY(pfnWaitForSynchronizationObjectFromGpuCb);
   COPY(pfnSignalSynchronizationObjectFromGpuCb);
   COPY(pfnCreatePagingQueueCb);
   COPY(pfnDestroyPagingQueueCb);
   COPY(pfnLock2Cb);
   COPY(pfnUnlock2Cb);
   COPY(pfnMapGpuVirtualAddressCb);
   COPY(pfnFreeGpuVirtualAddressCb);
   COPY(pfnCreateContextVirtualCb);
   COPY(pfnOfferAllocationsCb);
   COPY(pfnReclaimAllocations2Cb);
#undef COPY
}

HRESULT runtime_device_create(Device *dev)
{
   const auto &cb = dev->kt;
   const bool modern = dev->interfaceVersion == D3DWDDM2_0_DDI_INTERFACE_VERSION;
   if (!(modern ? dev->createContextVirtual != nullptr : cb.pfnCreateContextVirtualCb != nullptr) ||
       !cb.pfnDestroyContextCb || !cb.pfnAllocateCb ||
       !cb.pfnDeallocateCb || !cb.pfnCreatePagingQueueCb || !cb.pfnDestroyPagingQueueCb ||
       !cb.pfnMakeResidentCb || !cb.pfnMapGpuVirtualAddressCb || !cb.pfnFreeGpuVirtualAddressCb ||
       !cb.pfnWaitForSynchronizationObjectFromCpuCb || !cb.pfnLock2Cb || !cb.pfnUnlock2Cb)
      return DXGI_ERROR_UNSUPPORTED;
   /* NVK's kernel context is invisible to the D3D runtime. The runtime
    * needs a context of its own for implicit synchronization and Present. */
   HRESULT hr;
   HANDLE handle = nullptr;
   if (modern) {
      D3DWDDM2_0DDICB_CREATECONTEXTVIRTUAL context = {};
      context.EngineAffinity = 1;
      context.ContextTypeFlags = D3DWDDM2_0DDI_CONTEXTTYPE_3D | D3DWDDM2_0DDI_CONTEXTTYPE_COPY;
      hr = dev->createContextVirtual(dev->hRTCoreLayer, &context);
      handle = context.hContext;
   } else {
      D3DDDICB_CREATECONTEXTVIRTUAL context = {};
      context.EngineAffinity = 1;
      hr = cb.pfnCreateContextVirtualCb(dev->hRTDevice.handle, &context);
      handle = context.hContext;
   }
   if (SUCCEEDED(hr) && !handle)
      hr = E_FAIL;
   trace(TGD3D_TRACE_RUNTIME_CONTEXT, hr, 1, 0);
   if (FAILED(hr))
      return hr;
   dev->hContext = handle;

   D3DDDICB_CREATEPAGINGQUEUE queue = {};
   queue.Priority = D3DDDI_PAGINGQUEUE_PRIORITY_NORMAL;
   hr = cb.pfnCreatePagingQueueCb(dev->hRTDevice.handle, &queue);
   if (SUCCEEDED(hr)) {
      /* Retain each successfully returned handle for CreateDevice's unwind,
       * even if the callback returned an incomplete success payload. */
      dev->pagingQueue = queue.hPagingQueue;
      dev->pagingFence = queue.hSyncObject;
      dev->pagingFenceValue = static_cast<const volatile UINT64 *>(queue.FenceValueCPUVirtualAddress);
      if (!queue.hPagingQueue || !queue.hSyncObject) hr = E_FAIL;
   }
   trace(TGD3D_TRACE_RUNTIME_CONTEXT, hr, 2, queue.hPagingQueue);
   if (FAILED(hr))
      return hr;
   return S_OK;
}

void runtime_device_destroy(Device *dev)
{
   if (dev->pagingQueue) {
      D3DDDI_DESTROYPAGINGQUEUE queue = {};
      queue.hPagingQueue = dev->pagingQueue;
      dev->kt.pfnDestroyPagingQueueCb(dev->hRTDevice.handle, &queue);
      dev->pagingQueue = 0;
   }
   if (dev->hContext) {
      D3DDDICB_DESTROYCONTEXT context = {};
      context.hContext = dev->hContext;
      HRESULT hr = dev->kt.pfnDestroyContextCb(dev->hRTDevice.handle, &context);
      trace(TGD3D_TRACE_RUNTIME_CONTEXT, hr, 6, 0);
      dev->hContext = nullptr;
   }
}

static HRESULT create_owner(Device *dev, D3D10DDI_HRTRESOURCE runtime, RuntimeResource **out)
{
   if (!runtime.handle || !dev->pagingQueue)
      return DXGI_ERROR_UNSUPPORTED;
   auto p = static_cast<RuntimeResource *>(calloc(1, sizeof(RuntimeResource)));
   if (!p)
      return E_OUTOFMEMORY;
   p->refs = 1;
   p->device = dev->hRTDevice.handle;
   p->runtime_resource = runtime.handle;
   p->callbacks = dev->kt;
   p->queue = dev->pagingQueue;
   p->fence = dev->pagingFence;
   p->fence_value = dev->pagingFenceValue;
   p->memory.adapter_luid = dev->adapter->luid;
   InitializeSRWLock(&p->lock);
   *out = p;
   return S_OK;
}

HRESULT runtime_make_resident(Device *dev, RuntimeResource *p)
{
   if (!p) return S_OK; // internal Vulkan memory has its own owner
   if (!dev || p->device != dev->hRTDevice.handle || p->queue != dev->pagingQueue)
      return E_INVALIDARG;
   bool final_attempt = false;
retry:
   AcquireSRWLockExclusive(&p->lock);
   HRESULT hr = S_OK;
   if (p->dead || !p->memory.allocation || p->offered) hr = DXGI_ERROR_INVALID_CALL;
   else if (!p->resident) {
      if (!p->residency_requested) {
         D3DDDI_MAKERESIDENT resident = {};
         resident.hPagingQueue = p->queue;
         resident.NumAllocations = 1;
         resident.AllocationList = &p->memory.allocation;
         resident.Flags.CantTrimFurther = final_attempt;
         hr = p->callbacks.pfnMakeResidentCb(p->device, &resident);
         rt_trace(p, 2, hr, resident.PagingFenceValue);
         if (hr == E_OUTOFMEMORY && !final_attempt) {
            /* MakeResident is atomic on failure: no residency reference was
             * acquired. Drop the owner lock before walking other resources
             * (or retiring completed caches), then retry only after trimming.
             * The adapter's 512 MiB ceiling is not this process's VidMm budget.
             */
            const uint64_t requested = resident.NumBytesToTrim;
            ReleaseSRWLockExclusive(&p->lock);
            uint64_t released = 0;
            hr = trim_residency_for_allocation(dev, p, requested, &released);
            if (FAILED(hr)) return hr;
            final_attempt = released == 0;
            /* Never force MustSucceed for an ordinary resource creation.
             * An irreducible failure propagates to the caller; no GPU work
             * may reference that backing until residency really succeeds. */
            goto retry;
         }
         if (SUCCEEDED(hr) || hr == E_PENDING) {
            p->residency_requested = true;
            p->residency_fence = resident.PagingFenceValue;
            hr = S_OK;
         }
      }
      if (SUCCEEDED(hr))
         hr = rt_trace(p, 3, wait_paging(p->callbacks, p->device, p->fence, p->fence_value,
                                         p->residency_fence), p->residency_fence);
      if (SUCCEEDED(hr)) p->resident = true;
   }
   ReleaseSRWLockExclusive(&p->lock);
   return hr;
}

HRESULT runtime_evict(RuntimeResource *p, uint64_t *bytes, uint64_t *remaining_budget)
{
   *bytes = 0;
   if (remaining_budget) *remaining_budget = 0;
   if (!p) return S_OK;
   AcquireSRWLockExclusive(&p->lock);
   HRESULT hr = S_OK;
   /* Scanout can outlive rendering completion. Its residency remains tied to
    * the primary/runtime lifetime, not the rendering queue's trim cutoff. */
   if (!p->dead && p->resident && !p->primary && !p->offered) {
      if (!p->callbacks.pfnEvictCb) hr = DXGI_ERROR_UNSUPPORTED;
      else {
         D3DDDICB_EVICT evict = {};
         evict.NumAllocations = 1;
         evict.AllocationList = &p->memory.allocation;
         hr = rt_trace(p, 13, p->callbacks.pfnEvictCb(p->device, &evict), p->memory.size);
         if (SUCCEEDED(hr)) {
            p->resident = p->residency_requested = false;
            p->residency_fence = 0;
            *bytes = p->memory.size;
            if (remaining_budget) *remaining_budget = evict.NumBytesToTrim;
         }
      }
   }
   ReleaseSRWLockExclusive(&p->lock);
   return hr;
}

static HRESULT make_ready(Device *dev, RuntimeResource *p, uint64_t bytes)
{
   HRESULT hr = runtime_make_resident(dev, p);
   if (FAILED(hr))
      return hr;
   p->memory.version = WSI_WIN32_MEMORY_VERSION;
   p->memory.size = bytes;
   p->memory.context = p;
   p->memory.retain = owner_retain;
   p->memory.release = owner_release;
   p->memory.map_gpu = owner_map_gpu;
   p->memory.free_gpu = owner_free_gpu;
   p->memory.map_cpu = owner_map_cpu;
   p->memory.unmap_cpu = owner_unmap_cpu;
   return S_OK;
}

HRESULT runtime_private_create(Device *dev, D3D10DDI_HRTRESOURCE resource,
                                uint64_t bytes, RuntimeResource **out)
{
   *out = nullptr;
   if (!bytes || bytes > TG_GPU_MEMORY_BUDGET_BYTES)
      return E_OUTOFMEMORY;
   bytes = (bytes + 4095) & ~UINT64_C(4095);
   RuntimeResource *p = nullptr;
   HRESULT hr = create_owner(dev, resource, &p);
   if (FAILED(hr)) return hr;
   p->device_owned = true;
   tg_gpu_allocation_request request = {
      TG_GPU_ALLOCATION_ABI, TG_GPU_ALLOCATION_CPU_VISIBLE, bytes};
   D3DDDI_ALLOCATIONINFO2 info = {};
   info.pPrivateDriverData = &request;
   info.PrivateDriverDataSize = sizeof(request);
   D3DDDICB_ALLOCATE allocate = {};
   /* Private application backing belongs to the runtime device. Keeping it
    * independent of hRTResource allows renames to retire after the resource
    * object's lifetime; shared/primary resources keep their existing owner. */
   allocate.NumAllocations = 1;
   allocate.pAllocationInfo2 = &info;
   hr = p->callbacks.pfnAllocateCb(p->device, &allocate);
   rt_trace(p, 1, hr, bytes);
   if (SUCCEEDED(hr)) {
      p->memory.allocation = info.hAllocation;
      p->associated = info.hAllocation != 0;
      hr = make_ready(dev, p, bytes);
   }
   if (FAILED(hr)) { runtime_destroy(p); return hr; }
   *out = p;
   return S_OK;
}

bool runtime_requires_resource_lifetime(const RuntimeResource *p)
{
   return p && !p->device_owned;
}

bool runtime_is_offered(const RuntimeResource *p) { return p && p->offered; }

static HRESULT validate_residency_owners(Device *dev, RuntimeResource *const *resources,
                                         UINT count, bool offered)
{
   if (count && !resources) return E_INVALIDARG;
   for (UINT i = 0; i < count; ++i) {
      const RuntimeResource *p = resources[i];
      if (!p || p->dead || !p->associated || !p->memory.allocation ||
          p->device != dev->hRTDevice.handle || p->queue != dev->pagingQueue || p->offered != offered)
         return E_INVALIDARG;
   }
   return S_OK;
}

HRESULT runtime_offer(Device *dev, RuntimeResource *const *resources, UINT count,
                       D3DDDI_OFFER_PRIORITY priority)
{
   if (priority < D3DDDI_OFFER_PRIORITY_LOW || priority > D3DDDI_OFFER_PRIORITY_HIGH)
      return E_INVALIDARG;
   HRESULT hr = validate_residency_owners(dev, resources, count, false);
   if (FAILED(hr) || !count) return hr;
   if (!dev->kt.pfnOfferAllocationsCb) return DXGI_ERROR_UNSUPPORTED;
   std::unique_ptr<D3DKMT_HANDLE[]> handles(new (std::nothrow) D3DKMT_HANDLE[count]);
   if (!handles) return E_OUTOFMEMORY;
   UINT unique = 0;
   for (UINT i = 0; i < count; ++i) {
      RuntimeResource *p = resources[i];
      UINT j = 0;
      while (j < unique && handles[j] != p->memory.allocation) ++j;
      if (j != unique) continue;
      handles[unique++] = p->memory.allocation;
      AcquireSRWLockExclusive(&p->lock);
      if (p->cpu_locked) {
         D3DDDICB_UNLOCK2 unlock = {};
         unlock.hAllocation = p->memory.allocation;
         hr = rt_trace(p, 8, p->callbacks.pfnUnlock2Cb(p->device, &unlock));
         if (SUCCEEDED(hr)) { p->cpu_locked = false; p->cpu = nullptr; }
      }
      ReleaseSRWLockExclusive(&p->lock);
      if (FAILED(hr)) return hr;
   }
   D3DDDICB_OFFERALLOCATIONS offer = {};
   offer.HandleList = handles.get(); offer.NumAllocations = unique; offer.Priority = priority;
   /* Caller already flushed references and queued the runtime GPU gate.
    * VidMm defers discarding until pending GPU work is complete. */
   hr = dev->kt.pfnOfferAllocationsCb(dev->hRTDevice.handle, &offer);
   for (UINT i = 0; i < count; ++i) {
      RuntimeResource *p = resources[i];
      rt_trace(p, 11, hr, priority);
      if (SUCCEEDED(hr)) { p->offered = true; p->reclaim_pending = false; }
   }
   return hr;
}

HRESULT runtime_reclaim(Device *dev, RuntimeResource *const *resources, UINT count, BOOL *discarded)
{
   HRESULT hr = validate_residency_owners(dev, resources, count, true);
   if (FAILED(hr) || !count) return hr;
   if (!dev->kt.pfnReclaimAllocations2Cb) return DXGI_ERROR_UNSUPPORTED;
   std::unique_ptr<D3DKMT_HANDLE[]> handles(new (std::nothrow) D3DKMT_HANDLE[count]);
   std::unique_ptr<BOOL[]> result(new (std::nothrow) BOOL[count]());
   if (!handles || !result) return E_OUTOFMEMORY;
   UINT unique = 0;
   for (UINT i = 0; i < count; ++i) {
      RuntimeResource *p = resources[i];
      if (p->reclaim_pending) continue; // retry only its paging wait, not a second reclaim
      UINT j = 0;
      while (j < unique && handles[j] != p->memory.allocation) ++j;
      if (j == unique) handles[unique++] = p->memory.allocation;
   }
   if (unique) {
      D3DDDICB_RECLAIMALLOCATIONS2 reclaim = {};
      reclaim.PagingQueue = dev->pagingQueue;
      reclaim.NumAllocations = unique; reclaim.HandleList = handles.get();
      reclaim.pDiscarded = result.get();
      hr = dev->kt.pfnReclaimAllocations2Cb(dev->hRTDevice.handle, &reclaim);
      if (FAILED(hr) && hr != E_PENDING) return hr;
      for (UINT i = 0; i < count; ++i) {
         RuntimeResource *p = resources[i];
         if (p->reclaim_pending) continue;
         UINT j = 0;
         while (j < unique && handles[j] != p->memory.allocation) ++j;
         p->reclaim_pending = true;
         p->reclaim_fence = reclaim.PagingFenceValue;
         p->discarded = result[j] != FALSE;
         rt_trace(p, 12, hr, reclaim.PagingFenceValue);
      }
   }
   uint64_t fence = 0;
   for (UINT i = 0; i < count; ++i)
      if (resources[i]->reclaim_fence > fence) fence = resources[i]->reclaim_fence;
   hr = wait_paging(dev->kt, dev->hRTDevice.handle, dev->pagingFence, dev->pagingFenceValue, fence);
   if (FAILED(hr)) return hr; // retained receipt permits a retry without inventing completion
   for (UINT i = 0; i < count; ++i) {
      RuntimeResource *p = resources[i];
      if (discarded) discarded[i] = p->discarded;
      p->offered = p->reclaim_pending = false;
      rt_trace(p, 13, S_OK, fence);
   }
   return S_OK;
}

/* The KMD owns the storage envelope; the trailing pointer-free payload is
 * this driver's image description. OpenResource receives the same bytes. */
struct SharedMetadata {
   tg_gpu_resource_request storage;
   uint32_t magic, version;
   RuntimeSharedDescription description;
   uint32_t reserved[2];
};
static_assert(sizeof(SharedMetadata) == 72, "shared image metadata ABI");

static HRESULT shared_layout(const RuntimeSharedDescription &d,
                             tg_gpu_linear_allocation_request *allocation)
{
   if (!d.width || !d.height || d.usage != D3D10_DDI_USAGE_DEFAULT ||
       (d.bind_flags & ~(D3D10_DDI_BIND_SHADER_RESOURCE | D3D10_DDI_BIND_RENDER_TARGET |
                         D3D10_DDI_BIND_PRESENT)))
      return DXGI_ERROR_UNSUPPORTED;
   uint32_t format;
   switch (d.format) {
   case DXGI_FORMAT_B8G8R8A8_UNORM: format = D3DDDIFMT_A8R8G8B8; break;
   case DXGI_FORMAT_R8G8B8A8_UNORM: format = D3DDDIFMT_A8B8G8R8; break;
   default: return DXGI_ERROR_UNSUPPORTED;
   }
   /* 128-byte rows let the GPU render to the linear surface directly and
    * sample it through a pitch texture header; four padding rows keep the
    * last block of a tiled copy inside the allocation. */
   const uint64_t pitch = ((uint64_t)d.width * 4 + 127) & ~UINT64_C(127);
   const uint64_t rows = ((uint64_t)d.height + 3) & ~UINT64_C(3);
   if (pitch > TG_GPU_MEMORY_BUDGET_BYTES || rows > TG_GPU_MEMORY_BUDGET_BYTES / pitch)
      return D3DDDIERR_OUTOFVIDEOMEMORY;
   *allocation = {
      {TG_GPU_ALLOCATION_ABI, TG_GPU_ALLOCATION_CPU_VISIBLE | TG_GPU_ALLOCATION_LINEAR_SURFACE,
       (pitch * rows + 4095) & ~UINT64_C(4095)},
      format, d.width, d.height, (UINT)pitch};
   return S_OK;
}

static bool nonzero_uuid(const uint8_t *uuid)
{
   uint8_t bits = 0;
   for (UINT i = 0; i < VK_UUID_SIZE; ++i) bits |= uuid[i];
   return bits != 0;
}

HRESULT runtime_shared_tiled_validate(Device *dev, const Resource *res)
{
   if (!dev || !res || !dev->nvk || !dev->caps || !dev->adapter ||
       dev->interfaceVersion < D3DWDDM2_0_DDI_INTERFACE_VERSION || !dev->adapter->luid ||
       !nonzero_uuid(dev->caps->deviceUUID) || !nonzero_uuid(dev->caps->driverUUID) ||
       res->dim != D3D10DDIRESOURCE_TEXTURE2D || res->primary || res->depth != 1 ||
       res->usage != D3D10_DDI_USAGE_DEFAULT || res->mapFlags || res->mips != 1 ||
       !res->width || !res->height || !res->arraySize || res->arraySize > 2 ||
       res->width > dev->caps->maxImageDimension2D || res->height > dev->caps->maxImageDimension2D ||
       res->arraySize > dev->caps->maxImageArrayLayers ||
       !(res->misc & D3D10_DDI_RESOURCE_MISC_SHARED) ||
       (res->misc & ~(D3D10_DDI_RESOURCE_MISC_SHARED | D3D10_DDI_RESOURCE_MISC_DISCARD_ON_PRESENT)) ||
       (res->bind & ~(D3D10_DDI_BIND_SHADER_RESOURCE | D3D10_DDI_BIND_RENDER_TARGET | D3D10_DDI_BIND_PRESENT)))
      return DXGI_ERROR_UNSUPPORTED;
   switch (res->format) {
   case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
   case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
   case DXGI_FORMAT_B8G8R8X8_UNORM: case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
   case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R16G16B16A16_FLOAT:
      return S_OK;
   default: return DXGI_ERROR_UNSUPPORTED;
   }
}

static VkImageUsageFlags shared_image_usage(UINT bind)
{
   VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
   if (bind & (D3D10_DDI_BIND_SHADER_RESOURCE | D3D10_DDI_BIND_RENDER_TARGET | D3D10_DDI_BIND_PRESENT))
      usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
   if (bind & D3D10_DDI_BIND_RENDER_TARGET) usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
   return usage;
}

static VkFormat shared_srgb_twin(VkFormat format)
{
   switch (format) {
   case VK_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_SRGB;
   case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
   case VK_FORMAT_B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_SRGB;
   case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
   default: return VK_FORMAT_UNDEFINED;
   }
}

static VkImageCreateFlags shared_image_flags(const RuntimeSharedDescription &d)
{
   const bool mutableView = (d.bind_flags & (D3D10_DDI_BIND_RENDER_TARGET | D3D10_DDI_BIND_PRESENT)) &&
      shared_srgb_twin(format_info(d.format)->vk) != VK_FORMAT_UNDEFINED;
   return VK_IMAGE_CREATE_ALIAS_BIT | (mutableView ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0);
}

static HRESULT tiled_metadata_validate(Device *dev, const SharedTiledMetadata &m)
{
   Resource res;
   res.dim = D3D10DDIRESOURCE_TEXTURE2D; res.usage = m.description.usage;
   res.bind = m.description.bind_flags; res.misc = D3D10_DDI_RESOURCE_MISC_SHARED;
   res.width = m.description.width; res.height = m.description.height;
   res.format = m.description.format; res.arraySize = m.array_size;
   HRESULT hr = runtime_shared_tiled_validate(dev, &res);
   if (FAILED(hr)) return hr;
   const auto &s = m.storage;
   const FormatInfo *format = format_info(res.format);
   const UINT memoryTypes = dev->caps->memory.memoryTypeCount >= 32 ? UINT_MAX :
      (1u << dev->caps->memory.memoryTypeCount) - 1;
   if (m.magic != shared_magic || m.version != 2 || m.description.pitch ||
       m.reserved[0] || m.reserved[1] || s.reserved[0] || s.reserved[1] ||
       s.version != TG_GPU_RESOURCE_ABI || s.header_bytes != sizeof(s) || s.allocation_count != 1 ||
       s.driver_data_bytes != sizeof(m) - sizeof(s) ||
       !m.image_bytes || m.image_bytes > TG_GPU_MEMORY_BUDGET_BYTES ||
       m.image_bytes < (uint64_t)res.width * res.height * res.arraySize * format->bytes ||
       !m.alignment || (m.alignment & (m.alignment - 1)) || m.alignment > TG_GPU_MEMORY_BUDGET_BYTES ||
       !m.memory_types || (m.memory_types & ~memoryTypes) ||
       s.allocation_bytes != ((m.image_bytes + 4095) & ~UINT64_C(4095)) ||
       m.vk_format != format->vk || m.image_usage != shared_image_usage(res.bind) ||
       m.image_flags != shared_image_flags(m.description) ||
       m.adapter_luid != dev->adapter->luid || m.driver_version != dev->caps->driverVersion ||
       memcmp(m.device_uuid, dev->caps->deviceUUID, VK_UUID_SIZE) ||
       memcmp(m.driver_uuid, dev->caps->driverUUID, VK_UUID_SIZE))
      return DXGI_ERROR_UNSUPPORTED;
   return S_OK;
}

HRESULT runtime_shared_tiled_check(Device *dev, const RuntimeResource *p,
   const VkImageCreateInfo &image, const VkMemoryRequirements &requirements)
{
   if (!p || !p->tiled_shared || p->dead) return DXGI_ERROR_UNSUPPORTED;
   const auto &m = p->tiled_layout;
   HRESULT hr = tiled_metadata_validate(dev, m);
   if (FAILED(hr)) return hr;
   if (image.imageType != VK_IMAGE_TYPE_2D || image.tiling != VK_IMAGE_TILING_OPTIMAL ||
       image.extent.width != m.description.width || image.extent.height != m.description.height ||
       image.extent.depth != 1 || image.mipLevels != 1 || image.arrayLayers != m.array_size ||
       image.samples != VK_SAMPLE_COUNT_1_BIT || image.sharingMode != VK_SHARING_MODE_EXCLUSIVE ||
       image.initialLayout != VK_IMAGE_LAYOUT_UNDEFINED ||
       image.format != m.vk_format || image.flags != m.image_flags || image.usage != m.image_usage ||
       requirements.size != m.image_bytes || requirements.alignment != m.alignment ||
       requirements.memoryTypeBits != m.memory_types)
      return DXGI_ERROR_UNSUPPORTED;
   /* Version 2 fixes the view-format list to the stored format and its
    * sRGB/linear twin. Changing that contract requires a metadata version
    * change; matching total bytes alone cannot prove alias compatibility. */
   if (image.flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) {
      const auto *list = static_cast<const VkImageFormatListCreateInfo *>(image.pNext);
      const VkFormat twin = shared_srgb_twin(image.format);
      if (!list || list->sType != VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO ||
          list->pNext || list->viewFormatCount != 2 || !list->pViewFormats ||
          !((list->pViewFormats[0] == image.format && list->pViewFormats[1] == twin) ||
            (list->pViewFormats[1] == image.format && list->pViewFormats[0] == twin)))
         return DXGI_ERROR_UNSUPPORTED;
   } else if (image.pNext) {
      return DXGI_ERROR_UNSUPPORTED;
   }
   return S_OK;
}

HRESULT runtime_shared_tiled_create(Device *dev, const Resource *res,
   const VkImageCreateInfo &image, const VkMemoryRequirements &requirements, RuntimeResource **out)
{
   *out = nullptr;
   HRESULT hr = runtime_shared_tiled_validate(dev, res);
   if (FAILED(hr)) return hr;
   if (!res->hRT.handle) return E_INVALIDARG;
   SharedTiledMetadata m = {};
   m.storage = {TG_GPU_RESOURCE_ABI, sizeof(m.storage), 1, sizeof(m) - sizeof(m.storage),
      (requirements.size + 4095) & ~UINT64_C(4095), {0, 0}};
   m.magic = shared_magic; m.version = 2;
   m.description = {res->width, res->height, 0, res->format, res->bind, res->usage};
   m.array_size = res->arraySize; m.vk_format = image.format;
   m.image_flags = image.flags; m.image_usage = image.usage;
   m.image_bytes = requirements.size; m.alignment = requirements.alignment;
   m.memory_types = requirements.memoryTypeBits; m.driver_version = dev->caps->driverVersion;
   memcpy(m.device_uuid, dev->caps->deviceUUID, VK_UUID_SIZE);
   memcpy(m.driver_uuid, dev->caps->driverUUID, VK_UUID_SIZE);
   m.adapter_luid = dev->adapter->luid;
   hr = tiled_metadata_validate(dev, m);
   if (FAILED(hr)) return hr;
   RuntimeResource *p = nullptr;
   hr = create_owner(dev, res->hRT, &p);
   if (FAILED(hr)) return hr;
   p->tiled_shared = true; p->tiled_layout = m;
   hr = runtime_shared_tiled_check(dev, p, image, requirements);
   if (FAILED(hr)) { runtime_destroy(p); return hr; }
   tg_gpu_allocation_request request = {TG_GPU_ALLOCATION_ABI, TG_GPU_ALLOCATION_CPU_VISIBLE,
      m.storage.allocation_bytes};
   D3DDDI_ALLOCATIONINFO2 info = {};
   info.pPrivateDriverData = &request; info.PrivateDriverDataSize = sizeof(request);
   D3DDDICB_ALLOCATE allocate = {};
   allocate.hResource = res->hRT.handle;
   allocate.pPrivateDriverData = &m; allocate.PrivateDriverDataSize = sizeof(m);
   allocate.NumAllocations = 1; allocate.pAllocationInfo2 = &info;
   hr = p->callbacks.pfnAllocateCb(p->device, &allocate);
   if (SUCCEEDED(hr)) {
      p->memory.allocation = info.hAllocation;
      p->associated = info.hAllocation != 0;
      hr = make_ready(dev, p, request.bytes);
   }
   if (FAILED(hr)) { runtime_destroy(p); return hr; }
   *out = p;
   return S_OK;
}

bool runtime_is_tiled_shared(const RuntimeResource *p) { return p && p->tiled_shared; }
UINT runtime_shared_layers(const RuntimeResource *p) { return runtime_is_tiled_shared(p) ? p->tiled_layout.array_size : 1; }

HRESULT runtime_shared_create(Device *dev, const D3D10DDIARG_CREATERESOURCE *desc,
                              D3D10DDI_HRTRESOURCE runtime, RuntimeResource **out, UINT *pitch)
{
   *out = nullptr;
   if (!desc->pMipInfoList || desc->pPrimaryDesc ||
       desc->ResourceDimension != D3D10DDIRESOURCE_TEXTURE2D ||
       !(desc->MiscFlags & D3D10_DDI_RESOURCE_MISC_SHARED) ||
       (desc->MiscFlags & ~(D3D10_DDI_RESOURCE_MISC_SHARED |
                            D3D10_DDI_RESOURCE_MISC_DISCARD_ON_PRESENT)) || desc->MapFlags ||
       desc->MipLevels != 1 || desc->ArraySize != 1 ||
       desc->SampleDesc.Count != 1 || desc->SampleDesc.Quality ||
       desc->pMipInfoList[0].TexelDepth != 1)
      return DXGI_ERROR_UNSUPPORTED;
   SharedMetadata metadata = {};
   auto &d = metadata.description;
   d = {desc->pMipInfoList[0].TexelWidth, desc->pMipInfoList[0].TexelHeight, 0,
        desc->Format, desc->BindFlags, desc->Usage};
   tg_gpu_linear_allocation_request request;
   HRESULT hr = shared_layout(d, &request);
   if (FAILED(hr))
      return hr;
   d.pitch = request.Pitch;
   metadata.magic = shared_magic;
   metadata.version = 1;
   metadata.storage = {TG_GPU_RESOURCE_ABI, sizeof(metadata.storage), 1,
      sizeof(metadata) - sizeof(metadata.storage), request.Buffer.bytes, {0, 0}};
   RuntimeResource *p = nullptr;
   hr = create_owner(dev, runtime, &p);
   if (FAILED(hr))
      return hr;
   D3DDDI_ALLOCATIONINFO2 info = {};
   info.pPrivateDriverData = &request;
   info.PrivateDriverDataSize = sizeof(request);
   D3DDDICB_ALLOCATE allocate = {};
   allocate.hResource = runtime.handle;
   allocate.pPrivateDriverData = &metadata;
   allocate.PrivateDriverDataSize = sizeof(metadata);
   allocate.NumAllocations = 1;
   allocate.pAllocationInfo2 = &info;
   hr = p->callbacks.pfnAllocateCb(p->device, &allocate);
   rt_trace(p, 1, hr, request.Buffer.bytes);
   if (FAILED(hr)) {
      runtime_destroy(p);
      return hr;
   }
   p->associated = true;
   p->memory.allocation = info.hAllocation;
   hr = make_ready(dev, p, request.Buffer.bytes);
   if (FAILED(hr)) {
      runtime_destroy(p);
      return hr;
   }
   *pitch = request.Pitch;
   *out = p;
   return S_OK;
}

HRESULT runtime_shared_open(Device *dev, const D3D10DDIARG_OPENRESOURCE *args,
                            D3D10DDI_HRTRESOURCE runtime, RuntimeResource **out,
                            RuntimeSharedDescription *description)
{
   *out = nullptr;
   *description = {};
   if (args->NumAllocations != 1 || !args->pOpenAllocationInfo)
      return DXGI_ERROR_UNSUPPORTED;
   /* VidMm's standard primary/shadow and our DXGI primary allocations have
    * no UMD resource payload. Their layout comes from the KMD's explicit
    * allocation ABI, not the desktop's current orientation. */
   const auto &info = args->pOpenAllocationInfo[0];
   if (!info.hAllocation || !info.pPrivateDriverData)
      return DXGI_ERROR_UNSUPPORTED;
   if (!args->PrivateDriverDataSize) {
      if (info.PrivateDriverDataSize != sizeof(tg_gpu_primary_allocation_request))
         return DXGI_ERROR_UNSUPPORTED;
      tg_gpu_primary_allocation_request a;
      memcpy(&a, info.pPrivateDriverData, sizeof(a));
      const bool primary = a.Buffer.flags & TG_GPU_ALLOCATION_DISPLAY_PRIMARY;
      /* The allocation's storage orientation is explicit. Standard GDI
       * primaries remain panel-oriented; a direct DXGI primary may use DC
       * column scan. ModeRotation is display-mode metadata, not a second copy. */
      const bool quarter_turn = a.Rotation == D3DDDI_ROTATION_90 || a.Rotation == D3DDDI_ROTATION_270;
      const bool valid_extent = primary ?
         (a.Width == (quarter_turn ? 1280u : 720u) && a.Height == (quarter_turn ? 720u : 1280u)) :
         ((a.Width == 720 && a.Height == 1280) || (a.Width == 1280 && a.Height == 720));
      if (a.Buffer.version != TG_GPU_ALLOCATION_ABI ||
          a.Buffer.flags != (TG_GPU_ALLOCATION_CPU_VISIBLE |
                             (primary ? TG_GPU_ALLOCATION_DISPLAY_PRIMARY : 0)) ||
          (a.Format != D3DDDIFMT_A8R8G8B8 && a.Format != D3DDDIFMT_X8R8G8B8) ||
          a.Rotation < D3DDDI_ROTATION_IDENTITY || a.Rotation > D3DDDI_ROTATION_270 ||
          (!primary && a.Rotation != D3DDDI_ROTATION_IDENTITY) ||
          a.ModeRotation > D3DDDI_ROTATION_270 || (!primary && a.ModeRotation) ||
          !valid_extent || a.Pitch != a.Width * 4 ||
          a.Buffer.bytes != UINT64_C(720) * 1280 * 4)
         return DXGI_ERROR_UNSUPPORTED;
      RuntimeResource *p = nullptr;
      HRESULT hr = create_owner(dev, runtime, &p);
      if (FAILED(hr))
         return hr;
      p->associated = true;
      p->primary = primary;
      if (primary)
         p->primary_layout = a;
      p->memory.allocation = info.hAllocation;
      hr = make_ready(dev, p, a.Buffer.bytes);
      if (FAILED(hr)) {
         runtime_destroy(p);
         return hr;
      }
      const UINT bind = D3D10_DDI_BIND_SHADER_RESOURCE | D3D10_DDI_BIND_RENDER_TARGET |
                        (primary ? D3D10_DDI_BIND_PRESENT : 0);
      *description = {a.Width, a.Height, a.Pitch,
         a.Format == D3DDDIFMT_X8R8G8B8 ? DXGI_FORMAT_B8G8R8X8_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM,
         bind, D3D10_DDI_USAGE_DEFAULT};
      *out = p;
      return S_OK;
   }
   if (!args->hKMResource.handle || !args->pPrivateDriverData)
      return DXGI_ERROR_UNSUPPORTED;
   if (args->PrivateDriverDataSize == sizeof(SharedTiledMetadata)) {
      SharedTiledMetadata metadata;
      memcpy(&metadata, args->pPrivateDriverData, sizeof(metadata));
      HRESULT hr = tiled_metadata_validate(dev, metadata);
      if (FAILED(hr)) return hr;
      const tg_gpu_allocation_request expected = {TG_GPU_ALLOCATION_ABI, TG_GPU_ALLOCATION_CPU_VISIBLE,
         metadata.storage.allocation_bytes};
      if (info.PrivateDriverDataSize != sizeof(expected) ||
          memcmp(info.pPrivateDriverData, &expected, sizeof(expected))) return DXGI_ERROR_UNSUPPORTED;
      RuntimeResource *p = nullptr;
      hr = create_owner(dev, runtime, &p);
      if (FAILED(hr)) return hr;
      p->associated = true; p->tiled_shared = true; p->tiled_layout = metadata;
      p->memory.allocation = info.hAllocation;
      hr = make_ready(dev, p, expected.bytes);
      if (FAILED(hr)) { runtime_destroy(p); return hr; }
      *description = metadata.description; *out = p;
      return S_OK;
   }
   if (args->PrivateDriverDataSize != sizeof(SharedMetadata)) return DXGI_ERROR_UNSUPPORTED;
   SharedMetadata metadata;
   memcpy(&metadata, args->pPrivateDriverData, sizeof(metadata));
   const auto &s = metadata.storage;
   if (metadata.magic != shared_magic || metadata.version != 1 ||
       metadata.reserved[0] || metadata.reserved[1] ||
       s.version != TG_GPU_RESOURCE_ABI || s.header_bytes != sizeof(s) ||
       s.allocation_count != 1 || s.driver_data_bytes != sizeof(metadata) - sizeof(s) ||
       s.reserved[0] || s.reserved[1])
      return DXGI_ERROR_UNSUPPORTED;
   tg_gpu_linear_allocation_request expected;
   HRESULT hr = shared_layout(metadata.description, &expected);
   if (FAILED(hr))
      return hr;
   if (s.allocation_bytes != expected.Buffer.bytes || metadata.description.pitch != expected.Pitch)
      return DXGI_ERROR_UNSUPPORTED;
   /* Only the documented INFO/INFO2 common prefix is read. */
   if (info.PrivateDriverDataSize != sizeof(expected))
      return DXGI_ERROR_UNSUPPORTED;
   tg_gpu_linear_allocation_request actual;
   memcpy(&actual, info.pPrivateDriverData, sizeof(actual));
   if (memcmp(&actual, &expected, sizeof(actual)))
      return DXGI_ERROR_UNSUPPORTED;
   RuntimeResource *p = nullptr;
   hr = create_owner(dev, runtime, &p);
   if (FAILED(hr))
      return hr;
   /* The runtime has already opened these handles for this device. Keep its
    * hRTResource for callbacks and teardown; nothing is allocated here and
    * the producer's handles are never used. */
   p->associated = true;
   p->memory.allocation = info.hAllocation;
   hr = make_ready(dev, p, expected.Buffer.bytes);
   if (FAILED(hr)) {
      runtime_destroy(p);
      return hr;
   }
   *description = metadata.description;
   *out = p;
   return S_OK;
}

HRESULT runtime_primary_create(Device *dev, const D3D10DDIARG_CREATERESOURCE *desc,
                               D3D10DDI_HRTRESOURCE runtime, RuntimeResource **out,
                               UINT *out_width, UINT *out_height, UINT *out_pitch)
{
   *out = nullptr;
   if (!desc->pPrimaryDesc || !desc->pMipInfoList)
      return DXGI_ERROR_UNSUPPORTED;
   const auto &primary = *desc->pPrimaryDesc;
   const UINT mode_rotation = primary.ModeDesc.Rotation == DXGI_DDI_MODE_ROTATION_UNSPECIFIED ?
      DXGI_DDI_MODE_ROTATION_IDENTITY : primary.ModeDesc.Rotation;
   if (mode_rotation > DXGI_DDI_MODE_ROTATION_ROTATE270)
      return E_INVALIDARG;
   /* NONPREROTATED already contains panel-oriented pixels. Otherwise pass
    * the explicit storage orientation to the DC; no pre-rotation copy or
    * NO_SCANOUT fallback is needed for the supported full-panel primary. */
   const UINT storage_rotation = primary.Flags & DXGI_DDI_PRIMARY_NONPREROTATED ?
      DXGI_DDI_MODE_ROTATION_IDENTITY : mode_rotation;
   const bool quarter_turn = storage_rotation == DXGI_DDI_MODE_ROTATION_ROTATE90 ||
      storage_rotation == DXGI_DDI_MODE_ROTATION_ROTATE270;
   const UINT width = quarter_turn ? 1280 : 720, height = quarter_turn ? 720 : 1280;
   if (desc->ResourceDimension != D3D10DDIRESOURCE_TEXTURE2D ||
       (desc->Format != DXGI_FORMAT_B8G8R8A8_UNORM &&
        desc->Format != DXGI_FORMAT_B8G8R8X8_UNORM) || desc->MipLevels != 1 ||
       desc->ArraySize != 1 || desc->SampleDesc.Count != 1 || desc->SampleDesc.Quality ||
       primary.VidPnSourceId != 0 || mode_rotation > DXGI_DDI_MODE_ROTATION_ROTATE270 ||
       (primary.Flags & ~(DXGI_DDI_PRIMARY_OPTIONAL | DXGI_DDI_PRIMARY_NONPREROTATED)) ||
       desc->pMipInfoList[0].TexelWidth != width || desc->pMipInfoList[0].TexelHeight != height ||
       !((primary.ModeDesc.Width == width && primary.ModeDesc.Height == height) ||
         ((mode_rotation == DXGI_DDI_MODE_ROTATION_ROTATE90 ||
           mode_rotation == DXGI_DDI_MODE_ROTATION_ROTATE270) &&
          primary.ModeDesc.Width == height && primary.ModeDesc.Height == width)))
      return DXGI_ERROR_UNSUPPORTED;
   if (!dev->kt.pfnSetDisplayModeCb)
      return DXGI_ERROR_UNSUPPORTED;
   RuntimeResource *p = nullptr;
   HRESULT hr = create_owner(dev, runtime, &p);
   if (FAILED(hr))
      return hr;
   p->primary = true;
   const UINT pitch = width * 4;
   tg_gpu_primary_allocation_request request = {
      {TG_GPU_ALLOCATION_ABI, TG_GPU_ALLOCATION_CPU_VISIBLE | TG_GPU_ALLOCATION_DISPLAY_PRIMARY,
       (uint64_t)pitch * height},
      desc->Format == DXGI_FORMAT_B8G8R8X8_UNORM ? D3DDDIFMT_X8R8G8B8 : D3DDDIFMT_A8R8G8B8,
      {primary.ModeDesc.RefreshRate.Numerator, primary.ModeDesc.RefreshRate.Denominator},
      storage_rotation, width, height, pitch, mode_rotation};
   p->primary_layout = request;
   D3DDDI_ALLOCATIONINFO2 info = {};
   info.pPrivateDriverData = &request;
   info.PrivateDriverDataSize = sizeof(request);
   info.VidPnSourceId = primary.VidPnSourceId;
   info.Flags.Primary = true;
   D3DDDICB_ALLOCATE allocate = {};
   allocate.hResource = runtime.handle;
   allocate.NumAllocations = 1;
   allocate.pAllocationInfo2 = &info;
   hr = p->callbacks.pfnAllocateCb(p->device, &allocate);
   rt_trace(p, 1, hr, request.Buffer.bytes);
   if (FAILED(hr)) {
      runtime_destroy(p);
      return hr;
   }
   p->associated = true;
   p->memory.allocation = info.hAllocation;
   hr = make_ready(dev, p, request.Buffer.bytes);
   if (FAILED(hr)) {
      runtime_destroy(p);
      return hr;
   }
   *out_width = width;
   *out_height = height;
   *out_pitch = pitch;
   *out = p;
   return S_OK;
}

HRESULT runtime_set_display_mode(RuntimeResource *p, Device *dev, UINT subresource)
{
   if (!p || !p->primary)
      return DXGI_ERROR_UNSUPPORTED;
   if (p->device != dev->hRTDevice.handle || subresource)
      return E_INVALIDARG;
   AcquireSRWLockExclusive(&p->lock);
   D3DDDICB_SETDISPLAYMODE mode = {};
   mode.hPrimaryAllocation = p->memory.allocation;
   HRESULT hr = p->dead ? D3DDDIERR_DEVICEREMOVED :
      p->callbacks.pfnSetDisplayModeCb(p->device, &mode);
   const UINT format = p->primary_layout.Rotation - D3DDDI_ROTATION_IDENTITY;
   if (hr == D3DDDIERR_INCOMPATIBLEPRIVATEFORMAT && format < 4 &&
       mode.PrivateDriverFormatAttribute < 4 && mode.PrivateDriverFormatAttribute != format &&
       p->callbacks.pfnSetDisplayPrivateDriverFormatCb) {
      /* The returned attribute describes the current source, not this
       * allocation. Negotiate the immutable storage layout with the KMD;
       * ModeRotation may differ for a NONPREROTATED primary. */
      const D3DDDICB_SETDISPLAYPRIVATEDRIVERFORMAT change = {0, format};
      hr = p->callbacks.pfnSetDisplayPrivateDriverFormatCb(p->device, &change);
      if (SUCCEEDED(hr)) {
         mode = {};
         mode.hPrimaryAllocation = p->memory.allocation;
         hr = p->callbacks.pfnSetDisplayModeCb(p->device, &mode);
      }
      /* Retry once. A failed retry is not a completed modeset. Leave the
       * negotiated source format intact: blindly restoring an old format
       * could overwrite a newer owner's binding. The next primary must
       * negotiate its own layout through the same runtime contract. */
   }
   ReleaseSRWLockExclusive(&p->lock);
   return hr;
}

bool runtime_is_primary(const RuntimeResource *p) { return p && p->primary; }
bool runtime_primary_compatible(const RuntimeResource *source, const RuntimeResource *target)
{
   if (!runtime_is_primary(source) || !runtime_is_primary(target) ||
       !runtime_allocation(source) || !runtime_allocation(target) || source->device != target->device)
      return false;
   const auto &s = source->primary_layout, &t = target->primary_layout;
   /* Identity and 180-degree layouts share dimensions but require different
    * scan directions. Compare the immutable allocation metadata as well. */
   return s.Width == t.Width && s.Height == t.Height && s.Pitch == t.Pitch &&
          s.Format == t.Format && s.Rotation == t.Rotation;
}
D3DKMT_HANDLE runtime_allocation(const RuntimeResource *p)
{
   return p && !p->dead && p->associated ? p->memory.allocation : 0;
}

void runtime_rebind(RuntimeResource *p, D3D10DDI_HRTRESOURCE runtime)
{
   /* Identity rotation moves storage between logical resources. The
    * allocation is deallocated through the slot that now holds it. */
   AcquireSRWLockExclusive(&p->lock);
   p->runtime_resource = runtime.handle;
   ReleaseSRWLockExclusive(&p->lock);
}

static HRESULT allocate_import(Device *dev, RuntimeResource *p, const VkMemoryRequirements &req,
                               VkMemoryPropertyFlags properties, VkDeviceMemory *memory)
{
   if (req.size > p->memory.size)
      return DXGI_ERROR_UNSUPPORTED;
   VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
   bool imported = false;
   struct wsi_memory_allocate_info owner = {};
   VkImportMemoryHostPointerInfoEXT host = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT};
   const VulkanAdapter &va = dev->adapter->vk;
   if (dev->nvk) {
      /* The driver maps the runtime's allocation at an address it reserves
       * and retains the owner; nothing is allocated. */
      owner.sType = VK_STRUCTURE_TYPE_WSI_MEMORY_ALLOCATE_INFO_MESA;
      owner.win32_memory = &p->memory;
      owner.win32_imported = &imported;
      alloc.pNext = &owner;
      alloc.allocationSize = req.size;
      alloc.memoryTypeIndex = va.findMemoryType(req.memoryTypeBits, properties);
   } else {
      /* Controlled test build: the same backing through its CPU mapping. */
      void *cpu = nullptr;
      AcquireSRWLockExclusive(&p->lock);
      HRESULT hr = lock_cpu(p, &cpu);
      ReleaseSRWLockExclusive(&p->lock);
      if (FAILED(hr))
         return hr;
      const VkDeviceSize align = va.caps.minHostPointerAlignment;
      if (!align || ((uintptr_t)cpu & (align - 1)) || (p->memory.size & (align - 1)))
         return DXGI_ERROR_UNSUPPORTED;
      host.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
      host.pHostPointer = cpu;
      alloc.pNext = &host;
      alloc.allocationSize = p->memory.size;
      uint32_t types = req.memoryTypeBits;
      if (dev->vk.GetMemoryHostPointerPropertiesEXT) {
         VkMemoryHostPointerPropertiesEXT props = {
            VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
         if (dev->vk.GetMemoryHostPointerPropertiesEXT(dev->vkdev, host.handleType, cpu,
                                                       &props) == VK_SUCCESS)
            types &= props.memoryTypeBits;
      }
      alloc.memoryTypeIndex = va.findMemoryType(types, properties);
      imported = true;
   }
   if (alloc.memoryTypeIndex == UINT32_MAX)
      return DXGI_ERROR_UNSUPPORTED;
   VkResult result = dev->vk.AllocateMemory(dev->vkdev, &alloc, nullptr, memory);
   if (result != VK_SUCCESS)
      return result == VK_ERROR_DEVICE_LOST ? D3DDDIERR_DEVICEREMOVED :
         FAILED(p->error) ? p->error : resource_result(result);
   /* An ICD that ignored the owner structure would have allocated unrelated
    * memory: the image would render, and nobody else would ever see it. */
   HRESULT hr = imported ? S_OK : DXGI_ERROR_UNSUPPORTED;
   if (FAILED(hr)) {
      dev->vk.FreeMemory(dev->vkdev, *memory, nullptr);
      *memory = VK_NULL_HANDLE;
      return hr;
   }
   return S_OK;
}

HRESULT runtime_bind_image(Device *dev, RuntimeResource *p, VkImage image, VkDeviceMemory *memory)
{
   VkMemoryRequirements req;
   dev->vk.GetImageMemoryRequirements(dev->vkdev, image, &req);
   HRESULT hr = allocate_import(dev, p, req, 0, memory);
   if (SUCCEEDED(hr)) {
      hr = resource_result(dev->vk.BindImageMemory(dev->vkdev, image, *memory, 0));
      if (FAILED(hr)) { dev->vk.FreeMemory(dev->vkdev, *memory, nullptr); *memory = VK_NULL_HANDLE; }
   }
   return hr;
}

HRESULT runtime_bind_buffer(Device *dev, RuntimeResource *p, VkBuffer buffer, VkDeviceMemory *memory)
{
   VkMemoryRequirements req;
   dev->vk.GetBufferMemoryRequirements(dev->vkdev, buffer, &req);
   HRESULT hr = allocate_import(dev, p, req,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, memory);
   if (SUCCEEDED(hr)) {
      hr = resource_result(dev->vk.BindBufferMemory(dev->vkdev, buffer, *memory, 0));
      if (FAILED(hr)) { dev->vk.FreeMemory(dev->vkdev, *memory, nullptr); *memory = VK_NULL_HANDLE; }
   }
   return hr;
}

}
