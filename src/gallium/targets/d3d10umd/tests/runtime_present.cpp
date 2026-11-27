/* SPDX-License-Identifier: MIT */
/* Actual Present/Rotate/view DDIs with controlled runtime and Gallium owners.
 * This checks ownership, rebinding and ordering, not native GPU execution. */
#include "RuntimeMemory.h"
#include "State.h"
#include "DxgiFns.h"
#include "OutputMerger.h"
#include "Shader.h"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %u: %s\n", __LINE__, #x); exit(1); } } while (0)
static HANDLE rt_device = reinterpret_cast<HANDLE>(0x10);
static HANDLE rt_context = reinterpret_cast<HANDLE>(0x70);
static HANDLE rt_resource(unsigned i) { return reinterpret_cast<HANDLE>(0x20 + i); }
static unsigned creating, destroying, live_allocations, live_queues, live_views, destroyed_textures;
static unsigned presents, waits, flushes, resolved, sampler_attempt, fail_sampler;
static unsigned expected[3] = {0, 1, 2};
static unsigned expected_src, expected_dst;
static bool fail_wait;
static HRESULT present_result;
static pipe_resource textures[3];
static pipe_sampler_view *bound_views[PIPE_MAX_SHADER_SAMPLER_VIEWS];
static pipe_framebuffer_state bound_fb;
static pipe_resource *resolved_texture;
static pipe_screen screen = {};
static pipe_context pipe;
static Device device;
static Resource resources[3];
static unsigned fence_token;

extern "C" HRESULT d3d10_create_screen(HANDLE, const D3DDDI_ADAPTERCALLBACKS *, pipe_screen **)
{ return E_NOTIMPL; }

static HRESULT APIENTRY allocate(HANDLE d, D3DDDICB_ALLOCATE *a)
{
   CHECK(d == rt_device && a->hResource == rt_resource(creating) && a->NumAllocations == 1);
   CHECK(a->pAllocationInfo2->Flags.Primary && a->pAllocationInfo2->PrivateDriverDataSize == 48);
   a->pAllocationInfo2->hAllocation = 0x50 + creating;
   ++live_allocations; return S_OK;
}
static HRESULT APIENTRY deallocate(HANDLE d, const D3DDDICB_DEALLOCATE *a)
{
   CHECK(d == rt_device && a->hResource == rt_resource(destroying));
   CHECK(!a->NumAllocations && !a->HandleList && live_allocations);
   --live_allocations; return S_OK;
}
static HRESULT APIENTRY create_queue(HANDLE d, D3DDDICB_CREATEPAGINGQUEUE *q)
{
   CHECK(d == rt_device); q->hPagingQueue = 0x30 + creating;
   q->hSyncObject = 0x40 + creating; ++live_queues; return S_OK;
}
static HRESULT APIENTRY destroy_queue(HANDLE d, const D3DDDI_DESTROYPAGINGQUEUE *q)
{
   CHECK(d == rt_device && q->hPagingQueue == 0x30 + expected[destroying] && live_queues);
   --live_queues; return S_OK;
}
static HRESULT APIENTRY resident(HANDLE d, D3DDDI_MAKERESIDENT *r)
{
   CHECK(d == rt_device && r->NumAllocations == 1 && r->AllocationList[0] == 0x50 + creating);
   return S_OK;
}
static HRESULT APIENTRY present(HANDLE d, DXGIDDICB_PRESENT *p)
{
   CHECK(d == rt_device && p->hContext == rt_context);
   CHECK(p->pDXGIContext == reinterpret_cast<void *>(UINT64_C(0x123456789abcdef0)));
   CHECK(p->hSrcAllocation == 0x50 + expected_src && p->hDstAllocation == expected_dst);
   CHECK(!p->BroadcastContextCount && !p->BroadcastSrcAllocation && !p->BroadcastDstAllocation);
   CHECK(!p->PrivateDriverDataSize && !p->pPrivateDriverData && !p->SyncIntervalOverrideValid);
   CHECK(resolved && resolved_texture == &textures[expected_src] && waits == flushes && !fail_wait);
   ++presents; return present_result;
}
static pipe_sampler_view *make_view(pipe_context *, pipe_resource *texture, const pipe_sampler_view *desc)
{
   if (++sampler_attempt == fail_sampler) return NULL;
   auto view = static_cast<pipe_sampler_view *>(calloc(1, sizeof(pipe_sampler_view)));
   CHECK(view); *view = *desc; view->texture = NULL;
   pipe_resource_reference(&view->texture, texture);
   ++live_views; return view;
}
static void release_view(pipe_context *, pipe_sampler_view *view)
{
   CHECK(view && live_views);
   for (auto bound : bound_views) CHECK(bound != view);
   pipe_resource_reference(&view->texture, NULL); free(view); --live_views;
}
static void check_identities(const RenderTargetView *rtv, const ShaderResourceView *srv)
{
   for (unsigned i = 0; i < 3; ++i) {
      CHECK(resources[i].runtime_resource == rt_resource(i));
      CHECK(resources[i].resource == &textures[expected[i]]);
      CHECK(RuntimeResourceMemory(resources[i].runtime_memory)->allocation == 0x50 + expected[i]);
      CHECK(rtv[i].surface.texture == resources[i].resource);
      CHECK(srv[i].handle->texture == resources[i].resource);
      CHECK(srv[i].handle->format == PIPE_FORMAT_B8G8R8A8_UNORM);
   }
   CHECK(bound_fb.cbufs[0].texture == resources[0].resource);
   CHECK(bound_fb.cbufs[1].texture == resources[1].resource);
   CHECK(bound_views[0] == srv[0].handle && bound_views[1] == srv[1].handle);
}

