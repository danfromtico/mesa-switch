/* SPDX-License-Identifier: MIT
 *
 * Draws and clears. Bound D3D state becomes Vulkan dynamic state; there are
 * no pipeline objects to build, look up or compile at draw time.
 */
#include "tgd3d.h"
#include "vulkan/util/vk_internal_exts.h"

#include <algorithm>
#include <cmath>
#include <string.h>

namespace tgd3d {

HRESULT device_layouts_create(Device *dev)
{
   const VulkanCaps &caps = *dev->caps;
   std::vector<VkDescriptorSetLayoutBinding> bindings[SET_GRAPHICS_UAV + 1];
   try {
      bindings[SET_CB].reserve(GRAPHICS_STAGE_COUNT * caps.cbSlots);
      bindings[SET_TEX].reserve(GRAPHICS_STAGE_COUNT * SET1_STRIDE);
      bindings[SET_TBUF].reserve(GRAPHICS_STAGE_COUNT * D3D_SRV_SLOTS);
      bindings[SET_GRAPHICS_UAV].reserve((2 + __builtin_popcount(caps.graphicsUavStages)) * caps.graphicsUavSlots);
   } catch (const std::bad_alloc &) { return E_OUTOFMEMORY; }
   for (uint32_t slot = 0; slot < caps.graphicsUavSlots; ++slot) {
      for (auto type : {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER})
         bindings[SET_GRAPHICS_UAV].push_back({graphics_uav_binding(0, slot, type), type, 1, caps.graphicsUavStages, nullptr});
   }
   for (uint32_t stage = 0; stage < GRAPHICS_STAGE_COUNT; stage++) {
      if (!caps.graphicsStageSupported(stage))
         continue;
      const VkShaderStageFlags flags = graphics_stage_flag(stage);
      if (flags & caps.graphicsUavStages) {
         for (uint32_t slot = 0; slot < caps.graphicsUavSlots; ++slot) {
            const auto type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            bindings[SET_GRAPHICS_UAV].push_back({graphics_uav_binding(stage, slot, type), type, 1, flags, nullptr});
         }
      }
      for (uint32_t i = 0; i < caps.cbSlots; i++)
         bindings[SET_CB].push_back({stage * caps.cbSlots + i, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                     1, flags, nullptr});
      for (uint32_t i = 0; i < D3D_SAMPLER_SLOTS; i++)
         bindings[SET_TEX].push_back({stage * SET1_STRIDE + i, VK_DESCRIPTOR_TYPE_SAMPLER, 1, flags,
                                      nullptr});
      for (uint32_t i = 0; i < D3D_SRV_SLOTS; i++) {
         bindings[SET_TEX].push_back({stage * SET1_STRIDE + D3D_SAMPLER_SLOTS + i,
                                      VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, flags, nullptr});
         bindings[SET_TBUF].push_back({stage * D3D_SRV_SLOTS + i,
                                       VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1, flags, nullptr});
      }
   }
   for (uint32_t set = 0; set < caps.graphicsSetCount(); set++) {
      VkDescriptorSetLayoutCreateInfo info = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
      /* An immutable set snapshot fits all 3 x 14 D3D10 constant buffers.
       * NVK's push descriptor limit is 32, below that DDI requirement. */
      info.bindingCount = (uint32_t)bindings[set].size();
      info.pBindings = bindings[set].data();
      const VkResult result = dev->vk.CreateDescriptorSetLayout(dev->vkdev, &info, nullptr, &dev->setLayouts[set]);
      if (result != VK_SUCCESS) return resource_result(result);
   }
   VkPipelineLayoutCreateInfo info = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
   info.setLayoutCount = caps.graphicsSetCount();
   info.pSetLayouts = dev->setLayouts;
   const VkResult result = dev->vk.CreatePipelineLayout(dev->vkdev, &info, nullptr, &dev->pipelineLayout);
   if (result != VK_SUCCESS) return resource_result(result);

   /* Direct3D 10 default sampler for a slot the application left empty. */
   VkSamplerCreateInfo sampler = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
   sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
   sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
   sampler.addressModeU = sampler.addressModeV = sampler.addressModeW =
      VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
   sampler.maxAnisotropy = 1.0f;
   sampler.maxLod = VK_LOD_CLAMP_NONE;
   sampler.compareOp = VK_COMPARE_OP_NEVER;
   if (dev->vk.CreateSampler(dev->vkdev, &sampler, nullptr, &dev->defaultSampler) != VK_SUCCESS)
      return E_OUTOFMEMORY;
   return S_OK;
}

void device_layouts_destroy(Device *dev)
{
   if (dev->defaultSampler)
      dev->vk.DestroySampler(dev->vkdev, dev->defaultSampler, nullptr);
   if (dev->pipelineLayout)
      dev->vk.DestroyPipelineLayout(dev->vkdev, dev->pipelineLayout, nullptr);
   for (auto &layout : dev->setLayouts)
      if (layout)
         dev->vk.DestroyDescriptorSetLayout(dev->vkdev, layout, nullptr);
}

/* ------------------------------------------------------------------ */

VkDescriptorSet descriptor_set_alloc(Device *dev, uint32_t set)
{
   return alloc_descriptor_snapshot(dev, dev->setLayouts[set]);
}

VkDescriptorSet alloc_descriptor_snapshot(Device *dev, VkDescriptorSetLayout layout)
{
   if (FAILED(dev->lost))
      return VK_NULL_HANDLE;
   for (int attempt = 0; attempt < 2; attempt++) {
      if (!dev->framePools.empty()) {
         VkDescriptorSetAllocateInfo info = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
         info.descriptorPool = dev->framePools.back();
         info.descriptorSetCount = 1;
         info.pSetLayouts = &layout;
         VkDescriptorSet handle = VK_NULL_HANDLE;
         const VkResult result = dev->vk.AllocateDescriptorSets(dev->vkdev, &info, &handle);
         if (result == VK_SUCCESS)
            return handle;
         if (result != VK_ERROR_OUT_OF_POOL_MEMORY && result != VK_ERROR_FRAGMENTED_POOL) {
            TG_ERROR(dev, resource_result(result));
            return VK_NULL_HANDLE;
         }
         if (attempt == 1)
            break;
      }
      VkDescriptorPool pool = VK_NULL_HANDLE;
      if (!dev->freePools.empty()) {
         pool = dev->freePools.back();
         dev->freePools.pop_back();
      } else {
         enum { SETS = 64 };
         const VkDescriptorPoolSize sizes[] = {
            {VK_DESCRIPTOR_TYPE_SAMPLER, SETS * GRAPHICS_STAGE_COUNT * D3D_SAMPLER_SLOTS},
            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, SETS * GRAPHICS_STAGE_COUNT * D3D_SRV_SLOTS},
            {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 8 * GRAPHICS_STAGE_COUNT * D3D_SRV_SLOTS},
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, SETS * GRAPHICS_STAGE_COUNT * D3D_CB_SLOTS},
            {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 8 * D3D_UAV_SLOTS},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 8 * D3D_UAV_SLOTS},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 8 * D3D_UAV_SLOTS},
         };
         VkDescriptorPoolCreateInfo info = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
         info.maxSets = 2 * SETS + 8;
         info.poolSizeCount = dev->caps && (dev->caps->compute || dev->caps->graphicsUavSlots) ? std::size(sizes) : 4;
         info.pPoolSizes = sizes;
         const VkResult result = dev->vk.CreateDescriptorPool(dev->vkdev, &info, nullptr, &pool);
         if (result != VK_SUCCESS) {
            TG_ERROR(dev, resource_result(result));
            return VK_NULL_HANDLE;
         }
      }
      dev->framePools.push_back(pool);
   }
   TG_ERROR(dev, E_OUTOFMEMORY);
   return VK_NULL_HANDLE;
}

