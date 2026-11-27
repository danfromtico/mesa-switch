/* SPDX-License-Identifier: MIT */
struct Vertex { uint4 value : CONTROL0; };
struct Control {
    uint4 value : CONTROL0;
#ifdef WIDE
    float4 extra : CONTROL1;
#endif
};
struct Pixel { float4 position : SV_Position; nointerpolation uint tag : TEXCOORD0; };

#ifdef REPLAY
Pixel main(float4 position : POSITION, uint tag : TEXCOORD0)
{
    Pixel p; p.position = position; p.tag = tag; return p;
}
#elif defined(VERTEX)
Vertex main(uint id : SV_VertexID)
{
    Vertex v; v.value = uint4(id + 1, 1, 2, 3); return v;
}
#elif defined(PIXEL)
uint main(Pixel p, bool front : SV_IsFrontFace) : SV_Target
{
    uint2 xy = uint2(p.position.xy);
    return p.tag + xy.x + 8 * xy.y + (front ? 0x10000u : 0u);
}
#elif defined(GEOMETRY)
[maxvertexcount(3)]
void main(triangle Pixel input[3], inout TriangleStream<Pixel> stream)
{
    [unroll] for (uint i = 0; i < 3; ++i) {
        Pixel v = input[i]; v.tag += 1024; stream.Append(v);
    }
}
#else
#if defined(QUAD)
#define DOMAIN "quad"
#define OUTPUTS 4
#define EDGES 4
#define PARTITION "fractional_even"
#elif defined(LINE)
#define DOMAIN "isoline"
#define OUTPUTS 2
#define EDGES 2
#define PARTITION "fractional_odd"
#else
#define DOMAIN "tri"
#define OUTPUTS 3
#define EDGES 3
#define PARTITION "integer"
#endif
#ifndef INPUTS
#define INPUTS OUTPUTS
#endif
#ifdef LINE
#define PRIMITIVE "line"
#elif defined(POINTS)
#define PRIMITIVE "point"
#elif defined(CCW)
#define PRIMITIVE "triangle_ccw"
#else
#define PRIMITIVE "triangle_cw"
#endif
struct Constants {
    float edge[EDGES] : SV_TessFactor;
#ifdef QUAD
    float inside[2] : SV_InsideTessFactor;
#elif !defined(LINE)
    float inside : SV_InsideTessFactor;
#endif
    uint patchTag : PATCHDATA0;
};
cbuffer Params : register(b13) { uint seed; float factor; };
Texture2D<float> lut : register(t6);
SamplerState lookup : register(s3);
#ifdef HULL
RWByteAddressBuffer marks : register(u62);
Constants factors(InputPatch<Vertex, INPUTS> patch)
{
    Constants c;
    [unroll] for (uint i = 0; i < EDGES; ++i) c.edge[i] = factor;
#ifdef LINE
    c.edge[0] = factor > 0 ? 2.0 : 0.0;
#elif defined(QUAD)
    c.inside[0] = c.inside[1] = factor;
#else
    c.inside = factor;
#endif
    c.patchTag = seed + uint(lut.SampleLevel(lookup, float2(.25, .25), 0)) + patch[INPUTS - 1].value.x * 5 + 73;
    uint previous;
    marks.InterlockedOr(32, c.patchTag, previous);
    return c;
}
[domain(DOMAIN)] [partitioning(PARTITION)] [outputtopology(PRIMITIVE)]
[outputcontrolpoints(OUTPUTS)] [patchconstantfunc("factors")]
Control main(InputPatch<Vertex, INPUTS> patch, uint i : SV_OutputControlPointID)
{
    Control c;
    uint value = seed + uint(lut.SampleLevel(lookup, float2(.25, .25), 0)) + patch[INPUTS - 1].value.x * 3 + i * 13;
    c.value = uint4(value, 2, 3, 4);
#ifdef WIDE
    c.extra = float4(9, 10, 11, float(value));
#endif
    marks.Store(4 * i, value);
    return c;
}
#else
RWByteAddressBuffer marks : register(u63);
[domain(DOMAIN)]
Pixel main(Constants constants,
#if !defined(QUAD) && !defined(LINE)
           float3 location : SV_DomainLocation,
#else
           float2 location : SV_DomainLocation,
#endif
           const OutputPatch<Control, OUTPUTS> patch)
{
    uint2 q = uint2(round(saturate(location.xy) * 12));
    uint previous;
    marks.InterlockedAdd(0, 1, previous);
    marks.InterlockedOr(8 + 4 * q.y, 1u << q.x, previous);
    Pixel outp;
    outp.tag = seed + uint(lut.SampleLevel(lookup, float2(.75, .75), 0)) + constants.patchTag +
               patch[0].value.x + patch[OUTPUTS - 1].value.x;
    marks.InterlockedOr(4, outp.tag, previous);
#ifdef LINE
    outp.position = float4(location.x * 2 - 1, .5 - 2 * location.y, 0, 1);
#elif defined(QUAD)
    outp.position = float4(location.xy * 2 - 1, 0, 1);
#else
    outp.position = float4(location.xy * 4 - 1, 0, 1);
#endif
    return outp;
}
#endif
#endif
