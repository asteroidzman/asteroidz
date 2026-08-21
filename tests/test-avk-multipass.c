/*
 * M4E.4: a multipass frame, built by hand, before any effect needs one.
 *
 * WHY A SYNTHETIC ONE. M4F's blur is the first thing that will render into an
 * intermediate and then sample it, and it will arrive with a shader, a kernel,
 * an exclusion box and a darken clamp all at once. Debugging a dependency
 * ordering bug through that is debugging two things. So the architecture gets
 * exercised first by an operation with no visual purpose whatever:
 *
 *     PASS A   fill transient R0 with four known colours
 *     BARRIER  R0 color-write -> sampled-read      <- the edge that matters
 *     PASS B   sample R0 into the target R1
 *
 * The effect is deliberately trivial and deliberately NOT SHIPPED. It lives
 * here and only here: M4E is architecture, and a user-visible "M4E effect"
 * would be a feature nobody asked for that has to be maintained forever.
 *
 * WHAT MAKES IT A TEST RATHER THAN A DEMO. The readback is compared against
 * exact integers. Pass A writes four quadrants of R0 in colours chosen so that
 * every one is distinguishable from every other and from the clear, and pass B
 * samples a SUB-RECTANGLE of R0 into a DIFFERENT position in R1 -- so a
 * dependency that ran in the wrong order, a transient handed out while still in
 * flight, or a sample that read the wrong region all produce specific wrong
 * colours at specific coordinates rather than "something looks off".
 *
 * Exits 77 (skip) with no GPU.
 */

#define _POSIX_C_SOURCE 200809L

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "render/vulkan/graph/avk_graph.h"
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

#define W 64
#define H 64
#define TRANSIENT_W 64
#define TRANSIENT_H 64
#define FMT VK_FORMAT_B8G8R8A8_UNORM

/*
 * Four colours, one per quadrant of the transient. Chosen so that each channel
 * differs from every other quadrant's in at least one component by a wide
 * margin: a wrong quadrant is a wrong ANSWER, not a slightly-off shade, and a
 * region read one pixel adrift shows up as a hard edge in the wrong place.
 *
 * None of them is black, so "the transient was never written" is separable from
 * "the transient was written with the wrong thing".
 */
static const float QUAD_COLORS[4][4] = {
	{ 1.00f, 0.00f, 0.00f, 1.0f },   /* top-left     red   */
	{ 0.00f, 1.00f, 0.00f, 1.0f },   /* top-right    green */
	{ 0.00f, 0.00f, 1.00f, 1.0f },   /* bottom-right blue  */
	{ 1.00f, 1.00f, 0.00f, 1.0f },   /* bottom-left  yellow */
};

struct harness {
	struct avk_instance *inst;
	struct avk_device *dev;
	struct avk_renderer renderer;
	struct avk_transient_pool pool;
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
	struct avk_image *image = avk_image_alloc(dev);
	if (image == NULL) {
		return NULL;
	}
	image->format = FMT;
	image->extent = (VkExtent2D){ W, H };
	image->has_alpha = true;
	image->layout = VK_IMAGE_LAYOUT_UNDEFINED;
	image->plane_count = 1;

	VkImageCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = FMT,
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
	avk_cmd_ring_init(&ring, dev, "multipass-readback", AVK_FRAMES_IN_FLIGHT);
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
	if (ok) {
		void *src = NULL;
		vkMapMemory(dev->dev, memory, 0, size, 0, &src);
		memcpy(h->pixels, src, (size_t)size);
		vkUnmapMemory(dev->dev, memory);
	}
	avk_cmd_ring_finish(&ring);
	vkDestroyBuffer(dev->dev, buffer, NULL);
	vkFreeMemory(dev->dev, memory, NULL);
	return ok;
}

static uint32_t px(const struct harness *h, int x, int y) {
	return h->pixels[(size_t)y * W + x];
}
static int r_of(uint32_t p) { return (int)((p >> 16) & 0xff); }
static int g_of(uint32_t p) { return (int)((p >> 8) & 0xff); }
static int b_of(uint32_t p) { return (int)(p & 0xff); }

/* ── the fixture's two passes ───────────────────────────────────────────── */

struct pass_ctx {
	struct harness *h;
	struct avk_image *dst;
	struct avk_image *src;      /* pass B only */
	uint32_t width, height;
	struct avk_box copy_from;   /* pass B: the sub-rectangle sampled */
	struct avk_box copy_to;     /* pass B: where it lands */
};

