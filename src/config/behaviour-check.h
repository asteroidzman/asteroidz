#ifndef ASTEROIDZ_BEHAVIOUR_CHECK_H
#define ASTEROIDZ_BEHAVIOUR_CHECK_H

/* `asteroidz -T`: does a setting or a dispatch actually DO anything?
 *
 * `-S` proves a config key parses, clamps, and lands in the right field. That
 * is the whole of what a check with no compositor can say, and it is not what
 * anybody means by "does border_radius work" -- a key can round-trip through
 * the parser perfectly while nothing downstream ever reads it. `srgb_blending`
 * did exactly that for a whole release: parsed, stored, and consumed by a
 * function that left with SceneFX.
 *
 * THE TEST LIST IS THE SCHEMA. This walks config_schema[] and
 * dispatch_actions[] -- the same two tables `-S`, `-L` and `-D` walk -- and
 * runs whatever assertion is registered against each entry. An entry with no
 * assertion prints SKIP. Coverage is therefore in the output of every run
 * rather than in a document that goes stale; there is no list of "what is
 * tested" to maintain, and a key added to the schema starts life visibly
 * unchecked.
 *
 * THE ORACLE RULE, which matters more than the mechanism: an assertion reads
 * OUTPUT, never the field it just wrote. Setting config.borderpx and reading
 * config.borderpx back is `-S`'s job and proves nothing new -- it is true of a
 * compositor that ignores the value entirely. Every check here reads either
 * composited pixels (via the capture the renderer already has) or client
 * geometry the layout produced, both of which are downstream of the thing
 * under test.
 *
 * IN PROCESS, HEADLESS, NO SCRIPT. It forces WLR_BACKENDS=headless, runs the
 * real setup() and the real event loop, and steps a state machine on a timer:
 * apply, let the frame settle, observe, restore. There is no runner, no
 * fixture file and no external driver -- the same reason `-S` is a flag rather
 * than a program.
 *
 * WINDOWS COME FROM A CLIENT, because most of the schema is meaningless
 * without one. `-T -s '<cmd>'` uses that command; otherwise it looks for
 * contrib/wlbgeffect, which already draws a solid colour toplevel of a given
 * ARGB and is already used for exactly this. With no client, every check that
 * needs a window says so and skips -- it does not quietly pass.
 *
 * ANIMATIONS ARE OFF for the duration. A spring settling over 400ms makes
 * every geometry read a race with the frame it is racing, and the failure that
 * produces is indistinguishable from the option being broken. The cost is
 * stated rather than hidden: the `animations` group is the one part of the
 * schema this cannot check, and it prints SKIP with that reason.
 *
 * WHAT IT CANNOT SEE. Headless has no connector, so anything whose consequence
 * is a real link -- vrr, bitdepth, hdr -- skips permanently. Colour keys other
 * than pure primaries skip too: the border colours asserted here are chosen at
 * 0 and 1 on each channel, which are fixed points of every transfer function
 * in the pipeline, so the assertion cannot be fooled by the encode the capture
 * went through.
 */

#define BC_TICK_MS 60
#define BC_SETTLE 3      /* ticks between apply and observe */
#define BC_CAPTURE_WAIT 4/* extra ticks for an armed capture to be written */
#define BC_START_MS 2200 /* let the clients map and their open animation end */

typedef void (*BcFn)(void);

enum { BC_OPTION, BC_DISPATCH };

typedef struct {
	const char *name; /* a config_schema[] key, or a dispatch_actions[] name */
	int kind;
	int need_clients;
	bool capture; /* observe() wants the frame read back */
	BcFn apply;
	BcFn observe;
} BcCheck;

static struct {
	int assertions, failures, ran, skipped;
	const char *name;
	bool bad;
	char detail[192];
	char dir[160];
	uint8_t *img;
	int img_w, img_h;
	struct wl_event_source *timer;
	size_t idx;
	int tick;
} bc;

/* ---------- reporting ---------- */

static void bc_notef(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(bc.detail, sizeof(bc.detail), fmt, ap);
	va_end(ap);
}

static void bc_failf(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "FAIL  %-28s ", bc.name);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
	bc.bad = true;
}

static void bc_skipf(const char *name, const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "SKIP  %-28s ", name);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
	bc.skipped++;
}

static void bc_eq_i(const char *what, long got, long want) {
	bc.assertions++;
	if (got != want)
		bc_failf("%s: got %ld, want %ld", what, got, want);
}

