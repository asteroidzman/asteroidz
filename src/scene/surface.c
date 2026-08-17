#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <wlr/types/wlr_alpha_modifier_v1.h>
#include <wlr/types/wlr_color_management_v1.h>
#include <wlr/types/wlr_color_representation_v1.h>
#include <wlr/types/wlr_compositor.h>
#include <scene/wlr_scene.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_linux_drm_syncobj_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/util/transform.h>
#include "scene/wlr_scene_internal.h"

/* scenefx (vendored from wlroots 0.20.2): compositors can register a
 * fallback that supplies an image description for surfaces that carry
 * none via wp-color-management -- e.g. clients declaring colorimetry
 * through frog-color-management. Consulted on every reconfigure, so it
 * survives the per-commit reset of the scene buffer's colorimetry. */
static const struct wlr_image_description_v1_data *(
	*scene_surface_color_fallback)(struct wlr_surface *surface) = NULL;

void wlr_scene_set_surface_color_description_fallback(
		const struct wlr_image_description_v1_data *(*cb)(
			struct wlr_surface *surface)) {
	scene_surface_color_fallback = cb;
}

static double get_surface_preferred_buffer_scale(struct wlr_surface *surface) {
	double scale = 1;
	struct wlr_surface_output *surface_output;
	wl_list_for_each(surface_output, &surface->current_outputs, link) {
		if (surface_output->output->scale > scale) {
			scale = surface_output->output->scale;
		}
	}
	return scale;
}

// Output used for frame pacing (surface frame callbacks, presentation
// time feedback, etc), may be NULL
static struct wlr_output *get_surface_frame_pacing_output(struct wlr_surface *surface) {
	struct wlr_output *frame_pacing_output = NULL;
	struct wlr_surface_output *surface_output;
	wl_list_for_each(surface_output, &surface->current_outputs, link) {
		if (!surface_output->suspended && (frame_pacing_output == NULL ||
				surface_output->output->refresh > frame_pacing_output->refresh)) {
			frame_pacing_output = surface_output->output;
		}
	}
	return frame_pacing_output;
}

static int get_tf_preference(enum wlr_color_transfer_function tf) {
	switch (tf) {
	case WLR_COLOR_TRANSFER_FUNCTION_GAMMA22:
		return 0;
	case WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ:
		return 1;
	case WLR_COLOR_TRANSFER_FUNCTION_BT1886:
	case WLR_COLOR_TRANSFER_FUNCTION_SRGB:
	case WLR_COLOR_TRANSFER_FUNCTION_EXT_LINEAR:
		return -1;
	}
	abort(); // unreachable
}

static int get_primaries_preference(enum wlr_color_named_primaries primaries) {
	switch (primaries) {
	case WLR_COLOR_NAMED_PRIMARIES_SRGB:
		return 0;
	case WLR_COLOR_NAMED_PRIMARIES_BT2020:
		return 1;
	}
	abort(); // unreachable
}

static void get_surface_preferred_image_description(struct wlr_surface *surface,
		struct wlr_image_description_v1_data *out) {
	struct wlr_output_image_description preferred = {
		.transfer_function = WLR_COLOR_TRANSFER_FUNCTION_GAMMA22,
		.primaries = WLR_COLOR_NAMED_PRIMARIES_SRGB,
	};

	struct wlr_surface_output *surface_output;
	wl_list_for_each(surface_output, &surface->current_outputs, link) {
		const struct wlr_output_image_description *img_desc =
			surface_output->output->image_description;
		if (img_desc == NULL) {
			continue;
		}
		if (get_tf_preference(preferred.transfer_function) < get_tf_preference(img_desc->transfer_function)) {
			preferred.transfer_function = img_desc->transfer_function;
		}
		if (get_primaries_preference(preferred.primaries) < get_primaries_preference(img_desc->primaries)) {
			preferred.primaries = img_desc->primaries;
		}
	}

	*out = (struct wlr_image_description_v1_data){
		.tf_named = wlr_color_manager_v1_transfer_function_from_wlr(preferred.transfer_function),
		.primaries_named = wlr_color_manager_v1_primaries_from_wlr(preferred.primaries),
	};
}

