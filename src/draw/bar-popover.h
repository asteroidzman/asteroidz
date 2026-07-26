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

/* Generous, because a real application menu is long: Steam's lists every
 * installed game before Store/Library/Community and only then Quit. Truncating
 * at a dozen silently loses the entry people actually reach for. */
#define BAR_POPOVER_MAX_ROWS 32

/* Marks a row payload as a VERB ("leave", "hdr") rather than an identifier a
 * module looks up (a sink name, a Discord snowflake, an output). One byte no
 * identifier can contain, so the two need no second field to tell apart.
 *
 * Spelled as a macro and CONCATENATED -- BAR_POPOVER_VERB "back" -- never
 * inline as "\x01back": a hex escape has no length limit, so it swallows the
 * b, a and c as digits and yields one out-of-range character followed by "k".
 * That silently mis-routed the per-output back row into the HDR toggle. Octal
 * would also be safe (three digits max), but the name says what the byte is
 * for, which the byte itself never could. */
#define BAR_POPOVER_VERB "\001"
#define BAR_POPOVER_IS_VERB(v) ((v)[0] == BAR_POPOVER_VERB[0])

/* What the rows mean, so a click knows how to act on the payload. */
enum bar_popover_kind {
	BAR_POPOVER_NONE = 0,
	BAR_POPOVER_SINKS, /* audio outputs; row payload is the pactl sink name */
	BAR_POPOVER_MENU,  /* a tray item's DBusMenu; row payload is the item id */
	BAR_POPOVER_VOICE, /* discord voice channels; row payload is guild:channel */
	BAR_POPOVER_VPN,   /* nordvpn; row payload is a country, or a marked verb */
	BAR_POPOVER_OUTPUTS, /* every monitor; row payload is the output name */
	BAR_POPOVER_OUTPUT,  /* one monitor's settings; payload is a marked verb */
	/* Choice lists: a value picked from a set, then back to the output. Their
	 * payload is the value itself (a mode spelled "WxH@mHz", or a scale), so
	 * the click handler needs no index into a list that may have changed
	 * underneath it -- an output can lose a mode while its menu is open. */
	BAR_POPOVER_MODES,
	BAR_POPOVER_SCALES,
	BAR_POPOVER_MEDS,    /* today's doses; payload is the dose key */
	BAR_POPOVER_MED_DOSE, /* one dose: take / skip / postpone */
	BAR_POPOVER_SYSINFO,  /* cpu/memory/network figures; every row inert */
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
	/* DBusMenu item id, meaningless for other kinds */
	int32_t id;
	bool enabled;
	/* A row whose VALUE changes under the pointer rather than one that acts
	 * once. Adjusted by scrolling over it: the popover has no sub-row hit
	 * testing -- a row is one scene node -- so painting -/+ zones would be
	 * decoration that lies about where you may click. Scroll is already routed
	 * to whatever node is under the pointer, so it costs no new input model. */
	bool stepper;
	int32_t step_value, vmin, vmax, vstep;
	bool submenu;
	bool separator;
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
	/* Whose com.canonical.dbusmenu the rows came from, for BAR_POPOVER_MENU.
	 * Held on the popover rather than per row: every row of one menu belongs
	 * to the same object, and a row must not outlive it. */
	char menu_service[128];
	char menu_path[128];
	int32_t menu_parent; /* the id whose children are shown; 0 = top level */
	/* which output BAR_POPOVER_OUTPUT is showing; held by NAME because a
	 * Monitor can be unplugged while its menu is open */
	char output_name[64];
	/* which dose BAR_POPOVER_MED_DOSE is acting on, by key for the same
	 * reason: the store can be reloaded while the menu is open */
	char med_key[176];
	/* horizontal centre of the pill this hangs from, in layout coordinates */
	int32_t anchor_x;
	struct wlr_box box;
	bool open;
} BarPopover;

static BarPopover bar_popover;

static void bar_popover_close(void);
static void bar_popover_layout(void);
static void bar_popover_open_modes(Monitor *anchor_mon, int32_t anchor_x,
								   const char *name);
static void bar_popover_open_meds(Monitor *m, int32_t anchor_x);
static void bar_popover_open_med_dose(Monitor *anchor_mon, int32_t anchor_x,
									  const char *key);
static void bar_popover_open_scales(Monitor *anchor_mon, int32_t anchor_x,
									const char *name);

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
	r->id = 0;
	r->enabled = true;
	r->stepper = false;
	r->step_value = r->vmin = r->vmax = r->vstep = 0;
	r->submenu = false;
	r->separator = false;
	r->selected = false;
}

