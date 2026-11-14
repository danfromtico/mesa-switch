/* SPDX-License-Identifier: MIT
 * Real runtime owner implementation with two independent callback devices.
 */
#include "tgd3d.h"
#include "vulkan/util/vk_internal_exts.h"
#include "vulkan/util/vk_wsi_win32_memory.h"
#include "nouveau/vulkan/nvkmd/windows/allocation_abi.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
using namespace tgd3d;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%u: %s\n", __FILE__, __LINE__, #x); std::exit(1); } } while (0)

static BYTE metadata[72], bytes[8192];
static tg_gpu_linear_allocation_request allocation;
static uint64_t pagingValues[2];
static const wsi_win32_memory *owners[2];
static unsigned allocations, deallocations, maps, frees, locks, unlocks, waits, presents;
static HRESULT presentStatus;
static HRESULT primaryError;
static D3DDDIFORMAT primaryFormat;
static UINT primaryWidth, primaryHeight, primaryPitch, primaryRotation, primaryModeRotation;
static unsigned primaryImages, primaryModes;
static unsigned primaryFailureStep, primaryAllocations, primaryResidents, expectedPrimaryDeallocations;
static VkResult primaryFailure;
static HRESULT primaryRuntimeFailure;
static bool ignoreImport;
static HANDLE rt_device(unsigned i) { return (HANDLE)(uintptr_t)(0x100 + i); }
static HANDLE rt_resource(unsigned i) { return (HANDLE)(uintptr_t)(0x200 + i); }
static unsigned index(HANDLE h) { CHECK(h == rt_device(0) || h == rt_device(1)); return (uintptr_t)h - 0x100; }

