/*
 * M4F.2A.1: segmented composition.
 *
 * THE ONE STATEMENT THIS FILE EXISTS TO PROVE:
 *
 *   avk_render_frame can replay any exact scene prefix into an arbitrary
 *   regional target without changing the semantics of the commands it renders.
 *
 * Everything M4F builds after this depends on it, so it is proved on PIXELS.
 * The method throughout is the same and it is the only one that can fail
 * honestly: render a scene into a full-size reference, render the same commands
 * into a smaller target at a nonzero origin, and compare
 *
 *     regional[x, y]   against   reference[x + origin_x, y + origin_y]
 *
 * A struct-inspection test would pass for a renderer that translated geometry
 * and silently restarted every gradient at the transient's edge.
 *
 * THE TWO MATERIALS THAT MAKE THIS DANGEROUS. A gradient normalises within its
 * own box and the shadow dither is anchored to the output raster; both read
 * gl_FragCoord, both still produce a plausible picture when it is the wrong
 * space, and both differ ONLY where a regional target begins. They are the
 * reason az_frag_global() exists and they are tested first.
 *
 * Exits 77 (skip) with no GPU.
 */

#define _POSIX_C_SOURCE 200809L

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "render/vulkan/graph/avk_transient.h"
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
/*
 * The regional window: deliberately not at the origin and not a multiple of the
 * pool's granularity -- but EVEN, which is a real constraint rather than a
 * convenient choice. See test_odd_origin() at the bottom, which measures what an
 * odd origin costs and why.
 */
#define RX 36
#define RY 52
#define RW 128
#define RH 96
#define FMT VK_FORMAT_B8G8R8A8_UNORM

