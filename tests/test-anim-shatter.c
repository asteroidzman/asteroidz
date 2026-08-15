/*
 * The shatter trajectory — P3's physics, driven directly.
 *
 * animation/shatter.h is a closed form in t and nothing else, which is what
 * ADR-616 requires and what makes a break-up replayable. This file is where
 * that is a measurement rather than a claim.
 *
 * THREE OF P3'S FIVE GATES LIVE HERE, because three of them are statements
 * about arithmetic and need no compositor, no GPU and no display:
 *
 *   PURITY REPLAY. Recompute a fragment's corners from nothing but its seed
 *   and an instant, and get the same numbers -- twice, and in any order. A
 *   per-frame integrator cannot pass this: its answer depends on how many
 *   times it has been called.
 *
 *   GRAVITY RECOVERED. Difference the vertical position over equal spans of
 *   time. Under p = p0 + v0*t + g*t^2/2 the SECOND difference is exactly
 *   g*dt^2 whatever v0 is, so gravity can be read back out of the trajectory
 *   without trusting the constant that produced it. This is an independent
 *   derivation: the test does not call shatter_centre_at's formula, it
 *   measures the curve that formula draws.
 *
 *   ENERGY BOUND. No fragment's speed exceeds |v0| + g*t. Tight rather than
 *   generous -- for constant acceleration that inequality is an equality when
 *   v0 is parallel to g -- so a trajectory that gained speed from anywhere
 *   else fails it.
 *
 * The other two gates -- refresh independence across two outputs, and damage
 * bounded by the cloud AABB -- are properties of the compositor rather than of
 * this arithmetic, and live in the headless fixture.
 */

#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* What shatter.h asks of its includer, supplied here rather than by dragging
 * in the compositor. */
struct wlr_box {
	int x, y, width, height;
};
#define ASTEROIDZ_MIN(a, b) ((a) < (b) ? (a) : (b))

/* The trajectory, without the scene graph the emitter half needs. */
#define SHATTER_MATH_ONLY
#include "animation/shatter.h"

static int failures;
static int checks;

static void near(const char *what, double got, double want, double tol) {
	checks++;
	if (!(fabs(got - want) <= tol) || !isfinite(got)) {
		failures++;
		fprintf(stderr, "FAIL %s: got %.17g want %.17g (tol %.3g)\n",
			what, got, want, tol);
	}
}

static void ok_true(const char *what, bool cond) {
	checks++;
	if (!cond) {
		failures++;
		fprintf(stderr, "FAIL %s\n", what);
	}
}

/* A window big enough that the grid tiles are not degenerate, and not square,
 * so an x/y transposition in the tiling shows up. */
static const struct wlr_box WIN = {.x = 100, .y = 200, .width = 960,
	.height = 600};
#define GRID 6
#define SPAN 600.0   /* the smaller dimension of WIN */

static void build(struct ShatterFrag *f) {
	for (int row = 0; row < GRID; row++) {
		for (int col = 0; col < GRID; col++) {
			shatter_frag_init(&f[row * GRID + col], col, row, GRID, &WIN,
				960.0, 600.0);
		}
	}
}

/* ── PURITY REPLAY ──────────────────────────────────────────────────────── */

