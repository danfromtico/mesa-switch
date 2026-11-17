/* SPDX-License-Identifier: MIT
 * Synchronous libavcodec D3D11VA acceptance on the selected native adapter.
 * Uploaded planes come from a checked hardware-frame readback. The D3D11
 * pixel shader performs CSC on the same device as the decoder.
 */
#define WIN32_LEAN_AND_MEAN
#include "nvdec720_fixture.hpp"
#include "nvdec720_preview_vs.h"
#include "nvdec720_preview_ps.h"
#include <dxgi1_2.h>
#include <dwmapi.h>
#include <evntprov.h>
#include <evntrace.h>
#include <io.h>
#include <algorithm>
#include <cstddef>
#include <cmath>
#include <cstdlib>
#include <cwchar>
#include <memory>
#include <string>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/pixdesc.h>
}
#include "../../src/nouveau/nvdec/nvdec.h"
#include "../../src/nouveau/tgd3d/tgd3d_csc.h"
#include "../../src/nouveau/tgd3d/tgd3d_trace.h"

#ifndef EVENT_FILTER_TYPE_PID
// The Windows SDK defines this scope filter in evntprov.h.
#define EVENT_FILTER_TYPE_PID 0x80000004u
#endif

// Save the entry and return of public calls even if buffered output is lost.
class Journal {
   HANDLE file = INVALID_HANDLE_VALUE;
public:
   explicit Journal(const wchar_t *evidence_directory) {
      wchar_t path[32768];
      std::wstring journal_path;
      if (evidence_directory) {
         check(CreateDirectoryW(evidence_directory,nullptr),"create new evidence directory");
         journal_path=std::wstring(evidence_directory)+L"\\journal.txt";
      } else {
         DWORD length=GetEnvironmentVariableW(L"NVDEC720_JOURNAL",path,ARRAYSIZE(path));
         if (!length) return;
         check(length<ARRAYSIZE(path),"journal path too long");
         journal_path=path;
      }
      file=CreateFileW(journal_path.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,
                       FILE_ATTRIBUTE_NORMAL|FILE_FLAG_WRITE_THROUGH,nullptr);
      check(file!=INVALID_HANDLE_VALUE,"create preview journal");
   }
   ~Journal() { if (file!=INVALID_HANDLE_VALUE) CloseHandle(file); }
   Journal(const Journal &)=delete;
   Journal &operator=(const Journal &)=delete;
   void mark(const char *phase, const char *operation, HRESULT result=S_OK) const {
      if (file==INVALID_HANDLE_VALUE) return;
      char line[208]; DWORD written=0;
      int n=std::snprintf(line,sizeof(line),"%llu %lu %s %s %08lx\r\n",GetTickCount64(),
                          GetCurrentThreadId(),phase,operation,(unsigned long)result);
      check(n>0 && size_t(n)<sizeof(line) && WriteFile(file,line,DWORD(n),&written,nullptr) &&
            written==DWORD(n) && FlushFileBuffers(file),"persist preview journal");
   }
   template<typename Call> HRESULT invoke(const char *operation,Call call) const {
      mark("CALL",operation);
      HRESULT hr=call();mark("RETURN",operation,hr);require(hr,operation);return hr;
   }
   template<typename Call> int av_call(const char *operation,Call call) const {
      mark("CALL",operation);
      const int result=call();
      mark("RETURN",operation,static_cast<HRESULT>(result));
      return result;
   }
};