struct harness {
	struct avk_instance *inst;
	struct avk_device *dev;
	struct avk_renderer renderer;
	struct avk_image *full;      /* W x H reference target */
	struct avk_image *region;    /* RW x RH regional target */
	struct avk_image *tex;       /* a client-surface stand-in */
	uint32_t reference[W * H];
	uint32_t regional[RW * RH];
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

static struct avk_image *make_image(struct avk_device *dev, uint32_t w,
		uint32_t h, VkImageUsageFlags usage) {
	struct avk_image *image = avk_image_alloc(dev);
	if (image == NULL) {
		return NULL;
	}
	image->format = FMT;
	image->extent = (VkExtent2D){ w, h };
	image->has_alpha = true;
	image->plane_count = 1;
	image->layout = VK_IMAGE_LAYOUT_UNDEFINED;

	VkImageCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = FMT,
		.extent = { w, h, 1 },
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

static bool stage(struct harness *h, struct avk_image *image, uint32_t w,
		uint32_t h_px, const uint32_t *src, uint32_t *dst) {
	struct avk_device *dev = h->dev;
	VkDeviceSize size = (VkDeviceSize)w * h_px * 4;
	bool to_device = src != NULL;
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
	avk_cmd_ring_init(&ring, dev, "segment-stage");
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
		.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b,
	};
	vkCmdPipelineBarrier2(cb, &dep);
	image->layout = want;

	VkBufferImageCopy2 region = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
		.bufferRowLength = w, .bufferImageHeight = h_px,
		.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
		.imageExtent = { w, h_px, 1 },
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

/* ── rendering one segment ──────────────────────────────────────────────── */

static bool render_segment(struct harness *h, const struct avk_scene *scene,
		struct avk_image *target, uint32_t w, uint32_t hh,
		int32_t ox, int32_t oy, size_t begin, size_t end, bool clear) {
	struct avk_graph *g = &h->renderer.graph;
	avk_graph_reset(g);

	uint32_t rt = avk_graph_add_image(g, target, false, AVK_EXIT_KEEP);
	if (rt == AVK_GRAPH_INVALID) {
		return false;
	}
	struct avk_render_segment seg = {
		.renderer = &h->renderer,
		.scene = scene,
		.target = target,
		.width = w, .height = hh,
		.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		.origin_x = ox, .origin_y = oy,
		.begin = begin, .end = end,
		.active = NULL,      /* the whole target */
		.clear = clear,
		.load = false,
		.gradient_set = VK_NULL_HANDLE,
	};
	if (!avk_render_declare_segment(g, &seg, rt)) {
		return false;
	}
	VkCommandBuffer cb = avk_cmd_ring_begin(&h->renderer.ring);
	if (cb == VK_NULL_HANDLE) {
		return false;
	}
	avk_graph_execute(g, cb, NULL, 0);
	uint64_t v = avk_cmd_ring_submit(&h->renderer.ring, NULL, 0, NULL, 0);
	return v != 0 && avk_device_timeline_wait(h->dev, v, 4000000000ULL);
}

/* ── scenes ─────────────────────────────────────────────────────────────── */

static void scene_start(struct avk_scene *scene) {
	avk_scene_init(scene);
	pixman_region32_union_rect(&scene->damage, &scene->damage, 0, 0, W, H);
	scene->has_clear = true;
	scene->clear_color[3] = 1.0f;
}

static int r_of(uint32_t p) { return (int)((p >> 16) & 0xff); }
static int g_of(uint32_t p) { return (int)((p >> 8) & 0xff); }
static int b_of(uint32_t p) { return (int)(p & 0xff); }

/*
 * Compare the regional readback against the matching window of the reference.
 * Returns the number of differing pixels and reports the first.
 */
static int compare(struct harness *h, const char *what) {
	int diff = 0, worst = 0;
	int fx = -1, fy = -1;
	for (uint32_t y = 0; y < RH; y++) {
		for (uint32_t x = 0; x < RW; x++) {
			uint32_t a = h->regional[y * RW + x];
			uint32_t b = h->reference[(y + RY) * W + (x + RX)];
			if (a == b) {
				continue;
			}
			int d = abs(r_of(a) - r_of(b));
			int dg = abs(g_of(a) - g_of(b));
			int db = abs(b_of(a) - b_of(b));
			if (dg > d) { d = dg; }
			if (db > d) { d = db; }
			if (d > worst) { worst = d; }
			if (fx < 0) { fx = (int)x; fy = (int)y; }
			diff++;
		}
	}
	if (diff > 0) {
		printf("  ---- %s: %d of %d pixels differ, worst %d codes, first at "
			"regional %d,%d = global %d,%d\n", what, diff, RW * RH, worst,
			fx, fy, fx + RX, fy + RY);
		/* Are they on edges? A pixel whose neighbours differ from it by a lot
		 * in the REFERENCE is on a transition; one in a flat area is not. */
		int on_edge = 0;
		for (uint32_t y = 1; y < RH - 1; y++) {
			for (uint32_t x = 1; x < RW - 1; x++) {
				if (h->regional[y * RW + x]
						== h->reference[(y + RY) * W + (x + RX)]) {
					continue;
				}
				uint32_t c = h->reference[(y + RY) * W + (x + RX)];
				uint32_t l = h->reference[(y + RY) * W + (x + RX - 1)];
				uint32_t r = h->reference[(y + RY) * W + (x + RX + 1)];
				if (abs(g_of(c) - g_of(l)) > 8 || abs(g_of(c) - g_of(r)) > 8
						|| abs(r_of(c) - r_of(l)) > 8
						|| abs(r_of(c) - r_of(r)) > 8) {
					on_edge++;
				}
			}
		}
		printf("  ---- of those, %d sit on a horizontal transition in the "
			"reference (antialiasing band)\n", on_edge);
	}
	return diff;
}

/* Render a scene both ways and compare. */
static int both_ways(struct harness *h, struct avk_scene *scene,
		const char *what) {
	if (!render_segment(h, scene, h->full, W, H, 0, 0, 0, scene->len, true)) {
		return -1;
	}
	if (!stage(h, h->full, W, H, NULL, h->reference)) {
		return -1;
	}
	if (!render_segment(h, scene, h->region, RW, RH, RX, RY, 0, scene->len,
			true)) {
		return -1;
	}
	if (!stage(h, h->region, RW, RH, NULL, h->regional)) {
		return -1;
	}
	return compare(h, what);
}

/* ── 1. the two dangerous materials ─────────────────────────────────────── */
static void test_gradient(struct harness *h) {
	printf("\n-- gradient --\n");

	/*
	 * A gradient normalises within its own box. If a regional target's
	 * gl_FragCoord were used directly, the ramp would RESTART at the
	 * transient's edge -- and a restarted gradient is still a gradient, so a
	 * fixture that only checked "is there a ramp" would pass.
	 *
	 * The box deliberately straddles the regional window on both axes, so the
	 * comparison covers ramp values from well before RX to well after RX+RW.
	 */
	struct avk_scene scene;
	scene_start(&scene);
	struct avk_cmd *c = avk_scene_add(&scene, AVK_CMD_RECT);
	c->dst = (struct avk_box){ 8, 8, W - 16, H - 16 };
	c->opacity = 1.0f;
	c->color[3] = 1.0f;
	float colors[8] = { 1, 0, 0, 1,  0, 0, 1, 1 };
	float origin[2] = { 0.5f, 0.5f };
	CHECK(avk_cmd_set_gradient(&scene, c, AVK_GRADIENT_LINEAR, 37.0f, true,
		origin, colors, 2), "a 37-degree two-stop gradient is set");

	int diff = both_ways(h, &scene, "gradient");
	CHECK(diff == 0,
		"a gradient renders identically into a regional target -- the ramp "
		"does not restart at the origin (%d differing pixels)", diff);
	avk_scene_finish(&scene);
}

static void test_shadow_dither(struct harness *h) {
	printf("\n-- shadow, and its output-anchored dither --\n");

	/*
	 * M4D's dither is a function of the OUTPUT pixel and nothing else -- that
	 * is what makes a still desktop bit-identical frame to frame. Rendered into
	 * a regional target with local coordinates it would keep its texture and
	 * shift its PHASE, which is invisible in isolation and shows as a seam
	 * exactly at the transient's edge.
	 */
	struct avk_scene scene;
	scene_start(&scene);
	struct avk_cmd *c = avk_scene_add(&scene, AVK_CMD_SHADOW);
	c->dst = (struct avk_box){ 20, 20, 200, 200 };
	c->opacity = 1.0f;
	c->color[3] = 0.5f;
	c->blur_sigma = 24.0f;
	c->corners[0] = 0.0f; c->corners[1] = 7.0f;
	c->corners[2] = 19.0f; c->corners[3] = 37.0f;
	c->inner = (struct avk_box){ 44, 44, 152, 152 };
	c->has_inner = true;

	int diff = both_ways(h, &scene, "shadow+dither");
	CHECK(diff == 0,
		"a dithered shadow renders identically -- no dither phase reset at the "
		"regional origin, and per-corner radii 0/7/19/37 are preserved (%d "
		"differing pixels)", diff);
	avk_scene_finish(&scene);
}

/* ── 2. rounded corners, borders, textures ──────────────────────────────── */
static void test_rounded_and_border(struct harness *h) {
	printf("\n-- rounded corners and an annulus --\n");

	struct avk_scene scene;
	scene_start(&scene);
	/* M4A's asymmetric case: four different radii, so a single-radius or
	 * bottom-swapped implementation differs. */
	struct avk_cmd *c = avk_scene_add(&scene, AVK_CMD_RECT);
	c->dst = (struct avk_box){ 24, 40, 180, 150 };
	c->opacity = 1.0f;
	c->color[0] = 0.2f; c->color[1] = 0.8f; c->color[2] = 0.4f;
	c->color[3] = 1.0f;
	c->corners[0] = 0.0f; c->corners[1] = 7.0f;
	c->corners[2] = 19.0f; c->corners[3] = 37.0f;

	/* M4B's border: radius 40 outer, width 6, which is the case whose inner
	 * arc used to leave a wedge of background showing. */
	struct avk_cmd *b = avk_scene_add(&scene, AVK_CMD_RECT);
	b->dst = (struct avk_box){ 60, 90, 160, 140 };
	b->opacity = 1.0f;
	b->color[0] = 1.0f; b->color[3] = 1.0f;
	for (int i = 0; i < 4; i++) { b->corners[i] = 40.0f; }
	b->inner = (struct avk_box){ 66, 96, 148, 128 };
	for (int i = 0; i < 4; i++) { b->inner_corners[i] = 33.0f; }
	b->has_inner = true;

	int diff = both_ways(h, &scene, "rounded+border");
	CHECK(diff == 0,
		"per-corner radii and a radius-40/border-6 annulus render identically "
		"(%d differing pixels)", diff);
	avk_scene_finish(&scene);
}

static void test_texture(struct harness *h) {
	printf("\n-- client texture --\n");

	/* A high-frequency source: a translated destination must not change which
	 * source texel lands where. A uniform texture could not tell. */
	static uint32_t px[W * H];
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			bool on = ((x / 3) + (y / 5)) & 1;
			px[y * W + x] = 0xff000000u | (on ? 0x00ffcc00u : 0x00110044u);
		}
	}
	CHECK(stage(h, h->tex, W, H, px, NULL), "the source texture uploads");
	/*
	 * The layout is left exactly as stage() left it -- TRANSFER_DST_OPTIMAL --
	 * and the graph transitions it. Asserting SHADER_READ_ONLY_OPTIMAL here was
	 * a LIE about the image's real state: the graph then saw an entry layout
	 * that already matched, emitted no transition, and validation reported
	 * VUID-vkCmdDraw-None-09600 at submit. The image's layout field is the
	 * cross-frame source of truth and nothing may write it except whatever
	 * actually transitioned the image.
	 */

