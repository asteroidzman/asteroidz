/*
 * ── THE SPRING, AND NOTHING ELSE ──────────────────────────────────────────
 *
 * Split out of animation/common.h so it can be driven directly by
 * a display. Everything here is a pure function of its
 * arguments and the two configured spring constants; it touches no scene, no
 * client, no clock, and allocates nothing.
 *
 * THE INCLUDER SUPPLIES TWO THINGS, deliberately rather than by including
 * headers of its own -- that is what keeps this file free of the compositor:
 *
 *   struct dvec2   an { x, y } pair of doubles (asteroidz.c defines it)
 *   config         anything with `double spring_damping` and
 *                  `double spring_frequency` members
 *
 * The unit test provides its own minimal versions of both, which is how it
 * can sweep damping and frequency without a running compositor.
 *
 * Damped spring step response, with an optional initial velocity.
 * zeta >= 1 is critically damped (no overshoot), lower values bounce.
 *
 * ── THE PHYSICS THESE CURVES SOLVE ────────────────────────────────────────
 *
 * A Hooke's-law spring with viscous damping: m y'' = -k (y - 1) - c y'. Divided
 * through and written in the standard parametrisation,
 *
 *   y'' + 2 z w y' + w^2 (y - 1) = 0,   y(0) = 0,  y'(0) = v0
 *
 * where w = sqrt(k/m) is the undamped natural frequency (config
 * spring_frequency) and z = c / (2 sqrt(km)) is the damping ratio (config
 * spring_damping). The window is the mass, the target is the rest length, and
 * `1` is the normalised distance between where the motion started and where it
 * is going. Both closed forms below are that equation's step response, and
 * Each is checkable against a finite difference of the
 * other -- which is what makes "it is the standard solution" a checked claim.
 *
 * ── THE ONE PLACE THIS IS NOT THE PHYSICS ─────────────────────────────────
 *
 * zeta > 1 is genuinely OVERDAMPED: the roots are real and distinct and the
 * solution is a sum of two decaying exponentials with no trigonometric part.
 * This file does not implement that case. It routes every zeta >= 1 to the
 * CRITICALLY damped form, which is the zeta == 1 solution.
 *
 * The consequence is worth knowing before touching the config: the schema
 * clamps spring_damping to [0.1, 2.0], so every value above 1.0 is accepted,
 * stored, and then behaves exactly like 1.0. The motion is still monotone and
 * still settles, so nothing looks broken -- the slider simply stops responding
 * over the top half of its range. Implementing the overdamped branch would
 * change the motion of anyone who has set a damping above 1.0, which is why it
 * is recorded here rather than quietly fixed.
 *
 * `v0` is dy/dt at t = 0, in the same normalised time as t. Zero gives the
 * ordinary step response a fresh animation wants -- the window is at rest and
 * starts moving. Non-zero is what makes a RETARGET continuous in velocity
 * (M6A/ADR-608): a target arriving mid-flight starts a new spring that is
 * already moving at the speed the old one had reached, instead of stopping
 * dead and setting off again.
 *
 * Both closed forms below reduce to the previous ones at v0 == 0 -- to within
 * a few ulp, since the general form multiplies the same factors in a different
 * order. That is the property to preserve when touching this, because every
 * existing animation on the desktop is the v0 == 0 case.
 *
 *   critically damped   y = 1 - e^-wt (1 + (w - v0) t)
 *   underdamped         y = 1 - e^-zwt (cos wd t + C sin wd t),
 *                       C = (zw - v0) / wd
 */
static struct dvec2 calculate_spring_curve_at_v(double t, double v0) {
	struct dvec2 point = {.x = t, .y = 1.0};
	double zeta = config.spring_damping;
	double omega = config.spring_frequency;

	if (t >= 1.0)
		return point;
	if (zeta >= 1.0) {
		point.y = 1.0 - exp(-omega * t) * (1.0 + (omega - v0) * t);
	} else {
		double wd = omega * sqrt(1.0 - zeta * zeta);
		point.y = 1.0 - exp(-zeta * omega * t) *
			(cos(wd * t) + ((zeta * omega - v0) / wd) * sin(wd * t));
	}
	return point;
}

/*
 * dy/dt of the SAME curve, which is what a retarget must sample to know how
 * fast the outgoing motion was going.
 *
 * Analytic rather than a finite difference of stored integers: the geometry it
 * would difference is rounded to whole pixels, so at a 144Hz sample spacing the
 * quotient is dominated by the rounding rather than the motion.
 *
 * `v0` is the initial velocity the curve BEING DIFFERENTIATED started with.
 * Zero is the ordinary case -- a first retarget leaves a segment that began at
 * rest. It is not the only case: the segment being left may itself be a
 * retargeted one carrying its own v0, and differentiating the v0 == 0 curve
 * would then report the speed of a curve that was never on screen. Chained
 * retargets are not exotic; every redirected drag is one.
 *
 *   critically damped   dy/dt = e^-wt [ w(w - v0) t + v0 ]
 *   underdamped         dy/dt = e^-zwt [ v0 cos wd t
 *                                        + ((w^2 - zw v0)/wd) sin wd t ]
 *
 * Both reduce EXACTLY to the previous v0 == 0 expressions, which is the
 * property spring_curve_velocity_at() below now depends on.
 */
static double spring_curve_velocity_at_v(double t, double v0) {
	double zeta = config.spring_damping;
	double omega = config.spring_frequency;
	if (t >= 1.0 || t < 0.0)
		return 0.0;
	if (zeta >= 1.0)
		return exp(-omega * t) * (omega * (omega - v0) * t + v0);
	double wd = omega * sqrt(1.0 - zeta * zeta);
	return exp(-zeta * omega * t)
		* (v0 * cos(wd * t)
			+ ((omega * omega - zeta * omega * v0) / wd) * sin(wd * t));
}

static double spring_curve_velocity_at(double t) {
	return spring_curve_velocity_at_v(t, 0.0);
}

static struct dvec2 calculate_spring_curve_at(double t, int32_t type) {
	(void)type;
	return calculate_spring_curve_at_v(t, 0.0);
}
