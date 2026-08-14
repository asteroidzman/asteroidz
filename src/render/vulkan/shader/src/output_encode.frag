#version 450

/*
 * THE OUTPUT ENCODE PASS. Contract C6, ADR-008.
 *
 * The single place in the whole renderer where scene values become output
 * electrical values on Path B, and -- invariant 1 -- the ONLY shader in the
 * tree permitted to call az_pq_ieotf(). tests/test-color-pipeline.c gate 7
 * greps every other shader source and fails the suite if a second PQ encode
 * appears anywhere.
 *
 * WHAT IT READS. The working intermediate: premultiplied scene values,
 * linear-light BT.709/D65, 1.0 = SDR reference white, unbounded above and
 * signed below (ADR-002/003). Composition is COMPLETE by the time this runs,
 * so the texel is treated as opaque and its alpha is dead -- the target's
 * alpha is written as 1.0 because every scanout format here is an X format.
 *
 * THE STEP ORDER IS ADR-008's, AND IT IS NOT NEGOTIABLE:
 *
 *   1  sample the composited scene value
 *   2  tone map, on the COMPOSITED value (fx_vk maps per source and then sums,
 *      which overshoots the panel wherever two HDR windows overlap)
 *   3  gamut matrix, then clamp negatives
 *   4  luminance anchor, PQ only -- this is where the relative scene meets an
 *      absolute encoding, and it is AFTER the matrix on purpose: putting it
 *      before would scale the values the matrix mixes, and the result is a
 *      plausible picture with the wrong white
 *   5  inverse EOTF
 *   6  dither, at the target's quantum, on the ELECTRICAL value
 *   7  write
 *
 * ADR-008's falsifier is "a luminance constant is on the wrong side of the
 * matrix", and it is falsifiable step by step because the CPU reference (C4)
 * computes every one of these intermediates separately.
 *
 * WHAT IS NOT HERE: a decode. The intermediate is already scene values. A
 * decode in this shader would be the second one applied to the same pixel,
 * which is the anti-pattern the whole milestone exists to remove.
 */

#extension GL_GOOGLE_include_directive : require

#include "color.glsl"

/*
 * The encode transfer function, as a SPECIALISATION CONSTANT rather than a
 * push constant or a branch.
 *
 * Per-output, fixed for the life of the output's state, and known when the
 * pipeline is created -- so each output gets a pipeline with the other curve
 * compiled out entirely. A uniform branch would cost little, but it would also
 * keep a PQ encode reachable in an SDR output's shader, and "the PQ code is
 * compiled out of every SDR pipeline" is a stronger statement than "the branch
 * is not taken".
 */
#define AZ_ENCODE_TF_SRGB 0
#define AZ_ENCODE_TF_PQ 1
layout(constant_id = 0) const int AZ_ENCODE_TF = AZ_ENCODE_TF_SRGB;

/*
 * 64 bytes: three matrix rows with a scalar riding in each w, plus one vec4.
 * Deliberately not push.glsl's block -- see output_encode.vert.
 *
 * ROW-major, and taken as three vec3s rather than a mat3, because GLSL's mat3
 * constructor is COLUMN-major: nine row-major floats pushed from the CPU and
 * read as a mat3 are transposed, and a transposed 709->2020 matrix still
 * renders a plausible picture.
 */
layout(push_constant) uniform Encode {
	vec4 row0; /* xyz: matrix row 0   w: knee, in scene units (ADR-009)     */
	vec4 row1; /* xyz: matrix row 1   w: peak_scene, the output ceiling     */
	vec4 row2; /* xyz: matrix row 2   w: ref_nits / 10000, the PQ anchor    */
	vec4 misc; /* x: dither quantum, peak-to-peak, in electrical units;
	            * yz: this target's origin in output pixels, so the dither
	            *     pattern stays anchored to the OUTPUT raster and cannot
	            *     phase-shift; w: unused                                */
} epc;

#define AZ_ENC_KNEE epc.row0.w
#define AZ_ENC_PEAK epc.row1.w
#define AZ_ENC_ANCHOR epc.row2.w
#define AZ_ENC_DITHER epc.misc.x
#define AZ_ENC_ORIGIN epc.misc.yz

layout(set = 0, binding = 0) uniform sampler2D scene;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

void main() {
	/* 1. the composited scene value. Alpha is dead here. */
	vec3 v = texture(scene, v_uv).rgb;

	/* 2. tone map. Identity below the knee and identity for peak <= 1, so an
	 *    SDR-only frame on an SDR output passes through untouched. */
	v = az_tonemap(v, AZ_ENC_KNEE, AZ_ENC_PEAK);

	/* 3. gamut matrix, then clamp negatives (ADR-010). The clamp is the last
	 *    resort: the tone map has already bounded the max channel, so what is
	 *    left is out-of-container chroma, not luminance structure. */
	v = az_mat_mul(epc.row0.xyz, epc.row1.xyz, epc.row2.xyz, v);
	v = max(v, vec3(0.0));

	/* 4. luminance anchor, PQ only. There is nothing to anchor for an sRGB
	 *    output: scene 1.0 IS electrical 1.0 there, which is exactly what
	 *    makes C4's round-trip gate an exact identity. */
	if (AZ_ENCODE_TF == AZ_ENCODE_TF_PQ) {
		v *= AZ_ENC_ANCHOR;
	}
	v = clamp(v, vec3(0.0), vec3(1.0));

	/* 5. encode. The one PQ inverse EOTF in the tree. */
	vec3 e = AZ_ENCODE_TF == AZ_ENCODE_TF_PQ ? az_pq_ieotf(v)
											 : az_srgb_ieotf(v);

	/* 6. dither. On the ELECTRICAL value, at the target's quantum, RGB only.
	 *    Dithering the scene instead would be an amplitude correct at one grey
	 *    and wrong at every other -- one output code is not a constant amount
	 *    of scene under a non-linear encode. C4 gate 8 measures exactly that:
	 *    the scene-domain version keeps the mean nearly right while spraying
	 *    three codes at PQ black.
	 *
	 *    Anchored to the output raster via AZ_ENC_ORIGIN, not to this
	 *    attachment, so a regional target cannot phase-shift the pattern at
	 *    its own edge. No taper and no gradient term here, unlike
	 *    az_dither_alpha(): this value is a composed colour with nothing
	 *    special about its ends, and fwidth() on it would be a derivative of
	 *    a texture fetch. */
	e += (az_ign(gl_FragCoord.xy + AZ_ENC_ORIGIN) - 0.5) * AZ_ENC_DITHER;

	/* 7. write. Alpha 1.0: every scanout format on this path is an X format,
	 *    and alpha is never dithered and never transformed. */
	out_color = vec4(clamp(e, vec3(0.0), vec3(1.0)), 1.0);
}
