/* SPDX-License-Identifier: MIT */
/* Direct DDI smoke test with real KMT/NVK. This is not the D3D11 runtime. */
#include <windows.h>
#include "winddk_compat.h"
#include <d3d10_1.h>
#include <d3d10umddi.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdexcept>
#include <vector>

#include "nouveau/vulkan/nvkmd/windows/kmt_abi.h"
#include "nouveau/vulkan/nvkmd/windows/adapter_abi.h"

/* Existing Mesa/apitrace SM4 passthrough shaders; keep their upstream source. */
#include "tri_vs_4_0.h"
#include "tri_ps_4_0.h"

static LONG (WINAPI *query_kmt)(const tg_kmt_query *);
static LONG (WINAPI *close_kmt)(const tg_kmt_close *);
static HRESULT ddi_error;

static void exception_address(const char *name, const void *address)
{
   HMODULE module = NULL;
   char path[MAX_PATH] = "<unknown>";
   if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                          GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          static_cast<const char *>(address), &module))
      GetModuleFileNameA(module, path, sizeof(path));
   fprintf(stderr, "%s: %p %s+0x%llx\n", name, address, path,
           (unsigned long long)((uintptr_t)address - (uintptr_t)module));
}

static LONG WINAPI unhandled_exception(EXCEPTION_POINTERS *exception)
{
   fprintf(stderr, "Unhandled exception: %08lx\n", exception->ExceptionRecord->ExceptionCode);
   exception_address("Fault", exception->ExceptionRecord->ExceptionAddress);
#if defined(_M_ARM64) || defined(__aarch64__)
   exception_address("LR", (const void *)(uintptr_t)exception->ContextRecord->Lr);
#endif
   fflush(stderr);
   return EXCEPTION_EXECUTE_HANDLER;
}

static void require(bool ok, const char *operation)
{
   if (!ok)
      throw std::runtime_error(operation);
}

static void step(const char *operation)
{
   printf("%s: %08lx\n", operation, (unsigned long)ddi_error);
   fflush(stdout);
   require(SUCCEEDED(ddi_error), operation);
}

static HRESULT APIENTRY query_adapter(HANDLE adapter, const D3DDDICB_QUERYADAPTERINFO *args)
{
   const tg_kmt_query query = {(uint32_t)(uintptr_t)adapter, TG_KMT_UMDRIVERPRIVATE,
                             args->pPrivateDriverData, args->PrivateDriverDataSize};
   LONG status = query_kmt(&query);
   printf("KMT adapter query: %08lx\n", (unsigned long)status);
   if (status >= 0 && args->PrivateDriverDataSize == sizeof(tg_gpu_adapter_info)) {
      const auto *info = static_cast<const tg_gpu_adapter_info *>(args->pPrivateDriverData);
      printf("KMD identity: vendor=%04x chipset=%x flags=%x LUID=%08x%08x budget=%llu\n",
             info->vendor, info->chipset, info->flags, (uint32_t)info->luid_high, info->luid_low,
             (unsigned long long)info->memory_budget_bytes);
   }
   fflush(stdout);
   return status >= 0 ? S_OK : HRESULT_FROM_NT(status);
}

static void APIENTRY set_error(D3D10DDI_HRTCORELAYER, HRESULT error)
{
   ddi_error = error;
   fprintf(stderr, "DDI SetError: %08lx\n", (unsigned long)error);
   fflush(stderr);
}

struct Run {
   HMODULE gdi = NULL, umd = NULL;
   uint32_t kmt_adapter = 0;
   D3D10DDIARG_OPENADAPTER adapter = {};
   D3D10_2DDI_ADAPTERFUNCS adapters = {};
   D3D10DDI_HDEVICE device = {};
   D3D10DDI_DEVICEFUNCS functions = {};
   D3D10DDI_HRESOURCE target = {}, staging = {};
   D3D10DDI_HRENDERTARGETVIEW view = {};
   bool device_live = false, target_live = false, staging_live = false, view_live = false, mapped = false;
   D3D10DDI_HRESOURCE vertices = {}, indices = {};
   D3D10DDI_HSHADER vs = {}, ps = {};
   D3D10DDI_HELEMENTLAYOUT layout = {};
   D3D10DDI_HRASTERIZERSTATE raster = {};
   D3D10DDI_HBLENDSTATE blend = {};
   D3D10DDI_HDEPTHSTENCILSTATE depth = {};
   bool vertices_live = false, indices_live = false, vs_live = false, ps_live = false;
   bool layout_live = false, raster_live = false, blend_live = false, depth_live = false, draw_bound = false;