static void test_purity(void) {
	struct ShatterFrag a[GRID * GRID], b[GRID * GRID];
	build(a);
	build(b);

	/*
	 * The constants themselves must replay: same grid, same window, same
	 * numbers. If shatter_frag_init consulted anything global this fails.
	 *
	 * FIELD BY FIELD, NOT memcmp. The first version compared the structs
	 * wholesale and failed 25 times out of 36 on two runs of the same
	 * deterministic function -- struct ShatterFrag has padding after its
	 * trailing bool, the arrays are uninitialised stack, and memcmp was
	 * reading whatever was there. The values were identical throughout. A
	 * struct comparison that includes padding is not an equality test.
	 */
	for (int i = 0; i < GRID * GRID; i++) {
		bool same = a[i].sx == b[i].sx && a[i].sy == b[i].sy
			&& a[i].sw == b[i].sw && a[i].sh == b[i].sh
			&& a[i].x0 == b[i].x0 && a[i].y0 == b[i].y0
			&& a[i].hw == b[i].hw && a[i].hh == b[i].hh
			&& a[i].vx == b[i].vx && a[i].vy == b[i].vy
			&& a[i].omega == b[i].omega && a[i].theta0 == b[i].theta0
			&& a[i].dropped == b[i].dropped;
		ok_true("frag constants replay exactly", same);
	}

	/* And the corners must replay at any instant, evaluated in any order.
	 * Forwards first, then backwards over the same instants. */
	static const double ts[] = {0.0, 0.01, 0.13, 0.37, 0.5, 0.66, 0.81, 1.0};
	const int nt = (int)(sizeof(ts) / sizeof(*ts));
	static float fwd[GRID * GRID][8];
	static float rev[GRID * GRID][8];

	for (int k = 0; k < nt; k++) {
		for (int i = 0; i < GRID * GRID; i++) {
			shatter_corners_at(&a[i], ts[k], SPAN, fwd[i]);
		}
		for (int i = GRID * GRID - 1; i >= 0; i--) {
			shatter_corners_at(&a[i], ts[k], SPAN, rev[i]);
		}
		for (int i = 0; i < GRID * GRID; i++) {
			ok_true("corners replay bit-for-bit regardless of order",
				memcmp(fwd[i], rev[i], sizeof(fwd[i])) == 0);
		}
	}

	/* THE PREMISE: the fragments actually moved between those instants. An
	 * implementation that returned the same corners forever would satisfy
	 * every equality above and be entirely wrong. */
	float at0[8], at1[8];
	shatter_corners_at(&a[0], 0.0, SPAN, at0);
	shatter_corners_at(&a[0], 1.0, SPAN, at1);
	double moved = fabs(at1[0] - at0[0]) + fabs(at1[1] - at0[1]);
	ok_true("PREMISE: a fragment moves over the animation", moved > 10.0);
}

/* ── GRAVITY RECOVERED FROM THE TRAJECTORY ──────────────────────────────── */

static void test_gravity(void) {
	struct ShatterFrag f[GRID * GRID];
	build(f);

	/*
	 * The second difference of y over equal spans dt.
	 *
	 *   y(t) = y0 + vy*t + g*t^2/2
	 *   y(t+dt) - y(t)       = vy*dt + g*dt^2/2 + g*t*dt
	 *   second difference    = g*dt^2                 -- vy has cancelled
	 *
	 * So gravity comes back out of the CURVE without the test ever referring
	 * to the launch velocity, and without calling the position formula's own
	 * expression for it.
	 */
	const double dt = 0.05;
	const double expect = (SHATTER_GRAVITY * SPAN) * dt * dt;

	for (int i = 0; i < GRID * GRID; i++) {
		for (double t = 0.0; t < 0.8; t += 0.1) {
			double x0, y0, x1, y1, x2, y2;
			shatter_centre_at(&f[i], t, SPAN, &x0, &y0);
			shatter_centre_at(&f[i], t + dt, SPAN, &x1, &y1);
			shatter_centre_at(&f[i], t + 2 * dt, SPAN, &x2, &y2);
			double second = (y2 - y1) - (y1 - y0);
			near("g recovered from the vertical second difference",
				second, expect, 1e-6 * (1.0 + fabs(expect)));

			/* Horizontal motion has no acceleration at all: x is linear in t,
			 * so its second difference is zero. A gravity term that leaked
			 * into x would show up here and nowhere else. */
			double second_x = (x2 - x1) - (x1 - x0);
			near("x is unaccelerated", second_x, 0.0, 1e-9);
		}
	}
}

/* ── ENERGY / MONOTONICITY BOUND ────────────────────────────────────────── */