bool srv_matches(uint8_t kind, const View *view)
{
   /* dxbc_spv::ir::ResourceKind: 0-2 buffers, 3/4 1D, 5/6 2D, 9/10 cube, 11 3D. */
   const bool buffer = view->res->kind == ResKind::Buffer;
   if (kind <= 2)
      return buffer;
   if (buffer || view->res->kind != ResKind::Image)
      return false;
   switch (view->info.viewType) {
   case VK_IMAGE_VIEW_TYPE_1D: return kind == 3 || kind == 4;
   case VK_IMAGE_VIEW_TYPE_2D: return kind == 5 || kind == 6;
   case VK_IMAGE_VIEW_TYPE_CUBE: return kind == 9;
   case VK_IMAGE_VIEW_TYPE_3D: return kind == 11;
   default: return false;
   }
}

/* Makes every image a bound shader reads usable for sampling: initial layout
 * and ordering after an earlier write. May end the current render pass. */
static void prepare_reads(Device *dev)
{
   for (uint32_t s = 0; s < GRAPHICS_STAGE_COUNT; s++) {
      const StageBindings &stage = dev->stage[s];
      const Shader *shader = stage.shader;
      if (!shader)
         continue;
      for (uint32_t word = 0; word < 4; word++) {
         uint32_t mask = shader->srvMask[word];
         while (mask) {
            const uint32_t bit = __builtin_ctz(mask);
            mask &= mask - 1;
            View *view = stage.srv[word * 32 + bit];
            if (!view)
               continue;
            Resource *res = view->res;
            if (res->kind == ResKind::Image) {
               image_use(dev, res->storage, false);
               barrier_for_read(dev, res->storage.writeSerial);
            } else if (res->kind == ResKind::Buffer) {
               res->sliceLastUse = dev->recordingSeq();
            }
         }
      }
   }
}

static bool bind_textures(Device *dev, VkCommandBuffer cmd)
{
   bool any = false, any_buffers = false;
   for (uint32_t s = 0; s < GRAPHICS_STAGE_COUNT; ++s) {
      const Shader *shader = dev->stage[s].shader;
      if (!shader)
         continue;
      any |= shader->samplerMask != 0;
      for (uint32_t word = 0; word < 4; word++)
         any |= (shader->srvMask[word] & ~shader->srvBufferMask[word]) != 0;
      any_buffers |= shader->usesTexelBuffers;
   }
   const bool textures_dirty = any && (dev->dirty & DIRTY_TEXTURES);
   const bool buffers_dirty = any_buffers && (dev->dirty & DIRTY_TEXEL_BUFFERS);
   if (!textures_dirty && !buffers_dirty)
      return true;

   auto &writes = dev->descriptorWrites;
   auto &images = dev->descriptorImages;
   auto &buffers = dev->descriptorBuffers;
   writes.clear(); images.clear(); buffers.clear();
   /* Reserve the bounded D3D slot maximum before publishing any pointers
    * into these arrays. Reuse only host update arguments, never an in-flight
    * descriptor set. Vulkan consumes them during UpdateDescriptorSets. */
   try {
      writes.reserve(GRAPHICS_STAGE_COUNT * (D3D_SAMPLER_SLOTS + D3D_SRV_SLOTS));
      if (textures_dirty)
         images.reserve(GRAPHICS_STAGE_COUNT * (D3D_SAMPLER_SLOTS + D3D_SRV_SLOTS));
      if (buffers_dirty)
         buffers.reserve(GRAPHICS_STAGE_COUNT * D3D_SRV_SLOTS);
   } catch (const std::bad_alloc &) {
      TG_ERROR(dev, DXGI_ERROR_DEVICE_REMOVED);
      return false;
   }

   if (textures_dirty) {
      VkDescriptorSet set = descriptor_set_alloc(dev, SET_TEX);
      if (!set)
         return false;
      for (uint32_t s = 0; s < GRAPHICS_STAGE_COUNT; s++) {
         const StageBindings &stage = dev->stage[s];
         const Shader *shader = stage.shader;
         if (!shader)
            continue;
         for (uint32_t mask = shader->samplerMask; mask; mask &= mask - 1) {
            const uint32_t slot = __builtin_ctz(mask);
            const Sampler *sampler = stage.sampler[slot];
            images.push_back({sampler && sampler->sampler ? sampler->sampler : dev->defaultSampler,
                              VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED});
            VkWriteDescriptorSet write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            write.dstSet = set;
            write.dstBinding = s * SET1_STRIDE + slot;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
            write.pImageInfo = &images.back();
            writes.push_back(write);
         }
         for (uint32_t word = 0; word < 4; word++) {
            for (uint32_t mask = shader->srvMask[word] & ~shader->srvBufferMask[word]; mask;
                 mask &= mask - 1) {
               const uint32_t slot = word * 32 + __builtin_ctz(mask);
               View *view = stage.srv[slot];
               const uint8_t kind = shader->srvKind[slot];
               VkImageView handle = VK_NULL_HANDLE;
               /* An empty or mismatched slot reads zero, as Direct3D defines. */
               if (view && srv_matches(kind, view)) {
                  handle = view_get(view, kind == 4 || kind == 6 || kind == 10);
                  if (!handle)
                     return false;
               }
               images.push_back({VK_NULL_HANDLE, handle, VK_IMAGE_LAYOUT_GENERAL});
               VkWriteDescriptorSet write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
               write.dstSet = set;
               write.dstBinding = s * SET1_STRIDE + D3D_SAMPLER_SLOTS + slot;
               write.descriptorCount = 1;
               write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
               write.pImageInfo = &images.back();
               writes.push_back(write);
            }
         }
      }
      dev->vk.UpdateDescriptorSets(dev->vkdev, (uint32_t)writes.size(), writes.data(), 0, nullptr);
      dev->vk.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, dev->pipelineLayout,
                                    SET_TEX, 1, &set, 0, nullptr);
      dev->textureSet = set;
   }

   if (buffers_dirty) {
      VkDescriptorSet set = descriptor_set_alloc(dev, SET_TBUF);
      if (!set)
         return false;
      writes.clear();
      for (uint32_t s = 0; s < GRAPHICS_STAGE_COUNT; s++) {
         const StageBindings &stage = dev->stage[s];
         const Shader *shader = stage.shader;
         if (!shader)
            continue;
         for (uint32_t word = 0; word < 4; word++) {
            for (uint32_t mask = shader->srvBufferMask[word]; mask; mask &= mask - 1) {
               const uint32_t slot = word * 32 + __builtin_ctz(mask);
               View *view = stage.srv[slot];
               VkBufferView handle = VK_NULL_HANDLE;
               if (view && srv_matches(shader->srvKind[slot], view)) {
                  handle = view_get_buffer(view);
                  if (!handle)
                     return false;
               }
               buffers.push_back(handle);
               VkWriteDescriptorSet write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
               write.dstSet = set;
               write.dstBinding = s * D3D_SRV_SLOTS + slot;
               write.descriptorCount = 1;
               write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
               write.pTexelBufferView = &buffers.back();
               writes.push_back(write);
            }
         }
      }
      dev->vk.UpdateDescriptorSets(dev->vkdev, (uint32_t)writes.size(), writes.data(), 0, nullptr);
      dev->vk.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, dev->pipelineLayout,
                                    SET_TBUF, 1, &set, 0, nullptr);
      dev->texelBufferSet = set;
   }
   return true;
}

