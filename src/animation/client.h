#include "wlr/util/log.h"
#include "asteroid-break.h"
void client_actual_size(Client *c, int32_t *width, int32_t *height) {
	*width = c->animation.current.width - 2 * (int32_t)c->bw;

	*height = c->animation.current.height - 2 * (int32_t)c->bw;
}

void set_rect_size(struct wlr_scene_rect *rect, int32_t width, int32_t height) {
	wlr_scene_rect_set_size(rect, GEZERO(width), GEZERO(height));
}

enum corner_location set_client_corner_location(Client *c) {
	enum corner_location current_corner_location = CORNER_LOCATION_ALL;
	bool ov = c->mon && c->mon->isoverview;
	struct wlr_box target_geom =
		config.animations ? c->animation.current : c->geom;
	/* In overview every window is a discrete rounded tile in the OV desktop, so
	 * round all four corners -- the edge-vs-screen logic below would wrongly
	 * square the bottom/sides of the scaled, gapped windows. The titlebar block
	 * still runs, so the top-left is squared to blend with the tab when one is
	 * shown (that's the only corner a titlebar should square). */
	if (!ov) {
		/* a corner is squared off only where the window meets the screen edge */
		int32_t bnd_x = c->mon->m.x, bnd_y = c->mon->m.y;
		int32_t bnd_r = c->mon->m.x + c->mon->m.width;
		int32_t bnd_b = c->mon->m.y + c->mon->m.height;
		if (target_geom.x + config.border_radius <= bnd_x) {
			current_corner_location &= ~CORNER_LOCATION_LEFT;
		}
		if (target_geom.x + target_geom.width - config.border_radius >= bnd_r) {
			current_corner_location &= ~CORNER_LOCATION_RIGHT;
		}
		if (target_geom.y + config.border_radius <= bnd_y) {
			current_corner_location &= ~CORNER_LOCATION_TOP;
		}
		if (target_geom.y + target_geom.height - config.border_radius >= bnd_b) {
			current_corner_location &= ~CORNER_LOCATION_BOTTOM;
		}
	}
	/* the titlebar's close button (leftmost) owns the rounded top-left
	 * corner; square off the window's own top-left corner so the two
	 * pieces read as one shape. The title tab no longer reaches the
	 * window's right edge, so the top-right corner stays rounded. In
	 * monocle with more than one window, the titlebar row is left-aligned:
	 * it only spans the full width (and so only squares both top corners)
	 * when segments aren't capped by monocle_tab_max_width. */
	/* segment-row membership is independent of decoration mode: the shared
	 * monocle strip is layout furniture drawn flush above EVERY fake-tiled
	 * window (incl. CSD ones), so those windows must square against it; the
	 * per-window tab, in contrast, only exists for server-decorated windows */
	bool seg_row = !ov && is_monocle_layout(c->mon) &&
				   c->mon->visible_fake_tiling_clients > 1 && ISFAKETILED(c);
	if ((config.enable_titlebar || is_monocle_layout(c->mon)) &&
		c->titlebar_node && !c->isfullscreen &&
		(seg_row ||
		 (client_wants_ssd(c) && !client_no_titlebar(c) &&
		  (ISFAKETILED(c) || c->isfloating)))) {
		bool monocle_row_full_width = false;
		if (is_monocle_layout(c->mon) && c->mon->visible_fake_tiling_clients > 1) {
			int32_t n = c->mon->visible_fake_tiling_clients;
			int32_t cur_gappoh = enablegaps ? c->mon->gappoh : 0;
			int32_t cur_gapih = enablegaps ? c->mon->gappih : 0;
			int32_t tab_area_width = c->mon->w.width - 2 * cur_gappoh;
			int32_t total_gaps = (n - 1) * cur_gapih;
			int32_t base_width = (tab_area_width - total_gaps) / n;
			monocle_row_full_width = !(config.monocle_tab_max_width > 0 &&
									  base_width > config.monocle_tab_max_width);
		}
		if (seg_row && monocle_row_full_width) {
			current_corner_location &= ~CORNER_LOCATION_TOP;
		} else {
			/* overview tiles (and non-full-width monocle) use a per-window tab
			 * that owns only the rounded top-left */
			current_corner_location &= ~CORNER_LOCATION_TOP_LEFT;
		}
	}
	return current_corner_location;
}

/* master/stack proportion-group layouts (tile, deck, center_tile,
 * right_tile) were removed; dwindle/monocle/scroller each manage their own
 * per-node geometry instead of a shared master/stack split. */
bool is_horizontal_stack_layout(Monitor *m) {
	(void)m;
	return false;
}

bool is_horizontal_right_stack_layout(Monitor *m) {
	(void)m;
	return false;
}

int32_t is_special_animation_rule(Client *c) {

	if (is_scroller_layout(c->mon) && !c->isfloating) {
		return DOWN;
	} else if (c->mon->visible_tiling_clients == 1 && !c->isfloating) {
		return DOWN;
	} else if (c->mon->visible_tiling_clients == 2 && !c->isfloating &&
			   !config.new_is_master && is_horizontal_stack_layout(c->mon)) {
		return RIGHT;
	} else if (!c->isfloating && config.new_is_master &&
			   is_horizontal_stack_layout(c->mon)) {
		return LEFT;
	} else if (c->mon->visible_tiling_clients == 2 && !c->isfloating &&
			   !config.new_is_master &&
			   is_horizontal_right_stack_layout(c->mon)) {
		return LEFT;
	} else if (!c->isfloating && config.new_is_master &&
			   is_horizontal_right_stack_layout(c->mon)) {
		return RIGHT;
	} else {
		return UNDIR;
	}
}

void set_overview_enter_animation(Client *c) {
	struct wlr_box geo = c->geom;
	c->animainit_geom.width = geo.width * 1.2;
	c->animainit_geom.height = geo.height * 1.2;
	c->animainit_geom.x = geo.x + (geo.width - c->animainit_geom.width) / 2;
	c->animainit_geom.y = geo.y + (geo.height - c->animainit_geom.height) / 2;
}

void set_client_open_animation(Client *c, struct wlr_box geo) {
	int32_t slide_direction;
	int32_t horizontal, horizontal_value;
	int32_t vertical, vertical_value;
	int32_t special_direction;
	int32_t center_x, center_y;

	if ((!c->animation_type_open &&
		 strcmp(config.animation_type_open, "fade") == 0) ||
		(c->animation_type_open &&
		 strcmp(c->animation_type_open, "fade") == 0)) {
		c->animainit_geom.width = geo.width;
		c->animainit_geom.height = geo.height;
		c->animainit_geom.x = geo.x;
		c->animainit_geom.y = geo.y;
		return;
	} else if ((!c->animation_type_open &&
				strcmp(config.animation_type_open, "zoom") == 0) ||
			   (c->animation_type_open &&
				strcmp(c->animation_type_open, "zoom") == 0)) {
		c->animainit_geom.width = geo.width * config.zoom_initial_ratio;
		c->animainit_geom.height = geo.height * config.zoom_initial_ratio;
		c->animainit_geom.x = geo.x + (geo.width - c->animainit_geom.width) / 2;
		c->animainit_geom.y =
			geo.y + (geo.height - c->animainit_geom.height) / 2;
		return;
	} else {
		special_direction = is_special_animation_rule(c);
		center_x = c->geom.x + c->geom.width / 2;
		center_y = c->geom.y + c->geom.height / 2;
		if (special_direction == UNDIR) {
			horizontal = c->mon->w.x + c->mon->w.width - center_x <
								 center_x - c->mon->w.x
							 ? RIGHT
							 : LEFT;
			horizontal_value = horizontal == LEFT
								   ? center_x - c->mon->w.x
								   : c->mon->w.x + c->mon->w.width - center_x;
			vertical = c->mon->w.y + c->mon->w.height - center_y <
							   center_y - c->mon->w.y
						   ? DOWN
						   : UP;
			vertical_value = vertical == UP
								 ? center_y - c->mon->w.y
								 : c->mon->w.y + c->mon->w.height - center_y;
			slide_direction =
				horizontal_value < vertical_value ? horizontal : vertical;
		} else {
			slide_direction = special_direction;
		}
		c->animainit_geom.width = c->geom.width;
		c->animainit_geom.height = c->geom.height;
		switch (slide_direction) {
		case UP:
			c->animainit_geom.x = c->geom.x;
			c->animainit_geom.y = c->mon->m.y - c->geom.height;
			break;
		case DOWN:
			c->animainit_geom.x = c->geom.x;
			c->animainit_geom.y =
				c->geom.y + c->mon->m.height - (c->geom.y - c->mon->m.y);
			break;
		case LEFT:
			c->animainit_geom.x = c->mon->m.x - c->geom.width;
			c->animainit_geom.y = c->geom.y;
			break;
		case RIGHT:
			c->animainit_geom.x =
				c->geom.x + c->mon->m.width - (c->geom.x - c->mon->m.x);
			c->animainit_geom.y = c->geom.y;
			break;
		default:
			c->animainit_geom.x = c->geom.x;
			c->animainit_geom.y = 0 - c->geom.height;
		}
	}
}

void snap_scene_buffer_apply_effect(struct wlr_scene_buffer *buffer, int32_t sx,
									int32_t sy, void *data) {
	BufferData *buffer_data = (BufferData *)data;
	wlr_scene_buffer_set_dest_size(buffer, buffer_data->width,
								   buffer_data->height);
}

void scene_buffer_apply_effect(struct wlr_scene_buffer *buffer, int32_t sx,
							   int32_t sy, void *data) {
	BufferData *buffer_data = (BufferData *)data;

	/* Normally the surface is resized to match its geometry, so scaling it
	 * DOWN would be wrong -- these guards disable scaling when shrinking.
	 * In the overview live thumbnail the surface keeps its full size and must
	 * actually be shrunk into its cell, so skip the guards there. */
	if (!buffer_data->ov_live) {
		if (buffer_data->should_scale && buffer_data->height_scale < 1 &&
			buffer_data->width_scale < 1) {
			buffer_data->should_scale = false;
		}

		if (buffer_data->should_scale && buffer_data->height_scale == 1 &&
			buffer_data->width_scale < 1) {
			buffer_data->should_scale = false;
		}

		if (buffer_data->should_scale && buffer_data->height_scale < 1 &&
			buffer_data->width_scale == 1) {
			buffer_data->should_scale = false;
		}
	}

	struct wlr_scene_surface *scene_surface =
		wlr_scene_surface_try_from_buffer(buffer);

	if (scene_surface == NULL)
		return;

	struct wlr_surface *surface = scene_surface->surface;

	/* overview viewport-edge crop: show only the visible fraction of the ROOT
	 * surface via a buffer source-box crop (in buffer-local pixels).
	 * NB: never use wlr_scene_subsurface_tree_set_clip for this --
	 * asteroidz-scenefx doesn't implement it, so it binds to vanilla wlroots'
	 * walker, which mangles scenefx's scene structs (the surface vanishes). */
	if (wlr_subsurface_try_from_wlr_surface(surface) == NULL) {
		int32_t bufw = surface->current.buffer_width;
		int32_t bufh = surface->current.buffer_height;
		if (buffer_data->crop_active && bufw > 0 && bufh > 0) {
			struct wlr_fbox src = {
				.x = buffer_data->crop_l * bufw,
				.y = buffer_data->crop_t * bufh,
				.width = buffer_data->crop_w * bufw,
				.height = buffer_data->crop_h * bufh,
			};
			wlr_scene_buffer_set_source_box(buffer, &src);
		} else if (buffer_data->crop_clear) {
			wlr_scene_buffer_set_source_box(buffer, NULL);
		}
	}

	if (buffer_data->should_scale) {

		int32_t surface_width = surface->current.width;
		int32_t surface_height = surface->current.height;

		surface_width = buffer_data->width_scale < 1
							? surface_width
							: buffer_data->width_scale * surface_width;
		surface_height = buffer_data->height_scale < 1
							 ? surface_height
							 : buffer_data->height_scale * surface_height;

		if (surface_width > buffer_data->width &&
			wlr_subsurface_try_from_wlr_surface(surface) == NULL) {
			surface_width = buffer_data->width;
		}

		if (surface_height > buffer_data->height &&
			wlr_subsurface_try_from_wlr_surface(surface) == NULL) {
			surface_height = buffer_data->height;
		}

		if (surface_width > buffer_data->width &&
			wlr_subsurface_try_from_wlr_surface(surface) != NULL) {
			return;
		}

		if (surface_height > buffer_data->height &&
			wlr_subsurface_try_from_wlr_surface(surface) != NULL) {
			return;
		}

		if (surface_height > 0 && surface_width > 0) {
			wlr_scene_buffer_set_dest_size(buffer, surface_width,
										   surface_height);
		}
	}
	// TODO: blur set, opacity set

	if (wlr_xdg_popup_try_from_wlr_surface(surface) != NULL)
		return;

	wlr_scene_buffer_set_corner_radii(
		buffer, corner_radii_from_location(
					GEZERO(config.border_radius - (int32_t)buffer_data->bw),
					buffer_data->corner_location));
}

void scene_buffer_apply_overview_effect(struct wlr_scene_buffer *buffer,
										int32_t sx, int32_t sy, void *data) {
	BufferData *buffer_data = (BufferData *)data;

	int32_t surface_width = 0;
	int32_t surface_height = 0;
	bool is_subsurface = false;

	struct wlr_scene_tree *parent_tree = buffer->node.parent;
	SnapshotMetadata *meta = (SnapshotMetadata *)parent_tree->node.data;
	if (parent_tree->node.data != NULL && meta->type == Snapshot) {
		surface_width = meta->orig_width;
		surface_height = meta->orig_height;
		is_subsurface = meta->is_subsurface;
	} else {
		return;
	}

	surface_height = surface_height * buffer_data->height_scale;
	surface_width = surface_width * buffer_data->width_scale;

	if (is_subsurface && surface_width > 0 && surface_height > 0) {
		wlr_scene_buffer_set_dest_size(buffer, surface_width, surface_height);
	} else if (buffer_data->height > 0 && buffer_data->width > 0) {
		wlr_scene_buffer_set_dest_size(buffer, buffer_data->width,
									   buffer_data->height);
	}

	if (is_subsurface)
		return;

	/* content sits inset by the border width; its arcs use r - bw to stay
	 * concentric with the ring (matches the border's interior cutout) */
	wlr_scene_buffer_set_corner_radii(
		buffer, corner_radii_from_location(
					GEZERO(config.border_radius - (int32_t)buffer_data->bw),
					buffer_data->corner_location));
}

void buffer_set_effect(Client *c, BufferData data) {

	if (!c || c->iskilling)
		return;

	data.bw = c->bw;

	if (c->animation.tagouting || c->animation.tagouted ||
		c->animation.tagining) {
		data.should_scale = false;
	}

	if (c == grabc)
		data.should_scale = false;

	if (!(c->mon && c->mon->isoverview) &&
		(c->isnoradius || c->isfullscreen ||
		 (config.no_radius_when_single && c->mon &&
		  c->mon->visible_tiling_clients == 1))) {
		data.corner_location = CORNER_LOCATION_NONE;
	}

	if (c->overview_scene_surface) {
		wlr_scene_node_for_each_buffer(
			&c->scene_surface->node, scene_buffer_apply_overview_effect, &data);
	} else {
		wlr_scene_node_for_each_buffer(&c->scene_surface->node,
									   scene_buffer_apply_effect, &data);
	}
}

