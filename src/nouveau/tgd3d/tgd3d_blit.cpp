/* SPDX-License-Identifier: MIT
 * DXGI's explicitly requested rotation copy on the Vulkan graphics queue.
 * DC column presentation uses the original surface and bypasses this path.
 */
#include "tgd3d.h"
#include "shaders/blit_spv.h"

namespace tgd3d {

static HRESULT blit_error(Device *dev, VkResult result)
{
   const HRESULT hr = resource_result(result);
   if (FAILED(hr))
      TG_ERROR(dev, hr);
   return hr;
}

static HRESULT blit_shaders(Device *dev, UINT rotation)
{
   const VkPushConstantRange region = {VK_SHADER_STAGE_VERTEX_BIT, 0, 4 * sizeof(float)};
   if (!dev->blitPipelineLayout) {
      VkPipelineLayoutCreateInfo layout = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
      layout.setLayoutCount = SET_COUNT;
      layout.pSetLayouts = dev->setLayouts;
      layout.pushConstantRangeCount = 1;
      layout.pPushConstantRanges = &region;
      HRESULT hr = blit_error(dev, dev->vk.CreatePipelineLayout(dev->vkdev, &layout, nullptr,
                                                                 &dev->blitPipelineLayout));
      if (FAILED(hr)) return hr;
   }
   if (!dev->blitVertex[rotation]) {
      VkSpecializationMapEntry map = {0, 0, sizeof(rotation)};
      VkSpecializationInfo constants = {1, &map, sizeof(rotation), &rotation};
      VkShaderCreateInfoEXT info = {VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT};
      info.stage = VK_SHADER_STAGE_VERTEX_BIT;
      info.nextStage = VK_SHADER_STAGE_FRAGMENT_BIT;
      info.codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT;
      info.codeSize = sizeof(tgd3d_blit_vert);
      info.pCode = tgd3d_blit_vert;
      info.pName = "main";
      info.setLayoutCount = SET_COUNT;
      info.pSetLayouts = dev->setLayouts;
      info.pushConstantRangeCount = 1;
      info.pPushConstantRanges = &region;
      info.pSpecializationInfo = &constants;
      HRESULT hr = blit_error(dev, dev->vk.CreateShadersEXT(dev->vkdev, 1, &info, nullptr,
                                                            &dev->blitVertex[rotation]));
      if (FAILED(hr)) return hr;
   }
   if (!dev->blitFragment) {
      VkShaderCreateInfoEXT info = {VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT};
      info.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
      info.codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT;
      info.codeSize = sizeof(tgd3d_blit_frag);
      info.pCode = tgd3d_blit_frag;
      info.pName = "main";
      info.setLayoutCount = SET_COUNT;
      info.pSetLayouts = dev->setLayouts;
      info.pushConstantRangeCount = 1;
      info.pPushConstantRanges = &region;
      HRESULT hr = blit_error(dev, dev->vk.CreateShadersEXT(dev->vkdev, 1, &info, nullptr,
                                                            &dev->blitFragment));
      if (FAILED(hr)) return hr;
   }
   if (!dev->blitPointSampler) {
      VkSamplerCreateInfo info = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
      info.magFilter = info.minFilter = VK_FILTER_NEAREST;
      info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
      info.addressModeU = info.addressModeV = info.addressModeW =
         VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
      info.maxAnisotropy = 1.0f;
      HRESULT hr = blit_error(dev, dev->vk.CreateSampler(dev->vkdev, &info, nullptr,
                                                         &dev->blitPointSampler));
      if (FAILED(hr)) return hr;
   }
   return S_OK;
}

void blit_destroy(Device *dev)
{
   for (auto &shader : dev->blitVertex) {
      if (shader) dev->vk.DestroyShaderEXT(dev->vkdev, shader, nullptr);
      shader = VK_NULL_HANDLE;
   }
   if (dev->blitFragment) dev->vk.DestroyShaderEXT(dev->vkdev, dev->blitFragment, nullptr);
   if (dev->blitPointSampler) dev->vk.DestroySampler(dev->vkdev, dev->blitPointSampler, nullptr);
   if (dev->blitPipelineLayout) dev->vk.DestroyPipelineLayout(dev->vkdev, dev->blitPipelineLayout, nullptr);
   dev->blitFragment = VK_NULL_HANDLE;
   dev->blitPointSampler = VK_NULL_HANDLE;
   dev->blitPipelineLayout = VK_NULL_HANDLE;
}

/* DXGI presentation copies preserve encoded sRGB values when the destination
 * is UNORM. These display formats share a mutable UNORM view. */
static VkFormat blit_format(const Resource *res)
{
   switch (res->format) {
   case DXGI_FORMAT_B8G8R8A8_UNORM:
   case DXGI_FORMAT_B8G8R8X8_UNORM:
   case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
   case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
   case DXGI_FORMAT_R8G8B8A8_UNORM:
   case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
   case DXGI_FORMAT_R10G10B10A2_UNORM: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
   case DXGI_FORMAT_R16G16B16A16_FLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
   default: return VK_FORMAT_UNDEFINED;
   }
}

static HRESULT blit_view(Device *dev, Resource *res, UINT subresource, bool sampled,
                         VkFormat format, VkImageView *out)
{
   const UINT mip = subresource % res->mips, layer = subresource / res->mips;
   ImageStorage::BlitView *slot = nullptr;
   for (auto &entry : res->storage.blitViews) {
      if (entry.handle && entry.format == format && entry.mip == mip &&
          entry.layer == layer && entry.sampled == sampled) {
         *out = entry.handle;
         return S_OK;
      }
      if (!entry.handle) slot = &entry;
   }
   VkImageViewCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
   info.image = res->storage.image;
   info.viewType = VK_IMAGE_VIEW_TYPE_2D;
   info.format = format;
   info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, layer, 1};
   if (sampled && (format_info(res->format)->flags & FMT_ALPHA_ONE))
      info.components.a = VK_COMPONENT_SWIZZLE_ONE;
   VkImageView view = VK_NULL_HANDLE;
   HRESULT hr = blit_error(dev, dev->vk.CreateImageView(dev->vkdev, &info, nullptr, &view));
   if (FAILED(hr)) return hr;
   if (!slot) {
      slot = &res->storage.blitViews[0];
      defer(dev, DeferredType::ImageView, (uint64_t)slot->handle, res->storage.lastUse,
            res->storage.image);
   }
   *slot = {view, format, mip, layer, sampled};
   *out = view;
   return S_OK;
}

