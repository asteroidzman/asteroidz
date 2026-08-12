#version 450

/*
 * Dual-Kawase UPSAMPLE: the 8-tap double-resolution step.
 *
 * Four axis taps weighted 1 and four diagonal taps weighted 2, divided by 12 --
 * a tent that reconstructs smoothly rather than replicating texels. The
 * asymmetry between this and the 5-tap downsample is the whole trick: the pair
 * together approximate a Gaussian far better than either kernel iterated alone,
 * and swapping the weights (or using the same kernel both ways) produces
 * visible blocking at the level boundaries.
 *
 * POST-EFFECTS ARE FOLDED IN HERE, on the last upsample only. Brightness,
 * contrast, saturation and noise as a separate full-screen pass would be one
 * more read and one more write of the whole result for arithmetic that costs
 * nothing where it already has the texel in a register. AZ_BLUR_EFFECTS is a
 * push constant and therefore uniform across the draw, so the branch is not
 * divergent.
 */

#extension GL_GOOGLE_include_directive : require

#include "push.glsl"
#include "blur.glsl"

layout(set = 0, binding = 0) uniform sampler2D tex;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

void main() {
	vec2 h = AZ_BLUR_STEP;

	vec4 sum = az_blur_tap(tex, v_uv + vec2(-h.x * 2.0, 0.0));
	sum += az_blur_tap(tex, v_uv + vec2(-h.x, h.y)) * 2.0;
	sum += az_blur_tap(tex, v_uv + vec2(0.0, h.y * 2.0));
	sum += az_blur_tap(tex, v_uv + vec2(h.x, h.y)) * 2.0;
	sum += az_blur_tap(tex, v_uv + vec2(h.x * 2.0, 0.0));
	sum += az_blur_tap(tex, v_uv + vec2(h.x, -h.y)) * 2.0;
	sum += az_blur_tap(tex, v_uv + vec2(0.0, -h.y * 2.0));
	sum += az_blur_tap(tex, v_uv + vec2(-h.x, -h.y)) * 2.0;

	vec4 color = sum / 12.0;
	if (AZ_BLUR_EFFECTS > 0.5) {
		color = az_blur_effects(color, v_uv);
	}
	out_color = color;
}
