
typedef struct {
	float x, y, w, h;
} OvPlacedRect;

typedef struct {
	float x, y;
} OvPoint;

typedef struct {
	Client *c;
	float orig_w;
	float orig_h;
	float area;
} OvLayoutItem;

static int compare_layout_items(const void *a, const void *b) {
	float area_a = ((const OvLayoutItem *)a)->area;
	float area_b = ((const OvLayoutItem *)b)->area;
	if (area_a < area_b)
		return 1;
	if (area_a > area_b)
		return -1;
	return 0;
}

static bool try_place(OvPlacedRect *placed, int placed_cnt, float w, float h,
					  float gap, float avail_w, float avail_h,
					  OvPlacedRect *out, OvPoint *cands, OvPoint *feas) {
	int cand_cnt = 0;
	cands[cand_cnt++] = (OvPoint){0.0f, 0.0f};

	for (int i = 0; i < placed_cnt; i++) {
		OvPlacedRect p = placed[i];
		cands[cand_cnt++] = (OvPoint){p.x + p.w + gap, p.y};
		cands[cand_cnt++] = (OvPoint){p.x, p.y + p.h + gap};
		cands[cand_cnt++] = (OvPoint){p.x + p.w + gap, p.y + p.h + gap};
	}

	int unique_cnt = 0;
	for (int i = 0; i < cand_cnt; i++) {
		bool dup = false;
		for (int j = 0; j < unique_cnt; j++) {
			if (fabs(cands[i].x - cands[j].x) < 0.5f &&
				fabs(cands[i].y - cands[j].y) < 0.5f) {
				dup = true;
				break;
			}
		}
		if (!dup)
			cands[unique_cnt++] = cands[i];
	}
	cand_cnt = unique_cnt;

	int feas_cnt = 0;
	for (int i = 0; i < cand_cnt; i++) {
		float cx = cands[i].x;
		float cy = cands[i].y;

		if (cx < 0 || cy < 0 || cx + w > avail_w || cy + h > avail_h)
			continue;

		bool overlap = false;
		for (int j = 0; j < placed_cnt; j++) {
			OvPlacedRect p = placed[j];
			if (!(cx + w + gap <= p.x || cx >= p.x + p.w + gap ||
				  cy + h + gap <= p.y || cy >= p.y + p.h + gap)) {
				overlap = true;
				break;
			}
		}
		if (!overlap) {
			feas[feas_cnt++] = (OvPoint){cx, cy};
		}
	}

	if (feas_cnt == 0)
		return false;

	int best = 0;
	for (int i = 1; i < feas_cnt; i++) {
		if (feas[i].y < feas[best].y ||
			(fabs(feas[i].y - feas[best].y) < 0.5f &&
			 feas[i].x < feas[best].x)) {
			best = i;
		}
	}

	out->x = feas[best].x;
	out->y = feas[best].y;
	out->w = w;
	out->h = h;
	return true;
}

void overview_scale(Monitor *m) {
	int32_t target_gappo = config.overviewgappo;
	int32_t target_gappi = config.overviewgappi;

	int orig_n = m->visible_clients;
	if (orig_n == 0)
		return;

	OvLayoutItem *items = calloc(orig_n, sizeof(OvLayoutItem));
	if (!items)
		return;

	int n = 0;
	Client *c;
	wl_list_for_each(c, &clients, link) {
		if (c->mon != m)
			continue;
		if (VISIBLEON(c, m) && !c->isunglobal && !client_is_x11_popup(c)) {
			items[n].c = c;
			float w = c->overview_backup_geom.width;
			float h = c->overview_backup_geom.height;
			if (w <= 0 || h <= 0) {
				w = 100.0f;
				h = 100.0f;
			}
			items[n].orig_w = w;
			items[n].orig_h = h;
			items[n].area = w * h;
			n++;
		}
	}

	if (n == 0) {
		free(items);
		return;
	}

	qsort(items, n, sizeof(OvLayoutItem), compare_layout_items);

	float max_avail_w = fmaxf(1.0f, m->w.width - 2 * target_gappo);
	float max_avail_h = fmaxf(1.0f, m->w.height - 2 * target_gappo);

	int max_points = 1 + 3 * n;
	OvPlacedRect *placed = calloc(n, sizeof(OvPlacedRect));
	OvPoint *cands = calloc(max_points, sizeof(OvPoint));
	OvPoint *feas = calloc(max_points, sizeof(OvPoint));

	if (!placed || !cands || !feas) {
		free(items);
		free(placed);
		free(cands);
		free(feas);
		return;
	}

	float low = 0.0f, high = 1.0f, best_s = 0.0f;
	for (int iter = 0; iter < 50; iter++) {
		float mid = (low + high) / 2.0f;
		bool ok = true;
		int placed_cnt = 0;

		for (int k = 0; k < n; k++) {
			float w = items[k].orig_w * mid;
			float h = items[k].orig_h * mid;
			OvPlacedRect out;
			if (!try_place(placed, placed_cnt, w, h, (float)target_gappi,
						   max_avail_w, max_avail_h, &out, cands, feas)) {
				ok = false;
				break;
			}
			placed[placed_cnt++] = out;
		}

		if (ok) {
			best_s = mid;
			low = mid;
		} else {
			high = mid;
		}
	}

	if (best_s > 0.0f) {
		int placed_cnt = 0;

		for (int k = 0; k < n; k++) {
			float w = items[k].orig_w * best_s;
			float h = items[k].orig_h * best_s;
			OvPlacedRect out;
			try_place(placed, placed_cnt, w, h, (float)target_gappi,
					  max_avail_w, max_avail_h, &out, cands, feas);
			placed[placed_cnt++] = out;
		}

		if (n > 1) {
			float grid_box_w = 0;
			for (int k = 0; k < n - 1; k++) {
				float r = placed[k].x + placed[k].w;
				if (r > grid_box_w)
					grid_box_w = r;
			}

			OvPlacedRect *last = &placed[n - 1];
			float max_x = grid_box_w - last->w;

			if (max_x > last->x) {
				for (int k = 0; k < n - 1; k++) {
					OvPlacedRect p = placed[k];
					if (!(last->y + last->h + target_gappi <= p.y ||
						  last->y >= p.y + p.h + target_gappi)) {
						if (p.x > last->x) {
							float limit = p.x - target_gappi - last->w;
							if (limit < max_x) {
								max_x = limit;
							}
						}
					}
				}

				if (max_x > last->x) {
					last->x += (max_x - last->x) / 2.0f;
				}
			}
		}

		float box_w = 0, box_h = 0;
		for (int k = 0; k < n; k++) {
			float r = placed[k].x + placed[k].w;
			float b = placed[k].y + placed[k].h;
			if (r > box_w)
				box_w = r;
			if (b > box_h)
				box_h = b;
		}

		float dx = (max_avail_w - box_w) / 2.0f;
		float dy = (max_avail_h - box_h) / 2.0f;
		float base_x = m->w.x + target_gappo + dx;
		float base_y = m->w.y + target_gappo + dy;

		// collect target geometry for all clients, then call client_tile_resize once at the end
		struct wlr_box overview_boxes[n]; // C99 VLA, valid when n > 0
		for (int k = 0; k < n; k++) {
			float w = items[k].orig_w * best_s;
			float h = items[k].orig_h * best_s;
			int ix = (int)(base_x + placed[k].x + 0.5f);
			int iy = (int)(base_y + placed[k].y + 0.5f);
			int iw = (int)(ix + w + 0.5f) - ix;
			int ih = (int)(iy + h + 0.5f) - iy;
			overview_boxes[k] = (struct wlr_box){ix, iy, iw, ih};
		}

		for (int k = 0; k < n; k++) {
			client_tile_resize(items[k].c, overview_boxes[k], 0);
		}
	}

	free(items);
	free(placed);
	free(cands);
	free(feas);
}

