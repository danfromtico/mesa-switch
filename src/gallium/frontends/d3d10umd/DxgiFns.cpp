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
 * DxgiFns.cpp --
 *    DXGI related functions.
 */

#include <stdio.h>
#include <stdlib.h>

#include "DxgiFns.h"
#include "Format.h"
#include "State.h"

#include "Debug.h"
#include "Trace.h"
#ifdef D3D10_UMD_HARDWARE
#include "RuntimeMemory.h"
#endif

#include "util/format/u_format.h"


/*
 * ----------------------------------------------------------------------
 *
 * _Present --
 *
 *    This is turned into kernel callbacks rather than directly emitted
 *    as fifo packets.
 *
 * ----------------------------------------------------------------------
 */

HRESULT APIENTRY
_Present(DXGI_DDI_ARG_PRESENT *pPresentData)
{

   LOG_ENTRYPOINT();

#ifdef D3D10_UMD_HARDWARE
   HRESULT hr = RuntimePresent(pPresentData);
   D3D10Trace(D3D10_TRACE_PRESENT, hr, pPresentData ? pPresentData->Flags.Value : 0,
              pPresentData ? pPresentData->FlipInterval : 0);
   return hr;
#else
   struct Device *device = CastDevice(pPresentData->hDevice);
   Resource *pSrcResource = CastResource(pPresentData->hSurfaceToPresent);

   device->pipe->flush(device->pipe, NULL, 0);
   device->pipe->screen->flush_frontbuffer(device->pipe->screen, device->pipe, 
      pSrcResource->resource, 0, 0, pPresentData->pDXGIContext, 0, NULL);

   return S_OK;
#endif
}


/*
 * ----------------------------------------------------------------------
 *
 * _GetGammaCaps --
 *
 *    Return gamma capabilities.
 *
 * ----------------------------------------------------------------------
 */

HRESULT APIENTRY
_GetGammaCaps( DXGI_DDI_ARG_GET_GAMMA_CONTROL_CAPS *GetCaps )
{
   LOG_ENTRYPOINT();

   DXGI_GAMMA_CONTROL_CAPABILITIES *pCaps;

   pCaps = GetCaps->pGammaCapabilities;

   pCaps->ScaleAndOffsetSupported = false;
   pCaps->MinConvertedValue = 0.0;
   pCaps->MaxConvertedValue = 1.0;
   pCaps->NumGammaControlPoints = 17;

   for (UINT i = 0; i < pCaps->NumGammaControlPoints; i++) {
      pCaps->ControlPointPositions[i] = (float)i / (float)(pCaps->NumGammaControlPoints - 1);
   }

   return S_OK;
}


/*
 * ----------------------------------------------------------------------
 *
 * _SetDisplayMode --
 *
 *    Set the resource that is used to scan out to the display.
 *
 * ----------------------------------------------------------------------
 */

HRESULT APIENTRY
_SetDisplayMode( DXGI_DDI_ARG_SETDISPLAYMODE *SetDisplayMode )
{
   LOG_ENTRYPOINT();

#ifdef D3D10_UMD_HARDWARE
   if (!SetDisplayMode || !SetDisplayMode->hDevice || !SetDisplayMode->hResource)
      return E_INVALIDARG;
   Device *device = CastDevice(SetDisplayMode->hDevice);
   Resource *resource = CastResource(SetDisplayMode->hResource);
   HRESULT hr = RuntimeResourceFinish(device, resource);
   if (SUCCEEDED(hr))
      hr = RuntimePrimarySetMode(resource->runtime_memory, device->hDevice,
                                 SetDisplayMode->SubResourceIndex);
   D3D10Trace(D3D10_TRACE_SET_DISPLAY_MODE, hr,
      resource->runtime_memory ? RuntimeResourceMemory(resource->runtime_memory)->allocation : 0,
      SetDisplayMode->SubResourceIndex);
   return hr;
#else
   return S_OK;
#endif
}


/*
 * ----------------------------------------------------------------------
 *
 * _SetResourcePriority --
 *
 * ----------------------------------------------------------------------
 */

HRESULT APIENTRY
_SetResourcePriority( DXGI_DDI_ARG_SETRESOURCEPRIORITY *SetResourcePriority )
{
   LOG_ENTRYPOINT();

   /* ignore */

   return S_OK;
}


