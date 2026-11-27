/* SPDX-License-Identifier: MIT */
#pragma once
#include <dxgi1_2.h>
#include <wrl/client.h>
extern "C" {
#include "kmt.h"
}

struct Gm20bAdapter {
   Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
   Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
   tg_gpu_adapter_info gpu{};
   DXGI_ADAPTER_DESC1 identity{};

   bool matches(const DXGI_ADAPTER_DESC1 &desc) const {
      // DXGI identifies this ACPI adapter with its ACPI hardware ID. KMT
      // reports the silicon vendor and chipset for the same adapter LUID.
      return desc.AdapterLuid.LowPart == gpu.luid_low && desc.AdapterLuid.HighPart == gpu.luid_high &&
             !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE);
   }
   HRESULT open() {
      tg_kmt_scan scan{};
      const LONG status = tg_gpu_kmt_scan_system(&scan);
      if (status < 0) return HRESULT_FROM_NT(status);
      gpu = scan.first_ready;
      if (scan.native_ready != 1 || gpu.vendor != 0x10de || gpu.chipset != 0x12e || gpu.sm != 53) return DXGI_ERROR_NOT_FOUND;
      HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
      if (FAILED(hr)) return hr;
      for (UINT i = 0; ; ++i) {
         Microsoft::WRL::ComPtr<IDXGIAdapter1> candidate;
         hr = factory->EnumAdapters1(i,&candidate);
         if (FAILED(hr)) return hr;
         hr = candidate->GetDesc1(&identity);
         if (FAILED(hr)) return hr;
         if (matches(identity)) { adapter = candidate; return S_OK; }
      }
   }
};
