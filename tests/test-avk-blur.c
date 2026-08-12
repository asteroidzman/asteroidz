/*
 * M4F.1/.10/.11: the dual-Kawase blur, on high-frequency fixtures.
 *
 * WHY FLAT COLOUR IS NOT A TEST. Blurring a flat field returns the same flat
 * field, whatever the kernel does -- so a flat fixture passes for a blur that
 * samples the wrong level, uses the wrong step, drops the diagonal taps, or
 * runs zero passes. That blindness is what produced the shadow glow and the
 * shadow hole in M4D, and the M4E notes record it as permanent. So every
 * primary fixture here has high spatial frequency, and the flat cases exist
 * only as secondary sanity checks with that stated.
 *
 * WHAT EACH FIXTURE CAN FALSIFY is written at the fixture, because "it looks
 * blurred" is not an assertion:
 *
 *   1px checkerboard   a blur that does nothing leaves 0/255 contrast; a
 *                      correct one converges to the mean
 *   impulse            energy conservation, symmetry, and the ringing
 *                      dual-Kawase is known to have
 *   hard edge          monotonicity across the transition, and the support
 *                      width the damage code depends on
 *   thin bright line   the case a too-small kernel step leaves visible
 *
 * Exits 77 (skip) with no GPU.
 */

#define _POSIX_C_SOURCE 200809L

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "render/vulkan/effect/avk_blur.h"
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
#define FMT VK_FORMAT_B8G8R8A8_UNORM

struct harness {
	struct avk_instance *inst;
	struct avk_device *dev;
	struct avk_renderer renderer;
	struct avk_image *src;
	struct avk_image *dst;
	uint32_t pixels[W * H];
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

static struct avk_image *make_image(struct avk_device *dev,
		VkImageUsageFlags usage) {
	struct avk_image *image = avk_image_alloc(dev);
	if (image == NULL) {
		return NULL;
	}
	image->format = FMT;
	image->extent = (VkExtent2D){ W, H };
	image->has_alpha = true;
	image->plane_count = 1;
	image->layout = VK_IMAGE_LAYOUT_UNDEFINED;

	VkImageCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = FMT,
		.extent = { W, H, 1 },
		.mipLevels = 1, .arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = usage,
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

	VkImageViewCreateInfo vi = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = image->image,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = FMT,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
	};
	vkCreateImageView(dev->dev, &vi, NULL, &image->view);
	AVK_LIVE_INC(dev, image_views);
	return image;
}

/* ── host <-> device ────────────────────────────────────────────────────── */

