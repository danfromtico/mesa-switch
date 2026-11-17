/* SPDX-License-Identifier: MIT
 * Public video DDIs with CPU KMT/Vulkan substitutes. Exercises retained
 * failed cleanup, frame errors, array backing and decoder-buffer bounds.
 * The generated pixels and counters are controls, never hardware evidence.
 */
#include "tgd3d.h"
#include "tgd3d_dxva_h264.h"
#include "nouveau/vulkan/nvkmd/windows/nvdec_escape_abi.h"
#include "../../../../tegra-x1-wddm/windows/nvdec_control.h"
#include <d3dkmthk.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace tgd3d;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%u: %s\n", __FILE__, __LINE__, #x); std::exit(1); } } while (0)
namespace {
constexpr UINT frame_bytes = 1382400;
const GUID h264 = {0x1b81be68,0xa0c7,0x11d3,{0xb9,0x84,0x00,0xc0,0x4f,0x2e,0x73,0xc5}};
struct Memory { std::vector<uint8_t> bytes; };
struct Buffer { VkDeviceSize size; };
struct State {
   LONG load_error = 0, close_error = 0, destroy_error = 0;
   unsigned opens = 0, devices = 0, decodes = 0, destroys = 0, closes = 0;
   unsigned buffers = 0, memories = 0, notifications = 0;
   bool short_output = false;
   HRESULT error = S_OK;
   std::vector<uint8_t> freed;
} state;
template<typename T, typename H> T *object(H h) { return (T *)(uintptr_t)h; }
template<typename H, typename T> H handle(T *p) { return (H)(uintptr_t)p; }
}

extern "C" NTSTATUS APIENTRY D3DKMTOpenAdapterFromLuid(const D3DKMT_OPENADAPTERFROMLUID *a)
{
   CHECK(a->AdapterLuid.LowPart == 0x1234 && !a->AdapterLuid.HighPart);
   ++state.opens; const_cast<D3DKMT_OPENADAPTERFROMLUID *>(a)->hAdapter = 10; return 0;
}
extern "C" NTSTATUS APIENTRY D3DKMTCreateDevice(D3DKMT_CREATEDEVICE *d)
{ CHECK(d->hAdapter == 10); ++state.devices; d->hDevice = 20; return 0; }
extern "C" NTSTATUS APIENTRY D3DKMTDestroyDevice(const D3DKMT_DESTROYDEVICE *d)
{ CHECK(d->hDevice == 20); ++state.destroys; return state.destroy_error; }
extern "C" NTSTATUS APIENTRY D3DKMTCloseAdapter(const D3DKMT_CLOSEADAPTER *a)
{ CHECK(a->hAdapter == 10); ++state.closes; return 0; }
extern "C" NTSTATUS APIENTRY D3DKMTEscape(const D3DKMT_ESCAPE *e)
{
   CHECK(e->hAdapter == 10 && e->hDevice == 20 && e->Flags.Value == 1 && !e->hContext);
   auto *h = static_cast<tg_nvdec_escape *>(e->pPrivateDriverData);
   CHECK(h->size == e->PrivateDriverDataSize);
   void *payload = h + 1;
   switch (h->command) {
   case TG_NVDEC_ESCAPE_LOAD: {
      auto req = *static_cast<tg_nvdec_load_request *>(payload);
      CHECK(req.version == 1 && req.size == 128000);
      h->session = 31; h->result = state.load_error;
      auto *out = static_cast<tg_nvdec_load_result *>(payload);
      *out = {}; out->version = 1; out->booted = !state.load_error; out->firmware_bytes = 128000;
      std::memcpy(out->sha256, req.sha256, 32); return 0;
   }
   case TG_NVDEC_ESCAPE_CONFIGURE: {
      auto req = *static_cast<tg_nvdec_config_request *>(payload);
      CHECK(h->session == 31 && req.width == 1280 && req.height == 720);
      auto *out = static_cast<tg_nvdec_config_result *>(payload);
      *out = {}; out->version = 1; out->pitch = 1280; out->aligned_height = 720; out->frame_size = frame_bytes;
      return 0;
   }
   case TG_NVDEC_ESCAPE_DECODE: {
      auto *req = static_cast<tg_nvdec_decode_request *>(payload);
      CHECK(h->session == 31 && req->pic.curr_slot == 2 && req->slice_count == 1);
      CHECK(req->pic.scaling4x4[0][0] == 16 && req->output_size == frame_bytes);
      ++state.decodes;
      auto *out = static_cast<tg_nvdec_decode_result *>(payload);
      *out = {}; out->version = 1; out->width = 1280; out->height = 720;
      out->pitch = 1280; out->aligned_height = 720;
      out->frame_size = frame_bytes; out->bytes_written = frame_bytes - state.short_output;
      out->expected_syncpoint = out->observed_syncpoint = 40;
      out->mbs_correctly_decoded = 3600;
      std::memset(out + 1, 0x3d, frame_bytes); return 0;
   }
   case TG_NVDEC_ESCAPE_CLOSE:
      h->result = state.close_error;
      if (!h->result) h->session = 0;
      return 0;
   default: CHECK(false); return -1;
   }
}

