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
#include "vk_internal_exts.h"
#include "nvk_entrypoints.h"
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
static unsigned removal_wait_mode;
static HANDLE retained_export;

static struct {
   DWORD thread;
   unsigned next, context_refs, submits, waits, signals, cpu_waits;
   bool fail_submit;
   struct { unsigned refs; uint64_t value; } fences[16];
} rt;

static void runtime_call(void *state)
{
   CHECK(state == &rt && GetCurrentThreadId() == rt.thread);
}

static VkResult runtime_create_context(void *state, uint32_t types, void **context)
{
   runtime_call(state); CHECK(types == (WSI_WIN32_RUNTIME_3D | WSI_WIN32_RUNTIME_COPY));
   CHECK(!rt.context_refs); ++rt.context_refs; *context = &rt.context_refs; return VK_SUCCESS;
}

static VkResult runtime_destroy_context(void *state, void *context)
{
   runtime_call(state); CHECK(context == &rt.context_refs && rt.context_refs == 1);
   --rt.context_refs; return VK_SUCCESS;
}

static VkResult runtime_create_fence(void *state, uint64_t initial, struct wsi_win32_runtime_fence *out)
{
   runtime_call(state); unsigned id = ++rt.next; CHECK(id < ARRAY_SIZE(rt.fences));
   rt.fences[id].refs = 1; rt.fences[id].value = initial;
   *out = (struct wsi_win32_runtime_fence){&rt.fences[id], 500 + id, &rt.fences[id].value};
   return VK_SUCCESS;
}

static VkResult runtime_retain(void *state, void *token, struct wsi_win32_runtime_fence *out)
{
   runtime_call(state);
   for (unsigned i = 1; i <= rt.next; ++i) {
      if (token != &rt.fences[i] || !rt.fences[i].refs) continue;
      ++rt.fences[i].refs;
      *out = (struct wsi_win32_runtime_fence){token, 500 + i, &rt.fences[i].value};
      return VK_SUCCESS;
   }
   return VK_ERROR_INVALID_EXTERNAL_HANDLE;
}

static void runtime_release(void *state, void *token)
{
   runtime_call(state);
   for (unsigned i = 1; i <= rt.next; ++i) {
      if (token != &rt.fences[i]) continue;
      CHECK(rt.fences[i].refs); --rt.fences[i].refs; return;
   }
   CHECK(false);
}

static VkResult runtime_gpu_sync(void *state, void *context, bool signal, uint32_t count,
                                const uint32_t *handles, const uint64_t *points)
{
   runtime_call(state); CHECK(context == &rt.context_refs && rt.context_refs && count == 1);
   CHECK(handles[0] > 500 && handles[0] <= 500 + rt.next && rt.fences[handles[0] - 500].refs);
   CHECK(points[0] == (signal ? 11 : 7));
   if (signal) ++rt.signals; else ++rt.waits;
   return VK_SUCCESS; /* Enqueued GPU work is deliberately not completed. */
}

static VkResult runtime_wait_cpu(void *state, void *token, uint64_t value, uint32_t timeout)
{
   runtime_call(state); CHECK(token == &rt.fences[1] && value == 11 && timeout && timeout <= 50);
   ++rt.cpu_waits; return VK_TIMEOUT;
}

static VkResult runtime_signal_cpu(void *state, void *token, uint64_t value)
{
   runtime_call(state); CHECK(false); return VK_ERROR_DEVICE_LOST;
}

static const uint32_t runtime_words[] = {0xfeed, 0x12345678, 0x00998877, 0x42, 0x10};
static VkResult runtime_submit(void *state, void *context, uint64_t address, const void *words,
                              uint32_t bytes, uint32_t count, const uint32_t *written)
{
   runtime_call(state); CHECK(context == &rt.context_refs && address == 0x100004 && bytes == 16);
   CHECK(!memcmp(words, runtime_words + 1, bytes) && count == 2 && written[0] == 60 && written[1] == 61);
   ++rt.submits; return rt.fail_submit ? VK_ERROR_DEVICE_LOST : VK_SUCCESS;
}

static VkResult runtime_map(struct nvkmd_mem *mem, struct vk_object_base *log,
                           enum nvkmd_mem_map_flags flags, void *fixed, void **out)
{
   CHECK(!fixed && flags == NVKMD_MEM_MAP_RDWR); *out = (void *)runtime_words; return VK_SUCCESS;
}
static void runtime_unmap(struct nvkmd_mem *mem, enum nvkmd_mem_map_flags flags, void *map)
{
   CHECK(map == runtime_words);
}

