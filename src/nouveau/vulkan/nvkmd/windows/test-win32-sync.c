/* SPDX-License-Identifier: MIT */
/* Actual Vulkan semaphore entry points and NVK sync provider with KMT spies.
 * No physical device is opened; handle duplication uses the Windows API. */
#include "nvkmd_windows.h"
#include "nvk_device.h"
#include "vk_alloc.h"
#include "vk_common_entrypoints.h"
#include "vk_physical_device.h"
#include "vk_semaphore.h"
#include "vk_sync_binary.h"
#include "util/os_time.h"
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static uint64_t values[32];
static uint64_t *payloads[32];
static unsigned next_id, live, shared_creates, exports, waits;
static unsigned contexts, gpu_waits, gpu_signals;
static uint64_t *exported_value;
static bool fail_import;
static HANDLE retained_export;

VkResult
nvkmd_windows_status(struct vk_object_base *log, const char *operation, LONG status, VkResult failure)
{
   return status < 0 ? failure : VK_SUCCESS;
}

LONG
tg_kmt_create_fence(const struct tg_kmt_device *dev, uint64_t initial, struct tg_kmt_fence *fence)
{
   CHECK(dev->handle == 7 && next_id + 1 < ARRAY_SIZE(values));
   fence->handle = ++next_id;
   values[next_id] = initial;
   payloads[next_id] = &values[next_id];
   fence->value = &values[next_id];
   live++;
   return 0;
}

LONG
tg_kmt_create_shared_fence(const struct tg_kmt_device *dev, uint64_t initial, struct tg_kmt_fence *fence)
{
   shared_creates++;
   return tg_kmt_create_fence(dev, initial, fence);
}

LONG
tg_kmt_destroy_fence(struct tg_kmt_fence *fence)
{
   CHECK(fence->handle && live);
   payloads[fence->handle] = NULL;
   live--;
   *fence = (struct tg_kmt_fence){0};
   return 0;
}

LONG
tg_kmt_export_fence_win32(const struct tg_kmt_fence *fence, const SECURITY_ATTRIBUTES *security,
                          DWORD access, LPCWSTR name, HANDLE *handle)
{
   CHECK(access == GENERIC_ALL);
   if (name) CHECK(!wcscmp(name, L"Local\\nvk-sync-control"));
   *handle = CreateEventW((SECURITY_ATTRIBUTES *)security, FALSE, FALSE, NULL);
   CHECK(*handle);
   retained_export = *handle;
   exported_value = payloads[fence->handle];
   exports++;
   return 0;
}

LONG
tg_kmt_import_fence(const struct tg_kmt_device *dev, HANDLE handle,
                     LPCWSTR name, struct tg_kmt_fence *fence)
{
   if (fail_import) return TG_KMT_INVALID_PARAMETER;
   DWORD flags;
   CHECK(GetHandleInformation(handle, &flags) && !name);
   CHECK(tg_kmt_create_fence(dev, 0, fence) == 0);
   payloads[fence->handle] = exported_value;
   fence->value = exported_value;
   return 0;
}

LONG
tg_kmt_signal_cpu(const struct tg_kmt_device *dev, uint32_t fence, uint64_t value)
{
   CHECK(dev->handle == 7 && fence && fence <= next_id);
   CHECK(payloads[fence]);
   *payloads[fence] = value;
   return 0;
}

LONG
tg_kmt_create_context(const struct tg_kmt_device *dev, uint32_t *context)
{
   CHECK(dev->handle == 7 && !contexts);
   *context = 17;
   contexts++;
   return 0;
}

LONG
tg_kmt_destroy_context(uint32_t context)
{
   CHECK(context == 17 && contexts == 1);
   contexts--;
   return 0;
}

LONG
tg_kmt_wait_gpu(uint32_t context, uint32_t count, const uint32_t *handles,
                 const uint64_t *points)
{
   CHECK(context == 17 && contexts == 1 && count == 1 && points[0] == 47);
   CHECK(handles[0] <= next_id && payloads[handles[0]] == exported_value);
   CHECK(*payloads[handles[0]] == 47);
   gpu_waits++;
   return 0;
}

