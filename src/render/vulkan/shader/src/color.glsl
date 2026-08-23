/*
 * color.glsl -- the GLSL twin of src/render/color/az_color.c. Contract C1.
 *
 * HAND WRITTEN, NOT GENERATED, and that is a decision rather than laziness: a
 * generator would guarantee the two agree textually while hiding the thing
 * that actually differs between them -- GLSL's componentwise pow(), its
 * genType clamp, and the fact that a shader wants vec3 where the CPU wants
 * float. What keeps them honest instead is that both are pinned to the same
 * PUBLISHED anchors by tests/test-color-math.c, so a drift is a failing
 * assertion against ST 2084 and IEC 61966-2-1, not against each other.
 *
 * THE VOCABULARY IS THE ADR's (docs/decisions.md), used literally:
 *   scene value       premultiplied linear-light BT.709/D65, 1.0 = SDR
 *                     reference white; unbounded, components may be negative.
 *   electrical value  transfer-function-encoded, in [0, 1].
 *   eotf              electrical -> optical (DECODE).
 *   ieotf             optical -> electrical (ENCODE).
 *
 * TWO RULES THAT ARE NOT STYLE, identical to the C header's:
 *
 *  - NO FUNCTION HERE TAKES AN ALPHA. Every signature is vec3. Alpha is
 *    coverage; a transfer function applied to it is the bug ADR-005 exists to
 *    prevent, and a vec4 signature is how that bug gets written by accident.
 *
 *  - EVERY DECODE CLAMPS TO [0, 1] FIRST. Un-premultiplying a low-alpha texel
 *    divides its rounding error by alpha; a PQ value that lands above
 *    c2/c3 = 1.0088 flips the decode's denominator negative, and pow() with a
 *    negative base and a fractional exponent is undefined -- NaN on RADV,
 *    which takes out the whole quad, not one texel.
 *
 * WHERE PQ MAY APPEAR. az_pq_ieotf() is the OUTPUT ENCODE and may be called
 * from exactly one shader: output_encode.frag (ADR-008, invariant 1 -- PQ is
 * an output encoding, never an internal representation). tests/
 * test-color-pipeline.c greps for it and fails the suite if a second shader
 * grows one. Including this file does not itself count -- calling it does.
 */
#ifndef AZ_COLOR_GLSL
#define AZ_COLOR_GLSL

/* ── sRGB, IEC 61966-2-1 ────────────────────────────────────────────────── */

#define AZ_SRGB_E_THRESHOLD 0.04045
#define AZ_SRGB_O_THRESHOLD (0.04045 / 12.92)

vec3 az_srgb_eotf(vec3 e) {
	e = clamp(e, vec3(0.0), vec3(1.0));
	/* mix() rather than a branch: the two segments are both cheap, the
	 * selector is per-component, and a per-component branch in a shader is a
	 * per-component branch in a quad. */
	return mix(pow((e + 0.055) / 1.055, vec3(2.4)), e / 12.92,
		lessThanEqual(e, vec3(AZ_SRGB_E_THRESHOLD)));
}

vec3 az_srgb_ieotf(vec3 o) {
	o = clamp(o, vec3(0.0), vec3(1.0));
	return mix(1.055 * pow(o, vec3(1.0 / 2.4)) - 0.055, o * 12.92,
		lessThanEqual(o, vec3(AZ_SRGB_O_THRESHOLD)));
}

/* ── pure 2.2 power ─────────────────────────────────────────────────────── */

vec3 az_gamma22_eotf(vec3 e) {
	return pow(clamp(e, vec3(0.0), vec3(1.0)), vec3(2.2));
}

vec3 az_gamma22_ieotf(vec3 o) {
	return pow(clamp(o, vec3(0.0), vec3(1.0)), vec3(1.0 / 2.2));
}

/* ── ITU-R BT.1886 ──────────────────────────────────────────────────────── */

#define AZ_BT1886_LMIN 0.01
#define AZ_BT1886_LMAX 100.0

vec3 az_bt1886_eotf(vec3 e) {
	float lb = pow(AZ_BT1886_LMIN, 1.0 / 2.4);
	float lw = pow(AZ_BT1886_LMAX, 1.0 / 2.4);
	float a = pow(lw - lb, 2.4);
	float b = lb / (lw - lb);
	vec3 L = a * pow(clamp(e, vec3(0.0), vec3(1.0)) + vec3(b), vec3(2.4));
	return (L - AZ_BT1886_LMIN) / (AZ_BT1886_LMAX - AZ_BT1886_LMIN);
}

