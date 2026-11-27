/* SPDX-License-Identifier: MIT */
/* Real shared owner with controlled runtime allocations, not a GPU test. */
#include "RuntimeMemory.h"
#include "State.h"
#include "nouveau/vulkan/nvkmd/windows/allocation_abi.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "line %u: %s\n", __LINE__, #x); std::exit(1); } } while (0)
static unsigned refs, allocates, deallocates, queues, frees, waits, fail_queue, fail_resident;
static UINT64 fences[2];
static unsigned char metadata[72], bytes[8192];
static tg_gpu_linear_allocation_request allocation;
static UINT expected_format = D3DDDIFMT_A8R8G8B8;
static unsigned presents, resolved, finished;
static bool fail_finish;
static HRESULT present_status;
static HANDLE dev_handle(unsigned i) { return reinterpret_cast<HANDLE>(UINT_PTR(0x10 + i)); }
static HANDLE rt_handle(unsigned i) { return reinterpret_cast<HANDLE>(UINT_PTR(0x20 + i)); }
static unsigned owner(HANDLE h) { CHECK(h == dev_handle(0) || h == dev_handle(1)); return UINT_PTR(h) - 0x10; }
extern "C" void D3D10Trace(d3d10_umd_trace_event e, HRESULT, uint64_t, uint64_t)
{ CHECK(e == D3D10_TRACE_RUNTIME_MEMORY || e == D3D10_TRACE_RUNTIME_CONTEXT); }

static HRESULT APIENTRY present(HANDLE h, DXGIDDICB_PRESENT *p)
{
   CHECK(owner(h) == 0 && p->hSrcAllocation == 0x50 && !p->hDstAllocation);
   CHECK(p->hContext == reinterpret_cast<HANDLE>(UINT_PTR(0x90)));
   CHECK(p->pDXGIContext == reinterpret_cast<void *>(UINT_PTR(0x12345)));
   CHECK(!p->PrivateDriverDataSize && !p->pPrivateDriverData);
   CHECK(resolved && finished == resolved && !fail_finish);
   ++presents;
   return present_status;
}

