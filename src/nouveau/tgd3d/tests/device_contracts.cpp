/* SPDX-License-Identifier: MIT
 * Exercise the production device constructor/destructor and negotiated tables.
 * Vulkan/runtime substitutes only: no physical device or display is opened.
 */
#include "tgd3d.h"
#include "tgd3d_legacy9.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <type_traits>
#include <cstddef>

using namespace tgd3d;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%u: %s\n", __FILE__, __LINE__, #x); std::exit(1); } } while (0)
namespace {
template<typename T> T H(uintptr_t v) { return (T)v; }
template<typename T> struct Stub;
template<typename R, typename... A> struct Stub<R(VKAPI_PTR *)(A...)> {
   static R VKAPI_CALL call(A...) { if constexpr (!std::is_void_v<R>) return R{}; }
};
static struct {
   UINT failure, coreCreates, ktCreates, contexts, queues, devices, objects, errors;
   UINT deadContexts, deadQueues, deadDevices, deadObjects;
   bool modern;
   UINT64 paging;
} s;
static VkDeviceApi api;

static void init(Adapter *adapter, D3DDDI_DEVICECALLBACKS *kt,
                  D3DWDDM2_0DDI_CORELAYER_DEVICECALLBACKS *core)
{
   adapter->vk.caps.cbSlots = D3D_CB_SLOTS;
   adapter->vk.vk.GetPhysicalDeviceFeatures2 = [](VkPhysicalDevice, VkPhysicalDeviceFeatures2 *) VKAPI_CALL {};
   adapter->vk.vk.CreateDevice = [](VkPhysicalDevice, const VkDeviceCreateInfo *,
                                   const VkAllocationCallbacks *, VkDevice *out) VKAPI_CALL {
      if (s.failure == 5) return VK_ERROR_OUT_OF_HOST_MEMORY;
      *out = H<VkDevice>(0x100); ++s.devices; return VK_SUCCESS;
   };
   adapter->vk.vk.GetDeviceProcAddr = [](VkDevice, const char *name) VKAPI_CALL -> PFN_vkVoidFunction {
#define X(n) if (!std::strcmp(name, "vk" #n)) return reinterpret_cast<PFN_vkVoidFunction>(api.n);
      TGD3D_VK_DEVICE_FUNCS(X)
      TGD3D_VK_OPTIONAL_DEVICE_FUNCS(X)
#undef X
      return nullptr;
   };
#define X(n) api.n = Stub<PFN_vk##n>::call;
   TGD3D_VK_DEVICE_FUNCS(X)
   TGD3D_VK_OPTIONAL_DEVICE_FUNCS(X)
#undef X
   api.DestroyDevice = [](VkDevice d, const VkAllocationCallbacks *) VKAPI_CALL {
      CHECK(d == H<VkDevice>(0x100)); ++s.deadDevices;
   };
   api.GetDeviceQueue = [](VkDevice, UINT, UINT, VkQueue *out) VKAPI_CALL { *out = H<VkQueue>(0x200); };
   api.CreateSemaphore = [](VkDevice, const VkSemaphoreCreateInfo *, const VkAllocationCallbacks *, VkSemaphore *out) VKAPI_CALL {
      if (s.failure == 6) return VK_ERROR_OUT_OF_HOST_MEMORY;
      *out = H<VkSemaphore>(++s.objects); return VK_SUCCESS;
   };
   api.CreateCommandPool = [](VkDevice, const VkCommandPoolCreateInfo *, const VkAllocationCallbacks *, VkCommandPool *out) VKAPI_CALL {
      if (s.failure == 7) return VK_ERROR_OUT_OF_HOST_MEMORY;
      *out = H<VkCommandPool>(++s.objects); return VK_SUCCESS;
   };
   api.CreateDescriptorSetLayout = [](VkDevice, const VkDescriptorSetLayoutCreateInfo *, const VkAllocationCallbacks *, VkDescriptorSetLayout *out) VKAPI_CALL {
      if (s.failure == 8 && s.objects == 3) return VK_ERROR_OUT_OF_HOST_MEMORY;
      *out = H<VkDescriptorSetLayout>(++s.objects); return VK_SUCCESS;
   };
   api.CreatePipelineLayout = [](VkDevice, const VkPipelineLayoutCreateInfo *, const VkAllocationCallbacks *, VkPipelineLayout *out) VKAPI_CALL {
      *out = H<VkPipelineLayout>(++s.objects); return VK_SUCCESS;
   };
   api.CreateSampler = [](VkDevice, const VkSamplerCreateInfo *, const VkAllocationCallbacks *, VkSampler *out) VKAPI_CALL {
      *out = H<VkSampler>(++s.objects); return VK_SUCCESS;
   };
#define DESTROY(n, type) api.n = [](VkDevice, type object, const VkAllocationCallbacks *) VKAPI_CALL { if (object) ++s.deadObjects; }
   DESTROY(DestroySemaphore, VkSemaphore);
   DESTROY(DestroyCommandPool, VkCommandPool);
   DESTROY(DestroyDescriptorSetLayout, VkDescriptorSetLayout);
   DESTROY(DestroyPipelineLayout, VkPipelineLayout);
   DESTROY(DestroySampler, VkSampler);
#undef DESTROY
   api.GetSemaphoreCounterValue = [](VkDevice, VkSemaphore, UINT64 *out) VKAPI_CALL { *out = 0; return VK_SUCCESS; };
   core->pfnSetErrorCb = [](D3D10DDI_HRTCORELAYER, HRESULT) APIENTRY { ++s.errors; };
   core->pfnCreateContextVirtualCb = [](D3D10DDI_HRTCORELAYER h,
                                       D3DWDDM2_0DDICB_CREATECONTEXTVIRTUAL *c) APIENTRY -> HRESULT {
      CHECK(s.modern && h.handle == H<HANDLE>(0x300) && !c->NodeOrdinal && c->EngineAffinity == 1);
      CHECK(c->ContextTypeFlags == (D3DWDDM2_0DDI_CONTEXTTYPE_3D | D3DWDDM2_0DDI_CONTEXTTYPE_COPY));
      CHECK(!c->Flags.Value && !c->PrivateDriverDataSize && !c->pPrivateDriverData);
      ++s.coreCreates;
      if (s.failure == 1) return E_OUTOFMEMORY;
      if (s.failure != 2) { c->hContext = H<HANDLE>(0x400); ++s.contexts; }
      return S_OK;
   };
   kt->pfnCreateContextVirtualCb = [](HANDLE h, D3DDDICB_CREATECONTEXTVIRTUAL *c) APIENTRY -> HRESULT {
      CHECK(!s.modern && h == H<HANDLE>(0x500));
      c->hContext = H<HANDLE>(0x400); ++s.ktCreates; ++s.contexts; return S_OK;
   };
   kt->pfnDestroyContextCb = [](HANDLE h, const D3DDDICB_DESTROYCONTEXT *c) APIENTRY -> HRESULT {
      CHECK(h == H<HANDLE>(0x500) && c->hContext == H<HANDLE>(0x400)); ++s.deadContexts; return S_OK;
   };
   kt->pfnCreatePagingQueueCb = [](HANDLE h, D3DDDICB_CREATEPAGINGQUEUE *q) APIENTRY -> HRESULT {
      CHECK(h == H<HANDLE>(0x500) && q->Priority == D3DDDI_PAGINGQUEUE_PRIORITY_NORMAL);
      if (s.failure == 3) return E_OUTOFMEMORY;
      q->hPagingQueue = 0x600; ++s.queues;
      if (s.failure != 4) q->hSyncObject = 0x601;
      q->FenceValueCPUVirtualAddress = &s.paging; return S_OK;
   };
   kt->pfnDestroyPagingQueueCb = [](HANDLE h, const D3DDDI_DESTROYPAGINGQUEUE *q) APIENTRY -> HRESULT {
      CHECK(h == H<HANDLE>(0x500) && q->hPagingQueue == 0x600); ++s.deadQueues; return S_OK;
   };
   kt->pfnAllocateCb = [](HANDLE, D3DDDICB_ALLOCATE *) APIENTRY -> HRESULT { CHECK(false); return E_FAIL; };
   kt->pfnDeallocateCb = [](HANDLE, const D3DDDICB_DEALLOCATE *) APIENTRY -> HRESULT { CHECK(false); return E_FAIL; };
   kt->pfnMakeResidentCb = [](HANDLE, D3DDDI_MAKERESIDENT *) APIENTRY -> HRESULT { CHECK(false); return E_FAIL; };
   kt->pfnEvictCb = [](HANDLE, D3DDDICB_EVICT *) APIENTRY -> HRESULT { CHECK(false); return E_FAIL; };
   kt->pfnMapGpuVirtualAddressCb = [](HANDLE, D3DDDI_MAPGPUVIRTUALADDRESS *) APIENTRY -> HRESULT { CHECK(false); return E_FAIL; };
   kt->pfnFreeGpuVirtualAddressCb = [](HANDLE, const D3DDDICB_FREEGPUVIRTUALADDRESS *) APIENTRY -> HRESULT { CHECK(false); return E_FAIL; };
   kt->pfnWaitForSynchronizationObjectFromCpuCb = [](HANDLE, const D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMCPU *) APIENTRY -> HRESULT { CHECK(false); return E_FAIL; };
   kt->pfnLock2Cb = [](HANDLE, D3DDDICB_LOCK2 *) APIENTRY -> HRESULT { CHECK(false); return E_FAIL; };
   kt->pfnUnlock2Cb = [](HANDLE, const D3DDDICB_UNLOCK2 *) APIENTRY -> HRESULT { CHECK(false); return E_FAIL; };
}
}

