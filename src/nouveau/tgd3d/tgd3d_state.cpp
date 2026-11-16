/* SPDX-License-Identifier: MIT
 *
 * State objects and binding entry points. D3D10 state objects hold their
 * description; the draw path turns bound state into Vulkan dynamic state.
 */
#include "tgd3d.h"

#include <float.h>
#include <string.h>

namespace tgd3d {

template <typename T, typename H> static T *obj(H handle)
{
   return static_cast<T *>(handle.pDrvPrivate);
}

/* ------------------------------------------------------------------ */
/* Blend                                                               */

static VkBlendFactor blend_factor(D3D10_DDI_BLEND blend, bool alpha)
{
   switch (blend) {
   case D3D10_DDI_BLEND_ZERO: return VK_BLEND_FACTOR_ZERO;
   case D3D10_DDI_BLEND_ONE: return VK_BLEND_FACTOR_ONE;
   case D3D10_DDI_BLEND_SRC_COLOR: return VK_BLEND_FACTOR_SRC_COLOR;
   case D3D10_DDI_BLEND_INV_SRC_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
   case D3D10_DDI_BLEND_SRC_ALPHA: return VK_BLEND_FACTOR_SRC_ALPHA;
   case D3D10_DDI_BLEND_INV_SRC_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
   case D3D10_DDI_BLEND_DEST_ALPHA: return VK_BLEND_FACTOR_DST_ALPHA;
   case D3D10_DDI_BLEND_INV_DEST_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
   case D3D10_DDI_BLEND_DEST_COLOR: return VK_BLEND_FACTOR_DST_COLOR;
   case D3D10_DDI_BLEND_INV_DEST_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
   case D3D10_DDI_BLEND_SRC_ALPHASAT: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
   case D3D10_DDI_BLEND_BLEND_FACTOR:
      return alpha ? VK_BLEND_FACTOR_CONSTANT_ALPHA : VK_BLEND_FACTOR_CONSTANT_COLOR;
   case D3D10_DDI_BLEND_INVBLEND_FACTOR:
      return alpha ? VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
   case D3D10_DDI_BLEND_SRC1_COLOR: return VK_BLEND_FACTOR_SRC1_COLOR;
   case D3D10_DDI_BLEND_INV_SRC1_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
   case D3D10_DDI_BLEND_SRC1_ALPHA: return VK_BLEND_FACTOR_SRC1_ALPHA;
   case D3D10_DDI_BLEND_INV_SRC1_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
   default: return VK_BLEND_FACTOR_ONE;
   }
}

static VkBlendOp blend_op(D3D10_DDI_BLEND_OP op)
{
   switch (op) {
   case D3D10_DDI_BLEND_OP_SUBTRACT: return VK_BLEND_OP_SUBTRACT;
   case D3D10_DDI_BLEND_OP_REV_SUBTRACT: return VK_BLEND_OP_REVERSE_SUBTRACT;
   case D3D10_DDI_BLEND_OP_MIN: return VK_BLEND_OP_MIN;
   case D3D10_DDI_BLEND_OP_MAX: return VK_BLEND_OP_MAX;
   default: return VK_BLEND_OP_ADD;
   }
}

static bool is_src1(D3D10_DDI_BLEND blend)
{
   return blend >= D3D10_DDI_BLEND_SRC1_COLOR && blend <= D3D10_DDI_BLEND_INV_SRC1_ALPHA;
}

static bool is_factor(D3D10_DDI_BLEND blend)
{
   return blend == D3D10_DDI_BLEND_BLEND_FACTOR || blend == D3D10_DDI_BLEND_INVBLEND_FACTOR;
}

bool blend_uses_dual_source(const BlendState *state)
{
   if (!state)
      return false;
   const auto &d = state->desc;
   bool enabled = false;
   for (BOOL e : d.BlendEnable)
      enabled |= e != 0;
   return enabled && (is_src1(d.SrcBlend) || is_src1(d.DestBlend) || is_src1(d.SrcBlendAlpha) ||
                      is_src1(d.DestBlendAlpha));
}

static SIZE_T APIENTRY CalcPrivateBlendStateSize(D3D10DDI_HDEVICE, const D3D10_DDI_BLEND_DESC *)
{
   return sizeof(BlendState);
}

static void APIENTRY CreateBlendState(D3D10DDI_HDEVICE, const D3D10_DDI_BLEND_DESC *desc,
                                      D3D10DDI_HBLENDSTATE handle, D3D10DDI_HRTBLENDSTATE)
{
   BlendState *state = new (handle.pDrvPrivate) BlendState();
   state->desc = *desc;
   state->equation.srcColorBlendFactor = blend_factor(desc->SrcBlend, false);
   state->equation.dstColorBlendFactor = blend_factor(desc->DestBlend, false);
   state->equation.colorBlendOp = blend_op(desc->BlendOp);
   state->equation.srcAlphaBlendFactor = blend_factor(desc->SrcBlendAlpha, true);
   state->equation.dstAlphaBlendFactor = blend_factor(desc->DestBlendAlpha, true);
   state->equation.alphaBlendOp = blend_op(desc->BlendOpAlpha);
   state->usesBlendFactor = is_factor(desc->SrcBlend) || is_factor(desc->DestBlend) ||
                            is_factor(desc->SrcBlendAlpha) || is_factor(desc->DestBlendAlpha);
}

static void APIENTRY DestroyBlendState(D3D10DDI_HDEVICE hDevice, D3D10DDI_HBLENDSTATE handle)
{
   Device *dev = D(hDevice);
   if (dev->blend == obj<BlendState>(handle)) {
      dev->blend = nullptr;
      dev->dirty |= DIRTY_BLEND;
   }
}

static void APIENTRY SetBlendState(D3D10DDI_HDEVICE hDevice, D3D10DDI_HBLENDSTATE handle,
                                   const FLOAT factor[4], UINT sample_mask)
{
   Device *dev = D(hDevice);
   dev->blend = obj<BlendState>(handle);
   memcpy(dev->blendFactor, factor, sizeof(dev->blendFactor));
   dev->sampleMask = sample_mask;
   dev->dirty |= DIRTY_BLEND | DIRTY_SHADERS;   /* dual-source selects a pixel shader variant */
}

/* ------------------------------------------------------------------ */
/* Depth-stencil, rasterizer                                           */

static SIZE_T APIENTRY CalcPrivateDepthStencilStateSize(D3D10DDI_HDEVICE,
                                                        const D3D10_DDI_DEPTH_STENCIL_DESC *)
{
   return sizeof(DepthStencilState);
}

static void APIENTRY CreateDepthStencilState(D3D10DDI_HDEVICE, const D3D10_DDI_DEPTH_STENCIL_DESC *desc,
                                             D3D10DDI_HDEPTHSTENCILSTATE handle,
                                             D3D10DDI_HRTDEPTHSTENCILSTATE)
{
   (new (handle.pDrvPrivate) DepthStencilState())->desc = *desc;
}

static void APIENTRY DestroyDepthStencilState(D3D10DDI_HDEVICE hDevice,
                                              D3D10DDI_HDEPTHSTENCILSTATE handle)
{
   Device *dev = D(hDevice);
   if (dev->depthStencil == obj<DepthStencilState>(handle)) {
      dev->depthStencil = nullptr;
      dev->dirty |= DIRTY_DEPTH_STENCIL;
   }
}

static void APIENTRY SetDepthStencilState(D3D10DDI_HDEVICE hDevice, D3D10DDI_HDEPTHSTENCILSTATE handle,
                                          UINT stencil_ref)
{
   Device *dev = D(hDevice);
   dev->depthStencil = obj<DepthStencilState>(handle);
   dev->stencilRef = stencil_ref;
   dev->dirty |= DIRTY_DEPTH_STENCIL;
}

static SIZE_T APIENTRY CalcPrivateRasterizerStateSize(D3D10DDI_HDEVICE,
                                                      const D3D10_DDI_RASTERIZER_DESC *)
{
   return sizeof(RasterizerState);
}

static void APIENTRY CreateRasterizerState(D3D10DDI_HDEVICE, const D3D10_DDI_RASTERIZER_DESC *desc,
                                           D3D10DDI_HRASTERIZERSTATE handle,
                                           D3D10DDI_HRTRASTERIZERSTATE)
{
   (new (handle.pDrvPrivate) RasterizerState())->desc = *desc;
}

static void APIENTRY DestroyRasterizerState(D3D10DDI_HDEVICE hDevice, D3D10DDI_HRASTERIZERSTATE handle)
{
   Device *dev = D(hDevice);
   if (dev->rasterizer == obj<RasterizerState>(handle)) {
      dev->rasterizer = nullptr;
      dev->dirty |= DIRTY_RASTERIZER | DIRTY_SCISSORS;
   }
}

static void APIENTRY SetRasterizerState(D3D10DDI_HDEVICE hDevice, D3D10DDI_HRASTERIZERSTATE handle)
{
   Device *dev = D(hDevice);
   dev->rasterizer = obj<RasterizerState>(handle);
   dev->dirty |= DIRTY_RASTERIZER | DIRTY_SCISSORS;
}

/* ------------------------------------------------------------------ */
/* Samplers                                                            */

static VkSamplerAddressMode address_mode(D3D10_DDI_TEXTURE_ADDRESS_MODE mode)
{
   switch (mode) {
   case D3D10_DDI_TEXTURE_ADDRESS_MIRROR: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
   case D3D10_DDI_TEXTURE_ADDRESS_CLAMP: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
   case D3D10_DDI_TEXTURE_ADDRESS_BORDER: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
   case D3D10_DDI_TEXTURE_ADDRESS_MIRRORONCE: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
   default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
   }
}

static SIZE_T APIENTRY CalcPrivateSamplerSize(D3D10DDI_HDEVICE, const D3D10_DDI_SAMPLER_DESC *)
{
   return sizeof(Sampler);
}

static void APIENTRY CreateSampler(D3D10DDI_HDEVICE hDevice, const D3D10_DDI_SAMPLER_DESC *desc,
                                   D3D10DDI_HSAMPLER handle, D3D10DDI_HRTSAMPLER)
{
   Device *dev = D(hDevice);
   Sampler *sampler = new (handle.pDrvPrivate) Sampler();
   sampler->dev = dev;
   sampler->sampler = VK_NULL_HANDLE;
   const UINT filter = (UINT)desc->Filter;
   const bool anisotropic = (filter & 0x40) != 0;
   const bool comparison = (filter & 0x80) != 0;
   VkSamplerCreateInfo info = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
   info.magFilter = (anisotropic || (filter & 0x4)) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
   info.minFilter = (anisotropic || (filter & 0x10)) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
   info.mipmapMode = (anisotropic || (filter & 0x1)) ? VK_SAMPLER_MIPMAP_MODE_LINEAR :
                                                       VK_SAMPLER_MIPMAP_MODE_NEAREST;
   info.addressModeU = address_mode(desc->AddressU);
   info.addressModeV = address_mode(desc->AddressV);
   info.addressModeW = address_mode(desc->AddressW);
   info.mipLodBias = desc->MipLODBias;
   info.anisotropyEnable = anisotropic;
   info.maxAnisotropy = 1.0f;
   if (anisotropic) {
      info.maxAnisotropy = (float)(desc->MaxAnisotropy ? desc->MaxAnisotropy : 1);
      if (info.maxAnisotropy > dev->caps->maxSamplerAnisotropy)
         info.maxAnisotropy = dev->caps->maxSamplerAnisotropy;
   }
   info.compareEnable = comparison;
   info.compareOp = comparison && desc->ComparisonFunc >= 1 && desc->ComparisonFunc <= 8 ?
      (VkCompareOp)(desc->ComparisonFunc - 1) : VK_COMPARE_OP_ALWAYS;
   info.minLod = desc->MinLOD < 0.0f ? 0.0f : desc->MinLOD;
   info.maxLod = desc->MaxLOD > 1000.0f ? VK_LOD_CLAMP_NONE : desc->MaxLOD;
   if (info.maxLod < info.minLod)
      info.maxLod = info.minLod;
   VkSamplerCustomBorderColorCreateInfoEXT border = {
      VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT};
   const bool uses_border = info.addressModeU == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER ||
                            info.addressModeV == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER ||
                            info.addressModeW == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
   const FLOAT *c = desc->BorderColor;
   info.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
   if (uses_border) {
      if (c[0] == 0 && c[1] == 0 && c[2] == 0 && c[3] == 0)
         info.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
      else if (c[0] == 0 && c[1] == 0 && c[2] == 0 && c[3] == 1)
         info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
      else if (c[0] == 1 && c[1] == 1 && c[2] == 1 && c[3] == 1)
         info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
      else {
         info.borderColor = VK_BORDER_COLOR_FLOAT_CUSTOM_EXT;
         memcpy(border.customBorderColor.float32, c, sizeof(float) * 4);
         border.format = VK_FORMAT_UNDEFINED;
         info.pNext = &border;
      }
   }
   if (dev->vk.CreateSampler(dev->vkdev, &info, nullptr, &sampler->sampler) != VK_SUCCESS)
      TG_ERROR(dev, E_OUTOFMEMORY);
}

static void APIENTRY DestroySampler(D3D10DDI_HDEVICE hDevice, D3D10DDI_HSAMPLER handle)
{
   Device *dev = D(hDevice);
   Sampler *sampler = obj<Sampler>(handle);
   for (auto &stage : dev->stage)
      for (auto &bound : stage.sampler)
         if (bound == sampler) {
            bound = nullptr;
            dev->dirty |= DIRTY_TEXTURES;
         }
   defer(dev, DeferredType::Sampler, (uint64_t)sampler->sampler, dev->lastSeq());
}

/* ------------------------------------------------------------------ */
/* Input assembler                                                     */

static SIZE_T APIENTRY CalcPrivateElementLayoutSize(D3D10DDI_HDEVICE,
                                                    const D3D10DDIARG_CREATEELEMENTLAYOUT *)
{
   return sizeof(ElementLayout);
}

static void APIENTRY CreateElementLayout(D3D10DDI_HDEVICE hDevice,
                                         const D3D10DDIARG_CREATEELEMENTLAYOUT *args,
                                         D3D10DDI_HELEMENTLAYOUT handle, D3D10DDI_HRTELEMENTLAYOUT)
{
   Device *dev = D(hDevice);
   ElementLayout *layout = new (handle.pDrvPrivate) ElementLayout();
   layout->count = 0;
   layout->numericClasses = 0;
   layout->registerMask = 0;
   if (args->NumElements > 32) {
      TG_ERROR(dev, E_INVALIDARG);
      return;
   }
   for (UINT i = 0; i < args->NumElements; i++) {
      const auto &e = args->pVertexElements[i];
      if (e.InputRegister >= 16 || e.InputSlot >= 16 ||
          vertex_vk_format(e.Format) == VK_FORMAT_UNDEFINED) {
         layout->count = 0;
         TG_ERROR(dev, E_INVALIDARG);
         return;
      }
      layout->elements[layout->count++] = e;
      layout->numericClasses |= format_numeric_class(e.Format) << (2 * e.InputRegister);
      layout->registerMask |= 1u << e.InputRegister;
   }
}

static void APIENTRY DestroyElementLayout(D3D10DDI_HDEVICE hDevice, D3D10DDI_HELEMENTLAYOUT handle)
{
   Device *dev = D(hDevice);
   if (dev->inputLayout == obj<ElementLayout>(handle)) {
      dev->inputLayout = nullptr;
      dev->dirty |= DIRTY_VERTEX_INPUT | DIRTY_SHADERS;
   }
}

static void APIENTRY IaSetInputLayout(D3D10DDI_HDEVICE hDevice, D3D10DDI_HELEMENTLAYOUT handle)
{
   Device *dev = D(hDevice);
   dev->inputLayout = obj<ElementLayout>(handle);
   dev->dirty |= DIRTY_VERTEX_INPUT | DIRTY_SHADERS;   /* integer inputs select a VS variant */
}

static void APIENTRY IaSetVertexBuffers(D3D10DDI_HDEVICE hDevice, UINT start, UINT count,
                                        const D3D10DDI_HRESOURCE *buffers, const UINT *strides,
                                        const UINT *offsets)
{
   Device *dev = D(hDevice);
   for (UINT i = 0; i < count && start + i < 16; i++) {
      VertexBinding &vb = dev->vb[start + i];
      Resource *buffer = R(buffers[i]);
      if (vb.stride != strides[i])
         dev->dirty |= DIRTY_VERTEX_INPUT;
      vb.buffer = buffer;
      vb.stride = strides[i];
      vb.offset = offsets[i];
   }
   dev->dirty |= DIRTY_VERTEX_BUFFERS;
}

static void APIENTRY IaSetIndexBuffer(D3D10DDI_HDEVICE hDevice, D3D10DDI_HRESOURCE buffer,
                                      DXGI_FORMAT format, UINT offset)
{
   Device *dev = D(hDevice);
   dev->indexBuffer = R(buffer);
   dev->indexFormat = format;
   dev->indexOffset = offset;
   dev->dirty |= DIRTY_INDEX_BUFFER;
}

static void APIENTRY IaSetTopology(D3D10DDI_HDEVICE hDevice, D3D10_DDI_PRIMITIVE_TOPOLOGY topology)
{
   Device *dev = D(hDevice);
   dev->topology = topology;
   dev->dirty |= DIRTY_TOPOLOGY;
}

/* ------------------------------------------------------------------ */
/* Shader stage bindings                                               */

void set_constant_buffers(Device *dev, Stage stage, UINT start, UINT count,
                          const D3D10DDI_HRESOURCE *buffers, const UINT *first, const UINT *sizes)
{
   if (stage >= STAGE_COUNT || start > D3D_CB_SLOTS || count > D3D_CB_SLOTS - start ||
       (count && !buffers)) {
      TG_ERROR(dev, E_INVALIDARG);
      return;
   }
   /* Validate the whole request before changing state. Ranges are allowed to
    * extend beyond the allocation: Vulkan's robust descriptor bound supplies
    * zeros outside its intersection with the requested D3D window. */
   for (UINT i = 0; i < count; ++i) {
      Resource *buffer = R(buffers[i]);
      if ((buffer && (buffer->dev != dev || buffer->kind != ResKind::Buffer)) ||
          (first && (first[i] & 15)) ||
          (sizes && ((sizes[i] & 15) || sizes[i] > 4096))) {
         TG_ERROR(dev, E_INVALIDARG);
         return;
      }
   }
   StageBindings &s = dev->stage[stage];
   for (UINT i = 0; i < count; i++) {
      Resource *buffer = R(buffers[i]);
      const VkDeviceSize offset = first ? VkDeviceSize(first[i]) * 16 : 0;
      const VkDeviceSize bytes = sizes ? VkDeviceSize(sizes[i]) * 16 : 65536;
      if (s.cb[start + i] != buffer || s.cbOffset[start + i] != offset ||
          s.cbBytes[start + i] != bytes) {
         s.cb[start + i] = buffer;
         s.cbOffset[start + i] = offset;
         s.cbBytes[start + i] = bytes;
         s.cbDirty |= 1u << (start + i);
      }
   }
}

static void set_shader_resources(Device *dev, Stage stage, UINT start, UINT count,
                                 const D3D10DDI_HSHADERRESOURCEVIEW *views)
{
   StageBindings &s = dev->stage[stage];
   for (UINT i = 0; i < count && start + i < D3D_SRV_SLOTS; i++) {
      View *view = static_cast<View *>(views[i].pDrvPrivate);
      if (s.srv[start + i] != view) {
         s.srv[start + i] = view;
         dev->dirty |= DIRTY_TEXTURES | DIRTY_TEXEL_BUFFERS;
      }
   }
}

static void set_samplers(Device *dev, Stage stage, UINT start, UINT count,
                         const D3D10DDI_HSAMPLER *samplers)
{
   StageBindings &s = dev->stage[stage];
   for (UINT i = 0; i < count && start + i < D3D_SAMPLER_SLOTS; i++) {
      Sampler *sampler = obj<Sampler>(samplers[i]);
      if (s.sampler[start + i] != sampler) {
         s.sampler[start + i] = sampler;
         dev->dirty |= DIRTY_TEXTURES;
      }
   }
}

#define STAGE_ENTRY_POINTS(Name, stage) \
   static void APIENTRY Name##SetConstantBuffers(D3D10DDI_HDEVICE h, UINT start, UINT count, \
                                                 const D3D10DDI_HRESOURCE *b) \
   { set_constant_buffers(D(h), stage, start, count, b); } \
   static void APIENTRY Name##SetShaderResources(D3D10DDI_HDEVICE h, UINT start, UINT count, \
                                                 const D3D10DDI_HSHADERRESOURCEVIEW *v) \
   { set_shader_resources(D(h), stage, start, count, v); } \
   static void APIENTRY Name##SetSamplers(D3D10DDI_HDEVICE h, UINT start, UINT count, \
                                          const D3D10DDI_HSAMPLER *s) \
   { set_samplers(D(h), stage, start, count, s); }

