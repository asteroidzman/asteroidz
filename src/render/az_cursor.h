/*
 * az_cursor.h -- asteroidz's own cursor image state.
 *
 * WHY THIS EXISTS
 *
 * §5.4g of docs/architecture.md audits the three cursor sources
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
	/*
	 * The hotspot in LOGICAL units -- the same units as the cursor's on-screen
	 * size, not the buffer's pixels.
	 *
	 * This is the convention wlr_cursor_set_buffer() wants, and it is not the
	 * one its sibling uses: wlr_output_cursor_set_buffer() takes buffer pixels
	 * and divides by the output scale, while wlr_cursor_set_buffer() passes
	 * the value straight through to output_cursor_set_texture(), which
	 * MULTIPLIES by the output scale -- so it has to arrive already reduced by
	 * the buffer's own scale, exactly like the dst_width it is paired with.
	 *
	 * The two conventions are identical at scale 1, which is why getting this
	 * wrong is invisible on an ordinary desktop and shows up only as a
	 * scale-2 cursor offset by half of itself.
	 */
	int hotspot_x, hotspot_y;
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

	/* ── xcursor source ──
	 *
	 * The durable state here is the NAME and the scale, deliberately: a
	 * `struct wlr_xcursor *` belongs to a theme inside wlr_xcursor_manager,
	 * and `wlr_xcursor_manager_destroy()` frees every cursor, image and pixel
	 * buffer it owns. reapply_cursor_style() destroys and recreates that
	 * manager on ANY live config change, so a cached pointer outlives its
	 * owner by construction. It is resolved from the current manager at the
	 * point of use and never stored.
	 *
	 * That cost a live desktop: az_cursor_show() replayed the cached pointer
	 * to restore the cursor after an idle hide, read a freed
	 * wlr_xcursor_image, and handed wlroots a wild wlr_buffer.
	 * `image->hotspot_x / scale` on freed memory produced a NaN, so the core
	 * dump's hotspot read INT32_MIN -- the saturating result of casting it to
	 * int, and the fingerprint of this bug if it ever comes back.
	 */
	size_t xcursor_index;
	struct wl_event_source *xcursor_timer;
	char *xcursor_name;
	float xcursor_scale;

	/*
	 * DIAGNOSTIC ONLY -- not cursor state, and deliberately not consulted by
	 * anything that draws.
	 *
	 * The production model above has no durable borrowed pointer, so the bug
	 * this milestone fixed can no longer be reached; that also means nothing
	 * observable is left to write a regression test against. These two fields
	 * let a test build the OLD model on purpose -- keep the borrowed pointer,
	 * remember which manager generation produced it -- so the invalid lifetime
	 * can be detected as a generation mismatch BEFORE the freed memory is
	 * read. That turns a bug which only manifested as undefined behaviour into
	 * a deterministic assertion.
	 */
	struct wlr_xcursor *dbg_borrowed;
	uint64_t dbg_borrowed_generation;

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
	/* Which SOURCE asked for the image. Kept apart because they are three
	 * different paths through wlroots (§5.4g) and only one of them was ever
	 * broken; a session where client sets are 0 is a session that has not
	 * exercised the thing M3.5E fixed. */
	uint64_t sets_client;       /* wl_pointer.set_cursor with a surface */
	uint64_t sets_shape;        /* wp_cursor_shape_v1 */
	uint64_t sets_xcursor;      /* the compositor's own theme */
	uint64_t unsets;            /* set_cursor(NULL), or hidden */
	/* Borrowed resolutions used after their manager was replaced. Must be 0:
	 * a nonzero value is a use-after-free that has not faulted yet. */
	uint64_t stale_xcursor;

	/*
	 * Whether wlroots currently HAS an image from us.
	 *
	 * Not derivable from `buffer`, which deliberately survives a hide so the
	 * cursor can come back. Without this the re-selection guard below is a
	 * trap: hide the cursor, ask for the same xcursor again, and the guard
	 * says "already showing that" and returns without ever pushing the image
	 * back -- an invisible pointer that stays invisible.
	 */
	bool image_pushed;
} az_cursor;

/*
 * Never use the cursor plane.
 *
 * Read once. The mechanism is wlr_output_lock_software_cursors(), which
 * already exists and is what screencopy's overlay_cursor trips -- there is no
 * new backend behaviour here, only a permanent version of a lock that is
 * normally held for the duration of a capture.
 */
static bool az_cursor_force_software(void) {
	static int value = -1;
	if (value < 0) {
		const char *env = getenv("ASTEROIDZ_AVK_FORCE_SOFTWARE_CURSOR");
		value = (env != NULL && env[0] == '1');
	}
	return value == 1;
}

