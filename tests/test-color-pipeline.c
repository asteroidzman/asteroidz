/*
 * THE M5 GATE. Contract C4.
 *
 * This file is the executable form of "prove SDR before HDR". Gate 1 is an
 * EXACT identity -- an opaque sRGB source through an unmodified domain onto a
 * Path-A output must come back out as the codes that went in, with no
 * tolerance at all -- and HDR10 output work may not be enabled in the
 * compositor until the same gate is green through the GPU.
 *
 * WHY AN EXACT GATE IS POSSIBLE AT ALL. ADR-004 chose the piecewise sRGB EOTF
 * for untagged surfaces precisely so that decode and encode are the same curve
 * run in opposite directions, in hardware, on both the sampling and the
 * attachment side. Everything else in M5 -- the luminance anchor, the gamut
 * matrix, the tone curve -- is arranged to be the identity on that path. An
 * exact gate is the strongest correctness anchor available, and it exists
 * because the architecture was chosen to make it exist.
 *
 * THE BREAKS ARE PART OF THE SUITE, NOT A SEPARATE BUILD. Every gate is re-run
 * against each broken pipeline variant, and a break that does NOT fail its
 * oracle fails the suite. That is the whole point: a test which has quietly
 * stopped being able to fail is worse than no test, because it reports safety.
 * The project has found two breaks giving zero coverage this way before.
 */

#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "render/color/az_color.h"
#include "render/color/az_color_ref.h"

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
			printf("  (%.8f, |d| = %.3g)\n", (double)(a), _d); \
		} else { \
			failures++; \
			printf("  FAIL " __VA_ARGS__); \
			printf("  got %.9g want %.9g, |d| = %.3g > %.3g\n", \
				(double)(a), (double)(b), _d, (double)(tol)); \
		} \
	} while (0)

/* Gates 8 and 9 need an AREA rather than a pixel, so they are defined after
 * run_breaks() reads more naturally; declared here so run_breaks() can include
 * them in the per-break tally. Two of the breaks are caught by nothing else. */
static void gate8_dither(enum az_ref_break brk, int *fails);
static void gate9_encoded_blur(enum az_ref_break brk, int *fails);

/* ── fixtures ───────────────────────────────────────────────────────────── */

static struct az_output_desc SDR8 = {8, false, false, 0.0f, 203.0f, 1.0f, true};
static struct az_output_desc SDR10 = {10, false, false, 0.0f, 203.0f, 1.0f,
	true};
static struct az_output_desc HDR = {10, true, false, 1000.0f, 203.0f, 1.0f,
	true};

static struct az_ref_output output_of(const struct az_output_desc *d,
		bool dither) {
	struct az_ref_output o;
	memset(&o, 0, sizeof(o));
	o.state = az_output_color_derive(d);
	o.knee = 1.0; /* ADR-009 */
	o.dither = dither;
	o.blur_radius = 0;
	return o;
}

static struct az_lum_domain sdr_domain(float scale) {
	struct az_lum_source_desc s = {0};
	struct az_lum_rules r = {scale, 0.0f};
	return az_lum_resolve(&s, &r, 203.0f);
}

static struct az_lum_domain pq_domain(float ref) {
	struct az_lum_source_desc s = {true, AZ_TF_PQ, AZ_PRIM_BT2020, 0.0f};
	struct az_lum_rules r = {0.0f, 0.0f};
	return az_lum_resolve(&s, &r, ref);
}

/* A flat patch of one premultiplied electrical colour. */
static bool patch(struct az_ref_image *img, int w, int h, double r, double g,
		double b, double a) {
	if (!az_ref_image_init(img, w, h)) {
		return false;
	}
	double v[4] = {r, g, b, a};
	az_ref_image_fill(img, v);
	return true;
}

/* ── gate 1: the SDR round trip ─────────────────────────────────────────── */

/*
 * All 256 8-bit codes, opaque, untagged, Path A. Bit-exact. No tolerance.
 *
 * 8-bit codes rather than arbitrary floats because that is what a client
 * buffer contains, and because Path A's accumulator IS the 8-bit scanout
 * buffer -- an arbitrary float would be quantised by the hardware and the
 * "identity" would be an identity on the quantiser instead. The float-exact
 * form of the same claim is gate 1b, on Path B, whose intermediate is not
 * quantised.
 */
static void gate1_sdr_round_trip(enum az_ref_break brk, int *fails) {
	struct az_ref_output out = output_of(&SDR8, false);
	int bad = 0;
	double worst = 0.0;
	for (int code = 0; code < 256; code++) {
		double e = (double)code / 255.0;
		struct az_ref_layer layer;
		memset(&layer, 0, sizeof(layer));
		if (!patch(&layer.src, 2, 2, e, e, e, 1.0)) {
			exit(2);
		}
		layer.domain = sdr_domain(0.0f);
		layer.opacity = 1.0;
		struct az_ref_scene sc = {2, 2, {0, 0, 0, 1}, &layer, 1};
		struct az_ref_frame f;
		if (!az_ref_render(&sc, &out, brk, &f)) {
			exit(2);
		}
		double p[4];
		az_ref_image_get(&f.final, 1, 1, p);
		for (int c = 0; c < 3; c++) {
			double d = fabs(p[c] - e);
			if (d > worst) {
				worst = d;
			}
			if (p[c] != e) {
				bad++;
			}
		}
		az_ref_frame_free(&f);
		az_ref_image_free(&layer.src);
	}
	if (brk == AZ_REF_BREAK_NONE) {
		printf("    worst |d| over 256 codes: %.3g\n", worst);
		CHECK(bad == 0,
			"gate 1: opaque sRGB through Path A is BIT-EXACT (%d of 768 "
			"channels differ)",
			bad);
	}
	*fails = bad;
}