static void bc_near_i(const char *what, long got, long want, long tol) {
	bc.assertions++;
	if (labs(got - want) > tol)
		bc_failf("%s: got %ld, want %ld +/-%ld", what, got, want, tol);
}

static void bc_range_i(const char *what, long got, long lo, long hi) {
	bc.assertions++;
	if (got < lo || got > hi)
		bc_failf("%s: got %ld, want %ld..%ld", what, got, lo, hi);
}

static void bc_true(const char *what, bool cond) {
	bc.assertions++;
	if (!cond)
		bc_failf("%s: false", what);
}

/* ---------- the desktop under test ---------- */

static int bc_nclients(void) {
	Client *c;
	int n = 0;
	wl_list_for_each(c, &clients, link) {
		if (c->mon != NULL && !c->iskilling && client_surface(c)->mapped)
			n++;
	}
	return n;
}

static Client *bc_nth(int n) {
	Client *c;
	int i = 0;
	wl_list_for_each(c, &clients, link) {
		if (c->mon == NULL || c->iskilling || !client_surface(c)->mapped)
			continue;
		if (i++ == n)
			return c;
	}
	return NULL;
}

/* Leftmost / topmost tiled edge, rather than "client 0", because the list
 * order is insertion order and the layout is free to place either one first --
 * an assertion keyed to a particular client would be testing the list. */
static int bc_min_x(void) {
	Client *c;
	int x = INT32_MAX;
	wl_list_for_each(c, &clients, link) {
		if (c->mon != NULL && client_surface(c)->mapped && !c->isfloating
				&& c->geom.x < x)
			x = c->geom.x;
	}
	return x;
}

static int bc_min_y(void) {
	Client *c;
	int y = INT32_MAX;
	wl_list_for_each(c, &clients, link) {
		if (c->mon != NULL && client_surface(c)->mapped && !c->isfloating
				&& c->geom.y < y)
			y = c->geom.y;
	}
	return y;
}

static Client *bc_leftmost(void) {
	Client *c, *best = NULL;
	wl_list_for_each(c, &clients, link) {
		if (c->mon == NULL || !client_surface(c)->mapped || c->isfloating)
			continue;
		if (best == NULL || c->geom.x < best->geom.x)
			best = c;
	}
	return best;
}

static bool bc_dispatch(const char *name, const char *a1, const char *a2) {
	Arg arg = {0};
	char n[64], v1[64], v2[64], v3[8] = "0", v4[8] = "0", v5[8] = "0";
	snprintf(n, sizeof(n), "%s", name);
	snprintf(v1, sizeof(v1), "%s", a1 != NULL ? a1 : "0");
	snprintf(v2, sizeof(v2), "%s", a2 != NULL ? a2 : "0");
	FuncType f = parse_func_name(n, &arg, v1, v2, v3, v4, v5);
	if (f != NULL)
		f(&arg);
	free(arg.v);
	free(arg.v2);
	free(arg.v3);
	return f != NULL;
}

static void bc_focus_first(void) {
	Client *c = bc_nth(0);
	if (c != NULL)
		focusclient(c, 1);
}

/* ---------- pixels ---------- */

/* The capture the renderer already writes for `capture_output`: a P6 PPM of
 * the scan-out attachment, output pixels, after compositing. Read here rather
 * than re-implemented because a second readback path would be a second thing
 * that can be wrong about row pitch. */
static bool bc_load_capture(void) {
	if (selmon == NULL || selmon->wlr_output == NULL)
		return false;
	char path[512];
	snprintf(path, sizeof(path), "%s/%s.ppm", bc.dir,
			 selmon->wlr_output->name);
	FILE *f = fopen(path, "rb");
	if (f == NULL)
		return false;
	int w = 0, h = 0, maxv = 0;
	if (fscanf(f, "P6 %d %d %d", &w, &h, &maxv) != 3 || maxv != 255 || w <= 0
			|| h <= 0) {
		fclose(f);
		return false;
	}
	fgetc(f); /* the single whitespace the writer puts after the maxval */
	free(bc.img);
	size_t need = (size_t)w * (size_t)h * 3;
	bc.img = malloc(need);
	bool ok = bc.img != NULL && fread(bc.img, 1, need, f) == need;
	fclose(f);
	bc.img_w = w;
	bc.img_h = h;
	if (!ok) {
		free(bc.img);
		bc.img = NULL;
	}
	return ok;
}

