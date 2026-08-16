/*
 * The SHM upload's copy, split from everything around it.
 *
 * The memcpy that gets a wl_shm client's pixels into staging used to be fused
 * into avk_upload_image_write_regions() along with the plan and the Vulkan
 * submission. It had to come apart so the copy could run on a worker thread
 * instead of on the compositor's event loop -- 6.5 MB of it per repaint, with
 * libinput reporting 47-53ms of input lag while it ran.
 *
 * Splitting a working copy is exactly the kind of change that shears an image
 * and is not noticed until a screenshot looks subtly wrong, so the three
 * pieces are checked here directly, with no GPU involved: avk_upload_plan_*()
 * and avk_upload_pack() read nothing from a device, and a struct avk_image on
 * the stack carries everything they consult.
 *
 * What is checked, and what each check would catch:
 *
 *   1. PLACEMENT. Every packed byte equals the source byte at
 *      src[(y + row) * stride + x * bpp]. A copy that treats a rectangle as
 *      one contiguous block, or that computes the source row as
 *      y * width * bpp, produces a diagonally sheared rectangle -- and it does
 *      so ONLY when the rectangle is narrower than the buffer, which is every
 *      interesting case and no naive test case.
 *
 *   2. NOTHING ELSE IS WRITTEN. The destination is filled with a sentinel
 *      first and every byte outside a region's own span must survive. A pack
 *      that runs past a region's end passes check 1 and corrupts the region
 *      after it.
 *
 *   3. ALIGNMENT AND DISJOINTNESS of the offsets, which is what Vulkan
 *      requires of bufferOffset and what keeps two regions from landing on
 *      each other.
 *
 *   4. `bytes` counts what MOVED, not what was reserved. It is the number the
 *      damage path is judged by, and reporting the alignment padding as
 *      copied bytes would flatter it.
 *
 *   5. THE FULL PLAN KEEPS THE PADDING. A whole-image copy is verbatim,
 *      including the source's row padding, because its bufferRowLength is
 *      stride/bpp. Repacking it tightly would be correct only for an
 *      unpadded buffer.
 *
 *   6. REFUSALS: a rectangle outside the image, more regions than the path
 *      packs, a stride that is not a whole number of pixels. Each has to be
 *      a refusal rather than a wrong answer, because a truncated or rounded
 *      copy leaves the image holding a mixture of two generations with
 *      nothing to say so.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "render/vulkan/image/avk_upload.h"

static int failures, checks;
#define CHECK(cond, ...) do { \
		checks++; \
		if (cond) { printf("  ok   " __VA_ARGS__); printf("\n"); } \
		else { failures++; printf("  FAIL " __VA_ARGS__); printf("\n"); } \
	} while (0)

#define SENTINEL 0xCD

/* A recognisable byte for every source position, so a misplaced copy names the
 * position it actually read rather than merely differing. */
static uint8_t src_byte(uint32_t x, uint32_t y, uint32_t c)
{
	return (uint8_t)(x * 7u + y * 31u + c * 101u + 1u);
}

static uint8_t *make_source(uint32_t width, uint32_t height, uint32_t stride,
	uint32_t bpp)
{
	uint8_t *src = malloc((size_t)stride * height);
	/* The padding is filled with something distinctive too: a region copy that
	 * accidentally includes it must be visible, not merely wrong-by-zero. */
	memset(src, 0xAB, (size_t)stride * height);
	for (uint32_t y = 0; y < height; y++) {
		for (uint32_t x = 0; x < width; x++) {
			for (uint32_t c = 0; c < bpp; c++) {
				src[(size_t)y * stride + (size_t)x * bpp + c] =
					src_byte(x, y, c);
			}
		}
	}
	return src;
}

static struct avk_image make_image(uint32_t width, uint32_t height)
{
	struct avk_image img = { 0 };
	img.format = VK_FORMAT_B8G8R8A8_UNORM;
	img.extent = (VkExtent2D){ width, height };
	return img;
}

