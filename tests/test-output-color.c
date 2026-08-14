/*
 * The output-colour-state derivation. Contract C3, ADR-012.
 *
 * A decision table with five outcomes and six inputs. What makes it worth a
 * test file is that four of the five outcomes render a plausible picture when
 * chosen wrongly:
 *
 *   Path A picked for a 10-bit output   -> composition into a buffer whose
 *                                          encoding the hardware cannot do,
 *                                          i.e. banded but not obviously wrong
 *   Path B picked for an 8-bit output   -> correct pixels, a whole extra pass
 *                                          and 66 MB per output nobody asked
 *                                          for
 *   peak_scene != 1 on an SDR output    -> the tone map engages on SDR content
 *                                          and the desktop dims
 *   the saturation matrix composed the
 *   wrong way round                     -> desaturates toward BT.2020's luma
 *                                          axis; still a picture, wrong colour
 *
 * So every row asserts the WHOLE state, not the field it is about, and the
 * matrix rows are checked against C1's constants rather than against a second
 * copy of the composition.
 */

#define _POSIX_C_SOURCE 200809L

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "render/az_output_color.h"

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
			printf("  (%.6f)\n", (double)(a)); \
		} else { \
			failures++; \
			printf("  FAIL " __VA_ARGS__); \
			printf("  got %.9g want %.9g, |d| = %.3g\n", \
				(double)(a), (double)(b), _d); \
		} \
	} while (0)

static const char *path_name(enum az_output_path p) {
	switch (p) {
	case AZ_OUTPUT_PATH_A_DIRECT_SRGB:
		return "A";
	case AZ_OUTPUT_PATH_B_ENCODE:
		return "B";
	case AZ_OUTPUT_PATH_FALLBACK:
		return "FALLBACK";
	}
	return "?";
}

static bool matrix_is(const float m[9], const float want[9], double tol) {
	for (int i = 0; i < 9; i++) {
		if (fabs((double)m[i] - (double)want[i]) > tol) {
			return false;
		}
	}
	return true;
}

struct row {
	const char *what;
	struct az_output_desc out;
	enum az_output_path want_path;
	enum az_tf want_tf;
	double want_peak;
	double want_dither;
	bool want_identity_matrix;
};