static bool stage(struct harness *h, struct avk_image *image,
		const uint32_t *src, bool to_device) {
	struct avk_device *dev = h->dev;
	VkDeviceSize size = (VkDeviceSize)W * H * 4;
	VkBuffer buffer;
	VkDeviceMemory memory;
	VkBufferCreateInfo bi = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = size,
		.usage = to_device ? VK_BUFFER_USAGE_TRANSFER_SRC_BIT
			: VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	};
	if (vkCreateBuffer(dev->dev, &bi, NULL, &buffer) != VK_SUCCESS) {
		return false;
	}
	VkMemoryRequirements reqs;
	vkGetBufferMemoryRequirements(dev->dev, buffer, &reqs);
	uint32_t type = find_memory(dev, reqs.memoryTypeBits,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
		| VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	VkMemoryAllocateInfo ai = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = reqs.size,
		.memoryTypeIndex = type,
	};
	if (type == UINT32_MAX
			|| vkAllocateMemory(dev->dev, &ai, NULL, &memory) != VK_SUCCESS) {
		vkDestroyBuffer(dev->dev, buffer, NULL);
		return false;
	}
	vkBindBufferMemory(dev->dev, buffer, memory, 0);
	if (to_device) {
		void *p = NULL;
		vkMapMemory(dev->dev, memory, 0, size, 0, &p);
		memcpy(p, src, (size_t)size);
		vkUnmapMemory(dev->dev, memory);
	}

	struct avk_cmd_ring ring;
	avk_cmd_ring_init(&ring, dev, "blur-stage");
	VkCommandBuffer cb = avk_cmd_ring_begin(&ring);

	VkImageLayout want = to_device ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
		: VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	VkImageMemoryBarrier2 b = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
		.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
		.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
		.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
		.dstAccessMask = to_device ? VK_ACCESS_2_TRANSFER_WRITE_BIT
			: VK_ACCESS_2_TRANSFER_READ_BIT,
		.oldLayout = image->layout,
		.newLayout = want,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = image->image,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
	};
	VkDependencyInfo dep = {
		.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
		.imageMemoryBarrierCount = 1,
		.pImageMemoryBarriers = &b,
	};
	vkCmdPipelineBarrier2(cb, &dep);
	image->layout = want;

	VkBufferImageCopy2 region = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
		.bufferRowLength = W,
		.bufferImageHeight = H,
		.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
		.imageExtent = { W, H, 1 },
	};
	if (to_device) {
		VkCopyBufferToImageInfo2 copy = {
			.sType = VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2,
			.srcBuffer = buffer, .dstImage = image->image,
			.dstImageLayout = want, .regionCount = 1, .pRegions = &region,
		};
		vkCmdCopyBufferToImage2(cb, &copy);
	} else {
		VkCopyImageToBufferInfo2 copy = {
			.sType = VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2,
			.srcImage = image->image, .srcImageLayout = want,
			.dstBuffer = buffer, .regionCount = 1, .pRegions = &region,
		};
		vkCmdCopyImageToBuffer2(cb, &copy);
	}

	uint64_t v = avk_cmd_ring_submit(&ring, NULL, 0, NULL, 0);
	bool ok = v != 0 && avk_device_timeline_wait(dev, v, 4000000000ULL);
	if (ok && !to_device) {
		void *p = NULL;
		vkMapMemory(dev->dev, memory, 0, size, 0, &p);
		memcpy(h->pixels, p, (size_t)size);
		vkUnmapMemory(dev->dev, memory);
	}
	avk_cmd_ring_finish(&ring);
	vkDestroyBuffer(dev->dev, buffer, NULL);
	vkFreeMemory(dev->dev, memory, NULL);
	return ok;
}

/* Run one blur chain, source -> destination, and read the result back. */
static bool blur(struct harness *h, const struct avk_blur_params *params,
		struct avk_blur_stats *stats) {
	struct avk_graph *g = &h->renderer.graph;
	avk_graph_reset(g);
	avk_blur_frame_reset();

	uint32_t rs = avk_graph_add_image(g, h->src, false, AVK_EXIT_KEEP);
	uint32_t rd = avk_graph_add_image(g, h->dst, false, AVK_EXIT_KEEP);
	if (rs == AVK_GRAPH_INVALID || rd == AVK_GRAPH_INVALID) {
		return false;
	}
	if (!avk_blur_declare(g, &h->renderer.transients, &h->renderer.pipes,
			stats, rs, rd, W, H, FMT, params)) {
		return false;
	}

	VkCommandBuffer cb = avk_cmd_ring_begin(&h->renderer.ring);
	if (cb == VK_NULL_HANDLE) {
		return false;
	}
	avk_graph_execute(g, cb, NULL, 0);
	uint64_t v = avk_cmd_ring_submit(&h->renderer.ring, NULL, 0, NULL, 0);
	avk_transient_release_frame(&h->renderer.transients, v);
	return v != 0 && avk_device_timeline_wait(h->dev, v, 4000000000ULL)
		&& stage(h, h->dst, NULL, false);
}

/* ── fixtures ───────────────────────────────────────────────────────────── */

static uint32_t rgb(int r, int g, int b) {
	return 0xff000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}
static int r_of(uint32_t p) { return (int)((p >> 16) & 0xff); }
static int g_of(uint32_t p) { return (int)((p >> 8) & 0xff); }
static int b_of(uint32_t p) { return (int)(p & 0xff); }
static int lum(uint32_t p) { return (r_of(p) + g_of(p) + b_of(p)) / 3; }

