/*
 * The renderer's two raster extents, and the assignment that used to be legal.
 *
 * M4F.2C.4e. M4F.2C.4d's defect was not a transform bug. It was a NAMING bug
 * with a rendering consequence: one pair of `int width, height` held the
 * attachment extent on one line and the presentation extent on another, and no
 * compiler, no reviewer and no test could see the difference.
 *
 *     wlr_output_transformed_resolution(output, &width, &height);  // 600x800
 *     if (state->committed & WLR_OUTPUT_STATE_MODE)
 *             width = state->mode->width;                          // 800x600
 *
 * The swapchain was allocated from one and the scene walker's transform source
 * space was the other. On an 800x600 output at 90 degrees, 455417 of 480000
 * pixels came out wrong and the bottom 200 rows were never written.
 *
 * WHAT THIS FILE ASSERTS
 *
 *   1. The exact old failure: mode 800x600 at transform 90 has an ATTACHMENT
 *      extent of 800x600 and a PRESENTATION extent of 600x800, and the inverse
 *      case, mode 600x800 at 270.
 *   2. The relationship holds for all EIGHT wl_output_transforms, not the four
 *      rotations -- the flipped transforms swap on exactly the same parity and
 *      a `transform == 90 || transform == 270` test would get half of them
 *      wrong.
 *   3. Round trip, area and the pending-state accessors.
 *   4. AND THAT THE OLD IMPLEMENTATION FAILS IT. az_legacy_conflated_extent()
 *      below is the code as it was. If the checks in this file pass for that
 *      function too, they are not testing anything, so it is run through the
 *      same predicate and REQUIRED to disagree.
 *
 * There is no GPU here and no compositor. The point is that this is where a
 * reintroduction fails, rather than four hours later in a rotated desktop that
 * nothing can capture.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>

#include <wlr/types/wlr_output.h>
#include <wlr/util/box.h>
#include <wlr/util/transform.h>

#include "render/az_extent.h"

static int failures, checks;
#define CHECK(cond, ...) do { \
		checks++; \
		if (cond) { printf("  ok   " __VA_ARGS__); printf("\n"); } \
		else { failures++; printf("  FAIL " __VA_ARGS__); printf("\n"); } \
	} while (0)

static const enum wl_output_transform ALL[8] = {
	WL_OUTPUT_TRANSFORM_NORMAL,
	WL_OUTPUT_TRANSFORM_90,
	WL_OUTPUT_TRANSFORM_180,
	WL_OUTPUT_TRANSFORM_270,
	WL_OUTPUT_TRANSFORM_FLIPPED,
	WL_OUTPUT_TRANSFORM_FLIPPED_90,
	WL_OUTPUT_TRANSFORM_FLIPPED_180,
	WL_OUTPUT_TRANSFORM_FLIPPED_270,
};
static const char *NAMES[8] = {
	"normal", "90", "180", "270",
	"flipped", "flipped-90", "flipped-180", "flipped-270",
};

/*
 * THE OLD CODE, kept so that the new checks can be shown to reject it.
 *
 * It answers "how big is this output" with the TRANSFORMED resolution and then
 * lets a mode overwrite one meaning with the other -- which is exactly what
 * happened, one `if` at a time, over two milestones.
 */
static void az_legacy_conflated_extent(int mode_w, int mode_h,
		enum wl_output_transform tr, bool has_mode, int *w, int *h) {
	*w = mode_w;
	*h = mode_h;
	wlr_output_transform_coords(tr, w, h);   /* the presentation extent */
	if (has_mode) {                          /* ...and now the attachment one */
		*w = mode_w;
		*h = mode_h;
	}
}

/* ── 1. the exact failure ───────────────────────────────────────────────── */

