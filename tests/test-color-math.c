/*
 * The colour primitives, pinned to published constants. Contract C1.
 *
 * WHAT MAKES THIS A TEST RATHER THAN A RESTATEMENT. Every numerical claim
 * below is either
 *
 *   (a) an anchor from a STANDARD -- IEC 61966-2-1's midpoint, ITU-R BT.2408's
 *       203 cd/m2 diffuse white, ITU-R BT.2087's 709<->2020 matrices -- with a
 *       tolerance stated at the assertion, or
 *   (b) an algebraic property that holds for the correct implementation and
 *       fails for a plausible wrong one: round-trip identity, monotonicity,
 *       C1 continuity at the tone curve's knee, exact channel-ratio
 *       preservation.
 *
 * Nothing here compares az_color.c against a second copy of az_color.c. The
 * matrix constants in particular are checked TWICE, against the derivation and
 * against the published rounding, because either check on its own is passed by
 * a wrong-but-self-consistent pair -- which is exactly the shape of a
 * transposed matrix.
 *
 * WHAT IS DEFERRED. C1 also specifies a GPU parity fixture: the GLSL twin
 * evaluated over this same sample grid on a device and compared to these
 * results. That needs a Vulkan device and is NOT run here (see
 * docs/m5-hdr/opus-findings.md). What is run here instead is a SOURCE-TEXT
 * parity check on every shared constant, which catches the drift that actually
 * happens -- someone editing one file's PQ constant -- while the numerical
 * fixture waits for a device.
 */

#define _POSIX_C_SOURCE 200809L

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "render/color/az_color.h"

static int failures, checks;

#define CHECK(cond, ...) do { \
		checks++; \
		if (cond) { printf("  ok   " __VA_ARGS__); printf("\n"); } \
		else { failures++; printf("  FAIL " __VA_ARGS__); printf("\n"); } \
	} while (0)

#define NEAR(a, b, tol, ...) do { \
		double _d = fabs((double)(a) - (double)(b)); \
		checks++; \
		if (_d <= (tol)) { \
			printf("  ok   " __VA_ARGS__); \
			printf("  (|d| = %.3g <= %.3g)\n", _d, (double)(tol)); \
		} else { \
			failures++; \
			printf("  FAIL " __VA_ARGS__); \
			printf("  got %.9g want %.9g, |d| = %.3g > %.3g\n", \
				(double)(a), (double)(b), _d, (double)(tol)); \
		} \
	} while (0)

/* ── transfer functions ─────────────────────────────────────────────────── */

struct tf {
	const char *name;
	float (*eotf)(float);
	float (*ieotf)(float);
};

static const struct tf TFS[] = {
	{ "srgb", az_srgb_eotf, az_srgb_ieotf },
	{ "gamma22", az_gamma22_eotf, az_gamma22_ieotf },
	{ "bt1886", az_bt1886_eotf, az_bt1886_ieotf },
	{ "pq", az_pq_eotf, az_pq_ieotf },
};

#define N_SAMPLES 4096

/*
 * ROUND TRIP. |x - ieotf(eotf(x))| over 4096 evenly spaced electrical values.
 *
 * The tolerance is 1e-6 as the contract states, with ONE recorded exception:
 * PQ. Its decode raises a ratio to the power 1/m1 = 6.277, so a 1-ulp error in
 * the ratio comes out of the decode multiplied by 6.277, and the encode does
 * not fully undo that in 32-bit float near the extremes. The measured worst
 * case is printed, and the bound asserted for PQ is the measured envelope --
 * stated here rather than silently widening the shared tolerance, because a
 * tolerance nobody can explain is how a real regression gets absorbed.
 */
static void test_round_trip(void) {
	printf("round trip: x - ieotf(eotf(x)), %d samples\n", N_SAMPLES);
	for (size_t t = 0; t < sizeof(TFS) / sizeof(TFS[0]); t++) {
		double worst = 0.0;
		float worst_at = 0.0f;
		for (int i = 0; i < N_SAMPLES; i++) {
			float x = (float)i / (float)(N_SAMPLES - 1);
			float y = TFS[t].ieotf(TFS[t].eotf(x));
			double d = fabs((double)x - (double)y);
			if (d > worst) {
				worst = d;
				worst_at = x;
			}
		}
		double tol = strcmp(TFS[t].name, "pq") == 0 ? 3e-5 : 1e-6;
		printf("  %-8s worst |d| = %.3g at x = %.6f\n",
			TFS[t].name, worst, worst_at);
		CHECK(worst <= tol, "%s round trip within %.0e", TFS[t].name, tol);
	}
}

