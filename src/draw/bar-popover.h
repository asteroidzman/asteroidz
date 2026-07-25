#ifndef ASTEROIDZ_BAR_POPOVER_H
#define ASTEROIDZ_BAR_POPOVER_H

/* Click-through panels for the native bar: the surface a module drops below
 * itself when its pill is clicked, holding rows the pointer can act on.
 *
 * Built from the pieces the bar already has -- a scenefx blur/rect/shadow
 * stack for the panel and one tab-bar node per row -- so it inherits the theme
 * and the per-output scale for free, and needs no surface, no client and no
 * layer-shell round trip. A popover is just more scene nodes.
 *
 * Exactly ONE is open at a time, session-wide. Two open popovers would need
 * z-order arbitration and a per-popover grab for no benefit: a bar popover is
 * a menu, and menus are modal by convention.
 *
 * Input model, deliberately minimal for this first cut:
 *   - a click on a row runs that row's action and closes;
 *   - a click anywhere else closes and is SWALLOWED, so the click that
 *     dismisses a menu does not also land on whatever was underneath;
 *   - Escape closes.
 * There is no keyboard grab and no arrow-key navigation yet -- that needs the
 * popover to take focus, which would steal it from the window underneath and
 * wants its own thought.
 */

#define BAR_POPOVER_MAX_ROWS 12

/* What the rows mean, so a click knows how to act on the payload. */
enum bar_popover_kind {
	BAR_POPOVER_NONE = 0,
	BAR_POPOVER_SINKS, /* audio outputs; row payload is an index into names */
};

typedef struct {
	struct asteroidz_tab_bar_node *node;
	AsteroidzNodeData *hit;
	char text[BAR_TEXT_MAX];
	/* Opaque payload the popover's own click handler interprets. For the sink
	 * picker this is the pactl sink NAME, which is what set-default-sink
	 * takes; kept as a string rather than the numeric index because indices
	 * are recycled when a device is unplugged and replugged. */
	char value[192];
	bool selected;
	bool used;
} BarPopoverRow;

typedef struct {
	Monitor *mon;
	struct wlr_scene_tree *tree;
	/* Panel strictly below rows, as two trees rather than per-node
	 * restacking. The panel's background rect is created lazily on the first
	 * layout pass -- i.e. AFTER the rows exist -- so on a single shared tree
	 * it lands last and paints straight over every label. Same reason the bar
	 * splits panel_tree from pill_tree. */
	struct wlr_scene_tree *panel_tree;
	struct wlr_scene_tree *row_tree;
	BarPanel panel;
	BarPopoverRow rows[BAR_POPOVER_MAX_ROWS];
	int32_t nrows;
	enum bar_popover_kind kind;
	/* horizontal centre of the pill this hangs from, in layout coordinates */
	int32_t anchor_x;
	struct wlr_box box;
	bool open;
} BarPopover;

static BarPopover bar_popover;

static void bar_popover_close(void);
static void bar_popover_layout(void);

/* ─── rows ────────────────────────────────────────────────────────────────── */

static void bar_popover_row_release(BarPopoverRow *r) {
	if (!r->used)
		return;
	if (r->node) {
		/* takes the hit-test tag with it, exactly like a bar pill */
		asteroidz_tab_bar_node_destroy(r->node);
		r->node = NULL;
	}
	r->hit = NULL;
	r->used = false;
	r->text[0] = '\0';
	r->value[0] = '\0';
	r->selected = false;
}

static BarPopoverRow *bar_popover_row_get(int32_t idx) {
	if (idx < 0 || idx >= BAR_POPOVER_MAX_ROWS || !bar_popover.row_tree)
		return NULL;
	BarPopoverRow *r = &bar_popover.rows[idx];
	if (r->used)
		return r;

	AsteroidzNodeData *hit = ecalloc(1, sizeof(AsteroidzNodeData));
	hit->type = ASTEROIDZ_BAR_POPOVER_NODE;
	hit->node_data = r;
	r->node = asteroidz_tab_bar_node_create(hit, bar_popover.row_tree,
										   config.theme, 0, 0);
	if (!r->node) {
		free(hit);
		return NULL;
	}
	r->hit = hit;
	r->used = true;
	/* rows are a list, not a row of chips: text starts at the left edge so the
	 * labels line up with each other rather than each centring in its own row */
	asteroidz_tab_bar_node_set_text_align_left(r->node, true);
	asteroidz_tab_bar_node_set_padding(r->node, config.bar_popover_padding,
									   config.theme.padding_y);
	return r;
}

