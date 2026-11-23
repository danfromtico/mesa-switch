/* SPDX-License-Identifier: MIT
 *
 * OpenAdapter entry points exported to the Direct3D runtime.
 */
#include "tgd3d.h"

#include <string.h>

using namespace tgd3d;

HRESULT tgd3d::close_native_adapter(Adapter *adapter)
{
   if (adapter->undrainedDevices) {
      /* The DDI handles end here. These independently owned devices still
       * have ICD children. Never destroy their instance or unload the ICD
       * behind them. No callback is scheduled from this process-lifetime list. */
      static SRWLOCK lock = SRWLOCK_INIT;
      static Adapter *retained;
      AcquireSRWLockExclusive(&lock);
      adapter->nextUndrained = retained;
      retained = adapter;
      ReleaseSRWLockExclusive(&lock);
      return S_OK;
   }
   HMODULE module = adapter->umdReference;
   adapter->vk.close();
   delete adapter;
   if (module) FreeLibrary(module); /* the caller still holds its own UMD reference */
   return S_OK;
}

static HRESULT APIENTRY CloseAdapter(D3D10DDI_HADAPTER hAdapter)
{
   return close_native_adapter(static_cast<Adapter *>(hAdapter.pDrvPrivate));
}

/* Ordinary processes retain the accepted negotiation. WDDM 2.0 is offered
 * only to an explicitly opted-in NVK application, never to the desktop by
 * installing this UMD alone. */
static const UINT64 supported_versions[] = {
   D3D10_0_DDI_SUPPORTED,
   D3D10_0_x_DDI_SUPPORTED,
   D3D10_0_7_DDI_SUPPORTED,
   D3D11_1_DDI_SUPPORTED,
};

static HRESULT APIENTRY GetSupportedVersions(D3D10DDI_HADAPTER h, UINT32 *count, UINT64 *versions)
{
   if (!h.pDrvPrivate || !count) return E_INVALIDARG;
   wchar_t application[2] = {};
   const bool modern = static_cast<const Adapter *>(h.pDrvPrivate)->vk.nvk &&
      GetEnvironmentVariableW(L"TGD3D_WDDM20_APPLICATION", application, std::size(application)) == 1 &&
      application[0] == L'1';
   const UINT32 legacy = std::size(supported_versions);
   const UINT32 available = legacy + (modern ? 1 : 0);
   const UINT32 capacity = *count;
   *count = available;
   if (versions && capacity < available)
      return E_OUTOFMEMORY;
   if (versions) {
      memcpy(versions, supported_versions, sizeof(supported_versions));
      if (modern) versions[legacy] = D3DWDDM2_0_DDI_SUPPORTED;
   }
   return S_OK;
}

template<typename T>
static HRESULT write_caps(const D3D10_2DDIARG_GETCAPS *args, const T &value = {})
{
   if (!args->pData || args->DataSize != sizeof(T)) return E_INVALIDARG;
   memcpy(args->pData, &value, sizeof(T));
   return S_OK;
}

/* Compute one mask for the runtime query, size negotiation and creation.
 * Higher pipeline levels use the native SM5 stages and descriptor limits. */
UINT tgd3d::native_pipeline_levels(const Adapter *adapter)
{
   constexpr UINT level10 = D3D11DDI_ENCODE_3DPIPELINESUPPORT_CAP(D3D11DDI_3DPIPELINELEVEL_10_0);
   if (!adapter || !adapter->vk.nvk) return level10;
   const auto &c = adapter->vk.caps;
   if (!c.compute || !c.geometryShader || !c.tessellationShader || !c.transformFeedback ||
       !c.pipelineStatistics || !c.drawIndirectFirstInstance || !c.textureCompressionBC ||
       c.maxColorAttachments < 8 || c.maxViewports < 16 || c.maxImageDimension2D < 16384 ||
       c.maxImageDimension3D < 2048 || c.maxImageArrayLayers < 2048 ||
       !(c.graphicsUavStages & VK_SHADER_STAGE_FRAGMENT_BIT) || c.graphicsUavSlots < 8)
      return level10;

   UINT levels = level10 |
      D3D11DDI_ENCODE_3DPIPELINESUPPORT_CAP(D3D11DDI_3DPIPELINELEVEL_10_1) |
      D3D11DDI_ENCODE_3DPIPELINESUPPORT_CAP(D3D11DDI_3DPIPELINELEVEL_11_0);
   constexpr VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
      VK_SHADER_STAGE_GEOMETRY_BIT | VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT |
      VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
   if (c.logicOp && (c.graphicsUavStages & stages) == stages && c.graphicsUavSlots >= 64 &&
       ((c.attachmentlessSamples | c.tirSamples) & VK_SAMPLE_COUNT_16_BIT))
      levels |= D3D11DDI_ENCODE_3DPIPELINESUPPORT_CAP(D3D11_1DDI_3DPIPELINELEVEL_11_1);
   return levels;
}

