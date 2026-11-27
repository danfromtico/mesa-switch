// SPDX-License-Identifier: MIT
// Windowed Utah teapot for an app-local DXVK and hardware NVK acceptance run.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define VK_NO_PROTOTYPES
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <vulkan/vulkan_core.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace {
constexpr UINT width = 384, height = 256;
constexpr float clearColor[] = {0.04f, 0.07f, 0.12f, 1.0f};
struct Vertex { float x, y, z, nx, ny, nz; };
#include "teapot_mesh.inc"

struct Scene {
    bool running = true, paused = false, capture = true;
    float angle = 1.18f;
    unsigned revision = 0, presents = 0;
};

void check(HRESULT hr, const char* operation) {
    if (FAILED(hr)) {
        std::fprintf(stderr, "%s failed with HRESULT 0x%08lx\n", operation, static_cast<unsigned long>(hr));
        throw std::runtime_error(operation);
    }
}

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM key, LPARAM param) {
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(param);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* scene = reinterpret_cast<Scene*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (scene && message == WM_CLOSE) { scene->running = false; return 0; }
    if (scene && message == WM_KEYDOWN && !(param & (1u << 30))) {
        switch (key) {
        case VK_ESCAPE: scene->running = false; break;
        case VK_SPACE: scene->paused = !scene->paused; break;
        case VK_LEFT: scene->angle -= 0.75f; break;
        case VK_RIGHT: scene->angle += 0.75f; break;
        default: return DefWindowProcW(window, message, key, param);
        }
        ++scene->revision;
        scene->capture = true;
        std::printf("INPUT key=%u revision=%u paused=%d angle=%.3f\n",
                    unsigned(key), scene->revision, scene->paused, scene->angle);
        return 0;
    }
    return DefWindowProcW(window, message, key, param);
}

std::wstring modulePath(HMODULE module) {
    wchar_t path[MAX_PATH];
    const DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
    if (!length || length >= MAX_PATH) throw std::runtime_error("module path");
    return {path, length};
}

HMODULE sibling(const wchar_t* name) {
    auto path = modulePath(nullptr);
    path.resize(path.find_last_of(L'\\') + 1);
    path += name;
    HMODULE mod = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!mod || _wcsicmp(modulePath(mod).c_str(), path.c_str()))
        throw std::runtime_error("application-local Direct3D DLL required");
    std::printf("MODULE %ls\n", path.c_str());
    return mod;
}

// First and only method of DXVK's IDXGIVkInteropAdapter, from dxgi_interfaces.h.
struct DxvkAdapter : IUnknown {
    virtual void STDMETHODCALLTYPE GetVulkanHandles(VkInstance*, VkPhysicalDevice*) = 0;
};
constexpr GUID interopAdapterId =
    {0x3a6d8f2c,0xb0e8,0x4ab4,{0xb4,0xdc,0x4f,0xd2,0x48,0x91,0xbf,0xa5}};

void verifyAdapter(IDXGIAdapter* adapter) {
    DXGI_ADAPTER_DESC dx{};
    check(adapter->GetDesc(&dx), "DXGI adapter description");
    ComPtr<DxvkAdapter> interop;
    check(adapter->QueryInterface(interopAdapterId, reinterpret_cast<void**>(interop.GetAddressOf())),
          "DXVK Vulkan adapter interface");
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    interop->GetVulkanHandles(&instance, &physical);
    HMODULE loader = GetModuleHandleW(L"vulkan-1.dll");
    HMODULE icd = GetModuleHandleW(L"libvulkan_nouveau.dll");
    if (!instance || !physical || !loader || !icd) throw std::runtime_error("DXVK Vulkan handles or loaded ICD");
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        reinterpret_cast<void*>(GetProcAddress(loader, "vkGetInstanceProcAddr")));
    if (!gipa) throw std::runtime_error("Vulkan instance loader");
    auto query = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(gipa(instance, "vkGetPhysicalDeviceProperties2"));
    if (!query) throw std::runtime_error("Vulkan properties query");
    VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES, &driver};
    VkPhysicalDeviceProperties2 p{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id};
    query(physical, &p);
    const bool match = id.deviceLUIDValid && !std::memcmp(id.deviceLUID, &dx.AdapterLuid, sizeof(LUID));
    std::printf("ADAPTER dxgi=\"%ls\" vk=\"%s\" driver=\"%s\" driver_id=%d vendor=%04x "
                "device=%04x luid=%08lx%08lx matching_luid=%d\n", dx.Description, p.properties.deviceName,
                driver.driverName, int(driver.driverID), p.properties.vendorID, p.properties.deviceID,
                static_cast<unsigned long>(dx.AdapterLuid.HighPart), dx.AdapterLuid.LowPart, match);
    std::printf("MODULE %ls\nMODULE %ls\n", modulePath(loader).c_str(), modulePath(icd).c_str());
    if (!match || driver.driverID != VK_DRIVER_ID_MESA_NVK || p.properties.vendorID != 0x10de ||
        !std::strstr(p.properties.deviceName, "GM20B"))
        throw std::runtime_error("expected DXVK on the physical GM20B NVK adapter");
}