int main()
{
   device.hDevice = rt_device; device.hContext = rt_context; device.pipe = &pipe;
   pipe.screen = &screen;
   screen.get_device_luid = [](pipe_screen *, char *luid) { uint64_t id = 0x1234; memcpy(luid, &id, 8); };
   screen.resource_destroy = [](pipe_screen *, pipe_resource *r) {
      CHECK(r >= textures && r < textures + 3); ++destroyed_textures;
   };
   screen.fence_finish = [](pipe_screen *, pipe_context *, pipe_fence_handle *f, uint64_t timeout) {
      CHECK(f == reinterpret_cast<pipe_fence_handle *>(&fence_token) && timeout == UINT64_MAX);
      ++waits; return !fail_wait;
   };
   screen.fence_reference = [](pipe_screen *, pipe_fence_handle **dst, pipe_fence_handle *src) { *dst = src; };
   pipe.flush = [](pipe_context *, pipe_fence_handle **f, unsigned) {
      ++flushes; *f = reinterpret_cast<pipe_fence_handle *>(&fence_token);
   };
   pipe.flush_resource = [](pipe_context *, pipe_resource *r) { ++resolved; resolved_texture = r; };
   pipe.create_sampler_view = make_view; pipe.sampler_view_release = release_view;
   pipe.set_sampler_views = [](pipe_context *, mesa_shader_stage stage, unsigned start,
                               unsigned count, unsigned, pipe_sampler_view **views) {
      CHECK(stage == MESA_SHADER_FRAGMENT && !start && count == PIPE_MAX_SHADER_SAMPLER_VIEWS);
      memcpy(bound_views, views, sizeof(bound_views));
   };
   pipe.set_framebuffer_state = [](pipe_context *, const pipe_framebuffer_state *fb) { bound_fb = *fb; };
   DXGI_DDI_BASE_CALLBACKS dxgi = {}; dxgi.pfnPresentCb = present; device.pDXGIBaseCallbacks = &dxgi;
   auto &cb = device.KTCallbacks;
   cb.pfnAllocateCb = allocate; cb.pfnDeallocateCb = deallocate;
   cb.pfnCreatePagingQueueCb = create_queue; cb.pfnDestroyPagingQueueCb = destroy_queue;
   cb.pfnMakeResidentCb = resident;
   cb.pfnSetDisplayModeCb = [](HANDLE, D3DDDICB_SETDISPLAYMODE *) -> HRESULT { return S_OK; };
   cb.pfnWaitForSynchronizationObjectFromCpuCb = [](HANDLE, const D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMCPU *) -> HRESULT { CHECK(false); return E_FAIL; };
   cb.pfnMapGpuVirtualAddressCb = [](HANDLE, D3DDDI_MAPGPUVIRTUALADDRESS *) -> HRESULT { CHECK(false); return E_FAIL; };
   cb.pfnFreeGpuVirtualAddressCb = [](HANDLE, const D3DDDICB_FREEGPUVIRTUALADDRESS *) -> HRESULT { CHECK(false); return E_FAIL; };
   cb.pfnLock2Cb = [](HANDLE, D3DDDICB_LOCK2 *) -> HRESULT { CHECK(false); return E_FAIL; };
   cb.pfnUnlock2Cb = [](HANDLE, const D3DDDICB_UNLOCK2 *) -> HRESULT { CHECK(false); return E_FAIL; };
   D3D10DDI_HDEVICE hdevice = {&device};
   D3D10DDI_MIPINFO mip = {}; mip.TexelWidth = 720; mip.TexelHeight = 1280; mip.TexelDepth = 1;
   DXGI_DDI_PRIMARY_DESC primary = {}; primary.ModeDesc.Width = 720; primary.ModeDesc.Height = 1280;
   D3D10DDIARG_CREATERESOURCE desc = {};
   desc.ResourceDimension = D3D10DDIRESOURCE_TEXTURE2D; desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
   desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1; desc.pMipInfoList = &mip; desc.pPrimaryDesc = &primary;
   RenderTargetView rtv[3] = {}; ShaderResourceView srv[3] = {};
   D3D10DDI_HRENDERTARGETVIEW rtv_handles[3]; D3D10DDI_HSHADERRESOURCEVIEW srv_handles[3];
   DXGI_DDI_HRESOURCE handles[3];
   for (creating = 0; creating < 3; ++creating) {
      unsigned i = creating; auto &r = resources[i]; auto &t = textures[i];
      pipe_reference_init(&t.reference, 1); t.screen = &screen;
      t.target = PIPE_TEXTURE_2D; t.format = PIPE_FORMAT_B8G8R8A8_UNORM;
      t.width0 = 720; t.height0 = 1280; t.depth0 = t.array_size = 1;
      r.device = &device; r.runtime_resource = rt_resource(i); r.resource = &t;
      r.Format = desc.Format; r.MipLevels = r.NumSubResources = 1;
      CHECK(RuntimePrimaryCreate(&device, &desc, {rt_resource(i)}, &r.runtime_memory) == S_OK);
      handles[i] = reinterpret_cast<DXGI_DDI_HRESOURCE>(&r);
      rtv_handles[i] = {&rtv[i]}; srv_handles[i] = {&srv[i]};
      D3D10DDIARG_CREATERENDERTARGETVIEW v = {};
      v.hDrvResource = {&r}; v.Format = desc.Format; v.ResourceDimension = desc.ResourceDimension; v.Tex2D.ArraySize = 1;
      CreateRenderTargetView(hdevice, &v, rtv_handles[i], {});
      D3D10DDIARG_CREATESHADERRESOURCEVIEW s = {};
      s.hDrvResource = {&r}; s.Format = desc.Format; s.ResourceDimension = desc.ResourceDimension;
      s.Tex2D.ArraySize = s.Tex2D.MipLevels = 1;
      CreateShaderResourceView(hdevice, &s, srv_handles[i], {});
   }
   CHECK(live_allocations == 3 && live_queues == 3 && live_views == 3);
   SetRenderTargets(hdevice, rtv_handles, 2, 0, {});
   PsSetShaderResources(hdevice, 0, 2, srv_handles);
   check_identities(rtv, srv);
   DXGI_DDI_ARG_PRESENT p = {};
   p.hDevice = reinterpret_cast<DXGI_DDI_HDEVICE>(&device); p.hSurfaceToPresent = handles[0];
   p.Flags.Flip = true; p.FlipInterval = DXGI_DDI_FLIP_INTERVAL_ONE;
   p.pDXGIContext = reinterpret_cast<void *>(UINT64_C(0x123456789abcdef0));
   CHECK(_Present(&p) == S_OK && presents == 1);
   present_result = E_ACCESSDENIED; p.hDstResource = handles[1]; expected_dst = 0x51;
   CHECK(_Present(&p) == E_ACCESSDENIED && presents == 2);
   present_result = DXGI_STATUS_OCCLUDED;
   CHECK(_Present(&p) == DXGI_STATUS_OCCLUDED && presents == 3);
   p.hDstResource = 0; expected_dst = 0; present_result = S_OK;
   fail_wait = true; CHECK(_Present(&p) == D3DDDIERR_DEVICEREMOVED && presents == 3); fail_wait = false;
   unsigned before = flushes;
   p.SrcSubResourceIndex = 1; CHECK(_Present(&p) == DXGI_ERROR_UNSUPPORTED); p.SrcSubResourceIndex = 0;
   p.Flags.Value = 3; CHECK(_Present(&p) == DXGI_ERROR_UNSUPPORTED); p.Flags.Value = 2;
   device.hContext = NULL; CHECK(_Present(&p) == DXGI_ERROR_UNSUPPORTED); device.hContext = rt_context;
   dxgi.pfnPresentCb = NULL; CHECK(_Present(&p) == DXGI_ERROR_UNSUPPORTED); dxgi.pfnPresentCb = present;
   Device foreign = {}; resources[0].device = &foreign;
   CHECK(_Present(&p) == E_INVALIDARG); resources[0].device = &device;
   CHECK(_Present(NULL) == E_INVALIDARG && flushes == before && presents == 3);

   DXGI_DDI_ARG_ROTATE_RESOURCE_IDENTITIES rotation = {p.hDevice, handles, 3};
   handles[1] = handles[0]; CHECK(_RotateResourceIdentities(&rotation) == E_INVALIDARG); handles[1] = reinterpret_cast<DXGI_DDI_HRESOURCE>(&resources[1]);
   ++textures[1].width0; CHECK(_RotateResourceIdentities(&rotation) == DXGI_ERROR_UNSUPPORTED); --textures[1].width0;
   CHECK(flushes == before);
   fail_wait = true; CHECK(_RotateResourceIdentities(&rotation) == D3DDDIERR_DEVICEREMOVED); fail_wait = false;
   fail_sampler = sampler_attempt + 2;
   CHECK(_RotateResourceIdentities(&rotation) == E_OUTOFMEMORY && live_views == 3); fail_sampler = 0;
   check_identities(rtv, srv);
   for (unsigned round = 1; round <= 4; ++round) {
      CHECK(_RotateResourceIdentities(&rotation) == S_OK);
      for (unsigned i = 0; i < 3; ++i) expected[i] = (i + round) % 3;
      check_identities(rtv, srv); CHECK(live_views == 3);
      expected_src = expected[0]; CHECK(_Present(&p) == S_OK);
   }
   /* No destination allocation means its subresource index is unused. */
   p.DstSubResourceIndex = 7;
   CHECK(_Present(&p) == S_OK);
   p.hDstResource = handles[1];
   before = flushes;
   CHECK(_Present(&p) == DXGI_ERROR_UNSUPPORTED && flushes == before);
   D3D10DDI_HSHADERRESOURCEVIEW empty[2] = {};
   PsSetShaderResources(hdevice, 0, 2, empty); SetRenderTargets(hdevice, NULL, 0, 2, {});
   for (destroying = 0; destroying < 3; ++destroying) {
      DestroyShaderResourceView(hdevice, srv_handles[destroying]);
      DestroyRenderTargetView(hdevice, rtv_handles[destroying]);
      CHECK(!resources[destroying].views);
      pipe_resource_reference(&resources[destroying].resource, NULL);
      CHECK(RuntimeResourceDestroy(resources[destroying].runtime_memory) == S_OK);
   }
   CHECK(!live_views && !live_allocations && !live_queues && destroyed_textures == 3);
   puts("PASS: runtime Present preserves owners/context/opaque token/status after GPU completion; swapchain identities, RTV/SRV bindings, failed waits/allocations and cleanup pass (controlled substitutes)");
   return 0;
}