static HRESULT APIENTRY GetCaps(D3D10DDI_HADAPTER h, const D3D10_2DDIARG_GETCAPS *args)
{
   if (!h.pDrvPrivate || !args) return E_INVALIDARG;
   switch (args->Type) {
   case D3D11DDICAPS_3DPIPELINESUPPORT: {
      const auto *adapter = static_cast<const Adapter *>(h.pDrvPrivate);
      const UINT levels = native_pipeline_levels(adapter);
      const HRESULT result = write_caps(args, D3D11DDI_3DPIPELINESUPPORT_CAPS{levels});
      trace(TGD3D_TRACE_PIPELINE_LEVELS, result, levels, adapter->vk.caps.graphicsUavSlots);
      return result;
   }
   case D3D11DDICAPS_THREADING: return write_caps<D3D11DDI_THREADING_CAPS>(args);
   case D3D11DDICAPS_SHADER: {
      D3D11DDI_SHADER_CAPS shader{};
      const auto *adapter = static_cast<const Adapter *>(h.pDrvPrivate);
      if (adapter->vk.caps.downlevelCompute)
         shader.Caps |= D3D11DDICAPS_SHADER_COMPUTE_PLUS_RAW_AND_STRUCTURED_BUFFERS_IN_SHADER_4_X;
      return write_caps(args, shader);
   }
   case D3D11_1DDICAPS_D3D11_OPTIONS: {
      const auto *adapter = static_cast<const Adapter *>(h.pDrvPrivate);
      D3D11_1DDI_D3D11_OPTIONS_DATA options = {};
      options.OutputMergerLogicOp = adapter->vk.caps.logicOp;
      return write_caps(args, options);
   }
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

HRESULT tgd3d::open_native_adapter(HANDLE runtime_handle, const D3DDDI_ADAPTERCALLBACKS *callbacks,
                                   Adapter **out)
{
   if (!callbacks || !callbacks->pfnQueryAdapterInfoCb)
      return E_INVALIDARG;
   tg_gpu_adapter_info info = {};
   info.version = TG_GPU_ADAPTER_ABI;
   info.size = sizeof(info);
   D3DDDICB_QUERYADAPTERINFO query = {};
   query.pPrivateDriverData = &info;
   query.PrivateDriverDataSize = sizeof(info);
   HRESULT hr = callbacks->pfnQueryAdapterInfoCb(runtime_handle, &query);
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
   adapter->hRTAdapter = runtime_handle;
   adapter->callbacks = callbacks;
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
   /* A later failed final GPU wait can outlive the DDI device/adapter.
    * Acquire a counted module reference while failure can still unwind. */
   if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           reinterpret_cast<LPCWSTR>(&CloseAdapter), &adapter->umdReference)) {
      const HRESULT error = HRESULT_FROM_WIN32(GetLastError());
      adapter->vk.close();
      delete adapter;
      return error;
   }
   *out = adapter;
   return S_OK;
}

static HRESULT open_adapter(D3D10DDIARG_OPENADAPTER *args)
{
   trace(TGD3D_TRACE_OPEN_ADAPTER, S_OK, args->Interface, args->Version);
   Adapter *adapter = nullptr;
   HRESULT hr = open_native_adapter(args->hRTAdapter.handle, args->pAdapterCallbacks, &adapter);
   if (FAILED(hr)) return hr;
   video_adapter_init(adapter);
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
