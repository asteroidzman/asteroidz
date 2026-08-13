/*
 * The per-level required-work derivation — the denominator of the per-level
 * scissor decision.
 *
 * M4F.2D. The question that decision turns on is
 *
 *     actual blur-pass work / proven required blur-pass work
 *
 * and the previous attempt at it divided processed pixels by the composite's
 * read region, which is not the required work at all: it ignores that every
 * level must produce enough for the next pass's filter footprint to sample,
 * and it compares a sum over passes against an area in one space. Those two
 * errors pull in opposite directions and the result (92x, 148x) was
 * uninterpretable.
 *
 * avk_blur_work_of() derives both sides per pass. This file checks the
 * properties that make it a denominator rather than an opinion:
 *
 *   1. required <= actual, always. A derivation that ever asked for more than
 *      the chain processes would be describing a different chain.
 *   2. actual matches what the passes declare. Cross-checked against the same
 *      level_extent() arithmetic avk_blur_declare() sums into
 *      processed_pixels, so the two cannot drift.
 *   3. A full-extent demand needs (nearly) everything. If the composite reads
 *      the whole capture, there is almost nothing to save, and a derivation
 *      claiming otherwise is wrong.
 *   4. A tiny demand needs much less. The interesting case, and the one the
 *      decision is about.
 *   5. Monotonicity in radius and levels: a wider kernel or a deeper chain
 *      requires more, never less, for the same demand.
 *   6. ODD EXTENTS. 63/64/65 and 127/128/129, because level_extent() floors
 *      and texel_span() then measures the real ratio; a 2^i shortcut
 *      under-covers on every odd extent and would silently under-require.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>

#include "render/vulkan/effect/avk_blur.h"

static int failures, checks;
#define CHECK(cond, ...) do { \
		checks++; \
		if (cond) { printf("  ok   " __VA_ARGS__); printf("\n"); } \
		else { failures++; printf("  FAIL " __VA_ARGS__); printf("\n"); } \
	} while (0)

/* level_extent(), restated here on purpose: if the implementation's changes,
 * this test should FAIL rather than follow it. */
static uint32_t ext(uint32_t base, uint32_t level)
{
	uint32_t v = base >> level;
	return v > 0 ? v : 1;
}

/* What avk_blur_declare() will sum into processed_pixels for this chain: one
 * down pass per level writing level i, one up pass per level writing level
 * i-1. */
static uint64_t declared_actual(uint32_t w, uint32_t h, uint32_t levels)
{
	uint64_t total = 0;
	for (uint32_t i = 1; i <= levels; i++) {
		total += (uint64_t)ext(w, i) * ext(h, i);
	}
	for (uint32_t i = levels; i >= 1; i--) {
		total += (uint64_t)ext(w, i - 1) * ext(h, i - 1);
	}
	return total;
}

static struct avk_blur_params kernel(uint32_t levels, float radius)
{
	struct avk_blur_params p = {0};
	p.levels = levels;
	p.radius = radius;
	return p;
}

static void test_bounds(void)
{
	printf("required never exceeds actual, and actual matches the passes\n");
	static const uint32_t SIZES[][2] = {
		{ 800, 600 }, { 63, 63 }, { 64, 64 }, { 65, 65 },
		{ 127, 129 }, { 128, 128 }, { 129, 127 }, { 1000, 61 },
	};
	static const float RADII[] = { 1.0f, 5.0f, 12.0f };
	bool le = true, matches = true;
	for (size_t s = 0; s < sizeof(SIZES) / sizeof(SIZES[0]); s++) {
		for (uint32_t lv = 1; lv <= 4; lv++) {
			for (size_t r = 0; r < 3; r++) {
				struct avk_blur_params p = kernel(lv, RADII[r]);
				struct avk_box whole = { 0, 0, (int32_t)SIZES[s][0],
					(int32_t)SIZES[s][1] };
				struct avk_blur_work w;
				if (!avk_blur_work_of(&p, SIZES[s][0], SIZES[s][1], &whole,
						&w)) {
					continue;   /* no levels run at this size: no work at all */
				}
				if (w.required_px > w.actual_px) {
					le = false;
					printf("    %ux%u lv%u r%.0f: required %llu > actual %llu\n",
						SIZES[s][0], SIZES[s][1], lv, (double)RADII[r],
						(unsigned long long)w.required_px,
						(unsigned long long)w.actual_px);
				}
				if (w.actual_px != declared_actual(SIZES[s][0], SIZES[s][1],
						w.levels)) {
					matches = false;
					printf("    %ux%u lv%u: actual %llu, passes declare %llu\n",
						SIZES[s][0], SIZES[s][1], lv,
						(unsigned long long)w.actual_px,
						(unsigned long long)declared_actual(SIZES[s][0],
							SIZES[s][1], w.levels));
				}
			}
		}
	}
	CHECK(le, "required <= actual over 8 extents x 4 level counts x 3 radii");
	CHECK(matches, "actual equals the sum the passes themselves declare");
}

