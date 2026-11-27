/* SPDX-License-Identifier: MIT
 * Call the D3D9 adapter DDI with the real GM20B KMT query callback. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winddk_compat.h>
#include <d3d9.h>
#include <d3dumddi.h>
#include <d3dkmthk.h>
#include <stdio.h>
#include "kmt.h"

struct runtime_adapter {
   D3DKMT_HANDLE handle;
   PFND3DKMT_QUERYADAPTERINFO query;
   unsigned callbacks;
};

static HRESULT APIENTRY private_info(HANDLE adapter, const D3DDDICB_QUERYADAPTERINFO *in)
{
   struct runtime_adapter *rt = adapter;
   if (!rt || !in) return E_INVALIDARG;
   D3DKMT_QUERYADAPTERINFO req = {0};
   req.hAdapter = rt->handle;
   req.Type = KMTQAITYPE_UMDRIVERPRIVATE;
   req.pPrivateDriverData = in->pPrivateDriverData;
   req.PrivateDriverDataSize = in->PrivateDriverDataSize;
   const LONG result = rt->query(&req);
   rt->callbacks++;
   printf("private\t%u\t%08lx\n",req.PrivateDriverDataSize,(ULONG)result);
   return result < 0 ? HRESULT_FROM_NT(result) : S_OK;
}

int wmain(int argc, wchar_t **argv)
{
   if (argc!=2) return 2;
   setvbuf(stdout,NULL,_IONBF,0);
   SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
   struct tg_kmt_scan scan = {0};
   if (tg_gpu_kmt_scan_system(&scan)<0 || scan.native_ready!=1) return 3;
   HMODULE gdi=LoadLibraryExW(L"gdi32.dll",NULL,LOAD_LIBRARY_SEARCH_SYSTEM32);
   if (!gdi) return 3;
   PFND3DKMT_OPENADAPTERFROMLUID open=(PFND3DKMT_OPENADAPTERFROMLUID)(void*)GetProcAddress(gdi,"D3DKMTOpenAdapterFromLuid");
   PFND3DKMT_QUERYADAPTERINFO query=(PFND3DKMT_QUERYADAPTERINFO)(void*)GetProcAddress(gdi,"D3DKMTQueryAdapterInfo");
   PFND3DKMT_CLOSEADAPTER close_kmt=(PFND3DKMT_CLOSEADAPTER)(void*)GetProcAddress(gdi,"D3DKMTCloseAdapter");
   if (!open || !query || !close_kmt) return 3;
   D3DKMT_OPENADAPTERFROMLUID kmt={0};
   kmt.AdapterLuid.LowPart=scan.first_ready.luid_low;
   kmt.AdapterLuid.HighPart=scan.first_ready.luid_high;
   if(open(&kmt)<0) return 3;
   printf("kmt\t%08x%08x\t%x\t%x\t%u\n",(unsigned)kmt.AdapterLuid.HighPart,
          (unsigned)kmt.AdapterLuid.LowPart,scan.first_ready.vendor,scan.first_ready.chipset,scan.first_ready.sm);
   int code=4;
   HMODULE umd=LoadLibraryExW(argv[1],NULL,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
   if(umd){
      PFND3DDDI_OPENADAPTER open9=(PFND3DDDI_OPENADAPTER)(void*)GetProcAddress(umd,"OpenAdapter");
      if(open9){
         struct runtime_adapter rt={kmt.hAdapter,query,0};
         D3DDDI_ADAPTERCALLBACKS cb={0};cb.pfnQueryAdapterInfoCb=private_info;
         D3DDDI_ADAPTERFUNCS funcs={0};
         D3DDDIARG_OPENADAPTER args={0};args.hAdapter=&rt;args.Interface=9;
         args.Version=D3D_UMD_INTERFACE_VERSION_WDDM2_0;args.pAdapterCallbacks=&cb;args.pAdapterFuncs=&funcs;
         const HRESULT result=open9(&args);
         printf("open9\t%08lx\t%u\t%u\n",(ULONG)result,args.DriverVersion,rt.callbacks);
         if(SUCCEEDED(result) && args.hAdapter && args.hAdapter!=&rt &&
            funcs.pfnGetCaps && funcs.pfnCreateDevice && funcs.pfnCloseAdapter){
            UINT count=0; D3DDDIARG_GETCAPS caps={0};
            caps.Type=D3DDDICAPS_GETFORMATCOUNT;caps.pData=&count;caps.DataSize=sizeof(count);
            HRESULT a=funcs.pfnGetCaps(args.hAdapter,&caps);
            FORMATOP format={0};caps.Type=D3DDDICAPS_GETFORMATDATA;caps.pData=&format;caps.DataSize=sizeof(format);
            HRESULT b=funcs.pfnGetCaps(args.hAdapter,&caps);
            D3DCAPS9 flags={0};caps.Type=D3DDDICAPS_GETD3D9CAPS;caps.pData=&flags;caps.DataSize=sizeof(flags);
            HRESULT c=funcs.pfnGetCaps(args.hAdapter,&caps);
            wchar_t path[1024]={0};
            HMODULE nvk=GetModuleHandleW(L"libvulkan_nouveau.dll");
            const DWORD n=nvk?GetModuleFileNameW(nvk,path,1024):0;
            printf("format\t%08lx\t%u\t%08lx\t%u\t%08x\n",(ULONG)a,count,(ULONG)b,(unsigned)format.Format,format.Operations);
            printf("caps9\t%08lx\nicd\t%ls\n",(ULONG)c,path);
            HRESULT released=funcs.pfnCloseAdapter(args.hAdapter);
            printf("close9\t%08lx\n",(ULONG)released);
            code=SUCCEEDED(a)&&count==1&&SUCCEEDED(b)&&format.Format==D3DDDIFMT_X8R8G8B8&&
                 format.Operations==FORMATOP_DISPLAYMODE&&c==D3DERR_NOTAVAILABLE&&
                 n>0&&rt.callbacks==1&&SUCCEEDED(released)?0:6;
         } else code=5;
      }
      FreeLibrary(umd);
   }else printf("load_error\t%lu\n",GetLastError());
   D3DKMT_CLOSEADAPTER done={0};done.hAdapter=kmt.hAdapter;
   LONG end=close_kmt(&done);FreeLibrary(gdi);
   printf("close_kmt\t%08lx\n",(ULONG)end);
   return end<0?7:code;
}
