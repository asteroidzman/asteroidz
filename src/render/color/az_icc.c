/*
 * M6B/D2+D4. ICC matrix-shaper ingest. See az_icc.h for what this refuses and
 * why.
 */
#include "az_icc.h"

#include <lcms2.h>
#include <math.h>
#include <string.h>

const char *az_icc_reject_name(enum az_icc_reject r) {
	switch (r) {
	case AZ_ICC_OK:                  return "ok";
	case AZ_ICC_REJECT_UNREADABLE:   return "unreadable";
	case AZ_ICC_REJECT_NOT_RGB:      return "not an RGB profile";
	case AZ_ICC_REJECT_NOT_DISPLAY:  return "not a display profile";
	case AZ_ICC_REJECT_CLUT:         return "cLUT profile (matrix-shaper only)";
	case AZ_ICC_REJECT_NO_COLORANTS: return "no rXYZ/gXYZ/bXYZ colorants";
	case AZ_ICC_REJECT_NO_TRC:       return "no per-channel TRC";
	case AZ_ICC_REJECT_SINGULAR:     return "colorant matrix is singular";
	}
	return "?";
}

/*
 * BT.709 primaries to XYZ under D65, and the Bradford adaptation D65->D50.
 *
 * The PCS is D50 and ICC colorants are stored already adapted to it, so the
 * scene side has to be brought to the same white before the two can be
 * composed. Skipping this is the classic ICC error: the result looks almost
 * right and is wrong by the distance between the two illuminants, which on a
 * wide-gamut panel is a visible warm/cool cast rather than a rounding error.
 */
static const double BT709_TO_XYZ_D65[9] = {
	0.4123907993, 0.3575843394, 0.1804807884,
	0.2126390059, 0.7151686788, 0.0721923154,
	0.0193308187, 0.1191947798, 0.9505321522,
};
static const double BRADFORD_D65_TO_D50[9] = {
	 1.0478112,  0.0228866, -0.0501270,
	 0.0295424,  0.9904844, -0.0170491,
	-0.0092345,  0.0150436,  0.7521316,
};

static void mat3_mul(const double a[9], const double b[9], double out[9]) {
	for (int r = 0; r < 3; r++) {
		for (int c = 0; c < 3; c++) {
			out[r * 3 + c] = a[r * 3 + 0] * b[0 * 3 + c]
				+ a[r * 3 + 1] * b[1 * 3 + c]
				+ a[r * 3 + 2] * b[2 * 3 + c];
		}
	}
}

static bool mat3_invert(const double m[9], double out[9]) {
	double det = m[0] * (m[4] * m[8] - m[5] * m[7])
		- m[1] * (m[3] * m[8] - m[5] * m[6])
		+ m[2] * (m[3] * m[7] - m[4] * m[6]);
	if (!isfinite(det) || fabs(det) < 1e-12) {
		return false;
	}
	double inv = 1.0 / det;
	out[0] = (m[4] * m[8] - m[5] * m[7]) * inv;
	out[1] = (m[2] * m[7] - m[1] * m[8]) * inv;
	out[2] = (m[1] * m[5] - m[2] * m[4]) * inv;
	out[3] = (m[5] * m[6] - m[3] * m[8]) * inv;
	out[4] = (m[0] * m[8] - m[2] * m[6]) * inv;
	out[5] = (m[2] * m[3] - m[0] * m[5]) * inv;
	out[6] = (m[3] * m[7] - m[4] * m[6]) * inv;
	out[7] = (m[1] * m[6] - m[0] * m[7]) * inv;
	out[8] = (m[0] * m[4] - m[1] * m[3]) * inv;
	return true;
}

