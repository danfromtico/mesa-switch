/* SPDX-License-Identifier: MIT
 *
 * tgd3d: Direct3D 10 WDDM user-mode driver that renders with Vulkan (NVK on
 * Tegra GM20B). There is no Gallium, Zink or software rasterizer in this
 * driver: every DDI call maps onto Vulkan commands for one VkDevice.
 */
#ifndef TGD3D_H
#define TGD3D_H

#include <windows.h>
#include <winddk_compat.h>
#define D3D10DDI_MINOR_HEADER_VERSION 2
/* d3d10umddi.h defines D3D10.x constants as globals; d3d10_1.h avoids the
 * duplicate definitions. */
#include <d3d10_1.h>
#include <d3d10umddi.h>

#include "tgd3d_vk.h"
#include "tgd3d_trace.h"
#include "nouveau/vulkan/nvkmd/windows/adapter_abi.h"

#include <deque>
#include <new>
#include <vector>

namespace tgd3d {

struct Device;
struct Resource;
struct View;
struct Shader;
struct RuntimeResource;

/* ------------------------------------------------------------------ */
/* Adapter                                                             */

struct Adapter {
   HANDLE hRTAdapter = nullptr;
   const D3DDDI_ADAPTERCALLBACKS *callbacks = nullptr;
   tg_gpu_adapter_info info = {};
   uint64_t luid = 0;
   VulkanAdapter vk;
};

/* ------------------------------------------------------------------ */
/* Formats                                                             */

enum FormatFlags : uint16_t {
   FMT_TYPELESS = 1 << 0,
   FMT_DEPTH = 1 << 1,
   FMT_STENCIL = 1 << 2,
   FMT_BC = 1 << 3,
   FMT_SRGB = 1 << 4,
   FMT_UINT = 1 << 5,
   FMT_SINT = 1 << 6,
   FMT_ALPHA_ONE = 1 << 7,     /* X8: alpha reads one and is never written */
   FMT_R_AS_ALPHA = 1 << 8,    /* A8 stored as R8 when VK_FORMAT_A8_UNORM is absent */
   FMT_NO_VIEW = 1 << 9,       /* creation only (XR_BIAS) */
};

struct FormatInfo {
   VkFormat vk;           /* format of a view/typed resource */
   VkFormat depth;        /* format when the resource is a depth-stencil target */
   uint8_t bytes;         /* bytes per texel, or per 4x4 block for BC */
   uint16_t flags;
   uint8_t family;        /* typeless family id; views must stay inside it */
};

const FormatInfo *format_info(DXGI_FORMAT format);
/* Resource image format for a D3D resource format and bind flags. */
VkFormat resource_vk_format(const Device *dev, DXGI_FORMAT format, UINT bind);
/* View format and aspect of `view` on a resource created with `resource`. */
bool view_vk_format(const Device *dev, DXGI_FORMAT resource, UINT bind, DXGI_FORMAT view,
                    VkFormat *format, VkImageAspectFlags *aspect, VkComponentMapping *swizzle);
VkFormat vertex_vk_format(DXGI_FORMAT format);
uint32_t format_numeric_class(DXGI_FORMAT format); /* 0 float/norm, 1 uint, 2 sint */

/* ------------------------------------------------------------------ */
/* Host-visible suballocation                                           */

struct Chunk {
   VkBuffer buffer = VK_NULL_HANDLE;
   VkDeviceMemory memory = VK_NULL_HANDLE;
   uint8_t *cpu = nullptr;
   VkDeviceSize size = 0;
   VkDeviceSize used = 0;
   uint32_t live = 0;       /* slices held by a resource or by unfinished GPU work */
   bool dedicated = false;
};

struct Slice {
   Chunk *chunk = nullptr;
   VkDeviceSize offset = 0;
   VkDeviceSize size = 0;
   explicit operator bool() const { return chunk != nullptr; }
   uint8_t *cpu() const { return chunk->cpu + offset; }
   VkBuffer buffer() const { return chunk->buffer; }
};

/* ------------------------------------------------------------------ */
/* Resources                                                           */

enum class ResKind : uint8_t {
   Buffer,
   Image,
   Staging,   /* CPU-accessible texture: texels live in a host-visible slice */
};

/* Everything that DXGI identity rotation moves between two resources. */
struct ImageStorage {
   VkImage image = VK_NULL_HANDLE;
   VkDeviceMemory memory = VK_NULL_HANDLE;
   RuntimeResource *runtime = nullptr;   /* runtime-owned WDDM allocation */
   VkImageAspectFlags aspects = VK_IMAGE_ASPECT_COLOR_BIT;
   bool layoutReady = false;
   bool preinitialized = false;          /* imported contents must survive the first use */
   uint64_t lastUse = 0;                 /* submit sequence of the last GPU use */
   uint64_t writeSerial = 0;             /* barrier bookkeeping */
   struct BlitView {
      VkImageView handle = VK_NULL_HANDLE;
      VkFormat format = VK_FORMAT_UNDEFINED;
      UINT mip = 0, layer = 0;
      bool sampled = false;
   } blitViews[4];                        /* follows backing across identity rotation */
};

struct SubLayout {
   VkDeviceSize offset;
   uint32_t rowPitch, depthPitch;
   uint32_t width, height, depth;
};

struct MapState {
   bool mapped = false;
   bool write = false;
   Slice upload;     /* dynamic texture upload storage */
};

struct Resource {
   Device *dev = nullptr;
   D3D10DDI_HRTRESOURCE hRT = {};
   D3D10DDIRESOURCE_TYPE dim = D3D10DDIRESOURCE_BUFFER;
   UINT usage = 0, bind = 0, mapFlags = 0, misc = 0;
   DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
   UINT mips = 1, arraySize = 1;
   UINT width = 0, height = 1, depth = 1;
   ResKind kind = ResKind::Buffer;
   bool primary = false;
   bool opened = false;

