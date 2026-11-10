/* SPDX-License-Identifier: MIT */
#include "kmt_native.h"
#include <d3dkmthk.h>
#include <stdlib.h>
#include <string.h>
#include "allocation_abi.h"
#include "submit_abi.h"

LONG
tg_kmt_open_device(struct tg_kmt_device *dev,
                   const struct tg_gpu_adapter_info *info)
{
   *dev = (struct tg_kmt_device){0};
   InitializeSRWLock(&dev->memory_lock);
   dev->budget = info->memory_budget_bytes;
   D3DKMT_OPENADAPTERFROMLUID open = {
      .AdapterLuid = { .LowPart = info->luid_low, .HighPart = info->luid_high },
   };
   LONG status = D3DKMTOpenAdapterFromLuid(&open);
   if (status < 0)
      return status;
   dev->adapter = open.hAdapter;

   D3DKMT_CREATEDEVICE device = { .hAdapter = dev->adapter };
   status = D3DKMTCreateDevice(&device);
   if (status < 0)
      goto fail;
   dev->handle = device.hDevice;

   D3DKMT_CREATEPAGINGQUEUE queue = {
      .hDevice = dev->handle, .Priority = D3DDDI_PAGINGQUEUE_PRIORITY_NORMAL,
   };
   status = D3DKMTCreatePagingQueue(&queue);
   if (status < 0)
      goto fail;
   dev->paging_queue = queue.hPagingQueue;
   dev->paging = (struct tg_kmt_fence) {
      .handle = queue.hSyncObject, .value = queue.FenceValueCPUVirtualAddress,
   };
   return 0;

fail:
   tg_kmt_close_device(dev);
   return status;
}

LONG
tg_kmt_close_device(struct tg_kmt_device *dev)
{
   LONG status = 0;
   if (dev->paging_queue) {
      D3DDDI_DESTROYPAGINGQUEUE queue = { .hPagingQueue = dev->paging_queue };
      status = D3DKMTDestroyPagingQueue(&queue);
   }
   if (dev->handle) {
      const D3DKMT_DESTROYDEVICE device = { .hDevice = dev->handle };
      LONG next = D3DKMTDestroyDevice(&device);
      if (status >= 0)
         status = next;
   }
   if (dev->adapter) {
      const D3DKMT_CLOSEADAPTER adapter = { .hAdapter = dev->adapter };
      LONG next = D3DKMTCloseAdapter(&adapter);
      if (status >= 0)
         status = next;
   }
   *dev = (struct tg_kmt_device){0};
   return status;
}

LONG
tg_kmt_create_context(const struct tg_kmt_device *dev, uint32_t *context)
{
   D3DKMT_CREATECONTEXTVIRTUAL args = {
      .hDevice = dev->handle, .EngineAffinity = 1,
      .ClientHint = D3DKMT_CLIENTHINT_VULKAN,
   };
   LONG status = D3DKMTCreateContextVirtual(&args);
   if (status >= 0)
      *context = args.hContext;
   return status;
}

LONG
tg_kmt_destroy_context(uint32_t context)
{
   const D3DKMT_DESTROYCONTEXT args = { .hContext = context };
   return D3DKMTDestroyContext(&args);
}

LONG
tg_kmt_wait_fences(const struct tg_kmt_device *dev, uint32_t count,
                   const uint32_t *handles, const uint64_t *values,
                   bool any, DWORD timeout_ms)
{
   const HANDLE event = timeout_ms == INFINITE ?
                        NULL : CreateEventW(NULL, FALSE, FALSE, NULL);
   if (timeout_ms != INFINITE && !event)
      return TG_KMT_NO_MEMORY;
   const D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU args = {
      .hDevice = dev->handle, .ObjectCount = count,
      .ObjectHandleArray = handles, .FenceValueArray = values,
      .hAsyncEvent = event, .Flags.WaitAny = any,
   };
   LONG status = D3DKMTWaitForSynchronizationObjectFromCpu(&args);
   if (event) {
      if (status >= 0) {
         DWORD result = WaitForSingleObject(event, timeout_ms);
         status = result == WAIT_OBJECT_0 ? 0 :
                  result == WAIT_TIMEOUT ? TG_KMT_TIMEOUT : TG_KMT_DEVICE_REMOVED;
      }
      CloseHandle(event);
   }
   return status;
}

static LONG
wait_paging(const struct tg_kmt_device *dev, uint64_t target)
{
   if (dev->paging.value && *dev->paging.value >= target)
      return 0;
   return tg_kmt_wait_fences(dev, 1, &dev->paging.handle, &target, false, INFINITE);
}

