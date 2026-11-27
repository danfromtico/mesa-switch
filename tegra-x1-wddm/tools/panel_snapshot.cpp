/* SPDX-License-Identifier: MIT */
/* Read the selected native adapter's scanout boundary. No device creation,
 * rendering, display-mode change, register programming or GPU submission.
 * Exit zero means a snapshot was returned, not physical display acceptance.
 */
#include <windows.h>
#include <d3dkmthk.h>
#include <cstdio>
#include <cwchar>
#include "../windows/panel_snapshot.h"

static bool enable_debug()
{
   HANDLE token = nullptr;
   if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
      return false;
   TOKEN_PRIVILEGES privilege = {};
   privilege.PrivilegeCount = 1;
   bool ok = LookupPrivilegeValueW(nullptr, L"SeDebugPrivilege", &privilege.Privileges[0].Luid);
   privilege.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
   if (ok) {
      SetLastError(ERROR_SUCCESS);
      ok = AdjustTokenPrivileges(token, FALSE, &privilege, 0, nullptr, nullptr) &&
           GetLastError() == ERROR_SUCCESS;
   }
   CloseHandle(token);
   return ok;
}

int wmain(int argc, wchar_t **argv)
{
   static_assert(sizeof(tg_gpu_panel_snapshot) == 192, "panel snapshot ABI");
   if (argc != 2 || std::wcslen(argv[1]) != 16) {
      std::fputs("usage: panel-snapshot.exe <16-hex-digit-native-LUID>\n", stderr);
      return 2;
   }
   uint64_t luid = 0;
   for (unsigned i = 0; i < 16; ++i) {
      const wchar_t c = argv[1][i];
      const unsigned digit = c >= L'0' && c <= L'9' ? c - L'0' :
         c >= L'a' && c <= L'f' ? c - L'a' + 10 : c >= L'A' && c <= L'F' ? c - L'A' + 10 : 16;
      if (digit > 15) return 2;
      luid = (luid << 4) | digit;
   }
   if (!enable_debug()) {
      std::puts("{\"error\":\"debug privilege required\"}");
      return 3;
   }
   HMODULE gdi = LoadLibraryW(L"gdi32.dll");
   if (!gdi) return 4;
   const auto open = reinterpret_cast<PFND3DKMT_OPENADAPTERFROMLUID>(GetProcAddress(gdi, "D3DKMTOpenAdapterFromLuid"));
   const auto close = reinterpret_cast<PFND3DKMT_CLOSEADAPTER>(GetProcAddress(gdi, "D3DKMTCloseAdapter"));
   const auto escape = reinterpret_cast<PFND3DKMT_ESCAPE>(GetProcAddress(gdi, "D3DKMTEscape"));
   if (!open || !close || !escape) { FreeLibrary(gdi); return 4; }
   D3DKMT_OPENADAPTERFROMLUID adapter = {};
   adapter.AdapterLuid = {static_cast<DWORD>(luid), static_cast<LONG>(luid >> 32)};
   NTSTATUS status = open(&adapter);
   tg_gpu_panel_snapshot s = {};
   s.magic = TG_GPU_PANEL_SNAPSHOT_MAGIC;
   s.version = TG_GPU_PANEL_SNAPSHOT_VERSION;
   s.size = sizeof(s);
   if (status >= 0) {
      D3DKMT_ESCAPE request = {};
      request.hAdapter = adapter.hAdapter;
      request.Type = D3DKMT_ESCAPE_DRIVERPRIVATE;
      request.Flags.HardwareAccess = 1;
      request.pPrivateDriverData = &s;
      request.PrivateDriverDataSize = sizeof(s);
      status = escape(&request);
      D3DKMT_CLOSEADAPTER done = {adapter.hAdapter};
      (void)close(&done);
   }
   FreeLibrary(gdi);
   SYSTEMTIME utc;
   GetSystemTime(&utc);
   std::printf("{\"utc\":\"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ\",\"status\":\"%08x\","
               "\"requested_luid\":\"%016llx\"", utc.wYear, utc.wMonth, utc.wDay,
               utc.wHour, utc.wMinute, utc.wSecond, utc.wMilliseconds,
               static_cast<unsigned>(status), static_cast<unsigned long long>(luid));
   if (status < 0) { std::puts("}"); return 5; }
   if (s.magic != TG_GPU_PANEL_SNAPSHOT_MAGIC || s.version != TG_GPU_PANEL_SNAPSHOT_VERSION ||
       s.size != sizeof(s) || s.luid_low != static_cast<uint32_t>(luid) ||
       static_cast<uint32_t>(s.luid_high) != static_cast<uint32_t>(luid >> 32)) {
      std::puts(",\"error\":\"snapshot identity mismatch\"}");
      return 6;
   }
   std::printf(",\"flags\":%u,\"path_rotation\":%u,\"source_visible\":%u,\"target_active\":%u,"
      "\"state_before\":%u,\"state_after\":%u,\"interrupt_status\":%u,\"underflow\":%u,"
      "\"options\":%u,\"pitch\":%u,\"h_offset\":%u,\"v_offset\":%u,\"surface_kind\":%u,"
      "\"output_size\":%u,\"prescaled_size\":%u,\"dda\":%u,\"panel_active\":%u,"
      "\"address\":\"%016llx\",\"pending_address\":\"%016llx\",\"displayed_address\":\"%016llx\","
      "\"pending_present\":%llu,\"displayed_present\":%llu,\"rotation\":%u,\"width\":%u,\"height\":%u,\"pixels\":[",
      s.flags, s.path_rotation, s.source_visible, s.target_active,
      s.state_control_before, s.state_control_after, s.interrupt_status, s.underflow,
      s.options, s.pitch, s.h_offset, s.v_offset, s.surface_kind,
      s.output_size, s.prescaled_size, s.dda, s.panel_active,
      static_cast<unsigned long long>(s.address), static_cast<unsigned long long>(s.pending_address),
      static_cast<unsigned long long>(s.displayed_address), static_cast<unsigned long long>(s.pending_present),
      static_cast<unsigned long long>(s.displayed_present), s.rotation, s.width, s.height);
   for (unsigned i = 0; i < 4; ++i)
      std::printf("%s{\"x\":%u,\"y\":%u,\"bgra\":\"%08x\"}", i ? "," : "",
                  s.pixels[i].x, s.pixels[i].y, s.pixels[i].bgra);
   std::puts("]}");
   return 0;
}