// Limit the driver provider to this already-running acceptance process.
class VideoTrace {
   struct Storage {
      EVENT_TRACE_PROPERTIES properties;
      wchar_t session[64];
      wchar_t file[1024];
   } storage = {};
   TRACEHANDLE handle = 0;
public:
   void start(const wchar_t *directory) {
      auto &p=storage.properties;
      wchar_t name[64];
      DWORD pid=GetCurrentProcessId();
      std::swprintf(name,ARRAYSIZE(name),L"Nvdec720Preview-%lu",pid);
      const std::wstring path=std::wstring(directory)+L"\\video.etl";
      DWORD length=GetFullPathNameW(path.c_str(),ARRAYSIZE(storage.file),storage.file,nullptr);
      check(length && length<ARRAYSIZE(storage.file),"resolve trace path");
      p.Wnode.BufferSize=sizeof(storage);
      p.Wnode.Flags=WNODE_FLAG_TRACED_GUID;
      p.Wnode.ClientContext=1; // QPC timestamps
      p.BufferSize=64; p.MinimumBuffers=4; p.MaximumBuffers=16; p.FlushTimer=1;
      p.LogFileMode=EVENT_TRACE_FILE_MODE_SEQUENTIAL; p.MaximumFileSize=8;
      p.LoggerNameOffset=offsetof(Storage,session);
      p.LogFileNameOffset=offsetof(Storage,file);
      TRACEHANDLE created=0;
      ULONG status=StartTraceW(&created,name,&p);
      require(HRESULT_FROM_WIN32(status),"Start video ETW");
      handle=created;
      EVENT_FILTER_DESCRIPTOR filter={};
      filter.Ptr=reinterpret_cast<ULONG_PTR>(&pid);
      filter.Size=sizeof(pid);
      filter.Type=EVENT_FILTER_TYPE_PID;
      ENABLE_TRACE_PARAMETERS params={};
      params.Version=ENABLE_TRACE_PARAMETERS_VERSION_2;
      params.EnableFilterDesc=&filter;
      params.FilterDescCount=1;
      const GUID provider=TGD3D_TRACE_GUID_INIT;
      status=EnableTraceEx2(handle,&provider,EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                            TRACE_LEVEL_INFORMATION,1,0,0,&params);
      require(HRESULT_FROM_WIN32(status),"Enable video ETW for current PID");
      std::printf("TRACE pid=%lu session=%ls\n",pid,name);
   }
   void finish() {
      if (!handle) return;
      auto &p=storage.properties;
      const ULONG status=ControlTraceW(handle,nullptr,&p,EVENT_TRACE_CONTROL_STOP);
      require(HRESULT_FROM_WIN32(status),"Stop video ETW");
      handle=0;
      std::printf("TRACE_DONE events_lost=%lu log_buffers_lost=%lu buffers_written=%lu\n",
                  p.EventsLost,p.LogBuffersLost,p.BuffersWritten);
      check(!p.EventsLost && !p.LogBuffersLost && p.BuffersWritten,"complete video ETW");
   }
   ~VideoTrace() {
      if (handle) ControlTraceW(handle,nullptr,&storage.properties,EVENT_TRACE_CONTROL_STOP);
   }
   VideoTrace()=default;
   VideoTrace(const VideoTrace &)=delete;
   VideoTrace &operator=(const VideoTrace &)=delete;
};

class ContextLock {
   AVD3D11VADeviceContext *hw;
public:
   explicit ContextLock(AVD3D11VADeviceContext *device):hw(device) { hw->lock(hw->lock_ctx); }
   ~ContextLock() { hw->unlock(hw->lock_ctx); }
   ContextLock(const ContextLock &)=delete;
   ContextLock &operator=(const ContextLock &)=delete;
};

class Preview {
   static constexpr UINT width = 640, height = 360;
   struct Window {
      HWND handle = nullptr;
      ~Window() { if (handle && IsWindow(handle)) DestroyWindow(handle); }
   } window;
   Com<IDXGISwapChain> swap;
   Com<ID3D11Texture2D> back, y_plane, uv_plane, readback;
   Com<ID3D11ShaderResourceView> y_view, uv_view;
   Com<ID3D11RenderTargetView> target;
   Com<ID3D11PixelShader> pixel;
   Com<ID3D11VertexShader> vertex;
   Com<ID3D11Buffer> constants;
   Com<ID3D11RasterizerState> raster;
   Com<ID3D11Query> completion;
   tgd3d_csc_push transform = {};

   const Journal &journal;
   const wchar_t *first_frame_capture;

