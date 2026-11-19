/* SPDX-License-Identifier: MIT
 * Typed adapters for the modern device table's existing D3D10 pipeline.
 * WDDM2.0 pairs its typed table with DXGI1.4 and the D3D10 pipeline level.
 * Do not cast device tables or copy descriptors with different union layouts.
 */
#include "tgd3d.h"
#include <iterator>
#include <type_traits>

namespace tgd3d {

static const D3D10DDI_DEVICEFUNCS &legacy()
{
   static const D3D10DDI_DEVICEFUNCS functions = [] {
      D3D10DDI_DEVICEFUNCS t = {};
      resource_fill_table(&t);
      state_fill_table(&t);
      shader_fill_table(&t);
      draw_fill_table(&t);
      query_fill_table(&t);
      device_fill_table(&t);
      return t;
   }();
   return functions;
}

static bool resource_desc(Device *dev, const D3D11DDIARG_CREATERESOURCE *in,
                          D3D10DDIARG_CREATERESOURCE *out)
{
   if (!in || !in->pMipInfoList || (in->BindFlags & D3D11_DDI_BIND_VIDEO_ENCODER))
      return false;
   const UINT raw = D3D11_DDI_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
   const UINT structured = D3D11_DDI_RESOURCE_MISC_BUFFER_STRUCTURED;
   if (in->MiscFlags & (raw | structured)) {
      if (in->ResourceDimension != D3D10DDIRESOURCE_BUFFER || in->Format != DXGI_FORMAT_UNKNOWN ||
          (in->MiscFlags & (raw | structured)) == (raw | structured) ||
          (in->BindFlags & D3D10_DDI_BIND_CONSTANT_BUFFER)) return false;
      if (in->MiscFlags & structured) {
         if (!in->ByteStride || (in->ByteStride & 3) || in->ByteStride > 2048 ||
             in->pMipInfoList[0].TexelWidth % in->ByteStride) return false;
      } else if (in->ByteStride || (in->pMipInfoList[0].TexelWidth & 3)) return false;
   }
   if ((in->BindFlags & D3D11_DDI_BIND_UNORDERED_ACCESS) &&
       (!dev->caps || !dev->caps->compute || in->Usage != D3D10_DDI_USAGE_DEFAULT ||
        in->MapFlags || in->pPrimaryDesc || in->SampleDesc.Count != 1 ||
        (in->BindFlags & (D3D10_DDI_BIND_CONSTANT_BUFFER | D3D10_DDI_BIND_DEPTH_STENCIL |
                         D3D11_DDI_BIND_DECODER)))) return false;
   if (in->ResourceDimension == D3D10DDIRESOURCE_BUFFER && (in->BindFlags & D3D11_DDI_BIND_DECODER)) {
      /* Runtime-created decoder inputs are opaque byte buffers. The type of
       * each occupied byte range is supplied to VideoDecoderSubmitBuffers. */
      if (!video_device_supported(dev) || in->BindFlags != D3D11_DDI_BIND_DECODER ||
          in->Format != DXGI_FORMAT_UNKNOWN || in->Usage != D3D10_DDI_USAGE_DYNAMIC ||
          in->MapFlags != D3D10_DDI_CPU_ACCESS_WRITE || in->MiscFlags || in->pPrimaryDesc)
         return false;
   } else if ((in->ByteStride && !(in->MiscFlags & structured)) || ((in->BindFlags & D3D11_DDI_BIND_DECODER) && in->Format != DXGI_FORMAT_NV12)) {
      return false;
   }
   /* The 11.1 producer does not provide the WDDM2 TextureLayout tail. Only
    * access fields belonging to the negotiated descriptor revision. */
   out->pMipInfoList = in->pMipInfoList;
   out->pInitialDataUP = in->pInitialDataUP;
   out->ResourceDimension = in->ResourceDimension;
   out->Usage = in->Usage;
   out->BindFlags = in->BindFlags;
   out->MapFlags = in->MapFlags;
   out->MiscFlags = in->MiscFlags;
   out->Format = in->Format;
   out->SampleDesc = in->SampleDesc;
   out->MipLevels = in->MipLevels;
   out->ArraySize = in->ArraySize;
   out->pPrimaryDesc = in->pPrimaryDesc;
   return true;
}

static SIZE_T APIENTRY ResourceSize(D3D10DDI_HDEVICE, const D3D11DDIARG_CREATERESOURCE *)
{
   return sizeof(Resource);
}

static void APIENTRY CreateResource(D3D10DDI_HDEVICE h, const D3D11DDIARG_CREATERESOURCE *in,
                                    D3D10DDI_HRESOURCE resource, D3D10DDI_HRTRESOURCE rt)
{
   D3D10DDIARG_CREATERESOURCE desc = {};
   if (!resource_desc(D(h), in, &desc)) {
      resource_creation_error(D(h), DXGI_ERROR_UNSUPPORTED,
         in && (in->BindFlags & D3D10_DDI_BIND_PRESENT) ? in->pPrimaryDesc : nullptr);
      return;
   }
   create_resource_with_stride(h, &desc, resource, rt,
      (in->MiscFlags & D3D11_DDI_RESOURCE_MISC_BUFFER_STRUCTURED) ? in->ByteStride : 0);
}

static void APIENTRY CreateResource20(D3D10DDI_HDEVICE h, const D3D11DDIARG_CREATERESOURCE *in,
                                      D3D10DDI_HRESOURCE resource, D3D10DDI_HRTRESOURCE rt)
{
   if (!in) { resource_creation_error(D(h), E_INVALIDARG); return; }
   /* The runtime sets ROW_MAJOR on CPU-accessible buffers even when optional
    * default-texture mapping is not advertised. Buffers and private staging
    * textures already use linear Slice storage. Optimal/default textures and
    * explicit swizzles are separate contracts; do not reinterpret them here. */
   const bool linear = in->ResourceDimension == D3D10DDIRESOURCE_BUFFER ||
      (in->Usage == D3D10_DDI_USAGE_STAGING && !in->pPrimaryDesc &&
       !(in->MiscFlags & D3D10_DDI_RESOURCE_MISC_SHARED));
   if (in->TextureLayout != D3DWDDM2_0DDI_TL_UNDEFINED &&
       !(linear && in->TextureLayout == D3DWDDM2_0DDI_TL_ROW_MAJOR)) {
      resource_creation_error(D(h), DXGI_ERROR_UNSUPPORTED,
         (in->BindFlags & D3D10_DDI_BIND_PRESENT) ? in->pPrimaryDesc : nullptr);
      return;
   }
   CreateResource(h, in, resource, rt);
}

static SIZE_T APIENTRY SrvSize(D3D10DDI_HDEVICE, const D3D11DDIARG_CREATESHADERRESOURCEVIEW *)
{
   return sizeof(View);
}

static void APIENTRY CreateSrv(D3D10DDI_HDEVICE h, const D3D11DDIARG_CREATESHADERRESOURCEVIEW *in,
                               D3D10DDI_HSHADERRESOURCEVIEW view, D3D10DDI_HRTSHADERRESOURCEVIEW rt)
{
   if (!in) { TG_ERROR(D(h), E_INVALIDARG); return; }
   if (in->ResourceDimension == D3D11DDIRESOURCE_BUFFEREX ||
       (in->ResourceDimension == D3D10DDIRESOURCE_BUFFER && R(in->hDrvResource) && R(in->hDrvResource)->structureStride)) {
      const bool extended = in->ResourceDimension == D3D11DDIRESOURCE_BUFFEREX;
      create_buffer_srv11(D(h), R(in->hDrvResource), view.pDrvPrivate,
         extended ? in->BufferEx.FirstElement : in->Buffer.FirstElement,
         extended ? in->BufferEx.NumElements : in->Buffer.NumElements, in->Format,
         extended ? in->BufferEx.Flags : 0);
      return;
   }
   D3D10DDIARG_CREATESHADERRESOURCEVIEW desc = {};
   desc.hDrvResource = in->hDrvResource;
   desc.Format = in->Format;
   desc.ResourceDimension = in->ResourceDimension;
   switch (in->ResourceDimension) {
   case D3D10DDIRESOURCE_BUFFER: desc.Buffer = in->Buffer; break;
   case D3D10DDIRESOURCE_TEXTURE1D: desc.Tex1D = in->Tex1D; break;
   case D3D10DDIRESOURCE_TEXTURE2D: desc.Tex2D = in->Tex2D; break;
   case D3D10DDIRESOURCE_TEXTURE3D: desc.Tex3D = in->Tex3D; break;
   case D3D10DDIRESOURCE_TEXTURECUBE:
      if (in->TexCube.First2DArrayFace || in->TexCube.NumCubes != 1) {
         TG_ERROR(D(h), DXGI_ERROR_UNSUPPORTED); return;
      }
      desc.TexCube.MostDetailedMip = in->TexCube.MostDetailedMip;
      desc.TexCube.MipLevels = in->TexCube.MipLevels;
      break;
   default: TG_ERROR(D(h), DXGI_ERROR_UNSUPPORTED); return;
   }
   legacy().pfnCreateShaderResourceView(h, &desc, view, rt);
}

static SIZE_T APIENTRY SrvSize20(D3D10DDI_HDEVICE, const D3DWDDM2_0DDIARG_CREATESHADERRESOURCEVIEW *)
{
   return sizeof(View);
}

static void APIENTRY CreateSrv20(D3D10DDI_HDEVICE h, const D3DWDDM2_0DDIARG_CREATESHADERRESOURCEVIEW *in,
                                 D3D10DDI_HSHADERRESOURCEVIEW view, D3D10DDI_HRTSHADERRESOURCEVIEW rt)
{
   if (!in) { TG_ERROR(D(h), E_INVALIDARG); return; }
   D3D11DDIARG_CREATESHADERRESOURCEVIEW desc = {};
   desc.hDrvResource = in->hDrvResource; desc.Format = in->Format;
   desc.ResourceDimension = in->ResourceDimension;
   switch (in->ResourceDimension) {
   case D3D11DDIRESOURCE_BUFFEREX: desc.BufferEx = in->BufferEx; break;
   case D3D10DDIRESOURCE_BUFFER: desc.Buffer = in->Buffer; break;
   case D3D10DDIRESOURCE_TEXTURE1D: desc.Tex1D = in->Tex1D; break;
   case D3D10DDIRESOURCE_TEXTURE2D:
      if (in->Tex2D.PlaneSlice) { TG_ERROR(D(h), DXGI_ERROR_UNSUPPORTED); return; }
      desc.Tex2D = {in->Tex2D.MostDetailedMip, in->Tex2D.FirstArraySlice,
                     in->Tex2D.MipLevels, in->Tex2D.ArraySize};
      break;
   case D3D10DDIRESOURCE_TEXTURE3D: desc.Tex3D = in->Tex3D; break;
   case D3D10DDIRESOURCE_TEXTURECUBE: desc.TexCube = in->TexCube; break;
   default: TG_ERROR(D(h), DXGI_ERROR_UNSUPPORTED); return;
   }
   CreateSrv(h, &desc, view, rt);
}

static SIZE_T APIENTRY RtvSize20(D3D10DDI_HDEVICE, const D3DWDDM2_0DDIARG_CREATERENDERTARGETVIEW *)
{
   return sizeof(View);
}

static void APIENTRY CreateRtv20(D3D10DDI_HDEVICE h, const D3DWDDM2_0DDIARG_CREATERENDERTARGETVIEW *in,
                                 D3D10DDI_HRENDERTARGETVIEW view, D3D10DDI_HRTRENDERTARGETVIEW rt)
{
   if (!in) { TG_ERROR(D(h), E_INVALIDARG); return; }
   D3D10DDIARG_CREATERENDERTARGETVIEW desc = {};
   desc.hDrvResource = in->hDrvResource; desc.Format = in->Format;
   desc.ResourceDimension = in->ResourceDimension;
   switch (in->ResourceDimension) {
   case D3D10DDIRESOURCE_BUFFER: desc.Buffer = in->Buffer; break;
   case D3D10DDIRESOURCE_TEXTURE1D: desc.Tex1D = in->Tex1D; break;
   case D3D10DDIRESOURCE_TEXTURE2D:
      if (in->Tex2D.PlaneSlice) { TG_ERROR(D(h), DXGI_ERROR_UNSUPPORTED); return; }
      desc.Tex2D = {in->Tex2D.MipSlice, in->Tex2D.FirstArraySlice, in->Tex2D.ArraySize};
      break;
   case D3D10DDIRESOURCE_TEXTURE3D: desc.Tex3D = in->Tex3D; break;
   case D3D10DDIRESOURCE_TEXTURECUBE: desc.TexCube = in->TexCube; break;
   default: TG_ERROR(D(h), DXGI_ERROR_UNSUPPORTED); return;
   }
   legacy().pfnCreateRenderTargetView(h, &desc, view, rt);
}

static SIZE_T APIENTRY DsvSize(D3D10DDI_HDEVICE, const D3D11DDIARG_CREATEDEPTHSTENCILVIEW *)
{
   return sizeof(View);
}

static void APIENTRY CreateDsv(D3D10DDI_HDEVICE h, const D3D11DDIARG_CREATEDEPTHSTENCILVIEW *in,
                               D3D10DDI_HDEPTHSTENCILVIEW view, D3D10DDI_HRTDEPTHSTENCILVIEW rt)
{
   if (!in) { TG_ERROR(D(h), E_INVALIDARG); return; }
   if (in->Flags & ~D3D11_DDI_CREATE_DSV_FLAG_MASK) { TG_ERROR(D(h), E_INVALIDARG); return; }
   D3D10DDIARG_CREATEDEPTHSTENCILVIEW desc = {};
   desc.hDrvResource = in->hDrvResource;
   desc.Format = in->Format;
   desc.ResourceDimension = in->ResourceDimension;
   switch (in->ResourceDimension) {
   case D3D10DDIRESOURCE_TEXTURE1D: desc.Tex1D = in->Tex1D; break;
   case D3D10DDIRESOURCE_TEXTURE2D: desc.Tex2D = in->Tex2D; break;
   case D3D10DDIRESOURCE_TEXTURECUBE: desc.TexCube = in->TexCube; break;
   default: TG_ERROR(D(h), DXGI_ERROR_UNSUPPORTED); return;
   }
   (void)rt;
   create_depth_stencil_view(D(h), &desc, view.pDrvPrivate, in->Flags);
}

static SIZE_T APIENTRY BlendSize(D3D10DDI_HDEVICE, const D3D11_1_DDI_BLEND_DESC *)
{
   return sizeof(BlendState);
}

static bool same_blend(const D3D11_1_DDI_RENDER_TARGET_BLEND_DESC &a,
                       const D3D11_1_DDI_RENDER_TARGET_BLEND_DESC &b)
{
   return a.SrcBlend == b.SrcBlend && a.DestBlend == b.DestBlend && a.BlendOp == b.BlendOp &&
          a.SrcBlendAlpha == b.SrcBlendAlpha && a.DestBlendAlpha == b.DestBlendAlpha &&
          a.BlendOpAlpha == b.BlendOpAlpha;
}

static void APIENTRY CreateBlend(D3D10DDI_HDEVICE h, const D3D11_1_DDI_BLEND_DESC *in,
                                 D3D10DDI_HBLENDSTATE handle, D3D10DDI_HRTBLENDSTATE rt)
{
   if (!in) { TG_ERROR(D(h), E_INVALIDARG); return; }
   D3D10_DDI_BLEND_DESC desc = {};
   desc.AlphaToCoverageEnable = in->AlphaToCoverageEnable;
   const auto &first = in->RenderTarget[0];
   desc.SrcBlend = first.SrcBlend; desc.DestBlend = first.DestBlend; desc.BlendOp = first.BlendOp;
   desc.SrcBlendAlpha = first.SrcBlendAlpha; desc.DestBlendAlpha = first.DestBlendAlpha;
   desc.BlendOpAlpha = first.BlendOpAlpha;
   static constexpr VkLogicOp ops[] = {
      VK_LOGIC_OP_CLEAR, VK_LOGIC_OP_SET, VK_LOGIC_OP_COPY, VK_LOGIC_OP_COPY_INVERTED,
      VK_LOGIC_OP_NO_OP, VK_LOGIC_OP_INVERT, VK_LOGIC_OP_AND, VK_LOGIC_OP_NAND,
      VK_LOGIC_OP_OR, VK_LOGIC_OP_NOR, VK_LOGIC_OP_XOR, VK_LOGIC_OP_EQUIVALENT,
      VK_LOGIC_OP_AND_REVERSE, VK_LOGIC_OP_AND_INVERTED, VK_LOGIC_OP_OR_REVERSE,
      VK_LOGIC_OP_OR_INVERTED,
   };
   static_assert(std::size(ops) == D3D11_1_DDI_LOGIC_OP_OR_INVERTED + 1);
   for (UINT i = 0; i < D3D10_DDI_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
      const auto &target = in->RenderTarget[in->IndependentBlendEnable ? i : 0];
      if (target.LogicOpEnable && !D(h)->caps->logicOp) {
         TG_ERROR(D(h), DXGI_ERROR_UNSUPPORTED); return;
      }
      if (target.LogicOpEnable && (in->IndependentBlendEnable || target.BlendEnable ||
                                   static_cast<UINT>(target.LogicOp) >= std::size(ops))) {
         TG_ERROR(D(h), E_INVALIDARG); return;
      }
      if (target.BlendEnable && !same_blend(first, target)) {
         TG_ERROR(D(h), DXGI_ERROR_UNSUPPORTED); return;
      }
      desc.BlendEnable[i] = target.BlendEnable;
      desc.RenderTargetWriteMask[i] = target.RenderTargetWriteMask;
   }
   legacy().pfnCreateBlendState(h, &desc, handle, rt);
   if (first.LogicOpEnable) {
      auto *state = static_cast<BlendState *>(handle.pDrvPrivate);
      state->logicOpEnabled = true;
      state->logicOp = ops[first.LogicOp];
   }
}

static SIZE_T APIENTRY RasterizerSize(D3D10DDI_HDEVICE, const D3D11_1_DDI_RASTERIZER_DESC *)
{
   return sizeof(RasterizerState);
}

static void APIENTRY CreateRasterizer(D3D10DDI_HDEVICE h, const D3D11_1_DDI_RASTERIZER_DESC *in,
                                      D3D10DDI_HRASTERIZERSTATE handle, D3D10DDI_HRTRASTERIZERSTATE rt)
{
   if (!in || !handle.pDrvPrivate) { TG_ERROR(D(h), E_INVALIDARG); return; }
   const UINT samples = std::max(1u, in->ForcedSampleCount);
   if (samples != 1 && samples != 4 && samples != 8 && samples != 16) {
      TG_ERROR(D(h), E_INVALIDARG); return;
   }
   if (samples > 1 && (!D(h)->caps || !((D(h)->caps->attachmentlessSamples | D(h)->caps->tirSamples) & samples))) {
      TG_ERROR(D(h), DXGI_ERROR_UNSUPPORTED); return;
   }
   D3D10_DDI_RASTERIZER_DESC desc = {in->FillMode, in->CullMode, in->FrontCounterClockwise,
      in->DepthBias, in->DepthBiasClamp, in->SlopeScaledDepthBias, in->DepthClipEnable,
      in->ScissorEnable, in->MultisampleEnable, in->AntialiasedLineEnable};
   legacy().pfnCreateRasterizerState(h, &desc, handle, rt);
   static_cast<RasterizerState *>(handle.pDrvPrivate)->samples = VkSampleCountFlagBits(samples);
}

static SIZE_T APIENTRY RasterizerSize20(D3D10DDI_HDEVICE, const D3DWDDM2_0DDI_RASTERIZER_DESC *)
{
   return sizeof(RasterizerState);
}

static void APIENTRY CreateRasterizer20(D3D10DDI_HDEVICE h, const D3DWDDM2_0DDI_RASTERIZER_DESC *in,
                                        D3D10DDI_HRASTERIZERSTATE handle, D3D10DDI_HRTRASTERIZERSTATE rt)
{
   if (!in) { TG_ERROR(D(h), E_INVALIDARG); return; }
   if (in->ConservativeRasterizationMode != D3DWDDM2_0DDI_CONSERVATIVE_RASTERIZATION_OFF) {
      TG_ERROR(D(h), DXGI_ERROR_UNSUPPORTED); return;
   }
   D3D11_1_DDI_RASTERIZER_DESC desc = {in->FillMode, in->CullMode, in->FrontCounterClockwise,
      in->DepthBias, in->DepthBiasClamp, in->SlopeScaledDepthBias, in->DepthClipEnable,
      in->ScissorEnable, in->MultisampleEnable, in->AntialiasedLineEnable, in->ForcedSampleCount};
   CreateRasterizer(h, &desc, handle, rt);
}

static SIZE_T APIENTRY QuerySize20(D3D10DDI_HDEVICE, const D3DWDDM2_0DDIARG_CREATEQUERY *)
{
   return sizeof(Query);
}

static void APIENTRY CreateQuery20(D3D10DDI_HDEVICE h, const D3DWDDM2_0DDIARG_CREATEQUERY *in,
                                   D3D10DDI_HQUERY query, D3D10DDI_HRTQUERY rt)
{
   if (!in) { TG_ERROR(D(h), E_INVALIDARG); return; }
   if (in->ContextType != D3DWDDM2_0DDI_CONTEXTTYPE_ALL &&
       in->ContextType != D3DWDDM2_0DDI_CONTEXTTYPE_3D) {
      TG_ERROR(D(h), DXGI_ERROR_UNSUPPORTED); return;
   }
   D3D10DDIARG_CREATEQUERY desc = {in->Query, in->MiscFlags};
   legacy().pfnCreateQuery(h, &desc, query, rt);
}

static SIZE_T APIENTRY ShaderSize(D3D10DDI_HDEVICE, const UINT *, const D3D11_1DDIARG_STAGE_IO_SIGNATURES *)
{
   return sizeof(Shader);
}

template<Stage stage>
static void APIENTRY CreateGraphicsShader(D3D10DDI_HDEVICE h, const UINT *code, D3D10DDI_HSHADER handle,
                                          D3D10DDI_HRTSHADER, const D3D11_1DDIARG_STAGE_IO_SIGNATURES *io)
{
   create_shader11(h, code, handle, stage, io);
}

static SIZE_T APIENTRY TessShaderSize(D3D10DDI_HDEVICE, const UINT *, const D3D11_1DDIARG_TESSELLATION_IO_SIGNATURES *)
{
   return sizeof(Shader);
}

template<Stage stage>
static void APIENTRY CreateTessShader(D3D10DDI_HDEVICE h, const UINT *code, D3D10DDI_HSHADER handle,
                                      D3D10DDI_HRTSHADER, const D3D11_1DDIARG_TESSELLATION_IO_SIGNATURES *sig)
{
   if (!sig) { TG_ERROR(D(h), E_INVALIDARG); return; }
   D3D11_1DDIARG_STAGE_IO_SIGNATURES io{};
   io.pInputSignature = sig->pInputSignature; io.NumInputSignatureEntries = sig->NumInputSignatureEntries;
   io.pOutputSignature = sig->pOutputSignature; io.NumOutputSignatureEntries = sig->NumOutputSignatureEntries;
   create_shader11(h, code, handle, stage, &io, sig->pPatchConstantSignature, sig->NumPatchConstantSignatureEntries);
}

template<Stage stage>
static void APIENTRY SetGraphicsShader(D3D10DDI_HDEVICE h, D3D10DDI_HSHADER shader)
{ set_graphics_shader(h, shader, stage); }

template<Stage stage>
static void APIENTRY SetGraphicsResources(D3D10DDI_HDEVICE h, UINT start, UINT count, const D3D10DDI_HSHADERRESOURCEVIEW *views)
{ set_shader_resources(D(h), stage, start, count, views); }

template<Stage stage>
static void APIENTRY SetGraphicsSamplers(D3D10DDI_HDEVICE h, UINT start, UINT count, const D3D10DDI_HSAMPLER *samplers)
{ set_samplers(D(h), stage, start, count, samplers); }

static SIZE_T APIENTRY GeometrySoSize(D3D10DDI_HDEVICE,
   const D3D11DDIARG_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT *, const D3D11_1DDIARG_STAGE_IO_SIGNATURES *)
{
   return sizeof(Shader);
}

static void APIENTRY CreateGeometrySo(D3D10DDI_HDEVICE h,
   const D3D11DDIARG_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT *in, D3D10DDI_HSHADER handle,
   D3D10DDI_HRTSHADER, const D3D11_1DDIARG_STAGE_IO_SIGNATURES *sig)
{
   if (!in) { TG_ERROR(D(h), E_INVALIDARG); return; }
   create_shader11(h, in->pShaderCode, handle, STAGE_GS, sig, nullptr, 0, in);
}

#define STAGE_ENTRY(name, stage) \
static void APIENTRY name##Constants(D3D10DDI_HDEVICE h, UINT firstSlot, UINT count, \
   const D3D10DDI_HRESOURCE *buffers, const UINT *first, const UINT *sizes) \
{ set_constant_buffers(D(h), stage, firstSlot, count, buffers, first, sizes); } \
static void APIENTRY name##ShaderInterfaces(D3D10DDI_HDEVICE h, D3D10DDI_HSHADER shader, \
   UINT count, const UINT *, const D3D11DDIARG_POINTERDATA *) \
{ \
   if (count) { TG_ERROR(D(h), DXGI_ERROR_UNSUPPORTED); return; } \
   set_graphics_shader(h, shader, stage); \
}
STAGE_ENTRY(Vs, STAGE_VS)
STAGE_ENTRY(Gs, STAGE_GS)
STAGE_ENTRY(Ps, STAGE_PS)
STAGE_ENTRY(Hs, STAGE_HS)
STAGE_ENTRY(Ds, STAGE_DS)
#undef STAGE_ENTRY

