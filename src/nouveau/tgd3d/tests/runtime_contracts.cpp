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
      {6, VK_SUCCESS, E_INVALIDARG, D3DDDIERR_DEVICEREMOVED},
      {6, VK_SUCCESS, E_OUTOFMEMORY, D3DDDIERR_DEVICEREMOVED},
      {6, VK_SUCCESS, D3DDDIERR_OUTOFVIDEOMEMORY, D3DDDIERR_DEVICEREMOVED},
   };
   for (UINT rotation = D3DDDI_ROTATION_IDENTITY; rotation <= D3DDDI_ROTATION_270; rotation++)
   for (bool nonprerotated : {false, true})
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
      primaryRotation = nonprerotated ? D3DDDI_ROTATION_IDENTITY : rotation;
      primaryModeRotation = rotation;
      const bool quarter = primaryRotation == D3DDDI_ROTATION_90 || primaryRotation == D3DDDI_ROTATION_270;
      primaryWidth = quarter ? 1280 : 720;
      primaryHeight = quarter ? 720 : 1280;
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
         CHECK(h == rt_device(0) && a->BaseAddress == 0x100000 && a->Size == 3686400);
         return primaryFailureStep == 6 ? primaryRuntimeFailure : S_OK;
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
         CHECK(owner);
         const VkResult result = owner->free_gpu(owner->context, 0x100000, 3686400);
         CHECK((result == VK_SUCCESS) == (primaryFailureStep != 6));
         owner->release(owner->context); owners[0] = nullptr;
      };
      dev.vk.DestroyImage = [](VkDevice, VkImage image, const VkAllocationCallbacks *) { CHECK(image == (VkImage)0x22); --primaryImages; };
      D3D10DDI_MIPINFO mip = {primaryWidth, primaryHeight, 1, primaryWidth, primaryHeight, 1};
      DXGI_DDI_PRIMARY_DESC primary = {};
      primary.Flags = (optional ? DXGI_DDI_PRIMARY_OPTIONAL : 0) |
         (nonprerotated ? DXGI_DDI_PRIMARY_NONPREROTATED : 0);
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
         primaryFormat, {60, 1}, primaryRotation, primaryWidth, primaryHeight, primaryPitch, rotation};
      D3DDDI_OPENALLOCATIONINFO allocation = {};
      allocation.hAllocation = 0x60; allocation.pPrivateDriverData = &standard;
      allocation.PrivateDriverDataSize = sizeof(standard);
      D3D10DDIARG_OPENRESOURCE open = {}; open.NumAllocations = 1; open.pOpenAllocationInfo = &allocation;
      auto create_or_open = [&] {
         if (opened) table.pfnOpenResource({&dev}, &open, {storage}, {rt_resource(0)});
         else table.pfnCreateResource({&dev}, &desc, {storage}, {rt_resource(0)});
      };
      create_or_open();
      if (failure.step && failure.step != 6) {
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
      CHECK(!primaryImages && !owners[0] && primaryError == (failure.step == 6 ? failure.expected : S_OK));
      CHECK(FAILED(dev.lost) == (failure.step == 6));
      CHECK(!dev.removalOnlyErrors);
      runtime_device_destroy(&dev);
   }
   std::puts("PASS: ARGB/XRGB four-direction primary Create/OpenResource and NONPREROTATED modes, exact pitch/extent, imported GPU VA and teardown (CPU control)");
   std::puts("PASS: accepted optional/mandatory primaries publish direct-scanout DriverFlags without stale NO_SCANOUT (CPU control)");
   std::puts("PASS: four display-mode angles use explicit DC orientation; NONPREROTATED retains identity storage (CPU control)");
   std::puts("PASS: primary Create/OpenResource propagate image/import/bind OOM and runtime removal after releasing partial ownership (CPU control)");
   std::puts("PASS: DestroyResource reports only device removal for GPU-VA release failures, after cleanup; Create/OpenResource OOM stays recoverable (CPU control)");

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
      /* Deliberately wrong extent for the requested storage rotation. */
      D3D10DDI_MIPINFO mip = {quarter ? 720u : 1280u, quarter ? 1280u : 720u, 1, 0, 0, 1};
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
      CHECK(primaryError == DXGI_ERROR_UNSUPPORTED && SUCCEEDED(dev.lost));
      CHECK(primary.DriverFlags == 0);
      runtime_device_destroy(&dev);
   }
   std::puts("PASS: invalid rotated primary extents are rejected before allocation (CPU control)");
}

