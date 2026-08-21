/*
 * M4E.2: the transient image pool.
 *
 * THE CLAIM THAT MATTERS AND HOW IT IS CHECKED. "Reuse is timeline-safe" is a
 * statement about a resource the GPU may still be reading, and the failure mode
 * is corruption that appears on someone else's machine. So it is NOT tested by
 * looking for corruption. The pool counts every time it hands out an image
 * whose `last_use` the device timeline has not reached -- `unsafe_reuses` -- and
 * these tests assert on that counter. It is structurally zero, the break makes
 * it nonzero, and the assertion is exact in both directions.
 *
 * That is the same remedy as the cursor-lifetime invariant: assert the
 * ownership rule directly rather than waiting for undefined behaviour to become
 * visible.
 *
 * Exits 77 (skip) with no GPU.
 */

#define _POSIX_C_SOURCE 200809L

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "render/vulkan/command/avk_command.h"
#include "render/vulkan/graph/avk_transient.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond, ...) do { \
		checks++; \
		if (cond) { \
			printf("  ok   " __VA_ARGS__); \
			printf("\n"); \
		} else { \
			failures++; \
			printf("  FAIL " __VA_ARGS__); \
			printf("   (%s:%d)\n", __FILE__, __LINE__); \
		} \
	} while (0)

#define SKIP(...) do { \
		printf("SKIP: " __VA_ARGS__); printf("\n"); return 77; \
	} while (0)

#define FMT VK_FORMAT_B8G8R8A8_UNORM
#define USAGE (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)

struct harness {
	struct avk_instance *inst;
	struct avk_device *dev;
	struct avk_retire_queue retire;
	struct avk_cmd_ring ring;
	struct avk_transient_pool pool;
};

static bool harness_up(struct harness *h) {
	memset(h, 0, sizeof(*h));
	h->inst = avk_instance_create("avk-transient-test");
	if (h->inst == NULL) {
		return false;
	}
	h->dev = avk_device_create(h->inst, -1);
	if (h->dev == NULL) {
		return false;
	}
	avk_retire_init(&h->retire, "transient-test");
	if (!avk_cmd_ring_init(&h->ring, h->dev, "transient-test", AVK_FRAMES_IN_FLIGHT)) {
		return false;
	}
	h->ring.retire = &h->retire;
	avk_transient_pool_init(&h->pool, h->dev, &h->retire);
	return true;
}

static void harness_down(struct harness *h) {
	if (h->dev != NULL) {
		avk_device_wait_idle(h->dev);
		avk_transient_pool_finish(&h->pool);
		avk_retire_finish(&h->retire, h->dev);
		avk_cmd_ring_finish(&h->ring);
		avk_device_destroy(h->dev);
	}
	if (h->inst != NULL) {
		avk_instance_destroy(h->inst);
	}
}

/*
 * An empty submission, purely to advance the device timeline.
 *
 * Empty on purpose: this suite is about the pool's bookkeeping, and a
 * submission that did real work would make "was the GPU finished" depend on how
 * fast the GPU is rather than on what the pool decided.
 */
static uint64_t submit(struct harness *h) {
	VkCommandBuffer cb = avk_cmd_ring_begin(&h->ring);
	if (cb == VK_NULL_HANDLE) {
		return 0;
	}
	return avk_cmd_ring_submit(&h->ring, NULL, 0, NULL, 0);
}

/* One frame: acquire, submit, release. */
static uint64_t frame(struct harness *h, uint32_t w, uint32_t h_px, int count,
		struct avk_image **out) {
	for (int i = 0; i < count; i++) {
		struct avk_image *img = avk_transient_acquire(&h->pool, FMT, w, h_px,
			USAGE);
		if (out != NULL) {
			out[i] = img;
		}
	}
	uint64_t v = submit(h);
	avk_transient_release_frame(&h->pool, v);
	return v;
}

