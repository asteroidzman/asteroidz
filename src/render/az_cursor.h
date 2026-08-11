/*
 * az_cursor.h -- asteroidz's own cursor image state.
 *
 * WHY THIS EXISTS
 *
 * §5.4g of docs/vulkan-native-architecture.md audits the three cursor sources
 * and finds that exactly one of them broke when AVK stopped giving
 * wl_compositor a renderer. The break is `wlr_cursor.c:560`:
 *
 *     struct wlr_texture *texture = wlr_surface_get_texture(surface);
 *
 * which reads `surface->buffer` -- the `wlr_client_buffer` wrapper wlroots
 * creates only when the compositor has a renderer. AVK passes NULL, so the
 * texture is NULL, so `output_cursor_set_texture()` disables the cursor
 * outright: not a slow cursor, no cursor, on the hardware plane and in
 * software alike, and nothing logs it.
 *
 * xcursor and cursor-shape were never affected, because both end at
 * `wlr_xcursor_image_get_buffer()` and hand wlroots a **wlr_buffer**.
 *
 * That difference is the whole design. A `wlr_buffer` is the one
 * representation both consumers can take: wlroots imports it for the hardware
 * plane, and AVK imports it through the same content-generation cache M3.5D.1
 * built for client surfaces. So asteroidz takes over choosing the image -- and
 * only choosing the image -- for all three sources, and always expresses the
 * answer as a buffer.
 *
 * WHAT MOVES AND WHAT DOES NOT
 *
 *   cursor image      -> here. Which buffer, and when it changes.
 *   cursor hotspot    -> here, in buffer pixels, applied by whoever draws.
 *   cursor position   -> wlroots. wlr_cursor and wlr_output_cursor keep it,
 *                        including the per-output box and visibility.
 *   hardware plane    -> wlroots. Still the preferred path, for every source.
 *   software drawing  -> AVK, from this state.
 *
 * Position deliberately stays where it is. wlr_output_cursor already computes
 * the per-output box, applies output scale and transform, and tracks which
 * outputs the cursor is visible on; reimplementing that would be a rewrite
 * with no benefit, and this milestone is about the image.
 *
 * WHAT THIS COSTS
 *
 * wlroots picks the xcursor image per output, at that output's scale, with a
 * per-output animation timer. asteroidz picks one image for all outputs, at
 * the largest scale any output uses -- the same rule wlroots itself applies to
 * client surfaces. On a mixed-scale layout the cursor is therefore drawn from
 * a buffer sized for the sharper output and scaled down on the other, which is
 * correct but marginally softer than wlroots would have been. In exchange the
 * image is one thing rather than N, which is what makes it possible for AVK to
 * draw it at all: `wlr_cursor_output_cursor` is private, so its per-output
 * animation index cannot be read from outside wlroots, and an AVK that guessed
 * would show a different animation frame than the hardware plane.
 */
#ifndef AZ_CURSOR_H
#define AZ_CURSOR_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/util/log.h>
#include <wlr/xcursor.h>

#include "az_surface.h"

enum az_cursor_source {
	AZ_CURSOR_SOURCE_NONE,
	AZ_CURSOR_SOURCE_XCURSOR,
	AZ_CURSOR_SOURCE_CLIENT,
};

static struct az_cursor {
	enum az_cursor_source source;

	/*
	 * The image. Locked for as long as this points at it, because an xcursor
	 * buffer is owned by the theme and a client buffer by a client that may
	 * release it the instant it commits the next one.
	 */
	struct wlr_buffer *buffer;
	int hotspot_x, hotspot_y;   /* buffer pixels */
	float scale;                /* buffer pixels per logical pixel */

	/*
	 * Bumped whenever the PIXELS may differ.
	 *
	 * Not the same question as "did the buffer pointer change", and M3.5D.1
	 * exists because conflating those two is how a compositor either re-uploads
	 * an unchanged image forever or shows a stale one forever. A client is
	 * entitled to commit new content into a wl_buffer it already owns, and a
	 * cursor surface is no exception.
	 */
	uint64_t generation;

	/* ── xcursor source ── */
	struct wlr_xcursor *xcursor;
	size_t xcursor_index;
	struct wl_event_source *xcursor_timer;
	char *xcursor_name;
	float xcursor_scale;

	/* ── client surface source ── */
	struct wlr_surface *surface;
	int surface_hotspot_x, surface_hotspot_y;
	struct wl_listener surface_commit;
	struct wl_listener surface_destroy;
	bool surface_listening;

