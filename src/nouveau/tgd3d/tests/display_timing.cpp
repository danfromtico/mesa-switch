/* SPDX-License-Identifier: MIT
 * Explicit native measurement, not an automatic test. No window, draw,
 * Present or mode change: wait on the selected output's real VBlank.
 * Exit 0 means samples were collected, not that 60 Hz or pixels passed.
 */
#include <initguid.h>
#include <windows.h>
#include <dxgi1_2.h>
#include <dwmapi.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using Microsoft::WRL::ComPtr;
struct Request { uint64_t luid; unsigned samples; };
struct Sample { LARGE_INTEGER begin, end; };

static DWORD failure(const char *call, HRESULT hr)
{
   std::printf("{\"type\":\"error\",\"call\":\"%s\",\"status\":\"%08x\"}\n", call, (unsigned)hr);
   return 1;
}

static uint64_t filetime(FILETIME t)
{
   return ((uint64_t)t.dwHighDateTime << 32) | t.dwLowDateTime;
}

static void composition(const char *phase, HRESULT hr, const DWM_TIMING_INFO &t)
{
   std::printf("{\"type\":\"dwm\",\"phase\":\"%s\",\"status\":\"%08x\"", phase, (unsigned)hr);
   if (hr == S_OK)
      std::printf(",\"reported_refresh_num\":%u,\"reported_refresh_den\":%u,"
         "\"qpc_refresh_period\":%llu,\"qpc_vblank\":%llu,\"refresh_count\":%llu,"
         "\"qpc_compose\":%llu,\"composed_frame\":%llu,\"submitted_frame\":%llu,"
         "\"confirmed_frame\":%llu,\"outstanding_frames\":%u,\"displayed_frame\":%llu",
         t.rateRefresh.uiNumerator, t.rateRefresh.uiDenominator,
         t.qpcRefreshPeriod, t.qpcVBlank, t.cRefresh, t.qpcCompose, t.cFrame,
         t.cFrameSubmitted, t.cFrameConfirmed, t.cFramesOutstanding, t.cFrameDisplayed);
   /* The interval counters are valid only after a second timing query. */
   if (hr == S_OK && !strcmp(phase, "after"))
      std::printf(",\"frames_displayed\":%llu,\"frames_dropped\":%llu,\"frames_missed\":%llu",
         t.cFramesDisplayed, t.cFramesDropped, t.cFramesMissed);
   std::puts("}");
}