static void runtime_contract(struct nvk_device *nvk, struct nvkmd_windows_dev *kmd)
{
   struct nvkmd_pdev pdev = {0};
   kmd->base.pdev = &pdev;
   rt.thread = GetCurrentThreadId();
   kmd->runtime = (struct wsi_win32_runtime){WSI_WIN32_RUNTIME_VERSION, &rt, &rt,
      runtime_create_context, runtime_destroy_context, runtime_submit, runtime_gpu_sync,
      runtime_create_fence, runtime_retain, runtime_release, runtime_wait_cpu, runtime_signal_cpu};
   VkDevice device = vk_device_to_handle(&nvk->vk);
   struct wsi_win32_runtime_fence owner;
   CHECK(runtime_create_fence(&rt, 3, &owner) == VK_SUCCESS);
   VkSemaphoreTypeCreateInfo type = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
      .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE, .initialValue = 5};
   VkSemaphoreCreateInfo info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &type};
   VkSemaphore sem;
   CHECK(vk_common_CreateSemaphore(device, &info, NULL, &sem) == VK_SUCCESS);
   struct wsi_win32_runtime_fence_import imported = {
      .sType = VK_STRUCTURE_TYPE_WSI_WIN32_RUNTIME_FENCE_IMPORT_MESA,
      .version = 1, .device_cookie = &rt, .fence_token = owner.token};
   VkImportSemaphoreWin32HandleInfoKHR import = {
      .sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR,
      .pNext = &imported, .semaphore = sem, .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT};
   struct vk_sync *sync = &vk_semaphore_from_handle(sem)->permanent;
   for (unsigned bad = 0; bad < 3; ++bad) {
      import.pNext = bad == 0 ? NULL : &imported;
      imported.device_cookie = bad == 1 ? (void *)kmd : (void *)&rt;
      imported.fence_token = bad == 2 ? kmd : owner.token;
      CHECK(nvk_ImportSemaphoreWin32HandleKHR(device, &import) == VK_ERROR_INVALID_EXTERNAL_HANDLE);
      uint64_t value; CHECK(vk_sync_get_value(&nvk->vk, sync, &value) == VK_SUCCESS && value == 5);
   }
   imported.fence_token = owner.token;
   CHECK(nvk_ImportSemaphoreWin32HandleKHR(device, &import) == VK_SUCCESS);
   CHECK(rt.fences[1].refs == 2 && !rt.fences[2].refs);
   runtime_release(&rt, owner.token); CHECK(rt.fences[1].refs == 1);
   struct nvkmd_ctx *ctx;
   CHECK(nvkmd_windows_create_ctx(&kmd->base, &nvk->vk.base,
         NVKMD_ENGINE_3D | NVKMD_ENGINE_COPY, &ctx) == VK_SUCCESS);
   struct vk_sync_wait wait = {.sync = sync, .wait_value = 7};
   struct vk_sync_signal signal = {.sync = sync, .signal_value = 11};
   CHECK(nvkmd_ctx_wait(ctx, &nvk->vk.base, 1, &wait) == VK_SUCCESS);
   CHECK(nvkmd_ctx_signal(ctx, &nvk->vk.base, 1, &signal) == VK_SUCCESS);
   CHECK(rt.waits == 1 && rt.signals == 1 && !rt.cpu_waits && rt.fences[1].value == 3);
   CHECK(vk_sync_wait(&nvk->vk, sync, 11, VK_SYNC_WAIT_COMPLETE,
         os_time_get_absolute_timeout(50000000)) == VK_TIMEOUT && rt.cpu_waits == 1);
   rt.fences[1].value = UINT64_MAX;
   uint64_t value; CHECK(vk_sync_get_value(&nvk->vk, sync, &value) == VK_ERROR_DEVICE_LOST);
   rt.fences[1].value = 3;

   simple_mtx_init(&kmd->base.mems_mutex, mtx_plain); list_inithead(&kmd->base.mems);
   const struct nvkmd_mem_ops ops = {.map = runtime_map, .unmap = runtime_unmap};
   struct nvkmd_mem mem;
   nvkmd_mem_init(&kmd->base, &mem, &ops, NVKMD_MEM_CAN_MAP, sizeof(runtime_words), 4);
   struct nvkmd_va va = {.addr = 0x100000, .size_B = sizeof(runtime_words)}; mem.va = &va;
   list_addtail(&mem.link, &kmd->base.mems);
   const uint32_t written[] = {60, 61};
   struct nvkmd_ctx_exec exec = {.addr = 0x100004, .size_B = 16, .written_count = 2,
      .written_allocations = written};
   CHECK(nvkmd_ctx_exec(ctx, &nvk->vk.base, 1, &exec) == VK_SUCCESS);
   rt.fail_submit = true;
   CHECK(nvkmd_ctx_exec(ctx, &nvk->vk.base, 1, &exec) == VK_ERROR_DEVICE_LOST);
   CHECK(rt.submits == 2 && mem.refcnt == 1 && !mem.map_cnt);
   list_del(&mem.link); simple_mtx_destroy(&mem.map_mutex); simple_mtx_destroy(&kmd->base.mems_mutex);
   nvkmd_ctx_destroy(ctx); vk_common_DestroySemaphore(device, sem, NULL);
   CHECK(!rt.context_refs && !live && !contexts && gpu_waits == 1 && gpu_signals == 1);
   for (unsigned i = 1; i <= rt.next; ++i) CHECK(!rt.fences[i].refs);
   puts("PASS: actual NVK runtime transport, caller thread, retained same-device fences, real payload/write-list forwarding, no allocator KMT fallback, failure/timeout propagation (CPU callbacks; no GPU)");
}

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
   if (removal_wait_mode) {
      *payloads[handles[0]] = removal_wait_mode == 1 ? UINT64_MAX : points[0];
      return 0; /* A successful OS wait can accompany the removal sentinel. */
   }
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
   CHECK(nvk_ImportSemaphoreWin32HandleKHR(device, &import) == VK_ERROR_INVALID_EXTERNAL_HANDLE);
   uint64_t value;
   CHECK(vk_sync_get_value(&nvk->vk, sync_b, &value) == VK_SUCCESS && value == 33);
   /* A private Vulkan timeline keeps its full value domain. */
   *payloads[next_id] = UINT64_MAX;
   CHECK(vk_sync_get_value(&nvk->vk, sync_b, &value) == VK_SUCCESS && value == UINT64_MAX);
   CHECK(vk_sync_wait(&nvk->vk, sync_b, UINT64_MAX, VK_SYNC_WAIT_COMPLETE, 0) == VK_SUCCESS);
   *payloads[next_id] = 33;
   fail_import = false;
   CHECK(nvk_ImportSemaphoreWin32HandleKHR(device, &import) == VK_SUCCESS);
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
   /* Direct KMT imports must reject removal just like runtime-owned imports.
    * No CPU signal is used to manufacture completion in the production path. */
   *exported_value = UINT64_MAX;
   CHECK(vk_sync_get_value(&nvk->vk, sync_b, &value) == VK_ERROR_DEVICE_LOST);
   CHECK(vk_sync_wait(&nvk->vk, sync_b, 99, VK_SYNC_WAIT_COMPLETE,
         os_time_get_absolute_timeout(50000000)) == VK_ERROR_DEVICE_LOST && !waits);
   *exported_value = 61; removal_wait_mode = 1;
   CHECK(vk_sync_wait(&nvk->vk, sync_b, 99, VK_SYNC_WAIT_COMPLETE,
         os_time_get_absolute_timeout(50000000)) == VK_ERROR_DEVICE_LOST && waits == 1);
   *exported_value = 61; removal_wait_mode = 2;
   CHECK(vk_sync_wait(&nvk->vk, sync_b, 99, VK_SYNC_WAIT_COMPLETE,
         os_time_get_absolute_timeout(50000000)) == VK_SUCCESS && waits == 2);
   CHECK(*exported_value == 99);
   *exported_value = 61; removal_wait_mode = 0; waits = 0;
   CHECK(vk_sync_wait(&nvk->vk, sync_b, 99, VK_SYNC_WAIT_COMPLETE,
                      os_time_get_absolute_timeout(50000000)) == VK_TIMEOUT);
   CHECK(waits == 1);
   get.semaphore = b;
   CHECK(vk_common_GetSemaphoreWin32HandleKHR(device, &get, &first) == VK_SUCCESS);
   CHECK(GetHandleInformation(first, &flags) && !(flags & HANDLE_FLAG_INHERIT));
   /* Successful payload replacement changes the sentinel contract; failed
    * replacement must preserve the old contract and payload. */
   VkImportSemaphoreWin32HandleInfoKHR replacement = import;
   replacement.handle = first;
   replacement.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
   CHECK(nvk_ImportSemaphoreWin32HandleKHR(device, &replacement) == VK_SUCCESS);
   *exported_value = UINT64_MAX;
   CHECK(vk_sync_get_value(&nvk->vk, sync_b, &value) == VK_SUCCESS && value == UINT64_MAX);
   replacement.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
   fail_import = true;
   CHECK(nvk_ImportSemaphoreWin32HandleKHR(device, &replacement) == VK_ERROR_INVALID_EXTERNAL_HANDLE);
   CHECK(vk_sync_get_value(&nvk->vk, sync_b, &value) == VK_SUCCESS && value == UINT64_MAX);
   fail_import = false;
   CHECK(nvk_ImportSemaphoreWin32HandleKHR(device, &replacement) == VK_SUCCESS);
   CHECK(vk_sync_get_value(&nvk->vk, sync_b, &value) == VK_ERROR_DEVICE_LOST);
   *exported_value = 61;
   vk_common_DestroySemaphore(device, b, NULL);
   CHECK(GetHandleInformation(first, &flags) && CloseHandle(first) && !live);
   runtime_contract(nvk, &kmd);
   vk_object_base_finish(&nvk->vk.base);
   vk_object_base_finish(&physical.base);
   free(nvk);
   puts("PASS: actual Vulkan/NVK Win32 timeline capabilities, imports/exports, bidirectional payloads, queue wait/signal, failure preservation and blocking timeout; KMT mocked, no GPU execution");
   return 0;
}