static void init(Device *dev, Adapter *adapter, unsigned i)
{
   dev->adapter = adapter; dev->nvk = true; dev->hRTDevice.handle = rt_device(i);
   adapter->luid = 0x112233;
   adapter->vk.caps.memory.memoryTypeCount = 1;
   adapter->vk.caps.memory.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
   dev->vkdev = (VkDevice)(uintptr_t)(i + 1);
   auto &cb = dev->kt;
   cb.pfnCreateContextVirtualCb = [](HANDLE h, D3DDDICB_CREATECONTEXTVIRTUAL *a) -> HRESULT {
      a->hContext = (HANDLE)(uintptr_t)(0x300 + index(h)); return S_OK;
   };
   cb.pfnDestroyContextCb = [](HANDLE h, const D3DDDICB_DESTROYCONTEXT *a) -> HRESULT {
      CHECK(a->hContext == (HANDLE)(uintptr_t)(0x300 + index(h))); return S_OK;
   };
   cb.pfnCreatePagingQueueCb = [](HANDLE h, D3DDDICB_CREATEPAGINGQUEUE *a) -> HRESULT {
      unsigned n = index(h); a->hPagingQueue = 0x40 + n; a->hSyncObject = 0x50 + n;
      a->FenceValueCPUVirtualAddress = &pagingValues[n]; return S_OK;
   };
   cb.pfnDestroyPagingQueueCb = [](HANDLE h, const D3DDDI_DESTROYPAGINGQUEUE *a) -> HRESULT {
      CHECK(a->hPagingQueue == 0x40 + index(h)); return S_OK;
   };
   cb.pfnAllocateCb = [](HANDLE h, D3DDDICB_ALLOCATE *a) -> HRESULT {
      CHECK(index(h) == 0 && a->hResource == rt_resource(0) && a->NumAllocations == 1);
      CHECK(a->PrivateDriverDataSize == sizeof(metadata));
      CHECK(a->pAllocationInfo2->PrivateDriverDataSize == sizeof(allocation));
      memcpy(metadata, a->pPrivateDriverData, sizeof(metadata));
      memcpy(&allocation, a->pAllocationInfo2->pPrivateDriverData, sizeof(allocation));
      CHECK(allocation.Width == 65 && allocation.Height == 17 && allocation.Pitch == 384);
      CHECK(allocation.Buffer.bytes == sizeof(bytes));
      a->pAllocationInfo2->hAllocation = 0x60; ++allocations; return S_OK;
   };
   cb.pfnDeallocateCb = [](HANDLE h, const D3DDDICB_DEALLOCATE *a) -> HRESULT {
      CHECK(a->hResource == rt_resource(index(h)) && !a->NumAllocations && !a->HandleList);
      ++deallocations; return S_OK;
   };
   cb.pfnMakeResidentCb = [](HANDLE h, D3DDDI_MAKERESIDENT *a) -> HRESULT {
      CHECK(a->hPagingQueue == 0x40 + index(h) && a->NumAllocations == 1);
      CHECK(*a->AllocationList == 0x60 + index(h));
      a->PagingFenceValue = 7; return E_PENDING;
   };
   cb.pfnWaitForSynchronizationObjectFromCpuCb = [](HANDLE h,
         const D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMCPU *a) -> HRESULT {
      unsigned n = index(h); CHECK(a->ObjectCount == 1 && *a->ObjectHandleArray == 0x50 + n);
      pagingValues[n] = *a->FenceValueArray; ++waits; return S_OK;
   };
   cb.pfnMapGpuVirtualAddressCb = [](HANDLE h, D3DDDI_MAPGPUVIRTUALADDRESS *a) -> HRESULT {
      CHECK(a->hPagingQueue == 0x40 + index(h) && a->hAllocation == 0x60 + index(h));
      CHECK(a->BaseAddress == 0x100000 && a->SizeInPages == 2 && !a->OffsetInPages);
      a->VirtualAddress = a->BaseAddress; a->PagingFenceValue = 8; ++maps; return E_PENDING;
   };
   cb.pfnFreeGpuVirtualAddressCb = [](HANDLE h, const D3DDDICB_FREEGPUVIRTUALADDRESS *a) -> HRESULT {
      index(h); CHECK(a->BaseAddress == 0x100000 && a->Size == sizeof(bytes)); ++frees; return S_OK;
   };
   cb.pfnLock2Cb = [](HANDLE h, D3DDDICB_LOCK2 *a) -> HRESULT {
      CHECK(a->hAllocation == 0x60 + index(h)); a->pData = bytes; ++locks; return S_OK;
   };
   cb.pfnUnlock2Cb = [](HANDLE h, const D3DDDICB_UNLOCK2 *a) -> HRESULT {
      CHECK(a->hAllocation == 0x60 + index(h)); ++unlocks; return S_OK;
   };
   dev->vk.GetImageMemoryRequirements = [](VkDevice, VkImage, VkMemoryRequirements *r) {
      *r = {sizeof(bytes), 4096, 1};
   };
   dev->vk.AllocateMemory = [](VkDevice vkdev, const VkMemoryAllocateInfo *a,
                              const VkAllocationCallbacks *, VkDeviceMemory *memory) {
      unsigned n = (uintptr_t)vkdev - 1;
      CHECK(a->memoryTypeIndex == 0 && a->allocationSize == sizeof(bytes));
      const auto *import = static_cast<const wsi_memory_allocate_info *>(a->pNext);
      CHECK(import->sType == VK_STRUCTURE_TYPE_WSI_MEMORY_ALLOCATE_INFO_MESA && import->win32_imported);
      *memory = (VkDeviceMemory)(uintptr_t)(n + 1);
      if (ignoreImport) return VK_SUCCESS;
      const auto *owner = import->win32_memory;
      CHECK(owner->adapter_luid == 0x112233 && owner->allocation == 0x60 + n);
      owner->retain(owner->context); owners[n] = owner;
      CHECK(owner->map_gpu(owner->context, 0x100000, 0, sizeof(bytes), 0, false,
                            0x100000, sizeof(bytes)) == VK_SUCCESS);
      void *cpu = nullptr; CHECK(owner->map_cpu(owner->context, &cpu) == VK_SUCCESS && cpu == bytes);
      *import->win32_imported = true;
      return VK_SUCCESS;
   };
   dev->vk.FreeMemory = [](VkDevice vkdev, VkDeviceMemory, const VkAllocationCallbacks *) {
      unsigned n = (uintptr_t)vkdev - 1;
      if (owners[n]) {
         const auto *owner = owners[n];
         CHECK(owner->free_gpu(owner->context, 0x100000, sizeof(bytes)) == VK_SUCCESS);
         owner->unmap_cpu(owner->context); owner->release(owner->context); owners[n] = nullptr;
      }
   };
   dev->vk.BindImageMemory = [](VkDevice, VkImage, VkDeviceMemory, VkDeviceSize offset) {
      CHECK(!offset); return VK_SUCCESS;
   };
   CHECK(SUCCEEDED(runtime_device_create(dev)));
}

