/*
 * Copyright © 2026 Mesa Switch port contributors
 * SPDX-License-Identifier: MIT
 */

#include "nvkmd_switch.h"

#include "util/stack_array.h"
#include "util/u_debug.h"
#include "util/u_math.h"
#include "util/u_memory.h"
#include "vk_log.h"
#include "vk_sync.h"

#define NVKMD_SWITCH_MAX_MULTIFENCE_FENCES 4u
#define NVKMD_SWITCH_MAX_WAIT_BATCH 42u
#define NVKMD_SWITCH_MAX_EXEC_BATCH 64u

struct nvkmd_switch_mem {
   struct nvkmd_mem base;
   struct nouveau_horizon_memory *memory;
   void *cpu_addr;
};

struct nvkmd_switch_va {
   struct nvkmd_va base;
   struct nouveau_horizon_va *va;
};

struct nvkmd_switch_ctx {
   struct nvkmd_ctx base;
   enum nvkmd_engines engines;
   struct nouveau_horizon_channel *channel;
   struct nouveau_horizon_fence last_fence;
   bool has_last_fence;
   bool teardown_quarantined;
};

static struct nvkmd_switch_dev *
nvkmd_switch_dev(struct nvkmd_dev *dev)
{
   return container_of(dev, struct nvkmd_switch_dev, base);
}

void
nvkmd_switch_dev_get_zbc_state(
   struct nvkmd_dev *dev, struct nouveau_horizon_zbc_state *state_out)
{
   nouveau_horizon_device_get_zbc_state(
      nvkmd_switch_dev(dev)->horizon, state_out);
}

uint64_t
nvkmd_switch_dev_get_zbc_generation(struct nvkmd_dev *dev)
{
   return nouveau_horizon_device_get_zbc_generation(
      nvkmd_switch_dev(dev)->horizon);
}

void
nvkmd_switch_dev_record_zbc_program(struct nvkmd_dev *dev)
{
   nouveau_horizon_device_record_zbc_program(
      nvkmd_switch_dev(dev)->horizon);
}

static struct nvkmd_switch_mem *
nvkmd_switch_mem(struct nvkmd_mem *mem)
{
   return container_of(mem, struct nvkmd_switch_mem, base);
}

static struct nvkmd_switch_va *
nvkmd_switch_va(struct nvkmd_va *va)
{
   return container_of(va, struct nvkmd_switch_va, base);
}

static struct nvkmd_switch_ctx *
nvkmd_switch_ctx(struct nvkmd_ctx *ctx)
{
   return container_of(ctx, struct nvkmd_switch_ctx, base);
}

static struct vk_device *
nvkmd_switch_log_device(struct vk_object_base *log_obj)
{
   return log_obj != NULL ? log_obj->device : NULL;
}

static VkResult
nvkmd_switch_log_result(struct vk_object_base *log_obj, VkResult error)
{
   /* Device bring-up logs against the physical device.  The Vulkan runtime
    * cannot attribute memory-map/OODM errors to a logical device yet.
    */
   if (log_obj != NULL && log_obj->type == VK_OBJECT_TYPE_PHYSICAL_DEVICE &&
       (error == VK_ERROR_OUT_OF_DEVICE_MEMORY ||
        error == VK_ERROR_MEMORY_MAP_FAILED))
      return VK_ERROR_INITIALIZATION_FAILED;

   return error;
}

static VkResult
nvkmd_switch_status_result(struct vk_object_base *log_obj,
                           enum nouveau_horizon_status status,
                           VkResult fallback, const char *operation)
{
   if (status == NOUVEAU_HORIZON_SUCCESS)
      return VK_SUCCESS;

   VkResult result;
   switch (status) {
   case NOUVEAU_HORIZON_ERROR_OUT_OF_HOST_MEMORY:
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
      break;
   case NOUVEAU_HORIZON_ERROR_OUT_OF_DEVICE_MEMORY:
   case NOUVEAU_HORIZON_ERROR_NO_SPACE:
      result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
      break;
   case NOUVEAU_HORIZON_ERROR_TIMEOUT:
      result = VK_TIMEOUT;
      break;
   case NOUVEAU_HORIZON_ERROR_DEVICE_LOST:
      result = VK_ERROR_DEVICE_LOST;
      break;
   case NOUVEAU_HORIZON_ERROR_NOT_SUPPORTED:
      result = VK_ERROR_FEATURE_NOT_PRESENT;
      break;
   default:
      result = fallback;
      break;
   }

   result = nvkmd_switch_log_result(log_obj, result);
   if (log_obj == NULL)
      return result;

   return vk_errorf(log_obj, result, "nvkmd-switch: %s failed: %s",
                    operation, nouveau_horizon_status_string(status));
}

static VkResult
nvkmd_switch_ctx_status_result(struct nvkmd_switch_ctx *ctx,
                               struct vk_object_base *log_obj,
                               enum nouveau_horizon_status status,
                               const char *operation)
{
   if (status != NOUVEAU_HORIZON_ERROR_DEVICE_LOST ||
       ctx->channel == NULL)
      return nvkmd_switch_status_result(log_obj, status,
                                        VK_ERROR_DEVICE_LOST,
                                        operation);

   struct nouveau_horizon_error error = {0};
   (void)nouveau_horizon_channel_get_error(ctx->channel, &error);
   if (log_obj == NULL)
      return VK_ERROR_DEVICE_LOST;

   return vk_errorf(
      log_obj, VK_ERROR_DEVICE_LOST,
      "nvkmd-switch: %s failed: %s (native=0x%x, notification={type=%u, "
      "info=%u, status=%u}, channel={type=%u, info=[%u,%u,%u,%u]})",
      operation, nouveau_horizon_status_string(status), error.native_result,
      error.notification_type, error.notification_info,
      error.notification_status, error.channel_error_type,
      error.channel_error_info[0], error.channel_error_info[1],
      error.channel_error_info[2], error.channel_error_info[3]);
}

static uint64_t
nvkmd_switch_align_up(uint64_t value, uint64_t align)
{
   if (align == 0)
      return value;
   const uint64_t remainder = value % align;
   if (remainder == 0)
      return value;
   if (value > UINT64_MAX - (align - remainder))
      return 0;
   return value + align - remainder;
}