static void APIENTRY SetRenderTargets(D3D10DDI_HDEVICE h, const D3D10DDI_HRENDERTARGETVIEW *rtvs,
   UINT count, UINT clear, D3D10DDI_HDEPTHSTENCILVIEW dsv,
   const D3D11DDI_HUNORDEREDACCESSVIEW *views, const UINT *initialCounts, UINT start, UINT uavs, UINT, UINT)
{
   Device *dev = D(h);
   if (uavs && (!dev->caps || !dev->caps->graphicsUavSlots)) {
      TG_ERROR(dev, DXGI_ERROR_UNSUPPORTED); return;
   }
   if (count > D3D10_DDI_SIMULTANEOUS_RENDER_TARGET_COUNT ||
       clear > D3D10_DDI_SIMULTANEOUS_RENDER_TARGET_COUNT - count || (count && !rtvs) ||
       (uavs && (start < count || start > dev->caps->graphicsUavSlots || uavs > dev->caps->graphicsUavSlots - start))) {
      TG_ERROR(dev, E_INVALIDARG); return;
   }
   for (UINT i = 0; i < count; ++i) {
      const auto *v = static_cast<const View *>(rtvs[i].pDrvPrivate);
      if (v && (v->dev != dev || v->type != ViewType::RTV)) { TG_ERROR(dev, E_INVALIDARG); return; }
   }
   const auto *depth = static_cast<const View *>(dsv.pDrvPrivate);
   if (depth && (depth->dev != dev || depth->type != ViewType::DSV)) { TG_ERROR(dev, E_INVALIDARG); return; }
   // The last two DDI parameters are optimization hints. The provided views
   // include explicit null bindings and counters can be reset on a bound view.
   if (uavs && !set_uavs(dev, dev->graphicsUav, start, uavs, views, initialCounts)) return;
   for (UINT i = 0; i < count; ++i) {
      if (dev->graphicsUav[i]) {
         dev->graphicsUav[i] = nullptr;
         dev->dirty |= DIRTY_GRAPHICS_UAVS;
      }
   }
   legacy().pfnSetRenderTargets(h, rtvs, count, clear, dsv);
}