/* ── 1. warmup, then nothing ────────────────────────────────────────────── */
static void test_reuse(struct harness *h) {
	printf("\n-- steady state --\n");

	for (int i = 0; i < 4; i++) {
		frame(h, 800, 500, 1, NULL);
	}
	/* Let the GPU catch up so every entry is genuinely free. */
	avk_device_wait_idle(h->dev);
	uint64_t creates = h->pool.stats.creates;
	CHECK(creates >= 1, "warmup created at least one image (%" PRIu64 ")",
		creates);

	/*
	 * PACED, which is what a compositor is. Each frame waits for the previous
	 * one, exactly as a vsync-driven repaint does, so at most one transient is
	 * in flight at a time and steady state means one image.
	 *
	 * The unpaced version of this loop grew the pool to 4 and failed the
	 * assertion below -- correctly. Running the CPU three frames ahead of the
	 * GPU means three transients are genuinely live at once, and the pool
	 * allocating for them is the allocate-rather-than-wait rule doing its job,
	 * not churn. That case is measured in test_pressure(), where it belongs.
	 */
	for (int i = 0; i < 300; i++) {
		uint64_t v = frame(h, 800, 500, 1, NULL);
		avk_device_timeline_wait(h->dev, v, 1000000000ULL);
	}
	avk_device_wait_idle(h->dev);
	/*
	 * THE REQUIREMENT. Not "few allocations" -- none. A counter that stops
	 * moving is the only form of this claim a threshold cannot satisfy by
	 * accident.
	 */
	CHECK(h->pool.stats.creates == creates,
		"and 300 paced frames created ZERO more (%" PRIu64 ")",
		h->pool.stats.creates);
	CHECK(h->pool.stats.reuses >= 300, "they were all reuses (%" PRIu64 ")",
		h->pool.stats.reuses);
	CHECK(h->pool.stats.unsafe_reuses == 0,
		"and none of them was handed out early (%" PRIu64 ")",
		h->pool.stats.unsafe_reuses);
	/*
	 * Printed, not asserted. An unthrottled 300-frame loop legitimately stalls
	 * the command ring -- that is backpressure, the CPU being more than three
	 * frames ahead of the GPU, and it is the one place waiting is correct. What
	 * matters is that the POOL added none of its own, which is what
	 * unsafe_reuses == 0 above says: it allocated instead of waiting.
	 */
	printf("  ---- command-ring stalls during the run: %" PRIu64
		" (backpressure, not the pool)\n", h->ring.stalls);
}

/* ── 2. two at once ─────────────────────────────────────────────────────── */
static void test_two_in_one_frame(struct harness *h) {
	printf("\n-- two live at once --\n");

	avk_device_wait_idle(h->dev);
	struct avk_image *img[2] = {0};
	frame(h, 640, 480, 2, img);
	CHECK(img[0] != NULL && img[1] != NULL, "both acquires succeeded");
	/*
	 * The case a pool keyed on "is this size free" gets wrong. Both are live in
	 * the same frame, so the second must NOT be the first -- and this is
	 * exactly the two-outputs-sharing-a-renderer case from the M4E.0 audit,
	 * where output B acquires while output A's frame is still in flight.
	 */
	CHECK(img[0] != img[1],
		"two acquires in one frame return DIFFERENT images");
	CHECK(h->pool.stats.unsafe_reuses == 0, "and neither was in flight");
}

