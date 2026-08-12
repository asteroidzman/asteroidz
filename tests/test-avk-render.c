/*
 * M3 composition tests: AVK renders a scene snapshot, the pixels are read back
 * and compared.
 *
 * These are the tests that decide whether AVK is a compositor renderer. Every
 * one of them asserts on the framebuffer, not on a function returning true --
 * a renderer that binds the wrong sampler, mixes up premultiplied alpha, or
 * folds a transform the wrong way round returns success from every call it
 * makes and puts the wrong picture on screen.
 *
 * Composition happens in the surfaces' own encoding (8-bit sRGB-encoded,
 * premultiplied), which is what every SDR compositor does today and what makes
 * these results exact integers rather than tolerances. The linear FP16
 * pipeline is M5's job and will change these numbers deliberately.
 *
 * Exits 77 (skip) with no GPU.
 */

#define _POSIX_C_SOURCE 200809L

#include <drm_fourcc.h>
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

#define W 64
#define H 64
#define TARGET_FORMAT VK_FORMAT_B8G8R8A8_UNORM

struct harness {
	struct avk_instance *inst;
	struct avk_device *dev;
	struct avk_renderer renderer;
	struct avk_image *target;
	uint32_t pixels[W * H];
};

/* ── plumbing ───────────────────────────────────────────────────────────── */

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

static struct avk_image *make_image(struct avk_device *dev, uint32_t width,
		uint32_t height, VkImageUsageFlags usage, bool has_alpha) {
	/*
	 * avk_image_alloc(), not calloc(). A bare calloc leaves `life` at 0, and
	 * avk_image_destroy() correctly REFUSES to destroy an image that does not
	 * read as AVK_IMAGE_LIVE -- so every image this fixture made used to leak,
	 * and vkDestroyDevice reported 21 objects still alive. The suite passed
	 * throughout, because nothing it asserted was about teardown.
	 */
	struct avk_image *image = avk_image_alloc(dev);
	if (image == NULL) {
		return NULL;
	}
	image->format = TARGET_FORMAT;
	image->extent = (VkExtent2D){ width, height };
	image->has_alpha = has_alpha;
	image->layout = VK_IMAGE_LAYOUT_UNDEFINED;
	image->plane_count = 1;

	VkImageCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = TARGET_FORMAT,
		.extent = { width, height, 1 },
		.mipLevels = 1,
		.arrayLayers = 1,
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
	return image;
}

/* Fill an image from host pixels via a staging buffer -- the same route a SHM
 * client's contents take, so test 5 exercises the real upload shape. */
static bool upload(struct harness *h, struct avk_image *image,
		const uint32_t *src, uint32_t width, uint32_t height) {
	struct avk_device *dev = h->dev;
	VkDeviceSize size = (VkDeviceSize)width * height * 4;

	VkBuffer buffer;
	VkDeviceMemory memory;
	VkBufferCreateInfo bi = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = size,
		.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
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
	void *dst = NULL;
	vkMapMemory(dev->dev, memory, 0, size, 0, &dst);
	memcpy(dst, src, (size_t)size);
	vkUnmapMemory(dev->dev, memory);

	struct avk_cmd_ring ring;
	avk_cmd_ring_init(&ring, dev, "upload");
	VkCommandBuffer cb = avk_cmd_ring_begin(&ring);

	VkImageMemoryBarrier2 b = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
		.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
		.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
		.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
		.oldLayout = image->layout,
		.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
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

	VkBufferImageCopy2 region = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
		.bufferRowLength = width,
		.bufferImageHeight = height,
		.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
		.imageExtent = { width, height, 1 },
	};
	VkCopyBufferToImageInfo2 copy = {
		.sType = VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2,
		.srcBuffer = buffer,
		.dstImage = image->image,
		.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		.regionCount = 1,
		.pRegions = &region,
	};
	vkCmdCopyBufferToImage2(cb, &copy);
	image->layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

	uint64_t value = avk_cmd_ring_submit(&ring, NULL, 0, NULL, 0);
	bool ok = value != 0
		&& avk_device_timeline_wait(dev, value, 2000000000ULL);
	avk_cmd_ring_finish(&ring);
	vkDestroyBuffer(dev->dev, buffer, NULL);
	vkFreeMemory(dev->dev, memory, NULL);
	return ok;
}