STAGE_ENTRY_POINTS(Vs, STAGE_VS)
STAGE_ENTRY_POINTS(Gs, STAGE_GS)
STAGE_ENTRY_POINTS(Ps, STAGE_PS)

/* ------------------------------------------------------------------ */
/* Output merger, viewports                                            */

static void APIENTRY SetRenderTargets(D3D10DDI_HDEVICE hDevice, const D3D10DDI_HRENDERTARGETVIEW *views,
                                      UINT count, UINT clear_slots, D3D10DDI_HDEPTHSTENCILVIEW hDsv)
{
   Device *dev = D(hDevice);
   View *next[D3D10_DDI_SIMULTANEOUS_RENDER_TARGET_COUNT];
   for (UINT i = 0; i < D3D10_DDI_SIMULTANEOUS_RENDER_TARGET_COUNT; i++) {
      if (i < count)
         next[i] = static_cast<View *>(views[i].pDrvPrivate);
      else if (i < count + clear_slots)
         next[i] = nullptr;
      else
         next[i] = dev->rtv[i];
   }
   View *dsv = static_cast<View *>(hDsv.pDrvPrivate);
   if (!memcmp(next, dev->rtv, sizeof(next)) && dsv == dev->dsv)
      return;
   end_rendering(dev);
   memcpy(dev->rtv, next, sizeof(next));
   dev->dsv = dsv;
   /* A different DSV can change the write masks without changing the bound
    * depth/stencil state object (read-only depth or stencil planes). */
   dev->dirty |= DIRTY_BLEND | DIRTY_SCISSORS | DIRTY_SHADERS | DIRTY_DEPTH_STENCIL;
}