static bool copy_flags(Device *dev, UINT flags)
{
   /* TILEABLE permits a tiled renderer to limit its flush. The regular
    * synchronized region copy/update already satisfies this hint. */
   constexpr UINT basic = D3D11_1DDI_COPY_NO_OVERWRITE | D3D11_1DDI_COPY_DISCARD;
   if (!(flags & ~basic)) return true;
   const bool valid = !(flags & ~(basic | D3D11_1DDI_COPY_TILEABLE));
   trace(TGD3D_TRACE_COPY_FLAGS, valid ? S_OK : E_INVALIDARG, flags, 0);
   if (!valid) {
      TG_ERROR(dev, E_INVALIDARG); return false;
   }
   /* Preserve the existing synchronized copy. Discard permits forgetting
    * earlier contents; it does not require freeing or overwriting them early. */
   return true;
}

static void APIENTRY CopyRegion(D3D10DDI_HDEVICE h, D3D10DDI_HRESOURCE dst, UINT dstSub,
   UINT x, UINT y, UINT z, D3D10DDI_HRESOURCE src, UINT srcSub, const D3D10_DDI_BOX *box, UINT flags)
{
   Device *dev = D(h);
   RemovalOnlyErrorScope errors(dev);
   if (copy_flags(dev, flags)) legacy().pfnResourceCopyRegion(h, dst, dstSub, x, y, z, src, srcSub, box);
}

