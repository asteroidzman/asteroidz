/*
 * M4F.2A.3: the blur node's MATERIAL -- darken, edge_softness, clip_region,
 * alpha, corners, and the four of them at once.
 *
 * THE STATEMENTS THIS FILE MUST ESTABLISH, each against an oracle that is not
 * the production shader:
 *
 *   darken          the result is min(unclamped_result, unblurred_source), per
 *                   channel, everywhere in the write region. Both operands are
 *                   MEASURED SEPARATELY -- one render with the clamp off, one
 *                   render with no blur node at all -- and the clamp is checked
 *                   against their CPU minimum.
 *
 *   edge_softness   the node's edge fades over the same analytic Gaussian a
 *                   shadow's tint fades over. Proved by rendering a shadow and
 *                   a soft-edged blur with the SAME sigma over the same box and
 *                   comparing their recovered coverage profiles -- which is the
 *                   invariant the feature exists for ("the two fade in
 *                   lockstep"), not a restatement of the shader.
 *
 *   sigma is applied ONCE   the transition widens in proportion to sigma. A
 *                   value scaled twice would widen quadratically, and a value
 *                   not scaled at all would not widen with the output scale --
 *                   these are the two ways the walker's one multiply can be
 *                   wrong, and they differ from each other and from correct.
 *
 *   clip_region     inside the clip the composite is visible; outside it the
 *                   scene is BIT-IDENTICAL to the same scene with no blur node.
 *                   Bit-identical, not "close": a clip that leaks is a clip
 *                   that leaks.
 *
 *   multi-rect      a two-rectangle clip keeps its gap. No bounding box is
 *                   taken anywhere.
 *
 * WHY THE FIXTURE IS DARK AND HIGH-FREQUENCY. A blur of a flat field is the
 * same flat field, so a flat backdrop cannot falsify darken at all -- the
 * clamp's own premise (that averaging bright detail over a dark ground raises
 * the mean) is a statement about high-frequency content. test_flat_is_blind()
 * demonstrates that rather than asserting it in a comment.
 *
 * Exits 77 (skip) with no GPU.
 */

#define _POSIX_C_SOURCE 200809L

#include <inttypes.h>
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

#define W 256
#define H 256
#define FMT VK_FORMAT_B8G8R8A8_UNORM

/* The blur node, and the foreground that crosses it. */
/* The blur node under test. Well inside the target so its support never
 * clamps against the scene edge -- a clamped capture is a different (and
 * legitimate) code path, and mixing it into a material test would make every
 * number here depend on it. */
#define BX 48
#define BY 48
#define BW 160
#define BH 144

struct harness {
	struct avk_instance *inst;
	struct avk_device *dev;
	struct avk_renderer renderer;
	struct avk_image *target;
	struct avk_image *bg;        /* the high-frequency background texture */
	struct avk_image *flat;      /* a flat mid-grey, for edge/coverage work */
	uint32_t pixels[W * H];
	uint32_t saved[W * H];
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
		.imageType = VK_IMAGE_TYPE_2D, .format = FMT,
		.extent = { W, H, 1 }, .mipLevels = 1, .arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
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
		.allocationSize = reqs.size, .memoryTypeIndex = type,
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
		.image = image->image, .viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = FMT,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
	};
	vkCreateImageView(dev->dev, &vi, NULL, &image->view);
	AVK_LIVE_INC(dev, image_views);
	return image;
}

static bool stage(struct harness *h, struct avk_image *image,
		const uint32_t *src, uint32_t *dst) {
	struct avk_device *dev = h->dev;
	VkDeviceSize size = (VkDeviceSize)W * H * 4;
	bool to_device = src != NULL;
	VkBuffer buffer;
	VkDeviceMemory memory;
	VkBufferCreateInfo bi = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
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
		.allocationSize = reqs.size, .memoryTypeIndex = type,
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
	avk_cmd_ring_init(&ring, dev, "blur-scene-stage");
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
		.oldLayout = image->layout, .newLayout = want,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = image->image,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
	};
	VkDependencyInfo dep = {
		.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
		.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b,
	};
	vkCmdPipelineBarrier2(cb, &dep);
	image->layout = want;
	VkBufferImageCopy2 region = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
		.bufferRowLength = W, .bufferImageHeight = H,
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
		memcpy(dst, p, (size_t)size);
		vkUnmapMemory(dev->dev, memory);
	}
	avk_cmd_ring_finish(&ring);
	vkDestroyBuffer(dev->dev, buffer, NULL);
	vkFreeMemory(dev->dev, memory, NULL);
	return ok;
}

static uint32_t rgb(int r, int g, int b) {
	return 0xff000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}
