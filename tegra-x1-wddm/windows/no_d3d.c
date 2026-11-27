/* SPDX-License-Identifier: MIT
 * Windows 11 23H2 requires a UMD name even after accepting ComputeOnly.
 * This DLL owns no GPU state. The Vulkan loader opens the separate NVK ICD.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>

struct _D3DDDIARG_OPENADAPTER;
struct D3D10DDIARG_OPENADAPTER;

__declspec(dllexport) HRESULT APIENTRY OpenAdapter(struct _D3DDDIARG_OPENADAPTER *args)
{
    (void)args;
    return D3DERR_NOTAVAILABLE;
}

__declspec(dllexport) HRESULT APIENTRY OpenAdapter10(struct D3D10DDIARG_OPENADAPTER *args)
{
    (void)args;
    return DXGI_ERROR_UNSUPPORTED;
}

__declspec(dllexport) HRESULT APIENTRY OpenAdapter10_2(struct D3D10DDIARG_OPENADAPTER *args)
{
    (void)args;
    return DXGI_ERROR_UNSUPPORTED;
}
