// SPDX-License-Identifier: MIT
#version 450
layout(constant_id = 0) const uint rotation = 0;
layout(location = 0) out vec2 texcoord;
layout(push_constant) uniform SourceRegion { vec4 uv; } source;
void main()
{
   vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
   gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
   // DXGI Blt angles are counter-clockwise. Inverse-map destination to source;
   // the positive Vulkan viewport and texture coordinates both start at top.
   if (rotation == 1) texcoord = vec2(1.0 - p.y, p.x);
   else if (rotation == 2) texcoord = vec2(1.0 - p.x, 1.0 - p.y);
   else if (rotation == 3) texcoord = vec2(p.y, 1.0 - p.x);
   else texcoord = p;
   texcoord = source.uv.xy + texcoord * source.uv.zw;
}