static bool bind_constant_buffers(Device *dev, VkCommandBuffer cmd)
{
   bool dirty = false;
   for (UINT s = 0; s < GRAPHICS_STAGE_COUNT; ++s) {
      auto &stage = dev->stage[s];
      if (!stage.shader)
         continue;
      dirty |= (stage.cbDirty & stage.shader->cbMask) != 0;
      /* Every draw extends lifetime, even when its descriptor is reused. */
      for (uint32_t mask = stage.shader->cbMask; mask; mask &= mask - 1)
         if (Resource *res = stage.cb[__builtin_ctz(mask)])
            res->sliceLastUse = dev->recordingSeq();
   }
   if (!dirty)
      return true;
   VkDescriptorSet set = descriptor_set_alloc(dev, SET_CB);
   if (!set)
      return false;
   VkWriteDescriptorSet writes[GRAPHICS_STAGE_COUNT * D3D_CB_SLOTS];
   VkDescriptorBufferInfo infos[GRAPHICS_STAGE_COUNT * D3D_CB_SLOTS];
   uint32_t count = 0;
   const uint32_t slots = dev->caps->cbSlots;
   for (uint32_t s = 0; s < GRAPHICS_STAGE_COUNT; s++) {
      StageBindings &stage = dev->stage[s];
      const Shader *shader = stage.shader;
      if (!shader)
         continue;
      for (uint32_t mask = shader->cbMask; mask; mask &= mask - 1) {
         const uint32_t slot = __builtin_ctz(mask);
         if (slot >= slots)
            continue;
         Resource *res = stage.cb[slot];
         VkDescriptorBufferInfo &info = infos[count];
         const VkDeviceSize offset = stage.cbOffset[slot];
         const VkDeviceSize bytes = stage.cbBytes[slot];
         if (res && res->slice && offset < res->size && bytes) {
            info = {res->slice.buffer(), res->slice.offset + offset,
                    std::min(bytes, res->size - offset)};
            res->sliceLastUse = dev->recordingSeq();
         } else {
            info = {VK_NULL_HANDLE, 0, VK_WHOLE_SIZE};   /* reads zero */
         }
         VkWriteDescriptorSet &write = writes[count++];
         write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
         write.dstSet = set;
         write.dstBinding = s * slots + slot;
         write.descriptorCount = 1;
         write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
         write.pBufferInfo = &info;
      }
      stage.cbDirty &= ~shader->cbMask;
   }
   dev->vk.UpdateDescriptorSets(dev->vkdev, count, writes, 0, nullptr);
   dev->vk.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, dev->pipelineLayout,
                                 SET_CB, 1, &set, 0, nullptr);
   return true;
}

/* ------------------------------------------------------------------ */

