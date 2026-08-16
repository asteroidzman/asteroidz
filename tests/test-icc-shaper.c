/*
 * M6B gate G1 — ICC matrix-shaper ingest, checked against lcms2 itself.
 *
 * The claim: the matrix+curve reduction reproduces the profile's own
 * linear->device transform. The check runs our shaper and lcms2's full
 * transform over the same grid and compares.
 *
 * THE PREMISE COMES FIRST, and it is not decoration. A gate that would also
 * pass on an identity transform proves nothing -- that is the F15 coincidence,
 * where a falsifier moved zero codes because the thing it perturbed was
 * invariant on the content being measured. So this asserts the profile is
 * measurably non-identity BEFORE trusting any agreement, and records the
 * figure that G2's falsifier expects to see as its red.
 *
 * Measured for AORUS FI32U-2 (the display this was written for): worst 100
 * codes over a 16^3 RGB grid, 99.2% of samples moving more than one code --
 * against 9 codes on the neutral axis alone. An oracle built on greys would
 * have understated it elevenfold, which is the whole reason the grid below is
 * not a ramp.
 */
#include <lcms2.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/render/color/az_icc.h"
/*
 * The synthetic profiles, compiled in rather than shelled out to. These are the
 * profiles this machine does not have and cannot get -- above all a cLUT
 * display profile -- and without them D2's REFUSALS are untested code that has
 * only ever been reasoned about.
 */
#define AZ_ICC_SYNTH_NO_MAIN
#include "../contrib/icc-synth.c"

static int checks, failures;

static void ok(bool cond, const char *what) {
	checks++;
	if (cond) {
		printf("  ok   %s\n", what);
	} else {
		failures++;
		printf("  FAIL %s\n", what);
	}
}

static const char *PROFILE = "/home/ralf/FI32U.icm";

static bool slurp(const char *path, void **data, size_t *size) {
	FILE *f = fopen(path, "rb");
	if (f == NULL) {
		return false;
	}
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n <= 0) {
		fclose(f);
		return false;
	}
	*data = malloc((size_t)n);
	*size = fread(*data, 1, (size_t)n, f);
	fclose(f);
	return *size == (size_t)n;
}

/*
 * NO ENCODE HERE, DELIBERATELY. az_icc_apply already returns DEVICE CODE --
 * the curve's output is the encode -- and cmsDoTransform with TYPE_RGB_FLT
 * returns device code too. The first version of this test ran an sRGB encode
 * over our result before comparing, encoding twice, and reported 74 codes of
 * disagreement against a reduction that was correct. The matrix was never
 * wrong; the comparison was.
 */

/*
 * ── M6C GATE: THE cLUT FORM, ON THE CPU ───────────────────────────────────
 *
 * G1 above answers "does the matrix+curve reduction reproduce lcms2". This
 * answers the same question for the OTHER form, and it is a separate function
 * because it is NOT display-specific: it runs off the synthesised cLUT profile
 * and must therefore run on any machine, including the ones where G1 skips.
 *
 * WHAT IT CANNOT ESTABLISH, said here rather than discovered later: the
 * compositor samples the cube through wlr_color_transform_eval, and this
 * rebuilds the equivalent lcms2 transform by hand -- linear-sRGB source, the
 * profile as destination, relative colorimetric -- because the test binary
 * deliberately does not link wlroots. If wlroots ever changed the source
 * profile it builds from (render/color_lcms2.c), this gate would keep passing
 * against the old meaning. contrib/m6b-icc-drive-test.sh covers that seam, by
 * running a real compositor.
 */
static const cmsCIExyY G_SRGB_WP = { 0.3127, 0.3291, 1 };
static const cmsCIExyYTRIPLE G_SRGB_PRIM = {
	.Red = { 0.64, 0.33, 1 }, .Green = { 0.3, 0.6, 1 },
	.Blue = { 0.15, 0.06, 1 },
};

/* wlroots' source profile: sRGB primaries, GAMMA-1.0 TRCs. The gamma is the
 * whole domain statement -- it is what makes the transform's input axis linear
 * light rather than sRGB code. */
