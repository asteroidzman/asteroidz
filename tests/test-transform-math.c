/*
 * Output-transform rectangle and region mathematics — the properties AVK
 * depends on, driven directly.
 *
 * M4F.2C.4d. Every rotated-output quantity in the renderer is produced by
 * wlr_box_transform() / wlr_region_transform() with the LOGICAL-ORIENTATION
 * raster as the source space, and the two facts that make that safe were never
 * asserted anywhere:
 *
 *   - the mapping is a BIJECTION on the integer raster, so transforming and
 *     inverse-transforming returns the original rectangle exactly;
 *   - it preserves AREA, so no pixel is created or lost by a rotation.
 *
 * WHAT THIS IS NOT
 *
 * It is not a test of wlroots. It is a test of the CONTRACT this renderer
 * relies on, written down where a change to it becomes a failing test here
 * rather than a rotated desktop nobody can capture. Two of the cases exist
 * because the code broke them in practice:
 *
 *   OUT-OF-PRESENTATION SOURCE RECTANGLES. A blur's source halo legitimately
 *   lives outside the output -- x < 0 and x >= width -- and M4F.2C.4c found
 *   that a damage ring silently discards exactly those. The transform helpers
 *   must NOT inherit that: a source rectangle at x = -126 has to round-trip
 *   like any other, or the halo cannot be expressed at all on a rotated
 *   output.
 *
 *   THE 90/270 EXTENT SWAP. An 800x600 raster becomes 600x800, and a helper
 *   handed the wrong pair produces a box in a space that does not exist. AVK
 *   allocated its attachment from the transformed resolution and then mapped
 *   geometry into the untransformed one; the bottom 200 rows of every
 *   90-degree frame were never written.
 *
 * HALF-OPEN, EVERYWHERE. A box is [x, x+width) x [y, y+height). The far edge
 * is `width - x - w`, never `width - x`, and the round-trip cases below are
 * chosen so an inclusive-edge implementation fails them: a 1x1 box in each
 * corner, and one-pixel strips along each edge.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>

#include <pixman.h>
#include <wlr/util/box.h>
#include <wlr/util/region.h>
#include <wlr/util/transform.h>

static int failures, checks;
#define CHECK(cond, ...) do { \
		checks++; \
		if (cond) { printf("  ok   " __VA_ARGS__); printf("\n"); } \
		else { failures++; printf("  FAIL " __VA_ARGS__); printf("\n"); } \
	} while (0)

static const enum wl_output_transform ROTATIONS[4] = {
	WL_OUTPUT_TRANSFORM_NORMAL,
	WL_OUTPUT_TRANSFORM_90,
	WL_OUTPUT_TRANSFORM_180,
	WL_OUTPUT_TRANSFORM_270,
};
static const char *NAMES[4] = { "0", "90", "180", "270" };

struct named_box {
	const char *what;
	struct wlr_box box;
};

/* W x H is the SOURCE space: the logical-orientation raster. */
#define W 800
#define H 600

static const struct named_box CASES[] = {
	{ "1x1 top-left",        { 0, 0, 1, 1 } },
	{ "1x1 top-right",       { W - 1, 0, 1, 1 } },
	{ "1x1 bottom-left",     { 0, H - 1, 1, 1 } },
	{ "1x1 bottom-right",    { W - 1, H - 1, 1, 1 } },
	{ "the whole raster",    { 0, 0, W, H } },
	{ "left edge strip",     { 0, 0, 1, H } },
	{ "right edge strip",    { W - 1, 0, 1, H } },
	{ "top edge strip",      { 0, 0, W, 1 } },
	{ "bottom edge strip",   { 0, H - 1, W, 1 } },
	{ "odd rectangle",       { 37, 41, 63, 127 } },
	{ "even rectangle",      { 36, 40, 64, 128 } },
	{ "centre rectangle",    { W / 2 - 10, H / 2 - 10, 20, 20 } },
	/*
	 * SOURCE-DOMAIN RECTANGLES. Not presentable, and that is the point: a blur
	 * halo of 126 pixels reaches this far past every edge, and the transform
	 * has to carry it. 126 is the measured halo of the M4F kernel at scale 1.
	 */
	{ "halo, left of 0",     { -126, 100, 126, 200 } },
	{ "halo, past the right",{ W, 100, 126, 200 } },
	{ "halo, above 0",       { 100, -126, 200, 126 } },
	{ "halo, below the far edge", { 100, H, 200, 126 } },
	{ "halo, corner beyond both", { -126, -126, 126, 126 } },
	{ "a box straddling x=0", { -40, 50, 90, 70 } },
};

static bool box_eq(struct wlr_box a, struct wlr_box b) {
	return a.x == b.x && a.y == b.y && a.width == b.width
		&& a.height == b.height;
}

