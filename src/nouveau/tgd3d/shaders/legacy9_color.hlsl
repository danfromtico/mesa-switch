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
