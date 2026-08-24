/*
 * ── SHATTER: THE TRAJECTORY, AS A CLOSED FORM ─────────────────────────────
 *
 * The window breaks into a square grid of fragments, each of which is thrown
 * outward, falls under gravity, and tumbles. This file is the whole of the
 * MOTION: pure functions of the fragment's index and the elapsed time, with no
 * state, no scene, no renderer and no clock.
 *
 * ADR-616 REQUIRES THIS SHAPE, and it is worth saying why rather than citing
 * it. The obvious way to write a break-up is to keep a velocity per fragment
 * and add gravity to it once per frame. That makes the trajectory a function of
 * HOW MANY FRAMES HAVE BEEN DRAWN, so the same close finishes in different
 * places on a 60Hz and a 144Hz output, drifts on a VRR panel, and -- because
 * rendermon ticks every client on every output's pass -- runs at the SUM of the
 * refresh rates when a window straddles two monitors. None of that is
 * hypothetical -- it is what an advance-per-frame animation does.
 *
 * So the position is evaluated, never advanced:
 *
 *     p(t) = p0 + v0*t + 0.5*g*t^2
 *     theta(t) = theta0 + omega*t
 *
 * with t the elapsed WALL-CLOCK time at this output's own presentation
 * instant. Two outputs sampling the same instant get the same answer; an output
 * that misses a frame lands where it should be, not one frame behind. It also
 * means a fixture can replay a logged seed and instant and recompute a
 * fragment's corners exactly, which is the purity gate.
 *
 * ── WHY THE CONSTANTS ARE NOT CONFIGURABLE ────────────────────────────────
 *
 * Gravity, launch speed and spin are internal, with deterministic jitter. Only
 * the fragment COUNT is a setting. A spin rate a user can set is a spin rate a
 * user can set to something that does not look like breaking glass, and the
 * three of them are not independent -- the launch speed that reads as "thrown"
 * depends on the gravity that reads as "falling". Exposing one without the
 * others would be exposing a number that only works at its default.
 *
 * ── UNITS ────────────────────────────────────────────────────────────────
 *
 * Time is the animation's NORMALISED progress, 0 at the break and 1 at the end
 * of the close duration -- not seconds. Distances are in output pixels, and
 * speeds and gravity are therefore in pixels per unit progress. That keeps a
 * 350ms close and a 900ms close the same SHAPE rather than the same
 * acceleration, which is what a duration setting is for.
 *
 * Sizes scale with the window's smaller dimension, so a thumbnail and a
 * fullscreen window come apart the same way -- the same reasoning
 * init_fallout_shards() applies to `fall`.
 */

/* Deterministic noise in [lo,hi). A hash and not rand(): a window has to come
 * apart the same way twice, or the purity gate cannot replay it and the whole
 * closed-form argument above is unobservable. Same construction as ast_jitter
 * in asteroid-break.h. */
static double shatter_jitter(uint32_t seed, double lo, double hi) {
	seed = seed * 1664525u + 1013904223u;
	seed ^= seed >> 16;
	seed *= 0x7feb352du;
	seed ^= seed >> 15;
	return lo + (hi - lo) * ((double)(seed & 0xffffffu) / (double)0xffffffu);
}

/*
 * The physics, as fractions of the window's smaller dimension per unit
 * progress. Chosen together; see above.
 *
 * SHATTER_GRAVITY is what makes it fall rather than drift, and it is large
 * relative to the launch speed on purpose: a fragment thrown upward should
 * turn over well before the animation ends, or the cloud reads as an explosion
 * in space rather than glass hitting a floor.
 */
#define SHATTER_GRAVITY      3.2
#define SHATTER_LAUNCH       0.55   /* outward, before jitter */
#define SHATTER_LAUNCH_JIT   0.30   /* +/- fraction of SHATTER_LAUNCH */
#define SHATTER_LIFT         0.45   /* upward bias, so it breaks before it drops */
#define SHATTER_SPIN         2.6    /* radians per unit progress, before jitter */

/* One fragment's constant terms: everything about it that does not change once
 * the window has broken. Filled at break time, read at every tick. */
struct ShatterFrag {
	/* The source sub-rectangle, in the snapshot image's own pixels. */
	float sx, sy, sw, sh;
	/* Where this fragment starts, in LAYOUT pixels: the centre of its tile. */
	double x0, y0;
	/* Half-extents, so a corner is centre +/- a rotated half-diagonal. */
	double hw, hh;
	/* Launch velocity and spin, in pixels and radians per unit progress. */
	double vx, vy, omega;
	double theta0;
	/* Set once a fragment has entered another monitor; see the trespass rule
	 * in client.h. A dropped fragment stays dropped -- it must not flicker
	 * back when its centre happens to leave the neighbour again. */
	bool dropped;
};

/*
 * The centre of a fragment at normalised time t. THE closed form.
 *
 * `span` is the window's smaller dimension, the scale every constant above is
 * expressed in.
 */
static inline void shatter_centre_at(const struct ShatterFrag *f, double t,
		double span, double *out_x, double *out_y) {
	*out_x = f->x0 + f->vx * t;
	*out_y = f->y0 + f->vy * t + 0.5 * (SHATTER_GRAVITY * span) * t * t;
}

/* The fragment's rotation at normalised time t. Also closed form: a constant
 * angular velocity, so theta is linear in t. */
static inline double shatter_angle_at(const struct ShatterFrag *f, double t) {
	return f->theta0 + f->omega * t;
}

