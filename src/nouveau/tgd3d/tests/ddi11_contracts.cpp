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

static void adapter_caps_contracts()
{
   Adapter adapter;
   D3D10DDI_HADAPTER h = {&adapter};
   D3D10_2DDI_ADAPTERFUNCS functions = {};
   adapter_fill_queries(&functions);
   UINT count = 0;
   CHECK(functions.pfnGetSupportedVersions(h, nullptr, nullptr) == E_INVALIDARG);
   for (bool withVideo : {false, true}) {
      adapter.videoEnabled = withVideo;
      CHECK(functions.pfnGetSupportedVersions(h, &count, nullptr) == S_OK && count == 4);
      UINT64 versions[5] = {0xfeed, 0, 0, 0, 0xbeef}; count = 3;
      CHECK(functions.pfnGetSupportedVersions(h, &count, versions) == E_OUTOFMEMORY && count == 4);
      CHECK(versions[0] == 0xfeed && versions[4] == 0xbeef);
      CHECK(functions.pfnGetSupportedVersions(h, &count, versions) == S_OK);
      CHECK(versions[0] == D3D10_0_DDI_SUPPORTED && versions[1] == D3D10_0_x_DDI_SUPPORTED &&
            versions[2] == D3D10_0_7_DDI_SUPPORTED && versions[3] == D3D11_1_DDI_SUPPORTED &&
            versions[4] == 0xbeef);
   }
   /* The offer is process-scoped and exact: neither another value nor a
    * non-NVK adapter may enable the unselected modern presentation path. */
   CHECK(SetEnvironmentVariableW(L"TGD3D_WDDM20_APPLICATION", L"1"));
   count = 0;
   CHECK(functions.pfnGetSupportedVersions(h, &count, nullptr) == S_OK && count == 4);
   adapter.vk.nvk = true;
   for (const wchar_t *value : {L"0", L"10", L"", L"true"}) {
      CHECK(SetEnvironmentVariableW(L"TGD3D_WDDM20_APPLICATION", value));
      count = 0;
      CHECK(functions.pfnGetSupportedVersions(h, &count, nullptr) == S_OK && count == 4);
   }
   CHECK(SetEnvironmentVariableW(L"TGD3D_WDDM20_APPLICATION", L"1"));
   UINT64 modern[6] = {0xfeed, 0, 0, 0, 0, 0xbeef}; count = 4;
   CHECK(functions.pfnGetSupportedVersions(h, &count, modern) == E_OUTOFMEMORY && count == 5);
   CHECK(modern[0] == 0xfeed && modern[5] == 0xbeef);
   CHECK(functions.pfnGetSupportedVersions(h, &count, modern) == S_OK && count == 5 &&
         modern[3] == D3D11_1_DDI_SUPPORTED && modern[4] == D3DWDDM2_0_DDI_SUPPORTED && modern[5] == 0xbeef);
   CHECK(SetEnvironmentVariableW(L"TGD3D_WDDM20_APPLICATION", nullptr));
   count = 0;
   CHECK(functions.pfnGetSupportedVersions(h, &count, nullptr) == S_OK && count == 4);
   adapter.vk.nvk = false;
   adapter.videoEnabled = false;
   struct { UINT before, caps, after; } out = {0xdeadbeef, 0xffffffff, 0xbeefdead};
   D3D10_2DDIARG_GETCAPS query = {D3D11DDICAPS_3DPIPELINESUPPORT, nullptr, &out.caps, sizeof(out.caps)};
   CHECK(functions.pfnGetCaps(h, &query) == S_OK && out.caps == 1);
   CHECK(out.before == 0xdeadbeef && out.after == 0xbeefdead);
   // A size accepted here must correspond to a level in the adapter's mask.
   Adapter high;
   high.vk.nvk = true;
   auto &c = high.vk.caps;
   c.compute = c.geometryShader = c.tessellationShader = c.transformFeedback = true;
   c.pipelineStatistics = c.drawIndirectFirstInstance = c.textureCompressionBC = c.logicOp = true;
   c.maxColorAttachments = 8; c.maxViewports = 16;
   c.maxImageDimension2D = 16384; c.maxImageDimension3D = c.maxImageArrayLayers = 2048;
   c.graphicsUavStages = VK_SHADER_STAGE_ALL_GRAPHICS;
   c.graphicsUavSlots = 64;
   c.tirSamples = VK_SAMPLE_COUNT_16_BIT;
   auto levels = [&](UINT expected) {
      CHECK(functions.pfnGetCaps({&high}, &query) == S_OK && out.caps == expected);
      for (UINT level = 0; level <= 6; ++level) {
         D3D10DDIARG_CALCPRIVATEDEVICESIZE size{D3D11_1_DDI_INTERFACE_VERSION,
            D3D11_1_DDI_BUILD_VERSION << 16, level << D3D11DDI_CREATEDEVICE_FLAG_3DPIPELINESUPPORT_SHIFT};
         CHECK(!!CalcPrivateDeviceSize({&high}, &size) == !!(expected & (1u << level)));
      }
   };
   levels(15);
   c.graphicsUavSlots = 63; levels(7); c.graphicsUavSlots = 64;
   c.tirSamples = 0; levels(7); c.attachmentlessSamples = VK_SAMPLE_COUNT_16_BIT;
   c.logicOp = false; levels(7); c.logicOp = true;
   c.graphicsUavStages &= ~VK_SHADER_STAGE_GEOMETRY_BIT; levels(7);
   c.tessellationShader = false; levels(1);
   D3D11_1DDI_D3D11_OPTIONS_DATA options = {};
   D3D10_2DDIARG_GETCAPS optionsQuery = {D3D11_1DDICAPS_D3D11_OPTIONS, nullptr, &options, sizeof(options)};
   for (bool supported : {false, true}) {
      adapter.vk.caps.logicOp = supported;
      CHECK(functions.pfnGetCaps(h, &optionsQuery) == S_OK);
      CHECK(!!options.OutputMergerLogicOp == supported && !options.AssignDebugBinarySupport);
   }
   for (auto type : {D3D11DDICAPS_THREADING, D3D11DDICAPS_SHADER, D3DWDDM1_3DDICAPS_D3D11_OPTIONS1,
                    D3DWDDM1_3DDICAPS_MARKER, D3DWDDM2_0DDICAPS_D3D11_OPTIONS2,
                    D3DWDDM2_0DDICAPS_D3D11_OPTIONS3, D3DWDDM2_2DDICAPS_SHADERCACHE}) {
      query.Type = type; out.caps = 0xffffffff;
      CHECK(functions.pfnGetCaps(h, &query) == S_OK && !out.caps);
   }
   D3D11DDI_SHADER_CAPS compute{};
   D3D10_2DDIARG_GETCAPS computeQuery{D3D11DDICAPS_SHADER, nullptr, &compute, sizeof(compute)};
   adapter.vk.caps.downlevelCompute = true;
   CHECK(functions.pfnGetCaps(h, &computeQuery) == S_OK &&
         compute.Caps == D3D11DDICAPS_SHADER_COMPUTE_PLUS_RAW_AND_STRUCTURED_BUFFERS_IN_SHADER_4_X);
   query.Type = D3DWDDM2_0DDICAPS_GPUVA_CAPS;
   CHECK(functions.pfnGetCaps(h, &query) == S_OK && out.caps == 40);
   query.pInfo = &out; // unused input for this query must not be interpreted as layout metadata
   CHECK(functions.pfnGetCaps(h, &query) == S_OK && out.caps == 40);
   D3DWDDM2_0DDI_MEMORY_ARCHITECTURE_CAPS memory = {};
   query = {D3DWDDM2_0DDICAPS_MEMORY_ARCHITECTURE, nullptr, &memory, sizeof(memory)};
   CHECK(functions.pfnGetCaps(h, &query) == S_OK && memory.UMA && !memory.CacheCoherent);
   D3DWDDM2_0DDI_TEXTURE_LAYOUT_CAPS layout = {7,7,TRUE};
   query = {D3DWDDM2_0DDICAPS_TEXTURE_LAYOUT, nullptr, &layout, sizeof(layout)};
   CHECK(functions.pfnGetCaps(h, &query) == S_OK && !layout.DeviceDependentLayoutCount &&
         !layout.DeviceDependentSwizzleCount && !layout.Supports64KStandardSwizzle);
   query.DataSize = sizeof(layout)-1;
   CHECK(functions.pfnGetCaps(h, &query) == E_INVALIDARG);
   query.DataSize = sizeof(layout); query.pInfo = &out;
   CHECK(functions.pfnGetCaps(h, &query) == E_INVALIDARG);
   query.Type = static_cast<D3D10_2DDICAPS_TYPE>(0x7fff);
   CHECK(functions.pfnGetCaps(h, &query) == E_NOTIMPL);
   CHECK(functions.pfnGetCaps(h, nullptr) == E_INVALIDARG);
   std::puts("PASS: typed adapter queries report only the existing D3D10 pipeline, no optional compute/threading/layout promises; exact output bounds and UMA/coherence distinction (CPU control)");
}

