#version 450
#extension GL_GOOGLE_include_directive : require

/* push.glsl declares az_frag_global(), which reads gl_FragCoord -- an
 * identifier that does not exist in a vertex stage. */
#define AZ_VERTEX_STAGE
#include "push.glsl"

/*
 * The same quad, with its four corners placed independently.
 *
 * quad.vert derives all four vertices from one axis-aligned rectangle, which
 * is every command AVK had until now. A shatter fragment is not axis-aligned:
 * it tumbles, so its four corners have to be given rather than derived.
 *
 * ── WHERE THE CORNERS LIVE, AND WHY THEY LIVE THERE ──────────────────────
 *
 * In pc.round_box and pc.inner_box, which is not a pun on those names but a
 * consequence of what they mean. The push block is EXACTLY the 128 bytes every
 * Vulkan implementation is required to support, with no room to grow, so a new
 * primitive cannot add fields -- it can only reuse ones its own pipeline does
 * not read.
 *
 * az_rounded_coverage() returns 1.0 immediately when all four radii are zero,
 * without ever touching the box it was handed. A textured quad carries no
 * rounding and no interior cut-out -- pc.corners and pc.inner_corners are both
 * zero -- so texture.frag's two coverage calls early out and the two boxes are
 * dead storage for the whole draw. That is what makes them safe to carry
 * geometry in, and it is why this variant deliberately has no rounding: the
 * moment a radius were nonzero, the fragment shader would read a corner
 * position as a rectangle and shade nonsense.
 *
 *   round_box.xy  corner 0   (p = 0,0)
 *   round_box.zw  corner 1   (p = 1,0)
 *   inner_box.xy  corner 2   (p = 0,1)
 *   inner_box.zw  corner 3   (p = 1,1)
 *
 * The ordering matches gl_VertexIndex's bit pattern, the same one quad.vert
 * uses, so the strip winds identically.
 *
 * ── DEGENERACY IS EXACT ──────────────────────────────────────────────────
 *
 * Given the four corners of an axis-aligned rectangle this produces bit-identical
 * positions to quad.vert. At the vertices p is exactly 0 or 1, so every mix()
 * returns one of its endpoints unchanged rather than an interpolated value --
 * there is no arithmetic to round differently. tests assert that equality
 * rather than assuming it.
 */

layout(location = 0) out vec2 v_uv;

void main() {
	vec2 p = vec2(float(gl_VertexIndex & 1), float((gl_VertexIndex >> 1) & 1));

	/* Bilinear across the four given corners. For a general quadrilateral this
	 * is what the two-triangle strip interpolates anyway; writing it here means
	 * the vertex positions and the rasteriser agree by construction. */
	vec2 top = mix(pc.round_box.xy, pc.round_box.zw, p.x);
	vec2 bot = mix(pc.inner_box.xy, pc.inner_box.zw, p.x);
	vec2 px = mix(top, bot, p.y);

	/* THE ONE CONVERSION, identical to quad.vert: corners are in scene/global
	 * pixels and the attachment starts at AZ_TARGET_ORIGIN, so a quad replays
	 * into a regional target without any of its own numbers changing. */
	gl_Position = vec4((px - AZ_TARGET_ORIGIN) / pc.params.zw * 2.0 - 1.0,
		0.0, 1.0);

	/* Byte-for-byte the line quad.vert uses. The UV mapping is already an
	 * origin plus two edge vectors, which is general enough for any source
	 * crop and any of the eight output transforms, so a rotated DESTINATION
	 * needs no change here at all -- the source mapping is a separate
	 * question from where the corners land. */
	v_uv = pc.uv_org_dx.xy + p.x * pc.uv_org_dx.zw + p.y * pc.uv_dy.xy;
}
