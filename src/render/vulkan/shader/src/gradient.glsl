/*
 * Gradient material: where a gradient's parameters and colours live on the GPU,
 * and how a draw finds its own.
 *
 * Two facts about the reference are worth stating before any of the layout,
 * because they are what the layout is shaped by:
 *
 * 1. THERE ARE NO STOP POSITIONS. A gradient is a count and a colour array;
 *    spacing is derived from the count alone. There is no position array to
 *    carry, and inventing one would be a different feature that looks similar.
 *
 * 2. THE COUNT IS ARBITRARY. The GLES reference holds colours in a uniform
 *    array with a compile-time length and RELINKS the shader program whenever a
 *    scene wants more stops than the current build allows
 *    (link_quad_grad_program(..., count + 1) in fx_pass.c). The Vulkan
 *    translation of that would be a pipeline per stop count. A storage buffer
 *    has no length in the shader at all, so there is one pipeline for every
 *    gradient that will ever be drawn.
 */

layout(std430, set = 1, binding = 0) readonly buffer AzGradientData {
	vec4 data[];
} az_grad;

/*
 * One flat vec4 array holds both records and colours, and a record says where
 * its own colours are:
 *
 *   data[r + 0] = origin.x, origin.y, degree in RADIANS, blend
 *   data[r + 1] = type, colour offset (vec4 units), colour count, --
 *   ...
 *   data[offset .. offset + count - 1] = the colours, PREMULTIPLIED
 *
 * One array rather than two bindings because there is then one capacity, one
 * growth path and one upload to reason about, and a gradient's colours are
 * written next to the record that describes them.
 *
 * Radians rather than degrees because the CPU converts once per command and the
 * shader would otherwise convert once per fragment. The CPU-side snapshot keeps
 * DEGREES, which is the source semantic; this is the GPU's copy of it.
 */
#define AZ_GRADIENT_LINEAR 1
#define AZ_GRADIENT_CONIC  2

/*
 * The scalar coordinate, in the rectangle's OWN normalised box.
 *
 * `box_pos`/`box_size` are the rect in output pixels, the space gl_FragCoord is
 * in, so a gradient is a property of the rectangle and not of the screen: the
 * same window renders the same ramp wherever it is moved to.
 *
 * LINEAR, exactly as the reference derives it:
 *
 *     uv = normal - origin
 *     uv *= 1 / (|cos| + |sin|)
 *     step = uv.x*cos - uv.y*sin + origin.x
 *
 * The 1/(|cos| + |sin|) scale is what keeps the ramp spanning the box at every
 * angle; without it a 45-degree gradient runs out before the far corner.
 *
 * THE ORIGIN IS NOT DECORATIVE, and it is not conic-only. Expanding the last
 * line gives
 *
 *     step = k*cos*normal.x - k*sin*normal.y
 *          + origin.x*(1 - k*cos) + origin.y*(k*sin)
 *
 * so the origin contributes a CONSTANT OFFSET along the ramp, which vanishes
 * only at degree 0 (where k = 1, cos = 1, sin = 0). At 90 degrees the offset is
 * origin.x + origin.y, which is why a fixture that moves the origin along the
 * anti-diagonal -- (0.2, 0.8) against (0.5, 0.5) -- sees no change at all and
 * proves nothing.
 *
 * That offset is also why `step` genuinely leaves 0..1: with an off-centre
 * origin part of the rectangle sits past the end of the ramp. The reference
 * indexes out of range there. See az_gradient_color().
 *
 * CONIC IS A DIFFERENT COORDINATE, not the linear one rescaled:
 *
 *     uv = rotate(normal - origin, rad)
 *     step = -atan(uv.y, uv.x)/PI * 0.5 + 0.5
 *
 * and the three ways it differs are all observable:
 *
 *   - the origin is the CENTRE the ramp turns about, so moving it moves the
 *     whole pattern rather than sliding it along an axis;
 *   - the ramp WRAPS, and meets itself at a seam;
 *   - there is no 1/(|cos| + |sin|) scale, and there should not be. The angle
 *     is measured in the box's own unit square, so a conic gradient on a wide
 *     rectangle covers an ellipse's worth of angle rather than a circle's.
 *     That is the reference's shape.
 *
 * THE SEAM RUNS IN -X FROM THE ORIGIN, where atan2 flips between +PI and -PI:
 * step approaches 0 just below that ray and 1 just above it. NEITHER MODE
 * INTERPOLATES ACROSS IT. Banded jumps from the last band to the first;
 * interpolated does too, because the last segment ends AT the last colour and
 * nothing wraps round to the first. A conic gradient whose first and last
 * stops differ therefore has a hard edge in it by construction, and that is
 * the reference's behaviour rather than an artifact.
 *
 * The branch is on `type`, which comes from the record and is uniform across
 * the draw.
 */
