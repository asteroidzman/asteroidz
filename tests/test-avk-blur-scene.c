/*
 * M4F.2A.2: a blur's source is the current-frame scene prefix.
 *
 * THE ONE STATEMENT THIS FILE MUST ESTABLISH:
 *
 *   A blur node's source is reconstructed entirely from the exact current-frame
 *   scene prefix behind that node, with no dependency on whatever pixels
 *   happened to exist in the output during any previous frame.
 *
 * HOW IT IS PROVED, and why not by inspecting the graph. Scene order is a
 * property of PIXELS: a foreground object either did or did not contribute to
 * the blur beneath it. So the same object is placed on both sides of the blur
 * node and the blur result is read back and compared against INDEPENDENTLY
 * CONSTRUCTED references -- scenes built to contain only what should have
 * contributed, blurred by the same primitive. The prefix path is never its own
 * oracle.
 *
 * THE FIXTURE IS BUILT TO BE HARD TO PASS. The background is a dark
 * high-frequency field and the foreground is a near-white block covering much of
 * the blur region, so contamination is not a subtle shift -- it is tens of codes
 * of brightening across the whole area.
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
#define BLUR_X 48
#define BLUR_Y 48
#define BLUR_W 160
#define BLUR_H 120
/* Deliberately overlapping most of the blur, and near-white. */
#define FG_X 72
#define FG_Y 64
#define FG_W 110
#define FG_H 80