	struct avk_scene scene;
	scene_start(&scene);
	struct avk_cmd *c = avk_scene_add(&scene, AVK_CMD_TEXTURE);
	/* A source CROP and a non-integer destination scale, so the sampling
	 * mapping is not the identity. */
	c->dst = (struct avk_box){ 10, 30, 210, 170 };
	c->image = h->tex;
	c->src = (struct avk_fbox){ 12.5, 8.25, 180.0, 140.0 };
	c->opacity = 1.0f;
	c->filter_linear = true;

	int diff = both_ways(h, &scene, "texture");
	CHECK(diff == 0,
		"a cropped, scaled client texture samples identically regardless of "
		"target origin (%d differing pixels)", diff);
	avk_scene_finish(&scene);
}

/* ── 3. range semantics ─────────────────────────────────────────────────── */
static void test_range(struct harness *h) {
	printf("\n-- [begin, end) --\n");

	/*
	 * Four opaque rects in four disjoint quadrants of the regional window, so
	 * each command's contribution is separable by inspection: whichever
	 * quadrant is black was excluded from the range.
	 */
	struct avk_scene scene;
	scene_start(&scene);
	struct { int x, y; float r, g, b; } quads[4] = {
		{ RX + 8,  RY + 8,  1.0f, 0.0f, 0.0f },
		{ RX + 72, RY + 8,  0.0f, 1.0f, 0.0f },
		{ RX + 8,  RY + 56, 0.0f, 0.0f, 1.0f },
		{ RX + 72, RY + 56, 1.0f, 1.0f, 0.0f },
	};
	for (int i = 0; i < 4; i++) {
		struct avk_cmd *c = avk_scene_add(&scene, AVK_CMD_RECT);
		c->dst = (struct avk_box){ quads[i].x, quads[i].y, 40, 32 };
		c->opacity = 1.0f;
		c->color[0] = quads[i].r; c->color[1] = quads[i].g;
		c->color[2] = quads[i].b; c->color[3] = 1.0f;
	}

	struct { size_t begin, end; const char *name; int present[4]; } cases[] = {
		{ 0, 2, "[0,2)", { 1, 1, 0, 0 } },
		{ 2, 4, "[2,4)", { 0, 0, 1, 1 } },
		{ 1, 3, "[1,3)", { 0, 1, 1, 0 } },
	};
	for (size_t k = 0; k < 3; k++) {
		if (!render_segment(h, &scene, h->region, RW, RH, RX, RY,
				cases[k].begin, cases[k].end, true)
				|| !stage(h, h->region, RW, RH, NULL, h->regional)) {
			continue;
		}
		int wrong = 0;
		/* A point outside every quadrant: if the clear ran, it is black. */
		uint32_t bg = h->regional[(RH - 4) * RW + (RW - 4)];
		printf("  ---- %s bg=(%d,%d,%d) quadrants:", cases[k].name,
			r_of(bg), g_of(bg), b_of(bg));
		for (int i = 0; i < 4; i++) {
			/* The quadrant's centre, in regional coordinates. */
			int x = quads[i].x - RX + 20, y = quads[i].y - RY + 16;
			uint32_t p = h->regional[y * RW + x];
			bool lit = r_of(p) > 8 || g_of(p) > 8 || b_of(p) > 8;
			printf(" q%d=(%d,%d,%d)%s", i, r_of(p), g_of(p), b_of(p),
				lit == (cases[k].present[i] != 0) ? "" : " WRONG");
			if (lit != (cases[k].present[i] != 0)) {
				wrong++;
			}
		}
		printf("\n");
		CHECK(wrong == 0,
			"%s renders exactly commands %zu..%zu and nothing else (%d "
			"quadrants wrong)", cases[k].name, cases[k].begin,
			cases[k].end - 1, wrong);
	}
	avk_scene_finish(&scene);
}

