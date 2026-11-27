/* SPDX-License-Identifier: MIT */
#pragma once
#include "coverage_color_tile.h"
#include <d3d11_1.h>
#include "forced_shaders.h"

static bool public_coverage(ID3D11Device *device, ID3D11DeviceContext *ctx, bool sweep)
{
   using Microsoft::WRL::ComPtr;
   ComPtr<ID3D11Device1> dev;
   ComPtr<ID3D11VertexShader> vs;
   ComPtr<ID3D11PixelShader> ps;
   if (!success("device1", device->QueryInterface(IID_PPV_ARGS(&dev))) ||
       !success("coverage VS", dev->CreateVertexShader(sweep ? coverage_sweep_vs : coverage_vs,
            sweep ? sizeof(coverage_sweep_vs) : sizeof(coverage_vs), nullptr, &vs)) ||
       !success("coverage PS", dev->CreatePixelShader(coverage_ps, sizeof(coverage_ps), nullptr, &ps))) return false;
   constexpr UINT guard = 0xdecafbad, prefix = 4, pixels = 32, words = prefix + pixels + 4;
   UINT initial[words];
   for (auto &word : initial) word = guard;
   D3D11_BUFFER_DESC b{};
   b.ByteWidth = sizeof(initial); b.Usage = D3D11_USAGE_DEFAULT;
   b.BindFlags = D3D11_BIND_UNORDERED_ACCESS; b.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
   D3D11_SUBRESOURCE_DATA data{initial, 0, 0};
   ComPtr<ID3D11Buffer> buffer, staging;
   if (!success("raw output", dev->CreateBuffer(&b, &data, &buffer))) return false;
   b.Usage = D3D11_USAGE_STAGING; b.CPUAccessFlags = D3D11_CPU_ACCESS_READ; b.BindFlags = b.MiscFlags = 0;
   if (!success("staging", dev->CreateBuffer(&b, nullptr, &staging))) return false;
   D3D11_UNORDERED_ACCESS_VIEW_DESC u{};
   u.Format = DXGI_FORMAT_R32_TYPELESS; u.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
   u.Buffer.FirstElement = prefix; u.Buffer.NumElements = pixels; u.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
   ComPtr<ID3D11UnorderedAccessView> view;
   if (!success("raw UAV", dev->CreateUnorderedAccessView(buffer.Get(), &u, &view))) return false;
   D3D11_VIEWPORT vp{0, 0, 64, 64, 0, 1};
   ctx->RSSetViewports(1, &vp);
   ctx->VSSetShader(vs.Get(), nullptr, 0); ctx->PSSetShader(ps.Get(), nullptr, 0);
   ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
   D3D11_DEPTH_STENCIL_DESC dd{}; dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
   ComPtr<ID3D11DepthStencilState> depth;
   if (!success("depth disabled", dev->CreateDepthStencilState(&dd, &depth))) return false;
   ctx->OMSetDepthStencilState(depth.Get(), 0);
   const int x16[] = {1,-1,-3,4,-5,2,5,3,-2,0,-4,-6,-8,7,6,-7};
   UINT edge16 = 0;
   for (UINT i = 0; i < 16; ++i) if (2 * x16[i] < 5) edge16 |= 1u << i;
   bool ok = true;
   struct Trial { UINT forced, edge, mask; };
   const Trial trials[] = {{0,1,~0u},{4,0xd,~0u},{8,0x3b,0},{16,edge16,0},{1,1,0x80000000u}};
   ComPtr<ID3D11Buffer> parameters;
   if (sweep) {
      D3D11_BUFFER_DESC cb{};
      cb.ByteWidth = 16; cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
      if (!success("shape constants", dev->CreateBuffer(&cb, nullptr, &parameters))) return false;
      ctx->VSSetConstantBuffers(0, 1, parameters.GetAddressOf());
   }
   const int y16[] = {1,-3,2,-1,-2,5,3,-5,6,-7,-6,4,0,-4,7,-8};
   for (UINT shape = 0; shape < (sweep ? 33u : UINT(std::size(trials))); ++shape) {
      const bool horizontal = sweep && shape > 16;
      Trial trial = sweep ? Trial{16, 0, 0} : trials[shape];
      if (sweep) {
         const UINT constants[4] = {shape, 0, 0, 0};
         ctx->UpdateSubresource(parameters.Get(), 0, nullptr, constants, 0, 0);
         const int step = shape ? int((shape - 1) % 16) : 10;
         for (UINT sample = 0; sample < 16; ++sample)
            if (2 * (horizontal ? y16[sample] : x16[sample]) < 2 * step - 15)
               trial.edge |= 1u << sample;
      }
      D3D11_RASTERIZER_DESC1 rd{};
      rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE;
      rd.DepthClipEnable = rd.MultisampleEnable = TRUE;
      rd.ForcedSampleCount = trial.forced;
      ComPtr<ID3D11RasterizerState1> rs;
      if (!success("forced rasterizer", dev->CreateRasterizerState1(&rd, &rs))) return false;
      ctx->RSSetState(rs.Get()); ctx->OMSetBlendState(nullptr, nullptr, trial.mask);
      ctx->UpdateSubresource(buffer.Get(), 0, nullptr, initial, 0, 0);
      ctx->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, 1, view.GetAddressOf(), nullptr);
      ctx->Draw(3, 0);
      ID3D11UnorderedAccessView *empty = nullptr;
      ctx->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, 1, &empty, nullptr);
      ctx->CopyResource(staging.Get(), buffer.Get());
      D3D11_MAPPED_SUBRESOURCE map{};
      if (!success("coverage map", ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map))) return false;
      const UINT *values = static_cast<const UINT *>(map.pData);
      const UINT count = trial.forced ? trial.forced : 1;
      UINT matches = 0, edgeMatches = 0;
      for (UINT i = 0; i < words; ++i) {
         const UINT coordinate = horizontal ? (i - prefix) / 8 : (i - prefix) % 8;
         const UINT edge = horizontal ? 2 : 3;
         UINT wanted = guard;
         if (i >= prefix && i < prefix + pixels && coordinate <= edge) {
            const UINT mask = coordinate == edge ? trial.edge : (1u << count) - 1;
            if (mask) wanted = (1u << 16) | mask;
            if (coordinate == edge) edgeMatches += values[i] == wanted;
         }
         if (values[i] != wanted) std::printf("coverage-mismatch\t%u\t%u\t%08x\t%08x\n", trial.forced, i, values[i], wanted);
         matches += values[i] == wanted;
      }
      std::printf("public-coverage\t%u\t%08x\t%08x\t%08x\t%u\t%u\n", trial.forced, trial.mask,
                  values[prefix + 9], values[prefix + 11], matches, edgeMatches);
      if (sweep) {
         std::printf("coverage16-tile\t%u\t", shape);
         for (UINT i = prefix; i < prefix + pixels; ++i)
            std::printf("%04x", values[i] == guard ? 0u : values[i] & 0xffff);
         std::puts("");
      }
      ctx->Unmap(staging.Get(), 0);
      ok &= matches == words;
   }
   ctx->ClearState(); ctx->Flush();
   return ok && success("coverage device status", dev->GetDeviceRemovedReason());
}

