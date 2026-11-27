/**************************************************************************
 *
 * Copyright 2012-2021 VMware, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDERS, AUTHORS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 **************************************************************************/

/*
 * Device.cpp --
 *    Functions that provide the 3D device functionality.
 */


#include "Draw.h"
#include "DxgiFns.h"
#include "InputAssembly.h"
#include "OutputMerger.h"
#include "Query.h"
#include "Rasterizer.h"
#include "Resource.h"
#include "Shader.h"
#include "State.h"
#include "Format.h"

#include "Debug.h"
#include "Trace.h"
#ifdef D3D10_UMD_HARDWARE
#include "RuntimeMemory.h"
#endif

#include "util/u_sampler.h"
#include "util/u_framebuffer.h"


static void APIENTRY DestroyDevice(D3D10DDI_HDEVICE hDevice);
static void APIENTRY RelocateDeviceFuncs(D3D10DDI_HDEVICE hDevice,
                                __in struct D3D10DDI_DEVICEFUNCS *pDeviceFunctions);
static void APIENTRY RelocateDeviceFuncs1(D3D10DDI_HDEVICE hDevice,
                                __in struct D3D10_1DDI_DEVICEFUNCS *pDeviceFunctions);
static void APIENTRY Flush(D3D10DDI_HDEVICE hDevice);
static void APIENTRY CheckFormatSupport(D3D10DDI_HDEVICE hDevice, DXGI_FORMAT Format,
                               __out UINT *pFormatCaps);
static void APIENTRY CheckMultisampleQualityLevels(D3D10DDI_HDEVICE hDevice,
                                          DXGI_FORMAT Format,
                                          UINT SampleCount,
                                          __out UINT *pNumQualityLevels);
static void APIENTRY SetTextFilterSize(D3D10DDI_HDEVICE hDevice, UINT Width, UINT Height);


/*
 * ----------------------------------------------------------------------
 *
 * CalcPrivateDeviceSize --
 *
 *    The CalcPrivateDeviceSize function determines the size of a memory
 *    region that the user-mode display driver requires from the Microsoft
 *    Direct3D runtime to store frequently-accessed data.
 *
 * ----------------------------------------------------------------------
 */

SIZE_T APIENTRY
CalcPrivateDeviceSize(D3D10DDI_HADAPTER hAdapter,                          // IN
                      __in const D3D10DDIARG_CALCPRIVATEDEVICESIZE *pData) // IN
{
   return sizeof(Device);
}

/*
 * ----------------------------------------------------------------------
 *
 * CreateDevice --
 *
 *    The CreateDevice function creates a graphics context that is
 *    referenced in subsequent calls.
 *
 * ----------------------------------------------------------------------
 */

