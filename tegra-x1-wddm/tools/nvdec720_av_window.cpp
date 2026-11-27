/* SPDX-License-Identifier: MIT
 * Synchronous libavcodec D3D11VA acceptance on the selected native adapter.
 * Uploaded planes come from a checked hardware-frame readback. The D3D11
 * pixel shader performs CSC on the same device as the decoder.
 */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0a00
#endif
#define WIN32_LEAN_AND_MEAN
#include "nvdec720_fixture.hpp"
#include "nvdec720_preview_vs.h"
#include "nvdec720_preview_ps.h"
#include <dxgi1_2.h>
#include <dwmapi.h>
#include <bcrypt.h>
#include <evntprov.h>
#include <evntrace.h>
#include <io.h>
#include <algorithm>
#include <initializer_list>
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

#ifndef NVDEC_PREVIEW_SCALE
#define NVDEC_PREVIEW_SCALE 2
#endif
static constexpr int preview_scale = NVDEC_PREVIEW_SCALE;
static_assert(preview_scale == 2 || preview_scale == 3);
static constexpr int preview_width = 640, preview_height = 360;
static constexpr int sample_width = preview_width * preview_scale;
static constexpr int sample_height = preview_height * preview_scale;
static constexpr int coded_height = (sample_height + 15) & ~15;

#ifndef EVENT_FILTER_TYPE_PID
// The Windows SDK defines this scope filter in evntprov.h.
#define EVENT_FILTER_TYPE_PID 0x80000004u
#endif
#ifndef EVENT_FILTER_TYPE_EVENT_ID
#define EVENT_FILTER_TYPE_EVENT_ID 0x80000200u
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

// Request the client PID for each provider. DxgKrnl can also emit events
// under other process identities. Attribute its records by their event headers.
class VideoTrace {
   struct Storage {
      EVENT_TRACE_PROPERTIES properties;
      wchar_t session[64];
      wchar_t file[1024];
   } storage = {};
   TRACEHANDLE handle = 0;
   HANDLE stop_flush = nullptr, flusher = nullptr;
   ULONG flush_status = ERROR_SUCCESS;