static void test_the_old_failure(void) {
	printf("the M4F.2C.4d failure, stated as a contract\n");

	struct az_attachment_extent att = az_attachment_extent_make(800, 600);
	struct az_presentation_extent pres =
		az_presentation_of(att, WL_OUTPUT_TRANSFORM_90);
	CHECK(att.width == 800 && att.height == 600,
		"mode 800x600 at 90: attachment is 800x600 (got %dx%d)",
		att.width, att.height);
	CHECK(pres.width == 600 && pres.height == 800,
		"mode 800x600 at 90: presentation is 600x800 (got %dx%d)",
		pres.width, pres.height);

	/* The inverse case, because a swap implemented as "always transpose"
	 * passes the first one and fails this. */
	struct az_attachment_extent att2 = az_attachment_extent_make(600, 800);
	struct az_presentation_extent pres2 =
		az_presentation_of(att2, WL_OUTPUT_TRANSFORM_270);
	CHECK(att2.width == 600 && att2.height == 800,
		"mode 600x800 at 270: attachment is 600x800 (got %dx%d)",
		att2.width, att2.height);
	CHECK(pres2.width == 800 && pres2.height == 600,
		"mode 600x800 at 270: presentation is 800x600 (got %dx%d)",
		pres2.width, pres2.height);

	/* AND THE OLD IMPLEMENTATION DISAGREES. Without a modeset it returns the
	 * presentation extent for the question the swapchain asks; with one it
	 * returns the attachment extent for the question the walker asks. Either
	 * way one of its two callers is handed the other's answer. */
	int lw, lh;
	az_legacy_conflated_extent(800, 600, WL_OUTPUT_TRANSFORM_90, false,
		&lw, &lh);
	CHECK(!(lw == att.width && lh == att.height),
		"the OLD code hands the swapchain %dx%d for an 800x600 mode -- "
		"the wrong space, and this test says so", lw, lh);
	az_legacy_conflated_extent(800, 600, WL_OUTPUT_TRANSFORM_90, true,
		&lw, &lh);
	CHECK(!(lw == pres.width && lh == pres.height),
		"and hands the walker %dx%d on a modeset frame -- also the wrong "
		"space, in the other direction", lw, lh);
}

/* ── 2. all eight transforms ────────────────────────────────────────────── */

static void test_all_eight(void) {
	printf("\nthe swap is on transform PARITY, at all eight\n");
	for (int i = 0; i < 8; i++) {
		struct az_attachment_extent att = az_attachment_extent_make(800, 600);
		struct az_presentation_extent pres = az_presentation_of(att, ALL[i]);
		bool swapped = (ALL[i] & WL_OUTPUT_TRANSFORM_90) != 0;
		int want_w = swapped ? 600 : 800, want_h = swapped ? 800 : 600;
		CHECK(pres.width == want_w && pres.height == want_h,
			"%-12s presentation %dx%d (want %dx%d)", NAMES[i],
			pres.width, pres.height, want_w, want_h);
	}
}

static void test_round_trip(void) {
	printf("\nattachment -> presentation -> attachment\n");
	static const int SIZES[][2] = {
		{ 800, 600 }, { 600, 800 }, { 1920, 1080 },
		/* AWKWARD WIDTHS. A pitch or alignment assumption hidden in the
		 * conversion would show up on an odd or non-multiple-of-64 extent and
		 * on nothing else. These are the same widths the capture-layout test
		 * uses. */
		{ 799, 601 }, { 801, 599 }, { 1023, 767 }, { 1365, 769 },
	};
	for (size_t s = 0; s < sizeof(SIZES) / sizeof(SIZES[0]); s++) {
		bool ok = true;
		for (int i = 0; i < 8; i++) {
			struct az_attachment_extent a =
				az_attachment_extent_make(SIZES[s][0], SIZES[s][1]);
			struct az_presentation_extent p = az_presentation_of(a, ALL[i]);
			struct az_attachment_extent back = az_attachment_of(p, ALL[i]);
			if (!az_attachment_extent_eq(back, a)) {
				ok = false;
			}
			if ((long)p.width * p.height != (long)a.width * a.height) {
				ok = false;
			}
		}
		CHECK(ok, "%dx%d round-trips and preserves area at all eight",
			SIZES[s][0], SIZES[s][1]);
	}
}

/* ── 3. the pending-state accessors ─────────────────────────────────────── */

