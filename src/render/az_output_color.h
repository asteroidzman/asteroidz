/*
 * az_output_color -- how scene values LEAVE the scene, per output. Contract
 * C3, ADR-012/007/008/010/011.
 *
 * The other half of the pipeline from az_lum.h, and the split between them is
 * the invariant: a luminance domain says nothing about a display, and this
 * struct says nothing about a source. A window straddling DP-1 (HDR) and
 * HDMI-A-1 (SDR) is one scene intent rendered twice, with the same domains and
 * two of these.
 *
 * DERIVED ON OUTPUT-STATE CHANGE, NEVER PER FRAME. Every field is a function
 * of things that change on hotplug, on a monitor rule, on a config reload, or
 * when HDR is toggled -- and of nothing else. `path` in particular must not
 * change within a frame: it decides which attachment composition targets, and
 * a frame that changed its mind halfway would have written half its pixels to
 * the wrong buffer in the wrong encoding.
 *
 * NO VULKAN IN HERE. Path B needs a working intermediate image; that handle
 * lives renderer-side, keyed by output (contract C6), deliberately NOT in this
 * struct. Keeping it out is what lets the whole decision table be a unit test
 * with no device -- and the decision table is where the bugs are, not in the
 * image allocation.
 */
#ifndef AZ_OUTPUT_COLOR_H
#define AZ_OUTPUT_COLOR_H

#include <stdbool.h>

#include "render/az_lum.h"
#include "render/color/az_color.h"

/*
 * ADR-001's two paths, plus the refusal.
 *
 * A  the output's render format is 8-bit sRGB, it has no image description and
 *    no ICC transform, and its scanout buffer's modifier can carry a mutable
 *    _SRGB view. Composition blends scene-linearly straight into the scanout
 *    buffer through that view: the hardware decodes on sample and encodes on
 *    write, so linear compositing costs zero extra passes and zero extra bytes
 *    over what AVK does today. Storage stays 8-bit sRGB-coded -- the same
 *    quantisation as today, neither better nor worse.
 *
 * B  everything else AVK can drive: HDR10/PQ, 10-bit SDR, and any Path-A
 *    candidate whose _SRGB view probe failed. Composition renders into a
 *    persistent whole-output working image and one damage-scissored encode
 *    pass writes the scanout buffer.
 *
 * FALLBACK  AVK does not drive this output at all; fx_vk does. In M5 that is
 *    exactly the ICC/3D-LUT case (ADR-000 leaves it to M6). It is a state of
 *    this struct rather than a NULL because "which outputs did AVK refuse and
 *    why" is a question worth being able to answer from one place.
 */
enum az_output_path {
	AZ_OUTPUT_PATH_A_DIRECT_SRGB = 0,
	AZ_OUTPUT_PATH_B_ENCODE,
	AZ_OUTPUT_PATH_FALLBACK,
};

/* For logs and test output. Not a parser: nothing reads these back. */
static inline const char *az_output_path_name(enum az_output_path p) {
	switch (p) {
	case AZ_OUTPUT_PATH_A_DIRECT_SRGB: return "A-direct-srgb";
	case AZ_OUTPUT_PATH_B_ENCODE:      return "B-encode";
	case AZ_OUTPUT_PATH_FALLBACK:      return "fallback";
	}
	return "?";
}

/* wlroots' default when a display declares no peak (frog-color-management.h:211
 * uses the same number). Only ever consulted for an HDR output. */
#define AZ_HDR_DEFAULT_PEAK_NITS 1000.0f

/*
 * What the compositor knows about an output, in plain values -- same rule as
 * az_lum_source_desc: no wlroots types, so the decision table is testable
 * without a compositor.
 *
 * bits_per_channel  8 or 10. The quantum the encode pass dithers against, and
 *                   half of the path decision.
 * hdr               the output is presenting an HDR image description.
 * has_icc           an ICC transform is attached (M5 refuses; M6 absorbs it).
 * hdr_max_nits      the panel's peak, cd/m2. 0 = unstated -> 1000.
 * scene_ref_nits    the SCENE's reference (ADR-003), cached here because the
 *                   encode pass needs it on the push constants. It is a scene
 *                   property that happens to be read per output, NOT a
 *                   per-output setting: two outputs disagreeing about what a
 *                   nit of scene 1.0 means would split a straddling window.
 * sdr_saturation    the existing global control. 0 or 1 = untouched.
 * scanout_srgb_view_ok  the C5 probe: can this output's scanout buffer be
 *                   created MUTABLE with an _SRGB view usable as a colour
 *                   attachment, for the modifier KMS actually chose? False
 *                   costs Path A and nothing else.
 */
