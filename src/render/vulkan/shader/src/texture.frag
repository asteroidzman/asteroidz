#version 450

/*
 * A client surface.
 *
 * Two things are happening beyond the sample:
 *
 *  - alpha_mask forces alpha to 1.0 for the DRM X formats (XRGB8888 and
 *    friends), whose fourth channel exists in memory and means nothing.
 *    Sampling it as alpha is what makes an opaque window translucent, and it
 *    looks so exactly like a blending bug that it gets chased in the wrong
 *    file.
 *  - the result is multiplied by opacity, not blended toward it. The texel is
 *    already premultiplied, so scaling the whole vec4 is the correct way to
 *    make a premultiplied surface more transparent; touching only .a would
 *    leave the colour too bright.
 */

layout(set = 0, binding = 0) uniform sampler2D tex;

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
	vec4 c = texture(tex, v_uv);
	c.a = mix(1.0, c.a, pc.params.y);
	out_color = c * pc.params.x;
}
