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
 * The same discipline as az_color.c, and for the same reason: a colour
 * transform that can only be exercised through a renderer is a colour
 * transform nobody can check against a published constant.
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

/*
 * ── M6C: THE PROFILES THAT DO NOT REDUCE ──────────────────────────────────
 *
 * Everything above turns a profile into a 3x3 and a curve, and refuses when it
 * cannot. This turns one into a 3D table, and cannot refuse -- a cLUT profile
 * IS a table, so sampling one is not an approximation of the profile the way a
 * matrix would be.
 *
 * WHY BOTH FORMS SURVIVE. The matrix-shaper path is measured, tested and
 * cheaper: 2KB and three 1D fetches against 1.6MB and one 3D fetch, and its 256
 * taps on a warped index resolve the encode curve to 0.01 codes where the best
 * cube measured below resolves it to 0.9. Collapsing the two into "always a
 * cube" would make the profile AVK carries best the one it carries worst. So
 * D2's classification stays exactly as it is and this is the ELSE branch of it.
 *
 * ── THE DOMAIN, WHICH IS THE WHOLE OF THE CORRECTNESS ─────────────────────
 *
 * INPUT: scene-linear BT.709/D65, 1.0 = SDR reference white, clamped to [0,1].
 * OUTPUT: device code -- ELECTRICAL, post-TRC, what the scan-out buffer holds.
 *
 * That is not a choice, it is what the table is built from. wlroots builds its
 * ICC transform (render/color_lcms2.c) as lcms2 source profile = sRGB
 * primaries with GAMMA-1.0 TRCs -- i.e. a linear-light input axis -- to the
 * display profile in TYPE_RGB_FLT, which is device code. So the table's own
 * input axis is linear light and its output is already encoded.
 *
 * The consequence for the encode pass: the cube REPLACES both the gamut matrix
 * and the inverse EOTF, because it already contains both. A pass that applied
 * its 709->device matrix and then sampled would apply the colorant transform
 * twice; a pass that sampled and then applied sRGB^-1 would encode twice. See
 * az_output_color.h, which derives the identity matrix for exactly this reason,
 * and output_encode.frag, which says the same thing at the point of use.
 *
 * ── THE GRID ──────────────────────────────────────────────────────────────
 *
 * dim^3 samples, R varying FASTEST then G then B, so that
 *
 *     index = 3 * (r + dim*g + dim*dim*b)
 *
 * which is byte-for-byte what a Vulkan 3D image of extent (dim,dim,dim) wants
 * from a buffer copy, with x=R, y=G, z=B -- wlroots' own layout
 * (render/vulkan/pass.c), because it is the layout the copy imposes.
 *
 * SIXTEEN BITS, like the 1D curve and for the same reason: the encode pass
 * dithers against one output code, so a table quantised to the output's own
 * depth would put the table's resolution at the dither's amplitude.
 *
 * ── 65, AND A SQUARED INDEX. BOTH MEASURED, NEITHER INHERITED ─────────────
 *
 * wlroots uses a uniform 33. That is the obvious choice and it is not good
 * enough here, for the reason AZ_ICC_CURVE_TAPS already records one dimension
 * down: an encode curve has infinite slope at zero, so a grid uniform in LINEAR
 * light cannot track it near black however large it is. Measured against
 * lcms2's own transform for the synthesised cLUT profile (gamma 2.6, wide
 * primaries), worst error over a 41^3 off-grid sweep:
 *
 *     dim   index      worst      memory
 *      33   uniform    13.82      0.3 MB   <- wlroots' choice
 *      65   uniform     5.91      2.1 MB
 *      33   squared     3.99      0.3 MB
 *      45   squared     2.90      0.7 MB
 *      65   squared     1.60      2.1 MB   <- this
 *
 * The warp is worth more than the memory: a squared 33 beats a uniform 65 at
 * an eighth of the size. Both are taken anyway, because 2.1 MB against the
 * 66 MB Path-B intermediate the same output already owns is not a trade, and
 * the errors here land at the extreme toe where banding is most visible.
 *
 * The cost is at LOAD, not per frame: 274625 lcms2 evaluations, measured at
 * 59 ms, on a monitor rule change and nothing else. A uniform 33 would be 8 ms
 * -- 50 ms once, for 12x the accuracy, on the event that also reallocates
 * swapchains.
 *
 * CONSEQUENCE: AVK's cube is NOT SceneFX's cube, and that is deliberate. The
 * two renderers sample the same lcms2 transform at different resolutions, so
 * they agree to within the coarser one's error and AVK is the better of the
 * two. Anything comparing the two renderers pixel for pixel must budget for
 * that rather than treat it as a defect.
 */
#define AZ_ICC_CLUT_DIM 65

/*
 * The cube's index warp and its inverse: sample i on an axis holds the
 * transform at (i/(dim-1))^2, and a reader indexes with sqrt(v).
 *
 * SEPARATE FROM az_icc_curve_index ABOVE even though they compute the same
 * thing today. They are two tables' contracts, established by two different
 * measurements, and a change to one must not silently move the other -- the 1D
 * curve's warp is worth 580x on 256 taps of a single channel, this one is worth
 * 3.5x on a cube, and they could reasonably diverge.
 */
static inline float az_icc_clut_index(float linear) {
	return linear > 0.0f ? sqrtf(linear) : 0.0f;
}
static inline float az_icc_clut_domain(float index) {
	return index * index;
}

struct az_icc_clut {
	uint32_t dim;
	/* 3 * dim^3, device code, 65535 = 1.0. Flexible so the whole thing is one
	 * allocation with no interior pointer to get wrong. */
	uint16_t rgb[];
};

/*
 * Evaluate one scene-linear triple. The indirection is what keeps this file
 * free of wlroots: the compositor passes wlr_color_transform_eval, so AVK and
 * SceneFX sample a table with the same meaning, and the unit tests pass a
 * closed-form function so the gridding can be checked without an ICC profile
 * or a display at all.
 */
typedef void (*az_icc_clut_eval)(void *user, const float in[3], float out[3]);

/* dim^3 evaluations of `eval`, once, at profile-load time. NULL on a bad dim
 * or out of memory. */
struct az_icc_clut *az_icc_clut_build(uint32_t dim, az_icc_clut_eval eval,
	void *user);
void az_icc_clut_free(struct az_icc_clut *c);

/*
 * TRILINEAR, matching what a LINEAR-filtered 3D texture does on the GPU -- so
 * the CPU reference and the shader are one function rather than two
 * approximations of it, exactly as az_icc_apply is for the 1D curve.
 *
 * `in` is scene-linear and is clamped here; `out` is device code in [0,1].
 */
void az_icc_clut_apply(const struct az_icc_clut *c, const float in[3],
	float out[3]);

#endif /* AZ_ICC_H */
