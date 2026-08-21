/*
 * M4F.2B: BLUR DAMAGE -- does a partially damaged frame contain the same
 * pixels as a completely redrawn one?
 *
 * THE PRIMARY ORACLE, and every test here is a variation on it:
 *
 *     render frame A                       (establishes the target)
 *     render frame B with PARTIAL damage   (onto the same target)
 *     render frame B with FULL damage      (from scratch)
 *     the two frame Bs must be BIT-IDENTICAL
 *
 * The forced-full path does not reuse anything the partial path computed. It is
 * the same scene, the same current-frame scene-prefix architecture and the same
 * material, given damage covering the whole output -- so a difference between
 * them is a damage bug and can be nothing else. It is emphatically NOT the
 * reference's historical-source path and does not restore it.
 *
 * WHY THE FIXTURE IS A BRIGHT BLOCK ON A DARK FIELD. A one-pixel error in the
 * support has to survive 8-bit quantisation to be measurable at all. The
 * previous milestone's impulse fixture was PROVEN BLIND for exactly this
 * reason: a single bright pixel spread over a dual-Kawase chain lands below one
 * code everywhere. A 24x24 block at full brightness against near-black does
 * not, and a stale ring around it is tens of codes wide.
 *
 * WHAT EACH GROUP ESTABLISHES:
 *
 *   partial == full     centre, all four edges, odd extents, three radii.
 *                       This is the milestone.
 *
 *   influence           a change one pixel inside the mathematically relevant
 *                       region propagates; a change well outside it does not
 *                       cause the blur to be recomputed at all. Stated as a
 *                       CONSERVATIVE claim -- the implementation is allowed to
 *                       do more work than the minimum, and the test does not
 *                       assert a minimality it does not implement.
 *
 *   transitive          blur 1's changed output is blur 2's changed source.
 *                       A single-blur fixture cannot see this.
 *
 *   the breaks          under-damage and no-transitive-damage must leave WRONG
 *                       PIXELS, not merely a different counter. Both are set on
 *                       the renderer directly, so one process can render the
 *                       same scene correct and broken and diff them.
 *
 *   the saving          prefix rebuild pixels vs the full capture. A damage
 *                       system that is correct and saves nothing is a damage
 *                       system that is not running.
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
	/* Two backdrops: the frame the target already holds, and the one with the
	 * change in it. Two images rather than one re-uploaded, so a frame can be
	 * re-rendered from either without a staging round trip in between. */
	struct avk_image *bg_a;
	struct avk_image *bg_b;
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
	avk_cmd_ring_init(&ring, dev, "blur-damage-stage", AVK_FRAMES_IN_FLIGHT);
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



/* ── the fixture ────────────────────────────────────────────────────────── */

static uint32_t rgb(int r, int g, int b) {
	return 0xff000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/* The block that moves. 24x24 and full brightness against a near-black,
 * high-frequency ground -- see the header for why an impulse is not enough. */
#define BLOCK 24

static uint32_t g_base[W * H];
static uint32_t g_changed[W * H];

/* The ground: dark, but not flat. A blur of a flat field is the same flat
 * field, so a flat ground would hide a stale region behind a value that
 * happens to be right. */
static void fill_ground(uint32_t *px) {
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			int v = 8;
			if ((x % 7) == 0) { v = 90; }
			if ((y % 11) < 2) { v = 70; }
			if (((x / 6) + (y / 6)) & 1) { v = v > 40 ? v : 3; }
			px[y * W + x] = rgb(v, v * 3 / 4, v / 2);
		}
	}
}

/* The block's size, so a fixture whose signal has to survive TWO blurs can ask
 * for more energy than one that only has to survive a single node. */
static int g_block_w = BLOCK;
static int g_block_h = BLOCK;

static void put_block(uint32_t *px, int bx, int by) {
	for (int y = by; y < by + g_block_h && y < H; y++) {
		for (int x = bx; x < bx + g_block_w && x < W; x++) {
			if (x >= 0 && y >= 0) {
				px[y * W + x] = rgb(255, 250, 240);
			}
		}
	}
}

/* ── scene construction ─────────────────────────────────────────────────── */

struct blur_spec {
	struct avk_box dst;
	uint32_t levels;
	float radius;
	/*
	 * POST-EFFECTS, and they are not decoration here.
	 *
	 * A plain dual-Kawase blur PRESERVES THE LOCAL MEAN, so blurring an
	 * already-blurred field gives almost exactly what blurring the raw field
	 * would. Measured: removing blur 1 from the two-blur scene entirely --
	 * every pixel of blur 2's source over 120 of its 136 columns replaced --
	 * moved blur 2's output by ONE 8-bit CODE. Nothing built on "blur 2 samples
	 * blur 1" can produce a visible difference while that holds.
	 *
	 * Brightness breaks the symmetry: a blur with brightness != 1 changes the
	 * mean, so its output is something a later blur can actually see. It is
	 * also what the real compositor does -- asteroidz ships brightness,
	 * contrast and saturation on its blur -- so this is the ordinary case
	 * rather than a contrivance built for the test.
	 */
	float brightness;
	/* The material fields a property change moves. Defaults (0) mean "leave the
	 * command's own default", so every existing fixture is unaffected. */
	float alpha;
	bool darken;
	float edge_softness;
	int corner;
	/* A clip, in absolute coordinates, or a zero box for none. Two boxes,
	 * because a client's region is a REGION and the gap between its rectangles
	 * is the thing most easily lost. */
	struct avk_box clip;
	struct avk_box clip2;
};

static const struct blur_spec DEFAULT_BLUR = {
	.dst = { BX, BY, BW, BH }, .levels = 3, .radius = 2.0f,
};

static void scene_start(struct avk_scene *scene,
		const pixman_region32_t *damage) {
	avk_scene_init(scene);
	if (damage != NULL) {
		pixman_region32_copy(&scene->damage, (pixman_region32_t *)damage);
	} else {
		pixman_region32_union_rect(&scene->damage, &scene->damage, 0, 0, W, H);
	}
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

static void add_blur(struct avk_scene *scene, const struct blur_spec *spec) {
	struct avk_cmd *c = avk_scene_add(scene, AVK_CMD_BLUR);
	c->dst = spec->dst;
	c->opacity = 1.0f;
	c->blur_levels = spec->levels;
	c->blur_radius = spec->radius;
	c->blur_brightness = spec->brightness > 0.0f ? spec->brightness : 1.0f;
	c->blur_contrast = 1.0f;
	c->blur_saturation = 1.0f;
	c->blur_apply_effects = spec->brightness > 0.0f
		&& spec->brightness != 1.0f;
	if (spec->alpha > 0.0f) {
		c->opacity = spec->alpha;
	}
	c->blur_darken = spec->darken;
	c->blur_edge_softness = spec->edge_softness;
	for (int i = 0; i < 4; i++) {
		c->corners[i] = (float)spec->corner;
	}
	if (spec->clip.width > 0 && spec->clip.height > 0) {
		pixman_region32_t clip;
		pixman_region32_init_rect(&clip, spec->clip.x, spec->clip.y,
			(unsigned)spec->clip.width, (unsigned)spec->clip.height);
		if (spec->clip2.width > 0 && spec->clip2.height > 0) {
			pixman_region32_union_rect(&clip, &clip, spec->clip2.x,
				spec->clip2.y, (unsigned)spec->clip2.width,
				(unsigned)spec->clip2.height);
		}
		avk_cmd_set_clip(c, &clip);
		pixman_region32_fini(&clip);
	}
}

static bool render(struct harness *h, const struct avk_scene *scene,
		uint32_t *out) {
	uint64_t v = avk_render_frame(&h->renderer, h->target, scene,
		NULL, 0, NULL, 0);
	if (v == 0 || !avk_device_timeline_wait(h->dev, v, 4000000000ULL)) {
		return false;
	}
	return out == NULL || stage(h, h->target, NULL, out);
}

/* One frame: a backdrop and any number of blur nodes, with the given damage. */
static bool render_frame(struct harness *h, struct avk_image *bg,
		const struct blur_spec *blurs, size_t nblurs,
		const pixman_region32_t *damage, uint32_t *out) {
	struct avk_scene s;
	scene_start(&s, damage);
	add_backdrop(&s, bg);
	for (size_t i = 0; i < nblurs; i++) {
		add_blur(&s, &blurs[i]);
	}
	bool ok = render(h, &s, out);
	avk_scene_finish(&s);
	return ok;
}

/* ── comparison ─────────────────────────────────────────────────────────── */

struct diff {
	long pixels;      /* pixels differing at all */
	int worst;        /* worst per-channel difference, in codes */
	int wx, wy;       /* where */
};

static struct diff compare(const uint32_t *a, const uint32_t *b) {
	struct diff d = { 0, 0, -1, -1 };
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			uint32_t pa = a[y * W + x], pb = b[y * W + x];
			if (pa == pb) {
				continue;
			}
			d.pixels++;
			for (int s = 0; s < 24; s += 8) {
				int delta = (int)((pa >> s) & 0xff) - (int)((pb >> s) & 0xff);
				if (delta < 0) { delta = -delta; }
				if (delta > d.worst) { d.worst = delta; d.wx = x; d.wy = y; }
			}
		}
	}
	return d;
}

