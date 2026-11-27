/* SPDX-License-Identifier: MIT
 * Synthetic ETW transport control. Never creates a D3D/Vulkan device or GPU work.
 */
#include "tgd3d_trace.h"
#include <cstdio>
#include <initializer_list>

int main()
{
   FILETIME creation = {}, exit_time = {}, kernel = {}, user = {};
   if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit_time, &kernel, &user))
      return 1;
   const uint64_t start = (uint64_t(creation.dwHighDateTime) << 32) | creation.dwLowDateTime;
   const uint64_t begin = tgd3d::qpc_now();
   tgd3d::trace(TGD3D_TRACE_VULKAN, S_OK, 0xbc22f, (uint64_t(1) << 32) | 0x403000);
   tgd3d::trace(TGD3D_TRACE_CREATE_DEVICE, S_OK, 0xa0009, 0);
   for (tgd3d_trace_event event : {TGD3D_TRACE_DEVICE_STAGE,
                                  TGD3D_TRACE_SHADER_PHASE,
                                  TGD3D_TRACE_SUBMIT_PHASE,
                                  TGD3D_TRACE_RESOURCE_PHASE}) {
      tgd3d::trace(event, E_PENDING, 1, 0x1234);
      tgd3d::trace(event, S_OK, 1, 0x1234);
   }
   uint64_t wait_ticks[4] = {tgd3d::qpc_now(), 0, 0, 0};
   uint64_t wait_values[6] = {TGD3D_WAIT_DESTROY, 5, 0, 0, 0, 0};
   tgd3d::trace_timing(TGD3D_TIMING_CPU_WAIT_BEGIN, E_PENDING, 5, 4,
                      wait_ticks, wait_values);
   wait_ticks[1] = tgd3d::qpc_now();
   wait_values[2] = 2; /* Synthetic VK_TIMEOUT, no actual Vulkan wait. */
   tgd3d::trace_timing(TGD3D_TIMING_CPU_WAIT, E_FAIL, 5, 4, wait_ticks, wait_values);
   for (unsigned i = 0; i < 3; i++) {
      uint64_t ticks[4] = {tgd3d::qpc_now(), 0, 0, 0};
      uint64_t values[6] = {};
      tgd3d::trace_timing(TGD3D_TIMING_PRESENT, S_OK, i + 1, i, ticks, values);
      Sleep(1100); /* Cross real ETW buffer boundaries, without rendering. */
   }
   tgd3d::trace_message("CPU-only ETW fixture; all device and Present values are synthetic");
   std::printf("{\"pid\":%lu,\"process_start\":%llu,\"begin_qpc\":%llu,\"end_qpc\":%llu,\"gpu_work\":false}\n",
      GetCurrentProcessId(), (unsigned long long)start, (unsigned long long)begin,
      (unsigned long long)tgd3d::qpc_now());
   return 0;
}
