/* SPDX-License-Identifier: MIT
 *
 * Command recording, submission and the render-queue fence.
 *
 * NVK and the D3D runtime submit through different kernel contexts. Two
 * imported monitored fences bracket every submission: runtime signal ->
 * Vulkan wait/render/signal -> runtime wait. A separate wait on the prior
 * Vulkan completion value supplies memory ordering across submissions.
 * Polling/DO_NOT_WAIT submissions never apply CPU backpressure; ordinary
 * Flush/Present may wait once two submissions are outstanding.
 */
#include "tgd3d.h"
#include <winternl.h>

namespace tgd3d {

enum { TGD3D_MAX_IN_FLIGHT = 2 };

typedef NTSTATUS(APIENTRY *PFN_ShareObjects)(UINT, const D3DKMT_HANDLE *, void *, DWORD, HANDLE *);

void Device::setError(HRESULT hr, const char *file, int line)
{
   trace_error(hr, file, (unsigned)line);
   /* Keep the original failure in the trace. Draw and DestroyResource allow
    * only device removal; runtime treats OOM/invalid-argument and other errors
    * from these DDIs as critical driver errors. Create/Open keep their own
    * recoverable error rules outside this scope. */
   if (removalOnlyErrors && FAILED(hr))
      hr = D3DDDIERR_DEVICEREMOVED;
   if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == D3DDDIERR_DEVICEREMOVED ||
       hr == DXGI_ERROR_DEVICE_RESET) {
      lost = hr;
      /* SetErrorCb consumes D3D DDI errors, not DXGI API return values.
       * The runtime treats DXGI_ERROR_DEVICE_REMOVED/RESET as critical
       * driver errors here instead of the documented removal notification. */
      hr = D3DDDIERR_DEVICEREMOVED;
   }
   if (um.pfnSetErrorCb)
      um.pfnSetErrorCb(hRTCoreLayer, hr);
}

uint64_t Device::completed()
{
   uint64_t value = 0;
   if (FAILED(lost) || !fence || !vk.GetSemaphoreCounterValue)
      return 0;
   if (vk.GetSemaphoreCounterValue(vkdev, fence, &value) != VK_SUCCESS) {
      TG_ERROR(this, DXGI_ERROR_DEVICE_REMOVED);
      return 0;
   }
   return value;
}

bool sequence_done(Device *dev, uint64_t seq)
{
   return seq == 0 || dev->completed() >= seq;
}

/* ------------------------------------------------------------------ */

HRESULT sync_create(Device *dev)
{
   VkSemaphoreTypeCreateInfo type = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
   type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
   VkSemaphoreCreateInfo info = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &type};
   if (dev->vk.CreateSemaphore(dev->vkdev, &info, nullptr, &dev->fence) != VK_SUCCESS)
      return E_OUTOFMEMORY;

#ifdef TGD3D_CPU_SYNC_CONTROL
   /* Diagnostic build: reproduce the old desktop UMD's synchronous Flush /
    * Present boundary. Keep the local Vulkan completion timeline, but create
    * neither runtime fence nor Vulkan import of a runtime fence. */
   return S_OK;