static void legacy9_upload_contracts(HANDLE device, const D3DDDI_DEVICEFUNCS &table)
{
   std::array<BYTE, 24> buffer{};
   D3DDDI_SURFACEINFO layout{};
   layout.Width = UINT(buffer.size());
   layout.Height = layout.Depth = 1;
   layout.pSysMem = buffer.data();
   D3DDDIARG_CREATERESOURCE2 vb{};
   vb.hResource = &vb;
   vb.Format = D3DDDIFMT_VERTEXDATA;
   vb.Pool = D3DDDIPOOL_SYSTEMMEM;
   vb.Flags.VertexBuffer = 1;
   vb.SurfCount = vb.MipLevels = 1;
   vb.pSurfList = &layout;
   CHECK(table.pfnCreateResource2(device, &vb) == S_OK);
   const BYTE source[]{1, 4, 6, 8};
   D3DDDIARG_UPDATESUBRESOURCEUP update{};
   update.hResource = vb.hResource;
   update.pSysMemUP = source;
   update.Flags.BoxValid = update.Flags.NoOverwrite = 1;
   update.DstBox = {4, 0, 8, 1, 0, 1};
   CHECK(table.pfnUpdateSubresourceUP(device, &update) == S_OK);
   CHECK(!std::memcmp(buffer.data() + 4, source, sizeof(source)));
   CHECK(buffer[3] == 0 && buffer[8] == 0);
   update.DstBox.Right = UINT(buffer.size()) + 1;
   CHECK(table.pfnUpdateSubresourceUP(device, &update) == E_INVALIDARG);
   CHECK(table.pfnDestroyResource(device, vb.hResource) == S_OK);

   // Three active pixels plus one guard pixel per row. The source aliases
   // the destination one row earlier, so a forward per-row copy alone fails.
   UINT pixels[4][4] = {{1,2,3,90},{4,5,6,91},{7,8,9,92},{10,11,12,93}};
   const UINT expected[4][4] = {{1,2,3,90},{1,2,3,91},{4,5,6,92},{7,8,9,93}};
   layout.Width = 3; layout.Height = 4; layout.SysMemPitch = sizeof(pixels[0]); layout.pSysMem = pixels;
   D3DDDIARG_CREATERESOURCE2 tex{};
   tex.hResource = &tex; tex.Pool = D3DDDIPOOL_SYSTEMMEM; tex.Format = D3DDDIFMT_A8R8G8B8;
   tex.Flags.Texture = tex.Flags.CpuOptimized = 1;
   tex.SurfCount = tex.MipLevels = 1; tex.pSurfList = &layout;
   CHECK(table.pfnCreateResource2(device, &tex) == S_OK);
   update = {};
   update.hResource = tex.hResource; update.Flags.BoxValid = 1; update.RowPitch = sizeof(pixels[0]);
   update.DstBox = {0,1,3,4,0,1}; update.pSysMemUP = pixels;
   CHECK(table.pfnUpdateSubresourceUP(device, &update) == S_OK);
   CHECK(!std::memcmp(pixels, expected, sizeof(pixels)));
   update.Flags.Value = 0; update.pSysMemUP = expected; // Entire subresource; ignore stale DstBox.
   CHECK(table.pfnUpdateSubresourceUP(device, &update) == S_OK);
   D3DDDIARG_LOCK lock{}; lock.hResource = tex.hResource; lock.Flags.ReadOnly = 1;
   CHECK(table.pfnLock(device, &lock) == S_OK);
   CHECK(table.pfnUpdateSubresourceUP(device, &update) == D3DERR_INVALIDCALL);
   D3DDDIARG_UNLOCK unlock{}; unlock.hResource = tex.hResource;
   CHECK(table.pfnUnlock(device, &unlock) == S_OK);
   update.SubResourceIndex = 1;
   CHECK(table.pfnUpdateSubresourceUP(device, &update) == E_INVALIDARG);
   CHECK(table.pfnDestroyResource(device, tex.hResource) == S_OK);
}