   ULONG flush_buffers() {
      return ControlTraceW(handle,nullptr,&storage.properties,EVENT_TRACE_CONTROL_FLUSH);
   }
   static DWORD WINAPI flush_during_present(void *arg) {
      auto &trace=*static_cast<VideoTrace *>(arg);
      while (WaitForSingleObject(trace.stop_flush,40)==WAIT_TIMEOUT) {
         trace.flush_status=trace.flush_buffers();
         if (trace.flush_status!=ERROR_SUCCESS) break;
      }
      return 0;
   }
public:
   void start(const wchar_t *directory,bool window_present) {
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
      p.BufferSize=window_present?8:64; p.MinimumBuffers=4; p.MaximumBuffers=32; p.FlushTimer=1;
      p.LogFileMode=EVENT_TRACE_FILE_MODE_SEQUENTIAL; p.MaximumFileSize=8;
      p.LoggerNameOffset=offsetof(Storage,session);
      p.LogFileNameOffset=offsetof(Storage,file);
      TRACEHANDLE created=0;
      ULONG status=StartTraceW(&created,name,&p);
      require(HRESULT_FROM_WIN32(status),"Start video ETW");
      handle=created;
      EVENT_FILTER_DESCRIPTOR filters[2]={};
      filters[0].Ptr=reinterpret_cast<ULONG_PTR>(&pid);
      filters[0].Size=sizeof(pid);
      filters[0].Type=EVENT_FILTER_TYPE_PID;
      ENABLE_TRACE_PARAMETERS params={};
      params.Version=ENABLE_TRACE_PARAMETERS_VERSION_2;
      params.EnableFilterDesc=filters;
      params.FilterDescCount=1;
      const GUID provider=TGD3D_TRACE_GUID_INIT;
      status=EnableTraceEx2(handle,&provider,EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                            TRACE_LEVEL_INFORMATION,1,0,0,&params);
      require(HRESULT_FROM_WIN32(status),"Enable video ETW for current PID");
      if (window_present) {
         auto enable=[&](const GUID &guid,ULONGLONG keywords,std::initializer_list<USHORT> ids,const char *name) {
            // Public EVENT_FILTER_EVENT_ID header followed by the selected IDs.
            struct EventIds { BOOLEAN include; UCHAR reserved; USHORT count,values[16]; };
            static_assert(offsetof(EventIds,values)==4);
            EventIds list={TRUE,0,0,{}};
            check(ids.size()<=ARRAYSIZE(list.values),"presentation event ID count");
            list.count=static_cast<USHORT>(ids.size());
            std::copy(ids.begin(),ids.end(),list.values);
            filters[1].Type=EVENT_FILTER_TYPE_EVENT_ID;
            filters[1].Ptr=reinterpret_cast<ULONG_PTR>(&list);
            filters[1].Size=static_cast<ULONG>(offsetof(EventIds,values)+ids.size()*sizeof(USHORT));
            params.FilterDescCount=2;
            require(HRESULT_FROM_WIN32(EnableTraceEx2(handle,&guid,EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                    TRACE_LEVEL_VERBOSE,keywords,0,0,&params)),name);
         };
         const GUID dxgi={0xca11c036,0x0102,0x4a2d,{0xa6,0xad,0xf0,0x3c,0xfe,0xd5,0xd3,0xc9}};
         const GUID dxgkrnl={0x802ec45a,0x1e99,0x4b83,{0x99,0x20,0x87,0xc9,0x82,0x77,0xba,0x9d}};
         enable(dxgi,0x4000000000000002ull,{42,43,178,179},"Enable DXGI Present events");
         enable(dxgkrnl,1,{166,168,171,172,173,184,215,252,266,294,295,501},"Enable DxgKrnl present and sync events");
      }
      std::printf("TRACE pid=%lu session=%ls\n",pid,name);
   }
   void begin_first_present(const Journal &journal) {
      if (!handle) return;
      journal.invoke("FlushTraceBeforePresent",[&]{return HRESULT_FROM_WIN32(flush_buffers());});
      stop_flush=CreateEventW(nullptr,TRUE,FALSE,nullptr);
      check(stop_flush!=nullptr,"create ETW flush stop event");
      flusher=CreateThread(nullptr,0,flush_during_present,this,0,nullptr);
      check(flusher!=nullptr,"start first-Present ETW flusher");
   }
   void end_first_present() {
      if (flusher) {
         SetEvent(stop_flush);
         WaitForSingleObject(flusher,INFINITE);
         CloseHandle(flusher);flusher=nullptr;
      }
      if (stop_flush) { CloseHandle(stop_flush);stop_flush=nullptr; }
   }
   void finish() {
      if (!handle) return;
      end_first_present();
      auto &p=storage.properties;
      const ULONG status=ControlTraceW(handle,nullptr,&p,EVENT_TRACE_CONTROL_STOP);
      require(HRESULT_FROM_WIN32(status),"Stop video ETW");
      handle=0;
      require(HRESULT_FROM_WIN32(flush_status),"First-Present ETW flush status");
      std::printf("TRACE_DONE events_lost=%lu log_buffers_lost=%lu buffers_written=%lu\n",
                  p.EventsLost,p.LogBuffersLost,p.BuffersWritten);
      check(!p.EventsLost && !p.LogBuffersLost && p.BuffersWritten,"complete video ETW");
   }
   ~VideoTrace() {
      end_first_present();
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
   static constexpr UINT width = preview_width, height = preview_height;
   struct Window {
      HWND handle = nullptr;
      ~Window() { if (handle && IsWindow(handle)) DestroyWindow(handle); }
   } window;
   Com<IDXGISwapChain1> swap;
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
   VideoTrace &trace;
   const wchar_t *first_frame_capture;
   const bool visual_check;

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
   ULONGLONG hold_frame(DWORD milliseconds) {
      const ULONGLONG start=GetTickCount64();
      while (GetTickCount64()-start<milliseconds) {pump();Sleep(10);}
      return GetTickCount64()-start;
   }
public:
   Preview(const Journal &j,VideoTrace &t,const wchar_t *first_capture,bool visual):
      journal(j),trace(t),first_frame_capture(first_capture),visual_check(visual) {}
   void start(IDXGIFactory2 *factory, ID3D11Device *device) {
      journal.mark("CALL","CreatePreviewWindow");
      WNDCLASSW wc={}; wc.lpfnWndProc=DefWindowProcW;wc.hInstance=GetModuleHandleW(nullptr);
      wc.lpszClassName=L"Nvdec720Preview";wc.hCursor=LoadCursorW(nullptr,MAKEINTRESOURCEW(32512));
      check(RegisterClassW(&wc)!=0,"register preview window");
      const DWORD style=WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU;
      RECT rect={0,0,LONG(width),LONG(height)};check(AdjustWindowRect(&rect,style,FALSE),"size preview window");
      const wchar_t *title = sample_height == 1080 ? L"H.264 1080p — NVDEC" : L"H.264 720p — NVDEC";
      window.handle=CreateWindowExW(0,wc.lpszClassName,title,style,CW_USEDEFAULT,CW_USEDEFAULT,
                                   rect.right-rect.left,rect.bottom-rect.top,nullptr,nullptr,wc.hInstance,nullptr);
      check(window.handle!=nullptr,"create preview window");
      journal.mark("RETURN","CreatePreviewWindow");
      DXGI_SWAP_CHAIN_DESC1 sd={};
      sd.Width=width; sd.Height=height; sd.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
      sd.SampleDesc.Count=1; sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
      sd.BufferCount=2; sd.Scaling=DXGI_SCALING_STRETCH;
      sd.SwapEffect=DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL; sd.AlphaMode=DXGI_ALPHA_MODE_IGNORE;
      journal.invoke("CreateSwapChainForHwnd",[&]{
         return factory->CreateSwapChainForHwnd(device,window.handle,&sd,nullptr,nullptr,swap.put());
      });
      DXGI_SWAP_CHAIN_DESC1 actual={};
      journal.invoke("GetWindowSwapChainDesc",[&]{return swap->GetDesc1(&actual);});
      check(actual.Width==width && actual.Height==height && actual.Format==sd.Format &&
            actual.BufferCount==sd.BufferCount && actual.SwapEffect==sd.SwapEffect &&
            actual.SampleDesc.Count==1 && !actual.SampleDesc.Quality,"window swapchain description");
      journal.mark("VERIFIED","TwoBufferFlipSequential");
      journal.invoke("MakeWindowAssociation",[&]{return factory->MakeWindowAssociation(window.handle,DXGI_MWA_NO_ALT_ENTER);});
      journal.invoke("GetBackBuffer",[&]{return swap->GetBuffer(0,__uuidof(ID3D11Texture2D),(void **)back.put());});
      journal.invoke("CreateRenderTargetView",[&]{return device->CreateRenderTargetView(back.p,nullptr,target.put());});
      texture(device,sample_width,sample_height,DXGI_FORMAT_R8_UNORM,D3D11_BIND_SHADER_RESOURCE,y_plane,"CreateLumaTexture");
      texture(device,sample_width/2,sample_height/2,DXGI_FORMAT_R8G8_UNORM,D3D11_BIND_SHADER_RESOURCE,uv_plane,"CreateChromaTexture");
      texture(device,width,height,sd.Format,0,readback,"CreateReadbackTexture");
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
      if (first) trace.begin_first_present(journal);
      const HRESULT presented=journal.invoke("Present",[&]{return swap->Present(1,0);});
      if (first) trace.end_first_present();
      check(presented==S_OK,"preview did not present");
      if (last) verify_frame(hw,y,y_pitch,uv,uv_pitch,index,capture);
      if (visual_check && !last) {
         journal.mark("CALL","VisualFrameHold");
         hold_frame(1000);
         journal.mark("RETURN","VisualFrameHold");
      }
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
         const UINT sx=preview_scale*x+preview_scale/2, sy=preview_scale*py+preview_scale/2;
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
      {
         ContextLock lock(hw);
         journal.mark("CALL","EndPreviewGPUEvent");
         c->End(completion.p);
         journal.mark("RETURN","EndPreviewGPUEvent");
         journal.mark("CALL","FlushPreviewGPUEvent");
         c->Flush();
         journal.mark("RETURN","FlushPreviewGPUEvent");
      }
      const ULONGLONG start=GetTickCount64();BOOL done=FALSE;
      journal.mark("CALL","WaitPreviewGPUEvent");
      while (!done) {
         HRESULT hr;
         { ContextLock lock(hw);hr=c->GetData(completion.p,&done,sizeof(done),D3D11_ASYNC_GETDATA_DONOTFLUSH); }
         if (FAILED(hr)) require(hr,"Preview completion query");
         check(hr==S_OK || hr==S_FALSE,"unexpected preview query status");
         check(GetTickCount64()-start<10000,"preview completion timeout");
         if (hr==S_FALSE || !done) { done=FALSE;pump();Sleep(1); }
      }
      journal.mark("VERIFIED","PreviewGPUEvent");
      journal.invoke("DwmFlush",[&]{return DwmFlush();});
      DWORD cloaked=0;
      journal.invoke("GetPreviewWindowCloaking",[&]{
         return DwmGetWindowAttribute(window.handle,DWMWA_CLOAKED,&cloaked,sizeof(cloaked));
      });
      check(IsWindowVisible(window.handle) && !cloaked,"preview window not visible");
      journal.mark("VERIFIED","VisibleUncloakedWindow");
      journal.mark("CALL","FinalFrameHold");
      const ULONGLONG held=hold_frame(visual_check?8000:2000);
      journal.mark("RETURN","FinalFrameHold");
      ContextLock lock(hw);
      c->PSSetShader(nullptr,nullptr,0);c->VSSetShader(nullptr,nullptr,0);
      ID3D11Buffer *none=nullptr;c->PSSetConstantBuffers(0,1,&none);c->RSSetState(nullptr);
      journal.mark("COMPLETE","PresentedAndRetired");
      std::printf("PREVIEW_COMPLETE gpu_event=1 last_frame_hold_ms=%llu\n",held);
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
   check(frame.format==AV_PIX_FMT_NV12 && frame.width==sample_width && frame.height==sample_height &&
         frame.data[0] && frame.data[1] && frame.linesize[0]>=sample_width && frame.linesize[1]>=sample_width,
         "hardware readback NV12 layout");
   constexpr size_t luma_bytes = sample_width * sample_height;
   std::vector<uint8_t> i420(luma_bytes*3/2);
   uint8_t *u=i420.data()+luma_bytes, *v=u+luma_bytes/4;
   for (int y=0;y<sample_height;++y)
      std::memcpy(i420.data()+y*sample_width,frame.data[0]+size_t(y)*frame.linesize[0],sample_width);
   for (int y=0;y<sample_height/2;++y) {
      const uint8_t *uv=frame.data[1]+size_t(y)*frame.linesize[1];
      for (int x=0;x<sample_width/2;++x) {
         *u++=uv[2*x]; *v++=uv[2*x+1];
      }
   }
   uint8_t hash[32];
   static_assert(luma_bytes * 3 / 2 <= ULONG_MAX);
   check(BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, i420.data(),
                    static_cast<ULONG>(i420.size()), hash, sizeof(hash)) >= 0,
         "hash canonical frame with SHA-256");
   std::printf("NV12_FRAME index=%u sha256=",index);
   for (uint8_t b:hash) std::printf("%02x",b);
   std::putchar('\n');check(std::memcmp(hash,expected,32)==0,"NV12 software-reference mismatch");
}

enum class Mode { Present, PrepareOnly, DecodeOnly };

static void require_loaded_umd(const wchar_t *expected,const Journal &journal)
{
   if (!expected) return;
   HMODULE module=GetModuleHandleW(L"TegraGpuD3DVk.dll");
   check(module!=nullptr,"locate loaded Tegra UMD");
   std::vector<wchar_t> path(32768);
   DWORD length=GetModuleFileNameW(module,path.data(),static_cast<DWORD>(path.size()));
   check(length && length<path.size(),"read loaded UMD path");
   std::unique_ptr<FILE,decltype(&std::fclose)> file(_wfopen(path.data(),L"rb"),std::fclose);
   check(file && !std::fseek(file.get(),0,SEEK_END),"open loaded UMD file");
   const long size=std::ftell(file.get());
   check(size>0 && !std::fseek(file.get(),0,SEEK_SET),"measure loaded UMD file");
   std::vector<uint8_t> data(static_cast<size_t>(size));
   check(std::fread(data.data(),1,data.size(),file.get())==data.size(),"read loaded UMD file");
   uint8_t hash[32];tg_nvdec_sha256(data.data(),data.size(),hash);
   const wchar_t digits[]=L"0123456789abcdef";
   wchar_t hex[65]={};
   for (size_t i=0;i<sizeof(hash);++i) {hex[2*i]=digits[hash[i]>>4];hex[2*i+1]=digits[hash[i]&15];}
   std::printf("LOADED_UMD sha256=%ls path=%ls\n",hex,path.data());
   check(!_wcsicmp(hex,expected),"loaded UMD differs from requested candidate");
   journal.mark("VERIFIED","LoadedCandidateUMD");
}

struct DecodeTimings {
   struct Frame {
      uint64_t readback = 0, verify = 0, preview = 0;
   } frames[6];
   uint64_t frequency = 0, submit = 0, receive = 0;

