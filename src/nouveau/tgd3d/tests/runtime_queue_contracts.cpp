/* SPDX-License-Identifier: MIT */
/* Production callback transport; kernel callbacks are CPU substitutes. */
#include "tgd3d.h"
#include "nouveau/vulkan/nvkmd/windows/submit_abi.h"
#include "vulkan/util/vk_internal_exts.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace tgd3d;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%u: %s\n", __FILE__, __LINE__, #x); std::exit(1); } } while (0)
static_assert(VK_STRUCTURE_TYPE_WSI_WIN32_WRITTEN_PRIMARIES_MESA != VK_STRUCTURE_TYPE_NVK_EMBEDDED_INSTANCE_INFO_MESA);
static UINT liveContexts, liveFences, nextContext, nextFence, submits, gpuWaits, gpuSignals, cpuWaits, cpuSignals;
static bool failCreateContext, failCreateFence, failClose, nullContext, nullFenceValue;
static bool failSubmit, failGpu, reuseOriginal;
static unsigned cpuWaitMode;
static UINT64 values[64];
static HANDLE lastWaitEvent;
static DWORD callerThread;
static UINT expectedSequence;

static void callbacks(Device *dev, uintptr_t number)
{
   dev->nvk = true; dev->interfaceVersion = D3DWDDM2_0_DDI_INTERFACE_VERSION;
   dev->hRTDevice.handle = reinterpret_cast<HANDLE>(0x1000 + number);
   dev->hRTCoreLayer.handle = reinterpret_cast<void *>(0x2000 + number);
   dev->hContext = reinterpret_cast<HANDLE>(0x3000 + number);
   dev->createContextVirtual = [](D3D10DDI_HRTCORELAYER h, D3DWDDM2_0DDICB_CREATECONTEXTVIRTUAL *a) -> HRESULT {
      CHECK((uintptr_t)h.handle >= 0x2000 && (uintptr_t)h.handle <= 0x2001);
      CHECK(GetCurrentThreadId() == callerThread && a->NodeOrdinal == 0 && a->EngineAffinity == 1);
      CHECK(!a->Flags.Value && !a->pPrivateDriverData && !a->PrivateDriverDataSize);
      CHECK(a->ContextTypeFlags == WSI_WIN32_RUNTIME_COPY || a->ContextTypeFlags == (WSI_WIN32_RUNTIME_3D | WSI_WIN32_RUNTIME_COPY));
      if (reuseOriginal) { a->hContext = reinterpret_cast<HANDLE>((uintptr_t)h.handle + 0x1000); return S_OK; }
      if (!nullContext) { a->hContext = reinterpret_cast<HANDLE>(0x4000 + ++nextContext); ++liveContexts; }
      return failCreateContext ? E_OUTOFMEMORY : S_OK;
   };
   dev->kt.pfnDestroyContextCb = [](HANDLE h, const D3DDDICB_DESTROYCONTEXT *a) -> HRESULT {
      CHECK((uintptr_t)h >= 0x1000 && (uintptr_t)h <= 0x1001 && (uintptr_t)a->hContext > 0x4000);
      CHECK(GetCurrentThreadId() == callerThread && liveContexts);
      if (failClose) return D3DDDIERR_DEVICEREMOVED;
      --liveContexts; return S_OK;
   };
   dev->kt.pfnSubmitCommandCb = [](HANDLE h, const D3DDDICB_SUBMITCOMMAND *a) -> HRESULT {
      CHECK(h == reinterpret_cast<HANDLE>(0x1000) && GetCurrentThreadId() == callerThread);
      CHECK(a->Commands == 0x20000 && a->CommandLength == 16 && !a->Flags.Value);
      CHECK(a->BroadcastContextCount == 1 && (uintptr_t)a->BroadcastContext[0] > 0x4000);
      CHECK(a->NumPrimaries == 2 && a->WrittenPrimaries[0] == 0x60 && a->WrittenPrimaries[1] == 0x61);
      CHECK(a->RenderCBSequence == expectedSequence && !a->Reserved);
      const auto *request = static_cast<const tg_gpu_submit_request *>(a->pPrivateDriverData);
      CHECK(a->PrivateDriverDataSize == sizeof(*request) + 16 && request->version == TG_GPU_SUBMIT_ABI &&
            request->size == a->PrivateDriverDataSize && request->command_bytes == 16);
      const UINT expected[] = {0x12345678, 0x99887766, 0x10, 0x20};
      CHECK(!memcmp(request + 1, expected, sizeof(expected)));
      ++submits; return failSubmit ? E_FAIL : S_OK;
   };
   dev->kt.pfnCreateSynchronizationObject2Cb = [](HANDLE h, D3DDDICB_CREATESYNCHRONIZATIONOBJECT2 *a) -> HRESULT {
      CHECK((uintptr_t)h >= 0x1000 && (uintptr_t)h <= 0x1001 && GetCurrentThreadId() == callerThread);
      CHECK(a->Info.Type == D3DDDI_MONITORED_FENCE && a->Info.Flags.Shared && a->Info.Flags.NtSecuritySharing);
      CHECK(a->Info.MonitoredFence.EngineAffinity == 1 && nextFence + 1 < 64);
      UINT n = ++nextFence; values[n] = a->Info.MonitoredFence.InitialFenceValue;
      a->hSyncObject = 0x100 + n; ++liveFences;
      a->Info.MonitoredFence.FenceValueCPUVirtualAddress = nullFenceValue ? nullptr : &values[n];
      return failCreateFence ? E_OUTOFMEMORY : S_OK;
   };
   dev->kt.pfnDestroySynchronizationObjectCb = [](HANDLE h, const D3DDDICB_DESTROYSYNCHRONIZATIONOBJECT *a) -> HRESULT {
      CHECK((uintptr_t)h >= 0x1000 && (uintptr_t)h <= 0x1001 && a->hSyncObject > 0x100 && liveFences);
      if (failClose) return D3DDDIERR_DEVICEREMOVED;
      --liveFences; return S_OK;
   };
   dev->kt.pfnWaitForSynchronizationObjectFromGpuCb = [](HANDLE h, const D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMGPU *a) -> HRESULT {
      CHECK(h == reinterpret_cast<HANDLE>(0x1000) && GetCurrentThreadId() == callerThread);
      CHECK((uintptr_t)a->hContext > 0x4000 && a->ObjectCount == 1 && a->ObjectHandleArray[0] > 0x100 && a->MonitoredFenceValueArray[0] == 7);
      ++gpuWaits; return failGpu ? E_FAIL : S_OK;
   };
   dev->kt.pfnSignalSynchronizationObjectFromGpuCb = [](HANDLE h, const D3DDDICB_SIGNALSYNCHRONIZATIONOBJECTFROMGPU *a) -> HRESULT {
      CHECK(h == reinterpret_cast<HANDLE>(0x1000) && GetCurrentThreadId() == callerThread);
      CHECK((uintptr_t)a->hContext > 0x4000 && a->ObjectCount == 1 && a->ObjectHandleArray[0] > 0x100 && a->MonitoredFenceValueArray[0] == 9);
      ++gpuSignals; return S_OK; // submission success is not simulated GPU completion
   };
   dev->kt.pfnWaitForSynchronizationObjectFromCpuCb = [](HANDLE h, const D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMCPU *a) -> HRESULT {
      CHECK(h == reinterpret_cast<HANDLE>(0x1000) && a->ObjectCount == 1 && a->hAsyncEvent && !a->Flags.Value);
      ++cpuWaits; lastWaitEvent = a->hAsyncEvent;
      if (cpuWaitMode == 1) return D3DDDIERR_DEVICEREMOVED;
      if (cpuWaitMode == 2) return S_OK; // no completion: caller's bounded wait times out
      values[a->ObjectHandleArray[0] - 0x100] = cpuWaitMode == 3 ? UINT64_MAX : *a->FenceValueArray;
      CHECK(SetEvent(a->hAsyncEvent)); return S_OK;
   };
   dev->kt.pfnSignalSynchronizationObjectFromCpuCb = [](HANDLE h, const D3DDDICB_SIGNALSYNCHRONIZATIONOBJECTFROMCPU *a) -> HRESULT {
      CHECK(h == reinterpret_cast<HANDLE>(0x1000) && a->ObjectCount == 1);
      values[*a->ObjectHandleArray - 0x100] = *a->FenceValueArray; ++cpuSignals; return S_OK;
   };
}