static void APIENTRY UpdateResource(D3D10DDI_HDEVICE h, D3D10DDI_HRESOURCE dst, UINT sub,
   const D3D10_DDI_BOX *box, const void *data, UINT rowPitch, UINT depthPitch, UINT flags)
{
   Device *dev = D(h);
   RemovalOnlyErrorScope errors(dev);
   if (copy_flags(dev, flags)) legacy().pfnResourceUpdateSubresourceUP(h, dst, sub, box, data, rowPitch, depthPitch);
}

static BOOL APIENTRY Flush(D3D10DDI_HDEVICE h, UINT flags)
{
   Device *dev = D(h);
   RemovalOnlyErrorScope errors(dev);
   /* New flags may only introduce reasons not to flush. Unknown flags must
    * conservatively submit, rather than rejecting a valid future request. */
   legacy().pfnFlush(h);
   if (SUCCEEDED(dev->lost) && (flags & D3DWDDM1_3DDI_TRIM_MEMORY)) {
      HRESULT hr = trim_memory(dev);
      if (FAILED(hr)) TG_ERROR(dev, hr);
   }
   return SUCCEEDED(dev->lost);
}

static BOOL APIENTRY Flush(D3D10DDI_HDEVICE h, UINT contextType, UINT flags)
{
   /* Copies and draws share the real graphics queue. No compute/video queue
    * is exposed by this immediate D3D10 pipeline. */
   if (contextType & ~(D3DWDDM2_0DDI_CONTEXTTYPE_3D | D3DWDDM2_0DDI_CONTEXTTYPE_COPY)) {
      TG_ERROR(D(h), E_INVALIDARG); return FALSE;
   }
   return Flush(h, flags);
}