static void handle_scene_buffer_outputs_update(
		struct wl_listener *listener, void *data) {
	struct wlr_scene_surface *surface =
		wl_container_of(listener, surface, outputs_update);
	struct wlr_scene_outputs_update_event *event = data;
	struct wlr_scene *scene = scene_node_get_root(&surface->buffer->node);

	// If the surface is no longer visible on any output in the scene, keep the
	// last sent preferred configuration to avoid unnecessary redraws
	bool suspend = event->size == 0;

	// To avoid sending redundant leave/enter events when a surface is hidden and then shown
	// without moving to a different output the following policy is implemented:
	//
	// 1. When a surface transitions from being visible on >0 outputs to being visible on 0 outputs
	//    don't send any leave events.
	//
	// 2. When a surface transitions from being visible on 0 outputs to being visible on >0 outputs
	//    send leave events for all entered outputs on which the surface is no longer visible as
	//    well as enter events for any outputs not already entered.
	struct wlr_surface_output *entered_output, *tmp;
	wl_list_for_each_safe(entered_output, tmp, &surface->surface->current_outputs, link) {
		bool active = false;
		for (size_t i = 0; i < event->size; i++) {
			if (entered_output->output == event->active[i]->output) {
				active = true;
				break;
			}
		}

		struct wlr_scene_output *scene_output;
		wl_list_for_each(scene_output, &scene->outputs, link) {
			if (scene_output->output == entered_output->output) {
				entered_output->suspended = suspend;
				if (!suspend && !active) {
					wlr_surface_send_leave(surface->surface, entered_output->output);
				}
				break;
			}
		}
	}

	// No reason to update the preferred configuration if we aren't sending leave/enter events.
	if (suspend) {
		return;
	}

	for (size_t i = 0; i < event->size; i++) {
		// This function internally checks if an enter event was already sent for the output
		// to avoid sending redundant events.
		wlr_surface_send_enter(surface->surface, event->active[i]->output);
	}

	double scale = get_surface_preferred_buffer_scale(surface->surface);
	wlr_fractional_scale_v1_notify_scale(surface->surface, scale);
	wlr_surface_set_preferred_buffer_scale(surface->surface, ceil(scale));

	if (scene->color_manager_v1 != NULL) {
		struct wlr_image_description_v1_data img_desc = {0};
		get_surface_preferred_image_description(surface->surface, &img_desc);
		wlr_color_manager_v1_set_surface_preferred_image_description(scene->color_manager_v1,
			surface->surface, &img_desc);
	}
}

static void handle_scene_buffer_output_sample(
		struct wl_listener *listener, void *data) {
	struct wlr_scene_surface *surface =
		wl_container_of(listener, surface, output_sample);
	const struct wlr_scene_output_sample_event *event = data;
	struct wlr_output *output = event->output->output;
	if (get_surface_frame_pacing_output(surface->surface) != output) {
		return;
	}

	if (event->direct_scanout) {
		wlr_presentation_surface_scanned_out_on_output(surface->surface, output);
	} else {
		wlr_presentation_surface_textured_on_output(surface->surface, output);
	}

	struct wlr_linux_drm_syncobj_surface_v1_state *syncobj_surface_state =
		wlr_linux_drm_syncobj_v1_get_surface_state(surface->surface);
	if (syncobj_surface_state != NULL && event->release_timeline != NULL) {
		wlr_linux_drm_syncobj_v1_state_add_release_point(syncobj_surface_state,
			event->release_timeline, event->release_point, output->event_loop);
	}
}

static void handle_scene_buffer_frame_done(
		struct wl_listener *listener, void *data) {
	struct wlr_scene_surface *surface =
		wl_container_of(listener, surface, frame_done);
	struct wlr_scene_frame_done_event *event = data;
	if (get_surface_frame_pacing_output(surface->surface) != event->output->output) {
		return;
	}

	wlr_surface_send_frame_done(surface->surface, &event->when);
}