/* ── 4. order is semantic ───────────────────────────────────────────────── */
static void test_order(struct harness *h) {
	printf("\n-- order --\n");

	/* Two overlapping TRANSLUCENT rects: red over blue and blue over red give
	 * different results, so any reordering shows. */
	for (int swap = 0; swap < 2; swap++) {
		struct avk_scene scene;
		scene_start(&scene);
		for (int i = 0; i < 2; i++) {
			struct avk_cmd *c = avk_scene_add(&scene, AVK_CMD_RECT);
			c->dst = (struct avk_box){ RX + 20 + i * 20, RY + 20, 60, 50 };
			c->opacity = 1.0f;
			bool red = (i == 0) != (swap != 0);
			c->color[0] = red ? 1.0f : 0.0f;
			c->color[2] = red ? 0.0f : 1.0f;
			c->color[3] = 0.6f;
		}
		if (!render_segment(h, &scene, h->region, RW, RH, RX, RY, 0,
				scene.len, true)
				|| !stage(h, h->region, RW, RH, NULL, h->regional)) {
			avk_scene_finish(&scene);
			continue;
		}
		/* In the overlap, whichever was drawn LAST dominates. */
		uint32_t p = h->regional[45 * RW + 50];
		if (swap == 0) {
			CHECK(b_of(p) > r_of(p),
				"drawn second, blue dominates the overlap (%d,%d,%d)",
				r_of(p), g_of(p), b_of(p));
		} else {
			CHECK(r_of(p) > b_of(p),
				"with the order swapped, red does (%d,%d,%d)",
				r_of(p), g_of(p), b_of(p));
		}
		avk_scene_finish(&scene);
	}
}

