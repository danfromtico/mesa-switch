/* SPDX-License-Identifier: MIT
 * CPU controls of the production DDI code. No Vulkan driver is loaded.
 */
#include "tgd3d.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <type_traits>
#include <d3dcompiler.h>
#include "gallium/targets/d3d10umd/tests/tri_vs_4_0.h"
#include "gallium/targets/d3d10umd/tests/tri_ps_4_0.h"

using namespace tgd3d;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%u: %s\n", __FILE__, __LINE__, #x); std::exit(1); } } while (0)
template<typename T> static T handle(uintptr_t n) { return (T)n; }
template<typename T> struct Stub;
template<typename R, typename... A> struct Stub<R(VKAPI_PTR *)(A...)> {
   static R VKAPI_CALL call(A...) { if constexpr (!std::is_void_v<R>) return R{}; }
};
static uint64_t completedValue;
static unsigned signals, submissions, gates, cpuWaits, cpuSignals, barriers, viewsDestroyed;
static unsigned failAt, layouts, descriptorSets, updates, setErrorCalls;
static HRESULT lastError;
static std::vector<unsigned> events;
static std::vector<uint32_t> cbBindings;
static VkDeviceSize cbOffsets[3];
static const char *shaderOutput;
static Shader *failedShader;
static void *shaderAllocations[3];
static unsigned shaderAllocationFrees;
static VkResult shaderFailure;
static unsigned shaderBackendCalls;

/* Observe the real vector deallocations of a failed CreateShader. Do not
 * inspect its private storage after its C++ lifetime has ended. */
void operator delete(void *p) noexcept
{
   for (void *&allocation : shaderAllocations) {
      if (p && allocation == p) {
         allocation = nullptr;
         ++shaderAllocationFrees;
      }
   }
   std::free(p);
}

void operator delete(void *p, size_t) noexcept { ::operator delete(p); }

