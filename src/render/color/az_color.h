/*
 * az_color -- the colour primitives every M5 contract is built from.
 *
 * CONTRACT C1. Pure functions, no state, no I/O, no Vulkan and no wlroots
 * types. Compiled into asteroidz AND linked standalone into the unit tests,
 * which is why it lives in its own directory with no dependency beyond libm.
 *
 * THE VOCABULARY IS THE ADR's (docs/m5-hdr/adr.md) and is used literally:
 *
 *   scene value       premultiplied linear-light BT.709/D65, 1.0 == SDR
 *                     reference white. Unbounded above; components may be
 *                     NEGATIVE (ADR-002, scRGB convention).
 *   electrical value  transfer-function-encoded, in [0, 1].
 *   EOTF              electrical -> optical (DECODE).
 *   inverse EOTF      optical -> electrical (ENCODE).
 *
 * There is no "OETF" in this project: every encode here is the inverse of a
 * display EOTF, and calling it an OETF would import the camera-side
 * convention along with its OOTF.
 *
 * TWO RULES THAT ARE NOT STYLE:
 *
 *  - ALPHA NEVER APPEARS. No function in this header takes or returns an
 *    alpha channel. Alpha is coverage; putting it through a transfer function
 *    is the bug ADR-005 exists to prevent, and the way to make that
 *    unrepresentable is to give the library no way to express it.
 *
 *  - EVERY DECODE CLAMPS ITS INPUT TO [0, 1] FIRST. This is the PQ-NaN rule
 *    (ADR-005): un-premultiplying a low-alpha texel amplifies its rounding
 *    error by 1/a, and a PQ value above c2/c3 ~= 1.0088 makes the
 *    denominator negative, so the final pow() has a negative base and a
 *    fractional exponent -- undefined, NaN on RADV. The clamp is inside each
 *    function rather than at each call site, because there is exactly one
 *    place a caller can forget it and it is every call site.
 *
 * The GLSL twin is src/render/vulkan/shader/src/color.glsl. It is hand
 * written, not generated; tests/test-color-math.c pins both against published
 * constants so drift becomes a test failure rather than a colour shift.
 */
#ifndef AZ_COLOR_H
#define AZ_COLOR_H

#include <stdbool.h>

/* ── transfer functions ─────────────────────────────────────────────────── */

/* IEC 61966-2-1 sRGB, the PIECEWISE curve -- not a 2.2 power. ADR-004 chose
 * it for untagged surfaces because it is what Vulkan `_SRGB` image views
 * implement in hardware on both the sampling and the attachment side, which is
 * what makes Path A's exact round-trip gate possible at all.
 *
 * The two thresholds are exact reciprocals of each other on purpose: the
 * published encode threshold is the rounded decimal 0.0031308, and using it
 * would put a ~1e-5 step at the join, so `ieotf(eotf(x)) == x` would fail its
 * own round-trip test near black for no reason but a rounded constant. */
#define AZ_SRGB_E_THRESHOLD 0.04045f			/* electrical side */
#define AZ_SRGB_O_THRESHOLD (0.04045f / 12.92f) /* optical side, exact */
float az_srgb_eotf(float e);
float az_srgb_ieotf(float o);

/* Pure power 2.2. What wlroots/frog map an sRGB-tagged surface to (audit
 * §3.7) and what an explicitly GAMMA22-tagged surface declares. */
float az_gamma22_eotf(float e);
float az_gamma22_ieotf(float o);

/* ITU-R BT.1886 with Lmin 0.01 cd/m2, Lmax 100 cd/m2, normalised so that
 * electrical 1.0 -> optical 1.0. The normalisation ((L - Lmin)/(Lmax - Lmin))
 * matches fx_vk's (scenefx texture_mask_round.frag:88-97 and
 * output.frag:52-61); it is the reason electrical 0 maps to optical 0 rather
 * than to the 0.01 cd/m2 black floor. */
#define AZ_BT1886_LMIN 0.01f
#define AZ_BT1886_LMAX 100.0f
float az_bt1886_eotf(float e);
float az_bt1886_ieotf(float o);

/* SMPTE ST 2084 (PQ), H.273 transfer characteristic 16.
 *
 * OPTICAL 1.0 IS 10000 cd/m2, not the display's peak and not SDR white. That
 * is the whole reason ADR-003's scale factor exists: scene units are relative
 * to SDR reference white, PQ is absolute, and the conversion between them is
 * the one multiply the luminance domain carries. */
#define AZ_PQ_M1 0.1593017578125f
#define AZ_PQ_M2 78.84375f
#define AZ_PQ_C1 0.8359375f
#define AZ_PQ_C2 18.8515625f
#define AZ_PQ_C3 18.6875f
float az_pq_eotf(float e);
float az_pq_ieotf(float o);