static bool readback(struct harness *h) {
	struct avk_device *dev = h->dev;
	VkDeviceSize size = (VkDeviceSize)W * H * 4;
	VkBuffer buffer;
	VkDeviceMemory memory;

	VkBufferCreateInfo bi = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = size,
		.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
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

	struct avk_cmd_ring ring;
	avk_cmd_ring_init(&ring, dev, "readback");
	VkCommandBuffer cb = avk_cmd_ring_begin(&ring);

	VkImageMemoryBarrier2 b = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
		.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
		.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
		.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
		.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
		.oldLayout = h->target->layout,
		.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = h->target->image,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
	};
	VkDependencyInfo dep = {
		.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
		.imageMemoryBarrierCount = 1,
		.pImageMemoryBarriers = &b,
	};
	vkCmdPipelineBarrier2(cb, &dep);
	h->target->layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

	VkBufferImageCopy2 region = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
		.bufferRowLength = W,
		.bufferImageHeight = H,
		.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
		.imageExtent = { W, H, 1 },
	};
	VkCopyImageToBufferInfo2 copy = {
		.sType = VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2,
		.srcImage = h->target->image,
		.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		.dstBuffer = buffer,
		.regionCount = 1,
		.pRegions = &region,
	};
	vkCmdCopyImageToBuffer2(cb, &copy);

	uint64_t value = avk_cmd_ring_submit(&ring, NULL, 0, NULL, 0);
	bool ok = value != 0
		&& avk_device_timeline_wait(dev, value, 2000000000ULL);
	avk_cmd_ring_finish(&ring);

	if (ok) {
		void *mapped = NULL;
		if (vkMapMemory(dev->dev, memory, 0, size, 0, &mapped) == VK_SUCCESS) {
			memcpy(h->pixels, mapped, (size_t)size);
			vkUnmapMemory(dev->dev, memory);
		} else {
			ok = false;
		}
	}
	vkDestroyBuffer(dev->dev, buffer, NULL);
	vkFreeMemory(dev->dev, memory, NULL);
	return ok;
}

/* Target is B8G8R8A8: byte 0 is blue. Read as a little-endian uint32 that is
 * 0xAARRGGBB, so the accessors below match how the pattern is written. */
static uint32_t px(struct harness *h, uint32_t x, uint32_t y) {
	return h->pixels[y * W + x];
}
static int r_of(uint32_t p) { return (int)((p >> 16) & 0xFF); }
static int g_of(uint32_t p) { return (int)((p >> 8) & 0xFF); }
static int b_of(uint32_t p) { return (int)(p & 0xFF); }

static bool near(int got, int want, int tol) {
	return got >= want - tol && got <= want + tol;
}

static bool render(struct harness *h, struct avk_scene *scene) {
	uint64_t value = avk_render_frame(&h->renderer, h->target, scene, NULL, 0,
		NULL, 0);
	if (value == 0) {
		return false;
	}
	return avk_device_timeline_wait(h->dev, value, 2000000000ULL)
		&& readback(h);
}

/* ── test 1: overlapping surfaces and alpha ─────────────────────────────── */