static void APIENTRY Discard(D3D10DDI_HDEVICE h, D3D11DDI_HANDLETYPE type,
                             void *handle, const D3D10_DDI_RECT *rects, UINT count)
{
   Device *dev = D(h);
   if (!dev || FAILED(dev->lost)) return;
   RemovalOnlyErrorScope errors(dev);
   Resource *res = nullptr;
   if (type == D3D10DDI_HT_RESOURCE) {
      res = static_cast<Resource *>(handle);
   } else if (type == D3D10DDI_HT_SHADERRESOURCEVIEW || type == D3D10DDI_HT_RENDERTARGETVIEW ||
              type == D3D10DDI_HT_DEPTHSTENCILVIEW) {
      View *view = static_cast<View *>(handle);
      const ViewType expected = type == D3D10DDI_HT_SHADERRESOURCEVIEW ? ViewType::SRV :
         type == D3D10DDI_HT_RENDERTARGETVIEW ? ViewType::RTV : ViewType::DSV;
      if (!view || view->dev != dev || view->type != expected) {
         TG_ERROR(dev, E_INVALIDARG); return;
      }
      res = view->res;
   } else { TG_ERROR(dev, DXGI_ERROR_UNSUPPORTED); return; }
   if (!res || res->dev != dev || runtime_is_offered(resource_runtime(res))) {
      TG_ERROR(dev, E_INVALIDARG); return;
   }
   for (UINT i = 0; rects && i < count; ++i) {
      if (rects[i].left > rects[i].right || rects[i].top > rects[i].bottom) {
         TG_ERROR(dev, E_INVALIDARG); return;
      }
   }
   /* Discard is permission to forget contents, not eviction/Offer and not a
    * request to unbind or release the object. Retaining its storage is legal
    * for this immediate renderer. In particular do not change layoutReady,
    * pending clears, lastUse or completion while earlier GPU work uses it.
    * Whole/partial view contents may remain; partial discards must preserve
    * texels outside their range. No CPU fill or synthetic completion. */
}

