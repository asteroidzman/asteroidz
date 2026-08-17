#ifndef AZ_INTENT_H
#define AZ_INTENT_H

/*
 * ── WHAT ASTEROIDZ IS DOING TO THIS SURFACE. ONE ANSWER, FROM PRODUCTION. ──
 *
 * M11. Linux HDR and presentation failures are opaque in a specific way: every
 * layer is individually correct and the composition of them is wrong, so the
 * question that actually needs answering is never "is the code right" but
 * "which of the eight things that decide this pixel disagreed". This file
 * aggregates those eight things for one surface, so the answer can be read
 * instead of reconstructed.
 *
 * ── IT RESOLVES, IT DOES NOT STORE ────────────────────────────────────────
 *
 * There is no `intent` field on Client and nothing caches this. It is a pure
 * function of state that already exists, computed when someone asks, exactly
 * like az_preferred_resolve() -- which is the pattern this follows on purpose.
 * A stored authoritative copy would be a second source of truth for colour and
 * presentation policy, which is the one thing this milestone must not create:
 * the inspector's whole value is that it reports what production decided, and
 * a snapshot that can disagree with production is worse than no snapshot.
 *
 * So every field below is either copied from a live struct or computed by the
 * same function production calls. Nothing here re-derives a decision.
 *
 * ── WHAT IS DELIBERATELY ABSENT ───────────────────────────────────────────
 *
 * No luminance CLASS (SDR_UI / HDR_CONTENT / ...) and no presentation CLASS
 * (DESKTOP_UI / GAME / VIDEO). Those are M12's and M13's, and they do not exist
 * yet -- so this reports the source's transfer function, primaries, resolved
 * scale and content peak, which is the real information the classes will be
 * derived FROM. An enum whose every value is UNSET would be a slot pretending
 * to be an answer.
 *
 * Direct scanout is reported as not happening, with a reason, because at HEAD
 * it genuinely never happens: scene_entry_try_direct_scanout() has no caller.
 * That is a true statement about this compositor and it is better said than
 * omitted -- M13 revives the path and replaces the constant with a real one.
 */

#include <stdbool.h>
#include <stdint.h>

/*
 * The snapshot. POD apart from `mon`, which is borrowed for the life of the
 * call only -- callers serialize immediately and never cache this struct, for
 * the same reason az_preferred carries an identity instead of a monitor
 * pointer.
 */
struct az_surface_intent {
	/* ── placement ─────────────────────────────────────────────────────── */
	Monitor *mon;          /* the effective output; NULL = on none */

	/* ── the buffer, as attached ───────────────────────────────────────── */
	bool has_buffer;
	int buf_width, buf_height;
	bool buf_dmabuf;
	bool buf_shm;
	uint32_t buf_format;   /* DRM fourcc, 0 = unknown */
	uint64_t buf_modifier; /* DRM modifier; only meaningful for dmabuf */

	/* ── source colour, read the way the renderer reads it ─────────────── */
	struct az_lum_source_desc src;   /* tagged/tf/primaries/max_cll */
	struct az_lum_domain domain;     /* az_lum_resolve() of the above */

	/* ── M12: the luminance class, and where it came from ──────────────── */
	enum az_lum_class lum_class;
	bool lum_class_from_rule;  /* false = derived from what the client said */
	struct az_lum_rules lum_rules;  /* the multipliers actually applied */

	/* ── output colour, copied from the output's derived state ─────────── */
	bool have_output;
	struct az_output_color_state ocs;
	bool output_hdr;
	bool output_icc;
	int output_bpc;

	/* ── what this surface is TOLD to prefer (both protocol frontends) ─── */
	struct az_preferred pref;

	/* ── presentation, from the output's presenter ─────────────────────── */
	enum az_present_regime regime;
	uint64_t nominal_period_ns;  /* from the mode; under VRR a floor */
	uint64_t period_ns;          /* what the presenter actually uses */
	uint64_t err_count;
	int64_t err_mean_ns;         /* signed: actual - predicted */
	uint64_t misses;
	uint64_t prediction_exceeded;

	/* ── render path ───────────────────────────────────────────────────── */
	bool scanout;
	const char *scanout_why;     /* never NULL */

	/*
	 * THE IDENTITY OF EVERYTHING ABOVE THAT IS A DECISION.
	 *
	 * Timing counters are deliberately EXCLUDED: they change every frame, and
	 * an identity that changes every frame cannot be used to answer "did
	 * anything about this surface's treatment change", which is the only
	 * question it is for. What is mixed in is the colour and path state.
	 */
	uint64_t identity;
};

/*
 * WHY SCANOUT DID NOT HAPPEN.
 *
 * One constant today, and the reason is structural rather than per-surface:
 * AVK composes every frame and the scene's scanout decision function is
 * unreachable. M13 turns this into the real per-surface enum the brief asks
 * for ("modifier not scanout-capable", "HDR source needs compositor tone
 * mapping", ...). Naming it now, in the place that will carry it, keeps the
 * inspector's shape stable across that change.
 */
#define AZ_SCANOUT_WHY_NOT_IMPLEMENTED \
	"composition always: AVK has no direct-scanout path (M13)"