/*
 * ONE CLIP POLICY, ASKED BY EVERYTHING THAT DECORATES A CLIENT.
 *
 * Is this client's SURFACE cropped to its owning monitor? Every decoration --
 * border ring, shadow, split indicator, blur backdrop -- must answer the same
 * way, because a decoration is only ever a statement about where its client
 * is. The conditions below are clip_to_hide()'s, which is the function that
 * actually crops the surface; if that ever gains a case, this has to gain it
 * too, and they are deliberately adjacent in this file for that reason.
 *
 * WHY THIS EXISTS. It used to be two independent rules. The surface was
 * cropped only for scroll-tiled windows and tag animations; the decorations
 * were cropped ALWAYS, with a `c == grabc` escape hatch. For the case the
 * cropping was written for -- a scroller column scrolled past its own
 * monitor's edge, which must not paint blur or border onto a physically
 * adjacent output (de0b5c5) -- the two rules agree and everything is right.
 *
 * For an ORDINARY FLOATING WINDOW dragged across a monitor seam they do not.
 * Its surface is not cropped, so the client is visible on both outputs; its
 * border is, so the far output showed bare client with no decoration, missing
 * even the window's real outer edge. Worse, `c == grabc` hid it exactly while
 * the mouse button was down: the border was whole during the drag and lost its
 * far half the instant you let go, with no geometry change at all. Decoration
 * geometry must not depend on whether a button is still held, and that is why
 * grabc is gone from here rather than merely moved.
 *
 * A window may legitimately be OWNED by one monitor and stretch across two.
 * `c->mon` is a window-management fact; the output boundary is a scissor, not
 * a reason to stop drawing. Each output renders its own intersection.
 */
static inline bool client_clips_to_monitor(Client *c) {
	if (!c || !c->mon)
		return false;
	/* overview places scaled tiles that may overhang; the void-frame masks
	 * hide the overhang instead, so nothing is cropped here */
	if (c->mon->isoverview)
		return false;
	/*
	 * BREAK: AZ_BORDER_OWNER_MONITOR_CLIP=1 restores the exact pre-M4B.1
	 * policy -- decorations cropped to c->mon for every client except the one
	 * currently under the mouse. It reinstates the defect rather than merely
	 * disabling decorations, so the cross-output test fails against it for the
	 * reason the test exists, and the drag/release assertion fails too.
	 */
	static int break_owner_clip = -1;
	if (break_owner_clip < 0) {
		const char *env = getenv("AZ_BORDER_OWNER_MONITOR_CLIP");
		break_owner_clip = env != NULL && env[0] == '1';
	}
	if (break_owner_clip)
		return c != grabc;
	return ISSCROLLTILED(c) || c->animation.tagining || c->animation.tagouted ||
		   c->animation.tagouting;
}

static void client_draw_one_shadow(Client *c, struct wlr_scene_shadow *shadow,
								   struct wlr_scene_blur *blur_backdrop,
								   float blur_edge_sigma,
								   int32_t size, int32_t pos_x, int32_t pos_y,
								   enum corner_location corner_location,
								   bool hit_no_border) {
	int32_t bwoffset = c->bw != 0 && hit_no_border ? (int32_t)c->bw : 0;

	int32_t width, height;
	client_actual_size(c, &width, &height);

	int32_t delta = size + (int32_t)c->bw - bwoffset;

	struct wlr_box client_box = {
		.x = bwoffset,
		.y = bwoffset,
		.width = width + 2 * (int32_t)c->bw - 2 * bwoffset,
		.height = height + 2 * (int32_t)c->bw - 2 * bwoffset,
	};

	/* centered on client_box and grown by delta on every side -- NOT just
	 * pos_x/pos_y shifted, or the box would only ever grow right/down
	 * (bug: shadow_box.x/y used to equal client_box.x/y verbatim, so the
	 * +2*delta width/height only extended rightward/downward, leaving the
	 * left/top edges flush with the window with no margin for the falloff
	 * to occupy -- no shadow or blur there regardless of position_x/y). */
	struct wlr_box shadow_box = {
		.x = pos_x + bwoffset - delta,
		.y = pos_y + bwoffset - delta,
		.width = width + 2 * delta,
		.height = height + 2 * delta,
	};

	struct wlr_box intersection_box;
	wlr_box_intersection(&intersection_box, &client_box, &shadow_box);
	intersection_box.x -= shadow_box.x;
	intersection_box.y -= shadow_box.y;

	/* Underlap the shadow's interior cutout 1px beneath the window edge
	 * (same treatment as the border ring's cutout): the cutout arc and the
	 * window's outer arc are rasterized by different primitives, and
	 * abutting them exactly leaves an AA seam that shows the wallpaper as a
	 * bright wedge at the corner whenever the backdrop there is bright. */
	if (intersection_box.width > 2 && intersection_box.height > 2) {
		intersection_box.x += 1;
		intersection_box.y += 1;
		intersection_box.width -= 2;
		intersection_box.height -= 2;
	}
	struct clipped_region clipped_region = {
		.area = intersection_box,
		.corners = corner_radii_from_location(
			GEZERO(config.border_radius - 1), corner_location),
	};

	struct wlr_box absolute_shadow_box = {
		.x = shadow_box.x + c->animation.current.x,
		.y = shadow_box.y + c->animation.current.y,
		.width = shadow_box.width,
		.height = shadow_box.height,
	};

	int32_t right_offset, bottom_offset, left_offset, top_offset;

	/* the shadow follows its client: cropped to the monitor only when the
	 * surface is (see client_clips_to_monitor) */
	if (!client_clips_to_monitor(c)) {
		right_offset = 0;
		bottom_offset = 0;
		left_offset = 0;
		top_offset = 0;
	} else {
		right_offset =
			GEZERO(absolute_shadow_box.x + absolute_shadow_box.width -
				   c->mon->m.x - c->mon->m.width);
		bottom_offset =
			GEZERO(absolute_shadow_box.y + absolute_shadow_box.height -
				   c->mon->m.y - c->mon->m.height);

		left_offset = GEZERO(c->mon->m.x - absolute_shadow_box.x);
		top_offset = GEZERO(c->mon->m.y - absolute_shadow_box.y);
	}

	left_offset = ASTEROIDZ_MIN(left_offset, shadow_box.width);
	right_offset = ASTEROIDZ_MIN(right_offset, shadow_box.width);
	top_offset = ASTEROIDZ_MIN(top_offset, shadow_box.height);
	bottom_offset = ASTEROIDZ_MIN(bottom_offset, shadow_box.height);

	wlr_scene_node_set_position(&shadow->node, shadow_box.x + left_offset,
								shadow_box.y + top_offset);

	wlr_scene_shadow_set_size(
		shadow, GEZERO(shadow_box.width - left_offset - right_offset),
		GEZERO(shadow_box.height - top_offset - bottom_offset));

	clipped_region.area.x = clipped_region.area.x - left_offset;
	clipped_region.area.y = clipped_region.area.y - top_offset;

	wlr_scene_shadow_set_clipped_region(shadow, clipped_region);

	if (blur_backdrop) {
		/* same box as the shadow itself: the blurred backdrop should reach
		 * exactly as far as the tint drawn over it, no further */
		wlr_scene_node_set_position(&blur_backdrop->node,
									shadow_box.x + left_offset,
									shadow_box.y + top_offset);
		int32_t blur_width =
			GEZERO(shadow_box.width - left_offset - right_offset);
		int32_t blur_height =
			GEZERO(shadow_box.height - top_offset - bottom_offset);
		if (blur_backdrop->width != blur_width ||
			blur_backdrop->height != blur_height)
			wlr_scene_blur_set_size(blur_backdrop, blur_width, blur_height);
		/* The window's own box, kept out of what this blur SAMPLES.
		 *
		 * The blur's box is the shadow's, which is the window plus its
		 * spread -- so the region it samples covers the window itself, and
		 * the scene image holds the PREVIOUS frame there (this node draws
		 * beneath the window, and an undamaged area is never re-rendered).
		 * Without this the blur picks up the window's own pixels and spreads
		 * them outward: a halo in the window's own colour, a glow rather than
		 * a shadow on a dark backdrop.
		 *
		 * In the node's own coordinates, which start at the shadow box's
		 * top-left -- hence the subtraction. */
		struct wlr_box exclude = {
			.x = client_box.x - (shadow_box.x + left_offset),
			.y = client_box.y - (shadow_box.y + top_offset),
			.width = client_box.width,
			.height = client_box.height,
		};
		wlr_scene_blur_set_sample_exclude(blur_backdrop, &exclude);
		/* edge_softness makes the blur's own visibility fade via the same
		 * analytic falloff as the shadow tint (same box, same sigma), so
		 * the two blend into one continuous halo instead of the blur
		 * cutting off as an obviously rectangular patch. `corners` becomes
		 * that falloff's own corner radii (not a hard SDF radius) once
		 * edge_softness is set -- still just the window's own radius. */
		struct fx_corner_radii blur_radii =
			corner_radii_from_location(config.border_radius, corner_location);
		if (!fx_corner_radii_eq(blur_backdrop->corners, blur_radii))
			wlr_scene_blur_set_corner_radii(blur_backdrop, blur_radii);
		if (blur_backdrop->edge_softness != blur_edge_sigma)
			wlr_scene_blur_set_edge_softness(blur_backdrop, blur_edge_sigma);
	}
}

/* Put the shadow tree where this window's shadow is allowed to be drawn, and
 * keep it where the window is.
 *
 * A TILED window's shadow goes on LyrTileShadow, beneath every tiled window,
 * so it cannot reach a neighbour's pixels. A window that is anywhere else --
 * floating on LyrTop, maximized, fullscreen, animating out on LyrFadeOut, or
 * laid out by the overview -- keeps its shadow inside its own tree, which is
 * both correct (it really is above whatever is under it) and what makes it
 * travel with the window through every reparent those paths already do.
 *
 * Which of the two is decided by where c->scene actually IS, not by
 * c->isfloating: the flag and the tree disagree for a frame or two around
 * fullscreen and overview transitions, and it is the tree that decides what
 * gets drawn over what. */
static void client_sync_shadow_tree(Client *c) {
	if (!c->shadow_tree || !c->scene)
		return;

	bool tiled = c->scene->node.parent == layers[LyrTile];
	struct wlr_scene_tree *want = tiled ? layers[LyrTileShadow] : c->scene;

	if (c->shadow_tree->node.parent != want) {
		wlr_scene_node_reparent(&c->shadow_tree->node, want);
		/* reparent puts a node on top of its new siblings, which inside
		 * c->scene would be over the window's own surface */
		if (!tiled)
			wlr_scene_node_lower_to_bottom(&c->shadow_tree->node);
	}

	if (tiled) {
		/* c->scene sits at the window's layout position and LyrTileShadow is
		 * at the origin, so mirroring the one onto the other is what keeps
		 * every shadow box below in the window's own coordinates. */
		if (c->shadow_tree->node.x != c->scene->node.x ||
			c->shadow_tree->node.y != c->scene->node.y)
			wlr_scene_node_set_position(&c->shadow_tree->node, c->scene->node.x,
										c->scene->node.y);
		/* Enabled-ness no longer comes for free from the parent: a window on
		 * a hidden tag has its own tree disabled, and its shadow is no longer
		 * inside it. Without this the shadows of every window on every other
		 * tag stay on screen. */
		if (c->shadow_tree->node.enabled != c->scene->node.enabled)
			wlr_scene_node_set_enabled(&c->shadow_tree->node,
									   c->scene->node.enabled);
	} else {
		if (c->shadow_tree->node.x != 0 || c->shadow_tree->node.y != 0)
			wlr_scene_node_set_position(&c->shadow_tree->node, 0, 0);
		if (!c->shadow_tree->node.enabled)
			wlr_scene_node_set_enabled(&c->shadow_tree->node, true);
	}
}

void client_draw_shadow(Client *c) {

	if (c->iskilling || !client_surface(c)->mapped || c->isnoshadow)
		return;

	client_sync_shadow_tree(c);

	bool active = config.shadows && !c->isfullscreen &&
				  (c->isfloating || !config.shadow_only_floating);

	if (!active) {
		if (c->shadow->node.enabled)
			wlr_scene_node_set_enabled(&c->shadow->node, false);
		if (c->contact_shadow && c->contact_shadow->node.enabled)
			wlr_scene_node_set_enabled(&c->contact_shadow->node, false);
		if (c->shadow_blur) {
			wlr_scene_node_destroy(&c->shadow_blur->node);
			c->shadow_blur = NULL;
		}
		return;
	} else {
		if (c->scene_surface->node.enabled && !c->shadow->node.enabled)
			wlr_scene_node_set_enabled(&c->shadow->node, true);
		if (c->contact_shadow && c->scene_surface->node.enabled &&
			c->contact_shadow->node.enabled != (bool)config.shadows_contact)
			wlr_scene_node_set_enabled(&c->contact_shadow->node,
									   config.shadows_contact);
	}

	/* shadow_blur has a real GPU cost (an extra blur pass in the shadow's
	 * footprint), so it's created/destroyed on demand like blur_node --
	 * not just enabled/disabled like the shadow nodes above */
	if (!config.shadows_blur_background) {
		if (c->shadow_blur) {
			wlr_scene_node_destroy(&c->shadow_blur->node);
			c->shadow_blur = NULL;
		}
	} else if (!c->shadow_blur) {
		/* Into the shadow tree, beside the shadows it belongs to -- not into
		 * c->scene. A blur node under LyrTile would sample the neighbouring
		 * window it overlaps and spread that window's own pixels along the
		 * seam; on LyrTileShadow nothing tiled has been drawn yet when it
		 * runs, so what it samples is the backdrop it is named for. */
		c->shadow_blur = wlr_scene_blur_create(
			c->shadow_tree ? c->shadow_tree : c->scene, 0, 0);
		if (c->shadow_blur) {
			wlr_scene_node_place_below(&c->shadow_blur->node, &c->shadow->node);
		}
	}
	if (c->shadow_blur) {
		/* shadows_blur_background_strength drives ALPHA, not scenefx's own
		 * "strength" property, even though it's the subtlety knob either
		 * way: fx_render_pass_add_blur only takes the cheap cached-buffer
		 * path when blur_strength == 1.0 exactly (fx_pass.c's has_strength
		 * check) -- anything less forces a full per-frame re-blur even
		 * with should_only_blur_bottom_layer on. alpha is a separate,
		 * free blend applied at the final draw, so it never defeats the
		 * cache. Verified headlessly: strength=0.17/alpha=1.0 vs.
		 * strength=1.0/alpha=0.17 are visually indistinguishable for this
		 * soft-halo use case (no sharp edges to ghost, unlike the API's
		 * general "alpha alone can look off" caveat). Leave scenefx's own
		 * strength at its 1.0 default (set once at creation, untouched).
		 */
		wlr_scene_blur_set_darken(c->shadow_blur,
								  config.shadows_blur_background_darken);
		if (c->shadow_blur->alpha != config.shadows_blur_background_strength)
			wlr_scene_blur_set_alpha(c->shadow_blur,
									 config.shadows_blur_background_strength);
		/* Cached bottom-layer path (cheap: reuses the monitor's shared
		 * blurred-wallpaper snapshot instead of re-blurring every frame) is
		 * correct only when the shadow's actual backdrop IS just the
		 * wallpaper -- true for tiled windows, not guaranteed for floating
		 * ones (which can sit over arbitrary other windows). Same rule
		 * client_update_blur already applies to regular window blur. The
		 * cache node is monitor-shared and lazily created on demand --
		 * ensure it exists before relying on it (it may not if
		 * shadows_blur_background was only just turned on).
		 *
		 * BOTH PATHS HAVE AN ARTEFACT, and forcing this one for floating
		 * windows too (tried 2026-08-01, reverted the same day) trades a
		 * smaller one for a bigger one:
		 *
		 *   live  -- samples the framebuffer, and a shadow's footprint hugs
		 *            its window, so it picks up the window's OWN pixels and
		 *            smears them outward. On a black wallpaper the pixels
		 *            approaching the edge read 0, 5, 12, 20 instead of
		 *            staying black, and the halo takes the window's colour.
		 *            A thin rim, visible on a dark desktop.
		 *   cached -- draws the blurred WALLPAPER whatever is really beneath.
		 *            Over a dark window on a bright wallpaper that is a broad
		 *            bright haze, much worse than the rim, and it is what
		 *            a real desktop looks like: floating windows sit over
		 *            other windows, not over bare wallpaper.
		 *
		 * The honest fix is in the sampling, not in the choice between them:
		 * the blur wants what is beneath the window MINUS the window itself,
		 * and scenefx has no way to ask for that today. Until it does, the
		 * feature is off by default and `shadows_blur_background 0` gives a
		 * plain shadow with neither artefact. */
		bool blur_cached = config.blur_optimized && !c->isfloating;
		if (blur_cached)
			ensure_monitor_blur_node(c->mon);
		if (c->shadow_blur->should_only_blur_bottom_layer != blur_cached)
			wlr_scene_blur_set_should_only_blur_bottom_layer(c->shadow_blur,
															 blur_cached);
	}

	bool hit_no_border = check_hit_no_border(c);
	enum corner_location current_corner_location =
		c->isfullscreen || (config.no_radius_when_single && c->mon &&
							c->mon->visible_tiling_clients == 1)
			? CORNER_LOCATION_NONE
			: CORNER_LOCATION_ALL;

	/* full macOS-style shadow for floating windows; tiled windows get a
	 * compact version so it doesn't spill across gaps onto neighbours */
	float state_scale = c->isfloating ? 1.0f : config.shadows_tiled_scale;

	float shadow_blur_sigma = config.shadows_blur * state_scale;
	wlr_scene_shadow_set_blur_sigma(c->shadow, shadow_blur_sigma);
	client_draw_one_shadow(c, c->shadow, c->shadow_blur, shadow_blur_sigma,
						   (int32_t)(config.shadows_size * state_scale),
						   (int32_t)(config.shadows_position_x * state_scale),
						   (int32_t)(config.shadows_position_y * state_scale),
						   current_corner_location, hit_no_border);
	if (c->contact_shadow && config.shadows_contact) {
		wlr_scene_shadow_set_blur_sigma(
			c->contact_shadow, config.shadows_contact_blur * state_scale);
		client_draw_one_shadow(
			c, c->contact_shadow, NULL, 0.0f,
			(int32_t)ASTEROIDZ_MAX(config.shadows_contact_size * state_scale, 2),
			(int32_t)(config.shadows_contact_position_x * state_scale),
			(int32_t)(config.shadows_contact_position_y * state_scale),
			current_corner_location, hit_no_border);
	}
}