static void
nvkmd_switch_horizon_log(void *data,
                         enum nouveau_horizon_log_level level,
                         const char *message)
{
   struct vk_object_base *log_obj = data;
   if (log_obj == NULL)
      return;

   switch (level) {
   case NOUVEAU_HORIZON_LOG_DEBUG:
      vk_logd(VK_LOG_OBJS(log_obj), "nvkmd-switch: %s", message);
      break;
   case NOUVEAU_HORIZON_LOG_INFO:
      vk_logi(VK_LOG_OBJS(log_obj), "nvkmd-switch: %s", message);
      break;
   case NOUVEAU_HORIZON_LOG_WARNING:
      vk_logw(VK_LOG_OBJS(log_obj), "nvkmd-switch: %s", message);
      break;
   case NOUVEAU_HORIZON_LOG_ERROR:
      vk_loge(VK_LOG_OBJS(log_obj), "nvkmd-switch: %s", message);
      break;
   }
}

static VkResult
nvkmd_switch_ctx_submit(struct nvkmd_switch_ctx *ctx,
                        struct vk_object_base *log_obj,
                        enum nouveau_horizon_completion_mode mode)
{
   if (ctx->channel == NULL)
      return VK_SUCCESS;

   struct nouveau_horizon_fence fence = {
      .id = NOUVEAU_HORIZON_INVALID_FENCE_ID,
   };
   const enum nouveau_horizon_status status =
      nouveau_horizon_channel_submit(ctx->channel, mode, &fence);
   if (status != NOUVEAU_HORIZON_SUCCESS)
      return nvkmd_switch_ctx_status_result(ctx, log_obj, status,
                                            "channel submit");

   ctx->last_fence = fence;
   ctx->has_last_fence = nouveau_horizon_fence_is_valid(&fence);
   return VK_SUCCESS;
}

bool
nvkmd_switch_ctx_try_destroy(struct nvkmd_ctx *_ctx,
                             struct vk_object_base *log_obj)
{
   struct nvkmd_switch_ctx *ctx = nvkmd_switch_ctx(_ctx);

   /* channel_put() consumes our reference even when it has to quarantine the
    * zero-reference channel.  Never call it a second time in that case.
    */
   if (ctx->teardown_quarantined)
      return false;

   const enum nouveau_horizon_channel_put_result put_result =
      nouveau_horizon_channel_put(ctx->channel);
   if (put_result != NOUVEAU_HORIZON_CHANNEL_PUT_COMPLETE) {
      ctx->teardown_quarantined = true;
      const char *reason =
         put_result == NOUVEAU_HORIZON_CHANNEL_PUT_QUARANTINED ?
            "unknown native completion" : "retained channel reference";
      if (log_obj != NULL) {
         vk_loge(VK_LOG_OBJS(log_obj),
                 "nvkmd-switch: context teardown has %s; retaining the "
                 "adapter context and requiring owner quarantine",
                 reason);
      } else {
         _debug_printf("nvkmd-switch: context teardown has %s; retaining "
                       "the adapter context and requiring owner quarantine\n",
                       reason);
      }
      return false;
   }

   FREE(ctx);
   return true;
}

static void
nvkmd_switch_ctx_destroy(struct nvkmd_ctx *_ctx)
{
   (void)nvkmd_switch_ctx_try_destroy(_ctx, NULL);
}

static VkResult
nvkmd_switch_ctx_wait(struct nvkmd_ctx *_ctx,
                      struct vk_object_base *log_obj,
                      uint32_t wait_count,
                      const struct vk_sync_wait *waits)
{
   struct nvkmd_switch_ctx *ctx = nvkmd_switch_ctx(_ctx);
   struct vk_device *device = nvkmd_switch_log_device(log_obj);
   if (device == NULL) {
      return vk_errorf(log_obj, VK_ERROR_INITIALIZATION_FAILED,
                       "nvkmd-switch: missing vk_device for context wait");
   }

   if (ctx->channel == NULL || wait_count == 0) {
      return vk_sync_wait_many(device, wait_count, waits,
                               0, UINT64_MAX);
   }

   if ((size_t)wait_count >
       SIZE_MAX / NVKMD_SWITCH_MAX_MULTIFENCE_FENCES /
          sizeof(struct nouveau_horizon_fence))
      return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);

   const size_t native_fence_capacity =
      (size_t)wait_count * NVKMD_SWITCH_MAX_MULTIFENCE_FENCES;
   STACK_ARRAY(struct vk_sync_wait, host_waits, wait_count);
   STACK_ARRAY(struct nouveau_horizon_fence, native_fences,
               native_fence_capacity);
   if (host_waits == NULL || native_fences == NULL) {
      STACK_ARRAY_FINISH(native_fences);
      STACK_ARRAY_FINISH(host_waits);
      return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);
   }
   uint32_t host_wait_count = 0;
   uint32_t native_fence_count = 0;

   for (uint32_t i = 0; i < wait_count; i++) {
      const uint32_t count = nvkmd_switch_sync_peek_horizon_fences(
         waits[i].sync, NVKMD_SWITCH_MAX_MULTIFENCE_FENCES,
         &native_fences[native_fence_count]);
      if (count > 0)
         native_fence_count += count;
      else
         host_waits[host_wait_count++] = waits[i];
   }

   VkResult result = VK_SUCCESS;
   if (host_wait_count > 0) {
      result = vk_sync_wait_many(device, host_wait_count, host_waits,
                                 0, UINT64_MAX);
      if (result != VK_SUCCESS)
         goto done;
   }

   for (uint32_t first = 0; first < native_fence_count;) {
      const uint32_t count = MIN2(NVKMD_SWITCH_MAX_WAIT_BATCH,
                                  native_fence_count - first);
      enum nouveau_horizon_status status =
         nouveau_horizon_channel_enqueue_waits(ctx->channel, count,
                                                &native_fences[first]);
      if (status == NOUVEAU_HORIZON_ERROR_NO_SPACE) {
         result = nvkmd_switch_ctx_submit(ctx, log_obj,
                                          NOUVEAU_HORIZON_COMPLETION_GPU);
         if (result != VK_SUCCESS)
            goto done;
         status = nouveau_horizon_channel_enqueue_waits(
            ctx->channel, count, &native_fences[first]);
      }
      if (status != NOUVEAU_HORIZON_SUCCESS) {
         result = nvkmd_switch_ctx_status_result(ctx, log_obj, status,
                                                 "enqueue native waits");
         goto done;
      }
      first += count;
   }