   void texture(ID3D11Device *device, UINT w, UINT h, DXGI_FORMAT format, UINT bind,
                Com<ID3D11Texture2D> &out, const char *name) {
      D3D11_TEXTURE2D_DESC d={};
      d.Width=w; d.Height=h; d.ArraySize=d.MipLevels=d.SampleDesc.Count=1;
      d.Format=format; d.BindFlags=bind;
      d.Usage=bind ? D3D11_USAGE_DEFAULT : D3D11_USAGE_STAGING;
      d.CPUAccessFlags=bind ? 0 : D3D11_CPU_ACCESS_READ;
      journal.invoke(name,[&]{return device->CreateTexture2D(&d,nullptr,out.put());});
   }
   void pump() {
      MSG m;
      while (PeekMessageW(&m,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
      check(IsWindow(window.handle),"preview window closed");
   }
public:
   Preview(const Journal &j,const wchar_t *first_capture):journal(j),first_frame_capture(first_capture) {}
   void start(IDXGIFactory1 *factory, ID3D11Device *device) {
      journal.mark("CALL","CreatePreviewWindow");
      WNDCLASSW wc={}; wc.lpfnWndProc=DefWindowProcW;wc.hInstance=GetModuleHandleW(nullptr);
      wc.lpszClassName=L"Nvdec720Preview";wc.hCursor=LoadCursorW(nullptr,MAKEINTRESOURCEW(32512));
      check(RegisterClassW(&wc)!=0,"register preview window");
      const DWORD style=WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU;
      RECT rect={0,0,LONG(width),LONG(height)};check(AdjustWindowRect(&rect,style,FALSE),"size preview window");
      window.handle=CreateWindowExW(0,wc.lpszClassName,L"H.264 720p — NVDEC",style,CW_USEDEFAULT,CW_USEDEFAULT,
                                   rect.right-rect.left,rect.bottom-rect.top,nullptr,nullptr,wc.hInstance,nullptr);
      check(window.handle!=nullptr,"create preview window");
      journal.mark("RETURN","CreatePreviewWindow");
      DXGI_SWAP_CHAIN_DESC sd={};sd.BufferDesc.Width=width;sd.BufferDesc.Height=height;
      sd.BufferDesc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;sd.SampleDesc.Count=1;
      sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;sd.BufferCount=1;sd.OutputWindow=window.handle;
      sd.Windowed=TRUE;sd.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
      journal.invoke("CreateSwapChain",[&]{return factory->CreateSwapChain(device,&sd,swap.put());});
      journal.invoke("MakeWindowAssociation",[&]{return factory->MakeWindowAssociation(window.handle,DXGI_MWA_NO_ALT_ENTER);});
      journal.invoke("GetBackBuffer",[&]{return swap->GetBuffer(0,__uuidof(ID3D11Texture2D),(void **)back.put());});
      journal.invoke("CreateRenderTargetView",[&]{return device->CreateRenderTargetView(back.p,nullptr,target.put());});
      texture(device,1280,720,DXGI_FORMAT_R8_UNORM,D3D11_BIND_SHADER_RESOURCE,y_plane,"CreateLumaTexture");
      texture(device,640,360,DXGI_FORMAT_R8G8_UNORM,D3D11_BIND_SHADER_RESOURCE,uv_plane,"CreateChromaTexture");
      texture(device,width,height,sd.BufferDesc.Format,0,readback,"CreateReadbackTexture");
      journal.invoke("CreateLumaView",[&]{return device->CreateShaderResourceView(y_plane.p,nullptr,y_view.put());});
      journal.invoke("CreateChromaView",[&]{return device->CreateShaderResourceView(uv_plane.p,nullptr,uv_view.put());});
      journal.invoke("CreateVertexShader",[&]{return device->CreateVertexShader(nvdec720_preview_vs,
                     sizeof(nvdec720_preview_vs),nullptr,vertex.put());});
      journal.invoke("CreatePixelShader",[&]{return device->CreatePixelShader(nvdec720_preview_ps,
                     sizeof(nvdec720_preview_ps),nullptr,pixel.put());});
      D3D11_BUFFER_DESC bd={};bd.ByteWidth=sizeof(transform);bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
      bd.Usage=D3D11_USAGE_DEFAULT;
      journal.invoke("CreateConversionConstants",[&]{return device->CreateBuffer(&bd,nullptr,constants.put());});
      D3D11_RASTERIZER_DESC rd={};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;
      journal.invoke("CreateRasterizerState",[&]{return device->CreateRasterizerState(&rd,raster.put());});
      D3D11_QUERY_DESC q={D3D11_QUERY_EVENT,0};
      journal.invoke("CreateCompletionQuery",[&]{return device->CreateQuery(&q,completion.put());});
      journal.mark("READY","PreviewObjects");
   }
   void configure_color(AVD3D11VADeviceContext *hw,tgd3d_csc_matrix matrix,tgd3d_csc_range range) {
      tgd3d_csc_params(&transform,matrix,range);
      ContextLock lock(hw);
      journal.mark("CALL","UploadCSCConstants");
      hw->device_context->UpdateSubresource(constants.p,0,nullptr,&transform,0,0);
   }
   void draw(AVD3D11VADeviceContext *hw, const uint8_t *y, UINT y_pitch, const uint8_t *uv, UINT uv_pitch,
             unsigned index, const wchar_t *capture) {
      const bool first=index==0, last=index==5;
      const bool check_first=first && first_frame_capture;
      if (first) { ShowWindow(window.handle,SW_SHOWNORMAL);UpdateWindow(window.handle); }
      pump();
      auto *c=hw->device_context;
      {
         ContextLock lock(hw);
         if (first) journal.mark("CALL","UploadLuma");
         c->UpdateSubresource(y_plane.p,0,nullptr,y,y_pitch,0);
         if (first) journal.mark("CALL","UploadChroma");
         c->UpdateSubresource(uv_plane.p,0,nullptr,uv,uv_pitch,0);
         if (first) journal.mark("CALL","BindPipeline");
         D3D11_VIEWPORT viewport={0,0,float(width),float(height),0,1};
         ID3D11ShaderResourceView *views[]={y_view.p,uv_view.p};
         c->RSSetViewports(1,&viewport);c->RSSetState(raster.p);
         c->IASetInputLayout(nullptr);c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
         c->VSSetShader(vertex.p,nullptr,0);c->PSSetShader(pixel.p,nullptr,0);
         c->PSSetShaderResources(0,2,views);c->PSSetConstantBuffers(0,1,&constants.p);
         c->OMSetRenderTargets(1,&target.p,nullptr);
         if (first) journal.mark("CALL","DrawCSC");
         c->Draw(3,0);
         if (first) journal.mark("RETURN","DrawCSC");
         c->OMSetRenderTargets(0,nullptr,nullptr);views[0]=views[1]=nullptr;c->PSSetShaderResources(0,2,views);
         if (check_first) journal.mark("CALL","CopyFirstBackBuffer");
         if (last || check_first) c->CopyResource(readback.p,back.p);
         if (check_first) {
            journal.mark("RETURN","CopyFirstBackBuffer");
            journal.mark("CALL","FlushFirstDrawAndCopy");
            c->Flush();
            journal.mark("RETURN","FlushFirstDrawAndCopy");
         }
      }
      if (check_first) verify_frame(hw,y,y_pitch,uv,uv_pitch,index,first_frame_capture);
      const HRESULT presented=journal.invoke("Present",[&]{return swap->Present(1,0);});
      check(presented==S_OK,"preview did not present");
      if (last) verify_frame(hw,y,y_pitch,uv,uv_pitch,index,capture);
   }
   void verify_frame(AVD3D11VADeviceContext *hw,const uint8_t *y,UINT y_pitch,const uint8_t *uv,
                     UINT uv_pitch,unsigned index,const wchar_t *capture) {
      ContextLock lock(hw);
      auto *c=hw->device_context;
      D3D11_MAPPED_SUBRESOURCE result={};
      journal.invoke(index==0?"MapFirstRenderedFrame":"MapLastRenderedFrame",
                     [&]{return c->Map(readback.p,0,D3D11_MAP_READ,0,&result);});
      Mapping mapping{c,readback.p};check(result.pData && result.RowPitch>=width*4,"BGRA row pitch");
      unsigned worst=0;
      for (UINT row=0;row<5;++row) for (UINT col=0;col<7;++col) {
         const UINT x=col*(width-1)/6, py=row*(height-1)/4;
         const UINT sx=2*x+1, sy=2*py+1;
         const float l=(y[size_t(sy)*y_pitch+sx]-transform.range[0])*transform.range[1];
         const uint8_t *chroma=uv+size_t(sy/2)*uv_pitch+2*(sx/2);
         const float u=(float(chroma[0])-128)*transform.range[2],v=(float(chroma[1])-128)*transform.range[2];
         const auto *actual=static_cast<const uint8_t *>(result.pData)+size_t(py)*result.RowPitch+4*x;
         for (unsigned ch=0;ch<3;++ch) {
            const float *m=transform.yuv2rgb+ch;
            const int expected=int(std::clamp(m[0]*l+m[4]*u+m[8]*v+m[12],0.f,1.f)*255.f+.5f);
            worst=std::max(worst,unsigned(std::abs(int(actual[2-ch])-expected)));
         }
      }
      std::printf("CSC_FRAME index=%u points=35 max_rgb_error=%u\n",index,worst);
      if (capture) {
         FILE *file=_wfopen(capture,L"wb");
         check(file!=nullptr,"open preview capture");
         bool ok=std::fprintf(file,"P6\n%u %u\n255\n",width,height)>0;
         uint8_t row[3*width];
         for (UINT y=0;y<height && ok;++y) {
            const auto *bgra=static_cast<const uint8_t *>(result.pData)+size_t(y)*result.RowPitch;
            for (UINT x=0;x<width;++x) {
               row[3*x]=bgra[4*x+2];row[3*x+1]=bgra[4*x+1];row[3*x+2]=bgra[4*x];
            }
            ok=std::fwrite(row,1,sizeof(row),file)==sizeof(row);
         }
         if (ok && index==0)
            ok=std::fflush(file)==0 && _commit(_fileno(file))==0;
         ok=std::fclose(file)==0 && ok;check(ok,"write preview capture");
      }
      mapping.finish();
      check(worst<=2,"GPU CSC pixel mismatch");
      journal.mark("VERIFIED",index==0?"FirstRenderedFrame":"LastRenderedFrame");
   }
   void finish(AVD3D11VADeviceContext *hw) {
      auto *c=hw->device_context;
      { ContextLock lock(hw);c->End(completion.p);c->Flush(); }
      const ULONGLONG start=GetTickCount64();BOOL done=FALSE;
      while (!done) {
         HRESULT hr;
         { ContextLock lock(hw);hr=c->GetData(completion.p,&done,sizeof(done),D3D11_ASYNC_GETDATA_DONOTFLUSH); }
         if (FAILED(hr)) require(hr,"Preview completion query");
         check(GetTickCount64()-start<10000,"preview completion timeout");
         if (hr==S_FALSE || !done) { done=FALSE;pump();Sleep(1); }
      }
      require(DwmFlush(),"DWM flush after decoded preview");
      DWORD cloaked=0;
      require(DwmGetWindowAttribute(window.handle,DWMWA_CLOAKED,&cloaked,sizeof(cloaked)),"Get preview window state");
      check(IsWindowVisible(window.handle) && !cloaked,"preview window not visible");
      const ULONGLONG visible=GetTickCount64();
      while (GetTickCount64()-visible<2000) {pump();Sleep(10);}
      ContextLock lock(hw);
      c->PSSetShader(nullptr,nullptr,0);c->VSSetShader(nullptr,nullptr,0);
      ID3D11Buffer *none=nullptr;c->PSSetConstantBuffers(0,1,&none);c->RSSetState(nullptr);
      journal.mark("COMPLETE","PresentedAndRetired");
      std::printf("PREVIEW_COMPLETE gpu_event=1 last_frame_hold_ms=%llu\n",GetTickCount64()-visible);
   }
};

template<typename T, void (*release)(T **)> struct Av {
   T *p = nullptr;
   Av() = default;
   Av(const Av &) = delete;
   Av &operator=(const Av &) = delete;
   ~Av() { release(&p); }
   T *operator->() const { return p; }
};

static void av_check(int code, const char *operation)
{
   if (code >= 0) return;
   char error[AV_ERROR_MAX_STRING_SIZE] = {};
   av_strerror(code,error,sizeof(error));
   std::fprintf(stderr,"%s: %s (%d)\n",operation,error,code);
   throw Failure{E_FAIL,operation};
}

static AVPixelFormat hardware_format(AVCodecContext *ctx, const AVPixelFormat *formats)
{
   for (; *formats != AV_PIX_FMT_NONE; ++formats) {
      if (*formats != AV_PIX_FMT_D3D11) continue;
      AVBufferRef *pool = nullptr;
      int ret = avcodec_get_hw_frames_parameters(ctx,ctx->hw_device_ctx,*formats,&pool);
      if (ret < 0) return AV_PIX_FMT_NONE;
      const auto *desc = reinterpret_cast<const AVHWFramesContext *>(pool->data);
      std::printf("FRAME_POOL count=%d width=%d height=%d format=%s\n",desc->initial_pool_size,
                  desc->width,desc->height,av_get_pix_fmt_name(desc->sw_format));
      ret = av_hwframe_ctx_init(pool);
      if (ret < 0) { av_buffer_unref(&pool);return AV_PIX_FMT_NONE; }
      av_buffer_unref(&ctx->hw_frames_ctx);
      ctx->hw_frames_ctx=pool;
      return *formats;
   }
   return AV_PIX_FMT_NONE;
}

static uint64_t luid_value(LUID luid)
{ return uint64_t(uint32_t(luid.HighPart)) << 32 | luid.LowPart; }

static std::string utf8_path(const wchar_t *path)
{
   int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,path,-1,nullptr,0,nullptr,nullptr);
   check(n>0,"encode sample path");
   std::string s(size_t(n),'\0');
   check(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,path,-1,s.data(),n,nullptr,nullptr)==n,
         "encode sample path");
   s.pop_back();return s;
}

