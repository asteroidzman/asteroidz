/*
 * Synthesise ICC profiles that this machine does not otherwise have.
 *
 * M6B/G3b needed a cLUT display profile to prove that AVK's refusal was real,
 * and there is no such profile on this desk -- the only one that exists is
 * matrix-shaper (FI32U.icm). A falsifier that cannot be run is not a falsifier,
 * so the profile is generated.
 *
 * M6C NEEDS THE SAME PROFILE TO WORK, not merely to be refused, and that
 * changed what it has to contain: an A2B0 that describes a real display and a
 * B2A0 without which lcms2 would silently transform through the colorants
 * instead. See make_clut(). It is still the only cLUT profile this machine has,
 * and it is now the only thing exercising the cLUT ENCODE path as well as the
 * classification, which is worth knowing when reading a gate that uses it: it
 * proves the pipeline carries a cLUT profile correctly, not that any particular
 * colorimeter's output is handled.
 *
 * THE cLUT PROFILE CARRIES COLORANTS AND TRCs TOO, and that is the whole point
 * of it. A cLUT profile with no colorants would be refused by any reading of
 * the code, including one that merely failed to find a matrix -- so it would
 * pass while proving nothing. Real cLUT profiles routinely carry colorants as a
 * fallback for readers that cannot do better, and approximating an A2B/B2A
 * transform from them would characterise the display WRONGLY while looking like
 * it worked. This profile is the shape of that trap: everything a matrix-shaper
 * reader wants, plus the A2B0 tag that says the matrix is not the answer.
 *
 *     icc-synth clut  <out.icc>   RGB display, colorants + TRCs + A2B0
 *     icc-synth gray  <out.icc>   a GRAY display profile (not RGB)
 *     icc-synth input <out.icc>   an RGB INPUT-class profile (not display)
 *
 * The last two exist because az_icc_load_shaper has three separate refusals
 * before it reaches the cLUT test, and a fixture that only ever feeds it RGB
 * display profiles never learns whether the other two work.
 */
#include <lcms2.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* sRGB-ish colorants, already adapted to the D50 PCS, so the profile looks
 * entirely reasonable to a reader that ignores the A2B0 tag. */
static const cmsCIEXYZ RED   = {0.436066, 0.222488, 0.013916};
static const cmsCIEXYZ GREEN = {0.385147, 0.716873, 0.097076};
static const cmsCIEXYZ BLUE  = {0.143066, 0.060608, 0.714096};

static bool write_colorants_and_trc(cmsHPROFILE h) {
	cmsToneCurve *g22 = cmsBuildGamma(0, 2.2);
	if (g22 == NULL) {
		return false;
	}
	bool ok = cmsWriteTag(h, cmsSigRedColorantTag, &RED)
		&& cmsWriteTag(h, cmsSigGreenColorantTag, &GREEN)
		&& cmsWriteTag(h, cmsSigBlueColorantTag, &BLUE)
		&& cmsWriteTag(h, cmsSigRedTRCTag, g22)
		&& cmsWriteTag(h, cmsSigGreenTRCTag, g22)
		&& cmsWriteTag(h, cmsSigBlueTRCTag, g22);
	cmsFreeToneCurve(g22);
	return ok;
}

/*
 * ── THE DISPLAY THE cLUT ACTUALLY DESCRIBES ──────────────────────────────
 *
 * M6C needs this profile to DRIVE an output, not merely to be refused, so its
 * A2B0/B2A0 have to describe a real, invertible display rather than being a
 * shape. They describe this one: wide primaries and a gamma of 2.6.
 *
 * DELIBERATELY FAR FROM THE COLORANTS ABOVE. The colorants say "sRGB-ish, gamma
 * 2.2" and the tables say "wide gamut, gamma 2.6", and the distance between the
 * two IS the test: a reader that ignores the cLUT and believes the colorants
 * produces a picture that is smooth, plausible and tens of codes wrong. A
 * profile whose two readings agreed would be green whichever one was used.
 */
static cmsHPROFILE make_ref_display(void) {
	cmsCIExyY wp = { 0.3127, 0.3290, 1.0 };
	cmsCIExyYTRIPLE prim = {
		.Red   = { 0.68, 0.32, 1.0 },
		.Green = { 0.20, 0.70, 1.0 },
		.Blue  = { 0.15, 0.05, 1.0 },
	};
	cmsToneCurve *g26 = cmsBuildGamma(0, 2.6);
	if (g26 == NULL) {
		return NULL;
	}
	cmsToneCurve *trc[3] = { g26, g26, g26 };
	cmsHPROFILE h = cmsCreateRGBProfile(&wp, &prim, trc);
	cmsFreeToneCurve(g26);
	return h;
}

