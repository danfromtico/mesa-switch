/* SPDX-License-Identifier: MIT */
/* Real DDI frontend with a controlled Gallium backend; no GPU execution. */
#include "DriverIncludes.h"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { \
   fprintf(stderr, "line %u: %s\n", __LINE__, #x); exit(1); \
} } while (0)

extern "C" HRESULT APIENTRY OpenAdapter10_2(D3D10DDIARG_OPENADAPTER *);

static pipe_screen screen = {.caps = [] {
   pipe_caps caps = {};
   caps.max_vertex_buffers = PIPE_MAX_ATTRIBS;
   caps.supported_prim_modes = caps.supported_prim_modes_with_restart = (1u << MESA_PRIM_COUNT) - 1;
   caps.primitive_restart = true;
   caps.user_vertex_buffers = false;
   return caps;
}()};
static pipe_context context = {};
static unsigned mode, shaders, contexts, screen_destroys;
static unsigned vs, fs;
static HRESULT resource_error;
static bool allocate_resource;
static unsigned resources;
static pipe_resource buffer = {};
static bool device_removed, remove_during_map;
static unsigned maps, unmaps;
static pipe_transfer transfer = {};
static unsigned char mapped_bytes[16];
static unsigned vertex_elements, vertex_binds, draws;
static pipe_resource *borrowed_vertex;
static pipe_resource *deferred_resource;
static bool defer_vertex_unbind;
static unsigned resource_releases;
static unsigned vertex_unbinds;
static unsigned vertex_element_handle;
static unsigned runtime_contexts, fence_refs, fence_waits, fence_token;
static bool fail_context, fail_wait;
static HANDLE runtime_device = reinterpret_cast<HANDLE>(0x123);
static HANDLE runtime_context = reinterpret_cast<HANDLE>(0x456);

static HRESULT APIENTRY create_runtime_context(HANDLE device, D3DDDICB_CREATECONTEXTVIRTUAL *args)
{
   CHECK(device == runtime_device && !args->NodeOrdinal && args->EngineAffinity == 1);
   CHECK(!args->Flags.Value && !args->PrivateDriverDataSize && !args->pPrivateDriverData);
   if (fail_context) return E_ACCESSDENIED;
   CHECK(!runtime_contexts++);
   args->hContext = runtime_context;
   return S_OK;
}

static HRESULT APIENTRY destroy_runtime_context(HANDLE device, const D3DDDICB_DESTROYCONTEXT *args)
{
   CHECK(device == runtime_device && args->hContext == runtime_context);
   CHECK(runtime_contexts == 1 && contexts == 1 && !fence_refs && fence_waits && !shaders);
   --runtime_contexts;
   return S_OK;
}

static void APIENTRY set_error(D3D10DDI_HRTCORELAYER, HRESULT error)
{
   resource_error = error;
}

static void *create_shader(pipe_context *, const pipe_shader_state *state, unsigned stage)
{
   CHECK(state->tokens);
   if (mode == stage)
      return NULL;
   ++shaders;
   return stage == 1 ? &vs : &fs;
}

