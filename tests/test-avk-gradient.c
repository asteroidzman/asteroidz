/*
 * M4C: gradients, asserted on the framebuffer.
 *
 * These live here rather than in a headless compositor suite because a
 * gradient is a numerical claim. A screenshot of a real desktop can say "there
 * is a ramp"; it cannot say the ramp has five bands rather than four smooth
 * segments, that band three starts at 0.4 rather than 0.375, or that a
 * seventeen-stop gradient did not silently truncate at eight. Those are exact
 * integers when AVK renders into a UNORM target and the pixels are read back,
 * and every expectation below is one.
 *
 * WHAT IS BEING COMPARED AGAINST. SceneFX's gradient.frag, traced in
 * docs/avk-effects.md. Two of its properties drive the whole design:
 *
 *   - stop positions are IMPLICIT. There is no position array anywhere in the
 *     source; spacing is derived from the colour count alone.
 *   - `blend` chooses BANDS or INTERPOLATION, and nothing else. The same
 *     colours therefore land in different PLACES in the two modes, so every
 *     fixture names which one it means.
 *
 * Exits 77 (skip) with no GPU.
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

static struct avk_image *make_target(struct avk_device *dev) {
	/* avk_image_alloc, not calloc: a bare allocation leaves `life` at 0, which
	 * avk_image_destroy() correctly refuses to act on -- so the target and its
	 * view survive teardown and validation reports them leaked. */
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
	/* Counted here because this creates the objects by hand rather than going
	 * through AVK's own image path. Without it the destroy decrements a
	 * counter nothing incremented, and the shutdown check reports -1 -- which
	 * it reads as a DOUBLE DESTRUCTION, the loudest thing it knows. */
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

/* B8G8R8A8 read as a little-endian uint32 is 0xAARRGGBB. */
static uint32_t px(struct harness *h, uint32_t x, uint32_t y) {
	return h->pixels[y * W + x];
}
static int r_of(uint32_t p) { return (int)((p >> 16) & 0xFF); }
static int g_of(uint32_t p) { return (int)((p >> 8) & 0xFF); }
static int b_of(uint32_t p) { return (int)(p & 0xFF); }

static bool render(struct harness *h, struct avk_scene *scene) {
	uint64_t value = avk_render_frame(&h->renderer, h->target, scene, NULL, 0,
		NULL, 0);
	if (value == 0) {
		return false;
	}
	return avk_device_timeline_wait(h->dev, value, 2000000000ULL)
		&& readback(h);
}

static void scene_begin(struct avk_scene *scene) {
	avk_scene_init(scene);
	pixman_region32_union_rect(&scene->damage, &scene->damage, 0, 0, W, H);
	/* Opaque black underneath, so a gradient that fails to draw reads as
	 * black rather than as whatever the previous test left behind. */
	scene->has_clear = true;
	scene->clear_color[3] = 1.0f;
}

/*
 * A deterministic colour for stop `j` of gradient `i`, PREMULTIPLIED and
 * opaque so the readback is exact rather than within a tolerance.
 *
 * Both indices are recoverable from the pixel: red identifies the gradient and
 * green the stop. That is what makes "gradient 4 is showing gradient 5's
 * colours" a readable failure rather than a colour that is merely wrong.
 */
static void stop_color(int i, int j, float out[4]) {
	out[0] = (float)(16 + i * 12) / 255.0f;
	out[1] = (float)(16 + j * 13) / 255.0f;
	out[2] = 128.0f / 255.0f;
	out[3] = 1.0f;
}
static int stop_r(int i) { return 16 + i * 12; }
static int stop_g(int j) { return 16 + j * 13; }

/* ── M4C.1: the packing architecture ────────────────────────────────────── */

/*
 * Several gradients of DIFFERENT COLOUR COUNTS in one frame, each asserted to
 * be showing its own colours.
 *
 * This is the test the storage design exists for. The GLES reference holds
 * stops in a uniform array with a compile-time length and relinks its program
 * when a scene wants more than the build allows; anything that inherits that
 * shape breaks at a particular count -- eight, sixteen -- and renders every
 * smaller gradient perfectly. So the counts here straddle those numbers, and
 * the seventeen-stop case is not decoration.
 *
 * What is checked is the INDEXING, not the ramp: M4C.1's shader resolves every
 * gradient to its first stop, so each band must show exactly its own gradient's
 * first colour. A record pointed at the wrong run of colours shows a
 * neighbour's, which is what AZ_GRADIENT_COLOR_OFFSET=1 makes happen.
 */
static const int packing_counts[] = { 1, 2, 3, 5, 9, 17 };
#define PACKING_N ((int)(sizeof(packing_counts) / sizeof(packing_counts[0])))

