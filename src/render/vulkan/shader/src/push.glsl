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
 *   color          --                         premultiplied     .a gates only
 *   params.x       opacity                    opacity           opacity
 *   params.y       alpha mask (1 keep, 0 opaque)  --            RECORD INDEX
 *   params.zw      viewport, in output pixels (all)
 *   round_box      OUTER x0, y0, x1, y1, in output pixels (all)
 *   corners        OUTER radii, CLOCKWISE tl, tr, br, bl (all)
 *   inner_box      INNER cut-out box (all)
 *   inner_corners  INNER radii, CLOCKWISE (all)
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

/* Index of this command's gradient record in the storage buffer, in vec4
 * units. Written by the CPU as a float and exact for every count a scene can
 * hold: floats represent integers exactly below 2^24. */
#define AZ_GRAD_RECORD int(pc.params.y)