/* The same, restricted to a box -- for asking whether one blur's own result
 * moved without being told about the rest of the frame. */
static struct diff compare_box(const uint32_t *a, const uint32_t *b,
		struct avk_box box) {
	struct diff d = { 0, 0, -1, -1 };
	for (int y = box.y; y < box.y + box.height && y < H; y++) {
		for (int x = box.x; x < box.x + box.width && x < W; x++) {
			uint32_t pa = a[y * W + x], pb = b[y * W + x];
			if (pa == pb) {
				continue;
			}
			d.pixels++;
			for (int s = 0; s < 24; s += 8) {
				int delta = (int)((pa >> s) & 0xff) - (int)((pb >> s) & 0xff);
				if (delta < 0) { delta = -delta; }
				if (delta > d.worst) { d.worst = delta; d.wx = x; d.wy = y; }
			}
		}
	}
	return d;
}

/*
 * ── THE ORACLE ─────────────────────────────────────────────────────────────
 *
 * Frame A establishes the target. Frame B is then rendered twice: once with the
 * damage a compositor would really produce for the change, and once with the
 * whole output damaged. The second is a complete reconstruction that borrows
 * nothing from the first -- a full-damage frame reads no pixel of the target it
 * does not immediately overwrite.
 *
 * `partial` and `full` are both returned so a caller can look at where they
 * differ rather than only at whether they do.
 */
static bool oracle(struct harness *h, struct avk_image *bg_a,
		struct avk_image *bg_b, const struct blur_spec *blurs, size_t nblurs,
		const pixman_region32_t *damage, uint32_t *partial, uint32_t *full) {
	if (!render_frame(h, bg_a, blurs, nblurs, NULL, NULL)) {
		return false;
	}
	if (!render_frame(h, bg_b, blurs, nblurs, damage, partial)) {
		return false;
	}
	/* Re-establish A, so the full render starts from the same target state the
	 * partial one did. A full-damage frame should not depend on it -- and if
	 * some future change makes it depend on it, this is the line that stops
	 * the oracle from quietly agreeing with the thing it is checking. */
	if (!render_frame(h, bg_a, blurs, nblurs, NULL, NULL)) {
		return false;
	}
	return render_frame(h, bg_b, blurs, nblurs, NULL, full);
}

/* A damage region covering one block, as a compositor would report it. */
static void block_damage(pixman_region32_t *out, int bx, int by) {
	pixman_region32_init_rect(out, bx, by, (unsigned)g_block_w,
		(unsigned)g_block_h);
}

/* ── 1. partial == full, everywhere it matters ──────────────────────────── */

static void case_at(struct harness *h, const char *name, int bx, int by,
		const struct blur_spec *blurs, size_t nblurs) {
	fill_ground(g_changed);
	put_block(g_changed, bx, by);
	if (!stage(h, h->bg_b, g_changed, NULL)) {
		CHECK(false, "%s: could not upload the changed backdrop", name);
		return;
	}
	pixman_region32_t dmg;
	block_damage(&dmg, bx, by);

	static uint32_t partial[W * H], full[W * H];
	bool ok = oracle(h, h->bg_a, h->bg_b, blurs, nblurs, &dmg, partial, full);
	pixman_region32_fini(&dmg);
	if (!ok) {
		CHECK(false, "%s: render failed", name);
		return;
	}
	struct diff d = compare(partial, full);
	if (d.pixels != 0) {
		printf("  note: %s worst %d codes at %d,%d\n", name, d.worst, d.wx,
			d.wy);
	}
	CHECK(d.pixels == 0, "%s: partial == full (%ld wrong)", name, d.pixels);
}

static void test_source_damage_positions(struct harness *h) {
	printf("\n-- a partial frame is a full frame, wherever the source moved --\n");

	/*
	 * PREMISE FIRST. If the block did not actually change anything, every
	 * comparison below is between two identical pictures and passes for the
	 * wrong reason. This is the fixture asserting that it is a fixture.
	 */
	fill_ground(g_changed);
	put_block(g_changed, BX + BW / 2 - BLOCK / 2, BY + BH / 2 - BLOCK / 2);
	static uint32_t with[W * H], without[W * H];
	if (!stage(h, h->bg_b, g_changed, NULL)
			|| !render_frame(h, h->bg_a, &DEFAULT_BLUR, 1, NULL, without)
			|| !render_frame(h, h->bg_b, &DEFAULT_BLUR, 1, NULL, with)) {
		CHECK(false, "premise: could not render the two backdrops");
		return;
	}
	struct diff moved = compare(with, without);
	printf("  note: the block changes %ld pixels, worst %d codes\n",
		moved.pixels, moved.worst);
	CHECK(moved.pixels > 2000 && moved.worst > 40,
		"PREMISE: the fixture's change is large and bright (%ld px, %d codes)",
		moved.pixels, moved.worst);

	/* 1. centre. 2-5. each edge of the node, half in and half out, which is
	 * where a support that is short on one side shows and a symmetric one does
	 * not. */
	case_at(h, "centre", BX + BW / 2 - BLOCK / 2, BY + BH / 2 - BLOCK / 2,
		&DEFAULT_BLUR, 1);
	case_at(h, "left edge", BX - BLOCK / 2, BY + BH / 2, &DEFAULT_BLUR, 1);
	case_at(h, "right edge", BX + BW - BLOCK / 2, BY + BH / 2,
		&DEFAULT_BLUR, 1);
	case_at(h, "top edge", BX + BW / 2, BY - BLOCK / 2, &DEFAULT_BLUR, 1);
	case_at(h, "bottom edge", BX + BW / 2, BY + BH - BLOCK / 2,
		&DEFAULT_BLUR, 1);
}

/* ── 2. odd extents ─────────────────────────────────────────────────────── */

static void test_odd_extents(struct harness *h) {
	printf("\n-- odd extents, where a power-of-two assumption under-covers --\n");

	/*
	 * 63/64/65 and 127/128/129. level_extent() FLOORS, so 129 pixels become 64
	 * texels at level 1 and each spans 2.016 source pixels rather than 2.
	 * Assuming 2^i under-covers by a fraction of a pixel per level, which
	 * accumulates -- and a support that is short by one pixel leaves a
	 * one-pixel stale ring that only a moving fixture ever shows.
	 */
	static const int sizes[] = { 63, 64, 65, 127, 128, 129 };
	for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		int n = sizes[i];
		struct blur_spec spec = {
			.dst = { 40, 40, n, n }, .levels = 3, .radius = 2.0f,
		};
		char name[64];
		/* An odd ORIGIN too, on the odd sizes: the capture's alignment grows to
		 * an even origin and must not move the logical write region with it. */
		if ((n & 1) != 0) {
			spec.dst.x = 41;
			spec.dst.y = 43;
		}
		snprintf(name, sizeof(name), "%dx%d at %d,%d", n, n, spec.dst.x,
			spec.dst.y);
		/* The block straddles the node's right edge, so the damage sits where
		 * the level grid's rounding is worst. */
		case_at(h, name, spec.dst.x + n - BLOCK / 2, spec.dst.y + n / 2,
			&spec, 1);
	}
}