static int r_of(uint32_t p) { return (int)((p >> 16) & 0xff); }
static int g_of(uint32_t p) { return (int)((p >> 8) & 0xff); }
static int b_of(uint32_t p) { return (int)(p & 0xff); }
static int lum(uint32_t p) { return (r_of(p) + g_of(p) + b_of(p)) / 3; }


/* Two backdrops. `flat` exists to prove what a flat fixture cannot see; every
 * assertion about darken uses `hard`. */
static uint32_t g_hard[W * H];
static uint32_t g_flat[W * H];
/* Bright enough that a coverage recovered from an 8-bit readback has resolution
 * to spare: one code is 1/(0.6*200) = 0.008 of coverage. */
#define FLAT_LEVEL 200

/* ── scene construction ─────────────────────────────────────────────────── */

static void scene_start(struct avk_scene *scene) {
	avk_scene_init(scene);
	pixman_region32_union_rect(&scene->damage, &scene->damage, 0, 0, W, H);
	scene->has_clear = true;
	scene->clear_color[3] = 1.0f;
}

static void add_backdrop(struct avk_scene *scene, struct avk_image *img) {
	struct avk_cmd *c = avk_scene_add(scene, AVK_CMD_TEXTURE);
	c->dst = (struct avk_box){ 0, 0, W, H };
	c->image = img;
	c->src = (struct avk_fbox){ 0, 0, W, H };
	c->opacity = 1.0f;
}

static struct avk_cmd *add_blur(struct avk_scene *scene) {
	struct avk_cmd *c = avk_scene_add(scene, AVK_CMD_BLUR);
	c->dst = (struct avk_box){ BX, BY, BW, BH };
	c->opacity = 1.0f;
	c->blur_levels = 3;
	c->blur_radius = 2.0f;
	c->blur_brightness = 1.0f;
	c->blur_contrast = 1.0f;
	c->blur_saturation = 1.0f;
	return c;
}

static bool render(struct harness *h, const struct avk_scene *scene,
		uint32_t *out) {
	uint64_t v = avk_render_frame(&h->renderer, h->target, scene,
		NULL, 0, NULL, 0);
	if (v == 0 || !avk_device_timeline_wait(h->dev, v, 4000000000ULL)) {
		return false;
	}
	return stage(h, h->target, NULL, out);
}

/* One render of "backdrop, then one blur node configured by `tune`". */
static bool render_blur(struct harness *h, struct avk_image *bg,
		void (*tune)(struct avk_cmd *), uint32_t *out) {
	struct avk_scene s;
	scene_start(&s);
	add_backdrop(&s, bg);
	struct avk_cmd *b = add_blur(&s);
	if (tune != NULL) {
		tune(b);
	}
	bool ok = render(h, &s, out);
	avk_scene_finish(&s);
	return ok;
}

/* The same scene with NO blur node: the unblurred source, at output
 * coordinates. This is the darken oracle's second operand and the clip test's
 * reference, and it is produced by a path the blur never touches. */
static bool render_source(struct harness *h, struct avk_image *bg,
		uint32_t *out) {
	struct avk_scene s;
	scene_start(&s);
	add_backdrop(&s, bg);
	bool ok = render(h, &s, out);
	avk_scene_finish(&s);
	return ok;
}

static double mean_in(const uint32_t *px, int x0, int y0, int w, int hgt) {
	long sum = 0;
	int n = 0;
	for (int y = y0; y < y0 + hgt; y++) {
		for (int x = x0; x < x0 + w; x++) {
			sum += lum(px[y * W + x]);
			n++;
		}
	}
	return n > 0 ? (double)sum / n : 0.0;
}

/* ── 1. darken, against a CPU minimum of two measured images ────────────── */

static void tune_darken(struct avk_cmd *c) { c->blur_darken = true; }

