// SPDX-License-Identifier: MIT
// Check both directions of the public DXGI surface DC handoff on a selected D3D11 device.
// Modes select WARP or native drawing with a surface, chain, or blit (for example, native-blit).
// The blit cases copy a rectangle between two devices on the same adapter.
// Arguments are the mode,
// the GM20B display LUID in hex, native tgd3d and NVK SHA-256 values, and a fresh
// output directory. RGB comparisons ignore the alpha channel written by GDI.
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
COLORREF win_color(std::uint32_t x) { return RGB((x >> 16) & 255, (x >> 8) & 255, x & 255); }
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

struct Window {
   HWND handle = nullptr;
   ~Window()
   {
      if (handle)
         DestroyWindow(handle);
   }
   void create()
   {
      WNDCLASSW cl{};
      cl.lpfnWndProc = DefWindowProcW;
      cl.hInstance = GetModuleHandleW(nullptr);
      cl.lpszClassName = L"SwitchSurfaceDcControl";
      check(RegisterClassW(&cl) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS, "window class");
      RECT r{0, 0, (LONG)width, (LONG)height};
      check(AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE) != FALSE, "window size");
      handle =
          CreateWindowExW(0, cl.lpszClassName, L"Switch DXGI Surface DC", WS_OVERLAPPEDWINDOW, 32, 35,
                          r.right - r.left, r.bottom - r.top, nullptr, nullptr, cl.hInstance, nullptr);
      check(handle != nullptr, "window");
      ShowWindow(handle, SW_SHOWNOACTIVATE);
      UpdateWindow(handle);
      MSG msg{};
      while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
         TranslateMessage(&msg);
         DispatchMessageW(&msg);
      }
   }
};
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
class SurfaceDC
{
   IDXGISurface1 *surface;

