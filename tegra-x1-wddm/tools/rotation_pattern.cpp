/* SPDX-License-Identifier: MIT
 * Asymmetric logical-desktop pattern for physical rotation acceptance.
 * This draws through GDI/DWM; it neither rotates a bitmap nor changes a mode.
 * Screen capture can check the logical pixels, not the physical scanout.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cwchar>

static ULONGLONG started;
static unsigned duration;
static unsigned paints, clicks;
static POINT last_click = {-1, -1};
static const WCHAR *ready_path;
static const char *reason = "UserClosed";

static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM w, LPARAM l)
{
   if (message == WM_PAINT) {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(window, &ps);
      HFONT font = CreateFontW(-28, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
         DEFAULT_PITCH, L"Segoe UI");
      HGDIOBJ old_font = font ? SelectObject(dc, font) : nullptr;
      RECT area;
      GetClientRect(window, &area);
      const int width = area.right, height = area.bottom;
      const COLORREF colors[] = {RGB(255,0,0), RGB(0,255,0), RGB(0,0,255), RGB(255,255,0)};
      const WCHAR *labels[] = {L"1  TOP LEFT", L"2  TOP RIGHT", L"3  BOTTOM LEFT", L"4  BOTTOM RIGHT"};
      for (int i = 0; i < 4; ++i) {
         RECT cell = {(i & 1) * width / 2, (i / 2) * height / 2,
                      ((i & 1) + 1) * width / 2, (i / 2 + 1) * height / 2};
         HBRUSH brush = CreateSolidBrush(colors[i]);
         FillRect(dc, &cell, brush);
         DeleteObject(brush);
         SetTextColor(dc, i == 2 ? RGB(255,255,255) : RGB(0,0,0));
         SetBkMode(dc, TRANSPARENT);
         const int label_top = i < 2 ? 32 : height - 80;
         RECT text = {cell.left + 32, label_top, cell.right - 32, label_top + 48};
         DrawTextW(dc, labels[i], -1, &text, DT_LEFT | DT_SINGLELINE);
      }
      /* One desktop pixel at each edge, with no nonclient frame. */
      FrameRect(dc, &area, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
      const int center = width / 2;
      POINT arrow[] = {{center, 90}, {center - 38, 145}, {center - 12, 145},
         {center - 12, 230}, {center + 12, 230}, {center + 12, 145}, {center + 38, 145}};
      HGDIOBJ old_brush = SelectObject(dc, GetStockObject(BLACK_BRUSH));
      HGDIOBJ old_pen = SelectObject(dc, GetStockObject(WHITE_PEN));
      Polygon(dc, arrow, sizeof(arrow) / sizeof(arrow[0]));
      SelectObject(dc, old_pen);
      SelectObject(dc, old_brush);
      RECT banner = {center - 230, height / 2 - 36, center + 230, height / 2 + 54};
      FillRect(dc, &banner, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
      SetTextColor(dc, RGB(255,255,255));
      WCHAR text[160];
      swprintf(text, 160, L"TOP IS ABOVE THE ARROW\n%d x %d | click: %ld,%ld | Esc closes",
               width, height, last_click.x, last_click.y);
      DrawTextW(dc, text, -1, &banner, DT_CENTER);
      ++paints;
      if (font) { SelectObject(dc, old_font); DeleteObject(font); }
      EndPaint(window, &ps);
      return 0;
   }
   if (message == WM_LBUTTONDOWN) {
      last_click = {(SHORT)LOWORD(l), (SHORT)HIWORD(l)};
      ++clicks;
      std::printf("{\"event\":\"click\",\"x\":%ld,\"y\":%ld}\n", last_click.x, last_click.y);
      std::fflush(stdout);
      RECT area;
      GetClientRect(window, &area);
      RECT banner = {area.right / 2 - 230, area.bottom / 2 - 36,
                     area.right / 2 + 230, area.bottom / 2 + 54};
      InvalidateRect(window, &banner, FALSE);
      return 0;
   }
   if (message == WM_KEYDOWN && w == VK_ESCAPE) {
      reason = "Escape";
      DestroyWindow(window);
      return 0;
   }
   if (message == WM_TIMER && GetTickCount64() - started >= duration * 1000ULL) {
      reason = "DurationReached";
      DestroyWindow(window);
      return 0;
   }
   if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
   return DefWindowProcW(window, message, w, l);
}

int wmain(int argc, WCHAR **argv)
{
   if (argc != 3) { std::fprintf(stderr, "usage: rotation-pattern.exe seconds(10..600) ready.json\n"); return 2; }
   WCHAR *end = nullptr;
   duration = std::wcstoul(argv[1], &end, 10);
   if (!end || *end || duration < 10 || duration > 600) return 2;
   ready_path = argv[2];
   SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
   DWORD session = 0;
   ProcessIdToSessionId(GetCurrentProcessId(), &session);
   const int width = GetSystemMetrics(SM_CXSCREEN), height = GetSystemMetrics(SM_CYSCREEN);
   if (session != WTSGetActiveConsoleSessionId() ||
       !((width == 1280 && height == 720) || (width == 720 && height == 1280))) {
      std::fprintf(stderr, "Expected the interactive native panel extent\n"); return 3;
   }
   HINSTANCE instance = GetModuleHandleW(nullptr);
   WNDCLASSW type = {};
   type.lpfnWndProc = window_proc; type.hInstance = instance;
   type.hCursor = LoadCursorW(nullptr, IDC_ARROW); type.lpszClassName = L"Tgd3dRotationPattern";
   if (!RegisterClassW(&type)) return 4;
   HWND window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, type.lpszClassName,
      L"tgd3d rotation acceptance", WS_POPUP, 0, 0, width, height, nullptr, nullptr, instance, nullptr);
   if (!window) return 5;
   ShowWindow(window, SW_SHOWNORMAL);
   ShowWindow(window, SW_SHOW); /* Override a hidden launch's initial show state. */
   SetWindowPos(window, HWND_TOPMOST, 0, 0, width, height, SWP_SHOWWINDOW);
   SetCursorPos(width / 2, height / 2);
   UpdateWindow(window);
   started = GetTickCount64();
   if (!IsWindowVisible(window) || !SetTimer(window, 1, 250, nullptr)) { DestroyWindow(window); return 6; }
   char ready[320];
   const int bytes = std::snprintf(ready, sizeof(ready),
      "{\"pid\":%lu,\"session\":%lu,\"hwnd\":%llu,\"width\":%d,\"height\":%d,"
      "\"uptime_ms\":%llu,\"physical_acceptance\":false}\n", GetCurrentProcessId(), session,
      (unsigned long long)(UINT_PTR)window, width, height, started);
   HANDLE file = CreateFileW(ready_path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
                             FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
   DWORD written = 0;
   bool ok = file != INVALID_HANDLE_VALUE && WriteFile(file, ready, bytes, &written, nullptr) &&
      written == (DWORD)bytes && FlushFileBuffers(file);
   if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
   if (!ok) { DestroyWindow(window); return 7; }
   std::printf("%s", ready); std::fflush(stdout);
   MSG message;
   BOOL next;
   while ((next = GetMessageW(&message, nullptr, 0, 0)) > 0) {
      TranslateMessage(&message); DispatchMessageW(&message);
   }
   std::printf("{\"event\":\"finished\",\"reason\":\"%s\",\"elapsed_ms\":%llu,"
      "\"paints\":%u,\"clicks\":%u}\n", reason, GetTickCount64() - started, paints, clicks);
   return next == -1 ? 8 : 0;
}
