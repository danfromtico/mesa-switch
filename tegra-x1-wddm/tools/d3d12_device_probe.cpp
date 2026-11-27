/* SPDX-License-Identifier: MIT */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdio>

extern "C" {
#include "kmt.h"
}

using Microsoft::WRL::ComPtr;

static unsigned long long luid_number(LUID id)
{
   return (static_cast<unsigned long long>(static_cast<UINT32>(id.HighPart)) << 32) | id.LowPart;
}

static bool probe(PFN_D3D12_CREATE_DEVICE create, IDXGIAdapter1 *adapter,
                  const char *name, D3D_FEATURE_LEVEL minimum, bool software)
{
   DXGI_ADAPTER_DESC1 desc{};
   if (FAILED(adapter->GetDesc1(&desc)) || !!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != software)
      return false;
   std::printf("request\t%s\t%04x\t%016llx\t%u\n", name, static_cast<unsigned>(minimum),
               luid_number(desc.AdapterLuid), static_cast<unsigned>(desc.Flags));
   ComPtr<ID3D12Device> device;
   HRESULT hr = create(adapter, minimum, IID_PPV_ARGS(device.GetAddressOf()));
   auto actual = device ? luid_number(device->GetAdapterLuid()) : 0ULL;
   std::printf("device\t%s\t%04x\t%08lx\t%u\t%016llx\n", name,
               static_cast<unsigned>(minimum), static_cast<ULONG>(hr), device ? 1U : 0U, actual);
   if (FAILED(hr))
      return !device;
   return device && actual == luid_number(desc.AdapterLuid);
}

int main()
{
   std::setvbuf(stdout, nullptr, _IONBF, 0);
   SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
   HMODULE runtime = LoadLibraryExW(L"d3d12.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
   if (!runtime) return 2;
   auto create = reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(GetProcAddress(runtime, "D3D12CreateDevice"));
   char path[MAX_PATH]{};
   if (!create || !GetModuleFileNameA(runtime, path, ARRAYSIZE(path))) return 2;
   std::printf("runtime\t%lu\t%s\n", GetCurrentProcessId(), path);

   tg_kmt_scan scan{};
   LONG status = tg_gpu_kmt_scan_system(&scan);
   const auto &info = scan.first_ready;
   LUID luid{info.luid_low, info.luid_high};
   std::printf("kmt\t%08lx\t%u\t%016llx\t%04x\t%03x\t%u\n", static_cast<ULONG>(status),
               scan.native_ready, luid_number(luid), info.vendor, info.chipset, info.sm);
   if (status < 0 || scan.native_ready != 1 || !luid_number(luid)) return 3;
   ComPtr<IDXGIFactory4> factory;
   if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf())))) return 4;
   ComPtr<IDXGIAdapter1> native;
   if (FAILED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(native.GetAddressOf())))) return 5;
   if (!probe(create, native.Get(), "tegra", D3D_FEATURE_LEVEL_11_0, false) ||
       !probe(create, native.Get(), "tegra", D3D_FEATURE_LEVEL_12_0, false)) return 6;
   ComPtr<IDXGIAdapter1> warp;
   HRESULT hr = factory->EnumWarpAdapter(IID_PPV_ARGS(warp.GetAddressOf()));
   std::printf("enum-warp\t%08lx\n", static_cast<ULONG>(hr));
   if (SUCCEEDED(hr) && !probe(create, warp.Get(), "warp", D3D_FEATURE_LEVEL_11_0, true)) return 7;
   std::puts("complete");
   return 0;
}