static void begin_rendering(VkCommandBuffer cb, struct avk_image *dst,
		uint32_t width, uint32_t height, bool clear) {
	VkRenderingAttachmentInfo color = {
		.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
		.imageView = dst->view,
		.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		.loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR
			: VK_ATTACHMENT_LOAD_OP_LOAD,
		.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		.clearValue = { .color = { .float32 = { 0.0f, 0.0f, 0.0f, 1.0f } } },
	};
	VkRenderingInfo info = {
		.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
		.renderArea = { { 0, 0 }, { width, height } },
		.layerCount = 1,
		.colorAttachmentCount = 1,
		.pColorAttachments = &color,
	};
	vkCmdBeginRendering(cb, &info);
	VkViewport vp = { 0, 0, (float)width, (float)height, 0.0f, 1.0f };
	vkCmdSetViewport(cb, 0, 1, &vp);
}

/* PASS A: four solid quadrants into the transient. */
static void record_pattern(VkCommandBuffer cb, void *user) {
	struct pass_ctx *ctx = user;
	struct avk_renderer *r = &ctx->h->renderer;

	begin_rendering(cb, ctx->dst, ctx->width, ctx->height, true);
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, r->pipes.rect);

	int hw = (int)ctx->width / 2, hh = (int)ctx->height / 2;
	struct avk_box quads[4] = {
		{ 0, 0, hw, hh },
		{ hw, 0, hw, hh },
		{ hw, hh, hw, hh },
		{ 0, hh, hw, hh },
	};
	for (int i = 0; i < 4; i++) {
		struct avk_push_constants pc = {0};
		pc.round_box[0] = (float)quads[i].x;
		pc.round_box[1] = (float)quads[i].y;
		pc.round_box[2] = (float)(quads[i].x + quads[i].width);
		pc.round_box[3] = (float)(quads[i].y + quads[i].height);
		/* Premultiplied: the blend state is (ONE, 1-SRC_ALPHA). */
		for (int c = 0; c < 3; c++) {
			pc.color[c] = QUAD_COLORS[i][c] * QUAD_COLORS[i][3];
		}
		pc.color[3] = QUAD_COLORS[i][3];
		pc.params[0] = 1.0f;
		pc.params[2] = (float)ctx->width;
		pc.params[3] = (float)ctx->height;
		vkCmdPushConstants(cb, r->pipes.layout,
			VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
			sizeof(pc), &pc);
		VkRect2D scissor = { { quads[i].x, quads[i].y },
			{ (uint32_t)quads[i].width, (uint32_t)quads[i].height } };
		vkCmdSetScissor(cb, 0, 1, &scissor);
		vkCmdDraw(cb, 4, 1, 0, 0);
	}
	vkCmdEndRendering(cb);
}

/* PASS B: sample a sub-rectangle of the transient into the target. */
static void record_composite(VkCommandBuffer cb, void *user) {
	struct pass_ctx *ctx = user;
	struct avk_renderer *r = &ctx->h->renderer;

	begin_rendering(cb, ctx->dst, ctx->width, ctx->height, true);
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, r->pipes.texture);

	VkDescriptorSet set = avk_pipelines_texture_set(&r->pipes, ctx->src, false);
	if (set == VK_NULL_HANDLE) {
		vkCmdEndRendering(cb);
		return;
	}
	vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
		r->pipes.layout, 0, 1, &set, 0, NULL);

	struct avk_push_constants pc = {0};
	pc.round_box[0] = (float)ctx->copy_to.x;
	pc.round_box[1] = (float)ctx->copy_to.y;
	pc.round_box[2] = (float)(ctx->copy_to.x + ctx->copy_to.width);
	pc.round_box[3] = (float)(ctx->copy_to.y + ctx->copy_to.height);
	/* Normalised source rectangle, no transform: origin plus two edge
	 * vectors, exactly what transform_uv() produces for AVK_TRANSFORM_NORMAL. */
	float sw = (float)ctx->src->extent.width;
	float sh = (float)ctx->src->extent.height;
	pc.uv_org_dx[0] = (float)ctx->copy_from.x / sw;
	pc.uv_org_dx[1] = (float)ctx->copy_from.y / sh;
	pc.uv_org_dx[2] = (float)ctx->copy_from.width / sw;
	pc.uv_org_dx[3] = 0.0f;
	pc.uv_dy[0] = 0.0f;
	pc.uv_dy[1] = (float)ctx->copy_from.height / sh;
	pc.params[0] = 1.0f;
	pc.params[1] = 1.0f;   /* keep the sampled alpha */
	pc.params[2] = (float)ctx->width;
	pc.params[3] = (float)ctx->height;
	vkCmdPushConstants(cb, r->pipes.layout,
		VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
		sizeof(pc), &pc);

	VkRect2D scissor = { { ctx->copy_to.x, ctx->copy_to.y },
		{ (uint32_t)ctx->copy_to.width, (uint32_t)ctx->copy_to.height } };
	vkCmdSetScissor(cb, 0, 1, &scissor);
	vkCmdDraw(cb, 4, 1, 0, 0);
	vkCmdEndRendering(cb);
}