constexpr char shader[] = R"(
cbuffer Rotation : register(b0) { float2 yaw; float2 pad; };
struct V { float3 position : POSITION; float3 normal : NORMAL; };
struct P { float4 position : SV_Position; float3 normal : NORMAL; };
float3 rotate(float3 v) {
    float3 w = float3(yaw.x*v.x + yaw.y*v.z, v.y, -yaw.y*v.x + yaw.x*v.z);
    return float3(w.x, 0.966*w.y - 0.259*w.z, 0.259*w.y + 0.966*w.z);
}
P vs(V v) {
    P p; float3 view = rotate(v.position);
    float depth = 1.62 - view.z;
    p.position = float4(view.x * 1.62, view.y * 2.43, (depth-0.25)*1.08, depth);
    p.normal = rotate(v.normal); return p;
}
float4 ps(P p) : SV_Target {
    float3 n = normalize(p.normal);
    float d = saturate(dot(n, normalize(float3(-0.4, 0.7, 0.6))));
    float rim = pow(1.0-saturate(abs(n.z)), 3);
    return float4(float3(0.04, 0.53, 0.62)*(0.30+0.70*d) + 0.17*rim, 1);
})";

ComPtr<ID3DBlob> compile(const char* entry, const char* target) {
    ComPtr<ID3DBlob> blob, errors;
    HRESULT hr = D3DCompile(shader, sizeof(shader)-1, nullptr, nullptr, nullptr, entry, target,
                             D3DCOMPILE_OPTIMIZATION_LEVEL2, 0, &blob, &errors);
    if (errors) std::fwrite(errors->GetBufferPointer(), 1, errors->GetBufferSize(), stderr);
    check(hr, entry);
    return blob;
}

ComPtr<ID3D11Buffer> buffer(ID3D11Device* device, UINT bytes, UINT bind, const void* data) {
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = bytes; desc.BindFlags = bind;
    desc.Usage = data ? D3D11_USAGE_IMMUTABLE : D3D11_USAGE_DEFAULT;
    D3D11_SUBRESOURCE_DATA init{data, 0, 0};
    ComPtr<ID3D11Buffer> result;
    check(device->CreateBuffer(&desc, data ? &init : nullptr, &result), "create mesh or uniform buffer");
    return result;
}

void capture(ID3D11DeviceContext* ctx, ID3D11Texture2D* stage, ID3D11Texture2D* back, const Scene& scene) {
    std::vector<uint8_t> rgb(width*height*3);
    ctx->CopyResource(stage, back);
    D3D11_MAPPED_SUBRESOURCE map{};
    check(ctx->Map(stage, 0, D3D11_MAP_READ, 0, &map), "read back teapot");
    unsigned coverage = 0, lo = 255, hi = 0;
    uint64_t hash = UINT64_C(14695981039346656037);
    for (unsigned y = 0; y < height; ++y) {
        const auto* row = static_cast<const uint8_t*>(map.pData) + y*map.RowPitch;
        for (unsigned x = 0; x < width; ++x) {
            const auto* pixel = row + 4*x;
            std::memcpy(&rgb[(width*y+x)*3], pixel, 3);
            for (unsigned c=0; c<3; ++c) hash = (hash ^ pixel[c]) * UINT64_C(1099511628211);
            if (pixel[1] > 30 && pixel[1] > pixel[0]+12) {
                ++coverage; lo=std::min(lo, unsigned(pixel[1])); hi=std::max(hi, unsigned(pixel[1]));
            }
        }
    }
    ctx->Unmap(stage, 0);
    std::string name = "teapot-"+std::to_string(scene.revision)+".ppm";
    FILE* image = std::fopen(name.c_str(), "wb");
    if (!image) throw std::runtime_error("open readback image");
    const bool header = std::fprintf(image, "P6\n%u %u\n255\n", width, height) > 0;
    const auto bytes = std::fwrite(rgb.data(), 1, rgb.size(), image);
    const auto closed = std::fclose(image);
    if (!header || bytes != rgb.size() || closed) throw std::runtime_error("save readback image");
    std::printf("READBACK revision=%u pixels=%u green_min=%u green_max=%u fnv64=%016llx\n",
                scene.revision, coverage, lo, hi, static_cast<unsigned long long>(hash));
    if (coverage < 800 || coverage > width*height*3/4 || hi <= lo+18)
        throw std::runtime_error("inadequate teapot coverage or lighting range");
}