void overview_resize(Monitor *m) {
	int32_t target_gappo = config.overviewgappo;
	int32_t target_gappi = config.overviewgappi;
	float single_width_ratio = 0.7f;
	float single_height_ratio = 0.8f;

	int orig_n = m->visible_clients;
	if (orig_n == 0)
		return;

	Client **c_arr = malloc(orig_n * sizeof(Client *));
	if (!c_arr)
		return;

	int n = 0;
	Client *c;
	wl_list_for_each(c, &clients, link) {
		if (c->mon != m)
			continue;
		if (VISIBLEON(c, m) && !c->isunglobal && !client_is_x11_popup(c)) {
			c_arr[n++] = c;
		}
	}

	if (n == 0) {
		free(c_arr);
		return;
	}

	// temporarily store each client's target geometry
	struct wlr_box boxes[n]; // C99 VLA

	if (n == 1) {
		int32_t cw = (m->w.width - 2 * target_gappo) * single_width_ratio;
		int32_t ch = (m->w.height - 2 * target_gappo) * single_height_ratio;
		boxes[0].x = m->w.x + (m->w.width - cw) / 2;
		boxes[0].y = m->w.y + (m->w.height - ch) / 2;
		boxes[0].width = cw;
		boxes[0].height = ch;
	} else if (n == 2) {
		int32_t cw = (m->w.width - 2 * target_gappo - target_gappi) / 2;
		int32_t ch = (m->w.height - 2 * target_gappo) * 0.65f;

		boxes[0].x = m->w.x + target_gappo;
		boxes[0].y = m->w.y + (m->w.height - ch) / 2 + target_gappo;
		boxes[0].width = cw;
		boxes[0].height = ch;

		boxes[1].x = m->w.x + cw + target_gappo + target_gappi;
		boxes[1].y = m->w.y + (m->w.height - ch) / 2 + target_gappo;
		boxes[1].width = cw;
		boxes[1].height = ch;
	} else {
		int32_t cols = 1;
		while (cols * cols < n)
			cols++;
		int32_t rows = (n + cols - 1) / cols;

		int32_t ch =
			(m->w.height - 2 * target_gappo - (rows - 1) * target_gappi) / rows;
		int32_t cw =
			(m->w.width - 2 * target_gappo - (cols - 1) * target_gappi) / cols;

		if (ch < 1)
			ch = 1;
		if (cw < 1)
			cw = 1;

		int32_t overcols = n % cols;
		int32_t dx = 0;
		if (overcols) {
			dx = (m->w.width - overcols * cw - (overcols - 1) * target_gappi) /
					 2 -
				 target_gappo;
		}

		for (int i = 0; i < n; i++) {
			int32_t cx = m->w.x + (i % cols) * (cw + target_gappi);
			int32_t cy = m->w.y + (i / cols) * (ch + target_gappi);
			if (overcols && i >= n - overcols)
				cx += dx;

			boxes[i].x = cx + target_gappo;
			boxes[i].y = cy + target_gappo;
			boxes[i].width = cw;
			boxes[i].height = ch;
		}
	}

	// apply all geometry changes at once via client_tile_resize
	for (int k = 0; k < n; k++) {
		client_tile_resize(c_arr[k], boxes[k], 0);
	}

	free(c_arr);
}

void create_jump_hints(Monitor *m) {
	const char jump_labels[] = "HJKLASDFGQWERTYUIOPZXCVBNM";
	int label_idx = 0;
	Client *c;

	wl_list_for_each(c, &clients, link) {
		if (VISIBLEON(c, m) && !c->isunglobal && !client_is_x11_popup(c)) {
			if (label_idx >= 26)
				break;
			char c_char = jump_labels[label_idx];
			c->jump_char = c_char;

			// turn the character into a string
			char label_text[2] = {c_char, '\0'};

			asteroidz_jump_label_node_update(c->jump_label_node, label_text, 1.0f);
			wlr_scene_node_set_enabled(&c->jump_label_node->scene_buffer->node,
									   true);
			wlr_scene_node_raise_to_top(
				&c->jump_label_node->scene_buffer->node);
			wlr_scene_node_set_position(
				&c->jump_label_node->scene_buffer->node,
				c->geom.width / 2 - c->jump_label_node->logical_width / 2,
				c->geom.height / 2 - c->jump_label_node->logical_height / 2);
			label_idx++;
		}
	}
}

void begin_jump_mode(Monitor *m) { m->is_jump_mode = 1; }

void finish_jump_mode(Monitor *m) {
	Client *c = NULL;

	if (!m->is_jump_mode) {
		return;
	}

	wl_list_for_each(c, &clients, link) {
		if (VISIBLEON(c, m)) {
			if (c->jump_label_node->scene_buffer->node.enabled) {
				c->jump_char = '\0';
				wlr_scene_node_set_enabled(
					&c->jump_label_node->scene_buffer->node, false);
			}
		}
	}

	m->is_jump_mode = 0;
}

/* macOS-Expose-style dim: a dark scrim covering the whole monitor, on the
 * decoration layer (above the wallpaper, below the window thumbnails on
 * LyrTile) so the spread windows pop against a dimmed desktop. */
void overview_draw_backdrop(Monitor *m) {
	static const float dim_color[4] = {0.0f, 0.0f, 0.0f, 0.9f};
	if (!m->ov_dim) {
		m->ov_dim = wlr_scene_rect_create(layers[LyrDecorate], m->m.width,
										  m->m.height, dim_color);
		if (!m->ov_dim)
			return;
	}
	wlr_scene_rect_set_size(m->ov_dim, m->m.width, m->m.height);
	wlr_scene_node_set_position(&m->ov_dim->node, m->m.x, m->m.y);
	wlr_scene_node_lower_to_bottom(&m->ov_dim->node);
	wlr_scene_node_set_enabled(&m->ov_dim->node, true);
}

void overview_hide_chrome(Monitor *m) {
	/* restore the top layer-shell (DMS bar) hidden while overview was open */
	if (!layers[LyrTop]->node.enabled)
		wlr_scene_node_set_enabled(&layers[LyrTop]->node, true);
	if (m->ov_dim && m->ov_dim->node.enabled)
		wlr_scene_node_set_enabled(&m->ov_dim->node, false);
	if (m->ov_strip_blur && m->ov_strip_blur->node.enabled)
		wlr_scene_node_set_enabled(&m->ov_strip_blur->node, false);
	if (m->ov_strip_bg && m->ov_strip_bg->node.enabled)
		wlr_scene_node_set_enabled(&m->ov_strip_bg->node, false);
	if (m->ov_strip_shadow && m->ov_strip_shadow->node.enabled)
		wlr_scene_node_set_enabled(&m->ov_strip_shadow->node, false);
	for (int32_t i = 0; i < OV_STRIP_WINS; i++) {
		if (m->ov_snap[i]) {
			wlr_scene_node_destroy(&m->ov_snap[i]->node);
			m->ov_snap[i] = NULL;
		}
	}
	for (int32_t i = 0; i < 16; i++) {
		if (m->ov_main_crop[i]) {
			wlr_scene_node_destroy(&m->ov_main_crop[i]->node);
			m->ov_main_crop[i] = NULL;
		}
		if (m->ov_main_bord[i]) {
			wlr_scene_node_destroy(&m->ov_main_bord[i]->node);
			m->ov_main_bord[i] = NULL;
		}
	}
	for (int32_t i = 0; i < 4; i++)
		if (m->ov_void[i] && m->ov_void[i]->node.enabled)
			wlr_scene_node_set_enabled(&m->ov_void[i]->node, false);
	if (m->ov_main_wp && m->ov_main_wp->node.enabled)
		wlr_scene_node_set_enabled(&m->ov_main_wp->node, false);
	if (m->ov_main_shadow && m->ov_main_shadow->node.enabled)
		wlr_scene_node_set_enabled(&m->ov_main_shadow->node, false);
	for (int32_t vi = 0; vi < 4; vi++)
		if (m->ov_vignette[vi] && m->ov_vignette[vi]->node.enabled)
			wlr_scene_node_set_enabled(&m->ov_vignette[vi]->node, false);
	if (m->ov_main_border && m->ov_main_border->node.enabled)
		wlr_scene_node_set_enabled(&m->ov_main_border->node, false);
	if (m->ov_hover_hl && m->ov_hover_hl->node.enabled)
		wlr_scene_node_set_enabled(&m->ov_hover_hl->node, false);
	if (m->ov_main_chevron_l && m->ov_main_chevron_l->node.enabled)
		wlr_scene_node_set_enabled(&m->ov_main_chevron_l->node, false);
	if (m->ov_main_chevron_r && m->ov_main_chevron_r->node.enabled)
		wlr_scene_node_set_enabled(&m->ov_main_chevron_r->node, false);
	for (int32_t i = 0; i < OV_TAG_CELLS; i++) {
		if (m->ov_cell_bg[i] && m->ov_cell_bg[i]->node.enabled)
			wlr_scene_node_set_enabled(&m->ov_cell_bg[i]->node, false);
		if (m->ov_cell_wp[i] && m->ov_cell_wp[i]->node.enabled)
			wlr_scene_node_set_enabled(&m->ov_cell_wp[i]->node, false);
		if (m->ov_cell_shadow[i] && m->ov_cell_shadow[i]->node.enabled)
			wlr_scene_node_set_enabled(&m->ov_cell_shadow[i]->node, false);
		if (m->ov_cell_label[i] &&
			m->ov_cell_label[i]->scene_buffer->node.enabled)
			wlr_scene_node_set_enabled(
				&m->ov_cell_label[i]->scene_buffer->node, false);
	}
}

/* The monitor's wallpaper buffer (background/bottom layer surface), used as the
 * tile background so each tag reads as a scaled-down real screen (KDE/niri
 * style). NULL if no wallpaper is mapped yet. */
static struct wlr_buffer *overview_wallpaper_buffer(Monitor *m) {
	const enum zwlr_layer_shell_v1_layer order[] = {
		ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND,
		ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM,
	};
	for (size_t li = 0; li < 2; li++) {
		LayerSurface *l;
		wl_list_for_each(l, &m->layers[order[li]], link) {
			if (l->layer_surface && l->layer_surface->surface &&
				l->layer_surface->surface->buffer)
				return &l->layer_surface->surface->buffer->base;
		}
	}
	return NULL;
}

/* Draw cell `slot`: the monitor wallpaper scaled into a rounded tile (a
 * scaled-down screen), with the tag-name label overlaid. The tile sits behind
 * the window thumbnails (LyrDecorate < LyrTile); the tag you came from gets a
 * focused (accent) label. Falls back to a dark tile if no wallpaper is up. */