LONG
tg_kmt_allocate(struct tg_kmt_device *dev, uint64_t size, bool cpu_visible,
                struct tg_kmt_allocation *allocation)
{
   *allocation = (struct tg_kmt_allocation){0};
   if (!size || size > dev->budget)
      return TG_KMT_NO_VIDEO_MEMORY;
   size = (size + TG_KMT_PAGE_SIZE - 1) & ~(TG_KMT_PAGE_SIZE - 1);
   AcquireSRWLockExclusive(&dev->memory_lock);
   const bool fits = size <= dev->budget - dev->allocated;
   if (fits)
      dev->allocated += size;
   ReleaseSRWLockExclusive(&dev->memory_lock);
   if (!fits)
      return TG_KMT_NO_VIDEO_MEMORY;

   struct tg_gpu_allocation_request request = {
      TG_GPU_ALLOCATION_ABI, cpu_visible ? TG_GPU_ALLOCATION_CPU_VISIBLE : 0, size,
   };
   D3DDDI_ALLOCATIONINFO2 info = {
      .pPrivateDriverData = &request, .PrivateDriverDataSize = sizeof(request),
   };
   D3DKMT_CREATEALLOCATION args = {
      .hDevice = dev->handle, .NumAllocations = 1, .pAllocationInfo2 = &info,
   };
   LONG status = D3DKMTCreateAllocation2(&args);
   if (status < 0) {
      AcquireSRWLockExclusive(&dev->memory_lock);
      dev->allocated -= size;
      ReleaseSRWLockExclusive(&dev->memory_lock);
      return status;
   }
   *allocation = (struct tg_kmt_allocation){ .handle = info.hAllocation, .size = size };
   D3DDDI_MAKERESIDENT resident = {
      .hPagingQueue = dev->paging_queue, .NumAllocations = 1,
      .AllocationList = &allocation->handle,
   };
   status = D3DKMTMakeResident(&resident);
   if (status >= 0)
      status = wait_paging(dev, resident.PagingFenceValue);
   if (status < 0)
      tg_kmt_free(dev, allocation);
   return status;
}

LONG
tg_kmt_free(struct tg_kmt_device *dev, struct tg_kmt_allocation *allocation)
{
   const D3DKMT_DESTROYALLOCATION2 args = {
      .hDevice = dev->handle, .AllocationCount = 1,
      .phAllocationList = &allocation->handle,
   };
   LONG status = D3DKMTDestroyAllocation2(&args);
   if (status >= 0) {
      AcquireSRWLockExclusive(&dev->memory_lock);
      dev->allocated -= allocation->size;
      ReleaseSRWLockExclusive(&dev->memory_lock);
      *allocation = (struct tg_kmt_allocation){0};
   }
   return status;
}

LONG
tg_kmt_lock(const struct tg_kmt_device *dev, const struct tg_kmt_allocation *allocation,
            void **cpu)
{
   D3DKMT_LOCK2 args = { .hDevice = dev->handle, .hAllocation = allocation->handle };
   LONG status = D3DKMTLock2(&args);
   if (status >= 0)
      *cpu = args.pData;
   return status;
}

LONG
tg_kmt_unlock(const struct tg_kmt_device *dev, const struct tg_kmt_allocation *allocation)
{
   const D3DKMT_UNLOCK2 args = { .hDevice = dev->handle, .hAllocation = allocation->handle };
   return D3DKMTUnlock2(&args);
}

LONG
tg_kmt_reserve(const struct tg_kmt_device *dev, uint64_t size, uint64_t alignment,
               uint64_t fixed, struct tg_kmt_va *va)
{
   const uint64_t space = TG_KMT_VA_END - TG_KMT_VA_START;
   if (!size || (size & (TG_KMT_PAGE_SIZE - 1)) || alignment < TG_KMT_PAGE_SIZE ||
       (alignment & (alignment - 1)) || alignment > space || size > space ||
       (fixed && ((fixed & (alignment - 1)) || fixed < TG_KMT_VA_START ||
                  fixed > TG_KMT_VA_END - size)))
      return TG_KMT_INVALID_PARAMETER;

   const uint64_t padding = fixed ? 0 : alignment - TG_KMT_PAGE_SIZE;
   if (size > space - padding)
      return TG_KMT_NO_VIDEO_MEMORY;

   D3DDDI_RESERVEGPUVIRTUALADDRESS args = {
      .hPagingQueue = dev->paging_queue, .BaseAddress = fixed,
      .MinimumAddress = fixed ? 0 : TG_KMT_VA_START,
      .MaximumAddress = fixed ? 0 : TG_KMT_VA_END - 1,
      .Size = size + padding, .ReservationType = D3DDDIGPUVIRTUALADDRESS_RESERVE_NO_ACCESS,
   };
   LONG status = D3DKMTReserveGpuVirtualAddress(&args);
   if (status < 0)
      return status;
   *va = (struct tg_kmt_va) {
      .address = (args.VirtualAddress + alignment - 1) & ~(alignment - 1),
      .size = size, .reserved_base = args.VirtualAddress, .reserved_size = args.Size,
   };
   status = wait_paging(dev, args.PagingFenceValue);
   if (status < 0)
      tg_kmt_free_va(dev, va);
   return status;
}