struct harness {
	struct avk_instance *inst;
	struct avk_device *dev;
	struct avk_renderer renderer;
	struct avk_image *target;
	struct avk_image *bg;        /* the high-frequency background texture */
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
	avk_cmd_ring_init(&ring, dev, "blur-scene-stage", AVK_FRAMES_IN_FLIGHT);
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

/* ── scene construction ─────────────────────────────────────────────────── */

static void scene_start(struct avk_scene *scene) {
	avk_scene_init(scene);
	pixman_region32_union_rect(&scene->damage, &scene->damage, 0, 0, W, H);
	scene->has_clear = true;
	scene->clear_color[3] = 1.0f;
}

/* A: a dark, high-frequency background, drawn as a real client texture so the
 * prefix replay is exercising the full compositor renderer. */
static void add_background(struct harness *h, struct avk_scene *scene) {
	struct avk_cmd *c = avk_scene_add(scene, AVK_CMD_TEXTURE);
	c->dst = (struct avk_box){ 0, 0, W, H };
	c->image = h->bg;
	c->src = (struct avk_fbox){ 0, 0, W, H };
	c->opacity = 1.0f;
}

/* A shadowed rounded object, so the prefix contains an M4D material whose
 * dither is output-space anchored. */
static void add_shadowed_object(struct avk_scene *scene) {
	struct avk_cmd *s = avk_scene_add(scene, AVK_CMD_SHADOW);
	s->dst = (struct avk_box){ 20, 140, 120, 100 };
	s->opacity = 1.0f;
	s->color[3] = 0.55f;
	s->blur_sigma = 18.0f;
	for (int i = 0; i < 4; i++) { s->corners[i] = 10.0f; }
}

/* B: a near-white block crossing most of the blur region. If it ever reaches
 * the blur's source the result brightens by tens of codes everywhere. */
static void add_foreground(struct avk_scene *scene) {
	struct avk_cmd *c = avk_scene_add(scene, AVK_CMD_RECT);
	c->dst = (struct avk_box){ FG_X, FG_Y, FG_W, FG_H };
	c->opacity = 1.0f;
	c->color[0] = 0.97f; c->color[1] = 0.97f; c->color[2] = 0.94f;
	c->color[3] = 1.0f;
}

static struct avk_cmd *add_blur(struct avk_scene *scene, uint32_t levels) {
	struct avk_cmd *c = avk_scene_add(scene, AVK_CMD_BLUR);
	c->dst = (struct avk_box){ BLUR_X, BLUR_Y, BLUR_W, BLUR_H };
	c->opacity = 1.0f;
	c->blur_levels = levels;
	c->blur_radius = 2.0f;
	c->blur_brightness = 1.0f;
	c->blur_contrast = 1.0f;
	c->blur_saturation = 1.0f;
	return c;
}

static bool render(struct harness *h, const struct avk_scene *scene) {
	uint64_t v = avk_render_frame(&h->renderer, h->target, scene,
		NULL, 0, NULL, 0);
	return v != 0 && avk_device_timeline_wait(h->dev, v, 4000000000ULL)
		&& stage(h, h->target, NULL, h->pixels);
}

/* Mean luminance over the blur region, excluding where the foreground sits --
 * so the number describes the BLUR, not the object drawn over it. */
static double blur_mean(const uint32_t *px) {
	long sum = 0;
	int n = 0;
	for (int y = BLUR_Y + 4; y < BLUR_Y + BLUR_H - 4; y++) {
		for (int x = BLUR_X + 4; x < BLUR_X + BLUR_W - 4; x++) {
			if (x >= FG_X && x < FG_X + FG_W && y >= FG_Y && y < FG_Y + FG_H) {
				continue;
			}
			sum += lum(px[y * W + x]);
			n++;
		}
	}
	return n > 0 ? (double)sum / n : 0.0;
}

/* ── 1. scene order ─────────────────────────────────────────────────────── */
static void test_scene_order(struct harness *h) {
	printf("\n-- A, BLUR, B --\n");

	/*
	 * REFERENCE 1, built independently: the same background and shadow, the
	 * same blur, and NO foreground at all. Whatever the blur produces here is
	 * what it must produce when the foreground is added AFTER it.
	 */
	struct avk_scene ref;
	scene_start(&ref);
	add_background(h, &ref);
	add_shadowed_object(&ref);
	add_blur(&ref, 3);
	CHECK(render(h, &ref), "the reference (A, BLUR) renders");
	memcpy(h->saved, h->pixels, sizeof(h->saved));
	double ref_mean = blur_mean(h->saved);
	avk_scene_finish(&ref);

	/* The real scene: the foreground goes AFTER the blur. */
	struct avk_scene scene;
	scene_start(&scene);
	add_background(h, &scene);
	add_shadowed_object(&scene);
	add_blur(&scene, 3);
	add_foreground(&scene);
	CHECK(render(h, &scene), "the scene (A, BLUR, B) renders");
	double got_mean = blur_mean(h->pixels);

	printf("  ---- blur-region mean luminance: reference %.2f, scene %.2f\n",
		ref_mean, got_mean);

	/* Every blur pixel outside the foreground must match the reference. */
	int diff = 0, worst = 0;
	for (int y = BLUR_Y; y < BLUR_Y + BLUR_H; y++) {
		for (int x = BLUR_X; x < BLUR_X + BLUR_W; x++) {
			if (x >= FG_X && x < FG_X + FG_W && y >= FG_Y && y < FG_Y + FG_H) {
				continue;
			}
			uint32_t a = h->pixels[y * W + x], b = h->saved[y * W + x];
			if (a == b) {
				continue;
			}
			int d = abs(lum(a) - lum(b));
			if (d > worst) { worst = d; }
			diff++;
		}
	}
	printf("  ---- %d blur pixels differ from the no-foreground reference, "
		"worst %d codes\n", diff, worst);
	CHECK(diff == 0,
		"a foreground drawn AFTER the blur contributes NOTHING to it (%d "
		"pixels, worst %d codes)", diff, worst);

	/* And it is still there, sharp, on top. */
	uint32_t fg = h->pixels[(FG_Y + FG_H / 2) * W + (FG_X + FG_W / 2)];
	CHECK(lum(fg) > 230,
		"and it renders sharp above the blurred result (%d,%d,%d)",
		r_of(fg), g_of(fg), b_of(fg));
	avk_scene_finish(&scene);
}

/* ── 2. reversed order ──────────────────────────────────────────────────── */
static void test_reversed_order(struct harness *h) {
	printf("\n-- A, B, BLUR --\n");

	/*
	 * REFERENCE 2, again independent: background, shadow AND foreground, then
	 * the blur. The foreground is genuinely part of [0, k) now, so the blur
	 * must include it -- and the result must differ substantially from
	 * reference 1.
	 */
	struct avk_scene scene;
	scene_start(&scene);
	add_background(h, &scene);
	add_shadowed_object(&scene);
	add_foreground(&scene);
	add_blur(&scene, 3);
	CHECK(render(h, &scene), "the reversed scene (A, B, BLUR) renders");
	double rev_mean = blur_mean(h->pixels);

	/* Reference 1 again, for the comparison. */
	struct avk_scene ref;
	scene_start(&ref);
	add_background(h, &ref);
	add_shadowed_object(&ref);
	add_blur(&ref, 3);
	render(h, &ref);
	double ref_mean = blur_mean(h->pixels);
	avk_scene_finish(&ref);

	printf("  ---- blur-region mean: B after blur %.2f, B before blur %.2f, "
		"delta %.2f\n", ref_mean, rev_mean, rev_mean - ref_mean);
	/*
	 * THE POINT. The same object, the same geometry, the same blur -- only its
	 * scene position changed, and that alone decides whether it is in the
	 * source. A renderer keyed on node type, window type or a foreground flag
	 * would give the same answer both times.
	 */
	CHECK(rev_mean > ref_mean + 15.0,
		"the SAME object placed BEFORE the blur contributes substantially "
		"(%.2f vs %.2f)", rev_mean, ref_mean);
	avk_scene_finish(&scene);
}

/* ── 3. sample_exclude is structurally satisfied ────────────────────────── */
static void test_sample_exclude(struct harness *h) {
	printf("\n-- sample_exclude --\n");

	struct avk_scene with, without;
	scene_start(&with);
	add_background(h, &with);
	struct avk_cmd *b1 = add_blur(&with, 3);
	/* Exactly what the one audited producer sets: the owner's own footprint. */
	b1->has_sample_exclude = true;
	b1->sample_exclude = (struct avk_box){ FG_X, FG_Y, FG_W, FG_H };
	add_foreground(&with);
	CHECK(render(h, &with), "the scene with the metadata renders");
	memcpy(h->saved, h->pixels, sizeof(h->saved));
	avk_scene_finish(&with);

	scene_start(&without);
	add_background(h, &without);
	add_blur(&without, 3);
	add_foreground(&without);
	CHECK(render(h, &without), "and the same scene without it");

	int diff = 0;
	for (int i = 0; i < W * H; i++) {
		if (h->pixels[i] != h->saved[i]) {
			diff++;
		}
	}
	CHECK(diff == 0,
		"sample_exclude is PIXEL-NEUTRAL under prefix capture: the owner is "
		"not in [0,k) to begin with, so there is nothing to exclude (%d "
		"pixels)", diff);
	avk_scene_finish(&without);
}

/* ── 4. no frame history ────────────────────────────────────────────────── */
static void test_frame_history(struct harness *h) {
	printf("\n-- frame history --\n");

	/* Frame A: the foreground is present, BEFORE the blur, so it is genuinely
	 * in the source and the blur is bright. */
	struct avk_scene a;
	scene_start(&a);
	add_background(h, &a);
	add_foreground(&a);
	add_blur(&a, 3);
	CHECK(render(h, &a), "frame A renders with B in the source");
	double bright = blur_mean(h->pixels);
	avk_scene_finish(&a);

	/* Frame B: the foreground is GONE. If any pixel of the blur came from the
	 * previous frame's output, the brightness survives. */
	struct avk_scene b;
	scene_start(&b);
	add_background(h, &b);
	add_blur(&b, 3);
	CHECK(render(h, &b), "frame B renders with B removed");
	double after = blur_mean(h->pixels);
	memcpy(h->saved, h->pixels, sizeof(h->saved));

	/* A third render of the identical scene: the canonical value. */
	CHECK(render(h, &b), "and again, to give the settled value");
	double settled = blur_mean(h->pixels);
	printf("  ---- blur mean: with B %.2f, first frame without %.2f, "
		"settled %.2f\n", bright, after, settled);
	CHECK(bright > after + 15.0,
		"removing B removes its contribution IMMEDIATELY (%.2f -> %.2f)",
		bright, after);
	CHECK(after == settled || (after - settled < 0.01 && settled - after < 0.01),
		"and the very first frame without it already equals the settled "
		"value, so nothing carried over (%.2f vs %.2f)", after, settled);

	int diff = 0;
	for (int i = 0; i < W * H; i++) {
		if (h->pixels[i] != h->saved[i]) {
			diff++;
		}
	}
	CHECK(diff == 0, "two identical frames are bit-identical (%d pixels)",
		diff);
	avk_scene_finish(&b);
}

/* ── 5. static repeat ───────────────────────────────────────────────────── */
static void test_static_repeat(struct harness *h) {
	printf("\n-- static repeat --\n");

	struct avk_scene scene;
	scene_start(&scene);
	add_background(h, &scene);
	add_foreground(&scene);
	add_blur(&scene, 3);

	CHECK(render(h, &scene), "the first frame renders");
	memcpy(h->saved, h->pixels, sizeof(h->saved));
	avk_device_wait_idle(h->dev);
	uint64_t creates = h->renderer.transients.stats.creates;

	int drift = 0;
	for (int f = 0; f < 30; f++) {
		if (!render(h, &scene)) {
			continue;
		}
		for (int i = 0; i < W * H; i++) {
			if (h->pixels[i] != h->saved[i]) {
				drift++;
				break;
			}
		}
	}
	avk_device_wait_idle(h->dev);
	printf("  ---- 30 repeats: %d frames differed, transient creates "
		"%" PRIu64 " -> %" PRIu64 "\n", drift, creates,
		h->renderer.transients.stats.creates);
	/*
	 * A blur that fed on its own output would compound every frame -- the
	 * classic feedback halo, which grows rather than appearing at once. Thirty
	 * identical frames producing identical pixels is what rules it out.
	 */
	CHECK(drift == 0,
		"30 identical frames produce identical pixels -- no accumulation, no "
		"growing halo (%d differed)", drift);
	CHECK(h->renderer.transients.stats.creates == creates,
		"and allocated no new transients (%" PRIu64 ")",
		h->renderer.transients.stats.creates);
	CHECK(h->renderer.stats.cpu_sync_waits == h->renderer.ring.stalls,
		"every CPU wait was command-ring backpressure (%" PRIu64 " waits, "
		"%" PRIu64 " stalls)", h->renderer.stats.cpu_sync_waits,
		h->renderer.ring.stalls);
	avk_scene_finish(&scene);
}

/* ── 6. two blurs ───────────────────────────────────────────────────────── */
static void test_two_blurs(struct harness *h) {
	printf("\n-- A, BLUR1, B, BLUR2, C --\n");

	/*
	 * The architectural proof that a blur is not special-cased out of prefix
	 * replay: BLUR2's prefix is [0, k2), which CONTAINS BLUR1's command. Since
	 * every chain is declared in increasing scene order, BLUR1's result already
	 * exists when BLUR2's prefix replays it, and it composites there exactly as
	 * it does in the output.
	 *
	 * The two regions OVERLAP, so BLUR2 genuinely samples BLUR1's result.
	 */
	struct avk_scene scene;
	scene_start(&scene);
	add_background(h, &scene);
	struct avk_cmd *b1 = avk_scene_add(&scene, AVK_CMD_BLUR);
	b1->dst = (struct avk_box){ 30, 30, 110, 110 };
	b1->opacity = 1.0f;
	b1->blur_levels = 2; b1->blur_radius = 2.0f;
	b1->blur_brightness = 1.0f; b1->blur_contrast = 1.0f;
	b1->blur_saturation = 1.0f;
	add_foreground(&scene);
	struct avk_cmd *b2 = avk_scene_add(&scene, AVK_CMD_BLUR);
	b2->dst = (struct avk_box){ 90, 90, 120, 110 };
	b2->opacity = 1.0f;
	b2->blur_levels = 2; b2->blur_radius = 2.0f;
	b2->blur_brightness = 1.0f; b2->blur_contrast = 1.0f;
	b2->blur_saturation = 1.0f;

	uint64_t replays = h->renderer.blur_prefix_replays;
	CHECK(render(h, &scene), "a two-blur scene renders");
	printf("  ---- prefix replays this frame: %" PRIu64 ", commands replayed "
		"%" PRIu64 ", pixels %" PRIu64 "\n",
		h->renderer.blur_prefix_replays - replays,
		h->renderer.blur_prefix_commands, h->renderer.blur_prefix_pixels);
	CHECK(h->renderer.blur_prefix_replays - replays == 2,
		"two prefix captures, one per blur (%" PRIu64 ")",
		h->renderer.blur_prefix_replays - replays);

	/*
	 * BLUR2's region overlaps BLUR1's and sits after the foreground, so its
	 * source contains both -- it must be brighter than a BLUR2 whose prefix had
	 * neither. Built as an independent reference: the same second blur with
	 * nothing before it but the background.
	 */
	double got = 0;
	{
		/*
		 * The OVERLAP of blur1 (30,30 110x110) and blur2 (90,90 120x110),
		 * where the foreground also sits. Sampling outside it -- as the first
		 * version did, at 150..205 -- reaches only what blur2's support drags
		 * in from far away, so the delta was 1.17 codes and looked like a
		 * failure. It was measuring a region none of the interesting content
		 * covers.
		 */
		long sum = 0; int n = 0;
		for (int y = 95; y < 135; y++) {
			for (int x = 95; x < 135; x++) { sum += lum(h->pixels[y * W + x]); n++; }
		}
		got = (double)sum / n;
	}
	struct avk_scene ref;
	scene_start(&ref);
	add_background(h, &ref);
	struct avk_cmd *r2 = avk_scene_add(&ref, AVK_CMD_BLUR);
	r2->dst = (struct avk_box){ 90, 90, 120, 110 };
	r2->opacity = 1.0f;
	r2->blur_levels = 2; r2->blur_radius = 2.0f;
	r2->blur_brightness = 1.0f; r2->blur_contrast = 1.0f;
	r2->blur_saturation = 1.0f;
	render(h, &ref);
	double bare = 0;
	{
		long sum = 0; int n = 0;
		for (int y = 95; y < 135; y++) {
			for (int x = 95; x < 135; x++) { sum += lum(h->pixels[y * W + x]); n++; }
		}
		bare = (double)sum / n;
	}
	avk_scene_finish(&ref);

	printf("  ---- blur2 mean: with blur1+foreground before it %.2f, with only "
		"the background %.2f\n", got, bare);
	CHECK(got > bare + 4.0,
		"blur2's prefix genuinely contains what precedes it, including blur1's "
		"own composited result (%.2f vs %.2f)", got, bare);
	avk_scene_finish(&scene);
}

/* ── 7. the break ───────────────────────────────────────────────────────── */
static void test_break(struct harness *h) {
	printf("\n-- BREAK=blur-scene-after --\n");

	bool on = getenv("AZ_BLUR_SCENE_AFTER") != NULL;
	if (!on) {
		printf("  ---- not set; the assertion below is what it must break\n");
	}

	struct avk_scene ref;
	scene_start(&ref);
	add_background(h, &ref);
	add_blur(&ref, 3);
	render(h, &ref);
	memcpy(h->saved, h->pixels, sizeof(h->saved));
	double clean = blur_mean(h->saved);
	avk_scene_finish(&ref);

	struct avk_scene scene;
	scene_start(&scene);
	add_background(h, &scene);
	add_blur(&scene, 3);
	add_foreground(&scene);
	CHECK(render(h, &scene), "the scene renders");
	double got = blur_mean(h->pixels);

	int diff = 0, worst = 0;
	for (int y = BLUR_Y; y < BLUR_Y + BLUR_H; y++) {
		for (int x = BLUR_X; x < BLUR_X + BLUR_W; x++) {
			if (x >= FG_X && x < FG_X + FG_W && y >= FG_Y && y < FG_Y + FG_H) {
				continue;
			}
			int d = abs(lum(h->pixels[y * W + x]) - lum(h->saved[y * W + x]));
			if (d > 0) { diff++; }
			if (d > worst) { worst = d; }
		}
	}
	printf("  ---- blur mean %.2f (clean %.2f, delta %+.2f), %d wrong pixels, "
		"worst %d codes\n", got, clean, got - clean, diff, worst);
	CHECK(diff == 0,
		"the post-node foreground does not reach the blur (%d wrong pixels, "
		"worst %d codes)", diff, worst);
	avk_scene_finish(&scene);
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	printf("== avk blur scene order (M4F.2A.2) ==\n");

	struct harness h;
	memset(&h, 0, sizeof(h));
	h.inst = avk_instance_create("avk-blur-scene-test");
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
	if (h.target == NULL || h.bg == NULL) {
		SKIP("no images");
	}

	/* Dark, high-frequency: 1-3 px bright lines on near-black, plus dark
	 * patches. A flat background could not falsify any of this. */
	static uint32_t px[W * H];
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			int v = 8;
			if ((x % 7) == 0) { v = 200; }
			if ((y % 11) < 2) { v = 150; }
			if (((x / 16) + (y / 16)) & 1) { v = v / 3; }
			px[y * W + x] = rgb(v, v / 2, v / 4);
		}
	}
	if (!stage(&h, h.bg, px, NULL)) {
		SKIP("could not upload the background");
	}

	test_scene_order(&h);
	test_reversed_order(&h);
	test_sample_exclude(&h);
	test_frame_history(&h);
	test_static_repeat(&h);
	test_two_blurs(&h);
	test_break(&h);

	avk_device_wait_idle(h.dev);
	avk_image_destroy(h.dev, h.target);
	avk_image_destroy(h.dev, h.bg);
	avk_renderer_finish(&h.renderer);
	avk_device_destroy(h.dev);
	avk_instance_destroy(h.inst);

	printf("\n---- %d/%d checks passed\n", checks - failures, checks);
	return failures == 0 ? 0 : 1;
}