   ~Run()
   {
      if (mapped) functions.pfnStagingResourceUnmap(device, staging, 0);
      if (draw_bound) {
         const D3D10DDI_HRESOURCE none = {};
         const UINT zero = 0;
         const FLOAT factor[4] = {};
         functions.pfnSetRenderTargets(device, NULL, 0, 1, {});
         functions.pfnVsSetShader(device, {});
         functions.pfnPsSetShader(device, {});
         functions.pfnIaSetInputLayout(device, {});
         functions.pfnIaSetVertexBuffers(device, 0, 1, &none, &zero, &zero);
         functions.pfnIaSetIndexBuffer(device, {}, DXGI_FORMAT_UNKNOWN, 0);
         functions.pfnSetRasterizerState(device, {});
         functions.pfnSetBlendState(device, {}, factor, ~0u);
         functions.pfnSetDepthStencilState(device, {}, 0);
      }
      if (depth_live) functions.pfnDestroyDepthStencilState(device, depth);
      if (blend_live) functions.pfnDestroyBlendState(device, blend);
      if (raster_live) functions.pfnDestroyRasterizerState(device, raster);
      if (layout_live) functions.pfnDestroyElementLayout(device, layout);
      if (ps_live) functions.pfnDestroyShader(device, ps);
      if (vs_live) functions.pfnDestroyShader(device, vs);
      if (indices_live) functions.pfnDestroyResource(device, indices);
      if (vertices_live) functions.pfnDestroyResource(device, vertices);
      if (view_live) functions.pfnDestroyRenderTargetView(device, view);
      if (staging_live) functions.pfnDestroyResource(device, staging);
      if (target_live) functions.pfnDestroyResource(device, target);
      if (device_live) functions.pfnDestroyDevice(device);
      free(view.pDrvPrivate); free(staging.pDrvPrivate); free(target.pDrvPrivate); free(device.pDrvPrivate);
      free(vertices.pDrvPrivate); free(indices.pDrvPrivate); free(vs.pDrvPrivate); free(ps.pDrvPrivate);
      free(layout.pDrvPrivate); free(raster.pDrvPrivate); free(blend.pDrvPrivate); free(depth.pDrvPrivate);
      if (adapter.hAdapter.pDrvPrivate) {
         HRESULT status = adapters.pfnCloseAdapter(adapter.hAdapter);
         printf("CloseAdapter: %08lx\n", (unsigned long)status);
         if (FAILED(status)) ddi_error = status;
      }
      if (kmt_adapter) {
         const tg_kmt_close close = {kmt_adapter};
         LONG status = close_kmt(&close);
         printf("KMT close: %08lx\n", (unsigned long)status);
         if (status < 0) ddi_error = HRESULT_FROM_NT(status);
      }
      if (umd) FreeLibrary(umd);
      if (gdi) FreeLibrary(gdi);
      fflush(stdout);
   }
};

static void *private_storage(SIZE_T bytes)
{
   require(bytes != 0, "zero private storage size");
   void *ptr = calloc(1, bytes);
   require(ptr != NULL, "private storage allocation");
   return ptr;
}

