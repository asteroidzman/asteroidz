/*
 * The spring, per axis — the arithmetic P1 rests on, driven directly.
 *
 * M6A/ADR-608 gave a retarget velocity continuity through ONE scalar, obtained
 * by projecting the outgoing velocity onto the new direction of travel. P1
 * replaced it with one spring per axis. This file is where the difference
 * between those two is a number rather than an opinion.
 *
 * WHAT IS ASSERTED, AND WHY EACH ONE CAN FAIL
 *
 *   REDUCTION. spring_curve_velocity_at_v(t, 0) must reproduce the v0 == 0
 *   expression that shipped before it, to within a few ulp -- the general form
 *   multiplies the same factors in a different order, and IEEE multiplication
 *   is not associative, so bit equality is not available and is not asked for.
 *   Every animation on the desktop is the v0 == 0 case, so a general form that
 *   is genuinely different there changes motion nobody asked to change.
 *
 *   THE DERIVATIVE IS THE DERIVATIVE. spring_curve_velocity_at_v is compared
 *   against a central finite difference of calculate_spring_curve_at_v over a
 *   sweep of damping, frequency, v0 and t. This is an INDEPENDENT derivation:
 *   the closed form was differentiated by hand, and a hand-differentiated
 *   closed form is exactly the kind of thing that is wrong in one branch. Both
 *   branches -- critically damped and underdamped -- are swept.
 *
 *   PURITY. The curve is evaluated over a grid forwards and then backwards and
 *   the two must agree bit for bit. A spring that accumulated state between
 *   calls -- the per-frame integration ADR-616 forbids -- fails here, and it is
 *   the only property in this file that catches that class directly.
 *
 *   SAMPLING-GRID IDENTITY, PER AXIS. 48Hz and 240Hz walks of the same
 *   trajectory must return identical positions at the instants the two grids
 *   share, on all four axes carrying four different v0s. Closed-form evaluation
 *   makes this true by construction; that is the point, because it is the
 *   construction ADR-616 requires.
 *
 *   PER-AXIS SEED CONTINUITY, AND THE CASE THAT DISCRIMINATES. The seeding
 *   arithmetic must hand the new segment the outgoing speed of each axis, in
 *   px/ms, exactly. The interesting configuration is a 90-degree retarget of
 *   DIAGONAL motion: travelling (+x,+y), redirected along (+x,-y). There the
 *   projection onto the new direction is IDENTICALLY ZERO while both per-axis
 *   velocities are large -- so the scalar starts from rest and the per-axis
 *   seeding does not.
 *
 *   That zero is asserted too, not assumed. A fixture whose discriminating case
 *   does not actually discriminate is a fixture that passes for the wrong
 *   reason, so this file proves the premise before it relies on it.
 *
 * WHAT THIS FILE DOES NOT COVER, AND WHERE IT IS COVERED INSTEAD
 *
 * seed_axis() below is the seeding ARITHMETIC, written out here so it can be
 * driven without a compositor. It is not client.h's copy of it. So this file
 * proves the arithmetic is right; it cannot prove the compositor performs it,
 * and it never sees AZ_BREAK_ANIM_SPRING_SCALAR_V0 at all.
 *
 * That half belongs to contrib/anim-vector-continuity-test.sh, which drives a
 * real retarget through a real compositor, finite-differences the trace, and
 * is run against the break switch to confirm it goes red. Neither half is
 * sufficient alone: this one would pass on a compositor that ignored the
 * arithmetic entirely, and that one cannot sweep damping and frequency.
 */

#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <float.h>

/*
 * The two things animation/spring.h asks of its includer. The compositor
 * supplies a Config with a hundred other fields and a dvec2 from asteroidz.c;
 * the spring reads exactly these, which is what makes it testable at all.
 */
struct dvec2 {
	double x;
	double y;
};

static struct {
	double spring_damping;
	double spring_frequency;
} config;

#include "animation/spring.h"

static int failures;
static int checks;

static void fail(const char *what, double got, double want, double tol) {
	failures++;
	fprintf(stderr, "FAIL %s: got %.17g want %.17g (tol %.3g, delta %.3g)\n",
		what, got, want, tol, fabs(got - want));
}