/* Gate 1b: the same claim in float, on Path B, where nothing quantises the
 * intermediate. This is the one that would catch a decode/encode pair that is
 * self-inverse only on the 8-bit lattice. */
static void gate1b_float_identity(enum az_ref_break brk, int *fails) {
	struct az_ref_output out = output_of(&SDR10, false);
	int bad = 0;
	double worst = 0.0;
	for (int i = 0; i <= 512; i++) {
		double e = (double)i / 512.0;
		struct az_ref_layer layer;
		memset(&layer, 0, sizeof(layer));
		if (!patch(&layer.src, 1, 1, e, e, e, 1.0)) {
			exit(2);
		}
		layer.domain = sdr_domain(0.0f);
		layer.opacity = 1.0;
		struct az_ref_scene sc = {1, 1, {0, 0, 0, 1}, &layer, 1};
		struct az_ref_frame f;
		if (!az_ref_render(&sc, &out, brk, &f)) {
			exit(2);
		}
		double p[4];
		az_ref_image_get(&f.electrical, 0, 0, p);
		for (int c = 0; c < 3; c++) {
			double d = fabs(p[c] - e);
			if (d > worst) {
				worst = d;
			}
			if (d > 1e-6) {
				bad++;
			}
		}
		az_ref_frame_free(&f);
		az_ref_image_free(&layer.src);
	}
	if (brk == AZ_REF_BREAK_NONE) {
		printf("    worst |d| over 513 float samples: %.3g\n", worst);
		CHECK(bad == 0,
			"gate 1b: Path B pre-dither electrical == input within 1e-6");
	}
	*fails = bad;
}

/* ── gate 2: the translucent gate ───────────────────────────────────────── */

/*
 * ADR-005's falsifier. A 50%-alpha mid-grey over black.
 *
 * The closed form, computed here from the standard rather than from the
 * reference: the buffer holds the PREMULTIPLIED electrical texel
 * (a·E, a·E, a·E, a). Un-premultiplying recovers E; the sRGB EOTF of E is the
 * scene value; premultiplying by a and compositing over black leaves a·EOTF(E);
 * the output encodes that.
 *
 * The wrong answer -- hardware-decoding the premultiplied texel, i.e. the
 * shortcut ADR-005 rejects -- gives EOTF(a·E), which for E = 0.5 and a = 0.5
 * differs by roughly seven 8-bit codes. That is not a rounding, and it lands
 * exactly on AA edges and translucent UI, the most looked-at pixels here.
 */
static void gate2_translucent(enum az_ref_break brk, int *fails) {
	const double E = 0.5, A = 0.5;
	struct az_ref_output out = output_of(&SDR10, false);

	struct az_ref_layer layer;
	memset(&layer, 0, sizeof(layer));
	if (!patch(&layer.src, 1, 1, E * A, E * A, E * A, A)) {
		exit(2);
	}
	layer.domain = sdr_domain(0.0f);
	layer.opacity = 1.0;
	struct az_ref_scene sc = {1, 1, {0, 0, 0, 1}, &layer, 1};
	struct az_ref_frame f;
	if (!az_ref_render(&sc, &out, brk, &f)) {
		exit(2);
	}

	/* closed form, independent of the reference's ordering */
	double scene_want = A * (double)az_srgb_eotf((float)E);
	double elec_want = (double)az_srgb_ieotf((float)scene_want);

	double got_scene[4], got_elec[4];
	az_ref_image_get(&f.composite, 0, 0, got_scene);
	az_ref_image_get(&f.electrical, 0, 0, got_elec);

	int bad = 0;
	if (fabs(got_scene[0] - scene_want) > 1e-9) {
		bad++;
	}
	if (fabs(got_elec[0] - elec_want) > 1e-9) {
		bad++;
	}
	if (brk == AZ_REF_BREAK_NONE) {
		printf("    scene %.9f (closed form %.9f), electrical %.9f\n",
			got_scene[0], scene_want, got_elec[0]);
		NEAR(got_scene[0], scene_want, 1e-9,
			"gate 2: composited scene == a x EOTF(E)");
		NEAR(got_elec[0], elec_want, 1e-9,
			"gate 2: encoded output == sRGB^-1 of it");
		/* And the premise: the wrong ordering is far enough away that this
		 * assertion has teeth. */
		double wrong = (double)az_srgb_eotf((float)(A * E));
		double codes = fabs(wrong - scene_want) /
			(double)az_srgb_eotf(1.0f / 255.0f);
		CHECK(fabs(wrong - scene_want) > 0.02,
			"  premise: decoding the premultiplied texel instead misses by "
			"%.4f scene (~%.0f 8-bit codes near black)",
			fabs(wrong - scene_want), codes);
	}
	az_ref_frame_free(&f);
	az_ref_image_free(&layer.src);
	*fails = bad;
}

/* ── gate 3: PQ anchors ─────────────────────────────────────────────────── */

/*
 * ADR-008's falsifier, in unit form. A PQ source carrying a 203 cd/m2 patch
 * and a 1000 cd/m2 patch, onto the HDR output, must come back out as the PQ
 * codes for 203 and 1000 cd/m2 -- because the decode multiplies by 10000/ref
 * and the encode multiplies by ref/10000, and the two must cancel exactly.
 *
 * 203 cd/m2 is chosen because it is scene 1.0 at the default reference: it
 * sits below the tone curve's knee, so it must survive UNTOUCHED. 1000 cd/m2
 * is the panel's own peak, so it is above the knee and the curve does move it
 * -- the expected value there is computed through the same curve, and the
 * assertion is that it lands below the panel ceiling and above SDR white,
 * which is the property, rather than a number copied from the implementation.
 */