static void legacy9_mip_edges_contract()
{
   using namespace legacy9;
   Device graphics{};
   LegacyDevice device{};
   device.slot.state = &graphics;
   // DDI-level copy on a 5x3 image and its 2x1 and 1x1 sublevels. Separate
   // backing arrays make it observable which source level supplied each texel.
   Surface src{}, dst{};
   UINT inputs[3][15]{}, outputs[3][15]{};
   for (UINT i = 0; i < 3; ++i)
      for (UINT n = 0; n < 15; ++n) inputs[i][n] = 0x1100 + 16*i + n;
   auto initialize = [&](Surface &surface, UINT (&data)[3][15]) {
      surface.owner = &device;
      surface.width = 5; surface.height = 3; surface.levels = 3;
      surface.format = DXGI_FORMAT_B8G8R8A8_UNORM;
      surface.texture = std::make_unique<TextureLevel[]>(3);
      for (UINT i = 0; i < 3; ++i) {
         auto &level = surface.texture[i];
         level.width = i == 0 ? 5 : i == 1 ? 2 : 1;
         level.height = i == 0 ? 3 : 1;
         level.pitch = level.width * sizeof(UINT);
         level.bytes = reinterpret_cast<BYTE *>(data[i]);
      }
   };
   initialize(src, inputs);
   initialize(dst, outputs);
   src.system = true;
   static UINT writes;
   writes = 0;
   device.graphics.pfnResourceUpdateSubresourceUP = [](D3D10DDI_HDEVICE, D3D10DDI_HRESOURCE,
       UINT, const D3D10_DDI_BOX *, const void *, UINT, UINT) APIENTRY { ++writes; };
   D3DDDIARG_TEXBLT1 blt{};
   blt.hSrcResource = &src; blt.hDstResource = &dst;
   blt.SrcRect = {4,2,5,3}; blt.DstPoint = {4,2};
   CHECK(texture_blt(&device, &blt) == S_OK);
   CHECK(writes == 3 && outputs[0][14] == inputs[0][14] &&
         outputs[1][1] == inputs[1][1] && outputs[2][0] == inputs[2][0]);
   CHECK(!outputs[0][13] && !outputs[1][0] && !outputs[2][1]);
   blt.SrcRect = {1,0,3,1}; blt.DstPoint = {3,0};
   CHECK(texture_blt(&device, &blt) == S_OK);
   CHECK(writes == 6 && outputs[0][3] == inputs[0][1] && outputs[0][4] == inputs[0][2] &&
         outputs[1][1] == inputs[1][0] && !outputs[1][0]);
   blt.DstPoint.x = 4; // Invalid already at the largest common level.
   CHECK(texture_blt(&device, &blt) == D3DERR_INVALIDCALL && writes == 6);

   dst.width = 2; dst.height = 1; dst.levels = 2;
   dst.texture[0].width = 2; dst.texture[0].height = 1; dst.texture[0].pitch = 8;
   dst.texture[1].width = dst.texture[1].height = 1; dst.texture[1].pitch = 4;
   std::memset(outputs, 0, sizeof(outputs));
   blt.SrcRect = {4,2,5,3}; blt.DstPoint = {1,0};
   CHECK(texture_blt(&device, &blt) == S_OK && writes == 8);
   CHECK(outputs[0][1] == inputs[1][1] && outputs[1][0] == inputs[2][0]);
   CHECK(!outputs[0][0] && !outputs[0][2] && !outputs[1][1]);
}

