#ifndef AZ_WP_COLOR_MANAGEMENT_H
#define AZ_WP_COLOR_MANAGEMENT_H

/*
 * ── NATIVE wp-color-management-v1 — PHASE 1: THE MANAGER, AND NOTHING ELSE
 *
 * asteroidz speaks two colour-management protocols. frog is implemented here
 * and carries the full mastering metadata; wp-cm is mediated by wlroots 0.20.2,
 * which does not serialize the mastering-luminance or content-light-level
 * events -- so a wp-cm client learns THAT a display is HDR and not HOW BRIGHT
 * it goes. Empirically, against DP-1's rule of 400 / 0.4 / 250, a wp-cm client
 * reads maxlum=10000 minlum=50 maxcll=0 maxfall=0: wlroots' PQ defaults.
 *
 * Native ownership closes that. THE STANDING CONSTRAINT IS THAT IT CLOSES IT
 * WITHOUT PATCHING wlroots: no fork, no vendored copy, no local patch, no
 * dependency on an unmerged branch. wlroots stays a system package.
 *
 * ── THIS IS THE IMPLEMENTATION. THERE IS NO OTHER ONE. ───────────────────
 *
 * The wlroots path is gone rather than kept behind a switch. Two
 * implementations in one tree means two capability sets, two lifetimes and two
 * sets of bugs, and wlr_color_manager_v1 has no destroy function -- it lives
 * until display teardown -- so they could never have coexisted in a session
 * anyway. Keeping the loser as a fallback would have been a permanent
 * maintenance surface for a path nobody runs, which is the shape of the
 * back-compat this project declines on principle.
 *
 * What is NOT yet answered is answered LOUDLY: set_luminances and
 * set_mastering_display_primaries are refused with the protocol's own
 * unsupported_feature error rather than accepted and ignored. Native ownership
 * makes them reachable, which is the strongest argument for having done this at
 * all, but a request answered before the decode path uses the values would
 * promise what nothing keeps.
 *
 * ── VERSION 2, MATCHING WHAT wlroots ADVERTISES TODAY ────────────────────
 *
 * Not 3. The cutover must not change anything client-visible, and version 3
 * adds create_windows_bt2100, which nothing here implements. Note that v2 is
 * also load-bearing rather than cosmetic: COMPOUND_POWER_2_4 -- what this
 * compositor describes an SDR output with -- is `since="2"`, so a client bound
 * at 1 receives `failed: unhandled value for tf_named` instead of a
 * description. That is correct protocol behaviour and was observed live.
 */

#include "color-management-v1-protocol.h"

#define AZ_WPCM_VERSION 2

static struct wl_global *az_wpcm_global = NULL;
static struct az_cm_caps az_wpcm_caps;

/*
 * ── IDENTITIES ───────────────────────────────────────────────────────────
 *
 * The protocol requires an image description's identity to be unique among
 * LIVE descriptions, and equal for descriptions that describe the same image.
 * A monotonic counter satisfies both weakly (never claims two different
 * descriptions are the same) and is what wlroots does.
 *
 * NOT az_preferred's identity truncated to 32 bits: that hash answers "has the
 * answer changed", which is a different question with a different uniqueness
 * guarantee. Reusing it here would be borrowing a number because it was to
 * hand.
 */
static uint32_t az_wpcm_next_identity = 1;
static uint32_t az_wpcm_alloc_identity(void) {
	if (az_wpcm_next_identity == 0) {
		az_wpcm_next_identity = 1; /* 0 is reserved by the protocol */
	}
	return az_wpcm_next_identity++;
}

/* ── the image description object ─────────────────────────────────────── */

typedef struct {
	struct wl_resource *resource;
	struct wlr_image_description_v1_data data;
	uint32_t identity;
} AzWpcmDesc;

static void az_wpcm_desc_handle_destroy(struct wl_client *c,
		struct wl_resource *r) {
	(void)c;
	wl_resource_destroy(r);
}

/*
 * get_information. THE POINT OF THE WHOLE EXERCISE IS IN HERE.
 *
 * wlroots' implementation sends the transfer function, the primaries and the
 * transfer function's DEFAULT luminances, and carries two literal TODOs where
 * the mastering values belong. This sends the real ones: the display's own
 * max/min luminance, its max FALL, and its mastering primaries -- the numbers
 * the operator wrote in monitors.kdl and which frog has always carried.
 */
