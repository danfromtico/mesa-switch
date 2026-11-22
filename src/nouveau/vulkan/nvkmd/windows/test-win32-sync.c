/* SPDX-License-Identifier: MIT */
/* Actual Vulkan semaphore entry points and NVK sync provider with KMT spies.
 * No physical device is opened; handle duplication uses the Windows API. */
#include "nvkmd_windows.h"
#include "exec_transport.h"
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
   bool enabled;
   bool concurrent;
   bool fail_next_submit;
   volatile LONG live_contexts;
   volatile LONG next_context;
   volatile LONG wait_calls;
   volatile LONG signal_calls;
   volatile LONG linear_submits;
   volatile LONG indirect_submits;
   uint32_t last_sync_count;
   uint32_t last_sync_handles[WSI_WIN32_RUNTIME_MAX_SYNCS];
   uint64_t last_sync_points[WSI_WIN32_RUNTIME_MAX_SYNCS];
   uint32_t submit_count;
   uint64_t submit_addrs[8];
   uint32_t submit_bytes[8];
   uint32_t submit_written_count[8];
   uint32_t submit_written[8][16];
   bool submit_is_indirect[8];
   uint32_t submit_words[8][16];
} kmt_batch;

static struct {
   DWORD thread;
   unsigned next, context_refs, submits, waits, signals, cpu_waits;
   bool fail_submit;
   bool batch_mode;
   uint32_t last_sync_count;
   uint64_t last_sync_points[WSI_WIN32_RUNTIME_MAX_SYNCS];
   uint64_t last_submit_addr;
   uint32_t last_submit_bytes, last_submit_written_count;
   uint32_t last_submit_written[16];
   uint32_t last_submit_words[8];
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
   if (rt.batch_mode) {
      runtime_call(state);
      CHECK(context == &rt.context_refs && rt.context_refs && count > 0 && count <= WSI_WIN32_RUNTIME_MAX_SYNCS);
      for (uint32_t i = 0; i < count; i++)
         CHECK(handles[i] > 500 && handles[i] <= 500 + rt.next && rt.fences[handles[i] - 500].refs);
      rt.last_sync_count = count;
      memcpy(rt.last_sync_points, points, count * sizeof(*points));
      if (signal) ++rt.signals; else ++rt.waits;
      return VK_SUCCESS;
   }
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
   if (rt.batch_mode) {
      runtime_call(state);
      CHECK(context == &rt.context_refs && address && bytes && !(bytes & 3u) && bytes <= TG_GPU_SUBMIT_MAX_BYTES);
      CHECK(count <= 16u && (!count || written));
      rt.last_submit_addr = address;
      rt.last_submit_bytes = bytes;
      rt.last_submit_written_count = count;
      if (count)
         memcpy(rt.last_submit_written, written, count * sizeof(*written));
      memcpy(rt.last_submit_words, words, MIN2(bytes, (uint32_t)sizeof(rt.last_submit_words)));
      ++rt.submits;
      return rt.fail_submit ? VK_ERROR_DEVICE_LOST : VK_SUCCESS;
   }
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
   rt.batch_mode = true; rt.fail_submit = false;
   const struct vk_sync_wait rt_waits3[3] = {
      {.sync = sync, .wait_value = 21}, {.sync = sync, .wait_value = 22}, {.sync = sync, .wait_value = 23},
   };
   CHECK(nvkmd_ctx_wait(ctx, &nvk->vk.base, 3, rt_waits3) == VK_SUCCESS);
   CHECK(rt.waits == 2 && rt.last_sync_count == 3 && rt.last_sync_points[0] == 21 && rt.last_sync_points[2] == 23);
   struct vk_sync_signal rt_sigs[WSI_WIN32_RUNTIME_MAX_SYNCS + 2];
   for (uint32_t i = 0; i < ARRAY_SIZE(rt_sigs); i++)
      rt_sigs[i] = (struct vk_sync_signal){.sync = sync, .signal_value = 30u + i};
   CHECK(nvkmd_ctx_signal(ctx, &nvk->vk.base, ARRAY_SIZE(rt_sigs), rt_sigs) == VK_SUCCESS);
   CHECK(rt.signals == 3 && rt.last_sync_count == 2 && rt.last_sync_points[0] == 30u + WSI_WIN32_RUNTIME_MAX_SYNCS);
   const uint32_t written2[] = {61, 62};
   const struct nvkmd_ctx_exec rt_execs[2] = {
      {.addr = 0x100004, .size_B = 8, .written_count = 2, .written_allocations = written},
      {.addr = 0x10000c, .size_B = 8, .written_count = 2, .written_allocations = written2},
   };
   CHECK(nvkmd_ctx_exec(ctx, &nvk->vk.base, 2, rt_execs) == VK_SUCCESS);
   CHECK(rt.submits == 3 && rt.last_submit_addr == 0x100004 && rt.last_submit_bytes == 16 &&
         rt.last_submit_written_count == 3 && rt.last_submit_written[0] == 60 &&
         rt.last_submit_written[1] == 61 && rt.last_submit_written[2] == 62 &&
         !memcmp(rt.last_submit_words, runtime_words + 1, 16) && mem.refcnt == 1 && !mem.map_cnt);
   rt.fail_submit = true;
   CHECK(nvkmd_ctx_exec(ctx, &nvk->vk.base, 2, rt_execs) == VK_ERROR_DEVICE_LOST);
   CHECK(rt.submits == 4 && mem.refcnt == 1 && !mem.map_cnt);
   rt.fail_submit = false; rt.batch_mode = false;
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
   if (kmt_batch.enabled) {
      CHECK(dev->handle == 7);
      *context = 100u + (uint32_t)InterlockedIncrement(&kmt_batch.next_context);
      InterlockedIncrement(&kmt_batch.live_contexts);
      return 0;
   }
   CHECK(dev->handle == 7 && !contexts);
   *context = 17;
   contexts++;
   return 0;
}