static void test_round_trip(void) {
	printf("transform -> inverse round trip\n");
	for (int t = 0; t < 4; t++) {
		enum wl_output_transform tr = ROTATIONS[t];
		enum wl_output_transform inv = wlr_output_transform_invert(tr);
		/* The destination space of a 90/270 transform is transposed, so the
		 * inverse has to be told THAT pair. Getting this wrong is the exact
		 * shape of the attachment defect this milestone found. */
		int dw = W, dh = H;
		wlr_output_transform_coords(tr, &dw, &dh);

		bool all = true;
		for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
			struct wlr_box mid, back;
			wlr_box_transform(&mid, &CASES[i].box, tr, W, H);
			wlr_box_transform(&back, &mid, inv, dw, dh);
			if (!box_eq(back, CASES[i].box)) {
				printf("       %s: %s %d,%d %dx%d -> %d,%d %dx%d -> "
					"%d,%d %dx%d\n", NAMES[t], CASES[i].what,
					CASES[i].box.x, CASES[i].box.y, CASES[i].box.width,
					CASES[i].box.height, mid.x, mid.y, mid.width, mid.height,
					back.x, back.y, back.width, back.height);
				all = false;
			}
		}
		CHECK(all, "%s: every rectangle survives transform then inverse",
			NAMES[t]);
	}
}

static void test_area(void) {
	printf("area is preserved\n");
	for (int t = 0; t < 4; t++) {
		bool all = true;
		for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
			struct wlr_box out;
			wlr_box_transform(&out, &CASES[i].box, ROTATIONS[t], W, H);
			if (out.width * out.height
					!= CASES[i].box.width * CASES[i].box.height) {
				all = false;
			}
		}
		CHECK(all, "%s: a rotation neither creates nor loses a pixel",
			NAMES[t]);
	}
}

static void test_extent_swap(void) {
	printf("the 90/270 extent swap\n");
	int w, h;
	w = W; h = H;
	wlr_output_transform_coords(WL_OUTPUT_TRANSFORM_NORMAL, &w, &h);
	CHECK(w == W && h == H, "0 leaves %dx%d alone (%dx%d)", W, H, w, h);
	w = W; h = H;
	wlr_output_transform_coords(WL_OUTPUT_TRANSFORM_180, &w, &h);
	CHECK(w == W && h == H, "180 leaves %dx%d alone (%dx%d)", W, H, w, h);
	w = W; h = H;
	wlr_output_transform_coords(WL_OUTPUT_TRANSFORM_90, &w, &h);
	CHECK(w == H && h == W, "90 transposes to %dx%d (%dx%d)", H, W, w, h);
	w = W; h = H;
	wlr_output_transform_coords(WL_OUTPUT_TRANSFORM_270, &w, &h);
	CHECK(w == H && h == W, "270 transposes to %dx%d (%dx%d)", H, W, w, h);

	/* And the consequence the renderer depends on: the whole raster maps onto
	 * the whole transposed raster, exactly. */
	for (int t = 0; t < 4; t++) {
		struct wlr_box whole = { 0, 0, W, H }, out;
		int dw = W, dh = H;
		wlr_output_transform_coords(ROTATIONS[t], &dw, &dh);
		wlr_box_transform(&out, &whole, ROTATIONS[t], W, H);
		CHECK(out.x == 0 && out.y == 0 && out.width == dw && out.height == dh,
			"%s: the whole raster fills the whole destination "
			"(%d,%d %dx%d vs %dx%d)", NAMES[t], out.x, out.y, out.width,
			out.height, dw, dh);
	}
}

static uint64_t region_area(const pixman_region32_t *r) {
	int n = 0;
	const pixman_box32_t *b = pixman_region32_rectangles(
		(pixman_region32_t *)r, &n);
	uint64_t a = 0;
	for (int i = 0; i < n; i++) {
		a += (uint64_t)(b[i].x2 - b[i].x1) * (uint64_t)(b[i].y2 - b[i].y1);
	}
	return a;
}