#ifdef TGD3D_RED_PRIMARY_CONTROL
/* Diagnostic only: keep the runtime's backing and presentation callbacks,
 * but replace rotation with a full portrait clear. No source sampling,
 * rotation shaders, descriptors or draw commands participate in this copy. */
HRESULT red_primary_clear(Device *dev, Resource *res, UINT subresource)
{
   if (FAILED(dev->lost)) return dev->lost;
   if (res->dev != dev || res->kind != ResKind::Image ||
       res->imageType != VK_IMAGE_TYPE_2D || !res->storage.image || subresource ||
       res->width != 720 || res->height != 1280 || res->mips != 1 || res->arraySize != 1 ||
       !(res->bind & D3D10_DDI_BIND_RENDER_TARGET))
      return DXGI_ERROR_UNSUPPORTED;
   const VkFormat format = blit_format(res);
   if (format != VK_FORMAT_B8G8R8A8_UNORM)
      return DXGI_ERROR_UNSUPPORTED;
   const uint64_t allocation = runtime_allocation(res->storage.runtime);
   const uint64_t extent = uint64_t(res->width) << 32 | res->height;
   trace(TGD3D_TRACE_RED_PRIMARY, E_PENDING, allocation, extent);
   VkImageView view = VK_NULL_HANDLE;
   HRESULT hr = blit_view(dev, res, 0, false, format, &view);
   if (FAILED(hr)) return hr;
   if (!cmd_begin(dev, &res->storage)) return FAILED(dev->lost) ? dev->lost : E_OUTOFMEMORY;
   image_use(dev, res->storage, true);
   VkRenderingAttachmentInfo attachment = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
   attachment.imageView = view;
   attachment.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
   attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
   attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
   attachment.clearValue.color.float32[0] = attachment.clearValue.color.float32[3] = 1.0f;
   VkRenderingInfo render = {VK_STRUCTURE_TYPE_RENDERING_INFO};
   render.renderArea = {{0, 0}, {720, 1280}};
   render.layerCount = render.colorAttachmentCount = 1;
   render.pColorAttachments = &attachment;
   dev->vk.CmdBeginRendering(dev->cmd, &render);
   dev->vk.CmdEndRendering(dev->cmd);
   dev->stats.renderPasses++;
   trace(TGD3D_TRACE_RED_PRIMARY, S_OK, allocation, extent); // Recorded, not completed.
   return S_OK;
}
#endif