static void APIENTRY SetViewports(D3D10DDI_HDEVICE hDevice, UINT count, UINT clear,
                                  const D3D10_DDI_VIEWPORT *viewports)
{
   Device *dev = D(hDevice);
   (void)clear;
   dev->viewportCount = count > 16 ? 16 : count;
   if (dev->viewportCount)
      memcpy(dev->viewports, viewports, sizeof(*viewports) * dev->viewportCount);
   dev->dirty |= DIRTY_VIEWPORTS | DIRTY_SCISSORS;
}

static void APIENTRY SetScissorRects(D3D10DDI_HDEVICE hDevice, UINT count, UINT clear,
                                     const D3D10_DDI_RECT *rects)
{
   Device *dev = D(hDevice);
   (void)clear;
   dev->scissorCount = count > 16 ? 16 : count;
   if (dev->scissorCount)
      memcpy(dev->scissors, rects, sizeof(*rects) * dev->scissorCount);
   dev->dirty |= DIRTY_SCISSORS;
}

static void APIENTRY SoSetTargets(D3D10DDI_HDEVICE hDevice, UINT count, UINT clear,
                                  const D3D10DDI_HRESOURCE *buffers, const UINT *offsets)
{
   (void)clear;
   (void)offsets;
   /* Stream output is not implemented. Unbinding is always valid; binding a
    * target is reported instead of silently dropping the captured data. */
   for (UINT i = 0; i < count; i++) {
      if (buffers[i].pDrvPrivate) {
         TG_ERROR(D(hDevice), E_NOTIMPL);
         return;
      }
   }
}