static void gate3_pq_anchors(enum az_ref_break brk, int *fails) {
	struct az_ref_output out = output_of(&HDR, false);
	const double ref = 203.0;
	int bad = 0;

	struct {
		const char *what;
		double nits;
	} PATCHES[] = {{"203 cd/m2 (diffuse white)", 203.0},
		{"100 cd/m2", 100.0}, {"1000 cd/m2 (panel peak)", 1000.0}};

	for (size_t i = 0; i < sizeof(PATCHES) / sizeof(PATCHES[0]); i++) {
		double e = (double)az_pq_ieotf((float)(PATCHES[i].nits / 10000.0));
		struct az_ref_layer layer;
		memset(&layer, 0, sizeof(layer));
		if (!patch(&layer.src, 1, 1, e, e, e, 1.0)) {
			exit(2);
		}
		layer.domain = pq_domain(203.0f);
		layer.opacity = 1.0;
		struct az_ref_scene sc = {1, 1, {0, 0, 0, 1}, &layer, 1};
		struct az_ref_frame f;
		if (!az_ref_render(&sc, &out, brk, &f)) {
			exit(2);
		}
		double scene[4], elec[4];
		az_ref_image_get(&f.composite, 0, 0, scene);
		az_ref_image_get(&f.electrical, 0, 0, elec);

		/* the scene value this patch must decode to: nits / ref */
		double scene_want = PATCHES[i].nits / ref;
		/* through the curve, then back to nits */
		double v[3] = {scene_want, scene_want, scene_want};
		double peak = (double)out.state.peak_scene;
		if (peak > 1.0) {
			double m = v[0];
			if (m > out.knee) {
				double x = m - out.knee, P = peak - out.knee;
				double fm = out.knee + P * x / (P + x);
				for (int c = 0; c < 3; c++) {
					v[c] *= fm / m;
				}
			}
		}
		double elec_want = (double)az_pq_ieotf((float)(v[0] * ref / 10000.0));

		/* RELATIVE, not absolute. The decode's error is an error in the
		 * OPTICAL value, and the domain then multiplies it by 10000/ref =
		 * 49.26 on its way into the scene -- so a 4e-6 PQ round-trip residue
		 * (findings F3) arrives as 2e-4 of scene at the 1000-nit patch. An
		 * absolute 1e-4 would be a tolerance on the scale factor, not on the
		 * pipeline. */
		double scene_tol = 1e-4 * (1.0 + scene_want);
		if (brk == AZ_REF_BREAK_NONE) {
			printf("    %-26s scene %.6f  PQ code %.6f\n", PATCHES[i].what,
				scene[0], elec[0]);
			NEAR(scene[0], scene_want, scene_tol,
				"gate 3: %s decodes to nits/ref", PATCHES[i].what);
			/* 1/1023 is the contract's bound: one 10-bit code. */
			NEAR(elec[0], elec_want, 1.0 / 1023.0,
				"gate 3: %s encodes to its PQ code", PATCHES[i].what);
		}
		if (fabs(scene[0] - scene_want) > scene_tol ||
				fabs(elec[0] - elec_want) > 1.0 / 1023.0) {
			bad++;
		}
		az_ref_frame_free(&f);
		az_ref_image_free(&layer.src);
	}

	if (brk == AZ_REF_BREAK_NONE) {
		/* The 203-nit patch is scene 1.0 exactly, i.e. AT the knee, so the
		 * tone map must not have touched it -- and its PQ code must be the
		 * BT.2408 constant, straight off the standard. */
		double e = (double)az_pq_ieotf((float)(203.0 / 10000.0));
		NEAR(e, 0.58068888, 1e-4,
			"  and that code is BT.2408's 0.5806888, from the standard");
	}
	*fails = bad;
}

/* ── gate 4: the straddling window ──────────────────────────────────────── */

/*
 * ADR-012's claim, in unit form. The same scene, the same domains, rendered
 * for an SDR output and an HDR output. Below scene 1.0 the two must agree --
 * both encode the same light, one through sRGB and one through PQ -- and the
 * assertion is on the LIGHT, recovered by decoding each output's electrical
 * value back through its own transfer function.
 *
 * Above scene 1.0 they must diverge, and the divergence must have a
 * structure: the SDR side is bounded by 1.0 and the HDR side is not.
 */