 public:
   HDC hdc = nullptr;
   explicit SurfaceDC(IDXGISurface1 *s) : surface(s) {}
   ~SurfaceDC()
   {
      if (hdc)
         surface->ReleaseDC(nullptr);
   }
   HRESULT release(RECT *region = nullptr)
   {
      RECT r = dirty;
      HRESULT hr = surface->ReleaseDC(region ? region : &r);
      if (SUCCEEDED(hr))
         hdc = nullptr;
      return hr;
   }
};
void gdi(Device &d, const Journal &j, ID3D11Texture2D *target, UINT frame)
{
   ComPtr<IDXGISurface1> s;
   j.call("QuerySurface1", [&] { return target->QueryInterface(IID_PPV_ARGS(&s)); });
   SurfaceDC dc(s.Get());
   j.call("GetDC_preserve", [&] { return s->GetDC(FALSE, &dc.hdc); });
   check(dc.hdc != nullptr, "surface HDC");
   constexpr POINT points[] = {{0, 0},   {639, 0}, {0, 359},   {639, 359}, {11, 8},  {22, 14}, {33, 20},
                               {34, 21}, {47, 29}, {319, 179}, {43, 19},   {86, 50}, {87, 51}};
   unsigned wrong = 0;
   COLORREF first = 0, ref = 0;
   for (const auto &pt : points) {
      COLORREF actual = GetPixel(dc.hdc, pt.x, pt.y),
               want = win_color(expected(pt.x, pt.y, frame, false));
      if (actual != want) {
         if (wrong == 0) {
            first = actual;
            ref = want;
         }
         ++wrong;
      }
   }
   std::printf("DC_READ frame=%u samples=%zu wrong=%u first=%08lx expected=%08lx\n", frame,
               std::size(points), wrong, first, ref);
   check(!wrong, "GDI reads GPU RGB");
   j.mark("VERIFIED", "DCReadGPU");
   HBRUSH brush = CreateSolidBrush(win_color(expected(dirty.left, dirty.top, frame, true)));
   check(brush != nullptr, "GDI brush");
   int filled = FillRect(dc.hdc, &dirty, brush);
   BOOL flushed = GdiFlush();
   DeleteObject(brush);
   check(filled && flushed, "GDI fill and flush");
   check(GetPixel(dc.hdc, dirty.left, dirty.top) ==
             win_color(expected(dirty.left, dirty.top, frame, true)),
         "DC verifies its write");
   j.call("ReleaseDC_dirty", [&] { return dc.release(); });
   verify_gpu(d, j, target, frame, true, "AfterDCWrite");
}
void blit_between_devices(const Journal &j, bool warp, unsigned long long display, const wchar_t *umd,
                          const wchar_t *nvk)
{
   Device producer;
   Device consumer;
   producer.open(j, display, warp, umd, nvk);
   consumer.open(j, display, warp, umd, nvk);
   auto texture = desc();
   texture.MiscFlags = D3D11_RESOURCE_MISC_GDI_COMPATIBLE;
   ComPtr<ID3D11Texture2D> source;
   j.call("CreateGdiSource", [&] { return producer.dev->CreateTexture2D(&texture, nullptr, &source); });
   texture.Width = width + 1;
   texture.Height = height + 1;
   ComPtr<ID3D11Texture2D> destination;
   j.call("CreateGdiDestination",
          [&] { return consumer.dev->CreateTexture2D(&texture, nullptr, &destination); });
   ComPtr<ID3D11RenderTargetView> target;
   require(consumer.dev->CreateRenderTargetView(destination.Get(), nullptr, &target), "destination RTV");
   ComPtr<IDXGISurface1> src_surface;
   ComPtr<IDXGISurface1> dst_surface;
   require(source.As(&src_surface), "source DXGI surface");
   require(destination.As(&dst_surface), "destination DXGI surface");
   texture.MiscFlags = 0;
   texture.BindFlags = 0;
   texture.Usage = D3D11_USAGE_STAGING;
   texture.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
   ComPtr<ID3D11Texture2D> readback;
   require(consumer.dev->CreateTexture2D(&texture, nullptr, &readback), "destination readback");

   for (UINT frame = 0; frame < 2; ++frame) {
      draw(producer, source.Get(), frame);
      verify_gpu(producer, j, source.Get(), frame, false, "SourceBeforeBlt");
      const float green[] = {0, 1, 0, 1};
      consumer.ctx->ClearRenderTargetView(target.Get(), green);
      {
         SurfaceDC src_dc(src_surface.Get());
         SurfaceDC dst_dc(dst_surface.Get());
         j.call("GetSourceDC", [&] { return src_surface->GetDC(FALSE, &src_dc.hdc); });
         j.call("GetDestinationDC", [&] { return dst_surface->GetDC(FALSE, &dst_dc.hdc); });
         j.call("BitBltWindowRect", [&] {
            if (BitBlt(dst_dc.hdc, 1, 1, width, height, src_dc.hdc, 0, 0, SRCCOPY))
               return S_OK;
            DWORD error = GetLastError();
            return error ? HRESULT_FROM_WIN32(error) : E_FAIL;
         });
         check(GdiFlush() != FALSE, "flush DC copy");
         RECT written{1, 1, LONG(width + 1), LONG(height + 1)};
         RECT unmodified{};
         j.call("ReleaseDestinationDC", [&] { return dst_dc.release(&written); });
         j.call("ReleaseSourceDC", [&] { return src_dc.release(&unmodified); });
      }
      consumer.ctx->CopyResource(readback.Get(), destination.Get());
      D3D11_MAPPED_SUBRESOURCE mapped{};
      j.call("MapBltResult",
             [&] { return consumer.ctx->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped); });
      const UINT pixels = texture.Width * texture.Height;
      UINT wrong = 0;
      if (!mapped.pData || mapped.RowPitch < texture.Width * 4) {
         wrong = pixels;
      } else {
         for (UINT y = 0; y < texture.Height; ++y) {
            const auto *row =
                static_cast<const std::uint8_t *>(mapped.pData) + size_t(y) * mapped.RowPitch;
            for (UINT x = 0; x < texture.Width; ++x) {
               std::uint32_t value;
               std::memcpy(&value, row + x * 4, sizeof(value));
               const auto ref = x && y ? expected(x - 1, y - 1, frame, false) : 0x00ff00u;
               if ((value & 0xffffffu) != ref)
                  ++wrong;
            }
         }
      }
      consumer.ctx->Unmap(readback.Get(), 0);
      std::printf("BLIT_FRAME frame=%u pixels=%u border=%u mismatches=%u staging_pitch=%u\n", frame,
                  pixels, pixels - width * height, wrong, mapped.RowPitch);
      check(wrong == 0, "window blit contents and untouched border");
      j.mark("VERIFIED", "WindowBlitRGB");
   }
   j.call("FinalProducerDevice", [&] { return producer.dev->GetDeviceRemovedReason(); });
   j.call("FinalConsumerDevice", [&] { return consumer.dev->GetDeviceRemovedReason(); });
}

