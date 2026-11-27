/* SPDX-License-Identifier: MIT */
/* Decode the versioned UMD provider without installing an event manifest. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>
#include <cstdio>
#include <cstring>
#include "Trace.h"

static const GUID provider = D3D10_UMD_TRACE_GUID_INIT;
static unsigned matched, invalid;

static const char *event_name(USHORT id)
{
   static const char *names[] = {"invalid", "OpenAdapter10", "OpenAdapter10_2",
      "ScreenQuery", "ScreenIdentity", "ZinkBegin", "ZinkEnd", "OpenResult",
      "SupportedVersion", "GetCaps", "CreateDeviceBegin", "CreateDeviceResult",
      "OpenResource", "Present", "Blt", "SetDisplayMode", "ZinkFailure", "ZinkMessage",
      "ZinkInstanceCall", "ZinkInstanceInfo", "RuntimeError", "ResourceCreateBegin",
      "ResourceCreateSize", "ResourceCreateEnd", "FormatBegin", "FormatEnd",
      "Multisample", "DestroyDeviceBegin", "DestroyDeviceEnd", "RuntimeMemory", "RuntimeContext",
      "RotateIdentities", "PrimaryLayout"};
   return id < ARRAYSIZE(names) ? names[id] : "unknown";
}

static void json_image(const WCHAR *image)
{
   char utf8[4096] = {};
   WideCharToMultiByte(CP_UTF8, 0, image, -1, utf8, sizeof(utf8), nullptr, nullptr);
   std::putchar('"');
   for (const unsigned char *s = (unsigned char *)utf8; *s; ++s) {
      if (*s == '"' || *s == '\\') std::putchar('\\');
      if (*s < 32) std::printf("\\u%04x", *s);
      else std::putchar(*s);
   }
   std::putchar('"');
}

static void record_header(const EVENT_HEADER &header, const WCHAR *image)
{
   std::printf("{\"id\":%u,\"event\":\"%s\",\"pid\":%lu,\"tid\":%lu,\"time\":%llu,\"image\":",
      header.EventDescriptor.Id, event_name(header.EventDescriptor.Id), header.ProcessId,
      header.ThreadId, (unsigned long long)header.TimeStamp.QuadPart);
   json_image(image);
}

static void WINAPI record(EVENT_RECORD *event)
{
   if (std::memcmp(&event->EventHeader.ProviderId, &provider, sizeof(provider)))
      return;
   const auto &header = event->EventHeader;
   if (header.EventDescriptor.Version != 1) { ++invalid; return; }
   if (header.EventDescriptor.Id == D3D10_TRACE_RUNTIME_ERROR) {
      if (event->UserDataLength != sizeof(d3d10_umd_trace_error)) { ++invalid; return; }
      d3d10_umd_trace_error data;
      std::memcpy(&data, event->UserData, sizeof(data));
      if (data.image[31] || data.file[63] || data.function[63]) { ++invalid; return; }
      ++matched;
      record_header(header, data.image);
      std::printf(",\"status\":\"%08x\",\"line\":%u,\"file\":", (unsigned)data.result, data.line);
      json_image(data.file);
      std::printf(",\"function\":");
      json_image(data.function);
      std::puts("}");
      return;
   }
   if (header.EventDescriptor.Id == D3D10_TRACE_ZINK_MESSAGE) {
      if (event->UserDataLength != sizeof(d3d10_umd_trace_message)) { ++invalid; return; }
      d3d10_umd_trace_message message;
      std::memcpy(&message, event->UserData, sizeof(message));
      if (message.image[31] || message.message[1023]) { ++invalid; return; }
      ++matched;
      record_header(header, message.image);
      std::printf(",\"message\":");
      json_image(message.message);
      std::puts("}");
      return;
   }
   if (event->UserDataLength != sizeof(d3d10_umd_trace_data)) {
      ++invalid;
      return;
   }
   d3d10_umd_trace_data data;
   std::memcpy(&data, event->UserData, sizeof(data));
   if (data.reserved || data.image[31]) { ++invalid; return; }
   ++matched;
   record_header(header, data.image);
   std::printf(",\"status\":\"%08x\",\"arg0\":\"%016llx\",\"arg1\":\"%016llx\"",
      (unsigned)data.result, (unsigned long long)data.arg0, (unsigned long long)data.arg1);
   if (header.EventDescriptor.Id == D3D10_TRACE_ZINK_FAILURE)
      std::printf(",\"stage\":%u,\"line\":%u,\"detail\":%u,\"code\":%d",
                  (unsigned)(data.arg0 >> 32), (unsigned)data.arg0,
                  (unsigned)(data.arg1 >> 32), (int32_t)data.arg1);
   if (header.EventDescriptor.Id == D3D10_TRACE_ZINK_INSTANCE_CALL)
      std::printf(",\"call\":%u,\"code\":%d,\"value\":%u",
                  (unsigned)data.arg0, data.result, (unsigned)data.arg1);
   if (header.EventDescriptor.Id == D3D10_TRACE_ZINK_INSTANCE_INFO)
      std::printf(",\"called_mask\":%u,\"flags\":%u,\"extensions\":%u,\"layers\":%u",
                  (unsigned)(data.arg0 >> 32), (unsigned)data.arg0,
                  (unsigned)(data.arg1 >> 32), (unsigned)data.arg1);
   std::puts("}");
}

int wmain(int argc, WCHAR **argv)
{
   if (argc != 2) { std::fprintf(stderr, "usage: d3d10-trace-export.exe trace.etl\n"); return 2; }
   EVENT_TRACE_LOGFILEW log = {};
   log.LogFileName = argv[1];
   log.ProcessTraceMode = PROCESS_TRACE_MODE_EVENT_RECORD;
   log.EventRecordCallback = record;
   TRACEHANDLE handle = OpenTraceW(&log);
   if (handle == INVALID_PROCESSTRACE_HANDLE) {
      std::fprintf(stderr, "OpenTrace failed: %lu\n", GetLastError()); return 1;
   }
   const ULONG status = ProcessTrace(&handle, 1, nullptr, nullptr);
   CloseTrace(handle);
   std::fprintf(stderr, "ProcessTrace=%lu matched=%u invalid=%u events_lost=%lu buffers_lost=%lu end_time=%llu buffers_written=%lu\n",
                status, matched, invalid, log.LogfileHeader.EventsLost, log.LogfileHeader.BuffersLost,
                (unsigned long long)log.LogfileHeader.EndTime.QuadPart, log.LogfileHeader.BuffersWritten);
   return status || invalid || !matched ? 1 : 0;
}
