// SPDX-License-Identifier: MIT
// Point-sample the visible NV12 picture into the 640x360 acceptance window.
#ifndef NVDEC_PREVIEW_SCALE
#define NVDEC_PREVIEW_SCALE 2
#endif
float4 preview_vs(uint vertex_id : SV_VertexID) : SV_Position
{
    float2 p = float2((vertex_id << 1) & 2, vertex_id & 2);
    return float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
}

Texture2D<float> luma : register(t0);
Texture2D<float2> chroma : register(t1);
cbuffer Conversion : register(b0)
{
    float4 source_region;
    float4 range_scale;
    column_major float4x4 yuv_to_rgb;
};

float4 preview_ps(float4 position : SV_Position) : SV_Target
{
    int2 source = int2(position.xy) * NVDEC_PREVIEW_SCALE + NVDEC_PREVIEW_SCALE / 2;
    float y = (luma.Load(int3(source, 0)) * 255 - range_scale.x) * range_scale.y;
    float2 uv = (chroma.Load(int3(source / 2, 0)) * 255 - 128) * range_scale.z;
    return float4(saturate(mul(yuv_to_rgb, float4(y, uv, 1)).rgb), 1);
}