template<typename T> struct Storage {
   alignas(T) unsigned char bytes[sizeof(T)];
   T *object() { return reinterpret_cast<T *>(bytes); }
};

static void ddi20_contracts()
{
   Device dev;
   dev.um.pfnSetErrorCb = [](D3D10DDI_HRTCORELAYER, HRESULT hr) { error = hr; ++errors; };
   D3D10DDI_HDEVICE h = {&dev};
   struct { uint64_t before; D3DWDDM2_0DDI_DEVICEFUNCS table; uint64_t after; } guarded = {};
   guarded.before = guarded.after = UINT64_C(0x87654321abcdef09);
   device_fill_table20(&guarded.table);
   auto &t = guarded.table;
   CHECK(guarded.before == guarded.after && guarded.before == UINT64_C(0x87654321abcdef09));
   CHECK(t.pfnDraw && t.pfnCreateResource && t.pfnCreateShaderResourceView &&
         t.pfnCreateRenderTargetView && t.pfnCreateQuery && t.pfnFlush && t.pfnRelocateDeviceFuncs);
   CHECK(!t.pfnSetHardwareProtection && !t.pfnGetResourceLayout && t.pfnDispatch && t.pfnDispatchIndirect && t.pfnCreateComputeShader && t.pfnCsSetUnorderedAccessViews);
   t.pfnRelocateDeviceFuncs(h, &t);

   struct { uint64_t before; DXGI1_4_DDI_BASE_FUNCTIONS table; uint64_t after; } dxgi = {};
   dxgi.before = dxgi.after = UINT64_C(0x87654321abcdef09);
   dxgi_fill_presentation14(&dxgi.table);
   CHECK(dxgi.before == dxgi.after && dxgi.before == UINT64_C(0x87654321abcdef09));
   CHECK(dxgi.table.pfnPresent && dxgi.table.pfnPresent1 && dxgi.table.pfnPresentMultiplaneOverlay &&
         dxgi.table.pfnPresentMultiplaneOverlay1 && dxgi.table.pfnCheckMultiplaneOverlayColorSpaceSupport &&
         dxgi.table.pfnOfferResources && dxgi.table.pfnReclaimResources);
   CHECK(!dxgi.table.pfnReserved1 && !dxgi.table.pfnReserved2 && dxgi.table.pfnTrimResidencySet);
   // Exact slot read by the matched native D3D11 runtime. No shifted 1.3 table.
   static_assert(offsetof(DXGI1_4_DDI_BASE_FUNCTIONS, pfnPresentMultiplaneOverlay1) == 20 * sizeof(void *));
   static_assert(offsetof(DXGIDDI_MULTIPLANE_OVERLAY_PLANE_INFO, DriverPrivateDataSize) == 24);
   static_assert(offsetof(DXGIDDI_MULTIPLANE_OVERLAY_PLANE_INFO, pDriverPrivateData) == 32);
   static_assert(offsetof(DXGIDDI_MULTIPLANE_OVERLAY_PLANE_INFO, SyncIntervalOverrideValid) == 40);

   // Test actual constructors, including array/mip mapping, rather than only
   // pointer presence. Vulkan views are lazy; no physical device is involved.
   Resource res;
   res.dev = &dev; res.kind = ResKind::Image; res.dim = D3D10DDIRESOURCE_TEXTURE2D;
   res.imageType = VK_IMAGE_TYPE_2D; res.width = 32; res.height = 16;
   res.mips = 3; res.arraySize = 4; res.format = DXGI_FORMAT_R8G8B8A8_UNORM;
   res.aspects = VK_IMAGE_ASPECT_COLOR_BIT;
   Storage<View> srv, rtv;
   D3DWDDM2_0DDIARG_CREATESHADERRESOURCEVIEW sd = {};
   sd.hDrvResource = {&res}; sd.Format = res.format; sd.ResourceDimension = res.dim;
   sd.Tex2D = {1, 2, 2, 2, 0};
   errors = 0;
   CHECK(t.pfnCalcPrivateShaderResourceViewSize(h, &sd) == sizeof(View));
   t.pfnCreateShaderResourceView(h, &sd, {srv.object()}, {});
   CHECK(!errors && srv.object()->res == &res && srv.object()->width == 16 && srv.object()->height == 8);
   const auto sr = srv.object()->info.subresourceRange;
   CHECK(sr.baseMipLevel == 1 && sr.levelCount == 2 && sr.baseArrayLayer == 2 && sr.layerCount == 2);
   t.pfnDestroyShaderResourceView(h, {srv.object()});
   sd.Tex2D.PlaneSlice = 1;
   t.pfnCreateShaderResourceView(h, &sd, {srv.object()}, {});
   CHECK(errors == 1 && error == DXGI_ERROR_UNSUPPORTED && !res.views);

   D3DWDDM2_0DDIARG_CREATERENDERTARGETVIEW rd = {};
   rd.hDrvResource = {&res}; rd.Format = res.format; rd.ResourceDimension = res.dim;
   rd.Tex2D = {2, 1, 2, 0}; errors = 0;
   CHECK(t.pfnCalcPrivateRenderTargetViewSize(h, &rd) == sizeof(View));
   t.pfnCreateRenderTargetView(h, &rd, {rtv.object()}, {});
   CHECK(!errors && rtv.object()->width == 8 && rtv.object()->height == 4);
   const auto rr = rtv.object()->info.subresourceRange;
   CHECK(rr.baseMipLevel == 2 && rr.levelCount == 1 && rr.baseArrayLayer == 1 && rr.layerCount == 2);
   t.pfnDestroyRenderTargetView(h, {rtv.object()});
   rd.Tex2D.PlaneSlice = 1;
   t.pfnCreateRenderTargetView(h, &rd, {rtv.object()}, {});
   CHECK(errors == 1 && error == DXGI_ERROR_UNSUPPORTED && !res.views);

   Storage<RasterizerState> raster;
   D3DWDDM2_0DDI_RASTERIZER_DESC rasterDesc = {};
   rasterDesc.ForcedSampleCount = 1; rasterDesc.ScissorEnable = TRUE; errors = 0;
   CHECK(t.pfnCalcPrivateRasterizerStateSize(h, &rasterDesc) == sizeof(RasterizerState));
   t.pfnCreateRasterizerState(h, &rasterDesc, {raster.object()}, {});
   CHECK(!errors && raster.object()->desc.ScissorEnable);
   t.pfnDestroyRasterizerState(h, {raster.object()});
   VulkanCaps rasterCaps{};
   rasterCaps.attachmentlessSamples |= VK_SAMPLE_COUNT_4_BIT;
   dev.caps = &rasterCaps;
   rasterDesc.ForcedSampleCount = 4;
   t.pfnCreateRasterizerState(h, &rasterDesc, {raster.object()}, {});
   CHECK(!errors && raster.object()->samples == VK_SAMPLE_COUNT_4_BIT);
   t.pfnDestroyRasterizerState(h, {raster.object()});
   rasterDesc.ConservativeRasterizationMode = D3DWDDM2_0DDI_CONSERVATIVE_RASTERIZATION_ON;
   t.pfnCreateRasterizerState(h, &rasterDesc, {raster.object()}, {});
   CHECK(errors == 1 && error == DXGI_ERROR_UNSUPPORTED);

   Storage<Query> query;
   D3DWDDM2_0DDIARG_CREATEQUERY qd = {D3D10DDI_QUERY_EVENT, 0, D3DWDDM2_0DDI_CONTEXTTYPE_3D};
   errors = 0;
   CHECK(t.pfnCalcPrivateQuerySize(h, &qd) == sizeof(Query));
   t.pfnCreateQuery(h, &qd, {query.object()}, {});
   CHECK(!errors && query.object()->type == D3D10DDI_QUERY_EVENT && query.object()->seq == 0);
   t.pfnDestroyQuery(h, {query.object()});
   qd.ContextType = D3DWDDM2_0DDI_CONTEXTTYPE_COMPUTE;
   t.pfnCreateQuery(h, &qd, {query.object()}, {});
   CHECK(errors == 1 && error == DXGI_ERROR_UNSUPPORTED);
   errors = 0;
   CHECK(!t.pfnFlush(h, D3DWDDM2_0DDI_CONTEXTTYPE_VIDEO, 0));
   CHECK(errors == 1 && error == E_INVALIDARG);

   D3D10DDI_MIPINFO mip = {256, 1, 1, 256, 1, 1};
   D3D11DDIARG_CREATERESOURCE desc = {};
   desc.pMipInfoList = &mip; desc.ResourceDimension = D3D10DDIRESOURCE_BUFFER;
   desc.BindFlags = D3D10_DDI_BIND_CONSTANT_BUFFER;
   desc.SampleDesc.Count = desc.MipLevels = desc.ArraySize = 1;
   unsigned char input[256], backing[512];
   for (unsigned i = 0; i < sizeof(input); ++i) input[i] = (i * 7 + 3) & 255;
   D3D10_DDIARG_SUBRESOURCE_UP initial = {input, sizeof(input), sizeof(input)};
   desc.pInitialDataUP = &initial;
   Chunk chunk; chunk.cpu = backing; chunk.size = sizeof(backing); chunk.buffer = (VkBuffer)(uintptr_t)0x200;
   dev.currentChunk = &chunk;
   Storage<Resource> buffer;
   errors = 0;
   t.pfnCreateResource(h, &desc, {buffer.object()}, {});
   CHECK(!errors && chunk.live == 1 && !std::memcmp(backing, input, sizeof(input)));
   t.pfnDestroyResource(h, {buffer.object()});
   // Match 22621's actual buffer descriptor: CPU access implies ROW_MAJOR.
   // Exercise default, dynamic and staging buffers through the real entry.
   desc.TextureLayout = D3DWDDM2_0DDI_TL_ROW_MAJOR;
   for (UINT usage : {D3D10_DDI_USAGE_DEFAULT, D3D10_DDI_USAGE_DYNAMIC, D3D10_DDI_USAGE_STAGING}) {
      desc.Usage = usage; desc.MapFlags = D3D10_DDI_CPU_ACCESS_WRITE;
      errors = 0; std::memset(backing, 0, sizeof(backing));
      t.pfnCreateResource(h, &desc, {buffer.object()}, {});
      CHECK(!errors && chunk.live == 1 && buffer.object()->kind == ResKind::Buffer);
      CHECK(!std::memcmp(backing, input, sizeof(input)));
      t.pfnDestroyResource(h, {buffer.object()});
   }
   // Existing staging storage is linear too, including padded rows.
   mip = {3, 2, 1, 3, 2, 1}; desc.ResourceDimension = D3D10DDIRESOURCE_TEXTURE2D;
   desc.Usage = D3D10_DDI_USAGE_STAGING; desc.BindFlags = 0;
   desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
   initial.SysMemPitch = 20; initial.SysMemSlicePitch = 40; errors = 0;
   t.pfnCreateResource(h, &desc, {buffer.object()}, {});
   CHECK(!errors && buffer.object()->kind == ResKind::Staging && chunk.live == 1);
   const auto &rows = buffer.object()->layout[0];
   CHECK(rows.rowPitch >= 12 && !std::memcmp(buffer.object()->slice.cpu(), input, 12));
   CHECK(!std::memcmp(buffer.object()->slice.cpu() + rows.rowPitch, input + 20, 12));
   t.pfnDestroyResource(h, {buffer.object()});
   desc.Usage = D3D10_DDI_USAGE_DEFAULT;
   for (auto layout : {D3DWDDM2_0DDI_TL_ROW_MAJOR, D3DWDDM2_0DDI_TL_STANDARD_SWIZZLE_64K_TILE}) {
      desc.TextureLayout = layout; errors = 0; dev.lost = S_OK; // independent negative fixture
      t.pfnCreateResource(h, &desc, {buffer.object()}, {});
      CHECK(errors == 1 && error == D3DDDIERR_DEVICEREMOVED && !chunk.live && !dev.resources);
   }
   std::puts("PASS: typed WDDM2.0/DXGI1.4 boundaries, real views, ROW_MAJOR buffers/padded staging and unsupported texture-layout rejection (CPU control)");
}