/*
 * The four corners of a fragment at time t, in LAYOUT pixels, in the order
 * AVK_CMD_TEXTURE_QUAD wants them: top-left, top-right, bottom-left,
 * bottom-right of the UNROTATED tile, each carried round by the rotation.
 *
 * Writing the corners rather than a centre and an angle is what lets the
 * renderer stay a dumb consumer: it places four points and samples a
 * rectangle, and every question about where a fragment IS has already been
 * answered here, on the CPU, at this output's instant (ADR-612's Model A).
 */
static inline void shatter_corners_at(const struct ShatterFrag *f, double t,
		double span, float out[8]) {
	double cx, cy;
	shatter_centre_at(f, t, span, &cx, &cy);
	double th = shatter_angle_at(f, t);
	double c = cos(th), s = sin(th);

	/* The four half-diagonals of the unrotated tile, then rotated. */
	static const double sx[4] = {-1.0, +1.0, -1.0, +1.0};
	static const double sy[4] = {-1.0, -1.0, +1.0, +1.0};
	for (int i = 0; i < 4; i++) {
		double dx = sx[i] * f->hw;
		double dy = sy[i] * f->hh;
		out[i * 2 + 0] = (float)(cx + dx * c - dy * s);
		out[i * 2 + 1] = (float)(cy + dx * s + dy * c);
	}
}

/*
 * The speed bound the energy gate checks against.
 *
 * |v(t)| <= |v0| + g*t exactly, because the only acceleration is constant
 * gravity: v(t) = v0 + g*t, and the triangle inequality is tight when v0 is
 * parallel to g. A fragment that ever exceeds this is not on the trajectory
 * this file describes -- which is what a reintroduced per-frame integrator
 * would look like from the outside.
 */
static inline double shatter_speed_bound(const struct ShatterFrag *f, double t,
		double span) {
	double v0 = sqrt(f->vx * f->vx + f->vy * f->vy);
	return v0 + (SHATTER_GRAVITY * span) * t;
}

/*
 * Fill one fragment's constants. `col`/`row` index the grid, `n` is the grid
 * size per axis, `win` is the window in layout pixels and `img_w`/`img_h` the
 * snapshot image's own size.
 *
 * The seed is derived from the grid position alone, so the same window at the
 * same size breaks identically every time -- and a fixture that logs the seed
 * can recompute every corner without the compositor.
 */
static inline void shatter_frag_init(struct ShatterFrag *f, int32_t col,
		int32_t row, int32_t n, const struct wlr_box *win,
		double img_w, double img_h) {
	/* Edges derived from the window bounds so rounding leaves no seam and the
	 * last row/column reaches the far edge exactly -- the same derivation
	 * init_fallout_shards() uses. */
	double x0 = (double)win->width * col / n;
	double x1 = (double)win->width * (col + 1) / n;
	double y0 = (double)win->height * row / n;
	double y1 = (double)win->height * (row + 1) / n;

	f->hw = (x1 - x0) * 0.5;
	f->hh = (y1 - y0) * 0.5;
	f->x0 = (double)win->x + (x0 + x1) * 0.5;
	f->y0 = (double)win->y + (y0 + y1) * 0.5;

	/* The source rect, in image pixels: the same fractional position within
	 * the window, scaled to the snapshot's own resolution. A window rendered
	 * at a fractional scale has an image larger than its layout box, and
	 * sampling it with layout coordinates would show the wrong part of it. */
	f->sx = (float)(x0 * img_w / (double)win->width);
	f->sy = (float)(y0 * img_h / (double)win->height);
	f->sw = (float)((x1 - x0) * img_w / (double)win->width);
	f->sh = (float)((y1 - y0) * img_h / (double)win->height);

	double span = (double)ASTEROIDZ_MIN(win->width, win->height);
	uint32_t seed = (uint32_t)(row * n + col + 1);

	/*
	 * Outward from the window's centre, normalised.
	 *
	 * Normalised FIRST, so speed comes from the jitter and not from how far
	 * out a fragment happens to sit -- otherwise corner pieces leave at twice
	 * the speed of edge ones and the cloud comes out diamond-shaped. That is
	 * the same trap init_fallout_shards() documents, and it is repeated here
	 * because it is repeated in the geometry.
	 */
	double dx = f->x0 - ((double)win->x + (double)win->width * 0.5);
	double dy = f->y0 - ((double)win->y + (double)win->height * 0.5);
	double len = sqrt(dx * dx + dy * dy);
	if (len < 1.0) {
		/* Dead centre has no direction to go: give it a deterministic one
		 * rather than divide by nearly zero. */
		double a = shatter_jitter(seed + 31u, 0.0, 6.283185307179586);
		dx = cos(a);
		dy = sin(a);
	} else {
		dx /= len;
		dy /= len;
	}

	double speed = span * SHATTER_LAUNCH
		* (1.0 + shatter_jitter(seed + 977u,
			-SHATTER_LAUNCH_JIT, SHATTER_LAUNCH_JIT));
	f->vx = dx * speed;
	/* The upward bias is SUBTRACTED because y grows downward: the cloud has to
	 * rise a little before gravity takes it, or the window looks like it fell
	 * through the floor rather than broke. */
	f->vy = dy * speed - span * SHATTER_LIFT;
	f->omega = SHATTER_SPIN * shatter_jitter(seed + 5231u, -1.0, 1.0);
	f->theta0 = 0.0;
	f->dropped = false;
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
 * textured quad per fragment; every other consumer sees an ordinary buffer
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
	/* The snapshot image's own pixel size, which a fractionally scaled window
	 * does not share with its layout box. */
	double img_w, img_h;
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
