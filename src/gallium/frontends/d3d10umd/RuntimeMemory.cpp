/* SPDX-License-Identifier: MIT
 * Runtime ownership follows the Microsoft WDDM callback contracts; see
 * tegra-x1-wddm/docs/D3D_RUNTIME_SHARING.md. No runtime handle is cast to a
 * KMT device or NT shared-resource handle.
 */
#include "RuntimeMemory.h"
#include "State.h"
#include "nouveau/vulkan/nvkmd/windows/allocation_abi.h"
#include <stdlib.h>

struct PrimaryMapping {
   uint64_t address, size;
   bool freed;
   PrimaryMapping *next;
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
   UINT pitch;
   bool cpu_locked, dead, associated, primary;
   PrimaryMapping *mappings;
};

/* arg0: stage in high 32 bits, allocation handle in low 32 bits.
 * arg1: byte count, paging-fence value, or paging-queue handle. */
static HRESULT trace(RuntimeResource *p, unsigned stage, HRESULT hr, uint64_t value = 0)
{
   D3D10Trace(D3D10_TRACE_RUNTIME_MEMORY, hr,
              (uint64_t)stage << 32 | p->memory.allocation, value);
   return hr;
}

static VkResult result(RuntimeResource *p, HRESULT hr)
{
   if (SUCCEEDED(hr)) return VK_SUCCESS;
   p->error = hr;
   if (hr == E_OUTOFMEMORY) return VK_ERROR_OUT_OF_HOST_MEMORY;
   if (hr == D3DDDIERR_OUTOFVIDEOMEMORY) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   if (hr == D3DDDIERR_DEVICEREMOVED || hr == DXGI_ERROR_DEVICE_REMOVED ||
       hr == DXGI_ERROR_DEVICE_RESET) return VK_ERROR_DEVICE_LOST;
   return VK_ERROR_UNKNOWN;
}

static HRESULT wait_paging(RuntimeResource *p, UINT64 value)
{
   if (!value || (p->fence_value && *p->fence_value >= value)) return S_OK;
   D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMCPU wait = {};
   wait.ObjectCount = 1;
   wait.ObjectHandleArray = &p->fence;
   wait.FenceValueArray = &value;
   return trace(p, 3, p->callbacks.pfnWaitForSynchronizationObjectFromCpuCb(p->device, &wait), value);
}

static void retain(void *context)
{
   InterlockedIncrement(&static_cast<RuntimeResource *>(context)->refs);
}

static void release(void *context)
{
   auto p = static_cast<RuntimeResource *>(context);
   if (!InterlockedDecrement(&p->refs)) {
      while (p->mappings) {
         auto next = p->mappings->next;
         free(p->mappings); p->mappings = next;
      }
      free(p);
   }
}

static HRESULT free_address(RuntimeResource *p, uint64_t address, uint64_t bytes)
{
   D3DDDICB_FREEGPUVIRTUALADDRESS args = {};
   args.BaseAddress = address;
   args.Size = bytes;
   return trace(p, 5, p->callbacks.pfnFreeGpuVirtualAddressCb(p->device, &args), bytes);
}

static VkResult free_gpu(void *context, uint64_t address, uint64_t bytes)
{
   auto p = static_cast<RuntimeResource *>(context);
   AcquireSRWLockExclusive(&p->lock);
   PrimaryMapping **link = &p->mappings;
   while (*link && ((*link)->address != address || (*link)->size != bytes)) link = &(*link)->next;
   auto mapping = *link;
   HRESULT hr = S_OK;
   if ((!mapping || !mapping->freed) && !p->dead) hr = free_address(p, address, bytes);
   if (FAILED(hr)) p->cleanup_error = hr;
   if (mapping) { *link = mapping->next; free(mapping); }
   VkResult status = result(p, hr);
   ReleaseSRWLockExclusive(&p->lock);
   return status;
}

