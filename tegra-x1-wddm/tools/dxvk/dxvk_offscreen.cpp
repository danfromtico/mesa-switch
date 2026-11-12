// SPDX-License-Identifier: MIT
// Offscreen Direct3D 9/10/11 smoke test for a DXVK -> Vulkan driver stack.
//
// It renders into a 64x64 offscreen target and compares CPU readback; it
// creates no visible window content and never presents. By default the
// Direct3D DLLs are loaded by absolute path from this executable's
// directory, so a pass cannot come from the Microsoft runtime in System32.
// `--system` loads the system DLLs instead and is a control for the test
// itself, not a DXVK result; it refuses to run beside Direct3D DLLs.
//
// With `d3d11 --indirect`, the test exercises all four pre-Turing indirect
// submission paths through DXVK:
//   - DrawInstancedIndirect        (vkCmdDrawIndirect)
//   - DrawIndexedInstancedIndirect (vkCmdDrawIndexedIndirect)
//   - Stream-Output + DrawAuto     (vkCmdBeginTransformFeedbackEXT +
//                                   vkCmdDrawIndirectByteCountEXT)
//   - DispatchIndirect             (vkCmdDispatchIndirect + UAV readback)
//
// A pass shows that this fixed sequence rendered correctly on the machine
// that ran it. It says nothing about presentation, other features or games.
#include <windows.h>
#include <d3d9.h>
#include <d3d10.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>

namespace {

enum Exit {
  ExitPass = 0,
  ExitUsage = 1,
  ExitModule = 2,
  ExitDevice = 3,
  ExitCompiler = 4,
  ExitResource = 5,
  ExitClear = 10,
  ExitDraw = 11,
  ExitIndirect = 12,
  ExitIndirectIndexed = 13,
  ExitDrawAuto = 14,
  ExitIndirectDispatch = 15,
};

const UINT Size = 64;
// Clear color and its 8-bit UNORM encoding. 0.5 * 255 = 127.5 may round
// either way, hence the tolerance of one.
const float ClearRgba[4] = {0.25f, 0.5f, 0.75f, 1.0f};
const uint8_t ClearBytes[4] = {64, 128, 191, 255};
const uint8_t DrawBytes[4] = {255, 0, 0, 255};

template<typename T> struct Com {
  T* p = nullptr;
  Com() = default;
  Com(const Com&) = delete;
  Com& operator=(const Com&) = delete;
  ~Com() { if (p) p->Release(); }
  T* operator->() const { return p; }
  T** operator&() { return &p; }
  operator T*() const { return p; }
};

bool g_system = false;

std::wstring exeDirectory() {
  wchar_t path[MAX_PATH];
  DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
  if (!n || n == MAX_PATH)
    return L"";
  std::wstring dir(path, n);
  return dir.substr(0, dir.find_last_of(L'\\') + 1);
}

std::wstring systemDirectory() {
  wchar_t path[MAX_PATH];
  UINT n = GetSystemDirectoryW(path, MAX_PATH);
  return n && n < MAX_PATH ? std::wstring(path, n) + L"\\" : L"";
}

// Loads `name` by absolute path from the executable directory or, with
// --system, from the system directory; dependencies resolve beside it
// first. Never uses the default search order, which would let a DLL next to
// the executable answer for the system runtime. Prints the path actually
// loaded so the log proves which implementation ran.
HMODULE loadApi(const wchar_t* name) {
  const std::wstring path = (g_system ? systemDirectory() : exeDirectory()) + name;
  HMODULE module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!module) {
    printf("MODULE %ls: load failed, error %lu\n", name, GetLastError());
    return nullptr;
  }
  wchar_t loaded[MAX_PATH] = L"";
  GetModuleFileNameW(module, loaded, MAX_PATH);
  printf("MODULE %ls: %ls\n", name, loaded);
  return module;
}

bool withinOne(uint8_t a, uint8_t b) { return a > b ? a - b <= 1 : b - a <= 1; }

// `pixels` is RGBA byte order after the caller's swizzle.
bool pixelIs(const uint8_t* rgba, const uint8_t expected[4]) {
  return withinOne(rgba[0], expected[0]) && withinOne(rgba[1], expected[1]) &&
         withinOne(rgba[2], expected[2]) && withinOne(rgba[3], expected[3]);
}

struct Readback {
  uint8_t rgba[Size][Size][4];
};

// Copies a mapped surface into RGBA order. `bgra` selects D3D9's A8R8G8B8.
void capture(Readback& out, const void* data, UINT pitch, bool bgra) {
  for (UINT y = 0; y < Size; y++) {
    const uint8_t* row = static_cast<const uint8_t*>(data) + size_t(y) * pitch;
    for (UINT x = 0; x < Size; x++) {
      const uint8_t* p = row + 4 * x;
      out.rgba[y][x][0] = bgra ? p[2] : p[0];
      out.rgba[y][x][1] = p[1];
      out.rgba[y][x][2] = bgra ? p[0] : p[2];
      out.rgba[y][x][3] = p[3];
    }
  }
}

