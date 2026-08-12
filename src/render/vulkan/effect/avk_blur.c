#define _POSIX_C_SOURCE 200809L

#include "avk_blur.h"

#include "../avk.h"

#include <stdlib.h>
#include <string.h>

/*
 * A blur level's extent.
 *
 * Halved per level and never allowed below 1: a zero-extent image is invalid,
 * and a 1-pixel level is a degenerate but legal blur of everything into one
 * texel. The caller decides whether that is worth drawing -- see the minimum
 * size check in avk_blur_declare().
 */
static uint32_t level_extent(uint32_t base, uint32_t level) {
	uint32_t v = base >> level;
	return v > 0 ? v : 1;
}

uint32_t avk_blur_support(const struct avk_blur_params *params) {
	if (params == NULL || params->levels == 0) {
		return 0;
	}
	uint32_t levels = params->levels;
	if (levels > AVK_BLUR_MAX_LEVELS) {
		levels = AVK_BLUR_MAX_LEVELS;
	}
	/*
	 * Each level's kernel reaches 2 of ITS OWN texels, and a level-i texel is
	 * 2^i source pixels across. Summed over the way down and the way back up:
	 *
	 *     2 * radius * sum(2^i for i in 1..levels) * 2
	 *   = 4 * radius * (2^(levels+1) - 2)
	 *
	 * Rounded UP, and deliberately generous: this feeds damage expansion, and
	 * under-expanding leaves a stale fringe that only appears on a moving
	 * window, while over-expanding costs a few pixels of redraw.
	 */
	double reach = 0.0;
	for (uint32_t i = 1; i <= levels; i++) {
		reach += (double)(1u << i);
	}
	double r = params->radius > 0.0f ? (double)params->radius : 1.0;
	return (uint32_t)(4.0 * r * reach) + 1;
}

/*
 * What one down- or up-sample draw needs to know.
 *
 * Recorded as a value rather than a pointer into the caller's stack: the graph
 * records passes later, and a callback holding a pointer to a loop variable is
 * how a render graph acquires a use-after-scope that only shows under load.
 * The chain owns an array of these for the frame.
 */
struct blur_pass {
	struct avk_pipelines *pipes;
	VkPipeline pipeline;
	struct avk_image *src;
	struct avk_image *dst;
	/* Destination viewport and the rectangle being written, in dst pixels. */
	uint32_t dst_w, dst_h;
	/* Sampling step in SOURCE texture units, folded on the CPU. Derived from
	 * the source's ALLOCATION extent, because a texel is 1/allocation in UV --
	 * not 1/logical. */
	float step_x, step_y;
	/* The last valid texel centre in the source, in UV. The pool's allocation
	 * is >= the logical extent, and everything past this bound was never
	 * written; see AZ_BLUR_UV_MAX in blur.glsl. */
	float uv_max_x, uv_max_y;
	/* The source rectangle, normalised. Always the whole level here, but kept
	 * explicit so a future region-limited blur changes numbers rather than
	 * structure. */
	float uv_x, uv_y, uv_w, uv_h;
	struct avk_blur_params effects;
	bool apply_effects;
};

/*
 * The per-frame arena for blur passes.
 *
 * Static and fixed-size, one slot per possible pass across every chain a frame
 * can contain. A malloc here would be a per-frame allocation on the deadline
 * path, which is the one thing M4E's whole storage design exists to avoid --
 * and the bound is small and knowable: levels are capped, and a frame with more
 * blurred surfaces than this has other problems.
 */
#define AVK_BLUR_MAX_PASSES (AVK_BLUR_MAX_LEVELS * 2 * 32)
static struct blur_pass g_passes[AVK_BLUR_MAX_PASSES];
static uint32_t g_pass_len;

void avk_blur_frame_reset(void) {
	g_pass_len = 0;
}

/*
 * Fold the source's LOGICAL extent and its ALLOCATION extent into a UV mapping.
 *
 * These are different numbers whenever the transient pool rounds up, which it
 * does at a 64 px granularity -- so a 32x32 level lives in a 64x64 image whose
 * other three quarters were never written this frame. Three things follow, and
 * getting any one of them wrong silently averages in unwritten memory:
 *
 *   the quad spans logical/allocation of the UV range, not all of it;
 *   a texel is 1/allocation in UV, so the half-texel step uses that;
 *   and every tap is clamped to the last valid texel centre (blur.glsl).
 *
 * The first version of this used 1.0 for the span and the logical extent for
 * the step. A 128x128 fixture lost a factor of four of its energy and levels 2
 * and 3 produced nothing at all -- which the flat-colour check would have
 * passed, and the checkerboard and impulse fixtures caught.
 */
