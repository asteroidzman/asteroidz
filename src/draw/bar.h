#ifndef ASTEROIDZ_BAR_H
#define ASTEROIDZ_BAR_H

#ifdef ASTEROIDZ_NATIVE_BAR

/* The compositor-native status bar.
 *
 * A bar is a row of `asteroidz_tab_bar_node` pills -- the same widget the
 * titlebars and monocle tab strips are built from -- parented to a per-monitor
 * scene tree on LyrTop. It is NOT a layer-shell client: it reads compositor
 * state directly (tags, clients, monitors are structs, not JSON over a
 * socket), it is laid out in `arrangelayers` alongside the real layer
 * surfaces, and it renders at the output's own scale, so a 1.0-scale and a
 * 0.75-scale monitor both come out right without any per-monitor fudge factor.
 *
 * Reserving space: `bar_reserve` shrinks the usable area BEFORE layer surfaces
 * are arranged, so an external bar's exclusive zone stacks below this one
 * rather than fighting it. That is what makes the migration incremental --
 * waybar can keep owning the modules this bar does not implement yet.
 *
 * Stage 1 scope: the frame, the layout engine, hit testing, and the two
 * modules that need no external process (tags, clock). Modules that wrap a
 * CLI or a D-Bus service, and the popover layer, come later; until then the
 * corresponding waybar modules stay where they are.
 */

#define BAR_MAX_PILLS 16
#define BAR_MAX_MODULES 12
#define BAR_TEXT_MAX 256

enum bar_module_kind {
	BAR_MODULE_NONE = 0,
	BAR_MODULE_TAGS,
	BAR_MODULE_CLOCK,
	BAR_MODULE_TITLE,
	BAR_MODULE_LAYOUT,
	/* System metrics. Deliberately limited to what can be read straight out
	 * of /proc and /sys: no subprocess, no D-Bus, no network. That is the
	 * whole reason these are cheap to run in-compositor -- the waybar plugins
	 * they replace fork wpctl/nmcli/curl on their main loop, which is a
	 * stutter in a bar process and a dropped frame in a compositor. */
	BAR_MODULE_CPU,
	BAR_MODULE_MEMORY,
	BAR_MODULE_NETWORK,
	BAR_MODULE_IDLE,
};

enum bar_slot { BAR_SLOT_LEFT = 0, BAR_SLOT_CENTER, BAR_SLOT_RIGHT,
				BAR_SLOT_COUNT };

/* One clickable element. A module owns between zero and BAR_MAX_PILLS of
 * these: the clock is a single pill, the tags module is one pill per tag. */
typedef struct BarPill {
	struct asteroidz_tab_bar_node *node;
	/* Hit-test tag handed to the scene node's `data`. Its first member is the
	 * node-type enum, which is what `handle_buttonpress` reads generically --
	 * see the _Static_assert on AsteroidzNodeData. Ownership passes to the
	 * tab-bar node, which frees it in _destroy; this is a borrowed pointer. */
	AsteroidzNodeData *hit;
	struct BarModule *module;
	/* module-defined payload: the tag number for BAR_MODULE_TAGS, unused
	 * elsewhere. Kept as a plain integer so a pill never holds a pointer to
	 * anything with a shorter lifetime than the bar itself. */
	uint32_t arg;
	int32_t width;
	/* When non-zero the pill is exactly this wide regardless of its text.
	 * Modules whose content changes shape -- a clock, a percentage crossing
	 * 9%->10%, a throughput crossing K->M, a window title -- pin this to the
	 * width of their widest possible content, so the slot never reflows and
	 * neighbouring pills never move under the pointer. */
	int32_t fixed_width;
	char text[BAR_TEXT_MAX];
	bool used;
} BarPill;

typedef struct BarModule {
	enum bar_module_kind kind;
	enum bar_slot slot;
	Monitor *mon;
	BarPill pills[BAR_MAX_PILLS];
	int32_t npills; /* pills currently in use (<= BAR_MAX_PILLS) */
	int32_t width;  /* laid-out width including inter-pill spacing */
} BarModule;

/* The translucent rounded backdrop behind one slot's pills. The bar has no
 * background of its own -- it is fully transparent -- so these three panels
 * are the only surfaces, exactly like the grouped panels in the waybar config
 * this replaces. Blur and shadow are the same scenefx nodes the overview's
 * top strip uses. */
typedef struct BarPanel {
	struct wlr_scene_blur *blur;
	struct wlr_scene_rect *bg;
	struct wlr_scene_shadow *shadow;
} BarPanel;

typedef struct AsteroidzBar {
	Monitor *mon;
	struct wlr_scene_tree *tree;
	/* panels strictly below pills; two trees rather than per-node restacking
	 * so adding a pill can never land underneath its own backdrop */
	struct wlr_scene_tree *panel_tree;
	struct wlr_scene_tree *pill_tree;
	BarPanel panels[BAR_SLOT_COUNT];
	BarModule modules[BAR_MAX_MODULES];
	int32_t nmodules;
	/* the strip's own rect in layout coordinates, recomputed on every
	 * layout pass (used by bar_reserve and for pointer hit-testing) */
	struct wlr_box box;
	/* Hash of everything the bar currently displays. bar_update hangs off
	 * printstatus(), which fires on every arrange -- including each step of
	 * a drag -- and a full relayout re-measures every pill through pango.
	 * Hashing the inputs first is far cheaper than that, so an arrange that
	 * changes nothing the bar shows costs a client-list walk and no more.
	 * Same discipline as the media module's REDRAW_EPS guard. */
	uint64_t digest;
	bool digest_valid;
} AsteroidzBar;

static struct wl_event_source *bar_clock_timer = NULL;
static struct wl_event_source *bar_metrics_timer = NULL;

static void bar_update(Monitor *m);
static void bar_update_all(void);

/* ─── module registry ─────────────────────────────────────────────────────── */

static enum bar_module_kind bar_module_kind_from_name(const char *name) {
	if (!name || !*name)
		return BAR_MODULE_NONE;
	if (strcmp(name, "tags") == 0)
		return BAR_MODULE_TAGS;
	if (strcmp(name, "clock") == 0)
		return BAR_MODULE_CLOCK;
	if (strcmp(name, "title") == 0)
		return BAR_MODULE_TITLE;
	if (strcmp(name, "layout") == 0)
		return BAR_MODULE_LAYOUT;
	if (strcmp(name, "cpu") == 0)
		return BAR_MODULE_CPU;
	if (strcmp(name, "memory") == 0 || strcmp(name, "mem") == 0)
		return BAR_MODULE_MEMORY;
	if (strcmp(name, "network") == 0 || strcmp(name, "net") == 0)
		return BAR_MODULE_NETWORK;
	if (strcmp(name, "idle") == 0 || strcmp(name, "idle-inhibitor") == 0)
		return BAR_MODULE_IDLE;
	return BAR_MODULE_NONE;
}