/* Layout coordinates in, output pixels out. Scale is 1 on the headless output
 * and selmon->m is the only monitor, so this is a translation -- stated rather
 * than assumed, because it is the first thing that stops being true the day a
 * second output is added here. */
static bool bc_px(int lx, int ly, uint8_t rgb[3]) {
	if (bc.img == NULL || selmon == NULL)
		return false;
	int x = lx - selmon->m.x, y = ly - selmon->m.y;
	if (x < 0 || y < 0 || x >= bc.img_w || y >= bc.img_h)
		return false;
	const uint8_t *p = bc.img + ((size_t)y * bc.img_w + x) * 3;
	rgb[0] = p[0];
	rgb[1] = p[1];
	rgb[2] = p[2];
	return true;
}

static bool bc_px_is(int lx, int ly, const uint8_t want[3], int tol) {
	uint8_t got[3];
	if (!bc_px(lx, ly, got))
		return false;
	for (int i = 0; i < 3; i++) {
		if (abs((int)got[i] - (int)want[i]) > tol)
			return false;
	}
	return true;
}

/* Pure primaries only. 0 and 1 are fixed points of every transfer function in
 * the pipeline, so an assertion made on them cannot be defeated by whichever
 * encode the capture went through -- which a mid-grey assertion silently can
 * be, and was, in the colour work. */
static const uint8_t BC_RED[3] = {255, 0, 0};

static void bc_set_focus_red(void) {
	config.focuscolor[0] = 1.0f;
	config.focuscolor[1] = 0.0f;
	config.focuscolor[2] = 0.0f;
	config.focuscolor[3] = 1.0f;
}

/* ---------- the checks ---------- */

static int32_t bc_s_gappoh, bc_s_gappov, bc_s_gappih, bc_s_smartgaps;
static int32_t bc_s_borderpx, bc_s_radius;
static float bc_s_focuscolor[4];
static struct wlr_box bc_s_geom;
static int bc_s_floating;
static Client *bc_s_client;

/* --- layout/gaps --- */

static void bc_apply_gappoh(void) {
	bc_s_gappoh = config.gappoh;
	bc_s_smartgaps = config.smartgaps;
	config.smartgaps = 0;
	config.gappoh = 44;
	config_apply_live();
}
static void bc_observe_gappoh(void) {
	bc_eq_i("leftmost tiled edge", bc_min_x(), selmon->w.x + 44);
	bc_notef("left edge %d = w.x %d + 44", bc_min_x(), selmon->w.x);
	config.gappoh = bc_s_gappoh;
	config.smartgaps = bc_s_smartgaps;
	config_apply_live();
}

static void bc_apply_gappov(void) {
	bc_s_gappov = config.gappov;
	bc_s_smartgaps = config.smartgaps;
	config.smartgaps = 0;
	config.gappov = 37;
	config_apply_live();
}
static void bc_observe_gappov(void) {
	bc_eq_i("topmost tiled edge", bc_min_y(), selmon->w.y + 37);
	bc_notef("top edge %d = w.y %d + 37", bc_min_y(), selmon->w.y);
	config.gappov = bc_s_gappov;
	config.smartgaps = bc_s_smartgaps;
	config_apply_live();
}

/* The inner gap is the distance between the two halves of the split, which is
 * a quantity neither client knows on its own -- exactly the kind of thing a
 * per-client read-back cannot check. */
static void bc_apply_gappih(void) {
	bc_s_gappih = config.gappih;
	bc_s_smartgaps = config.smartgaps;
	config.smartgaps = 0;
	config.gappih = 30;
	config_apply_live();
}
static void bc_observe_gappih(void) {
	Client *c, *left = NULL, *right = NULL;
	wl_list_for_each(c, &clients, link) {
		if (c->mon == NULL || !client_surface(c)->mapped || c->isfloating)
			continue;
		if (left == NULL || c->geom.x < left->geom.x)
			left = c;
		if (right == NULL || c->geom.x > right->geom.x)
			right = c;
	}
	if (left == NULL || right == NULL || left == right) {
		bc_failf("expected two tiled windows side by side");
	} else {
		int gap = right->geom.x - (left->geom.x + left->geom.width);
		bc_eq_i("gap between the split halves", gap, 30);
		bc_notef("%d px between %d..%d and %d", gap, left->geom.x,
				 left->geom.x + left->geom.width, right->geom.x);
	}
	config.gappih = bc_s_gappih;
	config.smartgaps = bc_s_smartgaps;
	config_apply_live();
}