#endif
   /* The generic control build has no WDDM queue to synchronize. */
   if (!dev->nvk)
      return S_OK;
   if (!dev->kt.pfnCreateSynchronizationObject2Cb || !dev->kt.pfnDestroySynchronizationObjectCb ||
       !dev->kt.pfnWaitForSynchronizationObjectFromGpuCb ||
       !dev->kt.pfnSignalSynchronizationObjectFromGpuCb)
      return DXGI_ERROR_UNSUPPORTED;
   if (dev->vk.CreateSemaphore(dev->vkdev, &info, nullptr, &dev->ingress) != VK_SUCCESS)
      return E_OUTOFMEMORY;

   HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
   auto share = gdi ? reinterpret_cast<PFN_ShareObjects>(
      reinterpret_cast<void *>(GetProcAddress(gdi, "D3DKMTShareObjects"))) : nullptr;
   if (!share)
      return DXGI_ERROR_UNSUPPORTED;
   for (unsigned i = 0; i < 2; i++) {
      D3DDDICB_CREATESYNCHRONIZATIONOBJECT2 create = {};
      create.Info.Type = D3DDDI_MONITORED_FENCE;
      create.Info.Flags.Shared = 1;
      create.Info.Flags.NtSecuritySharing = 1;
      create.Info.MonitoredFence.EngineAffinity = 1;
      HRESULT hr = dev->kt.pfnCreateSynchronizationObject2Cb(dev->hRTDevice.handle, &create);
      D3DKMT_HANDLE &object = i ? dev->runtimeIngress : dev->runtimeFence;
      object = create.hSyncObject;
      trace(TGD3D_TRACE_RUNTIME_CONTEXT, hr, 3, object != 0);
      if (FAILED(hr))
         return hr;
      if (!object || !create.Info.MonitoredFence.FenceValueCPUVirtualAddress)
         return E_FAIL;
      OBJECT_ATTRIBUTES attributes = {};
      attributes.Length = sizeof(attributes);
      HANDLE handle = nullptr;
      NTSTATUS status = share(1, &object, &attributes, D3DDDI_SYNC_OBJECT_ALL_ACCESS, &handle);
      if (status < 0 || !handle) {
         if (handle) CloseHandle(handle);
         return status < 0 ? HRESULT_FROM_NT(status) : E_FAIL;
      }
      VkImportSemaphoreWin32HandleInfoKHR import = {
         VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR};
      import.semaphore = i ? dev->ingress : dev->fence;
      import.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
      import.handle = handle;
      VkResult result = dev->vk.ImportSemaphoreWin32HandleKHR(dev->vkdev, &import);
      CloseHandle(handle);
      if (result != VK_SUCCESS)
         return E_FAIL;
      if (!i)
         dev->runtimeFenceValue = static_cast<const volatile UINT64 *>(
            create.Info.MonitoredFence.FenceValueCPUVirtualAddress);
   }
   return S_OK;
}

void sync_destroy(Device *dev)
{
   /* Close Vulkan imports before their runtime objects. */
   if (dev->ingress)
      dev->vk.DestroySemaphore(dev->vkdev, dev->ingress, nullptr);
   if (dev->fence)
      dev->vk.DestroySemaphore(dev->vkdev, dev->fence, nullptr);
   dev->ingress = dev->fence = VK_NULL_HANDLE;
   dev->runtimeFenceValue = nullptr;
   for (D3DKMT_HANDLE object : {dev->runtimeIngress, dev->runtimeFence}) {
      if (object && dev->kt.pfnDestroySynchronizationObjectCb) {
         D3DDDICB_DESTROYSYNCHRONIZATIONOBJECT destroy = {};
         destroy.hSyncObject = object;
         dev->kt.pfnDestroySynchronizationObjectCb(dev->hRTDevice.handle, &destroy);
      }
   }
   dev->runtimeIngress = dev->runtimeFence = 0;
}

HRESULT cmd_create(Device *dev)
{
   VkCommandPoolCreateInfo info = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
   info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
   info.queueFamilyIndex = dev->caps->queueFamily;
   return dev->vk.CreateCommandPool(dev->vkdev, &info, nullptr, &dev->cmdPool) == VK_SUCCESS ?
      S_OK : E_OUTOFMEMORY;
}

void cmd_destroy(Device *dev)
{
   for (auto pool : dev->framePools)
      dev->vk.DestroyDescriptorPool(dev->vkdev, pool, nullptr);
   for (auto pool : dev->freePools)
      dev->vk.DestroyDescriptorPool(dev->vkdev, pool, nullptr);
   for (auto &submitted : dev->submittedCommands)
      for (auto pool : submitted.pools)
         dev->vk.DestroyDescriptorPool(dev->vkdev, pool, nullptr);
   dev->framePools.clear();
   dev->freePools.clear();
   dev->submittedCommands.clear();
   dev->sharedImages.clear();
   if (dev->cmdPool)
      dev->vk.DestroyCommandPool(dev->vkdev, dev->cmdPool, nullptr);
   dev->cmdPool = VK_NULL_HANDLE;
   dev->cmd = VK_NULL_HANDLE;
}