static void test_overlap_alpha(struct harness *h) {
	printf("test 1: background + opaque rect + 50%% alpha surface\n");

	/* A 50%-alpha white surface, PREMULTIPLIED as Wayland requires: the
	 * colour channels are already scaled by alpha, so 50% white is 0x80 in
	 * every channel, not 0xFF. Writing 0xFF here is the classic
	 * straight-vs-premultiplied mistake and it makes the result too bright --
	 * which the assertions below would catch. */
	uint32_t src[16 * 16];
	for (int i = 0; i < 16 * 16; i++) {
		src[i] = 0x80808080u;
	}
	struct avk_image *surface = make_image(h->dev, 16, 16,
		VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, true);
	if (surface == NULL || !upload(h, surface, src, 16, 16)) {
		CHECK(false, "prepare the translucent surface");
		return;
	}

	struct avk_scene scene;
	avk_scene_init(&scene);
	pixman_region32_union_rect(&scene.damage, &scene.damage, 0, 0, W, H);

	/* blue background */
	scene.has_clear = true;
	scene.clear_color[0] = 0.0f;
	scene.clear_color[1] = 0.0f;
	scene.clear_color[2] = 1.0f;
	scene.clear_color[3] = 1.0f;

	/* opaque red rectangle */
	struct avk_cmd *rect = avk_scene_add(&scene, AVK_CMD_RECT);
	rect->dst = (struct avk_box){ 8, 8, 32, 32 };
	rect->color[0] = 1.0f;
	rect->color[1] = 0.0f;
	rect->color[2] = 0.0f;
	rect->color[3] = 1.0f;

	/* The translucent surface, straddling the rectangle's bottom-right
	 * corner: the rect covers 8..40, so a 16x16 surface at 32 is half over
	 * red and half over the blue background. Placing it entirely inside the
	 * rect would test one blend twice and call it two cases. */
	struct avk_cmd *tex = avk_scene_add(&scene, AVK_CMD_TEXTURE);
	tex->dst = (struct avk_box){ 32, 32, 16, 16 };
	tex->image = surface;
	tex->src = (struct avk_fbox){ 0, 0, 16, 16 };

	CHECK(render(h, &scene), "rendered");

	CHECK(r_of(px(h, 2, 2)) == 0 && b_of(px(h, 2, 2)) == 255,
		"background is blue (%d,%d,%d)", r_of(px(h, 2, 2)),
		g_of(px(h, 2, 2)), b_of(px(h, 2, 2)));
	CHECK(r_of(px(h, 12, 12)) == 255 && b_of(px(h, 12, 12)) == 0,
		"the rectangle is red (%d,%d,%d)", r_of(px(h, 12, 12)),
		g_of(px(h, 12, 12)), b_of(px(h, 12, 12)));

	/* 50% white over red: 0x80 + 255*(1-0.5) = 128 + 127 = 255 in red,
	 * 128 + 0 = 128 in green and blue. */
	uint32_t over_red = px(h, 34, 34);
	CHECK(near(r_of(over_red), 255, 1) && near(g_of(over_red), 128, 1)
			&& near(b_of(over_red), 128, 1),
		"50%% white over red is (255,128,128), got (%d,%d,%d)",
		r_of(over_red), g_of(over_red), b_of(over_red));

	/* 50% white over blue: (128, 128, 255). */
	uint32_t over_blue = px(h, 44, 44);
	CHECK(near(r_of(over_blue), 128, 1) && near(g_of(over_blue), 128, 1)
			&& near(b_of(over_blue), 255, 1),
		"50%% white over blue is (128,128,255), got (%d,%d,%d)",
		r_of(over_blue), g_of(over_blue), b_of(over_blue));

	avk_scene_finish(&scene);
	avk_image_destroy(h->dev, surface);
}

/* ── test 2: source crop and destination scale ──────────────────────────── */

static void test_crop_scale(struct harness *h) {
	printf("test 2: source crop + destination scale\n");

	/* Four quadrants, each a flat colour, so a crop can be checked by which
	 * colours survive and a scale by where the boundary lands. */
	uint32_t src[32 * 32];
	for (uint32_t y = 0; y < 32; y++) {
		for (uint32_t x = 0; x < 32; x++) {
			uint32_t c;
			if (x < 16 && y < 16)       c = 0xFFFF0000u;  /* TL red   */
			else if (x >= 16 && y < 16) c = 0xFF00FF00u;  /* TR green */
			else if (x < 16)            c = 0xFF0000FFu;  /* BL blue  */
			else                        c = 0xFFFFFF00u;  /* BR yellow*/
			src[y * 32 + x] = c;
		}
	}
	struct avk_image *surface = make_image(h->dev, 32, 32,
		VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, true);
	if (surface == NULL || !upload(h, surface, src, 32, 32)) {
		CHECK(false, "prepare the quadrant surface");
		return;
	}

	struct avk_scene scene;
	avk_scene_init(&scene);
	pixman_region32_union_rect(&scene.damage, &scene.damage, 0, 0, W, H);
	scene.has_clear = true;
	scene.clear_color[3] = 1.0f;   /* black */

	/* Crop the bottom-right quadrant (yellow) and blow it up to fill the
	 * whole target: every pixel must be yellow. A crop that ignored the
	 * source offset would show red; one that ignored the size would show all
	 * four colours. */
	struct avk_cmd *tex = avk_scene_add(&scene, AVK_CMD_TEXTURE);
	tex->dst = (struct avk_box){ 0, 0, W, H };
	tex->image = surface;
	tex->src = (struct avk_fbox){ 16, 16, 16, 16 };
	tex->filter_linear = false;

	CHECK(render(h, &scene), "rendered a 16x16 crop scaled to 64x64");

	int wrong = 0;
	for (uint32_t y = 0; y < H; y++) {
		for (uint32_t x = 0; x < W; x++) {
			uint32_t p = px(h, x, y);
			if (r_of(p) != 255 || g_of(p) != 255 || b_of(p) != 0) {
				wrong++;
			}
		}
	}
	CHECK(wrong == 0, "all %d pixels are the cropped yellow quadrant "
		"(%d wrong)", W * H, wrong);

	/* Now crop the top half and scale it into the top half only, so the
	 * boundary position is checked as well as the colours. */
	avk_scene_finish(&scene);
	avk_scene_init(&scene);
	pixman_region32_union_rect(&scene.damage, &scene.damage, 0, 0, W, H);
	scene.has_clear = true;
	scene.clear_color[3] = 1.0f;

	tex = avk_scene_add(&scene, AVK_CMD_TEXTURE);
	tex->dst = (struct avk_box){ 0, 0, W, 32 };
	tex->image = surface;
	tex->src = (struct avk_fbox){ 0, 0, 32, 16 };

	CHECK(render(h, &scene), "rendered the top half");
	CHECK(r_of(px(h, 8, 8)) == 255 && g_of(px(h, 8, 8)) == 0,
		"top-left is red");
	CHECK(r_of(px(h, 56, 8)) == 0 && g_of(px(h, 56, 8)) == 255,
		"top-right is green");
	CHECK(r_of(px(h, 8, 40)) == 0 && g_of(px(h, 8, 40)) == 0
			&& b_of(px(h, 8, 40)) == 0,
		"below the destination box is untouched black");

	avk_scene_finish(&scene);
	avk_image_destroy(h->dev, surface);
}