   explicit DecodeTimings(bool enabled) {
      LARGE_INTEGER f;
      if (enabled) {
         check(QueryPerformanceFrequency(&f) && f.QuadPart > 0, "query performance frequency");
         frequency = static_cast<uint64_t>(f.QuadPart);
      }
   }

   uint64_t now() const {
      if (!frequency) return 0;
      LARGE_INTEGER t;
      QueryPerformanceCounter(&t);
      return static_cast<uint64_t>(t.QuadPart);
   }

   void print(uint64_t loop) const {
      if (!frequency) return;
      std::printf("DECODE_TIMING qpc=%llu loop=%llu send=%llu receive=%llu\n",
                  (unsigned long long)frequency, (unsigned long long)loop,
                  (unsigned long long)submit, (unsigned long long)receive);
      for (unsigned i = 0; i < 6; ++i) {
         const auto &f = frames[i];
         std::printf("FRAME_TIMING index=%u readback=%llu verify=%llu preview=%llu\n", i,
                     (unsigned long long)f.readback, (unsigned long long)f.verify,
                     (unsigned long long)f.preview);
      }
   }
};

static void run(uint64_t wanted,const wchar_t *sample,const wchar_t *reference,const wchar_t *capture,
                Mode mode, const Journal &journal,VideoTrace &trace,const wchar_t *first_frame_capture,
                const wchar_t *expected_umd,bool visual_check,bool report_timings)
{
   Com<IDXGIFactory1> enumeration;
   journal.invoke("CreateDXGIFactory1",[&]{return CreateDXGIFactory1(__uuidof(IDXGIFactory1),(void **)enumeration.put());});
   UINT ordinal=0;
   for (;;++ordinal) {
      Com<IDXGIAdapter1> adapter;
      require(enumeration->EnumAdapters1(ordinal,adapter.put()),"Find native adapter");
      DXGI_ADAPTER_DESC1 d={};require(adapter->GetDesc1(&d),"Describe adapter");
      if (!(d.Flags&DXGI_ADAPTER_FLAG_SOFTWARE) && luid_value(d.AdapterLuid)==wanted) break;
   }
   Av<AVBufferRef,av_buffer_unref> device;
   const auto device_index=std::to_string(ordinal);
   av_check(journal.av_call("av_hwdevice_ctx_create_D3D11VA",[&]{
      return av_hwdevice_ctx_create(&device.p,AV_HWDEVICE_TYPE_D3D11VA,device_index.c_str(),nullptr,0);
   }),"Create D3D11VA device");
   auto *hw=static_cast<AVD3D11VADeviceContext *>(reinterpret_cast<AVHWDeviceContext *>(device->data)->hwctx);
   require_loaded_umd(expected_umd,journal);
   Com<IDXGIDevice> dxgi;Com<IDXGIAdapter> adapter;DXGI_ADAPTER_DESC actual={};
   require(hw->device->QueryInterface(__uuidof(IDXGIDevice),(void **)dxgi.put()),"Query decode DXGI device");
   require(dxgi->GetAdapter(adapter.put()),"Get decode adapter");
   require(adapter->GetDesc(&actual),"Get decode adapter LUID");
   check(luid_value(actual.AdapterLuid)==wanted,"decoder adapter mismatch");
   std::printf("DECODER_ADAPTER luid=%016llx ordinal=%u description=%ls\n",(unsigned long long)wanted,ordinal,actual.Description);
   check(hw->lock && hw->unlock && hw->device_context,"D3D11VA context lock");
   std::unique_ptr<Preview> preview;
   if (mode!=Mode::DecodeOnly) {
      // FFmpeg opens the adapter by ordinal through its own DXGI factory.
      Com<IDXGIFactory2> device_factory;
      journal.invoke("GetDecodeAdapterFactory",[&]{
         return adapter->GetParent(__uuidof(IDXGIFactory2),(void **)device_factory.put());
      });
      if (first_frame_capture || mode==Mode::PrepareOnly) {
         Com<IUnknown> enumerator_id, device_factory_id;
         require(enumeration->QueryInterface(IID_IUnknown,(void **)enumerator_id.put()),"Identify enumeration factory");
         require(device_factory->QueryInterface(IID_IUnknown,(void **)device_factory_id.put()),"Identify device factory");
         journal.mark(enumerator_id.p==device_factory_id.p?"SAME":"DISTINCT","DXGIFactories");
      }
      preview=std::make_unique<Preview>(journal,trace,first_frame_capture,visual_check);
      preview->start(device_factory.p,hw->device);
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
   check(codec && params->codec_id==AV_CODEC_ID_H264 && params->width==sample_width && params->height==sample_height,
         "H.264 input must match the selected fixture dimensions");
   std::printf("INPUT visible=%dx%d expected_texture=%dx%d\n",sample_width,sample_height,sample_width,coded_height);
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
   DecodeTimings timing(report_timings);
   const uint64_t loop_start=timing.now();
   auto receive=[&](bool flushing) {
      for (;;) {
         const bool after_last=frames==6;
         if (after_last) journal.mark("CALL","ReceiveAfterLastFrame");
         const uint64_t receive_start=timing.now();
         int ret=avcodec_receive_frame(decoder.p,frame.p);
         timing.receive+=timing.now()-receive_start;
         if (after_last) journal.mark("RETURN","ReceiveAfterLastFrame",static_cast<HRESULT>(ret));
         if (ret==AVERROR_EOF) return;
         if (ret==AVERROR(EAGAIN)) {check(!flushing,"unexpected EAGAIN while draining");return;}
         av_check(ret,"Receive decoded frame");
         check(frames<6 && frame->format==AV_PIX_FMT_D3D11 && frame->hw_frames_ctx && frame->data[0],
               "native D3D11 frame required");
         auto *tex=reinterpret_cast<ID3D11Texture2D *>(frame->data[0]);Com<ID3D11Device> owner;tex->GetDevice(owner.put());
         D3D11_TEXTURE2D_DESC desc={};tex->GetDesc(&desc);
         const auto slice=reinterpret_cast<uintptr_t>(frame->data[1]);
         check(owner.p==hw->device && desc.Format==DXGI_FORMAT_NV12 && desc.Width==sample_width && desc.Height==coded_height &&
               (desc.BindFlags&D3D11_BIND_DECODER) && slice<desc.ArraySize,"decode texture ownership");
         check(frame->pts!=AV_NOPTS_VALUE && (last==AV_NOPTS_VALUE || frame->pts>last),"display frame order");last=frame->pts;
         auto &sample_time=timing.frames[frames];
         const uint64_t read_start=timing.now();
         av_check(av_hwframe_transfer_data(cpu.p,frame.p,0),"Read native NV12 frame");
         const uint64_t verify_start=timing.now();
         sample_time.readback=verify_start-read_start;
         check_pixels(*cpu.p,hashes.data()+frames*32,frames);
         sample_time.verify=timing.now()-verify_start;
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
         if (preview) {
            const uint64_t draw_start=timing.now();
            preview->draw(hw,cpu->data[0],cpu->linesize[0],cpu->data[1],cpu->linesize[1],frames,
                          frames==5 ? capture : nullptr);
            sample_time.preview=timing.now()-draw_start;
         }
         ++frames;
         if (frames==6) journal.mark("CALL","ReleaseLastFrameRefs");
         av_frame_unref(cpu.p);av_frame_unref(frame.p);
         if (frames==6) journal.mark("RETURN","ReleaseLastFrameRefs");
      }
   };
   for (;;) {
      int ret=av_read_frame(input.p,packet.p);
      if (ret==AVERROR_EOF) break;
      av_check(ret,"Read packet");
      if (packet->stream_index==stream) {
         const uint64_t send_start=timing.now();
         av_check(avcodec_send_packet(decoder.p,packet.p),"Submit packet");
         timing.submit+=timing.now()-send_start;
         receive(false);
      }
      av_packet_unref(packet.p);
   }
   av_check(journal.av_call("DrainDecoder",[&]{
      const uint64_t drain_start=timing.now();
      const int ret=avcodec_send_packet(decoder.p,nullptr);
      timing.submit+=timing.now()-drain_start;
      return ret;
   }),"Drain decoder");
   receive(true);
   const uint64_t loop_ticks=timing.now()-loop_start;
   check(frames==6,"incomplete decoded sequence");
   journal.mark("COMPLETE","DecodedSequence");
   // After drain, every decoder surface has been read back. The shader uses
   // separate uploaded textures. The retained AVHWDeviceContext and preview
   // own the graphics resources through the GPU event and compositor flush.
   journal.mark("CALL","CloseDecoder");
   avcodec_free_context(&decoder.p);
   journal.mark("RETURN","CloseDecoder");
   if (preview) {
      journal.invoke("DeviceAfterDecoderClose",[&]{return hw->device->GetDeviceRemovedReason();});
      preview->finish(hw);
      journal.mark("CALL","ReleasePreview");
      preview.reset();
      journal.mark("RETURN","ReleasePreview");
   }
   journal.invoke("FinalDeviceStatus",[&]{return hw->device->GetDeviceRemovedReason();});
   timing.print(loop_ticks);
   std::printf("DECODED_FRAMES=%u WINDOW_FRAMES=%u elapsed_ms=%llu\n",frames,
               mode==Mode::Present?frames:0,GetTickCount64()-start);
}

int wmain(int argc,wchar_t **argv)
{
   SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
   const bool report_timings=argc>=2 && std::wcscmp(argv[argc-1],L"--timings")==0;
   if (report_timings) --argc;
   const wchar_t *expected_umd=nullptr;
   if (argc>=3 && std::wcscmp(argv[argc-2],L"--require-umd")==0) {
      expected_umd=argv[argc-1];
      if (std::wcslen(expected_umd)!=64 || std::wcsspn(expected_umd,L"0123456789abcdefABCDEF")!=64) return 2;
      argc-=2;
   }
   const bool visual_check=argc>=2 && std::wcscmp(argv[argc-1],L"--visual-check")==0;
   if (visual_check) --argc;
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
      std::fprintf(stderr,"usage: nvdec720-av-window <native-luid-hex> <sample.mp4> <reference.sha256> [last-frame.ppm | --prepare-only | --decode-only] [--evidence new-directory [--check-first-submit] [--trace]] [--visual-check] [--require-umd sha256] [--timings]\n");
      return 2;
   }
   Mode mode=Mode::Present;
   if (argc==5 && std::wcscmp(argv[4],L"--prepare-only")==0) mode=Mode::PrepareOnly;
   if (argc==5 && std::wcscmp(argv[4],L"--decode-only")==0) mode=Mode::DecodeOnly;
   if ((check_first || visual_check) && mode!=Mode::Present) return 2;
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
         }
         if ((evidence || report_timings) && mode!=Mode::DecodeOnly)
            check(SetEnvironmentVariableW(L"TGD3D_VIDEO_WINDOW_VALIDATE",L"1"),"request same-device window submission");
         std::setvbuf(stdout,nullptr,_IONBF,0);
         std::setvbuf(stderr,nullptr,_IONBF,0);
         DWORD session=0;
         check(ProcessIdToSessionId(GetCurrentProcessId(),&session),"query interactive session");
         std::printf("START pid=%lu session=%lu\n",GetCurrentProcessId(),session);
         if (visual_check) std::puts("VISUAL_CHECK inter_frame_ms=1000 final_frame_ms=8000");
         VideoTrace trace;
         if (trace_requested) {
            journal.mark("CALL","TraceCurrentProcess");
            trace.start(evidence,mode==Mode::Present);
            journal.mark("RETURN","TraceCurrentProcess");
         }
         const std::wstring first_image=check_first?std::wstring(evidence)+L"\\first-frame.ppm":L"";
         run(wanted,argv[2],argv[3],argc==5 && mode==Mode::Present?argv[4]:nullptr,
             mode,journal,trace,check_first?first_image.c_str():nullptr,expected_umd,visual_check,report_timings);
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
