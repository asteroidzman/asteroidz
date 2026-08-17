#ifndef AZ_PREFERRED_H
#define AZ_PREFERRED_H

/*
 * ── WHAT A SURFACE'S DISPLAY PREFERS. ONE ANSWER, TWO PROTOCOLS. ──────────
 *
 * M6B/D6. Asteroidz speaks two colour-management protocols to clients:
 *
 *   frog-color-management-v1   implemented here, in this tree. Carries the
 *                              FULL mastering metadata -- primaries, max and
 *                              min luminance, max FALL -- and is what gamescope
 *                              uses.
 *
 *   wp-color-management-v1     mediated by wlroots. Carries the transfer
 *                              function and primaries. wlroots 0.20.2 does not
 *                              yet serialize the mastering-luminance or
 *                              content-light-level events (two TODOs in
 *                              types/wlr_color_management_v1.c), so a wp-cm
 *                              client currently learns THAT a display is HDR
 *                              and not HOW BRIGHT it goes.
 *
 * THE TWO PATHS HAVE DIFFERENT EXPRESSIVE POWER AND THAT IS STATED, NOT
 * PAPERED OVER. What they must NEVER have is different opinions: this file is
 * the single policy both of them serialize, so "frog says DP-1, wp-cm says
 * HDMI-A-1 for the same surface" cannot happen.
 *
 * It exists because it already did happen. frog resolved its output as "the
 * first enabled HDR monitor, else selmon" while wp-cm resolved the surface's
 * own -- so a window living entirely on the SDR panel was handed the HDR
 * panel's primaries and its 400-nit ceiling, and told to tone-map for a display
 * it was not on. Two protocol frontends with two policies drifted apart within
 * one milestone of each other being written.
 *
 * NOTHING HERE IS A FRAME EVENT. Everything is a function of which output a
 * surface is on and what colour state that output is in, and both change on
 * hotplug, on a layout move, and on an HDR toggle.
 */

#include <stdbool.h>
#include <stdint.h>

/*
 * THE EFFECTIVE OUTPUT FOR A SURFACE.
 *
 * `c->mon` and nothing else. Asteroidz already maintains exactly one output per
 * client -- setmon() is its only writer, the layout decides it, and every other
 * per-output decision in the compositor already follows it. A surface
 * straddling two monitors therefore has a deterministic answer that agrees with
 * where its borders are drawn, where its blur is clipped, and which output's
 * frame callbacks it gets.
 *
 * NO selmon FALLBACK. A surface that is not on an output has no preferred
 * display, and inventing one means telling a client to tone-map for a monitor
 * it is not on -- which is the defect this file was written to end. Callers get
 * NULL and must simply say nothing until the surface lands somewhere; the
 * setmon path re-sends the moment it does.
 */
static inline Monitor *az_surface_effective_output(struct wlr_surface *surface) {
	if (surface == NULL) {
		return NULL;
	}
	/*
	 * BREAK: AZ_BREAK_FROG_FIRST_HDR_OUTPUT reinstates the policy this file
	 * replaced -- "the first enabled HDR output, else the focused one" -- so
	 * the wrong-display defect can be reproduced on demand rather than only
	 * described. It is deliberately spelled as the OLD CODE, not as a
	 * perturbation of the new one: a falsifier that merely returns a different
	 * monitor would prove a fixture can notice a change, where this proves it
	 * notices THAT change.
	 */
	static int break_first_hdr = -1;
	if (break_first_hdr < 0) {
		break_first_hdr = getenv("AZ_BREAK_FROG_FIRST_HDR_OUTPUT") != NULL;
	}
	if (break_first_hdr) {
		Monitor *it;
		wl_list_for_each(it, &mons, link) {
			if (it->wlr_output->enabled && it->hdr) {
				return it;
			}
		}
		return selmon;
	}
	/*
	 * ── EVERY SURFACE ROLE, NOT JUST TOPLEVELS ────────────────────────────
	 *
	 * This used to walk `clients` comparing client_surface(c) == surface, and
	 * client_surface() returns a toplevel's OWN wl_surface and nothing else.
	 * So a layer-shell surface, a subsurface, an xdg popup or an XWayland
	 * override-redirect window resolved to NULL -- and NULL here means "no
	 * preferred display", which both frontends correctly render as silence.
	 * frog's send returned false and wp-cm's get_preferred fell through to the
	 * hard-coded sRGB/203 default, permanently.
	 *
	 * That is not a corner: asteroidz-bar's own wallpaper backdrop is a
	 * LAYER-SHELL wp-cm client that renders HDR10, and it was being told
	 * "sRGB, 203 nits" on a PQ/BT.2020 DP-1 for the life of the session.
	 *
	 * toplevel_from_wlr_surface() already answers this for the whole
	 * compositor -- it resolves through wlr_surface_get_root_surface(), walks
	 * an xdg popup up to its parent toplevel, and hands back a Client OR a
	 * LayerSurface. Both carry the `mon` the layout assigned, so the answer
	 * still comes from setmon() and still agrees with where borders are drawn.
	 *
	 * Roles with no output of their own -- a drag icon, a session-lock
	 * surface, an unassigned xdg_surface -- come back as -1 and still resolve
	 * to NULL, which is the correct answer for them rather than a gap.
	 */
	Client *c = NULL;
	LayerSurface *l = NULL;
	int32_t type = toplevel_from_wlr_surface(surface, &c, &l);
	if (type < 0) {
		return NULL;
	}
	if (type == LayerShell) {
		return l != NULL ? l->mon : NULL;
	}
	return c != NULL ? c->mon : NULL;
}

