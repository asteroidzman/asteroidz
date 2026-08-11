#version 450

/* A solid rectangle. The colour arrives PREMULTIPLIED, matching the blend
 * state (ONE, ONE_MINUS_SRC_ALPHA) and matching what every client buffer is,
 * so there is exactly one alpha convention in the renderer. */

layout(push_constant) uniform Push {
	vec4 dst;
	vec4 uv_org_dx;
	vec4 uv_dy;
	vec4 color;
	vec4 params;
} pc;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

void main() {
	out_color = pc.color * pc.params.x;
}
