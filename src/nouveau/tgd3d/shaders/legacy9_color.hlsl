/* SPDX-License-Identifier: MIT */
struct ColorVertex {
   float4 position : SV_Position;
   float4 diffuse : COLOR0;
};

ColorVertex color_vs(float4 position : POSITION, float4 diffuse : COLOR0)
{
   ColorVertex output;
   output.position = position;
   output.diffuse = diffuse;
   return output;
}

float4 color_ps(ColorVertex input) : SV_Target
{
   return input.diffuse;
}

struct TextureVertex {
   float4 position : SV_Position;
   float4 diffuse : COLOR0;
   float2 coord : TEXCOORD0;
};

TextureVertex texture_vs(float4 position : POSITION, float4 diffuse : COLOR0, float2 coord : TEXCOORD0)
{
   TextureVertex output = {position, diffuse, coord};
   return output;
}

Texture2D image : register(t0);
SamplerState pointSampler : register(s0);
cbuffer TextureStage : register(b0) {
   uint4 colorOp;
   uint4 alphaOp;
};

float4 stageArg(uint arg, float4 diffuse, float4 texel)
{
   float4 value = (arg & 15) == 2 ? texel : diffuse;
   if (arg & 32) value = value.aaaa;
   return (arg & 16) ? 1.0f - value : value;
}

float4 combine(uint4 state, float4 diffuse, float4 texel)
{
   float4 a = stageArg(state.y, diffuse, texel);
   float4 b = stageArg(state.z, diffuse, texel);
   if (state.x == 2) return a;
   if (state.x == 3) return b;
   return a * b;
}

float4 texture_ps(TextureVertex input) : SV_Target
{
   float4 sampleColor = image.Sample(pointSampler, input.coord);
   return float4(combine(colorOp, input.diffuse, sampleColor).rgb,
                 combine(alphaOp, input.diffuse, sampleColor).a);
}