void wlr_scene_surface_send_frame_done(struct wlr_scene_surface *scene_surface,
		const struct timespec *when) {
	if (!pixman_region32_empty(&scene_surface->buffer->node.visible)) {
		wlr_surface_send_frame_done(scene_surface->surface, when);
	}
}

static void scene_surface_handle_surface_destroy(
		struct wl_listener *listener, void *data) {
	struct wlr_scene_surface *surface =
		wl_container_of(listener, surface, surface_destroy);

	wlr_scene_node_destroy(&surface->buffer->node);
}

// This is used for wlr_scene where it unconditionally locks buffers preventing
// reuse of the existing texture for shm clients. With the usage pattern of
// wlr_scene surface handling, we can mark its locked buffer as safe
// for mutation.
static void client_buffer_mark_next_can_damage(struct wlr_client_buffer *buffer) {
	buffer->n_ignore_locks++;
}

static void scene_buffer_unmark_client_buffer(struct wlr_scene_buffer *scene_buffer) {
	if (!scene_buffer->buffer) {
		return;
	}

	struct wlr_client_buffer *buffer = wlr_client_buffer_get(scene_buffer->buffer);
	if (!buffer) {
		return;
	}

	// If the buffer was a single-pixel buffer where we cached its color
	// then it won't have been marked as damage-allowed.
	if (buffer->n_ignore_locks > 0) {
		buffer->n_ignore_locks--;
	}
}

static int min(int a, int b) {
	return a < b ? a : b;
}

// ASTEROIDZ FORK ADDITION, used by the view-scale change below. Divides every
// rectangle in `region` by the given factors, rounding each edge INWARD so the
// result is never larger than the truth -- an opaque region is a promise that
// nothing behind it needs drawing, and over-promising is what leaves holes.
static void region_scale_down(pixman_region32_t *region, float sx, float sy) {
	int nrects = 0;
	const pixman_box32_t *rects = pixman_region32_rectangles(region, &nrects);

	pixman_region32_t scaled;
	pixman_region32_init(&scaled);
	for (int i = 0; i < nrects; i++) {
		int x1 = (int)ceilf((float)rects[i].x1 / sx);
		int y1 = (int)ceilf((float)rects[i].y1 / sy);
		int x2 = (int)floorf((float)rects[i].x2 / sx);
		int y2 = (int)floorf((float)rects[i].y2 / sy);
		if (x2 > x1 && y2 > y1) {
			pixman_region32_union_rect(&scaled, &scaled, x1, y1,
				x2 - x1, y2 - y1);
		}
	}
	pixman_region32_copy(region, &scaled);
	pixman_region32_fini(&scaled);
}