static void fill_checker(uint32_t *px, int cell) {
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			bool on = ((x / cell) + (y / cell)) & 1;
			px[y * W + x] = on ? rgb(255, 255, 255) : rgb(0, 0, 0);
		}
	}
}
static void fill_impulse(uint32_t *px) {
	for (int i = 0; i < W * H; i++) {
		px[i] = rgb(0, 0, 0);
	}
	px[(H / 2) * W + (W / 2)] = rgb(255, 255, 255);
}
static void fill_half(uint32_t *px) {
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			px[y * W + x] = x < W / 2 ? rgb(0, 0, 0) : rgb(255, 255, 255);
		}
	}
}
static void fill_lines(uint32_t *px) {
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			px[y * W + x] = (x % 8) == 0 ? rgb(255, 255, 255) : rgb(0, 0, 0);
		}
	}
}
static void fill_flat(uint32_t *px, int v) {
	for (int i = 0; i < W * H; i++) {
		px[i] = rgb(v, v, v);
	}
}

static struct avk_blur_params plain(uint32_t levels, float radius) {
	return (struct avk_blur_params){
		.levels = levels, .radius = radius,
		.brightness = 1.0f, .contrast = 1.0f, .saturation = 1.0f,
		.noise = 0.0f, .apply_effects = false,
	};
}

/* ── 1. the 1px checkerboard ────────────────────────────────────────────── */
static void test_checker(struct harness *h) {
	printf("\n-- 1px checkerboard --\n");

	uint32_t in[W * H];
	fill_checker(in, 1);
	CHECK(stage(h, h->src, in, true), "the checkerboard uploads");

	struct avk_blur_stats st = {0};
	struct avk_blur_params p = plain(2, 1.0f);
	CHECK(blur(h, &p, &st), "a 2-level blur runs");
	CHECK(st.passes == 4, "four passes for two levels (%" PRIu64 ")",
		st.passes);
	CHECK(st.transients == 2, "two transients, one per level (%" PRIu64 ")",
		st.transients);

	/*
	 * THE ASSERTION A FLAT FIXTURE CANNOT MAKE. A 1px checkerboard has
	 * alternating 0 and 255; a blur that does nothing leaves that contrast
	 * intact, and one that works converges every pixel toward the 127.5 mean.
	 * So the check is on the SPREAD, in the middle of the image where no edge
	 * clamping applies.
	 */
	int lo = 255, hi = 0;
	long sum = 0;
	int n = 0;
	for (int y = 32; y < H - 32; y++) {
		for (int x = 32; x < W - 32; x++) {
			int l = lum(h->pixels[y * W + x]);
			if (l < lo) { lo = l; }
			if (l > hi) { hi = l; }
			sum += l;
			n++;
		}
	}
	double mean = (double)sum / n;
	printf("  ---- interior: min %d  max %d  spread %d  mean %.1f\n",
		lo, hi, hi - lo, mean);
	CHECK(hi - lo < 24,
		"the checkerboard is flattened -- spread %d, from 255 (%d..%d)",
		hi - lo, lo, hi);
	/* And it converged to the RIGHT value: a kernel with wrong weights
	 * converges to a mean that is not the input's. */
	CHECK(fabs(mean - 127.5) < 12.0,
		"toward the input's own mean, 127.5 (got %.1f)", mean);
}