static void test_pending_state(void) {
	printf("\nthe frame is drawn for the PENDING mode and transform\n");

	struct wlr_output output = {0};
	output.width = 800;
	output.height = 600;
	output.transform = WL_OUTPUT_TRANSFORM_NORMAL;

	struct az_attachment_extent now = az_output_attachment_extent(&output, NULL);
	CHECK(now.width == 800 && now.height == 600,
		"with no pending state the attachment is the current mode (%dx%d)",
		now.width, now.height);

	/* A modeset frame: 1024x768 arriving together with a 90-degree rotation.
	 * The attachment must be the NEW mode and the presentation extent must be
	 * the new mode transposed -- not the old mode, and not the new mode under
	 * the old transform. Both of those were reachable before this API. */
	struct wlr_output_mode mode = { .width = 1024, .height = 768 };
	struct wlr_output_state state = {0};
	state.committed = WLR_OUTPUT_STATE_MODE | WLR_OUTPUT_STATE_TRANSFORM;
	state.mode_type = WLR_OUTPUT_STATE_MODE_FIXED;
	state.mode = &mode;
	state.transform = WL_OUTPUT_TRANSFORM_90;

	struct az_attachment_extent att = az_output_attachment_extent(&output, &state);
	struct az_presentation_extent pres =
		az_output_presentation_extent(&output, &state);
	CHECK(att.width == 1024 && att.height == 768,
		"a modeset frame's attachment is the PENDING mode (%dx%d)",
		att.width, att.height);
	CHECK(pres.width == 768 && pres.height == 1024,
		"and its presentation extent uses the PENDING transform (%dx%d)",
		pres.width, pres.height);
	CHECK(az_output_pending_transform(&output, &state) == WL_OUTPUT_TRANSFORM_90,
		"and the pending transform is the state's, not the output's");

	/* A custom mode, which is the branch a headless output actually takes. */
	struct wlr_output_state custom = {0};
	custom.committed = WLR_OUTPUT_STATE_MODE;
	custom.mode_type = WLR_OUTPUT_STATE_MODE_CUSTOM;
	custom.custom_mode.width = 1366;
	custom.custom_mode.height = 768;
	struct az_attachment_extent c = az_output_attachment_extent(&output, &custom);
	CHECK(c.width == 1366 && c.height == 768,
		"a custom mode is read from custom_mode (%dx%d)", c.width, c.height);
}

/* ── 4. presentation -> attachment box mapping, half-open ───────────────── */

static void test_box_mapping(void) {
	printf("\na presentation-space box lands inside the attachment\n");
	for (int i = 0; i < 8; i++) {
		struct az_attachment_extent att = az_attachment_extent_make(800, 600);
		struct az_presentation_extent pres = az_presentation_of(att, ALL[i]);

		/* The whole presentation raster must map onto the whole attachment,
		 * exactly: no off-by-one at either edge, at any transform. */
		struct wlr_box whole = { 0, 0, pres.width, pres.height }, out;
		az_box_presentation_to_attachment(&out, &whole, ALL[i], pres);
		bool full = out.x == 0 && out.y == 0 &&
			out.width == att.width && out.height == att.height;

		/* And a 1x1 box at the far corner must land at the far corner --
		 * inside, never at width or at width+1. Half-open: the last valid
		 * pixel is width-1. */
		struct wlr_box corner = { pres.width - 1, pres.height - 1, 1, 1 }, co;
		az_box_presentation_to_attachment(&co, &corner, ALL[i], pres);
		bool inside = co.x >= 0 && co.y >= 0 &&
			co.x + co.width <= att.width && co.y + co.height <= att.height &&
			co.width == 1 && co.height == 1;

		CHECK(full && inside,
			"%-12s whole raster -> %d,%d %dx%d, far corner -> %d,%d",
			NAMES[i], out.x, out.y, out.width, out.height, co.x, co.y);
	}
}

int main(void) {
	printf("extent spaces\n\n");
	test_the_old_failure();
	test_all_eight();
	test_round_trip();
	test_pending_state();
	test_box_mapping();
	printf("\n---- %d/%d checks passed\n", checks - failures, checks);
	return failures == 0 ? 0 : 1;
}
