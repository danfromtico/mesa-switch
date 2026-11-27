/* SPDX-License-Identifier: MIT
 * Capture a user stack dump before native-desktop recovery removes the UMD.
 * Uses the Windows PSS/DbgHelp snapshot contract; never attaches a debugger
 * or writes a full process-memory dump. See the trial README for references.
 */
#ifndef NTDDI_VERSION
#define NTDDI_VERSION 0x06030000
#endif
#include <windows.h>
/* Use the SDK header: llvm-mingw's older DbgHelp header lacks I/O callbacks. */
#include <minidumpapiset.h>
#include <processsnapshot.h>
#include <psapi.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <wchar.h>

static uint64_t filetime(FILETIME t)
{
   return uint64_t(t.dwHighDateTime) << 32 | t.dwLowDateTime;
}

static int failure(const char *stage, DWORD error)
{
   printf("{\"stage\":\"%s\",\"error\":%lu}\n", stage, error);
   return 1;
}

static DWORD WINAPI deadline(void *)
{
   Sleep(25000);
   /* Dumping a snapshot avoids leaving live DWM threads suspended if the
    * writer must be terminated. The deadline survives a disconnected SSH. */
   TerminateProcess(GetCurrentProcess(), 124);
   return 0;
}

struct Writer {
   HANDLE file;
   ULONGLONG started;
};

static BOOL CALLBACK dump_callback(void *param, const PMINIDUMP_CALLBACK_INPUT in,
                                   MINIDUMP_CALLBACK_OUTPUT *out)
{
   auto *writer = static_cast<Writer *>(param);
   switch (in->CallbackType) {
   case IsProcessSnapshotCallback:
      out->Status = S_FALSE;
      break;
   case CancelCallback:
      out->CheckCancel = TRUE;
      out->Cancel = GetTickCount64() - writer->started >= 20000;
      break;
   case IoStartCallback:
      out->Status = S_FALSE; /* Use bounded file I/O below. */
      break;
   case IoWriteAllCallback: {
      constexpr uint64_t limit = 32 * 1024 * 1024;
      if (in->Io.Offset > limit || in->Io.BufferBytes > limit - in->Io.Offset) {
         out->Status = HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
         return FALSE;
      }
      LARGE_INTEGER at;
      at.QuadPart = in->Io.Offset;
      DWORD written = 0;
      if (!SetFilePointerEx(writer->file, at, nullptr, FILE_BEGIN) ||
          !WriteFile(writer->file, in->Io.Buffer, in->Io.BufferBytes, &written, nullptr)) {
         out->Status = HRESULT_FROM_WIN32(GetLastError());
         return FALSE;
      }
      out->Status = written == in->Io.BufferBytes ? S_OK : E_FAIL;
      return SUCCEEDED(out->Status);
   }
   case IoFinishCallback:
      out->Status = S_OK;
      break;
   default:
      break;
   }
   return TRUE;
}