static void APIENTRY Relocate(D3D10DDI_HDEVICE, D3D11_1DDI_DEVICEFUNCS *) {}
static void APIENTRY Relocate(D3D10DDI_HDEVICE, D3DWDDM1_3DDI_DEVICEFUNCS *) {}
static void APIENTRY Relocate(D3D10DDI_HDEVICE, D3DWDDM2_0DDI_DEVICEFUNCS *) {}

static void APIENTRY Multisample13(D3D10DDI_HDEVICE h, DXGI_FORMAT format,
                                   UINT samples, UINT flags, UINT *levels)
{
   if (!levels) return;
   *levels = 0;
   /* Tiled resources are not supported. A nonzero flag cannot be silently
    * dropped while forwarding the older callback with a shorter signature. */
   if (!flags) legacy().pfnCheckMultisampleQualityLevels(h, format, samples, levels);
}

static void APIENTRY CheckDirectFlip(D3D10DDI_HDEVICE h, D3D10DDI_HRESOURCE source,
   D3D10DDI_HRESOURCE target, UINT flags, BOOL *supported)
{
   if (supported)
      *supported = !flags && direct_flip_compatible(D(h), R(source), R(target));
}

template<typename Descriptor>
static SIZE_T APIENTRY UavSize(D3D10DDI_HDEVICE, const Descriptor *) { return sizeof(View); }

