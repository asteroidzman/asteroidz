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
#include <math.h>
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
static bool near_i(int got, int want, int tol) {
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
		/* x = 2 is inside band 0 for every count here -- the narrowest band is
		 * 128/17 = 7.5px wide -- so this reads the FIRST stop whatever the
		 * gradient's length. Sampling the middle would read a different band
		 * per gradient and make the expected value a function of the thing
		 * under test. */
		uint32_t p = px(h, 2, (uint32_t)(i * 20 + 10));
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
		uint32_t p = px(h, 2, (uint32_t)(i * 20 + 10));
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

/* ── M4C.2: the reference formula, on the CPU ───────────────────────────── */

/*
 * An INDEPENDENT implementation of SceneFX's gradient.frag, in C.
 *
 * This is the oracle the renderer is compared against, and it is written from
 * the reference shader rather than from AVK's -- otherwise it would agree with
 * whatever AVK does and prove nothing. Line for line:
 *
 *     vec2 normal = (gl_FragCoord.xy - grad_box)/size;
 *     vec2 uv = normal - origin;
 *     uv *= vec2(1.0)/vec2(abs(cos(rad)) + abs(sin(rad)));
 *     rotated = (uv.x*cos - uv.y*sin + origin.x, ...);
 *     step = rotated.x;
 *
 * TWO DELIBERATE DIVERGENCES, both of them the reference reading out of range:
 *
 *   - the banded path's `int(step/smooth_fac)` is unbounded, and step leaves
 *     0..1 whenever the origin offsets the ramp;
 *   - the blend path reads colors[ind + 1] under `if (ind <= count - 1)`,
 *     which is true on the last segment.
 *
 * Both are clamped here, exactly as AVK clamps them, and the comparison is
 * therefore against the reference's INTENDED semantics rather than against
 * whatever its driver leaves one element past the array. That divergence is
 * named in docs/avk-effects.md as REFERENCE BUG FIXED IN AVK.
 */
static float ref_step_linear(float fx, float fy, const struct avk_box *box,
		const float origin[2], float degree) {
	float nx = (fx - (float)box->x) / (float)box->width;
	float ny = (fy - (float)box->y) / (float)box->height;
	float ux = nx - origin[0];
	float uy = ny - origin[1];
	float rad = degree * (float)(3.14159265358979323846 / 180.0);
	float k = 1.0f / (fabsf(cosf(rad)) + fabsf(sinf(rad)));
	ux *= k;
	uy *= k;
	return ux * cosf(rad) - uy * sinf(rad) + origin[0];
}

static float ref_smoothstep(float e0, float e1, float x) {
	float t = (x - e0) / (e1 - e0);
	t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
	return t * t * (3.0f - 2.0f * t);
}

static void ref_color(const float *colors, int count, bool blend, float step_,
		float out[4]) {
	if (count <= 0) {
		out[0] = out[1] = out[2] = out[3] = 0.0f;
		return;
	}
	if (count == 1) {
		memcpy(out, colors, 4 * sizeof(float));
		return;
	}
	if (!blend) {
		float sf = 1.0f / (float)count;
		int ind = (int)floorf(step_ / sf);
		if (ind < 0) ind = 0;
		if (ind > count - 1) ind = count - 1;
		memcpy(out, colors + ind * 4, 4 * sizeof(float));
		return;
	}
	float sf = 1.0f / (float)(count - 1);
	int ind = (int)floorf(step_ / sf);
	if (ind < 0) ind = 0;
	if (ind > count - 1) ind = count - 1;
	float at = (float)ind * sf;

	float c[4];
	memcpy(c, colors + ind * 4, sizeof(c));
	if (ind > 0) {
		float t = ref_smoothstep(at - sf, at, step_);
		for (int i = 0; i < 4; i++) {
			c[i] = colors[(ind - 1) * 4 + i] + (c[i] - colors[(ind - 1) * 4 + i]) * t;
		}
	}
	int next = ind + 1 > count - 1 ? count - 1 : ind + 1;
	float t = ref_smoothstep(at, at + sf, step_);
	for (int i = 0; i < 4; i++) {
		out[i] = c[i] + (colors[next * 4 + i] - c[i]) * t;
	}
}

/*
 * One gradient filling the whole target, compared against the oracle at every
 * fourth pixel, reported as max and mean channel error.
 *
 * Numbers rather than "looks close": a wrong interpolation curve, a ramp
 * rotated the wrong way or a spacing derived from count instead of count-1 all
 * produce a picture that is recognisably a gradient, and only the error tells
 * them apart. The tolerance is 2/255 -- enough for fp32 on two different
 * evaluators and for UNORM rounding, and far tighter than any of those
 * mistakes.
 */
#define GRAD_TOL 2

/* The conic coordinate, from the same reference shader:
 *     uv = rotate(normal - origin, rad)
 *     step = -atan(uv.y, uv.x)/PI * 0.5 + 0.5
 * No 1/(|cos| + |sin|) scale and no origin add-back -- both are linear-only,
 * and putting either one here is how conic quietly becomes linear-with-extra-
 * steps. */
static float ref_step_conic(float fx, float fy, const struct avk_box *box,
		const float origin[2], float degree) {
	float nx = (fx - (float)box->x) / (float)box->width;
	float ny = (fy - (float)box->y) / (float)box->height;
	float ux = nx - origin[0];
	float uy = ny - origin[1];
	float rad = degree * (float)(3.14159265358979323846 / 180.0);
	float rx = ux * cosf(rad) - uy * sinf(rad);
	float ry = ux * sinf(rad) + uy * cosf(rad);
	return -atan2f(ry, rx) / 3.14159265f * 0.5f + 0.5f;
}

struct grad_case {
	const char *name;
	int count;
	const float *colors;
	bool blend;
	float degree;
	float origin[2];
	/* Opaque background the gradient composites over, premultiplied. */
	float bg[4];
	/* AVK_GRADIENT_LINEAR when left zero -- the enum's 0 is NONE, which no
	 * case here means, so the zero-initialised majority read as linear. */
	enum avk_gradient_type type;
};

static void run_case(struct harness *h, const struct grad_case *c) {
	struct avk_box box = { 0, 0, W, H };
	enum avk_gradient_type type = c->type == AVK_GRADIENT_NONE
		? AVK_GRADIENT_LINEAR : c->type;

	struct avk_scene scene;
	avk_scene_init(&scene);
	pixman_region32_union_rect(&scene.damage, &scene.damage, 0, 0, W, H);
	scene.has_clear = true;
	memcpy(scene.clear_color, c->bg, sizeof(scene.clear_color));

	struct avk_cmd *cmd = avk_scene_add(&scene, AVK_CMD_RECT);
	cmd->dst = box;
	cmd->color[3] = 1.0f;
	avk_cmd_set_gradient(&scene, cmd, type, c->degree, c->blend,
		c->origin, c->colors, (uint32_t)c->count);

	if (!render(h, &scene)) {
		CHECK(false, "%s: rendered", c->name);
		avk_scene_finish(&scene);
		return;
	}
	avk_scene_finish(&scene);

	int max_err = 0, outside = 0, samples = 0;
	long sum_err = 0;
	int worst_x = 0, worst_y = 0;
	for (uint32_t y = 0; y < H; y += 4) {
		for (uint32_t x = 0; x < W; x += 4) {
			float step_ = type == AVK_GRADIENT_CONIC
				? ref_step_conic((float)x + 0.5f, (float)y + 0.5f, &box,
					c->origin, c->degree)
				: ref_step_linear((float)x + 0.5f, (float)y + 0.5f, &box,
					c->origin, c->degree);
			float want[4];
			ref_color(c->colors, c->count, c->blend, step_, want);
			/* Premultiplied source-over onto the clear colour, which is what
			 * the blend state does. */
			float wr = want[0] + c->bg[0] * (1.0f - want[3]);
			float wg = want[1] + c->bg[1] * (1.0f - want[3]);
			float wb = want[2] + c->bg[2] * (1.0f - want[3]);
			uint32_t p = px(h, x, y);
			int e[3] = {
				abs(r_of(p) - (int)lrintf(wr * 255.0f)),
				abs(g_of(p) - (int)lrintf(wg * 255.0f)),
				abs(b_of(p) - (int)lrintf(wb * 255.0f)),
			};
			for (int i = 0; i < 3; i++) {
				samples++;
				sum_err += e[i];
				if (e[i] > max_err) {
					max_err = e[i];
					worst_x = (int)x;
					worst_y = (int)y;
				}
				if (e[i] > GRAD_TOL) {
					outside++;
				}
			}
		}
	}
	printf("      %-28s max %d  mean %.3f  outside(>%d) %d/%d\n", c->name,
		max_err, (double)sum_err / samples, GRAD_TOL, outside, samples);
	CHECK(max_err <= GRAD_TOL, "%s matches the reference formula "
		"(max channel error %d at %d,%d)", c->name, max_err, worst_x, worst_y);
}

/* red, green, blue, yellow, magenta -- opaque, and premultiplied is the same
 * thing for opaque colours. Chosen so that every segment swings a full channel
 * from 0 to 255, which is what makes a wrong interpolation curve visible in
 * whole numbers rather than in ones and twos. */
static const float PALETTE5[20] = {
	1, 0, 0, 1,
	0, 1, 0, 1,
	0, 0, 1, 1,
	1, 1, 0, 1,
	1, 0, 1, 1,
};
static const float PALETTE2[8] = { 1, 0, 0, 1,  0, 0, 1, 1 };
static const float PALETTE3[12] = { 1, 0, 0, 1,  0, 1, 0, 1,  0, 0, 1, 1 };

static void test_linear_oracle(struct harness *h) {
	printf("M4C.2 test 4: linear gradients against the reference formula\n");

	static const float O[2] = { 0.5f, 0.5f };
	const struct grad_case cases[] = {
		{ "2 colours, banded",   2, PALETTE2, false, 0,   { 0.5f, 0.5f } },
		{ "2 colours, smooth",   2, PALETTE2, true,  0,   { 0.5f, 0.5f } },
		{ "3 colours, banded",   3, PALETTE3, false, 0,   { 0.5f, 0.5f } },
		{ "3 colours, smooth",   3, PALETTE3, true,  0,   { 0.5f, 0.5f } },
		{ "5 colours, banded",   5, PALETTE5, false, 0,   { 0.5f, 0.5f } },
		{ "5 colours, smooth",   5, PALETTE5, true,  0,   { 0.5f, 0.5f } },
		/* Degrees. 0 and 90 are the axes; 45 and 135 exercise the
		 * 1/(|cos|+|sin|) scale, which is 1 at the axes and cannot be wrong
		 * there; 180 and 270 catch a sign error that mirrors the ramp. */
		{ "5 smooth, degree 45",  5, PALETTE5, true,  45,  { 0.5f, 0.5f } },
		{ "5 smooth, degree 90",  5, PALETTE5, true,  90,  { 0.5f, 0.5f } },
		{ "5 smooth, degree 135", 5, PALETTE5, true,  135, { 0.5f, 0.5f } },
		{ "5 smooth, degree 180", 5, PALETTE5, true,  180, { 0.5f, 0.5f } },
		{ "5 smooth, degree 270", 5, PALETTE5, true,  270, { 0.5f, 0.5f } },
		{ "5 banded, degree 45",  5, PALETTE5, false, 45,  { 0.5f, 0.5f } },
	};
	(void)O;
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		run_case(h, &cases[i]);
	}
}

