#include "ufo-node.h"

#include <cairo/cairo.h>
#include <drm_fourcc.h>
#include <math.h>
#include <stdlib.h>
#include <time.h>
#include <wlr/interfaces/wlr_buffer.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define UFO_FRAME_MS 16
#define UFO_FLIGHT_MS 4800
#define UFO_MIN_INTERVAL_MS (4 * 60 * 1000)
#define UFO_MAX_INTERVAL_MS (15 * 60 * 1000)
/* first fly-by after enable comes quickly so it's easy to confirm it works */
#define UFO_FIRST_MIN_MS (20 * 1000)
#define UFO_FIRST_MAX_MS (60 * 1000)

/* --- cairo-backed wlr_buffer (one per animation frame) --------------------- */
struct ufo_buffer {
	struct wlr_buffer base;
	cairo_surface_t *surface;
};

static void ufo_buffer_destroy(struct wlr_buffer *wlr_buffer) {
	struct ufo_buffer *buf = wl_container_of(wlr_buffer, buf, base);
	cairo_surface_destroy(buf->surface);
	free(buf);
}

static bool ufo_buffer_begin_data_ptr_access(struct wlr_buffer *wlr_buffer,
											 uint32_t flags, void **data,
											 uint32_t *format, size_t *stride) {
	(void)flags;
	struct ufo_buffer *buf = wl_container_of(wlr_buffer, buf, base);
	/* see the matching fix in text-node.c's text_buffer_begin_data_ptr_access:
	 * never claim success with a NULL data pointer. */
	if (cairo_surface_status(buf->surface) != CAIRO_STATUS_SUCCESS)
		return false;
	*data = cairo_image_surface_get_data(buf->surface);
	if (!*data)
		return false;
	*format = DRM_FORMAT_ARGB8888;
	*stride = cairo_image_surface_get_stride(buf->surface);
	return true;
}

static void ufo_buffer_end_data_ptr_access(struct wlr_buffer *wlr_buffer) {
	(void)wlr_buffer;
}

static const struct wlr_buffer_impl ufo_buffer_impl = {
	.destroy = ufo_buffer_destroy,
	.begin_data_ptr_access = ufo_buffer_begin_data_ptr_access,
	.end_data_ptr_access = ufo_buffer_end_data_ptr_access,
};

/* --- egg ------------------------------------------------------------------- */
struct ufo_egg {
	struct wlr_scene_buffer *scene_buffer;
	struct wlr_scene_tree *parent;
	struct wl_event_source *frame_timer;
	struct wl_event_source *schedule_timer;
	struct ufo_buffer *buffer; /* current, held by the scene */

	ufo_geometry_fn geometry;
	void *user_data;

	bool enabled;
	bool running;
	bool first_pending; /* next fly-by is the quick first one */
	int w, h;         /* current strip size (px) */
	double progress;  /* 0..1 */
	double seed;      /* 0..1 */
	uint32_t last_ms;
	float accent[3];
};

static uint32_t now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static int rand_interval_ms(void) {
	return UFO_MIN_INTERVAL_MS +
		   (rand() % (UFO_MAX_INTERVAL_MS - UFO_MIN_INTERVAL_MS));
}

static int rand_first_ms(void) {
	return UFO_FIRST_MIN_MS + (rand() % (UFO_FIRST_MAX_MS - UFO_FIRST_MIN_MS));
}

/* --- little math helpers --------------------------------------------------- */
static double clampd(double v, double lo, double hi) {
	return v < lo ? lo : (v > hi ? hi : v);
}
static double smoothstep(double e0, double e1, double x) {
	double t = clampd((x - e0) / (e1 - e0), 0.0, 1.0);
	return t * t * (3.0 - 2.0 * t);
}
static double mixd(double a, double b, double t) { return a + (b - a) * t; }
static double hash11(double p) {
	double s = sin(p * 127.1) * 43758.5453;
	return s - floor(s);
}

