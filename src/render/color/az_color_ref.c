/*
 * az_color_ref -- the CPU reference pipeline. Contract C4.
 *
 * Read this against docs/decisions.md, not against a shader. The ordering
 * below is transcribed from ADR-005 (decode and premultiplication) and ADR-008
 * (the output transform's seven steps) and each step is named after the step
 * in the ADR, so a disagreement between this and the GPU can be localised to a
 * step rather than to "the colour is wrong".
 */

#include "render/color/az_color_ref.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "render/color/az_color.h"

/* ── images ─────────────────────────────────────────────────────────────── */

bool az_ref_image_init(struct az_ref_image *img, int w, int h) {
	img->w = w;
	img->h = h;
	img->px = w > 0 && h > 0 ? calloc((size_t)w * h * 4, sizeof(double)) : NULL;
	return img->px != NULL;
}

void az_ref_image_free(struct az_ref_image *img) {
	free(img->px);
	img->px = NULL;
	img->w = img->h = 0;
}

void az_ref_image_get(const struct az_ref_image *img, int x, int y,
					  double out[4]) {
	if (img->px == NULL || x < 0 || y < 0 || x >= img->w || y >= img->h) {
		out[0] = out[1] = out[2] = out[3] = 0.0;
		return;
	}
	const double *p = &img->px[((size_t)y * img->w + x) * 4];
	out[0] = p[0];
	out[1] = p[1];
	out[2] = p[2];
	out[3] = p[3];
}

void az_ref_image_set(struct az_ref_image *img, int x, int y,
					  const double v[4]) {
	if (img->px == NULL || x < 0 || y < 0 || x >= img->w || y >= img->h) {
		return;
	}
	double *p = &img->px[((size_t)y * img->w + x) * 4];
	p[0] = v[0];
	p[1] = v[1];
	p[2] = v[2];
	p[3] = v[3];
}

void az_ref_image_fill(struct az_ref_image *img, const double v[4]) {
	for (int y = 0; y < img->h; y++) {
		for (int x = 0; x < img->w; x++) {
			az_ref_image_set(img, x, y, v);
		}
	}
}

double az_ref_max_abs_diff(const struct az_ref_image *a,
						   const struct az_ref_image *b, bool rgb_only) {
	if (a->w != b->w || a->h != b->h) {
		return INFINITY;
	}
	double worst = 0.0;
	const int n = rgb_only ? 3 : 4;
	for (int y = 0; y < a->h; y++) {
		for (int x = 0; x < a->w; x++) {
			double pa[4], pb[4];
			az_ref_image_get(a, x, y, pa);
			az_ref_image_get(b, x, y, pb);
			for (int c = 0; c < n; c++) {
				double d = fabs(pa[c] - pb[c]);
				if (d > worst) {
					worst = d;
				}
			}
		}
	}
	return worst;
}

const char *az_ref_break_name(enum az_ref_break b) {
	switch (b) {
	case AZ_REF_BREAK_NONE:
		return "none";
	case AZ_REF_BREAK_ENCODED_BLEND:
		return "encoded-domain blend";
	case AZ_REF_BREAK_NO_UNPREMULTIPLY:
		return "decode without un-premultiplying";
	case AZ_REF_BREAK_ALPHA_TRANSFER:
		return "transfer function applied to alpha";
	case AZ_REF_BREAK_CLAMP_SCENE:
		return "scene clamped at 1.0";
	case AZ_REF_BREAK_ENCODED_BLUR:
		return "encoded-domain blur";
	case AZ_REF_BREAK_NO_PQ:
		return "HDR output encoded without PQ";
	case AZ_REF_BREAK_SDR_PEAK:
		return "SDR white used as the output peak";
	case AZ_REF_BREAK_DITHER_EARLY:
		return "dither applied before the inverse EOTF";
	case AZ_REF_BREAK_SRGB_ENCODE_MISMATCH:
		return "sRGB decode, gamma-2.2 encode";
	case AZ_REF_BREAK_COUNT:
		break;
	}
	return "?";
}

