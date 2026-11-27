/* SPDX-License-Identifier: MIT
 *
 * NV12 -> RGB color-space conversion shared by the tgd3d video path and its
 * CPU tests.  The shader in shaders/nv12.frag uses the same column-major
 * matrix and range constants.  U and V are the interleaved Cb/Cr order of
 * NV12: U/Cb first, then V/Cr.
 */
#ifndef TGD3D_CSC_H
#define TGD3D_CSC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum tgd3d_csc_matrix {
    TGD3D_CSC_BT601 = 0,
    TGD3D_CSC_BT709 = 1,
};

enum tgd3d_csc_range {
    TGD3D_CSC_LIMITED = 0,
    TGD3D_CSC_FULL = 1,
};

struct tgd3d_csc_push {
    float source_region[4];
    float range[4];       /* luma offset, luma scale, chroma scale, reserved */
    float yuv2rgb[16];    /* column-major mat4; column 3 is the offset */
};

void tgd3d_csc_params(struct tgd3d_csc_push *out, enum tgd3d_csc_matrix matrix,
                      enum tgd3d_csc_range range);
void tgd3d_csc_nv12_to_rgba(const uint8_t *nv12, uint32_t width, uint32_t height,
                            uint32_t pitch, uint32_t aligned_height,
                            const struct tgd3d_csc_push *params,
                            uint8_t *rgba, uint32_t rgba_pitch);
void tgd3d_csc_rgb_to_nv12(const uint8_t *rgba, uint32_t rgba_pitch,
                           uint32_t width, uint32_t height,
                           enum tgd3d_csc_matrix matrix,
                           enum tgd3d_csc_range range,
                           uint8_t *nv12, uint32_t pitch, uint32_t aligned_height);

#ifdef __cplusplus
}
#endif

#endif
