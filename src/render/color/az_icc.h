#ifndef AZ_ICC_H
#define AZ_ICC_H

/*
 * ── A DISPLAY PROFILE, REDUCED TO WHAT THE ENCODE PASS CAN APPLY ──────────
 *
 * M6B/D2. Reads an ICC profile and produces the two things C6's encode pass
 * already has a slot for: a 3x3 matrix and a per-channel curve. Nothing else.
 *
 * MATRIX-SHAPER ONLY, AND THAT IS A DECISION. A profile carrying cLUT tags
 * (A2B/B2A) is refused by classification, and the refusal keeps today's
 * behaviour -- FALLBACK, SceneFX drives the output, one log line naming the
 * reason. The revival condition is stated so it does not run forever: a real
 * cLUT profile for a connected display existing on this machine. Not "someday
 * someone might". The only profile this desk has or can produce is
 * matrix-shaper (AORUS FI32U-2: rXYZ/gXYZ/bXYZ + single-gamma TRCs + vcgt, no
 * cLUT tags, no colorimeter installed), and M5 already built the slots it
 * fills -- 9 floats and 768 taps.
 *
 * PURE CPU. No Vulkan types, no wlroots types, no allocation on a frame path.
 * The same discipline as az_color_ref.c, and for the same reason: a colour
 * transform that can only be exercised through a renderer is a colour
 * transform nobody can unit-test.
 */

#include <stdbool.h>
#include <stddef.h>
#include <math.h>
#include <stdint.h>

/*
 * 256 taps per channel, sampled on a SQUARED index -- tap i holds the curve at
 * x = (i/(N-1))^2, and a reader indexes with sqrt(x).
 *
 * The warp is not an optimisation, it is the difference between working and
 * not. An encode curve is gamma^(-1), whose slope goes to infinity at zero, so
 * a uniform grid cannot track it near black however large it is: measured on
 * this profile, 256 uniform taps carry 5.81 codes of interpolation error and
 * 1024 still carry 2.51. On a squared index 256 taps carry 0.01 -- one sqrt in
 * the shader buys 580x.
 *
 * A table rather than a gamma exponent because the vcgt composed into it (D4)
 * is an arbitrary measured table with no closed form.
 */
#define AZ_ICC_CURVE_TAPS 256

/* Index warp and its inverse. Anything sampling `curve` must use these two --
 * they are the table's contract, not an implementation detail. */
static inline float az_icc_curve_index(float linear) {
	return linear > 0.0f ? sqrtf(linear) : 0.0f;
}
static inline float az_icc_curve_domain(float index) {
	return index * index;
}

struct az_icc_shaper {
	/*
	 * Scene BT.709 linear -> device linear, row-major, chromatic adaptation
	 * included. This is the same slot ADR-008 step 3 already fills with the
	 * BT.709->BT.2020 matrix on the HDR path, so a profiled SDR output costs
	 * the encode pass nothing it was not already doing.
	 */
	float matrix[9];
	/*
	 * ENCODE DIRECTION: linear -> device code. That is TRC inverted, with the
	 * vcgt composed on top of it -- vcgt(TRC^-1(x)) -- because the profile's
	 * measurements are only valid on the calibrated device state. Shipping
	 * the matrix without the vcgt would characterise a display that is not
	 * the one on the cable (D4).
	 *
	 * One application point. The DRM GAMMA_LUT is deliberately not touched.
	 */
	uint16_t curve[3][AZ_ICC_CURVE_TAPS];
	bool has_vcgt;
};

/* Why a profile was refused, so the log line can say something true. */
enum az_icc_reject {
	AZ_ICC_OK = 0,
	AZ_ICC_REJECT_UNREADABLE,
	AZ_ICC_REJECT_NOT_RGB,
	AZ_ICC_REJECT_NOT_DISPLAY,
	AZ_ICC_REJECT_CLUT,        /* A2B/B2A present: D2's revival condition */
	AZ_ICC_REJECT_NO_COLORANTS,
	AZ_ICC_REJECT_NO_TRC,
	AZ_ICC_REJECT_SINGULAR,    /* the colorant matrix will not invert */
};

const char *az_icc_reject_name(enum az_icc_reject r);

/*
 * Reduce `data` to a shaper. Returns AZ_ICC_OK and fills `out` on success;
 * on anything else `out` is untouched and the caller keeps FALLBACK.
 */
/*
 * `compose_vcgt` exists because the two halves are separately checkable and
 * must be separately checked. lcms2's own transform does NOT apply the vcgt --
 * it is a video-card LUT tag, not part of the ICC colour transform -- so the
 * matrix+curve reduction can only be compared against lcms2 with the vcgt
 * left out. Passing false is how G1 does that; it is also D4's falsifier, and
 * the difference between the two is the vcgt's own contribution.
 */
enum az_icc_reject az_icc_load_shaper(const void *data, size_t size,
	bool compose_vcgt, struct az_icc_shaper *out);

/* Evaluate the shaper on a scene-linear BT.709 triple, for tests and for the
 * CPU reference the GPU fixture is checked against. Output is device code in
 * [0,1]. */
void az_icc_apply(const struct az_icc_shaper *s, const float in[3],
	float out[3]);

#endif /* AZ_ICC_H */
