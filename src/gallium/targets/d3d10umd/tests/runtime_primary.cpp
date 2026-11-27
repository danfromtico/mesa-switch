/* SPDX-License-Identifier: MIT */
/* Real runtime-owner implementation with controlled WDDM callbacks. */
#include "RuntimeMemory.h"
#include "State.h"
#include "nouveau/vulkan/nvkmd/windows/allocation_abi.h"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %u: %s\n", __LINE__, #x); exit(1); } } while (0)
static HANDLE device_handle = reinterpret_cast<HANDLE>(0x10);
static HANDLE resource_handle = reinterpret_cast<HANDLE>(0x20);
static unsigned queues, allocations, waits, gpu_maps, gpu_frees, locks, modes;
static unsigned failure;
static UINT64 paging_value;
static HRESULT mode_result = S_OK, map_result = S_OK, resident_result = S_OK;
static UINT expected_rotation = D3DDDI_ROTATION_IDENTITY, expected_width = 720, expected_height = 1280;
static char cpu_bytes[16];
static pipe_screen screen = {};
static pipe_context context = {};

extern "C" void D3D10Trace(d3d10_umd_trace_event event, HRESULT, uint64_t arg0, uint64_t)
{
   if (event == D3D10_TRACE_PRIMARY_LAYOUT) return; /* Input mode metadata. */
   CHECK(event == D3D10_TRACE_RUNTIME_MEMORY && (arg0 >> 32) >= 1 && (arg0 >> 32) <= 10);
}