static void check_pixels(const AVFrame &frame, const uint8_t *expected, unsigned index)
{
   check(frame.format==AV_PIX_FMT_NV12 && frame.width==1280 && frame.height==720 &&
         frame.data[0] && frame.data[1] && frame.linesize[0]>=1280 && frame.linesize[1]>=1280,
         "hardware readback NV12 layout");
   std::vector<uint8_t> i420(1280*720*3/2);
   for (int y=0;y<720;++y) std::memcpy(i420.data()+y*1280,frame.data[0]+size_t(y)*frame.linesize[0],1280);
   for (int y=0;y<360;++y) {
      const uint8_t *uv=frame.data[1]+size_t(y)*frame.linesize[1];
      for (int x=0;x<640;++x) {
         i420[921600+y*640+x]=uv[2*x];i420[1152000+y*640+x]=uv[2*x+1];
      }
   }
   uint8_t hash[32];tg_nvdec_sha256(i420.data(),i420.size(),hash);
   std::printf("NV12_FRAME index=%u sha256=",index);
   for (uint8_t b:hash) std::printf("%02x",b);
   std::putchar('\n');check(std::memcmp(hash,expected,32)==0,"NV12 software-reference mismatch");
}

enum class Mode { Present, PrepareOnly, DecodeOnly };

