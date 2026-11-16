/* SPDX-License-Identifier: MIT
 * Real typed table and object constructors, with CPU-backed upload storage.
 * No Vulkan device, runtime device or physical display is created.
 */
#include "tgd3d.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstddef>

using namespace tgd3d;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%u: %s\n", __FILE__, __LINE__, #x); std::exit(1); } } while (0)
static HRESULT error;
static unsigned errors;

template<typename T> struct Storage {
   alignas(T) unsigned char bytes[sizeof(T)];
   T *object() { return reinterpret_cast<T *>(bytes); }
};

void ddi11_contracts()
{
   Device dev;
   dev.um.pfnSetErrorCb = [](D3D10DDI_HRTCORELAYER, HRESULT hr) { error = hr; ++errors; };
   D3D10DDI_HDEVICE h = {&dev};
   struct { uint64_t before; D3D11_1DDI_DEVICEFUNCS table; uint64_t after; } guarded = {};
   guarded.before = guarded.after = UINT64_C(0x9876543210abcdef);
   device_fill_table11(&guarded.table);
   auto &t = guarded.table;
   CHECK(guarded.before == UINT64_C(0x9876543210abcdef) && guarded.before == guarded.after);
   CHECK(t.pfnDraw && t.pfnCreateResource && t.pfnCreateShaderResourceView &&
         t.pfnVsSetConstantBuffers && t.pfnCheckDirectFlipSupport);
   /* The incomplete table stays unadvertised; these are explicit remaining
    * work, not successful stub callbacks for operations we do not implement. */
   CHECK(t.pfnClearView && t.pfnDiscard && t.pfnDynamicConstantBufferMapNoOverwrite &&
         t.pfnDrawInstancedIndirect && t.pfnDrawIndexedInstancedIndirect);

   struct { uint64_t before; D3DWDDM1_3DDI_DEVICEFUNCS table; uint64_t after; } guarded13 = {};
   guarded13.before = guarded13.after = UINT64_C(0x9876543210abcdef);
   device_fill_table13(&guarded13.table);
   CHECK(guarded13.before == UINT64_C(0x9876543210abcdef) && guarded13.before == guarded13.after);
   auto &t13 = guarded13.table;
   CHECK(t13.pfnDraw == t.pfnDraw && t13.pfnClearView == t.pfnClearView &&
         t13.pfnCreateResource == t.pfnCreateResource && !t13.pfnUpdateTileMappings);
   UINT levels = 999;
   t13.pfnCheckMultisampleQualityLevels(h, DXGI_FORMAT_B8G8R8A8_UNORM, 1, 0, &levels);
   CHECK(levels == 1);
   t13.pfnCheckMultisampleQualityLevels(h, DXGI_FORMAT_B8G8R8A8_UNORM, 1,
      D3DWDDM1_3DDI_CHECK_MULTISAMPLE_QUALITY_LEVELS_TILED_RESOURCE, &levels);
   CHECK(!levels);
   levels = 999;
   t13.pfnCheckMultisampleQualityLevels(h, DXGI_FORMAT_B8G8R8A8_UNORM, 4, 0, &levels);
   CHECK(!levels);
   t13.pfnRelocateDeviceFuncs(h, &t13);

   struct { uint64_t before; DXGI1_3_DDI_BASE_FUNCTIONS table; uint64_t after; } dxgi = {};
   dxgi.before = dxgi.after = UINT64_C(0x9876543210abcdef);
   dxgi_fill_presentation13(&dxgi.table);
   CHECK(dxgi.before == UINT64_C(0x9876543210abcdef) && dxgi.before == dxgi.after);
   CHECK(dxgi.table.pfnPresent && dxgi.table.pfnGetGammaCaps && dxgi.table.pfnSetDisplayMode &&
         dxgi.table.pfnSetResourcePriority && dxgi.table.pfnQueryResourceResidency &&
         dxgi.table.pfnRotateResourceIdentities && dxgi.table.pfnBlt && dxgi.table.pfnResolveSharedResource &&
         dxgi.table.pfnOfferResources && dxgi.table.pfnReclaimResources && dxgi.table.pfnBlt1 &&
         dxgi.table.pfnPresent1 && dxgi.table.pfnPresentMultiplaneOverlay &&
         !dxgi.table.pfnReserved1 && !dxgi.table.pfnReserved2);

   SYSTEM_INFO system; GetSystemInfo(&system);
   auto page = static_cast<unsigned char *>(VirtualAlloc(nullptr, system.dwPageSize * 2,
      MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
   CHECK(page);
   DWORD old = 0;
   CHECK(VirtualProtect(page + system.dwPageSize, system.dwPageSize, PAGE_NOACCESS, &old));
   const size_t callbacksBytes = offsetof(D3DDDI_DEVICECALLBACKS, pfnReclaimAllocations2Cb) +
      sizeof(PFND3DDDI_RECLAIMALLOCATIONS2CB);
   CHECK(callbacksBytes < sizeof(D3DDDI_DEVICECALLBACKS) && callbacksBytes < system.dwPageSize);
   auto producer = reinterpret_cast<D3DDDI_DEVICECALLBACKS *>(page + system.dwPageSize - callbacksBytes);
   std::memset(producer, 0, callbacksBytes);
   producer->pfnAllocateCb = [](HANDLE, D3DDDICB_ALLOCATE *) -> HRESULT { return S_OK; };
   producer->pfnCreateContextVirtualCb = [](HANDLE, D3DDDICB_CREATECONTEXTVIRTUAL *) -> HRESULT { return S_OK; };
   producer->pfnSignalSynchronizationObjectFromGpuCb = [](HANDLE,
      const D3DDDICB_SIGNALSYNCHRONIZATIONOBJECTFROMGPU *) -> HRESULT { return S_OK; };
   D3DDDI_DEVICECALLBACKS consumer;
   std::memset(&consumer, 0xcc, sizeof(consumer));
   runtime_copy_callbacks(&consumer, producer);
   CHECK(consumer.pfnAllocateCb == producer->pfnAllocateCb &&
         consumer.pfnCreateContextVirtualCb == producer->pfnCreateContextVirtualCb &&
         consumer.pfnSignalSynchronizationObjectFromGpuCb == producer->pfnSignalSynchronizationObjectFromGpuCb);
   CHECK(!consumer.pfnMakeResidentCb && !consumer.pfnLock2Cb && !consumer.pfnRenderCb);
   const auto bytes = reinterpret_cast<const unsigned char *>(&consumer);
   for (size_t i = callbacksBytes; i < sizeof(consumer); ++i) CHECK(!bytes[i]);
   std::puts("PASS: runtime callback import reads only used WDDM2 fields before a guard page, leaves newer/unused entries zero (CPU control)");
   const size_t prefix = offsetof(D3D11DDIARG_CREATERESOURCE, TextureLayout);
   auto desc = reinterpret_cast<D3D11DDIARG_CREATERESOURCE *>(page + system.dwPageSize - prefix);
   std::memset(desc, 0, prefix);
   D3D10DDI_MIPINFO mip = {512, 1, 1, 512, 1, 1};
   unsigned char input[512], backing[2048];
   for (unsigned i = 0; i < sizeof(input); ++i) input[i] = (i * 17 + 9) & 255;
   D3D10_DDIARG_SUBRESOURCE_UP initial = {input, sizeof(input), sizeof(input)};
   Chunk chunk; chunk.cpu = backing; chunk.size = sizeof(backing);
   chunk.buffer = (VkBuffer)(uintptr_t)0x200;
   dev.currentChunk = &chunk;
   desc->pMipInfoList = &mip; desc->pInitialDataUP = &initial;
   desc->ResourceDimension = D3D10DDIRESOURCE_BUFFER;
   desc->BindFlags = D3D10_DDI_BIND_CONSTANT_BUFFER;
   desc->SampleDesc.Count = 1; desc->MipLevels = desc->ArraySize = 1;
   Storage<Resource> resource;
   CHECK(t.pfnCalcPrivateResourceSize(h, desc) == sizeof(Resource));
   errors = 0;
   t.pfnCreateResource(h, desc, {resource.object()}, {});
   CHECK(!errors && chunk.live == 1 && resource.object()->size == 512 &&
         !std::memcmp(backing, input, sizeof(input)));
   const D3D10DDI_HRESOURCE bound = {resource.object()};
   UINT first = 16, count = 16;
   for (auto setter : {t.pfnVsSetConstantBuffers, t.pfnGsSetConstantBuffers, t.pfnPsSetConstantBuffers})
      setter(h, 2, 1, &bound, &first, &count);
   for (auto &stage : dev.stage)
      CHECK(stage.cb[2] == resource.object() && stage.cbOffset[2] == 256 && stage.cbBytes[2] == 256);
   /* Same object with a different window must dirty the immutable descriptor
    * snapshot. A legacy bind must then reset both offset and window length. */
   dev.stage[STAGE_VS].cbDirty = 0; first = 32;
   t.pfnVsSetConstantBuffers(h, 2, 1, &bound, &first, &count);
   CHECK(dev.stage[STAGE_VS].cbDirty == 4 && dev.stage[STAGE_VS].cbOffset[2] == 512);
   count = 0;
   t.pfnVsSetConstantBuffers(h, 2, 1, &bound, &first, &count);
   CHECK(!dev.stage[STAGE_VS].cbBytes[2]);
   D3D10DDI_DEVICEFUNCS legacy = {}; state_fill_table(&legacy);
   legacy.pfnVsSetConstantBuffers(h, 2, 1, &bound);
   CHECK(dev.stage[STAGE_VS].cbOffset[2] == 0 && dev.stage[STAGE_VS].cbBytes[2] == 65536);
   first = 1; count = 16; errors = 0;
   t.pfnVsSetConstantBuffers(h, 2, 1, &bound, &first, &count);
   CHECK(errors == 1 && error == E_INVALIDARG && dev.stage[STAGE_VS].cbOffset[2] == 0);
   t.pfnDestroyResource(h, bound);
   CHECK(!chunk.live);
   for (const auto &stage : dev.stage) CHECK(!stage.cb[2]);
   desc->ByteStride = 4; errors = 0;
   t.pfnCreateResource(h, desc, {resource.object()}, {});
   CHECK(errors == 1 && error == DXGI_ERROR_UNSUPPORTED && !chunk.live);
   CHECK(VirtualFree(page, 0, MEM_RELEASE));

   Resource image; image.dev = &dev; image.kind = ResKind::Image;
   image.width = 64; image.height = 32; image.mips = 3; image.arraySize = 4;
   image.format = DXGI_FORMAT_R8G8B8A8_UNORM;
   image.bind = D3D10_DDI_BIND_SHADER_RESOURCE;
   D3D11DDIARG_CREATESHADERRESOURCEVIEW srv = {};
   srv.hDrvResource = {&image}; srv.Format = image.format;
   srv.ResourceDimension = D3D10DDIRESOURCE_TEXTURE2D;
   srv.Tex2D.MostDetailedMip = 1; srv.Tex2D.MipLevels = 2;
   srv.Tex2D.FirstArraySlice = 1; srv.Tex2D.ArraySize = 2;
   Storage<View> view; errors = 0;
   t.pfnCreateShaderResourceView(h, &srv, {view.object()}, {});
   CHECK(!errors && view.object()->info.subresourceRange.baseMipLevel == 1 &&
         view.object()->info.subresourceRange.levelCount == 2 &&
         view.object()->info.subresourceRange.baseArrayLayer == 1 &&
         view.object()->info.subresourceRange.layerCount == 2);
   t.pfnDestroyShaderResourceView(h, {view.object()});
   CHECK(!image.views && dev.views.empty());
   image.cube = true; image.arraySize = 6; image.height = 64;
   srv.ResourceDimension = D3D10DDIRESOURCE_TEXTURECUBE;
   srv.TexCube.MostDetailedMip = 0; srv.TexCube.MipLevels = 3;
   srv.TexCube.First2DArrayFace = 0; srv.TexCube.NumCubes = 1;
   errors = 0; t.pfnCreateShaderResourceView(h, &srv, {view.object()}, {});
   CHECK(!errors && view.object()->info.viewType == VK_IMAGE_VIEW_TYPE_CUBE && view.object()->layers == 6);
   t.pfnDestroyShaderResourceView(h, {view.object()});
   srv.TexCube.First2DArrayFace = 6; errors = 0;
   t.pfnCreateShaderResourceView(h, &srv, {view.object()}, {});
   CHECK(errors == 1 && !image.views);

   Resource depth; depth.dev = &dev; depth.kind = ResKind::Image;
   depth.width = 64; depth.height = 32; depth.mips = 3; depth.arraySize = 4;
   depth.format = DXGI_FORMAT_R24G8_TYPELESS; depth.bind = D3D10_DDI_BIND_DEPTH_STENCIL;
   depth.aspects = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
   D3D11DDIARG_CREATEDEPTHSTENCILVIEW dsv = {};
   dsv.hDrvResource = {&depth}; dsv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
   dsv.ResourceDimension = D3D10DDIRESOURCE_TEXTURE2D;
   dsv.Tex2D.MipSlice = 1; dsv.Tex2D.FirstArraySlice = 1; dsv.Tex2D.ArraySize = 2;
   for (UINT flags = 0; flags < 4; ++flags) {
      dsv.Flags = flags; errors = 0;
      t.pfnCreateDepthStencilView(h, &dsv, {view.object()}, {});
      CHECK(!errors && view.object()->dsvFlags == flags && view.object()->width == 32 &&
            view.object()->info.subresourceRange.baseArrayLayer == 1 && view.object()->layers == 2);
      t.pfnDestroyDepthStencilView(h, {view.object()});
      CHECK(!depth.views && dev.views.empty());
   }
   for (UINT invalid = 0; invalid < 4; ++invalid) {
      dsv.Flags = 0; dsv.Tex2D.MipSlice = 1; dsv.Tex2D.FirstArraySlice = 1; dsv.Tex2D.ArraySize = 2;
      switch (invalid) {
      case 0: dsv.Flags = 4; break;
      case 1: dsv.Tex2D.MipSlice = 3; break;
      case 2: dsv.Tex2D.FirstArraySlice = 3; break;
      case 3: dsv.Tex2D.ArraySize = 0; break;
      }
      errors = 0; t.pfnCreateDepthStencilView(h, &dsv, {view.object()}, {});
      CHECK(errors == 1 && error == E_INVALIDARG && !depth.views && dev.views.empty());
   }

   D3D11_1_DDI_RASTERIZER_DESC raster = {};
   Storage<RasterizerState> rasterState;
   for (UINT samples : {0u, 1u, 2u}) {
      raster.ForcedSampleCount = samples; errors = 0;
      t.pfnCreateRasterizerState(h, &raster, {rasterState.object()}, {});
      if (samples < 2) { CHECK(!errors); t.pfnDestroyRasterizerState(h, {rasterState.object()}); }
      else CHECK(errors == 1 && error == DXGI_ERROR_UNSUPPORTED);
   }

   D3D11_1_DDI_BLEND_DESC blend = {};
   blend.RenderTarget[0].BlendEnable = TRUE;
   blend.RenderTarget[0].SrcBlend = D3D10_DDI_BLEND_SRC_ALPHA;
   blend.RenderTarget[0].DestBlend = D3D10_DDI_BLEND_INV_SRC_ALPHA;
   blend.RenderTarget[0].BlendOp = D3D10_DDI_BLEND_OP_ADD;
   blend.RenderTarget[0].SrcBlendAlpha = D3D10_DDI_BLEND_ONE;
   blend.RenderTarget[0].DestBlendAlpha = D3D10_DDI_BLEND_ZERO;
   blend.RenderTarget[0].BlendOpAlpha = D3D10_DDI_BLEND_OP_ADD;
   blend.RenderTarget[0].RenderTargetWriteMask = 15;
   Storage<BlendState> state; errors = 0;
   t.pfnCreateBlendState(h, &blend, {state.object()}, {});
   CHECK(!errors && state.object()->desc.SrcBlend == D3D10_DDI_BLEND_SRC_ALPHA);
   for (unsigned i = 0; i < 8; ++i)
      CHECK(state.object()->desc.BlendEnable[i] && state.object()->desc.RenderTargetWriteMask[i] == 15);
   t.pfnDestroyBlendState(h, {state.object()});
   blend.RenderTarget[0].LogicOpEnable = TRUE; errors = 0;
   t.pfnCreateBlendState(h, &blend, {state.object()}, {});
   CHECK(errors == 1 && error == DXGI_ERROR_UNSUPPORTED);

   BOOL direct = TRUE;
   t.pfnCheckDirectFlipSupport(h, {}, {}, 0, &direct);
   CHECK(!direct);
   std::puts("PASS: typed modern-table bridge, guarded descriptor revision, SRV unions, blend state and CB windows (CPU only; table not advertised)");
}