static bool bar_kind_is_metric(enum bar_module_kind k) {
	return k == BAR_MODULE_CPU || k == BAR_MODULE_MEMORY ||
		   k == BAR_MODULE_NETWORK;
}

/* ─── system metrics ──────────────────────────────────────────────────────── */

/* Sampled once per tick for the whole system, not per monitor: these readings
 * are machine-wide, so two outputs must not mean two reads of /proc. */
static struct {
	int32_t cpu_pct;
	int32_t mem_pct;
	uint64_t rx_bytes, tx_bytes; /* running totals, for the delta */
	double rx_rate, tx_rate;     /* bytes/sec since the previous sample */
	char iface[32];
	bool link_up;
	uint64_t prev_busy, prev_total;
	struct timespec prev_ts;
	bool primed;
} bar_metrics;

static void bar_sample_cpu(void) {
	FILE *f = fopen("/proc/stat", "re");
	if (!f)
		return;
	char label[16];
	uint64_t v[10] = {0};
	int n = fscanf(f, "%15s %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu", label,
				   &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7],
				   &v[8], &v[9]);
	fclose(f);
	if (n < 5)
		return;
	uint64_t total = 0;
	for (int i = 0; i < 10; i++)
		total += v[i];
	uint64_t idle = v[3] + v[4]; /* idle + iowait */
	uint64_t busy = total - idle;
	if (bar_metrics.prev_total && total > bar_metrics.prev_total) {
		uint64_t dt = total - bar_metrics.prev_total;
		uint64_t db = busy - bar_metrics.prev_busy;
		bar_metrics.cpu_pct = (int32_t)((db * 100 + dt / 2) / dt);
	}
	bar_metrics.prev_total = total;
	bar_metrics.prev_busy = busy;
}

static void bar_sample_memory(void) {
	FILE *f = fopen("/proc/meminfo", "re");
	if (!f)
		return;
	char line[256];
	uint64_t total = 0, avail = 0;
	while (fgets(line, sizeof(line), f)) {
		if (!total)
			sscanf(line, "MemTotal: %lu kB", &total);
		if (!avail)
			sscanf(line, "MemAvailable: %lu kB", &avail);
		if (total && avail)
			break;
	}
	fclose(f);
	if (total)
		bar_metrics.mem_pct = (int32_t)(((total - avail) * 100 + total / 2) /
										total);
}

/* First non-loopback interface reporting operstate "up", with the summed
 * byte counters of every interface (a laptop switching wifi<->ethernet should
 * not show a throughput spike from the counters resetting). */
static void bar_sample_network(void) {
	DIR *d = opendir("/sys/class/net");
	if (!d)
		return;
	struct dirent *e;
	uint64_t rx = 0, tx = 0;
	char up_iface[32] = "";
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.' || strcmp(e->d_name, "lo") == 0)
			continue;
		char path[512];
		char buf[64];
		FILE *f;

		snprintf(path, sizeof(path), "/sys/class/net/%s/operstate", e->d_name);
		if ((f = fopen(path, "re"))) {
			if (fgets(buf, sizeof(buf), f) && strncmp(buf, "up", 2) == 0 &&
				!up_iface[0])
				snprintf(up_iface, sizeof(up_iface), "%s", e->d_name);
			fclose(f);
		}
		snprintf(path, sizeof(path), "/sys/class/net/%s/statistics/rx_bytes",
				 e->d_name);
		if ((f = fopen(path, "re"))) {
			unsigned long v = 0;
			if (fscanf(f, "%lu", &v) == 1)
				rx += v;
			fclose(f);
		}
		snprintf(path, sizeof(path), "/sys/class/net/%s/statistics/tx_bytes",
				 e->d_name);
		if ((f = fopen(path, "re"))) {
			unsigned long v = 0;
			if (fscanf(f, "%lu", &v) == 1)
				tx += v;
			fclose(f);
		}
	}
	closedir(d);

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	if (bar_metrics.primed) {
		double dt = (now.tv_sec - bar_metrics.prev_ts.tv_sec) +
					(now.tv_nsec - bar_metrics.prev_ts.tv_nsec) / 1e9;
		if (dt > 0.05) {
			/* counters only ever climb; a decrease means an interface went
			 * away, so drop that sample rather than reporting a negative */
			bar_metrics.rx_rate = rx >= bar_metrics.rx_bytes
									  ? (rx - bar_metrics.rx_bytes) / dt
									  : 0.0;
			bar_metrics.tx_rate = tx >= bar_metrics.tx_bytes
									  ? (tx - bar_metrics.tx_bytes) / dt
									  : 0.0;
		}
	}
	bar_metrics.rx_bytes = rx;
	bar_metrics.tx_bytes = tx;
	bar_metrics.prev_ts = now;
	bar_metrics.primed = true;
	bar_metrics.link_up = up_iface[0] != '\0';
	snprintf(bar_metrics.iface, sizeof(bar_metrics.iface), "%s", up_iface);
}

static void bar_metrics_sample(void) {
	bar_sample_cpu();
	bar_sample_memory();
	bar_sample_network();
}

/* "1.2M" / "340K" / "12" -- short enough that the pill does not resize on
 * every sample, which would jitter the whole slot. */
static void bar_fmt_rate(double bytes_per_sec, char *out, size_t len) {
	if (bytes_per_sec >= 1024.0 * 1024.0)
		snprintf(out, len, "%.1fM", bytes_per_sec / (1024.0 * 1024.0));
	else if (bytes_per_sec >= 1024.0)
		snprintf(out, len, "%.0fK", bytes_per_sec / 1024.0);
	else
		snprintf(out, len, "%.0f", bytes_per_sec);
}

/* ─── pill lifecycle ──────────────────────────────────────────────────────── */

static void bar_pill_release(BarPill *p) {
	if (!p->used)
		return;
	if (p->node) {
		/* Takes the hit-test tag with it: _destroy frees whatever was passed
		 * as the node data at _create time. Freeing p->hit here as well is a
		 * double free -- it aborted in glibc on the first config reload. */
		asteroidz_tab_bar_node_destroy(p->node);
		p->node = NULL;
	}
	p->hit = NULL;
	p->used = false;
	p->width = 0;
	p->text[0] = '\0';
}