	/* ── counters, reported through avk-stats ── */
	uint64_t sets;              /* image changes pushed to wlroots */
	uint64_t generations;       /* content generations observed */
	uint64_t xcursor_frames;    /* animation ticks */
	uint64_t forced_reimports;  /* same buffer, new pixels (see below) */
	uint64_t client_no_buffer;  /* set_cursor with nothing committed yet */
} az_cursor;

/* The scale to pick an image for: the sharpest output in the layout. */
static float az_cursor_target_scale(void) {
	float scale = 1.0f;
	Monitor *m;
	wl_list_for_each(m, &mons, link) {
		if (m->wlr_output != NULL && m->wlr_output->enabled &&
				m->wlr_output->scale > scale) {
			scale = m->wlr_output->scale;
		}
	}
	return scale;
}

/*
 * Hand the current image to wlroots.
 *
 * One subtlety, and it is the cursor's version of the M3.5D.1 bug:
 * wlr_cursor_set_buffer() returns early when the buffer pointer, hotspot and
 * scale all match what it already has. For a client that animates a cursor by
 * rewriting one wl_buffer, every frame after the first would be dropped and
 * the plane would freeze on the first image. Clearing the image first forces
 * the re-import. It costs an extra cursor commit, so it happens only when the
 * content generation actually moved under an unchanged pointer, and it is
 * counted rather than hidden.
 */
static void az_cursor_push(struct wlr_buffer *buffer, int hotspot_x,
		int hotspot_y, float scale, bool content_changed) {
	if (cursor == NULL) {
		return;
	}
	if (buffer == NULL) {
		wlr_cursor_unset_image(cursor);
		return;
	}
	if (content_changed) {
		wlr_cursor_unset_image(cursor);
		az_cursor.forced_reimports++;
	}
	wlr_cursor_set_buffer(cursor, buffer, hotspot_x, hotspot_y, scale);
}

/*
 * Adopt a new image.
 *
 * `same_pointer_new_content` is the case above: the caller knows the pixels
 * changed even though the buffer did not.
 */
static void az_cursor_set_image(struct wlr_buffer *buffer, int hotspot_x,
		int hotspot_y, float scale, bool same_pointer_new_content) {
	bool changed = buffer != az_cursor.buffer ||
		hotspot_x != az_cursor.hotspot_x || hotspot_y != az_cursor.hotspot_y ||
		scale != az_cursor.scale || same_pointer_new_content;

	if (buffer != az_cursor.buffer) {
		wlr_buffer_unlock(az_cursor.buffer);
		az_cursor.buffer = buffer != NULL ? wlr_buffer_lock(buffer) : NULL;
	}
	az_cursor.hotspot_x = hotspot_x;
	az_cursor.hotspot_y = hotspot_y;
	az_cursor.scale = scale;

	if (changed) {
		az_cursor.generation++;
		az_cursor.generations++;
		az_cursor.sets++;
	}

	az_cursor_push(buffer, hotspot_x, hotspot_y, scale,
		same_pointer_new_content);
}

/* ── xcursor ─────────────────────────────────────────────────────────────── */

static void az_cursor_xcursor_show(size_t index);

static int az_cursor_xcursor_tick(void *data) {
	(void)data;
	if (az_cursor.xcursor == NULL || az_cursor.xcursor->image_count == 0) {
		return 0;
	}
	az_cursor.xcursor_frames++;
	az_cursor_xcursor_show(
		(az_cursor.xcursor_index + 1) % az_cursor.xcursor->image_count);
	return 0;
}

static void az_cursor_xcursor_show(size_t index) {
	struct wlr_xcursor *xc = az_cursor.xcursor;
	if (xc == NULL || index >= xc->image_count) {
		return;
	}
	struct wlr_xcursor_image *image = xc->images[index];
	az_cursor.xcursor_index = index;

	struct wlr_buffer *buffer = wlr_xcursor_image_get_buffer(image);
	if (buffer == NULL) {
		return;
	}
	/* Borrowed, not owned: wlr_xcursor_image_get_buffer() hands back the
	 * theme's own buffer, which normally sits at zero locks. Unlocking it here
	 * would release the lock az_cursor_set_image() takes -- and then the next
	 * image change would unlock it a second time, stealing the one wlr_cursor
	 * holds. That is an assertion inside wlr_cursor_set_buffer(), one image
	 * change later, which is nowhere near where the mistake was made.
	 *
	 * Each animation frame is a distinct buffer owned by the theme, so no
	 * forced re-import is ever needed here. */
	az_cursor_set_image(buffer, image->hotspot_x, image->hotspot_y,
		az_cursor.xcursor_scale, false);

	if (xc->image_count > 1 && image->delay > 0 &&
			az_cursor.xcursor_timer != NULL) {
		wl_event_source_timer_update(az_cursor.xcursor_timer, image->delay);
	}
}