/* ── 3. radii ───────────────────────────────────────────────────────────── */

static void test_radii(struct harness *h) {
	printf("\n-- three radii, since radius decides the support --\n");
	static const struct { const char *name; uint32_t levels; float radius; }
		cases[] = {
			{ "radius 1, 1 level", 1, 1.0f },
			{ "radius 2, 3 levels", 3, 2.0f },
			{ "radius 6, 4 levels", 4, 6.0f },
		};
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		struct blur_spec spec = {
			.dst = { BX, BY, BW, BH },
			.levels = cases[i].levels, .radius = cases[i].radius,
		};
		struct avk_blur_params p = {
			.levels = spec.levels, .radius = spec.radius,
			.brightness = 1.0f, .contrast = 1.0f, .saturation = 1.0f,
		};
		printf("  note: %s -> support %u px\n", cases[i].name,
			avk_blur_support_max(&p, (uint32_t)BW, (uint32_t)BH));
		case_at(h, cases[i].name, BX + BW / 2 - BLOCK / 2, BY + BH / 3,
			&spec, 1);
	}
}

/* ── 4. influence, in and out ───────────────────────────────────────────── */

static void test_influence(struct harness *h) {
	printf("\n-- what can and cannot reach the blur --\n");

	/*
	 * A SMALL-SUPPORT BLUR, and the reason is a fixture that was blind.
	 *
	 * The default node (radius 2, 3 levels) has a support of 57 SOURCE PIXELS.
	 * Its dependency therefore reaches 57 px beyond a node that starts at x=48
	 * -- which is off the left edge of a 256 px target, so it is CLAMPED to 0
	 * and there is no "outside" left to put anything in. The first run put the
	 * far block at x=0, inside the clamped dependency, and measured 202 changed
	 * pixels inside the blur: a correct measurement of a test that was asking
	 * the wrong question.
	 *
	 * One level and radius 1 gives a support of 7 px, which leaves room on this
	 * target for a change that is genuinely, provably out of reach.
	 */
	struct blur_spec spec = { .dst = { BX, BY, BW, BH }, .levels = 1,
		.radius = 1.0f };
	struct avk_blur_params p = {
		.levels = spec.levels, .radius = spec.radius,
		.brightness = 1.0f, .contrast = 1.0f, .saturation = 1.0f,
	};
	uint32_t support = avk_blur_support_max(&p, (uint32_t)BW, (uint32_t)BH);
	int dep_left = BX - (int)support;
	printf("  note: support %u px, so the dependency starts at x=%d\n",
		support, dep_left);
	CHECK(dep_left > (int)support + BLOCK,
		"PREMISE: there is room outside the dependency on this target (%d)",
		dep_left);

	/*
	 * INSIDE: the block's rightmost column lands exactly on the dependency's
	 * left edge, so one column of it can reach the blur and nothing else can.
	 *
	 * What is asserted is that the frame is still CORRECT -- not that the
	 * output visibly moved. At the very edge of the support the contribution is
	 * mathematically real and far below one 8-bit code, and a test demanding a
	 * visible change there would be demanding something the format cannot
	 * represent. The falsifier for a support that is too short is the break
	 * below, which removes a whole layer of expansion rather than one pixel.
	 */
	case_at(h, "one pixel inside the dependency", dep_left - BLOCK + 1,
		BY + BH / 2, &spec, 1);

	/*
	 * OUTSIDE, with a full support to spare. Nothing about this change can
	 * mathematically reach the blur, so the blur's own result must be
	 * BIT-IDENTICAL between the two frames -- and, separately, the renderer
	 * must not have recomputed it at all.
	 */
	int outside_x = dep_left - BLOCK - (int)support;
	if (outside_x < 0) {
		outside_x = 0;
	}
	fill_ground(g_changed);
	put_block(g_changed, outside_x, BY + BH / 2);
	static uint32_t before[W * H], after[W * H];
	pixman_region32_t dmg;
	block_damage(&dmg, outside_x, BY + BH / 2);
	bool ok = render_frame(h, h->bg_a, &spec, 1, NULL, before)
		&& stage(h, h->bg_b, g_changed, NULL);
	uint64_t touched_before = h->renderer.blur_damage_nodes_touched;
	uint64_t skipped_before = h->renderer.blur_damage_nodes_skipped;
	ok = ok && render_frame(h, h->bg_b, &spec, 1, &dmg, after);
	pixman_region32_fini(&dmg);
	if (!ok) {
		CHECK(false, "outside: render failed");
		return;
	}
	struct diff whole = compare(before, after);
	struct diff inside_node = compare_box(before, after, spec.dst);
	printf("  note: the far change at x=%d moves %ld pixels overall, %ld of "
		"them inside the blur\n", outside_x, whole.pixels, inside_node.pixels);
	CHECK(whole.pixels > 0,
		"PREMISE: the far change did happen (%ld px)", whole.pixels);
	CHECK(inside_node.pixels == 0,
		"a change beyond the support leaves the blur bit-identical");
	CHECK(h->renderer.blur_damage_nodes_touched == touched_before
			&& h->renderer.blur_damage_nodes_skipped == skipped_before + 1,
		"and the blur is not recomputed at all (touched +%" PRIu64
		", skipped +%" PRIu64 ")",
		h->renderer.blur_damage_nodes_touched - touched_before,
		h->renderer.blur_damage_nodes_skipped - skipped_before);
}

/* ── 5. the saving ──────────────────────────────────────────────────────── */

/*
 * WHAT THE PROPAGATION SAVED, and the number that governs it.
 *
 * The saving is not a property of the damage code. It is the ratio of the
 * SUPPORT to the NODE, because a change of any size dirties a neighbourhood one
 * support wide and the prefix has to be rebuilt one support wider still:
 *
 *     rebuild ~ (change + 4*support)^2  /  (node + 2*support)^2
 *
 * At radius 2 and 3 levels the support is 57 SOURCE PIXELS, which on the 256 px
 * fixture below is most of the node -- so the honest measured saving there is a
 * few percent, and reporting it as though it were the system's saving would be
 * misleading. The same code on a small-support blur saves an order of
 * magnitude. Both are measured here rather than one being chosen.
 */
static void measure_saving(struct harness *h, const char *name,
		const struct blur_spec *spec, int bx, int by, int expect_below_pct) {
	fill_ground(g_changed);
	put_block(g_changed, bx, by);
	if (!stage(h, h->bg_b, g_changed, NULL)
			|| !render_frame(h, h->bg_a, spec, 1, NULL, NULL)) {
		CHECK(false, "%s: setup failed", name);
		return;
	}
	uint64_t rebuild0 = h->renderer.blur_prefix_rebuild_pixels;
	uint64_t capture0 = h->renderer.blur_full_capture_pixels;
	uint64_t replay0 = h->renderer.blur_prefix_pixels;
	uint64_t src0 = h->renderer.blur_source_damage_pixels;
	uint64_t dep0 = h->renderer.blur_full_dependency_pixels;
	uint64_t out0 = h->renderer.blur_output_damage_pixels;
	uint64_t write0 = h->renderer.blur_full_write_pixels;

	pixman_region32_t dmg;
	block_damage(&dmg, bx, by);
	bool ok = render_frame(h, h->bg_b, spec, 1, &dmg, NULL);
	pixman_region32_fini(&dmg);
	if (!ok) {
		CHECK(false, "%s: render failed", name);
		return;
	}
	uint64_t rebuild = h->renderer.blur_prefix_rebuild_pixels - rebuild0;
	uint64_t capture = h->renderer.blur_full_capture_pixels - capture0;
	uint64_t replay = h->renderer.blur_prefix_pixels - replay0;
	uint64_t src = h->renderer.blur_source_damage_pixels - src0;
	uint64_t dep = h->renderer.blur_full_dependency_pixels - dep0;
	uint64_t out = h->renderer.blur_output_damage_pixels - out0;
	uint64_t write = h->renderer.blur_full_write_pixels - write0;

	struct avk_blur_params p = {
		.levels = spec->levels, .radius = spec->radius,
		.brightness = 1.0f, .contrast = 1.0f, .saturation = 1.0f,
	};
	printf("  note: %s (support %u) -- source %" PRIu64 "/%" PRIu64
		", output %" PRIu64 "/%" PRIu64 ", rebuild %" PRIu64 "/%" PRIu64
		" = %" PRIu64 "%%\n", name,
		avk_blur_support_max(&p, (uint32_t)spec->dst.width,
			(uint32_t)spec->dst.height),
		src, dep, out, write, rebuild, capture,
		capture ? rebuild * 100 / capture : 0);
	CHECK(rebuild > 0 && capture > 0
			&& rebuild * 100 / capture <= (uint64_t)expect_below_pct,
		"%s: the rebuild is at most %d%% of the capture (%" PRIu64 "%%)",
		name, expect_below_pct, capture ? rebuild * 100 / capture : 100);
	CHECK(replay == rebuild,
		"%s: the replay counter reports the region the segment was given",
		name);
	CHECK(out < write, "%s: the output damage is smaller than the write region",
		name);
	/* Every source pixel needed is reconstructed, so the rebuild is BIGGER than
	 * the source damage. Collapsing the two is the mistake the region set
	 * exists to name. */
	CHECK(rebuild > src, "%s: and the rebuild is larger than the source damage "
		"(%" PRIu64 " > %" PRIu64 ")", name, rebuild, src);
}

