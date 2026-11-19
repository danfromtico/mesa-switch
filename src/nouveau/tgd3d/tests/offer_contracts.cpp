/* SPDX-License-Identifier: MIT
 * Production private-allocation and DXGI offer/reclaim code with CPU runtime
 * and Vulkan substitutes. No real device, GPU or display is accessed.
 */
#include "tgd3d.h"
#include "nouveau/vulkan/nvkmd/windows/allocation_abi.h"
#include "vulkan/util/vk_internal_exts.h"
#include "vulkan/util/vk_wsi_win32_memory.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <type_traits>

using namespace tgd3d;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%u: %s\n", __FILE__, __LINE__, #x); std::exit(1); } } while (0)
namespace {
template<typename T> T h(uintptr_t v) { return (T)v; }
template<typename T> struct Stub;
template<typename R, typename... A> struct Stub<R(VKAPI_PTR *)(A...)> {
   static R VKAPI_CALL call(A...) { if constexpr (!std::is_void_v<R>) return R{}; }
};
struct Allocation {
   std::vector<BYTE> cpu, old;
   bool offered = false;
   bool resident = false;
};
struct Memory {
   const wsi_win32_memory *owner;
   uint64_t address, extra = 0;
   bool mapped = false;
};
struct State {
   std::map<UINT, Allocation> allocations;
   std::map<uintptr_t, Memory> memory;
   std::map<uintptr_t, uint64_t> buffers;
   UINT nextAllocation = 0x500, nextMemory = 0x100, nextBuffer = 0x700;
   UINT offers = 0, reclaims = 0, deallocations = 0, pagingWaits = 0, renderWaits = 0;
   UINT locks = 0, unlocks = 0, unmaps = 0, kinds = 0, errors = 0;
   UINT residents = 0, evictions = 0;
   UINT residencyAttempts = 0, finalResidencyAttempts = 0;
   std::vector<uint64_t> residencyTrims;
   size_t nextResidencyTrim = 0;
   bool failResidency = false;
   uint64_t evictionBudget = 0;
   uint64_t paging = 0, nextPaging = 0, render = 0;
   bool failPaging = false;
   UINT discard = 0;
   HRESULT offerResult = S_OK;
   HRESULT evictResult = S_OK;
   std::vector<int> order;
} s;

void init(Device *dev, Adapter *adapter)
{
   s = {};
   dev->adapter = adapter; dev->nvk = true; dev->interfaceVersion = D3DWDDM1_3_DDI_INTERFACE_VERSION;
   dev->caps = &adapter->vk.caps; adapter->luid = 0x112233;
   adapter->vk.caps.memory.memoryTypeCount = 1;
   adapter->vk.caps.memory.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
   dev->hRTDevice.handle = h<HANDLE>(0x1000); dev->vkdev = h<VkDevice>(1); dev->queue = h<VkQueue>(2);
   dev->fence = h<VkSemaphore>(3); dev->ingress = h<VkSemaphore>(4);
   dev->runtimeFence = 0x90; dev->runtimeIngress = 0x91; dev->cmdPool = h<VkCommandPool>(5);
   dev->um.pfnSetErrorCb = [](D3D10DDI_HRTCORELAYER, HRESULT) APIENTRY { ++s.errors; };
#define X(n) dev->vk.n = Stub<PFN_vk##n>::call;
   TGD3D_VK_DEVICE_FUNCS(X)
   TGD3D_VK_OPTIONAL_DEVICE_FUNCS(X)
#undef X
   auto &cb = dev->kt;
   cb.pfnCreateContextVirtualCb = [](HANDLE d, D3DDDICB_CREATECONTEXTVIRTUAL *a) APIENTRY -> HRESULT {
      CHECK(d == h<HANDLE>(0x1000)); a->hContext = h<HANDLE>(0x2000); return S_OK;
   };
   cb.pfnDestroyContextCb = [](HANDLE, const D3DDDICB_DESTROYCONTEXT *) APIENTRY -> HRESULT { return S_OK; };
   cb.pfnCreatePagingQueueCb = [](HANDLE, D3DDDICB_CREATEPAGINGQUEUE *a) APIENTRY -> HRESULT {
      a->hPagingQueue = 0x40; a->hSyncObject = 0x41; a->FenceValueCPUVirtualAddress = &s.paging; return S_OK;
   };
   cb.pfnDestroyPagingQueueCb = [](HANDLE, const D3DDDI_DESTROYPAGINGQUEUE *) APIENTRY -> HRESULT { return S_OK; };
   cb.pfnAllocateCb = [](HANDLE d, D3DDDICB_ALLOCATE *a) APIENTRY -> HRESULT {
      CHECK(d == h<HANDLE>(0x1000) && !a->hResource && !a->pPrivateDriverData &&
            !a->PrivateDriverDataSize && a->NumAllocations == 1);
      auto &info = a->pAllocationInfo2[0];
      CHECK(info.PrivateDriverDataSize == sizeof(tg_gpu_allocation_request));
      const auto *r = static_cast<const tg_gpu_allocation_request *>(info.pPrivateDriverData);
      CHECK(r->version == TG_GPU_ALLOCATION_ABI && r->flags == TG_GPU_ALLOCATION_CPU_VISIBLE &&
            r->bytes && !(r->bytes & 4095));
      info.hAllocation = ++s.nextAllocation; s.allocations[info.hAllocation].cpu.resize(r->bytes);
      return S_OK;
   };
   cb.pfnDeallocateCb = [](HANDLE d, const D3DDDICB_DEALLOCATE *a) APIENTRY -> HRESULT {
      CHECK(d == h<HANDLE>(0x1000) && !a->hResource && a->NumAllocations == 1 && a->HandleList);
      for (const auto &m : s.memory) CHECK(m.second.owner->allocation != a->HandleList[0]);
      CHECK(s.allocations.erase(a->HandleList[0]) == 1); ++s.deallocations; return S_OK;
   };
   cb.pfnMakeResidentCb = [](HANDLE, D3DDDI_MAKERESIDENT *a) APIENTRY -> HRESULT {
      CHECK(a->hPagingQueue == 0x40 && a->NumAllocations == 1 && s.allocations.count(a->AllocationList[0]));
      auto &allocation = s.allocations.at(a->AllocationList[0]);
      CHECK(!allocation.resident && !a->Flags.MustSucceed && !a->Flags.Reserved);
      ++s.residencyAttempts; s.finalResidencyAttempts += a->Flags.CantTrimFurther;
      if (s.failResidency || s.nextResidencyTrim < s.residencyTrims.size()) {
         a->NumBytesToTrim = s.failResidency ? 4096 : s.residencyTrims[s.nextResidencyTrim++];
         return E_OUTOFMEMORY; // atomic failure: no residency reference acquired
      }
      allocation.resident = true; ++s.residents;
      a->PagingFenceValue = ++s.nextPaging; return E_PENDING;
   };
   cb.pfnEvictCb = [](HANDLE d, D3DDDICB_EVICT *a) APIENTRY -> HRESULT {
      CHECK(d == h<HANDLE>(0x1000) && a->NumAllocations == 1 && !a->Flags.Value);
      auto &allocation = s.allocations.at(a->AllocationList[0]);
      CHECK(allocation.resident && !allocation.offered);
      if (FAILED(s.evictResult)) return s.evictResult;
      allocation.resident = false; ++s.evictions;
      a->NumBytesToTrim = s.evictionBudget; return S_OK;
   };
   cb.pfnWaitForSynchronizationObjectFromCpuCb = [](HANDLE,
      const D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMCPU *a) APIENTRY -> HRESULT {
      CHECK(a->ObjectCount == 1 && a->ObjectHandleArray[0] == 0x41); ++s.pagingWaits;
      if (s.failPaging) { s.failPaging = false; return E_FAIL; }
      s.paging = a->FenceValueArray[0]; return S_OK;
   };
   cb.pfnMapGpuVirtualAddressCb = [](HANDLE, D3DDDI_MAPGPUVIRTUALADDRESS *a) APIENTRY -> HRESULT {
      CHECK(a->hPagingQueue == 0x40 && s.allocations.count(a->hAllocation) && a->Protection.Write);
      CHECK(a->DriverProtection == 0 || a->DriverProtection == 0xfe);
      s.kinds += a->DriverProtection != 0;
      a->VirtualAddress = a->BaseAddress; a->PagingFenceValue = ++s.nextPaging; return E_PENDING;
   };
   cb.pfnFreeGpuVirtualAddressCb = [](HANDLE, const D3DDDICB_FREEGPUVIRTUALADDRESS *a) APIENTRY -> HRESULT {
      CHECK(a->BaseAddress && a->Size && !(a->Size & 4095)); return S_OK;
   };
   cb.pfnLock2Cb = [](HANDLE, D3DDDICB_LOCK2 *a) APIENTRY -> HRESULT {
      auto &block = s.allocations.at(a->hAllocation); CHECK(!block.offered);
      a->pData = block.cpu.data(); ++s.locks; return S_OK;
   };
   cb.pfnUnlock2Cb = [](HANDLE, const D3DDDICB_UNLOCK2 *a) APIENTRY -> HRESULT {
      CHECK(s.allocations.count(a->hAllocation)); ++s.unlocks; return S_OK;
   };
   cb.pfnSignalSynchronizationObjectFromGpuCb = [](HANDLE,
      const D3DDDICB_SIGNALSYNCHRONIZATIONOBJECTFROMGPU *a) APIENTRY -> HRESULT {
      CHECK(a->hContext == h<HANDLE>(0x2000) && a->ObjectHandleArray[0] == 0x91);
      s.order.push_back(1); return S_OK;
   };
   cb.pfnWaitForSynchronizationObjectFromGpuCb = [](HANDLE,
      const D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMGPU *a) APIENTRY -> HRESULT {
      CHECK(a->hContext == h<HANDLE>(0x2000) && a->ObjectHandleArray[0] == 0x90);
      s.order.push_back(3); return S_OK;
   };
   cb.pfnOfferAllocationsCb = [](HANDLE d, const D3DDDICB_OFFERALLOCATIONS *a) APIENTRY -> HRESULT {
      CHECK(d == h<HANDLE>(0x1000) && !a->pResources && a->NumAllocations);
      s.order.push_back(4); ++s.offers;
      for (UINT i = 0; i < a->NumAllocations; ++i) {
         for (UINT j = 0; j < i; ++j) CHECK(a->HandleList[j] != a->HandleList[i]);
         for (const auto &m : s.memory)
            if (m.second.owner->allocation == a->HandleList[i]) CHECK(!m.second.mapped);
      }
      if (FAILED(s.offerResult)) return s.offerResult;
      for (UINT i = 0; i < a->NumAllocations; ++i) s.allocations.at(a->HandleList[i]).offered = true;
      return S_OK;
   };
   cb.pfnReclaimAllocations2Cb = [](HANDLE, D3DDDICB_RECLAIMALLOCATIONS2 *a) APIENTRY -> HRESULT {
      CHECK(a->PagingQueue == 0x40 && !a->pResources && a->pDiscarded); ++s.reclaims;
      for (UINT i = 0; i < a->NumAllocations; ++i) {
         auto &block = s.allocations.at(a->HandleList[i]); CHECK(block.offered);
         for (UINT j = 0; j < i; ++j) CHECK(a->HandleList[i] != a->HandleList[j]);
         a->pDiscarded[i] = a->HandleList[i] == s.discard;
         if (a->pDiscarded[i]) {
            CHECK(s.render); // discard case runs only after this fixture completed its render work
            block.old = std::move(block.cpu); block.cpu.assign(block.old.size(), 0xcd);
         }
         block.offered = false;
      }
      a->PagingFenceValue = ++s.nextPaging; return E_PENDING;
   };
   dev->vk.CreateBuffer = [](VkDevice, const VkBufferCreateInfo *a, const VkAllocationCallbacks *, VkBuffer *out) VKAPI_CALL {
      *out = h<VkBuffer>(++s.nextBuffer); s.buffers[(uintptr_t)*out] = a->size; return VK_SUCCESS;
   };
   dev->vk.GetBufferMemoryRequirements = [](VkDevice, VkBuffer b, VkMemoryRequirements *a) VKAPI_CALL {
      *a = {s.buffers.at((uintptr_t)b), 4096, 1};
   };
   dev->vk.CreateImage = [](VkDevice, const VkImageCreateInfo *a, const VkAllocationCallbacks *, VkImage *out) VKAPI_CALL {
      CHECK(a->tiling == VK_IMAGE_TILING_OPTIMAL && a->initialLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
            a->extent.width == 65 && a->extent.height == 17);
      *out = h<VkImage>(0x800); return VK_SUCCESS;
   };
   dev->vk.GetImageMemoryRequirements = [](VkDevice, VkImage, VkMemoryRequirements *a) VKAPI_CALL { *a = {16384, 4096, 1}; };
   dev->vk.AllocateMemory = [](VkDevice, const VkMemoryAllocateInfo *a, const VkAllocationCallbacks *, VkDeviceMemory *out) VKAPI_CALL {
      auto *import = static_cast<const wsi_memory_allocate_info *>(a->pNext);
      CHECK(import && import->sType == VK_STRUCTURE_TYPE_WSI_MEMORY_ALLOCATE_INFO_MESA);
      const auto *owner = import->win32_memory;
      CHECK(owner && owner->adapter_luid == 0x112233 && a->allocationSize <= owner->size);
      *out = h<VkDeviceMemory>(++s.nextMemory);
      const uint64_t address = 0x100000 + ((uint64_t)s.nextMemory << 18);
      owner->retain(owner->context);
      s.memory.emplace((uintptr_t)*out, Memory{owner, address});
      CHECK(owner->map_gpu(owner->context, address, 0, owner->size, 0, false, address, owner->size) == VK_SUCCESS);
      *import->win32_imported = true; return VK_SUCCESS;
   };
   dev->vk.BindImageMemory = [](VkDevice, VkImage, VkDeviceMemory memory, VkDeviceSize) VKAPI_CALL {
      auto &m = s.memory.at((uintptr_t)memory); m.extra = m.address + 0x20000;
      CHECK(m.owner->map_gpu(m.owner->context, m.extra, 0, m.owner->size,
         0xfe, false, m.extra, m.owner->size) == VK_SUCCESS);
      return VK_SUCCESS;
   };
   dev->vk.MapMemory = [](VkDevice, VkDeviceMemory memory, VkDeviceSize, VkDeviceSize, VkMemoryMapFlags, void **cpu) VKAPI_CALL {
      auto &m = s.memory.at((uintptr_t)memory); CHECK(!m.mapped);
      VkResult result = m.owner->map_cpu(m.owner->context, cpu);
      m.mapped = result == VK_SUCCESS; return result;
   };
   dev->vk.UnmapMemory = [](VkDevice, VkDeviceMemory memory) VKAPI_CALL {
      auto &m = s.memory.at((uintptr_t)memory); CHECK(m.mapped);
      m.owner->unmap_cpu(m.owner->context); m.mapped = false; ++s.unmaps;
   };
   dev->vk.FreeMemory = [](VkDevice, VkDeviceMemory memory, const VkAllocationCallbacks *) VKAPI_CALL {
      auto m = s.memory.at((uintptr_t)memory);
      CHECK(m.owner->free_gpu(m.owner->context, m.address, m.owner->size) == VK_SUCCESS);
      if (m.extra) CHECK(m.owner->free_gpu(m.owner->context, m.extra, m.owner->size) == VK_SUCCESS);
      if (m.mapped) m.owner->unmap_cpu(m.owner->context);
      s.memory.erase((uintptr_t)memory); m.owner->release(m.owner->context);
   };
   dev->vk.DestroyBuffer = [](VkDevice, VkBuffer b, const VkAllocationCallbacks *) VKAPI_CALL { CHECK(s.buffers.erase((uintptr_t)b) == 1); };
   dev->vk.GetSemaphoreCounterValue = [](VkDevice, VkSemaphore, uint64_t *v) VKAPI_CALL { *v = s.render; return VK_SUCCESS; };
   dev->vk.AllocateCommandBuffers = [](VkDevice, const VkCommandBufferAllocateInfo *, VkCommandBuffer *c) VKAPI_CALL {
      *c = h<VkCommandBuffer>(0x900); return VK_SUCCESS;
   };
   dev->vk.QueueSubmit2 = [](VkQueue, UINT count, const VkSubmitInfo2 *info, VkFence) VKAPI_CALL {
      CHECK(count == 1 && info->signalSemaphoreInfoCount == 1); s.order.push_back(2); return VK_SUCCESS;
   };
   dev->vk.WaitSemaphores = [](VkDevice, const VkSemaphoreWaitInfo *, uint64_t) VKAPI_CALL {
      ++s.renderWaits; CHECK(false); return VK_TIMEOUT;
   };
   CHECK(runtime_device_create(dev) == S_OK);
}

static void trim_contracts()
{
   Adapter adapter; Device dev; init(&dev, &adapter);
   D3D11_1DDI_DEVICEFUNCS table = {}; device_fill_table11(&table);
   DXGI1_4_DDI_BASE_FUNCTIONS dxgi = {}; dxgi_fill_presentation14(&dxgi);
   alignas(Resource) BYTE storage[3][sizeof(Resource)];
   Resource *res[3];
   D3D10DDI_MIPINFO mip = {256,1,1,256,1,1};
   D3D11DDIARG_CREATERESOURCE desc = {};
   desc.pMipInfoList = &mip; desc.ResourceDimension = D3D10DDIRESOURCE_BUFFER;
   desc.Usage = D3D10_DDI_USAGE_DYNAMIC; desc.BindFlags = D3D10_DDI_BIND_VERTEX_BUFFER;
   desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
   for (UINT i = 0; i < 3; ++i) {
      res[i] = reinterpret_cast<Resource *>(storage[i]);
      table.pfnCreateResource({&dev}, &desc, {res[i]}, {h<HANDLE>(i+1)});
   }
   CHECK(!s.errors && dev.resources == res[2] && s.residents == 3);
   res[2]->primary = true; // a rendering fence cannot decide scanout lifetime
   res[1]->sliceLastUse = 2; dev.submitted = 2; s.render = 1;
   DXGI_DDI_ARG_TRIMRESIDENCYSET trim = {};
   trim.hDevice = (DXGI_DDI_HDEVICE)&dev; trim.TrimFlags.RestartPeriodicTrim = 1;
   CHECK(dxgi.pfnTrimResidencySet(&trim) == S_OK && !s.evictions && dev.periodicTrimSequence == 1);
   trim.TrimFlags.Value = 0; trim.TrimFlags.PeriodicTrim = 1;
   CHECK(dxgi.pfnTrimResidencySet(&trim) == S_OK && s.evictions == 1 && !s.renderWaits);
   CHECK(dxgi.pfnTrimResidencySet(&trim) == S_OK && s.evictions == 1); // balanced reference
   CHECK(!s.allocations.at(runtime_allocation(resource_runtime(res[0]))).resident);
   CHECK(cmd_begin(&dev)); res[0]->sliceLastUse = dev.recordingSeq();
   const UINT paging = s.pagingWaits;
   CHECK(flush(&dev, false) == S_OK && dev.submitted == 3 && s.render == 1 &&
         s.residents == 4 && s.pagingWaits > paging && !s.renderWaits && !s.deallocations);
   s.render = 3;
   trim.TrimFlags.Value = 0; trim.TrimFlags.TrimToBudget = 1; trim.NumBytesToTrim = 4096;
   CHECK(dxgi.pfnTrimResidencySet(&trim) == S_OK && s.evictions == 2);
   // A failed paging wait may be retried without incrementing MakeResident's refcount twice.
   s.failPaging = true;
   CHECK(runtime_make_resident(&dev, resource_runtime(res[1])) == E_FAIL && s.residents == 5);
   CHECK(runtime_make_resident(&dev, resource_runtime(res[1])) == S_OK && s.residents == 5 && s.render == 3);
   // Eviction failure keeps residency and contents; an idle retry can succeed.
   s.evictResult = E_OUTOFMEMORY;
   CHECK(dxgi.pfnTrimResidencySet(&trim) == E_OUTOFMEMORY && s.evictions == 2);
   s.evictResult = S_OK;
   CHECK(dxgi.pfnTrimResidencySet(&trim) == S_OK && s.evictions == 3);
   // Destroy-before-submit restores the old backing, then retires it only after GPU completion.
   CHECK(cmd_begin(&dev)); res[1]->sliceLastUse = dev.recordingSeq();
   table.pfnDestroyResource({&dev}, {res[1]});
   CHECK(s.residents == 6 && !s.deallocations && dev.resources->next == res[0]);
   CHECK(flush(&dev, false) == S_OK && dev.submitted == 4 && s.render == 3);
   CHECK(!s.deallocations);
   s.render = 4; retire(&dev); CHECK(s.deallocations == 1);
   trim.TrimFlags.PeriodicTrim = trim.TrimFlags.RestartPeriodicTrim = 1;
   CHECK(dxgi.pfnTrimResidencySet(&trim) == E_INVALIDARG);
   table.pfnDestroyResource({&dev}, {res[0]}); table.pfnDestroyResource({&dev}, {res[2]});
   CHECK(!dev.resources && s.allocations.empty() && s.memory.empty() && !s.errors && !s.renderWaits);
   CHECK(table.pfnFlush({&dev}, D3DWDDM1_3DDI_TRIM_MEMORY | 0x1000)); // unknown flag conservatively flushes
   CHECK(dev.freePools.empty() && dev.freeCommands.empty());
   cmd_destroy(&dev); runtime_device_destroy(&dev);
   std::puts("PASS: periodic/budget Trim evicts only completed idle backing, preserves scanout and in-flight memory, restores paging before GPU use, balances retry references and handles destroy-before-submit (CPU control)");
}

static void budget_failure_contracts()
{
   Adapter adapter; Device dev; init(&dev, &adapter);
   D3D11_1DDI_DEVICEFUNCS table = {}; device_fill_table11(&table);
   alignas(Resource) BYTE storage[6][sizeof(Resource)];
   Resource *res[6];
   D3D10DDI_MIPINFO mip = {256,1,1,256,1,1};
   D3D11DDIARG_CREATERESOURCE desc = {};
   desc.pMipInfoList = &mip; desc.ResourceDimension = D3D10DDIRESOURCE_BUFFER;
   desc.Usage = D3D10_DDI_USAGE_DYNAMIC; desc.BindFlags = D3D10_DDI_BIND_VERTEX_BUFFER;
   desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
   for (UINT i = 0; i < 6; ++i) {
      res[i] = reinterpret_cast<Resource *>(storage[i]);
      table.pfnCreateResource({&dev}, &desc, {res[i]}, {h<HANDLE>(i+1)});
   }
   CHECK(!s.errors && s.residents == 6);
   auto resident = [&](UINT i) { return s.allocations.at(runtime_allocation(resource_runtime(res[i]))).resident; };
   dev.submitted = 1; res[3]->sliceLastUse = 1; // GPU has not completed this batch
   res[4]->bufferMapped = true; res[5]->primary = true;
   uint64_t bytes = 0;
   CHECK(runtime_evict(resource_runtime(res[0]), &bytes) == S_OK && bytes == 4096);
   // The budget changes between attempts: each failure must trigger a fresh
   // trim, without evicting the target, mapped, in-flight or scanout resources.
   s.residencyTrims = {4096, 4096};
   UINT attempts = s.residencyAttempts;
   CHECK(runtime_make_resident(&dev, resource_runtime(res[0])) == S_OK);
   CHECK(s.residencyAttempts == attempts + 3 && s.evictions == 3 && !s.finalResidencyAttempts);
   CHECK(resident(0) && !resident(1) && !resident(2) && resident(3) && resident(4) && resident(5));
   // No eligible backing remains. One final CantTrimFurther request fails
   // cleanly; there is no endless retry or forced MustSucceed allocation.
   res[0]->sliceLastUse = 1; s.failResidency = true; attempts = s.residencyAttempts;
   CHECK(runtime_make_resident(&dev, resource_runtime(res[1])) == E_OUTOFMEMORY);
   CHECK(s.residencyAttempts == attempts + 2 && s.finalResidencyAttempts == 1 && !resident(1));
   CHECK(!s.renderWaits && !s.render && !s.errors && s.order.empty());
   // A paging wait retry still reuses the one successful residency reference.
   s.failResidency = false; s.failPaging = true; attempts = s.residencyAttempts;
   CHECK(runtime_make_resident(&dev, resource_runtime(res[1])) == E_FAIL);
   CHECK(runtime_make_resident(&dev, resource_runtime(res[1])) == S_OK);
   CHECK(s.residencyAttempts == attempts + 1);
   // A zero trim estimate exhausts idle backing instead of treating zero as
   // successful residency. Keep the render fence and protected objects intact.
   res[4]->bufferMapped = false; s.residencyTrims = {0}; s.nextResidencyTrim = 0;
   CHECK(runtime_make_resident(&dev, resource_runtime(res[2])) == S_OK);
   CHECK(!resident(1) && !resident(4) && resident(0) && resident(2) && resident(3) && resident(5));
   CHECK(!s.renderWaits && !s.render && s.order.empty());
   // Evict's updated budget can require more than the first allocation.
   CHECK(runtime_make_resident(&dev, resource_runtime(res[1])) == S_OK);
   CHECK(runtime_make_resident(&dev, resource_runtime(res[4])) == S_OK);
   s.evictionBudget = 8192;
   uint64_t released = 0;
   CHECK(trim_residency_for_allocation(&dev, resource_runtime(res[2]), 4096, &released) == S_OK);
   CHECK(released == 8192 && !resident(1) && !resident(4) && resident(2));
   s.evictionBudget = 0; s.render = dev.submitted;
   for (auto *r : res) { r->primary = false; table.pfnDestroyResource({&dev}, {r}); }
   CHECK(!dev.resources && s.allocations.empty() && s.memory.empty() && !s.errors && !s.renderWaits);
   cmd_destroy(&dev); runtime_device_destroy(&dev);
   std::puts("PASS: MakeResident budget failures trim/retry, follow changing estimates, preserve protected backing, terminate irreducible OOM and balance paging references (CPU control)");
}
}

