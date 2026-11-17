/* SPDX-License-Identifier: MIT */
#include "nvkmd_windows.h"
#include "nvk_device.h"
#include "submit_abi.h"
#include "util/os_time.h"
#include "util/u_atomic.h"
#include "util/u_math.h"
#include "vk_log.h"
#include "vk_sync_binary.h"
#include "vk_semaphore.h"
#include "vk_common_entrypoints.h"
#include "vk_internal_exts.h"
#include "nvk_entrypoints.h"

struct windows_sync {
   struct vk_sync base;
   struct tg_kmt_fence fence;
   HANDLE export_handle;
   bool export_inherit;
   bool d3d12_fence; /* UINT64_MAX is removal only for this external contract. */
   struct wsi_win32_runtime_fence runtime_fence;
};

struct windows_ctx {
   struct nvkmd_ctx base;
   uint32_t context;
   struct tg_kmt_fence idle;
   uint64_t idle_value;
   void *runtime_context;
   struct wsi_win32_runtime_fence runtime_idle;
};

static const struct wsi_win32_runtime *
ctx_runtime(struct nvkmd_dev *dev)
{
   const struct wsi_win32_runtime *runtime = &nvkmd_windows_dev(dev)->runtime;
   return runtime->version ? runtime : NULL;
}

static const struct wsi_win32_runtime *
sync_runtime(struct vk_device *device)
{
   struct nvk_device *nvk = container_of(device, struct nvk_device, vk);
   return ctx_runtime(nvk->nvkmd);
}

static struct tg_kmt_device *
sync_device(struct vk_device *device)
{
   struct nvk_device *nvk = container_of(device, struct nvk_device, vk);
   return &nvkmd_windows_dev(nvk->nvkmd)->kmt;
}

static struct tg_kmt_fence *
sync_fence(struct vk_sync *sync)
{
   assert(sync->type == &nvkmd_windows_sync_type);
   return &container_of(sync, struct windows_sync, base)->fence;
}

static VkResult
win_sync_init(struct vk_device *device, struct vk_sync *sync, uint64_t initial)
{
   const struct wsi_win32_runtime *runtime = sync_runtime(device);
   if (runtime) {
      struct windows_sync *win = container_of(sync, struct windows_sync, base);
      VkResult result = runtime->create_fence(runtime->context, initial, &win->runtime_fence);
      if (result == VK_SUCCESS)
         win->fence = (struct tg_kmt_fence){win->runtime_fence.handle, win->runtime_fence.value};
      return result;
   }
   LONG status = sync->flags & VK_SYNC_IS_SHAREABLE ?
      tg_kmt_create_shared_fence(sync_device(device), initial, sync_fence(sync)) :
      tg_kmt_create_fence(sync_device(device), initial, sync_fence(sync));
   return nvkmd_windows_status(&device->base, "create monitored fence", status,
                               VK_ERROR_OUT_OF_DEVICE_MEMORY);
}

static void
win_sync_finish(struct vk_device *device, struct vk_sync *sync)
{
   (void)device;
   struct windows_sync *win = container_of(sync, struct windows_sync, base);
   if (win->export_handle) {
      CloseHandle(win->export_handle);
      win->export_handle = NULL;
   }
   win->d3d12_fence = false;
   const struct wsi_win32_runtime *runtime = sync_runtime(device);
   if (win->runtime_fence.token) {
      runtime->release_fence(runtime->context, win->runtime_fence.token);
      win->runtime_fence = (struct wsi_win32_runtime_fence){0};
      win->fence = (struct tg_kmt_fence){0};
   } else {
      tg_kmt_destroy_fence(sync_fence(sync));
   }
}

static VkResult
win_sync_import(struct vk_device *device, struct vk_sync *sync,
                 void *handle, const wchar_t *name)
{
   if (sync_runtime(device))
      return vk_error(device, VK_ERROR_INVALID_EXTERNAL_HANDLE);
   struct tg_kmt_fence imported;
   LONG status = tg_kmt_import_fence(sync_device(device), handle, name, &imported);
   if (status < 0)
      return nvkmd_windows_status(&device->base, "import monitored fence", status,
                                  VK_ERROR_INVALID_EXTERNAL_HANDLE);
   /* A failed import must not discard the existing payload. The new KMT
    * reference survives the application's CloseHandle after import. */
   win_sync_finish(device, sync);
   *sync_fence(sync) = imported;
   return VK_SUCCESS;
}