static void run(uint64_t wanted,const wchar_t *sample,const wchar_t *reference,const wchar_t *capture,
                Mode mode, const Journal &journal,const wchar_t *first_frame_capture)
{
   Com<IDXGIFactory1> factory;
   journal.invoke("CreateDXGIFactory1",[&]{return CreateDXGIFactory1(__uuidof(IDXGIFactory1),(void **)factory.put());});
   UINT ordinal=0;
   for (;;++ordinal) {
      Com<IDXGIAdapter1> adapter;
      require(factory->EnumAdapters1(ordinal,adapter.put()),"Find native adapter");
      DXGI_ADAPTER_DESC1 d={};require(adapter->GetDesc1(&d),"Describe adapter");
      if (!(d.Flags&DXGI_ADAPTER_FLAG_SOFTWARE) && luid_value(d.AdapterLuid)==wanted) break;
   }
   Av<AVBufferRef,av_buffer_unref> device;
   const auto device_index=std::to_string(ordinal);
   av_check(journal.av_call("av_hwdevice_ctx_create_D3D11VA",[&]{
      return av_hwdevice_ctx_create(&device.p,AV_HWDEVICE_TYPE_D3D11VA,device_index.c_str(),nullptr,0);
   }),"Create D3D11VA device");
   auto *hw=static_cast<AVD3D11VADeviceContext *>(reinterpret_cast<AVHWDeviceContext *>(device->data)->hwctx);
   Com<IDXGIDevice> dxgi;Com<IDXGIAdapter> adapter;DXGI_ADAPTER_DESC actual={};
   require(hw->device->QueryInterface(__uuidof(IDXGIDevice),(void **)dxgi.put()),"Query decode DXGI device");
   require(dxgi->GetAdapter(adapter.put()),"Get decode adapter");
   require(adapter->GetDesc(&actual),"Get decode adapter LUID");
   check(luid_value(actual.AdapterLuid)==wanted,"decoder adapter mismatch");
   std::printf("DECODER_ADAPTER luid=%016llx ordinal=%u description=%ls\n",(unsigned long long)wanted,ordinal,actual.Description);
   check(hw->lock && hw->unlock && hw->device_context,"D3D11VA context lock");
   std::unique_ptr<Preview> preview;
   if (mode!=Mode::DecodeOnly) {
      preview=std::make_unique<Preview>(journal,first_frame_capture);
      preview->start(factory.p,hw->device);
   }
   if (mode==Mode::PrepareOnly) {
      preview.reset();
      require(hw->device->GetDeviceRemovedReason(),"Device after preview preparation");
      journal.mark("COMPLETE","PreparationOnly");
      std::puts("PREPARATION_ONLY=1 decoded=0 presented=0");
      return;
   }
   const auto hashes=references(reference);
   Av<AVFormatContext,avformat_close_input> input;
   const auto sample_path=utf8_path(sample);
   av_check(journal.av_call("avformat_open_input",[&]{
      return avformat_open_input(&input.p,sample_path.c_str(),nullptr,nullptr);
   }),"Open sample");
   av_check(journal.av_call("avformat_find_stream_info",[&]{
      return avformat_find_stream_info(input.p,nullptr);
   }),"Read stream information");
   const AVCodec *codec=nullptr;
   int stream=av_find_best_stream(input.p,AVMEDIA_TYPE_VIDEO,-1,-1,&codec,0);
   av_check(stream,"Find video stream");
   const AVCodecParameters *params=input->streams[stream]->codecpar;
   check(codec && params->codec_id==AV_CODEC_ID_H264 && params->width==1280 && params->height==720,
         "720p H.264 input required");
   journal.mark("CALL","OpenDecoder");
   Av<AVCodecContext,avcodec_free_context> decoder;
   decoder.p=avcodec_alloc_context3(codec);check(decoder.p!=nullptr,"allocate decoder");
   av_check(avcodec_parameters_to_context(decoder.p,params),"Set decoder parameters");
   decoder->thread_count=1;decoder->hw_device_ctx=av_buffer_ref(device.p);
   check(decoder->hw_device_ctx!=nullptr,"reference decoder device");
   decoder->get_format=hardware_format;
   av_check(avcodec_open2(decoder.p,codec,nullptr),"Open H.264 decoder");
   Av<AVPacket,av_packet_free> packet;packet.p=av_packet_alloc();check(packet.p!=nullptr,"allocate packet");
   Av<AVFrame,av_frame_free> frame,cpu;frame.p=av_frame_alloc();cpu.p=av_frame_alloc();
   check(frame.p && cpu.p,"allocate frames");
   unsigned frames=0;int64_t last=AV_NOPTS_VALUE;const ULONGLONG start=GetTickCount64();
   auto receive=[&](bool flushing) {
      for (;;) {
         int ret=avcodec_receive_frame(decoder.p,frame.p);
         if (ret==AVERROR_EOF) return;
         if (ret==AVERROR(EAGAIN)) {check(!flushing,"unexpected EAGAIN while draining");return;}
         av_check(ret,"Receive decoded frame");
         check(frames<6 && frame->format==AV_PIX_FMT_D3D11 && frame->hw_frames_ctx && frame->data[0],
               "native D3D11 frame required");
         auto *tex=reinterpret_cast<ID3D11Texture2D *>(frame->data[0]);Com<ID3D11Device> owner;tex->GetDevice(owner.put());
         D3D11_TEXTURE2D_DESC desc={};tex->GetDesc(&desc);
         const auto slice=reinterpret_cast<uintptr_t>(frame->data[1]);
         check(owner.p==hw->device && desc.Format==DXGI_FORMAT_NV12 && desc.Width==1280 && desc.Height==720 &&
               (desc.BindFlags&D3D11_BIND_DECODER) && slice<desc.ArraySize,"decode texture ownership");
         check(frame->pts!=AV_NOPTS_VALUE && (last==AV_NOPTS_VALUE || frame->pts>last),"display frame order");last=frame->pts;
         av_check(av_hwframe_transfer_data(cpu.p,frame.p,0),"Read native NV12 frame");
         check_pixels(*cpu.p,hashes.data()+frames*32,frames);
         journal.mark("VERIFIED","NV12Frame");
         if (preview && frames==0) {
            check(frame->colorspace==AVCOL_SPC_UNSPECIFIED || frame->colorspace==AVCOL_SPC_BT709 ||
                  frame->colorspace==AVCOL_SPC_SMPTE170M || frame->colorspace==AVCOL_SPC_BT470BG,"unsupported matrix");
            check(frame->color_range==AVCOL_RANGE_UNSPECIFIED || frame->color_range==AVCOL_RANGE_MPEG ||
                  frame->color_range==AVCOL_RANGE_JPEG,"unsupported range");
            const auto matrix=frame->colorspace==AVCOL_SPC_BT709?TGD3D_CSC_BT709:TGD3D_CSC_BT601;
            const auto range=frame->color_range==AVCOL_RANGE_JPEG?TGD3D_CSC_FULL:TGD3D_CSC_LIMITED;
            std::printf("CSC matrix=%s range=%s input_matrix=%d input_range=%d untagged_default=BT601-limited transfer=readback-upload\n",
                        matrix==TGD3D_CSC_BT709?"BT709":"BT601",range==TGD3D_CSC_FULL?"full":"limited",
                        frame->colorspace,frame->color_range);
            preview->configure_color(hw,matrix,range);
         }
         std::printf("SAMPLE index=%u subresource=%llu pts=%lld\n",frames,(unsigned long long)slice,(long long)last);
         if (preview)
            preview->draw(hw,cpu->data[0],cpu->linesize[0],cpu->data[1],cpu->linesize[1],frames,
                          frames==5 ? capture : nullptr);
         ++frames;av_frame_unref(cpu.p);av_frame_unref(frame.p);
      }
   };
   for (;;) {
      int ret=av_read_frame(input.p,packet.p);
      if (ret==AVERROR_EOF) break;
      av_check(ret,"Read packet");
      if (packet->stream_index==stream) {av_check(avcodec_send_packet(decoder.p,packet.p),"Submit packet");receive(false);}
      av_packet_unref(packet.p);
   }
   av_check(avcodec_send_packet(decoder.p,nullptr),"Drain decoder");receive(true);
   check(frames==6,"incomplete decoded sequence");
   if (preview) { preview->finish(hw);preview.reset(); }
   journal.mark("CALL","CloseDecoder");
   avcodec_free_context(&decoder.p);
   journal.mark("RETURN","CloseDecoder");
   require(hw->device->GetDeviceRemovedReason(),"Final device status");
   std::printf("DECODED_FRAMES=%u WINDOW_FRAMES=%u elapsed_ms=%llu\n",frames,
               mode==Mode::Present?frames:0,GetTickCount64()-start);
}