static VkResult map_gpu(void *context, uint64_t address, uint64_t offset,
                        uint64_t bytes, uint8_t kind, bool read_only,
                        uint64_t reserved, uint64_t reserved_size)
{
   auto p = static_cast<RuntimeResource *>(context);
   AcquireSRWLockExclusive(&p->lock);
   HRESULT hr = E_INVALIDARG;
   if (p->dead) {
      hr = D3DDDIERR_DEVICEREMOVED;
   } else if (bytes && !((address | offset | bytes | reserved | reserved_size) & 4095) && !kind &&
              offset <= p->memory.size && bytes <= p->memory.size - offset &&
              address >= reserved && address - reserved <= reserved_size &&
              bytes <= reserved_size - (address - reserved)) {
      PrimaryMapping *mapping = p->mappings;
      while (mapping && (mapping->address != reserved || mapping->size != reserved_size)) mapping = mapping->next;
      if (!mapping) {
         mapping = static_cast<PrimaryMapping *>(calloc(1, sizeof(*mapping)));
         if (!mapping) {
            result(p, E_OUTOFMEMORY);
            ReleaseSRWLockExclusive(&p->lock);
            return VK_ERROR_OUT_OF_HOST_MEMORY;
         }
         mapping->address = reserved; mapping->size = reserved_size;
         mapping->next = p->mappings; p->mappings = mapping;
      }
      D3DDDI_MAPGPUVIRTUALADDRESS map = {};
      map.hPagingQueue = p->queue;
      map.BaseAddress = address;
      map.hAllocation = p->memory.allocation;
      map.OffsetInPages = offset / 4096;
      map.SizeInPages = bytes / 4096;
      map.Protection.Write = !read_only;
      hr = trace(p, 4, p->callbacks.pfnMapGpuVirtualAddressCb(p->device, &map), bytes);
      /* WDDM queues page-table updates with E_PENDING, despite its HRESULT
       * failure bit. The allocation is usable only after the paging fence. */
      if (SUCCEEDED(hr) || hr == E_PENDING) {
         if (map.VirtualAddress != address) hr = E_FAIL;
         else hr = wait_paging(p, map.PagingFenceValue);
      }
   }
   VkResult status = result(p, hr);
   ReleaseSRWLockExclusive(&p->lock);
   return status;
}

static VkResult map_cpu(void *context, void **address)
{
   auto p = static_cast<RuntimeResource *>(context);
   AcquireSRWLockExclusive(&p->lock);
   D3DDDICB_LOCK2 lock = {};
   lock.hAllocation = p->memory.allocation;
   HRESULT hr = p->dead ? D3DDDIERR_DEVICEREMOVED :
      p->cpu_locked ? E_UNEXPECTED : trace(p, 7, p->callbacks.pfnLock2Cb(p->device, &lock));
   if (SUCCEEDED(hr)) {
      if (!lock.pData) hr = E_FAIL;
      else {
         *address = lock.pData;
         p->cpu_locked = true;
      }
   }
   VkResult status = result(p, hr);
   ReleaseSRWLockExclusive(&p->lock);
   return status;
}

static void unmap_cpu(void *context)
{
   auto p = static_cast<RuntimeResource *>(context);
   AcquireSRWLockExclusive(&p->lock);
   if (p->cpu_locked) {
      D3DDDICB_UNLOCK2 unlock = {};
      unlock.hAllocation = p->memory.allocation;
      result(p, trace(p, 8, p->callbacks.pfnUnlock2Cb(p->device, &unlock)));
      p->cpu_locked = false;
   }
   ReleaseSRWLockExclusive(&p->lock);
}

HRESULT RuntimeResourceDestroy(RuntimeResource *p)
{
   if (!p) return S_OK;
   AcquireSRWLockExclusive(&p->lock);
   HRESULT hr = p->cleanup_error;
   p->dead = true;
   for (auto mapping = p->mappings; mapping; mapping = mapping->next) {
      if (!mapping->freed) {
         HRESULT next = free_address(p, mapping->address, mapping->size);
         if (SUCCEEDED(hr)) hr = next;
         mapping->freed = true;
      }
   }
   if (p->cpu_locked) {
      D3DDDICB_UNLOCK2 unlock = {};
      unlock.hAllocation = p->memory.allocation;
      HRESULT next = trace(p, 8, p->callbacks.pfnUnlock2Cb(p->device, &unlock));
      if (SUCCEEDED(hr)) hr = next;
      p->cpu_locked = false;
   }
   if (p->associated) {
      D3DDDICB_DEALLOCATE deallocate = {};
      deallocate.hResource = p->runtime_resource;
      HRESULT next = trace(p, 6, p->callbacks.pfnDeallocateCb(p->device, &deallocate));
      if (SUCCEEDED(hr)) hr = next;
      p->memory.allocation = 0;
      p->associated = false;
   }
   if (p->queue) {
      D3DDDI_DESTROYPAGINGQUEUE queue = {};
      queue.hPagingQueue = p->queue;
      HRESULT next = trace(p, 10, p->callbacks.pfnDestroyPagingQueueCb(p->device, &queue), p->queue);
      if (SUCCEEDED(hr)) hr = next;
      p->queue = 0;
   }
   ReleaseSRWLockExclusive(&p->lock);
   release(p);
   return hr;
}