int main(void) {
	printf("== C3 output colour state ==\n");

	/* .bits_per_channel, .hdr, .has_icc, .hdr_max_nits, .scene_ref_nits,
	 * .sdr_saturation, .scanout_srgb_view_ok */
	const struct row ROWS[] = {
		{"SDR 8-bit, _SRGB scanout view available",
			{8, false, false, 0.0f, 0.0f, 1.0f, true},
			AZ_OUTPUT_PATH_A_DIRECT_SRGB, AZ_TF_SRGB, 1.0, 0.0, true},
		/* The probe is the ONLY thing between these two rows, and it is the
		 * audit's biggest unknown: if it comes back false on this GPU's
		 * scanout modifiers, Path A is dead code and every output is B. */
		{"SDR 8-bit, probe failed -> B with 1/255 dither",
			{8, false, false, 0.0f, 0.0f, 1.0f, false},
			AZ_OUTPUT_PATH_B_ENCODE, AZ_TF_SRGB, 1.0, 1.0 / 255.0, true},
		{"SDR 10-bit (bitdepth:10)",
			{10, false, false, 0.0f, 0.0f, 1.0f, true},
			AZ_OUTPUT_PATH_B_ENCODE, AZ_TF_SRGB, 1.0, 1.0 / 1023.0, true},
		/* HDR at the defaults: 1000-nit panel, 203-nit reference. */
		{"HDR, panel peak unstated -> 1000 nits",
			{10, true, false, 0.0f, 0.0f, 1.0f, true},
			AZ_OUTPUT_PATH_B_ENCODE, AZ_TF_PQ, 1000.0 / 203.0,
			1.0 / 1023.0, false},
		{"HDR, 600-nit panel", {10, true, false, 600.0f, 0.0f, 1.0f, true},
			AZ_OUTPUT_PATH_B_ENCODE, AZ_TF_PQ, 600.0 / 203.0,
			1.0 / 1023.0, false},
		{"HDR, reference moved to 400",
			{10, true, false, 1000.0f, 400.0f, 1.0f, true},
			AZ_OUTPUT_PATH_B_ENCODE, AZ_TF_PQ, 1000.0 / 400.0,
			1.0 / 1023.0, false},
		/* ADR-000: ICC stays on fx_vk for the whole of M5. */
		{"ICC profile attached -> AVK refuses",
			{10, false, true, 0.0f, 0.0f, 1.0f, true},
			AZ_OUTPUT_PATH_FALLBACK, AZ_TF_SRGB, 1.0, 0.0, true},
		{"ICC on an HDR output -> still refuses",
			{10, true, true, 1000.0f, 0.0f, 1.0f, true},
			AZ_OUTPUT_PATH_FALLBACK, AZ_TF_SRGB, 1.0, 0.0, true},
	};

	printf("the decision table\n");
	for (size_t i = 0; i < sizeof(ROWS) / sizeof(ROWS[0]); i++) {
		const struct row *r = &ROWS[i];
		struct az_output_color_state s = az_output_color_derive(&r->out);
		CHECK(s.path == r->want_path, "%s: path %s (got %s)", r->what,
			path_name(r->want_path), path_name(s.path));
		CHECK(s.encode_tf == r->want_tf, "%s: encode_tf", r->what);
		NEAR(s.peak_scene, r->want_peak, 1e-5, "%s: peak_scene", r->what);
		NEAR(s.dither_q, r->want_dither, 1e-9, "%s: dither_q", r->what);
		CHECK(matrix_is(s.matrix,
				  r->want_identity_matrix ? AZ_MAT_IDENTITY
										  : AZ_MAT_709_TO_2020, 1e-6),
			"%s: matrix", r->what);
	}

	/* ── the SDR guarantee ──────────────────────────────────────────────── */
	printf("SDR output => peak_scene is EXACTLY 1.0\n");
	{
		/* Not "about 1.0": the tone map's identity branch tests peak <= 1, so
		 * a peak of 1.0000001 on an SDR output turns the curve on for every
		 * highlight on the desktop. */
		int wrong = 0;
		for (int bpc = 8; bpc <= 10; bpc += 2) {
			for (int probe = 0; probe < 2; probe++) {
				for (int sat = 0; sat < 2; sat++) {
					struct az_output_desc o = {bpc, false, false, 1000.0f,
						203.0f, sat ? 1.7f : 1.0f, probe != 0};
					struct az_output_color_state s =
						az_output_color_derive(&o);
					if (s.peak_scene != 1.0f) {
						wrong++;
					}
				}
			}
		}
		CHECK(wrong == 0,
			"1.0f exactly, over every SDR combination (%d wrong)", wrong);
		/* and a panel peak declared on an SDR output is ignored rather than
		 * quietly becoming a ceiling above 1. */
		struct az_output_desc o = {8, false, false, 1600.0f, 203.0f, 1.0f,
			true};
		struct az_output_color_state s = az_output_color_derive(&o);
		CHECK(s.peak_scene == 1.0f,
			"an SDR output ignores a declared panel peak");
	}

	/* ── the saturation composition ─────────────────────────────────────── */
	printf("sdr_saturation changes the matrix and NOTHING else\n");
	{
		struct az_output_desc plain = {10, true, false, 1000.0f, 203.0f, 1.0f,
			true};
		struct az_output_desc sat = plain;
		sat.sdr_saturation = 0.5f;
		struct az_output_color_state a = az_output_color_derive(&plain);
		struct az_output_color_state b = az_output_color_derive(&sat);
		CHECK(a.path == b.path && a.encode_tf == b.encode_tf &&
				a.ref_nits == b.ref_nits && a.peak_scene == b.peak_scene &&
				a.dither_q == b.dither_q,
			"every non-matrix field is unchanged");
		CHECK(!matrix_is(b.matrix, a.matrix, 1e-6),
			"and the matrix DID change (premise)");

		/* Composed as scenefx composes it: primaries * saturation. Rebuilt
		 * here from C1's primitives in the stated order -- so a transposed
		 * composition fails, which is the failure that still renders. */
		float want[9], s9[9];
		az_mat_saturation(0.5f, s9);
		az_mat_mul(AZ_MAT_709_TO_2020, s9, want);
		CHECK(matrix_is(b.matrix, want, 1e-6),
			"matrix == M709->2020 * S(0.5), in that order");
		float wrong_order[9];
		az_mat_mul(s9, AZ_MAT_709_TO_2020, wrong_order);
		CHECK(!matrix_is(want, wrong_order, 1e-6),
			"  (the two orders differ, so the assertion above has teeth)");

		/* saturation 1.0 and saturation 0 (unset) must both give the bare
		 * gamut matrix, bit-for-bit -- no round trip through a composition
		 * that is only nearly the identity. */
		struct az_output_desc unset = plain;
		unset.sdr_saturation = 0.0f;
		struct az_output_color_state u = az_output_color_derive(&unset);
		CHECK(matrix_is(u.matrix, AZ_MAT_709_TO_2020, 0.0),
			"saturation unset gives M709->2020 exactly");
		CHECK(matrix_is(a.matrix, AZ_MAT_709_TO_2020, 0.0),
			"saturation 1.0 gives M709->2020 exactly");
		/* An SDR output is not colour managed, so saturation has nothing to
		 * act on there -- it must not sneak into the identity. */
		struct az_output_desc sdr_sat = {8, false, false, 0.0f, 203.0f, 0.5f,
			true};
		struct az_output_color_state ss = az_output_color_derive(&sdr_sat);
		CHECK(matrix_is(ss.matrix, AZ_MAT_IDENTITY, 0.0),
			"an SDR output's matrix stays the exact identity");
	}

	/* ── the matrix rows, against C1 ────────────────────────────────────── */
	printf("the HDR matrix is the BT.2087 one\n");
	{
		struct az_output_desc o = {10, true, false, 1000.0f, 203.0f, 1.0f,
			true};
		struct az_output_color_state s = az_output_color_derive(&o);
		float derived[9];
		az_mat_from_primaries(&AZ_PRIMARIES_BT709, &AZ_PRIMARIES_BT2020,
			derived);
		CHECK(matrix_is(s.matrix, derived, 1e-6),
			"equals az_mat_from_primaries(709, 2020)");
		float white[3] = {1.0f, 1.0f, 1.0f}, out[3];
		az_mat_mul_vec3(s.matrix, white, out);
		NEAR(out[0], 1.0, 1e-5, "white stays white through it (R)");
		NEAR(out[1], 1.0, 1e-5, "white stays white through it (G)");
		NEAR(out[2], 1.0, 1e-5, "white stays white through it (B)");
	}

	/* ── the reference is the scene's, not the output's ─────────────────── */
	printf("ref_nits\n");
	{
		struct az_output_desc o = {10, true, false, 1000.0f, 0.0f, 1.0f, true};
		struct az_output_color_state s = az_output_color_derive(&o);
		NEAR(s.ref_nits, 203.0, 1e-6, "unset resolves to the BT.2408 default");
		o.scene_ref_nits = 320.0f;
		s = az_output_color_derive(&o);
		NEAR(s.ref_nits, 320.0, 1e-6, "and is carried through when set");
		/* Raising the reference lowers the ceiling in scene units: the panel
		 * did not change, the meaning of scene 1.0 did. */
		NEAR(s.peak_scene, 1000.0 / 320.0, 1e-5,
			"peak_scene falls as the reference rises");
	}

	/* ── NULL ───────────────────────────────────────────────────────────── */
	printf("no output description\n");
	{
		struct az_output_color_state s = az_output_color_derive(NULL);
		CHECK(s.path == AZ_OUTPUT_PATH_FALLBACK,
			"NULL refuses rather than guessing a path");
		CHECK(matrix_is(s.matrix, AZ_MAT_IDENTITY, 0.0),
			"with an initialised identity matrix, not garbage");
	}

	printf("\n%d checks, %d failures\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