/* ── 5. a scissor smaller than the target ───────────────────────────────── */
static void test_active_region(struct harness *h) {
	printf("\n-- active region --\n");

	struct avk_scene scene;
	scene_start(&scene);
	struct avk_cmd *c = avk_scene_add(&scene, AVK_CMD_RECT);
	c->dst = (struct avk_box){ RX, RY, RW, RH };
	c->opacity = 1.0f;
	c->color[1] = 1.0f; c->color[3] = 1.0f;

	/* Clear the target to a known value first, then render with an active
	 * region covering only the left half in SCENE coordinates. */
	struct avk_scene blank;
	scene_start(&blank);
	render_segment(h, &blank, h->region, RW, RH, RX, RY, 0, 0, true);
	avk_scene_finish(&blank);

	struct avk_graph *g = &h->renderer.graph;
	avk_graph_reset(g);
	uint32_t rt = avk_graph_add_image(g, h->region, false, AVK_EXIT_KEEP);
	pixman_region32_t active;
	pixman_region32_init_rect(&active, RX, RY, RW / 2, RH);
	struct avk_render_segment seg = {
		.renderer = &h->renderer, .scene = &scene, .target = h->region,
		.width = RW, .height = RH,
		.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		.origin_x = RX, .origin_y = RY,
		.begin = 0, .end = scene.len,
		.active = &active,
		.clear = true, .load = true,
	};
	bool ok = avk_render_declare_segment(g, &seg, rt);
	VkCommandBuffer cb = avk_cmd_ring_begin(&h->renderer.ring);
	avk_graph_execute(g, cb, NULL, 0);
	uint64_t v = avk_cmd_ring_submit(&h->renderer.ring, NULL, 0, NULL, 0);
	ok = ok && v != 0 && avk_device_timeline_wait(h->dev, v, 4000000000ULL)
		&& stage(h, h->region, RW, RH, NULL, h->regional);
	pixman_region32_fini(&active);
	CHECK(ok, "the scissored segment renders");

	uint32_t left = h->regional[(RH / 2) * RW + RW / 4];
	uint32_t right = h->regional[(RH / 2) * RW + RW * 3 / 4];
	CHECK(g_of(left) > 200, "inside the active region the rect drew (%d)",
		g_of(left));
	CHECK(g_of(right) < 40,
		"outside it nothing was touched, so a regional target does NOT imply "
		"'render every pixel of the allocation' (%d)", g_of(right));
	avk_scene_finish(&scene);
}