/* ── rounded clipping: per corner, and owned by the destination ─────────── */

/*
 * M4A. Four DIFFERENT radii and a corner-coded source, so every mistake the
 * audit identified is separately visible:
 *
 *   one radius for all four   -> every corner cut the same, TL stops being square
 *   bottom corners swapped    -> BR and BL cut by each other's radius (19 vs 37)
 *   rounding in source space  -> a crop or a destination scale moves the arcs
 *   radii not permuted with a transform -> the right corner keeps the wrong arc
 *
 * The clear colour is opaque black, so "clipped" is measurable: a pixel inside
 * the arc carries the source quadrant's colour and one outside it is black.
 */
#define R_TL 0
#define R_TR 7
#define R_BR 19
#define R_BL 37

static bool clipped(struct harness *h, uint32_t x, uint32_t y) {
	uint32_t p = px(h, x, y);
	return r_of(p) == 0 && g_of(p) == 0 && b_of(p) == 0;
}

/* The deepest inset along a corner's diagonal that is still cut away. For a
 * quarter-disc of radius r the diagonal is cut for about r*(1 - 1/sqrt2)
 * pixels, so this rises and falls with r without having to model the AA. */
static int corner_cut_depth(struct harness *h, int cx, int cy, int dx, int dy) {
	int depth = 0;
	for (int i = 0; i < 48; i++) {
		int x = cx + (dx > 0 ? i : -i - 1);
		int y = cy + (dy > 0 ? i : -i - 1);
		if (x < 0 || y < 0 || x >= (int)W || y >= (int)H) {
			break;
		}
		if (!clipped(h, (uint32_t)x, (uint32_t)y)) {
			break;
		}
		depth = i + 1;
	}
	return depth;
}

static struct avk_image *quadrant_surface(struct harness *h) {
	uint32_t src[32 * 32];
	for (uint32_t y = 0; y < 32; y++) {
		for (uint32_t x = 0; x < 32; x++) {
			uint32_t c;
			if (x < 16 && y < 16)       c = 0xFFFF0000u;  /* TL red    */
			else if (x >= 16 && y < 16) c = 0xFF00FF00u;  /* TR green  */
			else if (x < 16)            c = 0xFF0000FFu;  /* BL blue   */
			else                        c = 0xFFFFFF00u;  /* BR yellow */
			src[y * 32 + x] = c;
		}
	}
	struct avk_image *s = make_image(h->dev, 32, 32,
		VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, true);
	if (s == NULL || !upload(h, s, src, 32, 32)) {
		return NULL;
	}
	return s;
}

static void scene_begin(struct avk_scene *scene) {
	avk_scene_init(scene);
	pixman_region32_union_rect(&scene->damage, &scene->damage, 0, 0, W, H);
	scene->has_clear = true;
	scene->clear_color[3] = 1.0f;   /* opaque black */
}

