/* SPDX-License-Identifier: MIT */
/* Windows SDK ABI and ownership checks with controlled KMT substitutes. */
#include "kmt_native.h"
#include "allocation_abi.h"
#include <d3dkmthk.h>
#include <winternl.h>
#include <string.h>
#include <stdio.h>
#include <wchar.h>

#define CHECK(test) do { if (!(test)) ExitProcess(__LINE__); } while (0)

static struct {
   UINT device;
   bool alive, shared;
} allocations[32];
static UINT next_allocation, opens, destroys, residents;
static struct tg_gpu_allocation_request shared_request;
static HANDLE shared_handle;
static HANDLE last_section;
static LONG create_status, open_status, resident_status, destroy_status;
static bool bad_query, bad_descriptor;
static WCHAR expected_name[128];
static DWORD expected_access = STANDARD_RIGHTS_READ | SHARED_ALLOCATION_WRITE;
static PVOID expected_security;
static bool expected_inherit;
static DWORD section_error;
static UINT section_calls;
static uint64_t expected_allocation_bytes = 8192;
static uint64_t reserved_base, reserved_size;
static UINT reserves, frees;

NTSTATUS APIENTRY
D3DKMTReserveGpuVirtualAddress(D3DDDI_RESERVEGPUVIRTUALADDRESS *args)
{
   CHECK(args->hPagingQueue == 3 && !args->BaseAddress &&
         args->MinimumAddress == TG_KMT_VA_START &&
         args->MaximumAddress == TG_KMT_VA_END - 1 &&
         args->ReservationType == D3DDDIGPUVIRTUALADDRESS_RESERVE_NO_ACCESS);
   reserved_base = args->VirtualAddress = TG_KMT_VA_START + TG_KMT_PAGE_SIZE;
   reserved_size = args->Size;
   args->PagingFenceValue = 7;
   reserves++;
   return 0;
}

NTSTATUS APIENTRY
D3DKMTFreeGpuVirtualAddress(const D3DKMT_FREEGPUVIRTUALADDRESS *args)
{
   CHECK(args->BaseAddress == reserved_base && args->Size == reserved_size);
   frees++;
   return 0;
}

typedef HANDLE (WINAPI *create_mapping_fn)(HANDLE, LPSECURITY_ATTRIBUTES,
                                          DWORD, DWORD, DWORD, LPCWSTR);

static HANDLE WINAPI
create_mapping_control(HANDLE file, LPSECURITY_ATTRIBUTES security,
                        DWORD protection, DWORD high, DWORD low, LPCWSTR name)
{
   section_calls++;
   CHECK(file == INVALID_HANDLE_VALUE && !security && !name && !high && low == 8192);
   CHECK(protection == (PAGE_READWRITE | SEC_COMMIT | SEC_WRITECOMBINE));
   if (section_error) {
      SetLastError(section_error);
      return NULL;
   }
   create_mapping_fn real_create = (create_mapping_fn)GetProcAddress(
      GetModuleHandleW(L"kernel32.dll"), "CreateFileMappingW");
   CHECK(real_create);
   return real_create(file, security, protection, high, low, name);
}

/* Intercept this SDK import in the fixture only. Successful requests still
 * use the real Windows section API and VirtualQuery checks below. */
#ifdef _M_IX86
create_mapping_fn imp_CreateFileMappingW __asm__("__imp__CreateFileMappingW@24") =
   create_mapping_control;
#else
create_mapping_fn __imp_CreateFileMappingW = create_mapping_control;
#endif

static void
check_object_attributes(const OBJECT_ATTRIBUTES *attributes)
{
   CHECK(attributes->Length == sizeof(*attributes) && !attributes->RootDirectory);
   if (*expected_name) {
      const UNICODE_STRING *name = attributes->ObjectName;
      CHECK(name && name->Length == wcslen(expected_name) * sizeof(WCHAR) &&
            name->MaximumLength == name->Length + sizeof(WCHAR) &&
            wcscmp(name->Buffer, expected_name) == 0);
   } else {
      CHECK(!attributes->ObjectName);
   }
}

static UINT
new_allocation(UINT device, bool shared)
{
   CHECK(next_allocation + 1 < ARRAYSIZE(allocations));
   const UINT handle = ++next_allocation;
   allocations[handle].device = device;
   allocations[handle].alive = true;
   allocations[handle].shared = shared;
   return handle;
}