/* ─── geometry ────────────────────────────────────────────────────────────── */

static void bar_popover_layout(void) {
	if (!bar_popover.open || !bar_popover.tree || !bar_popover.mon)
		return;
	Monitor *m = bar_popover.mon;
	float scale = m->wlr_output ? m->wlr_output->scale : 1.0f;
	if (scale <= 0.0f)
		scale = 1.0f;

	int32_t pad = config.bar_panel_padding;
	int32_t row_h = config.bar_popover_row_height;
	int32_t width = config.bar_popover_width;

	/* Width is configured rather than measured: these rows carry device names
	 * that run to eighty characters, so sizing to content would produce a
	 * panel wider than the output. Rows ellipsise into the fixed width. */
	int32_t inner_w = width - 2 * pad;
	if (inner_w < row_h)
		inner_w = row_h;

	int32_t nrows = 0;
	for (int32_t i = 0; i < bar_popover.nrows; i++)
		if (bar_popover.rows[i].used)
			nrows++;
	if (nrows == 0) {
		bar_popover_close();
		return;
	}

	int32_t height = nrows * row_h + (nrows - 1) * config.bar_popover_spacing +
					 2 * pad;

	/* Hangs off the bar's outer edge: below a top bar, above a bottom one. */
	int32_t bar_bottom = m->m.y + config.bar_margin_y + config.bar_height;
	int32_t y = config.bar_position_bottom
					? m->m.y + m->m.height - config.bar_margin_y -
						  config.bar_height - config.bar_popover_gap - height
					: bar_bottom + config.bar_popover_gap;

	int32_t x = bar_popover.anchor_x - width / 2;
	/* Clamp to the output. A pill near the right edge would otherwise hang its
	 * popover off the screen, which is where the tray's would always land. */
	int32_t min_x = m->m.x + config.bar_margin_x;
	int32_t max_x = m->m.x + m->m.width - config.bar_margin_x - width;
	if (x > max_x)
		x = max_x;
	if (x < min_x)
		x = min_x;

	bar_popover.box = (struct wlr_box){x, y, width, height};

	/* Panel first, rows above it -- same ordering rule as the bar's slots. */
	BarPanel *panel = &bar_popover.panel;
	int32_t radius = config.bar_panel_radius;
	if (config.bar_panel_blur && config.blur) {
		if (!panel->blur)
			panel->blur =
				wlr_scene_blur_create(bar_popover.panel_tree, width, height);
		if (panel->blur) {
			wlr_scene_blur_set_size(panel->blur, width, height);
			wlr_scene_blur_set_corner_radius(panel->blur, radius);
			wlr_scene_node_set_position(&panel->blur->node, x, y);
			wlr_scene_node_set_enabled(&panel->blur->node, true);
		}
	}
	if (config.bar_panel_shadow && config.shadows) {
		if (!panel->shadow)
			panel->shadow = wlr_scene_shadow_create(
				bar_popover.panel_tree, width, height, radius,
				config.shadows_blur, config.shadowscolor);
		if (panel->shadow) {
			wlr_scene_shadow_set_size(panel->shadow, width, height);
			wlr_scene_shadow_set_corner_radius(panel->shadow, radius);
			wlr_scene_node_set_position(&panel->shadow->node, x, y);
			wlr_scene_node_set_enabled(&panel->shadow->node, true);
			wlr_scene_node_lower_to_bottom(&panel->shadow->node);
		}
	}
	if (!panel->bg)
		panel->bg = wlr_scene_rect_create(bar_popover.panel_tree, width, height,
										  config.bar_popover_color);
	if (panel->bg) {
		wlr_scene_rect_set_size(panel->bg, width, height);
		wlr_scene_rect_set_color(panel->bg, config.bar_popover_color);
		wlr_scene_rect_set_corner_radius(panel->bg, radius);
		wlr_scene_node_set_position(&panel->bg->node, x, y);
		wlr_scene_node_set_enabled(&panel->bg->node, true);
		if (panel->blur)
			wlr_scene_node_place_above(&panel->bg->node, &panel->blur->node);
	}

	int32_t ry = y + pad;
	for (int32_t i = 0; i < bar_popover.nrows; i++) {
		BarPopoverRow *r = &bar_popover.rows[i];
		if (!r->used)
			continue;
		asteroidz_tab_bar_node_set_size(r->node, inner_w, row_h);
		asteroidz_tab_bar_node_apply_config(r->node, &config.theme);
		asteroidz_tab_bar_node_set_padding(r->node, config.bar_popover_padding,
										   config.theme.padding_y);
		asteroidz_tab_bar_node_set_text_align_left(r->node, true);
		/* the active entry is the one filled, like the selected tag chip */
		asteroidz_tab_bar_node_set_focus(r->node, r->selected);
		asteroidz_tab_bar_node_set_position(r->node, x + pad, ry);
		asteroidz_tab_bar_node_set_enabled(r->node, true);
		asteroidz_tab_bar_node_update(r->node, r->text, scale);
		ry += row_h + config.bar_popover_spacing;
	}
}