/* --- drawing --------------------------------------------------------------- */
static void stroke_tri(cairo_t *cr, double nx, double ny, double lx, double ly,
					   double rx, double ry, double kx, double ky) {
	cairo_move_to(cr, nx, ny);
	cairo_line_to(cr, lx, ly);
	cairo_line_to(cr, kx, ky);
	cairo_line_to(cr, rx, ry);
	cairo_close_path(cr);
	cairo_stroke(cr);
}

static void ufo_draw(struct ufo_egg *egg, cairo_t *cr) {
	double w = egg->w, h = egg->h, t = egg->progress, seed = egg->seed;
	cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
	cairo_paint(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);

	double dir = seed < 0.5 ? 1.0 : -1.0;
	double scale = h * 0.32;
	double lw = fmax(scale * 0.14, 1.5);
	double midY = h * 0.5;
	double hoverX = w * (0.42 + 0.16 * dir * hash11(seed * 3.7));

	double enter = smoothstep(0.0, 0.28, t);
	double fireT = clampd((t - 0.28) / 0.14, 0.0, 1.0);
	double boomT = clampd((t - 0.42) / 0.40, 0.0, 1.0);
	double exitT = smoothstep(0.80, 1.0, t);
	double gFade = smoothstep(0.0, 0.06, t) * (1.0 - smoothstep(0.9, 1.0, t));
	if (gFade <= 0.0)
		return;

	cairo_set_line_width(cr, lw);

	/* UFO (hovers, vanishes on hit) */
	double ufoA = (1.0 - boomT) * gFade;
	if (ufoA > 0.01) {
		double bob = sin(t * 34.0) * h * 0.05 * enter;
		double ux = hoverX, uy = midY + bob;
		cairo_save(cr);
		cairo_translate(cr, ux, uy);
		cairo_set_source_rgba(cr, 0.75, 0.95, 1.0, ufoA);
		/* saucer body */
		cairo_save(cr);
		cairo_scale(cr, scale * 1.25, scale * 0.42);
		cairo_arc(cr, 0, 0, 1.0, 0, 2 * M_PI);
		cairo_restore(cr);
		cairo_stroke(cr);
		/* dome */
		cairo_arc(cr, 0, -scale * 0.15, scale * 0.55, M_PI, 2 * M_PI);
		cairo_stroke(cr);
		/* blinking under-lights */
		for (int i = 0; i < 3; i++) {
			double fx = (i - 1) * scale * 0.7;
			double blink = 0.5 + 0.5 * sin(t * 60.0 + i * 2.1);
			cairo_set_source_rgba(cr, 1.0, 0.85, 0.4, ufoA * blink);
			cairo_arc(cr, fx, scale * 0.34, scale * 0.11, 0, 2 * M_PI);
			cairo_fill(cr);
		}
		cairo_restore(cr);
	}

	/* Asteroids ship */
	double shipStartX = dir > 0.0 ? -scale * 2.0 : w + scale * 2.0;
	double postX = hoverX - dir * w * 0.34;
	double shipX = mixd(mixd(shipStartX, postX, enter), shipStartX, exitT);
	double shipY = midY - h * 0.04;
	double ax = hoverX - shipX, ay = midY - shipY;
	double al = fmax(hypot(ax, ay), 1e-4);
	ax /= al;
	ay /= al;
	double px = -ay, py = ax;
	double nx = shipX + ax * scale * 1.15, ny = shipY + ay * scale * 1.15;
	double lx = shipX - ax * scale * 0.75 + px * scale * 0.7;
	double ly = shipY - ay * scale * 0.75 + py * scale * 0.7;
	double rx = shipX - ax * scale * 0.75 - px * scale * 0.7;
	double ry = shipY - ay * scale * 0.75 - py * scale * 0.7;
	double kx = shipX - ax * scale * 0.25, ky = shipY - ay * scale * 0.25;
	cairo_set_source_rgba(cr, egg->accent[0], egg->accent[1], egg->accent[2],
						  gFade);
	stroke_tri(cr, nx, ny, lx, ly, rx, ry, kx, ky);
	/* thruster flicker while flying in */
	double thrust = enter * (1.0 - exitT);
	if (thrust > 0.01) {
		double fl = 0.4 + 0.3 * hash11(t * 90.0);
		cairo_set_source_rgba(cr, 1.0, 0.6, 0.2, gFade * thrust);
		cairo_move_to(cr, kx, ky);
		cairo_line_to(cr, kx - ax * scale * fl, ky - ay * scale * fl);
		cairo_stroke(cr);
	}

	/* bullet: nose -> UFO during the fire window */
	if (fireT > 0.0 && fireT < 1.0 && boomT <= 0.0) {
		double bx = mixd(nx, hoverX, fireT), by = mixd(ny, midY, fireT);
		cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, gFade);
		cairo_move_to(cr, bx - ax * scale * 0.6, by - ay * scale * 0.6);
		cairo_line_to(cr, bx, by);
		cairo_stroke(cr);
		cairo_arc(cr, bx, by, scale * 0.12, 0, 2 * M_PI);
		cairo_fill(cr);
	}

	/* explosion */
	if (boomT > 0.0 && boomT < 1.0) {
		double rad = boomT * scale * 3.4;
		double fade = (1.0 - boomT) * gFade;
		double rr = mixd(1.0, 1.0, boomT), gg = mixd(0.95, 0.35, boomT),
			   bb = mixd(0.6, 0.05, boomT);
		cairo_set_source_rgba(cr, rr, gg, bb, fade);
		cairo_arc(cr, hoverX, midY, rad, 0, 2 * M_PI);
		cairo_stroke(cr);
		for (int i = 0; i < 12; i++) {
			double a = i / 12.0 * 2 * M_PI + seed * 10.0;
			double sp = 0.7 + 0.5 * hash11(i + seed * 5.0);
			double sx = hoverX + cos(a) * rad * sp;
			double sy = midY + sin(a) * rad * sp;
			cairo_arc(cr, sx, sy, scale * (0.12 - 0.07 * boomT), 0, 2 * M_PI);
			cairo_fill(cr);
		}
	}
}

