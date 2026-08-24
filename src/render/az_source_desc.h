#ifndef AZ_SOURCE_DESC_H
#define AZ_SOURCE_DESC_H

/*
 * ── WLROOTS' COLOUR FIELDS -> ASTEROIDZ'S SOURCE DESCRIPTION. ONE COPY. ────
 *
 * M11. Two places in this compositor need to answer "what colour is this
 * source": the renderer, from a wlr_scene_buffer it is about to draw, and the
 * inspector, from a wlr_surface someone asked about. Both answers must be the
 * same answer, and the way to guarantee that is not to write the switch twice
 * carefully -- it is to write it once.
 *
 * This existed as a switch inside az_avk_lum_of(). The inspector could not
 * reach it (static, and keyed on a scene buffer rather than a surface), and
 * copying it would have created exactly the failure this compositor already
 * fixed once at the protocol layer: two readers of the same state with two
 * opinions, drifting a milestone apart. See az_preferred.h for that story.
 *
 * NO POLICY HERE. This translates representations; it does not decide anything.
 * ADR-004's "untagged is piecewise-sRGB BT.709" is az_lum_resolve()'s decision
 * and stays there -- what this returns for an untagged source is
 * `tagged = false`, which is a fact about the source, not a default for it.
 */

#include <stdint.h>
#include <wlr/render/color.h>
#include <wlr/types/wlr_color_management_v1.h>
#include <wlr/types/wlr_compositor.h>

#include "az_lum.h"

/*
 * GAMMA22 IS WHAT UNTAGGED LOOKS LIKE FROM HERE, and that is why it maps to
 * `tagged = false` rather than to AZ_TF_GAMMA22.
 *
 * scenefx's surface adapter, copying wlroots 0.20.2, initialises `tf` to
 * GAMMA22 and overwrites it only when the surface carries an image description
 * (src/scene/surface.c:329). So a surface that has said NOTHING about its
 * colour arrives indistinguishable from one that explicitly declared a 2.2
 * power curve -- and on a real desktop essentially every surface is the former.
 *
 * Treating it as untagged is what makes Path A exist: Path A's encode is the
 * hardware's _SRGB attachment conversion and cannot be selected, so a source
 * decoded with 2.2 and encoded with sRGB cannot round trip. Measured: a flat
 * grey wallpaper at 128 came back as 129 on every pixel, which is exactly
 * srgb_ieotf(gamma22_eotf(128/255)) = 128.95. The cost of being wrong the other
 * way -- a client that genuinely meant 2.2 -- is bounded by the difference
 * between the two curves, about one code in the midtones. See F12.
 */
static inline struct az_lum_source_desc az_source_desc_from_wlr(
		enum wlr_color_transfer_function tf,
		enum wlr_color_named_primaries primaries,
		uint32_t max_cll) {
	struct az_lum_source_desc src = { .tagged = false };
	switch (tf) {
	case WLR_COLOR_TRANSFER_FUNCTION_SRGB:
		src.tagged = true; src.tf = AZ_TF_SRGB; break;
	case WLR_COLOR_TRANSFER_FUNCTION_GAMMA22:
		break; /* see above: this is what untagged looks like */
	case WLR_COLOR_TRANSFER_FUNCTION_BT1886:
		src.tagged = true; src.tf = AZ_TF_BT1886; break;
	case WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ:
		src.tagged = true; src.tf = AZ_TF_PQ; break;
	case WLR_COLOR_TRANSFER_FUNCTION_EXT_LINEAR:
		src.tagged = true; src.tf = AZ_TF_LINEAR_EXT; break;
	default:
		/* Includes 0, which is what an untagged surface carries. */
		break;
	}
	if (src.tagged) {
		src.primaries = primaries == WLR_COLOR_NAMED_PRIMARIES_BT2020
			? AZ_PRIM_BT2020 : AZ_PRIM_BT709;
		/* max_cll is cd/m2 and 0 means absent, which is the same convention
		 * az_lum_source_desc uses -- no translation. */
		src.max_cll = (float)max_cll;
	}
	/* A description the resolver cannot read is not half-honoured: it becomes
	 * untagged, which ADR-004 already has an answer for. */
	if (!az_lum_source_valid(&src)) {
		src = (struct az_lum_source_desc){ .tagged = false };
	}
	return src;
}

/*
 * ── AND WHICH DESCRIPTION APPLIES. ALSO ONE COPY. ─────────────────────────
 *
 * Two lookups, in this order: wp-color-management's own description first,
 * then the registered fallback -- az_cm_surface_description(), the multiplexer
 * that puts native wp-cm ahead of frog. src/scene/surface.c makes the same two
 * through the function pointer it is handed at startup.
 *
 * The sequence was written out at both renderer-side call sites, the intent
 * inspector and the scanout gate. That is two places to edit the day the
 * precedence changes and two chances to edit only one -- the same failure this
 * file's opening comment exists to prevent, one level up.
 *
 * Reading from the SURFACE rather than from a scene buffer is also what lets an
 * unmapped or never-drawn surface answer at all.
 */
static const struct wlr_image_description_v1_data *az_cm_surface_description(
	struct wlr_surface *surface);

static inline struct az_lum_source_desc az_source_desc_of_surface(
		struct wlr_surface *surface) {
	const struct az_lum_source_desc untagged = { .tagged = false };
	if (surface == NULL) {
		return untagged;
	}
	const struct wlr_image_description_v1_data *img =
		wlr_surface_get_image_description_v1_data(surface);
	if (img == NULL) {
		img = az_cm_surface_description(surface);
	}
	if (img == NULL) {
		return untagged;
	}
	return az_source_desc_from_wlr(
		wlr_color_manager_v1_transfer_function_to_wlr(img->tf_named),
		wlr_color_manager_v1_primaries_to_wlr(img->primaries_named),
		img->max_cll);
}

#endif /* AZ_SOURCE_DESC_H */