static void gate4_straddle(enum az_ref_break brk, int *fails) {
	struct az_ref_output sdr = output_of(&SDR10, false);
	struct az_ref_output hdr = output_of(&HDR, false);
	int bad = 0;
	double worst_below = 0.0;

	for (int i = 0; i <= 40; i++) {
		double scene = (double)i / 20.0; /* 0 .. 2.0 */
		/* Build the layer as an scRGB source so an arbitrary scene value is
		 * expressible without inverting a curve. */
		struct az_lum_source_desc sd = {true, AZ_TF_LINEAR_EXT, AZ_PRIM_BT709,
			0.0f};
		struct az_lum_rules rl = {0.0f, 0.0f};
		/* scRGB scale at ref 80 is exactly 1, so electrical == scene. */
		struct az_lum_domain dom = az_lum_resolve(&sd, &rl, 80.0f);

		double lin_sdr = 0.0, lin_hdr = 0.0;
		for (int which = 0; which < 2; which++) {
			struct az_ref_output *o = which == 0 ? &sdr : &hdr;
			struct az_ref_layer layer;
			memset(&layer, 0, sizeof(layer));
			double e = scene > 1.0 ? 1.0 : scene;
			if (!patch(&layer.src, 1, 1, e, e, e, 1.0)) {
				exit(2);
			}
			layer.domain = dom;
			layer.opacity = 1.0;
			struct az_ref_scene sc = {1, 1, {0, 0, 0, 1}, &layer, 1};
			struct az_ref_frame f;
			if (!az_ref_render(&sc, o, brk, &f)) {
				exit(2);
			}
			double p[4];
			az_ref_image_get(&f.electrical, 0, 0, p);
			/* back to light: the SDR output's code is relative, the HDR
			 * output's is absolute -- divide out the anchor to compare. */
			double lin = which == 0
				? (double)az_srgb_eotf((float)p[0])
				: (double)az_pq_eotf((float)p[0]) * 10000.0 /
					(double)o->state.ref_nits;
			if (which == 0) {
				lin_sdr = lin;
			} else {
				lin_hdr = lin;
			}
			az_ref_frame_free(&f);
			az_ref_image_free(&layer.src);
		}

		if (scene <= 1.0) {
			double d = fabs(lin_sdr - lin_hdr);
			if (d > worst_below) {
				worst_below = d;
			}
			if (d > 2e-3) {
				bad++;
			}
		} else if (lin_sdr > 1.0 + 1e-6) {
			bad++; /* the SDR side must be bounded by SDR white */
		}
	}
	if (brk == AZ_REF_BREAK_NONE) {
		printf("    worst light divergence below scene 1.0: %.3g\n",
			worst_below);
		CHECK(bad == 0,
			"gate 4: SDR and HDR renderings of one scene agree below 1.0 and "
			"the SDR side stays bounded above it");
	}
	*fails = bad;
}

/* ── gate 5: tone map and gamut vectors ─────────────────────────────────── */

static void gate5_tonemap_gamut(enum az_ref_break brk, int *fails) {
	struct az_ref_output hdr = output_of(&HDR, false);
	int bad = 0;

	/* ADR-009 (i): mid-grey invariance. Scene 0.18 is far below the knee, so
	 * it must encode to exactly PQ^-1(0.18 x ref / 10000) -- any drift means
	 * the knee is not at 1.0 or the anchor is misplaced. */
	double rgb[3] = {0.18, 0.18, 0.18}, e[3];
	az_ref_encode_scene(&hdr, rgb, e);
	double want = (double)az_pq_ieotf((float)(0.18 * 203.0 / 10000.0));
	if (fabs(e[0] - want) > 1e-6) {
		bad++;
	}

	/* ADR-009 (ii): hue. A saturated red at scene (4.0, 0.4, 0.4) must keep
	 * its ratios through the curve. The check is on the SCENE values after
	 * the curve, before the gamut matrix -- the matrix legitimately changes
	 * ratios, and folding the two would make the assertion meaningless. */
	double red[3] = {4.0, 0.4, 0.4};
	float v[3] = {(float)red[0], (float)red[1], (float)red[2]};
	az_tonemap(v, 1.0f, hdr.state.peak_scene);
	double ratio = (double)v[0] / (double)v[1];
	if (fabs(ratio - 10.0) > 1e-3) {
		bad++;
	}

	/* ADR-010: a BT.709-boundary colour must survive the 709->2020 matrix
	 * and come back. Pure 709 red is inside 2020, so no clamp fires. */
	double r709[3] = {1.0, 0.0, 0.0}, in2020[3], back[3];
	{
		float m[9];
		az_mat_from_primaries(&AZ_PRIMARIES_BT709, &AZ_PRIMARIES_BT2020, m);
		for (int i = 0; i < 3; i++) {
			in2020[i] = (double)m[i * 3 + 0] * r709[0] +
				(double)m[i * 3 + 1] * r709[1] +
				(double)m[i * 3 + 2] * r709[2];
		}
		float mi[9];
		az_mat_from_primaries(&AZ_PRIMARIES_BT2020, &AZ_PRIMARIES_BT709, mi);
		for (int i = 0; i < 3; i++) {
			back[i] = (double)mi[i * 3 + 0] * in2020[0] +
				(double)mi[i * 3 + 1] * in2020[1] +
				(double)mi[i * 3 + 2] * in2020[2];
		}
	}
	for (int i = 0; i < 3; i++) {
		if (fabs(back[i] - r709[i]) > 1e-5) {
			bad++;
		}
	}
	int neg = 0;
	for (int i = 0; i < 3; i++) {
		if (in2020[i] < 0.0) {
			neg++;
		}
	}
	if (neg != 0) {
		bad++;
	}

	if (brk == AZ_REF_BREAK_NONE) {
		NEAR(e[0], want, 1e-6,
			"gate 5: scene 0.18 encodes to PQ^-1(0.18 ref/10000) exactly");
		NEAR(ratio, 10.0, 1e-3, "gate 5: hue survives the tone curve");
		CHECK(neg == 0, "gate 5: 709 red is non-negative in 2020 (no clamp)");
		NEAR(back[0], 1.0, 1e-5, "gate 5: and round-trips to 709 red");
	}
	*fails = bad;
}

/* ── gate 6: the reference-luminance sweep ──────────────────────────────── */