/* ------------------------------------------------------------------ */

static VkCommandBuffer ensure_cmd(Device *dev)
{
   if (FAILED(dev->lost))
      return VK_NULL_HANDLE;
   if (dev->cmd)
      return dev->cmd;
   VkCommandBuffer cmd = VK_NULL_HANDLE;
   if (!dev->freeCommands.empty()) {
      cmd = dev->freeCommands.back();
      dev->freeCommands.pop_back();
   } else {
      VkCommandBufferAllocateInfo info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
      info.commandPool = dev->cmdPool;
      info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      info.commandBufferCount = 1;
      VkResult result = dev->vk.AllocateCommandBuffers(dev->vkdev, &info, &cmd);
      if (result != VK_SUCCESS) {
         /* QueryEnd (timestamps), Draw and Flush cannot report OOM. The
          * missing command cannot be skipped while keeping the device valid. */
         trace_error(resource_result(result), __FILE__, __LINE__);
         TG_ERROR(dev, D3DDDIERR_DEVICEREMOVED);
         return VK_NULL_HANDLE;
      }
   }
   VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
   begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
   if (dev->vk.BeginCommandBuffer(cmd, &begin) != VK_SUCCESS) {
      dev->freeCommands.push_back(cmd);
      TG_ERROR(dev, DXGI_ERROR_DEVICE_REMOVED);
      return VK_NULL_HANDLE;
   }
   dev->cmd = cmd;
   dev->cmdHasWork = false;
   /* A new command buffer inherits no state. */
   dev->dirty = DIRTY_ALL;
   for (auto &stage : dev->stage) {
      stage.cbDirty = ~0u;
      stage.bound = VK_NULL_HANDLE;
   }
   dev->textureSet = dev->texelBufferSet = VK_NULL_HANDLE;
   /* Each submit waits for the preceding completion timeline value. */
   dev->barrierSerial = dev->writeSerial;
   dev->firstCommandQpc = trace_timing_enabled() ? qpc_now() : 0;
   return cmd;
}

void end_rendering(Device *dev)
{
   if (!dev->rendering)
      return;
   query_pass_end(dev, dev->cmd);
   dev->vk.CmdEndRendering(dev->cmd);
   dev->rendering = false;
}

VkCommandBuffer cmd_draw(Device *dev)
{
   VkCommandBuffer cmd = ensure_cmd(dev);
   if (cmd)
      dev->cmdHasWork = true;
   return cmd;
}

VkCommandBuffer cmd_begin(Device *dev)
{
   end_rendering(dev);
   flush_pending_clears(dev, nullptr);
   VkCommandBuffer cmd = ensure_cmd(dev);
   if (!cmd)
      return VK_NULL_HANDLE;
   dev->cmdHasWork = true;
   command_barrier(dev);
   return cmd;
}

/* D3D's implicit ordering covers RAW, WAR and WAW hazards, including
 * attachment LOAD across separate dynamic rendering instances. */
void command_barrier(Device *dev)
{
   end_rendering(dev);
   VkCommandBuffer cmd = ensure_cmd(dev);
   if (!cmd)
      return;
   VkMemoryBarrier2 barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
   barrier.srcStageMask = barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
   barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
   barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
   VkDependencyInfo info = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
   info.memoryBarrierCount = 1;
   info.pMemoryBarriers = &barrier;
   dev->vk.CmdPipelineBarrier2(cmd, &info);
   dev->cmdHasWork = true;
   dev->barrierSerial = dev->writeSerial;
}