/* ── primitives, promoted ───────────────────────────────────────────────── */

/*
 * The transfer functions come from C1 -- the same code the shaders' twin is
 * pinned against -- called through float and returned as double. Reimplementing
 * them here in double would produce a reference that agrees with nothing and a
 * second place for a constant to be wrong.
 */
static double eotf(enum az_tf tf, double e) {
	switch (tf) {
	case AZ_TF_SRGB:
		return (double)az_srgb_eotf((float)e);
	case AZ_TF_GAMMA22:
		return (double)az_gamma22_eotf((float)e);
	case AZ_TF_BT1886:
		return (double)az_bt1886_eotf((float)e);
	case AZ_TF_PQ:
		return (double)az_pq_eotf((float)e);
	case AZ_TF_LINEAR_EXT:
		/* scRGB is already linear. It is still clamped, because the un-
		 * premultiply divide can push it out of range and the invariant is
		 * per-decode, not per-curve. */
		return e < 0.0 ? 0.0 : (e > 1.0 ? 1.0 : e);
	/*
	 * OUTPUT-ONLY ENCODES, WITH NO CLOSED FORM AND NO DECODE.
	 *
	 * A measured curve and a measured cube are tables, and no SOURCE ever
	 * declares either -- they exist only as the last step of the encode pass.
	 * Listed rather than left to fall through so that a future transfer
	 * function is a compile warning here instead of a silent identity, which is
	 * what these two were until M6C made the omission visible.
	 */
	case AZ_TF_LUT1D:
	case AZ_TF_CLUT3D:
	case AZ_TF_COUNT:
		break;
	}
	return e;
}

static double ieotf(enum az_tf tf, double o) {
	switch (tf) {
	case AZ_TF_SRGB:
		return (double)az_srgb_ieotf((float)o);
	case AZ_TF_GAMMA22:
		return (double)az_gamma22_ieotf((float)o);
	case AZ_TF_BT1886:
		return (double)az_bt1886_ieotf((float)o);
	case AZ_TF_PQ:
		return (double)az_pq_ieotf((float)o);
	case AZ_TF_LINEAR_EXT:
		return o < 0.0 ? 0.0 : (o > 1.0 ? 1.0 : o);
	/* See eotf() above: tables, not formulae, and the CPU reference has no
	 * profile to consult. az_icc_apply and az_icc_clut_apply ARE the reference
	 * for those two, and the GPU gates compare against them directly. */
	case AZ_TF_LUT1D:
	case AZ_TF_CLUT3D:
	case AZ_TF_COUNT:
		break;
	}
	return o;
}

static void mat_mul_vec3d(const float m[9], const double v[3], double out[3]) {
	double r[3];
	for (int row = 0; row < 3; row++) {
		r[row] = (double)m[row * 3 + 0] * v[0] + (double)m[row * 3 + 1] * v[1] +
				 (double)m[row * 3 + 2] * v[2];
	}
	out[0] = r[0];
	out[1] = r[1];
	out[2] = r[2];
}

/* ADR-009's curve, in double, matching C1's algebra exactly. Kept here in
 * double rather than calling az_tonemap() through float because the oracle's
 * job is to be more precise than the thing it certifies; the ALGEBRA is the
 * shared thing, not the rounding. */
static void tonemap(double v[3], double knee, double peak) {
	if (peak <= 1.0) {
		return;
	}
	double m = v[0] > v[1] ? v[0] : v[1];
	if (v[2] > m) {
		m = v[2];
	}
	if (m <= knee) {
		return;
	}
	double x = m - knee;
	double P = peak - knee;
	double f = knee + P * x / (P + x);
	double s = f / m;
	v[0] *= s;
	v[1] *= s;
	v[2] *= s;
}

static double clampd(double v, double lo, double hi) {
	return v < lo ? lo : (v > hi ? hi : v);
}

/* ── decode: ADR-005's ordering, one texel ──────────────────────────────── */