/* Get pill `idx` of `mod`, creating its scene node on first use. Pills are
 * pooled rather than destroyed and recreated every refresh: the tags module
 * rebuilds its text on every view change, and churning a wlr_buffer-backed
 * scene node at that rate is exactly the allocator pressure the tab-bar
 * per-tick g_strdup fix was about. */
static BarPill *bar_pill_get(BarModule *mod, int32_t idx) {
	if (idx < 0 || idx >= BAR_MAX_PILLS)
		return NULL;
	BarPill *p = &mod->pills[idx];
	if (p->used)
		return p;

	AsteroidzNodeData *hit = ecalloc(1, sizeof(AsteroidzNodeData));
	hit->type = ASTEROIDZ_BAR_NODE;
	hit->node_data = p;

	AsteroidzBar *bar = mod->mon ? mod->mon->bar : NULL;
	if (!bar || !bar->pill_tree) {
		free(hit);
		return NULL;
	}
	p->node =
		asteroidz_tab_bar_node_create(hit, bar->pill_tree, config.theme, 0, 0);
	if (!p->node) {
		free(hit);
		return NULL;
	}
	p->hit = hit;
	p->module = mod;
	p->used = true;
	asteroidz_tab_bar_node_set_shadow(p->node, config.shadows,
									  config.shadows_blur,
									  (int32_t)config.shadows_position_y,
									  config.shadowscolor);
	return p;
}

/* Style one pill. With a backing panel, a resting pill draws NO background of
 * its own -- the panel is the surface, and only the selected tag (or an urgent
 * one) gets a filled pill on top of it. Without a panel every pill carries the
 * theme's resting colours, which is the standalone-pills look. */
static void bar_pill_style(BarPill *p, bool active, bool urgent) {
	static const float transparent[4] = {0.0f, 0.0f, 0.0f, 0.0f};
	asteroidz_tab_bar_node_apply_config(p->node, &config.theme);
	if (urgent && !active) {
		asteroidz_tab_bar_node_set_colors(p->node, config.theme.fg_color,
										  config.theme.urgent_color);
		asteroidz_tab_bar_node_set_focus(p->node, false);
		return;
	}
	if (!active && config.bar_panel_enable)
		asteroidz_tab_bar_node_set_colors(p->node, config.theme.fg_color,
										  transparent);
	asteroidz_tab_bar_node_set_focus(p->node, active);
}

/* Width of the widest text this module can ever show, so its pill can be
 * pinned to it. Measured through the pill's own node, so it uses the same
 * font, padding and icon state the real draw will. */
static int32_t bar_template_width(BarPill *p, const char *template,
								  int32_t height) {
	return asteroidz_tab_bar_node_measure_width(p->node, template, height);
}

/* The clock's width depends on the user's strftime format, so probe it: one
 * sample per month (weekday cycled with it) covers %a/%b name-length
 * variation, and wide digits cover the numeric fields. Cached on the format
 * string -- this is 12 pango measurements, not something to redo per tick. */
static int32_t bar_clock_fixed_width(BarPill *p, int32_t height) {
	static char cached_fmt[64];
	static int32_t cached_h = -1;
	static int32_t cached_w = 0;
	if (cached_w > 0 && cached_h == height &&
		strcmp(cached_fmt, config.bar_clock_format) == 0)
		return cached_w;

	int32_t widest = 0;
	for (int mon = 0; mon < 12; mon++) {
		struct tm tm = {0};
		tm.tm_year = 126; /* 2026 */
		tm.tm_mon = mon;
		tm.tm_mday = 28;
		tm.tm_wday = mon % 7;
		tm.tm_hour = 22;
		tm.tm_min = 28;
		tm.tm_sec = 28;
		char buf[BAR_TEXT_MAX];
		if (strftime(buf, sizeof(buf), config.bar_clock_format, &tm) == 0)
			continue;
		int32_t w = bar_template_width(p, buf, height);
		if (w > widest)
			widest = w;
	}
	snprintf(cached_fmt, sizeof(cached_fmt), "%s", config.bar_clock_format);
	cached_h = height;
	cached_w = widest;
	return widest;
}

/* Path to one of the waybar plugin SVGs, so the native bar renders the exact
 * same artwork as the modules it replaces. resolve_icon_path() takes absolute
 * paths as-is, and a missing file resolves to no icon rather than an error, so
 * an incomplete asset install degrades to text. */
static void bar_icon_path(char *out, size_t len, const char *rel) {
	snprintf(out, len, "%s/%s", config.bar_icon_dir, rel);
}

/* ─── per-module content ──────────────────────────────────────────────────── */

/* Which tags on `m` currently hold at least one client. Mirrors the same walk
 * the IPC layer does; kept local so the bar never round-trips through IPC to
 * learn something it can read from the struct. */
static uint32_t bar_occupied_tags(Monitor *m) {
	uint32_t occ = 0;
	Client *c = NULL;
	wl_list_for_each(c, &clients, link) {
		if (c->mon == m)
			occ |= c->tags;
	}
	return occ & TAGMASK;
}

static uint32_t bar_urgent_tags(Monitor *m) {
	uint32_t urg = 0;
	Client *c = NULL;
	wl_list_for_each(c, &clients, link) {
		if (c->mon == m && c->isurgent)
			urg |= c->tags;
	}
	return urg & TAGMASK;
}

static void bar_module_refresh_tags(BarModule *mod) {
	Monitor *m = mod->mon;
	uint32_t sel = m->tagset[m->seltags] & TAGMASK;
	uint32_t occ = bar_occupied_tags(m);
	uint32_t urg = bar_urgent_tags(m);
	int32_t n = 0;

	for (uint32_t t = 1; t <= LENGTH(tags) && n < BAR_MAX_PILLS; t++) {
		uint32_t mask = 1u << (t - 1);
		bool selected = (sel & mask) != 0;
		bool occupied = (occ & mask) != 0;
		/* By default an empty, unselected tag gets no pill: that matches the
		 * waybar workspace module this replaces, and keeps a 9-tag setup from
		 * permanently spending 9 pills of width on tags holding nothing. It
		 * does mean a fresh session can show a single pill, which reads as
		 * broken rather than as tidy -- `bar { show-all-tags true }` gives
		 * the dwm/dwl behaviour of always rendering every configured tag. */
		if (!config.bar_show_all_tags && !selected && !occupied)
			continue;

		BarPill *p = bar_pill_get(mod, n);
		if (!p)
			break;
		p->arg = t;
		tag_display_name(m, t, p->text, sizeof(p->text));
		bar_pill_style(p, selected, (urg & mask) != 0);
		n++;
	}

	for (int32_t i = n; i < BAR_MAX_PILLS; i++)
		bar_pill_release(&mod->pills[i]);
	mod->npills = n;
}