static void near(const char *what, double got, double want, double tol) {
	checks++;
	if (!(fabs(got - want) <= tol) || !isfinite(got)) {
		fail(what, got, want, tol);
	}
}

static void exact(const char *what, double got, double want) {
	checks++;
	if (got != want) {
		fail(what, got, want, 0.0);
	}
}

/*
 * The damping/frequency corners the config schema permits: damping is clamped
 * to [0.1, 2.0] and frequency to [4, 60] (config-schema.h). Both sides of the
 * zeta == 1 branch are covered, and the boundary itself.
 *
 * NOTE that 1.5 and 2.0 do not exercise an overdamped solution, because
 * spring.h does not have one -- it routes every zeta >= 1 to the critically
 * damped form. These entries check that the shipped branch is self-consistent
 * at those settings, which is a genuine property; they do not check the
 * physics of overdamping, which is not implemented. See spring.h.
 */
static const double ZETAS[] = {0.1, 0.4, 0.75, 0.99, 1.0, 1.5, 2.0};
static const double OMEGAS[] = {4.0, 12.0, 18.0, 35.0, 60.0};
static const double V0S[] = {-40.0, -7.5, -1.0, 0.0, 1.0, 7.5, 40.0};

/* ── REDUCTION AND THE DERIVATIVE ───────────────────────────────────────── */

static void test_reduction_and_derivative(void) {
	for (size_t zi = 0; zi < sizeof(ZETAS) / sizeof(*ZETAS); zi++) {
		for (size_t oi = 0; oi < sizeof(OMEGAS) / sizeof(*OMEGAS); oi++) {
			config.spring_damping = ZETAS[zi];
			config.spring_frequency = OMEGAS[oi];

			/*
			 * v0 == 0 must reproduce the shipped expression. It is recomputed
			 * here rather than called, so that a change to the general form
			 * cannot silently redefine what it is compared to.
			 *
			 * TO WITHIN ROUNDING, NOT BIT FOR BIT. The general form multiplies
			 * the same factors in a different ORDER -- exp * (k * sin) against
			 * (k * exp) * sin -- and IEEE multiplication is not associative,
			 * so the two disagree in the last ulp. The first version of this
			 * demanded equality and failed 7098 times on differences of 1e-39.
			 * A relative epsilon of a few ulp still catches what matters: a
			 * genuine error in the derivation moves the result by a factor,
			 * not by a rounding.
			 */
			for (double t = 0.0; t < 1.0; t += 1.0 / 512.0) {
				double want;
				double z = config.spring_damping;
				double w = config.spring_frequency;
				if (z >= 1.0) {
					want = w * w * t * exp(-w * t);
				} else {
					double wd = w * sqrt(1.0 - z * z);
					want = (w * w / wd) * exp(-z * w * t) * sin(wd * t);
				}
				double tol = 8.0 * DBL_EPSILON * fabs(want);
				near("velocity_at_v(t,0) == the v0==0 closed form",
					spring_curve_velocity_at_v(t, 0.0), want, tol);
				/* This one IS exact: a wrapper that only forwards must not
				 * introduce arithmetic of its own. */
				exact("velocity_at(t) delegates to velocity_at_v(t,0)",
					spring_curve_velocity_at(t),
					spring_curve_velocity_at_v(t, 0.0));
			}

			for (size_t vi = 0; vi < sizeof(V0S) / sizeof(*V0S); vi++) {
				double v0 = V0S[vi];

				/* f'(0) IS v0: that is the definition the seeding arithmetic
				 * inverts, so if it does not hold, every seed is wrong. */
				near("f'(0; v0) == v0",
					spring_curve_velocity_at_v(0.0, v0), v0, 1e-9);

				/* Central difference, away from both ends so the window stays
				 * inside the domain where the closed form is defined. */
				const double h = 1e-6;
				for (double t = 0.02; t < 0.97; t += 0.01) {
					double up = calculate_spring_curve_at_v(t + h, v0).y;
					double dn = calculate_spring_curve_at_v(t - h, v0).y;
					double fd = (up - dn) / (2.0 * h);
					double an = spring_curve_velocity_at_v(t, v0);
					/* Relative tolerance: at omega 60 the derivative reaches
					 * several thousand, where an absolute epsilon would be
					 * meaningless in one direction and unmeetable in the
					 * other. */
					double tol = 1e-5 * (1.0 + fabs(an));
					near("analytic derivative == finite difference",
						an, fd, tol);
				}
			}
		}
	}
}