static void primary_creation_contract()
{
   const struct {
      unsigned step;
      VkResult vulkan;
      HRESULT runtime, expected;
   } cases[] = {
      {0, VK_SUCCESS, S_OK, S_OK},
      {1, VK_ERROR_OUT_OF_HOST_MEMORY, S_OK, E_OUTOFMEMORY},
      {1, VK_ERROR_OUT_OF_DEVICE_MEMORY, S_OK, E_OUTOFMEMORY},
      {2, VK_ERROR_OUT_OF_HOST_MEMORY, S_OK, E_OUTOFMEMORY},
      {2, VK_ERROR_OUT_OF_DEVICE_MEMORY, S_OK, E_OUTOFMEMORY},
      {3, VK_ERROR_OUT_OF_HOST_MEMORY, S_OK, E_OUTOFMEMORY},
      {3, VK_ERROR_OUT_OF_DEVICE_MEMORY, S_OK, E_OUTOFMEMORY},
      {4, VK_SUCCESS, D3DDDIERR_OUTOFVIDEOMEMORY, E_OUTOFMEMORY},
      {4, VK_SUCCESS, D3DDDIERR_DEVICEREMOVED, D3DDDIERR_DEVICEREMOVED},
      {5, VK_SUCCESS, D3DDDIERR_OUTOFVIDEOMEMORY, E_OUTOFMEMORY},
      {5, VK_SUCCESS, D3DDDIERR_DEVICEREMOVED, D3DDDIERR_DEVICEREMOVED},
   };
   for (UINT rotation = D3DDDI_ROTATION_IDENTITY; rotation <= D3DDDI_ROTATION_270; rotation++)
   for (bool alpha : {false, true}) for (bool opened : {false, true})
   for (bool optional : {false, true}) for (const auto &failure : cases) {
      if (opened && optional)
         continue; /* OpenResource has no primary descriptor output. */
      if (opened && failure.step == 4)
         continue; /* Opening receives its allocation from the runtime. */
      Adapter adapter; Device dev;
      pagingValues[0] = 0;
      init(&dev, &adapter, 0);
      primaryError = S_OK;
      primaryFormat = alpha ? D3DDDIFMT_A8R8G8B8 : D3DDDIFMT_X8R8G8B8;
      primaryRotation = D3DDDI_ROTATION_IDENTITY;
      primaryModeRotation = rotation;
      primaryWidth = 720;
      primaryHeight = 1280;
      primaryPitch = primaryWidth * 4;
      primaryImages = primaryModes = 0;
      primaryAllocations = primaryResidents = 0;
      primaryFailureStep = failure.step; primaryFailure = failure.vulkan;
      primaryRuntimeFailure = failure.runtime;
      expectedPrimaryDeallocations = deallocations + (failure.step != 4);
      dev.um.pfnSetErrorCb = [](D3D10DDI_HRTCORELAYER, HRESULT hr) {
         CHECK(!primaryImages && !owners[0] && deallocations == expectedPrimaryDeallocations);
         primaryError = hr;
      };
      dev.kt.pfnAllocateCb = [](HANDLE h, D3DDDICB_ALLOCATE *a) -> HRESULT {
         CHECK(h == rt_device(0) && a->hResource == rt_resource(0) && a->NumAllocations == 1);
         CHECK(!a->PrivateDriverDataSize && a->pAllocationInfo2->Flags.Primary);
         CHECK(a->pAllocationInfo2->PrivateDriverDataSize == sizeof(tg_gpu_primary_allocation_request));
         const auto &r = *static_cast<const tg_gpu_primary_allocation_request *>(a->pAllocationInfo2->pPrivateDriverData);
         CHECK(r.Buffer.flags == (TG_GPU_ALLOCATION_CPU_VISIBLE | TG_GPU_ALLOCATION_DISPLAY_PRIMARY));
         CHECK(r.Format == primaryFormat && r.Width == primaryWidth && r.Height == primaryHeight);
         CHECK(r.Pitch == primaryPitch && r.Buffer.bytes == 3686400 && r.Rotation == primaryRotation);
         CHECK(r.ModeRotation == primaryModeRotation);
         ++primaryAllocations;
         if (primaryFailureStep == 4)
            return primaryRuntimeFailure;
         a->pAllocationInfo2->hAllocation = 0x60; return S_OK;
      };
      dev.kt.pfnMakeResidentCb = [](HANDLE h, D3DDDI_MAKERESIDENT *a) -> HRESULT {
         CHECK(h == rt_device(0) && a->hPagingQueue == 0x40 && *a->AllocationList == 0x60);
         ++primaryResidents;
         if (primaryFailureStep == 5)
            return primaryRuntimeFailure;
         a->PagingFenceValue = 7; return E_PENDING;
      };
      dev.kt.pfnSetDisplayModeCb = [](HANDLE h, D3DDDICB_SETDISPLAYMODE *a) -> HRESULT {
         CHECK(h == rt_device(0) && a->hPrimaryAllocation == 0x60); ++primaryModes; return S_OK;
      };
      dev.kt.pfnMapGpuVirtualAddressCb = [](HANDLE h, D3DDDI_MAPGPUVIRTUALADDRESS *a) -> HRESULT {
         CHECK(h == rt_device(0) && a->hAllocation == 0x60 && a->SizeInPages == 900);
         CHECK(a->BaseAddress == 0x100000 && !a->OffsetInPages);
         a->VirtualAddress = a->BaseAddress; return S_OK;
      };
      dev.kt.pfnFreeGpuVirtualAddressCb = [](HANDLE h, const D3DDDICB_FREEGPUVIRTUALADDRESS *a) -> HRESULT {
         CHECK(h == rt_device(0) && a->BaseAddress == 0x100000 && a->Size == 3686400); return S_OK;
      };
      dev.vk.CreateImage = [](VkDevice, const VkImageCreateInfo *a, const VkAllocationCallbacks *, VkImage *image) {
         CHECK(a->format == VK_FORMAT_B8G8R8A8_UNORM && a->tiling == VK_IMAGE_TILING_LINEAR);
         CHECK(a->extent.width == primaryWidth && a->extent.height == primaryHeight && a->initialLayout == VK_IMAGE_LAYOUT_PREINITIALIZED);
         const auto *wsi = static_cast<const wsi_image_create_info *>(a->pNext);
         CHECK(wsi->sType == VK_STRUCTURE_TYPE_WSI_IMAGE_CREATE_INFO_MESA && wsi->scanout && wsi->win32_row_pitch == primaryPitch);
         if (primaryFailureStep == 1)
            return primaryFailure;
         *image = (VkImage)0x22; ++primaryImages; return VK_SUCCESS;
      };
      dev.vk.GetImageSubresourceLayout = [](VkDevice, VkImage, const VkImageSubresource *, VkSubresourceLayout *out) {
         *out = {0, 3686400, primaryPitch, 3686400, 3686400};
      };
      dev.vk.GetImageMemoryRequirements = [](VkDevice, VkImage, VkMemoryRequirements *out) { *out = {3686400, 4096, 1}; };
      dev.vk.AllocateMemory = [](VkDevice, const VkMemoryAllocateInfo *a, const VkAllocationCallbacks *, VkDeviceMemory *out) {
         CHECK(a->allocationSize == 3686400 && a->memoryTypeIndex == 0);
         if (primaryFailureStep == 2)
            return primaryFailure;
         const auto *wsi = static_cast<const wsi_memory_allocate_info *>(a->pNext);
         const auto *owner = wsi->win32_memory;
         CHECK(owner->allocation == 0x60 && owner->size == 3686400);
         CHECK(owner->map_gpu(owner->context, 0x100000, 0, 3686400, 0, false, 0x100000, 3686400) == VK_SUCCESS);
         owner->retain(owner->context); owners[0] = owner;
         *wsi->win32_imported = true; *out = (VkDeviceMemory)1; return VK_SUCCESS;
      };
      dev.vk.BindImageMemory = [](VkDevice, VkImage, VkDeviceMemory, VkDeviceSize offset) {
         CHECK(!offset); return primaryFailureStep == 3 ? primaryFailure : VK_SUCCESS;
      };
      dev.vk.FreeMemory = [](VkDevice, VkDeviceMemory, const VkAllocationCallbacks *) {
         const auto *owner = owners[0];
         CHECK(owner && owner->free_gpu(owner->context, 0x100000, 3686400) == VK_SUCCESS);
         owner->release(owner->context); owners[0] = nullptr;
      };
      dev.vk.DestroyImage = [](VkDevice, VkImage image, const VkAllocationCallbacks *) { CHECK(image == (VkImage)0x22); --primaryImages; };
      D3D10DDI_MIPINFO mip = {primaryWidth, primaryHeight, 1, primaryWidth, primaryHeight, 1};
      DXGI_DDI_PRIMARY_DESC primary = {};
      primary.Flags = (optional ? DXGI_DDI_PRIMARY_OPTIONAL : 0) |
         (rotation == D3DDDI_ROTATION_IDENTITY ? 0 : DXGI_DDI_PRIMARY_NONPREROTATED);
      /* DriverFlags is an output, including when the caller reuses a
       * descriptor that previously requested copy-style presentation. */
      primary.DriverFlags = DXGI_DDI_PRIMARY_DRIVER_FLAG_NO_SCANOUT;
      primary.ModeDesc.Width = 720; primary.ModeDesc.Height = 1280;
      primary.ModeDesc.Format = alpha ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_B8G8R8X8_UNORM;
      primary.ModeDesc.Rotation = static_cast<DXGI_DDI_MODE_ROTATION>(rotation);
      primary.ModeDesc.RefreshRate = {60, 1};
      D3D10DDIARG_CREATERESOURCE desc = {};
      desc.pMipInfoList = &mip; desc.pPrimaryDesc = &primary;
      desc.ResourceDimension = D3D10DDIRESOURCE_TEXTURE2D; desc.Usage = D3D10_DDI_USAGE_DEFAULT;
      desc.Format = primary.ModeDesc.Format; desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
      desc.BindFlags = D3D10_DDI_BIND_RENDER_TARGET | D3D10_DDI_BIND_PRESENT;
      alignas(Resource) unsigned char storage[sizeof(Resource)];
      D3D10DDI_DEVICEFUNCS table = {}; resource_fill_table(&table);
      tg_gpu_primary_allocation_request standard = {
         {TG_GPU_ALLOCATION_ABI, TG_GPU_ALLOCATION_CPU_VISIBLE | TG_GPU_ALLOCATION_DISPLAY_PRIMARY, 3686400},
         primaryFormat, {60, 1}, D3DDDI_ROTATION_IDENTITY, primaryWidth, primaryHeight, primaryPitch, rotation};
      D3DDDI_OPENALLOCATIONINFO allocation = {};
      allocation.hAllocation = 0x60; allocation.pPrivateDriverData = &standard;
      allocation.PrivateDriverDataSize = sizeof(standard);
      D3D10DDIARG_OPENRESOURCE open = {}; open.NumAllocations = 1; open.pOpenAllocationInfo = &allocation;
      auto create_or_open = [&] {
         if (opened) table.pfnOpenResource({&dev}, &open, {storage}, {rt_resource(0)});
         else table.pfnCreateResource({&dev}, &desc, {storage}, {rt_resource(0)});
      };
      create_or_open();
      if (failure.step) {
         CHECK(primaryError == failure.expected && !primaryImages && !owners[0]);
         CHECK(FAILED(dev.lost) == (failure.expected == D3DDDIERR_DEVICEREMOVED));
         if (FAILED(dev.lost)) {
            const unsigned a = primaryAllocations, r = primaryResidents;
            create_or_open();
            CHECK(primaryError == D3DDDIERR_DEVICEREMOVED && primaryAllocations == a && primaryResidents == r);
         }
         runtime_device_destroy(&dev);
         continue;
      }
      CHECK(primaryError == S_OK && primaryImages == 1);
      if (!opened)
         CHECK(primary.DriverFlags == 0);
      auto *res = reinterpret_cast<Resource *>(storage);
      CHECK(res->format == desc.Format && res->primary && res->pitch == primaryPitch);
      CHECK(res->width == primaryWidth && res->height == primaryHeight);
      CHECK(SUCCEEDED(runtime_set_display_mode(res->storage.runtime, &dev, 0)) && primaryModes == 1);
      table.pfnDestroyResource({&dev}, {storage});
      CHECK(!primaryImages && !owners[0] && primaryError == S_OK);
      runtime_device_destroy(&dev);
   }
   std::puts("PASS: ARGB/XRGB fixed-portrait primary Create/OpenResource and NONPREROTATED modes, exact pitch/extent, imported GPU VA and teardown (CPU control)");
   std::puts("PASS: accepted optional/mandatory primaries publish direct-scanout DriverFlags without stale NO_SCANOUT (CPU control)");
   std::puts("PASS: four display-mode angles retain fixed portrait storage through primary creation/open (CPU control)");
   std::puts("PASS: primary Create/OpenResource propagate image/import/bind OOM and runtime removal after releasing partial ownership (CPU control)");

   /* DXGI must receive its DDI opt-out, before any scanout allocation exists.
    * Returning a normal DXGI API error or S_OK here prevents rotated Blt. */
   for (UINT rotation = DXGI_DDI_MODE_ROTATION_ROTATE90;
        rotation <= DXGI_DDI_MODE_ROTATION_ROTATE270; ++rotation)
   for (UINT optional : {0u, (UINT)DXGI_DDI_PRIMARY_OPTIONAL}) {
      Adapter adapter; Device dev; init(&dev, &adapter, 0);
      primaryError = S_OK;
      dev.um.pfnSetErrorCb = [](D3D10DDI_HRTCORELAYER, HRESULT hr) { primaryError = hr; };
      dev.kt.pfnAllocateCb = [](HANDLE, D3DDDICB_ALLOCATE *) -> HRESULT { CHECK(false); return E_FAIL; };
      dev.vk.CreateImage = [](VkDevice, const VkImageCreateInfo *, const VkAllocationCallbacks *, VkImage *) {
         CHECK(false); return VK_ERROR_DEVICE_LOST;
      };
      const bool quarter = rotation != DXGI_DDI_MODE_ROTATION_ROTATE180;
      D3D10DDI_MIPINFO mip = {quarter ? 1280u : 720u, quarter ? 720u : 1280u, 1, 0, 0, 1};
      DXGI_DDI_PRIMARY_DESC primary = {};
      primary.Flags = optional;
      primary.ModeDesc.Rotation = (DXGI_DDI_MODE_ROTATION)rotation;
      primary.ModeDesc.Width = 720; primary.ModeDesc.Height = 1280;
      primary.ModeDesc.RefreshRate = {60, 1};
      D3D10DDIARG_CREATERESOURCE desc = {};
      desc.pMipInfoList = &mip; desc.pPrimaryDesc = &primary;
      desc.ResourceDimension = D3D10DDIRESOURCE_TEXTURE2D;
      desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
      desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
      desc.BindFlags = D3D10_DDI_BIND_RENDER_TARGET | D3D10_DDI_BIND_PRESENT;
      alignas(Resource) unsigned char storage[sizeof(Resource)];
      D3D10DDI_DEVICEFUNCS table = {}; resource_fill_table(&table);
      table.pfnCreateResource({&dev}, &desc, {storage}, {rt_resource(0)});
      CHECK(primaryError == DXGI_DDI_ERR_UNSUPPORTED && SUCCEEDED(dev.lost));
      CHECK(primary.DriverFlags == DXGI_DDI_PRIMARY_DRIVER_FLAG_NO_SCANOUT);
      runtime_device_destroy(&dev);
   }
   std::puts("PASS: rotated primary creation requests DXGI's GPU-copy fallback before allocation (CPU control)");
}

