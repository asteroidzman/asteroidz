/*
 * THE ANALYTIC BLURRED-ROUNDED-BOX COVERAGE, shared.
 *
 * Factored out of shadow.frag when M4F's blur gained `edge_softness`, which is
 * not "a similar falloff" -- it is THE SAME ONE. The reference says so in code:
 * texture_soft_edge.frag's coverage is box_shadow.frag's, line for line, with
 * `edge_softness` substituted for `blur_sigma`, and asteroidz's own producer
 * hands the blur node the shadow's sigma verbatim (src/animation/client.h:757).
 *
 * A second copy in a second shader would be two four-sample integrations that
 * agree today. They are meant to fade in lockstep -- a blur node deliberately
 * sized to a shadow's footprint, so the two blend into one continuous halo --
 * and a seam between them is exactly what a drifted copy produces.
 *
 * Nothing in here reads a push constant or gl_FragCoord: it is pure arithmetic
 * over its arguments, so both callers keep their own conventions about which
 * push-constant slot the sigma arrived in.
 */

const float AZ_PI = 3.141592653589793;

float az_gaussian(float x, float sigma) {
	return exp(-(x * x) / (2.0 * sigma * sigma)) / (sqrt(2.0 * AZ_PI) * sigma);
}

/*
 * Abramowitz & Stegun 7.1.26, the approximation the reference uses. Accurate
 * to about 5e-4, which is well under a step of an 8-bit channel.
 */
vec2 az_erf(vec2 x) {
	vec2 s = sign(x), a = abs(x);
	x = 1.0 + (0.278393 + (0.230389 + 0.078108 * (a * a)) * a) * a;
	x *= x;
	return s - s / (x * x);
}

/*
 * Blurred coverage of one scanline, with independent radii for the left and
 * right edge of that scanline.
 *
 * `y` is the offset from the caster's centre, so the caller has already
 * decided which pair of corners applies.
 */
float az_shadow_x(float x, float y, float sigma, float corner_l,
		float corner_r, vec2 half_size) {
	float delta_l = min(half_size.y - corner_l - abs(y), 0.0);
	float delta_r = min(half_size.y - corner_r - abs(y), 0.0);
	/* Where the rounded edge actually is on this scanline: the straight edge,
	 * pulled in by however much of the corner arc this row cuts through. */
	float curved_l = half_size.x - corner_l
		+ sqrt(max(0.0, corner_l * corner_l - delta_l * delta_l));
	float curved_r = half_size.x - corner_r
		+ sqrt(max(0.0, corner_r * corner_r - delta_r * delta_r));
	vec2 integral = 0.5 + 0.5
		* az_erf((x + vec2(-curved_l, curved_r)) * (sqrt(0.5) / sigma));
	return integral.y - integral.x;
}

/*
 * `radii` is CLOCKWISE from the top left -- tl, tr, br, bl -- which is struct
 * fx_corner_radii's order and struct avk_cmd::corners's order. The reference's
 * own helper takes (tl, tr, bl, br), and handing it a clockwise struct swaps
 * the two bottom corners: invisible on a symmetric window, wrong on every
 * titlebar-joined one. One convention in AVK, named where it is unpacked.
 */
float az_box_shadow(vec2 lower, vec2 upper, vec2 point, float sigma,
		vec4 radii) {
	float r_tl = radii.x, r_tr = radii.y, r_br = radii.z, r_bl = radii.w;

	vec2 center = (lower + upper) * 0.5;
	vec2 half_size = (upper - lower) * 0.5;
	point -= center;

	/* The Gaussian is negligible beyond 3 sigma, and outside the caster's own
	 * extent there is nothing to integrate, so the samples go where the signal
	 * is instead of being spread over an arbitrary window. */
	float low = point.y - half_size.y;
	float high = point.y + half_size.y;
	float start = clamp(-3.0 * sigma, low, high);
	float end = clamp(3.0 * sigma, low, high);

	float step_size = (end - start) / 4.0;
	float y = start + step_size * 0.5;
	float value = 0.0;
	for (int i = 0; i < 4; i++) {
		float sy = point.y - y;
		/* Negative y is the TOP of the box: gl_FragCoord is top-down and so is
		 * this box, so a sample above the centre takes the top corners. */
		float corner_l = sy < 0.0 ? r_tl : r_bl;
		float corner_r = sy < 0.0 ? r_tr : r_br;
		value += az_shadow_x(point.x, sy, sigma, corner_l, corner_r, half_size)
			* az_gaussian(y, sigma) * step_size;
		y += step_size;
	}

	return value;
}