static void bar_module_refresh_clock(BarModule *mod) {
	BarPill *p = bar_pill_get(mod, 0);
	if (!p) {
		mod->npills = 0;
		return;
	}
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	if (strftime(p->text, sizeof(p->text), config.bar_clock_format, &tm) == 0)
		snprintf(p->text, sizeof(p->text), "??:??");
	p->arg = 0;
	p->fixed_width = bar_clock_fixed_width(p, config.bar_height);
	bar_pill_style(p, false, false);
	mod->npills = 1;
	for (int32_t i = 1; i < BAR_MAX_PILLS; i++)
		bar_pill_release(&mod->pills[i]);
}

static void bar_module_refresh_title(BarModule *mod) {
	Monitor *m = mod->mon;
	Client *sel = m->sel;
	if (!sel || !VISIBLEON(sel, m)) {
		/* no focused window: drop the pill entirely rather than showing an
		 * empty one, so the slot collapses */
		for (int32_t i = 0; i < BAR_MAX_PILLS; i++)
			bar_pill_release(&mod->pills[i]);
		mod->npills = 0;
		return;
	}
	BarPill *p = bar_pill_get(mod, 0);
	if (!p) {
		mod->npills = 0;
		return;
	}
	const char *title = client_get_title(sel);
	snprintf(p->text, sizeof(p->text), "%s", title ? title : "");
	asteroidz_tab_bar_node_set_icon(p->node, sel->icon_name
												 ? sel->icon_name
												 : client_get_appid(sel));
	/* A title changes with every focus change and every tab a browser opens;
	 * left free-sized it would resize the whole left slot constantly. Pin it
	 * and let the pill ellipsise. */
	p->fixed_width = config.bar_title_width;
	/* resting colours, not the focus pair: a highlighted title competed with
	 * the selected-tag pill for "this is the active thing". */
	bar_pill_style(p, false, false);
	p->arg = 0;
	mod->npills = 1;
	for (int32_t i = 1; i < BAR_MAX_PILLS; i++)
		bar_pill_release(&mod->pills[i]);
}

static void bar_module_refresh_layout(BarModule *mod) {
	Monitor *m = mod->mon;
	BarPill *p = bar_pill_get(mod, 0);
	if (!p) {
		mod->npills = 0;
		return;
	}
	const Layout *lt =
		m->pertag ? m->pertag->ltidxs[m->pertag->curtag] : NULL;
	/* The artwork is keyed on the layout NAME, which is exactly what the
	 * asset filenames use (tile/scroller/monocle/float.svg) -- the same SVGs
	 * the waybar workspace plugin draws. The symbol stays as the text
	 * fallback for a layout with no artwork installed. */
	char icon[512];
	bar_icon_path(icon, sizeof(icon),
				  "waybar-asteroidz-workspaces/layouts/");
	if (lt && lt->name) {
		size_t n = strlen(icon);
		snprintf(icon + n, sizeof(icon) - n, "%s.svg", lt->name);
		asteroidz_tab_bar_node_set_icon(p->node, icon);
	}
	p->text[0] = '\0';
	/* the pill is clickable and cycles layouts, so it must not change width
	 * as the symbol changes under the pointer */
	/* square: icon only, so it never resizes as the layout cycles */
	p->fixed_width = config.bar_height;
	bar_pill_style(p, false, false);
	p->arg = 0;
	mod->npills = 1;
	for (int32_t i = 1; i < BAR_MAX_PILLS; i++)
		bar_pill_release(&mod->pills[i]);
}

/* One-pill metric modules. The glyphs are Nerd Font (the same family the
 * waybar modules these replace used); a font without them falls back through
 * fontconfig rather than drawing nothing. A reading past its threshold takes
 * the theme's urgent colour, which is how the sysinfo pill carried load. */
static void bar_module_refresh_metric(BarModule *mod) {
	BarPill *p = bar_pill_get(mod, 0);
	if (!p) {
		mod->npills = 0;
		return;
	}
	bool hot = false;
	char icon[512];
	switch (mod->kind) {
	case BAR_MODULE_CPU:
		bar_icon_path(icon, sizeof(icon), "waybar-sysmon/cpu.svg");
		asteroidz_tab_bar_node_set_icon(p->node, icon);
		snprintf(p->text, sizeof(p->text), "%d%%", bar_metrics.cpu_pct);
		p->fixed_width = bar_template_width(p, "100%", config.bar_height);
		hot = bar_metrics.cpu_pct >= 85;
		break;
	case BAR_MODULE_MEMORY:
		bar_icon_path(icon, sizeof(icon), "waybar-sysmon/ram.svg");
		asteroidz_tab_bar_node_set_icon(p->node, icon);
		snprintf(p->text, sizeof(p->text), "%d%%", bar_metrics.mem_pct);
		p->fixed_width = bar_template_width(p, "100%", config.bar_height);
		hot = bar_metrics.mem_pct >= 90;
		break;
	case BAR_MODULE_NETWORK: {
		/* predictable interface names: a wireless one starts with 'w' */
		const char *art = !bar_metrics.link_up ? "waybar-network/disconnected.svg"
							: bar_metrics.iface[0] == 'w'
								  ? "waybar-network/wifi3.svg"
								  : "waybar-network/ethernet.svg";
		bar_icon_path(icon, sizeof(icon), art);
		asteroidz_tab_bar_node_set_icon(p->node, icon);
		if (!bar_metrics.link_up) {
			p->text[0] = '\0';
			hot = true;
		} else {
			char rx[16], tx[16];
			bar_fmt_rate(bar_metrics.rx_rate, rx, sizeof(rx));
			bar_fmt_rate(bar_metrics.tx_rate, tx, sizeof(tx));
			snprintf(p->text, sizeof(p->text), "↓%s ↑%s", rx, tx);
		}
		/* widest reachable rendering: the K->M transition and the digit count
		 * both change the string length every few seconds otherwise */
		p->fixed_width =
			bar_template_width(p, "↓999.9M ↑999.9M", config.bar_height);
		break;
	}
	default:
		break;
	}
	p->arg = 0;
	bar_pill_style(p, false, hot);
	mod->npills = 1;
	for (int32_t i = 1; i < BAR_MAX_PILLS; i++)
		bar_pill_release(&mod->pills[i]);
}

/* Idle inhibit toggle. Reflects the compositor's manual override -- not the
 * client-driven idle_inhibit_v1 state, which no user action controls. */
