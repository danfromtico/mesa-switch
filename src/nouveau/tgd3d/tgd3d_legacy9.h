/* SPDX-License-Identifier: MIT */
#pragma once
#include "tgd3d.h"
#include <d3d9.h>
namespace tgd3d::legacy9 {

constexpr UINT max_primitives = 0xffff;
constexpr UINT max_stream_stride = 0xffff;

struct DrawState;
struct Surface;

struct LegacyDevice {
   DeviceSlot slot{};
   D3D10DDI_DEVICEFUNCS graphics{};
   HRESULT error = S_OK;
   PFND3DDDI_PRESENTCB present = nullptr;
   Surface *surfaces = nullptr;
   Surface *target = nullptr;
   DrawState *draw = nullptr;
   D3D10DDI_HDEVICE handle() { return {&slot}; }
   void begin() { error = slot.state->lost; }
};

struct Surface {
   LegacyDevice *owner;
   Surface *next = nullptr;
   UINT width, height;
   DXGI_FORMAT format;
   bool system, target, lockable, locked = false, write = false, hasShadow = false, shadowCurrent = false;
   bool externallyWritable = false;
   BYTE *external = nullptr;
   UINT externalPitch = 0;
   RECT lockedArea{};
   alignas(Resource) BYTE image[sizeof(Resource)];
   alignas(Resource) BYTE shadow[sizeof(Resource)];
   alignas(View) BYTE view[sizeof(View)];
   D3D10DDI_HRESOURCE resource() { return {image}; }
   D3D10DDI_HRESOURCE staging() { return system ? resource() : D3D10DDI_HRESOURCE{shadow}; }
   D3D10DDI_HRENDERTARGETVIEW rtv() { return {view}; }
   RECT bounds() const { return {0, 0, static_cast<LONG>(width), static_cast<LONG>(height)}; }
};

void install_draw_functions(D3DDDI_DEVICEFUNCS *table);
void draw_target_changed(LegacyDevice *dev);
void destroy_draw_state(LegacyDevice *dev);
}