/* monocle with more than one window: each client's own titlebar becomes one
 * segment of a shared row instead of a separate tab-bar widget. is_first/
 * is_last control which outer corner (if any) this segment rounds, matching
 * a browser-style tab strip: only the outermost segments round, and only on
 * their outward-facing side. The focused segment additionally gets a close
 * button (leftmost within its own segment); background segments are
 * title-only, click-to-focus (handled generically in handle_buttonpress). */
/* Titlebar corner rule, canonical for EVERY titlebar row (monocle segment
 * strips and standalone per-window bars alike): the row's first element
 * rounds its top-left corner, the last rounds its top-right, anything in
 * between stays square. A standalone titlebar is both first and last. The
 * close button and title tab split one bar, so whichever of the two
 * actually owns an outer edge gets that edge's rounding (a zero-width tab
 * hands the right edge back to the close button, and vice versa). */
static void titlebar_apply_corner_rule(Client *c, bool is_first, bool is_last,
									   int32_t close_w, int32_t tab_w) {
	bool has_close = c->titlebar_close_node != NULL && close_w > 0;
	bool has_tab = c->titlebar_node != NULL && tab_w > 0;
	enum corner_location close_mask = CORNER_LOCATION_NONE;
	enum corner_location tab_mask = CORNER_LOCATION_NONE;

	if (has_close) {
		if (is_first)
			close_mask |= CORNER_LOCATION_TOP_LEFT;
		if (is_last && !has_tab)
			close_mask |= CORNER_LOCATION_TOP_RIGHT;
	}
	if (has_tab) {
		if (is_last)
			tab_mask |= CORNER_LOCATION_TOP_RIGHT;
		if (is_first && !has_close)
			tab_mask |= CORNER_LOCATION_TOP_LEFT;
	}

	if (c->titlebar_close_node)
		asteroidz_tab_bar_node_set_corner_mask(c->titlebar_close_node, close_mask);
	if (c->titlebar_node)
		asteroidz_tab_bar_node_set_corner_mask(c->titlebar_node, tab_mask);
}

void client_draw_monocle_titlebar_segment(Client *c, int32_t x, int32_t y,
										  int32_t w, bool focused,
										  bool is_first, bool is_last) {
	if (!c || !c->titlebar_node)
		return;

	int32_t th = config.titlebar_height;
	if (th <= 0 || w <= 0) {
		asteroidz_tab_bar_node_set_enabled(c->titlebar_node, false);
		if (c->titlebar_close_node)
			asteroidz_tab_bar_node_set_enabled(c->titlebar_close_node, false);
		return;
	}

	/* Every segment is close-button (left) + title tab (right), whether
	 * focused or not -- so each window in the monocle strip can be closed
	 * directly. The segments touch to form one continuous strip; only the
	 * strip's outer corners round (first's top-left, last's top-right) and
	 * a separator divides each non-last segment from the next so adjacent
	 * same-colored (inactive) segments don't blend into one bar. */
	int32_t close_w = ASTEROIDZ_MIN(th, w);
	int32_t tab_w = w - close_w;
	if (tab_w < 0)
		tab_w = 0;

	/* the segment row is a SHARED strip: hidden monocle windows' scenes are
	 * disabled, but their segments must stay clickable -- so segments live on
	 * the global LyrDecorate (absolute coords), unlike per-window tabs which
	 * are parented inside the client's scene. */
	asteroidz_tab_bar_node_reparent(c->titlebar_node, layers[LyrDecorate]);

	if (c->titlebar_close_node) {
		asteroidz_tab_bar_node_reparent(c->titlebar_close_node,
									layers[LyrDecorate]);
		asteroidz_tab_bar_node_set_enabled(c->titlebar_close_node, true);
		asteroidz_tab_bar_node_set_position(c->titlebar_close_node, x, y);
		/* One logical pixel WIDER than the gap it fills, so it underlaps the
		 * tab that starts at x + close_w.
		 *
		 * The two segments abut exactly in logical coordinates, which was
		 * enough while both were rasterised at logical size and upscaled by
		 * one shared resampling. Each is now rasterised at the output's own
		 * scale, and at a fractional one close_w * scale is not a whole
		 * device pixel -- so the two buffers' edges land either side of a
		 * boundary and the backdrop shows through the join. Overlapping
		 * costs nothing: the pixel underneath is the same colour, and the
		 * tab draws over it. */
		asteroidz_tab_bar_node_set_size(c->titlebar_close_node, close_w + 1, th);
		asteroidz_tab_bar_node_set_content_scale(c->titlebar_close_node, 1.0f);
		/* close is the segment's left part: it owns the strip's left border
		 * when the segment is leftmost; its right side touches this
		 * segment's own tab, so no border/separator there (corners are set
		 * once for the whole segment by titlebar_apply_corner_rule below) */
		asteroidz_tab_bar_node_set_titlebar_border(
			c->titlebar_close_node, config.borderpx, is_first, false);
		asteroidz_tab_bar_node_set_titlebar_separator(c->titlebar_close_node,
												  false);
		asteroidz_tab_bar_node_set_focus(c->titlebar_close_node, focused);
	}

	asteroidz_tab_bar_node_set_enabled(c->titlebar_node, true);
	asteroidz_tab_bar_node_set_position(c->titlebar_node, x + close_w, y);
	asteroidz_tab_bar_node_set_size(c->titlebar_node, tab_w, th);
	titlebar_apply_corner_rule(c, is_first, is_last, close_w, tab_w);
	/* tab is the segment's right part: right border only when this is the
	 * rightmost segment; otherwise a separator divides it from the next
	 * segment. Its left touches this segment's close button (no left border). */
	asteroidz_tab_bar_node_set_titlebar_border(c->titlebar_node, config.borderpx,
										   false, is_last);
	asteroidz_tab_bar_node_set_titlebar_separator(c->titlebar_node, !is_last);
	asteroidz_tab_bar_node_set_content_scale(c->titlebar_node, 1.0f);
	asteroidz_tab_bar_node_update(c->titlebar_node, client_get_title(c),
								  client_render_scale(c));
	asteroidz_tab_bar_node_set_focus(c->titlebar_node, focused);
}

/* position the titlebar strip just above the client's current (animated)
 * geometry. BeOS-style: a compact tab sized to a fraction of the window
 * width (not a full-width strip), left-aligned, with the close button
 * immediately to its right rather than pinned to the window's far edge.
 * Geometry-linked: re-run whenever the client's animated geometry changes,
 * since a titlebar applies to any tiled client while config.enable_titlebar
 * is set. */
void client_draw_titlebar(Client *c) {
	if (!c || !c->mon || !c->titlebar_node)
		return;

	/* with more than one window, monocle lays its titlebars out as a row of
	 * segments itself (see client_draw_monocle_titlebar_segment, called from
	 * monocle() in horizontal.h) rather than one compact per-window tab.
	 * Only fake-TILED windows join the segment row; a floating window on a
	 * monocle tag keeps its own per-window tab below. */
	if (!c->mon->isoverview && is_monocle_layout(c->mon) &&
		c->mon->visible_fake_tiling_clients > 1 && ISFAKETILED(c))
		return;

	bool titlebar_wanted = config.enable_titlebar || is_monocle_layout(c->mon);
	bool ov = c->mon->isoverview;
	/* Draw the titlebar for tiled AND floating windows, incl. in the overview
	 * (scaled to the shrunk window). In the overview draw ONLY for windows on the
	 * previewed tag (ov_main_tag) that are actually shown: other tags' windows and
	 * columns dropped for overrunning the viewport (is_overview_hidden) are hidden
	 * in the main area, so their titlebars would otherwise linger as overlapping
	 * ghosts (drawn at a stale position by the per-frame animation path). */
	if (!titlebar_wanted || c->isfullscreen || !client_wants_ssd(c) ||
		client_no_titlebar(c) /* splash / no-titlebar rule: no tab */ ||
		(!ov && c->is_monocle_hide) /* the exposé shows ALL monocle windows */ ||
		c->isminimized || c->iskilling || c->isunglobal || !VISIBLEON(c, c->mon) ||
		(ov && (c->is_overview_hidden ||
				get_tags_first_tag_num(c->tags) != c->mon->ov_main_tag ||
				/* faithful mirror: the tab sits at the window's LEFT edge; a
				 * column clipped on the left has that edge (and thus its tab)
				 * off the desktop, so the mirror doesn't show one either */
				(c->ov_clip_active && c->ov_clip.x > 0)))) {
		/* use the helper (not a raw node disable): it also disables the
		 * tab's shadow, which would otherwise linger as a floating strip */
		asteroidz_tab_bar_node_set_enabled(c->titlebar_node, false);
		if (c->titlebar_close_node)
			asteroidz_tab_bar_node_set_enabled(c->titlebar_close_node, false);
		return;
	}

	int32_t th = config.titlebar_height;
	if (ov) { /* scale the bar to the shrunk overview window */
		/* a viewport-edge window is sized to its VISIBLE portion (ov_clip), so
		 * that portion -- not the full window -- is the reference width; this
		 * keeps th (and the font scale) identical to uncropped neighbours */
		float ref_w = (c->ov_clip_active && c->ov_clip.width > 0)
						  ? (float)c->ov_clip.width
						  : (float)c->overview_backup_geom.width;
		th = (int32_t)(th * ((float)c->animation.current.width /
							 fmaxf(1.0f, ref_w)));
		/* legibility floor: below ~22px the title is an unreadable sliver
		 * (monocle exposé shrinks to ~0.3x). Must match client_tile_resize. */
		th = ASTEROIDZ_MAX(th, ASTEROIDZ_MIN(22, config.titlebar_height));
	}
	/* Per-window tabs live INSIDE the client's scene tree, so they stack,
	 * move, animate, fade out and hide with their window automatically --
	 * a floating window's titlebar has exactly the window's own z-order.
	 * (The monocle segment row is the one exception: hidden monocle windows'
	 * scenes are disabled but their segments must stay visible, so segments
	 * reparent back to the global LyrDecorate.) Coordinates are relative to
	 * the scene origin (== animation.current.x/y), so the tab is simply at
	 * (0, -th) and needs no per-frame repositioning to track the window. */
	asteroidz_tab_bar_node_reparent(c->titlebar_node, c->scene);
	if (c->titlebar_close_node)
		asteroidz_tab_bar_node_reparent(c->titlebar_close_node, c->scene);
	int32_t tb_x = 0;
	/* Sit the tab flush ON the window's top border (overlap by bw): the
	 * frame line passes behind the tab and emerges at its right edge, so
	 * tab + frame read as one assembly instead of a tab floating above a
	 * detached line. */
	int32_t tb_y = -th + (int32_t)c->bw;
	int32_t tb_w = c->animation.current.width;
	int32_t close_w = ASTEROIDZ_MIN(th, tb_w);

	/* In the overview everything is a miniature of the desktop: one shrink
	 * factor (th / titlebar_height) drives the tab height, its fixed-width
	 * caps AND the font/padding, so every layout's titlebar scales alike. */
	float tbs = fmaxf(0.05f, (float)th / fmaxf(1.0f, (float)config.titlebar_height));
	int32_t tab_cap = (int32_t)(280 * tbs + 0.5f);
	int32_t tab_min = (int32_t)(160 * tbs + 0.5f);

	/* BeOS-style: a small, roughly fixed-width tab rather than a strip that
	 * scales with the window; only widen it on genuinely narrow windows. */
	int32_t tab_w = ASTEROIDZ_MIN(tab_cap, (int32_t)(tb_w * 0.6f));
	tab_w = ASTEROIDZ_MAX(tab_w, ASTEROIDZ_MIN(tab_min, tb_w - close_w));
	tab_w = ASTEROIDZ_MIN(tab_w, tb_w - close_w);
	if (tab_w < 0)
		tab_w = 0;

	/* the close+tab assembly sits at the window's own LEFT edge (tb_x=0),
	 * which -- like a scroller column transiting past its own monitor's
	 * edge -- can straddle the boundary with an adjacent monitor while
	 * MOVE-animating into or out of view. wlr_scene renders a node on every
	 * output its box overlaps (by design -- that's how a window dragged
	 * across two monitors is meant to look), so a mere overlap check still
	 * lets the tab render on the neighbour for however long the straddle
	 * lasts. Require the tab to be FULLY inside c->mon's bounds instead of
	 * merely intersecting it: this trades a few pixels of "pops in slightly
	 * late" for zero chance of ever putting a pixel on the wrong monitor. */
	struct wlr_box tab_screen_box = {
		.x = (int32_t)c->animation.current.x + tb_x,
		.y = (int32_t)c->animation.current.y + tb_y,
		.width = close_w + tab_w,
		.height = th,
	};
	bool tab_fully_on_own_mon =
		tab_screen_box.x >= c->mon->m.x &&
		tab_screen_box.x + tab_screen_box.width <=
			c->mon->m.x + c->mon->m.width &&
		tab_screen_box.y >= c->mon->m.y &&
		tab_screen_box.y + tab_screen_box.height <=
			c->mon->m.y + c->mon->m.height;
	if (!tab_fully_on_own_mon) {
		asteroidz_tab_bar_node_set_enabled(c->titlebar_node, false);
		if (c->titlebar_close_node)
			asteroidz_tab_bar_node_set_enabled(c->titlebar_close_node, false);
		return;
	}

	bool focused = c == selmon->sel;

	/* close (left) + tab (right) form one compact titlebar: close borders
	 * left+top, tab borders right+top, and the touching inner seam is
	 * left unbordered. */
	if (c->titlebar_close_node) {
		asteroidz_tab_bar_node_set_enabled(c->titlebar_close_node, true);
		asteroidz_tab_bar_node_set_position(c->titlebar_close_node, tb_x, tb_y);
		/* One logical pixel WIDER than the gap it fills, so it underlaps the
		 * tab that starts at x + close_w.
		 *
		 * The two segments abut exactly in logical coordinates, which was
		 * enough while both were rasterised at logical size and upscaled by
		 * one shared resampling. Each is now rasterised at the output's own
		 * scale, and at a fractional one close_w * scale is not a whole
		 * device pixel -- so the two buffers' edges land either side of a
		 * boundary and the backdrop shows through the join. Overlapping
		 * costs nothing: the pixel underneath is the same colour, and the
		 * tab draws over it. */
		asteroidz_tab_bar_node_set_size(c->titlebar_close_node, close_w + 1, th);
		/* content_scale shrinks font+padding+icon to fit the scaled-down bar;
		 * the _update scale param is a HiDPI density scale (dest-size cancels
		 * it visually), so it must stay 1.0 here */
		asteroidz_tab_bar_node_set_content_scale(c->titlebar_close_node, tbs);
		asteroidz_tab_bar_node_update(c->titlebar_close_node, "×",
								  client_render_scale(c));
		asteroidz_tab_bar_node_set_titlebar_border(c->titlebar_close_node,
											   config.borderpx, true, false);
		asteroidz_tab_bar_node_set_focus(c->titlebar_close_node, focused);
	}

	asteroidz_tab_bar_node_set_enabled(c->titlebar_node, true);
	asteroidz_tab_bar_node_set_position(c->titlebar_node, tb_x + close_w, tb_y);
	asteroidz_tab_bar_node_set_size(c->titlebar_node, tab_w, th);
	/* a standalone bar is both first and last in its "row"; this also resets
	 * masks a monocle segment row set (a middle segment's square corners
	 * used to leak into the per-window tab when leaving monocle) */
	titlebar_apply_corner_rule(c, true, true, close_w, tab_w);
	asteroidz_tab_bar_node_set_titlebar_border(c->titlebar_node, config.borderpx,
										   false, true);
	/* no separators in tile: each window is its own standalone titlebar, not
	 * a shared strip (reset in case this window came from a monocle tag) */
	asteroidz_tab_bar_node_set_titlebar_separator(c->titlebar_node, false);
	asteroidz_tab_bar_node_set_content_scale(c->titlebar_node, tbs);
	asteroidz_tab_bar_node_update(c->titlebar_node, client_get_title(c),
								  client_render_scale(c));
	asteroidz_tab_bar_node_set_focus(c->titlebar_node, focused);
}

