/* SPDX-License-Identifier: MIT */
#define WIN32_NO_STATUS
#include "kmt_native.h"
#undef WIN32_NO_STATUS
#include <ntstatus.h>
#include <d3dkmthk.h>
#include <winternl.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <stdio.h>
#include <wchar.h>
#include "allocation_abi.h"
#include "submit_abi.h"

/* Keep the failing API/NTSTATUS before cleanup, even when a desktop process
 * has no stderr or Vulkan debug messenger. One debugger message per failure. */
static void
log_create_failure(const char *operation, LONG status)
{
   char text[128] = "[NVK KMT] ";
   size_t n = sizeof("[NVK KMT] ") - 1;
   while (*operation && n < sizeof(text) - 22)
      text[n++] = *operation++;
   const char suffix[] = " status=0x";
   memcpy(text + n, suffix, sizeof(suffix) - 1);
   n += sizeof(suffix) - 1;
   for (int shift = 28; shift >= 0; shift -= 4)
      text[n++] = "0123456789abcdef"[((uint32_t)status >> shift) & 15];
   text[n++] = '\n'; text[n] = 0;
   OutputDebugStringA(text);
}

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
   if (status < 0) {
      log_create_failure("D3DKMTOpenAdapterFromLuid", status);
      return status;
   }
   dev->adapter = open.hAdapter;

   const char *operation = "D3DKMTCreateDevice";
   D3DKMT_CREATEDEVICE device = { .hAdapter = dev->adapter };
   status = D3DKMTCreateDevice(&device);
   if (status < 0)
      goto fail;
   dev->handle = device.hDevice;

   D3DKMT_CREATEPAGINGQUEUE queue = {
      .hDevice = dev->handle, .Priority = D3DDDI_PAGINGQUEUE_PRIORITY_NORMAL,
   };
   operation = "D3DKMTCreatePagingQueue";
   status = D3DKMTCreatePagingQueue(&queue);
   if (status < 0)
      goto fail;
   dev->paging_queue = queue.hPagingQueue;
   dev->paging = (struct tg_kmt_fence) {
      .handle = queue.hSyncObject, .value = queue.FenceValueCPUVirtualAddress,
   };
   return 0;

fail:
   log_create_failure(operation, status);
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
   else
      log_create_failure("D3DKMTCreateContextVirtual", status);
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
   const uint64_t completed = dev->paging.value ? *dev->paging.value : 0;
   if (completed == UINT64_MAX || target == UINT64_MAX)
      return TG_KMT_DEVICE_REMOVED;
   if (!target || (dev->paging.value && completed >= target))
      return 0;
   LONG status = tg_kmt_wait_fences(dev, 1, &dev->paging.handle, &target, false, INFINITE);
   if (status >= 0 && dev->paging.value && *dev->paging.value == UINT64_MAX)
      return TG_KMT_DEVICE_REMOVED;
   return status;
}

static bool
charge_memory(struct tg_kmt_device *dev, uint64_t size)
{
   if (!size || size > dev->budget)
      return false;
   AcquireSRWLockExclusive(&dev->memory_lock);
   const bool fits = dev->allocated <= dev->budget && size <= dev->budget - dev->allocated;
   if (fits)
      dev->allocated += size;
   ReleaseSRWLockExclusive(&dev->memory_lock);
   return fits;
}

static LONG
make_resident(struct tg_kmt_device *dev, struct tg_kmt_allocation *allocation)
{
   D3DDDI_MAKERESIDENT resident = {
      .hPagingQueue = dev->paging_queue, .NumAllocations = 1,
      .AllocationList = &allocation->handle,
   };
   LONG status = D3DKMTMakeResident(&resident);
   if (status >= 0)
      status = wait_paging(dev, resident.PagingFenceValue);
   if (status < 0)
      tg_kmt_free(dev, allocation);
   return status;
}

