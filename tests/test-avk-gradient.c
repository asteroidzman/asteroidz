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

struct grad_case {
	const char *name;
	int count;
	const float *colors;
	bool blend;
	float degree;
	float origin[2];
	/* Opaque background the gradient composites over, premultiplied. */
	float bg[4];
};

static void run_case(struct harness *h, const struct grad_case *c) {
	struct avk_box box = { 0, 0, W, H };

	struct avk_scene scene;
	avk_scene_init(&scene);
	pixman_region32_union_rect(&scene.damage, &scene.damage, 0, 0, W, H);
	scene.has_clear = true;
	memcpy(scene.clear_color, c->bg, sizeof(scene.clear_color));

	struct avk_cmd *cmd = avk_scene_add(&scene, AVK_CMD_RECT);
	cmd->dst = box;
	cmd->color[3] = 1.0f;
	avk_cmd_set_gradient(&scene, cmd, AVK_GRADIENT_LINEAR, c->degree, c->blend,
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
			float step_ = ref_step_linear((float)x + 0.5f, (float)y + 0.5f,
				&box, c->origin, c->degree);
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