static void bar_module_refresh_idle(BarModule *mod) {
	BarPill *p = bar_pill_get(mod, 0);
	if (!p) {
		mod->npills = 0;
		return;
	}
	/* The same glyph pair the waybar idle_inhibitor module was configured
	 * with; there is no SVG for this one in the plugin assets. */
	static const char *on = "\U000F0176";  /* activated   */
	static const char *off = "\U000F0FAA"; /* deactivated */
	snprintf(p->text, sizeof(p->text), "%s",
			 idle_inhibit_manual ? on : off);
	p->arg = 0;
	/* Widest of the two states, so toggling never resizes -- and measured
	 * rather than pinned square: a square pill at the theme's padding.x
	 * leaves almost no text area and ellipsised the glyph to "...". */
	int32_t wa = bar_template_width(p, on, config.bar_height);
	int32_t wb = bar_template_width(p, off, config.bar_height);
	p->fixed_width = wa > wb ? wa : wb;
	bar_pill_style(p, idle_inhibit_manual, false);
	mod->npills = 1;
	for (int32_t i = 1; i < BAR_MAX_PILLS; i++)
		bar_pill_release(&mod->pills[i]);
}

static void bar_module_refresh(BarModule *mod) {
	switch (mod->kind) {
	case BAR_MODULE_TAGS:
		bar_module_refresh_tags(mod);
		break;
	case BAR_MODULE_CLOCK:
		bar_module_refresh_clock(mod);
		break;
	case BAR_MODULE_TITLE:
		bar_module_refresh_title(mod);
		break;
	case BAR_MODULE_LAYOUT:
		bar_module_refresh_layout(mod);
		break;
	case BAR_MODULE_CPU:
	case BAR_MODULE_MEMORY:
	case BAR_MODULE_NETWORK:
		bar_module_refresh_metric(mod);
		break;
	case BAR_MODULE_IDLE:
		bar_module_refresh_idle(mod);
		break;
	default:
		mod->npills = 0;
		break;
	}
}

/* ─── layout ──────────────────────────────────────────────────────────────── */

/* Measure every pill and total the module's width (pills plus the gaps
 * between them). Sizing happens before positioning because the centre slot
 * cannot be placed until its own width is known. */
static void bar_module_measure(BarModule *mod, int32_t height, float scale) {
	int32_t total = 0;
	for (int32_t i = 0; i < mod->npills; i++) {
		BarPill *p = &mod->pills[i];
		if (!p->used)
			continue;
		p->width =
			p->fixed_width > 0
				? p->fixed_width
				: asteroidz_tab_bar_node_measure_width(p->node, p->text,
													   height);
		if (config.bar_pill_min_width > 0 &&
			p->width < config.bar_pill_min_width)
			p->width = config.bar_pill_min_width;
		asteroidz_tab_bar_node_set_size(p->node, p->width, height);
		asteroidz_tab_bar_node_update(p->node, p->text, scale);
		total += p->width;
		if (i + 1 < mod->npills)
			total += config.bar_spacing;
	}
	mod->width = total;
	(void)scale;
}

/* Draw (or hide) the backdrop for one slot. `x`..`x+w` is the extent of the
 * pills it contains; the panel is that grown by panel-padding on each side. */
static void bar_panel_apply(AsteroidzBar *bar, enum bar_slot slot, int32_t x,
							int32_t w, int32_t y, int32_t h) {
	BarPanel *panel = &bar->panels[slot];
	bool want = config.bar_panel_enable && w > 0;

	if (!want) {
		if (panel->blur)
			wlr_scene_node_set_enabled(&panel->blur->node, false);
		if (panel->bg)
			wlr_scene_node_set_enabled(&panel->bg->node, false);
		if (panel->shadow)
			wlr_scene_node_set_enabled(&panel->shadow->node, false);
		return;
	}

	/* Horizontal padding only, like the `padding: 0 6px` on the waybar groups
	 * this mirrors: the panel is exactly as tall as the bar strip, and the gap
	 * to the screen edge comes from margin.y alone. Padding vertically too
	 * grew the panel back up over that margin and left it flush against the
	 * top of the screen. */
	int32_t pad = config.bar_panel_padding;
	int32_t px = x - pad, py = y;
	int32_t pw = w + 2 * pad, ph = h;
	int32_t radius = config.bar_panel_radius;

	/* Blur goes in first so the tint composites over it, matching the
	 * overview strip. Only meaningful when the compositor's blur is on at
	 * all -- creating the node otherwise costs a pass for nothing. */
	if (config.bar_panel_blur && config.blur) {
		if (!panel->blur)
			panel->blur = wlr_scene_blur_create(bar->panel_tree, pw, ph);
		if (panel->blur) {
			wlr_scene_blur_set_size(panel->blur, pw, ph);
			wlr_scene_blur_set_corner_radius(panel->blur, radius);
			wlr_scene_node_set_position(&panel->blur->node, px, py);
			wlr_scene_node_set_enabled(&panel->blur->node, true);
		}
	} else if (panel->blur) {
		wlr_scene_node_set_enabled(&panel->blur->node, false);
	}

	if (config.bar_panel_shadow && config.shadows) {
		if (!panel->shadow)
			panel->shadow = wlr_scene_shadow_create(
				bar->panel_tree, pw, ph, radius, config.shadows_blur,
				config.shadowscolor);
		if (panel->shadow) {
			wlr_scene_shadow_set_size(panel->shadow, pw, ph);
			wlr_scene_shadow_set_corner_radius(panel->shadow, radius);
			wlr_scene_node_set_position(&panel->shadow->node, px, py);
			wlr_scene_node_set_enabled(&panel->shadow->node, true);
			wlr_scene_node_lower_to_bottom(&panel->shadow->node);
		}
	} else if (panel->shadow) {
		wlr_scene_node_set_enabled(&panel->shadow->node, false);
	}

	if (!panel->bg)
		panel->bg = wlr_scene_rect_create(bar->panel_tree, pw, ph,
										  config.bar_panel_color);
	if (panel->bg) {
		wlr_scene_rect_set_size(panel->bg, pw, ph);
		wlr_scene_rect_set_color(panel->bg, config.bar_panel_color);
		wlr_scene_rect_set_corner_radius(panel->bg, radius);
		wlr_scene_node_set_position(&panel->bg->node, px, py);
		wlr_scene_node_set_enabled(&panel->bg->node, true);
		if (panel->blur)
			wlr_scene_node_place_above(&panel->bg->node, &panel->blur->node);
	}
}

static int32_t bar_place_module(BarModule *mod, int32_t x, int32_t y) {
	for (int32_t i = 0; i < mod->npills; i++) {
		BarPill *p = &mod->pills[i];
		if (!p->used)
			continue;
		asteroidz_tab_bar_node_set_position(p->node, x, y);
		asteroidz_tab_bar_node_set_enabled(p->node, true);
		x += p->width + config.bar_spacing;
	}
	return x;
}