done:
   STACK_ARRAY_FINISH(native_fences);
   STACK_ARRAY_FINISH(host_waits);
   return result;
}

static VkResult
nvkmd_switch_ctx_exec(struct nvkmd_ctx *_ctx,
                      struct vk_object_base *log_obj,
                      uint32_t exec_count,
                      const struct nvkmd_ctx_exec *execs)
{
   struct nvkmd_switch_ctx *ctx = nvkmd_switch_ctx(_ctx);

   for (uint32_t i = 0; i < exec_count; i++) {
      if ((execs[i].addr & 3) != 0 || execs[i].size_B == 0 ||
          (execs[i].size_B & 3) != 0) {
         return vk_errorf(log_obj, VK_ERROR_INITIALIZATION_FAILED,
                          "nvkmd-switch: invalid push buffer");
      }
      if (execs[i].incomplete && i + 1 == exec_count) {
         return vk_errorf(log_obj, VK_ERROR_DEVICE_LOST,
                          "nvkmd-switch: incomplete push buffer without "
                          "continuation");
      }
   }

   if (ctx->channel == NULL)
      return VK_SUCCESS;

   STACK_ARRAY(struct nouveau_horizon_exec, horizon_execs, exec_count);
   if (horizon_execs == NULL)
      return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);

   for (uint32_t i = 0; i < exec_count; i++) {
      horizon_execs[i] = (struct nouveau_horizon_exec) {
         .addr = execs[i].addr,
         .size_B = execs[i].size_B,
         .incomplete = execs[i].incomplete,
         .no_prefetch = execs[i].no_prefetch,
      };
   }

   VkResult result = VK_SUCCESS;
   uint32_t first = 0;
   while (first < exec_count) {
      uint32_t count = MIN2(NVKMD_SWITCH_MAX_EXEC_BATCH,
                            exec_count - first);

      /* Never split an incomplete method across submissions. Extend
       * through its final entry; the channel capacity check rejects
       * oversized chains.
       */
      while (first + count < exec_count &&
             execs[first + count - 1].incomplete)
         count++;

      enum nouveau_horizon_status status =
         nouveau_horizon_channel_exec(ctx->channel, count,
                                      &horizon_execs[first]);
      if (status == NOUVEAU_HORIZON_ERROR_NO_SPACE) {
         result = nvkmd_switch_ctx_submit(
            ctx, log_obj, NOUVEAU_HORIZON_COMPLETION_GPU);
         if (result != VK_SUCCESS)
            goto done;
         status = nouveau_horizon_channel_exec(ctx->channel, count,
                                                &horizon_execs[first]);
      }
      if (status != NOUVEAU_HORIZON_SUCCESS) {
         result = nvkmd_switch_ctx_status_result(ctx, log_obj, status,
                                                 "enqueue push buffers");
         goto done;
      }
      first += count;
   }

done:
   STACK_ARRAY_FINISH(horizon_execs);
   return result;
}

static VkResult
nvkmd_switch_ctx_bind(struct nvkmd_ctx *_ctx,
                      struct vk_object_base *log_obj,
                      uint32_t bind_count,
                      const struct nvkmd_ctx_bind *binds)
{
   for (uint32_t i = 0; i < bind_count; i++) {
      VkResult result;
      if (binds[i].op == NVKMD_BIND_OP_BIND) {
         result = nvkmd_va_bind_mem(binds[i].va, log_obj,
                                    binds[i].va_offset_B, binds[i].mem,
                                    binds[i].mem_offset_B, binds[i].range_B);
      } else {
         result = nvkmd_va_unbind(binds[i].va, log_obj,
                                  binds[i].va_offset_B, binds[i].range_B);
      }
      if (result != VK_SUCCESS)
         return result;
   }
   return VK_SUCCESS;
}

static VkResult
nvkmd_switch_ctx_signal(struct nvkmd_ctx *_ctx,
                        struct vk_object_base *log_obj,
                        uint32_t signal_count,
                        const struct vk_sync_signal *signals)
{
   struct nvkmd_switch_ctx *ctx = nvkmd_switch_ctx(_ctx);
   struct vk_device *device = nvkmd_switch_log_device(log_obj);
   if (device == NULL) {
      return vk_errorf(log_obj, VK_ERROR_INITIALIZATION_FAILED,
                       "nvkmd-switch: missing vk_device for context signal");
   }

   if (ctx->channel != NULL) {
      const enum nouveau_horizon_completion_mode mode =
         signal_count > 0 ? NOUVEAU_HORIZON_COMPLETION_CPU :
                            NOUVEAU_HORIZON_COMPLETION_GPU;
      VkResult result = nvkmd_switch_ctx_submit(ctx, log_obj, mode);
      if (result != VK_SUCCESS)
         return result;

      if (ctx->has_last_fence) {
         for (uint32_t i = 0; i < signal_count; i++) {
            struct vk_sync *sync = signals[i].sync;
            if (sync != NULL && nvkmd_switch_sync_is_nvfence(sync->type)) {
               nvkmd_switch_sync_import_horizon_fence(sync,
                                                       &ctx->last_fence);
            }
         }
      }
   }

   return vk_sync_signal_many(device, signal_count, signals);
}

static VkResult
nvkmd_switch_ctx_flush(struct nvkmd_ctx *_ctx,
                       struct vk_object_base *log_obj)
{
   return nvkmd_switch_ctx_submit(nvkmd_switch_ctx(_ctx), log_obj,
                                  NOUVEAU_HORIZON_COMPLETION_GPU);
}

static VkResult
nvkmd_switch_ctx_sync(struct nvkmd_ctx *_ctx,
                      struct vk_object_base *log_obj)
{
   struct nvkmd_switch_ctx *ctx = nvkmd_switch_ctx(_ctx);
   if (ctx->channel == NULL)
      return VK_SUCCESS;

