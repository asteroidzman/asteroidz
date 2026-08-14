/*
 * Synthesise ICC profiles that this machine does not otherwise have.
 *
 * M6B/G3b needs a cLUT display profile to prove that AVK's refusal is real, and
 * there is no such profile on this desk -- the only one that exists is
 * matrix-shaper (FI32U.icm), which is exactly why D2 refuses cLUT by
 * classification and defers the rest. A falsifier that cannot be run is not a
 * falsifier, so the profile is generated.
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
 * A deliberately NON-TRIVIAL cLUT: it swaps green and blue.
 *
 * An identity cLUT would make "the profile was applied" and "the profile was
 * ignored" produce the same picture, which is the F15 coincidence in profile
 * form. If anything ever does start honouring these tags, a channel swap is
 * impossible to mistake for either.
 */
static void clut_fill(cmsUInt16Number *table, int grid) {
	int i = 0;
	for (int r = 0; r < grid; r++) {
		for (int g = 0; g < grid; g++) {
			for (int b = 0; b < grid; b++) {
				cmsUInt16Number rv = (cmsUInt16Number)
					(r * 65535 / (grid - 1));
				cmsUInt16Number gv = (cmsUInt16Number)
					(g * 65535 / (grid - 1));
				cmsUInt16Number bv = (cmsUInt16Number)
					(b * 65535 / (grid - 1));
				table[i++] = rv;
				table[i++] = bv; /* swapped, on purpose */
				table[i++] = gv;
			}
		}
	}
}

static bool make_clut(const char *path) {
	cmsHPROFILE h = cmsCreateProfilePlaceholder(0);
	if (h == NULL) {
		return false;
	}
	cmsSetProfileVersion(h, 4.3);
	cmsSetDeviceClass(h, cmsSigDisplayClass);
	cmsSetColorSpace(h, cmsSigRgbData);
	cmsSetPCS(h, cmsSigXYZData);
	cmsSetHeaderRenderingIntent(h, INTENT_RELATIVE_COLORIMETRIC);

	const int grid = 9;
	const size_t n = (size_t)grid * grid * grid * 3;
	cmsUInt16Number *table = calloc(n, sizeof(*table));
	if (table == NULL) {
		cmsCloseProfile(h);
		return false;
	}
	clut_fill(table, grid);

	/*
	 * INPUT CURVES, CLUT, OUTPUT CURVES -- all three, in that order.
	 *
	 * A pipeline holding only a CLUT will not serialise: the ICC lut types this
	 * becomes (mft2 / mAB) define the curve stages as part of the structure, and
	 * lcms2 refuses to write a pipeline it cannot express rather than inventing
	 * the missing pieces. The curves are identities; the CLUT is the subject.
	 */
	cmsToneCurve *ident[3];
	for (int i = 0; i < 3; i++) {
		ident[i] = cmsBuildGamma(0, 1.0);
	}
	cmsPipeline *pipe = cmsPipelineAlloc(0, 3, 3);
	cmsStage *pre = cmsStageAllocToneCurves(0, 3, ident);
	cmsStage *clut = cmsStageAllocCLut16bit(0, grid, 3, 3, table);
	cmsStage *post = cmsStageAllocToneCurves(0, 3, ident);
	/* cmsBool: nonzero is success. */
	bool ok = pipe != NULL && pre != NULL && clut != NULL && post != NULL
		&& cmsPipelineInsertStage(pipe, cmsAT_END, pre) != 0
		&& cmsPipelineInsertStage(pipe, cmsAT_END, clut) != 0
		&& cmsPipelineInsertStage(pipe, cmsAT_END, post) != 0;
	for (int i = 0; i < 3; i++) {
		cmsFreeToneCurve(ident[i]);
	}
	free(table);
	if (!ok) {
		if (pipe != NULL) {
			cmsPipelineFree(pipe);
		}
		cmsCloseProfile(h);
		return false;
	}

	cmsCIEXYZ d50 = *cmsD50_XYZ();
#define STEP(expr) do { if (!(expr)) { \
		fprintf(stderr, "icc-synth: failed at %s\n", #expr); \
		cmsPipelineFree(pipe); cmsCloseProfile(h); return false; } } while (0)
	STEP(cmsWriteTag(h, cmsSigMediaWhitePointTag, &d50));
	/* THE COLORANTS GO IN TOO. See the file header: a cLUT profile that omitted
	 * them would be refused by any implementation, including a wrong one. */
	STEP(write_colorants_and_trc(h));
	STEP(cmsWriteTag(h, cmsSigAToB0Tag, pipe));
	STEP(cmsMD5computeID(h));
	STEP(cmsSaveProfileToFile(h, path));
#undef STEP
	ok = true;
	cmsPipelineFree(pipe);
	cmsCloseProfile(h);
	return ok;
}

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
