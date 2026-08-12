#version 450
#extension GL_GOOGLE_include_directive : require
#include "rounded.glsl"

/* A solid rectangle. The colour arrives PREMULTIPLIED, matching the blend
 * state (ONE, ONE_MINUS_SRC_ALPHA) and matching what every client buffer is,
 * so there is exactly one alpha convention in the renderer. */

layout(push_constant) uniform Push {
	vec4 uv_org_dx;
	vec4 uv_dy;
	vec4 color;
	vec4 params;
	vec4 round_box;     // OUTER x0, y0, x1, y1 in output pixels
	vec4 corners;       // CLOCKWISE: tl, tr, br, bl, in output pixels
	vec4 inner_box;     // INNER x0, y0, x1, y1 in output pixels
	vec4 inner_corners; // CLOCKWISE, in output pixels
} pc;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

void main() {
	/*
	 * A BORDER IS ONE PRIMITIVE, NOT AN OUTLINE.
	 *
	 *     border = outer_coverage * (1 - inner_coverage)
	 *
	 * written here as az_rounded_coverage(..., is_cutout = true), which is
	 * that complement. Both edges are the same SDF with the same
	 * derivative-scaled antialiasing, so the annulus closes on the diagonal
	 * where its two arcs meet -- which the square scissor cut it replaces did
	 * not, and that is the whole of the M4A artifact.
	 */
	float cov = az_rounded_coverage(pc.round_box.xy,
		pc.round_box.zw - pc.round_box.xy, pc.corners, false);
	cov *= az_rounded_coverage(pc.inner_box.xy,
		pc.inner_box.zw - pc.inner_box.xy, pc.inner_corners, true);
	out_color = pc.color * pc.params.x * cov;
}
