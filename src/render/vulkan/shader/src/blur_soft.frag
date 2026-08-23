#version 450

/*
 * A blur node's finished result, composited with a SOFT edge.
 *
 * This is texture.frag with one substitution: where that shader gets its
 * coverage from rounded.glsl's hard SDF plus a ~1px antialiasing band, this one
 * gets it from shadow.glsl's wide analytic Gaussian -- the same coverage a drop
 * shadow's tint is drawn with, over the same box, with the same sigma.
 *
 * WHY THAT IS THE WHOLE FEATURE. A blur of uniform strength inside a hard
 * rounded rectangle reads as an obviously rectangular "blurred patch" of
 * whatever is behind it. asteroidz's producer sizes a shadow's backdrop blur to
 * the shadow's own footprint and hands it the shadow's own sigma
 * (src/animation/client.h:757, layer.h:331), precisely so the two fade over the
 * same span and blend into one continuous halo with no seam. If they used two
 * different falloffs there would be a visible ring where one ended.
 *
 * `pc.corners` changes meaning here, and the reference changes it too: with a
 * soft edge they are no longer a hard SDF radius but the soft box's OWN corner
 * radii, fed to the same integration. Same number from the producer either way.
 *
 * SIGMA IS IN OUTPUT PIXELS by the time it arrives, scaled at the walker beside
 * the box and the radii -- see the M4D note on shadow blur_sigma, and the
 * correction in docs/architecture.md about the reference scaling this field but
 * not the shadow's.
 */

#extension GL_GOOGLE_include_directive : require

#include "push.glsl"
#include "shadow.glsl"

layout(set = 0, binding = 0) uniform sampler2D tex;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

/* Same slot a shadow's blur_sigma uses, and for the same quantity. The texture
 * pipeline's alpha-mask flag and the gradient pipeline's record index also live
 * here; none of the three ever draws the same command. See push.glsl. */
#define AZ_BLUR_EDGE_SIGMA pc.params.y

void main() {
	vec4 c = texture(tex, v_uv);

	float sigma = max(AZ_BLUR_EDGE_SIGMA, 1e-4);
	/*
	 * The caster, inset by the FULL sigma -- not by the halved Gaussian sigma.
	 * Identical to shadow.frag and to the reference's
	 *
	 *     roundedBoxShadow(position + blur_sigma,
	 *                      position + size - blur_sigma, ..., blur_sigma * 0.5)
	 *
	 * The inset is the room the falloff occupies; the half-sigma is the falloff
	 * itself. They are two different numbers doing two different jobs, and
	 * collapsing them makes every soft edge twice as wide as the shadow it is
	 * supposed to match.
	 */
	vec2 lo = pc.round_box.xy + sigma;
	vec2 hi = max(pc.round_box.zw - sigma, lo);
	float cov = clamp(az_box_shadow(lo, hi, az_frag_global(), sigma * 0.5,
		pc.corners), 0.0, 1.0);

	/* Premultiplied throughout: the blur result is premultiplied, so scaling the
	 * whole vec4 is how coverage and opacity apply. Touching only .a would leave
	 * the colour too bright and show as a light fringe all round the fade. */
	out_color = c * pc.params.x * cov;
}