static void overview_draw_cell_label(Monitor *m, int32_t slot, float cx,
									 float cy, float cw, float ch, uint32_t tag,
									 bool current, int32_t hidden) {
	if (slot < 0 || slot >= OV_TAG_CELLS)
		return;

	/* Border ring: scene rects have no border property, so draw the background
	 * slightly larger than the wallpaper -- its edge shows as a border around
	 * the tile. The current tag gets a brighter accent border; when no
	 * wallpaper is up this doubles as the dark fallback fill. */
	float bw = current ? 3.0f : 2.0f; /* active tile gets a heavier accent ring */
	static const float border_normal[4] = {0.30f, 0.30f, 0.36f, 0.85f};
	static const float border_current[4] = {0.98f, 0.74f, 0.42f, 1.0f};
	const float *border = current ? border_current : border_normal;
	int32_t ox = (int32_t)(cx - bw), oy = (int32_t)(cy - bw);
	int32_t ow = (int32_t)(cw + 2 * bw), oh = (int32_t)(ch + 2 * bw);

	/* drop shadow behind the tile for depth (above the dim, below the tile) */
	{
		/* the active tile is lifted with a stronger, softer shadow */
		float shadow_color[4] = {0.0f, 0.0f, 0.0f, current ? 0.85f : 0.45f};
		float sigma = current ? 28.0f : 15.0f;
		int32_t pad = (int32_t)ceilf(sigma * 0.5f);
		if (!m->ov_cell_shadow[slot])
			m->ov_cell_shadow[slot] = wlr_scene_shadow_create(
				layers[LyrDecorate], ow + 2 * pad, oh + 2 * pad,
				(int32_t)(18 + pad), sigma, shadow_color);
		if (m->ov_cell_shadow[slot]) {
			wlr_scene_shadow_set_size(m->ov_cell_shadow[slot], ow + 2 * pad,
									  oh + 2 * pad);
			wlr_scene_shadow_set_corner_radius(m->ov_cell_shadow[slot],
											   (int32_t)(18 + pad));
			wlr_scene_shadow_set_blur_sigma(m->ov_cell_shadow[slot], sigma);
			wlr_scene_node_set_position(&m->ov_cell_shadow[slot]->node,
										ox - pad, oy - pad + 4);
			wlr_scene_node_set_enabled(&m->ov_cell_shadow[slot]->node, true);
		}
	}

	if (!m->ov_cell_bg[slot])
		m->ov_cell_bg[slot] =
			wlr_scene_rect_create(layers[LyrDecorate], ow, oh, border);
	if (m->ov_cell_bg[slot]) {
		wlr_scene_rect_set_size(m->ov_cell_bg[slot], ow, oh);
		wlr_scene_rect_set_color(m->ov_cell_bg[slot], border);
		wlr_scene_rect_set_corner_radii(
			m->ov_cell_bg[slot],
			corner_radii_from_location(18, CORNER_LOCATION_ALL));
		wlr_scene_node_set_position(&m->ov_cell_bg[slot]->node, ox, oy);
		wlr_scene_node_set_enabled(&m->ov_cell_bg[slot]->node, true);
		/* keep order: shadow < bg (< wallpaper, raised below) */
		if (m->ov_cell_shadow[slot])
			wlr_scene_node_place_below(&m->ov_cell_shadow[slot]->node,
									   &m->ov_cell_bg[slot]->node);
	}

	/* Wallpaper tile: the monitor wallpaper scaled into the tile (rounded),
	 * drawn on top of the dark fallback so each tag reads as a mini-screen.
	 * Non-current tiles are dimmed a touch so the active tag stands out. */
	struct wlr_buffer *wp = overview_wallpaper_buffer(m);
	if (wp) {
		if (!m->ov_cell_wp[slot])
			m->ov_cell_wp[slot] =
				wlr_scene_buffer_create(layers[LyrDecorate], wp);
		else
			wlr_scene_buffer_set_buffer(m->ov_cell_wp[slot], wp);
		if (m->ov_cell_wp[slot]) {
			wlr_scene_buffer_set_dest_size(m->ov_cell_wp[slot], (int)cw,
										   (int)ch);
			wlr_scene_buffer_set_corner_radii(
				m->ov_cell_wp[slot],
				corner_radii_from_location(16, CORNER_LOCATION_ALL));
			wlr_scene_buffer_set_opacity(m->ov_cell_wp[slot],
										 current ? 1.0f : 0.55f);
			wlr_scene_node_set_position(&m->ov_cell_wp[slot]->node, (int)cx,
										(int)cy);
			wlr_scene_node_raise_to_top(&m->ov_cell_wp[slot]->node);
			wlr_scene_node_set_enabled(&m->ov_cell_wp[slot]->node, true);
		}
	} else if (m->ov_cell_wp[slot]) {
		wlr_scene_node_set_enabled(&m->ov_cell_wp[slot]->node, false);
	}

	if (!m->ov_cell_label[slot]) {
		m->ov_cell_label[slot] =
			asteroidz_jump_label_node_create(layers[LyrOverlay], config.pilldata);
		/* overview tag labels: 1.25x larger and bold vs the normal pill font */
		if (m->ov_cell_label[slot]) {
			PangoFontDescription *fd = pango_font_description_from_string(
				config.pilldata.font_desc ? config.pilldata.font_desc
										  : "monospace Bold 16");
			pango_font_description_set_weight(fd, PANGO_WEIGHT_BOLD);
			int32_t sz = pango_font_description_get_size(fd);
			if (sz <= 0)
				sz = 16 * PANGO_SCALE;
			pango_font_description_set_size(fd, (int)(sz * 1.25f));
			char *s = pango_font_description_to_string(fd);
			g_free(m->ov_cell_label[slot]->font_desc);
			m->ov_cell_label[slot]->font_desc = g_strdup(s);
			g_free(s);
			pango_font_description_free(fd);
		}
	}
	if (m->ov_cell_label[slot]) {
		char name[64], text[96];
		tag_display_name(m, tag, name, sizeof(name));
		/* append a "+N" badge for windows scrolled off the viewport (scroller) */
		if (hidden > 0)
			snprintf(text, sizeof(text), "%s   +%d", name, hidden);
		else
			snprintf(text, sizeof(text), "%s", name);
		asteroidz_jump_label_node_set_focus(m->ov_cell_label[slot], current);
		asteroidz_jump_label_node_update(m->ov_cell_label[slot], text, 1.0f);
		int32_t lw = m->ov_cell_label[slot]->logical_width;
		int32_t lx = (int)(cx + (cw - lw) / 2.0f);
		int32_t ly = (int)(cy + 4);
		wlr_scene_node_set_position(
			&m->ov_cell_label[slot]->scene_buffer->node, lx, ly);
		wlr_scene_node_set_enabled(
			&m->ov_cell_label[slot]->scene_buffer->node, true);
		wlr_scene_node_raise_to_top(
			&m->ov_cell_label[slot]->scene_buffer->node);
	}
}

/* macOS-Mission-Control-style overview: one cell per tag, each showing that
 * tag's real windows (live surfaces, scaled down) in their actual layout, so
 * the overview reflects tags-and-their-layout rather than a single clump.
 *
 * Each cell shows the tag's MONITOR VIEWPORT (what is on screen right now),
 * scaled uniformly monitor->cell -- so window sizes stay consistent across
 * every tag instead of a lone window ballooning to fill its cell or a
 * scroller's off-screen row shrinking everything to nothing. Windows that are
 * scrolled fully off the viewport (scroller layout) are dropped and counted
 * into a "+N" badge on the tag label rather than crammed into the thumbnail. */
/* Lay out the previewed tag's windows in the big area. Split out of
 * overview_tags so horizontal scrolling can re-run just this (cheap)
 * instead of a full arrange (which would rebuild every snapshot). */