static void test_rounded_asymmetric(struct harness *h) {
	printf("test 6: per-corner rounded clipping\n");
	struct avk_image *surface = quadrant_surface(h);
	if (surface == NULL) {
		CHECK(false, "prepare the quadrant surface");
		return;
	}

	struct avk_scene scene;
	scene_begin(&scene);
	struct avk_cmd *tex = avk_scene_add(&scene, AVK_CMD_TEXTURE);
	tex->dst = (struct avk_box){ 0, 0, W, H };
	tex->image = surface;
	tex->src = (struct avk_fbox){ 0, 0, 32, 32 };
	tex->corners[0] = R_TL; tex->corners[1] = R_TR;
	tex->corners[2] = R_BR; tex->corners[3] = R_BL;
	CHECK(render(h, &scene), "rendered four different radii");

	int d_tl = corner_cut_depth(h, 0, 0, 1, 1);
	int d_tr = corner_cut_depth(h, W, 0, -1, 1);
	int d_br = corner_cut_depth(h, W, H, -1, -1);
	int d_bl = corner_cut_depth(h, 0, H, 1, -1);
	printf("  cut depth  TL=%d TR=%d BR=%d BL=%d  (radii %d %d %d %d)\n",
		d_tl, d_tr, d_br, d_bl, R_TL, R_TR, R_BR, R_BL);

	CHECK(d_tl == 0, "TL radius 0 leaves the corner square (cut %d)", d_tl);
	CHECK(d_tr > 0, "TR radius 7 cuts the corner (%d)", d_tr);
	/* THE ordering assertion. Swapping the two bottom corners exchanges 19
	 * and 37, and nothing else in the suite would notice. */
	CHECK(d_br > d_tr, "BR radius 19 cuts deeper than TR's 7 (%d > %d)",
		d_br, d_tr);
	CHECK(d_bl > d_br, "BL radius 37 cuts deeper than BR's 19 (%d > %d)",
		d_bl, d_br);

	/* The surviving content at each corner must be that corner's own quadrant:
	 * a build that clipped correctly but sampled wrongly would pass the depth
	 * checks alone. */
	CHECK(r_of(px(h, 3, 3)) > 200 && g_of(px(h, 3, 3)) < 60,
		"the square TL corner still shows the red quadrant");
	CHECK(b_of(px(h, W - 4, H - 4 - d_br)) < 60
			&& r_of(px(h, W - 4, H - 4 - d_br)) > 150,
		"content under the BR arc is the yellow quadrant");

	avk_scene_finish(&scene);
	avk_image_destroy(h->dev, surface);
}

static void test_rounded_destination_space(struct harness *h) {
	printf("test 7: rounding follows the destination, not the source\n");
	struct avk_image *surface = quadrant_surface(h);
	if (surface == NULL) {
		CHECK(false, "prepare the quadrant surface");
		return;
	}

	/*
	 * Crop ONE quadrant and blow it up to the whole target. Every pixel that
	 * survives is yellow, so the arcs cannot be coming from the source's own
	 * corners -- the source crop has no corners of its own in the destination.
	 * If coverage were computed in source space the arcs would shrink by the
	 * 4x scale factor, or land in the wrong place entirely.
	 */
	struct avk_scene scene;
	scene_begin(&scene);
	struct avk_cmd *tex = avk_scene_add(&scene, AVK_CMD_TEXTURE);
	tex->dst = (struct avk_box){ 0, 0, W, H };
	tex->image = surface;
	tex->src = (struct avk_fbox){ 16, 16, 16, 16 };   /* BR quadrant only */
	tex->corners[0] = R_TL; tex->corners[1] = R_TR;
	tex->corners[2] = R_BR; tex->corners[3] = R_BL;
	CHECK(render(h, &scene), "rendered a 16x16 crop upscaled 4x, rounded");

	int u_tr = corner_cut_depth(h, W, 0, -1, 1);
	int u_br = corner_cut_depth(h, W, H, -1, -1);
	int u_bl = corner_cut_depth(h, 0, H, 1, -1);
	printf("  upscaled crop cut depth  TR=%d BR=%d BL=%d\n", u_tr, u_br, u_bl);
	CHECK(u_bl > u_br && u_br > u_tr,
		"the destination's own radii still order the corners (%d > %d > %d)",
		u_bl, u_br, u_tr);
	CHECK(r_of(px(h, W / 2, H / 2)) > 200 && g_of(px(h, W / 2, H / 2)) > 200
			&& b_of(px(h, W / 2, H / 2)) < 60,
		"the interior is the cropped yellow quadrant");

	/* Now DOWNSCALE the whole source into a small destination. The radii are
	 * in destination pixels, so the arcs must not shrink with the content. */
	avk_scene_finish(&scene);
	scene_begin(&scene);
	tex = avk_scene_add(&scene, AVK_CMD_TEXTURE);
	tex->dst = (struct avk_box){ 0, 0, 48, 48 };
	tex->image = surface;
	tex->src = (struct avk_fbox){ 0, 0, 32, 32 };
	tex->corners[0] = 0; tex->corners[1] = 4;
	tex->corners[2] = 16; tex->corners[3] = 0;
	CHECK(render(h, &scene), "rendered the source downscaled into 48x48");
	int s_tr = corner_cut_depth(h, 48, 0, -1, 1);
	int s_br = corner_cut_depth(h, 48, 48, -1, -1);
	printf("  downscaled cut depth  TR=%d BR=%d\n", s_tr, s_br);
	CHECK(s_br > s_tr, "the destination radii survive a downscale (%d > %d)",
		s_br, s_tr);
	CHECK(clipped(h, 60, 60), "nothing is drawn outside the 48x48 destination");

	avk_scene_finish(&scene);
	avk_image_destroy(h->dev, surface);
}