void runtime_contracts()
{
   Adapter adapter; Device producer, consumer; init(&producer, &adapter, 0); init(&consumer, &adapter, 1);
   D3D10DDI_MIPINFO mip = {}; mip.TexelWidth = 65; mip.TexelHeight = 17; mip.TexelDepth = 1;
   D3D10DDIARG_CREATERESOURCE desc = {};
   desc.pMipInfoList = &mip; desc.ResourceDimension = D3D10DDIRESOURCE_TEXTURE2D;
   desc.Usage = D3D10_DDI_USAGE_DEFAULT; desc.BindFlags = D3D10_DDI_BIND_RENDER_TARGET | D3D10_DDI_BIND_SHADER_RESOURCE;
   desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1; desc.MipLevels = desc.ArraySize = 1;
   desc.MiscFlags = D3D10_DDI_RESOURCE_MISC_SHARED | D3D10_DDI_RESOURCE_MISC_DISCARD_ON_PRESENT;
   RuntimeResource *p = nullptr, *c = nullptr; UINT pitch = 0;
   CHECK(SUCCEEDED(runtime_shared_create(&producer, &desc, {rt_resource(0)}, &p, &pitch)) && pitch == 384);
   D3DDDI_OPENALLOCATIONINFO info = {}; info.hAllocation = 0x61;
   info.pPrivateDriverData = &allocation; info.PrivateDriverDataSize = sizeof(allocation);
   D3D10DDIARG_OPENRESOURCE open = {}; open.NumAllocations = 1; open.pOpenAllocationInfo = &info;
   open.hKMResource.handle = 0x99; open.pPrivateDriverData = metadata; open.PrivateDriverDataSize = sizeof(metadata);
   RuntimeSharedDescription description;
   CHECK(SUCCEEDED(runtime_shared_open(&consumer, &open, {rt_resource(1)}, &c, &description)));
   CHECK(allocations == 1 && description.pitch == 384 && description.width == 65 && description.height == 17);
   VkDeviceMemory pm, cm;
   CHECK(SUCCEEDED(runtime_bind_image(&producer, p, (VkImage)1, &pm)));
   CHECK(SUCCEEDED(runtime_bind_image(&consumer, c, (VkImage)2, &cm)));
   CHECK(maps == 2 && locks == 2 && waits == 4);
   bytes[123] = 0x5a;
   producer.vk.FreeMemory(producer.vkdev, pm, nullptr); CHECK(SUCCEEDED(runtime_destroy(p)));
   CHECK(deallocations == 1 && bytes[123] == 0x5a && runtime_allocation(c) == 0x61);
   CHECK(owners[1] && !owners[0]);
   consumer.vk.FreeMemory(consumer.vkdev, cm, nullptr); CHECK(SUCCEEDED(runtime_destroy(c)));
   CHECK(deallocations == 2 && frees == 2 && unlocks == 2);
   std::puts("PASS: 65x17 shared pitch/metadata, E_PENDING paging, consumer handles and independent owner teardown");

   /* Frozen .158 GetStandardAllocationDriverData returns this 48-byte
    * allocation ABI with zero resource-level bytes. It predates TGSR and
    * must not allocate replacement storage when opened by a consumer. */
   const unsigned allocated_before_standard = allocations;
   for (unsigned primary = 0; primary < 2; primary++) {
      for (unsigned alpha = 0; alpha < 2; alpha++) {
         tg_gpu_primary_allocation_request standard = {
            {TG_GPU_ALLOCATION_ABI, TG_GPU_ALLOCATION_CPU_VISIBLE |
               (primary ? TG_GPU_ALLOCATION_DISPLAY_PRIMARY : 0), UINT64_C(720) * 1280 * 4},
            alpha ? D3DDDIFMT_A8R8G8B8 : D3DDDIFMT_X8R8G8B8,
            {primary ? 60u : 0u, primary ? 1u : 0u}, D3DDDI_ROTATION_IDENTITY,
            720, 1280, 2880, 0};
         D3DDDI_OPENALLOCATIONINFO si = {};
         si.hAllocation = 0x61; si.pPrivateDriverData = &standard; si.PrivateDriverDataSize = sizeof(standard);
         D3D10DDIARG_OPENRESOURCE so = {};
         so.NumAllocations = 1; so.pOpenAllocationInfo = &si;
         RuntimeResource *opened = nullptr;
         CHECK(SUCCEEDED(runtime_shared_open(&consumer, &so, {rt_resource(1)}, &opened, &description)));
         CHECK(allocations == allocated_before_standard && runtime_allocation(opened) == 0x61);
         CHECK(runtime_is_primary(opened) == bool(primary));
         CHECK(description.width == 720 && description.height == 1280 && description.pitch == 2880);
         CHECK(description.format == (alpha ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_B8G8R8X8_UNORM));
         CHECK(bool(description.bind_flags & D3D10_DDI_BIND_PRESENT) == bool(primary));
         CHECK(SUCCEEDED(runtime_destroy(opened)));
         standard.ModeRotation = primary ? D3DDDI_ROTATION_270 + 1 : D3DDDI_ROTATION_IDENTITY;
         CHECK(runtime_shared_open(&consumer, &so, {rt_resource(1)}, &opened, &description) == DXGI_ERROR_UNSUPPORTED && !opened);
         standard.ModeRotation = 0; standard.Buffer.bytes += 4096;
         CHECK(runtime_shared_open(&consumer, &so, {rt_resource(1)}, &opened, &description) == DXGI_ERROR_UNSUPPORTED && !opened);
         standard.Buffer.bytes -= 4096;
         standard.Rotation = D3DDDI_ROTATION_90;
         /* A new angle cannot reinterpret the producer's old storage. */
         CHECK(runtime_shared_open(&consumer, &so, {rt_resource(1)}, &opened, &description) == DXGI_ERROR_UNSUPPORTED && !opened);
         standard.Width = 1280; standard.Height = 720; standard.Pitch = 5120;
         /* Neither primaries nor shadows may request DC rotation. */
         CHECK(runtime_shared_open(&consumer, &so, {rt_resource(1)}, &opened, &description) == DXGI_ERROR_UNSUPPORTED && !opened);
         standard.Rotation = D3DDDI_ROTATION_IDENTITY;
         const HRESULT landscape = runtime_shared_open(&consumer, &so, {rt_resource(1)}, &opened, &description);
         if (primary) {
            CHECK(landscape == DXGI_ERROR_UNSUPPORTED && !opened);
         } else {
            CHECK(landscape == S_OK && opened && !runtime_is_primary(opened));
            CHECK(description.width == 1280 && description.height == 720 && description.pitch == 5120);
            CHECK(allocations == allocated_before_standard && runtime_allocation(opened) == 0x61);
            CHECK(SUCCEEDED(runtime_destroy(opened)));
         }
         standard.Pitch += 4;
         CHECK(runtime_shared_open(&consumer, &so, {rt_resource(1)}, &opened, &description) == DXGI_ERROR_UNSUPPORTED && !opened);
         standard.Pitch -= 4;
         standard.Rotation = D3DDDI_ROTATION_270 + 1;
         CHECK(runtime_shared_open(&consumer, &so, {rt_resource(1)}, &opened, &description) == DXGI_ERROR_UNSUPPORTED && !opened);
      }
   }
   std::puts("PASS: standard primary/shadow opens without TGSR, XRGB/ARGB identity and consumer-owned handles (CPU control)");

   /* The ICD must positively acknowledge importing the runtime allocation. */
   CHECK(SUCCEEDED(runtime_shared_create(&producer, &desc, {rt_resource(0)}, &p, &pitch)));
   ignoreImport = true;
   CHECK(runtime_bind_image(&producer, p, (VkImage)1, &pm) == DXGI_ERROR_UNSUPPORTED && !pm);
   ignoreImport = false;
   DXGI_DDI_BASE_CALLBACKS callbacks = {};
   callbacks.pfnPresentCb = [](HANDLE h, DXGIDDICB_PRESENT *a) -> HRESULT {
      CHECK(h == rt_device(0) && a->hContext == (HANDLE)(uintptr_t)0x300);
      CHECK(a->hSrcAllocation == 0x60 && !a->hDstAllocation && a->pDXGIContext == (void *)0x12345);
      CHECK(a->SyncIntervalOverrideValid && a->SyncIntervalOverride == DXGI_DDI_FLIP_INTERVAL_ONE);
      ++presents; return presentStatus;
   };
   producer.dxgi = &callbacks;
   Resource surface; surface.dev = &producer; surface.storage.runtime = p;
   DXGI_DDI_ARG_PRESENT present = {}; present.hDevice = (DXGI_DDI_HDEVICE)&producer;
   present.hSurfaceToPresent = (DXGI_DDI_HRESOURCE)&surface; present.Flags.Blt = 1;
   present.pDXGIContext = (void *)0x12345; present.FlipInterval = DXGI_DDI_FLIP_INTERVAL_IMMEDIATE;
   DXGI1_1_DDI_BASE_FUNCTIONS dxgi = {}; dxgi_fill_table(&dxgi, true);
   presentStatus = DXGI_STATUS_OCCLUDED;
   CHECK(dxgi.pfnPresent(&present) == DXGI_STATUS_OCCLUDED && presents == 1);
   presentStatus = DXGI_ERROR_DEVICE_REMOVED;
   CHECK(dxgi.pfnPresent(&present) == DXGI_ERROR_DEVICE_REMOVED && FAILED(producer.lost));
   CHECK(SUCCEEDED(runtime_destroy(p)));
   runtime_device_destroy(&consumer); runtime_device_destroy(&producer);
   std::puts("PASS: rejected ignored owner import; Present preserves token/status and requests interval one (no timing proof)");
   primary_creation_contract();
}