static void add_gradient(struct avk_scene *scene, int i, int count,
		struct avk_box box, enum avk_gradient_type type, bool blend,
		float degree, const float origin[2]) {
	float colors[17 * 4];
	for (int j = 0; j < count; j++) {
		stop_color(i, j, colors + j * 4);
	}
	struct avk_cmd *cmd = avk_scene_add(scene, AVK_CMD_RECT);
	cmd->dst = box;
	cmd->color[3] = 1.0f;
	avk_cmd_set_gradient(scene, cmd, type, degree, blend, origin, colors,
		(uint32_t)count);
}

static void test_packing(struct harness *h) {
	printf("M4C.1 test 1: six gradients, counts 1/2/3/5/9/17, one frame\n");

	struct avk_gradient_stats before = h->renderer.gradients.stats;

	struct avk_scene scene;
	scene_begin(&scene);
	const float origin[2] = { 0.5f, 0.5f };
	for (int i = 0; i < PACKING_N; i++) {
		struct avk_box box = { 0, i * 20, W, 20 };
		add_gradient(&scene, i, packing_counts[i], box, AVK_GRADIENT_LINEAR,
			false, 0.0f, origin);
	}
	CHECK(render(h, &scene), "rendered");

	int total_colors = 0;
	for (int i = 0; i < PACKING_N; i++) {
		total_colors += packing_counts[i];
		uint32_t p = px(h, W / 2, (uint32_t)(i * 20 + 10));
		/* The first stop of gradient i, and nobody else's. */
		CHECK(r_of(p) == stop_r(i) && g_of(p) == stop_g(0) && b_of(p) == 128,
			"gradient %d (%2d stops) shows its own first colour "
			"(%d,%d,%d), got (%d,%d,%d)", i, packing_counts[i], stop_r(i),
			stop_g(0), 128, r_of(p), g_of(p), b_of(p));
	}
	avk_scene_finish(&scene);

	struct avk_gradient_stats *s = &h->renderer.gradients.stats;
	CHECK(s->draws - before.draws == PACKING_N,
		"%d gradient draws recorded", PACKING_N);
	CHECK(s->linear_draws - before.linear_draws == PACKING_N,
		"all %d counted as linear", PACKING_N);
	CHECK(s->conic_draws - before.conic_draws == 0, "none counted as conic");
	CHECK(s->colors_processed - before.colors_processed
			== (uint64_t)total_colors,
		"%d colours packed", total_colors);
	/* PREMISE. If the frame uploaded nothing, every assertion above was
	 * scored against a buffer left over from a previous frame. */
	CHECK(s->buffer_uploads - before.buffer_uploads == 1,
		"the frame uploaded its gradient data exactly once");
	CHECK(s->buffer_upload_bytes - before.buffer_upload_bytes
			== (uint64_t)(PACKING_N * 2 + total_colors) * 16,
		"%d vec4s written (%d records + %d colours)",
		PACKING_N * 2 + total_colors, PACKING_N * 2, total_colors);
}

/*
 * Growth, and then no growth.
 *
 * A frame far larger than the initial capacity must allocate exactly once --
 * geometric, not one buffer per gradient -- and the frames after it must reuse
 * what that allocation left behind. The second half is the half that matters:
 * a store that grew every frame would pass the first assertion and quietly
 * allocate a buffer per frame forever.
 */
#define BIG_GRADIENTS 20

