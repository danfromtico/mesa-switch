// SPDX-License-Identifier: MIT
#version 450
layout(location = 0) in vec2 texcoord;
layout(location = 0) out vec4 color;
// Existing tgd3d fragment-stage descriptor slots: sampler 0 and image 0.
layout(set = 1, binding = 288) uniform sampler source_sampler;
layout(set = 1, binding = 304) uniform texture2D source_image;
void main()
{
   color = textureLod(sampler2D(source_image, source_sampler), texcoord, 0.0);
}
