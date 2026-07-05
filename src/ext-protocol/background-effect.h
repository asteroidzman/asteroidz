/*
 * ext-background-effect-v1: lets clients request background effects (blur)
 * behind their surfaces, with a surface-local region.
 *
 * Policy: asteroidz already applies blur per its own config rules. A client
 * region refines that: a non-empty region clips the surface's blur node to
 * the region's extents, an explicitly empty region disables blur for the
 * surface, and a NULL region (or destroying the object) restores the
 * config-driven default.
 */
#include "ext-background-effect-v1-protocol.h"
#include <wlr/types/wlr_compositor.h>
#include <wlr/util/addon.h>

#define BACKGROUND_EFFECT_MANAGER_VERSION 1

void client_update_blur(Client *c);
void layer_update_blur(LayerSurface *l);

struct background_effect_surface {
	struct wl_resource *resource;
	struct wlr_surface *surface; /* NULL once the surface is destroyed */
	struct wlr_addon addon;

	pixman_region32_t pending_region, current_region;
	bool pending_has_region, has_region;
	bool pending_dirty;

	struct wl_listener surface_commit;
};

static struct wl_global *background_effect_global;
static const struct wlr_addon_interface background_effect_surface_addon_impl;

static void background_effect_update_target(struct wlr_surface *surface) {
	Client *c = NULL;
	LayerSurface *l = NULL;

	toplevel_from_wlr_surface(surface, &c, &l);
	if (c)
		client_update_blur(c);
	else if (l)
		layer_update_blur(l);
}

struct background_effect_surface *
background_effect_try_from_surface(struct wlr_surface *surface) {
	struct wlr_addon *addon;
	struct background_effect_surface *effect;

	if (!surface || !background_effect_global)
		return NULL;
	addon = wlr_addon_find(&surface->addons, background_effect_global,
						   &background_effect_surface_addon_impl);
	if (!addon)
		return NULL;
	effect = wl_container_of(addon, effect, addon);
	return effect;
}

static void background_effect_surface_handle_surface_commit(
	struct wl_listener *listener, void *data) {
	struct background_effect_surface *effect =
		wl_container_of(listener, effect, surface_commit);

	if (!effect->pending_dirty)
		return;

	pixman_region32_copy(&effect->current_region, &effect->pending_region);
	effect->has_region = effect->pending_has_region;
	effect->pending_dirty = false;

	background_effect_update_target(effect->surface);
}

static void
background_effect_surface_make_inert(struct background_effect_surface *effect) {
	if (!effect->surface)
		return;
	wlr_addon_finish(&effect->addon);
	wl_list_remove(&effect->surface_commit.link);
	effect->surface = NULL;
}

static void background_effect_surface_addon_destroy(struct wlr_addon *addon) {
	struct background_effect_surface *effect =
		wl_container_of(addon, effect, addon);
	background_effect_surface_make_inert(effect);
}

static const struct wlr_addon_interface background_effect_surface_addon_impl = {
	.name = "asteroidz_background_effect_surface",
	.destroy = background_effect_surface_addon_destroy,
};

static void background_effect_surface_handle_destroy(struct wl_client *client,
													 struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void background_effect_surface_handle_set_blur_region(
	struct wl_client *client, struct wl_resource *resource,
	struct wl_resource *region_resource) {
	struct background_effect_surface *effect =
		wl_resource_get_user_data(resource);

	if (!effect)
		return;
	if (!effect->surface) {
		wl_resource_post_error(
			resource, EXT_BACKGROUND_EFFECT_SURFACE_V1_ERROR_SURFACE_DESTROYED,
			"the associated surface has been destroyed");
		return;
	}

	if (region_resource) {
		const pixman_region32_t *region =
			wlr_region_from_resource(region_resource);
		pixman_region32_copy(&effect->pending_region, region);
		effect->pending_has_region = true;
	} else {
		pixman_region32_clear(&effect->pending_region);
		effect->pending_has_region = false;
	}
	effect->pending_dirty = true;
}

static const struct ext_background_effect_surface_v1_interface
	background_effect_surface_impl = {
		.destroy = background_effect_surface_handle_destroy,
		.set_blur_region = background_effect_surface_handle_set_blur_region,
};

static void
background_effect_surface_handle_resource_destroy(struct wl_resource *resource) {
	struct background_effect_surface *effect =
		wl_resource_get_user_data(resource);
	struct wlr_surface *surface;

	if (!effect)
		return;

	surface = effect->surface;
	background_effect_surface_make_inert(effect);
	pixman_region32_fini(&effect->pending_region);
	pixman_region32_fini(&effect->current_region);
	free(effect);

	/* effect regions are removed once the object goes away */
	if (surface)
		background_effect_update_target(surface);
}

static void background_effect_manager_handle_destroy(struct wl_client *client,
													 struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void background_effect_manager_handle_get_background_effect(
	struct wl_client *client, struct wl_resource *resource, uint32_t id,
	struct wl_resource *surface_resource) {
	struct wlr_surface *surface = wlr_surface_from_resource(surface_resource);
	struct background_effect_surface *effect;

	if (wlr_addon_find(&surface->addons, background_effect_global,
					   &background_effect_surface_addon_impl)) {
		wl_resource_post_error(
			resource,
			EXT_BACKGROUND_EFFECT_MANAGER_V1_ERROR_BACKGROUND_EFFECT_EXISTS,
			"the surface already has a background effect object");
		return;
	}

	effect = ecalloc(1, sizeof(*effect));
	effect->resource = wl_resource_create(
		client, &ext_background_effect_surface_v1_interface,
		wl_resource_get_version(resource), id);
	if (!effect->resource) {
		free(effect);
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(
		effect->resource, &background_effect_surface_impl, effect,
		background_effect_surface_handle_resource_destroy);

	effect->surface = surface;
	pixman_region32_init(&effect->pending_region);
	pixman_region32_init(&effect->current_region);

	wlr_addon_init(&effect->addon, &surface->addons, background_effect_global,
				   &background_effect_surface_addon_impl);

	effect->surface_commit.notify =
		background_effect_surface_handle_surface_commit;
	wl_signal_add(&surface->events.commit, &effect->surface_commit);
}

static const struct ext_background_effect_manager_v1_interface
	background_effect_manager_impl = {
		.destroy = background_effect_manager_handle_destroy,
		.get_background_effect =
			background_effect_manager_handle_get_background_effect,
};

static void background_effect_manager_bind(struct wl_client *client, void *data,
										   uint32_t version, uint32_t id) {
	struct wl_resource *resource = wl_resource_create(
		client, &ext_background_effect_manager_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &background_effect_manager_impl,
								   NULL, NULL);

	ext_background_effect_manager_v1_send_capabilities(
		resource,
		config.blur ? EXT_BACKGROUND_EFFECT_MANAGER_V1_CAPABILITY_BLUR : 0);
}

void background_effect_manager_create(struct wl_display *display) {
	background_effect_global = wl_global_create(
		display, &ext_background_effect_manager_v1_interface,
		BACKGROUND_EFFECT_MANAGER_VERSION, NULL,
		background_effect_manager_bind);
}
