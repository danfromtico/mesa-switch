/* SPDX-License-Identifier: MIT */
#ifndef D3D10_RUNTIME_MEMORY_H
#define D3D10_RUNTIME_MEMORY_H

#include "DriverIncludes.h"
#include "vulkan/util/vk_wsi_win32_memory.h"

struct RuntimeResource;
struct Device;
struct Resource;
HRESULT RuntimeDeviceCreateContext(Device *device);
HRESULT RuntimeDeviceDestroyContext(Device *device);
HRESULT RuntimeDeviceFinish(Device *device);
HRESULT RuntimePrimaryCreate(Device *device, const D3D10DDIARG_CREATERESOURCE *desc,
                              D3D10DDI_HRTRESOURCE runtime, RuntimeResource **out);
struct RuntimeSharedDescription {
   UINT width, height, pitch;
   DXGI_FORMAT format;
   UINT bind_flags, usage;
};
HRESULT RuntimeSharedCreate(Device *device, const D3D10DDIARG_CREATERESOURCE *desc,
                             D3D10DDI_HRTRESOURCE runtime, RuntimeResource **out);
HRESULT RuntimeSharedOpen(Device *device, const D3D10DDIARG_OPENRESOURCE *args,
                           D3D10DDI_HRTRESOURCE runtime, RuntimeResource **out,
                           RuntimeSharedDescription *description);
HRESULT RuntimeResourceDestroy(RuntimeResource *primary);
HRESULT RuntimePrimarySetMode(RuntimeResource *primary, HANDLE device, UINT subresource);
HRESULT RuntimeResourceError(RuntimeResource *primary);
HRESULT RuntimeResourceFinish(Device *device, Resource *resource);
HRESULT RuntimePresent(const DXGI_DDI_ARG_PRESENT *args);
HRESULT RuntimeRotateOwnership(Device *device, Resource *const *resources, UINT count);
const wsi_win32_memory *RuntimeResourceMemory(RuntimeResource *primary);
UINT RuntimeResourcePitch(RuntimeResource *primary);

#endif