   /* Buffer and staging storage. Renamed on discard; never written while the
    * previous contents are still referenced by recorded or queued commands. */
   Slice slice;
   VkDeviceSize size = 0;
   uint64_t sliceLastUse = 0;
   uint64_t sliceLastWrite = 0;   /* last GPU write into the slice (staging readback) */

   /* Image storage. */
   ImageStorage storage;
   VkFormat vkFormat = VK_FORMAT_UNDEFINED;
   VkImageAspectFlags aspects = 0;
   VkImageType imageType = VK_IMAGE_TYPE_2D;
   bool cube = false;
   uint32_t pitch = 0;            /* row pitch of a runtime-owned linear image */

   std::vector<SubLayout> layout; /* staging texel layout per subresource */
   std::vector<MapState> maps;
   View *views = nullptr;

   UINT subresources() const { return mips * arraySize; }
   UINT mipWidth(UINT mip) const { UINT v = width >> mip; return v ? v : 1; }
   UINT mipHeight(UINT mip) const { UINT v = height >> mip; return v ? v : 1; }
   UINT mipDepth(UINT mip) const { UINT v = depth >> mip; return v ? v : 1; }
};

enum class ViewType : uint8_t { SRV, RTV, DSV };

struct View {
   Device *dev = nullptr;
   Resource *res = nullptr;
   View *next = nullptr;
   ViewType type = ViewType::SRV;
   DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
   uint16_t formatFlags = 0;
   VkImageViewCreateInfo info = {};
   /* One Vulkan view per backing image: a flip chain settles after one
    * rotation of every buffer and then creates nothing per frame. */
   struct Entry { VkImage image; VkImageView view; bool arrayed; } cache[6] = {};
   /* Buffer SRV. Recreated when the buffer is renamed. */
   VkBufferView bufferView = VK_NULL_HANDLE;
   VkBuffer bufferViewBuffer = VK_NULL_HANDLE;
   VkDeviceSize bufferViewOffset = 0, bufferOffset = 0, bufferRange = 0;
   VkFormat bufferFormat = VK_FORMAT_UNDEFINED;
   uint32_t width = 0, height = 0, layers = 1, mip = 0;
};

/* ------------------------------------------------------------------ */
/* State objects                                                       */

struct BlendState {
   D3D10_DDI_BLEND_DESC desc;
   VkColorBlendEquationEXT equation;
   bool usesBlendFactor;
};

struct DepthStencilState {
   D3D10_DDI_DEPTH_STENCIL_DESC desc;
};

struct RasterizerState {
   D3D10_DDI_RASTERIZER_DESC desc;
};

struct Sampler {
   Device *dev;
   VkSampler sampler;
};

struct ElementLayout {
   UINT count;
   uint32_t numericClasses;   /* two bits per input register */
   uint32_t registerMask;
   D3D10DDIARG_INPUT_ELEMENT_DESC elements[32];
};

enum class QueryState : uint8_t { Idle, Active, Ended };

/* Vulkan queries cannot span render passes, D3D queries can: an active
 * occlusion or statistics query takes one pool slot per render pass and the
 * result is the sum of the slots. */
enum { QUERY_SLOTS = 128 };

struct Query {
   Device *dev;
   D3D10DDI_QUERY type;
   UINT misc;
   QueryState state;
   VkQueryPool pool;
   uint64_t seq;          /* submit sequence that must complete before data is ready */
   uint32_t used;         /* slots written since Begin */
   bool inPass;
   std::vector<VkQueryPool> fullPools;
   HRESULT error = S_OK;
};

/* ------------------------------------------------------------------ */
/* Shaders                                                             */

struct ShaderVariant {
   uint32_t key;
   VkShaderEXT shader;
};

struct Shader {
   Device *dev = nullptr;
   Stage stage = STAGE_VS;
   std::vector<uint32_t> tokens;                     /* SHDR token stream */
   std::vector<D3D10DDIARG_SIGNATURE_ENTRY> inputs, outputs;
   /* Reflection of statically used bindings. */
   uint32_t cbMask = 0;
   uint32_t samplerMask = 0;
   uint32_t srvMask[4] = {};
   uint32_t srvBufferMask[4] = {};
   uint8_t srvKind[D3D_SRV_SLOTS] = {};      /* dxbc_spv::ir::ResourceKind per slot */
   uint32_t inputRegisters = 0;     /* VS: v# registers read */
   uint32_t outputRegisters = 0;    /* PS: o# registers written */
   bool reflected = false;
   bool usesTexelBuffers = false;
   std::vector<ShaderVariant> variants;
   HRESULT error = S_OK;
};

/* ------------------------------------------------------------------ */
/* Device                                                              */

enum Dirty : uint32_t {
   DIRTY_SHADERS = 1u << 0,
   DIRTY_VERTEX_INPUT = 1u << 1,
   DIRTY_VERTEX_BUFFERS = 1u << 2,
   DIRTY_INDEX_BUFFER = 1u << 3,
   DIRTY_TOPOLOGY = 1u << 4,
   DIRTY_RASTERIZER = 1u << 5,
   DIRTY_DEPTH_STENCIL = 1u << 6,
   DIRTY_BLEND = 1u << 7,
   DIRTY_VIEWPORTS = 1u << 8,
   DIRTY_SCISSORS = 1u << 9,
   DIRTY_TEXTURES = 1u << 10,
   DIRTY_TEXEL_BUFFERS = 1u << 11,
   DIRTY_STATIC = 1u << 12,       /* state set once per command buffer */
   DIRTY_ALL = 0xffffffffu,
};

struct StageBindings {
   Shader *shader = nullptr;
   Resource *cb[D3D_CB_SLOTS] = {};
   View *srv[D3D_SRV_SLOTS] = {};
   Sampler *sampler[D3D_SAMPLER_SLOTS] = {};
   uint32_t cbDirty = 0;
   VkShaderEXT bound = VK_NULL_HANDLE;
};

struct VertexBinding {
   Resource *buffer = nullptr;
   UINT stride = 0, offset = 0;
};

struct PendingClear {
   View *view;
   VkClearValue value;
   VkImageAspectFlags aspects;
};

struct DeferredSlice { Slice slice; uint64_t seq; };

enum class DeferredType : uint8_t {
   Image, Memory, ImageView, BufferView, Sampler, Shader, QueryPool, Runtime,
};
struct Deferred {
   DeferredType type;
   uint64_t seq;
   uint64_t handle;
   VkImage image = VK_NULL_HANDLE; /* parent of a deferred ImageView */
};

struct SubmittedCommands {
   VkCommandBuffer cmd;
   uint64_t seq;
   std::vector<VkDescriptorPool> pools;
};

struct FrameStats {
   uint32_t draws = 0, renderPasses = 0, submits = 0;
   uint64_t uploadBytes = 0, copyBytes = 0, readbackBytes = 0;
   uint64_t backpressureNs = 0;
};

struct Device {
   Adapter *adapter = nullptr;
   D3D10DDI_HRTDEVICE hRTDevice = {};
   D3D10DDI_HRTCORELAYER hRTCoreLayer = {};
   D3DDDI_DEVICECALLBACKS kt = {};
   D3D10DDI_CORELAYER_DEVICECALLBACKS um = {};
   const DXGI_DDI_BASE_CALLBACKS *dxgi = nullptr;
   HANDLE hContext = nullptr;           /* runtime-registered context: sync signals and Present */
   UINT interfaceVersion = 0;
   HRESULT lost = S_OK;
   bool inDraw = false; /* Draw DDIs can report only device removal. */