static uint64_t modeCompleted;
static VkResult modeWaitResult;
static unsigned modeWaits, modeCallbacks, modeErrors;

static UINT sourceFormat, wantedFormat, privateModeCalls, privateFormatCalls;
static HRESULT privateInitial, privateChange, privateRetry;

static void primary_private_format_contract()
{
   const struct {
      HRESULT initial, change, retry, expected;
      UINT current; /* 4 = unknown, 5 = already the requested layout */
      bool callback;
      UINT modes, formats;
   } cases[] = {
      {S_OK, S_OK, S_OK, S_OK, 0, true, 1, 0},
      {E_INVALIDARG, S_OK, S_OK, E_INVALIDARG, 0, true, 1, 0},
      {D3DDDIERR_INCOMPATIBLEPRIVATEFORMAT, S_OK, S_OK, D3DDDIERR_INCOMPATIBLEPRIVATEFORMAT, 4, true, 1, 0},
      {D3DDDIERR_INCOMPATIBLEPRIVATEFORMAT, S_OK, S_OK, D3DDDIERR_INCOMPATIBLEPRIVATEFORMAT, 5, true, 1, 0},
      {D3DDDIERR_INCOMPATIBLEPRIVATEFORMAT, S_OK, S_OK, D3DDDIERR_INCOMPATIBLEPRIVATEFORMAT, 0, false, 1, 0},
      {D3DDDIERR_INCOMPATIBLEPRIVATEFORMAT, E_FAIL, S_OK, E_FAIL, 0, true, 1, 1},
      {D3DDDIERR_INCOMPATIBLEPRIVATEFORMAT, S_OK, S_OK, S_OK, 0, true, 2, 1},
      {D3DDDIERR_INCOMPATIBLEPRIVATEFORMAT, S_OK, D3DDDIERR_INCOMPATIBLEPRIVATEFORMAT,
         D3DDDIERR_INCOMPATIBLEPRIVATEFORMAT, 0, true, 2, 1},
      {D3DDDIERR_INCOMPATIBLEPRIVATEFORMAT, S_OK, D3DDDIERR_DEVICEREMOVED,
         D3DDDIERR_DEVICEREMOVED, 0, true, 2, 1},
   };
   for (UINT rotation = 1; rotation <= 4; ++rotation)
   for (bool opened : {false, true}) for (const auto &c : cases) {
      Adapter adapter; Device dev;
      init(&dev, &adapter, 0);
      wantedFormat = rotation - 1;
      sourceFormat = c.current == 5 ? wantedFormat :
         (c.current == 4 ? 4 : (wantedFormat + 1) % 4);
      privateInitial = c.initial; privateChange = c.change; privateRetry = c.retry;
      privateModeCalls = privateFormatCalls = 0;
      dev.kt.pfnSetDisplayModeCb = [](HANDLE h, D3DDDICB_SETDISPLAYMODE *a) -> HRESULT {
         CHECK(h == rt_device(0) && a->hPrimaryAllocation == 0x60);
         CHECK(!a->PrivateDriverFormatAttribute);
         ++privateModeCalls;
         a->PrivateDriverFormatAttribute = sourceFormat;
         return privateModeCalls == 1 ? privateInitial : privateRetry;
      };
      if (c.callback) dev.kt.pfnSetDisplayPrivateDriverFormatCb = [](HANDLE h,
         const D3DDDICB_SETDISPLAYPRIVATEDRIVERFORMAT *a) -> HRESULT {
         CHECK(h == rt_device(0) && !a->VidPnSourceId);
         CHECK(a->PrivateDriverFormatAttribute == wantedFormat);
         CHECK(privateModeCalls == 1 && !privateFormatCalls);
         ++privateFormatCalls;
         if (SUCCEEDED(privateChange)) sourceFormat = wantedFormat;
         return privateChange;
      };
      dev.kt.pfnAllocateCb = [](HANDLE h, D3DDDICB_ALLOCATE *a) -> HRESULT {
         CHECK(h == rt_device(0) && a->NumAllocations == 1 && a->pAllocationInfo2->Flags.Primary);
         const auto &r = *static_cast<const tg_gpu_primary_allocation_request *>(a->pAllocationInfo2->pPrivateDriverData);
         CHECK(r.Rotation == wantedFormat + 1);
         a->pAllocationInfo2->hAllocation = 0x60; return S_OK;
      };
      const bool quarter = rotation == 2 || rotation == 4;
      tg_gpu_primary_allocation_request primary = {
         {TG_GPU_ALLOCATION_ABI, TG_GPU_ALLOCATION_CPU_VISIBLE | TG_GPU_ALLOCATION_DISPLAY_PRIMARY, 3686400},
         D3DDDIFMT_A8R8G8B8, {60, 1}, rotation,
         quarter ? 1280u : 720u, quarter ? 720u : 1280u, quarter ? 5120u : 2880u,
         rotation == 1 ? 4u : rotation}; /* NONPREROTATED storage is independent of mode rotation. */
      RuntimeResource *res = nullptr;
      if (opened) {
         D3DDDI_OPENALLOCATIONINFO allocation = {};
         allocation.hAllocation = 0x60; allocation.pPrivateDriverData = &primary;
         allocation.PrivateDriverDataSize = sizeof(primary);
         D3D10DDIARG_OPENRESOURCE open = {};
         open.NumAllocations = 1; open.pOpenAllocationInfo = &allocation;
         RuntimeSharedDescription description;
         CHECK(runtime_shared_open(&dev, &open, {rt_resource(0)}, &res, &description) == S_OK);
      } else {
         D3D10DDI_MIPINFO mip = {primary.Width, primary.Height, 1, 0, 0, 1};
         DXGI_DDI_PRIMARY_DESC desc = {};
         desc.Flags = rotation == 1 ? DXGI_DDI_PRIMARY_NONPREROTATED : 0;
         desc.ModeDesc.Rotation = (DXGI_DDI_MODE_ROTATION)primary.ModeRotation;
         desc.ModeDesc.Width = primary.Width; desc.ModeDesc.Height = primary.Height;
         desc.ModeDesc.RefreshRate = {60, 1};
         D3D10DDIARG_CREATERESOURCE create = {};
         create.pMipInfoList = &mip; create.pPrimaryDesc = &desc;
         create.ResourceDimension = D3D10DDIRESOURCE_TEXTURE2D;
         create.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
         create.MipLevels = create.ArraySize = create.SampleDesc.Count = 1;
         UINT w, h, pitch;
         CHECK(runtime_primary_create(&dev, &create, {rt_resource(0)}, &res, &w, &h, &pitch) == S_OK);
         CHECK(w == primary.Width && h == primary.Height && pitch == primary.Pitch);
      }
      CHECK(runtime_set_display_mode(res, &dev, 1) == E_INVALIDARG && !privateModeCalls);
      CHECK(runtime_set_display_mode(res, &dev, 0) == c.expected);
      CHECK(privateModeCalls == c.modes && privateFormatCalls == c.formats);
      CHECK(runtime_allocation(res) == 0x60 && dev.submitted == 0);
      if (c.formats && SUCCEEDED(c.change)) CHECK(sourceFormat == wantedFormat);
      CHECK(runtime_destroy(res) == S_OK);
      runtime_device_destroy(&dev);
   }
   std::puts("PASS: primary source-format negotiation covers four storage layouts, bounded retry and failure propagation (CPU control)");
}