static bool begin_rendering(Device *dev, bool hasStorageOutput)
{
   uint32_t width = ~0u, height = ~0u, layers = ~0u, colors = 0;
   for (uint32_t i = 0; i < D3D10_DDI_SIMULTANEOUS_RENDER_TARGET_COUNT; i++) {
      if (View *view = dev->rtv[i]) {
         colors = i + 1;
         width = view->width < width ? view->width : width;
         height = view->height < height ? view->height : height;
         layers = view->layers < layers ? view->layers : layers;
      }
   }
   if (View *view = dev->dsv) {
      width = view->width < width ? view->width : width;
      height = view->height < height ? view->height : height;
      layers = view->layers < layers ? view->layers : layers;
   }
   if (width == ~0u) {
      if (!hasStorageOutput) return false;
      // Attachmentless rasterization is bounded by the viewport, independent
      // of the size and dimensionality of the resources a shader accesses.
      double right = 1, bottom = 1;
      for (UINT i = 0; i < dev->viewportCount; ++i) {
         const auto &vp = dev->viewports[i];
         right = std::max(right, double(vp.TopLeftX) + vp.Width);
         bottom = std::max(bottom, double(vp.TopLeftY) + vp.Height);
      }
      width = uint32_t(std::min(std::ceil(right), double(dev->caps->maxFramebufferWidth)));
      height = uint32_t(std::min(std::ceil(bottom), double(dev->caps->maxFramebufferHeight)));
      layers = 1;
   }

   command_barrier(dev);
   for (uint32_t i = 0; i < colors; i++)
      if (dev->rtv[i])
         image_use(dev, dev->rtv[i]->res->storage, true);
   if (dev->dsv)
      image_use(dev, dev->dsv->res->storage, true);

   VkRenderingAttachmentInfo color[D3D10_DDI_SIMULTANEOUS_RENDER_TARGET_COUNT];
   VkRenderingAttachmentInfo depth = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
   VkRenderingAttachmentInfo stencil = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
   auto take_clear = [dev](View *view, VkImageAspectFlags aspect, VkRenderingAttachmentInfo *info) {
      info->loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      for (size_t c = 0; c < dev->pendingClears.size(); c++) {
         PendingClear &clear = dev->pendingClears[c];
         if (clear.view != view || !(clear.aspects & aspect))
            continue;
         info->loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
         info->clearValue = clear.value;
         clear.aspects &= ~aspect;
         if (!clear.aspects)
            dev->pendingClears.erase(dev->pendingClears.begin() + c);
         return;
      }
   };
   for (uint32_t i = 0; i < colors; i++) {
      color[i] = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
      color[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
      color[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      if (View *view = dev->rtv[i]) {
         color[i].imageView = view_get(view);
         if (!color[i].imageView)
            return false;
         take_clear(view, VK_IMAGE_ASPECT_COLOR_BIT, &color[i]);
      }
   }
   VkRenderingInfo info = {VK_STRUCTURE_TYPE_RENDERING_INFO};
   nvk_tir_rendering_info tir = {VK_STRUCTURE_TYPE_NVK_TIR_RENDERING_INFO_MESA};
   if (dev->rasterizer && (dev->caps->tirSamples & dev->rasterizer->samples)) {
      tir.rasterSamples = dev->rasterizer->samples;
      info.pNext = &tir;
   }
   info.renderArea = {{0, 0}, {width, height}};
   info.layerCount = layers;
   info.colorAttachmentCount = colors;
   info.pColorAttachments = color;
   if (View *view = dev->dsv) {
      depth.imageView = stencil.imageView = view_get(view);
      if (!depth.imageView)
         return false;
      depth.imageLayout = stencil.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
      depth.storeOp = stencil.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      take_clear(view, VK_IMAGE_ASPECT_DEPTH_BIT, &depth);
      info.pDepthAttachment = &depth;
      if (view->formatFlags & FMT_STENCIL) {
         take_clear(view, VK_IMAGE_ASPECT_STENCIL_BIT, &stencil);
         info.pStencilAttachment = &stencil;
      }
   }
   VkCommandBuffer cmd = cmd_draw(dev);
   if (!cmd)
      return false;
   dev->vk.CmdBeginRendering(cmd, &info);
   dev->rendering = true;
   dev->stats.renderPasses++;
   query_pass_begin(dev, cmd);
   if (dev->renderWidth != width || dev->renderHeight != height || dev->boundColorCount != colors)
      dev->dirty |= DIRTY_SCISSORS | DIRTY_BLEND;
   dev->renderWidth = width;
   dev->renderHeight = height;
   dev->boundColorCount = colors;
   return true;
}

static VkCompareOp compare_op(D3D10_DDI_COMPARISON_FUNC func)
{
   return func >= 1 && func <= 8 ? (VkCompareOp)(func - 1) : VK_COMPARE_OP_ALWAYS;
}

static VkStencilOp stencil_op(D3D10_DDI_STENCIL_OP op)
{
   return op >= 1 && op <= 8 ? (VkStencilOp)(op - 1) : VK_STENCIL_OP_KEEP;
}

static UINT patch_points(D3D10_DDI_PRIMITIVE_TOPOLOGY topology)
{
   const UINT value = UINT(topology);
   if (value < D3D11_DDI_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST ||
       value > D3D11_DDI_PRIMITIVE_TOPOLOGY_32_CONTROL_POINT_PATCHLIST) return 0;
   return value - D3D11_DDI_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST + 1;
}

static void emit_state(Device *dev, VkCommandBuffer cmd)
{
   const VulkanCaps &caps = *dev->caps;
   const uint32_t dirty = dev->dirty;
   const auto &vk = dev->vk;

   if (dirty & DIRTY_STATIC) {
      if (caps.tessellationShader)
         vk.CmdSetTessellationDomainOriginEXT(cmd, VK_TESSELLATION_DOMAIN_ORIGIN_UPPER_LEFT);
      if (caps.transformFeedback) vk.CmdSetRasterizationStreamEXT(cmd, 0);
      vk.CmdSetLineWidth(cmd, 1.0f);
      vk.CmdSetDepthBoundsTestEnable(cmd, VK_FALSE);
      vk.CmdSetAlphaToOneEnableEXT(cmd, VK_FALSE);
   }

   if (dirty & (DIRTY_STATIC | DIRTY_SHADERS | DIRTY_VIEWPORTS)) {
      const Shader *gs = dev->stage[STAGE_GS].shader;
      vk.CmdSetRasterizerDiscardEnable(cmd, gs && gs->so && (gs->so->discardRaster || (gs->so->stride && !dev->viewportCount)));
   }
   if (dirty & (DIRTY_RASTERIZER | DIRTY_BLEND | DIRTY_STATIC)) {
      const auto samples = dev->rasterizer ? dev->rasterizer->samples : VK_SAMPLE_COUNT_1_BIT;
      const auto targetSamples = dev->caps->tirSamples & samples ? VK_SAMPLE_COUNT_1_BIT : samples;
      vk.CmdSetRasterizationSamplesEXT(cmd, targetSamples);
      // D3D11 ignores the output-merger sample mask for UAV-only rasterization.
      const VkSampleMask sample_mask = dev->boundColorCount || dev->dsv ? dev->sampleMask : ~0u;
      vk.CmdSetSampleMaskEXT(cmd, targetSamples, &sample_mask);
   }
   if (dirty & DIRTY_TOPOLOGY) {
      VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      bool strip = false;
      switch (dev->topology) {
      case D3D10_DDI_PRIMITIVE_TOPOLOGY_POINTLIST: topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; break;
      case D3D10_DDI_PRIMITIVE_TOPOLOGY_LINELIST: topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
      case D3D10_DDI_PRIMITIVE_TOPOLOGY_LINESTRIP:
         topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; strip = true; break;
      case D3D10_DDI_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP:
         topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; strip = true; break;
      case D3D10_DDI_PRIMITIVE_TOPOLOGY_LINELIST_ADJ:
         topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY; break;
      case D3D10_DDI_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ:
         topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY; strip = true; break;
      case D3D10_DDI_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ:
         topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY; break;
      case D3D10_DDI_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ:
         topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY; strip = true; break;
      default:
         if (UINT points = patch_points(dev->topology)) {
            topology = VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
            vk.CmdSetPatchControlPointsEXT(cmd, points);
         }
         break;
      }
      vk.CmdSetPrimitiveTopology(cmd, topology);
      vk.CmdSetPrimitiveRestartEnable(cmd, strip);   /* strip-cut index, always on in D3D10 */
   }

   if (dirty & DIRTY_RASTERIZER) {
      static const D3D10_DDI_RASTERIZER_DESC defaults = {
         D3D10_DDI_FILL_SOLID, D3D10_DDI_CULL_BACK, FALSE, 0, 0.0f, 0.0f, TRUE, FALSE, FALSE, FALSE};
      const auto &d = dev->rasterizer ? dev->rasterizer->desc : defaults;
      vk.CmdSetCullMode(cmd, d.CullMode == D3D10_DDI_CULL_FRONT ? VK_CULL_MODE_FRONT_BIT :
                             d.CullMode == D3D10_DDI_CULL_BACK ? VK_CULL_MODE_BACK_BIT :
                                                                 VK_CULL_MODE_NONE);
      /* The viewport is flipped, so winding is judged exactly as D3D does. */
      vk.CmdSetFrontFace(cmd, d.FrontCounterClockwise ? VK_FRONT_FACE_COUNTER_CLOCKWISE :
                                                        VK_FRONT_FACE_CLOCKWISE);
      vk.CmdSetPolygonModeEXT(cmd, d.FillMode == D3D10_DDI_FILL_WIREFRAME ? VK_POLYGON_MODE_LINE :
                                                                           VK_POLYGON_MODE_FILL);
      const bool bias = d.DepthBias != 0 || d.SlopeScaledDepthBias != 0.0f;
      vk.CmdSetDepthBiasEnable(cmd, bias);
      vk.CmdSetDepthBias(cmd, (float)d.DepthBias, d.DepthBiasClamp, d.SlopeScaledDepthBias);
      vk.CmdSetDepthClampEnableEXT(cmd, caps.depthClamp && !d.DepthClipEnable);
      vk.CmdSetDepthClipEnableEXT(cmd, d.DepthClipEnable != 0);
   }

   if (dirty & DIRTY_DEPTH_STENCIL) {
      const D3D10_DDI_DEPTH_STENCILOP_DESC keep = {
         D3D10_DDI_STENCIL_OP_KEEP, D3D10_DDI_STENCIL_OP_KEEP, D3D10_DDI_STENCIL_OP_KEEP,
         D3D10_DDI_COMPARISON_ALWAYS};
      D3D10_DDI_DEPTH_STENCIL_DESC d = {};
      if (dev->depthStencil) {
         d = dev->depthStencil->desc;
      } else {
         d.DepthEnable = TRUE;
         d.DepthWriteMask = D3D10_DDI_DEPTH_WRITE_MASK_ALL;
         d.DepthFunc = D3D10_DDI_COMPARISON_LESS;
         d.FrontFace = d.BackFace = keep;
      }
      vk.CmdSetDepthTestEnable(cmd, d.DepthEnable != 0);
      const UINT readOnly = dev->dsv ? dev->dsv->dsvFlags : 0;
      vk.CmdSetDepthWriteEnable(cmd, d.DepthEnable && d.DepthWriteMask == D3D10_DDI_DEPTH_WRITE_MASK_ALL &&
                                  !(readOnly & D3D11_DDI_CREATE_DSV_READ_ONLY_DEPTH));
      vk.CmdSetDepthCompareOp(cmd, compare_op(d.DepthFunc));
      vk.CmdSetStencilTestEnable(cmd, d.StencilEnable != 0);
      const auto &front = d.FrontEnable ? d.FrontFace : keep;
      const auto &back = d.BackEnable ? d.BackFace : keep;
      vk.CmdSetStencilOp(cmd, VK_STENCIL_FACE_FRONT_BIT, stencil_op(front.StencilFailOp),
                         stencil_op(front.StencilPassOp), stencil_op(front.StencilDepthFailOp),
                         compare_op(front.StencilFunc));
      vk.CmdSetStencilOp(cmd, VK_STENCIL_FACE_BACK_BIT, stencil_op(back.StencilFailOp),
                         stencil_op(back.StencilPassOp), stencil_op(back.StencilDepthFailOp),
                         compare_op(back.StencilFunc));
      vk.CmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, d.StencilReadMask);
      vk.CmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK,
         (readOnly & D3D11_DDI_CREATE_DSV_READ_ONLY_STENCIL) ? 0 : d.StencilWriteMask);
      vk.CmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, dev->stencilRef);
   }

   if (dirty & DIRTY_BLEND) {
      const BlendState *state = dev->blend;
      const bool logic = state && state->logicOpEnabled;
      vk.CmdSetLogicOpEnableEXT(cmd, logic);
      if (logic)
         vk.CmdSetLogicOpEXT(cmd, state->logicOp);
      const uint32_t count = dev->boundColorCount;
      VkBool32 enables[D3D10_DDI_SIMULTANEOUS_RENDER_TARGET_COUNT];
      VkColorBlendEquationEXT equations[D3D10_DDI_SIMULTANEOUS_RENDER_TARGET_COUNT];
      VkColorComponentFlags masks[D3D10_DDI_SIMULTANEOUS_RENDER_TARGET_COUNT];
      const VkColorBlendEquationEXT replace = {VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO,
         VK_BLEND_OP_ADD, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD};
      for (uint32_t i = 0; i < count; i++) {
         const View *view = dev->rtv[i];
         const uint16_t flags = view ? view->formatFlags : 0;
         /* Integer targets cannot blend; X8 targets never store alpha. */
         enables[i] = state && state->desc.BlendEnable[i] && !(flags & (FMT_UINT | FMT_SINT));
         equations[i] = state ? state->equation : replace;
         masks[i] = state ? state->desc.RenderTargetWriteMask[i] : 0xf;
         if (flags & FMT_ALPHA_ONE)
            masks[i] &= ~VK_COLOR_COMPONENT_A_BIT;
      }
      if (count) {
         vk.CmdSetColorBlendEnableEXT(cmd, 0, count, enables);
         vk.CmdSetColorBlendEquationEXT(cmd, 0, count, equations);
         vk.CmdSetColorWriteMaskEXT(cmd, 0, count, masks);
      }
      vk.CmdSetBlendConstants(cmd, dev->blendFactor);
      vk.CmdSetAlphaToCoverageEnableEXT(cmd, dev->boundColorCount && state && state->desc.AlphaToCoverageEnable);
   }

   const uint32_t viewport_count = std::max(1u, std::min(dev->viewportCount, caps.maxViewports));
   if (dirty & DIRTY_VIEWPORTS) {
      VkViewport viewports[16];
      for (uint32_t i = 0; i < viewport_count; i++) {
         if (!dev->viewportCount) { viewports[i] = {0, 0, 1, 1, 0, 1}; continue; }
         const D3D10_DDI_VIEWPORT &v = dev->viewports[i];
         /* Negative height: Direct3D clip space has +Y up. */
         viewports[i] = {(float)v.TopLeftX, (float)v.TopLeftY + (float)v.Height, (float)v.Width,
                         -(float)v.Height, v.MinDepth, v.MaxDepth};
      }
      vk.CmdSetViewportWithCount(cmd, viewport_count, viewports);
   }
   if (dirty & (DIRTY_SCISSORS | DIRTY_VIEWPORTS)) {
      VkRect2D rects[16];
      const bool enabled = dev->rasterizer && dev->rasterizer->desc.ScissorEnable;
      for (uint32_t i = 0; i < viewport_count; i++) {
         rects[i] = {{0, 0}, {dev->renderWidth, dev->renderHeight}};
         if (!dev->viewportCount) { rects[i].extent = {0, 0}; continue; }
         if (!enabled)
            continue;
         if (i >= dev->scissorCount) {
            rects[i].extent = {0, 0};
            continue;
         }
         const D3D10_DDI_RECT &r = dev->scissors[i];
         const LONG left = r.left < 0 ? 0 : r.left, top = r.top < 0 ? 0 : r.top;
         const LONG right = r.right > (LONG)dev->renderWidth ? (LONG)dev->renderWidth : r.right;
         const LONG bottom = r.bottom > (LONG)dev->renderHeight ? (LONG)dev->renderHeight : r.bottom;
         rects[i].offset = {left, top};
         rects[i].extent = {right > left ? (uint32_t)(right - left) : 0,
                            bottom > top ? (uint32_t)(bottom - top) : 0};
         if (right <= left || bottom <= top)
            rects[i].offset = {0, 0};
      }
      vk.CmdSetScissorWithCount(cmd, viewport_count, rects);
   }

   const ElementLayout *layout = dev->inputLayout;
   uint32_t slot_mask = 0;
   if (layout)
      for (UINT i = 0; i < layout->count; i++)
         slot_mask |= 1u << layout->elements[i].InputSlot;
   if (dirty & DIRTY_VERTEX_INPUT) {
      VkVertexInputBindingDescription2EXT bindings[16];
      VkVertexInputAttributeDescription2EXT attributes[32];
      uint32_t binding_count = 0, attribute_count = 0;
      for (UINT i = 0; layout && i < layout->count; i++) {
         const auto &e = layout->elements[i];
         VkVertexInputAttributeDescription2EXT &a = attributes[attribute_count++];
         a = {VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT};
         a.location = e.InputRegister;
         a.binding = e.InputSlot;
         a.format = vertex_vk_format(e.Format);
         a.offset = e.AlignedByteOffset;
         bool known = false;
         for (uint32_t b = 0; b < binding_count; b++)
            known |= bindings[b].binding == e.InputSlot;
         if (known)
            continue;
         VkVertexInputBindingDescription2EXT &b = bindings[binding_count++];
         b = {VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT};
         b.binding = e.InputSlot;
         b.stride = dev->vb[e.InputSlot].stride;
         b.divisor = 1;
         if (e.InputSlotClass == D3D10_DDI_INPUT_PER_INSTANCE_DATA) {
            b.inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
            if (e.InstanceDataStepRate != 1 && caps.instanceDivisor &&
                (e.InstanceDataStepRate || caps.instanceZeroDivisor))
               b.divisor = e.InstanceDataStepRate;
         } else {
            b.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
         }
      }
      vk.CmdSetVertexInputEXT(cmd, binding_count, bindings, attribute_count, attributes);
   }
   if ((dirty & (DIRTY_VERTEX_BUFFERS | DIRTY_VERTEX_INPUT)) && slot_mask) {
      VkBuffer buffers[16];
      VkDeviceSize offsets[16], sizes[16], strides[16];
      const uint32_t count = 32 - __builtin_clz(slot_mask);
      for (uint32_t i = 0; i < count; i++) {
         const VertexBinding &vb = dev->vb[i];
         Resource *res = vb.buffer;
         if (res && res->slice && vb.offset < res->size) {
            buffers[i] = res->slice.buffer();
            offsets[i] = res->slice.offset + vb.offset;
            sizes[i] = res->size - vb.offset;
            res->sliceLastUse = dev->recordingSeq();
         } else {
            buffers[i] = VK_NULL_HANDLE;     /* an unbound stream reads zero */
            offsets[i] = 0;
            sizes[i] = VK_WHOLE_SIZE;
         }
         strides[i] = vb.stride;
      }
      vk.CmdBindVertexBuffers2(cmd, 0, count, buffers, offsets, sizes, strides);
   }
}