static LONG
allocate_memory(struct tg_kmt_device *dev, uint64_t size, bool cpu_visible,
                 bool shared, struct tg_kmt_allocation *allocation)
{
   *allocation = (struct tg_kmt_allocation){0};
   if (size > UINT64_MAX - (TG_KMT_PAGE_SIZE - 1))
      return TG_KMT_NO_VIDEO_MEMORY;
   size = (size + TG_KMT_PAGE_SIZE - 1) & ~(TG_KMT_PAGE_SIZE - 1);
   if (!charge_memory(dev, size))
      return TG_KMT_NO_VIDEO_MEMORY;

   struct tg_gpu_allocation_request request = {
      TG_GPU_ALLOCATION_ABI, cpu_visible ? TG_GPU_ALLOCATION_CPU_VISIBLE : 0, size,
   };
   D3DDDI_ALLOCATIONINFO2 info = {
      .pPrivateDriverData = &request, .PrivateDriverDataSize = sizeof(request),
   };
   D3DKMT_CREATEALLOCATION args = {
      .hDevice = dev->handle, .NumAllocations = 1, .pAllocationInfo2 = &info,
      .Flags = {.CreateResource = shared, .CreateShared = shared, .NtSecuritySharing = shared},
   };
   HANDLE section = NULL;
   D3DKMT_CREATESTANDARDALLOCATION standard = {
      .Type = D3DKMT_STANDARDALLOCATIONTYPE_EXISTINGHEAP,
      .ExistingHeapData.Size = size,
   };
   LONG status;
   if (shared && cpu_visible) {
      /* On GM20B's GpuMmu-only path, an imported ordinary shared allocation
       * cannot be CPU-locked. Give VidMm an existing section to retain and share.
       * Use write combining to avoid a cached host alias on Tegra.
       */
      section = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL,
         PAGE_READWRITE | SEC_COMMIT | SEC_WRITECOMBINE,
         (DWORD)(size >> 32), (DWORD)size, NULL);
      if (!section) {
         /* Keep the actual Win32 failure. Access/privilege/flag errors are
          * not evidence of exhausted host memory. The Vulkan boundary
          * classifies this standard NTWIN32 encoding separately. */
         const DWORD error = GetLastError();
         status = error ? NTSTATUS_FROM_WIN32(error) : STATUS_UNSUCCESSFUL;
         goto fail_charge;
      }
      info.hSection = section;
      info.pPrivateDriverData = NULL;
      info.PrivateDriverDataSize = 0;
      args.pStandardAllocation = &standard;
      args.Flags.StandardAllocation = true;
      args.Flags.ExistingSection = true;
      args.Flags.CrossAdapter = true;
   }
   status = D3DKMTCreateAllocation2(&args);
   if (section)
      CloseHandle(section);
   if (status >= 0) {
      *allocation = (struct tg_kmt_allocation){
         .handle = info.hAllocation, .size = size, .resource = args.hResource, .flags = request.flags,
      };
      return make_resident(dev, allocation);
   }
fail_charge:
   AcquireSRWLockExclusive(&dev->memory_lock);
   dev->allocated -= size;
   ReleaseSRWLockExclusive(&dev->memory_lock);
   return status;
}

LONG
tg_kmt_allocate(struct tg_kmt_device *dev, uint64_t size, bool cpu_visible,
                struct tg_kmt_allocation *allocation)
{
   return allocate_memory(dev, size, cpu_visible, false, allocation);
}

LONG
tg_kmt_allocate_shared(struct tg_kmt_device *dev, uint64_t size, bool cpu_visible,
                       struct tg_kmt_allocation *allocation)
{
   return allocate_memory(dev, size, cpu_visible, true, allocation);
}

LONG
tg_kmt_export_memory(const struct tg_kmt_allocation *allocation, HANDLE *handle)
{
   return tg_kmt_export_memory_win32(allocation, NULL,
                                     STANDARD_RIGHTS_READ | SHARED_ALLOCATION_WRITE,
                                     NULL, handle);
}