static inline void az_surface_intent_resolve(struct wlr_surface *surface,
		struct az_surface_intent *out) {
	*out = (struct az_surface_intent){0};
	out->scanout = false;
	out->scanout_why = AZ_SCANOUT_WHY_NOT_IMPLEMENTED;
	out->domain = az_lum_domain_untagged();
	if (surface == NULL) {
		return;
	}

	/* ── the buffer ────────────────────────────────────────────────────── */
	struct wlr_buffer *buf = az_surface_buffer(surface);
	if (buf != NULL) {
		out->has_buffer = true;
		out->buf_width = buf->width;
		out->buf_height = buf->height;
		struct wlr_dmabuf_attributes dmabuf;
		struct wlr_shm_attributes shm;
		if (wlr_buffer_get_dmabuf(buf, &dmabuf)) {
			out->buf_dmabuf = true;
			out->buf_format = dmabuf.format;
			out->buf_modifier = dmabuf.modifier;
		} else if (wlr_buffer_get_shm(buf, &shm)) {
			out->buf_shm = true;
			out->buf_format = shm.format;
		}
	}

	/*
	 * ── SOURCE COLOUR ─────────────────────────────────────────────────────
	 *
	 * The same two lookups src/scene/surface.c makes, in the same order:
	 * wp-color-management's own description first, then the registered
	 * fallback -- which is az_cm_surface_description(), the multiplexer that
	 * puts native wp-cm ahead of frog. Reading it here rather than reading the
	 * scene buffer means an unmapped or never-drawn surface still answers.
	 */
	const struct wlr_image_description_v1_data *img =
		wlr_surface_get_image_description_v1_data(surface);
	if (img == NULL) {
		img = az_cm_surface_description(surface);
	}
	if (img != NULL) {
		out->src = az_source_desc_from_wlr(
			wlr_color_manager_v1_transfer_function_to_wlr(img->tf_named),
			wlr_color_manager_v1_primaries_to_wlr(img->primaries_named),
			img->max_cll);
	}

	/* ── the effective output, and everything that follows from it ─────── */
	Monitor *m = az_surface_effective_output(surface);
	out->mon = m;
	az_preferred_resolve(surface, &out->pref);

	/*
	 * The scene reference is the output's, not a constant: az_lum_resolve()'s
	 * PQ and scRGB scales are inversely proportional to it, so resolving with
	 * the wrong reference reports a scale the renderer never used.
	 */
	float ref = AZ_SCENE_REF_DEFAULT;
	if (m != NULL) {
		out->have_output = true;
		out->ocs = m->color_state;
		out->output_hdr = m->hdr > 0;
		out->output_icc = m->icc_transform != NULL;
		out->output_bpc = m->color_state.encode_tf == AZ_TF_PQ ? 10 : 8;
		if (m->color_state.ref_nits > 0.0f) {
			ref = m->color_state.ref_nits;
		}
		out->regime = m->presenter.regime;
		out->nominal_period_ns = m->presenter.nominal_period_ns;
		out->period_ns = az_presenter_period_ns(m);
		out->err_count = m->presenter.err_count;
		out->err_mean_ns = m->presenter.err_count > 0
			? m->presenter.err_sum_ns / (int64_t)m->presenter.err_count : 0;
		out->misses = m->presenter.misses;
		out->prediction_exceeded = m->presenter.prediction_exceeded;
	}

	/*
	 * THE SAME FUNCTION THE RENDERER CALLS, not a copy of its reasoning. If
	 * this recomputed the precedence, the inspector would eventually report a
	 * policy the renderer had stopped applying -- and an inspector that can
	 * disagree with production is worse than none.
	 */
	out->lum_rules = az_lum_rules_for_surface(surface, &out->src, ref,
		&out->lum_class, &out->lum_class_from_rule);
	out->domain = az_lum_resolve(&out->src, &out->lum_rules, ref);

	/* ── identity: the decisions, never the counters ───────────────────── */
	uint64_t h = 1469598103934665603ULL;
	const char *name = (m != NULL && m->wlr_output != NULL
		&& m->wlr_output->name != NULL) ? m->wlr_output->name : "-";
	az_preferred_mix(&h, name, strlen(name));
	az_preferred_mix(&h, &out->src, sizeof(out->src));
	az_preferred_mix(&h, &out->domain, sizeof(out->domain));
	az_preferred_mix(&h, &out->lum_class, sizeof(out->lum_class));
	az_preferred_mix(&h, &out->lum_rules, sizeof(out->lum_rules));
	az_preferred_mix(&h, &out->ocs.path, sizeof(out->ocs.path));
	az_preferred_mix(&h, &out->ocs.encode_tf, sizeof(out->ocs.encode_tf));
	az_preferred_mix(&h, &out->pref.identity, sizeof(out->pref.identity));
	az_preferred_mix(&h, &out->scanout, sizeof(out->scanout));
	az_preferred_mix(&h, &out->buf_format, sizeof(out->buf_format));
	az_preferred_mix(&h, &out->buf_modifier, sizeof(out->buf_modifier));
	out->identity = h != 0 ? h : 1;
}

/* For the dump. Not a parser; nothing reads these back. */
static inline const char *az_primaries_name(enum az_primaries p) {
	switch (p) {
	case AZ_PRIM_BT709:  return "bt709";
	case AZ_PRIM_BT2020: return "bt2020";
	case AZ_PRIM_COUNT:  break;
	}
	return "?";
}

#endif /* AZ_INTENT_H */
