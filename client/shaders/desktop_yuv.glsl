/*
 * WiVRn VR streaming
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#version 450

// Converts a decoded desktop frame (YCbCr, sampled through the conversion of an
// immutable sampler) to an RGBA image that the GUI can draw.

#ifdef VERT_SHADER
// Decoded buffers can be padded, only the cropped part is converted
layout(push_constant) uniform pc
{
	vec2 uv_scale;
};

layout(location = 0) out vec2 outUV;

void main()
{
	// Fullscreen triangle
	vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
	outUV = uv * uv_scale;
	gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
#endif

#ifdef FRAG_SHADER
layout(set = 0, binding = 0) uniform sampler2D frame;

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

void main()
{
	outColor = vec4(texture(frame, inUV).rgb, 1.0);
}
#endif