void apply_split_border(Client *c, bool hit_no_border) {

	if (c->iskilling || !c->mon || !client_surface(c)->mapped)
		return;

	const Layout *layout = c->mon->pertag->ltidxs[c->mon->pertag->curtag];

	if (hit_no_border || !ISTILED(c) || layout->id != DWINDLE ||
		!config.dwindle_manual_split || c->isfullscreen) {
		if (c->splitindicator[0]->node.enabled) {
			wlr_scene_node_set_enabled(&c->splitindicator[0]->node, false);
		}
		if (c->splitindicator[1]->node.enabled) {
			wlr_scene_node_set_enabled(&c->splitindicator[1]->node, false);
		}
		return;
	} else {

		DwindleNode **root =
			&c->mon->pertag->dwindle_root[c->mon->pertag->curtag];
		DwindleNode *dnode = dwindle_find_leaf(*root, c);

		if (!dnode) {
			wlr_scene_node_set_enabled(&c->splitindicator[0]->node, false);
			wlr_scene_node_set_enabled(&c->splitindicator[1]->node, false);
			return;
		} else {
			if (dnode->custom_leaf_split_h) {
				wlr_scene_node_set_enabled(&c->splitindicator[0]->node, false);
				wlr_scene_node_set_enabled(&c->splitindicator[1]->node, true);
			} else {
				wlr_scene_node_set_enabled(&c->splitindicator[0]->node, true);
				wlr_scene_node_set_enabled(&c->splitindicator[1]->node, false);
			}
		}
	}

	struct wlr_box fullgeom = c->animation.current;
	// Must stay signed here: if GEZERO used unsigned, the other operands would
	// get promoted to unsigned too and lose their negative values, causing errors
	int32_t bw = (int32_t)c->bw;

	int32_t right_offset, bottom_offset, left_offset, top_offset;

	if (!client_clips_to_monitor(c)) {
		right_offset = 0;
		bottom_offset = 0;
		left_offset = 0;
		top_offset = 0;
	} else {
		right_offset =
			GEZERO(c->animation.current.x + c->animation.current.width -
				   c->mon->m.x - c->mon->m.width);
		bottom_offset =
			GEZERO(c->animation.current.y + c->animation.current.height -
				   c->mon->m.y - c->mon->m.height);

		left_offset = GEZERO(c->mon->m.x - c->animation.current.x);
		top_offset = GEZERO(c->mon->m.y - c->animation.current.y);
	}

	int32_t border_down_width =
		GEZERO(fullgeom.width - 2 * config.border_radius -
			   GEZERO((left_offset + right_offset) - config.border_radius));
	int32_t border_down_height =
		GEZERO(bw - bottom_offset - GEZERO(top_offset + bw - fullgeom.height));

	int32_t border_right_width =
		GEZERO(bw - right_offset - GEZERO(left_offset + bw - fullgeom.width));
	int32_t border_right_height =
		GEZERO(fullgeom.height - 2 * config.border_radius -
			   GEZERO((top_offset + bottom_offset) - config.border_radius));

	int32_t border_down_x = GEZERO(config.border_radius +
								   GEZERO(left_offset - config.border_radius));
	int32_t border_down_y = GEZERO(fullgeom.height - bw) +
							GEZERO(top_offset + bw - fullgeom.height);

	int32_t border_right_x =
		GEZERO(fullgeom.width - bw) + GEZERO(left_offset + bw - fullgeom.width);
	int32_t border_right_y = GEZERO(config.border_radius +
									GEZERO(top_offset - config.border_radius));

	set_rect_size(c->splitindicator[0], border_down_width, border_down_height);
	set_rect_size(c->splitindicator[1], border_right_width,
				  border_right_height);
	wlr_scene_node_set_position(&c->splitindicator[0]->node, border_down_x,
								border_down_y);
	wlr_scene_node_set_position(&c->splitindicator[1]->node, border_right_x,
								border_right_y);
}

void apply_border(Client *c) {
	if (!c || c->iskilling || !client_surface(c)->mapped)
		return;

	if (c->isfullscreen) {
		if (c->border->node.enabled) {
			wlr_scene_node_set_position(&c->scene_surface->node, 0, 0);
			wlr_scene_node_set_enabled(&c->border->node, false);
		}
		return;
	}
	/* NB: the border is enabled at the end, only once a valid interior cut-out
	 * has been computed -- enabling it here (before the clip) let it render as
	 * a full window-filling rect for one frame when animation.current is still
	 * degenerate (the open/close focus-colour "flash"). */

	bool hit_no_border = check_hit_no_border(c);

	apply_split_border(c, hit_no_border);

	enum corner_location current_corner_location;
	if (!(c->mon && c->mon->isoverview) &&
		(c->isfullscreen || (config.no_radius_when_single && c->mon &&
							 c->mon->visible_tiling_clients == 1))) {
		current_corner_location = CORNER_LOCATION_NONE;
	} else {
		current_corner_location = set_client_corner_location(c);
	}

	if (hit_no_border && config.smartgaps) {
		c->bw = 0;
		c->fake_no_border = true;
	} else if (hit_no_border && !config.smartgaps) {
		wlr_scene_rect_set_size(c->border, 0, 0);
		wlr_scene_node_set_position(&c->scene_surface->node, c->bw, c->bw);
		c->fake_no_border = true;
		return;
	} else if (!c->isfullscreen && VISIBLEON(c, c->mon)) {
		c->bw = c->isnoborder ? 0 : config.borderpx;
		c->fake_no_border = false;
	}

	struct wlr_box clip_box = c->animation.current;
	// Must stay signed here: if GEZERO used unsigned, the other operands would
	// get promoted to unsigned too and lose their negative values, causing errors
	int32_t bw = (int32_t)c->bw;

	int32_t right_offset, bottom_offset, left_offset, top_offset;

	if (!client_clips_to_monitor(c)) {
		right_offset = 0;
		bottom_offset = 0;
		left_offset = 0;
		top_offset = 0;
	} else {
		right_offset =
			GEZERO(c->animation.current.x + c->animation.current.width -
				   c->mon->m.x - c->mon->m.width);
		bottom_offset =
			GEZERO(c->animation.current.y + c->animation.current.height -
				   c->mon->m.y - c->mon->m.height);

		left_offset = GEZERO(c->mon->m.x - c->animation.current.x);
		top_offset = GEZERO(c->mon->m.y - c->animation.current.y);
	}

	int32_t inner_surface_width = GEZERO(clip_box.width - 2 * bw);
	int32_t inner_surface_height = GEZERO(clip_box.height - 2 * bw);

	int32_t inner_surface_x = GEZERO(bw - left_offset);
	int32_t inner_surface_y = GEZERO(bw - top_offset);

	int32_t rect_x = left_offset;
	int32_t rect_y = top_offset;

	int32_t rect_width =
		GEZERO(c->animation.current.width - left_offset - right_offset);
	int32_t rect_height =
		GEZERO(c->animation.current.height - top_offset - bottom_offset);

	if (left_offset > c->bw)
		inner_surface_width =
			inner_surface_width - left_offset + (int32_t)c->bw;

	if (top_offset > c->bw)
		inner_surface_height =
			inner_surface_height - top_offset + (int32_t)c->bw;

	if (right_offset > 0) {
		inner_surface_width =
			ASTEROIDZ_MIN(clip_box.width, inner_surface_width + right_offset);
	}

	if (bottom_offset > 0) {
		inner_surface_height =
			ASTEROIDZ_MIN(clip_box.height, inner_surface_height + bottom_offset);
	}

	/* the ring's interior cutout is inset by bw, so its arcs must use
	 * radius r - bw to stay concentric with the outer rounding; reusing the
	 * outer radius thins the ring at corners and leaves AA slivers where
	 * the cutout and the content rounding disagree */
	struct clipped_region clipped_region = {
		.area = {inner_surface_x, inner_surface_y, inner_surface_width,
				 inner_surface_height},
		.corners = corner_radii_from_location(
			/* 1px tighter than the content arc: the ring UNDERLAPS the
			 * content corner so the AA seam between the two independently
			 * rasterized arcs lands on border paint, not on the wallpaper
			 * behind the window (showed as a bright dot mid-arc) */
			GEZERO(config.border_radius - bw - 1), current_corner_location),
	};

	wlr_scene_node_set_position(&c->scene_surface->node, c->bw, c->bw);
	wlr_scene_rect_set_size(c->border, rect_width, rect_height);
	wlr_scene_node_set_position(&c->border->node, rect_x, rect_y);
	wlr_scene_rect_set_corner_radii(
		c->border, corner_radii_from_location(config.border_radius,
											  current_corner_location));
	wlr_scene_rect_set_clipped_region(c->border, clipped_region);

	/* Only show the border once its interior is actually cut out. On a window's
	 * first animation frame animation.current is still degenerate, so the
	 * cut-out collapses to empty and the border would fill the whole window
	 * with the focus/border colour for one frame -- the open/close flash. */
	bool border_cut_valid = inner_surface_width > 0 && inner_surface_height > 0;
	wlr_scene_node_set_enabled(&c->border->node, border_cut_valid);

	/* keep the shadow shapes in sync with the corners actually rounded */
	if (c->shadow)
		wlr_scene_shadow_set_corner_radii(
			c->shadow, corner_radii_from_location(config.border_radius,
												  current_corner_location));
	if (c->contact_shadow)
		wlr_scene_shadow_set_corner_radii(
			c->contact_shadow,
			corner_radii_from_location(config.border_radius,
									   current_corner_location));

	if (c->blur_node) {
		/* overview: the dim scrim sits on LyrDecorate (not the bottom layer),
		 * so cached bottom-layer-only blur would sample the UNDIMMED wallpaper
		 * -- keep sampling all layers below while the overview is up (this
		 * runs per-frame and would otherwise stomp the arrange-time setting) */
		bool blur_cached = config.blur_optimized && !c->isfloating &&
						   !(c->mon && c->mon->isoverview);
		if (c->blur_node->should_only_blur_bottom_layer != blur_cached)
			wlr_scene_blur_set_should_only_blur_bottom_layer(c->blur_node,
															 blur_cached);
		/* clip to the client's own monitor bounds exactly like the border
		 * ring above does (left/right/top/bottom_offset): a scroller column
		 * scrolled off-screen can sit far past its own monitor's edge, and
		 * without this the blur -- unlike the already-clipped border --
		 * rendered at full size, bleeding into whatever's physically next
		 * in the global output layout (a real neighboring monitor, if one
		 * happens to sit there). */
		int32_t blur_width =
			GEZERO(clip_box.width - 2 * bw - left_offset - right_offset);
		int32_t blur_height =
			GEZERO(clip_box.height - 2 * bw - top_offset - bottom_offset);
		/* The blur node backs the TRANSLUCENT content and is fully covered
		 * by content + border ring above it, so its corners must round LESS
		 * than the content arc (r - bw): rounding at the same radius let the
		 * blur's one-sided edge AA undercover the content corner, and the
		 * translucent content then composited over RAW wallpaper -- sharp
		 * unblurred detail (wallpaper sparkles) surfacing as bright dots
		 * just inside some corners. 2px tighter keeps the blur strictly
		 * inside the ring's interior paint (cutout is r - bw - 1). */
		struct fx_corner_radii blur_radii = corner_radii_from_location(
			GEZERO(config.border_radius - bw - 2), current_corner_location);

		/* only touch the scene when something changed: this runs on
		 * every animation tick */
		wlr_scene_node_set_position(&c->blur_node->node, bw + left_offset,
									bw + top_offset);
		if (c->blur_node->width != blur_width ||
			c->blur_node->height != blur_height)
			wlr_scene_blur_set_size(c->blur_node, blur_width, blur_height);
		if (!fx_corner_radii_eq(c->blur_node->corners, blur_radii))
			wlr_scene_blur_set_corner_radii(c->blur_node, blur_radii);
	}
}

struct ivec2 clip_to_hide(Client *c, struct wlr_box *clip_box) {
	int32_t offsetx = 0, offsety = 0, offsetw = 0, offseth = 0;
	struct ivec2 offset = {0, 0, 0, 0};

