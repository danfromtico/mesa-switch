// SPDX-License-Identifier: MIT
// Fixed 640x360 preview of the 1280x720 NV12 acceptance stream.
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
    int2 source = int2(position.xy) * 2 + 1;
    float y = (luma.Load(int3(source, 0)) * 255 - range_scale.x) * range_scale.y;
    float2 uv = (chroma.Load(int3(source / 2, 0)) * 255 - 128) * range_scale.z;
    return float4(saturate(mul(yuv_to_rgb, float4(y, uv, 1)).rgb), 1);
}
