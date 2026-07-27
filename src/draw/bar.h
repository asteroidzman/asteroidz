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
/* Total across ALL THREE sections, not per section.
 *
 * Was 12, which is fewer than the number of module kinds that exist -- a bar
 * listing each module once could not be expressed, and the overflow is a
 * stderr warning nobody reads: a real config asking for 17 silently lost its
 * last five, tray included. Sized above the kind count with room for the
 * handful that legitimately repeat, so hitting this again means a config
 * doing something genuinely unusual.
 *
 * Not free: a BarModule embeds BAR_MAX_PILLS pills, each carrying a
 * BAR_TEXT_MAX text buffer, and one of these arrays exists per monitor. */
#define BAR_MAX_MODULES 24
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
	BAR_MODULE_WEATHER,
	BAR_MODULE_MEDIA,
	BAR_MODULE_TRAY,
	BAR_MODULE_VOLUME,
	BAR_MODULE_NOTIFY,
	BAR_MODULE_MEDICATION,
	BAR_MODULE_DISCORD,
	BAR_MODULE_VPN,
	BAR_MODULE_DISPLAY,
	/* not a module: the count, so the cap below can be checked against it */
	BAR_MODULE_KIND_COUNT,
};

/* A config listing every module once must fit, or the parse loop silently
 * drops the tail -- which is exactly what happened when this cap sat at 12
 * while seventeen kinds existed. Checked by the compiler rather than left to
 * whoever adds the eighteenth module to remember. */
_Static_assert(BAR_MAX_MODULES >= BAR_MODULE_KIND_COUNT - 1,
			   "BAR_MAX_MODULES must fit one of every bar module kind");

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
	/* Pinned width is a preference, not a requirement: a pill carrying
	 * ellipsisable text can give width back when the slots do not fit. */
	bool flexible;
	/* Mirrors the node's text alignment, so the layout knows which side a
	 * pinned pill's unused width ends up on. */
	bool align_left;
	/* How much of this pill's width is reserve the current content does not
	 * use, split by where that reserve actually falls. A pinned pill is as
	 * wide as its WIDEST possible content -- the clock is probed across all
	 * twelve months -- so most of the time it carries slack that is invisible
	 * but still occupies the slot. The panel needs to know, or it hugs the
	 * reserve rather than the content and the section looks lopsided. */
	int32_t slack_lead, slack_trail;
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
	if (strcmp(name, "weather") == 0)
		return BAR_MODULE_WEATHER;
	if (strcmp(name, "media") == 0)
		return BAR_MODULE_MEDIA;
	if (strcmp(name, "tray") == 0 || strcmp(name, "systray") == 0)
		return BAR_MODULE_TRAY;
	if (strcmp(name, "volume") == 0 || strcmp(name, "vol") == 0)
		return BAR_MODULE_VOLUME;
	if (strcmp(name, "notifications") == 0 || strcmp(name, "notify") == 0)
		return BAR_MODULE_NOTIFY;
	if (strcmp(name, "medication") == 0 || strcmp(name, "meds") == 0)
		return BAR_MODULE_MEDICATION;
	if (strcmp(name, "discord") == 0 || strcmp(name, "discord-voice") == 0)
		return BAR_MODULE_DISCORD;
	if (strcmp(name, "vpn") == 0 || strcmp(name, "nordvpn") == 0)
		return BAR_MODULE_VPN;
	if (strcmp(name, "display") == 0 || strcmp(name, "monitors") == 0)
		return BAR_MODULE_DISPLAY;
	return BAR_MODULE_NONE;
}

/* Which part of the media module a pill is, carried in BarPill.arg. The
 * transport buttons and the track are separate pills because hit testing is
 * per node. */
enum {
	BAR_MEDIA_TITLE = 0,
	BAR_MEDIA_PREV,
	BAR_MEDIA_PLAYPAUSE,
	BAR_MEDIA_NEXT,
};

/* Chip modules draw a permanent background of their own -- they are discrete
 * rounded tiles, like the waybar workspace plugin's pills -- so they take the
 * roomier `tag-padding` and a real gap to their neighbours. Everything else is
 * text on the shared panel and gets neither.
 *
 * Keyed on the MODULE rather than on the current look on purpose: a metric
 * crossing its threshold takes an urgent fill, and if that also changed its
 * padding and gap the whole right-hand section would shift sideways every time
 * the CPU spiked. Geometry has to be a property of what a module is, not of
 * what it happens to be showing. */
static bool bar_kind_is_metric(enum bar_module_kind k) {
	return k == BAR_MODULE_CPU || k == BAR_MODULE_MEMORY ||
		   k == BAR_MODULE_NETWORK;
}

static bool bar_kind_is_chips(enum bar_module_kind k) {
	return k == BAR_MODULE_TAGS || k == BAR_MODULE_LAYOUT;
}

static bool bar_pill_is_chip(const BarPill *p) {
	return p && p->module && bar_kind_is_chips(p->module->kind);
}

/* A bare glyph on the panel: artwork, no label, no fill (cpu, memory). These
 * are laid out as one run with an exact gap between them rather than with
 * padding on each -- padding is symmetric, so it can only ever produce an even
 * gap, and it also pads the ends of the run against the panel edge. */
static bool bar_pill_is_icon_only(const BarPill *p) {
	if (!p || !p->module || bar_pill_is_chip(p) || p->text[0] != '\0')
		return false;
	/* Every module that draws artwork with no label. The text check above
	 * already excludes the ones that merely HAPPEN to be label-less right now
	 * (notify with no unread count, medication with no dose), so this list is
	 * about which pills are artwork-first, not which are momentarily empty. */
	return bar_kind_is_metric(p->module->kind) ||
		   p->module->kind == BAR_MODULE_TRAY ||
		   p->module->kind == BAR_MODULE_VPN ||
		   p->module->kind == BAR_MODULE_IDLE ||
		   p->module->kind == BAR_MODULE_NOTIFY ||
		   p->module->kind == BAR_MODULE_DISPLAY;
}

