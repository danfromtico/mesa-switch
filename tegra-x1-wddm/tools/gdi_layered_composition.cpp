// SPDX-License-Identifier: MIT
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dwmapi.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <cwchar>

using Microsoft::WRL::ComPtr;

namespace
{
constexpr int width = 257;
constexpr int height = 119;
constexpr int inset = 2;
constexpr unsigned expected = (width - 2 * inset) * (height - 2 * inset);
constexpr UINT swoa_vendor = 0x414f5753;
constexpr UINT swoa_device = 0x31304630;
constexpr DWORD no_redirection_bitmap = 0x00200000; // WS_EX_NOREDIRECTIONBITMAP in the Windows SDK.

uint64_t qpc()
{
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

void mark(const char *stage)
{
    std::printf("MARK %s qpc=%llu tid=%lu\n", stage,
                static_cast<unsigned long long>(qpc()), GetCurrentThreadId());
}

void require(bool ok, const char *condition)
{
    if (!ok) {
        std::printf("FAILED %s\n", condition);
        throw E_FAIL;
    }
}

void check_win32(bool ok, const char *operation)
{
    if (!ok) {
        std::printf("FAILED %s win32=%lu\n", operation, GetLastError());
        throw E_FAIL;
    }
}

void checked(HRESULT result, const char *operation)
{
    std::printf("CALL %s hr=%08lx qpc=%llu\n", operation, static_cast<unsigned long>(result),
                static_cast<unsigned long long>(qpc()));
    if (FAILED(result))
        throw result;
}

uint32_t color(int x, int y, unsigned frame)
{
    return 0xff000000u | ((71*x + 3*y + 11 + 43*frame) & 255u) << 16 |
           ((5*x + 29*y + 67 + 17*frame) & 255u) << 8 |
           ((7*x + 43*y + 103 + 89*frame) & 255u);
}

class Dib
{
public:
    void allocate()
    {
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        dc = CreateCompatibleDC(nullptr);
        require(dc != nullptr, "CreateCompatibleDC");
        void *pixels = nullptr;
        bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        bits = static_cast<uint32_t *>(pixels);
        check_win32(bitmap && bits, "CreateDIBSection");
        previous = SelectObject(dc, bitmap);
        require(previous && previous != HGDI_ERROR, "SelectObject");
    }

    ~Dib()
    {
        if (previous && previous != HGDI_ERROR)
            SelectObject(dc, previous);
        if (bitmap)
            DeleteObject(bitmap);
        if (dc)
            DeleteDC(dc);
    }

    HDC dc = nullptr;
    uint32_t *bits = nullptr;

private:
    HBITMAP bitmap = nullptr;
    HGDIOBJ previous = nullptr;
};

void pump(unsigned milliseconds)
{
    const auto deadline = GetTickCount64() + milliseconds;
    do {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        Sleep(12);
    } while (GetTickCount64() < deadline);
}

ComPtr<IDXGIDevice> native_device(uint64_t target)
{
    ComPtr<IDXGIFactory1> factory;
    checked(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");
    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> adapter;
        const HRESULT enumeration = factory->EnumAdapters1(index, &adapter);
        if (enumeration == DXGI_ERROR_NOT_FOUND)
            break;
        checked(enumeration, "EnumAdapters1");
        DXGI_ADAPTER_DESC1 info{};
        checked(adapter->GetDesc1(&info), "GetDesc1");
        const uint64_t luid = (uint64_t(uint32_t(info.AdapterLuid.HighPart)) << 32) | info.AdapterLuid.LowPart;
        const bool software = (info.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
        std::printf("ADAPTER luid=%016llx vendor=%08x device=%08x software=%u\n",
                    static_cast<unsigned long long>(luid), info.VendorId, info.DeviceId,
                    static_cast<unsigned>(software));
        if (luid != target || software)
            continue;
        // The native ACPI adapter reports SWOA0F01 through DXGI.
        require(info.VendorId == swoa_vendor && info.DeviceId == swoa_device, "native SWOA0F01 adapter");
        ComPtr<ID3D11Device> device;
        D3D_FEATURE_LEVEL level{};
        checked(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                  nullptr, 0, D3D11_SDK_VERSION, &device, &level, nullptr), "D3D11CreateNative");
        std::printf("NATIVE luid=%016llx feature=%x\n", static_cast<unsigned long long>(luid),
                    static_cast<unsigned>(level));
        ComPtr<IDXGIDevice> dxgi;
        checked(device.As(&dxgi), "IDXGIDevice");
        return dxgi;
    }
    require(false, "native adapter LUID");
    return {};
}

class Scene
{
    bool close_windows()
    {
        wrapper.Reset();
        visual.Reset();
        target.Reset();
        composition.Reset();
        if (mirror && DestroyWindow(mirror))
            mirror = nullptr;
        if (source && DestroyWindow(source))
            source = nullptr;
        const bool closed = !source && !mirror;
        if (IsWindow(previous))
            SetForegroundWindow(previous);
        if (closed)
            mark("WindowsClosed");
        return closed;
    }

public:
    ~Scene()
    {
        if (source || mirror)
            close_windows();
    }

    void open(IDXGIDevice *dxgi)
    {
        require(GetSystemMetrics(SM_CXSCREEN) >= 650 && GetSystemMetrics(SM_CYSCREEN) >= 310, "desktop bounds");
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSW wc{};
        wc.hInstance = instance;
        wc.lpfnWndProc = window_proc;
        wc.lpszClassName = L"SwitchLayeredGdiProducer";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        check_win32(RegisterClassW(&wc) != 0, "RegisterClass");
        staging.allocate();
        capture.allocate();
        mark("WindowsBegin");
        source = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST, wc.lpszClassName, L"Layered GDI source",
                                 WS_POPUP, 40, 140, width, height, nullptr, nullptr, instance, this);
        check_win32(source != nullptr, "create layered GDI window");
        update(0);
        ShowWindow(source, SW_SHOW);
        const auto deadline = GetTickCount64() + 2600;
        unsigned source_matches;
        do {
            flush_desktop();
            source_matches = visible(source, 0);
        } while (source_matches != expected && GetTickCount64() < deadline);
        std::printf("LAYERED_ONLY pixels=%u expected=%u\n", source_matches, expected);
        require(source_matches == expected, "layered source readback");
        mark("LayeredOnlyEnd");

        mirror = CreateWindowExW(no_redirection_bitmap | WS_EX_TOPMOST | WS_EX_NOACTIVATE, wc.lpszClassName,
                                 L"DirectComposition mirror", WS_POPUP, 335, 140, width, height,
                                 nullptr, nullptr, instance, this);
        check_win32(mirror != nullptr, "create DirectComposition target window");
        std::printf("WINDOW source=%llu mirror=%llu width=%d height=%d\n",
                    static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(source)),
                    static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(mirror)), width, height);
        ShowWindow(mirror, SW_SHOWNOACTIVATE);
        checked(DCompositionCreateDevice(dxgi, IID_PPV_ARGS(&composition)), "DCompositionCreateDevice");
        checked(composition->CreateTargetForHwnd(mirror, TRUE, &target), "CreateTargetForHwnd");
        checked(composition->CreateVisual(&visual), "CreateVisual");
        mark("WrapLayeredWindowBegin");
        checked(composition->CreateSurfaceFromHwnd(source, &wrapper), "CreateSurfaceFromHwnd");
        checked(visual->SetContent(wrapper.Get()), "SetContent");
        checked(target->SetRoot(visual.Get()), "SetRoot");
        checked(composition->Commit(), "Commit");
        checked(composition->WaitForCommitCompletion(), "WaitForCommitCompletion");
        mark("WrapLayeredWindowEnd");
    }

    void verify(unsigned frame)
    {
        const auto deadline = GetTickCount64() + 2600;
        unsigned source_matches, mirror_matches;
        do {
            flush_desktop();
            source_matches = visible(source, frame);
            mirror_matches = visible(mirror, frame);
        } while ((source_matches != expected || mirror_matches != expected) && GetTickCount64() < deadline);
        std::printf("LAYERED_RGB frame=%u source=%u mirror=%u pixels_per_window=%u screen_format=BGRX32 screen_pitch=%d qpc=%llu\n",
                    frame, source_matches, mirror_matches, expected, width * 4, static_cast<unsigned long long>(qpc()));
        require(source_matches == expected && mirror_matches == expected, "both layered frame readbacks");
    }

    void input()
    {
        SetForegroundWindow(source);
        pump(100);
        require(GetForegroundWindow() == source, "own source foreground");
        SetFocus(source);
        require(GetFocus() == source && GetActiveWindow() == source, "own source keyboard focus");
        INPUT events[2]{};
        events[0].type = events[1].type = INPUT_KEYBOARD;
        events[0].ki.wVk = events[1].ki.wVk = VK_F8;
        events[0].ki.wScan = events[1].ki.wScan =
            static_cast<WORD>(MapVirtualKeyW(VK_F8, MAPVK_VK_TO_VSC));
        events[1].ki.dwFlags = KEYEVENTF_KEYUP;
        mark("OwnedKeyboardBegin");
        const UINT sent = SendInput(2, events, sizeof(INPUT));
        const auto deadline = GetTickCount64() + 2600;
        while ((!received || !released) && GetTickCount64() < deadline)
            pump(12);
        std::printf("INPUT keys_sent=%u F8_received=%u F8_released=%u source_focused=%u\n",
                    sent, received, released, static_cast<unsigned>(GetFocus() == source));
        require(sent == 2 && received == 1 && released == 1, "owned source input");
        mark("OwnedKeyboardEnd");
        update(1);
    }

    void finish()
    {
        require(close_windows(), "close owned windows");
        flush_desktop();
    }

