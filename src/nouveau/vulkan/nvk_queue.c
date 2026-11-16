/*
 * Copyright © 2022 Collabora Ltd. and Red Hat Inc.
 * SPDX-License-Identifier: MIT
 */
#include "nvk_queue.h"

#include "nvk_buffer.h"
#include "nvk_cmd_buffer.h"
#include "nvk_device.h"
#include "nvk_image.h"
#include "nvk_physical_device.h"
#include "nv_push.h"

#include "nv_push_cl9039.h"
#include "nv_push_cl9097.h"
#include "nv_push_cl90b5.h"
#include "nv_push_cla0c0.h"
#include "cla1c0.h"
#include "nv_push_clc3c0.h"
#include "nv_push_clc397.h"
#include "vk_log.h"

#ifdef __SWITCH__
#include "nvkmd/switch/nvkmd_switch.h"
#include "nv_push_clb197.h"
#include "drf.h"
#endif

#include <string.h>

static VkResult
nvk_queue_push(struct nvk_queue *queue, const struct nv_push *push);

#ifdef __SWITCH__
static void
nvk_push_zbc_state(struct nv_push *p,
                   const struct nouveau_horizon_zbc_state *state)
{
   static_assert(DRF_MASK(
                    NVB197_SET_COLOR_ZERO_BANDWIDTH_CLEAR_SLOT_DISABLE_MASK) ==
                    (UINT32_C(1) << NOUVEAU_HORIZON_ZBC_SLOT_COUNT) - 1u,
                 "Horizon and GM20B ZBC slot widths must match");
   static_assert(NVB197_SET_Z_ZERO_BANDWIDTH_CLEAR ==
                    NVB197_SET_COLOR_ZERO_BANDWIDTH_CLEAR + 4,
                 "GM20B color/depth ZBC methods must be consecutive");

   P_IMMD(p, NVB197, SET_COLOR_ZERO_BANDWIDTH_CLEAR,
          state->slot_disable_mask);
   P_IMMD(p, NVB197, SET_Z_ZERO_BANDWIDTH_CLEAR,
          state->slot_disable_mask);
}

static bool
nvk_queue_get_changed_zbc_state(
   struct nvk_queue *queue,
   struct nouveau_horizon_zbc_state *state_out)
{
   if (!(queue->engines & NVKMD_ENGINE_3D))
      return false;

   struct nvk_device *dev = nvk_queue_device(queue);
   if (queue->zbc_generation ==
       nvkmd_switch_dev_get_zbc_generation(dev->nvkmd))
      return false;

   nvkmd_switch_dev_get_zbc_state(dev->nvkmd, state_out);
   return queue->zbc_generation != state_out->generation;
}

static void
nvk_queue_commit_zbc_state(
   struct nvk_queue *queue,
   const struct nouveau_horizon_zbc_state *state)
{
   struct nvk_device *dev = nvk_queue_device(queue);
   queue->zbc_generation = state->generation;
   nvkmd_switch_dev_record_zbc_program(dev->nvkmd);
}
#endif

static void
nvk_queue_state_init(struct nvk_queue_state *qs)
{
   memset(qs, 0, sizeof(*qs));
#ifdef _WIN32
   list_inithead(&qs->retired_slm);
#endif
}

#ifdef _WIN32
struct nvk_retired_slm {
   struct list_head link;
   struct nvkmd_mem *mem;
   uint64_t time_point;
};

static void
nvk_queue_release_slm(struct nvk_device *dev, struct nvk_queue_state *qs,
                      uint64_t completed)
{
   list_for_each_entry_safe(struct nvk_retired_slm, slm, &qs->retired_slm, link) {
      if (slm->time_point > completed)
         break;

      list_del(&slm->link);
      nvkmd_mem_unref(slm->mem);
      vk_free(&dev->vk.alloc, slm);
   }
}
#endif

static void
nvk_queue_state_finish(struct nvk_device *dev,
                       struct nvk_queue_state *qs)
{
#ifdef _WIN32
   nvk_queue_release_slm(dev, qs, UINT64_MAX);
#endif
   if (qs->slm.mem)
      nvkmd_mem_unref(qs->slm.mem);
}