vec3 az_bt1886_ieotf(vec3 o) {
	float lb = pow(AZ_BT1886_LMIN, 1.0 / 2.4);
	float lw = pow(AZ_BT1886_LMAX, 1.0 / 2.4);
	float a = pow(lw - lb, 2.4);
	float b = lb / (lw - lb);
	vec3 L = clamp(o, vec3(0.0), vec3(1.0)) *
		(AZ_BT1886_LMAX - AZ_BT1886_LMIN) + vec3(AZ_BT1886_LMIN);
	return pow(L / a, vec3(1.0 / 2.4)) - vec3(b);
}

/* ── SMPTE ST 2084 (PQ) ─────────────────────────────────────────────────── */

#define AZ_PQ_M1 0.1593017578125
#define AZ_PQ_M2 78.84375
#define AZ_PQ_C1 0.8359375
#define AZ_PQ_C2 18.8515625
#define AZ_PQ_C3 18.6875

/* Optical 1.0 is 10000 cd/m2, NOT the display peak and NOT SDR white. The
 * conversion to scene units is the luminance domain's single multiply
 * (ADR-003/006), and it is deliberately not folded in here -- folding it in is
 * how a constant ends up on the wrong side of the gamut matrix. */
vec3 az_pq_eotf(vec3 e) {
	vec3 ep = pow(clamp(e, vec3(0.0), vec3(1.0)), vec3(1.0 / AZ_PQ_M2));
	vec3 num = max(ep - AZ_PQ_C1, vec3(0.0));
	vec3 den = AZ_PQ_C2 - AZ_PQ_C3 * ep;
	return pow(num / den, vec3(1.0 / AZ_PQ_M1));
}

vec3 az_pq_ieotf(vec3 o) {
	vec3 yn = pow(clamp(o, vec3(0.0), vec3(1.0)), vec3(AZ_PQ_M1));
	return pow((AZ_PQ_C1 + AZ_PQ_C2 * yn) / (1.0 + AZ_PQ_C3 * yn),
		vec3(AZ_PQ_M2));
}

/* ── ARIB STD-B67 / BT.2100 HLG, decode only (ADR-000 scope) ────────────── */

#define AZ_HLG_A 0.17883277
#define AZ_HLG_B 0.28466892
#define AZ_HLG_C 0.55991073

vec3 az_hlg_eotf(vec3 e) {
	e = clamp(e, vec3(0.0), vec3(1.0));
	return mix((exp((e - AZ_HLG_C) / AZ_HLG_A) + AZ_HLG_B) / 12.0,
		e * e / 3.0, lessThanEqual(e, vec3(0.5)));
}

/* ── tone mapping (ADR-009) ─────────────────────────────────────────────── */

/*
 * ONE common scale on all three channels. The ratios r:g:b survive exactly,
 * which is the whole of "hue preserving" here; a per-channel curve walks a
 * saturated highlight toward white, which is the artefact this replaces.
 *
 * Branchless on purpose: `peak` and `knee` are push constants and therefore
 * uniform, but `m` is not, and a per-fragment branch on it would diverge
 * inside a quad along every highlight edge -- exactly where the curve is doing
 * work.
 */
vec3 az_tonemap(vec3 v, float knee, float peak) {
	float m = max(max(v.r, v.g), v.b);
	float x = max(m - knee, 0.0);
	float P = max(peak - knee, 0.0);
	/* f = knee + P x / (P + x); asymptote at peak, f(knee) = knee and
	 * f'(knee) = 1, so the join is C1. The +1e-6 guards P == 0 (peak <= knee),
	 * which the step() below discards anyway. */
	float f = knee + P * x / (P + x + 1e-6);
	/* step(a, b) is b >= a. peak > 1 AND m > knee, spelled as the complement
	 * of each <= so the boundary cases land on "identity", which is the side
	 * the ADR specifies. */
	float engaged = (1.0 - step(peak, 1.0)) * (1.0 - step(m, knee));
	return v * mix(1.0, f / max(m, 1e-6), engaged);
}

/* ── gamut (ADR-010) ────────────────────────────────────────────────────── */

/*
 * ROW-MAJOR, as everywhere else in this project and in every printed colour
 * standard. GLSL's mat3 constructor is COLUMN-major, so a matrix pushed from
 * the CPU as nine row-major floats and read here as a mat3 is transposed --
 * and a transposed 709<->2020 matrix still renders a plausible picture, which
 * is why this takes the rows explicitly instead of a mat3.
 */
vec3 az_mat_mul(vec3 r0, vec3 r1, vec3 r2, vec3 v) {
	return vec3(dot(r0, v), dot(r1, v), dot(r2, v));
}