static void surface_reconfigure(struct wlr_scene_surface *scene_surface) {
	struct wlr_scene_buffer *scene_buffer = scene_surface->buffer;
	struct wlr_surface *surface = scene_surface->surface;
	struct wlr_surface_state *state = &surface->current;

	struct wlr_fbox src_box;
	wlr_surface_get_buffer_source_box(surface, &src_box);

	pixman_region32_t opaque;
	pixman_region32_init(&opaque);
	pixman_region32_copy(&opaque, &surface->opaque_region);

	int width = state->width;
	int height = state->height;

	if (!wlr_box_empty(&scene_surface->clip)) {
		struct wlr_box *clip = &scene_surface->clip;

		int buffer_width = state->buffer_width;
		int buffer_height = state->buffer_height;
		width = min(clip->width, width - clip->x);
		height = min(clip->height, height - clip->y);

		wlr_fbox_transform(&src_box, &src_box, state->transform,
			buffer_width, buffer_height);
		wlr_output_transform_coords(state->transform, &buffer_width, &buffer_height);

		src_box.x += (double)(clip->x * src_box.width) / state->width;
		src_box.y += (double)(clip->y * src_box.height) / state->height;
		src_box.width *= (double)width / state->width;
		src_box.height *= (double)height / state->height;

		wlr_fbox_transform(&src_box, &src_box, wlr_output_transform_invert(state->transform),
			buffer_width, buffer_height);

		pixman_region32_translate(&opaque, -clip->x, -clip->y);
		pixman_region32_intersect_rect(&opaque, &opaque, 0, 0, width, height);
	}

	if (width <= 0 || height <= 0) {
		wlr_scene_buffer_set_buffer(scene_buffer, NULL);
		pixman_region32_fini(&opaque);
		return;
	}

	float opacity = 1.0;
	const struct wlr_alpha_modifier_surface_v1_state *alpha_modifier_state =
		wlr_alpha_modifier_v1_get_surface_state(surface);
	if (alpha_modifier_state != NULL) {
		opacity = (float)alpha_modifier_state->multiplier;
	}

	enum wlr_color_transfer_function tf = WLR_COLOR_TRANSFER_FUNCTION_GAMMA22;
	enum wlr_color_named_primaries primaries = WLR_COLOR_NAMED_PRIMARIES_SRGB;
	const struct wlr_image_description_v1_data *img_desc =
		wlr_surface_get_image_description_v1_data(surface);
	if (img_desc == NULL && scene_surface_color_fallback != NULL) {
		img_desc = scene_surface_color_fallback(surface);
	}
	// max_cll is a scenefx-specific addition on top of this otherwise exact
	// wlroots 0.20.2 copy of surface_reconfigure: forwarded to the scene
	// buffer for the composited-render tone-mapping rolloff (see
	// wlr_scene.c's content_peak), 0 (unset) for anything that isn't
	// declaring PQ colorimetry.
	uint32_t max_cll = 0;
	if (img_desc != NULL) {
		tf = wlr_color_manager_v1_transfer_function_to_wlr(img_desc->tf_named);
		primaries = wlr_color_manager_v1_primaries_to_wlr(img_desc->primaries_named);
		if (tf == WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ) {
			max_cll = img_desc->max_cll;
		}
	}

	enum wlr_color_encoding color_encoding = WLR_COLOR_ENCODING_NONE;
	enum wlr_color_range color_range = WLR_COLOR_RANGE_NONE;
	const struct wlr_color_representation_v1_surface_state *color_repr =
		wlr_color_representation_v1_get_surface_state(surface);
	if (color_repr != NULL) {
		if (color_repr->coefficients != 0) {
			color_encoding = wlr_color_representation_v1_color_encoding_to_wlr(color_repr->coefficients);
		}
		if (color_repr->range != 0) {
			color_range = wlr_color_representation_v1_color_range_to_wlr(color_repr->range);
		}
	}

	// ── ASTEROIDZ FORK CHANGE 1 of 2 ──────────────────────────────────────
	//
	// The destination size is re-asserted on EVERY commit, from the surface's
	// own size. That is right for a Wayland client, which negotiates its
	// buffer scale with the compositor and therefore commits a buffer that
	// already means the right number of logical pixels.
	//
	// An X11 client cannot do that -- there is no protocol for it to be told
	// an output's fractional scale -- so asteroidz asks the X window for the
	// output's raw pixel count instead and presents it across fewer logical
	// pixels. The compositor cannot express that by calling
	// wlr_scene_buffer_set_dest_size() itself: this line would overwrite it on
	// the client's very next commit, which for a game is 60 times a second.
	// So the ratio has to live where the recomputation happens.
	//
	// A scale of 0 (never set) or 1 leaves the arithmetic exactly as it was.
	int dest_width = width, dest_height = height;
	if (scene_surface->view_scale > 0.0f && scene_surface->view_scale != 1.0f) {
		dest_width = (int)lroundf((float)width / scene_surface->view_scale);
		dest_height = (int)lroundf((float)height / scene_surface->view_scale);
		// A surface narrower than the scale would otherwise round to zero,
		// which wlr_scene_buffer_set_dest_size treats as "use the buffer
		// size" and would silently undo the whole thing.
		if (dest_width < 1) {
			dest_width = 1;
		}
		if (dest_height < 1) {
			dest_height = 1;
		}
	}

	// The opaque region is carried in the NODE's coordinates, and the node
	// just shrank. Left alone, a surface's partial opaque region would be
	// intersected against a smaller node and would claim a larger fraction of
	// it than the client declared -- 50% of a 1920-wide surface read as 62.5%
	// of a 1536-wide node -- which is the direction that produces artifacts:
	// the scene skips drawing what it believes is hidden. Shrinking each
	// rectangle inward is conservative in the safe direction.
	if (dest_width != width || dest_height != height) {
		region_scale_down(&opaque, (float)width / (float)dest_width,
			(float)height / (float)dest_height);
	}

	wlr_scene_buffer_set_opaque_region(scene_buffer, &opaque);
	wlr_scene_buffer_set_source_box(scene_buffer, &src_box);
	wlr_scene_buffer_set_dest_size(scene_buffer, dest_width, dest_height);
	wlr_scene_buffer_set_transform(scene_buffer, state->transform);
	wlr_scene_buffer_set_opacity(scene_buffer, opacity);
	wlr_scene_buffer_set_transfer_function(scene_buffer, tf);
	wlr_scene_buffer_set_primaries(scene_buffer, primaries);
	wlr_scene_buffer_set_color_encoding(scene_buffer, color_encoding);
	wlr_scene_buffer_set_color_range(scene_buffer, color_range);
	wlr_scene_buffer_set_max_cll(scene_buffer, max_cll);

	scene_buffer_unmark_client_buffer(scene_buffer);

	// Which buffer the scene draws.
	//
	// With a wlr_renderer on the compositor, wlroots wraps every client buffer
	// in a wlr_client_buffer so it can upload the pixels into a wlr_texture,
	// and that wrapper is what the scene has always been given.
	//
	// A compositor created with a NULL renderer -- which is what asteroidz
	// does when its own Vulkan engine composites, see
	// docs/vulkan-native-architecture.md -- uploads nothing, so surface->buffer
	// stays NULL forever. The client's buffer is still right there in the
	// committed state, and on that path wlroots leaves it LOCKED until the next
	// commit replaces it, so handing it straight to the scene is both possible
	// and correct.
	//
	// This is not a fallback for a missing texture. It is the absence of an
	// upload that never needed to happen: a renderer that imports dma-bufs
	// itself has no use for a copy made by a different renderer, and going
	// through the wrapper actively loses information -- once it has uploaded,
	// the wrapper may release the original, after which it can report neither
	// a dma-buf nor readable pixels to anyone else.
	struct wlr_buffer *committed = surface->buffer != NULL
		? &surface->buffer->base : surface->current.buffer;
	// The client's own buffer, whichever route it arrived by. Used for the
	// questions that are about the buffer itself rather than the wrapper.
	struct wlr_buffer *source = surface->buffer != NULL
		? surface->buffer->source : surface->current.buffer;

	if (committed) {
		// If we've cached the buffer's single-pixel buffer color
		// then any in-place updates to the texture wouldn't be
		// reflected in rendering. So only allow in-place texture
		// updates if it's not a single pixel buffer.  Note that we
		// can't use the cached scene_buffer->is_single_pixel_buffer
		// because that's only set later on.
		bool is_single_pixel_buffer = false;
		if (source != NULL) {
			struct wlr_single_pixel_buffer_v1 *spb =
				wlr_single_pixel_buffer_v1_try_from_buffer(source);
			is_single_pixel_buffer = spb != NULL;
		}
		// Only meaningful for a wrapper: it marks the wrapper's texture as
		// updatable in place. With no wrapper there is no texture to update.
		if (!is_single_pixel_buffer && surface->buffer != NULL) {
			client_buffer_mark_next_can_damage(surface->buffer);
		}

		struct wlr_linux_drm_syncobj_surface_v1_state *syncobj_surface_state =
			wlr_linux_drm_syncobj_v1_get_surface_state(surface);

		struct wlr_scene_buffer_set_buffer_options options = {
			.damage = &surface->buffer_damage,
		};
		if (syncobj_surface_state != NULL) {
			options.wait_timeline = syncobj_surface_state->acquire_timeline;
			options.wait_point = syncobj_surface_state->acquire_point;
		}
		wlr_scene_buffer_set_buffer_with_options(scene_buffer, committed,
			&options);
	} else if (!wlr_surface_has_buffer(surface)) {
		// Genuinely unmapped: the surface committed a NULL buffer.
		wlr_scene_buffer_set_buffer(scene_buffer, NULL);
	}
	// Otherwise the surface HAS content and we simply cannot see it from here.
	//
	// wlr_surface.current.buffer is live only for the duration of the commit
	// event -- wlroots unlocks it as soon as every listener has run -- but
	// surface_reconfigure() is also called from scene_surface_create() and
	// from scene_surface_set_clip(), neither of which is a commit. Clearing
	// the node in those cases unmaps a window that is perfectly mapped, and
	// with the raw-buffer path that meant every clip change blanked its
	// surface. The scene already holds its own lock on the buffer it was
	// last given; leaving it alone is both correct and what the wrapper
	// path did implicitly, since surface->buffer persisted between commits.

	pixman_region32_fini(&opaque);
}

