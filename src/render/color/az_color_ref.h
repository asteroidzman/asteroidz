/*
 * az_color_ref -- the whole pixel pipeline, on the CPU, in double precision.
 * Contract C4.
 *
 * WHAT THIS IS FOR. It is the ORACLE: when the GPU and this disagree beyond a
 * stated bound, the GPU is wrong. And it is the executable form of "prove SDR
 * before HDR" -- gate 1 of tests/test-color-pipeline.c is an exact identity
 * that no HDR work may be enabled in the compositor until it is green on the
 * GPU as well.
 *
 * WHY IT IS NOT LINKED INTO THE COMPOSITOR. It allocates, it is slow, it holds
 * every intermediate image at once, and it is written to be readable against
 * the ADRs rather than fast. It lives in the tests. The primitives it calls --
 * every transfer function, matrix and curve -- are C1's, shared with the
 * shaders, so a bug in a primitive cannot hide by being wrong identically on
 * both sides. What is NOT shared is the ORDERING, which this file spells out
 * from ADR-005 and ADR-008 step by step, because the ordering is what the
 * oracle exists to check.
 *
 * DOUBLE PRECISION INTERNALLY, float at the edges. The scene the GPU computes
 * is FP16; the reference must be enough better than that for the difference
 * between them to be attributable.
 *
 * EVERY INTERMEDIATE IS KEPT. ADR-008's falsifier is "a luminance constant is
 * on the wrong side of the matrix", and the only way to answer that is to
 * assert on the value BETWEEN the two steps. A reference that returned only
 * the final image would make every failure look the same.
 */
#ifndef AZ_COLOR_REF_H
#define AZ_COLOR_REF_H

#include <stdbool.h>

#include "render/az_lum.h"
#include "render/az_output_color.h"

/* RGBA, four doubles per pixel, row-major. Whether the values are electrical
 * or scene is a property of WHICH image it is, and is documented at each
 * field -- there is no flag, because a flag is a thing that can be wrong. */
struct az_ref_image {
	int w, h;
	double *px;
};

bool az_ref_image_init(struct az_ref_image *img, int w, int h);
void az_ref_image_free(struct az_ref_image *img);
/* Bounds-checked accessors; out of range reads (0,0,0,0). */
void az_ref_image_get(const struct az_ref_image *img, int x, int y,
					  double out[4]);
void az_ref_image_set(struct az_ref_image *img, int x, int y,
					  const double v[4]);
void az_ref_image_fill(struct az_ref_image *img, const double v[4]);

/*
 * One source. `src` holds ELECTRICAL PREMULTIPLIED values, which is what a
 * Wayland buffer contains: (a·R', a·G', a·B', a). The premultiplication having
 * happened in the electrical domain is the entire reason ADR-005 exists.
 */
struct az_ref_layer {
	struct az_ref_image src;
	struct az_lum_domain domain;
	int dst_x, dst_y;
	double opacity;
};

struct az_ref_scene {
	int w, h;
	/* Scene-value backdrop, premultiplied linear. Usually opaque black. */
	double background[4];
	const struct az_ref_layer *layers;
	int n_layers;
};

/*
 * THE BREAKS. Contract C4 asks for the broken pipeline variants to be kept as
 * fixtures and re-run in every full run, so that a test which has quietly
 * stopped being able to fail shows up as a GREEN break run rather than as
 * nothing at all.
 *
 * DEVIATION FROM C4, recorded: the contract says `#ifdef` fixtures. These are
 * a runtime enum instead. The reason is the rule the contract is enforcing --
 * an #ifdef variant is compiled only when someone remembers to configure a
 * second build, and a break that is never built is exactly the break that
 * silently stops breaking. As a runtime parameter, every break runs in every
 * run of the ordinary test binary and a green one fails the suite.
 *
 * Each is a plausible implementation, not a scribble: every one of these is
 * something a renderer has actually shipped.
 */
