// SPDX-License-Identifier: MIT
// Check bidirectional D3D11 texture sharing between two native devices in one or two processes.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <memory>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
#include "../../src/nouveau/nvdec/nvdec.h"
namespace
{
constexpr UINT width = 640, height = 360, patchWidth = 23, patchHeight = 13, patchX = 11, patchY = 8;
constexpr RECT dirty{43, 19, 87, 51};
struct Failure {
   const char *where;
   HRESULT status;
};
void require(HRESULT hr, const char *where)
{
   if (FAILED(hr))
      throw Failure{where, hr};
}
void check(bool value, const char *where)
{
   if (!value)
      throw Failure{where, E_FAIL};
}
unsigned long long luid(LUID v)
{
   return (static_cast<unsigned long long>(static_cast<DWORD>(v.HighPart)) << 32) | v.LowPart;
}

class Journal
{
   HANDLE file = INVALID_HANDLE_VALUE;

 public:
   explicit Journal(const std::wstring &dir)
   {
      file = CreateFileW((dir + L"\\journal.txt").c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                         CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
      check(file != INVALID_HANDLE_VALUE, "create journal");
   }
   ~Journal()
   {
      if (file != INVALID_HANDLE_VALUE)
         CloseHandle(file);
   }
   void mark(const char *phase, const char *where, HRESULT code = S_OK) const
   {
      LARGE_INTEGER t{};
      QueryPerformanceCounter(&t);
      char buf[224];
      DWORD written = 0;
      int n = std::snprintf(buf, sizeof(buf), "%lld %lu %s %s %08lx\r\n", t.QuadPart,
                            GetCurrentThreadId(), phase, where, (unsigned long)code);
      check(n > 0 && size_t(n) < sizeof(buf) && WriteFile(file, buf, n, &written, nullptr) &&
                written == DWORD(n) && FlushFileBuffers(file),
            "write progress");
   }
   template <class F> void call(const char *where, F fn) const
   {
      mark("CALL", where);
      HRESULT hr = fn();
      mark("RETURN", where, hr);
      require(hr, where);
   }
};

std::wstring loaded_hash(const wchar_t *leaf)
{
   HMODULE module = GetModuleHandleW(leaf);
   std::vector<wchar_t> path(32768);
   check(module != nullptr, "native module loaded");
   DWORD n = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
   check(n && n < path.size(), "module path");
   std::unique_ptr<FILE, decltype(&std::fclose)> f(_wfopen(path.data(), L"rb"), &std::fclose);
   check(f && _fseeki64(f.get(), 0, SEEK_END) == 0, "module file");
   auto length = _ftelli64(f.get());
   check(length > 0 && length < 256LL * 1024 * 1024 && _fseeki64(f.get(), 0, SEEK_SET) == 0,
         "module length");
   std::vector<std::uint8_t> bytes(static_cast<size_t>(length));
   check(std::fread(bytes.data(), 1, bytes.size(), f.get()) == bytes.size(), "module bytes");
   std::uint8_t hash[32];
   tg_nvdec_sha256(bytes.data(), bytes.size(), hash);
   std::wstring out;
   const wchar_t *digits = L"0123456789abcdef";
   for (auto b : hash) {
      out += digits[b >> 4];
      out += digits[b & 15];
   }
   return out;
}

struct Device {
   ComPtr<IDXGIFactory1> factory;
   ComPtr<ID3D11Device> dev;
   ComPtr<ID3D11DeviceContext> ctx;
   void open(const Journal &j, unsigned long long requested, bool warp, const wchar_t *umd,
             const wchar_t *nvk)
   {
      require(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "DXGI factory");
      ComPtr<IDXGIAdapter1> native;
      for (UINT i = 0;; ++i) {
         ComPtr<IDXGIAdapter1> a;
         require(factory->EnumAdapters1(i, &a), "native adapter enumeration");
         DXGI_ADAPTER_DESC1 d{};
         require(a->GetDesc1(&d), "adapter description");
         if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && luid(d.AdapterLuid) == requested) {
            native = a;
            break;
         }
      }
      D3D_FEATURE_LEVEL fl{}, want = D3D_FEATURE_LEVEL_11_0;
      j.call("CreateDevice", [&] {
         return D3D11CreateDevice(
             warp ? nullptr : native.Get(), warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_UNKNOWN,
             nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, &want, 1, D3D11_SDK_VERSION, &dev, &fl, &ctx);
      });
      check(dev && ctx && fl == want, "device, context and level");
      ComPtr<IDXGIDevice> dx;
      require(dev.As(&dx), "DXGI device");
      ComPtr<IDXGIAdapter> a;
      require(dx->GetAdapter(&a), "drawing adapter");
      ComPtr<IDXGIAdapter1> a1;
      require(a.As(&a1), "drawing adapter1");
      DXGI_ADAPTER_DESC1 d{};
      require(a1->GetDesc1(&d), "capabilities of drawing adapter");
      bool software = (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
      check(software == warp &&
                (warp ? luid(d.AdapterLuid) != requested : luid(d.AdapterLuid) == requested),
            "actual drawing route");
      std::printf("DEVICE software=%u luid=%016llx display=%016llx vendor=%04x device=%04x fl=%x\n",
                  unsigned(software), luid(d.AdapterLuid), requested, d.VendorId, d.DeviceId,
                  unsigned(fl));
      if (!warp) {
         auto u = loaded_hash(L"tg_d3d_arm64.dll"), n = loaded_hash(L"tg_nvk_arm64.dll");
         check(u == umd && n == nvk, "native drawing modules");
         std::printf("NATIVE umd=%ls nvk=%ls\n", u.c_str(), n.c_str());
      }
      j.mark("VERIFIED", warp ? "WarpDevice" : "NativeGM20B");
   }
};

std::uint32_t rgb(UINT r, UINT g, UINT b) { return (r << 16) | (g << 8) | b; }
std::uint32_t patch(UINT x, UINT y, UINT frame)
{
   return rgb((13 * x + 5 * y + 19 * frame) & 255, (3 * x + 17 * y + 47) & 255,
              (19 * x + 11 * y + 61 * frame) & 255);
}
std::uint32_t expected(UINT x, UINT y, UINT f, bool modified)
{
   if (modified && x >= UINT(dirty.left) && x < UINT(dirty.right) && y >= UINT(dirty.top) &&
       y < UINT(dirty.bottom))
      return f ? 0x992bc7 : 0x1e9349;
   if (x >= patchX && x < patchX + patchWidth && y >= patchY && y < patchY + patchHeight)
      return patch(x - patchX, y - patchY, f);
   return f ? 0xffff00 : 0x0000ff;
}

D3D11_TEXTURE2D_DESC desc()
{
   D3D11_TEXTURE2D_DESC t{};
   t.Width = width;
   t.Height = height;
   t.MipLevels = t.ArraySize = 1;
   t.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
   t.SampleDesc.Count = 1;
   t.Usage = D3D11_USAGE_DEFAULT;
   t.BindFlags = D3D11_BIND_RENDER_TARGET;
   return t;
}

void verify_gpu(Device &d, const Journal &j, ID3D11Texture2D *target, UINT frame, bool modified,
                const char *stage)
{
   auto t = desc();
   t.Usage = D3D11_USAGE_STAGING;
   t.BindFlags = 0;
   t.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
   ComPtr<ID3D11Texture2D> read;
   require(d.dev->CreateTexture2D(&t, nullptr, &read), "create readback");
   d.ctx->CopyResource(read.Get(), target);
   D3D11_MAPPED_SUBRESOURCE m{};
   j.call("MapReadback", [&] { return d.ctx->Map(read.Get(), 0, D3D11_MAP_READ, 0, &m); });
   unsigned errors = 0, firstX = 0, firstY = 0;
   std::uint32_t got = 0, want = 0;
   if (!m.pData || m.RowPitch < width * 4)
      errors = width * height;
   else
      for (UINT y = 0; y < height; ++y)
         for (UINT x = 0; x < width; ++x) {
            std::uint32_t v;
            std::memcpy(&v, static_cast<const std::uint8_t *>(m.pData) + size_t(y) * m.RowPitch + 4 * x,
                        4);
            auto ref = expected(x, y, frame, modified);
            if ((v & 0xffffff) != ref) {
               if (errors == 0) {
                  firstX = x;
                  firstY = y;
                  got = v & 0xffffff;
                  want = ref;
               }
               ++errors;
            }
         }
   d.ctx->Unmap(read.Get(), 0);
   std::printf(
       "GPU stage=%s frame=%u pixels=%u wrong=%u first=%u,%u actual=%06x expected=%06x pitch=%u\n",
       stage, frame, width * height, errors, firstX, firstY, got, want, m.RowPitch);
   check(!errors, "GPU RGB readback");
   j.mark("VERIFIED", stage);
}
void draw(Device &d, ID3D11Texture2D *target, UINT frame)
{
   ComPtr<ID3D11RenderTargetView> rtv;
   require(d.dev->CreateRenderTargetView(target, nullptr, &rtv), "create RTV");
   ID3D11RenderTargetView *view = rtv.Get();
   d.ctx->OMSetRenderTargets(1, &view, nullptr);
   const float blue[4] = {0, 0, 1, 1}, yellow[4] = {1, 1, 0, 1};
   d.ctx->ClearRenderTargetView(view, frame ? yellow : blue);
   d.ctx->OMSetRenderTargets(0, nullptr, nullptr);
   auto t = desc();
   t.Width = patchWidth;
   t.Height = patchHeight;
   t.BindFlags = D3D11_BIND_SHADER_RESOURCE;
   t.Usage = D3D11_USAGE_IMMUTABLE;
   std::array<std::uint32_t, patchWidth * patchHeight> data{};
   for (UINT y = 0; y < patchHeight; ++y)
      for (UINT x = 0; x < patchWidth; ++x)
         data[y * patchWidth + x] = 0xff000000 | patch(x, y, frame);
   D3D11_SUBRESOURCE_DATA in{data.data(), patchWidth * 4, patchWidth * patchHeight * 4};
   ComPtr<ID3D11Texture2D> source;
   require(d.dev->CreateTexture2D(&t, &in, &source), "create GPU patch");
   d.ctx->CopySubresourceRegion(target, 0, patchX, patchY, 0, source.Get(), 0, nullptr);
}
void wait_for_write(Device &device, const Journal &journal)
{
   ComPtr<ID3D11Query> fence;
   D3D11_QUERY_DESC query{D3D11_QUERY_EVENT, 0};
   require(device.dev->CreateQuery(&query, &fence), "create GPU completion event");
   device.ctx->End(fence.Get());
   device.ctx->Flush();
   const ULONGLONG start = GetTickCount64();
   while (GetTickCount64() - start < 15000) {
      BOOL ready = FALSE;
      HRESULT hr = device.ctx->GetData(fence.Get(), &ready, sizeof(ready), D3D11_ASYNC_GETDATA_DONOTFLUSH);
      require(hr, "poll GPU event");
      if (hr == S_OK && ready) {
         require(device.dev->GetDeviceRemovedReason(), "device status after write");
         journal.mark("VERIFIED", "GPUWriteComplete");
         return;
      }
      Sleep(1);
   }
   throw Failure{"GPU completion deadline", HRESULT_FROM_WIN32(WAIT_TIMEOUT)};
}

void peer(const Journal &journal, unsigned long long display, const wchar_t *umd, const wchar_t *nvk,
          HANDLE shared)
{
   Device consumer;
   consumer.open(journal, display, false, umd, nvk);
   std::printf("ROLE peer %lu\n", GetCurrentProcessId());
   ComPtr<ID3D11Texture2D> opened;
   journal.call("OpenSharedResource", [&] {
      return consumer.dev->OpenSharedResource(shared, IID_PPV_ARGS(&opened));
   });
   D3D11_TEXTURE2D_DESC actual{};
   opened->GetDesc(&actual);
   check(actual.Width == width && actual.Height == height && actual.Format == DXGI_FORMAT_B8G8R8A8_UNORM,
         "opened shared texture layout");
   verify_gpu(consumer, journal, opened.Get(), 0, false, "PeerSeesProducer");
   draw(consumer, opened.Get(), 1);
   wait_for_write(consumer, journal);
}

void child_peer(const Journal &journal, wchar_t **argv, HANDLE resource)
{
   std::vector<wchar_t> path(32768);
   DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
   check(n && n < path.size(), "client executable path");
   wchar_t value[2 * sizeof(uintptr_t) + 1]{};
   check(std::swprintf(value, std::size(value), L"%llx",
                       static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(resource))) > 0,
         "format shared resource handle");
   std::wstring cmd = L"\"" + std::wstring(path.data()) + L"\" peer " + argv[2] + L" " + argv[3] + L" " +
                      argv[4] + L" \"" + argv[5] + L"\\peer\" " + value;
   STARTUPINFOW startup{};
   startup.cb = sizeof(startup);
   PROCESS_INFORMATION child{};
   journal.mark("CALL", "StartPeer");
   check(CreateProcessW(path.data(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        nullptr, &startup, &child) != FALSE, "start peer process");
   CloseHandle(child.hThread);
   DWORD exit_code = 1;
   DWORD waited = WaitForSingleObject(child.hProcess, 65000);
   if (waited != WAIT_OBJECT_0) {
      TerminateProcess(child.hProcess, 3);
      WaitForSingleObject(child.hProcess, 3000);
   }
   BOOL read_exit = GetExitCodeProcess(child.hProcess, &exit_code);
   CloseHandle(child.hProcess);
   std::printf("CHILD pid=%lu wait=%lu exit=%lu\n", child.dwProcessId, waited, exit_code);
   check(waited == WAIT_OBJECT_0 && read_exit && exit_code == 0, "peer process exit");
   journal.mark("RETURN", "StartPeer");
}

void producer(const Journal &journal, wchar_t **argv, bool separate)
{
   unsigned long long display = std::wcstoull(argv[2], nullptr, 16);
   Device source;
   source.open(journal, display, false, argv[3], argv[4]);
   std::printf("ROLE producer %lu\n", GetCurrentProcessId());
   ComPtr<ID3D11Texture2D> texture;
   auto layout = desc();
   layout.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
   layout.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
   journal.call("CreateSharedTexture", [&] {
      return source.dev->CreateTexture2D(&layout, nullptr, &texture);
   });
   ComPtr<IDXGIResource> dxgi;
   require(texture.As(&dxgi), "query shared texture");
   HANDLE shared = nullptr;
   journal.call("GetSharedHandle", [&] { return dxgi->GetSharedHandle(&shared); });
   check(shared != nullptr, "valid texture handle");
   draw(source, texture.Get(), 0);
   wait_for_write(source, journal);
   if (separate)
      child_peer(journal, argv, shared);
   else
      peer(journal, display, argv[3], argv[4], shared);
   verify_gpu(source, journal, texture.Get(), 1, false, "ProducerSeesPeer");
   require(source.dev->GetDeviceRemovedReason(), "producer device status after return");
   // The legacy DXGI handle is owned by the resource. Both devices retain it until their work completes.
}
} // namespace

int wmain(int argc, wchar_t **argv)
{
   if (argc < 6 || argc > 7)
      return 2;
   std::wstring mode = argv[1], directory = argv[5];
   const bool child = mode == L"peer";
   if ((child && argc != 7) || (!child && (argc != 6 || (mode != L"local" && mode != L"process"))))
      return 2;
   if (!CreateDirectoryW(directory.c_str(), nullptr) ||
       !_wfreopen((directory + L"\\stdout.txt").c_str(), L"wb", stdout) ||
       !_wfreopen((directory + L"\\stderr.txt").c_str(), L"wb", stderr))
      return 3;
   std::setvbuf(stdout, nullptr, _IONBF, 0);
   std::setvbuf(stderr, nullptr, _IONBF, 0);
   SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
   try {
      Journal journal(directory);
      try {
         if (child) {
            auto value = std::wcstoull(argv[6], nullptr, 16);
            check(value != 0, "shared handle argument");
            peer(journal, std::wcstoull(argv[2], nullptr, 16), argv[3], argv[4],
                 reinterpret_cast<HANDLE>(static_cast<uintptr_t>(value)));
         } else {
            producer(journal, argv, mode == L"process");
         }
      } catch (const Failure &f) {
         journal.mark("FAIL", f.where, f.status);
         throw;
      }
      journal.mark("EXIT", "RunAndRelease");
      return 0;
   } catch (const Failure &f) {
      std::fprintf(stderr, "FAIL %s %08lx\n", f.where, static_cast<unsigned long>(f.status));
      return 4;
   } catch (...) {
      return 5;
   }
}