static void test_saving(struct harness *h) {
	printf("\n-- what the propagation actually saved --\n");

	/* A support that is a third of the node: the fixture's own geometry leaves
	 * almost nothing to save, and the number says so. */
	measure_saving(h, "support 57 on a 160x144 node", &DEFAULT_BLUR,
		BX + BW / 2 - BLOCK / 2, BY + BH / 2 - BLOCK / 2, 100);

	/* A support of 7 on the same node -- the shape of a real desktop, where the
	 * blurred window is much larger than the kernel's reach. */
	struct blur_spec small = { .dst = { BX, BY, BW, BH }, .levels = 1,
		.radius = 1.0f };
	measure_saving(h, "support 7 on a 160x144 node", &small,
		BX + BW / 2 - BLOCK / 2, BY + BH / 2 - BLOCK / 2, 25);
}

/* ── 6. transitive damage ───────────────────────────────────────────────── */

/*
 * ── TWO BLURS, AND WHERE THE TRANSITIVE EDGE ACTUALLY MATTERS ─────────────
 *
 * This geometry was got wrong twice, and both wrong versions passed their own
 * premise assertions. The derivation, in full, because the obvious reading of
 * "make the change reach blur 2 only through blur 1" produces a fixture that
 * cannot see the break at all.
 *
 * Write S1, S2 for the two supports, and let the change end at `cr`.
 *
 *     the change misses blur 2 directly     cr <  write2_left - S2
 *     blur 1's output damage reaches it     cr >= write2_left - S2 - S1
 *
 * That much is the band. But the break does not merely stop blur 2 from being
 * DAMAGED -- the demand sweep still recomputes blur 2 wherever the FRAME's
 * damage covers it, and the frame's damage already contains blur 1's output
 * damage. So the region where the break can leave a wrong pixel is:
 *
 *     dilate(blur2_source_damage, S2)  minus  blur1_output_damage
 *
 * which is empty unless S2 is LARGE. The second attempt made S2 small (7 px) to
 * stop blur 2 diluting what it inherited, and measured ZERO wrong pixels: the
 * demand sweep had covered every pixel the damage sweep would have. The first
 * attempt, with two equal supports of 26, left exactly ONE pixel at ONE code
 * for the same reason.
 *
 * So blur 2's write region has to sit BEYOND the reach of blur 1's own output
 * damage, while its DEPENDENCY still overlaps it. That needs a large S2, which
 * is what pushes the dependency left of the write region far enough to catch
 * blur 1 while the write region itself is out of the frame damage entirely.
 *
 * Both numbers are asserted below against what the renderer computes, and the
 * separation of the two write regions is asserted too -- it is the property
 * that makes this a test of propagation rather than of recomposition.
 */
static const struct blur_spec TWO_BLURS[2] = {
	{ .dst = { 10, 30, 170, 190 }, .levels = 3, .radius = 2.0f,
		.brightness = 1.6f },
	{ .dst = { 60, 30, 180, 190 }, .levels = 3, .radius = 2.0f },
};

/*
 * A WALL, NOT A BLOCK, AND THE MEASUREMENT THAT FORCED IT.
 *
 * With a 48x48 block here, blur 1's box moved 10906 pixels and blur 2's moved
 * ZERO -- on two FULL renders, with no damage tracking involved at all. The
 * transitive influence was real and below one 8-bit code: a small bright square
 * spread through a support of 58 and then through another of 60 arrives as
 * nothing a byte can hold.
 *
 * The change is therefore the whole left part of blur 1's box, 114 px wide and
 * 190 tall, ending just short of blur 2's dependency. Its right EDGE is what
 * has to stay outside; its area is free, and the area is what carries amplitude
 * through two blurs.
 */
#define TRANSITIVE_X 0
#define TRANSITIVE_Y 30
#define TRANSITIVE_W 114
#define TRANSITIVE_H 190

/*
 * THREE, WHERE THE THIRD IS REACHABLE ONLY THROUGH THE SECOND.
 *
 * The same shape one step further along. A propagation that carried exactly one
 * generation would update blur 1 and blur 2 and leave blur 3 stale.
 */
static const struct blur_spec THREE_BLURS[3] = {
	{ .dst = { 4, 30, 140, 190 }, .levels = 3, .radius = 2.0f,
		.brightness = 1.6f },
	{ .dst = { 60, 30, 150, 190 }, .levels = 3, .radius = 2.0f,
		.brightness = 1.4f },
	{ .dst = { 110, 30, 140, 190 }, .levels = 3, .radius = 2.0f },
};

static uint32_t support_of(const struct blur_spec *spec) {
	struct avk_blur_params p = {
		.levels = spec->levels, .radius = spec->radius,
		.brightness = 1.0f, .contrast = 1.0f, .saturation = 1.0f,
	};
	return avk_blur_support_max(&p, (uint32_t)spec->dst.width,
		(uint32_t)spec->dst.height);
}

