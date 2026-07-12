static void client_swap_layout_properties(Client *c1, Client *c2) {
	// swap Grid properties
	double grid_col_per = c1->grid_col_per;
	double grid_row_per = c1->grid_row_per;
	int32_t grid_col_idx = c1->grid_col_idx;
	int32_t grid_row_idx = c1->grid_row_idx;

	c1->grid_col_per = c2->grid_col_per;
	c1->grid_row_per = c2->grid_row_per;
	c1->grid_col_idx = c2->grid_col_idx;
	c1->grid_row_idx = c2->grid_row_idx;

	c2->grid_col_per = grid_col_per;
	c2->grid_row_per = grid_row_per;
	c2->grid_col_idx = grid_col_idx;
	c2->grid_row_idx = grid_row_idx;

	// swap Master / Stack properties
	double master_inner_per = c1->master_inner_per;
	double master_mfact_per = c1->master_mfact_per;
	double stack_inner_per = c1->stack_inner_per;

	c1->master_inner_per = c2->master_inner_per;
	c1->master_mfact_per = c2->master_mfact_per;
	c1->stack_inner_per = c2->stack_inner_per;

	c2->master_inner_per = master_inner_per;
	c2->master_mfact_per = master_mfact_per;
	c2->stack_inner_per = stack_inner_per;
}

static void client_swap_monitors_and_tags(Client *c1, Client *c2) {
	Monitor *tmp_mon = c2->mon;
	uint32_t tmp_tags = c2->tags;
	c2->mon = c1->mon;
	c1->mon = tmp_mon;
	c2->tags = c1->tags;
	c1->tags = tmp_tags;
}

static void finish_exchange_arrange_and_focus(Client *c1, Client *c2,
											  Monitor *m1, Monitor *m2) {
	if (m1 != m2) {
		arrange(c1->mon, false, false);
		arrange(c2->mon, false, false);
	} else {
		arrange(c1->mon, false, false);
	}
	wl_list_safe_reinsert_next(&c1->flink, &c2->flink);

	if (config.warpcursor)
		warp_cursor(c1);
}

void client_tile_resize(Client *c, struct wlr_box geo, int32_t interact) {
	if (!ISFAKETILED(c))
		return;

	if (!c->mon->isoverview && !c->isfullscreen &&
		(c->group_next || c->group_prev)) {
		geo.y = geo.y + config.group_bar_height;
		geo.height -= config.group_bar_height;
	}

	if (config.enable_titlebar && !c->isfullscreen && client_wants_ssd(c)) {
		/* reserve title-bar space; in the overview scale it to the shrunk window
		 * (a viewport-edge window is sized to its VISIBLE portion -- ov_clip --
		 * so use that as the reference width to keep th uniform) */
		int32_t th = config.titlebar_height;
		if (c->mon->isoverview) {
			float ref_w = (c->ov_clip_active && c->ov_clip.width > 0)
							  ? (float)c->ov_clip.width
							  : (float)c->overview_backup_geom.width;
			th = (int32_t)(th * ((float)geo.width / fmaxf(1.0f, ref_w)));
			/* legibility floor -- must match client_draw_titlebar */
			th = ASTEROIDZ_MAX(th, ASTEROIDZ_MIN(22, config.titlebar_height));
		}
		geo.y = geo.y + th;
		geo.height -= th;
	} else if (is_monocle_layout(c->mon) && !c->isfullscreen &&
			   !c->mon->isoverview &&
			   (c->mon->visible_fake_tiling_clients > 1 ||
				client_wants_ssd(c))) {
		/* >1 windows: the shared segment ROW is layout furniture and appears
		 * for every window (incl. CSD ones); a lone CSD window has no row and
		 * gets no per-window tab, so nothing to reserve for */
		geo.y = geo.y + config.titlebar_height;
		geo.height -= config.titlebar_height;
	}

	if ((!c->isfullscreen && !c->ismaximizescreen) ||
		is_scroller_layout(c->mon)) {
		resize(c, geo, interact);
	}
}

static uint32_t next_client_id = 0;
uint32_t generate_client_id(void) { return ++next_client_id; }

void client_active(Client *c) {
	uint32_t target;

	if (client_is_unmanaged(c)) {
		focusclient(c, 1);
		return;
	}

	if (c->swallowing || !c->mon)
		return;

	if (c->isminimized) {
		c->is_in_scratchpad = 0;
		c->isnamedscratchpad = 0;
		c->is_scratchpad_show = 0;
		setborder_color(c);
		show_hide_client(c);
		arrange(c->mon, true, false);
		return;
	}

	target = get_tags_first_tag(c->tags);
	view_in_mon(&(Arg){.ui = target}, true, c->mon, true);
	focusclient(c, 1);
}

void client_pending_force_kill(Client *c) {
	if (!c)
		return;
	kill(c->pid, SIGKILL);
}