void overview_arrange_main(Monitor *m, bool instant) {
	float main_x = m->ov_main_x, main_y = m->ov_main_y;
	float main_w = m->ov_main_w, main_h = m->ov_main_h;
	uint32_t main_tag = m->ov_main_tag;
	Client *c;

	/* Monocle tag: windows are stacked (all at the same spot), so the faithful
	 * mirror would show only the top one. Lay them out side by side (expose)
	 * in a grid instead. */
	bool is_mono = (main_tag >= 1 && main_tag <= LENGTH(tags) &&
					m->pertag->ltidxs[main_tag] &&
					m->pertag->ltidxs[main_tag]->id == MONOCLE);
	if (is_mono) {
		m->ov_main_more_l = false;
		m->ov_main_more_r = false;
		m->ov_scroll_tag = 0;
		Client *arr[64];
		int32_t n = 0;
		wl_list_for_each(c, &clients, link) {
			if (c->mon != m || c->isunglobal || c->isminimized ||
				client_is_x11_popup(c))
				continue;
			if (get_tags_first_tag_num(c->tags) != main_tag)
				continue;
			if (n < 64)
				arr[n++] = c;
		}
		int32_t cols = (int32_t)ceilf(sqrtf((float)fmaxf(1, n)));
		int32_t rows = (n + cols - 1) / cols;
		float gap = 18.0f;
		float cw = (main_w - (cols + 1) * gap) / fmaxf(1, cols);
		float ch = (main_h - (rows + 1) * gap) / fmaxf(1, rows);
		for (int32_t i = 0; i < n; i++) {
			c = arr[i];
			if (c->is_overview_hidden) {
				c->is_overview_hidden = false;
				wlr_scene_node_set_enabled(&c->scene->node, true);
			}
			int32_t col = i % cols, row = i / cols;
			float cx = main_x + gap + col * (cw + gap);
			float cy = main_y + gap + row * (ch + gap);
			float gw = fmaxf(1.0f, (float)c->overview_backup_geom.width);
			float gh = fmaxf(1.0f, (float)c->overview_backup_geom.height);
			float s = fminf(cw / gw, ch / gh);
			int32_t bw = (int32_t)fmaxf(1.0f, gw * s);
			int32_t bh = (int32_t)fmaxf(1.0f, gh * s);
			int32_t bx = (int32_t)(cx + (cw - bw) / 2.0f);
			int32_t by = (int32_t)(cy + (ch - bh) / 2.0f);
			client_tile_resize(c, (struct wlr_box){bx, by, bw, bh}, 0);
			if (c->blur_node)
				wlr_scene_blur_set_should_only_blur_bottom_layer(c->blur_node,
																 false);
			if (instant) {
				c->animation.should_animate = false;
				c->animation.running = false;
				c->animainit_geom = c->animation.current = c->current =
					c->pending = c->geom;
			}
			if (c->ov_title)
				wlr_scene_node_set_enabled(&c->ov_title->scene_buffer->node,
										   false);
			int32_t isz =
				(int32_t)fminf(40.0f, fmaxf(20.0f, fminf(bw, bh) * 0.18f));
			const char *icon =
				c->icon_name ? c->icon_name : client_get_appid(c);
			if (!c->ov_icon)
				c->ov_icon = asteroidz_icon_node_create(layers[LyrOverlay]);
			if (c->ov_icon && icon &&
				asteroidz_icon_node_set(c->ov_icon, icon, isz)) {
				wlr_scene_buffer_set_opacity(c->ov_icon->scene_buffer, 0.85f);
				wlr_scene_node_set_position(&c->ov_icon->scene_buffer->node,
											bx + (bw - isz) / 2, by + bh - isz - 6);
				wlr_scene_node_set_enabled(&c->ov_icon->scene_buffer->node,
										   true);
				wlr_scene_node_raise_to_top(&c->ov_icon->scene_buffer->node);
			}
		}
		goto ov_main_chrome;
	}

	/* Faithful mirror of the tag's desktop: scale the monitor viewport (m->w)
	 * uniformly into the big area and place each window at its real on-screen
	 * position (exactly what's on that tag, just smaller). Windows scrolled off
	 * the desktop aren't shown; empty desktop shows the wallpaper backdrop. */
	m->ov_main_more_l = false;
	m->ov_main_more_r = false;
	/* Translate desktop coords into the big area with a UNIFORM scale (no
	 * aspect distortion) and anchor top-left (not centred). Windows keep their
	 * real relative positions and proportions; a window clipped at the screen
	 * edge touches the mirrored-screen edge just like on the real desktop. */
	float sx = fminf(main_w / (float)m->w.width, main_h / (float)m->w.height);
	float sy = sx;
	float sb = sx; /* uniform scale for border thickness */
	float off_x = main_x, off_y = main_y;
	float vl = m->w.x, vt = m->w.y;
	float vr = m->w.x + m->w.width, vb = m->w.y + m->w.height;
	m->ov_vp_x = main_x;
	m->ov_vp_y = main_y;
	m->ov_vp_w = m->w.width * sx;  /* the actual mirrored-screen rect */
	m->ov_vp_h = m->w.height * sx;

	/* scroller: if the tag's windows span wider than the viewport, let the wheel
	 * pan across them (with edge indicators). Shift the viewport by the scroll
	 * offset so off-screen windows slide into the OV desktop. */
	{
		float cmin = 1e9f, cmax = -1e9f;
		Client *sc;
		wl_list_for_each(sc, &clients, link) {
			if (sc->mon != m || sc->isunglobal || sc->isminimized ||
				client_is_x11_popup(sc))
				continue;
			if (get_tags_first_tag_num(sc->tags) != main_tag)
				continue;
			float gx = sc->overview_backup_geom.x;
			float gw = fmaxf(1.0f, (float)sc->overview_backup_geom.width);
			if (gx < cmin) cmin = gx;
			if (gx + gw > cmax) cmax = gx + gw;
		}
		if (cmax - cmin > (float)m->w.width + 2.0f) {
			if (m->ov_scroll_tag != main_tag) { /* new tag -> reset offset */
				m->ov_scroll_tag = main_tag;
				m->ov_main_scroll = 0.0f;
			}
			float smin = cmin - vl, smax = cmax - vr;
			m->ov_main_scroll = fmaxf(smin, fminf(m->ov_main_scroll, smax));
			vl += m->ov_main_scroll;
			vr += m->ov_main_scroll;
			m->ov_main_more_l = (cmin < vl - 1.0f);
			m->ov_main_more_r = (cmax > vr + 1.0f);
		} else {
			m->ov_scroll_tag = 0;
		}
	}
	/* cropped-buffer pool for partially-visible (edge) windows, rebuilt each
	 * layout; a live node can't be cropped cleanly, so those use a source-box
	 * crop like the strip tiles instead */
	for (int32_t s = 0; s < 16; s++) {
		if (m->ov_main_crop[s]) {
			wlr_scene_node_destroy(&m->ov_main_crop[s]->node);
			m->ov_main_crop[s] = NULL;
		}
		if (m->ov_main_bord[s]) {
			wlr_scene_node_destroy(&m->ov_main_bord[s]->node);
			m->ov_main_bord[s] = NULL;
		}
	}
	int32_t crop_idx = 0;
	wl_list_for_each(c, &clients, link) {
		if (c->mon != m || c->isunglobal || c->isminimized ||
			client_is_x11_popup(c))
			continue;
		if (get_tags_first_tag_num(c->tags) != main_tag)
			continue;
		float gx = c->overview_backup_geom.x, gy = c->overview_backup_geom.y;
		float gw = fmaxf(1.0f, (float)c->overview_backup_geom.width);
		float gh = fmaxf(1.0f, (float)c->overview_backup_geom.height);
		/* fraction of the window cropped off each edge by the viewport */
		float fl = fmaxf(0.0f, vl - gx) / gw;
		float fr = fmaxf(0.0f, (gx + gw) - vr) / gw;
		float ft = fmaxf(0.0f, vt - gy) / gh;
		float fb = fmaxf(0.0f, (gy + gh) - vb) / gh;
		bool on_vp = (fl + fr < 0.98f && ft + fb < 0.98f);
		bool fully = (fl + fr < 0.01f && ft + fb < 0.01f);
		/* a floating window sits on top and is freely placed on the desktop --
		 * show it whole, don't treat it as a clipped tiled/scroller edge */
		if (c->overview_isfloatingbak) {
			on_vp = true;
			fully = true;
		}
		c->ov_clip_active = false;
		(void)crop_idx;
		(void)sb;

		if (!on_vp) {
			/* off the desktop -> not shown. Disable the title bar too: a hidden
			 * window won't be redrawn, so its bar would linger (the scroll path
			 * re-runs only this, not the full arrange that resets bars) */
			if (c->scene && !c->is_overview_hidden) {
				c->is_overview_hidden = true;
				wlr_scene_node_set_enabled(&c->scene->node, false);
			}
			if (c->ov_icon)
				wlr_scene_node_set_enabled(&c->ov_icon->scene_buffer->node,
										   false);
			if (c->ov_title)
				wlr_scene_node_set_enabled(&c->ov_title->scene_buffer->node,
										   false);
			if (c->titlebar_node)
				asteroidz_tab_bar_node_set_enabled(c->titlebar_node, false);
			if (c->titlebar_close_node)
				asteroidz_tab_bar_node_set_enabled(c->titlebar_close_node,
												   false);
			continue;
		}

		/* on the desktop -> the REAL live window, so it keeps its blur,
		 * translucency and border. A partially-clipped window keeps its full
		 * box and overruns the panel; the void masks drawn after the loop hide
		 * the overhang, so it stops at the panel (mirrored-screen) edge. */
		if (c->is_overview_hidden) {
			c->is_overview_hidden = false;
			wlr_scene_node_set_enabled(&c->scene->node, true);
		}
		int32_t bx = (int32_t)(off_x + (gx - vl) * sx + 0.5f);
		int32_t by = (int32_t)(off_y + (gy - vt) * sy + 0.5f);
		int32_t bw = (int32_t)fmaxf(1.0f, gw * sx);
		int32_t bh = (int32_t)fmaxf(1.0f, gh * sy);
		/* small inset so adjacent windows read as separate tiles, not one block */
		int32_t wgap = 4;
		bx += wgap;
		by += wgap;
		bw = (int32_t)fmaxf(1.0f, bw - 2 * wgap);
		bh = (int32_t)fmaxf(1.0f, bh - 2 * wgap);
		client_tile_resize(c, (struct wlr_box){bx, by, bw, bh}, 0);
		/* blur the OV desktop wallpaper behind translucent windows: it's on
		 * LyrDecorate (not the bottom layer), so a window's blur must sample all
		 * layers below, not only the bottom one */
		if (c->blur_node)
			wlr_scene_blur_set_should_only_blur_bottom_layer(c->blur_node,
															 false);
		if (instant) {
			c->animation.should_animate = false;
			c->animation.running = false;
			c->animainit_geom = c->animation.current = c->current =
				c->pending = c->geom;
		}

		/* title pills removed for a cleaner look -- keep any old one hidden */
		if (c->ov_title)
			wlr_scene_node_set_enabled(&c->ov_title->scene_buffer->node, false);

		/* a small, subtle app icon near the window's bottom edge; fully-visible
		 * windows only (a clipped edge has no room) */
		if (!fully) {
			if (c->ov_icon)
				wlr_scene_node_set_enabled(&c->ov_icon->scene_buffer->node,
										   false);
			continue;
		}
		int32_t isz =
			(int32_t)fminf(40.0f, fmaxf(20.0f, fminf(bw, bh) * 0.18f));
		const char *icon = c->icon_name ? c->icon_name : client_get_appid(c);
		if (!c->ov_icon)
			c->ov_icon = asteroidz_icon_node_create(layers[LyrOverlay]);
		if (c->ov_icon && icon &&
			asteroidz_icon_node_set(c->ov_icon, icon, isz)) {
			wlr_scene_buffer_set_opacity(c->ov_icon->scene_buffer, 0.85f);
			wlr_scene_node_set_position(&c->ov_icon->scene_buffer->node,
										bx + (bw - isz) / 2, by + bh - isz - 6);
			wlr_scene_node_set_enabled(&c->ov_icon->scene_buffer->node, true);
			wlr_scene_node_raise_to_top(&c->ov_icon->scene_buffer->node);
		}
	}

ov_main_chrome:
	/* Fill the area BEYOND the OV desktop with a flat inactive-title-bar colour
	 * (no image), drawn ABOVE the windows so no window can extend past the OV
	 * desktop boundary. One rect covers the whole content region below the strip
	 * with a ROUNDED cut-out for the OV desktop, so the flat surround wraps the
	 * desktop's rounded corners cleanly (four plain rects left dark corner
	 * notches). ov_void[1..3] are unused now; keep them disabled. */
	{
		const float *vc = config.pilldata.bg_color; /* inactive title bar */
		float col[4] = {vc[0], vc[1], vc[2], 1.0f};
		float mmb = m->m.y + m->m.height;
		int32_t sx0 = (int32_t)m->m.x, sy0 = (int32_t)m->ov_avail_y;
		int32_t sw = (int32_t)m->m.width, sh = (int32_t)(mmb - m->ov_avail_y);
		for (int32_t i = 1; i < 4; i++)
			if (m->ov_void[i] && m->ov_void[i]->node.enabled)
				wlr_scene_node_set_enabled(&m->ov_void[i]->node, false);
		if (sw > 0 && sh > 0) {
			if (!m->ov_void[0])
				m->ov_void[0] =
					wlr_scene_rect_create(layers[LyrTile], sw, sh, col);
			if (m->ov_void[0]) {
				wlr_scene_rect_set_size(m->ov_void[0], sw, sh);
				wlr_scene_rect_set_color(m->ov_void[0], col);
				/* a plain (square) rect takes a fast render path that ignores
				 * clipped_region, so give it a tiny outer radius to force the
				 * rounded-rect path (which honours the hole). 2px is invisible
				 * at the screen-edge corners. */
				wlr_scene_rect_set_corner_radii(
					m->ov_void[0],
					corner_radii_from_location(2, CORNER_LOCATION_ALL));
				/* rounded hole where the OV desktop shows through (node-local) */
				struct clipped_region hole = {
					.area = {(int32_t)main_x - sx0, (int32_t)main_y - sy0,
							 (int32_t)main_w, (int32_t)main_h},
					.corners =
						corner_radii_from_location(18, CORNER_LOCATION_ALL),
				};
				wlr_scene_rect_set_clipped_region(m->ov_void[0], hole);
				wlr_scene_node_set_position(&m->ov_void[0]->node, sx0, sy0);
				wlr_scene_node_set_enabled(&m->ov_void[0]->node, true);
				wlr_scene_node_raise_to_top(&m->ov_void[0]->node);
			}
		} else if (m->ov_void[0]) {
			wlr_scene_node_set_enabled(&m->ov_void[0]->node, false);
		}
	}

	/* floating windows sit on top and are shown whole, so lift them back above
	 * the void frame -- otherwise the masks would clip a floating window that
	 * runs past the OV desktop edge */
	wl_list_for_each(c, &clients, link) {
		if (c->mon != m || c->isunglobal || c->isminimized ||
			client_is_x11_popup(c))
			continue;
		if (get_tags_first_tag_num(c->tags) != main_tag)
			continue;
		if (c->overview_isfloatingbak && c->scene && !c->is_overview_hidden)
			wlr_scene_node_raise_to_top(&c->scene->node);
	}

	/* scroll-more indicators: accent pills on the big area's left/right edges
	 * when the scroller has windows off that side */
	{
		static const float acc[4] = {0.98f, 0.74f, 0.42f, 0.9f};
		int32_t ih = 60, iw = 6;
		int32_t iy = (int32_t)(main_y + (main_h - ih) / 2.0f);
		if (!m->ov_main_chevron_l)
			m->ov_main_chevron_l =
				wlr_scene_rect_create(layers[LyrOverlay], iw, ih, acc);
		if (m->ov_main_chevron_l) {
			wlr_scene_rect_set_size(m->ov_main_chevron_l, iw, ih);
			wlr_scene_rect_set_corner_radius(m->ov_main_chevron_l, 3);
			wlr_scene_node_set_position(&m->ov_main_chevron_l->node,
										(int32_t)(main_x + 10), iy);
			wlr_scene_node_set_enabled(&m->ov_main_chevron_l->node,
									   m->ov_main_more_l);
			wlr_scene_node_raise_to_top(&m->ov_main_chevron_l->node);
		}
		if (!m->ov_main_chevron_r)
			m->ov_main_chevron_r =
				wlr_scene_rect_create(layers[LyrOverlay], iw, ih, acc);
		if (m->ov_main_chevron_r) {
			wlr_scene_rect_set_size(m->ov_main_chevron_r, iw, ih);
			wlr_scene_rect_set_corner_radius(m->ov_main_chevron_r, 3);
			wlr_scene_node_set_position(&m->ov_main_chevron_r->node,
										(int32_t)(main_x + main_w - 10 - iw), iy);
			wlr_scene_node_set_enabled(&m->ov_main_chevron_r->node,
									   m->ov_main_more_r);
			wlr_scene_node_raise_to_top(&m->ov_main_chevron_r->node);
		}
	}
}