static void primary_mode_completion_contract()
{
   const struct {
      uint64_t completed;
      VkResult wait;
      unsigned waits;
   } cases[] = {
      {0, VK_SUCCESS, 1},
      {1, VK_SUCCESS, 0},
      {0, VK_TIMEOUT, 30},
      {0, VK_ERROR_DEVICE_LOST, 1},
   };
   for (const auto &c : cases) {
      Adapter adapter; Device dev;
      init(&dev, &adapter, 0);
      modeCompleted = c.completed; modeWaitResult = c.wait;
      modeWaits = modeCallbacks = modeErrors = 0;
      dev.submitted = dev.gated = 1;
      dev.fence = (VkSemaphore)3;
      dev.vk.GetSemaphoreCounterValue = [](VkDevice, VkSemaphore fence, uint64_t *out) {
         CHECK(fence == (VkSemaphore)3); *out = modeCompleted; return VK_SUCCESS;
      };
      dev.vk.WaitSemaphores = [](VkDevice, const VkSemaphoreWaitInfo *wait, uint64_t timeout) {
         CHECK(wait->semaphoreCount == 1 && *wait->pSemaphores == (VkSemaphore)3);
         CHECK(*wait->pValues == 1 && timeout == UINT64_C(1000000000));
         ++modeWaits;
         if (modeWaitResult == VK_SUCCESS)
            modeCompleted = 1; /* Simulated GPU completion, never a CPU signal. */
         return modeWaitResult;
      };
      dev.um.pfnSetErrorCb = [](D3D10DDI_HRTCORELAYER, HRESULT hr) {
         CHECK(hr == D3DDDIERR_DEVICEREMOVED); ++modeErrors;
      };
      dev.kt.pfnSetDisplayModeCb = [](HANDLE h, D3DDDICB_SETDISPLAYMODE *mode) -> HRESULT {
         CHECK(h == rt_device(0) && mode->hPrimaryAllocation == 0x60);
         CHECK(modeCompleted == 1); ++modeCallbacks; return S_OK;
      };
      tg_gpu_primary_allocation_request primary = {
         {TG_GPU_ALLOCATION_ABI, TG_GPU_ALLOCATION_CPU_VISIBLE | TG_GPU_ALLOCATION_DISPLAY_PRIMARY, 3686400},
         D3DDDIFMT_A8R8G8B8, {60, 1}, D3DDDI_ROTATION_IDENTITY, 720, 1280, 2880, D3DDDI_ROTATION_270};
      D3DDDI_OPENALLOCATIONINFO allocation = {};
      allocation.hAllocation = 0x60; allocation.pPrivateDriverData = &primary;
      allocation.PrivateDriverDataSize = sizeof(primary);
      D3D10DDIARG_OPENRESOURCE open = {};
      open.NumAllocations = 1; open.pOpenAllocationInfo = &allocation;
      RuntimeSharedDescription description;
      Resource res; res.dev = &dev;
      CHECK(runtime_shared_open(&dev, &open, {rt_resource(0)}, &res.storage.runtime, &description) == S_OK);
      DXGI1_1_DDI_BASE_FUNCTIONS dxgi = {}; dxgi_fill_table(&dxgi, true);
      DXGI_DDI_ARG_SETDISPLAYMODE mode = {};
      mode.hDevice = (DXGI_DDI_HDEVICE)&dev; mode.hResource = (DXGI_DDI_HRESOURCE)&res;
      mode.SubResourceIndex = 1;
      CHECK(dxgi.pfnSetDisplayMode(&mode) == E_INVALIDARG && !modeWaits && !modeCallbacks);
      mode.SubResourceIndex = 0;
      Device foreign;
      mode.hDevice = (DXGI_DDI_HDEVICE)&foreign;
      CHECK(dxgi.pfnSetDisplayMode(&mode) == E_INVALIDARG && !modeWaits && !modeCallbacks);
      mode.hDevice = (DXGI_DDI_HDEVICE)&dev;
      const HRESULT hr = dxgi.pfnSetDisplayMode(&mode);
      CHECK(modeWaits == c.waits && dev.submitted == 1);
      if (c.wait == VK_SUCCESS) {
         CHECK(hr == S_OK && modeCallbacks == 1 && !modeErrors && modeCompleted == 1);
      } else {
         CHECK(hr == DXGI_ERROR_DEVICE_REMOVED && dev.lost == hr);
         CHECK(!modeCallbacks && modeErrors == 1 && !modeCompleted);
      }
      /* This fixture imports runtime metadata only: no real GPU backing or
       * in-flight work exists to release after a simulated wait failure. */
      CHECK(runtime_destroy(res.storage.runtime) == S_OK); res.storage.runtime = nullptr;
      runtime_device_destroy(&dev);
   }
   std::puts("PASS: SetDisplayMode waits for submitted GPU work; completed work does not wait; timeout/removal cannot publish the primary (CPU control)");
}