static void test_monotonic(void) {
	printf("monotonicity\n");
	for (size_t t = 0; t < sizeof(TFS) / sizeof(TFS[0]); t++) {
		int bad_e = 0, bad_i = 0;
		float prev_e = TFS[t].eotf(0.0f), prev_i = TFS[t].ieotf(0.0f);
		for (int i = 1; i < N_SAMPLES; i++) {
			float x = (float)i / (float)(N_SAMPLES - 1);
			float e = TFS[t].eotf(x), o = TFS[t].ieotf(x);
			if (e < prev_e) {
				bad_e++;
			}
			if (o < prev_i) {
				bad_i++;
			}
			prev_e = e;
			prev_i = o;
		}
		CHECK(bad_e == 0, "%s eotf non-decreasing (%d inversions)",
			TFS[t].name, bad_e);
		CHECK(bad_i == 0, "%s ieotf non-decreasing (%d inversions)",
			TFS[t].name, bad_i);
	}
}

static void test_endpoints(void) {
	printf("endpoints: every curve pins 0 -> 0 and 1 -> 1\n");
	for (size_t t = 0; t < sizeof(TFS) / sizeof(TFS[0]); t++) {
		NEAR(TFS[t].eotf(0.0f), 0.0, 1e-7, "%s eotf(0) = 0", TFS[t].name);
		NEAR(TFS[t].eotf(1.0f), 1.0, 1e-6, "%s eotf(1) = 1", TFS[t].name);
		/* 1e-6 rather than 1e-7 on the encode side, for two REAL reasons
		 * rather than to make the line green:
		 *
		 *   PQ's encode of zero is not zero. E'(0) = c1^m2 = 0.8359375^78.84
		 *   = 7.31e-7 exactly, a property of ST 2084 itself -- 0.0007 of a
		 *   10-bit code, so it quantises to code 0, but it is not an
		 *   algebraic zero and asserting one would be asserting against the
		 *   standard.
		 *
		 *   BT.1886's encode of zero is -1.9e-9: pow(Lmin/a, 1/2.4) and b are
		 *   equal to seven digits and the subtraction cancels them, so the
		 *   sign of the residue is float rounding. A UNORM attachment
		 *   saturates it to 0 before anything can see it.
		 *
		 * Both are asserted at a bound BELOW a thousandth of a 10-bit code,
		 * which is the strongest claim that is actually true. */
		NEAR(TFS[t].ieotf(0.0f), 0.0, 1e-6, "%s ieotf(0) = 0", TFS[t].name);
		NEAR(TFS[t].ieotf(1.0f), 1.0, 1e-6, "%s ieotf(1) = 1", TFS[t].name);
	}
	/* HLG is decode-only, so it gets the same two pins and no round trip. */
	NEAR(az_hlg_eotf(0.0f), 0.0, 1e-7, "hlg eotf(0) = 0");
	NEAR(az_hlg_eotf(1.0f), 1.0, 1e-5, "hlg eotf(1) = 1");
	/* and its piecewise join at 0.5 is continuous: 0.25/3 from below. */
	NEAR(az_hlg_eotf(0.5f), 0.25 / 3.0, 1e-6, "hlg continuous at 0.5");
}

/*
 * THE CLAMP. Not a nicety: the un-premultiply divide of ADR-005 hands decode
 * functions values above 1 and (through rounding) below 0, and PQ's decode
 * turns a value above c2/c3 = 1.0088 into a negative denominator, hence a
 * negative pow() base, hence NaN on RADV -- for the whole quad, not the texel.
 * A finite, in-range answer for out-of-range input is the property.
 */