static int32_t bar_pill_padding_x(const BarPill *p) {
	if (bar_pill_is_chip(p))
		return config.bar_tag_padding;
	if (bar_pill_is_icon_only(p))
		return 0;
	return config.bar_pill_padding;
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
/* Never more than five characters, whatever the rate.
 *
 * The pill is width-pinned so it cannot reflow as the numbers change, and the
 * pin has to cover the widest string the formatter can produce. The old
 * formatter could emit "999.9M" (six), which meant reserving for a rendering
 * an ordinary link never reaches -- the reserve sat there as dead space, and
 * once the tray moved to the far right that space landed between the
 * throughput and the tray icons. Capping the format instead makes the reserve
 * nearly exact.
 *
 * Precision drops as the magnitude rises, which is the right trade anyway:
 * at 1.5M/s the tenth is informative, at 125M/s it is noise. */
static void bar_fmt_rate(double bytes_per_sec, char *out, size_t len) {
	double mb = bytes_per_sec / (1024.0 * 1024.0);
	if (mb >= 100.0)
		snprintf(out, len, "%.0fM", mb); /* "125M" */
	else if (mb >= 1.0)
		snprintf(out, len, "%.1fM", mb); /* "99.9M" */
	else if (bytes_per_sec >= 1024.0)
		snprintf(out, len, "%.0fK", bytes_per_sec / 1024.0); /* "1023K" */
	else
		snprintf(out, len, "%.0f", bytes_per_sec); /* "1023" */
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
	if (p->used) {
		/* Re-assert the padding before the caller measures anything: a refresh
		 * probes its widest content through this very node, and bar_pill_style
		 * (which runs after those probes) calls apply_config, which resets the
		 * padding to the titlebar theme's. Without this the first frame after
		 * every style pass measured at one padding and drew at another. */
		asteroidz_tab_bar_node_set_padding(p->node, bar_pill_padding_x(p),
										   config.theme.padding_y);
		return p;
	}

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
	asteroidz_tab_bar_node_set_padding(p->node, bar_pill_padding_x(p),
									   config.theme.padding_y);
	asteroidz_tab_bar_node_set_shadow(p->node, config.shadows,
									  config.shadows_blur,
									  (int32_t)config.shadows_position_y,
									  config.shadowscolor);
	return p;
}

/* Pill styling, mirroring the grouped-mode waybar workspace CSS this replaces:
 *   .ws-pill.focused   accent fill              (theme focus pair)
 *   .ws-pill.occupied  rgba(0,0,0,0.44)         sunken, darker than the panel
 *   .ws-pill.empty     rgba(255,255,255,0.06)   faint fill, label at 35% alpha
 *   .ws-layout         rgba(0,0,0,0.30)         same family, one step lighter
 * Without a backing panel there is nothing to sink into, so every pill falls
 * back to the theme's resting pair instead. */
enum bar_pill_look {
	BAR_LOOK_FLAT = 0, /* no fill of its own; the panel is the surface */
	BAR_LOOK_ACTIVE,   /* selected / focused */
	BAR_LOOK_OCCUPIED, /* holds windows */
	BAR_LOOK_EMPTY,    /* padding tag */
	BAR_LOOK_SUNKEN,   /* layout toggler */
	BAR_LOOK_URGENT,
};

/* Height of a pill: the strip minus its inset top and bottom, so a filled chip
 * sits inside the panel rather than spanning it edge to edge. Floored well
 * short of zero so a hand-set inset cannot collapse the row. */
static int32_t bar_pill_height(void) {
	int32_t h = config.bar_height - 2 * config.bar_pill_inset;
	return h < 8 ? (config.bar_height < 8 ? config.bar_height : 8) : h;
}

/* How far a pill's own edge sits from the thing you actually SEE in it.
 *
 * A chip draws a background, so its box edge is visible and the answer is
 * zero -- the gap between two chips is a gap between two tiles. Everything
 * else is flat artwork or text on the shared panel, where the pill's
 * horizontal padding is invisible slack that still pushes the neighbour away.
 *
 * Keyed on the pill's KIND, never on the look it currently wears: a metric
 * crossing its urgent threshold takes a fill, and if that also changed the
 * geometry the whole section would shift sideways every time the CPU spiked. */
static int32_t bar_pill_ink_inset(const BarPill *p) {
	if (bar_pill_is_chip(p))
		return 0;
	return bar_pill_padding_x(p);
}

/* Never let two pills actually touch, even where their own padding already
 * exceeds the configured separation. Two pounded-together backgrounds read as
 * one wider pill, and the compensation below can ask for exactly that. */
#define BAR_GAP_MIN 2

/* Separation between two adjacent pills or modules, measured the way you see
 * it: ink to ink, not box to box.
 *
 * A pill's own padding is part of what sits between it and its neighbour, and
 * that padding differs by kind -- 0 for icon-only artwork, `pill-padding` for
 * anything with a label. A constant BOX gap therefore renders as an
 * inconsistent VISIBLE one: 12px between two icons, 18 between an icon and a
 * label, 24 between two labels. That is the "bigger gap between sound,
 * notifications and display" -- the separation was constant all along, the
 * content inside it was not.
 *
 * So `base` is the separation and each side's inset comes off it. Chips are
 * exempt (inset 0): their backgrounds are the edges you see. */
static int32_t bar_gap_between(const BarPill *a, const BarPill *b,
							   int32_t base) {
	if (!a || !b)
		return 0;
	int32_t gap = base - bar_pill_ink_inset(a) - bar_pill_ink_inset(b);
	return gap < BAR_GAP_MIN ? BAR_GAP_MIN : gap;
}

/* Gap between two pills of the SAME module (tray items, tag chips). */
static int32_t bar_pill_gap(const BarPill *a, const BarPill *b) {
	if (!a || !b)
		return 0;
	/* two tiles: box to box, at the chip spacing they have always used */
	if (bar_pill_is_chip(a) || bar_pill_is_chip(b))
		return config.bar_spacing;
	return bar_gap_between(a, b, config.bar_module_spacing);
}

/* A foreground that can actually be read on `bg`.
 *
 * The urgent look pairs the theme's foreground with the theme's urgent colour,
 * and those two are set independently: a pale salmon urgent with a white
 * foreground is white-on-pink, which is exactly the state you are meant to be
 * able to read at a glance. The urgent colour is chosen for how it reads
 * against the BAR, so it cannot also be constrained to contrast with the
 * label -- the label has to give.
 *
 * Rec. 709 relative luminance against the midpoint, which is the same rule a
 * theme editor applies by eye. Near-black rather than black, and the theme's
 * own foreground when that is the readable choice, so an urgent pill still
 * looks like it belongs to the theme. */
static void bar_readable_fg(const float bg[4], float out[4]) {
	float lum = 0.2126f * bg[0] + 0.7152f * bg[1] + 0.0722f * bg[2];
	if (lum < 0.5f) {
		/* dark fill: the theme's own foreground is almost certainly light */
		memcpy(out, config.theme.fg_color, sizeof(float) * 4);
		return;
	}
	out[0] = out[1] = out[2] = 0.08f;
	out[3] = config.theme.fg_color[3];
}

static void bar_pill_style(BarPill *p, enum bar_pill_look look) {
	static const float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
	static const float occupied_bg[4] = {0.0f, 0.0f, 0.0f, 0.44f};
	static const float sunken_bg[4] = {0.0f, 0.0f, 0.0f, 0.30f};
	static const float empty_bg[4] = {1.0f, 1.0f, 1.0f, 0.06f};

	asteroidz_tab_bar_node_apply_config(p->node, &config.theme);
	/* apply_config resets the padding to the shared theme's, which is sized for
	 * titlebars; put the bar's own back on top of it */
	asteroidz_tab_bar_node_set_padding(p->node, bar_pill_padding_x(p),
									   config.theme.padding_y);
	asteroidz_tab_bar_node_set_focus(p->node, look == BAR_LOOK_ACTIVE);
	if (look == BAR_LOOK_ACTIVE)
		return;
	if (look == BAR_LOOK_URGENT) {
		float fg[4];
		bar_readable_fg(config.theme.urgent_color, fg);
		asteroidz_tab_bar_node_set_colors(p->node, fg,
										  config.theme.urgent_color);
		return;
	}
	if (!config.bar_panel_enable)
		return; /* standalone pills keep the theme's resting pair */

	float fg[4];
	memcpy(fg, config.theme.fg_color, sizeof(fg));
	const float *bg = clear;
	switch (look) {
	case BAR_LOOK_OCCUPIED:
		bg = occupied_bg;
		break;
	case BAR_LOOK_SUNKEN:
		bg = sunken_bg;
		break;
	case BAR_LOOK_EMPTY:
		bg = empty_bg;
		fg[3] *= 0.35f;
		break;
	default:
		break;
	}
	asteroidz_tab_bar_node_set_colors(p->node, fg, bg);
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
 * an incomplete asset install degrades to text.
 *
 * `bar_icon_dir` is a colon-separated search path because the plugins do not
 * share a prefix: waybar-sysinfo installs to ~/.local/share from its Makefile
 * while the packaged ones land in /usr/share. First readable candidate wins;
 * with no hit at all the LAST candidate is returned, which then resolves to no
 * icon exactly as a single missing path used to. */
static void bar_icon_path(char *out, size_t len, const char *rel) {
	const char *p = config.bar_icon_dir;
	if (!p || !*p) {
		snprintf(out, len, "%s", rel);
		return;
	}
	for (;;) {
		const char *sep = strchr(p, ':');
		int32_t dirlen = sep ? (int32_t)(sep - p) : (int32_t)strlen(p);
		snprintf(out, len, "%.*s/%s", dirlen, p, rel);
		if (access(out, R_OK) == 0 || !sep)
			return;
		p = sep + 1;
	}
}

/* Width an icon-only pill needs. The icon is drawn at the text area's height,
 * so a pill pinned to the bar height is narrower than its own icon once the
 * theme's padding_x comes off, and the artwork renders clipped. */
static int32_t bar_icon_pill_width(BarPill *p, int32_t height) {
	return bar_template_width(p, "", height);
}

/* ─── weather ─────────────────────────────────────────────────────────────── */

/* open-meteo, the same source and the same WMO->artwork mapping the
 * waybar-weather plugin uses, so the pill is indistinguishable from it.
 * Fetched through async_spawn(curl) -- never synchronously: this runs on the
 * compositor's event loop, where a 3-second DNS stall would be a 3-second
 * freeze. */
/* One day of the forecast, as the popover shows it. */
typedef struct {
	char day[12]; /* "Mon", or "Today" for the first */
	int32_t code;
	int32_t tmin, tmax;
	int32_t precip; /* max probability, % */
} BarWeatherDay;

#define BAR_WEATHER_DAYS 7

static struct {
	double lat, lon;
	bool located;
	int32_t temp_c;
	int32_t code;
	bool is_day;
	bool valid;
	bool in_flight;
	/* Everything below is for the popover only: the pill shows a glyph and a
	 * temperature and nothing else. Fetched in the same request, because one
	 * curl that returns a little more costs nothing next to a second one. */
	char city[96];
	int32_t feels_c;
	int32_t humidity_pct;
	int32_t wind_kmh;
	int32_t pressure_hpa;
	int32_t precip_pct;
	char sunrise[8], sunset[8];
	BarWeatherDay days[BAR_WEATHER_DAYS];
	int32_t ndays;
} bar_weather;

static struct wl_event_source *bar_weather_timer = NULL;

static const char *bar_wmo_icon(int32_t code, bool day) {
	switch (code) {
	case 0:
	case 1:
		return day ? "sunny.svg" : "night.svg";
	case 2:
		return day ? "pcloudy.svg" : "npcloudy.svg";
	case 3:
		return "cloud.svg";
	case 45:
	case 48:
		return "fog.svg";
	case 65:
	case 67:
	case 82:
		return "pour.svg";
	case 51:
	case 53:
	case 55:
	case 56:
	case 57:
	case 61:
	case 63:
	case 66:
	case 80:
	case 81:
		return "rain.svg";
	case 75:
	case 86:
		return "heavy-snow.svg";
	case 71:
	case 73:
	case 77:
	case 85:
		return "snow.svg";
	case 95:
	case 96:
	case 99:
		return "tstorm.svg";
	default:
		return "cloud.svg";
	}
}

/* Plain-language WMO code, for the weather pill's hover and the forecast
 * popover. The pill shows the temperature and an icon; "which of the four rain
 * icons is that" is exactly the question a glyph cannot answer. Kept beside
 * the icon map so the two cannot drift apart, and defined here rather than in
 * bar-tooltip.h because the popover is included first and needs it too. */
static const char *bar_wmo_text(int32_t code) {
	switch (code) {
	case 0: return "clear";
	case 1: return "mainly clear";
	case 2: return "partly cloudy";
	case 3: return "overcast";
	case 45: case 48: return "fog";
	case 51: case 53: case 55: return "drizzle";
	case 56: case 57: return "freezing drizzle";
	case 61: case 63: return "rain";
	case 65: return "heavy rain";
	case 66: case 67: return "freezing rain";
	case 71: case 73: return "snow";
	case 75: return "heavy snow";
	case 77: return "snow grains";
	case 80: case 81: return "showers";
	case 82: return "violent showers";
	case 85: case 86: return "snow showers";
	case 95: return "thunderstorm";
	case 96: case 99: return "thunderstorm with hail";
	default: return "";
	}
}

static void bar_weather_fetch_forecast(void);

/* A rounded integer out of a JSON number, or `fallback` if it is not one.
 * open-meteo omits a field entirely when the model has no value for it. */
static int32_t bar_num(cJSON *o, const char *key, int32_t fallback) {
	cJSON *v = cJSON_GetObjectItem(o, key);
	return cJSON_IsNumber(v) ? (int32_t)lround(v->valuedouble) : fallback;
}

/* "2026-07-26T05:14" -> "05:14". The API returns local time already, so this
 * is a substring, not a conversion. */
static void bar_weather_clock(const char *iso, char *out, size_t outlen) {
	out[0] = '\0';
	const char *t = iso ? strchr(iso, 'T') : NULL;
	if (t && strlen(t) >= 6)
		snprintf(out, outlen, "%.5s", t + 1);
}

/* "2026-07-26" -> "Mon". Uses timegm on a midday UTC timestamp so a date near
 * a DST boundary cannot land on the previous day. */
static void bar_weather_dayname(const char *iso, int32_t index, char *out,
								size_t outlen) {
	if (index == 0) {
		snprintf(out, outlen, "Today");
		return;
	}
	int y = 0, mo = 0, d = 0;
	/* sscanf rather than strptime: strptime needs _XOPEN_SOURCE, and this
	 * parses exactly one fixed shape that the API documents. */
	if (!iso || sscanf(iso, "%4d-%2d-%2d", &y, &mo, &d) != 3) {
		snprintf(out, outlen, "--");
		return;
	}
	struct tm tm = {0};
	tm.tm_year = y - 1900;
	tm.tm_mon = mo - 1;
	tm.tm_mday = d;
	tm.tm_hour = 12;
	time_t at = timegm(&tm);
	struct tm g;
	if (!gmtime_r(&at, &g) || strftime(out, outlen, "%a", &g) == 0)
		snprintf(out, outlen, "--");
}

/* The `daily` block is column-oriented: parallel arrays, one entry per day. */
static void bar_weather_parse_daily(cJSON *daily) {
	bar_weather.ndays = 0;
	if (!daily)
		return;
	cJSON *time = cJSON_GetObjectItem(daily, "time");
	cJSON *tmax = cJSON_GetObjectItem(daily, "temperature_2m_max");
	cJSON *tmin = cJSON_GetObjectItem(daily, "temperature_2m_min");
	cJSON *code = cJSON_GetObjectItem(daily, "weather_code");
	cJSON *prec = cJSON_GetObjectItem(daily, "precipitation_probability_max");
	if (!cJSON_IsArray(time))
		return;

	/* sunrise/sunset are daily arrays too, but only today's is worth showing */
	cJSON *sr = cJSON_GetObjectItem(daily, "sunrise");
	cJSON *ss = cJSON_GetObjectItem(daily, "sunset");
	cJSON *sr0 = sr ? cJSON_GetArrayItem(sr, 0) : NULL;
	cJSON *ss0 = ss ? cJSON_GetArrayItem(ss, 0) : NULL;
	if (cJSON_IsString(sr0))
		bar_weather_clock(sr0->valuestring, bar_weather.sunrise,
						  sizeof(bar_weather.sunrise));
	if (cJSON_IsString(ss0))
		bar_weather_clock(ss0->valuestring, bar_weather.sunset,
						  sizeof(bar_weather.sunset));

	int32_t n = cJSON_GetArraySize(time);
	if (n > BAR_WEATHER_DAYS)
		n = BAR_WEATHER_DAYS;
	for (int32_t i = 0; i < n; i++) {
		BarWeatherDay *d = &bar_weather.days[i];
		cJSON *ti = cJSON_GetArrayItem(time, i);
		bar_weather_dayname(cJSON_IsString(ti) ? ti->valuestring : NULL, i,
							d->day, sizeof(d->day));
		cJSON *v;
		v = tmax ? cJSON_GetArrayItem(tmax, i) : NULL;
		d->tmax = cJSON_IsNumber(v) ? (int32_t)lround(v->valuedouble) : 0;
		v = tmin ? cJSON_GetArrayItem(tmin, i) : NULL;
		d->tmin = cJSON_IsNumber(v) ? (int32_t)lround(v->valuedouble) : 0;
		v = code ? cJSON_GetArrayItem(code, i) : NULL;
		d->code = cJSON_IsNumber(v) ? (int32_t)v->valuedouble : 3;
		v = prec ? cJSON_GetArrayItem(prec, i) : NULL;
		d->precip = cJSON_IsNumber(v) ? (int32_t)lround(v->valuedouble) : -1;
		bar_weather.ndays = i + 1;
	}
}

static void bar_weather_on_forecast(const char *out, size_t len, void *user) {
	(void)user;
	bar_weather.in_flight = false;
	if (!len)
		return;
	cJSON *root = cJSON_Parse(out);
	if (!root)
		return;
	cJSON *cur = cJSON_GetObjectItem(root, "current");
	if (cur) {
		cJSON *t = cJSON_GetObjectItem(cur, "temperature_2m");
		cJSON *c = cJSON_GetObjectItem(cur, "weather_code");
		cJSON *d = cJSON_GetObjectItem(cur, "is_day");
		if (cJSON_IsNumber(t)) {
			bar_weather.temp_c = (int32_t)lround(t->valuedouble);
			bar_weather.code = cJSON_IsNumber(c) ? (int32_t)c->valuedouble : 3;
			bar_weather.is_day = !cJSON_IsNumber(d) || d->valuedouble != 0;
			bar_weather.valid = true;
			bar_weather.feels_c = bar_num(cur, "apparent_temperature",
										  bar_weather.temp_c);
			bar_weather.humidity_pct = bar_num(cur, "relative_humidity_2m", -1);
			bar_weather.wind_kmh = bar_num(cur, "wind_speed_10m", -1);
			bar_weather.pressure_hpa = bar_num(cur, "surface_pressure", -1);
			bar_weather.precip_pct =
				bar_num(cur, "precipitation_probability", -1);
			bar_update_all();
		}
	}
	bar_weather_parse_daily(cJSON_GetObjectItem(root, "daily"));
	cJSON_Delete(root);
}

static void bar_weather_on_geo(const char *out, size_t len, void *user) {
	(void)user;
	bar_weather.in_flight = false;
	if (!len)
		return;
	cJSON *root = cJSON_Parse(out);
	if (!root)
		return;
	/* ip-api.com uses lat/lon; the open-meteo geocoder nests them under
	 * results[0] as latitude/longitude. Accept either shape so a configured
	 * city and IP geolocation share this one parser. */
	cJSON *lat = cJSON_GetObjectItem(root, "lat");
	cJSON *lon = cJSON_GetObjectItem(root, "lon");
	if (!cJSON_IsNumber(lat) || !cJSON_IsNumber(lon)) {
		cJSON *res = cJSON_GetObjectItem(root, "results");
		cJSON *first = res ? cJSON_GetArrayItem(res, 0) : NULL;
		if (first) {
			lat = cJSON_GetObjectItem(first, "latitude");
			lon = cJSON_GetObjectItem(first, "longitude");
		}
	}
	if (cJSON_IsNumber(lat) && cJSON_IsNumber(lon)) {
		bar_weather.lat = lat->valuedouble;
		bar_weather.lon = lon->valuedouble;
		bar_weather.located = true;
		/* the place name, for the popover: ip-api calls it "city", the
		 * open-meteo geocoder calls it "name" on the result it matched */
		cJSON *city = cJSON_GetObjectItem(root, "city");
		if (!cJSON_IsString(city)) {
			cJSON *res = cJSON_GetObjectItem(root, "results");
			cJSON *first = res ? cJSON_GetArrayItem(res, 0) : NULL;
			city = first ? cJSON_GetObjectItem(first, "name") : NULL;
		}
		if (cJSON_IsString(city) && city->valuestring)
			snprintf(bar_weather.city, sizeof(bar_weather.city), "%s",
					 city->valuestring);
		bar_weather_fetch_forecast();
	}
	cJSON_Delete(root);
}

static void bar_weather_curl(const char *url, AsyncSpawnCb cb) {
	char *argv[] = {"curl",   "-sS",  "--fail", "--connect-timeout", "3",
					"--max-time", "8", "--compressed", "-A", "asteroidz-bar",
					(char *)url, NULL};
	bar_weather.in_flight = true;
	if (!async_spawn(event_loop, argv, cb, NULL))
		bar_weather.in_flight = false;
}

static void bar_weather_fetch_forecast(void) {
	char url[512];
	/* The same request the waybar plugin makes: current conditions for the
	 * pill, plus the metrics and seven days the popover draws. */
	snprintf(url, sizeof(url),
			 "https://api.open-meteo.com/v1/forecast?latitude=%.4f"
			 "&longitude=%.4f&current=temperature_2m,apparent_temperature,"
			 "relative_humidity_2m,is_day,weather_code,surface_pressure,"
			 "wind_speed_10m,precipitation_probability"
			 "&daily=sunrise,sunset,temperature_2m_max,temperature_2m_min,"
			 "weather_code,precipitation_probability_max"
			 "&timezone=auto&forecast_days=%d",
			 bar_weather.lat, bar_weather.lon, BAR_WEATHER_DAYS);
	bar_weather_curl(url, bar_weather_on_forecast);
}

static void bar_weather_fetch(void) {
	/* one request in flight at a time: a slow network must not queue up a
	 * curl per tick */
	if (bar_weather.in_flight)
		return;
	if (bar_weather.located) {
		bar_weather_fetch_forecast();
		return;
	}
	if (config.bar_weather_location[0]) {
		char url[512];
		snprintf(url, sizeof(url),
				 "https://geocoding-api.open-meteo.com/v1/search?name=%s"
				 "&count=1&language=en&format=json",
				 config.bar_weather_location);
		bar_weather_curl(url, bar_weather_on_geo);
	} else {
		bar_weather_curl("http://ip-api.com/json/", bar_weather_on_geo);
	}
}

static int bar_weather_tick(void *data) {
	(void)data;
	bar_weather_fetch();
	if (bar_weather_timer)
		wl_event_source_timer_update(
			bar_weather_timer, config.bar_weather_interval * 60 * 1000);
	return 0;
}

/* ─── media (MPRIS) ───────────────────────────────────────────────────────── */

/* Now-playing over MPRIS, on the session bus asteroidz already owns and
 * already pumps from the event loop (ipc/session-bus.h). Every call is
 * ASYNC: sd_bus_call() blocks up to its 25-second default timeout, and one
 * unresponsive media player must not be able to freeze the compositor.
 *
 * No playerctl subprocess. The waybar module shells out to one; here that
 * would mean a fork every poll for something the bus answers directly. */
static struct {
	char player[128]; /* bus name of the player we are following */
	char title[192];
	char artist[128];
	bool playing;
	/* Which MPRIS states earn a pill: Playing or Paused, nothing else.
	 *
	 * A player that has FINISHED lingers in MPRIS as "Stopped" -- browsers
	 * especially -- with its last track's metadata still populated. Keying
	 * visibility off "we got some metadata" therefore left a stale
	 * "title . artist" on the bar with nothing playing anywhere, sometimes for
	 * days. Stopped, and any other non-play state, now hide exactly like no
	 * player at all. The waybar media plugin this replaces learned the same
	 * lesson (media_should_show). */
	bool showable;
	bool have;
	bool in_flight;
} bar_media;

static struct wl_event_source *bar_media_timer = NULL;

/* Read one a{sv} metadata dict, picking out the two fields we render. */
static void bar_media_read_metadata(sd_bus_message *m) {
	if (sd_bus_message_enter_container(m, 'a', "{sv}") < 0)
		return;
	while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
		const char *key = NULL;
		if (sd_bus_message_read(m, "s", &key) < 0) {
			sd_bus_message_exit_container(m);
			break;
		}
		if (key && strcmp(key, "xesam:title") == 0) {
			const char *v = NULL;
			if (sd_bus_message_enter_container(m, 'v', "s") > 0) {
				if (sd_bus_message_read(m, "s", &v) > 0 && v)
					snprintf(bar_media.title, sizeof(bar_media.title), "%s", v);
				sd_bus_message_exit_container(m);
			} else {
				sd_bus_message_skip(m, "v");
			}
		} else if (key && strcmp(key, "xesam:artist") == 0) {
			/* xesam:artist is an ARRAY of strings; take the first */
			if (sd_bus_message_enter_container(m, 'v', "as") > 0) {
				if (sd_bus_message_enter_container(m, 'a', "s") > 0) {
					const char *v = NULL;
					if (sd_bus_message_read(m, "s", &v) > 0 && v)
						snprintf(bar_media.artist, sizeof(bar_media.artist),
								 "%s", v);
					sd_bus_message_exit_container(m);
				}
				sd_bus_message_exit_container(m);
			} else {
				sd_bus_message_skip(m, "v");
			}
		} else {
			sd_bus_message_skip(m, "v");
		}
		sd_bus_message_exit_container(m);
	}
	sd_bus_message_exit_container(m);
}

static void bar_media_get_props(const char *name);

/* Called for every MPRIS name when the followed player is NOT playing, so a
 * player that IS gets to take over. Only that direction: a playing player is
 * never displaced by another, which is what keeps the pill stable. */
static int bar_media_on_probe(sd_bus_message *m, void *user, sd_bus_error *err) {
	(void)err;
	char *name = user;
	if (!m || sd_bus_message_is_method_error(m, NULL)) {
		free(name);
		return 0;
	}
	const char *status = NULL;
	if (sd_bus_message_enter_container(m, 'v', "s") > 0) {
		if (sd_bus_message_read(m, "s", &status) > 0 && status &&
			strcmp(status, "Playing") == 0 && !bar_media.playing &&
			strcmp(name, bar_media.player) != 0) {
			snprintf(bar_media.player, sizeof(bar_media.player), "%s", name);
			bar_media_get_props(name);
		}
		sd_bus_message_exit_container(m);
	}
	free(name);
	return 0;
}

static int bar_media_on_props(sd_bus_message *m, void *user, sd_bus_error *err) {
	(void)user;
	(void)err;
	bar_media.in_flight = false;
	if (!m || sd_bus_message_is_method_error(m, NULL)) {
		/* the player went away between ListNames and this call */
		bar_media.have = false;
		bar_media.showable = false;
		bar_media.player[0] = '\0';
		bar_update_all();
		return 0;
	}

	bool found = false;
	if (sd_bus_message_enter_container(m, 'a', "{sv}") < 0)
		return 0;
	while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
		const char *key = NULL;
		if (sd_bus_message_read(m, "s", &key) < 0) {
			sd_bus_message_exit_container(m);
			break;
		}
		if (key && strcmp(key, "PlaybackStatus") == 0) {
			const char *v = NULL;
			if (sd_bus_message_enter_container(m, 'v', "s") > 0) {
				if (sd_bus_message_read(m, "s", &v) > 0 && v) {
					bar_media.playing = strcmp(v, "Playing") == 0;
					bar_media.showable =
						bar_media.playing || strcmp(v, "Paused") == 0;
					found = true;
				}
				sd_bus_message_exit_container(m);
			} else {
				sd_bus_message_skip(m, "v");
			}
		} else if (key && strcmp(key, "Metadata") == 0) {
			if (sd_bus_message_enter_container(m, 'v', "a{sv}") > 0) {
				bar_media.title[0] = '\0';
				bar_media.artist[0] = '\0';
				bar_media_read_metadata(m);
				sd_bus_message_exit_container(m);
				found = true;
			} else {
				sd_bus_message_skip(m, "v");
			}
		} else {
			sd_bus_message_skip(m, "v");
		}
		sd_bus_message_exit_container(m);
	}
	sd_bus_message_exit_container(m);

	bar_media.have =
		found && bar_media.showable && bar_media.title[0] != '\0';
	bar_update_all();
	return 0;
}

static void bar_media_get_props(const char *name) {
	if (!session_bus || !name || !*name)
		return;
	bar_media.in_flight = true;
	if (sd_bus_call_method_async(session_bus, NULL, name,
								 "/org/mpris/MediaPlayer2",
								 "org.freedesktop.DBus.Properties", "GetAll",
								 bar_media_on_props, NULL, "s",
								 "org.mpris.MediaPlayer2.Player") < 0)
		bar_media.in_flight = false;
}