static void handle_scene_surface_surface_commit(
		struct wl_listener *listener, void *data) {
	struct wlr_scene_surface *surface =
		wl_container_of(listener, surface, surface_commit);
	struct wlr_scene_buffer *scene_buffer = surface->buffer;

	surface_reconfigure(surface);

	// If the surface has requested a frame done event, honour that. The
	// frame_callback_list will be populated in this case. We should only
	// schedule the frame however if the node is enabled and there is an
	// output intersecting, otherwise the frame done events would never reach
	// the surface anyway.
	int lx, ly;
	bool enabled = wlr_scene_node_coords(&scene_buffer->node, &lx, &ly);
	struct wlr_output *output = get_surface_frame_pacing_output(surface->surface);
	if (!wl_list_empty(&surface->surface->current.frame_callback_list) && output && enabled) {
		wlr_output_schedule_frame(output);
	}
}

static bool scene_buffer_point_accepts_input(struct wlr_scene_buffer *scene_buffer,
		double *sx, double *sy) {
	struct wlr_scene_surface *scene_surface =
		wlr_scene_surface_try_from_buffer(scene_buffer);

	// ── ASTEROIDZ FORK CHANGE 2 of 2 ──────────────────────────────────────
	//
	// sx/sy arrive in NODE-LOCAL coordinates and are used as SURFACE
	// coordinates: for the hit test here, and -- because wlr_scene_node_at()
	// hands back whatever this function leaves behind -- as the surface-local
	// position the compositor then sends to the client. Everywhere else in
	// wlroots those two spaces are the same thing, so nothing converts
	// between them.
	//
	// The view scale is the one case where they are not. A surface presented
	// at 1/1.25 of its size has node-local coordinates 1.25x smaller than its
	// own, so without this every click inside an X11 window on a 1.25x output
	// lands 1.25x short of where it was aimed -- and it lands there
	// SILENTLY. The picture is pixel-perfect; only the pointer is wrong,
	// increasingly so towards the bottom right, which reads as a broken
	// application rather than as a compositor bug.
	//
	// Written from view_scale rather than from the dst_width/state.width
	// ratio on purpose. That ratio is also non-1 while an open or close
	// animation is scaling a window, and converting there would change the
	// behaviour of every animated client on both renderers -- a much larger
	// change than this is, for a case where the window is moving under the
	// pointer anyway.
	//
	// BREAK: AZ_BREAK_X11_INPUT_SCALE=1 drops the conversion and nothing
	// else. It is the shape of the bug this is here to prevent -- a perfect
	// picture with every click in the wrong place -- and it is what
	// contrib/xw-scale-test.sh's click assertions are falsified against.
	static int break_input_scale = -1;
	if (break_input_scale < 0) {
		const char *e = getenv("AZ_BREAK_X11_INPUT_SCALE");
		break_input_scale = e && *e && *e != '0';
	}
	if (!break_input_scale && scene_surface->view_scale > 0.0f &&
			scene_surface->view_scale != 1.0f) {
		*sx *= scene_surface->view_scale;
		*sy *= scene_surface->view_scale;
	}

	*sx += scene_surface->clip.x;
	*sy += scene_surface->clip.y;

	return wlr_surface_point_accepts_input(scene_surface->surface, *sx, *sy);
}

