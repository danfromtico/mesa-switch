/* SPDX-License-Identifier: MIT
 * D3D9 adapter and capability queries for the Windows runtime. */
#include "tgd3d_legacy9.h"
#include "vulkan/util/vk_internal_exts.h"
#include <cstring>

using namespace tgd3d;

namespace {

enum Operation : uint32_t { Open = 1, Capabilities, DeviceCreation, Close };

void report(Operation operation, UINT selector, HRESULT result, uint64_t value)
{
   trace(TGD3D_TRACE_LEGACY9, result, (uint64_t(operation) << 32) | selector, value);
}

template<typename T>
HRESULT write(const D3DDDIARG_GETCAPS &query, const T &value)
{
   if (!query.pData || query.DataSize != sizeof(T))
      return E_INVALIDARG;
   std::memcpy(query.pData, &value, sizeof(T));
   return S_OK;
}

UINT display_formats(Adapter *adapter, FORMATOP *formats)
{
   // Both D3D9 formats use BGRA8 storage. Query the image presented to the
   // ICD, including the WDDM row-pitch contract used for window backbuffers.
   const VkFormat family[] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB};
   VkImageFormatListCreateInfo list{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
   list.viewFormatCount = 2; list.pViewFormats = family;
   wsi_image_create_info layout{};
   layout.sType = VK_STRUCTURE_TYPE_WSI_IMAGE_CREATE_INFO_MESA;
   layout.win32_row_pitch = 128;
   layout.pNext = &list;
   VkPhysicalDeviceExternalImageFormatInfo external{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
   external.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
   external.pNext = &list;
   VkPhysicalDeviceImageFormatInfo2 image{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
   image.pNext = adapter->vk.nvk ? static_cast<const void *>(&layout) : static_cast<const void *>(&external);
   image.format = family[0]; image.type = VK_IMAGE_TYPE_2D; image.tiling = VK_IMAGE_TILING_LINEAR;
   image.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
   image.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                 VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
   VkImageFormatProperties2 props{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
   if (adapter->vk.vk.GetPhysicalDeviceImageFormatProperties2(adapter->vk.pdev, &image, &props) != VK_SUCCESS ||
       !(props.imageFormatProperties.sampleCounts & VK_SAMPLE_COUNT_1_BIT))
      return 0;
   const DWORD surface = FORMATOP_OFFSCREENPLAIN | FORMATOP_OFFSCREEN_RENDERTARGET;
   if (formats) {
      formats[0] = {D3DDDIFMT_X8R8G8B8, surface | FORMATOP_DISPLAYMODE | FORMATOP_3DACCELERATION, 0, 0, 0};
      formats[1] = {D3DDDIFMT_A8R8G8B8, surface, 0, 0, 0};
   }
   return 2;
}

HRESULT APIENTRY get_caps(HANDLE handle, const D3DDDIARG_GETCAPS *query)
{
   if (!handle || !query) return E_INVALIDARG;
   auto adapter = static_cast<Adapter *>(handle);
   HRESULT result = E_NOTIMPL;
   switch (query->Type) {
   case D3DDDICAPS_GETFORMATCOUNT:
      result = write(*query, display_formats(adapter, nullptr));
      break;
   case D3DDDICAPS_GETFORMATDATA: {
      FORMATOP formats[2]{};
      const UINT count = display_formats(adapter, formats);
      const size_t size = count * sizeof(FORMATOP);
      if (!count)
         result = query->DataSize ? E_INVALIDARG : S_OK;
      else if (query->pData && query->DataSize >= size) {
         std::memcpy(query->pData, formats, size);
         result = S_OK;
      } else result = E_INVALIDARG;
      break;
   }
   case D3DDDICAPS_GETD3DQUERYCOUNT:
      result = write(*query, UINT(0));
      break;
   case D3DDDICAPS_GETD3DQUERYDATA:
      result = query->DataSize == 0 ? S_OK : E_INVALIDARG;
      break;
   case D3DDDICAPS_DDRAW:
      result = write(*query, DDRAW_CAPS{});
      break;
   case D3DDDICAPS_DDRAW_MODE_SPECIFIC:
      result = write(*query, DDRAW_MODE_SPECIFIC_CAPS{});
      break;
   case D3DDDICAPS_GET_ARCHITECTURE_INFO:
      result = write(*query, D3DDDICAPS_ARCHITECTURE_INFO{});
      break;
   case D3DDDICAPS_GETD3D9CAPS: {
      D3DCAPS9 caps{};
      caps.DeviceType = D3DDEVTYPE_HAL;
      caps.NumberOfAdaptersInGroup = 1;
      caps.NumSimultaneousRTs = 1;
      caps.DevCaps = D3DDEVCAPS_DRAWPRIMTLVERTEX | D3DDEVCAPS_TLVERTEXSYSTEMMEMORY;
      caps.PrimitiveMiscCaps = D3DPMISCCAPS_CULLNONE | D3DPMISCCAPS_CULLCW | D3DPMISCCAPS_CULLCCW;
      caps.ShadeCaps = D3DPSHADECAPS_COLORGOURAUDRGB;
      caps.MaxPrimitiveCount = legacy9::max_primitives;
      caps.MaxStreams = 1;
      caps.MaxStreamStride = legacy9::max_stream_stride;
      result = write(*query, caps);
      break;
   }
   case D3DDDICAPS_GETMULTISAMPLEQUALITYLEVELS:
      if (query->pData && query->DataSize == sizeof(DDIMULTISAMPLEQUALITYLEVELSDATA)) {
         static_cast<DDIMULTISAMPLEQUALITYLEVELSDATA *>(query->pData)->QualityLevels = 0;
         result = S_OK;
      } else result = E_INVALIDARG;
      break;
   default:
      break;
   }
   report(Capabilities, query->Type, result, query->DataSize);
   return result;
}

HRESULT APIENTRY create_device(HANDLE adapter, D3DDDIARG_CREATEDEVICE *args)
{
   if (!adapter || !args) return E_INVALIDARG;
   const HRESULT hr = legacy9_create_device(static_cast<Adapter *>(adapter), args);
   report(DeviceCreation, args->Interface, hr, args->Version);
   return hr;
}

HRESULT APIENTRY close_adapter(HANDLE handle)
{
   if (!handle) return E_INVALIDARG;
   auto adapter = static_cast<Adapter *>(handle);
   report(Close, 0, S_OK, adapter->luid);
   return close_native_adapter(adapter);
}
}

extern "C" HRESULT APIENTRY OpenAdapter(D3DDDIARG_OPENADAPTER *args)
{
   if (!args || !args->hAdapter || !args->pAdapterCallbacks || !args->pAdapterFuncs)
      return E_INVALIDARG;
   report(Open, args->Interface, E_PENDING, args->Version);
   Adapter *adapter = nullptr;
   HRESULT hr = open_native_adapter(args->hAdapter, args->pAdapterCallbacks, &adapter);
   if (FAILED(hr)) {
      report(Open, args->Interface, hr, args->Version);
      return hr;
   }
   args->pAdapterFuncs->pfnGetCaps = get_caps;
   args->pAdapterFuncs->pfnCreateDevice = create_device;
   args->pAdapterFuncs->pfnCloseAdapter = close_adapter;
   args->hAdapter = adapter;
   args->DriverVersion = args->Version < D3D_UMD_INTERFACE_VERSION_WDDM2_0 ?
                         args->Version : D3D_UMD_INTERFACE_VERSION_WDDM2_0;
   report(Open, args->Interface, S_OK, adapter->luid);
   return S_OK;
}