/* ─── open / close ────────────────────────────────────────────────────────── */

static void bar_popover_close(void) {
	if (!bar_popover.open && !bar_popover.tree)
		return;
	for (int32_t i = 0; i < BAR_POPOVER_MAX_ROWS; i++)
		bar_popover_row_release(&bar_popover.rows[i]);
	if (bar_popover.tree) {
		/* the panel nodes are children, so this takes them too */
		wlr_scene_node_destroy(&bar_popover.tree->node);
		bar_popover.tree = NULL;
	}
	bar_popover.panel_tree = NULL;
	bar_popover.row_tree = NULL;
	bar_popover.panel = (BarPanel){0};
	bar_popover.nrows = 0;
	bar_popover.kind = BAR_POPOVER_NONE;
	bar_popover.open = false;
	bar_popover.mon = NULL;
}

/* Returns false if the popover could not be created. `anchor_x` is the centre
 * of the pill it should hang from. */
static bool bar_popover_open(Monitor *m, enum bar_popover_kind kind,
							 int32_t anchor_x) {
	bar_popover_close();
	if (!m)
		return false;
	/* LyrTop, like the bar itself, but created after it so it stacks above --
	 * a popover that rendered under its own bar would be invisible. */
	bar_popover.tree = wlr_scene_tree_create(layers[LyrTop]);
	if (!bar_popover.tree)
		return false;
	/* order matters: panel_tree first so it sits below row_tree */
	bar_popover.panel_tree = wlr_scene_tree_create(bar_popover.tree);
	bar_popover.row_tree = wlr_scene_tree_create(bar_popover.tree);
	if (!bar_popover.panel_tree || !bar_popover.row_tree) {
		wlr_scene_node_destroy(&bar_popover.tree->node);
		bar_popover.tree = NULL;
		return false;
	}
	wlr_scene_node_raise_to_top(&bar_popover.tree->node);
	bar_popover.mon = m;
	bar_popover.kind = kind;
	bar_popover.anchor_x = anchor_x;
	bar_popover.open = true;
	bar_popover.nrows = 0;
	return true;
}

static bool bar_popover_is_open(enum bar_popover_kind kind) {
	return bar_popover.open && bar_popover.kind == kind;
}

/* ─── audio output picker ─────────────────────────────────────────────────── */

/* Two async commands, because the sink list and the current default come from
 * different pactl calls. The default arrives first and is stashed, so the list
 * callback can mark the right row without a second round trip. */
static char bar_popover_default_sink[192];