/* Recompute the whole strip: refresh every module's content, measure, then
 * place the three slots. Left packs from the left inset, right packs to the
 * right inset, centre is centred on the monitor and only moved aside if it
 * would collide with a neighbour. */
static void bar_layout(Monitor *m) {
	AsteroidzBar *bar = m->bar;
	if (!bar || !bar->tree)
		return;

	int32_t height = config.bar_height;
	float scale = m->wlr_output ? m->wlr_output->scale : 1.0f;
	if (scale <= 0.0f)
		scale = 1.0f;

	int32_t slot_w[BAR_SLOT_COUNT] = {0, 0, 0};
	for (int32_t i = 0; i < bar->nmodules; i++) {
		BarModule *mod = &bar->modules[i];
		bar_module_refresh(mod);
		bar_module_measure(mod, height, scale);
		if (mod->width > 0) {
			if (slot_w[mod->slot] > 0)
				slot_w[mod->slot] += config.bar_spacing;
			slot_w[mod->slot] += mod->width;
		}
	}

	/* The margin is measured to the PANEL edge, not to the first pill, so the
	 * pill row starts one panel-padding further in. Without this the outer
	 * panels overhung the margin by exactly that padding. */
	int32_t pad = config.bar_panel_enable ? config.bar_panel_padding : 0;
	int32_t inset = config.bar_margin_x + pad;
	int32_t left = m->m.x + inset;
	int32_t right = m->m.x + m->m.width - inset;
	int32_t y = config.bar_position_bottom
					? m->m.y + m->m.height - height - config.bar_margin_y
					: m->m.y + config.bar_margin_y;

	bar->box.x = m->m.x;
	bar->box.width = m->m.width;
	bar->box.y = y;
	bar->box.height = height;

	int32_t cursor[BAR_SLOT_COUNT];
	cursor[BAR_SLOT_LEFT] = left;
	cursor[BAR_SLOT_RIGHT] = right - slot_w[BAR_SLOT_RIGHT];
	cursor[BAR_SLOT_CENTER] =
		m->m.x + (m->m.width - slot_w[BAR_SLOT_CENTER]) / 2;

	/* Keep the centre slot from overlapping its neighbours. Overlap is
	 * possible on a narrow output or with a long window title, and two pills
	 * drawn on top of each other is worse than an off-centre strip. */
	int32_t left_end = left + slot_w[BAR_SLOT_LEFT];
	if (slot_w[BAR_SLOT_LEFT] > 0 &&
		cursor[BAR_SLOT_CENTER] < left_end + config.bar_spacing)
		cursor[BAR_SLOT_CENTER] = left_end + config.bar_spacing;
	if (slot_w[BAR_SLOT_RIGHT] > 0 &&
		cursor[BAR_SLOT_CENTER] + slot_w[BAR_SLOT_CENTER] >
			cursor[BAR_SLOT_RIGHT] - config.bar_spacing)
		cursor[BAR_SLOT_CENTER] = cursor[BAR_SLOT_RIGHT] -
								  config.bar_spacing - slot_w[BAR_SLOT_CENTER];

	int32_t slot_start[BAR_SLOT_COUNT];
	for (int32_t s = 0; s < BAR_SLOT_COUNT; s++)
		slot_start[s] = cursor[s];

	for (int32_t i = 0; i < bar->nmodules; i++) {
		BarModule *mod = &bar->modules[i];
		if (mod->width <= 0)
			continue;
		cursor[mod->slot] =
			bar_place_module(mod, cursor[mod->slot], y);
	}

	/* One backdrop per non-empty slot. The bar itself paints nothing, so
	 * these three panels are the whole visible surface. bar_place_module
	 * leaves a trailing inter-pill gap on the cursor, so subtract it back off
	 * rather than padding the panel by it. */
	for (int32_t s = 0; s < BAR_SLOT_COUNT; s++) {
		int32_t w = slot_w[s];
		bar_panel_apply(bar, (enum bar_slot)s, slot_start[s], w, y, height);
	}
}

/* ─── space reservation ───────────────────────────────────────────────────── */

/* Shrink `usable` by the bar's footprint. Called from `arrangelayers` before
 * any layer surface is arranged, so exclusive-zone clients stack below the
 * native bar instead of overlapping it. */
static void bar_reserve(Monitor *m, struct wlr_box *usable) {
	if (!config.bar_enable || !m || !m->bar)
		return;
	int32_t take = config.bar_height + 2 * config.bar_margin_y;
	if (take <= 0 || take >= usable->height)
		return;
	if (config.bar_position_bottom) {
		usable->height -= take;
	} else {
		usable->y += take;
		usable->height -= take;
	}
}

/* ─── refresh entry points ────────────────────────────────────────────────── */

/* FNV-1a over the bar's visible inputs. */
static void bar_hash(uint64_t *h, const void *data, size_t len) {
	const uint8_t *p = data;
	for (size_t i = 0; i < len; i++) {
		*h ^= p[i];
		*h *= 1099511628211ULL;
	}
}

static void bar_hash_str(uint64_t *h, const char *s) {
	bar_hash(h, s ? s : "", s ? strlen(s) : 0);
}

