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

/* M6C. Any non-identity evaluator will do for the decision table -- what is
 * being asserted is what the state DERIVES from a cube's presence, not what the
 * cube contains. A channel swap because it cannot be mistaken for the identity
 * if it ever ends up printed. */
static void clut_eval_swap(void *user, const float in[3], float out[3]) {
	(void)user;
	out[0] = in[0];
	out[1] = in[2];
	out[2] = in[1];
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
		/* M6B/D3: hdr is decided BEFORE has_icc. A profile on an HDR output
		 * is inert -- the connector is already colour-managing it, and
		 * az_output_color_transform() has always returned NULL there -- so it
		 * must not also cost the output its renderer. This row asserted the
		 * opposite until the operator's own calibrated-display config line
		 * turned out to be unrestorable because of it. */
		{"ICC on an HDR output -> HDR wins, profile inert",
			{10, true, true, 1000.0f, 0.0f, 1.0f, true},
			AZ_OUTPUT_PATH_B_ENCODE, AZ_TF_PQ, 1000.0 / 203.0, 1.0 / 1023.0,
			false},
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

	/* ── M6B/G2: A PROFILE AVK CAN CARRY ────────────────────────────────── */
	printf("a matrix-shaper profile puts an SDR output on Path B with LUT1D\n");
	{
		/*
		 * A SYNTHETIC shaper, built here rather than read from a file. The
		 * decision table's question is "what does C3 do when it is handed
		 * one", and a real profile would tie this test to one desk while
		 * answering the same question. G1 and G2 check the real one.
		 *
		 * The matrix is deliberately NOT the identity and its rows deliberately
		 * do NOT sum to 1: a shaper whose matrix is near-identity would let
		 * this pass against a derivation that ignored the shaper's matrix and
		 * kept its own -- the neutral blind spot again, one level up.
		 */
		struct az_icc_shaper shaper = {0};
		const float M[9] = {
			1.10f, -0.06f, -0.02f,
			-0.03f, 1.04f, -0.01f,
			0.00f, -0.07f, 1.20f,
		};
		for (int i = 0; i < 9; i++) {
			shaper.matrix[i] = M[i];
		}
		shaper.has_vcgt = true;

		struct az_output_desc o = {8, false, true, 0.0f, 0.0f, 1.0f, true,
			&shaper};
		struct az_output_color_state s = az_output_color_derive(&o);
		CHECK(s.path == AZ_OUTPUT_PATH_B_ENCODE,
			"profiled SDR output takes Path B (got %s)", path_name(s.path));
		CHECK(s.encode_tf == AZ_TF_LUT1D,
			"and encodes with the measured curve");
		CHECK(matrix_is(s.matrix, M, 1e-6),
			"the profile's OWN matrix reaches the encode pass");
		/* Neither of these is a colour-management question, and both would be
		 * easy to change by accident while wiring one. */
		CHECK(s.peak_scene == 1.0f,
			"an SDR ceiling is still exactly 1.0 with a profile attached");
		NEAR(s.dither_q, 1.0 / 255.0, 1e-9, "and still one output code");

		/*
		 * THE SAME OUTPUT WITHOUT THE SHAPER. Two rows differing in one
		 * pointer, because the whole of D2's refusal is that difference: a
		 * cLUT profile sets has_icc and leaves this NULL.
		 */
		struct az_output_desc no = {8, false, true, 0.0f, 0.0f, 1.0f, true,
			NULL};
		struct az_output_color_state ns = az_output_color_derive(&no);
		CHECK(ns.path == AZ_OUTPUT_PATH_FALLBACK
				&& ns.encode_tf == AZ_TF_SRGB,
			"a profile AVK cannot reduce still means FALLBACK");

		/*
		 * AND ON AN HDR OUTPUT THE SHAPER CHANGES NOTHING. D3 decided hdr
		 * first; G2 must not have quietly reopened that by adding a branch
		 * above it. An HDR output's connector is already colour-managing, and
		 * a profile's SDR characterisation on top would be two transforms.
		 */
		struct az_output_desc hdr = {10, true, true, 1000.0f, 0.0f, 1.0f, true,
			&shaper};
		struct az_output_color_state hs = az_output_color_derive(&hdr);
		CHECK(hs.path == AZ_OUTPUT_PATH_B_ENCODE && hs.encode_tf == AZ_TF_PQ,
			"HDR + a reducible profile is still PQ, profile inert");
		CHECK(matrix_is(hs.matrix, AZ_MAT_709_TO_2020, 1e-6),
			"and the gamut matrix is BT.709->BT.2020, not the profile's");

		/*
		 * ── THE INTERLOCK ────────────────────────────────────────────────
		 *
		 * LUT1D says the picture is defined by a table only the encode pass can
		 * sample, and C3 took the output off FALLBACK to say AVK would apply
		 * it -- which strips the profile from SceneFX. Accepting the output
		 * with the pass switched off would leave the profile applied by
		 * nobody: uncharacterised colour on a calibrated display, arrived at by
		 * honouring the calibration.
		 */
		CHECK(az_output_may_drive(&s, false, false, true),
			"LUT1D + encode pass enabled: DRIVEN");
		CHECK(!az_output_may_drive(&s, false, false, false),
			"LUT1D + encode pass DISABLED: refused (the M6B/G2 interlock)");
	}

	/* ── M6C: A PROFILE THAT DOES NOT REDUCE ────────────────────────────── */
	printf("a cLUT profile puts an SDR output on Path B with CLUT3D\n");
	{
		/*
		 * A SYNTHETIC cube, and its CONTENTS are irrelevant to this table:
		 * every field of the derived state is a function of the pointer being
		 * non-NULL and of nothing inside it. Two samples is enough to have
		 * built one legally. The cube's arithmetic is test-icc-shaper's gate
		 * and its pixels are test-avk-render's.
		 */
		struct az_icc_clut *c = az_icc_clut_build(2, clut_eval_swap, NULL);
		CHECK(c != NULL, "PREMISE: a cube can be built at all");
		if (c == NULL) {
			return 1;
		}

		struct az_output_desc o = {
			.bits_per_channel = 8, .has_icc = true,
			.sdr_saturation = 1.0f, .scanout_srgb_view_ok = true,
			.icc_clut = c,
		};
		struct az_output_color_state s = az_output_color_derive(&o);
		CHECK(s.path == AZ_OUTPUT_PATH_B_ENCODE,
			"a cLUT-profiled SDR output takes Path B (got %s)",
			path_name(s.path));
		CHECK(s.encode_tf == AZ_TF_CLUT3D, "and encodes through the cube");
		/*
		 * THE IDENTITY MATRIX IS THE ASSERTION THAT MATTERS HERE.
		 *
		 * The cube already contains the colorant transform. A derivation that
		 * filled this with the profile's 709->device -- the obvious thing to do
		 * by analogy with LUT1D one branch above -- would apply the gamut
		 * conversion twice and render a picture that is smooth, saturated and
		 * wrong. Nothing downstream could notice: the encode pass multiplies by
		 * whatever it is given.
		 */
		CHECK(matrix_is(s.matrix, AZ_MAT_IDENTITY, 1e-6),
			"and the matrix is the IDENTITY -- the cube carries the gamut");
		CHECK(s.peak_scene == 1.0f, "an SDR ceiling is still exactly 1.0");
		NEAR(s.dither_q, 1.0 / 255.0, 1e-9, "and still one output code");

		/* THE INTERLOCK, the same one LUT1D has and for the same reason: C3
		 * took the output off FALLBACK on the promise that the encode pass
		 * would sample the cube. */
		CHECK(az_output_may_drive(&s, false, false, true),
			"CLUT3D + encode pass enabled: DRIVEN");
		CHECK(!az_output_may_drive(&s, false, false, false),
			"CLUT3D + encode pass DISABLED: refused (the M6C interlock)");

		/*
		 * ── THE SHAPER STILL WINS ─────────────────────────────────────────
		 *
		 * A profile offered in BOTH forms must take the matrix-shaper one. The
		 * compositor never offers both, but "never" is a property of one call
		 * site and this is the table that would silently absorb it if that
		 * changed -- and absorbing it would swap a 0.01-code curve for a
		 * trilinear approximation of the same display, invisibly.
		 */
		struct az_icc_shaper shaper = {0};
		const float M[9] = {
			1.10f, -0.06f, -0.02f,
			-0.03f, 1.04f, -0.01f,
			0.00f, -0.07f, 1.20f,
		};
		for (int i = 0; i < 9; i++) {
			shaper.matrix[i] = M[i];
		}
		struct az_output_desc both = o;
		both.icc_shaper = &shaper;
		struct az_output_color_state bs = az_output_color_derive(&both);
		CHECK(bs.encode_tf == AZ_TF_LUT1D,
			"both forms offered: the matrix-shaper wins");
		CHECK(matrix_is(bs.matrix, M, 1e-6),
			"and it is the shaper's matrix, not the cube's identity");

		/*
		 * AND ON AN HDR OUTPUT THE CUBE IS INERT, exactly as the shaper is
		 * (D3). A profile's SDR characterisation on top of a connector that is
		 * already colour-managing would be two transforms.
		 */
		struct az_output_desc hdr = o;
		hdr.hdr = true;
		hdr.bits_per_channel = 10;
		hdr.hdr_max_nits = 1000.0f;
		struct az_output_color_state hs = az_output_color_derive(&hdr);
		CHECK(hs.path == AZ_OUTPUT_PATH_B_ENCODE && hs.encode_tf == AZ_TF_PQ,
			"HDR + a cLUT profile is still PQ, cube inert");

		/*
		 * AND NEITHER FORM IS STILL FALLBACK. This is the row D2 used to be,
		 * kept because it is now the only path to it: an unreadable profile,
		 * or one whose transform would not evaluate.
		 */
		struct az_output_desc neither = o;
		neither.icc_clut = NULL;
		struct az_output_color_state ns = az_output_color_derive(&neither);
		CHECK(ns.path == AZ_OUTPUT_PATH_FALLBACK && ns.encode_tf == AZ_TF_SRGB,
			"a profile in NEITHER form is still FALLBACK");
		CHECK(!az_output_may_drive(&ns, false, false, true),
			"and is refused whatever the encode pass is doing");

		az_icc_clut_free(c);
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

	/* ── M5.6: MAY AVK DRIVE THIS OUTPUT? ──────────────────────────────── */
	//
	// The refusal that used to read
	//     color_transform != NULL || output->image_description != NULL
	// was one condition covering two unrelated things, and only the second may
	// be lifted. This is the table that replaced it.
	//
	// The row that matters most is the third: an HDR output with the encode
	// pass DISABLED must be refused. Accepting it composites scene-linear
	// values into a PQ-encoded BT.2020 scan-out buffer as though they were
	// already electrical -- a wrong picture on exactly the content HDR was
	// enabled for, and strictly worse than the SceneFX fallback it replaces.
	printf("M5.6 may-drive decision\n");
	{
		struct az_output_desc hdr_desc = {10, true, false, 1000.0f, 0.0f, 1.0f,
			false};
		struct az_output_color_state hdr = az_output_color_derive(&hdr_desc);
		struct az_output_desc sdr8_desc = {8, false, false, 0.0f, 0.0f, 1.0f,
			true};
		struct az_output_color_state sdr8 = az_output_color_derive(&sdr8_desc);
		struct az_output_desc icc_desc = {8, false, true, 0.0f, 0.0f, 1.0f,
			true};
		struct az_output_color_state icc = az_output_color_derive(&icc_desc);

		CHECK(az_output_may_drive(&hdr, true, false, true),
			"HDR + encode pass enabled: DRIVEN");
		CHECK(!az_output_may_drive(&hdr, true, false, false),
			"HDR + encode pass DISABLED: refused (the interlock)");
		CHECK(!az_output_may_drive(&icc, false, true, true),
			"an SDR ICC transform is refused even with the pass on (M6B/D2)");
		CHECK(!az_output_may_drive(&icc, false, false, true),
			"and C3's own FALLBACK is refused without being asked twice");

		/*
		 * ── M6B/D3: A PROFILE MUST NOT COST AN HDR OUTPUT ITS RENDERER ────
		 *
		 * has_icc used to be tested before hdr, so ANY profiled output derived
		 * FALLBACK regardless of what else it was. The operator's own
		 * monitors.kdl carries the consequence, commented out beside a note
		 * that the display is calibrated:
		 *
		 *     icc-profile "/home/ralf/FI32U.icm"; hdr 1;
		 *
		 * Restoring that line lost DP-1's HDR and its renderer together. An
		 * HDR output is colour-managed by the connector; the profile is inert
		 * there, not a reason to abandon the output.
		 */
		struct az_output_desc hdr_icc_desc = {10, true, true, 400.0f, 0.0f,
			1.0f, false};
		struct az_output_color_state hdr_icc =
			az_output_color_derive(&hdr_icc_desc);
		CHECK(hdr_icc.path == AZ_OUTPUT_PATH_B_ENCODE,
			"HDR + profile: Path B, the profile does not cost the renderer");
		CHECK(hdr_icc.encode_tf == AZ_TF_PQ,
			"HDR + profile: still PQ -- the connector manages the colour");
		/* has_color_transform is FALSE here, modelling the real call site:
		 * az_output_color_transform() returns NULL whenever the output carries
		 * its own image description, so an HDR output never presents AVK with
		 * a transform to refuse. Passing true here would test a state the
		 * compositor cannot produce. */
		CHECK(az_output_may_drive(&hdr_icc, true, false, true),
			"HDR + profile + encode pass: DRIVEN (the config line is "
			"restorable)");
		CHECK(!az_output_may_drive(&hdr_icc, true, false, false),
			"HDR + profile without the pass: still refused (the interlock "
			"outranks the profile)");
		CHECK(az_output_may_drive(&sdr8, false, false, false),
			"SDR with no encode pass: driven, as every build before M5 did");
		CHECK(az_output_may_drive(&sdr8, false, false, true),
			"SDR with the pass available: driven");
		CHECK(!az_output_may_drive(NULL, false, false, true),
			"a NULL state refuses rather than guessing");
		/* An image description on an output C3 did NOT put on Path B is either
		 * a stale state or a derivation bug. Both are reasons to refuse: the
		 * buffer is PQ and the pass would not encode for it. */
		CHECK(!az_output_may_drive(&sdr8, true, false, true),
			"an image description on a non-Path-B state: refused, not guessed");
	}

	printf("\n%d checks, %d failures\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