static BarPopoverRow *bar_popover_row_get(int32_t idx) {
	if (idx < 0 || idx >= BAR_POPOVER_MAX_ROWS || !bar_popover.row_tree)
		return NULL;
	BarPopoverRow *r = &bar_popover.rows[idx];
	if (r->used)
		return r;

	/* A fresh slot needs the same defaults a released one gets. The rows live
	 * in a zero-initialised static array, so `enabled` starts FALSE -- and the
	 * per-output and VPN handlers both begin with "if (!r->enabled) return",
	 * meaning every row of those menus was dead on its first use. It survived
	 * because reaching it needs a real click on a real menu: the HDR toggle,
	 * every VPN country, and both new choice rows did nothing at all. */
	r->enabled = true;
	r->stepper = false;
	r->submenu = false;
	r->separator = false;
	r->selected = false;
	r->id = 0;
	r->text[0] = '\0';
	r->value[0] = '\0';

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

	/* Clamp to the output. A long menu on a short screen would otherwise run
	 * off the bottom, taking its last entries -- which is where Quit lives --
	 * with it. Rows past what fits are hidden rather than drawn off-screen;
	 * scrolling a popover needs a viewport this cut does not have. */
	int32_t avail = m->m.height - config.bar_margin_y * 2 - config.bar_height -
					config.bar_popover_gap;
	if (height > avail && row_h + config.bar_popover_spacing > 0) {
		int32_t fits = (avail - 2 * pad + config.bar_popover_spacing) /
					   (row_h + config.bar_popover_spacing);
		if (fits < 1)
			fits = 1;
		if (fits < nrows) {
			int32_t seen = 0;
			for (int32_t i = 0; i < bar_popover.nrows; i++) {
				if (!bar_popover.rows[i].used)
					continue;
				if (seen++ >= fits)
					bar_popover_row_release(&bar_popover.rows[i]);
			}
			nrows = fits;
			height = nrows * row_h +
					 (nrows - 1) * config.bar_popover_spacing + 2 * pad;
		}
	}

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
		if (!r->selected && (!r->enabled || r->separator)) {
			/* Greyed AND untiled: a disabled entry has to read as
			 * unavailable, or clicking it and getting nothing looks like a
			 * broken menu -- and a filled tile is what every other row uses to
			 * say "this is a target". Dropping the background is the same
			 * distinction the bar itself draws between a flat pill and a
			 * filled one, and it matters most on a panel that is ENTIRELY
			 * readings (sysinfo), where a stack of tiles reads as a menu whose
			 * every entry is dead. */
			static const float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
			float fg[4];
			memcpy(fg, config.theme.fg_color, sizeof(fg));
			fg[3] *= r->separator ? 0.25f : 0.55f;
			asteroidz_tab_bar_node_set_colors(r->node, fg, clear);
		}
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
	/* Scroll-to-adjust volume, above the output list. This is the closest
	 * thing to the slider the waybar popover had that an honest row can be:
	 * the number moves under the pointer, and nothing is drawn that cannot be
	 * clicked. */
	BarPopoverRow *vol = bar_popover_row_get(n);
	if (vol) {
		vol->stepper = true;
		vol->step_value = bar_volume.have ? bar_volume.pct : 0;
		vol->vmin = 0;
		vol->vmax = 100;
		vol->vstep = config.bar_volume_step;
		snprintf(vol->text, sizeof(vol->text), "Volume  %d%%", vol->step_value);
		n++;
	}
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

/* ─── tray context menus (com.canonical.dbusmenu) ─────────────────────────── */

/* The menu half of the StatusNotifierItem world. An item's `Menu` property
 * names an object implementing com.canonical.dbusmenu, whose GetLayout returns
 * a recursive tree:
 *
 *   GetLayout(parentId, depth, props) -> (u revision, (ia{sv}av) layout)
 *
 * where each element of the `av` is a variant wrapping another (ia{sv}av). We
 * ask for depth 1 and render one level at a time: a submenu is a row that
 * re-opens the popover against that row's id, which keeps the drawing code
 * flat and means an application with a deep menu costs one round trip per
 * level entered rather than one enormous reply up front.
 *
 * Everything is async, like the rest of the tray. */
#define BAR_MENU_IFACE "com.canonical.dbusmenu"

static void bar_popover_open_menu(Monitor *m, int32_t anchor_x,
								  const char *service, const char *path,
								  int32_t parent);

/* Read one child's a{sv} into `r`. Leaves the message positioned after the
 * property dict. */
static void bar_popover_read_menu_props(sd_bus_message *msg, BarPopoverRow *r) {
	if (sd_bus_message_enter_container(msg, 'a', "{sv}") <= 0)
		return;
	while (sd_bus_message_enter_container(msg, 'e', "sv") > 0) {
		const char *key = NULL;
		if (sd_bus_message_read(msg, "s", &key) <= 0 || !key) {
			sd_bus_message_exit_container(msg);
			break;
		}
		if (!strcmp(key, "label")) {
			const char *v = NULL;
			if (sd_bus_message_enter_container(msg, 'v', "s") > 0) {
				if (sd_bus_message_read(msg, "s", &v) > 0 && v) {
					/* DBusMenu labels carry GTK mnemonics ("_Quit"); strip the
					 * marker rather than rendering a stray underscore */
					size_t o = 0;
					for (const char *c = v; *c && o + 1 < sizeof(r->text); c++) {
						if (*c == '_' && c[1] == '_')
							c++;
						else if (*c == '_')
							continue;
						r->text[o++] = *c;
					}
					r->text[o] = '\0';
				}
				sd_bus_message_exit_container(msg);
			} else {
				sd_bus_message_skip(msg, "v");
			}
		} else if (!strcmp(key, "enabled") || !strcmp(key, "visible")) {
			int v = 1;
			if (sd_bus_message_enter_container(msg, 'v', "b") > 0) {
				sd_bus_message_read(msg, "b", &v);
				sd_bus_message_exit_container(msg);
			} else {
				sd_bus_message_skip(msg, "v");
			}
			if (!strcmp(key, "enabled"))
				r->enabled = v != 0;
			else if (!v)
				r->separator = true; /* invisible: drawn out of the way */
		} else if (!strcmp(key, "type")) {
			const char *v = NULL;
			if (sd_bus_message_enter_container(msg, 'v', "s") > 0) {
				if (sd_bus_message_read(msg, "s", &v) > 0 && v &&
					!strcmp(v, "separator"))
					r->separator = true;
				sd_bus_message_exit_container(msg);
			} else {
				sd_bus_message_skip(msg, "v");
			}
		} else if (!strcmp(key, "children-display")) {
			const char *v = NULL;
			if (sd_bus_message_enter_container(msg, 'v', "s") > 0) {
				if (sd_bus_message_read(msg, "s", &v) > 0 && v &&
					!strcmp(v, "submenu"))
					r->submenu = true;
				sd_bus_message_exit_container(msg);
			} else {
				sd_bus_message_skip(msg, "v");
			}
		} else if (!strcmp(key, "toggle-state")) {
			int v = 0;
			if (sd_bus_message_enter_container(msg, 'v', "i") > 0) {
				sd_bus_message_read(msg, "i", &v);
				sd_bus_message_exit_container(msg);
			} else {
				sd_bus_message_skip(msg, "v");
			}
			r->selected = v > 0; /* checked entries read like the active row */
		} else {
			sd_bus_message_skip(msg, "v");
		}
		sd_bus_message_exit_container(msg);
	}
	sd_bus_message_exit_container(msg);
}

static int bar_popover_on_layout(sd_bus_message *msg, void *user,
								 sd_bus_error *err) {
	(void)user;
	(void)err;
	if (!bar_popover_is_open(BAR_POPOVER_MENU))
		return 0;
	if (!msg || sd_bus_message_is_method_error(msg, NULL)) {
		bar_popover_close();
		return 0;
	}

	uint32_t revision = 0;
	if (sd_bus_message_read(msg, "u", &revision) <= 0 ||
		sd_bus_message_enter_container(msg, 'r', "ia{sv}av") <= 0) {
		bar_popover_close();
		return 0;
	}
	int32_t root_id = 0;
	sd_bus_message_read(msg, "i", &root_id);
	sd_bus_message_skip(msg, "a{sv}"); /* the root's own properties */

	int32_t n = 0;
	if (sd_bus_message_enter_container(msg, 'a', "v") > 0) {
		while (sd_bus_message_enter_container(msg, 'v', "(ia{sv}av)") > 0) {
			if (sd_bus_message_enter_container(msg, 'r', "ia{sv}av") > 0) {
				int32_t id = 0;
				sd_bus_message_read(msg, "i", &id);
				BarPopoverRow *r =
					n < BAR_POPOVER_MAX_ROWS ? bar_popover_row_get(n) : NULL;
				if (r) {
					r->id = id;
					r->enabled = true;
					bar_popover_read_menu_props(msg, r);
					if (r->separator && !r->text[0])
						snprintf(r->text, sizeof(r->text), "%s",
								 "\u2500\u2500\u2500");
					if (r->submenu) {
						size_t l = strlen(r->text);
						snprintf(r->text + l, sizeof(r->text) - l, "  %s",
								 "\u203a");
					}
					if (r->text[0])
						n++;
					else
						bar_popover_row_release(r);
				} else {
					sd_bus_message_skip(msg, "a{sv}");
				}
				sd_bus_message_skip(msg, "av"); /* grandchildren */
				sd_bus_message_exit_container(msg);
			}
			sd_bus_message_exit_container(msg);
		}
		sd_bus_message_exit_container(msg);
	}
	sd_bus_message_exit_container(msg);

	bar_popover.nrows = n;
	if (n == 0) {
		bar_popover_close();
		return 0;
	}
	bar_popover_layout();
	return 0;
}

static void bar_popover_open_menu(Monitor *m, int32_t anchor_x,
								  const char *service, const char *path,
								  int32_t parent) {
	if (!session_bus || !service || !*service || !path || !*path)
		return;
	if (!bar_popover_open(m, BAR_POPOVER_MENU, anchor_x))
		return;
	snprintf(bar_popover.menu_service, sizeof(bar_popover.menu_service), "%s",
			 service);
	snprintf(bar_popover.menu_path, sizeof(bar_popover.menu_path), "%s", path);
	bar_popover.menu_parent = parent;

	/* AboutToShow first: applications populate their menu lazily on it, and
	 * skipping it gets an empty or stale layout from anything Qt-based.
	 * Fire-and-forget -- its bool reply only says whether the layout changed,
	 * and we are about to fetch it either way. */
	sd_bus_call_method_async(session_bus, NULL, service, path, BAR_MENU_IFACE,
							 "AboutToShow", NULL, NULL, "i", parent);

	int r = sd_bus_call_method_async(session_bus, NULL, service, path,
									 BAR_MENU_IFACE, "GetLayout",
									 bar_popover_on_layout, NULL, "iias",
									 parent, 1, 0);
	if (r < 0) {
		wlr_log(WLR_DEBUG, "tray menu: GetLayout on %s: %s", service,
				strerror(-r));
		bar_popover_close();
		return;
	}
	sd_bus_flush(session_bus);
}

/* ─── discord voice channels ──────────────────────────────────────────────── */

/* Guild/channel list from the daemon's last `channels` snapshot, plus the two
 * actions worth having on a bar: leave, and mute. Joining is a row click.
 *
 * Built from cached state rather than a request, so opening it is instant and
 * works even if the daemon is momentarily busy. */
static void bar_popover_open_voice(Monitor *m, int32_t anchor_x) {
	if (bar_popover_is_open(BAR_POPOVER_VOICE)) {
		bar_popover_close();
		return;
	}
	if (!bar_popover_open(m, BAR_POPOVER_VOICE, anchor_x))
		return;

	int32_t n = 0;
	if (bar_dv.state == BAR_DV_CONNECTED) {
		BarPopoverRow *r = bar_popover_row_get(n);
		if (r) {
			snprintf(r->text, sizeof(r->text), "%s",
					 bar_dv.muted ? "Unmute" : "Mute");
			snprintf(r->value, sizeof(r->value), "%s", BAR_POPOVER_VERB "mute");
			n++;
		}
		r = bar_popover_row_get(n);
		if (r) {
			snprintf(r->text, sizeof(r->text), "%s", "Leave");
			snprintf(r->value, sizeof(r->value), "%s", BAR_POPOVER_VERB "leave");
			n++;
		}
	}

	for (int32_t i = 0; i < bar_dv.nchannels && n < BAR_POPOVER_MAX_ROWS; i++) {
		BarDvChannel *c = &bar_dv.channels[i];
		BarPopoverRow *r = bar_popover_row_get(n);
		if (!r)
			break;
		/* "guild / channel" with a headcount, so a busy room is obvious
		 * without opening Discord itself */
		if (c->people > 0)
			snprintf(r->text, sizeof(r->text), "%s / %s  (%d)",
					 c->guild_name[0] ? c->guild_name : "server", c->name,
					 c->people);
		else
			snprintf(r->text, sizeof(r->text), "%s / %s",
					 c->guild_name[0] ? c->guild_name : "server", c->name);
		/* the join command needs both ids, so carry them together */
		snprintf(r->value, sizeof(r->value), "%s\x1f%s", c->guild, c->id);
		r->selected = strcmp(c->id, bar_dv.channel_id) == 0;
		n++;
	}

	bar_popover.nrows = n;
	if (n == 0) {
		bar_popover_close();
		return;
	}
	bar_popover_layout();
}

/* ─── nordvpn ─────────────────────────────────────────────────────────────── */

/* Where you are connected, then the two verbs, then countries to connect to.
 * The status lines are inert rows rather than a separate widget -- a popover
 * has one row type, and a disabled row already reads as information. */
static void bar_popover_open_vpn(Monitor *m, int32_t anchor_x) {
	if (bar_popover_is_open(BAR_POPOVER_VPN)) {
		bar_popover_close();
		return;
	}
	if (!bar_popover_open(m, BAR_POPOVER_VPN, anchor_x))
		return;
	bar_vpn_fetch_countries();

	int32_t n = 0;
	BarPopoverRow *r = NULL;

	if (bar_vpn.state == BAR_VPN_UNAVAILABLE) {
		r = bar_popover_row_get(n);
		if (r) {
			snprintf(r->text, sizeof(r->text), "%s",
					 "nordvpn CLI not available");
			r->enabled = false;
			n++;
		}
	} else if (bar_vpn.state == BAR_VPN_CONNECTED) {
		if ((r = bar_popover_row_get(n))) {
			snprintf(r->text, sizeof(r->text), "%s%s%s",
					 bar_vpn.country[0] ? bar_vpn.country : "Connected",
					 bar_vpn.server[0] ? " · " : "", bar_vpn.server);
			r->enabled = false;
			r->selected = true;
			n++;
		}
		if (bar_vpn.ip[0] && (r = bar_popover_row_get(n))) {
			snprintf(r->text, sizeof(r->text), "IP %s", bar_vpn.ip);
			r->enabled = false;
			n++;
		}
		if (bar_vpn.uptime[0] && (r = bar_popover_row_get(n))) {
			snprintf(r->text, sizeof(r->text), "Up %s", bar_vpn.uptime);
			r->enabled = false;
			n++;
		}
		if ((r = bar_popover_row_get(n))) {
			snprintf(r->text, sizeof(r->text), "%s", "Disconnect");
			snprintf(r->value, sizeof(r->value), "%s", BAR_POPOVER_VERB "off");
			n++;
		}
	} else {
		if ((r = bar_popover_row_get(n))) {
			snprintf(r->text, sizeof(r->text), "%s", "Quick Connect");
			snprintf(r->value, sizeof(r->value), "%s", BAR_POPOVER_VERB "quick");
			n++;
		}
	}

	for (int32_t i = 0; i < bar_vpn.ncountries && n < BAR_POPOVER_MAX_ROWS;
		 i++) {
		if (!(r = bar_popover_row_get(n)))
			break;
		snprintf(r->text, sizeof(r->text), "%s", bar_vpn.countries[i]);
		snprintf(r->value, sizeof(r->value), "%s", bar_vpn.countries[i]);
		n++;
	}

	bar_popover.nrows = n;
	if (n == 0) {
		bar_popover_close();
		return;
	}
	bar_popover_layout();
}

/* ─── outputs ─────────────────────────────────────────────────────────────── */

static void bar_popover_open_output(Monitor *anchor_mon, int32_t anchor_x,
									const char *name);

/* Every connected output, current one marked. A row drills into that output's
 * own settings -- the same one-level-at-a-time shape the tray menus use. */
static void bar_popover_open_outputs(Monitor *m, int32_t anchor_x) {
	if (bar_popover_is_open(BAR_POPOVER_OUTPUTS)) {
		bar_popover_close();
		return;
	}
	if (!bar_popover_open(m, BAR_POPOVER_OUTPUTS, anchor_x))
		return;

	int32_t n = 0;

	/* SDR white, when it can do anything.
	 *
	 * This is the luminance an SDR surface is mapped to inside an HDR output's
	 * pipeline -- the single value that decides how bright the ordinary
	 * desktop looks once HDR is on, and the one people actually reach for.
	 * It is scene-wide rather than per-output, so it belongs on THIS panel
	 * ("displays in general") and not inside one output's own.
	 *
	 * Hidden while no output is in HDR, because with none it changes nothing
	 * you can see: a control that visibly does nothing is worse than an
	 * absent one. Same reason the Resolution row is hidden on an output with
	 * no mode list.
	 *
	 * The per-output mastering/max-CLL/max-FALL values are deliberately NOT
	 * offered beside it. Those describe what the PANEL can do -- they are
	 * forwarded to the display so its tone-mapper knows what it is being
	 * handed -- so a stepper on them would be inventing hardware facts, and
	 * an unset one has no honest starting value to step away from. They stay
	 * output rules in the config, where a claim about your hardware belongs. */
	bool any_hdr = false;
	Monitor *hm = NULL;
	wl_list_for_each(hm, &mons, link) {
		if (hm->wlr_output && hm->wlr_output->enabled && hm->hdr)
			any_hdr = true;
	}
	if (any_hdr) {
		BarPopoverRow *sdr = bar_popover_row_get(n);
		if (sdr) {
			sdr->stepper = true;
			/* 0 means "unset" in the config and 203 is the spec's default,
			 * which is what the scene is actually running at */
			sdr->step_value = (int32_t)(config.sdr_reference_luminance > 0
											? config.sdr_reference_luminance
											: 203.0f);
			/* the rails set_sdr_luminance itself clamps to, so the row cannot
			 * display a value the dispatcher would refuse */
			sdr->vmin = 80;
			sdr->vmax = 1000;
			sdr->vstep = 10;
			snprintf(sdr->value, sizeof(sdr->value), "%s",
					 BAR_POPOVER_VERB "sdr");
			snprintf(sdr->text, sizeof(sdr->text), "SDR white  %d cd/m²",
					 sdr->step_value);
			n++;
		}
	}

	Monitor *it = NULL;
	wl_list_for_each(it, &mons, link) {
		if (n >= BAR_POPOVER_MAX_ROWS || !it->wlr_output)
			continue;
		BarPopoverRow *r = bar_popover_row_get(n);
		if (!r)
			break;
		char sum[BAR_TEXT_MAX];
		bar_display_summary(it, sum, sizeof(sum));
		snprintf(r->text, sizeof(r->text), "%s%s  \u203a", sum,
				 it->hdr ? "  HDR" : "");
		snprintf(r->value, sizeof(r->value), "%s", it->wlr_output->name);
		r->selected = (it == m);
		r->submenu = true;
		n++;
	}
	bar_popover.nrows = n;
	if (n == 0) {
		bar_popover_close();
		return;
	}
	bar_popover_layout();
}

/* One output's controls: the HDR toggle, and the resolution and scale lists it
 * drills into. SDR white lives one level up, on the outputs panel, because it
 * is scene-wide rather than a property of this output. */
static void bar_popover_open_output(Monitor *anchor_mon, int32_t anchor_x,
									const char *name) {
	Monitor *target = bar_display_by_name(name);
	if (!target)
		return;
	if (!bar_popover_open(anchor_mon, BAR_POPOVER_OUTPUT, anchor_x))
		return;
	snprintf(bar_popover.output_name, sizeof(bar_popover.output_name), "%s",
			 name);

	int32_t n = 0;
	BarPopoverRow *r = bar_popover_row_get(n);
	if (r) {
		char sum[BAR_TEXT_MAX];
		bar_display_summary(target, sum, sizeof(sum));
		snprintf(r->text, sizeof(r->text), "%s", sum);
		r->enabled = false;
		n++;
	}
	if ((r = bar_popover_row_get(n))) {
		snprintf(r->text, sizeof(r->text), "HDR  %s",
				 target->hdr ? "on" : "off");
		snprintf(r->value, sizeof(r->value), "%s", BAR_POPOVER_VERB "hdr");
		r->selected = target->hdr != 0;
		n++;
	}
	if (target->hdr_capability_failed && (r = bar_popover_row_get(n))) {
		/* the output said no to BT.2020+PQ; say so rather than leave a toggle
		 * that silently never takes */
		snprintf(r->text, sizeof(r->text), "%s", "output cannot do HDR");
		r->enabled = false;
		n++;
	}
	/* Choice rows: the value is shown inline and the row drills into a list,
	 * the same shape the tray's submenus use. A resolution cannot be a stepper
	 * -- the valid values are an arbitrary set the panel dictates, not a
	 * range -- so scrolling one would either invent modes or lie about them. */
	if (!wl_list_empty(&target->wlr_output->modes) &&
		(r = bar_popover_row_get(n))) {
		int32_t hz = bar_display_hz(target);
		snprintf(r->text, sizeof(r->text), "Resolution  %dx%d@%d  \u203a",
				 target->wlr_output->width, target->wlr_output->height, hz);
		snprintf(r->value, sizeof(r->value), "%s", BAR_POPOVER_VERB "modes");
		r->submenu = true;
		n++;
	}
	if ((r = bar_popover_row_get(n))) {
		snprintf(r->text, sizeof(r->text), "Scale  %.2f  \u203a",
				 (double)target->wlr_output->scale);
		snprintf(r->value, sizeof(r->value), "%s", BAR_POPOVER_VERB "scales");
		r->submenu = true;
		n++;
	}
	if ((r = bar_popover_row_get(n))) {
		snprintf(r->text, sizeof(r->text), "%s", "\u2039  All outputs");
		snprintf(r->value, sizeof(r->value), "%s", BAR_POPOVER_VERB "back");
		n++;
	}
	bar_popover.nrows = n;
	bar_popover_layout();
}

/* ─── choice lists ────────────────────────────────────────────────────────── */

/* Every mode the output reports, current one marked.
 *
 * The list is what the panel says it can do, in the order it says it -- not
 * sorted or de-duplicated. Two entries that read identically (3840x2160@60
 * appears twice on this desktop's DP-1) are genuinely different modes to the
 * driver, and collapsing them would pick one on the user's behalf. */
static void bar_popover_open_modes(Monitor *anchor_mon, int32_t anchor_x,
								   const char *name) {
	Monitor *target = bar_display_by_name(name);
	if (!target || !target->wlr_output)
		return;
	if (!bar_popover_open(anchor_mon, BAR_POPOVER_MODES, anchor_x))
		return;
	snprintf(bar_popover.output_name, sizeof(bar_popover.output_name), "%s",
			 name);

	int32_t n = 0;
	struct wlr_output_mode *mode;
	wl_list_for_each(mode, &target->wlr_output->modes, link) {
		if (n >= BAR_POPOVER_MAX_ROWS - 1)
			break;
		BarPopoverRow *r = bar_popover_row_get(n);
		if (!r)
			break;
		snprintf(r->text, sizeof(r->text), "%dx%d@%d%s", mode->width,
				 mode->height, (int32_t)((mode->refresh + 500) / 1000),
				 mode->preferred ? "  (preferred)" : "");
		/* the mode's own numbers, so the click resolves it by value rather
		 * than by an index into a list that can change while this is open */
		snprintf(r->value, sizeof(r->value), "%d,%d,%d", mode->width,
				 mode->height, mode->refresh);
		r->selected = (mode == target->wlr_output->current_mode);
		n++;
	}
	BarPopoverRow *r = bar_popover_row_get(n);
	if (r) {
		snprintf(r->text, sizeof(r->text), "%s", "\u2039  Back");
		snprintf(r->value, sizeof(r->value), "%s", BAR_POPOVER_VERB "back");
		n++;
	}
	bar_popover.nrows = n;
	bar_popover_layout();
}

static void bar_popover_open_scales(Monitor *anchor_mon, int32_t anchor_x,
									const char *name) {
	Monitor *target = bar_display_by_name(name);
	if (!target || !target->wlr_output)
		return;
	if (!bar_popover_open(anchor_mon, BAR_POPOVER_SCALES, anchor_x))
		return;
	snprintf(bar_popover.output_name, sizeof(bar_popover.output_name), "%s",
			 name);

	int32_t n = 0;
	for (size_t i = 0; i < LENGTH(bar_display_scales); i++) {
		BarPopoverRow *r = bar_popover_row_get(n);
		if (!r)
			break;
		float s = bar_display_scales[i];
		/* the logical size this scale produces, because "1.25" means nothing
		 * on its own and the resulting desktop size is the actual choice */
		snprintf(r->text, sizeof(r->text), "%.2f    %dx%d", (double)s,
				 (int32_t)(target->wlr_output->width / s),
				 (int32_t)(target->wlr_output->height / s));
		snprintf(r->value, sizeof(r->value), "%.4f", (double)s);
		r->selected = fabsf(target->wlr_output->scale - s) < 0.01f;
		n++;
	}
	BarPopoverRow *r = bar_popover_row_get(n);
	if (r) {
		snprintf(r->text, sizeof(r->text), "%s", "\u2039  Back");
		snprintf(r->value, sizeof(r->value), "%s", BAR_POPOVER_VERB "back");
		n++;
	}
	bar_popover.nrows = n;
	bar_popover_layout();
}

/* ─── stepper rows ────────────────────────────────────────────────────────── */

/* Apply a stepper row's new value. Kept beside the popover rather than in each
 * module so a row's behaviour is visible where the row is built. */
static void bar_popover_apply_step(BarPopoverRow *r) {
	/* Steppers carrying a marked verb are dispatched on the VERB, not on the
	 * popover kind: a panel is allowed more than one of them, and keying the
	 * whole thing on the kind would silently apply the first one's action to
	 * the second. */
	if (BAR_POPOVER_IS_VERB(r->value)) {
		if (!strcmp(r->value, BAR_POPOVER_VERB "sdr")) {
			/* absolute, like the volume stepper: the row shows the value it
			 * is setting, so a relative step would drift from the reading */
			set_sdr_luminance(&(Arg){.i = 0, .f = (float)r->step_value});
			snprintf(r->text, sizeof(r->text), "SDR white  %d cd/m²",
					 r->step_value);
		}
		bar_popover_layout();
		return;
	}
	switch (bar_popover.kind) {
	case BAR_POPOVER_SINKS: {
		/* Absolute, not a delta. The row displays the value it is setting, so
		 * sending a relative step would drift away from the reading the moment
		 * anything else moved the volume. */
		char arg[32];
		snprintf(arg, sizeof(arg), "%d%%", r->step_value);
		char *const argv[] = {"wpctl",   "set-volume", "-l", "1.0",
							  "@DEFAULT_AUDIO_SINK@", arg, NULL};
		async_spawn(event_loop, argv, NULL, NULL);
		snprintf(r->text, sizeof(r->text), "Volume  %d%%", r->step_value);
		break;
	}
	default:
		break;
	}
	/* redraw in place: the whole point of a stepper is watching the number
	 * move as you scroll */
	bar_popover_layout();
}

/* Scroll over a popover row. Returns true when it was consumed. */
static bool bar_popover_handle_node_scroll(AsteroidzNodeData *hit,
										   int32_t notches) {
	BarPopoverRow *r = hit ? (BarPopoverRow *)hit->node_data : NULL;
	if (!r || !r->used || !r->stepper || notches == 0)
		return false;
	/* scroll up raises, matching the bar pills */
	int32_t v = r->step_value - notches * r->vstep;
	if (v < r->vmin)
		v = r->vmin;
	if (v > r->vmax)
		v = r->vmax;
	if (v == r->step_value)
		return true; /* already at the rail: consumed, nothing to do */
	r->step_value = v;
	bar_popover_apply_step(r);
	return true;
}

/* ─── medication ──────────────────────────────────────────────────────────── */

/* Today's doses, each drilling into its own actions.
 *
 * Every dose is listed, not just the due one: "did I take the morning one?" is
 * the question this exists to answer, and a menu that shows only what is due
 * cannot answer it. Status is spelled out per row for the same reason. */
static void bar_popover_open_meds(Monitor *m, int32_t anchor_x) {
	if (bar_popover_is_open(BAR_POPOVER_MEDS)) {
		bar_popover_close();
		return;
	}
	bar_med_reload();
	if (!bar_popover_open(m, BAR_POPOVER_MEDS, anchor_x))
		return;

	int32_t n = 0;
	for (int32_t i = 0; i < bar_med.ndoses && n < BAR_POPOVER_MAX_ROWS; i++) {
		BarMedDose *d = &bar_med.doses[i];
		BarPopoverRow *r = bar_popover_row_get(n);
		if (!r)
			break;
		/* Kept SHORT on purpose. The popover is a fixed 340px and rows
		 * ellipsise into it, and the first thing an over-long row loses is
		 * its tail -- which is the status, the one thing this menu exists to
		 * show. No chevron here for the same reason. */
		/* The marker LEADS. The popover is a fixed width and rows ellipsise
		 * into it, so a status written after a long medication name is the
		 * first thing lost -- and it is the one thing this menu exists to
		 * show. "escitalopram" alone is wide enough to prove the point. */
		const char *mark = !strcmp(d->status, "taken")     ? "\u2713"
						   : !strcmp(d->status, "skipped") ? "\u2715"
						   : d->due                        ? "\u25cf"
						   : d->pending                    ? "\u00b7"
														   : "!";
		snprintf(r->text, sizeof(r->text), "%s  %s  %s", mark, d->time,
				 d->name);
		snprintf(r->value, sizeof(r->value), "%s", d->key);
		r->selected = d->due;
		r->submenu = true;
		n++;
	}
	if (n == 0) {
		BarPopoverRow *r = bar_popover_row_get(n);
		if (r) {
			snprintf(r->text, sizeof(r->text), "%s", "nothing scheduled today");
			r->enabled = false;
			n++;
		}
	}
	bar_popover.nrows = n;
	bar_popover_layout();
}

static BarMedDose *bar_med_by_key(const char *key) {
	if (!key || !*key)
		return NULL;
	for (int32_t i = 0; i < bar_med.ndoses; i++)
		if (strcmp(bar_med.doses[i].key, key) == 0)
			return &bar_med.doses[i];
	return NULL;
}

/* What can be done to one dose. Take and Skip are offered even for a dose
 * already marked, because the answer to "I pressed the wrong one" has to be
 * pressing the right one -- the store keeps only the latest state per dose. */
static void bar_popover_open_med_dose(Monitor *anchor_mon, int32_t anchor_x,
									  const char *key) {
	BarMedDose *d = bar_med_by_key(key);
	if (!d)
		return;
	if (!bar_popover_open(anchor_mon, BAR_POPOVER_MED_DOSE, anchor_x))
		return;
	snprintf(bar_popover.med_key, sizeof(bar_popover.med_key), "%s", key);

	int32_t n = 0;
	BarPopoverRow *r = bar_popover_row_get(n);
	if (r) {
		snprintf(r->text, sizeof(r->text), "%s  %s", d->time, d->name);
		r->enabled = false;
		n++;
	}
	if ((r = bar_popover_row_get(n))) {
		snprintf(r->text, sizeof(r->text), "%s", "Take");
		snprintf(r->value, sizeof(r->value), "%s", BAR_POPOVER_VERB "take");
		r->selected = !strcmp(d->status, "taken");
		n++;
	}
	if ((r = bar_popover_row_get(n))) {
		snprintf(r->text, sizeof(r->text), "%s", "Skip");
		snprintf(r->value, sizeof(r->value), "%s", BAR_POPOVER_VERB "skip");
		r->selected = !strcmp(d->status, "skipped");
		n++;
	}
	if ((r = bar_popover_row_get(n))) {
		snprintf(r->text, sizeof(r->text), "Postpone %d min",
				 bar_med_snooze_minutes());
		snprintf(r->value, sizeof(r->value), "%s", BAR_POPOVER_VERB "snooze");
		n++;
	}
	if ((r = bar_popover_row_get(n))) {
		snprintf(r->text, sizeof(r->text), "%s", "\u2039  All doses");
		snprintf(r->value, sizeof(r->value), "%s", BAR_POPOVER_VERB "back");
		n++;
	}
	bar_popover.nrows = n;
	bar_popover_layout();
}

/* ─── system figures ──────────────────────────────────────────────────────── */

/* The bar's metric pills are deliberately numberless -- the colour is the
 * reading, because a percentage that changes every second is something you
 * cannot help reading and almost never need. This is where the numbers live:
 * clicking any of cpu, memory or network opens the SAME panel, because they
 * are three views of one machine and three separate popovers of four rows
 * each would be filing rather than answering.
 *
 * Every row is inert. Nothing here is a control -- there is no honest action
 * for "CPU 37%" -- so the panel is dismissed by Escape or a click outside it,
 * exactly like the disabled entries in a tray menu.
 *
 * Sampled when it OPENS, not on a timer. A popover that rewrites itself under
 * the pointer is hard to read, and these figures are being consulted, not
 * watched; the pill beside it is the live one. */

static void bar_sysinfo_bytes(char *out, size_t len, double bytes) {
	static const char *unit[] = {"B", "K", "M", "G", "T"};
	int u = 0;
	while (bytes >= 1024.0 && u < 4) {
		bytes /= 1024.0;
		u++;
	}
	snprintf(out, len, u == 0 ? "%.0f %s" : "%.1f %s", bytes, unit[u]);
}

/* The N processes holding the most resident memory, by a single pass over
 * /proc.
 *
 * RSS and not CPU on purpose: a process's CPU share is a RATE, and one sample
 * of /proc/<pid>/stat can only give its average since it started -- which for
 * a browser open since breakfast is a number that looks like a reading and is
 * not one. Reporting the wrong quantity confidently is worse than reporting
 * less. RSS is a level, so a single sample is the truth.
 *
 * Returns how many were filled. */
struct bar_sysinfo_proc {
	char name[32];
	uint64_t rss_kb;
};

static int32_t bar_sysinfo_top_rss(struct bar_sysinfo_proc *out, int32_t max) {
	DIR *d = opendir("/proc");
	if (!d)
		return 0;
	int32_t n = 0;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (e->d_name[0] < '0' || e->d_name[0] > '9')
			continue;
		char path[300];
		snprintf(path, sizeof(path), "/proc/%s/statm", e->d_name);
		FILE *f = fopen(path, "re");
		if (!f)
			continue;
		unsigned long size = 0, resident = 0;
		int got = fscanf(f, "%lu %lu", &size, &resident);
		fclose(f);
		if (got < 2 || resident == 0)
			continue;
		/* statm counts PAGES; the bar reports bytes like everything else.
		 * sysconf, not getpagesize(): the latter is not declared under
		 * _POSIX_C_SOURCE=200809L, which this is built with. */
		uint64_t rss_kb =
			(uint64_t)resident * (uint64_t)(sysconf(_SC_PAGESIZE) / 1024);

		/* keep only if it beats the weakest entry we are holding */
		if (n == max && rss_kb <= out[n - 1].rss_kb)
			continue;

		char name[32] = "";
		snprintf(path, sizeof(path), "/proc/%s/comm", e->d_name);
		if ((f = fopen(path, "re"))) {
			if (fgets(name, sizeof(name), f)) {
				char *nl = strchr(name, '\n');
				if (nl)
					*nl = '\0';
			}
			fclose(f);
		}
		if (!name[0])
			continue;

		/* insertion sort into a list this short beats sorting the ~500 entries
		 * of a real /proc */
		int32_t pos = n < max ? n : max - 1;
		while (pos > 0 && out[pos - 1].rss_kb < rss_kb) {
			out[pos] = out[pos - 1];
			pos--;
		}
		snprintf(out[pos].name, sizeof(out[pos].name), "%s", name);
		out[pos].rss_kb = rss_kb;
		if (n < max)
			n++;
	}
	closedir(d);
	return n;
}