float az_gradient_step(vec2 box_pos, vec2 box_size, vec2 origin, float rad,
		int type) {
	vec2 normal = (az_frag_global() - box_pos) / box_size;
	vec2 uv = normal - origin;

	if (type == AZ_GRADIENT_CONIC) {
		uv = vec2(uv.x * cos(rad) - uv.y * sin(rad),
			uv.x * sin(rad) + uv.y * cos(rad));
		return -atan(uv.y, uv.x) / 3.14159265 * 0.5 + 0.5;
	}

	uv *= vec2(1.0) / vec2(abs(cos(rad)) + abs(sin(rad)));
	return uv.x * cos(rad) - uv.y * sin(rad) + origin.x;
}

/*
 * The colour at `step`.
 *
 * PREMULTIPLIED, and not converted on the way in or out: wlr_scene_rect stores
 * its colours premultiplied, the gradient stops are the same values from the
 * same producers, and AVK's blend state is (ONE, ONE_MINUS_SRC_ALPHA). One
 * convention the whole way through.
 *
 * THE TWO MODES PUT THE SAME COLOURS IN DIFFERENT PLACES, which is the single
 * most confusable thing about this feature:
 *
 *   blend off   count bands, each 1/count wide.       Colour k fills
 *               [k/count, (k+1)/count). No interpolation anywhere, including
 *               at the boundaries.
 *   blend on    count-1 segments, each 1/(count-1) wide, smoothstepped between
 *               neighbours. Colour k sits AT k/(count-1) -- an endpoint, not a
 *               band centre.
 *
 * REFERENCE BUG FIXED IN AVK: terminal gradient interpolation clamps to the
 * last valid colour.
 *
 * The reference's blend path reads `colors[ind + 1]` guarded by
 * `if (ind <= count - 1)`, which is true on the LAST segment -- so at the final
 * endpoint it indexes colors[count], one past the array. The value is whatever
 * the driver leaves there, and reproducing it byte for byte would not be parity
 * with anything. Every index here is clamped instead: for every coordinate the
 * reference defines this gives the identical colour, and at the endpoint it
 * gives the last colour, which is what the guard was plainly written to
 * produce.
 *
 * The same clamp covers coordinates outside 0..1, which the origin offset
 * above genuinely produces and which the reference also indexes out of range
 * for -- in the banded path as well as the blended one.
 *
 * floor() rather than the reference's int(), which truncates toward zero. They
 * differ only for negative coordinates, where truncation gives 0 and floor
 * gives -1, and the clamp then puts both at 0. Same answer, no special case.
 *
 * NO DERIVATIVE IS TAKEN HERE, and none should be added. A gradient is a point
 * function -- it needs no fwidth() -- and the branches below would then be
 * non-uniform control flow around one. See rounded.glsl for what that costs.
 */
vec4 az_gradient_color(int offset, int count, bool blend, float step_) {
	if (count <= 0) {
		return vec4(0.0);
	}
	/* Uniform across the draw: count comes from the record, which is per
	 * command. One colour has no ramp and no count-1 to divide by. */
	if (count == 1) {
		return az_grad.data[offset];
	}

	if (!blend) {
		float sf = 1.0 / float(count);
		int ind = clamp(int(floor(step_ / sf)), 0, count - 1);
		return az_grad.data[offset + ind];
	}

	float sf = 1.0 / float(count - 1);
	int ind = clamp(int(floor(step_ / sf)), 0, count - 1);
	float at = float(ind) * sf;

	vec4 c = az_grad.data[offset + ind];
	if (ind > 0) {
		c = mix(az_grad.data[offset + ind - 1], c,
			smoothstep(at - sf, at, step_));
	}
	/* min(): the clamp described above. The reference has ind + 1 here with no
	 * upper bound, and on the last segment that is colors[count]. */
	int next = min(ind + 1, count - 1);
	return mix(c, az_grad.data[offset + next], smoothstep(at, at + sf, step_));
}

/* The whole material, given a record and the rectangle it covers. */
vec4 az_gradient(int rec, vec2 box_pos, vec2 box_size) {
	vec2 origin = az_grad.data[rec + 0].xy;
	float rad = az_grad.data[rec + 0].z;
	bool blend = az_grad.data[rec + 0].w > 0.5;
	int type = int(az_grad.data[rec + 1].x);
	int offset = int(az_grad.data[rec + 1].y);
	int count = int(az_grad.data[rec + 1].z);

	return az_gradient_color(offset, count, blend,
		az_gradient_step(box_pos, box_size, origin, rad, type));
}