   const enum nouveau_horizon_status status =
      nouveau_horizon_channel_wait_idle(ctx->channel, UINT64_MAX);
   if (status == NOUVEAU_HORIZON_SUCCESS) {
      ctx->has_last_fence = false;
      ctx->last_fence.id = NOUVEAU_HORIZON_INVALID_FENCE_ID;
      return VK_SUCCESS;
   }
   return nvkmd_switch_ctx_status_result(ctx, log_obj, status,
                                         "channel wait idle");
}

static const struct nvkmd_ctx_ops nvkmd_switch_ctx_ops = {
   .destroy = nvkmd_switch_ctx_destroy,
   .wait = nvkmd_switch_ctx_wait,
   .exec = nvkmd_switch_ctx_exec,
   .bind = nvkmd_switch_ctx_bind,
   .signal = nvkmd_switch_ctx_signal,
   .flush = nvkmd_switch_ctx_flush,
   .sync = nvkmd_switch_ctx_sync,
};

static void
nvkmd_switch_mem_free(struct nvkmd_mem *_mem)
{
   struct nvkmd_switch_mem *mem = nvkmd_switch_mem(_mem);
   if (mem->base.va != NULL)
      nvkmd_va_free(mem->base.va);
   nouveau_horizon_memory_put(mem->memory);
   simple_mtx_destroy(&mem->base.map_mutex);
   FREE(mem);
}

static VkResult
nvkmd_switch_mem_map(struct nvkmd_mem *_mem,
                     struct vk_object_base *log_obj,
                     enum nvkmd_mem_map_flags flags,
                     void *fixed_addr, void **map_out)
{
   struct nvkmd_switch_mem *mem = nvkmd_switch_mem(_mem);
   void *map = NULL;
   enum nouveau_horizon_status status =
      nouveau_horizon_memory_map(mem->memory, &map);
   if (status != NOUVEAU_HORIZON_SUCCESS) {
      return nvkmd_switch_status_result(log_obj, status,
                                        VK_ERROR_MEMORY_MAP_FAILED,
                                        "map memory");
   }

   if ((flags & NVKMD_MEM_MAP_FIXED) && fixed_addr != map) {
      return vk_errorf(log_obj,
                       nvkmd_switch_log_result(log_obj,
                                               VK_ERROR_MEMORY_MAP_FAILED),
                       "nvkmd-switch: fixed CPU mappings are unsupported");
   }

   *map_out = map;
   return VK_SUCCESS;
}

static void
nvkmd_switch_mem_unmap(struct nvkmd_mem *_mem,
                       enum nvkmd_mem_map_flags flags, void *map)
{
}

static VkResult
nvkmd_switch_mem_overmap(struct nvkmd_mem *_mem,
                         struct vk_object_base *log_obj,
                         enum nvkmd_mem_map_flags flags, void *map)
{
   return vk_errorf(log_obj,
                    nvkmd_switch_log_result(log_obj,
                                            VK_ERROR_MEMORY_MAP_FAILED),
                    "nvkmd-switch: overmap is unsupported");
}

static void
nvkmd_switch_mem_sync_to_gpu(struct nvkmd_mem *_mem,
                             uint64_t offset_B, uint64_t range_B)
{
   nouveau_horizon_memory_sync_to_gpu(nvkmd_switch_mem(_mem)->memory,
                                      offset_B, range_B);
}

static void
nvkmd_switch_mem_sync_from_gpu(struct nvkmd_mem *_mem,
                               uint64_t offset_B, uint64_t range_B)
{
   nouveau_horizon_memory_sync_from_gpu(nvkmd_switch_mem(_mem)->memory,
                                        offset_B, range_B);
}

static VkResult
nvkmd_switch_mem_export_dma_buf(struct nvkmd_mem *_mem,
                                struct vk_object_base *log_obj,
                                int *fd_out)
{
   return vk_errorf(log_obj, VK_ERROR_INVALID_EXTERNAL_HANDLE,
                    "nvkmd-switch: dma-buf export is unsupported");
}

static uint32_t
nvkmd_switch_mem_log_handle(struct nvkmd_mem *_mem)
{
   return nouveau_horizon_memory_get_nvmap_handle(
      nvkmd_switch_mem(_mem)->memory);
}

static const struct nvkmd_mem_ops nvkmd_switch_mem_ops = {
   .free = nvkmd_switch_mem_free,
   .map = nvkmd_switch_mem_map,
   .unmap = nvkmd_switch_mem_unmap,
   .overmap = nvkmd_switch_mem_overmap,
   .sync_to_gpu = nvkmd_switch_mem_sync_to_gpu,
   .sync_from_gpu = nvkmd_switch_mem_sync_from_gpu,
   .export_dma_buf = nvkmd_switch_mem_export_dma_buf,
   .log_handle = nvkmd_switch_mem_log_handle,
};

static void
nvkmd_switch_va_free(struct nvkmd_va *_va)
{
   struct nvkmd_switch_va *va = nvkmd_switch_va(_va);
   nouveau_horizon_va_put(va->va);
   FREE(va);
}

static VkResult
nvkmd_switch_va_bind_mem(struct nvkmd_va *_va,
                         struct vk_object_base *log_obj,
                         uint64_t va_offset_B, struct nvkmd_mem *_mem,
                         uint64_t mem_offset_B, uint64_t range_B)
{
   struct nvkmd_switch_va *va = nvkmd_switch_va(_va);
   struct nvkmd_switch_mem *mem = nvkmd_switch_mem(_mem);
   const enum nouveau_horizon_status status = nouveau_horizon_va_bind(
      va->va, va_offset_B, mem->memory, mem_offset_B, range_B);
   return nvkmd_switch_status_result(log_obj, status,
                                     VK_ERROR_INITIALIZATION_FAILED,
                                     "bind memory to VA");
}

static VkResult
nvkmd_switch_va_unbind(struct nvkmd_va *_va,
                       struct vk_object_base *log_obj,
                       uint64_t va_offset_B, uint64_t range_B)
{
   const enum nouveau_horizon_status status = nouveau_horizon_va_unbind(
      nvkmd_switch_va(_va)->va, va_offset_B, range_B);
   return nvkmd_switch_status_result(log_obj, status,
                                     VK_ERROR_INITIALIZATION_FAILED,
                                     "unbind VA");
}