void runtime_queue_contracts()
{
   callerThread = GetCurrentThreadId();
   Device first, second; callbacks(&first, 0); callbacks(&second, 1);
   wsi_win32_runtime a = {}, b = {};
   auto submitCallback = first.kt.pfnSubmitCommandCb; first.kt.pfnSubmitCommandCb = nullptr;
   CHECK(runtime_queue_prepare(&first, &a) == DXGI_ERROR_UNSUPPORTED && !first.runtimeQueue);
   first.kt.pfnSubmitCommandCb = submitCallback;
   CHECK(runtime_queue_prepare(&first, &a) == S_OK && a.device_cookie == first.hRTDevice.handle);
   CHECK(runtime_queue_prepare(&second, &b) == S_OK);
   void *graphics = nullptr, *copy = nullptr, *foreign = nullptr;
   CHECK(a.create_context(a.context, 0x80000000, &graphics) == VK_ERROR_FEATURE_NOT_PRESENT && !graphics);
   CHECK(a.create_context(a.context, WSI_WIN32_RUNTIME_3D | WSI_WIN32_RUNTIME_COPY, &graphics) == VK_SUCCESS);
   CHECK(a.create_context(a.context, WSI_WIN32_RUNTIME_COPY, &copy) == VK_SUCCESS && graphics != copy);
   CHECK(b.create_context(b.context, WSI_WIN32_RUNTIME_COPY, &foreign) == VK_SUCCESS);
   const UINT words[] = {0x12345678, 0x99887766, 0x10, 0x20}, written[] = {0x60, 0x61};
   expectedSequence = 1;
   CHECK(a.submit(a.context, foreign, 0x20000, words, 16, 2, written) == VK_ERROR_INVALID_EXTERNAL_HANDLE);
   CHECK(a.submit(a.context, graphics, 0x20000, words, 16, 17, written) == VK_ERROR_FEATURE_NOT_PRESENT);
   CHECK(a.submit(a.context, graphics, 0x20000, words, 0, 2, written) == VK_ERROR_FEATURE_NOT_PRESENT);
   CHECK(a.submit(a.context, graphics, UINT64_MAX - 3, words, 16, 2, written) == VK_ERROR_FEATURE_NOT_PRESENT);
   for (; expectedSequence <= 2; ++expectedSequence)
      CHECK(a.submit(a.context, graphics, 0x20000, words, 16, 2, written) == VK_SUCCESS);
   expectedSequence = 1;
   CHECK(a.submit(a.context, copy, 0x20000, words, 16, 2, written) == VK_SUCCESS && submits == 3);
   expectedSequence = 3; failSubmit = true;
   CHECK(a.submit(a.context, graphics, 0x20000, words, 16, 2, written) == VK_ERROR_DEVICE_LOST);
   failSubmit = false;
   wsi_win32_runtime_fence fence = {}, imported = {}, other = {};
   CHECK(a.create_fence(a.context, 3, &fence) == VK_SUCCESS && *fence.value == 3);
   CHECK(a.retain_fence(a.context, fence.token, &imported) == VK_SUCCESS && imported.handle == fence.handle);
   CHECK(b.create_fence(b.context, 0, &other) == VK_SUCCESS);
   wsi_win32_runtime_fence rejected;
   CHECK(a.retain_fence(a.context, other.token, &rejected) == VK_ERROR_INVALID_EXTERNAL_HANDLE);
   UINT64 wait = 7, signal = 9;
   CHECK(a.gpu_sync(a.context, graphics, false, 1, &other.handle, &wait) != VK_SUCCESS);
   CHECK(a.gpu_sync(a.context, graphics, false, 1, &fence.handle, &wait) == VK_SUCCESS);
   CHECK(a.gpu_sync(a.context, copy, true, 1, &fence.handle, &signal) == VK_SUCCESS);
   CHECK(gpuWaits == 1 && gpuSignals == 1 && !cpuWaits && !cpuSignals && *fence.value == 3);
   failGpu = true;
   CHECK(a.gpu_sync(a.context, graphics, false, 1, &fence.handle, &wait) == VK_ERROR_DEVICE_LOST);
   failGpu = false;
   CHECK(a.wait_cpu(a.context, fence.token, 9, 0) == VK_TIMEOUT && !cpuWaits);
   for (cpuWaitMode = 0; cpuWaitMode < 4; ++cpuWaitMode) {
      values[fence.handle - 0x100] = 3;
      VkResult expected = cpuWaitMode == 0 ? VK_SUCCESS : cpuWaitMode == 2 ? VK_TIMEOUT : VK_ERROR_DEVICE_LOST;
      CHECK(a.wait_cpu(a.context, fence.token, 9, 1) == expected);
      DWORD flags; CHECK(!GetHandleInformation(lastWaitEvent, &flags));
   }
   CHECK(a.wait_cpu(a.context, fence.token, 1, 0) == VK_ERROR_DEVICE_LOST); // all-ones before wait
   values[fence.handle - 0x100] = 3;
   CHECK(a.signal_cpu(a.context, fence.token, 4) == VK_SUCCESS && cpuSignals == 1 && *fence.value == 4);
   CHECK(runtime_queue_finish(&first) == DXGI_ERROR_INVALID_CALL); // live adopter references
   a.release_fence(a.context, fence.token); CHECK(liveFences == 2); // import still owns it
   failClose = true; a.release_fence(a.context, imported.token);
   CHECK(a.retain_fence(a.context, imported.token, &rejected) == VK_ERROR_INVALID_EXTERNAL_HANDLE);
   CHECK(a.destroy_context(a.context, graphics) == VK_ERROR_DEVICE_LOST);
   CHECK(a.submit(a.context, graphics, 0x20000, words, 16, 2, written) == VK_ERROR_INVALID_EXTERNAL_HANDLE);
   failClose = false;
   CHECK(a.destroy_context(a.context, copy) == VK_SUCCESS);
   failClose = true; CHECK(runtime_queue_finish(&first) == D3DDDIERR_DEVICEREMOVED);
   failClose = false; CHECK(runtime_queue_finish(&first) == S_OK && !first.runtimeQueue);
   b.release_fence(b.context, other.token);
   CHECK(b.destroy_context(b.context, foreign) == VK_SUCCESS);
   CHECK(runtime_queue_finish(&second) == S_OK && !liveContexts && !liveFences);

   /* Context destruction is the completion proof before allocator teardown.
    * A failed proof must preserve both tokens and fences, and reject new work. */
   {
      Device dev; callbacks(&dev, 0); CHECK(runtime_queue_prepare(&dev, &a) == S_OK);
      CHECK(a.create_context(a.context, WSI_WIN32_RUNTIME_COPY, &copy) == VK_SUCCESS);
      CHECK(a.create_fence(a.context, 0, &fence) == VK_SUCCESS);
      failClose = true;
      CHECK(runtime_queue_quiesce(&dev) == D3DDDIERR_DEVICEREMOVED && liveContexts == 1 && liveFences == 1);
      CHECK(a.submit(a.context, copy, 0x20000, words, 16, 2, written) == VK_ERROR_INVALID_EXTERNAL_HANDLE);
      CHECK(a.create_context(a.context, WSI_WIN32_RUNTIME_COPY, &graphics) == VK_ERROR_DEVICE_LOST);
      CHECK(runtime_queue_finish(&dev) == DXGI_ERROR_INVALID_CALL);
      failClose = false;
      CHECK(runtime_queue_quiesce(&dev) == S_OK && !liveContexts && liveFences == 1 && *fence.value == 0);
      CHECK(runtime_queue_finish(&dev) == DXGI_ERROR_INVALID_CALL); // adopter still owns the tokens
      CHECK(a.destroy_context(a.context, copy) == VK_SUCCESS);
      a.release_fence(a.context, fence.token);
      CHECK(runtime_queue_finish(&dev) == S_OK && !liveFences);
   }

   for (unsigned failure = 0; failure < 5; ++failure) {
      Device dev; callbacks(&dev, 0); CHECK(runtime_queue_prepare(&dev, &a) == S_OK);
      nullContext = failure == 0; failCreateContext = failure == 1;
      nullFenceValue = failure == 2; failCreateFence = failure == 3;
      failClose = failure == 1 || failure == 3;
      reuseOriginal = failure == 4;
      if (failure < 2 || failure == 4) CHECK(a.create_context(a.context, WSI_WIN32_RUNTIME_COPY, &copy) != VK_SUCCESS && !copy);
      else CHECK(a.create_fence(a.context, 0, &fence) != VK_SUCCESS && !fence.token);
      if (failClose) CHECK(runtime_queue_finish(&dev) == D3DDDIERR_DEVICEREMOVED);
      failClose = nullContext = failCreateContext = nullFenceValue = failCreateFence = reuseOriginal = false;
      CHECK(runtime_queue_finish(&dev) == S_OK && !liveContexts && !liveFences);
   }
   {
      Device video;
      callbacks(&video, 0);
      video.interfaceVersion = D3D11_1_DDI_INTERFACE_VERSION;
      video.hRTCoreLayer.handle = nullptr;
      video.createContextVirtual = nullptr;
      CHECK(runtime_queue_prepare(&video, &a) == DXGI_ERROR_UNSUPPORTED);
      video.kt.pfnCreateContextVirtualCb = [](HANDLE device, D3DDDICB_CREATECONTEXTVIRTUAL *args) -> HRESULT {
         CHECK(device == reinterpret_cast<HANDLE>(0x1000) && GetCurrentThreadId() == callerThread);
         CHECK(!args->NodeOrdinal && args->EngineAffinity == 1 && !args->Flags.Value);
         CHECK(!args->pPrivateDriverData && !args->PrivateDriverDataSize && !args->hContext);
         args->hContext = reinterpret_cast<HANDLE>(0x4000 + ++nextContext);
         ++liveContexts;
         return S_OK;
      };
      CHECK(runtime_queue_prepare(&video, &a) == S_OK);
      CHECK(a.create_context(a.context, WSI_WIN32_RUNTIME_3D | WSI_WIN32_RUNTIME_COPY, &graphics) == VK_SUCCESS);
      expectedSequence = 1;
      CHECK(a.submit(a.context, graphics, 0x20000, words, sizeof(words), 2, written) == VK_SUCCESS);
      CHECK(a.destroy_context(a.context, graphics) == VK_SUCCESS);
      CHECK(runtime_queue_finish(&video) == S_OK && !liveContexts && !liveFences);
   }
   std::puts("PASS: runtime-owned WDDM2 and 11.1 contexts, command payload/WrittenPrimaries, per-context sequences, same-device GPU fences, retained import lifetime, bounded CPU waits, quiescence before release and partial/failed cleanup (CPU callbacks; no GPU)");
}