static cmsHPROFILE linear_srgb_profile(void) {
	cmsToneCurve *g1 = cmsBuildGamma(0, 1);
	if (g1 == NULL) {
		return NULL;
	}
	cmsToneCurve *tf[3] = { g1, g1, g1 };
	cmsHPROFILE h = cmsCreateRGBProfile(&G_SRGB_WP, &G_SRGB_PRIM, tf);
	cmsFreeToneCurve(g1);
	return h;
}

struct clut_cargo {
	cmsHTRANSFORM t;
	/* AZ_BREAK_CLUT_DOMAIN's CPU twin: build the cube as though its input axis
	 * were the sRGB-ENCODED value. Nothing else changes. */
	bool encoded_domain;
	/* The layout falsifier: fill the cube with the channels transposed. */
	bool transposed;
};

static float srgb_ieotf(float v) {
	if (v <= 0.0031308f) {
		return v * 12.92f;
	}
	return 1.055f * powf(v, 1.0f / 2.4f) - 0.055f;
}

static void clut_eval_icc(void *user, const float in[3], float out[3]) {
	struct clut_cargo *c = user;
	float x[3] = { in[0], in[1], in[2] };
	if (c->encoded_domain) {
		for (int i = 0; i < 3; i++) {
			x[i] = srgb_ieotf(x[i]);
		}
	}
	cmsDoTransform(c->t, x, out, 1);
	if (c->transposed) {
		float tmp = out[0];
		out[0] = out[2];
		out[2] = tmp;
	}
}

/* Deliberately asymmetric per channel, so a transposed grid cannot look right.
 * out = (r, g/2, b/4) has a different slope on every axis. */
static void clut_eval_slopes(void *user, const float in[3], float out[3]) {
	(void)user;
	out[0] = in[0];
	out[1] = in[1] * 0.5f;
	out[2] = in[2] * 0.25f;
}