static HRESULT create_owner(Device *device, D3D10DDI_HRTRESOURCE runtime,
                            UINT pitch, RuntimeResource **out)
{
   const auto &cb = device->KTCallbacks;
   if (!runtime.handle || !cb.pfnDeallocateCb ||
       !cb.pfnCreatePagingQueueCb || !cb.pfnDestroyPagingQueueCb ||
       !cb.pfnMakeResidentCb || !cb.pfnMapGpuVirtualAddressCb || !cb.pfnFreeGpuVirtualAddressCb ||
       !cb.pfnWaitForSynchronizationObjectFromCpuCb || !cb.pfnLock2Cb || !cb.pfnUnlock2Cb ||
       !device->pipe->screen->get_device_luid)
      return DXGI_ERROR_UNSUPPORTED;
   auto p = static_cast<RuntimeResource *>(calloc(1, sizeof(RuntimeResource)));
   if (!p) return E_OUTOFMEMORY;
   p->refs = 1;
   p->pitch = pitch;
   p->device = device->hDevice;
   p->runtime_resource = runtime.handle;
   p->callbacks = cb;
   InitializeSRWLock(&p->lock);
   device->pipe->screen->get_device_luid(device->pipe->screen,
                                        reinterpret_cast<char *>(&p->memory.adapter_luid));
   if (!p->memory.adapter_luid) {
      release(p);
      return DXGI_ERROR_UNSUPPORTED;
   }
   *out = p;
   return S_OK;
}

static HRESULT create_queue(RuntimeResource *p)
{
   D3DDDICB_CREATEPAGINGQUEUE queue = {};
   queue.Priority = D3DDDI_PAGINGQUEUE_PRIORITY_NORMAL;
   HRESULT hr = p->callbacks.pfnCreatePagingQueueCb(p->device, &queue);
   trace(p, 9, hr, queue.hPagingQueue);
   if (FAILED(hr)) return hr;
   p->queue = queue.hPagingQueue;
   p->fence = queue.hSyncObject;
   p->fence_value = static_cast<const volatile UINT64 *>(queue.FenceValueCPUVirtualAddress);
   return p->queue && p->fence ? S_OK : E_FAIL;
}

static HRESULT make_ready(RuntimeResource *p, uint64_t bytes)
{
   if (!p->memory.allocation) return E_FAIL;
   D3DDDI_MAKERESIDENT resident = {};
   resident.hPagingQueue = p->queue;
   resident.NumAllocations = 1;
   resident.AllocationList = &p->memory.allocation;
   HRESULT hr = p->callbacks.pfnMakeResidentCb(p->device, &resident);
   trace(p, 2, hr, resident.PagingFenceValue);
   if (SUCCEEDED(hr) || hr == E_PENDING) hr = wait_paging(p, resident.PagingFenceValue);
   if (FAILED(hr)) return hr;
   p->memory.version = WSI_WIN32_MEMORY_VERSION;
   p->memory.size = bytes;
   p->memory.context = p;
   p->memory.retain = retain;
   p->memory.release = release;
   p->memory.map_gpu = map_gpu;
   p->memory.free_gpu = free_gpu;
   p->memory.map_cpu = map_cpu;
   p->memory.unmap_cpu = unmap_cpu;
   return S_OK;
}

/* The KMD owns the storage envelope; this pointer-free payload belongs to
 * the D3D UMD. OpenResource receives the original bytes from the runtime. */
