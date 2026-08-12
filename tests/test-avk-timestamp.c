/*
 * M4D.P: GPU timestamps that never make the CPU wait.
 *
 * Two claims, and they need completely different kinds of test.
 *
 * THE ARITHMETIC is hardware-independent and is tested synthetically. It has
 * to be: this GPU reports 64 valid timestamp bits, so masking to the valid
 * range is the identity here and the counter would take about 585 years to
 * wrap. Every masking and wrap case would pass against real queries on this
 * machine whether the code masked or not -- coverage by coincidence, and the
 * remedy is a constructed case rather than more assertions on the real one.
 *
 * THE LIFETIME needs a real device, because what is being asserted is that
 * results come back from work the GPU has genuinely finished, across enough
 * frames to wrap the three-slot ring many times, WITHOUT the test ever waiting
 * for a frame. If this file waited on the timeline between frames the way
 * test-avk-gradient does, it would prove nothing: results are always ready
 * after a wait. So the render loop below deliberately does not wait, and the
 * samples it collects are only the ones the timeline said were complete.
 *
 * Exits 77 (skip) with no GPU, and again if the device cannot write
 * timestamps -- which is a legitimate outcome, not a failure.
 */

#define _POSIX_C_SOURCE 200809L

#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "render/vulkan/scene/avk_render.h"

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

#define W 128
#define H 128
#define TARGET_FORMAT VK_FORMAT_B8G8R8A8_UNORM

/* ── the arithmetic, on constructed values ──────────────────────────────── */

static void test_ticks_arithmetic(void) {
	printf("tick arithmetic (synthetic; this GPU's 64 valid bits cannot "
		"produce these)\n");

	const uint64_t m64 = UINT64_MAX;
	CHECK(avk_ts_ticks_between(m64, 100, 350) == 250,
		"the ordinary case: 350 - 100 = 250");
	CHECK(avk_ts_ticks_between(m64, 42, 42) == 0, "a zero-length span is 0");

	/*
	 * 36 valid bits, which is what a good many drivers report. The upper 28
	 * bits of a raw result are UNDEFINED -- not zero -- so an implementation
	 * that subtracts without masking gets whatever the driver left there.
	 * Here the garbage is made explicit.
	 */
	const uint32_t bits = 36;
	const uint64_t m36 = ((uint64_t)1 << bits) - 1;
	uint64_t begin = 0x0000000FF0000000ULL & m36;
	uint64_t end = begin + 5000;
	CHECK(avk_ts_ticks_between(m36, begin, end) == 5000,
		"36-bit span, both values in range");

	uint64_t dirty_begin = begin | 0xABCD000000000000ULL;
	uint64_t dirty_end = end | 0x1234000000000000ULL;
	CHECK(avk_ts_ticks_between(m36, dirty_begin, dirty_end) == 5000,
		"the SAME span with undefined high bits set differently on each "
		"value -- masking is what makes this 5000 and not nonsense");
	/* And the premise: without masking it really would be nonsense, so the
	 * assertion above is not passing for free. */
	CHECK((dirty_end - dirty_begin) != 5000,
		"premise: unmasked subtraction of those two is NOT 5000");

	/* Wrap. The counter rolls over inside its valid bits, so `end` is
	 * numerically smaller than `begin` and plain subtraction underflows to
	 * something astronomically large. */
	uint64_t near_top = m36 - 200;
	uint64_t wrapped = 300;  /* 501 ticks later, having rolled over */
	CHECK(avk_ts_ticks_between(m36, near_top, wrapped) == 501,
		"a span that wraps the 36-bit counter is 501 ticks");
	CHECK((wrapped - near_top) > (uint64_t)1 << 40,
		"premise: the same span computed without the mask underflows");

	/* The shift that is undefined behaviour if written as (1 << bits) - 1. */
	CHECK(avk_ts_ticks_between(UINT64_MAX, UINT64_MAX - 10, 4) == 15,
		"64 valid bits: the wrap still works, and the mask is not a shift");
}

/* ── a real device, across many ring wraps ──────────────────────────────── */

struct harness {
	struct avk_instance *inst;
	struct avk_device *dev;
	struct avk_renderer renderer;
	struct avk_image *target;
};

static uint32_t find_memory(struct avk_device *dev, uint32_t bits,
		VkMemoryPropertyFlags want) {
	VkPhysicalDeviceMemoryProperties props;
	vkGetPhysicalDeviceMemoryProperties(dev->phys, &props);
	for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
		if ((bits & (1u << i))
				&& (props.memoryTypes[i].propertyFlags & want) == want) {
			return i;
		}
	}
	return UINT32_MAX;
}

/* avk_image_alloc, not calloc: a bare allocation leaves `life` at 0, which
 * avk_image_destroy() refuses to act on, and the objects show up leaked. */
static struct avk_image *make_target(struct avk_device *dev) {
	struct avk_image *image = avk_image_alloc(dev);
	if (image == NULL) {
		return NULL;
	}
	image->format = TARGET_FORMAT;
	image->extent = (VkExtent2D){ W, H };
	image->has_alpha = true;
	image->layout = VK_IMAGE_LAYOUT_UNDEFINED;
	image->plane_count = 1;

	VkImageCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = TARGET_FORMAT,
		.extent = { W, H, 1 },
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
			| VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	if (vkCreateImage(dev->dev, &info, NULL, &image->image) != VK_SUCCESS) {
		free(image);
		return NULL;
	}
	AVK_LIVE_INC(dev, images);
	VkMemoryRequirements reqs;
	vkGetImageMemoryRequirements(dev->dev, image->image, &reqs);
	uint32_t type = find_memory(dev, reqs.memoryTypeBits,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	VkMemoryAllocateInfo alloc = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = reqs.size,
		.memoryTypeIndex = type,
	};
	if (type == UINT32_MAX
			|| vkAllocateMemory(dev->dev, &alloc, NULL, &image->memory[0])
				!= VK_SUCCESS
			|| vkBindImageMemory(dev->dev, image->image, image->memory[0], 0)
				!= VK_SUCCESS) {
		avk_image_destroy(dev, image);
		return NULL;
	}
	AVK_LIVE_INC(dev, device_memory);
	image->memory_count = 1;
	return image;
}

/* Something with real fragment work in it, so a frame is not so short that its
 * duration rounds to nothing. */
static void build_scene(struct avk_scene *scene) {
	avk_scene_init(scene);
	pixman_region32_union_rect(&scene->damage, &scene->damage, 0, 0, W, H);
	scene->has_clear = true;
	scene->clear_color[3] = 1.0f;
	for (int i = 0; i < 24; i++) {
		struct avk_cmd *cmd = avk_scene_add(scene, AVK_CMD_RECT);
		if (cmd == NULL) {
			return;
		}
		cmd->dst = (struct avk_box){ 0, 0, W, H };
		cmd->opacity = 0.5f;
		cmd->color[0] = 0.3f;
		cmd->color[1] = 0.4f;
		cmd->color[2] = 0.5f;
		cmd->color[3] = 0.5f;
	}
}

#define FRAMES 60

static void test_lifetime(struct harness *h) {
	struct avk_timestamps *ts = &h->renderer.timestamps;
	printf("timestamp lifetime over %d frames "
		"(ring is %d slots, so %d wraps)\n",
		FRAMES, AVK_FRAMES_IN_FLIGHT, FRAMES / AVK_FRAMES_IN_FLIGHT);

	CHECK(ts->supported, "premise: this device writes timestamps");
	CHECK(ts->period_ns > 0.0f,
		"premise: timestampPeriod is %.3f ns, not zero",
		(double)ts->period_ns);
	CHECK(ts->valid_mask != 0,
		"premise: the queue family reports valid bits (mask 0x%" PRIx64 ")",
		ts->valid_mask);

	uint64_t last = 0;
	for (int i = 0; i < FRAMES; i++) {
		struct avk_scene scene;
		build_scene(&scene);
		/*
		 * NO WAIT between frames, deliberately -- see the file comment. The
		 * only thing that ever blocks here is the command ring's own
		 * backpressure when all three slots are still in flight, which is
		 * counted separately and asserted on below.
		 */
		uint64_t value = avk_render_frame(&h->renderer, h->target, &scene,
			NULL, 0, NULL, 0);
		if (value != 0) {
			last = value;
		}
		avk_scene_finish(&scene);
		avk_renderer_collect(&h->renderer);
	}

	CHECK(last != 0, "premise: frames were actually submitted");

	/* Drain what is still outstanding. This wait is at the END of the run,
	 * after every measurement that matters has been taken, so it cannot make
	 * the no-waiting claim above true by accident. */
	if (last != 0) {
		avk_device_timeline_wait(h->dev, last, 2000000000ULL);
	}
	avk_renderer_collect(&h->renderer);

	CHECK(ts->samples > 0, "samples came back at all (%" PRIu64 ")",
		ts->samples);
	/*
	 * The ring holds three frames, so at any moment up to three are still in
	 * flight and uncollected. Everything before that must have been read.
	 */
	CHECK(ts->samples >= (uint64_t)FRAMES - AVK_FRAMES_IN_FLIGHT,
		"nearly every frame produced a sample: %" PRIu64 " of %d",
		ts->samples, FRAMES);
	CHECK(ts->dropped == 0,
		"no slot came round again with an uncollected result (dropped %"
		PRIu64 ")", ts->dropped);

	/*
	 * WHAT THIS DOES NOT ASSERT, and why.
	 *
	 * The first version of this test asserted that the ring never stalled. It
	 * failed, at 27 stalls in 60 frames, and the assertion was the thing that
	 * was wrong: this loop submits frames as fast as the CPU can build them,
	 * with nothing throttling it, so it naturally runs the ring dry and waits
	 * on backpressure. That is the ring's designed behaviour and it has
	 * nothing to do with timestamps -- a real compositor is throttled by
	 * vblank and does not do this.
	 *
	 * What matters is that timestamps introduce no wait of their OWN, so the
	 * claim is that every CPU wait on the frame path is still ring
	 * backpressure and there is no second category. The throttled loop below
	 * is the one that asserts on zero.
	 */
	CHECK(h->renderer.stats.cpu_sync_waits == h->renderer.ring.stalls,
		"every CPU wait is still ring backpressure, none is a timestamp "
		"readback (%" PRIu64 " == %" PRIu64 ")",
		h->renderer.stats.cpu_sync_waits, h->renderer.ring.stalls);

	uint64_t avg = ts->samples ? ts->gpu_frame_ns_total / ts->samples : 0;
	printf("    gpu_frame avg %" PRIu64 " ns (%.1f us), last %" PRIu64 " ns\n",
		avg, (double)avg / 1000.0, ts->gpu_frame_ns);
	/*
	 * Bounds rather than a value: the number is hardware. But it must be a
	 * PLAUSIBLE duration, and the two ways this code fails loudly are a zero
	 * (marks never written, or the period misapplied) and an absurdity
	 * (unmasked subtraction, or ticks reported as nanoseconds).
	 */
	CHECK(avg > 0, "the mean GPU frame time is not zero");
	CHECK(avg < 1000000000ULL,
		"the mean GPU frame time is under a second -- i.e. it is not raw "
		"ticks or an underflowed subtraction");
}