/* --- appearance/border --- */

/* Thickness MEASURED off the frame, by walking in from the window's own left
 * edge at mid-height and counting how far the border colour continues. A
 * compositor that stored borderpx and drew 4 anyway passes every static check
 * and fails this one. */
static void bc_apply_borderpx(void) {
	bc_s_borderpx = config.borderpx;
	bc_s_radius = config.border_radius;
	memcpy(bc_s_focuscolor, config.focuscolor, sizeof(bc_s_focuscolor));
	config.borderpx = 11;
	config.border_radius = 0;
	bc_set_focus_red();
	config_apply_live();
	bc_focus_first();
}
static void bc_observe_borderpx(void) {
	Client *c = focustop(selmon);
	if (c == NULL) {
		bc_failf("no focused window");
	} else {
		int y = c->geom.y + c->geom.height / 2;
		int run = 0;
		while (run < 64 && bc_px_is(c->geom.x + run, y, BC_RED, 6))
			run++;
		bc_eq_i("measured border thickness", run, 11);
		bc_notef("%d px of border at y=%d", run, y);
	}
	config.borderpx = bc_s_borderpx;
	config.border_radius = bc_s_radius;
	memcpy(config.focuscolor, bc_s_focuscolor, sizeof(config.focuscolor));
	config_apply_live();
}

static void bc_apply_focuscolor(void) {
	bc_s_borderpx = config.borderpx;
	bc_s_radius = config.border_radius;
	memcpy(bc_s_focuscolor, config.focuscolor, sizeof(bc_s_focuscolor));
	config.borderpx = 10;
	config.border_radius = 0;
	bc_set_focus_red();
	config_apply_live();
	bc_focus_first();
}
static void bc_observe_focuscolor(void) {
	Client *c = focustop(selmon);
	if (c == NULL) {
		bc_failf("no focused window");
	} else {
		int y = c->geom.y + c->geom.height / 2;
		uint8_t got[3] = {0, 0, 0};
		bc_px(c->geom.x + 4, y, got);
		bc_true("focused border is the focus colour",
				bc_px_is(c->geom.x + 4, y, BC_RED, 6));
		bc_notef("border pixel %u,%u,%u", got[0], got[1], got[2]);
	}
	config.borderpx = bc_s_borderpx;
	config.border_radius = bc_s_radius;
	memcpy(config.focuscolor, bc_s_focuscolor, sizeof(config.focuscolor));
	config_apply_live();
}

/* The rounded corner measured as an AREA, not as one probe pixel. A quarter
 * disc of radius R cut from an RxR box removes R^2(1 - pi/4) pixels, which for
 * R=24 is about 124; a square corner removes none. The band is wide because
 * antialiasing and the exact arc are not the thing under test -- whether the
 * corner is cut at all, and by roughly the right amount, is. */
static void bc_apply_border_radius(void) {
	bc_s_borderpx = config.borderpx;
	bc_s_radius = config.border_radius;
	bc_s_gappoh = config.gappoh;
	bc_s_gappov = config.gappov;
	bc_s_smartgaps = config.smartgaps;
	/* Wide outer gaps so the window's own corner sits over the root
	 * background and nothing else: the assertion is "the backdrop shows
	 * through here", which needs the backdrop to be what is behind. */
	config.smartgaps = 0;
	config.gappoh = 60;
	config.gappov = 60;
	config.borderpx = 6;
	config.border_radius = 24;
	config_apply_live();
}
static void bc_observe_border_radius(void) {
	Client *c = bc_leftmost();
	uint8_t bg[3];
	if (c == NULL) {
		bc_failf("no tiled window");
	} else if (!bc_px(selmon->w.x + 8, selmon->w.y + 8, bg)) {
		bc_failf("could not sample the backdrop");
	} else {
		int cut = 0;
		for (int dy = 0; dy < 24; dy++) {
			for (int dx = 0; dx < 24; dx++) {
				if (bc_px_is(c->geom.x + dx, c->geom.y + dy, bg, 6))
					cut++;
			}
		}
		bc_range_i("corner pixels showing the backdrop", cut, 60, 240);
		bc_notef("%d of 576 corner pixels cut (a square corner cuts 0)", cut);
	}
	config.borderpx = bc_s_borderpx;
	config.border_radius = bc_s_radius;
	config.gappoh = bc_s_gappoh;
	config.gappov = bc_s_gappov;
	config.smartgaps = bc_s_smartgaps;
	config_apply_live();
}