static Device make_device(Adapter *adapter)
{
   Device dev;
   dev.adapter = adapter;
   adapter->vk.caps.cbSlots = D3D_CB_SLOTS;
   adapter->vk.caps.geometryShader = true;
   adapter->vk.caps.maxViewports = 16;
   dev.caps = &adapter->vk.caps;
   dev.vkdev = handle<VkDevice>(1);
   dev.queue = handle<VkQueue>(2);
   dev.fence = handle<VkSemaphore>(3);
#ifndef TGD3D_CPU_SYNC_CONTROL
   dev.ingress = handle<VkSemaphore>(4);
   dev.runtimeFence = 0x20;
   dev.runtimeIngress = 0x21;
#endif
   dev.hContext = handle<HANDLE>(0x3000);
   dev.hRTDevice.handle = handle<HANDLE>(0x4000);
#define X(n) dev.vk.n = Stub<PFN_vk##n>::call;
   TGD3D_VK_DEVICE_FUNCS(X)
   TGD3D_VK_OPTIONAL_DEVICE_FUNCS(X)
#undef X
   dev.um.pfnSetErrorCb = [](D3D10DDI_HRTCORELAYER, HRESULT hr) { ++setErrorCalls; lastError = hr; };
   dev.vk.AllocateCommandBuffers = [](VkDevice, const VkCommandBufferAllocateInfo *, VkCommandBuffer *out) {
      *out = handle<VkCommandBuffer>(0x100); return VK_SUCCESS;
   };
   dev.vk.GetSemaphoreCounterValue = [](VkDevice, VkSemaphore, uint64_t *out) {
      *out = completedValue; return VK_SUCCESS;
   };
   dev.vk.WaitSemaphores = [](VkDevice, const VkSemaphoreWaitInfo *info, uint64_t) {
      ++cpuWaits; completedValue = *info->pValues; return VK_SUCCESS;
   };
   dev.vk.SignalSemaphore = [](VkDevice, const VkSemaphoreSignalInfo *) {
      ++cpuSignals; return VK_SUCCESS;
   };
   dev.vk.CmdPipelineBarrier2 = [](VkCommandBuffer cmd, const VkDependencyInfo *info) {
      CHECK(cmd && (info->memoryBarrierCount || info->imageMemoryBarrierCount)); ++barriers;
   };
   dev.kt.pfnSignalSynchronizationObjectFromGpuCb = [](HANDLE device,
         const D3DDDICB_SIGNALSYNCHRONIZATIONOBJECTFROMGPU *info) -> HRESULT {
      CHECK(device == handle<HANDLE>(0x4000) && info->hContext == handle<HANDLE>(0x3000));
      CHECK(info->ObjectCount == 1 && info->ObjectHandleArray[0] == 0x21);
      CHECK(info->MonitoredFenceValueArray[0] == submissions + 1);
      ++signals; events.push_back(1); return failAt == 1 ? E_FAIL : S_OK;
   };
   dev.vk.QueueSubmit2 = [](VkQueue, uint32_t count, const VkSubmitInfo2 *info, VkFence) {
      CHECK(count == 1 && info->commandBufferInfoCount == 1 && info->signalSemaphoreInfoCount == 1);
      CHECK(info->pSignalSemaphoreInfos[0].value == submissions + 1);
      CHECK(info->pSignalSemaphoreInfos[0].semaphore == handle<VkSemaphore>(3));
#ifdef TGD3D_CPU_SYNC_CONTROL
      CHECK(info->waitSemaphoreInfoCount == (submissions ? 1u : 0u));
#else
      CHECK(info->waitSemaphoreInfoCount == (submissions ? 2u : 1u));
#endif
      if (submissions) {
         CHECK(info->pWaitSemaphoreInfos[0].semaphore == handle<VkSemaphore>(3));
         CHECK(info->pWaitSemaphoreInfos[0].value == submissions);
      }
#ifndef TGD3D_CPU_SYNC_CONTROL
      const auto &wait = info->pWaitSemaphoreInfos[info->waitSemaphoreInfoCount - 1];
      CHECK(wait.semaphore == handle<VkSemaphore>(4) && wait.value == submissions + 1);
      CHECK(wait.stageMask == VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
#endif
      ++submissions; events.push_back(2);
      return failAt == 2 ? VK_ERROR_DEVICE_LOST : VK_SUCCESS;
   };
   dev.kt.pfnWaitForSynchronizationObjectFromGpuCb = [](HANDLE,
         const D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMGPU *info) -> HRESULT {
      CHECK(info->hContext == handle<HANDLE>(0x3000) && info->ObjectCount == 1);
      CHECK(info->ObjectHandleArray[0] == 0x20 && info->MonitoredFenceValueArray[0] == submissions);
      ++gates; events.push_back(3); return failAt == 3 ? E_FAIL : S_OK;
   };
   dev.vk.DestroyImageView = [](VkDevice, VkImageView, const VkAllocationCallbacks *) {
      ++viewsDestroyed; events.push_back(4);
   };
   dev.vk.DestroyImage = [](VkDevice, VkImage, const VkAllocationCallbacks *) { events.push_back(5); };
   dev.vk.FreeMemory = [](VkDevice, VkDeviceMemory, const VkAllocationCallbacks *) { events.push_back(6); };
   return dev;
}

static void reset()
{
   completedValue = 0;
   signals = submissions = gates = cpuWaits = cpuSignals = barriers = viewsDestroyed = 0;
   failAt = layouts = descriptorSets = updates = setErrorCalls = 0;
   lastError = S_OK; events.clear(); cbBindings.clear();
}

static void submission_contract()
{
   Adapter adapter;
   reset(); Device dev = make_device(&adapter);
#ifdef TGD3D_CPU_SYNC_CONTROL
   dev.nvk = true;
   dev.vk.CreateSemaphore = [](VkDevice, const VkSemaphoreCreateInfo *info,
                               const VkAllocationCallbacks *, VkSemaphore *out) {
      const auto *type = static_cast<const VkSemaphoreTypeCreateInfo *>(info->pNext);
      CHECK(type && type->semaphoreType == VK_SEMAPHORE_TYPE_TIMELINE);
      *out = handle<VkSemaphore>(3); return VK_SUCCESS;
   };
   /* No runtime create/import callbacks are supplied. The diagnostic path
    * must not need them even for the native adapter. */
   CHECK(SUCCEEDED(sync_create(&dev)));
   CHECK(dev.fence && !dev.ingress && !dev.runtimeFence && !dev.runtimeIngress);
   for (unsigned i = 0; i < 4; i++) {
      CHECK(cmd_begin(&dev)); CHECK(SUCCEEDED(flush(&dev, false)));
   }
   CHECK(dev.submitted == 4 && !dev.gated && !cpuWaits && !signals && !gates);
   CHECK(events == std::vector<unsigned>({2,2,2,2}));
   CHECK(SUCCEEDED(flush(&dev, true)) && cpuWaits == 1 && completedValue == 4);
   CHECK(dev.submittedCommands.empty() && !cpuSignals && !signals && !gates);
   reset(); dev = make_device(&adapter); failAt = 2;
   D3D10DDI_DEVICEFUNCS devTable = {}; device_fill_table(&devTable);
   CHECK(cmd_begin(&dev)); devTable.pfnFlush({&dev});
   CHECK(setErrorCalls == 1 && lastError == D3DDDIERR_DEVICEREMOVED);
   CHECK(FAILED(dev.lost) && !cpuWaits && !cpuSignals && !signals && !gates);
   std::puts("PASS: CPU sync control has no cross-context fence; ordinary Flush completes, polling does not block");
#else
   for (unsigned i = 0; i < 4; i++) {
      CHECK(cmd_begin(&dev));
      CHECK(SUCCEEDED(flush(&dev, false)));
   }
   CHECK(dev.submitted == 4 && dev.gated == 4 && !cpuWaits && !cpuSignals);
   CHECK(events == std::vector<unsigned>({1,2,3,1,2,3,1,2,3,1,2,3}));
   CHECK(barriers >= 8);
   completedValue = 4; retire(&dev);
   CHECK(dev.submittedCommands.empty());
   D3D10DDI_DEVICEFUNCS devTable = {}; device_fill_table(&devTable);
   for (unsigned failure = 1; failure <= 3; failure++) {
      reset(); dev = make_device(&adapter); failAt = failure;
      CHECK(cmd_begin(&dev)); devTable.pfnFlush({&dev});
      CHECK(setErrorCalls == 1 && lastError == D3DDDIERR_DEVICEREMOVED);
      CHECK(FAILED(dev.lost) && !cpuSignals);
      CHECK(submissions == (failure >= 2) && gates == (failure == 3));
      CHECK(dev.submitted == (failure == 3));
      const unsigned previous = signals;
      devTable.pfnFlush({&dev});
      CHECK(signals == previous && setErrorCalls == 2 && lastError == D3DDDIERR_DEVICEREMOVED);
   }
   std::puts("PASS: every submit is bidirectional; failures never fabricate completion");
#endif
}

static void nonblocking_map()
{
   reset(); Adapter adapter; Device dev = make_device(&adapter);
   D3D10DDI_DEVICEFUNCS table = {}; resource_fill_table(&table);
   Chunk chunk; uint8_t bytes[64] = {}; chunk.cpu = bytes;
   Resource res; res.dev = &dev; res.size = sizeof(bytes); res.slice = {&chunk, 0, sizeof(bytes)};
   CHECK(cmd_begin(&dev)); res.sliceLastWrite = res.sliceLastUse = 1;
   D3D10DDI_MAPPED_SUBRESOURCE out = {};
   table.pfnStagingResourceMap({&dev}, {&res}, 0, D3D10_DDI_MAP_READ,
                              D3D10_DDI_MAP_FLAG_DONOTWAIT, &out);
   CHECK(!out.pData && lastError == DXGI_DDI_ERR_WASSTILLDRAWING);
   CHECK(!cpuWaits && dev.submitted == 1 && SUCCEEDED(dev.lost));
   completedValue = 1; lastError = S_OK;
   table.pfnStagingResourceMap({&dev}, {&res}, 0, D3D10_DDI_MAP_READ,
                              D3D10_DDI_MAP_FLAG_DONOTWAIT, &out);
   CHECK(out.pData == bytes && out.RowPitch == sizeof(bytes) && lastError == S_OK);
   std::puts("PASS: DONOTWAIT submits progress without a CPU wait");
}

static void incomplete_fence_keeps_resources()
{
   Adapter adapter;
   for (VkResult result : {VK_TIMEOUT, VK_ERROR_DEVICE_LOST}) {
      reset(); Device dev = make_device(&adapter);
      CHECK(cmd_begin(&dev)); CHECK(SUCCEEDED(flush(&dev, false)));
      defer(&dev, DeferredType::ImageView, 0x500, 1);
      dev.vk.WaitSemaphores = result == VK_TIMEOUT ?
         +[](VkDevice, const VkSemaphoreWaitInfo *, uint64_t timeout) -> VkResult {
            CHECK(timeout == UINT64_C(1000000000)); ++cpuWaits; return VK_TIMEOUT;
         } : +[](VkDevice, const VkSemaphoreWaitInfo *, uint64_t) -> VkResult {
            ++cpuWaits; return VK_ERROR_DEVICE_LOST;
         };
      CHECK(wait_sequence(&dev, 1, TGD3D_WAIT_DESTROY) == DXGI_ERROR_DEVICE_REMOVED);
      CHECK(lastError == D3DDDIERR_DEVICEREMOVED);
      CHECK(cpuWaits == (result == VK_TIMEOUT ? 30u : 1u));
      CHECK(!completedValue && !cpuSignals && !viewsDestroyed);
      CHECK(dev.submittedCommands.size() == 1 && dev.deferred.size() == 1);
      const unsigned previous = cpuWaits;
      CHECK(FAILED(wait_sequence(&dev, 1, TGD3D_WAIT_READBACK)) && cpuWaits == previous);
   }
   std::puts("PASS: timeout and device loss never complete or retire in-flight resources");
}

static void device_error_contract()
{
   Adapter adapter;
   for (HRESULT error : {DXGI_ERROR_DEVICE_REMOVED, DXGI_ERROR_DEVICE_RESET,
                         D3DDDIERR_DEVICEREMOVED}) {
      reset(); Device dev = make_device(&adapter);
      TG_ERROR(&dev, error);
      CHECK(dev.lost == error && lastError == D3DDDIERR_DEVICEREMOVED);
      CHECK(!cmd_begin(&dev) && FAILED(flush(&dev, true)) && !submissions);
   }
   reset(); Device dev = make_device(&adapter);
   TG_ERROR(&dev, E_INVALIDARG);
   CHECK(lastError == E_INVALIDARG && SUCCEEDED(dev.lost));
   std::puts("PASS: device loss uses the D3D DDI callback code and blocks new submissions");
}

static void rotated_views()
{
   reset(); Adapter adapter; Device dev = make_device(&adapter);
   Resource a, b; a.dev = b.dev = &dev;
   a.kind = b.kind = ResKind::Image;
   a.storage.image = handle<VkImage>(10); b.storage.image = handle<VkImage>(11);
   a.storage.memory = handle<VkDeviceMemory>(20); b.storage.memory = handle<VkDeviceMemory>(21);
   a.storage.lastUse = b.storage.lastUse = 0;
   View va, vb; va.res = &a; vb.res = &b;
   va.cache[0] = {a.storage.image, handle<VkImageView>(30), false};
   vb.cache[0] = {a.storage.image, handle<VkImageView>(31), false};
   dev.views = {&va, &vb};
   defer(&dev, DeferredType::ImageView, 32, 0, a.storage.image);
   DXGI1_1_DDI_BASE_FUNCTIONS table = {}; dxgi_fill_table(&table, true);
   DXGI_DDI_HRESOURCE resources[2] = {(DXGI_DDI_HRESOURCE)&a, (DXGI_DDI_HRESOURCE)&b};
   DXGI_DDI_ARG_ROTATE_RESOURCE_IDENTITIES rotate = {};
   rotate.hDevice = (DXGI_DDI_HDEVICE)&dev; rotate.Resources = 2; rotate.pResources = resources;
   CHECK(SUCCEEDED(table.pfnRotateResourceIdentities(&rotate)));
   CHECK(b.storage.image == handle<VkImage>(10));
   storage_destroy(&dev, b.storage, true);
   CHECK(events == std::vector<unsigned>({4,4,4,5,6}) && dev.deferred.empty());
   CHECK(!va.cache[0].view && !vb.cache[0].view);
   events.clear();
   a.storage.lastUse = 2;
   va.cache[0] = {a.storage.image, handle<VkImageView>(33), false};
   storage_destroy(&dev, a.storage, false);
   CHECK(events.empty() && dev.deferred.size() == 3);
   completedValue = 1; retire(&dev); CHECK(events.empty());
   completedValue = 2; retire(&dev);
   CHECK(events == std::vector<unsigned>({4,5,6}) && dev.deferred.empty());
   std::puts("PASS: rotated cached/deferred views die before their image and memory");
}

static void descriptor_contract()
{
   reset(); Adapter adapter; Device dev = make_device(&adapter);
   dev.vk.CreateDescriptorSetLayout = [](VkDevice, const VkDescriptorSetLayoutCreateInfo *info,
                                        const VkAllocationCallbacks *, VkDescriptorSetLayout *out) {
      CHECK(!(info->flags & VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR));
      if (!layouts) CHECK(info->bindingCount == 42);
      *out = handle<VkDescriptorSetLayout>(100 + layouts++); return VK_SUCCESS;
   };
   dev.vk.CreateDescriptorPool = [](VkDevice, const VkDescriptorPoolCreateInfo *,
                                   const VkAllocationCallbacks *, VkDescriptorPool *out) {
      *out = handle<VkDescriptorPool>(200); return VK_SUCCESS;
   };
   dev.vk.AllocateDescriptorSets = [](VkDevice, const VkDescriptorSetAllocateInfo *, VkDescriptorSet *out) {
      *out = handle<VkDescriptorSet>(300 + descriptorSets++); return VK_SUCCESS;
   };
   dev.vk.UpdateDescriptorSets = [](VkDevice, uint32_t count, const VkWriteDescriptorSet *writes,
                                   uint32_t, const VkCopyDescriptorSet *) {
      CHECK(count == 3); cbBindings.clear(); ++updates;
      for (unsigned i = 0; i < count; i++) {
         CHECK(writes[i].descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
         cbBindings.push_back(writes[i].dstBinding); cbOffsets[i] = writes[i].pBufferInfo->offset;
      }
   };
   CHECK(SUCCEEDED(device_layouts_create(&dev)) && layouts == 3);
   Shader shaders[3]; Chunk chunk; chunk.buffer = handle<VkBuffer>(500);
   Resource cb[3];
   for (unsigned i = 0; i < 3; i++) {
      shaders[i].dev = &dev; shaders[i].stage = (Stage)i;
      shaders[i].cbMask = 1 << 13; shaders[i].variants.push_back({0, handle<VkShaderEXT>(600 + i)});
      dev.stage[i].shader = &shaders[i];
      cb[i].slice = {&chunk, i * 256, 64}; cb[i].size = 64;
      dev.stage[i].cb[13] = &cb[i];
   }
   Resource target; target.dev = &dev; target.kind = ResKind::Image;
   target.storage.image = handle<VkImage>(700); target.storage.layoutReady = true;
   View view; view.dev = &dev; view.res = &target; view.type = ViewType::RTV;
   view.width = view.height = 32; view.layers = 1;
   view.cache[0] = {target.storage.image, handle<VkImageView>(701), false};
   dev.rtv[0] = &view; dev.viewportCount = 1; dev.viewports[0].Width = dev.viewports[0].Height = 32;
   D3D10DDI_DEVICEFUNCS table = {}; draw_fill_table(&table);
   table.pfnDraw({&dev}, 3, 0);
   CHECK(updates == 1 && cbBindings == std::vector<uint32_t>({13,27,41}));
   CHECK(cbOffsets[0] == 0 && cbOffsets[1] == 256 && cbOffsets[2] == 512);
   table.pfnDraw({&dev}, 3, 0); CHECK(updates == 1);
   cb[0].slice.offset = 768; dev.stage[0].cbDirty = 1 << 13;
   table.pfnDraw({&dev}, 3, 0);
   CHECK(updates == 2 && cbOffsets[0] == 768 && cbOffsets[1] == 256 && cbOffsets[2] == 512);
   std::puts("PASS: all 42 constant-buffer slots; immutable full snapshots after one-slot changes");
}

static VkResult drawFailure;
static unsigned drawFailureAt, poolCreates, setAllocates, imageViews, drawCalls;

static void draw_error_contract()
{
   D3D10DDI_DEVICEFUNCS table = {}; draw_fill_table(&table);
   for (unsigned entry = 0; entry < 4; entry++)
   for (unsigned where = 0; where < 3; where++)
   for (VkResult failure : {VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                            VK_ERROR_DEVICE_LOST}) {
      reset(); Adapter adapter; Device dev = make_device(&adapter);
      drawFailure = failure; drawFailureAt = where;
      poolCreates = setAllocates = imageViews = drawCalls = 0;
      dev.vk.CreateDescriptorPool = [](VkDevice, const VkDescriptorPoolCreateInfo *,
                                      const VkAllocationCallbacks *, VkDescriptorPool *out) {
         ++poolCreates; *out = VK_NULL_HANDLE;
         CHECK(drawFailureAt == 0); return drawFailure;
      };
      dev.vk.AllocateDescriptorSets = [](VkDevice, const VkDescriptorSetAllocateInfo *, VkDescriptorSet *out) {
         ++setAllocates; *out = VK_NULL_HANDLE;
         CHECK(drawFailureAt == 1); return drawFailure;
      };
      dev.vk.CreateImageView = [](VkDevice, const VkImageViewCreateInfo *,
                                 const VkAllocationCallbacks *, VkImageView *out) {
         ++imageViews; *out = VK_NULL_HANDLE;
         CHECK(drawFailureAt == 2); return drawFailure;
      };
      dev.vk.CmdDraw = [](VkCommandBuffer, uint32_t, uint32_t, uint32_t, uint32_t) { ++drawCalls; };
      dev.vk.CmdDrawIndexed = [](VkCommandBuffer, uint32_t, uint32_t, uint32_t, int32_t, uint32_t) { ++drawCalls; };
      if (where == 1) dev.framePools.push_back(handle<VkDescriptorPool>(200));
      Shader vs; vs.dev = &dev; vs.stage = STAGE_VS; vs.cbMask = 1;
      vs.variants.push_back({0, handle<VkShaderEXT>(600)}); dev.stage[STAGE_VS].shader = &vs;
      Chunk chunk; chunk.buffer = handle<VkBuffer>(500);
      Resource buffer; buffer.slice = {&chunk, 0, 64}; buffer.size = 64;
      dev.stage[STAGE_VS].cb[0] = dev.indexBuffer = &buffer;
      Resource target; target.dev = &dev; target.kind = ResKind::Image;
      target.storage.image = handle<VkImage>(700); target.storage.layoutReady = true;
      View view; view.dev = &dev; view.res = &target; view.type = ViewType::RTV;
      view.width = view.height = 32;
      if (where != 2) view.cache[0] = {target.storage.image, handle<VkImageView>(701), false};
      dev.rtv[0] = &view; dev.viewportCount = 1; dev.viewports[0].Width = dev.viewports[0].Height = 32;
      auto draw = [&] {
         switch (entry) {
         case 0: table.pfnDraw({&dev}, 3, 0); break;
         case 1: table.pfnDrawIndexed({&dev}, 3, 0, 0); break;
         case 2: table.pfnDrawInstanced({&dev}, 3, 2, 0, 0); break;
         case 3: table.pfnDrawIndexedInstanced({&dev}, 3, 2, 0, 0, 0); break;
         }
      };
      draw();
      CHECK(setErrorCalls == 1 && lastError == D3DDDIERR_DEVICEREMOVED);
      CHECK(dev.lost == D3DDDIERR_DEVICEREMOVED && !drawCalls);
      CHECK(poolCreates + setAllocates + imageViews == 1);
      draw();
      CHECK(setErrorCalls == 1 && !drawCalls && poolCreates + setAllocates + imageViews == 1);
   }
   reset(); Adapter adapter; Device dev = make_device(&adapter);
   /* A no-op Draw must leave the ordinary allocation error contract intact. */
   table.pfnDraw({&dev}, 0, 0);
   dev.vk.CreateDescriptorPool = [](VkDevice, const VkDescriptorPoolCreateInfo *,
                                   const VkAllocationCallbacks *, VkDescriptorPool *) {
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   };
   CHECK(!descriptor_set_alloc(&dev, 0) && lastError == E_OUTOFMEMORY && SUCCEEDED(dev.lost));
   setErrorCalls = 0;
   table.pfnDrawAuto({&dev});
   CHECK(lastError == D3DDDIERR_DEVICEREMOVED && dev.lost == D3DDDIERR_DEVICEREMOVED);
   table.pfnDrawAuto({&dev}); CHECK(setErrorCalls == 1);
   for (VkResult full : {VK_ERROR_OUT_OF_POOL_MEMORY, VK_ERROR_FRAGMENTED_POOL}) {
      reset(); Adapter a; Device d = make_device(&a);
      drawFailure = full; poolCreates = setAllocates = 0;
      d.framePools.push_back(handle<VkDescriptorPool>(200));
      d.vk.AllocateDescriptorSets = [](VkDevice, const VkDescriptorSetAllocateInfo *, VkDescriptorSet *out) {
         if (++setAllocates == 1) { *out = VK_NULL_HANDLE; return drawFailure; }
         *out = handle<VkDescriptorSet>(300); return VK_SUCCESS;
      };
      d.vk.CreateDescriptorPool = [](VkDevice, const VkDescriptorPoolCreateInfo *,
                                    const VkAllocationCallbacks *, VkDescriptorPool *out) {
         ++poolCreates; *out = handle<VkDescriptorPool>(201); return VK_SUCCESS;
      };
      CHECK(descriptor_set_alloc(&d, 0) == handle<VkDescriptorSet>(300));
      CHECK(setAllocates == 2 && poolCreates == 1 && !setErrorCalls && SUCCEEDED(d.lost));
   }
   std::puts("PASS: all Draw DDIs report only removal on allocation/view failure and stop further work; non-Draw OOM remains recoverable (CPU control)");
}

static unsigned regionCopies;
static VkImageCopy regionImage;
static VkBufferImageCopy regionTransfer;

static void empty_box_contract()
{
   const D3D10_DDI_BOX boxes[] = {
      {8,0,0,4,1,1}, {4,0,0,4,1,1},
      {0,5,0,1,3,1}, {0,3,0,1,3,1},
      {0,0,1,1,1,0}, {0,0,1,1,1,1}, {0,0,0,0,0,0},
   };
   const ResKind kinds[][2] = {
      {ResKind::Image, ResKind::Image}, {ResKind::Image, ResKind::Staging},
      {ResKind::Staging, ResKind::Image}, {ResKind::Staging, ResKind::Staging},
      {ResKind::Buffer, ResKind::Buffer},
   };
   D3D10DDI_DEVICEFUNCS table = {}; resource_fill_table(&table);
   for (const auto &kind : kinds) {
      reset(); Adapter adapter; Device dev = make_device(&adapter);
      regionCopies = 0;
      dev.submitted = dev.gated = 1; /* Existing work must remain untouched by a no-op. */
      dev.vk.CmdCopyImage = [](VkCommandBuffer, VkImage, VkImageLayout, VkImage, VkImageLayout,
                              uint32_t count, const VkImageCopy *r) {
         CHECK(count == 1); ++regionCopies; regionImage = *r;
      };
      dev.vk.CmdCopyImageToBuffer = [](VkCommandBuffer, VkImage, VkImageLayout, VkBuffer,
                                      uint32_t count, const VkBufferImageCopy *r) {
         CHECK(count == 1); ++regionCopies; regionTransfer = *r;
      };
      dev.vk.CmdCopyBufferToImage = [](VkCommandBuffer, VkBuffer, VkImage, VkImageLayout,
                                      uint32_t count, const VkBufferImageCopy *r) {
         CHECK(count == 1); ++regionCopies; regionTransfer = *r;
      };
      dev.vk.CmdCopyBuffer = [](VkCommandBuffer, VkBuffer, VkBuffer, uint32_t, const VkBufferCopy *) {
         ++regionCopies;
      };
      std::vector<uint8_t> source(9216, 0x19), destination(9216, 0xa5), upload(8840, 0x3c);
      Chunk sc, dc; sc.cpu = source.data(); dc.cpu = destination.data();
      sc.buffer = handle<VkBuffer>(100); dc.buffer = handle<VkBuffer>(101);
      Resource src, dst;
      auto init = [&](Resource &r, ResKind k, Chunk *chunk, uintptr_t image) {
         r.dev = &dev; r.kind = k; r.dim = k == ResKind::Buffer ? D3D10DDIRESOURCE_BUFFER : D3D10DDIRESOURCE_TEXTURE3D;
         r.format = DXGI_FORMAT_B8G8R8A8_UNORM; r.width = 65; r.height = 17; r.depth = 2;
         r.storage.image = handle<VkImage>(image); r.storage.layoutReady = true;
         r.storage.lastUse = r.sliceLastUse = r.sliceLastWrite = 1;
         r.aspects = VK_IMAGE_ASPECT_COLOR_BIT;
         r.slice = {chunk, 16, 8840}; r.size = 8840;
         r.layout.push_back({0, 260, 4420, 65, 17, 2}); r.maps.resize(1);
      };
      init(src, kind[0], &sc, 200); init(dst, kind[1], &dc, 201);
      for (const auto &box : boxes) {
         table.pfnResourceCopyRegion({&dev}, {&dst}, 0, 0, 0, 0, {&src}, 0, &box);
         CHECK(!regionCopies && !dev.cmd && !setErrorCalls && !cpuWaits && !submissions);
         table.pfnResourceUpdateSubresourceUP({&dev}, {&dst}, 0, &box, upload.data(), 260, 4420);
         CHECK(!regionCopies && !dev.cmd && !setErrorCalls && !cpuWaits && !submissions);
         CHECK(destination == std::vector<uint8_t>(9216, 0xa5));
         CHECK(dev.submitted == 1 && !completedValue && dev.deferredSlices.empty());
         CHECK(!dev.stats.copyBytes && !dev.stats.uploadBytes);
      }
      /* NULL still means the full nonempty source, not an empty box. */
      completedValue = 1;
      table.pfnResourceCopyRegion({&dev}, {&dst}, 0, 0, 0, 0, {&src}, 0, nullptr);
      if (kind[0] == ResKind::Staging && kind[1] == ResKind::Staging) {
         CHECK(!regionCopies);
         for (size_t i = 0; i < destination.size(); i++)
            CHECK(destination[i] == (i >= 16 && i < 8856 ? 0x19 : 0xa5));
      } else {
         CHECK(regionCopies == 1);
         if (kind[0] == ResKind::Image && kind[1] == ResKind::Image)
            CHECK(regionImage.extent.width == 65 && regionImage.extent.height == 17 && regionImage.extent.depth == 2);
         else if (kind[0] != ResKind::Buffer)
            CHECK(regionTransfer.imageExtent.width == 65 && regionTransfer.imageExtent.height == 17 &&
                  regionTransfer.imageExtent.depth == 2 && regionTransfer.bufferOffset == 16 &&
                  regionTransfer.bufferRowLength == 65 && regionTransfer.bufferImageHeight == 17);
      }
      CHECK(!setErrorCalls);
   }
   std::puts("PASS: empty/inverted copy and update boxes touch no commands, waits or memory; NULL full 65x17x2 copies retain exact layout (CPU control)");
}

static uint32_t read32(const BYTE *p) { uint32_t v; memcpy(&v, p, 4); return v; }

static void query_contract()
{
   reset(); Adapter adapter; Device dev = make_device(&adapter);
   D3D10DDI_DEVICEFUNCS table = {}; query_fill_table(&table);
   dev.vk.CreateQueryPool = [](VkDevice, const VkQueryPoolCreateInfo *,
                               const VkAllocationCallbacks *, VkQueryPool *out) {
      static uintptr_t next = 1; *out = handle<VkQueryPool>(next++); return VK_SUCCESS;
   };
   dev.vk.GetQueryPoolResults = [](VkDevice, VkQueryPool, uint32_t, uint32_t count,
                                   size_t size, void *data, VkDeviceSize stride, VkQueryResultFlags) {
      CHECK(size == count * 8 && stride == 8);
      for (uint32_t i = 0; i < count; i++) static_cast<uint64_t *>(data)[i] = 1;
      return VK_SUCCESS;
   };
   alignas(Query) BYTE storage[sizeof(Query)]; D3D10DDIARG_CREATEQUERY desc = {};
   desc.Query = D3D10DDI_QUERY_OCCLUSION;
   table.pfnCreateQuery({&dev}, &desc, {storage}, {});
   table.pfnQueryBegin({&dev}, {storage});
   VkCommandBuffer cmd = cmd_draw(&dev);
   for (unsigned i = 0; i < 129; i++) { query_pass_begin(&dev, cmd); query_pass_end(&dev, cmd); }
   table.pfnQueryEnd({&dev}, {storage});
   uint64_t value = 0;
   table.pfnQueryGetData({&dev}, {storage}, &value, sizeof(value), D3D10_DDI_GET_DATA_DO_NOT_FLUSH);
   CHECK(lastError == DXGI_DDI_ERR_WASSTILLDRAWING && !submissions && !cpuWaits);
   lastError = S_OK;
   table.pfnQueryGetData({&dev}, {storage}, &value, sizeof(value), 0);
   CHECK(lastError == DXGI_DDI_ERR_WASSTILLDRAWING && submissions == 1 && !cpuWaits);
   completedValue = 1; lastError = S_OK;
   table.pfnQueryGetData({&dev}, {storage}, &value, sizeof(value), 0);
   CHECK(lastError == S_OK && value == 129);
   /* A second End without Begin describes a fresh empty interval. */
   table.pfnQueryEnd({&dev}, {storage});
   value = UINT64_MAX;
   table.pfnQueryGetData({&dev}, {storage}, &value, sizeof(value), 0);
   CHECK(lastError == S_OK && value == 0);
   table.pfnDestroyQuery({&dev}, {storage}); retire(&dev);
   CHECK(dev.deferred.empty());
   std::puts("PASS: nonblocking query polling; all 129 render passes counted across pool rollover");
}

static unsigned queryPoolCalls, queryFailPoolCall, queryResultCalls, queryBeginCalls, timestampCalls;
static VkResult queryPoolFailure, queryReadResult;

static void query_backend(Device &dev)
{
   queryPoolCalls = queryFailPoolCall = queryResultCalls = queryBeginCalls = timestampCalls = 0;
   queryPoolFailure = VK_ERROR_OUT_OF_DEVICE_MEMORY;
   queryReadResult = VK_NOT_READY;
   dev.vk.CreateQueryPool = [](VkDevice, const VkQueryPoolCreateInfo *,
                               const VkAllocationCallbacks *, VkQueryPool *out) {
      ++queryPoolCalls;
      if (queryPoolCalls == queryFailPoolCall) {
         *out = VK_NULL_HANDLE;
         return queryPoolFailure;
      }
      *out = handle<VkQueryPool>(queryPoolCalls);
      return VK_SUCCESS;
   };
   dev.vk.CmdBeginQuery = [](VkCommandBuffer, VkQueryPool pool, uint32_t slot, VkQueryControlFlags) {
      CHECK(pool && slot < QUERY_SLOTS); ++queryBeginCalls;
   };
   dev.vk.CmdWriteTimestamp = [](VkCommandBuffer, VkPipelineStageFlagBits, VkQueryPool pool, uint32_t) {
      CHECK(pool); ++timestampCalls;
   };
   dev.vk.GetQueryPoolResults = [](VkDevice, VkQueryPool, uint32_t, uint32_t,
                                   size_t, void *, VkDeviceSize, VkQueryResultFlags) {
      ++queryResultCalls; return queryReadResult;
   };
}

static void query_failure_contract()
{
   D3D10DDI_DEVICEFUNCS table = {}; query_fill_table(&table);
   D3D10DDIARG_CREATEQUERY desc = {}; desc.Query = D3D10DDI_QUERY_OCCLUSION;
   alignas(Query) BYTE storage[sizeof(Query)];
   for (VkResult failure : {VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY, VK_ERROR_UNKNOWN}) {
      reset(); Adapter adapter; Device dev = make_device(&adapter); query_backend(dev);
      queryFailPoolCall = 1; queryPoolFailure = failure;
      table.pfnCreateQuery({&dev}, &desc, {storage}, {});
      const bool oom = failure != VK_ERROR_UNKNOWN;
      CHECK(lastError == (oom ? E_OUTOFMEMORY : D3DDDIERR_DEVICEREMOVED));
      CHECK(FAILED(dev.lost) == !oom && queryPoolCalls == 1 && dev.deferred.empty());
      /* The failed handle is invalid: runtime does not call DestroyQuery. */
      if (oom) {
         lastError = S_OK;
         table.pfnCreateQuery({&dev}, &desc, {storage}, {});
         CHECK(lastError == S_OK && queryPoolCalls == 2);
         table.pfnDestroyQuery({&dev}, {storage}); retire(&dev);
      }
   }
   for (D3D10DDI_QUERY type : {D3D10DDI_QUERY_STREAMOUTPUTSTATS, D3D10DDI_QUERY_TIMESTAMP,
                              D3D10DDI_QUERY_PIPELINESTATS}) {
      reset(); Adapter adapter; Device dev = make_device(&adapter); query_backend(dev);
      desc.Query = type;
      table.pfnCreateQuery({&dev}, &desc, {storage}, {});
      CHECK(lastError == D3DDDIERR_DEVICEREMOVED && FAILED(dev.lost) && !queryPoolCalls);
   }
   /* Pool replacement during Begin, timestamp End, and pass rollover must
    * keep old GPU ownership while stopping the failed command stream. */
   for (unsigned mode = 0; mode < 3; mode++) {
      reset(); Adapter adapter; Device dev = make_device(&adapter); query_backend(dev);
      adapter.vk.caps.timestampValidBits = 64;
      desc.Query = mode == 1 ? D3D10DDI_QUERY_TIMESTAMP : D3D10DDI_QUERY_OCCLUSION;
      table.pfnCreateQuery({&dev}, &desc, {storage}, {});
      CHECK(lastError == S_OK);
      dev.um.pfnSetErrorCb = [](D3D10DDI_HRTCORELAYER, HRESULT hr) {
         CHECK(hr == D3DDDIERR_DEVICEREMOVED); ++setErrorCalls; lastError = hr;
      };
      queryFailPoolCall = 2;
      if (mode == 1) {
         table.pfnQueryEnd({&dev}, {storage});
         table.pfnQueryEnd({&dev}, {storage});
      } else {
         table.pfnQueryBegin({&dev}, {storage});
         VkCommandBuffer cmd = cmd_draw(&dev);
         const unsigned count = mode == 2 ? QUERY_SLOTS : 1;
         for (unsigned i = 0; i < count; i++) { query_pass_begin(&dev, cmd); query_pass_end(&dev, cmd); }
         if (mode == 2) query_pass_begin(&dev, cmd);
         else {
            table.pfnQueryEnd({&dev}, {storage});
            table.pfnQueryBegin({&dev}, {storage});
         }
      }
      CHECK(lastError == D3DDDIERR_DEVICEREMOVED && FAILED(dev.lost));
      Query *q = reinterpret_cast<Query *>(storage);
      CHECK(q->pool == handle<VkQueryPool>(1) && q->fullPools.empty());
      const unsigned begins = queryBeginCalls, timestamps = timestampCalls;
      table.pfnQueryBegin({&dev}, {storage});
      table.pfnQueryEnd({&dev}, {storage});
      query_pass_begin(&dev, dev.cmd);
      uint64_t value = UINT64_MAX;
      table.pfnQueryGetData({&dev}, {storage}, &value, sizeof(value), 0);
      CHECK(value == UINT64_MAX && !queryResultCalls && queryPoolCalls == 2);
      CHECK(queryBeginCalls == begins && timestampCalls == timestamps);
      CHECK(FAILED(flush(&dev, false)) && !submissions && !cpuWaits && !cpuSignals);
      table.pfnDestroyQuery({&dev}, {storage}); retire(&dev);
      CHECK(!dev.deferred.empty()); /* failed device does not fake completion */
      retire(&dev, true); /* fixture teardown: no GPU was ever executed */
   }
   for (VkResult result : {VK_NOT_READY, VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_DEVICE_LOST}) {
      reset(); Adapter adapter; Device dev = make_device(&adapter); query_backend(dev);
      desc.Query = D3D10DDI_QUERY_OCCLUSION;
      table.pfnCreateQuery({&dev}, &desc, {storage}, {});
      table.pfnQueryBegin({&dev}, {storage});
      VkCommandBuffer cmd = cmd_draw(&dev);
      query_pass_begin(&dev, cmd); query_pass_end(&dev, cmd);
      table.pfnQueryEnd({&dev}, {storage}); CHECK(SUCCEEDED(flush(&dev, false)));
      completedValue = dev.submitted; queryReadResult = result;
      uint64_t value = UINT64_MAX;
      table.pfnQueryGetData({&dev}, {storage}, &value, sizeof(value), 0);
      CHECK(value == UINT64_MAX && queryResultCalls == 1 && !cpuWaits);
      CHECK(lastError == (result == VK_NOT_READY ? DXGI_DDI_ERR_WASSTILLDRAWING : D3DDDIERR_DEVICEREMOVED));
      CHECK(FAILED(dev.lost) == (result != VK_NOT_READY));
      table.pfnDestroyQuery({&dev}, {storage}); retire(&dev, true);
   }
   /* Completion failure must not be overwritten by WASSTILLDRAWING. */
   reset(); Adapter adapter; Device dev = make_device(&adapter); query_backend(dev);
   desc.Query = D3D10DDI_QUERY_EVENT;
   table.pfnCreateQuery({&dev}, &desc, {storage}, {});
   CHECK(cmd_draw(&dev)); table.pfnQueryEnd({&dev}, {storage});
   CHECK(SUCCEEDED(flush(&dev, false)));
   dev.vk.GetSemaphoreCounterValue = [](VkDevice, VkSemaphore, uint64_t *) { return VK_ERROR_DEVICE_LOST; };
   uint64_t value = UINT64_MAX;
   table.pfnQueryGetData({&dev}, {storage}, &value, sizeof(value), 0);
   CHECK(lastError == D3DDDIERR_DEVICEREMOVED && value == UINT64_MAX && !queryResultCalls);
   table.pfnDestroyQuery({&dev}, {storage}); retire(&dev, true);

   /* Timestamp recording also reaches the command-buffer allocator. */
   reset(); Adapter timestampAdapter; Device timestampDev = make_device(&timestampAdapter);
   query_backend(timestampDev); timestampAdapter.vk.caps.timestampValidBits = 64;
   desc.Query = D3D10DDI_QUERY_TIMESTAMP;
   table.pfnCreateQuery({&timestampDev}, &desc, {storage}, {});
   timestampDev.um.pfnSetErrorCb = [](D3D10DDI_HRTCORELAYER, HRESULT hr) {
      CHECK(hr == D3DDDIERR_DEVICEREMOVED); lastError = hr;
   };
   timestampDev.vk.AllocateCommandBuffers = [](VkDevice, const VkCommandBufferAllocateInfo *, VkCommandBuffer *) {
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   };
   table.pfnQueryEnd({&timestampDev}, {storage});
   CHECK(lastError == D3DDDIERR_DEVICEREMOVED && !timestampCalls && FAILED(timestampDev.lost));
   table.pfnDestroyQuery({&timestampDev}, {storage}); retire(&timestampDev, true);
   std::puts("PASS: query failures obey DDI error classes, preserve output/ownership and stop recording; End without Begin resets results");
}

static void shader_contract(const BYTE *dxbc, bool vertex, const char *outputPath,
                            bool sampled = false, bool exportOnly = false)
{
   reset(); Adapter adapter; Device dev = make_device(&adapter);
   shaderOutput = outputPath;
   dev.vk.CreateShadersEXT = [](VkDevice, uint32_t count, const VkShaderCreateInfoEXT *info,
                               const VkAllocationCallbacks *, VkShaderEXT *out) {
      CHECK(count == 1 && info->codeSize > 20 && info->codeType == VK_SHADER_CODE_TYPE_SPIRV_EXT);
      CHECK(static_cast<const uint32_t *>(info->pCode)[0] == 0x07230203);
      FILE *file = fopen(shaderOutput, "wb"); CHECK(file);
      CHECK(fwrite(info->pCode, 1, info->codeSize, file) == info->codeSize); CHECK(!fclose(file));
      *out = handle<VkShaderEXT>(1); return VK_SUCCESS;
   };
   std::vector<D3D10DDIARG_SIGNATURE_ENTRY> input, output;
   std::vector<UINT> tokens;
   for (UINT c = 0; c < read32(dxbc + 28); c++) {
      const BYTE *chunk = dxbc + read32(dxbc + 32 + 4 * c);
      const BYTE *data = chunk + 8;
      if (!memcmp(chunk, "SHDR", 4) || !memcmp(chunk, "SHEX", 4)) {
         tokens.resize(read32(chunk + 4) / 4); memcpy(tokens.data(), data, read32(chunk + 4));
      } else if (!memcmp(chunk, "ISGN", 4) || !memcmp(chunk, "OSGN", 4)) {
         auto &entries = chunk[0] == 'I' ? input : output;
         for (UINT i = 0; i < read32(data); i++) {
            const BYTE *entry = data + 8 + 24 * i;
            entries.push_back({(D3D10_SB_NAME)read32(entry + 8), read32(entry + 16), entry[20]});
         }
      }
   }
   CHECK(!tokens.empty());
   D3D10DDIARG_STAGE_IO_SIGNATURES sig = {input.data(), (UINT)input.size(), output.data(), (UINT)output.size()};
   D3D10DDI_DEVICEFUNCS table = {}; shader_fill_table(&table);
   alignas(Shader) BYTE storage[sizeof(Shader)];
   auto create = vertex ? table.pfnCreateVertexShader : table.pfnCreatePixelShader;
   create({&dev}, tokens.data(), {storage}, {}, &sig);
   CHECK(SUCCEEDED(lastError) && reinterpret_cast<Shader *>(storage)->variants.size() == 1);
   if (sampled) {
      const Shader *shader = reinterpret_cast<Shader *>(storage);
      CHECK(shader->cbMask == (1u << 13) && shader->srvMask[0] == 1 && shader->samplerMask == 1);
   }
   if (exportOnly) {
      table.pfnDestroyShader({&dev}, {storage});
      retire(&dev, true);
      std::puts("PASS: supplied SM4 DXBC converted through the production DDI; no GPU execution");
      return;
   }
   /* A lazily compiled variant can discover removal after base creation.
    * The base handle must not make the device look usable again. */
   shaderBackendCalls = 0;
   dev.vk.CreateShadersEXT = [](VkDevice, uint32_t, const VkShaderCreateInfoEXT *,
                               const VkAllocationCallbacks *, VkShaderEXT *out) {
      ++shaderBackendCalls; *out = VK_NULL_HANDLE; return VK_ERROR_DEVICE_LOST;
   };
   Shader *created = reinterpret_cast<Shader *>(storage);
   CHECK(!shader_variant(created, 1u << 16));
   CHECK(lastError == D3DDDIERR_DEVICEREMOVED && FAILED(dev.lost));
   CHECK(created->error == D3DDDIERR_DEVICEREMOVED && shaderBackendCalls == 1);
   CHECK(!shader_variant(created, 0) && !shader_variant(created, 1u << 17));
   CHECK(shaderBackendCalls == 1);
   table.pfnDestroyShader({&dev}, {storage}); retire(&dev, true);
   dev.lost = S_OK;
   std::puts(vertex ? "PASS: real SM4 VS DDI-to-SPIR-V conversion" : "PASS: real SM4 PS DDI-to-SPIR-V conversion");

   /* The runtime does not call DestroyShader after a creation error. Test
    * that the production callback frees its arrays before reporting it. */
   failedShader = reinterpret_cast<Shader *>(storage);
   dev.vk.CreateShadersEXT = [](VkDevice, uint32_t, const VkShaderCreateInfoEXT *,
                               const VkAllocationCallbacks *, VkShaderEXT *out) {
      shaderAllocations[0] = failedShader->tokens.data();
      shaderAllocations[1] = failedShader->inputs.data();
      shaderAllocations[2] = failedShader->outputs.data();
      CHECK(shaderAllocations[0] && shaderAllocations[1] && shaderAllocations[2]);
      *out = VK_NULL_HANDLE;
      return shaderFailure;
   };
   dev.um.pfnSetErrorCb = [](D3D10DDI_HRTCORELAYER, HRESULT hr) {
      CHECK(shaderAllocationFrees == 3);
      lastError = hr;
   };
   for (VkResult failure : {VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                            VK_ERROR_UNKNOWN, VK_ERROR_DEVICE_LOST}) {
      shaderFailure = failure;
      shaderAllocationFrees = 0;
      lastError = S_OK;
      create({&dev}, tokens.data(), {storage}, {}, &sig);
      const HRESULT expected = failure == VK_ERROR_DEVICE_LOST ? D3DDDIERR_DEVICEREMOVED :
         failure == VK_ERROR_UNKNOWN ? E_FAIL : E_OUTOFMEMORY;
      CHECK(lastError == expected && shaderAllocationFrees == 3 && dev.deferred.empty());
      CHECK(FAILED(dev.lost) == (failure == VK_ERROR_DEVICE_LOST));
   }
   failedShader = nullptr;
   /* Even an existing cached variant cannot be reused after device loss. */
   Shader cached; cached.dev = &dev; cached.variants.push_back({0, handle<VkShaderEXT>(1)});
   CHECK(shader_variant(&cached, 0) == VK_NULL_HANDLE);
   CHECK(!cmd_begin(&dev) && FAILED(flush(&dev, true)) && !submissions);
   dev.vk.CreateShadersEXT = [](VkDevice, uint32_t, const VkShaderCreateInfoEXT *,
                               const VkAllocationCallbacks *, VkShaderEXT *) -> VkResult {
      CHECK(false); return VK_ERROR_UNKNOWN;
   };
   create({&dev}, tokens.data(), {storage}, {}, &sig);
   CHECK(lastError == D3DDDIERR_DEVICEREMOVED);
   std::puts("PASS: shader failure preserves OOM/device-loss status and cleans before SetErrorCb");
}

static void sampled_shader_contract(const char *source, const char *outputPath)
{
   HMODULE module = LoadLibraryW(L"d3dcompiler_47.dll"); CHECK(module);
   auto compile = reinterpret_cast<pD3DCompile>(reinterpret_cast<void *>(GetProcAddress(module, "D3DCompile")));
   CHECK(compile);
   ID3DBlob *code = nullptr, *error = nullptr;
   HRESULT hr = compile(source, strlen(source), "tgd3d-control", nullptr, nullptr,
                        "main", "ps_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &error);
   if (error) { std::fwrite(error->GetBufferPointer(), 1, error->GetBufferSize(), stderr); error->Release(); }
   CHECK(SUCCEEDED(hr) && code);
   shader_contract(static_cast<const BYTE *>(code->GetBufferPointer()), false, outputPath, true);
   code->Release(); FreeLibrary(module);
   std::puts("PASS: SM4 sampled image, sampler and constant buffer 13 reflection and SPIR-V");
}

static void presentation_rotation_contract()
{
#ifdef TGD3D_RED_PRIMARY_CONTROL
   Adapter adapter; reset(); Device dev = make_device(&adapter);
   static unsigned clears, redViews;
   clears = redViews = 0;
   dev.vk.CreateShadersEXT = [](VkDevice, UINT, const VkShaderCreateInfoEXT *,
                               const VkAllocationCallbacks *, VkShaderEXT *) {
      CHECK(false); return VK_ERROR_UNKNOWN;
   };
   dev.vk.CmdDraw = [](VkCommandBuffer, UINT, UINT, UINT, UINT) { CHECK(false); };
   dev.vk.UpdateDescriptorSets = [](VkDevice, UINT, const VkWriteDescriptorSet *,
                                    UINT, const VkCopyDescriptorSet *) { CHECK(false); };
   dev.vk.CreateImageView = [](VkDevice, const VkImageViewCreateInfo *info,
                               const VkAllocationCallbacks *, VkImageView *view) {
      CHECK(info->image == handle<VkImage>(0x200) && info->format == VK_FORMAT_B8G8R8A8_UNORM);
      CHECK(!info->subresourceRange.baseMipLevel && !info->subresourceRange.baseArrayLayer &&
            info->subresourceRange.levelCount == 1 && info->subresourceRange.layerCount == 1);
      ++redViews; *view = handle<VkImageView>(0x400); return VK_SUCCESS;
   };
   dev.vk.CmdBeginRendering = [](VkCommandBuffer, const VkRenderingInfo *info) {
      CHECK(info->colorAttachmentCount == 1 && info->layerCount == 1);
      CHECK(!info->renderArea.offset.x && !info->renderArea.offset.y &&
            info->renderArea.extent.width == 720 && info->renderArea.extent.height == 1280);
      const auto &a = info->pColorAttachments[0];
      CHECK(a.imageLayout == VK_IMAGE_LAYOUT_GENERAL && a.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR &&
            a.storeOp == VK_ATTACHMENT_STORE_OP_STORE);
      CHECK(a.clearValue.color.float32[0] == 1 && a.clearValue.color.float32[1] == 0 &&
            a.clearValue.color.float32[2] == 0 && a.clearValue.color.float32[3] == 1);
      ++clears;
   };
   Resource src, dst;
   src.dev = dst.dev = &dev; src.kind = dst.kind = ResKind::Image;
   src.bind = dst.bind = D3D10_DDI_BIND_RENDER_TARGET | D3D10_DDI_BIND_PRESENT;
   src.format = dst.format = DXGI_FORMAT_B8G8R8A8_UNORM;
   src.width = dst.height = 1280; src.height = dst.width = 720;
   src.storage.image = handle<VkImage>(0x100); dst.storage.image = handle<VkImage>(0x200);
   src.aspects = dst.aspects = src.storage.aspects = dst.storage.aspects = VK_IMAGE_ASPECT_COLOR_BIT;
   DXGI1_1_DDI_BASE_FUNCTIONS table = {}; dxgi_fill_table(&table, true);
   DXGI_DDI_ARG_BLT args = {};
   args.hDevice = (DXGI_DDI_HDEVICE)&dev; args.hSrcResource = (DXGI_DDI_HRESOURCE)&src;
   args.hDstResource = (DXGI_DDI_HRESOURCE)&dst; args.DstRight = 720; args.DstBottom = 1280;
   for (UINT angle : {1u, 2u, 3u}) {
      args.Rotate = (DXGI_DDI_MODE_ROTATION)(DXGI_DDI_MODE_ROTATION_IDENTITY + angle);
      CHECK(table.pfnBlt(&args) == S_OK && clears == angle);
   }
   CHECK(redViews == 1 && !src.storage.lastUse && dst.storage.lastUse == dev.recordingSeq());
   CHECK(!cpuWaits && !cpuSignals && !submissions && !descriptorSets);
   DXGI_DDI_ARG_SETDISPLAYMODE mode = {};
   mode.hDevice = args.hDevice; mode.hResource = args.hDstResource;
   /* No runtime owner in this fixture: only the final callback rejects it.
    * The clear must still be submitted and actually waited before that call. */
   CHECK(table.pfnSetDisplayMode(&mode) == DXGI_ERROR_UNSUPPORTED);
   CHECK(clears == 4 && submissions == 1 && cpuWaits == 1 && completedValue == 1 && !cpuSignals);
   dst.width = 1280;
   CHECK(red_primary_clear(&dev, &dst, 0) == DXGI_ERROR_UNSUPPORTED && clears == 4);
   dst.width = 720;
   CHECK(red_primary_clear(&dev, &dst, 1) == DXGI_ERROR_UNSUPPORTED && clears == 4);
   dev.lost = D3DDDIERR_DEVICEREMOVED;
   CHECK(red_primary_clear(&dev, &dst, 0) == dev.lost && clears == 4);
   storage_destroy(&dev, dst.storage, true); cmd_destroy(&dev);
   std::puts("PASS: diagnostic portrait red clear, no rotation shader/source sampling/draw, real wait before modeset (CPU control; not display acceptance)");
#else
   Adapter adapter; reset(); Device dev = make_device(&adapter);
   for (UINT i = 0; i < SET_COUNT; ++i) dev.setLayouts[i] = handle<VkDescriptorSetLayout>(0x10+i);
   dev.pipelineLayout = handle<VkPipelineLayout>(0x20);
   dev.defaultSampler = handle<VkSampler>(0x30);
   static struct { UINT shaderCalls, viewCalls, draws, angle, width, height; bool scaled; } observed;
   observed = {};
   dev.vk.CreateShadersEXT = [](VkDevice, UINT count, const VkShaderCreateInfoEXT *info,
                              const VkAllocationCallbacks *, VkShaderEXT *shader) {
      CHECK(count == 1 && info->codeType == VK_SHADER_CODE_TYPE_SPIRV_EXT &&
            info->setLayoutCount == SET_COUNT && info->codeSize > 20);
      ++observed.shaderCalls;
      if (info->stage == VK_SHADER_STAGE_VERTEX_BIT) {
         const auto *c = info->pSpecializationInfo;
         CHECK(c && c->mapEntryCount == 1 && c->dataSize == sizeof(UINT));
         CHECK(c->pMapEntries[0].constantID == 0 && !c->pMapEntries[0].offset);
         const UINT angle = *static_cast<const UINT *>(c->pData);
         CHECK(angle >= 1 && angle <= 3);
         *shader = handle<VkShaderEXT>(0x1000+angle);
      } else {
         CHECK(info->stage == VK_SHADER_STAGE_FRAGMENT_BIT);
         *shader = handle<VkShaderEXT>(0x2000);
      }
      return VK_SUCCESS;
   };
   dev.vk.CreateSampler = [](VkDevice, const VkSamplerCreateInfo *info,
                             const VkAllocationCallbacks *, VkSampler *sampler) {
      CHECK(info->minFilter == VK_FILTER_NEAREST && info->magFilter == VK_FILTER_NEAREST);
      *sampler = handle<VkSampler>(0x31); return VK_SUCCESS;
   };
   dev.vk.CreateImageView = [](VkDevice, const VkImageViewCreateInfo *info,
                               const VkAllocationCallbacks *, VkImageView *view) {
      ++observed.viewCalls;
      CHECK(info->viewType == VK_IMAGE_VIEW_TYPE_2D && info->subresourceRange.layerCount == 1);
      if (info->image == handle<VkImage>(0x100)) {
         CHECK(info->format == VK_FORMAT_B8G8R8A8_UNORM && info->components.a == VK_COMPONENT_SWIZZLE_ONE);
         CHECK(info->subresourceRange.baseMipLevel == 1 && info->subresourceRange.baseArrayLayer == 1);
      } else {
         CHECK(info->image == handle<VkImage>(0x200) && info->format == VK_FORMAT_R8G8B8A8_UNORM);
         CHECK(!info->subresourceRange.baseMipLevel && !info->subresourceRange.baseArrayLayer);
      }
      *view = handle<VkImageView>(0x400+observed.viewCalls); return VK_SUCCESS;
   };
   dev.vk.CreateDescriptorPool = [](VkDevice, const VkDescriptorPoolCreateInfo *,
                                    const VkAllocationCallbacks *, VkDescriptorPool *pool) {
      *pool = handle<VkDescriptorPool>(0x500); return VK_SUCCESS;
   };
   dev.vk.AllocateDescriptorSets = [](VkDevice, const VkDescriptorSetAllocateInfo *, VkDescriptorSet *set) {
      *set = handle<VkDescriptorSet>(0x600); return VK_SUCCESS;
   };
   dev.vk.UpdateDescriptorSets = [](VkDevice, UINT count, const VkWriteDescriptorSet *write,
                                   UINT, const VkCopyDescriptorSet *) {
      CHECK(count == 2 && write[0].dstBinding == 288 && write[1].dstBinding == 304);
      CHECK(write[0].pImageInfo->sampler == handle<VkSampler>(observed.scaled ? 0x30 : 0x31));
      CHECK(write[1].pImageInfo->imageLayout == VK_IMAGE_LAYOUT_GENERAL);
   };
   dev.vk.CmdBeginRendering = [](VkCommandBuffer, const VkRenderingInfo *info) {
      CHECK(info->colorAttachmentCount == 1 && info->layerCount == 1);
      CHECK(info->renderArea.offset.x == 7 && info->renderArea.offset.y == 11);
      CHECK(info->renderArea.extent.width == observed.width && info->renderArea.extent.height == observed.height);
      CHECK(info->pColorAttachments[0].loadOp == VK_ATTACHMENT_LOAD_OP_LOAD &&
            info->pColorAttachments[0].storeOp == VK_ATTACHMENT_STORE_OP_STORE);
   };
   dev.vk.CmdBindShadersEXT = [](VkCommandBuffer, UINT count, const VkShaderStageFlagBits *stages,
                                 const VkShaderEXT *shaders) {
      CHECK(count == 3 && stages[0] == VK_SHADER_STAGE_VERTEX_BIT &&
            stages[1] == VK_SHADER_STAGE_FRAGMENT_BIT && stages[2] == VK_SHADER_STAGE_GEOMETRY_BIT);
      CHECK(shaders[0] == handle<VkShaderEXT>(0x1000+observed.angle) &&
            shaders[1] == handle<VkShaderEXT>(0x2000) && !shaders[2]);
   };
   dev.vk.CmdDraw = [](VkCommandBuffer, UINT vertices, UINT instances, UINT first, UINT base) {
      CHECK(vertices == 3 && instances == 1 && !first && !base); ++observed.draws;
   };
   Resource src, dst;
   src.dev = dst.dev = &dev; src.kind = dst.kind = ResKind::Image;
   src.bind = D3D10_DDI_BIND_RENDER_TARGET | D3D10_DDI_BIND_PRESENT;
   dst.bind = D3D10_DDI_BIND_RENDER_TARGET;
   src.format = DXGI_FORMAT_B8G8R8X8_UNORM; dst.format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
   src.width = 34; src.height = 18; src.mips = src.arraySize = 2;
   dst.width = dst.height = 64;
   src.storage.image = handle<VkImage>(0x100); dst.storage.image = handle<VkImage>(0x200);
   src.storage.memory = handle<VkDeviceMemory>(0x700); dst.storage.memory = handle<VkDeviceMemory>(0x800);
   src.aspects = dst.aspects = VK_IMAGE_ASPECT_COLOR_BIT;
   DXGI1_1_DDI_BASE_FUNCTIONS table = {}; dxgi_fill_table(&table, true);
   DXGI_DDI_ARG_BLT args = {};
   args.hDevice = (DXGI_DDI_HDEVICE)&dev; args.hSrcResource = (DXGI_DDI_HRESOURCE)&src;
   args.hDstResource = (DXGI_DDI_HRESOURCE)&dst; args.SrcSubresource = 3;
   args.DstLeft = 7; args.DstTop = 11;
   for (UINT angle : {1u, 2u, 3u, 1u}) {
      observed.angle = angle; observed.scaled = false;
      observed.width = angle & 1 ? 9 : 17; observed.height = angle & 1 ? 17 : 9;
      args.DstRight = 7+observed.width; args.DstBottom = 11+observed.height;
      args.Rotate = (DXGI_DDI_MODE_ROTATION)(DXGI_DDI_MODE_ROTATION_IDENTITY+angle);
      CHECK(table.pfnBlt(&args) == S_OK && dev.dirty == DIRTY_ALL && !dev.rendering);
      CHECK(src.storage.lastUse == dev.recordingSeq() && dst.storage.lastUse == dev.recordingSeq());
   }
   CHECK(observed.shaderCalls == 4 && observed.viewCalls == 2 && observed.draws == 4);
   observed.angle = 1; observed.scaled = true; observed.width = 13; observed.height = 7;
   args.DstRight = 20; args.DstBottom = 18;
   CHECK(table.pfnBlt(&args) == S_OK && observed.draws == 5);
   args.Rotate = (DXGI_DDI_MODE_ROTATION)99;
   CHECK(table.pfnBlt(&args) == E_INVALIDARG && observed.draws == 5);
   CHECK(!cpuWaits && !cpuSignals && !submissions); // copy only records GPU work
   storage_destroy(&dev, src.storage, false); storage_destroy(&dev, dst.storage, false);
   CHECK(!viewsDestroyed);
   retire(&dev); CHECK(!viewsDestroyed); // unfinished draw keeps its views/backing
   completedValue = dev.recordingSeq(); retire(&dev);
   CHECK(viewsDestroyed == 2);
   blit_destroy(&dev); cmd_destroy(&dev);

   reset(); dev = make_device(&adapter);
   dev.vk.CreateShadersEXT = [](VkDevice, UINT, const VkShaderCreateInfoEXT *,
                               const VkAllocationCallbacks *, VkShaderEXT *) { return VK_ERROR_OUT_OF_DEVICE_MEMORY; };
   src.dev = dst.dev = &dev; src.storage.image = handle<VkImage>(1); dst.storage.image = handle<VkImage>(2);
   args.Rotate = DXGI_DDI_MODE_ROTATION_ROTATE90;
   CHECK(table.pfnBlt(&args) == E_OUTOFMEMORY && setErrorCalls == 1 && !dev.cmdHasWork);
   std::puts("PASS: GPU rotated-Blt command/state, nonzero rectangle, subresources, sRGB/XRGB views, caching, OOM and in-flight retirement (CPU control; no pixels rendered)");
#endif
}

void runtime_contracts();
void resource_creation_contracts();
void shader_cache_contracts(bool child);

/* Reproduce compiler issues with real application shaders without executing
 * them or installing a driver. This D3D10 control accepts standalone SM4.0
 * VS/PS containers with the corresponding D3D10 signature chunks. */
static void export_dxbc(const char *inputPath, const char *outputPath)
{
   FILE *file = std::fopen(inputPath, "rb"); CHECK(file);
   CHECK(!std::fseek(file, 0, SEEK_END));
   const long size = std::ftell(file); CHECK(size >= 32);
   std::rewind(file);
   std::vector<BYTE> bytes((size_t)size);
   CHECK(std::fread(bytes.data(), 1, bytes.size(), file) == bytes.size());
   CHECK(!std::fclose(file));
   const BYTE *data = bytes.data();
   CHECK(!std::memcmp(data, "DXBC", 4) && read32(data + 20) == 1);
   CHECK(read32(data + 24) == bytes.size());
   const UINT chunks = read32(data + 28);
   CHECK(chunks && chunks <= (bytes.size() - 32) / 4);
   unsigned programs = 0, inputs = 0, outputs = 0;
   UINT stage = ~0u;
   for (UINT i = 0; i < chunks; ++i) {
      const UINT offset = read32(data + 32 + 4 * i);
      CHECK(offset >= 32 + 4 * chunks && offset <= bytes.size() - 8);
      const BYTE *chunk = data + offset;
      const UINT length = read32(chunk + 4);
      CHECK(length <= bytes.size() - offset - 8);
      if (!std::memcmp(chunk, "SHDR", 4) || !std::memcmp(chunk, "SHEX", 4)) {
         CHECK(length >= 8 && !(length & 3) && read32(chunk + 12) == length / 4);
         const UINT version = read32(chunk + 8);
         CHECK((version & 0xffff) == 0x40 && (version >> 16) <= 1);
         stage = version >> 16;
         ++programs;
      } else if (!std::memcmp(chunk, "ISGN", 4) || !std::memcmp(chunk, "OSGN", 4)) {
         CHECK(length >= 8 && read32(chunk + 8) <= (length - 8) / 24);
         (chunk[0] == 'I' ? inputs : outputs)++;
      }
   }
   CHECK(programs == 1 && inputs == 1 && outputs == 1);
   shader_contract(data, stage == 1, outputPath, false, true);
}

int main(int argc, char **argv)
{
   if (argc == 4 && !strcmp(argv[1], "--export-dxbc")) {
      export_dxbc(argv[2], argv[3]);
      return 0;
   }
   if (argc == 2 && !strcmp(argv[1], "--shader-cache-child")) {
      shader_cache_contracts(true);
      return 0;
   }
   submission_contract(); nonblocking_map(); incomplete_fence_keeps_resources();
   device_error_contract();
   rotated_views(); descriptor_contract(); draw_error_contract(); empty_box_contract();
   query_contract();
   query_failure_contract();
   shader_contract(g_VS, true, "tgd3d-vs.spv"); shader_contract(g_PS, false, "tgd3d-ps.spv");
   sampled_shader_contract(
      "Texture2D<float4> tex:register(t0); SamplerState samp:register(s0);"
      "cbuffer C:register(b13){float4 color;}"
      "float4 main(float4 p:SV_Position,float2 uv:TEXCOORD0):SV_Target {"
      "return tex.Sample(samp,uv)*color;}", "tgd3d-sample.spv");
   /* Exercise uniform-controlled loop bounds and dynamic constant-buffer
    * indexing in a sampled pixel shader. A simple triangle omits both. */
   sampled_shader_contract(
      "Texture2D<float4> tex:register(t0); SamplerState samp:register(s0);"
      "cbuffer C:register(b13){uint taps; float threshold; float2 step; float4 weights[16];}"
      "float4 main(float4 p:SV_Position,float2 uv:TEXCOORD0):SV_Target {"
      "float4 sum=0; [loop] for(uint i=0;i<min(taps,16u);++i)"
      "sum+=tex.SampleLevel(samp,uv+step*i,0)*weights[i];"
      "clip(sum.a-threshold); return sum;}", "tgd3d-loop.spv");
   presentation_rotation_contract();
   runtime_contracts();
   resource_creation_contracts();
   shader_cache_contracts(false);
   std::puts("CPU controls passed; no GPU execution, WDDM runtime or physical display tested.");
}