/* 1, 2, 3, 4: two separated rectangles out of a padded buffer. */
static void test_regions(void)
{
	printf("two rectangles, padded stride\n");
	const uint32_t w = 64, h = 48, bpp = 4;
	const uint32_t stride = w * bpp + 37 * bpp;  /* padded, and by a whole
	                                              * number of pixels so the
	                                              * plan does not refuse it */
	struct avk_image img = make_image(w, h);
	uint8_t *src = make_source(w, h, stride, bpp);

	struct avk_upload_rect rects[2] = {
		{ .x = 3,  .y = 5,  .width = 11, .height = 7 },
		{ .x = 40, .y = 20, .width = 9,  .height = 13 },
	};
	struct avk_upload_plan plan;
	CHECK(avk_upload_plan_regions(&plan, &img, stride, h, rects, 2),
		"the plan was accepted");
	CHECK(plan.rect_count == 2, "with both rectangles (%u)", plan.rect_count);
	CHECK(!plan.full, "and it is not a full-image plan");

	uint64_t want_bytes = (uint64_t)11 * 7 * bpp + (uint64_t)9 * 13 * bpp;
	CHECK(plan.bytes == want_bytes,
		"bytes counts only what moves: %llu, expected %llu",
		(unsigned long long)plan.bytes, (unsigned long long)want_bytes);
	CHECK(plan.total >= want_bytes,
		"and total reserves at least that much (%llu)",
		(unsigned long long)plan.total);

	/* 3: aligned and disjoint. */
	bool aligned = true, disjoint = true;
	for (uint32_t i = 0; i < plan.rect_count; i++) {
		if (plan.offsets[i] % 4 != 0) {
			aligned = false;
		}
		uint64_t end_i = plan.offsets[i]
			+ (uint64_t)plan.rects[i].width * plan.rects[i].height * bpp;
		for (uint32_t j = i + 1; j < plan.rect_count; j++) {
			uint64_t end_j = plan.offsets[j]
				+ (uint64_t)plan.rects[j].width * plan.rects[j].height * bpp;
			if (plan.offsets[i] < end_j && plan.offsets[j] < end_i) {
				disjoint = false;
			}
		}
	}
	CHECK(aligned, "every offset is a multiple of 4 and of the texel size");
	CHECK(disjoint, "and no two regions overlap in staging");

	uint8_t *dst = malloc((size_t)plan.total + 64);
	memset(dst, SENTINEL, (size_t)plan.total + 64);
	avk_upload_pack(&plan, src, dst);

	/* 1: every packed byte came from the right place. */
	uint32_t wrong = 0;
	for (uint32_t i = 0; i < plan.rect_count; i++) {
		const struct avk_upload_rect *r = &plan.rects[i];
		for (uint32_t row = 0; row < r->height; row++) {
			for (uint32_t col = 0; col < r->width; col++) {
				for (uint32_t c = 0; c < bpp; c++) {
					size_t at = (size_t)plan.offsets[i]
						+ ((size_t)row * r->width + col) * bpp + c;
					if (dst[at] != src_byte(r->x + col, r->y + row, c)) {
						wrong++;
					}
				}
			}
		}
	}
	CHECK(wrong == 0, "every packed byte is the source byte at its own x,y "
		"(%u wrong)", wrong);

	/* 2: and nothing outside a region's own span was touched. Written as a
	 * count of sentinel bytes that survived where they should have, because
	 * "no crash" is not evidence of a bounded write. */
	uint32_t clobbered = 0;
	for (size_t at = 0; at < (size_t)plan.total + 64; at++) {
		bool inside = false;
		for (uint32_t i = 0; i < plan.rect_count; i++) {
			size_t begin = (size_t)plan.offsets[i];
			size_t end = begin + (size_t)plan.rects[i].width
				* plan.rects[i].height * bpp;
			if (at >= begin && at < end) {
				inside = true;
			}
		}
		if (!inside && dst[at] != SENTINEL) {
			clobbered++;
		}
	}
	CHECK(clobbered == 0, "and no byte outside a region was written (%u)",
		clobbered);

	free(dst);
	free(src);
}