/* ── 2. the impulse ─────────────────────────────────────────────────────── */
static void test_impulse(struct harness *h) {
	printf("\n-- single-pixel impulse --\n");

	uint32_t in[W * H];
	fill_impulse(in);
	CHECK(stage(h, h->src, in, true), "the impulse uploads");

	struct avk_blur_params p = plain(2, 1.0f);
	CHECK(blur(h, &p, NULL), "the blur runs");

	int cx = W / 2, cy = H / 2;
	int centre = lum(h->pixels[cy * W + cx]);
	CHECK(centre > 0 && centre < 255,
		"the impulse spread: centre is %d, neither 255 nor 0", centre);

	/*
	 * SYMMETRY. The kernel is symmetric, so the response must be. An
	 * asymmetric result means a sign error or an off-by-half-texel in the
	 * step -- both of which produce a picture that still looks blurred and is
	 * shifted, which is exactly the class of bug a "looks fine" check misses.
	 */
	int left = lum(h->pixels[cy * W + (cx - 6)]);
	int right = lum(h->pixels[cy * W + (cx + 6)]);
	int up = lum(h->pixels[(cy - 6) * W + cx]);
	int down = lum(h->pixels[(cy + 6) * W + cx]);
	printf("  ---- centre %d   L %d R %d   U %d D %d\n",
		centre, left, right, up, down);
	CHECK(abs(left - right) <= 2, "horizontally symmetric (%d vs %d)",
		left, right);
	CHECK(abs(up - down) <= 2, "vertically symmetric (%d vs %d)", up, down);

	/* MONOTONIC falloff away from the impulse. Ringing would break this, and
	 * dual-Kawase is known to have a little -- so the tolerance is stated and
	 * measured rather than assumed to be zero. */
	int prev = centre;
	int worst_rise = 0;
	for (int d = 1; d < 24; d++) {
		int v = lum(h->pixels[cy * W + (cx + d)]);
		if (v > prev) {
			int rise = v - prev;
			if (rise > worst_rise) { worst_rise = rise; }
		}
		prev = v;
	}
	printf("  ---- worst non-monotonic rise along +x: %d codes\n", worst_rise);
	CHECK(worst_rise <= 3,
		"falloff is monotonic to within dual-Kawase's own ringing (%d codes)",
		worst_rise);
}

/* ── 3. a hard edge, and the support width ──────────────────────────────── */
static void test_edge(struct harness *h) {
	printf("\n-- hard edge --\n");

	uint32_t in[W * H];
	fill_half(in);
	CHECK(stage(h, h->src, in, true), "the edge uploads");

	struct avk_blur_params p = plain(2, 1.0f);
	CHECK(blur(h, &p, NULL), "the blur runs");

	int y = H / 2;
	/* Monotonic non-decreasing across the transition: an edge that overshoots
	 * on either side is ringing, and a step that is still a step is a blur
	 * that did nothing. */
	int prev = -1, drops = 0;
	for (int x = 8; x < W - 8; x++) {
		int v = lum(h->pixels[y * W + x]);
		if (prev >= 0 && v < prev - 2) { drops++; }
		prev = v;
	}
	CHECK(drops == 0, "the edge is monotonic left to right (%d drops)", drops);

	/* The transition WIDTH is what damage expansion depends on. Measured
	 * between the 10% and 90% crossings and compared against the support the
	 * blur itself reports -- if those disagree, damage is wrong. */
	int x10 = -1, x90 = -1;
	for (int x = 0; x < W; x++) {
		int v = lum(h->pixels[y * W + x]);
		if (x10 < 0 && v > 25) { x10 = x; }
		if (x90 < 0 && v > 229) { x90 = x; }
	}
	uint32_t support = avk_blur_support(&p);
	printf("  ---- 10%%..90%% transition: %d px (%d..%d), reported support %u\n",
		x90 - x10, x10, x90, support);
	CHECK(x10 > 0 && x90 > x10, "the transition is measurable");
	CHECK((uint32_t)(x90 - x10) <= support,
		"the measured transition (%d px) fits inside the support the blur "
		"reports (%u px), so damage expansion cannot under-cover it",
		x90 - x10, support);
}

