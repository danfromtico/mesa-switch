/* SPDX-License-Identifier: MIT */
#include "tgd3d_csc.h"

#include <math.h>
#include <string.h>

struct matrix {
    float y_r, y_g, y_b;
    float u_r, u_g, u_b;
    float v_r, v_g, v_b;
};

static struct matrix matrix_for(enum tgd3d_csc_matrix m)
{
    struct matrix out;
    if (m == TGD3D_CSC_BT709) {
        out.y_r = 1.0f; out.y_g = 1.0f; out.y_b = 1.0f;
        out.u_r = 0.0f; out.u_g = -0.187324f; out.u_b = 1.8556f;
        out.v_r = 1.5748f; out.v_g = -0.468124f; out.v_b = 0.0f;
    } else {
        out.y_r = 1.0f; out.y_g = 1.0f; out.y_b = 1.0f;
        out.u_r = 0.0f; out.u_g = -0.344136f; out.u_b = 1.772f;
        out.v_r = 1.402f; out.v_g = -0.714136f; out.v_b = 0.0f;
    }
    return out;
}

static uint8_t clamp8(float v)
{
    if (v <= 0.0f)
        return 0;
    if (v >= 1.0f)
        return 255;
    return (uint8_t)(v * 255.0f + 0.5f);
}

void tgd3d_csc_params(struct tgd3d_csc_push *out, enum tgd3d_csc_matrix matrix,
                      enum tgd3d_csc_range range)
{
    const struct matrix m = matrix_for(matrix);

    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    out->source_region[0] = 0.0f;
    out->source_region[1] = 0.0f;
    out->source_region[2] = 1.0f;
    out->source_region[3] = 1.0f;
    if (range == TGD3D_CSC_FULL) {
        out->range[0] = 0.0f;
        out->range[1] = 1.0f / 255.0f;
        out->range[2] = 1.0f / 255.0f;
    } else {
        out->range[0] = 16.0f;
        out->range[1] = 1.0f / 219.0f;
        out->range[2] = 1.0f / 224.0f;
    }
    /* Column-major mat4: column j is the coefficient of y/u/v for R/G/B. */
    out->yuv2rgb[0] = m.y_r;
    out->yuv2rgb[1] = m.y_g;
    out->yuv2rgb[2] = m.y_b;
    out->yuv2rgb[3] = 0.0f;
    out->yuv2rgb[4] = m.u_r;
    out->yuv2rgb[5] = m.u_g;
    out->yuv2rgb[6] = m.u_b;
    out->yuv2rgb[7] = 0.0f;
    out->yuv2rgb[8] = m.v_r;
    out->yuv2rgb[9] = m.v_g;
    out->yuv2rgb[10] = m.v_b;
    out->yuv2rgb[11] = 0.0f;
    out->yuv2rgb[12] = 0.0f;
    out->yuv2rgb[13] = 0.0f;
    out->yuv2rgb[14] = 0.0f;
    out->yuv2rgb[15] = 1.0f;
}

void tgd3d_csc_nv12_to_rgba(const uint8_t *nv12, uint32_t width, uint32_t height,
                            uint32_t pitch, uint32_t aligned_height,
                            const struct tgd3d_csc_push *params,
                            uint8_t *rgba, uint32_t rgba_pitch)
{
    uint32_t y, x;
    const uint8_t *uv = nv12 + (size_t)pitch * aligned_height;
    const float y_off = params->range[0];
    const float y_scale = params->range[1];
    const float c_scale = params->range[2];
    const float *m = params->yuv2rgb;

    for (y = 0; y < height; y++) {
        const uint8_t *yrow = nv12 + (size_t)y * pitch;
        const uint8_t *uvrow = uv + (size_t)(y / 2) * pitch;
        uint8_t *orow = rgba + (size_t)y * rgba_pitch;
        for (x = 0; x < width; x++) {
            const float yy = ((float)yrow[x] - y_off) * y_scale;
            const float uu = ((float)uvrow[(x / 2) * 2] - 128.0f) * c_scale;
            const float vv = ((float)uvrow[(x / 2) * 2 + 1] - 128.0f) * c_scale;
            const float r = m[0] * yy + m[4] * uu + m[8] * vv + m[12];
            const float g = m[1] * yy + m[5] * uu + m[9] * vv + m[13];
            const float b = m[2] * yy + m[6] * uu + m[10] * vv + m[14];
            orow[x * 4 + 0] = clamp8(r);
            orow[x * 4 + 1] = clamp8(g);
            orow[x * 4 + 2] = clamp8(b);
            orow[x * 4 + 3] = 255;
        }
    }
}

static uint8_t clamp_round(float v, float lo, float hi)
{
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return (uint8_t)(v + 0.5f);
}

void tgd3d_csc_rgb_to_nv12(const uint8_t *rgba, uint32_t rgba_pitch,
                           uint32_t width, uint32_t height,
                           enum tgd3d_csc_matrix matrix,
                           enum tgd3d_csc_range range,
                           uint8_t *nv12, uint32_t pitch, uint32_t aligned_height)
{
    const int bt709 = matrix == TGD3D_CSC_BT709;
    const float kr = bt709 ? 0.2126f : 0.299f;
    const float kg = bt709 ? 0.7152f : 0.587f;
    const float kb = bt709 ? 0.0722f : 0.114f;
    const float y_off = range == TGD3D_CSC_FULL ? 0.0f : 16.0f;
    const float y_scale = range == TGD3D_CSC_FULL ? 255.0f : 219.0f;
    const float c_scale = range == TGD3D_CSC_FULL ? 255.0f : 224.0f;
    uint8_t *uv = nv12 + (size_t)pitch * aligned_height;
    uint32_t y, x;

    for (y = 0; y < height; y++) {
        const uint8_t *irow = rgba + (size_t)y * rgba_pitch;
        uint8_t *orow = nv12 + (size_t)y * pitch;
        for (x = 0; x < width; x++) {
            const float r = irow[x * 4 + 0] / 255.0f;
            const float g = irow[x * 4 + 1] / 255.0f;
            const float b = irow[x * 4 + 2] / 255.0f;
            const float yy = kr * r + kg * g + kb * b;
            const float uu = (b - yy) / (2.0f * (1.0f - kb));
            const float vv = (r - yy) / (2.0f * (1.0f - kr));
            orow[x] = clamp_round(y_off + y_scale * yy, 0.0f, 255.0f);
            if (!(y & 1u) && !(x & 1u)) {
                uint8_t *dst = uv + (size_t)(y / 2) * pitch + x;
                dst[0] = clamp_round(128.0f + c_scale * uu, 0.0f, 255.0f);
                dst[1] = clamp_round(128.0f + c_scale * vv, 0.0f, 255.0f);
            }
        }
    }
}