/*
 * Banded mode has BOUNDARIES, not ramps.
 *
 * The oracle above would pass a renderer that interpolated slightly, because
 * the error is averaged over a grid that mostly lands inside bands. This looks
 * at the two pixels either side of a boundary and requires both to be exactly
 * one of the two pure colours.
 *
 * THE HALF PIXEL IS NOT OPTIONAL. gl_FragCoord.x is x + 0.5, so band k+1 begins
 * at the first x with x + 0.5 >= 128(k+1)/5, i.e. ceil(128(k+1)/5 - 0.5).
 * Dropping the 0.5 puts the sample one pixel late at two of the four
 * boundaries -- both readings land inside the SAME band, the assertion fails,
 * and it looks exactly like a renderer that smeared the boundary.
 */
static void test_banded_is_banded(struct harness *h) {
	printf("M4C.2 test 5: five hard bands, checked at their edges\n");

	const struct grad_case c = { "banded edges", 5, PALETTE5, false, 0,
		{ 0.5f, 0.5f }, { 0, 0, 0, 1 } };
	struct avk_scene scene;
	avk_scene_init(&scene);
	pixman_region32_union_rect(&scene.damage, &scene.damage, 0, 0, W, H);
	scene.has_clear = true;
	scene.clear_color[3] = 1.0f;
	struct avk_cmd *cmd = avk_scene_add(&scene, AVK_CMD_RECT);
	cmd->dst = (struct avk_box){ 0, 0, W, H };
	cmd->color[3] = 1.0f;
	avk_cmd_set_gradient(&scene, cmd, AVK_GRADIENT_LINEAR, c.degree, c.blend,
		c.origin, c.colors, 5);
	CHECK(render(h, &scene), "rendered");
	avk_scene_finish(&scene);

	static const int want[5][3] = {
		{ 255, 0, 0 }, { 0, 255, 0 }, { 0, 0, 255 },
		{ 255, 255, 0 }, { 255, 0, 255 },
	};
	/* Band centres: step = (k + 0.5)/5. */
	for (int k = 0; k < 5; k++) {
		int x = (int)(128.0 * (k + 0.5) / 5.0 - 0.5);
		uint32_t p = px(h, (uint32_t)x, H / 2);
		CHECK(r_of(p) == want[k][0] && g_of(p) == want[k][1]
				&& b_of(p) == want[k][2],
			"band %d at x=%d is exactly (%d,%d,%d), got (%d,%d,%d)", k, x,
			want[k][0], want[k][1], want[k][2], r_of(p), g_of(p), b_of(p));
	}
	/* Boundaries: the last pixel of one band and the first of the next, both
	 * pure. An implementation that interpolated at all would land between. */
	for (int k = 0; k < 4; k++) {
		int first = (int)ceil(128.0 * (k + 1) / 5.0 - 0.5);
		uint32_t lo = px(h, (uint32_t)(first - 1), H / 2);
		uint32_t hi = px(h, (uint32_t)first, H / 2);
		bool lo_pure = r_of(lo) == want[k][0] && g_of(lo) == want[k][1]
			&& b_of(lo) == want[k][2];
		bool hi_pure = r_of(hi) == want[k + 1][0] && g_of(hi) == want[k + 1][1]
			&& b_of(hi) == want[k + 1][2];
		CHECK(lo_pure && hi_pure,
			"boundary %d/%d at x=%d is a step, not a ramp: (%d,%d,%d) then "
			"(%d,%d,%d)", k, k + 1, first, r_of(lo), g_of(lo), b_of(lo),
			r_of(hi), g_of(hi), b_of(hi));
	}
}

/*
 * Interpolated mode is SMOOTHSTEP, and the colours sit at the segment
 * ENDPOINTS.
 *
 * Two claims a plain linear ramp would also satisfy, and one it would not:
 *
 *   - colour k lands at step = k/(count-1). If the spacing were derived from
 *     `count` -- the banded rule -- colour 2 would sit at 0.4 instead of 0.5,
 *     and the middle of the rectangle would be a blend rather than pure blue.
 *   - a quarter of the way into a segment, smoothstep(0.25) = 0.15625, not
 *     0.25. On the red-to-green segment that is red = 215 rather than 191:
 *     twenty-four levels apart, which no rounding explains.
 */
static void test_blend_is_smoothstep(struct harness *h) {
	printf("M4C.2 test 6: interpolation is smoothstep between endpoints\n");

	struct avk_scene scene;
	avk_scene_init(&scene);
	pixman_region32_union_rect(&scene.damage, &scene.damage, 0, 0, W, H);
	scene.has_clear = true;
	scene.clear_color[3] = 1.0f;
	struct avk_cmd *cmd = avk_scene_add(&scene, AVK_CMD_RECT);
	cmd->dst = (struct avk_box){ 0, 0, W, H };
	cmd->color[3] = 1.0f;
	const float origin[2] = { 0.5f, 0.5f };
	avk_cmd_set_gradient(&scene, cmd, AVK_GRADIENT_LINEAR, 0.0f, true, origin,
		PALETTE5, 5);
	CHECK(render(h, &scene), "rendered");
	avk_scene_finish(&scene);

	static const int want[5][3] = {
		{ 255, 0, 0 }, { 0, 255, 0 }, { 0, 0, 255 },
		{ 255, 255, 0 }, { 255, 0, 255 },
	};
	/* Endpoints: step = k/4, so gl_FragCoord.x = 32k, i.e. the pixel at
	 * x = 32k - 1 or 32k is within half a pixel of the endpoint. */
	for (int k = 0; k < 5; k++) {
		int x = k == 0 ? 0 : (k == 4 ? W - 1 : 32 * k);
		uint32_t p = px(h, (uint32_t)x, H / 2);
		CHECK(near_i(r_of(p), want[k][0], 6) && near_i(g_of(p), want[k][1], 6)
				&& near_i(b_of(p), want[k][2], 6),
			"colour %d is at step %.2f (x=%d): want (%d,%d,%d), got "
			"(%d,%d,%d)", k, k / 4.0, x, want[k][0], want[k][1], want[k][2],
			r_of(p), g_of(p), b_of(p));
	}

	/* A quarter into the first segment. step = 0.0625 -> x + 0.5 = 8. */
	uint32_t p = px(h, 7, H / 2);
	float t_raw = 7.5f / 128.0f / 0.25f;
	float ss = t_raw * t_raw * (3.0f - 2.0f * t_raw);
	int want_r = (int)lrintf(255.0f * (1.0f - ss));
	int linear_r = (int)lrintf(255.0f * (1.0f - t_raw));
	CHECK(near_i(r_of(p), want_r, GRAD_TOL),
		"a quarter into segment 0 is smoothstep (%d), not linear (%d): got %d",
		want_r, linear_r, r_of(p));
	CHECK(abs(want_r - linear_r) > 10,
		"premise: smoothstep and linear differ by %d levels there, so the "
		"assertion above can fail", abs(want_r - linear_r));
}