/*
 * ── THE CEILING CURVE (C7 / F5), AND WHY IT IS NOT az_tonemap ─────────────
 *
 * az_tonemap() is the identity for peak <= 1 -- ADR-009 says so and C1's unit
 * test asserts it over 101 samples, because "everything at or below SDR white
 * is untouched on every output" is the guarantee that keeps ordinary SDR
 * content bit-exact. Calling it with peak = 1.0 therefore does NOTHING no
 * matter what knee it is given.
 *
 * That is exactly the conflict F5 recorded: ADR-007 promised that a >1-capable
 * source on an 8-bit output would roll off so "the attachment never clamps",
 * while ADR-009's guard makes the only available curve an identity there. The
 * sentence was struck and replaced with option 2 -- a knee BELOW 1.0, in the
 * >1-capable variant only.
 *
 * So this is the same algebra with the peak pinned at 1.0 and the peak guard
 * removed, and it is a SEPARATE FUNCTION rather than a relaxation of
 * az_tonemap so that ADR-009's invariant stays literally true for everything
 * that uses it. It is reachable only from the decode variant of a domain whose
 * scale exceeds 1.0; ordinary SDR content never sees it.
 *
 *     f(m) = knee + P*x/(P + x),  P = 1 - knee,  x = m - knee
 *
 * C1 at the knee (f(knee) = knee, f'(knee) = 1) and asymptotic to 1.0, so
 * nothing above the knee ever reaches the clamp and nothing below it moves.
 */
vec3 az_rolloff_ceiling(vec3 v, float knee) {
	float m = max(max(v.r, v.g), v.b);
	float x = max(m - knee, 0.0);
	float P = max(1.0 - knee, 0.0);
	float f = knee + P * x / (P + x + 1e-6);
	/* Engaged only above the knee. One common scale on all three channels, so
	 * the ratios -- and therefore the hue -- survive exactly. */
	float engaged = 1.0 - step(m, knee);
	return v * mix(1.0, f / max(m, 1e-6), engaged);
}

/*
 * ── BT.2020 -> BT.709, AS A CONSTANT (C7) ─────────────────────────────────
 *
 * The SOURCE side of the gamut question, and the only conversion that exists
 * on it: the scene is BT.709 (ADR-002), and the only other primaries a client
 * can declare through wp-color-management that this renderer will meet is
 * BT.2020. So this is a fixed matrix rather than a push constant -- which is
 * what lets the whole conversion be a uniform branch instead of nine floats
 * that would not fit in push.glsl's 128 bytes anyway.
 *
 * THESE NINE NUMBERS ARE DUPLICATED FROM az_color.c's AZ_MAT_2020_TO_709 and
 * that duplication is pinned: tests/test-color-pipeline.c parses both and
 * fails if they drift. The alternative -- pushing the matrix per draw -- costs
 * a push-constant budget this block does not have, for a value that is one of
 * exactly two possibilities.
 *
 * Rows, not a mat3, for the reason az_mat_mul() states: GLSL's mat3
 * constructor is column-major and a transposed gamut matrix still renders a
 * plausible picture.
 */
const vec3 AZ_2020_TO_709_R0 = vec3( 1.66049100, -0.58764114, -0.07284986);
const vec3 AZ_2020_TO_709_R1 = vec3(-0.12455047,  1.13289990, -0.00834942);
const vec3 AZ_2020_TO_709_R2 = vec3(-0.01815076, -0.10057890,  1.11872966);

/*
 * Convert a linear-light triple from BT.2020 into the scene's BT.709.
 *
 * NEGATIVES ARE KEPT. A BT.709 container cannot hold every BT.2020 colour, so
 * out-of-gamut inputs come out with a negative component -- and that is the
 * honest scene value, not an error. ADR-010 clamps at the OUTPUT, once, after
 * the tone map has bounded the max channel; clamping here instead would throw
 * away chroma that the output's own primaries may well be able to show, and on
 * a BT.2020 output it would clamp a colour that never needed clamping at all.
 */
vec3 az_2020_to_709(vec3 v) {
	return az_mat_mul(AZ_2020_TO_709_R0, AZ_2020_TO_709_R1,
		AZ_2020_TO_709_R2, v);
}

/* ── dither (ADR-011) ───────────────────────────────────────────────────── */

/* Interleaved gradient noise, [0, 1). Identical arithmetic to
 * az_dither_noise() in dither.glsl and to az_ign() in az_color.c -- three
 * copies of five operations, pinned to each other by test-color-math.c,
 * because the alternative is dither.glsl including this file and every blur
 * shader growing a PQ encode it must not have. */
float az_ign(vec2 p) {
	return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

#endif /* AZ_COLOR_GLSL */