/* ── 4. thin bright lines ───────────────────────────────────────────────── */
static void test_lines(struct harness *h) {
	printf("\n-- 1px lines on black, every 8px --\n");

	uint32_t in[W * H];
	fill_lines(in);
	CHECK(stage(h, h->src, in, true), "the lines upload");

	struct avk_blur_params p = plain(2, 1.0f);
	CHECK(blur(h, &p, NULL), "the blur runs");

	/*
	 * The case a too-small kernel step leaves visible. The lines are 8 px
	 * apart, so a blur whose support does not reach 8 px leaves them as
	 * separate ridges; one that does merges them into a near-uniform field.
	 * The mean must also be preserved: 1 lit pixel in 8 is 255/8 = 31.9.
	 */
	int lo = 255, hi = 0;
	long sum = 0; int n = 0;
	for (int y = 32; y < H - 32; y++) {
		for (int x = 32; x < W - 32; x++) {
			int l = lum(h->pixels[y * W + x]);
			if (l < lo) { lo = l; }
			if (l > hi) { hi = l; }
			sum += l; n++;
		}
	}
	printf("  ---- min %d  max %d  spread %d  mean %.1f (input mean 31.9)\n",
		lo, hi, hi - lo, (double)sum / n);
	CHECK(hi - lo < 40, "the ridges merged (spread %d)", hi - lo);
	CHECK(fabs((double)sum / n - 31.9) < 8.0,
		"and energy is preserved (%.1f vs 31.9)", (double)sum / n);
}

/* ── 5. levels actually change the result ───────────────────────────────── */
static void test_levels(struct harness *h) {
	printf("\n-- level count --\n");

	uint32_t in[W * H];
	fill_impulse(in);
	stage(h, h->src, in, true);

	int spread[4] = {0};
	for (uint32_t levels = 1; levels <= 3; levels++) {
		stage(h, h->src, in, true);
		struct avk_blur_params p = plain(levels, 1.0f);
		if (!blur(h, &p, NULL)) {
			continue;
		}
		/* How far from the centre the response is still above 1/255. */
		int reach = 0;
		for (int d = 1; d < W / 2 - 2; d++) {
			if (lum(h->pixels[(H / 2) * W + (W / 2 + d)]) > 0) { reach = d; }
		}
		spread[levels] = reach;
		printf("  ---- %u level(s): reach %d px, support reports %u\n",
			levels, reach, avk_blur_support(&p));
	}
	/*
	 * The point of dual-Kawase: radius comes from LEVELS. If more levels do not
	 * reach further, the chain is not actually iterating -- which is what a
	 * wrong level extent, a reused transient or an unbound descriptor produces,
	 * all of which still render something blurred.
	 */
	CHECK(spread[2] > spread[1] && spread[3] > spread[2],
		"each level reaches further than the last (%d, %d, %d px)",
		spread[1], spread[2], spread[3]);
}

/* ── 6. flat colour: a SECONDARY sanity check only ──────────────────────── */
static void test_flat(struct harness *h) {
	printf("\n-- flat colour (secondary) --\n");

	uint32_t in[W * H];
	fill_flat(in, 100);
	stage(h, h->src, in, true);
	struct avk_blur_params p = plain(2, 1.0f);
	CHECK(blur(h, &p, NULL), "the blur runs");

	int c = lum(h->pixels[(H / 2) * W + W / 2]);
	CHECK(abs(c - 100) <= 2, "a flat field survives blurring (%d vs 100)", c);
	printf("  ---- this check would ALSO pass for a blur that did nothing, "
		"which is why it is not a primary fixture\n");
}

/* ── 7. the effects fold-in ─────────────────────────────────────────────── */
static void test_effects(struct harness *h) {
	printf("\n-- brightness/contrast/saturation --\n");

	uint32_t in[W * H];
	fill_flat(in, 128);
	stage(h, h->src, in, true);

	struct avk_blur_params base = plain(2, 1.0f);
	CHECK(blur(h, &base, NULL), "plain blur runs");
	int plain_v = lum(h->pixels[(H / 2) * W + W / 2]);

	stage(h, h->src, in, true);
	struct avk_blur_params dim = plain(2, 1.0f);
	dim.brightness = 0.5f;
	dim.apply_effects = true;
	CHECK(blur(h, &dim, NULL), "dimmed blur runs");
	int dim_v = lum(h->pixels[(H / 2) * W + W / 2]);

	printf("  ---- brightness 1.0 -> %d, brightness 0.5 -> %d\n",
		plain_v, dim_v);
	CHECK(dim_v < plain_v * 3 / 4,
		"brightness 0.5 materially darkens the result (%d vs %d)",
		dim_v, plain_v);
	/* Black stays black: the shapes in blur.glsl are chosen so an HDR/PQ
	 * output cannot show a lifted floor as a grey glow. */
	uint32_t black[W * H];
	fill_flat(black, 0);
	stage(h, h->src, black, true);
	CHECK(blur(h, &dim, NULL), "blur of black runs");
	CHECK(lum(h->pixels[(H / 2) * W + W / 2]) == 0,
		"and black stays exactly black under the effects");
}

