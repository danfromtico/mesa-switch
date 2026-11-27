// SPDX-License-Identifier: MIT
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <wrl/client.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include "dxvk_device_interop.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

namespace {
constexpr DWORD clear_color = D3DCOLOR_ARGB(255, 19, 83, 171);
constexpr DWORD vertex_color = D3DCOLOR_ARGB(255, 175, 99, 203);
constexpr unsigned side = 48;

class Journal {
public:
    explicit Journal(const fs::path& file) {
        handle = CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                             CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            throw std::runtime_error("Cannot create the diagnostic journal");
    }
    ~Journal() { CloseHandle(handle); }
    void mark(const std::string& event, const std::string& arg = "") {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        const std::string row = std::to_string(now.QuadPart) + "\t" + event + "\t" + arg + "\r\n";
        DWORD written = 0;
        if (!WriteFile(handle, row.data(), static_cast<DWORD>(row.size()), &written, nullptr)
            || written != row.size() || !FlushFileBuffers(handle))
            throw std::runtime_error("Journal I/O failed");
    }
private:
    HANDLE handle = INVALID_HANDLE_VALUE;
};

void checked(HRESULT result, const char* name) {
    if (FAILED(result)) {
        char hex[12]{};
        std::snprintf(hex, sizeof(hex), "%08lx", static_cast<unsigned long>(result));
        throw std::runtime_error(std::string(name) + " HRESULT=" + hex);
    }
}

struct Window {
    HWND handle;
    Window() : handle(CreateWindowExW(0, L"STATIC", L"PAL4 offscreen shader control", WS_POPUP,
                                       0,0,800,600,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr)) {
        if (!handle) throw std::runtime_error("Cannot create the hidden device window");
    }
    ~Window() { DestroyWindow(handle); }
};

std::string path_utf8(HMODULE module) {
    wchar_t buf[32768];
    DWORD len = GetModuleFileNameW(module,buf,32768);
    if (!len || len>=32768) throw std::runtime_error("Cannot resolve module path");
    return fs::path(std::wstring(buf,len)).u8string();
}

std::vector<DWORD> shader_tokens(const fs::path& file) {
    const auto size = fs::file_size(file);
    if (size < 8 || size%4 || size>1024*1024) throw std::runtime_error("Invalid captured byte count");
    std::vector<DWORD> words(size/4);
    std::ifstream in(file,std::ios::binary);
    if (!in.read(reinterpret_cast<char*>(words.data()),static_cast<std::streamsize>(size)))
        throw std::runtime_error("Cannot read captured tokens");
    if ((words.front()>>16)!=0xffff || words.back()!=D3DSIO_END)
        throw std::runtime_error("Captured file is not a complete D3D9 pixel program");
    return words;
}

struct Program { std::string name; ComPtr<IDirect3DPixelShader9> shader; };
std::vector<Program> register_programs(IDirect3DDevice9* dev, const fs::path& order,
                                       const fs::path& corpus, Journal& log) {
    std::ifstream list(order);
    if (!list) throw std::runtime_error("Cannot open captured program list");
    std::vector<Program> programs;
    for (std::string name;std::getline(list,name);) {
        if (!name.empty() && name.back()=='\r') name.pop_back();
        if (name.size()!=35 || name.compare(0,3,"fs.") || name.find_first_not_of("0123456789abcdef",3)!=std::string::npos)
            throw std::runtime_error("Invalid captured program name");
        auto words=shader_tokens(corpus/(name+".sm3_dxbc"));
        Program prog{name,{}};
        log.mark("CreatePixelShader.begin",name);
        checked(dev->CreatePixelShader(words.data(),&prog.shader),"CreatePixelShader");
        UINT bytes=0;
        checked(prog.shader->GetFunction(nullptr,&bytes),"GetFunction length");
        if (bytes!=words.size()*sizeof(DWORD)) throw std::runtime_error("Bytecode length differs");
        std::vector<DWORD> copy(words.size());
        checked(prog.shader->GetFunction(copy.data(),&bytes),"GetFunction bytes");
        if (words!=copy) throw std::runtime_error("Round-trip bytecode differs");
        log.mark("CreatePixelShader.verified",name);
        programs.push_back(std::move(prog));
    }
    if (programs.empty()) throw std::runtime_error("No programs to replay");
    return programs;
}

struct Vertex { float x,y,z,rhw; DWORD color,specular; float uv[8][2]; };
Vertex vertex(float x,float y,float u,float v) {
    Vertex out{x,y,0.5f,1.0f,vertex_color,0xff6b78a9,{}};
    for (auto& uv:out.uv) { uv[0]=u;uv[1]=v; }
    return out;
}
using Pixels=std::array<DWORD,side*side>;
struct Target {
    IDirect3DDevice9* dev;
    ComPtr<IDirect3DSurface9> target,system,depth;
    ComPtr<IDirect3DTexture9> texture;
    explicit Target(IDirect3DDevice9* device) : dev(device) {
        checked(dev->CreateRenderTarget(side,side,D3DFMT_A8R8G8B8,D3DMULTISAMPLE_NONE,0,FALSE,&target,nullptr),"CreateRenderTarget");
        checked(dev->CreateOffscreenPlainSurface(side,side,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,&system,nullptr),"CreateOffscreenPlainSurface");
        checked(dev->CreateDepthStencilSurface(side,side,D3DFMT_D24S8,D3DMULTISAMPLE_NONE,0,TRUE,&depth,nullptr),"CreateDepthStencilSurface");
        checked(dev->SetRenderTarget(0,target.Get()),"SetRenderTarget");
        checked(dev->SetDepthStencilSurface(depth.Get()),"SetDepthStencilSurface");
        D3DVIEWPORT9 vp{0,0,side,side,0.0f,1.0f};
        checked(dev->SetViewport(&vp),"SetViewport");
        checked(dev->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE),"Cull");
        checked(dev->SetRenderState(D3DRS_LIGHTING,FALSE),"Lighting");
        checked(dev->SetRenderState(D3DRS_ZENABLE,D3DZB_TRUE),"ZEnable");
        checked(dev->SetFVF(D3DFVF_XYZRHW|D3DFVF_DIFFUSE|D3DFVF_SPECULAR|D3DFVF_TEX8),"SetFVF");
        checked(dev->CreateTexture(2,2,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&texture,nullptr),"CreateTexture");
        D3DLOCKED_RECT map{};
        checked(texture->LockRect(0,&map,nullptr,0),"Texture LockRect");
        const DWORD values[2][2]={{0xffd2562e,0xff428adb},{0xffad683b,0xff7eaf95}};
        for (int y=0;y<2;++y) std::memcpy(static_cast<BYTE*>(map.pBits)+y*map.Pitch,values[y],sizeof(values[y]));
        checked(texture->UnlockRect(0),"Texture UnlockRect");
        for (DWORD stage=0;stage<16;++stage) {
            checked(dev->SetTexture(stage,texture.Get()),"SetTexture");
            checked(dev->SetSamplerState(stage,D3DSAMP_MINFILTER,D3DTEXF_POINT),"MinFilter");
            checked(dev->SetSamplerState(stage,D3DSAMP_MAGFILTER,D3DTEXF_POINT),"MagFilter");
        }
        checked(dev->SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_SELECTARG1),"ColorOp");
        checked(dev->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_DIFFUSE),"ColorArg");
        checked(dev->SetTextureStageState(0,D3DTSS_ALPHAOP,D3DTOP_SELECTARG1),"AlphaOp");
        checked(dev->SetTextureStageState(0,D3DTSS_ALPHAARG1,D3DTA_DIFFUSE),"AlphaArg");
        float constants[32][4];
        for (auto& c:constants) {c[0]=.25f;c[1]=.5f;c[2]=.75f;c[3]=1.0f;}
        checked(dev->SetPixelShaderConstantF(0,constants[0],32),"SetPixelShaderConstantF");
    }
    Pixels draw(IDirect3DPixelShader9* shader, Journal& log, const std::string& name) {
        const Vertex quad[4]={vertex(8,8,0,0),vertex(40,8,1,0),vertex(8,40,0,1),vertex(40,40,1,1)};
        checked(dev->Clear(0,nullptr,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER|D3DCLEAR_STENCIL,clear_color,1.0f,0),"Clear");
        checked(dev->SetPixelShader(shader),"SetPixelShader");
        checked(dev->BeginScene(),"BeginScene");
        log.mark("DrawPrimitiveUP.begin",name);
        checked(dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,quad,sizeof(Vertex)),"DrawPrimitiveUP");
        checked(dev->EndScene(),"EndScene");
        log.mark("GetRenderTargetData.begin",name);
        checked(dev->GetRenderTargetData(target.Get(),system.Get()),"GetRenderTargetData");
        D3DLOCKED_RECT map{};
        checked(system->LockRect(&map,nullptr,D3DLOCK_READONLY),"Readback LockRect");
        Pixels result{};
        if (map.Pitch<static_cast<int>(side*sizeof(DWORD))) {system->UnlockRect();throw std::runtime_error("Invalid readback pitch");}
        for (unsigned y=0;y<side;++y)
            std::memcpy(result.data()+y*side,static_cast<BYTE*>(map.pBits)+y*map.Pitch,side*sizeof(DWORD));
        checked(system->UnlockRect(),"Readback UnlockRect");
        for (unsigned y=0;y<side;++y) for(unsigned x=0;x<side;++x) {
            if ((x<7 || x>40 || y<7 || y>40) && result[y*side+x]!=clear_color)
                throw std::runtime_error("Draw modified the clear border");
        }
        log.mark("GetRenderTargetData.verified",name);
        return result;
    }
};