/* ── 6. everything at once ──────────────────────────────────────────────── */
static void test_mixed(struct harness *h) {
	printf("\n-- all materials in one scene --\n");

	struct avk_scene scene;
	scene_start(&scene);

	struct avk_cmd *bg = avk_scene_add(&scene, AVK_CMD_RECT);
	bg->dst = (struct avk_box){ 0, 0, W, H };
	bg->opacity = 1.0f;
	float colors[12] = { 0.9f, 0.1f, 0.1f, 1,  0.1f, 0.9f, 0.1f, 1,
		0.1f, 0.1f, 0.9f, 1 };
	float origin[2] = { 0.35f, 0.65f };
	bg->color[3] = 1.0f;
	avk_cmd_set_gradient(&scene, bg, AVK_GRADIENT_LINEAR, 113.0f, true,
		origin, colors, 3);

	struct avk_cmd *sh = avk_scene_add(&scene, AVK_CMD_SHADOW);
	sh->dst = (struct avk_box){ 30, 30, 170, 170 };
	sh->opacity = 1.0f;
	sh->color[3] = 0.4f;
	sh->blur_sigma = 20.0f;
	for (int i = 0; i < 4; i++) { sh->corners[i] = 12.0f; }

	struct avk_cmd *tx = avk_scene_add(&scene, AVK_CMD_TEXTURE);
	tx->dst = (struct avk_box){ 50, 60, 120, 100 };
	tx->image = h->tex;
	tx->src = (struct avk_fbox){ 0, 0, W, H };
	tx->opacity = 0.85f;
	tx->filter_linear = true;
	for (int i = 0; i < 4; i++) { tx->corners[i] = 9.0f; }

	int diff = both_ways(h, &scene, "mixed");
	CHECK(diff == 0,
		"a gradient, a dithered shadow and a rounded translucent texture all "
		"render identically into a regional target (%d differing pixels)",
		diff);
	avk_scene_finish(&scene);
}

/* ── 7. why the origin must be even ─────────────────────────────────────── */
static void test_odd_origin(struct harness *h) {
	printf("\n-- odd origin --\n");

	/*
	 * The constraint, measured rather than asserted.
	 *
	 * rounded.glsl antialiases with fwidth(dist) -- a 2x2 QUAD derivative whose
	 * grid is aligned to the ATTACHMENT's pixels. An odd origin shifts every
	 * derivative quad one pixel relative to the output, so an edge's AA band is
	 * computed over a different neighbourhood and comes out a few codes
	 * different.
	 *
	 * This renders the border scene at origin RX+1, RY+1 and reports the
	 * difference. It is not a failure -- it is the reason
	 * avk_render_segment_align_origin() exists, and a regression here would mean
	 * the constraint had changed.
	 */
	struct avk_scene scene;
	scene_start(&scene);
	struct avk_cmd *b = avk_scene_add(&scene, AVK_CMD_RECT);
	b->dst = (struct avk_box){ 60, 90, 160, 140 };
	b->opacity = 1.0f;
	b->color[0] = 1.0f; b->color[3] = 1.0f;
	for (int i = 0; i < 4; i++) { b->corners[i] = 40.0f; }
	b->inner = (struct avk_box){ 66, 96, 148, 128 };
	for (int i = 0; i < 4; i++) { b->inner_corners[i] = 33.0f; }
	b->has_inner = true;

	if (!render_segment(h, &scene, h->full, W, H, 0, 0, 0, scene.len, true)
			|| !stage(h, h->full, W, H, NULL, h->reference)) {
		avk_scene_finish(&scene);
		return;
	}
	/* ODD in both axes, one pixel off the aligned window. */
	if (!render_segment(h, &scene, h->region, RW, RH, RX + 1, RY + 1, 0,
			scene.len, true)
			|| !stage(h, h->region, RW, RH, NULL, h->regional)) {
		avk_scene_finish(&scene);
		return;
	}
	int diff = 0, worst = 0;
	for (uint32_t y = 0; y < RH; y++) {
		for (uint32_t x = 0; x < RW; x++) {
			uint32_t a = h->regional[y * RW + x];
			uint32_t c = h->reference[(y + RY + 1) * W + (x + RX + 1)];
			if (a == c) {
				continue;
			}
			int d = abs(r_of(a) - r_of(c));
			if (d > worst) { worst = d; }
			diff++;
		}
	}
	printf("  ---- origin %d,%d (odd): %d of %d pixels differ, worst %d codes\n",
		RX + 1, RY + 1, diff, RW * RH, worst);
	CHECK(diff > 0,
		"an odd origin DOES perturb the antialiasing band, which is why "
		"avk_render_segment_align_origin() exists (%d pixels)", diff);
	CHECK(worst <= 8,
		"and only the band -- the difference is a few codes, not a wrong "
		"picture (%d codes)", worst);

	int32_t ox = RX + 1, oy = RY + 1;
	uint32_t ow = RW, oh = RH;
	avk_render_segment_align_origin(&ox, &oy, &ow, &oh);
	CHECK(ox == RX && oy == RY && ow == RW + 1 && oh == RH + 1,
		"and the aligner rounds %d,%d down to %d,%d, growing the extent to "
		"keep the requested area covered", RX + 1, RY + 1, ox, oy);
	avk_scene_finish(&scene);
}