/*
 * Seventeen stops.
 *
 * The number is not arbitrary. A uniform-array implementation has a
 * compile-time length -- the GLES reference's default is small and it relinks
 * to grow -- so an inherited design breaks at eight or sixteen and renders
 * every smaller gradient perfectly. Every one of the seventeen band centres is
 * checked, so a gradient that silently stopped at sixteen fails on one band
 * rather than on none.
 */
static void test_seventeen(struct harness *h) {
	printf("M4C.2 test 7: seventeen stops, every band checked\n");

	float colors[17 * 4];
	for (int j = 0; j < 17; j++) {
		colors[j * 4 + 0] = (float)(j * 15) / 255.0f;
		colors[j * 4 + 1] = (float)(255 - j * 15) / 255.0f;
		colors[j * 4 + 2] = (float)((j % 2) * 255) / 255.0f;
		colors[j * 4 + 3] = 1.0f;
	}

	struct avk_scene scene;
	avk_scene_init(&scene);
	pixman_region32_union_rect(&scene.damage, &scene.damage, 0, 0, W, H);
	scene.has_clear = true;
	scene.clear_color[3] = 1.0f;
	struct avk_cmd *cmd = avk_scene_add(&scene, AVK_CMD_RECT);
	cmd->dst = (struct avk_box){ 0, 0, W, H };
	cmd->color[3] = 1.0f;
	const float origin[2] = { 0.5f, 0.5f };
	avk_cmd_set_gradient(&scene, cmd, AVK_GRADIENT_LINEAR, 0.0f, false, origin,
		colors, 17);
	CHECK(render(h, &scene), "rendered");
	avk_scene_finish(&scene);

	int wrong = 0;
	for (int k = 0; k < 17; k++) {
		int x = (int)(128.0 * (k + 0.5) / 17.0 - 0.5);
		uint32_t p = px(h, (uint32_t)x, H / 2);
		int wr = k * 15, wg = 255 - k * 15, wb = (k % 2) * 255;
		if (r_of(p) != wr || g_of(p) != wg || b_of(p) != wb) {
			wrong++;
			printf("      band %2d at x=%3d: want (%3d,%3d,%3d) got "
				"(%3d,%3d,%3d)\n", k, x, wr, wg, wb, r_of(p), g_of(p),
				b_of(p));
		}
	}
	CHECK(wrong == 0, "all 17 bands are their own colour");
}

/* One colour, both modes. The interpolated path divides by count - 1, so this
 * is the case that produces a division by zero if it is not special-cased --
 * and a NaN reaching a UNORM target is a colour, not a crash, which is why it
 * needs asserting rather than assuming. */
static void test_single_color(struct harness *h) {
	printf("M4C.2 test 8: a one-stop gradient is a solid colour\n");

	const float one[4] = { 0.25f, 0.5f, 0.75f, 1.0f };
	const float origin[2] = { 0.3f, 0.7f };
	for (int blend = 0; blend < 2; blend++) {
		struct avk_scene scene;
		avk_scene_init(&scene);
		pixman_region32_union_rect(&scene.damage, &scene.damage, 0, 0, W, H);
		scene.has_clear = true;
		scene.clear_color[3] = 1.0f;
		struct avk_cmd *cmd = avk_scene_add(&scene, AVK_CMD_RECT);
		cmd->dst = (struct avk_box){ 0, 0, W, H };
		cmd->color[3] = 1.0f;
		avk_cmd_set_gradient(&scene, cmd, AVK_GRADIENT_LINEAR, 33.0f,
			blend != 0, origin, one, 1);
		CHECK(render(h, &scene), "rendered blend=%d", blend);
		avk_scene_finish(&scene);

		int bad = 0;
		for (uint32_t y = 0; y < H; y += 8) {
			for (uint32_t x = 0; x < W; x += 8) {
				uint32_t p = px(h, x, y);
				if (!near_i(r_of(p), 64, 1) || !near_i(g_of(p), 128, 1)
						|| !near_i(b_of(p), 191, 1)) {
					bad++;
				}
			}
		}
		CHECK(bad == 0, "blend=%d: every pixel is the single colour "
			"(64,128,191), %d wrong", blend, bad);
	}
}

/*
 * The origin, for a LINEAR gradient.
 *
 * It is easy to assume the origin is conic-only, and easy to write a fixture
 * that "proves" it. Expanding the reference gives a constant offset of
 * origin.x*(1 - k*cos) + origin.y*(k*sin) along the ramp, so at 90 degrees the
 * offset is origin.x + origin.y -- meaning (0.2, 0.8) and (0.5, 0.5) produce
 * IDENTICAL output, because both sum to 1. The pair below differ in that sum
 * by 0.5, which is half the ramp.
 */
static void test_origin(struct harness *h) {
	printf("M4C.2 test 9: the origin shifts a linear ramp\n");

	const struct grad_case a = { "origin 0.50,0.50 at 90 deg", 5, PALETTE5,
		true, 90.0f, { 0.5f, 0.5f }, { 0, 0, 0, 1 } };
	const struct grad_case b = { "origin 0.25,0.25 at 90 deg", 5, PALETTE5,
		true, 90.0f, { 0.25f, 0.25f }, { 0, 0, 0, 1 } };
	run_case(h, &a);
	uint32_t centre_a = px(h, W / 2, H / 2);
	run_case(h, &b);
	uint32_t centre_b = px(h, W / 2, H / 2);

	/* PREMISE, and the whole point of the pair: the two really do differ, so
	 * "both match the oracle" is not two runs of the same picture. */
	int delta = abs(r_of(centre_a) - r_of(centre_b))
		+ abs(g_of(centre_a) - g_of(centre_b))
		+ abs(b_of(centre_a) - b_of(centre_b));
	CHECK(delta > 60, "moving the origin changed the picture (centre "
		"(%d,%d,%d) -> (%d,%d,%d), delta %d)",
		r_of(centre_a), g_of(centre_a), b_of(centre_a),
		r_of(centre_b), g_of(centre_b), b_of(centre_b), delta);
}

/*
 * The terminal index, which the reference reads out of range.
 *
 * At the final endpoint the reference's blend path evaluates colors[count].
 * AVK clamps to the last valid colour, so past the end of the ramp must be
 * exactly the last stop and nothing else. An off-centre origin pushes a whole
 * STRIP of the rectangle past it, so this is not one texel's worth of evidence.
 *
 * GETTING THERE NEEDS DEGREE 90, and the first version of this test used 0.
 * At degree 0 the origin's contribution is origin.x*(1 - k*cos) + origin.y*
 * (k*sin) with k = 1, cos = 1, sin = 0 -- identically zero. The ramp ran
 * normally from edge to edge, nothing was ever past its end, and the test
 * reported a clamp failure that was entirely its own. At 90 degrees the offset
 * is origin.x + origin.y, so origin (1.0, 0.5) gives step = 1.5 - normal.y:
 * the top half of the rectangle is past the end, the bottom half is on the
 * ramp.
 */