static void az_cursor_drop_surface(void);

static void az_cursor_set_xcursor(const char *name) {
	if (cursor_mgr == NULL || name == NULL) {
		return;
	}
	az_cursor_drop_surface();

	float scale = az_cursor_target_scale();
	wlr_xcursor_manager_load(cursor_mgr, scale);
	struct wlr_xcursor *xc =
		wlr_xcursor_manager_get_xcursor(cursor_mgr, name, scale);
	if (xc == NULL) {
		/* Better the wrong image than an invisible pointer -- the same
		 * fallback wlroots makes, for the same reason. */
		xc = wlr_xcursor_manager_get_xcursor(cursor_mgr, "default", scale);
	}
	if (xc == NULL || xc->image_count == 0) {
		wlr_log(WLR_DEBUG, "cursor: theme has neither '%s' nor 'default'",
			name);
		return;
	}

	/* Re-selecting the same cursor at the same scale must not restart its
	 * animation: pointer motion re-asserts the cursor constantly. */
	if (az_cursor.source == AZ_CURSOR_SOURCE_XCURSOR &&
			az_cursor.xcursor == xc && az_cursor.xcursor_scale == scale) {
		return;
	}

	az_cursor.source = AZ_CURSOR_SOURCE_XCURSOR;
	az_cursor.xcursor = xc;
	az_cursor.xcursor_scale = scale;
	free(az_cursor.xcursor_name);
	az_cursor.xcursor_name = strdup(name);

	if (xc->image_count > 1 && az_cursor.xcursor_timer == NULL) {
		az_cursor.xcursor_timer = wl_event_loop_add_timer(
			wl_display_get_event_loop(dpy), az_cursor_xcursor_tick, NULL);
	}
	if (xc->image_count == 1 && az_cursor.xcursor_timer != NULL) {
		wl_event_source_timer_update(az_cursor.xcursor_timer, 0);
	}
	az_cursor_xcursor_show(0);
}

/* ── client surface ──────────────────────────────────────────────────────── */

/*
 * The bookkeeping wlr_cursor_set_surface() would have done.
 *
 * A cursor surface is a surface: it needs to know which outputs it is on so it
 * can pick a buffer scale, and it needs its frame callbacks to keep animating.
 * Dropping this on the floor would leave an animated client cursor stalled on
 * its first frame with no error anywhere.
 */
static void az_cursor_surface_outputs(struct wlr_surface *surface) {
	float scale = 1.0f;
	Monitor *m;
	wl_list_for_each(m, &mons, link) {
		if (m->wlr_output == NULL) {
			continue;
		}
		bool on = m == xytomon(cursor->x, cursor->y);
		if (on) {
			wlr_surface_send_enter(surface, m->wlr_output);
			if (m->wlr_output->scale > scale) {
				scale = m->wlr_output->scale;
			}
		} else {
			wlr_surface_send_leave(surface, m->wlr_output);
		}
	}
	wlr_fractional_scale_v1_notify_scale(surface, scale);
	wlr_surface_set_preferred_buffer_scale(surface, (int32_t)ceilf(scale));
}

static void az_cursor_surface_update(bool content_changed) {
	struct wlr_surface *surface = az_cursor.surface;
	if (surface == NULL) {
		return;
	}
	struct wlr_buffer *buffer = az_surface_buffer(surface);
	if (buffer == NULL) {
		/* set_cursor before the surface has committed anything. Legal, and it
		 * means "hide the pointer" until content arrives. */
		az_cursor.client_no_buffer++;
		az_cursor_set_image(NULL, 0, 0, 1.0f, false);
		return;
	}

	/* Hotspots arrive in surface-local coordinates and the image is in buffer
	 * pixels, so the surface's own buffer scale is the conversion. */
	float scale = surface->current.scale > 0 ? (float)surface->current.scale
		: 1.0f;
	az_cursor_set_image(buffer,
		(int)roundf(az_cursor.surface_hotspot_x * scale),
		(int)roundf(az_cursor.surface_hotspot_y * scale), scale,
		content_changed);

	az_cursor_surface_outputs(surface);
}

static void az_cursor_handle_surface_commit(struct wl_listener *listener,
		void *data) {
	(void)listener;
	(void)data;
	/* A commit on a cursor surface is exactly the statement "these pixels are
	 * current", whether or not the wl_buffer is the same object as last time.
	 * That is the D.1 content model, applied to the cursor. */
	az_cursor_surface_update(true);
}

static void az_cursor_handle_surface_destroy(struct wl_listener *listener,
		void *data) {
	(void)listener;
	(void)data;
	az_cursor_drop_surface();
	az_cursor_set_image(NULL, 0, 0, 1.0f, false);
	az_cursor.source = AZ_CURSOR_SOURCE_NONE;
}