extern "C" HRESULT
d3d10_create_screen(HANDLE, const D3DDDI_ADAPTERCALLBACKS *, pipe_screen **result)
{
   screen.destroy = [](pipe_screen *) { ++screen_destroys; };
   screen.resource_create = [](pipe_screen *s, const pipe_resource *templ) -> pipe_resource * {
      if (!allocate_resource)
         return NULL;
      CHECK(!resources);
      ++resources;
      buffer = *templ;
      buffer.screen = s;
      pipe_reference_init(&buffer.reference, 1);
      return &buffer;
   };
   screen.resource_destroy = [](pipe_screen *, pipe_resource *resource) {
      CHECK(resources && borrowed_vertex != resource);
      --resources;
   };
   screen.fence_reference = [](pipe_screen *, pipe_fence_handle **dst, pipe_fence_handle *src) {
      if (*dst) { CHECK(*dst == reinterpret_cast<pipe_fence_handle *>(&fence_token) && fence_refs); --fence_refs; }
      if (src) ++fence_refs;
      *dst = src;
   };
   screen.fence_finish = [](pipe_screen *, pipe_context *, pipe_fence_handle *fence, uint64_t timeout) {
      CHECK(fence == reinterpret_cast<pipe_fence_handle *>(&fence_token) && timeout == UINT64_MAX);
      ++fence_waits; return !fail_wait;
   };
   screen.is_format_supported = [](pipe_screen *, pipe_format, pipe_texture_target, unsigned, unsigned, unsigned) { return true; };
   screen.context_create = [](pipe_screen *screen, void *, unsigned) -> pipe_context * {
      if (!mode)
         return NULL;
      ++contexts;
      context.screen = screen;
      context.destroy = [](pipe_context *) { CHECK(contexts); --contexts; };
      context.create_vs_state = [](pipe_context *p, const pipe_shader_state *s) { return create_shader(p, s, 1); };
      context.create_fs_state = [](pipe_context *p, const pipe_shader_state *s) { return create_shader(p, s, 2); };
      context.delete_vs_state = context.delete_fs_state = [](pipe_context *, void *s) {
         CHECK(shaders && (s == &vs || s == &fs)); --shaders;
      };
      context.bind_vs_state = context.bind_fs_state = [](pipe_context *, void *) {};
      context.bind_blend_state = context.bind_rasterizer_state = context.bind_vs_state;
      context.bind_depth_stencil_alpha_state = context.bind_vs_state;
      context.bind_vertex_elements_state = [](pipe_context *, void *state) {
         if (!state && borrowed_vertex) {
            CHECK(resources && borrowed_vertex->reference.count > 0);
            borrowed_vertex = NULL;
            pipe_resource_reference(&deferred_resource, NULL);
         }
      };
      context.resource_release = [](pipe_context *, pipe_resource *resource) {
         CHECK(resource == &buffer);
         ++resource_releases;
         if (borrowed_vertex == resource) {
            CHECK(!deferred_resource);
            deferred_resource = resource;
         } else {
            pipe_resource_reference(&resource, NULL);
         }
      };
      context.flush = [](pipe_context *, pipe_fence_handle **fence, unsigned) {
         if (fence) { *fence = reinterpret_cast<pipe_fence_handle *>(&fence_token); ++fence_refs; }
      };
      context.set_stencil_ref = [](pipe_context *, pipe_stencil_ref) {};
      context.set_constant_buffer = [](pipe_context *, mesa_shader_stage, unsigned, const pipe_constant_buffer *) {};
      context.create_vertex_elements_state = [](pipe_context *, unsigned count, const pipe_vertex_element *elements) -> void * {
         CHECK(count == 1 && elements[0].vertex_buffer_index == 0);
         ++vertex_elements; return &vertex_element_handle;
      };
      context.delete_vertex_elements_state = [](pipe_context *, void *state) {
         CHECK(state == &vertex_element_handle && vertex_elements); --vertex_elements;
      };
      context.set_vertex_buffers = [](pipe_context *, unsigned count, const pipe_vertex_buffer *buffers) {
         /* Zink can retain a borrowed binding after count=0, until vertex
          * elements are unbound. The owner must use resource_release so that
          * the backend can defer destruction through that final use. */
         if (borrowed_vertex) CHECK(resources && borrowed_vertex->reference.count > 0);
         if (!count) {
            if (!defer_vertex_unbind) borrowed_vertex = NULL;
            ++vertex_unbinds;
            return;
         }
         CHECK(count <= PIPE_MAX_ATTRIBS && buffers[0].buffer.resource == &buffer);
         for (unsigned i = 0; i < count; ++i) {
            CHECK(!buffers[i].is_user_buffer);
            if (i) CHECK(!buffers[i].buffer.resource);
         }
         borrowed_vertex = &buffer;
         ++vertex_binds;
      };
      context.draw_vbo = [](pipe_context *, const pipe_draw_info *info, unsigned,
                            const pipe_draw_indirect_info *indirect,
                            const pipe_draw_start_count_bias *draw, unsigned count) {
         CHECK(vertex_binds && vertex_elements && !indirect && count == 1);
         CHECK(info->mode == MESA_PRIM_TRIANGLES && draw[0].count == 3);
         CHECK(info->instance_count == 1 || info->instance_count == 2);
         if (info->index_size) CHECK(info->index_size == 2 && info->index.resource == &buffer);
         ++draws;
      };
      context.set_framebuffer_state = [](pipe_context *, const pipe_framebuffer_state *) {};
      context.set_sample_mask = [](pipe_context *, unsigned) {};
      context.set_sampler_views = [](pipe_context *, mesa_shader_stage, unsigned, unsigned, unsigned, pipe_sampler_view **) {};
      context.get_device_reset_status = [](pipe_context *) {
         return device_removed ? PIPE_GUILTY_CONTEXT_RESET : PIPE_NO_RESET;
      };
      context.buffer_map = context.texture_map = [](pipe_context *, pipe_resource *, unsigned, unsigned, const pipe_box *, pipe_transfer **out) -> void * {
         ++maps;
         if (!remove_during_map) return NULL;
         device_removed = true;
         *out = &transfer;
         return mapped_bytes;
      };
      context.buffer_unmap = context.texture_unmap = [](pipe_context *, pipe_transfer *t) {
         CHECK(t == &transfer); ++unmaps;
      };
      return &context;
   };
   *result = &screen;
   return S_OK;
}