static VkResult
nvk_queue_state_update(struct nvk_queue *queue,
                       struct nvk_queue_state *qs)
{
   struct nvk_device *dev = nvk_queue_device(queue);
   const struct nvk_physical_device *pdev = nvk_device_physical(dev);
   struct nvkmd_mem *mem;
   uint32_t alloc_count, bytes_per_warp, bytes_per_tpc;
   bool dirty = false;
#ifdef _WIN32
   if (!list_is_empty(&qs->retired_slm)) {
      uint64_t completed;
      VkResult result = vk_sync_get_value(&dev->vk, queue->push_stream.sync,
                                          &completed);
      if (result != VK_SUCCESS)
         return result;
      nvk_queue_release_slm(dev, qs, completed);
   }
#endif
#ifdef __SWITCH__
   struct nouveau_horizon_zbc_state zbc_state;
   const bool zbc_dirty =
      nvk_queue_get_changed_zbc_state(queue, &zbc_state);
   dirty |= zbc_dirty;
#endif

   alloc_count = nvk_descriptor_table_alloc_count(&dev->images);
   if (qs->images.alloc_count != alloc_count) {
      qs->images.alloc_count = alloc_count;
      dirty = true;
   }

   alloc_count = nvk_descriptor_table_alloc_count(&dev->samplers);
   if (qs->samplers.alloc_count != alloc_count) {
      qs->samplers.alloc_count = alloc_count;
      dirty = true;
   }