/*
 * ----------------------------------------------------------------------
 *
 * _QueryResourceResidency --
 *
 * ----------------------------------------------------------------------
 */

HRESULT APIENTRY
_QueryResourceResidency( DXGI_DDI_ARG_QUERYRESOURCERESIDENCY *QueryResourceResidency )
{
   LOG_ENTRYPOINT();

   for (UINT i = 0; i < QueryResourceResidency->Resources; ++i) {
      QueryResourceResidency->pStatus[i] = DXGI_DDI_RESIDENCY_FULLY_RESIDENT;
   }

   return S_OK;
}


/*
 * ----------------------------------------------------------------------
 *
 * _RotateResourceIdentities --
 *
 *    Rotate a list of resources by recreating their views with
 *    the updated rotations.
 *
 * ----------------------------------------------------------------------
 */

#ifdef D3D10_UMD_HARDWARE
static HRESULT
rotate_hardware_resources(const DXGI_DDI_ARG_ROTATE_RESOURCE_IDENTITIES *args)
{
   if (!args || !args->hDevice || (args->Resources && !args->pResources))
      return E_INVALIDARG;
   if (args->Resources <= 1) return S_OK;
   Device *device = CastDevice(args->hDevice);
   auto pipe = device->pipe;
   const UINT count = args->Resources;
   auto resources = static_cast<Resource **>(calloc(count, sizeof(Resource *)));
   if (!resources) return E_OUTOFMEMORY;
   HRESULT hr = S_OK;
   for (UINT i = 0; i < count; ++i) {
      Resource *r = resources[i] = CastResource(args->pResources[i]);
      if (!r || r->device != device || !r->resource || r->buffer || r->NumSubResources != 1 ||
          (r->transfers && r->transfers[0])) {
         hr = E_INVALIDARG; goto done;
      }
      for (UINT j = 0; j < i; ++j) {
         if (r == resources[j] || r->resource == resources[j]->resource) {
            hr = E_INVALIDARG; goto done;
         }
      }
      const pipe_resource *a = resources[0]->resource, *b = r->resource;
      if (!!r->runtime_memory != !!resources[0]->runtime_memory ||
          r->Format != resources[0]->Format || a->target != PIPE_TEXTURE_2D ||
          b->target != a->target || b->format != a->format ||
          b->width0 != a->width0 || b->height0 != a->height0 || b->depth0 != a->depth0 ||
          b->array_size != a->array_size || b->last_level != a->last_level ||
          b->nr_samples != a->nr_samples || b->nr_storage_samples != a->nr_storage_samples) {
         hr = DXGI_ERROR_UNSUPPORTED; goto done;
      }
   }
   hr = RuntimeDeviceFinish(device);
   if (FAILED(hr)) goto done;

   /* Prepare all immutable sampler views before changing any identities.
    * Failure leaves the original resources, views and bindings intact. */
   for (UINT i = 0; i < count; ++i) {
      pipe_resource *next = resources[(i + 1) % count]->resource;
      for (auto view = resources[i]->views; view; view = view->next) {
         if (view->sampler) {
            view->replacement = pipe->create_sampler_view(pipe, next, *view->sampler);
            if (!view->replacement) { hr = E_OUTOFMEMORY; goto release_views; }
         }
      }
   }
   hr = RuntimeRotateOwnership(device, resources, count);
   if (FAILED(hr)) goto release_views;
   {
      bool framebuffer_changed = false;
      bool samplers_changed[MESA_SHADER_STAGES] = {};
      auto replace_texture = [&](pipe_resource **texture) {
         for (UINT i = 0; i < count; ++i) {
            if (*texture == resources[i]->resource) {
               pipe_resource_reference(texture, resources[(i + 1) % count]->resource);
               framebuffer_changed = true;
               break;
            }
         }
      };
      for (UINT i = 0; i < PIPE_MAX_COLOR_BUFS; ++i) replace_texture(&device->fb.cbufs[i].texture);
      replace_texture(&device->fb.zsbuf.texture);
      replace_texture(&device->fb.resolve);

      for (UINT i = 0; i < count; ++i) {
         pipe_resource *next = resources[(i + 1) % count]->resource;
         for (auto view = resources[i]->views; view; view = view->next) {
            if (view->surface) pipe_resource_reference(&view->surface->texture, next);
            if (view->sampler) {
               pipe_sampler_view *old = *view->sampler;
               *view->sampler = view->replacement;
               view->replacement = old; /* Release only after rebinding. */
               for (UINT stage = 0; stage < MESA_SHADER_STAGES; ++stage) {
                  for (UINT slot = 0; slot < PIPE_MAX_SHADER_SAMPLER_VIEWS; ++slot) {
                     if (device->sampler_views[stage][slot] == old) {
                        device->sampler_views[stage][slot] = *view->sampler;
                        samplers_changed[stage] = true;
                     }
                  }
               }
            }
         }
      }
      pipe_resource *first = resources[0]->resource;
      for (UINT i = 0; i + 1 < count; ++i) resources[i]->resource = resources[i + 1]->resource;
      resources[count - 1]->resource = first;
      if (framebuffer_changed) pipe->set_framebuffer_state(pipe, &device->fb);
      for (UINT stage = 0; stage < MESA_SHADER_STAGES; ++stage)
         if (samplers_changed[stage])
            pipe->set_sampler_views(pipe, (mesa_shader_stage)stage, 0,
                                    PIPE_MAX_SHADER_SAMPLER_VIEWS, 0, device->sampler_views[stage]);
   }
release_views:
   for (UINT i = 0; i < count; ++i)
      for (auto view = resources[i]->views; view; view = view->next)
         if (view->replacement) {
            pipe->sampler_view_release(pipe, view->replacement);
            view->replacement = NULL;
         }
done:
   free(resources);
   return hr;
}
#endif