static void legacy9_retry_contract()
{
   using namespace legacy9;
   Device graphics{};
   LegacyDevice device{};
   device.slot.state = &graphics;
   Surface texture{};
   texture.owner = &device;
   texture.lockable = true;
   texture.levels = 1;
   texture.texture = std::make_unique<TextureLevel[]>(1);
   UINT cpu[2][3]{};
   auto &level = texture.texture[0];
   level.width = 3; level.height = 2; level.pitch = sizeof(cpu[0]);
   level.bytes = reinterpret_cast<BYTE *>(cpu);
   static struct { UINT failures, accepted, gpu[2][3]; } backend;
   backend = {};
   backend.failures = 2;
   device.graphics.pfnResourceUpdateSubresourceUP = [](D3D10DDI_HDEVICE h, D3D10DDI_HRESOURCE,
       UINT sub, const D3D10_DDI_BOX *box, const void *input, UINT pitch, UINT) APIENTRY {
      auto *dev = reinterpret_cast<LegacyDevice *>(h.pDrvPrivate); // LegacyDevice starts with DeviceSlot.
      static_assert(offsetof(LegacyDevice, slot) == 0);
      CHECK(sub == 0 && box && box->back == 1);
      if (backend.failures) {
         --backend.failures;
         dev->error = E_OUTOFMEMORY;
         return;
      }
      for (LONG y = box->top; y < box->bottom; ++y)
         std::memcpy(&backend.gpu[y][box->left], static_cast<const BYTE *>(input) + size_t(y - box->top) * pitch,
                     (box->right - box->left) * sizeof(UINT));
      ++backend.accepted;
   };
   D3DDDIARG_LOCK lock{};
   lock.Flags.AreaValid = lock.Flags.WriteOnly = 1; lock.Area = {1,0,2,1};
   CHECK(lock_texture(&texture, &lock) == S_OK);
   *static_cast<UINT *>(lock.pSurfData) = 23;
   D3DDDIARG_UNLOCK unlock{};
   CHECK(unlock_texture(&texture, &unlock) == E_OUTOFMEMORY);
   CHECK(!level.locked && !IsRectEmpty(&level.pendingUpload) && !backend.gpu[0][1]);
   device.begin();
   CHECK(prepare_texture(&texture) == E_OUTOFMEMORY);
   lock.Area = {2,1,3,2};
   CHECK(lock_texture(&texture, &lock) == S_OK);
   *static_cast<UINT *>(lock.pSurfData) = 37;
   CHECK(prepare_texture(&texture) == D3DERR_INVALIDCALL);
   CHECK(unlock_texture(&texture, &unlock) == S_OK);
   CHECK(backend.accepted == 1 && IsRectEmpty(&level.pendingUpload));
   CHECK(!std::memcmp(backend.gpu, cpu, sizeof(cpu)));
   CHECK(prepare_texture(&texture) == S_OK && backend.accepted == 1);

   UINT replacement = 51;
   texture.format = DXGI_FORMAT_B8G8R8A8_UNORM;
   D3DDDIARG_UPDATESUBRESOURCEUP up{};
   up.hResource = &texture; up.SubResourceIndex = 0; up.Flags.BoxValid = 1;
   up.DstBox = {1,0,2,1,0,1}; up.pSysMemUP = &replacement;
   backend.failures = 1;
   CHECK(update_subresource(&device, &up) == E_OUTOFMEMORY);
   CHECK(cpu[0][1] == 23 && backend.gpu[0][1] == 23);
   CHECK(update_subresource(&device, &up) == S_OK && cpu[0][1] == 51 && backend.gpu[0][1] == 51);
}