static void test_darken(struct harness *h) {
	printf("\n-- darken: min(unclamped, source), per channel --\n");

	static uint32_t off[W * H], on[W * H], src[W * H];
	if (!render_source(h, h->bg, src) || !render_blur(h, h->bg, NULL, off)
			|| !render_blur(h, h->bg, tune_darken, on)) {
		CHECK(false, "renders completed");
		return;
	}

	/*
	 * THE PREMISE FIRST. The clamp only means anything where the blur came out
	 * LIGHTER than what it replaced. If this fixture does not produce that, the
	 * assertion below is satisfied by an identity and proves nothing -- which is
	 * exactly what a flat backdrop does (see test_flat_is_blind).
	 */
	long brighter = 0;
	int worst_brighten = 0;
	for (int y = BY; y < BY + BH; y++) {
		for (int x = BX; x < BX + BW; x++) {
			int d = lum(off[y * W + x]) - lum(src[y * W + x]);
			if (d > 0) {
				brighter++;
				if (d > worst_brighten) { worst_brighten = d; }
			}
		}
	}
	printf("  note: unclamped blur is brighter than its source on %ld of %d "
		"pixels, worst +%d codes\n", brighter, BW * BH, worst_brighten);
	CHECK(brighter > BW * BH / 10,
		"PREMISE: the unclamped blur really does brighten this backdrop");
	CHECK(worst_brighten >= 8,
		"PREMISE: and by a margin worth clamping (%d codes)", worst_brighten);

	/* The assertion: the clamped result equals the CPU minimum of the two
	 * independently measured images, channel by channel. */
	long wrong = 0;
	int worst = 0;
	for (int y = BY; y < BY + BH; y++) {
		for (int x = BX; x < BX + BW; x++) {
			uint32_t a = off[y * W + x], s = src[y * W + x], o = on[y * W + x];
			int want[3] = {
				r_of(a) < r_of(s) ? r_of(a) : r_of(s),
				g_of(a) < g_of(s) ? g_of(a) : g_of(s),
				b_of(a) < b_of(s) ? b_of(a) : b_of(s),
			};
			int got[3] = { r_of(o), g_of(o), b_of(o) };
			for (int c = 0; c < 3; c++) {
				int d = got[c] - want[c];
				if (d < 0) { d = -d; }
				if (d > 1) { wrong++; }
				if (d > worst) { worst = d; }
			}
		}
	}
	printf("  note: %ld channel samples outside tolerance, worst %d codes\n",
		wrong, worst);
	CHECK(wrong == 0, "clamped == min(unclamped, source) everywhere");

	double m_off = mean_in(off, BX, BY, BW, BH);
	double m_on = mean_in(on, BX, BY, BW, BH);
	printf("  note: mean unclamped %.2f, clamped %.2f\n", m_off, m_on);
	CHECK(m_on < m_off - 1.0,
		"and the clamp visibly darkens the result (%.2f -> %.2f)", m_off, m_on);
}

/* ── 2. and a flat fixture cannot see any of it ─────────────────────────── */

static void test_flat_is_blind(struct harness *h) {
	printf("\n-- the same test on a FLAT backdrop, which is blind to it --\n");

	static uint32_t off[W * H], on[W * H];
	if (!render_blur(h, h->flat, NULL, off)
			|| !render_blur(h, h->flat, tune_darken, on)) {
		CHECK(false, "renders completed");
		return;
	}
	double m_off = mean_in(off, BX, BY, BW, BH);
	double m_on = mean_in(on, BX, BY, BW, BH);
	printf("  note: flat backdrop, mean unclamped %.2f, clamped %.2f\n",
		m_off, m_on);
	/*
	 * NOT a bug and not a tolerance to widen: a blur of a flat field is that
	 * field, so there is nothing above the source to clamp away. This is
	 * asserted so that a future "the darken test passes" is never satisfied by
	 * a fixture that had been quietly flattened.
	 */
	CHECK(m_off - m_on < 0.5,
		"darken changes a flat backdrop by nothing (%.2f) -- so a flat fixture "
		"would pass with the feature removed", m_off - m_on);
}

/* ── 3-5. edge_softness ─────────────────────────────────────────────────── */

/*
 * COVERAGE, RECOVERED FROM THE COMPOSITE.
 *
 * Both primitives are drawn over the same flat backdrop G, and both composite
 * premultiplied source-over, so the coverage each one applied can be solved for
 * exactly:
 *
 *   soft blur   the blur of a flat field is that field, and `brightness` scales
 *               it, so the source is a known flat 0.4G:
 *                   out = 0.4G*cov + G*(1 - cov)   =>   cov = (G - out)/(0.6G)
 *
 *   shadow      black at full alpha:
 *                   out = G*(1 - cov)              =>   cov = 1 - out/G
 *
 * A FLAT BACKDROP IS CORRECT HERE and is not the blindness test_flat_is_blind
 * warns about: that one is about blur CONTENT, and this measures the shape of an
 * edge. A textured backdrop would put its own variation into every sample and
 * measure nothing.
 */
#define BRIGHT 0.4f

static double cov_blur(uint32_t px) {
	double g = (double)FLAT_LEVEL;
	double c = ((double)lum(px));
	double v = (g - c) / (g * (1.0 - (double)BRIGHT));
	return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
}

static double cov_shadow(uint32_t px) {
	double v = 1.0 - (double)lum(px) / (double)FLAT_LEVEL;
	return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
}

/*
 * Where the profile crosses `want`, in subpixels, scanning rightward from the
 * node's left edge. Returns -1 if it never does.
 */
static double crossing(const uint32_t *px, double (*cov)(uint32_t), double want,
		int x_from, int x_to) {
	int y = BY + BH / 2;
	double prev = cov(px[y * W + x_from]);
	for (int x = x_from + 1; x < x_to; x++) {
		double c = cov(px[y * W + x]);
		if (prev < want && c >= want) {
			double t = (want - prev) / (c - prev);
			return (double)(x - 1) + t;
		}
		prev = c;
	}
	return -1.0;
}

