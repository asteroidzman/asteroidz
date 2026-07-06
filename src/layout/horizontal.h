void monocle(Monitor *m) {
	Client *c = NULL, *fc = NULL;
	struct wlr_box geom;
	int32_t cur_gappov = enablegaps ? m->gappov : 0;
	int32_t cur_gappoh = enablegaps ? m->gappoh : 0;
	int32_t cur_gapih = enablegaps ? m->gappih : 0;

	if (config.smartgaps && m->visible_fake_tiling_clients == 1) {
		cur_gappov = cur_gappoh = cur_gapih = 0;
	}

	int n = m->visible_fake_tiling_clients;
	if (n == 0)
		return;

	wl_list_for_each(c, &fstack, flink) {
		if (c->iskilling || c->isunglobal || !ISFAKETILED(c))
			continue;
		if (VISIBLEON(c, m)) {
			fc = c;
			break;
		}
	}

	/* every window occupies the same content rect; client_tile_resize
	 * reserves titlebar space above it on its own (same mechanism every
	 * other layout uses), so a single geom serves all clients here. */
	geom.x = m->w.x + cur_gappoh;
	geom.y = m->w.y + cur_gappov;
	geom.width = m->w.width - 2 * cur_gappoh;
	geom.height = m->w.height - 2 * cur_gappov;

	if (n == 1) {
		client_tile_resize(fc, geom, 0);
		monocle_set_focus(fc, true);
		return;
	}

	/* more than one window: each client's own titlebar becomes one segment
	 * of a shared row (browser-tab-strip style) instead of a separate
	 * tab-bar widget. */
	int tab_area_width = geom.width;
	int total_gaps = (n - 1) * cur_gapih;
	int base_width = (tab_area_width - total_gaps) / n;
	int remainder = (tab_area_width - total_gaps) % n;
	int tab_x = geom.x;

	/* cap segment width so pills don't stretch across the whole screen; the
	 * capped row is left-aligned in the tab area */
	if (config.monocle_tab_max_width > 0 &&
		base_width > config.monocle_tab_max_width) {
		base_width = config.monocle_tab_max_width;
		remainder = 0;
	}
	int idx = 0;

	wl_list_for_each(c, &clients, link) {
		if (!VISIBLEON(c, m) || !ISFAKETILED(c))
			continue;

		bool focused = (c == fc);
		monocle_set_focus(c, focused);
		client_tile_resize(c, geom, 0);

		int tw = base_width + (idx < remainder ? 1 : 0);
		client_draw_monocle_titlebar_segment(c, tab_x, geom.y, tw, focused,
											 idx == 0, idx == n - 1);

		tab_x += tw + cur_gapih;
		idx++;
	}
}