static HRESULT APIENTRY create_queue(HANDLE h, D3DDDICB_CREATEPAGINGQUEUE *q)
{
   unsigned i = owner(h);
   if (fail_queue) return E_ACCESSDENIED;
   ++queues; q->hPagingQueue = 0x30 + i; q->hSyncObject = 0x40 + i;
   q->FenceValueCPUVirtualAddress = &fences[i]; return S_OK;
}
static HRESULT APIENTRY destroy_queue(HANDLE h, const D3DDDI_DESTROYPAGINGQUEUE *q)
{ CHECK(q->hPagingQueue == 0x30 + owner(h) && queues); --queues; return S_OK; }
static HRESULT APIENTRY allocate(HANDLE h, D3DDDICB_ALLOCATE *a)
{
   CHECK(owner(h) == 0 && a->hResource == rt_handle(0) && a->NumAllocations == 1);
   CHECK(a->PrivateDriverDataSize == sizeof(metadata) && a->pPrivateDriverData);
   auto info = a->pAllocationInfo2;
   CHECK(info && !info->Flags.Value && !info->pSystemMem);
   CHECK(info->PrivateDriverDataSize == sizeof(allocation));
   memcpy(&allocation, info->pPrivateDriverData, sizeof(allocation));
   CHECK(allocation.Buffer.version == 1 && allocation.Buffer.flags == 5);
   CHECK(allocation.Format == expected_format && allocation.Width == 65 && allocation.Height == 17);
   CHECK(allocation.Pitch == 384 && allocation.Buffer.bytes == sizeof(bytes));
   memcpy(metadata, a->pPrivateDriverData, sizeof(metadata));
   tg_gpu_resource_request header;
   memcpy(&header, metadata, sizeof(header));
   CHECK(header.version == 1 && header.header_bytes == 32 && header.allocation_count == 1);
   CHECK(header.driver_data_bytes == 40 && header.allocation_bytes == sizeof(bytes));
   CHECK(!header.reserved[0] && !header.reserved[1]);
   CHECK(!refs); refs = 1; ++allocates;
   info->hAllocation = 0x50; a->hKMResource = 0; return S_OK;
}
static HRESULT APIENTRY deallocate(HANDLE h, const D3DDDICB_DEALLOCATE *a)
{
   CHECK(a->hResource == rt_handle(owner(h)) && !a->NumAllocations && !a->HandleList && refs);
   --refs; ++deallocates; return S_OK;
}
static HRESULT APIENTRY resident(HANDLE h, D3DDDI_MAKERESIDENT *r)
{
   unsigned i = owner(h);
   CHECK(r->hPagingQueue == 0x30 + i && r->NumAllocations == 1 && r->AllocationList[0] == 0x50 + i);
   if (fail_resident) return D3DDDIERR_OUTOFVIDEOMEMORY;
   r->PagingFenceValue = 2; return E_PENDING;
}
static HRESULT APIENTRY wait_cpu(HANDLE h, const D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMCPU *w)
{
   unsigned i = owner(h); CHECK(w->ObjectCount == 1 && w->ObjectHandleArray[0] == 0x40 + i);
   fences[i] = w->FenceValueArray[0]; ++waits; return S_OK;
}
static HRESULT APIENTRY map_gpu(HANDLE h, D3DDDI_MAPGPUVIRTUALADDRESS *m)
{
   unsigned i = owner(h);
   CHECK(m->hPagingQueue == 0x30 + i && m->hAllocation == 0x50 + i);
   CHECK(m->BaseAddress == UINT64_C(0x40000) * (i + 1) && !m->OffsetInPages && m->SizeInPages == 2);
   m->VirtualAddress = m->BaseAddress; m->PagingFenceValue = 3; return E_PENDING;
}
static HRESULT APIENTRY free_gpu(HANDLE h, const D3DDDICB_FREEGPUVIRTUALADDRESS *f)
{
   unsigned i = owner(h); CHECK(f->BaseAddress == UINT64_C(0x40000) * (i + 1) && f->Size == 0x10000 && refs);
   ++frees; return S_OK;
}
static HRESULT APIENTRY lock_cpu(HANDLE h, D3DDDICB_LOCK2 *l)
{ CHECK(l->hAllocation == 0x50 + owner(h) && refs); l->pData = bytes; return S_OK; }
static HRESULT APIENTRY unlock_cpu(HANDLE h, const D3DDDICB_UNLOCK2 *l)
{ CHECK(l->hAllocation == 0x50 + owner(h) && refs); return S_OK; }