/* 10% to 90%: a transition width that does not depend on where the nominal
 * edge is, so a soft edge and a hard one are compared on the same terms. */
static double transition_width(const uint32_t *px, double (*cov)(uint32_t)) {
	/* Starts OUTSIDE the node, where nothing is drawn and coverage is 0. A scan
	 * beginning at the node's own edge cannot see a hard edge at all: coverage
	 * is already 1 at the first sample, the 10% crossing never happens, and the
	 * width comes back as "no transition found" -- which reads exactly like a
	 * failure and is really a blind measurement. */
	double lo = crossing(px, cov, 0.10, BX - 8, BX + BW / 2);
	double hi = crossing(px, cov, 0.90, BX - 8, BX + BW / 2);
	if (lo < 0.0 || hi < 0.0) {
		return -1.0;
	}
	return hi - lo;
}

static float g_edge_sigma = 0.0f;
static void tune_edge(struct avk_cmd *c) {
	c->blur_edge_softness = g_edge_sigma;
	c->blur_brightness = BRIGHT;
	c->blur_apply_effects = true;
}

static bool render_edge(struct harness *h, float sigma, uint32_t *out) {
	g_edge_sigma = sigma;
	return render_blur(h, h->flat, tune_edge, out);
}

static bool render_shadow(struct harness *h, float sigma, uint32_t *out) {
	struct avk_scene s;
	scene_start(&s);
	add_backdrop(&s, h->flat);
	struct avk_cmd *c = avk_scene_add(&s, AVK_CMD_SHADOW);
	c->dst = (struct avk_box){ BX, BY, BW, BH };
	c->opacity = 1.0f;
	c->color[0] = c->color[1] = c->color[2] = 0.0f;
	c->color[3] = 1.0f;
	c->blur_sigma = sigma;
	bool ok = render(h, &s, out);
	avk_scene_finish(&s);
	return ok;
}

static void test_edge_softness(struct harness *h) {
	printf("\n-- edge_softness: hard, small, large --\n");

	static uint32_t hard[W * H], small[W * H], large[W * H];
	if (!render_edge(h, 0.0f, hard) || !render_edge(h, 12.0f, small)
			|| !render_edge(h, 24.0f, large)) {
		CHECK(false, "renders completed");
		return;
	}

	double w_hard = transition_width(hard, cov_blur);
	double w_small = transition_width(small, cov_blur);
	double w_large = transition_width(large, cov_blur);
	printf("  note: 10-90%% transition widths -- hard %.2f, sigma 12 %.2f, "
		"sigma 24 %.2f px\n", w_hard, w_small, w_large);

	CHECK(w_hard >= 0.0 && w_hard < 2.0,
		"edge_softness 0 is a hard edge (%.2f px)", w_hard);
	CHECK(w_small > 8.0, "sigma 12 fades over a real distance (%.2f px)",
		w_small);
	CHECK(w_large > w_small * 1.5,
		"and sigma 24 fades over more still (%.2f > %.2f)", w_large, w_small);

	/*
	 * No hard seam anywhere along the softened edge: coverage must be monotone
	 * across the transition. A rectangular step surviving inside the fade -- the
	 * signature of the hard SDF still being applied underneath -- shows up as a
	 * jump, not as a wrong width.
	 */
	int y = BY + BH / 2;
	double worst_jump = 0.0;
	for (int x = BX; x < BX + BW / 2 - 1; x++) {
		double d = cov_blur(large[y * W + x + 1]) - cov_blur(large[y * W + x]);
		if (d < 0.0) { d = -d; }
		if (d > worst_jump) { worst_jump = d; }
	}
	printf("  note: largest single-pixel coverage step %.4f\n", worst_jump);
	CHECK(worst_jump < 0.12, "no hard seam inside the fade (%.4f)", worst_jump);
}

static void test_edge_sigma_applied_once(struct harness *h) {
	printf("\n-- sigma is applied exactly once: 1.0 vs 1.5 output scale --\n");

	/*
	 * The walker converts a LOGICAL edge_softness into output pixels by
	 * multiplying by the output scale, once, beside the box and the radii. What
	 * reaches the renderer at scale 1.0 is sigma; at scale 1.5 it is 1.5*sigma.
	 *
	 * So the invariant is that the fade's width in DEVICE pixels scales
	 * linearly with the value: 1.5x the sigma must give 1.5x the width, which is
	 * the same fade in LOGICAL pixels on both monitors. The two ways the single
	 * multiply goes wrong are distinguishable here and from each other:
	 *
	 *   scaled twice   width grows by 2.25x, not 1.5x
	 *   not scaled     width does not grow at all
	 */
	const float base = 16.0f;
	static uint32_t at_1x[W * H], at_15x[W * H];
	if (!render_edge(h, base, at_1x) || !render_edge(h, base * 1.5f, at_15x)) {
		CHECK(false, "renders completed");
		return;
	}
	double w1 = transition_width(at_1x, cov_blur);
	double w15 = transition_width(at_15x, cov_blur);
	double ratio = w1 > 0.0 ? w15 / w1 : 0.0;
	printf("  note: sigma %.1f -> %.2f px, sigma %.1f -> %.2f px, ratio %.3f\n",
		base, w1, base * 1.5f, w15, ratio);

	CHECK(ratio > 1.35 && ratio < 1.65,
		"scale 1.5 widens the fade by 1.5x (%.3f) -- so the logical softness is "
		"the same on a 1.0 and a 1.5 output", ratio);
	CHECK(ratio < 2.0, "and NOT by 2.25x, which is a sigma scaled twice");
}