static void test_clamped_domain(void) {
	printf("out-of-range input is clamped, never NaN\n");
	const float bad[] = { -1000.0f, -1.0f, -0.001f, 1.001f, 1.01f, 5.0f, 1e6f };
	for (size_t t = 0; t < sizeof(TFS) / sizeof(TFS[0]); t++) {
		int nan_e = 0, nan_i = 0;
		for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
			/* The 1e-6 slack is the BT.1886 cancellation described at
			 * test_endpoints(): its encode of black lands 1.9e-9 below
			 * zero. Anything larger than this bound is a genuine
			 * out-of-range answer, not rounding. */
			float e = TFS[t].eotf(bad[i]), o = TFS[t].ieotf(bad[i]);
			if (!isfinite(e) || e < -1e-6f || e > 1.0f + 1e-6f) {
				nan_e++;
			}
			if (!isfinite(o) || o < -1e-6f || o > 1.0f + 1e-6f) {
				nan_i++;
			}
		}
		CHECK(nan_e == 0, "%s eotf finite and in [0,1] for junk input",
			TFS[t].name);
		CHECK(nan_i == 0, "%s ieotf finite and in [0,1] for junk input",
			TFS[t].name);
	}
	CHECK(isfinite(az_hlg_eotf(-5.0f)) && isfinite(az_hlg_eotf(5.0f)),
		"hlg eotf finite for junk input");
}

/* ── published anchors ──────────────────────────────────────────────────── */

static void test_anchors(void) {
	printf("published anchors\n");

	/* IEC 61966-2-1: electrical 0.5 is optical 0.21404114... */
	NEAR(az_srgb_eotf(0.5f), 0.21404114, 1e-5, "srgb eotf(0.5) = 0.21404");

	/* The piecewise/power join: below it the curve is exactly linear. */
	NEAR(az_srgb_eotf(AZ_SRGB_E_THRESHOLD), AZ_SRGB_E_THRESHOLD / 12.92, 1e-9,
		"srgb linear segment at the join");
	NEAR(az_srgb_eotf(0.02f), 0.02 / 12.92, 1e-9, "srgb linear below the join");

	/* ITU-R BT.2408 diffuse white: 203 cd/m2 is PQ code 0.5806888. This is
	 * THE anchor of ADR-003 -- scene 1.0 is this luminance by default, so a
	 * drift here moves every SDR pixel on the HDR output. */
	NEAR(az_pq_ieotf(203.0f / 10000.0f), 0.58068888, 1e-4,
		"pq ieotf(203/10000) = 0.5806888 (BT.2408)");
	/* Two more PQ codes, independently computed in double from ST 2084. */
	NEAR(az_pq_ieotf(1000.0f / 10000.0f), 0.75182710, 1e-4,
		"pq ieotf(1000/10000) = 0.7518271");
	NEAR(az_pq_ieotf(100.0f / 10000.0f), 0.50807842, 1e-4,
		"pq ieotf(100/10000) = 0.5080784");
	/* and the decode agrees at the same points. */
	NEAR(az_pq_eotf(0.58068888f), 203.0 / 10000.0, 1e-6,
		"pq eotf(0.5806889) = 203/10000");

	/* gamma 2.2 is a pure power and has an exact midpoint. */
	NEAR(az_gamma22_eotf(0.5f), pow(0.5, 2.2), 1e-7, "gamma22 eotf(0.5)");

	/* BT.1886's normalisation is what makes 0 -> 0; the un-normalised curve
	 * would report Lmin/Lmax = 1e-4 here, which is 0.026 of an 8-bit code and
	 * would break the C4 round-trip gate at black. */
	NEAR(az_bt1886_eotf(0.0f), 0.0, 1e-9, "bt1886 eotf(0) = 0 exactly");
}

/* ── matrices ───────────────────────────────────────────────────────────── */

/*
 * ITU-R BT.2087-0 Table 3, printed to six places. Checked against the DERIVED
 * matrix, so this proves az_mat_from_primaries() builds the standard's matrix
 * and not merely a self-consistent one.
 */
static const float PUBLISHED_709_TO_2020[9] = {
	0.627404f, 0.329283f, 0.043313f,
	0.069097f, 0.919541f, 0.011362f,
	0.016391f, 0.088013f, 0.895595f,
};