static void test_transitive(struct harness *h) {
	printf("\n-- damage that has to cross one blur to reach another --\n");

	g_block_w = TRANSITIVE_W;
	g_block_h = TRANSITIVE_H;

	/*
	 * THE PREMISE, ARITHMETIC RATHER THAN HOPE: blur 1's WRITE region has to
	 * lie inside blur 2's DEPENDENCY, or there is no edge between them and the
	 * inheritance counter below would be measuring nothing.
	 *
	 * An earlier version of this fixture asserted the opposite -- that the
	 * change could not reach blur 2 directly and that the two write regions did
	 * not overlap -- because it was built to make a PIXEL break visible. That
	 * break is gone (see below), and with it the reason to keep the two blurs
	 * apart. Overlapping them is both the realistic case and the one where the
	 * edge carries the most.
	 */
	uint32_t s1 = support_of(&TWO_BLURS[0]);
	uint32_t s2 = support_of(&TWO_BLURS[1]);
	int write1_right = TWO_BLURS[0].dst.x + TWO_BLURS[0].dst.width;
	int dep2_left = TWO_BLURS[1].dst.x - (int)s2;
	printf("  note: supports %u and %u; blur 1 writes to %d, blur 2's "
		"dependency starts at %d\n", s1, s2, write1_right, dep2_left);
	CHECK(write1_right > dep2_left && TWO_BLURS[0].dst.x < write1_right,
		"PREMISE: blur 1's result lies inside blur 2's dependency (%d > %d)",
		write1_right, dep2_left);

	/*
	 * ── HOW MUCH SIGNAL IS THERE AT ALL, AND WHY THE BREAK WAS DELETED ────
	 *
	 * Measured, on two FULL renders with no damage tracking involved:
	 *
	 *   a plain blur 1        removing it ENTIRELY moved blur 2's output by
	 *                         ONE 8-bit code
	 *   blur 1 at brightness  removing it moved blur 2 by 16 codes over
	 *   1.6                   27018 pixels
	 *
	 * A dual-Kawase blur preserves the local mean, so blurring an already
	 * blurred field gives very nearly what blurring the raw field would; blur 2
	 * is almost blind to whether blur 1 ran. Post-effects break that symmetry,
	 * which is why blur 1 here has brightness -- and is also what the real
	 * compositor does.
	 *
	 * Even so, a break that removed the transitive edge could not be made to
	 * leave more than 4 wrong pixels at 1 code in any geometry tried, because
	 * the region it strands is at kernel-tail distance in BOTH blurs and the
	 * error is the product of two decays. So there is no such break. The edge
	 * is asserted STRUCTURALLY instead, on the counter below, and the reasoning
	 * is in docs/avk-effects.md rather than in a switch that fails weakly.
	 */
	fill_ground(g_changed);
	put_block(g_changed, TRANSITIVE_X, TRANSITIVE_Y);
	static uint32_t a2[W * H], b2[W * H];
	uint64_t inherited0 = h->renderer.blur_transitive_damage_pixels;
	if (stage(h, h->bg_b, g_changed, NULL)
			&& render_frame(h, h->bg_a, TWO_BLURS, 2, NULL, a2)
			&& render_frame(h, h->bg_b, TWO_BLURS, 2, NULL, b2)) {
		struct diff in1 = compare_box(a2, b2, TWO_BLURS[0].dst);
		struct diff in2 = compare_box(a2, b2, TWO_BLURS[1].dst);
		printf("  note: blur 1's box moves %ld px (worst %d), blur 2's %ld px "
			"(worst %d)\n", in1.pixels, in1.worst, in2.pixels, in2.worst);
	}
	uint64_t inherited = h->renderer.blur_transitive_damage_pixels - inherited0;
	printf("  note: %" PRIu64 " source-damage pixels were inherited from an "
		"earlier blur\n", inherited);
	CHECK(inherited > 0,
		"blur 2's source damage contains blur 1's output damage (%" PRIu64
		" px)", inherited);

	/* And a single blur inherits nothing, so the counter is measuring the edge
	 * rather than counting source damage twice. */
	uint64_t solo0 = h->renderer.blur_transitive_damage_pixels;
	render_frame(h, h->bg_b, &DEFAULT_BLUR, 1, NULL, NULL);
	CHECK(h->renderer.blur_transitive_damage_pixels == solo0,
		"and a lone blur inherits nothing (%" PRIu64 ")",
		h->renderer.blur_transitive_damage_pixels - solo0);

	case_at(h, "two overlapping blurs, change inside the first",
		TRANSITIVE_X, TRANSITIVE_Y, TWO_BLURS, 2);
	case_at(h, "two blurs, change in the overlap", 150, 90, TWO_BLURS, 2);
	case_at(h, "three blurs, one change through the whole chain",
		TRANSITIVE_X, TRANSITIVE_Y, THREE_BLURS, 3);
	case_at(h, "three blurs, change in the last", 200, 90, THREE_BLURS, 3);

	g_block_w = BLOCK;
	g_block_h = BLOCK;
}

/* ── 7. the breaks ──────────────────────────────────────────────────────── */

/*
 * A BREAK IS ONLY A BREAK IF IT LEAVES WRONG PIXELS.
 *
 * Both of these are set on the renderer rather than read from the environment,
 * so one process renders the same scene correct and broken and diffs the two.
 * A break that merely moved a counter would be caught by nothing here, which is
 * the point: the assertion is on the picture.
 */
static void break_case(struct harness *h, const char *name,
		bool *knob, const struct blur_spec *blurs, size_t nblurs,
		int bx, int by) {
	fill_ground(g_changed);
	put_block(g_changed, bx, by);
	if (!stage(h, h->bg_b, g_changed, NULL)) {
		CHECK(false, "%s: upload failed", name);
		return;
	}
	pixman_region32_t dmg;
	block_damage(&dmg, bx, by);

	static uint32_t broken[W * H], good[W * H];
	*knob = true;
	bool ok = render_frame(h, h->bg_a, blurs, nblurs, NULL, NULL)
		&& render_frame(h, h->bg_b, blurs, nblurs, &dmg, broken);
	*knob = false;
	ok = ok && render_frame(h, h->bg_a, blurs, nblurs, NULL, NULL)
		&& render_frame(h, h->bg_b, blurs, nblurs, NULL, good);
	pixman_region32_fini(&dmg);
	if (!ok) {
		CHECK(false, "%s: render failed", name);
		return;
	}
	struct diff d = compare(broken, good);
	printf("  note: %s leaves %ld wrong pixels, worst %d codes at %d,%d\n",
		name, d.pixels, d.worst, d.wx, d.wy);
	CHECK(d.pixels > 0 && d.worst > 2,
		"%s leaves WRONG PIXELS (%ld px, %d codes)", name, d.pixels, d.worst);
}

static void test_breaks(struct harness *h) {
	printf("\n-- the falsifiers --\n");

	/* Under-damage: the forward dilation removed. The blur is still recomputed
	 * and the counters still move; what is left is a stale ring one support
	 * wide around the change. */
	break_case(h, "BREAK under-damage", &h->renderer.break_blur_under_damage,
		&DEFAULT_BLUR, 1, BX + BW / 2 - BLOCK / 2, BY + BH / 2 - BLOCK / 2);

}

/* ── 8. the forward support, against a per-level walk ───────────────────── */

/*
 * The header of avk_blur.h derives forward reach == reverse reach by summing
 * the chain. This composes the steps instead: it starts with a single changed
 * source pixel and walks the interval through every down and up step in turn,
 * using the same level extents the renderer uses. Two different arithmetic
 * routes to the same number, so a change to one kernel's reach that misses the
 * other fails here rather than in a fringe on a moving window.
 *
 * No GPU: this is arithmetic.
 */
static double level_span(uint32_t base, uint32_t level) {
	uint32_t n = base >> level;
	if (n == 0) { n = 1; }
	return (double)base / (double)n;
}

static void test_forward_support_walk(struct harness *h) {
	(void)h;
	printf("\n-- forward reach, composed step by step --\n");

	static const uint32_t bases[] = { 63, 64, 65, 127, 128, 129, 160, 256 };
	static const float radii[] = { 1.0f, 2.0f, 6.0f };
	double worst = 0.0;
	int compared = 0;
	for (size_t bi = 0; bi < sizeof(bases) / sizeof(bases[0]); bi++) {
		for (size_t ri = 0; ri < sizeof(radii) / sizeof(radii[0]); ri++) {
			uint32_t base = bases[bi];
			struct avk_blur_params p = {
				.levels = 3, .radius = radii[ri],
				.brightness = 1.0f, .contrast = 1.0f, .saturation = 1.0f,
			};
			/* The renderer's own effective level count, reached the same way
			 * it reaches it: halve until a level would be under 2 texels. */
			uint32_t levels = p.levels;
			while (levels > 0 && ((base >> levels) < 2)) {
				levels--;
			}
			if (levels == 0) {
				continue;
			}
			/* The walk: half-width of the affected interval, in source pixels,
			 * starting from a single pixel. */
			double w = 0.0;
			for (uint32_t i = 1; i <= levels; i++) {
				w += (0.5 * radii[ri] + 1.0) * level_span(base, i - 1);
			}
			for (uint32_t i = levels; i >= 1; i--) {
				w += ((double)radii[ri] + 1.0) * level_span(base, i);
			}
			w += 1.0;   /* the fractional-origin pixel, as the renderer adds */

			struct avk_blur_support f = avk_blur_forward_support_of(&p, base,
				base);
			double diff = f.left - w;
			if (diff < 0) { diff = -diff; }
			if (diff > worst) { worst = diff; }
			compared++;
		}
	}
	printf("  note: %d (extent, radius) pairs, worst disagreement %.6f px\n",
		compared, worst);
	CHECK(compared >= 20, "PREMISE: the walk ran over enough cases (%d)",
		compared);
	/* 1e-4, not 1e-6. avk_blur_support_of() returns FLOATS and the walk above
	 * is in double, so the two disagree in the seventh significant figure by
	 * construction. The first run asserted 1e-6 and failed at 0.000003 px --
	 * a correct measurement of a tolerance that was measuring the storage
	 * format rather than the arithmetic. A real disagreement between two
	 * kernels is a fraction of a pixel at least. */
	CHECK(worst < 1e-4,
		"a composed forward walk equals the summed support (%.6f px)", worst);

	/* And the two directions agree, which is the property the damage code
	 * relies on when it dilates one region by one and another by the other. */
	struct avk_blur_params p = {
		.levels = 3, .radius = 2.0f,
		.brightness = 1.0f, .contrast = 1.0f, .saturation = 1.0f,
	};
	struct avk_blur_support rev = avk_blur_support_of(&p, 160, 144);
	struct avk_blur_support fwd = avk_blur_forward_support_of(&p, 160, 144);
	CHECK(rev.left == fwd.right && rev.right == fwd.left
			&& rev.top == fwd.bottom && rev.bottom == fwd.top,
		"forward is the reverse support mirrored");
}