static bool bind_shaders(Device *dev, VkCommandBuffer cmd)
{
   Shader *vs = dev->stage[STAGE_VS].shader;
   Shader *gs = dev->stage[STAGE_GS].shader;
   Shader *ps = dev->stage[STAGE_PS].shader;
   Shader *hs = dev->stage[STAGE_HS].shader;
   Shader *ds = dev->stage[STAGE_DS].shader;
   if (!vs)
      return false;
   uint32_t vs_key = 0, ps_key = 0;
   if (dev->inputLayout) {
      for (uint32_t mask = vs->inputRegisters; mask; mask &= mask - 1) {
         const uint32_t reg = __builtin_ctz(mask);
         vs_key |= dev->inputLayout->numericClasses & (3u << (2 * reg));
      }
   }
   if (ps) {
      for (uint32_t mask = ps->outputRegisters; mask; mask &= mask - 1) {
         const uint32_t reg = __builtin_ctz(mask);
         const View *view = dev->rtv[reg];
         if (view)
            ps_key |= ((view->formatFlags & FMT_UINT) ? 1u : (view->formatFlags & FMT_SINT) ? 2u : 0u)
                      << (2 * reg);
      }
      if (blend_uses_dual_source(dev->blend))
         ps_key |= 1u << 31;
   }
   VkShaderEXT handles[GRAPHICS_STAGE_COUNT]{};
   const bool passthrough = gs && gs->soPassthrough;
   if (passthrough && !ds) handles[STAGE_VS] = shader_variant(vs, vs_key, gs->so);
   else handles[STAGE_VS] = shader_variant(vs, vs_key);
   if (gs && !passthrough) handles[STAGE_GS] = shader_variant(gs, 0);
   if (ps) handles[STAGE_PS] = shader_variant(ps, ps_key);
   if (hs && ds) {
      handles[STAGE_HS] = shader_variant(hs, 0);
      handles[STAGE_DS] = passthrough ? shader_variant(ds, hs->tessIoMask, gs->so) : shader_variant(ds, hs->tessIoMask);
   }
   if (!handles[STAGE_VS] || (gs && !passthrough && !handles[STAGE_GS]) || (ps && !handles[STAGE_PS]) ||
       (hs && (!handles[STAGE_HS] || !handles[STAGE_DS])))
      return false;
   VkShaderStageFlagBits stages[GRAPHICS_STAGE_COUNT];
   VkShaderEXT shaders[GRAPHICS_STAGE_COUNT];
   uint32_t count = 0;
   for (uint32_t s = 0; s < GRAPHICS_STAGE_COUNT; s++) {
      if (!dev->caps->graphicsStageSupported(s))
         continue;
      if (dev->stage[s].bound == handles[s] && !(dev->dirty & DIRTY_STATIC))
         continue;
      stages[count] = (VkShaderStageFlagBits)graphics_stage_flag(s);
      shaders[count++] = handles[s];
      dev->stage[s].bound = handles[s];
   }
   if (count)
      dev->vk.CmdBindShadersEXT(cmd, count, stages, shaders);
   return true;
}

