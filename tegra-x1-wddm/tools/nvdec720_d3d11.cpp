/* SPDX-License-Identifier: MIT
 * Standard Media Foundation -> D3D11VA -> NV12 readback acceptance.
 * Hardware use must additionally be proven by this process's video-DDI
 * submission/completion trace. A DXGI surface alone is not that proof.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3d10.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <new>
#include <vector>
#include "../../src/nouveau/nvdec/nvdec.h"

#include "nvdec720_fixture.hpp"

static unsigned decode(unsigned long long wanted, const wchar_t *path, const wchar_t *reference, const wchar_t *dump)
{
   auto hashes = references(reference);
   Com<IDXGIFactory1> factory;
   require(CreateDXGIFactory1(__uuidof(IDXGIFactory1),(void **)factory.put()),"CreateDXGIFactory1");
   Com<IDXGIAdapter1> adapter;
   for (UINT i=0;;++i) {
      HRESULT hr=factory->EnumAdapters1(i,adapter.put());
      if (hr==DXGI_ERROR_NOT_FOUND) break;
      require(hr,"EnumAdapters1");
      DXGI_ADAPTER_DESC1 d={};require(adapter->GetDesc1(&d),"GetDesc1");
      unsigned long long luid=(unsigned long long)(UINT32)d.AdapterLuid.HighPart<<32 | d.AdapterLuid.LowPart;
      if (luid==wanted && !(d.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)) {
         std::printf("ADAPTER luid=%016llx vendor=%08x device=%08x desc=%ls\n",luid,d.VendorId,d.DeviceId,d.Description);
         break;
      }
   }
   check(adapter.p!=nullptr,"native adapter absent");
   Com<ID3D11Device> device;Com<ID3D11DeviceContext> context;
   const D3D_FEATURE_LEVEL requested=D3D_FEATURE_LEVEL_10_0;
   D3D_FEATURE_LEVEL actual=(D3D_FEATURE_LEVEL)0;
   std::puts("CALL D3D11CreateDevice(video)");
   require(D3D11CreateDevice(adapter.p,D3D_DRIVER_TYPE_UNKNOWN,nullptr,
       D3D11_CREATE_DEVICE_VIDEO_SUPPORT|D3D11_CREATE_DEVICE_BGRA_SUPPORT,
       &requested,1,D3D11_SDK_VERSION,device.put(),&actual,context.put()),"D3D11CreateDevice(video)");
   Com<ID3D11VideoDevice> video;
   require(device->QueryInterface(__uuidof(ID3D11VideoDevice),(void **)video.put()),"ID3D11VideoDevice");
   std::printf("VIDEO_PROFILES=%u feature_level=0x%x\n",video->GetVideoDecoderProfileCount(),actual);
   Com<ID3D10Multithread> mt;
   require(device->QueryInterface(__uuidof(ID3D10Multithread),(void **)mt.put()),"ID3D10Multithread");
   mt->SetMultithreadProtected(TRUE);
   Com<IMFDXGIDeviceManager> manager;UINT token=0;
   require(MFCreateDXGIDeviceManager(&token,manager.put()),"MFCreateDXGIDeviceManager");
   require(manager->ResetDevice(device.p,token),"ResetDevice");
   Com<IMFAttributes> attributes;
   require(MFCreateAttributes(attributes.put(),8),"MFCreateAttributes");
   require(attributes->SetUnknown(MF_SOURCE_READER_D3D_MANAGER,manager.p),"Set D3D manager");
   require(attributes->SetUINT32(MF_READWRITE_D3D_OPTIONAL,FALSE),"Require D3D surfaces");
   require(attributes->SetUINT32(MF_SOURCE_READER_DISABLE_DXVA,FALSE),"Enable DXVA");
   require(attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS,TRUE),"Enable hardware transforms");
   require(attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING,FALSE),"Disable source-reader color conversion");
   Com<IMFSourceReader> reader;
   std::puts("CALL MFCreateSourceReaderFromURL");
   require(MFCreateSourceReaderFromURL(path,attributes.p,reader.put()),"MFCreateSourceReaderFromURL");
   require(reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS,FALSE),"Deselect streams");
   require(reader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM,TRUE),"Select video stream");
   Com<IMFMediaType> native;
   require(reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,0,native.put()),"GetNativeMediaType");
   GUID subtype={};require(native->GetGUID(MF_MT_SUBTYPE,&subtype),"Get input subtype");
   check(IsEqualGUID(subtype,MFVideoFormat_H264),"input is not H264");
   UINT width=0,height=0;
   require(MFGetAttributeSize(native.p,MF_MT_FRAME_SIZE,&width,&height),"Get input dimensions");
   check(width==1280 && height==720,"input is not720p");
   Com<IMFMediaType> output;
   require(MFCreateMediaType(output.put()),"MFCreateMediaType");
   require(output->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Video),"Set output major");
   require(output->SetGUID(MF_MT_SUBTYPE,MFVideoFormat_NV12),"Set output NV12");
   std::puts("CALL SetCurrentMediaType(NV12)");
   require(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,nullptr,output.p),"SetCurrentMediaType(NV12)");
   D3D11_TEXTURE2D_DESC readback={};readback.Width=1280;readback.Height=720;readback.MipLevels=1;
   readback.ArraySize=1;readback.Format=DXGI_FORMAT_NV12;readback.SampleDesc.Count=1;
   readback.Usage=D3D11_USAGE_STAGING;readback.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
   Com<ID3D11Texture2D> staging;
   require(device->CreateTexture2D(&readback,nullptr,staging.put()),"Create NV12 readback");
   unsigned frames=0;bool eof=false;
   const ULONGLONG start=GetTickCount64();
   for (unsigned attempt=0;attempt<80 && GetTickCount64()-start<30000;++attempt) {
      Com<IMFSample> sample;DWORD stream=0,flags=0;LONGLONG time=0;
      std::printf("CALL ReadSample index=%u\n",frames);
      require(reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM,0,&stream,&flags,&time,sample.put()),"ReadSample");
      std::printf("SAMPLE time=%lld flags=0x%08lx present=%d\n",time,(unsigned long)flags,sample.p!=nullptr);
      check(!(flags&MF_SOURCE_READERF_ERROR),"source-reader error flag");
      if (sample.p) {
         check(frames<hashes.size()/32,"unexpected extra frame");
         Com<IMFMediaBuffer> buffer;require(sample->GetBufferByIndex(0,buffer.put()),"GetBufferByIndex");
         Com<IMFDXGIBuffer> dxgi;
         require(buffer->QueryInterface(__uuidof(IMFDXGIBuffer),(void **)dxgi.put()),"IMFDXGIBuffer required");
         Com<ID3D11Texture2D> texture;require(dxgi->GetResource(__uuidof(ID3D11Texture2D),(void **)texture.put()),"Get decoder texture");
         UINT subresource=0;require(dxgi->GetSubresourceIndex(&subresource),"GetSubresourceIndex");
         Com<ID3D11Device> owner;texture->GetDevice(owner.put());check(owner.p==device.p,"foreign decoder device");
         D3D11_TEXTURE2D_DESC d={};texture->GetDesc(&d);
         check(d.Width==1280 && d.Height==720 && d.Format==DXGI_FORMAT_NV12 && d.MipLevels==1 &&
               subresource<d.ArraySize && d.SampleDesc.Count==1,"decoder texture layout");
         context->CopySubresourceRegion(staging.p,0,0,0,0,texture.p,subresource,nullptr);
         require(device->GetDeviceRemovedReason(),"Copy device status");
         D3D11_MAPPED_SUBRESOURCE mapped={};
         require(context->Map(staging.p,0,D3D11_MAP_READ,0,&mapped),"Map NV12 readback");
         Mapping mapping{context.p,staging.p};
         check(mapped.pData && mapped.RowPitch>=1280,"NV12 map layout");
         std::vector<uint8_t> i420(1382400);
         const auto *bytes=static_cast<const uint8_t *>(mapped.pData);
         for (UINT y=0;y<720;++y) std::memcpy(i420.data()+y*1280,bytes+(size_t)y*mapped.RowPitch,1280);
         for (UINT y=0;y<360;++y) for (UINT x=0;x<640;++x) {
            const uint8_t *uv=bytes+(size_t)(720+y)*mapped.RowPitch+x*2;
            i420[921600+y*640+x]=uv[0];i420[1152000+y*640+x]=uv[1];
         }
         if (dump) {
            wchar_t name[32768];int n=std::swprintf(name,ARRAYSIZE(name),L"%ls\\frame-%02u.nv12",dump,frames);
            FILE *f=n>0 ? _wfopen(name,L"wb") : nullptr;
            check(f!=nullptr,"open frame dump");
            bool ok=true;
            for (UINT y=0;y<1080;++y) if(std::fwrite(bytes+(size_t)y*mapped.RowPitch,1,1280,f)!=1280) ok=false;
            ok=std::fclose(f)==0 && ok;
            check(ok,"write frame dump");
         }
         mapping.finish();
         uint8_t hash[32];tg_nvdec_sha256(i420.data(),i420.size(),hash);
         std::printf("FRAME index=%u subresource=%u pitch=%u sha256=",frames,subresource,mapped.RowPitch);
         for (uint8_t x:hash) std::printf("%02x",x);std::putchar('\n');
         check(!std::memcmp(hash,hashes.data()+frames*32,32),"decoded pixels differ");
         ++frames;
      }
      if (flags&MF_SOURCE_READERF_ENDOFSTREAM) { eof=true;break; }
   }
   check(eof && frames==6,"incomplete frame sequence");
   require(device->GetDeviceRemovedReason(),"Final device status");
   reader.reset();staging.reset();native.reset();output.reset();attributes.reset();manager.reset();mt.reset();video.reset();context.reset();
   ULONG refs=device.p->Release();device.p=nullptr;
   std::printf("DEVICE_RELEASE=%lu frames=%u elapsed_ms=%llu\n",(unsigned long)refs,frames,GetTickCount64()-start);
   std::puts("D3D11_NV12_PIXELS=PASS; NVDEC_USE=requires matching native video-DDI trace; WINDOW_PRESENT=not-tested");
   return frames;
}

int wmain(int argc,wchar_t **argv)
{
   std::setvbuf(stdout,nullptr,_IONBF,0);std::setvbuf(stderr,nullptr,_IONBF,0);
   SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
   if (argc!=4 && argc!=5) { std::fprintf(stderr,"usage: nvdec720-d3d11 <native-luid-hex> <sample.mp4> <reference.sha256> [dump-dir]\n");return 2; }
   wchar_t *end=nullptr;unsigned long long luid=std::wcstoull(argv[1],&end,16);
   if (!luid || !end || *end || std::wcslen(argv[1])!=16) return 2;
   HRESULT co=CoInitializeEx(nullptr,COINIT_MULTITHREADED);if (FAILED(co)) return 3;
   HRESULT mf=MFStartup(MF_VERSION);if (FAILED(mf)) { CoUninitialize();return 3; }
   int result=0;
   try { decode(luid,argv[2],argv[3],argc==5?argv[4]:nullptr); }
   catch (const Failure &f) { std::fprintf(stderr,"FAIL %s=0x%08lx\n",f.call,(unsigned long)f.result);result=4; }
   catch (const std::bad_alloc &) { std::fprintf(stderr,"FAIL allocation\n");result=5; }
   MFShutdown();CoUninitialize();return result;
}
