/*
 * az_lum -- the per-window luminance domain, and the pure resolver that
 * compiles one. Contract C2, ADR-006.
 *
 * WHAT A DOMAIN IS. The complete recipe for taking ONE source's electrical
 * premultiplied pixels into scene values (ADR-002/003: linear-light BT.709/D65,
 * 1.0 = SDR reference white). Four fields, sixteen bytes, no pointers, no
 * output in it anywhere.
 *
 * WHAT A DOMAIN IS NOT. It is not a description of a display, and it must never
 * grow a field that is. A window straddling an HDR and an SDR output has ONE
 * domain and two output transforms (ADR-012, invariant 2); the moment a domain
 * knows which output it is on, the straddling case has two different intents
 * and the bug is unfixable without redesigning this struct.
 *
 * WHY THE WHOLE LUMINANCE MODEL IS ONE FLOAT. `scale` is the entire
 * absolute-to-relative conversion collapsed to a single multiply, resolved on
 * the CPU at surface commit. The renderer never sees a nit, never sees a
 * protocol enum, and never re-derives anything per frame -- it multiplies. The
 * alternative (carrying the wp-cm description into the command stream) makes
 * every frame recompute what changes when a client commits, which is to say
 * almost never.
 *
 * PURE. az_lum_resolve() reads its arguments and returns a value. It does not
 * log, which is why the "unknown enum" case is reported through
 * az_lum_source_valid() instead: the caller owns the once-per-surface log,
 * because "once per surface" is a fact about a surface and this file has never
 * heard of one.
 *
 * HEADER-ONLY, and that is a build decision with a reason: the resolver is
 * consumed by the scene walk in a performance-owned file (az_avk.h) whose
 * integration is deferred (ADR-013). A header with no translation unit adds no
 * source file to any target, so landing it now cannot make an owned file
 * relink.
 */
#ifndef AZ_LUM_H
#define AZ_LUM_H

#include <stdbool.h>
#include <stddef.h>

/* The electrical encoding of a source. M5's set; HLG is one more value when
 * ADR-000's scope moves, which is the reason this is an enum and not a bool
 * called `is_hdr`. */
enum az_tf {
	AZ_TF_SRGB = 0,
	AZ_TF_GAMMA22,
	AZ_TF_BT1886,
	AZ_TF_PQ,
	AZ_TF_LINEAR_EXT, /* scRGB: linear light, 1.0 == 80 cd/m2 */
	AZ_TF_COUNT,
};

/* For logs and test output. Not a parser: nothing reads these back. */
static inline const char *az_tf_name(enum az_tf tf) {
	switch (tf) {
	case AZ_TF_SRGB:       return "srgb";
	case AZ_TF_GAMMA22:    return "gamma2.2";
	case AZ_TF_BT1886:     return "bt1886";
	case AZ_TF_PQ:         return "pq";
	case AZ_TF_LINEAR_EXT: return "linear-ext";
	case AZ_TF_COUNT:      break;
	}
	return "?";
}

enum az_primaries {
	AZ_PRIM_BT709 = 0,
	AZ_PRIM_BT2020,
	AZ_PRIM_COUNT,
};

/* ITU-R BT.2408 diffuse white, and wlroots' PQ default. ADR-003's anchor when
 * the user has not set config.sdr_reference_luminance. */
#define AZ_SCENE_REF_DEFAULT 203.0f

/* PQ optical 1.0 is 10000 cd/m2 (ST 2084); scRGB 1.0 is 80 cd/m2 (the sRGB
 * spec's reference display white). Both are absolute, which is exactly why
 * they need converting into a relative scene at all. */
#define AZ_PQ_PEAK_NITS 10000.0f
#define AZ_SCRGB_WHITE_NITS 80.0f