static void om_binding_contracts()
{
   Device dev;
   VulkanCaps caps;
   dev.caps = &caps;
   dev.um.pfnSetErrorCb = [](D3D10DDI_HRTCORELAYER, HRESULT hr) { error = hr; ++errors; };
   D3D10DDI_HDEVICE h{&dev};
   D3D11_1DDI_DEVICEFUNCS t{};
   device_fill_table11(&t);
   View left{}, right{}, srv{}, rtv{};
   left.dev = right.dev = srv.dev = rtv.dev = &dev;
   left.type = right.type = ViewType::UAV;
   srv.type = ViewType::SRV; rtv.type = ViewType::RTV;
   D3D11DDI_HUNORDEREDACCESSVIEW last[] = {{&left}, {&right}};
   errors = 0;
   t.pfnSetRenderTargets(h, nullptr, 0, 0, {}, last, nullptr, 62, 2, 62, 2);
   CHECK(errors == 1 && error == DXGI_ERROR_UNSUPPORTED && !dev.graphicsUav[62]);
   caps.graphicsUavSlots = D3D_UAV_SLOTS; errors = 0;
   t.pfnCsSetUnorderedAccessViews(h, 63, 1, last, nullptr);
   t.pfnSetRenderTargets(h, nullptr, 0, 0, {}, last, nullptr, 62, 2, 62, 2);
   CHECK(!errors && dev.graphicsUav[62] == &left && dev.graphicsUav[63] == &right && dev.computeUav[63] == &left);
   dev.dirty = 0;
   t.pfnSetRenderTargets(h, nullptr, 0, 0, {}, last + 1, nullptr, 63, 1, 63, 1);
   CHECK(!errors && !dev.dirty && dev.graphicsUav[62] == &left);
   last[0] = {}; last[1] = {&srv};
   t.pfnSetRenderTargets(h, nullptr, 0, 0, {}, last, nullptr, 62, 2, 62, 2);
   CHECK(errors == 1 && error == E_INVALIDARG && !dev.dirty && dev.graphicsUav[62] == &left && dev.graphicsUav[63] == &right);
   errors = 0;
   t.pfnSetRenderTargets(h, nullptr, 0, 0, {}, last, nullptr, 63, 2, 63, 1);
   CHECK(errors == 1 && error == E_INVALIDARG && dev.graphicsUav[63] == &right);
   errors = 0;
   t.pfnSetRenderTargets(h, nullptr, 0, 0, {}, last, nullptr, 63, 1, 63, 1);
   CHECK(!errors && (dev.dirty & DIRTY_GRAPHICS_UAVS) && !dev.graphicsUav[63] && dev.computeUav[63] == &left);
   D3D11DDI_HUNORDEREDACCESSVIEW low{&left};
   t.pfnSetRenderTargets(h, nullptr, 0, 0, {}, &low, nullptr, 0, 1, 0, 1);
   D3D10DDI_HRENDERTARGETVIEW target{&rtv};
   t.pfnSetRenderTargets(h, &target, 1, 0, {}, &low, nullptr, 0, 1, 0, 1);
   CHECK(errors == 1 && error == E_INVALIDARG && !dev.rtv[0] && dev.graphicsUav[0] == &left);
   errors = 0;
   t.pfnSetRenderTargets(h, &target, 1, 0, {}, nullptr, nullptr, 0, 0, 0, 0);
   CHECK(!errors && dev.rtv[0] == &rtv && !dev.graphicsUav[0] && dev.graphicsUav[62] == &left);
   std::puts("PASS: OM UAV range updates, RTV slot conflicts, compute isolation and failure-atomic validation (CPU control)");
}