static void test_growth(struct harness *h) {
	printf("M4C.1 test 2: one growth for a large frame, none after\n");

	struct avk_gradient_stats before = h->renderer.gradients.stats;
	const float origin[2] = { 0.5f, 0.5f };

	/* 20 x (2 record + 17 colours) = 380 vec4s, well past the 128 a slot
	 * starts with, so this cannot pass by fitting. */
	struct avk_scene scene;
	scene_begin(&scene);
	for (int i = 0; i < BIG_GRADIENTS; i++) {
		struct avk_box box = { (int32_t)(i % 8) * 16,
			(int32_t)(i / 8) * 16, 16, 16 };
		add_gradient(&scene, i % PACKING_N, 17, box, AVK_GRADIENT_LINEAR,
			false, 0.0f, origin);
	}
	CHECK(render(h, &scene), "rendered the large frame");
	avk_scene_finish(&scene);

	uint64_t grows_after_big = h->renderer.gradients.stats.buffer_grows
		- before.buffer_grows;
	CHECK(grows_after_big == 1,
		"the large frame allocated exactly one buffer, not one per gradient "
		"(got %" PRIu64 ")", grows_after_big);

	/* Now several smaller frames into the SAME ring slot and its neighbours.
	 * AVK_FRAMES_IN_FLIGHT slots exist, so the first pass around the ring
	 * grows each of them once and nothing after that may grow at all. */
	for (int pass = 0; pass < 2 * AVK_FRAMES_IN_FLIGHT; pass++) {
		scene_begin(&scene);
		for (int i = 0; i < PACKING_N; i++) {
			struct avk_box box = { 0, i * 20, W, 20 };
			add_gradient(&scene, i, packing_counts[i], box,
				AVK_GRADIENT_LINEAR, false, 0.0f, origin);
		}
		if (!render(h, &scene)) {
			CHECK(false, "rendered small frame %d", pass);
		}
		avk_scene_finish(&scene);
		if (pass == AVK_FRAMES_IN_FLIGHT - 1) {
			/* Every slot has now seen the big frame's demand at least once. */
			before.buffer_grows = h->renderer.gradients.stats.buffer_grows;
			before.buffer_reuses = h->renderer.gradients.stats.buffer_reuses;
		}
	}
	uint64_t grows_steady = h->renderer.gradients.stats.buffer_grows
		- before.buffer_grows;
	uint64_t reuses_steady = h->renderer.gradients.stats.buffer_reuses
		- before.buffer_reuses;
	CHECK(grows_steady == 0,
		"steady state allocated nothing (got %" PRIu64 " growths)",
		grows_steady);
	CHECK(reuses_steady == AVK_FRAMES_IN_FLIGHT,
		"every steady-state frame reused its slot's buffer (%" PRIu64 "/%d)",
		reuses_steady, AVK_FRAMES_IN_FLIGHT);

	/* And the pixels are still right after all that, which is what says the
	 * growth did not leave a slot pointing at a retired buffer. */
	for (int i = 0; i < PACKING_N; i++) {
		uint32_t p = px(h, W / 2, (uint32_t)(i * 20 + 10));
		CHECK(r_of(p) == stop_r(i) && g_of(p) == stop_g(0),
			"gradient %d still correct after growth and reuse", i);
	}
}

/*
 * Resource churn, stated as Vulkan objects rather than as bytes.
 *
 * The requirement is not that nothing is uploaded -- a per-frame slot is
 * written every frame that draws a gradient, and that is by design. It is that
 * no Vulkan RESOURCE is created on the frame path. The device's live-object
 * counters are what say so, and they count the whole device, so a pipeline or a
 * descriptor pool created anywhere would show up here.
 */
static void test_no_churn(struct harness *h) {
	printf("M4C.1 test 3: a steady gradient scene allocates no Vulkan "
		"objects\n");

	const float origin[2] = { 0.5f, 0.5f };
	struct avk_scene scene;

	/* Warm up past the ring, so first-time-per-slot work is behind us. */
	for (int pass = 0; pass < AVK_FRAMES_IN_FLIGHT; pass++) {
		scene_begin(&scene);
		add_gradient(&scene, 0, 5, (struct avk_box){ 8, 8, 112, 112 },
			AVK_GRADIENT_LINEAR, true, 45.0f, origin);
		render(h, &scene);
		avk_scene_finish(&scene);
	}

	struct avk_live_objects before = h->dev->live;
	uint64_t uploads_before = h->renderer.gradients.stats.buffer_uploads;

	for (int pass = 0; pass < 30; pass++) {
		scene_begin(&scene);
		add_gradient(&scene, 0, 5, (struct avk_box){ 8, 8, 112, 112 },
			AVK_GRADIENT_LINEAR, true, 45.0f, origin);
		render(h, &scene);
		avk_scene_finish(&scene);
	}

	struct avk_live_objects after = h->dev->live;
	CHECK(after.buffers == before.buffers,
		"no VkBuffer created in 30 frames (%" PRId64 " -> %" PRId64 ")",
		before.buffers, after.buffers);
	CHECK(after.device_memory == before.device_memory,
		"no VkDeviceMemory allocated in 30 frames");
	CHECK(after.descriptor_pools == before.descriptor_pools,
		"no VkDescriptorPool created in 30 frames");
	CHECK(after.pipelines == before.pipelines,
		"no VkPipeline created in 30 frames -- and in particular not one per "
		"stop count");
	/* PREMISE, and the reason the four above are not vacuous: the frames did
	 * happen and they did carry gradient data. */
	CHECK(h->renderer.gradients.stats.buffer_uploads - uploads_before == 30,
		"all 30 frames uploaded gradient data");
}

/* ── main ───────────────────────────────────────────────────────────────── */

int main(void) {
	int drm_fd = -1;
	for (int i = 128; i < 132 && drm_fd < 0; i++) {
		char path[64];
		snprintf(path, sizeof(path), "/dev/dri/renderD%d", i);
		drm_fd = open(path, O_RDWR | O_CLOEXEC);
	}
	if (drm_fd < 0) {
		SKIP("no DRM render node");
	}

	struct harness h = {0};
	h.inst = avk_instance_create("test-avk-gradient");
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
	h.target = make_target(h.dev);
	if (h.target == NULL) {
		CHECK(false, "output target allocates");
		goto done;
	}

	test_packing(&h);
	test_growth(&h);
	test_no_churn(&h);

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