static void gate_clut(void) {
	printf("== M6C: the cLUT form, against lcms2 ==\n");

	/* ── the grid's LAYOUT, before anything that depends on it ─────────── */
	{
		struct az_icc_clut *c = az_icc_clut_build(9, clut_eval_slopes, NULL);
		ok(c != NULL, "a cube builds");
		if (c == NULL) {
			return;
		}
		/*
		 * ON a grid point, so this is a lookup and not an interpolation: the
		 * question here is purely "did r, g and b land on the axes they were
		 * written to". A symmetric evaluator would answer yes to a cube filled
		 * B-fastest, which is the mistake this exists to catch -- and the one a
		 * picture would show as a plausible colour cast.
		 */
		float in[3] = { 1.0f, 1.0f, 1.0f };
		float out[3];
		az_icc_clut_apply(c, in, out);
		ok(fabsf(out[0] - 1.0f) < 1e-3f && fabsf(out[1] - 0.5f) < 1e-3f
			&& fabsf(out[2] - 0.25f) < 1e-3f,
			"R, G and B land on their own axes (1.0 -> 1, 0.5, 0.25)");
		/*
		 * ── AND THE WARP, WHICH THE BUILDER AND THE READER MUST AGREE ON ──
		 *
		 * Sample 4 of a 9-cube holds the transform at (4/8)^2 = 0.25, so
		 * reading the cube at 0.25 must land exactly on it -- sqrt(0.25)*8 = 4
		 * -- and return the evaluator's own answer with no interpolation at
		 * all.
		 *
		 * A READER THAT FORGOT THE WARP would index 0.25*8 = 2 and take sample
		 * 2, which holds (2/8)^2 = 0.0625: 0.19 away on the R axis. That is the
		 * discrimination this assertion has, and it is the reason the check is
		 * a specific number at a specific point rather than "close to right".
		 */
		float on[3] = { 0.25f, 0.25f, 0.25f };
		az_icc_clut_apply(c, on, out);
		ok(fabsf(out[0] - 0.25f) < 1e-3f && fabsf(out[1] - 0.125f) < 1e-3f
			&& fabsf(out[2] - 0.0625f) < 1e-3f,
			"the builder's warp and the reader's index are the same warp");
		az_icc_clut_free(c);
	}

	/* ── the cube built from the synthesised cLUT profile ──────────────── */
	const char *dir = getenv("TMPDIR");
	char path[512];
	snprintf(path, sizeof(path), "%s/az-m6c-clut.icc",
		dir != NULL ? dir : "/tmp");
	if (!make_clut(path)) {
		ok(false, "PREMISE: the synthetic cLUT profile is written");
		return;
	}
	void *data = NULL;
	size_t size = 0;
	if (!slurp(path, &data, &size)) {
		ok(false, "PREMISE: the synthetic cLUT profile reads back");
		return;
	}

	/*
	 * THE PREMISE THAT PUTS THIS FILE ON THE cLUT PATH AT ALL. If the profile
	 * reduced, the compositor would carry it as a matrix and a curve and none
	 * of what follows would ever run.
	 */
	struct az_icc_shaper sh;
	ok(az_icc_load_shaper(data, size, true, &sh) == AZ_ICC_REJECT_CLUT,
		"PREMISE: this profile does NOT reduce -- it is the cLUT path or "
		"nothing");

	cmsHPROFILE dst = cmsOpenProfileFromMem(data, (cmsUInt32Number)size);
	cmsHPROFILE lin = linear_srgb_profile();
	cmsHTRANSFORM t = (dst != NULL && lin != NULL)
		? cmsCreateTransform(lin, TYPE_RGB_FLT, dst, TYPE_RGB_FLT,
			INTENT_RELATIVE_COLORIMETRIC, cmsFLAGS_NOCACHE)
		: NULL;
	ok(t != NULL, "lcms2 builds the linear->ICC transform wlroots would");
	if (t == NULL) {
		free(data);
		return;
	}

	struct clut_cargo cargo = { .t = t };
	struct az_icc_clut *cube = az_icc_clut_build(AZ_ICC_CLUT_DIM,
		clut_eval_icc, &cargo);
	ok(cube != NULL, "the 65-cube builds from it");
	if (cube == NULL) {
		free(data);
		return;
	}

	/*
	 * OFF-GRID SAMPLES, and not by accident. A cube evaluated at its own grid
	 * points is a table lookup and would agree with itself whatever the
	 * interpolation did. 17^3 over [0,1] lands between the 33 grid's samples
	 * everywhere except the even indices, which is where trilinear error lives.
	 *
	 * NOT A NEUTRAL RAMP either, for G1's reason one level up: this profile's
	 * primaries are wide and its gamma is 2.6, and both of those are far more
	 * visible off the neutral axis.
	 */
	const int N = 17;
	double worst_agree = 0.0, worst_ident = 0.0;
	for (int ri = 0; ri < N; ri++) {
		for (int gi = 0; gi < N; gi++) {
			for (int bi = 0; bi < N; bi++) {
				float in[3] = {
					(float)ri / (N - 1) * 0.97f + 0.011f,
					(float)gi / (N - 1) * 0.97f + 0.011f,
					(float)bi / (N - 1) * 0.97f + 0.011f,
				};
				float ref[3], got[3];
				cmsDoTransform(t, in, ref, 1);
				az_icc_clut_apply(cube, in, got);
				for (int c = 0; c < 3; c++) {
					double d = fabs((double)got[c] - (double)ref[c]);
					if (d > worst_agree) {
						worst_agree = d;
					}
					/* What the same scene value would have been on an
					 * unprofiled output: a plain sRGB encode. */
					double e = fabs((double)ref[c]
						- (double)srgb_ieotf(in[c]));
					if (e > worst_ident) {
						worst_ident = e;
					}
				}
			}
		}
	}
	printf("  the profile moves the grid by up to %.1f codes away from a "
		"plain sRGB encode\n", worst_ident * 255.0);
	ok(worst_ident * 255.0 > 20.0,
		"PREMISE: the cLUT profile is measurably NON-IDENTITY (>20 codes)");
	/*
	 * 2.0 CODES, AND THE NUMBER IS THE MEASUREMENT'S. A 65-cube on a squared
	 * index carries 1.60 codes of worst-case trilinear error against lcms2 on
	 * this profile (az_icc.h has the table); the threshold is that plus room
	 * for the 16-bit quantisation and this sweep's own grid. It is NOT the 1D
	 * curve's 0.01 and cannot be: a cube of a gamma-2.6 transform is an
	 * approximation, and pretending otherwise here would make the gate fail on
	 * arithmetic that is working as designed.
	 */
	printf("  65-cube + trilinear vs lcms2's own transform: worst %.2f "
		"codes\n", worst_agree * 255.0);
	ok(worst_agree * 255.0 < 2.0,
		"the cube reproduces lcms2's transform off-grid (<2 codes)");

	/*
	 * ── FALSIFIER: THE DOMAIN ─────────────────────────────────────────────
	 *
	 * The same table, sampled along the sRGB-ENCODED axis instead of the linear
	 * one. This is AZ_BREAK_CLUT_DOMAIN's arithmetic, measured here without a
	 * GPU so that the number the renderer's fixture expects to see is
	 * established first.
	 *
	 * It has to be LARGE. sRGB encoding and linear light agree only at 0 and 1,
	 * and the whole point of the domain being invisible is that the picture
	 * stays smooth -- so a small delta here would mean the fixture that follows
	 * cannot tell the two apart either.
	 */
	{
		struct clut_cargo bad = { .t = t, .encoded_domain = true };
		struct az_icc_clut *wrong = az_icc_clut_build(AZ_ICC_CLUT_DIM,
			clut_eval_icc, &bad);
		ok(wrong != NULL, "BREAK: the wrong-domain cube builds");
		double w = 0.0;
		for (int i = 0; wrong != NULL && i < 512; i++) {
			float in[3] = {
				(float)((i * 37) % 97) / 96.0f,
				(float)((i * 53) % 89) / 88.0f,
				(float)((i * 71) % 83) / 82.0f,
			};
			float good[3], got[3];
			az_icc_clut_apply(cube, in, good);
			az_icc_clut_apply(wrong, in, got);
			for (int c = 0; c < 3; c++) {
				double d = fabs((double)got[c] - (double)good[c]);
				if (d > w) {
					w = d;
				}
			}
		}
		printf("  FALSIFIER sampled in the ENCODED domain: worst %.1f codes\n",
			w * 255.0);
		ok(w * 255.0 > 20.0, "BREAK: the wrong colour domain is DETECTED");
		az_icc_clut_free(wrong);
	}

	/*
	 * ── FALSIFIER: THE LAYOUT ─────────────────────────────────────────────
	 *
	 * R and B exchanged on the way in. A cube whose axes are transposed reads
	 * as a colour cast and nothing else -- no banding, no artefact -- which is
	 * exactly why it needs a falsifier rather than an inspection.
	 */
	{
		struct clut_cargo bad = { .t = t, .transposed = true };
		struct az_icc_clut *wrong = az_icc_clut_build(AZ_ICC_CLUT_DIM,
			clut_eval_icc, &bad);
		double w = 0.0;
		for (int i = 0; wrong != NULL && i < 512; i++) {
			float in[3] = {
				(float)((i * 37) % 97) / 96.0f,
				(float)((i * 53) % 89) / 88.0f,
				(float)((i * 71) % 83) / 82.0f,
			};
			float good[3], got[3];
			az_icc_clut_apply(cube, in, good);
			az_icc_clut_apply(wrong, in, got);
			for (int c = 0; c < 3; c++) {
				double d = fabs((double)got[c] - (double)good[c]);
				if (d > w) {
					w = d;
				}
			}
		}
		printf("  FALSIFIER R and B exchanged: worst %.1f codes\n", w * 255.0);
		ok(w * 255.0 > 20.0, "BREAK: a transposed cube is DETECTED");
		az_icc_clut_free(wrong);
	}

	az_icc_clut_free(cube);
	cmsDeleteTransform(t);
	cmsCloseProfile(dst);
	cmsCloseProfile(lin);
	free(data);
	remove(path);
}