void run(const Journal &j, bool chain, bool warp, unsigned long long display, const wchar_t *umd,
         const wchar_t *nvk)
{
   Device d;
   d.open(j, display, warp, umd, nvk);
   auto t = desc();
   {
      ComPtr<ID3D11Texture2D> ordinary;
      j.call("CreatePlain", [&] { return d.dev->CreateTexture2D(&t, nullptr, &ordinary); });
      draw(d, ordinary.Get(), 0);
      verify_gpu(d, j, ordinary.Get(), 0, false, "PlainControl");
   }
   Window window;
   ComPtr<IDXGISwapChain> swap;
   ComPtr<ID3D11Texture2D> target;
   if (chain) {
      window.create();
      DXGI_SWAP_CHAIN_DESC sc{};
      sc.BufferDesc.Width = width;
      sc.BufferDesc.Height = height;
      sc.BufferDesc.Format = t.Format;
      sc.SampleDesc.Count = 1;
      sc.BufferCount = 2;
      sc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
      sc.Windowed = TRUE;
      sc.OutputWindow = window.handle;
      sc.SwapEffect = DXGI_SWAP_EFFECT_SEQUENTIAL;
      sc.Flags = DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE;
      j.call("CreateGdiSwapChain", [&] { return d.factory->CreateSwapChain(d.dev.Get(), &sc, &swap); });
      j.call("GetBackBuffer", [&] { return swap->GetBuffer(0, IID_PPV_ARGS(&target)); });
   } else {
      t.MiscFlags = D3D11_RESOURCE_MISC_GDI_COMPATIBLE;
      j.call("CreateGdiTexture", [&] { return d.dev->CreateTexture2D(&t, nullptr, &target); });
   }
   D3D11_TEXTURE2D_DESC got{};
   target->GetDesc(&got);
   check(got.Width == width && got.Height == height && got.Format == DXGI_FORMAT_B8G8R8A8_UNORM &&
             got.SampleDesc.Count == 1,
         "target dimensions/format");
   std::printf("SURFACE chain=%u flags=%x usage=%x bind=%x\n", unsigned(chain), got.MiscFlags, got.Usage,
               got.BindFlags);
   for (UINT frame = 0; frame < 2; ++frame) {
      draw(d, target.Get(), frame);
      verify_gpu(d, j, target.Get(), frame, false, "BeforeDCRead");
      gdi(d, j, target.Get(), frame);
   }
   d.ctx->ClearState();
   d.ctx->Flush();
   j.call("FinalDevice", [&] { return d.dev->GetDeviceRemovedReason(); });
}
} // namespace
int wmain(int argc, wchar_t **argv)
{
   if (argc != 6)
      return 2;
   std::wstring mode = argv[1];
   bool blit = mode == L"warp-blit" || mode == L"native-blit";
   bool warp = mode == L"warp-surface" || mode == L"warp-chain" || mode == L"warp-blit";
   bool chain = mode == L"warp-chain" || mode == L"native-chain";
   if (!warp && !chain && !blit && mode != L"native-surface")
      return 2;
   if (std::wcslen(argv[2]) != 16 || std::wcsspn(argv[2], L"0123456789abcdefABCDEF") != 16)
      return 2;
   std::wstring dir = argv[5];
   if (!CreateDirectoryW(dir.c_str(), nullptr))
      return 3;
   if (!_wfreopen((dir + L"\\stdout.txt").c_str(), L"wb", stdout) ||
       !_wfreopen((dir + L"\\stderr.txt").c_str(), L"wb", stderr))
      return 3;
   std::setvbuf(stdout, nullptr, _IONBF, 0);
   std::setvbuf(stderr, nullptr, _IONBF, 0);
   SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
   try {
      Journal j(dir);
      j.mark("ENTER", "Case");
      try {
         if (blit)
            blit_between_devices(j, warp, std::wcstoull(argv[2], nullptr, 16), argv[3], argv[4]);
         else
            run(j, chain, warp, std::wcstoull(argv[2], nullptr, 16), argv[3], argv[4]);
      } catch (const Failure &f) {
         j.mark("FAIL", f.where, f.status);
         throw;
      }
      j.mark("EXIT", "RunAndRelease");
      return 0;
   } catch (const Failure &f) {
      std::fprintf(stderr, "FAIL %s hr=%08lx\n", f.where, (unsigned long)f.status);
      return 4;
   } catch (...) {
      return 5;
   }
}