/* ── PURITY ─────────────────────────────────────────────────────────────── */

static void test_purity(void) {
	config.spring_damping = 0.75;
	config.spring_frequency = 18.0;

	enum { N = 601 };
	static double fwd[N];
	static double rev[N];

	for (int i = 0; i < N; i++) {
		fwd[i] = calculate_spring_curve_at_v(i / (double)(N - 1), 3.25).y;
	}
	for (int i = N - 1; i >= 0; i--) {
		rev[i] = calculate_spring_curve_at_v(i / (double)(N - 1), 3.25).y;
	}
	for (int i = 0; i < N; i++) {
		exact("evaluation order does not change the curve", rev[i], fwd[i]);
	}
}

/* ── SAMPLING-GRID IDENTITY, PER AXIS ───────────────────────────────────── */

/*
 * One axis of the trajectory, evaluated the way client.h evaluates it: a
 * normalised time, the curve, and a lerp between the segment's own endpoints.
 */
static double axis_at_ms(double passed_ms, double duration_ms,
		double from, double to, double v0) {
	double linear = passed_ms / duration_ms;
	double f = calculate_spring_curve_at_v(linear, v0).y;
	return from + (to - from) * f;
}

static void test_grid_identity(void) {
	config.spring_damping = 0.62;
	config.spring_frequency = 22.0;

	const double duration = 300.0;
	/* Four axes, four different v0s -- the whole point of P1. If any axis were
	 * still reading a shared scalar, the four columns below would move
	 * together and the per-axis expectations would not be met. */
	const double from[4] = {100.0, 240.0, 800.0, 600.0};
	const double to[4]   = {900.0,  60.0, 400.0, 950.0};
	const double v0[4]   = {  6.5,  -4.0,   0.0,  11.25};

	/*
	 * THE SHARED INSTANTS ARE CONSTRUCTED, NOT ARRIVED AT.
	 *
	 * The first version of this walked one grid at 1000/48 ms and the other at
	 * 1000/240 ms and compared the instants: they disagree in the last bits
	 * (83.333333333333343 against 83.333333333333329) and every position
	 * comparison then failed by 1e-14 for a reason that has nothing to do with
	 * animation. That is float grid arithmetic, not the property under test.
	 *
	 * The property under test is refresh independence: a 240Hz output and a
	 * 48Hz output that sample THE SAME INSTANT must get the same answer. So
	 * the 240Hz grid is generated once and the 48Hz grid is every fifth
	 * element of it -- coincidence by construction -- and the comparison is
	 * then exact, as it should be for a pure function of its argument.
	 */
	enum { N240 = 72 };
	static double grid[N240];
	for (int j = 0; j < N240; j++) {
		grid[j] = j * (1000.0 / 240.0);
	}

	for (int k = 0; k * 5 < N240; k++) {
		double t = grid[k * 5];      /* a 48Hz instant */
		if (t >= duration) {
			break;
		}
		for (int a = 0; a < 4; a++) {
			/* The 48Hz output reaches this instant having evaluated only
			 * every fifth one; the 240Hz output reaches it having evaluated
			 * all of them. Both call the same closed form with the same
			 * number, so both must return the same bits. */
			double p48 = axis_at_ms(t, duration, from[a], to[a], v0[a]);
			for (int fill = k * 5 + 1; fill < (k + 1) * 5 && fill < N240;
					fill++) {
				(void)axis_at_ms(grid[fill], duration, from[a], to[a], v0[a]);
			}
			double p240 = axis_at_ms(t, duration, from[a], to[a], v0[a]);
			exact("48Hz and 240Hz agree at a shared instant", p240, p48);
		}
	}

	/* And the axes really are independent: with distinct v0s, the normalised
	 * progress of axis 0 and axis 1 must differ somewhere in mid-flight. A
	 * build that collapsed them to one scalar would pass every equality above
	 * and fail this. */
	bool differ = false;
	for (double t = 0.05; t < 0.95; t += 0.01) {
		double f0 = calculate_spring_curve_at_v(t, v0[0]).y;
		double f1 = calculate_spring_curve_at_v(t, v0[1]).y;
		if (fabs(f0 - f1) > 1e-6) {
			differ = true;
			break;
		}
	}
	checks++;
	if (!differ) {
		failures++;
		fprintf(stderr, "FAIL axes with different v0 produced identical "
			"curves -- the per-axis state is not reaching the curve\n");
	}
}