	/* in overview a scaled window is placed at its mirrored position and may run
	 * off the screen edge; DON'T crop it here (that would shrink its ov_live
	 * scale). The void-frame masks hide the off-desktop overhang instead. */
	if (c->mon && c->mon->isoverview)
		return offset;

	if (!ISSCROLLTILED(c) && !c->animation.tagining && !c->animation.tagouted &&
		!c->animation.tagouting)
		return offset;

	int32_t bottom_out_offset =
		GEZERO(c->animation.current.y + c->animation.current.height -
			   c->mon->m.y - c->mon->m.height);
	int32_t right_out_offset =
		GEZERO(c->animation.current.x + c->animation.current.width -
			   c->mon->m.x - c->mon->m.width);
	int32_t left_out_offset = GEZERO(c->mon->m.x - c->animation.current.x);
	int32_t top_out_offset = GEZERO(c->mon->m.y - c->animation.current.y);

	// Must cast to int, otherwise the calculation loses negative values and
	// the comparisons below break
	int32_t bw = (int32_t)c->bw;

	/*
	  Compute how far the window surface overflows the screen on each of the
	  four sides, so the window can be kept from overflowing the screen.
	  Only start counting the offset once the surface itself overflows —
	  not just when the border does.
	*/
	if (ISSCROLLTILED(c) || c->animation.tagining || c->animation.tagouted ||
		c->animation.tagouting) {
		if (left_out_offset > 0) {
			offsetx = GEZERO(left_out_offset - bw);
			clip_box->x = clip_box->x + offsetx;
			clip_box->width = clip_box->width - offsetx;
		} else if (right_out_offset > 0) {
			offsetw = GEZERO(right_out_offset - bw);
			clip_box->width = clip_box->width - offsetw;
		}

		if (top_out_offset > 0) {
			offsety = GEZERO(top_out_offset - bw);
			clip_box->y = clip_box->y + offsety;
			clip_box->height = clip_box->height - offsety;
		} else if (bottom_out_offset > 0) {
			offseth = GEZERO(bottom_out_offset - bw);
			clip_box->height = clip_box->height - offseth;
		}
	}

	// Offset by which the window surface overflows the screen on each of the four sides
	offset.x = offsetx;
	offset.y = offsety;
	offset.width = offsetw;
	offset.height = offseth;

	if ((clip_box->width + bw <= 0 || clip_box->height + bw <= 0) &&
		(ISSCROLLTILED(c) || c->animation.tagouting || c->animation.tagining)) {
		c->is_clip_to_hide = true;
		client_set_scene_enabled(c, false);
	} else if (c->is_clip_to_hide && VISIBLEON(c, c->mon) &&
			   (!c->is_monocle_hide || !is_monocle_layout(c->mon))) {
		c->is_clip_to_hide = false;
		c->is_monocle_hide = false;
		client_set_scene_enabled(c, true);
	}

	return offset;
}

void client_set_drop_area(Client *c) {
	bool first_draw = false;
	int32_t drop_direction = UNDIR;

	if (!c || !c->mon)
		return;

	if (!c->enable_drop_area_draw && !c->droparea->node.enabled) {
		return;
	}

	if (!c->enable_drop_area_draw && c->droparea->node.enabled) {
		wlr_scene_node_lower_to_bottom(&c->droparea->node);
		wlr_scene_node_set_enabled(&c->droparea->node, false);
		return;
	} else if (c->enable_drop_area_draw && !c->droparea->node.enabled) {
		wlr_scene_node_raise_to_top(&c->droparea->node);
		wlr_scene_node_set_enabled(&c->droparea->node, true);
		first_draw = true;
	}

	int32_t bw = (int32_t)c->bw;
	int32_t client_width = c->geom.width - 2 * bw;
	int32_t client_height = c->geom.height - 2 * bw;

	// Cursor position relative to the window's client area
	double rel_x = cursor->x - c->geom.x - bw;
	double rel_y = cursor->y - c->geom.y - bw;

	struct wlr_box drop_box;

	const Layout *cur_layout = c->mon->pertag->ltidxs[c->mon->pertag->curtag];
	bool dwindle_familiar =
		cur_layout->id == DWINDLE && config.dwindle_drop_simple_split;

	if (dwindle_familiar) {
		bool split_h = c->geom.width >= c->geom.height;
		float ratio = config.dwindle_split_ratio;
		if (split_h) {
			if (rel_x < client_width * 0.5) {
				drop_direction = LEFT;
				drop_box.x = bw;
				drop_box.y = bw;
				drop_box.width = (int32_t)(client_width * ratio);
				drop_box.height = client_height;
			} else {
				drop_direction = RIGHT;
				drop_box.x = bw + (int32_t)(client_width * ratio);
				drop_box.y = bw;
				drop_box.width = client_width - (int32_t)(client_width * ratio);
				drop_box.height = client_height;
			}
		} else {
			if (rel_y < client_height * 0.5) {
				drop_direction = UP;
				drop_box.x = bw;
				drop_box.y = bw;
				drop_box.width = client_width;
				drop_box.height = (int32_t)(client_height * ratio);
			} else {
				drop_direction = DOWN;
				drop_box.x = bw;
				drop_box.y = bw + (int32_t)(client_height * ratio);
				drop_box.width = client_width;
				drop_box.height =
					client_height - (int32_t)(client_height * ratio);
			}
		}
	} else {
		double dist_left = rel_x;
		double dist_right = client_width - rel_x;
		double dist_top = rel_y;
		double dist_bottom = client_height - rel_y;

		if (dist_left <= dist_right && dist_left <= dist_top &&
			dist_left <= dist_bottom) {
			drop_direction = LEFT;
			drop_box.x = bw;
			drop_box.y = bw;
			drop_box.width = client_width / 2;
			drop_box.height = client_height;
		} else if (dist_right <= dist_top && dist_right <= dist_bottom) {
			drop_direction = RIGHT;
			drop_box.x = bw + client_width / 2;
			drop_box.y = bw;
			drop_box.width = client_width / 2;
			drop_box.height = client_height;
		} else if (dist_top <= dist_bottom) {
			drop_direction = UP;
			drop_box.x = bw;
			drop_box.y = bw;
			drop_box.width = client_width;
			drop_box.height = client_height / 2;
		} else {
			drop_direction = DOWN;
			drop_box.x = bw;
			drop_box.y = bw + client_height / 2;
			drop_box.width = client_width;
			drop_box.height = client_height / 2;
		}
	}

	if (!first_draw && c->drop_direction == drop_direction) {
		return;
	}
	c->drop_direction = drop_direction;

	wlr_scene_node_set_position(&c->droparea->node, drop_box.x, drop_box.y);
	wlr_scene_rect_set_size(c->droparea, drop_box.width, drop_box.height);
}

void client_apply_clip(Client *c, float factor) {

	if (c->iskilling || !client_surface(c)->mapped)
		return;

	struct wlr_box clip_box;
	bool should_render_client_surface = false;
	struct ivec2 offset;
	BufferData buffer_data;

	enum corner_location current_corner_location =
		set_client_corner_location(c);

	if (!config.animations && !c->overview_scene_surface &&
		!(c->mon && c->mon->isoverview && config.ov_no_resize)) {
		c->animation.running = false;
		c->need_output_flush = false;
		c->animainit_geom = c->current = c->pending = c->animation.current =
			c->geom;

		client_get_clip(c, &clip_box);

		offset = clip_to_hide(c, &clip_box);

		apply_border(c);
		client_draw_shadow(c);

		client_draw_titlebar(c);

		if (clip_box.width <= 0 || clip_box.height <= 0) {
			return;
		}

		if (!c->overview_scene_surface) {
			wlr_scene_subsurface_tree_set_clip(&c->scene_surface->node,
											   &clip_box);
		}
		client_draw_shield(c, clip_box);

		buffer_set_effect(c, (BufferData){1.0f, 1.0f, clip_box.width,
										  clip_box.height,
										  current_corner_location, true});
		return;
	}

	// Get the window's current animated position rect
	int32_t width, height;
	client_actual_size(c, &width, &height);

	/* overview thumbnail: the surface keeps its full size and is only scaled
	 * down (dest-size) into its cell, so the clip must cover the WHOLE surface
	 * -- clipping to the small cell size would crop the window to its
	 * top-left corner instead of shrinking it */
	bool ov_live = c->mon && c->mon->isoverview && config.ov_no_resize;

	// Compute the actual clip size excluding the border
	struct wlr_box geometry;
	client_get_geometry(c, &geometry);
	clip_box = (struct wlr_box){
		.x = geometry.x,
		.y = geometry.y,
		.width = width,
		.height = height,
	};

	if (client_is_x11(c)) {
		clip_box.x = 0;
		clip_box.y = 0;
	}

	// Check whether the window needs clipping where it overflows the screen,
	// and adjust the clip rect accordingly if so
	offset = clip_to_hide(c, &clip_box);

	// Apply window decorations
	apply_border(c);
	client_draw_shadow(c);

	client_draw_titlebar(c);

	// Skip rendering the window surface if the clip area has shrunk to 0
	if (clip_box.width <= 0 || clip_box.height <= 0) {
		should_render_client_surface = false;
		wlr_scene_node_set_enabled(&c->scene_surface->node, false);
	} else {
		should_render_client_surface = true;
		wlr_scene_node_set_enabled(&c->scene_surface->node, true);
	}

	// No need to run the surface clip/scale effects below
	if (!should_render_client_surface) {
		return;
	}

	// Apply the window surface clip
	if (!c->overview_scene_surface) {
		if (ov_live) {
			/* clip to the full natural surface; the dest-size scaling below
			 * shrinks the whole window into its cell */
			struct wlr_box full_clip = {
				.x = geometry.x,
				.y = geometry.y,
				.width = geometry.width,
				.height = geometry.height,
			};
			if (client_is_x11(c)) {
				full_clip.x = 0;
				full_clip.y = 0;
			}
			wlr_scene_subsurface_tree_set_clip(&c->scene_surface->node,
											   &full_clip);
		} else {
			wlr_scene_subsurface_tree_set_clip(&c->scene_surface->node,
											   &clip_box);
		}
	}
	client_draw_shield(c, clip_box);

	// Get the actual size of the clipped surface, used to compute the scale
	int32_t acutal_surface_width = geometry.width - offset.x - offset.width;
	int32_t acutal_surface_height = geometry.height - offset.y - offset.height;

	if (acutal_surface_width <= 0 || acutal_surface_height <= 0)
		return;

	buffer_data.should_scale = true;
	buffer_data.width = clip_box.width;
	buffer_data.height = clip_box.height;
	buffer_data.corner_location = current_corner_location;
	buffer_data.ov_live = ov_live;

	/* in overview (ov_no_resize) the surface stays full-size and is only
	 * scaled visually, so it must keep its real down-scale even once the
	 * animation has settled (factor == 1.0) -- otherwise the live thumbnail
	 * would snap to full size */
	if (factor == 1.0 && !c->overview_scene_surface && !ov_live) {
		buffer_data.width_scale = 1.0;
		buffer_data.height_scale = 1.0;
	} else {
		buffer_data.width_scale =
			(float)buffer_data.width / acutal_surface_width;
		buffer_data.height_scale =
			(float)buffer_data.height / acutal_surface_height;
	}

	/* overview viewport-edge window: crop the root surface to its visible
	 * fraction (buffer source box, applied in scene_buffer_apply_effect) so
	 * it ends exactly at the panel edge instead of overrunning it */
	buffer_data.crop_active = false;
	buffer_data.crop_clear = false;
	if (ov_live && c->ov_clip_active && c->overview_backup_geom.width > 0 &&
		c->overview_backup_geom.height > 0) {
		buffer_data.crop_active = true;
		buffer_data.crop_l =
			(float)c->ov_clip.x / (float)c->overview_backup_geom.width;
		buffer_data.crop_t =
			(float)c->ov_clip.y / (float)c->overview_backup_geom.height;
		buffer_data.crop_w =
			(float)c->ov_clip.width / (float)c->overview_backup_geom.width;
		buffer_data.crop_h =
			(float)c->ov_clip.height / (float)c->overview_backup_geom.height;
		c->ov_crop_set = true;
	} else if (c->ov_crop_set) {
		buffer_data.crop_clear = true;
		c->ov_crop_set = false;
	}

	buffer_set_effect(c, buffer_data);
}

void client_draw_shield(Client *c, struct wlr_box clip_box) {
	if (!c->shield)
		return;

	if (clip_box.width <= 0 || clip_box.height <= 0) {
		wlr_scene_node_set_enabled(&c->shield->node, false);
		return;
	}

	struct wlr_box surface_relative_geom;
	client_get_clip(c, &surface_relative_geom);

	if (c == grabc || (!ISSCROLLTILED(c) && !c->animation.tagining &&
					   !c->animation.tagouting)) {
		clip_box.x = surface_relative_geom.x;
		clip_box.y = surface_relative_geom.y;
		clip_box.width = c->animation.current.width - 2 * (int32_t)c->bw;
		clip_box.height = c->animation.current.height - 2 * (int32_t)c->bw;
	}

	if (active_capture_count > 0 && c->shield_when_capture) {
		int32_t shield_x =
			clip_box.x - surface_relative_geom.x + (int32_t)c->bw;
		int32_t shield_y =
			clip_box.y - surface_relative_geom.y + (int32_t)c->bw;
		wlr_scene_node_raise_to_top(&c->shield->node);
		wlr_scene_node_set_position(&c->shield->node, shield_x, shield_y);
		wlr_scene_rect_set_size(c->shield, clip_box.width, clip_box.height);
		wlr_scene_node_set_enabled(&c->shield->node, true);
	} else if (c->shield->node.enabled) {
		wlr_scene_node_lower_to_bottom(&c->shield->node);
		wlr_scene_node_set_position(&c->shield->node, 0, 0);
		wlr_scene_rect_set_size(c->shield, c->animation.current.width,
								c->animation.current.height);
		wlr_scene_node_set_enabled(&c->shield->node, false);
	}
}

/* ---- "asteroid" close animation ("fall" is the old name) -------------------------------------------
 *
 * An Asteroids explosion. The closing window is snapshotted, sliced into a
 * grid of tiles, and every tile is flung straight out from the centre and
 * gone inside a few hundred milliseconds.
 *
 * What makes the arcade version read the way it does is what it LACKS: no
 * gravity, no arc, no settling. Debris in that game leaves the wreck in a
 * straight line at constant speed and fades before it reaches anywhere. This
 * used to throw the tiles sideways and pull them down, which reads as a
 * dropped plate rather than something detonating in space, so the downward
 * acceleration is gone and the velocity is radial: each tile's direction is
 * the line from the window's centre through the tile's own centre, which is
 * exactly where a piece of a shattering rock would go.
 *
 * A tile is an ordinary scene tree holding cropped copies of whatever
 * snapshot nodes overlap it (surface buffers via a source box, borders and
 * other rects as smaller rects), so this is pure scene-graph work: it renders
 * identically on the GLES and Vulkan backends and needs no renderer or shader
 * support. That is also why the pieces stay axis-aligned -- a scene buffer
 * cannot be rotated, so fragments cannot tumble the way the vector originals
 * do. The tangential kick below is what stands in for it: neighbouring tiles
 * drift apart as they travel, so the cloud shears instead of expanding like a
 * rigid diagram.
 */
struct FalloutShard {
	struct wlr_scene_tree *tree; /* this tile's cropped pieces */
	int32_t x, y;				 /* tile origin, layout coordinates */
	int32_t w, h;				 /* tile size, for the monitor bounds check */
	float vx, vy;				 /* velocity, px over the whole run */
};