static void test_terminal_clamp(struct harness *h) {
	printf("M4C.2 test 10: past the end of the ramp is the LAST colour\n");

	for (int count = 2; count <= 5; count += 3) {
		struct avk_scene scene;
		avk_scene_init(&scene);
		pixman_region32_union_rect(&scene.damage, &scene.damage, 0, 0, W, H);
		scene.has_clear = true;
		scene.clear_color[3] = 1.0f;
		struct avk_cmd *cmd = avk_scene_add(&scene, AVK_CMD_RECT);
		cmd->dst = (struct avk_box){ 0, 0, W, H };
		cmd->color[3] = 1.0f;
		const float origin[2] = { 1.0f, 0.5f };
		avk_cmd_set_gradient(&scene, cmd, AVK_GRADIENT_LINEAR, 90.0f, true,
			origin, PALETTE5, (uint32_t)count);
		CHECK(render(h, &scene), "rendered count=%d", count);
		avk_scene_finish(&scene);

		const float *last = PALETTE5 + (count - 1) * 4;
		int lr = (int)lrintf(last[0] * 255.0f);
		int lg = (int)lrintf(last[1] * 255.0f);
		int lb = (int)lrintf(last[2] * 255.0f);
		int bad = 0;
		for (uint32_t y = 0; y < 48; y++) {
			uint32_t p = px(h, W / 2, y);
			if (r_of(p) != lr || g_of(p) != lg || b_of(p) != lb) {
				bad++;
			}
		}
		CHECK(bad == 0, "count=%d: %d/48 rows past the ramp are not the "
			"last colour (%d,%d,%d)", count, bad, lr, lg, lb);
		/* PREMISE. Without this the assertion above is satisfied by a
		 * gradient that is the last colour EVERYWHERE -- which is what the
		 * first-colour break would look like if the palette were reversed, and
		 * what a clamp applied to the whole ramp would look like now. */
		uint32_t bottom = px(h, W / 2, H - 1);
		CHECK(!(r_of(bottom) == lr && g_of(bottom) == lg
				&& b_of(bottom) == lb),
			"count=%d premise: the bottom of the rect is still ON the ramp "
			"(%d,%d,%d), so the clamp above is not vacuous", count,
			r_of(bottom), g_of(bottom), b_of(bottom));
	}
}

/*
 * Alpha, through the same premultiplied blend as everything else.
 *
 * opaque red -> 50%% green -> transparent blue, over white. Note what
 * premultiplication means for the last stop: "transparent blue" IS (0,0,0,0),
 * the blue is gone, and the end of the ramp is the background. That is the
 * reference's behaviour and not a rounding artifact -- there is nowhere in a
 * premultiplied pipeline for the hue of a fully transparent colour to live.
 */
static void test_alpha(struct harness *h) {
	printf("M4C.2 test 11: an alpha gradient over white\n");

	static const float fade[12] = {
		1.0f, 0.0f, 0.0f, 1.0f,     /* opaque red */
		0.0f, 0.5f, 0.0f, 0.5f,     /* 50%% green, premultiplied */
		0.0f, 0.0f, 0.0f, 0.0f,     /* transparent */
	};
	const struct grad_case c = { "alpha ramp over white", 3, fade, true, 0.0f,
		{ 0.5f, 0.5f }, { 1, 1, 1, 1 } };
	run_case(h, &c);

	uint32_t left = px(h, 1, H / 2);
	uint32_t mid = px(h, W / 2, H / 2);
	uint32_t right = px(h, W - 2, H / 2);
	CHECK(r_of(left) == 255 && g_of(left) == 0 && b_of(left) == 0,
		"the opaque end is red, not blended with the background (%d,%d,%d)",
		r_of(left), g_of(left), b_of(left));
	/* 50%% green over white: 0 + 255*0.5 = 128 red and blue, 128 + 128 = 255
	 * green. Getting this wrong in the other direction -- treating the stop as
	 * straight rather than premultiplied -- gives (128, 255, 128) too dark or
	 * too light by exactly its own alpha. */
	CHECK(near_i(r_of(mid), 128, 2) && near_i(g_of(mid), 255, 2)
			&& near_i(b_of(mid), 128, 2),
		"the middle is 50%% green over white = (128,255,128), got (%d,%d,%d)",
		r_of(mid), g_of(mid), b_of(mid));
	CHECK(r_of(right) == 255 && g_of(right) == 255 && b_of(right) == 255,
		"the transparent end is the background (%d,%d,%d)",
		r_of(right), g_of(right), b_of(right));
}

/*
 * Gradient + rounded clipping, with ASYMMETRIC corners.
 *
 * The point is that the material did not bypass the geometry: gradient.frag
 * calls the same az_rounded_coverage() quad.frag does, from the same file, so
 * a gradient rect must be clipped exactly as a solid one is. Asymmetric radii
 * because a single-radius mistake renders a symmetric fixture perfectly.
 */
static void test_rounded(struct harness *h) {
	printf("M4C.2 test 12: gradient composed with asymmetric rounded "
		"corners\n");

	struct avk_scene scene;
	avk_scene_init(&scene);
	pixman_region32_union_rect(&scene.damage, &scene.damage, 0, 0, W, H);
	scene.has_clear = true;
	scene.clear_color[2] = 1.0f;   /* blue background, premultiplied */
	scene.clear_color[3] = 1.0f;

	struct avk_cmd *cmd = avk_scene_add(&scene, AVK_CMD_RECT);
	cmd->dst = (struct avk_box){ 0, 0, W, H };
	cmd->color[3] = 1.0f;
	/* CLOCKWISE tl, tr, br, bl. Only two corners are cut. */
	cmd->corners[0] = 32.0f;
	cmd->corners[1] = 0.0f;
	cmd->corners[2] = 32.0f;
	cmd->corners[3] = 0.0f;
	const float origin[2] = { 0.5f, 0.5f };
	avk_cmd_set_gradient(&scene, cmd, AVK_GRADIENT_LINEAR, 0.0f, false, origin,
		PALETTE5, 5);
	CHECK(render(h, &scene), "rendered");
	avk_scene_finish(&scene);

	/* The two cut corners show the background; the two square ones do not. */
	uint32_t tl = px(h, 1, 1), tr = px(h, W - 2, 1);
	uint32_t br = px(h, W - 2, H - 2), bl = px(h, 1, H - 2);
	CHECK(b_of(tl) == 255 && r_of(tl) == 0,
		"top-left is cut away (%d,%d,%d)", r_of(tl), g_of(tl), b_of(tl));
	CHECK(b_of(br) == 255 && r_of(br) == 0,
		"bottom-right is cut away (%d,%d,%d)", r_of(br), g_of(br), b_of(br));
	CHECK(r_of(tr) == 255 && b_of(tr) == 255 && g_of(tr) == 0,
		"top-right is square and shows the last band, magenta (%d,%d,%d)",
		r_of(tr), g_of(tr), b_of(tr));
	CHECK(r_of(bl) == 255 && g_of(bl) == 0 && b_of(bl) == 0,
		"bottom-left is square and shows the first band, red (%d,%d,%d)",
		r_of(bl), g_of(bl), b_of(bl));
	/* And the ramp is still a ramp inside the shape. */
	uint32_t centre = px(h, W / 2, H / 2);
	CHECK(b_of(centre) == 255 && r_of(centre) == 0 && g_of(centre) == 0,
		"the middle band is still blue inside the rounded shape (%d,%d,%d)",
		r_of(centre), g_of(centre), b_of(centre));
}

/* ── M4C.3: conic ───────────────────────────────────────────────────────── */

/* Four colours so the quadrants of a centred conic gradient are exactly the
 * four bands, which makes the angular mapping something to read off rather
 * than to average. */
static const float PALETTE4[16] = {
	1, 0, 0, 1,     /* red     */
	0, 1, 0, 1,     /* green   */
	0, 0, 1, 1,     /* blue    */
	1, 1, 0, 1,     /* yellow  */
};
static const int WANT4[4][3] = {
	{ 255, 0, 0 }, { 0, 255, 0 }, { 0, 0, 255 }, { 255, 255, 0 },
};

static void render_conic(struct harness *h, int count, const float *colors,
		bool blend, float degree, const float origin[2]) {
	struct avk_scene scene;
	avk_scene_init(&scene);
	pixman_region32_union_rect(&scene.damage, &scene.damage, 0, 0, W, H);
	scene.has_clear = true;
	scene.clear_color[3] = 1.0f;
	struct avk_cmd *cmd = avk_scene_add(&scene, AVK_CMD_RECT);
	cmd->dst = (struct avk_box){ 0, 0, W, H };
	cmd->color[3] = 1.0f;
	avk_cmd_set_gradient(&scene, cmd, AVK_GRADIENT_CONIC, degree, blend,
		origin, colors, (uint32_t)count);
	if (!render(h, &scene)) {
		CHECK(false, "rendered conic degree %.0f", (double)degree);
	}
	avk_scene_finish(&scene);
}