// These fixtures exercise the real Present1/MPO implementations with runtime
// callbacks and Vulkan substitutes. They never create a GPU or display device.
static std::vector<unsigned> presentOrder;
static UINT presentFailure;
static DWORD presentThread;
static bool presentPlaneEnabled;
static HRESULT modernPresentStatus;

static void presentation13_contracts()
{
   for (UINT angle = D3DDDI_ROTATION_IDENTITY; angle <= D3DDDI_ROTATION_270; ++angle)
   for (UINT failure = 0; failure < 6; ++failure) {
      Adapter adapter; Device dev;
      init(&dev, &adapter, 0);
      const bool quarter = angle == D3DDDI_ROTATION_90 || angle == D3DDDI_ROTATION_270;
      tg_gpu_primary_allocation_request primary = {
         {TG_GPU_ALLOCATION_ABI, TG_GPU_ALLOCATION_CPU_VISIBLE | TG_GPU_ALLOCATION_DISPLAY_PRIMARY, 3686400},
         D3DDDIFMT_A8R8G8B8, {60, 1}, angle, quarter ? 1280u : 720u,
         quarter ? 720u : 1280u, quarter ? 5120u : 2880u, angle};
      D3DDDI_OPENALLOCATIONINFO allocation = {};
      allocation.hAllocation = 0x60; allocation.pPrivateDriverData = &primary;
      allocation.PrivateDriverDataSize = sizeof(primary);
      D3D10DDIARG_OPENRESOURCE open = {};
      open.NumAllocations = 1; open.pOpenAllocationInfo = &allocation;
      RuntimeSharedDescription description;
      Resource res; res.dev = &dev; res.kind = ResKind::Image;
      CHECK(runtime_shared_open(&dev, &open, {rt_resource(0)}, &res.storage.runtime, &description) == S_OK);
      res.width = description.width; res.height = description.height; res.pitch = description.pitch;
      res.format = description.format; res.storage.image = (VkImage)5;
      CHECK(direct_flip_compatible(&dev, &res, &res));
      // Same extent with an opposite scan direction must not be accepted by
      // CheckDirectFlipSupport merely because its pitch and format match.
      Resource other = res;
      primary.Rotation = angle <= D3DDDI_ROTATION_90 ? angle + 2 : angle - 2;
      CHECK(runtime_shared_open(&dev, &open, {rt_resource(0)}, &other.storage.runtime, &description) == S_OK);
      CHECK(!direct_flip_compatible(&dev, &res, &other));
      CHECK(runtime_destroy(other.storage.runtime) == S_OK);
      other = res; other.pitch += 64; CHECK(!direct_flip_compatible(&dev, &res, &other));
      other = res; other.dev = nullptr; CHECK(!direct_flip_compatible(&dev, &res, &other));
      other = res; other.storage.runtime = nullptr; CHECK(!direct_flip_compatible(&dev, &res, &other));
      other = res; other.format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
      CHECK(!direct_flip_compatible(&dev, &res, &other));

      DXGI1_3_DDI_BASE_FUNCTIONS dxgi = {};
      dxgi_fill_presentation13(&dxgi);
      CHECK(!dxgi.pfnReserved1 && !dxgi.pfnReserved2);
      DXGI_DDI_ARG_GETMULTIPLANEOVERLAYCAPS caps = {};
      caps.hDevice = (DXGI_DDI_HDEVICE)&dev;
      CHECK(dxgi.pfnGetMultiplaneOverlayCaps(&caps) == S_OK &&
            caps.MultiplaneOverlayCaps.MaxPlanes == 1 && caps.MultiplaneOverlayCaps.NumCapabilityGroups == 1);
      DXGI_DDI_ARG_GETMULTIPLANEOVERLAYGROUPCAPS group = {};
      group.hDevice = caps.hDevice;
      CHECK(dxgi.pfnGetMultiplaneOverlayGroupCaps(&group) == S_OK);
      CHECK(group.MultiplaneOverlayGroupCaps.NumPlanes == 1 &&
            group.MultiplaneOverlayGroupCaps.OverlayCaps ==
            (DXGI_DDI_MULTIPLANE_OVERLAY_FEATURE_CAPS_RGB |
             DXGI_DDI_MULTIPLANE_OVERLAY_FEATURE_CAPS_ROTATION_WITHOUT_INDEPENDENT_FLIP));
      CHECK(group.MultiplaneOverlayGroupCaps.MaxStretchFactor == 1.0f &&
            group.MultiplaneOverlayGroupCaps.MaxShrinkFactor == 1.0f);
      group.GroupIndex = 1;
      CHECK(dxgi.pfnGetMultiplaneOverlayGroupCaps(&group) == E_INVALIDARG);
      DXGI_DDI_ARG_CHECKPRESENTDURATIONSUPPORT duration = {};
      duration.hDevice = caps.hDevice; duration.DesiredPresentDuration = 100000;
      duration.ClosestSmallerDuration = duration.ClosestLargerDuration = UINT_MAX;
      CHECK(dxgi.pfnCheckPresentDurationSupport(&duration) == S_OK &&
            !duration.ClosestSmallerDuration && !duration.ClosestLargerDuration);

      DXGI_DDI_BASE_CALLBACKS callbacks = {};
      callbacks.pfnPresentMultiplaneOverlayCb = [](HANDLE device,
            const DXGIDDICB_PRESENT_MULTIPLANE_OVERLAY *present) -> HRESULT {
         CHECK(GetCurrentThreadId() == presentThread);
         CHECK(device == rt_device(0) && present->hContext == (HANDLE)0x300 &&
               present->pDXGIContext == (void *)0xdeadbeef && !present->BroadcastContextCount);
         CHECK(present->AllocationInfoCount == 1 && !present->AllocationInfo[0].SubResourceIndex);
         CHECK(present->AllocationInfo[0].PresentAllocation == (presentPlaneEnabled ? 0x60u : 0u));
         presentOrder.push_back(4);
         return modernPresentStatus;
      };
      dev.dxgi = &callbacks;
      presentThread = GetCurrentThreadId(); presentFailure = failure;
      presentPlaneEnabled = true; modernPresentStatus = failure == 4 ? DXGI_ERROR_DEVICE_REMOVED : S_OK;
      presentOrder.clear();
      dev.um.pfnSetErrorCb = [](D3D10DDI_HRTCORELAYER, HRESULT hr) { CHECK(hr == D3DDDIERR_DEVICEREMOVED); };
      dev.fence = (VkSemaphore)8; dev.runtimeFence = 0x70;
      dev.ingress = (VkSemaphore)9; dev.runtimeIngress = 0x71;
      dev.vk.WaitSemaphores = [](VkDevice, const VkSemaphoreWaitInfo *, uint64_t) -> VkResult {
         CHECK(false && "MPO must queue GPU synchronization, not wait for this frame on the CPU");
         return VK_ERROR_DEVICE_LOST;
      };
      dev.vk.GetSemaphoreCounterValue = [](VkDevice, VkSemaphore fence, uint64_t *value) {
         CHECK(fence == (VkSemaphore)8); *value = 0;
         return presentFailure == 5 ? VK_ERROR_DEVICE_LOST : VK_SUCCESS;
      };
      dev.kt.pfnSignalSynchronizationObjectFromGpuCb = [](HANDLE device,
            const D3DDDICB_SIGNALSYNCHRONIZATIONOBJECTFROMGPU *signal) -> HRESULT {
         CHECK(device == rt_device(0) && signal->hContext == (HANDLE)0x300);
         CHECK(signal->ObjectCount == 1 && *signal->ObjectHandleArray == 0x71 &&
               *signal->MonitoredFenceValueArray == 1);
         presentOrder.push_back(1);
         return presentFailure == 1 ? E_FAIL : S_OK;
      };
      dev.vk.QueueSubmit2 = [](VkQueue, uint32_t count, const VkSubmitInfo2 *submit, VkFence) -> VkResult {
         CHECK(count == 1 && submit->waitSemaphoreInfoCount == 1 && submit->signalSemaphoreInfoCount == 1);
         CHECK(submit->pWaitSemaphoreInfos[0].semaphore == (VkSemaphore)9 &&
               submit->pWaitSemaphoreInfos[0].value == 1 &&
               submit->pWaitSemaphoreInfos[0].stageMask == VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
         CHECK(submit->pSignalSemaphoreInfos[0].semaphore == (VkSemaphore)8 &&
               submit->pSignalSemaphoreInfos[0].value == 1);
         presentOrder.push_back(2);
         return presentFailure == 2 ? VK_ERROR_DEVICE_LOST : VK_SUCCESS;
      };
      dev.kt.pfnWaitForSynchronizationObjectFromGpuCb = [](HANDLE device,
            const D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMGPU *wait) -> HRESULT {
         CHECK(device == rt_device(0) && wait->hContext == (HANDLE)0x300);
         CHECK(wait->ObjectCount == 1 && *wait->ObjectHandleArray == 0x70 &&
               *wait->MonitoredFenceValueArray == 1);
         presentOrder.push_back(3);
         return presentFailure == 3 ? E_FAIL : S_OK;
      };
      dev.vk.CmdPipelineBarrier2 = [](VkCommandBuffer, const VkDependencyInfo *) {};
      dev.vk.EndCommandBuffer = [](VkCommandBuffer) { return VK_SUCCESS; };
      DXGI_DDI_PRESENT_MULTIPLANE_OVERLAY plane = {};
      plane.Enabled = TRUE; plane.hResource = (DXGI_DDI_HRESOURCE)&res;
      plane.PlaneAttributes.SrcRect = {0, 0, (LONG)res.width, (LONG)res.height};
      plane.PlaneAttributes.DstRect = plane.PlaneAttributes.ClipRect = {0, 0, 720, 1280};
      plane.PlaneAttributes.Rotation = (DXGI_DDI_MODE_ROTATION)angle;
      DXGI_DDI_ARG_PRESENTMULTIPLANEOVERLAY present = {};
      present.hDevice = (DXGI_DDI_HDEVICE)&dev; present.pDXGIContext = (void *)0xdeadbeef;
      present.Flags.Flip = 1; present.FlipInterval = DXGI_DDI_FLIP_INTERVAL_ONE;
      present.PresentPlaneCount = 1; present.pPresentPlanes = &plane;
      const auto good = plane;
      for (unsigned bad = 0; bad < 7; ++bad) {
         plane = good;
         switch (bad) {
         case 0: plane.LayerIndex = 1; break;
         case 1: plane.SubResourceIndex = 1; break;
         case 2: plane.PlaneAttributes.DstRect.right--; break;
         case 3: plane.PlaneAttributes.SrcRect.right--; break;
         case 4: plane.PlaneAttributes.Blend = DXGI_DDI_MULTIPLANE_OVERLAY_BLEND_ALPHABLEND; break;
         case 5: plane.PlaneAttributes.Rotation = DXGI_DDI_MODE_ROTATION_UNSPECIFIED; break;
         case 6: plane.hResource = 0; break;
         }
         CHECK(dxgi.pfnPresentMultiplaneOverlay(&present) == DXGI_ERROR_UNSUPPORTED && presentOrder.empty());
      }
      plane = good;
      present.FlipInterval = DXGI_DDI_FLIP_INTERVAL_IMMEDIATE;
      CHECK(dxgi.pfnPresentMultiplaneOverlay(&present) == DXGI_ERROR_UNSUPPORTED && presentOrder.empty());
      present.FlipInterval = DXGI_DDI_FLIP_INTERVAL_ONE;
      present.Flags.AllowTearing = 1;
      CHECK(dxgi.pfnPresentMultiplaneOverlay(&present) == DXGI_ERROR_UNSUPPORTED && presentOrder.empty());
      present.Flags.AllowTearing = 0;
      dev.cmd = (VkCommandBuffer)0x19; dev.cmdHasWork = true;
      const HRESULT result = dxgi.pfnPresentMultiplaneOverlay(&present);
      const std::vector<unsigned> expected = failure == 1 ? std::vector<unsigned>{1} :
         failure == 2 ? std::vector<unsigned>{1, 2} :
         failure == 3 || failure == 5 ? std::vector<unsigned>{1, 2, 3} :
         std::vector<unsigned>{1, 2, 3, 4};
      CHECK(presentOrder == expected && result == (failure ? DXGI_ERROR_DEVICE_REMOVED : S_OK));
      CHECK(dev.completed() == 0 && dev.submittedCommands.size() == (failure == 1 || failure == 2 ? 0u : 1u));
      if (!failure) {
         presentOrder.clear(); presentPlaneEnabled = false;
         plane.Enabled = FALSE; plane.hResource = 0;
         CHECK(dxgi.pfnPresentMultiplaneOverlay(&present) == S_OK && presentOrder == std::vector<unsigned>{4});
         CHECK(dev.submitted == 1 && dev.gated == 1 && dev.completed() == 0);
      }
      // Only simulated commands exist in this fixture; no backing is in flight.
      dev.cmd = VK_NULL_HANDLE; dev.cmdHasWork = false; dev.submittedCommands.clear();
      CHECK(runtime_destroy(res.storage.runtime) == S_OK);
      runtime_device_destroy(&dev);
   }
   std::puts("PASS: MPO four rotations validate exact layouts; runtime signal -> Vulkan wait/submit -> runtime GPU wait -> same-thread Present callback; no CPU completion or early retirement (CPU control)");

   Adapter adapter; Device dev;
   init(&dev, &adapter, 0);
   D3D10DDI_MIPINFO mip = {65,17,1,65,17,1};
   D3D10DDIARG_CREATERESOURCE desc = {};
   desc.ResourceDimension = D3D10DDIRESOURCE_TEXTURE2D; desc.pMipInfoList = &mip;
   desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
   desc.MiscFlags = D3D10_DDI_RESOURCE_MISC_SHARED;
   Resource surface; surface.dev = &dev; surface.width = 65; surface.height = 17;
   CHECK(runtime_shared_create(&dev, &desc, {rt_resource(0)}, &surface.storage.runtime, &surface.pitch) == S_OK);
   DXGI_DDI_BASE_CALLBACKS callbacks = {};
   callbacks.pfnPresentCb = [](HANDLE device, DXGIDDICB_PRESENT *present) -> HRESULT {
      CHECK(GetCurrentThreadId() == presentThread && device == rt_device(0));
      CHECK(present->hContext == (HANDLE)0x300 && present->hSrcAllocation == 0x60 && !present->hDstAllocation);
      CHECK(present->pDXGIContext == (void *)0xbeef && present->SyncIntervalOverrideValid &&
            present->SyncIntervalOverride == DXGI_DDI_FLIP_INTERVAL_ONE);
      presentOrder.push_back(4); return DXGI_STATUS_OCCLUDED;
   };
   dev.dxgi = &callbacks; presentThread = GetCurrentThreadId(); presentOrder.clear();
   DXGI1_3_DDI_BASE_FUNCTIONS dxgi = {}; dxgi_fill_presentation13(&dxgi);
   DXGI_DDI_ARG_PRESENTSURFACE source = {(DXGI_DDI_HRESOURCE)&surface, 0};
   RECT dirty[] = {{0, 0, 10, 9}, {10, 9, 65, 17}};
   DXGI_DDI_ARG_PRESENT1 present = {};
   present.hDevice = (DXGI_DDI_HDEVICE)&dev; present.phSurfacesToPresent = &source;
   present.SurfacesToPresent = 1; present.Flags.Blt = 1; present.pDXGIContext = (void *)0xbeef;
   present.DirtyRects = 2; present.pDirtyRects = dirty; present.BackBufferMultiplicity = 0x12345678;
   CHECK(dxgi.pfnPresent1(&present) == DXGI_STATUS_OCCLUDED && presentOrder == std::vector<unsigned>{4});
   CHECK(present.BackBufferMultiplicity == 0x12345678); // WDDM 1.3 may not touch this later field.
   presentOrder.clear(); dirty[1].right = 66;
   CHECK(dxgi.pfnPresent1(&present) == E_INVALIDARG && presentOrder.empty());
   dirty[1].right = 65; present.SurfacesToPresent = 2;
   CHECK(dxgi.pfnPresent1(&present) == E_INVALIDARG && presentOrder.empty());
   present.SurfacesToPresent = 1; surface.dev = nullptr;
   CHECK(dxgi.pfnPresent1(&present) == E_INVALIDARG && presentOrder.empty());
   surface.dev = &dev;
   CHECK(runtime_destroy(surface.storage.runtime) == S_OK);
   runtime_device_destroy(&dev);
   std::puts("PASS: Present1 preserves opaque context/status and one callback for dirty rects; validates bounds/ownership and leaves WDDM2 fields untouched (CPU control)");
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
         /* Rotated direct primaries carry matching storage; shadows remain
          * logical copy sources and cannot themselves request DC rotation. */
         const HRESULT rotated = runtime_shared_open(&consumer, &so, {rt_resource(1)}, &opened, &description);
         if (primary) {
            CHECK(rotated == S_OK && opened && runtime_is_primary(opened));
            CHECK(description.width == 1280 && description.height == 720 && description.pitch == 5120);
            CHECK(allocations == allocated_before_standard && runtime_allocation(opened) == 0x61);
            CHECK(SUCCEEDED(runtime_destroy(opened)));
         } else {
            CHECK(rotated == DXGI_ERROR_UNSUPPORTED && !opened);
         }
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
   primary_private_format_contract();
   primary_mode_completion_contract();
   presentation13_contracts();
}
