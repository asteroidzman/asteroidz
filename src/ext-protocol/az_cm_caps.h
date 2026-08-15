#ifndef AZ_CM_CAPS_H
#define AZ_CM_CAPS_H

/*
 * ── WHAT THIS COMPOSITOR CAN BE TOLD, WRITTEN ONCE ───────────────────────
 *
 * The transfer functions and primaries a client may tag a surface with. Two
 * implementations of wp-color-management-v1 will advertise them -- wlroots'
 * today, asteroidz's own once native ownership lands -- and they must advertise
 * the SAME SET. Two lists built in two places is how a capability set drifts,
 * and this file exists because a colour-policy drift of exactly that shape has
 * already happened once in this tree (see src/render/az_preferred.h).
 *
 * ── THE LIST DESCRIBES WHATEVER DECODES THE SURFACE ──────────────────────
 *
 * Not the wlroots compatibility renderer, which composites nothing. Derived
 * from `drw` the list came back EMPTY, so `wayland-info` reported no named
 * transfer functions and no named primaries and NO CLIENT COULD ATTACH AN
 * IMAGE DESCRIPTION AT ALL. mpv would announce "transfer: pq, primaries:
 * bt.2020", fail to create it, tone-map its own HDR10 down to SDR and hand
 * over gamma2.2/BT.709 -- which the compositor then re-encoded to PQ for the
 * panel. AVK's PQ decode was unreachable for that reason alone and nothing in
 * the renderer could have fixed it.
 *
 * ── AND EVERY VALUE COMES FROM A wlroots ENUM, NEVER A PROTOCOL ONE ──────
 *
 * wlr_color_manager_v1_transfer_function_to_wlr() ABORTS when a protocol value
 * has no matching wlroots entry, and scenefx calls it on whatever a client
 * attaches. Advertising a value wlroots cannot map does not degrade -- it kills
 * the compositor as soon as any client uses it, which at login is immediately.
 *
 * Starting from wlroots' own enum values and mapping outward with _from_wlr()
 * (which has no such abort) makes that impossible by construction: every value
 * advertised came from a wlroots entry, so a matching entry necessarily exists
 * on the way back. The defensive alternative -- calling _to_wlr() to check --
 * would be calling the aborting function to find out whether it aborts.
 *
 * That property is why native ownership can be safe: the native creator will
 * validate client input against THIS set, so nothing that reaches scenefx can
 * be unmappable.
 */

/*
 * ── AND THE LISTS OUTLIVE EVERY CALLER ───────────────────────────────────
 *
 * There is no finish function and nothing frees these. The manager reads them
 * on every bind for the life of the session, so whichever way they were built
 * -- static storage on the AVK path, heap from wlroots' renderer helpers on the
 * other -- they are process-lifetime. The one that used to free them ran on the
 * wlroots-manager path, which no longer exists; freeing them now would hand the
 * manager a dangling list to advertise from on the next client that connects.
 */
struct az_cm_caps {
	const enum wp_color_manager_v1_transfer_function *tfs;
	size_t tf_len;
	const enum wp_color_manager_v1_primaries *primaries;
	size_t primaries_len;
};

/* Exactly the curves AVK has a decode variant for -- see enum
 * avk_decode_variant and texture.frag. EXT_LINEAR is deliberately absent: AVK
 * handles it correctly (a linear source needs no EOTF, only the domain's
 * scale) but it has no variant of its own and therefore no test, and
 * advertising a curve nothing has measured is how a client is invited to send
 * something that comes out wrong. */
static const enum wlr_color_transfer_function az_cm_avk_wlr_tfs[] = {
	WLR_COLOR_TRANSFER_FUNCTION_SRGB,
	WLR_COLOR_TRANSFER_FUNCTION_GAMMA22,
	WLR_COLOR_TRANSFER_FUNCTION_BT1886,
	WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ,
};
static const enum wlr_color_named_primaries az_cm_avk_wlr_prims[] = {
	WLR_COLOR_NAMED_PRIMARIES_SRGB,   /* the scene's own, BT.709 */
	WLR_COLOR_NAMED_PRIMARIES_BT2020, /* converted at decode */
};

static enum wp_color_manager_v1_transfer_function az_cm_avk_tfs[8];
static enum wp_color_manager_v1_primaries az_cm_avk_prims[4];

/* Perceptual only. The one intent this compositor implements; advertising
 * others would invite a client to ask for a rendering it will not get. */
static const enum wp_color_manager_v1_render_intent az_cm_render_intents[] = {
	WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL,
};

static void az_cm_caps_build(struct az_cm_caps *out, struct wlr_renderer *drw) {
	*out = (struct az_cm_caps){0};
#ifdef AZ_HAVE_VULKAN
	if (az_renderer == AZ_RENDERER_AVK) {
		size_t n = 0, m = 0;
		for (size_t i = 0; i < LENGTH(az_cm_avk_wlr_tfs); i++) {
			az_cm_avk_tfs[n++] =
				wlr_color_manager_v1_transfer_function_from_wlr(
					az_cm_avk_wlr_tfs[i]);
		}
		for (size_t i = 0; i < LENGTH(az_cm_avk_wlr_prims); i++) {
			az_cm_avk_prims[m++] =
				wlr_color_manager_v1_primaries_from_wlr(
					az_cm_avk_wlr_prims[i]);
		}
		out->tfs = az_cm_avk_tfs;
		out->tf_len = n;
		out->primaries = az_cm_avk_prims;
		out->primaries_len = m;
		wlr_log(WLR_INFO, "color-management: advertising AVK's %zu transfer "
				"functions and %zu primaries (the wlroots renderer composites "
				"nothing and is not asked)", n, m);
		return;
	}
#endif
	out->tfs = wlr_color_manager_v1_transfer_function_list_from_renderer(
		drw, &out->tf_len);
	out->primaries = wlr_color_manager_v1_primaries_list_from_renderer(
		drw, &out->primaries_len);
}

#endif /* AZ_CM_CAPS_H */