/* ── PER-AXIS SEED CONTINUITY ───────────────────────────────────────────── */

/*
 * The seeding arithmetic from client.h, isolated: the outgoing speed of one
 * axis in px/ms, converted into the normalised initial velocity the new
 * segment's curve needs.
 */
static double seed_axis(double old_span, double u, double old_v0,
		double new_span, double duration) {
	double px_per_ms = old_span * spring_curve_velocity_at_v(u, old_v0)
		/ duration;
	if (fabs(new_span) <= 0.5) {
		return 0.0;
	}
	return px_per_ms * duration / new_span;
}

/* The outgoing speed of one axis, in px/ms. */
static double outgoing_px_per_ms(double old_span, double u, double old_v0,
		double duration) {
	return old_span * spring_curve_velocity_at_v(u, old_v0) / duration;
}

static void test_seed_continuity(void) {
	config.spring_damping = 0.75;
	config.spring_frequency = 18.0;

	const double duration = 400.0;
	/*
	 * ── WHERE "MID-FLIGHT" ACTUALLY IS ───────────────────────────────────
	 *
	 * u = 0.37 was the first choice, on the assumption that a third of the way
	 * through is the fast part. At the shipped defaults it is not: with
	 * frequency 18 and damping 0.75 the spring has already overshot and is
	 * travelling BACKWARDS by then (f'(0.37) is about -0.18), so both seeded
	 * velocities came out with the opposite sign and the test failed for a
	 * reason that had nothing to do with the code under test.
	 *
	 * The velocity peaks near wd*t = atan(wd / zw), which is u ~ 0.06 here --
	 * the same "the spring finishes in the first quarter and the rest is dead
	 * tail" the pacing work found. A retarget fixture that wants a moving
	 * window has to catch it early.
	 */
	const double u = 0.06;

	/*
	 * ── THE DISCRIMINATING CASE, AND WHY THE OBVIOUS ONE IS NOT ──────────
	 *
	 * The obvious spelling of "90-degree retarget" is a window travelling +x
	 * redirected to a target straight up: old span (D,0), new span (0,D).
	 * THAT CASE PROVES NOTHING. The outgoing velocity is entirely in x, and
	 * the new segment has ZERO x span -- so x cannot carry a velocity under
	 * any scheme, per-axis or scalar, because its position is initial + 0*f(t)
	 * regardless. Meanwhile y had no outgoing velocity to preserve. Old and
	 * new implementations agree on every number, and a fixture built on it
	 * goes green against the broken code.
	 *
	 * The case that discriminates turns DIAGONAL motion through 90 degrees:
	 * travelling (+x,+y), redirected along (+x,-y). Both axes have an outgoing
	 * velocity AND both have somewhere to spend it, while the new direction is
	 * still perpendicular to the old one -- so the projection onto it vanishes
	 * exactly and the scalar scheme starts from rest.
	 *
	 * Equal magnitudes on both axes are what make that vanishing exact. Do not
	 * "simplify" this back to the axis-aligned case.
	 */
	const double old_span_x = 600.0, old_span_y = 600.0;
	const double new_span_x = 500.0, new_span_y = -500.0;

	double vx = outgoing_px_per_ms(old_span_x, u, 0.0, duration);
	double vy = outgoing_px_per_ms(old_span_y, u, 0.0, duration);

	/* ── THE PREMISE ──────────────────────────────────────────────────────
	 * The outgoing velocity must be genuinely moving, or "velocity survives"
	 * is a statement about zero. */
	checks++;
	if (!(fabs(vx) > 0.1 && fabs(vy) > 0.1)) {
		failures++;
		fprintf(stderr, "FAIL premise: outgoing velocity is not moving "
			"(vx=%.6g vy=%.6g px/ms)\n", vx, vy);
	}

	/* ── THE PREMISE THAT MAKES THE CASE DISCRIMINATE ─────────────────────
	 * The projection of the outgoing velocity onto the new direction of
	 * travel is exactly zero here. This is what the scalar seeding would
	 * compute, and it is why the scalar starts the new segment from rest. */
	{
		double len = sqrt(new_span_x * new_span_x + new_span_y * new_span_y);
		double along = (vx * new_span_x + vy * new_span_y) / len;
		near("the scalar projection vanishes on a 90-degree turn",
			along, 0.0, 1e-9);
	}

	/* ── AND THE PER-AXIS SEEDS DO NOT ────────────────────────────────────
	 * Each axis is handed back exactly the speed it had. The equality is the
	 * continuity claim: new_span * f'(0) / duration == the outgoing px/ms,
	 * and f'(0) is the seeded v0. */
	{
		double sx = seed_axis(old_span_x, u, 0.0, new_span_x, duration);
		double sy = seed_axis(old_span_y, u, 0.0, new_span_y, duration);

		near("axis x re-enters at its outgoing speed",
			new_span_x * spring_curve_velocity_at_v(0.0, sx) / duration,
			vx, 1e-9);
		near("axis y re-enters at its outgoing speed",
			new_span_y * spring_curve_velocity_at_v(0.0, sy) / duration,
			vy, 1e-9);

		/* Signs: x continues with the turn, y is now travelling against its
		 * new span and must therefore be seeded negative -- it decelerates
		 * through the turn instead of snapping round. */
		checks++;
		if (!(sx > 0.0 && sy < 0.0)) {
			failures++;
			fprintf(stderr, "FAIL seeded signs: sx=%.6g sy=%.6g -- expected "
				"x positive (with the turn), y negative (against it)\n",
				sx, sy);
		}
	}

	/* ── AN AXIS WITH NOWHERE TO GO ───────────────────────────────────────
	 * A new segment with no span on an axis cannot express a velocity there:
	 * its position is initial + 0 * f(t) whatever v0 says. Zero, not a
	 * division by nearly nothing. */
	exact("a zero-span axis seeds zero",
		seed_axis(600.0, u, 0.0, 0.0, duration), 0.0);
	exact("a sub-pixel-span axis seeds zero",
		seed_axis(600.0, u, 0.0, 0.25, duration), 0.0);

	/* ── CHAINED RETARGETS ────────────────────────────────────────────────
	 * The segment being left may itself carry a v0. Differentiating the
	 * v0 == 0 curve would report the speed of a curve that was never on
	 * screen, so the two must differ -- and the seed must remain continuous
	 * against the curve that WAS on screen. */
	{
		double old_v0 = 9.0;
		double with = outgoing_px_per_ms(700.0, u, old_v0, duration);
		double without = outgoing_px_per_ms(700.0, u, 0.0, duration);
		checks++;
		if (!(fabs(with - without) > 1e-6)) {
			failures++;
			fprintf(stderr, "FAIL a carried v0 made no difference to the "
				"outgoing velocity (%.9g vs %.9g) -- the chained-retarget "
				"case is not being exercised\n", with, without);
		}
		double s = seed_axis(700.0, u, old_v0, 450.0, duration);
		near("a chained retarget is continuous against the live curve",
			450.0 * spring_curve_velocity_at_v(0.0, s) / duration,
			with, 1e-9);
	}
}

int main(void) {
	test_reduction_and_derivative();
	test_purity();
	test_grid_identity();
	test_seed_continuity();

	if (failures) {
		fprintf(stderr, "\n%d of %d checks FAILED\n", failures, checks);
		return 1;
	}
	printf("ok - %d checks passed\n", checks);
	return 0;
}