static void test_rounded_with_transform(struct harness *h) {
	printf("test 8: rounded clipping composed with a transform\n");
	struct avk_image *surface = quadrant_surface(h);
	if (surface == NULL) {
		CHECK(false, "prepare the quadrant surface");
		return;
	}

	/*
	 * The defect this catches: the texture transform is applied and the radii
	 * are not, so the right corner shows the right CONTENT with the wrong ARC.
	 * A symmetric-radius test cannot see it at all.
	 *
	 * The radii handed to the renderer are already in output space -- the
	 * compositor permutes them (az_avk_corners_from_scenefx) before they get
	 * here -- so what is asserted here is that the two are applied to the SAME
	 * corner, independently of which corner the compositor chose.
	 */
	struct avk_scene scene;
	scene_begin(&scene);
	struct avk_cmd *tex = avk_scene_add(&scene, AVK_CMD_TEXTURE);
	tex->dst = (struct avk_box){ 0, 0, W, H };
	tex->image = surface;
	tex->src = (struct avk_fbox){ 0, 0, 32, 32 };
	tex->transform = AVK_TRANSFORM_90;
	tex->corners[0] = R_TL; tex->corners[1] = R_TR;
	tex->corners[2] = R_BR; tex->corners[3] = R_BL;
	CHECK(render(h, &scene), "rendered 90-degree transform with four radii");

	int t_tl = corner_cut_depth(h, 0, 0, 1, 1);
	int t_tr = corner_cut_depth(h, W, 0, -1, 1);
	int t_br = corner_cut_depth(h, W, H, -1, -1);
	int t_bl = corner_cut_depth(h, 0, H, 1, -1);
	printf("  transformed cut depth  TL=%d TR=%d BR=%d BL=%d\n",
		t_tl, t_tr, t_br, t_bl);
	CHECK(t_tl == 0 && t_bl > t_br && t_br > t_tr,
		"the arcs stay on the physical corners they were given");
	/* 90 degrees puts the source's BLUE (bottom-left) quadrant top-left --
	 * the existing transform test asserts the same mapping. */
	CHECK(b_of(px(h, 4, 4)) > 200 && r_of(px(h, 4, 4)) < 60,
		"the transformed content is in place too (blue at top-left)");

	avk_scene_finish(&scene);
	avk_image_destroy(h->dev, surface);
}

/* ── test 3: transforms ─────────────────────────────────────────────────── */

static void test_transforms(struct harness *h) {
	printf("test 3: all eight transforms\n");

	uint32_t src[32 * 32];
	for (uint32_t y = 0; y < 32; y++) {
		for (uint32_t x = 0; x < 32; x++) {
			uint32_t c;
			if (x < 16 && y < 16)       c = 0xFFFF0000u;  /* TL red    */
			else if (x >= 16 && y < 16) c = 0xFF00FF00u;  /* TR green  */
			else if (x < 16)            c = 0xFF0000FFu;  /* BL blue   */
			else                        c = 0xFFFFFF00u;  /* BR yellow */
			src[y * 32 + x] = c;
		}
	}
	struct avk_image *surface = make_image(h->dev, 32, 32,
		VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, true);
	if (surface == NULL || !upload(h, surface, src, 32, 32)) {
		CHECK(false, "prepare the quadrant surface");
		return;
	}

	/* Which source quadrant ends up in the destination's top-left, for each
	 * transform. Written out as the answer rather than derived, so the test
	 * disagrees with the implementation rather than restating it.
	 *   0 = red(TL) 1 = green(TR) 2 = blue(BL) 3 = yellow(BR) */
	static const struct {
		enum avk_transform transform;
		const char *name;
		int expect_tl;
	} cases[] = {
		{ AVK_TRANSFORM_NORMAL,      "normal",      0 },
		{ AVK_TRANSFORM_90,          "90",          2 },
		{ AVK_TRANSFORM_180,         "180",         3 },
		{ AVK_TRANSFORM_270,         "270",         1 },
		{ AVK_TRANSFORM_FLIPPED,     "flipped",     1 },
		{ AVK_TRANSFORM_FLIPPED_90,  "flipped-90",  0 },
		{ AVK_TRANSFORM_FLIPPED_180, "flipped-180", 2 },
		{ AVK_TRANSFORM_FLIPPED_270, "flipped-270", 3 },
	};
	static const int quad_rgb[4][3] = {
		{ 255, 0, 0 }, { 0, 255, 0 }, { 0, 0, 255 }, { 255, 255, 0 },
	};

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		struct avk_scene scene;
		avk_scene_init(&scene);
		pixman_region32_union_rect(&scene.damage, &scene.damage, 0, 0, W, H);
		scene.has_clear = true;
		scene.clear_color[3] = 1.0f;

		struct avk_cmd *tex = avk_scene_add(&scene, AVK_CMD_TEXTURE);
		tex->dst = (struct avk_box){ 0, 0, W, H };
		tex->image = surface;
		tex->src = (struct avk_fbox){ 0, 0, 32, 32 };
		tex->transform = cases[i].transform;

		if (!render(h, &scene)) {
			CHECK(false, "%s: rendered", cases[i].name);
			avk_scene_finish(&scene);
			continue;
		}

		uint32_t p = px(h, 8, 8);
		const int *want = quad_rgb[cases[i].expect_tl];
		CHECK(r_of(p) == want[0] && g_of(p) == want[1] && b_of(p) == want[2],
			"%s puts (%d,%d,%d) top-left, got (%d,%d,%d)", cases[i].name,
			want[0], want[1], want[2], r_of(p), g_of(p), b_of(p));

		avk_scene_finish(&scene);
	}

	avk_image_destroy(h->dev, surface);
}

