// SPDX-License-Identifier: MIT
#version 450
layout(location = 0) out vec2 texcoord;
layout(push_constant) uniform Params {
   vec4 source_region;
   vec4 range;
   mat4 yuv2rgb;
} pc;
void main()
{
   vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
   gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
   texcoord = pc.source_region.xy + p * pc.source_region.zw;
}