namespace {
void init(Device &dev, Adapter &adapter)
{
   state = {};
   adapter.videoEnabled = true; adapter.videoFirmware.resize(128000);
   adapter.info.version = TG_GPU_ADAPTER_ABI; adapter.info.size = sizeof(adapter.info);
   adapter.info.flags = TG_GPU_ADAPTER_LUID_VALID | TG_GPU_ADAPTER_NATIVE_SUBMIT;
   adapter.info.luid_low = 0x1234;
   adapter.vk.caps.memory.memoryTypeCount = 1;
   adapter.vk.caps.memory.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
   dev.adapter = &adapter; dev.caps = &adapter.vk.caps;
   dev.interfaceVersion = D3D11_1_DDI_INTERFACE_VERSION;
   dev.vkdev = (VkDevice)(uintptr_t)1;
   dev.um.pfnSetErrorCb = [](D3D10DDI_HRTCORELAYER, HRESULT hr) { state.error = hr; ++state.notifications; };
   dev.vk.CreateBuffer = [](VkDevice, const VkBufferCreateInfo *i, const VkAllocationCallbacks *, VkBuffer *out) {
      *out = handle<VkBuffer>(new Buffer{i->size}); ++state.buffers; return VK_SUCCESS;
   };
   dev.vk.GetBufferMemoryRequirements = [](VkDevice, VkBuffer b, VkMemoryRequirements *out) {
      *out = {object<Buffer>(b)->size, 256, 1};
   };
   dev.vk.AllocateMemory = [](VkDevice, const VkMemoryAllocateInfo *i, const VkAllocationCallbacks *, VkDeviceMemory *out) {
      auto *m = new Memory; m->bytes.resize(i->allocationSize, 0xa5);
      *out = handle<VkDeviceMemory>(m); ++state.memories; return VK_SUCCESS;
   };
   dev.vk.BindBufferMemory = [](VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize) { return VK_SUCCESS; };
   dev.vk.MapMemory = [](VkDevice, VkDeviceMemory m, VkDeviceSize, VkDeviceSize, VkMemoryMapFlags, void **out) {
      *out = object<Memory>(m)->bytes.data(); return VK_SUCCESS;
   };
   dev.vk.UnmapMemory = [](VkDevice, VkDeviceMemory) { CHECK(false); };
   dev.vk.DestroyBuffer = [](VkDevice, VkBuffer b, const VkAllocationCallbacks *) {
      delete object<Buffer>(b); --state.buffers;
   };
   dev.vk.FreeMemory = [](VkDevice, VkDeviceMemory m, const VkAllocationCallbacks *) {
      auto *mem = object<Memory>(m); state.freed = mem->bytes; delete mem; --state.memories;
   };
}

D3D11_1DDIARG_CREATEVIDEODECODER decoder_args(Device &dev, D3D11_1DDI_VIDEODEVICEFUNCS &video)
{
   D3D11_1DDIARG_CREATEVIDEODECODER a = {};
   a.Desc = {h264, 1280, 720, DXGI_FORMAT_NV12};
   UINT count = 0; video.pfnGetVideoDecoderConfigCount({&dev}, &a.Desc, &count); CHECK(count == 1);
   video.pfnGetVideoDecoderConfig({&dev}, &a.Desc, 0, &a.Config);
   CHECK(a.Config.ConfigBitstreamRaw == 2 && a.Config.ConfigMinRenderTargetBuffCount == 17);
   return a;
}

void retained_cleanup()
{
   Device dev; Adapter adapter; init(dev, adapter);
   D3D11_1DDI_VIDEODEVICEFUNCS video; video_fill_table(&video);
   auto args = decoder_args(dev, video);
   std::vector<uint8_t> storage(video.pfnCalcPrivateVideoDecoderSize({&dev}, &args), 0xcc);
   state.load_error = (LONG)0xc00000b5u; state.close_error = state.destroy_error = (LONG)0xc000009eu;
   CHECK(FAILED(video.pfnCreateVideoDecoder({&dev}, &args, {storage.data()}, {})));
   CHECK(storage[0] == 0xcc && dev.videoSessions && state.destroys == 1 && !state.closes);
   CHECK(!video_device_destroy(&dev) && dev.videoSessions && state.destroys == 2 && !state.closes);
   state.close_error = state.destroy_error = 0;
   CHECK(video_device_destroy(&dev) && !dev.videoSessions && state.closes == 1);
   CHECK(!state.notifications);
}

void nv12_copy_planes()
{
   Device dev; Adapter adapter; init(dev, adapter);
   D3D10DDI_DEVICEFUNCS functions = {}; resource_fill_table(&functions);
   std::vector<uint8_t> source(352), target(352, 0xa5);
   for (size_t i=0;i<source.size();++i) source[i] = (i*37+i/13)&255;
   Chunk a, b; a.cpu=source.data(); b.cpu=target.data();
   Resource src, dst;
   for (Resource *r : {&src,&dst}) {
      r->dev=&dev; r->kind=ResKind::Staging; r->dim=D3D10DDIRESOURCE_TEXTURE2D;
      r->format=DXGI_FORMAT_NV12; r->width=r->height=8; r->arraySize=2;
      r->layout={{16,12,144,8,8,1},{176,12,144,8,8,1}};
      r->maps.resize(2); r->size=352;
   }
   src.slice={&a,0,352}; dst.slice={&b,0,352};
   auto expected=target;
   D3D10_DDI_BOX box={2,2,0,6,6,1};
   for (UINT y=0;y<4;++y) for (UINT x=0;x<4;++x)
      expected[16+y*12+4+x]=source[176+(2+y)*12+2+x];
   for (UINT y=0;y<2;++y) for (UINT x=0;x<4;++x)
      expected[16+(8+y)*12+4+x]=source[176+(9+y)*12+2+x];
   functions.pfnResourceCopyRegion({&dev},{&dst},0,4,0,0,{&src},1,&box);
   CHECK(target==expected && !state.notifications);
   const auto before=target;
   box={0,0,0,4,4,1};
   for (UINT y=0;y<4;++y) for (UINT x=0;x<4;++x)
      expected[16+(2+y)*12+2+x]=before[16+y*12+x];
   for (UINT y=0;y<2;++y) for (UINT x=0;x<4;++x)
      expected[16+(9+y)*12+2+x]=before[16+(8+y)*12+x];
   functions.pfnResourceCopyRegion({&dev},{&dst},0,2,2,0,{&dst},0,&box);
   CHECK(target==expected && !state.notifications);
   uint8_t upload[36]; for (UINT i=0;i<36;++i) upload[i]=(uint8_t)(i+0x30);
   box={2,2,0,6,6,1};
   for (UINT y=0;y<4;++y) for (UINT x=0;x<4;++x)
      expected[16+(2+y)*12+2+x]=upload[y*6+x];
   for (UINT y=0;y<2;++y) for (UINT x=0;x<4;++x)
      expected[16+(9+y)*12+2+x]=upload[24+y*6+x];
   functions.pfnResourceUpdateSubresourceUP({&dev},{&dst},0,&box,upload,6,36);
   CHECK(target==expected && !state.notifications);
   box.left=1;box.right=5;
   functions.pfnResourceCopyRegion({&dev},{&dst},0,0,0,0,{&src},0,&box);
   CHECK(state.notifications==1 && target==expected);
   dev.lost=S_OK; a.videoWrites=1;
   functions.pfnResourceCopyRegion({&dev},{&dst},0,0,0,0,{&src},0,nullptr);
   CHECK(state.notifications==2 && target==expected);
}

void decode_frame(unsigned fault, bool release_target_early)
{
   Device dev; Adapter adapter; init(dev, adapter);
   D3D11_1DDI_VIDEODEVICEFUNCS video; video_fill_table(&video);
   auto args = decoder_args(dev, video);
   std::vector<uint8_t> storage(video.pfnCalcPrivateVideoDecoderSize({&dev}, &args));
   D3D11_1DDI_HDECODE decode = {storage.data()};
   CHECK(video.pfnCreateVideoDecoder({&dev}, &args, decode, {}) == S_OK);
   CHECK(dev.videoSessions && state.opens == 1 && state.devices == 1);
   for (UINT i = 0; i < 4; ++i) {
      D3D11_1DDI_VIDEO_DECODER_BUFFER_INFO info = {};
      video.pfnGetVideoDecoderBufferInfo({&dev}, &args.Desc, i, &info);
      CHECK(info.Size && info.Usage == D3D11_1DDI_VIDEO_USAGE_PLAYBACK_NORMAL);
   }
   D3D11_1DDI_DEVICEFUNCS functions = {}; device_fill_table11(&functions);
   D3D10DDI_MIPINFO mip = {}; mip.TexelWidth = mip.PhysicalWidth = 1280;
   mip.TexelHeight = mip.PhysicalHeight = 720; mip.TexelDepth = mip.PhysicalDepth = 1;
   D3D11DDIARG_CREATERESOURCE desc = {};
   desc.pMipInfoList = &mip; desc.ResourceDimension = D3D10DDIRESOURCE_TEXTURE2D;
   desc.Usage = D3D10_DDI_USAGE_DEFAULT; desc.BindFlags = D3D11_DDI_BIND_DECODER;
   desc.Format = DXGI_FORMAT_NV12; desc.SampleDesc.Count = 1; desc.MipLevels = 1; desc.ArraySize = 3;
   auto *res = static_cast<Resource *>(::operator new(sizeof(Resource)));
   functions.pfnCreateResource({&dev}, &desc, {res}, {(HANDLE)1});
   CHECK(!state.notifications && res->kind == ResKind::Staging && res->layout.size() == 3);
   CHECK(res->size == 3u * frame_bytes && res->layout[2].offset == 2u * frame_bytes);
   D3D11_1DDIARG_CREATEVIDEODECODEROUTPUTVIEW v = {{res}, h264, 0, 2, 1};
   std::vector<uint8_t> view_storage(video.pfnCalcPrivateVideoDecoderOutputViewSize({&dev}, &v));
   D3D11_1DDI_HVIDEODECODEROUTPUTVIEW view = {view_storage.data()};
   CHECK(video.pfnCreateVideoDecoderOutputView({&dev}, &v, view, {}) == S_OK);
   D3D11_1DDIARG_VIDEODECODERBEGINFRAME begin = {}; begin.hOutputView = view;
   CHECK(video.pfnVideoDecoderBeginFrame({&dev}, decode, &begin) == S_OK);
   CHECK(res->slice.chunk->live == 2 && res->slice.chunk->videoWrites == 1);
   CHECK(video.pfnVideoDecoderBeginFrame({&dev}, decode, &begin) == E_INVALIDARG);
   slice_unmap(&dev, res->slice); CHECK(res->slice.cpu());
   tgd3d_dxva_pic_h264 pic = {}; pic.wFrameWidthInMbsMinus1 = 79; pic.wFrameHeightInMbsMinus1 = 44;
   pic.CurrPic.bPicEntry = 2; pic.wBitFields = 0x1050;
   std::memset(pic.RefFrameList, 0xff, sizeof(pic.RefFrameList));
   tgd3d_dxva_qmatrix_h264 matrix; std::memset(&matrix, 16, sizeof(matrix));
   const uint8_t stream[] = {0,0,1,0x65,0xaa};
   tgd3d_dxva_slice_h264_short slice = {0, sizeof(stream), 0};
   Resource buffers[4]; Chunk chunks[4];
   const void *data[] = {&pic,&matrix,&slice,stream};
   UINT lengths[] = {sizeof(pic),sizeof(matrix),sizeof(slice),sizeof(stream)};
   const D3D11_1DDI_VIDEO_DECODER_BUFFER_TYPE types[] = {
      D3D11_1DDI_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS,
      D3D11_1DDI_VIDEO_DECODER_BUFFER_INVERSE_QUANTIZATION_MATRIX,
      D3D11_1DDI_VIDEO_DECODER_BUFFER_SLICE_CONTROL,D3D11_1DDI_VIDEO_DECODER_BUFFER_BITSTREAM};
   D3D11_1DDI_VIDEO_DECODER_BUFFER_DESC input[4] = {};
   for (UINT i=0;i<4;++i) {
      chunks[i].cpu = (uint8_t *)data[i]; chunks[i].size = lengths[i];
      buffers[i].dev = &dev; buffers[i].size = lengths[i]; buffers[i].slice = {&chunks[i],0,lengths[i]};
      input[i].hResource = {&buffers[i]}; input[i].BufferType = types[i]; input[i].DataSize = lengths[i];
   }
   if (fault == 1) input[2].DataSize--;
   HRESULT submitted = video.pfnVideoDecoderSubmitBuffers({&dev}, decode, 4, input);
   CHECK((fault == 1) == FAILED(submitted));
   if (fault == 2) { // a later invalid submission poisons already-complete inputs
      CHECK(FAILED(video.pfnVideoDecoderSubmitBuffers({&dev}, decode, 1, input)));
   }
   state.short_output = fault == 3;
   if (release_target_early) {
      video.pfnDestroyVideoDecoderOutputView({&dev}, view); std::memset(view_storage.data(), 0xdd, view_storage.size());
      functions.pfnDestroyResource({&dev}, {res}); std::memset(static_cast<void *>(res), 0xdd, sizeof(*res)); ::operator delete(res);
      CHECK(state.memories == 1 && state.buffers == 1);
   }
   video.pfnVideoDecoderEndFrame({&dev}, decode);
   CHECK(state.decodes == (fault == 1 || fault == 2 ? 0u : 1u));
   CHECK(state.notifications == (fault ? 1u : 0u));
   if (!release_target_early) {
      CHECK(!res->slice.chunk->videoWrites && res->slice.chunk->live == 1);
      video.pfnDestroyVideoDecoderOutputView({&dev}, view);
      functions.pfnDestroyResource({&dev}, {res}); ::operator delete(res);
   }
   CHECK(!state.buffers && !state.memories && state.freed.size() >= 3u * frame_bytes);
   for (size_t i=0;i<state.freed.size();++i)
      CHECK(state.freed[i] == (!fault && i >= 2u*frame_bytes && i < 3u*frame_bytes ? 0x3d : 0xa5));
   video.pfnDestroyVideoDecoder({&dev}, decode);
   CHECK(!dev.videoSessions && state.destroys == 1 && state.closes == 1);
}
}

void video_contracts()
{
   retained_cleanup();
   nv12_copy_planes();
   decode_frame(0, false); decode_frame(0, true);
   decode_frame(1, false); decode_frame(2, false); decode_frame(3, false);
   std::puts("PASS: video KMT cleanup failures retain ownership; bounded DXVA buffers, frame errors and NV12 array backing survive view/resource release (CPU substitutes only)");
}
