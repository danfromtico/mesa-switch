/* SPDX-License-Identifier: MIT */
/* Actual SDK bridge, controlled KMT calls and real NT handle ownership.
 * No adapter is opened and no GPU work is executed. */
#include "kmt_native.h"
#include <d3dkmthk.h>
#include <winternl.h>
#include <stdio.h>
#include <wchar.h>

#define CHECK(x) do { if (!(x)) { \
   fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); ExitProcess(1); \
} } while (0)

static uint64_t payload;
static unsigned next_object, live_objects, creates, opens, destroys;
static bool shared_create, fail_open, malformed_open;
static bool expected_inherit;
static PVOID expected_security;
static DWORD expected_access = GENERIC_ALL;
static WCHAR expected_name[160];
static HANDLE exported, named_copy;

static void
check_attributes(const OBJECT_ATTRIBUTES *attrs)
{
   CHECK(attrs->Length == sizeof(*attrs) && !attrs->RootDirectory);
   CHECK(attrs->SecurityDescriptor == expected_security);
   CHECK(attrs->Attributes == (expected_inherit ? OBJ_INHERIT : 0));
   if (*expected_name) {
      CHECK(attrs->ObjectName && !wcscmp(attrs->ObjectName->Buffer, expected_name));
      CHECK(attrs->ObjectName->Length == wcslen(expected_name) * sizeof(WCHAR));
   } else {
      CHECK(!attrs->ObjectName);
   }
}

NTSTATUS APIENTRY
D3DKMTCreateSynchronizationObject2(D3DKMT_CREATESYNCHRONIZATIONOBJECT2 *args)
{
   creates++;
   CHECK(args->hDevice == 1 && args->Info.Type == D3DDDI_MONITORED_FENCE);
   CHECK(args->Info.Flags.Value == (shared_create ? 3u : 0u));
   CHECK(args->Info.MonitoredFence.EngineAffinity == 1);
   payload = args->Info.MonitoredFence.InitialFenceValue;
   args->hSyncObject = ++next_object;
   args->Info.MonitoredFence.FenceValueCPUVirtualAddress = &payload;
   live_objects++;
   return 0;
}

NTSTATUS APIENTRY
D3DKMTDestroySynchronizationObject(const D3DKMT_DESTROYSYNCHRONIZATIONOBJECT *args)
{
   CHECK(args->hSyncObject && args->hSyncObject <= next_object && live_objects);
   live_objects--;
   destroys++;
   return 0;
}

NTSTATUS APIENTRY
D3DKMTShareObjects(UINT count, const D3DKMT_HANDLE *objects,
                   POBJECT_ATTRIBUTES attributes, DWORD access, HANDLE *handle)
{
   CHECK(count == 1 && *objects && *objects <= next_object);
   CHECK(access == expected_access);
   check_attributes(attributes);
   SECURITY_ATTRIBUTES security = {sizeof(security), NULL, expected_inherit};
   *handle = CreateEventW(&security, FALSE, FALSE, NULL);
   CHECK(*handle);
   exported = *handle;
   return 0;
}

NTSTATUS APIENTRY
D3DKMTOpenSyncObjectNtHandleFromName(D3DKMT_OPENSYNCOBJECTNTHANDLEFROMNAME *args)
{
   CHECK(args->dwDesiredAccess == GENERIC_ALL);
   check_attributes(args->pObjAttrib);
   CHECK(DuplicateHandle(GetCurrentProcess(), exported, GetCurrentProcess(),
                         &args->hNtHandle, 0, FALSE, DUPLICATE_SAME_ACCESS));
   named_copy = args->hNtHandle;
   return 0;
}

NTSTATUS APIENTRY
D3DKMTOpenSyncObjectFromNtHandle2(D3DKMT_OPENSYNCOBJECTFROMNTHANDLE2 *args)
{
   opens++;
   DWORD flags;
   CHECK(GetHandleInformation(args->hNtHandle, &flags));
   CHECK(args->hDevice == 2 && !args->Flags.Value);
   CHECK(args->MonitoredFence.EngineAffinity == 1);
   if (fail_open)
      return TG_KMT_INVALID_PARAMETER;
   args->hSyncObject = ++next_object;
   args->MonitoredFence.FenceValueCPUVirtualAddress = malformed_open ? NULL : &payload;
   live_objects++;
   return 0;
}

NTSTATUS APIENTRY
D3DKMTSignalSynchronizationObjectFromCpu(const D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMCPU *args)
{
   CHECK(args->hDevice == 1 && args->ObjectCount == 1);
   CHECK(args->ObjectHandleArray[0] && args->ObjectHandleArray[0] <= next_object);
   payload = args->FenceValueArray[0];
   return 0;
}