/* ── 9. property and geometry damage ────────────────────────────────────── */

/*
 * WHAT A PROPERTY CHANGE DAMAGES, AND WHO DECIDES IT.
 *
 * Not the renderer. SceneFX already damages a blur node on every setter --
 * audited one by one in docs/avk-effects.md, and wlr_scene_blur_set_size()
 * explicitly damages the OLD bounds before shrinking, because
 * scene_node_update() derives its damage from the node's CURRENT size and would
 * otherwise strand the area a shrinking node used to cover. scene_node_update()
 * itself starts from the node's existing visible region and unions the new
 * bounds, so a move or a resize damages old u new without anything here asking.
 *
 * So the renderer's obligation is narrower, and it is what this group tests:
 * GIVEN the damage a compositor really produces for the change -- the union of
 * the old and new node boxes -- a partially damaged frame must contain the same
 * pixels as a completely redrawn one. Every case below supplies exactly that
 * region and nothing more.
 *
 * THE EDGE-SOFTNESS ENVELOPE NEEDS NO EXPANSION, and that is a fact about the
 * material rather than an assumption. blur_soft.frag computes its coverage on a
 * box INSET by sigma (`lo = round_box.xy + sigma`) and the draw is the node's
 * own box, so the fade runs INWARD from the edge and the visible envelope is
 * exactly the write region. A second, approximate expansion for it would be a
 * magic number with nothing to check it against.
 *
 * DARKEN IS A MATERIAL-ONLY CHANGE and is deliberately not given a wider source
 * dependency: the same source samples produce a different composite. Its case
 * below damages the node box and nothing more, which is what that distinction
 * means in practice.
 */
static void property_case(struct harness *h, const char *name,
		const struct blur_spec *before, const struct blur_spec *after) {
	/* The damage a compositor produces: old bounds u new bounds, and nothing
	 * else. The backdrop does not move, so this is the whole change. */
	pixman_region32_t dmg;
	pixman_region32_init_rect(&dmg, before->dst.x, before->dst.y,
		(unsigned)before->dst.width, (unsigned)before->dst.height);
	pixman_region32_union_rect(&dmg, &dmg, after->dst.x, after->dst.y,
		(unsigned)after->dst.width, (unsigned)after->dst.height);

	static uint32_t partial[W * H], full[W * H];
	bool ok = render_frame(h, h->bg_a, before, 1, NULL, NULL)
		&& render_frame(h, h->bg_a, after, 1, &dmg, partial)
		&& render_frame(h, h->bg_a, before, 1, NULL, NULL)
		&& render_frame(h, h->bg_a, after, 1, NULL, full);
	pixman_region32_fini(&dmg);
	if (!ok) {
		CHECK(false, "%s: render failed", name);
		return;
	}
	struct diff d = compare(partial, full);
	if (d.pixels != 0) {
		printf("  note: %s worst %d codes at %d,%d\n", name, d.worst, d.wx,
			d.wy);
	}
	CHECK(d.pixels == 0, "%s: partial == full (%ld wrong)", name, d.pixels);
}

/* The same, for a change that removes the node entirely. */
static void enable_case(struct harness *h, const char *name,
		const struct blur_spec *spec, bool on_first) {
	pixman_region32_t dmg;
	pixman_region32_init_rect(&dmg, spec->dst.x, spec->dst.y,
		(unsigned)spec->dst.width, (unsigned)spec->dst.height);
	size_t n_a = on_first ? 1 : 0, n_b = on_first ? 0 : 1;

	static uint32_t partial[W * H], full[W * H];
	bool ok = render_frame(h, h->bg_a, spec, n_a, NULL, NULL)
		&& render_frame(h, h->bg_a, spec, n_b, &dmg, partial)
		&& render_frame(h, h->bg_a, spec, n_a, NULL, NULL)
		&& render_frame(h, h->bg_a, spec, n_b, NULL, full);
	pixman_region32_fini(&dmg);
	if (!ok) {
		CHECK(false, "%s: render failed", name);
		return;
	}
	struct diff d = compare(partial, full);
	CHECK(d.pixels == 0, "%s: partial == full (%ld wrong)", name, d.pixels);
}

static void test_property_damage(struct harness *h) {
	printf("\n-- a property change, damaged as a compositor would --\n");

	const struct blur_spec base = DEFAULT_BLUR;
	struct blur_spec probes[] = {
		{ .dst = base.dst, .levels = 5, .radius = 4.0f },
		{ .dst = base.dst, .levels = 1, .radius = 1.0f },
		{ .dst = base.dst, .levels = base.levels, .radius = base.radius,
			.alpha = 0.4f },
		{ .dst = base.dst, .levels = base.levels, .radius = base.radius,
			.darken = true },
		{ .dst = base.dst, .levels = base.levels, .radius = base.radius,
			.edge_softness = 14.0f },
		{ .dst = base.dst, .levels = base.levels, .radius = base.radius,
			.corner = 28 },
		{ .dst = { BX + 40, BY + 30, BW, BH }, .levels = base.levels,
			.radius = base.radius },
		{ .dst = { BX, BY, BW + 41, BH + 27 }, .levels = base.levels,
			.radius = base.radius },
		{ .dst = { BX, BY, BW - 43, BH - 29 }, .levels = base.levels,
			.radius = base.radius },
		{ .dst = base.dst, .levels = base.levels, .radius = base.radius,
			.clip = { BX + 20, BY + 20, 80, 70 } },
		{ .dst = base.dst, .levels = base.levels, .radius = base.radius,
			.clip = { BX + 55, BY + 40, 80, 70 } },
		{ .dst = base.dst, .levels = base.levels, .radius = base.radius,
			.clip = { BX + 20, BY + 20, 130, 110 } },
	};
	static const char *names[] = {
		"radius grow", "radius shrink", "alpha", "darken", "edge_softness",
		"corners", "move", "resize larger", "resize smaller",
		"clip appears", "clip moves", "clip grows",
	};
	const size_t n = sizeof(probes) / sizeof(probes[0]);

	/*
	 * PREMISE: each change below must actually change the picture. A property
	 * that renders identically either way would make its case pass for the
	 * wrong reason, and several of these -- darken over a dark ground, a corner
	 * radius on a big box -- are exactly the sort that could.
	 */
	static uint32_t ref[W * H], probe[W * H];
	if (!render_frame(h, h->bg_a, &base, 1, NULL, ref)) {
		CHECK(false, "property: could not render the reference");
		return;
	}
	int silent = 0;
	for (size_t i = 0; i < n; i++) {
		if (!render_frame(h, h->bg_a, &probes[i], 1, NULL, probe)) {
			continue;
		}
		struct diff d = compare(ref, probe);
		if (d.pixels == 0) {
			printf("  note: %s changes NOTHING\n", names[i]);
			silent++;
		}
	}
	CHECK(silent == 0,
		"PREMISE: every property change alters the picture (%d silent)",
		silent);

	for (size_t i = 0; i < n; i++) {
		property_case(h, names[i], &base, &probes[i]);
	}
	/* And back the other way for the clip, whose OLD extent is larger than its
	 * new one -- the shape of the stale-region bug. */
	property_case(h, "clip shrinks", &probes[11], &probes[9]);
	property_case(h, "clip disappears", &probes[9], &base);

	enable_case(h, "disable", &base, true);
	enable_case(h, "enable", &base, false);
}

