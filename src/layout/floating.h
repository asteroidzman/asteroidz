/* Floating layout: every window on the tag floats, new windows cascade
 * from the top-left of the work area, focus raises (see focusclient).
 *
 * Windows are floated with c->autofloated set so switching the tag to any
 * tiled layout re-tiles exactly the windows this layout floated (see
 * arrange()); windows the user floated by hand keep floating. */

/* Next cascade slot on m: a fixed ring of diagonal offsets from the work
 * area's top-left. Deliberately geometry-independent — at initial commit
 * the client's size is not settled yet — resize()/applybounds clamp the
 * window back into the work area if a slot would overflow. */
#define FLOAT_CASCADE_SLOTS 10
static struct wlr_box floating_cascade_box(Monitor *m, struct wlr_box geom) {
	int32_t step = config.titlebar_height > 0
					   ? (int32_t)config.titlebar_height + 12
					   : 48;
	int32_t idx = (int32_t)(m->cascade_idx++ % FLOAT_CASCADE_SLOTS);
	geom.x = m->w.x + config.gappoh + idx * step;
	geom.y = m->w.y + config.gappov + idx * step;
	return geom;
}

void floatlayout(Monitor *m) {
	Client *c;

	wl_list_for_each(c, &clients, link) {
		if (!VISIBLEON(c, m) || c->iskilling || c->isminimized ||
			c->isunglobal || c->isfullscreen || c->ismaximizescreen ||
			client_is_unmanaged(c) || c->isfloating)
			continue;

		c->isfloating = 1;
		c->autofloated = 1;
		wlr_scene_node_reparent(&c->scene->node,
								layers[c->isoverlay ? LyrOverlay : LyrTop]);

		struct wlr_box geom =
			c->float_geom.width > 0 && c->float_geom.height > 0 ? c->float_geom
																: c->geom;
		/* windows without a chosen floating spot get placed once; ones with
		 * one (moved by hand, rule offsets, an earlier placement) keep it.
		 * i3-style: center the window on the work area (float_center_new);
		 * the old top-left cascade is the opt-out. */
		if (!c->iscustompos && !c->cascaded) {
			geom = config.float_center_new
					   ? setclient_coordinate_center(c, m, geom, 0, 0)
					   : floating_cascade_box(m, geom);
			c->cascaded = 1;
		}
		c->float_geom = geom;
		resize(c, geom, 0);
	}
}

/* arrange() calls this before running any tiled layout so windows the float
 * layout floated fall back into the tiling; user-floated windows keep their
 * floating state. */
static void floating_layout_untangle(Monitor *m) {
	Client *c;

	if (is_float_layout(m))
		return;

	wl_list_for_each(c, &clients, link) {
		if (!c->autofloated || !VISIBLEON(c, m) || c->iskilling)
			continue;
		c->autofloated = 0;
		c->isfloating = 0;
		wlr_scene_node_reparent(&c->scene->node, layers[LyrTile]);
	}
}