struct az_output_desc {
	int bits_per_channel;
	bool hdr;
	bool has_icc;
	float hdr_max_nits;
	float scene_ref_nits;
	float sdr_saturation;
	bool scanout_srgb_view_ok;
};

/*
 * THE PER-OUTPUT HALF OF THE PIPELINE.
 *
 * matrix      scene primaries -> output primaries, composed with the
 *             saturation matrix. Row-major, y = M x, like everything else in
 *             this project. Identity on SDR outputs.
 * ref_nits    the resolved scene reference (never 0).
 * peak_scene  the output's ceiling IN SCENE UNITS: the tone map's asymptote.
 *             Exactly 1.0 on an SDR output, by definition -- an SDR display
 *             cannot show anything above SDR white, which is what SDR white
 *             means.
 * dither_q    the ENCODE PASS's dither amplitude, peak-to-peak, in electrical
 *             units. 0 on Path A, and that zero is not an omission: Path A has
 *             no encode pass, so there is no composed electrical value to
 *             perturb. Path A keeps M4D's shadow-local dither, unchanged,
 *             including its documented limitation (ADR-011).
 */
struct az_output_color_state {
	enum az_output_path path;
	enum az_tf encode_tf;
	float matrix[9];
	float ref_nits;
	float peak_scene;
	float dither_q;
};

static inline void az_output_color_set_identity(float m[9]) {
	for (int i = 0; i < 9; i++) {
		m[i] = AZ_MAT_IDENTITY[i];
	}
}

/*
 * THE DERIVATION. Pure, a few microseconds of matrix arithmetic, run when an
 * output's state changes.
 *
 * The order of the tests is the decision table of ADR-001, and it is written
 * most-restrictive-first so that no combination falls through two of them:
 *
 *   ICC            -> FALLBACK        AVK does not drive it in M5
 *   HDR            -> B, PQ           always B; PQ is an output encoding only
 *   10-bit SDR     -> B, sRGB         no hardware encode at 10 bits
 *   8-bit, probed  -> A, sRGB         hardware decode and encode, free
 *   8-bit, no view -> B, sRGB         the probe's only consequence
 */