/*
 * ADR-003's falsifier, in unit form and end to end. Sweep the scene reference
 * 80 -> 400 with a PQ source on the HDR output: the ABSOLUTE luminance of the
 * content must not move, because the decode's 10000/ref and the encode's
 * ref/10000 cancel. The SDR content on the same output must move with the
 * sweep, because that is what the reference IS.
 *
 * THE CANCELLATION ONLY SURVIVES BELOW THE KNEE, and that is a finding rather
 * than a caveat. ADR-009 puts the tone curve's knee at scene 1.0, which IS the
 * reference -- so moving the reference moves the knee in absolute terms, and a
 * patch above it is compressed by a different amount at every reference. The
 * ADR-003 falsifier as written ("the 1000-nit patch must hold constant while
 * sweeping 80 -> 400") is therefore not satisfiable together with ADR-009 on a
 * 1000-nit panel. Measured, at a 400 cd/m2 patch: 317 cd/m2 at ref 80, rising
 * to 400 at ref 400. See docs/m5-hdr/opus-findings.md, F6.
 *
 * So this gate asserts the invariance where the architecture actually claims
 * it -- a patch below the knee at every reference in the sweep -- and asserts
 * the above-knee behaviour as the monotone dependence it is, which is still a
 * falsifiable statement and still catches every break.
 */
static void gate6_reference_sweep(enum az_ref_break brk, int *fails) {
	const double refs[] = {80.0, 120.0, 203.0, 300.0, 400.0};
	const double PATCH_NITS = 50.0; /* below scene 1.0 at every ref above */
	double pq_nits[5], sdr_nits[5], hi_nits[5];
	int bad = 0;

	for (size_t i = 0; i < 5; i++) {
		struct az_output_desc d = HDR;
		d.scene_ref_nits = (float)refs[i];
		struct az_ref_output out = output_of(&d, false);

		for (int which = 0; which < 3; which++) {
			struct az_ref_layer layer;
			memset(&layer, 0, sizeof(layer));
			double e;
			struct az_lum_domain dom;
			if (which == 0) {
				/* a PQ patch BELOW the knee at every reference */
				e = (double)az_pq_ieotf((float)(PATCH_NITS / 10000.0));
				dom = pq_domain((float)refs[i]);
			} else if (which == 1) {
				/* SDR white */
				e = 1.0;
				dom = sdr_domain(0.0f);
			} else {
				/* a 400 cd/m2 patch, ABOVE the knee at the low references --
				 * the F6 evidence */
				e = (double)az_pq_ieotf((float)(400.0 / 10000.0));
				dom = pq_domain((float)refs[i]);
			}
			if (!patch(&layer.src, 1, 1, e, e, e, 1.0)) {
				exit(2);
			}
			layer.domain = dom;
			layer.opacity = 1.0;
			struct az_ref_scene sc = {1, 1, {0, 0, 0, 1}, &layer, 1};
			struct az_ref_frame f;
			if (!az_ref_render(&sc, &out, brk, &f)) {
				exit(2);
			}
			double p[4];
			az_ref_image_get(&f.electrical, 0, 0, p);
			double nits = (double)az_pq_eotf((float)p[0]) * 10000.0;
			if (which == 0) {
				pq_nits[i] = nits;
			} else if (which == 1) {
				sdr_nits[i] = nits;
			} else {
				hi_nits[i] = nits;
			}
			az_ref_frame_free(&f);
			az_ref_image_free(&layer.src);
		}
	}

	for (size_t i = 0; i < 5; i++) {
		/* Below the knee the decode's 10000/ref and the encode's ref/10000
		 * cancel exactly, so the absolute luminance must hold. 1 cd/m2 is
		 * generous: one 10-bit PQ code at 50 cd/m2 is about 0.35 cd/m2. */
		if (fabs(pq_nits[i] - PATCH_NITS) > 1.0) {
			bad++;
		}
		if (fabs(sdr_nits[i] - refs[i]) > 2.0) {
			bad++;
		}
		/* Above the knee it must NOT hold, and it must move the right way:
		 * a higher reference means the knee sits higher in absolute terms, so
		 * the same patch is compressed less. Monotone non-decreasing, and
		 * never above the patch's own luminance. */
		if (i > 0 && hi_nits[i] < hi_nits[i - 1] - 1.0) {
			bad++;
		}
		if (hi_nits[i] > 400.0 + 1.0) {
			bad++;
		}
	}
	if (brk == AZ_REF_BREAK_NONE) {
		for (size_t i = 0; i < 5; i++) {
			printf("    ref %5.0f   50 cd/m2 patch %7.2f   SDR white %7.2f"
				   "   400 cd/m2 patch %7.2f\n",
				refs[i], pq_nits[i], sdr_nits[i], hi_nits[i]);
		}
		CHECK(bad == 0,
			"gate 6: below the knee HDR content holds its absolute nits while "
			"SDR white tracks the reference");
		CHECK(hi_nits[0] < hi_nits[4] - 10.0,
			"  and ABOVE the knee it does not (%.0f -> %.0f cd/m2) -- ADR-003's "
			"falsifier as written is not satisfiable with ADR-009's knee at "
			"scene 1.0; see findings F6",
			hi_nits[0], hi_nits[4]);
	}
	*fails = bad;
}

/* ── gate 7: PQ never internal ──────────────────────────────────────────── */

/*
 * A grep gate, blunt and effective. Invariant 1 says PQ is an OUTPUT ENCODING:
 * the only shader permitted to CALL az_pq_ieotf() is the output-encode pass.
 * color.glsl defines it, which is not a call; every other shader must not
 * mention it.
 */
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