static void test_conic_oracle(struct harness *h) {
	printf("M4C.3 test 13: conic gradients against the reference formula\n");

	const struct grad_case cases[] = {
		{ "conic 4, banded",       4, PALETTE4, false, 0,   { 0.5f, 0.5f },
			{ 0, 0, 0, 1 }, AVK_GRADIENT_CONIC },
		{ "conic 4, smooth",       4, PALETTE4, true,  0,   { 0.5f, 0.5f },
			{ 0, 0, 0, 1 }, AVK_GRADIENT_CONIC },
		{ "conic 5, smooth",       5, PALETTE5, true,  0,   { 0.5f, 0.5f },
			{ 0, 0, 0, 1 }, AVK_GRADIENT_CONIC },
		{ "conic 5, degree 90",    5, PALETTE5, true,  90,  { 0.5f, 0.5f },
			{ 0, 0, 0, 1 }, AVK_GRADIENT_CONIC },
		{ "conic 5, degree 180",   5, PALETTE5, true,  180, { 0.5f, 0.5f },
			{ 0, 0, 0, 1 }, AVK_GRADIENT_CONIC },
		{ "conic 5, degree 270",   5, PALETTE5, true,  270, { 0.5f, 0.5f },
			{ 0, 0, 0, 1 }, AVK_GRADIENT_CONIC },
		{ "conic 5, off-centre",   5, PALETTE5, true,  0,   { 0.25f, 0.70f },
			{ 0, 0, 0, 1 }, AVK_GRADIENT_CONIC },
		{ "conic 17, banded",      0, NULL,     false, 0,   { 0.5f, 0.5f },
			{ 0, 0, 0, 1 }, AVK_GRADIENT_CONIC },
	};
	float many[17 * 4];
	for (int j = 0; j < 17; j++) {
		many[j * 4 + 0] = (float)(j * 15) / 255.0f;
		many[j * 4 + 1] = (float)(255 - j * 15) / 255.0f;
		many[j * 4 + 2] = (float)((j % 2) * 255) / 255.0f;
		many[j * 4 + 3] = 1.0f;
	}
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		struct grad_case c = cases[i];
		if (c.colors == NULL) {
			c.colors = many;
			c.count = 17;
		}
		run_case(h, &c);
	}
}

/*
 * Which way round a conic gradient goes, at four angles.
 *
 * With a centred origin and four banded colours the quadrants ARE the bands, so
 * the mapping can be stated as a table instead of averaged. At degree 0 the
 * coordinate increases left -> down -> right -> up, which on a screen (y
 * downward) is counter-clockwise, so:
 *
 *     lower-left = 0, lower-right = 1, upper-right = 2, upper-left = 3
 *
 * and each 90 degrees moves every band one quadrant counter-clockwise. The
 * table catches all four of the mistakes worth having a test for: a sign error
 * (the sequence reverses), a direction error (the same), a missing degree
 * offset (nothing moves between rows), and PI where 2*PI belongs (the pattern
 * repeats twice round and no quadrant is a single colour).
 */
static void test_conic_degrees(struct harness *h) {
	printf("M4C.3 test 14: the angular mapping, at 0/90/180/270\n");

	/* [degree index][quadrant] -> colour index.
	 * quadrant order: lower-left, lower-right, upper-right, upper-left. */
	static const int expect[4][4] = {
		{ 0, 1, 2, 3 },   /*   0 deg */
		{ 3, 0, 1, 2 },   /*  90 deg */
		{ 2, 3, 0, 1 },   /* 180 deg */
		{ 1, 2, 3, 0 },   /* 270 deg */
	};
	static const uint32_t qx[4] = { 32, 96, 96, 32 };
	static const uint32_t qy[4] = { 96, 96, 32, 32 };
	static const char *qname[4] = { "lower-left", "lower-right",
		"upper-right", "upper-left" };
	const float origin[2] = { 0.5f, 0.5f };

	for (int d = 0; d < 4; d++) {
		float degree = (float)(d * 90);
		render_conic(h, 4, PALETTE4, false, degree, origin);
		for (int q = 0; q < 4; q++) {
			int want = expect[d][q];
			uint32_t p = px(h, qx[q], qy[q]);
			CHECK(r_of(p) == WANT4[want][0] && g_of(p) == WANT4[want][1]
					&& b_of(p) == WANT4[want][2],
				"degree %3.0f: %s is colour %d (%d,%d,%d), got (%d,%d,%d)",
				(double)degree, qname[q], want, WANT4[want][0],
				WANT4[want][1], WANT4[want][2], r_of(p), g_of(p), b_of(p));
		}
	}
}

/*
 * The seam.
 *
 * atan2 flips between +PI and -PI along the -x ray from the origin, so the
 * coordinate jumps from ~1 to ~0 across it and the ramp meets its own
 * beginning. Neither mode interpolates over that: banded steps from the last
 * band to the first, and interpolated does too, because its last segment ENDS
 * at the last colour rather than wrapping.
 *
 * Both are checked, because "smooth mode is smooth everywhere" is the natural
 * assumption and it is wrong. The rows either side of the ray must be the last
 * and first colours exactly, in both modes.
 */
static void test_conic_seam(struct harness *h) {
	printf("M4C.3 test 15: the wrap seam is a step, in both modes\n");

	const float origin[2] = { 0.5f, 0.5f };
	/* uv.y = 0 at y + 0.5 = 64, so rows 63 and 64 straddle the ray, and it
	 * runs leftward from the centre -- sample well to the left of it. */
	for (int blend = 0; blend < 2; blend++) {
		render_conic(h, 4, PALETTE4, blend != 0, 0.0f, origin);
		uint32_t above = px(h, 8, 63);
		uint32_t below = px(h, 8, 64);
		CHECK(r_of(above) == WANT4[3][0] && g_of(above) == WANT4[3][1]
				&& b_of(above) == WANT4[3][2],
			"blend=%d: just above the seam is the LAST colour (%d,%d,%d), got "
			"(%d,%d,%d)", blend, WANT4[3][0], WANT4[3][1], WANT4[3][2],
			r_of(above), g_of(above), b_of(above));
		CHECK(r_of(below) == WANT4[0][0] && g_of(below) == WANT4[0][1]
				&& b_of(below) == WANT4[0][2],
			"blend=%d: just below it is the FIRST colour (%d,%d,%d), got "
			"(%d,%d,%d)", blend, WANT4[0][0], WANT4[0][1], WANT4[0][2],
			r_of(below), g_of(below), b_of(below));
	}
}

/*
 * An off-centre origin moves the CENTRE of rotation, and the seam with it.
 *
 * This is the fixture a shader hard-coded to {0.5, 0.5} must fail, and it is
 * worth being precise about why the obvious version would not: every gradient
 * asteroidz itself creates passes {0.5, 0.5}, so a centred shader renders the
 * whole desktop correctly and only a synthetic origin can show it.
 *
 * With origin (0.25, 0.70) the seam runs leftward from y = 0.70*128 - 0.5 =
 * 89.1, so it falls between rows 89 and 90 rather than 63 and 64 -- and to the
 * left of x = 0.25*128 - 0.5 = 31.5.
 */
static void test_conic_origin(struct harness *h) {
	printf("M4C.3 test 16: an off-centre origin moves the pattern's centre\n");

	const float origin[2] = { 0.25f, 0.70f };
	render_conic(h, 4, PALETTE4, false, 0.0f, origin);

	uint32_t above = px(h, 8, 89);
	uint32_t below = px(h, 8, 90);
	CHECK(r_of(above) == WANT4[3][0] && g_of(above) == WANT4[3][1]
			&& b_of(above) == WANT4[3][2],
		"the seam sits at the origin's row: y=89 is the last colour "
		"(%d,%d,%d)", r_of(above), g_of(above), b_of(above));
	CHECK(r_of(below) == WANT4[0][0] && g_of(below) == WANT4[0][1]
			&& b_of(below) == WANT4[0][2],
		"y=90 is the first colour (%d,%d,%d)", r_of(below), g_of(below),
		b_of(below));
	/* And NOT at the centre, which is where a hard-coded origin would put it.
	 * Rows 63 and 64 must be the same colour as each other there. */
	uint32_t c63 = px(h, 8, 63), c64 = px(h, 8, 64);
	CHECK(r_of(c63) == r_of(c64) && g_of(c63) == g_of(c64)
			&& b_of(c63) == b_of(c64),
		"there is no seam at the rectangle's centre: y=63 (%d,%d,%d) equals "
		"y=64 (%d,%d,%d)", r_of(c63), g_of(c63), b_of(c63),
		r_of(c64), g_of(c64), b_of(c64));
}

/*
 * Conic and linear are DIFFERENT PICTURES from the same inputs.
 *
 * Stated as a test because "do not collapse conic into linear" is the kind of
 * requirement that is met by accident and lost by accident. The same four
 * colours, the same count, the same angle, the same origin, the same box: a
 * linear ramp has no variation along y at degree 0 and a conic one is nothing
 * but variation along y.
 */