/*
 * THE COMPILED RECIPE. 16 bytes, POD, copyable into a draw command.
 *
 * scale         linear gain into scene units, applied POST-EOTF and
 *               PRE-primaries-matrix. That ordering is not a detail: the matrix
 *               is a linear operator, so the two commute mathematically, but
 *               the tone map between them is not, and a scale applied on the
 *               wrong side of it changes which highlights roll off.
 * content_peak  the source's declared maximum, in SCENE units. 0 means
 *               UNKNOWN, not zero -- consumers must read it as "no per-source
 *               ceiling", never as "this source is black".
 */
struct az_lum_domain {
	enum az_tf tf;
	enum az_primaries primaries;
	float scale;
	float content_peak;
};

/*
 * What the compositor knows about a source, in plain values.
 *
 * NO WLROOTS TYPES, deliberately: the resolver is the part of M5 that most
 * needs to be exercised in a unit test, and a signature mentioning
 * wlr_color_transfer_function drags a wayland display into the test binary.
 * The adapter that translates scenefx's scene-buffer colour fields into this
 * struct is three switch statements and belongs with the scene walk.
 *
 * `tagged` is false for the overwhelming majority of surfaces -- no
 * wp-color-management description, no frog description. ADR-004 decides what
 * that means and the resolver implements it in one place.
 */
struct az_lum_source_desc {
	bool tagged;
	enum az_tf tf;
	enum az_primaries primaries;
	/* Content light level in cd/m2 from the source's metadata, 0 = absent. */
	float max_cll;
};

/*
 * The per-window rules, already merged for this client (C2's two new window
 * rules). 0 or negative means "unset", matching APPLY_FLOAT_PROP's convention
 * everywhere else in the config -- a rule that was never written must not be
 * distinguishable from one written as its default.
 *
 * sdr_white_scale  multiplies SDR sources. This is the per-window replacement
 *                  for a global HDR brightness slider, which ADR-006 forbids:
 *                  brightening one SDR app on an HDR output is a rule on that
 *                  window and touches nothing else on the screen.
 * hdr_gain         multiplies HDR sources (PQ, scRGB). Two windows of the same
 *                  video with different gains render at different absolute
 *                  luminance, which is the intent, not a bug.
 */
struct az_lum_rules {
	float sdr_white_scale;
	float hdr_gain;
};

static inline struct az_lum_rules az_lum_rules_default(void) {
	struct az_lum_rules r = {.sdr_white_scale = 1.0f, .hdr_gain = 1.0f};
	return r;
}

/* Is this description one the resolver can honour as written? False means the
 * resolver WILL fall back to the untagged default (ADR-004) -- the caller logs
 * it once for the surface and carries on. Never an error path: a client that
 * sends a transfer function from a newer protocol version must still get
 * pixels on the screen. */
static inline bool az_lum_source_valid(const struct az_lum_source_desc *src) {
	if (src == NULL) {
		return false;
	}
	if (!src->tagged) {
		return true; /* untagged is a valid state, not a malformed one */
	}
	/* Cast to int because an enum's underlying type may be unsigned, and
	 * `tf >= 0` on an unsigned type is a tautology the compiler folds away --
	 * which would make this check pass for the exact garbage it exists to
	 * catch. */
	return (int)src->tf >= 0 && (int)src->tf < (int)AZ_TF_COUNT &&
		   (int)src->primaries >= 0 && (int)src->primaries < (int)AZ_PRIM_COUNT;
}

static inline bool az_lum_tf_is_hdr(enum az_tf tf) {
	return tf == AZ_TF_PQ || tf == AZ_TF_LINEAR_EXT;
}

static inline float az_lum_ref_nits(float scene_ref_nits) {
	return scene_ref_nits > 0.0f ? scene_ref_nits : AZ_SCENE_REF_DEFAULT;
}

static inline float az_lum_rule_or(float v) { return v > 0.0f ? v : 1.0f; }

/*
 * The ADR-004 default, as a value.
 *
 * Exists so a consumer that has no source description at all -- an output
 * cursor, a solid rect, a blur result -- can hold a VALID domain rather than a
 * zeroed one. `scale > 0` is the resolver's contract and a consumer is
 * entitled to multiply by it; a zeroed struct's scale of 0 is not a neutral
 * default, it is black.
 */