static std::vector<UINT> shader_tokens(const BYTE *dxbc, size_t bytes)
{
   /* The DDI takes the version/length token stream, not a DXBC container. */
   auto word = [&](size_t offset) {
      require(offset <= bytes && bytes - offset >= 4, "DXBC word bounds");
      UINT value; memcpy(&value, dxbc + offset, sizeof(value)); return value;
   };
   require(bytes >= 32 && !memcmp(dxbc, "DXBC", 4) && word(24) == bytes, "DXBC header");
   const UINT count = word(28);
   require(count <= (bytes - 32) / 4, "DXBC chunk table");
   for (UINT i = 0; i < count; ++i) {
      const size_t offset = word(32 + i * 4);
      require(offset <= bytes && bytes - offset >= 8, "DXBC chunk bounds");
      if (memcmp(dxbc + offset, "SHDR", 4)) continue;
      const UINT size = word(offset + 4);
      require(size >= 8 && !(size & 3) && size <= bytes - offset - 8, "shader chunk bounds");
      std::vector<UINT> tokens(size / 4);
      memcpy(tokens.data(), dxbc + offset + 8, size);
      require(tokens[1] == tokens.size(), "shader token length");
      return tokens;
   }
   throw std::runtime_error("missing SM4 shader chunk");
}

static void create_buffer(Run &r, D3D10DDI_HRESOURCE &handle, bool &live,
                          UINT bind, const void *data, UINT bytes, const char *name)
{
   D3D10DDI_MIPINFO mip = {};
   mip.TexelWidth = mip.PhysicalWidth = bytes;
   mip.TexelHeight = mip.PhysicalHeight = mip.TexelDepth = mip.PhysicalDepth = 1;
   const D3D10_DDIARG_SUBRESOURCE_UP initial = {const_cast<void *>(data), bytes, bytes};
   D3D10DDIARG_CREATERESOURCE desc = {};
   desc.pMipInfoList = &mip; desc.pInitialDataUP = &initial;
   desc.ResourceDimension = D3D10DDIRESOURCE_BUFFER;
   desc.Usage = D3D10_DDI_USAGE_IMMUTABLE; desc.BindFlags = bind;
   desc.Format = DXGI_FORMAT_UNKNOWN;
   desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
   handle.pDrvPrivate = private_storage(r.functions.pfnCalcPrivateResourceSize(r.device, &desc));
   r.functions.pfnCreateResource(r.device, &desc, handle, {});
   step(name); live = true;
}