/* ── 8. no synchronisation of its own ───────────────────────────────────── */
static void test_architecture(struct harness *h) {
	printf("\n-- architecture --\n");

	uint32_t in[W * H];
	fill_checker(in, 1);
	stage(h, h->src, in, true);

	struct avk_transient_stats before = h->renderer.transients.stats;
	struct avk_blur_params p = plain(3, 1.0f);
	struct avk_blur_stats st = {0};
	CHECK(blur(h, &p, &st), "a 3-level blur runs");

	const struct avk_graph *g = &h->renderer.graph;
	CHECK(g->stats.passes == 6, "six passes (%u)", g->stats.passes);
	/*
	 * One barrier call per pass, all DERIVED. The blur declares usages and
	 * nothing else -- there is no vkCmdPipelineBarrier2 in avk_blur.c, which
	 * is the M4F.2 requirement, and this is what proves it rather than a grep.
	 */
	CHECK(g->stats.barriers == 6, "six barrier calls, one per pass (%u)",
		g->stats.barriers);
	CHECK(g->stats.buffer_barriers == 0, "and no buffer barriers");
	CHECK(h->renderer.transients.stats.acquires > before.acquires,
		"every intermediate came from the transient pool");
	CHECK(h->renderer.transients.stats.unsafe_reuses == 0,
		"and none was reused before the GPU had finished (%" PRIu64 ")",
		h->renderer.transients.stats.unsafe_reuses);

	/* Reuse: the same blur, many times, must stop allocating. */
	uint64_t creates = h->renderer.transients.stats.creates;
	for (int i = 0; i < 60; i++) {
		blur(h, &p, NULL);
	}
	avk_device_wait_idle(h->dev);
	CHECK(h->renderer.transients.stats.creates == creates,
		"60 further blurs allocated ZERO new images (%" PRIu64 " -> %" PRIu64
		")", creates, h->renderer.transients.stats.creates);
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	printf("== avk blur (M4F.1) ==\n");

	struct harness h;
	memset(&h, 0, sizeof(h));
	h.inst = avk_instance_create("avk-blur-test");
	if (h.inst == NULL) {
		SKIP("no Vulkan instance");
	}
	h.dev = avk_device_create(h.inst, -1);
	if (h.dev == NULL) {
		SKIP("no Vulkan device");
	}
	if (!avk_renderer_init(&h.renderer, h.dev, FMT)) {
		SKIP("no renderer");
	}
	h.src = make_image(h.dev, VK_IMAGE_USAGE_SAMPLED_BIT
		| VK_IMAGE_USAGE_TRANSFER_DST_BIT);
	h.dst = make_image(h.dev, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
		| VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
	if (h.src == NULL || h.dst == NULL) {
		SKIP("no images");
	}

	test_checker(&h);
	test_impulse(&h);
	test_edge(&h);
	test_lines(&h);
	test_levels(&h);
	test_flat(&h);
	test_effects(&h);
	test_architecture(&h);

	avk_device_wait_idle(h.dev);
	avk_image_destroy(h.dev, h.src);
	avk_image_destroy(h.dev, h.dst);
	avk_renderer_finish(&h.renderer);
	avk_device_destroy(h.dev);
	avk_instance_destroy(h.inst);

	printf("\n---- %d/%d checks passed\n", checks - failures, checks);
	return failures == 0 ? 0 : 1;
}