HRESULT APIENTRY
CreateDevice(D3D10DDI_HADAPTER hAdapter,                 // IN
             __in D3D10DDIARG_CREATEDEVICE *pCreateData) // IN
{
   LOG_ENTRYPOINT();
   D3D10Trace(D3D10_TRACE_CREATE_DEVICE_BEGIN, S_OK, pCreateData->Interface, pCreateData->Flags);

   if (0) {
      DebugPrintf("hAdapter = %p\n", hAdapter);
      DebugPrintf("pKTCallbacks = %p\n", pCreateData->pKTCallbacks);
      DebugPrintf("p10_1DeviceFuncs = %p\n", pCreateData->p10_1DeviceFuncs);
      DebugPrintf("hDrvDevice = %p\n", pCreateData->hDrvDevice);
      DebugPrintf("DXGIBaseDDI = %p\n", pCreateData->DXGIBaseDDI);
      DebugPrintf("hRTCoreLayer = %p\n", pCreateData->hRTCoreLayer);
      DebugPrintf("pUMCallbacks = %p\n", pCreateData->pUMCallbacks);
   }

   switch (pCreateData->Interface) {
   case D3D10_0_DDI_INTERFACE_VERSION:
   case D3D10_0_x_DDI_INTERFACE_VERSION:
   case D3D10_0_7_DDI_INTERFACE_VERSION:
#if SUPPORT_D3D10_1
   case D3D10_1_DDI_INTERFACE_VERSION:
   case D3D10_1_x_DDI_INTERFACE_VERSION:
   case D3D10_1_7_DDI_INTERFACE_VERSION:
#endif
      break;
   default:
      DebugPrintf("%s: unsupported interface version 0x%08x\n",
                  __func__, pCreateData->Interface);
      D3D10Trace(D3D10_TRACE_CREATE_DEVICE_RESULT, E_FAIL, pCreateData->Interface, 0);
      return E_FAIL;
   }

   Adapter *pAdapter = CastAdapter(hAdapter);

   Device *pDevice = CastDevice(pCreateData->hDrvDevice);
   memset(pDevice, 0, sizeof *pDevice);

   struct pipe_screen *screen = pAdapter->screen;
   struct pipe_context *pipe = screen->context_create(screen, NULL, 0);
   if (!pipe) {
      D3D10Trace(D3D10_TRACE_CREATE_DEVICE_RESULT, E_OUTOFMEMORY, pCreateData->Interface, 1);
      return E_OUTOFMEMORY;
   }
   pDevice->pipe = pipe;
   /* Null D3D vertex bindings use CPU-backed zero values. Let CSO upload
    * those (and translate unsupported formats) for hardware backends.
    */
   pDevice->cso = cso_create_context(pipe, 0);
   if (!pDevice->cso) {
      pipe->destroy(pipe);
      pDevice->pipe = NULL;
      D3D10Trace(D3D10_TRACE_CREATE_DEVICE_RESULT, E_OUTOFMEMORY, pCreateData->Interface, 2);
      return E_OUTOFMEMORY;
   }

   pDevice->empty_vs = CreateEmptyShader(pDevice, MESA_SHADER_VERTEX);
   pDevice->empty_fs = CreateEmptyShader(pDevice, MESA_SHADER_FRAGMENT);
   if (!pDevice->empty_vs || !pDevice->empty_fs) {
      if (pDevice->empty_vs)
         DeleteEmptyShader(pDevice, MESA_SHADER_VERTEX, pDevice->empty_vs);
      if (pDevice->empty_fs)
         DeleteEmptyShader(pDevice, MESA_SHADER_FRAGMENT, pDevice->empty_fs);
      cso_destroy_context(pDevice->cso);
      pipe->destroy(pipe);
      memset(pDevice, 0, sizeof *pDevice);
      D3D10Trace(D3D10_TRACE_CREATE_DEVICE_RESULT, E_OUTOFMEMORY, pCreateData->Interface, 3);
      return E_OUTOFMEMORY;
   }

   pipe->bind_vs_state(pipe, pDevice->empty_vs);
   pipe->bind_fs_state(pipe, pDevice->empty_fs);

   pDevice->max_dual_source_render_targets =
         screen->caps.max_dual_source_render_targets;

   pDevice->hRTCoreLayer = pCreateData->hRTCoreLayer;
   pDevice->hDevice = (HANDLE)pCreateData->hRTDevice.handle;
   pDevice->KTCallbacks = *pCreateData->pKTCallbacks;
   pDevice->UMCallbacks = *pCreateData->pUMCallbacks;
   pDevice->pDXGIBaseCallbacks = pCreateData->DXGIBaseDDI.pDXGIBaseCallbacks;

#ifdef D3D10_UMD_HARDWARE
   HRESULT runtime_context = RuntimeDeviceCreateContext(pDevice);
   if (FAILED(runtime_context)) {
      DestroyDevice(pCreateData->hDrvDevice);
      memset(pDevice, 0, sizeof *pDevice);
      D3D10Trace(D3D10_TRACE_CREATE_DEVICE_RESULT, runtime_context, pCreateData->Interface, 4);
      return runtime_context;
   }
#endif

   pDevice->draw_so_target = NULL;

   if (0) {
      DebugPrintf("pDevice = %p\n", pDevice);
   }

   st_debug_parse();

   /*
    * Fill in the D3D10 DDI functions
    */
   D3D10DDI_DEVICEFUNCS *pDeviceFuncs = pCreateData->pDeviceFuncs;
   pDeviceFuncs->pfnDefaultConstantBufferUpdateSubresourceUP = ResourceUpdateSubResourceUP;
   pDeviceFuncs->pfnVsSetConstantBuffers = VsSetConstantBuffers;
   pDeviceFuncs->pfnPsSetShaderResources = PsSetShaderResources;
   pDeviceFuncs->pfnPsSetShader = PsSetShader;
   pDeviceFuncs->pfnPsSetSamplers = PsSetSamplers;
   pDeviceFuncs->pfnVsSetShader = VsSetShader;
   pDeviceFuncs->pfnDrawIndexed = DrawIndexed;
   pDeviceFuncs->pfnDraw = Draw;
   pDeviceFuncs->pfnDynamicIABufferMapNoOverwrite = ResourceMap;
   pDeviceFuncs->pfnDynamicIABufferUnmap = ResourceUnmap;
   pDeviceFuncs->pfnDynamicConstantBufferMapDiscard = ResourceMap;
   pDeviceFuncs->pfnDynamicIABufferMapDiscard = ResourceMap;
   pDeviceFuncs->pfnDynamicConstantBufferUnmap = ResourceUnmap;
   pDeviceFuncs->pfnPsSetConstantBuffers = PsSetConstantBuffers;
   pDeviceFuncs->pfnIaSetInputLayout = IaSetInputLayout;
   pDeviceFuncs->pfnIaSetVertexBuffers = IaSetVertexBuffers;
   pDeviceFuncs->pfnIaSetIndexBuffer = IaSetIndexBuffer;
   pDeviceFuncs->pfnDrawIndexedInstanced = DrawIndexedInstanced;
   pDeviceFuncs->pfnDrawInstanced = DrawInstanced;
   pDeviceFuncs->pfnDynamicResourceMapDiscard = ResourceMap;
   pDeviceFuncs->pfnDynamicResourceUnmap = ResourceUnmap;
   pDeviceFuncs->pfnGsSetConstantBuffers = GsSetConstantBuffers;
   pDeviceFuncs->pfnGsSetShader = GsSetShader;
   pDeviceFuncs->pfnIaSetTopology = IaSetTopology;
   pDeviceFuncs->pfnStagingResourceMap = ResourceMap;
   pDeviceFuncs->pfnStagingResourceUnmap = ResourceUnmap;
   pDeviceFuncs->pfnVsSetShaderResources = VsSetShaderResources;
   pDeviceFuncs->pfnVsSetSamplers = VsSetSamplers;
   pDeviceFuncs->pfnGsSetShaderResources = GsSetShaderResources;
   pDeviceFuncs->pfnGsSetSamplers = GsSetSamplers;
   pDeviceFuncs->pfnSetRenderTargets = SetRenderTargets;
   pDeviceFuncs->pfnShaderResourceViewReadAfterWriteHazard = ShaderResourceViewReadAfterWriteHazard;
   pDeviceFuncs->pfnResourceReadAfterWriteHazard = ResourceReadAfterWriteHazard;
   pDeviceFuncs->pfnSetBlendState = SetBlendState;
   pDeviceFuncs->pfnSetDepthStencilState = SetDepthStencilState;
   pDeviceFuncs->pfnSetRasterizerState = SetRasterizerState;
   pDeviceFuncs->pfnQueryEnd = QueryEnd;
   pDeviceFuncs->pfnQueryBegin = QueryBegin;
   pDeviceFuncs->pfnResourceCopyRegion = ResourceCopyRegion;
   pDeviceFuncs->pfnResourceUpdateSubresourceUP = ResourceUpdateSubResourceUP;
   pDeviceFuncs->pfnSoSetTargets = SoSetTargets;
   pDeviceFuncs->pfnDrawAuto = DrawAuto;
   pDeviceFuncs->pfnSetViewports = SetViewports;
   pDeviceFuncs->pfnSetScissorRects = SetScissorRects;
   pDeviceFuncs->pfnClearRenderTargetView = ClearRenderTargetView;
   pDeviceFuncs->pfnClearDepthStencilView = ClearDepthStencilView;
   pDeviceFuncs->pfnSetPredication = SetPredication;
   pDeviceFuncs->pfnQueryGetData = QueryGetData;
   pDeviceFuncs->pfnFlush = Flush;
   pDeviceFuncs->pfnGenMips = GenMips;
   pDeviceFuncs->pfnResourceCopy = ResourceCopy;
   pDeviceFuncs->pfnResourceResolveSubresource = ResourceResolveSubResource;
   pDeviceFuncs->pfnResourceMap = ResourceMap;
   pDeviceFuncs->pfnResourceUnmap = ResourceUnmap;
   pDeviceFuncs->pfnResourceIsStagingBusy = ResourceIsStagingBusy;
   pDeviceFuncs->pfnRelocateDeviceFuncs = RelocateDeviceFuncs;
   pDeviceFuncs->pfnCalcPrivateResourceSize = CalcPrivateResourceSize;
   pDeviceFuncs->pfnCalcPrivateOpenedResourceSize = CalcPrivateOpenedResourceSize;
   pDeviceFuncs->pfnCreateResource = CreateResource;
   pDeviceFuncs->pfnOpenResource = OpenResource;
   pDeviceFuncs->pfnDestroyResource = DestroyResource;
   pDeviceFuncs->pfnCalcPrivateShaderResourceViewSize = CalcPrivateShaderResourceViewSize;
   pDeviceFuncs->pfnCreateShaderResourceView = CreateShaderResourceView;
   pDeviceFuncs->pfnDestroyShaderResourceView = DestroyShaderResourceView;
   pDeviceFuncs->pfnCalcPrivateRenderTargetViewSize = CalcPrivateRenderTargetViewSize;
   pDeviceFuncs->pfnCreateRenderTargetView = CreateRenderTargetView;
   pDeviceFuncs->pfnDestroyRenderTargetView = DestroyRenderTargetView;
   pDeviceFuncs->pfnCalcPrivateDepthStencilViewSize = CalcPrivateDepthStencilViewSize;
   pDeviceFuncs->pfnCreateDepthStencilView = CreateDepthStencilView;
   pDeviceFuncs->pfnDestroyDepthStencilView = DestroyDepthStencilView;
   pDeviceFuncs->pfnCalcPrivateElementLayoutSize = CalcPrivateElementLayoutSize;
   pDeviceFuncs->pfnCreateElementLayout = CreateElementLayout;
   pDeviceFuncs->pfnDestroyElementLayout = DestroyElementLayout;
   pDeviceFuncs->pfnCalcPrivateBlendStateSize = CalcPrivateBlendStateSize;
   pDeviceFuncs->pfnCreateBlendState = CreateBlendState;
   pDeviceFuncs->pfnDestroyBlendState = DestroyBlendState;
   pDeviceFuncs->pfnCalcPrivateDepthStencilStateSize = CalcPrivateDepthStencilStateSize;
   pDeviceFuncs->pfnCreateDepthStencilState = CreateDepthStencilState;
   pDeviceFuncs->pfnDestroyDepthStencilState = DestroyDepthStencilState;
   pDeviceFuncs->pfnCalcPrivateRasterizerStateSize = CalcPrivateRasterizerStateSize;
   pDeviceFuncs->pfnCreateRasterizerState = CreateRasterizerState;
   pDeviceFuncs->pfnDestroyRasterizerState = DestroyRasterizerState;
   pDeviceFuncs->pfnCalcPrivateShaderSize = CalcPrivateShaderSize;
   pDeviceFuncs->pfnCreateVertexShader = CreateVertexShader;
   pDeviceFuncs->pfnCreateGeometryShader = CreateGeometryShader;
   pDeviceFuncs->pfnCreatePixelShader = CreatePixelShader;
   pDeviceFuncs->pfnCalcPrivateGeometryShaderWithStreamOutput = CalcPrivateGeometryShaderWithStreamOutput;
   pDeviceFuncs->pfnCreateGeometryShaderWithStreamOutput = CreateGeometryShaderWithStreamOutput;
   pDeviceFuncs->pfnDestroyShader = DestroyShader;
   pDeviceFuncs->pfnCalcPrivateSamplerSize = CalcPrivateSamplerSize;
   pDeviceFuncs->pfnCreateSampler = CreateSampler;
   pDeviceFuncs->pfnDestroySampler = DestroySampler;
   pDeviceFuncs->pfnCalcPrivateQuerySize = CalcPrivateQuerySize;
   pDeviceFuncs->pfnCreateQuery = CreateQuery;
   pDeviceFuncs->pfnDestroyQuery = DestroyQuery;
   pDeviceFuncs->pfnCheckFormatSupport = CheckFormatSupport;
   pDeviceFuncs->pfnCheckMultisampleQualityLevels = CheckMultisampleQualityLevels;
   pDeviceFuncs->pfnCheckCounterInfo = CheckCounterInfo;
   pDeviceFuncs->pfnCheckCounter = CheckCounter;
   pDeviceFuncs->pfnDestroyDevice = DestroyDevice;
   pDeviceFuncs->pfnSetTextFilterSize = SetTextFilterSize;
   if (pCreateData->Interface == D3D10_1_DDI_INTERFACE_VERSION ||
       pCreateData->Interface == D3D10_1_x_DDI_INTERFACE_VERSION ||
       pCreateData->Interface == D3D10_1_7_DDI_INTERFACE_VERSION) {
      D3D10_1DDI_DEVICEFUNCS *p10_1DeviceFuncs = pCreateData->p10_1DeviceFuncs;
      p10_1DeviceFuncs->pfnRelocateDeviceFuncs = RelocateDeviceFuncs1;
      p10_1DeviceFuncs->pfnCalcPrivateShaderResourceViewSize = CalcPrivateShaderResourceViewSize1;
      p10_1DeviceFuncs->pfnCreateShaderResourceView = CreateShaderResourceView1;
      p10_1DeviceFuncs->pfnCalcPrivateBlendStateSize = CalcPrivateBlendStateSize1;
      p10_1DeviceFuncs->pfnCreateBlendState = CreateBlendState1;
      p10_1DeviceFuncs->pfnResourceConvert = ResourceCopy;
      p10_1DeviceFuncs->pfnResourceConvertRegion = ResourceCopyRegion;
   }

   /*
    * Fill in DXGI DDI functions
    */
   pCreateData->DXGIBaseDDI.pDXGIDDIBaseFunctions->pfnPresent =
      _Present;
   pCreateData->DXGIBaseDDI.pDXGIDDIBaseFunctions->pfnGetGammaCaps =
      _GetGammaCaps;
   pCreateData->DXGIBaseDDI.pDXGIDDIBaseFunctions->pfnSetDisplayMode =
      _SetDisplayMode;
   pCreateData->DXGIBaseDDI.pDXGIDDIBaseFunctions->pfnSetResourcePriority =
      _SetResourcePriority;
   pCreateData->DXGIBaseDDI.pDXGIDDIBaseFunctions->pfnQueryResourceResidency =
      _QueryResourceResidency;
   pCreateData->DXGIBaseDDI.pDXGIDDIBaseFunctions->pfnRotateResourceIdentities =
      _RotateResourceIdentities;
   pCreateData->DXGIBaseDDI.pDXGIDDIBaseFunctions->pfnBlt =
      _Blt;

#ifdef D3D10_UMD_HARDWARE
   D3D10Trace(D3D10_TRACE_CREATE_DEVICE_RESULT, S_OK, pCreateData->Interface, 0);
   return S_OK;
#else
   D3D10Trace(D3D10_TRACE_CREATE_DEVICE_RESULT, DXGI_STATUS_NO_REDIRECTION, pCreateData->Interface, 0);
   // The GDI software target does not use DWM's shared-resource presentation.
   return DXGI_STATUS_NO_REDIRECTION;
#endif
}