void overview_tags(Monitor *m) {
	int32_t gappo = config.overviewgappo;
	int32_t gappi = config.overviewgappi;
	uint32_t ntags = LENGTH(tags);
	uint32_t cur_tag = get_tags_first_tag_num(m->ovbk_current_tagset);
	if (cur_tag == 0 || cur_tag > ntags)
		cur_tag = m->pertag->prevtag;

	/* which tags have at least one window (each window under its lowest tag),
	 * plus the tag we're currently on */
	bool used[OV_TAG_CELLS] = {false};
	uint32_t used_tags[OV_TAG_CELLS];
	int32_t uc = 0;
	Client *c;
	wl_list_for_each(c, &clients, link) {
		if (c->mon != m || c->isunglobal || c->isminimized ||
			client_is_x11_popup(c))
			continue;
		/* icon+title (and title bars) are re-enabled only for windows shown in
		 * the main area; disable them here so hidden/other-tag windows can't
		 * leave ghost title bars or icons behind after tag/preview switches */
		if (c->ov_icon)
			wlr_scene_node_set_enabled(&c->ov_icon->scene_buffer->node, false);
		if (c->ov_title)
			wlr_scene_node_set_enabled(&c->ov_title->scene_buffer->node, false);
		if (c->titlebar_node)
			asteroidz_tab_bar_node_set_enabled(c->titlebar_node, false);
		if (c->titlebar_close_node)
			asteroidz_tab_bar_node_set_enabled(c->titlebar_close_node, false);
		uint32_t t = get_tags_first_tag_num(c->tags);
		if (t >= 1 && t <= ntags && t < OV_TAG_CELLS)
			used[t] = true;
	}
	if (cur_tag >= 1 && cur_tag <= ntags && cur_tag < OV_TAG_CELLS)
		used[cur_tag] = true;
	for (uint32_t t = 1; t <= ntags && t < OV_TAG_CELLS; t++)
		if (used[t])
			used_tags[uc++] = t;
	if (uc == 0)
		return;

	/* the main area shows the hovered/previewed tag (falling back to the tag we
	 * were really on); hovering a strip tile updates m->ov_preview_tag */
	uint32_t main_tag = cur_tag;
	if (m->ov_preview_tag) {
		for (int32_t k = 0; k < uc; k++)
			if (used_tags[k] == m->ov_preview_tag) {
				main_tag = m->ov_preview_tag;
				break;
			}
	}

	/* tiles mirror the FULL monitor (m->m), so the bar's area at the top is
	 * included -- a faithful thumbnail of the whole screen, bar and all */
	float mon_aspect = (float)m->m.width / fmaxf(1.0f, (float)m->m.height);
	float pad = 6.0f;

	/* the overview covers the whole output (the DMS/top layer is hidden while
	 * it's open), so lay out against the full monitor m->m, not the usable m->w */
	float ox0 = m->m.x, oy0 = m->m.y, ow0 = m->m.width, oh0 = m->m.height;

	/* ---- top strip: a horizontal row of monitor-aspect tag tiles ---- */
	float strip_h = fmaxf(60.0f, oh0 * 0.10f);
	float tile_h = fmaxf(1.0f, strip_h - 2.0f * pad);
	float tile_w = tile_h * mon_aspect;
	float strip_gap = 16.0f;
	float total_w = uc * tile_w + (uc - 1) * strip_gap;
	float avail = fmaxf(1.0f, ow0 - 2.0f * gappo);
	float start_x;
	if (total_w <= avail) {
		start_x = ox0 + (ow0 - total_w) / 2.0f;
		m->ov_strip_scroll = 0.0f;
	} else {
		float max_scroll = total_w - avail;
		if (m->ov_strip_scroll < 0.0f)
			m->ov_strip_scroll = 0.0f;
		if (m->ov_strip_scroll > max_scroll)
			m->ov_strip_scroll = max_scroll;
		start_x = ox0 + gappo - m->ov_strip_scroll;
	}
	float strip_y = oy0 + (strip_h - tile_h) / 2.0f; /* tile centred in strip */

	/* ---- main area below the strip: a SCREEN-ASPECT panel so the desktop
	 * mirror fills it exactly (uniform translate, no void, no distortion) ---- */
	float avail_x = ox0 + gappo;
	float avail_y = strip_y + tile_h + pad + 4.0f; /* right below the strip tiles */
	float avail_w = fmaxf(1.0f, ow0 - 2.0f * gappo);
	float avail_h = fmaxf(1.0f, (oy0 + oh0 - gappo) - avail_y);
	m->ov_avail_y = avail_y; /* content-region top (below strip), for void masks */
	float screen_asp = (float)m->w.width / fmaxf(1.0f, (float)m->w.height);
	float main_w = fminf(avail_w, avail_h * screen_asp);
	float main_h = main_w / screen_asp;
	/* A faithful (undistorted) mirror can't touch all four edges when the area
	 * below the strip isn't the screen's shape, so scale it down a touch and
	 * FLOAT it centred in the available region -- the leftover margin is split
	 * evenly (matching top & bottom gaps) instead of pooling at the bottom. */
	float ov_shrink = 0.93f;
	main_w *= ov_shrink;
	main_h *= ov_shrink;
	float main_x = avail_x + (avail_w - main_w) / 2.0f;
	float main_y = avail_y + (avail_h - main_h) / 2.0f;

	/* the mirrored-screen rect: the desktop, uniformly scaled, anchored top-left
	 * in the big area (matches overview_arrange_main). The wallpaper backdrop
	 * fills exactly this rect so it reads as the screen; the rest is void. */
	float mscale =
		fminf(main_w / (float)m->w.width, main_h / (float)m->w.height);
	float mvp_w = m->w.width * mscale, mvp_h = m->w.height * mscale;

	/* depth: a soft drop shadow + a subtle 1px light edge so the OV desktop
	 * floats above the flat void (macOS-space feel) */
	{
		static const float shcol[4] = {0.0f, 0.0f, 0.0f, 0.55f};
		float ssig = 34.0f;
		int32_t spad = (int32_t)ceilf(ssig * 0.5f);
		if (!m->ov_main_shadow)
			m->ov_main_shadow = wlr_scene_shadow_create(
				layers[LyrDecorate], (int)mvp_w + 2 * spad,
				(int)mvp_h + 2 * spad, 18 + spad, ssig, shcol);
		if (m->ov_main_shadow) {
			wlr_scene_shadow_set_size(m->ov_main_shadow, (int)mvp_w + 2 * spad,
									  (int)mvp_h + 2 * spad);
			wlr_scene_shadow_set_corner_radius(m->ov_main_shadow, 18 + spad);
			wlr_scene_shadow_set_blur_sigma(m->ov_main_shadow, ssig);
			wlr_scene_node_set_position(&m->ov_main_shadow->node,
										(int)main_x - spad, (int)main_y - spad + 8);
			wlr_scene_node_set_enabled(&m->ov_main_shadow->node, true);
			if (m->ov_dim)
				wlr_scene_node_place_above(&m->ov_main_shadow->node,
										   &m->ov_dim->node);
		}
		/* no border rect: the drop shadow gives the depth, and a 1px edge only
		 * ended up half-covered by the void masks anyway */
		if (m->ov_main_border && m->ov_main_border->node.enabled)
			wlr_scene_node_set_enabled(&m->ov_main_border->node, false);
	}

	/* wallpaper backdrop for the mirrored screen, so windows sit on the desktop
	 * like a real screen instead of the near-black dim (drop the dark fallback) */
	{
		struct wlr_buffer *wp = overview_wallpaper_buffer(m);
		if (wp) {
			if (!m->ov_main_wp)
				m->ov_main_wp = wlr_scene_buffer_create(layers[LyrDecorate], wp);
			else
				wlr_scene_buffer_set_buffer(m->ov_main_wp, wp);
			if (m->ov_main_wp) {
				wlr_scene_buffer_set_dest_size(m->ov_main_wp, (int)mvp_w,
											   (int)mvp_h);
				wlr_scene_buffer_set_corner_radii(
					m->ov_main_wp,
					corner_radii_from_location(18, CORNER_LOCATION_ALL));
				wlr_scene_buffer_set_opacity(m->ov_main_wp, 1.0f);
				wlr_scene_node_set_position(&m->ov_main_wp->node, (int)main_x,
											(int)main_y);
				wlr_scene_node_set_enabled(&m->ov_main_wp->node, true);
				if (m->ov_main_shadow)
					wlr_scene_node_place_above(&m->ov_main_wp->node,
											   &m->ov_main_shadow->node);
				else if (m->ov_dim)
					wlr_scene_node_place_above(&m->ov_main_wp->node,
											   &m->ov_dim->node);
			}
		} else if (m->ov_main_wp) {
			wlr_scene_node_set_enabled(&m->ov_main_wp->node, false);
		}
	}

	/* Subtle vignette over the wallpaper (below the windows on LyrTile): two
	 * desktop-sized gradient rects, one vertical and one horizontal, each a
	 * symmetric dark->clear->dark ramp so the edges darken and the centre stays
	 * clear (corners get both, so they read darkest). A symmetric ramp is
	 * invariant to the renderer's y-flip, and the dark stops sit in the outer
	 * ~25% so the darkening hugs the edges instead of washing over the middle. */
	if (m->ov_main_wp && m->ov_main_wp->node.enabled) {
		/* black, premultiplied: {0,0,0,a}. 5 stops: dark, clear x3, dark. */
		const float A = 0.22f;
		const float vig_colors[20] = {
			0.0f, 0.0f, 0.0f, A,   0.0f, 0.0f, 0.0f, 0.0f,
			0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
			0.0f, 0.0f, 0.0f, A,
		};
		const float vig_origin[2] = {0.5f, 0.5f};
		const float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
		const float degs[2] = {90.0f, 0.0f}; /* [0]=vertical, [1]=horizontal */
		for (int32_t vi = 0; vi < 2; vi++) {
			if (!m->ov_vignette[vi])
				m->ov_vignette[vi] = wlr_scene_rect_create(
					layers[LyrDecorate], (int)mvp_w, (int)mvp_h, clear);
			if (!m->ov_vignette[vi])
				continue;
			wlr_scene_rect_set_size(m->ov_vignette[vi], (int)mvp_w, (int)mvp_h);
			wlr_scene_rect_set_corner_radii(
				m->ov_vignette[vi],
				corner_radii_from_location(18, CORNER_LOCATION_ALL));
			wlr_scene_rect_set_gradient(m->ov_vignette[vi], degs[vi], 1, 1,
										vig_origin, 5, vig_colors);
			wlr_scene_node_set_position(&m->ov_vignette[vi]->node, (int)main_x,
										(int)main_y);
			wlr_scene_node_set_enabled(&m->ov_vignette[vi]->node, true);
			wlr_scene_node_place_above(&m->ov_vignette[vi]->node,
									   &m->ov_main_wp->node);
		}
	} else {
		for (int32_t vi = 0; vi < 2; vi++)
			if (m->ov_vignette[vi] && m->ov_vignette[vi]->node.enabled)
				wlr_scene_node_set_enabled(&m->ov_vignette[vi]->node, false);
	}

	/* strip chrome, full width from the very top down to just below the tiles:
	 * an xray-blurred backdrop, a translucent pill-colour tint (5% translucent),
	 * and a pronounced drop shadow along the bottom edge */
	{
		int32_t bx = (int32_t)ox0, by = (int32_t)oy0;
		int32_t bw = (int32_t)ow0;
		int32_t bh = (int32_t)(strip_y + tile_h + pad - oy0);

		if (!m->ov_strip_blur)
			m->ov_strip_blur =
				wlr_scene_blur_create(layers[LyrDecorate], bw, bh);
		if (m->ov_strip_blur) {
			wlr_scene_blur_set_size(m->ov_strip_blur, bw, bh);
			wlr_scene_blur_set_should_only_blur_bottom_layer(m->ov_strip_blur,
															 true);
			wlr_scene_node_set_position(&m->ov_strip_blur->node, bx, by);
			wlr_scene_node_set_enabled(&m->ov_strip_blur->node, true);
		}

		/* the strip reads 50% darker than the dimmed main area below it: the
		 * main area transmits (1 - ov_dim_alpha) of the wallpaper through the
		 * backdrop dim, so the strip transmits half that (a matching black
		 * overlay over its xray-blurred wallpaper). Keep in sync with the
		 * ov_dim alpha in overview_draw_backdrop. */
		float dim_a = 0.9f;
		/* strip transmits half the main area's light, then 15% less again */
		float strip_a = 1.0f - (1.0f - dim_a) * 0.5f * 0.85f;
		float tint[4] = {0.0f, 0.0f, 0.0f, strip_a};
		if (!m->ov_strip_bg)
			m->ov_strip_bg =
				wlr_scene_rect_create(layers[LyrDecorate], bw, bh, tint);
		if (m->ov_strip_bg) {
			wlr_scene_rect_set_size(m->ov_strip_bg, bw, bh);
			wlr_scene_rect_set_color(m->ov_strip_bg, tint);
			wlr_scene_node_set_position(&m->ov_strip_bg->node, bx, by);
			wlr_scene_node_set_enabled(&m->ov_strip_bg->node, true);
			if (m->ov_strip_blur)
				wlr_scene_node_place_above(&m->ov_strip_bg->node,
										   &m->ov_strip_blur->node);
		}

		/* A pronounced drop shadow cast onto the (lighter) dimmed main area
		 * below. It must sit fully below the strip edge -- the part behind the
		 * strip is occluded by the near-black tint, and a black shadow over the
		 * ~equally-black strip reads as nothing. Kept above ov_dim so it darkens
		 * the dimmed wallpaper for real contrast. */
		static const float sh[4] = {0.0f, 0.0f, 0.0f, 0.95f};
		float sigma = 16.0f;
		int32_t shh = 34;
		if (!m->ov_strip_shadow)
			m->ov_strip_shadow = wlr_scene_shadow_create(layers[LyrDecorate],
														 bw, shh, 0, sigma, sh);
		if (m->ov_strip_shadow) {
			wlr_scene_shadow_set_size(m->ov_strip_shadow, bw, shh);
			wlr_scene_shadow_set_blur_sigma(m->ov_strip_shadow, sigma);
			wlr_scene_node_set_position(&m->ov_strip_shadow->node, bx,
										(int32_t)(by + bh));
			wlr_scene_node_set_enabled(&m->ov_strip_shadow->node, true);
			if (m->ov_dim)
				wlr_scene_node_place_above(&m->ov_strip_shadow->node,
										   &m->ov_dim->node);
		}
	}

	/* strip tiles: every tag is drawn as a mini-monitor from frozen surface-
	 * buffer snapshots of its on-screen windows (a live node can only be in one
	 * place, and the current tag's live nodes are used by the main area below).
	 * The snapshot pool is rebuilt from scratch every draw. */
	for (int32_t s = 0; s < OV_STRIP_WINS; s++) {
		if (m->ov_snap[s]) {
			wlr_scene_node_destroy(&m->ov_snap[s]->node);
			m->ov_snap[s] = NULL;
		}
	}
	int32_t snap_idx = 0; /* running index into the m->ov_snap snapshot pool */
	m->ov_tile_count = 0;
	m->ov_tile_y = strip_y;
	m->ov_tile_w = tile_w;
	m->ov_tile_h = tile_h;
	for (int32_t i = 0; i < uc; i++) {
		float tx = start_x + i * (tile_w + strip_gap);
		/* the previewed (main-area) tile gets the accent highlight */
		bool current = (used_tags[i] == main_tag);
		overview_draw_cell_label(m, i, tx, strip_y, tile_w, tile_h,
								 used_tags[i], current, 0);
		/* record the tile rect for pointer hover hit-testing */
		if (i < OV_TAG_CELLS) {
			m->ov_tile_x[i] = tx;
			m->ov_tile_tag[i] = used_tags[i];
			m->ov_tile_count = i + 1;
		}

		/* mini-monitor: scale the tag's viewport (m->w) to fill the tile, which
		 * is monitor-aspect so it fills exactly. On-screen windows land inside
		 * the tile; scroller windows scrolled off-screen are dropped, and edge
		 * windows are clipped, so every tile reads like a little screen. */
		float scale = fminf(tile_w / (float)m->m.width,
							tile_h / (float)m->m.height);
		float off_x = tx + (tile_w - m->m.width * scale) / 2.0f;
		float off_y = strip_y + (tile_h - m->m.height * scale) / 2.0f;
		float vl = m->m.x, vt = m->m.y;
		float vr = m->m.x + m->m.width, vb = m->m.y + m->m.height;
		wl_list_for_each(c, &clients, link) {
			if (c->mon != m || c->isunglobal || c->isminimized ||
				client_is_x11_popup(c))
				continue;
			if (get_tags_first_tag_num(c->tags) != used_tags[i])
				continue;
			/* every strip tile is drawn from frozen surface-buffer snapshots, so
			 * the real node isn't shown live here: hide windows that aren't the
			 * previewed tag (whose live nodes fill the main area below) */
			if (!current && c->scene && !c->is_overview_hidden) {
				c->is_overview_hidden = true;
				wlr_scene_node_set_enabled(&c->scene->node, false);
			}
			float gx = c->overview_backup_geom.x, gy = c->overview_backup_geom.y;
			float gw = fmaxf(1.0f, (float)c->overview_backup_geom.width);
			float gh = fmaxf(1.0f, (float)c->overview_backup_geom.height);
			/* visible portion of the window within the viewport */
			float vx0 = fmaxf(gx, vl), vy0 = fmaxf(gy, vt);
			float vx1 = fminf(gx + gw, vr), vy1 = fminf(gy + gh, vb);
			if (vx1 - vx0 < 2.0f || vy1 - vy0 < 2.0f)
				continue; /* scrolled off-screen */
			/* frozen at overview launch -> a static screenshot of the tag */
			struct wlr_buffer *snapbuf = c->ov_snap_buf;
			if (snap_idx >= OV_STRIP_WINS || !snapbuf)
				continue;
			int32_t bx = (int32_t)(off_x + (vx0 - vl) * scale + 0.5f);
			int32_t by = (int32_t)(off_y + (vy0 - vt) * scale + 0.5f);
			int32_t bw = (int32_t)fmaxf(1.0f, (vx1 - vx0) * scale);
			int32_t bh = (int32_t)fmaxf(1.0f, (vy1 - vy0) * scale);
			int32_t bufw = snapbuf->width;
			int32_t bufh = snapbuf->height;
			struct wlr_scene_buffer *sb =
				wlr_scene_buffer_create(layers[LyrDecorate], NULL);
			if (!sb)
				continue;
			struct wlr_fbox src = {
				.x = (double)(vx0 - gx) / gw * bufw,
				.y = (double)(vy0 - gy) / gh * bufh,
				.width = (double)(vx1 - vx0) / gw * bufw,
				.height = (double)(vy1 - vy0) / gh * bufh,
			};
			wlr_scene_buffer_set_buffer(sb, snapbuf);
			wlr_scene_buffer_set_source_box(sb, &src);
			wlr_scene_buffer_set_dest_size(sb, bw, bh);
			/* round only the corners that touch the tile's outer corners, so the
			 * tile edge is smooth while inter-window seams stay square (no
			 * pixelated overhang where a square snapshot meets the rounded tile) */
			int32_t tlx = (int32_t)tx, tty = (int32_t)strip_y;
			int32_t trx = (int32_t)(tx + tile_w);
			int32_t tby = (int32_t)(strip_y + tile_h);
			enum corner_location loc = 0;
			if (bx <= tlx + 2 && by <= tty + 2)
				loc |= CORNER_LOCATION_TOP_LEFT;
			if (bx + bw >= trx - 2 && by <= tty + 2)
				loc |= CORNER_LOCATION_TOP_RIGHT;
			if (bx + bw >= trx - 2 && by + bh >= tby - 2)
				loc |= CORNER_LOCATION_BOTTOM_RIGHT;
			if (bx <= tlx + 2 && by + bh >= tby - 2)
				loc |= CORNER_LOCATION_BOTTOM_LEFT;
			wlr_scene_buffer_set_corner_radii(
				sb, corner_radii_from_location(16, loc));
			wlr_scene_node_set_position(&sb->node, bx, by);
			wlr_scene_node_raise_to_top(&sb->node);
			m->ov_snap[snap_idx++] = sb;
		}

		/* composite the bar (top/overlay layer-shell panels) into the tile at
		 * their real position, so the tile reads as a full-screen thumbnail */
		for (int32_t li = 2; li <= 3; li++) {
			LayerSurface *lsurf;
			wl_list_for_each(lsurf, &m->layers[li], link) {
				if (snap_idx >= OV_STRIP_WINS || !lsurf->mapped ||
					!lsurf->layer_surface || !lsurf->layer_surface->surface ||
					!lsurf->layer_surface->surface->buffer)
					continue;
				struct wlr_buffer *lbuf =
					&lsurf->layer_surface->surface->buffer->base;
				int32_t lbx =
					(int32_t)(off_x + (lsurf->geom.x - vl) * scale + 0.5f);
				int32_t lby =
					(int32_t)(off_y + (lsurf->geom.y - vt) * scale + 0.5f);
				int32_t lbw = (int32_t)fmaxf(1.0f, lsurf->geom.width * scale);
				int32_t lbh = (int32_t)fmaxf(1.0f, lsurf->geom.height * scale);
				struct wlr_scene_buffer *lsb =
					wlr_scene_buffer_create(layers[LyrDecorate], NULL);
				if (!lsb)
					continue;
				wlr_scene_buffer_set_buffer(lsb, lbuf);
				wlr_scene_buffer_set_dest_size(lsb, lbw, lbh);
				enum corner_location lloc = 0;
				if (lbx <= (int32_t)tx + 2 && lby <= (int32_t)strip_y + 2)
					lloc |= CORNER_LOCATION_TOP_LEFT;
				if (lbx + lbw >= (int32_t)(tx + tile_w) - 2 &&
					lby <= (int32_t)strip_y + 2)
					lloc |= CORNER_LOCATION_TOP_RIGHT;
				wlr_scene_buffer_set_corner_radii(
					lsb, corner_radii_from_location(16, lloc));
				wlr_scene_node_set_position(&lsb->node, lbx, lby);
				wlr_scene_node_raise_to_top(&lsb->node);
				m->ov_snap[snap_idx++] = lsb;
			}
		}
	}

	/* cache the big-area rect + tag, then lay out its windows via the
	 * standalone helper (so scrolling can re-lay-out without a full arrange) */
	m->ov_main_x = main_x;
	m->ov_main_y = main_y;
	m->ov_main_w = main_w;
	m->ov_main_h = main_h;
	m->ov_main_tag = main_tag;
	overview_arrange_main(m, false);

	/* hide leftover cell chrome from a previous layout with more tags */
	for (int32_t i = uc; i < OV_TAG_CELLS; i++) {
		if (m->ov_cell_bg[i] && m->ov_cell_bg[i]->node.enabled)
			wlr_scene_node_set_enabled(&m->ov_cell_bg[i]->node, false);
		if (m->ov_cell_wp[i] && m->ov_cell_wp[i]->node.enabled)
			wlr_scene_node_set_enabled(&m->ov_cell_wp[i]->node, false);
		if (m->ov_cell_shadow[i] && m->ov_cell_shadow[i]->node.enabled)
			wlr_scene_node_set_enabled(&m->ov_cell_shadow[i]->node, false);
		if (m->ov_cell_label[i] &&
			m->ov_cell_label[i]->scene_buffer->node.enabled)
			wlr_scene_node_set_enabled(
				&m->ov_cell_label[i]->scene_buffer->node, false);
	}
}