/* --- dispatches --- */

static void bc_apply_toggle_floating(void) {
	bc_focus_first();
	bc_s_client = focustop(selmon);
	if (bc_s_client != NULL) {
		bc_s_geom = bc_s_client->geom;
		bc_s_floating = bc_s_client->isfloating;
	}
	bc_dispatch("toggle_floating", NULL, NULL);
}
static void bc_observe_toggle_floating(void) {
	Client *c = bc_s_client;
	if (c == NULL) {
		bc_failf("no focused window");
		return;
	}
	bc_true("window left the tiling", c->isfloating != bc_s_floating);
	bc_true("geometry changed", c->geom.width != bc_s_geom.width
									|| c->geom.height != bc_s_geom.height
									|| c->geom.x != bc_s_geom.x
									|| c->geom.y != bc_s_geom.y);
	bc_notef("%dx%d+%d+%d -> %dx%d+%d+%d", bc_s_geom.width, bc_s_geom.height,
			 bc_s_geom.x, bc_s_geom.y, c->geom.width, c->geom.height,
			 c->geom.x, c->geom.y);
	bc_dispatch("toggle_floating", NULL, NULL);
}

static void bc_apply_toggle_fullscreen(void) {
	bc_focus_first();
	bc_s_client = focustop(selmon);
	bc_dispatch("toggle_fullscreen", NULL, NULL);
}
static void bc_observe_toggle_fullscreen(void) {
	Client *c = bc_s_client;
	if (c == NULL) {
		bc_failf("no focused window");
		return;
	}
	bc_eq_i("fullscreen width", c->geom.width, selmon->m.width);
	bc_eq_i("fullscreen height", c->geom.height, selmon->m.height);
	bc_notef("%dx%d+%d+%d covers the %dx%d output", c->geom.width,
			 c->geom.height, c->geom.x, c->geom.y, selmon->m.width,
			 selmon->m.height);
	bc_dispatch("toggle_fullscreen", NULL, NULL);
}

static void bc_apply_focus_stack(void) {
	bc_focus_first();
	bc_s_client = focustop(selmon);
	bc_dispatch("focus_stack", "next", NULL);
}
static void bc_observe_focus_stack(void) {
	Client *now = focustop(selmon);
	bc_true("focus moved to another window",
			now != NULL && now != bc_s_client);
	bc_notef("focus %p -> %p", (void *)bc_s_client, (void *)now);
}

static void bc_apply_center_window(void) {
	bc_focus_first();
	bc_s_client = focustop(selmon);
	if (bc_s_client != NULL && !bc_s_client->isfloating)
		bc_dispatch("toggle_floating", NULL, NULL);
	bc_dispatch("center_window", NULL, NULL);
}
static void bc_observe_center_window(void) {
	Client *c = bc_s_client;
	if (c == NULL) {
		bc_failf("no focused window");
		return;
	}
	int cx = c->geom.x + c->geom.width / 2;
	int cy = c->geom.y + c->geom.height / 2;
	bc_near_i("centre x", cx, selmon->w.x + selmon->w.width / 2, 2);
	bc_near_i("centre y", cy, selmon->w.y + selmon->w.height / 2, 2);
	bc_notef("centre %d,%d of %d,%d", cx, cy,
			 selmon->w.x + selmon->w.width / 2,
			 selmon->w.y + selmon->w.height / 2);
	if (c->isfloating)
		bc_dispatch("toggle_floating", NULL, NULL);
}

static void bc_apply_move_window(void) {
	bc_focus_first();
	bc_s_client = focustop(selmon);
	if (bc_s_client != NULL && !bc_s_client->isfloating)
		bc_dispatch("toggle_floating", NULL, NULL);
	bc_dispatch("move_window", "220", "160");
}
static void bc_observe_move_window(void) {
	Client *c = bc_s_client;
	if (c == NULL) {
		bc_failf("no focused window");
		return;
	}
	bc_eq_i("x", c->geom.x, 220);
	bc_eq_i("y", c->geom.y, 160);
	bc_notef("moved to %d,%d", c->geom.x, c->geom.y);
	if (c->isfloating)
		bc_dispatch("toggle_floating", NULL, NULL);
}

