#version 450

/*
 * Dual-Kawase DOWNSAMPLE: the 5-tap half-resolution step.
 *
 * One centre tap weighted 4, four diagonal taps weighted 1, divided by 8. The
 * diagonals sit half a source texel out so that bilinear filtering folds four
 * texels into each of them -- which is why five taps cover a 4x4 neighbourhood
 * and why the sampler MUST be linear. With a nearest sampler this is a sparse
 * five-point cross and the result shows a visible plus-shaped kernel.
 *
 * The destination is half the source in each axis, so this shader is run with a
 * viewport of the SMALLER image and its UV mapping already spans the whole
 * source; the reference's `v_texcoord * 2.0` is folded into the CPU-side origin
 * and edge vectors instead (see quad.vert), so there is no magic factor here.
 */

#extension GL_GOOGLE_include_directive : require

#include "push.glsl"
#include "blur.glsl"

layout(set = 0, binding = 0) uniform sampler2D tex;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

void main() {
	vec2 h = AZ_BLUR_STEP;

	vec4 sum = az_blur_tap(tex, v_uv) * 4.0;
	sum += az_blur_tap(tex, v_uv - h);
	sum += az_blur_tap(tex, v_uv + h);
	sum += az_blur_tap(tex, v_uv + vec2(h.x, -h.y));
	sum += az_blur_tap(tex, v_uv - vec2(h.x, -h.y));

	out_color = sum / 8.0;
}
