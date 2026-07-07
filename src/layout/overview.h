
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
	static const float dim_color[4] = {0.0f, 0.0f, 0.0f, 0.55f};
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
	if (m->ov_dim && m->ov_dim->node.enabled)
		wlr_scene_node_set_enabled(&m->ov_dim->node, false);
	for (int32_t i = 0; i < OV_TAG_CELLS; i++) {
		if (m->ov_cell_bg[i] && m->ov_cell_bg[i]->node.enabled)
			wlr_scene_node_set_enabled(&m->ov_cell_bg[i]->node, false);
		if (m->ov_cell_label[i] &&
			m->ov_cell_label[i]->scene_buffer->node.enabled)
			wlr_scene_node_set_enabled(
				&m->ov_cell_label[i]->scene_buffer->node, false);
	}
}

/* Draw cell `slot`: an OPAQUE rounded dark tile (so each tag reads as its own
 * "workspace" rather than showing an inconsistent crop of the single shared
 * wallpaper) with the tag-name label at the top. The tile is drawn behind the
 * window thumbnails (LyrDecorate < LyrTile); the tag you came from gets a
 * slightly warmer tile and a focused (accent) label. */
static void overview_draw_cell_label(Monitor *m, int32_t slot, float cx,
									 float cy, float cw, float ch, uint32_t tag,
									 bool current, int32_t hidden) {
	if (slot < 0 || slot >= OV_TAG_CELLS)
		return;

	static const float tile_normal[4] = {0.10f, 0.10f, 0.13f, 1.0f};
	static const float tile_current[4] = {0.17f, 0.14f, 0.10f, 1.0f};
	const float *tile = current ? tile_current : tile_normal;

	if (!m->ov_cell_bg[slot])
		m->ov_cell_bg[slot] = wlr_scene_rect_create(layers[LyrDecorate],
													(int)cw, (int)ch, tile);
	if (m->ov_cell_bg[slot]) {
		wlr_scene_rect_set_size(m->ov_cell_bg[slot], (int)cw, (int)ch);
		wlr_scene_rect_set_color(m->ov_cell_bg[slot], tile);
		wlr_scene_rect_set_corner_radii(
			m->ov_cell_bg[slot],
			corner_radii_from_location(16, CORNER_LOCATION_ALL));
		wlr_scene_node_set_position(&m->ov_cell_bg[slot]->node, (int)cx,
									(int)cy);
		wlr_scene_node_set_enabled(&m->ov_cell_bg[slot]->node, true);
	}

	if (!m->ov_cell_label[slot]) {
		m->ov_cell_label[slot] =
			asteroidz_jump_label_node_create(layers[LyrOverlay], config.pilldata);
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
void overview_tags(Monitor *m) {
	int32_t gappo = config.overviewgappo;
	int32_t gappi = config.overviewgappi;
	uint32_t ntags = LENGTH(tags);
	uint32_t cur_tag = get_tags_first_tag_num(m->ovbk_current_tagset);
	if (cur_tag == 0 || cur_tag > ntags)
		cur_tag = m->pertag->prevtag;

	/* which tags have at least one window (each window counted under the
	 * lowest tag it belongs to, so it appears in exactly one cell) */
	bool used[OV_TAG_CELLS] = {false};
	uint32_t used_tags[OV_TAG_CELLS];
	int32_t uc = 0;
	Client *c;
	wl_list_for_each(c, &clients, link) {
		if (c->mon != m || c->isunglobal || c->isminimized ||
			client_is_x11_popup(c))
			continue;
		uint32_t t = get_tags_first_tag_num(c->tags);
		if (t >= 1 && t <= ntags && t < OV_TAG_CELLS)
			used[t] = true;
	}
	/* always include the tag we came from, even if it's empty, so the
	 * overview still shows where you are */
	if (cur_tag >= 1 && cur_tag <= ntags && cur_tag < OV_TAG_CELLS)
		used[cur_tag] = true;

	for (uint32_t t = 1; t <= ntags && t < OV_TAG_CELLS; t++)
		if (used[t])
			used_tags[uc++] = t;
	if (uc == 0)
		return;

	int32_t cols = 1;
	while (cols * cols < uc)
		cols++;
	int32_t rows = (uc + cols - 1) / cols;

	float area_w = fmaxf(1.0f, m->w.width - 2.0f * gappo);
	float area_h = fmaxf(1.0f, m->w.height - 2.0f * gappo);
	float cell_w = (area_w - (cols - 1) * gappi) / cols;
	float cell_h = (area_h - (rows - 1) * gappi) / rows;
	float label_h = 30.0f; /* strip at cell top reserved for the tag label */
	float pad = 6.0f;

	for (int32_t i = 0; i < uc; i++) {
		int32_t col = i % cols;
		int32_t row = i / cols;
		float cell_x = m->w.x + gappo + col * (cell_w + gappi);
		float cell_y = m->w.y + gappo + row * (cell_h + gappi);

		/* content area (below the label strip) */
		float content_x = cell_x + pad;
		float content_y = cell_y + label_h;
		float content_w = fmaxf(1.0f, cell_w - 2.0f * pad);
		float content_h = fmaxf(1.0f, cell_h - label_h - pad);

		/* uniform monitor->cell scale: the same for every tag, so window
		 * thumbnails are the same size regardless of tag or layout */
		float scale = fminf(content_w / (float)m->w.width,
							content_h / (float)m->w.height);
		float scaled_w = m->w.width * scale;
		float scaled_h = m->w.height * scale;
		/* origin of the monitor's top-left, centering the scaled viewport */
		float off_x = content_x + (content_w - scaled_w) / 2.0f;
		float off_y = content_y + (content_h - scaled_h) / 2.0f;

		/* count how many of this tag's windows are fully off the viewport
		 * (scrolled away in a scroller) so the label can show a "+N" badge */
		int32_t hidden = 0;
		wl_list_for_each(c, &clients, link) {
			if (c->mon != m || c->isunglobal || c->isminimized ||
				client_is_x11_popup(c))
				continue;
			if (get_tags_first_tag_num(c->tags) != used_tags[i])
				continue;
			float x = c->overview_backup_geom.x;
			float y = c->overview_backup_geom.y;
			float w = c->overview_backup_geom.width;
			float h = c->overview_backup_geom.height;
			bool on_screen = (x + w > m->w.x) && (x < m->w.x + m->w.width) &&
							 (y + h > m->w.y) && (y < m->w.y + m->w.height);
			if (!on_screen)
				hidden++;
		}

		bool current = (used_tags[i] == cur_tag);
		overview_draw_cell_label(m, i, cell_x, cell_y, cell_w, cell_h,
								 used_tags[i], current, hidden);

		wl_list_for_each(c, &clients, link) {
			if (c->mon != m || c->isunglobal || c->isminimized ||
				client_is_x11_popup(c))
				continue;
			if (get_tags_first_tag_num(c->tags) != used_tags[i])
				continue;

			float cx0 = c->overview_backup_geom.x;
			float cy0 = c->overview_backup_geom.y;
			float cw0 = c->overview_backup_geom.width;
			float ch0 = c->overview_backup_geom.height;
			/* skip windows scrolled fully off the viewport (counted in +N) */
			if (!((cx0 + cw0 > m->w.x) && (cx0 < m->w.x + m->w.width) &&
				  (cy0 + ch0 > m->w.y) && (cy0 < m->w.y + m->w.height)))
				continue;

			float rx = cx0 - m->w.x;
			float ry = cy0 - m->w.y;
			struct wlr_box box = {
				.x = (int)(off_x + rx * scale + 0.5f),
				.y = (int)(off_y + ry * scale + 0.5f),
				.width = (int)(cw0 * scale + 0.5f),
				.height = (int)(ch0 * scale + 0.5f),
			};
			/* clamp a partly-off-screen edge window to the cell content rect so
			 * it can't spill past the tile into a neighbour */
			int32_t cl = (int)content_x, ct = (int)content_y;
			int32_t cr = (int)(content_x + content_w);
			int32_t cb = (int)(content_y + content_h);
			if (box.x < cl) {
				box.width -= (cl - box.x);
				box.x = cl;
			}
			if (box.y < ct) {
				box.height -= (ct - box.y);
				box.y = ct;
			}
			if (box.x + box.width > cr)
				box.width = cr - box.x;
			if (box.y + box.height > cb)
				box.height = cb - box.y;
			if (box.width < 1)
				box.width = 1;
			if (box.height < 1)
				box.height = 1;
			client_tile_resize(c, box, 0);
		}
	}

	/* hide any cells left over from a previous overview with more tags */
	for (int32_t i = uc; i < OV_TAG_CELLS; i++) {
		if (m->ov_cell_bg[i] && m->ov_cell_bg[i]->node.enabled)
			wlr_scene_node_set_enabled(&m->ov_cell_bg[i]->node, false);
		if (m->ov_cell_label[i] &&
			m->ov_cell_label[i]->scene_buffer->node.enabled)
			wlr_scene_node_set_enabled(
				&m->ov_cell_label[i]->scene_buffer->node, false);
	}
}

void overview(Monitor *m) {

	overview_draw_backdrop(m);
	overview_tags(m);

	if (m->is_jump_mode) {
		create_jump_hints(m);
	}
}