/* D3DKMT takes an object-manager name, rather than the Win32 name used by
 * Vulkan. Match the per-session, Local\\ and Global\\ named-object namespaces.
 */
static LONG
shared_object_name(LPCWSTR name, UNICODE_STRING *object_name)
{
   WCHAR prefix[64];
   int prefix_len;
   if (wcsncmp(name, L"Global\\", 7) == 0) {
      name += 7;
      prefix_len = _snwprintf(prefix, ARRAYSIZE(prefix), L"\\BaseNamedObjects\\");
   } else {
      if (wcsncmp(name, L"Local\\", 6) == 0)
         name += 6;
      DWORD session;
      if (!ProcessIdToSessionId(GetCurrentProcessId(), &session))
         return TG_KMT_INVALID_PARAMETER;
      prefix_len = session ?
         _snwprintf(prefix, ARRAYSIZE(prefix), L"\\Sessions\\%lu\\BaseNamedObjects\\", session) :
         _snwprintf(prefix, ARRAYSIZE(prefix), L"\\BaseNamedObjects\\");
   }
   if (!*name || wcschr(name, L'\\') || prefix_len < 0)
      return TG_KMT_INVALID_PARAMETER;
   const size_t length = (size_t)prefix_len + wcslen(name);
   if (length >= USHRT_MAX / sizeof(WCHAR))
      return TG_KMT_INVALID_PARAMETER;
   WCHAR *buffer = calloc(length + 1, sizeof(WCHAR));
   if (!buffer)
      return TG_KMT_NO_MEMORY;
   memcpy(buffer, prefix, prefix_len * sizeof(WCHAR));
   memcpy(buffer + prefix_len, name, (length - prefix_len) * sizeof(WCHAR));
   *object_name = (UNICODE_STRING) {
      .Length = (USHORT)(length * sizeof(WCHAR)),
      .MaximumLength = (USHORT)((length + 1) * sizeof(WCHAR)),
      .Buffer = buffer,
   };
   return 0;
}

static LONG
export_object_win32(uint32_t object, const SECURITY_ATTRIBUTES *security,
                     DWORD access, LPCWSTR name, HANDLE *handle)
{
   if (!handle)
      return TG_KMT_INVALID_PARAMETER;
   *handle = NULL;
   if (!object)
      return TG_KMT_INVALID_PARAMETER;
   UNICODE_STRING object_name = {0};
   if (name) {
      LONG status = shared_object_name(name, &object_name);
      if (status < 0)
         return status;
   }
   OBJECT_ATTRIBUTES attributes = {
      .Length = sizeof(attributes),
      .ObjectName = name ? &object_name : NULL,
      .Attributes = security && security->bInheritHandle ? OBJ_INHERIT : 0,
      .SecurityDescriptor = security ? security->lpSecurityDescriptor : NULL,
   };
   LONG status = D3DKMTShareObjects(1, &object, &attributes, access, handle);
   free(object_name.Buffer);
   return status;
}

LONG
tg_kmt_export_memory_win32(const struct tg_kmt_allocation *allocation,
                           const SECURITY_ATTRIBUTES *security, DWORD access,
                           LPCWSTR name, HANDLE *handle)
{
   uint32_t object = allocation->handle && allocation->size ? allocation->resource : 0;
   return export_object_win32(object, security, access, name, handle);
}

LONG
tg_kmt_open_memory_name(LPCWSTR name, HANDLE *handle)
{
   if (!handle)
      return TG_KMT_INVALID_PARAMETER;
   *handle = NULL;
   if (!name)
      return TG_KMT_INVALID_PARAMETER;
   UNICODE_STRING object_name;
   LONG status = shared_object_name(name, &object_name);
   if (status < 0)
      return status;
   OBJECT_ATTRIBUTES attributes = {
      .Length = sizeof(attributes), .ObjectName = &object_name,
   };
   D3DKMT_OPENNTHANDLEFROMNAME open = {
      .dwDesiredAccess = STANDARD_RIGHTS_READ | SHARED_ALLOCATION_WRITE,
      .pObjAttrib = &attributes,
   };
   status = D3DKMTOpenNtHandleFromName(&open);
   if (status >= 0)
      *handle = open.hNtHandle;
   free(object_name.Buffer);
   return status;
}