static inline struct az_lum_domain az_lum_domain_untagged(void) {
	struct az_lum_domain d = {
		.tf = AZ_TF_SRGB,
		.primaries = AZ_PRIM_BT709,
		.scale = 1.0f,
		.content_peak = 0.0f,
	};
	return d;
}

/*
 * THE RESOLVER. Pure: same inputs, same domain, no state, no logging, no
 * allocation. Runs at surface commit and rule application, never per frame.
 *
 * The three scale rules are ADR-006's, and each is a conversion between two
 * definitions of white:
 *
 *   SDR      electrical 1.0 already MEANS SDR reference white, so the only
 *            factor is the window's own rule. Note what is absent: no
 *            reference-luminance term. An SDR-only desktop on an SDR output
 *            never multiplies by a luminance constant at all, which is what
 *            makes C4's round-trip gate a pure transfer-function identity.
 *   PQ       optical 1.0 is 10000 cd/m2 absolute; scene 1.0 is ref cd/m2. The
 *            ratio is the conversion. At the 203 default this is 49.2611, so a
 *            203-nit diffuse white in the content lands on scene 1.0 exactly --
 *            HDR paper white and SDR UI white coincide by default and diverge
 *            only when the user moves the reference.
 *   scRGB    the same conversion with 80 cd/m2 as the absolute anchor.
 *
 * The PQ and scRGB scales are inversely proportional to the reference, and the
 * SDR scale is independent of it. That asymmetry is ADR-003's whole claim, and
 * it is what makes `set_sdr_luminance` move the UI while leaving HDR media's
 * absolute nits alone -- the encode side cancels the decode side.
 */
static inline struct az_lum_domain
az_lum_resolve(const struct az_lum_source_desc *src,
			   const struct az_lum_rules *rules, float scene_ref_nits) {
	const float ref = az_lum_ref_nits(scene_ref_nits);
	const float sdr_white_scale =
		rules != NULL ? az_lum_rule_or(rules->sdr_white_scale) : 1.0f;
	const float hdr_gain =
		rules != NULL ? az_lum_rule_or(rules->hdr_gain) : 1.0f;

	/* ADR-004: untagged, and anything the resolver cannot read, is a
	 * piecewise-sRGB BT.709 SDR surface. One place, so a protocol addition
	 * cannot produce a second silent default somewhere else. */
	struct az_lum_domain d = {
		.tf = AZ_TF_SRGB,
		.primaries = AZ_PRIM_BT709,
		.scale = sdr_white_scale,
		.content_peak = 0.0f,
	};
	if (src == NULL || !src->tagged || !az_lum_source_valid(src)) {
		return d;
	}

	d.tf = src->tf;
	d.primaries = src->primaries;
	switch (src->tf) {
	case AZ_TF_PQ:
		d.scale = (AZ_PQ_PEAK_NITS / ref) * hdr_gain;
		break;
	case AZ_TF_LINEAR_EXT:
		d.scale = (AZ_SCRGB_WHITE_NITS / ref) * hdr_gain;
		break;
	case AZ_TF_SRGB:
	case AZ_TF_GAMMA22:
	case AZ_TF_BT1886:
	case AZ_TF_COUNT:
	default:
		d.scale = sdr_white_scale;
		break;
	}

	/* max_cll is absolute cd/m2 and the domain is relative, so it converts
	 * through the same anchor everything else does. It is NOT multiplied by
	 * the gain: a gain is the user's choice about presentation, whereas
	 * content_peak is a statement about the master, and folding one into the
	 * other would let a window rule lie to the tone mapper about what the
	 * content contains. */
	if (src->max_cll > 0.0f) {
		d.content_peak = src->max_cll / ref;
	}

	/* Invariant: scale > 0 always. A zero would make a window invisible and a
	 * negative one would invert it; both are reachable from a malformed rule
	 * and neither should be reachable from here. */
	if (!(d.scale > 0.0f)) {
		d.scale = 1.0f;
	}
	return d;
}

#endif /* AZ_LUM_H */
