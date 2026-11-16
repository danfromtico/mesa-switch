/* SPDX-License-Identifier: MIT
 *
 * OpenAdapter entry points exported to the Direct3D runtime.
 */
#include "tgd3d.h"

#include <string.h>

using namespace tgd3d;

static HRESULT APIENTRY CloseAdapter(D3D10DDI_HADAPTER hAdapter)
{
   Adapter *adapter = static_cast<Adapter *>(hAdapter.pDrvPrivate);
   adapter->vk.close();
   delete adapter;
   return S_OK;
}

/* Keep the working D3D10 admission. The WDDM2.0/DXGI1.4 table remains under
 * control tests after the 0b9b7044 native trial failed before any mode switch.
 * Do not automatically expose that path in subsequent ordinary builds. */
static const UINT64 supported_versions[] = {
   D3D10_0_DDI_SUPPORTED,
   D3D10_0_x_DDI_SUPPORTED,
   D3D10_0_7_DDI_SUPPORTED,
};

static HRESULT APIENTRY GetSupportedVersions(D3D10DDI_HADAPTER, UINT32 *count, UINT64 *versions)
{
   if (!count) return E_INVALIDARG;
   const UINT32 available = sizeof(supported_versions) / sizeof(supported_versions[0]);
   const UINT32 capacity = *count;
   *count = available;
   if (versions && capacity < available)
      return E_OUTOFMEMORY;
   if (versions)
      memcpy(versions, supported_versions, sizeof(supported_versions));
   return S_OK;
}

template<typename T>
static HRESULT write_caps(const D3D10_2DDIARG_GETCAPS *args, const T &value = {})
{
   if (!args->pData || args->DataSize != sizeof(T)) return E_INVALIDARG;
   memcpy(args->pData, &value, sizeof(T));
   return S_OK;
}

static HRESULT APIENTRY GetCaps(D3D10DDI_HADAPTER h, const D3D10_2DDIARG_GETCAPS *args)
{
   if (!h.pDrvPrivate || !args) return E_INVALIDARG;
   /* An interface revision is not a new pipeline level. Keep the existing
    * D3D10 pipeline; no compute, tessellation, UAV, command lists or FL11/12.
    * Format and sample support are still queried independently on the device. */
   switch (args->Type) {
   case D3D11DDICAPS_3DPIPELINESUPPORT:
      return write_caps(args, D3D11DDI_3DPIPELINESUPPORT_CAPS{
         D3D11DDI_ENCODE_3DPIPELINESUPPORT_CAP(D3D11DDI_3DPIPELINELEVEL_10_0)});
   case D3D11DDICAPS_THREADING: return write_caps<D3D11DDI_THREADING_CAPS>(args);
   case D3D11DDICAPS_SHADER: return write_caps<D3D11DDI_SHADER_CAPS>(args);
   case D3D11_1DDICAPS_D3D11_OPTIONS: return write_caps<D3D11_1DDI_D3D11_OPTIONS_DATA>(args);
   case D3D11_1DDICAPS_ARCHITECTURE_INFO: return write_caps<D3D11_1DDI_ARCHITECTURE_INFO_DATA>(args);
   case D3D11_1DDICAPS_SHADER_MIN_PRECISION_SUPPORT:
      return write_caps<D3D11_DDI_SHADER_MIN_PRECISION_SUPPORT_DATA>(args);
   case D3DWDDM1_3DDICAPS_D3D11_OPTIONS1: return write_caps<D3DWDDM1_3DDI_D3D11_OPTIONS_DATA1>(args);
   case D3DWDDM1_3DDICAPS_MARKER: return write_caps(args, D3DWDDM1_3DDI_MARKER_TYPE_NONE);
   case D3DWDDM2_0DDICAPS_D3D11_OPTIONS2: return write_caps<D3DWDDM2_0DDI_D3D11_OPTIONS2_DATA>(args);
   case D3DWDDM2_0DDICAPS_D3D11_OPTIONS3: return write_caps<D3DWDDM2_0DDI_D3D11_OPTIONS3_DATA>(args);
   case D3DWDDM2_0DDICAPS_MEMORY_ARCHITECTURE:
      /* Tegra shares system RAM; CPU/GPU coherence is not implied by UMA. */
      return write_caps(args, D3DWDDM2_0DDI_MEMORY_ARCHITECTURE_CAPS{TRUE, FALSE});
   case D3DWDDM2_0DDICAPS_GPUVA_CAPS:
      /* Matching .158 KMD's two-level GPU MMU has a 40-bit virtual address. */
      return write_caps(args, D3DWDDM2_0DDI_GPUVA_CAPS_DATA{40});
   case D3DWDDM2_0DDICAPS_TEXTURE_LAYOUT:
      if (args->pInfo) return E_INVALIDARG; // no enumerated caller-selected layouts
      return write_caps<D3DWDDM2_0DDI_TEXTURE_LAYOUT_CAPS>(args);
   case D3DWDDM2_2DDICAPS_SHADERCACHE:
      return write_caps<D3DWDDM2_2DDI_SHADERCACHE_DATA>(args); // own bounded cache, not runtime cache
   default:
      return E_NOTIMPL; // never manufacture a successful layout/format answer
   }
}

