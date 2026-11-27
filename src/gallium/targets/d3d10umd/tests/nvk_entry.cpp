/* SPDX-License-Identifier: MIT */
#include <windows.h>
#include "winddk_compat.h"
#include <d3d10_1.h>
#include <d3d10umddi.h>
#include <stdio.h>
#include <stdlib.h>

#include "nouveau/vulkan/nvkmd/windows/adapter_abi.h"

#define CHECK(x) do { if (!(x)) { \
   fprintf(stderr, "line %u: %s\n", __LINE__, #x); exit(1); \
} } while (0)

static HRESULT query_result = E_ACCESSDENIED;
static unsigned queries;

static HRESULT APIENTRY
query_adapter(HANDLE adapter, const D3DDDICB_QUERYADAPTERINFO *query)
{
   CHECK(adapter == (HANDLE)(uintptr_t)37);
   CHECK(query->PrivateDriverDataSize == sizeof(tg_gpu_adapter_info));
   auto *info = static_cast<tg_gpu_adapter_info *>(query->pPrivateDriverData);
   CHECK(info->version == TG_GPU_ADAPTER_ABI && info->size == sizeof(*info));
   ++queries;
   info->flags = TG_GPU_ADAPTER_LUID_VALID; /* Native submissions not enabled. */
   info->vendor = 0x10de;
   info->chipset = 0x12e;
   info->memory_budget_bytes = TG_GPU_MEMORY_BUDGET_BYTES;
   info->luid_low = 1;
   return query_result;
}

int main(int argc, char **argv)
{
   CHECK(argc == 2);
   HMODULE dll = LoadLibraryA(argv[1]);
   if (!dll)
      fprintf(stderr, "LoadLibrary error %lu\n", GetLastError());
   CHECK(dll);
   CHECK(!GetProcAddress(dll, "D3DKMTCreateDevice"));
   CHECK(!GetProcAddress(dll, "D3DKMTQueryAdapterInfo"));
   auto open = reinterpret_cast<PFND3D10DDI_OPENADAPTER>(GetProcAddress(dll, "OpenAdapter10"));
   auto open2 = reinterpret_cast<PFND3D10DDI_OPENADAPTER>(GetProcAddress(dll, "OpenAdapter10_2"));
   CHECK(open && open2);
   D3DDDI_ADAPTERCALLBACKS callbacks = {};
   callbacks.pfnQueryAdapterInfoCb = query_adapter;
   D3D10_2DDI_ADAPTERFUNCS functions = {};
   D3D10DDIARG_OPENADAPTER args = {};
   args.hRTAdapter.handle = (HANDLE)(uintptr_t)37;
   args.Interface = UINT32_MAX;
   args.pAdapterCallbacks = &callbacks;
   args.pAdapterFuncs_2 = &functions;
   CHECK(FAILED(open(&args)) && !queries && !args.hAdapter.pDrvPrivate);
   /* OpenAdapter10_2 negotiates versions later, and must query the adapter
    * even when this initial Interface value is not one it supports. */
   CHECK(open2(&args) == E_ACCESSDENIED && queries == 1 && !args.hAdapter.pDrvPrivate);
   query_result = S_OK;
   CHECK(open2(&args) == DXGI_ERROR_UNSUPPORTED && queries == 2 && !args.hAdapter.pDrvPrivate);
   args.Interface = D3D10_0_DDI_INTERFACE_VERSION;
   CHECK(open(&args) == DXGI_ERROR_UNSUPPORTED && queries == 3 && !args.hAdapter.pDrvPrivate);
   CHECK(!functions.pfnCreateDevice && !functions.pfnCloseAdapter);
   CHECK(FreeLibrary(dll));
   puts("NVK UMD DLL load, hardware exports and runtime callback rejection pass (controlled callbacks)");
   return 0;
}