   mem = nvk_slm_area_get_mem_ref(&dev->slm, &bytes_per_warp, &bytes_per_tpc);
   if (qs->slm.mem != mem || qs->slm.bytes_per_warp != bytes_per_warp ||
       qs->slm.bytes_per_tpc != bytes_per_tpc) {
      if (qs->slm.mem) {
#ifdef _WIN32
         struct nvk_retired_slm *old = vk_alloc(&dev->vk.alloc, sizeof(*old), 8,
                                                VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
         if (!old) {
            if (mem)
               nvkmd_mem_unref(mem);
            return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
         }

         /* The state push signals this point after earlier SLM users. */
         old->mem = qs->slm.mem;
         old->time_point = queue->push_stream.next_time_point;
         list_addtail(&old->link, &qs->retired_slm);
#else
         nvkmd_mem_unref(qs->slm.mem);
#endif
      }
      qs->slm.mem = mem;
      qs->slm.bytes_per_warp = bytes_per_warp;
      qs->slm.bytes_per_tpc = bytes_per_tpc;
      dirty = true;
   } else {
      /* No change */
      if (mem)
         nvkmd_mem_unref(mem);
   }

   if (!dirty)
      return VK_SUCCESS;

   /* Worst case: image/sampler pools (12 dwords each), SLM (17), Switch
    * ZBC (4): 45 total, plus growth headroom.
    */
   uint32_t push_data[80];
   static_assert(ARRAY_SIZE(push_data) >= 45,
                 "queue-state push must fit the worst-case ZBC update");
   struct nv_push push;
   nv_push_init(&push, push_data, ARRAY_SIZE(push_data),
                nvk_queue_subchannels_from_engines(queue->engines));
   struct nv_push *p = &push;

#ifdef __SWITCH__
   if (zbc_dirty)
      nvk_push_zbc_state(p, &zbc_state);
#endif

   if (qs->images.alloc_count > 0) {
      const uint64_t tex_pool_addr =
         nvk_descriptor_table_base_address(&dev->images);
      if (queue->engines & NVKMD_ENGINE_COMPUTE) {
         P_MTHD(p, NVA0C0, SET_TEX_HEADER_POOL_A);
         P_NVA0C0_SET_TEX_HEADER_POOL_A(p, tex_pool_addr >> 32);
         P_NVA0C0_SET_TEX_HEADER_POOL_B(p, tex_pool_addr);
         P_NVA0C0_SET_TEX_HEADER_POOL_C(p, qs->images.alloc_count - 1);
         P_IMMD(p, NVA0C0, INVALIDATE_TEXTURE_HEADER_CACHE_NO_WFI, {
            .lines = LINES_ALL
         });
      }

      if (queue->engines & NVKMD_ENGINE_3D) {
         P_MTHD(p, NV9097, SET_TEX_HEADER_POOL_A);
         P_NV9097_SET_TEX_HEADER_POOL_A(p, tex_pool_addr >> 32);
         P_NV9097_SET_TEX_HEADER_POOL_B(p, tex_pool_addr);
         P_NV9097_SET_TEX_HEADER_POOL_C(p, qs->images.alloc_count - 1);
         P_IMMD(p, NV9097, INVALIDATE_TEXTURE_HEADER_CACHE_NO_WFI, {
            .lines = LINES_ALL
         });
      }
   }

   if (qs->samplers.alloc_count > 0) {
      const uint64_t sampler_pool_addr =
         nvk_descriptor_table_base_address(&dev->samplers);
      if (queue->engines & NVKMD_ENGINE_COMPUTE) {
         P_MTHD(p, NVA0C0, SET_TEX_SAMPLER_POOL_A);
         P_NVA0C0_SET_TEX_SAMPLER_POOL_A(p, sampler_pool_addr >> 32);
         P_NVA0C0_SET_TEX_SAMPLER_POOL_B(p, sampler_pool_addr);
         P_NVA0C0_SET_TEX_SAMPLER_POOL_C(p, qs->samplers.alloc_count - 1);
         P_IMMD(p, NVA0C0, INVALIDATE_SAMPLER_CACHE_NO_WFI, {
            .lines = LINES_ALL
         });
      }

      if (queue->engines & NVKMD_ENGINE_3D) {
         P_MTHD(p, NV9097, SET_TEX_SAMPLER_POOL_A);
         P_NV9097_SET_TEX_SAMPLER_POOL_A(p, sampler_pool_addr >> 32);
         P_NV9097_SET_TEX_SAMPLER_POOL_B(p, sampler_pool_addr);
         P_NV9097_SET_TEX_SAMPLER_POOL_C(p, qs->samplers.alloc_count - 1);
         P_IMMD(p, NV9097, INVALIDATE_SAMPLER_CACHE_NO_WFI, {
            .lines = LINES_ALL
         });
      }
   }

   if (qs->slm.mem) {
      const uint64_t slm_addr = qs->slm.mem->va->addr;
      const uint64_t slm_size = qs->slm.mem->size_B;
      const uint64_t slm_per_warp = qs->slm.bytes_per_warp;
      const uint64_t slm_per_tpc = qs->slm.bytes_per_tpc;
      assert(!(slm_per_tpc & 0x7fff));

      if (queue->engines & NVKMD_ENGINE_COMPUTE) {
         P_MTHD(p, NVA0C0, SET_SHADER_LOCAL_MEMORY_A);
         P_NVA0C0_SET_SHADER_LOCAL_MEMORY_A(p, slm_addr >> 32);
         P_NVA0C0_SET_SHADER_LOCAL_MEMORY_B(p, slm_addr);

         P_MTHD(p, NVA0C0, SET_SHADER_LOCAL_MEMORY_NON_THROTTLED_A);
         P_NVA0C0_SET_SHADER_LOCAL_MEMORY_NON_THROTTLED_A(p, slm_per_tpc >> 32);
         P_NVA0C0_SET_SHADER_LOCAL_MEMORY_NON_THROTTLED_B(p, slm_per_tpc);
         P_NVA0C0_SET_SHADER_LOCAL_MEMORY_NON_THROTTLED_C(p, 0xff);

         if (pdev->info.cls_compute < VOLTA_COMPUTE_A) {
            P_MTHD(p, NVA0C0, SET_SHADER_LOCAL_MEMORY_THROTTLED_A);
            P_NVA0C0_SET_SHADER_LOCAL_MEMORY_THROTTLED_A(p, slm_per_tpc >> 32);
            P_NVA0C0_SET_SHADER_LOCAL_MEMORY_THROTTLED_B(p, slm_per_tpc);
            P_NVA0C0_SET_SHADER_LOCAL_MEMORY_THROTTLED_C(p, 0xff);
         }
      }

      if (queue->engines & NVKMD_ENGINE_3D) {
         P_MTHD(p, NV9097, SET_SHADER_LOCAL_MEMORY_A);
         P_NV9097_SET_SHADER_LOCAL_MEMORY_A(p, slm_addr >> 32);
         P_NV9097_SET_SHADER_LOCAL_MEMORY_B(p, slm_addr);
         P_NV9097_SET_SHADER_LOCAL_MEMORY_C(p, slm_size >> 32);
         P_NV9097_SET_SHADER_LOCAL_MEMORY_D(p, slm_size);
         P_NV9097_SET_SHADER_LOCAL_MEMORY_E(p, slm_per_warp);
      }
   }

   const VkResult result = nvk_queue_push(queue, p);
#ifdef __SWITCH__
   if (result == VK_SUCCESS && zbc_dirty)
      nvk_queue_commit_zbc_state(queue, &zbc_state);
#endif
   return result;
}

static VkResult
nvk_queue_submit_bind(struct nvk_queue *queue,
                      struct vk_queue_submit *submit)
{
   VkResult result;

