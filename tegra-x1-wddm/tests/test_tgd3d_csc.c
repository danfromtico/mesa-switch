/* SPDX-License-Identifier: MIT
 *
 * CPU control for the tgd3d NV12 CSC.  It checks the same matrices, range
 * handling, UV order, pitch/offset and clipping that the validated SPIR-V
 * shader uses.  This is not a GPU render result.
 */
#include "tgd3d_csc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

static int close8(int a, int b, int tol)
{
    const int d = a - b;
    return d <= tol && d >= -tol;
}

static void fill_bars(uint8_t *rgba, uint32_t pitch, uint32_t width, uint32_t height)
{
    static const uint8_t bars[8][3] = {
        {191, 191, 191}, {191, 191, 0}, {0, 191, 191}, {0, 191, 0},
        {191, 0, 191}, {191, 0, 0}, {0, 0, 191}, {0, 0, 0},
    };
    uint32_t y, x;
    for (y = 0; y < height; y++) {
        uint8_t *row = rgba + (size_t)y * pitch;
        for (x = 0; x < width; x++) {
            const uint32_t b = x * 8u / width;
            row[x * 4 + 0] = bars[b][0];
            row[x * 4 + 1] = bars[b][1];
            row[x * 4 + 2] = bars[b][2];
            row[x * 4 + 3] = 255;
        }
    }
}

static void test_roundtrip(enum tgd3d_csc_matrix matrix, enum tgd3d_csc_range range)
{
    const uint32_t w = 1280, h = 720, pitch = 1344, ah = 720;
    const size_t nv_size = (size_t)pitch * ah * 3u / 2u;
    uint8_t *rgba = malloc((size_t)pitch * 4u * h);
    uint8_t *nv12 = calloc(1, nv_size);
    uint8_t *back = malloc((size_t)pitch * 4u * h);
    struct tgd3d_csc_push params;
    uint32_t y, x;

    CHECK(rgba && nv12 && back);
    if (!rgba || !nv12 || !back) {
        free(rgba); free(nv12); free(back);
        return;
    }
    fill_bars(rgba, pitch * 4u, w, h);
    tgd3d_csc_rgb_to_nv12(rgba, pitch * 4u, w, h, matrix, range, nv12, pitch, ah);
    tgd3d_csc_params(&params, matrix, range);
    tgd3d_csc_nv12_to_rgba(nv12, w, h, pitch, ah, &params, back, pitch * 4u);

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            const uint8_t *a = rgba + (size_t)y * pitch * 4u + x * 4u;
            const uint8_t *b = back + (size_t)y * pitch * 4u + x * 4u;
            if (!close8(a[0], b[0], 2) || !close8(a[1], b[1], 2) ||
                !close8(a[2], b[2], 2)) {
                fprintf(stderr, "roundtrip mismatch matrix=%d range=%d at %u,%u: "
                        "%u,%u,%u -> %u,%u,%u\n", matrix, range, x, y,
                        a[0], a[1], a[2], b[0], b[1], b[2]);
                failures++;
                free(rgba); free(nv12); free(back);
                return;
            }
        }
    }
    free(rgba);
    free(nv12);
    free(back);
}

static void test_gray(void)
{
    const uint32_t w = 16, h = 16, pitch = 32, ah = 16;
    uint8_t nv12[32 * 16 * 3 / 2];
    uint8_t rgba[32 * 4 * 16];
    struct tgd3d_csc_push params;
    uint32_t i;

    memset(nv12, 0, sizeof(nv12));
    for (i = 0; i < h; i++) {
        memset(nv12 + i * pitch, 126 + (int)i, w);
        memset(nv12 + pitch * ah + (i / 2) * pitch, 128, pitch);
    }
    tgd3d_csc_params(&params, TGD3D_CSC_BT601, TGD3D_CSC_LIMITED);
    tgd3d_csc_nv12_to_rgba(nv12, w, h, pitch, ah, &params, rgba, pitch * 4u);
    for (i = 0; i < h; i++) {
        const uint8_t *p = rgba + (size_t)i * pitch * 4u;
        CHECK(close8(p[0], p[1], 2) && close8(p[1], p[2], 2));
    }
}