/* Hover-to-preview: while overview is open, hovering a strip tile makes the
 * main area show that tag. Called from motionnotify on real pointer motion. */
void overview_pointer_preview(Monitor *m, double px, double py) {
	if (!m || !m->isoverview || m->ov_tile_count == 0)
		return;
	if (py < m->ov_tile_y || py > m->ov_tile_y + m->ov_tile_h)
		return; /* pointer isn't over the strip row */
	for (int32_t i = 0; i < m->ov_tile_count; i++) {
		if (px >= m->ov_tile_x[i] && px < m->ov_tile_x[i] + m->ov_tile_w) {
			if (m->ov_preview_tag != m->ov_tile_tag[i]) {
				m->ov_preview_tag = m->ov_tile_tag[i];
				arrange(m, false, false);
			}
			return;
		}
	}
}

/* Tag of the strip tile under (px,py), or 0 if the pointer isn't over a tile. */
uint32_t overview_tile_at(Monitor *m, double px, double py) {
	if (!m || !m->isoverview || m->ov_tile_count == 0)
		return 0;
	if (py < m->ov_tile_y || py > m->ov_tile_y + m->ov_tile_h)
		return 0;
	for (int32_t i = 0; i < m->ov_tile_count; i++)
		if (px >= m->ov_tile_x[i] && px < m->ov_tile_x[i] + m->ov_tile_w)
			return m->ov_tile_tag[i];
	return 0;
}

