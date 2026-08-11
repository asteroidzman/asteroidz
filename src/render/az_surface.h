#ifndef AZ_SURFACE_H
#define AZ_SURFACE_H

/*
 * What buffer is this surface showing?
 *
 * That question used to have an easy answer -- `surface->buffer` -- and the
 * easy answer was wrong in a way nothing noticed until AVK stopped using
 * wlroots' renderer.
 *
 * `wlr_surface.buffer` is not the client's buffer. It is a wlr_client_buffer:
 * a wrapper wlroots creates so it can upload the client's pixels into a
 * wlr_texture. It exists only when the compositor was given a renderer, and
 * asteroidz deliberately does not give it one in AVK mode -- so every test of
 * the form `if (surface->buffer)` silently became "if wlroots is doing the
 * rendering", which is not what any of those call sites meant. One of them
 * disabled every layer-shell surface on the desktop, wallpaper included.
 *
 * `wlr_surface.current.buffer` is the client's real buffer, but it is live
 * only for the duration of the commit event: wlroots unlocks it as soon as the
 * last listener has run (see the comment in surface_commit_state()). Reading
 * it from a render frame gets NULL.
 *
 * So neither field answers the question at an arbitrary moment, and this file
 * exists to. It records the committed buffer per surface and holds a lock on
 * it, which is exactly the persistence the wlr_texture used to provide by
 * accident. The record is installed on every surface in both renderer modes,
 * because "which buffer is this showing" should not have two answers depending
 * on who is drawing.
 */

#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/util/addon.h>

struct az_surface {
	struct wlr_addon addon;
	struct wlr_surface *surface;
	/* Locked. Released when replaced or when the surface goes away, so it
	 * outlives the client's own reference exactly as long as we need it. */
	struct wlr_buffer *buffer;
	struct wl_listener commit;
};

static const struct wlr_addon_interface az_surface_addon_impl;

static void az_surface_destroy(struct az_surface *record) {
	if (record->buffer != NULL) {
		wlr_buffer_unlock(record->buffer);
		record->buffer = NULL;
	}
	wl_list_remove(&record->commit.link);
	wlr_addon_finish(&record->addon);
	free(record);
}

static void az_surface_addon_destroy(struct wlr_addon *addon) {
	struct az_surface *record = wl_container_of(addon, record, addon);
	az_surface_destroy(record);
}

static const struct wlr_addon_interface az_surface_addon_impl = {
	.name = "az_surface",
	.destroy = az_surface_addon_destroy,
};

static struct az_surface *az_surface_get(struct wlr_surface *surface) {
	if (surface == NULL) {
		return NULL;
	}
	struct wlr_addon *addon = wlr_addon_find(&surface->addons,
		&az_surface_addon_impl, &az_surface_addon_impl);
	if (addon == NULL) {
		return NULL;
	}
	struct az_surface *record = wl_container_of(addon, record, addon);
	return record;
}

/*
 * The buffer this surface is currently showing, or NULL if it has none.
 *
 * Valid at any time, in either renderer mode. This is what every
 * `surface->buffer` test in asteroidz should have been.
 */
static inline struct wlr_buffer *az_surface_buffer(
		struct wlr_surface *surface) {
	struct az_surface *record = az_surface_get(surface);
	return record != NULL ? record->buffer : NULL;
}

/* Whether the surface has content to draw. Prefer wlr_surface_has_buffer()
 * where only the yes/no matters -- it reads the committed dimensions and needs
 * no record at all -- and this where the buffer itself is wanted too. */
static inline bool az_surface_has_content(struct wlr_surface *surface) {
	return az_surface_buffer(surface) != NULL;
}

static void az_surface_handle_commit(struct wl_listener *listener, void *data) {
	struct az_surface *record = wl_container_of(listener, record, commit);
	struct wlr_buffer *next = record->surface->current.buffer;
	if (!(record->surface->current.committed & WLR_SURFACE_STATE_BUFFER)) {
		/* No buffer in this commit: the surface is still showing whatever it
		 * showed before, so the record must not be touched. A commit that only
		 * moves a subsurface or updates an input region is not a buffer
		 * change. */
		return;
	}
	if (record->buffer != NULL) {
		wlr_buffer_unlock(record->buffer);
	}
	record->buffer = next != NULL ? wlr_buffer_lock(next) : NULL;
}

static void az_new_surface_notify(struct wl_listener *listener, void *data);

static struct wl_listener az_new_surface_listener = {
	.notify = az_new_surface_notify,
};
static bool az_new_surface_attached = false;

/* Detach before wl_display_destroy(): wlr_compositor asserts that nothing is
 * still listening to new_surface when it is torn down. */
static inline void az_surface_detach(void) {
	if (az_new_surface_attached) {
		wl_list_remove(&az_new_surface_listener.link);
		az_new_surface_attached = false;
	}
}

/* Start tracking `surface`. Call once, from the compositor's new_surface. */
static void az_surface_track(struct wlr_surface *surface) {
	struct az_surface *record = calloc(1, sizeof(*record));
	if (record == NULL) {
		return;
	}
	record->surface = surface;
	wlr_addon_init(&record->addon, &surface->addons, &az_surface_addon_impl,
		&az_surface_addon_impl);
	record->commit.notify = az_surface_handle_commit;
	wl_signal_add(&surface->events.commit, &record->commit);
}

static void az_new_surface_notify(struct wl_listener *listener, void *data) {
	az_surface_track(data);
}

#endif /* AZ_SURFACE_H */