enum az_ref_break {
	AZ_REF_BREAK_NONE = 0,
	/* Blend the electrical values directly -- what AVK does today, and what a
	 * naive port of it to HDR would keep doing. */
	AZ_REF_BREAK_ENCODED_BLEND,
	/* Decode the premultiplied texel without un-premultiplying first: the
	 * "hardware decode for everything" shortcut ADR-005 rejects. Exact for
	 * a = 1, wrong everywhere a is not. */
	AZ_REF_BREAK_NO_UNPREMULTIPLY,
	/* Run alpha through the transfer function with the colour channels. */
	AZ_REF_BREAK_ALPHA_TRANSFER,
	/* Clamp the composited scene to [0, 1] -- an intermediate with no
	 * headroom, i.e. a UNORM working format. */
	AZ_REF_BREAK_CLAMP_SCENE,
	/* Blur the electrical values rather than the scene values. This is the
	 * one with a scar: blurring encoded values raises the mean of
	 * bright-on-dark content, which is how a shadow came out lighter than its
	 * backdrop (f8be42c). */
	AZ_REF_BREAK_ENCODED_BLUR,
	/* Encode an HDR output with the sRGB inverse EOTF instead of PQ. */
	AZ_REF_BREAK_NO_PQ,
	/* Tone-map an HDR output to a ceiling of 1.0 -- SDR white treated as the
	 * panel's peak, the whole point of ADR-003 discarded. */
	AZ_REF_BREAK_SDR_PEAK,
	/* Add the dither to the SCENE value, before the inverse EOTF, where one
	 * output code is not a constant amount of scene. */
	AZ_REF_BREAK_DITHER_EARLY,
	/* Decode with the piecewise sRGB EOTF and encode with a pure 2.2 power.
	 * This one exists because gate 1 -- the bit-exact SDR round trip -- is
	 * blind to every break above it: an opaque single layer is unaffected by
	 * how blending, premultiplication or alpha are handled, so the ONLY thing
	 * that can move it is a decode/encode pair that is not self-inverse. It is
	 * also the most likely real bug on this path: the stack today genuinely
	 * mixes the two SDR curves (frog maps sRGB to GAMMA22; wlroots assumes 2.2
	 * for untagged; Vulkan _SRGB views implement the piecewise one), and
	 * ADR-004's entire argument is that picking ONE of them is what makes the
	 * exact gate possible. */
	AZ_REF_BREAK_SRGB_ENCODE_MISMATCH,
	AZ_REF_BREAK_COUNT,
};

const char *az_ref_break_name(enum az_ref_break b);

/*
 * The output side. `state` is C3's, verbatim.
 *
 * knee is here rather than in az_output_color_state because contract C6 puts
 * it on the encode pass's push constants, not on the per-output state -- and
 * because ADR-009's value of 1.0 interacts with an SDR ceiling of 1.0 in a way
 * that needs to be adjustable to be discussed at all. See
 * docs/decisions.md, F5.
 *
 * blur_radius drives an optional box blur of the composited scene, present for
 * exactly one reason: the encoded-domain-blur break needs something to break.
 */
struct az_ref_output {
	struct az_output_color_state state;
	double knee;
	bool dither;
	int blur_radius;
};

/*
 * Every named intermediate, ADR-008's step list made addressable.
 *
 * decoded[i]  layer i after decode: SCENE values, premultiplied, in the
 *             layer's own raster. This is what a texture draw hands to the
 *             blend unit.
 * composite   the whole output after source-over: SCENE values, premultiplied.
 *             On Path A this has been quantised to the scanout buffer's 8-bit
 *             sRGB codes after every layer, because that is what compositing
 *             through an _SRGB attachment view actually does.
 * blurred     composite after the optional blur (== composite when off).
 * electrical  the output transform applied, PRE-dither. Alpha is 1.
 * final       post-dither and quantised to the output's depth. Alpha is 1.
 */
struct az_ref_frame {
	struct az_ref_image *decoded;
	int n_decoded;
	struct az_ref_image composite;
	struct az_ref_image blurred;
	struct az_ref_image electrical;
	struct az_ref_image final;
};

bool az_ref_render(const struct az_ref_scene *scene,
				   const struct az_ref_output *out, enum az_ref_break brk,
				   struct az_ref_frame *frame);
void az_ref_frame_free(struct az_ref_frame *frame);

/* Helpers the tests share rather than re-deriving. */
void az_ref_encode_scene(const struct az_ref_output *out, const double rgb[3],
						 double e_out[3]);
double az_ref_max_abs_diff(const struct az_ref_image *a,
						   const struct az_ref_image *b, bool rgb_only);

#endif /* AZ_COLOR_REF_H */