bool report(const char* stage, bool pass, const Readback& r, UINT x, UINT y) {
  if (pass) {
    printf("STAGE %s: PASS\n", stage);
  } else {
    const uint8_t* p = r.rgba[y][x];
    printf("STAGE %s: FAIL first mismatch at (%u,%u) = %u,%u,%u,%u\n", stage,
           x, y, p[0], p[1], p[2], p[3]);
  }
  return pass;
}

bool checkClear(const char* stage, const Readback& r) {
  for (UINT y = 0; y < Size; y++)
    for (UINT x = 0; x < Size; x++)
      if (!pixelIs(r.rgba[y][x], ClearBytes))
        return report(stage, false, r, x, y);
  return report(stage, true, r, 0, 0);
}

// The triangle (0,0) (64,0) (0,64) covers x + y < 64. Pixels within two of
// the diagonal are skipped so the result does not depend on edge rules.
bool checkTriangle(const char* stage, const Readback& r) {
  for (UINT y = 0; y < Size; y++) {
    for (UINT x = 0; x < Size; x++) {
      const int distance = int(x + y) - int(Size - 1);
      if (distance >= -2 && distance <= 2)
        continue;
      if (!pixelIs(r.rgba[y][x], distance < 0 ? DrawBytes : ClearBytes))
        return report(stage, false, r, x, y);
    }
  }
  return report(stage, true, r, 0, 0);
}

// ---- shaders for Direct3D 10/11 -------------------------------------------

const char VertexSource[] =
  "float4 main(float2 position : POSITION) : SV_Position {\n"
  "  return float4(position, 0.0, 1.0);\n"
  "}\n";
const char StreamOutPassVsSource[] =
  "float4 main(float4 position : POSITION) : SV_Position {\n"
  "  return position;\n"
  "}\n";
// The color comes from a constant buffer so the test covers uniform data.
const char PixelSource[] =
  "cbuffer Color : register(b0) { float4 color; };\n"
  "float4 main() : SV_Target { return color; }\n";
const char ComputeSource[] =
  "RWStructuredBuffer<uint> outBuf : register(u0);\n"
  "[numthreads(4, 1, 1)]\n"
  "void main(uint3 tid : SV_DispatchThreadID) {\n"
  "  outBuf[tid.x] = 0xA5000000u | (tid.x * 17u + 3u);\n"
  "}\n";

// Clockwise in a y-down target: top-left, top-right, bottom-left.
const float TriangleNdc[6] = {-1.0f, 1.0f, 1.0f, 1.0f, -1.0f, -1.0f};
const float DrawRgba[4] = {1.0f, 0.0f, 0.0f, 1.0f};

typedef HRESULT (WINAPI *CompileFn)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*,
  ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);

CompileFn loadCompiler() {
  HMODULE module = LoadLibraryW(L"d3dcompiler_47.dll");
  CompileFn compile = module ?
    reinterpret_cast<CompileFn>(reinterpret_cast<void*>(GetProcAddress(module, "D3DCompile"))) : nullptr;
  if (!compile)
    printf("STAGE compile: UNAVAILABLE d3dcompiler_47.dll, error %lu\n", GetLastError());
  return compile;
}

bool compileBlob(CompileFn compile, const char* src, size_t len, const char* name,
                 const char* profile, ID3DBlob** out) {
  Com<ID3DBlob> errors;
  HRESULT hr = compile(src, len, name, nullptr, nullptr, "main", profile, 0, 0, out, &errors);
  if (FAILED(hr)) {
    printf("STAGE compile (%s): FAIL hr=0x%08lx %s\n", name, static_cast<unsigned long>(hr),
           errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
    return false;
  }
  return true;
}

// HLSL is compiled by the system compiler at run time. Its absence is a
// distinct result, not a rendering failure.
bool compileShaders(ID3DBlob** vs, ID3DBlob** ps, CompileFn* outCompile = nullptr) {
  CompileFn compile = loadCompiler();
  if (!compile)
    return false;
  if (!compileBlob(compile, VertexSource, sizeof(VertexSource) - 1, "vs", "vs_4_0", vs) ||
      !compileBlob(compile, PixelSource, sizeof(PixelSource) - 1, "ps", "ps_4_0", ps))
    return false;
  if (outCompile)
    *outCompile = compile;
  printf("STAGE compile: PASS\n");
  return true;
}

#define REQUIRE(call) do { HRESULT hr_ = (call); if (FAILED(hr_)) { \
  printf("RESOURCE %s: hr=0x%08lx\n", #call, static_cast<unsigned long>(hr_)); \
  return ExitResource; } } while (0)

// ---- Direct3D 11 -----------------------------------------------------------