static int bar_media_on_names(sd_bus_message *m, void *user, sd_bus_error *err) {
	(void)user;
	(void)err;
	bar_media.in_flight = false;
	if (!m || sd_bus_message_is_method_error(m, NULL))
		return 0;
	if (sd_bus_message_enter_container(m, 'a', "s") < 0)
		return 0;
	char pick[128] = "";
	char first[128] = "";
	bool kept = false;
	const char *n = NULL;
	while (sd_bus_message_read(m, "s", &n) > 0) {
		if (!n || strncmp(n, "org.mpris.MediaPlayer2.", 23) != 0)
			continue;
		if (!first[0])
			snprintf(first, sizeof(first), "%s", n);
		/* An already-followed player keeps priority so the pill does not flap
		 * between two open players -- but only while it is still the
		 * interesting one. A browser tab that has been sitting Stopped since
		 * the session started should not outrank the track someone just
		 * pressed play on: opening a media player and seeing the bar keep
		 * showing something else reads as broken. */
		if (bar_media.player[0] && strcmp(n, bar_media.player) == 0) {
			snprintf(pick, sizeof(pick), "%s", n);
			kept = true;
		}
	}
	sd_bus_message_exit_container(m);
	/* the followed player is gone: fall back to whatever is there */
	if (!kept && first[0])
		snprintf(pick, sizeof(pick), "%s", first);

	if (!pick[0]) {
		bar_media.have = false;
		bar_media.showable = false;
		bar_media.player[0] = '\0';
		bar_update_all();
		return 0;
	}
	snprintf(bar_media.player, sizeof(bar_media.player), "%s", pick);
	bar_media_get_props(pick);
	return 0;
}

/* Ask every MPRIS name for its PlaybackStatus, so a player that started
 * playing can claim the pill from an idle one. Only issued while the followed
 * player is not itself playing, so the common case costs nothing extra. */
static int bar_media_on_names_probe(sd_bus_message *m, void *user,
									sd_bus_error *err) {
	(void)user;
	(void)err;
	if (!m || bar_media.playing || sd_bus_message_is_method_error(m, NULL))
		return 0;
	if (sd_bus_message_enter_container(m, 'a', "s") < 0)
		return 0;
	const char *n = NULL;
	while (sd_bus_message_read(m, "s", &n) > 0) {
		if (!n || strncmp(n, "org.mpris.MediaPlayer2.", 23) != 0)
			continue;
		if (bar_media.player[0] && strcmp(n, bar_media.player) == 0)
			continue;
		char *copy = strdup(n);
		if (!copy)
			continue;
		if (sd_bus_call_method_async(session_bus, NULL, n,
									 "/org/mpris/MediaPlayer2",
									 "org.freedesktop.DBus.Properties", "Get",
									 bar_media_on_probe, copy, "ss",
									 "org.mpris.MediaPlayer2.Player",
									 "PlaybackStatus") < 0)
			free(copy);
	}
	sd_bus_message_exit_container(m);
	return 0;
}

static void bar_media_poll(void) {
	if (!session_bus || bar_media.in_flight)
		return;
	bar_media.in_flight = true;
	if (sd_bus_call_method_async(session_bus, NULL, "org.freedesktop.DBus",
								 "/org/freedesktop/DBus",
								 "org.freedesktop.DBus", "ListNames",
								 bar_media_on_names, NULL, NULL) < 0)
		bar_media.in_flight = false;
}

static int bar_media_tick(void *data) {
	(void)data;
	bar_media_poll();
	/* While the followed player is idle, ask the others whether any of them
	 * has started. Skipped entirely once something is playing, so the steady
	 * state is the single ListNames above and nothing more. */
	if (!bar_media.playing && session_bus)
		sd_bus_call_method_async(session_bus, NULL, "org.freedesktop.DBus",
								 "/org/freedesktop/DBus", "org.freedesktop.DBus",
								 "ListNames", bar_media_on_names_probe, NULL,
								 NULL);
	if (bar_media_timer)
		wl_event_source_timer_update(bar_media_timer, 2000);
	return 0;
}

/* ─── volume (PipeWire via wpctl) ─────────────────────────────────────────── */

/* Event-driven, not polled. One long-lived `pactl subscribe` reports every
 * mixer change on stdout, and each relevant event triggers a single async
 * `wpctl get-volume`. The waybar plugin this replaces calls
 * g_spawn_command_line_sync five times per interaction; polling from the
 * compositor's own event loop would be a fork per tick for state that changes
 * a few times an hour.
 *
 * wpctl/pactl rather than a libpulse or libpipewire dependency: the round trip
 * is off the event loop either way, and linking a sound server client into the
 * compositor to render one glyph is not a trade worth making. */
static struct {
	int32_t pct;
	bool muted;
	bool have;
	bool in_flight;
} bar_volume;

static AsyncSpawn *bar_volume_sub = NULL;

static void bar_volume_on_get(const char *out, size_t len, void *user) {
	(void)len;
	(void)user;
	bar_volume.in_flight = false;
	/* "Volume: 0.42", or "Volume: 0.42 [MUTED]" */
	const char *v = strstr(out, "Volume:");
	if (!v)
		return;
	double level = 0.0;
	if (sscanf(v + strlen("Volume:"), "%lf", &level) != 1)
		return;

	int32_t pct = (int32_t)(level * 100.0 + 0.5);
	bool muted = strstr(out, "[MUTED]") != NULL;
	/* redraw only on a real change: pactl fires several events for one volume
	 * key press, and each would otherwise relayout the bar */
	if (bar_volume.have && pct == bar_volume.pct && muted == bar_volume.muted)
		return;
	bar_volume.pct = pct;
	bar_volume.muted = muted;
	bar_volume.have = true;
	bar_update_all();
}

static void bar_volume_fetch(void) {
	if (bar_volume.in_flight)
		return;
	char *const argv[] = {"wpctl", "get-volume", "@DEFAULT_AUDIO_SINK@", NULL};
	bar_volume.in_flight =
		async_spawn(event_loop, argv, bar_volume_on_get, NULL);
}

static void bar_volume_on_event(const char *line, void *user) {
	(void)user;
	/* pactl prints lines like "Event 'change' on sink #52". Server events
	 * matter too: switching the default output is a server change, not a sink
	 * one, and without it the pill kept showing the old device's level. */
	if (strstr(line, "on sink") || strstr(line, "on server"))
		bar_volume_fetch();
}

/* Called from the module refresh, i.e. potentially on every arrange, so it has
 * to be cheap AND it has to stop trying when there is nothing to talk to.
 *
 * With no sound server, `pactl subscribe` exits immediately (clearing the
 * handle) and `wpctl get-volume` returns nothing (leaving .have false), so an
 * unguarded start would fork two processes per refresh -- a fork storm during
 * a window drag. Back off to one attempt per BAR_VOLUME_RETRY_S instead, which
 * still reconnects on its own if pipewire is restarted under us. */
#define BAR_VOLUME_RETRY_S 10
static time_t bar_volume_last_try = 0;

static void bar_volume_start(void) {
	if (bar_volume_sub && bar_volume.have)
		return;
	time_t now = time(NULL);
	if (bar_volume_last_try && now - bar_volume_last_try < BAR_VOLUME_RETRY_S)
		return;
	bar_volume_last_try = now;
	if (!bar_volume.have)
		bar_volume_fetch();
	if (!bar_volume_sub) {
		char *const argv[] = {"pactl", "subscribe", NULL};
		async_spawn_lines(event_loop, argv, bar_volume_on_event, NULL,
						  &bar_volume_sub);
	}
}

static void bar_volume_finish(void) {
	async_spawn_stop(bar_volume_sub);
	bar_volume_sub = NULL;
	bar_volume_last_try = 0;
}

static void bar_volume_set(int32_t delta_pct) {
	char arg[32];
	if (delta_pct == 0) {
		char *const argv[] = {"wpctl", "set-mute", "@DEFAULT_AUDIO_SINK@",
							  "toggle", NULL};
		async_spawn(event_loop, argv, NULL, NULL);
		return;
	}
	snprintf(arg, sizeof(arg), "%d%%%c", delta_pct < 0 ? -delta_pct : delta_pct,
			 delta_pct < 0 ? '-' : '+');
	/* -l caps at 1.0 so a scroll cannot push the sink into software boost */
	char *const argv[] = {"wpctl", "set-volume", "-l", "1.0",
						  "@DEFAULT_AUDIO_SINK@", arg, NULL};
	async_spawn(event_loop, argv, NULL, NULL);
}

/* ─── notifications (swaync) ──────────────────────────────────────────────── */

/* Unread count and do-not-disturb state from swaync, over the session bus.
 *
 * The waybar module this replaces runs `swaync-client -swb` as a long-lived
 * subprocess and shells out again on every click. swaync exposes the whole
 * thing on D-Bus, so none of that is needed: SubscribeV2 pushes every change,
 * and the toggles are plain method calls.
 *
 * The glyphs are the same Nerd Font set the waybar config maps, including its
 * full six-state matrix -- do-not-disturb and "inhibited" are different
 * conditions and an inhibited bar that looked like a DND bar would be
 * misleading about whether notifications are being dropped or merely held. */
#define BAR_NOTIFY_BUS "org.erikreider.swaync.cc"
#define BAR_NOTIFY_PATH "/org/erikreider/swaync/cc"
#define BAR_NOTIFY_IFACE "org.erikreider.swaync.cc"

static struct {
	uint32_t count;
	bool dnd;
	bool cc_open;
	bool inhibited;
	bool have;
	bool subscribed;
} bar_notify;

static sd_bus_slot *bar_notify_slots[2] = {0};

/* Which bell the pill draws -- which is the WHOLE reading, count included.
 *
 * An empty bell means nothing is waiting, a filled one means something is.
 * That is the question this module gets consulted for; how many there are is
 * not, and a number beside the glyph cost a pill that changed width, a reserve
 * to stop it reflowing, and half that reserve as dead space either side of the
 * module whenever the count was absent -- which is nearly always.
 *
 * Inhibited outranks dnd, being the stronger statement about what is happening
 * to incoming notifications; both outrank "there are some", because whether
 * you will be interrupted matters more than whether there is anything to be
 * interrupted by. */
static const char *bar_notify_icon_name(void) {
	if (bar_notify.inhibited)
		return "asteroidz-bar/bell-sleep.svg";
	if (bar_notify.dnd)
		return "asteroidz-bar/bell-off.svg";
	return bar_notify.count > 0 ? "asteroidz-bar/bell.svg"
								: "asteroidz-bar/bell-outline.svg";
}

static void bar_notify_set(uint32_t count, bool dnd, bool cc_open,
						   bool inhibited) {
	if (bar_notify.have && count == bar_notify.count && dnd == bar_notify.dnd &&
		cc_open == bar_notify.cc_open && inhibited == bar_notify.inhibited)
		return; /* swaync re-emits freely; only redraw on a real change */
	bar_notify.count = count;
	bar_notify.dnd = dnd;
	bar_notify.cc_open = cc_open;
	bar_notify.inhibited = inhibited;
	bar_notify.have = true;
	bar_update_all();
}

static int bar_notify_on_subscribe(sd_bus_message *m, void *user,
								   sd_bus_error *err) {
	(void)user;
	(void)err;
	uint32_t count = 0;
	int dnd = 0, cc_open = 0, inhibited = 0;
	/* SubscribeV2 carries the inhibited flag, Subscribe does not. swaync
	 * emits both, so read what is there and leave the rest alone. */
	if (sd_bus_message_read(m, "ubb", &count, &dnd, &cc_open) <= 0)
		return 0;
	if (sd_bus_message_read(m, "b", &inhibited) <= 0)
		inhibited = bar_notify.inhibited;
	bar_notify_set(count, dnd != 0, cc_open != 0, inhibited != 0);
	return 0;
}

static int bar_notify_on_count(sd_bus_message *m, void *user,
							   sd_bus_error *err) {
	(void)user;
	(void)err;
	uint32_t count = 0;
	if (!m || sd_bus_message_is_method_error(m, NULL) ||
		sd_bus_message_read(m, "u", &count) <= 0)
		return 0;
	bar_notify_set(count, bar_notify.dnd, bar_notify.cc_open,
				   bar_notify.inhibited);
	return 0;
}

static int bar_notify_on_dnd(sd_bus_message *m, void *user, sd_bus_error *err) {
	(void)user;
	(void)err;
	int dnd = 0;
	if (!m || sd_bus_message_is_method_error(m, NULL) ||
		sd_bus_message_read(m, "b", &dnd) <= 0)
		return 0;
	bar_notify_set(bar_notify.count, dnd != 0, bar_notify.cc_open,
				   bar_notify.inhibited);
	return 0;
}

static void bar_notify_start(void) {
	if (bar_notify.subscribed || !session_bus)
		return;
	bar_notify.subscribed = true;

	/* Both spellings: SubscribeV2 on anything current, Subscribe as the
	 * fallback. The handler reads whichever fields the message actually has. */
	sd_bus_match_signal(session_bus, &bar_notify_slots[0], NULL,
						BAR_NOTIFY_PATH, BAR_NOTIFY_IFACE, "SubscribeV2",
						bar_notify_on_subscribe, NULL);
	sd_bus_match_signal(session_bus, &bar_notify_slots[1], NULL,
						BAR_NOTIFY_PATH, BAR_NOTIFY_IFACE, "Subscribe",
						bar_notify_on_subscribe, NULL);

	/* Initial state from the individual getters rather than GetSubscribeData:
	 * that returns a bare (bbub) whose field order is not self-describing, and
	 * guessing it wrong would silently swap dnd for the count. */
	sd_bus_call_method_async(session_bus, NULL, BAR_NOTIFY_BUS,
							 BAR_NOTIFY_PATH, BAR_NOTIFY_IFACE,
							 "NotificationCount", bar_notify_on_count, NULL,
							 NULL);
	sd_bus_call_method_async(session_bus, NULL, BAR_NOTIFY_BUS,
							 BAR_NOTIFY_PATH, BAR_NOTIFY_IFACE, "GetDnd",
							 bar_notify_on_dnd, NULL, NULL);
	sd_bus_flush(session_bus);
}

static void bar_notify_finish(void) {
	for (size_t i = 0; i < LENGTH(bar_notify_slots); i++) {
		if (bar_notify_slots[i])
			sd_bus_slot_unref(bar_notify_slots[i]);
		bar_notify_slots[i] = NULL;
	}
	bar_notify.subscribed = false;
}

static void bar_notify_call(const char *method) {
	if (!session_bus)
		return;
	sd_bus_call_method_async(session_bus, NULL, BAR_NOTIFY_BUS, BAR_NOTIFY_PATH,
							 BAR_NOTIFY_IFACE, method, NULL, NULL, NULL);
	sd_bus_flush(session_bus);
}

/* ─── media visualiser ────────────────────────────────────────────────────── */

/* A live spectrum in the now-playing pill, from one long-lived `cava` in raw
 * ASCII mode -- the same source the waybar media plugin uses, read a line at a
 * time through async_spawn_lines rather than a per-frame fork.
 *
 * The expensive part of this is NOT the FFT. cava's own analysis is a fraction
 * of a percent of a core; the cost is that an animating bar element damages
 * the output every frame, so the compositor recomposites at the animation rate
 * for as long as music plays. Three things keep that honest:
 *
 *   - cava only runs while something is actually playing, and is stopped the
 *     moment playback does;
 *   - the frame rate is deliberately low (bar { media-fps }, default 20), not
 *     the display's;
 *   - a frame whose bars have not moved past BAR_VIZ_EPS is skipped entirely,
 *     so a quiet passage or a paused-but-open player costs nothing.
 */
/* _POSIX_C_SOURCE alone does not expose M_PI, and the build sets it. */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define BAR_VIZ_MAX_BARS 8
#define BAR_VIZ_EPS 0.02 /* per-bar movement worth a redraw */

static struct {
	double target[BAR_VIZ_MAX_BARS]; /* latest frame from cava */
	double level[BAR_VIZ_MAX_BARS];  /* eased, what is on screen */
	double drawn[BAR_VIZ_MAX_BARS];  /* levels at the last redraw */
	int32_t silent_frames;
	bool active;
	/* No signal to analyse, so the pill shows its transport glyph instead of a
	 * dead meter. This is not a fault to fix: a player doing bitstream
	 * PASSTHROUGH (AC3/DTS over S/PDIF, mpv's audio-spdif) with
	 * audio-exclusive puts no PCM in the graph at all and locks the device, so
	 * there is nothing for cava -- or any other visualiser -- to read. Better
	 * to show the glyph than six bars pinned at zero through a whole track. */
	bool silent;
} bar_viz;

static AsyncSpawn *bar_viz_proc = NULL;
static bool bar_viz_pending;
static struct wl_event_source *bar_viz_timer = NULL;
static char bar_viz_cfg_path[256];

static void bar_viz_stop(void) {
	bar_viz_pending = false;
	if (bar_viz_proc) {
		async_spawn_stop(bar_viz_proc);
		bar_viz_proc = NULL;
	}
	if (bar_viz_timer) {
		wl_event_source_timer_update(bar_viz_timer, 0);
	}
	bar_viz.active = false;
	memset(&bar_viz, 0, sizeof(bar_viz));
}

/* "v0;v1;...;" with each value 0..1000 */
static void bar_viz_on_line(const char *line, void *user) {
	(void)user;
	int32_t i = 0;
	bool any = false;
	const char *p = line;
	while (*p && i < config.bar_media_bars) {
		char *end = NULL;
		long v = strtol(p, &end, 10);
		if (end == p)
			break;
		if (v < 0)
			v = 0;
		if (v > 1000)
			v = 1000;
		/* sqrt curve: linear magnitudes leave the bars hugging the floor for
		 * anything but a bass hit */
		if (v > 0)
			any = true;
		bar_viz.target[i++] = sqrt((double)v / 1000.0);
		p = end;
		while (*p == ';' || *p == ' ')
			p++;
	}

	/* A few seconds of pure silence while the player says it is playing means
	 * there is no PCM to see -- passthrough, or a device we cannot monitor. */
	if (any) {
		bar_viz.silent_frames = 0;
		if (bar_viz.silent) {
			bar_viz.silent = false;
			bar_update_all();
		}
	} else if (!bar_viz.silent &&
			   ++bar_viz.silent_frames > config.bar_media_fps * 3) {
		bar_viz.silent = true;
		bar_update_all();
	}
}