static HRESULT APIENTRY create_queue(HANDLE device, D3DDDICB_CREATEPAGINGQUEUE *q)
{
   CHECK(device == device_handle && q->Priority == D3DDDI_PAGINGQUEUE_PRIORITY_NORMAL);
   if (failure == 1) return E_ACCESSDENIED;
   CHECK(!queues++);
   q->hPagingQueue = 0x30; q->hSyncObject = 0x40;
   q->FenceValueCPUVirtualAddress = &paging_value;
   return S_OK;
}
static HRESULT APIENTRY destroy_queue(HANDLE device, const D3DDDI_DESTROYPAGINGQUEUE *q)
{
   CHECK(device == device_handle && q->hPagingQueue == 0x30 && queues == 1);
   --queues; return S_OK;
}
static HRESULT APIENTRY allocate(HANDLE device, D3DDDICB_ALLOCATE *a)
{
   CHECK(device == device_handle && a->hResource == resource_handle && a->NumAllocations == 1);
   CHECK(!a->pPrivateDriverData && !a->PrivateDriverDataSize);
   auto i = a->pAllocationInfo2;
   CHECK(i && i->Flags.Primary && !i->VidPnSourceId && !i->pSystemMem);
   CHECK(i->PrivateDriverDataSize == sizeof(tg_gpu_primary_allocation_request));
   auto desc = static_cast<const tg_gpu_allocation_request *>(i->pPrivateDriverData);
   CHECK(desc->version == 1 && desc->flags == 3 && desc->bytes == 3686400);
   auto layout = static_cast<const tg_gpu_primary_allocation_request *>(i->pPrivateDriverData);
   CHECK(layout->Format == D3DDDIFMT_A8R8G8B8 && layout->Rotation == expected_rotation &&
         layout->Width == expected_width && layout->Height == expected_height &&
         layout->Pitch == expected_width * 4 && !layout->Reserved);
   if (failure == 2) return E_OUTOFMEMORY;
   CHECK(!allocations++);
   i->hAllocation = failure == 5 ? 0 : 0x50;
   /* A usable allocation does not require a returned hKMResource. */
   a->hKMResource = 0;
   return S_OK;
}
static HRESULT APIENTRY deallocate(HANDLE device, const D3DDDICB_DEALLOCATE *a)
{
   CHECK(device == device_handle && a->hResource == resource_handle);
   CHECK(!a->NumAllocations && !a->HandleList && allocations == 1);
   --allocations; return S_OK;
}
static HRESULT APIENTRY resident(HANDLE device, D3DDDI_MAKERESIDENT *r)
{
   CHECK(device == device_handle && r->hPagingQueue == 0x30 && r->NumAllocations == 1);
   CHECK(r->AllocationList[0] == 0x50 && allocations == 1);
   if (failure == 3) return D3DDDIERR_OUTOFVIDEOMEMORY;
   r->PagingFenceValue = 7; return resident_result;
}
static HRESULT APIENTRY wait_cpu(HANDLE device, const D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMCPU *w)
{
   CHECK(device == device_handle && !w->hAsyncEvent && w->ObjectCount == 1);
   CHECK(w->ObjectHandleArray[0] == 0x40);
   ++waits;
   if (failure == 4) return D3DDDIERR_DEVICEREMOVED;
   paging_value = w->FenceValueArray[0]; return S_OK;
}
static HRESULT APIENTRY map_gpu(HANDLE device, D3DDDI_MAPGPUVIRTUALADDRESS *m)
{
   CHECK(device == device_handle && m->hPagingQueue == 0x30 && m->hAllocation == 0x50);
   CHECK(m->BaseAddress == 0x40000 && m->OffsetInPages == 1 && m->SizeInPages == 2);
   CHECK(!m->DriverProtection && m->Protection.Write);
   ++gpu_maps;
   if (FAILED(map_result) && map_result != E_PENDING) return map_result;
   m->VirtualAddress = m->BaseAddress; m->PagingFenceValue = 9; return map_result;
}
static HRESULT APIENTRY lock_cpu(HANDLE device, D3DDDICB_LOCK2 *l)
{
   CHECK(device == device_handle && l->hAllocation == 0x50 && !locks++);
   l->pData = cpu_bytes; return S_OK;
}
static HRESULT APIENTRY free_gpu(HANDLE device, const D3DDDICB_FREEGPUVIRTUALADDRESS *f)
{
   CHECK(device == device_handle && f->BaseAddress == 0x40000 && f->Size == 0x4000);
   CHECK(allocations == 1);
   ++gpu_frees; return S_OK;
}
static HRESULT APIENTRY unlock_cpu(HANDLE device, const D3DDDICB_UNLOCK2 *l)
{
   CHECK(device == device_handle && l->hAllocation == 0x50 && locks == 1);
   --locks; return S_OK;
}
static HRESULT APIENTRY set_mode(HANDLE device, D3DDDICB_SETDISPLAYMODE *m)
{
   CHECK(device == device_handle && m->hPrimaryAllocation == 0x50 && allocations == 1);
   ++modes; m->PrivateDriverFormatAttribute = 0x1234; return mode_result;
}