HRESULT APIENTRY
_RotateResourceIdentities( DXGI_DDI_ARG_ROTATE_RESOURCE_IDENTITIES *RotateResourceIdentities )
{
   LOG_ENTRYPOINT();

#ifdef D3D10_UMD_HARDWARE
   HRESULT hr = rotate_hardware_resources(RotateResourceIdentities);
   D3D10Trace(D3D10_TRACE_ROTATE_IDENTITIES, hr,
              RotateResourceIdentities ? RotateResourceIdentities->Resources : 0, 0);
   return hr;
#else

   if (RotateResourceIdentities->Resources <= 1) {
      return S_OK;
   }

   struct pipe_context *pipe = CastPipeDevice(RotateResourceIdentities->hDevice);
   struct pipe_screen *screen = pipe->screen;

   struct pipe_resource *resource0 = CastPipeResource(RotateResourceIdentities->pResources[0]);

   assert(resource0);
   LOG_UNSUPPORTED(resource0->last_level);

   /*
    * XXX: Copying is not very efficient, but it is much simpler than the
    * alternative of recreating all views.
    */

   struct pipe_resource *temp_resource;
   temp_resource = screen->resource_create(screen, resource0);
   assert(temp_resource);
   if (!temp_resource) {
      return E_OUTOFMEMORY;
   }

   struct pipe_box src_box;
   src_box.x = 0;
   src_box.y = 0;
   src_box.z = 0;
   src_box.width  = resource0->width0;
   src_box.height = resource0->height0;
   src_box.depth  = resource0->depth0;

   for (UINT i = 0; i < RotateResourceIdentities->Resources + 1; ++i) {
      struct pipe_resource *src_resource;
      struct pipe_resource *dst_resource;

      if (i < RotateResourceIdentities->Resources) {
         src_resource = CastPipeResource(RotateResourceIdentities->pResources[i]);
      } else {
         src_resource = temp_resource;
      }

      if (i > 0) {
         dst_resource = CastPipeResource(RotateResourceIdentities->pResources[i - 1]);
      } else {
         dst_resource = temp_resource;
      }

      assert(dst_resource);
      assert(src_resource);

      pipe->resource_copy_region(pipe,
                                 dst_resource,
                                 0, // dst_level
                                 0, 0, 0, // dst_x,y,z
                                 src_resource,
                                 0, // src_level
                                 &src_box);
   }

   pipe_resource_reference(&temp_resource, NULL);

   return S_OK;
#endif
}


/*
 * ----------------------------------------------------------------------
 *
 * _Blt --
 *
 *    Do a blt between two subresources. Apply MSAA resolve, format
 *    conversion and stretching.
 *
 * ----------------------------------------------------------------------
 */

HRESULT APIENTRY
_Blt(DXGI_DDI_ARG_BLT *Blt)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   D3D10Trace(D3D10_TRACE_BLT, S_OK, Blt->Flags.Value, Blt->Rotate);

   return S_OK;
}