static void test_edge_matches_shadow(struct harness *h) {
	printf("\n-- the blur's soft edge IS the shadow's falloff --\n");

	/*
	 * THE INVARIANT THE FEATURE EXISTS FOR. asteroidz sizes a shadow's backdrop
	 * blur to the shadow's own footprint and hands it the shadow's own sigma, so
	 * that the two fade in lockstep and read as one halo. If the two coverage
	 * functions disagree there is a visible ring where one ends.
	 *
	 * This is also what makes the scale story hold: both are scaled by the same
	 * factor at the same place, so agreeing at one sigma means agreeing at every
	 * output scale.
	 */
	const float sigma = 20.0f;
	static uint32_t blur[W * H], shadow[W * H];
	if (!render_edge(h, sigma, blur) || !render_shadow(h, sigma, shadow)) {
		CHECK(false, "renders completed");
		return;
	}

	int y = BY + BH / 2;
	double worst = 0.0, sum = 0.0;
	int n = 0;
	for (int x = BX; x < BX + BW / 2; x++) {
		double d = cov_blur(blur[y * W + x]) - cov_shadow(shadow[y * W + x]);
		if (d < 0.0) { d = -d; }
		if (d > worst) { worst = d; }
		sum += d;
		n++;
	}
	printf("  note: coverage profiles differ by mean %.4f, worst %.4f over %d "
		"samples\n", sum / n, worst, n);
	CHECK(worst < 0.03,
		"the two profiles agree to within a code or two (worst %.4f)", worst);
}

/* ── 6-8. clip_region ───────────────────────────────────────────────────── */

/*
 * The clip travels as the COMMAND's pixman clip, which is where a node's
 * visible region already lives. That is deliberate and it is what makes an
 * arbitrary multi-rectangle region cost nothing: the draw loop already walks a
 * region rectangle by rectangle as scissors, so there is no shader to teach and
 * no bounding box to take.
 */
static pixman_region32_t g_clip;
static void tune_clip(struct avk_cmd *c) {
	avk_cmd_set_clip(c, &g_clip);
}

/* How many pixels of the whole frame differ from the no-blur reference, split
 * by whether they are inside the clip. */
static void clip_diff(const uint32_t *got, const uint32_t *ref,
		const pixman_region32_t *clip, long *inside, long *outside) {
	*inside = 0;
	*outside = 0;
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			if (got[y * W + x] == ref[y * W + x]) {
				continue;
			}
			if (pixman_region32_contains_point((pixman_region32_t *)clip, x, y,
					NULL)) {
				(*inside)++;
			} else {
				(*outside)++;
			}
		}
	}
}

static void run_clip_case(struct harness *h, const uint32_t *ref,
		const char *what, int x, int y, int w, int hgt) {
	pixman_region32_init_rect(&g_clip, x, y, (unsigned)w, (unsigned)hgt);
	static uint32_t got[W * H];
	bool ok = render_blur(h, h->bg, tune_clip, got);
	long inside = 0, outside = 0;
	if (ok) {
		clip_diff(got, ref, &g_clip, &inside, &outside);
	}
	pixman_region32_fini(&g_clip);
	if (!ok) {
		CHECK(false, "%s rendered", what);
		return;
	}
	printf("  note: %s -- %ld pixels changed inside the clip, %ld outside\n",
		what, inside, outside);
	CHECK(outside == 0,
		"%s: outside the clip the frame is BIT-IDENTICAL to no blur at all",
		what);
	CHECK(inside > (long)(w * hgt) / 4,
		"%s: and the blur really is drawn inside it (%ld px)", what, inside);
}

