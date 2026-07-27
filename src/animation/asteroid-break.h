/* The "asteroid" close animation: a window comes apart the way a rock does in
 * the arcade game.
 *
 * Not the window's own pixels. The 1979 machine drew everything as white line
 * loops on black, and a rock breaking up is that loop splitting into smaller
 * loops that tumble away -- so this replaces the window with a jagged vector
 * outline of the same size and splits THAT. Slicing the snapshot into moving
 * tiles (still available as `fall`) is a different effect entirely: it reads
 * as breaking glass, because the pieces carry photographic content and cannot
 * rotate.
 *
 * Rotation is the whole reason this is drawn rather than sliced. A scene
 * buffer cannot be rotated -- there is no transform on the node -- so a
 * tumbling fragment has to be RE-DRAWN each frame at its current angle. Cairo
 * into a wlr_buffer per frame, exactly as the UFO easter egg does, which is
 * also why no shader is involved: this is a dozen stroked polygons a frame,
 * work a CPU does not notice, and it renders identically on the GLES and
 * Vulkan backends because the scene graph only ever sees an ARGB buffer.
 *
 * One node per fragment, each with its own small buffer, rather than one
 * screen-sized surface redrawn per frame: a fullscreen window would otherwise
 * mean an 11 MB allocation sixty times a second for the length of the run.
 */
#ifndef ASTEROIDZ_ASTEROID_BREAK_H
#define ASTEROIDZ_ASTEROID_BREAK_H

#include <cairo/cairo.h>
#include <drm_fourcc.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/box.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define AST_MAX_FRAGS 10
#define AST_MAX_VERTS 12
/* Points at which a fragment's outline is sampled. The arcade rocks are
 * 10-12 vertex loops; fewer reads as a triangle, more stops looking hand-made. */
#define AST_FRAG_VERTS 7

/* A tumbling piece of the rock, in its own local space: vertices are relative
 * to the fragment's centre, so rotating it is rotating the path, not moving
 * points. */
typedef struct {
	double vx[AST_MAX_VERTS], vy[AST_MAX_VERTS];
	int32_t nverts;
	double cx, cy;	 /* centre at t=0, layout coordinates */
	double dx, dy;	 /* travel over the whole run, layout px */
	double angle;	 /* radians at t=0 */
	double spin;	 /* radians over the whole run */
	double radius;	 /* max vertex distance, sizes the buffer */
	bool spark;		 /* a two-point streak rather than a closed loop */
	struct wlr_scene_buffer *node;
	/* Two buffers, alternated per tick, allocated once. The side is constant
	 * for the whole run (see ast_frag_render), so there is nothing to resize
	 * and nothing to reallocate -- this used to build a fresh cairo surface
	 * and a fresh ast_buffer every fragment every frame, which measured at
	 * ~0.5ms per tick and 78% of all animation time.
	 *
	 * Two rather than one: the attached buffer may still be being read for the
	 * frame in flight, so drawing over it in place is a race. Alternating
	 * hands the scene a buffer nothing is reading. */
	struct ast_buffer *buffers[2];
	int32_t cur; /* which of buffers[] the node currently shows */
} AsteroidFrag;

typedef struct AsteroidBreak {
	struct wlr_scene_tree *tree;
	AsteroidFrag frags[AST_MAX_FRAGS];
	int32_t nfrags;
	float color[4];
	Monitor *mon;
	/* Progress at the last render. The fadeout list is walked by EVERY
	 * output's frame handler, so on a two-monitor desk this animation is
	 * ticked twice per frame -- and unlike the tile version, a tick here
	 * rasterises ten polygons. Re-drawing the same instant twice is pure
	 * waste, so a repeated t is skipped. */
	double last_t;
	bool drawn;
} AsteroidBreak;

/* --- cairo-backed wlr_buffer ---------------------------------------------- */

struct ast_buffer {
	struct wlr_buffer base;
	cairo_surface_t *surface;
};