NTSTATUS APIENTRY
D3DKMTWaitForSynchronizationObjectFromGpu(const D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMGPU *args)
{
   CHECK(args->hContext == 21 && args->ObjectCount == 1);
   CHECK(args->ObjectHandleArray[0] && args->ObjectHandleArray[0] <= next_object);
   CHECK(args->MonitoredFenceValueArray[0] == 23);
   return 0;
}

NTSTATUS APIENTRY
D3DKMTSignalSynchronizationObjectFromGpu(const D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMGPU *args)
{
   CHECK(args->hContext == 11 && args->ObjectCount == 1);
   CHECK(args->ObjectHandleArray[0] && args->ObjectHandleArray[0] <= next_object);
   CHECK(args->MonitoredFenceValueArray[0] == 23);
   return 0;
}

int main(void)
{
   const struct tg_kmt_device producer = {.handle = 1}, consumer = {.handle = 2};
   struct tg_kmt_fence a, b;
   CHECK(tg_kmt_create_fence(&producer, 7, &a) == 0 && *a.value == 7);
   CHECK(tg_kmt_destroy_fence(&a) == 0 && !a.handle && !a.value);
   shared_create = true;
   CHECK(tg_kmt_create_shared_fence(&producer, 13, &a) == 0 && *a.value == 13);
   CHECK(tg_kmt_export_fence_win32(&a, NULL, GENERIC_ALL, NULL, &exported) == 0);
   DWORD flags;
   CHECK(GetHandleInformation(exported, &flags) && !(flags & HANDLE_FLAG_INHERIT));
   CHECK(tg_kmt_import_fence(&consumer, exported, NULL, &b) == 0 && *b.value == 13);
   CHECK(GetHandleInformation(exported, &flags));
   CHECK(CloseHandle(exported));
   CHECK(tg_kmt_signal_cpu(&producer, a.handle, 17) == 0 && *b.value == 17);
   const uint64_t value = 23;
   CHECK(tg_kmt_wait_gpu(21, 1, &b.handle, &value) == 0);
   CHECK(tg_kmt_signal_gpu(11, 1, &a.handle, &value) == 0);
   CHECK(tg_kmt_destroy_fence(&b) == 0);

   DWORD session;
   CHECK(ProcessIdToSessionId(GetCurrentProcessId(), &session));
   if (session)
      _snwprintf(expected_name, ARRAYSIZE(expected_name), L"\\Sessions\\%lu\\BaseNamedObjects\\nvk-fence", session);
   else
      _snwprintf(expected_name, ARRAYSIZE(expected_name), L"\\BaseNamedObjects\\nvk-fence");
   SECURITY_DESCRIPTOR descriptor;
   CHECK(InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION));
   SECURITY_ATTRIBUTES security = {sizeof(security), &descriptor, TRUE};
   expected_inherit = true;
   expected_security = &descriptor;
   expected_access = GENERIC_READ | GENERIC_WRITE;
   CHECK(tg_kmt_export_fence_win32(&a, &security, expected_access, L"Local\\nvk-fence", &exported) == 0);
   CHECK(GetHandleInformation(exported, &flags) && (flags & HANDLE_FLAG_INHERIT));
   expected_inherit = false;
   expected_security = NULL;
   CHECK(tg_kmt_import_fence(&consumer, NULL, L"nvk-fence", &b) == 0);
   CHECK(!GetHandleInformation(named_copy, &flags));
   CHECK(GetHandleInformation(exported, &flags));
   CHECK(tg_kmt_destroy_fence(&b) == 0);

   fail_open = true;
   CHECK(tg_kmt_import_fence(&consumer, NULL, L"Local\\nvk-fence", &b) < 0);
   CHECK(!b.handle && !b.value && !GetHandleInformation(named_copy, &flags));
   fail_open = false;
   malformed_open = true;
   unsigned before = destroys;
   CHECK(tg_kmt_import_fence(&consumer, exported, NULL, &b) < 0);
   CHECK(!b.handle && !b.value && destroys == before + 1);
   malformed_open = false;
   before = opens;
   CHECK(tg_kmt_import_fence(&consumer, exported, L"bad", &b) < 0);
   CHECK(tg_kmt_import_fence(&consumer, NULL, NULL, &b) < 0);
   CHECK(tg_kmt_import_fence(&consumer, NULL, L"Local\\bad\\name", &b) < 0);
   CHECK(opens == before && GetHandleInformation(exported, &flags));
   CHECK(CloseHandle(exported));
   CHECK(tg_kmt_destroy_fence(&a) == 0 && !live_objects && creates == 2);
   puts("PASS: SDK monitored-fence sharing, names, security, ownership and queue-call ABI; KMT calls controlled, no GPU execution");
   return 0;
}
