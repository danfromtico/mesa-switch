/* SPDX-License-Identifier: MIT */
/* Compare read/execute and DLL loading in the caller and a disposable
 * AppContainer. No D3D/Vulkan entry point is called and no DLL ACL is changed.
 * Only this probe's executable temporarily grants its new package SID RX. */
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0a00
#include <windows.h>
#include <userenv.h>
#include <aclapi.h>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

static void json_path(const WCHAR *path)
{
   char text[4096];
   const int n = WideCharToMultiByte(CP_UTF8, 0, path, -1, text, sizeof(text), nullptr, nullptr);
   std::putchar('"');
   for (int i = 0; i < n - 1; ++i) {
      const unsigned char c = text[i];
      if (c == '"' || c == '\\') std::putchar('\\');
      if (c < 32) std::printf("\\u%04x", c); else std::putchar(c);
   }
   std::putchar('"');
}

static int probe(int count, WCHAR **paths, bool expected_container)
{
   HANDLE token = nullptr;
   DWORD appcontainer = 0, size = 0;
   if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return 2;
   const BOOL queried = GetTokenInformation(token, TokenIsAppContainer,
                                            &appcontainer, sizeof(appcontainer), &size);
   CloseHandle(token);
   DWORD session = MAXDWORD, name_bytes = 0;
   WCHAR station[128] = {}, desktop[128] = {};
   ProcessIdToSessionId(GetCurrentProcessId(), &session);
   GetUserObjectInformationW(GetProcessWindowStation(), UOI_NAME, station, sizeof(station), &name_bytes);
   GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME, desktop, sizeof(desktop), &name_bytes);
   std::printf("{\"operation\":\"token\",\"appcontainer\":%lu,\"query_ok\":%s,\"session\":%lu,\"window_station\":",
               appcontainer, queried ? "true" : "false", session);
   json_path(station); std::printf(",\"desktop\":"); json_path(desktop); std::printf("}\n");
   if (!queried || !!appcontainer != expected_container) return 3;
   SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
   for (int i = 0; i < count; ++i) {
      HANDLE file = CreateFileW(paths[i], GENERIC_READ | GENERIC_EXECUTE,
         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
      const DWORD file_error = file == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
      if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
      HMODULE module = LoadLibraryExW(paths[i], nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
      const DWORD load_error = module ? ERROR_SUCCESS : GetLastError();
      std::printf("{\"operation\":\"load\",\"appcontainer\":%lu,\"path\":", appcontainer);
      json_path(paths[i]);
      std::printf(",\"read_execute_error\":%lu,\"load_error\":%lu}\n", file_error, load_error);
      if (module) FreeLibrary(module);
      std::fflush(stdout);
   }
   return 0; /* Observed load failures are data, not a broken probe. */
}

static DWORD launch(PSID sid, const WCHAR *exe, int argc, WCHAR **argv, DWORD *child_exit)
{
   SIZE_T bytes = 0;
   InitializeProcThreadAttributeList(nullptr, 2, 0, &bytes);
   std::vector<unsigned char> storage(bytes);
   auto list = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
   if (!InitializeProcThreadAttributeList(list, 2, 0, &bytes)) return GetLastError();
   HANDLE handles[3] = {};
   DWORD error = ERROR_SUCCESS;
   const DWORD std_ids[] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
   for (unsigned i = 0; i < 3; ++i) {
      if (!DuplicateHandle(GetCurrentProcess(), GetStdHandle(std_ids[i]), GetCurrentProcess(),
                           &handles[i], 0, TRUE, DUPLICATE_SAME_ACCESS)) {
         error = GetLastError(); break;
      }
   }
   SECURITY_CAPABILITIES capabilities = {};
   capabilities.AppContainerSid = sid;
   if (!error && (!UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES,
         &capabilities, sizeof(capabilities), nullptr, nullptr) ||
       !UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
         handles, sizeof(handles), nullptr, nullptr))) error = GetLastError();
   STARTUPINFOEXW startup = {};
   startup.StartupInfo.cb = sizeof(startup);
   startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
   startup.StartupInfo.hStdInput = handles[0];
   startup.StartupInfo.hStdOutput = handles[1];
   startup.StartupInfo.hStdError = handles[2];
   startup.lpAttributeList = list;
   std::wstring command = L"\"" + std::wstring(exe) + L"\" --child";
   for (int i = 1; i < argc; ++i) command += L" \"" + std::wstring(argv[i]) + L"\"";
   PROCESS_INFORMATION process = {};
   std::fflush(stdout);
   if (!error && !CreateProcessW(exe, command.data(), nullptr, nullptr, TRUE,
         EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, nullptr, nullptr,
         &startup.StartupInfo, &process)) error = GetLastError();
   if (process.hProcess) {
      if (WaitForSingleObject(process.hProcess, 30000) != WAIT_OBJECT_0) {
         error = ERROR_TIMEOUT;
         TerminateProcess(process.hProcess, ERROR_TIMEOUT);
         WaitForSingleObject(process.hProcess, 5000);
      }
      if (!GetExitCodeProcess(process.hProcess, child_exit) && !error) error = GetLastError();
      CloseHandle(process.hThread); CloseHandle(process.hProcess);
   }
   for (HANDLE handle : handles) if (handle) CloseHandle(handle);
   DeleteProcThreadAttributeList(list);
   return error;
}

