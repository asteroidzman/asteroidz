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
} FrogColorSurface;

static void frog_surface_addon_destroy(struct wlr_addon *addon) {
	FrogColorSurface *fs = wl_container_of(addon, fs, addon);
	wlr_addon_finish(&fs->addon);
	fs->surface = NULL;
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
static void frog_surface_send_preferred_metadata(struct wl_resource *resource) {
	Monitor *m = NULL, *it;
	wl_list_for_each(it, &mons, link) {
		if (it->wlr_output->enabled && it->hdr) {
			m = it;
			break;
		}
	}
	if (!m)
		m = selmon;

	bool hdr = m && m->hdr;
	/* BT.2020 / sRGB primaries in the protocol's 1/50000 CIE xy units */
	uint32_t rx = hdr ? 35400 : 32000, ry = hdr ? 14600 : 16500;
	uint32_t gx = hdr ? 8500 : 15000, gy = hdr ? 39850 : 30000;
	uint32_t bx = hdr ? 6550 : 7500, by = hdr ? 2300 : 3000;
	uint32_t wx = 15635, wy = 16450; /* D65 */

	float max_lum = 203.0f, min_lum = 0.2f, max_fall = 203.0f;
	if (hdr) {
		max_lum = m->hdr_max_luminance > 0 ? m->hdr_max_luminance : 1000.0f;
		min_lum = m->hdr_min_luminance >= 0 ? m->hdr_min_luminance : 0.005f;
		max_fall = m->hdr_max_fall > 0 ? m->hdr_max_fall : max_lum;
	}

	frog_color_managed_surface_send_preferred_metadata(
		resource,
		hdr ? FROG_COLOR_MANAGED_SURFACE_TRANSFER_FUNCTION_ST2084_PQ
			: FROG_COLOR_MANAGED_SURFACE_TRANSFER_FUNCTION_GAMMA_22,
		rx, ry, gx, gy, bx, by, wx, wy, (uint32_t)max_lum,
		(uint32_t)(min_lum * 10000.0f), (uint32_t)max_fall);
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

	frog_surface_send_preferred_metadata(fs->resource);
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
	wl_global_create(dpy, &frog_color_management_factory_v1_interface, 1, NULL,
					 frog_factory_bind);
	if (color_manager)
		filtered_wp_color_manager_global = color_manager->global;
	wlr_scene_set_surface_color_description_fallback(
		frog_surface_image_description);
	wlr_log(WLR_INFO, "frog-color-management-v1 enabled "
					  "(wp-color-management hidden from gamescope)");
}