/*
 * The permitted files, and ONLY these two:
 *   output_encode.frag  the output pass itself (ADR-008)
 *   color.glsl          which DEFINES az_pq_ieotf; defining is not calling,
 *                       and every shader that includes it is checked for a
 *                       call rather than for the include
 */
static bool pq_exempt(const char *name) {
	return strcmp(name, "output_encode.frag") == 0 ||
		strcmp(name, "color.glsl") == 0;
}

/*
 * The directory is SCANNED, not listed. A hardcoded list is a gate that stops
 * covering the next shader somebody adds, which is precisely the shape of a
 * test that quietly stops testing -- and the file being added is exactly when
 * a PQ encode would appear in the wrong place.
 */
static void gate7_pq_never_internal(const char *root) {
	printf("gate 7: PQ appears in no shader but the output pass\n");
	char dirpath[1024];
	snprintf(dirpath, sizeof(dirpath), "%s/src/render/vulkan/shader/src",
		root);
	DIR *d = opendir(dirpath);
	if (d == NULL) {
		CHECK(0, "  cannot open %s", dirpath);
		return;
	}
	int offenders = 0, seen = 0, exempt = 0;
	struct dirent *ent;
	while ((ent = readdir(d)) != NULL) {
		const char *dot = strrchr(ent->d_name, '.');
		if (dot == NULL || (strcmp(dot, ".frag") != 0 &&
							   strcmp(dot, ".vert") != 0 &&
							   strcmp(dot, ".glsl") != 0)) {
			continue;
		}
		if (pq_exempt(ent->d_name)) {
			exempt++;
			continue;
		}
		char path[2048];
		snprintf(path, sizeof(path), "%s/%s", dirpath, ent->d_name);
		char *s = slurp(path);
		if (s == NULL) {
			continue;
		}
		seen++;
		/* The call, and the constant. A shader that open-codes ST 2084's m2
		 * rather than calling the shared function is the same violation with
		 * the grep filed off. */
		if (strstr(s, "az_pq_ieotf") != NULL || strstr(s, "78.84375") != NULL) {
			printf("    %s contains a PQ encode\n", ent->d_name);
			offenders++;
		}
		free(s);
	}
	closedir(d);
	CHECK(seen >= 10, "  premise: scanned %d shader sources (%d exempt)", seen,
		exempt);
	CHECK(exempt == 2,
		"  premise: exactly the two permitted files exist (%d)", exempt);
	CHECK(offenders == 0,
		"no PQ inverse EOTF outside the output pass (%d offenders)",
		offenders);

	/* And the premise that the gate can fail at all: the exempt file really
	 * does contain what everything else is forbidden. */
	char enc[2048];
	snprintf(enc, sizeof(enc), "%s/output_encode.frag", dirpath);
	char *e = slurp(enc);
	CHECK(e != NULL && strstr(e, "az_pq_ieotf") != NULL,
		"  premise: output_encode.frag DOES call az_pq_ieotf");
	free(e);

	/*
	 * ── C7: THE GAMUT MATRIX EXISTS TWICE, SO PIN IT ──────────────────────
	 *
	 * color.glsl carries BT.2020 -> BT.709 as nine literals because the source
	 * conversion has to happen in the shader and nine floats do not fit in
	 * push.glsl's 128 bytes. That is a duplicate of az_color.c's
	 * AZ_MAT_2020_TO_709, and a duplicate nobody checks is a duplicate that
	 * drifts -- silently, because a slightly wrong gamut matrix renders a
	 * completely plausible picture.
	 *
	 * So the literals are parsed back out of the shader source and compared.
	 * Text-level, like the PQ scan above, and for the same reason: it needs no
	 * device and it cannot be satisfied by the implementation agreeing with
	 * itself.
	 */
	char glsl[2048];
	snprintf(glsl, sizeof(glsl), "%s/color.glsl", dirpath);
	char *g = slurp(glsl);
	CHECK(g != NULL, "  premise: color.glsl is readable");
	if (g != NULL) {
		double got[9];
		int n = 0;
		for (int row = 0; row < 3 && n == row * 3; row++) {
			char key[64];
			snprintf(key, sizeof(key), "AZ_2020_TO_709_R%d = vec3(", row);
			const char *at = strstr(g, key);
			if (at == NULL) {
				break;
			}
			at += strlen(key);
			for (int c = 0; c < 3; c++) {
				got[row * 3 + c] = strtod(at, (char **)&at);
				while (*at == ' ' || *at == ',') {
					at++;
				}
				n++;
			}
		}
		CHECK(n == 9, "  premise: parsed 9 literals from color.glsl (%d)", n);
		double worst = 0.0;
		for (int i = 0; i < n; i++) {
			double d = got[i] - (double)AZ_MAT_2020_TO_709[i];
			if (d < 0.0) { d = -d; }
			if (d > worst) { worst = d; }
		}
		CHECK(n == 9 && worst < 1e-7,
			"color.glsl's BT.2020->BT.709 matches az_color.c (worst %.3g)",
			worst);
		free(g);
	}
}

/* ── the breaks ─────────────────────────────────────────────────────────── */

/*
 * Which gates each break is expected to fail. A break that fails NOTHING is a
 * suite failure: it means the gate it was written against can no longer
 * detect it, which is how a test quietly stops testing.
 */