static void blur_uv(struct blur_pass *p, uint32_t logical_w,
		uint32_t logical_h, float radius) {
	float aw = (float)p->src->extent.width;
	float ah = (float)p->src->extent.height;
	float lw = (float)logical_w, lh = (float)logical_h;

	p->uv_x = 0.0f;
	p->uv_y = 0.0f;
	p->uv_w = lw / aw;
	p->uv_h = lh / ah;
	/* Half an ALLOCATION texel, times the configured radius. */
	p->step_x = 0.5f / aw * radius;
	p->step_y = 0.5f / ah * radius;
	/* The centre of the last valid texel: (lw - 0.5) / aw. Sampling beyond it
	 * with a linear filter would blend the first padding texel in. */
	p->uv_max_x = (lw - 0.5f) / aw;
	p->uv_max_y = (lh - 0.5f) / ah;
}

static void record_blur_pass(VkCommandBuffer cb, void *user) {
	struct blur_pass *p = user;

	VkRenderingAttachmentInfo color = {
		.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
		.imageView = p->dst->view,
		.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		/*
		 * DONT_CARE, and it is correct here rather than lazy: this pass writes
		 * every pixel of the region it renders, with a replacing pipeline, so
		 * there is nothing to preserve and nothing to clear. LOAD would read a
		 * full level back out of memory for values that are all about to be
		 * overwritten.
		 */
		.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
	};
	VkRenderingInfo info = {
		.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
		.renderArea = { { 0, 0 }, { p->dst_w, p->dst_h } },
		.layerCount = 1,
		.colorAttachmentCount = 1,
		.pColorAttachments = &color,
	};
	vkCmdBeginRendering(cb, &info);

	VkViewport vp = { 0, 0, (float)p->dst_w, (float)p->dst_h, 0.0f, 1.0f };
	vkCmdSetViewport(cb, 0, 1, &vp);
	VkRect2D scissor = { { 0, 0 }, { p->dst_w, p->dst_h } };
	vkCmdSetScissor(cb, 0, 1, &scissor);
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p->pipeline);

	/* LINEAR, and the kernel depends on it: the 5-tap downsample's diagonal
	 * taps sit half a texel out so bilinear folds four texels into each. With
	 * a nearest sampler this is a sparse cross and the plus shape is visible. */
	VkDescriptorSet set = avk_pipelines_texture_set(p->pipes, p->src, true);
	if (set == VK_NULL_HANDLE) {
		vkCmdEndRendering(cb);
		return;
	}
	vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
		p->pipes->layout, 0, 1, &set, 0, NULL);

	struct avk_push_constants pc = {0};
	pc.round_box[0] = 0.0f;
	pc.round_box[1] = 0.0f;
	pc.round_box[2] = (float)p->dst_w;
	pc.round_box[3] = (float)p->dst_h;
	pc.params[0] = 1.0f;
	pc.params[2] = (float)p->dst_w;
	pc.params[3] = (float)p->dst_h;
	/* Origin and two edge vectors, the same convention every AVK draw uses --
	 * see quad.vert. No transform: a blur level is always axis-aligned. */
	pc.uv_org_dx[0] = p->uv_x;
	pc.uv_org_dx[1] = p->uv_y;
	pc.uv_org_dx[2] = p->uv_w;
	pc.uv_org_dx[3] = 0.0f;
	pc.uv_dy[0] = 0.0f;
	pc.uv_dy[1] = p->uv_h;
	/* The kernel step, in source texture units, and the valid bound past which
	 * the source holds nothing this frame wrote. */
	pc.color[0] = p->step_x;
	pc.color[1] = p->step_y;
	pc.color[2] = p->uv_max_x;
	pc.color[3] = p->uv_max_y;
	pc.inner_box[0] = p->effects.brightness;
	pc.inner_box[1] = p->effects.contrast;
	pc.inner_box[2] = p->effects.saturation;
	pc.inner_box[3] = p->effects.noise;
	pc.inner_corners[0] = p->apply_effects ? 1.0f : 0.0f;

	vkCmdPushConstants(cb, p->pipes->layout,
		VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
		sizeof(pc), &pc);
	vkCmdDraw(cb, 4, 1, 0, 0);
	vkCmdEndRendering(cb);
}

