/*
 * M4E: the render graph -- topology, derived barriers, and the promise that a
 * frame with no multipass effects still costs what it cost before.
 *
 * WHAT THESE ASSERT ON. Not "the function returned true". A render graph fails
 * in exactly two ways, and both are silent: it emits a barrier that was not
 * needed (which costs performance and nothing says so), or it omits one that
 * was (which costs correctness on a driver other than this one). So the checks
 * here are on the barriers ACTUALLY EMITTED -- recorded by the graph as it
 * emits them, not re-derived afterwards from the same rules the implementation
 * used, because a test that re-derives cannot fail.
 *
 * The one table (usage -> stage, access, layout) is restated INDEPENDENTLY at
 * the top of test_usage_table(). That is deliberate duplication: comparing the
 * implementation's table against itself is a tautology, and the whole point of
 * having one table is that a wrong row is wrong everywhere.
 *
 * Exits 77 (skip) with no GPU.
 */

#define _POSIX_C_SOURCE 200809L

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "render/vulkan/graph/avk_graph.h"
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
	struct avk_image *tex_a;
	struct avk_image *tex_b;
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
		uint32_t height, VkImageUsageFlags usage) {
	struct avk_image *image = avk_image_alloc(dev);
	if (image == NULL) {
		return NULL;
	}
	image->format = TARGET_FORMAT;
	image->extent = (VkExtent2D){ width, height };
	image->has_alpha = true;
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

	VkImageViewCreateInfo vi = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = image->image,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = TARGET_FORMAT,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
	};
	vkCreateImageView(dev->dev, &vi, NULL, &image->view);
	AVK_LIVE_INC(dev, image_views);
	return image;
}