int main()
{
   pipe_screen screen = {};
   screen.get_device_luid = [](pipe_screen *, char *p) { uint64_t luid = 0x123456789; memcpy(p, &luid, sizeof(luid)); };
   pipe_context pipe = {}; pipe.screen = &screen;
   pipe.flush_resource = [](pipe_context *, pipe_resource *) { ++resolved; };
   pipe.flush = [](pipe_context *, pipe_fence_handle **fence, unsigned) {
      *fence = reinterpret_cast<pipe_fence_handle *>(UINT_PTR(0x88));
   };
   screen.fence_reference = [](pipe_screen *, pipe_fence_handle **out, pipe_fence_handle *in) { *out = in; };
   screen.fence_finish = [](pipe_screen *, pipe_context *, pipe_fence_handle *fence, uint64_t timeout) {
      CHECK(fence == reinterpret_cast<pipe_fence_handle *>(UINT_PTR(0x88)) && timeout == UINT64_MAX);
      ++finished; return !fail_finish;
   };
   DXGI_DDI_BASE_CALLBACKS dxgi = {}; dxgi.pfnPresentCb = present;
   Device devices[2] = {};
   for (unsigned i = 0; i < 2; ++i) {
      auto &d = devices[i]; d.hDevice = dev_handle(i); d.pipe = &pipe;
      d.hContext = reinterpret_cast<HANDLE>(UINT_PTR(0x90 + i)); d.pDXGIBaseCallbacks = &dxgi;
      auto &cb = d.KTCallbacks;
      cb.pfnAllocateCb = i == 0 ? allocate : NULL; /* Open must not allocate. */
      cb.pfnDeallocateCb = deallocate;
      cb.pfnCreatePagingQueueCb = create_queue; cb.pfnDestroyPagingQueueCb = destroy_queue;
      cb.pfnMakeResidentCb = resident; cb.pfnWaitForSynchronizationObjectFromCpuCb = wait_cpu;
      cb.pfnMapGpuVirtualAddressCb = map_gpu; cb.pfnFreeGpuVirtualAddressCb = free_gpu;
      cb.pfnLock2Cb = lock_cpu; cb.pfnUnlock2Cb = unlock_cpu;
   }
   D3D10DDI_MIPINFO mip = {}; mip.TexelWidth = 65; mip.TexelHeight = 17; mip.TexelDepth = 1;
   D3D10DDIARG_CREATERESOURCE create = {};
   create.ResourceDimension = D3D10DDIRESOURCE_TEXTURE2D;
   create.Format = DXGI_FORMAT_B8G8R8A8_UNORM; create.pMipInfoList = &mip;
   create.MipLevels = create.ArraySize = create.SampleDesc.Count = 1;
   create.MiscFlags = D3D10_DDI_RESOURCE_MISC_SHARED;
   create.BindFlags = D3D10_DDI_BIND_SHADER_RESOURCE | D3D10_DDI_BIND_RENDER_TARGET | D3D10_DDI_BIND_PRESENT;
   RuntimeResource *producer = NULL, *consumer = NULL;
   for (unsigned format = 0; format < 2; ++format) {
      create.Format = format ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
      expected_format = format ? D3DDDIFMT_A8B8G8R8 : D3DDDIFMT_A8R8G8B8;
      fences[0] = fences[1] = 0;
      const unsigned before_allocates = allocates, before_frees = frees;
      CHECK(RuntimeSharedCreate(&devices[0], &create, {rt_handle(0)}, &producer) == S_OK);
      CHECK(allocates == before_allocates + 1 && refs == 1 && queues == 1 && fences[0] == 2);
      /* The .151 shell path presents a non-primary shared allocation with
       * Blt. Preserve its runtime handle/token and return the callback status,
       * including positive statuses; never signal before GPU completion. */
      pipe_resource texture = {};
      Resource source = {}; source.device = &devices[0]; source.resource = &texture;
      source.runtime_memory = producer;
      DXGI_DDI_ARG_PRESENT args = {};
      args.hDevice = reinterpret_cast<DXGI_DDI_HDEVICE>(&devices[0]);
      args.hSurfaceToPresent = reinterpret_cast<DXGI_DDI_HRESOURCE>(&source);
      args.pDXGIContext = reinterpret_cast<void *>(UINT_PTR(0x12345)); args.Flags.Blt = 1;
      const unsigned before_presents = presents;
      present_status = S_OK;
      CHECK(RuntimePresent(&args) == S_OK && presents == before_presents + 1);
      present_status = E_ACCESSDENIED;
      CHECK(RuntimePresent(&args) == E_ACCESSDENIED && presents == before_presents + 2);
      present_status = DXGI_STATUS_OCCLUDED;
      CHECK(RuntimePresent(&args) == DXGI_STATUS_OCCLUDED && presents == before_presents + 3);
      fail_finish = true;
      CHECK(RuntimePresent(&args) == D3DDDIERR_DEVICEREMOVED && presents == before_presents + 3);
      fail_finish = false;
      const unsigned before_resolved = resolved;
      args.Flags.Value = 2;
      CHECK(RuntimePresent(&args) == DXGI_ERROR_UNSUPPORTED);
      args.Flags.Value = 3;
      CHECK(RuntimePresent(&args) == DXGI_ERROR_UNSUPPORTED);
      CHECK(resolved == before_resolved && presents == before_presents + 3);
      auto pm = *RuntimeResourceMemory(producer);
      CHECK(pm.size == sizeof(bytes) && RuntimeResourcePitch(producer) == 384);
      CHECK(pm.map_gpu(pm.context, 0x40000, 0, sizeof(bytes), 0, false, 0x40000, 0x10000) == VK_SUCCESS);
      void *data = NULL; CHECK(pm.map_cpu(pm.context, &data) == VK_SUCCESS);
      memset(data, 0x57 + format, sizeof(bytes)); pm.unmap_cpu(pm.context);

      D3DDDI_OPENALLOCATIONINFO info = {};
      info.hAllocation = 0x51; info.pPrivateDriverData = &allocation; info.PrivateDriverDataSize = sizeof(allocation);
      D3D10DDIARG_OPENRESOURCE open = {};
      open.NumAllocations = 1; open.pOpenAllocationInfo = &info;
      open.hKMResource.handle = 0x900; /* Not an allocation/device/NT handle. */
      open.pPrivateDriverData = metadata; open.PrivateDriverDataSize = sizeof(metadata);
      RuntimeSharedDescription description = {};
      /* Invalid metadata must fail before the driver takes callback ownership. */
      for (unsigned offset : {0u, 8u, 16u, 32u, 36u, 48u, 68u}) {
         metadata[offset] ^= 1;
         CHECK(FAILED(RuntimeSharedOpen(&devices[1], &open, {rt_handle(1)}, &consumer, &description)));
         CHECK(!consumer && refs == 1 && queues == 1 && allocates == before_allocates + 1);
         metadata[offset] ^= 1;
      }
      allocation.Pitch += 128;
      CHECK(FAILED(RuntimeSharedOpen(&devices[1], &open, {rt_handle(1)}, &consumer, &description)));
      allocation.Pitch -= 128;
      /* Model the runtime's existing opened reference, not a second AllocateCb. */
      ++refs;
      CHECK(RuntimeSharedOpen(&devices[1], &open, {rt_handle(1)}, &consumer, &description) == S_OK);
      CHECK(refs == 2 && queues == 2 && allocates == before_allocates + 1 && fences[1] == 2);
      CHECK(description.width == 65 && description.height == 17 && description.pitch == 384 && description.format == create.Format);
      CHECK(RuntimePrimarySetMode(consumer, dev_handle(1), 0) == DXGI_ERROR_UNSUPPORTED);
      auto cm = *RuntimeResourceMemory(consumer);
      CHECK(cm.map_gpu(cm.context, 0x80000, 0, sizeof(bytes), 0, false, 0x80000, 0x10000) == VK_SUCCESS);
      pm.retain(pm.context);
      CHECK(RuntimeResourceDestroy(producer) == S_OK && refs == 1 && queues == 1 && frees == before_frees + 1);
      CHECK(pm.map_cpu(pm.context, &data) == VK_ERROR_DEVICE_LOST);
      CHECK(pm.free_gpu(pm.context, 0x40000, 0x10000) == VK_SUCCESS && frees == before_frees + 1);
      pm.release(pm.context);
      CHECK(cm.map_cpu(cm.context, &data) == VK_SUCCESS);
      for (unsigned j = 0; j < sizeof(bytes); ++j) CHECK(static_cast<unsigned char *>(data)[j] == 0x57 + format);
      cm.unmap_cpu(cm.context);
      CHECK(RuntimeResourceDestroy(consumer) == S_OK && !refs && !queues && frees == before_frees + 2);

      /* Once a valid open is accepted, queue/residency failure must release
       * its runtime resource, because no DestroyResource DDI will follow. */
      for (unsigned step = 0; step < 2; ++step) {
         refs = 1; fail_queue = step == 0; fail_resident = step == 1;
         CHECK(FAILED(RuntimeSharedOpen(&devices[1], &open, {rt_handle(1)}, &consumer, &description)));
         CHECK(!consumer && !refs && !queues && allocates == before_allocates + 1);
      }
      fail_queue = fail_resident = 0;
   }
   const unsigned before_allocates = allocates;
   mip.TexelWidth = UINT32_MAX;
   CHECK(RuntimeSharedCreate(&devices[0], &create, {rt_handle(0)}, &producer) == D3DDDIERR_OUTOFVIDEOMEMORY);
   CHECK(!producer && !refs && !queues && allocates == before_allocates);
   std::puts("Shared runtime ownership: aligned BGRA/RGBA metadata, consumer handles, paging, producer release and failure cleanup pass (controlled callbacks; no GPU rendering)");
   return 0;
}