static void test_speed_bound(void) {
	struct ShatterFrag f[GRID * GRID];
	build(f);

	const double h = 1e-5;
	for (int i = 0; i < GRID * GRID; i++) {
		for (double t = 0.0; t <= 1.0; t += 0.02) {
			/* Speed as the trajectory actually travels it: a central
			 * difference of position, not the analytic velocity. Measuring the
			 * curve rather than the formula is the point. */
			double xa, ya, xb, yb;
			shatter_centre_at(&f[i], t, SPAN, &xa, &ya);
			shatter_centre_at(&f[i], t + h, SPAN, &xb, &yb);
			double speed = sqrt((xb - xa) * (xb - xa) + (yb - ya) * (yb - ya))
				/ h;
			double bound = shatter_speed_bound(&f[i], t + h, SPAN);
			ok_true("speed never exceeds |v0| + g*t",
				speed <= bound * (1.0 + 1e-6) + 1e-6);
		}
	}

	/* THE PREMISE: the bound is not vacuous. If every fragment crawled, "under
	 * the bound" would be true and meaningless -- so at least one has to get
	 * genuinely close to it. */
	double closest = 0.0;
	for (int i = 0; i < GRID * GRID; i++) {
		double xa, ya, xb, yb;
		shatter_centre_at(&f[i], 0.5, SPAN, &xa, &ya);
		shatter_centre_at(&f[i], 0.5 + h, SPAN, &xb, &yb);
		double speed = sqrt((xb - xa) * (xb - xa) + (yb - ya) * (yb - ya)) / h;
		double frac = speed / shatter_speed_bound(&f[i], 0.5, SPAN);
		if (frac > closest) {
			closest = frac;
		}
	}
	printf("  closest approach to the speed bound: %.3f\n", closest);
	ok_true("PREMISE: the speed bound is approached, not merely respected",
		closest > 0.5);
}

/* ── THE TILING ─────────────────────────────────────────────────────────── */

static void test_tiling(void) {
	struct ShatterFrag f[GRID * GRID];
	build(f);

	/* The source rects must tile the image exactly: no gaps (a seam of
	 * wallpaper through the middle of a fragment) and no overlap. Summed area
	 * is the cheap whole-cover check. */
	double area = 0.0;
	for (int i = 0; i < GRID * GRID; i++) {
		area += (double)f[i].sw * (double)f[i].sh;
	}
	near("the source rects tile the whole image", area, 960.0 * 600.0, 1.0);

	/* And the fragments start where their tiles are: the union of the starting
	 * boxes is the window. Checked at the extremes, which is where an
	 * off-by-one in the edge derivation shows. */
	double min_x = 1e30, max_x = -1e30, min_y = 1e30, max_y = -1e30;
	for (int i = 0; i < GRID * GRID; i++) {
		float c[8];
		shatter_corners_at(&f[i], 0.0, SPAN, c);
		for (int k = 0; k < 4; k++) {
			if (c[k * 2] < min_x) min_x = c[k * 2];
			if (c[k * 2] > max_x) max_x = c[k * 2];
			if (c[k * 2 + 1] < min_y) min_y = c[k * 2 + 1];
			if (c[k * 2 + 1] > max_y) max_y = c[k * 2 + 1];
		}
	}
	near("fragments start flush with the window's left edge", min_x, 100.0, 0.01);
	near("fragments start flush with the window's right edge", max_x, 1060.0, 0.01);
	near("fragments start flush with the window's top edge", min_y, 200.0, 0.01);
	near("fragments start flush with the window's bottom edge", max_y, 800.0, 0.01);
}

int main(void) {
	printf("== shatter trajectory (P3) ==\n");
	test_purity();
	test_gravity();
	test_speed_bound();
	test_tiling();

	if (failures) {
		fprintf(stderr, "\n%d of %d checks FAILED\n", failures, checks);
		return 1;
	}
	printf("ok - %d checks passed\n", checks);
	return 0;
}
