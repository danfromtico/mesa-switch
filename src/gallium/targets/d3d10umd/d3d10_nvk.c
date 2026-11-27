/* SPDX-License-Identifier: MIT */
#include <windows.h>
#include <d3d9.h>
#include "winddk_compat.h"
#include <d3dumddi.h>

#include "nouveau/vulkan/nvkmd/windows/adapter_abi.h"
#include "zink/zink_public.h"
#include "Trace.h"

extern HRESULT
d3d10_create_screen(HANDLE hRTAdapter, const D3DDDI_ADAPTERCALLBACKS *callbacks,
                    struct pipe_screen **screen);

HRESULT
d3d10_create_screen(HANDLE hRTAdapter, const D3DDDI_ADAPTERCALLBACKS *callbacks,
                    struct pipe_screen **screen)
{
   *screen = NULL;
   if (!callbacks || !callbacks->pfnQueryAdapterInfoCb)
      return E_INVALIDARG;

   struct tg_gpu_adapter_info info = {
      .version = TG_GPU_ADAPTER_ABI,
      .size = sizeof(info),
   };
   D3DDDICB_QUERYADAPTERINFO query = {
      .pPrivateDriverData = &info,
      .PrivateDriverDataSize = sizeof(info),
   };
   HRESULT hr = callbacks->pfnQueryAdapterInfoCb(hRTAdapter, &query);
   D3D10Trace(D3D10_TRACE_SCREEN_QUERY, hr, info.flags, info.memory_budget_bytes);
   if (FAILED(hr))
      return hr;

   const uint32_t required = TG_GPU_ADAPTER_LUID_VALID | TG_GPU_ADAPTER_NATIVE_SUBMIT;
   if (info.version != TG_GPU_ADAPTER_ABI || info.size != sizeof(info) ||
       info.reserved || (info.flags & required) != required ||
       info.vendor != 0x10de || info.chipset != 0x12e ||
       !info.memory_budget_bytes || info.memory_budget_bytes > TG_GPU_MEMORY_BUDGET_BYTES) {
      D3D10Trace(D3D10_TRACE_SCREEN_IDENTITY, DXGI_ERROR_UNSUPPORTED, info.version, info.flags);
      return DXGI_ERROR_UNSUPPORTED;
   }

   const uint64_t luid = (uint64_t)(uint32_t)info.luid_high << 32 | info.luid_low;
   /* Zero is Zink's automatic-selection sentinel, not an adapter identity. */
   if (!luid) {
      D3D10Trace(D3D10_TRACE_SCREEN_IDENTITY, DXGI_ERROR_UNSUPPORTED, 0, info.flags);
      return DXGI_ERROR_UNSUPPORTED;
   }

   D3D10Trace(D3D10_TRACE_ZINK_BEGIN, S_OK, luid, 0);
   struct zink_screen_create_error error = {0};
   *screen = zink_win32_create_screen_with_error(luid, &error);
   const struct zink_instance_diagnostics *instance = &error.instance;
   for (unsigned i = 0; i < ZINK_INSTANCE_CALL_COUNT; i++) {
      if (instance->called_mask & (1u << i))
         D3D10Trace(D3D10_TRACE_ZINK_INSTANCE_CALL, instance->calls[i].result,
                   i, instance->calls[i].value);
   }
   if (instance->called_mask)
      D3D10Trace(D3D10_TRACE_ZINK_INSTANCE_INFO, S_OK,
                ((uint64_t)instance->called_mask << 32) | instance->flags,
                ((uint64_t)instance->enabled_extensions << 32) | instance->enabled_layers);
   if (!*screen) {
      D3D10Trace(D3D10_TRACE_ZINK_FAILURE, DXGI_ERROR_UNSUPPORTED,
                ((uint64_t)error.stage << 32) | error.line,
                ((uint64_t)error.detail << 32) | (uint32_t)error.result);
      D3D10TraceMessage(error.message);
   }
   D3D10Trace(D3D10_TRACE_ZINK_END, *screen ? S_OK : DXGI_ERROR_UNSUPPORTED, luid, 0);
   return *screen ? S_OK : DXGI_ERROR_UNSUPPORTED;
}