/* cmsStageSampleCLut16bit's callback: `cargo` is the transform being tabulated,
 * so the table is lcms2's own answer sampled on a grid rather than arithmetic
 * written twice. */
static cmsInt32Number sample_through(const cmsUInt16Number In[],
		cmsUInt16Number Out[], void *cargo) {
	cmsDoTransform((cmsHTRANSFORM)cargo, In, Out, 1);
	return 1;
}

/*
 * One A2B/B2A pipeline: identity curves, a CLUT sampled through `xform`,
 * identity curves.
 *
 * All three stages, in that order, because the ICC lut types this becomes (mft2
 * / mAB) define the curve stages as part of the structure and lcms2 refuses to
 * write a pipeline it cannot express rather than inventing the missing pieces.
 */
static cmsPipeline *lut_pipeline(int grid, cmsHTRANSFORM xform) {
	cmsToneCurve *ident[3] = { NULL, NULL, NULL };
	for (int i = 0; i < 3; i++) {
		ident[i] = cmsBuildGamma(0, 1.0);
	}
	cmsPipeline *pipe = cmsPipelineAlloc(0, 3, 3);
	cmsStage *pre = cmsStageAllocToneCurves(0, 3, ident);
	cmsStage *clut = cmsStageAllocCLut16bit(0, grid, 3, 3, NULL);
	cmsStage *post = cmsStageAllocToneCurves(0, 3, ident);
	bool ok = pipe != NULL && pre != NULL && clut != NULL && post != NULL
		&& cmsStageSampleCLut16bit(clut, sample_through, xform, 0) != 0
		&& cmsPipelineInsertStage(pipe, cmsAT_END, pre) != 0
		&& cmsPipelineInsertStage(pipe, cmsAT_END, clut) != 0
		&& cmsPipelineInsertStage(pipe, cmsAT_END, post) != 0;
	for (int i = 0; i < 3; i++) {
		if (ident[i] != NULL) {
			cmsFreeToneCurve(ident[i]);
		}
	}
	if (!ok) {
		if (pipe != NULL) {
			cmsPipelineFree(pipe);
		}
		return NULL;
	}
	return pipe;
}

static bool make_clut(const char *path) {
	/*
	 * ── BOTH DIRECTIONS, AND B2A0 IS THE ONE THAT MATTERS ────────────────
	 *
	 * A2B0 alone was enough while this profile existed only to be REFUSED: any
	 * reader classifying it sees the tag and stops. It is not enough for a
	 * reader that has to USE it. lcms2 builds a transform INTO a profile from
	 * its PCS2Device tags -- B2A1, falling back to B2A0 -- and falls back to the
	 * colorants when it finds neither. So a profile with A2B0 and no B2A0 would
	 * be classified as a cLUT and then quietly transformed through its
	 * matrix-shaper fallback: the cLUT path would look tested and would have
	 * exercised a matrix.
	 */
	cmsHPROFILE ref = make_ref_display();
	cmsHPROFILE lab = cmsCreateLab4Profile(NULL);
	cmsHPROFILE h = cmsCreateProfilePlaceholder(0);
	cmsHTRANSFORM a2b = NULL, b2a = NULL;
	cmsPipeline *pipe_a2b = NULL, *pipe_b2a = NULL;
	bool ok = false;
	if (ref == NULL || lab == NULL || h == NULL) {
		goto out;
	}

	cmsSetProfileVersion(h, 4.3);
	cmsSetDeviceClass(h, cmsSigDisplayClass);
	cmsSetColorSpace(h, cmsSigRgbData);
	/* Lab, which is what real cLUT display profiles use and what lcms2 writes
	 * an mAB tag against without any encoding of our own. */
	cmsSetPCS(h, cmsSigLabData);
	cmsSetHeaderRenderingIntent(h, INTENT_RELATIVE_COLORIMETRIC);

	/* 33 on both, matching the cube AVK samples. A coarser grid here would put
	 * the profile's own quantisation inside the tolerance of every gate that
	 * uses it, which is a fixture measuring its own fixture. */
	const int grid = 33;
	a2b = cmsCreateTransform(ref, TYPE_RGB_16, lab, TYPE_Lab_16,
		INTENT_RELATIVE_COLORIMETRIC, cmsFLAGS_NOCACHE);
	b2a = cmsCreateTransform(lab, TYPE_Lab_16, ref, TYPE_RGB_16,
		INTENT_RELATIVE_COLORIMETRIC, cmsFLAGS_NOCACHE);
	if (a2b == NULL || b2a == NULL) {
		goto out;
	}
	pipe_a2b = lut_pipeline(grid, a2b);
	pipe_b2a = lut_pipeline(grid, b2a);
	if (pipe_a2b == NULL || pipe_b2a == NULL) {
		goto out;
	}

	cmsCIEXYZ d50 = *cmsD50_XYZ();
#define STEP(expr) do { if (!(expr)) { \
		fprintf(stderr, "icc-synth: failed at %s\n", #expr); \
		goto out; } } while (0)
	STEP(cmsWriteTag(h, cmsSigMediaWhitePointTag, &d50));
	/* THE COLORANTS GO IN TOO. See the file header: a cLUT profile that omitted
	 * them would be refused by an implementation that merely failed to find a
	 * matrix, and would prove nothing about classification. */
	STEP(write_colorants_and_trc(h));
	STEP(cmsWriteTag(h, cmsSigAToB0Tag, pipe_a2b));
	STEP(cmsWriteTag(h, cmsSigBToA0Tag, pipe_b2a));
	STEP(cmsMD5computeID(h));
	STEP(cmsSaveProfileToFile(h, path));