/* Deterministic per-tile noise in [lo,hi) -- a hash of the tile index rather
 * than rand(), so a shatter never depends on global RNG state and repeats
 * identically for the same window/grid. */
static float fallout_jitter(uint32_t seed, float lo, float hi) {
	seed = seed * 1664525u + 1013904223u;
	seed ^= seed >> 16;
	seed *= 0x7feb352du;
	seed ^= seed >> 15;
	return lo + (hi - lo) * ((float)(seed & 0xffffffu) / (float)0xffffffu);
}

/* Copy the part of every leaf under `node` that overlaps `tile` into `dst`,
 * at coordinates relative to the tile's origin. */
static void fallout_slice_tree(struct wlr_scene_node *node,
							   struct wlr_scene_tree *dst,
							   const struct wlr_box *tile) {
	if (!node->enabled)
		return;

	if (node->type == WLR_SCENE_NODE_TREE) {
		struct wlr_scene_tree *tree = wlr_scene_tree_from_node(node);
		struct wlr_scene_node *child;
		wl_list_for_each(child, &tree->children, link)
			fallout_slice_tree(child, dst, tile);
		return;
	}

	int32_t lx = 0, ly = 0;
	if (!wlr_scene_node_coords(node, &lx, &ly))
		return;

	struct wlr_box box = {.x = lx, .y = ly};
	struct wlr_box hit;

	if (node->type == WLR_SCENE_NODE_BUFFER) {
		struct wlr_scene_buffer *src = wlr_scene_buffer_from_node(node);
		if (!src->buffer)
			return;
		box.width = src->dst_width;
		box.height = src->dst_height;
		if (box.width <= 0 || box.height <= 0 ||
			!wlr_box_intersection(&hit, &box, tile))
			return;

		struct wlr_scene_buffer *piece = wlr_scene_buffer_create(dst, NULL);
		if (!piece)
			return;

		/* An unset source box means "the whole buffer"; buffer dimensions are
		 * pre-transform, so a 90/270 rotation swaps them in surface space. */
		struct wlr_fbox sbox = src->src_box;
		if (sbox.width <= 0 || sbox.height <= 0) {
			double bw = src->buffer->width, bh = src->buffer->height;
			if (src->transform & WL_OUTPUT_TRANSFORM_90) {
				double swap = bw;
				bw = bh;
				bh = swap;
			}
			sbox = (struct wlr_fbox){.x = 0, .y = 0, .width = bw, .height = bh};
		}

		/* Map the overlap back into buffer coordinates proportionally. */
		double fx = (double)(hit.x - box.x) / (double)box.width;
		double fy = (double)(hit.y - box.y) / (double)box.height;
		double fw = (double)hit.width / (double)box.width;
		double fh = (double)hit.height / (double)box.height;
		struct wlr_fbox crop = {
			.x = sbox.x + sbox.width * fx,
			.y = sbox.y + sbox.height * fy,
			.width = sbox.width * fw,
			.height = sbox.height * fh,
		};

		wlr_scene_buffer_set_buffer(piece, src->buffer);
		wlr_scene_buffer_set_source_box(piece, &crop);
		wlr_scene_buffer_set_dest_size(piece, hit.width, hit.height);
		wlr_scene_buffer_set_transform(piece, src->transform);
		wlr_scene_buffer_set_filter_mode(piece, src->filter_mode);
		wlr_scene_buffer_set_opacity(piece, src->opacity);
		wlr_scene_node_set_position(&piece->node, hit.x - tile->x,
									hit.y - tile->y);
	} else if (node->type == WLR_SCENE_NODE_RECT) {
		struct wlr_scene_rect *src = wlr_scene_rect_from_node(node);
		box.width = src->width;
		box.height = src->height;
		if (box.width <= 0 || box.height <= 0 ||
			!wlr_box_intersection(&hit, &box, tile))
			return;

		struct wlr_scene_rect *piece =
			wlr_scene_rect_create(dst, hit.width, hit.height, src->color);
		if (!piece)
			return;
		wlr_scene_node_set_position(&piece->node, hit.x - tile->x,
									hit.y - tile->y);
	}
	/* Anything else (shadows, blur nodes) is dropped: it has no meaningful
	 * per-tile crop and the pieces are gone within a few hundred ms. */
}

/* Build the vector break-up for `fadeout_client` from `c`'s geometry.
 *
 * Geometry only: the window's pixels play no part, because the arcade rock is
 * an outline and so is this. What the window contributes is its box -- the
 * rock is inscribed in it, so a wide window comes apart into a wide spray and
 * a small one into a small one. */
static bool init_asteroid_break(Client *fadeout_client, Client *c) {
	struct wlr_box win = c->animation.current;
	if (win.width <= 0 || win.height <= 0)
		return false;

	AsteroidBreak *br = ecalloc(1, sizeof(*br));
	br->tree = wlr_scene_tree_create(layers[LyrFadeOut]);
	if (!br->tree) {
		free(br);
		return false;
	}
	br->mon = c->mon;
	/* The arcade drew in white. Follow the theme's foreground instead: this is
	 * a desktop, and a hard white flash on a themed desk reads as a glitch
	 * rather than as a nod. */
	memcpy(br->color, config.theme.fg_color, sizeof(br->color));

	double ox = win.x + win.width / 2.0, oy = win.y + win.height / 2.0;
	double span = ASTEROIDZ_MIN(win.width, win.height);
	/* How far the pieces get. Two thirds of the smaller dimension is enough to
	 * clear the window's own footprint without still travelling when they have
	 * already faded out. */
	double reach = span * 0.66;

	/* Four rocks, arranged as quarters of the window, plus streaks. Four
	 * because that is what a rock does in the game -- it halves, twice -- and
	 * because a dozen pieces of a window read as confetti rather than as
	 * something breaking. */
	static const double quad[4][2] = {
		{-0.25, -0.25}, {0.25, -0.25}, {-0.25, 0.25}, {0.25, 0.25}};
	int32_t n = 0;
	for (int32_t i = 0; i < 4 && n < AST_MAX_FRAGS; i++) {
		AsteroidFrag *f = &br->frags[n];
		double cx = ox + win.width * quad[i][0];
		double cy = oy + win.height * quad[i][1];
		/* Sized so the four together roughly cover the window: a quarter of
		 * each dimension, taking the smaller so a very wide window does not
		 * produce rocks wider than they are tall. */
		double size = ASTEROIDZ_MIN(win.width, win.height) * 0.28;
		ast_make_frag(f, (uint32_t)(i * 131 + 7), cx, cy, ox, oy, size, reach);
		n++;
	}
	for (int32_t i = 0; n < AST_MAX_FRAGS; i++, n++)
		ast_make_spark(&br->frags[n], (uint32_t)(i * 733 + 29), ox, oy, reach);

	for (int32_t i = 0; i < n; i++) {
		br->frags[i].node = wlr_scene_buffer_create(br->tree, NULL);
		if (!br->frags[i].node) {
			n = i;
			break;
		}
	}
	if (n == 0) {
		wlr_scene_node_destroy(&br->tree->node);
		free(br);
		return false;
	}
	br->nfrags = n;

	fadeout_client->scene = br->tree;
	fadeout_client->rocks = br;
	return true;
}

/* Advance every fragment: drift outward, keep turning, fade.
 *
 * Distance eases out for the same reason the tile version does -- most of the
 * travel in the first third is what makes a burst read as a burst -- but the
 * SPIN is linear. A rock that visibly slows its rotation looks like it is
 * being braked by something, and there is nothing out there to brake it. */
static void asteroid_break_next_tick(Client *c, double t) {
	AsteroidBreak *br = c->rocks;
	if (!br)
		return;
	if (br->drawn && fabs(t - br->last_t) < 1e-6)
		return; /* same instant, already on screen */
	br->last_t = t;
	br->drawn = true;

	/* Opened after the same-instant guard so the zone counts real work only.
	 * Each fragment is re-rasterised with cairo into its own buffer every
	 * frame -- the scene graph cannot rotate a node, so the rotation has to be
	 * baked in per tick. That is the most expensive thing asteroidz does per
	 * frame and, until now, the least measured; the fragment count is attached
	 * because cost scales with it and it is what a budget would be spent on. */
	AZ_ZONE(az_rocks, "asteroid tick");
	AZ_ZONE_VALUE(az_rocks, br->nfrags);
	double travel = 1.0 - (1.0 - t) * (1.0 - t);
	/* Hold full opacity briefly so the break is legible, then go. */
	double alpha = t < 0.25 ? 1.0 : 1.0 - (t - 0.25) / 0.75;
	if (alpha < 0.0)
		alpha = 0.0;

	for (int32_t i = 0; i < br->nfrags; i++) {
		AsteroidFrag *f = &br->frags[i];
		if (!f->node)
			continue;
		double cx = f->cx + f->dx * travel;
		double cy = f->cy + f->dy * travel;

		/* Same rule as the tile version: debris never lands on a screen it did
		 * not come from. */
		struct wlr_box box = {.x = (int32_t)(cx - f->radius),
							  .y = (int32_t)(cy - f->radius),
							  .width = (int32_t)(f->radius * 2),
							  .height = (int32_t)(f->radius * 2)};
		bool trespassing = false;
		Monitor *other = NULL;
		wl_list_for_each(other, &mons, link) {
			struct wlr_box hit;
			if (other == br->mon || !other->wlr_output ||
				!other->wlr_output->enabled)
				continue;
			if (wlr_box_intersection(&hit, &box, &other->m)) {
				trespassing = true;
				break;
			}
		}
		if (trespassing || alpha <= 0.0) {
			wlr_scene_node_set_enabled(&f->node->node, false);
			continue;
		}
		wlr_scene_node_set_enabled(&f->node->node, true);
		ast_frag_render(f, br->color, alpha, f->angle + f->spin * t, cx, cy);
	}
	AZ_ZONE_END(az_rocks);
}

static void asteroid_break_destroy(AsteroidBreak *br) {
	if (!br)
		return;
	for (int32_t i = 0; i < br->nfrags; i++) {
		if (br->frags[i].node)
			wlr_scene_buffer_set_buffer(br->frags[i].node, NULL);
		if (br->frags[i].buffer)
			wlr_buffer_drop(&br->frags[i].buffer->base);
	}
	free(br);
}

/* Build the tile grid for `fadeout_client` from `c`'s current appearance.
 * Returns false if the window can't be sliced, in which case the caller falls
 * back to one of the whole-window close animations. */
static bool init_fallout_shards(Client *fadeout_client, Client *c) {
	int32_t cols = CLAMP_INT(config.fall_cols, 1, 12);
	int32_t rows = CLAMP_INT(config.fall_rows, 1, 12);
	struct wlr_box win = c->animation.current;

	if (win.width <= 0 || win.height <= 0)
		return false;

	struct wlr_scene_tree *snap =
		wlr_scene_tree_snapshot(&c->scene->node, layers[LyrFadeOut]);
	if (!snap)
		return false;
	wlr_scene_node_set_enabled(&snap->node, true);

	struct wlr_scene_tree *root = wlr_scene_tree_create(layers[LyrFadeOut]);
	struct FalloutShard *shards = root ? calloc((size_t)cols * (size_t)rows,
												sizeof(*shards))
									   : NULL;
	if (!shards) {
		if (root)
			wlr_scene_node_destroy(&root->node);
		wlr_scene_node_destroy(&snap->node);
		return false;
	}

	int32_t n = 0;
	for (int32_t row = 0; row < rows; row++) {
		for (int32_t col = 0; col < cols; col++) {
			/* Derive edges from the window bounds so rounding never leaves a
			 * seam or overshoots the last row/column. */
			int32_t x0 = win.x + win.width * col / cols;
			int32_t x1 = win.x + win.width * (col + 1) / cols;
			int32_t y0 = win.y + win.height * row / rows;
			int32_t y1 = win.y + win.height * (row + 1) / rows;
			struct wlr_box tile = {
				.x = x0, .y = y0, .width = x1 - x0, .height = y1 - y0};
			if (tile.width <= 0 || tile.height <= 0)
				continue;

			struct wlr_scene_tree *tree = wlr_scene_tree_create(root);
			if (!tree)
				continue;
			wlr_scene_node_set_position(&tree->node, tile.x, tile.y);
			fallout_slice_tree(&snap->node, tree, &tile);

			/* Straight out from the centre, through this tile's centre.
			 *
			 * Normalised first, so speed is set by the jitter alone rather
			 * than by how far from the middle a tile happens to sit: without
			 * that, corner pieces leave at twice the speed of edge ones and
			 * the cloud comes out diamond-shaped. Distance scales with the
			 * window's own size, so a thumbnail and a fullscreen window blow
			 * up the same way.
			 *
			 * A tile dead on the centre has no direction to go: it gets a
			 * deterministic one instead of a division by zero. */
			uint32_t seed = (uint32_t)(row * cols + col + 1);
			float cx = (float)(tile.x + tile.width / 2) -
					   ((float)win.x + (float)win.width * 0.5f);
			float cy = (float)(tile.y + tile.height / 2) -
					   ((float)win.y + (float)win.height * 0.5f);
			float len = sqrtf(cx * cx + cy * cy);
			if (len < 1.0f) {
				float a = fallout_jitter(seed + 31u, 0.0f, 6.2831853f);
				cx = cosf(a);
				cy = sinf(a);
			} else {
				cx /= len;
				cy /= len;
			}

			/* Reach, as a fraction of the window's smaller dimension: far
			 * enough to clear the wreck, not so far that a fragment is still
			 * visibly travelling when it has already faded out. */
			float span = (float)ASTEROIDZ_MIN(win.width, win.height);
			float speed = span * (0.85f + fallout_jitter(seed + 977u, -0.25f,
														 0.35f));
			/* Perpendicular kick: the shear that stands in for tumbling. */
			float swirl = span * fallout_jitter(seed + 5231u, -0.12f, 0.12f);

			struct FalloutShard *s = &shards[n++];
			s->tree = tree;
			s->x = tile.x;
			s->y = tile.y;
			s->w = tile.width;
			s->h = tile.height;
			s->vx = cx * speed - cy * swirl;
			s->vy = cy * speed + cx * swirl;
		}
	}

	wlr_scene_node_destroy(&snap->node);

	if (n == 0) {
		wlr_scene_node_destroy(&root->node);
		free(shards);
		return false;
	}

	fadeout_client->scene = root;
	fadeout_client->shards = shards;
	fadeout_client->nshards = n;
	return true;
}

/* Advance every tile.
 *
 * Distance eases OUT -- fast at the instant of the break, slowing as the
 * pieces get further away. Constant velocity is what the arcade original
 * does, but it needs the whole run to sell the burst; easing puts most of the
 * travel in the first third, which is what makes this read as an explosion at
 * a duration short enough not to be in the way. */