static void ufo_render_frame(struct ufo_egg *egg) {
	if (egg->w <= 0 || egg->h <= 0)
		return;
	cairo_surface_t *surface =
		cairo_image_surface_create(CAIRO_FORMAT_ARGB32, egg->w, egg->h);
	if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(surface);
		return;
	}
	cairo_t *cr = cairo_create(surface);
	ufo_draw(egg, cr);
	cairo_destroy(cr);
	cairo_surface_flush(surface);

	struct ufo_buffer *buf = calloc(1, sizeof(*buf));
	if (!buf) {
		cairo_surface_destroy(surface);
		return;
	}
	wlr_buffer_init(&buf->base, &ufo_buffer_impl, egg->w, egg->h);
	buf->surface = surface;

	/* attach the new buffer before dropping the old one -- the scene node
	 * must never be left pointing at an already-freed wlr_buffer, even
	 * momentarily (see the matching fix + comment in text-node.c's
	 * asteroidz_icon_node_set for the exact same bug -- this one repaints
	 * every 16ms for ~4.8s during a fly-by, the fastest-repeating
	 * instance of this pattern in the codebase). */
	wlr_scene_buffer_set_buffer(egg->scene_buffer, &buf->base);
	if (egg->buffer)
		wlr_buffer_drop(&egg->buffer->base);
	egg->buffer = buf;
	wlr_scene_buffer_set_dest_size(egg->scene_buffer, egg->w, egg->h);
}

static void ufo_stop(struct ufo_egg *egg) {
	egg->running = false;
	wlr_scene_node_set_enabled(&egg->scene_buffer->node, false);
	if (egg->buffer) {
		wlr_scene_buffer_set_buffer(egg->scene_buffer, NULL);
		wlr_buffer_drop(&egg->buffer->base);
		egg->buffer = NULL;
	}
}

static int ufo_frame_cb(void *data) {
	struct ufo_egg *egg = data;
	if (!egg->running)
		return 0;
	uint32_t t = now_ms();
	uint32_t dt = t - egg->last_ms;
	egg->last_ms = t;
	egg->progress += (double)dt / (double)UFO_FLIGHT_MS;
	if (egg->progress >= 1.0) {
		ufo_stop(egg);
		if (egg->enabled)
			wl_event_source_timer_update(egg->schedule_timer,
										 rand_interval_ms());
		return 0;
	}
	ufo_render_frame(egg);
	wl_event_source_timer_update(egg->frame_timer, UFO_FRAME_MS);
	return 0;
}

