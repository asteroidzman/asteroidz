#ifndef AZ_CORNER_PERMUTE_H
#define AZ_CORNER_PERMUTE_H

#include <wlr/types/wlr_output.h>
#include <wlr/util/box.h>

/*
 * Move per-corner radii from LOGICAL corners to PHYSICAL ones under an output
 * transform.
 *
 * Both arrays are CLOCKWISE from the top left -- tl, tr, br, bl -- matching
 * struct fx_corner_radii and struct avk_cmd.corners.
 *
 * The permutation is DERIVED, not reasoned about. A 1x1 probe box is placed at
 * each corner of a 2x2 square and pushed through wlr_box_transform(); where it
 * lands says which physical corner that logical corner became. Working it out
 * from the Wayland rotation convention instead is a coin flip that renders
 * identically on a normal output either way, so nothing would catch a wrong
 * guess -- and a rotated output is exactly the configuration a headless suite
 * has no convenient way to build.
 *
 * Split out of az_avk.h so a test can drive the SAME code production does,
 * rather than a restatement of it that could agree with a bug.
 */
static inline void az_corner_permute(const float logical[static 4],
		enum wl_output_transform t, float out[static 4]) {
	static const int px[4] = { 0, 1, 1, 0 };
	static const int py[4] = { 0, 0, 1, 1 };
	for (int i = 0; i < 4; i++) {
		struct wlr_box probe = { px[i], py[i], 1, 1 };
		wlr_box_transform(&probe, &probe, t, 2, 2);
		int j = (probe.x == 0 && probe.y == 0) ? 0
			: (probe.x == 1 && probe.y == 0) ? 1
			: (probe.x == 1 && probe.y == 1) ? 2 : 3;
		out[j] = logical[i];
	}
}

#endif /* AZ_CORNER_PERMUTE_H */