void ddi11_contracts()
{
   adapter_caps_contracts();
   ddi20_contracts();
   om_binding_contracts();
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
   for (auto setter : {t.pfnVsSetConstantBuffers, t.pfnGsSetConstantBuffers, t.pfnPsSetConstantBuffers,
                       t.pfnHsSetConstantBuffers, t.pfnDsSetConstantBuffers, t.pfnCsSetConstantBuffers})
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
   CHECK(errors == 1 && error == D3DDDIERR_DEVICEREMOVED && !chunk.live && !dev.resources);
   dev.lost = S_OK; // subsequent view fixtures model a separate healthy device
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
      dev.dsv = view.object();
      dev.dirty = 0;
      t.pfnDestroyDepthStencilView(h, {view.object()});
      CHECK(!dev.dsv && (dev.dirty & DIRTY_BLEND) && !depth.views && dev.views.empty());
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

   VulkanCaps caps = {};
   caps.attachmentlessSamples = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT | VK_SAMPLE_COUNT_8_BIT;
   dev.caps = &caps;
   D3D11_1_DDI_RASTERIZER_DESC raster = {};
   Storage<RasterizerState> rasterState;
   for (UINT samples : {0u, 1u, 2u, 3u, 4u, 8u, 16u}) {
      raster.ForcedSampleCount = samples; errors = 0;
      t.pfnCreateRasterizerState(h, &raster, {rasterState.object()}, {});
      if (samples <= 1 || samples == 4 || samples == 8) {
         CHECK(!errors && UINT(rasterState.object()->samples) == std::max(1u, samples));
         t.pfnDestroyRasterizerState(h, {rasterState.object()});
      }
      else CHECK(errors == 1 && error == (samples == 16 ? DXGI_ERROR_UNSUPPORTED : E_INVALIDARG));
   }
   errors = 0;
   t.pfnCreateRasterizerState(h, &raster, {}, {});
   CHECK(errors == 1 && error == E_INVALIDARG);
   caps.attachmentlessSamples |= VK_SAMPLE_COUNT_16_BIT;
   errors = 0;
   t.pfnCreateRasterizerState(h, &raster, {rasterState.object()}, {});
   CHECK(!errors && rasterState.object()->samples == VK_SAMPLE_COUNT_16_BIT);
   t.pfnDestroyRasterizerState(h, {rasterState.object()});

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
   caps.logicOp = true; errors = 0;
   t.pfnCreateBlendState(h, &blend, {state.object()}, {});
   CHECK(errors == 1 && error == E_INVALIDARG);
   blend.RenderTarget[0].BlendEnable = FALSE;
   constexpr VkLogicOp operations[] = {
      VK_LOGIC_OP_CLEAR, VK_LOGIC_OP_SET, VK_LOGIC_OP_COPY, VK_LOGIC_OP_COPY_INVERTED,
      VK_LOGIC_OP_NO_OP, VK_LOGIC_OP_INVERT, VK_LOGIC_OP_AND, VK_LOGIC_OP_NAND,
      VK_LOGIC_OP_OR, VK_LOGIC_OP_NOR, VK_LOGIC_OP_XOR, VK_LOGIC_OP_EQUIVALENT,
      VK_LOGIC_OP_AND_REVERSE, VK_LOGIC_OP_AND_INVERTED, VK_LOGIC_OP_OR_REVERSE, VK_LOGIC_OP_OR_INVERTED,
   };
   for (unsigned i = 0; i < sizeof(operations)/sizeof(operations[0]); ++i) {
      blend.RenderTarget[0].LogicOp = static_cast<D3D11_1_DDI_LOGIC_OP>(i);
      errors = 0;
      t.pfnCreateBlendState(h, &blend, {state.object()}, {});
      CHECK(!errors && state.object()->logicOpEnabled && state.object()->logicOp == operations[i]);
      t.pfnDestroyBlendState(h, {state.object()});
   }
   blend.IndependentBlendEnable = TRUE;
   errors = 0;
   t.pfnCreateBlendState(h, &blend, {state.object()}, {});
   CHECK(errors == 1 && error == E_INVALIDARG);

   BOOL direct = TRUE;
   t.pfnCheckDirectFlipSupport(h, {}, {}, 0, &direct);
   CHECK(!direct);
   std::puts("PASS: typed modern-table bridge, guarded descriptor revision, SRV unions, blend state and CB windows (CPU only; table not advertised)");
}