int wmain(int argc,wchar_t **argv)
{
   SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
   const bool trace_requested=argc>=2 && std::wcscmp(argv[argc-1],L"--trace")==0;
   if (trace_requested) --argc;
   const bool check_first=argc>=2 && std::wcscmp(argv[argc-1],L"--check-first-submit")==0;
   if (check_first) --argc;
   const wchar_t *evidence=nullptr;
   if (argc>=3 && std::wcscmp(argv[argc-2],L"--evidence")==0) {
      evidence=argv[argc-1];
      argc-=2;
   }
   if ((argc!=4 && argc!=5) || ((check_first || trace_requested) && !evidence)) {
      std::fprintf(stderr,"usage: nvdec720-av-window <native-luid-hex> <sample.mp4> <reference.sha256> [last-frame.ppm | --prepare-only | --decode-only] [--evidence new-directory [--check-first-submit] [--trace]]\n");
      return 2;
   }
   Mode mode=Mode::Present;
   if (argc==5 && std::wcscmp(argv[4],L"--prepare-only")==0) mode=Mode::PrepareOnly;
   if (argc==5 && std::wcscmp(argv[4],L"--decode-only")==0) mode=Mode::DecodeOnly;
   if (check_first && mode!=Mode::Present) return 2;
   wchar_t *end=nullptr;const uint64_t wanted=std::wcstoull(argv[1],&end,16);
   if (!wanted || !end || *end || std::wcslen(argv[1])!=16) return 2;
   try {
      Journal journal(evidence);
      journal.mark("ENTER","wmain");
      try {
         if (evidence) {
            const std::wstring dir=evidence;
            check(_wfreopen((dir+L"\\stderr.txt").c_str(),L"wb",stderr)!=nullptr,"open evidence stderr");
            check(_wfreopen((dir+L"\\stdout.txt").c_str(),L"wb",stdout)!=nullptr,"open evidence stdout");
            check(SetEnvironmentVariableW(L"TGD3D_VIDEO_VALIDATE",L"1"),"enable process-local video validation");
         }
         std::setvbuf(stdout,nullptr,_IONBF,0);
         std::setvbuf(stderr,nullptr,_IONBF,0);
         DWORD session=0;
         check(ProcessIdToSessionId(GetCurrentProcessId(),&session),"query interactive session");
         std::printf("START pid=%lu session=%lu\n",GetCurrentProcessId(),session);
         VideoTrace trace;
         if (trace_requested) {
            journal.mark("CALL","TraceCurrentProcess");
            trace.start(evidence);
            journal.mark("RETURN","TraceCurrentProcess");
         }
         const std::wstring first_image=check_first?std::wstring(evidence)+L"\\first-frame.ppm":L"";
         run(wanted,argv[2],argv[3],argc==5 && mode==Mode::Present?argv[4]:nullptr,
             mode,journal,check_first?first_image.c_str():nullptr);
         trace.finish();
         journal.mark("EXIT","RunAndRelease",S_OK);
         return 0;
      } catch (const Failure &f) {
         journal.mark("FAIL",f.call,f.result);
         throw;
      }
   }
   catch(const Failure &f){std::fprintf(stderr,"FAIL %s=0x%08lx\n",f.call,(unsigned long)f.result);return 4;}
   catch(const std::bad_alloc &){std::fprintf(stderr,"FAIL allocation\n");return 5;}
}