/* ── 3. the key ─────────────────────────────────────────────────────────── */
static void test_key(struct harness *h) {
	printf("\n-- compatibility key --\n");

	avk_device_wait_idle(h->dev);
	struct avk_image *a = avk_transient_acquire(&h->pool, FMT, 300, 200, USAGE);
	avk_transient_release_frame(&h->pool, submit(h));
	avk_device_wait_idle(h->dev);

	/* Same request: the same image comes back. */
	struct avk_image *again = avk_transient_acquire(&h->pool, FMT, 300, 200,
		USAGE);
	CHECK(again == a, "the same request returns the same image");
	avk_transient_release_frame(&h->pool, submit(h));
	avk_device_wait_idle(h->dev);

	/*
	 * A DIFFERENT USAGE must not be served by it. This is the one the extent
	 * check cannot catch: an image created without SAMPLED_BIT has the right
	 * dimensions and the right format and still cannot be sampled, and reusing
	 * it that way is a validation error rather than a slow path.
	 */
	struct avk_image *other_usage = avk_transient_acquire(&h->pool, FMT,
		300, 200, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
	CHECK(other_usage != a,
		"an image created with different usage is NOT reused, even at the "
		"same size and format");

	struct avk_image *other_fmt = avk_transient_acquire(&h->pool,
		VK_FORMAT_R8G8B8A8_UNORM, 300, 200, USAGE);
	CHECK(other_fmt != a && other_fmt != other_usage,
		"nor is one of a different format");

	/* Rounding: 300 and 320 both land in the 320 bucket at granularity 64, so
	 * the SECOND of those is a reuse rather than a create. */
	avk_transient_release_frame(&h->pool, submit(h));
	avk_device_wait_idle(h->dev);
	uint64_t before = h->pool.stats.creates;
	struct avk_image *rounded = avk_transient_acquire(&h->pool, FMT, 320, 250,
		USAGE);
	CHECK(h->pool.stats.creates == before && rounded == a,
		"a 320x250 request reuses the 300x200 image: both round to 320x256");
	CHECK(rounded != NULL && rounded->extent.width == 320
			&& rounded->extent.height == 256,
		"and the image really is the rounded-up size (%ux%u)",
		rounded ? rounded->extent.width : 0,
		rounded ? rounded->extent.height : 0);
	CHECK(avk_transient_round(300, 64) == 320
			&& avk_transient_round(320, 64) == 320
			&& avk_transient_round(1080, 64) == 1088
			&& avk_transient_round(1000, 1) == 1000,
		"the rounding is what the header says it is");
	avk_transient_release_frame(&h->pool, submit(h));
}

/* ── 4. the resize storm ────────────────────────────────────────────────── */
static void test_resize_storm(struct harness *h) {
	printf("\n-- resize --\n");

	/*
	 * A drag, as a compositor actually sees one: the requested intermediate
	 * changes by a few pixels every frame, up and then back down. This is the
	 * workload the pool exists for, and the one an exact-extent key turns into
	 * an allocation per frame.
	 *
	 * Both granularities are measured HERE, in one run, on two pools. Arguing
	 * about which is better without the numbers is what the brief asks not to
	 * do.
	 */
	struct avk_transient_pool exact, bucketed;
	avk_transient_pool_init(&exact, h->dev, &h->retire);
	exact.granularity = 1;
	avk_transient_pool_init(&bucketed, h->dev, &h->retire);
	bucketed.granularity = 64;

	for (int pass = 0; pass < 2; pass++) {
		for (int i = 0; i < 40; i++) {
			uint32_t w = 800 + (pass == 0 ? i : 40 - i) * 5;
			uint32_t ht = 500 + (pass == 0 ? i : 40 - i) * 3;
			avk_transient_acquire(&exact, FMT, w, ht, USAGE);
			avk_transient_acquire(&bucketed, FMT, w, ht, USAGE);
			uint64_t v = submit(h);
			avk_transient_release_frame(&exact, v);
			avk_transient_release_frame(&bucketed, v);
			avk_device_wait_idle(h->dev);
		}
	}

	printf("  ---- 80 resize frames: exact %" PRIu64 " creates, "
		"granularity-64 %" PRIu64 " creates\n",
		exact.stats.creates, bucketed.stats.creates);
	printf("  ---- peak bytes: exact %" PRIu64 " KiB, granularity-64 "
		"%" PRIu64 " KiB\n",
		(uint64_t)exact.stats.peak_bytes / 1024,
		(uint64_t)bucketed.stats.peak_bytes / 1024);

	/*
	 * The premise: exact extents really do allocate per frame here. Without
	 * this the comparison below would hold just as well if the sweep had never
	 * changed size.
	 */
	CHECK(exact.stats.creates >= 40,
		"exact extents allocate roughly one image per resize frame "
		"(%" PRIu64 " of 80)", exact.stats.creates);
	CHECK(bucketed.stats.creates * 4 < exact.stats.creates,
		"rounding the backing extent to 64 px cuts that by more than 4x "
		"(%" PRIu64 " vs %" PRIu64 ")",
		bucketed.stats.creates, exact.stats.creates);
	/* And it is not paid for in memory: the whole sweep fits in a handful of
	 * buckets, where exact extents hold 80 distinct images. */
	CHECK(bucketed.stats.peak_bytes < exact.stats.peak_bytes,
		"and uses LESS peak memory, not more (%" PRIu64 " vs %" PRIu64 " KiB)",
		(uint64_t)bucketed.stats.peak_bytes / 1024,
		(uint64_t)exact.stats.peak_bytes / 1024);

	avk_device_wait_idle(h->dev);
	avk_transient_pool_finish(&exact);
	avk_transient_pool_finish(&bucketed);
}

/* ── 5. retirement and budget ───────────────────────────────────────────── */
static void test_retire(struct harness *h) {
	printf("\n-- retirement --\n");

	struct avk_transient_pool pool;
	avk_transient_pool_init(&pool, h->dev, &h->retire);

	/* Three sizes, then only one of them is asked for again. */
	for (int i = 0; i < 3; i++) {
		avk_transient_acquire(&pool, FMT, 256u * (uint32_t)(i + 1), 256, USAGE);
	}
	avk_transient_release_frame(&pool, submit(h));
	avk_device_wait_idle(h->dev);
	CHECK(pool.stats.live == 3, "three sizes held (%u)", pool.stats.live);

	for (int i = 0; i < AVK_TRANSIENT_IDLE_FRAMES + 5; i++) {
		avk_transient_acquire(&pool, FMT, 256, 256, USAGE);
		avk_transient_release_frame(&pool, submit(h));
		avk_transient_pool_collect(&pool);
	}
	avk_device_wait_idle(h->dev);
	/*
	 * Asserted on the KEYS, not on a count.
	 *
	 * The loop above submits without waiting, so the CPU runs ahead and the
	 * pool legitimately grows to several 256x256 entries -- that is the
	 * allocate-rather-than-wait rule working. A count-based assertion here read
	 * "4 left, 6 creates" and looked like a retirement failure; it was the
	 * fixture assuming serialised frames. What retirement actually promises is
	 * that the sizes nothing asks for any more go away.
	 */
	uint32_t stale = 0, wanted = 0;
	for (uint32_t i = 0; i < pool.len; i++) {
		if (pool.entries[i].image == NULL) {
			continue;
		}
		if (pool.entries[i].key.width == 256) {
			wanted++;
		} else {
			stale++;
		}
	}
	CHECK(stale == 0,
		"the two sizes nothing asked for again were retired (%u left)", stale);
	CHECK(wanted > 0, "the one still wanted was kept (%u)", wanted);
	CHECK(pool.stats.retires == 2, "exactly two retires (%" PRIu64 ")",
		pool.stats.retires);
	CHECK(pool.stats.unsafe_reuses == 0,
		"and the growth was allocation, not early reuse (%" PRIu64 ")",
		pool.stats.unsafe_reuses);

	printf("-- budget --\n");
	struct avk_transient_pool tight;
	avk_transient_pool_init(&tight, h->dev, &h->retire);
	/* 4 MB: a 512x512 BGRA image is 1 MB, so this holds about four. */
	tight.budget = 4ull * 1024 * 1024;
	for (int i = 0; i < 10; i++) {
		avk_transient_acquire(&tight, FMT, 512, 512u + (uint32_t)i * 64, USAGE);
		avk_transient_release_frame(&tight, submit(h));
		avk_device_wait_idle(h->dev);
		avk_transient_pool_collect(&tight);
	}
	printf("  ---- 10 distinct sizes under a 4 MiB budget: %u live, "
		"%" PRIu64 " KiB held, peak %" PRIu64 " KiB\n",
		tight.stats.live, (uint64_t)tight.stats.bytes / 1024,
		(uint64_t)tight.stats.peak_bytes / 1024);
	CHECK(tight.stats.bytes <= tight.budget,
		"the cache stays inside its budget (%" PRIu64 " <= %" PRIu64 ")",
		(uint64_t)tight.stats.bytes, (uint64_t)tight.budget);
	CHECK(tight.stats.retires > 0, "by retiring, not by refusing to allocate");
	CHECK(tight.stats.unsafe_reuses == 0,
		"and nothing was evicted while in flight");

	avk_device_wait_idle(h->dev);
	avk_transient_pool_finish(&pool);
	avk_transient_pool_finish(&tight);
}

/* ── 6. drop everything, with work in flight ────────────────────────────── */
static void test_drop_in_flight(struct harness *h) {
	printf("\n-- output resize --\n");

	struct avk_transient_pool pool;
	avk_transient_pool_init(&pool, h->dev, &h->retire);
	for (int i = 0; i < 3; i++) {
		avk_transient_acquire(&pool, FMT, 512, 512, USAGE);
	}
	uint64_t v = submit(h);
	avk_transient_release_frame(&pool, v);

	/*
	 * NO WAIT before dropping. This is the output-mode-change case: the old
	 * size is wanted by nothing, and the previous frame is still in flight.
	 * Destroying in place here is the bug; going through the retire queue
	 * against each entry's own last_use is the fix, and the retire queue is
	 * where the check lands.
	 */
	size_t queued_before = h->retire.len;
	avk_transient_pool_drop_all(&pool);
	CHECK(pool.stats.live == 0, "every entry dropped (%u)", pool.stats.live);
	CHECK(h->retire.len > queued_before,
		"and they went onto the retire queue rather than being destroyed while "
		"the GPU may still be reading them (%zu -> %zu)",
		queued_before, h->retire.len);
	CHECK(h->dev->lifecycle_violations == 0,
		"no image was destroyed twice (%" PRIu64 ")",
		h->dev->lifecycle_violations);

	avk_device_wait_idle(h->dev);
	avk_retire_collect(&h->retire, h->dev);
	avk_transient_pool_finish(&pool);
}

/* ── 7. pressure ────────────────────────────────────────────────────────── */
static void test_pressure(struct harness *h) {
	printf("\n-- pressure --\n");

	struct avk_transient_pool pool;
	avk_transient_pool_init(&pool, h->dev, &h->retire);

	/*
	 * Run the CPU ahead of the GPU deliberately: no wait_idle in the loop, four
	 * transients a frame, 500 frames. Every pool slot is reused many times over
	 * and the ring wraps constantly.
	 *
	 * What this is looking for is the case where "free" was decided by
	 * something other than the timeline -- a frame counter, a slot index, an
	 * in_use flag that a wrap resets. Any of those produces an early reuse
	 * here, and the counter catches it whether or not the GPU happened to have
	 * finished.
	 */
	for (int i = 0; i < 500; i++) {
		for (int j = 0; j < 4; j++) {
			avk_transient_acquire(&pool, FMT, 256, 256, USAGE);
		}
		uint64_t v = submit(h);
		avk_transient_release_frame(&pool, v);
		avk_transient_pool_collect(&pool);
	}
	avk_device_wait_idle(h->dev);

	printf("  ---- 500 frames x 4: %" PRIu64 " acquires, %" PRIu64 " reuses, "
		"%" PRIu64 " creates, %u live, peak %" PRIu64 " KiB\n",
		pool.stats.acquires, pool.stats.reuses, pool.stats.creates,
		pool.stats.live, (uint64_t)pool.stats.peak_bytes / 1024);
	CHECK(pool.stats.acquires == 2000, "2000 acquires (%" PRIu64 ")",
		pool.stats.acquires);
	CHECK(pool.stats.unsafe_reuses == 0,
		"NOT ONE was handed out before the GPU had finished with it "
		"(%" PRIu64 ")", pool.stats.unsafe_reuses);
	/*
	 * The pool grows to whatever the CPU's lead requires and then stops. More
	 * than four means the CPU really did get ahead -- which is what makes the
	 * assertion above worth anything -- and a bound rules out a leak that
	 * allocates a fresh image every frame.
	 */
	CHECK(pool.stats.creates >= 4 && pool.stats.creates < 64,
		"the pool grew to the CPU's lead and stopped (%" PRIu64 " creates for "
		"2000 acquires)", pool.stats.creates);

	avk_transient_pool_finish(&pool);
}

/* ── 8. the break ───────────────────────────────────────────────────────── */
static void test_break(struct harness *h) {
	printf("\n-- BREAK=transient-early-reuse --\n");

	bool on = getenv("AZ_TRANSIENT_EARLY_REUSE") != NULL;
	if (!on) {
		printf("  ---- not set; the assertion below is what it must break\n");
	}

	struct avk_transient_pool pool;
	avk_transient_pool_init(&pool, h->dev, &h->retire);

	/*
	 * Deterministic by construction, not by racing. The first acquire is
	 * released against a timeline point that is NEVER SIGNALLED -- one past the
	 * device's next -- so "has the GPU finished with it" has a definite answer
	 * and the answer is no, regardless of hardware or load. A correct pool
	 * allocates a second image; the break hands the first one back.
	 */
	struct avk_image *a = avk_transient_acquire(&pool, FMT, 128, 128, USAGE);
	avk_transient_release_frame(&pool, avk_device_timeline_value(h->dev)
		+ 1000000);

	struct avk_image *b = avk_transient_acquire(&pool, FMT, 128, 128, USAGE);
	CHECK(a != NULL && b != NULL, "both acquires returned an image");
	CHECK(b != a,
		"an image the GPU has not finished with is NOT handed out again");
	CHECK(pool.stats.unsafe_reuses == 0,
		"and the pool counted no unsafe reuse (%" PRIu64 ")",
		pool.stats.unsafe_reuses);

	avk_device_wait_idle(h->dev);
	/* Both entries carry an unreachable last_use, so the retire queue would
	 * hold them forever. Finish destroys in place, which is correct here
	 * because the device is idle. */
	avk_transient_pool_finish(&pool);
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	printf("== avk transient pool ==\n");

	struct harness h;
	if (!harness_up(&h)) {
		harness_down(&h);
		SKIP("no Vulkan device");
	}

	test_reuse(&h);
	test_two_in_one_frame(&h);
	test_key(&h);
	test_resize_storm(&h);
	test_retire(&h);
	test_drop_in_flight(&h);
	test_pressure(&h);
	test_break(&h);

	harness_down(&h);
	printf("\n---- %d/%d checks passed\n", checks - failures, checks);
	return failures == 0 ? 0 : 1;
}
