/*
 * az_color -- implementation of the M5 colour primitives. Contract C1.
 *
 * Every constant here is the PUBLISHED one, transcribed from the standard, not
 * copied out of a shader in this tree. That is deliberate: the unit test pins
 * these functions against published anchor values (BT.2408's 203-nit PQ code,
 * IEC 61966-2-1's midpoint), so if the library and the shader ever disagree,
 * the test says which of the two is wrong instead of certifying whichever one
 * they both are.
 *
 * Float, not double. The GLSL twin runs in 32-bit float and the point of the
 * parity test is that the two agree; computing the C side in double would make
 * the reference more accurate than the thing it is the reference FOR, and the
 * parity bound would have to absorb the difference. Where more precision is
 * genuinely wanted -- the CPU pipeline reference, C4 -- the reference promotes
 * to double around these calls rather than changing them.
 */

#include "render/color/az_color.h"

#include <math.h>

/* Every decode clamps. See the header: this is the PQ-NaN rule, and it is here
 * rather than at the call sites because there is exactly one call site that can
 * forget it and it is all of them. */
static inline float clamp01(float v) {
	return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

/* ── sRGB, IEC 61966-2-1 ────────────────────────────────────────────────── */

float az_srgb_eotf(float e) {
	e = clamp01(e);
	if (e <= AZ_SRGB_E_THRESHOLD) {
		return e / 12.92f;
	}
	return powf((e + 0.055f) / 1.055f, 2.4f);
}

float az_srgb_ieotf(float o) {
	o = clamp01(o);
	if (o <= AZ_SRGB_O_THRESHOLD) {
		return o * 12.92f;
	}
	return 1.055f * powf(o, 1.0f / 2.4f) - 0.055f;
}

/* ── pure 2.2 power ─────────────────────────────────────────────────────── */

float az_gamma22_eotf(float e) { return powf(clamp01(e), 2.2f); }

float az_gamma22_ieotf(float o) { return powf(clamp01(o), 1.0f / 2.2f); }

/* ── ITU-R BT.1886 ──────────────────────────────────────────────────────── */

/*
 * L = a(V + b)^2.4 with a and b derived from the black and white levels, then
 * normalised to [0, 1] by (L - Lmin)/(Lmax - Lmin).
 *
 * The normalisation is what makes electrical 0 land on optical 0. Without it
 * the curve's floor is Lmin/Lmax = 1e-4 and every black in the compositor
 * would sit a hundredth of a percent above zero -- invisible, but it would
 * break the round-trip identity at 0 and therefore the C4 gate. fx_vk
 * normalises the same way (texture_mask_round.frag:88-97), which matters only
 * because a BT1886-tagged surface must not change appearance when its decode
 * moves from there to here.
 */
static void bt1886_coeffs(float *a, float *b) {
	float lb = powf(AZ_BT1886_LMIN, 1.0f / 2.4f);
	float lw = powf(AZ_BT1886_LMAX, 1.0f / 2.4f);
	*a = powf(lw - lb, 2.4f);
	*b = lb / (lw - lb);
}

float az_bt1886_eotf(float e) {
	float a, b;
	bt1886_coeffs(&a, &b);
	float L = a * powf(clamp01(e) + b, 2.4f);
	return (L - AZ_BT1886_LMIN) / (AZ_BT1886_LMAX - AZ_BT1886_LMIN);
}

float az_bt1886_ieotf(float o) {
	float a, b;
	bt1886_coeffs(&a, &b);
	float L = clamp01(o) * (AZ_BT1886_LMAX - AZ_BT1886_LMIN) + AZ_BT1886_LMIN;
	return powf(L / a, 1.0f / 2.4f) - b;
}

/* ── SMPTE ST 2084 (PQ) ─────────────────────────────────────────────────── */

/*
 * eotf: Y = ((max(E'^(1/m2) - c1, 0)) / (c2 - c3 E'^(1/m2)))^(1/m1)
 * ieotf: E' = ((c1 + c2 Y^m1) / (1 + c3 Y^m1))^m2
 *
 * The max() in the decode's numerator is not decoration: below E' ~= 0.0151
 * the numerator is negative, and a negative base with the fractional exponent
 * 1/m1 = 6.277 is undefined -- NaN on RADV, which is how a single translucent
 * texel takes out a whole quad. The denominator cannot go negative once the
 * input is clamped to [0, 1] (it would need E'^(1/m2) > c2/c3 = 1.0088), which
 * is the other half of the same rule.
 */
float az_pq_eotf(float e) {
	float ep = powf(clamp01(e), 1.0f / AZ_PQ_M2);
	float num = ep - AZ_PQ_C1;
	if (num < 0.0f) {
		num = 0.0f;
	}
	float den = AZ_PQ_C2 - AZ_PQ_C3 * ep;
	return powf(num / den, 1.0f / AZ_PQ_M1);
}

float az_pq_ieotf(float o) {
	float yn = powf(clamp01(o), AZ_PQ_M1);
	return powf((AZ_PQ_C1 + AZ_PQ_C2 * yn) / (1.0f + AZ_PQ_C3 * yn), AZ_PQ_M2);
}

/* ── ARIB STD-B67 / BT.2100 HLG, decode only ────────────────────────────── */

float az_hlg_eotf(float e) {
	e = clamp01(e);
	if (e <= 0.5f) {
		return e * e / 3.0f;
	}
	return (expf((e - AZ_HLG_C) / AZ_HLG_A) + AZ_HLG_B) / 12.0f;
}

/* ── primaries and matrices ─────────────────────────────────────────────── */

/* clang-format off */
const struct az_primaries_xy AZ_PRIMARIES_BT709 = {
	.red = { 0.640f, 0.330f },
	.green = { 0.300f, 0.600f },
	.blue = { 0.150f, 0.060f },
	.white = { 0.3127f, 0.3290f },
};
/* clang-format on */

/* clang-format off */
const struct az_primaries_xy AZ_PRIMARIES_BT2020 = {
	.red = { 0.708f, 0.292f },
	.green = { 0.170f, 0.797f },
	.blue = { 0.131f, 0.046f },
	.white = { 0.3127f, 0.3290f },
};
/* clang-format on */

/* clang-format off */
const float AZ_MAT_IDENTITY[9] = {
	1.0f, 0.0f, 0.0f,
	0.0f, 1.0f, 0.0f,
	0.0f, 0.0f, 1.0f,
};
/* clang-format on */

/*
 * These are what az_mat_from_primaries() produces for the two sets above,
 * carried to more digits than the literature usually prints. The unit test
 * checks them BOTH ways: against the derivation (1e-6, so the table cannot
 * drift from the code) and against the published rounded BT.2087 values
 * (1e-5, so the derivation cannot drift from the standard). Either check
 * alone would be satisfiable by a wrong-but-consistent pair.
 */
/* clang-format off */
const float AZ_MAT_709_TO_2020[9] = {
	0.62740390f, 0.32928304f, 0.04331307f,
	0.06909729f, 0.91954040f, 0.01136232f,
	0.01639144f, 0.08801331f, 0.89559525f,
};
/* clang-format on */

/* clang-format off */
const float AZ_MAT_2020_TO_709[9] = {
	 1.66049100f, -0.58764114f, -0.07284986f,
	-0.12455047f,  1.13289990f, -0.00834942f,
	-0.01815076f, -0.10057890f,  1.11872966f,
};
/* clang-format on */

void az_mat_mul_vec3(const float m[9], const float v[3], float out[3]) {
	float r[3];
	for (int row = 0; row < 3; row++) {
		r[row] = m[row * 3 + 0] * v[0] + m[row * 3 + 1] * v[1] +
				 m[row * 3 + 2] * v[2];
	}
	out[0] = r[0];
	out[1] = r[1];
	out[2] = r[2];
}

void az_mat_mul(const float a[9], const float b[9], float out[9]) {
	float r[9];
	for (int row = 0; row < 3; row++) {
		for (int col = 0; col < 3; col++) {
			float v = 0.0f;
			for (int k = 0; k < 3; k++) {
				v += a[row * 3 + k] * b[k * 3 + col];
			}
			r[row * 3 + col] = v;
		}
	}
	for (int i = 0; i < 9; i++) {
		out[i] = r[i];
	}
}

static bool mat3_inverse(const float m[9], float out[9]) {
	float c00 = m[4] * m[8] - m[5] * m[7];
	float c01 = m[5] * m[6] - m[3] * m[8];
	float c02 = m[3] * m[7] - m[4] * m[6];
	float det = m[0] * c00 + m[1] * c01 + m[2] * c02;
	if (det == 0.0f) {
		return false;
	}
	float inv = 1.0f / det;
	/* adjugate = cofactor transposed */
	out[0] = c00 * inv;
	out[1] = (m[2] * m[7] - m[1] * m[8]) * inv;
	out[2] = (m[1] * m[5] - m[2] * m[4]) * inv;
	out[3] = c01 * inv;
	out[4] = (m[0] * m[8] - m[2] * m[6]) * inv;
	out[5] = (m[2] * m[3] - m[0] * m[5]) * inv;
	out[6] = c02 * inv;
	out[7] = (m[1] * m[6] - m[0] * m[7]) * inv;
	out[8] = (m[0] * m[4] - m[1] * m[3]) * inv;
	return true;
}

/*
 * RGB -> XYZ for one primaries set, the standard NPM construction:
 *
 *   each primary's chromaticity (x, y) gives the direction (x/y, 1, z/y);
 *   the three directions form M; the per-primary scale factors S solve
 *   M S = W, the white point at Y = 1; the answer is M diag(S).
 *
 * Y = 1 for the white point rather than 100: the working space is relative
 * (ADR-003) and the absolute anchor lives in the luminance domain, not in a
 * matrix.
 */
static void rgb_to_xyz(const struct az_primaries_xy *p, float out[9]) {
	const struct az_cie_xy *c[3] = {&p->red, &p->green, &p->blue};
	float m[9];
	for (int i = 0; i < 3; i++) {
		m[0 * 3 + i] = c[i]->x / c[i]->y;
		m[1 * 3 + i] = 1.0f;
		m[2 * 3 + i] = (1.0f - c[i]->x - c[i]->y) / c[i]->y;
	}
	float w[3] = {
		p->white.x / p->white.y,
		1.0f,
		(1.0f - p->white.x - p->white.y) / p->white.y,
	};
	float minv[9];
	if (!mat3_inverse(m, minv)) {
		for (int i = 0; i < 9; i++) {
			out[i] = AZ_MAT_IDENTITY[i];
		}
		return;
	}
	float s[3];
	az_mat_mul_vec3(minv, w, s);
	for (int row = 0; row < 3; row++) {
		for (int col = 0; col < 3; col++) {
			out[row * 3 + col] = m[row * 3 + col] * s[col];
		}
	}
}

void az_mat_from_primaries(const struct az_primaries_xy *src,
						   const struct az_primaries_xy *dst, float out[9]) {
	float src_to_xyz[9], dst_to_xyz[9], xyz_to_dst[9];
	rgb_to_xyz(src, src_to_xyz);
	rgb_to_xyz(dst, dst_to_xyz);
	if (!mat3_inverse(dst_to_xyz, xyz_to_dst)) {
		for (int i = 0; i < 9; i++) {
			out[i] = AZ_MAT_IDENTITY[i];
		}
		return;
	}
	az_mat_mul(xyz_to_dst, src_to_xyz, out);
}

void az_mat_saturation(float s, float out[9]) {
	/* Rec.709 luma weights in linear light, exactly scenefx's composition
	 * (wlr_scene.c:4104-4126): out = luma[col](1 - s) + s on the diagonal. */
	static const float luma[3] = {0.2126f, 0.7152f, 0.0722f};
	for (int row = 0; row < 3; row++) {
		for (int col = 0; col < 3; col++) {
			out[row * 3 + col] =
				luma[col] * (1.0f - s) + (row == col ? s : 0.0f);
		}
	}
}

/* ── tone mapping (ADR-009) ─────────────────────────────────────────────── */

void az_tonemap(float v[3], float knee, float peak) {
	if (peak <= 1.0f) {
		return;
	}
	float m = v[0] > v[1] ? v[0] : v[1];
	if (v[2] > m) {
		m = v[2];
	}
	if (m <= knee) {
		return;
	}
	float x = m - knee;
	float P = peak - knee;
	float f = knee + P * x / (P + x);
	float scale = f / m;
	v[0] *= scale;
	v[1] *= scale;
	v[2] *= scale;
}

/* ── dither (ADR-011) ───────────────────────────────────────────────────── */

/* GLSL fract(), which is x - floor(x) and therefore non-negative even for
 * negative x -- C's fmodf() is not, and using it would put a sign flip in the
 * noise on any output whose origin is negative. */
static inline float fractf(float x) { return x - floorf(x); }

float az_ign(float x, float y) {
	return fractf(52.9829189f * fractf(x * 0.06711056f + y * 0.00583715f));
}