LONG
tg_kmt_free_va(const struct tg_kmt_device *dev, struct tg_kmt_va *va)
{
   const D3DKMT_FREEGPUVIRTUALADDRESS args = {
      .hAdapter = dev->adapter, .BaseAddress = va->reserved_base, .Size = va->reserved_size,
   };
   LONG status = D3DKMTFreeGpuVirtualAddress(&args);
   if (status >= 0)
      *va = (struct tg_kmt_va){0};
   return status;
}

LONG
tg_kmt_map(const struct tg_kmt_device *dev, const struct tg_kmt_va *va, uint64_t va_offset,
           const struct tg_kmt_allocation *allocation, uint64_t offset, uint64_t size,
           uint8_t kind)
{
   if (!size || ((va_offset | offset | size) & (TG_KMT_PAGE_SIZE - 1)) ||
       va_offset > va->size || size > va->size - va_offset ||
       offset > allocation->size || size > allocation->size - offset)
      return TG_KMT_INVALID_PARAMETER;
   D3DDDI_MAPGPUVIRTUALADDRESS args = {
      .hPagingQueue = dev->paging_queue, .BaseAddress = va->address + va_offset,
      .hAllocation = allocation->handle,
      .OffsetInPages = offset / TG_KMT_PAGE_SIZE, .SizeInPages = size / TG_KMT_PAGE_SIZE,
      .Protection.Write = 1, .DriverProtection = kind,
   };
   LONG status = D3DKMTMapGpuVirtualAddress(&args);
   if (status < 0)
      return status;
   return wait_paging(dev, args.PagingFenceValue);
}

LONG
tg_kmt_create_fence(const struct tg_kmt_device *dev, uint64_t initial, struct tg_kmt_fence *fence)
{
   D3DKMT_CREATESYNCHRONIZATIONOBJECT2 args = {
      .hDevice = dev->handle, .Info.Type = D3DDDI_MONITORED_FENCE,
      .Info.MonitoredFence = { .InitialFenceValue = initial, .EngineAffinity = 1 },
   };
   LONG status = D3DKMTCreateSynchronizationObject2(&args);
   if (status >= 0)
      *fence = (struct tg_kmt_fence) {
         .handle = args.hSyncObject, .value = args.Info.MonitoredFence.FenceValueCPUVirtualAddress,
      };
   return status;
}

LONG
tg_kmt_destroy_fence(struct tg_kmt_fence *fence)
{
   const D3DKMT_DESTROYSYNCHRONIZATIONOBJECT args = { .hSyncObject = fence->handle };
   LONG status = D3DKMTDestroySynchronizationObject(&args);
   if (status >= 0)
      *fence = (struct tg_kmt_fence){0};
   return status;
}

LONG
tg_kmt_signal_cpu(const struct tg_kmt_device *dev, uint32_t fence, uint64_t value)
{
   const D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMCPU args = {
      .hDevice = dev->handle, .ObjectCount = 1,
      .ObjectHandleArray = &fence, .FenceValueArray = &value,
   };
   MemoryBarrier();
   return D3DKMTSignalSynchronizationObjectFromCpu(&args);
}

LONG
tg_kmt_wait_gpu(uint32_t context, uint32_t count, const uint32_t *handles,
                const uint64_t *values)
{
   const D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMGPU args = {
      .hContext = context, .ObjectCount = count,
      .ObjectHandleArray = handles, .MonitoredFenceValueArray = values,
   };
   return D3DKMTWaitForSynchronizationObjectFromGpu(&args);
}

LONG
tg_kmt_signal_gpu(uint32_t context, uint32_t count, const uint32_t *handles,
                  const uint64_t *values)
{
   const D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMGPU args = {
      .hContext = context, .ObjectCount = count,
      .ObjectHandleArray = handles, .MonitoredFenceValueArray = values,
   };
   return D3DKMTSignalSynchronizationObjectFromGpu(&args);
}

LONG
tg_kmt_submit(uint32_t context, uint64_t address, const void *words, uint32_t bytes)
{
   if (!context || !address || !words || !bytes || (bytes & 3) || bytes > TG_GPU_SUBMIT_MAX_BYTES)
      return TG_KMT_INVALID_PARAMETER;
   const uint32_t size = sizeof(struct tg_gpu_submit_request) + bytes;
   struct tg_gpu_submit_request *request = malloc(size);
   if (!request)
      return TG_KMT_NO_MEMORY;
   *request = (struct tg_gpu_submit_request) {
      .version = TG_GPU_SUBMIT_ABI, .size = size, .command_bytes = bytes,
   };
   MemoryBarrier();
   memcpy(request + 1, words, bytes);
   const D3DKMT_SUBMITCOMMAND args = {
      .Commands = address, .CommandLength = bytes,
      .BroadcastContextCount = 1, .BroadcastContext = { context },
      .pPrivateDriverData = request, .PrivateDriverDataSize = size,
   };
   LONG status = D3DKMTSubmitCommand(&args);
   free(request);
   return status;
}
