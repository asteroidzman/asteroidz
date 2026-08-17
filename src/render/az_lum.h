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
#include <string.h>

/* The electrical encoding of a source. M5's set; HLG is one more value when
 * ADR-000's scope moves, which is the reason this is an enum and not a bool
 * called `is_hdr`. */
enum az_tf {
	AZ_TF_SRGB = 0,
	AZ_TF_GAMMA22,
	AZ_TF_BT1886,
	AZ_TF_PQ,
	AZ_TF_LINEAR_EXT, /* scRGB: linear light, 1.0 == 80 cd/m2 */
	/*
	 * M6B. A measured display curve -- TRC^-1 with the profile's vcgt composed
	 * on -- carried as a table rather than a formula, because a vcgt has no
	 * closed form. Only ever an OUTPUT encode: no source declares it.
	 */
	AZ_TF_LUT1D,
	/*
	 * M6C. A measured display carried as a 3D table instead of a matrix and a
	 * curve, for the profiles that do not reduce to those -- a cLUT (A2B/B2A)
	 * profile. Like LUT1D it is only ever an OUTPUT encode, and unlike LUT1D it
	 * carries the GAMUT as well, which is why the state that names it also
	 * derives the identity matrix. See az_icc.h.
	 */
	AZ_TF_CLUT3D,
	AZ_TF_COUNT,
};

/* For logs and test output. Not a parser: nothing reads these back. */
static inline const char *az_tf_name(enum az_tf tf) {
	switch (tf) {
	case AZ_TF_SRGB:       return "srgb";
	case AZ_TF_GAMMA22:    return "gamma2.2";
	case AZ_TF_BT1886:     return "bt1886";
	case AZ_TF_PQ:         return "pq";
	case AZ_TF_LUT1D:      return "lut1d";
	case AZ_TF_CLUT3D:     return "clut3d";
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
 * ── M12: WHAT LUMINANCE ROLE DOES THIS CONTENT HAVE? ──────────────────────
 *
 * A transfer function says how a source is ENCODED. It does not say what the
 * content is FOR, and on an HDR output those are different questions: a
 * terminal and a film are both sRGB-encoded and should not share a white.
 *
 * The class is a NAMED DEFAULT for az_lum_rules, not a second mechanism. It
 * resolves to the same two multipliers the per-window rules already set, so
 * there is exactly one thing the pipeline applies and one place it is applied
 * (az_lum_resolve, post-EOTF, pre-matrix, in scene-linear). Nothing here is a
 * post-encode brightness knob, and nothing here is per-frame.
 */
enum az_lum_class {
	/* Conventional SDR content at the scene reference. THE DEFAULT, and the
	 * reason is below. */
	AZ_LUM_CLASS_SDR_NORMAL = 0,
	/* Desktop furniture: terminal, panel, launcher. Restrained white. */
	AZ_LUM_CLASS_SDR_UI,
	/* Wide-gamut SDR -- photography, BT.2020-primaries SDR sources. */
	AZ_LUM_CLASS_SDR_EXTENDED,
	/* HDR video, HDR games, HDR stills: absolute luminance semantics. */
	AZ_LUM_CLASS_HDR_CONTENT,
	AZ_LUM_CLASS_COUNT,
};

static inline const char *az_lum_class_name(enum az_lum_class c) {
	switch (c) {
	case AZ_LUM_CLASS_SDR_NORMAL:   return "sdr-normal";
	case AZ_LUM_CLASS_SDR_UI:       return "sdr-ui";
	case AZ_LUM_CLASS_SDR_EXTENDED: return "sdr-extended";
	case AZ_LUM_CLASS_HDR_CONTENT:  return "hdr-content";
	case AZ_LUM_CLASS_COUNT:        break;
	}
	return "?";
}

/* Unlike az_tf_name(), this one IS a parser: a window rule spells the class as
 * a string. Returns false for anything unrecognised, and the caller keeps the
 * derived class -- a typo in a rule must not silently become a policy. */
static inline bool az_lum_class_from_name(const char *s,
		enum az_lum_class *out) {
	if (s == NULL || *s == '\0' || out == NULL) {
		return false;
	}
	for (int i = 0; i < (int)AZ_LUM_CLASS_COUNT; i++) {
		if (strcmp(s, az_lum_class_name((enum az_lum_class)i)) == 0) {
			*out = (enum az_lum_class)i;
			return true;
		}
	}
	return false;
}

/* ITU-R BT.2408 diffuse white: what SDR_UI's white means in absolute terms,
 * independent of where the user put the scene reference. */
#define AZ_LUM_UI_WHITE_NITS 203.0f

/*
 * THE DERIVATION, and the one judgement in it stated plainly.
 *
 * Order: an explicit rule wins, then what the source declared, then the
 * default. What is NOT here is any attempt to tell a terminal from a film by
 * inspection -- nothing in any protocol distinguishes them, and guessing would
 * mean occasionally dimming a movie because it was untagged.
 *
 * SO UNTAGGED DEFAULTS TO SDR_NORMAL, NOT SDR_UI. That is a deviation from the
 * obvious reading of "UI should be restrained", and it is deliberate: SDR_UI as
 * the default would silently dim every ordinary window on an HDR output the
 * moment this shipped, for every user, with no rule written. The project's own
 * standard -- automatic semantics must never be REQUIRED for correct operation
 * -- cuts the other way here: the safe automatic answer is the one that changes
 * nothing, and SDR_UI is opt-in per window.
 *
 * BT.2020 primaries with an SDR transfer IS derivable, and is the one automatic
 * non-default answer: that is wide-gamut SDR by construction.
 */
static inline enum az_lum_class az_lum_class_derive(
		const struct az_lum_source_desc *src, bool rule_set,
		enum az_lum_class rule_class) {
	if (rule_set) {
		return rule_class;
	}
	if (src != NULL && src->tagged && az_lum_source_valid(src)) {
		if (az_lum_tf_is_hdr(src->tf)) {
			return AZ_LUM_CLASS_HDR_CONTENT;
		}
		if (src->primaries == AZ_PRIM_BT2020) {
			return AZ_LUM_CLASS_SDR_EXTENDED;
		}
	}
	return AZ_LUM_CLASS_SDR_NORMAL;
}

/*
 * THE CLASS AS RULES. One conversion, so the class cannot grow a second
 * pipeline of its own.
 *
 * SDR_UI targets AZ_LUM_UI_WHITE_NITS absolute, which is why the scale is
 * reference-relative: at the 203 default it is exactly 1.0 and costs nothing,
 * and at the operator's 280 it is 0.725.
 *
 * IT NEVER EXCEEDS 1.0. With the reference below 203 the UI is already dimmer
 * than the target, and amplifying it would make "restrained" mean "brighter
 * than normal content" -- a class named for restraint must not brighten.
 */
static inline struct az_lum_rules az_lum_class_rules(enum az_lum_class c,
		float scene_ref_nits) {
	struct az_lum_rules r = az_lum_rules_default();
	if (c == AZ_LUM_CLASS_SDR_UI) {
		const float ref = az_lum_ref_nits(scene_ref_nits);
		float s = AZ_LUM_UI_WHITE_NITS / ref;
		r.sdr_white_scale = s < 1.0f ? s : 1.0f;
	}
	return r;
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