LONG
tg_kmt_destroy_context(uint32_t context)
{
   if (kmt_batch.enabled) {
      CHECK(context > 100u && InterlockedDecrement(&kmt_batch.live_contexts) >= 0);
      return 0;
   }
   CHECK(context == 17 && contexts == 1);
   contexts--;
   return 0;
}

LONG
tg_kmt_wait_gpu(uint32_t context, uint32_t count, const uint32_t *handles,
                 const uint64_t *points)
{
   if (kmt_batch.enabled) {
      CHECK(context > 100u && count > 0 && count <= WSI_WIN32_RUNTIME_MAX_SYNCS);
      InterlockedIncrement(&kmt_batch.wait_calls);
      if (!kmt_batch.concurrent) {
         kmt_batch.last_sync_count = count;
         memcpy(kmt_batch.last_sync_handles, handles, count * sizeof(*handles));
         memcpy(kmt_batch.last_sync_points, points, count * sizeof(*points));
      }
      return 0;
   }
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
   if (kmt_batch.enabled) {
      CHECK(context > 100u && count > 0 && count <= WSI_WIN32_RUNTIME_MAX_SYNCS);
      InterlockedIncrement(&kmt_batch.signal_calls);
      if (!kmt_batch.concurrent) {
         kmt_batch.last_sync_count = count;
         memcpy(kmt_batch.last_sync_handles, handles, count * sizeof(*handles));
         memcpy(kmt_batch.last_sync_points, points, count * sizeof(*points));
      }
      return 0;
   }
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
   if (kmt_batch.enabled) {
      CHECK(context > 100u && address && bytes && !(bytes & 3u) && bytes <= TG_GPU_SUBMIT_MAX_BYTES);
      if (count > 16u || (count && !allocations))
         return TG_KMT_INVALID_PARAMETER;
      for (uint32_t i = 0; i < count; i++) {
         if (!allocations[i])
            return TG_KMT_INVALID_PARAMETER;
         for (uint32_t j = 0; j < i; j++)
            if (allocations[i] == allocations[j])
               return TG_KMT_INVALID_PARAMETER;
      }
      InterlockedIncrement(&kmt_batch.linear_submits);
      if (!kmt_batch.concurrent && kmt_batch.submit_count < ARRAY_SIZE(kmt_batch.submit_addrs)) {
         uint32_t idx = kmt_batch.submit_count++;
         kmt_batch.submit_addrs[idx] = address;
         kmt_batch.submit_bytes[idx] = bytes;
         kmt_batch.submit_written_count[idx] = count;
         if (count)
            memcpy(kmt_batch.submit_written[idx], allocations, count * sizeof(*allocations));
         kmt_batch.submit_is_indirect[idx] = false;
         memcpy(kmt_batch.submit_words[idx], words, MIN2(bytes, (uint32_t)sizeof(kmt_batch.submit_words[idx])));
      }
      if (kmt_batch.fail_next_submit) {
         kmt_batch.fail_next_submit = false;
         return TG_KMT_DEVICE_REMOVED;
      }
      return 0;
   }
   CHECK(!count); (void)allocations;
   CHECK(false); /* A synchronization control must never execute GPU commands. */
   return TG_KMT_INVALID_PARAMETER;
}