LONG
tg_kmt_import_memory(struct tg_kmt_device *dev, HANDLE handle,
                     struct tg_kmt_allocation *allocation)
{
   *allocation = (struct tg_kmt_allocation){0};
   D3DKMT_QUERYRESOURCEINFOFROMNTHANDLE query = {
      .hDevice = dev->handle, .hNtHandle = handle,
   };
   LONG status = D3DKMTQueryResourceInfoFromNtHandle(&query);
   if (status < 0)
      return status;
   /* Only the Tegra opaque-storage resource with one allocation belongs to
    * this transport. D3D textures need their own layout/runtime metadata.
    */
   if (query.NumAllocations != 1 || query.PrivateRuntimeDataSize ||
       query.ResourcePrivateDriverDataSize ||
       query.TotalPrivateDriverDataSize != sizeof(struct tg_gpu_allocation_request))
      return TG_KMT_INVALID_PARAMETER;

   struct tg_gpu_allocation_request request = {0};
   D3DDDI_OPENALLOCATIONINFO2 info = {0};
   D3DKMT_OPENRESOURCEFROMNTHANDLE open = {
      .hDevice = dev->handle, .hNtHandle = handle,
      .NumAllocations = 1, .pOpenAllocationInfo2 = &info,
      .TotalPrivateDriverDataBufferSize = sizeof(request), .pTotalPrivateDriverDataBuffer = &request,
   };
   status = D3DKMTOpenResourceFromNtHandle(&open);
   if (status < 0)
      return status;
   allocation->handle = info.hAllocation;
   allocation->resource = open.hResource;
   status = TG_KMT_INVALID_PARAMETER;
   if (!open.hResource || !info.hAllocation ||
       open.TotalPrivateDriverDataBufferSize != sizeof(request) ||
       info.PrivateDriverDataSize != sizeof(request) ||
       request.version != TG_GPU_ALLOCATION_ABI ||
       (request.flags & ~TG_GPU_ALLOCATION_CPU_VISIBLE) ||
       !request.bytes || (request.bytes & (TG_KMT_PAGE_SIZE - 1)))
      goto fail;
   status = TG_KMT_NO_VIDEO_MEMORY;
   if (!charge_memory(dev, request.bytes))
      goto fail;
   allocation->size = request.bytes;
   allocation->flags = request.flags;
   return make_resident(dev, allocation);

fail:
   tg_kmt_free(dev, allocation);
   return status;
}