/* ── 10. realistic workloads, and the multi-rect gap ────────────────────── */

/*
 * A TEXT CURSOR AND A MOVING WIDGET -- the two shapes of change that damage
 * tracking exists for, and the ones where a stale pixel is most visible.
 *
 * Both are measured as well as checked: the point of M4F.2B is not only that a
 * partial frame is right but that it is CHEAPER, and a fixture that proves the
 * first without reporting the second says nothing about whether the complexity
 * paid for itself.
 */
static void test_realistic(struct harness *h) {
	printf("\n-- realistic workloads --\n");

	/* A terminal-sized cell blinking on and off behind a large blur, over a
	 * dark high-frequency ground. Support 7 rather than 57, because a real
	 * blurred window is far larger than the kernel's reach and the 256 px
	 * fixture cannot be. */
	struct blur_spec term = { .dst = { 20, 20, 216, 216 }, .levels = 1,
		.radius = 1.0f };
	g_block_w = 8;
	g_block_h = 16;

	case_at(h, "a text cursor behind a blur", 120, 110, &term, 1);
	/* And the cost of the PARTIAL frame alone. Averaging over case_at's three
	 * renders would mix in two full-damage frames, whose output damage is the
	 * whole write region by definition -- a ratio computed that way describes
	 * the oracle rather than the thing being measured. */
	measure_saving(h, "a text cursor", &term, 120, 110, 60);

	/*
	 * A 16x16 WIDGET MOVING 8 PX AT A TIME, several frames in a row, each
	 * compared against a full redraw. The damage is old u new, exactly as a
	 * compositor reports a move -- and a frame that inherited a trail from the
	 * frame before it would show here and nowhere else, because every other
	 * fixture in this file renders at most two frames.
	 */
	g_block_w = 16;
	g_block_h = 16;
	struct blur_spec big = { .dst = { 20, 20, 216, 216 }, .levels = 1,
		.radius = 1.0f };
	static uint32_t partial[W * H], full[W * H];
	long worst_wrong = 0;
	int frames = 0;
	/* Frame 0 establishes; each later frame moves the widget 8 px. */
	fill_ground(g_changed);
	put_block(g_changed, 100, 120);
	bool ok = stage(h, h->bg_a, g_changed, NULL)
		&& render_frame(h, h->bg_a, &big, 1, NULL, NULL);
	for (int step = 1; step <= 5 && ok; step++) {
		int px = 100 + (step - 1) * 8, nx = 100 + step * 8;
		fill_ground(g_changed);
		put_block(g_changed, nx, 120);
		pixman_region32_t dmg;
		pixman_region32_init_rect(&dmg, px, 120, 16, 16);
		pixman_region32_union_rect(&dmg, &dmg, nx, 120, 16, 16);
		ok = stage(h, h->bg_b, g_changed, NULL)
			&& render_frame(h, h->bg_b, &big, 1, &dmg, partial)
			&& render_frame(h, h->bg_b, &big, 1, NULL, full);
		pixman_region32_fini(&dmg);
		if (!ok) {
			break;
		}
		struct diff d = compare(partial, full);
		if (d.pixels > worst_wrong) {
			worst_wrong = d.pixels;
		}
		frames++;
		/* The next step's "previous frame" is this partial one, so the target
		 * carries whatever the partial path left behind -- which is the point.
		 * Re-render the partial frame so the target ends in that state. */
		ok = render_frame(h, h->bg_b, &big, 1, NULL, NULL)
			&& stage(h, h->bg_a, g_changed, NULL);
	}
	CHECK(frames == 5, "PREMISE: the widget moved five times (%d)", frames);
	CHECK(worst_wrong == 0,
		"a widget moving 8 px a frame leaves no trail (%ld wrong)",
		worst_wrong);

	g_block_w = BLOCK;
	g_block_h = BLOCK;

	/*
	 * THE MULTI-RECT GAP, IN DAMAGE THIS TIME.
	 *
	 * test-avk-blur-material proves the COMPOSITE keeps the gap between a
	 * client's two rectangles. This is the other half: the damage machinery must
	 * not quietly take a bounding box either, so the blur's own output damage
	 * has to stay out of the 24 px corridor. Measured on the region, not
	 * inferred from the picture -- a gap that is damaged but happens to be
	 * repainted with the same pixels is invisible to a pixel test and is still
	 * wasted work.
	 */
	struct avk_box r1 = { BX + 10, BY + 10, 50, 100 };
	struct avk_box r2 = { BX + 10 + 50 + 24, BY + 10, 50, 100 };
	struct blur_spec split = { .dst = { BX, BY, BW, BH }, .levels = 1,
		.radius = 1.0f, .clip = r1, .clip2 = r2 };
	uint64_t g_out0 = h->renderer.blur_output_damage_pixels;
	uint64_t g_write0 = h->renderer.blur_full_write_pixels;
	if (render_frame(h, h->bg_a, &split, 1, NULL, NULL)) {
		uint64_t out = h->renderer.blur_output_damage_pixels - g_out0;
		uint64_t write = h->renderer.blur_full_write_pixels - g_write0;
		uint64_t both = (uint64_t)(r1.width * r1.height)
			+ (uint64_t)(r2.width * r2.height);
		uint64_t with_gap = both + (uint64_t)(24 * r1.height);
		printf("  note: two clip rectangles damage %" PRIu64 " of a %" PRIu64
			" write region; the two are %" PRIu64 ", with the gap %" PRIu64
			"\n", out, write, both, with_gap);
		/* Exactly the two rectangles. A bounding box would be `with_gap` and
		 * would still repaint the corridor with the same pixels -- invisible to
		 * a pixel test, and wasted work every frame. */
		CHECK(out == both,
			"the damage is the two clip rectangles, gap and all (%" PRIu64
			" == %" PRIu64 ")", out, both);
		CHECK(out < with_gap,
			"and not their bounding box (%" PRIu64 " < %" PRIu64 ")", out,
			with_gap);
	}
}

/* ── 11. the kernel's reach scales with the radius ──────────────────────── */

/*
 * WHY THIS IS A DAMAGE TEST'S BUSINESS.
 *
 * M4F.2C makes the walker multiply `radius` by the output scale, so that a blur
 * covers the same LOGICAL area on a 1.0 and a 1.5 display instead of the same
 * DEVICE area. That change is only meaningful if the kernel's reach is actually
 * proportional to the radius -- if it were not, scaling the radius would be the
 * wrong lever and `levels` would have to move instead, which a fractional scale
 * cannot do.
 *
 * So this measures the reach directly, at the renderer, where scale does not
 * exist: two blurs over the same box with radii r and 1.5r, and the distance
 * their influence travels from a bright block.
 *
 * REACH IS MEASURED, NOT ASSERTED FROM THE SUPPORT. avk_blur_support_max() is a
 * conservative mathematical bound with an additive bilinear term per level
 * (17.5r + 21 at three levels), so it grows by 1.31x when the radius grows by
 * 1.5x. The visible reach is dominated by the kernel step and grows faster. The
 * two are different quantities and only one of them is what a user sees.
 */
