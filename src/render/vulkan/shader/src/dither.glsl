/*
 * Sub-step dither, for effects that are quantised straight into a low-precision
 * attachment.
 *
 * WHY THIS EXISTS. AVK composites directly into the scanout buffer:
 * VK_FORMAT_B8G8R8A8_UNORM, eight bits a channel, no intermediate
 * higher-precision target, blending done in that format. A smooth analytic
 * ramp therefore has only as many output values as eight bits can hold, and a
 * SHALLOW ramp runs out of them. Measured on a real desktop, a window shadow
 * over mid-grey spanned nine values across 130 px, in runs of
 * 41,27,15,11,11,9,8,3,2 pixels -- and the eye reads those run boundaries as
 * concentric contour rings around the window.
 *
 * The ramp is not wrong. There is nowhere to put it. So the fix is to break
 * the correlation between the ramp and the quantiser: perturb by less than one
 * step, with zero mean, so a band boundary becomes a scatter of pixels either
 * side of it that averages back to the ramp that was always there.
 *
 * WHERE THIS BELONGS IN THE END, WHICH IS NOT HERE. The architecturally right
 * place for display-quantisation dither is the final output-encoding stage:
 *
 *     scene-linear FP16 composition
 *         -> tone / gamut mapping
 *         -> output transfer function
 *         -> FINAL quantisation   <- dither belongs HERE
 *
 * because at that point the value being quantised is the composed colour, the
 * destination is known, and one implementation fixes gradients, blur,
 * wallpaper, transparency and HDR->SDR mapping at the same time. M5 builds
 * that pipeline. Until it exists, composition targets 8-bit directly and a
 * shadow can only dither its own contribution -- so this file is deliberately
 * a standalone helper with no shadow in it, ready to be called from the output
 * stage instead of from shadow.frag when that stage arrives.
 */

/*
 * Interleaved gradient noise (Jimenez, "Next Generation Post Processing in
 * Call of Duty: Advanced Warfare", 2014). Returns [0, 1).
 *
 * One dot product, two fracts and a multiply -- no texture, no binding, no
 * table. Chosen over a plain integer hash by measurement, not by reputation:
 * see test_dither_noise_choice(), which scores both on the metric that
 * actually matters here (longest surviving constant-code run) and on whether
 * the result has visible structure.
 *
 * A FUNCTION OF THE OUTPUT PIXEL ALONE. No frame counter, no clock, no
 * animation phase. That is a requirement rather than a simplification:
 *
 *   - a still desktop must be bit-identical frame to frame, or an idle
 *     compositor is generating work and M4C.3H's invariant is broken;
 *   - a partially damaged frame repaints some pixels and not others, so a
 *     pattern that changed per frame would leave the repainted region
 *     visibly out of step with the region beside it;
 *   - and a window moving through a stationary noise field looks right,
 *     whereas noise travelling WITH the window looks like the shadow has
 *     acquired a texture.
 */
float az_dither_noise(vec2 output_pixel) {
	return fract(52.9829189
		* fract(dot(output_pixel, vec2(0.06711056, 0.00583715))));
}

/*
 * Perturb a coverage/alpha value so its quantisation decorrelates.
 *
 * `amplitude` is PEAK-TO-PEAK, in alpha units, and is computed on the CPU from
 * the attachment's precision -- see avk_dither_amplitude(). Zero disables the
 * whole thing, which is what a high-precision target should pass.
 *
 * TWO MODULATIONS, and neither is decoration.
 *
 * THE ENDS. At alpha 0 and alpha 1 the value is exact and there is nothing to
 * decorrelate; perturbing there would sprinkle isolated dark pixels across the
 * fully-transparent margin of every envelope and inside the caster's own
 * cut-out. That artefact is far more obvious than the banding it would be
 * fixing. The taper is a multiply, not a branch, so no fragment in a quad
 * takes a different path.
 *
 * THE GRADIENT. Dither only helps where the ramp is flatter than the
 * quantiser. Where it is steeper -- the contact edge, a rounded silhouette,
 * anywhere the value already moves by more than the dither in a single pixel
 * -- there is no band to break and the noise would only roughen an edge that
 * was clean. `fwidth(alpha) / amplitude` is the ratio of those two, is
 * dimensionless, and needs no tuning: at 1 the ramp is moving as fast as the
 * dither and the dither switches itself off.
 *
 * DERIVATIVE RULE. fwidth() is a 2x2-quad operation and is UNDEFINED in
 * non-uniform control flow. Everything above it here is straight-line, and the
 * only branch is on `amplitude`, which is a push constant and therefore
 * uniform across the draw. contrib/check-shader-derivatives.sh enforces this.
 */
/*
 * A plain integer hash of the pixel coordinate -- white noise, no structure by
 * construction. Two multiplies, an xor, a shift and a convert.
 */
float az_dither_hash(vec2 output_pixel) {
	uvec2 q = uvec2(ivec2(output_pixel));
	uint n = q.x * 1597334673u ^ q.y * 3812015801u;
	n = n * (n ^ (n >> 15));
	return float(n) * (1.0 / 4294967296.0);
}

/* Selected at draw time only while the two are being compared; see
 * test_dither_noise_choice(). */
#ifndef AZ_DITHER_SELECT
#define AZ_DITHER_SELECT pc.uv_dy.x
#endif

float az_dither_sample(vec2 p) {
	return AZ_DITHER_SELECT > 0.5 ? az_dither_hash(p) : az_dither_noise(p);
}

float az_dither_alpha(float alpha, float amplitude) {
	if (amplitude <= 0.0) {
		return alpha;
	}
	float slope = fwidth(alpha);
	float flatness = clamp(1.0 - slope / amplitude, 0.0, 1.0);
	float ends = clamp(min(alpha, 1.0 - alpha) / amplitude, 0.0, 1.0);
	float n = az_dither_sample(gl_FragCoord.xy) - 0.5;
	return clamp(alpha + n * amplitude * flatness * ends, 0.0, 1.0);
}