int wmain(int argc, WCHAR **argv)
{
   if (argc < 2) {
      std::fprintf(stderr, "usage: appcontainer-load-check.exe DLL [DLL ...]\n"); return 2;
   }
   if (!std::wcscmp(argv[1], L"--child")) return probe(argc - 2, argv + 2, true);
   for (int i = 1; i < argc; ++i)
      if (std::wcschr(argv[i], L'"')) return 2;
   const int ordinary = probe(argc - 1, argv + 1, false);
   if (ordinary) return ordinary;
   WCHAR exe[MAX_PATH], name[96];
   const DWORD length = GetModuleFileNameW(nullptr, exe, MAX_PATH);
   if (!length || length >= MAX_PATH) return 2;
   std::swprintf(name, 96, L"SwitchWOA.LoadProbe.%lu.%llu", GetCurrentProcessId(), GetTickCount64());
   PSID sid = nullptr;
   const HRESULT created = CreateAppContainerProfile(name, name, L"Temporary driver load check",
                                                      nullptr, 0, &sid);
   if (FAILED(created)) {
      std::printf("{\"operation\":\"profile\",\"status\":\"%08lx\"}\n", (ULONG)created); return 4;
   }
   PSECURITY_DESCRIPTOR saved = nullptr;
   PACL old_acl = nullptr, new_acl = nullptr;
   DWORD error = GetNamedSecurityInfoW(exe, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                                      nullptr, nullptr, &old_acl, nullptr, &saved);
   EXPLICIT_ACCESSW access = {};
   access.grfAccessPermissions = GENERIC_READ | GENERIC_EXECUTE;
   access.grfAccessMode = GRANT_ACCESS;
   access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
   access.Trustee.TrusteeType = TRUSTEE_IS_GROUP;
   access.Trustee.ptstrName = static_cast<LPWSTR>(sid);
   if (!error) error = SetEntriesInAclW(1, &access, old_acl, &new_acl);
   bool granted = false;
   if (!error) {
      error = SetNamedSecurityInfoW(exe, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                                   nullptr, nullptr, new_acl, nullptr);
      granted = !error;
   }
   DWORD child_exit = MAXDWORD;
   if (!error) error = launch(sid, exe, argc, argv, &child_exit);
   DWORD restored = ERROR_SUCCESS;
   if (granted) restored = SetNamedSecurityInfoW(exe, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                                                nullptr, nullptr, old_acl, nullptr);
   if (new_acl) LocalFree(new_acl);
   if (saved) LocalFree(saved);
   FreeSid(sid);
   const HRESULT deleted = DeleteAppContainerProfile(name);
   std::printf("{\"operation\":\"complete\",\"launch_error\":%lu,\"child_exit\":%lu,"
               "\"acl_restore_error\":%lu,\"profile_delete_status\":\"%08lx\"}\n",
               error, child_exit, restored, (ULONG)deleted);
   return error || child_exit || restored || FAILED(deleted) ? 1 : 0;
}