LONG
tg_kmt_signal_gpu(uint32_t context, uint32_t count, const uint32_t *handles,
                   const uint64_t *points)
{
   CHECK(context == 17 && contexts == 1 && count == 1 && points[0] == 51);
   CHECK(handles[0] <= next_id && payloads[handles[0]] == exported_value);
   *payloads[handles[0]] = points[0];
   gpu_signals++;
   return 0;
}

LONG
tg_kmt_submit_primaries(uint32_t context, uint64_t address, const void *words,
                       uint32_t bytes, uint32_t count, const uint32_t *allocations)
{
   CHECK(!count); (void)allocations;
   CHECK(false); /* A synchronization control must never execute GPU commands. */
   return TG_KMT_INVALID_PARAMETER;
}

LONG
tg_kmt_wait_fences(const struct tg_kmt_device *dev, uint32_t count,
                   const uint32_t *handles, const uint64_t *points, bool any, DWORD timeout)
{
   CHECK(dev->handle == 7 && count == 1 && !any && timeout && timeout != INFINITE);
   CHECK(handles[0] && points[0] == 99);
   waits++;
   Sleep(timeout);
   return TG_KMT_TIMEOUT;
}

int main(void)
{
   struct nvk_device *nvk = calloc(1, sizeof(*nvk));
   struct nvkmd_windows_dev kmd = {.kmt.handle = 7};
   struct vk_sync_binary_type binary = vk_sync_binary_get_type(&nvkmd_windows_sync_type);
   const struct vk_sync_type *types[] = {&binary.sync, &nvkmd_windows_sync_type, NULL};
   struct vk_physical_device physical = {.supported_sync_types = types};
   CHECK(nvk);
   vk_object_base_instance_init(NULL, &physical.base, VK_OBJECT_TYPE_PHYSICAL_DEVICE);
   vk_object_base_init(&nvk->vk, &nvk->vk.base, VK_OBJECT_TYPE_DEVICE);
   nvk->vk.physical = &physical;
   nvk->vk.alloc = *vk_default_allocator();
   nvk->vk.timeline_mode = VK_DEVICE_TIMELINE_MODE_NATIVE;
   nvk->nvkmd = &kmd.base;
   VkDevice device = vk_device_to_handle(&nvk->vk);
   const VkExternalSemaphoreHandleTypeFlags handle_types =
      VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT |
      VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
   VkSemaphoreTypeCreateInfo type = {
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
      .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE, .initialValue = 13,
   };
   VkPhysicalDeviceExternalSemaphoreInfo query = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO,
      .pNext = &type, .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT,
   };
   VkExternalSemaphoreProperties props = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
   vk_common_GetPhysicalDeviceExternalSemaphoreProperties(vk_physical_device_to_handle(&physical), &query, &props);
   CHECK(props.externalSemaphoreFeatures == (VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT |
                                            VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT));
   CHECK(props.compatibleHandleTypes == handle_types && props.exportFromImportedHandleTypes == handle_types);
   query.pNext = NULL;
   vk_common_GetPhysicalDeviceExternalSemaphoreProperties(vk_physical_device_to_handle(&physical), &query, &props);
   CHECK(!props.externalSemaphoreFeatures && !props.compatibleHandleTypes);

   SECURITY_ATTRIBUTES security = {sizeof(security), NULL, TRUE};
   const VkExportSemaphoreWin32HandleInfoKHR win32 = {
      .sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR,
      .pAttributes = &security, .dwAccess = GENERIC_ALL, .name = L"Local\\nvk-sync-control",
   };
   const VkExportSemaphoreCreateInfo export = {
      .sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO, .pNext = &win32, .handleTypes = handle_types,
   };
   type.pNext = &export;
   const VkSemaphoreCreateInfo info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &type};
   VkSemaphore a, b;
   CHECK(vk_common_CreateSemaphore(device, &info, NULL, &a) == VK_SUCCESS);
   CHECK(shared_creates == 1 && exports == 1);
   struct vk_sync *sync_a = &vk_semaphore_from_handle(a)->permanent;
   CHECK(sync_a->flags & VK_SYNC_IS_SHARED);
   VkSemaphoreGetWin32HandleInfoKHR get = {
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR,
      .semaphore = a, .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT,
   };
   HANDLE first, second;
   CHECK(vk_common_GetSemaphoreWin32HandleKHR(device, &get, &first) == VK_SUCCESS);
   get.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
   CHECK(vk_common_GetSemaphoreWin32HandleKHR(device, &get, &second) == VK_SUCCESS);
   DWORD flags;
   CHECK(first != second && first != retained_export && exports == 1);
   CHECK(GetHandleInformation(first, &flags) && (flags & HANDLE_FLAG_INHERIT));
   CHECK(CloseHandle(first) && GetHandleInformation(second, &flags));

   /* An ordinary semaphore starts with a private payload before import. */
   type.pNext = NULL;
   type.initialValue = 33;
   CHECK(vk_common_CreateSemaphore(device, &info, NULL, &b) == VK_SUCCESS);
   CHECK(shared_creates == 1 && exports == 1);
   struct vk_sync *sync_b = &vk_semaphore_from_handle(b)->permanent;
   const VkImportSemaphoreWin32HandleInfoKHR import = {
      .sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR,
      .semaphore = b, .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT, .handle = second,
   };
   fail_import = true;
   CHECK(vk_common_ImportSemaphoreWin32HandleKHR(device, &import) == VK_ERROR_INVALID_EXTERNAL_HANDLE);
   uint64_t value;
   CHECK(vk_sync_get_value(&nvk->vk, sync_b, &value) == VK_SUCCESS && value == 33);
   fail_import = false;
   CHECK(vk_common_ImportSemaphoreWin32HandleKHR(device, &import) == VK_SUCCESS);
   CHECK(GetHandleInformation(second, &flags) && CloseHandle(second));
   CHECK(vk_sync_signal(&nvk->vk, sync_a, 47) == VK_SUCCESS);
   CHECK(vk_sync_get_value(&nvk->vk, sync_b, &value) == VK_SUCCESS && value == 47);
   /* Exercise the actual NVK queue callbacks with the imported payload. These
    * must enqueue KMT GPU operations instead of falling back to a CPU wait. */
   struct nvkmd_ctx *ctx = NULL;
   CHECK(nvkmd_windows_create_ctx(&kmd.base, &nvk->vk.base, NVKMD_ENGINE_3D, &ctx) == VK_SUCCESS);
   const struct vk_sync_wait wait = {.sync = sync_b, .wait_value = 47};
   const struct vk_sync_signal signal = {.sync = sync_b, .signal_value = 51};
   CHECK(nvkmd_ctx_wait(ctx, &nvk->vk.base, 1, &wait) == VK_SUCCESS);
   CHECK(nvkmd_ctx_signal(ctx, &nvk->vk.base, 1, &signal) == VK_SUCCESS);
   CHECK(gpu_waits == 1 && gpu_signals == 1 && waits == 0);
   CHECK(vk_sync_get_value(&nvk->vk, sync_a, &value) == VK_SUCCESS && value == 51);
   nvkmd_ctx_destroy(ctx);
   CHECK(!contexts);
   CHECK(vk_sync_signal(&nvk->vk, sync_b, 61) == VK_SUCCESS);
   CHECK(vk_sync_get_value(&nvk->vk, sync_a, &value) == VK_SUCCESS && value == 61);
   vk_common_DestroySemaphore(device, a, NULL);
   CHECK(!GetHandleInformation(retained_export, &flags));
   CHECK(vk_sync_get_value(&nvk->vk, sync_b, &value) == VK_SUCCESS && value == 61);
   CHECK(vk_sync_wait(&nvk->vk, sync_b, 99, VK_SYNC_WAIT_COMPLETE,
                      os_time_get_absolute_timeout(50000000)) == VK_TIMEOUT);
   CHECK(waits == 1);
   get.semaphore = b;
   CHECK(vk_common_GetSemaphoreWin32HandleKHR(device, &get, &first) == VK_SUCCESS);
   CHECK(GetHandleInformation(first, &flags) && !(flags & HANDLE_FLAG_INHERIT));
   vk_common_DestroySemaphore(device, b, NULL);
   CHECK(GetHandleInformation(first, &flags) && CloseHandle(first) && !live);
   vk_object_base_finish(&nvk->vk.base);
   vk_object_base_finish(&physical.base);
   free(nvk);
   puts("PASS: actual Vulkan/NVK Win32 timeline capabilities, imports/exports, bidirectional payloads, queue wait/signal, failure preservation and blocking timeout; KMT mocked, no GPU execution");
   return 0;
}