NTSTATUS APIENTRY
D3DKMTCreateAllocation2(D3DKMT_CREATEALLOCATION *args)
{
   CHECK(args->NumAllocations == 1 && args->pAllocationInfo2 &&
         !args->PrivateRuntimeDataSize && !args->PrivateDriverDataSize);
   const bool shared = args->Flags.CreateShared != 0;
   const D3DDDI_ALLOCATIONINFO2 *info = args->pAllocationInfo2;
   struct tg_gpu_allocation_request section_request = {0};
   const struct tg_gpu_allocation_request *request;
   if (args->Flags.StandardAllocation) {
      CHECK(shared && args->Flags.ExistingSection && args->Flags.CrossAdapter &&
            !args->Flags.ExistingSysMem && !args->Flags.CreateCached &&
            !args->Flags.CreateWriteCombined && !info->PrivateDriverDataSize &&
            !info->pPrivateDriverData && info->hSection &&
            args->pStandardAllocation->Type == D3DKMT_STANDARDALLOCATIONTYPE_EXISTINGHEAP);
      void *view = MapViewOfFile(info->hSection, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0);
      MEMORY_BASIC_INFORMATION memory;
      CHECK(view && VirtualQuery(view, &memory, sizeof(memory)) == sizeof(memory) &&
            (memory.Protect & PAGE_WRITECOMBINE));
      CHECK(UnmapViewOfFile(view));
      last_section = info->hSection;
      section_request = (struct tg_gpu_allocation_request) {
         TG_GPU_ALLOCATION_ABI, TG_GPU_ALLOCATION_CPU_VISIBLE,
         args->pStandardAllocation->ExistingHeapData.Size,
      };
      request = &section_request;
   } else {
      CHECK(!shared && !args->Flags.ExistingSection && !args->Flags.CrossAdapter);
      CHECK(info->PrivateDriverDataSize == sizeof(shared_request));
      request = info->pPrivateDriverData;
   }
   CHECK(request->version == TG_GPU_ALLOCATION_ABI && request->bytes == expected_allocation_bytes &&
         request->flags == TG_GPU_ALLOCATION_CPU_VISIBLE);
   if (create_status)
      return create_status;
   CHECK(args->Flags.CreateResource == shared && args->Flags.NtSecuritySharing == shared);
   args->pAllocationInfo2->hAllocation = new_allocation(args->hDevice, shared);
   if (shared) {
      args->hResource = args->pAllocationInfo2->hAllocation + 100;
      shared_request = *request;
   }
   return 0;
}

NTSTATUS APIENTRY
D3DKMTDestroyAllocation2(const D3DKMT_DESTROYALLOCATION2 *args)
{
   UINT handle;
   if (args->hResource) {
      CHECK(!args->AllocationCount && !args->phAllocationList);
      handle = args->hResource - 100;
      CHECK(handle <= next_allocation && allocations[handle].shared);
   } else {
      CHECK(args->AllocationCount == 1 && args->phAllocationList);
      handle = args->phAllocationList[0];
      CHECK(handle <= next_allocation && !allocations[handle].shared);
   }
   CHECK(handle && allocations[handle].alive && allocations[handle].device == args->hDevice);
   if (destroy_status)
      return destroy_status;
   allocations[handle].alive = false;
   destroys++;
   return 0;
}

NTSTATUS APIENTRY
D3DKMTMakeResident(D3DDDI_MAKERESIDENT *args)
{
   CHECK(args->NumAllocations == 1 && args->AllocationList &&
         allocations[args->AllocationList[0]].alive);
   residents++;
   args->PagingFenceValue = 7;
   return resident_status;
}

NTSTATUS APIENTRY
D3DKMTWaitForSynchronizationObjectFromCpu(const D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU *args)
{
   (void)args;
   /* The fixture's completed paging fence already covers every request. */
   return TG_KMT_INVALID_PARAMETER;
}

NTSTATUS APIENTRY
D3DKMTShareObjects(UINT count, const D3DKMT_HANDLE *objects,
                   POBJECT_ATTRIBUTES attributes, DWORD access, HANDLE *handle)
{
   CHECK(count == 1 && *objects > 100 && allocations[*objects - 100].alive);
   check_object_attributes(attributes);
   CHECK(access == expected_access && attributes->SecurityDescriptor == expected_security &&
         attributes->Attributes == (expected_inherit ? OBJ_INHERIT : 0));
   SECURITY_ATTRIBUTES security = {sizeof(security), NULL, expected_inherit};
   *handle = CreateEventW(&security, FALSE, FALSE, NULL);
   CHECK(*handle);
   shared_handle = *handle;
   return 0;
}