/* ── test 4: damage ─────────────────────────────────────────────────────── */

static void test_damage(struct harness *h) {
	printf("test 4: damage restricts what is redrawn\n");

	struct avk_scene scene;
	avk_scene_init(&scene);
	pixman_region32_union_rect(&scene.damage, &scene.damage, 0, 0, W, H);
	scene.has_clear = true;
	scene.clear_color[2] = 1.0f;   /* blue */
	scene.clear_color[3] = 1.0f;
	CHECK(render(h, &scene), "frame A: full-damage blue");
	CHECK(b_of(px(h, 32, 32)) == 255, "frame A is blue");
	avk_scene_finish(&scene);

	/*
	 * Frame B asks to paint the WHOLE target red, but damages only a 16x16
	 * box. Everything outside that box must still be frame A's blue.
	 *
	 * This is the assertion that fails if damage is ignored -- and it fails
	 * loudly, because ignoring damage turns the whole target red rather than
	 * producing a subtle artefact.
	 */
	avk_scene_init(&scene);
	pixman_region32_union_rect(&scene.damage, &scene.damage, 20, 20, 16, 16);
	scene.has_clear = true;
	scene.clear_color[0] = 1.0f;
	scene.clear_color[3] = 1.0f;
	CHECK(render(h, &scene), "frame B: red, damaged to a 16x16 box");

	CHECK(r_of(px(h, 28, 28)) == 255 && b_of(px(h, 28, 28)) == 0,
		"inside the damage is the new red");
	CHECK(b_of(px(h, 4, 4)) == 255 && r_of(px(h, 4, 4)) == 0,
		"outside the damage the old blue survives");
	CHECK(b_of(px(h, 60, 60)) == 255, "the far corner survives too");
	CHECK(r_of(px(h, 19, 28)) == 0,
		"the pixel just left of the damage box is untouched");
	CHECK(r_of(px(h, 20, 28)) == 255,
		"the first pixel inside the damage box IS touched");

	avk_scene_finish(&scene);
}

/* ── test 5: clipping to a visible region ───────────────────────────────── */

static void test_clip(struct harness *h) {
	printf("test 5: a command's visible region clips it\n");

	struct avk_scene scene;
	avk_scene_init(&scene);
	pixman_region32_union_rect(&scene.damage, &scene.damage, 0, 0, W, H);
	scene.has_clear = true;
	scene.clear_color[3] = 1.0f;   /* black */

	/* A full-target green rectangle that is only visible in its left half --
	 * exactly what occlusion produces when another window covers the right
	 * half of this one. */
	struct avk_cmd *rect = avk_scene_add(&scene, AVK_CMD_RECT);
	rect->dst = (struct avk_box){ 0, 0, W, H };
	rect->color[1] = 1.0f;
	rect->color[3] = 1.0f;

	pixman_region32_t visible;
	pixman_region32_init_rect(&visible, 0, 0, 32, H);
	avk_cmd_set_clip(rect, &visible);
	pixman_region32_fini(&visible);

	CHECK(render(h, &scene), "rendered a clipped rectangle");
	CHECK(g_of(px(h, 8, 32)) == 255, "the visible half is green");
	CHECK(g_of(px(h, 48, 32)) == 0, "the clipped half is untouched black");
	CHECK(g_of(px(h, 31, 32)) == 255, "the last visible column is drawn");
	CHECK(g_of(px(h, 32, 32)) == 0, "the first clipped column is not");

	avk_scene_finish(&scene);
}