struct SharedMetadata {
   tg_gpu_resource_request storage;
   uint32_t magic, version;
   RuntimeSharedDescription description;
   uint32_t reserved[2];
};
static_assert(sizeof(SharedMetadata) == 72, "shared image metadata ABI");
static constexpr uint32_t shared_magic = 0x52534754; /* TGSR */

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
   const uint64_t pitch = ((uint64_t)d.width * 4 + 127) & ~UINT64_C(127);
   const uint64_t rows = ((uint64_t)d.height + 3) & ~UINT64_C(3);
   const uint64_t budget = UINT64_C(512) * 1024 * 1024;
   if (pitch > budget || rows > budget / pitch)
      return D3DDDIERR_OUTOFVIDEOMEMORY;
   *allocation = {
      {TG_GPU_ALLOCATION_ABI, TG_GPU_ALLOCATION_CPU_VISIBLE | TG_GPU_ALLOCATION_LINEAR_SURFACE,
       (pitch * rows + 4095) & ~UINT64_C(4095)},
      format, d.width, d.height, (UINT)pitch};
   return S_OK;
}

HRESULT RuntimeSharedCreate(Device *device, const D3D10DDIARG_CREATERESOURCE *desc,
                            D3D10DDI_HRTRESOURCE runtime, RuntimeResource **out)
{
   *out = NULL;
   if (!device->KTCallbacks.pfnAllocateCb || !desc->pMipInfoList || desc->pPrimaryDesc ||
       desc->ResourceDimension != D3D10DDIRESOURCE_TEXTURE2D ||
       desc->MiscFlags != D3D10_DDI_RESOURCE_MISC_SHARED || desc->MapFlags ||
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
   if (FAILED(hr)) return hr;
   d.pitch = request.Pitch;
   metadata.magic = shared_magic;
   metadata.version = 1;
   metadata.storage = {TG_GPU_RESOURCE_ABI, sizeof(metadata.storage), 1,
      sizeof(metadata) - sizeof(metadata.storage), request.Buffer.bytes, {0, 0}};
   RuntimeResource *p = NULL;
   hr = create_owner(device, runtime, d.pitch, &p);
   if (FAILED(hr)) return hr;
   hr = create_queue(p);
   if (FAILED(hr)) { RuntimeResourceDestroy(p); return hr; }
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
   trace(p, 1, hr, request.Buffer.bytes);
   if (FAILED(hr)) { RuntimeResourceDestroy(p); return hr; }
   p->associated = true;
   p->memory.allocation = info.hAllocation;
   hr = make_ready(p, request.Buffer.bytes);
   if (FAILED(hr)) { RuntimeResourceDestroy(p); return hr; }
   *out = p;
   return S_OK;
}

HRESULT RuntimeSharedOpen(Device *device, const D3D10DDIARG_OPENRESOURCE *args,
                          D3D10DDI_HRTRESOURCE runtime, RuntimeResource **out,
                          RuntimeSharedDescription *description)
{
   *out = NULL;
   *description = {};
   if (args->NumAllocations != 1 || !args->pOpenAllocationInfo ||
       !args->hKMResource.handle || !args->pPrivateDriverData ||
       args->PrivateDriverDataSize != sizeof(SharedMetadata))
      return DXGI_ERROR_UNSUPPORTED;
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
   if (FAILED(hr)) return hr;
   if (s.allocation_bytes != expected.Buffer.bytes || metadata.description.pitch != expected.Pitch)
      return DXGI_ERROR_UNSUPPORTED;
   /* For one allocation, only the documented INFO/INFO2 common prefix is
    * needed. Do not interpret a reserved/unnegotiated GPU-VA suffix. */
   const auto &info = args->pOpenAllocationInfo[0];
   if (!info.hAllocation || !info.pPrivateDriverData ||
       info.PrivateDriverDataSize != sizeof(expected))
      return DXGI_ERROR_UNSUPPORTED;
   tg_gpu_linear_allocation_request actual;
   memcpy(&actual, info.pPrivateDriverData, sizeof(actual));
   if (memcmp(&actual, &expected, sizeof(actual))) return DXGI_ERROR_UNSUPPORTED;
   RuntimeResource *p = NULL;
   hr = create_owner(device, runtime, expected.Pitch, &p);
   if (FAILED(hr)) return hr;
   /* The runtime has opened these handles for the consumer device already.
    * Keep its hRTResource, not the producer's handles or hKMResource, for
    * callbacks and teardown. No second allocation or KMT open is performed. */
   p->associated = true;
   p->memory.allocation = info.hAllocation;
   hr = create_queue(p);
   if (SUCCEEDED(hr)) hr = make_ready(p, expected.Buffer.bytes);
   if (FAILED(hr)) { RuntimeResourceDestroy(p); return hr; }
   *description = metadata.description;
   *out = p;
   return S_OK;
}

HRESULT RuntimePrimaryCreate(Device *device, const D3D10DDIARG_CREATERESOURCE *desc,
                              D3D10DDI_HRTRESOURCE runtime, RuntimeResource **out)
{
   *out = NULL;
   if (!desc->pPrimaryDesc || !desc->pMipInfoList) return DXGI_ERROR_UNSUPPORTED;
   const auto &primary = *desc->pPrimaryDesc;
   const UINT mode_rotation = primary.ModeDesc.Rotation == DXGI_DDI_MODE_ROTATION_UNSPECIFIED ?
      DXGI_DDI_MODE_ROTATION_IDENTITY : primary.ModeDesc.Rotation;
   /* NONPREROTATED explicitly leaves output orientation to the application.
    * Preserve that identity storage rather than applying the mode twice. */
   const UINT rotation = primary.Flags & DXGI_DDI_PRIMARY_NONPREROTATED ?
      DXGI_DDI_MODE_ROTATION_IDENTITY : mode_rotation;
   const bool quarter = rotation == DXGI_DDI_MODE_ROTATION_ROTATE90 ||
                        rotation == DXGI_DDI_MODE_ROTATION_ROTATE270;
   const UINT width = quarter ? 1280 : 720, height = quarter ? 720 : 1280;
   D3D10Trace(D3D10_TRACE_PRIMARY_LAYOUT, S_OK,
      (uint64_t)primary.ModeDesc.Width << 32 | primary.ModeDesc.Height,
      (uint64_t)primary.Flags << 32 | primary.ModeDesc.Rotation);
   if (!desc->pPrimaryDesc || !runtime.handle || !desc->pMipInfoList ||
       desc->ResourceDimension != D3D10DDIRESOURCE_TEXTURE2D ||
       desc->Format != DXGI_FORMAT_B8G8R8A8_UNORM || desc->MipLevels != 1 ||
       desc->ArraySize != 1 || desc->SampleDesc.Count != 1 || desc->SampleDesc.Quality ||
       primary.VidPnSourceId != 0 || mode_rotation > DXGI_DDI_MODE_ROTATION_ROTATE270 ||
       (primary.Flags & ~(DXGI_DDI_PRIMARY_OPTIONAL | DXGI_DDI_PRIMARY_NONPREROTATED)) ||
       desc->pMipInfoList[0].TexelWidth != width || desc->pMipInfoList[0].TexelHeight != height ||
       !((primary.ModeDesc.Width == width && primary.ModeDesc.Height == height) ||
         ((mode_rotation == DXGI_DDI_MODE_ROTATION_ROTATE90 ||
           mode_rotation == DXGI_DDI_MODE_ROTATION_ROTATE270) &&
          primary.ModeDesc.Width == height && primary.ModeDesc.Height == width)))
      return DXGI_ERROR_UNSUPPORTED;

   if (!device->KTCallbacks.pfnAllocateCb || !device->KTCallbacks.pfnSetDisplayModeCb)
      return DXGI_ERROR_UNSUPPORTED;
   RuntimeResource *p = NULL;
   HRESULT hr = create_owner(device, runtime, width * 4, &p);
   if (FAILED(hr)) return hr;
   p->primary = true;
   hr = create_queue(p);
   if (FAILED(hr)) { RuntimeResourceDestroy(p); return hr; }

   tg_gpu_primary_allocation_request request = {
      {TG_GPU_ALLOCATION_ABI, TG_GPU_ALLOCATION_CPU_VISIBLE | TG_GPU_ALLOCATION_DISPLAY_PRIMARY,
       (uint64_t)p->pitch * height}, D3DDDIFMT_A8R8G8B8,
      {primary.ModeDesc.RefreshRate.Numerator, primary.ModeDesc.RefreshRate.Denominator},
      rotation, width, height, p->pitch, 0};
   D3DDDI_ALLOCATIONINFO2 info = {};
   info.pPrivateDriverData = &request;
   info.PrivateDriverDataSize = sizeof(request);
   info.VidPnSourceId = desc->pPrimaryDesc->VidPnSourceId;
   info.Flags.Primary = true;
   D3DDDICB_ALLOCATE allocate = {};
   allocate.hResource = runtime.handle;
   allocate.NumAllocations = 1;
   allocate.pAllocationInfo2 = &info;
   hr = p->callbacks.pfnAllocateCb(p->device, &allocate);
   trace(p, 1, hr, request.Buffer.bytes);
   if (FAILED(hr)) { RuntimeResourceDestroy(p); return hr; }
   p->associated = true;
   p->memory.allocation = info.hAllocation;
   hr = make_ready(p, request.Buffer.bytes);
   if (FAILED(hr)) { RuntimeResourceDestroy(p); return hr; }
   *out = p;
   return S_OK;
}

HRESULT RuntimePrimarySetMode(RuntimeResource *p, HANDLE device, UINT subresource)
{
   if (!p || !p->primary) return DXGI_ERROR_UNSUPPORTED;
   if (p->device != device || subresource) return E_INVALIDARG;
   AcquireSRWLockExclusive(&p->lock);
   D3DDDICB_SETDISPLAYMODE mode = {};
   mode.hPrimaryAllocation = p->memory.allocation;
   HRESULT hr = p->dead ? D3DDDIERR_DEVICEREMOVED :
      p->callbacks.pfnSetDisplayModeCb(p->device, &mode);
   result(p, hr);
   ReleaseSRWLockExclusive(&p->lock);
   return hr;
}

HRESULT RuntimeResourceError(RuntimeResource *p)
{
   return FAILED(p->error) ? p->error : DXGI_ERROR_UNSUPPORTED;
}

const wsi_win32_memory *RuntimeResourceMemory(RuntimeResource *p) { return &p->memory; }
UINT RuntimeResourcePitch(RuntimeResource *p) { return p->pitch; }

HRESULT RuntimePresent(const DXGI_DDI_ARG_PRESENT *args)
{
   if (!args || !args->hDevice || !args->hSurfaceToPresent)
      return E_INVALIDARG;
   Device *device = CastDevice(args->hDevice);
   Resource *source = CastResource(args->hSurfaceToPresent);
   Resource *destination = args->hDstResource ? CastResource(args->hDstResource) : NULL;
   if (source->device != device || (destination && destination->device != device))
      return E_INVALIDARG;
   /* DXGI selects the presentation mode behind pDXGIContext. Windowed shared
    * surfaces arrive with Blt, even though they are not scanout primaries.
    * Forward their real allocation after GPU completion; the runtime/KMD
    * remains responsible for destination selection and the actual operation. */
   const bool flip = args->Flags.Value == 2;
   if ((!flip && args->Flags.Value != 1) || args->SrcSubResourceIndex ||
       (destination && args->DstSubResourceIndex) ||
       (unsigned)args->FlipInterval > DXGI_DDI_FLIP_INTERVAL_FOUR ||
       !device->hContext || !device->pDXGIBaseCallbacks ||
       !device->pDXGIBaseCallbacks->pfnPresentCb || !source->runtime_memory ||
       !source->resource || (destination && (!destination->runtime_memory || !destination->resource)))
      return DXGI_ERROR_UNSUPPORTED;
   RuntimeResource *src = source->runtime_memory;
   RuntimeResource *dst = destination ? destination->runtime_memory : NULL;
   if (flip && (!src->primary || (dst && !dst->primary))) return DXGI_ERROR_UNSUPPORTED;
   if (src->device != device->hDevice || (dst && dst->device != device->hDevice))
      return E_INVALIDARG;
   if (src->dead || !src->associated || (dst && (dst->dead || !dst->associated)))
      return D3DDDIERR_DEVICEREMOVED;
   HRESULT hr = RuntimeResourceFinish(device, source);
   if (FAILED(hr)) return hr;
   DXGIDDICB_PRESENT present = {};
   present.hSrcAllocation = src->memory.allocation;
   present.hDstAllocation = dst ? dst->memory.allocation : 0;
   present.hContext = device->hContext;
   /* DXGI owns the flags/interval protocol behind this opaque value. */
   present.pDXGIContext = args->pDXGIContext;
   return device->pDXGIBaseCallbacks->pfnPresentCb(device->hDevice, &present);
}

HRESULT RuntimeRotateOwnership(Device *device, Resource *const *resources, UINT count)
{
   if (!count || !resources[0]->runtime_memory) return S_OK;
   for (UINT i = 0; i < count; ++i) {
      const RuntimeResource *p = resources[i]->runtime_memory;
      if (!p || p->device != device->hDevice || !resources[i]->runtime_resource)
         return E_INVALIDARG;
      if (p->dead || !p->associated) return D3DDDIERR_DEVICEREMOVED;
   }
   RuntimeResource *first = resources[0]->runtime_memory;
   for (UINT i = 0; i + 1 < count; ++i)
      resources[i]->runtime_memory = resources[i + 1]->runtime_memory;
   resources[count - 1]->runtime_memory = first;
   /* Allocation ownership moves with the Gallium storage. The runtime's
    * resource handles stay in their original logical resource slots. */
   for (UINT i = 0; i < count; ++i)
      resources[i]->runtime_memory->runtime_resource = resources[i]->runtime_resource;
   return S_OK;
}

HRESULT RuntimeDeviceCreateContext(Device *device)
{
   const auto &cb = device->KTCallbacks;
   auto screen = device->pipe->screen;
   if (!cb.pfnCreateContextVirtualCb || !cb.pfnDestroyContextCb ||
       !screen->fence_finish || !screen->fence_reference)
      return DXGI_ERROR_UNSUPPORTED;
   /* NVK's KMT context is not registered with the D3D runtime. The runtime
    * needs its own context for implicit synchronization and DXGI Present. */
   D3DDDICB_CREATECONTEXTVIRTUAL args = {};
   args.EngineAffinity = 1;
   HRESULT hr = cb.pfnCreateContextVirtualCb(device->hDevice, &args);
   if (SUCCEEDED(hr)) {
      if (!args.hContext) hr = E_FAIL;
      else device->hContext = args.hContext;
   }
   D3D10Trace(D3D10_TRACE_RUNTIME_CONTEXT, hr, 1, device->hContext != NULL);
   return hr;
}

HRESULT RuntimeDeviceDestroyContext(Device *device)
{
   if (!device->hContext) return S_OK;
   D3DDDICB_DESTROYCONTEXT args = {};
   args.hContext = device->hContext;
   HRESULT hr = device->KTCallbacks.pfnDestroyContextCb(device->hDevice, &args);
   D3D10Trace(D3D10_TRACE_RUNTIME_CONTEXT, hr, 2, 1);
   device->hContext = NULL;
   return hr;
}

HRESULT RuntimeDeviceFinish(Device *device)
{
   auto pipe = device->pipe;
   auto screen = pipe->screen;
   if (!screen->fence_finish || !screen->fence_reference) return E_FAIL;
   struct pipe_fence_handle *fence = NULL;
   /* NVK submits on a separate KMT context. Complete its work before the
    * runtime signals the context above, so an implicit signal cannot pass
    * pending rendering. A shared GPU fence can replace this synchronous
    * bridge when the two context owners expose a common timeline. */
   pipe->flush(pipe, &fence, PIPE_FLUSH_HINT_FINISH);
   bool finished = fence && screen->fence_finish(screen, pipe, fence, UINT64_MAX);
   screen->fence_reference(screen, &fence, NULL);
   HRESULT hr = finished ? S_OK : D3DDDIERR_DEVICEREMOVED;
   if (pipe->get_device_reset_status && pipe->get_device_reset_status(pipe) != PIPE_NO_RESET)
      hr = D3DDDIERR_DEVICEREMOVED;
   D3D10Trace(D3D10_TRACE_RUNTIME_CONTEXT, hr, 3, finished);
   return hr;
}

HRESULT RuntimeResourceFinish(Device *device, Resource *resource)
{
   if (!resource->runtime_memory || !resource->resource) return S_OK;
   if (device->pipe->flush_resource)
      device->pipe->flush_resource(device->pipe, resource->resource);
   return RuntimeDeviceFinish(device);
}
