/* SPDX-License-Identifier: MIT
 * Manifest-free decoder for tgd3d's versioned numeric and timing events.
 */
#include "tgd3d_trace.h"
#include <evntrace.h>
#include <evntcons.h>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cstddef>

static const GUID provider = TGD3D_TRACE_GUID_INIT;
static const GUID kernel_provider =
   {0xc27c1f9b, 0x62e8, 0x4e21, {0xb2, 0x9b, 0x6b, 0xb2, 0xd5, 0x85, 0xc6, 0xd0}};
static const GUID dxg_provider =
   {0x802ec45a, 0x1e99, 0x4b83, {0x99, 0x20, 0x87, 0xc9, 0x82, 0x77, 0xba, 0x9d}};
static unsigned matched, invalid;
static unsigned raw_matched;
static bool follow_all;
static const WCHAR *live_session;
static ULONGLONG last_state;
static HANDLE flush_stop;
static volatile LONG flush_error;

/* This flushes tracing buffers only. It neither submits GPU commands nor
 * acknowledges a rendering/display fence. File-only ETL capture previously
 * lost the interval containing a direction-change reset. */
static ULONG flush_session()
{
   struct {
      EVENT_TRACE_PROPERTIES properties;
      WCHAR session[1024], filename[1024];
   } buffer = {};
   auto props = &buffer.properties;
   props->Wnode.BufferSize = sizeof(buffer);
   props->LoggerNameOffset = sizeof(*props);
   props->LogFileNameOffset = sizeof(*props) + sizeof(buffer.session);
   ULONG status = ControlTraceW(0, live_session, props, EVENT_TRACE_CONTROL_QUERY);
   if (!status)
      status = ControlTraceW(0, live_session, props, EVENT_TRACE_CONTROL_FLUSH);
   return status;
}

static DWORD WINAPI flush_thread(void *)
{
   while (WaitForSingleObject(flush_stop, 20) == WAIT_TIMEOUT) {
      const ULONG status = flush_session();
      if (status == ERROR_WMI_INSTANCE_NOT_FOUND)
         break; /* The controller stopped our session. */
      if (status) {
         InterlockedExchange(&flush_error, status);
         break;
      }
   }
   return 0;
}

static void json_string(const WCHAR *value)
{
   char utf8[2048] = {};
   WideCharToMultiByte(CP_UTF8, 0, value, -1, utf8, sizeof(utf8), nullptr, nullptr);
   std::putchar('"');
   for (const unsigned char *p = (const unsigned char *)utf8; *p; p++) {
      if (*p == '"' || *p == '\\') std::putchar('\\');
      if (*p < 32) std::printf("\\u%04x", *p);
      else std::putchar(*p);
   }
   std::putchar('"');
}

template<typename T> static bool payload(EVENT_RECORD *event, T *out)
{
   const size_t size = event->EventHeader.EventDescriptor.Version == 1 ?
      offsetof(T, process_start_time) : sizeof(T);
   if (event->UserDataLength != size) return false;
   *out = {};
   std::memcpy(out, event->UserData, size);
   return !out->image[31];
}

static void header(const EVENT_HEADER &h, const WCHAR *image, uint64_t process_start)
{
   ++matched;
   std::printf("{\"id\":%u,\"version\":%u,\"pid\":%lu,\"tid\":%lu,\"%s\":%llu,\"process_start\":%llu,\"image\":",
      h.EventDescriptor.Id, h.EventDescriptor.Version, h.ProcessId, h.ThreadId,
      (live_session || follow_all) ? "qpc_stamp" : "time", (unsigned long long)h.TimeStamp.QuadPart,
      (unsigned long long)process_start);
   json_string(image);
}