   result = nvkmd_ctx_wait(queue->bind_ctx, &queue->vk.base,
                           submit->wait_count, submit->waits);
   if (result != VK_SUCCESS)
      return result;

   for (uint32_t i = 0; i < submit->buffer_bind_count; i++) {
      result = nvk_queue_buffer_bind(queue, &submit->buffer_binds[i]);
      if (result != VK_SUCCESS)
         return result;
   }

   for (uint32_t i = 0; i < submit->image_bind_count; i++) {
      result = nvk_queue_image_bind(queue, &submit->image_binds[i]);
      if (result != VK_SUCCESS)
         return result;
   }

   for (uint32_t i = 0; i < submit->image_opaque_bind_count; i++) {
      result = nvk_queue_image_opaque_bind(queue, &submit->image_opaque_binds[i]);
      if (result != VK_SUCCESS)
         return result;
   }

   result = nvkmd_ctx_signal(queue->bind_ctx, &queue->vk.base,
                             submit->signal_count, submit->signals);
   if (result != VK_SUCCESS)
      return result;

   return VK_SUCCESS;
}

#ifdef __SWITCH__
#define NVK_QUEUE_GPFIFO_MAX_COMMANDS 0x1fffffU
#define NVK_QUEUE_GPFIFO_MAX_SIZE_B \
   (NVK_QUEUE_GPFIFO_MAX_COMMANDS * sizeof(uint32_t))
#define NVK_QUEUE_EXEC_BATCH_SIZE 0x800

static bool
nvk_queue_try_merge_exec(struct nvkmd_ctx_exec *last,
                         const struct nvk_cmd_push *push)
{
   if (push->no_prefetch ||
       last->addr > UINT64_MAX - last->size_B ||
       last->addr + last->size_B != push->addr ||
       last->size_B > NVK_QUEUE_GPFIFO_MAX_SIZE_B ||
       push->range > NVK_QUEUE_GPFIFO_MAX_SIZE_B - last->size_B)
      return false;

   last->size_B += push->range;
   last->incomplete = push->incomplete;
   return true;
}

static VkResult
nvk_queue_submit_execs(struct nvk_queue *queue,
                       uint32_t exec_count,
                       const struct nvkmd_ctx_exec *execs);

static VkResult
nvk_queue_append_exec(struct nvkmd_ctx_exec *execs,
                      uint32_t *exec_count,
                      const struct nvk_cmd_push *push,
                      struct nvk_queue *queue)
{
   if (*exec_count > 0 &&
       nvk_queue_try_merge_exec(&execs[*exec_count - 1], push))
      return VK_SUCCESS;

   if (*exec_count == NVK_QUEUE_EXEC_BATCH_SIZE) {
      uint32_t flush_count = *exec_count - 1;
      while (flush_count > 0 && execs[flush_count - 1].incomplete)
         flush_count--;

      if (flush_count == 0 && !execs[*exec_count - 1].incomplete)
         flush_count = *exec_count;

      if (flush_count == 0) {
         return vk_errorf(queue, VK_ERROR_DEVICE_LOST,
                          "Unable to split an incomplete GPFIFO chain");
      }

      VkResult result = nvk_queue_submit_execs(queue, flush_count, execs);
      if (result != VK_SUCCESS)
         return result;

      *exec_count -= flush_count;
      memmove(execs, &execs[flush_count],
              *exec_count * sizeof(*execs));
   }