int runD3D11(bool indirect) {
  HMODULE module = loadApi(L"d3d11.dll");
  if (!module)
    return ExitModule;
  typedef HRESULT (WINAPI *CreateFn)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
    const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
  CreateFn create = reinterpret_cast<CreateFn>(
    reinterpret_cast<void*>(GetProcAddress(module, "D3D11CreateDevice")));
  if (!create)
    return ExitModule;

  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
  Com<ID3D11Device> device;
  Com<ID3D11DeviceContext> context;
  D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL(0);
  HRESULT hr = create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 3,
                      D3D11_SDK_VERSION, &device, &level, &context);
  if (FAILED(hr)) {
    printf("DEVICE d3d11: FAIL hr=0x%08lx\n", static_cast<unsigned long>(hr));
    return ExitDevice;
  }
  Com<IDXGIDevice> dxgiDevice;
  Com<IDXGIAdapter> adapter;
  DXGI_ADAPTER_DESC desc = {};
  if (SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice))) &&
      SUCCEEDED(dxgiDevice->GetAdapter(&adapter)))
    adapter->GetDesc(&desc);
  printf("DEVICE d3d11: feature level 0x%x, adapter \"%ls\" %04x:%04x\n",
         unsigned(level), desc.Description, desc.VendorId, desc.DeviceId);

  D3D11_TEXTURE2D_DESC td = {};
  td.Width = td.Height = Size;
  td.MipLevels = td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_RENDER_TARGET;
  Com<ID3D11Texture2D> target, staging;
  Com<ID3D11RenderTargetView> view;
  REQUIRE(device->CreateTexture2D(&td, nullptr, &target));
  REQUIRE(device->CreateRenderTargetView(target, nullptr, &view));
  td.Usage = D3D11_USAGE_STAGING;
  td.BindFlags = 0;
  td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  REQUIRE(device->CreateTexture2D(&td, nullptr, &staging));

  Readback readback;
  auto read = [&]() -> HRESULT {
    context->CopyResource(staging, target);
    D3D11_MAPPED_SUBRESOURCE map;
    HRESULT result = context->Map(staging, 0, D3D11_MAP_READ, 0, &map);
    if (SUCCEEDED(result)) {
      capture(readback, map.pData, map.RowPitch, false);
      context->Unmap(staging, 0);
    }
    return result;
  };

  context->ClearRenderTargetView(view, ClearRgba);
  REQUIRE(read());
  if (!checkClear("d3d11 clear", readback))
    return ExitClear;

  Com<ID3DBlob> vsBlob, psBlob;
  CompileFn compile = nullptr;
  if (!compileShaders(&vsBlob, &psBlob, &compile))
    return ExitCompiler;
  Com<ID3D11VertexShader> vs;
  Com<ID3D11PixelShader> ps;
  Com<ID3D11InputLayout> layout;
  Com<ID3D11Buffer> vertices, constants;
  REQUIRE(device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vs));
  REQUIRE(device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &ps));
  const D3D11_INPUT_ELEMENT_DESC element = {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
                                            D3D11_INPUT_PER_VERTEX_DATA, 0};
  REQUIRE(device->CreateInputLayout(&element, 1, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &layout));
  D3D11_BUFFER_DESC bd = {};
  bd.ByteWidth = sizeof(TriangleNdc);
  bd.Usage = D3D11_USAGE_IMMUTABLE;
  bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
  D3D11_SUBRESOURCE_DATA init = {TriangleNdc, 0, 0};
  REQUIRE(device->CreateBuffer(&bd, &init, &vertices));
  bd.ByteWidth = sizeof(DrawRgba);
  bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  init.pSysMem = DrawRgba;
  REQUIRE(device->CreateBuffer(&bd, &init, &constants));

  const UINT stride = 2 * sizeof(float), offset = 0;
  const D3D11_VIEWPORT viewport = {0.0f, 0.0f, float(Size), float(Size), 0.0f, 1.0f};
  ID3D11RenderTargetView* views[] = {view};
  ID3D11Buffer* vbs[] = {vertices};
  ID3D11Buffer* cbs[] = {constants};
  context->OMSetRenderTargets(1, views, nullptr);
  context->RSSetViewports(1, &viewport);
  context->IASetInputLayout(layout);
  context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  context->IASetVertexBuffers(0, 1, vbs, &stride, &offset);
  context->VSSetShader(vs, nullptr, 0);
  context->PSSetShader(ps, nullptr, 0);
  context->PSSetConstantBuffers(0, 1, cbs);
  context->Draw(3, 0);
  REQUIRE(read());
  if (!checkTriangle("d3d11 draw", readback))
    return ExitDraw;

  if (indirect) {
    if (level < D3D_FEATURE_LEVEL_11_0) {
      printf("STAGE d3d11 indirect: UNAVAILABLE below feature level 11_0\n");
      return ExitIndirect;
    }

    // 1. DrawInstancedIndirect (vkCmdDrawIndirect -> NVK_MME_DRAW_INDIRECT)
    const UINT drawArgs[4] = {3, 1, 0, 0};
    Com<ID3D11Buffer> argBuffer;
    D3D11_BUFFER_DESC argDesc = {};
    argDesc.ByteWidth = sizeof(drawArgs);
    argDesc.Usage = D3D11_USAGE_IMMUTABLE;
    argDesc.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
    init.pSysMem = drawArgs;
    REQUIRE(device->CreateBuffer(&argDesc, &init, &argBuffer));
    context->ClearRenderTargetView(view, ClearRgba);
    context->DrawInstancedIndirect(argBuffer, 0);
    hr = read();
    HRESULT removed = device->GetDeviceRemovedReason();
    if (FAILED(hr) || FAILED(removed)) {
      printf("STAGE d3d11 indirect draw: FAIL readback hr=0x%08lx device removed reason=0x%08lx\n",
             static_cast<unsigned long>(hr), static_cast<unsigned long>(removed));
      return ExitIndirect;
    }
    if (!checkTriangle("d3d11 indirect draw", readback))
      return ExitIndirect;

    // 2. DrawIndexedInstancedIndirect (vkCmdDrawIndexedIndirect -> NVK_MME_DRAW_INDEXED_INDIRECT)
    const uint16_t indices[4] = {0, 1, 2, 0};
    Com<ID3D11Buffer> indexBuffer;
    D3D11_BUFFER_DESC ibDesc = {};
    ibDesc.ByteWidth = sizeof(indices);
    ibDesc.Usage = D3D11_USAGE_IMMUTABLE;
    ibDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;
    init.pSysMem = indices;
    REQUIRE(device->CreateBuffer(&ibDesc, &init, &indexBuffer));

    const UINT indexedArgs[5] = {3, 1, 0, 0, 0};
    Com<ID3D11Buffer> indexedArgBuffer;
    argDesc.ByteWidth = sizeof(indexedArgs);
    init.pSysMem = indexedArgs;
    REQUIRE(device->CreateBuffer(&argDesc, &init, &indexedArgBuffer));

    context->IASetIndexBuffer(indexBuffer, DXGI_FORMAT_R16_UINT, 0);
    context->ClearRenderTargetView(view, ClearRgba);
    context->DrawIndexedInstancedIndirect(indexedArgBuffer, 0);
    hr = read();
    removed = device->GetDeviceRemovedReason();
    if (FAILED(hr) || FAILED(removed)) {
      printf("STAGE d3d11 indirect indexed draw: FAIL readback hr=0x%08lx device removed reason=0x%08lx\n",
             static_cast<unsigned long>(hr), static_cast<unsigned long>(removed));
      return ExitIndirectIndexed;
    }
    if (!checkTriangle("d3d11 indirect indexed draw", readback))
      return ExitIndirectIndexed;

    // 3. Stream-Output + DrawAuto (vkCmdBeginTransformFeedbackEXT + vkCmdDrawIndirectByteCountEXT)
    Com<ID3DBlob> soVsBlob;
    if (!compileBlob(compile, StreamOutPassVsSource, sizeof(StreamOutPassVsSource) - 1, "so_vs", "vs_4_0", &soVsBlob))
      return ExitCompiler;
    Com<ID3D11VertexShader> soVs;
    Com<ID3D11InputLayout> soLayout;
    Com<ID3D11GeometryShader> soGs;
    Com<ID3D11Buffer> soBuffer;
    REQUIRE(device->CreateVertexShader(soVsBlob->GetBufferPointer(), soVsBlob->GetBufferSize(), nullptr, &soVs));
    const D3D11_INPUT_ELEMENT_DESC soElement = {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,
                                                D3D11_INPUT_PER_VERTEX_DATA, 0};
    REQUIRE(device->CreateInputLayout(&soElement, 1, soVsBlob->GetBufferPointer(), soVsBlob->GetBufferSize(), &soLayout));
    const D3D11_SO_DECLARATION_ENTRY soDecl = {0, "SV_Position", 0, 0, 4, 0};
    const UINT soStride = 4 * sizeof(float);
    REQUIRE(device->CreateGeometryShaderWithStreamOutput(
      vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
      &soDecl, 1, &soStride, 1, D3D11_SO_NO_RASTERIZED_STREAM, nullptr, &soGs));
    D3D11_BUFFER_DESC soDesc = {};
    soDesc.ByteWidth = 3 * soStride;
    soDesc.Usage = D3D11_USAGE_DEFAULT;
    soDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_STREAM_OUTPUT;
    REQUIRE(device->CreateBuffer(&soDesc, nullptr, &soBuffer));

    ID3D11Buffer* soTargets[] = {soBuffer};
    const UINT soOffsets[] = {0};
    context->OMSetRenderTargets(0, nullptr, nullptr);
    context->IASetInputLayout(layout);
    context->IASetVertexBuffers(0, 1, vbs, &stride, &offset);
    context->VSSetShader(vs, nullptr, 0);
    context->GSSetShader(soGs, nullptr, 0);
    context->PSSetShader(nullptr, nullptr, 0);
    context->SOSetTargets(1, soTargets, soOffsets);
    context->Draw(3, 0);

    ID3D11Buffer* nullSo[] = {nullptr};
    context->SOSetTargets(1, nullSo, soOffsets);
    context->GSSetShader(nullptr, nullptr, 0);
    context->OMSetRenderTargets(1, views, nullptr);
    context->ClearRenderTargetView(view, ClearRgba);
    context->IASetInputLayout(soLayout);
    context->IASetVertexBuffers(0, 1, soTargets, &soStride, &offset);
    context->VSSetShader(soVs, nullptr, 0);
    context->PSSetShader(ps, nullptr, 0);
    context->DrawAuto();
    hr = read();
    removed = device->GetDeviceRemovedReason();
    if (FAILED(hr) || FAILED(removed)) {
      printf("STAGE d3d11 drawauto: FAIL readback hr=0x%08lx device removed reason=0x%08lx\n",
             static_cast<unsigned long>(hr), static_cast<unsigned long>(removed));
      return ExitDrawAuto;
    }
    if (!checkTriangle("d3d11 drawauto", readback))
      return ExitDrawAuto;

    // 4. DispatchIndirect (vkCmdDispatchIndirect -> NVK_MME_DISPATCH_INDIRECT)
    Com<ID3DBlob> csBlob;
    if (!compileBlob(compile, ComputeSource, sizeof(ComputeSource) - 1, "cs", "cs_5_0", &csBlob))
      return ExitCompiler;
    Com<ID3D11ComputeShader> cs;
    REQUIRE(device->CreateComputeShader(csBlob->GetBufferPointer(), csBlob->GetBufferSize(), nullptr, &cs));

    const UINT kThreads = 16;
    D3D11_BUFFER_DESC uavBufDesc = {};
    uavBufDesc.ByteWidth = kThreads * sizeof(uint32_t);
    uavBufDesc.Usage = D3D11_USAGE_DEFAULT;
    uavBufDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    uavBufDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    uavBufDesc.StructureByteStride = sizeof(uint32_t);
    Com<ID3D11Buffer> csOutBuffer, csStagingBuffer, csArgBuffer;
    REQUIRE(device->CreateBuffer(&uavBufDesc, nullptr, &csOutBuffer));

    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = DXGI_FORMAT_UNKNOWN;
    uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.NumElements = kThreads;
    Com<ID3D11UnorderedAccessView> uav;
    REQUIRE(device->CreateUnorderedAccessView(csOutBuffer, &uavDesc, &uav));

    D3D11_BUFFER_DESC csStageDesc = {};
    csStageDesc.ByteWidth = kThreads * sizeof(uint32_t);
    csStageDesc.Usage = D3D11_USAGE_STAGING;
    csStageDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    REQUIRE(device->CreateBuffer(&csStageDesc, nullptr, &csStagingBuffer));

    const UINT csArgs[3] = {kThreads / 4, 1, 1};
    argDesc.ByteWidth = sizeof(csArgs);
    init.pSysMem = csArgs;
    REQUIRE(device->CreateBuffer(&argDesc, &init, &csArgBuffer));

    ID3D11UnorderedAccessView* uavs[] = {uav};
    context->CSSetShader(cs, nullptr, 0);
    context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
    context->DispatchIndirect(csArgBuffer, 0);
    context->CopyResource(csStagingBuffer, csOutBuffer);
    D3D11_MAPPED_SUBRESOURCE csMap = {};
    hr = context->Map(csStagingBuffer, 0, D3D11_MAP_READ, 0, &csMap);
    removed = device->GetDeviceRemovedReason();
    if (FAILED(hr) || FAILED(removed)) {
      printf("STAGE d3d11 indirect dispatch: FAIL readback hr=0x%08lx device removed reason=0x%08lx\n",
             static_cast<unsigned long>(hr), static_cast<unsigned long>(removed));
      return ExitIndirectDispatch;
    }
    const uint32_t* csWords = static_cast<const uint32_t*>(csMap.pData);
    bool csOk = true;
    UINT badIdx = 0;
    uint32_t gotWord = 0, expWord = 0;
    for (UINT i = 0; i < kThreads; i++) {
      const uint32_t expected = 0xA5000000u | (i * 17u + 3u);
      if (csWords[i] != expected) {
        csOk = false;
        badIdx = i;
        gotWord = csWords[i];
        expWord = expected;
        break;
      }
    }
    context->Unmap(csStagingBuffer, 0);
    if (!csOk) {
      printf("STAGE d3d11 indirect dispatch: FAIL index %u = 0x%08x (expected 0x%08x)\n",
             badIdx, gotWord, expWord);
      return ExitIndirectDispatch;
    }
    printf("STAGE d3d11 indirect dispatch: PASS\n");
  }
  return ExitPass;
}