/*
 *   un-premultiply -> EOTF on RGB only -> x luminance scale
 *   -> source primaries -> [Path A: tone map to the output ceiling]
 *   -> re-premultiply -> x opacity
 *
 * The order of the first two is the whole of ADR-005: the EOTF is non-linear,
 * so EOTF(a·R') is not a·EOTF(R'), and a hardware _SRGB view applied to a
 * premultiplied texel is wrong everywhere 0 < a < 1 -- which is every AA edge
 * and the whole of a translucent terminal.
 */
static void decode_texel(const double in[4], const struct az_lum_domain *d,
						 double opacity, const struct az_ref_output *out,
						 enum az_ref_break brk, double res[4]) {
	double a = in[3];
	double rgb[3];

	if (brk == AZ_REF_BREAK_NO_UNPREMULTIPLY) {
		rgb[0] = in[0];
		rgb[1] = in[1];
		rgb[2] = in[2];
	} else {
		/* a == 0 carries no colour: the premultiplied texel is (0,0,0,0) and
		 * the divide would be 0/0. Straight black is the only answer that
		 * survives being multiplied by zero again. */
		for (int c = 0; c < 3; c++) {
			rgb[c] = a > 0.0 ? in[c] / a : 0.0;
		}
	}

	for (int c = 0; c < 3; c++) {
		rgb[c] = eotf(d->tf, clampd(rgb[c], 0.0, 1.0));
		rgb[c] *= (double)d->scale;
	}

	if (brk == AZ_REF_BREAK_ALPHA_TRANSFER) {
		/* Alpha is coverage. Putting it through an EOTF makes every
		 * translucent edge darker AND changes what fraction of the backdrop
		 * survives, so the error compounds through the stack. */
		a = eotf(d->tf, clampd(a, 0.0, 1.0));
	}

	if (d->primaries == AZ_PRIM_BT2020) {
		mat_mul_vec3d(AZ_MAT_2020_TO_709, rgb, rgb);
	}

	/*
	 * ADR-007's asymmetry. Path B tone-maps the COMPOSITED value in the encode
	 * pass; Path A has no encode pass, so a source that can exceed 1.0 must be
	 * mapped here or the 8-bit attachment clamps it.
	 *
	 * Note what this does with ADR-009's knee of 1.0 and an SDR ceiling of
	 * 1.0: nothing. The curve is the identity for peak <= 1, so the value
	 * survives to the attachment and clamps there after all. That is a real
	 * inconsistency between ADR-007 and ADR-009, not an implementation
	 * shortcut -- see docs/decisions.md, F5. The reference applies
	 * the curve as specified so that the consequence is measurable rather than
	 * papered over.
	 */
	if (out->state.path == AZ_OUTPUT_PATH_A_DIRECT_SRGB &&
		(az_lum_tf_is_hdr(d->tf) || d->scale > 1.0f)) {
		tonemap(rgb, out->knee, (double)out->state.peak_scene);
	}

	for (int c = 0; c < 3; c++) {
		res[c] = rgb[c] * a * opacity;
	}
	res[3] = a * opacity;
}

/* ── the 8-bit sRGB attachment, modelled ────────────────────────────────── */

/*
 * Path A composites straight into the scanout buffer through an _SRGB view, so
 * the accumulator IS eight bits of sRGB code after every draw. Modelling that
 * is not pedantry: it is the difference between the oracle predicting the GPU
 * to within a rounding and the oracle predicting something the GPU never
 * computes. Alpha is UNORM, not sRGB -- Vulkan's _SRGB formats leave the alpha
 * channel linear, and dithering or encoding it would be the ADR-005 bug in a
 * different costume.
 */
static enum az_tf path_a_tf(enum az_ref_break brk) {
	return brk == AZ_REF_BREAK_SRGB_ENCODE_MISMATCH ? AZ_TF_GAMMA22
													: AZ_TF_SRGB;
}