/*
 * The same thing, paced. A compositor is throttled by vblank and is not
 * normally three frames ahead of its GPU, so this is the case where the ring
 * must never stall -- and where every single frame must yield a sample.
 */
static void test_throttled(struct harness *h) {
	struct avk_timestamps *ts = &h->renderer.timestamps;
	const int frames = 12;
	printf("throttled loop, %d frames (what a vblank-paced compositor does)\n",
		frames);

	uint64_t samples_before = ts->samples;
	uint64_t dropped_before = ts->dropped;
	uint64_t stalls_before = h->renderer.ring.stalls;

	for (int i = 0; i < frames; i++) {
		struct avk_scene scene;
		build_scene(&scene);
		uint64_t value = avk_render_frame(&h->renderer, h->target, &scene,
			NULL, 0, NULL, 0);
		avk_scene_finish(&scene);
		if (value != 0) {
			/* Stands in for the vblank the compositor would be waiting on. */
			avk_device_timeline_wait(h->dev, value, 2000000000ULL);
		}
		avk_renderer_collect(&h->renderer);
	}

	CHECK(h->renderer.ring.stalls == stalls_before,
		"the ring never stalled when frames were paced (%" PRIu64 ")",
		h->renderer.ring.stalls - stalls_before);
	CHECK(ts->samples - samples_before == (uint64_t)frames,
		"every paced frame produced a sample (%" PRIu64 " of %d)",
		ts->samples - samples_before, frames);
	CHECK(ts->dropped == dropped_before, "and none was dropped");
}

/* ── main ───────────────────────────────────────────────────────────────── */

int main(void) {
	test_ticks_arithmetic();

	int drm_fd = -1;
	for (int i = 128; i < 132 && drm_fd < 0; i++) {
		char path[64];
		snprintf(path, sizeof(path), "/dev/dri/renderD%d", i);
		drm_fd = open(path, O_RDWR | O_CLOEXEC);
	}
	if (drm_fd < 0) {
		printf("----\n%d/%d checks passed (device part skipped: "
			"no DRM render node)\n", checks - failures, checks);
		return failures == 0 ? 77 : 1;
	}

	struct harness h = {0};
	h.inst = avk_instance_create("test-avk-timestamp");
	if (h.inst == NULL) {
		close(drm_fd);
		SKIP("no Vulkan instance");
	}
	h.dev = avk_device_create(h.inst, drm_fd);
	if (h.dev == NULL) {
		avk_instance_destroy(h.inst);
		close(drm_fd);
		SKIP("no Vulkan device");
	}

	if (!avk_renderer_init(&h.renderer, h.dev, TARGET_FORMAT)) {
		CHECK(false, "renderer initialises");
		goto done;
	}
	if (!h.renderer.timestamps.supported) {
		/* A clean, documented outcome and not a failure: the rest of the
		 * renderer is expected to work exactly as before. */
		printf("GPU TIMESTAMPS: UNSUPPORTED on this device -- the lifetime "
			"checks need real queries and are skipped\n");
		avk_renderer_finish(&h.renderer);
		goto done;
	}
	h.target = make_target(h.dev);
	if (h.target == NULL) {
		CHECK(false, "output target allocates");
		goto done;
	}

	test_lifetime(&h);
	test_throttled(&h);

done:
	if (h.target != NULL) {
		avk_image_destroy(h.dev, h.target);
	}
	if (h.renderer.dev != NULL) {
		avk_renderer_finish(&h.renderer);
	}
	if (h.dev != NULL) {
		avk_device_destroy(h.dev);
	}
	if (h.inst != NULL) {
		avk_instance_destroy(h.inst);
	}
	close(drm_fd);

	printf("----\n%d/%d checks passed\n", checks - failures, checks);
	return failures == 0 ? 0 : 1;
}