void client_add_jump_label_node(Client *c) {
	c->jump_label_node =
		asteroidz_jump_label_node_create(c->scene, config.theme);
	wlr_scene_node_lower_to_bottom(&c->jump_label_node->scene_buffer->node);
	wlr_scene_node_set_enabled(&c->jump_label_node->scene_buffer->node, false);
}

void client_add_titlebar(Client *c) {

	if (config.titlebar_height == 0) {
		return;
	}

	/* close button is leftmost: it owns the window's top-left corner */
	AsteroidzNodeData *closedata = ecalloc(1, sizeof(AsteroidzNodeData));
	closedata->node_data = c;
	closedata->type = ASTEROIDZ_TITLEBAR_CLOSE_NODE;

	c->titlebar_close_node = asteroidz_tab_bar_node_create(
		closedata, layers[LyrDecorate], config.theme, 0, 0);
	asteroidz_tab_bar_node_set_enabled(c->titlebar_close_node, false);
	/* same drop shadow as the window itself (tiled scale), so the titlebar
	 * casts a matching shadow */
	asteroidz_tab_bar_node_set_shadow(
		c->titlebar_close_node, config.shadows,
		config.shadows_blur * config.shadows_tiled_scale,
		(int32_t)config.shadows_position_y, config.shadowscolor);
	asteroidz_tab_bar_node_set_corner_mask(c->titlebar_close_node,
									   CORNER_LOCATION_TOP_LEFT);
	/* the close button is a small square, not a padded pill: the pill's
	 * usual padding_x (~16px) leaves almost no room for the × glyph at this
	 * width, so it gets ellipsized into "..." */
	asteroidz_tab_bar_node_set_padding(c->titlebar_close_node, 4, 4);
	asteroidz_tab_bar_node_update(c->titlebar_close_node, "×", 1.0);

	/* title tab sits immediately to the close button's right */
	AsteroidzNodeData *nodedata = ecalloc(1, sizeof(AsteroidzNodeData));
	nodedata->node_data = c;
	nodedata->type = ASTEROIDZ_TITLEBAR_NODE;

	c->titlebar_node = asteroidz_tab_bar_node_create(
		nodedata, layers[LyrDecorate], config.theme, 0, 0);
	asteroidz_tab_bar_node_set_enabled(c->titlebar_node, false);
	asteroidz_tab_bar_node_set_shadow(
		c->titlebar_node, config.shadows,
		config.shadows_blur * config.shadows_tiled_scale,
		(int32_t)config.shadows_position_y, config.shadowscolor);
	asteroidz_tab_bar_node_set_corner_mask(c->titlebar_node,
									   CORNER_LOCATION_TOP_RIGHT);
	asteroidz_tab_bar_node_set_text_align_left(c->titlebar_node, true);
	asteroidz_tab_bar_node_set_icon(
		c->titlebar_node, c->icon_name ? c->icon_name : client_get_appid(c));
	asteroidz_tab_bar_node_update(c->titlebar_node, client_get_title(c), 1.0);
}

void client_add_group_bar(Client *c) {

	if (config.group_bar_height <= 0) {
		return;
	}

	uint32_t layer = c->isoverlay						? LyrOverlay
					 : c->isfloating || c->isfullscreen ? LyrTop
					 : c->ismaximizescreen				? LyrMaximize
														: LyrTile;

	c->group_bar = asteroidz_group_bar_create(c, GroupBar, layers[layer],
										  config.theme, 0, 0);
	wlr_scene_node_lower_to_bottom(&c->group_bar->scene_buffer->node);
	wlr_scene_node_set_enabled(&c->group_bar->scene_buffer->node, false);
	asteroidz_group_bar_update(c->group_bar, client_get_title(c),
						   c->mon	? c->mon->wlr_output->scale
						   : selmon ? selmon->wlr_output->scale
									: 1.0f);
}

void client_focus_group_member(Client *c) {
	if (!c->group_prev && !c->group_next)
		return;

	if (c->isgroupfocusing)
		return;

	Client *head = c;
	while (head->group_prev)
		head = head->group_prev;

	Client *cur_focusing = NULL;
	while (head) {
		if (head->isgroupfocusing) {
			cur_focusing = head;
			break;
		}
		head = head->group_next;
	}

	if (!cur_focusing || !cur_focusing->mon)
		return;

	if (cur_focusing && cur_focusing->mon->isoverview)
		return;

	cur_focusing->isgroupfocusing = false;
	c->mon = cur_focusing->mon;
	client_replace(c, cur_focusing, true, false);
	asteroidz_group_bar_set_focus(cur_focusing->group_bar, false);

	c->isgroupfocusing = true;
	asteroidz_group_bar_set_focus(c->group_bar, true);

	client_reparent_group(c);

	focusclient(c, 1);

	arrange(c->mon, false, false);
}