static void run_breaks(void) {
	printf("the breaks: each must FAIL at least one gate\n");
	for (int b = AZ_REF_BREAK_NONE + 1; b < AZ_REF_BREAK_COUNT; b++) {
		int f1 = 0, f1b = 0, f2 = 0, f3 = 0, f4 = 0, f5 = 0, f6 = 0, f8 = 0,
			f9 = 0;
		gate1_sdr_round_trip((enum az_ref_break)b, &f1);
		gate1b_float_identity((enum az_ref_break)b, &f1b);
		gate2_translucent((enum az_ref_break)b, &f2);
		gate3_pq_anchors((enum az_ref_break)b, &f3);
		gate4_straddle((enum az_ref_break)b, &f4);
		gate5_tonemap_gamut((enum az_ref_break)b, &f5);
		gate6_reference_sweep((enum az_ref_break)b, &f6);
		gate8_dither((enum az_ref_break)b, &f8);
		gate9_encoded_blur((enum az_ref_break)b, &f9);
		int total = f1 + f1b + f2 + f3 + f4 + f5 + f6 + f8 + f9;
		printf("    %-38s 1:%-3d 1b:%-3d 2:%-2d 3:%-2d 4:%-3d 6:%-3d 8:%-2d "
			   "9:%-2d\n",
			az_ref_break_name((enum az_ref_break)b), f1, f1b, f2, f3, f4, f6,
			f8, f9);
		CHECK(total > 0, "break \"%s\" is caught",
			az_ref_break_name((enum az_ref_break)b));
	}
}

/* ── the dither break needs its own oracle ──────────────────────────────── */

/*
 * AZ_REF_BREAK_DITHER_EARLY does not move a flat patch far enough to trip the
 * gates above -- it is a per-pixel amplitude error, and the gates all read one
 * pixel of a flat patch. The property it breaks is that the dither is
 * ZERO-MEAN AT THE OUTPUT QUANTUM: perturbing a scene value by one output code
 * is a different amount of output at every level, so the dithered mean drifts
 * away from the value that was there in the shadows and vanishes in the
 * highlights.
 *
 * THE ORACLE IS THE PRE-DITHER ELECTRICAL VALUE, not the undithered output.
 * That distinction is the whole idea of dither: the undithered output is the
 * true value ROUNDED, up to half a code away by construction, and comparing
 * against it would score correct dither as a 0.24-code error -- which is what
 * this test did on its first run. Dither exists so that the MEAN of the
 * quantised pixels recovers the unquantised value, so the unquantised value is
 * what it must be measured against.
 *
 * Measuring that needs an area, not a pixel, which is why this gate is
 * separate rather than shoehorned into the loop above.
 */
static void gate8_dither(enum az_ref_break brk, int *fails) {
	const int N = 64;
	const double scene_level = 0.02; /* dark, where PQ banding lives */
	struct az_ref_output on = output_of(&HDR, true);
	struct az_ref_output off = output_of(&HDR, false);

	struct az_lum_source_desc sd = {true, AZ_TF_LINEAR_EXT, AZ_PRIM_BT709,
		0.0f};
	struct az_lum_rules rl = {0.0f, 0.0f};
	struct az_lum_domain dom = az_lum_resolve(&sd, &rl, 80.0f);

	struct az_ref_layer layer;
	memset(&layer, 0, sizeof(layer));
	if (!patch(&layer.src, N, N, scene_level, scene_level, scene_level, 1.0)) {
		exit(2);
	}
	layer.domain = dom;
	layer.opacity = 1.0;
	struct az_ref_scene sc = {N, N, {0, 0, 0, 1}, &layer, 1};

	struct az_ref_frame fon, foff;
	if (!az_ref_render(&sc, &on, brk, &fon) ||
			!az_ref_render(&sc, &off, brk, &foff)) {
		exit(2);
	}
	double mean_on = 0.0, mean_true = 0.0, lo = 1e9, hi = -1e9;
	for (int y = 0; y < N; y++) {
		for (int x = 0; x < N; x++) {
			double p[4], q[4];
			az_ref_image_get(&fon.final, x, y, p);
			/* the UNQUANTISED electrical value -- see the comment above */
			az_ref_image_get(&foff.electrical, x, y, q);
			mean_on += p[0];
			mean_true += q[0];
			if (p[0] < lo) {
				lo = p[0];
			}
			if (p[0] > hi) {
				hi = p[0];
			}
		}
	}
	mean_on /= (double)(N * N);
	mean_true /= (double)(N * N);
	double drift_codes = fabs(mean_on - mean_true) * 1023.0;
	double spread_codes = (hi - lo) * 1023.0;
	az_ref_frame_free(&fon);
	az_ref_frame_free(&foff);
	az_ref_image_free(&layer.src);

	/* TWO properties, because the break fails only the second. ADR-011 asks
	 * for noise that is zero-mean AND one code peak-to-peak. Dithering the
	 * scene instead of the electrical value keeps the mean nearly right --
	 * 0.03 of a code -- while spraying the output over three codes, because
	 * one output code is not a constant amount of scene and PQ near black is
	 * where that gap is widest. A mean-only oracle scores it as correct. */
	int bad = 0;
	if (drift_codes > 0.1) {
		bad++;
	}
	if (spread_codes > 1.2 || spread_codes < 0.5) {
		bad++;
	}
	if (brk == AZ_REF_BREAK_NONE) {
		printf("gate 8: dither is zero-mean AND one code peak-to-peak "
			   "(ADR-011)\n");
		printf("    mean drift from the true value %.4f codes, spread %.2f "
			   "codes\n",
			drift_codes, spread_codes);
		CHECK(drift_codes <= 0.1,
			"correct dither recovers the unquantised value to within 0.1 of "
			"a code");
		CHECK(spread_codes >= 0.5 && spread_codes <= 1.2,
			"and it is one code peak-to-peak, not three");
	}
	*fails = bad;
}