/*
 * ----------------------------------------------------------------------
 *
 * DestroyDevice --
 *
 *    The DestroyDevice function destroys a graphics context.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
DestroyDevice(D3D10DDI_HDEVICE hDevice)   // IN
{
   unsigned i;

   LOG_ENTRYPOINT();
   D3D10Trace(D3D10_TRACE_DESTROY_DEVICE_BEGIN, S_OK, 0, 0);

   Device *pDevice = CastDevice(hDevice);
   struct pipe_context *pipe = pDevice->pipe;

#ifdef D3D10_UMD_HARDWARE
   if (pDevice->hContext) {
      HRESULT hr = RuntimeDeviceFinish(pDevice);
      if (FAILED(hr)) SetError(hDevice, hr);
   } else
#endif
      pipe->flush(pipe, NULL, 0);

   for (i = 0; i < PIPE_MAX_SO_BUFFERS; ++i) {
      pipe_so_target_reference(&pDevice->so_targets[i], NULL);
   }
   if (pDevice->draw_so_target) {
      pipe_so_target_reference(&pDevice->draw_so_target, NULL);
   }

   pipe->bind_fs_state(pipe, NULL);
   pipe->bind_vs_state(pipe, NULL);
   cso_unbind_context(pDevice->cso);
   cso_destroy_context(pDevice->cso);

   DeleteEmptyShader(pDevice, MESA_SHADER_FRAGMENT, pDevice->empty_fs);
   DeleteEmptyShader(pDevice, MESA_SHADER_VERTEX, pDevice->empty_vs);

   util_unreference_framebuffer_state(&pDevice->fb);

   for (i = 0; i < PIPE_MAX_ATTRIBS; ++i) {
      if (!pDevice->vertex_buffers[i].is_user_buffer) {
         pipe_resource_reference(&pDevice->vertex_buffers[i].buffer.resource, NULL);
      }
   }

   pipe_resource_reference(&pDevice->index_buffer, NULL);

   struct pipe_sampler_view *sampler_views[PIPE_MAX_SHADER_SAMPLER_VIEWS] = {};
   pipe->set_sampler_views(pipe, MESA_SHADER_FRAGMENT, 0,
                           PIPE_MAX_SHADER_SAMPLER_VIEWS, 0, sampler_views);
   pipe->set_sampler_views(pipe, MESA_SHADER_VERTEX, 0,
                           PIPE_MAX_SHADER_SAMPLER_VIEWS, 0, sampler_views);
   pipe->set_sampler_views(pipe, MESA_SHADER_GEOMETRY, 0,
                           PIPE_MAX_SHADER_SAMPLER_VIEWS, 0, sampler_views);

#ifdef D3D10_UMD_HARDWARE
   HRESULT released = RuntimeDeviceDestroyContext(pDevice);
   if (FAILED(released)) SetError(hDevice, released);
#endif
   pipe->destroy(pipe);
   D3D10Trace(D3D10_TRACE_DESTROY_DEVICE_END, S_OK, 0, 0);
}


/*
 * ----------------------------------------------------------------------
 *
 * RelocateDeviceFuncs --
 *
 *    The RelocateDeviceFuncs function notifies the user-mode
 *    display driver about the new location of the driver function table.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
RelocateDeviceFuncs(D3D10DDI_HDEVICE hDevice,                           // IN
                    __in struct D3D10DDI_DEVICEFUNCS *pDeviceFunctions) // IN
{
   LOG_ENTRYPOINT();

   /*
    * Nothing to do as we don't store a pointer to this entity.
    */
}