void image_use(Device *dev, ImageStorage &storage, bool write)
{
   storage.lastUse = dev->recordingSeq();
   if (storage.externalShared && storage.acquireSeq != dev->recordingSeq()) {
      end_rendering(dev);
      VkCommandBuffer cmd = ensure_cmd(dev);
      if (!cmd) return;
      try {
         dev->sharedImages.push_back(storage.image);
      } catch (const std::bad_alloc &) {
         TG_ERROR(dev, DXGI_ERROR_DEVICE_REMOVED); return;
      }
      VkImageMemoryBarrier2 barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
      barrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
      barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
      barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
      barrier.oldLayout = storage.layoutReady ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
      barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
      barrier.srcQueueFamilyIndex = storage.layoutReady ? VK_QUEUE_FAMILY_EXTERNAL : VK_QUEUE_FAMILY_IGNORED;
      barrier.dstQueueFamilyIndex = storage.layoutReady ? dev->caps->queueFamily : VK_QUEUE_FAMILY_IGNORED;
      barrier.image = storage.image;
      barrier.subresourceRange = {storage.aspects, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
      VkDependencyInfo info = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
      info.imageMemoryBarrierCount = 1; info.pImageMemoryBarriers = &barrier;
      dev->vk.CmdPipelineBarrier2(cmd, &info);
      storage.layoutReady = true; storage.acquireSeq = dev->recordingSeq();
      dev->cmdHasWork = true;
   } else if (!storage.layoutReady) {
      /* Every image lives in GENERAL. Imported runtime allocations already
       * hold another owner's pixels, which the transition must keep. */
      end_rendering(dev);
      VkCommandBuffer cmd = ensure_cmd(dev);
      if (!cmd)
         return;
      dev->cmdHasWork = true;
      VkImageMemoryBarrier2 barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
      barrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
      barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
      barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
      barrier.oldLayout = storage.preinitialized ? VK_IMAGE_LAYOUT_PREINITIALIZED :
                                                   VK_IMAGE_LAYOUT_UNDEFINED;
      barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
      barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.image = storage.image;
      barrier.subresourceRange = {storage.aspects, 0, VK_REMAINING_MIP_LEVELS, 0,
                                  VK_REMAINING_ARRAY_LAYERS};
      VkDependencyInfo info = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
      info.imageMemoryBarrierCount = 1;
      info.pImageMemoryBarriers = &barrier;
      dev->vk.CmdPipelineBarrier2(cmd, &info);
      storage.layoutReady = true;
   }
   if (write)
      storage.writeSerial = ++dev->writeSerial;
}

void buffer_written(Device *dev)
{
   dev->bufferWriteSerial = ++dev->writeSerial;
}

void barrier_for_read(Device *dev, uint64_t writeSerial)
{
   if (writeSerial <= dev->barrierSerial)
      return;
   command_barrier(dev);
}

/* Clears recorded before a draw become that render pass's load operation.
 * A clear of anything else is executed as its own one-attachment pass. */
void flush_pending_clears(Device *dev, const View *except_bound)
{
   if (dev->pendingClears.empty())
      return;
   std::vector<PendingClear> clears;
   clears.swap(dev->pendingClears);
   for (const auto &clear : clears) {
      if (except_bound) {
         bool bound = clear.view == dev->dsv;
         for (View *rtv : dev->rtv)
            bound |= clear.view == rtv;
         if (bound) {
            dev->pendingClears.push_back(clear);
            continue;
         }
      }
      View *view = clear.view;
      command_barrier(dev);
      image_use(dev, view->res->storage, true);
      VkCommandBuffer cmd = ensure_cmd(dev);
      if (!cmd)
         return;
      dev->cmdHasWork = true;
      VkRenderingAttachmentInfo attachment = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
      {
         /* Materializing a recorded clear inside Flush/Present/QueryEnd/Destroy
          * cannot report recoverable OOM or drop the clear while leaving lost == S_OK. */
         RemovalOnlyErrorScope errors(dev);
         attachment.imageView = view_get(view);
      }
      if (!attachment.imageView)
         return;
      attachment.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
      attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
      attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      attachment.clearValue = clear.value;
      VkRenderingAttachmentInfo keep = attachment;
      keep.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      VkRenderingInfo info = {VK_STRUCTURE_TYPE_RENDERING_INFO};
      info.renderArea = {{0, 0}, {view->width, view->height}};
      info.layerCount = view->layers;
      if (view->type == ViewType::RTV) {
         info.colorAttachmentCount = 1;
         info.pColorAttachments = &attachment;
      } else {
         const uint16_t flags = view->formatFlags;
         if (flags & FMT_DEPTH)
            info.pDepthAttachment = (clear.aspects & VK_IMAGE_ASPECT_DEPTH_BIT) ? &attachment : &keep;
         if (flags & FMT_STENCIL)
            info.pStencilAttachment = (clear.aspects & VK_IMAGE_ASPECT_STENCIL_BIT) ? &attachment : &keep;
      }
      dev->vk.CmdBeginRendering(cmd, &info);
      dev->vk.CmdEndRendering(cmd);
      dev->stats.renderPasses++;
   }
}

/* ------------------------------------------------------------------ */

void defer(Device *dev, DeferredType type, uint64_t handle, uint64_t seq, VkImage image)
{
   if (handle)
      dev->deferred.push_back({type, seq, handle, image});
}

static void destroy_deferred(Device *dev, const Deferred &d)
{
   switch (d.type) {
   case DeferredType::Image:
      dev->vk.DestroyImage(dev->vkdev, (VkImage)d.handle, nullptr);
      break;
   case DeferredType::Memory:
      dev->vk.FreeMemory(dev->vkdev, (VkDeviceMemory)d.handle, nullptr);
      break;
   case DeferredType::ImageView:
      dev->vk.DestroyImageView(dev->vkdev, (VkImageView)d.handle, nullptr);
      break;
   case DeferredType::BufferView:
      dev->vk.DestroyBufferView(dev->vkdev, (VkBufferView)d.handle, nullptr);
      break;
   case DeferredType::Sampler:
      dev->vk.DestroySampler(dev->vkdev, (VkSampler)d.handle, nullptr);
      break;
   case DeferredType::Shader:
      dev->vk.DestroyShaderEXT(dev->vkdev, (VkShaderEXT)d.handle, nullptr);
      break;
   case DeferredType::QueryPool:
      dev->vk.DestroyQueryPool(dev->vkdev, (VkQueryPool)d.handle, nullptr);
      break;
   case DeferredType::Runtime: {
      HRESULT hr = runtime_destroy(reinterpret_cast<RuntimeResource *>((uintptr_t)d.handle));
      if (FAILED(hr))
         TG_ERROR(dev, hr);
      break;
   }
   }
}

void retire(Device *dev, bool force)
{
   if (!force && FAILED(dev->lost))
      return;
   const uint64_t done = force ? UINT64_MAX : dev->completed();
   if (!force && FAILED(dev->lost))
      return;
   while (!dev->submittedCommands.empty() && dev->submittedCommands.front().seq <= done) {
      SubmittedCommands &submitted = dev->submittedCommands.front();
      dev->vk.ResetCommandBuffer(submitted.cmd, 0);
      dev->freeCommands.push_back(submitted.cmd);
      for (auto pool : submitted.pools) {
         dev->vk.ResetDescriptorPool(dev->vkdev, pool, 0);
         dev->freePools.push_back(pool);
      }
      dev->submittedCommands.pop_front();
   }
   /* Deferred objects are queued in sequence order per kind, but a resource
    * destroyed early may carry an older sequence than its neighbours. */
   for (size_t i = 0; i < dev->deferred.size();) {
      if (dev->deferred[i].seq <= done) {
         Deferred d = dev->deferred[i];
         dev->deferred.erase(dev->deferred.begin() + i);
         destroy_deferred(dev, d);
      } else {
         i++;
      }
   }
   for (size_t i = 0; i < dev->deferredSlices.size();) {
      if (dev->deferredSlices[i].seq <= done) {
         Slice slice = dev->deferredSlices[i].slice;
         dev->deferredSlices.erase(dev->deferredSlices.begin() + i);
         slice_release(dev, slice, 0);
      } else {
         i++;
      }
   }
}

static HRESULT wait_cpu(Device *dev, uint64_t seq, tgd3d_wait_reason reason)
{
   if (FAILED(dev->lost))
      return dev->lost;
   const uint64_t completed = dev->completed();
   if (FAILED(dev->lost))
      return dev->lost;
   if (completed >= seq)
      return S_OK;
   const uint64_t begin = qpc_now();
   const uint64_t entering[4] = {begin, 0, 0, 0};
   const uint64_t requested[6] = {(uint64_t)reason, seq, 0, 0, 0, 0};
   trace_timing(TGD3D_TIMING_CPU_WAIT_BEGIN, E_PENDING, dev->submitted,
                completed, entering, requested);
   VkSemaphoreWaitInfo wait = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
   wait.semaphoreCount = 1;
   wait.pSemaphores = &dev->fence;
   wait.pValues = &seq;
   VkResult result = VK_TIMEOUT;
   /* The driver sleeps on the fence's kernel event. Exhausting this wait
    * fails the device; it does not establish GPU completion or permit
    * in-flight resources to be retired. */
   for (int second = 0; second < 30 && result == VK_TIMEOUT; second++)
      result = dev->vk.WaitSemaphores(dev->vkdev, &wait, UINT64_C(1000000000));
   const uint64_t end = qpc_now();
   if (reason == TGD3D_WAIT_BACKPRESSURE)
      dev->stats.backpressureNs += qpc_nanoseconds(end - begin);
   const uint64_t qpc[4] = {begin, end, 0, 0};
   const uint64_t value[6] = {(uint64_t)reason, seq, (uint32_t)result, 0, 0, 0};
   const uint64_t after = result == VK_SUCCESS ? dev->completed() : completed;
   trace_timing(TGD3D_TIMING_CPU_WAIT, result == VK_SUCCESS ? S_OK : E_FAIL, dev->submitted,
                after, qpc, value);
   if (result != VK_SUCCESS) {
      TG_ERROR(dev, DXGI_ERROR_DEVICE_REMOVED);
      return DXGI_ERROR_DEVICE_REMOVED;
   }
   /* The completion sample used by timing can itself report device loss. */
   return dev->lost;
}

HRESULT wait_sequence(Device *dev, uint64_t seq, tgd3d_wait_reason reason)
{
   if (seq > dev->submitted) {
      HRESULT hr = flush(dev, false);
      if (FAILED(hr))
         return hr;
   }
   HRESULT hr = wait_cpu(dev, seq, reason);
   if (FAILED(hr))
      return hr;
   retire(dev);
   return dev->lost;
}

static HRESULT gate_runtime(Device *dev)
{
   if (dev->gated >= dev->submitted || !dev->runtimeFence || !dev->hContext)
      return S_OK;
   /* Queued, not waited: the runtime context executes nothing after this
    * point until the render queue has signalled `submitted`. */
   D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMGPU wait = {};
   wait.hContext = dev->hContext;
   wait.ObjectCount = 1;
   wait.ObjectHandleArray = &dev->runtimeFence;
   wait.MonitoredFenceValueArray = &dev->submitted;
   trace(TGD3D_TRACE_SUBMIT_PHASE, E_PENDING, 3, dev->submitted);
   HRESULT hr = dev->kt.pfnWaitForSynchronizationObjectFromGpuCb(dev->hRTDevice.handle, &wait);
   trace(TGD3D_TRACE_SUBMIT_PHASE, hr, 3, dev->submitted);
   if (FAILED(hr))
      return hr;
   dev->gated = dev->submitted;
   return S_OK;
}

HRESULT flush(Device *dev, bool waitForSpace)
{
   if (FAILED(dev->lost))
      return dev->lost;
   flush_pending_clears(dev, nullptr);
   if (FAILED(dev->lost))
      return dev->lost;
   if (dev->cmd && dev->cmdHasWork) {
      end_rendering(dev);
      if (FAILED(dev->lost))
         return dev->lost;
      /* Every shared optimal image used by this recording leaves in GENERAL
       * before the real NVK fence is signaled. The runtime queue's GPU wait
       * then orders the next owner's access; no CPU completion is invented.
       * Track image handles, not Resource pointers, because DXGI may rotate
       * logical resource identities before this command buffer is submitted. */
      for (VkImage image : dev->sharedImages) {
         VkImageMemoryBarrier2 release = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
         release.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
         release.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT;
         release.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
         release.oldLayout = release.newLayout = VK_IMAGE_LAYOUT_GENERAL;
         release.srcQueueFamilyIndex = dev->caps->queueFamily;
         release.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
         release.image = image;
         release.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0,
                                     VK_REMAINING_ARRAY_LAYERS};
         VkDependencyInfo info = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
         info.imageMemoryBarrierCount = 1; info.pImageMemoryBarriers = &release;
         dev->vk.CmdPipelineBarrier2(dev->cmd, &info);
      }
      /* Coherent upload/readback storage still needs a device-to-host
       * memory dependency before a CPU fence wait can expose GPU writes. */
      VkMemoryBarrier2 host = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
      host.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
      host.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
      host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
      host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
      VkDependencyInfo dependency = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
      dependency.memoryBarrierCount = 1;
      dependency.pMemoryBarriers = &host;
      dev->vk.CmdPipelineBarrier2(dev->cmd, &dependency);
      const uint64_t submit_begin = trace_timing_enabled() ? qpc_now() : 0;
      if (dev->vk.EndCommandBuffer(dev->cmd) != VK_SUCCESS) {
         TG_ERROR(dev, DXGI_ERROR_DEVICE_REMOVED);
         return dev->lost;
      }
      const uint64_t value = dev->submitted + 1;
      VkSemaphoreSubmitInfo waits[2] = {};
      uint32_t waitCount = 0;
      if (dev->submitted) {
         auto &wait = waits[waitCount++];
         wait = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
         wait.semaphore = dev->fence;
         wait.value = dev->submitted;
         wait.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
      }
      if (dev->runtimeIngress) {
         D3DDDICB_SIGNALSYNCHRONIZATIONOBJECTFROMGPU prepare = {};
         prepare.hContext = dev->hContext;
         prepare.ObjectCount = 1;
         prepare.ObjectHandleArray = &dev->runtimeIngress;
         prepare.MonitoredFenceValueArray = &value;
         trace(TGD3D_TRACE_SUBMIT_PHASE, E_PENDING, 1, value);
         HRESULT hr = dev->kt.pfnSignalSynchronizationObjectFromGpuCb(
            dev->hRTDevice.handle, &prepare);
         trace(TGD3D_TRACE_SUBMIT_PHASE, hr, 1, value);
         if (FAILED(hr)) {
            TG_ERROR(dev, DXGI_ERROR_DEVICE_REMOVED);
            return dev->lost;
         }
         auto &wait = waits[waitCount++];
         wait = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
         wait.semaphore = dev->ingress;
         wait.value = value;
         wait.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
      }
      VkSemaphoreSubmitInfo signal = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
      signal.semaphore = dev->fence;
      signal.value = value;
      signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
      VkCommandBufferSubmitInfo buffer = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
      buffer.commandBuffer = dev->cmd;
      VkSubmitInfo2 submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
      submit.waitSemaphoreInfoCount = waitCount;
      submit.pWaitSemaphoreInfos = waits;
      submit.commandBufferInfoCount = 1;
      submit.pCommandBufferInfos = &buffer;
      submit.signalSemaphoreInfoCount = 1;
      submit.pSignalSemaphoreInfos = &signal;
      trace(TGD3D_TRACE_SUBMIT_PHASE, E_PENDING, 2, value);
      VkResult result = dev->vk.QueueSubmit2(dev->queue, 1, &submit, VK_NULL_HANDLE);
      trace(TGD3D_TRACE_SUBMIT_PHASE, result == VK_SUCCESS ? S_OK : E_FAIL, 2, value);
      if (result != VK_SUCCESS) {
         /* No wait is queued for a failed submission. Never fabricate GPU
          * completion on the CPU: older submissions can still own memory. */
         TG_ERROR(dev, DXGI_ERROR_DEVICE_REMOVED);
         return dev->lost;
      }
      SubmittedCommands done = {dev->cmd, value, {}};
      dev->sharedImages.clear();
      done.pools.swap(dev->framePools);
      dev->submittedCommands.push_back(done);
      dev->cmd = VK_NULL_HANDLE;
      dev->cmdHasWork = false;
      dev->submitted = value;
      dev->stats.submits++;
      if (submit_begin) {
         const uint64_t qpc[4] = {dev->firstCommandQpc, submit_begin, qpc_now(), 0};
         const uint64_t values[6] = {dev->stats.draws, dev->stats.renderPasses,
            dev->stats.uploadBytes, dev->stats.copyBytes, dev->stats.readbackBytes, 0};
         trace_timing(TGD3D_TIMING_SUBMIT, result == VK_SUCCESS ? S_OK : E_FAIL, value,
                      dev->completed(), qpc, values);
      }
      dev->stats.draws = dev->stats.renderPasses = 0;
      dev->stats.uploadBytes = dev->stats.copyBytes = dev->stats.readbackBytes = 0;

   }
   /* Every submission, including upload/readback-triggered ones, must be
    * visible to subsequent runtime signals and waits, even if the optional
    * post-submit timing sample just observed device loss. */
   HRESULT hr = gate_runtime(dev);
   if (FAILED(hr)) {
      if (SUCCEEDED(dev->lost))
         TG_ERROR(dev, DXGI_ERROR_DEVICE_REMOVED);
      return dev->lost;
   }
   if (FAILED(dev->lost))
      return dev->lost;
   retire(dev);
   /* Retirement reads the real completion fence. A failed read cannot be
    * turned into a successful Flush/Present/SetDisplayMode by returning the
    * earlier runtime-queue gate result. Keep outstanding objects owned. */
   if (FAILED(dev->lost))
      return dev->lost;
#ifdef TGD3D_CPU_SYNC_CONTROL
   /* Ordinary Flush, Present and shared-resource release must complete the
    * rendering before the runtime can signal its own context. Polling calls
    * still submit without waiting, preserving DONOTWAIT / QueryGetData. */
   if (waitForSpace && SUCCEEDED(hr)) {
      hr = wait_cpu(dev, dev->submitted, TGD3D_WAIT_SYNC_CONTROL);
      if (FAILED(hr))
         return hr;
      retire(dev);
   }
#else
   if (waitForSpace && SUCCEEDED(hr) && dev->submitted > TGD3D_MAX_IN_FLIGHT) {
      const uint64_t completed = dev->completed();
      if (FAILED(dev->lost))
         return dev->lost;
      if (completed < dev->submitted - TGD3D_MAX_IN_FLIGHT) {
         hr = wait_cpu(dev, dev->submitted - TGD3D_MAX_IN_FLIGHT, TGD3D_WAIT_BACKPRESSURE);
         if (FAILED(hr))
            return hr;
         retire(dev);
      }
   }
#endif
   return FAILED(hr) ? hr : dev->lost;
}

}