static const struct nvkmd_va_ops nvkmd_switch_va_ops = {
   .free = nvkmd_switch_va_free,
   .bind_mem = nvkmd_switch_va_bind_mem,
   .unbind = nvkmd_switch_va_unbind,
};

static void
nvkmd_switch_dev_destroy(struct nvkmd_dev *_dev)
{
   struct nvkmd_switch_dev *dev = nvkmd_switch_dev(_dev);

   /* Anything still listed here is a client leak, so release it rather than
    * leaving the NvMap and its backing store behind.
    */
   list_for_each_entry_safe(struct nvkmd_mem, mem, &_dev->mems, link) {
      list_del(&mem->link);
      nvkmd_switch_mem_free(mem);
   }

   simple_mtx_destroy(&dev->base.mems_mutex);
   nouveau_horizon_device_put(dev->horizon);
   nouveau_horizon_runtime_put(dev->runtime);
   FREE(dev);
}

static VkResult
nvkmd_switch_dev_get_gpu_timestamp(struct nvkmd_dev *_dev,
                                   uint64_t *timestamp)
{
   uint64_t gpu_timestamp;
   Result rc = nvGpuGetTimestamp(&gpu_timestamp);
   if (R_FAILED(rc))
      return VK_ERROR_DEVICE_LOST;

   *timestamp = gpu_timestamp;
   return VK_SUCCESS;
}

static int
nvkmd_switch_dev_get_drm_fd(struct nvkmd_dev *_dev)
{
   return -1;
}

static VkResult
nvkmd_switch_dev_alloc_va(struct nvkmd_dev *_dev,
                          struct vk_object_base *log_obj,
                          enum nvkmd_va_flags flags, uint8_t pte_kind,
                          uint64_t size_B, uint64_t align_B,
                          uint64_t fixed_addr, struct nvkmd_va **va_out)
{
   struct nvkmd_switch_dev *dev = nvkmd_switch_dev(_dev);
   struct nvkmd_switch_va *va = CALLOC_STRUCT(nvkmd_switch_va);
   if (va == NULL)
      return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);

   struct nouveau_horizon_va_create_info create_info = {
      .size_B = size_B,
      .align_B = align_B,
      .fixed_addr = fixed_addr,
      .flags = ((flags & NVKMD_VA_ALLOC_FIXED) ?
                   NOUVEAU_HORIZON_VA_FIXED : 0) |
               ((flags & NVKMD_VA_SPARSE) ?
                   NOUVEAU_HORIZON_VA_SPARSE : 0),
      .has_pte_kind = pte_kind != 0,
      .pte_kind = pte_kind,
   };
   const enum nouveau_horizon_status status = nouveau_horizon_va_create(
      dev->horizon, &create_info, &va->va);
   if (status != NOUVEAU_HORIZON_SUCCESS) {
      FREE(va);
      const VkResult fallback =
         (flags & NVKMD_VA_ALLOC_FIXED) ?
            VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS :
            VK_ERROR_OUT_OF_DEVICE_MEMORY;
      return nvkmd_switch_status_result(log_obj, status, fallback,
                                         "allocate VA");
   }

   va->base.ops = &nvkmd_switch_va_ops;
   va->base.dev = _dev;
   va->base.flags = flags;
   va->base.pte_kind = pte_kind;
   va->base.addr = nouveau_horizon_va_get_addr(va->va);
   va->base.size_B = nouveau_horizon_va_get_size(va->va);
   *va_out = &va->base;
   return VK_SUCCESS;
}

static uint32_t
nvkmd_switch_memory_flags(enum nvkmd_mem_flags flags)
{
   uint32_t horizon_flags = NOUVEAU_HORIZON_MEMORY_CPU_VISIBLE;
   if (!(flags & NVKMD_MEM_COHERENT))
      horizon_flags |= NOUVEAU_HORIZON_MEMORY_CPU_CACHED;
   if (!(flags & NVKMD_MEM_GPU_UNCACHED))
      horizon_flags |= NOUVEAU_HORIZON_MEMORY_GPU_CACHED;
   return horizon_flags;
}

static VkResult
nvkmd_switch_dev_alloc_mem_impl(struct nvkmd_switch_dev *dev,
                                 struct vk_object_base *log_obj,
                                 uint64_t size_B, uint64_t align_B,
                                 uint8_t pte_kind, uint16_t tile_mode,
                                 enum nvkmd_mem_flags flags,
                                struct nvkmd_mem **mem_out)
{
   struct nouveau_horizon_device_properties properties;
   nouveau_horizon_device_get_properties(dev->horizon, &properties);
   const uint32_t bind_align_B = properties.bind_align_B;
   size_B = nvkmd_switch_align_up(size_B, bind_align_B);
   align_B = MAX2(align_B, (uint64_t)bind_align_B);
   if (size_B == 0 || !util_is_power_of_two_nonzero64(align_B)) {
      return vk_errorf(log_obj,
                       nvkmd_switch_log_result(
                          log_obj, VK_ERROR_OUT_OF_DEVICE_MEMORY),
                       "nvkmd-switch: invalid memory size/alignment");
   }

