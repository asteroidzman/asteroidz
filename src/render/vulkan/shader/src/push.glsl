/*
 * THE push constant block. One declaration, included by every AVK shader.
 *
 * It is a file rather than four copies because M4C made the block
 * DUAL-PURPOSE: the first two vec4s mean one thing to a texture command and a
 * different thing to a gradient one. Four independently maintained copies of a
 * struct whose fields change meaning by pipeline is a silent-corruption bug
 * waiting for someone to add a field to three of them.
 *
 * WHY IT IS DUAL-PURPOSE. The block is exactly 128 bytes, which is the
 * guaranteed minimum maxPushConstantsSize -- every Vulkan implementation must
 * support that much and nothing more. There is no room to grow, so a gradient's
 * parameters live in the two vec4s a rectangle never uses, and its colours live
 * in a storage buffer (see gradient.glsl).
 *
 *   field          TEXTURE                    RECT              GRADIENT RECT
 *   -------------  -------------------------  ----------------  --------------
 *   uv_org_dx.x    uv origin x                --                --
 *                  (SHADOW: dither amplitude -- see shadow.frag)
 *   uv_org_dx.y    uv origin y                --                --
 *   uv_org_dx.zw   du/dx                      --                --
 *   uv_dy.xy       du/dy                      --                --
 *   uv_dy.zw       --                         --                --
 *   color.x        LUMINANCE SCALE MINUS ONE  --                --
 *                  (M5/C2 -- zero is the identity, see AZ_TEX_LUM_SCALE)
 *   color.yzw      --                         premultiplied     .a gates only
 *   params.x       opacity                    opacity           opacity
 *   params.y       alpha mask (1 keep, 0 opaque)  --            RECORD INDEX
 *   params.zw      viewport, in output pixels (all)
 *   round_box      OUTER x0, y0, x1, y1, in output pixels (all)
 *   corners        OUTER radii, CLOCKWISE tl, tr, br, bl (all)
 *   inner_box      INNER cut-out box (all)
 *   inner_corners  INNER radii, CLOCKWISE (all)
 *
 * uv_dy.zw is the TARGET ORIGIN, and it is the one field every pipeline shares.
 * See AZ_TARGET_ORIGIN below.
 *
 * `params.y` is written by three pipelines now -- texture's alpha mask,
 * gradient's record index, shadow's blur sigma -- and `uv_org_dx.x` by two.
 * None of them ever draws the same command as another. AZ_GRAD_RECORD,
 * AZ_SHADOW_SIGMA and AZ_SHADOW_DITHER name each reading so the overlay is
 * spelled out at the point of use rather than inferred from this comment.
 */

layout(push_constant) uniform Push {
	vec4 uv_org_dx;
	vec4 uv_dy;
	vec4 color;
	vec4 params;
	vec4 round_box;     // OUTER x0, y0, x1, y1 in output pixels
	vec4 corners;       // CLOCKWISE: tl, tr, br, bl, in output pixels
	vec4 inner_box;     // INNER x0, y0, x1, y1 in output pixels
	vec4 inner_corners; // CLOCKWISE, in output pixels
} pc;

/*
 * WHERE THIS TARGET'S TOP-LEFT SITS IN OUTPUT/SCENE SPACE.
 *
 * Zero when rendering into the output itself. Nonzero when a command range is
 * being replayed into a REGIONAL transient -- a blur's scene prefix, say, which
 * covers a 640x360 window of a 3840x2160 scene.
 *
 * THE COORDINATE CONTRACT, in one place:
 *
 *     scene/global space   every geometry field in this block: round_box,
 *                          inner_box, corners. ALWAYS global. A command does
 *                          not know or care which target it lands in.
 *
 *     target-local space   gl_FragCoord, the viewport, the scissor, and
 *                          params.zw. These describe the attachment.
 *
 *     target_pixel  =  scene_pixel - AZ_TARGET_ORIGIN
 *
 * The vertex shader applies that once, and every fragment shader that needs to
 * measure against a geometry field converts back with az_frag_global(). There
 * is deliberately no third space and no per-shader offset arithmetic.
 *
 * uv_dy.zw because it is the only slot free in ALL FIVE pipelines: a rect and a
 * gradient use neither uv vector, a texture uses uv_dy.xy, a shadow uses
 * uv_dy.x, and a blur uses uv_dy.xy. Anything else would have needed a
 * different field per pipeline, which is how an overlay drifts.
 */
#define AZ_TARGET_ORIGIN pc.uv_dy.zw

/*
 * This fragment's position in OUTPUT/SCENE raster space.
 *
 * EVERY existing gl_FragCoord user wants this rather than the attachment's own
 * coordinates, and each for its own reason:
 *
 *   rounded.glsl   measures a signed distance against round_box, which is global
 *   shadow.frag    evaluates the caster's SDF against a global box
 *   gradient.glsl  normalises within round_box, so a local origin would RESTART
 *                  the ramp at the transient's edge
 *   dither.glsl    is deliberately anchored to the output raster, so a local
 *                  origin would PHASE-SHIFT the noise at the transient's edge
 *
 * The last two are the dangerous ones: both still produce a plausible picture
 * when wrong, and both differ only where a regional target begins.
 *
 * Guarded because gl_FragCoord does not exist in a vertex stage, and this file
 * is the one declaration all six shaders share. quad.vert defines
 * AZ_VERTEX_STAGE before including it.
 */
#ifndef AZ_VERTEX_STAGE
vec2 az_frag_global(void) {
	return gl_FragCoord.xy + AZ_TARGET_ORIGIN;
}
#endif

/* Index of this command's gradient record in the storage buffer, in vec4
 * units. Written by the CPU as a float and exact for every count a scene can
 * hold: floats represent integers exactly below 2^24. */
#define AZ_GRAD_RECORD int(pc.params.y)

/*
 * M5/C2. The source's luminance scale, on the TEXTURE pipeline only.
 *
 * `color` is unused by a texture draw -- a texture's colour comes from its
 * sampler -- so the scale rides in .x rather than growing a block that is
 * already exactly 128 bytes, the guaranteed minimum maxPushConstantsSize.
 *
 * STORED AS scale MINUS ONE, SO THAT ZERO IS THE IDENTITY.
 *
 * The obvious encoding is the scale itself, and it is a trap: every
 * `struct avk_push_constants pc = {0}` in the tree then renders BLACK unless
 * its author knew to set this field. That is not hypothetical -- it was tried.
 * The blur composite draws through pipes.texture, the blur chain's own passes
 * share this block, and binding pipes.texture
 * directly: four call sites, three of them nowhere near anything called
 * "colour", and every blurred region on screen went black. Fixing it meant
 * four defensive assignments and a comment at each, all of which stay correct
 * only until the fifth caller.
 *
 * With the offset, a zeroed block is neutral and no caller has to know. The
 * cost is one add per texel.
 *
 * IT MULTIPLIES RGB AND NOT ALPHA. The texel is premultiplied, and a luminance
 * scale changes how bright the surface is, not how opaque: scaling the whole
 * vec4 would make a half-transparent window more opaque as it got brighter.
 * Above 1.0 this deliberately produces rgb > a, which is what an HDR source
 * above SDR white IS -- an 8-bit attachment clamps it, and ADR-007's claim that
 * it does not is what F5 struck.
 */
#define AZ_TEX_LUM_SCALE (1.0 + pc.color.x)