/*
 * ----------------------------------------------------------------------
 *
 * RelocateDeviceFuncs1 --
 *
 *    The RelocateDeviceFuncs1 function notifies the user-mode
 *    display driver about the new location of the driver function table.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
RelocateDeviceFuncs1(D3D10DDI_HDEVICE hDevice,                           // IN
                    __in struct D3D10_1DDI_DEVICEFUNCS *pDeviceFunctions) // IN
{
   LOG_ENTRYPOINT();

   /*
    * Nothing to do as we don't store a pointer to this entity.
    */
}


/*
 * ----------------------------------------------------------------------
 *
 * Flush --
 *
 *    The Flush function submits outstanding hardware commands that
 *    are in the hardware command buffer to the display miniport driver.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
Flush(D3D10DDI_HDEVICE hDevice)  // IN
{
   LOG_ENTRYPOINT();

#ifdef D3D10_UMD_HARDWARE
   HRESULT hr = RuntimeDeviceFinish(CastDevice(hDevice));
   if (FAILED(hr)) SetError(hDevice, hr);
#else
   struct pipe_context *pipe = CastPipeContext(hDevice);

   pipe->flush(pipe, NULL, 0);
#endif
}


/*
 * ----------------------------------------------------------------------
 *
 * CheckFormatSupport --
 *
 *    The CheckFormatSupport function retrieves the capabilites that
 *    the device has with the specified format.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
CheckFormatSupport(D3D10DDI_HDEVICE hDevice, // IN
                   DXGI_FORMAT Format,       // IN
                   __out UINT *pFormatCaps)  // OUT
{
   D3D10Trace(D3D10_TRACE_FORMAT_BEGIN, S_OK, Format, 0);

   struct pipe_context *pipe = CastPipeContext(hDevice);
   struct pipe_screen *screen = pipe->screen;

   *pFormatCaps = 0;

   enum pipe_format format = FormatTranslate(Format, false);
   if (format == PIPE_FORMAT_NONE) {
      *pFormatCaps = D3D10_DDI_FORMAT_SUPPORT_NOT_SUPPORTED;
      D3D10Trace(D3D10_TRACE_FORMAT_END, S_OK, Format, *pFormatCaps);
      return;
   }

   if (Format == DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM) {
      /*
       * We only need to support creation.
       * http://msdn.microsoft.com/en-us/library/windows/hardware/ff552818.aspx
       */
      D3D10Trace(D3D10_TRACE_FORMAT_END, S_OK, Format, *pFormatCaps);
      return;
   }

   if (screen->is_format_supported(screen, format, PIPE_TEXTURE_2D, 0, 0,
                                   PIPE_BIND_RENDER_TARGET)) {
      *pFormatCaps |= D3D10_DDI_FORMAT_SUPPORT_RENDERTARGET;
      *pFormatCaps |= D3D10_DDI_FORMAT_SUPPORT_BLENDABLE;

#if SUPPORT_MSAA
      if (screen->is_format_supported(screen, format, PIPE_TEXTURE_2D, 4, 4,
                                      PIPE_BIND_RENDER_TARGET)) {
         *pFormatCaps |= D3D10_DDI_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET;
      }
#endif
   }

   if (screen->is_format_supported(screen, format, PIPE_TEXTURE_2D, 0, 0,
                                   PIPE_BIND_SAMPLER_VIEW)) {
      *pFormatCaps |= D3D10_DDI_FORMAT_SUPPORT_SHADER_SAMPLE;

#if SUPPORT_MSAA
      if (screen->is_format_supported(screen, format, PIPE_TEXTURE_2D, 4, 4,
                                      PIPE_BIND_RENDER_TARGET)) {
         *pFormatCaps |= D3D10_DDI_FORMAT_SUPPORT_MULTISAMPLE_LOAD;
      }
#endif
   }
   D3D10Trace(D3D10_TRACE_FORMAT_END, S_OK, Format, *pFormatCaps);
}


/*
 * ----------------------------------------------------------------------
 *
 * CheckMultisampleQualityLevels --
 *
 *    The CheckMultisampleQualityLevels function retrieves the number
 *    of quality levels that the device supports for the specified
 *    number of samples.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
CheckMultisampleQualityLevels(D3D10DDI_HDEVICE hDevice,        // IN
                              DXGI_FORMAT Format,              // IN
                              UINT SampleCount,                // IN
                              __out UINT *pNumQualityLevels)   // OUT
{
   //LOG_ENTRYPOINT();

   /* XXX: Disable MSAA */
   *pNumQualityLevels = 0;
   D3D10Trace(D3D10_TRACE_MULTISAMPLE, S_OK, Format,
              (uint64_t)SampleCount << 32 | *pNumQualityLevels);
}


/*
 * ----------------------------------------------------------------------
 *
 * SetTextFilterSize --
 *
 *    The SetTextFilterSize function sets the width and height
 *    of the monochrome convolution filter.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
SetTextFilterSize(D3D10DDI_HDEVICE hDevice,  // IN
                  UINT Width,                // IN
                  UINT Height)               // IN
{
   LOG_ENTRYPOINT();

   LOG_UNSUPPORTED(Width != 1 || Height != 1);
}