int main()
{
   Device device = {};
   device.hDevice = device_handle; device.pipe = &context; context.screen = &screen;
   screen.get_device_luid = [](pipe_screen *, char *luid) { const uint64_t id = 0x1234567890ull; memcpy(luid, &id, sizeof(id)); };
   auto &cb = device.KTCallbacks;
   cb.pfnCreatePagingQueueCb = create_queue; cb.pfnDestroyPagingQueueCb = destroy_queue;
   cb.pfnAllocateCb = allocate; cb.pfnDeallocateCb = deallocate;
   cb.pfnMakeResidentCb = resident; cb.pfnWaitForSynchronizationObjectFromCpuCb = wait_cpu;
   cb.pfnMapGpuVirtualAddressCb = map_gpu; cb.pfnLock2Cb = lock_cpu; cb.pfnUnlock2Cb = unlock_cpu;
   cb.pfnFreeGpuVirtualAddressCb = free_gpu;
   cb.pfnSetDisplayModeCb = set_mode;
   D3D10DDI_MIPINFO mip = {}; mip.TexelWidth = 720; mip.TexelHeight = 1280; mip.TexelDepth = 1;
   DXGI_DDI_PRIMARY_DESC primary = {};
   primary.ModeDesc.Width = 720; primary.ModeDesc.Height = 1280;
   primary.ModeDesc.RefreshRate.Numerator = 60; primary.ModeDesc.RefreshRate.Denominator = 1;
   D3D10DDIARG_CREATERESOURCE desc = {};
   desc.ResourceDimension = D3D10DDIRESOURCE_TEXTURE2D; desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
   desc.ArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
   desc.pMipInfoList = &mip; desc.pPrimaryDesc = &primary;
   D3D10DDI_HRTRESOURCE runtime = {resource_handle};
   RuntimeResource *p = NULL;
   const HRESULT expected[] = {S_OK, E_ACCESSDENIED, E_OUTOFMEMORY,
      D3DDDIERR_OUTOFVIDEOMEMORY, D3DDDIERR_DEVICEREMOVED, E_FAIL};
   for (failure = 1; failure < ARRAYSIZE(expected); ++failure) {
      paging_value = 0;
      CHECK(RuntimePrimaryCreate(&device, &desc, runtime, &p) == expected[failure]);
      CHECK(!p && !queues && !allocations);
   }
   /* The real runtime returned E_PENDING for residency. Both paging
    * callbacks may queue work, but a failed wait must still fail creation. */
   resident_result = E_PENDING;
   failure = 4; paging_value = 0;
   CHECK(RuntimePrimaryCreate(&device, &desc, runtime, &p) == D3DDDIERR_DEVICEREMOVED);
   CHECK(!p && !queues && !allocations);
   failure = 0; paging_value = 0;
   const unsigned resident_waits = waits;
   CHECK(RuntimePrimaryCreate(&device, &desc, runtime, &p) == S_OK);
   auto memory = *RuntimeResourceMemory(p);
   CHECK(memory.adapter_luid == 0x1234567890ull && memory.size == 3686400 && memory.allocation == 0x50);
   CHECK(paging_value == 7 && waits == resident_waits + 1);
   const unsigned before = waits;
   CHECK(memory.map_gpu(memory.context, 0x40001, 4096, 8192, 0, false, 0x40000, 0x4000) == VK_ERROR_UNKNOWN);
   CHECK(memory.map_gpu(memory.context, 0x40000, memory.size, 4096, 0, false, 0x40000, 0x4000) == VK_ERROR_UNKNOWN);
   CHECK(!gpu_maps && waits == before);
   map_result = E_PENDING;
   CHECK(memory.map_gpu(memory.context, 0x40000, 4096, 8192, 0, false, 0x40000, 0x4000) == VK_SUCCESS);
   CHECK(gpu_maps == 1 && waits == before + 1 && paging_value == 9);
   paging_value = 7; failure = 4;
   CHECK(memory.map_gpu(memory.context, 0x40000, 4096, 8192, 0, false, 0x40000, 0x4000) == VK_ERROR_DEVICE_LOST);
   CHECK(RuntimeResourceError(p) == D3DDDIERR_DEVICEREMOVED && paging_value == 7);
   failure = 0;
   map_result = DXGI_ERROR_DEVICE_REMOVED;
   CHECK(memory.map_gpu(memory.context, 0x40000, 4096, 8192, 0, false, 0x40000, 0x4000) == VK_ERROR_DEVICE_LOST);
   CHECK(RuntimeResourceError(p) == DXGI_ERROR_DEVICE_REMOVED);
   void *cpu = NULL;
   CHECK(memory.map_cpu(memory.context, &cpu) == VK_SUCCESS && cpu == cpu_bytes);
   memory.unmap_cpu(memory.context); CHECK(!locks);
   CHECK(RuntimePrimarySetMode(p, reinterpret_cast<HANDLE>(0x11), 0) == E_INVALIDARG);
   CHECK(RuntimePrimarySetMode(p, device_handle, 1) == E_INVALIDARG && !modes);
   mode_result = D3DDDIERR_INCOMPATIBLEPRIVATEFORMAT;
   CHECK(RuntimePrimarySetMode(p, device_handle, 0) == mode_result && modes == 1);
   mode_result = S_OK;
   CHECK(RuntimePrimarySetMode(p, device_handle, 0) == S_OK && modes == 2);
   memory.retain(memory.context);
   CHECK(memory.map_cpu(memory.context, &cpu) == VK_SUCCESS && locks == 1);
   CHECK(RuntimeResourceDestroy(p) == S_OK && !locks && !queues && !allocations && gpu_frees == 1);
   CHECK(memory.map_cpu(memory.context, &cpu) == VK_ERROR_DEVICE_LOST);
   CHECK(memory.map_gpu(memory.context, 0x40000, 4096, 8192, 0, false, 0x40000, 0x4000) == VK_ERROR_DEVICE_LOST);
   CHECK(memory.free_gpu(memory.context, 0x40000, 0x4000) == VK_SUCCESS && gpu_frees == 1);
   memory.release(memory.context);
   resident_result = map_result = S_OK;
   for (UINT rotation = 1; rotation <= 4; ++rotation) {
      const bool quarter = rotation == 2 || rotation == 4;
      expected_rotation = rotation;
      mip.TexelWidth = primary.ModeDesc.Width = expected_width = quarter ? 1280 : 720;
      mip.TexelHeight = primary.ModeDesc.Height = expected_height = quarter ? 720 : 1280;
      primary.ModeDesc.Rotation = static_cast<DXGI_DDI_MODE_ROTATION>(rotation);
      CHECK(RuntimePrimaryCreate(&device, &desc, runtime, &p) == S_OK);
      CHECK(RuntimeResourcePitch(p) == expected_width * 4 && RuntimeResourceMemory(p)->size == 3686400);
      CHECK(RuntimeResourceDestroy(p) == S_OK && !queues && !allocations);
      if (quarter) {
         /* The mode can retain its unrotated extent; storage follows mip info. */
         primary.ModeDesc.Width = 720; primary.ModeDesc.Height = 1280;
         CHECK(RuntimePrimaryCreate(&device, &desc, runtime, &p) == S_OK);
         CHECK(RuntimeResourcePitch(p) == 5120);
         CHECK(RuntimeResourceDestroy(p) == S_OK && !queues && !allocations);
      }
   }
   expected_rotation = D3DDDI_ROTATION_IDENTITY;
   mip.TexelWidth = expected_width = 720; mip.TexelHeight = expected_height = 1280;
   primary.ModeDesc.Width = 1280; primary.ModeDesc.Height = 720;
   primary.ModeDesc.Rotation = DXGI_DDI_MODE_ROTATION_ROTATE90;
   primary.Flags = DXGI_DDI_PRIMARY_NONPREROTATED;
   CHECK(RuntimePrimaryCreate(&device, &desc, runtime, &p) == S_OK && RuntimeResourcePitch(p) == 2880);
   CHECK(RuntimeResourceDestroy(p) == S_OK && !queues && !allocations);
   primary.Flags = 0;
   CHECK(RuntimePrimaryCreate(&device, &desc, runtime, &p) == DXGI_ERROR_UNSUPPORTED && !p);
   mip.TexelWidth = 1280; mip.TexelHeight = 720;
   primary.ModeDesc.Width = 640;
   CHECK(RuntimePrimaryCreate(&device, &desc, runtime, &p) == DXGI_ERROR_UNSUPPORTED && !p);
   primary.ModeDesc.Width = 1280; primary.Flags = DXGI_DDI_PRIMARY_STEREO;
   CHECK(RuntimePrimaryCreate(&device, &desc, runtime, &p) == DXGI_ERROR_UNSUPPORTED && !p);
   primary.Flags = 0; primary.ModeDesc.Rotation = static_cast<DXGI_DDI_MODE_ROTATION>(5);
   CHECK(RuntimePrimaryCreate(&device, &desc, runtime, &p) == DXGI_ERROR_UNSUPPORTED && !p);
   CHECK(!queues && !allocations);
   puts("Runtime primary: device ownership, allocation flags, S_OK/E_PENDING paging and failed waits, maps, mode status and lifetime cleanup pass (controlled callbacks)");
   return 0;
}