   execs[(*exec_count)++] = (struct nvkmd_ctx_exec) {
      .addr = push->addr,
      .size_B = push->range,
      .incomplete = push->incomplete,
      .no_prefetch = push->no_prefetch,
   };
   return VK_SUCCESS;
}

static VkResult
nvk_queue_submit_execs(struct nvk_queue *queue,
                       uint32_t exec_count,
                       const struct nvkmd_ctx_exec *execs)
{
   if (exec_count == 0)
      return VK_SUCCESS;

   return nvkmd_ctx_exec(queue->exec_ctx, &queue->vk.base, exec_count, execs);
}

static VkResult
nvk_queue_submit_cmd_buffers(struct nvk_queue *queue,
                             const struct vk_queue_submit *submit)
{
   if (submit->command_buffer_count == 0)
      return VK_SUCCESS;

   struct nvkmd_ctx_exec *execs = queue->submit_execs;
   assert(execs != NULL);
   VkResult result = VK_SUCCESS;
   uint32_t exec_count = 0;

   for (uint32_t i = 0; i < submit->command_buffer_count; i++) {
      struct nvk_cmd_buffer *cmd =
         container_of(submit->command_buffers[i], struct nvk_cmd_buffer, vk);
      const struct nvk_cmd_push *pushes = util_dynarray_begin(&cmd->pushes);
      const uint32_t push_count =
         util_dynarray_num_elements(&cmd->pushes, struct nvk_cmd_push);

      for (uint32_t j = 0; j < push_count; ) {
         const struct nvk_cmd_push *push = &pushes[j];

         if (push->range == 0)
            goto next_push;

         result = nvk_queue_append_exec(execs, &exec_count, push, queue);
         if (result != VK_SUCCESS)
            return result;

next_push:
         j++;
      }
   }

   result = nvk_queue_submit_execs(queue, exec_count, execs);
   return result;
}
#endif

static VkResult
nvk_queue_submit_exec(struct nvk_queue *queue,
                      struct vk_queue_submit *submit)
{
   struct nvk_device *dev = nvk_queue_device(queue);
   VkResult result;
   if (submit->command_buffer_count > 0) {
      assert(dev->event_heap.arena.mem_flags & NVKMD_MEM_COHERENT);

      result = nvk_queue_state_update(queue, &queue->state);
      if (result != VK_SUCCESS)
         goto fail;

      uint64_t upload_time_point;
      result = nvk_upload_queue_flush(dev, &dev->upload, &upload_time_point);
      if (result != VK_SUCCESS)
         goto fail;

      if (upload_time_point > 0) {
         struct vk_sync_wait wait = {
            .sync = dev->upload.stream.sync,
            .stage_mask = ~0,
            .wait_value = upload_time_point,
         };
         result = nvkmd_ctx_wait(queue->exec_ctx, &queue->vk.base, 1, &wait);
         if (result != VK_SUCCESS)
            goto fail;
      }
   }

   result = nvkmd_ctx_wait(queue->exec_ctx, &queue->vk.base,
                           submit->wait_count, submit->waits);
   if (result != VK_SUCCESS)
      goto fail;

#ifdef __SWITCH__
   result = nvk_queue_submit_cmd_buffers(queue, submit);
   if (result != VK_SUCCESS)
      goto fail;
#else
   for (unsigned i = 0; i < submit->command_buffer_count; i++) {
      struct nvk_cmd_buffer *cmd =
         container_of(submit->command_buffers[i], struct nvk_cmd_buffer, vk);
      const uint32_t max_execs =
         util_dynarray_num_elements(&cmd->pushes, struct nvk_cmd_push);
      STACK_ARRAY(struct nvkmd_ctx_exec, execs, max_execs);
      uint32_t exec_count = 0;

      util_dynarray_foreach(&cmd->pushes, struct nvk_cmd_push, push) {
         if (push->range == 0)
            continue;

         execs[exec_count++] = (struct nvkmd_ctx_exec) {
            .addr = push->addr,
            .size_B = push->range,
            .incomplete = push->incomplete,
            .no_prefetch = push->no_prefetch,
#ifdef _WIN32
            .written_count = submit->win32_written_count,
            .written_allocations = submit->win32_written_allocations,
#endif
         };
      }

      result = nvkmd_ctx_exec(queue->exec_ctx, &queue->vk.base,
                              exec_count, execs);

      STACK_ARRAY_FINISH(execs);

      if (result != VK_SUCCESS)
         goto fail;
   }
#endif

   result = nvkmd_ctx_signal(queue->exec_ctx, &queue->vk.base,
                             submit->signal_count, submit->signals);
   if (result != VK_SUCCESS)
      goto fail;

fail:
   return result;
}

static VkResult
nvk_queue_submit(struct vk_queue *vk_queue,
                 struct vk_queue_submit *submit)
{
   struct nvk_queue *queue = container_of(vk_queue, struct nvk_queue, vk);
   VkResult result;

   if (vk_queue_is_lost(&queue->vk))
      return VK_ERROR_DEVICE_LOST;