bool avk_blur_declare(struct avk_graph *graph, struct avk_transient_pool *pool,
		struct avk_pipelines *pipes, struct avk_blur_stats *stats,
		uint32_t src_resource, uint32_t dst_resource,
		uint32_t width, uint32_t height, VkFormat format,
		const struct avk_blur_params *params) {
	if (graph == NULL || pool == NULL || pipes == NULL || params == NULL) {
		return false;
	}
	if (src_resource >= graph->res_len || dst_resource >= graph->res_len) {
		avk_log(AVK_ERROR, "avk blur: source or destination is not a declared "
			"graph resource");
		if (stats) { stats->skipped++; }
		return false;
	}
	uint32_t levels = params->levels;
	if (levels == 0) {
		if (stats) { stats->skipped++; }
		return false;
	}
	if (levels > AVK_BLUR_MAX_LEVELS) {
		levels = AVK_BLUR_MAX_LEVELS;
	}
	/*
	 * A level must stay at least 2 px so the kernel has something to sample
	 * across. Rather than refusing the whole blur on a small window, the level
	 * count is REDUCED until it fits -- a 32x32 surface gets a 4-level blur's
	 * worth of levels it can actually hold, which looks like less blur rather
	 * than like no blur.
	 */
	while (levels > 0 && (level_extent(width, levels) < 2
			|| level_extent(height, levels) < 2)) {
		levels--;
	}
	if (levels == 0) {
		if (stats) { stats->skipped++; }
		return false;
	}
	if (g_pass_len + levels * 2 > AVK_BLUR_MAX_PASSES) {
		avk_log(AVK_ERROR, "avk blur: more than %d blur passes in one frame",
			AVK_BLUR_MAX_PASSES);
		if (stats) { stats->skipped++; }
		return false;
	}

	/*
	 * Acquire every level up front.
	 *
	 * Up front and not as the chain is walked, because a failure halfway
	 * through would leave passes already declared that read a transient the
	 * frame never got -- and the graph has no way to un-declare a pass. Either
	 * the whole chain exists or none of it does.
	 */
	struct avk_image *level_img[AVK_BLUR_MAX_LEVELS + 1];
	uint32_t level_res[AVK_BLUR_MAX_LEVELS + 1];
	for (uint32_t i = 1; i <= levels; i++) {
		uint32_t w = level_extent(width, i), h = level_extent(height, i);
		level_img[i] = avk_transient_acquire(pool, format, w, h,
			VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
		if (level_img[i] == NULL) {
			avk_log(AVK_ERROR, "avk blur: no transient for level %u (%ux%u)",
				i, w, h);
			if (stats) { stats->skipped++; }
			return false;
		}
		level_res[i] = avk_graph_add_image(graph, level_img[i], false,
			AVK_EXIT_KEEP);
		if (level_res[i] == AVK_GRAPH_INVALID) {
			if (stats) { stats->skipped++; }
			return false;
		}
		if (stats) { stats->transients++; }
	}

	float radius = params->radius > 0.0f ? params->radius : 1.0f;

	/* ── down ───────────────────────────────────────────────────────────── */
	for (uint32_t i = 1; i <= levels; i++) {
		uint32_t src_w = level_extent(width, i - 1);
		uint32_t src_h = level_extent(height, i - 1);
		uint32_t dw = level_extent(width, i), dh = level_extent(height, i);

		struct blur_pass *p = &g_passes[g_pass_len++];
		memset(p, 0, sizeof(*p));
		p->pipes = pipes;
		p->pipeline = pipes->blur_down;
		/* Taken from the GRAPH's own record rather than passed in separately:
		 * the resource index and the image it refers to cannot then disagree,
		 * which is the failure a second parameter would make possible. */
		p->src = i == 1 ? graph->resources[src_resource].image
			: level_img[i - 1];
		p->dst = level_img[i];
		p->dst_w = dw;
		p->dst_h = dh;
		blur_uv(p, src_w, src_h, radius);

		uint32_t from = i == 1 ? src_resource : level_res[i - 1];
		avk_graph_pass_begin(graph, i == 1 ? "blur_down_0" : "blur_down",
			record_blur_pass, p);
		avk_graph_use(graph, from, AVK_USE_SAMPLED_READ, NULL);
		avk_graph_use(graph, level_res[i], AVK_USE_COLOR_WRITE, NULL);
		avk_graph_pass_end(graph);
		if (stats) { stats->passes++; }
	}

	/* ── up ─────────────────────────────────────────────────────────────── */
	for (uint32_t i = levels; i >= 1; i--) {
		uint32_t src_w = level_extent(width, i);
		uint32_t src_h = level_extent(height, i);
		uint32_t dw = level_extent(width, i - 1);
		uint32_t dh = level_extent(height, i - 1);
		bool last = (i == 1);

		struct blur_pass *p = &g_passes[g_pass_len++];
		memset(p, 0, sizeof(*p));
		p->pipes = pipes;
		p->pipeline = pipes->blur_up;
		p->src = level_img[i];
		p->dst = last ? graph->resources[dst_resource].image
			: level_img[i - 1];
		p->dst_w = dw;
		p->dst_h = dh;
		blur_uv(p, src_w, src_h, radius);
		p->effects = *params;
		/* Only the LAST upsample folds the effects in. Applying them at every
		 * level would compound brightness and contrast once per level, which
		 * is a different picture and a much darker one. */
		p->apply_effects = last && params->apply_effects;

		uint32_t to = last ? dst_resource : level_res[i - 1];
		avk_graph_pass_begin(graph, last ? "blur_up_final" : "blur_up",
			record_blur_pass, p);
		avk_graph_use(graph, level_res[i], AVK_USE_SAMPLED_READ, NULL);
		avk_graph_use(graph, to, AVK_USE_COLOR_WRITE, NULL);
		avk_graph_pass_end(graph);
		if (stats) { stats->passes++; }
	}

	if (stats) { stats->chains++; }
	return true;
}
