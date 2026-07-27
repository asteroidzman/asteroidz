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

/* Generous, because a real menu is long: Steam's lists every installed game
 * before Store/Library/Community and only then Quit, and a Discord account
 * with a dozen servers has over a hundred voice channels. Truncating silently
 * loses the entry people actually reach for. Raised from 32 once the panel
 * gained a scrolling viewport -- before that, rows past the screen were drawn
 * nowhere, so a bigger cap bought nothing.
 *
 * 64 was still too small for the case the paragraph above describes: this
 * account's voice menu wants 119 channels plus a header per server plus the
 * controls, and the cut landed mid-list, so three whole servers had no header
 * at all and looked as though the account were not in them. A row is ~0.5K and
 * they are only allocated as the menu uses them, so the ceiling costs nothing
 * to raise and everything to hit. */
#define BAR_POPOVER_MAX_ROWS 256

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
	BAR_POPOVER_WEATHER,  /* conditions, metrics and the week; every row inert */
	BAR_POPOVER_ARRANGE,  /* the monitor layout canvas */
};

/* One draggable monitor tile on the arrange canvas.
 *
 * The canvas is the one thing here that is not a list. Everything else the bar
 * shows is a stack of rows because a row is honest about what it can do; a
 * monitor arrangement is a 2D relationship between rectangles, and a list of
 * "DP-1 is left of HDMI-A-1" rows would be a worse description of it than a
 * picture that is wrong until you drag it right.
 *
 * Each tile is a tab-bar node -- the same widget as a pill or a row -- so it
 * gets the theme, the corner radius, the label and the hit-test tag for free.
 * What it adds is that dragging one means something. */
#define BAR_CANVAS_MAX_TILES 8

/* Height the canvas block takes inside the panel. Four rows' worth: enough
 * that a 4K beside a 1080p is legible at a glance, without turning a menu into
 * a window. */
#define BAR_CANVAS_HEIGHT (config.bar_popover_row_height * 4)

typedef struct {
	struct asteroidz_tab_bar_node *node;
	AsteroidzNodeData *hit;
	char output[64]; /* held by NAME: a Monitor can be unplugged mid-drag */
	struct wlr_box layout_box; /* where this output is, in layout coords */
	bool used;
} BarCanvasTile;