NTSTATUS APIENTRY
D3DKMTOpenNtHandleFromName(D3DKMT_OPENNTHANDLEFROMNAME *args)
{
   check_object_attributes(args->pObjAttrib);
   CHECK(args->dwDesiredAccess == (STANDARD_RIGHTS_READ | SHARED_ALLOCATION_WRITE) &&
         !args->pObjAttrib->Attributes && !args->pObjAttrib->SecurityDescriptor);
   CHECK(DuplicateHandle(GetCurrentProcess(), shared_handle, GetCurrentProcess(),
                         &args->hNtHandle, 0, FALSE, DUPLICATE_SAME_ACCESS));
   return 0;
}

NTSTATUS APIENTRY
D3DKMTQueryResourceInfoFromNtHandle(D3DKMT_QUERYRESOURCEINFOFROMNTHANDLE *args)
{
   DWORD flags;
   CHECK(args->hNtHandle == shared_handle && GetHandleInformation(shared_handle, &flags));
   args->NumAllocations = bad_query ? 2 : 1;
   args->TotalPrivateDriverDataSize = sizeof(shared_request);
   return 0;
}

NTSTATUS APIENTRY
D3DKMTOpenResourceFromNtHandle(D3DKMT_OPENRESOURCEFROMNTHANDLE *args)
{
   CHECK(args->hNtHandle == shared_handle && args->NumAllocations == 1 &&
         !args->PrivateRuntimeDataSize && !args->ResourcePrivateDriverDataSize &&
         args->TotalPrivateDriverDataBufferSize == sizeof(shared_request));
   opens++;
   if (open_status)
      return open_status;
   args->pOpenAllocationInfo2->hAllocation = new_allocation(args->hDevice, true);
   args->hResource = args->pOpenAllocationInfo2->hAllocation + 100;
   args->pOpenAllocationInfo2->PrivateDriverDataSize = sizeof(shared_request);
   args->pOpenAllocationInfo2->pPrivateDriverData = args->pTotalPrivateDriverDataBuffer;
   struct tg_gpu_allocation_request *request = args->pTotalPrivateDriverDataBuffer;
   *request = shared_request;
   if (bad_descriptor)
      request->version++;
   return 0;
}

