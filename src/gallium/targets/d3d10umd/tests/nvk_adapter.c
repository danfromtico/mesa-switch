/* SPDX-License-Identifier: MIT */
#include <windows.h>
#include <d3d9.h>
#include "winddk_compat.h"
#include <d3dumddi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nouveau/vulkan/nvkmd/windows/adapter_abi.h"
#include "zink/zink_public.h"
#include "Trace.h"

/* This fixture tests adapter selection, not the ETW transport. */
void D3D10Trace(enum d3d10_umd_trace_event event, HRESULT result, uint64_t a, uint64_t b)
{
   (void)event; (void)result; (void)a; (void)b;
}

void D3D10TraceMessage(const char *message) { (void)message; }

#define CHECK(x) do { if (!(x)) { \
   fprintf(stderr, "line %u: %s\n", __LINE__, #x); exit(1); \
} } while (0)

extern HRESULT d3d10_create_screen(HANDLE, const D3DDDI_ADAPTERCALLBACKS *,
                                   struct pipe_screen **);

static struct tg_gpu_adapter_info reply;
static HRESULT query_result;
static unsigned queries, screens;
static uint64_t selected_luid;
static struct pipe_screen *backend_screen;

static HRESULT APIENTRY
query_adapter(HANDLE adapter, const D3DDDICB_QUERYADAPTERINFO *query)
{
   CHECK(adapter == (HANDLE)(uintptr_t)37);
   CHECK(query->PrivateDriverDataSize == sizeof(reply));
   const struct tg_gpu_adapter_query *request = query->pPrivateDriverData;
   CHECK(request->version == TG_GPU_ADAPTER_ABI && request->size == sizeof(reply));
   ++queries;
   memcpy(query->pPrivateDriverData, &reply, sizeof(reply));
   return query_result;
}

struct pipe_screen *
zink_win32_create_screen_with_error(uint64_t luid, struct zink_screen_create_error *error)
{
   *error = (struct zink_screen_create_error){0};
   ++screens;
   selected_luid = luid;
   return backend_screen;
}

int main(void)
{
   const D3DDDI_ADAPTERCALLBACKS callbacks = {.pfnQueryAdapterInfoCb = query_adapter};
   const HANDLE adapter = (HANDLE)(uintptr_t)37;
   struct pipe_screen *screen = (void *)(uintptr_t)1;
   CHECK(d3d10_create_screen(adapter, NULL, &screen) == E_INVALIDARG && !screen);
   reply = (struct tg_gpu_adapter_info) {
      .version = TG_GPU_ADAPTER_ABI, .size = sizeof(reply),
      .flags = TG_GPU_ADAPTER_LUID_VALID | TG_GPU_ADAPTER_NATIVE_SUBMIT,
      .vendor = 0x10de, .chipset = 0x12e,
      .memory_budget_bytes = TG_GPU_MEMORY_BUDGET_BYTES,
      .luid_low = 0x76543210, .luid_high = (int32_t)0x89abcdef,
   };
   backend_screen = (void *)(uintptr_t)123;
   CHECK(d3d10_create_screen(adapter, &callbacks, &screen) == S_OK);
   CHECK(screen == backend_screen && queries == 1 && screens == 1 &&
         selected_luid == UINT64_C(0x89abcdef76543210));

   query_result = E_ACCESSDENIED;
   CHECK(d3d10_create_screen(adapter, &callbacks, &screen) == E_ACCESSDENIED && !screen);
   CHECK(screens == 1);
   query_result = S_OK;
   reply.flags &= ~TG_GPU_ADAPTER_NATIVE_SUBMIT;
   CHECK(d3d10_create_screen(adapter, &callbacks, &screen) == DXGI_ERROR_UNSUPPORTED && !screen);
   reply.flags |= TG_GPU_ADAPTER_NATIVE_SUBMIT;
   reply.luid_low = reply.luid_high = 0;
   CHECK(d3d10_create_screen(adapter, &callbacks, &screen) == DXGI_ERROR_UNSUPPORTED && !screen);
   reply.luid_low = 1;
   reply.chipset = 0x130;
   CHECK(d3d10_create_screen(adapter, &callbacks, &screen) == DXGI_ERROR_UNSUPPORTED && !screen);
   reply.chipset = 0x12e;
   reply.size = 16;
   CHECK(d3d10_create_screen(adapter, &callbacks, &screen) == DXGI_ERROR_UNSUPPORTED && !screen);
   reply.size = sizeof(reply);
   reply.memory_budget_bytes = TG_GPU_MEMORY_BUDGET_BYTES + 1;
   CHECK(d3d10_create_screen(adapter, &callbacks, &screen) == DXGI_ERROR_UNSUPPORTED && !screen);
   CHECK(screens == 1);
   reply.memory_budget_bytes = TG_GPU_MEMORY_BUDGET_BYTES;
   backend_screen = NULL;
   CHECK(d3d10_create_screen(adapter, &callbacks, &screen) == DXGI_ERROR_UNSUPPORTED && !screen);
   CHECK(screens == 2 && selected_luid == 1);
   puts("NVK UMD adapter callback, exact LUID, capability gate and failure propagation pass");
   return 0;
}