static void bar_popover_on_sinks(const char *out, size_t len, void *user) {
	(void)len;
	(void)user;
	if (!bar_popover_is_open(BAR_POPOVER_SINKS))
		return;

	cJSON *root = cJSON_Parse(out);
	if (!root) {
		bar_popover_close();
		return;
	}
	int32_t n = 0;
	cJSON *sink = NULL;
	cJSON_ArrayForEach(sink, root) {
		if (n >= BAR_POPOVER_MAX_ROWS)
			break;
		cJSON *name = cJSON_GetObjectItem(sink, "name");
		cJSON *desc = cJSON_GetObjectItem(sink, "description");
		if (!cJSON_IsString(name) || !name->valuestring)
			continue;
		BarPopoverRow *r = bar_popover_row_get(n);
		if (!r)
			break;
		snprintf(r->value, sizeof(r->value), "%s", name->valuestring);
		/* description is the human-readable one ("Desktop Audio"); the node
		 * name is an alsa path and is only a fallback */
		snprintf(r->text, sizeof(r->text), "%s",
				 cJSON_IsString(desc) && desc->valuestring ? desc->valuestring
														   : name->valuestring);
		r->selected = strcmp(r->value, bar_popover_default_sink) == 0;
		n++;
	}
	cJSON_Delete(root);
	bar_popover.nrows = n;
	if (n == 0) {
		bar_popover_close();
		return;
	}
	bar_popover_layout();
}

static void bar_popover_on_default_sink(const char *out, size_t len,
										void *user) {
	(void)len;
	(void)user;
	if (!bar_popover_is_open(BAR_POPOVER_SINKS))
		return;
	snprintf(bar_popover_default_sink, sizeof(bar_popover_default_sink), "%s",
			 out);
	/* strip the trailing newline pactl leaves */
	char *nl = strchr(bar_popover_default_sink, '\n');
	if (nl)
		*nl = '\0';

	char *const argv[] = {"pactl", "-f", "json", "list", "sinks", NULL};
	if (!async_spawn(event_loop, argv, bar_popover_on_sinks, NULL))
		bar_popover_close();
}

static void bar_popover_open_sinks(Monitor *m, int32_t anchor_x) {
	if (bar_popover_is_open(BAR_POPOVER_SINKS)) {
		bar_popover_close(); /* clicking the pill again dismisses */
		return;
	}
	if (!bar_popover_open(m, BAR_POPOVER_SINKS, anchor_x))
		return;
	bar_popover_default_sink[0] = '\0';
	char *const argv[] = {"pactl", "get-default-sink", NULL};
	if (!async_spawn(event_loop, argv, bar_popover_on_default_sink, NULL))
		bar_popover_close();
}

/* ─── input ───────────────────────────────────────────────────────────────── */

static bool bar_popover_handle_node_click(AsteroidzNodeData *hit,
										  uint32_t button) {
	BarPopoverRow *r = hit ? (BarPopoverRow *)hit->node_data : NULL;
	if (!r || !r->used || button != BTN_LEFT)
		return false;
	switch (bar_popover.kind) {
	case BAR_POPOVER_SINKS: {
		char *const argv[] = {"pactl", "set-default-sink", r->value, NULL};
		async_spawn(event_loop, argv, NULL, NULL);
		break;
	}
	default:
		break;
	}
	bar_popover_close();
	return true;
}

/* Any click that is NOT on a popover row dismisses. Returns true when the
 * click was spent doing so, which the caller must honour: a click that
 * dismisses a menu should not also press whatever it landed on. */
static bool bar_popover_dismiss_click(struct wlr_scene_node *node) {
	if (!bar_popover.open)
		return false;
	if (node && node->data) {
		AsteroidzNodeData *d = (AsteroidzNodeData *)node->data;
		if (d->type == ASTEROIDZ_BAR_POPOVER_NODE)
			return false; /* the row handler deals with it */
	}
	bar_popover_close();
	return true;
}

/* Escape closes. Returns true when it was consumed. */
static bool bar_popover_handle_key(uint32_t keysym) {
	if (!bar_popover.open || keysym != XKB_KEY_Escape)
		return false;
	bar_popover_close();
	return true;
}

#endif /* ASTEROIDZ_BAR_POPOVER_H */