static DWORD WINAPI measure(void *context)
{
   const auto &request = *static_cast<const Request *>(context);
   ComPtr<IDXGIFactory1> factory;
   HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)factory.GetAddressOf());
   if (FAILED(hr)) return failure("CreateDXGIFactory1", hr);
   ComPtr<IDXGIAdapter1> adapter;
   for (UINT i = 0; ; ++i) {
      ComPtr<IDXGIAdapter1> candidate;
      hr = factory->EnumAdapters1(i, &candidate);
      if (hr == DXGI_ERROR_NOT_FOUND) break;
      if (FAILED(hr)) return failure("EnumAdapters1", hr);
      DXGI_ADAPTER_DESC1 desc = {};
      hr = candidate->GetDesc1(&desc);
      if (FAILED(hr)) return failure("GetDesc1", hr);
      const uint64_t luid = ((uint64_t)(uint32_t)desc.AdapterLuid.HighPart << 32) | desc.AdapterLuid.LowPart;
      if (luid == request.luid && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
         adapter = candidate;
         break;
      }
   }
   if (!adapter) return failure("requested native adapter not found", DXGI_ERROR_NOT_FOUND);
   ComPtr<IDXGIOutput> output;
   DXGI_OUTPUT_DESC selected = {};
   for (UINT i = 0; ; ++i) {
      ComPtr<IDXGIOutput> candidate;
      hr = adapter->EnumOutputs(i, &candidate);
      if (hr == DXGI_ERROR_NOT_FOUND) break;
      if (FAILED(hr)) return failure("EnumOutputs", hr);
      DXGI_OUTPUT_DESC desc = {};
      hr = candidate->GetDesc(&desc);
      if (FAILED(hr)) return failure("Get output description", hr);
      if (desc.AttachedToDesktop) {
         if (output) return failure("multiple attached outputs; measurement is ambiguous", E_INVALIDARG);
         output = candidate; selected = desc;
      }
   }
   if (!output) return failure("no attached output", DXGI_ERROR_NOT_FOUND);
   LARGE_INTEGER frequency;
   if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0)
      return failure("QueryPerformanceFrequency", E_FAIL);
   DWORD session = UINT32_MAX;
   ProcessIdToSessionId(GetCurrentProcessId(), &session);
   std::printf("{\"type\":\"identity\",\"luid\":\"%016llx\",\"samples\":%u,"
      "\"qpc_frequency\":%lld,\"pid\":%lu,\"uptime_ms\":%llu,"
      "\"session\":%lu,\"console_session\":%lu,\"desktop\":[%ld,%ld,%ld,%ld],\"rotation\":%u}\n",
      request.luid, request.samples, frequency.QuadPart, GetCurrentProcessId(), GetTickCount64(),
      session, WTSGetActiveConsoleSessionId(), selected.DesktopCoordinates.left,
      selected.DesktopCoordinates.top, selected.DesktopCoordinates.right, selected.DesktopCoordinates.bottom,
      (unsigned)selected.Rotation);
   fflush(stdout);

   DWM_TIMING_INFO before = {}, after = {};
   before.cbSize = after.cbSize = sizeof(before);
   const HRESULT dwmBefore = DwmGetCompositionTimingInfo(nullptr, &before);
   FILETIME created, exited, kernelBefore, userBefore, kernelAfter, userAfter;
   bool cpuTimes = GetThreadTimes(GetCurrentThread(), &created, &exited, &kernelBefore, &userBefore);
   Sample samples[601];
   for (unsigned i = 0; i <= request.samples; ++i) {
      QueryPerformanceCounter(&samples[i].begin);
      hr = output->WaitForVBlank();
      QueryPerformanceCounter(&samples[i].end);
      if (FAILED(hr)) return failure("WaitForVBlank", hr);
   }
   cpuTimes = GetThreadTimes(GetCurrentThread(), &created, &exited, &kernelAfter, &userAfter) && cpuTimes;
   const HRESULT dwmAfter = DwmGetCompositionTimingInfo(nullptr, &after);
   composition("before", dwmBefore, before);
   composition("after", dwmAfter, after);
   double periods[600], waitMs = 0;
   for (unsigned i = 0; i <= request.samples; ++i) {
      std::printf("{\"type\":\"vblank_wait\",\"index\":%u,\"begin_qpc\":%lld,\"end_qpc\":%lld}\n",
         i, samples[i].begin.QuadPart, samples[i].end.QuadPart);
      if (!i) continue; /* The initial partial refresh is not a full period. */
      periods[i - 1] = (samples[i].end.QuadPart - samples[i - 1].end.QuadPart) * 1000.0 / frequency.QuadPart;
      waitMs += (samples[i].end.QuadPart - samples[i].begin.QuadPart) * 1000.0 / frequency.QuadPart;
   }
   std::sort(periods, periods + request.samples);
   const double seconds = (samples[request.samples].end.QuadPart - samples[0].end.QuadPart) /
                          (double)frequency.QuadPart;
   if (seconds <= 0) return failure("nonadvancing sample clock", E_FAIL);
   std::printf("{\"type\":\"summary\",\"observed_wake_hz\":%.6f,\"median_ms\":%.6f,"
      "\"p95_ms\":%.6f,\"max_ms\":%.6f,\"wait_call_ms\":%.6f,\"dwm_available\":%s",
      request.samples / seconds, periods[request.samples / 2],
      periods[(request.samples * 95 - 1) / 100], periods[request.samples - 1], waitMs,
      dwmBefore == S_OK && dwmAfter == S_OK ? "true" : "false");
   if (cpuTimes)
      std::printf(",\"probe_thread_cpu_ms\":%.6f", (filetime(kernelAfter) - filetime(kernelBefore) +
         filetime(userAfter) - filetime(userBefore)) / 10000.0);
   std::puts(",\"physical_display_accepted\":false,\"gpu_work_measured\":false}");
   return 0;
}

int main(int argc, char **argv)
{
   Request request = {};
   char *end = nullptr;
   if (argc == 4 && !strcmp(argv[1], "--luid") && strlen(argv[2]) == 16 &&
       strspn(argv[2], "0123456789abcdefABCDEF") == 16) {
      request.luid = strtoull(argv[2], nullptr, 16);
      const unsigned long count = strtoul(argv[3], &end, 10);
      if (end != argv[3] && !*end && count >= 30 && count <= 600) request.samples = count;
   }
   if (!request.luid || !request.samples) {
      std::fprintf(stderr, "usage: %s --luid 16-hex-digits samples(30..600)\n", argv[0]);
      return 2;
   }
   SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
   HANDLE thread = CreateThread(nullptr, 0, measure, &request, 0, nullptr);
   if (!thread) return failure("CreateThread", HRESULT_FROM_WIN32(GetLastError()));
   const DWORD timeout = 5000 + request.samples * 50;
   const DWORD wait = WaitForSingleObject(thread, timeout);
   if (wait != WAIT_OBJECT_0) {
      const DWORD error = wait == WAIT_FAILED ? GetLastError() : ERROR_TIMEOUT;
      std::fprintf(stderr, "display timing probe incomplete: wait=%lu error=%lu limit=%lu ms; no 60 Hz acceptance\n",
         wait, error, timeout);
      fflush(nullptr);
      /* Stop only this read-only probe if the display API never returns.
       * It owns no submitted GPU work and never restarts a driver/device. */
      TerminateProcess(GetCurrentProcess(), error);
      return (int)error;
   }
   DWORD status = 1;
   GetExitCodeThread(thread, &status);
   CloseHandle(thread);
   return (int)status;
}