static void test_conic_is_not_linear(struct harness *h) {
	printf("M4C.3 test 17: conic is not linear with a different scale\n");

	const float origin[2] = { 0.5f, 0.5f };
	render_conic(h, 4, PALETTE4, false, 0.0f, origin);
	uint32_t conic_top = px(h, 100, 8);
	uint32_t conic_bottom = px(h, 100, 120);

	struct avk_scene scene;
	avk_scene_init(&scene);
	pixman_region32_union_rect(&scene.damage, &scene.damage, 0, 0, W, H);
	scene.has_clear = true;
	scene.clear_color[3] = 1.0f;
	struct avk_cmd *cmd = avk_scene_add(&scene, AVK_CMD_RECT);
	cmd->dst = (struct avk_box){ 0, 0, W, H };
	cmd->color[3] = 1.0f;
	avk_cmd_set_gradient(&scene, cmd, AVK_GRADIENT_LINEAR, 0.0f, false, origin,
		PALETTE4, 4);
	CHECK(render(h, &scene), "rendered the linear comparison");
	avk_scene_finish(&scene);
	uint32_t linear_top = px(h, 100, 8);
	uint32_t linear_bottom = px(h, 100, 120);

	CHECK(linear_top == linear_bottom,
		"the linear ramp does not vary down the column at degree 0");
	CHECK(conic_top != conic_bottom,
		"the conic one does (%d,%d,%d) vs (%d,%d,%d)", r_of(conic_top),
		g_of(conic_top), b_of(conic_top), r_of(conic_bottom),
		g_of(conic_bottom), b_of(conic_bottom));
	CHECK(h->renderer.gradients.stats.conic_draws > 0,
		"premise: conic draws were counted as conic (%" PRIu64 ")",
		h->renderer.gradients.stats.conic_draws);
}

/* Conic composed with asymmetric rounded corners, so the material/geometry
 * split is checked for the second gradient kind too rather than assumed to
 * carry over. */
static void test_conic_rounded(struct harness *h) {
	printf("M4C.3 test 18: conic composed with asymmetric rounded corners\n");

	struct avk_scene scene;
	avk_scene_init(&scene);
	pixman_region32_union_rect(&scene.damage, &scene.damage, 0, 0, W, H);
	scene.has_clear = true;
	scene.clear_color[0] = 1.0f;   /* white background, premultiplied */
	scene.clear_color[1] = 1.0f;
	scene.clear_color[2] = 1.0f;
	scene.clear_color[3] = 1.0f;

	struct avk_cmd *cmd = avk_scene_add(&scene, AVK_CMD_RECT);
	cmd->dst = (struct avk_box){ 0, 0, W, H };
	cmd->color[3] = 1.0f;
	cmd->corners[0] = 40.0f;   /* tl */
	cmd->corners[1] = 8.0f;    /* tr */
	cmd->corners[2] = 40.0f;   /* br */
	cmd->corners[3] = 8.0f;    /* bl */
	const float origin[2] = { 0.5f, 0.5f };
	avk_cmd_set_gradient(&scene, cmd, AVK_GRADIENT_CONIC, 0.0f, false, origin,
		PALETTE4, 4);
	CHECK(render(h, &scene), "rendered");
	avk_scene_finish(&scene);

	uint32_t tl = px(h, 1, 1), tr = px(h, W - 2, 1);
	CHECK(r_of(tl) == 255 && g_of(tl) == 255 && b_of(tl) == 255,
		"the 40px corner is cut away (%d,%d,%d)", r_of(tl), g_of(tl),
		b_of(tl));
	CHECK(!(r_of(tr) == 255 && g_of(tr) == 255 && b_of(tr) == 255),
		"the 8px corner is not (%d,%d,%d)", r_of(tr), g_of(tr), b_of(tr));
	/* The quadrants are still the quadrants inside the shape. */
	for (int q = 0; q < 4; q++) {
		static const uint32_t qx[4] = { 32, 96, 96, 32 };
		static const uint32_t qy[4] = { 96, 96, 32, 32 };
		uint32_t p = px(h, qx[q], qy[q]);
		CHECK(r_of(p) == WANT4[q][0] && g_of(p) == WANT4[q][1]
				&& b_of(p) == WANT4[q][2],
			"quadrant %d is still colour %d inside the rounded shape "
			"(%d,%d,%d)", q, q, r_of(p), g_of(p), b_of(p));
	}
}

/* ── M4C.4: damage, and the properties that must reach the pixels ───────── */

/*
 * Render `g` into `box`, damaging only `dmg`, without clearing.
 *
 * No scene.has_clear: the target keeps what the previous frame left, which is
 * the whole point -- what survives outside the damaged region, and what must
 * NOT survive inside it.
 */
static bool render_damaged(struct harness *h, const struct grad_case *c,
		const struct avk_box *dmg, bool clear) {
	struct avk_scene scene;
	avk_scene_init(&scene);
	pixman_region32_union_rect(&scene.damage, &scene.damage, dmg->x, dmg->y,
		(unsigned)dmg->width, (unsigned)dmg->height);
	/*
	 * The clear is DAMAGE-CLIPPED, exactly as in the compositor -- it is an
	 * ordinary scissored command, not a full-screen wipe -- so a partial
	 * damage region is cleared and redrawn from the bottom up while everything
	 * outside it survives untouched. That models what really happens to a
	 * damaged rectangle, and it is what makes NODE OPACITY testable: without
	 * it, a half-opaque redraw composites over the same gradient it was
	 * already drawn on (0.5g + g*(1 - 0.5) = g) and the property looks like it
	 * has no effect at all -- which cost two assertions before this comment
	 * existed.
	 */
	if (clear) {
		scene.has_clear = true;
		scene.clear_color[3] = 1.0f;
	}
	struct avk_cmd *cmd = avk_scene_add(&scene, AVK_CMD_RECT);
	cmd->dst = (struct avk_box){ 0, 0, W, H };
	cmd->color[3] = 1.0f;
	cmd->opacity = c->bg[0] > 0.0f ? c->bg[0] : 1.0f;   /* bg[0] carries opacity here */
	avk_cmd_set_gradient(&scene, cmd,
		c->type == AVK_GRADIENT_NONE ? AVK_GRADIENT_LINEAR : c->type,
		c->degree, c->blend, c->origin, c->colors, (uint32_t)c->count);
	bool ok = render(h, &scene);
	avk_scene_finish(&scene);
	return ok;
}

/*
 * ONE PROPERTY AT A TIME, and each one asserted three ways.
 *
 * The matrix exists because a gradient has seven independent inputs and a
 * renderer can drop any one of them while still drawing a convincing gradient
 * -- that is exactly what the first-colour, blend-swap, linear-only and
 * centre-origin breaks each are. So for every property:
 *
 *   1. VISIBLE   changing it alone changes the pixels. A property that cannot
 *                change the picture cannot be damage-correct either, and the
 *                assertion below it would be vacuous.
 *   2. REPAINTED inside a damaged region the new value wins completely --
 *                no pixel of the old gradient survives where it was redrawn.
 *   3. STABLE    outside that region the old content is untouched, and a
 *                third frame with the same inputs changes nothing at all.
 *
 * (3) is the renderer's half of the M4C.3H invariant: a settled gradient must
 * not repaint itself, and must not smear when something else does.
 */
static const float PALETTE5B[20] = {
	0, 1, 1, 1,     /* cyan    */
	1, 0, 1, 1,     /* magenta */
	1, 1, 0, 1,     /* yellow  */
	0, 0, 1, 1,     /* blue    */
	0, 1, 0, 1,     /* green   */
};

struct prop_case {
	const char *name;
	struct grad_case a;
	struct grad_case b;
};

static bool frames_differ(struct harness *h, uint32_t *before, int n) {
	for (int i = 0; i < n; i++) {
		if (h->pixels[i] != before[i]) {
			return true;
		}
	}
	return false;
}