LONG
tg_kmt_free(struct tg_kmt_device *dev, struct tg_kmt_allocation *allocation)
{
   const D3DKMT_DESTROYALLOCATION2 args = {
      .hDevice = dev->handle, .hResource = allocation->resource,
      .AllocationCount = allocation->resource ? 0 : 1,
      .phAllocationList = allocation->resource ? NULL : &allocation->handle,
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
           uint8_t kind, bool read_only)
{
   if (!size || ((va_offset | offset | size) & (TG_KMT_PAGE_SIZE - 1)) ||
       va_offset > va->size || size > va->size - va_offset ||
       offset > allocation->size || size > allocation->size - offset)
      return TG_KMT_INVALID_PARAMETER;
   D3DDDI_MAPGPUVIRTUALADDRESS args = {
      .hPagingQueue = dev->paging_queue, .BaseAddress = va->address + va_offset,
      .hAllocation = allocation->handle,
      .OffsetInPages = offset / TG_KMT_PAGE_SIZE, .SizeInPages = size / TG_KMT_PAGE_SIZE,
      .Protection.Write = !read_only, .DriverProtection = kind,
   };
   LONG status = D3DKMTMapGpuVirtualAddress(&args);
   if (status < 0)
      return status;
   return wait_paging(dev, args.PagingFenceValue);
}

static LONG
create_fence(const struct tg_kmt_device *dev, uint64_t initial, bool shared,
              struct tg_kmt_fence *fence)
{
   *fence = (struct tg_kmt_fence){0};
   D3DKMT_CREATESYNCHRONIZATIONOBJECT2 args = {
      .hDevice = dev->handle, .Info.Type = D3DDDI_MONITORED_FENCE,
      .Info.Flags = { .Shared = shared, .NtSecuritySharing = shared },
      .Info.MonitoredFence = { .InitialFenceValue = initial, .EngineAffinity = 1 },
   };
   LONG status = D3DKMTCreateSynchronizationObject2(&args);
   if (status >= 0)
      *fence = (struct tg_kmt_fence) {
         .handle = args.hSyncObject, .value = args.Info.MonitoredFence.FenceValueCPUVirtualAddress,
      };
   else
      log_create_failure("D3DKMTCreateSynchronizationObject2", status);
   return status;
}

LONG
tg_kmt_create_fence(const struct tg_kmt_device *dev, uint64_t initial,
                     struct tg_kmt_fence *fence)
{
   return create_fence(dev, initial, false, fence);
}

LONG
tg_kmt_create_shared_fence(const struct tg_kmt_device *dev, uint64_t initial,
                            struct tg_kmt_fence *fence)
{
   return create_fence(dev, initial, true, fence);
}

LONG
tg_kmt_export_fence_win32(const struct tg_kmt_fence *fence,
                          const SECURITY_ATTRIBUTES *security, DWORD access,
                          LPCWSTR name, HANDLE *handle)
{
   return export_object_win32(fence->handle, security, access, name, handle);
}

LONG
tg_kmt_import_fence(const struct tg_kmt_device *dev, HANDLE handle,
                     LPCWSTR name, struct tg_kmt_fence *fence)
{
   *fence = (struct tg_kmt_fence){0};
   if ((!handle && !name) || (handle && name))
      return TG_KMT_INVALID_PARAMETER;

   HANDLE named_handle = NULL;
   if (name) {
      UNICODE_STRING object_name = {0};
      LONG status = shared_object_name(name, &object_name);
      if (status < 0)
         return status;
      OBJECT_ATTRIBUTES attributes = {
         .Length = sizeof(attributes), .ObjectName = &object_name,
      };
      D3DKMT_OPENSYNCOBJECTNTHANDLEFROMNAME open = {
         .dwDesiredAccess = GENERIC_ALL, .pObjAttrib = &attributes,
      };
      status = D3DKMTOpenSyncObjectNtHandleFromName(&open);
      free(object_name.Buffer);
      if (status < 0)
         return status;
      named_handle = handle = open.hNtHandle;
   }

   /* Zero behavior flags mean waits/signals are enabled and signal happens
    * after execution, not merely after the commands enter the GPU pipeline. */
   D3DKMT_OPENSYNCOBJECTFROMNTHANDLE2 open = {
      .hNtHandle = handle, .hDevice = dev->handle,
      .MonitoredFence.EngineAffinity = 1,
   };
   LONG status = D3DKMTOpenSyncObjectFromNtHandle2(&open);
   if (named_handle)
      CloseHandle(named_handle);
   if (status < 0)
      return status;
   if (!open.hSyncObject || !open.MonitoredFence.FenceValueCPUVirtualAddress) {
      if (open.hSyncObject) {
         const D3DKMT_DESTROYSYNCHRONIZATIONOBJECT destroy = { .hSyncObject = open.hSyncObject };
         D3DKMTDestroySynchronizationObject(&destroy);
      }
      return TG_KMT_INVALID_PARAMETER;
   }
   *fence = (struct tg_kmt_fence) {
      .handle = open.hSyncObject,
      .value = open.MonitoredFence.FenceValueCPUVirtualAddress,
   };
   return 0;
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
tg_kmt_submit_primaries(uint32_t context, uint64_t address, const void *words,
                       uint32_t bytes, uint32_t count, const uint32_t *allocations)
{
   if (!context || !address || !words || !bytes || (bytes & 3) || bytes > TG_GPU_SUBMIT_MAX_BYTES ||
       count > D3DDDI_MAX_WRITTEN_PRIMARIES || (count && !allocations))
      return TG_KMT_INVALID_PARAMETER;
   for (uint32_t i = 0; i < count; ++i) {
      if (!allocations[i])
         return TG_KMT_INVALID_PARAMETER;
      for (uint32_t j = 0; j < i; ++j)
         if (allocations[i] == allocations[j])
            return TG_KMT_INVALID_PARAMETER;
   }
   const uint32_t size = sizeof(struct tg_gpu_submit_request) + bytes;
   struct tg_gpu_submit_request *request = malloc(size);
   if (!request)
      return TG_KMT_NO_MEMORY;
   *request = (struct tg_gpu_submit_request) {
      .version = TG_GPU_SUBMIT_ABI, .size = size, .command_bytes = bytes,
   };
   MemoryBarrier();
   memcpy(request + 1, words, bytes);
   D3DKMT_SUBMITCOMMAND args = {
      .Commands = address, .CommandLength = bytes,
      .BroadcastContextCount = 1, .BroadcastContext = { context },
      .pPrivateDriverData = request, .PrivateDriverDataSize = size,
      .NumPrimaries = count,
   };
   if (count)
      memcpy(args.WrittenPrimaries, allocations, count * sizeof(*allocations));
   LONG status = D3DKMTSubmitCommand(&args);
   free(request);
   return status;
}

LONG
tg_kmt_submit(uint32_t context, uint64_t address, const void *words, uint32_t bytes)
{
   return tg_kmt_submit_primaries(context, address, words, bytes, 0, NULL);
}

LONG
tg_kmt_close_runtime_alias(const struct tg_kmt_device *dev, struct tg_kmt_runtime_alias *alias)
{
   if (!dev || !alias || (alias->device && alias->device != dev->handle))
      return TG_KMT_INVALID_PARAMETER;
   if (!alias->resource && !alias->allocation) {
      *alias = (struct tg_kmt_runtime_alias){0};
      return 0;
   }
   const D3DKMT_DESTROYALLOCATION2 destroy = {
      .hDevice = dev->handle, .hResource = alias->resource,
      .AllocationCount = alias->resource ? 0 : 1,
      .phAllocationList = alias->resource ? NULL : &alias->allocation,
   };
   LONG status = D3DKMTDestroyAllocation2(&destroy);
   if (status >= 0)
      *alias = (struct tg_kmt_runtime_alias){0};
   return status;
}

LONG
tg_kmt_open_runtime_alias(struct tg_kmt_device *dev, uint32_t resource,
                          uint64_t expected_bytes, struct tg_kmt_runtime_alias *alias)
{
   if (!dev || !dev->handle || !resource || !alias || alias->device || alias->resource ||
       alias->allocation || !expected_bytes || (expected_bytes & (TG_KMT_PAGE_SIZE - 1)))
      return TG_KMT_INVALID_PARAMETER;
   HANDLE handle = NULL;
   LONG status = export_object_win32(resource, NULL,
      STANDARD_RIGHTS_READ | SHARED_ALLOCATION_WRITE, NULL, &handle);
   if (status < 0)
      return status; /* Do not fall back to a foreign device's allocation. */
   if (!handle)
      return TG_KMT_INVALID_PARAMETER;
   D3DKMT_QUERYRESOURCEINFOFROMNTHANDLE query = {
      .hDevice = dev->handle, .hNtHandle = handle,
   };
   status = D3DKMTQueryResourceInfoFromNtHandle(&query);
   void *runtime_data = NULL, *resource_data = NULL, *allocation_data = NULL;
   if (status < 0)
      goto done;
   /* This transport is only for one validated Tegra allocation. Runtime
    * private data is opaque; bound each returned length before allocation. */
   if (query.NumAllocations != 1 || query.PrivateRuntimeDataSize > 1024 * 1024 ||
       query.ResourcePrivateDriverDataSize > 4096 ||
       query.TotalPrivateDriverDataSize < sizeof(struct tg_gpu_allocation_request) ||
       query.TotalPrivateDriverDataSize > 4096) {
      status = TG_KMT_INVALID_PARAMETER;
      goto done;
   }
   runtime_data = calloc(1, query.PrivateRuntimeDataSize ? query.PrivateRuntimeDataSize : 1);
   resource_data = calloc(1, query.ResourcePrivateDriverDataSize ? query.ResourcePrivateDriverDataSize : 1);
   allocation_data = calloc(1, query.TotalPrivateDriverDataSize);
   if (!runtime_data || !resource_data || !allocation_data) {
      status = TG_KMT_NO_MEMORY;
      goto done;
   }
   D3DDDI_OPENALLOCATIONINFO2 info = {0};
   D3DKMT_OPENRESOURCEFROMNTHANDLE open = {
      .hDevice = dev->handle, .hNtHandle = handle,
      .NumAllocations = 1, .pOpenAllocationInfo2 = &info,
      .PrivateRuntimeDataSize = query.PrivateRuntimeDataSize, .pPrivateRuntimeData = runtime_data,
      .ResourcePrivateDriverDataSize = query.ResourcePrivateDriverDataSize,
      .pResourcePrivateDriverData = resource_data,
      .TotalPrivateDriverDataBufferSize = query.TotalPrivateDriverDataSize,
      .pTotalPrivateDriverDataBuffer = allocation_data,
   };
   status = D3DKMTOpenResourceFromNtHandle(&open);
   *alias = (struct tg_kmt_runtime_alias) {dev->handle, open.hResource, info.hAllocation};
   if (status < 0)
      goto unwind;
   const uintptr_t begin = (uintptr_t)allocation_data, data = (uintptr_t)info.pPrivateDriverData;
   const uint32_t private_bytes = query.TotalPrivateDriverDataSize;
   status = TG_KMT_INVALID_PARAMETER;
   if (!alias->resource || !alias->allocation ||
       open.TotalPrivateDriverDataBufferSize != private_bytes ||
       open.PrivateRuntimeDataSize != query.PrivateRuntimeDataSize ||
       open.ResourcePrivateDriverDataSize != query.ResourcePrivateDriverDataSize ||
       info.PrivateDriverDataSize < sizeof(struct tg_gpu_allocation_request) ||
       data < begin || data - begin > private_bytes ||
       info.PrivateDriverDataSize > private_bytes - (data - begin))
      goto unwind;
   struct tg_gpu_allocation_request storage;
   memcpy(&storage, info.pPrivateDriverData, sizeof(storage));
   if (storage.version != TG_GPU_ALLOCATION_ABI || storage.bytes != expected_bytes ||
       (storage.flags & ~(TG_GPU_ALLOCATION_CPU_VISIBLE | TG_GPU_ALLOCATION_DISPLAY_PRIMARY |
                          TG_GPU_ALLOCATION_LINEAR_SURFACE)))
      goto unwind;
   D3DDDI_MAKERESIDENT resident = {
      .hPagingQueue = dev->paging_queue, .NumAllocations = 1,
      .AllocationList = &alias->allocation,
   };
   status = D3DKMTMakeResident(&resident);
   if (status >= 0)
      status = wait_paging(dev, resident.PagingFenceValue);
   if (status < 0) {
unwind:;
      LONG cleanup = tg_kmt_close_runtime_alias(dev, alias);
      if (cleanup < 0)
         status = cleanup; /* Keep failed-close ownership in alias. */
   }
done:
   free(allocation_data); free(resource_data); free(runtime_data);
   CloseHandle(handle);
   return status;
}
