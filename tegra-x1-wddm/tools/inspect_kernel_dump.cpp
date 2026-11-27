/* SPDX-License-Identifier: MIT
 * Read an existing dump with the Windows inbox debugger engine. No live attach.
 */
#define INITGUID
#include <windows.h>
#include <dbgeng.h>
#include <stdio.h>
#include <string.h>

class ConsoleSink final : public IDebugOutputCallbacks {
public:
   HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
   {
      if (!out) return E_POINTER;
      *out = nullptr;
      if (iid != IID_IUnknown && iid != IID_IDebugOutputCallbacks)
         return E_NOINTERFACE;
      *out = static_cast<IDebugOutputCallbacks *>(this);
      AddRef();
      return S_OK;
   }
   /* The sink has process lifetime, including client cleanup. */
   ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
   ULONG STDMETHODCALLTYPE Release() override { return 1; }
   HRESULT STDMETHODCALLTYPE Output(ULONG, PCSTR text) override
   {
      fputs(text, stdout);
      fflush(stdout);
      return S_OK;
   }
};

static ConsoleSink sink;

static DWORD WINAPI analysis_deadline(void *)
{
   Sleep(45000);
   TerminateProcess(GetCurrentProcess(), 124);
   return 0;
}

int main(int argc, char **argv)
{
   const bool user_dump = argc == 5 && !strcmp(argv[3], "--user");
   if (argc != 3 && !user_dump) {
      fprintf(stderr, "usage: inspect-kernel-dump.exe existing.dmp symbol-path [--user image-path]\n");
      return 2;
   }
   /* The target has 4 GiB and recently exhausted commit during collection.
    * Bound this offline reader itself, even if its SSH client disappears. */
   HANDLE job = CreateJobObjectW(nullptr, nullptr);
   JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
   limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_PROCESS_MEMORY;
   limits.ProcessMemoryLimit = 256 * 1024 * 1024;
   if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                       &limits, sizeof(limits)) ||
       !AssignProcessToJobObject(job, GetCurrentProcess())) {
      fprintf(stderr, "Bound dump reader memory: %lu\n", GetLastError());
      return 1;
   }
   HANDLE deadline = CreateThread(nullptr, 0, analysis_deadline, nullptr, 0, nullptr);
   if (!deadline) {
      fprintf(stderr, "Start dump reader deadline: %lu\n", GetLastError());
      return 1;
   }
   CloseHandle(deadline);
   SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
   HMODULE module = LoadLibraryExW(L"dbgeng.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
   if (!module) {
      fprintf(stderr, "Load inbox dbgeng: %lu\n", GetLastError());
      return 1;
   }
   using Create = HRESULT (WINAPI *)(REFIID, void **);
   auto create = reinterpret_cast<Create>(GetProcAddress(module, "DebugCreate"));
   IDebugClient *client = nullptr;
   IDebugControl *control = nullptr;
   IDebugSymbols *symbols = nullptr;
   HRESULT hr = create ? create(IID_IDebugClient, reinterpret_cast<void **>(&client)) : E_NOINTERFACE;
   if (SUCCEEDED(hr)) hr = client->SetOutputCallbacks(&sink);
   if (SUCCEEDED(hr)) hr = client->QueryInterface(IID_IDebugControl, reinterpret_cast<void **>(&control));
   if (SUCCEEDED(hr)) hr = client->QueryInterface(IID_IDebugSymbols, reinterpret_cast<void **>(&symbols));
   if (SUCCEEDED(hr)) hr = symbols->SetSymbolPath(argv[2]);
   /* ARM64 stack unwinding also needs the matching image's unwind tables.
    * DbgEng validates identities; this is an image search, not code loading. */
   if (SUCCEEDED(hr)) hr = symbols->SetImagePath(user_dump ? argv[4] :
      "C:\\Windows\\System32;C:\\Windows\\System32\\drivers");
   if (SUCCEEDED(hr)) hr = client->OpenDumpFile(argv[1]);
   if (SUCCEEDED(hr)) hr = control->WaitForEvent(0, INFINITE);
   int result = FAILED(hr) ? 1 : 0;
   if (FAILED(hr)) fprintf(stderr, "Open dump: 0x%08lx\n", static_cast<ULONG>(hr));
   if (SUCCEEDED(hr)) {
      const char *kernel_commands[] = {"vertarget", ".bugcheck", "lmv m tegragpurender",
                                       "kv", ".enumtag", nullptr};
      const char *user_commands[] = {"vertarget", "lm", "~* kv", nullptr};
      for (const char **cmd = user_dump ? user_commands : kernel_commands; *cmd; ++cmd) {
         const char *command = *cmd;
         printf("\n=== %s ===\n", command);
         fflush(stdout);
         hr = control->Execute(DEBUG_OUTCTL_THIS_CLIENT, command, DEBUG_EXECUTE_ECHO);
         if (FAILED(hr)) {
            fprintf(stderr, "Command '%s': 0x%08lx\n", command, static_cast<ULONG>(hr));
            result = 1;
         }
      }
   }
   if (symbols) symbols->Release();
   if (control) control->Release();
   if (client) {
      client->EndSession(DEBUG_END_PASSIVE);
      client->SetOutputCallbacks(nullptr);
      client->Release();
   }
   FreeLibrary(module);
   return result;
}