static void test_property_damage(struct harness *h) {
	printf("M4C.4 test 19: property-change damage matrix\n");

	static const float O1[2] = { 0.5f, 0.5f };
	static const float O2[2] = { 0.25f, 0.30f };
	const struct prop_case cases[] = {
		{ "colour contents",
		  { NULL, 5, PALETTE5,  true, 0, { 0.5f, 0.5f }, { 1, 0, 0, 0 } },
		  { NULL, 5, PALETTE5B, true, 0, { 0.5f, 0.5f }, { 1, 0, 0, 0 } } },
		{ "colour count",
		  { NULL, 5, PALETTE5,  true, 0, { 0.5f, 0.5f }, { 1, 0, 0, 0 } },
		  { NULL, 3, PALETTE5,  true, 0, { 0.5f, 0.5f }, { 1, 0, 0, 0 } } },
		{ "gradient type",
		  { NULL, 5, PALETTE5,  true, 0, { 0.5f, 0.5f }, { 1, 0, 0, 0 },
		    AVK_GRADIENT_LINEAR },
		  { NULL, 5, PALETTE5,  true, 0, { 0.5f, 0.5f }, { 1, 0, 0, 0 },
		    AVK_GRADIENT_CONIC } },
		{ "degree",
		  { NULL, 5, PALETTE5,  true, 0,  { 0.5f, 0.5f }, { 1, 0, 0, 0 } },
		  { NULL, 5, PALETTE5,  true, 55, { 0.5f, 0.5f }, { 1, 0, 0, 0 } } },
		{ "origin",
		  { NULL, 5, PALETTE5,  true, 90, { 0.5f, 0.5f },  { 1, 0, 0, 0 } },
		  { NULL, 5, PALETTE5,  true, 90, { 0.25f, 0.30f }, { 1, 0, 0, 0 } } },
		{ "blend mode",
		  { NULL, 5, PALETTE5,  true,  0, { 0.5f, 0.5f }, { 1, 0, 0, 0 } },
		  { NULL, 5, PALETTE5,  false, 0, { 0.5f, 0.5f }, { 1, 0, 0, 0 } } },
		{ "node opacity",
		  { NULL, 5, PALETTE5,  true, 0, { 0.5f, 0.5f }, { 1.0f, 0, 0, 0 } },
		  { NULL, 5, PALETTE5,  true, 0, { 0.5f, 0.5f }, { 0.5f, 0, 0, 0 } } },
	};
	(void)O1; (void)O2;

	const struct avk_box full = { 0, 0, W, H };
	/* The damaged strip, and a column well outside it. */
	const struct avk_box strip = { 0, 0, W / 2, H };
	const uint32_t inside_x = W / 4, outside_x = 3 * W / 4;

	static uint32_t snapshot[W * H];
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		const struct prop_case *pc = &cases[i];

		/* Frame A, whole target. */
		if (!render_damaged(h, &pc->a, &full, true)) {
			CHECK(false, "%s: rendered A", pc->name);
			continue;
		}
		memcpy(snapshot, h->pixels, sizeof(snapshot));
		uint32_t a_out = px(h, outside_x, H / 2);

		/* Frame B, whole target, one property changed.
		 *
		 * COUNTED OVER THE WHOLE FRAME, not read at one pixel. Two of these
		 * properties coincide at any given point -- at x = W/4 a five-stop
		 * gradient is inside band 1 in BOTH blend modes, so a single sample
		 * there reports that changing the mode changed nothing. The property
		 * is visible if the picture differs anywhere. */
		if (!render_damaged(h, &pc->b, &full, true)) {
			CHECK(false, "%s: rendered B", pc->name);
			continue;
		}
		uint32_t b_in = px(h, inside_x, H / 2);
		int changed = 0;
		for (int q = 0; q < W * H; q += 7) {
			if (h->pixels[q] != snapshot[q]) {
				changed++;
			}
		}
		CHECK(changed > 0, "%s: changing it alone changes the picture "
			"(%d of %d sampled pixels)", pc->name, changed, W * H / 7);

		/* Back to A everywhere, then B damaging only the left strip. */
		render_damaged(h, &pc->a, &full, true);
		memcpy(snapshot, h->pixels, sizeof(snapshot));
		if (!render_damaged(h, &pc->b, &strip, true)) {
			CHECK(false, "%s: rendered B into a partial damage region",
				pc->name);
			continue;
		}
		CHECK(px(h, inside_x, H / 2) == b_in,
			"%s: inside the damaged region the NEW gradient won completely "
			"(%06x)", pc->name, px(h, inside_x, H / 2) & 0xFFFFFF);
		CHECK(px(h, outside_x, H / 2) == a_out,
			"%s: outside it the old content is untouched (%06x, was %06x)",
			pc->name, px(h, outside_x, H / 2) & 0xFFFFFF, a_out & 0xFFFFFF);
		/* No stale band anywhere in the damaged strip: every pixel there must
		 * match a full-frame render of B, not a blend of the two. */
		memcpy(snapshot, h->pixels, sizeof(snapshot));
		render_damaged(h, &pc->b, &full, true);
		int stale = 0;
		for (uint32_t y = 0; y < H; y += 4) {
			for (uint32_t x = 0; x < (uint32_t)strip.width; x += 4) {
				if (snapshot[y * W + x] != h->pixels[y * W + x]) {
					stale++;
				}
			}
		}
		CHECK(stale == 0, "%s: no pixel of the old gradient survived inside "
			"the damaged region (%d stale)", pc->name, stale);

		/* STABLE: the same inputs again must change nothing. */
		memcpy(snapshot, h->pixels, sizeof(snapshot));
		render_damaged(h, &pc->b, &full, true);
		CHECK(!frames_differ(h, snapshot, W * H),
			"%s: re-rendering identical inputs changed no pixel", pc->name);
	}
}

/*
 * Output scale must not rotate a gradient or move its bands.
 *
 * The compositor hands AVK a destination box already in output pixels, so
 * "scale 1.5" is the same gradient in a box 1.5x larger. The semantics live in
 * the box's own normalised space, so every band boundary must land at the same
 * FRACTION of the box, and the conic centre at the same fraction -- if any of
 * that were computed against the output instead, a scaled window's gradient
 * would drift.
 */
static void test_scale_invariance(struct harness *h) {
	printf("M4C.4 test 20: gradient semantics are invariant to box scale\n");

	const float origin[2] = { 0.5f, 0.5f };
	/* 80x80 and 120x120: a 1.5x scale, both fitting the target. */
	const int sizes[2] = { 80, 120 };
	int band_at[2][4];
	int conic_q[2][4];

	for (int s = 0; s < 2; s++) {
		int n = sizes[s];
		struct avk_scene scene;
		avk_scene_init(&scene);
		pixman_region32_union_rect(&scene.damage, &scene.damage, 0, 0, W, H);
		scene.has_clear = true;
		scene.clear_color[3] = 1.0f;
		struct avk_cmd *cmd = avk_scene_add(&scene, AVK_CMD_RECT);
		cmd->dst = (struct avk_box){ 0, 0, n, n };
		cmd->color[3] = 1.0f;
		avk_cmd_set_gradient(&scene, cmd, AVK_GRADIENT_LINEAR, 0.0f, false,
			origin, PALETTE5, 5);
		CHECK(render(h, &scene), "rendered linear at %dx%d", n, n);
		avk_scene_finish(&scene);

		/* Where does each band boundary fall, as a percentage of the box? */
		int prev = -1, found = 0;
		for (int x = 0; x < n && found < 4; x++) {
			uint32_t p = px(h, (uint32_t)x, (uint32_t)(n / 2));
			int id = (r_of(p) > 128) * 4 + (g_of(p) > 128) * 2 + (b_of(p) > 128);
			if (prev >= 0 && id != prev) {
				band_at[s][found++] = (int)lrintf(100.0f * x / n);
			}
			prev = id;
		}
		for (; found < 4; found++) {
			band_at[s][found] = -1;
		}

		/* And the conic quadrants, which pin the centre. */
		avk_scene_init(&scene);
		pixman_region32_union_rect(&scene.damage, &scene.damage, 0, 0, W, H);
		scene.has_clear = true;
		scene.clear_color[3] = 1.0f;
		cmd = avk_scene_add(&scene, AVK_CMD_RECT);
		cmd->dst = (struct avk_box){ 0, 0, n, n };
		cmd->color[3] = 1.0f;
		avk_cmd_set_gradient(&scene, cmd, AVK_GRADIENT_CONIC, 0.0f, false,
			origin, PALETTE4, 4);
		CHECK(render(h, &scene), "rendered conic at %dx%d", n, n);
		avk_scene_finish(&scene);
		const int qx[4] = { 1, 3, 3, 1 }, qy[4] = { 3, 3, 1, 1 };
		for (int q = 0; q < 4; q++) {
			uint32_t p = px(h, (uint32_t)(n * qx[q] / 4),
				(uint32_t)(n * qy[q] / 4));
			conic_q[s][q] = (r_of(p) > 128) * 4 + (g_of(p) > 128) * 2
				+ (b_of(p) > 128);
		}
	}

	printf("      band boundaries (%% of box): 80px %d/%d/%d/%d   "
		"120px %d/%d/%d/%d\n", band_at[0][0], band_at[0][1], band_at[0][2],
		band_at[0][3], band_at[1][0], band_at[1][1], band_at[1][2],
		band_at[1][3]);
	int drift = 0;
	for (int i = 0; i < 4; i++) {
		if (band_at[0][i] < 0 || abs(band_at[0][i] - band_at[1][i]) > 1) {
			drift++;
		}
	}
	CHECK(drift == 0, "every band boundary lands at the same fraction of the "
		"box at both scales (%d drifted)", drift);
	int qdiff = 0;
	for (int q = 0; q < 4; q++) {
		if (conic_q[0][q] != conic_q[1][q]) {
			qdiff++;
		}
	}
	CHECK(qdiff == 0, "the conic centre and rotation are unchanged by scale "
		"(%d quadrants differ)", qdiff);
}

