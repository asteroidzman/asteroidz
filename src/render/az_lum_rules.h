#ifndef AZ_LUM_RULES_H
#define AZ_LUM_RULES_H

/*
 * ── WHICH LUMINANCE RULES APPLY TO THIS SURFACE. ONE ANSWER. ──────────────
 *
 * M12. Two callers need it: the renderer, resolving a domain for a buffer it
 * is about to draw, and the inspector, reporting what was decided. If they
 * computed it separately the inspector would eventually describe a policy the
 * renderer had stopped applying -- which is the failure mode this compositor
 * has already had twice (az_preferred.h's two frontends, az_source_desc.h's
 * two colour translations) and does not need a third of.
 *
 * It lives between client.h and az_avk.h in the include order because it is
 * the seam: it needs Client to read a rule, and the renderer needs it.
 */

#include "az_lum.h"

/*
 * THE PRECEDENCE, in one place:
 *
 *   1. the window rule's explicit multipliers   (most specific)
 *   2. the window rule's luminance class
 *   3. the class derived from what the client declared
 *   4. the default, which changes nothing
 *
 * A window given both a class and an explicit scale gets the explicit scale:
 * the specific instruction beats the category, as everywhere else here.
 *
 * An UNPARSEABLE class keeps the derived one. A typo must not quietly become a
 * policy, and the derived answer is what was correct before the rule existed.
 *
 * LAYER SURFACES GET THEIR OWN RULE, via `layerrule`. They cannot use a window
 * rule -- those match app-id and title, and a layer surface has neither -- but a
 * bar and a wallpaper are the most chrome-like things on a desktop, and until
 * M13 they were the only surfaces with no way to say so. They carry the class
 * only: `sdr-white-scale` and `hdr-gain` remain window-rule properties, because
 * a layer surface has no per-window luminance lever and inventing one would be
 * two spellings for the same thing.
 */
static inline struct az_lum_rules az_lum_rules_for_surface(
		struct wlr_surface *surface, const struct az_lum_source_desc *src,
		float scene_ref_nits, enum az_lum_class *out_class,
		bool *out_from_rule) {
	enum az_lum_class cls = az_lum_class_derive(src, false,
		AZ_LUM_CLASS_SDR_NORMAL);
	bool from_rule = false;
	struct az_lum_rules r = az_lum_class_rules(cls, scene_ref_nits);

	Client *c = NULL;
	LayerSurface *l = NULL;
	if (surface != NULL && toplevel_from_wlr_surface(surface, &c, &l) >= 0) {
		/* One of the two, never both: the resolver sets whichever role the
		 * surface has. A layer surface carries a class and nothing else. */
		const char *ruled_name = c != NULL ? c->luminance_domain
			: (l != NULL ? l->luminance_domain : NULL);
		enum az_lum_class ruled;
		if (az_lum_class_from_name(ruled_name, &ruled)) {
			cls = ruled;
			from_rule = true;
			r = az_lum_class_rules(cls, scene_ref_nits);
		}
		if (c != NULL) {
			/* The Client initialises both to 1.0 and APPLY_FLOAT_PROP only
			 * overwrites above zero, so "set to something other than
			 * neutral" is the test -- an explicit 1.0 and an unset rule mean
			 * the same thing and must not stamp over a class. */
			if (c->sdr_white_scale > 0.0f && c->sdr_white_scale != 1.0f) {
				r.sdr_white_scale = c->sdr_white_scale;
			}
			if (c->hdr_gain > 0.0f && c->hdr_gain != 1.0f) {
				r.hdr_gain = c->hdr_gain;
			}
		}
	}

	if (out_class != NULL) {
		*out_class = cls;
	}
	if (out_from_rule != NULL) {
		*out_from_rule = from_rule;
	}
	return r;
}

#endif /* AZ_LUM_RULES_H */
