/*
 * Gradient material: where a gradient's parameters and colours live on the GPU,
 * and how a draw finds its own.
 *
 * M4C.1 is the DATA ARCHITECTURE only. The colour lookup here resolves to the
 * first stop, which is what AVK has shipped since M3 -- the point of this stage
 * is that the record and every one of its colours have arrived correctly, not
 * that the ramp is evaluated. M4C.2 replaces the lookup; M4C.3 adds conic.
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
 * The colour this fragment takes.
 *
 * PREMULTIPLIED, and not converted on the way in or out: wlr_scene_rect stores
 * its colours premultiplied, the gradient stops are the same values from the
 * same producers, and AVK's blend state is (ONE, ONE_MINUS_SRC_ALPHA). One
 * convention the whole way through.
 *
 * NO DERIVATIVE IS TAKEN HERE, and none should be added. A gradient is a point
 * function -- it needs no fwidth() -- and the count/type tests below are
 * branches, so a derivative underneath one of them would be evaluated in
 * non-uniform control flow the moment anything made them per-fragment. See
 * rounded.glsl for what that costs.
 */
vec4 az_gradient(int rec, vec2 box_pos, vec2 box_size) {
	int offset = int(az_grad.data[rec + 1].y);
	int count = int(az_grad.data[rec + 1].z);

	if (count <= 0) {
		return vec4(0.0);
	}
	/* M4C.1: the first stop, unconditionally. Every other field of the record
	 * is uploaded and is asserted by the packing test, but nothing reads it
	 * yet. */
	return az_grad.data[offset];
}