static void WINAPI record(EVENT_RECORD *event)
{
   const auto &h = event->EventHeader;
   if (std::memcmp(&h.ProviderId, &provider, sizeof(provider))) {
      if (!follow_all)
         return;
      const char *name = nullptr;
      if (!std::memcmp(&h.ProviderId, &kernel_provider, sizeof(kernel_provider)))
         name = "c27c1f9b-62e8-4e21-b29b-6bb2d585c6d0";
      else if (!std::memcmp(&h.ProviderId, &dxg_provider, sizeof(dxg_provider)))
         name = "802ec45a-1e99-4b83-9920-87c98277ba9d";
      if (!name)
         return;
      if (event->UserDataLength && !event->UserData) { ++invalid; return; }
      ++raw_matched;
      /* Preserve the provider's actual bytes. Decode using the matching
       * driver schema or this machine's manifest, never guessed layouts. */
      std::printf("{\"provider\":\"%s\",\"raw\":true,\"id\":%u,\"version\":%u,"
         "\"opcode\":%u,\"task\":%u,\"header_flags\":%u,\"pid\":%lu,\"tid\":%lu,"
         "\"qpc_stamp\":%llu,\"bytes\":%u,\"data\":\"",
         name, h.EventDescriptor.Id, h.EventDescriptor.Version,
         h.EventDescriptor.Opcode, h.EventDescriptor.Task, h.Flags,
         h.ProcessId, h.ThreadId, (unsigned long long)h.TimeStamp.QuadPart,
         event->UserDataLength);
      const auto bytes = static_cast<const unsigned char *>(event->UserData);
      static const char digits[] = "0123456789abcdef";
      char hex[1024];
      for (unsigned offset = 0; offset < event->UserDataLength;) {
         const unsigned left = event->UserDataLength - offset;
         const unsigned count = left < sizeof(hex) / 2 ? left : sizeof(hex) / 2;
         for (unsigned i = 0; i < count; ++i) {
            hex[i * 2] = digits[bytes[offset + i] >> 4];
            hex[i * 2 + 1] = digits[bytes[offset + i] & 15];
         }
         std::fwrite(hex, 2, count, stdout);
         offset += count;
      }
      std::puts("\"}");
      std::fflush(stdout);
      return;
   }
   if (h.EventDescriptor.Version != 1 && h.EventDescriptor.Version != 2) { ++invalid; return; }
   if (h.EventDescriptor.Id == TGD3D_TRACE_ERROR) {
      tgd3d_trace_error p;
      if (!payload(event, &p) || p.file[63]) { ++invalid; return; }
      header(h, p.image, p.process_start_time);
      std::printf(",\"status\":\"%08x\",\"line\":%u,\"file\":", (unsigned)p.result, p.line);
      json_string(p.file);
   } else if (h.EventDescriptor.Id == TGD3D_TRACE_MESSAGE) {
      tgd3d_trace_message p;
      if (!payload(event, &p) || p.message[255]) { ++invalid; return; }
      header(h, p.image, p.process_start_time); std::printf(",\"message\":"); json_string(p.message);
   } else if (h.EventDescriptor.Id == TGD3D_TRACE_TIMING) {
      tgd3d_trace_timing p;
      if (!payload(event, &p)) { ++invalid; return; }
      header(h, p.image, p.process_start_time);
      std::printf(",\"status\":\"%08x\",\"kind\":%u,\"sequence\":%llu,\"completed\":%llu,\"qpc_frequency\":%llu,\"qpc\":[",
         (unsigned)p.result, p.kind, (unsigned long long)p.sequence,
         (unsigned long long)p.completed, (unsigned long long)p.qpc_frequency);
      for (unsigned i = 0; i < 4; i++) std::printf("%s%llu", i ? "," : "", (unsigned long long)p.qpc[i]);
      std::printf("],\"value\":[");
      for (unsigned i = 0; i < 6; i++) std::printf("%s%llu", i ? "," : "", (unsigned long long)p.value[i]);
      std::putchar(']');
      if (p.kind == TGD3D_TIMING_RESIDENCY_SCAN && p.qpc_frequency && p.qpc[1] >= p.qpc[0]) {
         const uint64_t elapsed = p.qpc[1] - p.qpc[0];
         const double usPerTick = 1e6 / (double)p.qpc_frequency;
         std::printf(",\"scanned\":%llu,\"used\":%llu,\"residency_us\":%.3f,\"device\":\"%llx\",\"vk_result\":%d",
                     (unsigned long long)p.value[0], (unsigned long long)p.value[1], elapsed * usPerTick,
                     (unsigned long long)p.value[2], (int32_t)p.value[3]);
         if (p.value[4] <= elapsed)
            std::printf(",\"make_resident_us\":%.3f,\"walk_us\":%.3f",
                        p.value[4] * usPerTick, (elapsed - p.value[4]) * usPerTick);
      }
   } else {
      tgd3d_trace_data p;
      if (!payload(event, &p)) { ++invalid; return; }
      header(h, p.image, p.process_start_time);
      std::printf(",\"status\":\"%08x\",\"arg0\":%llu,\"arg1\":%llu",
         (unsigned)p.result, (unsigned long long)p.arg0, (unsigned long long)p.arg1);
   }
   std::puts("}");
   if (follow_all)
      std::fflush(stdout);
}

