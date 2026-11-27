/* SPDX-License-Identifier: MIT */
#pragma once
#include "tgd3d.h"
#include <d3d9.h>
#include <memory>
namespace tgd3d::legacy9 {

constexpr UINT max_vertex_index = 0xffff;
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
   Surface *vertexStream = nullptr;
   Surface *indexStream = nullptr;
   Surface *sampled = nullptr;
   DrawState *draw = nullptr;
   D3D10DDI_HDEVICE handle() { return {&slot}; }
   void begin() { error = slot.state->lost; }
};

struct TextureLevel {
   UINT width, height, pitch;
   std::unique_ptr<BYTE[]> owned;
   BYTE *bytes = nullptr;
   RECT area{}, pendingUpload{};
   bool locked = false, write = false;
};

struct Surface {
   LegacyDevice *owner;
   Surface *next = nullptr;
   UINT width, height;
   DXGI_FORMAT format;
   std::unique_ptr<BYTE[]> ownedBytes;
   std::unique_ptr<TextureLevel[]> texture;
   UINT levels = 0, lockedLevels = 0;
   bool needsTextureUpload = false;
   UINT indexSize = 0; // Zero for vertex buffers, two or four bytes for index buffers.
   bool mightDrawLocked = false, allowLockedDraw = false;
   bool isBuffer() const { return format == DXGI_FORMAT_UNKNOWN; }
   BYTE *bufferData() const { return external ? external : ownedBytes.get(); }
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

HRESULT create_texture(LegacyDevice *dev, D3DDDIARG_CREATERESOURCE *args, bool displayable);
HRESULT APIENTRY texture_blt(HANDLE handle, const D3DDDIARG_TEXBLT1 *args);
HRESULT APIENTRY update_subresource(HANDLE handle, const D3DDDIARG_UPDATESUBRESOURCEUP *args);
HRESULT prepare_texture(Surface *surface);
HRESULT lock_texture(Surface *texture, D3DDDIARG_LOCK *args);
HRESULT unlock_texture(Surface *texture, const D3DDDIARG_UNLOCK *args);
void install_draw_functions(D3DDDI_DEVICEFUNCS *table);
RECT clear_bounds(const LegacyDevice *dev);
void draw_target_changed(LegacyDevice *dev);
void destroy_draw_state(LegacyDevice *dev);
}