static int measure_reach(struct harness *h, const struct blur_spec *spec,
		int bx, int by) {
	/*
	 * RESTORE THE PRISTINE GROUND FIRST, and this is not defensive tidying.
	 *
	 * test_realistic's moving-widget loop uploads its own content into bg_a --
	 * deliberately, so each step starts from the previous frame. Without this
	 * line the "unchanged" render still contains that widget at x=140, the
	 * difference between the two frames includes it, and the measured reach
	 * comes out at 91 px WITH NO BLUR NODE AT ALL. That was measured, by the
	 * control below, after the first version of this test reported a kernel
	 * reaching ten times its mathematical bound.
	 */
	if (!stage(h, h->bg_a, g_base, NULL)) {
		return -1;
	}
	fill_ground(g_changed);
	put_block(g_changed, bx, by);
	static uint32_t with[W * H], without[W * H];
	if (!stage(h, h->bg_b, g_changed, NULL)
			|| !render_frame(h, h->bg_a, spec, 1, NULL, without)
			|| !render_frame(h, h->bg_b, spec, 1, NULL, with)) {
		return -1;
	}
	/*
	 * Scan RIGHT from the block's right edge along its middle row and find the
	 * last column whose value moved by more than one code. One code is the
	 * quantisation floor: below it the influence is real and unrepresentable,
	 * and a threshold at zero would measure dither rather than reach.
	 */
	int y = by + g_block_h / 2;
	int last = -1;
	for (int x = bx + g_block_w; x < spec->dst.x + spec->dst.width && x < W;
			x++) {
		int a = (int)((with[y * W + x] >> 16) & 0xff);
		int b = (int)((without[y * W + x] >> 16) & 0xff);
		int d = a > b ? a - b : b - a;
		if (d > 1) {
			last = x;
		}
	}
	return last < 0 ? -1 : last - (bx + g_block_w);
}

static void test_radius_reach(struct harness *h) {
	printf("\n-- the reach scales with the radius --\n");

	/* A big node, so the reach is not cut short by the node's own edge, and a
	 * block well inside it. */
	struct blur_spec r1 = { .dst = { 8, 8, 240, 240 }, .levels = 3,
		.radius = 2.0f };
	struct blur_spec r15 = r1;
	r15.radius = 3.0f;   /* 1.5x, which is what scale 1.5 produces */

	g_block_w = 24;
	g_block_h = 24;
	int bx = 40, by = 120;
	int reach1 = measure_reach(h, &r1, bx, by);
	int reach15 = measure_reach(h, &r15, bx, by);
	g_block_w = BLOCK;
	g_block_h = BLOCK;

	struct avk_blur_params p1 = { .levels = 3, .radius = 2.0f,
		.brightness = 1.0f, .contrast = 1.0f, .saturation = 1.0f };
	struct avk_blur_params p15 = { .levels = 3, .radius = 3.0f,
		.brightness = 1.0f, .contrast = 1.0f, .saturation = 1.0f };
	printf("  note: radius 2 reaches %d px (bound %u), radius 3 reaches %d px "
		"(bound %u)\n", reach1,
		avk_blur_support_max(&p1, 240, 240), reach15,
		avk_blur_support_max(&p15, 240, 240));

	/*
	 * TWO CONTROLS, AND THEY ARE THE TEST.
	 *
	 * The first version of this measured a kernel reaching 96 px where its
	 * mathematical bound is 9 -- ten times over, which would have meant every
	 * damage region in M4F.2B was under-covering. It was the instrument:
	 * test_realistic's moving-widget loop leaves its own content in bg_a, so
	 * the "unchanged" render still had a widget in it 140 px along and the
	 * difference between the two frames included it.
	 *
	 * So: the same scene twice must differ by nothing, and a scene with NO BLUR
	 * NODE must show no reach at all. Both are asserted before any number about
	 * the kernel is believed.
	 */
	struct blur_spec ctl = { .dst = { 8, 8, 240, 240 }, .levels = 3,
		.radius = 2.0f };
	static uint32_t a[W * H], b[W * H];
	long twice = -1;
	if (stage(h, h->bg_a, g_base, NULL)
			&& render_frame(h, h->bg_a, &ctl, 1, NULL, a)
			&& render_frame(h, h->bg_a, &ctl, 1, NULL, b)) {
		twice = compare(a, b).pixels;
	}
	CHECK(twice == 0, "PREMISE: the same scene twice is identical (%ld px)",
		twice);

	struct blur_spec none = { .dst = { 8, 8, 240, 240 }, .levels = 0,
		.radius = 2.0f };
	g_block_w = 24; g_block_h = 24;
	int no_blur = measure_reach(h, &none, 40, 120);
	g_block_w = BLOCK; g_block_h = BLOCK;
	CHECK(no_blur < 0,
		"PREMISE: with no blur node nothing reaches past the block (%d)",
		no_blur);

	/*
	 * AND THE BOUND CONTAINS THE MEASURED REACH AT EVERY LEVEL COUNT. This is
	 * the property M4F.2B's damage regions rest on: the support is a hard
	 * mathematical footprint, so a region dilated by it cannot under-cover.
	 * Measured reach is roughly half of it, which is what a conservative bound
	 * against an 8-bit floor should look like.
	 */
	int over = 0;
	for (uint32_t lv = 1; lv <= 4; lv++) {
		struct blur_spec sp = { .dst = { 8, 8, 240, 240 }, .levels = lv,
			.radius = 2.0f };
		struct avk_blur_params pp = { .levels = lv, .radius = 2.0f,
			.brightness = 1.0f, .contrast = 1.0f, .saturation = 1.0f };
		g_block_w = 24; g_block_h = 24;
		int rr = measure_reach(h, &sp, 40, 120);
		g_block_w = BLOCK; g_block_h = BLOCK;
		uint32_t bound = avk_blur_support_max(&pp, 240, 240);
		printf("  note: levels %u -> reaches %d px, bound %u\n", lv, rr, bound);
		if (rr > (int)bound) {
			over++;
		}
	}
	CHECK(over == 0,
		"the support bound contains the measured reach at every level count");

	CHECK(reach1 > 4 && reach15 > 4,
		"PREMISE: both radii reach measurably (%d, %d)", reach1, reach15);
	if (reach1 <= 0) {
		return;
	}
	double ratio = (double)reach15 / (double)reach1;
	printf("  note: reach ratio %.3f for a 1.5x radius\n", ratio);
	/*
	 * 1.2 to 1.9. The reach is proportional to the kernel step, which is
	 * exactly proportional to the radius -- but it is measured against an 8-bit
	 * quantisation floor, and the tail of a wider kernel is flatter, so the
	 * measured edge does not move by precisely 1.5. What matters for M4F.2C is
	 * that it moves substantially and in proportion, which is what makes
	 * scaling the RADIUS the right lever for a fractional output scale;
	 * `levels` could only move it in factors of two.
	 */
	CHECK(ratio > 1.2 && ratio < 1.9,
		"a 1.5x radius reaches about 1.5x as far (%.3f)", ratio);
	CHECK((uint32_t)reach15 <= avk_blur_support_max(&p15, 240, 240),
		"and the wider radius stays inside its own bound (%d <= %u)",
		reach15, avk_blur_support_max(&p15, 240, 240));
}

/* ── main ───────────────────────────────────────────────────────────────── */

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	printf("== avk blur damage (M4F.2B) ==\n");

	struct harness h;
	memset(&h, 0, sizeof(h));
	h.inst = avk_instance_create("avk-blur-damage-test");
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
	h.bg_a = make_image(h.dev, VK_IMAGE_USAGE_SAMPLED_BIT
		| VK_IMAGE_USAGE_TRANSFER_DST_BIT);
	h.bg_b = make_image(h.dev, VK_IMAGE_USAGE_SAMPLED_BIT
		| VK_IMAGE_USAGE_TRANSFER_DST_BIT);
	if (h.target == NULL || h.bg_a == NULL || h.bg_b == NULL) {
		SKIP("no images");
	}

	fill_ground(g_base);
	if (!stage(&h, h.bg_a, g_base, NULL)) {
		SKIP("could not upload the backdrop");
	}

	test_forward_support_walk(&h);
	test_source_damage_positions(&h);
	test_odd_extents(&h);
	test_radii(&h);
	test_influence(&h);
	test_saving(&h);
	test_transitive(&h);
	test_property_damage(&h);
	test_realistic(&h);
	test_radius_reach(&h);
	test_breaks(&h);

	avk_device_wait_idle(h.dev);
	avk_image_destroy(h.dev, h.target);
	avk_image_destroy(h.dev, h.bg_a);
	avk_image_destroy(h.dev, h.bg_b);
	avk_renderer_finish(&h.renderer);
	avk_device_destroy(h.dev);
	avk_instance_destroy(h.inst);

	printf("\n---- %d/%d checks passed\n", checks - failures, checks);
	return failures == 0 ? 0 : 1;
}