/* The hovered window is highlighted by focusing it (its border takes the theme
 * focus colour) -- subtle and theme-driven. The old bright accent tint is gone;
 * this just makes sure any leftover tint node stays hidden. */
void overview_hover_highlight(Monitor *m, Client *c) {
	(void)c;
	if (m && m->ov_hover_hl && m->ov_hover_hl->node.enabled)
		wlr_scene_node_set_enabled(&m->ov_hover_hl->node, false);
}

/* Scroll the big area horizontally through a scroller tag's windows. dir is +1
 * (reveal windows to the right) or -1 (to the left). */
void overview_main_scroll(Monitor *m, double px, double py, int32_t dir) {
	if (!m || !m->isoverview || m->ov_scroll_tag == 0)
		return;
	(void)px;
	(void)py;
	/* pan by a fraction of the viewport; overview_arrange_main clamps to range */
	m->ov_main_scroll += dir * (m->w.width * 0.18f);
	/* re-lay-out only the big area (no full arrange -> no snapshot rebuild),
	 * instant so scrolling stays smooth */
	overview_arrange_main(m, true);
	if (m->wlr_output)
		wlr_output_schedule_frame(m->wlr_output);
}

/* open/close zoom-fade: the windows already animate their geometry (the
 * "zoom"); this ramps the chrome opacity (the "fade") in lockstep so the
 * backdrop, wallpaper, desktop shadow and strip tiles don't pop in/out. */