/*
 * One multipass frame, through the graph.
 *
 * This is the shape M4F will have: acquire an intermediate, render into it,
 * sample it, release it against the submission that used it.
 */
static uint64_t multipass_frame(struct harness *h, struct avk_image **out_tr) {
	struct avk_graph *g = &h->renderer.graph;
	avk_graph_reset(g);

	struct avk_image *tr = avk_transient_acquire(&h->pool, FMT,
		TRANSIENT_W, TRANSIENT_H,
		VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
	if (tr == NULL) {
		return 0;
	}
	if (out_tr != NULL) {
		*out_tr = tr;
	}

	uint32_t r0 = avk_graph_add_image(g, tr, false, AVK_EXIT_KEEP);
	uint32_t r1 = avk_graph_add_image(g, h->target, false, AVK_EXIT_KEEP);

	struct pass_ctx a = {
		.h = h, .dst = tr,
		.width = tr->extent.width, .height = tr->extent.height,
	};
	struct pass_ctx b = {
		.h = h, .dst = h->target, .src = tr, .width = W, .height = H,
		/* The top-right quadrant of the transient -- green -- landing in the
		 * BOTTOM-LEFT of the target. Different source and destination on
		 * purpose: a pass that ignored one of the two rectangles would still
		 * produce a plausible picture if they matched. */
		.copy_from = { TRANSIENT_W / 2, 0, TRANSIENT_W / 2, TRANSIENT_H / 2 },
		.copy_to = { 0, H / 2, W / 2, H / 2 },
	};

	avk_graph_pass_begin(g, "pattern", record_pattern, &a);
	avk_graph_use(g, r0, AVK_USE_COLOR_WRITE, NULL);
	avk_graph_pass_end(g);

	avk_graph_pass_begin(g, "composite", record_composite, &b);
	/* READ region and WRITE region, stated separately -- the thing M4F needs
	 * and every primitive AVK draws today does not. */
	avk_graph_use(g, r0, AVK_USE_SAMPLED_READ, &b.copy_from);
	avk_graph_use(g, r1, AVK_USE_COLOR_WRITE, &b.copy_to);
	avk_graph_pass_end(g);

	VkCommandBuffer cb = avk_cmd_ring_begin(&h->renderer.ring);
	if (cb == VK_NULL_HANDLE) {
		return 0;
	}
	avk_graph_execute(g, cb, NULL, 0);
	uint64_t v = avk_cmd_ring_submit(&h->renderer.ring, NULL, 0, NULL, 0);
	avk_transient_release_frame(&h->pool, v);
	return v;
}

/* ── 1. topology ────────────────────────────────────────────────────────── */
static void test_topology(struct harness *h) {
	printf("\n-- topology --\n");

	uint64_t v = multipass_frame(h, NULL);
	CHECK(v != 0, "the multipass frame submitted");
	avk_device_timeline_wait(h->dev, v, 2000000000ULL);

	const struct avk_graph *g = &h->renderer.graph;
	CHECK(g->stats.passes == 2, "two passes (%u)", g->stats.passes);
	CHECK(g->stats.resources == 2, "two resources (%u)", g->stats.resources);
	CHECK(g->stats.uses == 3, "three uses (%u)", g->stats.uses);
	/* One barrier call per pass. Not one per resource, and not one giant
	 * up-front batch: pass B's dependency cannot be emitted before pass A has
	 * been recorded. */
	CHECK(g->stats.barriers == 2, "two barrier calls, one per pass (%u)",
		g->stats.barriers);

	bool edge = false;
	for (uint32_t i = 0; i < g->log_len; i++) {
		const struct avk_graph_barrier_log *bl = &g->log[i];
		if (bl->pass == 1 && bl->resource == 0
				&& bl->old_layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
				&& bl->new_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
				&& (bl->src_access & VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT)
				&& (bl->dst_access & VK_ACCESS_2_SHADER_SAMPLED_READ_BIT)) {
			edge = true;
		}
	}
	CHECK(edge, "the write->sample edge exists, and names the colour write it "
		"has to wait for");

	char dump[1024];
	avk_graph_dump(g, dump, sizeof(dump));
	printf("%s", dump);
}

/* ── 2. the pixels ──────────────────────────────────────────────────────── */
static void test_pixels(struct harness *h) {
	printf("\n-- pixels --\n");

	CHECK(readback(h), "the target reads back");

	/*
	 * The transient's TOP-RIGHT quadrant is green, and pass B put it in the
	 * target's BOTTOM-LEFT. So:
	 *
	 *   bottom-left of the target   green    -- the sampled region, relocated
	 *   everywhere else             black    -- pass B cleared and drew nothing
	 *
	 * A pass that sampled the wrong quadrant gives red, blue or yellow; one
	 * that ignored the destination rectangle covers the whole target; one that
	 * ran before pass A gives black where green should be. Each is a distinct,
	 * nameable wrong answer.
	 */
	uint32_t p = px(h, W / 4, H * 3 / 4);
	CHECK(r_of(p) < 8 && g_of(p) > 247 && b_of(p) < 8,
		"the sampled quadrant landed where pass B put it, and it is GREEN "
		"(%d,%d,%d)", r_of(p), g_of(p), b_of(p));

	uint32_t q = px(h, W * 3 / 4, H / 4);
	CHECK(r_of(q) < 8 && g_of(q) < 8 && b_of(q) < 8,
		"and nothing was written outside it (%d,%d,%d)",
		r_of(q), g_of(q), b_of(q));

	/* The corners of the relocated region, so a one-pixel offset in either
	 * rectangle is caught rather than averaged away. */
	uint32_t tl = px(h, 1, H / 2 + 1);
	uint32_t br = px(h, W / 2 - 2, H - 2);
	CHECK(g_of(tl) > 247 && g_of(br) > 247,
		"filling it corner to corner (%d, %d)", g_of(tl), g_of(br));
	uint32_t just_outside = px(h, W / 2 + 1, H - 2);
	CHECK(g_of(just_outside) < 8,
		"and stopping exactly at its edge (%d)", g_of(just_outside));
}

/* ── 3. reuse across frames ─────────────────────────────────────────────── */
static void test_reuse(struct harness *h) {
	printf("\n-- transient reuse --\n");

	avk_device_wait_idle(h->dev);
	uint64_t creates = h->pool.stats.creates;

	for (int i = 0; i < 120; i++) {
		uint64_t v = multipass_frame(h, NULL);
		avk_device_timeline_wait(h->dev, v, 2000000000ULL);
	}
	CHECK(h->pool.stats.creates == creates,
		"120 multipass frames allocated ZERO new images (%" PRIu64 ")",
		h->pool.stats.creates);
	CHECK(h->pool.stats.unsafe_reuses == 0,
		"and none was reused before the GPU had finished (%" PRIu64 ")",
		h->pool.stats.unsafe_reuses);

	/* The picture is still right after all that reuse -- which is the thing a
	 * counter cannot say. A transient handed back early would show the
	 * PREVIOUS frame's contents, and since every frame draws the same pattern
	 * that would be invisible... except that pass A clears it first, so an
	 * early hand-back races the clear and the readback goes black. */
	CHECK(readback(h), "the target still reads back");
	uint32_t p = px(h, W / 4, H * 3 / 4);
	CHECK(g_of(p) > 247, "and still shows the right pixels (%d,%d,%d)",
		r_of(p), g_of(p), b_of(p));
}

/* ── 4. wrap pressure ───────────────────────────────────────────────────── */
static void test_pressure(struct harness *h) {
	printf("\n-- wrap pressure --\n");

	/*
	 * Unthrottled: the CPU runs ahead, the command ring wraps every three
	 * frames, and every transient slot is reused many times over. This is where
	 * a reuse decided by anything other than the timeline -- a slot index, a
	 * frame counter, a flag a wrap resets -- shows up.
	 */
	uint64_t before = h->pool.stats.unsafe_reuses;
	for (int i = 0; i < 400; i++) {
		multipass_frame(h, NULL);
	}
	avk_device_wait_idle(h->dev);

	printf("  ---- %" PRIu64 " acquires, %" PRIu64 " creates, %u live\n",
		h->pool.stats.acquires, h->pool.stats.creates, h->pool.stats.live);
	CHECK(h->pool.stats.unsafe_reuses == before,
		"400 unthrottled frames, not one early reuse (%" PRIu64 ")",
		h->pool.stats.unsafe_reuses);
	/*
	 * The premise: the loop really ran. Weaker than it first looked, and
	 * deliberately so.
	 *
	 * The first version asserted `ring.stalls > 0` -- the CPU outrunning the
	 * GPU -- as proof that transients were genuinely in flight when the next
	 * frame asked for one. It read 224 stalls normally and ZERO under
	 * validation layers, because validation slows the CPU enough that the GPU
	 * keeps up. A premise that holds only when the layers are off is a premise
	 * that stops holding in exactly the run where correctness is being checked
	 * hardest.
	 *
	 * The in-flight case is proved deterministically elsewhere and by
	 * construction: test_two_in_one_frame() in the transient suite acquires
	 * twice within one frame and asserts the two images differ, and the break's
	 * own case releases against a timeline point that is never signalled. So
	 * what this loop is for is volume -- 400 frames of acquire, submit, release
	 * with the ring wrapping throughout -- and the numbers below are reported
	 * as data rather than asserted into a threshold.
	 *
	 * cpu_sync_waits is deliberately NOT checked: this fixture drives the
	 * command ring itself rather than going through avk_render_frame(), so the
	 * renderer never updates that field and comparing it would be comparing
	 * against a number nothing wrote.
	 */
	printf("  ---- %" PRIu64 " ring stalls (0 is normal under validation "
		"layers, which slow the CPU enough that the GPU keeps up)\n",
		h->renderer.ring.stalls);
	CHECK(h->pool.stats.acquires >= 400,
		"the loop really ran (%" PRIu64 " acquires)", h->pool.stats.acquires);
	CHECK(readback(h) && g_of(px(h, W / 4, H * 3 / 4)) > 247,
		"and the picture survives it");
}

/* ── 5. validation ──────────────────────────────────────────────────────── */
static void test_validation(struct harness *h) {
	printf("\n-- validation --\n");
	(void)h;

	if (!avk_debug_enabled()) {
		printf("  ---- ASTEROIDZ_VK_DEBUG unset: no layers loaded, so this "
			"says nothing either way\n");
		return;
	}
	uint64_t errors = avk_validation_errors();
	/*
	 * Under the write->read break this is where the failure lands:
	 * synchronization validation reports a read-after-write hazard on the
	 * transient, because pass B samples what pass A wrote with no dependency
	 * between them. That is the second, independent signal -- the first is the
	 * topology assertion in test_topology(), which fails on the barrier's
	 * source scope rather than on the GPU's behaviour.
	 */
	CHECK(errors == 0,
		"no validation or synchronization message in the whole run "
		"(%" PRIu64 ")", errors);
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	printf("== avk multipass (M4E.4) ==\n");

	struct harness h;
	memset(&h, 0, sizeof(h));
	h.inst = avk_instance_create("avk-multipass-test");
	if (h.inst == NULL) {
		SKIP("no Vulkan instance");
	}
	h.dev = avk_device_create(h.inst, -1);
	if (h.dev == NULL) {
		avk_instance_destroy(h.inst);
		SKIP("no Vulkan device");
	}
	if (!avk_renderer_init(&h.renderer, h.dev, FMT)) {
		avk_device_destroy(h.dev);
		avk_instance_destroy(h.inst);
		SKIP("no renderer");
	}
	avk_transient_pool_init(&h.pool, h.dev, &h.renderer.retire);
	h.target = make_target(h.dev);
	if (h.target == NULL) {
		SKIP("no target image");
	}

	test_topology(&h);
	test_pixels(&h);
	test_reuse(&h);
	test_pressure(&h);
	test_validation(&h);

	avk_device_wait_idle(h.dev);
	avk_transient_pool_finish(&h.pool);
	avk_image_destroy(h.dev, h.target);
	avk_renderer_finish(&h.renderer);
	avk_device_destroy(h.dev);
	avk_instance_destroy(h.inst);

	printf("\n---- %d/%d checks passed\n", checks - failures, checks);
	return failures == 0 ? 0 : 1;
}