static void legacy9_initialization_contract()
{
   Adapter adapter{};
   D3DDDI_DEVICECALLBACKS callbacks{};
   D3DWDDM2_0DDI_CORELAYER_DEVICECALLBACKS unusedCore{};
   s = {};
   init(&adapter, &callbacks, &unusedCore);
   adapter.vk.caps.maxImageDimension2D = 4096;

   struct {
      UINT64 before;
      D3DDDI_DEVICEFUNCS table;
      UINT64 after;
   } output{};
   output.before = output.after = UINT64_C(0xd011cab1ed011cab);
   std::memset(&output.table, 0xa5, sizeof(output.table));
   D3DDDIARG_CREATEDEVICE create{};
   create.hDevice = H<HANDLE>(0x500);
   create.Interface = D3D9_RUNTIME_INTERFACE;
   create.Version = 0x10000; // Runtime build field observed on the Switch.
   create.pDeviceFuncs = &output.table;
   create.pCallbacks = &callbacks;
   CHECK(legacy9_create_device(&adapter, &create) == S_OK);
   CHECK(create.hDevice && create.hDevice != H<HANDLE>(0x500));
   const auto &table = output.table;
   CHECK(!table.pfnCreateResource && table.pfnCreateResource2);
   CHECK(!table.pfnBufBlt && table.pfnBufBlt1);
   CHECK(!table.pfnTexBlt && table.pfnTexBlt1);
   CHECK(!table.pfnVolBlt && table.pfnVolBlt1);
   CHECK(table.pfnSetScissorRect && table.pfnSetRenderState && table.pfnSetTextureStageState);
   CHECK(table.pfnSetPixelShader && table.pfnSetVertexShaderFunc && table.pfnSetDepthStencil && table.pfnDestroyDevice);
   constexpr size_t used = offsetof(D3DDDI_DEVICEFUNCS, pfnTrimResidencySet) + sizeof(table.pfnTrimResidencySet);
   const auto *bytes = reinterpret_cast<const BYTE *>(&table);
   for (size_t i = used; i < sizeof(table); ++i) CHECK(bytes[i] == 0xa5);
   CHECK(output.before == UINT64_C(0xd011cab1ed011cab) && output.after == output.before);

   const HANDLE device = create.hDevice;
   CHECK(table.pfnPresent1 && table.pfnFlush1 && table.pfnUpdateSubresourceUP && table.pfnCheckCounterInfo &&
         table.pfnCheckCounter && table.pfnSetMarker && table.pfnSetMarkerMode);
   D3DDDIARG_COUNTER_INFO info{};
   info.NumSimultaneousCounters = 7;
   table.pfnCheckCounterInfo(device, &info);
   CHECK(info.NumSimultaneousCounters == 0 && UINT(info.LastDeviceDependentCounter) == 0);
   D3DDDI_COUNTER_TYPE counterType{};
   UINT active = 0;
   CHECK(table.pfnCheckCounter(device, D3DDDIQUERYTYPE_EVENT, &counterType, &active, nullptr, nullptr,
                               nullptr, nullptr, nullptr, nullptr) == D3DERR_NOTAVAILABLE);
   CHECK(table.pfnFlush1(device, 0) == S_OK && table.pfnFlush1(device, D3DDDI_TRIM_MEMORY) == S_OK);
   CHECK(table.pfnSetMarkerMode(device, D3DDDIMT_NONE, 0) == S_OK && table.pfnSetMarker(device) == S_OK);
   CHECK(table.pfnSetMarkerMode(device, D3DDDIMT_PROFILE, 0) == D3DERR_NOTAVAILABLE);
   legacy9_upload_contracts(device, table);
   legacy9_retry_contract();
   legacy9_mip_edges_contract();
   D3DDDIARG_SETDEPTHSTENCIL depth{};
   CHECK(table.pfnSetDepthStencil(device, &depth) == S_OK);
   CHECK(table.pfnSetPixelShader(device, nullptr) == S_OK);
   CHECK(table.pfnSetVertexShaderFunc(device, nullptr) == S_OK);
   RECT scissor{0, 0, 96, 64};
   CHECK(table.pfnSetScissorRect(device, &scissor) == S_OK);
   scissor.left = -1;
   CHECK(table.pfnSetScissorRect(device, &scissor) == D3DERR_INVALIDCALL);

   D3DDDIARG_RENDERSTATE state{D3DDDIRS_LINEPATTERN, 0};
   CHECK(table.pfnSetRenderState(device, &state) == S_OK);
   state.State = D3DDDIRS_VERTEXBLEND;
   CHECK(table.pfnSetRenderState(device, &state) == S_OK);
   for (UINT id : {196u, 197u}) {
      state = {static_cast<D3DDDIRENDERSTATETYPE>(id), UINT32_MAX};
      CHECK(table.pfnSetRenderState(device, &state) == S_OK);
      state.Value = 1;
      CHECK(table.pfnSetRenderState(device, &state) == D3DERR_NOTAVAILABLE);
   }
   D3DDDIARG_TEXTURESTAGESTATE stage{0, D3DDDITSS_MAXANISOTROPY, 1};
   CHECK(table.pfnSetTextureStageState(device, &stage) == S_OK);
   stage = {1, D3DDDITSS_COLOROP, D3DTOP_DISABLE};
   CHECK(table.pfnSetTextureStageState(device, &stage) == S_OK);
   stage.Stage = UINT_MAX;
   CHECK(table.pfnSetTextureStageState(device, &stage) == E_INVALIDARG);
   CHECK(table.pfnDestroyDevice(device) == S_OK);
   CHECK(s.ktCreates == 1 && s.contexts == s.deadContexts && s.devices == s.deadDevices &&
         s.queues == s.deadQueues && s.objects == s.deadObjects && !s.errors);
   std::puts("PASS: D3D9 WDDM 1.3 upload, trim, counter and marker callbacks; padded overlap and deferred mip upload retry (CPU substitutes)");
   std::puts("PASS: D3D9 WDDM2 device table, fixed-function initialization and teardown (CPU substitutes)");
}