static void fallout_client_next_tick(Client *c, double t) {
	double fade = find_animation_curve_at(t, OPAFADEOUT);
	double opacity = ASTEROIDZ_MAX(
		config.fadeout_begin_opacity - fade * config.fadeout_begin_opacity, 0);
	double travel = 1.0 - (1.0 - t) * (1.0 - t);

	/* Debris never lands on a screen it did not come from.
	 *
	 * These are scene nodes in a global layer, so a piece thrown past the edge
	 * of its own monitor keeps going and turns up on the NEIGHBOURING one -- a
	 * window closing over here, throwing shrapnel across a second screen that
	 * had nothing to do with it. The scene graph has no per-node clip to lean
	 * on, so the shard is dropped instead.
	 *
	 * Dropped on ENTERING another monitor, not on leaving its own: those are
	 * different tests and only the second one is wrong. A maximised window's
	 * outermost tiles start flush against the edge of their monitor, so
	 * "wholly inside" fails on the first frame and the entire outer ring of
	 * debris blinks out the instant the window closes. Flying off the outside
	 * edge of the desk is fine -- there is nothing out there to pollute. */
	for (int32_t i = 0; i < c->nshards; i++) {
		struct FalloutShard *s = &c->shards[i];
		double dx = s->vx * travel;
		double dy = s->vy * travel;
		int32_t px = s->x + (int32_t)dx, py = s->y + (int32_t)dy;

		struct wlr_box box = {.x = px, .y = py, .width = s->w, .height = s->h};
		bool trespassing = false;
		Monitor *other = NULL;
		wl_list_for_each(other, &mons, link) {
			struct wlr_box hit;
			if (other == c->mon || !other->wlr_output ||
				!other->wlr_output->enabled)
				continue;
			if (wlr_box_intersection(&hit, &box, &other->m)) {
				trespassing = true;
				break;
			}
		}
		if (trespassing) {
			wlr_scene_node_set_enabled(&s->tree->node, false);
			continue;
		}

		wlr_scene_node_set_position(&s->tree->node, px, py);
		if (config.animation_fade_out && !c->nofadeout)
			wlr_scene_node_for_each_buffer(
				&s->tree->node, scene_buffer_apply_opacity, &opacity);
	}
}

void fadeout_client_animation_next_tick(Client *c) {
	if (!c)
		return;

	BufferData buffer_data;

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);

	int32_t passed_time = timespec_to_ms(&now) - c->animation.time_started;
	double animation_passed =
		c->animation.duration
			? (double)passed_time / (double)c->animation.duration
			: 1.0;

	/* The vector break-up owns a tree of its own nodes, each re-rendered per
	 * frame, so it has its own tick and teardown. */
	if (c->rocks) {
		asteroid_break_next_tick(c, ASTEROIDZ_MIN(animation_passed, 1.0));
		if (animation_passed >= 1.0) {
			wl_list_remove(&c->fadeout_link);
			wlr_scene_node_destroy(&c->scene->node);
			asteroid_break_destroy(c->rocks);
			free(c);
		}
		return;
	}

	/* "fall": the window is a grid of independently moving tiles, not one
	 * node, so it has its own tick and teardown. */
	if (c->shards) {
		fallout_client_next_tick(c, ASTEROIDZ_MIN(animation_passed, 1.0));
		if (animation_passed >= 1.0) {
			wl_list_remove(&c->fadeout_link);
			wlr_scene_node_destroy(&c->scene->node);
			free(c->shards);
			free(c);
		}
		return;
	}

	int32_t type = c->animation.action = c->animation.action;
	double factor = find_animation_curve_at(animation_passed, type);

	int32_t width = c->animation.initial.width +
					(c->current.width - c->animation.initial.width) * factor;
	int32_t height = c->animation.initial.height +
					 (c->current.height - c->animation.initial.height) * factor;

	int32_t x = c->animation.initial.x +
				(c->current.x - c->animation.initial.x) * factor;
	int32_t y = c->animation.initial.y +
				(c->current.y - c->animation.initial.y) * factor;

	wlr_scene_node_set_position(&c->scene->node, x, y);

	c->animation.current = (struct wlr_box){
		.x = x,
		.y = y,
		.width = width,
		.height = height,
	};

	double opacity_eased_progress =
		find_animation_curve_at(animation_passed, OPAFADEOUT);

	double percent = config.fadeout_begin_opacity -
					 (opacity_eased_progress * config.fadeout_begin_opacity);

	double opacity = ASTEROIDZ_MAX(percent, 0);

	if (config.animation_fade_out && !c->nofadeout)
		wlr_scene_node_for_each_buffer(&c->scene->node,
									   scene_buffer_apply_opacity, &opacity);

	if ((c->animation_type_close &&
		 strcmp(c->animation_type_close, "zoom") == 0) ||
		(!c->animation_type_close &&
		 strcmp(config.animation_type_close, "zoom") == 0)) {

		buffer_data.width = width;
		buffer_data.height = height;
		buffer_data.width_scale = animation_passed;
		buffer_data.height_scale = animation_passed;

		wlr_scene_node_for_each_buffer(
			&c->scene->node, snap_scene_buffer_apply_effect, &buffer_data);
	}

	if (animation_passed >= 1.0) {
		wl_list_remove(&c->fadeout_link);
		wlr_scene_node_destroy(&c->scene->node);
		free(c);
		c = NULL;
	}
}

void client_animation_next_tick(Client *c) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);

	int32_t passed_time = timespec_to_ms(&now) - c->animation.time_started;
	double animation_passed =
		c->animation.duration
			? (double)passed_time / (double)c->animation.duration
			: 1.0;

	int32_t type = c->animation.action == NONE ? MOVE : c->animation.action;
	double factor = find_animation_curve_at(animation_passed, type);

	/* Fade the backdrop blur in with the open animation instead of popping to
	 * full on the first frame. Fade only the ALPHA and keep strength at 1: a
	 * strength < 1 triggers the per-frame re-blur split (see
	 * fx_vk_render_pass_add_blur), which added latency and made the blurred
	 * backdrop linger before content. The blur node stays enabled, so
	 * steady-state blur behind translucent windows is unaffected. */
	if (c->blur_node && c->animation.action == OPEN) {
		/* Keep the backdrop blur fully present during the open animation so a
		 * translucent window is frosted from the first frame instead of
		 * flashing the sharp wallpaper; the window's own opacity still fades in
		 * over it. Strength stays at 1 (a lower strength forces the costly
		 * per-frame re-blur split, see fx_vk_render_pass_add_blur). */
		wlr_scene_blur_set_strength(c->blur_node, 1.0f);
		wlr_scene_blur_set_alpha(c->blur_node, 1.0f);
	}

	Client *pointer_c = NULL;
	double sx = 0, sy = 0;
	struct wlr_surface *surface = NULL;

	int32_t width = c->animation.initial.width +
					(c->current.width - c->animation.initial.width) * factor;
	int32_t height = c->animation.initial.height +
					 (c->current.height - c->animation.initial.height) * factor;

	int32_t x = c->animation.initial.x +
				(c->current.x - c->animation.initial.x) * factor;
	int32_t y = c->animation.initial.y +
				(c->current.y - c->animation.initial.y) * factor;

	wlr_scene_node_set_position(&c->scene->node, x, y);
	c->animation.current = (struct wlr_box){
		.x = x,
		.y = y,
		.width = width,
		.height = height,
	};

	c->is_pending_open_animation = false;

	/* Direct scanout is decided per-output by wlroots purely from scene-node
	 * geometry overlap, completely bypassing the clip_box logic above -- so a
	 * fullscreen client's buffer can get handed straight to a neighbouring
	 * monitor's CRTC while its node is mid-slide (e.g. tag-switch animation)
	 * and briefly spans both outputs, even though the composited path would
	 * have clipped it correctly. Force scanout off for the duration of any
	 * animation; restored to the user's own noscanout setting once the
	 * client settles back into a steady, un-animated position below. */
	client_set_prevent_scanout(c, true);

	client_apply_clip(c, factor);

	if (animation_passed >= 1.0) {

		// clear the open action state
		// To prevent him from being mistaken that
		// it's still in the opening animation in resize
		c->animation.action = MOVE;

		c->animation.tagining = false;
		c->animation.running = false;
		c->animation.overining = false;

		if (c->animation.tagouting) {
			c->animation.tagouting = false;
			client_set_scene_enabled(c, false);
			c->animation.tagouted = true;
			c->animation.current = c->geom;
		}

		/* A fullscreen client that just settled isn't necessarily stable
		 * enough yet to safely hand its buffer straight to a KMS plane --
		 * give it a short grace period (checked/expired in
		 * client_draw_frame()) before scanout becomes eligible again,
		 * instead of restoring it the instant this tick ends. Skip the
		 * grace for anything that wasn't going to be scanout-eligible
		 * anyway (non-fullscreen, or noscanout already set). */
		if (c->isfullscreen && !c->noscanout)
			c->scanout_grace_until_ms = get_now_in_ms() + 250;
		else
			client_set_prevent_scanout(c, c->noscanout);

		struct wlr_surface *pointer_surf = NULL;
		xytonode(cursor->x, cursor->y, &pointer_surf, &pointer_c, NULL, &sx,
				 &sy);

		/* only re-enter when the cursor is over the client's actual SURFACE:
		 * xytonode also resolves the client for hover over its titlebar TAB,
		 * where sx/sy are tab-local and must not be sent as surface coords */
		surface = pointer_c && pointer_c == c && pointer_surf ? pointer_surf
															  : NULL;

		// avoid game window force grab pointer in overview mode
		if (surface && pointer_c == selmon->sel && !selmon->isoverview) {
			/* Counted like the ones in pointerfocus(): a client cannot tell
			 * which code path re-entered it, so neither should the number. */
			az_pointer_enters++;
			wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
		}

		// end flush in next frame, not the current frame
		c->need_output_flush = false;
	}
}

void init_fadeout_client(Client *c) {

	if (!c->mon || client_is_unmanaged(c))
		return;

	if (!c->scene) {
		return;
	}

	if (c->shield_when_capture && active_capture_count > 0) {
		return;
	}

	if ((c->animation_type_close &&
		 strcmp(c->animation_type_close, "none") == 0) ||
		(!c->animation_type_close &&
		 strcmp(config.animation_type_close, "none") == 0)) {
		return;
	}

	Client *fadeout_client = ecalloc(1, sizeof(*fadeout_client));

	client_set_scene_enabled(c, true);
	client_set_border_color(c, config.bordercolor);
	if (c->overview_scene_surface) {
		wlr_scene_node_destroy(&c->overview_scene_surface->node);
		c->overview_scene_surface = NULL;
	}

	/* Two different animations, not two names for one:
	 *
	 *   asteroid  the window becomes a vector rock and splits, tumbling
	 *   fall      the window's own pixels break into tiles that scatter
	 *
	 * "fall" was what "asteroid" used to be, so it keeps working -- but it now
	 * selects the tile effect it always actually was, which is a perfectly
	 * good thing to want and nothing else does. */
	const char *close_type = c->animation_type_close ? c->animation_type_close
													 : config.animation_type_close;
	bool want_rock = close_type && strcmp(close_type, "asteroid") == 0;
	bool want_fall = close_type && strcmp(close_type, "fall") == 0;

	/* Both builders can decline (a zero-sized window, a failed allocation), in
	 * which case the plain whole-window snapshot below still gives the close a
	 * fade to play. */
	bool built = false;
	if (want_rock)
		built = init_asteroid_break(fadeout_client, c);
	else if (want_fall)
		built = init_fallout_shards(fadeout_client, c);
	if (!built)
		fadeout_client->scene =
			wlr_scene_tree_snapshot(&c->scene->node, layers[LyrFadeOut]);
	client_set_scene_enabled(c, false);

	if (!fadeout_client->scene) {
		free(fadeout_client);
		return;
	}

	fadeout_client->animation.duration = config.animation_duration_close;
	fadeout_client->geom = fadeout_client->current =
		fadeout_client->animainit_geom = fadeout_client->animation.initial =
			c->animation.current;
	fadeout_client->mon = c->mon;
	fadeout_client->animation_type_close = c->animation_type_close;
	fadeout_client->animation.action = CLOSE;
	fadeout_client->bw = c->bw;
	fadeout_client->nofadeout = c->nofadeout;

	// The snap node's coordinates here are relative, so the original
	// coordinates must not be added on top of them — unlike a regular node

	fadeout_client->animation.initial.x = 0;
	fadeout_client->animation.initial.y = 0;

	if ((!c->animation_type_close &&
		 strcmp(config.animation_type_close, "fade") == 0) ||
		(c->animation_type_close &&
		 strcmp(c->animation_type_close, "fade") == 0)) {
		fadeout_client->current.x = 0;
		fadeout_client->current.y = 0;
		fadeout_client->current.width = 0;
		fadeout_client->current.height = 0;
	} else if ((c->animation_type_close &&
				strcmp(c->animation_type_close, "slide") == 0) ||
			   (!c->animation_type_close &&
				strcmp(config.animation_type_close, "slide") == 0)) {
		fadeout_client->current.y =
			c->geom.y + c->geom.height / 2 > c->mon->m.y + c->mon->m.height / 2
				? c->mon->m.height -
					  (c->animation.current.y - c->mon->m.y) // down out
				: c->mon->m.y - c->geom.height;				 // up out
		fadeout_client->current.x = 0; // x unchanged, slide out vertically
	} else {
		fadeout_client->current.y =
			(fadeout_client->geom.height -
			 fadeout_client->geom.height * config.zoom_end_ratio) /
			2;
		fadeout_client->current.x =
			(fadeout_client->geom.width -
			 fadeout_client->geom.width * config.zoom_end_ratio) /
			2;
		fadeout_client->current.width =
			fadeout_client->geom.width * config.zoom_end_ratio;
		fadeout_client->current.height =
			fadeout_client->geom.height * config.zoom_end_ratio;
	}

	fadeout_client->animation.time_started = get_now_in_ms();
	wlr_scene_node_set_enabled(&fadeout_client->scene->node, true);
	wl_list_insert(&fadeout_clients, &fadeout_client->fadeout_link);

	// Request a screen refresh
	request_fresh_all_monitors();
}

void client_commit(Client *c) {
	c->current = c->pending; // Set the animation's end position

	if (c->animation.should_animate) {
		if (!c->animation.running) {
			c->animation.current = c->animainit_geom;
		}

		c->animation.initial = c->animainit_geom;
		c->animation.time_started = get_now_in_ms();

		// Mark the animation as started
		c->animation.running = true;
		c->animation.should_animate = false;
	}
	// Request a screen refresh
	request_fresh_all_monitors();
}

void client_set_pending_state(Client *c) {

	if (!c || c->iskilling)
		return;

	if (!config.animations) {
		c->animation.should_animate = false;
	} else if (config.animations && c->animation.tagining) {
		c->animation.should_animate = true;
	} else if (c == grabc || (!c->is_pending_open_animation &&
							  wlr_box_equal(&c->current, &c->pending))) {
		c->animation.should_animate = false;
	} else {
		c->animation.should_animate = true;
	}

	if (((c->animation_type_open &&
		  strcmp(c->animation_type_open, "none") == 0) ||
		 (!c->animation_type_open &&
		  strcmp(config.animation_type_open, "none") == 0)) &&
		c->animation.action == OPEN) {
		c->animation.duration = 0;
	}

	if (c->istagswitching) {
		c->animation.duration = 0;
		c->istagswitching = 0;
	}

	if (start_drag_window) {
		c->animation.should_animate = false;
		c->animation.duration = 0;
	}

	if (c->isnoanimation) {
		c->animation.should_animate = false;
		c->animation.duration = 0;
	}

	// Start the animation
	client_commit(c);
	c->dirty = true;
}

/* i3-style floating min/max size policy: a floor (float_min_*) and a ceiling
 * (float_max_*, defaulting to the output size) on a floating window's box.
 * Applied on top of the client's own size hints (client_set_size_bound). */