/* Live-buffer/query handling reused from the existing D3D trace exporter.
 * See tegra-x1-wddm/tools/tgd3d-trial/tool-provenance.json for the exact source snapshot. */
static ULONG live_state(const char *event, ULONG process_status = ERROR_SUCCESS)
{
   struct {
      EVENT_TRACE_PROPERTIES properties;
      WCHAR session[1024], filename[1024];
   } buffer = {};
   auto props = &buffer.properties;
   props->Wnode.BufferSize = sizeof(buffer);
   props->LoggerNameOffset = sizeof(*props);
   props->LogFileNameOffset = sizeof(*props) + sizeof(buffer.session);
   ULONG status = ControlTraceW(0, live_session, props, EVENT_TRACE_CONTROL_QUERY);
   if (!status && (!(props->LogFileMode & EVENT_TRACE_REAL_TIME_MODE) ||
                   props->Wnode.ClientContext != 1))
      status = ERROR_INVALID_PARAMETER;
   FILETIME now;
   GetSystemTimeAsFileTime(&now);
   LARGE_INTEGER counter, frequency;
   QueryPerformanceCounter(&counter);
   QueryPerformanceFrequency(&frequency);
   const uint64_t time = (uint64_t(now.dwHighDateTime) << 32) | now.dwLowDateTime;
   std::printf("{\"event\":\"%s\",\"time\":%llu,\"pid\":%lu,\"session\":",
      event, (unsigned long long)time, GetCurrentProcessId());
   json_string(live_session);
   std::printf(",\"clock\":\"qpc\",\"clock_context\":%lu,\"qpc_stamp\":%llu,\"qpc_frequency\":%llu",
      props->Wnode.ClientContext, (unsigned long long)counter.QuadPart,
      (unsigned long long)frequency.QuadPart);
   std::printf(",\"query_status\":%lu,\"process_status\":%lu,\"matched\":%u,\"invalid\":%u,"
      "\"raw_matched\":%u,\"flush_interval_ms\":%u,\"flush_status\":%ld,"
      "\"events_lost\":%lu,\"log_buffers_lost\":%lu,\"realtime_buffers_lost\":%lu}\n",
      status, process_status, matched, invalid, raw_matched, follow_all ? 20u : 0u,
      InterlockedCompareExchange(&flush_error, 0, 0), props->EventsLost,
      props->LogBuffersLost, props->RealTimeBuffersLost);
   last_state = GetTickCount64();
   std::fflush(stdout);
   return status;
}

static ULONG WINAPI live_buffer(EVENT_TRACE_LOGFILEW *)
{
   /* Flush complete records at buffer boundaries, not per printf/event. */
   if (GetTickCount64() - last_state >= 1000) {
      if (live_state("TraceState") != ERROR_SUCCESS)
         return FALSE;
   }
   return std::fflush(stdout) == 0;
}