#define OV_ANIM_DUR_OPEN 200.0f
#define OV_ANIM_DUR_CLOSE 160.0f

/* apply a chrome visibility t in [0,1] to the fadeable overview nodes */
void overview_anim_apply(Monitor *m, float t) {
	if (t < 0.0f)
		t = 0.0f;
	if (t > 1.0f)
		t = 1.0f;
	if (m->ov_dim) {
		float c[4] = {0.0f, 0.0f, 0.0f, 0.9f * t};
		wlr_scene_rect_set_color(m->ov_dim, c);
	}
	if (m->ov_main_wp)
		wlr_scene_buffer_set_opacity(m->ov_main_wp, t);
	if (m->ov_main_shadow) {
		float c[4] = {0.0f, 0.0f, 0.0f, 0.55f * t};
		wlr_scene_shadow_set_color(m->ov_main_shadow, c);
	}
	/* strip: fade the tint + shadow (base alphas kept in sync with
	 * overview_tags); the xray blur behind them has no opacity, so it just
	 * stays put — a blurred patch under a fading tint is unobtrusive */
	if (m->ov_strip_bg) {
		float c[4] = {0.0f, 0.0f, 0.0f, 0.9575f * t};
		wlr_scene_rect_set_color(m->ov_strip_bg, c);
	}
	if (m->ov_strip_shadow) {
		float c[4] = {0.0f, 0.0f, 0.0f, 0.95f * t};
		wlr_scene_shadow_set_color(m->ov_strip_shadow, c);
	}
	for (int32_t i = 0; i < OV_STRIP_WINS; i++)
		if (m->ov_snap[i] && m->ov_snap[i]->node.enabled)
			wlr_scene_buffer_set_opacity(m->ov_snap[i], t);
}

/* begin a fade; open == true fades in, false fades out. Seeds the start time
 * so a mid-flight direction flip continues from the current visibility. */
void overview_anim_start(Monitor *m, bool open) {
	uint32_t now = get_now_in_ms();
	float t = m->ov_anim_running ? m->ov_anim_t : (open ? 0.0f : 1.0f);
	float dur = open ? OV_ANIM_DUR_OPEN : OV_ANIM_DUR_CLOSE;
	/* progress that reproduces the current visibility (linear approx of the
	 * smoothstep ease — close enough to avoid a visible jump on reversal) */
	float prog = open ? t : (1.0f - t);
	m->ov_anim_start_ms = now - (uint32_t)(prog * dur);
	m->ov_anim_open = open;
	m->ov_anim_t = t;
	m->ov_anim_running = true;
}

/* advance the fade one frame; returns true while more frames are needed */
bool overview_anim_frame(Monitor *m) {
	if (!m->ov_anim_running)
		return false;
	float dur = m->ov_anim_open ? OV_ANIM_DUR_OPEN : OV_ANIM_DUR_CLOSE;
	float prog = (get_now_in_ms() - m->ov_anim_start_ms) / dur;
	if (prog < 0.0f)
		prog = 0.0f;
	bool done = prog >= 1.0f;
	if (done)
		prog = 1.0f;
	float e = prog * prog * (3.0f - 2.0f * prog); /* smoothstep */
	m->ov_anim_t = m->ov_anim_open ? e : (1.0f - e);
	overview_anim_apply(m, m->ov_anim_t);
	if (done) {
		m->ov_anim_running = false;
		if (!m->ov_anim_open) {
			/* fade-out finished: now actually tear the chrome down */
			m->ov_anim_t = 0.0f;
			overview_hide_chrome(m);
		}
		return false;
	}
	return true;
}

void overview(Monitor *m) {

	/* the strip covers the top layer-shell (DMS bar): hide it while overview
	 * is open, restored by overview_hide_chrome() on exit */
	wlr_scene_node_set_enabled(&layers[LyrTop]->node, false);

	overview_draw_backdrop(m);
	overview_tags(m);

	if (m->is_jump_mode) {
		create_jump_hints(m);
	}
}