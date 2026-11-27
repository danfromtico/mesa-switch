/* SPDX-License-Identifier: MIT */
#ifdef VERTEX
#ifdef SWEEP
cbuffer Shape : register(b0) { uint shape; };
#endif
float4 main(uint id : SV_VertexID) : SV_Position
{
    // Keep the entire triangle in clip space and sample an interior 8x4 tile.
    float cut = 35.65625;
#ifdef SWEEP
    bool horizontal = shape > 16u;
    uint index = shape == 0u ? 10u : ((shape - 1u) % 16u);
    cut = (horizontal ? 34.0 : 35.0) + (2u * index + 1u) / 32.0;
#endif
    float2 p = id == 0 ? float2(cut, 16) : id == 1 ? float2(cut, 56) : float2(0, 36);
#ifdef SWEEP
    if (horizontal) p = p.yx;
#endif
    return float4(p.x / 32 - 1, 1 - p.y / 32, 0, 1);
}
#elif defined(COLOR_MASK)
uint main(uint coverage : SV_Coverage) : SV_Target
{
    return coverage;
}
#else
RWByteAddressBuffer result : register(u0);
void main(float4 position : SV_Position, uint coverage : SV_Coverage)
{
    uint2 pixel = uint2(position.xy) - uint2(32, 32);
    if (pixel.x >= 8 || pixel.y >= 4) return;
    uint index = 8 * pixel.y + pixel.x;
#ifdef COUNT_INVOCATIONS
    uint ignored;
    result.InterlockedAdd(8 * index + 4, 1, ignored);
    index *= 2;
#endif
    result.Store(4 * index, coverage | (GetRenderTargetSampleCount() << 16));
}
#endif