/* ── test 6: opacity ────────────────────────────────────────────────────── */

static void test_opacity(struct harness *h) {
	printf("test 6: per-command opacity\n");

	struct avk_scene scene;
	avk_scene_init(&scene);
	pixman_region32_union_rect(&scene.damage, &scene.damage, 0, 0, W, H);
	scene.has_clear = true;
	scene.clear_color[3] = 1.0f;   /* black */

	struct avk_cmd *rect = avk_scene_add(&scene, AVK_CMD_RECT);
	rect->dst = (struct avk_box){ 0, 0, W, H };
	rect->color[0] = 1.0f;
	rect->color[3] = 1.0f;
	rect->opacity = 0.5f;

	CHECK(render(h, &scene), "rendered a 50%% opaque red over black");
	uint32_t p = px(h, 32, 32);
	CHECK(near(r_of(p), 128, 1) && g_of(p) == 0 && b_of(p) == 0,
		"50%% red over black is (128,0,0), got (%d,%d,%d)", r_of(p), g_of(p),
		b_of(p));

	avk_scene_finish(&scene);

	/*
	 * A rect whose COLOUR carries the alpha, rather than opacity carrying it.
	 *
	 * This case exists because of a break test: removing AVK's premultiply
	 * changed nothing in any other assertion here, since every other rect has
	 * alpha 1.0 and `c * 1.0` is `c`. A renderer that forgot to premultiply
	 * would have passed the whole file. Here it cannot: unpremultiplied, the
	 * shader emits (1,0,0,0.5) and the blend leaves 255; premultiplied it
	 * emits (0.5,0,0,0.5) and leaves 128.
	 */
	avk_scene_init(&scene);
	pixman_region32_union_rect(&scene.damage, &scene.damage, 0, 0, W, H);
	scene.has_clear = true;
	scene.clear_color[3] = 1.0f;   /* black */

	struct avk_cmd *translucent = avk_scene_add(&scene, AVK_CMD_RECT);
	translucent->dst = (struct avk_box){ 0, 0, W, H };
	translucent->color[0] = 1.0f;
	translucent->color[3] = 0.5f;   /* straight alpha, AVK premultiplies */

	CHECK(render(h, &scene), "rendered a rect with 50%% colour alpha");
	p = px(h, 32, 32);
	CHECK(near(r_of(p), 128, 1),
		"a half-alpha red rect over black is 128, not 255 (got %d) -- this is "
		"the premultiply", r_of(p));

	avk_scene_finish(&scene);
}

/* ── main ───────────────────────────────────────────────────────────────── */

int main(void) {
	printf("== avk composition (M3) ==\n");

	int drm_fd = -1;
	for (int i = 128; i < 192 && drm_fd < 0; i++) {
		char path[32];
		snprintf(path, sizeof(path), "/dev/dri/renderD%d", i);
		drm_fd = open(path, O_RDWR | O_CLOEXEC);
	}
	if (drm_fd < 0) {
		SKIP("no DRM render node");
	}

	struct harness h = {0};
	h.inst = avk_instance_create("test-avk-render");
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
	h.target = make_image(h.dev, W, H,
		VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
		| VK_IMAGE_USAGE_SAMPLED_BIT, true);
	if (h.target == NULL) {
		CHECK(false, "output target allocates");
		goto done;
	}

	test_overlap_alpha(&h);
	test_crop_scale(&h);
	test_transforms(&h);
	test_damage(&h);
	test_rounded_asymmetric(&h);
	test_rounded_destination_space(&h);
	test_rounded_with_transform(&h);
	test_clip(&h);
	test_opacity(&h);

	/* The claim the whole architecture rests on, stated as a number. */
	printf("instrumentation\n");
	avk_renderer_log_stats(&h.renderer);
	CHECK(h.renderer.stats.frames > 0, "%" PRIu64 " frames composited by AVK",
		h.renderer.stats.frames);

done:
	if (h.target != NULL) {
		avk_image_destroy(h.dev, h.target);
	}
	avk_device_wait_idle(h.dev);
	avk_renderer_finish(&h.renderer);
	avk_device_destroy(h.dev);
	avk_instance_destroy(h.inst);
	close(drm_fd);

	printf("\n%d checks, %d failed\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