int main(void)
{
   const volatile uint64_t completed = 7;
   struct tg_kmt_device producer = {
      .handle = 1, .paging_queue = 3, .paging = {.value = &completed}, .budget = 65536,
   };
   struct tg_kmt_device consumer = {
      .handle = 2, .paging_queue = 4, .paging = {.value = &completed}, .budget = 65536,
   };
   InitializeSRWLock(&producer.memory_lock);
   InitializeSRWLock(&consumer.memory_lock);
   struct tg_kmt_allocation original, imported, second;
   HANDLE handle;
   DWORD flags;

   const DWORD section_errors[] = {ERROR_ACCESS_DENIED, ERROR_PRIVILEGE_NOT_HELD,
      ERROR_INVALID_PARAMETER, ERROR_NOT_SUPPORTED, ERROR_NOT_ENOUGH_MEMORY,
      ERROR_OUTOFMEMORY, ERROR_COMMITMENT_LIMIT, ERROR_NO_SYSTEM_RESOURCES,
      ERROR_NONPAGED_SYSTEM_RESOURCES, ERROR_PAGED_SYSTEM_RESOURCES,
      ERROR_WORKING_SET_QUOTA, ERROR_PAGEFILE_QUOTA, ERROR_NOT_ENOUGH_QUOTA};
   for (UINT i = 0; i < ARRAYSIZE(section_errors); i++) {
      section_error = section_errors[i];
      CHECK(tg_kmt_allocate_shared(&producer, 4097, true, &original) ==
            (LONG)(UINT32_C(0xc0070000) | section_error));
      CHECK(section_calls == i + 1 && !producer.allocated && !original.handle &&
            !next_allocation && !residents);
   }
   section_error = 0;

   /* Charge page-rounded byte counts at the actual 512 MiB ceiling without
    * committing that much RAM: these nonshared KMT calls are substituted. */
   producer.budget = TG_GPU_MEMORY_BUDGET_BYTES;
   expected_allocation_bytes = producer.budget - TG_KMT_PAGE_SIZE;
   CHECK(tg_kmt_allocate(&producer, expected_allocation_bytes, true, &original) == 0);
   CHECK(tg_kmt_allocate(&producer, TG_KMT_PAGE_SIZE + 1, true, &second) == TG_KMT_NO_VIDEO_MEMORY);
   expected_allocation_bytes = TG_KMT_PAGE_SIZE;
   CHECK(tg_kmt_allocate(&producer, 1, true, &second) == 0 &&
         producer.allocated == UINT64_C(536870912));
   CHECK(tg_kmt_allocate(&producer, 1, true, &imported) == TG_KMT_NO_VIDEO_MEMORY);
   CHECK(tg_kmt_free(&producer, &original) == 0 && tg_kmt_free(&producer, &second) == 0 &&
         !producer.allocated);
   producer.budget = 65536;
   expected_allocation_bytes = 8192;

   const uint64_t alignments[] = {TG_KMT_PAGE_SIZE, UINT64_C(65536), UINT64_C(2) * 1024 * 1024};
   struct tg_kmt_va va;
   for (UINT i = 0; i < ARRAYSIZE(alignments); i++) {
      CHECK(tg_kmt_reserve(&producer, TG_KMT_PAGE_SIZE, alignments[i], 0, &va) == 0);
      CHECK(!(va.address % alignments[i]) && va.address >= reserved_base &&
            va.address + va.size <= reserved_base + reserved_size &&
            reserved_size == alignments[i]);
      CHECK(tg_kmt_free_va(&producer, &va) == 0);
   }
   CHECK(tg_kmt_reserve(&producer, TG_KMT_PAGE_SIZE + 1, TG_KMT_PAGE_SIZE, 0, &va) == TG_KMT_INVALID_PARAMETER);
   CHECK(tg_kmt_reserve(&producer, TG_KMT_PAGE_SIZE, 3 * TG_KMT_PAGE_SIZE, 0, &va) == TG_KMT_INVALID_PARAMETER);
   CHECK(tg_kmt_reserve(&producer, TG_KMT_PAGE_SIZE, 65536, TG_KMT_VA_START + 1, &va) == TG_KMT_INVALID_PARAMETER);
   CHECK(reserves == 3 && frees == 3);

   CHECK(tg_kmt_allocate(&producer, 4097, true, &original) == 0);
   CHECK(original.size == 8192 && !original.resource && producer.allocated == 8192);
   CHECK(tg_kmt_export_memory(&original, &handle) == TG_KMT_INVALID_PARAMETER && !handle);
   CHECK(tg_kmt_free(&producer, &original) == 0 && !producer.allocated && !original.handle);
   CHECK(tg_kmt_allocate_shared(&producer, UINT64_MAX, true, &original) == TG_KMT_NO_VIDEO_MEMORY);
   create_status = TG_KMT_NO_MEMORY;
   CHECK(tg_kmt_allocate_shared(&producer, 4097, true, &original) == create_status);
   CHECK(!producer.allocated && !original.handle && last_section &&
         !GetHandleInformation(last_section, &flags));
   create_status = 0;
   CHECK(tg_kmt_allocate_shared(&producer, 4097, true, &original) == 0);
   CHECK(original.resource && original.flags == TG_GPU_ALLOCATION_CPU_VISIBLE &&
         !GetHandleInformation(last_section, &flags));

   DWORD session;
   CHECK(ProcessIdToSessionId(GetCurrentProcessId(), &session));
   if (session)
      _snwprintf(expected_name, ARRAYSIZE(expected_name),
                  L"\\Sessions\\%lu\\BaseNamedObjects\\nvk-storage-test", session);
   else
      wcscpy(expected_name, L"\\BaseNamedObjects\\nvk-storage-test");
   CHECK(tg_kmt_export_memory_win32(&original, NULL, expected_access,
                                   L"Local\\nvk-storage-test", &handle) == 0);
   HANDLE named;
   CHECK(tg_kmt_open_memory_name(L"nvk-storage-test", &named) == 0 && named != handle);
   CHECK(GetHandleInformation(named, &flags) && !(flags & HANDLE_FLAG_INHERIT));
   CHECK(CloseHandle(named) && CloseHandle(handle));
   wcscpy(expected_name, L"\\BaseNamedObjects\\nvk-storage-test");
   SECURITY_DESCRIPTOR descriptor = {.Revision = SECURITY_DESCRIPTOR_REVISION};
   SECURITY_ATTRIBUTES security = {sizeof(security), &descriptor, TRUE};
   expected_access = STANDARD_RIGHTS_READ;
   expected_security = &descriptor;
   expected_inherit = true;
   CHECK(tg_kmt_export_memory_win32(&original, &security, expected_access,
                                   L"Global\\nvk-storage-test", &handle) == 0);
   CHECK(GetHandleInformation(handle, &flags) && (flags & HANDLE_FLAG_INHERIT));
   CHECK(CloseHandle(handle));
   CHECK(tg_kmt_open_memory_name(L"Global\\", &named) == TG_KMT_INVALID_PARAMETER && !named);
   CHECK(tg_kmt_open_memory_name(L"bad\\name", &named) == TG_KMT_INVALID_PARAMETER && !named);
   *expected_name = 0;
   expected_access = STANDARD_RIGHTS_READ | SHARED_ALLOCATION_WRITE;
   expected_security = NULL;
   expected_inherit = false;
   CHECK(tg_kmt_export_memory(&original, &handle) == 0 && GetHandleInformation(handle, &flags));
   CHECK(tg_kmt_import_memory(&consumer, handle, &imported) == 0);
   CHECK(tg_kmt_import_memory(&consumer, handle, &second) == 0);
   CHECK(imported.handle != original.handle && imported.handle != second.handle &&
         imported.resource != original.resource && imported.resource != second.resource &&
         imported.size == original.size && imported.flags == original.flags &&
         consumer.allocated == 16384 && producer.allocated == 8192);

   /* The NT handle preserves the payload after the exporter releases its
    * local resource. Each subsequent import has independent ownership. */
   CHECK(tg_kmt_free(&producer, &original) == 0 && !producer.allocated);
   CHECK(GetHandleInformation(handle, &flags));
   CHECK(tg_kmt_free(&consumer, &second) == 0 && consumer.allocated == 8192);
   CHECK(tg_kmt_import_memory(&consumer, handle, &second) == 0);
   CHECK(tg_kmt_free(&consumer, &second) == 0);

   const UINT before_opens = opens, before_destroys = destroys;
   bad_query = true;
   CHECK(tg_kmt_import_memory(&consumer, handle, &second) == TG_KMT_INVALID_PARAMETER &&
         !second.handle && opens == before_opens && destroys == before_destroys);
   bad_query = false;
   bad_descriptor = true;
   CHECK(tg_kmt_import_memory(&consumer, handle, &second) == TG_KMT_INVALID_PARAMETER &&
         !second.handle && destroys == before_destroys + 1 && consumer.allocated == 8192);
   bad_descriptor = false;
   consumer.budget = consumer.allocated;
   CHECK(tg_kmt_import_memory(&consumer, handle, &second) == TG_KMT_NO_VIDEO_MEMORY &&
         !second.handle && destroys == before_destroys + 2 && consumer.allocated == 8192);
   consumer.budget = 65536;
   open_status = TG_KMT_NO_MEMORY;
   CHECK(tg_kmt_import_memory(&consumer, handle, &second) == open_status &&
         !second.handle && consumer.allocated == 8192);
   open_status = 0;
   resident_status = TG_KMT_NO_VIDEO_MEMORY;
   CHECK(tg_kmt_import_memory(&consumer, handle, &second) == resident_status &&
         !second.handle && destroys == before_destroys + 3 && consumer.allocated == 8192);
   destroy_status = TG_KMT_DEVICE_REMOVED;
   CHECK(tg_kmt_import_memory(&consumer, handle, &second) == resident_status &&
         second.resource && second.size == 8192 && consumer.allocated == 16384);
   resident_status = destroy_status = 0;
   CHECK(tg_kmt_free(&consumer, &second) == 0 && consumer.allocated == 8192);
   CHECK(GetHandleInformation(handle, &flags) && CloseHandle(handle));
   CHECK(tg_kmt_free(&consumer, &imported) == 0 && !consumer.allocated);
   CHECK(residents >= 6 && destroys == next_allocation);
   for (UINT i = 1; i <= next_allocation; i++)
      CHECK(!allocations[i].alive);
   const char pass[] = "PASS: Win32 section errors and budget rollback, SDK shared-resource handles, named objects, security/access/inheritance, repeated imports and failed-residency cleanup (controlled KMT)\r\n";
   DWORD written;
   CHECK(WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), pass, sizeof(pass) - 1, &written, NULL));
   return 0;
}