int run(const wchar_t* mode,const fs::path& manifest,const fs::path& corpus,
        const fs::path& output,const wchar_t* backend,Journal& log) {
    const bool draw=wcscmp(mode,L"draw")==0;
    const bool native=wcscmp(backend,L"native")==0;
    const fs::path dir=fs::path(path_utf8(nullptr)).parent_path();
    HMODULE module=LoadLibraryW((dir/L"d3d9.dll").c_str());
    if (!module) throw std::runtime_error("Application DXVK unavailable");
    using Create=IDirect3D9*(WINAPI*)(UINT);
    auto create=reinterpret_cast<Create>(reinterpret_cast<void*>(GetProcAddress(module,"Direct3DCreate9")));
    if (!create) throw std::runtime_error("Missing Direct3DCreate9");
    Window window;
    ComPtr<IDirect3D9> api;api.Attach(create(D3D_SDK_VERSION));
    if (!api) throw std::runtime_error("Direct3DCreate9 failed");
    D3DADAPTER_IDENTIFIER9 adapter{};
    checked(api->GetAdapterIdentifier(0,0,&adapter),"GetAdapterIdentifier");
    log.mark("AdvertisedD3D9Adapter",std::string(adapter.Description)+" vendor="+std::to_string(adapter.VendorId));
    D3DPRESENT_PARAMETERS pp{};
    pp.BackBufferFormat=D3DFMT_X8R8G8B8;pp.BackBufferCount=1;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow=window.handle;pp.Windowed=TRUE;pp.EnableAutoDepthStencil=TRUE;
    pp.AutoDepthStencilFormat=D3DFMT_D24S8;pp.PresentationInterval=D3DPRESENT_INTERVAL_ONE;
    ComPtr<IDirect3DDevice9> device;
    log.mark("CreateDevice.begin");
    checked(api->CreateDevice(0,D3DDEVTYPE_HAL,window.handle,D3DCREATE_HARDWARE_VERTEXPROCESSING,
                              &pp,&device),"CreateDevice");
    log.mark("CreateDevice.return");
    {
        ComPtr<ID3D9VkInteropDevice> interop;
        checked(device->QueryInterface(IID_PPV_ARGS(&interop)), "DXVK device interop");
        VkInstance instance=VK_NULL_HANDLE;
        VkPhysicalDevice physical=VK_NULL_HANDLE;
        VkDevice logical=VK_NULL_HANDLE;
        interop->GetVulkanHandles(&instance,&physical,&logical);
        auto loader=GetModuleHandleW(L"vulkan-1.dll");
        if (!loader || !instance || !physical || !logical)
            throw std::runtime_error("DXVK device did not expose live Vulkan handles");
        auto gipa=reinterpret_cast<PFN_vkGetInstanceProcAddr>(reinterpret_cast<void*>(
            GetProcAddress(loader,"vkGetInstanceProcAddr")));
        auto get=gipa ? reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
            gipa(instance,"vkGetPhysicalDeviceProperties2")) : nullptr;
        if (!get) throw std::runtime_error("Missing Vulkan physical-device query");
        VkPhysicalDeviceIDProperties id{};
        id.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
        VkPhysicalDeviceProperties2 props{};
        props.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;props.pNext=&id;
        get(physical,&props);
        const auto& bound=props.properties;
        uint64_t luid=0;std::memcpy(&luid,id.deviceLUID,sizeof(luid));
        char ident[150]{};
        std::snprintf(ident,sizeof(ident)," vendor=%04x device=%04x type=%d luid_valid=%u luid=%016llx",
                      bound.vendorID,bound.deviceID,static_cast<int>(bound.deviceType),
                      id.deviceLUIDValid,static_cast<unsigned long long>(luid));
        log.mark("BoundVulkanDevice",std::string(bound.deviceName)+ident);
        if (native ? (bound.vendorID!=0x10de || !std::strstr(bound.deviceName,"GM20B") || !id.deviceLUIDValid ||
                      !luid || id.deviceNodeMask!=1 || bound.deviceType==VK_PHYSICAL_DEVICE_TYPE_CPU)
                   : (bound.deviceType!=VK_PHYSICAL_DEVICE_TYPE_CPU || !std::strstr(bound.deviceName,"llvmpipe")))
            throw std::runtime_error("Unexpected bound Vulkan device");
    }
    for(const wchar_t* name : {L"d3d9.dll",L"vulkan-1.dll",native ? L"libvulkan_nouveau.dll" : L"vulkan_lvp.dll"}) {
        auto mod=GetModuleHandleW(name);
        if(!mod) throw std::runtime_error("Rendering module not observed");
        log.mark("Module",path_utf8(mod));
    }
    {
        auto programs=register_programs(device.Get(),manifest,corpus,log);
        if(draw){
            Target target(device.Get());
            auto control=target.draw(nullptr,log,"fixed-function");
            if(control[24*side+24]!=vertex_color) throw std::runtime_error("Known-color draw differs");
            auto pixels_dir=output.parent_path()/"pixels";fs::create_directory(pixels_dir);
            for(auto& prog:programs){
                auto first=target.draw(prog.shader.Get(),log,prog.name);
                if(first!=target.draw(prog.shader.Get(),log,prog.name))
                    throw std::runtime_error("Repeated program readback differs");
                std::ofstream file(pixels_dir/(prog.name+".bgra"),std::ios::binary);
                file.write(reinterpret_cast<const char*>(first.data()),sizeof(first));file.close();
                if(!file) throw std::runtime_error("Cannot retain program pixels");
            }
        }
        log.mark("Programs.complete",std::to_string(programs.size()));
        checked(device->SetPixelShader(nullptr),"Unbind pixel program");
    }
    log.mark("ReleaseDevice.begin");device.Reset();api.Reset();log.mark("ReleaseDevice.return");
    // DXVK defers worker cleanup. Leave its library loaded until process exit.
    return 0;
}
}
int wmain(int argc,wchar_t** argv) {
    if (argc!=6 || (wcscmp(argv[1],L"draw") && wcscmp(argv[1],L"create"))
        || (wcscmp(argv[5],L"native") && wcscmp(argv[5],L"software"))) return 2;
    try {
        Journal log(argv[4]);
        try { return run(argv[1],argv[2],argv[3],argv[4],argv[5],log); }
        catch(const std::exception& e){log.mark("Failure",e.what());return 1;}
    } catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 3;}
}
