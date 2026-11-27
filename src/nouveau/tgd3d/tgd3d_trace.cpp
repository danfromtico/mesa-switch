/* SPDX-License-Identifier: MIT */
#include "tgd3d_trace.h"
#include <evntprov.h>
#include <cwchar>
#include <cstring>

static_assert(sizeof(tgd3d_trace_data) == 96, "ETW numeric payload layout");
static_assert(sizeof(tgd3d_trace_timing) == 184, "ETW timing payload layout");

namespace tgd3d {

namespace {

struct Provider {
   REGHANDLE handle = 0;
   WCHAR image[32] = {};
   uint64_t frequency = 0, process_start_time = 0;
   Provider()
   {
      WCHAR path[MAX_PATH] = {};
      DWORD length = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
      if (length && length < ARRAYSIZE(path)) {
         const WCHAR *base = std::wcsrchr(path, L'\\');
         std::wcsncpy(image, base ? base + 1 : path, ARRAYSIZE(image) - 1);
      }
      FILETIME creation = {}, exit = {}, kernel = {}, user = {};
      if (GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user))
         process_start_time = (uint64_t(creation.dwHighDateTime) << 32) | creation.dwLowDateTime;
      LARGE_INTEGER f;
      QueryPerformanceFrequency(&f);
      frequency = (uint64_t)f.QuadPart;
      static const GUID guid = TGD3D_TRACE_GUID_INIT;
      EventRegister(&guid, nullptr, nullptr, &handle);
   }
   ~Provider() { EventUnregister(handle); }
};

Provider &provider()
{
   /* Registered on first use, outside DllMain; unregistered before unload. */
   static Provider p;
   return p;
}

bool enabled(Provider &p, USHORT id, EVENT_DESCRIPTOR *descriptor)
{
   *descriptor = {id, 2, 0, 4, 0, 0, 1};
   return EventEnabled(p.handle, descriptor);
}

void write(Provider &p, const EVENT_DESCRIPTOR &descriptor, void *data, ULONG size)
{
   EVENT_DATA_DESCRIPTOR payload;
   EventDataDescCreate(&payload, data, size);
   EventWrite(p.handle, &descriptor, 1, &payload);
}

}

uint64_t qpc_now()
{
   LARGE_INTEGER now;
   QueryPerformanceCounter(&now);
   return (uint64_t)now.QuadPart;
}

uint64_t qpc_nanoseconds(uint64_t ticks)
{
   return (uint64_t)((double)ticks * 1000000000.0 / provider().frequency);
}

void trace(tgd3d_trace_event event, HRESULT result, uint64_t arg0, uint64_t arg1)
{
   auto &p = provider();
   EVENT_DESCRIPTOR descriptor;
   if (!enabled(p, (USHORT)event, &descriptor))
      return;
   tgd3d_trace_data data = {};
   data.result = result;
   data.arg0 = arg0;
   data.arg1 = arg1;
   data.process_start_time = p.process_start_time;
   std::wmemcpy(data.image, p.image, ARRAYSIZE(data.image));
   write(p, descriptor, &data, sizeof(data));
}

void trace_message(const char *message)
{
   auto &p = provider();
   EVENT_DESCRIPTOR descriptor;
   if (!message || !*message || !enabled(p, TGD3D_TRACE_MESSAGE, &descriptor))
      return;
   tgd3d_trace_message data = {};
   if (!MultiByteToWideChar(CP_UTF8, 0, message, -1, data.message, ARRAYSIZE(data.message) - 1))
      return;
   data.process_start_time = p.process_start_time;
   std::wmemcpy(data.image, p.image, ARRAYSIZE(data.image));
   write(p, descriptor, &data, sizeof(data));
}

void trace_error(HRESULT result, const char *file, unsigned line)
{
   auto &p = provider();
   EVENT_DESCRIPTOR descriptor;
   if (!enabled(p, TGD3D_TRACE_ERROR, &descriptor))
      return;
   tgd3d_trace_error data = {};
   data.result = result;
   data.line = line;
   /* The source site without the build host's directory. */
   for (const char *s = file; *s; ++s)
      if (*s == '/' || *s == '\\')
         file = s + 1;
   MultiByteToWideChar(CP_UTF8, 0, file, -1, data.file, ARRAYSIZE(data.file) - 1);
   data.process_start_time = p.process_start_time;
   std::wmemcpy(data.image, p.image, ARRAYSIZE(data.image));
   write(p, descriptor, &data, sizeof(data));
}

bool trace_timing_enabled()
{
   EVENT_DESCRIPTOR descriptor;
   return enabled(provider(), TGD3D_TRACE_TIMING, &descriptor);
}

void trace_timing(tgd3d_timing_kind kind, HRESULT result, uint64_t sequence, uint64_t completed,
                  const uint64_t qpc[4], const uint64_t value[6])
{
   auto &p = provider();
   EVENT_DESCRIPTOR descriptor;
   if (!enabled(p, TGD3D_TRACE_TIMING, &descriptor))
      return;
   tgd3d_trace_timing data = {};
   data.result = result;
   data.kind = kind;
   data.sequence = sequence;
   data.completed = completed;
   std::memcpy(data.qpc, qpc, sizeof(data.qpc));
   std::memcpy(data.value, value, sizeof(data.value));
   data.qpc_frequency = p.frequency;
   data.process_start_time = p.process_start_time;
   std::wmemcpy(data.image, p.image, ARRAYSIZE(data.image));
   write(p, descriptor, &data, sizeof(data));
}

}
