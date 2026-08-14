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

int main(void) {
	printf("== G1: ICC matrix-shaper ingest ==\n");

	void *data = NULL;
	size_t size = 0;
	if (!slurp(PROFILE, &data, &size)) {
		/* Not a failure: the profile belongs to one machine. Skipping loudly
		 * is honest; passing silently would let this rot into a test that
		 * checks nothing on every other machine. */
		printf("  SKIP %s not present -- this gate is display-specific\n",
			PROFILE);
		return 77;
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

	cmsDeleteTransform(t);
	cmsCloseProfile(src);
	cmsCloseProfile(dst);
	free(data);

	printf("\n%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
