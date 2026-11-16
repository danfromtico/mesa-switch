/* SPDX-License-Identifier: MIT
 *
 * Adapter and device entry points.
 */
#include "tgd3d.h"

#include <string.h>

namespace tgd3d {

static inline Adapter *A(D3D10DDI_HADAPTER h) { return static_cast<Adapter *>(h.pDrvPrivate); }

static void APIENTRY Flush(D3D10DDI_HDEVICE hDevice)
{
   Device *dev = D(hDevice);
   /* Submit, then keep the runtime's own context behind the render queue.
    * The production path waits on the CPU only for bounded queue backpressure. */
   /* A new failure is reported by flush itself. A previously removed device
    * takes its early-return path and needs a notification for this DDI. */
   const bool wasLost = FAILED(dev->lost);
   HRESULT hr = flush(dev, true);
   if (FAILED(hr) && wasLost)
      TG_ERROR(dev, hr);
}

static void APIENTRY RelocateDeviceFuncs(D3D10DDI_HDEVICE, D3D10DDI_DEVICEFUNCS *)
{
   /* The table is not retained. */
}

static void APIENTRY CheckFormatSupport(D3D10DDI_HDEVICE hDevice, DXGI_FORMAT format, UINT *out)
{
   Device *dev = D(hDevice);
   const FormatInfo *info = format_info(format);
   *out = 0;
   if (!info || ((info->flags & FMT_BC) && !dev->caps->textureCompressionBC)) {
      *out = D3D10_DDI_FORMAT_SUPPORT_NOT_SUPPORTED;
      return;
   }
   if (info->flags & FMT_NO_VIEW)
      return;       /* creation only */
   VkFormat vk = info->vk != VK_FORMAT_UNDEFINED ? info->vk : info->depth;
   if ((info->flags & FMT_R_AS_ALPHA) && !dev->hasA8)
      vk = VK_FORMAT_R8_UNORM;
   VkFormatProperties props = {};
   dev->adapter->vk.vk.GetPhysicalDeviceFormatProperties(dev->adapter->vk.pdev, vk, &props);
   const VkFormatFeatureFlags f = props.optimalTilingFeatures;
   if (f & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)
      *out |= D3D10_DDI_FORMAT_SUPPORT_SHADER_SAMPLE;
   /* A8 stored as R8 can be sampled through a swizzle but not rendered. */
   if ((f & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) &&
       !((info->flags & FMT_R_AS_ALPHA) && !dev->hasA8)) {
      *out |= D3D10_DDI_FORMAT_SUPPORT_RENDERTARGET;
      if (f & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT)
         *out |= D3D10_DDI_FORMAT_SUPPORT_BLENDABLE;
   }
}

static void APIENTRY CheckMultisampleQualityLevels(D3D10DDI_HDEVICE, DXGI_FORMAT, UINT samples,
                                                   UINT *levels)
{
   *levels = samples == 1 ? 1 : 0;      /* multisampling is not implemented */
}

static void device_teardown(Device *dev)
{
   if (dev->vkdev) {
      if (dev->fence && dev->cmdPool) {
         flush(dev, false);
         if (SUCCEEDED(dev->lost) && dev->submitted) {
            if (FAILED(wait_sequence(dev, dev->submitted, TGD3D_WAIT_DESTROY)))
               dev->vk.DeviceWaitIdle(dev->vkdev);
         } else {
            dev->vk.DeviceWaitIdle(dev->vkdev);
         }
         retire(dev);
      }
      /* The device is idle: release everything still deferred. */
      retire(dev, true);
      blit_destroy(dev);
      device_layouts_destroy(dev);
      cmd_destroy(dev);
      upload_destroy(dev);
      sync_destroy(dev);
      dev->vk.DestroyDevice(dev->vkdev, nullptr);
      dev->vkdev = VK_NULL_HANDLE;
   }
   runtime_device_destroy(dev);
}

static void APIENTRY DestroyDevice(D3D10DDI_HDEVICE hDevice)
{
   Device *dev = D(hDevice);
   trace(TGD3D_TRACE_DESTROY_DEVICE, dev->lost, dev->submitted, dev->completed());
   device_teardown(dev);
   dev->~Device();
}

void device_fill_table(D3D10DDI_DEVICEFUNCS *table)
{
   table->pfnFlush = Flush;
   table->pfnRelocateDeviceFuncs = RelocateDeviceFuncs;
   table->pfnCheckFormatSupport = CheckFormatSupport;
   table->pfnCheckMultisampleQualityLevels = CheckMultisampleQualityLevels;
   table->pfnDestroyDevice = DestroyDevice;
}

SIZE_T APIENTRY CalcPrivateDeviceSize(D3D10DDI_HADAPTER, const D3D10DDIARG_CALCPRIVATEDEVICESIZE *)
{
   return sizeof(Device);
}

HRESULT APIENTRY CreateDevice(D3D10DDI_HADAPTER hAdapter, D3D10DDIARG_CREATEDEVICE *args)
{
   if (!hAdapter.pDrvPrivate || !args || !args->hDrvDevice.pDrvPrivate ||
       !args->pKTCallbacks || !args->pUMCallbacks)
      return E_INVALIDARG;
   Adapter *adapter = A(hAdapter);
   switch (args->Interface) {
   case D3D10_0_DDI_INTERFACE_VERSION:
   case D3D10_0_x_DDI_INTERFACE_VERSION:
   case D3D10_0_7_DDI_INTERFACE_VERSION:
      break;
   default:
      trace(TGD3D_TRACE_CREATE_DEVICE, E_FAIL, args->Interface, 0);
      return E_FAIL;
   }
   Device *dev = new (args->hDrvDevice.pDrvPrivate) Device();
   dev->adapter = adapter;
   dev->hRTDevice = args->hRTDevice;
   dev->hRTCoreLayer = args->hRTCoreLayer;
   runtime_copy_callbacks(&dev->kt, args->pKTCallbacks);
   /* SetError is the only core-layer callback used by this implementation.
    * Do not copy unused fields whose signatures change in newer tables. */
   dev->um.pfnSetErrorCb = args->pUMCallbacks->pfnSetErrorCb;
   dev->dxgi = args->DXGIBaseDDI.pDXGIBaseCallbacks;
   dev->interfaceVersion = args->Interface;
   dev->caps = &adapter->vk.caps;
   dev->nvk = adapter->vk.nvk;

   unsigned stage = 1;
   const uint64_t identity = (uintptr_t)dev;
   trace(TGD3D_TRACE_DEVICE_STAGE, E_PENDING, stage, identity);
   HRESULT hr = runtime_device_create(dev);
   trace(TGD3D_TRACE_DEVICE_STAGE, hr, stage, identity);
   if (SUCCEEDED(hr)) {
      stage = 2;
      trace(TGD3D_TRACE_DEVICE_STAGE, E_PENDING, stage, identity);
      const VkResult result = adapter->vk.createDevice(&dev->vkdev, &dev->vk, &dev->queue);
      hr = result == VK_SUCCESS ? S_OK :
           result == VK_ERROR_OUT_OF_HOST_MEMORY ? E_OUTOFMEMORY : DXGI_ERROR_UNSUPPORTED;
      trace(TGD3D_TRACE_DEVICE_STAGE, hr, stage, identity);
   }
   if (SUCCEEDED(hr)) {
      stage = 3;
      trace(TGD3D_TRACE_DEVICE_STAGE, E_PENDING, stage, identity);
      hr = sync_create(dev);
      trace(TGD3D_TRACE_DEVICE_STAGE, hr, stage, identity);
   }
   if (SUCCEEDED(hr)) {
      stage = 4;
      trace(TGD3D_TRACE_DEVICE_STAGE, E_PENDING, stage, identity);
      hr = cmd_create(dev);
      trace(TGD3D_TRACE_DEVICE_STAGE, hr, stage, identity);
   }
   if (SUCCEEDED(hr)) {
      stage = 5;
      trace(TGD3D_TRACE_DEVICE_STAGE, E_PENDING, stage, identity);
      hr = device_layouts_create(dev);
      trace(TGD3D_TRACE_DEVICE_STAGE, hr, stage, identity);
   }
   if (FAILED(hr)) {
      trace(TGD3D_TRACE_CREATE_DEVICE, hr, args->Interface, stage);
      device_teardown(dev);
      dev->~Device();
      return hr;
   }
   /* This consumer requests Vulkan 1.3 and does not enable maintenance5.
    * A 1.4-capable physical device alone does not enable native A8 usage. */
   dev->hasA8 = false;

   D3D10DDI_DEVICEFUNCS *table = args->pDeviceFuncs;
   resource_fill_table(table);
   state_fill_table(table);
   shader_fill_table(table);
   draw_fill_table(table);
   query_fill_table(table);
   device_fill_table(table);
   /* The Windows 7 interface revisions pass the DXGI 1.1 table, which adds
    * the shared-surface resolve notification. */
   dxgi_fill_table(args->DXGIBaseDDI.pDXGIDDIBaseFunctions2,
                   IS_DXGI1_1_BASE_FUNCTIONS(args->Interface, args->Version));
   trace(TGD3D_TRACE_CREATE_DEVICE, S_OK, args->Interface, 0);
   return S_OK;
}

}