   /* Vulkan */
   VkDevice vkdev = VK_NULL_HANDLE;
   VkQueue queue = VK_NULL_HANDLE;
   VkDeviceApi vk;
   const VulkanCaps *caps = nullptr;
   bool nvk = false;
   bool hasA8 = false;                  /* VK_FORMAT_A8_UNORM is usable */
   VkCommandPool cmdPool = VK_NULL_HANDLE;
   VkCommandBuffer cmd = VK_NULL_HANDLE;
   bool cmdHasWork = false;
   bool rendering = false;
   std::vector<VkCommandBuffer> freeCommands;
   std::deque<SubmittedCommands> submittedCommands;
   std::vector<VkDescriptorPool> framePools, freePools;
   VkDescriptorSetLayout setLayouts[SET_COUNT] = {};
   VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
   VkShaderEXT blitVertex[4] = {}, blitFragment = VK_NULL_HANDLE;
   VkSampler blitPointSampler = VK_NULL_HANDLE;

   /* Queue fence. `submitted` is the value the last vkQueueSubmit2 signals;
    * `gated` is the last value queued as a GPU wait on the runtime context. */
   VkSemaphore fence = VK_NULL_HANDLE;
   VkSemaphore ingress = VK_NULL_HANDLE;
   D3DKMT_HANDLE runtimeIngress = 0;
   uint64_t submitted = 0;
   uint64_t gated = 0;
   D3DKMT_HANDLE runtimeFence = 0;
   const volatile UINT64 *runtimeFenceValue = nullptr;
   D3DKMT_HANDLE pagingQueue = 0, pagingFence = 0;
   const volatile UINT64 *pagingFenceValue = nullptr;

