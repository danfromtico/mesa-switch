/* SPDX-License-Identifier: MIT
 * Runtime-owned GPU-VA command transport for the paired NVK device.
 * Commands and WrittenPrimaries travel together through the runtime callback.
 */
#include "tgd3d.h"
#include "nouveau/vulkan/nvkmd/windows/submit_abi.h"
#include <memory>

namespace tgd3d {

static_assert(WSI_WIN32_RUNTIME_3D == D3DWDDM2_0DDI_CONTEXTTYPE_3D);
static_assert(WSI_WIN32_RUNTIME_COMPUTE == D3DWDDM2_0DDI_CONTEXTTYPE_COMPUTE);
static_assert(WSI_WIN32_RUNTIME_COPY == D3DWDDM2_0DDI_CONTEXTTYPE_COPY);

struct RuntimeSubmitContext {
   RuntimeSubmitContext *next = nullptr;
   HANDLE handle = nullptr;
   SRWLOCK lock = SRWLOCK_INIT;
   LONG refs = 1;                    // registry reference plus active calls
   UINT sequence = 0;
   bool closing = false;
   bool released = false;
};

struct RuntimeSubmitFence {
   RuntimeSubmitFence *next = nullptr;
   D3DKMT_HANDLE handle = 0;
   const volatile UINT64 *value = nullptr;
   UINT refs = 1;                    // protected by state.lock; zero means failed cleanup
};

struct RuntimeQueueState {
   Device *device = nullptr;
   SRWLOCK lock = SRWLOCK_INIT;
   RuntimeSubmitContext *contexts = nullptr;
   RuntimeSubmitFence *fences = nullptr;
   bool finishing = false;
};

static VkResult result(HRESULT hr)
{
   if (SUCCEEDED(hr)) return VK_SUCCESS;
   if (hr == E_OUTOFMEMORY) return VK_ERROR_OUT_OF_HOST_MEMORY;
   if (hr == D3DDDIERR_OUTOFVIDEOMEMORY) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   if (hr == E_INVALIDARG || hr == DXGI_ERROR_UNSUPPORTED) return VK_ERROR_FEATURE_NOT_PRESENT;
   return VK_ERROR_DEVICE_LOST;
}

static void context_unref(RuntimeSubmitContext *context)
{
   if (!InterlockedDecrement(&context->refs)) delete context;
}

/* The registry lock never spans a runtime callback. An operation retains its
 * context before dropping that lock; the per-context lock serializes calls
 * without blocking callbacks on other contexts or fence bookkeeping. */
class ContextCall {
public:
   RuntimeSubmitContext *context = nullptr;
   ContextCall(RuntimeQueueState *state, void *token, bool closing = false)
   {
      AcquireSRWLockShared(&state->lock);
      for (auto *c = state->contexts; c; c = c->next) {
         if (c == token) { InterlockedIncrement(&c->refs); context = c; break; }
      }
      ReleaseSRWLockShared(&state->lock);
      if (context) {
         AcquireSRWLockExclusive(&context->lock);
         if (context->closing && !closing) {
            ReleaseSRWLockExclusive(&context->lock);
            context_unref(context); context = nullptr;
         }
      }
   }
   ~ContextCall()
   {
      if (context) {
         ReleaseSRWLockExclusive(&context->lock);
         context_unref(context);
      }
   }
};

static VkResult destroy_context(void *opaque, void *token)
{
   auto *state = static_cast<RuntimeQueueState *>(opaque);
   ContextCall call(state, token, true);
   if (!call.context) return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   auto *c = call.context;
   c->closing = true;
   c->released = true;
   if (c->handle) {
      D3DDDICB_DESTROYCONTEXT args = {}; args.hContext = c->handle;
      const HRESULT hr = state->device->kt.pfnDestroyContextCb(state->device->hRTDevice.handle, &args);
      trace(TGD3D_TRACE_RUNTIME_TRANSPORT, hr, UINT64_C(2) << 32, reinterpret_cast<uintptr_t>(c->handle));
      if (FAILED(hr)) return result(hr); // preserve the handle for a later finish
      c->handle = nullptr;
   }
   AcquireSRWLockExclusive(&state->lock);
   auto **link = &state->contexts;
   while (*link && *link != c) link = &(*link)->next;
   if (*link) *link = c->next;
   ReleaseSRWLockExclusive(&state->lock);
   context_unref(c);                  // active ContextCall still owns a reference
   return VK_SUCCESS;
}

static VkResult create_context(void *opaque, uint32_t types, void **token)
{
   if (!token) return VK_ERROR_INITIALIZATION_FAILED;
   *token = nullptr;
   auto *state = static_cast<RuntimeQueueState *>(opaque);
   if (!types || (types & ~(WSI_WIN32_RUNTIME_3D | WSI_WIN32_RUNTIME_COMPUTE | WSI_WIN32_RUNTIME_COPY)))
      return VK_ERROR_FEATURE_NOT_PRESENT;
   if (state->finishing) return VK_ERROR_DEVICE_LOST;
   auto *c = new (std::nothrow) RuntimeSubmitContext;
   if (!c) return VK_ERROR_OUT_OF_HOST_MEMORY;
   D3DWDDM2_0DDICB_CREATECONTEXTVIRTUAL args = {};
   args.EngineAffinity = 1; args.ContextTypeFlags = types;
   trace(TGD3D_TRACE_RUNTIME_TRANSPORT, E_PENDING, (UINT64_C(1) << 32) | types, 0);
   HRESULT hr = state->device->createContextVirtual(state->device->hRTCoreLayer, &args);
   trace(TGD3D_TRACE_RUNTIME_TRANSPORT, hr, (UINT64_C(1) << 32) | types, reinterpret_cast<uintptr_t>(args.hContext));
   c->handle = args.hContext;
   if (!c->handle) { delete c; return FAILED(hr) ? result(hr) : VK_ERROR_INITIALIZATION_FAILED; }
   AcquireSRWLockExclusive(&state->lock);
   bool duplicate = c->handle == state->device->hContext;
   for (auto *existing = state->contexts; existing; existing = existing->next)
      duplicate |= existing->handle == c->handle;
   if (duplicate) {
      ReleaseSRWLockExclusive(&state->lock);
      delete c; // do not steal or destroy an already-owned runtime context
      return VK_ERROR_INITIALIZATION_FAILED;
   }
   c->next = state->contexts; state->contexts = c;
   ReleaseSRWLockExclusive(&state->lock);
   if (FAILED(hr)) {
      destroy_context(state, c); // retain a partial handle if its cleanup also fails
      return result(hr);
   }
   *token = c;
   return VK_SUCCESS;
}

static VkResult submit(void *opaque, void *token, uint64_t address, const void *words,
                        uint32_t bytes, uint32_t count, const uint32_t *written)
{
   if (!address || (address & 3) || address > UINT64_MAX - bytes || !words || !bytes || (bytes & 3) ||
       bytes > TG_GPU_SUBMIT_MAX_BYTES || count > D3DDDI_MAX_WRITTEN_PRIMARIES ||
       (count && !written)) return VK_ERROR_FEATURE_NOT_PRESENT;
   for (UINT i = 0; i < count; ++i) {
      if (!written[i]) return VK_ERROR_INVALID_EXTERNAL_HANDLE;
      for (UINT j = 0; j < i; ++j)
         if (written[i] == written[j]) return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   }
   auto *state = static_cast<RuntimeQueueState *>(opaque);
   ContextCall call(state, token);
   if (!call.context) return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   auto *c = call.context;
   if (c->sequence == 0x7fffffffu) return VK_ERROR_DEVICE_LOST;
   const UINT size = sizeof(tg_gpu_submit_request) + bytes;
   std::unique_ptr<BYTE[]> storage(new (std::nothrow) BYTE[size]);
   if (!storage) return VK_ERROR_OUT_OF_HOST_MEMORY;
   auto *request = reinterpret_cast<tg_gpu_submit_request *>(storage.get());
   *request = {}; request->version = TG_GPU_SUBMIT_ABI; request->size = size; request->command_bytes = bytes;
   MemoryBarrier(); memcpy(request + 1, words, bytes);
   D3DDDICB_SUBMITCOMMAND args = {};
   args.Commands = address; args.CommandLength = bytes;
   args.BroadcastContextCount = 1; args.BroadcastContext[0] = c->handle;
   args.pPrivateDriverData = request; args.PrivateDriverDataSize = size;
   args.NumPrimaries = count;
   if (count) memcpy(args.WrittenPrimaries, written, count * sizeof(*written));
   args.RenderCBSequence = ++c->sequence;
   trace(TGD3D_TRACE_RUNTIME_TRANSPORT, E_PENDING, (UINT64_C(3) << 32) | count, bytes);
   HRESULT hr = state->device->kt.pfnSubmitCommandCb(state->device->hRTDevice.handle, &args);
   trace(TGD3D_TRACE_RUNTIME_TRANSPORT, hr, (UINT64_C(3) << 32) | count, bytes);
   return result(hr);
}

static RuntimeSubmitFence *fence_ref(RuntimeQueueState *state, void *token)
{
   RuntimeSubmitFence *found = nullptr;
   AcquireSRWLockExclusive(&state->lock);
   for (auto *f = state->fences; f; f = f->next) {
      if (f == token && f->refs && f->refs < UINT_MAX) { ++f->refs; found = f; break; }
   }
   ReleaseSRWLockExclusive(&state->lock);
   return found;
}

static HRESULT fence_close(RuntimeQueueState *state, RuntimeSubmitFence *f)
{
   D3DDDICB_DESTROYSYNCHRONIZATIONOBJECT args = {}; args.hSyncObject = f->handle;
   HRESULT hr = state->device->kt.pfnDestroySynchronizationObjectCb(state->device->hRTDevice.handle, &args);
   if (FAILED(hr)) return hr;
   AcquireSRWLockExclusive(&state->lock);
   auto **link = &state->fences;
   while (*link && *link != f) link = &(*link)->next;
   if (*link) *link = f->next;
   ReleaseSRWLockExclusive(&state->lock);
   delete f;
   return S_OK;
}

static void release_fence(void *opaque, void *token)
{
   auto *state = static_cast<RuntimeQueueState *>(opaque);
   RuntimeSubmitFence *close = nullptr;
   AcquireSRWLockExclusive(&state->lock);
   for (auto *f = state->fences; f; f = f->next) {
      if (f == token && f->refs) {
         if (!--f->refs) close = f;
         break;
      }
   }
   ReleaseSRWLockExclusive(&state->lock);
   if (close) fence_close(state, close); // zero-ref failures remain in the registry
}

static VkResult create_fence(void *opaque, uint64_t initial, wsi_win32_runtime_fence *out)
{
   if (!out) return VK_ERROR_INITIALIZATION_FAILED;
   *out = {};
   auto *state = static_cast<RuntimeQueueState *>(opaque);
   if (state->finishing || initial == UINT64_MAX) return VK_ERROR_DEVICE_LOST;
   auto *f = new (std::nothrow) RuntimeSubmitFence;
   if (!f) return VK_ERROR_OUT_OF_HOST_MEMORY;
   D3DDDICB_CREATESYNCHRONIZATIONOBJECT2 args = {};
   args.Info.Type = D3DDDI_MONITORED_FENCE;
   args.Info.Flags.Shared = args.Info.Flags.NtSecuritySharing = 1;
   args.Info.MonitoredFence.EngineAffinity = 1;
   args.Info.MonitoredFence.InitialFenceValue = initial;
   HRESULT hr = state->device->kt.pfnCreateSynchronizationObject2Cb(state->device->hRTDevice.handle, &args);
   f->handle = args.hSyncObject;
   f->value = static_cast<const volatile UINT64 *>(args.Info.MonitoredFence.FenceValueCPUVirtualAddress);
   if (!f->handle) { delete f; return FAILED(hr) ? result(hr) : VK_ERROR_INITIALIZATION_FAILED; }
   AcquireSRWLockExclusive(&state->lock);
   f->next = state->fences; state->fences = f;
   ReleaseSRWLockExclusive(&state->lock);
   if (FAILED(hr) || !f->value || (reinterpret_cast<uintptr_t>(f->value) & 7)) {
      release_fence(state, f);
      return FAILED(hr) ? result(hr) : VK_ERROR_INITIALIZATION_FAILED;
   }
   *out = {f, f->handle, f->value};
   return VK_SUCCESS;
}

static VkResult retain_fence(void *opaque, void *token, wsi_win32_runtime_fence *out)
{
   if (!out) return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   *out = {};
   auto *f = fence_ref(static_cast<RuntimeQueueState *>(opaque), token);
   if (!f) return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   *out = {f, f->handle, f->value};
   return VK_SUCCESS;
}

static VkResult gpu_sync(void *opaque, void *token, bool signal, uint32_t count,
                         const uint32_t *handles, const uint64_t *values)
{
   if (!count || count > WSI_WIN32_RUNTIME_MAX_SYNCS || !handles || !values)
      return VK_ERROR_FEATURE_NOT_PRESENT;
   auto *state = static_cast<RuntimeQueueState *>(opaque);
   ContextCall call(state, token);
   if (!call.context) return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   RuntimeSubmitFence *refs[WSI_WIN32_RUNTIME_MAX_SYNCS] = {};
   UINT retained = 0;
   AcquireSRWLockExclusive(&state->lock);
   for (; retained < count; ++retained) {
      for (auto *f = state->fences; f; f = f->next) {
         if (f->handle == handles[retained] && f->refs && f->refs < UINT_MAX) {
            ++f->refs; refs[retained] = f; break;
         }
      }
      if (!refs[retained] || values[retained] == UINT64_MAX) break;
   }
   ReleaseSRWLockExclusive(&state->lock);
   HRESULT hr = E_INVALIDARG;
   if (retained == count) {
      trace(TGD3D_TRACE_RUNTIME_TRANSPORT, E_PENDING, (uint64_t(signal ? 5 : 4) << 32) | count,
            reinterpret_cast<uintptr_t>(call.context->handle));
      if (signal) {
         D3DDDICB_SIGNALSYNCHRONIZATIONOBJECTFROMGPU args = {};
         args.hContext = call.context->handle; args.ObjectCount = count;
         args.ObjectHandleArray = handles; args.MonitoredFenceValueArray = values;
         hr = state->device->kt.pfnSignalSynchronizationObjectFromGpuCb(state->device->hRTDevice.handle, &args);
      } else {
         D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMGPU args = {};
         args.hContext = call.context->handle; args.ObjectCount = count;
         args.ObjectHandleArray = handles; args.MonitoredFenceValueArray = values;
         hr = state->device->kt.pfnWaitForSynchronizationObjectFromGpuCb(state->device->hRTDevice.handle, &args);
      }
      trace(TGD3D_TRACE_RUNTIME_TRANSPORT, hr, (uint64_t(signal ? 5 : 4) << 32) | count,
            reinterpret_cast<uintptr_t>(call.context->handle));
   }
   for (auto *f : refs) if (f) release_fence(state, f);
   return result(hr);
}

static VkResult wait_cpu(void *opaque, void *token, uint64_t value, uint32_t timeout_ms)
{
   if (value == UINT64_MAX) return VK_ERROR_DEVICE_LOST;
   auto *state = static_cast<RuntimeQueueState *>(opaque);
   auto *f = fence_ref(state, token);
   if (!f) return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   MemoryBarrier(); const UINT64 current = *f->value;
   VkResult status = current == UINT64_MAX ? VK_ERROR_DEVICE_LOST :
                     current >= value ? VK_SUCCESS : VK_TIMEOUT;
   if (status == VK_TIMEOUT && timeout_ms) {
      HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
      if (!event) status = VK_ERROR_OUT_OF_HOST_MEMORY;
      else {
         D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMCPU args = {};
         args.ObjectCount = 1; args.ObjectHandleArray = &f->handle;
         args.FenceValueArray = &value; args.hAsyncEvent = event;
         HRESULT hr = state->device->kt.pfnWaitForSynchronizationObjectFromCpuCb(state->device->hRTDevice.handle, &args);
         if (FAILED(hr)) status = result(hr);
         else {
            DWORD waited = WaitForSingleObject(event, timeout_ms);
            MemoryBarrier(); const UINT64 completed = *f->value;
            status = completed == UINT64_MAX ? VK_ERROR_DEVICE_LOST :
                     waited == WAIT_TIMEOUT ? VK_TIMEOUT :
                     waited == WAIT_OBJECT_0 && completed >= value ? VK_SUCCESS : VK_ERROR_DEVICE_LOST;
         }
         CloseHandle(event);
      }
   }
   release_fence(state, f);
   return status;
}

static VkResult signal_cpu(void *opaque, void *token, uint64_t value)
{
   auto *state = static_cast<RuntimeQueueState *>(opaque);
   auto *f = fence_ref(state, token);
   if (!f) return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   D3DDDICB_SIGNALSYNCHRONIZATIONOBJECTFROMCPU args = {};
   args.ObjectCount = 1; args.ObjectHandleArray = &f->handle; args.FenceValueArray = &value;
   VkResult status = value == UINT64_MAX ? VK_ERROR_DEVICE_LOST :
      result(state->device->kt.pfnSignalSynchronizationObjectFromCpuCb(state->device->hRTDevice.handle, &args));
   release_fence(state, f);
   return status;
}

static VkResult quiesce(void *opaque)
{
   auto *state = static_cast<RuntimeQueueState *>(opaque);
   /* The caller has excluded new DDIs and submission callbacks. Destroy is
    * a runtime completion boundary; never manufacture a fence signal here. */
   state->finishing = true;
   VkResult status = VK_SUCCESS;
   for (auto *c = state->contexts; c; c = c->next) {
      ContextCall call(state, c, true);
      c->closing = true;
      if (!c->handle) continue;
      D3DDDICB_DESTROYCONTEXT args = {}; args.hContext = c->handle;
      const HRESULT hr = state->device->kt.pfnDestroyContextCb(state->device->hRTDevice.handle, &args);
      trace(TGD3D_TRACE_RUNTIME_TRANSPORT, hr, UINT64_C(2) << 32, reinterpret_cast<uintptr_t>(c->handle));
      if (SUCCEEDED(hr)) c->handle = nullptr;
      else status = result(hr);
   }
   return status;
}

HRESULT runtime_queue_quiesce(Device *dev)
{
   return !dev->runtimeQueue || quiesce(dev->runtimeQueue) == VK_SUCCESS ? S_OK : D3DDDIERR_DEVICEREMOVED;
}

HRESULT runtime_queue_prepare(Device *dev, wsi_win32_runtime *out)
{
   if (!dev || !out) return E_INVALIDARG;
   *out = {};
   if (dev->runtimeQueue) return DXGI_ERROR_INVALID_CALL;
   const auto &cb = dev->kt;
   if (!dev->nvk || dev->interfaceVersion != D3DWDDM2_0_DDI_INTERFACE_VERSION ||
       !dev->hRTDevice.handle || !dev->hRTCoreLayer.handle || !dev->createContextVirtual ||
       !cb.pfnDestroyContextCb || !cb.pfnSubmitCommandCb ||
       !cb.pfnCreateSynchronizationObject2Cb || !cb.pfnDestroySynchronizationObjectCb ||
       !cb.pfnWaitForSynchronizationObjectFromGpuCb || !cb.pfnSignalSynchronizationObjectFromGpuCb ||
       !cb.pfnWaitForSynchronizationObjectFromCpuCb || !cb.pfnSignalSynchronizationObjectFromCpuCb)
      return DXGI_ERROR_UNSUPPORTED;
   auto *state = new (std::nothrow) RuntimeQueueState;
   if (!state) return E_OUTOFMEMORY;
   state->device = dev; dev->runtimeQueue = state;
   *out = {WSI_WIN32_RUNTIME_VERSION, state, dev->hRTDevice.handle,
      create_context, destroy_context, submit, gpu_sync,
      create_fence, retain_fence, release_fence, wait_cpu, signal_cpu, quiesce};
   dev->runtimeTransport = *out;
   return S_OK;
}

HRESULT runtime_queue_finish(Device *dev)
{
   auto *state = dev ? dev->runtimeQueue : nullptr;
   if (!state) return S_OK;
   /* Caller has stopped all inline callbacks. Live tokens mean the adopter
    * has not finished teardown; do not destroy objects behind its back. */
   for (auto *c = state->contexts; c; c = c->next)
      if (!c->released || c->refs != 1) return DXGI_ERROR_INVALID_CALL;
   for (auto *f = state->fences; f; f = f->next)
      if (f->refs) return DXGI_ERROR_INVALID_CALL;
   state->finishing = true;
   for (auto *c = state->contexts, *next = c ? c->next : nullptr; c; c = next, next = c ? c->next : nullptr)
      destroy_context(state, c);
   for (auto *f = state->fences, *next = f ? f->next : nullptr; f; f = next, next = f ? f->next : nullptr)
      fence_close(state, f);
   if (state->contexts || state->fences) return D3DDDIERR_DEVICEREMOVED;
   delete state; dev->runtimeQueue = nullptr;
   dev->runtimeTransport = {}; dev->runtimeQueueAccepted = false;
   return S_OK;
}

}