static void quantise_srgb8(double p[4], enum az_ref_break brk) {
	const enum az_tf enc = path_a_tf(brk);
	for (int c = 0; c < 3; c++) {
		double e = ieotf(enc, clampd(p[c], 0.0, 1.0));
		e = round(e * 255.0) / 255.0;
		/* decode back with the SAME curve the attachment uses: the point of
		 * the break is that the WRITE side and the read side disagree, not
		 * that the buffer forgets what it holds. */
		p[c] = eotf(enc, e);
	}
	p[3] = round(clampd(p[3], 0.0, 1.0) * 255.0) / 255.0;
}

/* ── blur, for the one break that needs one ─────────────────────────────── */

static void box_blur(struct az_ref_image *img, int radius, bool encoded) {
	if (radius <= 0) {
		return;
	}
	struct az_ref_image tmp;
	if (!az_ref_image_init(&tmp, img->w, img->h)) {
		return;
	}
	/* The encoded break blurs the ELECTRICAL values: encode, average, decode.
	 * A blur of high-frequency bright-on-dark content comes out LIGHTER that
	 * way, because the encoding is concave and the mean of the encoded values
	 * is above the encoding of the mean. That is the shadow-glow scar
	 * (f8be42c), reproduced on purpose. */
	for (int pass = 0; pass < 2; pass++) {
		for (int y = 0; y < img->h; y++) {
			for (int x = 0; x < img->w; x++) {
				double acc[4] = {0, 0, 0, 0};
				int n = 0;
				for (int k = -radius; k <= radius; k++) {
					int sx = pass == 0 ? x + k : x;
					int sy = pass == 0 ? y : y + k;
					sx = sx < 0 ? 0 : (sx >= img->w ? img->w - 1 : sx);
					sy = sy < 0 ? 0 : (sy >= img->h ? img->h - 1 : sy);
					double p[4];
					az_ref_image_get(img, sx, sy, p);
					for (int c = 0; c < 3; c++) {
						acc[c] +=
							encoded ? ieotf(AZ_TF_SRGB, clampd(p[c], 0.0, 1.0))
									: p[c];
					}
					acc[3] += p[3];
					n++;
				}
				double o[4];
				for (int c = 0; c < 3; c++) {
					o[c] = encoded ? eotf(AZ_TF_SRGB, acc[c] / n) : acc[c] / n;
				}
				o[3] = acc[3] / n;
				az_ref_image_set(&tmp, x, y, o);
			}
		}
		memcpy(img->px, tmp.px, (size_t)img->w * img->h * 4 * sizeof(double));
	}
	az_ref_image_free(&tmp);
}

/* ── the output transform: ADR-008's seven steps ────────────────────────── */

void az_ref_encode_scene(const struct az_ref_output *out, const double rgb[3],
						 double e_out[3]) {
	double v[3] = {rgb[0], rgb[1], rgb[2]};

	/* 2. tone map, on the COMPOSITED value. fx_vk maps per source and then
	 *    sums, which overshoots the panel wherever two HDR windows overlap;
	 *    the composed value is the thing the curve must see. */
	tonemap(v, out->knee, (double)out->state.peak_scene);

	/* 3. gamut matrix, then clamp negatives (ADR-010). The clamp is last
	 *    resort: the tone map has already bounded the max channel, so what
	 *    remains is out-of-container chroma only. */
	mat_mul_vec3d(out->state.matrix, v, v);
	for (int c = 0; c < 3; c++) {
		if (v[c] < 0.0) {
			v[c] = 0.0;
		}
	}

	/* 4. luminance anchor. PQ is ABSOLUTE -- its optical 1.0 is 10000 cd/m2 --
	 *    and the scene is relative, so this is where the two meet. There is no
	 *    anchor for an sRGB output because there is nothing to anchor: scene
	 *    1.0 IS electrical 1.0 there, which is what makes the SDR gate an
	 *    exact identity. */
	if (out->state.encode_tf == AZ_TF_PQ) {
		double k = (double)out->state.ref_nits / 10000.0;
		for (int c = 0; c < 3; c++) {
			v[c] *= k;
		}
	}
	for (int c = 0; c < 3; c++) {
		v[c] = clampd(v[c], 0.0, 1.0);
	}

	/* 5. encode. */
	for (int c = 0; c < 3; c++) {
		e_out[c] = ieotf(out->state.encode_tf, v[c]);
	}
}