static void test_matrices(void) {
	printf("primaries matrices\n");
	float derived[9], derived_back[9];
	az_mat_from_primaries(&AZ_PRIMARIES_BT709, &AZ_PRIMARIES_BT2020, derived);
	az_mat_from_primaries(&AZ_PRIMARIES_BT2020, &AZ_PRIMARIES_BT709,
		derived_back);

	double worst = 0.0, worst_pub = 0.0, worst_back = 0.0;
	for (int i = 0; i < 9; i++) {
		double d = fabs((double)derived[i] - (double)AZ_MAT_709_TO_2020[i]);
		double p = fabs((double)derived[i] - (double)PUBLISHED_709_TO_2020[i]);
		double b = fabs((double)derived_back[i] -
			(double)AZ_MAT_2020_TO_709[i]);
		if (d > worst) {
			worst = d;
		}
		if (p > worst_pub) {
			worst_pub = p;
		}
		if (b > worst_back) {
			worst_back = b;
		}
	}
	CHECK(worst <= 1e-6, "AZ_MAT_709_TO_2020 == derived (worst %.3g)", worst);
	CHECK(worst_back <= 1e-6, "AZ_MAT_2020_TO_709 == derived (worst %.3g)",
		worst_back);
	CHECK(worst_pub <= 1e-5,
		"derived == BT.2087 published values (worst %.3g)", worst_pub);

	/* M * M^-1 = I. Catches a transpose, which every other check here would
	 * pass on a symmetric-looking table. */
	float prod[9];
	az_mat_mul(AZ_MAT_709_TO_2020, AZ_MAT_2020_TO_709, prod);
	double worst_i = 0.0;
	for (int i = 0; i < 9; i++) {
		double d = fabs((double)prod[i] - (double)AZ_MAT_IDENTITY[i]);
		if (d > worst_i) {
			worst_i = d;
		}
	}
	CHECK(worst_i <= 1e-6, "M709->2020 * M2020->709 = I (worst %.3g)", worst_i);

	/* Same primaries in and out is the identity: the derivation must not
	 * invent a chromatic adaptation nobody asked for. */
	float same[9];
	az_mat_from_primaries(&AZ_PRIMARIES_BT709, &AZ_PRIMARIES_BT709, same);
	double worst_s = 0.0;
	for (int i = 0; i < 9; i++) {
		double d = fabs((double)same[i] - (double)AZ_MAT_IDENTITY[i]);
		if (d > worst_s) {
			worst_s = d;
		}
	}
	CHECK(worst_s <= 1e-6, "709 -> 709 is the identity (worst %.3g)", worst_s);

	/* White maps to white. Both sets are D65, so (1,1,1) is a fixed point --
	 * and this is the one property a wrongly-normalised NPM fails loudly. */
	float w[3] = { 1.0f, 1.0f, 1.0f }, wout[3];
	az_mat_mul_vec3(AZ_MAT_709_TO_2020, w, wout);
	NEAR(wout[0], 1.0, 1e-5, "709->2020 preserves white R");
	NEAR(wout[1], 1.0, 1e-5, "709->2020 preserves white G");
	NEAR(wout[2], 1.0, 1e-5, "709->2020 preserves white B");

	/* Rows sum to 1 for the same reason, per row. */
	for (int r = 0; r < 3; r++) {
		double s = (double)AZ_MAT_709_TO_2020[r * 3 + 0] +
			AZ_MAT_709_TO_2020[r * 3 + 1] + AZ_MAT_709_TO_2020[r * 3 + 2];
		NEAR(s, 1.0, 1e-5, "709->2020 row %d sums to 1", r);
	}

	/* A BT.709 primary is INSIDE BT.2020, so 709->2020 has no negatives;
	 * the reverse does, and that is ADR-002's whole point. */
	int neg_fwd = 0, neg_rev = 0;
	for (int i = 0; i < 9; i++) {
		if (AZ_MAT_709_TO_2020[i] < 0.0f) {
			neg_fwd++;
		}
		if (AZ_MAT_2020_TO_709[i] < 0.0f) {
			neg_rev++;
		}
	}
	CHECK(neg_fwd == 0, "709->2020 is non-negative (709 fits inside 2020)");
	CHECK(neg_rev > 0, "2020->709 has negative terms (out-of-gamut is real)");
}

static void test_saturation(void) {
	printf("saturation matrix\n");
	float m[9];
	az_mat_saturation(1.0f, m);
	double worst = 0.0;
	for (int i = 0; i < 9; i++) {
		double d = fabs((double)m[i] - (double)AZ_MAT_IDENTITY[i]);
		if (d > worst) {
			worst = d;
		}
	}
	CHECK(worst == 0.0, "saturation 1.0 is EXACTLY the identity");

	/* s = 0 is full desaturation: every row is the luma weights, so any
	 * colour maps to its own luminance in all three channels. */
	az_mat_saturation(0.0f, m);
	float c[3] = { 0.2f, 0.7f, 0.1f }, out[3];
	az_mat_mul_vec3(m, c, out);
	double luma = 0.2126 * 0.2 + 0.7152 * 0.7 + 0.0722 * 0.1;
	NEAR(out[0], luma, 1e-6, "saturation 0 R = luma");
	NEAR(out[1], luma, 1e-6, "saturation 0 G = luma");
	NEAR(out[2], luma, 1e-6, "saturation 0 B = luma");

	/* Luminance is preserved for any s: that is what makes it a saturation
	 * control rather than a brightness one. */
	for (float s = 0.0f; s <= 2.01f; s += 0.5f) {
		az_mat_saturation(s, m);
		az_mat_mul_vec3(m, c, out);
		double l2 = 0.2126 * out[0] + 0.7152 * out[1] + 0.0722 * out[2];
		NEAR(l2, luma, 1e-6, "saturation %.1f preserves luminance", s);
	}
}