static bool public_color_coverage(ID3D11Device *device, ID3D11DeviceContext *ctx)
{
   using Microsoft::WRL::ComPtr;
   ComPtr<ID3D11Device1> dev;
   ComPtr<ID3D11VertexShader> vs;
   ComPtr<ID3D11PixelShader> ps;
   if (!success("device1", device->QueryInterface(IID_PPV_ARGS(&dev))) ||
       !success("mask VS", dev->CreateVertexShader(coverage_sweep_vs, sizeof(coverage_sweep_vs), nullptr, &vs)) ||
       !success("mask PS", dev->CreatePixelShader(coverage_color_ps, sizeof(coverage_color_ps), nullptr, &ps)))
      return false;

   D3D11_TEXTURE2D_DESC td{};
   td.Width = td.Height = 64;
   td.ArraySize = td.MipLevels = td.SampleDesc.Count = 1;
   td.Format = DXGI_FORMAT_R16_UINT;
   td.BindFlags = D3D11_BIND_RENDER_TARGET;
   ComPtr<ID3D11Texture2D> target, staging;
   ComPtr<ID3D11RenderTargetView> rtv;
   if (!success("mask target", dev->CreateTexture2D(&td, nullptr, &target)) ||
       !success("mask RTV", dev->CreateRenderTargetView(target.Get(), nullptr, &rtv))) return false;
   td.BindFlags = 0; td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
   if (!success("mask readback texture", dev->CreateTexture2D(&td, nullptr, &staging))) return false;

   D3D11_RASTERIZER_DESC1 rd{};
   rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE;
   rd.DepthClipEnable = rd.MultisampleEnable = TRUE; rd.ForcedSampleCount = 16;
   ComPtr<ID3D11RasterizerState1> raster;
   D3D11_DEPTH_STENCIL_DESC dd{}; dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
   ComPtr<ID3D11DepthStencilState> depth;
   D3D11_BUFFER_DESC cb{}; cb.ByteWidth = 16; cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
   ComPtr<ID3D11Buffer> params;
   if (!success("mask rasterizer", dev->CreateRasterizerState1(&rd, &raster)) ||
       !success("mask depth disabled", dev->CreateDepthStencilState(&dd, &depth)) ||
       !success("shape buffer", dev->CreateBuffer(&cb, nullptr, &params))) return false;
   ComPtr<ID3D11BlendState1> blend[2];
   for (UINT i = 0; i < 2; ++i) {
      D3D11_BLEND_DESC1 b{};
      auto &rt = b.RenderTarget[0];
      rt.SrcBlend = rt.SrcBlendAlpha = D3D11_BLEND_ONE;
      rt.DestBlend = rt.DestBlendAlpha = D3D11_BLEND_ZERO;
      rt.BlendOp = rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
      rt.LogicOpEnable = TRUE; rt.LogicOp = i ? D3D11_LOGIC_OP_XOR : D3D11_LOGIC_OP_OR;
      rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
      if (!success("mask blend", dev->CreateBlendState1(&b, &blend[i]))) return false;
   }
   const FLOAT seed[4] = {float(color_coverage_seed), 0, 0, 0};
   ctx->ClearRenderTargetView(rtv.Get(), seed);
   ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
   ctx->OMSetDepthStencilState(depth.Get(), 0);
   const D3D11_VIEWPORT vp{0,0,64,64,0,1};
   ctx->RSSetViewports(1, &vp); ctx->RSSetState(raster.Get());
   ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
   ctx->VSSetShader(vs.Get(), nullptr, 0); ctx->PSSetShader(ps.Get(), nullptr, 0);
   ctx->VSSetConstantBuffers(0, 1, params.GetAddressOf());
   for (UINT i = 0; i < std::size(color_coverage_shapes); ++i) {
      const UINT constants[4] = {color_coverage_shapes[i], 0, 0, 0};
      ctx->UpdateSubresource(params.Get(), 0, nullptr, constants, 0, 0);
      ctx->OMSetBlendState(blend[i == 1].Get(), nullptr, ~0u);
      ctx->Draw(3, 0);
   }
   ctx->OMSetRenderTargets(0, nullptr, nullptr);
   ctx->CopyResource(staging.Get(), target.Get());
   D3D11_MAPPED_SUBRESOURCE map{};
   if (!success("mask map", ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map))) return false;
   const bool pixelsMatch = check_color_coverage_tile(map.pData, map.RowPitch);
   ctx->Unmap(staging.Get(), 0);
   ctx->ClearState(); ctx->Flush();
   return success("mask device status", dev->GetDeviceRemovedReason()) && pixelsMatch;
}