void offer_contracts()
{
#ifdef TGD3D_CPU_SYNC_CONTROL
   return; // this fixture verifies the production bidirectional path
#else
   Adapter adapter; Device dev; init(&dev, &adapter);
   D3D11_1DDI_DEVICEFUNCS table = {}; device_fill_table11(&table);
   DXGI1_3_DDI_BASE_FUNCTIONS dxgi = {}; dxgi_fill_presentation13(&dxgi);
   alignas(Resource) BYTE aBytes[sizeof(Resource)], bBytes[sizeof(Resource)], imageBytes[sizeof(Resource)];
   Resource *a = reinterpret_cast<Resource *>(aBytes), *b = reinterpret_cast<Resource *>(bBytes);
   Resource *image = reinterpret_cast<Resource *>(imageBytes);
   BYTE initial[256]; std::memset(initial, 0x42, sizeof(initial));
   D3D10DDI_MIPINFO mip = {256, 1, 1, 256, 1, 1};
   D3D10_DDIARG_SUBRESOURCE_UP data = {initial, 256, 256};
   D3D11DDIARG_CREATERESOURCE desc = {};
   desc.pMipInfoList = &mip; desc.pInitialDataUP = &data;
   desc.ResourceDimension = D3D10DDIRESOURCE_BUFFER; desc.Usage = D3D10_DDI_USAGE_DYNAMIC;
   desc.BindFlags = D3D10_DDI_BIND_VERTEX_BUFFER | D3D10_DDI_BIND_SHADER_RESOURCE;
   desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
   table.pfnCreateResource({&dev}, &desc, {a}, {h<HANDLE>(1)});
   table.pfnCreateResource({&dev}, &desc, {b}, {h<HANDLE>(2)});
   CHECK(!s.errors && a->slice.chunk != b->slice.chunk && a->slice.chunk->dedicated && b->slice.chunk->dedicated);
   CHECK(a->slice.chunk->runtime && !std::memcmp(a->slice.cpu(), initial, sizeof(initial)));
   const UINT aHandle = runtime_allocation(resource_runtime(a)), bHandle = runtime_allocation(resource_runtime(b));
   BYTE *oldCpu = a->slice.cpu();
   mip = {65,17,1,65,17,1}; desc.pInitialDataUP = nullptr;
   desc.ResourceDimension = D3D10DDIRESOURCE_TEXTURE2D; desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
   desc.Usage = D3D10_DDI_USAGE_DEFAULT; desc.BindFlags = D3D10_DDI_BIND_RENDER_TARGET;
   table.pfnCreateResource({&dev}, &desc, {image}, {h<HANDLE>(3)});
   CHECK(!s.errors && s.kinds == 1 && image->storage.runtime && !image->storage.preinitialized);
   CHECK(!runtime_requires_resource_lifetime(image->storage.runtime));
   View view; view.res = a; dev.stage[STAGE_PS].srv[0] = &view;
   dev.vb[0].buffer = a; dev.indexBuffer = a; dev.stage[STAGE_VS].cb[0] = b;
   CHECK(cmd_begin(&dev)); a->sliceLastUse = image->storage.lastUse = dev.recordingSeq();
   DXGI_DDI_HRESOURCE resources[] = {(DXGI_DDI_HRESOURCE)a, (DXGI_DDI_HRESOURCE)image, (DXGI_DDI_HRESOURCE)a};
   DXGI_DDI_ARG_OFFERRESOURCES offer = {(DXGI_DDI_HDEVICE)&dev, resources, 3, D3DDDI_OFFER_PRIORITY_NORMAL};
   s.order.clear(); const unsigned waits = s.pagingWaits;
   CHECK(dxgi.pfnOfferResources(&offer) == S_OK && s.order == (std::vector<int>{1,2,3,4}));
   CHECK(!s.renderWaits && s.pagingWaits == waits && !s.render && s.offers == 1);
   CHECK(runtime_is_offered(resource_runtime(a)) && !a->slice.chunk->cpu && !s.allocations.at(bHandle).offered);
   CHECK(!dev.stage[STAGE_PS].srv[0] && !dev.vb[0].buffer && !dev.indexBuffer && dev.stage[STAGE_VS].cb[0] == b);
   CHECK(slice_map(&dev, a->slice) == DXGI_ERROR_INVALID_CALL && !s.errors);
   BOOL discarded[3] = {77,77,77};
   DXGI_DDI_ARG_RECLAIMRESOURCES reclaim = {(DXGI_DDI_HDEVICE)&dev, resources, discarded, 3};
   s.failPaging = true;
   CHECK(dxgi.pfnReclaimResources(&reclaim) == E_FAIL && s.reclaims == 1 && discarded[0] == 77);
   CHECK(runtime_is_offered(resource_runtime(a)) && !s.render);
   CHECK(dxgi.pfnReclaimResources(&reclaim) == S_OK && s.reclaims == 1);
   CHECK(!discarded[0] && !discarded[1] && !discarded[2] && !a->slice.chunk->cpu && !s.render);
   // Paging completion must not release backing still held by render sequence 1.
   table.pfnDestroyResource({&dev}, {image}); retire(&dev);
   CHECK(!s.deallocations && !s.renderWaits && s.allocations.size() == 3);
   s.render = dev.submitted; retire(&dev); CHECK(s.deallocations == 1);
   offer.Resources = 1; reclaim.Resources = 1; s.discard = aHandle;
   CHECK(dxgi.pfnOfferResources(&offer) == S_OK);
   CHECK(dxgi.pfnReclaimResources(&reclaim) == S_OK && discarded[0]);
   D3D10DDI_MAPPED_SUBRESOURCE mapped = {};
   table.pfnDynamicIABufferMapNoOverwrite({&dev}, {a}, 0, D3D10_DDI_MAP_WRITE_NOOVERWRITE, 0, &mapped);
   CHECK(mapped.pData && mapped.pData != oldCpu && *(BYTE *)mapped.pData == 0xcd && a->bufferMapped);
   const unsigned offered = s.offers;
   CHECK(dxgi.pfnOfferResources(&offer) == E_INVALIDARG && s.offers == offered);
   table.pfnDynamicIABufferUnmap({&dev}, {a}, 0);
   s.offerResult = E_OUTOFMEMORY;
   CHECK(dxgi.pfnOfferResources(&offer) == E_OUTOFMEMORY && !a->slice.chunk->cpu && !runtime_is_offered(resource_runtime(a)));
   table.pfnResourceUpdateSubresourceUP({&dev}, {a}, 0, nullptr, initial, 256, 256, 0);
   CHECK(!s.errors && !std::memcmp(a->slice.cpu(), initial, 256) && !std::memcmp(b->slice.cpu(), initial, 256));
   s.offerResult = S_OK;
   const auto attempts = s.offers;
   a->usage = D3D10_DDI_USAGE_IMMUTABLE;
   CHECK(dxgi.pfnOfferResources(&offer) == E_INVALIDARG);
   a->usage = D3D10_DDI_USAGE_DYNAMIC; a->misc = D3D10_DDI_RESOURCE_MISC_SHARED;
   CHECK(dxgi.pfnOfferResources(&offer) == E_INVALIDARG && s.offers == attempts);
   a->misc = 0;
   table.pfnDestroyResource({&dev}, {a}); table.pfnDestroyResource({&dev}, {b});
   CHECK(s.allocations.empty() && s.memory.empty() && s.buffers.empty() && !s.errors && !s.renderWaits);
   CHECK(dev.chunks.empty()); cmd_destroy(&dev); runtime_device_destroy(&dev);
   std::puts("PASS: runtime-owned private image/buffer backing, optimal PTE kind, distinct buffers, GPU-gated Offer, deduplication, paging-retry Reclaim, stale CPU remapping and real render retirement (CPU control)");
   trim_contracts();
   budget_failure_contracts();
#endif
}
