/* SPDX-License-Identifier: MIT
 * Immediate DirectCompute dispatch on the same NVK queue as graphics and copies.
 */
#include "tgd3d.h"
#include <algorithm>

namespace tgd3d {

void APIENTRY CsSetShaderResources(D3D10DDI_HDEVICE h, UINT start, UINT count,
                                   const D3D10DDI_HSHADERRESOURCEVIEW *views)
{
   Device *dev = D(h);
   if (start > D3D_SRV_SLOTS || count > D3D_SRV_SLOTS - start || (count && !views)) {
      TG_ERROR(dev, E_INVALIDARG); return;
   }
   for (UINT i = 0; i < count; ++i) {
      const View *v = static_cast<const View *>(views[i].pDrvPrivate);
      if (v && (v->dev != dev || v->type != ViewType::SRV)) { TG_ERROR(dev, E_INVALIDARG); return; }
   }
   for (UINT i = 0; i < count; ++i)
      dev->stage[STAGE_CS].srv[start+i] = static_cast<View *>(views[i].pDrvPrivate);
}

void APIENTRY CsSetSamplers(D3D10DDI_HDEVICE h, UINT start, UINT count, const D3D10DDI_HSAMPLER *samplers)
{
   Device *dev = D(h);
   if (start > D3D_SAMPLER_SLOTS || count > D3D_SAMPLER_SLOTS - start || (count && !samplers)) {
      TG_ERROR(dev, E_INVALIDARG); return;
   }
   for (UINT i = 0; i < count; ++i) {
      const Sampler *s = static_cast<const Sampler *>(samplers[i].pDrvPrivate);
      if (s && s->dev != dev) { TG_ERROR(dev, E_INVALIDARG); return; }
   }
   for (UINT i = 0; i < count; ++i)
      dev->stage[STAGE_CS].sampler[start+i] = static_cast<Sampler *>(samplers[i].pDrvPrivate);
}

bool set_uavs(Device *dev, View **slots, UINT start, UINT count, const D3D11DDI_HUNORDEREDACCESSVIEW *views,
              const UINT *initialCounts)
{
   if (start > D3D_UAV_SLOTS || count > D3D_UAV_SLOTS - start || (count && !views)) {
      TG_ERROR(dev, E_INVALIDARG); return false;
   }
   for (UINT i = 0; i < count; ++i) {
      const View *v = static_cast<const View *>(views[i].pDrvPrivate);
      if (v && (v->dev != dev || v->type != ViewType::UAV)) { TG_ERROR(dev, E_INVALIDARG); return false; }
   }
   for (UINT i = 0; i < count; ++i) {
      View *v = static_cast<View *>(views[i].pDrvPrivate);
      if (v && v->counter && initialCounts && initialCounts[i] != UINT_MAX) {
         VkCommandBuffer cmd = cmd_begin(dev);
         if (!cmd) return false;
         dev->vk.CmdFillBuffer(cmd, v->counter.buffer(), v->counter.offset, 4, initialCounts[i]);
         buffer_written(dev);
      }
      if (slots[start+i] != v && slots == dev->graphicsUav)
         dev->dirty |= DIRTY_GRAPHICS_UAVS;
      slots[start+i] = v;
   }
   return true;
}

void APIENTRY CsSetUavs(D3D10DDI_HDEVICE h, UINT start, UINT count, const D3D11DDI_HUNORDEREDACCESSVIEW *views,
                       const UINT *initialCounts)
{
   Device *dev = D(h);
   set_uavs(dev, dev->computeUav, start, count, views, initialCounts);
}

void APIENTRY ClearUavUint11(D3D10DDI_HDEVICE h, D3D11DDI_HUNORDEREDACCESSVIEW handle, const UINT values[4])
{
   Device *dev = D(h);
   auto *v = static_cast<View *>(handle.pDrvPrivate);
   if (!v || v->dev != dev || v->type != ViewType::UAV || !values) { TG_ERROR(dev, E_INVALIDARG); return; }
   Resource *r = v->res;
   // Raw and structured clears repeat the first word over the entire view.
   if (r->kind != ResKind::Buffer || !r->slice ||
       (v->format != DXGI_FORMAT_UNKNOWN && v->format != DXGI_FORMAT_R32_TYPELESS &&
        v->format != DXGI_FORMAT_R32_UINT && v->format != DXGI_FORMAT_R32_SINT && v->format != DXGI_FORMAT_R32_FLOAT)) {
      TG_ERROR(dev, DXGI_ERROR_UNSUPPORTED); return;
   }
   if (VkCommandBuffer cmd = cmd_begin(dev)) {
      dev->vk.CmdFillBuffer(cmd, r->slice.buffer(), r->slice.offset + v->bufferOffset, v->bufferRange, values[0]);
      r->sliceLastUse = r->sliceLastWrite = dev->recordingSeq();
      buffer_written(dev);
   }
}

void APIENTRY CopyStructureCount11(D3D10DDI_HDEVICE h, D3D10DDI_HRESOURCE dstHandle, UINT offset,
                                   D3D11DDI_HUNORDEREDACCESSVIEW srcHandle)
{
   Device *dev = D(h);
   Resource *dst = R(dstHandle);
   auto *src = static_cast<View *>(srcHandle.pDrvPrivate);
   if (!dst || dst->dev != dev || dst->kind != ResKind::Buffer || !dst->slice || (offset & 3) ||
       dst->size < 4 || offset > dst->size - 4 || !src || src->dev != dev || !src->counter) {
      TG_ERROR(dev, E_INVALIDARG); return;
   }
   VkCommandBuffer cmd = cmd_begin(dev);
   if (!cmd) return;
   VkBufferCopy region{src->counter.offset, dst->slice.offset + offset, 4};
   dev->vk.CmdCopyBuffer(cmd, src->counter.buffer(), dst->slice.buffer(), 1, &region);
   dst->sliceLastUse = dst->sliceLastWrite = dev->recordingSeq();
   buffer_written(dev);
}

static bool is_uav(const ShaderBinding &b)
{
   return b.type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER || b.type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
}

static bool write_descriptor(Device *dev, uint32_t stageIndex, const ShaderBinding &b, VkDescriptorSet set,
                             DescriptorData &data, VkWriteDescriptorSet &w)
{
   StageBindings &stage = dev->stage[stageIndex];
   View *const *uavs = stageIndex == STAGE_CS ? dev->computeUav : dev->graphicsUav;
   data = {}; // A previously populated scratch entry can now describe a null view.
   w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
   w.dstSet = set; w.dstBinding = b.binding; w.descriptorType = b.type; w.descriptorCount = 1;
   if (b.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
      Resource *res = stage.cb[b.slot];
      const VkDeviceSize offset = stage.cbOffset[b.slot];
      data.buffer.range = VK_WHOLE_SIZE;
      if (res && res->slice && offset < res->size && stage.cbBytes[b.slot]) {
         data.buffer = {res->slice.buffer(), res->slice.offset + offset,
                        std::min(stage.cbBytes[b.slot], res->size - offset)};
         res->sliceLastUse = dev->recordingSeq();
      }
      w.pBufferInfo = &data.buffer;
   } else if (b.type == VK_DESCRIPTOR_TYPE_SAMPLER) {
      const Sampler *sampler = stage.sampler[b.slot];
      data.image.sampler = sampler ? sampler->sampler : dev->defaultSampler;
      w.pImageInfo = &data.image;
   } else if (b.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
      const View *view = uavs[b.slot];
      data.buffer = view && view->counter ? VkDescriptorBufferInfo{view->counter.buffer(),view->counter.offset,4} :
                                           VkDescriptorBufferInfo{VK_NULL_HANDLE,0,VK_WHOLE_SIZE};
      w.pBufferInfo = &data.buffer;
   } else {
      const bool writable = is_uav(b);
      View *view = writable ? uavs[b.slot] : stage.srv[b.slot];
      if (view && !srv_matches(b.kind, view)) view = nullptr;
      if (b.type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || b.type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) {
         if (view) {
            data.texel = view_get_buffer(view);
            if (!data.texel) return false;
            view->res->sliceLastUse = dev->recordingSeq();
            if (writable) view->res->sliceLastWrite = dev->recordingSeq();
         }
         w.pTexelBufferView = &data.texel;
      } else {
         if (view) {
            const bool arrayed = b.kind == 4 || b.kind == 6 || b.kind == 10;
            data.image.imageView = view_get(view, arrayed);
            if (!data.image.imageView) return false;
            if (stageIndex == STAGE_CS) image_use(dev, view->res->storage, writable);
            if (FAILED(dev->lost)) return false;
         }
         data.image.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
         w.pImageInfo = &data.image;
      }
   }
   return true;
}

static bool bind_descriptors(Device *dev, VkCommandBuffer cmd, uint32_t first, uint32_t end,
                             VkDescriptorSetLayout setLayout, VkPipelineLayout layout, uint32_t setIndex)
{
   size_t count = 0;
   for (uint32_t i = first; i < end; ++i)
      if (const Shader *sh = dev->stage[i].shader) count += sh->bindings.size();
   if (!count) return true;
   VkDescriptorSet set = alloc_descriptor_snapshot(dev, setLayout);
   if (!set) return false;
   auto &updates = dev->descriptorWrites;
   auto &contents = dev->shaderDescriptorData;
   try {
      updates.resize(count);
      contents.resize(count);
   } catch (const std::bad_alloc &) { TG_ERROR(dev, E_OUTOFMEMORY); return false; }
   size_t index = 0;
   uint64_t texels = 0, counters = 0;
   for (uint32_t i = first; i < end; ++i) {
      const Shader *shader = dev->stage[i].shader;
      if (!shader) continue;
      for (const auto &binding : shader->bindings) {
         if (first != STAGE_CS && binding.type != VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
            uint64_t &written = binding.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ? counters : texels;
            const uint64_t bit = UINT64_C(1) << binding.slot;
            if (written & bit) continue;
            written |= bit;
         }
         if (!write_descriptor(dev, i, binding, set, contents[index], updates[index])) return false;
         ++index;
      }
   }
   dev->vk.UpdateDescriptorSets(dev->vkdev, static_cast<UINT>(index), updates.data(), 0, nullptr);
   const auto point = first == STAGE_CS ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS;
   dev->vk.CmdBindDescriptorSets(cmd, point, layout, setIndex, 1, &set, 0, nullptr);
   return true;
}

bool bind_graphics_uavs(Device *dev, VkCommandBuffer cmd)
{
   return bind_descriptors(dev, cmd, 0, GRAPHICS_STAGE_COUNT, dev->setLayouts[SET_GRAPHICS_UAV],
                           dev->pipelineLayout, SET_GRAPHICS_UAV);
}

static VkCommandBuffer prepare_compute(Device *dev)
{
   Shader *shader = dev->stage[STAGE_CS].shader;
   if (!shader || FAILED(dev->lost)) return VK_NULL_HANDLE;
   VkShaderEXT handle = shader_variant(shader, 0);
   if (!handle) return VK_NULL_HANDLE;
   const ImageStorage *writes[D3D_UAV_SLOTS]{};
   UINT count = 0;
   for (const auto &b : shader->bindings) {
      if (b.type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
         const View *v = dev->computeUav[b.slot];
         if (v && v->res->kind == ResKind::Image) writes[count++] = &v->res->storage;
      }
   }
   // Draining earlier attachment clears can itself use a submit slot.
   flush_pending_clears(dev, nullptr);
   if (!prepare_image_writes(dev, writes, count)) return VK_NULL_HANDLE;
   VkCommandBuffer cmd = cmd_begin(dev);
   if (!cmd) return VK_NULL_HANDLE;
   if (!bind_descriptors(dev, cmd, STAGE_CS, STAGE_CS + 1, shader->computeSet, shader->computeLayout, 0))
      return VK_NULL_HANDLE;
   const VkShaderStageFlagBits cs = VK_SHADER_STAGE_COMPUTE_BIT;
   dev->vk.CmdBindShadersEXT(cmd, 1, &cs, &handle);
   return cmd;
}

static bool gm20b_statistics(const Device *dev)
{
   return dev->nvk && dev->adapter->info.chipset == 0x12e;
}

void APIENTRY Dispatch11(D3D10DDI_HDEVICE h, UINT x, UINT y, UINT z)
{
   Device *dev = D(h);
   if (!x || !y || !z) return;
   if (!dev->caps || !dev->caps->compute || x > dev->caps->maxWorkgroups[0] ||
       y > dev->caps->maxWorkgroups[1] || z > dev->caps->maxWorkgroups[2]) {
      TG_ERROR(dev, E_INVALIDARG); return;
   }
   RemovalOnlyErrorScope errors(dev);
   if (VkCommandBuffer cmd = prepare_compute(dev)) {
      const Shader *shader = dev->stage[STAGE_CS].shader;
      const bool countExplicit = gm20b_statistics(dev);
      if (!countExplicit) query_pass_begin(dev, cmd, true);
      if (FAILED(dev->lost)) return;
      dev->vk.CmdDispatch(cmd, x, y, z);
      if (countExplicit) {
         // These unconditional direct dispatches have explicit thread counts.
         // The older desktop ICD can execute them but return a zero counter.
         // GetData still gates the computed total on this submission's fence.
         const uint64_t invocations = uint64_t(x) * y * z * shader->groupSize;
         for (Query *q : dev->activeQueries)
            if (q->type == D3D11DDI_QUERY_PIPELINESTATS) q->explicitInvocations += invocations;
      } else {
         query_pass_end(dev, cmd);
      }
      buffer_written(dev);
   }
}

void APIENTRY DispatchIndirect11(D3D10DDI_HDEVICE h, D3D10DDI_HRESOURCE buffer, UINT offset)
{
   Device *dev = D(h);
   Resource *args = R(buffer);
   if (!args || args->dev != dev || args->kind != ResKind::Buffer || !args->slice ||
       !(args->misc & D3D11_DDI_RESOURCE_MISC_DRAWINDIRECT_ARGS) || (offset & 3) ||
       args->size < 12 || offset > args->size - 12) { TG_ERROR(dev, E_INVALIDARG); return; }
   RemovalOnlyErrorScope errors(dev);
   if (VkCommandBuffer cmd = prepare_compute(dev)) {
      const bool capture = gm20b_statistics(dev);
      if (capture) {
         if (!capture_indirect_invocations(dev, cmd, args, offset, dev->stage[STAGE_CS].shader->groupSize))
            return;
      } else {
         query_pass_begin(dev, cmd, true);
      }
      if (FAILED(dev->lost)) return;
      dev->vk.CmdDispatchIndirect(cmd, args->slice.buffer(), args->slice.offset + offset);
      if (!capture) query_pass_end(dev, cmd);
      args->sliceLastUse = dev->recordingSeq();
      buffer_written(dev);
   }
}
} // namespace tgd3d