typedef struct {
	struct asteroidz_tab_bar_node *node;
	AsteroidzNodeData *hit;
	char text[BAR_TEXT_MAX];
	/* Opaque payload the popover's own click handler interprets. For the sink
	 * picker this is the pactl sink NAME, which is what set-default-sink
	 * takes; kept as a string rather than the numeric index because indices
	 * are recycled when a device is unplugged and replugged. */
	char value[192];
	/* Artwork for this row, as _set_icon takes it: an icon name, an absolute
	 * path, or a cache key. Empty for the rows that are pure text, which is
	 * most of them -- a menu of identical glyphs is noise. The weather
	 * forecast is the case that needs it: a day without its condition is a
	 * pair of numbers. */
	char icon[192];
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
	/* Viewport: index of the first row DRAWN, and how many of the rows there
	 * are ended up on screen. A menu taller than the output shows a window of
	 * itself and scrolls; one that fits keeps scroll pinned at 0. */
	int32_t scroll;
	int32_t visible_rows, total_rows;
	/* The arrange canvas: tiles, the area they are drawn in, and the scale
	 * from layout pixels to that area. Empty for every other kind. */
	BarCanvasTile tiles[BAR_CANVAS_MAX_TILES];
	int32_t ntiles;
	struct wlr_box canvas;   /* on-screen, inside the panel */
	/* The transform, as a pair of origins and a scale: layout point P draws at
	 * (P - canvas_o) * scale + canvas_s. Both origins are needed -- the
	 * arrangement is CENTRED in the canvas area, so the screen origin is not
	 * the area's corner, and inverting the mapping without it puts a dragged
	 * monitor hundreds of layout pixels from the pointer. */
	double canvas_scale;          /* layout px -> canvas px */
	int32_t canvas_ox, canvas_oy; /* layout-space origin */
	int32_t canvas_sx, canvas_sy; /* screen-space origin of the drawn box */
	/* which tile is being dragged (index, or -1), and where in it the press
	 * landed, so the tile does not jump to centre on the pointer */
	int32_t drag_tile;
	double drag_dx, drag_dy;
	bool dragged; /* did it actually move? a click that did not is not a drag */
	/* Where the keyboard is, as an index into `rows`, or -1 for "nowhere yet".
	 * Starts nowhere on purpose: a popover opened by the pointer should not
	 * pre-arm Enter on some row the user never looked at. The first arrow key
	 * places it. */
	int32_t cursor;
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
	/* Centre of the pill this hangs from, RELATIVE to its monitor.
	 *
	 * Absolute would be simpler and is wrong: the arrange canvas can move the
	 * very output the popover is drawn on, and an absolute anchor then points
	 * at where that monitor used to be -- the panel jumps to the far edge of
	 * the screen mid-drag. Relative, it stays under its pill because the pill
	 * moved with the monitor too. */
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
static void bar_popover_open_arrange(Monitor *m, int32_t anchor_x);
static void bar_canvas_release_tiles(void);
static void bar_canvas_layout(struct wlr_box area);

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
	/* Cleared for reused rows as well as fresh ones: a row is pooled, and
	 * every menu but the forecast leaves this field alone, so a stale icon
	 * would follow the node from one menu into the next. */
	r->icon[0] = '\0';
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

	/* `popover.width` is a FLOOR, not the width: the panel grows to whatever
	 * its longest row needs, up to half the output.
	 *
	 * Fixed at 340 it ellipsised our own readouts -- a throughput line reading
	 * "enp11s0 ↓160.2 K/s ↑20.4 …" is a bug, not a long device name, and no
	 * amount of shortening the text fixes it for the next interface with a
	 * longer name. The cap is what the fixed width was really protecting
	 * against: a tray menu entry can carry an eighty-character title, and a
	 * panel wider than the screen is worse than a truncated row.
	 *
	 * Measured, not guessed, using each row's own node -- the same call the
	 * bar sizes its pills with, so it accounts for the font, the padding and
	 * the per-output scale. */
	int32_t widest = 0;
	for (int32_t i = 0; i < bar_popover.nrows; i++) {
		BarPopoverRow *r = &bar_popover.rows[i];
		if (!r->used || !r->node)
			continue;
		int32_t w = asteroidz_tab_bar_node_measure_width(r->node, r->text,
														 row_h);
		if (w > widest)
			widest = w;
	}
	int32_t max_w = m->m.width / 2;
	if (widest + 2 * pad > width)
		width = widest + 2 * pad;
	if (width > max_w)
		width = max_w;
	if (width < config.bar_popover_width)
		width = config.bar_popover_width; /* the floor wins on a tiny output */

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

	/* The arrange canvas sits above the rows and is not one of them: it is a
	 * 2D picture, and the row stack has no way to express that. It takes a
	 * fixed block at the top of the panel and the rows carry on beneath it. */
	int32_t canvas_h = bar_popover.kind == BAR_POPOVER_ARRANGE
						   ? BAR_CANVAS_HEIGHT + config.bar_popover_spacing
						   : 0;

	int32_t height = nrows * row_h + (nrows - 1) * config.bar_popover_spacing +
					 2 * pad + canvas_h;

	/* Clamp to the output, and SCROLL what does not fit.
	 *
	 * A long menu on a short screen would otherwise run off the bottom, taking
	 * its last entries -- which is where Quit lives -- with it. This used to
	 * drop those rows outright, which is worse than it sounds: Steam's tray
	 * menu lists every installed game before Store/Library/Community and only
	 * then Quit, so the entry people actually reach for was the one reliably
	 * missing.
	 *
	 * The rows are all kept; only a window of them is placed and shown. That
	 * is the whole viewport: there is no clipping to do, because a row outside
	 * the window is simply never enabled. */
	int32_t avail = m->m.height - config.bar_margin_y * 2 - config.bar_height -
					config.bar_popover_gap;
	int32_t total_rows = nrows;
	int32_t first = 0;
	if (height > avail && row_h + config.bar_popover_spacing > 0) {
		int32_t fits = (avail - 2 * pad - canvas_h +
						config.bar_popover_spacing) /
					   (row_h + config.bar_popover_spacing);
		if (fits < 1)
			fits = 1;
		if (fits < nrows) {
			first = bar_popover.scroll;
			if (first > total_rows - fits)
				first = total_rows - fits;
			if (first < 0)
				first = 0;
			bar_popover.scroll = first;
			nrows = fits;
			height = nrows * row_h +
					 (nrows - 1) * config.bar_popover_spacing + 2 * pad +
					 canvas_h;
		}
	}
	if (nrows == total_rows)
		bar_popover.scroll = 0;
	bar_popover.visible_rows = nrows;
	bar_popover.total_rows = total_rows;

	/* Hangs off the bar's outer edge: below a top bar, above a bottom one. */
	int32_t bar_bottom = m->m.y + config.bar_margin_y + config.bar_height;
	int32_t y = config.bar_position_bottom
					? m->m.y + m->m.height - config.bar_margin_y -
						  config.bar_height - config.bar_popover_gap - height
					: bar_bottom + config.bar_popover_gap;

	int32_t x = m->m.x + bar_popover.anchor_x - width / 2;
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
		/* Grown on every side and offset, exactly as the bar's own panels are
		 * -- a shadow drawn at the panel's own box has no room outside it to
		 * fall off into, so an opaque panel hides the whole thing. See
		 * bar_panel_apply. */
		int32_t delta = config.bar_panel_shadow_size;
		int32_t sx = x - delta;
		int32_t sy = y - delta + delta / 3;
		int32_t sw = width + 2 * delta, sh = height + 2 * delta;
		int32_t sradius = radius + delta;
		if (!panel->shadow)
			panel->shadow = wlr_scene_shadow_create(
				bar_popover.panel_tree, sw, sh, sradius, config.shadows_blur,
				config.shadowscolor);
		if (panel->shadow) {
			wlr_scene_shadow_set_size(panel->shadow, sw, sh);
			wlr_scene_shadow_set_corner_radius(panel->shadow, sradius);
			wlr_scene_shadow_set_blur_sigma(panel->shadow,
											config.bar_panel_shadow_blur);
			wlr_scene_shadow_set_color(panel->shadow,
									   config.bar_panel_shadow_color);
			wlr_scene_node_set_position(&panel->shadow->node, sx, sy);
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

	if (canvas_h > 0)
		bar_canvas_layout((struct wlr_box){x + pad, y + pad, width - 2 * pad,
										   BAR_CANVAS_HEIGHT});

	int32_t ry = y + pad + canvas_h;
	int32_t seen = 0;
	for (int32_t i = 0; i < bar_popover.nrows; i++) {
		BarPopoverRow *r = &bar_popover.rows[i];
		if (!r->used)
			continue;
		/* outside the scrolled window: kept, but not drawn */
		int32_t slot = seen++;
		if (slot < first || slot >= first + nrows) {
			asteroidz_tab_bar_node_set_enabled(r->node, false);
			continue;
		}
		asteroidz_tab_bar_node_set_size(r->node, inner_w, row_h);
		asteroidz_tab_bar_node_apply_config(r->node, &config.theme);
		asteroidz_tab_bar_node_set_padding(r->node, config.bar_popover_padding,
										   config.theme.padding_y);
		asteroidz_tab_bar_node_set_text_align_left(r->node, true);
		/* the active entry is the one filled, like the selected tag chip */
		asteroidz_tab_bar_node_set_focus(r->node, r->selected);
		if (!r->selected && bar_popover.cursor == i) {
			/* Where the keyboard is. Distinct from `selected`, which means
			 * "this is the current value" -- the two are different questions
			 * and can point at different rows, so the cursor is the accent in
			 * the TEXT rather than a second filled tile. A row that is both
			 * stays filled: accent on accent would be invisible. */
			asteroidz_tab_bar_node_set_colors(r->node,
											  config.theme.focus_bg_color,
											  config.theme.bg_color);
		} else if (!r->selected && (!r->enabled || r->separator)) {
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
		/* Set every frame, not only when it changes: rows are pooled, so the
		 * node drawing a forecast day now may have drawn a plain text row a
		 * moment ago and would otherwise keep that row's artwork. */
		asteroidz_tab_bar_node_set_icon(r->node, r->icon[0] ? r->icon : NULL);
		if (r->icon[0])
			asteroidz_tab_bar_node_set_icon_scale(r->node, 0.8);
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
	/* Both of these destroy scene nodes that are CHILDREN of the tree below,
	 * so they have to run while it is still standing: destroying the tree
	 * takes its children with it, and freeing them again afterwards is a
	 * double free through a dangling pointer -- which is exactly what a
	 * canvas tile released after the tree did, on the first dismiss of the
	 * first arrange popover ever opened. */
	for (int32_t i = 0; i < BAR_POPOVER_MAX_ROWS; i++)
		bar_popover_row_release(&bar_popover.rows[i]);
	bar_canvas_release_tiles();
	if (bar_popover.tree) {
		/* the panel nodes are children, so this takes them too */
		wlr_scene_node_destroy(&bar_popover.tree->node);
		bar_popover.tree = NULL;
	}
	bar_popover.panel_tree = NULL;
	bar_popover.row_tree = NULL;
	bar_popover.panel = (BarPanel){0};
	bar_popover.nrows = 0;
	bar_popover.scroll = 0;
	bar_popover.visible_rows = bar_popover.total_rows = 0;
	bar_popover.cursor = -1;
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
	bar_popover.anchor_x = anchor_x - m->m.x;
	bar_popover.open = true;
	bar_popover.nrows = 0;
	/* Each level of a drill-down is its own list: keeping the previous one's
	 * scroll offset or cursor would land you halfway down a menu you have not
	 * seen. */
	bar_popover.scroll = 0;
	bar_popover.cursor = -1;
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
/* Push-to-talk is a GlobalShortcuts binding the DAEMON registers with us --
 * we are the portal backend, so the current key is ours to read, no file
 * parsing and no guessing. The waybar plugin had to read
 * ~/.config/asteroidz/global-shortcuts by hand precisely because it was not
 * the compositor. */
#define BAR_DV_PTT_APPID "org.matrixtui.MatrixTui"
#define BAR_DV_PTT_ID "push-to-talk"

static void bar_popover_open_voice(Monitor *m, int32_t anchor_x) {
	if (bar_popover_is_open(BAR_POPOVER_VOICE)) {
		bar_popover_close();
		return;
	}
	if (!bar_popover_open(m, BAR_POPOVER_VOICE, anchor_x))
		return;

	/* No request is sent. The daemon's command set is join/leave/mute/
	 * rebind_ptt and nothing else -- it pushes `channels` itself, as part of
	 * the snapshot it sends on connect and again whenever the list changes.
	 * Asking for one got back "unknown command: channels", which the daemon
	 * reports as an error, which latched the pill into its urgent state: an
	 * invented command is worse than a missing feature, because it breaks the
	 * thing it was meant to help. bar_popover_voice_channels_arrived redraws
	 * this menu if a push lands while it is open.
	 */

	int32_t n = 0;
	BarPopoverRow *r = NULL;

	/* What is going on, always. Without this the menu was EMPTY whenever the
	 * daemon was connected but idle and no channel snapshot had arrived, and
	 * an empty popover closes itself -- so clicking the module did nothing at
	 * all, which reads as a broken button rather than as "nothing to show". */
	if ((r = bar_popover_row_get(n))) {
		if (bar_dv.error[0])
			snprintf(r->text, sizeof(r->text), "%s", bar_dv.error);
		else if (bar_dv.username[0] && bar_dv.state != BAR_DV_CONNECTED)
			snprintf(r->text, sizeof(r->text), "%s  ·  %s", bar_dv.username,
					 bar_dv_label());
		else
			snprintf(r->text, sizeof(r->text), "%s", bar_dv_label());
		r->enabled = false;
		n++;
	}

	/* ── controls, ABOVE the channels ──
	 *
	 * They used to sit under the list, which put Mute and Disconnect behind a
	 * hundred and thirty channels on a real account: the two things you reach
	 * for while you are IN a call were the two furthest away, and any push
	 * from the daemon rebuilds the menu, so a scroll that got you there could
	 * be undone before you clicked. Both are fixed here -- these rows are
	 * first, and the viewport survives a rebuild. */
	if (bar_dv.state == BAR_DV_CONNECTED &&
		n < BAR_POPOVER_MAX_ROWS && (r = bar_popover_row_get(n))) {
		snprintf(r->text, sizeof(r->text), "%s",
				 bar_dv.muted ? "Unmute" : "Mute");
		snprintf(r->value, sizeof(r->value), "%s", BAR_POPOVER_VERB "mute");
		r->selected = bar_dv.muted;
		n++;
	}
	if ((bar_dv.state == BAR_DV_CONNECTED ||
		 bar_dv.state == BAR_DV_CONNECTING) &&
		n < BAR_POPOVER_MAX_ROWS && (r = bar_popover_row_get(n))) {
		snprintf(r->text, sizeof(r->text), "%s", "Disconnect");
		snprintf(r->value, sizeof(r->value), "%s", BAR_POPOVER_VERB "leave");
		n++;
	}

	/* Push-to-talk. Shows what is bound now, straight from our own portal
	 * store, and a click hands the daemon a rebind request -- which comes back
	 * to us as a GlobalShortcuts BindShortcuts and opens the on-screen picker.
	 * Pointless with no daemon to ask, so it is offered only when there is
	 * one. */
	if (bar_dv.state != BAR_DV_OFFLINE && n < BAR_POPOVER_MAX_ROWS &&
		(r = bar_popover_row_get(n))) {
		char *key = gs_load_saved(BAR_DV_PTT_APPID, BAR_DV_PTT_ID);
		if (key && *key)
			snprintf(r->text, sizeof(r->text), "PTT key: %s  —  change…", key);
		else
			snprintf(r->text, sizeof(r->text), "%s", "Set PTT key…");
		free(key);
		snprintf(r->value, sizeof(r->value), "%s", BAR_POPOVER_VERB "ptt");
		n++;
	}

	/* Discord session. The daemon starts logged OUT -- a session coming up
	 * must not put an account online -- so being connected is a thing you ask
	 * for, and the row that asks is the first one that matters when the menu
	 * is otherwise empty. Only offered while a daemon is actually there to
	 * ask; with no daemon the row below offers to start one instead. */
	if (bar_dv.linked && n < BAR_POPOVER_MAX_ROWS &&
		(r = bar_popover_row_get(n))) {
		if (bar_dv.state == BAR_DV_OFFLINE) {
			snprintf(r->text, sizeof(r->text), "%s", "Connect to Discord");
			snprintf(r->value, sizeof(r->value), "%s",
					 BAR_POPOVER_VERB "connect");
		} else {
			snprintf(r->text, sizeof(r->text), "%s", "Disconnect from Discord");
			snprintf(r->value, sizeof(r->value), "%s",
					 BAR_POPOVER_VERB "disconnect");
		}
		n++;
	}

	/* Daemon lifecycle. The module used to hide itself outright while the
	 * daemon was down, which made stopping it a one-way door: no pill, no
	 * popover, nothing to start it from again. */
	if (n < BAR_POPOVER_MAX_ROWS && (r = bar_popover_row_get(n))) {
		if (!bar_dv.linked) {
			snprintf(r->text, sizeof(r->text), "%s", "Start daemon");
			snprintf(r->value, sizeof(r->value), "%s",
					 BAR_POPOVER_VERB "dstart");
		} else {
			snprintf(r->text, sizeof(r->text), "%s", "Shut down daemon");
			snprintf(r->value, sizeof(r->value), "%s",
					 BAR_POPOVER_VERB "dstop");
		}
		n++;
	}

	/* ── channels, grouped by guild ──
	 *
	 * The list arrives flat with a guild id on each entry; the plugin draws a
	 * header per server and the channels under it, which is the only way a
	 * dozen channels called "General" tell you anything. */
	char shown[BAR_DV_MAX_CHANNELS][48];
	int32_t nshown = 0;
	for (int32_t i = 0; i < bar_dv.nchannels && n < BAR_POPOVER_MAX_ROWS - 4;
		 i++) {
		BarDvChannel *c = &bar_dv.channels[i];
		bool seen = false;
		for (int32_t g = 0; g < nshown; g++)
			if (!strcmp(shown[g], c->guild))
				seen = true;
		if (!seen && nshown < BAR_DV_MAX_CHANNELS) {
			snprintf(shown[nshown], sizeof(shown[0]), "%s", c->guild);
			nshown++;
			if ((r = bar_popover_row_get(n))) {
				snprintf(r->text, sizeof(r->text), "%s",
						 c->guild_name[0] ? c->guild_name : "server");
				r->enabled = false;
				n++;
			}
		}
		/* every channel of this guild, in order, under that header */
		for (int32_t j = i; j < bar_dv.nchannels && n < BAR_POPOVER_MAX_ROWS - 4;
			 j++) {
			BarDvChannel *cc = &bar_dv.channels[j];
			if (strcmp(cc->guild, c->guild))
				continue;
			if (!(r = bar_popover_row_get(n)))
				break;
			if (cc->people > 0)
				snprintf(r->text, sizeof(r->text), "    %s  (%d)", cc->name,
						 cc->people);
			else
				snprintf(r->text, sizeof(r->text), "    %s", cc->name);
			/* the join command needs both ids, so carry them together */
			snprintf(r->value, sizeof(r->value), "%s\x1f%s", cc->guild,
					 cc->id);
			r->selected = strcmp(cc->id, bar_dv.channel_id) == 0;
			n++;
		}
	}
	if (bar_dv.nchannels == 0 && n < BAR_POPOVER_MAX_ROWS - 4 &&
		(r = bar_popover_row_get(n))) {
		snprintf(r->text, sizeof(r->text), "%s",
				 !bar_dv.linked			? "daemon not running"
				 : bar_dv.state == BAR_DV_OFFLINE ? "not connected to Discord"
												  : "no voice channels");
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

	int32_t noutputs = 0;
	Monitor *it = NULL;
	wl_list_for_each(it, &mons, link) {
		if (n >= BAR_POPOVER_MAX_ROWS || !it->wlr_output)
			continue;
		noutputs++;
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

	/* Only worth offering with something to arrange: with one output there is
	 * no relationship to change, and a canvas showing a single rectangle you
	 * can drag nowhere meaningful is a control that lies. */
	if (noutputs >= 2 && n < BAR_POPOVER_MAX_ROWS) {
		BarPopoverRow *r = bar_popover_row_get(n);
		if (r) {
			snprintf(r->text, sizeof(r->text), "%s", "Arrange displays  ›");
			snprintf(r->value, sizeof(r->value), "%s",
					 BAR_POPOVER_VERB "arrange");
			r->submenu = true;
			n++;
		}
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

/* Move the viewport by `notches` rows. Returns true if it moved. */
static bool bar_popover_scroll_by(int32_t notches) {
	if (bar_popover.total_rows <= bar_popover.visible_rows)
		return false;
	int32_t max = bar_popover.total_rows - bar_popover.visible_rows;
	int32_t want = bar_popover.scroll + notches;
	if (want < 0)
		want = 0;
	if (want > max)
		want = max;
	if (want == bar_popover.scroll)
		return false;
	bar_popover.scroll = want;
	bar_popover_layout();
	return true;
}

/* Scroll over a popover row. Returns true when it was consumed. */
static bool bar_popover_handle_node_scroll(AsteroidzNodeData *hit,
										   int32_t notches) {
	BarPopoverRow *r = hit ? (BarPopoverRow *)hit->node_data : NULL;
	if (!r || !r->used || notches == 0)
		return false;
	if (!r->stepper) {
		/* Not a value to adjust, so the wheel means what it means everywhere
		 * else: move the list. Consumed either way -- a scroll that lands on
		 * an open menu must never fall through to the window underneath, and
		 * a menu short enough to fit simply has nowhere to go. */
		bar_popover_scroll_by(notches);
		return true;
	}
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

/* ── weather ──
 *
 * The same three blocks the waybar plugin's popover has, in the shape a list
 * of rows can carry them: current conditions, the metrics grid, and the week.
 *
 * A row per day rather than the plugin's row OF days: this popover is a
 * vertical list, and seven columns squeezed into a menu width would put three
 * characters under each icon. Down the page each day gets its own line, which
 * is also the direction the panel already scrolls.
 *
 * Every row is inert. Nothing here is a command -- there is no "set the
 * weather" -- so a row that highlighted and did nothing on click would be a
 * lie about what it is. */
static void bar_popover_open_weather(Monitor *m, int32_t anchor_x) {
	if (bar_popover_is_open(BAR_POPOVER_WEATHER)) {
		bar_popover_close();
		return;
	}
	if (!bar_popover_open(m, BAR_POPOVER_WEATHER, anchor_x))
		return;

	int32_t n = 0;
	BarPopoverRow *r;
	char icon[512], rel[64];

	if (!bar_weather.valid) {
		if ((r = bar_popover_row_get(n))) {
			snprintf(r->text, sizeof(r->text), "%s",
					 bar_weather.located ? "Weather unavailable"
										 : "Locating…");
			r->enabled = false;
			n++;
		}
		bar_popover.nrows = n;
		bar_popover_layout();
		return;
	}

	/* ── now ── */
	if ((r = bar_popover_row_get(n))) {
		snprintf(rel, sizeof(rel), "waybar-weather/%s",
				 bar_wmo_icon(bar_weather.code, bar_weather.is_day));
		bar_icon_path(icon, sizeof(icon), rel);
		snprintf(r->icon, sizeof(r->icon), "%s", icon);
		snprintf(r->text, sizeof(r->text), "%d°  ·  %s", bar_weather.temp_c,
				 bar_wmo_text(bar_weather.code));
		r->enabled = false;
		n++;
	}
	if ((r = bar_popover_row_get(n))) {
		if (bar_weather.city[0])
			snprintf(r->text, sizeof(r->text), "Feels like %d°  ·  %s",
					 bar_weather.feels_c, bar_weather.city);
		else
			snprintf(r->text, sizeof(r->text), "Feels like %d°",
					 bar_weather.feels_c);
		r->enabled = false;
		n++;
	}

	/* ── metrics ──
	 *
	 * Skipped individually when the model has no value: a row reading
	 * "Humidity -1%" is worse than no row. */
	struct {
		const char *label;
		int32_t value;
		const char *unit;
	} metrics[] = {
		{"Humidity", bar_weather.humidity_pct, "%"},
		{"Wind", bar_weather.wind_kmh, " km/h"},
		{"Pressure", bar_weather.pressure_hpa, " hPa"},
		{"Precipitation", bar_weather.precip_pct, "%"},
	};
	bool any_metric = false;
	for (size_t i = 0; i < LENGTH(metrics); i++) {
		if (metrics[i].value < 0)
			continue;
		if (!any_metric && (r = bar_popover_row_get(n))) {
			r->separator = true;
			r->enabled = false;
			r->text[0] = '\0';
			n++;
			any_metric = true;
		}
		if ((r = bar_popover_row_get(n))) {
			snprintf(r->text, sizeof(r->text), "%s  %d%s", metrics[i].label,
					 metrics[i].value, metrics[i].unit);
			r->enabled = false;
			n++;
		}
	}
	if (bar_weather.sunrise[0] && (r = bar_popover_row_get(n))) {
		snprintf(r->text, sizeof(r->text), "Sunrise  %s  ·  Sunset  %s",
				 bar_weather.sunrise, bar_weather.sunset);
		r->enabled = false;
		n++;
	}

	/* ── the week ── */
	if (bar_weather.ndays > 0 && (r = bar_popover_row_get(n))) {
		r->separator = true;
		r->enabled = false;
		r->text[0] = '\0';
		n++;
	}
	for (int32_t i = 0; i < bar_weather.ndays && n < BAR_POPOVER_MAX_ROWS; i++) {
		BarWeatherDay *d = &bar_weather.days[i];
		if (!(r = bar_popover_row_get(n)))
			break;
		/* Daytime artwork for every day: a forecast is about the day, and the
		 * night variant of a clear sky reads as "clear tonight". */
		snprintf(rel, sizeof(rel), "waybar-weather/%s",
				 bar_wmo_icon(d->code, true));
		bar_icon_path(icon, sizeof(icon), rel);
		snprintf(r->icon, sizeof(r->icon), "%s", icon);
		if (d->precip > 0)
			snprintf(r->text, sizeof(r->text), "%-6s %3d° / %3d°   %d%%",
					 d->day, d->tmax, d->tmin, d->precip);
		else
			snprintf(r->text, sizeof(r->text), "%-6s %3d° / %3d°", d->day,
					 d->tmax, d->tmin);
		r->enabled = false;
		n++;
	}

	bar_popover.nrows = n;
	bar_popover_layout();
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

/* ─── arrange canvas ──────────────────────────────────────────────────────── */

static void bar_canvas_release_tiles(void) {
	for (int32_t i = 0; i < BAR_CANVAS_MAX_TILES; i++) {
		BarCanvasTile *t = &bar_popover.tiles[i];
		if (!t->used)
			continue;
		if (t->node) {
			asteroidz_tab_bar_node_destroy(t->node); /* takes the hit tag */
			t->node = NULL;
		}
		t->hit = NULL;
		t->used = false;
		t->output[0] = '\0';
	}
	bar_popover.ntiles = 0;
	bar_popover.drag_tile = -1;
	bar_popover.dragged = false;
}

/* Read the current arrangement into tiles. Called on open and after every
 * applied move, so the canvas always shows what the compositor actually has
 * rather than a model of it that can drift. */
static void bar_canvas_sync_tiles(void) {
	int32_t n = 0;
	Monitor *m = NULL;
	wl_list_for_each(m, &mons, link) {
		if (n >= BAR_CANVAS_MAX_TILES || !m->wlr_output ||
			!m->wlr_output->enabled || !m->wlr_output->name)
			continue;
		BarCanvasTile *t = &bar_popover.tiles[n];
		if (!t->used) {
			AsteroidzNodeData *hit = ecalloc(1, sizeof(AsteroidzNodeData));
			hit->type = ASTEROIDZ_BAR_CANVAS_NODE;
			hit->node_data = (void *)(intptr_t)n; /* index, not a pointer:
												   * the tile array is stable
												   * but its Monitor is not */
			t->node = asteroidz_tab_bar_node_create(hit, bar_popover.row_tree,
												   config.theme, 0, 0);
			if (!t->node) {
				free(hit);
				break;
			}
			t->hit = hit;
			t->used = true;
		}
		snprintf(t->output, sizeof(t->output), "%s", m->wlr_output->name);
		t->layout_box = m->m;
		n++;
	}
	for (int32_t i = n; i < BAR_CANVAS_MAX_TILES; i++) {
		BarCanvasTile *t = &bar_popover.tiles[i];
		if (t->used && t->node) {
			asteroidz_tab_bar_node_destroy(t->node);
			t->node = NULL;
			t->hit = NULL;
			t->used = false;
		}
	}
	bar_popover.ntiles = n;
}

/* Place the tiles inside `area`, scaled to fit the whole arrangement.
 *
 * The scale is uniform and derived from the arrangement's own bounding box, so
 * relative sizes stay true: a 1080p next to a 4K reads as a quarter of it,
 * because that is what it is. Fitting each tile to a cell would make the
 * picture prettier and useless. */
static void bar_canvas_layout(struct wlr_box area) {
	if (bar_popover.ntiles <= 0)
		return;

	/* Fit the view only when nothing is being dragged.
	 *
	 * Refitting per motion event looks reasonable and is unusable: the
	 * arrangement's bounding box grows as you pull a monitor away from its
	 * neighbours, so the scale shrinks, so everything -- including the tile
	 * under the pointer -- slides out from under it, and the layout->screen
	 * mapping the drag is inverting changes between the frame it was measured
	 * in and the frame it is applied to. Dragged 300px, the monitor landed
	 * 8000 layout pixels away. While a drag is live the transform is frozen
	 * and only the tiles' positions change. */
	if (bar_popover.drag_tile >= 0 && bar_popover.canvas_scale > 0.0) {
		float sc = bar_popover.mon && bar_popover.mon->wlr_output
					   ? bar_popover.mon->wlr_output->scale
					   : 1.0f;
		for (int32_t i = 0; i < bar_popover.ntiles; i++) {
			BarCanvasTile *t = &bar_popover.tiles[i];
			struct wlr_box *b = &t->layout_box;
			double s = bar_popover.canvas_scale;
			int32_t tw = (int32_t)(b->width * s), th = (int32_t)(b->height * s);
			if (tw < 24) tw = 24;
			if (th < 18) th = 18;
			asteroidz_tab_bar_node_set_size(t->node, tw, th);
			asteroidz_tab_bar_node_set_focus(t->node,
											 bar_popover.drag_tile == i);
			asteroidz_tab_bar_node_set_position(
				t->node,
				bar_popover.canvas_sx +
					(int32_t)((b->x - bar_popover.canvas_ox) * s),
				bar_popover.canvas_sy +
					(int32_t)((b->y - bar_popover.canvas_oy) * s));
			asteroidz_tab_bar_node_set_enabled(t->node, true);
			asteroidz_tab_bar_node_update(t->node, t->output, sc);
		}
		return;
	}

	int32_t x0 = INT32_MAX, y0 = INT32_MAX, x1 = INT32_MIN, y1 = INT32_MIN;
	for (int32_t i = 0; i < bar_popover.ntiles; i++) {
		struct wlr_box *b = &bar_popover.tiles[i].layout_box;
		if (b->x < x0) x0 = b->x;
		if (b->y < y0) y0 = b->y;
		if (b->x + b->width > x1) x1 = b->x + b->width;
		if (b->y + b->height > y1) y1 = b->y + b->height;
	}
	int32_t lw = x1 - x0, lh = y1 - y0;
	if (lw <= 0 || lh <= 0)
		return;

	/* leave a margin so a monitor dragged past the edge is still visible as
	 * having been dragged past the edge */
	const int32_t margin = 8;
	double sx = (double)(area.width - 2 * margin) / lw;
	double sy = (double)(area.height - 2 * margin) / lh;
	double s = sx < sy ? sx : sy;
	if (s <= 0.0)
		return;

	/* centre the drawn arrangement in the area */
	int32_t drawn_w = (int32_t)(lw * s), drawn_h = (int32_t)(lh * s);
	int32_t ox = area.x + (area.width - drawn_w) / 2;
	int32_t oy = area.y + (area.height - drawn_h) / 2;

	bar_popover.canvas = area;
	bar_popover.canvas_scale = s;
	bar_popover.canvas_ox = x0;
	bar_popover.canvas_oy = y0;
	bar_popover.canvas_sx = ox;
	bar_popover.canvas_sy = oy;

	float scale = bar_popover.mon && bar_popover.mon->wlr_output
					  ? bar_popover.mon->wlr_output->scale
					  : 1.0f;
	for (int32_t i = 0; i < bar_popover.ntiles; i++) {
		BarCanvasTile *t = &bar_popover.tiles[i];
		struct wlr_box *b = &t->layout_box;
		int32_t tx = ox + (int32_t)((b->x - x0) * s);
		int32_t ty = oy + (int32_t)((b->y - y0) * s);
		int32_t tw = (int32_t)(b->width * s);
		int32_t th = (int32_t)(b->height * s);
		/* a tile has to stay clickable however small the monitor is */
		if (tw < 24) tw = 24;
		if (th < 18) th = 18;

		char label[BAR_TEXT_MAX];
		snprintf(label, sizeof(label), "%s", t->output);

		asteroidz_tab_bar_node_apply_config(t->node, &config.theme);
		asteroidz_tab_bar_node_set_padding(t->node, 4, 2);
		asteroidz_tab_bar_node_set_size(t->node, tw, th);
		/* the tile being dragged is filled, so it is obvious which one is
		 * moving when two of them overlap */
		asteroidz_tab_bar_node_set_focus(t->node,
										 bar_popover.drag_tile == i);
		asteroidz_tab_bar_node_set_position(t->node, tx, ty);
		asteroidz_tab_bar_node_set_enabled(t->node, true);
		asteroidz_tab_bar_node_update(t->node, label, scale);
	}
}

/* Pointer press on a tile: start dragging it. */
static bool bar_canvas_press(int32_t idx, double cx, double cy) {
	if (!bar_popover_is_open(BAR_POPOVER_ARRANGE) || idx < 0 ||
		idx >= bar_popover.ntiles)
		return false;
	BarCanvasTile *t = &bar_popover.tiles[idx];
	if (!t->used)
		return false;
	bar_popover.drag_tile = idx;
	bar_popover.dragged = false;
	/* where in the tile the press landed, in LAYOUT units, so the monitor
	 * moves with the pointer instead of snapping its corner to it */
	double s = bar_popover.canvas_scale > 0 ? bar_popover.canvas_scale : 1.0;
	int32_t node_x = t->node->last_x;
	int32_t node_y = t->node->last_y;
	bar_popover.drag_dx = (cx - node_x) / s;
	bar_popover.drag_dy = (cy - node_y) / s;
	bar_canvas_layout(bar_popover.canvas); /* redraw: it is filled now */
	return true;
}

/* Pointer motion while dragging. Moves the TILE only -- the output itself is
 * left alone until the button comes up.
 *
 * Applying every intermediate position would re-run updatemons() and rearrange
 * every client on every motion event, which is a storm of relayouts for
 * positions the user is passing through rather than choosing. */
static bool bar_canvas_motion(double cx, double cy) {
	if (!bar_popover_is_open(BAR_POPOVER_ARRANGE) ||
		bar_popover.drag_tile < 0)
		return false;
	BarCanvasTile *t = &bar_popover.tiles[bar_popover.drag_tile];
	if (!t->used)
		return false;
	double s = bar_popover.canvas_scale > 0 ? bar_popover.canvas_scale : 1.0;
	/* canvas point -> layout point, minus where in the tile we grabbed */
	double lx = (cx - bar_popover.canvas_sx) / s + bar_popover.canvas_ox -
				bar_popover.drag_dx;
	double ly = (cy - bar_popover.canvas_sy) / s + bar_popover.canvas_oy -
				bar_popover.drag_dy;
	int32_t nx = (int32_t)lx, ny = (int32_t)ly;
	if (nx == t->layout_box.x && ny == t->layout_box.y)
		return true;
	t->layout_box.x = nx;
	t->layout_box.y = ny;
	bar_popover.dragged = true;
	bar_canvas_layout(bar_popover.canvas);
	return true;
}

/* Button release: snap, apply, and re-read the result. */
static bool bar_canvas_release(void) {
	if (!bar_popover_is_open(BAR_POPOVER_ARRANGE) ||
		bar_popover.drag_tile < 0)
		return false;
	int32_t idx = bar_popover.drag_tile;
	bool moved = bar_popover.dragged;
	bar_popover.drag_tile = -1;
	bar_popover.dragged = false;

	BarCanvasTile *t = &bar_popover.tiles[idx];
	Monitor *m = t->used ? bar_display_by_name(t->output) : NULL;
	if (moved && m) {
		struct wlr_box want = t->layout_box;
		bar_display_snap(m, &want);
		bar_display_move(m, want.x, want.y);
	}
	/* Re-read rather than trust the request: wlroots may not have placed the
	 * output exactly where it was asked, and the canvas must show what IS. */
	bar_canvas_sync_tiles();
	bar_popover_layout();
	return true;
}

static void bar_popover_open_arrange(Monitor *m, int32_t anchor_x) {
	if (bar_popover_is_open(BAR_POPOVER_ARRANGE)) {
		bar_popover_close();
		return;
	}
	if (!bar_popover_open(m, BAR_POPOVER_ARRANGE, anchor_x))
		return;
	bar_canvas_sync_tiles();
	if (bar_popover.ntiles == 0) {
		bar_popover_close();
		return;
	}

	int32_t n = 0;
	BarPopoverRow *r = bar_popover_row_get(n);
	if (r) {
		snprintf(r->text, sizeof(r->text), "%s", "Drag a display to move it");
		r->enabled = false;
		n++;
	}
	if ((r = bar_popover_row_get(n))) {
		snprintf(r->text, sizeof(r->text), "%s", "Save arrangement");
		snprintf(r->value, sizeof(r->value), "%s", BAR_POPOVER_VERB "save");
		n++;
	}
	if ((r = bar_popover_row_get(n))) {
		snprintf(r->text, sizeof(r->text), "%s", "‹  All outputs");
		snprintf(r->value, sizeof(r->value), "%s", BAR_POPOVER_VERB "back");
		n++;
	}
	bar_popover.nrows = n;
	bar_popover_layout();
}

/* ─── late content ────────────────────────────────────────────────────────── */

/* Both of these menus are built from data that is fetched when they OPEN and
 * arrives a round trip later -- a subprocess for nordvpn, a daemon push for
 * discord. Drawn once, the first open always showed the pre-answer version and
 * only a second one showed the real menu. Rebuilding in place is the whole
 * fix: nothing was broken except that nobody told the popover its content had
 * turned up.
 *
 * Rebuild rather than patch rows, because the row COUNT changes and the panel
 * has to resize with it. */
static void bar_popover_rebuild(enum bar_popover_kind kind) {
	if (!bar_popover_is_open(kind))
		return;
	Monitor *m = bar_popover.mon;
	if (!m)
		return;
	int32_t ax = m->m.x + bar_popover.anchor_x;
	/* Where the reader was.
	 *
	 * A rebuild is close-then-open, which starts the new menu at the top --
	 * and the daemon pushes a fresh channel list whenever anyone anywhere
	 * joins or leaves a voice channel, which on a busy account is often. So
	 * scrolling down a long menu was a race against strangers: the list would
	 * snap back to the top under the pointer, sometimes twice on the way to
	 * one row. The content is rebuilt; the viewport is not. */
	int32_t scroll = bar_popover.scroll;
	int32_t cursor = bar_popover.cursor;
	bar_popover_close();
	switch (kind) {
	case BAR_POPOVER_VPN:
		bar_popover_open_vpn(m, ax);
		break;
	case BAR_POPOVER_VOICE:
		bar_popover_open_voice(m, ax);
		break;
	default:
		break;
	}
	if (!bar_popover_is_open(kind) || scroll <= 0)
		return;
	/* Clamped: the new menu can be shorter than the old one. */
	int32_t max = bar_popover.total_rows - bar_popover.visible_rows;
	if (max < 0)
		max = 0;
	bar_popover.scroll = scroll > max ? max : scroll;
	if (cursor >= 0 && cursor < bar_popover.nrows)
		bar_popover.cursor = cursor;
	bar_popover_layout();
}

static void bar_popover_vpn_countries_arrived(void) {
	bar_popover_rebuild(BAR_POPOVER_VPN);
}

static void bar_popover_voice_channels_arrived(void) {
	bar_popover_rebuild(BAR_POPOVER_VOICE);
}

/* ─── input ───────────────────────────────────────────────────────────────── */

/* Run a row. Split out of the click handler so the keyboard can reach the
 * same code -- Enter on a row and a click on it have to mean one thing, and
 * two copies of this switch would drift apart. Returns true when the row
 * consumed the interaction. */
static bool bar_popover_activate_row(BarPopoverRow *r) {
	if (!r || !r->used)
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
		/* back to absolute: the stored anchor is monitor-relative, and
		 * bar_popover_open() re-relativises whatever it is given */
		int32_t ax = bar_popover.mon ? bar_popover.mon->m.x +
										   bar_popover.anchor_x
								 : bar_popover.anchor_x;
		char name[64];
		snprintf(name, sizeof(name), "%s", r->value);
		if (BAR_POPOVER_IS_VERB(name)) {
			if (!strcmp(name + 1, "arrange"))
				bar_popover_open_arrange(anchor_mon, ax);
			return true;
		}
		bar_popover_open_output(anchor_mon, ax, name);
		return true; /* already reopened against the output */
	}
	case BAR_POPOVER_ARRANGE: {
		if (!BAR_POPOVER_IS_VERB(r->value))
			return true;
		Monitor *anchor_mon = bar_popover.mon;
		/* back to absolute: the stored anchor is monitor-relative, and
		 * bar_popover_open() re-relativises whatever it is given */
		int32_t ax = bar_popover.mon ? bar_popover.mon->m.x +
										   bar_popover.anchor_x
								 : bar_popover.anchor_x;
		const char *verb = r->value + 1;
		if (!strcmp(verb, "back")) {
			bar_popover_open_outputs(anchor_mon, ax);
			return true;
		}
		if (!strcmp(verb, "save")) {
			int32_t missing = 0;
			int32_t saved = bar_display_save_positions(&missing);
			/* Report in place rather than closing. Writing to someone's config
			 * is exactly the kind of action where "did that do anything?" must
			 * not be a question, and an output with no block to write to is
			 * the one outcome nobody would predict. */
			BarPopoverRow *note = &bar_popover.rows[0];
			if (note->used) {
				if (missing > 0)
					snprintf(note->text, sizeof(note->text),
							 "Saved %d; %d had no output block", saved,
							 missing);
				else if (saved > 0)
					snprintf(note->text, sizeof(note->text),
							 "Saved %d display position%s", saved,
							 saved == 1 ? "" : "s");
				else
					snprintf(note->text, sizeof(note->text), "%s",
							 "Nothing saved: no output blocks found");
				bar_popover_layout();
			}
			return true;
		}
		return true;
	}
	case BAR_POPOVER_OUTPUT: {
		if (!r->enabled)
			return true; /* a reading, not a control */
		Monitor *anchor_mon = bar_popover.mon;
		/* back to absolute: the stored anchor is monitor-relative, and
		 * bar_popover_open() re-relativises whatever it is given */
		int32_t ax = bar_popover.mon ? bar_popover.mon->m.x +
										   bar_popover.anchor_x
								 : bar_popover.anchor_x;
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
		/* back to absolute: the stored anchor is monitor-relative, and
		 * bar_popover_open() re-relativises whatever it is given */
		int32_t ax = bar_popover.mon ? bar_popover.mon->m.x +
										   bar_popover.anchor_x
								 : bar_popover.anchor_x;
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
		/* back to absolute: the stored anchor is monitor-relative, and
		 * bar_popover_open() re-relativises whatever it is given */
		int32_t ax = bar_popover.mon ? bar_popover.mon->m.x +
										   bar_popover.anchor_x
								 : bar_popover.anchor_x;
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
		/* back to absolute: the stored anchor is monitor-relative, and
		 * bar_popover_open() re-relativises whatever it is given */
		int32_t ax = bar_popover.mon ? bar_popover.mon->m.x +
										   bar_popover.anchor_x
								 : bar_popover.anchor_x;
		char key[176];
		snprintf(key, sizeof(key), "%s", r->value);
		if (BAR_POPOVER_IS_VERB(key)) {
			if (!strcmp(key + 1, "edit")) {
				/* the schedule is a JSON file; open it in whatever handles
				 * JSON rather than pretend a row can be a form */
				char path[512];
				bar_med_path(path, sizeof(path));
				char *const argv[] = {"xdg-open", path, NULL};
				async_spawn(event_loop, argv, NULL, NULL);
			}
			break;
		}
		bar_popover_open_med_dose(anchor_mon, ax, key);
		return true;
	}
	case BAR_POPOVER_MED_DOSE: {
		if (!r->enabled || !BAR_POPOVER_IS_VERB(r->value))
			return true;
		Monitor *anchor_mon = bar_popover.mon;
		/* back to absolute: the stored anchor is monitor-relative, and
		 * bar_popover_open() re-relativises whatever it is given */
		int32_t ax = bar_popover.mon ? bar_popover.mon->m.x +
										   bar_popover.anchor_x
								 : bar_popover.anchor_x;
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
			/* verbs, distinguished from a channel id by a marker byte no
			 * Discord snowflake can contain */
			const char *verb = r->value + 1;
			if (!strcmp(verb, "mute")) {
				bar_dv_send("{\"cmd\":\"mute\"}");
			} else if (!strcmp(verb, "leave")) {
				bar_dv_send("{\"cmd\":\"leave\"}");
			} else if (!strcmp(verb, "ptt")) {
				/* empty key = "ask me": the daemon re-registers the shortcut,
				 * which lands back here as a portal bind and opens the picker */
				bar_dv_send("{\"cmd\":\"rebind_ptt\",\"key\":\"\"}");
			} else if (!strcmp(verb, "connect")) {
				bar_dv_send("{\"cmd\":\"connect\"}");
			} else if (!strcmp(verb, "disconnect")) {
				bar_dv_send("{\"cmd\":\"disconnect\"}");
			} else if (!strcmp(verb, "dstart")) {
				bar_dv_restart_daemon();
			} else if (!strcmp(verb, "dstop")) {
				bar_dv_send("{\"cmd\":\"shutdown\"}");
			}
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
			/* back to absolute: the stored anchor is monitor-relative, and
		 * bar_popover_open() re-relativises whatever it is given */
		int32_t ax = bar_popover.mon ? bar_popover.mon->m.x +
										   bar_popover.anchor_x
								 : bar_popover.anchor_x;
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
		/* a canvas tile is INSIDE the popover, so a press on one is not a
		 * click outside it -- dismissing here would close the panel the
		 * instant a drag began */
		if (d->type == ASTEROIDZ_BAR_CANVAS_NODE)
			return false;
	}
	/* mid-drag the pointer routinely leaves the tile it grabbed; the button
	 * that comes up at the end of that is the drag ending, not a click
	 * elsewhere */
	if (bar_popover.drag_tile >= 0)
		return false;
	bar_popover_close();
	return true;
}

static bool bar_popover_handle_node_click(AsteroidzNodeData *hit,
										  uint32_t button) {
	BarPopoverRow *r = hit ? (BarPopoverRow *)hit->node_data : NULL;
	if (!r || !r->used || button != BTN_LEFT)
		return false;
	/* the pointer moves the keyboard's place too, so tabbing on after
	 * clicking carries on from where you clicked rather than from the top */
	bar_popover.cursor = (int32_t)(r - bar_popover.rows);
	return bar_popover_activate_row(r);
}

/* ─── keyboard ────────────────────────────────────────────────────────────── */

/* Can the keyboard land on this row? Separators and readings cannot be acted
 * on, so stopping the cursor there would just make Down feel broken. A stepper
 * CAN be landed on -- left/right adjust it. */
static bool bar_popover_row_focusable(const BarPopoverRow *r) {
	return r && r->used && r->enabled && !r->separator;
}

/* Move the cursor `dir` focusable rows, and bring it into the viewport.
 * Wraps, because a menu is a ring: Down off the bottom of a short list should
 * reach the top rather than stopping dead. */
static void bar_popover_cursor_move(int32_t dir) {
	int32_t n = bar_popover.nrows;
	if (n <= 0)
		return;
	int32_t start = bar_popover.cursor;
	/* first press enters the list from whichever end you pressed toward */
	int32_t i = start < 0 ? (dir > 0 ? -1 : n) : start;
	for (int32_t tried = 0; tried < n; tried++) {
		i += dir;
		if (i < 0)
			i = n - 1;
		if (i >= n)
			i = 0;
		if (bar_popover_row_focusable(&bar_popover.rows[i])) {
			bar_popover.cursor = i;
			/* scroll only as far as it takes to show it: a cursor stepping off
			 * the edge should move the list by one row, not recentre it */
			if (bar_popover.visible_rows > 0 &&
				bar_popover.total_rows > bar_popover.visible_rows) {
				if (i < bar_popover.scroll)
					bar_popover.scroll = i;
				else if (i >= bar_popover.scroll + bar_popover.visible_rows)
					bar_popover.scroll = i - bar_popover.visible_rows + 1;
			}
			bar_popover_layout();
			return;
		}
	}
}

/* Escape closes; arrows walk the rows; Enter runs one. Returns true when the
 * key was consumed.
 *
 * There is still no keyboard GRAB and the popover never takes focus -- this
 * runs from the compositor's own key handler, ahead of the binding tables and
 * the focused client, exactly where Escape was already being caught. Only the
 * keys listed here are taken; everything else keeps working normally while a
 * menu is up, which is the property that made a grab unattractive in the first
 * place. */
static bool bar_popover_handle_key(uint32_t keysym) {
	if (!bar_popover.open)
		return false;
	switch (keysym) {
	case XKB_KEY_Escape:
		bar_popover_close();
		return true;
	case XKB_KEY_Down:
		bar_popover_cursor_move(1);
		return true;
	case XKB_KEY_Up:
		bar_popover_cursor_move(-1);
		return true;
	case XKB_KEY_Page_Down:
		bar_popover_scroll_by(bar_popover.visible_rows);
		return true;
	case XKB_KEY_Page_Up:
		bar_popover_scroll_by(-bar_popover.visible_rows);
		return true;
	case XKB_KEY_Left:
	case XKB_KEY_Right: {
		/* a stepper under the cursor is what these adjust; on anything else
		 * they are not ours, so let them through to whatever is focused */
		if (bar_popover.cursor < 0)
			return false;
		BarPopoverRow *r = &bar_popover.rows[bar_popover.cursor];
		if (!r->used || !r->stepper)
			return false;
		int32_t v = r->step_value +
					(keysym == XKB_KEY_Right ? r->vstep : -r->vstep);
		if (v < r->vmin)
			v = r->vmin;
		if (v > r->vmax)
			v = r->vmax;
		if (v != r->step_value) {
			r->step_value = v;
			bar_popover_apply_step(r);
		}
		return true;
	}
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
	case XKB_KEY_space:
		if (bar_popover.cursor < 0)
			return false; /* nothing aimed at: not ours to swallow */
		bar_popover_activate_row(&bar_popover.rows[bar_popover.cursor]);
		return true;
	default:
		return false;
	}
}

#endif /* ASTEROIDZ_BAR_POPOVER_H */