/* 5: the whole-image plan is verbatim, padding included. */
static void test_full(void)
{
	printf("\nwhole image, padded stride\n");
	const uint32_t w = 33, h = 17, bpp = 4;
	const uint32_t stride = w * bpp + 5 * bpp;
	struct avk_image img = make_image(w, h);
	uint8_t *src = make_source(w, h, stride, bpp);

	struct avk_upload_plan plan;
	CHECK(avk_upload_plan_full(&plan, &img, stride, h), "the plan was accepted");
	CHECK(plan.full, "and it is a full-image plan");
	CHECK(plan.total == (uint64_t)stride * h,
		"sized stride * height (%llu), NOT width * height * bpp (%llu)",
		(unsigned long long)plan.total, (unsigned long long)w * h * bpp);

	uint8_t *dst = malloc((size_t)plan.total);
	memset(dst, SENTINEL, (size_t)plan.total);
	avk_upload_pack(&plan, src, dst);
	CHECK(memcmp(dst, src, (size_t)plan.total) == 0,
		"and the copy is byte-for-byte, padding and all");

	free(dst);
	free(src);
}

/* 6: the refusals. */
static void test_refusals(void)
{
	printf("\nrefusals\n");
	struct avk_image img = make_image(64, 64);
	struct avk_upload_plan plan;

	struct avk_upload_rect outside = { .x = 60, .y = 0, .width = 8,
		.height = 4 };
	CHECK(!avk_upload_plan_regions(&plan, &img, 64 * 4, 64, &outside, 1),
		"a rectangle running off the right edge is refused");

	struct avk_upload_rect below = { .x = 0, .y = 62, .width = 4,
		.height = 8 };
	CHECK(!avk_upload_plan_regions(&plan, &img, 64 * 4, 64, &below, 1),
		"and one running off the bottom");

	static struct avk_upload_rect many[AVK_UPLOAD_MAX_REGIONS + 1];
	for (uint32_t i = 0; i < AVK_UPLOAD_MAX_REGIONS + 1; i++) {
		many[i] = (struct avk_upload_rect){ .x = 0, .y = i, .width = 4,
			.height = 1 };
	}
	CHECK(!avk_upload_plan_regions(&plan, &img, 64 * 4, 64, many,
			AVK_UPLOAD_MAX_REGIONS + 1),
		"more regions than the path packs is refused, not truncated");
	CHECK(avk_upload_plan_regions(&plan, &img, 64 * 4, 64, many,
			AVK_UPLOAD_MAX_REGIONS),
		"exactly as many as it packs is accepted");

	/* A stride that is not a whole number of pixels cannot be expressed as
	 * bufferRowLength, which is in PIXELS. Rounding it shears the image. */
	CHECK(!avk_upload_plan_full(&plan, &img, 64 * 4 + 2, 64),
		"a stride that is not a multiple of the texel size is refused");

	struct avk_upload_rect empty = { .x = 0, .y = 0, .width = 0,
		.height = 4 };
	CHECK(!avk_upload_plan_regions(&plan, &img, 64 * 4, 64, &empty, 1),
		"a zero-area rectangle plans nothing");
}

/*
 * The test that the tests can fail.
 *
 * Every check above compares the pack against an independently computed
 * expectation, so this asserts the OPPOSITE of check 1 on a deliberately
 * wrong reading of the same data: a rectangle read as though its rows were
 * contiguous -- the shortcut the row-by-row loop exists to avoid. If that
 * still matched the packed bytes, the placement check would be comparing a
 * value against itself and could not fail.
 */
static void test_the_test(void)
{
	printf("\nthe shortcut this is protecting against\n");
	const uint32_t w = 64, h = 48, bpp = 4;
	const uint32_t stride = w * bpp;
	struct avk_image img = make_image(w, h);
	uint8_t *src = make_source(w, h, stride, bpp);

	struct avk_upload_rect r = { .x = 3, .y = 5, .width = 11, .height = 7 };
	struct avk_upload_plan plan;
	avk_upload_plan_regions(&plan, &img, stride, h, &r, 1);
	uint8_t *dst = malloc((size_t)plan.total);
	avk_upload_pack(&plan, src, dst);

	const uint8_t *naive = src + (size_t)r.y * stride + (size_t)r.x * bpp;
	CHECK(memcmp(dst, naive, (size_t)r.width * r.height * bpp) != 0,
		"a contiguous read of the same rectangle differs from the packed "
		"copy, so the placement check is capable of failing");

	free(dst);
	free(src);
}

int main(void)
{
	printf("shm upload: plan and pack\n\n");
	test_regions();
	test_full();
	test_refusals();
	test_the_test();
	printf("\n---- %d/%d checks passed\n", checks - failures, checks);
	return failures == 0 ? 0 : 1;
}