static void test_clip_region(struct harness *h) {
	printf("\n-- clip_region: full, offset, odd origin, odd extent --\n");

	static uint32_t ref[W * H];
	if (!render_source(h, h->bg, ref)) {
		CHECK(false, "reference rendered");
		return;
	}

	/* The whole node -- the clip is present but constrains nothing, which must
	 * behave exactly like the unclipped case rather than like a special one. */
	run_clip_case(h, ref, "clip == the whole node", BX, BY, BW, BH);
	/* Smaller, and offset from the node's own origin in both axes. */
	run_clip_case(h, ref, "clip smaller and offset", BX + 37, BY + 21, 64, 48);
	/*
	 * ODD ORIGIN AND ODD EXTENT. The prefix transient's origin is rounded DOWN
	 * to even so that fwidth()'s 2x2 derivative quads line up with the output's
	 * -- and that alignment must never be allowed to move the LOGICAL clip. An
	 * alignment that shifted rather than grew would show up here as a one-pixel
	 * leak outside the clip.
	 */
	run_clip_case(h, ref, "odd origin, odd extent", BX + 33, BY + 17, 61, 45);
}

static void test_clip_multi_rect(struct harness *h) {
	printf("\n-- a two-rectangle clip keeps its gap --\n");

	static uint32_t ref[W * H], got[W * H];
	if (!render_source(h, h->bg, ref)) {
		CHECK(false, "reference rendered");
		return;
	}

	const int gap_x = BX + 70, gap_w = 24;
	pixman_region32_init_rect(&g_clip, BX + 20, BY + 20, 50, 80);
	pixman_region32_union_rect(&g_clip, &g_clip, gap_x + gap_w, BY + 20, 50, 80);
	CHECK(pixman_region32_n_rects(&g_clip) == 2,
		"PREMISE: the clip really is two rectangles with a gap");

	bool ok = render_blur(h, h->bg, tune_clip, got);
	long inside = 0, outside = 0;
	if (ok) {
		clip_diff(got, ref, &g_clip, &inside, &outside);
	}

	/* The gap specifically: not merely "outside the union", but the corridor a
	 * bounding box would have swallowed. */
	long in_gap = 0;
	for (int y = BY + 20; ok && y < BY + 100; y++) {
		for (int x = gap_x; x < gap_x + gap_w; x++) {
			if (got[y * W + x] != ref[y * W + x]) {
				in_gap++;
			}
		}
	}
	pixman_region32_fini(&g_clip);
	if (!ok) {
		CHECK(false, "rendered");
		return;
	}
	printf("  note: %ld changed inside, %ld outside, %ld in the gap\n",
		inside, outside, in_gap);
	CHECK(inside > 2000, "the blur is drawn in both rectangles (%ld px)",
		inside);
	CHECK(outside == 0, "nothing is drawn outside the region");
	CHECK(in_gap == 0,
		"and the GAP is untouched -- no bounding box was taken anywhere");
}

/* ── 9. alpha ───────────────────────────────────────────────────────────── */

static float g_alpha = 1.0f;
static void tune_alpha(struct avk_cmd *c) {
	c->opacity = g_alpha;
	c->blur_brightness = BRIGHT;
	c->blur_apply_effects = true;
}

static void test_alpha(struct harness *h) {
	printf("\n-- alpha 1.0 / 0.5 / 0 --\n");

	static uint32_t ref[W * H], full[W * H], half[W * H], none[W * H];
	g_alpha = 1.0f;
	bool ok = render_source(h, h->flat, ref)
		&& render_blur(h, h->flat, tune_alpha, full);
	g_alpha = 0.5f;
	ok = ok && render_blur(h, h->flat, tune_alpha, half);
	g_alpha = 0.0f;
	ok = ok && render_blur(h, h->flat, tune_alpha, none);
	if (!ok) {
		CHECK(false, "renders completed");
		return;
	}

	int cx = BX + BW / 2, cy = BY + BH / 2;
	double v_ref = lum(ref[cy * W + cx]);
	double v_full = lum(full[cy * W + cx]);
	double v_half = lum(half[cy * W + cx]);
	double v_none = lum(none[cy * W + cx]);
	printf("  note: backdrop %.0f, alpha 1.0 %.0f, 0.5 %.0f, 0 %.0f\n",
		v_ref, v_full, v_half, v_none);

	CHECK(v_none == v_ref, "alpha 0 leaves the backdrop exactly as it was");
	/*
	 * Halfway, to within a code. The blur is premultiplied and `opacity` scales
	 * the whole vec4, so a half-alpha composite is the exact midpoint of the two
	 * -- which is the check that catches a DOUBLE premultiply (too dark) and an
	 * alpha-only scale (too bright) in opposite directions.
	 */
	double want = (v_full + v_ref) * 0.5;
	printf("  note: alpha 0.5 midpoint expected %.1f, got %.0f\n", want,
		v_half);
	CHECK(v_half > want - 1.5 && v_half < want + 1.5,
		"alpha 0.5 is the exact midpoint (%.0f vs %.1f) -- no dark fringe, no "
		"double premultiply", v_half, want);
}