static inline struct az_output_color_state
az_output_color_derive(const struct az_output_desc *o) {
	struct az_output_color_state s;
	s.path = AZ_OUTPUT_PATH_FALLBACK;
	s.encode_tf = AZ_TF_SRGB;
	az_output_color_set_identity(s.matrix);
	s.ref_nits = az_lum_ref_nits(o != NULL ? o->scene_ref_nits : 0.0f);
	s.peak_scene = 1.0f;
	s.dither_q = 0.0f;
	if (o == NULL) {
		return s;
	}

	if (o->has_icc) {
		/* ADR-000: fx_vk keeps this one. Every other field stays at its
		 * neutral value so a caller that reads them anyway gets an SDR
		 * identity rather than an uninitialised matrix. */
		return s;
	}

	/* One code (1/255 or 1/1023) peak-to-peak, per ADR-011. Computed from the
	 * depth rather than switched on it, so a future 12-bit output is not a
	 * silent zero. */
	const int bpc = o->bits_per_channel > 0 ? o->bits_per_channel : 8;
	const float quantum = 1.0f / (float)((1u << bpc) - 1u);

	if (o->hdr) {
		s.path = AZ_OUTPUT_PATH_B_ENCODE;
		s.encode_tf = AZ_TF_PQ;
		s.dither_q = quantum;
		/* ADR-003: the ceiling in scene units. A 1000-nit panel at the 203
		 * default is 4.926 -- so SDR white sits at a fifth of the panel's
		 * peak, which is the headroom HDR is for. */
		float peak_nits =
			o->hdr_max_nits > 0.0f ? o->hdr_max_nits : AZ_HDR_DEFAULT_PEAK_NITS;
		s.peak_scene = peak_nits / s.ref_nits;
		/* ADR-008 step 3's matrix, composed exactly as scenefx composes it
		 * (wlr_scene.c:4104-4126): primaries first, saturation on the right,
		 * so the saturation acts on SCENE-primaries values before the gamut
		 * conversion. Composing them the other way round desaturates toward
		 * BT.2020's luma axis instead of BT.709's, which is a different
		 * colour and a plausible-looking picture. */
		if (o->sdr_saturation > 0.0f && o->sdr_saturation != 1.0f) {
			float sat[9];
			az_mat_saturation(o->sdr_saturation, sat);
			az_mat_mul(AZ_MAT_709_TO_2020, sat, s.matrix);
		} else {
			for (int i = 0; i < 9; i++) {
				s.matrix[i] = AZ_MAT_709_TO_2020[i];
			}
		}
		return s;
	}

	/* SDR from here down. peak_scene is exactly 1.0 and the matrix is exactly
	 * the identity -- the scene's primaries ARE the output's (ADR-002), so
	 * there is nothing to convert and sdr_saturation has no colour-managed
	 * output to act on. */
	s.encode_tf = AZ_TF_SRGB;
	s.peak_scene = 1.0f;
	if (bpc > 8 || !o->scanout_srgb_view_ok) {
		s.path = AZ_OUTPUT_PATH_B_ENCODE;
		s.dither_q = quantum;
		return s;
	}
	s.path = AZ_OUTPUT_PATH_A_DIRECT_SRGB;
	s.dither_q = 0.0f; /* no encode pass to dither in; see the struct */
	return s;
}

/*
 * ── MAY AVK DRIVE THIS OUTPUT? (M5.6) ─────────────────────────────────────
 *
 * The refusal this replaces was one condition covering two unrelated things:
 *
 *     color_transform != NULL || output->image_description != NULL
 *
 * and only the second may be lifted. They are different questions:
 *
 *   an ICC / 3D-LUT transform  is ADR-000 scope: M6. AVK has no LUT stage and
 *                              must keep refusing, forever as far as M5 knows.
 *                              C3 already says so by deriving FALLBACK.
 *
 *   an image description       means the output is presenting HDR. AVK can now
 *                              drive that -- but ONLY through the C6 encode
 *                              pass, because the scan-out buffer is PQ-encoded
 *                              BT.2020 and compositing into it without the pass
 *                              writes scene-linear values as though they were
 *                              already electrical.
 *
 * THE INTERLOCK IS THE POINT. An HDR output accepted while the encode pass is
 * not going to run is far worse than an HDR output refused: refusing costs the
 * SceneFX fallback, accepting costs a wrong picture on the one content type the
 * user turned HDR on for. So acceptance requires the encode pass to be reachable
 * -- both C3 choosing Path B and the pass being enabled in this session.
 *
 * Pure, and separate from az_avk.h, so the decision table is a unit test rather
 * than something only a real HDR display can exercise.
 */
static inline bool az_output_may_drive(const struct az_output_color_state *s,
		bool has_image_description, bool has_color_transform,
		bool encode_pass_enabled) {
	if (s == NULL) {
		return false;
	}
	/* M6. Not a colour-path question -- AVK has nowhere to put a LUT. */
	if (has_color_transform || s->path == AZ_OUTPUT_PATH_FALLBACK) {
		return false;
	}
	if (has_image_description) {
		/* Every HDR output is Path B by ADR-008, so a state that says anything
		 * else here is either stale or a derivation bug, and both are reasons
		 * to refuse rather than to guess. */
		if (s->path != AZ_OUTPUT_PATH_B_ENCODE || s->encode_tf != AZ_TF_PQ) {
			return false;
		}
		return encode_pass_enabled;
	}
	/*
	 * SDR. Path A and Path B are both fine, and so is neither: an SDR output
	 * whose encode pass is disabled composites straight into its own buffer in
	 * its own encoding, which is what every version before M5 did and is not
	 * wrong -- only un-colour-managed.
	 */
	return true;
}

#endif /* AZ_OUTPUT_COLOR_H */