static void ast_buffer_destroy(struct wlr_buffer *wlr_buffer) {
	struct ast_buffer *buf = wl_container_of(wlr_buffer, buf, base);
	cairo_surface_destroy(buf->surface);
	free(buf);
}

static bool ast_buffer_begin_data_ptr_access(struct wlr_buffer *wlr_buffer,
											 uint32_t flags, void **data,
											 uint32_t *format, size_t *stride) {
	(void)flags;
	struct ast_buffer *buf = wl_container_of(wlr_buffer, buf, base);
	/* never claim success with a NULL pointer -- same rule as the text and
	 * UFO buffers, and the same crash if it is broken */
	if (cairo_surface_status(buf->surface) != CAIRO_STATUS_SUCCESS)
		return false;
	*data = cairo_image_surface_get_data(buf->surface);
	if (!*data)
		return false;
	*format = DRM_FORMAT_ARGB8888;
	*stride = cairo_image_surface_get_stride(buf->surface);
	return true;
}

static void ast_buffer_end_data_ptr_access(struct wlr_buffer *wlr_buffer) {
	(void)wlr_buffer;
}

static const struct wlr_buffer_impl ast_buffer_impl = {
	.destroy = ast_buffer_destroy,
	.begin_data_ptr_access = ast_buffer_begin_data_ptr_access,
	.end_data_ptr_access = ast_buffer_end_data_ptr_access,
};

/* --- shape ---------------------------------------------------------------- */

/* Deterministic noise in [lo,hi). A hash rather than rand(), so a break-up
 * never depends on global RNG state and the same window comes apart the same
 * way twice. */
static double ast_jitter(uint32_t seed, double lo, double hi) {
	seed = seed * 1664525u + 1013904223u;
	seed ^= seed >> 16;
	seed *= 0x7feb352du;
	seed ^= seed >> 15;
	return lo + (hi - lo) * ((double)(seed & 0xffffffu) / (double)0xffffffu);
}

/* Build one fragment: a jagged closed loop, roughly a wedge of the parent
 * rock, centred on `cx,cy` and thrown outward from `ox,oy`. */
static void ast_make_frag(AsteroidFrag *f, uint32_t seed, double cx, double cy,
						  double ox, double oy, double size, double reach) {
	f->cx = cx;
	f->cy = cy;
	f->nverts = AST_FRAG_VERTS;
	f->radius = 0.0;
	f->spark = false;

	/* Vertices at jittered radii around the circle: the rocks in the game are
	 * a circle with each vertex pushed in or out, never a regular polygon. */
	for (int32_t i = 0; i < f->nverts; i++) {
		double a = (2.0 * M_PI * i) / f->nverts;
		double r = size * ast_jitter(seed + (uint32_t)i * 71u, 0.55, 1.0);
		f->vx[i] = cos(a) * r;
		f->vy[i] = sin(a) * r;
		if (r > f->radius)
			f->radius = r;
	}

	/* Straight out from the window's centre, like every other piece of debris
	 * in that game, at a speed that does not depend on where it started. */
	double nx = cx - ox, ny = cy - oy;
	double len = sqrt(nx * nx + ny * ny);
	if (len < 1.0) {
		double a = ast_jitter(seed + 977u, 0.0, 2.0 * M_PI);
		nx = cos(a);
		ny = sin(a);
	} else {
		nx /= len;
		ny /= len;
	}
	double speed = reach * ast_jitter(seed + 313u, 0.7, 1.15);
	f->dx = nx * speed;
	f->dy = ny * speed;
	f->angle = ast_jitter(seed + 51u, 0.0, 2.0 * M_PI);
	/* Half a turn to two turns over the run, either way round. */
	f->spin = ast_jitter(seed + 907u, -4.0 * M_PI, 4.0 * M_PI);
}

/* A spark: the short straight streaks the game throws off alongside the rocks.
 * Two points, so it draws as a line rather than a loop. */
