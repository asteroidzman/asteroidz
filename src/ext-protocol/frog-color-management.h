/* frog-color-management-v1: gamescope's color-management protocol, and the
 * only realistic route to HDR passthrough from it -- gamescope's
 * wp-color-management path demands six manager features of which wlroots
 * 0.20 implements two (the rest assert), while its frog path enables HDR as
 * soon as preferred_metadata says the output is PQ (see gamescope
 * WaylandBackend.cpp Wayland_FrogColorManagedSurface_PreferredMetadata).
 *
 * Clients declare per-surface colorimetry (known transfer function,
 * container color volume, HDR10 metadata); we synthesize a
 * wlr_image_description_v1_data from it and hand it to the scene through
 * scenefx's surface-color fallback (see wlr_scene_set_surface_color_
 * description_fallback), so frog surfaces flow through the exact same
 * render (PQ sampling) and direct-scanout (colorimetry match) paths as
 * wp-color-management surfaces. */

#include "frog-color-management-v1-protocol.h"

typedef struct {
	struct wl_resource *resource;
	struct wlr_surface *surface; /* NULL once the surface is destroyed */
	struct wlr_addon addon;		 /* on surface->addons, keyed by frog impl */
	struct wlr_image_description_v1_data data;
	bool active; /* client declared some colorimetry */
	struct wl_list link;   /* frog_surfaces */
	uint64_t sent_identity; /* az_preferred.identity last SENT; 0 = never */
} FrogColorSurface;

/*
 * M6B/D6. EVERY LIVE frog SURFACE, so the preferred metadata can be re-sent.
 *
 * It used to be sent exactly once, when the client created the object, and
 * never again -- so an HDR toggle after a client connected never reached it and
 * the client kept tone-mapping for the display state it was told about at
 * startup. That is the same defect wp-cm had one protocol object over, and it
 * needs the same fix: a list to walk when the answer changes.
 */
static struct wl_list frog_surfaces;
static bool frog_surfaces_init;
/* How many preferred_metadata events have actually gone out. A transition
 * oracle has to be able to tell "the metadata is correct" from "nothing was
 * ever sent and the client is reading its own initial state". */
static uint64_t frog_metadata_sends;

static void frog_surface_addon_destroy(struct wlr_addon *addon) {
	FrogColorSurface *fs = wl_container_of(addon, fs, addon);
	wlr_addon_finish(&fs->addon);
	fs->surface = NULL;
}

/* The monitor a frog surface is actually ON -- through the SHARED policy, so
 * frog and wp-cm cannot answer this differently for the same surface. See
 * az_preferred.h for why that is not a hypothetical. */
static Monitor *frog_surface_monitor(FrogColorSurface *fs) {
	return az_surface_effective_output(fs->surface);
}

static const struct wlr_addon_interface frog_surface_addon_impl = {
	.name = "asteroidz_frog_color_surface",
	.destroy = frog_surface_addon_destroy,
};

/* scenefx surface-color fallback: runs on every scene-surface reconfigure
 * for surfaces without a wp-color-management image description */
static const struct wlr_image_description_v1_data *
frog_surface_image_description(struct wlr_surface *surface) {
	struct wlr_addon *addon = wlr_addon_find(&surface->addons, NULL,
											 &frog_surface_addon_impl);
	if (!addon)
		return NULL;
	FrogColorSurface *fs = wl_container_of(addon, fs, addon);
	return fs->active ? &fs->data : NULL;
}