static void az_cursor_drop_surface(void) {
	if (!az_cursor.surface_listening) {
		return;
	}
	wl_list_remove(&az_cursor.surface_commit.link);
	wl_list_remove(&az_cursor.surface_destroy.link);
	az_cursor.surface_listening = false;
	az_cursor.surface = NULL;
}

/*
 * BREAK SWITCH: put the client cursor back on wlroots' texture path.
 *
 * AZ_CURSOR_LEGACY_SURFACE=1 calls wlr_cursor_set_surface() exactly as
 * asteroidz did before M3.5E, which under a NULL-renderer wl_compositor means
 * wlr_surface_get_texture() returns NULL and the cursor is disabled. This is
 * not a simulated failure: it is the original call, restored. It exists
 * because contrib/avk-cursor-test.sh has to be able to fail, and a cursor test
 * that cannot distinguish "the cursor is drawn" from "no cursor exists and
 * nothing was asserted" is worth nothing.
 */
static bool az_cursor_legacy_surface(void) {
	static int value = -1;
	if (value < 0) {
		const char *env = getenv("AZ_CURSOR_LEGACY_SURFACE");
		value = (env != NULL && env[0] != '\0' && strcmp(env, "0") != 0);
	}
	return value == 1;
}

static void az_cursor_set_surface(struct wlr_surface *surface, int hotspot_x,
		int hotspot_y) {
	if (az_cursor_legacy_surface()) {
		wlr_cursor_set_surface(cursor, surface, hotspot_x, hotspot_y);
		return;
	}
	if (surface == NULL) {
		az_cursor_drop_surface();
		az_cursor.source = AZ_CURSOR_SOURCE_NONE;
		az_cursor_set_image(NULL, 0, 0, 1.0f, false);
		return;
	}

	if (az_cursor.surface != surface) {
		az_cursor_drop_surface();
		az_cursor.surface = surface;
		az_cursor.surface_commit.notify = az_cursor_handle_surface_commit;
		wl_signal_add(&surface->events.commit, &az_cursor.surface_commit);
		az_cursor.surface_destroy.notify = az_cursor_handle_surface_destroy;
		wl_signal_add(&surface->events.destroy, &az_cursor.surface_destroy);
		az_cursor.surface_listening = true;
	}

	az_cursor.source = AZ_CURSOR_SOURCE_CLIENT;
	az_cursor.surface_hotspot_x = hotspot_x;
	az_cursor.surface_hotspot_y = hotspot_y;
	az_cursor.xcursor = NULL;

	/* Re-asserting the same surface is not new content; only a commit is. */
	az_cursor_surface_update(false);
}

/* ── shared ──────────────────────────────────────────────────────────────── */

/* Hide the pointer without forgetting what it was. handlecursoractivity()
 * puts it back by replaying the source. */
static void az_cursor_hide(void) {
	if (az_cursor.xcursor_timer != NULL) {
		wl_event_source_timer_update(az_cursor.xcursor_timer, 0);
	}
	az_cursor_push(NULL, 0, 0, 1.0f, false);
}

static void az_cursor_show(void) {
	switch (az_cursor.source) {
	case AZ_CURSOR_SOURCE_XCURSOR:
		az_cursor_xcursor_show(az_cursor.xcursor_index);
		break;
	case AZ_CURSOR_SOURCE_CLIENT:
		az_cursor_surface_update(false);
		break;
	case AZ_CURSOR_SOURCE_NONE:
		break;
	}
}

/* An output's scale changed, so the image chosen for the old scale may be the
 * wrong size now. Only the xcursor source cares: a client picks its own. */
static void az_cursor_reload(void) {
	if (az_cursor.source != AZ_CURSOR_SOURCE_XCURSOR ||
			az_cursor.xcursor_name == NULL) {
		return;
	}
	if (az_cursor_target_scale() == az_cursor.xcursor_scale) {
		return;
	}
	char *name = strdup(az_cursor.xcursor_name);
	if (name == NULL) {
		return;
	}
	az_cursor.xcursor = NULL;   /* force reselection */
	az_cursor_set_xcursor(name);
	free(name);
}

static void az_cursor_finish(void) {
	az_cursor_drop_surface();
	if (az_cursor.xcursor_timer != NULL) {
		wl_event_source_remove(az_cursor.xcursor_timer);
		az_cursor.xcursor_timer = NULL;
	}
	wlr_buffer_unlock(az_cursor.buffer);
	az_cursor.buffer = NULL;
	free(az_cursor.xcursor_name);
	az_cursor.xcursor_name = NULL;
}

#endif /* AZ_CURSOR_H */
