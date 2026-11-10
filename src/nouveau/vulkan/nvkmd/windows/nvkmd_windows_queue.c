/* SPDX-License-Identifier: MIT */
#include "nvkmd_windows.h"
#include "nvk_device.h"
#include "submit_abi.h"
#include "util/os_time.h"
#include "util/u_math.h"
#include "vk_log.h"
#include "vk_sync_binary.h"

struct windows_sync {
   struct vk_sync base;
   struct tg_kmt_fence fence;
};

struct windows_ctx {
   struct nvkmd_ctx base;
   uint32_t context;
   struct tg_kmt_fence idle;
   uint64_t idle_value;
};

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
   LONG status = tg_kmt_create_fence(sync_device(device), initial, sync_fence(sync));
   return nvkmd_windows_status(&device->base, "create monitored fence", status,
                               VK_ERROR_OUT_OF_DEVICE_MEMORY);
}

static void
win_sync_finish(struct vk_device *device, struct vk_sync *sync)
{
   (void)device;
   tg_kmt_destroy_fence(sync_fence(sync));
}

static VkResult
win_sync_signal(struct vk_device *device, struct vk_sync *sync, uint64_t value)
{
   LONG status = tg_kmt_signal_cpu(sync_device(device), sync_fence(sync)->handle, value);
   return nvkmd_windows_status(&device->base, "CPU signal monitored fence", status,
                               VK_ERROR_DEVICE_LOST);
}

static VkResult
win_sync_value(struct vk_device *device, struct vk_sync *sync, uint64_t *value)
{
   (void)device;
   *value = *sync_fence(sync)->value;
   return VK_SUCCESS;
}

static VkResult
win_sync_wait(struct vk_device *device, struct vk_sync *sync, uint64_t value,
              enum vk_sync_wait_flags flags, uint64_t deadline)
{
   assert(!(flags & VK_SYNC_WAIT_PENDING));
   struct tg_kmt_fence *fence = sync_fence(sync);
   while (*fence->value < value) {
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
            LONG status = tg_kmt_submit(ctx->context, exec->addr,
                                        (const uint8_t *)cpu + offset, exec->size_B);
            nvkmd_mem_unmap(mem, 0);
            result = nvkmd_windows_status(log, "D3DKMTSubmitCommand", status,
                                          VK_ERROR_DEVICE_LOST);
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
   (void)engines;
   struct windows_ctx *ctx = calloc(1, sizeof(*ctx));
   if (!ctx)
      return vk_error(log, VK_ERROR_OUT_OF_HOST_MEMORY);
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