   struct nvkmd_switch_mem *mem = CALLOC_STRUCT(nvkmd_switch_mem);
   if (mem == NULL)
      return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);

   uint32_t horizon_flags = nvkmd_switch_memory_flags(flags) |
                            NOUVEAU_HORIZON_MEMORY_ZERO;

   const struct nouveau_horizon_memory_create_info create_info = {
      .size_B = size_B,
      .align_B = align_B,
       .backing_kind = NvKind_Pitch,
       .flags = horizon_flags,
       .layout = {
          /* General Vulkan memory may intentionally be aliased through image
           * VAs with several PTE kinds/tile modes.  Only allocations created
           * for a known tiled layout are immutable/exportable identities.
           */
          .valid = pte_kind != 0 || tile_mode != 0,
          .pte_kind = pte_kind,
          .tile_mode = tile_mode,
       },
   };
   enum nouveau_horizon_status status = nouveau_horizon_memory_create(
      dev->horizon, &create_info, &mem->memory);
   if (status != NOUVEAU_HORIZON_SUCCESS) {
      FREE(mem);
      return nvkmd_switch_status_result(log_obj, status,
                                         VK_ERROR_OUT_OF_DEVICE_MEMORY,
                                         "allocate memory");
   }

   status = nouveau_horizon_memory_map(mem->memory, &mem->cpu_addr);
   if (status != NOUVEAU_HORIZON_SUCCESS) {
      nouveau_horizon_memory_put(mem->memory);
      FREE(mem);
      return nvkmd_switch_status_result(log_obj, status,
                                         VK_ERROR_MEMORY_MAP_FAILED,
                                         "map allocated memory");
   }

   size_B = nouveau_horizon_memory_get_size(mem->memory);
   nvkmd_mem_init(&dev->base, &mem->base, &nvkmd_switch_mem_ops,
                  flags, size_B, bind_align_B);

   VkResult result = nvkmd_dev_alloc_va(&dev->base, log_obj, 0, pte_kind,
                                        size_B, align_B, 0,
                                        &mem->base.va);
   if (result != VK_SUCCESS)
      goto fail_memory;

   result = nvkmd_va_bind_mem(mem->base.va, log_obj, 0, &mem->base,
                              0, size_B);
   if (result != VK_SUCCESS)
      goto fail_va;

   *mem_out = &mem->base;
   return VK_SUCCESS;

fail_va:
   nvkmd_va_free(mem->base.va);
   mem->base.va = NULL;
fail_memory:
   simple_mtx_destroy(&mem->base.map_mutex);
   nouveau_horizon_memory_put(mem->memory);
   FREE(mem);
   return result;
}

static VkResult
nvkmd_switch_dev_alloc_mem(struct nvkmd_dev *_dev,
                           struct vk_object_base *log_obj,
                           uint64_t size_B, uint64_t align_B,
                           enum nvkmd_mem_flags flags,
                           struct nvkmd_mem **mem_out)
{
   return nvkmd_switch_dev_alloc_mem_impl(nvkmd_switch_dev(_dev), log_obj,
                                           size_B, align_B, 0, 0,
                                           flags, mem_out);
}

static VkResult
nvkmd_switch_dev_alloc_tiled_mem(struct nvkmd_dev *_dev,
                                 struct vk_object_base *log_obj,
                                 uint64_t size_B, uint64_t align_B,
                                 uint8_t pte_kind, uint16_t tile_mode,
                                 enum nvkmd_mem_flags flags,
                                 struct nvkmd_mem **mem_out)
{
   return nvkmd_switch_dev_alloc_mem_impl(nvkmd_switch_dev(_dev), log_obj,
                                           size_B, align_B, pte_kind,
                                           tile_mode,
                                           flags, mem_out);
}

VkResult
nvkmd_switch_dev_import_nvmap(struct nvkmd_dev *_dev,
                              struct vk_object_base *log_obj,
                              uint32_t nvmap_id,
                              uint64_t size_B,
                              uint64_t align_B,
                              uint8_t pte_kind,
                              uint16_t tile_mode,
                              enum nvkmd_mem_flags flags,
                              struct nvkmd_mem **mem_out)
{
   struct nvkmd_switch_dev *dev = nvkmd_switch_dev(_dev);
   struct nouveau_horizon_device_properties properties;
   nouveau_horizon_device_get_properties(dev->horizon, &properties);
   const uint32_t bind_align_B = properties.bind_align_B;

   size_B = nvkmd_switch_align_up(size_B, bind_align_B);
   align_B = MAX2(align_B, (uint64_t)bind_align_B);
   if (nvmap_id == 0 || size_B == 0 ||
       !util_is_power_of_two_nonzero64(align_B))
      return vk_error(log_obj, VK_ERROR_INVALID_EXTERNAL_HANDLE);

   struct nvkmd_switch_mem *mem = CALLOC_STRUCT(nvkmd_switch_mem);
   if (mem == NULL)
      return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);

   const struct nouveau_horizon_memory_import_info import_info = {
      .nvmap_id = nvmap_id,
      .require_existing = true,
   };
   enum nouveau_horizon_status status = nouveau_horizon_memory_import(
      dev->horizon, &import_info, &mem->memory);
   if (status != NOUVEAU_HORIZON_SUCCESS) {
      FREE(mem);
      return nvkmd_switch_status_result(log_obj, status,
                                         VK_ERROR_INVALID_EXTERNAL_HANDLE,
                                         "import NvMap memory");
   }

   struct nouveau_horizon_memory_layout layout;
   nouveau_horizon_memory_get_layout(mem->memory, &layout);
   const bool layout_valid = pte_kind != 0 || tile_mode != 0;
   const uint32_t identity_flags =
      NOUVEAU_HORIZON_MEMORY_CPU_VISIBLE |
      NOUVEAU_HORIZON_MEMORY_CPU_CACHED |
      NOUVEAU_HORIZON_MEMORY_GPU_CACHED;
   const uint32_t expected_flags =
      nvkmd_switch_memory_flags(flags) & identity_flags;
   const uint32_t actual_flags =
      nouveau_horizon_memory_get_flags(mem->memory) & identity_flags;

   if (nouveau_horizon_memory_get_size(mem->memory) != size_B ||
       layout.valid != layout_valid || layout.pte_kind != pte_kind ||
       layout.tile_mode != tile_mode || actual_flags != expected_flags) {
      nouveau_horizon_memory_put(mem->memory);
      FREE(mem);
      return vk_error(log_obj, VK_ERROR_INVALID_EXTERNAL_HANDLE);
   }

   status = nouveau_horizon_memory_map(mem->memory, &mem->cpu_addr);
   if (status != NOUVEAU_HORIZON_SUCCESS) {
      nouveau_horizon_memory_put(mem->memory);
      FREE(mem);
      return nvkmd_switch_status_result(log_obj, status,
                                         VK_ERROR_MEMORY_MAP_FAILED,
                                         "map imported NvMap memory");
   }

   nvkmd_mem_init(&dev->base, &mem->base, &nvkmd_switch_mem_ops,
                  flags, size_B, bind_align_B);

   VkResult result = nvkmd_dev_alloc_va(&dev->base, log_obj, 0, pte_kind,
                                        size_B, align_B, 0, &mem->base.va);
   if (result != VK_SUCCESS)
      goto fail_memory;

   result = nvkmd_va_bind_mem(mem->base.va, log_obj, 0, &mem->base,
                              0, size_B);
   if (result != VK_SUCCESS)
      goto fail_va;

   simple_mtx_lock(&_dev->mems_mutex);
   list_addtail(&mem->base.link, &_dev->mems);
   simple_mtx_unlock(&_dev->mems_mutex);
   *mem_out = &mem->base;
   return VK_SUCCESS;