int wmain(int argc, WCHAR **argv)
{
   const bool export_all = argc == 3 && !std::wcscmp(argv[1], L"--export-all");
   const bool live_all = argc == 3 && !std::wcscmp(argv[1], L"--follow-all");
   follow_all = live_all || export_all;
   const bool follow = live_all || (argc == 3 && !std::wcscmp(argv[1], L"--follow"));
   if (argc != 2 && !follow && !export_all) {
      std::fprintf(stderr, "usage: tdr-trace-export.exe trace.etl | --export-all trace.etl | --follow session | --follow-all session\n");
      return 2;
   }
   EVENT_TRACE_LOGFILEW log = {};
   log.ProcessTraceMode = PROCESS_TRACE_MODE_EVENT_RECORD;
   if (export_all)
      log.ProcessTraceMode |= PROCESS_TRACE_MODE_RAW_TIMESTAMP;
   if (follow) {
      live_session = log.LoggerName = argv[2];
      log.ProcessTraceMode |= PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_RAW_TIMESTAMP;
      log.BufferCallback = live_buffer;
   } else {
      log.LogFileName = export_all ? argv[2] : argv[1];
   }
   log.EventRecordCallback = record;
   TRACEHANDLE handle = OpenTraceW(&log);
   if (handle == INVALID_PROCESSTRACE_HANDLE) {
      std::fprintf(stderr, "OpenTrace failed: %lu\n", GetLastError()); return 1;
   }
   if (export_all && (log.LogfileHeader.ReservedFlags != 1 ||
                      log.LogfileHeader.PerfFreq.QuadPart <= 0)) {
      std::fprintf(stderr, "Unsupported ETL clock_context=%lu perf_frequency=%lld (expected QPC)\n",
                   log.LogfileHeader.ReservedFlags, (long long)log.LogfileHeader.PerfFreq.QuadPart);
      CloseTrace(handle);
      return 1;
   }
   HANDLE flusher = nullptr;
   if (live_all) {
      const ULONG status = flush_session();
      flush_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
      if (status || !flush_stop || !(flusher = CreateThread(nullptr, 0, flush_thread, nullptr, 0, nullptr))) {
         std::fprintf(stderr, "Live flush setup failed: %lu\n", status ? status : GetLastError());
         if (flush_stop) CloseHandle(flush_stop);
         CloseTrace(handle);
         return 1;
      }
   }
   if (follow && live_state("TraceStart") != ERROR_SUCCESS) {
      if (flusher) {
         SetEvent(flush_stop);
         if (WaitForSingleObject(flusher, 5000) == WAIT_OBJECT_0)
            CloseHandle(flush_stop);
         CloseHandle(flusher);
      }
      CloseTrace(handle);
      return 1;
   }
   const ULONG status = ProcessTrace(&handle, 1, nullptr, nullptr);
   CloseTrace(handle);
   if (flusher) {
      SetEvent(flush_stop);
      if (WaitForSingleObject(flusher, 5000) == WAIT_OBJECT_0)
         CloseHandle(flush_stop);
      else
         InterlockedExchange(&flush_error, ERROR_TIMEOUT);
      CloseHandle(flusher);
   }
   if (follow)
      live_state("TraceEnd", status);
   std::fprintf(stderr, "ProcessTrace=%lu matched=%u raw_matched=%u invalid=%u events_lost=%lu buffers_lost=%lu end_time=%llu buffers_written=%lu clock_context=%lu perf_frequency=%lld\n",
                status, matched, raw_matched, invalid, log.LogfileHeader.EventsLost, log.LogfileHeader.BuffersLost,
                (unsigned long long)log.LogfileHeader.EndTime.QuadPart, log.LogfileHeader.BuffersWritten,
                log.LogfileHeader.ReservedFlags, (long long)log.LogfileHeader.PerfFreq.QuadPart);
   return status || invalid || !(matched || raw_matched) ||
      InterlockedCompareExchange(&flush_error, 0, 0) || std::ferror(stdout) ? 1 : 0;
}
