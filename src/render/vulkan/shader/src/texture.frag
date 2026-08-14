#version 450
#extension GL_GOOGLE_include_directive : require
#include "push.glsl"
#include "rounded.glsl"
#include "color.glsl"

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

/*
 * ── M5/C7: WHICH DECODE THIS PIPELINE VARIANT COMPILES ────────────────────
 *
 * A SPECIALISATION CONSTANT, not a uniform branch. It is folded at
 * pipeline-compile time, so the branch costs nothing at runtime and the decode
 * paths this variant does not use are not present in it at all. The value is
 * constant across a whole draw -- it comes from the source's luminance domain,
 * which is resolved at commit -- so paying for it per texel would be paying for
 * nothing.
 *
 * ZERO IS THE PATH THAT EXISTS TODAY. AVK composites in the surfaces' own
 * encoding and decodes nothing; the default variant must therefore be exactly
 * what shipped before M5, so that every pipeline that has not been taught to
 * ask for a variant keeps rendering what it always did.
 */
#define AZ_DECODE_NONE     0
#define AZ_DECODE_SRGB     1
#define AZ_DECODE_GAMMA22  2
#define AZ_DECODE_BT1886   3
layout(constant_id = 0) const int az_decode = AZ_DECODE_NONE;

layout(set = 0, binding = 0) uniform sampler2D tex;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

void main() {
	vec4 c = texture(tex, v_uv);
	c.a = mix(1.0, c.a, pc.params.y);

	/*
	 * ── DECODE, ON THE UN-PREMULTIPLIED VALUE ────────────────────────────
	 *
	 * A transfer function is defined on a colour, and a premultiplied texel is
	 * a colour that has already been multiplied by its coverage. Feeding it
	 * straight to an EOTF applies the curve to the product, which is wrong
	 * everywhere alpha is neither 0 nor 1 -- and wrong in the direction that
	 * makes translucent windows too dark, which reads as "the blur got
	 * heavier" rather than as a colour bug.
	 *
	 * So: divide out, decode, scale, multiply back. The guard is on alpha
	 * being nonzero rather than on a branch per component, and a fully
	 * transparent texel keeps its zero.
	 */
	if (az_decode != AZ_DECODE_NONE) {
		float a = c.a;
		vec3 straight = a > 0.0 ? c.rgb / a : vec3(0.0);
		if (az_decode == AZ_DECODE_SRGB) {
			straight = az_srgb_eotf(straight);
		} else if (az_decode == AZ_DECODE_GAMMA22) {
			straight = az_gamma22_eotf(straight);
		} else {
			straight = az_bt1886_eotf(straight);
		}
		/* M5/C2: the source's luminance scale, applied in LINEAR light where
		 * it means what it says. See AZ_TEX_LUM_SCALE. */
		straight *= AZ_TEX_LUM_SCALE;
		c.rgb = straight * a;
	} else {
		/* The pre-M5 path: no decode, and the scale is an identity because
		 * every source resolves to 1.0 until a variant is selected. */
		c.rgb *= AZ_TEX_LUM_SCALE;
	}
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
