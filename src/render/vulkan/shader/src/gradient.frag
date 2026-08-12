#version 450
#extension GL_GOOGLE_include_directive : require
#include "push.glsl"
#include "rounded.glsl"
#include "gradient.glsl"

/*
 * A rectangle filled with a gradient.
 *
 * This is a MATERIAL swap and nothing else: the geometry -- the rounded outer
 * edge, the rounded inner cut-out that makes a border an annulus -- is the same
 * az_rounded_coverage() calls quad.frag makes, from the same file, against the
 * same push constants. That is deliberate and it is the reason a gradient
 * BORDER needs no second border path: M4B's annulus already exists, and M4C
 * only changes what colour fills it.
 *
 * The reference does the same composition, in quad_grad_round.frag:
 *
 *     rect_alpha = v_color.a * quad_corner_alpha * clip_corner_alpha
 *     gl_FragColor = mix(vec4(0.0), gradient(...), rect_alpha)
 *
 * -- so the rect's OWN alpha gates the gradient, and the gradient supplies the
 * colour. pc.color.a is that alpha. Its rgb is deliberately unused.
 */

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

void main() {
	float cov = az_rounded_coverage(pc.round_box.xy,
		pc.round_box.zw - pc.round_box.xy, pc.corners, false);
	cov *= az_rounded_coverage(pc.inner_box.xy,
		pc.inner_box.zw - pc.inner_box.xy, pc.inner_corners, true);

	vec4 g = az_gradient(AZ_GRAD_RECORD, pc.round_box.xy,
		pc.round_box.zw - pc.round_box.xy);
	out_color = g * pc.color.a * pc.params.x * cov;
}
