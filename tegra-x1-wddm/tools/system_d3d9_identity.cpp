/* SPDX-License-Identifier: MIT
 * Report system D3D9 and D3D9Ex adapters in the calling process. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

extern "C" {
#include "kmt.h"
}

static HANDLE journal = INVALID_HANDLE_VALUE;

static bool row(const char *format, ...)
{
    char line[4096]{};
    va_list args;
    va_start(args, format);
    int length = std::vsnprintf(line, sizeof(line) - 1, format, args);
    va_end(args);
    if (length < 0 || static_cast<size_t>(length) >= sizeof(line) - 1)
        return false;
    line[length++] = '\n';
    DWORD written = 0;
    return WriteFile(journal, line, static_cast<DWORD>(length), &written, nullptr) &&
           written == static_cast<DWORD>(length) && FlushFileBuffers(journal);
}

static std::string utf8(const wchar_t *p)
{
    if (!*p) return {};
    int bytes = WideCharToMultiByte(CP_UTF8, 0, p, -1, nullptr, 0, nullptr, nullptr);
    if (!bytes) return {};
    std::string result(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, p, -1, result.data(), bytes, nullptr, nullptr);
    result.resize(result.size() - 1);
    return result;
}

static std::string utf8_from_ansi(const char *p)
{
    wchar_t name[1024]{};
    return MultiByteToWideChar(CP_ACP, 0, p, -1, name, 1024) ? utf8(name) : std::string{};
}

static bool module_path(const char *phase, const wchar_t *name)
{
    HMODULE m = GetModuleHandleW(name);
    if (!m) return true;
    wchar_t loaded[1024]{}, final[1024]{};
    const DWORD len = GetModuleFileNameW(m, loaded, 1024);
    if (!len || len >= 1024) return false;
    HANDLE f = CreateFileW(loaded, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        const auto size = GetFinalPathNameByHandleW(f, final, 1024, VOLUME_NAME_DOS | FILE_NAME_NORMALIZED);
        if (!size || size >= 1024) final[0] = 0;
        CloseHandle(f);
    }
    return row("module\t%s\t%s\t%s\t%s", phase, utf8(name).c_str(), utf8(loaded).c_str(), utf8(final).c_str());
}

static bool modules(const char *phase)
{
    const wchar_t *names[] = {L"d3d9.dll", L"d3d9on12.dll", L"d3d12.dll", L"D3D12Core.dll",
                              L"d3d10warp.dll", L"d3d11.dll", L"dxgi.dll", L"vulkan-1.dll",
                              L"TegraGpuD3DVk.dll", L"tegragpuno3d.dll", L"libvulkan_nouveau.dll"};
    for (const auto *name : names)
        if (!module_path(phase, name)) return false;
    return true;
}

static unsigned long long luid(LUID id)
{
    return (static_cast<unsigned long long>(static_cast<UINT32>(id.HighPart)) << 32) | id.LowPart;
}

static bool enumerate(const char *api, IDirect3D9 *d3d, IDirect3D9Ex *ex, unsigned long long native)
{
    const UINT count = d3d->GetAdapterCount();
    if (!row("adapters\t%s\t%u", api, count)) return false;
    for (UINT i = 0; i < count; ++i) {
        D3DADAPTER_IDENTIFIER9 id{}; D3DDISPLAYMODE mode{}; D3DCAPS9 caps{}; LUID id_luid{};
        const HRESULT desc = d3d->GetAdapterIdentifier(i, 0, &id);
        const HRESULT mode_hr = d3d->GetAdapterDisplayMode(i, &mode);
        const HRESULT caps_hr = d3d->GetDeviceCaps(i, D3DDEVTYPE_HAL, &caps);
        const HRESULT luid_hr = ex ? ex->GetAdapterLUID(i, &id_luid) : E_NOINTERFACE;
        const HRESULT device_hr = SUCCEEDED(mode_hr)
            ? d3d->CheckDeviceType(i, D3DDEVTYPE_HAL, mode.Format, D3DFMT_X8R8G8B8, TRUE) : mode_hr;
        if (!row("adapter\t%s\t%u\t%08lx\t%08lx\t%08lx\t%s\t%s\t%s", api, i, (ULONG)desc,
                 id.VendorId, id.DeviceId, utf8_from_ansi(id.Driver).c_str(), utf8_from_ansi(id.Description).c_str(),
                 utf8_from_ansi(id.DeviceName).c_str()) ||
            !row("luid\t%s\t%u\t%08lx\t%016llx\t%d", api, i, (ULONG)luid_hr, luid(id_luid),
                 SUCCEEDED(luid_hr) && luid(id_luid) == native) ||
            !row("display\t%s\t%u\t%08lx\t%u\t%u\t%u\t%u", api, i, (ULONG)mode_hr,
                 mode.Width, mode.Height, mode.RefreshRate, (UINT)mode.Format) ||
            !row("hal\t%s\t%u\t%08lx\t%08lx\t%08lx\t%08lx\t%08lx", api, i,
                 (ULONG)caps_hr, (ULONG)device_hr, caps.DevCaps, caps.VertexShaderVersion, caps.PixelShaderVersion))
            return false;
    }
    return modules(api);
}

static int inspect()
{
    if (!row("begin\t%lu\t%zu", GetCurrentProcessId(), sizeof(void *) * 8) || !modules("before")) return 3;
    tg_kmt_scan scan{};
    const LONG status = tg_gpu_kmt_scan_system(&scan);
    const auto &i = scan.first_ready;
    const auto native = static_cast<unsigned long long>(static_cast<UINT32>(i.luid_high)) << 32 | i.luid_low;
    if (!row("kmt\t%08lx\t%u\t%016llx\t%04x\t%03x\t%u", (ULONG)status, scan.native_ready,
             native, i.vendor, i.chipset, i.sm) || status < 0 || scan.native_ready != 1) return 4;

    HMODULE dll = LoadLibraryExW(L"d3d9.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!dll) { row("load_error\t%lu", GetLastError()); return 5; }
    using Create = IDirect3D9 *(WINAPI *)(UINT);
    using CreateEx = HRESULT (WINAPI *)(UINT,IDirect3D9Ex **);
    auto create = reinterpret_cast<Create>(reinterpret_cast<void *>(GetProcAddress(dll, "Direct3DCreate9")));
    auto create_ex = reinterpret_cast<CreateEx>(reinterpret_cast<void *>(GetProcAddress(dll, "Direct3DCreate9Ex")));
    bool ok = create && create_ex && modules("runtime");
    if (ok) {
        if (!row("enter\tDirect3DCreate9")) return 6;
        IDirect3D9 *classic = create(D3D_SDK_VERSION);
        ok = row("factory\tclassic\t%s", classic ? "available" : "null");
        if (classic) { ok = ok && enumerate("classic", classic, nullptr, native); classic->Release(); }
    }
    if (ok) {
        if (!row("enter\tDirect3DCreate9Ex")) return 6;
        IDirect3D9Ex *ex = nullptr;
        const HRESULT result = create_ex(D3D_SDK_VERSION, &ex);
        ok = row("factory\tex\t%08lx\t%d", (ULONG)result, ex != nullptr);
        if (ex) { ok = ok && enumerate("ex", ex, ex, native); ex->Release(); }
    }
    FreeLibrary(dll);
    return ok && row("complete") ? 0 : 6;
}

int wmain(int argc, wchar_t **argv)
{
    if (argc != 2) return 2;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    journal = CreateFileW(argv[1], GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
                          FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (journal == INVALID_HANDLE_VALUE) return 2;
    const int result = inspect();
    CloseHandle(journal);
    return result;
}
