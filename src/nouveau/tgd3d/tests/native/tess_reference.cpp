/* SPDX-License-Identifier: MIT
 * Run the native DDI tessellation fixtures through the public system D3D11 runtime.
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <cmath>
#include <cstring>
#include <iterator>
#include "tess_cases.h"
#include "gm20b_adapter.h"
#include "logic_shaders.h"

using Microsoft::WRL::ComPtr;
using namespace tess_test;

static bool success(const char *operation, HRESULT hr)
{
   if (SUCCEEDED(hr)) return true;
   std::printf("api-error\t%s\t%08lx\n", operation, static_cast<unsigned long>(hr));
   return false;
}

#include "coverage_reference.h"

struct Reference {
   ComPtr<ID3D11Device> dev;
   ComPtr<ID3D11DeviceContext> ctx;
   ComPtr<ID3D11HullShader> hs[std::size(hulls)];
   ComPtr<ID3D11DomainShader> ds[std::size(domains)];
   ComPtr<ID3D11VertexShader> vs;
   ComPtr<ID3D11GeometryShader> gs;
   bool lineStream = false;
   bool quadLines = false;

   bool triangle(bool hardware)
   {
      constexpr UINT width = 31, height = 19;
      const D3D11_RECT rect{3, 2, 23, 16};
      ComPtr<ID3D11PixelShader> ps;
      ComPtr<ID3D11Texture2D> color, staging;
      ComPtr<ID3D11RenderTargetView> rtv;
      ComPtr<ID3D11RasterizerState> raster;
      D3D11_TEXTURE2D_DESC td{};
      td.Width = width; td.Height = height;
      td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;
      td.Format = DXGI_FORMAT_R8G8B8A8_UINT;
      td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET;
      D3D11_RASTERIZER_DESC rd{};
      rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE;
      rd.ScissorEnable = rd.DepthClipEnable = TRUE;
      if (!success("triangle VS", dev->CreateVertexShader(vs_dxbc, sizeof(vs_dxbc), nullptr, &vs)) ||
          !success("triangle PS", dev->CreatePixelShader(ps_dxbc, sizeof(ps_dxbc), nullptr, &ps)) ||
          !success("triangle texture", dev->CreateTexture2D(&td, nullptr, &color)) ||
          !success("triangle target", dev->CreateRenderTargetView(color.Get(), nullptr, &rtv)) ||
          !success("triangle rasterizer", dev->CreateRasterizerState(&rd, &raster))) return false;
      td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      if (!success("triangle staging", dev->CreateTexture2D(&td, nullptr, &staging))) return false;
      const FLOAT zero[4]{};
      ctx->ClearRenderTargetView(rtv.Get(), zero);
      ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
      ctx->RSSetState(raster.Get());
      D3D11_VIEWPORT vp{0, 0, FLOAT(width), FLOAT(height), 0, 1};
      ctx->RSSetViewports(1, &vp); ctx->RSSetScissorRects(1, &rect);
      ctx->VSSetShader(vs.Get(), nullptr, 0); ctx->PSSetShader(ps.Get(), nullptr, 0);
      ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      ctx->Draw(3, 0);
      ctx->OMSetRenderTargets(0, nullptr, nullptr);
      ctx->CopyResource(staging.Get(), color.Get());
      D3D11_MAPPED_SUBRESOURCE map{};
      if (!success("triangle map", ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map))) return false;
      UINT painted = 0, untouched = 0;
      if (map.pData && map.RowPitch >= width * 4) {
         for (LONG y = 0; y < LONG(height); ++y) {
            const BYTE *row = static_cast<const BYTE *>(map.pData) + y * map.RowPitch;
            for (LONG x = 0; x < LONG(width); ++x) {
               UINT word;
               std::memcpy(&word, row + x * 4, sizeof(word));
               if (x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom)
                  painted += word == 0x0227c35a;
               else
                  untouched += word == 0;
            }
         }
      }
      ctx->Unmap(staging.Get(), 0);
      ctx->ClearState(); ctx->Flush();
      if (hardware) {
         for (const wchar_t *name : {L"TegraGpuD3DVk.dll", L"libvulkan_nouveau.dll"}) {
            HMODULE module = GetModuleHandleW(name);
            wchar_t path[32768];
            if (!module || !GetModuleFileNameW(module, path, UINT(std::size(path)))) return false;
            std::printf("hardware-module\t%ls\n", path);
         }
      }
      std::printf("triangle-readback\t%u\t%u\n", painted, untouched);
      return painted == UINT((rect.right - rect.left) * (rect.bottom - rect.top)) &&
             painted + untouched == width * height && success("device status", dev->GetDeviceRemovedReason());
   }

   bool init(D3D_DRIVER_TYPE type, bool triangleOnly)
   {
      HMODULE module = LoadLibraryExW(L"d3d11.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
      auto create = module ? reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(GetProcAddress(module, "D3D11CreateDevice")) : nullptr;
      const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_1;
      D3D_FEATURE_LEVEL actual{};
      Gm20bAdapter native;
      if (type == D3D_DRIVER_TYPE_HARDWARE) {
         if (!success("native adapter", native.open())) return false;
         std::printf("hardware-adapter\t%08lx%08lx\t%x\t%x\t%u\n", ULONG(native.identity.AdapterLuid.HighPart),
            native.identity.AdapterLuid.LowPart,native.gpu.vendor,native.gpu.chipset,native.gpu.sm);
      }
      if (!create || !success("D3D11CreateDevice", create(native.adapter.Get(), native.adapter ? D3D_DRIVER_TYPE_UNKNOWN : type, nullptr, 0, &level, 1,
          D3D11_SDK_VERSION, &dev, &actual, &ctx))) return false;
      if (!dev || !ctx || ctx->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE ||
          dev->GetFeatureLevel() != level) return false;
      ComPtr<IDXGIDevice> dxgi; ComPtr<IDXGIAdapter> adapter; ComPtr<IDXGIAdapter1> info;
      DXGI_ADAPTER_DESC1 desc{};
      if (!success("DXGI device", dev.As(&dxgi)) || !success("adapter", dxgi->GetAdapter(&adapter)) ||
          !success("DXGI adapter1", adapter.As(&info)) || !success("adapter identity", info->GetDesc1(&desc))) return false;
      std::printf("runtime\t%s\t%x\t%x\t%x\t%x\t%08lx%08lx\n", type == D3D_DRIVER_TYPE_WARP ? "WARP" : "hardware",
         actual, desc.VendorId, desc.DeviceId, desc.Flags, static_cast<unsigned long>(desc.AdapterLuid.HighPart),
         static_cast<unsigned long>(desc.AdapterLuid.LowPart));
      if (actual != level || (type == D3D_DRIVER_TYPE_WARP ?
          !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) || desc.VendorId != 0x1414 :
          !native.matches(desc))) return false;

      if (triangleOnly) return true;
      ComPtr<ID3D11PixelShader> ps;
      if (!success("VS", dev->CreateVertexShader(tess_vs, sizeof(tess_vs), nullptr, &vs)) ||
          !success("PS", dev->CreatePixelShader(tess_ps, sizeof(tess_ps), nullptr, &ps)) ||
          !success("GS", dev->CreateGeometryShader(tess_gs, sizeof(tess_gs), nullptr, &gs))) return false;
      ctx->PSSetShader(ps.Get(), nullptr, 0);
      for (size_t i = 0; i < std::size(hulls); ++i)
         if (!success("HS", dev->CreateHullShader(hulls[i].code, hulls[i].bytes(), nullptr, &hs[i]))) return false;
      for (size_t i = 0; i < std::size(domains); ++i)
         if (!success("DS", dev->CreateDomainShader(domains[i].code, domains[i].bytes(), nullptr, &ds[i]))) return false;

      D3D11_TEXTURE2D_DESC tex{};
      tex.Width = tex.Height = 2; tex.MipLevels = tex.ArraySize = tex.SampleDesc.Count = 1;
      tex.Format = DXGI_FORMAT_R32_FLOAT; tex.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      const float samples[] = {7, 11, 13, 17};
      D3D11_SUBRESOURCE_DATA data{samples, 8, 16};
      ComPtr<ID3D11Texture2D> lut; ComPtr<ID3D11ShaderResourceView> srv;
      if (!success("lookup image", dev->CreateTexture2D(&tex, &data, &lut)) ||
          !success("lookup view", dev->CreateShaderResourceView(lut.Get(), nullptr, &srv))) return false;
      ctx->HSSetShaderResources(6, 1, srv.GetAddressOf()); ctx->DSSetShaderResources(6, 1, srv.GetAddressOf());
      D3D11_SAMPLER_DESC sd{}; sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
      sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
      sd.MaxAnisotropy = 1; sd.ComparisonFunc = D3D11_COMPARISON_NEVER; sd.MaxLOD = 1;
      ComPtr<ID3D11SamplerState> sampler;
      if (!success("sampler", dev->CreateSamplerState(&sd, &sampler))) return false;
      ctx->HSSetSamplers(3, 1, sampler.GetAddressOf()); ctx->DSSetSamplers(3, 1, sampler.GetAddressOf());
      D3D11_RASTERIZER_DESC rs{}; rs.CullMode = D3D11_CULL_NONE; rs.FillMode = D3D11_FILL_SOLID; rs.DepthClipEnable = TRUE;
      rs.MultisampleEnable = quadLines;
      ComPtr<ID3D11RasterizerState> raster;
      if (!success("rasterizer", dev->CreateRasterizerState(&rs, &raster))) return false;
      ctx->RSSetState(raster.Get());
      D3D11_DEPTH_STENCIL_DESC dd{};
      dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
      dd.FrontFace = dd.BackFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_ALWAYS};
      ComPtr<ID3D11DepthStencilState> depth;
      if (!success("depth state", dev->CreateDepthStencilState(&dd, &depth))) return false;
      ctx->OMSetDepthStencilState(depth.Get(), 0);
      D3D11_VIEWPORT vp{0, 0, float(width), float(height), 0, 1}; ctx->RSSetViewports(1, &vp);
      return true;
   }

   bool replay_lines(ID3D11Buffer *stream, const std::array<UINT, 16 * 5> &vertices, const Case &c, UINT tag)
   {
      if (!verify_line_segments(vertices, c, tag)) return false;
      ComPtr<ID3D11VertexShader> replay;
      ComPtr<ID3D11InputLayout> layout;
      const D3D11_INPUT_ELEMENT_DESC elements[]{
         {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
         {"TEXCOORD", 0, DXGI_FORMAT_R32_UINT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0}};
      if (!success("line replay VS", dev->CreateVertexShader(line_replay_vs, sizeof(line_replay_vs), nullptr, &replay)) ||
          !success("line replay layout", dev->CreateInputLayout(elements, UINT(std::size(elements)),
             line_replay_vs, sizeof(line_replay_vs), &layout))) return false;
      ctx->HSSetShader(nullptr, nullptr, 0); ctx->DSSetShader(nullptr, nullptr, 0); ctx->GSSetShader(nullptr, nullptr, 0);
      ctx->VSSetShader(replay.Get(), nullptr, 0); ctx->IASetInputLayout(layout.Get());
      const UINT stride = 5 * sizeof(UINT), offset = 0;
      ctx->IASetVertexBuffers(0, 1, &stream, &stride, &offset);
      ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
      ctx->Draw(12, 0);
      return true;
   }

   template<typename T>
   bool buffer(const T &src, UINT bind, ComPtr<ID3D11Buffer> &buf)
   {
      D3D11_BUFFER_DESC bd{}; bd.ByteWidth = sizeof(src); bd.BindFlags = bind;
      if (bind == D3D11_BIND_UNORDERED_ACCESS) bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
      D3D11_SUBRESOURCE_DATA data{src.data(), 0, 0};
      return success("buffer", dev->CreateBuffer(&bd, &data, &buf));
   }

   bool view(ID3D11Buffer *buf, UINT words, ComPtr<ID3D11UnorderedAccessView> &out)
   {
      D3D11_UNORDERED_ACCESS_VIEW_DESC uv{};
      uv.Format = DXGI_FORMAT_R32_TYPELESS; uv.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
      uv.Buffer = {firstWord, words, D3D11_BUFFER_UAV_FLAG_RAW};
      return success("buffer UAV", dev->CreateUnorderedAccessView(buf, &uv, &out));
   }

   template<typename T>
   bool read(ID3D11Buffer *buf, T &values)
   {
      D3D11_BUFFER_DESC bd{}; bd.ByteWidth = sizeof(values); bd.Usage = D3D11_USAGE_STAGING; bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      ComPtr<ID3D11Buffer> staging;
      if (!success("staging buffer", dev->CreateBuffer(&bd, nullptr, &staging))) return false;
      ctx->CopyResource(staging.Get(), buf);
      D3D11_MAPPED_SUBRESOURCE m{};
      if (!success("map buffer", ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
      std::memcpy(values.data(), m.pData, sizeof(values)); ctx->Unmap(staging.Get(), 0);
      return true;
   }

   bool scene(UINT index)
   {
      const Case &c = scenes[index];
      std::printf("tess-begin\t%s\n", c.name);
      ctx->VSSetShader(vs.Get(), nullptr, 0); ctx->IASetInputLayout(nullptr);
      ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY(D3D11_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST + c.in - 1));
      ctx->HSSetShader(hs[c.hull].Get(), nullptr, 0); ctx->DSSetShader(ds[c.domain].Get(), nullptr, 0);
      ctx->GSSetShader(c.gs ? gs.Get() : nullptr, nullptr, 0);
      HullData h = initial_hull(); DomainData d = initial_domain(); Pixels pixels = initial_pixels();
      UINT factor; std::memcpy(&factor, &c.factor, sizeof(factor));
      const std::array<UINT, 4> hc{hs_seed(index), factor}, dc{ds_seed(index)};
      ComPtr<ID3D11Buffer> hb, db, hcb, dcb;
      ComPtr<ID3D11UnorderedAccessView> hu, du;
      if (!buffer(h, D3D11_BIND_UNORDERED_ACCESS, hb) || !buffer(d, D3D11_BIND_UNORDERED_ACCESS, db) ||
          !buffer(hc, D3D11_BIND_CONSTANT_BUFFER, hcb) || !buffer(dc, D3D11_BIND_CONSTANT_BUFFER, dcb) ||
          !view(hb.Get(), hullWords, hu) || !view(db.Get(), domainWords, du)) return false;
      ctx->HSSetConstantBuffers(13, 1, hcb.GetAddressOf()); ctx->DSSetConstantBuffers(13, 1, dcb.GetAddressOf());
      D3D11_TEXTURE2D_DESC td{}; td.Width = width; td.Height = height; td.Format = DXGI_FORMAT_R32_UINT;
      td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1; td.BindFlags = D3D11_BIND_RENDER_TARGET;
      D3D11_SUBRESOURCE_DATA init{pixels.data(), width * 4, width * height * 4}; ComPtr<ID3D11Texture2D> color, staging;
      ComPtr<ID3D11RenderTargetView> rtv;
      if (!success("target", dev->CreateTexture2D(&td, &init, &color)) ||
          !success("target view", dev->CreateRenderTargetView(color.Get(), nullptr, &rtv))) return false;
      td.BindFlags = 0; td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      if (!success("staging image", dev->CreateTexture2D(&td, nullptr, &staging))) return false;
      D3D11_QUERY_DESC qd{D3D11_QUERY_PIPELINE_STATISTICS, 0}; ComPtr<ID3D11Query> query;
      if (!success("statistics", dev->CreateQuery(&qd, &query))) return false;
      ID3D11UnorderedAccessView *uavs[]{hu.Get(), du.Get()};
      ctx->OMSetRenderTargetsAndUnorderedAccessViews(1, rtv.GetAddressOf(), nullptr, 62, 2, uavs, nullptr);
      std::array<UINT, 16 * 5> vertices;
      ComPtr<ID3D11Buffer> stream;
      if (lineStream) {
         vertices.fill(guard);
         const D3D11_SO_DECLARATION_ENTRY fields[] = {{0, "SV_Position", 0, 0, 4, 0}, {0, "TEXCOORD", 0, 0, 1, 0}};
         const UINT stride = 5 * sizeof(UINT), offset = 0;
         ComPtr<ID3D11GeometryShader> capture;
         if (!success("DS output signature", dev->CreateGeometryShaderWithStreamOutput(line_ds, sizeof(line_ds),
             fields, UINT(std::size(fields)), &stride, 1, D3D11_SO_NO_RASTERIZED_STREAM, nullptr, &capture)) ||
             !buffer(vertices, D3D11_BIND_STREAM_OUTPUT | D3D11_BIND_VERTEX_BUFFER, stream)) return false;
         ctx->GSSetShader(capture.Get(), nullptr, 0); ctx->SOSetTargets(1, stream.GetAddressOf(), &offset);
      }
      ctx->Begin(query.Get()); ctx->Draw(c.in, 0); ctx->End(query.Get());
      if (lineStream) {
         ctx->SOSetTargets(0, nullptr, nullptr);
         if (!read(stream.Get(), vertices)) return false;
         emit("line-stream", c.name, vertices);
         const UINT control0 = hs_seed(index) + 7 + 3 * c.in;
         const UINT tag = ds_seed(index) + 17 + hs_seed(index) + 7 + 5 * c.in + 73 + 2 * control0 + 13 * (c.out - 1);
         if (!replay_lines(stream.Get(), vertices, c, tag)) return false;
      }
      ID3D11UnorderedAccessView *empty[2]{};
      ctx->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 62, 2, empty, nullptr);
      ctx->CopyResource(staging.Get(), color.Get());
      if (!read(hb.Get(), h) || !read(db.Get(), d)) return false;
      D3D11_MAPPED_SUBRESOURCE map{};
      if (!success("map pixels", ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map))) return false;
      for (UINT y = 0; y < height; ++y) std::memcpy(pixels.data() + width * y,
          static_cast<const BYTE *>(map.pData) + y * map.RowPitch, width * 4);
      ctx->Unmap(staging.Get(), 0);
      D3D11_QUERY_DATA_PIPELINE_STATISTICS stats{};
      const ULONGLONG deadline = GetTickCount64() + 5000;
      HRESULT hr;
      while ((hr = ctx->GetData(query.Get(), &stats, sizeof(stats), 0)) == S_FALSE && GetTickCount64() < deadline) Sleep(1);
      if (hr != S_OK) { std::printf("statistics-unavailable\t%08lx\n", static_cast<unsigned long>(hr)); return false; }
      const bool result = verify(index, h, d, pixels, stats.IAVertices, stats.HSInvocations, stats.DSInvocations, stats.GSPrimitives);
      std::printf("tess-%s\t%s\n", result ? "verified" : "mismatch", c.name);
      return result;
   }
};

int wmain(int argc, wchar_t **argv)
{
   SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
   std::setvbuf(stdout, nullptr, _IONBF, 0);
   Reference ref;
   if (argc != 2) return 2;
   ref.lineStream = !std::wcscmp(argv[1], L"--warp-lines");
   ref.quadLines = !std::wcscmp(argv[1], L"--warp-quad-lines");
   const bool hardwareDraw = !std::wcscmp(argv[1], L"--hardware-draw");
   const bool triangle = hardwareDraw || !std::wcscmp(argv[1], L"--warp-draw");
   const bool hardwareCoverage = !std::wcscmp(argv[1], L"--hardware-coverage");
   const bool sweep = !std::wcscmp(argv[1], L"--warp-coverage-sweep");
   const bool hardwareColor = !std::wcscmp(argv[1], L"--hardware-coverage-color");
   const bool color = hardwareColor || !std::wcscmp(argv[1], L"--warp-coverage-color");
   const bool coverage = color || sweep || hardwareCoverage || !std::wcscmp(argv[1], L"--warp-coverage");
   const bool hardware = hardwareDraw || hardwareCoverage || hardwareColor || !std::wcscmp(argv[1], L"--hardware");
   if (!ref.lineStream && !ref.quadLines && !hardware && !triangle && !coverage && std::wcscmp(argv[1], L"--warp")) return 2;
   if (!ref.init(hardware ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_WARP, triangle || coverage)) return 3;
   if (coverage) {
      const bool ok = color ? public_color_coverage(ref.dev.Get(), ref.ctx.Get()) :
                              public_coverage(ref.dev.Get(), ref.ctx.Get(), sweep);
      std::printf("complete\t%d\n", ok);
      return ok ? 0 : 4;
   }
   if (triangle) {
      const bool ok = ref.triangle(hardware);
      std::printf("complete\t%d\n", ok);
      return ok ? 0 : 4;
   }
   bool pass = true;
   for (UINT i = 0; i < std::size(scenes); ++i) {
      if ((ref.lineStream || ref.quadLines) && scenes[i].domain != Isoline) continue;
      pass = ref.scene(i) && pass;
   }
   std::printf("complete\t%d\n", pass);
   return pass ? 0 : 4;
}