static const char *bar_viz_icon(void) {
	/* Keyed on the quantised levels so an unchanged frame reuses the cached
	 * surface instead of rasterising an identical one. */
	static char key[128];
	int32_t n = config.bar_media_bars;
	size_t o = snprintf(key, sizeof(key), "asteroidz-viz");
	for (int32_t i = 0; i < n && o < sizeof(key); i++)
		o += snprintf(key + o, sizeof(key) - o, ":%d",
					  (int32_t)(bar_viz.level[i] * 20.0));

	const int32_t sz = 64;
	cairo_surface_t *surf =
		cairo_image_surface_create(CAIRO_FORMAT_ARGB32, sz, sz);
	if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(surf);
		return NULL;
	}
	cairo_t *cr = cairo_create(surf);
	cairo_set_antialias(cr, CAIRO_ANTIALIAS_BEST);

	const float *c = config.theme.focus_bg_color;
	cairo_set_source_rgba(cr, c[0], c[1], c[2], c[3]);

	/* Roughly an 11:1 bar-to-gap ratio. It has been widened twice: 3:1 read as
	 * a row of thin lines rather than a level meter, and 5:1 was still more
	 * gap than a spectrum this small can afford -- at eight bars in a 64px
	 * square, every pixel spent on a gap is a pixel the bar does not have. */
	double gap = (double)sz / (n * 12.0);
	double bw = ((double)sz - gap * (n - 1)) / n;
	double floor_h = sz * 0.10; /* a resting bar is a dot, not nothing */
	for (int32_t i = 0; i < n; i++) {
		double h = floor_h + bar_viz.level[i] * (sz - floor_h);
		double x = i * (bw + gap);
		/* Mirrored: each bar is centred on the middle and grows BOTH ways,
		 * which is the waybar visualiser's `mirror` mode. Grown from the
		 * bottom instead, a quiet passage collapses the whole thing onto the
		 * floor of its box -- so the glyph stops sitting on the same optical
		 * line as the text and icons beside it, and the pill reads as
		 * misaligned even though nothing moved. */
		double y = (sz - h) / 2.0;
		/* rounded caps, like the waybar visualiser's */
		double r = bw / 2.0;
		if (r > h / 2.0)
			r = h / 2.0;
		cairo_new_path(cr);
		cairo_arc(cr, x + r, y + r, r, M_PI, 1.5 * M_PI);
		cairo_arc(cr, x + bw - r, y + r, r, 1.5 * M_PI, 2.0 * M_PI);
		cairo_arc(cr, x + bw - r, y + h - r, r, 0.0, 0.5 * M_PI);
		cairo_arc(cr, x + r, y + h - r, r, 0.5 * M_PI, M_PI);
		cairo_close_path(cr);
		cairo_fill(cr);
	}
	cairo_destroy(cr);
	cairo_surface_flush(surf);
	if (!asteroidz_icon_cache_put_surface(key, surf))
		return NULL;
	return key;
}

static int bar_viz_tick(void *data) {
	(void)data;
	if (!bar_viz.active) {
		if (bar_viz_timer)
			wl_event_source_timer_update(bar_viz_timer, 0);
		return 0;
	}

	/* ease toward the latest cava frame; falling slower than rising reads as
	 * a decay rather than a flicker */
	bool moved = false;
	for (int32_t i = 0; i < config.bar_media_bars; i++) {
		double t = bar_viz.target[i];
		double l = bar_viz.level[i];
		bar_viz.level[i] = l + (t - l) * (t > l ? 0.5 : 0.2);
		if (fabs(bar_viz.level[i] - bar_viz.drawn[i]) > BAR_VIZ_EPS)
			moved = true;
	}
	if (moved) {
		memcpy(bar_viz.drawn, bar_viz.level, sizeof(bar_viz.drawn));
		bar_update_all();
	}
	if (bar_viz_timer)
		wl_event_source_timer_update(bar_viz_timer,
									 1000 / config.bar_media_fps);
	return 0;
}

/* Write the config and launch cava against `sink`'s monitor. */
static void bar_viz_launch(const char *sink) {
	if (bar_viz_proc)
		return;
	if (!bar_viz_cfg_path[0]) {
		const char *rt = getenv("XDG_RUNTIME_DIR");
		snprintf(bar_viz_cfg_path, sizeof(bar_viz_cfg_path),
				 "%s/asteroidz-cava.conf", rt && *rt ? rt : "/tmp");
	}
	FILE *f = fopen(bar_viz_cfg_path, "we");
	if (!f)
		return;
	/* The DEFAULT SINK'S MONITOR, not "auto". auto picks whatever pipewire
	 * offers first, which on a machine with more than one output is regularly
	 * not the one being played to -- and a player doing passthrough or
	 * upmixing to a second device then leaves the visualiser flat while music
	 * is audibly playing. The waybar plugin names the monitor explicitly for
	 * the same reason.
	 *
	 * autosens so a quiet podcast still fills the bars instead of sitting on
	 * the floor; mono because the pill is far too small for stereo to read. */
	char source[256];
	if (sink && *sink)
		snprintf(source, sizeof(source), "%s.monitor", sink);
	else
		snprintf(source, sizeof(source), "auto");
	fprintf(f,
			"[general]\nbars = %d\nframerate = %d\nautosens = 1\n"
			"sensitivity = 100\nlower_cutoff_freq = 50\n"
			"higher_cutoff_freq = 12000\n"
			"[input]\nmethod = pipewire\nsource = %s\n"
			"[output]\nmethod = raw\nraw_target = /dev/stdout\n"
			"data_format = ascii\nascii_max_range = 1000\nchannels = mono\n"
			"[smoothing]\nnoise_reduction = 35\n",
			config.bar_media_bars, config.bar_media_fps, source);
	fclose(f);

	char *const argv[] = {"cava", "-p", bar_viz_cfg_path, NULL};
	async_spawn_lines(event_loop, argv, bar_viz_on_line, NULL, &bar_viz_proc);
	if (!bar_viz_proc)
		return;
	bar_viz.active = true;
	if (!bar_viz_timer)
		bar_viz_timer =
			wl_event_loop_add_timer(event_loop, bar_viz_tick, NULL);
	if (bar_viz_timer)
		wl_event_source_timer_update(bar_viz_timer,
									 1000 / config.bar_media_fps);
}

/* Pick the monitor to listen on from the sink list.
 *
 * NOT the default sink, which is the obvious choice and the wrong one: the
 * default is only where audio goes when nothing says otherwise. A player
 * pointed at a specific device, or doing passthrough to a receiver, plays
 * somewhere else entirely -- and on this machine the default sink sat IDLE
 * while the music was audibly running through the S/PDIF output, so a
 * visualiser watching the default monitor showed a flat line through an
 * entire track.
 *
 * The sink that is RUNNING is the one making noise, so that is the one to
 * watch. */
static void bar_viz_on_sinks(const char *out, size_t len, void *user) {
	(void)len;
	(void)user;
	bar_viz_pending = false;

	char chosen[192] = "";
	cJSON *root = cJSON_Parse(out);
	if (root) {
		cJSON *sink = NULL;
		cJSON_ArrayForEach(sink, root) {
			cJSON *name = cJSON_GetObjectItem(sink, "name");
			cJSON *state = cJSON_GetObjectItem(sink, "state");
			if (!cJSON_IsString(name) || !name->valuestring)
				continue;
			if (cJSON_IsString(state) && state->valuestring &&
				strcmp(state->valuestring, "RUNNING") == 0) {
				snprintf(chosen, sizeof(chosen), "%s", name->valuestring);
				break;
			}
		}
		cJSON_Delete(root);
	}
	/* nothing running: hand cava "auto" rather than guessing, so it makes its
	 * own choice instead of us pinning a silent device */
	bar_viz_launch(chosen[0] ? chosen : NULL);
}

/* Called from the media refresh, i.e. potentially on every arrange while
 * something is playing. With no cava installed, or no pipewire to read, the
 * child exits immediately and clears the handle -- so an unguarded restart
 * would fork twice per refresh. Exactly the failure the volume module hit;
 * same fix, and the retry still recovers if cava or the sound server comes
 * back mid-session. */
#define BAR_VIZ_RETRY_S 10
static time_t bar_viz_last_try = 0;

static void bar_viz_start(void) {
	if (bar_viz_proc || bar_viz_pending)
		return;
	time_t now = time(NULL);
	if (bar_viz_last_try && now - bar_viz_last_try < BAR_VIZ_RETRY_S)
		return;
	bar_viz_last_try = now;
	/* one round trip to find the sink actually in use, then cava stays up for
	 * as long as playback does */
	char *const argv[] = {"pactl", "-f", "json", "list", "sinks", NULL};
	bar_viz_pending = async_spawn(event_loop, argv, bar_viz_on_sinks, NULL);
	if (!bar_viz_pending)
		bar_viz_launch(NULL);
}

static void bar_viz_finish(void) {
	bar_viz_stop();
	bar_viz_last_try = 0;
	if (bar_viz_timer) {
		wl_event_source_remove(bar_viz_timer);
		bar_viz_timer = NULL;
	}
	if (bar_viz_cfg_path[0])
		unlink(bar_viz_cfg_path);
}

#include "bar-display.h"
#include "bar-vpn.h"
#include "bar-discord.h"
#include "bar-medication.h"
#include "bar-popover.h"
#include "bar-tray.h"
/* last: its content function reaches into every module's state, including the
 * tray's */
#include "bar-tooltip.h"

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

/* Up to `max` app icons for the windows on `tag`, newest-first in stack
 * order. Duplicated app-ids are collapsed: three terminals on one tag should
 * read as "terminals live here", not fill the pill with the same glyph. */
static int32_t bar_tag_app_icons(Monitor *m, uint32_t tag, const char **out,
								 int32_t max) {
	int32_t n = 0;
	Client *c = NULL;
	uint32_t mask = 1u << (tag - 1);
	wl_list_for_each(c, &clients, link) {
		if (n >= max)
			break;
		if (c->mon != m || !(c->tags & mask))
			continue;
		const char *id = c->icon_name ? c->icon_name : client_get_appid(c);
		if (!id || !*id)
			continue;
		bool dup = false;
		for (int32_t i = 0; i < n && !dup; i++)
			dup = strcmp(out[i], id) == 0;
		if (!dup)
			out[n++] = id;
	}
	return n;
}

/* The waybar workspace label: "N: name" for a tag that is in use and carries a
 * custom name, bare "N" otherwise. The index is always shown -- a row reading
 * "web · code · chat" gives no clue which Super+<n> reaches which, and the
 * padding slots have no name to show in the first place. */
static void bar_tag_label(Monitor *m, uint32_t t, bool relevant, char *buf,
						  size_t len) {
	char idx[16];
	snprintf(idx, sizeof(idx), "%u", t);
	char name[BAR_TEXT_MAX];
	tag_display_name(m, t, name, sizeof(name));
	if (relevant && strcmp(name, idx) != 0)
		snprintf(buf, len, "%s: %s", idx, name);
	else
		snprintf(buf, len, "%s", idx);
}

static void bar_module_refresh_tags(BarModule *mod) {
	Monitor *m = mod->mon;
	uint32_t sel = m->tagset[m->seltags] & TAGMASK;
	uint32_t occ = bar_occupied_tags(m);
	uint32_t urg = bar_urgent_tags(m);
	int32_t n = 0;

	/* the asteroidz ship, leading the workspace group like the waybar pill */
	if (config.bar_show_logo) {
		BarPill *logo = bar_pill_get(mod, n);
		if (logo) {
			char path[512];
			bar_icon_path(path, sizeof(path),
						  "waybar-asteroidz-workspaces/logo.svg");
			asteroidz_tab_bar_node_set_icon(logo->node, path);
			logo->text[0] = '\0';
			logo->arg = 0; /* arg 0 => not a tag, so clicks are ignored */
			logo->fixed_width = bar_icon_pill_width(logo, bar_pill_height());
			bar_pill_style(logo, BAR_LOOK_FLAT);
			n++;
		}
	}

	/* Which tags get a pill, mirroring the waybar workspace module exactly:
	 * every tag that is selected or holds a window, then padded with the
	 * lowest-numbered empty tags up to `min-tags` (its `min-pills`). */
	bool show[LENGTH(tags) + 1];
	int32_t relevant = 0;
	for (uint32_t t = 1; t <= LENGTH(tags); t++) {
		uint32_t mask = 1u << (t - 1);
		show[t] = config.bar_show_all_tags || (sel & mask) || (occ & mask);
		if (show[t])
			relevant++;
	}
	for (uint32_t t = 1; t <= LENGTH(tags) && relevant < config.bar_min_tags;
		 t++)
		if (!show[t]) {
			show[t] = true;
			relevant++;
		}

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
		if (!show[t])
			continue;

		BarPill *p = bar_pill_get(mod, n);
		if (!p)
			break;
		p->arg = t;
		bar_tag_label(m, t, selected || occupied, p->text, sizeof(p->text));
		const char *icons[ASTEROIDZ_TAB_MAX_ICONS];
		int32_t ni = config.bar_tag_icons > 0
						 ? bar_tag_app_icons(m, t, icons, config.bar_tag_icons)
						 : 0;
		asteroidz_tab_bar_node_set_icons(p->node, icons, ni);
		/* label first, then what is running on the tag -- the waybar workspace
		 * plugin's order (the app icons read as belonging to the number they
		 * follow, where leading icons pushed the number away from its pill) */
		asteroidz_tab_bar_node_set_icons_after_text(p->node, true);
		/* free-sized: the icon count varies per tag, and a tag gaining a
		 * window is a real layout change rather than per-tick jitter */
		p->fixed_width = 0;
		bar_pill_style(p, selected           ? BAR_LOOK_ACTIVE
					  : (urg & mask) ? BAR_LOOK_URGENT
					  : occupied     ? BAR_LOOK_OCCUPIED
									 : BAR_LOOK_EMPTY);
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
	p->fixed_width = bar_clock_fixed_width(p, bar_pill_height());
	bar_pill_style(p, BAR_LOOK_FLAT);
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
	/* title-width is a CAP, not a pin: a short title should occupy a short
	 * pill, exactly like the waybar label's max-width-chars + ellipsize. Pinned
	 * to the cap it left a wide empty gap after every short title and pushed
	 * the centre section off-centre for no reason. */
	int32_t natural = asteroidz_tab_bar_node_measure_width(p->node, p->text,
														   bar_pill_height());
	p->fixed_width = (config.bar_title_width > 0 &&
					  natural > config.bar_title_width)
						 ? config.bar_title_width
						 : natural;
	p->flexible = true;
	/* left-aligned: centring the icon+title group inside the pill made the
	 * title drift sideways as it changed length, and detached it from the
	 * layout chip it follows */
	asteroidz_tab_bar_node_set_text_align_left(p->node, true);
	p->align_left = true;
	/* resting colours, not the focus pair: a highlighted title competed with
	 * the selected-tag pill for "this is the active thing". */
	bar_pill_style(p, BAR_LOOK_FLAT);
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
	/* sized from the icon, not the bar height: a square pill loses its
	 * artwork to the theme's padding_x and renders clipped */
	p->fixed_width = bar_icon_pill_width(p, bar_pill_height());
	bar_pill_style(p, BAR_LOOK_SUNKEN);
	p->arg = 0;
	mod->npills = 1;
	for (int32_t i = 1; i < BAR_MAX_PILLS; i++)
		bar_pill_release(&mod->pills[i]);
}

/* The load ramp for the cpu/memory icons. No numbers: the whole point of the
 * sysinfo pill these replace is that the COLOUR carries the reading, which is
 * legible at a glance in a way a two-digit percentage is not (the exact figure
 * belongs in a popover, which the native bar does not have yet).
 *
 * Four steps rather than sysinfo's three, so "busy" and "about to hurt" are
 * distinguishable: resting, working, heavy, saturated. */
static const float bar_load_amber[4] = {0.98f, 0.72f, 0.20f, 1.0f};

static void bar_load_tint(int32_t pct, float out[4]) {
	if (pct >= 85) { /* saturated */
		memcpy(out, config.theme.urgent_color, sizeof(float) * 4);
		return;
	}
	if (pct >= 60) { /* heavy */
		memcpy(out, bar_load_amber, sizeof(float) * 4);
		return;
	}
	if (pct >= 20) { /* working: the theme accent, like .si-ic.warn */
		memcpy(out, config.theme.focus_bg_color, sizeof(float) * 4);
		return;
	}
	/* resting: the foreground, dimmed to read as inactive (@outline) */
	memcpy(out, config.theme.fg_color, sizeof(float) * 4);
	out[3] *= 0.45f;
}

/* ─── network activity indicator ──────────────────────────────────────────── */

/* Two stacked arrows -- upload on top, download below -- each lit by its own
 * direction's throughput, like the pair of activity LEDs on a switch port.
 *
 * DRAWN rather than tinted, because the two halves take different colours and
 * a tint is per-node: one stencil cannot carry two states. The result goes
 * into the shared icon cache under a key naming the two tiers, so only the
 * sixteen possible combinations are ever rasterised however long the session
 * runs -- not one surface per sample. */
#define BAR_NET_TIERS 4

/* Throughput bands, in bytes/sec. Chosen so the common case reads as calm: a
 * desktop trickling background chatter should show resting, not "busy". */
static int32_t bar_net_tier(double bytes_per_sec) {
	if (bytes_per_sec >= 4.0 * 1024 * 1024)
		return 3; /* saturated */
	if (bytes_per_sec >= 512.0 * 1024)
		return 2; /* heavy */
	if (bytes_per_sec >= 8.0 * 1024)
		return 1; /* active */
	return 0;     /* resting */
}

static void bar_net_tier_color(int32_t tier, float out[4]) {
	switch (tier) {
	case 3:
		memcpy(out, config.theme.urgent_color, sizeof(float) * 4);
		return;
	case 2:
		memcpy(out, bar_load_amber, sizeof(float) * 4);
		return;
	case 1:
		memcpy(out, config.theme.focus_bg_color, sizeof(float) * 4);
		return;
	default:
		memcpy(out, config.theme.fg_color, sizeof(float) * 4);
		out[3] *= 0.35f; /* unlit, like a dark LED */
		return;
	}
}