static void az_wpcm_desc_get_information(struct wl_client *client,
		struct wl_resource *resource, uint32_t id) {
	AzWpcmDesc *d = wl_resource_get_user_data(resource);
	struct wl_resource *info = wl_resource_create(client,
		&wp_image_description_info_v1_interface,
		wl_resource_get_version(resource), id);
	if (info == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(info, NULL, NULL, NULL);

	if (d->data.tf_named != 0) {
		wp_image_description_info_v1_send_tf_named(info, d->data.tf_named);
	}
	if (d->data.primaries_named != 0) {
		wp_image_description_info_v1_send_primaries_named(info,
			d->data.primaries_named);
	}
	if (d->data.has_mastering_display_primaries) {
		const struct wlr_color_primaries *p =
			&d->data.mastering_display_primaries;
		/* The protocol's units are 1/1000000 of the CIE 1931 xy value. */
		wp_image_description_info_v1_send_target_primaries(info,
			(int32_t)(p->red.x * 1000000.0f), (int32_t)(p->red.y * 1000000.0f),
			(int32_t)(p->green.x * 1000000.0f), (int32_t)(p->green.y * 1000000.0f),
			(int32_t)(p->blue.x * 1000000.0f), (int32_t)(p->blue.y * 1000000.0f),
			(int32_t)(p->white.x * 1000000.0f), (int32_t)(p->white.y * 1000000.0f));
	}
	if (d->data.has_mastering_luminance) {
		/* min in 1/10000 cd/m2, max in cd/m2 -- the protocol's own units, and
		 * the same asymmetry frog uses. Getting this wrong is invisible on a
		 * display whose floor is 0, which is most of them. */
		wp_image_description_info_v1_send_target_luminance(info,
			(uint32_t)(d->data.mastering_luminance.min * 10000.0f + 0.5f),
			(uint32_t)d->data.mastering_luminance.max);
	}
	if (d->data.max_cll != 0) {
		wp_image_description_info_v1_send_target_max_cll(info, d->data.max_cll);
	}
	if (d->data.max_fall != 0) {
		wp_image_description_info_v1_send_target_max_fall(info,
			d->data.max_fall);
	}
	wp_image_description_info_v1_send_done(info);
	wl_resource_destroy(info);
}

static const struct wp_image_description_v1_interface az_wpcm_desc_impl = {
	.destroy = az_wpcm_desc_handle_destroy,
	.get_information = az_wpcm_desc_get_information,
};

static void az_wpcm_desc_resource_destroy(struct wl_resource *r) {
	AzWpcmDesc *d = wl_resource_get_user_data(r);
	free(d);
}

/* Create a ready image description carrying `data`. Sends ready/ready2 by the
 * bound version -- v2 deprecated `ready` in favour of `ready2`, and a client
 * bound at 2 has no listener slot for the old one. */
static struct wl_resource *az_wpcm_desc_create(struct wl_client *client,
		uint32_t version, uint32_t id,
		const struct wlr_image_description_v1_data *data) {
	AzWpcmDesc *d = calloc(1, sizeof(*d));
	if (d == NULL) {
		wl_client_post_no_memory(client);
		return NULL;
	}
	d->data = *data;
	d->identity = az_wpcm_alloc_identity();
	d->resource = wl_resource_create(client,
		&wp_image_description_v1_interface, (int)version, id);
	if (d->resource == NULL) {
		free(d);
		wl_client_post_no_memory(client);
		return NULL;
	}
	wl_resource_set_implementation(d->resource, &az_wpcm_desc_impl, d,
		az_wpcm_desc_resource_destroy);
	if (version >= 2) {
		wp_image_description_v1_send_ready2(d->resource, d->identity, 0);
	} else {
		wp_image_description_v1_send_ready(d->resource, d->identity);
	}
	return d->resource;
}

/* ── the parametric creator ───────────────────────────────────────────── */

typedef struct {
	struct wl_resource *resource;
	struct wlr_image_description_v1_data data;
	bool set_tf, set_primaries, set_max_cll, set_max_fall;
} AzWpcmCreator;

/* Is this a value we advertised? THE ANSWER MUST BE YES BEFORE IT REACHES THE
 * SCENE, because scenefx calls wlr_color_manager_v1_transfer_function_to_wlr()
 * on whatever a surface carries, and that function ABORTS on a value with no
 * wlroots entry -- it has killed this compositor twice. The advertised set is
 * built from wlroots enums via _from_wlr(), so anything in it round-trips. */
static bool az_wpcm_tf_advertised(uint32_t tf) {
	for (size_t i = 0; i < az_wpcm_caps.tf_len; i++) {
		if ((uint32_t)az_wpcm_caps.tfs[i] == tf) {
			return true;
		}
	}
	return false;
}
static bool az_wpcm_primaries_advertised(uint32_t p) {
	for (size_t i = 0; i < az_wpcm_caps.primaries_len; i++) {
		if ((uint32_t)az_wpcm_caps.primaries[i] == p) {
			return true;
		}
	}
	return false;
}

static void az_wpcm_creator_set_tf_named(struct wl_client *c,
		struct wl_resource *r, uint32_t tf) {
	(void)c;
	AzWpcmCreator *cr = wl_resource_get_user_data(r);
	if (cr->set_tf) {
		wl_resource_post_error(r,
			WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_ALREADY_SET,
			"transfer function already set");
		return;
	}
	static int break_novalidate = -1;
	if (break_novalidate < 0) {
		break_novalidate = getenv("AZ_BREAK_WPCM_NO_VALIDATE") != NULL;
	}
	if (!break_novalidate && !az_wpcm_tf_advertised(tf)) {
		/* BREAK: AZ_BREAK_WPCM_NO_VALIDATE accepts it, and the compositor then
		 * dies inside scenefx's _to_wlr() the moment the surface is
		 * reconfigured. The break exists so that "validation is what stands
		 * between us and an abort" is demonstrated rather than asserted. */
		wl_resource_post_error(r,
			WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INVALID_TF,
			"transfer function %u was not advertised", tf);
		return;
	}
	cr->data.tf_named = tf;
	cr->set_tf = true;
}

static void az_wpcm_creator_set_tf_power(struct wl_client *c,
		struct wl_resource *r, uint32_t eexp) {
	(void)c; (void)eexp;
	wl_resource_post_error(r,
		WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INVALID_TF,
		"a power transfer function is not among the advertised features");
}

static void az_wpcm_creator_set_primaries_named(struct wl_client *c,
		struct wl_resource *r, uint32_t p) {
	(void)c;
	AzWpcmCreator *cr = wl_resource_get_user_data(r);
	if (cr->set_primaries) {
		wl_resource_post_error(r,
			WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_ALREADY_SET,
			"primaries already set");
		return;
	}
	static int break_novalidate = -1;
	if (break_novalidate < 0) {
		break_novalidate = getenv("AZ_BREAK_WPCM_NO_VALIDATE") != NULL;
	}
	if (!break_novalidate && !az_wpcm_primaries_advertised(p)) {
		wl_resource_post_error(r,
			WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INVALID_PRIMARIES_NAMED,
			"primaries %u were not advertised", p);
		return;
	}
	cr->data.primaries_named = p;
	cr->set_primaries = true;
}

static void az_wpcm_creator_set_primaries(struct wl_client *c,
		struct wl_resource *r, int32_t rx, int32_t ry, int32_t gx, int32_t gy,
		int32_t bx, int32_t by, int32_t wx, int32_t wy) {
	(void)c; (void)rx; (void)ry; (void)gx; (void)gy;
	(void)bx; (void)by; (void)wx; (void)wy;
	wl_resource_post_error(r,
		WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INVALID_PRIMARIES_NAMED,
		"custom primaries are not among the advertised features");
}

static void az_wpcm_creator_set_luminances(struct wl_client *c,
		struct wl_resource *r, uint32_t min, uint32_t max, uint32_t ref) {
	(void)c; (void)min; (void)max; (void)ref;
	/* Not advertised yet. Native ownership makes this REACHABLE -- it is one of
	 * the two features wlroots asserts on -- but a request answered before the
	 * decode path uses the values would be a promise nothing keeps. */
	wl_resource_post_error(r,
		WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_UNSUPPORTED_FEATURE,
		"set_luminances is not among the advertised features");
}

static void az_wpcm_creator_set_mastering_display_primaries(
		struct wl_client *c, struct wl_resource *r, int32_t rx, int32_t ry,
		int32_t gx, int32_t gy, int32_t bx, int32_t by, int32_t wx,
		int32_t wy) {
	(void)c; (void)rx; (void)ry; (void)gx; (void)gy;
	(void)bx; (void)by; (void)wx; (void)wy;
	wl_resource_post_error(r,
		WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_UNSUPPORTED_FEATURE,
		"set_mastering_display_primaries is not among the advertised features");
}

static void az_wpcm_creator_set_mastering_luminance(struct wl_client *c,
		struct wl_resource *r, uint32_t min, uint32_t max) {
	(void)c; (void)min; (void)max;
	wl_resource_post_error(r,
		WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_UNSUPPORTED_FEATURE,
		"set_mastering_luminance is not among the advertised features");
}

static void az_wpcm_creator_set_max_cll(struct wl_client *c,
		struct wl_resource *r, uint32_t v) {
	(void)c;
	AzWpcmCreator *cr = wl_resource_get_user_data(r);
	if (cr->set_max_cll) {
		wl_resource_post_error(r,
			WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_ALREADY_SET,
			"max_cll already set");
		return;
	}
	cr->data.max_cll = v;
	cr->set_max_cll = true;
}

static void az_wpcm_creator_set_max_fall(struct wl_client *c,
		struct wl_resource *r, uint32_t v) {
	(void)c;
	AzWpcmCreator *cr = wl_resource_get_user_data(r);
	if (cr->set_max_fall) {
		wl_resource_post_error(r,
			WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_ALREADY_SET,
			"max_fall already set");
		return;
	}
	cr->data.max_fall = v;
	cr->set_max_fall = true;
}

static void az_wpcm_creator_create(struct wl_client *client,
		struct wl_resource *r, uint32_t id) {
	AzWpcmCreator *cr = wl_resource_get_user_data(r);
	/* The protocol requires BOTH before create; an image description missing
	 * either is not describable. */
	if (!cr->set_tf || !cr->set_primaries) {
		wl_resource_post_error(r,
			WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INCOMPLETE_SET,
			"a parametric description needs both a transfer function and "
			"primaries");
		return;
	}
	az_wpcm_desc_create(client, wl_resource_get_version(r), id, &cr->data);
	/* The creator is single-use and is destroyed by `create` per the protocol
	 * -- the client must not reuse it, and the resource going away is what
	 * enforces that rather than a flag we would have to check everywhere. */
	wl_resource_destroy(r);
}

static const struct wp_image_description_creator_params_v1_interface
		az_wpcm_creator_impl = {
	.create = az_wpcm_creator_create,
	.set_tf_named = az_wpcm_creator_set_tf_named,
	.set_tf_power = az_wpcm_creator_set_tf_power,
	.set_primaries_named = az_wpcm_creator_set_primaries_named,
	.set_primaries = az_wpcm_creator_set_primaries,
	.set_luminances = az_wpcm_creator_set_luminances,
	.set_mastering_display_primaries =
		az_wpcm_creator_set_mastering_display_primaries,
	.set_mastering_luminance = az_wpcm_creator_set_mastering_luminance,
	.set_max_cll = az_wpcm_creator_set_max_cll,
	.set_max_fall = az_wpcm_creator_set_max_fall,
};

static void az_wpcm_creator_resource_destroy(struct wl_resource *r) {
	free(wl_resource_get_user_data(r));
}

/* ── the surface object, with COMMIT-LATCHED state ────────────────────── */

typedef struct {
	bool has;
	struct wlr_image_description_v1_data data;
} AzWpcmSurfaceState;

typedef struct {
	struct wl_resource *resource;
	struct wlr_surface *surface;
	struct wlr_addon addon;
	struct wlr_surface_synced synced;
	AzWpcmSurfaceState pending, current;
} AzWpcmSurface;

/*
 * WHY wlr_surface_synced AND NOT A COMMIT LISTENER.
 *
 * set_image_description and unset_image_description are DOUBLE-BUFFERED
 * surface state: they take effect at the next wl_surface.commit, not when the
 * request arrives. A commit listener would get the ordering right for a simple
 * surface and wrong for a synchronized subsurface, whose commits are cached
 * and applied later with its parent. wlr_surface_synced handles that; frog has
 * no equivalent to copy because frog's state is not double-buffered at all.
 */
static const struct wlr_surface_synced_impl az_wpcm_synced_impl = {
	.state_size = sizeof(AzWpcmSurfaceState),
};

static const struct wlr_addon_interface az_wpcm_surface_addon_impl;

static void az_wpcm_surface_addon_destroy(struct wlr_addon *addon) {
	AzWpcmSurface *s = wl_container_of(addon, s, addon);
	wlr_addon_finish(&s->addon);
	wlr_surface_synced_finish(&s->synced);
	s->surface = NULL;
	if (s->resource != NULL) {
		wl_resource_set_user_data(s->resource, NULL);
	}
	free(s);
}

static const struct wlr_addon_interface az_wpcm_surface_addon_impl = {
	.name = "az_wpcm_surface",
	.destroy = az_wpcm_surface_addon_destroy,
};

static void az_wpcm_surface_set(struct wl_client *c, struct wl_resource *r,
		struct wl_resource *desc_resource, uint32_t render_intent) {
	(void)c;
	AzWpcmSurface *s = wl_resource_get_user_data(r);
	if (s == NULL) {
		return; /* the surface went away; the resource is inert */
	}
	bool ok = false;
	for (size_t i = 0; i < LENGTH(az_cm_render_intents); i++) {
		if ((uint32_t)az_cm_render_intents[i] == render_intent) {
			ok = true;
		}
	}
	if (!ok) {
		wl_resource_post_error(r,
			WP_COLOR_MANAGEMENT_SURFACE_V1_ERROR_RENDER_INTENT,
			"render intent %u was not advertised", render_intent);
		return;
	}
	AzWpcmDesc *d = wl_resource_get_user_data(desc_resource);
	s->pending.has = true;
	s->pending.data = d->data;
}

static void az_wpcm_surface_unset(struct wl_client *c, struct wl_resource *r) {
	(void)c;
	AzWpcmSurface *s = wl_resource_get_user_data(r);
	if (s == NULL) {
		return;
	}
	s->pending.has = false;
}

static void az_wpcm_surface_handle_destroy(struct wl_client *c,
		struct wl_resource *r) {
	(void)c;
	wl_resource_destroy(r);
}

static const struct wp_color_management_surface_v1_interface
		az_wpcm_surface_impl = {
	.destroy = az_wpcm_surface_handle_destroy,
	.set_image_description = az_wpcm_surface_set,
	.unset_image_description = az_wpcm_surface_unset,
};

static void az_wpcm_surface_resource_destroy(struct wl_resource *r) {
	AzWpcmSurface *s = wl_resource_get_user_data(r);
	if (s != NULL) {
		s->resource = NULL;
	}
}

/* The scene's fallback asks this for a surface's colour description. Returns
 * the CURRENT (committed) state, never the pending one. */
static const struct wlr_image_description_v1_data *
az_wpcm_surface_image_description(struct wlr_surface *surface) {
	struct wlr_addon *addon = wlr_addon_find(&surface->addons, NULL,
		&az_wpcm_surface_addon_impl);
	if (addon == NULL) {
		return NULL;
	}
	AzWpcmSurface *s = wl_container_of(addon, s, addon);
	return s->current.has ? &s->current.data : NULL;
}

/*
 * ONE SLOT, TWO PROTOCOLS. wlr_scene_set_surface_color_description_fallback
 * takes a single callback, and both native wp-cm and frog need to answer
 * through it. Native first: a client that speaks both has told wp-cm something
 * more specific than frog's per-display metadata.
 *
 * Registered unconditionally from setup, after both inits, so precedence does
 * not depend on which one registered last -- the ordering hazard that a
 * "whoever calls it later wins" design would carry.
 */
static const struct wlr_image_description_v1_data *
az_cm_surface_description(struct wlr_surface *surface) {
	const struct wlr_image_description_v1_data *d =
		az_wpcm_surface_image_description(surface);
	if (d != NULL) {
		return d;
	}
	return frog_surface_image_description(surface);
}

/* ── the feedback object (compositor -> client) ───────────────────────── */

typedef struct {
	struct wl_resource *resource;
	struct wlr_surface *surface;
	struct wl_list link;      /* az_wpcm_feedbacks */
	uint64_t sent_identity;   /* az_preferred's, 0 = never sent */
} AzWpcmFeedback;

static struct wl_list az_wpcm_feedbacks;
static bool az_wpcm_feedbacks_init = false;
static uint64_t az_wpcm_preferred_sends = 0;

/* Build a wp-cm image description from the shared policy. THIS is where the
 * mastering values wlroots drops actually reach the wire. */
static void az_wpcm_data_from_preferred(const struct az_preferred *pref,
		struct wlr_image_description_v1_data *out) {
	*out = (struct wlr_image_description_v1_data){
		.tf_named = wlr_color_manager_v1_transfer_function_from_wlr(
			pref->hdr ? WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ
					  : WLR_COLOR_TRANSFER_FUNCTION_SRGB),
		.primaries_named = wlr_color_manager_v1_primaries_from_wlr(
			pref->bt2020 ? WLR_COLOR_NAMED_PRIMARIES_BT2020
						 : WLR_COLOR_NAMED_PRIMARIES_SRGB),
		.has_mastering_luminance = true,
		.mastering_luminance = {
			.min = pref->min_luminance,
			.max = pref->max_luminance,
		},
		.max_cll = (uint32_t)pref->max_luminance,
		.max_fall = (uint32_t)pref->max_fall,
	};
	/* Through _from_wlr(), never a protocol constant: see az_cm_caps.h. The
	 * obvious constant here is WP_..._TRANSFER_FUNCTION_SRGB, and reaching for
	 * it killed the compositor with a SIGABRT the first time a client asked a
	 * description for its information. */
}

static void az_wpcm_feedback_get_preferred(struct wl_client *client,
		struct wl_resource *r, uint32_t id) {
	AzWpcmFeedback *fb = wl_resource_get_user_data(r);
	struct az_preferred pref;
	az_preferred_resolve(fb != NULL ? fb->surface : NULL, &pref);
	struct wlr_image_description_v1_data data;
	if (pref.identity == 0) {
		/* Not on an output. Answer the SDR default rather than failing: a
		 * surface with no display still needs something to render against,
		 * and "no answer" is not one of the protocol's options here. */
		struct az_preferred sdr = {
			.max_luminance = 203.0f, .min_luminance = 0.2f, .max_fall = 203.0f,
		};
		az_wpcm_data_from_preferred(&sdr, &data);
	} else {
		az_wpcm_data_from_preferred(&pref, &data);
	}
	az_wpcm_desc_create(client, wl_resource_get_version(r), id, &data);
}

static void az_wpcm_feedback_handle_destroy(struct wl_client *c,
		struct wl_resource *r) {
	(void)c;
	wl_resource_destroy(r);
}

static const struct wp_color_management_surface_feedback_v1_interface
		az_wpcm_feedback_impl = {
	.destroy = az_wpcm_feedback_handle_destroy,
	.get_preferred = az_wpcm_feedback_get_preferred,
	.get_preferred_parametric = az_wpcm_feedback_get_preferred,
};

static void az_wpcm_feedback_resource_destroy(struct wl_resource *r) {
	AzWpcmFeedback *fb = wl_resource_get_user_data(r);
	if (fb == NULL) {
		return;
	}
	/* OFF THE REGISTRY BEFORE THE FREE. The reverse order leaves a freed node
	 * linked, and the next walk of the list reads it -- the exact ordering
	 * frog's own comment calls out. */
	wl_list_remove(&fb->link);
	free(fb);
}

/*
 * Tell every feedback object for this surface that its preferred description
 * changed -- and ONLY when it actually changed. az_preferred's identity is the
 * gate: an HDR toggle on another monitor, a hotplug elsewhere, or a layout
 * change that did not move the surface all cost one comparison and no traffic.
 */
static bool az_wpcm_send_preferred(struct wlr_surface *surface) {
	if (!az_wpcm_feedbacks_init) {
		return false;
	}
	struct az_preferred pref;
	az_preferred_resolve(surface, &pref);
	if (pref.identity == 0) {
		return false;
	}
	static int break_send_once = -1;
	if (break_send_once < 0) {
		break_send_once = getenv("AZ_BREAK_WPCM_SEND_ONCE") != NULL;
	}
	static int break_no_gate = -1;
	if (break_no_gate < 0) {
		break_no_gate = getenv("AZ_BREAK_WPCM_NO_IDENTITY_GATE") != NULL;
	}
	bool sent = false;
	AzWpcmFeedback *fb;
	wl_list_for_each(fb, &az_wpcm_feedbacks, link) {
		if (fb->surface != surface || fb->resource == NULL) {
			continue;
		}
		if (break_send_once && fb->sent_identity != 0) {
			continue;
		}
		if (!break_no_gate && pref.identity == fb->sent_identity) {
			continue;
		}
		uint32_t version = wl_resource_get_version(fb->resource);
		if (version >= 2) {
			wp_color_management_surface_feedback_v1_send_preferred_changed2(
				fb->resource, az_wpcm_alloc_identity(), 0);
		} else {
			wp_color_management_surface_feedback_v1_send_preferred_changed(
				fb->resource, az_wpcm_alloc_identity());
		}
		fb->sent_identity = pref.identity;
		az_wpcm_preferred_sends++;
		sent = true;
	}
	return sent;
}

/* ── the output object ────────────────────────────────────────────────── */

typedef struct {
	struct wl_resource *resource;
	struct wlr_output *output;
	struct wl_list link;   /* az_wpcm_outputs */
} AzWpcmOutput;

static struct wl_list az_wpcm_outputs;
static bool az_wpcm_outputs_init = false;

static void az_wpcm_output_get_description(struct wl_client *client,
		struct wl_resource *r, uint32_t id) {
	AzWpcmOutput *o = wl_resource_get_user_data(r);
	struct wlr_image_description_v1_data data = {
		.tf_named = wlr_color_manager_v1_transfer_function_from_wlr(
			WLR_COLOR_TRANSFER_FUNCTION_SRGB),
		.primaries_named = wlr_color_manager_v1_primaries_from_wlr(
			WLR_COLOR_NAMED_PRIMARIES_SRGB),
	};
	if (o != NULL && o->output != NULL
			&& o->output->image_description != NULL) {
		const struct wlr_output_image_description *od =
			o->output->image_description;
		data.tf_named = wlr_color_manager_v1_transfer_function_from_wlr(
			od->transfer_function);
		data.primaries_named = wlr_color_manager_v1_primaries_from_wlr(
			od->primaries);
		/* wlr_output_image_description carries no has_* flags -- a zero max
		 * IS the "unset" signal, which is why this tests the value rather than
		 * a companion boolean that does not exist. */
		data.has_mastering_display_primaries = true;
		data.mastering_display_primaries = od->mastering_display_primaries;
		if (od->mastering_luminance.max > 0.0) {
			data.has_mastering_luminance = true;
			data.mastering_luminance.min =
				(float)od->mastering_luminance.min;
			data.mastering_luminance.max =
				(float)od->mastering_luminance.max;
		}
		data.max_cll = (uint32_t)od->max_cll;
		data.max_fall = (uint32_t)od->max_fall;
	}
	az_wpcm_desc_create(client, wl_resource_get_version(r), id, &data);
}

static void az_wpcm_output_handle_destroy(struct wl_client *c,
		struct wl_resource *r) {
	(void)c;
	wl_resource_destroy(r);
}

static const struct wp_color_management_output_v1_interface
		az_wpcm_output_impl = {
	.destroy = az_wpcm_output_handle_destroy,
	.get_image_description = az_wpcm_output_get_description,
};

static void az_wpcm_output_resource_destroy(struct wl_resource *r) {
	AzWpcmOutput *o = wl_resource_get_user_data(r);
	if (o == NULL) {
		return;
	}
	wl_list_remove(&o->link);
	free(o);
}

/* An output changed colour state: every object describing it must be told. */
static void az_wpcm_output_changed(struct wlr_output *output) {
	if (!az_wpcm_outputs_init) {
		return;
	}
	AzWpcmOutput *o;
	wl_list_for_each(o, &az_wpcm_outputs, link) {
		if (o->output == output && o->resource != NULL) {
			wp_color_management_output_v1_send_image_description_changed(
				o->resource);
		}
	}
}

/* An output is going away. The objects describing it stay alive -- a client may
 * still hold them -- but must stop pointing at freed memory. */
static void az_wpcm_output_gone(struct wlr_output *output) {
	if (!az_wpcm_outputs_init) {
		return;
	}
	AzWpcmOutput *o;
	wl_list_for_each(o, &az_wpcm_outputs, link) {
		if (o->output == output) {
			o->output = NULL;
		}
	}
}

static void az_wpcm_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	(void)client;
	wl_resource_destroy(resource);
}