   /* Host-visible storage */
   std::vector<Chunk *> chunks;
   Chunk *currentChunk = nullptr;
   std::deque<DeferredSlice> deferredSlices;
   std::deque<Deferred> deferred;
   std::vector<View *> views; /* includes views caching rotated swapchain images */

   /* Bound state */
   StageBindings stage[STAGE_COUNT];
   ElementLayout *inputLayout = nullptr;
   VertexBinding vb[16];
   Resource *indexBuffer = nullptr;
   DXGI_FORMAT indexFormat = DXGI_FORMAT_R16_UINT;
   UINT indexOffset = 0;
   D3D10_DDI_PRIMITIVE_TOPOLOGY topology = D3D10_DDI_PRIMITIVE_TOPOLOGY_UNDEFINED;
   BlendState *blend = nullptr;
   float blendFactor[4] = {1, 1, 1, 1};
   UINT sampleMask = 0xffffffff;
   DepthStencilState *depthStencil = nullptr;
   UINT stencilRef = 0;
   RasterizerState *rasterizer = nullptr;
   View *rtv[D3D10_DDI_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
   View *dsv = nullptr;
   UINT viewportCount = 0;
   D3D10_DDI_VIEWPORT viewports[16] = {};
   UINT scissorCount = 0;
   D3D10_DDI_RECT scissors[16] = {};
   uint32_t dirty = DIRTY_ALL;
   uint32_t renderWidth = 0, renderHeight = 0;
   uint32_t boundColorCount = 0;
   VkDescriptorSet textureSet = VK_NULL_HANDLE, texelBufferSet = VK_NULL_HANDLE;
   std::vector<PendingClear> pendingClears;
   std::vector<Query *> activeQueries;
   VkSampler defaultSampler = VK_NULL_HANDLE;
   uint64_t writeSerial = 1, barrierSerial = 0, bufferWriteSerial = 0;

   FrameStats stats;
   uint64_t firstCommandQpc = 0;

   /* Sequence the command buffer being recorded will signal. */
   uint64_t recordingSeq() const { return submitted + 1; }
   uint64_t lastSeq() const { return cmdHasWork ? recordingSeq() : submitted; }
   uint64_t completed();
   void setError(HRESULT hr, const char *file, int line);
};

#define TG_ERROR(dev, hr) (dev)->setError((hr), __FILE__, __LINE__)

static inline Device *D(D3D10DDI_HDEVICE h) { return static_cast<Device *>(h.pDrvPrivate); }
static inline Resource *R(D3D10DDI_HRESOURCE h) { return static_cast<Resource *>(h.pDrvPrivate); }

/* tgd3d_device.cpp */
void device_fill_table(D3D10DDI_DEVICEFUNCS *table);
SIZE_T APIENTRY CalcPrivateDeviceSize(D3D10DDI_HADAPTER, const D3D10DDIARG_CALCPRIVATEDEVICESIZE *);
HRESULT APIENTRY CreateDevice(D3D10DDI_HADAPTER, D3D10DDIARG_CREATEDEVICE *);

/* tgd3d_cmd.cpp: command recording, submission and the queue fence */
VkCommandBuffer cmd_begin(Device *dev);       /* command buffer for non-draw commands; ends rendering */
VkCommandBuffer cmd_draw(Device *dev);        /* command buffer inside the current render pass */
void end_rendering(Device *dev);
HRESULT flush(Device *dev, bool waitForSpace);
void command_barrier(Device *dev);
HRESULT wait_sequence(Device *dev, uint64_t seq, tgd3d_wait_reason reason);
bool sequence_done(Device *dev, uint64_t seq);
void retire(Device *dev, bool force = false);
HRESULT sync_create(Device *dev);
void sync_destroy(Device *dev);
HRESULT cmd_create(Device *dev);
void cmd_destroy(Device *dev);
void defer(Device *dev, DeferredType type, uint64_t handle, uint64_t seq,
           VkImage image = VK_NULL_HANDLE);
void image_use(Device *dev, ImageStorage &storage, bool write);
void buffer_written(Device *dev);
void barrier_for_read(Device *dev, uint64_t writeSerial);
void flush_pending_clears(Device *dev, const View *except_bound);

/* tgd3d_upload.cpp */
HRESULT slice_alloc(Device *dev, VkDeviceSize size, VkDeviceSize alignment, Slice *slice);
void slice_release(Device *dev, Slice slice, uint64_t lastUse);
void upload_destroy(Device *dev);

/* tgd3d_resource.cpp */
HRESULT resource_result(VkResult result);
void resource_fill_table(D3D10DDI_DEVICEFUNCS *table);
VkImageView view_get(View *view, bool arrayed = false);
VkBufferView view_get_buffer(View *view);
void resource_unbind(Device *dev, Resource *res);
HRESULT image_create(Device *dev, Resource *res);
void storage_destroy(Device *dev, ImageStorage &storage, bool immediate);

/* tgd3d_state.cpp */
void state_fill_table(D3D10DDI_DEVICEFUNCS *table);
bool blend_uses_dual_source(const BlendState *state);

/* tgd3d_shader.cpp */
void shader_fill_table(D3D10DDI_DEVICEFUNCS *table);
VkShaderEXT shader_variant(Shader *shader, uint32_t key);
VkResult create_shader_cached(Device *dev, const VkShaderCreateInfoEXT &info, VkShaderEXT *shader);

/* tgd3d_draw.cpp */
void draw_fill_table(D3D10DDI_DEVICEFUNCS *table);
HRESULT device_layouts_create(Device *dev);
void device_layouts_destroy(Device *dev);
VkDescriptorSet descriptor_set_alloc(Device *dev, uint32_t set);

/* tgd3d_query.cpp */
void query_fill_table(D3D10DDI_DEVICEFUNCS *table);
void query_pass_begin(Device *dev, VkCommandBuffer cmd);
void query_pass_end(Device *dev, VkCommandBuffer cmd);

/* tgd3d_dxgi.cpp */
void dxgi_fill_table(DXGI1_1_DDI_BASE_FUNCTIONS *table, bool has_resolve);
HRESULT rotated_blit(Device *dev, const DXGI_DDI_ARG_BLT *args);
void blit_destroy(Device *dev);
#ifdef TGD3D_RED_PRIMARY_CONTROL
HRESULT red_primary_clear(Device *dev, Resource *res, UINT subresource);
#endif

/* tgd3d_runtime.cpp: runtime-owned WDDM allocations */
struct RuntimeSharedDescription {
   UINT width, height, pitch;
   DXGI_FORMAT format;
   UINT bind_flags, usage;
};
HRESULT runtime_device_create(Device *dev);
void runtime_device_destroy(Device *dev);
HRESULT runtime_primary_create(Device *dev, const D3D10DDIARG_CREATERESOURCE *desc,
                               D3D10DDI_HRTRESOURCE runtime, RuntimeResource **out,
                               UINT *width, UINT *height, UINT *pitch);
HRESULT runtime_shared_create(Device *dev, const D3D10DDIARG_CREATERESOURCE *desc,
                              D3D10DDI_HRTRESOURCE runtime, RuntimeResource **out, UINT *pitch);
HRESULT runtime_shared_open(Device *dev, const D3D10DDIARG_OPENRESOURCE *args,
                            D3D10DDI_HRTRESOURCE runtime, RuntimeResource **out,
                            RuntimeSharedDescription *description);
/* Releases GPU mappings, then the runtime allocation. The caller has already
 * destroyed every Vulkan object that referenced the backing. */
HRESULT runtime_destroy(RuntimeResource *resource);
HRESULT runtime_set_display_mode(RuntimeResource *resource, Device *dev, UINT subresource);
bool runtime_is_primary(const RuntimeResource *resource);
D3DKMT_HANDLE runtime_allocation(const RuntimeResource *resource);
void runtime_rebind(RuntimeResource *resource, D3D10DDI_HRTRESOURCE runtime);
/* Binds `image` to the runtime allocation: NVK owner import, or a host
 * pointer in the test build. No pixels are copied on either path. */
HRESULT runtime_bind_image(Device *dev, RuntimeResource *resource, VkImage image,
                           VkDeviceMemory *memory);

}

#endif