static void test_full_demand(void)
{
	printf("\na whole-capture demand requires nearly the whole chain\n");
	struct avk_blur_params p = kernel(3, 5.0f);
	struct avk_box whole = { 0, 0, 800, 600 };
	struct avk_blur_work w;
	CHECK(avk_blur_work_of(&p, 800, 600, &whole, &w), "derived");
	double frac = (double)w.required_px / (double)w.actual_px;
	printf("    required/actual = %.4f (%llu / %llu)\n", frac,
		(unsigned long long)w.required_px, (unsigned long long)w.actual_px);
	/* Every level is clamped to its own extent, so a full demand dilated by
	 * the footprint saturates: there is nothing to scissor away. A derivation
	 * that found savings here would be wrong. */
	CHECK(frac > 0.99, "a full demand needs essentially all of it (%.4f)", frac);
}

static void test_small_demand(void)
{
	printf("\na small demand requires much less -- the case that matters\n");
	struct avk_blur_params p = kernel(3, 5.0f);
	struct avk_blur_work w;
	/* 16x16, the size of a terminal cursor, in the middle of a 800x600
	 * capture. */
	struct avk_box small = { 392, 292, 16, 16 };
	CHECK(avk_blur_work_of(&p, 800, 600, &small, &w), "derived");
	double amp = (double)w.actual_px / (double)w.required_px;
	printf("    actual/required = %.2fx (%llu / %llu)\n", amp,
		(unsigned long long)w.actual_px, (unsigned long long)w.required_px);
	for (uint32_t i = 1; i <= w.levels; i++) {
		printf("    down L%u  extent %ux%u  required %ux%u at %u,%u\n", i,
			w.down[i].extent_w, w.down[i].extent_h, w.down[i].req_w,
			w.down[i].req_h, w.down[i].req_x, w.down[i].req_y);
	}
	for (uint32_t i = w.levels; i >= 1; i--) {
		printf("    up   L%u  extent %ux%u  renders %ux%u at %u,%u  "
			"reads %ux%u of L%u\n", i - 1,
			w.up[i - 1].extent_w, w.up[i - 1].extent_h, w.up[i - 1].req_w,
			w.up[i - 1].req_h, w.up[i - 1].req_x, w.up[i - 1].req_y,
			w.up[i - 1].in_w, w.up[i - 1].in_h, w.up[i - 1].in_level);
	}
	CHECK(amp > 1.5, "a 16x16 demand is much cheaper than the whole chain "
		"(%.2fx)", amp);
	/*
	 * AND THE FOOTPRINT IS REALLY IN THERE -- on the INPUT side.
	 *
	 * The final upsample RENDERS exactly the demand: it writes 16x16 and no
	 * more, which is what a scissor would set. What is wider is what it READS:
	 * the demand mapped into level 1 and dilated by (radius + 1) texels. The
	 * first version of this check asserted on the rendered region and failed,
	 * which was the test confusing the two quantities the derivation now keeps
	 * apart.
	 *
	 * The demand mapped to level 1 is 8x8; anything at or below that means the
	 * filter footprint was forgotten -- the exact error that made the old
	 * denominator useless.
	 */
	CHECK(w.up[0].req_w == 16 && w.up[0].req_h == 16,
		"the final upsample RENDERS exactly the demand (%ux%u)",
		w.up[0].req_w, w.up[0].req_h);
	CHECK(w.up[0].in_w > 8 && w.up[0].in_h > 8,
		"and READS more than the demand mapped down: %ux%u of level %u for an "
		"8x8 mapping", w.up[0].in_w, w.up[0].in_h, w.up[0].in_level);
}

