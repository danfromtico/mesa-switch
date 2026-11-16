// SPDX-License-Identifier: MIT
//
// NV12 -> RGB.  Y and interleaved CbCr are sampled separately so the caller
// controls the plane pitch/offset.  range.xyz selects the luma offset, luma
// scale and chroma scale (BT.601/709 limited or full range).  yuv2rgb is a
// row-major matrix with the output offset in column 3.
#version 450
layout(location = 0) in vec2 texcoord;
layout(location = 0) out vec4 color;
layout(set = 0, binding = 0) uniform sampler2D y_plane;
layout(set = 0, binding = 1) uniform sampler2D uv_plane;
layout(push_constant) uniform Params {
   vec4 source_region;
   vec4 range;
   mat4 yuv2rgb;
} pc;
void main()
{
   float y = (texture(y_plane, texcoord).r * 255.0 - pc.range.x) * pc.range.y;
   vec2 uv = (texture(uv_plane, texcoord).rg * 255.0 - 128.0) * pc.range.z;
   vec3 yuv = vec3(y, uv.x, uv.y);
   vec3 rgb = mat3(pc.yuv2rgb) * yuv + pc.yuv2rgb[3].xyz;
   color = vec4(clamp(rgb, 0.0, 1.0), 1.0);
}