static bool harness_up(struct harness *h) {
	memset(h, 0, sizeof(*h));
	h->inst = avk_instance_create("avk-graph-test");
	if (h->inst == NULL) {
		return false;
	}
	h->dev = avk_device_create(h->inst, -1);
	if (h->dev == NULL) {
		return false;
	}
	if (!avk_renderer_init(&h->renderer, h->dev, TARGET_FORMAT)) {
		return false;
	}
	h->target = make_image(h->dev, W, H,
		VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
	h->tex_a = make_image(h->dev, W, H,
		VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
	h->tex_b = make_image(h->dev, W, H,
		VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
	return h->target != NULL && h->tex_a != NULL && h->tex_b != NULL;
}

static void harness_down(struct harness *h) {
	if (h->dev != NULL) {
		avk_device_wait_idle(h->dev);
	}
	if (h->target != NULL) {
		avk_image_destroy(h->dev, h->target);
	}
	if (h->tex_a != NULL) {
		avk_image_destroy(h->dev, h->tex_a);
	}
	if (h->tex_b != NULL) {
		avk_image_destroy(h->dev, h->tex_b);
	}
	if (h->dev != NULL) {
		avk_renderer_finish(&h->renderer);
		avk_device_destroy(h->dev);
	}
	if (h->inst != NULL) {
		avk_instance_destroy(h->inst);
	}
}

/* Render a scene and wait for it, so the graph's stats describe a frame that
 * really happened. */
static bool render(struct harness *h, struct avk_scene *scene) {
	uint64_t value = avk_render_frame(&h->renderer, h->target, scene,
		NULL, 0, NULL, 0);
	return value != 0 && avk_device_timeline_wait(h->dev, value, 2000000000ULL);
}

static void scene_full_damage(struct avk_scene *scene) {
	avk_scene_init(scene);
	pixman_region32_union_rect(&scene->damage, &scene->damage, 0, 0, W, H);
	scene->has_clear = true;
	scene->clear_color[3] = 1.0f;
}

static struct avk_cmd *add_texture(struct avk_scene *scene,
		struct avk_image *image) {
	struct avk_cmd *cmd = avk_scene_add(scene, AVK_CMD_TEXTURE);
	if (cmd == NULL) {
		return NULL;
	}
	cmd->dst = (struct avk_box){ 8, 8, 16, 16 };
	cmd->image = image;
	cmd->src = (struct avk_fbox){ 0, 0, W, H };
	cmd->opacity = 1.0f;
	return cmd;
}

/* How many barriers the graph logged whose target is `resource`. */
static uint32_t log_count_for(const struct avk_graph *g, uint32_t resource) {
	uint32_t n = 0;
	for (uint32_t i = 0; i < g->log_len; i++) {
		if (g->log[i].resource == resource) {
			n++;
		}
	}
	return n;
}

/* ── 1. the usage table ─────────────────────────────────────────────────────
 *
 * Restated here from the Vulkan spec rather than read from the implementation.
 * If these two ever disagree, one of them is wrong and this is the one that
 * says so.
 */
static void test_usage_table(void) {
	printf("\n-- usage table --\n");

	struct {
		enum avk_graph_usage usage;
		VkPipelineStageFlags2 stage;
		VkAccessFlags2 access;
		VkImageLayout layout;
		const char *name;
	} want[] = {
		{ AVK_USE_COLOR_WRITE,
			VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
			VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT
				| VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
			VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, "color-write" },
		{ AVK_USE_SAMPLED_READ,
			VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
			VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, "sampled-read" },
		{ AVK_USE_TRANSFER_READ,
			VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
			VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, "transfer-read" },
		{ AVK_USE_TRANSFER_WRITE,
			VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, "transfer-write" },
	};

	for (size_t i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
		VkPipelineStageFlags2 stage = 0;
		VkAccessFlags2 access = 0;
		VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
		avk_graph_usage_state(want[i].usage, &stage, &access, &layout);
		CHECK(stage == want[i].stage && access == want[i].access
				&& layout == want[i].layout,
			"%s maps to the stage/access/layout the spec requires",
			want[i].name);
		CHECK(strcmp(avk_graph_usage_name(want[i].usage), want[i].name) == 0,
			"%s is named %s", want[i].name,
			avk_graph_usage_name(want[i].usage));
	}

	/*
	 * The one that is easy to get wrong and impossible to see. loadOp LOAD
	 * reads the attachment, so a colour write must declare the READ access as
	 * well -- a barrier covering only the write leaves that load unsynchronised
	 * against whatever produced the previous contents.
	 */
	VkAccessFlags2 access = 0;
	avk_graph_usage_state(AVK_USE_COLOR_WRITE, NULL, &access, NULL);
	CHECK((access & VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT) != 0,
		"colour write carries the READ bit, because loadOp LOAD is a read");
}

/* ── 2. the direct path's topology ───────────────────────────────────────── */
static void test_direct_path(struct harness *h) {
	printf("\n-- direct path --\n");

	struct avk_scene scene;
	scene_full_damage(&scene);
	struct avk_cmd *rect = avk_scene_add(&scene, AVK_CMD_RECT);
	rect->dst = (struct avk_box){ 0, 0, W, H };
	rect->color[0] = rect->color[3] = 1.0f;
	rect->opacity = 1.0f;
	CHECK(render(h, &scene), "a rect-only frame renders");

	const struct avk_graph *g = &h->renderer.graph;
	CHECK(g->stats.passes == 1, "one pass (%u)", g->stats.passes);
	CHECK(g->stats.resources == 1, "one resource -- the target (%u)",
		g->stats.resources);
	CHECK(g->stats.uses == 1, "one use (%u)", g->stats.uses);
	/*
	 * ONE barrier call. The target is one of ours (not an imported dma-buf), so
	 * there is no exit release and therefore no second flush. This is the
	 * number that says the graph did not turn a one-pass frame into a
	 * multi-stage pipeline.
	 */
	CHECK(g->stats.barriers == 1, "one vkCmdPipelineBarrier2 (%u)",
		g->stats.barriers);
	CHECK(g->stats.image_transitions == 1,
		"one image transition: UNDEFINED -> COLOR_ATTACHMENT_OPTIMAL (%u)",
		g->stats.image_transitions);
	CHECK(g->log_len == 1 && g->log[0].new_layout
			== VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		"the target ends up a colour attachment");
	CHECK(h->target->layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		"and the image's own layout field agrees, so the next frame's entry "
		"state is the truth");

	avk_scene_finish(&scene);
}

/* ── 3. two commands, one image ─────────────────────────────────────────────
 *
 * The read->read requirement. Two draws sampling one surface is completely
 * ordinary -- a window and its own subsurface, a texture drawn twice under
 * different clips -- and the pre-graph renderer needed an explicit duplicate
 * check to avoid emitting a second barrier whose oldLayout the first had
 * already consumed. Here it must fall out of the model.
 */
static void test_read_after_read(struct harness *h) {
	printf("\n-- read after read --\n");

	struct avk_scene scene;
	scene_full_damage(&scene);
	add_texture(&scene, h->tex_a);
	add_texture(&scene, h->tex_a);
	add_texture(&scene, h->tex_a);
	CHECK(render(h, &scene), "three draws sampling one image render");

	const struct avk_graph *g = &h->renderer.graph;
	CHECK(g->stats.uses == 4, "four uses: one write plus three reads (%u)",
		g->stats.uses);
	CHECK(g->stats.resources == 2, "but only two resources (%u)",
		g->stats.resources);
	CHECK(log_count_for(g, 1) == 1,
		"and ONE barrier for the sampled image, not three (%u)",
		log_count_for(g, 1));
	CHECK(g->stats.barriers == 1, "still one barrier call for the frame (%u)",
		g->stats.barriers);

	avk_scene_finish(&scene);

	/*
	 * ACROSS frames is a different question, and the answer is the other way
	 * round.
	 *
	 * Within a frame the graph knows nothing wrote the image between the two
	 * reads, because it watched. Between frames it knows no such thing: a
	 * client may have redrawn its surface, an SHM upload may have landed on the
	 * other command ring, KMS may have finished with a scan-out buffer. So each
	 * frame re-acquires, and the barrier is emitted again.
	 *
	 * This is a DIFFERENCE FROM THE PRE-GRAPH RENDERER, which skipped the
	 * barrier for a non-foreign image already in the right layout. That skip
	 * was an assumption about a producer outside its knowledge; it happened to
	 * hold. The cost of not making it is one extra VkImageMemoryBarrier2 inside
	 * a vkCmdPipelineBarrier2 call that was happening anyway -- the number of
	 * barrier CALLS is unchanged, which is the number that costs a pipeline
	 * flush.
	 */
	scene_full_damage(&scene);
	add_texture(&scene, h->tex_a);
	CHECK(render(h, &scene), "the same image renders again next frame");
	CHECK(log_count_for(g, 1) == 1,
		"and IS re-acquired, because between frames the graph does not know "
		"what wrote it (%u)", log_count_for(g, 1));
	CHECK(g->stats.barriers == 1,
		"still one barrier CALL, which is what costs a flush (%u)",
		g->stats.barriers);
	avk_scene_finish(&scene);
}

/* ── 4. two images ──────────────────────────────────────────────────────── */
static void test_two_images(struct harness *h) {
	printf("\n-- two sampled images --\n");

	struct avk_scene scene;
	scene_full_damage(&scene);
	add_texture(&scene, h->tex_a);
	add_texture(&scene, h->tex_b);
	CHECK(render(h, &scene), "two different images render");

	const struct avk_graph *g = &h->renderer.graph;
	CHECK(g->stats.resources == 3, "three resources (%u)", g->stats.resources);
	CHECK(g->stats.barriers == 1,
		"one barrier CALL carrying every transition the pass needs (%u)",
		g->stats.barriers);
	CHECK(log_count_for(g, 1) == 1 && log_count_for(g, 2) == 1,
		"one barrier each, both folded into that single call");

	avk_scene_finish(&scene);
}

/* ── 5. write then read: the edge M4F cannot do without ──────────────────── */
static void test_write_then_read(struct harness *h) {
	printf("\n-- write -> read --\n");

	struct avk_graph *g = &h->renderer.graph;
	avk_graph_reset(g);

	uint32_t r0 = avk_graph_add_image(g, h->tex_a, false, AVK_EXIT_KEEP);
	uint32_t r1 = avk_graph_add_image(g, h->target, false, AVK_EXIT_KEEP);
	CHECK(r0 == 0 && r1 == 1, "two resources declared");

	avk_graph_pass_begin(g, "produce", NULL, NULL);
	avk_graph_use(g, r0, AVK_USE_COLOR_WRITE, NULL);
	avk_graph_pass_end(g);

	avk_graph_pass_begin(g, "consume", NULL, NULL);
	avk_graph_use(g, r0, AVK_USE_SAMPLED_READ, NULL);
	avk_graph_use(g, r1, AVK_USE_COLOR_WRITE, NULL);
	avk_graph_pass_end(g);

	/* Compiled onto a throwaway command buffer: this test is about what the
	 * graph decides, not about what the GPU does with it. */
	struct avk_cmd_ring ring;
	avk_cmd_ring_init(&ring, h->dev, "graph-test");
	VkCommandBuffer cb = avk_cmd_ring_begin(&ring);
	avk_graph_execute(g, cb, NULL, 0);
	avk_cmd_ring_abandon(&ring);
	avk_cmd_ring_finish(&ring);

	CHECK(g->stats.passes == 2, "two passes (%u)", g->stats.passes);
	CHECK(g->stats.barriers == 2, "two barrier calls, one per pass (%u)",
		g->stats.barriers);

	/* The edge itself: r0 goes from colour attachment to sampled. */
	bool found = false;
	for (uint32_t i = 0; i < g->log_len; i++) {
		const struct avk_graph_barrier_log *b = &g->log[i];
		if (b->resource == r0 && b->pass == 1
				&& b->old_layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
				&& b->new_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
				&& (b->src_access & VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT) != 0
				&& (b->dst_access & VK_ACCESS_2_SHADER_SAMPLED_READ_BIT) != 0) {
			found = true;
		}
	}
	CHECK(found, "the write->read barrier exists, names the colour write as "
		"its source and the sampled read as its destination");

	char dump[1024];
	avk_graph_dump(g, dump, sizeof(dump));
	CHECK(strstr(dump, "PASS produce") != NULL
			&& strstr(dump, "PASS consume") != NULL
			&& strstr(dump, "R0 color-write -> sampled-read") != NULL,
		"and the dump says so in one readable line");
	printf("%s", dump);
}

/* ── 6. the break ────────────────────────────────────────────────────────── */
static void test_break(struct harness *h) {
	printf("\n-- BREAK=graph-missing-write-read --\n");

	if (getenv("AZ_GRAPH_NO_WRITE_READ") == NULL) {
		printf("  ---- not set; the assertions below are what it must break\n");
	}

	struct avk_graph g;
	avk_graph_init(&g, h->dev);

	uint32_t r0 = avk_graph_add_image(&g, h->tex_a, false, AVK_EXIT_KEEP);
	avk_graph_pass_begin(&g, "produce", NULL, NULL);
	avk_graph_use(&g, r0, AVK_USE_COLOR_WRITE, NULL);
	avk_graph_pass_end(&g);
	avk_graph_pass_begin(&g, "consume", NULL, NULL);
	avk_graph_use(&g, r0, AVK_USE_SAMPLED_READ, NULL);
	avk_graph_pass_end(&g);

	struct avk_cmd_ring ring;
	avk_cmd_ring_init(&ring, h->dev, "graph-break");
	VkCommandBuffer cb = avk_cmd_ring_begin(&ring);
	avk_graph_execute(&g, cb, NULL, 0);
	avk_cmd_ring_abandon(&ring);
	avk_cmd_ring_finish(&ring);

	/*
	 * With the break OFF this is 2 (the initial acquire, then the write->read
	 * transition). With it ON the second one is gone, which is the whole and
	 * only difference -- and it is a genuine missing DEPENDENCY rather than a
	 * missing transition, because the image is still legally readable in the
	 * layout it is left in.
	 *
	 * The read->read fast path is why the break has to be tested here rather
	 * than inferred: a naive "does it emit fewer barriers" check would also
	 * pass for a graph that had simply stopped working.
	 */
	CHECK(g.stats.barriers == 2,
		"a write followed by a read produces TWO barrier calls (%u)",
		g.stats.barriers);

	avk_graph_finish(&g);
}

/* ── 7. no heap churn ────────────────────────────────────────────────────── */
static void test_no_churn(struct harness *h) {
	printf("\n-- allocation --\n");

	struct avk_scene scene;
	scene_full_damage(&scene);
	add_texture(&scene, h->tex_a);
	add_texture(&scene, h->tex_b);
	struct avk_cmd *rect = avk_scene_add(&scene, AVK_CMD_RECT);
	rect->dst = (struct avk_box){ 0, 0, 32, 32 };
	rect->color[3] = 1.0f;
	rect->opacity = 1.0f;

	/* Warm up. Growth is geometric, so a handful of frames is enough to reach
	 * the capacity a stable scene needs. */
	for (int i = 0; i < 8; i++) {
		render(h, &scene);
	}
	uint64_t warm = h->renderer.graph.stats.allocs;
	uint64_t build_before = h->renderer.graph.stats.build_ns;
	uint64_t frames_before = h->renderer.graph.stats.frames;

	for (int i = 0; i < 200; i++) {
		render(h, &scene);
	}
	uint64_t after = h->renderer.graph.stats.allocs;

	CHECK(warm > 0, "the graph did allocate during warmup (%" PRIu64 ")", warm);
	/*
	 * THE REQUIREMENT. Not "few allocations" -- none. A counter that stops
	 * moving is the only form of this claim that cannot be satisfied by a
	 * threshold nobody checks.
	 */
	CHECK(after == warm,
		"and then ZERO in 200 further frames (%" PRIu64 " -> %" PRIu64 ")",
		warm, after);

	uint64_t frames = h->renderer.graph.stats.frames - frames_before;
	uint64_t build = h->renderer.graph.stats.build_ns - build_before;
	printf("  ---- graph_build_ns/frame = %" PRIu64 " over %" PRIu64
		" frames\n", frames ? build / frames : 0, frames);
	/*
	 * A ceiling, not a target, and generous on purpose: this runs on whatever
	 * machine builds the project, under whatever load. The number to look at is
	 * the one printed above. 100us would be 1.4% of a 144Hz frame and is far
	 * beyond anything a three-array walk can reach; if this ever fires,
	 * something is doing real work per frame that should not be.
	 */
	CHECK(frames > 0 && build / frames < 100000,
		"graph construction is microseconds, not milliseconds");

	avk_scene_finish(&scene);
}

/* ── 8. regions are three, not one ───────────────────────────────────────── */
static void test_regions(struct harness *h) {
	printf("\n-- write / read / extent --\n");

	struct avk_graph g;
	avk_graph_init(&g, h->dev);
	uint32_t r0 = avk_graph_add_image(&g, h->tex_a, false, AVK_EXIT_KEEP);

	/*
	 * The shape M4F needs: a pass writes a small box and reads a larger one,
	 * out of an image larger still. Nothing here consumes the regions yet --
	 * M4E does not implement blur -- but the model must be able to CARRY them,
	 * because a graph that stored one rectangle per use would have to change
	 * shape rather than gain a field.
	 */
	struct avk_box write = { 20, 20, 24, 24 };
	struct avk_box read = { 4, 4, 56, 56 };   /* dilated by a 16px radius */

	avk_graph_pass_begin(&g, "blur_h", NULL, NULL);
	avk_graph_use(&g, r0, AVK_USE_SAMPLED_READ, &read);
	avk_graph_use(&g, r0, AVK_USE_COLOR_WRITE, &write);
	avk_graph_pass_end(&g);

	CHECK(g.use_len == 2, "two uses of one resource with different regions");
	CHECK(g.uses[0].region.width == 56 && g.uses[1].region.width == 24,
		"the read region (56) and the write region (24) are both kept");
	CHECK(h->tex_a->extent.width == W,
		"and neither is the resource extent (%u)", h->tex_a->extent.width);
	CHECK(g.uses[0].region.width > g.uses[1].region.width,
		"a blur's read region is LARGER than what it writes -- the case a "
		"single-rectangle model cannot express");

	avk_graph_finish(&g);
}

/* ── 9. the frame's exit state ───────────────────────────────────────────── */
static void test_exit_state(struct harness *h) {
	printf("\n-- exit state --\n");

	struct avk_graph g;
	avk_graph_init(&g, h->dev);

	/*
	 * A foreign resource: what a client's dma-buf and the scan-out target
	 * actually are. It must be acquired from VK_QUEUE_FAMILY_FOREIGN_EXT and
	 * released back, and the release is what makes a real frame visible --
	 * without it a compressed AMD scan-out buffer is handed to KMS in a state
	 * the display engine cannot read and the monitor comes up flat white. No
	 * headless test can catch that by looking at pixels, so it is asserted on
	 * the queue-family indices instead.
	 */
	h->tex_b->layout = VK_IMAGE_LAYOUT_GENERAL;
	uint32_t r0 = avk_graph_add_image(&g, h->tex_b, true, AVK_EXIT_FOREIGN);
	avk_graph_pass_begin(&g, "compose", NULL, NULL);
	avk_graph_use(&g, r0, AVK_USE_SAMPLED_READ, NULL);
	avk_graph_pass_end(&g);

	struct avk_cmd_ring ring;
	avk_cmd_ring_init(&ring, h->dev, "graph-exit");
	VkCommandBuffer cb = avk_cmd_ring_begin(&ring);
	avk_graph_execute(&g, cb, NULL, 0);
	avk_cmd_ring_abandon(&ring);
	avk_cmd_ring_finish(&ring);

	CHECK(g.log_len == 2, "an acquire and a release (%u)", g.log_len);
	CHECK(g.log_len == 2 && strcmp(g.log[0].from, "external") == 0,
		"the first names the resource as arriving from outside");
	CHECK(g.log_len == 2 && strcmp(g.log[1].to, "exit") == 0
			&& g.log[1].new_layout == VK_IMAGE_LAYOUT_GENERAL,
		"the last hands it back in GENERAL");
	CHECK(h->tex_b->layout == VK_IMAGE_LAYOUT_GENERAL,
		"and the image records that, so the NEXT frame acquires from GENERAL "
		"rather than from an assumption");

	avk_graph_finish(&g);
	h->tex_b->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	printf("== avk graph ==\n");

	struct harness h;
	if (!harness_up(&h)) {
		harness_down(&h);
		SKIP("no Vulkan device");
	}

	test_usage_table();
	test_direct_path(&h);
	test_read_after_read(&h);
	test_two_images(&h);
	test_write_then_read(&h);
	test_break(&h);
	test_no_churn(&h);
	test_regions(&h);
	test_exit_state(&h);

	harness_down(&h);
	printf("\n---- %d/%d checks passed\n", checks - failures, checks);
	return failures == 0 ? 0 : 1;
}