static void az_wpcm_get_output(struct wl_client *client,
		struct wl_resource *resource, uint32_t id,
		struct wl_resource *output_resource) {
	AzWpcmOutput *o = calloc(1, sizeof(*o));
	if (o == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	o->output = wlr_output_from_resource(output_resource);
	o->resource = wl_resource_create(client,
		&wp_color_management_output_v1_interface,
		wl_resource_get_version(resource), id);
	if (o->resource == NULL) {
		free(o);
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(o->resource, &az_wpcm_output_impl, o,
		az_wpcm_output_resource_destroy);
	wl_list_insert(&az_wpcm_outputs, &o->link);
}

static void az_wpcm_get_surface(struct wl_client *client,
		struct wl_resource *resource, uint32_t id,
		struct wl_resource *surface_resource) {
	struct wlr_surface *surface = wlr_surface_from_resource(surface_resource);
	/* THE PROTOCOL REQUIRES THIS ERROR, and it is not defensive noise: two
	 * colour-management surfaces for one wl_surface would give a client two
	 * independent double-buffered states racing into one addon. */
	if (wlr_addon_find(&surface->addons, NULL,
			&az_wpcm_surface_addon_impl) != NULL) {
		wl_resource_post_error(resource,
			WP_COLOR_MANAGER_V1_ERROR_SURFACE_EXISTS,
			"a colour-management surface already exists for this surface");
		return;
	}
	AzWpcmSurface *s = calloc(1, sizeof(*s));
	if (s == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	s->surface = surface;
	if (!wlr_surface_synced_init(&s->synced, surface, &az_wpcm_synced_impl,
			&s->pending, &s->current)) {
		free(s);
		wl_client_post_no_memory(client);
		return;
	}
	s->resource = wl_resource_create(client,
		&wp_color_management_surface_v1_interface,
		wl_resource_get_version(resource), id);
	if (s->resource == NULL) {
		wlr_surface_synced_finish(&s->synced);
		free(s);
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(s->resource, &az_wpcm_surface_impl, s,
		az_wpcm_surface_resource_destroy);
	wlr_addon_init(&s->addon, &surface->addons, NULL,
		&az_wpcm_surface_addon_impl);
}

static void az_wpcm_get_surface_feedback(struct wl_client *client,
		struct wl_resource *resource, uint32_t id,
		struct wl_resource *surface_resource) {
	AzWpcmFeedback *fb = calloc(1, sizeof(*fb));
	if (fb == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	fb->surface = wlr_surface_from_resource(surface_resource);
	fb->resource = wl_resource_create(client,
		&wp_color_management_surface_feedback_v1_interface,
		wl_resource_get_version(resource), id);
	if (fb->resource == NULL) {
		free(fb);
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(fb->resource, &az_wpcm_feedback_impl, fb,
		az_wpcm_feedback_resource_destroy);
	wl_list_insert(&az_wpcm_feedbacks, &fb->link);
}

static void az_wpcm_create_icc_creator(struct wl_client *client,
		struct wl_resource *resource, uint32_t id) {
	(void)client; (void)id;
	/* REFUSED, permanently. The ICC feature is not advertised, and the protocol
	 * requires this exact error for a creator the compositor did not offer. */
	wl_resource_post_error(resource,
		WP_COLOR_MANAGER_V1_ERROR_UNSUPPORTED_FEATURE,
		"the ICC creator is not among the advertised features");
}

static void az_wpcm_create_parametric_creator(struct wl_client *client,
		struct wl_resource *resource, uint32_t id) {
	AzWpcmCreator *cr = calloc(1, sizeof(*cr));
	if (cr == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	cr->resource = wl_resource_create(client,
		&wp_image_description_creator_params_v1_interface,
		wl_resource_get_version(resource), id);
	if (cr->resource == NULL) {
		free(cr);
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(cr->resource, &az_wpcm_creator_impl, cr,
		az_wpcm_creator_resource_destroy);
}

static void az_wpcm_create_windows_scrgb(struct wl_client *client,
		struct wl_resource *resource, uint32_t id) {
	(void)client; (void)id;
	wl_resource_post_error(resource,
		WP_COLOR_MANAGER_V1_ERROR_UNSUPPORTED_FEATURE,
		"the Windows scRGB image description is not among the advertised "
		"features");
}

static const struct wp_color_manager_v1_interface az_wpcm_impl = {
	.destroy = az_wpcm_destroy,
	.get_output = az_wpcm_get_output,
	.get_surface = az_wpcm_get_surface,
	.get_surface_feedback = az_wpcm_get_surface_feedback,
	.create_icc_creator = az_wpcm_create_icc_creator,
	.create_parametric_creator = az_wpcm_create_parametric_creator,
	.create_windows_scrgb = az_wpcm_create_windows_scrgb,
};

static void az_wpcm_bind(struct wl_client *client, void *data,
		uint32_t version, uint32_t id) {
	(void)data;
	struct wl_resource *resource = wl_resource_create(client,
		&wp_color_manager_v1_interface, (int)version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &az_wpcm_impl, NULL, NULL);

	/*
	 * THE ADVERTISEMENT, FROM THE SHARED CAPABILITY SET.
	 *
	 * az_cm_caps.h builds it, and the wlroots path is handed the same struct
	 * -- so "the native manager advertises what wlroots advertised" is true by
	 * construction rather than by a second list that has to be kept in step.
	 *
	 * BREAK: AZ_BREAK_WPCM_EMPTY_CAPS suppresses the transfer-function list,
	 * reproducing the shape of the defect that made the wlroots-renderer list
	 * come back empty -- no named transfer functions, no client able to attach
	 * any image description at all. It exists because that failure was silent:
	 * every call succeeded and every client quietly gave up.
	 */
	static int break_empty = -1;
	if (break_empty < 0) {
		break_empty = getenv("AZ_BREAK_WPCM_EMPTY_CAPS") != NULL;
	}

	/*
	 * FEATURES BEFORE INTENTS, because that is the order wlroots emits them
	 * and the cutover must not be client-visible. A client that takes the
	 * first value it recognises sees a different advertisement from the same
	 * set in a different order -- which is why the Phase 1 gate diffs these
	 * streams verbatim instead of comparing them as sets. It caught this.
	 *
	 * Parametric only, matching what the wlroots path enables. The features
	 * wlroots asserts on -- set_luminances, mastering primaries -- are what
	 * native ownership eventually unlocks, but advertising them before the
	 * code answers them would promise what nothing delivers.
	 */
	wp_color_manager_v1_send_supported_feature(resource,
		WP_COLOR_MANAGER_V1_FEATURE_PARAMETRIC);
	for (size_t i = 0; i < LENGTH(az_cm_render_intents); i++) {
		wp_color_manager_v1_send_supported_intent(resource,
			az_cm_render_intents[i]);
	}
	if (!break_empty) {
		for (size_t i = 0; i < az_wpcm_caps.tf_len; i++) {
			wp_color_manager_v1_send_supported_tf_named(resource,
				az_wpcm_caps.tfs[i]);
		}
	}
	for (size_t i = 0; i < az_wpcm_caps.primaries_len; i++) {
		wp_color_manager_v1_send_supported_primaries_named(resource,
			az_wpcm_caps.primaries[i]);
	}
	wp_color_manager_v1_send_done(resource);
}

/* Create the native manager global. Returns false when the native path is not
 * selected, so the caller creates wlroots' instead -- exactly one of the two
 * exists in any session. */
static bool az_wpcm_create(struct wl_display *dpy,
		const struct az_cm_caps *caps) {
	az_wpcm_caps = *caps;
	if (!az_wpcm_feedbacks_init) {
		wl_list_init(&az_wpcm_feedbacks);
		az_wpcm_feedbacks_init = true;
	}
	if (!az_wpcm_outputs_init) {
		wl_list_init(&az_wpcm_outputs);
		az_wpcm_outputs_init = true;
	}
	az_wpcm_global = wl_global_create(dpy, &wp_color_manager_v1_interface,
		AZ_WPCM_VERSION, NULL, az_wpcm_bind);
	if (az_wpcm_global == NULL) {
		wlr_log(WLR_ERROR, "native colour manager: wl_global_create failed");
		return false;
	}
	wlr_log(WLR_INFO, "colour management: wp_color_manager_v1 version %d, "
			"%zu transfer functions, %zu primaries",
			AZ_WPCM_VERSION, az_wpcm_caps.tf_len, az_wpcm_caps.primaries_len);
	return true;
}

#endif /* AZ_WP_COLOR_MANAGEMENT_H */