static void APIENTRY CreateUav20(D3D10DDI_HDEVICE h, const D3DWDDM2_0DDIARG_CREATEUNORDEREDACCESSVIEW *in,
                                 D3D11DDI_HUNORDEREDACCESSVIEW view, D3D11DDI_HRTUNORDEREDACCESSVIEW rt)
{
   if (!in) { TG_ERROR(D(h), E_INVALIDARG); return; }
   D3D11DDIARG_CREATEUNORDEREDACCESSVIEW desc{};
   desc.hDrvResource = in->hDrvResource; desc.Format = in->Format; desc.ResourceDimension = in->ResourceDimension;
   switch (in->ResourceDimension) {
   case D3D10DDIRESOURCE_BUFFER: desc.Buffer = in->Buffer; break;
   case D3D10DDIRESOURCE_TEXTURE1D: desc.Tex1D = in->Tex1D; break;
   case D3D10DDIRESOURCE_TEXTURE2D:
      if (in->Tex2D.PlaneSlice) { TG_ERROR(D(h), DXGI_ERROR_UNSUPPORTED); return; }
      desc.Tex2D = {in->Tex2D.MipSlice, in->Tex2D.FirstArraySlice, in->Tex2D.ArraySize}; break;
   case D3D10DDIRESOURCE_TEXTURE3D: desc.Tex3D = in->Tex3D; break;
   default: TG_ERROR(D(h), E_INVALIDARG); return;
   }
   CreateUav11(h, &desc, view, rt);
}

static void APIENTRY CsConstants(D3D10DDI_HDEVICE h, UINT start, UINT count, const D3D10DDI_HRESOURCE *buffers,
                                 const UINT *first, const UINT *sizes)
{ set_constant_buffers(D(h), STAGE_CS, start, count, buffers, first, sizes); }

static void APIENTRY CsInterfaces(D3D10DDI_HDEVICE h, D3D10DDI_HSHADER shader, UINT count,
                                  const UINT *, const D3D11DDIARG_POINTERDATA *)
{
   if (count) { TG_ERROR(D(h), DXGI_ERROR_UNSUPPORTED); return; }
   CsSetShader(h, shader);
}