void device_contracts()
{
   legacy9_initialization_contract();
   for (bool withVideo : {false, true}) {
      Adapter adapter; D3DDDI_DEVICECALLBACKS kt = {};
      D3DWDDM2_0DDI_CORELAYER_DEVICECALLBACKS oldCore = {};
      s = {}; init(&adapter, &kt, &oldCore);
      D3D11DDI_CORELAYER_DEVICECALLBACKS core = {};
      core.pfnSetErrorCb = oldCore.pfnSetErrorCb;
      struct { UINT64 before; D3D11_1DDI_DEVICEFUNCS table; UINT64 after; } table = {};
      table.before = table.after = 0x12345678;
      constexpr size_t dxgi_size = sizeof(DXGI1_2_DDI_BASE_FUNCTIONS);
      alignas(DXGI1_2_DDI_BASE_FUNCTIONS) BYTE dxgi[dxgi_size + 16]; std::memset(dxgi, 0xcc, sizeof(dxgi));
      alignas(DeviceSlot) BYTE storage[sizeof(DeviceSlot)]; std::memset(storage, 0xab, sizeof(storage));
      PFND3D10DDI_RETRIEVESUBOBJECT retrieve = nullptr;
      D3D10DDIARG_CREATEDEVICE args = {};
      args.hDrvDevice = {storage}; args.hRTDevice = {H<HANDLE>(0x500)}; args.hRTCoreLayer = {H<HANDLE>(0x300)};
      args.Interface = D3D11_1_DDI_INTERFACE_VERSION;
      args.pKTCallbacks = &kt; args.p11UMCallbacks = &core; args.p11_1DeviceFuncs = &table.table;
      args.DXGIBaseDDI.pDXGIDDIBaseFunctions3 = reinterpret_cast<DXGI1_2_DDI_BASE_FUNCTIONS *>(dxgi + 8);
      args.ppfnRetrieveSubObject = &retrieve;
      D3D10DDIARG_CALCPRIVATEDEVICESIZE size = {args.Interface, 0, 0};
      if (withVideo) {
         adapter.videoEnabled = true;
         adapter.videoFirmware.resize(128000);
      }
      CHECK(CalcPrivateDeviceSize({&adapter}, &size) == sizeof(DeviceSlot));
      CHECK(CreateDevice({&adapter}, &args) == S_OK && s.ktCreates == 1 && !s.coreCreates && retrieve);
      for (unsigned i=0;i<8;++i) CHECK(dxgi[i] == 0xcc && dxgi[dxgi_size+8+i] == 0xcc);
      CHECK(!args.DXGIBaseDDI.pDXGIDDIBaseFunctions3->pfnPresentMultiplaneOverlay);
      struct { UINT64 before; D3D11_1DDI_VIDEODEVICEFUNCS table; UINT64 after; } video = {};
      video.before = video.after = 0x12345678;
      D3D11_1DDI_VIDEO_INPUT input = {FALSE,&video.table};
      BYTE output[3] = {0xa5,0xa5,0xa5};
      if (!withVideo) {
         CHECK(retrieve(args.hDrvDevice, D3D11_1DDI_VIDEO_FUNCTIONS, sizeof(input), &input, 1,
                        output+1) == E_NOINTERFACE);
         CHECK(!video.table.pfnCreateVideoDecoder && output[1] == 0xa5);
         table.table.pfnDestroyDevice(args.hDrvDevice);
         continue;
      }
      CHECK(retrieve(args.hDrvDevice, D3D11_1DDI_VIDEO_FUNCTIONS, sizeof(input), &input,
                     sizeof(video.table), output+1) == E_INVALIDARG);
      CHECK(!video.table.pfnCreateVideoDecoder && output[1] == 0xa5);
      CHECK(retrieve(args.hDrvDevice, D3D11_1DDI_VIDEO_FUNCTIONS, sizeof(input), &input, 1, output+1) == S_OK);
      CHECK(output[0] == 0xa5 && output[1] == 0 && output[2] == 0xa5);
      CHECK(video.table.pfnCreateVideoDecoder && !video.table.pfnVideoProcessorBlt);
      UINT profiles = 0; video.table.pfnGetVideoDecoderProfileCount(args.hDrvDevice, &profiles); CHECK(profiles == 1);
      input.Relocate = TRUE;
      CHECK(retrieve(args.hDrvDevice, D3D11_1DDI_VIDEO_FUNCTIONS, sizeof(input), &input, 1, output+1) == S_OK);
      CHECK(video.before == video.after && video.before == 0x12345678);
      CHECK(table.before == table.after && table.before == 0x12345678);
      table.table.pfnDestroyDevice(args.hDrvDevice);
      CHECK(s.contexts == s.deadContexts && s.queues == s.deadQueues && s.devices == s.deadDevices &&
            s.objects == s.deadObjects && !s.errors);
   }
   for (UINT failure = 0; failure <= 8; ++failure) {
      Adapter adapter; D3DDDI_DEVICECALLBACKS kt = {};
      D3DWDDM2_0DDI_CORELAYER_DEVICECALLBACKS core = {};
      s = {}; s.failure = failure; s.modern = true; init(&adapter, &kt, &core);
      struct { UINT64 before; D3DWDDM2_0DDI_DEVICEFUNCS table; UINT64 after; } table = {};
      struct { UINT64 before; DXGI1_4_DDI_BASE_FUNCTIONS table; UINT64 after; } dxgi = {};
      table.before = table.after = dxgi.before = dxgi.after = UINT64_C(0x123456789abcdef0);
      alignas(DeviceSlot) BYTE storage[sizeof(DeviceSlot)]; std::memset(storage, 0xab, sizeof(storage));
      PFND3D10DDI_RETRIEVESUBOBJECT subobject = nullptr;
      D3D10DDIARG_CREATEDEVICE args = {};
      args.hDrvDevice = {storage}; args.hRTDevice = {H<HANDLE>(0x500)}; args.hRTCoreLayer = {H<HANDLE>(0x300)};
      args.Interface = D3DWDDM2_0_DDI_INTERFACE_VERSION; args.Version = D3DWDDM2_0_DDI_BUILD_VERSION << 16;
      args.pKTCallbacks = &kt; args.pWDDM2_0UMCallbacks = &core; args.pWDDM2_0DeviceFuncs = &table.table;
      args.DXGIBaseDDI.pDXGIDDIBaseFunctions5 = &dxgi.table; args.ppfnRetrieveSubObject = &subobject;
      D3D10DDIARG_CALCPRIVATEDEVICESIZE size = {args.Interface, args.Version, 0};
      CHECK(CalcPrivateDeviceSize({&adapter}, &size) == sizeof(DeviceSlot));
      if (!failure) {
         size.Flags = 2 << D3D11DDI_CREATEDEVICE_FLAG_3DPIPELINESUPPORT_SHIFT;
         CHECK(!CalcPrivateDeviceSize({&adapter}, &size)); args.Flags = size.Flags;
         CHECK(CreateDevice({&adapter}, &args) == DXGI_ERROR_UNSUPPORTED && storage[0] == 0xab && !s.coreCreates);
         args.Flags = 0; args.Version -= 0x10000;
         CHECK(CreateDevice({&adapter}, &args) == DXGI_ERROR_UNSUPPORTED && !s.coreCreates);
         args.Version += 0x10000;
         auto cb = core.pfnCreateContextVirtualCb; core.pfnCreateContextVirtualCb = nullptr;
         CHECK(CreateDevice({&adapter}, &args) == DXGI_ERROR_UNSUPPORTED && !s.ktCreates);
         core.pfnCreateContextVirtualCb = cb;
      }
      HRESULT hr = CreateDevice({&adapter}, &args);
      CHECK((SUCCEEDED(hr) == !failure) && s.coreCreates == 1 && !s.ktCreates);
      if (!failure) {
         auto *dev = D(args.hDrvDevice);
         CHECK(dev->hContext == H<HANDLE>(0x400) && dev->createContextVirtual == core.pfnCreateContextVirtualCb);
         CHECK(table.table.pfnDestroyDevice && table.table.pfnFlush && table.table.pfnCreateQuery);
         CHECK(dxgi.table.pfnPresentMultiplaneOverlay1 && dxgi.table.pfnTrimResidencySet && subobject);
         BYTE output[16]; std::memset(output, 0xcc, sizeof(output));
         CHECK(subobject(args.hDrvDevice, D3DWDDM2_0DDI_VIDEO_FUNCTIONS, 0, nullptr, sizeof(output), output) == E_NOINTERFACE);
         CHECK(output[0] == 0xcc && output[15] == 0xcc);
         table.table.pfnDestroyDevice(args.hDrvDevice);
      } else CHECK(!table.table.pfnDraw && !dxgi.table.pfnPresent && !subobject);
      CHECK(table.before == table.after && table.before == dxgi.before && dxgi.before == dxgi.after);
      CHECK(s.contexts == s.deadContexts && s.queues == s.deadQueues && s.devices == s.deadDevices &&
            s.objects == s.deadObjects && !s.errors);
   }
   std::puts("PASS: full WDDM2 device creation pairs typed core context/3D+copy flags with DXGI1.4 MPO1; no legacy callback, higher-FL or old-build admission; eight failures unwind and leave tables untouched (CPU control)");
}
