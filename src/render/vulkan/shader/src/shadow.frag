#version 450

/*
 * A rounded-rectangle drop shadow, evaluated analytically.
 *
 * No blurred texture, no CPU-generated bitmap, no multipass Gaussian: the
 * caster's geometry is known exactly, so its blurred coverage is a function of
 * position and is computed in one pass. Technique after Evan Wallace,
 * https://madebyevan.com/shaders/fast-rounded-rectangle-shadows/ , which is
 * also what SceneFX's box_shadow.frag uses.
 *
 * PORTED DELIBERATELY CLOSELY, and the reason is the same as rounded.glsl's.
 * This is not "a Gaussian-blurred rounded rectangle" -- it is a specific
 * approximation of one, and a more principled implementation would differ
 * visibly from every shadow on the desktop today. Specifically:
 *
 *   - the x direction is a CLOSED FORM. The convolution of a Gaussian with a
 *     half-plane is an error function, so the blurred coverage of a scanline
 *     between two edges is a difference of two erf()s -- exact, one
 *     evaluation, no sampling. erf() itself is Abramowitz & Stegun's rational
 *     approximation, because GLSL has no erf().
 *
 *   - the y direction is NUMERICALLY INTEGRATED with FOUR samples over +-3
 *     sigma. Four. That is not a placeholder; it is the whole cost of the
 *     effect, and the reason a shadow is affordable at all.
 *
 *   - the corner radius used for a scanline is chosen by whether that scanline
 *     is above or below the caster's centre. A true 2D distance field would
 *     not do this, and would round the corners differently.
 *
 * THE BOX IS THE ENVELOPE, NOT THE CASTER. pc.round_box is the shadow node's
 * own box, which the compositor has already grown by (size + border) on every
 * side and then offset -- so the falloff has room to occupy. The caster is
 * that box inset by sigma. Getting this backwards produces a shadow that is
 * correct in the middle of an edge and clipped off at the corners.
 *
 * AND THE GAUSSIAN'S SIGMA IS HALF THE `blur_sigma` FIELD. Transcribing the
 * field name straight into the Gaussian makes every shadow twice as soft as
 * the reference's.
 */

#extension GL_GOOGLE_include_directive : require

#include "push.glsl"
#include "rounded.glsl"
#include "dither.glsl"
/* az_gaussian / az_erf / az_shadow_x / az_box_shadow. The blur node's
 * edge_softness is the same falloff and includes the same file. */
#include "shadow.glsl"

layout(location = 0) out vec4 out_color;

/* pc.params.y carries this command's blur sigma, in OUTPUT PIXELS. The field
 * is otherwise the texture pipeline's alpha-mask flag and the gradient
 * pipeline's record index; see push.glsl for the full overlay. */
#define AZ_SHADOW_SIGMA pc.params.y

/*
 * Peak-to-peak dither amplitude, as a fraction of full alpha. A PUSH CONSTANT
 * and not a compile-time constant, because the break switch would otherwise
 * need a second SPIR-V module and a second pipeline for a value that is one
 * multiply -- and a shadow leaves uv_org_dx completely unused (it is the
 * texture pipeline's UV origin), so there is a free slot sitting right there.
 * See push.glsl for the full overlay.
 */
#define AZ_SHADOW_DITHER pc.uv_org_dx.x

void main() {
	vec2 lo = pc.round_box.xy;
	vec2 hi = pc.round_box.zw;
	float sigma = max(AZ_SHADOW_SIGMA, 1e-4);

	/*
	 * The caster, inset by the full blur_sigma on every side -- NOT by the
	 * halved Gaussian sigma. Two different numbers doing two different jobs:
	 * the inset is how much room the envelope leaves for the falloff, and it
	 * is the compositor's `size` that put the room there.
	 */
	float inset = AZ_SHADOW_SIGMA;
	vec2 caster_lo = lo + inset;
	vec2 caster_hi = hi - inset;
	/* A window smaller than its own falloff would give an inverted caster and
	 * a shadow that grows as the window shrinks. */
	caster_hi = max(caster_hi, caster_lo);

	float coverage = az_box_shadow(caster_lo, caster_hi, az_frag_global(),
		sigma * 0.5, pc.corners);
	coverage = clamp(coverage, 0.0, 1.0);

	/*
	 * The interior cut-out: the window's own footprint, kept out of its own
	 * shadow. az_rounded_coverage returns 1.0 for all-zero radii, which is the
	 * same "arcs only, the region did the rest" contract the border path uses
	 * -- and the same thing the reference's corner_alpha() does.
	 *
	 * The half-pixel-and-a-bit inset is the reference's, kept: the cut-out and
	 * the window's own outer arc are rasterised by different primitives, and
	 * abutting them exactly leaves an antialiasing seam that shows the
	 * wallpaper through as a bright wedge at the corner.
	 */
	vec2 clip_pos = pc.inner_box.xy + 0.75;
	vec2 clip_size = (pc.inner_box.zw - pc.inner_box.xy) - 1.5;
	float clip = az_rounded_coverage(clip_pos, max(clip_size, vec2(0.0)),
		pc.inner_corners, true);

	float alpha = pc.color.a * pc.params.x * coverage * clip;

	/*
	 * ANTI-BANDING. See dither.glsl -- the amplitude arrives already derived
	 * from the attachment's precision, so this shader neither knows nor
	 * assumes that the target is 8-bit.
	 */
	alpha = az_dither_alpha(alpha, AZ_SHADOW_DITHER);

	/*
	 * PREMULTIPLIED, which is a divergence and a deliberate one.
	 *
	 * REFERENCE BUG FIXED IN AVK: the shadow's colour is premultiplied by its
	 * own coverage. box_shadow.frag emits vec4(v_color.rgb, shadow_alpha) --
	 * the rgb is NOT multiplied by the alpha -- while the blend mode is
	 * PREMULTIPLIED. For the default black shadow rgb is 0 and the two agree
	 * exactly, which is why this has never been visible; for any non-black
	 * shadowscolor the reference emits rgb far larger than its alpha and the
	 * shadow adds light instead of removing it. Reproducing that would make a
	 * coloured shadow glow.
	 */
	out_color = vec4(pc.color.rgb * alpha, alpha);
}