static uint64_t bar_digest(Monitor *m) {
	uint64_t h = 1469598103934665603ULL;
	uint32_t sel = m->tagset[m->seltags] & TAGMASK;
	uint32_t occ = bar_occupied_tags(m);
	uint32_t urg = bar_urgent_tags(m);
	bar_hash(&h, &sel, sizeof(sel));
	bar_hash(&h, &occ, sizeof(occ));
	bar_hash(&h, &urg, sizeof(urg));
	/* geometry: a mode/scale/position change must force a relayout */
	bar_hash(&h, &m->m, sizeof(m->m));
	float scale = m->wlr_output ? m->wlr_output->scale : 1.0f;
	bar_hash(&h, &scale, sizeof(scale));

	for (int32_t i = 0; i < m->bar->nmodules; i++) {
		switch (m->bar->modules[i].kind) {
		case BAR_MODULE_TITLE: {
			Client *c = m->sel;
			bool vis = c && VISIBLEON(c, m);
			bar_hash(&h, &vis, sizeof(vis));
			if (vis) {
				bar_hash_str(&h, client_get_title(c));
				bar_hash_str(&h, c->icon_name ? c->icon_name
											  : client_get_appid(c));
			}
			break;
		}
		case BAR_MODULE_LAYOUT:
			if (m->pertag && m->pertag->ltidxs[m->pertag->curtag])
				bar_hash_str(&h, m->pertag->ltidxs[m->pertag->curtag]->symbol);
			break;
		case BAR_MODULE_IDLE:
			bar_hash(&h, &idle_inhibit_manual, sizeof(idle_inhibit_manual));
			break;
		case BAR_MODULE_CPU:
			bar_hash(&h, &bar_metrics.cpu_pct, sizeof(bar_metrics.cpu_pct));
			break;
		case BAR_MODULE_MEMORY:
			bar_hash(&h, &bar_metrics.mem_pct, sizeof(bar_metrics.mem_pct));
			break;
		case BAR_MODULE_NETWORK: {
			/* hash the FORMATTED rates, not the raw doubles: the pill only
			 * redraws when the displayed string changes, and the rate jitters
			 * continuously while the text does not. */
			char rx[16], tx[16];
			bar_fmt_rate(bar_metrics.rx_rate, rx, sizeof(rx));
			bar_fmt_rate(bar_metrics.tx_rate, tx, sizeof(tx));
			bar_hash_str(&h, rx);
			bar_hash_str(&h, tx);
			bar_hash_str(&h, bar_metrics.iface);
			bar_hash(&h, &bar_metrics.link_up, sizeof(bar_metrics.link_up));
			break;
		}
		case BAR_MODULE_CLOCK: {
			char buf[BAR_TEXT_MAX];
			time_t now = time(NULL);
			struct tm tm;
			localtime_r(&now, &tm);
			if (strftime(buf, sizeof(buf), config.bar_clock_format, &tm) == 0)
				buf[0] = '\0';
			bar_hash_str(&h, buf);
			break;
		}
		case BAR_MODULE_TAGS: {
			bar_hash(&h, &config.bar_show_all_tags,
					 sizeof(config.bar_show_all_tags));
			/* custom tag names can change without any mask changing */
			for (uint32_t t = 1; t <= LENGTH(tags); t++) {
				char name[64];
				tag_display_name(m, t, name, sizeof(name));
				bar_hash_str(&h, name);
			}
			break;
		}
		default:
			break;
		}
	}
	return h;
}

static void bar_update(Monitor *m) {
	if (!m || !m->bar || !m->bar->tree)
		return;
	if (!config.bar_enable) {
		wlr_scene_node_set_enabled(&m->bar->tree->node, false);
		return;
	}
	wlr_scene_node_set_enabled(&m->bar->tree->node, true);

	uint64_t digest = bar_digest(m);
	if (m->bar->digest_valid && digest == m->bar->digest)
		return;
	m->bar->digest = digest;
	m->bar->digest_valid = true;

	bar_layout(m);
}

static void bar_update_all(void) {
	Monitor *m = NULL;
	wl_list_for_each(m, &mons, link) {
		if (m->wlr_output && m->wlr_output->enabled)
			bar_update(m);
	}
}

/* ─── clock tick ──────────────────────────────────────────────────────────── */

/* Milliseconds until the next boundary the configured clock format can
 * actually show. A format without %S only changes once a minute, so waking
 * every second to redraw an identical string would be pure waste -- across
 * two monitors that is the same per-frame recomposite cost that made the
 * waybar media module expensive. Align to the boundary rather than sleeping a
 * fixed interval so the displayed minute flips on time. */
static int32_t bar_clock_next_delay_ms(void) {
	bool seconds = strstr(config.bar_clock_format, "%S") != NULL ||
				   strstr(config.bar_clock_format, "%T") != NULL ||
				   strstr(config.bar_clock_format, "%r") != NULL;
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	int32_t ms_into_second = (int32_t)(ts.tv_nsec / 1000000);
	if (seconds)
		return 1000 - ms_into_second;
	time_t now = ts.tv_sec;
	struct tm tm;
	localtime_r(&now, &tm);
	return (60 - tm.tm_sec) * 1000 - ms_into_second;
}

static int bar_clock_tick(void *data) {
	(void)data;
	bar_update_all();
	if (bar_clock_timer)
		wl_event_source_timer_update(bar_clock_timer,
									 bar_clock_next_delay_ms());
	return 0;
}

static int bar_metrics_tick(void *data) {
	(void)data;
	bar_metrics_sample();
	bar_update_all();
	if (bar_metrics_timer)
		wl_event_source_timer_update(bar_metrics_timer,
									 config.bar_interval * 1000);
	return 0;
}

/* Arm or disarm the shared clock and metrics timers. One of each for every
 * monitor: metrics are machine-wide, and the per-pill dirty check in the
 * tab-bar node means a tick that produces the same string costs no redraw and
 * no damage. */
static void bar_clock_sync(void) {
	bool want = false, want_metrics = false;
	Monitor *m = NULL;
	if (config.bar_enable) {
		wl_list_for_each(m, &mons, link) {
			if (!m->bar)
				continue;
			for (int32_t i = 0; i < m->bar->nmodules; i++) {
				if (m->bar->modules[i].kind == BAR_MODULE_CLOCK)
					want = true;
				if (bar_kind_is_metric(m->bar->modules[i].kind))
					want_metrics = true;
			}
		}
	}

	if (want_metrics && !bar_metrics_timer) {
		/* prime immediately: cpu% and throughput are both deltas, so the
		 * first displayed value would otherwise be a meaningless zero until
		 * the second tick */
		bar_metrics_sample();
		bar_metrics_timer =
			wl_event_loop_add_timer(event_loop, bar_metrics_tick, NULL);
		if (bar_metrics_timer)
			wl_event_source_timer_update(bar_metrics_timer,
										 config.bar_interval * 1000);
	} else if (!want_metrics && bar_metrics_timer) {
		wl_event_source_remove(bar_metrics_timer);
		bar_metrics_timer = NULL;
	}
	if (want && !bar_clock_timer) {
		bar_clock_timer =
			wl_event_loop_add_timer(event_loop, bar_clock_tick, NULL);
		if (bar_clock_timer)
			wl_event_source_timer_update(bar_clock_timer,
										 bar_clock_next_delay_ms());
	} else if (!want && bar_clock_timer) {
		wl_event_source_remove(bar_clock_timer);
		bar_clock_timer = NULL;
	}
}

/* ─── construction ────────────────────────────────────────────────────────── */

/* Parse one comma-separated module list ("tags,layout") into `bar`. Unknown
 * names are reported and skipped rather than being fatal: a config naming a
 * module this build does not have yet should degrade, not refuse to start. */