// ---- Direct3D 10 -----------------------------------------------------------

int runD3D10() {
  Com<ID3D10Device> device;
  HRESULT hr;
  if (g_system) {
    HMODULE module = loadApi(L"d3d10.dll");
    typedef HRESULT (WINAPI *CreateFn)(IDXGIAdapter*, D3D10_DRIVER_TYPE, HMODULE, UINT, UINT, ID3D10Device**);
    CreateFn create = module ? reinterpret_cast<CreateFn>(
      reinterpret_cast<void*>(GetProcAddress(module, "D3D10CreateDevice"))) : nullptr;
    if (!create)
      return ExitModule;
    hr = create(nullptr, D3D10_DRIVER_TYPE_HARDWARE, nullptr, 0, D3D10_SDK_VERSION, &device);
  } else {
    // DXVK implements Direct3D 10 in d3d10core.dll on top of its d3d11.dll.
    // Calling its entry point directly keeps the system d3d10.dll, and its
    // own choice of core DLL, out of the result.
    HMODULE dxgi = loadApi(L"dxgi.dll");
    HMODULE core = loadApi(L"d3d10core.dll");
    typedef HRESULT (WINAPI *FactoryFn)(REFIID, void**);
    typedef HRESULT (WINAPI *CreateFn)(IDXGIFactory*, IDXGIAdapter*, UINT, D3D_FEATURE_LEVEL, ID3D10Device**);
    FactoryFn createFactory = dxgi ? reinterpret_cast<FactoryFn>(
      reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory1"))) : nullptr;
    CreateFn create = core ? reinterpret_cast<CreateFn>(
      reinterpret_cast<void*>(GetProcAddress(core, "D3D10CoreCreateDevice"))) : nullptr;
    if (!createFactory || !create)
      return ExitModule;
    Com<IDXGIFactory> factory;
    Com<IDXGIAdapter> adapter;
    hr = createFactory(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&factory));
    if (SUCCEEDED(hr))
      hr = factory->EnumAdapters(0, &adapter);
    if (SUCCEEDED(hr))
      hr = create(factory, adapter, 0, D3D_FEATURE_LEVEL_10_0, &device);
  }
  if (FAILED(hr)) {
    printf("DEVICE d3d10: FAIL hr=0x%08lx\n", static_cast<unsigned long>(hr));
    return ExitDevice;
  }
  Com<IDXGIDevice> dxgiDevice;
  Com<IDXGIAdapter> adapter;
  DXGI_ADAPTER_DESC desc = {};
  if (SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice))) &&
      SUCCEEDED(dxgiDevice->GetAdapter(&adapter)))
    adapter->GetDesc(&desc);
  printf("DEVICE d3d10: adapter \"%ls\" %04x:%04x\n", desc.Description, desc.VendorId, desc.DeviceId);

  D3D10_TEXTURE2D_DESC td = {};
  td.Width = td.Height = Size;
  td.MipLevels = td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D10_USAGE_DEFAULT;
  td.BindFlags = D3D10_BIND_RENDER_TARGET;
  Com<ID3D10Texture2D> target, staging;
  Com<ID3D10RenderTargetView> view;
  REQUIRE(device->CreateTexture2D(&td, nullptr, &target));
  REQUIRE(device->CreateRenderTargetView(target, nullptr, &view));
  td.Usage = D3D10_USAGE_STAGING;
  td.BindFlags = 0;
  td.CPUAccessFlags = D3D10_CPU_ACCESS_READ;
  REQUIRE(device->CreateTexture2D(&td, nullptr, &staging));

  Readback readback;
  auto read = [&]() -> HRESULT {
    device->CopyResource(staging, target);
    D3D10_MAPPED_TEXTURE2D map;
    HRESULT result = staging->Map(0, D3D10_MAP_READ, 0, &map);
    if (SUCCEEDED(result)) {
      capture(readback, map.pData, map.RowPitch, false);
      staging->Unmap(0);
    }
    return result;
  };

  device->ClearRenderTargetView(view, ClearRgba);
  REQUIRE(read());
  if (!checkClear("d3d10 clear", readback))
    return ExitClear;

  Com<ID3DBlob> vsBlob, psBlob;
  if (!compileShaders(&vsBlob, &psBlob))
    return ExitCompiler;
  Com<ID3D10VertexShader> vs;
  Com<ID3D10PixelShader> ps;
  Com<ID3D10InputLayout> layout;
  Com<ID3D10Buffer> vertices, constants;
  REQUIRE(device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &vs));
  REQUIRE(device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), &ps));
  const D3D10_INPUT_ELEMENT_DESC element = {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
                                            D3D10_INPUT_PER_VERTEX_DATA, 0};
  REQUIRE(device->CreateInputLayout(&element, 1, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &layout));
  D3D10_BUFFER_DESC bd = {};
  bd.ByteWidth = sizeof(TriangleNdc);
  bd.Usage = D3D10_USAGE_IMMUTABLE;
  bd.BindFlags = D3D10_BIND_VERTEX_BUFFER;
  D3D10_SUBRESOURCE_DATA init = {TriangleNdc, 0, 0};
  REQUIRE(device->CreateBuffer(&bd, &init, &vertices));
  bd.ByteWidth = sizeof(DrawRgba);
  bd.BindFlags = D3D10_BIND_CONSTANT_BUFFER;
  init.pSysMem = DrawRgba;
  REQUIRE(device->CreateBuffer(&bd, &init, &constants));

  const UINT stride = 2 * sizeof(float), offset = 0;
  const D3D10_VIEWPORT viewport = {0, 0, Size, Size, 0.0f, 1.0f};
  ID3D10RenderTargetView* views[] = {view};
  ID3D10Buffer* vbs[] = {vertices};
  ID3D10Buffer* cbs[] = {constants};
  device->OMSetRenderTargets(1, views, nullptr);
  device->RSSetViewports(1, &viewport);
  device->IASetInputLayout(layout);
  device->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  device->IASetVertexBuffers(0, 1, vbs, &stride, &offset);
  device->VSSetShader(vs);
  device->PSSetShader(ps);
  device->PSSetConstantBuffers(0, 1, cbs);
  device->Draw(3, 0);
  REQUIRE(read());
  return checkTriangle("d3d10 draw", readback) ? ExitPass : ExitDraw;
}