static void bc_apply_resize_window(void) {
	bc_focus_first();
	bc_s_client = focustop(selmon);
	if (bc_s_client != NULL && !bc_s_client->isfloating)
		bc_dispatch("toggle_floating", NULL, NULL);
	bc_dispatch("resize_window", "700", "500");
}
static void bc_observe_resize_window(void) {
	Client *c = bc_s_client;
	if (c == NULL) {
		bc_failf("no focused window");
		return;
	}
	bc_eq_i("width", c->geom.width, 700);
	bc_eq_i("height", c->geom.height, 500);
	bc_notef("resized to %dx%d", c->geom.width, c->geom.height);
	if (c->isfloating)
		bc_dispatch("toggle_floating", NULL, NULL);
}

static const BcCheck bc_table[] = {
	{"gappoh", BC_OPTION, 1, false, bc_apply_gappoh, bc_observe_gappoh},
	{"gappov", BC_OPTION, 1, false, bc_apply_gappov, bc_observe_gappov},
	{"gappih", BC_OPTION, 2, false, bc_apply_gappih, bc_observe_gappih},
	{"borderpx", BC_OPTION, 1, true, bc_apply_borderpx, bc_observe_borderpx},
	{"focuscolor", BC_OPTION, 1, true, bc_apply_focuscolor,
	 bc_observe_focuscolor},
	{"border_radius", BC_OPTION, 1, true, bc_apply_border_radius,
	 bc_observe_border_radius},
	{"toggle_floating", BC_DISPATCH, 1, false, bc_apply_toggle_floating,
	 bc_observe_toggle_floating},
	{"toggle_fullscreen", BC_DISPATCH, 1, false, bc_apply_toggle_fullscreen,
	 bc_observe_toggle_fullscreen},
	{"focus_stack", BC_DISPATCH, 2, false, bc_apply_focus_stack,
	 bc_observe_focus_stack},
	{"center_window", BC_DISPATCH, 1, false, bc_apply_center_window,
	 bc_observe_center_window},
	{"move_window", BC_DISPATCH, 1, false, bc_apply_move_window,
	 bc_observe_move_window},
	{"resize_window", BC_DISPATCH, 1, false, bc_apply_resize_window,
	 bc_observe_resize_window},
};
#define BC_COUNT (sizeof(bc_table) / sizeof(bc_table[0]))

static const BcCheck *bc_find(const char *name, int kind) {
	for (size_t i = 0; i < BC_COUNT; i++) {
		if (bc_table[i].kind == kind && strcmp(bc_table[i].name, name) == 0)
			return &bc_table[i];
	}
	return NULL;
}

/* ---------- what has no check ---------- */

/* Printed from the schema, so the gap is in the output of every run. The
 * reasons are grouped rather than per-key: "headless has no connector" is one
 * fact about eleven keys, and eleven copies of it is a report nobody reads to
 * the end. */
static void bc_report_uncovered(void) {
	for (size_t i = 0; i < CONFIG_SCHEMA_COUNT; i++) {
		const ConfigOption *o = &config_schema[i];
		if (bc_find(o->key, BC_OPTION) != NULL)
			continue;
		if (strncmp(o->group, "animations", 10) == 0)
			bc_skipf(o->key, "no check: animations are off for the run");
		else
			bc_skipf(o->key, "no check");
	}
	for (size_t i = 0; i < DISPATCH_ACTION_COUNT; i++) {
		if (bc_find(dispatch_actions[i].name, BC_DISPATCH) == NULL)
			bc_skipf(dispatch_actions[i].name, "no check");
	}
}

/* Every name in the table must resolve. Static, so it runs before the
 * compositor: a dispatch a keybind editor offers and parse_func_name has never
 * heard of is a dead entry in a UI, and nothing else reports it. */
static int bc_check_dispatch_names(void) {
	int bad = 0;
	for (size_t i = 0; i < DISPATCH_ACTION_COUNT; i++) {
		Arg arg = {0};
		char n[64], z[8] = "0";
		snprintf(n, sizeof(n), "%s", dispatch_actions[i].name);
		char a1[8] = "0", a2[8] = "0", a3[8] = "0", a4[8] = "0";
		if (parse_func_name(n, &arg, a1, a2, a3, a4, z) == NULL) {
			fprintf(stderr, "FAIL  %-28s parse_func_name does not know it\n",
					dispatch_actions[i].name);
			bad++;
		}
		free(arg.v);
		free(arg.v2);
		free(arg.v3);
	}
	bc.assertions += (int)DISPATCH_ACTION_COUNT;
	bc.failures += bad;
	return bad;
}