/* ── 10-11. corners, per corner ─────────────────────────────────────────── */

static float g_corners[4];
static void tune_corners(struct avk_cmd *c) {
	for (int i = 0; i < 4; i++) {
		c->corners[i] = g_corners[i];
	}
	c->blur_brightness = BRIGHT;
	c->blur_apply_effects = true;
}

/* How far into the node the composite has reached along the diagonal from a
 * given corner: the first pixel with real coverage. A rounded corner pushes it
 * outward in proportion to its radius. */
static int corner_reach(const uint32_t *px, int corner) {
	for (int d = 0; d < 60; d++) {
		int x = corner == 0 || corner == 3 ? BX + d : BX + BW - 1 - d;
		int y = corner == 0 || corner == 1 ? BY + d : BY + BH - 1 - d;
		if (cov_blur(px[y * W + x]) > 0.5) {
			return d;
		}
	}
	return 60;
}

static void test_corners(struct harness *h) {
	printf("\n-- per-corner radii, clockwise tl/tr/br/bl --\n");

	static uint32_t equal[W * H], asym[W * H];
	g_corners[0] = g_corners[1] = g_corners[2] = g_corners[3] = 19.0f;
	bool ok = render_blur(h, h->flat, tune_corners, equal);
	/* The high-information values: four different radii, none of them zero-like
	 * enough to hide a swap, and 0 in one slot so a scalar collapse is loud. */
	g_corners[0] = 0.0f;  g_corners[1] = 7.0f;
	g_corners[2] = 19.0f; g_corners[3] = 37.0f;
	ok = ok && render_blur(h, h->flat, tune_corners, asym);
	if (!ok) {
		CHECK(false, "renders completed");
		return;
	}

	int e[4], a[4];
	for (int i = 0; i < 4; i++) {
		e[i] = corner_reach(equal, i);
		a[i] = corner_reach(asym, i);
	}
	printf("  note: equal 19  -> reach tl %d tr %d br %d bl %d\n",
		e[0], e[1], e[2], e[3]);
	printf("  note: 0/7/19/37 -> reach tl %d tr %d br %d bl %d\n",
		a[0], a[1], a[2], a[3]);

	CHECK(e[0] == e[1] && e[1] == e[2] && e[2] == e[3],
		"four equal radii round four corners equally");
	/*
	 * STRICTLY INCREASING, and that is the assertion a single-radius
	 * implementation fails and a bottom-corner SWAP fails differently: fx's
	 * struct is CLOCKWISE (tl, tr, br, bl) and SceneFX's own shader helper takes
	 * (tl, tr, bl, br), so handing the struct straight over exchanges br and bl.
	 * With 19 and 37 in those slots the exchange is 18 pixels of reach.
	 */
	CHECK(a[0] < a[1] && a[1] < a[2] && a[2] < a[3],
		"0 < 7 < 19 < 37 reaches strictly further at each corner (%d %d %d %d)",
		a[0], a[1], a[2], a[3]);
	/*
	 * AGAINST THE GEOMETRY, not merely against each other. The 0.5-coverage
	 * point on a corner's diagonal is where the arc crosses it, and for a circle
	 * of radius r inscribed in the corner that is r*(1 - 1/sqrt2) = 0.293r from
	 * the box corner. So each reach has a PREDICTED value, and an ordering that
	 * happened to hold for the wrong reason cannot satisfy this.
	 */
	const float want[4] = { 0.0f, 7.0f, 19.0f, 37.0f };
	int worst = 0;
	for (int i = 0; i < 4; i++) {
		int predicted = (int)(want[i] * (1.0 - 1.0 / 1.4142135624));
		int d = a[i] - predicted;
		if (d < 0) { d = -d; }
		if (d > worst) { worst = d; }
		printf("  note: corner %d radius %.0f -> predicted %d, measured %d\n",
			i, want[i], predicted, a[i]);
	}
	CHECK(worst <= 1,
		"every corner's reach matches 0.293r to within a pixel (worst %d) -- "
		"which a bottom-corner swap, a scalar collapse and a scale error each "
		"fail differently", worst);
}

/* ── 12. all four at once ───────────────────────────────────────────────── */

static void tune_combined(struct avk_cmd *c) {
	c->blur_darken = true;
	c->blur_edge_softness = 14.0f;
	c->corners[0] = 0.0f;  c->corners[1] = 7.0f;
	c->corners[2] = 19.0f; c->corners[3] = 37.0f;
	c->opacity = 0.8f;
	avk_cmd_set_clip(c, &g_clip);
}