/*
 * THE RESOLVED DESCRIPTION, in asteroidz's own units.
 *
 * Values are the monitor rule's, unnormalised and uninterpreted: a protocol
 * frontend converts to whatever units its wire format wants, and does not get
 * to decide what the numbers mean. The operator's DP-1 rule says
 * max-luminance 400, min-luminance 0.4, max-fall 250, and both frontends must
 * be able to show those exact numbers arriving.
 */
struct az_preferred {
	Monitor *mon;   /* the effective output; NULL = nothing to say yet */
	bool hdr;       /* the output is presenting an HDR image description */
	bool bt2020;    /* primaries: BT.2020 when HDR, BT.709/sRGB otherwise */
	float max_luminance; /* cd/m2 */
	float min_luminance; /* cd/m2 */
	float max_fall;      /* cd/m2 */
	/*
	 * THE IDENTITY OF ALL OF THE ABOVE.
	 *
	 * A frontend caches this per client object and re-sends only when it
	 * changes, so an output-state change that does not alter what a surface
	 * should prefer produces ZERO protocol traffic. Without it, "re-send when
	 * something happened" degrades into re-sending on every hotplug, every
	 * layout change and every HDR toggle on an unrelated monitor.
	 *
	 * Zero is reserved for "no output", so a frontend can use 0 as its initial
	 * cached value and be guaranteed to send once.
	 */
	uint64_t identity;
};

/* FNV-1a over the tuple. A hash rather than a struct comparison because the
 * frontends cache it across events and a stale pointer inside a cached copy of
 * `mon` would be a use-after-free waiting for a hotplug. */
static inline void az_preferred_mix(uint64_t *h, const void *p, size_t n) {
	const unsigned char *b = p;
	for (size_t i = 0; i < n; i++) {
		*h ^= b[i];
		*h *= 1099511628211ULL;
	}
}

static inline void az_preferred_resolve(struct wlr_surface *surface,
		struct az_preferred *out) {
	*out = (struct az_preferred){0};
	Monitor *m = az_surface_effective_output(surface);
	if (m == NULL || m->wlr_output == NULL) {
		return;
	}
	out->mon = m;
	out->hdr = m->hdr > 0;
	out->bt2020 = out->hdr;
	if (out->hdr) {
		/* The rule's values, with the same defaults mon_state_apply_color uses
		 * when a rule is silent -- so what a client is told and what the
		 * connector is told cannot disagree. */
		out->max_luminance = m->hdr_max_luminance > 0
			? m->hdr_max_luminance : 1000.0f;
		out->min_luminance = m->hdr_min_luminance >= 0
			? m->hdr_min_luminance : 0.005f;
		out->max_fall = m->hdr_max_fall > 0
			? m->hdr_max_fall : out->max_luminance;
	} else {
		/* SDR: the scene reference, which is what an SDR display's white IS
		 * under ADR-003. Not a panel measurement and not claimed as one. */
		out->max_luminance = config.sdr_reference_luminance > 0
			? config.sdr_reference_luminance : 203.0f;
		out->min_luminance = 0.2f;
		out->max_fall = out->max_luminance;
	}

	uint64_t h = 1469598103934665603ULL;
	/* The output's NAME, not its pointer: a monitor destroyed and recreated at
	 * the same address must not read as unchanged. */
	const char *name = m->wlr_output->name != NULL ? m->wlr_output->name : "?";
	az_preferred_mix(&h, name, strlen(name));
	az_preferred_mix(&h, &out->hdr, sizeof(out->hdr));
	az_preferred_mix(&h, &out->bt2020, sizeof(out->bt2020));
	az_preferred_mix(&h, &out->max_luminance, sizeof(out->max_luminance));
	az_preferred_mix(&h, &out->min_luminance, sizeof(out->min_luminance));
	az_preferred_mix(&h, &out->max_fall, sizeof(out->max_fall));
	/* Never 0: that value means "nothing to say" to every frontend. */
	out->identity = h != 0 ? h : 1;
}

#endif /* AZ_PREFERRED_H */