/* One arrow, pointing up or down, filling the given band of the surface. */
static void bar_net_draw_arrow(cairo_t *cr, double x, double y, double w,
							   double h, bool up, const float rgba[4]) {
	double mid = x + w / 2.0;
	cairo_new_path(cr);
	if (up) {
		cairo_move_to(cr, mid, y);
		cairo_line_to(cr, x + w, y + h);
		cairo_line_to(cr, x, y + h);
	} else {
		cairo_move_to(cr, mid, y + h);
		cairo_line_to(cr, x + w, y);
		cairo_line_to(cr, x, y);
	}
	cairo_close_path(cr);
	cairo_set_source_rgba(cr, rgba[0], rgba[1], rgba[2], rgba[3]);
	cairo_fill(cr);
}

static const char *bar_net_icon(int32_t up_tier, int32_t down_tier) {
	static char key[64];
	snprintf(key, sizeof(key), "asteroidz-net:%d:%d", up_tier, down_tier);

	/* Drawn at a fixed 64px and scaled down by the pill, like every other
	 * icon: the artwork is cached on the tiers alone, so it must not also
	 * depend on whatever height the bar happens to be configured to today. */
	const int32_t sz = 64;
	cairo_surface_t *surf =
		cairo_image_surface_create(CAIRO_FORMAT_ARGB32, sz, sz);
	if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(surf);
		return NULL;
	}
	cairo_t *cr = cairo_create(surf);
	cairo_set_antialias(cr, CAIRO_ANTIALIAS_BEST);

	float up[4], down[4];
	bar_net_tier_color(up_tier, up);
	bar_net_tier_color(down_tier, down);

	/* a gap between the two so they read as two lights, not one shape */
	const double gap = sz * 0.10;
	const double half = (sz - gap) / 2.0;
	/* Edge to edge horizontally. This used to inset the arrows by 12% a side,
	 * which is drawn-in transparent margin: the bar reserves the icon's
	 * extent, so that margin rendered as 4px of extra gap either side of this
	 * module and nowhere else. Every other icon fills its canvas
	 * (contrib/normalize-bar-icons.py); this one is drawn, so it has to do the
	 * same by construction. */
	bar_net_draw_arrow(cr, 0.0, 0.0, sz, half, true, up);
	bar_net_draw_arrow(cr, 0.0, half + gap, sz, half, false, down);

	cairo_destroy(cr);
	cairo_surface_flush(surf);
	if (!asteroidz_icon_cache_put_surface(key, surf))
		return NULL;
	return key;
}

/* One-pill metric modules. The artwork is the waybar sysinfo plugin's
 * monochrome SVG set, tinted here rather than drawn as-is -- those files are
 * a solid #000 silhouette and paint as an invisible black blob on a dark
 * panel otherwise. */
static void bar_module_refresh_metric(BarModule *mod) {
	BarPill *p = bar_pill_get(mod, 0);
	if (!p) {
		mod->npills = 0;
		return;
	}
	char icon[512];
	switch (mod->kind) {
	/* The sysinfo plugin's artwork, not sysmon's: sysinfo is what the waybar
	 * config actually loads (`cffi/sysinfo`, which replaced cffi/sysmon +
	 * cffi/network), and its icons are the flat monochrome set the rest of the
	 * bar is drawn in -- sysmon's are gradient-filled and read as from a
	 * different bar entirely. */
	case BAR_MODULE_CPU:
	case BAR_MODULE_MEMORY: {
		int32_t pct = mod->kind == BAR_MODULE_CPU ? bar_metrics.cpu_pct
												  : bar_metrics.mem_pct;
		bar_icon_path(icon, sizeof(icon),
					  mod->kind == BAR_MODULE_CPU ? "waybar-sysinfo/cpu.svg"
												  : "waybar-sysinfo/mem.svg");
		asteroidz_tab_bar_node_set_icon(p->node, icon);
		float tint[4];
		bar_load_tint(pct, tint);
		asteroidz_tab_bar_node_set_icon_tint(p->node, tint);
		/* icon only: the tint is the reading */
		p->text[0] = '\0';
		p->fixed_width = bar_icon_pill_width(p, bar_pill_height());
		break;
	}
	case BAR_MODULE_NETWORK: {
		/* Icon only, like cpu and memory: the two arrows carry the reading,
		 * so it joins the same tight run instead of trailing a wide reserve
		 * for throughput text that was mostly empty space. A down link lights
		 * both arrows in the urgent colour rather than showing a separate
		 * "disconnected" glyph -- the pair going red IS the reading. */
		int32_t up_tier = bar_metrics.link_up
							  ? bar_net_tier(bar_metrics.tx_rate)
							  : BAR_NET_TIERS - 1;
		int32_t down_tier = bar_metrics.link_up
								? bar_net_tier(bar_metrics.rx_rate)
								: BAR_NET_TIERS - 1;
		const char *key = bar_net_icon(up_tier, down_tier);
		if (key)
			asteroidz_tab_bar_node_set_icon(p->node, key);
		/* the artwork carries its own colours; tinting would flatten both
		 * halves to one */
		asteroidz_tab_bar_node_set_icon_tint(p->node, NULL);
		p->text[0] = '\0';
		p->fixed_width = bar_icon_pill_width(p, bar_pill_height());
		break;
	}
	default:
		break;
	}
	p->arg = 0;
	/* always flat: the state lives in the icon tint now, and a filled urgent
	 * pill would change this module's padding and shove its neighbours */
	bar_pill_style(p, BAR_LOOK_FLAT);
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
	/* SVG art, not a glyph pair: the mug matches the nf-md-coffee glyph this
	 * replaces, but every pill in the bar now draws real artwork at one
	 * uniform square size, which a font glyph cannot be held to. */
	char icon[512];
	bar_icon_path(icon, sizeof(icon),
				  idle_inhibit_manual ? "asteroidz-bar/idle-on.svg"
									  : "asteroidz-bar/idle-off.svg");
	asteroidz_tab_bar_node_set_icon(p->node, icon);
	asteroidz_tab_bar_node_set_icon_tint(p->node,
										 idle_inhibit_manual
											 ? config.theme.focus_bg_color
											 : config.theme.fg_color);
	p->text[0] = '\0';
	p->arg = 0;
	/* Icon-only, so the width is the artwork's square box: both states draw
	 * the same size and toggling cannot reflow the section. */
	p->fixed_width = bar_icon_pill_width(p, bar_pill_height());
	/* Never filled -- the same trap the bell fell into. The icon is tinted
	 * with the accent, and the active look fills the pill with that same
	 * accent, so turning the inhibitor ON is exactly when its mug vanished
	 * into a blank chip. The state is the artwork: crossed mug off, plain mug
	 * in accent on. */
	bar_pill_style(p, BAR_LOOK_FLAT);
	mod->npills = 1;
	for (int32_t i = 1; i < BAR_MAX_PILLS; i++)
		bar_pill_release(&mod->pills[i]);
}

static void bar_module_refresh_weather(BarModule *mod) {
	BarPill *p = bar_pill_get(mod, 0);
	if (!p) {
		mod->npills = 0;
		return;
	}
	char icon[512], rel[64];
	snprintf(rel, sizeof(rel), "waybar-weather/%s",
			 bar_wmo_icon(bar_weather.valid ? bar_weather.code : 3,
						  bar_weather.is_day));
	bar_icon_path(icon, sizeof(icon), rel);
	asteroidz_tab_bar_node_set_icon(p->node, icon);
	if (bar_weather.valid)
		snprintf(p->text, sizeof(p->text), "%d°", bar_weather.temp_c);
	else
		snprintf(p->text, sizeof(p->text), "--°");
	p->arg = 0;
	/* -99 deg is wider than any real reading here, and the pill must not
	 * resize when the temperature crosses 0 or 10 */
	p->fixed_width = bar_template_width(p, "-99°", bar_pill_height());
	bar_pill_style(p, BAR_LOOK_FLAT);
	mod->npills = 1;
	for (int32_t i = 1; i < BAR_MAX_PILLS; i++)
		bar_pill_release(&mod->pills[i]);
}

static void bar_module_refresh_media(BarModule *mod) {
	if (!bar_media.have) {
		bar_viz_stop();
		/* nothing playing: drop the pill so the slot collapses rather than
		 * showing a permanently empty widget */
		for (int32_t i = 0; i < BAR_MAX_PILLS; i++)
			bar_pill_release(&mod->pills[i]);
		mod->npills = 0;
		return;
	}
	/* ── transport ──
	 *
	 * Three buttons ahead of the track, each its own pill because a pill is
	 * the unit of hit testing: one node cannot have three targets in it. They
	 * are icon-only, so the row costs about as much width as one word.
	 *
	 * `arg` is what the click handler dispatches on -- an index, not a
	 * pointer, so a pill pool that reorders underneath it cannot mislead. */
	int32_t n = 0;
	char icon[512];
	static const struct {
		const char *svg;
		uint32_t arg;
	} transport[] = {
		{"waybar-media-cava/prev.svg", BAR_MEDIA_PREV},
		{NULL, BAR_MEDIA_PLAYPAUSE}, /* play or pause, decided below */
		{"waybar-media-cava/next.svg", BAR_MEDIA_NEXT},
	};
	for (size_t i = 0; i < LENGTH(transport); i++) {
		BarPill *b = bar_pill_get(mod, n);
		if (!b)
			break;
		/* the ACTION, not the state: a playing track offers pause */
		const char *svg = transport[i].svg
							  ? transport[i].svg
							  : (bar_media.playing
									 ? "waybar-media-cava/pause.svg"
									 : "waybar-media-cava/play.svg");
		bar_icon_path(icon, sizeof(icon), svg);
		asteroidz_tab_bar_node_set_icon(b->node, icon);
		asteroidz_tab_bar_node_set_icon_tint(b->node, config.theme.fg_color);
		/* These svgs fill their viewBox edge to edge, with none of the margin
		 * a themed icon carries, so at the pill's full height they tower over
		 * every other glyph on the bar. Two thirds puts their ink on the same
		 * optical size as the status icons beside them. */
		asteroidz_tab_bar_node_set_icon_scale(b->node, 0.66);
		b->text[0] = '\0';
		b->arg = transport[i].arg;
		b->flexible = false;
		b->fixed_width = bar_icon_pill_width(b, bar_pill_height());
		bar_pill_style(b, BAR_LOOK_FLAT);
		n++;
	}

	BarPill *p = bar_pill_get(mod, n);
	if (!p) {
		mod->npills = n;
		return;
	}
	/* While something is playing the leading glyph is a live spectrum; paused,
	 * it falls back to the transport glyph. The visualiser is started and
	 * stopped here rather than on the MPRIS callback so it follows what is
	 * actually ON SCREEN -- a player that vanishes takes the pill with it, and
	 * an animation left running for a pill nobody can see is pure waste. */
	const char *viz = NULL;
	if (config.bar_media_viz && bar_media.playing) {
		bar_viz_start();
		/* cava stays up either way -- it costs nothing while silent, and it is
		 * how we notice the signal coming back */
		if (!bar_viz.silent)
			viz = bar_viz_icon();
	} else {
		bar_viz_stop();
	}
	if (viz) {
		asteroidz_tab_bar_node_set_icon(p->node, viz);
		/* drawn in the accent already; a tint would flatten it */
		asteroidz_tab_bar_node_set_icon_tint(p->node, NULL);
	} else {
		/* the ACTION, not the state: clicking this pill sends PlayPause, so a
		 * playing track shows the pause glyph and vice versa */
		bar_icon_path(icon, sizeof(icon),
					  bar_media.playing ? "waybar-media-cava/pause.svg"
										: "waybar-media-cava/play.svg");
		asteroidz_tab_bar_node_set_icon(p->node, icon);
		asteroidz_tab_bar_node_set_icon_tint(p->node, NULL);
	}
	if (bar_media.artist[0])
		snprintf(p->text, sizeof(p->text), "%s \u2022 %s", bar_media.title,
				 bar_media.artist);
	else
		snprintf(p->text, sizeof(p->text), "%s", bar_media.title);
	p->arg = BAR_MEDIA_TITLE;
	p->fixed_width = config.bar_media_width;
	p->flexible = true;
	bar_pill_style(p, BAR_LOOK_FLAT);
	n++;
	mod->npills = n;
	for (int32_t i = n; i < BAR_MAX_PILLS; i++)
		bar_pill_release(&mod->pills[i]);
}

/* One pill per tray item, icon only. Items are pooled like every other pill,
 * so an application coming and going does not churn scene nodes.
 *
 * A Passive item is hidden: that status means "nothing to say right now", and
 * showing it anyway is how a tray ends up as a row of identical grey squares. */
static void bar_module_refresh_tray(BarModule *mod) {
	bar_tray_start();
	bar_tray_poll_props();

	int32_t n = 0;
	for (int32_t i = 0; i < bar_tray_nitems && n < BAR_MAX_PILLS; i++) {
		BarTrayItem *it = &bar_tray_items[i];
		if (!it->used)
			continue;
		if (it->status[0] && strcmp(it->status, "Passive") == 0)
			continue;
		BarPill *p = bar_pill_get(mod, n);
		if (!p)
			break;
		/* the index into bar_tray_items, so a click can find the item again
		 * without the pill holding a pointer into a table that compacts */
		p->arg = (uint32_t)i;
		/* An icon-only pill with no icon measures to a single pixel and simply
		 * is not there -- the item vanishes from the tray with no hint that it
		 * exists. Fall back to the start of its Id so the slot is visible and
		 * identifiable while the artwork is still being chased. */
		if (it->icon_key[0]) {
			p->text[0] = '\0';
		} else {
			const char *label = it->id[0] ? it->id : it->title;
			snprintf(p->text, sizeof(p->text), "%.3s", label[0] ? label : "?");
		}
		asteroidz_tab_bar_node_set_icon(p->node, it->icon_key);
		/* real application artwork, not a stencil: never tint it */
		asteroidz_tab_bar_node_set_icon_tint(p->node, NULL);
		p->fixed_width = p->text[0]
							 ? bar_template_width(p, p->text, bar_pill_height())
							 : bar_icon_pill_width(p, bar_pill_height());
		bar_pill_style(p, strcmp(it->status, "NeedsAttention") == 0
							  ? BAR_LOOK_URGENT
							  : BAR_LOOK_FLAT);
		n++;
	}
	for (int32_t i = n; i < BAR_MAX_PILLS; i++)
		bar_pill_release(&mod->pills[i]);
	mod->npills = n;
}

/* Speaker icon plus the level, matching the waybar volume module. The artwork
 * is that plugin's own SVG set -- gradient-filled, but only its alpha is used
 * when tinted, so it lands as a flat silhouette in the same visual family as
 * the sysinfo glyphs it sits beside. */
static void bar_module_refresh_volume(BarModule *mod) {
	bar_volume_start();
	BarPill *p = bar_pill_get(mod, 0);
	if (!p) {
		mod->npills = 0;
		return;
	}
	const char *art = bar_volume.muted || bar_volume.pct == 0
						  ? "waybar-volume/vol-mute.svg"
					  : bar_volume.pct < 34 ? "waybar-volume/vol-low.svg"
					  : bar_volume.pct < 67 ? "waybar-volume/vol-med.svg"
											: "waybar-volume/vol-high.svg";
	char icon[512];
	bar_icon_path(icon, sizeof(icon), art);
	asteroidz_tab_bar_node_set_icon(p->node, icon);

	float tint[4];
	memcpy(tint, config.theme.fg_color, sizeof(tint));
	if (bar_volume.muted)
		tint[3] *= 0.45f; /* reads as inactive, like a resting metric */
	asteroidz_tab_bar_node_set_icon_tint(p->node, tint);

	/* The level is shown even when muted -- unmuting restores it, so hiding it
	 * behind a dash just means looking up what you are about to get. Mute is
	 * carried by the crossed-out icon and its dimmed tint instead. */
	if (!bar_volume.have)
		p->text[0] = '\0';
	else
		snprintf(p->text, sizeof(p->text), "%d%%", bar_volume.pct);
	p->arg = 0;
	/* Pinned to the widest reading with the SAME number of digits, not to the
	 * widest reading there is. Digits are proportional -- "11%" is narrower
	 * than "88%" -- so an unpinned pill twitches on every step, while pinning
	 * to "100%" leaves a two-digit level floating in a hole a whole digit
	 * wide. Per-digit-count, the width is stable through a volume ramp and
	 * only steps at 9->10 and 99->100, which is a moment the pill is visibly
	 * changing anyway. No level at all (no audio server) reserves nothing. */
	p->fixed_width =
		!bar_volume.have ? 0
		: bar_volume.pct >= 100
			? bar_template_width(p, "100%", bar_pill_height())
		: bar_volume.pct >= 10 ? bar_template_width(p, "88%", bar_pill_height())
							   : bar_template_width(p, "8%", bar_pill_height());
	bar_pill_style(p, BAR_LOOK_FLAT);
	mod->npills = 1;
	for (int32_t i = 1; i < BAR_MAX_PILLS; i++)
		bar_pill_release(&mod->pills[i]);
}

/* Bell glyph, and nothing else: empty when nothing is waiting, filled when
 * something is. See bar_notify_icon_name. */
static void bar_module_refresh_notify(BarModule *mod) {
	bar_notify_start();
	BarPill *p = bar_pill_get(mod, 0);
	if (!p) {
		mod->npills = 0;
		return;
	}
	char icon[512];
	bar_icon_path(icon, sizeof(icon), bar_notify_icon_name());
	asteroidz_tab_bar_node_set_icon(p->node, icon);
	asteroidz_tab_bar_node_set_icon_tint(p->node,
										 bar_notify.count > 0 && !bar_notify.dnd
											 ? config.theme.focus_bg_color
											 : config.theme.fg_color);
	p->text[0] = '\0';
	p->arg = 0;
	/* Icon-only, always: with no label there is nothing to reserve for and
	 * nothing that can reflow. */
	p->fixed_width = 0;
	/* Never filled. The state is the artwork now, and a filled pill made it
	 * WORSE: the icon is tinted with the accent, and the active look fills the
	 * pill with that same accent, so having a notification is precisely when
	 * the bell became invisible. */
	bar_pill_style(p, BAR_LOOK_FLAT);
	mod->npills = 1;
	for (int32_t i = 1; i < BAR_MAX_PILLS; i++)
		bar_pill_release(&mod->pills[i]);
}