template<typename Table>
static void common_table(Table *table)
{
   *table = {};
   const auto &old = legacy();
   /* Each assignment is checked against the actual WDK function type. */
#define SAME(name) table->name = old.name
   // Identical signatures are assigned below; changed ones use typed wrappers.
   SAME(pfnPsSetShaderResources);
   SAME(pfnPsSetShader);
   SAME(pfnPsSetSamplers);
   SAME(pfnVsSetShader);
   SAME(pfnDrawIndexed);
   SAME(pfnDraw);
   SAME(pfnDynamicIABufferMapNoOverwrite);
   SAME(pfnDynamicIABufferUnmap);
   SAME(pfnDynamicConstantBufferMapDiscard);
   SAME(pfnDynamicIABufferMapDiscard);
   SAME(pfnDynamicConstantBufferUnmap);
   SAME(pfnIaSetInputLayout);
   SAME(pfnIaSetVertexBuffers);
   SAME(pfnIaSetIndexBuffer);
   SAME(pfnDrawIndexedInstanced);
   SAME(pfnDrawInstanced);
   SAME(pfnDynamicResourceMapDiscard);
   SAME(pfnDynamicResourceUnmap);
   SAME(pfnGsSetShader);
   SAME(pfnIaSetTopology);
   SAME(pfnStagingResourceMap);
   SAME(pfnStagingResourceUnmap);
   SAME(pfnVsSetShaderResources);
   SAME(pfnVsSetSamplers);
   SAME(pfnGsSetShaderResources);
   SAME(pfnGsSetSamplers);
   SAME(pfnShaderResourceViewReadAfterWriteHazard);
   SAME(pfnResourceReadAfterWriteHazard);
   SAME(pfnSetBlendState);
   SAME(pfnSetDepthStencilState);
   SAME(pfnSetRasterizerState);
   SAME(pfnQueryEnd);
   SAME(pfnQueryBegin);
   SAME(pfnSoSetTargets);
   SAME(pfnDrawAuto);
   SAME(pfnSetViewports);
   SAME(pfnSetScissorRects);
   SAME(pfnClearRenderTargetView);
   SAME(pfnClearDepthStencilView);
   SAME(pfnSetPredication);
   SAME(pfnQueryGetData);
   SAME(pfnGenMips);
   SAME(pfnResourceCopy);
   SAME(pfnResourceResolveSubresource);
   SAME(pfnResourceMap);
   SAME(pfnResourceUnmap);
   SAME(pfnResourceIsStagingBusy);
   SAME(pfnCalcPrivateOpenedResourceSize);
   SAME(pfnOpenResource);
   SAME(pfnDestroyResource);
   SAME(pfnDestroyShaderResourceView);
   SAME(pfnDestroyRenderTargetView);
   SAME(pfnDestroyDepthStencilView);
   SAME(pfnCalcPrivateElementLayoutSize);
   SAME(pfnCreateElementLayout);
   SAME(pfnDestroyElementLayout);
   SAME(pfnDestroyBlendState);
   SAME(pfnCalcPrivateDepthStencilStateSize);
   SAME(pfnCreateDepthStencilState);
   SAME(pfnDestroyDepthStencilState);
   SAME(pfnDestroyRasterizerState);
   SAME(pfnDestroyShader);
   SAME(pfnCalcPrivateSamplerSize);
   SAME(pfnCreateSampler);
   SAME(pfnDestroySampler);
   SAME(pfnDestroyQuery);
   SAME(pfnCheckFormatSupport);
   SAME(pfnCheckCounterInfo);
   SAME(pfnCheckCounter);
   SAME(pfnDestroyDevice);
   SAME(pfnSetTextFilterSize);
#undef SAME
   table->pfnCalcPrivateResourceSize = ResourceSize;
   table->pfnCalcPrivateDepthStencilViewSize = DsvSize;
   table->pfnCreateDepthStencilView = CreateDsv;
   table->pfnCalcPrivateBlendStateSize = BlendSize;
   table->pfnCreateBlendState = CreateBlend;
   table->pfnCalcPrivateShaderSize = ShaderSize;
   table->pfnCreateVertexShader = CreateGraphicsShader<STAGE_VS>;
   table->pfnCreateGeometryShader = CreateGraphicsShader<STAGE_GS>;
   table->pfnCreatePixelShader = CreateGraphicsShader<STAGE_PS>;
   table->pfnCalcPrivateGeometryShaderWithStreamOutput = GeometrySoSize;
   table->pfnCreateGeometryShaderWithStreamOutput = CreateGeometrySo;
   table->pfnVsSetConstantBuffers = VsConstants;
   table->pfnGsSetConstantBuffers = GsConstants;
   table->pfnPsSetConstantBuffers = PsConstants;
   table->pfnVsSetShaderWithIfaces = VsShaderInterfaces;
   table->pfnGsSetShaderWithIfaces = GsShaderInterfaces;
   table->pfnPsSetShaderWithIfaces = PsShaderInterfaces;
   table->pfnSetRenderTargets = SetRenderTargets;
   table->pfnCalcPrivateTessellationShaderSize = TessShaderSize;
   table->pfnCreateHullShader = CreateTessShader<STAGE_HS>;
   table->pfnCreateDomainShader = CreateTessShader<STAGE_DS>;
   table->pfnHsSetShader = SetGraphicsShader<STAGE_HS>;
   table->pfnDsSetShader = SetGraphicsShader<STAGE_DS>;
   table->pfnHsSetConstantBuffers = HsConstants;
   table->pfnDsSetConstantBuffers = DsConstants;
   table->pfnHsSetShaderResources = SetGraphicsResources<STAGE_HS>;
   table->pfnDsSetShaderResources = SetGraphicsResources<STAGE_DS>;
   table->pfnHsSetSamplers = SetGraphicsSamplers<STAGE_HS>;
   table->pfnDsSetSamplers = SetGraphicsSamplers<STAGE_DS>;
   table->pfnHsSetShaderWithIfaces = HsShaderInterfaces;
   table->pfnDsSetShaderWithIfaces = DsShaderInterfaces;
   table->pfnCreateComputeShader = CreateComputeShader;
   table->pfnCsSetShader = CsSetShader;
   table->pfnCsSetShaderWithIfaces = CsInterfaces;
   table->pfnCsSetSamplers = CsSetSamplers;
   table->pfnCsSetShaderResources = CsSetShaderResources;
   table->pfnCsSetConstantBuffers = CsConstants;
   table->pfnCalcPrivateUnorderedAccessViewSize = UavSize;
   table->pfnDestroyUnorderedAccessView = DestroyUav11;
   table->pfnCsSetUnorderedAccessViews = CsSetUavs;
   table->pfnClearUnorderedAccessViewUint = ClearUavUint11;
   table->pfnCopyStructureCount = CopyStructureCount11;
   table->pfnDispatch = Dispatch11;
   table->pfnDispatchIndirect = DispatchIndirect11;
   table->pfnResourceCopyRegion = CopyRegion;
   table->pfnResourceUpdateSubresourceUP = UpdateResource;
   table->pfnDefaultConstantBufferUpdateSubresourceUP = UpdateResource;
   table->pfnFlush = Flush;
   table->pfnRelocateDeviceFuncs = Relocate;
   table->pfnCheckDirectFlipSupport = CheckDirectFlip;
   table->pfnClearView = ClearView11;
   table->pfnDiscard = Discard;
   table->pfnDynamicConstantBufferMapNoOverwrite = old.pfnResourceMap;
   table->pfnDrawInstancedIndirect = DrawIndirect11;
   table->pfnDrawIndexedInstancedIndirect = DrawIndexedIndirect11;
   if constexpr (std::is_same_v<Table, D3DWDDM2_0DDI_DEVICEFUNCS>) {
      table->pfnCreateUnorderedAccessView = CreateUav20;
      table->pfnCreateResource = CreateResource20;
      table->pfnCalcPrivateShaderResourceViewSize = SrvSize20;
      table->pfnCreateShaderResourceView = CreateSrv20;
      table->pfnCalcPrivateRenderTargetViewSize = RtvSize20;
      table->pfnCreateRenderTargetView = CreateRtv20;
      table->pfnCalcPrivateRasterizerStateSize = RasterizerSize20;
      table->pfnCreateRasterizerState = CreateRasterizer20;
      table->pfnCalcPrivateQuerySize = QuerySize20;
      table->pfnCreateQuery = CreateQuery20;
   } else {
      table->pfnCreateUnorderedAccessView = CreateUav11;
      table->pfnCreateResource = CreateResource;
      table->pfnCalcPrivateShaderResourceViewSize = SrvSize;
      table->pfnCreateShaderResourceView = CreateSrv;
      table->pfnCalcPrivateRenderTargetViewSize = old.pfnCalcPrivateRenderTargetViewSize;
      table->pfnCreateRenderTargetView = old.pfnCreateRenderTargetView;
      table->pfnCalcPrivateRasterizerStateSize = RasterizerSize;
      table->pfnCreateRasterizerState = CreateRasterizer;
      table->pfnCalcPrivateQuerySize = old.pfnCalcPrivateQuerySize;
      table->pfnCreateQuery = old.pfnCreateQuery;
   }
}

void device_fill_table11(D3D11_1DDI_DEVICEFUNCS *table)
{
   common_table(table);
   table->pfnCheckMultisampleQualityLevels = legacy().pfnCheckMultisampleQualityLevels;
}

void device_fill_table13(D3DWDDM1_3DDI_DEVICEFUNCS *table)
{
   /* DXGI1.3's MPO attributes include ClipRect/DirtyRects, unlike the Win8
    * structure. Pair that ABI with its real device table, never a cast of
    * the shorter 11.1 table. Optional tiled/marker entries remain null. */
   common_table(table);
   table->pfnCheckMultisampleQualityLevels = Multisample13;
}

void device_fill_table20(D3DWDDM2_0DDI_DEVICEFUNCS *table)
{
   common_table(table);
   table->pfnCheckMultisampleQualityLevels = Multisample13;
   /* Paired with DXGI1.4. Do not copy a 1.3 table:
    * Flush, query, view and rasterizer descriptors have changed signatures.
    * Optional hardware protection/default texture layouts remain unavailable. */
}

} // namespace tgd3d