/*
 * Load the theme at every scale anything might ask for, and return the one to
 * pick an image at: the sharpest output in the layout.
 *
 * Loading EVERY output's scale matters even though only one is used here,
 * because this is no longer the only consumer of the manager. wlroots used to
 * load per output as a side effect of choosing a per-output image; taking that
 * over meant scales nobody here wanted stopped being loaded, and
 * wlr_xcursor_manager_get_xcursor() returns NULL for a scale that was never
 * loaded rather than loading it on demand.
 *
 * That is not hypothetical. xwaylandready() asks for "default" at scale 1 and
 * hands the result to wlr_xwayland_set_cursor(). On a layout whose sharpest
 * output is 1.5, scale 1 was never loaded, the lookup returned NULL, XWayland
 * was never given a cursor at all -- and every X11 window showed the X server's
 * own 'X' root cursor. Nothing logged it, on either side.
 *
 * Scale 1 is loaded unconditionally for that reason: it is what a consumer with
 * no output in hand asks for.
 */
static float az_cursor_target_scale(void) {
	float scale = 1.0f;
	/* BREAK SWITCH: load only the sharpest scale, as the shipped bug did. */
	bool one_scale = getenv("AZ_CURSOR_ONE_SCALE") != NULL;
	if (cursor_mgr != NULL && !one_scale) {
		wlr_xcursor_manager_load(cursor_mgr, 1.0f);
	}
	Monitor *m;
	wl_list_for_each(m, &mons, link) {
		if (m->wlr_output == NULL || !m->wlr_output->enabled) {
			continue;
		}
		if (cursor_mgr != NULL && !one_scale) {
			wlr_xcursor_manager_load(cursor_mgr, m->wlr_output->scale);
		}
		if (m->wlr_output->scale > scale) {
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
		az_cursor.image_pushed = false;
		return;
	}
	if (content_changed) {
		wlr_cursor_unset_image(cursor);
		az_cursor.forced_reimports++;
	}
	wlr_cursor_set_buffer(cursor, buffer, hotspot_x, hotspot_y, scale);
	az_cursor.image_pushed = true;
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

/*
 * Which generation of the xcursor manager is current.
 *
 * Bumped every time the manager is destroyed and rebuilt. Nothing in the
 * drawing path reads it -- it exists so a test can state the ownership rule
 * as a checkable proposition:
 *
 *   a borrowed xcursor may only be dereferenced while the manager generation
 *   that produced it is still current.
 *
 * wlr_xcursor_manager_load() is NOT a generation change. It appends a scaled
 * theme to a list and frees nothing (wlr_xcursor_manager.c:33-54), so pointers
 * handed out earlier stay valid. Only destruction invalidates, which is worth
 * writing down because the first theory of the live crash blamed mixed-scale
 * loading and was wrong.
 */
static uint64_t az_cursor_mgr_generation = 1;

static void az_cursor_manager_replaced(void) {
	az_cursor_mgr_generation++;
}

/*
 * The xcursor for the current name and scale, from the CURRENT manager.
 *
 * Always a fresh lookup. The manager owns what it returns and frees it on
 * destroy, so the answer is only valid until the next thing that could
 * rebuild the manager -- which is why nothing keeps it.
 */
static struct wlr_xcursor *az_cursor_resolve_xcursor(void) {
	if (cursor_mgr == NULL || az_cursor.source != AZ_CURSOR_SOURCE_XCURSOR ||
			az_cursor.xcursor_name == NULL) {
		return NULL;
	}

	/*
	 * BREAK SWITCH: AZ_CURSOR_STALE_XCURSOR=1 restores the shipped bug --
	 * keep the borrowed pointer and replay it instead of asking the manager
	 * again. The generation check in front of the dereference is what makes
	 * the defect observable: the old code went straight to freed memory and
	 * only sometimes faulted, which is why ~110 headless attempts under ASan
	 * never caught it.
	 */
	if (getenv("AZ_CURSOR_STALE_XCURSOR") != NULL) {
		if (az_cursor.dbg_borrowed != NULL) {
			if (az_cursor.dbg_borrowed_generation != az_cursor_mgr_generation) {
				az_cursor.stale_xcursor++;
				wlr_log(WLR_ERROR, "cursor: borrowed xcursor from manager "
					"generation %" PRIu64 " used at generation %" PRIu64
					" -- this is the use-after-free M3.5E shipped",
					az_cursor.dbg_borrowed_generation,
					az_cursor_mgr_generation);
				/* Deliberately do NOT dereference it. The point is to prove
				 * the lifetime is invalid, not to crash the test host. */
				return NULL;
			}
			return az_cursor.dbg_borrowed;
		}
	}

	struct wlr_xcursor *xc = wlr_xcursor_manager_get_xcursor(cursor_mgr,
		az_cursor.xcursor_name, az_cursor.xcursor_scale);
	if (xc == NULL) {
		xc = wlr_xcursor_manager_get_xcursor(cursor_mgr, "default",
			az_cursor.xcursor_scale);
	}
	if (xc != NULL && getenv("AZ_CURSOR_STALE_XCURSOR") != NULL) {
		az_cursor.dbg_borrowed = xc;
		az_cursor.dbg_borrowed_generation = az_cursor_mgr_generation;
	}
	return xc;
}

static int az_cursor_xcursor_tick(void *data) {
	(void)data;
	struct wlr_xcursor *xc = az_cursor_resolve_xcursor();
	if (xc == NULL || xc->image_count == 0) {
		return 0;
	}
	az_cursor.xcursor_frames++;
	az_cursor_xcursor_show((az_cursor.xcursor_index + 1) % xc->image_count);
	return 0;
}

static void az_cursor_xcursor_show(size_t index) {
	struct wlr_xcursor *xc = az_cursor_resolve_xcursor();
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
	/* An xcursor image's hotspot is in BUFFER pixels -- the theme loads a
	 * bigger image for a bigger scale and scales the hotspot with it -- so it
	 * has to come down to logical units here. */
	float scale = az_cursor.xcursor_scale > 0 ? az_cursor.xcursor_scale : 1.0f;
	az_cursor_set_image(buffer, (int)roundf(image->hotspot_x / scale),
		(int)roundf(image->hotspot_y / scale), scale, false);

	if (xc->image_count > 1 && image->delay > 0 &&
			az_cursor.xcursor_timer != NULL) {
		wl_event_source_timer_update(az_cursor.xcursor_timer, image->delay);
	}
}

static void az_cursor_drop_surface(void);

/* See az_cursor_set_xcursor(). */
static void az_cursor_forget_pushed(void) {
	az_cursor.image_pushed = false;
}

/*
 * Select a themed cursor by name. THE way to do it.
 *
 * wlr_cursor_set_xcursor() is not an alternative spelling of this, and calling
 * it from compositor code is a bug. The two differ in who owns the image:
 *
 *   this          one image, chosen at the SHARPEST scale in the layout, and
 *                 pushed with wlr_cursor_set_buffer(..., scale) so wlroots
 *                 divides it back down to a logical size that is the same on
 *                 every output.
 *   wlroots'      a PER-OUTPUT image at each output's NATIVE scale, handed to
 *                 wlr_output_cursor_set_buffer() 1:1 -- there is no scale
 *                 argument on that call.
 *
 * Both are self-consistent; mixing them is not. az_avk_emit_cursors() draws
 * az_cursor.buffer into a box sized from wlr_output_cursor's width/height, so
 * the moment something selects a cursor through wlroots the pixels come from
 * one owner and the geometry from the other. On a mixed-scale layout that is
 * visible immediately: with outputs at 1.5 and 1.0, this path yields 24
 * physical px on the 1.0 output and wlroots' yields ~28, so the cursor grows
 * -- while still showing the old shape, because the shape wlroots picked was
 * never given to az_cursor at all.
 *
 * Found on a real desktop: dragging a window showed no "grab" cursor, and
 * resizing one on the 1.0 output made the arrow bigger instead of becoming a
 * resize cursor. Seven call sites were doing this. It is invisible with a
 * hardware cursor plane, because there wlroots' own image is what reaches the
 * plane and asteroidz's never enters it -- which is why the daily driver
 * never showed it.
 */
static void az_cursor_set_xcursor(const char *name) {
	if (cursor_mgr == NULL || name == NULL) {
		return;
	}
	/*
	 * BREAK SWITCH: hand the selection to wlroots, as the seven call sites
	 * that bypassed this function used to. One place rather than seven,
	 * because what is being restored is the OWNERSHIP MODEL, not a particular
	 * caller -- and a break that has to be spelled out seven times is a break
	 * that will be half-removed later.
	 */
	const char *wlr_break = getenv("AZ_CURSOR_WLROOTS_XCURSOR");
	if (wlr_break != NULL) {
		/*
		 * "moveresize" restores the SHIPPED defect exactly: only the
		 * compositor's own move/resize shapes went to wlroots, while client
		 * shapes and motion kept coming through here. That mixture is the
		 * interesting one -- az_cursor still holds a valid image, so AVK
		 * still composites, and the disagreement shows up as a box that does
		 * not fit the image rather than as no cursor at all.
		 *
		 * Without the qualifier every selection goes to wlroots, az_cursor
		 * ends up with no image, and cursor_no_image fires instead. That is a
		 * louder failure but a different one, and it leaves
		 * cursor_geometry_mismatch untested -- which is why both exist.
		 */
		bool only_moveresize = strcmp(wlr_break, "moveresize") == 0;
		bool is_moveresize = strcmp(name, "grab") == 0 ||
			strcmp(name, "grabbing") == 0 || strstr(name, "-resize") != NULL;
		if (!only_moveresize || is_moveresize) {
			wlr_cursor_set_xcursor(cursor, cursor_mgr, name);
			return;
		}
	}
	az_cursor_drop_surface();

	float scale = az_cursor_target_scale();
	wlr_xcursor_manager_load(cursor_mgr, scale);
	(void)0;
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

	/*
	 * Re-selecting the same cursor at the same scale must not restart its
	 * animation: pointer motion re-asserts the cursor constantly.
	 *
	 * `image_pushed` is the part that is easy to leave out and expensive to
	 * leave out. Hiding the cursor -- the idle timeout, or a client asking for
	 * none -- clears the image from wlroots but not the record of WHICH
	 * xcursor we last chose. Without the extra term, the first thing to ask
	 * for that same cursor again matches the guard, returns, and the pointer
	 * never comes back. handlecursoractivity() does exactly that: it replays
	 * `last_cursor.shape`, which is usually the shape that was showing when
	 * the cursor was hidden.
	 */
	/* BREAK SWITCH: drop the image_pushed term, restoring the trap. */
	bool pushed = az_cursor.image_pushed ||
		getenv("AZ_CURSOR_NO_PUSH_CHECK") != NULL;
	/*
	 * Compared by NAME, not by pointer. The pointer test that used to be here
	 * was wrong twice over: it kept a theme-owned pointer alive as durable
	 * state, and after a manager rebuild the allocator can hand the same
	 * address back for a different cursor, so it could match on identity that
	 * no longer means anything.
	 */
	if (pushed && az_cursor.source == AZ_CURSOR_SOURCE_XCURSOR
			&& az_cursor.xcursor_name != NULL
			&& strcmp(az_cursor.xcursor_name, name) == 0
			&& az_cursor.xcursor_scale == scale) {
		return;
	}

	az_cursor.sets_xcursor++;
	az_cursor.source = AZ_CURSOR_SOURCE_XCURSOR;
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

	/* A client states its hotspot in surface-local coordinates, which are
	 * already the logical units wanted here -- so it passes through untouched.
	 * The scale is reported separately, and is what turns the buffer's size
	 * into the cursor's size. */
	float scale = surface->current.scale > 0 ? (float)surface->current.scale
		: 1.0f;
	az_cursor_set_image(buffer, az_cursor.surface_hotspot_x,
		az_cursor.surface_hotspot_y, scale, content_changed);

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
		/* The client asking for NO pointer image. Distinct from the
		 * compositor hiding it on an idle timeout: this is a request, it
		 * persists, and only another set_cursor undoes it. */
		az_cursor.unsets++;
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

	az_cursor.sets_client++;
	az_cursor.source = AZ_CURSOR_SOURCE_CLIENT;
	az_cursor.surface_hotspot_x = hotspot_x;
	az_cursor.surface_hotspot_y = hotspot_y;
	free(az_cursor.xcursor_name);
	az_cursor.xcursor_name = NULL;

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

/*
 * The xcursor manager was destroyed and rebuilt -- a new theme, a new size, or
 * just a live config apply.
 *
 * Two things have to happen and neither is optional. wlroots' image was
 * cleared by the caller, so something must put one back or the pointer stays
 * invisible until the next motion. And whatever is put back has to be resolved
 * from the NEW manager, because the old one's images are freed.
 *
 * What must NOT happen is a re-SELECTION. The cursor showing before a config
 * apply should still be showing after it: forcing "left_ptr" throws away the
 * client's or the compositor's current shape for no reason, which is what the
 * old wlr_cursor_set_xcursor(cursor, cursor_mgr, "left_ptr") here did. Nor is
 * a fresh selection needed to get correct pixels -- az_cursor_show() replays
 * the current source, and the xcursor branch re-resolves by name against the
 * new manager on its way through.
 */
static void az_cursor_show(void);

static void az_cursor_theme_replaced(void) {
	/* Without this the guard in az_cursor_set_xcursor() and the early-out in
	 * wlr_cursor_set_buffer() both think the image is already up. */
	az_cursor_forget_pushed();
	if (az_cursor.source == AZ_CURSOR_SOURCE_NONE) {
		az_cursor_set_xcursor("left_ptr");
		return;
	}
	az_cursor_show();
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
	free(az_cursor.xcursor_name);
	az_cursor.xcursor_name = NULL;   /* force reselection */
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