/* ── the frame ──────────────────────────────────────────────────────────── */

bool az_ref_render(const struct az_ref_scene *scene,
				   const struct az_ref_output *out, enum az_ref_break brk,
				   struct az_ref_frame *frame) {
	memset(frame, 0, sizeof(*frame));
	if (scene == NULL || out == NULL || scene->w <= 0 || scene->h <= 0) {
		return false;
	}
	frame->n_decoded = scene->n_layers;
	if (scene->n_layers > 0) {
		frame->decoded =
			calloc((size_t)scene->n_layers, sizeof(struct az_ref_image));
		if (frame->decoded == NULL) {
			return false;
		}
	}
	if (!az_ref_image_init(&frame->composite, scene->w, scene->h) ||
		!az_ref_image_init(&frame->blurred, scene->w, scene->h) ||
		!az_ref_image_init(&frame->electrical, scene->w, scene->h) ||
		!az_ref_image_init(&frame->final, scene->w, scene->h)) {
		az_ref_frame_free(frame);
		return false;
	}

	const bool encoded_blend = brk == AZ_REF_BREAK_ENCODED_BLEND;

	/* The backdrop. In the encoded-blend break the accumulator holds
	 * electrical values throughout, so the backdrop enters encoded too --
	 * otherwise the break would be a different bug (mixed domains) rather than
	 * the one it is named after. */
	double bg[4] = {scene->background[0], scene->background[1],
					scene->background[2], scene->background[3]};
	if (encoded_blend) {
		for (int c = 0; c < 3; c++) {
			bg[c] = ieotf(AZ_TF_SRGB, clampd(bg[c], 0.0, 1.0));
		}
	}
	az_ref_image_fill(&frame->composite, bg);

	for (int i = 0; i < scene->n_layers; i++) {
		const struct az_ref_layer *l = &scene->layers[i];
		if (!az_ref_image_init(&frame->decoded[i], l->src.w, l->src.h)) {
			az_ref_frame_free(frame);
			return false;
		}
		for (int y = 0; y < l->src.h; y++) {
			for (int x = 0; x < l->src.w; x++) {
				double in[4], res[4];
				az_ref_image_get(&l->src, x, y, in);
				if (encoded_blend) {
					/* No decode at all: the electrical texel is blended as if
					 * it were light, which is what compositing into an
					 * unmanaged 8-bit target does today. */
					for (int c = 0; c < 4; c++) {
						res[c] = in[c] * l->opacity;
					}
				} else {
					decode_texel(in, &l->domain, l->opacity, out, brk, res);
				}
				az_ref_image_set(&frame->decoded[i], x, y, res);

				/* source-over, premultiplied: dst = src + dst(1 - src.a). One
				 * blend equation for both paths (ADR-005); on Path A the
				 * hardware runs it on decoded values because the attachment
				 * carries an _SRGB view. */
				int dx = l->dst_x + x, dy = l->dst_y + y;
				if (dx < 0 || dy < 0 || dx >= scene->w || dy >= scene->h) {
					continue;
				}
				double dst[4];
				az_ref_image_get(&frame->composite, dx, dy, dst);
				double o[4];
				for (int c = 0; c < 4; c++) {
					o[c] = res[c] + dst[c] * (1.0 - res[3]);
				}
				if (brk == AZ_REF_BREAK_CLAMP_SCENE) {
					for (int c = 0; c < 3; c++) {
						o[c] = clampd(o[c], 0.0, 1.0);
					}
				}
				if (out->state.path == AZ_OUTPUT_PATH_A_DIRECT_SRGB &&
					!encoded_blend) {
					quantise_srgb8(o, brk);
				}
				az_ref_image_set(&frame->composite, dx, dy, o);
			}
		}
	}

	memcpy(frame->blurred.px, frame->composite.px,
		   (size_t)scene->w * scene->h * 4 * sizeof(double));
	box_blur(&frame->blurred, out->blur_radius,
			 brk == AZ_REF_BREAK_ENCODED_BLUR);

	/* The output transform. The intermediate is fully composited, so its alpha
	 * is dead here and the target's alpha is written as 1 (X formats). */
	struct az_ref_output eff = *out;
	if (brk == AZ_REF_BREAK_NO_PQ) {
		eff.state.encode_tf = AZ_TF_SRGB;
	}
	if (brk == AZ_REF_BREAK_SDR_PEAK) {
		eff.state.peak_scene = 1.0f;
	}
	if (brk == AZ_REF_BREAK_SRGB_ENCODE_MISMATCH &&
		eff.state.encode_tf == AZ_TF_SRGB) {
		eff.state.encode_tf = AZ_TF_GAMMA22;
	}

	for (int y = 0; y < scene->h; y++) {
		for (int x = 0; x < scene->w; x++) {
			double p[4], e[4];
			az_ref_image_get(&frame->blurred, x, y, p);
			if (encoded_blend) {
				/* Already electrical; nothing to encode. */
				e[0] = p[0];
				e[1] = p[1];
				e[2] = p[2];
			} else if (eff.state.path == AZ_OUTPUT_PATH_A_DIRECT_SRGB) {
				/* Path A has no encode pass -- the accumulator is already the
				 * scanout buffer's contents. Reading it back out as
				 * electrical is the hardware's write-side conversion, and it
				 * is exactly the inverse of the read-side one, which is what
				 * makes the round-trip gate exact. */
				for (int c = 0; c < 3; c++) {
					e[c] = ieotf(path_a_tf(brk), clampd(p[c], 0.0, 1.0));
				}
			} else {
				az_ref_encode_scene(&eff, p, e);
			}
			e[3] = 1.0;
			az_ref_image_set(&frame->electrical, x, y, e);

			/* 6. dither, then 7. write. IGN is anchored to the OUTPUT raster,
			 * so a regional target cannot phase-shift the pattern. RGB only:
			 * alpha is never dithered. */
			double f[4] = {e[0], e[1], e[2], 1.0};
			double q = (double)eff.state.dither_q;
			if (eff.dither && q > 0.0) {
				double n = (double)az_ign((float)x, (float)y) - 0.5;
				if (brk == AZ_REF_BREAK_DITHER_EARLY) {
					/* One output code is not a constant amount of SCENE, so
					 * an amplitude correct at one grey is wrong at every
					 * other -- vanishing in the highlights and swamping the
					 * shadows. */
					double s[3];
					for (int c = 0; c < 3; c++) {
						s[c] = clampd(p[c] + n * q, 0.0, 1.0);
					}
					double e2[3];
					az_ref_encode_scene(&eff, s, e2);
					for (int c = 0; c < 3; c++) {
						f[c] = e2[c];
					}
				} else {
					for (int c = 0; c < 3; c++) {
						f[c] = clampd(e[c] + n * q, 0.0, 1.0);
					}
				}
			}
			if (q > 0.0) {
				double levels = round(1.0 / q);
				for (int c = 0; c < 3; c++) {
					f[c] = round(f[c] * levels) / levels;
				}
			} else if (eff.state.path == AZ_OUTPUT_PATH_A_DIRECT_SRGB) {
				/* Already quantised by the attachment; rounding again is a
				 * no-op and doing it explicitly documents that. */
				for (int c = 0; c < 3; c++) {
					f[c] = round(f[c] * 255.0) / 255.0;
				}
			}
			az_ref_image_set(&frame->final, x, y, f);
		}
	}
	return true;
}

void az_ref_frame_free(struct az_ref_frame *frame) {
	for (int i = 0; i < frame->n_decoded; i++) {
		if (frame->decoded != NULL) {
			az_ref_image_free(&frame->decoded[i]);
		}
	}
	free(frame->decoded);
	frame->decoded = NULL;
	frame->n_decoded = 0;
	az_ref_image_free(&frame->composite);
	az_ref_image_free(&frame->blurred);
	az_ref_image_free(&frame->electrical);
	az_ref_image_free(&frame->final);
}