// ---- Direct3D 9 ------------------------------------------------------------

struct WindowGuard {
  HWND hwnd = nullptr;
  ~WindowGuard() { if (hwnd) DestroyWindow(hwnd); }
};

int runD3D9() {
  HMODULE module = loadApi(L"d3d9.dll");
  typedef IDirect3D9* (WINAPI *CreateFn)(UINT);
  CreateFn create = module ? reinterpret_cast<CreateFn>(
    reinterpret_cast<void*>(GetProcAddress(module, "Direct3DCreate9"))) : nullptr;
  if (!create)
    return ExitModule;
  Com<IDirect3D9> d3d;
  d3d.p = create(D3D_SDK_VERSION);
  if (!d3d) {
    printf("DEVICE d3d9: FAIL Direct3DCreate9 returned null\n");
    return ExitDevice;
  }
  D3DADAPTER_IDENTIFIER9 id = {};
  d3d->GetAdapterIdentifier(D3DADAPTER_DEFAULT, 0, &id);

  // Direct3D 9 requires a device window and an implicit swap chain. The
  // window is never shown and Present is never called; rendering goes to a
  // separate offscreen target.
  WindowGuard window{CreateWindowExW(0, L"STATIC", L"dxvk-offscreen", WS_POPUP, 0, 0, Size, Size,
                                     nullptr, nullptr, GetModuleHandleW(nullptr), nullptr)};
  if (!window.hwnd) {
    printf("DEVICE d3d9: FAIL hidden window, error %lu\n", GetLastError());
    return ExitDevice;
  }
  D3DPRESENT_PARAMETERS pp = {};
  pp.Windowed = TRUE;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.BackBufferWidth = pp.BackBufferHeight = Size;
  pp.BackBufferFormat = D3DFMT_X8R8G8B8;
  pp.hDeviceWindow = window.hwnd;
  Com<IDirect3DDevice9> device;
  HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window.hwnd,
                                 D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &device);
  if (FAILED(hr)) {
    printf("DEVICE d3d9: FAIL hr=0x%08lx\n", static_cast<unsigned long>(hr));
    return ExitDevice;
  }
  // DXVK may report a substitute Direct3D 9 identity; its own log names the
  // Vulkan device.
  printf("DEVICE d3d9: reported adapter \"%s\" %04lx:%04lx\n", id.Description, id.VendorId, id.DeviceId);

  Com<IDirect3DSurface9> target, system;
  REQUIRE(device->CreateRenderTarget(Size, Size, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &target, nullptr));
  REQUIRE(device->CreateOffscreenPlainSurface(Size, Size, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &system, nullptr));
  REQUIRE(device->SetRenderTarget(0, target));

  Readback readback;
  auto read = [&]() -> HRESULT {
    HRESULT result = device->GetRenderTargetData(target, system);
    D3DLOCKED_RECT rect;
    if (SUCCEEDED(result))
      result = system->LockRect(&rect, nullptr, D3DLOCK_READONLY);
    if (SUCCEEDED(result)) {
      capture(readback, rect.pBits, rect.Pitch, true);
      system->UnlockRect();
    }
    return result;
  };

  const D3DCOLOR clear = D3DCOLOR_ARGB(ClearBytes[3], ClearBytes[0], ClearBytes[1], ClearBytes[2]);
  REQUIRE(device->Clear(0, nullptr, D3DCLEAR_TARGET, clear, 1.0f, 0));
  REQUIRE(read());
  if (!checkClear("d3d9 clear", readback))
    return ExitClear;

  // Pre-transformed vertices through the fixed-function pipeline: DXVK
  // generates the shaders, so no HLSL compiler is involved.
  struct Vertex { float x, y, z, rhw; D3DCOLOR color; };
  const D3DCOLOR red = D3DCOLOR_ARGB(255, 255, 0, 0);
  const Vertex triangle[3] = {
    {0.0f, 0.0f, 0.5f, 1.0f, red}, {float(Size), 0.0f, 0.5f, 1.0f, red}, {0.0f, float(Size), 0.5f, 1.0f, red},
  };
  REQUIRE(device->BeginScene());
  REQUIRE(device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE));
  REQUIRE(device->SetRenderState(D3DRS_LIGHTING, FALSE));
  REQUIRE(device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE));
  REQUIRE(device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, triangle, sizeof(Vertex)));
  REQUIRE(device->EndScene());
  REQUIRE(read());
  return checkTriangle("d3d9 draw", readback) ? ExitPass : ExitDraw;
}