/* ── 8. the alignment contract, at every origin parity ──────────────────── */
static void test_alignment_matrix(struct harness *h) {
	printf("\n-- alignment parity matrix --\n");

	/*
	 * ── IN WHICH SPACE MUST THE ORIGIN BE EVEN? ───────────────────────────
	 *
	 * In the ATTACHMENT's, and that answer is forced rather than chosen. The
	 * constraint exists because rounded.glsl antialiases with fwidth(), a 2x2
	 * QUAD derivative, and the quad grid is a property of the framebuffer being
	 * rendered into. Nothing upstream of the attachment has derivative quads to
	 * misalign.
	 *
	 * That also settles the question of whether alignment happens before or
	 * after the output transform, which M4F.2C.4c left open. There is no
	 * "before": AVK never sees an output transform. The compositor applies it in
	 * az_avk_box_to_output() while emitting commands, so every box the renderer
	 * is handed -- and therefore every capture region derived from one -- is
	 * already in attachment pixels. A capture origin has exactly one space to
	 * be even in, and a rotation cannot move it to another one.
	 *
	 * What is asserted here is the property that makes that safe: whatever
	 * parity a caller asks for, the ALIGNED region renders bit-identically to
	 * the matching window of a full render, and still covers everything that
	 * was asked for.
	 */
	struct avk_scene scene;
	scene_start(&scene);
	struct avk_cmd *b = avk_scene_add(&scene, AVK_CMD_RECT);
	b->dst = (struct avk_box){ 60, 90, 160, 140 };
	b->opacity = 1.0f;
	b->color[0] = 1.0f; b->color[3] = 1.0f;
	/* Asymmetric radii, so a permutation is visible as well as a phase shift. */
	b->corners[0] = 0.0f;  b->corners[1] = 7.0f;
	b->corners[2] = 19.0f; b->corners[3] = 37.0f;
	b->inner = (struct avk_box){ 66, 96, 148, 128 };
	for (int i = 0; i < 4; i++) { b->inner_corners[i] = 33.0f; }
	b->has_inner = true;

	if (!render_segment(h, &scene, h->full, W, H, 0, 0, 0, scene.len, true)
			|| !stage(h, h->full, W, H, NULL, h->reference)) {
		avk_scene_finish(&scene);
		return;
	}

	static const struct { int dx, dy; const char *what; } PARITY[] = {
		{ 0, 0, "even/even" },
		{ 0, 1, "even/odd"  },
		{ 1, 0, "odd/even"  },
		{ 1, 1, "odd/odd"   },
	};
	for (size_t i = 0; i < sizeof(PARITY) / sizeof(PARITY[0]); i++) {
		int32_t want_x = RX + PARITY[i].dx, want_y = RY + PARITY[i].dy;
		/* ONE PIXEL SHORT OF THE REGIONAL TARGET, deliberately: aligning an odd
		 * origin GROWS the extent by one per axis, and asking for the full
		 * RWxRH would make the aligned request exceed both the regional image
		 * and the staging array. The first version of this test did exactly
		 * that and reported 128 differing pixels at worst 255 -- which was its
		 * own buffer overflow, and glibc said so. */
		uint32_t want_w = RW - 1, want_h = RH - 1;
		int32_t ox = want_x, oy = want_y;
		uint32_t ow = want_w, oh = want_h;
		avk_render_segment_align_origin(&ox, &oy, &ow, &oh);

		CHECK((ox & 1) == 0 && (oy & 1) == 0,
			"%s: the aligned origin is even in both axes (%d,%d)",
			PARITY[i].what, ox, oy);
		/* CONTAINMENT. Alignment may enlarge the backing capture; it may never
		 * lose a pixel the caller asked for, which is the same requirement the
		 * blur's dependency has of its capture. */
		CHECK(ox <= want_x && oy <= want_y
				&& (int64_t)ox + ow >= (int64_t)want_x + want_w
				&& (int64_t)oy + oh >= (int64_t)want_y + want_h,
			"%s: and still covers the requested window (%d,%d %ux%u contains "
			"%d,%d %ux%u)", PARITY[i].what, ox, oy, ow, oh, want_x, want_y,
			want_w, want_h);

		if (ow > RW + 1 || oh > RH + 1) {
			/* The regional image is allocated at RW+1 x RH+1; a larger request
			 * would be a test bug rather than a finding. */
			CHECK(false, "%s: aligned extent %ux%u exceeds the test target",
				PARITY[i].what, ow, oh);
			continue;
		}
		if (!render_segment(h, &scene, h->region, ow, oh, ox, oy, 0, scene.len,
				true) || !stage(h, h->region, ow, oh, NULL, h->regional)) {
			continue;
		}
		int diff = 0, worst = 0;
		for (uint32_t y = 0; y < oh; y++) {
			for (uint32_t x = 0; x < ow; x++) {
				if ((int32_t)x + ox >= (int32_t)W
						|| (int32_t)y + oy >= (int32_t)H) {
					continue;
				}
				uint32_t a = h->regional[y * ow + x];
				uint32_t c = h->reference[(y + oy) * W + (x + ox)];
				if (a == c) {
					continue;
				}
				int d = abs(r_of(a) - r_of(c));
				if (d > worst) { worst = d; }
				diff++;
			}
		}
		CHECK(diff == 0,
			"%s: the aligned regional render is bit-identical to the full one "
			"(%d differing, worst %d)", PARITY[i].what, diff, worst);
	}
	avk_scene_finish(&scene);
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	printf("== avk segmented compose (M4F.2A.1) ==\n");
	printf("  ---- regional window: %dx%d at %d,%d of %dx%d\n",
		RW, RH, RX, RY, W, H);

	struct harness h;
	memset(&h, 0, sizeof(h));
	h.inst = avk_instance_create("avk-segment-test");
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
	h.full = make_image(h.dev, W, H, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
		| VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
	h.region = make_image(h.dev, RW, RH, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
		| VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
	h.tex = make_image(h.dev, W, H, VK_IMAGE_USAGE_SAMPLED_BIT
		| VK_IMAGE_USAGE_TRANSFER_DST_BIT);
	if (h.full == NULL || h.region == NULL || h.tex == NULL) {
		SKIP("no images");
	}

	test_gradient(&h);
	test_shadow_dither(&h);
	test_rounded_and_border(&h);
	test_texture(&h);
	test_range(&h);
	test_order(&h);
	test_active_region(&h);
	test_mixed(&h);
	test_odd_origin(&h);
	test_alignment_matrix(&h);

	avk_device_wait_idle(h.dev);
	avk_image_destroy(h.dev, h.full);
	avk_image_destroy(h.dev, h.region);
	avk_image_destroy(h.dev, h.tex);
	avk_renderer_finish(&h.renderer);
	avk_device_destroy(h.dev);
	avk_instance_destroy(h.inst);

	printf("\n---- %d/%d checks passed\n", checks - failures, checks);
	return failures == 0 ? 0 : 1;
}
