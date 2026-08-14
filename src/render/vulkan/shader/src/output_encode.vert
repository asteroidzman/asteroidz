#version 450

/*
 * The output-encode pass's vertex stage. Contract C6.
 *
 * A FULLSCREEN TRIANGLE, not a quad, and its own file rather than quad.vert.
 *
 * Its own file because this pass has a DIFFERENT push-constant layout from
 * every other AVK pipeline. push.glsl's block is exactly 128 bytes -- the
 * guaranteed minimum maxPushConstantsSize -- and is already dual-purpose
 * across four pipelines; there is no room in it for a 3x3 matrix and four
 * scalars, and overlaying them onto fields that mean something else in a
 * texture command is how an overlay table stops being maintainable. This pass
 * is the last thing in the frame and shares nothing with the commands before
 * it, so it takes its own 64-byte block and quad.vert stays as it is.
 *
 * A triangle rather than a quad because a quad's diagonal seam makes the two
 * halves separate primitives, and fragments on the seam are rasterised by both
 * -- a real cost on a pass that covers every damaged pixel of a 4K output. One
 * oversized triangle covers the viewport with no seam. There are no vertex
 * buffers here either: the position is a function of gl_VertexIndex, so the
 * draw is vkCmdDraw(cb, 3, 1, 0, 0) with nothing bound.
 *
 * DAMAGE IS A SCISSOR, NOT GEOMETRY. The triangle always covers the whole
 * viewport; the damage rects are set as scissors and the draw is repeated per
 * rect, exactly as the blur passes do. Fragments outside the scissor are never
 * generated, and the attachment is LOADed rather than cleared, so undamaged
 * pixels keep what the previous frame left there.
 */

layout(location = 0) out vec2 v_uv;

void main() {
	/* 0 -> (0,0), 1 -> (2,0), 2 -> (0,2): a triangle twice the viewport in
	 * each direction, whose [0,1] sub-square is exactly the viewport. */
	v_uv = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
	gl_Position = vec4(v_uv * 2.0 - 1.0, 0.0, 1.0);
}