HRESULT rotated_blit(Device *dev, const DXGI_DDI_ARG_BLT *args, const RECT *source)
{
   Resource *src = reinterpret_cast<Resource *>(args->hSrcResource);
   Resource *dst = reinterpret_cast<Resource *>(args->hDstResource);
   const UINT rotation = (UINT)args->Rotate - (UINT)DXGI_DDI_MODE_ROTATION_IDENTITY;
   if (rotation < 1 || rotation > 3 || src->imageType != VK_IMAGE_TYPE_2D ||
       dst->imageType != VK_IMAGE_TYPE_2D || src->storage.image == dst->storage.image ||
       !(src->bind & (D3D10_DDI_BIND_PRESENT | D3D10_DDI_BIND_RENDER_TARGET |
                      D3D10_DDI_BIND_SHADER_RESOURCE)) ||
       !(dst->bind & D3D10_DDI_BIND_RENDER_TARGET))
      return DXGI_ERROR_UNSUPPORTED;
#ifdef TGD3D_RED_PRIMARY_CONTROL
   return red_primary_clear(dev, dst, args->DstSubresource);
#endif
   const VkFormat source_format = blit_format(src), target_format = blit_format(dst);
   if (source_format == VK_FORMAT_UNDEFINED || target_format == VK_FORMAT_UNDEFINED ||
       /* Float/display conversion needs DXGI's implicit gamma conversion. It
        * is not advertised as implemented by this bounded rotation path. */
       ((source_format == VK_FORMAT_R16G16B16A16_SFLOAT) !=
        (target_format == VK_FORMAT_R16G16B16A16_SFLOAT)))
      return DXGI_ERROR_UNSUPPORTED;
   HRESULT hr = blit_shaders(dev, rotation);
   VkImageView source_view = VK_NULL_HANDLE, target_view = VK_NULL_HANDLE;
   if (SUCCEEDED(hr)) hr = blit_view(dev, src, args->SrcSubresource, true, source_format, &source_view);
   if (SUCCEEDED(hr)) hr = blit_view(dev, dst, args->DstSubresource, false, target_format, &target_view);
   if (FAILED(hr)) return hr;
   /* Split before allocating descriptors: a split retires the old pools. */
   VkCommandBuffer cmd = cmd_begin(dev, &dst->storage);
   if (!cmd) return FAILED(dev->lost) ? dev->lost : E_OUTOFMEMORY;
   VkDescriptorSet set = descriptor_set_alloc(dev, SET_TEX);
   if (!set) return E_OUTOFMEMORY;

   const UINT imageWidth = src->mipWidth(args->SrcSubresource % src->mips);
   const UINT imageHeight = src->mipHeight(args->SrcSubresource % src->mips);
   const UINT sw = source ? source->right - source->left : imageWidth;
   const UINT sh = source ? source->bottom - source->top : imageHeight;
   const float region[4] = {source ? float(source->left) / imageWidth : 0.0f,
      source ? float(source->top) / imageHeight : 0.0f,
      float(sw) / imageWidth, float(sh) / imageHeight};
   const UINT dw = args->DstRight - args->DstLeft, dh = args->DstBottom - args->DstTop;
   const bool quarter = rotation & 1;
   const bool scaled = dw != (quarter ? sh : sw) || dh != (quarter ? sw : sh);
   VkDescriptorImageInfo images[] = {
      {scaled ? dev->defaultSampler : dev->blitPointSampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED},
      {VK_NULL_HANDLE, source_view, VK_IMAGE_LAYOUT_GENERAL},
   };
   VkWriteDescriptorSet writes[2] = {};
   for (UINT i = 0; i < 2; i++) {
      writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[i].dstSet = set;
      writes[i].dstBinding = STAGE_PS * SET1_STRIDE + (i ? D3D_SAMPLER_SLOTS : 0);
      writes[i].descriptorCount = 1;
      writes[i].descriptorType = i ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLER;
      writes[i].pImageInfo = &images[i];
   }
   dev->vk.UpdateDescriptorSets(dev->vkdev, 2, writes, 0, nullptr);
   image_use(dev, src->storage, false);
   barrier_for_read(dev, src->storage.writeSerial);
   image_use(dev, dst->storage, true);
   cmd = dev->cmd;
   const auto &vk = dev->vk;
   VkRenderingAttachmentInfo attachment = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
   attachment.imageView = target_view;
   attachment.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
   attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
   attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
   VkRenderingInfo render = {VK_STRUCTURE_TYPE_RENDERING_INFO};
   render.renderArea = {{(int32_t)args->DstLeft, (int32_t)args->DstTop}, {dw, dh}};
   render.layerCount = 1;
   render.colorAttachmentCount = 1;
   render.pColorAttachments = &attachment;
   vk.CmdBeginRendering(cmd, &render);
   VkShaderStageFlagBits stages[5] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT};
   VkShaderEXT shaders[5] = {dev->blitVertex[rotation], dev->blitFragment};
   UINT count = 2;
   if (dev->caps->geometryShader) stages[count++] = VK_SHADER_STAGE_GEOMETRY_BIT;
   if (dev->caps->tessellationShader) {
      stages[count++] = VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
      stages[count++] = VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
   }
   vk.CmdBindShadersEXT(cmd, count, stages, shaders);
   vk.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, dev->blitPipelineLayout,
                           SET_TEX, 1, &set, 0, nullptr);
   vk.CmdPushConstants(cmd, dev->blitPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
                       0, sizeof(region), region);
   vk.CmdSetVertexInputEXT(cmd, 0, nullptr, 0, nullptr);
   vk.CmdSetPrimitiveTopology(cmd, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
   vk.CmdSetPrimitiveRestartEnable(cmd, VK_FALSE);
   vk.CmdSetRasterizerDiscardEnable(cmd, VK_FALSE);
   vk.CmdSetRasterizationSamplesEXT(cmd, VK_SAMPLE_COUNT_1_BIT);
   vk.CmdSetPolygonModeEXT(cmd, VK_POLYGON_MODE_FILL);
   vk.CmdSetCullMode(cmd, VK_CULL_MODE_NONE);
   vk.CmdSetFrontFace(cmd, VK_FRONT_FACE_COUNTER_CLOCKWISE);
   vk.CmdSetLineWidth(cmd, 1.0f);
   vk.CmdSetDepthBiasEnable(cmd, VK_FALSE);
   vk.CmdSetDepthClampEnableEXT(cmd, VK_FALSE);
   vk.CmdSetDepthClipEnableEXT(cmd, VK_TRUE);
   vk.CmdSetDepthBoundsTestEnable(cmd, VK_FALSE);
   vk.CmdSetDepthTestEnable(cmd, VK_FALSE);
   vk.CmdSetDepthWriteEnable(cmd, VK_FALSE);
   vk.CmdSetStencilTestEnable(cmd, VK_FALSE);
   vk.CmdSetAlphaToCoverageEnableEXT(cmd, VK_FALSE);
   vk.CmdSetAlphaToOneEnableEXT(cmd, VK_FALSE);
   vk.CmdSetLogicOpEnableEXT(cmd, VK_FALSE);
   const VkSampleMask sample_mask = ~0u;
   vk.CmdSetSampleMaskEXT(cmd, VK_SAMPLE_COUNT_1_BIT, &sample_mask);
   const VkBool32 blend = VK_FALSE;
   const VkColorBlendEquationEXT replace = {VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD,
                                           VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD};
   const VkColorComponentFlags mask = (format_info(dst->format)->flags & FMT_ALPHA_ONE) ? 0x7 : 0xf;
   vk.CmdSetColorBlendEnableEXT(cmd, 0, 1, &blend);
   vk.CmdSetColorBlendEquationEXT(cmd, 0, 1, &replace);
   vk.CmdSetColorWriteMaskEXT(cmd, 0, 1, &mask);
   if (dev->caps->transformFeedback && vk.CmdSetRasterizationStreamEXT)
      vk.CmdSetRasterizationStreamEXT(cmd, 0);
   const VkViewport viewport = {(float)args->DstLeft, (float)args->DstTop,
                                 (float)dw, (float)dh, 0.0f, 1.0f};
   vk.CmdSetViewportWithCount(cmd, 1, &viewport);
   vk.CmdSetScissorWithCount(cmd, 1, &render.renderArea);
   vk.CmdDraw(cmd, 3, 1, 0, 0);
   vk.CmdEndRendering(cmd);
   // The internal copy is outside D3D queries and must not leak dynamic state.
   dev->dirty = DIRTY_ALL;
   for (auto &stage : dev->stage) { stage.bound = VK_NULL_HANDLE; stage.cbDirty = ~0u; }
   dev->textureSet = dev->texelBufferSet = VK_NULL_HANDLE;
   dev->stats.renderPasses++;
   dev->stats.copyBytes += (uint64_t)dw * dh * format_info(dst->format)->bytes;
   return S_OK;
}

}
