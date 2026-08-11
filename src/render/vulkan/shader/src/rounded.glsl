/*
 * Per-corner rounded-rectangle coverage.
 *
 * Ported deliberately closely from SceneFX's corner_alpha.frag, because M4's
 * requirement is visual PARITY with the renderer this replaces -- a formulation
 * that is merely "also correct" would differ on the antialiased boundary of
 * every window on screen.
 *
 * THREE THINGS THAT LOOK LIKE DETAIL AND ARE NOT
 *
 * 1. The distance is the MAX of four corner-anchored SDFs, not a quadrant
 *    select. Selecting a radius by quadrant looks equivalent and produces a
 *    discontinuity along the seam wherever two adjacent corners have different
 *    radii -- which is the normal case here, since a window joined to its
 *    titlebar is rounded on two corners and square on the other two.
 *
 * 2. Antialiasing is DERIVATIVE-scaled: fwidth(dist), not a fixed 1.0 band.
 *    That gives ~1 device pixel of smoothing however the box is scaled, so a
 *    window at output scale 1.5 and the same window in an overview thumbnail
 *    both get a one-pixel edge. SceneFX's comment records that this was itself
 *    ported FROM the Vulkan renderer; going back to a fixed band would undo it.
 *
 * 3. `is_cutout` inverts the coverage, which is how a border's inner edge is
 *    rounded: the same primitive, subtracting instead of adding. M4B uses it;
 *    M4A only ever passes false.
 *
 * ARGUMENT ORDER IS A TRAP. struct fx_corner_radii is laid out CLOCKWISE --
 * top_left, top_right, bottom_right, bottom_left -- and says so in a comment.
 * SceneFX's own shader function takes them in a DIFFERENT order:
 * (tl, tr, bl, br). Passing the struct straight through swaps the two bottom
 * corners, which on a titlebar-joined window is subtle enough to survive
 * review. This function therefore takes ONE vec4 in the struct's clockwise
 * order and names the components where they are unpacked, so there is exactly
 * one convention in AVK.
 */

float az_corner_dist(vec2 q, float radius) {
	return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - radius;
}

/*
 * `pos`/`size` are the rectangle in OUTPUT PIXELS, matching gl_FragCoord.
 * `radii` is CLOCKWISE from the top left: tl, tr, br, bl.
 * Returns coverage in 0..1. Zero radii return 1.0 without touching a
 * derivative, which is the common case and the reason this is cheap.
 */
float az_rounded_coverage(vec2 pos, vec2 size, vec4 radii, bool is_cutout) {
	float r_tl = radii.x, r_tr = radii.y, r_br = radii.z, r_bl = radii.w;

	if (r_tl <= 0.0 && r_tr <= 0.0 && r_br <= 0.0 && r_bl <= 0.0) {
		return is_cutout ? 0.0 : 1.0;
	}

	vec2 p = gl_FragCoord.xy - pos;
	if (p.x < 0.0 || p.y < 0.0 || p.x > size.x || p.y > size.y) {
		/* Outside the rectangle entirely. Not `discard`: AVK's caller has
		 * already scissored the draw to this box, and returning coverage keeps
		 * this function free of control flow the caller cannot see. */
		return is_cutout ? 1.0 : 0.0;
	}

	/* Each corner measured against a box anchored at the OPPOSITE corner, so
	 * every one of the four is a distance from the same rectangle expressed in
	 * that corner's own frame. */
	vec2 q_tl = abs(p - size) - size + r_tl;
	vec2 q_tr = abs(p - vec2(0.0, size.y)) - size + r_tr;
	vec2 q_bl = abs(p - vec2(size.x, 0.0)) - size + r_bl;
	vec2 q_br = abs(p) - size + r_br;

	float dist = max(
		max(az_corner_dist(q_tl, r_tl), az_corner_dist(q_tr, r_tr)),
		max(az_corner_dist(q_bl, r_bl), az_corner_dist(q_br, r_br)));

	float aa = max(fwidth(dist), 1e-4);
	float result = smoothstep(0.0, aa, dist);
	return is_cutout ? result : 1.0 - result;
}