int wmain(int argc, wchar_t **argv)
{
   setvbuf(stdout, nullptr, _IONBF, 0);
   SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
   if (argc == 2 && !wcscmp(argv[1], L"--fixture")) {
      FILETIME created, exited, kernel, user;
      if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
         return failure("fixture-identity", GetLastError());
      printf("{\"pid\":%lu,\"creation_filetime\":%llu}\n",
             GetCurrentProcessId(), (unsigned long long)filetime(created));
      Sleep(20000); /* CPU-only control target; no graphics device/window. */
      return 0;
   }
   if (argc != 5) {
      fputs("usage: tgd3d-dump-capture.exe PID creation-FILETIME image-name output.dmp\n", stderr);
      return 2;
   }
   wchar_t *end = nullptr;
   unsigned long pid = wcstoul(argv[1], &end, 10);
   if (!pid || *end) return 2;
   uint64_t expected = wcstoull(argv[2], &end, 10);
   if (!expected || *end || !*argv[3]) return 2;

   HANDLE job = CreateJobObjectW(nullptr, nullptr);
   JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
   limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_PROCESS_MEMORY;
   limits.ProcessMemoryLimit = 256 * 1024 * 1024;
   if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                       &limits, sizeof(limits)) ||
       !AssignProcessToJobObject(job, GetCurrentProcess()))
      return failure("bound-writer-memory", GetLastError());
   HANDLE watchdog = CreateThread(nullptr, 0, deadline, nullptr, 0, nullptr);
   if (!watchdog) return failure("start-deadline", GetLastError());
   CloseHandle(watchdog);

   /* SeDebugPrivilege is enabled only in this short-lived process. A denied
    * protected-process capture is reported, never bypassed by changing policy. */
   HANDLE token = nullptr;
   if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
      TOKEN_PRIVILEGES privileges = {};
      privileges.PrivilegeCount = 1;
      if (LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &privileges.Privileges[0].Luid)) {
         privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
         SetLastError(ERROR_SUCCESS);
         AdjustTokenPrivileges(token, FALSE, &privileges, 0, nullptr, nullptr);
         printf("{\"stage\":\"debug-privilege\",\"error\":%lu}\n",
                GetLastError());
      }
      CloseHandle(token);
   }
   HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ |
                                PROCESS_CREATE_PROCESS | PROCESS_DUP_HANDLE | SYNCHRONIZE,
                                FALSE, pid);
   if (!process) return failure("open-target", GetLastError());
   FILETIME created, exited, kernel, user;
   wchar_t image[MAX_PATH];
   DWORD length = MAX_PATH;
   if (!GetProcessTimes(process, &created, &exited, &kernel, &user) ||
       !QueryFullProcessImageNameW(process, 0, image, &length))
      return failure("read-target-identity", GetLastError());
   const wchar_t *base = wcsrchr(image, L'\\');
   base = base ? base + 1 : image;
   if (filetime(created) != expected || _wcsicmp(base, argv[3]) ||
       WaitForSingleObject(process, 0) != WAIT_TIMEOUT)
      return failure("target-identity-mismatch", ERROR_INVALID_DATA);
   printf("{\"stage\":\"target-verified\",\"pid\":%lu,\"creation_filetime\":%llu}\n",
          pid, (unsigned long long)expected);

   HMODULE dbghelp = LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
   if (!dbghelp) return failure("load-inbox-dbghelp", GetLastError());
   auto write_dump = reinterpret_cast<decltype(&MiniDumpWriteDump)>(
      GetProcAddress(dbghelp, "MiniDumpWriteDump"));
   if (!write_dump) return failure("resolve-writer", GetLastError());
   const std::wstring pending = std::wstring(argv[4]) + L".pending";
   HANDLE file = CreateFileW(pending.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                             CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
   if (file == INVALID_HANDLE_VALUE) return failure("create-output", GetLastError());

   HPSS snapshot = nullptr;
   const ULONGLONG started = GetTickCount64();
   DWORD status = PssCaptureSnapshot(process, static_cast<PSS_CAPTURE_FLAGS>(
      PSS_CAPTURE_VA_CLONE | PSS_CAPTURE_THREADS | PSS_CAPTURE_THREAD_CONTEXT),
      CONTEXT_ALL, &snapshot);
   printf("{\"stage\":\"snapshot\",\"error\":%lu,\"elapsed_ms\":%llu}\n",
          status, (unsigned long long)(GetTickCount64() - started));
   bool success = false;
   DWORD dump_error = status, free_error = ERROR_SUCCESS;
   if (status == ERROR_SUCCESS) {
      Writer writer = {file, started};
      MINIDUMP_CALLBACK_INFORMATION callback = {dump_callback, &writer};
      success = write_dump(reinterpret_cast<HANDLE>(snapshot), pid, file,
         static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo |
                                   MiniDumpWithUnloadedModules),
         nullptr, nullptr, &callback);
      dump_error = success ? ERROR_SUCCESS : GetLastError();
      free_error = PssFreeSnapshot(GetCurrentProcess(), snapshot);
   }
   LARGE_INTEGER bytes = {};
   if (success && (!FlushFileBuffers(file) || !GetFileSizeEx(file, &bytes))) {
      success = false;
      dump_error = GetLastError();
   }
   CloseHandle(file);
   if (success && !MoveFileExW(pending.c_str(), argv[4], MOVEFILE_WRITE_THROUGH)) {
      success = false;
      dump_error = GetLastError();
   }
   CloseHandle(process);
   FreeLibrary(dbghelp);
   PROCESS_MEMORY_COUNTERS counters = {};
   counters.cb = sizeof(counters);
   GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters));
   printf("{\"stage\":\"dump\",\"success\":%s,\"error\":%lu,\"free_snapshot_error\":%lu,"
          "\"bytes\":%llu,\"elapsed_ms\":%llu,\"peak_commit_bytes\":%llu}\n",
          success ? "true" : "false", dump_error, free_error,
          (unsigned long long)bytes.QuadPart, (unsigned long long)(GetTickCount64() - started),
          (unsigned long long)counters.PeakPagefileUsage);
   /* Failed/interrupted output stays .pending and is not a completed dump. */
   return success && free_error == ERROR_SUCCESS ? 0 : 1;
}