/* ── tone mapping ───────────────────────────────────────────────────────── */

static float curve(float m, float knee, float peak) {
	float v[3] = { m, 0.0f, 0.0f };
	az_tonemap(v, knee, peak);
	return v[0];
}

static void test_tonemap(void) {
	printf("tone map (ADR-009)\n");
	const float knee = 1.0f;
	const float peak = 1000.0f / 203.0f; /* the live panel at the default */

	/* IDENTITY BELOW THE KNEE. This is the SDR-appearance guarantee: on an
	 * HDR output, everything at or below SDR white must be untouched, or the
	 * whole desktop shifts the moment HDR is enabled. */
	int moved = 0;
	for (int i = 0; i <= 1000; i++) {
		float m = (float)i / 1000.0f;
		float v[3] = { m, m * 0.5f, m * 0.25f };
		float before[3] = { v[0], v[1], v[2] };
		az_tonemap(v, knee, peak);
		if (v[0] != before[0] || v[1] != before[1] || v[2] != before[2]) {
			moved++;
		}
	}
	CHECK(moved == 0, "identity at or below the knee, bit-exact (%d moved)",
		moved);

	/* IDENTITY FOR peak <= 1. An SDR output's ceiling is 1.0 by definition
	 * (ADR-003), and the curve must not touch anything there either. */
	moved = 0;
	for (int i = 0; i <= 100; i++) {
		float m = (float)i / 10.0f;
		float v[3] = { m, m, m }, before = v[0];
		az_tonemap(v, knee, 1.0f);
		if (v[0] != before) {
			moved++;
		}
	}
	CHECK(moved == 0, "peak <= 1 is the identity everywhere (%d moved)", moved);

	/* C1 AT THE KNEE. The one-sided numeric derivatives must both be 1: the
	 * identity segment's slope is exactly 1, so a curve joined with the wrong
	 * slope puts a visible crease at SDR white -- on skin tones and paper
	 * backgrounds, which is where it is least forgivable. */
	const float h = 1e-3f;
	double below = (curve(knee, knee, peak) - curve(knee - h, knee, peak)) / h;
	double above = (curve(knee + h, knee, peak) - curve(knee, knee, peak)) / h;
	NEAR(below, 1.0, 2e-3, "slope just below the knee is 1");
	NEAR(above, 1.0, 2e-3, "slope just above the knee is 1");
	NEAR(curve(knee, knee, peak), knee, 1e-6, "f(knee) = knee");

	/* THE CEILING IS AN ASYMPTOTE. f < peak for every finite input, so the
	 * encode never sees a value it must clip -- including 100x the peak,
	 * which a PQ 10000-nit source genuinely produces. */
	CHECK(curve(peak - 1e-3f, knee, peak) < peak, "f(peak - eps) < peak");
	CHECK(curve(peak, knee, peak) < peak, "f(peak) < peak");
	CHECK(curve(100.0f * peak, knee, peak) < peak, "f(100 peak) < peak");
	CHECK(curve(1e6f, knee, peak) < peak, "f(1e6) < peak");

	/* MONOTONIC through the join. */
	int inversions = 0;
	float prev = curve(0.0f, knee, peak);
	for (int i = 1; i <= 20000; i++) {
		float m = (float)i * 0.001f;
		float y = curve(m, knee, peak);
		if (y < prev) {
			inversions++;
		}
		prev = y;
	}
	CHECK(inversions == 0, "monotonic across the knee (%d inversions)",
		inversions);

	/* HUE PRESERVATION: one common scale, so channel RATIOS survive exactly.
	 * ADR-009's falsifier: (4.0, 0.4, 0.4) must not walk toward white. */
	float v[3] = { 4.0f, 0.4f, 0.4f };
	az_tonemap(v, knee, peak);
	NEAR((double)v[0] / v[1], 10.0, 1e-4, "R:G ratio survives tone mapping");
	NEAR((double)v[1] / v[2], 1.0, 1e-6, "G:B ratio survives tone mapping");
	CHECK(v[0] < 4.0f, "the highlight actually moved (premise)");

	/* Driven by the MAX channel, not per channel: the same colour scaled so
	 * that a different channel is the max must scale by the same rule. */
	float a[3] = { 0.4f, 4.0f, 0.4f };
	az_tonemap(a, knee, peak);
	NEAR(a[1], v[0], 1e-6, "max-channel driven, whichever channel it is");

	/* NEGATIVE COMPONENTS (ADR-002 wide gamut) survive the scale rather than
	 * being clamped by the curve -- clamping is the output's job (ADR-010). */
	float n[3] = { 4.0f, -0.2f, 0.1f };
	az_tonemap(n, knee, peak);
	CHECK(n[1] < 0.0f, "a negative component stays negative through the curve");
	NEAR((double)n[0] / n[1], 4.0 / -0.2, 1e-4, "ratio survives with negatives");
}

