#version 450
#extension GL_GOOGLE_include_directive : require
#include "rounded.glsl"

/* A solid rectangle. The colour arrives PREMULTIPLIED, matching the blend
 * state (ONE, ONE_MINUS_SRC_ALPHA) and matching what every client buffer is,
 * so there is exactly one alpha convention in the renderer. */

layout(push_constant) uniform Push {
	vec4 dst;
	vec4 uv_org_dx;
	vec4 uv_dy;
	vec4 color;
	vec4 params;
	vec4 round_box;   // x0, y0, x1, y1 in output pixels
	vec4 corners;     // CLOCKWISE: tl, tr, br, bl, in output pixels
} pc;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

void main() {
	float cov = az_rounded_coverage(pc.round_box.xy,
		pc.round_box.zw - pc.round_box.xy, pc.corners, false);
	out_color = pc.color * pc.params.x * cov;
}