static void draw_triangles(Run &r)
{
   const auto vs = shader_tokens(g_VS, sizeof(g_VS));
   const auto ps = shader_tokens(g_PS, sizeof(g_PS));
   D3D10DDIARG_SIGNATURE_ENTRY inputs[] = {{D3D10_SB_NAME_UNDEFINED, 0, 15},
                                          {D3D10_SB_NAME_UNDEFINED, 1, 15}};
   D3D10DDIARG_SIGNATURE_ENTRY varyings[] = {{D3D10_SB_NAME_POSITION, 0, 15},
                                            {D3D10_SB_NAME_UNDEFINED, 1, 15}};
   D3D10DDIARG_SIGNATURE_ENTRY output = {D3D10_SB_NAME_UNDEFINED, 0, 15};
   const D3D10DDIARG_STAGE_IO_SIGNATURES vs_sig = {inputs, 2, varyings, 2};
   const D3D10DDIARG_STAGE_IO_SIGNATURES ps_sig = {varyings, 2, &output, 1};
   r.vs.pDrvPrivate = private_storage(r.functions.pfnCalcPrivateShaderSize(r.device, vs.data(), &vs_sig));
   r.functions.pfnCreateVertexShader(r.device, vs.data(), r.vs, {}, &vs_sig);
   step("Create vertex shader"); r.vs_live = true;
   r.ps.pDrvPrivate = private_storage(r.functions.pfnCalcPrivateShaderSize(r.device, ps.data(), &ps_sig));
   r.functions.pfnCreatePixelShader(r.device, ps.data(), r.ps, {}, &ps_sig);
   step("Create pixel shader"); r.ps_live = true;

   const FLOAT vertices[3][8] = {
      {-0.75f, -0.75f, 0.5f, 1, 1, 0, 0, 1},
      { 0.00f,  0.75f, 0.5f, 1, 1, 0, 0, 1},
      { 0.75f, -0.75f, 0.5f, 1, 1, 0, 0, 1},
   };
   const uint16_t indices[3] = {0, 1, 2};
   create_buffer(r, r.vertices, r.vertices_live, D3D10_DDI_BIND_VERTEX_BUFFER,
                 vertices, sizeof(vertices), "Create vertex buffer");
   create_buffer(r, r.indices, r.indices_live, D3D10_DDI_BIND_INDEX_BUFFER,
                 indices, sizeof(indices), "Create index buffer");
   const D3D10DDIARG_INPUT_ELEMENT_DESC elements[] = {
      {0, 0, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D10_DDI_INPUT_PER_VERTEX_DATA, 0, 0},
      {0, 16, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D10_DDI_INPUT_PER_VERTEX_DATA, 0, 1},
   };
   const D3D10DDIARG_CREATEELEMENTLAYOUT layout = {elements, 2};
   r.layout.pDrvPrivate = private_storage(r.functions.pfnCalcPrivateElementLayoutSize(r.device, &layout));
   r.functions.pfnCreateElementLayout(r.device, &layout, r.layout, {});
   step("Create input layout"); r.layout_live = true;
   D3D10_DDI_RASTERIZER_DESC raster = {};
   raster.FillMode = D3D10_DDI_FILL_SOLID; raster.CullMode = D3D10_DDI_CULL_NONE;
   raster.DepthClipEnable = raster.ScissorEnable = TRUE;
   r.raster.pDrvPrivate = private_storage(r.functions.pfnCalcPrivateRasterizerStateSize(r.device, &raster));
   r.functions.pfnCreateRasterizerState(r.device, &raster, r.raster, {});
   step("Create rasterizer state"); r.raster_live = true;
   D3D10_DDI_BLEND_DESC blend = {};
   blend.SrcBlend = blend.SrcBlendAlpha = D3D10_DDI_BLEND_ONE;
   blend.DestBlend = blend.DestBlendAlpha = D3D10_DDI_BLEND_ZERO;
   blend.BlendOp = blend.BlendOpAlpha = D3D10_DDI_BLEND_OP_ADD;
   for (auto &mask : blend.RenderTargetWriteMask) mask = 15;
   r.blend.pDrvPrivate = private_storage(r.functions.pfnCalcPrivateBlendStateSize(r.device, &blend));
   r.functions.pfnCreateBlendState(r.device, &blend, r.blend, {});
   step("Create blend state"); r.blend_live = true;
   D3D10_DDI_DEPTH_STENCIL_DESC depth = {};
   depth.DepthFunc = D3D10_DDI_COMPARISON_ALWAYS;
   r.depth.pDrvPrivate = private_storage(r.functions.pfnCalcPrivateDepthStencilStateSize(r.device, &depth));
   r.functions.pfnCreateDepthStencilState(r.device, &depth, r.depth, {});
   step("Create depth state"); r.depth_live = true;

   r.draw_bound = true;
   const UINT stride = sizeof(vertices[0]), offset = 0;
   const FLOAT factor[4] = {1, 1, 1, 1};
   const D3D10_DDI_VIEWPORT viewport = {0, 0, 64, 64, 0, 1};
   r.functions.pfnSetRenderTargets(r.device, &r.view, 1, 0, {});
   r.functions.pfnVsSetShader(r.device, r.vs);
   r.functions.pfnPsSetShader(r.device, r.ps);
   r.functions.pfnIaSetInputLayout(r.device, r.layout);
   r.functions.pfnIaSetVertexBuffers(r.device, 0, 1, &r.vertices, &stride, &offset);
   r.functions.pfnIaSetIndexBuffer(r.device, r.indices, DXGI_FORMAT_R16_UINT, 0);
   r.functions.pfnIaSetTopology(r.device, D3D10_DDI_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
   r.functions.pfnSetRasterizerState(r.device, r.raster);
   r.functions.pfnSetBlendState(r.device, r.blend, factor, ~0u);
   r.functions.pfnSetDepthStencilState(r.device, r.depth, 0);
   r.functions.pfnSetViewports(r.device, 1, 0, &viewport);
   step("Bind triangle pipeline");

   for (unsigned pass = 0; pass < 2; ++pass) {
      const D3D10_DDI_RECT rect = pass ? D3D10_DDI_RECT{16, 16, 48, 48} : D3D10_DDI_RECT{0, 0, 64, 64};
      r.functions.pfnSetScissorRects(r.device, 1, 0, &rect);
      FLOAT blue[4] = {0, 0, 1, 1};
      r.functions.pfnClearRenderTargetView(r.device, r.view, blue);
      step("Clear triangle background");
      if (pass) r.functions.pfnDrawIndexed(r.device, 3, 0, 0);
      else r.functions.pfnDraw(r.device, 3, 0);
      step(pass ? "DrawIndexed scissored triangle" : "Draw triangle");
      r.functions.pfnResourceCopy(r.device, r.staging, r.target);
      r.functions.pfnFlush(r.device); step("Copy/flush triangle");
      D3D10DDI_MAPPED_SUBRESOURCE map = {};
      r.functions.pfnStagingResourceMap(r.device, r.staging, 0, D3D10_DDI_MAP_READ, 0, &map);
      step("Map triangle read"); require(map.pData && map.RowPitch >= 256 && !(map.RowPitch & 3), "triangle mapping");
      r.mapped = true;
      unsigned mismatches = 0, red = 0;
      for (int y = 0; y < 64; ++y) {
         const volatile uint32_t *row = (const volatile uint32_t *)((const char *)map.pData + y * map.RowPitch);
         for (int x = 0; x < 64; ++x) {
            /* Doubled screen coordinates; no pixel center lies on an edge. */
            const int px = 2*x+1, py = 2*y+1;
            const bool inside = 48*(py-112)+96*(px-16) > 0 &&
                                48*(py-16)-96*(px-64) > 0 && -96*(py-112) > 0 &&
                                x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom;
            const uint32_t expected = inside ? 0xff0000ff : 0xffff0000;
            red += inside;
            const uint32_t observed = row[x];
            if (observed != expected && mismatches++ < 8)
               printf("Triangle %u mismatch (%d,%d): %08x expected %08x\n", pass, x, y, observed, expected);
         }
      }
      printf("Triangle %u: red=%u blue=%u mismatches=%u pitch=%u\n", pass, red, 4096-red, mismatches, map.RowPitch);
      fflush(stdout);
      r.functions.pfnStagingResourceUnmap(r.device, r.staging, 0); r.mapped = false;
      step("Unmap triangle read"); require(!mismatches && red, "triangle pixel mismatch");
   }
}

static void run(Run &r, const char *umd_path, uint64_t luid)
{
   r.gdi = LoadLibraryExW(L"gdi32.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
   require(r.gdi != NULL, "load system gdi32");
   auto enumerate = reinterpret_cast<LONG (WINAPI *)(tg_kmt_enum *)>(GetProcAddress(r.gdi, "D3DKMTEnumAdapters3"));
   query_kmt = reinterpret_cast<decltype(query_kmt)>(GetProcAddress(r.gdi, "D3DKMTQueryAdapterInfo"));
   close_kmt = reinterpret_cast<decltype(close_kmt)>(GetProcAddress(r.gdi, "D3DKMTCloseAdapter"));
   require(enumerate && query_kmt && close_kmt, "KMT entry points");
   tg_kmt_enum list = {TG_KMT_INCLUDE_COMPUTE_ONLY, 0, NULL};
   require(enumerate(&list) >= 0 && list.count, "enumerate adapter count");
   std::vector<tg_kmt_adapter> devices(list.count);
   list.adapters = devices.data();
   require(enumerate(&list) >= 0, "enumerate adapters");
   require(list.count <= devices.size(), "adapter count grew");
   for (uint32_t i = 0; i < list.count; ++i) {
      const auto &d = devices[i];
      const uint64_t found = (uint64_t)(uint32_t)d.luid.high << 32 | d.luid.low;
      if (found == luid && !r.kmt_adapter) {
         r.kmt_adapter = d.handle;
      } else {
         const tg_kmt_close close = {d.handle};
         require(close_kmt(&close) >= 0, "close unselected adapter");
      }
   }
   require(r.kmt_adapter != 0, "requested LUID absent");
   printf("GM20B requested LUID: %016llx\n", (unsigned long long)luid);
   r.umd = LoadLibraryA(umd_path);
   require(r.umd != NULL, "load NVK UMD");
   auto open = reinterpret_cast<PFND3D10DDI_OPENADAPTER>(GetProcAddress(r.umd, "OpenAdapter10_2"));
   require(open != NULL && !GetProcAddress(r.umd, "D3DKMTCreateDevice"), "hardware UMD exports");
   D3DDDI_ADAPTERCALLBACKS callbacks = {};
   callbacks.pfnQueryAdapterInfoCb = query_adapter;
   r.adapter.hRTAdapter.handle = (HANDLE)(uintptr_t)r.kmt_adapter;
   r.adapter.pAdapterCallbacks = &callbacks;
   r.adapter.pAdapterFuncs_2 = &r.adapters;
   ddi_error = open(&r.adapter);
   step("OpenAdapter10_2");
   UINT count = 0;
   require(SUCCEEDED(r.adapters.pfnGetSupportedVersions(r.adapter.hAdapter, &count, NULL)) && count,
           "query DDI versions");
   std::vector<UINT64> versions(count);
   require(SUCCEEDED(r.adapters.pfnGetSupportedVersions(r.adapter.hAdapter, &count, versions.data())),
           "read DDI versions");
   bool supported = false;
   for (UINT i = 0; i < count; ++i) supported |= versions[i] == D3D10_0_DDI_SUPPORTED;
   require(supported, "D3D10 DDI support");

   D3D10DDIARG_CALCPRIVATEDEVICESIZE size = {};
   size.Interface = D3D10_0_DDI_INTERFACE_VERSION;
   size.Version = (UINT)D3D10_0_DDI_SUPPORTED;
   r.device.pDrvPrivate = private_storage(r.adapters.pfnCalcPrivateDeviceSize(r.adapter.hAdapter, &size));
   D3DDDI_DEVICECALLBACKS kernel_callbacks = {};
   D3D10DDI_CORELAYER_DEVICECALLBACKS runtime_callbacks = {};
   runtime_callbacks.pfnSetErrorCb = set_error;
   DXGI_DDI_BASE_FUNCTIONS dxgi = {};
   D3D10DDIARG_CREATEDEVICE create = {};
   create.Interface = size.Interface; create.Version = size.Version;
   create.hDrvDevice = r.device; create.pDeviceFuncs = &r.functions;
   create.pKTCallbacks = &kernel_callbacks; create.pUMCallbacks = &runtime_callbacks;
   create.DXGIBaseDDI.pDXGIDDIBaseFunctions = &dxgi;
   ddi_error = r.adapters.pfnCreateDevice(r.adapter.hAdapter, &create);
   step("CreateDevice"); r.device_live = true;

   const UINT width = 64, height = 64;
   D3D10DDI_MIPINFO mip = {};
   mip.TexelWidth = mip.PhysicalWidth = width;
   mip.TexelHeight = mip.PhysicalHeight = height;
   mip.TexelDepth = mip.PhysicalDepth = 1;
   D3D10DDIARG_CREATERESOURCE desc = {};
   desc.pMipInfoList = &mip; desc.ResourceDimension = D3D10DDIRESOURCE_TEXTURE2D;
   desc.Usage = D3D10_DDI_USAGE_DEFAULT; desc.BindFlags = D3D10_DDI_BIND_RENDER_TARGET;
   desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
   desc.SampleDesc.Count = desc.MipLevels = desc.ArraySize = 1;
   r.target.pDrvPrivate = private_storage(r.functions.pfnCalcPrivateResourceSize(r.device, &desc));
   r.functions.pfnCreateResource(r.device, &desc, r.target, {});
   step("Create render target"); r.target_live = true;
   desc.Usage = D3D10_DDI_USAGE_STAGING; desc.BindFlags = 0;
   desc.MapFlags = D3D10_DDI_CPU_ACCESS_READ | D3D10_DDI_CPU_ACCESS_WRITE;
   r.staging.pDrvPrivate = private_storage(r.functions.pfnCalcPrivateResourceSize(r.device, &desc));
   r.functions.pfnCreateResource(r.device, &desc, r.staging, {});
   step("Create staging texture"); r.staging_live = true;
   D3D10DDIARG_CREATERENDERTARGETVIEW view = {};
   view.hDrvResource = r.target; view.ResourceDimension = desc.ResourceDimension; view.Format = desc.Format;
   view.Tex2D.ArraySize = 1;
   r.view.pDrvPrivate = private_storage(r.functions.pfnCalcPrivateRenderTargetViewSize(r.device, &view));
   r.functions.pfnCreateRenderTargetView(r.device, &view, r.view, {});
   step("Create render target view"); r.view_live = true;

   for (unsigned pass = 0; pass < 3; ++pass) {
      D3D10DDI_MAPPED_SUBRESOURCE map = {};
      r.functions.pfnStagingResourceMap(r.device, r.staging, 0, D3D10_DDI_MAP_WRITE, 0, &map);
      step("Map staging write"); require(map.pData && map.RowPitch >= width * 4 && !(map.RowPitch & 3), "write mapping");
      r.mapped = true;
      for (UINT y = 0; y < height; ++y) {
         volatile uint32_t *row = (volatile uint32_t *)((char *)map.pData + y * map.RowPitch);
         for (UINT x = 0; x < width; ++x) row[x] = 0x5a5a5a5a;
      }
      r.functions.pfnStagingResourceUnmap(r.device, r.staging, 0); r.mapped = false;
      step("Unmap staging write");
      FLOAT color[4] = {0, 0, 0, 1}; color[pass] = 1;
      r.functions.pfnClearRenderTargetView(r.device, r.view, color); step("ClearRenderTargetView");
      r.functions.pfnResourceCopy(r.device, r.staging, r.target); step("ResourceCopy");
      r.functions.pfnFlush(r.device); step("Flush");
      map = {};
      r.functions.pfnStagingResourceMap(r.device, r.staging, 0, D3D10_DDI_MAP_READ, 0, &map);
      step("Map staging read"); require(map.pData && map.RowPitch >= width * 4 && !(map.RowPitch & 3), "read mapping");
      r.mapped = true;
      const uint32_t expected = 0xff000000 | (0xffu << (pass * 8));
      unsigned mismatches = 0;
      for (UINT y = 0; y < height; ++y) {
         const volatile uint32_t *row = (const volatile uint32_t *)((char *)map.pData + y * map.RowPitch);
         for (UINT x = 0; x < width; ++x) mismatches += row[x] != expected;
      }
      printf("Pass %u: expected=%08x words=%u mismatches=%u pitch=%u\n",
             pass, expected, width * height, mismatches, map.RowPitch); fflush(stdout);
      r.functions.pfnStagingResourceUnmap(r.device, r.staging, 0); r.mapped = false;
      step("Unmap staging read"); require(!mismatches, "pixel mismatch");
   }
   draw_triangles(r);
}

int main(int argc, char **argv)
{
   if (argc != 4) {
      fprintf(stderr, "Usage: %s <absolute-UMD.dll> <absolute-NVK-ICD.json> <16-digit-LUID>\n", argv[0]);
      return 2;
   }
   char *end = NULL;
   uint64_t luid = strtoull(argv[3], &end, 16);
   if (strlen(argv[3]) != 16 || !end || *end || !luid) return 2;
   SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
   SetUnhandledExceptionFilter(unhandled_exception);
   if (!SetEnvironmentVariableA("VK_DRIVER_FILES", argv[2])) return 2;
   try {
      Run state;
      run(state, argv[1], luid);
   } catch (const std::exception &error) {
      fprintf(stderr, "FAIL: %s\n", error.what());
      return 1;
   }
   if (FAILED(ddi_error)) return 1;
   puts("Native DDI clear/draw/indexed-scissor/copy/readback passed; Windows D3D runtime and presentation were not exercised");
   return 0;
}
