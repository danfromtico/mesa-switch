/* SPDX-License-Identifier: MIT
 * Generated from legacy9_color.hlsl by generate_legacy9.py. */

static constexpr D3D10DDIARG_SIGNATURE_ENTRY color_vs_in[] = {
   {static_cast<D3D10_SB_NAME>(0), 0, 0xf},
   {static_cast<D3D10_SB_NAME>(0), 1, 0xf},
};

static constexpr D3D10DDIARG_SIGNATURE_ENTRY color_vs_out[] = {
   {static_cast<D3D10_SB_NAME>(1), 0, 0xf},
   {static_cast<D3D10_SB_NAME>(0), 1, 0xf},
};

static constexpr UINT color_vs[] = {
   0x00010040, 0x0000001a, 0x04000067, 0x001020f2, 0x00000000, 0x00000001,
   0x03000065, 0x001020f2, 0x00000001, 0x0300005f, 0x001010f2, 0x00000000,
   0x0300005f, 0x001010f2, 0x00000001, 0x05000036, 0x001020f2, 0x00000000,
   0x00101e46, 0x00000000, 0x05000036, 0x001020f2, 0x00000001, 0x00101e46,
   0x00000001, 0x0100003e,
};

static constexpr D3D10DDIARG_SIGNATURE_ENTRY color_ps_in[] = {
   {static_cast<D3D10_SB_NAME>(1), 0, 0xf},
   {static_cast<D3D10_SB_NAME>(0), 1, 0xf},
};

static constexpr D3D10DDIARG_SIGNATURE_ENTRY color_ps_out[] = {
   {static_cast<D3D10_SB_NAME>(0), 0, 0xf},
};

static constexpr UINT color_ps[] = {
   0x00000040, 0x0000000e, 0x03000065, 0x001020f2, 0x00000000, 0x03001062,
   0x001010f2, 0x00000001, 0x05000036, 0x001020f2, 0x00000000, 0x00101e46,
   0x00000001, 0x0100003e,
};
