/* Glass shards share an irregular radial fracture mesh. All vertices and
 * motion are deterministic functions of the pane size, density and time;
 * no per-frame integration or image copies are needed. */
static double shatter_jitter(uint32_t seed, double lo, double hi) {
	seed = seed * 1664525u + 1013904223u;
	seed ^= seed >> 16;
	seed *= 0x7feb352du;
	seed ^= seed >> 15;
	return lo + (hi - lo) * ((double)(seed & 0xffffffu) / 16777216.0);
}

#define SHATTER_GRAVITY 2.6
#define SHATTER_LAUNCH 0.24
#define SHATTER_LIFT 0.16

struct ShatterFrag {
	/* Three original vertices, relative to the shard's centroid, and their
	 * normalised coordinates within the source crop. The fourth draw vertex
	 * repeats the third, making the quad's second triangle degenerate. */
	double local[6];
	float uv[6];
	double x0, y0, vx, vy, omega;
	double tilt, axis, delay;
	bool dropped;
};

static inline int32_t shatter_frag_count(int32_t n) {
	return 4 * n * (2 * ((n + 2) / 3) - 1);
}

/* Each spoke ends on the pane's perimeter. Shared vertices give exact
 * coverage at t=0; jitter changes the crack pattern without opening holes. */
static void shatter_mesh_vertex(int32_t spoke, int32_t ring, int32_t n,
		double *x, double *y) {
	const double impact_x = 0.43, impact_y = 0.38;
	int32_t rings = (n + 2) / 3;
	spoke %= 4 * n;
	int32_t edge = spoke / n, along = spoke % n;
	double u = (double)along / n;
	if (along != 0)
		u += shatter_jitter((uint32_t)spoke + 73u, -0.32, 0.32) / n;
	double px, py;
	switch (edge) {
	case 0: px = u;       py = 0.0;     break;
	case 1: px = 1.0;     py = u;       break;
	case 2: px = 1.0 - u; py = 1.0;     break;
	default: px = 0.0;     py = 1.0 - u; break;
	}
	double r = (double)ring / rings;
	if (ring > 0 && ring < rings)
		r += shatter_jitter((uint32_t)(spoke * 31 + ring * 977),
			-0.20, 0.20) / rings;
	*x = impact_x + (px - impact_x) * r;
	*y = impact_y + (py - impact_y) * r;
}

static void shatter_frag_init(struct ShatterFrag *f, int32_t index,
		int32_t n, const struct wlr_box *win) {
	int32_t per_spoke = 2 * ((n + 2) / 3) - 1;
	int32_t spoke = index / per_spoke, part = index % per_spoke;
	int32_t ring = (part + 1) / 2;
	double p[4][2];
	shatter_mesh_vertex(spoke, ring, n, &p[0][0], &p[0][1]);
	shatter_mesh_vertex(spoke, ring + 1, n, &p[1][0], &p[1][1]);
	shatter_mesh_vertex(spoke + 1, ring + 1, n, &p[2][0], &p[2][1]);
	shatter_mesh_vertex(spoke + 1, ring, n, &p[3][0], &p[3][1]);
	/* Fan at the impact, then two triangles per ring sector. */
	int a = 0, b = 1, c = 2;
	if (part > 0 && !(part & 1)) {
		b = 2; c = 3;
	}
	const int vertices[3] = {a, b, c};
	double cx = (p[a][0] + p[b][0] + p[c][0]) / 3.0;
	double cy = (p[a][1] + p[b][1] + p[c][1]) / 3.0;
	f->x0 = win->x + cx * win->width;
	f->y0 = win->y + cy * win->height;
	for (int k = 0; k < 3; k++) {
		f->uv[k * 2] = (float)p[vertices[k]][0];
		f->uv[k * 2 + 1] = (float)p[vertices[k]][1];
		f->local[k * 2] = (p[vertices[k]][0] - cx) * win->width;
		f->local[k * 2 + 1] = (p[vertices[k]][1] - cy) * win->height;
	}
	double span = ASTEROIDZ_MIN(win->width, win->height);
	double dx = (cx - 0.43) * win->width, dy = (cy - 0.38) * win->height;
	double distance = hypot(dx, dy);
	uint32_t seed = (uint32_t)index + 1u;
	double speed = span * SHATTER_LAUNCH * shatter_jitter(seed + 977u, 0.65, 1.25);
	f->vx = distance > 0.0 ? dx / distance * speed : 0.0;
	f->vy = (distance > 0.0 ? dy / distance * speed : 0.0) - span * SHATTER_LIFT;
	f->omega = shatter_jitter(seed + 5231u, -1.8, 1.8);
	f->tilt = shatter_jitter(seed + 319u, -4.5, 4.5);
	f->axis = shatter_jitter(seed + 71u, 0.0, 6.283185307179586);
	f->delay = 0.055 + fmin(distance / span, 1.5) * 0.035;
	f->dropped = false;
}

static inline double shatter_motion_at(const struct ShatterFrag *f, double t) {
	return fmax(0.0, t - f->delay) / (1.0 - f->delay);
}

static inline void shatter_centre_at(const struct ShatterFrag *f, double t,
		double span, double *out_x, double *out_y) {
	double u = shatter_motion_at(f, t);
	*out_x = f->x0 + f->vx * u;
	*out_y = f->y0 + f->vy * u + 0.5 * SHATTER_GRAVITY * span * u * u;
}