/* The client that supplies the windows. wlbgeffect already draws a solid
 * colour toplevel of a given ARGB and already maps reliably -- writing a
 * second one for this would be a second thing to keep working. Two of them,
 * because a single window cannot show an inner gap or a focus change. */
static char *bc_client_command(void) {
	static const char *candidates[] = {
		"contrib/wlbgeffect/wlbgeffect",
		"/usr/bin/wlbgeffect",
	};
	for (size_t i = 0; i < LENGTH(candidates); i++) {
		if (access(candidates[i], X_OK) != 0)
			continue;
		/* SSD, or there is no border to measure: asteroidz draws none for a
		 * toplevel that never negotiates decorations, and the first border
		 * audit recorded border_draws = 0 with borderpx 4 for exactly that
		 * reason. Without this, every border assertion here reads backdrop
		 * and reports the option broken. */
		return string_printf(
			"WLBGEFFECT_SSD=1 %s bc-a 600 ff3050e0 & "
			"WLBGEFFECT_SSD=1 %s bc-b 600 ff30e050",
			candidates[i], candidates[i]);
	}
	fprintf(stderr, "-T: no -s command and contrib/wlbgeffect is not built "
					"here; every check that needs a window will skip\n");
	return NULL;
}

/* ---------- the driver ---------- */

static int bc_step(void *data) {
	(void)data;
	for (;;) {
		if (bc.idx >= BC_COUNT) {
			bc_report_uncovered();
			fprintf(stderr,
				   "\n%zu options, %zu dispatches, %d checks, %d assertions, "
				   "%d skipped, %d failures\n",
				   (size_t)CONFIG_SCHEMA_COUNT, (size_t)DISPATCH_ACTION_COUNT,
				   bc.ran, bc.assertions, bc.skipped, bc.failures);
			wl_display_terminate(dpy);
			return 0;
		}
		const BcCheck *ck = &bc_table[bc.idx];
		int obs = ck->capture ? BC_SETTLE + BC_CAPTURE_WAIT : BC_SETTLE;
		if (bc.idx == 0 && bc.tick == 0)
			fprintf(stderr, "-T: %d window(s) on %s %dx%d\n", bc_nclients(),
					selmon != NULL && selmon->wlr_output != NULL
						? selmon->wlr_output->name
						: "(none)",
					selmon != NULL ? selmon->m.width : 0,
					selmon != NULL ? selmon->m.height : 0);
		if (bc.tick == 0) {
			bc.name = ck->name;
			bc.bad = false;
			bc.detail[0] = '\0';
			if (bc_nclients() < ck->need_clients) {
				bc_skipf(ck->name, "needs %d window(s), %d mapped",
						 ck->need_clients, bc_nclients());
				bc.idx++;
				continue;
			}
			ck->apply();
		} else if (ck->capture && bc.tick == BC_SETTLE) {
			capture_output(NULL);
		} else if (bc.tick == obs) {
			if (ck->capture && !bc_load_capture())
				bc_failf("no capture was written to %s", bc.dir);
			else
				ck->observe();
			if (bc.bad) {
				bc.failures++;
			} else {
				bc.ran++;
				fprintf(stderr, "OK    %-28s %s\n", ck->name, bc.detail);
			}
			bc.idx++;
			bc.tick = 0;
			continue;
		}
		bc.tick++;
		break;
	}
	wl_event_source_timer_update(bc.timer, BC_TICK_MS);
	return 0;
}

/* Called from run(), once the backend is up and the client command has been
 * spawned. */
static void bc_arm(void) {
	/* See the header: a settling animation and a broken option look the same
	 * through a geometry read. */
	config.animations = 0;
	config.layer_animations = 0;

	bc.timer = wl_event_loop_add_timer(wl_display_get_event_loop(dpy), bc_step,
									   NULL);
	wl_event_source_timer_update(bc.timer, BC_START_MS);
}

#endif /* ASTEROIDZ_BEHAVIOUR_CHECK_H */
