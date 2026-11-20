/* SPDX-License-Identifier: MIT
 * Fixed-function D3D9 color drawing from a transformed stream zero.
 */
#include "tgd3d_legacy9.h"
#include "shaders/legacy9_color.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <new>

namespace tgd3d::legacy9 {

struct Declaration {
   LegacyDevice *owner = nullptr;
   Declaration *next = nullptr;
   UINT position = UINT_MAX;
   UINT diffuse = UINT_MAX;
   UINT texcoord = UINT_MAX, coordFloats = 0;
   UINT extent = 0;
};

struct ColorVertex {
   float x, y, z, w;
   uint32_t rgba;
   float uv[2];
};

struct TextureStage {
   UINT color[4]{D3DTOP_MODULATE, D3DTA_TEXTURE, D3DTA_CURRENT, 0};
   UINT alpha[4]{D3DTOP_SELECTARG1, D3DTA_TEXTURE, D3DTA_CURRENT, 0};
};
static_assert(sizeof(TextureStage) == 32);

struct DrawState {
   Declaration *declarations = nullptr;
   Declaration *decl = nullptr;
   const BYTE *stream = nullptr;
   UINT stride = 0, streamOffset = 0;
   const BYTE *userIndices = nullptr;
   UINT indexSize = 0;
   D3DDDIARG_VIEWPORTINFO viewport{};
   D3DDDIARG_ZRANGE depthRange{0, 1};
   RECT scissor{};
   RasterizerState raster{};
   DepthStencilState depth{};
   ElementLayout layout{}, textureLayout{};
   TextureStage stage{};
   UINT addressU = D3DTADDRESS_WRAP, addressV = D3DTADDRESS_WRAP;
   UINT minFilter = D3DTEXF_POINT, magFilter = D3DTEXF_POINT, mipFilter = D3DTEXF_NONE;
   UINT maxMip = 0;
   Sampler sampler{};
   bool samplerReady = false, samplerDirty = true, constantsReady = false, constantsDirty = true;
   alignas(Resource) BYTE constants[sizeof(Resource)];
   alignas(Shader) BYTE textureVs[sizeof(Shader)], texturePs[sizeof(Shader)];
   bool textureVsReady = false, texturePsReady = false;
   alignas(Shader) BYTE vs[sizeof(Shader)], ps[sizeof(Shader)];
   bool vsReady = false, psReady = false;
   alignas(Resource) BYTE vertices[sizeof(Resource)];
   UINT capacity = 0;
};

void draw_target_changed(LegacyDevice *dev)
{
   if (dev->draw && dev->target) {
      dev->draw->viewport = {0, 0, dev->target->width, dev->target->height};
      dev->draw->scissor = dev->target->bounds();
      dev->draw->depthRange = {0, 1};
   }
}

RECT clear_bounds(const LegacyDevice *dev)
{
   const DrawState *s = dev->draw;
   if (!s)
      return dev->target->bounds();
   const auto &v = s->viewport;
   RECT bounds{LONG(v.X), LONG(v.Y), LONG(v.X + v.Width), LONG(v.Y + v.Height)};
   if (s->raster.desc.ScissorEnable) {
      RECT clipped{};
      IntersectRect(&clipped, &bounds, &s->scissor);
      return clipped;
   }
   return bounds;
}

static DrawState *draw_state(LegacyDevice *dev)
{
   if (dev->draw)
      return dev->draw;
   dev->draw = new (std::nothrow) DrawState{};
   if (!dev->draw)
      return nullptr;
   auto &s = *dev->draw;
   s.raster.desc.FillMode = D3D10_DDI_FILL_SOLID;
   s.raster.desc.CullMode = D3D10_DDI_CULL_BACK; // D3D9 defaults to culling counter-clockwise triangles
   s.raster.desc.DepthClipEnable = TRUE;
   s.depth.desc.DepthFunc = D3D10_DDI_COMPARISON_ALWAYS;
   s.layout.count = 2;
   s.layout.registerMask = 3;
   s.layout.elements[0] = {0, 0, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D10_DDI_INPUT_PER_VERTEX_DATA, 0, 0};
   s.layout.elements[1] = {0, offsetof(ColorVertex, rgba), DXGI_FORMAT_R8G8B8A8_UNORM,
                           D3D10_DDI_INPUT_PER_VERTEX_DATA, 0, 1};
   s.textureLayout = s.layout;
   s.textureLayout.count = 3;
   s.textureLayout.registerMask = 7;
   s.textureLayout.elements[2] = {0, offsetof(ColorVertex, uv), DXGI_FORMAT_R32G32_FLOAT,
                                  D3D10_DDI_INPUT_PER_VERTEX_DATA, 0, 2};
   draw_target_changed(dev);
   return &s;
}

static HRESULT APIENTRY create_decl(HANDLE handle, D3DDDIARG_CREATEVERTEXSHADERDECL *args,
                                    const D3DDDIVERTEXELEMENT *elems)
{
   if (!handle || !args || !elems || !args->NumVertexElements || args->NumVertexElements > MAXD3DDECLLENGTH + 1)
      return E_INVALIDARG;
   auto *dev = static_cast<LegacyDevice *>(handle);
   Declaration input{};
   for (UINT i = 0; i < args->NumVertexElements; ++i) {
      const auto &e = elems[i];
      if (e.Stream == 0xff && e.Type == D3DDECLTYPE_UNUSED && i + 1 == args->NumVertexElements)
         break;
      if (e.Stream || e.Method != D3DDECLMETHOD_DEFAULT || e.UsageIndex)
         return D3DERR_NOTAVAILABLE;
      if (e.Usage == D3DDECLUSAGE_POSITIONT && e.Type == D3DDECLTYPE_FLOAT4 && input.position == UINT_MAX) {
         input.position = e.Offset;
         input.extent = std::max(input.extent, UINT(e.Offset) + 16);
      } else if (e.Usage == D3DDECLUSAGE_COLOR && e.Type == D3DDECLTYPE_D3DCOLOR && input.diffuse == UINT_MAX) {
         input.diffuse = e.Offset;
         input.extent = std::max(input.extent, UINT(e.Offset) + 4);
      } else if (e.Usage == D3DDECLUSAGE_TEXCOORD && e.Type <= D3DDECLTYPE_FLOAT4 && input.texcoord == UINT_MAX) {
         input.texcoord = e.Offset;
         input.coordFloats = 1 + e.Type - D3DDECLTYPE_FLOAT1;
         input.extent = std::max(input.extent, UINT(e.Offset) + 4 * input.coordFloats);
      } else {
         return D3DERR_NOTAVAILABLE;
      }
   }
   if (input.position == UINT_MAX)
      return D3DERR_NOTAVAILABLE;
   auto *s = draw_state(dev);
   if (!s)
      return E_OUTOFMEMORY;
   input.owner = dev;
   input.next = s->declarations;
   auto *decl = new (std::nothrow) Declaration(input);
   if (!decl)
      return E_OUTOFMEMORY;
   s->declarations = decl;
   args->ShaderHandle = decl;
   return S_OK;
}

static HRESULT APIENTRY bind_decl(HANDLE handle, HANDLE object)
{
   auto *dev = static_cast<LegacyDevice *>(handle);
   auto *decl = static_cast<Declaration *>(object);
   if (!dev || (decl && decl->owner != dev))
      return E_INVALIDARG;
   if (dev->draw)
      dev->draw->decl = decl;
   return S_OK;
}

static HRESULT APIENTRY delete_decl(HANDLE handle, HANDLE object)
{
   auto *dev = static_cast<LegacyDevice *>(handle);
   auto *decl = static_cast<Declaration *>(object);
   if (!dev || !decl || decl->owner != dev)
      return E_INVALIDARG;
   auto &s = *dev->draw;
   if (s.decl == decl)
      s.decl = nullptr;
   Declaration **prev = &s.declarations;
   while (*prev != decl)
      prev = &(*prev)->next;
   *prev = decl->next;
   delete decl;
   return S_OK;
}

static HRESULT APIENTRY set_stream(HANDLE handle, const D3DDDIARG_SETSTREAMSOURCEUM *args, const void *data)
{
   if (!handle || !args)
      return E_INVALIDARG;
   if (args->Stream || args->Stride > max_stream_stride)
      return D3DERR_NOTAVAILABLE;
   auto *s = draw_state(static_cast<LegacyDevice *>(handle));
   if (!s)
      return E_OUTOFMEMORY;
   static_cast<LegacyDevice *>(handle)->vertexStream = nullptr;
   s->streamOffset = 0;
   s->stream = static_cast<const BYTE *>(data);
   s->stride = args->Stride;
   return S_OK;
}

static HRESULT APIENTRY set_buffer_stream(HANDLE handle, const D3DDDIARG_SETSTREAMSOURCE *args)
{
   if (!handle || !args) return E_INVALIDARG;
   if (args->Stream || args->Stride > max_stream_stride) return D3DERR_NOTAVAILABLE;
   auto *dev = static_cast<LegacyDevice *>(handle);
   auto *buffer = static_cast<Surface *>(args->hVertexBuffer);
   if (buffer && (buffer->owner != dev || !buffer->isBuffer() || buffer->indexSize || args->Offset > buffer->width))
      return E_INVALIDARG;
   auto *s = draw_state(dev);
   if (!s) return E_OUTOFMEMORY;
   dev->vertexStream = buffer;
   s->stream = nullptr;
   s->streamOffset = args->Offset;
   s->stride = args->Stride;
   return S_OK;
}

static HRESULT APIENTRY set_indices(HANDLE handle, const D3DDDIARG_SETINDICES *args)
{
   auto *dev = static_cast<LegacyDevice *>(handle);
   if (!dev || !args)
      return E_INVALIDARG;
   auto *buffer = static_cast<Surface *>(args->hIndexBuffer);
   if (buffer && (buffer->owner != dev || !buffer->indexSize || buffer->indexSize != args->Stride))
      return E_INVALIDARG;
   auto *s = draw_state(dev);
   if (!s)
      return E_OUTOFMEMORY;
   dev->indexStream = buffer;
   s->userIndices = nullptr;
   s->indexSize = buffer ? args->Stride : 0;
   return S_OK;
}

static HRESULT APIENTRY set_indices_um(HANDLE handle, UINT size, const void *indices)
{
   auto *dev = static_cast<LegacyDevice *>(handle);
   if (!dev || (indices && size != 2 && size != 4))
      return E_INVALIDARG;
   auto *s = draw_state(dev);
   if (!s)
      return E_OUTOFMEMORY;
   dev->indexStream = nullptr;
   s->userIndices = static_cast<const BYTE *>(indices);
   s->indexSize = size;
   return S_OK;
}

static HRESULT APIENTRY set_viewport(HANDLE handle, const D3DDDIARG_VIEWPORTINFO *viewport)
{
   if (!handle || !viewport || !viewport->Width || !viewport->Height)
      return E_INVALIDARG;
   auto *dev = static_cast<LegacyDevice *>(handle);
   auto *target = dev->target;
   if (target && (viewport->Width > target->width || viewport->X > target->width - viewport->Width ||
                  viewport->Height > target->height || viewport->Y > target->height - viewport->Height))
      return D3DERR_INVALIDCALL;
   auto *s = draw_state(dev);
   if (!s)
      return E_OUTOFMEMORY;
   s->viewport = *viewport;
   return S_OK;
}

static HRESULT APIENTRY set_scissor(HANDLE handle, const RECT *rect)
{
   auto *dev = static_cast<LegacyDevice *>(handle);
   if (!dev || !rect)
      return E_INVALIDARG;
   if (rect->left < 0 || rect->top < 0 || rect->left > rect->right || rect->top > rect->bottom)
      return D3DERR_INVALIDCALL;
   // The system D3D9 runtime initializes rasterizer state before binding RT0.
   if (dev->target && (UINT(rect->right) > dev->target->width || UINT(rect->bottom) > dev->target->height))
      return D3DERR_INVALIDCALL;
   auto *s = draw_state(dev);
   if (!s)
      return E_OUTOFMEMORY;
   s->scissor = *rect;
   return S_OK;
}

static HRESULT APIENTRY set_depth_range(HANDLE handle, const D3DDDIARG_ZRANGE *range)
{
   if (!handle || !range || !(range->MinZ >= 0 && range->MaxZ <= 1 && range->MinZ <= range->MaxZ))
      return E_INVALIDARG;
   auto *s = draw_state(static_cast<LegacyDevice *>(handle));
   if (!s)
      return E_OUTOFMEMORY;
   s->depthRange = *range;
   return S_OK;
}

// D3D9 initializes old DDI state slots even for a device exposing only
// transformed triangles, one color target, and no depth/stencil formats.
static HRESULT fixed_state(D3DDDIRENDERSTATETYPE state, DWORD value)
{
   // The 22621 x86 runtime submits these two reserved slots at device initialization.
   // Neither is a public D3D9 render state. Accept only the observed initialization value.
   if (UINT(state) == 196 || UINT(state) == 197)
      return value == UINT32_MAX ? S_OK : D3DERR_NOTAVAILABLE;
   if ((state >= D3DDDIRS_WRAP0 && state <= D3DDDIRS_WRAP7) ||
       (state >= D3DDDIRS_WRAP8 && state <= D3DDDIRS_WRAP15))
      return value == 0 ? S_OK : D3DERR_NOTAVAILABLE;
   if (state >= D3DDDIRS_STIPPLEPATTERN00 && state <= D3DDDIRS_STIPPLEPATTERN31)
      return value == 0 || value == UINT32_MAX ? S_OK : D3DERR_NOTAVAILABLE;
   switch (state) {
   case D3DDDIRS_VERTEXBLEND: case D3DDDIRS_LINEPATTERN: case D3DDDIRS_DITHERENABLE: case D3DDDIRS_SPECULARENABLE:
   case D3DDDIRS_ZVISIBLE: case D3DDDIRS_EDGEANTIALIAS: case D3DDDIRS_COLORKEYENABLE:
   case D3DDDIRS_OLDALPHABLENDENABLE: case D3DDDIRS_ZBIAS: case D3DDDIRS_RANGEFOGENABLE:
   case D3DDDIRS_COLORKEYBLENDENABLE: case D3DDDIRS_CLIPPLANEENABLE: case D3DDDIRS_POINTSPRITEENABLE:
   case D3DDDIRS_POINTSCALEENABLE: case D3DDDIRS_INDEXEDVERTEXBLENDENABLE:
   case D3DDDIRS_ENABLEADAPTIVETESSELLATION: case D3DDDIRS_TWOSIDEDSTENCILMODE:
   case D3DDDIRS_ANTIALIASEDLINEENABLE: case D3DDDIRS_SRGBWRITEENABLE:
   case D3DDDIRS_SEPARATEALPHABLENDENABLE: case D3DDDIRS_DEPTHBIAS: case D3DDDIRS_SLOPESCALEDEPTHBIAS:
      return value == 0 ? S_OK : D3DERR_NOTAVAILABLE;
   case D3DDDIRS_SRCBLEND: case D3DDDIRS_SRCBLENDALPHA:
      return value == D3DBLEND_ONE ? S_OK : D3DERR_NOTAVAILABLE;
   case D3DDDIRS_DESTBLEND: case D3DDDIRS_DESTBLENDALPHA:
      return value == D3DBLEND_ZERO ? S_OK : D3DERR_NOTAVAILABLE;
   case D3DDDIRS_BLENDOP: case D3DDDIRS_BLENDOPALPHA:
      return value == D3DBLENDOP_ADD ? S_OK : D3DERR_NOTAVAILABLE;
   case D3DDDIRS_MULTISAMPLEMASK: case D3DDDIRS_TEXTUREFACTOR:
      return value == UINT32_MAX ? S_OK : D3DERR_NOTAVAILABLE;
   case D3DDDIRS_CLIPPING:
      return value ? S_OK : D3DERR_NOTAVAILABLE;
   // These values are only consumed by stages that the current caps exclude,
   // or by system vertex processing before POSITIONT vertices reach this DDI.
   case D3DDDIRS_LASTPIXEL: case D3DDDIRS_ALPHAREF: case D3DDDIRS_ALPHAFUNC:
   case D3DDDIRS_FOGCOLOR: case D3DDDIRS_FOGTABLEMODE: case D3DDDIRS_FOGSTART:
   case D3DDDIRS_FOGEND: case D3DDDIRS_FOGDENSITY: case D3DDDIRS_FOGVERTEXMODE:
   case D3DDDIRS_STENCILFAIL: case D3DDDIRS_STENCILZFAIL: case D3DDDIRS_STENCILPASS:
   case D3DDDIRS_STENCILFUNC: case D3DDDIRS_STENCILREF: case D3DDDIRS_STENCILMASK:
   case D3DDDIRS_STENCILWRITEMASK: case D3DDDIRS_CCW_STENCILFAIL: case D3DDDIRS_CCW_STENCILZFAIL:
   case D3DDDIRS_CCW_STENCILPASS: case D3DDDIRS_CCW_STENCILFUNC: case D3DDDIRS_LIGHTING:
   case D3DDDIRS_AMBIENT: case D3DDDIRS_COLORVERTEX: case D3DDDIRS_LOCALVIEWER:
   case D3DDDIRS_NORMALIZENORMALS: case D3DDDIRS_DIFFUSEMATERIALSOURCE:
   case D3DDDIRS_SPECULARMATERIALSOURCE: case D3DDDIRS_AMBIENTMATERIALSOURCE:
   case D3DDDIRS_EMISSIVEMATERIALSOURCE: case D3DDDIRS_SOFTWAREVERTEXPROCESSING:
   case D3DDDIRS_POINTSIZE: case D3DDDIRS_POINTSIZE_MIN: case D3DDDIRS_POINTSIZE_MAX:
   case D3DDDIRS_POINTSCALE_A: case D3DDDIRS_POINTSCALE_B: case D3DDDIRS_POINTSCALE_C:
   case D3DDDIRS_MULTISAMPLEANTIALIAS: case D3DDDIRS_COLORWRITEENABLE1:
   case D3DDDIRS_COLORWRITEENABLE2: case D3DDDIRS_COLORWRITEENABLE3: case D3DDDIRS_BLENDFACTOR:
   case D3DDDIRS_PATCHEDGESTYLE: case D3DDDIRS_PATCHSEGMENTS: case D3DDDIRS_TWEENFACTOR:
   case D3DDDIRS_POSITIONDEGREE: case D3DDDIRS_NORMALDEGREE: case D3DDDIRS_MINTESSELLATIONLEVEL:
   case D3DDDIRS_MAXTESSELLATIONLEVEL: case D3DDDIRS_ADAPTIVETESS_X: case D3DDDIRS_ADAPTIVETESS_Y:
   case D3DDDIRS_ADAPTIVETESS_Z: case D3DDDIRS_ADAPTIVETESS_W:
      return S_OK;
   default: return D3DERR_NOTAVAILABLE;
   }
}

static HRESULT APIENTRY set_render_state(HANDLE handle, const D3DDDIARG_RENDERSTATE *args)
{
   if (!handle || !args)
      return E_INVALIDARG;
   auto *dev = static_cast<LegacyDevice *>(handle);
   auto *s = draw_state(dev);
   if (!s)
      return E_OUTOFMEMORY;
   switch (static_cast<D3DRENDERSTATETYPE>(args->State)) {
   case D3DRS_CULLMODE:
      switch (args->Value) {
      case D3DCULL_NONE: s->raster.desc.CullMode = D3D10_DDI_CULL_NONE; break;
      case D3DCULL_CW: s->raster.desc.CullMode = D3D10_DDI_CULL_FRONT; break;
      case D3DCULL_CCW: s->raster.desc.CullMode = D3D10_DDI_CULL_BACK; break;
      default: return E_INVALIDARG;
      }
      return S_OK;
   case D3DRS_ZWRITEENABLE:
      s->depth.desc.DepthWriteMask = args->Value ? D3D10_DDI_DEPTH_WRITE_MASK_ALL : D3D10_DDI_DEPTH_WRITE_MASK_ZERO;
      return S_OK;
   case D3DRS_ZFUNC:
      if (args->Value < D3DCMP_NEVER || args->Value > D3DCMP_ALWAYS) return E_INVALIDARG;
      s->depth.desc.DepthFunc = static_cast<D3D10_DDI_COMPARISON_FUNC>(args->Value);
      return S_OK;
   case D3DRS_FILLMODE: return args->Value == D3DFILL_SOLID ? S_OK : D3DERR_NOTAVAILABLE;
   case D3DRS_SHADEMODE: return args->Value == D3DSHADE_GOURAUD ? S_OK : D3DERR_NOTAVAILABLE;
   case D3DRS_COLORWRITEENABLE: return args->Value == 15 ? S_OK : D3DERR_NOTAVAILABLE;
   case D3DRS_ZENABLE: case D3DRS_ALPHABLENDENABLE: case D3DRS_ALPHATESTENABLE:
   case D3DRS_FOGENABLE: case D3DRS_STENCILENABLE:
      return args->Value == FALSE ? S_OK : D3DERR_NOTAVAILABLE;
   case D3DRS_SCISSORTESTENABLE:
      s->raster.desc.ScissorEnable = args->Value != FALSE;
      return S_OK;
   default: return fixed_state(args->State, args->Value);
   }
}

template<size_t Inputs, size_t Outputs>
static HRESULT create_color_shader(LegacyDevice *dev, Stage stage, const UINT *code, BYTE *storage,
                                  const D3D10DDIARG_SIGNATURE_ENTRY (&inputs)[Inputs],
                                  const D3D10DDIARG_SIGNATURE_ENTRY (&outputs)[Outputs])
{
   // The WDK's legacy signature structure takes writable arrays.
   std::array<D3D10DDIARG_SIGNATURE_ENTRY, Inputs> in;
   std::array<D3D10DDIARG_SIGNATURE_ENTRY, Outputs> out;
   std::copy_n(inputs, Inputs, in.begin());
   std::copy_n(outputs, Outputs, out.begin());
   D3D10DDIARG_STAGE_IO_SIGNATURES sig{in.data(), UINT(Inputs), out.data(), UINT(Outputs)};
   const auto create = stage == STAGE_VS ? dev->graphics.pfnCreateVertexShader : dev->graphics.pfnCreatePixelShader;
   create(dev->handle(), code, {storage}, {}, &sig);
   return dev->error;
}

static HRESULT color_shaders(LegacyDevice *dev, DrawState &s)
{
   if (!s.vsReady) {
      if (FAILED(create_color_shader(dev, STAGE_VS, color_vs, s.vs, color_vs_in, color_vs_out)))
         return dev->error;
      s.vsReady = true;
   }
   if (!s.psReady) {
      if (FAILED(create_color_shader(dev, STAGE_PS, color_ps, s.ps, color_ps_in, color_ps_out)))
         return dev->error;
      s.psReady = true;
   }
   return S_OK;
}

static HRESULT APIENTRY set_texture(HANDLE handle, UINT stage, HANDLE resource)
{
   auto *dev = static_cast<LegacyDevice *>(handle);
   if (!dev) return E_INVALIDARG;
   if (stage) return resource ? D3DERR_NOTAVAILABLE : S_OK;
   auto *surface = static_cast<Surface *>(resource);
   if (surface && (surface->owner != dev || !surface->texture || surface->system))
      return D3DERR_INVALIDCALL;
   dev->sampled = surface;
   return S_OK;
}

static HRESULT APIENTRY set_texture_state(HANDLE handle, const D3DDDIARG_TEXTURESTAGESTATE *args)
{
   if (!handle || !args) return E_INVALIDARG;
   if (args->Stage) {
      if (args->Stage >= 16 && (args->Stage < D3DDMAPSAMPLER || args->Stage > D3DVERTEXTEXTURESAMPLER3))
         return E_INVALIDARG;
      // State for a disabled stage has no effect. Reject attempts to enable
      // additional combiners or to bind a second texture.
      if (args->State == D3DDDITSS_COLOROP && args->Value != D3DTOP_DISABLE) return D3DERR_NOTAVAILABLE;
      return args->State >= D3DDDITSS_COLOROP && args->State <= D3DDDITSS_TEXTURECOLORKEYVAL ? S_OK : E_INVALIDARG;
   }
   auto *s = draw_state(static_cast<LegacyDevice *>(handle));
   if (!s) return E_OUTOFMEMORY;
   const UINT value = args->Value;
   UINT *field = nullptr;
   switch (args->State) {
   case D3DDDITSS_COLOROP: case D3DDDITSS_ALPHAOP:
      if (value < D3DTOP_DISABLE || value > D3DTOP_MODULATE) return D3DERR_NOTAVAILABLE;
      if (args->State == D3DDDITSS_ALPHAOP && value == D3DTOP_DISABLE) return D3DERR_NOTAVAILABLE;
      field = args->State == D3DDDITSS_COLOROP ? s->stage.color : s->stage.alpha;
      break;
   case D3DDDITSS_COLORARG1: case D3DDDITSS_COLORARG2: case D3DDDITSS_ALPHAARG1: case D3DDDITSS_ALPHAARG2:
      if (value & ~(D3DTA_SELECTMASK | D3DTA_COMPLEMENT | D3DTA_ALPHAREPLICATE) ||
          (value & D3DTA_SELECTMASK) > D3DTA_TEXTURE) return D3DERR_NOTAVAILABLE;
      field = args->State <= D3DDDITSS_COLORARG2 ? &s->stage.color[1 + args->State - D3DDDITSS_COLORARG1]
                                                 : &s->stage.alpha[1 + args->State - D3DDDITSS_ALPHAARG1];
      break;
   case D3DDDITSS_ADDRESSU: case D3DDDITSS_ADDRESSV:
      if (value != D3DTADDRESS_WRAP && value != D3DTADDRESS_CLAMP) return D3DERR_NOTAVAILABLE;
      field = args->State == D3DDDITSS_ADDRESSU ? &s->addressU : &s->addressV;
      break;
   case D3DDDITSS_MINFILTER: case D3DDDITSS_MAGFILTER: case D3DDDITSS_MIPFILTER:
      if (value != D3DTEXF_POINT && value != D3DTEXF_LINEAR &&
          !(value == D3DTEXF_NONE && args->State == D3DDDITSS_MIPFILTER)) return D3DERR_NOTAVAILABLE;
      field = args->State == D3DDDITSS_MINFILTER ? &s->minFilter :
              args->State == D3DDDITSS_MAGFILTER ? &s->magFilter : &s->mipFilter;
      break;
   case D3DDDITSS_BUMPENVMAT00: case D3DDDITSS_BUMPENVMAT01:
   case D3DDDITSS_BUMPENVMAT10: case D3DDDITSS_BUMPENVMAT11:
   case D3DDDITSS_BUMPENVLSCALE: case D3DDDITSS_BUMPENVLOFFSET:
   case D3DDDITSS_CONSTANT: case D3DDDITSS_BORDERCOLOR: case D3DDDITSS_MIPMAPLODBIAS:
   case D3DDDITSS_DMAPOFFSET: case D3DDDITSS_ELEMENTINDEX:
      return value == 0 ? S_OK : D3DERR_NOTAVAILABLE;
   case D3DDDITSS_COLORARG0: case D3DDDITSS_ALPHAARG0:
      return value == D3DTA_CURRENT ? S_OK : D3DERR_NOTAVAILABLE;
   case D3DDDITSS_MAXANISOTROPY:
      return value == 1 ? S_OK : D3DERR_NOTAVAILABLE;
   case D3DDDITSS_ADDRESSW:
      return value == D3DTADDRESS_WRAP || value == D3DTADDRESS_CLAMP ? S_OK : D3DERR_NOTAVAILABLE;
   case D3DDDITSS_MAXMIPLEVEL: field = &s->maxMip; break;
   case D3DDDITSS_TEXCOORDINDEX: case D3DDDITSS_SRGBTEXTURE: case D3DDDITSS_TEXTURETRANSFORMFLAGS:
      return value == 0 ? S_OK : D3DERR_NOTAVAILABLE;
   case D3DDDITSS_RESULTARG: return value == D3DTA_CURRENT ? S_OK : D3DERR_NOTAVAILABLE;
   default: return D3DERR_NOTAVAILABLE;
   }
   if (*field != value) {
      *field = value;
      if (args->State <= D3DDDITSS_ALPHAARG2) s->constantsDirty = true;
      else s->samplerDirty = true;
   }
   return S_OK;
}

static HRESULT texture_pipeline(LegacyDevice *dev, DrawState &s)
{
   HRESULT hr = prepare_texture(dev->sampled);
   if (FAILED(hr)) return hr;
   if (!s.textureVsReady) {
      if (FAILED(create_color_shader(dev, STAGE_VS, texture_vs, s.textureVs, texture_vs_in, texture_vs_out)))
         return dev->error;
      s.textureVsReady = true;
   }
   if (!s.texturePsReady) {
      if (FAILED(create_color_shader(dev, STAGE_PS, texture_ps, s.texturePs, texture_ps_in, texture_ps_out)))
         return dev->error;
      s.texturePsReady = true;
   }
   auto &table = dev->graphics;
   const auto h = dev->handle();
   if (s.samplerDirty) {
      if (s.samplerReady) {
         table.pfnDestroySampler(h, {&s.sampler});
         s.samplerReady = false;
      }
      D3D10_DDI_SAMPLER_DESC desc{};
      desc.Filter = static_cast<D3D10_DDI_FILTER>((s.minFilter == D3DTEXF_LINEAR ? 0x10 : 0) |
                    (s.magFilter == D3DTEXF_LINEAR ? 4 : 0) | (s.mipFilter == D3DTEXF_LINEAR ? 1 : 0));
      desc.AddressU = static_cast<D3D10_DDI_TEXTURE_ADDRESS_MODE>(s.addressU);
      desc.AddressV = static_cast<D3D10_DDI_TEXTURE_ADDRESS_MODE>(s.addressV);
      desc.AddressW = D3D10_DDI_TEXTURE_ADDRESS_CLAMP;
      desc.MinLOD = float(s.maxMip);
      desc.MaxLOD = s.mipFilter == D3DTEXF_NONE ? desc.MinLOD : D3D10_FLOAT32_MAX;
      desc.MaxAnisotropy = 1;
      desc.ComparisonFunc = D3D10_DDI_COMPARISON_ALWAYS;
      table.pfnCreateSampler(h, &desc, {&s.sampler}, {});
      if (FAILED(dev->error)) return dev->error;
      s.samplerReady = true;
      s.samplerDirty = false;
   }
   if (!s.constantsReady) {
      constexpr UINT bytes = sizeof(s.stage);
      D3D10DDI_MIPINFO mip{bytes, 1, 1, bytes, 1, 1};
      D3D10DDIARG_CREATERESOURCE desc{};
      desc.ResourceDimension = D3D10DDIRESOURCE_BUFFER;
      desc.Usage = D3D10_DDI_USAGE_DYNAMIC;
      desc.BindFlags = D3D10_DDI_BIND_CONSTANT_BUFFER;
      desc.MapFlags = D3D10_DDI_CPU_ACCESS_WRITE;
      desc.pMipInfoList = &mip;
      desc.SampleDesc.Count = desc.MipLevels = desc.ArraySize = 1;
      table.pfnCreateResource(h, &desc, {s.constants}, {});
      if (FAILED(dev->error)) return dev->error;
      s.constantsReady = true;
   }
   D3D10DDI_HRESOURCE constants{s.constants};
   if (s.constantsDirty) {
      D3D10DDI_MAPPED_SUBRESOURCE mapped{};
      table.pfnDynamicConstantBufferMapDiscard(h, constants, 0, D3D10_DDI_MAP_WRITE_DISCARD, 0, &mapped);
      if (FAILED(dev->error)) return dev->error;
      std::memcpy(mapped.pData, &s.stage, sizeof(s.stage));
      table.pfnDynamicConstantBufferUnmap(h, constants, 0);
      if (FAILED(dev->error)) return dev->error;
      s.constantsDirty = false;
   }
   D3D10DDI_HSAMPLER sampler{&s.sampler};
   D3D10DDI_HSHADERRESOURCEVIEW srv{dev->sampled->view};
   table.pfnPsSetConstantBuffers(h, 0, 1, &constants);
   table.pfnPsSetShaderResources(h, 0, 1, &srv);
   table.pfnPsSetSamplers(h, 0, 1, &sampler);
   return dev->error;
}

static HRESULT vertex_buffer(LegacyDevice *dev, DrawState &s, UINT bytes)
{
   if (bytes <= s.capacity)
      return S_OK;
   if (s.capacity) {
      dev->graphics.pfnDestroyResource(dev->handle(), {s.vertices});
      s.capacity = 0;
      if (FAILED(dev->error))
         return dev->error;
   }
   D3D10DDI_MIPINFO mip{bytes, 1, 1, bytes, 1, 1};
   D3D10DDIARG_CREATERESOURCE desc{};
   desc.ResourceDimension = D3D10DDIRESOURCE_BUFFER;
   desc.Usage = D3D10_DDI_USAGE_DYNAMIC;
   desc.BindFlags = D3D10_DDI_BIND_VERTEX_BUFFER;
   desc.MapFlags = D3D10_DDI_CPU_ACCESS_WRITE;
   desc.pMipInfoList = &mip;
   desc.SampleDesc.Count = desc.MipLevels = desc.ArraySize = 1;
   dev->graphics.pfnCreateResource(dev->handle(), &desc, {s.vertices}, {});
   if (SUCCEEDED(dev->error))
      s.capacity = bytes;
   return dev->error;
}

static ColorVertex transform(const BYTE *source, const DrawState &s)
{
   float position[4];
   std::memcpy(position, source + s.decl->position, sizeof(position));
   const auto &v = s.viewport;
   const float w = position[3] == 0.0f ? 1.0f : 1.0f / position[3];
   // D3D9 sample centers lie on integers; Vulkan centers lie half a pixel further in.
   const float x = (position[0] - float(v.X) + 0.5f) * (2.0f / float(v.Width)) - 1.0f;
   const float y = 1.0f - (position[1] - float(v.Y) + 0.5f) * (2.0f / float(v.Height));
   const float interval = s.depthRange.MaxZ - s.depthRange.MinZ;
   const float z = interval > 0 ? (position[2] - s.depthRange.MinZ) / interval : 0.0f;
   uint32_t argb = UINT32_MAX;
   if (s.decl->diffuse != UINT_MAX)
      std::memcpy(&argb, source + s.decl->diffuse, sizeof(argb));
   const uint32_t rgba = (argb & 0xff00ff00u) | ((argb >> 16) & 0xffu) | ((argb & 0xffu) << 16);
   ColorVertex result{x*w, y*w, z*w, w, rgba, {}};
   if (s.decl->texcoord != UINT_MAX)
      std::memcpy(result.uv, source + s.decl->texcoord, sizeof(float) * std::min(2u, s.decl->coordFloats));
   return result;
}

struct IndexSpan {
   const BYTE *data;
   uint64_t start;
   uint64_t bytes;
   UINT size, minimum, vertices;
};

static bool valid_span(const BYTE *data, uint64_t bytes, uint64_t start, uint64_t length)
{
   if (!data || !length || start > bytes || length > bytes - start)
      return false;
   const uint64_t addressRoom = UINTPTR_MAX - reinterpret_cast<uintptr_t>(data);
   return start <= addressRoom && length - 1 <= addressRoom - start;
}

static HRESULT draw(LegacyDevice *dev, D3DPRIMITIVETYPE primitive, UINT primitives, int64_t offset,
                    const IndexSpan *indices = nullptr)
{
   if (!primitives)
      return S_OK;
   DrawState *s = dev->draw;
   if (!dev->target || dev->target->locked || !s || !s->decl || (s->stride && s->stride < s->decl->extent))
      return D3DERR_INVALIDCALL;
   if (primitive != D3DPT_TRIANGLELIST && primitive != D3DPT_TRIANGLESTRIP)
      return D3DERR_NOTAVAILABLE;
   if (primitives > max_primitives)
      return D3DERR_INVALIDCALL;
   const UINT count = primitive == D3DPT_TRIANGLELIST ? primitives * 3 : primitives + 2;
   UINT minimum = 0, verticesInRange = count;
   if (indices) {
      if (!indices->vertices || indices->minimum > max_vertex_index ||
          indices->vertices - 1 > max_vertex_index - indices->minimum ||
          (indices->size != 2 && indices->size != 4) ||
          !valid_span(indices->data, indices->bytes, indices->start, uint64_t(count) * indices->size))
         return E_INVALIDARG;
      minimum = indices->minimum;
      verticesInRange = indices->vertices;
   }

   const BYTE *source = s->stream;
   uint64_t bytes = UINT64_MAX; // SetStreamSourceUm supplies no allocation size.
   if (Surface *buffer = dev->vertexStream) {
      if (buffer->locked && !buffer->allowLockedDraw) return D3DERR_INVALIDCALL;
      source = buffer->bufferData();
      bytes = buffer->width;
      source += s->streamOffset;
      bytes -= s->streamOffset;
   }
   if (!source) return D3DERR_INVALIDCALL;
   offset += int64_t(minimum) * s->stride;
   const uint64_t span = uint64_t(verticesInRange - 1) * s->stride + s->decl->extent;
   if (offset < 0 || !valid_span(source, bytes, uint64_t(offset), span))
      return E_INVALIDARG;
   source += size_t(offset);
   dev->begin();
   if (FAILED(dev->error)) return dev->error;
   const bool textured = dev->sampled && s->stage.color[0] != D3DTOP_DISABLE;
   HRESULT hr = textured ? texture_pipeline(dev, *s) : color_shaders(dev, *s);
   if (FAILED(hr)) return hr;
   if (FAILED(vertex_buffer(dev, *s, count * sizeof(ColorVertex))))
      return dev->error;
   auto &table = dev->graphics;
   const auto h = dev->handle();
   D3D10DDI_HRESOURCE vertices{s->vertices};
   D3D10DDI_MAPPED_SUBRESOURCE mapped{};
   table.pfnDynamicIABufferMapDiscard(h, vertices, 0, D3D10_DDI_MAP_WRITE_DISCARD, 0, &mapped);
   if (FAILED(dev->error))
      return dev->error;
   auto *out = static_cast<ColorVertex *>(mapped.pData);
   bool valid = true;
   const BYTE *indexBytes = indices ? indices->data + size_t(indices->start) : nullptr;
   for (UINT i = 0; i < count; ++i) {
      UINT vertex = i;
      if (indices) {
         UINT value = 0;
         std::memcpy(&value, indexBytes + size_t(i) * indices->size, indices->size);
         if (value < minimum || value - minimum >= verticesInRange) {
            valid = false;
            break;
         }
         vertex = value - minimum;
      }
      out[i] = transform(source + size_t(vertex) * s->stride, *s);
   }
   table.pfnDynamicIABufferUnmap(h, vertices, 0);
   if (FAILED(dev->error))
      return dev->error;
   if (!valid) return E_INVALIDARG;
   table.pfnVsSetShader(h, {textured ? s->textureVs : s->vs});
   table.pfnPsSetShader(h, {textured ? s->texturePs : s->ps});
   table.pfnIaSetInputLayout(h, {textured ? &s->textureLayout : &s->layout});
   const UINT stride = sizeof(ColorVertex), start = 0;
   table.pfnIaSetVertexBuffers(h, 0, 1, &vertices, &stride, &start);
   table.pfnIaSetTopology(h, primitive == D3DPT_TRIANGLELIST ? D3D10_DDI_PRIMITIVE_TOPOLOGY_TRIANGLELIST
                                                            : D3D10_DDI_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
   const auto &v = s->viewport;
   D3D10_DDI_VIEWPORT viewport{float(v.X), float(v.Y), float(v.Width), float(v.Height),
                               s->depthRange.MinZ, s->depthRange.MaxZ};
   table.pfnSetViewports(h, 1, 0, &viewport);
   const D3D10_DDI_RECT scissor{s->scissor.left, s->scissor.top, s->scissor.right, s->scissor.bottom};
   table.pfnSetScissorRects(h, 1, 0, &scissor);
   table.pfnSetRasterizerState(h, {&s->raster});
   table.pfnSetDepthStencilState(h, {&s->depth}, 0);
   table.pfnDraw(h, count, 0);
   dev->target->shadowCurrent = false;
   return dev->error;
}

static HRESULT APIENTRY draw_primitive(HANDLE handle, const D3DDDIARG_DRAWPRIMITIVE *args, const UINT *)
{
   if (!handle || !args)
      return E_INVALIDARG;
   auto *dev = static_cast<LegacyDevice *>(handle);
   int64_t offset = dev->draw ? int64_t(args->VStart) * dev->draw->stride : 0;
   return draw(dev, args->PrimitiveType, args->PrimitiveCount, offset);
}

static HRESULT APIENTRY draw_primitive2(HANDLE handle, const D3DDDIARG_DRAWPRIMITIVE2 *args)
{
   if (!handle || !args)
      return E_INVALIDARG;
   return draw(static_cast<LegacyDevice *>(handle), args->PrimitiveType, args->PrimitiveCount, args->FirstVertexOffset);
}

static HRESULT APIENTRY draw_indexed(HANDLE handle, const D3DDDIARG_DRAWINDEXEDPRIMITIVE *args)
{
   auto *dev = static_cast<LegacyDevice *>(handle);
   if (!dev || !args) return E_INVALIDARG;
   const DrawState *s = dev->draw;
   if (!s || (!dev->indexStream && !s->userIndices)) return D3DERR_INVALIDCALL;
   Surface *buffer = dev->indexStream;
   if (buffer && buffer->locked && !buffer->allowLockedDraw) return D3DERR_INVALIDCALL;
   IndexSpan indices{buffer ? buffer->bufferData() : s->userIndices, uint64_t(args->StartIndex) * s->indexSize,
                     buffer ? buffer->width : UINT64_MAX, s->indexSize, args->MinIndex, args->NumVertices};
   return draw(dev, args->PrimitiveType, args->PrimitiveCount, int64_t(args->BaseVertexIndex) * s->stride, &indices);
}

static HRESULT APIENTRY draw_indexed2(HANDLE handle, const D3DDDIARG_DRAWINDEXEDPRIMITIVE2 *args,
                                      UINT size, const void *data, const UINT *)
{
   if (!handle || !args) return E_INVALIDARG;
   IndexSpan indices{static_cast<const BYTE *>(data), args->StartIndexOffset, UINT64_MAX,
                     size, args->MinIndex, args->NumVertices};
   return draw(static_cast<LegacyDevice *>(handle), args->PrimitiveType, args->PrimitiveCount,
               args->BaseVertexOffset, &indices);
}

void destroy_draw_state(LegacyDevice *dev)
{
   auto *s = dev->draw;
   if (!s)
      return;
   auto &table = dev->graphics;
   const auto h = dev->handle();
   table.pfnIaSetInputLayout(h, {});
   table.pfnSetDepthStencilState(h, {}, 0);
   table.pfnSetRasterizerState(h, {});
   if (s->capacity)
      table.pfnDestroyResource(h, {s->vertices});
   if (s->samplerReady) table.pfnDestroySampler(h, {&s->sampler});
   if (s->constantsReady) table.pfnDestroyResource(h, {s->constants});
   if (s->texturePsReady) table.pfnDestroyShader(h, {s->texturePs});
   if (s->textureVsReady) table.pfnDestroyShader(h, {s->textureVs});
   if (s->psReady)
      table.pfnDestroyShader(h, {s->ps});
   if (s->vsReady)
      table.pfnDestroyShader(h, {s->vs});
   while (s->declarations)
      delete_decl(dev, s->declarations);
   delete s;
   dev->draw = nullptr;
}

static HRESULT APIENTRY set_fixed_shader(HANDLE handle, HANDLE shader)
{
   if (!handle) return E_INVALIDARG;
   // A null programmable shader selects our fixed-function path.
   return shader ? D3DERR_NOTAVAILABLE : static_cast<LegacyDevice *>(handle)->slot.state->lost;
}

static HRESULT APIENTRY set_depth_surface(HANDLE handle, const D3DDDIARG_SETDEPTHSTENCIL *args)
{
   if (!handle || !args) return E_INVALIDARG;
   // No depth formats are exposed. A null binding is the normal color-only configuration.
   return args->hZBuffer ? D3DERR_NOTAVAILABLE : static_cast<LegacyDevice *>(handle)->slot.state->lost;
}

void install_draw_functions(D3DDDI_DEVICEFUNCS *table)
{
   table->pfnCreateVertexShaderDecl = create_decl;
   table->pfnSetVertexShaderDecl = bind_decl;
   table->pfnDeleteVertexShaderDecl = delete_decl;
   table->pfnSetStreamSource = set_buffer_stream;
   table->pfnSetStreamSourceUm = set_stream;
   table->pfnSetViewport = set_viewport;
   table->pfnSetScissorRect = set_scissor;
   table->pfnSetZRange = set_depth_range;
   table->pfnSetRenderState = set_render_state;
   table->pfnSetPixelShader = set_fixed_shader;
   table->pfnSetVertexShaderFunc = set_fixed_shader;
   table->pfnSetDepthStencil = set_depth_surface;
   table->pfnSetTexture = set_texture;
   table->pfnSetTextureStageState = set_texture_state;
   table->pfnSetIndices = set_indices;
   table->pfnSetIndicesUm = set_indices_um;
   table->pfnDrawIndexedPrimitive = draw_indexed;
   table->pfnDrawIndexedPrimitive2 = draw_indexed2;
   table->pfnDrawPrimitive = draw_primitive;
   table->pfnDrawPrimitive2 = draw_primitive2;
}

} // namespace tgd3d::legacy9