static void test_uv_order(void)
{
    const uint32_t w = 16, h = 16, pitch = 16, ah = 16;
    uint8_t nv12[16 * 16 * 3 / 2];
    uint8_t swapped[16 * 16 * 3 / 2];
    uint8_t a[16 * 4 * 16], b[16 * 4 * 16];
    struct tgd3d_csc_push params;
    uint32_t i, diff = 0;

    memset(nv12, 0, sizeof(nv12));
    for (i = 0; i < h; i++)
        memset(nv12 + i * pitch, 120, w);
    for (i = 0; i < h / 2; i++) {
        uint8_t *uv = nv12 + pitch * ah + i * pitch;
        uint32_t x;
        for (x = 0; x < w; x += 2) {
            uv[x] = 90;      /* U */
            uv[x + 1] = 240; /* V: strong red */
        }
    }
    memcpy(swapped, nv12, sizeof(nv12));
    for (i = 0; i < h / 2; i++) {
        uint8_t *uv = swapped + pitch * ah + i * pitch;
        uint32_t x;
        for (x = 0; x < w; x += 2) {
            const uint8_t u = uv[x];
            uv[x] = uv[x + 1];
            uv[x + 1] = u;
        }
    }
    tgd3d_csc_params(&params, TGD3D_CSC_BT601, TGD3D_CSC_LIMITED);
    tgd3d_csc_nv12_to_rgba(nv12, w, h, pitch, ah, &params, a, pitch * 4u);
    tgd3d_csc_nv12_to_rgba(swapped, w, h, pitch, ah, &params, b, pitch * 4u);
    for (i = 0; i < w * h; i++) {
        if (!close8(a[i * 4 + 0], b[i * 4 + 0], 4) ||
            !close8(a[i * 4 + 2], b[i * 4 + 2], 4))
            diff++;
    }
    CHECK(diff == w * h);
}

static void test_params(void)
{
    struct tgd3d_csc_push p;
    tgd3d_csc_params(&p, TGD3D_CSC_BT601, TGD3D_CSC_LIMITED);
    CHECK(p.range[0] == 16.0f);
    CHECK(p.range[1] > 0.0045f && p.range[1] < 0.0046f);
    CHECK(p.range[2] > 0.0044f && p.range[2] < 0.0045f);
    CHECK(p.yuv2rgb[0] == 1.0f && p.yuv2rgb[4] == 0.0f);
    CHECK(p.yuv2rgb[8] > 1.4f && p.yuv2rgb[8] < 1.41f);
    tgd3d_csc_params(&p, TGD3D_CSC_BT709, TGD3D_CSC_FULL);
    CHECK(p.range[0] == 0.0f);
    CHECK(p.yuv2rgb[8] > 1.57f && p.yuv2rgb[8] < 1.58f);
    CHECK(p.yuv2rgb[5] < -0.18f && p.yuv2rgb[5] > -0.19f);
}

static void test_clip(void)
{
    const uint32_t w = 4, h = 2, pitch = 8, ah = 2;
    uint8_t nv12[8 * 2 * 3 / 2];
    uint8_t rgba[8 * 4 * 2];
    struct tgd3d_csc_push params;

    memset(nv12, 0, sizeof(nv12));
    /* Two 2x2 chroma blocks: (Y,U,V) = (16,255,0) and (235,0,255). */
    nv12[0] = 16; nv12[1] = 16; nv12[2] = 235; nv12[3] = 235;
    nv12[4] = 16; nv12[5] = 16; nv12[6] = 235; nv12[7] = 235;
    {
        uint8_t *uv = nv12 + pitch * ah;
        uv[0] = 255; uv[1] = 0;
        uv[2] = 0; uv[3] = 255;
    }
    tgd3d_csc_params(&params, TGD3D_CSC_BT601, TGD3D_CSC_LIMITED);
    tgd3d_csc_nv12_to_rgba(nv12, w, h, pitch, ah, &params, rgba, pitch * 4u);
    /* Block 0: red is clipped low, blue is clipped high. */
    CHECK(rgba[0] == 0);
    CHECK(rgba[2] == 255);
    /* Block 1: red is clipped high, blue is clipped low. */
    CHECK(rgba[(2 * 4) + 0] == 255);
    CHECK(rgba[(2 * 4) + 2] == 0);
    CHECK(rgba[3] == 255 && rgba[(2 * 4) + 3] == 255);
}

int main(void)
{
    test_roundtrip(TGD3D_CSC_BT601, TGD3D_CSC_LIMITED);
    test_roundtrip(TGD3D_CSC_BT601, TGD3D_CSC_FULL);
    test_roundtrip(TGD3D_CSC_BT709, TGD3D_CSC_LIMITED);
    test_roundtrip(TGD3D_CSC_BT709, TGD3D_CSC_FULL);
    test_gray();
    test_uv_order();
    test_params();
    test_clip();
    if (failures) {
        fprintf(stderr, "tgd3d NV12 CSC control: %d failure(s)\n", failures);
        return 1;
    }
    printf("tgd3d NV12 CSC (BT.601/709, limited/full, pitch/UV/clip): PASS (CPU control only)\n");
    return 0;
}