/* ── the encoded-blur break needs an area too ───────────────────────────── */

/*
 * The shadow-glow scar, as a test. Averaging ENCODED values and reading the
 * average back as light is not the same as averaging light: the sRGB EOTF is
 * convex, so decoding the mean of two extremes lands well BELOW the mean of
 * the decodes. A one-pixel black/white checkerboard makes it extreme --
 * averaging the codes gives 0.5, which decodes to 0.214, against the true
 * light mean of 0.5. A 57% error, on the most ordinary content there is.
 *
 * (The f8be42c episode was the mirror image of the same identity: values that
 * were already encoded, blurred as though they were light, so the result came
 * out LIGHTER than the backdrop. Which direction the error runs depends on
 * which side of the curve the blur is standing; that it is large depends only
 * on the content having contrast.)
 *
 * A FLAT backdrop is blind to all of it, in either direction. That is the
 * lesson of f8be42c, and it is asserted below rather than remembered.
 */
static void gate9_encoded_blur(enum az_ref_break brk, int *fails) {
	const int N = 32;
	struct az_ref_layer layer;
	memset(&layer, 0, sizeof(layer));
	if (!az_ref_image_init(&layer.src, N, N)) {
		exit(2);
	}
	for (int y = 0; y < N; y++) {
		for (int x = 0; x < N; x++) {
			/* one-pixel checkerboard: white on black, the highest spatial
			 * frequency the raster can hold */
			double v = ((x + y) & 1) ? 1.0 : 0.0;
			double p[4] = {v, v, v, 1.0};
			az_ref_image_set(&layer.src, x, y, p);
		}
	}
	layer.domain = sdr_domain(0.0f);
	layer.opacity = 1.0;
	struct az_ref_scene sc = {N, N, {0, 0, 0, 1}, &layer, 1};

	struct az_ref_output out = output_of(&SDR10, false);
	out.blur_radius = 3;

	struct az_ref_frame f;
	if (!az_ref_render(&sc, &out, brk, &f)) {
		exit(2);
	}
	double mean = 0.0;
	int n = 0;
	for (int y = 8; y < N - 8; y++) {
		for (int x = 8; x < N - 8; x++) {
			double a[4];
			az_ref_image_get(&f.blurred, x, y, a);
			mean += a[0];
			n++;
		}
	}
	mean /= n;
	az_ref_frame_free(&f);

	/* The true mean of half white, half black IN LIGHT is 0.5. That number
	 * comes from the content, not from the pipeline, which is what makes it
	 * an oracle. */
	int bad = fabs(mean - 0.5) > 0.05 ? 1 : 0;

	if (brk == AZ_REF_BREAK_NONE) {
		printf("gate 9: a blur is arithmetic on light, not on codes\n");
		printf("    linear blur mean %.5f (true light mean 0.5)\n", mean);
		NEAR(mean, 0.5, 0.02, "the linear blur lands on the true mean");

		/* The flat-backdrop blindness, asserted rather than remembered: the
		 * same two blurs over a UNIFORM patch agree exactly, so a test built
		 * on one would have passed the broken code. */
		struct az_ref_layer flat;
		memset(&flat, 0, sizeof(flat));
		if (!patch(&flat.src, N, N, 0.5, 0.5, 0.5, 1.0)) {
			exit(2);
		}
		flat.domain = sdr_domain(0.0f);
		flat.opacity = 1.0;
		struct az_ref_scene fsc = {N, N, {0, 0, 0, 1}, &flat, 1};
		struct az_ref_frame flin, fenc;
		if (!az_ref_render(&fsc, &out, AZ_REF_BREAK_NONE, &flin) ||
				!az_ref_render(&fsc, &out, AZ_REF_BREAK_ENCODED_BLUR, &fenc)) {
			exit(2);
		}
		double d = az_ref_max_abs_diff(&flin.blurred, &fenc.blurred, true);
		CHECK(d < 1e-6,
			"  and a FLAT backdrop cannot tell the two apart (|d| = %.3g) -- "
			"which is why the fixture is a checkerboard",
			d);
		az_ref_frame_free(&flin);
		az_ref_frame_free(&fenc);
		az_ref_image_free(&flat.src);
	}
	az_ref_image_free(&layer.src);
	*fails = bad;
}

int main(int argc, char **argv) {
	const char *root = argc > 1 ? argv[1] : ".";
	printf("== C4 the M5 pipeline gate ==\n");

	int f;
	printf("gate 1: SDR round trip\n");
	gate1_sdr_round_trip(AZ_REF_BREAK_NONE, &f);
	gate1b_float_identity(AZ_REF_BREAK_NONE, &f);
	printf("gate 2: translucent\n");
	gate2_translucent(AZ_REF_BREAK_NONE, &f);
	printf("gate 3: PQ anchors\n");
	gate3_pq_anchors(AZ_REF_BREAK_NONE, &f);
	printf("gate 4: straddling window\n");
	gate4_straddle(AZ_REF_BREAK_NONE, &f);
	printf("gate 5: tone map and gamut\n");
	gate5_tonemap_gamut(AZ_REF_BREAK_NONE, &f);
	printf("gate 6: reference sweep\n");
	gate6_reference_sweep(AZ_REF_BREAK_NONE, &f);
	gate7_pq_never_internal(root);
	gate8_dither(AZ_REF_BREAK_NONE, &f);
	gate9_encoded_blur(AZ_REF_BREAK_NONE, &f);
	run_breaks();

	printf("\n%d checks, %d failures\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