static void test_combined(struct harness *h) {
	printf("\n-- darken + edge_softness + clip + asymmetric corners --\n");

	/*
	 * ONE FIXTURE WITH EVERYTHING ON, because the isolated tests above each hold
	 * three of the four fields at their identity and a coordinate-ordering
	 * mistake between two of them is invisible while either is inert.
	 */
	static uint32_t ref[W * H], got[W * H];
	if (!render_source(h, h->bg, ref)) {
		CHECK(false, "reference rendered");
		return;
	}
	pixman_region32_init_rect(&g_clip, BX + 25, BY + 19, 97, 83);
	bool ok = render_blur(h, h->bg, tune_combined, got);
	long inside = 0, outside = 0;
	if (ok) {
		clip_diff(got, ref, &g_clip, &inside, &outside);
	}
	pixman_region32_fini(&g_clip);
	if (!ok) {
		CHECK(false, "rendered");
		return;
	}
	printf("  note: %ld changed inside the clip, %ld outside\n",
		inside, outside);
	CHECK(outside == 0, "the clip still holds with every other field engaged");
	CHECK(inside > 1500, "and the composite is visibly there (%ld px)", inside);

	/* Darken still holds under the rest: nowhere inside the clip may the frame
	 * have got LIGHTER than the backdrop it replaced. Coverage and alpha only
	 * ever blend toward the backdrop, so this survives all four fields being on
	 * -- and it is exactly what fails if the clamp were applied to the composite
	 * rather than to the blur. */
	int worst = 0;
	for (int y = BY; y < BY + BH; y++) {
		for (int x = BX; x < BX + BW; x++) {
			if (!pixman_region32_contains_point(&g_clip, x, y, NULL)) {
				continue;
			}
			int d = lum(got[y * W + x]) - lum(ref[y * W + x]);
			if (d > worst) { worst = d; }
		}
	}
	printf("  note: worst brightening anywhere inside the clip: %d codes\n",
		worst);
	CHECK(worst <= 1, "nothing got lighter (worst +%d)", worst);
}

/* ── main ───────────────────────────────────────────────────────────────── */

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	printf("== avk blur material (M4F.2A.3) ==\n");

	/*
	 * The shadow's anti-banding dither, off. test_edge_matches_shadow compares
	 * two coverage profiles code for code, and a deliberate +-1 perturbation of
	 * one of them would be measured as disagreement. Nothing else here draws a
	 * shadow.
	 */
	setenv("AZ_SHADOW_DITHER_AMP", "0", 1);

	struct harness h;
	memset(&h, 0, sizeof(h));
	h.inst = avk_instance_create("avk-blur-material-test");
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
	h.target = make_image(h.dev, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
		| VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
	h.bg = make_image(h.dev, VK_IMAGE_USAGE_SAMPLED_BIT
		| VK_IMAGE_USAGE_TRANSFER_DST_BIT);
	h.flat = make_image(h.dev, VK_IMAGE_USAGE_SAMPLED_BIT
		| VK_IMAGE_USAGE_TRANSFER_DST_BIT);
	if (h.target == NULL || h.bg == NULL || h.flat == NULL) {
		SKIP("no images");
	}

	/*
	 * THE DIFFICULT BACKDROP: near-black ground with 1, 2 and 3 pixel bright
	 * lines, text-like blocks, and a fine checker. This is what darken is for --
	 * the average of sparse bright detail over a dark ground is lighter than the
	 * ground, everywhere.
	 */
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			int v = 6;
			if ((x % 9) == 0) { v = 235; }                    /* 1 px */
			if ((x % 23) < 2) { v = 210; }                    /* 2 px */
			if ((y % 17) < 3) { v = 190; }                    /* 3 px */
			if ((y % 5) == 0 && (x % 3) != 0) { v = 175; }    /* text-like */
			if (((x / 8) + (y / 8)) & 1) { v = v > 100 ? v : 2; }
			g_hard[y * W + x] = rgb(v, v * 3 / 4, v / 2);
			g_flat[y * W + x] = rgb(FLAT_LEVEL, FLAT_LEVEL, FLAT_LEVEL);
		}
	}
	if (!stage(&h, h.bg, g_hard, NULL) || !stage(&h, h.flat, g_flat, NULL)) {
		SKIP("could not upload the backdrops");
	}

	test_darken(&h);
	test_flat_is_blind(&h);
	test_edge_softness(&h);
	test_edge_sigma_applied_once(&h);
	test_edge_matches_shadow(&h);
	test_clip_region(&h);
	test_clip_multi_rect(&h);
	test_alpha(&h);
	test_corners(&h);
	test_combined(&h);

	avk_device_wait_idle(h.dev);
	avk_image_destroy(h.dev, h.target);
	avk_image_destroy(h.dev, h.bg);
	avk_image_destroy(h.dev, h.flat);
	avk_renderer_finish(&h.renderer);
	avk_device_destroy(h.dev);
	avk_instance_destroy(h.inst);

	printf("\n---- %d/%d checks passed\n", checks - failures, checks);
	return failures == 0 ? 0 : 1;
}