static inline double shatter_angle_at(const struct ShatterFrag *f, double t) {
	return f->omega * shatter_motion_at(f, t);
}

static inline void shatter_corners_at(const struct ShatterFrag *f, double t,
		double span, float out[8]) {
	double cx, cy;
	shatter_centre_at(f, t, span, &cx, &cy);
	double angle = shatter_angle_at(f, t), cs = cos(angle), sn = sin(angle);
	double ax = cos(f->axis), ay = sin(f->axis);
	double tilt = cos(f->tilt * shatter_motion_at(f, t));
	/* A short crack phase: seams open before the pieces start falling. */
	double crack = fmin(fmax(t / 0.08, 0.0), 1.0);
	double shrink = 1.0 - 0.018 * crack * crack * (3.0 - 2.0 * crack);
	for (int k = 0; k < 3; k++) {
		double x = f->local[k * 2] * shrink, y = f->local[k * 2 + 1] * shrink;
		/* Orthographic tumble about an in-plane axis; UVs stay attached to
		 * the glass as it turns edge-on and exposes its reverse face. */
		double along = x * ax + y * ay;
		double across = (-x * ay + y * ax) * tilt;
		x = along * ax - across * ay;
		y = along * ay + across * ax;
		out[k * 2] = (float)(cx + x * cs - y * sn);
		out[k * 2 + 1] = (float)(cy + x * sn + y * cs);
	}
	out[6] = out[4]; out[7] = out[5];
}

static inline float shatter_light_at(const struct ShatterFrag *f, double t) {
	double u = shatter_motion_at(f, t);
	double facing = fabs(cos(f->tilt * u));
	/* A restrained glint near edge-on, with no brightness jump at impact. */
	return (float)(1.0 - 0.18 * (1.0 - facing)
		+ 0.38 * pow(1.0 - facing, 6.0) * fmin(u * 12.0, 1.0));
}

#ifndef SHATTER_MATH_ONLY
/*
 * ── THE EMITTER, AND HOW THE RENDERER RECOGNISES IT ───────────────────────
 *
 * EVERYTHING BELOW NEEDS THE COMPOSITOR -- wl_list, Monitor, wlr_scene_buffer.
 * Everything above needs a dvec-free struct wlr_box and a min macro, which is
 * what lets the trajectory be driven with no display and
 * no scene graph. The guard is what keeps that promise honest: the test
 * defines SHATTER_MATH_ONLY and gets exactly the arithmetic.
 *
 * One scene node stands for the whole cloud: a wlr_scene_buffer in LyrFadeOut
 * holding the window's snapshot buffer. The AVK walker expands it into one
 * textured triangle per fragment; every other consumer sees an ordinary buffer
 * node and treats it as one, which is what keeps damage, culling and the
 * layer ordering working without knowing anything about shatter.
 *
 * A REGISTRY RATHER THAN A TAG IN node->data. `data` already carries a Client
 * pointer for other node kinds (see text-node.c), so reading a magic number
 * out of it would mean reinterpreting whatever that pointer happens to be --
 * type punning that is undefined and, worse, occasionally right. The list is
 * short (the number of windows closing at once, normally zero or one), the
 * scan is a pointer compare, and it cannot be fooled.
 */
struct ShatterEmitter {
	struct wl_list link;
	/* The node the walker matches on. Borrowed: the scene owns it. */
	struct wlr_scene_buffer *marker;
	/*
	 * The monitor the window's PIXELS were on when it broke, resolved from its
	 * geometry rather than taken from Client.mon.
	 *
	 * Those two disagree, and the disagreement is silent: a window moved to
	 * another output keeps its assigned monitor until something reassigns it,
	 * so a fadeout client can claim a monitor its pixels are nowhere near. The
	 * trespass rule below asks "is this fragment on a monitor that is not
	 * home?", and with the wrong home the answer was yes for every fragment on
	 * the first tick -- the entire cloud retired before it was ever drawn.
	 */
	Monitor *home;
	struct ShatterFrag *frags;
	int32_t nfrags;
	/* Progress at the last tick, and the scale its constants are in. The
	 * walker does not evaluate the trajectory -- the CPU already did, at this
	 * output's instant (ADR-612 Model A) -- it reads the corners computed
	 * there. */
	double span;
	float opacity;
	double progress;
	/* Corners for every fragment, in LAYOUT pixels, refreshed each tick:
	 * 8 floats per fragment. Kept here rather than recomputed in the walker so
	 * that two outputs drawing the same frame cannot disagree about where a
	 * fragment is. */
	float *corners;
};

static struct wl_list shatter_emitters;
static bool shatter_emitters_ready;

static void shatter_registry_init(void) {
	if (!shatter_emitters_ready) {
		wl_list_init(&shatter_emitters);
		shatter_emitters_ready = true;
	}
}

/* The renderer's question: is this buffer node a fragment cloud? */
static struct ShatterEmitter *shatter_emitter_for(
		const struct wlr_scene_buffer *buf) {
	if (!shatter_emitters_ready || buf == NULL) {
		return NULL;
	}
	struct ShatterEmitter *e;
	wl_list_for_each(e, &shatter_emitters, link) {
		if (e->marker == buf) {
			return e;
		}
	}
	return NULL;
}
#endif /* SHATTER_MATH_ONLY */