static void test_monotonic(void)
{
	printf("\nmore radius and more levels require more, never less\n");
	struct avk_box small = { 392, 292, 16, 16 };
	struct avk_blur_work a, b, c;
	struct avk_blur_params p1 = kernel(3, 1.0f);
	struct avk_blur_params p5 = kernel(3, 5.0f);
	struct avk_blur_params p12 = kernel(3, 12.0f);
	avk_blur_work_of(&p1, 800, 600, &small, &a);
	avk_blur_work_of(&p5, 800, 600, &small, &b);
	avk_blur_work_of(&p12, 800, 600, &small, &c);
	printf("    r=1 %llu  r=5 %llu  r=12 %llu required\n",
		(unsigned long long)a.required_px, (unsigned long long)b.required_px,
		(unsigned long long)c.required_px);
	CHECK(a.required_px <= b.required_px && b.required_px <= c.required_px,
		"required grows with radius");

	struct avk_blur_work l2, l4;
	struct avk_blur_params q2 = kernel(2, 5.0f);
	struct avk_blur_params q4 = kernel(4, 5.0f);
	avk_blur_work_of(&q2, 800, 600, &small, &l2);
	avk_blur_work_of(&q4, 800, 600, &small, &l4);
	printf("    levels 2 %llu  levels 4 %llu required\n",
		(unsigned long long)l2.required_px, (unsigned long long)l4.required_px);
	CHECK(l2.required_px <= l4.required_px, "required grows with level count");
}

static void test_odd_extents(void)
{
	printf("\nodd extents: no 2^i shortcut anywhere\n");
	/* 129 pixels is 64 texels at level 1, so a texel spans 2.016 pixels and a
	 * derivation using 2 under-covers. The check is that the required region
	 * at level 1 is at least as wide as the demand mapped by the REAL span. */
	struct avk_blur_params p = kernel(2, 5.0f);
	struct avk_box demand = { 0, 0, 129, 129 };
	struct avk_blur_work w;
	CHECK(avk_blur_work_of(&p, 129, 129, &demand, &w), "129x129 derived");
	CHECK(w.down[1].extent_w == 64 && w.down[1].extent_h == 64,
		"level 1 of 129 is 64 texels (got %ux%u)", w.down[1].extent_w,
		w.down[1].extent_h);
	CHECK(w.down[1].req_w == 64 && w.down[1].req_h == 64,
		"and a full demand requires all 64 of them (got %ux%u)",
		w.down[1].req_w, w.down[1].req_h);

	bool ok = true;
	for (uint32_t base = 63; base <= 65; base++) {
		struct avk_blur_work o;
		struct avk_box d = { 0, 0, (int32_t)base, (int32_t)base };
		if (!avk_blur_work_of(&p, base, base, &d, &o)) {
			continue;
		}
		if (o.required_px > o.actual_px || o.required_px == 0) {
			ok = false;
		}
	}
	CHECK(ok, "63/64/65 all derive a nonzero required <= actual");
}

/*
 * THE TEST'S OWN PREMISE. If the derivation returned the level extents
 * verbatim -- required == actual for every demand -- every check above except
 * the small-demand one would still pass, and the ratio would be a constant 1
 * that says nothing. This is the case that fails if that ever happens.
 */
static void test_the_test(void)
{
	printf("\nthe derivation is not just returning the extents\n");
	struct avk_blur_params p = kernel(3, 5.0f);
	struct avk_box small = { 392, 292, 16, 16 };
	struct avk_blur_work w;
	avk_blur_work_of(&p, 800, 600, &small, &w);
	bool any_smaller = false;
	for (uint32_t i = 1; i <= w.levels; i++) {
		if (w.down[i].required_px < w.down[i].actual_px) {
			any_smaller = true;
		}
	}
	for (uint32_t i = w.levels; i >= 1; i--) {
		if (w.up[i - 1].required_px < w.up[i - 1].actual_px) {
			any_smaller = true;
		}
	}
	CHECK(any_smaller, "at least one pass requires less than its full extent");
}

int main(void)
{
	printf("blur work: actual vs required\n\n");
	test_bounds();
	test_full_demand();
	test_small_demand();
	test_monotonic();
	test_odd_extents();
	test_the_test();
	printf("\n---- %d/%d checks passed\n", checks - failures, checks);
	return failures == 0 ? 0 : 1;
}
