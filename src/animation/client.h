#include "wlr/util/log.h"
void client_actual_size(Client *c, int32_t *width, int32_t *height) {
	*width = c->animation.current.width - 2 * (int32_t)c->bw;

	*height = c->animation.current.height - 2 * (int32_t)c->bw;
}

void set_rect_size(struct wlr_scene_rect *rect, int32_t width, int32_t height) {
	wlr_scene_rect_set_size(rect, GEZERO(width), GEZERO(height));
}

enum corner_location set_client_corner_location(Client *c) {
	enum corner_location current_corner_location = CORNER_LOCATION_ALL;
	struct wlr_box target_geom =
		config.animations ? c->animation.current : c->geom;
	if (target_geom.x + config.border_radius <= c->mon->m.x) {
		current_corner_location &= ~CORNER_LOCATION_LEFT;
	}
	if (target_geom.x + target_geom.width - config.border_radius >=
		c->mon->m.x + c->mon->m.width) {
		current_corner_location &= ~CORNER_LOCATION_RIGHT;
	}
	if (target_geom.y + config.border_radius <= c->mon->m.y) {
		current_corner_location &= ~CORNER_LOCATION_TOP;
	}
	if (target_geom.y + target_geom.height - config.border_radius >=
		c->mon->m.y + c->mon->m.height) {
		current_corner_location &= ~CORNER_LOCATION_BOTTOM;
	}
	return current_corner_location;
}

bool is_horizontal_stack_layout(Monitor *m) {

	if (m->pertag->curtag &&
		(m->pertag->ltidxs[m->pertag->curtag]->id == TILE ||
		 m->pertag->ltidxs[m->pertag->curtag]->id == DECK))
		return true;

	return false;
}