/* Begin a fly-by right now over the current bar strip. Returns false if there
 * is no output/bar to draw over (or one is already in flight). */
static bool ufo_start_now(struct ufo_egg *egg) {
	if (egg->running)
		return true;
	int x, y, w, h;
	if (!egg->geometry || !egg->geometry(egg->user_data, &x, &y, &w, &h) ||
		w <= 0 || h <= 0)
		return false;
	egg->w = w;
	egg->h = h;
	egg->seed = (rand() % 1000) / 1000.0;
	egg->progress = 0.0;
	egg->running = true;
	egg->first_pending = false; /* the quick intro fly-by has happened */
	egg->last_ms = now_ms();
	wlr_scene_node_set_position(&egg->scene_buffer->node, x, y);
	wlr_scene_node_set_enabled(&egg->scene_buffer->node, true);
	wlr_scene_node_raise_to_top(&egg->scene_buffer->node);
	ufo_render_frame(egg);
	wl_event_source_timer_update(egg->frame_timer, UFO_FRAME_MS);
	return true;
}

static int ufo_schedule_cb(void *data) {
	struct ufo_egg *egg = data;
	if (!egg->enabled)
		return 0;
	if (!ufo_start_now(egg)) /* no output/bar right now -- retry shortly */
		wl_event_source_timer_update(egg->schedule_timer, 30 * 1000);
	return 0;
}

struct ufo_egg *ufo_egg_create(struct wl_event_loop *loop,
							   struct wlr_scene_tree *parent,
							   ufo_geometry_fn geometry, void *user_data) {
	struct ufo_egg *egg = calloc(1, sizeof(*egg));
	if (!egg)
		return NULL;
	egg->scene_buffer = wlr_scene_buffer_create(parent, NULL);
	if (!egg->scene_buffer) {
		free(egg);
		return NULL;
	}
	egg->parent = parent;
	egg->geometry = geometry;
	egg->user_data = user_data;
	egg->first_pending = true;
	egg->accent[0] = 0.97f;
	egg->accent[1] = 0.67f;
	egg->accent[2] = 1.0f;
	wlr_scene_node_set_enabled(&egg->scene_buffer->node, false);
	egg->frame_timer = wl_event_loop_add_timer(loop, ufo_frame_cb, egg);
	egg->schedule_timer = wl_event_loop_add_timer(loop, ufo_schedule_cb, egg);
	srand((unsigned)now_ms());
	return egg;
}

void ufo_egg_set_accent(struct ufo_egg *egg, float r, float g, float b) {
	if (!egg)
		return;
	egg->accent[0] = r;
	egg->accent[1] = g;
	egg->accent[2] = b;
}

void ufo_egg_set_enabled(struct ufo_egg *egg, bool enabled) {
	if (!egg || egg->enabled == enabled)
		return;
	egg->enabled = enabled;
	if (enabled) {
		wl_event_source_timer_update(
			egg->schedule_timer,
			egg->first_pending ? rand_first_ms() : rand_interval_ms());
	} else {
		wl_event_source_timer_update(egg->schedule_timer, 0); /* disarm */
		if (egg->running)
			ufo_stop(egg);
	}
}

void ufo_egg_trigger(struct ufo_egg *egg) {
	if (egg)
		ufo_start_now(egg); /* fire on demand, regardless of the timer/config */
}

void ufo_egg_destroy(struct ufo_egg *egg) {
	if (!egg)
		return;
	if (egg->frame_timer)
		wl_event_source_remove(egg->frame_timer);
	if (egg->schedule_timer)
		wl_event_source_remove(egg->schedule_timer);
	if (egg->buffer) {
		wlr_scene_buffer_set_buffer(egg->scene_buffer, NULL);
		wlr_buffer_drop(&egg->buffer->base);
	}
	if (egg->scene_buffer)
		wlr_scene_node_destroy(&egg->scene_buffer->node);
	free(egg);
}
