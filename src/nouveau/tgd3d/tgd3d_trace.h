/* SPDX-License-Identifier: MIT
 *
 * Opt-in ETW provider. Version 2 appends the emitter's process creation
 * FILETIME to each v1 payload. This is identity, not an event clock: live
 * consumers use raw QPC stamps so wall-clock corrections cannot hide events.
 */
#ifndef TGD3D_TRACE_H
#define TGD3D_TRACE_H

#include <windows.h>
#include <stdint.h>

/* {5f0c4c6e-7a0b-4b52-9d3a-2f6d1e8b4a11} */
#define TGD3D_TRACE_GUID_INIT \
   {0x5f0c4c6e, 0x7a0b, 0x4b52, {0x9d, 0x3a, 0x2f, 0x6d, 0x1e, 0x8b, 0x4a, 0x11}}

enum tgd3d_trace_event {
   TGD3D_TRACE_OPEN_ADAPTER = 1,   /* arg0 interface, arg1 version */
   TGD3D_TRACE_ADAPTER_INFO,       /* arg0 flags, arg1 budget bytes */
   TGD3D_TRACE_VULKAN,             /* arg0 LUID, arg1 api version | nvk << 32 */
   TGD3D_TRACE_CREATE_DEVICE,      /* arg0 interface, arg1 stage on failure */
   TGD3D_TRACE_DESTROY_DEVICE,
   TGD3D_TRACE_RUNTIME_MEMORY,     /* arg0 stage << 32 | allocation, arg1 value */
   TGD3D_TRACE_RUNTIME_CONTEXT,    /* arg0 stage */
   TGD3D_TRACE_RESOURCE,           /* Entry only: dimension << 32 | bind, width << 32 | height */
   TGD3D_TRACE_OPEN_RESOURCE,      /* arg0 width << 32 | height, arg1 pitch */
   TGD3D_TRACE_SET_DISPLAY_MODE,   /* arg0 allocation */
   TGD3D_TRACE_ROTATE,             /* arg0 resources */
   TGD3D_TRACE_BLT,                /* arg0 flags, arg1 rotate */
   TGD3D_TRACE_SHADER,             /* arg0 stage << 32 | token count, arg1 microseconds */
   /* E_PENDING is entry; the matching result is return, paired by PID/TID.
    * These events must not be counted as successful device creation/submission. */
   TGD3D_TRACE_DEVICE_STAGE,       /* arg0 creation stage 1..5, arg1 Device address */
   TGD3D_TRACE_SHADER_PHASE,       /* arg0 phase << 32 | stage, arg1 Shader address */
   TGD3D_TRACE_SUBMIT_PHASE,       /* arg0 phase (1 runtime signal, 2 Vulkan, 3 runtime wait), arg1 sequence */
   TGD3D_TRACE_RESOURCE_PHASE,     /* arg0 phase (0 DDI, 1 VkImage, 2 memory, 3 bind, 4 runtime bind), arg1 Resource address */
   TGD3D_TRACE_SHADER_CACHE,       /* arg0 0 miss, 1 binary hit, 2 queued write; arg1 binary bytes */
   TGD3D_TRACE_RED_PRIMARY,        /* diagnostic: arg0 phase << 32 | allocation; arg1 extent (0), wait target sequence (1) */
   TGD3D_TRACE_ERROR = 20,         /* tgd3d_trace_error */
   TGD3D_TRACE_MESSAGE = 21,       /* tgd3d_trace_message */
   /* arg0 operation << 32 | types/count, arg1 context identity or command bytes.
    * 1 create context, 2 destroy, 3 actual submit, 4 GPU wait, 5 GPU signal.
    * 6 runtime queue admission: low arg0 is the negotiated interface,
    * arg1 bits 0 and 1 report the public virtual-context and submit callbacks.
    * E_PENDING is entry; a successful return is admission, not GPU completion. */
   TGD3D_TRACE_RUNTIME_TRANSPORT = 22,
   TGD3D_TRACE_VIDEO = 23,         /* arg0 operation, arg1 real session or sync target/observed */
   TGD3D_TRACE_TIMING = 30,        /* tgd3d_trace_timing */
   TGD3D_TRACE_LEGACY9 = 31,       /* arg0 operation << 32 | selector, arg1 operation data */
   TGD3D_TRACE_COPY_FLAGS = 32,    /* arg0 raw D3D11.1 copy/update flags; result from flag validation */
   TGD3D_TRACE_PIPELINE_LEVELS = 33, /* arg0 advertised DDI level mask, arg1 graphics UAV slots */
};

enum tgd3d_timing_kind {
   /* qpc: first command, submit enter, submit return.
    * value: draws, render passes, upload bytes, GPU copy bytes, readback bytes. */
   TGD3D_TIMING_SUBMIT = 1,
   /* qpc: enter, after submit and fence gate, after backpressure, after PresentCb.
    * value: DDI flags, interval, in-flight submissions, backpressure ns. */
   TGD3D_TIMING_PRESENT = 2,
   /* qpc: wait begin, wait end. value: reason, target sequence, raw VkResult. */
   TGD3D_TIMING_CPU_WAIT = 3,
   /* Same fields, end is zero and result is E_PENDING. */
   TGD3D_TIMING_CPU_WAIT_BEGIN = 4,
   /* One record per QueueSubmit2 call. qpc: residency preparation enter/return.
    * value: resource-list nodes scanned, nodes used by this recording, Device
    * identity, VkResult (signed low 32 bits), accumulated make-resident ticks.
    * The outer interval includes any paging waits.
    * completed is unused. sequence is the attempted render timeline value. */
   TGD3D_TIMING_RESIDENCY_SCAN = 5,
};

enum tgd3d_wait_reason {
   TGD3D_WAIT_BACKPRESSURE = 1,
   TGD3D_WAIT_READBACK = 2,
   TGD3D_WAIT_DESTROY = 3,
   TGD3D_WAIT_QUERY = 4,
   TGD3D_WAIT_SYNC_CONTROL = 5,
   TGD3D_WAIT_MODE_CHANGE = 6,
};

struct tgd3d_trace_data {
   int32_t result;
   uint32_t reserved;
   uint64_t arg0, arg1;
   WCHAR image[32];
   uint64_t process_start_time;
};

struct tgd3d_trace_timing {
   int32_t result;
   uint32_t kind;
   uint64_t sequence, completed;
   uint64_t qpc[4];
   uint64_t value[6];
   uint64_t qpc_frequency;
   WCHAR image[32];
   uint64_t process_start_time;
};

struct tgd3d_trace_message {
   WCHAR image[32];
   WCHAR message[256];
   uint64_t process_start_time;
};

struct tgd3d_trace_error {
   int32_t result;
   uint32_t line;
   WCHAR image[32];
   WCHAR file[64];
   uint64_t process_start_time;
};

#ifdef __cplusplus
namespace tgd3d {
void trace(tgd3d_trace_event event, HRESULT result, uint64_t arg0, uint64_t arg1);
void trace_message(const char *message);
void trace_error(HRESULT result, const char *file, unsigned line);
bool trace_timing_enabled();
void trace_timing(tgd3d_timing_kind kind, HRESULT result, uint64_t sequence, uint64_t completed,
                  const uint64_t qpc[4], const uint64_t value[6]);
uint64_t qpc_now();
uint64_t qpc_nanoseconds(uint64_t ticks);
}
#endif

#endif