// The system runtime loads some of its parts by name, which would find DXVK
// DLLs placed beside this executable and mix the two implementations.
bool systemControlIsIsolated() {
  bool isolated = true;
  for (const wchar_t* name : {L"d3d9.dll", L"d3d10core.dll", L"d3d11.dll", L"dxgi.dll"}) {
    if (GetFileAttributesW((exeDirectory() + name).c_str()) != INVALID_FILE_ATTRIBUTES) {
      printf("CONTROL --system: %ls is beside the executable; run the control from a "
             "directory without Direct3D DLLs\n", name);
      isolated = false;
    }
  }
  return isolated;
}

// Model of the comparison rules themselves; needs no Direct3D.
int selfTest() {
  static Readback r;
  for (UINT y = 0; y < Size; y++)
    for (UINT x = 0; x < Size; x++)
      memcpy(r.rgba[y][x], ClearBytes, 4);
  if (!checkClear("self clear", r))
    return 20;
  r.rgba[63][63][1] = 126; // two below 128: outside the tolerance
  if (checkClear("self clear rejects one wrong pixel (FAIL expected)", r))
    return 21;
  r.rgba[63][63][1] = 127;
  for (UINT y = 0; y < Size; y++)
    for (UINT x = 0; x < Size; x++)
      if (x + y < Size - 1)
        memcpy(r.rgba[y][x], DrawBytes, 4);
  if (!checkTriangle("self triangle", r))
    return 22;
  memcpy(r.rgba[40][40], DrawBytes, 4); // covered pixel outside the triangle
  if (checkTriangle("self triangle rejects overdraw (FAIL expected)", r))
    return 23;
  memcpy(r.rgba[40][40], ClearBytes, 4);
  memcpy(r.rgba[5][5], ClearBytes, 4); // hole inside the triangle
  if (checkTriangle("self triangle rejects a hole (FAIL expected)", r))
    return 24;
  // A BGRA source must be swizzled, an RGBA one must not.
  static uint8_t raw[Size * Size * 4];
  for (UINT i = 0; i < Size * Size; i++) {
    raw[4 * i + 0] = 191; raw[4 * i + 1] = 128; raw[4 * i + 2] = 64; raw[4 * i + 3] = 255;
  }
  capture(r, raw, Size * 4, true);
  if (!checkClear("self bgra capture", r))
    return 25;
  capture(r, raw, Size * 4, false);
  if (checkClear("self rgba capture of bgra data (FAIL expected)", r))
    return 26;
  printf("SELFTEST PASS: comparison rules (no Direct3D involved)\n");
  return 0;
}

} // namespace

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  const char* api = argc > 1 ? argv[1] : "";
  bool indirect = false;
  for (int i = 2; i < argc; i++) {
    if (!strcmp(argv[i], "--system"))
      g_system = true;
    else if (!strcmp(argv[i], "--indirect") && !strcmp(api, "d3d11"))
      indirect = true;
    else
      api = "";
  }
  int result;
  if (!strcmp(api, "selftest") && argc == 2)
    return selfTest();
  if (*api && g_system && !systemControlIsIsolated())
    return ExitModule;
  else if (!strcmp(api, "d3d9"))
    result = runD3D9();
  else if (!strcmp(api, "d3d10"))
    result = runD3D10();
  else if (!strcmp(api, "d3d11"))
    result = runD3D11(indirect);
  else {
    printf("usage: dxvk-offscreen.exe selftest | <d3d9|d3d10|d3d11> [--system] [--indirect (d3d11)]\n");
    return ExitUsage;
  }
  printf("RESULT %s%s: %s (exit %d)\n", api, g_system ? " (system runtime control)" : "",
         result == ExitPass ? "PASS" : "FAIL", result);
  return result;
}
