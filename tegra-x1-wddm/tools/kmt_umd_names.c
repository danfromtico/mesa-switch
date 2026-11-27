/* SPDX-License-Identifier: MIT */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winddk_compat.h>
#include <d3dkmthk.h>
#include <stdio.h>
#include "kmt.h"

int main(void)
{
    struct tg_kmt_scan scan = {0};
    LONG status = tg_gpu_kmt_scan_system(&scan);
    USHORT guest = 0, native = 0;
    if (!IsWow64Process2(GetCurrentProcess(), &guest, &native))
        return 2;
    printf("identity\t%lu\t%u\t%04x\t%04x\t%08lx\t%u\t%08x%08x\n",
           GetCurrentProcessId(), (unsigned)(sizeof(void *) * 8), guest, native, (ULONG)status,
           scan.native_ready, (unsigned)scan.first_ready.luid_high, scan.first_ready.luid_low);
    if (status < 0 || scan.native_ready != 1) return 3;
    HMODULE gdi = LoadLibraryExW(L"gdi32.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!gdi) return 4;
    PFND3DKMT_OPENADAPTERFROMLUID open = (PFND3DKMT_OPENADAPTERFROMLUID)(void*)GetProcAddress(gdi,"D3DKMTOpenAdapterFromLuid");
    PFND3DKMT_QUERYADAPTERINFO query = (PFND3DKMT_QUERYADAPTERINFO)(void*)GetProcAddress(gdi,"D3DKMTQueryAdapterInfo");
    PFND3DKMT_CLOSEADAPTER close_adapter = (PFND3DKMT_CLOSEADAPTER)(void*)GetProcAddress(gdi,"D3DKMTCloseAdapter");
    if (!open || !query || !close_adapter) return 4;
    D3DKMT_OPENADAPTERFROMLUID req = {0};
    req.AdapterLuid.LowPart = scan.first_ready.luid_low;
    req.AdapterLuid.HighPart = scan.first_ready.luid_high;
    status = open(&req);
    printf("open\t%08lx\n", (ULONG)status);
    if (status < 0) return 5;
    D3DKMT_ADAPTERTYPE type = {0};
    D3DKMT_QUERYADAPTERINFO a = {0};
    a.hAdapter = req.hAdapter;
    a.Type = KMTQAITYPE_ADAPTERTYPE; a.pPrivateDriverData = &type; a.PrivateDriverDataSize = sizeof(type);
    status = query(&a);
    printf("type\t%08lx\t%08x\t%u\t%u\t%u\n", (ULONG)status, type.Value,
           type.RenderSupported, type.DisplaySupported, type.SoftwareDevice);
    D3DKMT_DRIVERVERSION version = 0;
    a.Type = KMTQAITYPE_DRIVERVERSION; a.pPrivateDriverData = &version; a.PrivateDriverDataSize = sizeof(version);
    status = query(&a);
    printf("wddm\t%08lx\t%u\n", (ULONG)status, (unsigned)version);
    for (UINT slot = KMTUMDVERSION_DX9; slot <= KMTUMDVERSION_DX12; ++slot) {
        D3DKMT_UMDFILENAMEINFO info = {0};
        info.Version = (KMTUMDVERSION)slot;
        a.Type = KMTQAITYPE_UMDRIVERNAME; a.pPrivateDriverData = &info; a.PrivateDriverDataSize = sizeof(info);
        status = query(&a);
        info.UmdFileName[MAX_PATH - 1] = 0;
        char path[4*MAX_PATH] = {0};
        if (status >= 0) WideCharToMultiByte(CP_UTF8,0,info.UmdFileName,-1,path,sizeof(path),NULL,NULL);
        printf("slot\t%u\t%08lx\t%s\n", slot, (ULONG)status, path);
    }
    D3DKMT_CLOSEADAPTER done = {0};
    done.hAdapter = req.hAdapter;
    status = close_adapter(&done);
    printf("close\t%08lx\n", (ULONG)status);
    FreeLibrary(gdi);
    return status < 0 ? 6 : 0;
}
