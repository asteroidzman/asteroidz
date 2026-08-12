#version 450
#extension GL_GOOGLE_include_directive : require
#include "push.glsl"
#include "rounded.glsl"

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

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

void main() {
	vec4 c = texture(tex, v_uv);
	c.a = mix(1.0, c.a, pc.params.y);
	/* Coverage multiplies the whole premultiplied vec4, exactly as opacity
	 * does. Scaling only .a would leave the colour too bright and show as a
	 * light fringe along every rounded edge. */
	float cov = az_rounded_coverage(pc.round_box.xy,
		pc.round_box.zw - pc.round_box.xy, pc.corners, false);
	/* Inert today -- no texture command carries an interior cut-out yet. It is
	 * applied anyway so the annulus is a property of the PRIMITIVE and not of
	 * the rect pipeline, which is what M4D's shadows and M4F's blur need when
	 * they start carrying a clipped_region of their own. Zero inner corners
	 * cost one uniform branch. */
	cov *= az_rounded_coverage(pc.inner_box.xy,
		pc.inner_box.zw - pc.inner_box.xy, pc.inner_corners, true);
	out_color = c * pc.params.x * cov;
}