int main(void) {
	/*
	 * M6C FIRST, because it is machine-independent and G1 below is not. Running
	 * it after the skip would make it unreachable on every machine without
	 * FI32U.icm -- which is every machine but this desk.
	 */
	gate_clut();

	printf("== G1: ICC matrix-shaper ingest ==\n");

	void *data = NULL;
	size_t size = 0;
	if (!slurp(PROFILE, &data, &size)) {
		/* Not a failure: the profile belongs to one machine. Skipping loudly
		 * is honest; passing silently would let this rot into a test that
		 * checks nothing on every other machine. */
		printf("  SKIP %s not present -- this gate is display-specific\n",
			PROFILE);
		/* But a cLUT failure is NOT display-specific and must not be swallowed
		 * by G1's skip: 77 tells meson "nothing ran", which would be a lie. */
		return failures ? 1 : 77;
	}

	/*
	 * WITHOUT the vcgt, because that is the only form comparable to lcms2's
	 * transform -- lcms2 does not apply a vcgt, by design. The composition is
	 * checked separately below, which is what G1 asks for and what makes each
	 * half falsifiable on its own.
	 */
	struct az_icc_shaper s;
	enum az_icc_reject rc = az_icc_load_shaper(data, size, false, &s);
	ok(rc == AZ_ICC_OK, "the profile is accepted as matrix-shaper");
	if (rc != AZ_ICC_OK) {
		printf("  rejected: %s\n", az_icc_reject_name(rc));
		free(data);
		return 1;
	}
	ok(s.has_vcgt, "PREMISE: it carries a vcgt (D4 has something to compose)");

	/* lcms2's own transform, as the independent implementation to check
	 * against -- deliberately not our matrix and curve applied twice. */
	cmsHPROFILE dst = cmsOpenProfileFromMem(data, (cmsUInt32Number)size);
	cmsHPROFILE src = cmsCreate_sRGBProfile();
	cmsHTRANSFORM t = cmsCreateTransform(src, TYPE_RGB_FLT, dst, TYPE_RGB_FLT,
		INTENT_RELATIVE_COLORIMETRIC, cmsFLAGS_NOOPTIMIZE);
	ok(t != NULL, "lcms2 builds the reference transform");
	if (t == NULL) {
		free(data);
		return 1;
	}

	/*
	 * A 16^3 grid, NOT a neutral ramp. Every row of a plausible wrong matrix
	 * sums to 1, so greys are invariant under exactly the error most likely to
	 * occur -- recorded three times during M5 before it stopped costing days.
	 */
	const int N = 16;
	double worst_ident = 0.0, worst_agree = 0.0;
	long moved = 0, total = 0;
	for (int ri = 0; ri < N; ri++) {
		for (int gi = 0; gi < N; gi++) {
			for (int bi = 0; bi < N; bi++) {
				float enc[3] = {
					(float)ri / (N - 1), (float)gi / (N - 1),
					(float)bi / (N - 1),
				};
				float ref[3];
				cmsDoTransform(t, enc, ref, 1);

				/* Our path: sRGB-decode to scene linear, shaper, then encode
				 * for comparison in the same units lcms2 reported. */
				float lin[3];
				for (int c = 0; c < 3; c++) {
					lin[c] = enc[c] <= 0.04045f ? enc[c] / 12.92f
						: powf((enc[c] + 0.055f) / 1.055f, 2.4f);
				}
				float got[3];
				az_icc_apply(&s, lin, got);

				for (int c = 0; c < 3; c++) {
					double d_ident = fabs((double)ref[c] - (double)enc[c]);
					if (d_ident > worst_ident) {
						worst_ident = d_ident;
					}
					double d_agree = fabs((double)got[c] - (double)ref[c]);
					if (d_agree > worst_agree) {
						worst_agree = d_agree;
					}
				}
				total++;
				if (fabs((double)ref[0] - (double)enc[0]) > 1.0 / 255.0) {
					moved++;
				}
			}
		}
	}

	printf("  profile moves the grid by up to %.1f codes (8-bit), "
		"%.1f%% of samples move\n", worst_ident * 255.0,
		100.0 * (double)moved / (double)total);
	ok(worst_ident * 255.0 > 20.0,
		"PREMISE: the profile is measurably NON-IDENTITY (>20 codes)");

	printf("  our reduction vs lcms2's own transform: worst %.2f codes\n",
		worst_agree * 255.0);
	ok(worst_agree * 255.0 < 2.0,
		"the matrix+curve reproduces lcms2's transform (<2 codes)");

	/*
	 * ── FALSIFIERS: each must go red on its own ───────────────────────────
	 *
	 * Agreement above is only evidence if disagreement is reachable. These
	 * perturb one thing each and re-measure.
	 */
	{
		/*
		 * Measured against the CORRECT shaper, not against an absolute
		 * tolerance. The question a falsifier answers is "would a wrong matrix
		 * be noticed", and that is a difference between two of our own
		 * results; comparing the broken one to lcms2 instead conflates the
		 * perturbation with the gate's own residual, and the first version of
		 * this check duly reported 1.23 codes -- BELOW its own threshold --
		 * while the perturbation was working perfectly.
		 *
		 * Over the whole grid, too. A thin slice can miss the region a given
		 * matrix element actually governs.
		 */
		struct az_icc_shaper bad = s;
		bad.matrix[0] += 1e-2f;
		double w = 0.0;
		for (int ri = 0; ri < N; ri++) {
			for (int gi = 0; gi < N; gi++) {
				for (int bi = 0; bi < N; bi++) {
					float enc[3] = {
						(float)ri / (N - 1), (float)gi / (N - 1),
						(float)bi / (N - 1),
					};
					float lin[3];
					for (int c = 0; c < 3; c++) {
						lin[c] = enc[c] <= 0.04045f ? enc[c] / 12.92f
							: powf((enc[c] + 0.055f) / 1.055f, 2.4f);
					}
					float good[3], broke[3];
					az_icc_apply(&s, lin, good);
					az_icc_apply(&bad, lin, broke);
					for (int c = 0; c < 3; c++) {
						double d = fabs((double)broke[c] - (double)good[c]);
						if (d > w) {
							w = d;
						}
					}
				}
			}
		}
		printf("  FALSIFIER perturbed matrix[0] by 1e-2: worst %.2f codes\n",
			w * 255.0);
		ok(w * 255.0 > 2.0, "BREAK: a perturbed matrix is DETECTED");
	}
	/*
	 * ── D4: THE vcgt IS COMPOSED, AND THE COMPOSITION IS CHECKED ──────────
	 *
	 * Every tap of the composed curve must equal vcgt(uncomposed tap). That is
	 * the claim; comparing against lcms2 cannot make it, because lcms2 never
	 * applies a vcgt.
	 */
	{
		struct az_icc_shaper withv;
		enum az_icc_reject rc2 = az_icc_load_shaper(data, size, true, &withv);
		ok(rc2 == AZ_ICC_OK, "the profile loads with the vcgt composed");

		cmsHPROFILE h2 = cmsOpenProfileFromMem(data, (cmsUInt32Number)size);
		cmsToneCurve **vcgt = cmsReadTag(h2, cmsSigVcgtTag);
		double worst_tap = 0.0, worst_effect = 0.0;
		for (int ch = 0; ch < 3 && vcgt != NULL; ch++) {
			for (int i = 0; i < AZ_ICC_CURVE_TAPS; i++) {
				float plain = (float)s.curve[ch][i] / 65535.0f;
				float want = cmsEvalToneCurveFloat(vcgt[ch], plain);
				float got = (float)withv.curve[ch][i] / 65535.0f;
				double d = fabs((double)got - (double)want);
				if (d > worst_tap) {
					worst_tap = d;
				}
				double e = fabs((double)got - (double)plain);
				if (e > worst_effect) {
					worst_effect = e;
				}
			}
		}
		printf("  vcgt composition matches the tag at every tap: worst "
			"%.4f codes\n", worst_tap * 255.0);
		ok(worst_tap * 255.0 < 0.02,
			"D4: the composed curve IS vcgt(TRC^-1(x)) at every tap");

		/* PREMISE for that check: if the vcgt were an identity the agreement
		 * above would hold trivially and prove nothing. */
		printf("  and the vcgt actually changes the curve by %.2f codes\n",
			worst_effect * 255.0);
		ok(worst_effect * 255.0 > 0.5,
			"PREMISE: the vcgt is non-trivial, so composing it MATTERS");
		cmsCloseProfile(h2);
	}

	/*
	 * ── THE REFUSALS, WHICH ARE BEHAVIOUR AND NOT LEFTOVERS ───────────────
	 *
	 * D2 refuses three things before it ever looks for a matrix, and until now
	 * none of them had ever been executed: the only profile on this desk is an
	 * RGB display matrix-shaper, so every path except the accepting one was
	 * reasoned about and never run. Each profile below is synthesised for the
	 * refusal it is meant to trigger.
	 *
	 * THE cLUT ONE CARRIES COLORANTS AND TRCs. That is what makes it a test: a
	 * cLUT profile without them would be refused by an implementation that
	 * merely failed to find a matrix, so it would pass while proving nothing
	 * about classification. This one gives a matrix-shaper reader everything it
	 * wants and must still be refused, because an A2B0 transform is not a
	 * matrix and approximating it would characterise the display wrongly while
	 * looking like it worked.
	 */
	{
		const char *dir = getenv("TMPDIR");
		char p_clut[512], p_gray[512], p_input[512];
		snprintf(p_clut, sizeof(p_clut), "%s/az-g1-clut.icc",
			dir != NULL ? dir : "/tmp");
		snprintf(p_gray, sizeof(p_gray), "%s/az-g1-gray.icc",
			dir != NULL ? dir : "/tmp");
		snprintf(p_input, sizeof(p_input), "%s/az-g1-input.icc",
			dir != NULL ? dir : "/tmp");
		ok(make_clut(p_clut) && make_gray(p_gray) && make_input(p_input),
			"PREMISE: the synthetic profiles are written");

		const struct { const char *path; enum az_icc_reject want; const char *what; }
		REFUSE[] = {
			{p_clut,  AZ_ICC_REJECT_CLUT,
			 "a cLUT profile WITH colorants is refused by classification"},
			{p_gray,  AZ_ICC_REJECT_NOT_RGB, "a GRAY profile is refused"},
			{p_input, AZ_ICC_REJECT_NOT_DISPLAY,
			 "an INPUT-class RGB profile is refused"},
		};
		for (size_t i = 0; i < sizeof(REFUSE) / sizeof(REFUSE[0]); i++) {
			void *d2 = NULL;
			size_t s2 = 0;
			char msg[256];
			if (!slurp(REFUSE[i].path, &d2, &s2)) {
				snprintf(msg, sizeof(msg), "%s (could not read it back)",
					REFUSE[i].what);
				ok(false, msg);
				continue;
			}
			struct az_icc_shaper sh;
			enum az_icc_reject r2 = az_icc_load_shaper(d2, s2, true, &sh);
			snprintf(msg, sizeof(msg), "%s (got %s)", REFUSE[i].what,
				az_icc_reject_name(r2));
			ok(r2 == REFUSE[i].want, msg);
			free(d2);
		}

		/* THE PREMISE FOR ALL THREE. If the loader refused everything --
		 * including the real profile -- the rows above would pass for the
		 * wrong reason. That it accepted FI32U.icm is asserted at the top of
		 * this file; this states the pair explicitly. */
		ok(rc == AZ_ICC_OK,
			"PREMISE: the same loader ACCEPTS the real profile, so the "
			"refusals above are discrimination and not blanket rejection");
		remove(p_clut);
		remove(p_gray);
		remove(p_input);
	}

	cmsDeleteTransform(t);
	cmsCloseProfile(src);
	cmsCloseProfile(dst);
	free(data);

	printf("\n%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