static void bar_add_modules(AsteroidzBar *bar, const char *list,
							enum bar_slot slot) {
	if (!list || !*list)
		return;
	char buf[512];
	snprintf(buf, sizeof(buf), "%s", list);
	char *save = NULL;
	for (char *tok = strtok_r(buf, ",", &save); tok;
		 tok = strtok_r(NULL, ",", &save)) {
		while (*tok == ' ')
			tok++;
		char *end = tok + strlen(tok);
		while (end > tok && end[-1] == ' ')
			*--end = '\0';
		if (!*tok)
			continue;
		enum bar_module_kind kind = bar_module_kind_from_name(tok);
		if (kind == BAR_MODULE_NONE) {
			fprintf(stderr,
					"\033[1m\033[33m[WARN]:\033[0m unknown bar module '%s'\n",
					tok);
			continue;
		}
		if (bar->nmodules >= BAR_MAX_MODULES) {
			fprintf(stderr,
					"\033[1m\033[33m[WARN]:\033[0m too many bar modules "
					"(max %d), ignoring '%s'\n",
					BAR_MAX_MODULES, tok);
			return;
		}
		BarModule *mod = &bar->modules[bar->nmodules++];
		mod->kind = kind;
		mod->slot = slot;
		mod->mon = bar->mon;
	}
}

static void bar_destroy(Monitor *m) {
	if (!m || !m->bar)
		return;
	AsteroidzBar *bar = m->bar;
	for (int32_t i = 0; i < bar->nmodules; i++)
		for (int32_t j = 0; j < BAR_MAX_PILLS; j++)
			bar_pill_release(&bar->modules[i].pills[j]);
	/* the panel nodes are children of bar->tree, so destroying it takes them
	 * with it -- but null the pointers so a stale bar can't be reused */
	for (int32_t s = 0; s < BAR_SLOT_COUNT; s++)
		bar->panels[s] = (BarPanel){0};
	if (bar->tree)
		wlr_scene_node_destroy(&bar->tree->node);
	free(bar);
	m->bar = NULL;
	bar_clock_sync();
}

static void bar_create(Monitor *m) {
	if (!m || m->bar)
		return;
	AsteroidzBar *bar = ecalloc(1, sizeof(*bar));
	bar->mon = m;
	/* LyrTop: above tiled and floating windows, below LyrFS so a fullscreen
	 * window still covers the bar, matching what a layer-shell bar on the
	 * "top" layer does today. */
	bar->tree = wlr_scene_tree_create(layers[LyrTop]);
	if (!bar->tree) {
		free(bar);
		return;
	}
	/* order matters: panel_tree is created first so it sits below pill_tree */
	bar->panel_tree = wlr_scene_tree_create(bar->tree);
	bar->pill_tree = wlr_scene_tree_create(bar->tree);
	if (!bar->panel_tree || !bar->pill_tree) {
		wlr_scene_node_destroy(&bar->tree->node);
		free(bar);
		return;
	}
	m->bar = bar;
	bar_add_modules(bar, config.bar_modules_left, BAR_SLOT_LEFT);
	bar_add_modules(bar, config.bar_modules_center, BAR_SLOT_CENTER);
	bar_add_modules(bar, config.bar_modules_right, BAR_SLOT_RIGHT);
	wlr_scene_node_set_enabled(&bar->tree->node, config.bar_enable);
	bar_clock_sync();
}

/* Rebuild every bar from scratch. Used on config reload, where the module
 * lists themselves may have changed -- a plain bar_update only refreshes
 * content, not which modules exist. */
static void bar_reconfigure_all(void) {
	Monitor *m = NULL;
	wl_list_for_each(m, &mons, link) {
		bar_destroy(m);
		bar_create(m);
	}
	wl_list_for_each(m, &mons, link) {
		if (m->wlr_output && m->wlr_output->enabled)
			arrangelayers(m);
	}
	bar_update_all();
}

/* ─── input ───────────────────────────────────────────────────────────────── */

/* Dispatch a click on a bar pill. Returns true when the click was consumed.
 * `handle_buttonpress` resolves the pill from the scene node's data tag, so
 * stacking is respected for free: a pill covered by something above it is not
 * reachable. */
static bool bar_handle_node_click(AsteroidzNodeData *hit, uint32_t button) {
	BarPill *p = hit ? (BarPill *)hit->node_data : NULL;
	if (!p || !p->used || !p->module)
		return false;
	Monitor *m = p->module->mon;
	switch (p->module->kind) {
	case BAR_MODULE_TAGS:
		if (button == BTN_LEFT && p->arg >= 1 && p->arg <= LENGTH(tags)) {
			/* `view` acts on selmon, so clicking a tag on an unfocused
			 * output has to move focus there first -- otherwise the click
			 * would switch tags on the wrong monitor. Same minimal switch
			 * focusmon() performs, without the directional lookup. */
			if (m && m != selmon && m->wlr_output && m->wlr_output->enabled) {
				selmon = m;
				Client *top = focustop(selmon);
				if (top)
					focusclient(top, 1);
			}
			view(&(Arg){.ui = 1u << (p->arg - 1)}, true);
			return true;
		}
		break;
	case BAR_MODULE_TITLE:
		if (button == BTN_LEFT && m && m->sel) {
			focusclient(m->sel, 1);
			return true;
		}
		break;
	case BAR_MODULE_IDLE:
		if (button == BTN_LEFT) {
			toggle_idle_inhibit(&(Arg){.i = -1});
			return true;
		}
		break;
	case BAR_MODULE_LAYOUT:
		if (button == BTN_LEFT) {
			/* switch_layout cycles config.circle_layout on selmon, so point
			 * selmon at the clicked pill's output first -- same reason the
			 * tags click does. */
			if (m && m != selmon && m->wlr_output && m->wlr_output->enabled)
				selmon = m;
			switch_layout(NULL);
			return true;
		}
		break;
	default:
		break;
	}
	return false;
}

#else /* !ASTEROIDZ_NATIVE_BAR */

/* Built without the native bar: the call sites in asteroidz.c stay
 * unconditional and these compile to nothing, so there is no #ifdef scattered
 * through the compositor and nothing of the bar reaches the binary. */
static void bar_create(Monitor *m) { (void)m; }
static void bar_destroy(Monitor *m) { (void)m; }
static void bar_update(Monitor *m) { (void)m; }
static void bar_update_all(void) {}
static void bar_reconfigure_all(void) {}
static void bar_reserve(Monitor *m, struct wlr_box *usable) {
	(void)m;
	(void)usable;
}
/* Nothing ever tags a scene node ASTEROIDZ_BAR_NODE in this build, so the
 * caller's branch is dead -- it just has to compile. */
static bool bar_handle_node_click(AsteroidzNodeData *hit,
										 uint32_t button) {
	(void)hit;
	(void)button;
	return false;
}

#endif /* ASTEROIDZ_NATIVE_BAR */

#endif /* ASTEROIDZ_BAR_H */