/* Returns the command buffer positioned inside a render pass with all bound
 * state applied, or null when Direct3D defines the draw as a no-op. */
static VkCommandBuffer prepare_draw(Device *dev, bool indexed)
{
   if (FAILED(dev->lost) || !dev->stage[STAGE_VS].shader)
      return VK_NULL_HANDLE;
   const Shader *hs = dev->stage[STAGE_HS].shader, *ds = dev->stage[STAGE_DS].shader;
   const UINT points = patch_points(dev->topology);
   if (points || hs || ds) {
      if (!points || !hs || !ds || points != hs->inputControlPoints || hs->tessDomain != ds->tessDomain)
         return VK_NULL_HANDLE;
   }
   if (indexed && (!dev->indexBuffer || !dev->indexBuffer->slice))
      return VK_NULL_HANDLE;
   uint64_t slots = 0;
   for (UINT s = 0; s < GRAPHICS_STAGE_COUNT; ++s)
      if (const Shader *sh = dev->stage[s].shader) slots |= sh->uavMask;
   const Shader *gs = dev->stage[STAGE_GS].shader;
   const bool capturing = gs && gs->so && gs->so->stride && dev->soTargets[0];
   if (!dev->viewportCount && !slots && !capturing) return VK_NULL_HANDLE;
   flush_pending_clears(dev, dev->dsv ? dev->dsv : dev->rtv[0]);
   ImageStorage *writes[D3D10_DDI_SIMULTANEOUS_RENDER_TARGET_COUNT + 1 + D3D_UAV_SLOTS];
   UINT count = 0;
   for (UINT i = 0; i < D3D10_DDI_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
      if (dev->rtv[i]) writes[count++] = &dev->rtv[i]->res->storage;
   if (dev->dsv) writes[count++] = &dev->dsv->res->storage;
   const UINT attachments = count;
   if (attachments && dev->rasterizer && dev->rasterizer->samples > VK_SAMPLE_COUNT_1_BIT) {
      // Mixing single-sample attachments and multisample rasterization needs
      // its own target-independent rendering path.
      trace_message("Forced multisampling with an attachment is unsupported");
      return VK_NULL_HANDLE;
   }
   for (uint64_t mask = slots; mask; mask &= mask - 1) {
      const View *v = dev->graphicsUav[__builtin_ctzll(mask)];
      if (v && v->res->kind == ResKind::Image) writes[count++] = &v->res->storage;
   }
   if (!prepare_image_writes(dev, writes, count))
      return VK_NULL_HANDLE;
   prepare_reads(dev);
   // SO targets may have been read as vertex/indirect data in the preceding
   // draw. Order those reads as well as earlier SO writes before capturing.
   if (capturing) end_rendering(dev);
   if (slots) {
      // UAVs can read and write in the same draw, including after an SRV or
      // indirect read in the previous draw. Complete these uses before the pass.
      end_rendering(dev);
      for (UINT i = attachments; i < count; ++i)
         image_use(dev, *writes[i], false);
   } else if (!capturing) {
      barrier_for_read(dev, dev->bufferWriteSerial);
   }
   if (FAILED(dev->lost) || (!dev->rendering && !begin_rendering(dev, slots || capturing)))
      return VK_NULL_HANDLE;
   VkCommandBuffer cmd = cmd_draw(dev);
   if (!cmd)
      return VK_NULL_HANDLE;
   if (!bind_shaders(dev, cmd))
      return VK_NULL_HANDLE;
   emit_state(dev, cmd);
   if (!bind_constant_buffers(dev, cmd))
      return VK_NULL_HANDLE;
   if (!bind_textures(dev, cmd))
      return VK_NULL_HANDLE;
   if (slots && (dev->dirty & DIRTY_GRAPHICS_UAVS) && !bind_graphics_uavs(dev, cmd))
      return VK_NULL_HANDLE;
   if (indexed && (dev->dirty & DIRTY_INDEX_BUFFER)) {
      Resource *res = dev->indexBuffer;
      dev->vk.CmdBindIndexBuffer(cmd, res->slice.buffer(), res->slice.offset + dev->indexOffset,
                                 dev->indexFormat == DXGI_FORMAT_R32_UINT ? VK_INDEX_TYPE_UINT32 :
                                                                            VK_INDEX_TYPE_UINT16);
      dev->dirty &= ~DIRTY_INDEX_BUFFER;
   }
   if (indexed)
      dev->indexBuffer->sliceLastUse = dev->recordingSeq();
   for (uint64_t mask = slots; mask; mask &= mask - 1) {
      View *v = dev->graphicsUav[__builtin_ctzll(mask)];
      if (!v) continue;
      if (v->res->kind == ResKind::Image) {
         image_use(dev, v->res->storage, true);
      } else {
         v->res->sliceLastUse = v->res->sliceLastWrite = dev->recordingSeq();
         buffer_written(dev);
      }
   }
   if (FAILED(dev->lost)) return VK_NULL_HANDLE;
   dev->dirty &= DIRTY_INDEX_BUFFER;
   dev->stats.draws++;
   return cmd;
}

/* Scope feedback to one draw. A pass split, failed state bind or internal
 * blit cannot leave feedback active. The next draw resumes the GPU cursor. */
template<typename DrawCommand>
static void record_draw(Device *dev, bool indexed, DrawCommand draw)
{
   VkCommandBuffer cmd = prepare_draw(dev, indexed);
   if (!cmd) return;
   Shader *gs = dev->stage[STAGE_GS].shader;
   Resource *target = dev->soTargets[0];
   if (!gs || !gs->so || !gs->so->stride || !target) {
      draw(cmd);
      return;
   }
   VkBuffer buffer = target->slice.buffer(), counter = target->soCounter.buffer();
   dev->vk.CmdBindTransformFeedbackBuffersEXT(cmd, 0, 1, &buffer, &target->slice.offset, &target->size);
   dev->vk.CmdBeginTransformFeedbackEXT(cmd, 0, 1, &counter, &target->soCounter.offset);
   draw(cmd);
   dev->vk.CmdEndTransformFeedbackEXT(cmd, 0, 1, &counter, &target->soCounter.offset);
   target->soVertexStride = gs->so->stride;
   target->soCounterLastUse = target->sliceLastUse = target->sliceLastWrite = dev->recordingSeq();
   buffer_written(dev);
}

static void APIENTRY Draw(D3D10DDI_HDEVICE hDevice, UINT count, UINT start)
{
   Device *dev = D(hDevice);
   RemovalOnlyErrorScope errors(dev);
   record_draw(dev, false, [&](VkCommandBuffer cmd) { dev->vk.CmdDraw(cmd, count, 1, start, 0); });
}

static void APIENTRY DrawIndexed(D3D10DDI_HDEVICE hDevice, UINT count, UINT start, INT base)
{
   Device *dev = D(hDevice);
   RemovalOnlyErrorScope errors(dev);
   record_draw(dev, true, [&](VkCommandBuffer cmd) { dev->vk.CmdDrawIndexed(cmd, count, 1, start, base, 0); });
}

static void APIENTRY DrawInstanced(D3D10DDI_HDEVICE hDevice, UINT count, UINT instances, UINT start,
                                   UINT start_instance)
{
   Device *dev = D(hDevice);
   RemovalOnlyErrorScope errors(dev);
   record_draw(dev, false, [&](VkCommandBuffer cmd) { dev->vk.CmdDraw(cmd, count, instances, start, start_instance); });
}

static void APIENTRY DrawIndexedInstanced(D3D10DDI_HDEVICE hDevice, UINT count, UINT instances,
                                          UINT start, INT base, UINT start_instance)
{
   Device *dev = D(hDevice);
   RemovalOnlyErrorScope errors(dev);
   record_draw(dev, true, [&](VkCommandBuffer cmd) { dev->vk.CmdDrawIndexed(cmd, count, instances, start, base, start_instance); });
}

static void draw_indirect(Device *dev, Resource *args, UINT offset, bool indexed)
{
   RemovalOnlyErrorScope errors(dev);
   if (FAILED(dev->lost))
      return;
   const VkDeviceSize bytes = indexed ? sizeof(VkDrawIndexedIndirectCommand) : sizeof(VkDrawIndirectCommand);
   if (!args || args->dev != dev || args->kind != ResKind::Buffer || !args->slice ||
       !(args->misc & D3D11_DDI_RESOURCE_MISC_DRAWINDIRECT_ARGS) ||
       (offset & 3) || offset > args->size || bytes > args->size - offset ||
       offset > args->slice.size || bytes > args->slice.size - offset ||
       runtime_is_offered(resource_runtime(args))) {
      TG_ERROR(dev, E_INVALIDARG);
      return;
   }
   if (!dev->caps->drawIndirectFirstInstance) {
      TG_ERROR(dev, DXGI_ERROR_UNSUPPORTED);
      return;
   }
   /* The argument buffer stays on the GPU. prepare_draw orders preceding
    * buffer writes before indirect reads; retirement follows the same real
    * completion sequence as vertex/index buffers, including after renaming. */
   record_draw(dev, indexed, [&](VkCommandBuffer cmd) {
      args->sliceLastUse = dev->recordingSeq();
      if (indexed)
         dev->vk.CmdDrawIndexedIndirect(cmd, args->slice.buffer(), args->slice.offset + offset, 1, bytes);
      else
         dev->vk.CmdDrawIndirect(cmd, args->slice.buffer(), args->slice.offset + offset, 1, bytes);
   });
}

void APIENTRY DrawIndirect11(D3D10DDI_HDEVICE device, D3D10DDI_HRESOURCE args, UINT offset)
{
   draw_indirect(D(device), static_cast<Resource *>(args.pDrvPrivate), offset, false);
}

void APIENTRY DrawIndexedIndirect11(D3D10DDI_HDEVICE device, D3D10DDI_HRESOURCE args, UINT offset)
{
   draw_indirect(D(device), static_cast<Resource *>(args.pDrvPrivate), offset, true);
}

static void APIENTRY DrawAuto(D3D10DDI_HDEVICE hDevice)
{
   Device *dev = D(hDevice);
   if (FAILED(dev->lost))
      return;
   RemovalOnlyErrorScope errors(dev);
   if (!dev->caps || !dev->caps->transformFeedback) { TG_ERROR(dev, E_NOTIMPL); return; }
   const VertexBinding &vb = dev->vb[0];
   Resource *source = vb.buffer;
   if (!source || !source->soCounter || !source->soVertexStride) return;
   record_draw(dev, false, [&](VkCommandBuffer cmd) {
      source->sliceLastUse = source->soCounterLastUse = dev->recordingSeq();
      dev->vk.CmdDrawIndirectByteCountEXT(cmd, 1, 0, source->soCounter.buffer(), source->soCounter.offset,
                                          vb.offset, source->soVertexStride);
   });
}

/* ------------------------------------------------------------------ */

static void clear_view(Device *dev, View *view, VkImageAspectFlags aspects, const VkClearValue &value)
{
   if (!view || view->res->kind != ResKind::Image)
      return;
   if (dev->rendering) {
      /* Clear inside the open pass when the view is one of its attachments. */
      VkClearAttachment attachment = {};
      attachment.aspectMask = aspects;
      attachment.clearValue = value;
      bool bound = view == dev->dsv;
      for (uint32_t i = 0; i < dev->boundColorCount && !bound; i++) {
         if (dev->rtv[i] == view) {
            attachment.colorAttachment = i;
            bound = true;
         }
      }
      if (bound && view->width == dev->renderWidth && view->height == dev->renderHeight) {
         const VkClearRect rect = {{{0, 0}, {dev->renderWidth, dev->renderHeight}}, 0, view->layers};
         dev->vk.CmdClearAttachments(dev->cmd, 1, &attachment, 1, &rect);
         return;
      }
      end_rendering(dev);
   }
   for (auto &clear : dev->pendingClears) {
      if (clear.view == view) {
         if (aspects & VK_IMAGE_ASPECT_COLOR_BIT)
            clear.value = value;
         if (aspects & VK_IMAGE_ASPECT_DEPTH_BIT)
            clear.value.depthStencil.depth = value.depthStencil.depth;
         if (aspects & VK_IMAGE_ASPECT_STENCIL_BIT)
            clear.value.depthStencil.stencil = value.depthStencil.stencil;
         clear.aspects |= aspects;
         return;
      }
   }
   dev->pendingClears.push_back({view, value, aspects});
}

static void APIENTRY ClearRenderTargetView(D3D10DDI_HDEVICE hDevice, D3D10DDI_HRENDERTARGETVIEW hView,
                                           FLOAT color[4])
{
   Device *dev = D(hDevice);
   View *view = static_cast<View *>(hView.pDrvPrivate);
   VkClearValue value = {};
   if (view->formatFlags & FMT_UINT) {
      for (int i = 0; i < 4; i++)
         value.color.uint32[i] = color[i] <= 0.0f ? 0u : (uint32_t)color[i];
   } else if (view->formatFlags & FMT_SINT) {
      for (int i = 0; i < 4; i++)
         value.color.int32[i] = (int32_t)color[i];
   } else {
      memcpy(value.color.float32, color, sizeof(float) * 4);
      if (view->formatFlags & FMT_ALPHA_ONE)
         value.color.float32[3] = 1.0f;
   }
   clear_view(dev, view, VK_IMAGE_ASPECT_COLOR_BIT, value);
}

static void APIENTRY ClearDepthStencilView(D3D10DDI_HDEVICE hDevice, D3D10DDI_HDEPTHSTENCILVIEW hView,
                                           UINT flags, FLOAT depth, UINT8 stencil)
{
   Device *dev = D(hDevice);
   View *view = static_cast<View *>(hView.pDrvPrivate);
   VkImageAspectFlags aspects = 0;
   if ((flags & D3D10_DDI_CLEAR_DEPTH) && !(view->dsvFlags & D3D11_DDI_CREATE_DSV_READ_ONLY_DEPTH))
      aspects |= VK_IMAGE_ASPECT_DEPTH_BIT;
   if ((flags & D3D10_DDI_CLEAR_STENCIL) && (view->formatFlags & FMT_STENCIL) &&
       !(view->dsvFlags & D3D11_DDI_CREATE_DSV_READ_ONLY_STENCIL))
      aspects |= VK_IMAGE_ASPECT_STENCIL_BIT;
   if (!aspects)
      return;
   VkClearValue value = {};
   value.depthStencil = {depth, stencil};
   clear_view(dev, view, aspects, value);
}

void draw_fill_table(D3D10DDI_DEVICEFUNCS *t)
{
   t->pfnDraw = Draw;
   t->pfnDrawIndexed = DrawIndexed;
   t->pfnDrawInstanced = DrawInstanced;
   t->pfnDrawIndexedInstanced = DrawIndexedInstanced;
   t->pfnDrawAuto = DrawAuto;
   t->pfnClearRenderTargetView = ClearRenderTargetView;
   t->pfnClearDepthStencilView = ClearDepthStencilView;
}

}