bool is_horizontal_right_stack_layout(Monitor *m) {

	if (m->pertag->curtag &&
		(m->pertag->ltidxs[m->pertag->curtag]->id == RIGHT_TILE))
		return true;

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

	struct wlr_scene_surface *scene_surface =
		wlr_scene_surface_try_from_buffer(buffer);

	if (scene_surface == NULL)
		return;

	struct wlr_surface *surface = scene_surface->surface;

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
		buffer, corner_radii_from_location(config.border_radius,
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

	wlr_scene_buffer_set_corner_radii(
		buffer, corner_radii_from_location(config.border_radius,
										   buffer_data->corner_location));
}

void buffer_set_effect(Client *c, BufferData data) {

	if (!c || c->iskilling)
		return;

	if (c->animation.tagouting || c->animation.tagouted ||
		c->animation.tagining) {
		data.should_scale = false;
	}

	if (c == grabc)
		data.should_scale = false;

	if (c->isnoradius || c->isfullscreen ||
		(config.no_radius_when_single && c->mon &&
		 c->mon->visible_tiling_clients == 1)) {
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

static void client_draw_one_shadow(Client *c, struct wlr_scene_shadow *shadow,
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

	struct wlr_box shadow_box = {
		.x = pos_x + bwoffset,
		.y = pos_y + bwoffset,
		.width = width + 2 * delta,
		.height = height + 2 * delta,
	};

	struct wlr_box intersection_box;
	wlr_box_intersection(&intersection_box, &client_box, &shadow_box);
	intersection_box.x -= pos_x + bwoffset;
	intersection_box.y -= pos_y + bwoffset;

	struct clipped_region clipped_region = {
		.area = intersection_box,
		.corners = corner_radii_from_location(config.border_radius,
											  corner_location),
	};

	struct wlr_box absolute_shadow_box = {
		.x = shadow_box.x + c->animation.current.x,
		.y = shadow_box.y + c->animation.current.y,
		.width = shadow_box.width,
		.height = shadow_box.height,
	};

	int32_t right_offset, bottom_offset, left_offset, top_offset;

	if (c == grabc) {
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
}

void client_draw_shadow(Client *c) {

	if (c->iskilling || !client_surface(c)->mapped || c->isnoshadow)
		return;

	if (!config.shadows || c->isfullscreen ||
		(!c->isfloating && config.shadow_only_floating)) {
		if (c->shadow->node.enabled)
			wlr_scene_node_set_enabled(&c->shadow->node, false);
		if (c->contact_shadow && c->contact_shadow->node.enabled)
			wlr_scene_node_set_enabled(&c->contact_shadow->node, false);
		return;
	} else {
		if (c->scene_surface->node.enabled && !c->shadow->node.enabled)
			wlr_scene_node_set_enabled(&c->shadow->node, true);
		if (c->contact_shadow && c->scene_surface->node.enabled &&
			c->contact_shadow->node.enabled != (bool)config.shadows_contact)
			wlr_scene_node_set_enabled(&c->contact_shadow->node,
									   config.shadows_contact);
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

	wlr_scene_shadow_set_blur_sigma(c->shadow,
									config.shadows_blur * state_scale);
	client_draw_one_shadow(c, c->shadow,
						   (int32_t)(config.shadows_size * state_scale),
						   (int32_t)(config.shadows_position_x * state_scale),
						   (int32_t)(config.shadows_position_y * state_scale),
						   current_corner_location, hit_no_border);
	if (c->contact_shadow && config.shadows_contact) {
		wlr_scene_shadow_set_blur_sigma(
			c->contact_shadow, config.shadows_contact_blur * state_scale);
		client_draw_one_shadow(
			c, c->contact_shadow,
			(int32_t)ASTEROIDZ_MAX(config.shadows_contact_size * state_scale, 2),
			(int32_t)(config.shadows_contact_position_x * state_scale),
			(int32_t)(config.shadows_contact_position_y * state_scale),
			current_corner_location, hit_no_border);
	}
}

void global_draw_tab_bar(Client *c, int32_t x, int32_t y, int32_t width,
						 int32_t height) {
	if (!c->tab_bar_node)
		return;

	if (height <= 0) {
		asteroidz_tab_bar_node_set_enabled(c->tab_bar_node, false);
	}

	asteroidz_tab_bar_node_set_enabled(c->tab_bar_node, true);
	asteroidz_tab_bar_node_set_position(c->tab_bar_node, x, y);
	asteroidz_tab_bar_node_set_icon(c->tab_bar_node,
								!config.monocle_tab_icons ? NULL
								: c->icon_name		  ? c->icon_name
													  : client_get_appid(c));
	asteroidz_tab_bar_node_set_size(c->tab_bar_node, width, height);
}

void global_draw_group_bar(Client *c, int32_t x, int32_t y, int32_t width,
						   int32_t height) {
	if (!c->group_bar)
		return;

	wlr_scene_node_set_position(&c->group_bar->scene_buffer->node, x, y);
	asteroidz_group_bar_set_size(c->group_bar, width, height);
}

void client_draw_title(Client *c) {

	if (!c || !c->mon || !c->group_bar)
		return;

	if (!c->group_next && !c->group_prev &&
		c->group_bar->scene_buffer->node.enabled) {
		wlr_scene_node_set_enabled(&c->group_bar->scene_buffer->node, false);
		return;
	}

	if (c->is_monocle_hide)
		return;

	if (!c->group_next && !c->group_prev)
		return;

	Client *head = c;
	while (head->group_prev)
		head = head->group_prev;

	int count = 0;
	Client *cur = head;
	while (cur) {
		count++;
		cur = cur->group_next;
	}

	int32_t tab_x = c->animation.current.x;
	int32_t tab_y = c->animation.current.y - config.group_bar_height;
	int32_t tw = c->animation.current.width;
	int32_t th = config.group_bar_height;

	int32_t left_over = c->mon->m.x - tab_x;
	int32_t right_over = tab_x + tw - c->mon->m.x - c->mon->m.width;
	int32_t top_over = c->mon->m.y - tab_y;
	int32_t bottom_over =
		tab_y + config.group_bar_height - c->mon->m.y - c->mon->m.height;

	if (c == grabc || (!ISSCROLLTILED(c) && !c->animation.tagining &&
					   !c->animation.tagouting)) {
		top_over = 0;
		bottom_over = 0;
		left_over = 0;
		right_over = 0;
	}

	if (top_over > 0) {
		tab_y = c->mon->m.y;
		th = config.group_bar_height - top_over;
	}
	if (bottom_over > 0) {
		th = th - bottom_over;
	}
	if (right_over > 0) {
		tw = tw - right_over;
	}
	if (left_over > 0) {
		tab_x = c->mon->m.x;
		tw = tw - left_over;
	}

	if (tw <= 0 || th <= 0) {
		cur = head;
		while (cur) {
			if (cur->group_bar)
				wlr_scene_node_set_enabled(&cur->group_bar->scene_buffer->node,
										   false);
			cur = cur->group_next;
		}
		return;
	} else {
		client_check_tab_node_visible(c);
	}

	int32_t bar_w = tw / count;
	int32_t rem = tw % count;
	int32_t x = tab_x;
	cur = head;

	for (int i = 0; i < count && cur; i++) {
		int32_t w = bar_w + (i < rem ? 1 : 0);
		global_draw_group_bar(cur, x, tab_y, w, th);
		x += w;
		cur = cur->group_next;
	}
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

	if (c == grabc) {
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
	} else {
		if (!c->border->node.enabled) {
			wlr_scene_node_set_enabled(&c->border->node, true);
		}
	}

	bool hit_no_border = check_hit_no_border(c);

	apply_split_border(c, hit_no_border);

	enum corner_location current_corner_location;
	if (c->isfullscreen || (config.no_radius_when_single && c->mon &&
							c->mon->visible_tiling_clients == 1)) {
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

	if (c == grabc) {
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

	struct clipped_region clipped_region = {
		.area = {inner_surface_x, inner_surface_y, inner_surface_width,
				 inner_surface_height},
		.corners = corner_radii_from_location(config.border_radius,
											  current_corner_location),
	};

	wlr_scene_node_set_position(&c->scene_surface->node, c->bw, c->bw);
	wlr_scene_rect_set_size(c->border, rect_width, rect_height);
	wlr_scene_node_set_position(&c->border->node, rect_x, rect_y);
	wlr_scene_rect_set_corner_radii(
		c->border, corner_radii_from_location(config.border_radius,
											  current_corner_location));
	wlr_scene_rect_set_clipped_region(c->border, clipped_region);

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
		bool blur_cached = config.blur_optimized && !c->isfloating;
		if (c->blur_node->should_only_blur_bottom_layer != blur_cached)
			wlr_scene_blur_set_should_only_blur_bottom_layer(c->blur_node,
															 blur_cached);
		int32_t blur_width = GEZERO(clip_box.width - 2 * bw);
		int32_t blur_height = GEZERO(clip_box.height - 2 * bw);
		struct fx_corner_radii blur_radii = corner_radii_from_location(
			config.border_radius, current_corner_location);

		/* only touch the scene when something changed: this runs on
		 * every animation tick */
		wlr_scene_node_set_position(&c->blur_node->node, bw, bw);
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
		wlr_scene_node_set_enabled(&c->scene->node, false);
	} else if (c->is_clip_to_hide && VISIBLEON(c, c->mon) &&
			   (!c->is_monocle_hide || !is_monocle_layout(c->mon))) {
		c->is_clip_to_hide = false;
		c->is_monocle_hide = false;
		wlr_scene_node_set_enabled(&c->scene->node, true);
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
	} else if (cur_layout->id == TILE || cur_layout->id == DECK ||
			   cur_layout->id == CENTER_TILE || cur_layout->id == RIGHT_TILE) {

		if (c->ismaster) {
			if (c->mon->visible_tiling_clients == 1) {
				if (rel_x < client_width * 0.5) {
					drop_direction = LEFT;
					drop_box.x = bw;
					drop_box.y = bw;
					drop_box.width = client_width / 2;
					drop_box.height = client_height;
				} else {
					drop_direction = RIGHT;
					drop_box.x = bw + client_width / 2;
					drop_box.y = bw;
					drop_box.width = client_width / 2;
					drop_box.height = client_height;
				}
			} else {
				drop_box.x = bw;
				drop_box.y = bw;
				drop_box.width = client_width;
				drop_box.height = client_height;
				drop_direction = UNDIR;
			}
		} else {
			if (rel_y < client_height * 0.5) {
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
	} else if (cur_layout->id == VERTICAL_TILE ||
			   cur_layout->id == VERTICAL_DECK) {
		if (c->ismaster) {
			if (c->mon->visible_tiling_clients == 1) {
				if (rel_y < client_height * 0.5) {
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
			} else {
				drop_box.x = bw;
				drop_box.y = bw;
				drop_box.width = client_width;
				drop_box.height = client_height;
				drop_direction = UNDIR;
			}

		} else {
			if (rel_x < client_width * 0.5) {
				drop_direction = LEFT;
				drop_box.x = bw;
				drop_box.y = bw;
				drop_box.width = client_width / 2;
				drop_box.height = client_height;
			} else {
				drop_direction = RIGHT;
				drop_box.x = bw + client_width / 2;
				drop_box.y = bw;
				drop_box.width = client_width / 2;
				drop_box.height = client_height;
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

	if (!config.animations && !c->overview_scene_surface) {
		c->animation.running = false;
		c->need_output_flush = false;
		c->animainit_geom = c->current = c->pending = c->animation.current =
			c->geom;

		client_get_clip(c, &clip_box);

		offset = clip_to_hide(c, &clip_box);

		apply_border(c);
		client_draw_shadow(c);

		client_draw_title(c);

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

	client_draw_title(c);

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
		wlr_scene_subsurface_tree_set_clip(&c->scene_surface->node, &clip_box);
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

	if (factor == 1.0 && !c->overview_scene_surface) {
		buffer_data.width_scale = 1.0;
		buffer_data.height_scale = 1.0;
	} else {
		buffer_data.width_scale =
			(float)buffer_data.width / acutal_surface_width;
		buffer_data.height_scale =
			(float)buffer_data.height / acutal_surface_height;
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

	/* fade the backdrop blur in with the open animation instead of
	 * popping to full strength on the first frame */
	if (c->blur_node && c->animation.action == OPEN) {
		float blur_p = animation_passed >= 1.0 ? 1.0f
			: (float)ASTEROIDZ_MAX(0.0, ASTEROIDZ_MIN(factor, 1.0));
		wlr_scene_blur_set_strength(c->blur_node, blur_p);
		wlr_scene_blur_set_alpha(c->blur_node, blur_p);
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
			wlr_scene_node_set_enabled(&c->scene->node, false);
			c->animation.tagouted = true;
			c->animation.current = c->geom;
		}

		xytonode(cursor->x, cursor->y, NULL, &pointer_c, NULL, NULL, &sx, &sy);

		surface =
			pointer_c && pointer_c == c ? client_surface(pointer_c) : NULL;

		// avoid game window force grab pointer in overview mode
		if (surface && pointer_c == selmon->sel && !selmon->isoverview) {
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

	wlr_scene_node_set_enabled(&c->scene->node, true);
	client_set_border_color(c, config.bordercolor);
	if (c->overview_scene_surface) {
		wlr_scene_node_destroy(&c->overview_scene_surface->node);
		c->overview_scene_surface = NULL;
	}
	fadeout_client->scene =
		wlr_scene_tree_snapshot(&c->scene->node, layers[LyrFadeOut]);
	wlr_scene_node_set_enabled(&c->scene->node, false);

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
		client_draw_title(c);
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
	if (config.border_gradient && c->mon && c == c->mon->sel &&
		!c->iskilling) {
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
	} else {
		c->opacity_animation.running = false;
		c->opacity_animation.current_opacity = c->unfocused_opacity;
		memcpy(c->opacity_animation.current_border_color, border_color,
			   sizeof(c->opacity_animation.current_border_color));
		client_set_opacity(c, c->unfocused_opacity);
	}

	return false;
}

bool client_draw_frame(Client *c) {

	if (!c || !client_surface(c)->mapped)
		return false;

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