private:
    static void flush_desktop()
    {
        pump(45);
        const HRESULT flushed = DwmFlush();
        if (FAILED(flushed))
            checked(flushed, "DwmFlush");
    }

    static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
    {
        if (message == WM_NCCREATE) {
            auto *create = reinterpret_cast<CREATESTRUCTW *>(lparam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        }
        auto *scene = reinterpret_cast<Scene *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (scene && hwnd == scene->source && wparam == VK_F8 && (message == WM_KEYDOWN || message == WM_KEYUP)) {
            if (message == WM_KEYDOWN)
                ++scene->received;
            else
                ++scene->released;
            return 0;
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    void update(unsigned frame)
    {
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
                staging.bits[y * width + x] = color(x, y, frame);
        POINT origin{0, 0};
        SIZE extent{width, height};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        check_win32(UpdateLayeredWindow(source, nullptr, nullptr, &extent, staging.dc, &origin,
                                       0, &blend, ULW_ALPHA) != FALSE, "UpdateLayeredWindow");
        require(GdiFlush() != FALSE, "GdiFlush");
    }

    unsigned visible(HWND window, unsigned frame)
    {
        POINT position{};
        require(ClientToScreen(window, &position) != FALSE, "ClientToScreen");
        HDC screen = GetDC(nullptr);
        require(screen != nullptr, "screen DC");
        const BOOL copied = BitBlt(capture.dc, 0, 0, width, height, screen, position.x, position.y, SRCCOPY | CAPTUREBLT);
        const BOOL flushed = GdiFlush();
        ReleaseDC(nullptr, screen);
        require(copied && flushed, "screen RGB readback");
        unsigned matches = 0;
        for (int y = inset; y < height - inset; ++y)
            for (int x = inset; x < width - inset; ++x)
                matches += ((capture.bits[y * width + x] ^ color(x, y, frame)) & 0x00ffffffu) == 0;
        return matches;
    }

    HWND previous = GetForegroundWindow();
    HWND source = nullptr;
    HWND mirror = nullptr;
    Dib staging, capture;
    unsigned received = 0;
    unsigned released = 0;
    ComPtr<IDCompositionDevice> composition;
    ComPtr<IDCompositionTarget> target;
    ComPtr<IDCompositionVisual> visual;
    ComPtr<IUnknown> wrapper;
};
}

int wmain(int argc, wchar_t **argv)
{
    if (argc != 2 || std::wcslen(argv[1]) != 16 ||
        std::wcsspn(argv[1], L"0123456789abcdefABCDEF") != 16)
        return 2;
    SetProcessDPIAware();
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    try {
        auto dxgi = native_device(std::wcstoull(argv[1], nullptr, 16));
        {
            Scene scene;
            scene.open(dxgi.Get());
            scene.verify(0);
            scene.input();
            scene.verify(1);
            mark("GdiOnlyEnd");
            scene.finish();
        }
        std::puts("EXIT normal");
        return 0;
    } catch (HRESULT error) {
        std::printf("EXIT error=%08lx\n", static_cast<unsigned long>(error));
        return 1;
    }
}
