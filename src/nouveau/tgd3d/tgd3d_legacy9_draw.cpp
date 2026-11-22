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
   UINT extent = 0;
};

struct ColorVertex {
   float x, y, z, w;
   uint32_t rgba;
};

struct DrawState {
   Declaration *declarations = nullptr;
   Declaration *decl = nullptr;
   const BYTE *stream = nullptr;
   UINT stride = 0;
   D3DDDIARG_VIEWPORTINFO viewport{};
   D3DDDIARG_ZRANGE depthRange{0, 1};
   RasterizerState raster{};
   DepthStencilState depth{};
   ElementLayout layout{};
   alignas(Shader) BYTE vs[sizeof(Shader)], ps[sizeof(Shader)];
   bool vsReady = false, psReady = false;
   alignas(Resource) BYTE vertices[sizeof(Resource)];
   UINT capacity = 0;
};

void draw_target_changed(LegacyDevice *dev)
{
   if (dev->draw && dev->target) {
      dev->draw->viewport = {0, 0, dev->target->width, dev->target->height};
      dev->draw->depthRange = {0, 1};
   }
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
   s->stream = static_cast<const BYTE *>(data);
   s->stride = args->Stride;
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
   case D3DRS_FILLMODE: return args->Value == D3DFILL_SOLID ? S_OK : D3DERR_NOTAVAILABLE;
   case D3DRS_SHADEMODE: return args->Value == D3DSHADE_GOURAUD ? S_OK : D3DERR_NOTAVAILABLE;
   case D3DRS_COLORWRITEENABLE: return args->Value == 15 ? S_OK : D3DERR_NOTAVAILABLE;
   case D3DRS_ZENABLE: case D3DRS_ALPHABLENDENABLE: case D3DRS_ALPHATESTENABLE:
   case D3DRS_FOGENABLE: case D3DRS_STENCILENABLE: case D3DRS_SCISSORTESTENABLE:
      return args->Value == FALSE ? S_OK : D3DERR_NOTAVAILABLE;
   default: return D3DERR_NOTAVAILABLE;
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
   return {x*w, y*w, z*w, w, rgba};
}

static HRESULT draw(LegacyDevice *dev, D3DPRIMITIVETYPE primitive, UINT primitives, uint64_t offset)
{
   if (!primitives)
      return S_OK;
   DrawState *s = dev->draw;
   if (!dev->target || dev->target->locked || !s || !s->decl || !s->stream || s->stride < s->decl->extent)
      return D3DERR_INVALIDCALL;
   if (primitive != D3DPT_TRIANGLELIST && primitive != D3DPT_TRIANGLESTRIP)
      return D3DERR_NOTAVAILABLE;
   if (primitives > max_primitives)
      return D3DERR_INVALIDCALL;
   const UINT count = primitive == D3DPT_TRIANGLELIST ? primitives * 3 : primitives + 2;
   const uint64_t tail = uint64_t(count - 1) * s->stride + s->decl->extent;
   const uintptr_t base = reinterpret_cast<uintptr_t>(s->stream);
   if (offset > UINTPTR_MAX - base || tail > UINTPTR_MAX - base - offset)
      return E_INVALIDARG;
   dev->begin();
   if (FAILED(dev->error) || FAILED(color_shaders(dev, *s)) ||
       FAILED(vertex_buffer(dev, *s, count * sizeof(ColorVertex))))
      return dev->error;
   auto &table = dev->graphics;
   const auto h = dev->handle();
   D3D10DDI_HRESOURCE vertices{s->vertices};
   D3D10DDI_MAPPED_SUBRESOURCE mapped{};
   table.pfnDynamicIABufferMapDiscard(h, vertices, 0, D3D10_DDI_MAP_WRITE_DISCARD, 0, &mapped);
   if (FAILED(dev->error))
      return dev->error;
   auto *out = static_cast<ColorVertex *>(mapped.pData);
   for (UINT i = 0; i < count; ++i)
      out[i] = transform(s->stream + size_t(offset) + size_t(i) * s->stride, *s);
   table.pfnDynamicIABufferUnmap(h, vertices, 0);
   if (FAILED(dev->error))
      return dev->error;
   table.pfnVsSetShader(h, {s->vs});
   table.pfnPsSetShader(h, {s->ps});
   table.pfnIaSetInputLayout(h, {&s->layout});
   const UINT stride = sizeof(ColorVertex), start = 0;
   table.pfnIaSetVertexBuffers(h, 0, 1, &vertices, &stride, &start);
   table.pfnIaSetTopology(h, primitive == D3DPT_TRIANGLELIST ? D3D10_DDI_PRIMITIVE_TOPOLOGY_TRIANGLELIST
                                                            : D3D10_DDI_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
   const auto &v = s->viewport;
   D3D10_DDI_VIEWPORT viewport{float(v.X), float(v.Y), float(v.Width), float(v.Height),
                               s->depthRange.MinZ, s->depthRange.MaxZ};
   table.pfnSetViewports(h, 1, 0, &viewport);
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
   uint64_t offset = dev->draw ? uint64_t(args->VStart) * dev->draw->stride : 0;
   return draw(dev, args->PrimitiveType, args->PrimitiveCount, offset);
}

static HRESULT APIENTRY draw_primitive2(HANDLE handle, const D3DDDIARG_DRAWPRIMITIVE2 *args)
{
   if (!handle || !args)
      return E_INVALIDARG;
   return draw(static_cast<LegacyDevice *>(handle), args->PrimitiveType, args->PrimitiveCount, args->FirstVertexOffset);
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
   if (s->psReady)
      table.pfnDestroyShader(h, {s->ps});
   if (s->vsReady)
      table.pfnDestroyShader(h, {s->vs});
   while (s->declarations)
      delete_decl(dev, s->declarations);
   delete s;
   dev->draw = nullptr;
}

void install_draw_functions(D3DDDI_DEVICEFUNCS *table)
{
   table->pfnCreateVertexShaderDecl = create_decl;
   table->pfnSetVertexShaderDecl = bind_decl;
   table->pfnDeleteVertexShaderDecl = delete_decl;
   table->pfnSetStreamSourceUm = set_stream;
   table->pfnSetViewport = set_viewport;
   table->pfnSetZRange = set_depth_range;
   table->pfnSetRenderState = set_render_state;
   table->pfnDrawPrimitive = draw_primitive;
   table->pfnDrawPrimitive2 = draw_primitive2;
}

} // namespace tgd3d::legacy9