LONG
tg_kmt_submit_indirect(uint32_t context, uint64_t address, const void *words,
                       uint32_t bytes, const struct tg_gpu_submit_indirect *indirect,
                       uint32_t count, const uint32_t *allocations)
{
   if (kmt_batch.enabled) {
      CHECK(context > 100u && address && words && bytes && indirect && indirect->address && indirect->bytes);
      InterlockedIncrement(&kmt_batch.indirect_submits);
      if (!kmt_batch.concurrent && kmt_batch.submit_count < ARRAY_SIZE(kmt_batch.submit_addrs)) {
         uint32_t idx = kmt_batch.submit_count++;
         kmt_batch.submit_addrs[idx] = address;
         kmt_batch.submit_bytes[idx] = bytes;
         kmt_batch.submit_is_indirect[idx] = true;
      }
      return 0;
   }
   (void)context;
   (void)address;
   (void)words;
   (void)bytes;
   (void)indirect;
   (void)count;
   (void)allocations;
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

struct batch_mem_backing {
   struct nvkmd_mem mem;
   struct nvkmd_va va;
   uint32_t *words;
};

static VkResult
batch_mem_map(struct nvkmd_mem *mem, struct vk_object_base *log,
              enum nvkmd_mem_map_flags flags, void *fixed, void **out)
{
   (void)log; (void)fixed; (void)flags;
   struct batch_mem_backing *b = container_of(mem, struct batch_mem_backing, mem);
   *out = b->words;
   return VK_SUCCESS;
}

static void
batch_mem_unmap(struct nvkmd_mem *mem, enum nvkmd_mem_map_flags flags, void *map)
{
   (void)flags;
   struct batch_mem_backing *b = container_of(mem, struct batch_mem_backing, mem);
   CHECK(map == b->words);
}

struct concurrent_worker_args {
   struct nvk_device *nvk;
   struct nvkmd_ctx *ctx;
   struct vk_sync *syncs[2];
   uint64_t addr0;
   uint64_t addr1;
};

static DWORD WINAPI
concurrent_worker(LPVOID param)
{
   struct concurrent_worker_args *a = param;
   const uint32_t written[] = {71, 72};
   for (unsigned iter = 0; iter < 32; iter++) {
      const struct vk_sync_wait w[2] = {
         {.sync = a->syncs[0], .wait_value = iter + 1},
         {.sync = a->syncs[1], .wait_value = iter + 2},
      };
      const struct vk_sync_signal s[2] = {
         {.sync = a->syncs[0], .signal_value = iter + 3},
         {.sync = a->syncs[1], .signal_value = iter + 4},
      };
      const struct nvkmd_ctx_exec e[2] = {
         {.addr = a->addr0, .size_B = 16, .written_count = 2, .written_allocations = written},
         {.addr = a->addr1, .size_B = 16, .written_count = 2, .written_allocations = written},
      };
      CHECK(nvkmd_ctx_wait(a->ctx, &a->nvk->vk.base, 2, w) == VK_SUCCESS);
      CHECK(nvkmd_ctx_exec(a->ctx, &a->nvk->vk.base, 2, e) == VK_SUCCESS);
      CHECK(nvkmd_ctx_signal(a->ctx, &a->nvk->vk.base, 2, s) == VK_SUCCESS);
   }
   return 0;
}

static void
kmt_batch_and_concurrency_tests(struct nvk_device *nvk, struct nvkmd_windows_dev *kmd)
{
   memset(&kmt_batch, 0, sizeof(kmt_batch));
   kmt_batch.enabled = true;
   simple_mtx_init(&kmd->base.mems_mutex, mtx_plain);
   list_inithead(&kmd->base.mems);

   const struct nvkmd_mem_ops ops = {.map = batch_mem_map, .unmap = batch_mem_unmap};
   uint32_t *buf0 = calloc(TG_GPU_SUBMIT_MAX_BYTES / 4u, sizeof(uint32_t));
   uint32_t *buf1 = calloc(TG_GPU_SUBMIT_MAX_BYTES / 4u, sizeof(uint32_t));
   uint32_t *ind_buf = calloc(64, sizeof(uint32_t));
   CHECK(buf0 && buf1 && ind_buf);
   for (uint32_t i = 0; i < 16; i++) {
      buf0[i] = 0x1000u + i;
      buf1[i] = 0x2000u + i;
   }
   /* Valid DrawIndirect prefix at buf0[16..20] */
   buf0[16] = 0x20000000u | (1u << 16) | (NVB06F_WFI >> 2);
   buf0[17] = NVB06F_WFI_SCOPE_ALL;
   buf0[18] = 0xa0000000u | (12u << 16) | (NVB197_CALL_MME_MACRO(NVK_MME_DRAW_INDIRECT) >> 2);
   buf0[19] = 2;
   buf0[20] = 1;

   struct batch_mem_backing b0 = {.words = buf0}, b1 = {.words = buf1}, bi = {.words = ind_buf};
   nvkmd_mem_init(&kmd->base, &b0.mem, &ops, NVKMD_MEM_CAN_MAP, TG_GPU_SUBMIT_MAX_BYTES, 4);
   b0.va = (struct nvkmd_va){.addr = 0x200000, .size_B = TG_GPU_SUBMIT_MAX_BYTES};
   b0.mem.va = &b0.va;
   list_addtail(&b0.mem.link, &kmd->base.mems);

   nvkmd_mem_init(&kmd->base, &b1.mem, &ops, NVKMD_MEM_CAN_MAP, TG_GPU_SUBMIT_MAX_BYTES, 4);
   b1.va = (struct nvkmd_va){.addr = 0x300000, .size_B = TG_GPU_SUBMIT_MAX_BYTES};
   b1.mem.va = &b1.va;
   list_addtail(&b1.mem.link, &kmd->base.mems);

   nvkmd_mem_init(&kmd->base, &bi.mem, &ops, NVKMD_MEM_CAN_MAP, 256, 4);
   bi.va = (struct nvkmd_va){.addr = 0x400000, .size_B = 256};
   bi.mem.va = &bi.va;
   list_addtail(&bi.mem.link, &kmd->base.mems);

   struct nvkmd_ctx *ctx0 = NULL, *ctx1 = NULL;
   CHECK(nvkmd_windows_create_ctx(&kmd->base, &nvk->vk.base, NVKMD_ENGINE_3D, &ctx0) == VK_SUCCESS);
   CHECK(nvkmd_windows_create_ctx(&kmd->base, &nvk->vk.base, NVKMD_ENGINE_3D, &ctx1) == VK_SUCCESS);

   /* 1. Batched GPU wait/signal (3 fences -> 1 call; chunking; pre-validation). */
   VkDevice device = vk_device_to_handle(&nvk->vk);
   VkSemaphoreTypeCreateInfo st = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
      .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE, .initialValue = 0};
   VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &st};
   VkSemaphore sems[3];
   struct vk_sync *syncs[3];
   for (unsigned i = 0; i < 3; i++) {
      CHECK(vk_common_CreateSemaphore(device, &si, NULL, &sems[i]) == VK_SUCCESS);
      syncs[i] = &vk_semaphore_from_handle(sems[i])->permanent;
   }
   const struct vk_sync_wait waits3[3] = {
      {.sync = syncs[0], .wait_value = 10},
      {.sync = syncs[1], .wait_value = 20},
      {.sync = syncs[2], .wait_value = 30},
   };
   const struct vk_sync_signal sigs3[3] = {
      {.sync = syncs[0], .signal_value = 11},
      {.sync = syncs[1], .signal_value = 21},
      {.sync = syncs[2], .signal_value = 31},
   };
   CHECK(nvkmd_ctx_wait(ctx0, &nvk->vk.base, 3, waits3) == VK_SUCCESS);
   CHECK(kmt_batch.wait_calls == 1 && kmt_batch.last_sync_count == 3 &&
         kmt_batch.last_sync_points[0] == 10 && kmt_batch.last_sync_points[2] == 30);
   CHECK(nvkmd_ctx_signal(ctx0, &nvk->vk.base, 3, sigs3) == VK_SUCCESS);
   CHECK(kmt_batch.signal_calls == 1 && kmt_batch.last_sync_count == 3 &&
         kmt_batch.last_sync_points[0] == 11 && kmt_batch.last_sync_points[2] == 31);

   /* Pre-validation failure must not emit partial GPU sync calls. */
   struct vk_sync_type bogus_type = {0};
   struct vk_sync bogus_sync = {.type = &bogus_type};
   const struct vk_sync_wait bad_waits[2] = {
      {.sync = syncs[0], .wait_value = 40},
      {.sync = &bogus_sync, .wait_value = 41},
   };
   CHECK(nvkmd_ctx_wait(ctx0, &nvk->vk.base, 2, bad_waits) == VK_ERROR_FEATURE_NOT_PRESENT);
   CHECK(kmt_batch.wait_calls == 1);

   /* Chunking above WSI_WIN32_RUNTIME_MAX_SYNCS preserves order. */
   struct vk_sync_wait chunk_waits[WSI_WIN32_RUNTIME_MAX_SYNCS + 2];
   for (uint32_t i = 0; i < ARRAY_SIZE(chunk_waits); i++)
      chunk_waits[i] = (struct vk_sync_wait){.sync = syncs[i % 3], .wait_value = 100u + i};
   CHECK(nvkmd_ctx_wait(ctx0, &nvk->vk.base, ARRAY_SIZE(chunk_waits), chunk_waits) == VK_SUCCESS);
   CHECK(kmt_batch.wait_calls == 3 && kmt_batch.last_sync_count == 2 &&
         kmt_batch.last_sync_points[0] == 100u + WSI_WIN32_RUNTIME_MAX_SYNCS &&
         kmt_batch.last_sync_points[1] == 101u + WSI_WIN32_RUNTIME_MAX_SYNCS);

   /* 2. Contiguous linear coalescing; discontiguous BOs keep their actual command VA. */
   const uint32_t w0[] = {10, 11}, w1[] = {11, 12};
   const struct nvkmd_ctx_exec coalesced[3] = {
      {.addr = 0x200000, .size_B = 8, .written_count = 2, .written_allocations = w0},
      {.addr = 0x200008, .size_B = 8, .written_count = 2, .written_allocations = w0},
      {.addr = 0x300004, .size_B = 8, .written_count = 2, .written_allocations = w1},
   };
   CHECK(nvkmd_ctx_exec(ctx0, &nvk->vk.base, 3, coalesced) == VK_SUCCESS);
   CHECK(kmt_batch.submit_count == 2 && kmt_batch.submit_bytes[0] == 16 &&
         kmt_batch.submit_addrs[0] == 0x200000 && kmt_batch.submit_written_count[0] == 2 &&
         kmt_batch.submit_written[0][0] == 10 && kmt_batch.submit_written[0][1] == 11);
   CHECK(kmt_batch.submit_bytes[1] == 8 && kmt_batch.submit_addrs[1] == 0x300004 &&
         kmt_batch.submit_written_count[1] == 2 && kmt_batch.submit_written[1][0] == 11 &&
         kmt_batch.submit_written[1][1] == 12);
   CHECK(kmt_batch.submit_words[0][0] == 0x1000u && kmt_batch.submit_words[0][3] == 0x1003u &&
         kmt_batch.submit_words[1][0] == 0x2001u && kmt_batch.submit_words[1][1] == 0x2002u);
   CHECK(b0.mem.refcnt == 1 && !b0.mem.map_cnt && b1.mem.refcnt == 1 && !b1.mem.map_cnt);

   /* 3. 16-written cap split + 64 KiB split + indirect boundary ordering. */
   kmt_batch.submit_count = 0;
   const uint32_t w_cap0[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
   const uint32_t w_cap1[8] = {11, 12, 13, 14, 15, 16, 17, 18};
   const struct nvkmd_ctx_exec written_split[2] = {
      {.addr = 0x200000, .size_B = 8, .written_count = 10, .written_allocations = w_cap0},
      {.addr = 0x200008, .size_B = 8, .written_count = 8, .written_allocations = w_cap1},
   };
   CHECK(nvkmd_ctx_exec(ctx0, &nvk->vk.base, 2, written_split) == VK_SUCCESS);
   CHECK(kmt_batch.submit_count == 2 && kmt_batch.submit_written_count[0] == 10 &&
         kmt_batch.submit_written_count[1] == 8);

   kmt_batch.submit_count = 0;
   const struct nvkmd_ctx_exec split_and_ind[5] = {
      {.addr = 0x200000, .size_B = TG_GPU_SUBMIT_MAX_BYTES - 8},
      {.addr = 0x300000, .size_B = 16},
      {.addr = 0x200040, .size_B = 20, .incomplete = true},
      {.addr = 0x400000, .size_B = 40, .no_prefetch = true},
      {.addr = 0x300010, .size_B = 8},
   };
   CHECK(nvkmd_ctx_exec(ctx0, &nvk->vk.base, 5, split_and_ind) == VK_SUCCESS);
   CHECK(kmt_batch.submit_count == 4 && !kmt_batch.submit_is_indirect[0] &&
         !kmt_batch.submit_is_indirect[1] && kmt_batch.submit_is_indirect[2] &&
         !kmt_batch.submit_is_indirect[3]);

   /* 4. Malformed duplicate written primary is preserved as error (not masked). */
   const uint32_t bad_written[] = {55, 55};
   const struct nvkmd_ctx_exec malformed = {
      .addr = 0x200000, .size_B = 8, .written_count = 2, .written_allocations = bad_written,
   };
   CHECK(nvkmd_ctx_exec(ctx0, &nvk->vk.base, 1, &malformed) == VK_ERROR_DEVICE_LOST);

   /* 5. Submit failure & retry + unmapped second slice leave zero ref/map leaks. */
   kmt_batch.fail_next_submit = true;
   CHECK(nvkmd_ctx_exec(ctx0, &nvk->vk.base, 3, coalesced) == VK_ERROR_DEVICE_LOST);
   CHECK(b0.mem.refcnt == 1 && !b0.mem.map_cnt && b1.mem.refcnt == 1 && !b1.mem.map_cnt);
   CHECK(nvkmd_ctx_exec(ctx0, &nvk->vk.base, 3, coalesced) == VK_SUCCESS);
   kmt_batch.submit_count = 0;
   const struct nvkmd_ctx_exec bad_second[2] = {
      {.addr = 0x200000, .size_B = 8},
      {.addr = 0x900000, .size_B = 8},
   };
   CHECK(nvkmd_ctx_exec(ctx0, &nvk->vk.base, 2, bad_second) == VK_ERROR_DEVICE_LOST);
   CHECK(kmt_batch.submit_count == 1 && b0.mem.refcnt == 1 && !b0.mem.map_cnt &&
         b1.mem.refcnt == 1 && !b1.mem.map_cnt);

   /* 6. Concurrent execution on two KMT contexts sharing device mems_mutex. */
   kmt_batch.concurrent = true;
   struct concurrent_worker_args a0 = {nvk, ctx0, {syncs[0], syncs[1]}, 0x200000, 0x300000};
   struct concurrent_worker_args a1 = {nvk, ctx1, {syncs[1], syncs[2]}, 0x200010, 0x300010};
   HANDLE threads[2] = {
      CreateThread(NULL, 0, concurrent_worker, &a0, 0, NULL),
      CreateThread(NULL, 0, concurrent_worker, &a1, 0, NULL),
   };
   CHECK(threads[0] && threads[1]);
   CHECK(WaitForMultipleObjects(2, threads, TRUE, 30000) == WAIT_OBJECT_0);
   CHECK(CloseHandle(threads[0]) && CloseHandle(threads[1]));
   CHECK(b0.mem.refcnt == 1 && !b0.mem.map_cnt && b1.mem.refcnt == 1 && !b1.mem.map_cnt);

   nvkmd_ctx_destroy(ctx0);
   nvkmd_ctx_destroy(ctx1);
   CHECK(!kmt_batch.live_contexts);
   for (unsigned i = 0; i < 3; i++)
      vk_common_DestroySemaphore(device, sems[i], NULL);
   list_del(&bi.mem.link); simple_mtx_destroy(&bi.mem.map_mutex);
   list_del(&b1.mem.link); simple_mtx_destroy(&b1.mem.map_mutex);
   list_del(&b0.mem.link); simple_mtx_destroy(&b0.mem.map_mutex);
   simple_mtx_destroy(&kmd->base.mems_mutex);
   free(ind_buf); free(buf1); free(buf0);
   kmt_batch.enabled = false;
}

int main(void)
{
   struct nvk_device *nvk = calloc(1, sizeof(*nvk));
   struct nvkmd_pdev pdev = {0};
   struct nvkmd_windows_dev kmd = {.base.pdev = &pdev, .kmt.handle = 7};
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
   kmt_batch_and_concurrency_tests(nvk, &kmd);
   runtime_contract(nvk, &kmd);
   vk_object_base_finish(&nvk->vk.base);
   vk_object_base_finish(&physical.base);
   free(nvk);
   puts("PASS: actual Vulkan/NVK Win32 timeline capabilities, imports/exports, bidirectional payloads, queue wait/signal, failure preservation and blocking timeout; KMT mocked, no GPU execution");
   return 0;
}