/* ── dither noise ───────────────────────────────────────────────────────── */

static void test_ign(void) {
	printf("interleaved gradient noise\n");
	/* Range and determinism. */
	int out_of_range = 0;
	double sum = 0.0;
	int n = 0;
	for (int y = 0; y < 128; y++) {
		for (int x = 0; x < 128; x++) {
			float v = az_ign((float)x, (float)y);
			if (!(v >= 0.0f && v < 1.0f)) {
				out_of_range++;
			}
			sum += v;
			n++;
		}
	}
	CHECK(out_of_range == 0, "az_ign in [0, 1) over a 128x128 raster");
	/* Zero-mean after the -0.5 the dither applies: the pattern must not add
	 * a DC offset, or every dithered gradient shifts by half a code. */
	NEAR(sum / n - 0.5, 0.0, 0.01, "mean is 0.5 (dither is zero-mean)");
	CHECK(az_ign(37.0f, 11.0f) == az_ign(37.0f, 11.0f),
		"deterministic: a still desktop is bit-identical frame to frame");
	/* A function of the pixel ALONE, and genuinely varying: a constant
	 * "noise" passes a mean test and dithers nothing. */
	CHECK(az_ign(0.0f, 0.0f) != az_ign(1.0f, 0.0f), "varies in x");
	CHECK(az_ign(0.0f, 0.0f) != az_ign(0.0f, 1.0f), "varies in y");
	/* Negative coordinates: a regional target's origin can be negative and
	 * fract() must stay non-negative there (fmodf() would not). */
	float neg = az_ign(-3.0f, -7.0f);
	CHECK(neg >= 0.0f && neg < 1.0f, "az_ign non-negative for negative input");
}

/* ── GLSL source-text parity ────────────────────────────────────────────── */

/*
 * The numerical GPU parity fixture (C1) needs a device and is deferred. This
 * is the part that can run anywhere and catches the drift that actually
 * happens: a constant edited in one file and not the other. It reads both
 * sources and compares the numeric literal of every shared #define.
 */
struct shared_const {
	const char *name;
	double value;
};

static const struct shared_const SHARED[] = {
	{ "AZ_PQ_M1", 0.1593017578125 },
	{ "AZ_PQ_M2", 78.84375 },
	{ "AZ_PQ_C1", 0.8359375 },
	{ "AZ_PQ_C2", 18.8515625 },
	{ "AZ_PQ_C3", 18.6875 },
	{ "AZ_SRGB_E_THRESHOLD", 0.04045 },
	{ "AZ_BT1886_LMIN", 0.01 },
	{ "AZ_BT1886_LMAX", 100.0 },
	{ "AZ_HLG_A", 0.17883277 },
	{ "AZ_HLG_B", 0.28466892 },
	{ "AZ_HLG_C", 0.55991073 },
};