fail_va:
   nvkmd_va_free(mem->base.va);
   mem->base.va = NULL;
fail_memory:
   simple_mtx_destroy(&mem->base.map_mutex);
   nouveau_horizon_memory_put(mem->memory);
   FREE(mem);
   return result;
}

bool
nvkmd_switch_mem_export(struct nvkmd_mem *_mem,
                        uint32_t *nvmap_id_out,
                        void **reference_out)
{
   if (_mem == NULL || nvmap_id_out == NULL || reference_out == NULL)
      return false;

   struct nouveau_horizon_memory *memory = nvkmd_switch_mem(_mem)->memory;
   const uint32_t nvmap_id =
      nouveau_horizon_memory_export_nvmap_id(memory);
   if (nvmap_id == 0)
      return false;

   *nvmap_id_out = nvmap_id;
   *reference_out = nouveau_horizon_memory_ref(memory);
   return true;
}

void
nvkmd_switch_memory_reference_release(void *reference)
{
   nouveau_horizon_memory_put(reference);
}

/* Imported host memory is GPU-mapped once at a kernel-chosen small-page VA;
 * nothing rebinds it.
 */
struct nvkmd_switch_foreign_va {
   struct nvkmd_va base;
   struct nouveau_horizon_memory *memory;
};

static void
nvkmd_switch_foreign_va_free(struct nvkmd_va *_va)
{
   struct nvkmd_switch_foreign_va *va =
      (struct nvkmd_switch_foreign_va *)_va;
   if (nouveau_horizon_memory_unmap_gpu_small(va->memory, va->base.addr) !=
       NOUVEAU_HORIZON_SUCCESS) {
      /* Keep the backing reference so the NvMap is never closed while the
       * GPU address space may still reach it.
       */
      return;
   }
   nouveau_horizon_memory_put(va->memory);
   FREE(va);
}

static VkResult
nvkmd_switch_foreign_va_bind_mem(struct nvkmd_va *_va,
                                 struct vk_object_base *log_obj,
                                 uint64_t va_offset_B,
                                 struct nvkmd_mem *_mem,
                                 uint64_t mem_offset_B, uint64_t range_B)
{
   return vk_errorf(log_obj, VK_ERROR_UNKNOWN,
                    "nvkmd-switch: imported host memory VAs are immutable");
}

static VkResult
nvkmd_switch_foreign_va_unbind(struct nvkmd_va *_va,
                               struct vk_object_base *log_obj,
                               uint64_t va_offset_B, uint64_t range_B)
{
   return vk_errorf(log_obj, VK_ERROR_UNKNOWN,
                    "nvkmd-switch: imported host memory VAs are immutable");
}

static const struct nvkmd_va_ops nvkmd_switch_foreign_va_ops = {
   .free = nvkmd_switch_foreign_va_free,
   .bind_mem = nvkmd_switch_foreign_va_bind_mem,
   .unbind = nvkmd_switch_foreign_va_unbind,
};

static VkResult
nvkmd_switch_dev_import_host_ptr(struct nvkmd_dev *_dev,
                                 struct vk_object_base *log_obj,
                                 void *host_ptr, uint64_t size_B,
                                 enum nvkmd_mem_flags flags,
                                 struct nvkmd_mem **mem_out)
{
   struct nvkmd_switch_dev *dev = nvkmd_switch_dev(_dev);

   if (host_ptr == NULL || size_B == 0 ||
       ((uintptr_t)host_ptr & 0xFFFu) != 0 || (size_B & 0xFFFu) != 0) {
      return vk_errorf(log_obj, VK_ERROR_INVALID_EXTERNAL_HANDLE,
                       "nvkmd-switch: host imports need a 4 KiB aligned "
                       "pointer and size");
   }

   struct nvkmd_switch_mem *mem = CALLOC_STRUCT(nvkmd_switch_mem);
   if (mem == NULL)
      return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);

   /* Use the same cache policy as native allocations: coherent imports must
    * turn off CPU caching, while cached imports use explicit flush/invalidate.
    */
   uint32_t horizon_flags = nvkmd_switch_memory_flags(flags);

   const struct nouveau_horizon_memory_create_info create_info = {
      .size_B = size_B,
      .align_B = 0x1000,
      .backing_kind = NvKind_Pitch,
      .flags = horizon_flags,
      .import_host_ptr = host_ptr,
   };
   enum nouveau_horizon_status status = nouveau_horizon_memory_create(
      dev->horizon, &create_info, &mem->memory);
   if (status != NOUVEAU_HORIZON_SUCCESS) {
      FREE(mem);
      return nvkmd_switch_status_result(log_obj, status,
                                        VK_ERROR_INVALID_EXTERNAL_HANDLE,
                                        "import host memory");
   }

   status = nouveau_horizon_memory_map(mem->memory, &mem->cpu_addr);
   if (status != NOUVEAU_HORIZON_SUCCESS) {
      nouveau_horizon_memory_put(mem->memory);
      FREE(mem);
      return nvkmd_switch_status_result(log_obj, status,
                                        VK_ERROR_MEMORY_MAP_FAILED,
                                        "map imported host memory");
   }

   uint64_t gpu_addr = 0;
   status = nouveau_horizon_memory_map_gpu_small(mem->memory, &gpu_addr);
   if (status != NOUVEAU_HORIZON_SUCCESS) {
      nouveau_horizon_memory_put(mem->memory);
      FREE(mem);
      return nvkmd_switch_status_result(log_obj, status,
                                        VK_ERROR_INVALID_EXTERNAL_HANDLE,
                                        "GPU-map imported host memory");
   }

   struct nvkmd_switch_foreign_va *va =
      CALLOC_STRUCT(nvkmd_switch_foreign_va);
   if (va == NULL) {
      nouveau_horizon_memory_unmap_gpu_small(mem->memory, gpu_addr);
      nouveau_horizon_memory_put(mem->memory);
      FREE(mem);
      return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   va->base.ops = &nvkmd_switch_foreign_va_ops;
   va->base.dev = _dev;
   va->base.flags = 0;
   va->base.pte_kind = 0;
   va->base.addr = gpu_addr;
   va->base.size_B = size_B;
   va->memory = nouveau_horizon_memory_ref(mem->memory);

   nvkmd_mem_init(&dev->base, &mem->base, &nvkmd_switch_mem_ops,
                  flags, size_B, 0x1000 /* pinned 4 KiB sysmem */);
   mem->base.va = &va->base;

   *mem_out = &mem->base;
   return VK_SUCCESS;
}