static VkResult
win_sync_export(struct vk_device *device, struct vk_sync *sync, void **handle)
{
   struct windows_sync *win = container_of(sync, struct windows_sync, base);
   *handle = NULL;
   if (win->export_handle) {
      if (!DuplicateHandle(GetCurrentProcess(), win->export_handle,
                           GetCurrentProcess(), handle, 0, win->export_inherit,
                           DUPLICATE_SAME_ACCESS))
         return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
      return VK_SUCCESS;
   }
   LONG status = tg_kmt_export_fence_win32(&win->fence, NULL, GENERIC_ALL,
                                           NULL, handle);
   return nvkmd_windows_status(&device->base, "export monitored fence", status,
                               VK_ERROR_OUT_OF_HOST_MEMORY);
}

static VkResult
win_sync_export_params(struct vk_device *device, struct vk_sync *sync,
                        const void *security_attributes, uint32_t access,
                        const wchar_t *name)
{
   struct windows_sync *win = container_of(sync, struct windows_sync, base);
   const SECURITY_ATTRIBUTES *security = security_attributes;
   /* Materialize the named/security-qualified handle during creation. Do not
    * retain caller-owned SECURITY_DESCRIPTOR or UTF-16 pointers. */
   HANDLE handle;
   LONG status = tg_kmt_export_fence_win32(&win->fence, security, access, name, &handle);
   if (status < 0)
      return nvkmd_windows_status(&device->base, "prepare monitored fence export", status,
                                  VK_ERROR_OUT_OF_HOST_MEMORY);
   win->export_handle = handle;
   win->export_inherit = security && security->bInheritHandle;
   return VK_SUCCESS;
}

static VkResult
win_sync_signal(struct vk_device *device, struct vk_sync *sync, uint64_t value)
{
   const struct wsi_win32_runtime *runtime = sync_runtime(device);
   if (runtime) {
      struct windows_sync *win = container_of(sync, struct windows_sync, base);
      return runtime->signal_cpu(runtime->context, win->runtime_fence.token, value);
   }
   LONG status = tg_kmt_signal_cpu(sync_device(device), sync_fence(sync)->handle, value);
   return nvkmd_windows_status(&device->base, "CPU signal monitored fence", status,
                               VK_ERROR_DEVICE_LOST);
}

static VkResult
win_sync_value(struct vk_device *device, struct vk_sync *sync, uint64_t *value)
{
   (void)device;
   *value = p_atomic_read(sync_fence(sync)->value);
   const struct windows_sync *win = container_of(sync, struct windows_sync, base);
   if ((sync_runtime(device) || win->d3d12_fence) && *value == UINT64_MAX)
      return VK_ERROR_DEVICE_LOST;
   return VK_SUCCESS;
}