enum az_icc_reject az_icc_load_shaper(const void *data, size_t size,
		bool compose_vcgt, struct az_icc_shaper *out) {
	if (data == NULL || size == 0 || out == NULL) {
		return AZ_ICC_REJECT_UNREADABLE;
	}
	cmsHPROFILE h = cmsOpenProfileFromMem(data, (cmsUInt32Number)size);
	if (h == NULL) {
		return AZ_ICC_REJECT_UNREADABLE;
	}
	enum az_icc_reject rc = AZ_ICC_OK;

	if (cmsGetColorSpace(h) != cmsSigRgbData) {
		rc = AZ_ICC_REJECT_NOT_RGB;
		goto done;
	}
	if (cmsGetDeviceClass(h) != cmsSigDisplayClass) {
		rc = AZ_ICC_REJECT_NOT_DISPLAY;
		goto done;
	}
	/*
	 * cLUT refused by CLASSIFICATION, before anything is read. A profile
	 * carrying A2B/B2A describes a transform the matrix+curve form cannot
	 * express, and approximating it with the colorants -- which such profiles
	 * often still carry as a fallback -- would silently characterise the
	 * display wrongly while looking like it worked.
	 */
	if (cmsIsTag(h, cmsSigAToB0Tag) || cmsIsTag(h, cmsSigBToA0Tag)) {
		rc = AZ_ICC_REJECT_CLUT;
		goto done;
	}

	cmsCIEXYZ *r = cmsReadTag(h, cmsSigRedColorantTag);
	cmsCIEXYZ *g = cmsReadTag(h, cmsSigGreenColorantTag);
	cmsCIEXYZ *b = cmsReadTag(h, cmsSigBlueColorantTag);
	if (r == NULL || g == NULL || b == NULL) {
		rc = AZ_ICC_REJECT_NO_COLORANTS;
		goto done;
	}
	cmsToneCurve *trc[3] = {
		cmsReadTag(h, cmsSigRedTRCTag),
		cmsReadTag(h, cmsSigGreenTRCTag),
		cmsReadTag(h, cmsSigBlueTRCTag),
	};
	if (trc[0] == NULL || trc[1] == NULL || trc[2] == NULL) {
		rc = AZ_ICC_REJECT_NO_TRC;
		goto done;
	}

	/* device linear -> XYZ(D50): the colorants ARE that matrix, in columns. */
	const double dev_to_xyz[9] = {
		r->X, g->X, b->X,
		r->Y, g->Y, b->Y,
		r->Z, g->Z, b->Z,
	};
	double xyz_to_dev[9];
	if (!mat3_invert(dev_to_xyz, xyz_to_dev)) {
		rc = AZ_ICC_REJECT_SINGULAR;
		goto done;
	}
	double scene_to_d50[9];
	mat3_mul(BRADFORD_D65_TO_D50, BT709_TO_XYZ_D65, scene_to_d50);
	double scene_to_dev[9];
	mat3_mul(xyz_to_dev, scene_to_d50, scene_to_dev);
	for (int i = 0; i < 9; i++) {
		out->matrix[i] = (float)scene_to_dev[i];
	}

	/*
	 * The curve, in the ENCODE direction and with the vcgt composed on.
	 *
	 * The profile's TRC is device -> linear, so the encode direction is its
	 * inverse. cmsReverseToneCurve samples rather than solving in closed form,
	 * which is exactly right here: the composed result has no closed form
	 * either, because a vcgt is a measured table.
	 */
	cmsToneCurve **vcgt = cmsReadTag(h, cmsSigVcgtTag);
	out->has_vcgt = vcgt != NULL;
	if (!compose_vcgt) {
		vcgt = NULL;
	}
	for (int ch = 0; ch < 3; ch++) {
		cmsToneCurve *inv = cmsReverseToneCurve(trc[ch]);
		if (inv == NULL) {
			rc = AZ_ICC_REJECT_NO_TRC;
			goto done;
		}
		for (int i = 0; i < AZ_ICC_CURVE_TAPS; i++) {
			/* Squared index: tap i is the curve at (i/(N-1))^2, which puts the
			 * taps where gamma^-1 actually bends. See AZ_ICC_CURVE_TAPS. */
			float x = az_icc_curve_domain(
				(float)i / (float)(AZ_ICC_CURVE_TAPS - 1));
			float v = cmsEvalToneCurveFloat(inv, x);
			/* Calibration acts on DEVICE values, so it composes after the
			 * encode and never before it. The other order applies the
			 * calibration to light rather than to code and is wrong in a way
			 * that still produces a picture. */
			if (vcgt != NULL) {
				v = cmsEvalToneCurveFloat(vcgt[ch], v);
			}
			if (!(v >= 0.0f)) {
				v = 0.0f;
			} else if (v > 1.0f) {
				v = 1.0f;
			}
			out->curve[ch][i] = (uint16_t)(v * 65535.0f + 0.5f);
		}
		cmsFreeToneCurve(inv);
	}

done:
	cmsCloseProfile(h);
	return rc;
}

void az_icc_apply(const struct az_icc_shaper *s, const float in[3],
		float out[3]) {
	float lin[3];
	for (int i = 0; i < 3; i++) {
		lin[i] = s->matrix[i * 3 + 0] * in[0]
			+ s->matrix[i * 3 + 1] * in[1]
			+ s->matrix[i * 3 + 2] * in[2];
		if (!(lin[i] >= 0.0f)) {
			lin[i] = 0.0f;
		} else if (lin[i] > 1.0f) {
			lin[i] = 1.0f;
		}
	}
	/* Linear interpolation between taps, matching what a LINEAR-filtered 1D
	 * texture does on the GPU -- so the CPU reference and the shader are the
	 * same function rather than two approximations of one. */
	for (int ch = 0; ch < 3; ch++) {
		float t = az_icc_curve_index(lin[ch])
			* (float)(AZ_ICC_CURVE_TAPS - 1);
		int i0 = (int)t;
		if (i0 >= AZ_ICC_CURVE_TAPS - 1) {
			out[ch] = (float)s->curve[ch][AZ_ICC_CURVE_TAPS - 1] / 65535.0f;
			continue;
		}
		float f = t - (float)i0;
		float a = (float)s->curve[ch][i0] / 65535.0f;
		float b = (float)s->curve[ch][i0 + 1] / 65535.0f;
		out[ch] = a + (b - a) * f;
	}
}