static VkResult
nvkmd_switch_dev_import_dma_buf(struct nvkmd_dev *_dev,
                                struct vk_object_base *log_obj,
                                int fd, struct nvkmd_mem **mem_out)
{
   return vk_errorf(log_obj, VK_ERROR_INITIALIZATION_FAILED,
                    "nvkmd-switch: dma-buf import is unsupported");
}

static VkResult
nvkmd_switch_dev_create_ctx(struct nvkmd_dev *_dev,
                            struct vk_object_base *log_obj,
                            enum nvkmd_engines engines,
                            struct nvkmd_ctx **ctx_out)
{
   struct nvkmd_switch_ctx *ctx = CALLOC_STRUCT(nvkmd_switch_ctx);
   if (ctx == NULL)
      return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);

   ctx->base.ops = &nvkmd_switch_ctx_ops;
   ctx->base.dev = _dev;
   ctx->engines = engines;
   ctx->last_fence.id = NOUVEAU_HORIZON_INVALID_FENCE_ID;

   if (engines != NVKMD_ENGINE_BIND) {
      const struct nouveau_horizon_channel_create_info create_info = {
         .priority = NOUVEAU_HORIZON_CHANNEL_PRIORITY_MEDIUM,
         .mapped_completion_mode =
            (engines & NVKMD_ENGINE_3D) &&
            debug_get_bool_option("NVK_SWITCH_MAPPED_COMPLETION", true) ?
               NOUVEAU_HORIZON_MAPPED_COMPLETION_GM20B_3D_SUBCHANNEL_0 :
               NOUVEAU_HORIZON_MAPPED_COMPLETION_DISABLED,
      };
      const enum nouveau_horizon_status status =
         nouveau_horizon_channel_create(nvkmd_switch_dev(_dev)->horizon,
                                         &create_info, &ctx->channel);
      if (status != NOUVEAU_HORIZON_SUCCESS) {
         FREE(ctx);
         return nvkmd_switch_status_result(log_obj, status,
                                            VK_ERROR_INITIALIZATION_FAILED,
                                            "create channel");
      }
   }

   *ctx_out = &ctx->base;
   return VK_SUCCESS;
}

static const struct nvkmd_dev_ops nvkmd_switch_dev_ops = {
   .destroy = nvkmd_switch_dev_destroy,
   .get_gpu_timestamp = nvkmd_switch_dev_get_gpu_timestamp,
   .get_drm_fd = nvkmd_switch_dev_get_drm_fd,
   .alloc_mem = nvkmd_switch_dev_alloc_mem,
   .alloc_tiled_mem = nvkmd_switch_dev_alloc_tiled_mem,
   .import_dma_buf = nvkmd_switch_dev_import_dma_buf,
   .import_host_ptr = nvkmd_switch_dev_import_host_ptr,
   .alloc_va = nvkmd_switch_dev_alloc_va,
   .create_ctx = nvkmd_switch_dev_create_ctx,
};

VkResult
nvkmd_switch_create_dev(struct nvkmd_pdev *pdev,
                        struct vk_object_base *log_obj,
                        struct nvkmd_dev **dev_out)
{
   struct nvkmd_switch_dev *dev = CALLOC_STRUCT(nvkmd_switch_dev);
   if (dev == NULL)
      return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);

   enum nouveau_horizon_status status =
      nouveau_horizon_runtime_get(&dev->runtime);
   if (status != NOUVEAU_HORIZON_SUCCESS) {
      FREE(dev);
      return nvkmd_switch_status_result(log_obj, status,
                                         VK_ERROR_INITIALIZATION_FAILED,
                                         "initialize Horizon runtime");
   }

   const struct nouveau_horizon_device_create_info create_info = {
      .total_order_channels = false,
      .enable_timing =
         debug_get_bool_option("NVK_SWITCH_PERF_LOG", false),
      .logger = {
         .log = nvkmd_switch_horizon_log,
         .data = log_obj,
      },
   };
   status = nouveau_horizon_device_create(dev->runtime, &create_info,
                                           &dev->horizon);
   if (status != NOUVEAU_HORIZON_SUCCESS) {
      nouveau_horizon_runtime_put(dev->runtime);
      FREE(dev);
      return nvkmd_switch_status_result(log_obj, status,
                                         VK_ERROR_INITIALIZATION_FAILED,
                                         "create Horizon device");
   }

   dev->base.ops = &nvkmd_switch_dev_ops;
   dev->base.pdev = pdev;
   list_inithead(&dev->base.mems);
   simple_mtx_init(&dev->base.mems_mutex, mtx_plain);

   struct nouveau_horizon_device_properties properties;
   nouveau_horizon_device_get_properties(dev->horizon, &properties);
   dev->base.va_start = properties.va_start;
   dev->base.va_end = properties.va_end;
   pdev->bind_align_B = properties.bind_align_B;

   *dev_out = &dev->base;
   return VK_SUCCESS;
}

uint32_t
nvkmd_switch_mem_get_nvmap_id(struct nvkmd_mem *mem)
{
   if (mem == NULL || mem->ops != &nvkmd_switch_mem_ops)
      return 0;
   return nouveau_horizon_memory_export_nvmap_id(
      nvkmd_switch_mem(mem)->memory);
}