static void bar_popover_open_sysinfo(Monitor *m, int32_t anchor_x) {
	if (bar_popover_is_open(BAR_POPOVER_SYSINFO)) {
		bar_popover_close();
		return;
	}
	if (!bar_popover_open(m, BAR_POPOVER_SYSINFO, anchor_x))
		return;

	int32_t n = 0;
	BarPopoverRow *r;

	/* ── cpu ── */
	if ((r = bar_popover_row_get(n))) {
		double l1 = 0, l5 = 0, l15 = 0;
		FILE *f = fopen("/proc/loadavg", "re");
		if (f) {
			if (fscanf(f, "%lf %lf %lf", &l1, &l5, &l15) != 3)
				l1 = l5 = l15 = 0;
			fclose(f);
		}
		snprintf(r->text, sizeof(r->text), "CPU  %d%%    load %.2f %.2f %.2f",
				 bar_metrics.cpu_pct, l1, l5, l15);
		r->enabled = false;
		n++;
	}

	/* ── memory and swap ── */
	uint64_t mem_total = 0, mem_avail = 0, swap_total = 0, swap_free = 0;
	FILE *mi = fopen("/proc/meminfo", "re");
	if (mi) {
		char line[256];
		while (fgets(line, sizeof(line), mi)) {
			sscanf(line, "MemTotal: %lu kB", &mem_total);
			sscanf(line, "MemAvailable: %lu kB", &mem_avail);
			sscanf(line, "SwapTotal: %lu kB", &swap_total);
			sscanf(line, "SwapFree: %lu kB", &swap_free);
		}
		fclose(mi);
	}
	if (mem_total && (r = bar_popover_row_get(n))) {
		char used[24], total[24];
		bar_sysinfo_bytes(used, sizeof(used),
						  (double)(mem_total - mem_avail) * 1024.0);
		bar_sysinfo_bytes(total, sizeof(total), (double)mem_total * 1024.0);
		snprintf(r->text, sizeof(r->text), "Memory  %s / %s    %d%%", used,
				 total, bar_metrics.mem_pct);
		r->enabled = false;
		n++;
	}
	/* only when there IS swap: "Swap 0 / 0" is a row that answers nothing */
	if (swap_total && (r = bar_popover_row_get(n))) {
		char used[24], total[24];
		bar_sysinfo_bytes(used, sizeof(used),
						  (double)(swap_total - swap_free) * 1024.0);
		bar_sysinfo_bytes(total, sizeof(total), (double)swap_total * 1024.0);
		snprintf(r->text, sizeof(r->text), "Swap  %s / %s", used, total);
		r->enabled = false;
		n++;
	}

	/* ── what is holding the memory ── */
	struct bar_sysinfo_proc top[3];
	int32_t ntop = bar_sysinfo_top_rss(top, 3);
	for (int32_t i = 0; i < ntop && (r = bar_popover_row_get(n)); i++) {
		char rss[24];
		bar_sysinfo_bytes(rss, sizeof(rss), (double)top[i].rss_kb * 1024.0);
		snprintf(r->text, sizeof(r->text), "    %s  %s", rss, top[i].name);
		r->enabled = false;
		n++;
	}

	/* ── network ── */
	if ((r = bar_popover_row_get(n))) {
		char down[24], up[24];
		bar_sysinfo_bytes(down, sizeof(down), bar_metrics.rx_rate);
		bar_sysinfo_bytes(up, sizeof(up), bar_metrics.tx_rate);
		if (bar_metrics.link_up)
			snprintf(r->text, sizeof(r->text), "%s  ↓%s/s  ↑%s/s",
					 bar_metrics.iface[0] ? bar_metrics.iface : "network",
					 down, up);
		else
			snprintf(r->text, sizeof(r->text), "%s", "network  down");
		r->enabled = false;
		n++;
	}
	if (bar_metrics.link_up && (r = bar_popover_row_get(n))) {
		char down[24], up[24];
		bar_sysinfo_bytes(down, sizeof(down), (double)bar_metrics.rx_bytes);
		bar_sysinfo_bytes(up, sizeof(up), (double)bar_metrics.tx_bytes);
		snprintf(r->text, sizeof(r->text), "    total  ↓%s  ↑%s", down, up);
		r->enabled = false;
		n++;
	}

	/* ── uptime ── */
	double up_s = 0;
	FILE *ut = fopen("/proc/uptime", "re");
	if (ut) {
		if (fscanf(ut, "%lf", &up_s) != 1)
			up_s = 0;
		fclose(ut);
	}
	if (up_s > 0 && (r = bar_popover_row_get(n))) {
		int32_t days = (int32_t)(up_s / 86400);
		int32_t hours = (int32_t)(up_s / 3600) % 24;
		int32_t mins = (int32_t)(up_s / 60) % 60;
		if (days)
			snprintf(r->text, sizeof(r->text), "Uptime  %dd %dh %dm", days,
					 hours, mins);
		else
			snprintf(r->text, sizeof(r->text), "Uptime  %dh %dm", hours, mins);
		r->enabled = false;
		n++;
	}

	bar_popover.nrows = n;
	if (n == 0) {
		bar_popover_close();
		return;
	}
	bar_popover_layout();
}