static void surface_addon_destroy(struct wlr_addon *addon) {
	struct wlr_scene_surface *surface = wl_container_of(addon, surface, addon);

	scene_buffer_unmark_client_buffer(surface->buffer);

	wlr_addon_finish(&surface->addon);

	wl_list_remove(&surface->outputs_update.link);
	wl_list_remove(&surface->output_sample.link);
	wl_list_remove(&surface->frame_done.link);
	wl_list_remove(&surface->surface_destroy.link);
	wl_list_remove(&surface->surface_commit.link);

	free(surface);
}

static const struct wlr_addon_interface surface_addon_impl = {
	.name = "wlr_scene_surface",
	.destroy = surface_addon_destroy,
};

struct wlr_scene_surface *wlr_scene_surface_try_from_buffer(
		struct wlr_scene_buffer *scene_buffer) {
	struct wlr_addon *addon = wlr_addon_find(&scene_buffer->node.addons,
		scene_buffer, &surface_addon_impl);
	if (!addon) {
		return NULL;
	}

	struct wlr_scene_surface *surface = wl_container_of(addon, surface, addon);
	return surface;
}

struct wlr_scene_surface *wlr_scene_surface_create(struct wlr_scene_tree *parent,
		struct wlr_surface *wlr_surface) {
	struct wlr_scene_surface *surface = calloc(1, sizeof(*surface));
	if (surface == NULL) {
		return NULL;
	}

	struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_create(parent, NULL);
	if (!scene_buffer) {
		free(surface);
		return NULL;
	}

	surface->buffer = scene_buffer;
	surface->surface = wlr_surface;
	// ASTEROIDZ FORK ADDITION. Explicit rather than left at calloc's 0, which
	// the arithmetic treats as 1 anyway -- but a scale field that reads as
	// zero is the kind of thing a later division gets wrong once.
	surface->view_scale = 1.0f;
	scene_buffer->point_accepts_input = scene_buffer_point_accepts_input;

	surface->outputs_update.notify = handle_scene_buffer_outputs_update;
	wl_signal_add(&scene_buffer->events.outputs_update, &surface->outputs_update);

	surface->output_sample.notify = handle_scene_buffer_output_sample;
	wl_signal_add(&scene_buffer->events.output_sample, &surface->output_sample);

	surface->frame_done.notify = handle_scene_buffer_frame_done;
	wl_signal_add(&scene_buffer->events.frame_done, &surface->frame_done);

	surface->surface_destroy.notify = scene_surface_handle_surface_destroy;
	wl_signal_add(&wlr_surface->events.destroy, &surface->surface_destroy);

	surface->surface_commit.notify = handle_scene_surface_surface_commit;
	wl_signal_add(&wlr_surface->events.commit, &surface->surface_commit);

	wlr_addon_init(&surface->addon, &scene_buffer->node.addons,
		scene_buffer, &surface_addon_impl);

	surface_reconfigure(surface);

	return surface;
}

// ASTEROIDZ FORK ADDITION. See the field's documentation in wlr_scene.h.
void wlr_scene_surface_set_view_scale(struct wlr_scene_surface *surface,
		float scale) {
	if (scale <= 0.0f) {
		scale = 1.0f;
	}
	if (surface->view_scale == scale) {
		return;
	}

	surface->view_scale = scale;
	surface_reconfigure(surface);
}

void scene_surface_set_clip(struct wlr_scene_surface *surface, struct wlr_box *clip) {
	if (wlr_box_equal(clip, &surface->clip)) {
		return;
	}

	if (clip) {
		surface->clip = *clip;
	} else {
		surface->clip = (struct wlr_box){0};
	}

	surface_reconfigure(surface);
}