static void APIENTRY SetPredication(D3D10DDI_HDEVICE hDevice, D3D10DDI_HQUERY query, BOOL value)
{
   (void)value;
   if (query.pDrvPrivate)
      TG_ERROR(D(hDevice), E_NOTIMPL);
}

static void APIENTRY SetTextFilterSize(D3D10DDI_HDEVICE, UINT, UINT)
{
   /* Only meaningful for the monochrome text filter, which is not offered. */
}

void state_fill_table(D3D10DDI_DEVICEFUNCS *t)
{
   t->pfnCalcPrivateBlendStateSize = CalcPrivateBlendStateSize;
   t->pfnCreateBlendState = CreateBlendState;
   t->pfnDestroyBlendState = DestroyBlendState;
   t->pfnSetBlendState = SetBlendState;
   t->pfnCalcPrivateDepthStencilStateSize = CalcPrivateDepthStencilStateSize;
   t->pfnCreateDepthStencilState = CreateDepthStencilState;
   t->pfnDestroyDepthStencilState = DestroyDepthStencilState;
   t->pfnSetDepthStencilState = SetDepthStencilState;
   t->pfnCalcPrivateRasterizerStateSize = CalcPrivateRasterizerStateSize;
   t->pfnCreateRasterizerState = CreateRasterizerState;
   t->pfnDestroyRasterizerState = DestroyRasterizerState;
   t->pfnSetRasterizerState = SetRasterizerState;
   t->pfnCalcPrivateSamplerSize = CalcPrivateSamplerSize;
   t->pfnCreateSampler = CreateSampler;
   t->pfnDestroySampler = DestroySampler;
   t->pfnCalcPrivateElementLayoutSize = CalcPrivateElementLayoutSize;
   t->pfnCreateElementLayout = CreateElementLayout;
   t->pfnDestroyElementLayout = DestroyElementLayout;
   t->pfnIaSetInputLayout = IaSetInputLayout;
   t->pfnIaSetVertexBuffers = IaSetVertexBuffers;
   t->pfnIaSetIndexBuffer = IaSetIndexBuffer;
   t->pfnIaSetTopology = IaSetTopology;
   t->pfnVsSetConstantBuffers = VsSetConstantBuffers;
   t->pfnVsSetShaderResources = VsSetShaderResources;
   t->pfnVsSetSamplers = VsSetSamplers;
   t->pfnGsSetConstantBuffers = GsSetConstantBuffers;
   t->pfnGsSetShaderResources = GsSetShaderResources;
   t->pfnGsSetSamplers = GsSetSamplers;
   t->pfnPsSetConstantBuffers = PsSetConstantBuffers;
   t->pfnPsSetShaderResources = PsSetShaderResources;
   t->pfnPsSetSamplers = PsSetSamplers;
   t->pfnSetRenderTargets = SetRenderTargets;
   t->pfnSetViewports = SetViewports;
   t->pfnSetScissorRects = SetScissorRects;
   t->pfnSoSetTargets = SoSetTargets;
   t->pfnSetPredication = SetPredication;
   t->pfnSetTextFilterSize = SetTextFilterSize;
}

}