/*
 * WHICH SPACE DOES `degree` LIVE IN? Output raster space, not node space.
 *
 * Traced rather than assumed: SceneFX sets range = dst_box, which is in OUTPUT
 * coordinates after the output transform, and the shader derives its
 * coordinate from gl_FragCoord, which is also output. AVK does the same. So a
 * gradient's direction is fixed relative to the OUTPUT, and rotating an output
 * rotates the window underneath a ramp that stays put.
 *
 * That is worth a fixture because the opposite is the intuitive guess, and a
 * renderer that "helpfully" rotated the ramp with the node would look correct
 * on an unrotated output -- which is every output anyone tests on.
 *
 * The check: two boxes of the same size at different POSITIONS must show the
 * same ramp relative to themselves (the gradient belongs to the box, not the
 * screen), while the direction stays along output +x.
 */
static void test_gradient_space(struct harness *h) {
	printf("M4C.4 test 21: the ramp belongs to the box, its direction to the "
		"output\n");

	const float origin[2] = { 0.5f, 0.5f };
	const struct avk_box boxes[2] = { { 0, 0, 64, 64 }, { 64, 64, 64, 64 } };
	int ids[2][3];

	for (int b = 0; b < 2; b++) {
		struct avk_scene scene;
		avk_scene_init(&scene);
		pixman_region32_union_rect(&scene.damage, &scene.damage, 0, 0, W, H);
		scene.has_clear = true;
		scene.clear_color[3] = 1.0f;
		struct avk_cmd *cmd = avk_scene_add(&scene, AVK_CMD_RECT);
		cmd->dst = boxes[b];
		cmd->color[3] = 1.0f;
		avk_cmd_set_gradient(&scene, cmd, AVK_GRADIENT_LINEAR, 0.0f, false,
			origin, PALETTE5, 5);
		CHECK(render(h, &scene), "rendered at %d,%d", boxes[b].x, boxes[b].y);
		avk_scene_finish(&scene);
		/* Left, middle and right THIRDS of the box itself. */
		for (int t = 0; t < 3; t++) {
			uint32_t x = (uint32_t)(boxes[b].x + boxes[b].width * (2 * t + 1) / 6);
			uint32_t y = (uint32_t)(boxes[b].y + boxes[b].height / 2);
			uint32_t p = px(h, x, y);
			ids[b][t] = (r_of(p) > 128) * 4 + (g_of(p) > 128) * 2
				+ (b_of(p) > 128);
		}
	}
	CHECK(ids[0][0] == ids[1][0] && ids[0][1] == ids[1][1]
			&& ids[0][2] == ids[1][2],
		"moving the box does not move the ramp through it "
		"(%d/%d/%d vs %d/%d/%d)", ids[0][0], ids[0][1], ids[0][2],
		ids[1][0], ids[1][1], ids[1][2]);
	/* And it really is a ramp along +x, not a constant. */
	CHECK(ids[0][0] != ids[0][2],
		"premise: the thirds differ, so the comparison above is not three "
		"readings of one colour");
}

/* ── M4C.4: performance baseline ────────────────────────────────────────── */

/*
 * What a gradient costs, measured rather than asserted.
 *
 * GPU TIME IS NOT MEASURED, and is not guessed at. AVK has no timestamp query
 * pool -- avk_phys.c reads timestampPeriod and nothing uses it -- so the only
 * timing available is CPU wall-clock around recording and submitting, which is
 * a different quantity in a different place. Reporting it as GPU cost would
 * understate a shader-bound frame and overstate a submission-bound one, in
 * opposite directions. It is labelled CPU throughout.
 *
 * The numbers that ARE meaningful here: how much data a gradient uploads per
 * frame, how many draws it costs, and whether any of it allocates.
 */
static void perf_scene(struct harness *h, const char *label, int gradients,
		int count, enum avk_gradient_type type, bool tile) {
	const int FRAMES = 60;
	struct avk_renderer_stats s0 = h->renderer.stats;
	struct avk_gradient_stats g0 = h->renderer.gradients.stats;
	struct avk_live_objects l0 = h->dev->live;

	float colors[17 * 4];
	for (int j = 0; j < 17; j++) {
		stop_color(j % 6, j, colors + j * 4);
	}
	const float origin[2] = { 0.5f, 0.5f };

	for (int f = 0; f < FRAMES; f++) {
		struct avk_scene scene;
		avk_scene_init(&scene);
		pixman_region32_union_rect(&scene.damage, &scene.damage, 0, 0, W, H);
		scene.has_clear = true;
		scene.clear_color[3] = 1.0f;
		for (int i = 0; i < gradients; i++) {
			struct avk_cmd *cmd = avk_scene_add(&scene, AVK_CMD_RECT);
			if (tile) {
				int n = 4, side = W / n;
				cmd->dst = (struct avk_box){ (i % n) * side, (i / n) * side,
					side, side };
			} else {
				cmd->dst = (struct avk_box){ 0, 0, W, H };
			}
			cmd->color[0] = 0.2f; cmd->color[1] = 0.4f; cmd->color[2] = 0.8f;
			cmd->color[3] = 1.0f;
			if (count > 0) {
				/* A colour that CHANGES every  frame for the animated case, so the
				 * upload is real work and not a repeated identical write. */
				colors[0] = (float)(f % 64) / 64.0f;
				avk_cmd_set_gradient(&scene, cmd, type, 45.0f, true, origin,
					colors, (uint32_t)count);
			}
		}
		render(h, &scene);
		avk_scene_finish(&scene);
	}

	const struct avk_renderer_stats *s = &h->renderer.stats;
	const struct avk_gradient_stats *g = &h->renderer.gradients.stats;
	const struct avk_live_objects *l = &h->dev->live;
	uint64_t frames = s->frames - s0.frames;
	printf("      %-26s cpu %5" PRIu64 " us/frame  draws %4" PRIu64
		"  upload %5" PRIu64 " B/frame  alloc %" PRId64 "\n",
		label,
		frames ? (s->cpu_record_ns - s0.cpu_record_ns) / frames / 1000 : 0,
		(s->draws - s0.draws) / (frames ? frames : 1),
		frames ? (g->buffer_upload_bytes - g0.buffer_upload_bytes) / frames : 0,
		(l->buffers - l0.buffers) + (l->device_memory - l0.device_memory)
			+ (l->pipelines - l0.pipelines));
	CHECK((l->buffers - l0.buffers) == 0
			&& (l->device_memory - l0.device_memory) == 0
			&& (l->pipelines - l0.pipelines) == 0,
		"%s: allocated no Vulkan objects over %d frames", label, FRAMES);
}

static void test_performance(struct harness *h) {
	printf("M4C.4 test 22: performance baseline (GPU time = NOT MEASURED)\n");
	/* Warm every ring slot first, so first-use work is not charged to the
	 * first scene measured. */
	perf_scene(h, "(warmup)", 1, 5, AVK_GRADIENT_LINEAR, false);
	perf_scene(h, "solid rects, no gradient", 1, 0, AVK_GRADIENT_LINEAR, false);
	perf_scene(h, "5-colour linear", 1, 5, AVK_GRADIENT_LINEAR, false);
	perf_scene(h, "17-colour linear", 1, 17, AVK_GRADIENT_LINEAR, false);
	perf_scene(h, "5-colour conic", 1, 5, AVK_GRADIENT_CONIC, false);
	perf_scene(h, "16 gradient windows", 16, 5, AVK_GRADIENT_LINEAR, true);
	perf_scene(h, "animated 2-stop border", 1, 2, AVK_GRADIENT_LINEAR, false);
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
	test_linear_oracle(&h);
	test_banded_is_banded(&h);
	test_blend_is_smoothstep(&h);
	test_seventeen(&h);
	test_single_color(&h);
	test_origin(&h);
	test_terminal_clamp(&h);
	test_alpha(&h);
	test_rounded(&h);
	test_conic_oracle(&h);
	test_conic_degrees(&h);
	test_conic_seam(&h);
	test_conic_origin(&h);
	test_conic_is_not_linear(&h);
	test_conic_rounded(&h);
	test_property_damage(&h);
	test_scale_invariance(&h);
	test_gradient_space(&h);
	test_performance(&h);

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