int main(int argc, char **argv)
{
   CHECK(argc == 1 || (argc == 2 && !strcmp(argv[1], "--deferred-bindings")));
   defer_vertex_unbind = argc == 2;
   D3D10_2DDI_ADAPTERFUNCS adapter_functions = {};
   D3D10DDIARG_OPENADAPTER open = {};
   open.pAdapterFuncs_2 = &adapter_functions;
   CHECK(OpenAdapter10_2(&open) == S_OK && open.hAdapter.pDrvPrivate);
   UINT count = 0;
   CHECK(adapter_functions.pfnGetSupportedVersions(open.hAdapter, &count, NULL) == S_OK && count);
   UINT64 versions[8] = {};
   CHECK(count <= ARRAYSIZE(versions));
   CHECK(adapter_functions.pfnGetSupportedVersions(open.hAdapter, &count, versions) == S_OK);
   bool found = false;
   for (unsigned i = 0; i < count; ++i)
      found |= versions[i] == D3D10_0_DDI_SUPPORTED;
   CHECK(found);

   D3D10DDIARG_CALCPRIVATEDEVICESIZE size = {};
   size.Interface = D3D10_0_DDI_INTERFACE_VERSION;
   const SIZE_T bytes = adapter_functions.pfnCalcPrivateDeviceSize(open.hAdapter, &size);
   CHECK(bytes);
   D3DDDI_DEVICECALLBACKS kernel_callbacks = {};
   kernel_callbacks.pfnCreateContextVirtualCb = create_runtime_context;
   kernel_callbacks.pfnDestroyContextCb = destroy_runtime_context;
   D3D10DDI_CORELAYER_DEVICECALLBACKS runtime_callbacks = {};
   runtime_callbacks.pfnSetErrorCb = set_error;
   DXGI_DDI_BASE_FUNCTIONS dxgi = {};
   D3D10DDI_DEVICEFUNCS functions = {};
   D3D10DDIARG_CREATEDEVICE create = {};
   create.Interface = size.Interface;
   create.hRTDevice.handle = runtime_device;
   create.hDrvDevice.pDrvPrivate = calloc(1, bytes);
   CHECK(create.hDrvDevice.pDrvPrivate);
   create.pDeviceFuncs = &functions;
   create.pKTCallbacks = &kernel_callbacks;
   create.pUMCallbacks = &runtime_callbacks;
   create.DXGIBaseDDI.pDXGIDDIBaseFunctions = &dxgi;
   for (mode = 0; mode < 3; ++mode) {
      CHECK(adapter_functions.pfnCreateDevice(open.hAdapter, &create) == E_OUTOFMEMORY);
      CHECK(!shaders && !contexts && !functions.pfnDestroyDevice);
   }
   fail_context = true;
   CHECK(adapter_functions.pfnCreateDevice(open.hAdapter, &create) == E_ACCESSDENIED);
   CHECK(!runtime_contexts && !contexts && !shaders && !functions.pfnDestroyDevice);
   fail_context = false;
   CHECK(adapter_functions.pfnCreateDevice(open.hAdapter, &create) == S_OK);
   CHECK(contexts == 1 && runtime_contexts == 1 && shaders == 2 && functions.pfnCreateResource && functions.pfnDraw);
   functions.pfnFlush(create.hDrvDevice);
   CHECK(fence_waits == 1 && !fence_refs && resource_error == S_OK);
   fail_wait = true;
   functions.pfnFlush(create.hDrvDevice);
   CHECK(fence_waits == 2 && !fence_refs && resource_error == D3DDDIERR_DEVICEREMOVED);
   fail_wait = false; resource_error = S_OK;
   D3D10DDI_MIPINFO mip = {};
   mip.TexelWidth = 16;
   mip.TexelHeight = mip.TexelDepth = 1;
   D3D10DDIARG_CREATERESOURCE resource = {};
   resource.ResourceDimension = D3D10DDIRESOURCE_BUFFER;
   resource.pMipInfoList = &mip;
   resource.MipLevels = resource.ArraySize = resource.SampleDesc.Count = 1;
   unsigned char initial_data[16] = {};
   D3D10_DDIARG_SUBRESOURCE_UP initial = {};
   initial.pSysMem = initial_data;
   resource.pInitialDataUP = &initial;
   D3D10DDI_HRESOURCE handle = {};
   handle.pDrvPrivate = calloc(1, functions.pfnCalcPrivateResourceSize(create.hDrvDevice, &resource));
   CHECK(handle.pDrvPrivate);
   functions.pfnCreateResource(create.hDrvDevice, &resource, handle, {});
   CHECK(resource_error == E_OUTOFMEMORY && !resources);
   allocate_resource = true;
   resource_error = S_OK;
   functions.pfnCreateResource(create.hDrvDevice, &resource, handle, {});
   CHECK(resource_error == E_OUTOFMEMORY && !resources); /* Initial buffer map fails. */
   resource.ResourceDimension = D3D10DDIRESOURCE_TEXTURE2D;
   resource.Format = DXGI_FORMAT_R8_UNORM;
   resource_error = S_OK;
   functions.pfnCreateResource(create.hDrvDevice, &resource, handle, {});
   CHECK(resource_error == E_OUTOFMEMORY && !resources); /* Initial texture map fails. */
   resource.pInitialDataUP = NULL;
   resource.Usage = D3D10_DDI_USAGE_STAGING;
   resource.MapFlags = D3D10_DDI_CPU_ACCESS_READ;
   resource_error = S_OK;
   functions.pfnCreateResource(create.hDrvDevice, &resource, handle, {});
   CHECK(resource_error == S_OK && resources == 1);
   const unsigned previous_maps = maps;
   device_removed = true;
   D3D10DDI_MAPPED_SUBRESOURCE mapped;
   memset(&mapped, 0xa5, sizeof(mapped));
   functions.pfnStagingResourceMap(create.hDrvDevice, handle, 0, D3D10_DDI_MAP_READ, 0, &mapped);
   CHECK(resource_error == D3DDDIERR_DEVICEREMOVED && !mapped.pData && !mapped.RowPitch &&
         !mapped.DepthPitch && maps == previous_maps);
   resource_error = S_OK; device_removed = false; remove_during_map = true;
   functions.pfnStagingResourceMap(create.hDrvDevice, handle, 0, D3D10_DDI_MAP_READ, 0, &mapped);
   CHECK(resource_error == D3DDDIERR_DEVICEREMOVED && !mapped.pData &&
         maps == previous_maps + 1 && unmaps == 1);
   functions.pfnDestroyResource(create.hDrvDevice, handle);
   CHECK(!resources && unmaps == 1);
   free(handle.pDrvPrivate);
   /* Hardware drivers must never receive the dummy CPU pointers in unused
    * D3D vertex slots, including when the real bound input needs no upload.
    */
   device_removed = remove_during_map = false; resource_error = S_OK;
   resource.ResourceDimension = D3D10DDIRESOURCE_BUFFER;
   resource.Format = DXGI_FORMAT_UNKNOWN; resource.Usage = D3D10_DDI_USAGE_DEFAULT;
   resource.BindFlags = D3D10_DDI_BIND_VERTEX_BUFFER | D3D10_DDI_BIND_INDEX_BUFFER;
   resource.MapFlags = 0; mip.TexelWidth = 48;
   handle.pDrvPrivate = calloc(1, functions.pfnCalcPrivateResourceSize(create.hDrvDevice, &resource));
   CHECK(handle.pDrvPrivate);
   functions.pfnCreateResource(create.hDrvDevice, &resource, handle, {});
   CHECK(resource_error == S_OK && resources == 1);
   const D3D10DDIARG_INPUT_ELEMENT_DESC element = {
      0, 0, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D10_DDI_INPUT_PER_VERTEX_DATA, 0, 0
   };
   const D3D10DDIARG_CREATEELEMENTLAYOUT elements = {&element, 1};
   D3D10DDI_HELEMENTLAYOUT layout = {};
   layout.pDrvPrivate = calloc(1, functions.pfnCalcPrivateElementLayoutSize(create.hDrvDevice, &elements));
   CHECK(layout.pDrvPrivate);
   functions.pfnCreateElementLayout(create.hDrvDevice, &elements, layout, {});
   const UINT stride = 16, offset = 0;
   functions.pfnIaSetInputLayout(create.hDrvDevice, layout);
   functions.pfnIaSetVertexBuffers(create.hDrvDevice, 0, 1, &handle, &stride, &offset);
   functions.pfnIaSetIndexBuffer(create.hDrvDevice, handle, DXGI_FORMAT_R16_UINT, 0);
   functions.pfnIaSetTopology(create.hDrvDevice, D3D10_DDI_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
   functions.pfnDraw(create.hDrvDevice, 3, 0);
   functions.pfnDrawInstanced(create.hDrvDevice, 3, 2, 0, 0);
   functions.pfnDrawIndexed(create.hDrvDevice, 3, 0, 0);
   functions.pfnDrawIndexedInstanced(create.hDrvDevice, 3, 2, 0, 0, 0);
   CHECK(draws == 4 && vertex_binds && !resource_error);
   CHECK(borrowed_vertex == &buffer);
   const unsigned unbinds = vertex_unbinds;
   const D3D10DDI_HRESOURCE none = {};
   functions.pfnIaSetVertexBuffers(create.hDrvDevice, 0, 1, &none, &stride, &offset);
   CHECK(borrowed_vertex == (defer_vertex_unbind ? &buffer : NULL));
   CHECK(vertex_unbinds == unbinds + 1);
   functions.pfnIaSetIndexBuffer(create.hDrvDevice, {}, DXGI_FORMAT_UNKNOWN, 0);
   functions.pfnIaSetInputLayout(create.hDrvDevice, {});
   functions.pfnDestroyElementLayout(create.hDrvDevice, layout); free(layout.pDrvPrivate);
   const unsigned previous_releases = resource_releases;
   functions.pfnDestroyResource(create.hDrvDevice, handle); free(handle.pDrvPrivate);
   CHECK(resource_releases == previous_releases + 1);
   CHECK(resources == (defer_vertex_unbind ? 1u : 0u));
   CHECK(deferred_resource == (defer_vertex_unbind ? &buffer : NULL));
   DXGI_DDI_ARG_PRESENT present = {};
   present.hDevice = reinterpret_cast<DXGI_DDI_HDEVICE>(create.hDrvDevice.pDrvPrivate);
   CHECK(dxgi.pfnPresent(&present) == E_INVALIDARG);
   functions.pfnDestroyDevice(create.hDrvDevice);
   CHECK(!contexts && !runtime_contexts && !shaders && !vertex_elements && !fence_refs);
   CHECK(!resources && !borrowed_vertex && !deferred_resource);
   free(create.hDrvDevice.pDrvPrivate);
   CHECK(adapter_functions.pfnCloseAdapter(open.hAdapter) == S_OK && screen_destroys == 1);
   puts("D3D UMD runtime context ownership, GPU fence completion/loss cleanup and four draw paths pass (controlled Gallium backend)");
   printf("Buffer release ownership passed with %s vertex unbinding\n",
          defer_vertex_unbind ? "deferred" : "immediate");
   return 0;
}