#undef STEP
	ok = true;

out:
	if (pipe_a2b != NULL) {
		cmsPipelineFree(pipe_a2b);
	}
	if (pipe_b2a != NULL) {
		cmsPipelineFree(pipe_b2a);
	}
	if (a2b != NULL) {
		cmsDeleteTransform(a2b);
	}
	if (b2a != NULL) {
		cmsDeleteTransform(b2a);
	}
	if (h != NULL) {
		cmsCloseProfile(h);
	}
	if (lab != NULL) {
		cmsCloseProfile(lab);
	}
	if (ref != NULL) {
		cmsCloseProfile(ref);
	}
	return ok;
}

/*
 * The other two makers exist for az_icc_load_shaper's OTHER refusals, and a
 * consumer that only wants the cLUT profile does not want them: an unused
 * static function is a warning, and warnings that are expected are warnings
 * nobody reads. tests/test-avk-render.c defines AZ_ICC_SYNTH_CLUT_ONLY.
 */
#ifndef AZ_ICC_SYNTH_CLUT_ONLY
static bool make_gray(const char *path) {
	cmsToneCurve *g22 = cmsBuildGamma(0, 2.2);
	cmsHPROFILE h = cmsCreateGrayProfile(cmsD50_xyY(), g22);
	if (h == NULL) {
		return false;
	}
	cmsSetDeviceClass(h, cmsSigDisplayClass);
	bool ok = cmsSaveProfileToFile(h, path);
	cmsCloseProfile(h);
	cmsFreeToneCurve(g22);
	return ok;
}

static bool make_input(const char *path) {
	cmsHPROFILE h = cmsCreate_sRGBProfile();
	if (h == NULL) {
		return false;
	}
	/* RGB, matrix-shaper, entirely readable -- and INPUT class, which a
	 * display characterisation is not. */
	cmsSetDeviceClass(h, cmsSigInputClass);
	bool ok = cmsSaveProfileToFile(h, path);
	cmsCloseProfile(h);
	return ok;
}
#endif /* AZ_ICC_SYNTH_CLUT_ONLY */

/*
 * The three makers are also compiled straight into tests/test-icc-shaper.c,
 * which #includes this file with AZ_ICC_SYNTH_NO_MAIN defined. One
 * implementation, two consumers: the unit test wants the profiles in a
 * temporary directory and the headless fixture wants them on disk for a
 * compositor to load, and a second copy of the synthesis would be a second
 * thing to keep honest.
 */
#ifndef AZ_ICC_SYNTH_NO_MAIN
int main(int argc, char **argv) {
	if (argc != 3) {
		fprintf(stderr, "usage: %s clut|gray|input <out.icc>\n", argv[0]);
		return 2;
	}
	bool ok;
	if (strcmp(argv[1], "clut") == 0) {
		ok = make_clut(argv[2]);
	} else if (strcmp(argv[1], "gray") == 0) {
		ok = make_gray(argv[2]);
	} else if (strcmp(argv[1], "input") == 0) {
		ok = make_input(argv[2]);
	} else {
		fprintf(stderr, "unknown kind: %s\n", argv[1]);
		return 2;
	}
	if (!ok) {
		fprintf(stderr, "icc-synth: failed to write %s\n", argv[2]);
		return 1;
	}
	printf("wrote %s (%s)\n", argv[2], argv[1]);
	return 0;
}
#endif /* AZ_ICC_SYNTH_NO_MAIN */
