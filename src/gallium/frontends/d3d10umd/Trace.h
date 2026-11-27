/* SPDX-License-Identifier: MIT */
#ifndef D3D10_UMD_TRACE_H
#define D3D10_UMD_TRACE_H

#include <windows.h>
#include <stdint.h>

/* Opt-in ETW provider, enabled by the trace controller rather than a
 * process environment variable. Version 1 payloads use this fixed layout.
 * PID/TID/time are supplied by ETW. Numeric events do not log pointers;
 * the separate bounded Vulkan initialization message may include DLL paths.
 */
#define D3D10_UMD_TRACE_GUID_INIT \
   {0x78edcf0e, 0xb51c, 0x4b24, {0x9b, 0x28, 0xc8, 0x6d, 0x48, 0xf5, 0x21, 0xa9}}

enum d3d10_umd_trace_event {
   D3D10_TRACE_OPEN10 = 1,
   D3D10_TRACE_OPEN10_2,
   D3D10_TRACE_SCREEN_QUERY,
   D3D10_TRACE_SCREEN_IDENTITY,
   D3D10_TRACE_ZINK_BEGIN,
   D3D10_TRACE_ZINK_END,
   D3D10_TRACE_OPEN_RESULT,
   D3D10_TRACE_SUPPORTED_VERSION,
   D3D10_TRACE_CAPS,
   D3D10_TRACE_CREATE_DEVICE_BEGIN,
   D3D10_TRACE_CREATE_DEVICE_RESULT,
   D3D10_TRACE_OPEN_RESOURCE,
   D3D10_TRACE_PRESENT,
   D3D10_TRACE_BLT,
   D3D10_TRACE_SET_DISPLAY_MODE,
   D3D10_TRACE_ZINK_FAILURE,
   D3D10_TRACE_ZINK_MESSAGE,
   D3D10_TRACE_ZINK_INSTANCE_CALL,
   D3D10_TRACE_ZINK_INSTANCE_INFO,
   D3D10_TRACE_RUNTIME_ERROR,
   D3D10_TRACE_RESOURCE_CREATE_BEGIN,
   D3D10_TRACE_RESOURCE_CREATE_SIZE,
   D3D10_TRACE_RESOURCE_CREATE_END,
   D3D10_TRACE_FORMAT_BEGIN,
   D3D10_TRACE_FORMAT_END,
   D3D10_TRACE_MULTISAMPLE,
   D3D10_TRACE_DESTROY_DEVICE_BEGIN,
   D3D10_TRACE_DESTROY_DEVICE_END,
   D3D10_TRACE_RUNTIME_MEMORY,
   D3D10_TRACE_RUNTIME_CONTEXT,
   D3D10_TRACE_ROTATE_IDENTITIES,
   D3D10_TRACE_PRIMARY_LAYOUT,
};

struct d3d10_umd_trace_data {
   int32_t result;
   uint32_t reserved;
   uint64_t arg0, arg1;
   WCHAR image[32];
};

struct d3d10_umd_trace_message {
   WCHAR image[32];
   WCHAR message[1024];
};

struct d3d10_umd_trace_error {
   int32_t result;
   uint32_t line;
   WCHAR image[32];
   WCHAR file[64];
   WCHAR function[64];
};

#ifdef __cplusplus
extern "C" {
#endif
void D3D10Trace(enum d3d10_umd_trace_event event, HRESULT result,
                uint64_t arg0, uint64_t arg1);
void D3D10TraceMessage(const char *message);
void D3D10TraceError(HRESULT result, const char *file, const char *function,
                     unsigned line);
#ifdef __cplusplus
}
#endif
#endif