static void clamp_floating_size(Client *c) {
	if (!c || !c->isfloating || c->isfullscreen || c->ismaximizescreen)
		return;

	int32_t maxw = config.float_max_width;
	int32_t maxh = config.float_max_height;
	if (c->mon) {
		if (maxw <= 0)
			maxw = c->mon->w.width;
		if (maxh <= 0)
			maxh = c->mon->w.height;
	}

	if (config.float_min_width > 0 && c->geom.width < config.float_min_width)
		c->geom.width = config.float_min_width;
	if (config.float_min_height > 0 && c->geom.height < config.float_min_height)
		c->geom.height = config.float_min_height;
	if (maxw > 0 && c->geom.width > maxw)
		c->geom.width = maxw;
	if (maxh > 0 && c->geom.height > maxh)
		c->geom.height = maxh;
}

void resize(Client *c, struct wlr_box geo, int32_t interact) {

	// Entry point for animation setup; used to compute some of the animation's
	// initial values. The animation's initial position/size is determined by
	// c->animainit_geom

	if (!c || !c->mon || !client_surface(c)->mapped)
		return;

	struct wlr_box *bbox;
	struct wlr_box clip;

	if (!c->mon)
		return;

	c->need_output_flush = true;
	c->dirty = true;

	// float_geom = c->geom;
	bbox = (interact || c->isfloating || c->isfullscreen) ? &sgeom : &c->mon->w;

	if (is_scroller_layout(c->mon) && (!c->isfloating || c == grabc)) {
		c->geom = geo;
		c->geom.width = ASTEROIDZ_MAX(1 + 2 * (int32_t)c->bw, c->geom.width);
		c->geom.height = ASTEROIDZ_MAX(1 + 2 * (int32_t)c->bw, c->geom.height);
	} else { // this clamps the window so it can't be moved off-screen
		c->geom = geo;
		clamp_floating_size(c); // i3-style floating min/max size policy
		applybounds(
			c,
			bbox); // drop this suggested window size, since it's sometimes huge and breaks tiling
	}

	if (!c->isnosizehint && !c->ismaximizescreen && !c->isfullscreen &&
		c->isfloating) {
		client_set_size_bound(c);
	}

	if (!c->is_pending_open_animation) {
		c->animation.begin_fade_in = false;
	}

	if (c->animation.overining) {
		c->animation.action = OVERVIEW;
	} else if (c->animation.action == OPEN && !c->animation.tagining &&
			   !c->animation.tagouting &&
			   wlr_box_equal(&c->geom, &c->current)) {
		c->animation.action = c->animation.action;
	} else if (c->animation.tagouting) {
		c->animation.duration = config.animation_duration_tag;
		c->animation.action = TAG;
	} else if (c->animation.tagining) {
		c->animation.duration = config.animation_duration_tag;
		c->animation.action = TAG;
	} else if (c->is_pending_open_animation) {
		c->animation.duration = config.animation_duration_open;
		c->animation.action = OPEN;
	} else {
		c->animation.duration = config.animation_duration_move;
		c->animation.action = MOVE;
	}

	// Set the animation's initial position/size
	if (c->animation.tagouting) {
		c->animainit_geom = c->animation.current;
	} else if (c->animation.tagining) {
		c->animainit_geom.height = c->animation.current.height;
		c->animainit_geom.width = c->animation.current.width;
	} else if (c->is_pending_open_animation) {
		set_client_open_animation(c, c->geom);
	} else {
		c->animainit_geom = c->animation.current;
	}

	if (c->isnoborder || c->iskilling) {
		c->bw = 0;
	}

	bool hit_no_border = check_hit_no_border(c);
	if (hit_no_border && config.smartgaps) {
		c->bw = 0;
		c->fake_no_border = true;
	}

	// c->geom is the real window size/position, independent of the transition
	// animation; used for layout calculations
	if (!c->mon->isoverview || !config.ov_no_resize) {
		c->configure_serial = client_set_size(c, c->geom.width - 2 * c->bw,
											  c->geom.height - 2 * c->bw);
	}

	if (c->configure_serial != 0) {
		c->mon->resizing_count_pending++;
	}

	if (c == grabc) {
		c->animation.running = false;
		c->need_output_flush = false;

		c->animainit_geom = c->current = c->pending = c->animation.current =
			c->geom;
		wlr_scene_node_set_position(&c->scene->node, c->geom.x, c->geom.y);

		client_draw_shadow(c);
		apply_border(c);
		client_draw_titlebar(c);
		client_get_clip(c, &clip);
		wlr_scene_subsurface_tree_set_clip(&c->scene_surface->node, &clip);
		client_draw_shield(c, clip);
		return;
	}
	// If this window isn't being slid out for a tag switch, let the
	// animation's end position be the real position/size set above.
	// c->pending determines the animation's endpoint; it's usually set
	// near other callers of resize
	if (!c->animation.tagouting && !c->iskilling) {
		c->pending = c->geom;
	}

	if (c->swallowedby && c->animation.action == OPEN) {
		c->animainit_geom = c->swallowedby->animation.current;
	}

	if (c->swallowing) {
		c->animainit_geom = c->geom;
	}

	if ((c->isglobal || c->isunglobal) && c->isfloating &&
		c->animation.action == TAG) {
		c->animainit_geom = c->geom;
	}

	if (c->scratchpad_switching_mon && c->isfloating) {
		c->animainit_geom = c->geom;
	}

	if (config.animations && config.ov_no_resize && c->mon->isoverview &&
		c != c->mon->sel && c->animation.action == OVERVIEW) {
		set_overview_enter_animation(c);
	}

	if (!config.animations && config.ov_no_resize && c->mon->isoverview) {
		c->animainit_geom = c->geom;
	}

	// Apply the animation settings
	client_set_pending_state(c);

	setborder_color(c);
}

bool client_draw_fadeout_frame(Client *c) {
	if (!c)
		return false;

	fadeout_client_animation_next_tick(c);
	return true;
}

/* Solid or gradient border fill; the gradient starts from the (possibly
 * animated) base color so focus transitions stay smooth. */
void client_set_border_fill(Client *c, const float color[4]) {
	/* c->mon->sel is per-monitor and stays pointing at the last client
	 * focused on that monitor even after global focus (selmon) moves
	 * elsewhere, so it must not be used alone to decide "is this the
	 * focused window" -- that goes stale across a monitor focus switch
	 * and would draw the gradient (focused look) on a window that
	 * get_border_color() already decided is unfocused. */
	if (config.border_gradient && c->mon && selmon && c->mon == selmon &&
		c == selmon->sel && !c->iskilling) {
		float colors[8];
		float origin[2] = {0.5f, 0.5f};
		memcpy(colors, color, sizeof(float) * 4);
		memcpy(colors + 4, config.border_gradient_color2, sizeof(float) * 4);
		wlr_scene_rect_set_gradient(c->border, config.border_gradient_angle,
									1, 1, origin, 2, colors);
	} else {
		if (c->border->has_gradient)
			wlr_scene_rect_set_gradient(c->border, 0, 0, 0, NULL, 0, NULL);
		client_set_border_color(c, color);
	}
}

/* t: 1.0 = focused look, 0.0 = unfocused. Applies shadow dimming and
 * backdrop blur strength together. */
void client_apply_focus_effects(Client *c, float t) {
	float color[4];
	float scale = config.shadows_unfocused_scale +
		(1.0f - config.shadows_unfocused_scale) * t;

	if (c->shadow) {
		memcpy(color, config.shadowscolor, sizeof(color));
		color[3] *= scale;
		wlr_scene_shadow_set_color(c->shadow, color);
	}
	if (c->contact_shadow) {
		memcpy(color, config.shadowscolor_contact, sizeof(color));
		color[3] *= scale;
		wlr_scene_shadow_set_color(c->contact_shadow, color);
	}
	if (c->blur_node && config.blur_unfocused_strength < 1.0f &&
		!(c->animation.running && c->animation.action == OPEN)) {
		float strength = config.blur_unfocused_strength +
			(1.0f - config.blur_unfocused_strength) * t;
		wlr_scene_blur_set_strength(c->blur_node, strength);
	}
	c->opacity_animation.current_effect = t;
}

void client_set_focused_opacity_animation(Client *c) {
	float *border_color = get_border_color(c);
	wlr_scene_node_lower_to_bottom(&c->border->node);

	if (!config.animations) {
		setborder_color(c);
		return;
	}

	c->opacity_animation.duration = config.animation_duration_focus;
	memcpy(c->opacity_animation.target_border_color, border_color,
		   sizeof(c->opacity_animation.target_border_color));
	c->opacity_animation.target_opacity = c->focused_opacity;
	c->opacity_animation.time_started = get_now_in_ms();
	memcpy(c->opacity_animation.initial_border_color,
		   c->opacity_animation.current_border_color,
		   sizeof(c->opacity_animation.initial_border_color));
	c->opacity_animation.initial_opacity = c->opacity_animation.current_opacity;
	c->opacity_animation.initial_effect = c->opacity_animation.current_effect;
	c->opacity_animation.target_effect = 1.0f;

	c->opacity_animation.running = true;
}

void client_set_unfocused_opacity_animation(Client *c) {
	float *border_color = get_border_color(c);
	wlr_scene_node_raise_to_top(&c->border->node);
	if (!config.animations) {
		setborder_color(c);
		return;
	}

	c->opacity_animation.duration = config.animation_duration_focus;
	memcpy(c->opacity_animation.target_border_color, border_color,
		   sizeof(c->opacity_animation.target_border_color));
	// Start opacity animation to unfocused
	c->opacity_animation.target_opacity = c->unfocused_opacity;
	c->opacity_animation.time_started = get_now_in_ms();

	memcpy(c->opacity_animation.initial_border_color,
		   c->opacity_animation.current_border_color,
		   sizeof(c->opacity_animation.initial_border_color));
	c->opacity_animation.initial_opacity = c->opacity_animation.current_opacity;
	c->opacity_animation.initial_effect = c->opacity_animation.current_effect;
	c->opacity_animation.target_effect = 0.0f;

	c->opacity_animation.running = true;
}

bool client_apply_focus_opacity(Client *c) {
	/* Keep the titlebar's (and, in the steady-state branches below, the
	 * border's) focus color in sync with the actually-focused client on
	 * every render. focusclient() doesn't recompute titlebar geometry for
	 * tile layouts (it only re-arranges scroller/monocle, and a plain focus
	 * change sets no need_output_flush), so without this a window's
	 * titlebar/border keeps whatever focus state it had the last time it
	 * was explicitly pushed -- e.g. a cross-monitor focus switch can leave
	 * the previously-focused window's border stuck focused. set_focus and
	 * set_border_fill are dirty-checked, so this is a no-op when unchanged. */
	if (c->titlebar_node) {
		bool tb_focused = (selmon && c == selmon->sel);
		asteroidz_tab_bar_node_set_focus(c->titlebar_node, tb_focused);
		if (c->titlebar_close_node)
			asteroidz_tab_bar_node_set_focus(c->titlebar_close_node,
										 tb_focused);
	}

	// Animate focus transitions (opacity + border color)
	float *border_color = get_border_color(c);
	if (c->isfullscreen) {
		c->opacity_animation.running = false;
		client_set_opacity(c, 1);
	} else if (c->animation.running && c->animation.action == OPEN) {
		c->opacity_animation.running = false;
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);

		int32_t passed_time = timespec_to_ms(&now) - c->animation.time_started;
		double linear_progress =
			c->animation.duration
				? (double)passed_time / (double)c->animation.duration
				: 1.0;

		double opacity_eased_progress =
			find_animation_curve_at(linear_progress, OPAFADEIN);

		float percent = config.animation_fade_in && !c->nofadein
							? opacity_eased_progress
							: 1.0;
		float opacity =
			c == selmon->sel ? c->focused_opacity : c->unfocused_opacity;

		float target_opacity = percent * (1.0 - config.fadein_begin_opacity) +
							   config.fadein_begin_opacity;
		if (target_opacity > opacity) {
			target_opacity = opacity;
		}
		memcpy(c->opacity_animation.current_border_color,
			   c->opacity_animation.target_border_color,
			   sizeof(c->opacity_animation.current_border_color));
		c->opacity_animation.current_opacity = target_opacity;
		client_set_opacity(c, target_opacity);
		client_set_border_fill(c, c->opacity_animation.target_border_color);
	} else if (config.animations && c->opacity_animation.running) {

		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);

		int32_t passed_time =
			timespec_to_ms(&now) - c->opacity_animation.time_started;
		double linear_progress =
			c->opacity_animation.duration
				? (double)passed_time / (double)c->opacity_animation.duration
				: 1.0;

		float eased_progress = find_animation_curve_at(linear_progress, FOCUS);

		float effect = c->opacity_animation.initial_effect +
			(c->opacity_animation.target_effect -
			 c->opacity_animation.initial_effect) *
				(float)eased_progress;
		client_apply_focus_effects(
			c, ASTEROIDZ_MAX(0.0f, ASTEROIDZ_MIN(effect, 1.0f)));

		c->opacity_animation.current_opacity =
			c->opacity_animation.initial_opacity +
			(c->opacity_animation.target_opacity -
			 c->opacity_animation.initial_opacity) *
				eased_progress;
		client_set_opacity(c, c->opacity_animation.current_opacity);

		// Animate border color
		for (int32_t i = 0; i < 4; i++) {
			c->opacity_animation.current_border_color[i] =
				c->opacity_animation.initial_border_color[i] +
				(c->opacity_animation.target_border_color[i] -
				 c->opacity_animation.initial_border_color[i]) *
					eased_progress;
		}
		client_set_border_fill(c, c->opacity_animation.current_border_color);
		if (linear_progress >= 1.0f) {
			c->opacity_animation.running = false;
		} else {
			return true;
		}
	} else if (c == selmon->sel) {
		c->opacity_animation.running = false;
		c->opacity_animation.current_opacity = c->focused_opacity;
		memcpy(c->opacity_animation.current_border_color, border_color,
			   sizeof(c->opacity_animation.current_border_color));
		client_set_opacity(c, c->focused_opacity);
		client_set_border_fill(c, border_color);
	} else {
		c->opacity_animation.running = false;
		c->opacity_animation.current_opacity = c->unfocused_opacity;
		memcpy(c->opacity_animation.current_border_color, border_color,
			   sizeof(c->opacity_animation.current_border_color));
		client_set_opacity(c, c->unfocused_opacity);
		client_set_border_fill(c, border_color);
	}

	return false;
}

bool client_draw_frame(Client *c) {

	if (!c || !client_surface(c)->mapped)
		return false;

	/* overview: window scrolled off the viewport -- keep it hidden (don't let
	 * the draw/clip path re-enable it) until overview exit clears the flag */
	if (c->is_overview_hidden) {
		if (c->scene->node.enabled)
			client_set_scene_enabled(c, false);
		return false;
	}

	/* expire a fullscreen client's post-animation scanout grace period (see
	 * client_animation_next_tick()) independently of need_output_flush,
	 * since a settled client with nothing left to animate stops going
	 * through that path entirely -- nothing else would ever clear this. */
	if (c->scanout_grace_until_ms &&
		get_now_in_ms() >= c->scanout_grace_until_ms) {
		c->scanout_grace_until_ms = 0;
		client_set_prevent_scanout(c, c->noscanout);
	}

	if (!c->need_output_flush) {
		return client_apply_focus_opacity(c);
	}

	if (config.animations && c->animation.running) {
		client_animation_next_tick(c);
	} else {
		wlr_scene_node_set_position(&c->scene->node, c->pending.x,
									c->pending.y);
		c->animation.current = c->animainit_geom = c->animation.initial =
			c->pending = c->current = c->geom;
		client_apply_clip(c, 1.0);
		c->need_output_flush = false;
	}
	client_apply_focus_opacity(c);
	return true;
}
