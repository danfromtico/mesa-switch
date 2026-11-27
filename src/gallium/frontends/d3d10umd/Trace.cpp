/* SPDX-License-Identifier: MIT */
#include "Trace.h"
#include <evntprov.h>
#include <cwchar>
#include <cstring>

static_assert(sizeof(d3d10_umd_trace_data) == 88, "ETW payload layout");
static_assert(sizeof(d3d10_umd_trace_message) == 2112, "ETW message layout");
static_assert(sizeof(d3d10_umd_trace_error) == 328, "ETW runtime error layout");

class TraceProvider {
public:
   REGHANDLE handle = 0;
   WCHAR image[32] = {};

   TraceProvider()
   {
      WCHAR path[MAX_PATH] = {};
      DWORD length = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
      if (length && length < ARRAYSIZE(path)) {
         const WCHAR *base = std::wcsrchr(path, L'\\');
         std::wcsncpy(image, base ? base + 1 : path, ARRAYSIZE(image) - 1);
      }
      static const GUID guid = D3D10_UMD_TRACE_GUID_INIT;
      EventRegister(&guid, nullptr, nullptr, &handle);
   }

   ~TraceProvider() { EventUnregister(handle); }
};

static TraceProvider &trace_provider()
{
   /* Function-static registration occurs outside DllMain on first use.
    * Its destructor unregisters before the DLL unloads. */
   static TraceProvider provider;
   return provider;
}

void D3D10Trace(enum d3d10_umd_trace_event event, HRESULT result,
                uint64_t arg0, uint64_t arg1)
{
   auto &provider = trace_provider();
   const EVENT_DESCRIPTOR descriptor = {(USHORT)event, 1, 0, 4, 0, 0, 1};
   if (!EventEnabled(provider.handle, &descriptor))
      return;
   d3d10_umd_trace_data data = {};
   data.result = result;
   data.arg0 = arg0;
   data.arg1 = arg1;
   std::wmemcpy(data.image, provider.image, ARRAYSIZE(data.image));
   EVENT_DATA_DESCRIPTOR payload;
   EventDataDescCreate(&payload, &data, sizeof(data));
   EventWrite(provider.handle, &descriptor, 1, &payload);
}

void D3D10TraceMessage(const char *message)
{
   auto &provider = trace_provider();
   const EVENT_DESCRIPTOR descriptor = {D3D10_TRACE_ZINK_MESSAGE, 1, 0, 4, 0, 0, 1};
   if (!message || !*message || !EventEnabled(provider.handle, &descriptor))
      return;
   d3d10_umd_trace_message data = {};
   if (!MultiByteToWideChar(CP_UTF8, 0, message, -1, data.message, ARRAYSIZE(data.message)))
      return;
   std::wmemcpy(data.image, provider.image, ARRAYSIZE(data.image));
   EVENT_DATA_DESCRIPTOR payload;
   EventDataDescCreate(&payload, &data, sizeof(data));
   EventWrite(provider.handle, &descriptor, 1, &payload);
}

void D3D10TraceError(HRESULT result, const char *file, const char *function,
                     unsigned line)
{
   auto &provider = trace_provider();
   const EVENT_DESCRIPTOR descriptor = {D3D10_TRACE_RUNTIME_ERROR, 1, 0, 4, 0, 0, 1};
   if (!EventEnabled(provider.handle, &descriptor))
      return;
   d3d10_umd_trace_error data = {};
   data.result = result;
   data.line = line;
   /* Keep the source site, without embedding the build host's directory. */
   for (const char *s = file; *s; ++s)
      if (*s == '/' || *s == '\\') file = s + 1;
   if (!MultiByteToWideChar(CP_UTF8, 0, file, -1, data.file, ARRAYSIZE(data.file)) ||
       !MultiByteToWideChar(CP_UTF8, 0, function, -1, data.function, ARRAYSIZE(data.function)))
      return;
   std::wmemcpy(data.image, provider.image, ARRAYSIZE(data.image));
   EVENT_DATA_DESCRIPTOR payload;
   EventDataDescCreate(&payload, &data, sizeof(data));
   EventWrite(provider.handle, &descriptor, 1, &payload);
}