/* ARIB STD-B67 / ITU-R BT.2100 HLG, DECODE ONLY (ADR-000 scope).
 *
 * This is the inverse OETF alone: it returns scene-referred relative light
 * with E' = 1 -> 1.0, and does NOT apply the OOTF, which is display-peak
 * dependent and has no consumer in M5. It exists so the library's test
 * surface is complete and so that adding HLG later is one enum value plus a
 * scale, not a new curve nobody has ever run. */
#define AZ_HLG_A 0.17883277f
#define AZ_HLG_B 0.28466892f /* 1 - 4a */
#define AZ_HLG_C 0.55991073f /* 0.5 - a*ln(4a) */
float az_hlg_eotf(float e);

/* ── primaries and matrices ─────────────────────────────────────────────── */

/* Row-major 3x3: out[row * 3 + col], and y = M * x. Row-major because every
 * printed matrix in every colour standard is row-major, and a transposed
 * constant table is a bug that renders a plausible picture. */
struct az_cie_xy {
	float x, y;
};
struct az_primaries_xy {
	struct az_cie_xy red, green, blue, white;
};

extern const struct az_primaries_xy AZ_PRIMARIES_BT709;
extern const struct az_primaries_xy AZ_PRIMARIES_BT2020;

/* ABSOLUTE COLORIMETRIC conversion via XYZ. No chromatic adaptation is
 * applied: every primaries set M5 handles is D65, so a Bradford transform
 * would be the identity dressed up as diligence. If a non-D65 set is ever
 * added this function must grow the adaptation, and the assert-by-test is
 * that src and dst white points agree. */
void az_mat_from_primaries(const struct az_primaries_xy *src,
						   const struct az_primaries_xy *dst, float out[9]);

/* Precomputed, and checked against az_mat_from_primaries by the unit test to
 * within 1e-6 -- so these can be read at a glance and still cannot drift. */
extern const float AZ_MAT_709_TO_2020[9];
extern const float AZ_MAT_2020_TO_709[9];
extern const float AZ_MAT_IDENTITY[9];

void az_mat_mul_vec3(const float m[9], const float v[3], float out[3]);
void az_mat_mul(const float a[9], const float b[9], float out[9]);

/* The saturation matrix scenefx composes for `sdr_saturation`
 * (wlr_scene.c:4104-4126), reimplemented here so C3 can build an output
 * matrix without linking scenefx. Rec.709 luma weights; s == 1 is exactly the
 * identity. */
void az_mat_saturation(float s, float out[9]);

/* ── tone mapping (ADR-009) ─────────────────────────────────────────────── */

/*
 * Static, hue-preserving, max-channel-driven Reinhard with an identity
 * segment below the knee.
 *
 *     m = max(r, g, b)
 *     m <= knee  or  peak <= 1        ->  identity
 *     otherwise   x = m - knee, P = peak - knee
 *                 f(m) = knee + P*x/(P + x)
 *                 v   *= f(m)/m
 *
 * f(knee) = knee and f'(knee) = 1, so the curve is C1 at the join; f is
 * strictly increasing and strictly below `peak` for every finite input, so
 * the ceiling is an asymptote and never an actual clip. ONE common scale on
 * all three channels: the channel RATIOS are preserved exactly, which is what
 * "hue preserving" means here and what per-channel clipping destroys by
 * walking a saturated highlight toward white.
 *
 * DEVIATION FROM ADR-009's PROSE, recorded rather than silently taken: the
 * ADR sketches `f(m) = m(1 + m/peak^2)/(1 + m)` "rescaled to be C1 at the
 * knee" and then delegates the exact algebra here. That sketch is the
 * extended-Reinhard form whose `peak` is a CONTENT white point mapped to
 * output 1.0, which is not what ADR-003 defines `peak` to be (an output
 * ceiling in scene units). Taking it literally would make f(peak) = 1, i.e.
 * a 1000-nit panel would render its own ceiling at SDR white. The asymptotic
 * form above is the one that satisfies every property the ADR and contract
 * actually assert. See docs/m5-hdr/opus-findings.md.
 */
void az_tonemap(float v[3], float knee, float peak);

/* ── dither (ADR-011) ───────────────────────────────────────────────────── */

/* Interleaved gradient noise (Jimenez 2014), returns [0, 1). Identical
 * arithmetic to az_dither_noise() in shader/src/dither.glsl, in the same
 * order and in 32-bit float, so the CPU reference and the shader agree on
 * which pixels moved which way. */
float az_ign(float x, float y);

#endif /* AZ_COLOR_H */
