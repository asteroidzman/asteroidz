#version 450
#extension GL_GOOGLE_include_directive : require

/* push.glsl declares az_frag_global(), which reads gl_FragCoord -- an
 * identifier that does not exist in a vertex stage. */
#define AZ_VERTEX_STAGE
#include "push.glsl"

/*
 * One vertex shader for every command AVK draws.
 *
 * There are no vertex buffers and no index buffers. A quad is four vertices
 * whose positions are a function of gl_VertexIndex, so a draw is
 * vkCmdDraw(cb, 4, 1, 0, 0) with nothing bound -- no per-frame vertex upload,
 * no buffer to keep alive, and no descriptor for geometry.
 *
 * The UV mapping is passed as an origin and two edge vectors rather than a
 * source rectangle plus a transform enum. That is what makes all eight Wayland
 * output transforms, the flipped variants, and a source crop the SAME code
 * path: the CPU folds rotation, flip and crop into two vectors, and the shader
 * has no idea any of it happened. A transform enum in here would be eight
 * branches and eight chances to get a corner wrong.
 */

layout(location = 0) out vec2 v_uv;

void main() {
	vec2 p = vec2(float(gl_VertexIndex & 1), float((gl_VertexIndex >> 1) & 1));
	/* The destination is stored ONCE, in output pixels, and converted here.
	 * The fragment shader measures its signed distances against the same
	 * numbers, so the shape drawn and the shape covered cannot disagree. */
	vec2 px = mix(pc.round_box.xy, pc.round_box.zw, p);
	/* THE ONE CONVERSION. round_box is in scene/global pixels; the attachment
	 * starts at AZ_TARGET_ORIGIN and is params.zw across. Subtracting here, and
	 * only here, is what lets every command be replayed into a regional target
	 * without any of its own numbers changing. */
	gl_Position = vec4((px - AZ_TARGET_ORIGIN) / pc.params.zw * 2.0 - 1.0,
		0.0, 1.0);
	/* Only the texture pipeline means anything by this. For a rectangle the uv
	 * fields are zero and for a gradient they are not uv at all, so v_uv is
	 * computed and then ignored -- see push.glsl. Reading it in quad.frag or
	 * gradient.frag would be reading another pipeline's parameters. */
	v_uv = pc.uv_org_dx.xy + p.x * pc.uv_org_dx.zw + p.y * pc.uv_dy.xy;
}
