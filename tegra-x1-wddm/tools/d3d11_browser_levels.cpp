/* SPDX-License-Identifier: MIT
 * Compare Windows D3D11 feature negotiation on the KMT-identified GM20B.
 * The D3D runtime receives null device/context outputs for each level query.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi1_6.h>
#include <d3d11.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>

extern "C" {
#include "kmt.h"
}

static HANDLE journal = INVALID_HANDLE_VALUE;

static bool record(const char *fmt, ...)
{
    char line[1024];
    va_list args;
    va_start(args, fmt);
    int length = std::vsnprintf(line, sizeof(line) - 1, fmt, args);
    va_end(args);
    if (length < 0 || static_cast<size_t>(length) >= sizeof(line) - 1)
        return false;
    line[length++] = '\n';
    DWORD written = 0;
    return WriteFile(journal, line, static_cast<DWORD>(length), &written, nullptr) &&
           written == static_cast<DWORD>(length) && FlushFileBuffers(journal);
}

static unsigned long long luid_value(LUID id)
{
    return (static_cast<unsigned long long>(static_cast<UINT32>(id.HighPart)) << 32) | id.LowPart;
}

static bool query(const char *case_name, IDXGIAdapter *adapter, D3D_DRIVER_TYPE type,
                  UINT flags, const D3D_FEATURE_LEVEL *levels, UINT count)
{
    if (!record("query_enter,%s,0x%x,0x%x,%u", case_name, flags, levels[0], count))
        return false;
    D3D_FEATURE_LEVEL actual{};
    HRESULT hr = D3D11CreateDevice(adapter, type, nullptr, flags, levels, count, D3D11_SDK_VERSION,
                                   nullptr, &actual, nullptr);
    return record("query_result,%s,0x%08lx,0x%x", case_name, static_cast<unsigned long>(hr), actual);
}

static int inspect()
{
    tg_kmt_scan scan{};
    LONG status = tg_gpu_kmt_scan_system(&scan);
    const auto &info = scan.first_ready;
    const unsigned long long native =
        static_cast<unsigned long long>(static_cast<UINT32>(info.luid_high)) << 32 | info.luid_low;
    if (!record("kmt,0x%08lx,%u,%016llx,%04x,%03x,%u", static_cast<unsigned long>(status),
                scan.native_ready, native, info.vendor, info.chipset, info.sm) ||
        status < 0 || scan.native_ready != 1 || !native)
        return 3;
    IDXGIFactory1 *factory = nullptr;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory));
    if (FAILED(hr) || !record("factory,0x%08lx", static_cast<unsigned long>(hr)))
        return 4;
    IDXGIAdapter1 *tegra = nullptr;
    for (UINT i = 0; ; ++i) {
        IDXGIAdapter1 *adapter = nullptr;
        hr = factory->EnumAdapters1(i, &adapter);
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(hr)) { factory->Release(); return 4; }
        DXGI_ADAPTER_DESC1 desc{};
        hr = adapter->GetDesc1(&desc);
        if (FAILED(hr)) { adapter->Release(); factory->Release(); return 4; }
        char name[480]{};
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), nullptr, nullptr);
        IDXGIOutput *output = nullptr;
        HRESULT output_hr = adapter->EnumOutputs(0, &output);
        if (output) output->Release();
        const unsigned long long id = luid_value(desc.AdapterLuid);
        if (!record("adapter,%u,%016llx,0x%x,0x%x,0x%x,0x%08lx,%s", i, id, desc.VendorId,
                    desc.DeviceId, desc.Flags, static_cast<unsigned long>(output_hr), name)) {
            adapter->Release(); factory->Release(); return 4;
        }
        if (id == native && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            if (tegra) { adapter->Release(); tegra->Release(); factory->Release(); return 4; }
            tegra = adapter;
        } else {
            adapter->Release();
        }
    }
    IDXGIFactory6 *by_pref = nullptr;
    if (SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory6), reinterpret_cast<void **>(&by_pref)))) {
        for (UINT i = DXGI_GPU_PREFERENCE_UNSPECIFIED; i <= DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE; ++i) {
            IDXGIAdapter1 *first = nullptr;
            hr = by_pref->EnumAdapterByGpuPreference(0, static_cast<DXGI_GPU_PREFERENCE>(i),
                                                     __uuidof(IDXGIAdapter1), reinterpret_cast<void **>(&first));
            DXGI_ADAPTER_DESC1 desc{};
            if (first) { first->GetDesc1(&desc); first->Release(); }
            record("preference,%u,0x%08lx,%016llx", i, static_cast<unsigned long>(hr), luid_value(desc.AdapterLuid));
        }
        by_pref->Release();
    }
    factory->Release();
    if (!tegra) return 5;
    const D3D_FEATURE_LEVEL need = D3D_FEATURE_LEVEL_11_0;
    const D3D_FEATURE_LEVEL range[]{D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0,
                                    D3D_FEATURE_LEVEL_10_1,D3D_FEATURE_LEVEL_10_0};
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    bool complete = query("warp-11", nullptr, D3D_DRIVER_TYPE_WARP, D3D11_CREATE_DEVICE_BGRA_SUPPORT, &need, 1) &&
                    query("tegra-video-11", tegra, D3D_DRIVER_TYPE_UNKNOWN, flags, &need, 1) &&
                    query("tegra-video-range", tegra, D3D_DRIVER_TYPE_UNKNOWN, flags, range, ARRAYSIZE(range));
    tegra->Release();
    return complete && record("complete") ? 0 : 6;
}

int wmain(int argc, wchar_t **argv)
{
    if (argc != 2) return 2;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    journal = CreateFileW(argv[1], GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
                          FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (journal == INVALID_HANDLE_VALUE) return 2;
    int result = inspect();
    CloseHandle(journal);
    return result;
}