/* Rx glyph, plus what the waybar plugin puts beside it: the medication's name
 * when exactly one dose is due, the count when several are, the next dose's
 * time when none is, and nothing at all once the day is done. */
static void bar_module_refresh_medication(BarModule *mod) {
	bar_med_reload();
	/* nothing scheduled at all -- no store, or every dose taken -- collapses
	 * the pill rather than showing a permanently idle glyph */
	/* Never hidden while there is a store to read.
	 *
	 * It used to drop off the bar whenever nothing was due and nothing was
	 * upcoming -- which is the state you are in the moment you take the last
	 * dose of the day, so acting on the module made it vanish. Worse, the
	 * popover is the only way in to the list at all, so a bar with no
	 * medication pill is a bar with no way to reach medication. An indicator
	 * you cannot get to is not an indicator. */
	if (!bar_med.have) {
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
	/* the plugin's own pill artwork, tinted urgent while a dose is actually
	 * due and plain while it is only upcoming */
	char icon[512];
	bar_icon_path(icon, sizeof(icon), "waybar-medication/pill.svg");
	asteroidz_tab_bar_node_set_icon(p->node, icon);
	/* Tinted to READ, not to the urgent colour. A due dose fills the pill
	 * urgent, and tinting the artwork with that same colour paints it in the
	 * exact shade it is sitting on -- the glyph disappears at the one moment
	 * the module is shouting for attention. Identical to what the bell did. */
	if (bar_med.ndue > 0) {
		float tint[4];
		bar_readable_fg(config.theme.urgent_color, tint);
		asteroidz_tab_bar_node_set_icon_tint(p->node, tint);
	} else {
		asteroidz_tab_bar_node_set_icon_tint(p->node, config.theme.fg_color);
	}
	if (bar_med.ndue == 1 && bar_med.due_name[0])
		snprintf(p->text, sizeof(p->text), "%s", bar_med.due_name);
	else if (bar_med.ndue > 1)
		snprintf(p->text, sizeof(p->text), "%d", bar_med.ndue);
	else
		snprintf(p->text, sizeof(p->text), "%s", bar_med.next_time);
	p->arg = 0;
	/* Free-sized: a dose becoming due is a real change worth a relayout, and
	 * pinning to the longest medication name would reserve most of the
	 * section for a pill that is usually just a time. */
	p->fixed_width = 0;
	/* due is the whole point of the module, so it takes the urgent fill the
	 * plugin's pulsing red class stood for */
	bar_pill_style(p, bar_med.ndue > 0 ? BAR_LOOK_URGENT : BAR_LOOK_FLAT);
	mod->npills = 1;
	for (int32_t i = 1; i < BAR_MAX_PILLS; i++)
		bar_pill_release(&mod->pills[i]);
}

/* Discord logo plus the voice state. Hidden entirely while the daemon is not
 * running: an always-present "Offline" pill is noise on a machine where
 * discord-voiced simply is not installed, and the module is opt-in anyway. */
static void bar_module_refresh_discord(BarModule *mod) {
	bar_dv_start();
	/* Hidden only when there is no daemon to run at all.
	 *
	 * An always-present "Offline" pill is noise on a machine where
	 * discord-voiced simply is not installed. But hiding it whenever the
	 * daemon is merely STOPPED made shutting it down a one-way door: no pill,
	 * so no popover, so nothing to start it from again. Installed-but-down is
	 * the one state you can actually act on.
	 *
	 * `linked` counts too, and has to: the daemon now starts LOGGED OUT, so a
	 * perfectly healthy one sits at state OFFLINE from session start. A
	 * running daemon is stronger evidence than an installed binary -- it is
	 * answering us right now -- and hiding the pill there would leave the
	 * Connect action with nothing to open it from. */
	if (bar_dv.state == BAR_DV_OFFLINE && !bar_dv.error[0] && !bar_dv.linked &&
		!bar_dv_daemon_installed()) {
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
	/* Logo, plus the volume plugin's mic-mute artwork as a second icon while
	 * muted. Mute used to be a font glyph prefixed to the label; drawn as art
	 * it matches the size and weight of every other icon in the row, which a
	 * glyph never did. */
	char icon[512], mic[512];
	bar_icon_path(icon, sizeof(icon), "waybar-discord-voice/discord.svg");
	if (bar_dv.muted) {
		bar_icon_path(mic, sizeof(mic), "waybar-volume/mic-mute.svg");
		const char *icons[2] = {icon, mic};
		asteroidz_tab_bar_node_set_icons(p->node, icons, 2);
	} else {
		asteroidz_tab_bar_node_set_icon(p->node, icon);
	}
	/* The logo ships as a solid #000 stencil -- the waybar plugin tints it to
	 * the widget colour and painted as-is it is an invisible black blob on a
	 * dark panel, exactly like the sysinfo glyphs. Tinted by state: the theme
	 * accent while push-to-talk is held, dimmed while muted. */
	float tint[4];
	if (bar_dv.ptt_active)
		memcpy(tint, config.theme.focus_bg_color, sizeof(tint));
	else if (bar_dv.error[0])
		memcpy(tint, config.theme.urgent_color, sizeof(tint));
	else {
		memcpy(tint, config.theme.fg_color, sizeof(tint));
		if (bar_dv.muted)
			tint[3] *= 0.45f;
	}
	asteroidz_tab_bar_node_set_icon_tint(p->node, tint);
	snprintf(p->text, sizeof(p->text), "%s", bar_dv_label());
	p->arg = 0;
	p->fixed_width = 0; /* the channel name changes only when you move rooms */
	/* push-to-talk held reads as the active state -- it is the one thing worth
	 * seeing at a glance mid-conversation; an error outranks it */
	bar_pill_style(p, bar_dv.error[0]     ? BAR_LOOK_URGENT
					  : bar_dv.ptt_active ? BAR_LOOK_ACTIVE
										  : BAR_LOOK_FLAT);
	mod->npills = 1;
	for (int32_t i = 1; i < BAR_MAX_PILLS; i++)
		bar_pill_release(&mod->pills[i]);
}

/* Shield, tinted by connection state. Icon only: the colour IS the reading,
 * and a server hostname is popover material rather than bar material. */
static void bar_module_refresh_vpn(BarModule *mod) {
	BarPill *p = bar_pill_get(mod, 0);
	if (!p) {
		mod->npills = 0;
		return;
	}
	char icon[512];
	bar_icon_path(icon, sizeof(icon),
				  bar_vpn.state == BAR_VPN_CONNECTED
					  ? "waybar-nordvpn/shield.svg"
					  : "waybar-nordvpn/shield-off.svg");
	asteroidz_tab_bar_node_set_icon(p->node, icon);

	float tint[4];
	switch (bar_vpn.state) {
	case BAR_VPN_CONNECTED:
		memcpy(tint, config.theme.focus_bg_color, sizeof(tint));
		break;
	case BAR_VPN_CONNECTING:
		memcpy(tint, bar_load_amber, sizeof(tint));
		break;
	case BAR_VPN_UNAVAILABLE:
		memcpy(tint, config.theme.urgent_color, sizeof(tint));
		break;
	default:
		memcpy(tint, config.theme.fg_color, sizeof(tint));
		tint[3] *= 0.45f; /* an unlit indicator */
		break;
	}
	asteroidz_tab_bar_node_set_icon_tint(p->node, tint);
	p->text[0] = '\0';
	p->arg = 0;
	p->fixed_width = bar_icon_pill_width(p, bar_pill_height());
	bar_pill_style(p, BAR_LOOK_FLAT);
	mod->npills = 1;
	for (int32_t i = 1; i < BAR_MAX_PILLS; i++)
		bar_pill_release(&mod->pills[i]);
}

/* The output this pill's own bar is on, and its mode. Per-monitor by nature:
 * each bar reports ITS OWN output rather than the focused one, so a two-head
 * setup shows each screen what it is running at. */
static void bar_module_refresh_display(BarModule *mod) {
	BarPill *p = bar_pill_get(mod, 0);
	if (!p) {
		mod->npills = 0;
		return;
	}
	/* Icon only, the waybar-display plugin's own artwork. The dimensions used
	 * to ride along here, which was both noise on a bar and misleading: it was
	 * m->m, the monitor's LOGICAL box, so a 1920x1080 panel at scale 0.75
	 * announced itself as 2560x1440 while sitting next to a label that reads
	 * like a mode. The resolution belongs in the popover, where
	 * bar_display_summary already puts it and where a mode picker will
	 * eventually need it. */
	char icon[512];
	bar_icon_path(icon, sizeof(icon), "waybar-display/display.svg");
	asteroidz_tab_bar_node_set_icon(p->node, icon);
	asteroidz_tab_bar_node_set_icon_tint(p->node, config.theme.fg_color);
	p->text[0] = '\0';
	p->arg = 0;
	p->fixed_width = bar_icon_pill_width(p, bar_pill_height());
	bar_pill_style(p, BAR_LOOK_FLAT);
	mod->npills = 1;
	for (int32_t i = 1; i < BAR_MAX_PILLS; i++)
		bar_pill_release(&mod->pills[i]);
}

static bool bar_right_belongs_here(Monitor *m);

static void bar_module_refresh(BarModule *mod) {
	/* A section that is not on this screen renders nothing at all -- no pills,
	 * so the slot is empty and its panel is not drawn either. */
	if (mod->slot == BAR_SLOT_RIGHT && !bar_right_belongs_here(mod->mon)) {
		for (int32_t i = 0; i < BAR_MAX_PILLS; i++)
			bar_pill_release(&mod->pills[i]);
		mod->npills = 0;
		return;
	}
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
	case BAR_MODULE_WEATHER:
		bar_module_refresh_weather(mod);
		break;
	case BAR_MODULE_MEDIA:
		bar_module_refresh_media(mod);
		break;
	case BAR_MODULE_TRAY:
		bar_module_refresh_tray(mod);
		break;
	case BAR_MODULE_VOLUME:
		bar_module_refresh_volume(mod);
		break;
	case BAR_MODULE_NOTIFY:
		bar_module_refresh_notify(mod);
		break;
	case BAR_MODULE_MEDICATION:
		bar_module_refresh_medication(mod);
		break;
	case BAR_MODULE_DISCORD:
		bar_module_refresh_discord(mod);
		break;
	case BAR_MODULE_VPN:
		bar_module_refresh_vpn(mod);
		break;
	case BAR_MODULE_DISPLAY:
		bar_module_refresh_display(mod);
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
		/* The min-width floor exists so a single-glyph LABEL still reads as a
		 * pill. An icon-only pill has no label to protect and is already
		 * exactly as wide as its artwork, so floring it just pads slack around
		 * the icon -- which is the gap this module is trying to control. */
		if (config.bar_pill_min_width > 0 &&
			p->width < config.bar_pill_min_width &&
			!bar_pill_is_icon_only(p))
			p->width = config.bar_pill_min_width;
		/* Where the unused part of a pinned width sits. Centred content splits
		 * it; left-aligned content pushes all of it to the trailing edge. */
		p->slack_lead = p->slack_trail = 0;
		if (p->fixed_width > 0) {
			int32_t natural = asteroidz_tab_bar_node_measure_width(
				p->node, p->text, height);
			int32_t slack = p->width - natural;
			if (slack > 0) {
				if (p->align_left) {
					p->slack_trail = slack;
				} else {
					p->slack_lead = slack / 2;
					p->slack_trail = slack - p->slack_lead;
				}
			}
		}
		asteroidz_tab_bar_node_set_size(p->node, p->width, height);
		asteroidz_tab_bar_node_update(p->node, p->text, scale);
		total += p->width;
		if (i + 1 < mod->npills)
			total += bar_pill_gap(p, &mod->pills[i + 1]);
	}
	mod->width = total;
	(void)scale;
}

/* Gap BETWEEN two modules, decided by the pills that actually touch: the last
 * of one and the first of the next. Measured ink to ink (see
 * bar_gap_between), so a label and a glyph end up the same distance apart as
 * two glyphs.
 *
 * The tray gets its own, larger separation. It is a different KIND of thing --
 * other applications' icons, in a set that changes while you use the desktop --
 * and running it up against the compositor's own readouts made it look like one
 * more of them. */
static int32_t bar_module_gap(BarModule *a, BarModule *b) {
	if (!a || !b || a->npills <= 0 || b->npills <= 0)
		return 0;
	const BarPill *last = &a->pills[a->npills - 1];
	const BarPill *first = &b->pills[0];
	/* two runs of tiles: box to box, at the same spacing the tiles inside a
	 * chip module already use */
	if (bar_pill_is_chip(last) && bar_pill_is_chip(first))
		return config.bar_spacing;
	int32_t base = (a->kind == BAR_MODULE_TRAY || b->kind == BAR_MODULE_TRAY)
					   ? config.bar_tray_spacing
					   : config.bar_module_spacing;
	return bar_gap_between(last, first, base);
}

/* Draw (or hide) the backdrop for one slot. `x`..`x+w` is the extent of the
 * pills it contains; the panel is that grown by panel-padding on each side. */
static void bar_panel_apply(AsteroidzBar *bar, enum bar_slot slot, int32_t x,
							int32_t w, int32_t y, int32_t h, int32_t trim_lead,
							int32_t trim_trail) {
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
	/* Trim the leading/trailing pill's unused reserve before padding, so the
	 * gap from the panel edge to the first thing you can SEE matches the gap
	 * after the last one. Without this a section led by a pinned pill (the
	 * clock, which reserves its widest month) looked padded on one side and
	 * flush on the other -- the padding was symmetric all along, the content
	 * inside it was not. */
	int32_t pad = config.bar_panel_padding;
	int32_t px = x + trim_lead - pad, py = y;
	int32_t pw = w - trim_lead - trim_trail + 2 * pad, ph = h;
	if (pw < 2 * pad)
		pw = 2 * pad;
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
		/* Grown on every side, not drawn at the panel's own box.
		 *
		 * A shadow node the same size and in the same place as the panel is
		 * entirely BEHIND it: the falloff has nowhere outside the panel to
		 * occupy, so an opaque panel hides all of it and the shadow reads as
		 * not being drawn at all. Every other shadow here already knows this
		 * -- a window's box is grown by `delta` a side (see the comment in
		 * animation/client.h, which documents this exact bug), and an
		 * overview cell's by the blur sigma -- the bar's was the one that
		 * did not.
		 *
		 * The corner radius grows with the box for the same reason a window's
		 * does: the shadow's rounding has to follow the panel's outline at
		 * its new, larger extent, or the corners square off.
		 *
		 * `shadow.position` shifts it after that, so the configured downward
		 * offset lands under the panel the way it does under a window. */
		int32_t delta = config.bar_panel_shadow_size;
		int32_t sx = px - delta;
		int32_t sy = py - delta + delta / 3;
		int32_t sw = pw + 2 * delta, sh = ph + 2 * delta;
		int32_t sradius = radius + delta;
		if (!panel->shadow)
			panel->shadow = wlr_scene_shadow_create(
				bar->panel_tree, sw, sh, sradius, config.shadows_blur,
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
		x += p->width;
		if (i + 1 < mod->npills)
			x += bar_pill_gap(p, &mod->pills[i + 1]);
	}
	return x;
}

/* Claw back up to `want` pixels from the flexible pills of `slot`
 * (BAR_SLOT_COUNT = any slot), narrowing them in place and updating the
 * module and slot widths as it goes. Flexible means "carries ellipsisable
 * text": the window title and the now-playing string, both of which would
 * rather be cut short than shove a neighbour out of position. Returns how
 * much it actually recovered, which is less than `want` once every flexible
 * pill has reached the floor. */
static int32_t bar_shrink_flexible(AsteroidzBar *bar, int32_t slot,
								   int32_t want, int32_t *slot_w,
								   int32_t pill_h, float scale) {
	/* below this a pill shows an ellipsis and nothing else, so stop there and
	 * let the caller decide what to drop instead */
	int32_t floor_w = config.bar_height * 2;
	int32_t got = 0;
	for (int32_t i = 0; i < bar->nmodules && want > 0; i++) {
		BarModule *mod = &bar->modules[i];
		if (slot != BAR_SLOT_COUNT && mod->slot != slot)
			continue;
		for (int32_t j = 0; j < mod->npills && want > 0; j++) {
			BarPill *fp = &mod->pills[j];
			if (!fp->used || !fp->flexible)
				continue;
			int32_t shrink = fp->width - floor_w;
			if (shrink > want)
				shrink = want;
			if (shrink <= 0)
				continue;
			fp->width -= shrink;
			fp->fixed_width = fp->width;
			asteroidz_tab_bar_node_set_size(fp->node, fp->width, pill_h);
			asteroidz_tab_bar_node_update(fp->node, fp->text, scale);
			mod->width -= shrink;
			slot_w[mod->slot] -= shrink;
			want -= shrink;
			got += shrink;
		}
	}
	return got;
}

/* Shed order once the strip cannot fit everything, lowest first.
 *
 * A bar with a fixed module list and no width budget is choosing what to show
 * by accident -- whatever happens to be last simply falls off the edge. The
 * TRAY makes that unworkable rather than merely untidy: its width is however
 * many applications happen to be running, so on a narrow output an unrelated
 * program starting could push a module out. Deciding on purpose means saying
 * what the bar is FOR, which is what this order is: things you interact with
 * outlast things you glance at, and ambient readouts go first.
 *
 * Tags and tray are never shed. Both are the bar's reason to exist -- one is
 * how you navigate, the other is how applications reach you -- and a tray that
 * disappears to make room for a weather pill would be absurd. */
#define BAR_SHED_NEVER 1000

static int32_t bar_module_shed_rank(enum bar_module_kind k) {
	switch (k) {
	case BAR_MODULE_WEATHER:    return 10;
	case BAR_MODULE_MEDIA:      return 20;
	case BAR_MODULE_MEDICATION: return 30;
	case BAR_MODULE_DISCORD:    return 40;
	case BAR_MODULE_TITLE:      return 50;
	case BAR_MODULE_IDLE:       return 60;
	case BAR_MODULE_DISPLAY:    return 70;
	case BAR_MODULE_VPN:        return 80;
	case BAR_MODULE_NETWORK:    return 90;
	case BAR_MODULE_MEMORY:     return 100;
	case BAR_MODULE_CPU:        return 110;
	case BAR_MODULE_NOTIFY:     return 120;
	case BAR_MODULE_VOLUME:     return 130;
	case BAR_MODULE_LAYOUT:     return 140;
	case BAR_MODULE_CLOCK:      return 150;
	case BAR_MODULE_TAGS:
	case BAR_MODULE_TRAY:       return BAR_SHED_NEVER;
	default:                    return 200;
	}
}

/* Take a module off the strip for this pass. Its nodes are disabled rather
 * than destroyed: bar_place_module re-enables whatever it places, so a module
 * comes back by itself the moment the width is there again. Destroying would
 * churn scene nodes every metrics tick on an output that is merely narrow. */
static void bar_module_hide(BarModule *mod) {
	for (int32_t i = 0; i < mod->npills; i++)
		if (mod->pills[i].used)
			asteroidz_tab_bar_node_set_enabled(mod->pills[i].node, false);
	mod->width = 0;
}

/* Slot widths from whatever is currently visible, gaps included. Recomputed
 * from scratch after each drop rather than patched, because removing a module
 * also removes one inter-module gap, and which gap depends on where in the
 * slot it sat. */
static void bar_measure_slots(AsteroidzBar *bar, int32_t *slot_w) {
	BarModule *last[BAR_SLOT_COUNT] = {NULL, NULL, NULL};
	for (int32_t s = 0; s < BAR_SLOT_COUNT; s++)
		slot_w[s] = 0;
	for (int32_t i = 0; i < bar->nmodules; i++) {
		BarModule *mod = &bar->modules[i];
		if (mod->width <= 0)
			continue;
		if (last[mod->slot])
			slot_w[mod->slot] += bar_module_gap(last[mod->slot], mod);
		slot_w[mod->slot] += mod->width;
		last[mod->slot] = mod;
	}
}

/* What the three slots need, including the gaps between them. */
static int32_t bar_needed_width(const int32_t *slot_w) {
	int32_t needed = 0, slots = 0;
	for (int32_t s = 0; s < BAR_SLOT_COUNT; s++)
		if (slot_w[s] > 0) {
			needed += slot_w[s];
			slots++;
		}
	return needed + (slots > 1 ? (slots - 1) * config.bar_spacing : 0);
}

/* Recompute the whole strip: refresh every module's content, measure, then
 * place the three slots. Left packs from the left inset, right packs to the
 * right inset, centre is centred on the monitor and only moved aside if it
 * would collide with a neighbour. */
static void bar_layout(Monitor *m) {
	AsteroidzBar *bar = m->bar;
	if (!bar || !bar->tree)
		return;

	/* Two heights: the strip (what the panel fills and what space reservation
	 * is measured against) and the pill row inset inside it. */
	int32_t height = config.bar_height;
	int32_t pill_h = bar_pill_height();
	float scale = m->wlr_output ? m->wlr_output->scale : 1.0f;
	if (scale <= 0.0f)
		scale = 1.0f;

	int32_t slot_w[BAR_SLOT_COUNT] = {0, 0, 0};
	BarModule *slot_last[BAR_SLOT_COUNT] = {NULL, NULL, NULL};
	for (int32_t i = 0; i < bar->nmodules; i++) {
		BarModule *mod = &bar->modules[i];
		bar_module_refresh(mod);
		bar_module_measure(mod, pill_h, scale);
		if (mod->width > 0) {
			if (slot_last[mod->slot])
				slot_w[mod->slot] += bar_module_gap(slot_last[mod->slot], mod);
			slot_w[mod->slot] += mod->width;
			slot_last[mod->slot] = mod;
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

	/* Keep the centre slot from overlapping its neighbours.
	 *
	 * Order matters and used to be wrong: the left clamp ran first and the
	 * right clamp second, so on an output too narrow for all three slots the
	 * right clamp shoved the centre back LEFT, straight through the title
	 * pill -- the clock and the window title drew on top of each other.
	 *
	 * The left slot is the flexible one (its title pill is pinned to
	 * title-width, which is a preference, not a requirement), so resolve the
	 * conflict by shrinking that pill until the centre fits, rather than by
	 * moving the centre somewhere it cannot go. */
	int32_t left_end = left + slot_w[BAR_SLOT_LEFT];
	/* How much wider than the output the three slots are, once their inter-
	 * slot gaps are counted. Measured against the TOTAL rather than just the
	 * centre's natural position: the right slot is anchored to the right
	 * edge, so a left slot that is merely "past centre" is fine while one
	 * that collides with the right slot is not. */
	int32_t avail = m->m.width - 2 * inset;
	int32_t needed = bar_needed_width(slot_w);
	int32_t overflow = needed - avail;
	if (overflow > 0) {
		/* Take it out of every flexible pill (title, now-playing) before
		 * considering dropping a whole slot -- hiding the centre section
		 * because a window title wanted 320px is the wrong trade, and it
		 * took the clock and the idle toggle with it. */
		bar_shrink_flexible(bar, BAR_SLOT_COUNT, overflow, slot_w, pill_h,
							scale);
		bar_measure_slots(bar, slot_w);
		overflow = bar_needed_width(slot_w) - avail;

		/* Every flexible pill is at its floor and it still does not fit, so
		 * something has to go. Drop whole modules, least important first,
		 * until it does -- deciding by priority rather than letting the last
		 * one laid out run off the edge of the output. */
		for (int32_t guard = 0; overflow > 0 && guard < BAR_MAX_MODULES;
			 guard++) {
			BarModule *victim = NULL;
			for (int32_t i = 0; i < bar->nmodules; i++) {
				BarModule *mod = &bar->modules[i];
				if (mod->width <= 0 ||
					bar_module_shed_rank(mod->kind) >= BAR_SHED_NEVER)
					continue;
				if (!victim || bar_module_shed_rank(mod->kind) <
								   bar_module_shed_rank(victim->kind))
					victim = mod;
			}
			if (!victim)
				break; /* only unsheddable modules left: they overflow */
			bar_module_hide(victim);
			bar_measure_slots(bar, slot_w);
			overflow = bar_needed_width(slot_w) - avail;
		}
		left_end = left + slot_w[BAR_SLOT_LEFT];
		cursor[BAR_SLOT_RIGHT] = right - slot_w[BAR_SLOT_RIGHT];
		cursor[BAR_SLOT_CENTER] =
			m->m.x + (m->m.width - slot_w[BAR_SLOT_CENTER]) / 2;
	}

	/* Yield to the centred slot before it has to move.
	 *
	 * The whole point of the centre section is that it is centred: a clock
	 * that drifts with the length of the focused window's title is worse than
	 * a title that ellipsises a little sooner. So once the left slot reaches
	 * where the centre wants to start, take the difference out of the left
	 * slot's flexible pills (the title) rather than pushing the centre along.
	 * Same on the other side for a long now-playing string in the centre. */
	if (slot_w[BAR_SLOT_CENTER] > 0) {
		int32_t centre_x =
			m->m.x + (m->m.width - slot_w[BAR_SLOT_CENTER]) / 2;
		int32_t encroach =
			(left_end + config.bar_spacing) - centre_x;
		if (encroach > 0 && slot_w[BAR_SLOT_LEFT] > 0) {
			bar_shrink_flexible(bar, BAR_SLOT_LEFT, encroach, slot_w, pill_h,
								scale);
			left_end = left + slot_w[BAR_SLOT_LEFT];
			centre_x = m->m.x + (m->m.width - slot_w[BAR_SLOT_CENTER]) / 2;
		}
		int32_t past_right = (centre_x + slot_w[BAR_SLOT_CENTER] +
							  config.bar_spacing) -
							 cursor[BAR_SLOT_RIGHT];
		if (past_right > 0 && slot_w[BAR_SLOT_RIGHT] > 0)
			bar_shrink_flexible(bar, BAR_SLOT_CENTER, past_right, slot_w,
								pill_h, scale);
	}

	/* Fit the centre into the gap the anchored slots leave, or drop it.
	 *
	 * The left slot is anchored left and the right slot right; only the
	 * centre has anywhere to go. Clamping it against one neighbour and then
	 * the other just moves the collision around -- on an output too narrow
	 * for the configured modules the centre ended up 107px past the right
	 * edge with the right slot overlapping the left. Priority is
	 * workspaces > status > clock, so when the gap cannot hold the centre it
	 * is hidden rather than drawn through its neighbours. */
	int32_t gap_start = slot_w[BAR_SLOT_LEFT] > 0
							? left_end + config.bar_spacing
							: m->m.x + inset;
	int32_t gap_end = slot_w[BAR_SLOT_RIGHT] > 0
						  ? cursor[BAR_SLOT_RIGHT] - config.bar_spacing
						  : m->m.x + m->m.width - inset;
	bool centre_fits = slot_w[BAR_SLOT_CENTER] > 0 &&
					   gap_end - gap_start >= slot_w[BAR_SLOT_CENTER];
	if (centre_fits) {
		cursor[BAR_SLOT_CENTER] =
			m->m.x + (m->m.width - slot_w[BAR_SLOT_CENTER]) / 2;
		if (cursor[BAR_SLOT_CENTER] < gap_start)
			cursor[BAR_SLOT_CENTER] = gap_start;
		if (cursor[BAR_SLOT_CENTER] + slot_w[BAR_SLOT_CENTER] > gap_end)
			cursor[BAR_SLOT_CENTER] = gap_end - slot_w[BAR_SLOT_CENTER];
	} else if (slot_w[BAR_SLOT_CENTER] > 0) {
		/* no room: hide every centre pill and collapse the slot so its panel
		 * is not drawn either */
		for (int32_t i = 0; i < bar->nmodules; i++) {
			BarModule *mod = &bar->modules[i];
			if (mod->slot != BAR_SLOT_CENTER)
				continue;
			for (int32_t j = 0; j < mod->npills; j++)
				if (mod->pills[j].used)
					asteroidz_tab_bar_node_set_enabled(mod->pills[j].node,
													   false);
			mod->width = 0;
		}
		slot_w[BAR_SLOT_CENTER] = 0;
	}

	int32_t slot_start[BAR_SLOT_COUNT];
	for (int32_t s = 0; s < BAR_SLOT_COUNT; s++)
		slot_start[s] = cursor[s];

	/* The pill row is centred in the strip, so a filled chip reads as inset in
	 * the panel rather than spanning it from edge to edge. */
	int32_t pill_y = y + (height - pill_h) / 2;
	BarModule *placed_last[BAR_SLOT_COUNT] = {NULL, NULL, NULL};
	for (int32_t i = 0; i < bar->nmodules; i++) {
		BarModule *mod = &bar->modules[i];
		if (mod->width <= 0)
			continue;
		if (placed_last[mod->slot])
			cursor[mod->slot] += bar_module_gap(placed_last[mod->slot], mod);
		cursor[mod->slot] = bar_place_module(mod, cursor[mod->slot], pill_y);
		placed_last[mod->slot] = mod;
	}

	/* One backdrop per non-empty slot. The bar itself paints nothing, so
	 * these three panels are the whole visible surface. They span the full
	 * strip height; the pills inside them are the inset row. */
	for (int32_t s = 0; s < BAR_SLOT_COUNT; s++) {
		int32_t w = slot_w[s];
		/* the outermost pills actually drawn in this slot, whose unused
		 * reserve is what makes the section look off-centre */
		BarPill *lead = NULL, *trail = NULL;
		for (int32_t i = 0; i < bar->nmodules; i++) {
			BarModule *mod = &bar->modules[i];
			if (mod->slot != s || mod->width <= 0)
				continue;
			for (int32_t j = 0; j < mod->npills; j++) {
				BarPill *pp = &mod->pills[j];
				if (!pp->used)
					continue;
				if (!lead)
					lead = pp;
				trail = pp;
			}
		}
		/* Trim the end pills' INK inset as well as their unused reserve, so
		 * `panel.padding` is the distance to what you can see rather than to a
		 * pill's box.
		 *
		 * The two differ by kind, which made a section's own two ends
		 * disagree: measured, a centre panel led by the clock and ended by an
		 * icon read 14px on the left and 6px on the right, because a labelled
		 * pill carries `pill-padding` inside its box and artwork carries
		 * nothing. Chips are exempt for the same reason they are exempt from
		 * the module gap -- a chip's background is an edge you can see, so its
		 * padding is not slack. */
		bar_panel_apply(bar, (enum bar_slot)s, slot_start[s], w, y, height,
						lead ? lead->slack_lead + bar_pill_ink_inset(lead) : 0,
						trail ? trail->slack_trail + bar_pill_ink_inset(trail)
							  : 0);
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
	/* Whether this is the focused monitor. Only the right-section rule reads
	 * it today, and only in "focused" mode -- but leaving it out means the
	 * monitor LOSING focus keeps a stale section: its own content did not
	 * change, so its digest did not either, so it never redrew. */
	bool focused = (m == selmon);
	bar_hash(&h, &focused, sizeof(focused));
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
		case BAR_MODULE_MEDIA:
			bar_hash_str(&h, bar_media.title);
			bar_hash_str(&h, bar_media.artist);
			bar_hash(&h, &bar_media.playing, sizeof(bar_media.playing));
			bar_hash(&h, &bar_media.have, sizeof(bar_media.have));
			/* the visualiser's DRAWN levels, quantised the same way the icon
			 * cache key is: a frame that moved gets a redraw, a still one
			 * costs nothing */
			bar_hash(&h, &bar_viz.silent, sizeof(bar_viz.silent));
			for (int32_t b = 0; b < config.bar_media_bars; b++) {
				int32_t q = (int32_t)(bar_viz.drawn[b] * 20.0);
				bar_hash(&h, &q, sizeof(q));
			}
			break;
		case BAR_MODULE_WEATHER:
			bar_hash(&h, &bar_weather.temp_c, sizeof(bar_weather.temp_c));
			bar_hash(&h, &bar_weather.code, sizeof(bar_weather.code));
			bar_hash(&h, &bar_weather.is_day, sizeof(bar_weather.is_day));
			bar_hash(&h, &bar_weather.valid, sizeof(bar_weather.valid));
			break;
		case BAR_MODULE_CPU:
			bar_hash(&h, &bar_metrics.cpu_pct, sizeof(bar_metrics.cpu_pct));
			break;
		case BAR_MODULE_MEMORY:
			bar_hash(&h, &bar_metrics.mem_pct, sizeof(bar_metrics.mem_pct));
			break;
		case BAR_MODULE_NETWORK: {
			/* hash the TIERS, not the rates: the indicator only changes when a
			 * rate crosses a band, and the rate itself jitters every sample.
			 * Hashing the raw doubles would relayout the bar twice a second for
			 * artwork that did not change. */
			int32_t ut = bar_metrics.link_up ? bar_net_tier(bar_metrics.tx_rate)
											 : BAR_NET_TIERS - 1;
			int32_t dt = bar_metrics.link_up ? bar_net_tier(bar_metrics.rx_rate)
											 : BAR_NET_TIERS - 1;
			bar_hash(&h, &ut, sizeof(ut));
			bar_hash(&h, &dt, sizeof(dt));
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
		case BAR_MODULE_VPN:
			bar_hash(&h, &bar_vpn.state, sizeof(bar_vpn.state));
			break;
		case BAR_MODULE_DISPLAY: {
			int32_t hz = bar_display_hz(m);
			bar_hash(&h, &m->m.width, sizeof(m->m.width));
			bar_hash(&h, &m->m.height, sizeof(m->m.height));
			bar_hash(&h, &hz, sizeof(hz));
			bar_hash(&h, &m->hdr, sizeof(m->hdr));
			break;
		}
		case BAR_MODULE_DISCORD:
			bar_hash(&h, &bar_dv.state, sizeof(bar_dv.state));
			bar_hash(&h, &bar_dv.muted, sizeof(bar_dv.muted));
			bar_hash(&h, &bar_dv.ptt_active, sizeof(bar_dv.ptt_active));
			bar_hash_str(&h, bar_dv.channel_name);
			bar_hash_str(&h, bar_dv.error);
			break;
		case BAR_MODULE_MEDICATION:
			bar_hash(&h, &bar_med.ndue, sizeof(bar_med.ndue));
			bar_hash_str(&h, bar_med.next_time);
			bar_hash_str(&h, bar_med.due_name);
			bar_hash(&h, &bar_med.have, sizeof(bar_med.have));
			break;
		case BAR_MODULE_NOTIFY:
			bar_hash(&h, &bar_notify.count, sizeof(bar_notify.count));
			bar_hash(&h, &bar_notify.dnd, sizeof(bar_notify.dnd));
			bar_hash(&h, &bar_notify.inhibited, sizeof(bar_notify.inhibited));
			break;
		case BAR_MODULE_VOLUME:
			bar_hash(&h, &bar_volume.pct, sizeof(bar_volume.pct));
			bar_hash(&h, &bar_volume.muted, sizeof(bar_volume.muted));
			bar_hash(&h, &bar_volume.have, sizeof(bar_volume.have));
			break;
		case BAR_MODULE_TRAY:
			/* Everything a tray pill draws: which items exist, in what order,
			 * with which artwork and status. Without this the digest gate
			 * swallowed the bar_update_all() the bus callbacks fire, and an
			 * item appearing or swapping its icon only showed up on the next
			 * unrelated arrange. */
			for (int32_t t = 0; t < bar_tray_nitems; t++) {
				if (!bar_tray_items[t].used)
					continue;
				bar_hash_str(&h, bar_tray_items[t].service);
				bar_hash_str(&h, bar_tray_items[t].icon_key);
				bar_hash_str(&h, bar_tray_items[t].status);
			}
			break;
		case BAR_MODULE_TAGS: {
			bar_hash(&h, &config.bar_show_all_tags,
					 sizeof(config.bar_show_all_tags));
			bar_hash(&h, &config.bar_min_tags, sizeof(config.bar_min_tags));
			/* custom tag names can change without any mask changing */
			for (uint32_t t = 1; t <= LENGTH(tags); t++) {
				char name[64];
				tag_display_name(m, t, name, sizeof(name));
				bar_hash_str(&h, name);
				const char *ic[ASTEROIDZ_TAB_MAX_ICONS];
				int32_t ni = config.bar_tag_icons > 0
								 ? bar_tag_app_icons(m, t, ic,
													 config.bar_tag_icons)
								 : 0;
				for (int32_t k = 0; k < ni; k++)
					bar_hash_str(&h, ic[k]);
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
/* Land just PAST the boundary, not on it.
 *
 * The delay is computed from CLOCK_REALTIME but slept on the event loop's
 * CLOCK_MONOTONIC timerfd, and NTP slews the two apart by up to ~0.5ms per
 * second. Aiming exactly at the boundary therefore sits on a knife edge:
 * measured headless, every tick landed at ms=0 of its second, which is one
 * slewed millisecond away from landing at ms=999 of the PREVIOUS one and
 * formatting a second that has already been shown. A few milliseconds of
 * margin is invisible and takes the edge away entirely. */
#define BAR_CLOCK_MARGIN_MS 5

static int32_t bar_clock_next_delay_ms(void) {
	bool seconds = strstr(config.bar_clock_format, "%S") != NULL ||
				   strstr(config.bar_clock_format, "%T") != NULL ||
				   strstr(config.bar_clock_format, "%r") != NULL;
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	int32_t ms_into_second = (int32_t)(ts.tv_nsec / 1000000);
	if (seconds)
		return 1000 - ms_into_second + BAR_CLOCK_MARGIN_MS;
	time_t now = ts.tv_sec;
	struct tm tm;
	localtime_r(&now, &tm);
	return (60 - tm.tm_sec) * 1000 - ms_into_second + BAR_CLOCK_MARGIN_MS;
}

static int bar_clock_tick(void *data) {
	(void)data;
	/* Say so when a second was skipped.
	 *
	 * The tick re-arms to the next boundary from wherever it actually ran, so
	 * it can never drift -- but it also cannot show a second it was not woken
	 * inside of. If the event loop is blocked past one, the displayed time
	 * jumps, and from the outside that is indistinguishable from a broken
	 * timer. Recording the real gap makes it a fact in the log with a
	 * timestamp to correlate against, instead of something to guess at later:
	 * the tick itself measures ~1.3ms, so a gap this size is always something
	 * ELSE holding the loop. */
	static time_t last_sec = 0;
	time_t now = time(NULL);
	if (last_sec && now > last_sec + 1)
		wlr_log(WLR_INFO, "bar: clock tick %lds late (skipped %ld second%s)",
				(long)(now - last_sec - 1), (long)(now - last_sec - 1),
				now - last_sec == 2 ? "" : "s");
	last_sec = now;

	bar_update_all();
	if (bar_clock_timer)
		wl_event_source_timer_update(bar_clock_timer,
									 bar_clock_next_delay_ms());
	return 0;
}

static int bar_metrics_tick(void *data) {
	bar_tray_retry_icons();
	bar_vpn_poll();
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

	bool want_weather = false;
	if (config.bar_enable) {
		wl_list_for_each(m, &mons, link) {
			if (!m->bar)
				continue;
			for (int32_t i = 0; i < m->bar->nmodules; i++)
				if (m->bar->modules[i].kind == BAR_MODULE_WEATHER)
					want_weather = true;
		}
	}
	bool want_media = false;
	if (config.bar_enable) {
		wl_list_for_each(m, &mons, link) {
			if (!m->bar)
				continue;
			for (int32_t i = 0; i < m->bar->nmodules; i++)
				if (m->bar->modules[i].kind == BAR_MODULE_MEDIA)
					want_media = true;
		}
	}
	if (want_media && !bar_media_timer) {
		bar_media_timer =
			wl_event_loop_add_timer(event_loop, bar_media_tick, NULL);
		if (bar_media_timer) {
			bar_media_poll();
			wl_event_source_timer_update(bar_media_timer, 2000);
		}
	} else if (!want_media && bar_media_timer) {
		wl_event_source_remove(bar_media_timer);
		bar_media_timer = NULL;
	}

	if (want_weather && !bar_weather_timer) {
		bar_weather_timer =
			wl_event_loop_add_timer(event_loop, bar_weather_tick, NULL);
		if (bar_weather_timer) {
			/* fetch straight away rather than after a full interval, or the
			 * pill sits on "--" for 15 minutes after every start */
			bar_weather_fetch();
			wl_event_source_timer_update(
				bar_weather_timer, config.bar_weather_interval * 60 * 1000);
		}
	} else if (!want_weather && bar_weather_timer) {
		wl_event_source_remove(bar_weather_timer);
		bar_weather_timer = NULL;
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
			/* Name every module being dropped, not just the first. This
			 * returns -- abandoning the rest of THIS section and every section
			 * after it -- so a bare "ignoring 'medication'" understates the
			 * damage by however many modules follow. */
			fprintf(stderr,
					"\033[1m\033[33m[WARN]:\033[0m bar: more than %d modules "
					"across all sections; DROPPING '%s' and everything after "
					"it in the list\n",
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
	/* the popover holds this monitor and hangs off this bar's geometry, so it
	 * cannot outlive either -- an output being unplugged with a menu up left
	 * it pointing at a freed Monitor */
	if (bar_popover.mon == m)
		bar_popover_close();
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

/* Does this monitor draw the right-hand section?
 *
 * The left and centre sections are per-monitor by nature: tags and the focused
 * window's title describe the screen you are looking at. The right one is
 * machine state -- one tray, one clock, one battery -- and on a multi-head
 * desk that is the same row of pills repeated on every screen, competing for
 * the same glance.
 *
 *   ""/"all"    every monitor (the default, and what this always did)
 *   "focused"   whichever monitor has focus, following it as it moves
 *   "<output>"  that output alone
 *
 * Asked on every refresh rather than when the bar is built, because "focused"
 * changes under a bar that already exists. The modules are always constructed;
 * the ones that do not belong here simply render nothing, which also means
 * their state stays warm when focus moves rather than being torn down and
 * rebuilt on every hop between screens.
 *
 * A name that no CONNECTED output matches falls back to the focused monitor
 * rather than disappearing: unplugging a screen must not take the tray and the
 * clock off the desk entirely, with no way to get them back short of editing
 * the config. */
static bool bar_right_belongs_here(Monitor *m) {
	const char *want = config.bar_modules_right_monitor;
	if (!want[0] || !strcmp(want, "all"))
		return true;
	if (!m || !m->wlr_output || !m->wlr_output->name)
		return false;
	if (!strcmp(want, "focused") || !strcmp(want, "focus"))
		return m == selmon;
	if (!strcmp(want, m->wlr_output->name))
		return true;

	Monitor *fallback = NULL, *other = NULL;
	wl_list_for_each(other, &mons, link) {
		if (!other->wlr_output || !other->wlr_output->enabled)
			continue;
		if (other->wlr_output->name && !strcmp(want, other->wlr_output->name))
			return false; /* the output it asked for is here; not us */
		if (!fallback)
			fallback = other;
	}
	/* The focused monitor, not the first in the list: `mons` is in the order
	 * outputs were added, which is not something anyone can predict from
	 * looking at their desk. Falling back to the screen being used puts the
	 * tray where the eyes already are. */
	if (selmon && selmon->wlr_output && selmon->wlr_output->enabled)
		fallback = selmon;
	return fallback == m;
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
	case BAR_MODULE_MEDIA:
		if (button == BTN_LEFT && session_bus && bar_media.player[0]) {
			/* which pill was hit -- the track itself still toggles, so the
			 * long-standing click-anywhere-to-pause habit keeps working */
			const char *method = p->arg == BAR_MEDIA_PREV	  ? "Previous"
								 : p->arg == BAR_MEDIA_NEXT	  ? "Next"
															  : "PlayPause";
			/* fire-and-forget: the reply carries nothing we need, and the
			 * next poll picks up the new state anyway */
			sd_bus_call_method_async(session_bus, NULL, bar_media.player,
									 "/org/mpris/MediaPlayer2",
									 "org.mpris.MediaPlayer2.Player", method,
									 NULL, NULL, NULL);
			return true;
		}
		break;
	case BAR_MODULE_IDLE:
		if (button == BTN_LEFT) {
			toggle_idle_inhibit(&(Arg){.i = -1});
			return true;
		}
		break;
	case BAR_MODULE_DISPLAY:
		if (button == BTN_LEFT) {
			bar_popover_open_outputs(m, p->node->last_x + p->width / 2);
			return true;
		}
		break;
	case BAR_MODULE_VPN:
		if (button == BTN_LEFT) {
			bar_popover_open_vpn(m, p->node->last_x + p->width / 2);
			return true;
		}
		break;
	case BAR_MODULE_DISCORD:
		if (button == BTN_LEFT) {
			bar_popover_open_voice(m, p->node->last_x + p->width / 2);
			return true;
		}
		if (button == BTN_RIGHT && bar_dv.state == BAR_DV_CONNECTED) {
			bar_dv_send("{\"cmd\":\"mute\"}");
			return true;
		}
		break;
	case BAR_MODULE_MEDICATION:
		if (button == BTN_LEFT) {
			bar_popover_open_meds(m, p->node->last_x + p->width / 2);
			return true;
		}
		break;
	case BAR_MODULE_WEATHER:
		if (button == BTN_LEFT) {
			/* The pill is one glyph and a temperature; the forecast the
			 * request already returns lives here, as the waybar plugin's
			 * popover did. */
			bar_popover_open_weather(m, p->node->last_x + p->width / 2);
			return true;
		}
		break;
	case BAR_MODULE_CPU:
	case BAR_MODULE_MEMORY:
	case BAR_MODULE_NETWORK:
		if (button == BTN_LEFT) {
			/* One panel for all three: they are three views of one machine,
			 * and the pills are numberless by design -- this is where the
			 * figures they stand for actually live. */
			bar_popover_open_sysinfo(m, p->node->last_x + p->width / 2);
			return true;
		}
		break;
	case BAR_MODULE_NOTIFY:
		if (button == BTN_LEFT) {
			bar_notify_call("ToggleVisibility");
			return true;
		}
		if (button == BTN_RIGHT) {
			bar_notify_call("ToggleDnd");
			return true;
		}
		break;
	case BAR_MODULE_VOLUME:
		if (button == BTN_LEFT) {
			bar_volume_set(0); /* toggle mute */
			return true;
		}
		if (button == BTN_RIGHT) {
			/* the output picker hangs from the middle of this pill;
			 * last_x is where bar_place_module put it */
			bar_popover_open_sinks(m, p->node->last_x + p->width / 2);
			return true;
		}
		break;
	case BAR_MODULE_TRAY: {
		if (p->arg >= (uint32_t)bar_tray_nitems)
			break;
		BarTrayItem *it = &bar_tray_items[p->arg];
		if (!it->used)
			break;
		/* The spec passes the click's SCREEN position so the item can put its
		 * own window next to the pill it was launched from. */
		int32_t x = (int32_t)cursor->x, y = (int32_t)cursor->y;
		if (button == BTN_LEFT) {
			bar_tray_activate(it, "Activate", x, y);
			return true;
		}
		if (button == BTN_RIGHT) {
			/* The item's own context menu when it publishes one, which is what
			 * a right-click on a tray icon means everywhere else. Items with
			 * no Menu property still fall back to SecondaryActivate -- some
			 * applications wire that to "open my menu" and ship no DBusMenu at
			 * all. */
			if (it->menu_path[0])
				bar_popover_open_menu(m, p->node->last_x + p->width / 2,
									  it->service, it->menu_path, 0);
			else
				bar_tray_activate(it, "SecondaryActivate", x, y);
			return true;
		}
		if (button == BTN_MIDDLE) {
			bar_tray_activate(it, "SecondaryActivate", x, y);
			return true;
		}
		break;
	}
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

/* Dispatch a scroll over a bar pill. `delta` is the discrete notch count,
 * negative for up/left. Returns true when the scroll was consumed, so it is
 * not also forwarded to whatever client happens to be under the bar.
 *
 * Separate from bar_handle_node_click because a scroll is not a click: the
 * volume pill mutes on click but steps on scroll, and the tray spec has its
 * own Scroll method carrying an orientation. */
static bool bar_handle_node_scroll(AsteroidzNodeData *hit, int32_t delta,
								   bool horizontal) {
	BarPill *p = hit ? (BarPill *)hit->node_data : NULL;
	if (!p || !p->used || !p->module || delta == 0)
		return false;
	switch (p->module->kind) {
	case BAR_MODULE_VOLUME:
		/* scroll up raises. delta is negative for "up", so negate it. */
		bar_volume_set(-delta * config.bar_volume_step);
		return true;
	case BAR_MODULE_TRAY: {
		if (p->arg >= (uint32_t)bar_tray_nitems)
			break;
		BarTrayItem *it = &bar_tray_items[p->arg];
		if (!it->used)
			break;
		/* The item decides what a scroll means -- volume for a mixer applet,
		 * workspace for a pager -- so pass the notch count and the axis
		 * through rather than interpreting them here. */
		bar_tray_scroll(it, delta, horizontal ? "horizontal" : "vertical");
		return true;
	}
	default:
		break;
	}
	return false;
}

/* Resolve the pill under (x, y) and give it the scroll. Kept here rather than
 * in axisnotify so the caller needs no #ifdef and no knowledge of the scene
 * layout -- same arrangement as bar_reserve and bar_handle_node_click.
 *
 * Takes the raw axis event rather than a notch count because the two input
 * families report scrolling completely differently: a wheel sends discrete
 * clicks (delta_discrete, in 120ths of a notch), while a trackpad or a
 * high-resolution wheel sends a stream of small continuous deltas and no
 * discrete value at all. Keying off delta_discrete alone made the pills
 * unscrollable on a trackpad, so continuous motion is accumulated here until
 * it adds up to a notch.
 *
 * The lookup mirrors handle_buttonpress: per-layer, top down, skipping the
 * fade-out and screenshot layers whose node data belongs to freed clients. */
#define BAR_SCROLL_UNIT 10.0 /* continuous delta that counts as one notch */

static bool bar_scroll_at(double x, double y, double delta,
						  double delta_discrete, bool horizontal) {
	if (!config.bar_enable)
		return false;

	int32_t notches = 0;
	if (delta_discrete != 0.0) {
		/* 120ths of a notch, the high-resolution wheel convention */
		notches = (int32_t)(delta_discrete / 120.0);
		if (notches == 0)
			notches = delta_discrete > 0 ? 1 : -1;
	} else if (delta != 0.0) {
		/* Accumulated across events, and reset on a direction change so a
		 * flick back the other way responds immediately instead of first
		 * having to cancel out what came before. */
		static double accum = 0.0;
		if ((accum > 0.0) != (delta > 0.0))
			accum = 0.0;
		accum += delta;
		notches = (int32_t)(accum / BAR_SCROLL_UNIT);
		if (notches != 0)
			accum -= notches * BAR_SCROLL_UNIT;
	}
	if (notches == 0)
		return false;
	struct wlr_scene_node *node = NULL;
	for (int32_t li = NUM_LAYERS - 1; li >= 0 && !node; li--) {
		if (li == LyrFadeOut || li == LyrScreenshot)
			continue;
		node = wlr_scene_node_at(&layers[li]->node, x, y, NULL, NULL);
	}
	if (!node || !node->data)
		return false;
	AsteroidzNodeData *data = (AsteroidzNodeData *)node->data;
	/* popover rows scroll too: that is how a stepper row is adjusted */
	if (data->type == ASTEROIDZ_BAR_POPOVER_NODE)
		return bar_popover_handle_node_scroll(data, notches);
	if (data->type != ASTEROIDZ_BAR_NODE)
		return false;
	return bar_handle_node_scroll(data, notches, horizontal);
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
static void bar_tray_finish(void) {}
static void bar_volume_finish(void) {}
static void bar_viz_finish(void) {}
static void bar_notify_finish(void) {}
static void bar_dv_finish(void) {}
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
static bool bar_handle_node_scroll(AsteroidzNodeData *hit, int32_t delta,
								   bool horizontal) {
	(void)hit;
	(void)delta;
	(void)horizontal;
	return false;
}
static bool bar_scroll_at(double x, double y, double delta,
						  double delta_discrete, bool horizontal) {
	(void)x;
	(void)y;
	(void)delta;
	(void)delta_discrete;
	(void)horizontal;
	return false;
}
static bool bar_popover_handle_node_click(AsteroidzNodeData *hit,
										  uint32_t button) {
	(void)hit;
	(void)button;
	return false;
}
static bool bar_popover_dismiss_click(struct wlr_scene_node *node) {
	(void)node;
	return false;
}
static bool bar_popover_handle_key(uint32_t keysym) {
	(void)keysym;
	return false;
}
static bool bar_canvas_press(int32_t idx, double cx, double cy) {
	(void)idx;
	(void)cx;
	(void)cy;
	return false;
}
static bool bar_canvas_motion(double cx, double cy) {
	(void)cx;
	(void)cy;
	return false;
}
static bool bar_canvas_release(void) { return false; }
static void bar_popover_close(void) {}

#endif /* ASTEROIDZ_NATIVE_BAR */

#endif /* ASTEROIDZ_BAR_H */