void render(HWND window, Scene& scene) {
    sibling(L"dxgi.dll");
    HMODULE d3d11 = sibling(L"d3d11.dll");
    auto create = reinterpret_cast<decltype(&D3D11CreateDevice)>(
        reinterpret_cast<void*>(GetProcAddress(d3d11, "D3D11CreateDevice")));
    if (!create) throw std::runtime_error("D3D11CreateDevice export");
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL level;
    check(create(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,levels,UINT(std::size(levels)),D3D11_SDK_VERSION,
                 &device,&level,&ctx), "create D3D11 hardware device");
    ComPtr<IDXGIDevice> dxgi; check(device.As(&dxgi),"DXGI device");
    ComPtr<IDXGIAdapter> adapter; check(dxgi->GetAdapter(&adapter),"device adapter");
    verifyAdapter(adapter.Get());
    ComPtr<IDXGIFactory> factory; check(adapter->GetParent(IID_PPV_ARGS(&factory)),"device factory");
    DXGI_SWAP_CHAIN_DESC swap{};
    swap.BufferDesc.Width=width; swap.BufferDesc.Height=height; swap.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    swap.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT; swap.SampleDesc.Count=1; swap.BufferCount=2;
    swap.OutputWindow=window; swap.Windowed=TRUE; swap.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
    ComPtr<IDXGISwapChain> chain; check(factory->CreateSwapChain(device.Get(),&swap,&chain),"window swapchain");
    ComPtr<ID3D11Texture2D> back; check(chain->GetBuffer(0,IID_PPV_ARGS(&back)),"back buffer");
    ComPtr<ID3D11RenderTargetView> color; check(device->CreateRenderTargetView(back.Get(),nullptr,&color),"color view");
    D3D11_TEXTURE2D_DESC td{}; back->GetDesc(&td);
    td.BindFlags=0; td.Usage=D3D11_USAGE_STAGING; td.CPUAccessFlags=D3D11_CPU_ACCESS_READ; td.MiscFlags=0;
    ComPtr<ID3D11Texture2D> stage; check(device->CreateTexture2D(&td,nullptr,&stage),"readback surface");
    td.Format=DXGI_FORMAT_D24_UNORM_S8_UINT; td.BindFlags=D3D11_BIND_DEPTH_STENCIL;
    td.Usage=D3D11_USAGE_DEFAULT; td.CPUAccessFlags=0;
    ComPtr<ID3D11Texture2D> depth; check(device->CreateTexture2D(&td,nullptr,&depth),"depth surface");
    ComPtr<ID3D11DepthStencilView> depthView;
    check(device->CreateDepthStencilView(depth.Get(),nullptr,&depthView),"depth view");

    auto vsBlob=compile("vs","vs_4_0"), psBlob=compile("ps","ps_4_0");
    ComPtr<ID3D11VertexShader> vs; ComPtr<ID3D11PixelShader> ps;
    check(device->CreateVertexShader(vsBlob->GetBufferPointer(),vsBlob->GetBufferSize(),nullptr,&vs),"vertex shader");
    check(device->CreatePixelShader(psBlob->GetBufferPointer(),psBlob->GetBufferSize(),nullptr,&ps),"pixel shader");
    const D3D11_INPUT_ELEMENT_DESC elements[] = {
        {"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"NORMAL",0,DXGI_FORMAT_R32G32B32_FLOAT,0,12,D3D11_INPUT_PER_VERTEX_DATA,0}
    };
    ComPtr<ID3D11InputLayout> layout;
    check(device->CreateInputLayout(elements,2,vsBlob->GetBufferPointer(),vsBlob->GetBufferSize(),&layout),"input layout");
    auto vb=buffer(device.Get(),sizeof(vertices),D3D11_BIND_VERTEX_BUFFER,vertices);
    auto ib=buffer(device.Get(),sizeof(indices),D3D11_BIND_INDEX_BUFFER,indices);
    auto cb=buffer(device.Get(),16,D3D11_BIND_CONSTANT_BUFFER,nullptr);
    D3D11_RASTERIZER_DESC raster{}; raster.FillMode=D3D11_FILL_SOLID; raster.CullMode=D3D11_CULL_NONE;
    raster.DepthClipEnable=TRUE;
    ComPtr<ID3D11RasterizerState> rs; check(device->CreateRasterizerState(&raster,&rs),"raster state");
    const D3D11_VIEWPORT viewport{0,0,float(width),float(height),0,1};
    ctx->RSSetState(rs.Get()); ctx->RSSetViewports(1,&viewport);
    ctx->IASetInputLayout(layout.Get()); ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    UINT stride=sizeof(Vertex),offset=0;
    ctx->IASetVertexBuffers(0,1,vb.GetAddressOf(),&stride,&offset);
    ctx->IASetIndexBuffer(ib.Get(),DXGI_FORMAT_R16_UINT,0);
    ctx->VSSetShader(vs.Get(),nullptr,0); ctx->PSSetShader(ps.Get(),nullptr,0);
    ctx->VSSetConstantBuffers(0,1,cb.GetAddressOf());
    std::printf("READY feature_level=0x%x vertices=%zu triangles=%zu\n",unsigned(level),std::size(vertices),std::size(indices)/3);
    while (scene.running) {
        MSG message;
        while (PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        if (!scene.running) break;
        const float rotation[4]={std::cos(scene.angle),std::sin(scene.angle),0,0};
        ctx->UpdateSubresource(cb.Get(),0,nullptr,rotation,0,0);
        ctx->OMSetRenderTargets(1,color.GetAddressOf(),depthView.Get());
        ctx->ClearRenderTargetView(color.Get(),clearColor);
        ctx->ClearDepthStencilView(depthView.Get(),D3D11_CLEAR_DEPTH,1.0f,0);
        ctx->DrawIndexed(UINT(std::size(indices)),0,0);
        const bool sampled=scene.capture;
        if (sampled) { capture(ctx.Get(),stage.Get(),back.Get(),scene); scene.capture=false; }
        check(chain->Present(1,0),"present teapot");
        ++scene.presents;
        if (sampled) std::printf("PRESENT revision=%u frames=%u\n",scene.revision,scene.presents);
        if (!scene.paused) scene.angle+=0.025f;
        MsgWaitForMultipleObjects(0,nullptr,FALSE,85,QS_ALLINPUT);
    }
    ctx->ClearState(); ctx->Flush();
}
}

int main() {
    std::setvbuf(stdout,nullptr,_IONBF,0);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    Scene scene;
    WNDCLASSW cls{}; cls.lpfnWndProc=windowProc; cls.hInstance=GetModuleHandleW(nullptr);
    cls.lpszClassName=L"SwitchDxvkTeapot"; cls.hCursor=LoadCursorW(nullptr,MAKEINTRESOURCEW(32512));
    if (!RegisterClassW(&cls)) return 1;
    DWORD style=WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX;
    RECT size{0,0,LONG(width),LONG(height)}; AdjustWindowRect(&size,style,FALSE);
    HWND window=CreateWindowW(cls.lpszClassName,L"Utah Teapot | Space: pause   Arrows: rotate   Esc: exit",
                              style,CW_USEDEFAULT,CW_USEDEFAULT,size.right-size.left,size.bottom-size.top,
                              nullptr,nullptr,cls.hInstance,&scene);
    if (!window) return 1;
    ShowWindow(window,SW_SHOWNORMAL);
    int result=0;
    try { render(window,scene); }
    catch (const std::exception& e) { std::fprintf(stderr,"FAIL %s\n",e.what()); result=1; }
    DestroyWindow(window);
    std::printf("EXIT frames=%u input_events=%u code=%d\n",scene.presents,scene.revision,result);
    return result;
}