   if (submit->buffer_bind_count > 0 ||
       submit->image_bind_count > 0  ||
       submit->image_opaque_bind_count > 0) {
      assert(submit->command_buffer_count == 0);
      result = nvk_queue_submit_bind(queue, submit);
      if (result != VK_SUCCESS)
         return vk_queue_set_lost(&queue->vk, "Bind operation failed");
   } else {
      result = nvk_queue_submit_exec(queue, submit);
      if (result != VK_SUCCESS)
         return vk_queue_set_lost(&queue->vk, "Submit failed");
   }

   return VK_SUCCESS;
}

static VkResult
nvk_queue_push(struct nvk_queue *queue, const struct nv_push *push)
{
   struct nvk_device *dev = nvk_queue_device(queue);

   if (vk_queue_is_lost(&queue->vk))
      return VK_ERROR_DEVICE_LOST;

   if (nv_push_dw_count(push) == 0)
      return VK_SUCCESS;

   return nvk_mem_stream_push(dev, &queue->push_stream, queue->exec_ctx,
                              push->start, nv_push_dw_count(push), NULL);
}

static VkResult
nvk_queue_init_context_state(struct nvk_queue *queue)
{
   struct nvk_device *dev = nvk_queue_device(queue);
   const struct nvk_physical_device *pdev = nvk_device_physical(dev);
   VkResult result;
#ifdef __SWITCH__
   struct nouveau_horizon_zbc_state zbc_state;
   bool zbc_programmed = false;
#endif

   uint32_t push_data[4096 + 1024];
   struct nv_push push;
   nv_push_init(&push, push_data, ARRAY_SIZE(push_data),
                nvk_queue_subchannels_from_engines(queue->engines));
   struct nv_push *p = &push;

#ifdef _WIN32
   if (queue->engines & NVKMD_ENGINE_COPY)
      nvk_push_bind_copy(p, pdev->info.cls_copy);
#endif

   /* M2MF state */
   if (pdev->info.cls_m2mf <= FERMI_MEMORY_TO_MEMORY_FORMAT_A) {
      /* we absolutely do not support Fermi, but if somebody wants to toy
       * around with it, this is a must
       */
      P_MTHD(p, NV9039, SET_OBJECT);
      P_NV9039_SET_OBJECT(p, {
         .class_id = pdev->info.cls_m2mf,
         .engine_id = 0,
      });
   }

   if (queue->engines & NVKMD_ENGINE_3D) {
      result = nvk_push_draw_state_init(queue, p);
      if (result != VK_SUCCESS)
         return result;

#ifdef __SWITCH__
      nvkmd_switch_dev_get_zbc_state(dev->nvkmd, &zbc_state);
      nvk_push_zbc_state(p, &zbc_state);
      zbc_programmed = true;
#endif
   }

   if (queue->engines & NVKMD_ENGINE_COMPUTE) {
      result = nvk_push_dispatch_state_init(queue, p);
      if (result != VK_SUCCESS)
         return result;
   }

   result = nvk_queue_push(queue, &push);
#ifdef _WIN32
   /* The KMD admits later MME calls after this context's upload retires. */
   if (result == VK_SUCCESS && (queue->engines & NVKMD_ENGINE_3D))
      result = nvk_mem_stream_sync(dev, &queue->push_stream, queue->exec_ctx);
#endif
#ifdef __SWITCH__
   if (result == VK_SUCCESS && zbc_programmed)
      nvk_queue_commit_zbc_state(queue, &zbc_state);
#endif
   return result;
}

static VkQueueGlobalPriority
get_queue_global_priority(const VkDeviceQueueCreateInfo *pCreateInfo)
{
   const VkDeviceQueueGlobalPriorityCreateInfo *priority_info =
      vk_find_struct_const(pCreateInfo->pNext,
                           DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO);
   if (priority_info == NULL)
      return VK_QUEUE_GLOBAL_PRIORITY_MEDIUM;

   return priority_info->globalPriority;
}

VkResult
nvk_queue_create(struct nvk_device *dev,
                 const VkDeviceQueueCreateInfo *pCreateInfo,
                 uint32_t index_in_family)
{
   const struct nvk_physical_device *pdev = nvk_device_physical(dev);
   VkResult result;

   assert(pCreateInfo->queueFamilyIndex < pdev->queue_family_count);
   const struct nvk_queue_family *queue_family =
      &pdev->queue_families[pCreateInfo->queueFamilyIndex];

   const VkQueueGlobalPriority global_priority =
      get_queue_global_priority(pCreateInfo);

   /* From the Vulkan 1.3.295 spec:
    *
    *    "If the globalPriorityQuery feature is enabled and the requested
    *    global priority is not reported via
    *    VkQueueFamilyGlobalPriorityPropertiesKHR, the driver implementation
    *    must fail the queue creation. In this scenario,
    *    VK_ERROR_INITIALIZATION_FAILED is returned."
    */
   if (dev->vk.enabled_features.globalPriorityQuery &&
       global_priority != VK_QUEUE_GLOBAL_PRIORITY_MEDIUM)
      return VK_ERROR_INITIALIZATION_FAILED;

   if (global_priority > VK_QUEUE_GLOBAL_PRIORITY_MEDIUM)
      return VK_ERROR_NOT_PERMITTED;

   struct nvk_queue *queue = vk_zalloc(&dev->vk.alloc, sizeof(struct nvk_queue),
                                       8, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!queue)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   result = vk_queue_init(&queue->vk, &dev->vk, pCreateInfo, index_in_family);
   if (result != VK_SUCCESS)
      goto fail_alloc;
#ifdef _WIN32
   queue->vk.supports_wsi_written_primaries = true;
#endif

   nvk_queue_state_init(&queue->state);

   queue->engines =
      nvk_queue_engines_from_queue_flags(queue_family->queue_flags);

#ifdef __SWITCH__
   if (queue->engines) {
      queue->submit_execs =
         vk_alloc(&dev->vk.alloc,
                  NVK_QUEUE_EXEC_BATCH_SIZE * sizeof(*queue->submit_execs),
                  8, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
      if (queue->submit_execs == NULL) {
         result = VK_ERROR_OUT_OF_HOST_MEMORY;
         goto fail_init;
      }
   }
#endif

   if (queue->engines) {
      result = nvkmd_dev_create_ctx(dev->nvkmd, &dev->vk.base,
                                    queue->engines, &queue->exec_ctx);
      if (result != VK_SUCCESS)
         goto fail_init;

      result = nvkmd_dev_alloc_mem(dev->nvkmd, &dev->vk.base,
                                   4096, 0, NVKMD_MEM_LOCAL,
                                   &queue->draw_cb0);
      if (result != VK_SUCCESS)
         goto fail_exec_ctx;
   }

   if (queue_family->queue_flags & VK_QUEUE_SPARSE_BINDING_BIT) {
      result = nvkmd_dev_create_ctx(dev->nvkmd, &dev->vk.base,
                                    NVKMD_ENGINE_BIND, &queue->bind_ctx);
      if (result != VK_SUCCESS)
         goto fail_draw_cb0;
   }

   result = nvk_mem_stream_init(dev, &queue->push_stream);
   if (result != VK_SUCCESS)
      goto fail_bind_ctx;

   result = nvk_queue_init_context_state(queue);
   if (result != VK_SUCCESS)
      goto fail_push_stream;

   queue->vk.driver_submit = nvk_queue_submit;

   return VK_SUCCESS;

fail_push_stream:
#ifdef __SWITCH__
   /* This path may have submitted the context-state push.  If completion is
    * uncertain, nvk_queue_try_destroy() deliberately leaves the queue object
    * and its embedded stream/list bookkeeping intact for device quarantine.
    */
   (void)nvk_queue_try_destroy(dev, queue);
   return result;
#else
   nvk_mem_stream_sync(dev, &queue->push_stream, queue->exec_ctx);
   nvk_mem_stream_finish(dev, &queue->push_stream);
#endif
fail_bind_ctx:
   if (queue->bind_ctx != NULL)
      nvkmd_ctx_destroy(queue->bind_ctx);
fail_draw_cb0:
   if (queue->draw_cb0 != NULL)
      nvkmd_mem_unref(queue->draw_cb0);
fail_exec_ctx:
   if (queue->exec_ctx != NULL)
      nvkmd_ctx_destroy(queue->exec_ctx);
fail_init:
#ifdef __SWITCH__
   vk_free(&dev->vk.alloc, queue->submit_execs);
#endif
   nvk_queue_state_finish(dev, &queue->state);
   vk_queue_finish(&queue->vk);
fail_alloc:
   vk_free(&dev->vk.alloc, queue);

   return result;
}

#ifdef __SWITCH__
static bool
nvk_queue_quarantine_teardown(struct nvk_queue *queue,
                              const char *operation, VkResult result)
{
   queue->teardown_quarantined = true;
   vk_loge(VK_LOG_OBJS(&queue->vk.base),
           "nvk-switch: %s failed during queue teardown (VkResult %d); "
           "quarantining the queue, push stream, contexts, and GPU-visible "
           "ownership graph",
           operation, result);
   return false;
}

bool
nvk_queue_try_destroy(struct nvk_device *dev, struct nvk_queue *queue)
{
   if (queue->teardown_quarantined)
      return false;

   /* Every successfully initialized queue has a push stream.  Engine-less
    * queue families have no execution context and never enqueue stream work;
    * skip the context-backed completion operation in that degenerate case.
    */
   if (queue->exec_ctx == NULL) {
      assert(!queue->push_stream.needs_flush);
      nvk_mem_stream_finish(dev, &queue->push_stream);
      nvk_queue_state_finish(dev, &queue->state);
      if (queue->bind_ctx != NULL) {
         nvkmd_ctx_destroy(queue->bind_ctx);
         queue->bind_ctx = NULL;
      }
      vk_free(&dev->vk.alloc, queue->submit_execs);
      vk_queue_finish(&queue->vk);
      vk_free(&dev->vk.alloc, queue);
      return true;
   }

   /* Wait for this signal after all prior submissions before releasing
    * queue resources. push_stream owns command BOs and the recycle-list
    * links retaining them.
    */
   VkResult result =
      nvk_mem_stream_sync(dev, &queue->push_stream, queue->exec_ctx);
   if (result != VK_SUCCESS) {
      return nvk_queue_quarantine_teardown(queue,
                                            "push-stream completion wait",
                                            result);
   }

   /* draw_cb0 can still be the destination of the independent upload channel
    * even after this queue is idle.  Preserve it (and the containing device)
    * if upload completion is unknown.
    */
   if (queue->draw_cb0 != NULL) {
      result = nvk_upload_queue_sync(dev, &dev->upload);
      if (result != VK_SUCCESS) {
         return nvk_queue_quarantine_teardown(queue,
                                               "upload completion wait",
                                               result);
      }
   }

   /* Keep command storage alive through the checked channel put. If
    * completion is unknown, retain the queue and quarantine the containing
    * device.
    */
   if (queue->exec_ctx != NULL &&
       !nvkmd_switch_ctx_try_destroy(queue->exec_ctx, &queue->vk.base)) {
      return nvk_queue_quarantine_teardown(queue,
                                            "native context release",
                                            VK_ERROR_DEVICE_LOST);
   }
   queue->exec_ctx = NULL;

   nvk_mem_stream_finish(dev, &queue->push_stream);
   if (queue->draw_cb0 != NULL) {
      nvkmd_mem_unref(queue->draw_cb0);
      queue->draw_cb0 = NULL;
   }
   nvk_queue_state_finish(dev, &queue->state);
   if (queue->bind_ctx != NULL) {
      nvkmd_ctx_destroy(queue->bind_ctx);
      queue->bind_ctx = NULL;
   }
   vk_free(&dev->vk.alloc, queue->submit_execs);
   vk_queue_finish(&queue->vk);
   vk_free(&dev->vk.alloc, queue);
   return true;
}
#endif

void
nvk_queue_destroy(struct nvk_device *dev, struct nvk_queue *queue)
{
#ifdef __SWITCH__
   (void)nvk_queue_try_destroy(dev, queue);
#else
   nvk_mem_stream_sync(dev, &queue->push_stream, queue->exec_ctx);
   nvk_mem_stream_finish(dev, &queue->push_stream);
   if (queue->draw_cb0 != NULL) {
      nvk_upload_queue_sync(dev, &dev->upload);
      nvkmd_mem_unref(queue->draw_cb0);
   }
   if (queue->bind_ctx != NULL)
      nvkmd_ctx_destroy(queue->bind_ctx);
   if (queue->exec_ctx != NULL)
      nvkmd_ctx_destroy(queue->exec_ctx);
   nvk_queue_state_finish(dev, &queue->state);
   vk_queue_finish(&queue->vk);
   vk_free(&dev->vk.alloc, queue);
#endif
}