void tgd3d::adapter_fill_queries(D3D10_2DDI_ADAPTERFUNCS *table)
{
   table->pfnGetSupportedVersions = GetSupportedVersions;
   table->pfnGetCaps = GetCaps;
}

static HRESULT open_adapter(D3D10DDIARG_OPENADAPTER *args)
{
   trace(TGD3D_TRACE_OPEN_ADAPTER, S_OK, args->Interface, args->Version);
   if (!args->pAdapterCallbacks || !args->pAdapterCallbacks->pfnQueryAdapterInfoCb)
      return E_INVALIDARG;
   tg_gpu_adapter_info info = {};
   info.version = TG_GPU_ADAPTER_ABI;
   info.size = sizeof(info);
   D3DDDICB_QUERYADAPTERINFO query = {};
   query.pPrivateDriverData = &info;
   query.PrivateDriverDataSize = sizeof(info);
   HRESULT hr = args->pAdapterCallbacks->pfnQueryAdapterInfoCb(args->hRTAdapter.handle, &query);
   trace(TGD3D_TRACE_ADAPTER_INFO, hr, info.flags, info.memory_budget_bytes);
   if (FAILED(hr))
      return hr;
   /* The kernel driver must be the matching Tegra miniport with native
    * submission enabled; anything else is not this driver's adapter. */
   const uint32_t required = TG_GPU_ADAPTER_LUID_VALID | TG_GPU_ADAPTER_NATIVE_SUBMIT;
   const uint64_t luid = (uint64_t)(uint32_t)info.luid_high << 32 | info.luid_low;
   if (info.version != TG_GPU_ADAPTER_ABI || info.size != sizeof(info) || info.reserved ||
       (info.flags & required) != required || info.vendor != 0x10de || info.chipset != 0x12e ||
       !info.memory_budget_bytes || info.memory_budget_bytes > TG_GPU_MEMORY_BUDGET_BYTES || !luid)
      return DXGI_ERROR_UNSUPPORTED;

   Adapter *adapter = new (std::nothrow) Adapter();
   if (!adapter)
      return E_OUTOFMEMORY;
   adapter->hRTAdapter = args->hRTAdapter.handle;
   adapter->callbacks = args->pAdapterCallbacks;
   adapter->info = info;
   adapter->luid = luid;
   hr = adapter->vk.open(luid);
   trace(TGD3D_TRACE_VULKAN, hr, luid,
         (uint64_t)adapter->vk.nvk << 32 | adapter->vk.caps.apiVersion);
   if (FAILED(hr)) {
      trace_message(adapter->vk.failure);
      delete adapter;
      return hr;
   }
   args->hAdapter.pDrvPrivate = adapter;
   args->pAdapterFuncs->pfnCalcPrivateDeviceSize = CalcPrivateDeviceSize;
   args->pAdapterFuncs->pfnCreateDevice = CreateDevice;
   args->pAdapterFuncs->pfnCloseAdapter = CloseAdapter;
   return S_OK;
}

extern "C" HRESULT APIENTRY OpenAdapter10(D3D10DDIARG_OPENADAPTER *args)
{
   switch (args->Interface) {
   case D3D10_0_DDI_INTERFACE_VERSION:
   case D3D10_0_x_DDI_INTERFACE_VERSION:
   case D3D10_0_7_DDI_INTERFACE_VERSION:
      return open_adapter(args);
   default:
      trace(TGD3D_TRACE_OPEN_ADAPTER, E_FAIL, args->Interface, args->Version);
      return E_FAIL;
   }
}

extern "C" HRESULT APIENTRY OpenAdapter10_2(D3D10DDIARG_OPENADAPTER *args)
{
   HRESULT hr = open_adapter(args);
   if (SUCCEEDED(hr)) {
      adapter_fill_queries(args->pAdapterFuncs_2);
   }
   return hr;
}
