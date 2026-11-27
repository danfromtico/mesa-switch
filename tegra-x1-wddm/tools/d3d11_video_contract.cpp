/* SPDX-License-Identifier: MIT
 * Observe the native runtime's video entry points on one exact adapter.
 * No decoder, GPU work, swapchain, software fallback or driver installation.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

static void module_path(const wchar_t *name)
{
   wchar_t path[32768];
   HMODULE module = GetModuleHandleW(name);
   DWORD n = module ? GetModuleFileNameW(module, path, ARRAYSIZE(path)) : 0;
   if (n && n < ARRAYSIZE(path)) std::printf("MODULE %ls=%ls\n", name, path);
}

int main(int argc, char **argv)
{
   std::setvbuf(stdout, nullptr, _IONBF, 0);
   std::setvbuf(stderr, nullptr, _IONBF, 0);
   SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
   if (argc < 2 || argc > 3 || (argc == 3 && std::strcmp(argv[2], "--plain"))) {
      std::fprintf(stderr, "usage: d3d11-video-contract <native-luid-hex> [--plain]\n");
      return 2;
   }
   char *end = nullptr;
   unsigned long long wanted = std::strtoull(argv[1], &end, 16);
   if (!wanted || !end || *end || std::strlen(argv[1]) != 16) return 2;
   IDXGIFactory1 *factory = nullptr;
   HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&factory);
   std::printf("CreateDXGIFactory1=0x%08lx\n", (unsigned long)hr);
   if (FAILED(hr)) return 3;
   IDXGIAdapter1 *selected = nullptr;
   for (UINT i = 0;; i++) {
      IDXGIAdapter1 *adapter = nullptr;
      hr = factory->EnumAdapters1(i, &adapter);
      if (hr == DXGI_ERROR_NOT_FOUND) break;
      if (FAILED(hr)) { factory->Release(); return 3; }
      DXGI_ADAPTER_DESC1 desc = {};
      hr = adapter->GetDesc1(&desc);
      unsigned long long luid = (unsigned long long)(UINT32)desc.AdapterLuid.HighPart << 32 |
                                 desc.AdapterLuid.LowPart;
      std::printf("ENUM[%u] hr=0x%08lx luid=%016llx vendor=%04x device=%04x flags=0x%x desc=%ls\n",
                  i, (unsigned long)hr, luid, desc.VendorId, desc.DeviceId, desc.Flags, desc.Description);
      /* ACPI\SWOA0F01 is not a PCI device: DXGI reports its ACPI IDs here.
       * The caller supplies the LUID already verified by the native KMT
       * adapter-info contract; a fabricated PCI vendor filter hides it. */
      if (SUCCEEDED(hr) && luid == wanted &&
          !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
         selected = adapter;
         std::printf("ADAPTER luid=%016llx vendor=%04x device=%04x desc=%ls\n",
                     luid, desc.VendorId, desc.DeviceId, desc.Description);
         break;
      }
      adapter->Release();
   }
   factory->Release();
   if (!selected) { std::fprintf(stderr, "Requested native adapter absent\n"); return 3; }
   UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
   if (argc != 3) flags |= D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
   const D3D_FEATURE_LEVEL requested = D3D_FEATURE_LEVEL_10_0;
   D3D_FEATURE_LEVEL actual = (D3D_FEATURE_LEVEL)0;
   ID3D11Device *device = nullptr;
   ID3D11DeviceContext *context = nullptr;
   std::printf("CALL D3D11CreateDevice flags=0x%x feature_level=0x%x\n", flags, requested);
   hr = D3D11CreateDevice(selected, D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags,
                         &requested, 1, D3D11_SDK_VERSION, &device, &actual, &context);
   selected->Release();
   std::printf("RETURN D3D11CreateDevice=0x%08lx actual_level=0x%x\n", (unsigned long)hr, actual);
   for (auto name : {L"d3d11.dll", L"dxgi.dll", L"TegraGpuD3DVk.dll", L"libvulkan_nouveau.dll"})
      module_path(name);
   if (FAILED(hr)) return 4;
   UINT format = 0;
   hr = device->CheckFormatSupport(DXGI_FORMAT_NV12, &format);
   std::printf("CheckFormatSupport(NV12)=0x%08lx mask=0x%x\n", (unsigned long)hr, format);
   ID3D11VideoDevice *video = nullptr;
   hr = device->QueryInterface(__uuidof(ID3D11VideoDevice), (void **)&video);
   std::printf("QueryInterface(ID3D11VideoDevice)=0x%08lx\n", (unsigned long)hr);
   ID3D11VideoContext *videoContext = nullptr;
   HRESULT chr = context->QueryInterface(__uuidof(ID3D11VideoContext), (void **)&videoContext);
   std::printf("QueryInterface(ID3D11VideoContext)=0x%08lx\n", (unsigned long)chr);
   bool found = false;
   if (video) {
      UINT count = video->GetVideoDecoderProfileCount();
      std::printf("GetVideoDecoderProfileCount=%u\n", count);
      for (UINT i = 0; i < count; i++) {
         GUID profile = {};
         hr = video->GetVideoDecoderProfile(i, &profile);
         if (FAILED(hr)) { std::printf("GetVideoDecoderProfile[%u]=0x%08lx\n", i, (unsigned long)hr); break; }
         if (IsEqualGUID(profile, D3D11_DECODER_PROFILE_H264_VLD_NOFGT)) {
            BOOL nv12 = FALSE;
            hr = video->CheckVideoDecoderFormat(&profile, DXGI_FORMAT_NV12, &nv12);
            std::printf("CheckVideoDecoderFormat(H264,NV12)=0x%08lx supported=%d\n", (unsigned long)hr, nv12);
            found = SUCCEEDED(hr) && nv12;
         }
      }
      video->Release();
   }
   if (videoContext) videoContext->Release();
   hr = device->GetDeviceRemovedReason();
   std::printf("GetDeviceRemovedReason=0x%08lx\n", (unsigned long)hr);
   context->Release();
   std::printf("DeviceRelease=%lu\n", (unsigned long)device->Release());
   std::printf("FRAME_DECODE=not-attempted; VIDEO_PRESENT=not-attempted\n");
   return found && SUCCEEDED(hr) ? 0 : 4;
}