static void frog_surface_handle_destroy(struct wl_client *client,
										struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void frog_surface_resource_destroy(struct wl_resource *resource) {
	FrogColorSurface *fs = wl_resource_get_user_data(resource);
	if (!fs)
		return;
	if (fs->surface)
		wlr_addon_finish(&fs->addon);
	/* Off the registry BEFORE the free, or the next re-send walks a freed
	 * entry -- the list is walked from an output-state change, which can
	 * happen at any point relative to a client disconnecting. */
	wl_list_remove(&fs->link);
	free(fs);
}

/* colorimetry changed: nudge the scene to re-run surface_reconfigure (it
 * derives colorimetry on commit; a damage-less state poke is enough since
 * clients set colorimetry around commits anyway) */
static void frog_surface_apply(FrogColorSurface *fs) {
	fs->active = fs->data.tf_named != 0 || fs->data.primaries_named != 0;
}

static void frog_surface_set_known_transfer_function(
	struct wl_client *client, struct wl_resource *resource,
	uint32_t transfer_function) {
	FrogColorSurface *fs = wl_resource_get_user_data(resource);
	if (!fs)
		return;

	switch (transfer_function) {
	case FROG_COLOR_MANAGED_SURFACE_TRANSFER_FUNCTION_ST2084_PQ:
		fs->data.tf_named = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ;
		break;
	case FROG_COLOR_MANAGED_SURFACE_TRANSFER_FUNCTION_SCRGB_LINEAR:
		fs->data.tf_named = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_LINEAR;
		break;
	case FROG_COLOR_MANAGED_SURFACE_TRANSFER_FUNCTION_GAMMA_22:
	case FROG_COLOR_MANAGED_SURFACE_TRANSFER_FUNCTION_SRGB:
		/* the frog spec explicitly allows displaying sRGB as gamma 2.2
		 * "for consistency with content rendering across displays" */
		fs->data.tf_named = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22;
		break;
	case FROG_COLOR_MANAGED_SURFACE_TRANSFER_FUNCTION_UNDEFINED:
	default:
		fs->data.tf_named = 0;
		break;
	}
	frog_surface_apply(fs);
}

static void frog_surface_set_known_container_color_volume(
	struct wl_client *client, struct wl_resource *resource,
	uint32_t primaries) {
	FrogColorSurface *fs = wl_resource_get_user_data(resource);
	if (!fs)
		return;

	switch (primaries) {
	case FROG_COLOR_MANAGED_SURFACE_PRIMARIES_REC709:
		fs->data.primaries_named = WP_COLOR_MANAGER_V1_PRIMARIES_SRGB;
		break;
	case FROG_COLOR_MANAGED_SURFACE_PRIMARIES_REC2020:
		fs->data.primaries_named = WP_COLOR_MANAGER_V1_PRIMARIES_BT2020;
		break;
	case FROG_COLOR_MANAGED_SURFACE_PRIMARIES_UNDEFINED:
	default:
		fs->data.primaries_named = 0;
		/* gamescope's own PASSTHRU case (WaylandBackend.cpp) means to
		 * reset the whole image description to "no known colorimetry" by
		 * calling set_known_container_color_volume(PRIMARIES_UNDEFINED)
		 * followed by what should be set_known_transfer_function(
		 * TRANSFER_FUNCTION_UNDEFINED) -- but that second call is a
		 * copy-paste bug: it invokes this same setter again, so the
		 * transfer function is never actually cleared. A raw/passthrough
		 * buffer then keeps whatever non-zero tf_named a previous PQ/HDR
		 * frame left behind, and we'd decode it through the PQ inverse
		 * EOTF -- an aggressive nonlinear transform that turns arbitrary
		 * (non-PQ-range) pixel data into wild, saturated RGB noise.
		 * Primaries reset to undefined is never legitimately paired with
		 * a still-known transfer function, so clear both together. */
		fs->data.tf_named = 0;
		break;
	}
	frog_surface_apply(fs);
}

static void frog_surface_set_render_intent(struct wl_client *client,
										   struct wl_resource *resource,
										   uint32_t render_intent) {
	/* perceptual is the only intent we render with; accept and ignore */
}

static void frog_surface_set_hdr_metadata(
	struct wl_client *client, struct wl_resource *resource,
	uint32_t mastering_display_primary_red_x,
	uint32_t mastering_display_primary_red_y,
	uint32_t mastering_display_primary_green_x,
	uint32_t mastering_display_primary_green_y,
	uint32_t mastering_display_primary_blue_x,
	uint32_t mastering_display_primary_blue_y, uint32_t mastering_white_point_x,
	uint32_t mastering_white_point_y, uint32_t max_display_mastering_luminance,
	uint32_t min_display_mastering_luminance, uint32_t max_cll,
	uint32_t max_fall) {
	FrogColorSurface *fs = wl_resource_get_user_data(resource);
	if (!fs)
		return;

	/* protocol encodes CIE xy in 1/50000 units, min luminance in
	 * 1/10000 cd/m2, everything else in cd/m2 (HDR10 SMPTE 2086 style) */
	fs->data.has_mastering_display_primaries =
		mastering_display_primary_red_x || mastering_display_primary_red_y ||
		mastering_display_primary_green_x || mastering_display_primary_green_y;
	fs->data.mastering_display_primaries = (struct wlr_color_primaries){
		.red = {mastering_display_primary_red_x / 50000.0f,
				mastering_display_primary_red_y / 50000.0f},
		.green = {mastering_display_primary_green_x / 50000.0f,
				  mastering_display_primary_green_y / 50000.0f},
		.blue = {mastering_display_primary_blue_x / 50000.0f,
				 mastering_display_primary_blue_y / 50000.0f},
		.white = {mastering_white_point_x / 50000.0f,
				  mastering_white_point_y / 50000.0f},
	};
	fs->data.has_mastering_luminance = max_display_mastering_luminance != 0;
	fs->data.mastering_luminance.max = (float)max_display_mastering_luminance;
	fs->data.mastering_luminance.min =
		min_display_mastering_luminance / 10000.0f;
	fs->data.max_cll = max_cll;
	fs->data.max_fall = max_fall;
	frog_surface_apply(fs);
}

static const struct frog_color_managed_surface_interface frog_surface_impl = {
	.destroy = frog_surface_handle_destroy,
	.set_known_transfer_function = frog_surface_set_known_transfer_function,
	.set_known_container_color_volume =
		frog_surface_set_known_container_color_volume,
	.set_render_intent = frog_surface_set_render_intent,
	.set_hdr_metadata = frog_surface_set_hdr_metadata,
};

/* the output metadata a frog client should target: the HDR monitor's PQ
 * envelope when one exists, the SDR defaults otherwise. gamescope flips
 * its HDR exposure on iff the transfer function here is ST2084_PQ. */
/*
 * SERIALIZE THE SHARED DESCRIPTION. This function decides nothing: which
 * output, whether it is HDR and what its mastering values are all come from
 * az_preferred_resolve(). What belongs here and nowhere else is frog's wire
 * format -- CIE xy in units of 1/50000, luminance in cd/m2, min luminance in
 * units of 0.0001 cd/m2.
 *
 * RETURNS whether it actually sent, so a caller can count.
 */
static bool frog_surface_send_preferred_metadata(FrogColorSurface *fs) {
	struct az_preferred pref;
	az_preferred_resolve(fs->surface, &pref);
	if (pref.identity == 0) {
		/* Not on an output yet. Say NOTHING rather than invent a display: the
		 * setmon path re-sends the instant it lands somewhere. */
		return false;
	}
	/*
	 * BREAK: AZ_BREAK_FROG_SEND_ONCE suppresses every send after the first, so
	 * the stale-metadata defect is reproducible. The object still exists, the
	 * walk still reaches it, the identity still changes -- only the wire event
	 * is withheld, which is exactly the shape of the original bug.
	 */
	static int break_send_once = -1;
	if (break_send_once < 0) {
		break_send_once = getenv("AZ_BREAK_FROG_SEND_ONCE") != NULL;
	}
	if (break_send_once && fs->sent_identity != 0) {
		return false;
	}
	if (pref.identity == fs->sent_identity) {
		/* ZERO CHURN. An HDR toggle on the other monitor, a hotplug elsewhere,
		 * a layout change that did not move this surface -- all reach here and
		 * all stop, at the cost of one comparison. */
		return false;
	}

	/* BT.2020 or BT.709, in the protocol's 1/50000 CIE xy units. */
	uint32_t rx = pref.bt2020 ? 35400 : 32000, ry = pref.bt2020 ? 14600 : 16500;
	uint32_t gx = pref.bt2020 ?  8500 : 15000, gy = pref.bt2020 ? 39850 : 30000;
	uint32_t bx = pref.bt2020 ?  6550 :  7500, by = pref.bt2020 ?  2300 :  3000;
	uint32_t wx = 15635, wy = 16450; /* D65 */

	frog_color_managed_surface_send_preferred_metadata(
		fs->resource,
		pref.hdr ? FROG_COLOR_MANAGED_SURFACE_TRANSFER_FUNCTION_ST2084_PQ
			: FROG_COLOR_MANAGED_SURFACE_TRANSFER_FUNCTION_GAMMA_22,
		rx, ry, gx, gy, bx, by, wx, wy,
		(uint32_t)pref.max_luminance,
		/* min luminance rides the wire in units of 0.0001 cd/m2 -- the rule's
		 * 0.4 becomes 4000. Rounded, not truncated: 0.4f * 10000 lands at
		 * 3999.99 in float and truncation would ship 3999. */
		(uint32_t)(pref.min_luminance * 10000.0f + 0.5f),
		(uint32_t)pref.max_fall);
	fs->sent_identity = pref.identity;
	frog_metadata_sends++;
	return true;
}

static void frog_factory_handle_destroy(struct wl_client *client,
										struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void frog_factory_get_color_managed_surface(
	struct wl_client *client, struct wl_resource *resource,
	struct wl_resource *surface_resource, uint32_t id) {
	struct wlr_surface *surface = wlr_surface_from_resource(surface_resource);

	FrogColorSurface *fs = calloc(1, sizeof(*fs));
	if (!fs) {
		wl_client_post_no_memory(client);
		return;
	}

	fs->resource =
		wl_resource_create(client, &frog_color_managed_surface_interface, 1, id);
	if (!fs->resource) {
		free(fs);
		wl_client_post_no_memory(client);
		return;
	}

	fs->surface = surface;
	wlr_addon_init(&fs->addon, &surface->addons, NULL,
				   &frog_surface_addon_impl);
	wl_resource_set_implementation(fs->resource, &frog_surface_impl, fs,
								   frog_surface_resource_destroy);

	wl_list_insert(&frog_surfaces, &fs->link);
	frog_surface_send_preferred_metadata(fs);
	/* No error if that sent nothing: a surface with no output yet simply has
	 * no preferred display, and setmon will deliver one. */
}

/*
 * ── M6B/D6: THE MASTERING VALUES, RE-SENT WHEN THEY CHANGE ────────────────
 *
 * Called from the same place the wp-cm preferred description is: after an HDR
 * state change COMMITS, and when a surface changes output. Both are the moments
 * the answer can differ, and neither is a frame event.
 *
 * frog carries what wp-cm currently cannot. wlroots 0.20.2 sends a preferred
 * image description's transfer function and primaries but drops its mastering
 * luminances and max_cll/max_fall on the floor (two TODOs in
 * types/wlr_color_management_v1.c), so on the wp-cm path a client learns THAT
 * the display is HDR and not HOW BRIGHT it goes. On this path it learns both --
 * the monitor rule's own max-luminance, min-luminance and max-fall -- which is
 * why this is worth keeping correct rather than treating as gamescope's legacy
 * corner.
 */
static void frog_send_preferred_metadata_all(Monitor *m) {
	if (!frog_surfaces_init) {
		return;
	}
	FrogColorSurface *fs;
	wl_list_for_each(fs, &frog_surfaces, link) {
		if (fs->resource == NULL) {
			continue;
		}
		/*
		 * NULL m means "reconsider every surface" -- a surface that MOVED has
		 * changed output and neither its old nor its new monitor identifies it
		 * usefully here. Otherwise only surfaces on the output that changed.
		 *
		 * Either way the identity comparison in the sender is what actually
		 * decides: this filter is an optimisation, not the correctness
		 * boundary, so getting it wrong costs a comparison rather than a wrong
		 * announcement.
		 */
		if (m != NULL && frog_surface_monitor(fs) != m) {
			continue;
		}
		frog_surface_send_preferred_metadata(fs);
	}
}

static const struct frog_color_management_factory_v1_interface
	frog_factory_impl = {
		.destroy = frog_factory_handle_destroy,
		.get_color_managed_surface = frog_factory_get_color_managed_surface,
};

static void frog_factory_bind(struct wl_client *client, void *data,
							  uint32_t version, uint32_t id) {
	struct wl_resource *resource = wl_resource_create(
		client, &frog_color_management_factory_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &frog_factory_impl, NULL, NULL);
}

/* gamescope takes the wp-color-management path whenever that global merely
 * EXISTS (WaylandBackend.cpp: `if (GetWPColorManager())`), then discovers
 * wlroots' feature set is too small and disables HDR -- it only ever uses
 * frog as a fallback when wp-cm is absent. So wp-cm is hidden from
 * gamescope processes specifically: they see frog alone and light up HDR,
 * while mpv/browsers keep the full wp-cm path. Identified by process name
 * -- the only reliable pre-bind signal a global filter gets. */
static const struct wl_global *filtered_wp_color_manager_global;

/* consulted from security_context_global_filter (the display's single
 * global-filter slot lives in modern.h) */
static bool frog_wp_color_manager_visible(const struct wl_client *client,
										  const struct wl_global *global) {
	if (global != filtered_wp_color_manager_global ||
		!filtered_wp_color_manager_global)
		return true;

	pid_t pid = 0;
	wl_client_get_credentials((struct wl_client *)client, &pid, NULL, NULL);
	if (pid <= 0)
		return true;

	char path[64], comm[32] = {0};
	snprintf(path, sizeof(path), "/proc/%d/comm", pid);
	FILE *f = fopen(path, "r");
	if (!f)
		return true;
	if (!fgets(comm, sizeof(comm), f))
		comm[0] = '\0';
	fclose(f);

	return strncmp(comm, "gamescope", 9) != 0;
}

static void frog_color_management_init(void) {
	if (!frog_surfaces_init) {
		wl_list_init(&frog_surfaces);
		frog_surfaces_init = true;
	}
	wl_global_create(dpy, &frog_color_management_factory_v1_interface, 1, NULL,
					 frog_factory_bind);
	if (color_manager)
		filtered_wp_color_manager_global = color_manager->global;
	/*
	 * THE SCENE FALLBACK IS NOT REGISTERED HERE ANY MORE.
	 *
	 * It is a single slot and two protocols need to answer through it, so
	 * whichever registered last would silently win. setup() registers one
	 * multiplexer for both (az_cm_surface_description) after both inits, which
	 * makes precedence a decision rather than an ordering accident.
	 */
	wlr_log(WLR_INFO, "frog-color-management-v1 enabled "
					  "(wp-color-management hidden from gamescope)");
}