/* ─── input ───────────────────────────────────────────────────────────────── */

static bool bar_popover_handle_node_click(AsteroidzNodeData *hit,
										  uint32_t button) {
	BarPopoverRow *r = hit ? (BarPopoverRow *)hit->node_data : NULL;
	if (!r || !r->used || button != BTN_LEFT)
		return false;
	/* a separator is scenery, not a target: clicking one must neither act nor
	 * dismiss, the way it behaves in every other menu */
	if (bar_popover.kind == BAR_POPOVER_MENU && r->separator)
		return true;
	/* a stepper is adjusted by scrolling, so a click on it is a miss, not a
	 * selection -- dismissing would punish reaching for it */
	if (r->stepper)
		return true;
	/* An inert row is a reading, not a target: the click is consumed but the
	 * panel stays up, the way a greyed entry behaves in every other menu.
	 * Enforced here rather than per kind so a panel made ENTIRELY of readings
	 * (sysinfo) does not close on any click that lands inside it -- the
	 * individual cases below still carry their own checks, which this makes
	 * redundant rather than wrong. */
	if (!r->enabled)
		return true;
	switch (bar_popover.kind) {
	case BAR_POPOVER_SINKS: {
		char *const argv[] = {"pactl", "set-default-sink", r->value, NULL};
		async_spawn(event_loop, argv, NULL, NULL);
		break;
	}
	case BAR_POPOVER_OUTPUTS: {
		Monitor *anchor_mon = bar_popover.mon;
		int32_t ax = bar_popover.anchor_x;
		char name[64];
		snprintf(name, sizeof(name), "%s", r->value);
		bar_popover_open_output(anchor_mon, ax, name);
		return true; /* already reopened against the output */
	}
	case BAR_POPOVER_OUTPUT: {
		if (!r->enabled)
			return true; /* a reading, not a control */
		Monitor *anchor_mon = bar_popover.mon;
		int32_t ax = bar_popover.anchor_x;
		char name[64];
		snprintf(name, sizeof(name), "%s", bar_popover.output_name);
		/* Match on the verb explicitly and do nothing for anything else. The
		 * previous "not back, so it must be HDR" reading is what let a payload
		 * that failed to compare equal silently flip the output. */
		if (!BAR_POPOVER_IS_VERB(r->value))
			return true;
		if (!strcmp(r->value + 1, "back")) {
			bar_popover_open_outputs(anchor_mon, ax);
			return true;
		}
		if (!strcmp(r->value + 1, "modes")) {
			bar_popover_open_modes(anchor_mon, ax, name);
			return true;
		}
		if (!strcmp(r->value + 1, "scales")) {
			bar_popover_open_scales(anchor_mon, ax, name);
			return true;
		}
		if (strcmp(r->value + 1, "hdr") != 0)
			return true;
		bar_display_toggle_hdr(bar_display_by_name(name));
		/* stay open and re-render, so the toggle's new state is visible
		 * without having to reopen the menu to check it took */
		bar_popover_open_output(anchor_mon, ax, name);
		return true;
	}
	case BAR_POPOVER_MODES: {
		Monitor *anchor_mon = bar_popover.mon;
		int32_t ax = bar_popover.anchor_x;
		char name[64];
		snprintf(name, sizeof(name), "%s", bar_popover.output_name);
		if (BAR_POPOVER_IS_VERB(r->value)) {
			bar_popover_open_output(anchor_mon, ax, name);
			return true;
		}
		int32_t w = 0, h = 0, refresh = 0;
		if (sscanf(r->value, "%d,%d,%d", &w, &h, &refresh) != 3)
			return true;
		Monitor *target = bar_display_by_name(name);
		struct wlr_output_mode *mode = NULL, *it = NULL;
		if (target && target->wlr_output) {
			wl_list_for_each(it, &target->wlr_output->modes, link) {
				if (it->width == w && it->height == h &&
					it->refresh == refresh) {
					mode = it;
					break;
				}
			}
		}
		/* the mode may be gone -- the output can be reconfigured while its
		 * menu is open -- in which case do nothing rather than guess a
		 * neighbour */
		if (mode)
			bar_display_apply(target, mode, 0.0f);
		bar_popover_open_output(anchor_mon, ax, name);
		return true;
	}
	case BAR_POPOVER_SCALES: {
		Monitor *anchor_mon = bar_popover.mon;
		int32_t ax = bar_popover.anchor_x;
		char name[64];
		snprintf(name, sizeof(name), "%s", bar_popover.output_name);
		if (BAR_POPOVER_IS_VERB(r->value)) {
			bar_popover_open_output(anchor_mon, ax, name);
			return true;
		}
		float scale = strtof(r->value, NULL);
		if (scale > 0.0f)
			bar_display_apply(bar_display_by_name(name), NULL, scale);
		bar_popover_open_output(anchor_mon, ax, name);
		return true;
	}
	case BAR_POPOVER_MEDS: {
		if (!r->enabled)
			return true;
		Monitor *anchor_mon = bar_popover.mon;
		int32_t ax = bar_popover.anchor_x;
		char key[176];
		snprintf(key, sizeof(key), "%s", r->value);
		bar_popover_open_med_dose(anchor_mon, ax, key);
		return true;
	}
	case BAR_POPOVER_MED_DOSE: {
		if (!r->enabled || !BAR_POPOVER_IS_VERB(r->value))
			return true;
		Monitor *anchor_mon = bar_popover.mon;
		int32_t ax = bar_popover.anchor_x;
		char key[176];
		snprintf(key, sizeof(key), "%s", bar_popover.med_key);
		const char *verb = r->value + 1;
		if (!strcmp(verb, "back")) {
			bar_popover_open_meds(anchor_mon, ax);
			return true;
		}
		BarMedDose *d = bar_med_by_key(key);
		if (d) {
			if (!strcmp(verb, "take"))
				bar_med_mark(d, "taken");
			else if (!strcmp(verb, "skip"))
				bar_med_mark(d, "skipped");
			else if (!strcmp(verb, "snooze"))
				bar_med_postpone(d, bar_med_snooze_minutes());
			bar_med_reload();
			bar_update_all();
		}
		/* back to the list, which now shows the new state -- the point of
		 * recording a dose is seeing that it was recorded */
		bar_popover_open_meds(anchor_mon, ax);
		return true;
	}
	case BAR_POPOVER_VPN: {
		if (!r->enabled)
			return true; /* a status line: consumed, but keeps the menu up */
		if (BAR_POPOVER_IS_VERB(r->value))
			!strcmp(r->value + 1, "off") ? bar_vpn_disconnect()
										 : bar_vpn_connect(NULL);
		else
			bar_vpn_connect(r->value);
		break;
	}
	case BAR_POPOVER_VOICE: {
		if (BAR_POPOVER_IS_VERB(r->value)) {
			/* the two verbs, distinguished from a channel id by a marker byte
			 * no Discord snowflake can contain */
			if (!strcmp(r->value + 1, "mute"))
				bar_dv_send("{\"cmd\":\"mute\"}");
			else
				bar_dv_send("{\"cmd\":\"leave\"}");
			break;
		}
		char guild[48] = "", chan[48] = "";
		const char *sep = strchr(r->value, '\x1f');
		if (sep) {
			size_t gl = (size_t)(sep - r->value);
			if (gl >= sizeof(guild))
				gl = sizeof(guild) - 1;
			memcpy(guild, r->value, gl);
			guild[gl] = '\0';
			snprintf(chan, sizeof(chan), "%s", sep + 1);
		}
		if (guild[0] && chan[0]) {
			char cmd[192];
			snprintf(cmd, sizeof(cmd),
					 "{\"cmd\":\"join\",\"guild\":\"%s\",\"channel\":\"%s\"}",
					 guild, chan);
			bar_dv_send(cmd);
		}
		break;
	}
	case BAR_POPOVER_MENU: {
		if (r->separator || !r->enabled)
			return true; /* consumed, but nothing to do -- and stays open */
		if (r->submenu) {
			/* descend in place: same anchor, this row's id as the parent */
			char svc[128], path[128];
			snprintf(svc, sizeof(svc), "%s", bar_popover.menu_service);
			snprintf(path, sizeof(path), "%s", bar_popover.menu_path);
			Monitor *m = bar_popover.mon;
			int32_t ax = bar_popover.anchor_x;
			bar_popover_open_menu(m, ax, svc, path, r->id);
			return true; /* bar_popover_open_menu already reset the popover */
		}
		if (session_bus)
			sd_bus_call_method_async(session_bus, NULL,
									 bar_popover.menu_service,
									 bar_popover.menu_path, BAR_MENU_IFACE,
									 "Event", NULL, NULL, "isvu", r->id,
									 "clicked", "s", "",
									 (uint32_t)time(NULL));
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