void client_check_tab_node_visible(Client *c) {

	if (!c || !c->mon)
		return;

	Client *head = c;
	while (head->group_prev)
		head = head->group_prev;

	Client *cur = head;
	while (cur) {
		if (!c->mon->isoverview && cur->group_bar &&
			(cur->group_next || cur->group_prev) && TAGMATCH(c, c->mon) &&
			ISNORMAL(c) && !c->isfullscreen &&
			(!is_monocle_layout(c->mon) || c == c->mon->sel)) {
			wlr_scene_node_set_enabled(&cur->group_bar->scene_buffer->node,
									   true);
		} else if (cur->group_bar) {
			wlr_scene_node_set_enabled(&cur->group_bar->scene_buffer->node,
									   false);
		}
		cur = cur->group_next;
	}
}

void client_raise_group(Client *c) {
	if (!c || !c->mon)
		return;

	if (!c->group_prev && !c->group_next)
		return;

	Client *head = c;
	while (head->group_prev)
		head = head->group_prev;

	Client *cur = head;
	while (cur) {
		if (cur->group_bar) {
			wlr_scene_node_raise_to_top(&cur->group_bar->scene_buffer->node);
		}
		wlr_scene_node_raise_to_top(&cur->scene->node);
		cur = cur->group_next;
	}
}

void client_reparent_group(Client *c) {
	if (!c || !c->mon)
		return;

	if (!c->group_prev && !c->group_next)
		return;

	int32_t layer = c->isoverlay					   ? LyrOverlay
					: c->isfloating || c->isfullscreen ? LyrTop
					: c->ismaximizescreen			   ? LyrMaximize
													   : LyrTile;

	Client *head = c;
	while (head->group_prev)
		head = head->group_prev;

	Client *cur = head;
	while (cur) {
		if (cur->group_bar) {
			wlr_scene_node_reparent(&cur->group_bar->scene_buffer->node,
									layers[layer]);
		}
		wlr_scene_node_reparent(&cur->scene->node, layers[layer]);
		cur = cur->group_next;
	}
}

void client_handle_decorate_click(AsteroidzGroupBar *gb) {

	if (!gb)
		return;

	if (gb->node_data) {
		Client *c = gb->node_data;
		client_focus_group_member(c);
	}
}

void client_set_group_mon(Client *c, Monitor *m) {
	Client *head = c;
	while (head->group_prev)
		head = head->group_prev;

	Client *cur = head;
	while (cur) {
		client_change_mon(cur, m);
		cur = cur->group_next;
	}
}

void client_set_group_config(Client *c) {
	Client *head = c;
	while (head->group_prev)
		head = head->group_prev;

	Client *cur = head;
	while (cur) {
		asteroidz_jump_label_node_apply_config(cur->jump_label_node,
										   &config.theme);
		asteroidz_tab_bar_node_apply_config(cur->titlebar_node, &config.theme);
		asteroidz_tab_bar_node_set_shadow(
			cur->titlebar_node, config.shadows,
			config.shadows_blur * config.shadows_tiled_scale,
			(int32_t)config.shadows_position_y, config.shadowscolor);
		asteroidz_tab_bar_node_apply_config(cur->titlebar_close_node,
										&config.theme);
		asteroidz_tab_bar_node_set_shadow(
			cur->titlebar_close_node, config.shadows,
			config.shadows_blur * config.shadows_tiled_scale,
			(int32_t)config.shadows_position_y, config.shadowscolor);
		asteroidz_tab_bar_node_set_padding(cur->titlebar_close_node, 4, 4);
		wlr_scene_rect_set_color(cur->droparea, config.dropcolor);
		wlr_scene_rect_set_color(cur->splitindicator[0], config.splitcolor);
		wlr_scene_rect_set_color(cur->splitindicator[1], config.splitcolor);
		asteroidz_group_bar_apply_config(cur->group_bar, &config.theme);
		cur = cur->group_next;
	}
}

void client_group_detach(Client *c) {
	if (c->group_prev)
		c->group_prev->group_next = c->group_next;
	if (c->group_next)
		c->group_next->group_prev = c->group_prev;
	c->group_prev = NULL;
	c->group_next = NULL;
	c->isgroupfocusing = false;
	c->group_locked = false;
}

void client_group_replace(Client *old, Client *new) {
	client_group_detach(new);

	new->group_prev = old->group_prev;
	new->group_next = old->group_next;
	if (old->group_prev)
		old->group_prev->group_next = new;
	if (old->group_next)
		old->group_next->group_prev = new;
	old->group_prev = NULL;
	old->group_next = NULL;

	if (!new->group_prev && !new->group_next) {
		new->isgroupfocusing = false;
		new->group_locked = false;
	} else {
		new->isgroupfocusing = old->isgroupfocusing;
		new->group_locked = old->group_locked;
	}
	old->group_locked = false;
}