static void test_region(void) {
	printf("multi-rectangle regions\n");
	/*
	 * THREE RECTANGLES WITH A GAP, and one of them out of presentation bounds.
	 * A region is what damage and clips are made of, and a region transform
	 * that collapsed to a bounding box would still pass every single-rectangle
	 * case above.
	 */
	for (int t = 0; t < 4; t++) {
		pixman_region32_t src, mid, back;
		pixman_region32_init(&src);
		pixman_region32_init(&mid);
		pixman_region32_init(&back);
		pixman_region32_union_rect(&src, &src, 40, 60, 100, 80);
		pixman_region32_union_rect(&src, &src, 300, 200, 50, 400);
		pixman_region32_union_rect(&src, &src, -126, 10, 126, 90);

		int dw = W, dh = H;
		wlr_output_transform_coords(ROTATIONS[t], &dw, &dh);
		wlr_region_transform(&mid, &src, ROTATIONS[t], W, H);
		wlr_region_transform(&back, &mid,
			wlr_output_transform_invert(ROTATIONS[t]), dw, dh);

		CHECK(region_area(&mid) == region_area(&src),
			"%s: total covered area is unchanged (%llu vs %llu)", NAMES[t],
			(unsigned long long)region_area(&mid),
			(unsigned long long)region_area(&src));
		CHECK(pixman_region32_equal(&back, &src),
			"%s: the region round-trips, gap and all", NAMES[t]);

		/* THE OUT-OF-BOUNDS RECTANGLE SURVIVES. A damage ring would have
		 * dropped it; a transform must not. */
		pixman_box32_t e = *pixman_region32_extents(&mid);
		bool outside = e.x1 < 0 || e.y1 < 0 || e.x2 > dw || e.y2 > dh;
		CHECK(outside, "%s: the source halo rectangle is still outside the "
			"presentation bounds after transforming (%d,%d..%d,%d in %dx%d)",
			NAMES[t], e.x1, e.y1, e.x2, e.y2, dw, dh);

		pixman_region32_fini(&src);
		pixman_region32_fini(&mid);
		pixman_region32_fini(&back);
	}
}

static void test_half_open(void) {
	printf("the half-open contract\n");
	/*
	 * ANCHORED, not merely self-consistent. A round trip passes for an
	 * inclusive-edge implementation too, as long as it is inclusive in both
	 * directions -- so one case states where a corner pixel actually lands.
	 *
	 * At 180 degrees the top-left pixel of an 800x600 raster becomes the
	 * bottom-right one: x = W - 0 - 1 = 799, y = H - 0 - 1 = 599. An
	 * implementation using W - x would put it at 800, one past the far edge,
	 * and every rectangle would still round-trip.
	 */
	struct wlr_box tl = { 0, 0, 1, 1 }, out;
	wlr_box_transform(&out, &tl, WL_OUTPUT_TRANSFORM_180, W, H);
	CHECK(out.x == W - 1 && out.y == H - 1,
		"180: the top-left pixel becomes the bottom-right one (%d,%d)",
		out.x, out.y);

	struct wlr_box br = { W - 1, H - 1, 1, 1 };
	wlr_box_transform(&out, &br, WL_OUTPUT_TRANSFORM_180, W, H);
	CHECK(out.x == 0 && out.y == 0,
		"180: and the bottom-right pixel becomes the top-left one (%d,%d)",
		out.x, out.y);

	/* At 90 the top-left goes to the TOP-RIGHT of the transposed raster:
	 * x = H - y - h = 600 - 0 - 1 = 599, y = 0. */
	wlr_box_transform(&out, &tl, WL_OUTPUT_TRANSFORM_90, W, H);
	CHECK(out.x == H - 1 && out.y == 0,
		"90: the top-left pixel becomes the top-right one (%d,%d)",
		out.x, out.y);

	/* And a full-width strip stays full-width in the axis it became. */
	struct wlr_box top = { 0, 0, W, 1 };
	wlr_box_transform(&out, &top, WL_OUTPUT_TRANSFORM_90, W, H);
	CHECK(out.width == 1 && out.height == W,
		"90: a 1-pixel top strip becomes a 1-pixel side strip (%dx%d)",
		out.width, out.height);
}

/*
 * THE TEST CAN FAIL, stated as a test rather than as a belief.
 *
 * Every property above is symmetric enough to hold for a WRONG implementation
 * if the wrongness is applied twice -- a round trip through an inclusive-edge
 * transform and its inclusive-edge inverse returns the original. So one case
 * deliberately uses the transform instead of its inverse for the return leg,
 * and requires the round trip to BREAK. If this passes, the round-trip cases
 * above are not testing anything.
 */
static void test_the_test(void) {
	printf("the round trip is not vacuous\n");
	struct wlr_box src = { 37, 41, 63, 127 }, mid, back;
	int dw = W, dh = H;
	wlr_output_transform_coords(WL_OUTPUT_TRANSFORM_90, &dw, &dh);
	wlr_box_transform(&mid, &src, WL_OUTPUT_TRANSFORM_90, W, H);
	/* 90 again, not 270: the wrong return leg. */
	wlr_box_transform(&back, &mid, WL_OUTPUT_TRANSFORM_90, dw, dh);
	CHECK(!box_eq(back, src),
		"90 applied twice does NOT return the original (%d,%d %dx%d)",
		back.x, back.y, back.width, back.height);
}

int main(void) {
	printf("transform math\n\n");
	test_the_test();
	test_half_open();
	test_round_trip();
	test_area();
	test_extent_swap();
	test_region();
	printf("\n---- %d/%d checks passed\n", checks - failures, checks);
	return failures == 0 ? 0 : 1;
}