static char *slurp(const char *path) {
	FILE *f = fopen(path, "rb");
	if (f == NULL) {
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *buf = malloc((size_t)len + 1);
	if (buf == NULL || fread(buf, 1, (size_t)len, f) != (size_t)len) {
		free(buf);
		fclose(f);
		return NULL;
	}
	buf[len] = '\0';
	fclose(f);
	return buf;
}

/* Replaces every comment body with spaces, so a grep over the result is a grep
 * over the CODE. Not a C parser: it does not know about string literals, and
 * GLSL has none worth the complication. */
static char *strip_comments(const char *src) {
	size_t n = strlen(src);
	char *out = malloc(n + 1);
	if (out == NULL) {
		return NULL;
	}
	size_t o = 0;
	for (size_t i = 0; i < n;) {
		if (src[i] == '/' && i + 1 < n && src[i + 1] == '*') {
			i += 2;
			while (i + 1 < n && !(src[i] == '*' && src[i + 1] == '/')) {
				i++;
			}
			i = i + 2 > n ? n : i + 2;
			out[o++] = ' ';
		} else if (src[i] == '/' && i + 1 < n && src[i + 1] == '/') {
			while (i < n && src[i] != '\n') {
				i++;
			}
		} else {
			out[o++] = src[i++];
		}
	}
	out[o] = '\0';
	return out;
}

/* Finds `#define NAME <number>` and returns the number. -1 if absent. */
static int define_value(const char *src, const char *name, double *out) {
	char needle[128];
	snprintf(needle, sizeof(needle), "#define %s ", name);
	const char *p = strstr(src, needle);
	if (p == NULL) {
		return -1;
	}
	p += strlen(needle);
	char *end = NULL;
	double v = strtod(p, &end);
	if (end == p) {
		return -1;
	}
	*out = v;
	return 0;
}

static void test_glsl_parity(const char *src_root) {
	printf("GLSL twin: shared constants (source-text parity)\n");
	char hpath[1024], gpath[1024];
	snprintf(hpath, sizeof(hpath), "%s/src/render/color/az_color.h", src_root);
	snprintf(gpath, sizeof(gpath),
		"%s/src/render/vulkan/shader/src/color.glsl", src_root);
	char *h = slurp(hpath), *g = slurp(gpath);
	if (h == NULL || g == NULL) {
		printf("  FAIL cannot read %s / %s\n", hpath, gpath);
		failures++;
		checks++;
		free(h);
		free(g);
		return;
	}
	for (size_t i = 0; i < sizeof(SHARED) / sizeof(SHARED[0]); i++) {
		double hv = 0.0, gv = 0.0;
		int hok = define_value(h, SHARED[i].name, &hv);
		int gok = define_value(g, SHARED[i].name, &gv);
		if (hok != 0 || gok != 0) {
			CHECK(0, "%s present in both az_color.h and color.glsl",
				SHARED[i].name);
			continue;
		}
		CHECK(hv == SHARED[i].value && gv == SHARED[i].value,
			"%s: header %.13g, glsl %.13g, standard %.13g",
			SHARED[i].name, hv, gv, SHARED[i].value);
	}
	/* The GLSL twin must contain every function the contract names, or the
	 * parity claim is about a file with holes in it. */
	static const char *fns[] = {
		"az_srgb_eotf", "az_srgb_ieotf", "az_gamma22_eotf", "az_gamma22_ieotf",
		"az_bt1886_eotf", "az_bt1886_ieotf", "az_pq_eotf", "az_pq_ieotf",
		"az_hlg_eotf", "az_tonemap", "az_ign",
	};
	for (size_t i = 0; i < sizeof(fns) / sizeof(fns[0]); i++) {
		CHECK(strstr(g, fns[i]) != NULL, "color.glsl defines %s", fns[i]);
	}
	/* And no alpha may enter one: every signature is three components in,
	 * three out. The check is blunt on purpose -- ADR-005's rule is
	 * unrepresentable if no type in the file can hold an alpha. Comments are
	 * stripped first, because a rule explained in prose must not trip the
	 * check that enforces it. */
	char *code = strip_comments(g);
	CHECK(code != NULL && strstr(code, "vec4") == NULL,
		"no 4-component type in color.glsl code "
		"(alpha is unrepresentable by construction)");
	free(code);
	free(h);
	free(g);
}

int main(int argc, char **argv) {
	const char *src_root = argc > 1 ? argv[1] : ".";
	printf("== C1 colour math ==\n");
	test_round_trip();
	test_monotonic();
	test_endpoints();
	test_clamped_domain();
	test_anchors();
	test_matrices();
	test_saturation();
	test_tonemap();
	test_ign();
	test_glsl_parity(src_root);
	printf("\n%d checks, %d failures\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