static VkResult
win_sync_wait(struct vk_device *device, struct vk_sync *sync, uint64_t value,
              enum vk_sync_wait_flags flags, uint64_t deadline)
{
   assert(!(flags & VK_SYNC_WAIT_PENDING));
   struct tg_kmt_fence *fence = sync_fence(sync);
   const struct wsi_win32_runtime *runtime = sync_runtime(device);
   const struct windows_sync *win = container_of(sync, struct windows_sync, base);
   const bool removal_sentinel = runtime || win->d3d12_fence;
   /* D3D12 fence imports (including tgd3d's runtime fences) use all ones
    * for device removal. Ordinary Vulkan timelines retain the full uint64
    * value domain; a maximum value alone must not remove those devices. */
   if (removal_sentinel && p_atomic_read(fence->value) == UINT64_MAX)
      return VK_ERROR_DEVICE_LOST;
   if (runtime) {
      DWORD timeout = INFINITE;
      if (deadline != OS_TIMEOUT_INFINITE) {
         uint64_t now = os_time_get_nano();
         uint64_t remaining = deadline > now ? deadline - now : 0;
         timeout = MIN2(remaining / 1000000 + (remaining % 1000000 != 0), INFINITE - 1);
      }
      struct windows_sync *win = container_of(sync, struct windows_sync, base);
      VkResult result = runtime->wait_cpu(runtime->context, win->runtime_fence.token, value, timeout);
      if (result == VK_SUCCESS && p_atomic_read(fence->value) == UINT64_MAX)
         return VK_ERROR_DEVICE_LOST;
      return result;
   }
   for (;;) {
      const uint64_t completed = p_atomic_read(fence->value);
      if (removal_sentinel && completed == UINT64_MAX)
         return VK_ERROR_DEVICE_LOST;
      if (completed >= value)
         return VK_SUCCESS;
      DWORD timeout = INFINITE;
      if (deadline != OS_TIMEOUT_INFINITE) {
         uint64_t now = os_time_get_nano();
         if (now >= deadline)
            return VK_TIMEOUT;
         uint64_t remaining = deadline - now;
         timeout = MIN2(remaining / 1000000 + (remaining % 1000000 != 0),
                        INFINITE - 1);
      }
      LONG status = tg_kmt_wait_fences(sync_device(device), 1, &fence->handle,
                                       &value, false, timeout);
      if (status < 0)
         return nvkmd_windows_status(&device->base, "CPU wait monitored fence", status,
                                     VK_ERROR_DEVICE_LOST);
   }
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_ImportSemaphoreWin32HandleKHR(VkDevice device, const VkImportSemaphoreWin32HandleInfoKHR *info)
{
   VK_FROM_HANDLE(nvk_device, dev, device);
   const struct wsi_win32_runtime *runtime = sync_runtime(&dev->vk);
   if (!runtime) {
      VkResult result = vk_common_ImportSemaphoreWin32HandleKHR(device, info);
      if (result == VK_SUCCESS) {
         VK_FROM_HANDLE(vk_semaphore, semaphore, info->semaphore);
         struct vk_sync *sync = vk_semaphore_get_active_sync(semaphore);
         if (sync->type == &nvkmd_windows_sync_type) {
            struct windows_sync *win = container_of(sync, struct windows_sync, base);
            win->d3d12_fence = info->handleType == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
         }
      }
      return result;
   }
   const struct wsi_win32_runtime_fence_import *import =
      vk_find_struct_const(info->pNext, WSI_WIN32_RUNTIME_FENCE_IMPORT_MESA);
   VK_FROM_HANDLE(vk_semaphore, semaphore, info->semaphore);
   if (!import || import->version != 1 || import->device_cookie != runtime->device_cookie ||
       !import->fence_token || info->flags || info->name ||
       info->handleType != VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT ||
       !semaphore || semaphore->base.device != &dev->vk ||
       semaphore->type != VK_SEMAPHORE_TYPE_TIMELINE || semaphore->temporary ||
       semaphore->permanent.type != &nvkmd_windows_sync_type)
      return vk_error(dev, VK_ERROR_INVALID_EXTERNAL_HANDLE);
   struct wsi_win32_runtime_fence fence = {0};
   VkResult result = runtime->retain_fence(runtime->context, import->fence_token, &fence);
   if (result != VK_SUCCESS) return result;
   /* Import the frontend's already-owned reference. Do not reopen it on the
    * unrelated KMT allocator device: GPU waits validate the context device. */
   struct windows_sync *win = container_of(&semaphore->permanent, struct windows_sync, base);
   win_sync_finish(&dev->vk, &win->base);
   win->runtime_fence = fence;
   win->fence = (struct tg_kmt_fence){fence.handle, fence.value};
   return VK_SUCCESS;
}

const struct vk_sync_type nvkmd_windows_sync_type = {
   .size = sizeof(struct windows_sync),
   .features = VK_SYNC_FEATURE_TIMELINE | VK_SYNC_FEATURE_CPU_WAIT |
               VK_SYNC_FEATURE_CPU_SIGNAL | VK_SYNC_FEATURE_GPU_WAIT |
               VK_SYNC_FEATURE_WAIT_BEFORE_SIGNAL,
   .init = win_sync_init,
   .finish = win_sync_finish,
   .signal = win_sync_signal,
   .get_value = win_sync_value,
   .wait = win_sync_wait,
   .import_win32_handle = win_sync_import,
   .export_win32_handle = win_sync_export,
   .set_win32_export_params = win_sync_export_params,
};

static struct tg_kmt_fence *
queue_fence(struct vk_sync *sync, uint64_t *value)
{
   struct vk_sync_binary *binary = vk_sync_as_binary(sync);
   if (binary) {
      sync = &binary->timeline;
      *value = binary->next_point;
   }
   return sync->type == &nvkmd_windows_sync_type ? sync_fence(sync) : NULL;
}

static VkResult
win_ctx_wait(struct nvkmd_ctx *base, struct vk_object_base *log,
             uint32_t count, const struct vk_sync_wait *waits)
{
   struct windows_ctx *ctx = container_of(base, struct windows_ctx, base);
   for (uint32_t i = 0; i < count; i++) {
      uint64_t value = waits[i].wait_value;
      struct tg_kmt_fence *fence = queue_fence(waits[i].sync, &value);
      if (!fence)
         return vk_error(log, VK_ERROR_FEATURE_NOT_PRESENT);
      const struct wsi_win32_runtime *runtime = ctx_runtime(base->dev);
      if (runtime) {
         VkResult result = runtime->gpu_sync(runtime->context, ctx->runtime_context,
                                            false, 1, &fence->handle, &value);
         if (result != VK_SUCCESS) return result;
         continue;
      }
      LONG status = tg_kmt_wait_gpu(ctx->context, 1, &fence->handle, &value);
      if (status < 0)
         return nvkmd_windows_status(log, "GPU wait monitored fence", status,
                                     VK_ERROR_DEVICE_LOST);
   }
   return VK_SUCCESS;
}

static VkResult
win_ctx_signal(struct nvkmd_ctx *base, struct vk_object_base *log,
               uint32_t count, const struct vk_sync_signal *signals)
{
   struct windows_ctx *ctx = container_of(base, struct windows_ctx, base);
   for (uint32_t i = 0; i < count; i++) {
      uint64_t value = signals[i].signal_value;
      struct tg_kmt_fence *fence = queue_fence(signals[i].sync, &value);
      if (!fence)
         return vk_error(log, VK_ERROR_FEATURE_NOT_PRESENT);
      const struct wsi_win32_runtime *runtime = ctx_runtime(base->dev);
      if (runtime) {
         VkResult result = runtime->gpu_sync(runtime->context, ctx->runtime_context,
                                            true, 1, &fence->handle, &value);
         if (result != VK_SUCCESS) return result;
         continue;
      }
      LONG status = tg_kmt_signal_gpu(ctx->context, 1, &fence->handle, &value);
      if (status < 0)
         return nvkmd_windows_status(log, "GPU signal monitored fence", status,
                                     VK_ERROR_DEVICE_LOST);
   }
   return VK_SUCCESS;
}

static VkResult
win_ctx_exec(struct nvkmd_ctx *base, struct vk_object_base *log,
             uint32_t count, const struct nvkmd_ctx_exec *execs)
{
   struct windows_ctx *ctx = container_of(base, struct windows_ctx, base);
   for (uint32_t i = 0; i < count; i++) {
      const struct nvkmd_ctx_exec *exec = &execs[i];
      if (exec->incomplete || exec->no_prefetch || !exec->size_B ||
          exec->size_B > TG_GPU_SUBMIT_MAX_BYTES)
         return vk_errorf(log, VK_ERROR_FEATURE_NOT_PRESENT,
                          "Windows NVK requires complete CPU-visible push buffers");

      uint64_t offset;
      struct nvkmd_mem *mem = nvkmd_dev_lookup_mem_by_va(base->dev, exec->addr, &offset);
      if (!mem)
         return vk_error(log, VK_ERROR_DEVICE_LOST);

      VkResult result = VK_ERROR_DEVICE_LOST;
      if (offset <= mem->size_B && exec->size_B <= mem->size_B - offset) {
         void *cpu = NULL;
         result = nvkmd_mem_map(mem, log, NVKMD_MEM_MAP_RD, NULL, &cpu);
         if (result == VK_SUCCESS) {
            const struct wsi_win32_runtime *runtime = ctx_runtime(base->dev);
            if (runtime) {
               result = runtime->submit(runtime->context, ctx->runtime_context, exec->addr,
                  (const uint8_t *)cpu + offset, exec->size_B, exec->written_count, exec->written_allocations);
            } else {
               LONG status = tg_kmt_submit_primaries(ctx->context, exec->addr,
                  (const uint8_t *)cpu + offset, exec->size_B,
                  exec->written_count, exec->written_allocations);
               result = nvkmd_windows_status(log, "D3DKMTSubmitCommand", status,
                                             VK_ERROR_DEVICE_LOST);
            }
            nvkmd_mem_unmap(mem, 0);
         }
      }
      nvkmd_mem_unref(mem);
      if (result != VK_SUCCESS)
         return result;
   }
   return VK_SUCCESS;
}

static VkResult
win_ctx_bind(struct nvkmd_ctx *base, struct vk_object_base *log,
             uint32_t count, const struct nvkmd_ctx_bind *binds)
{
   (void)base;
   for (uint32_t i = 0; i < count; i++) {
      const struct nvkmd_ctx_bind *bind = &binds[i];
      VkResult result;
      if (bind->op == NVKMD_BIND_OP_BIND)
         result = nvkmd_va_bind_mem(bind->va, log, bind->va_offset_B,
                                    bind->mem, bind->mem_offset_B, bind->range_B);
      else
         result = nvkmd_va_unbind(bind->va, log, bind->va_offset_B, bind->range_B);
      if (result != VK_SUCCESS)
         return result;
   }
   return VK_SUCCESS;
}

static VkResult
win_ctx_flush(struct nvkmd_ctx *base, struct vk_object_base *log)
{
   (void)base;
   (void)log;
   return VK_SUCCESS;
}

static VkResult
win_ctx_sync(struct nvkmd_ctx *base, struct vk_object_base *log)
{
   struct windows_ctx *ctx = container_of(base, struct windows_ctx, base);
   uint64_t value = ++ctx->idle_value;
   const struct wsi_win32_runtime *runtime = ctx_runtime(base->dev);
   if (runtime) {
      VkResult result = runtime->gpu_sync(runtime->context, ctx->runtime_context,
                                         true, 1, &ctx->runtime_idle.handle, &value);
      if (result == VK_SUCCESS)
         result = runtime->wait_cpu(runtime->context, ctx->runtime_idle.token, value, INFINITE);
      return result;
   }
   LONG status = tg_kmt_signal_gpu(ctx->context, 1, &ctx->idle.handle, &value);
   if (status >= 0)
      status = tg_kmt_wait_fences(&nvkmd_windows_dev(base->dev)->kmt, 1,
                                  &ctx->idle.handle, &value, false, INFINITE);
   return nvkmd_windows_status(log, "wait for KMT context idle", status,
                               VK_ERROR_DEVICE_LOST);
}

static void
win_destroy_ctx(struct nvkmd_ctx *base)
{
   struct windows_ctx *ctx = container_of(base, struct windows_ctx, base);
   const struct wsi_win32_runtime *runtime = ctx_runtime(base->dev);
   if (runtime) {
      runtime->destroy_context(runtime->context, ctx->runtime_context);
      runtime->release_fence(runtime->context, ctx->runtime_idle.token);
      free(ctx);
      return;
   }
   tg_kmt_destroy_context(ctx->context);
   tg_kmt_destroy_fence(&ctx->idle);
   free(ctx);
}

static const struct nvkmd_ctx_ops win_ctx_ops = {
   .destroy = win_destroy_ctx,
   .wait = win_ctx_wait,
   .exec = win_ctx_exec,
   .bind = win_ctx_bind,
   .signal = win_ctx_signal,
   .flush = win_ctx_flush,
   .sync = win_ctx_sync,
};

VkResult
nvkmd_windows_create_ctx(struct nvkmd_dev *dev, struct vk_object_base *log,
                         enum nvkmd_engines engines, struct nvkmd_ctx **out)
{
   struct windows_ctx *ctx = calloc(1, sizeof(*ctx));
   if (!ctx)
      return vk_error(log, VK_ERROR_OUT_OF_HOST_MEMORY);
   const struct wsi_win32_runtime *runtime = ctx_runtime(dev);
   if (runtime) {
      uint32_t types = 0;
      if (engines & (NVKMD_ENGINE_3D | NVKMD_ENGINE_2D)) types |= WSI_WIN32_RUNTIME_3D;
      if (engines & NVKMD_ENGINE_COMPUTE) types |= WSI_WIN32_RUNTIME_COMPUTE;
      if (engines & (NVKMD_ENGINE_COPY | NVKMD_ENGINE_M2MF | NVKMD_ENGINE_BIND)) types |= WSI_WIN32_RUNTIME_COPY;
      VkResult result = runtime->create_context(runtime->context, types, &ctx->runtime_context);
      if (result == VK_SUCCESS) result = runtime->create_fence(runtime->context, 0, &ctx->runtime_idle);
      if (result != VK_SUCCESS) {
         if (ctx->runtime_context) runtime->destroy_context(runtime->context, ctx->runtime_context);
         free(ctx); return result;
      }
      ctx->base = (struct nvkmd_ctx){.ops = &win_ctx_ops, .dev = dev};
      *out = &ctx->base;
      return VK_SUCCESS;
   }
   struct tg_kmt_device *kmt = &nvkmd_windows_dev(dev)->kmt;
   LONG status = tg_kmt_create_context(kmt, &ctx->context);
   if (status >= 0)
      status = tg_kmt_create_fence(kmt, 0, &ctx->idle);
   if (status < 0) {
      if (ctx->context)
         tg_kmt_destroy_context(ctx->context);
      free(ctx);
      return nvkmd_windows_status(log, "create KMT context and idle fence", status,
                                  VK_ERROR_INITIALIZATION_FAILED);
   }
   ctx->base = (struct nvkmd_ctx){ .ops = &win_ctx_ops, .dev = dev };
   *out = &ctx->base;
   return VK_SUCCESS;
}
