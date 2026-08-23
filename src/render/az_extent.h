/*
 * az_extent.h -- named raster extents, so that two of them cannot be confused.
 *
 * ── WHY THIS FILE EXISTS ──────────────────────────────────────────────────
 *
 * M4F.2C.4d found a defect that no test could see and no compiler could object
 * to, because it was expressed entirely in `int width, height`:
 *
 *     wlr_output_transformed_resolution(output, &width, &height);  // 600x800
 *     ...
 *     if (state->committed & WLR_OUTPUT_STATE_MODE)
 *             width = state->mode->width;                          // 800x600
 *
 * One pair of variables, two coordinate spaces, and the value used to allocate
 * the attachment was also used as the source space of the scene walker's
 * transform. On an 800x600 output at 90 degrees that allocated a 600x800
 * attachment and then mapped every node's geometry through an 800x600 space:
 * the right-hand 200 columns of the desktop were clipped away and the bottom
 * 200 rows of the attachment were never written at all. 455417 of 480000
 * pixels differed from the same desktop drawn at 0 degrees.
 *
 * The types below make that assignment a compile error. `struct
 * az_attachment_extent` and `struct az_presentation_extent` have identical
 * layouts and are still not assignable to one another -- C compares struct
 * types by identity, not by shape -- so the only way to get from one to the
 * other is to name the conversion, and naming it is the whole point.
 *
 * ── THE FOUR SPACES ───────────────────────────────────────────────────────
 *
 * SCENE / LOGICAL      compositor geometry, before the output transform and
 *                      before the scale. wlr_scene node coordinates and the
 *                      output layout live here.
 *
 * PRESENTATION         the output's raster as the user sees it, in device
 *                      pixels: logical size x scale. 90 and 270 SWAP its
 *                      extents relative to the attachment. This is the source
 *                      space of the wlr_box_transform() that maps a node into
 *                      the attachment, and it is wlr_scene's
 *                      `trans_width`/`trans_height`.
 *
 * ATTACHMENT / MODE    the physical raster backing the output image: the
 *                      buffer AVK renders into and the backend scans out. It
 *                      is the MODE, at every transform. wlr_scene asserts
 *                      exactly this (`buffer->width == resolution_width` in
 *                      wlr_scene_output_build_state) and on real KMS a
 *                      framebuffer of the other shape is a rejected commit
 *                      rather than a rotated picture.
 *
 * DAMAGE               always named at its conversion boundary. Output damage,
 *                      the damage ring and the KMS presentation damage are all
 *                      in ATTACHMENT space; blur source regions are in
 *                      attachment space too but are NOT confined to it (see
 *                      docs/architecture.md, "a damage ring cannot carry an
 *                      out-of-bounds rectangle").
 *
 * For a mode of 800x600 at transform 90:
 *
 *     attachment extent    800 x 600      <- allocate the swapchain from this
 *     presentation extent  600 x 800      <- walk the scene through this
 *
 * DO NOT allocate the Vulkan attachment from the presentation extent. The
 * renderer is built on mode-space attachments; the other model would be a
 * different renderer, not a different variable.
 */
#ifndef AZ_EXTENT_H
#define AZ_EXTENT_H

#include <stdbool.h>
#include <wayland-server-protocol.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/box.h>
#include <wlr/util/transform.h>

/* The physical raster: the attachment, the swapchain, the scanned-out buffer,
 * and every damage region that may reach a wlr_damage_ring. */
struct az_attachment_extent {
	int width;
	int height;
};

/* The same raster in the orientation the user sees, in device pixels. Odd
 * transforms swap it. Nothing is ever ALLOCATED from this. */
struct az_presentation_extent {
	int width;
	int height;
};

static inline struct az_attachment_extent az_attachment_extent_make(int w, int h)
{
	return (struct az_attachment_extent){ .width = w, .height = h };
}

static inline bool az_attachment_extent_valid(struct az_attachment_extent e)
{
	return e.width > 0 && e.height > 0;
}

static inline bool az_attachment_extent_eq(struct az_attachment_extent a,
		struct az_attachment_extent b)
{
	return a.width == b.width && a.height == b.height;
}

/*
 * THE ONE CONVERSION, both ways. wlr_output_transform_coords() swaps the pair
 * for an odd transform and leaves it alone otherwise, which is the same
 * operation in both directions -- the two names exist so that a call site says
 * which way it is going and a reviewer can check it against the consumer.
 */
static inline struct az_presentation_extent az_presentation_of(
		struct az_attachment_extent a, enum wl_output_transform tr)
{
	int w = a.width, h = a.height;
	wlr_output_transform_coords(tr, &w, &h);
	return (struct az_presentation_extent){ .width = w, .height = h };
}

static inline struct az_attachment_extent az_attachment_of(
		struct az_presentation_extent p, enum wl_output_transform tr)
{
	int w = p.width, h = p.height;
	wlr_output_transform_coords(tr, &w, &h);
	return (struct az_attachment_extent){ .width = w, .height = h };
}

/*
 * THE TRANSFORM THIS FRAME IS BEING DRAWN FOR, which on a modeset frame is the
 * state's and not the output's. A frame that mixed them would map its geometry
 * through the transform it is leaving while writing the attachment it is
 * arriving at -- the same class of error as the extents, one field over.
 */
static inline enum wl_output_transform az_output_pending_transform(
		const struct wlr_output *output, const struct wlr_output_state *state)
{
	if (state != NULL && (state->committed & WLR_OUTPUT_STATE_TRANSFORM)) {
		return state->transform;
	}
	return output->transform;
}

/*
 * THE ATTACHMENT EXTENT THIS FRAME IS BEING DRAWN INTO: the pending mode if
 * this frame carries one, the current mode otherwise. Never the transformed
 * resolution -- wlr_output_transformed_resolution() answers a different
 * question, and answering it here is the M4F.2C.4d defect.
 */
static inline struct az_attachment_extent az_output_attachment_extent(
		const struct wlr_output *output, const struct wlr_output_state *state)
{
	struct az_attachment_extent e =
		az_attachment_extent_make(output->width, output->height);
	if (state != NULL && (state->committed & WLR_OUTPUT_STATE_MODE)) {
		if (state->mode_type == WLR_OUTPUT_STATE_MODE_FIXED) {
			e = az_attachment_extent_make(state->mode->width,
				state->mode->height);
		} else {
			e = az_attachment_extent_make(state->custom_mode.width,
				state->custom_mode.height);
		}
	}
	return e;
}

/* The presentation extent of the frame described by `state`, for callers that
 * want the walker's source space directly. */
static inline struct az_presentation_extent az_output_presentation_extent(
		const struct wlr_output *output, const struct wlr_output_state *state)
{
	return az_presentation_of(az_output_attachment_extent(output, state),
		az_output_pending_transform(output, state));
}

/*
 * A box in PRESENTATION space mapped into ATTACHMENT space.
 *
 * The inverse transform, because wl_output_transform names the transform from
 * the buffer to the screen and this goes the other way. Half-open throughout:
 * the box covers [x, x+width), so the far edge of a presentation-space box at
 * the right-hand edge maps to attachment x = 0, never to -1 or to +1.
 */
static inline void az_box_presentation_to_attachment(struct wlr_box *dst,
		const struct wlr_box *src, enum wl_output_transform tr,
		struct az_presentation_extent p)
{
	wlr_box_transform(dst, src, wlr_output_transform_invert(tr),
		p.width, p.height);
}

#endif /* AZ_EXTENT_H */