static void ast_make_spark(AsteroidFrag *f, uint32_t seed, double ox, double oy,
						   double reach) {
	double a = ast_jitter(seed, 0.0, 2.0 * M_PI);
	double l = reach * ast_jitter(seed + 17u, 0.05, 0.12);
	f->nverts = 2;
	f->vx[0] = -cos(a) * l;
	f->vy[0] = -sin(a) * l;
	f->vx[1] = cos(a) * l;
	f->vy[1] = sin(a) * l;
	f->radius = l;
	f->spark = true;
	f->cx = ox;
	f->cy = oy;
	double speed = reach * ast_jitter(seed + 41u, 0.9, 1.6);
	f->dx = cos(a) * speed;
	f->dy = sin(a) * speed;
	f->angle = a;
	f->spin = 0.0; /* a streak points the way it is going */
}

/* --- drawing -------------------------------------------------------------- */

/* Redraw one fragment at `angle`, into a square buffer big enough to hold it
 * at any rotation, and move its node so the fragment's centre lands on
 * (cx,cy). Returns false if the piece has nothing to show. */
/* Side of a fragment's square buffer: diameter plus room for the stroke.
 * Constant across the run, so rotation never resizes the node -- which is what
 * makes the buffers reusable. */
static int32_t ast_frag_side(const AsteroidFrag *f) {
	return (int32_t)ceil(f->radius * 2.0) + 6;
}

static struct ast_buffer *ast_buffer_new(int32_t side) {
	cairo_surface_t *surface =
		cairo_image_surface_create(CAIRO_FORMAT_ARGB32, side, side);
	if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(surface);
		return NULL;
	}
	struct ast_buffer *buf = calloc(1, sizeof(*buf));
	if (!buf) {
		cairo_surface_destroy(surface);
		return NULL;
	}
	wlr_buffer_init(&buf->base, &ast_buffer_impl, side, side);
	buf->surface = surface;
	return buf;
}

static bool ast_frag_render(AsteroidFrag *f, const float color[4], double alpha,
							double angle, double cx, double cy) {
	int32_t side = ast_frag_side(f);
	if (side < 4)
		return false;

	/* Allocated on first use rather than at init: a break that is culled
	 * immediately (debris landing on another monitor) never pays for them. */
	if (!f->buffers[0] && !(f->buffers[0] = ast_buffer_new(side)))
		return false;
	if (!f->buffers[1] && !(f->buffers[1] = ast_buffer_new(side)))
		return false;

	int32_t next = f->cur ^ 1;
	struct ast_buffer *buf = f->buffers[next];
	cairo_surface_t *surface = buf->surface;

	cairo_t *cr = cairo_create(surface);
	/* Reused buffer, so wipe last tick's rock before drawing this one --
	 * a fresh surface came zeroed, this one does not. */
	cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
	cairo_paint(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

	cairo_translate(cr, side / 2.0, side / 2.0);
	cairo_rotate(cr, angle);
	cairo_set_line_width(cr, 2.0);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
	/* Alpha is NOT baked in any more -- the scene applies it per node below.
	 * Baking it meant the pixels depended on the fade as well as the angle,
	 * so nothing could ever be reused. */
	cairo_set_source_rgba(cr, color[0], color[1], color[2], color[3]);

	cairo_move_to(cr, f->vx[0], f->vy[0]);
	for (int32_t i = 1; i < f->nverts; i++)
		cairo_line_to(cr, f->vx[i], f->vy[i]);
	if (!f->spark)
		cairo_close_path(cr);
	cairo_stroke(cr);
	cairo_destroy(cr);
	cairo_surface_flush(surface);

	/* with_damage: the scene has seen this buffer before and would otherwise
	 * be entitled to assume its contents are unchanged. */
	wlr_scene_buffer_set_buffer_with_damage(f->node, &buf->base, NULL);
	f->cur = next;
	wlr_scene_buffer_set_opacity(f->node, (float)alpha);
	wlr_scene_buffer_set_dest_size(f->node, side, side);
	wlr_scene_node_set_position(&f->node->node, (int32_t)(cx - side / 2.0),
								(int32_t)(cy - side / 2.0));
	return true;
}

#endif /* ASTEROIDZ_ASTEROID_BREAK_H */
