#define _POSIX_C_SOURCE 200809L

#include "avk_blur.h"

#include "../avk.h"
#include "../scene/avk_render.h"

#include <math.h>
#include <stdio.h>
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

/*
 * How many SOURCE pixels one texel of `level` spans.
 *
 * Computed from the actual extents rather than as 2^level: level_extent()
 * floors, so 129 source pixels become 64 texels at level 1 and each of those
 * texels spans 2.016 source pixels, not 2. Assuming the power of two
 * under-covers on every odd extent.
 */
static double texel_span(uint32_t base, uint32_t level) {
	uint32_t n = level_extent(base, level);
	return n > 0 ? (double)base / (double)n : (double)(1u << level);
}

/* Levels this configuration will actually run at this size -- the same
 * reduction avk_blur_declare() applies, so support and reality cannot
 * disagree about how many passes there are. */
static uint32_t effective_levels(const struct avk_blur_params *params,
		uint32_t width, uint32_t height) {
	uint32_t levels = params->levels;
	if (levels > AVK_BLUR_MAX_LEVELS) {
		levels = AVK_BLUR_MAX_LEVELS;
	}
	while (levels > 0 && (level_extent(width, levels) < 2
			|| level_extent(height, levels) < 2)) {
		levels--;
	}
	return levels;
}

static double support_axis(const struct avk_blur_params *params,
		uint32_t base, uint32_t levels) {
	double radius = params->radius > 0.0f ? (double)params->radius : 1.0;
	double reach = 0.0;

	/* Down: level i-1 -> level i, reaching (0.5*radius + 1) texels of i-1. */
	for (uint32_t i = 1; i <= levels; i++) {
		reach += (0.5 * radius + 1.0) * texel_span(base, i - 1);
	}
	/* Up: level i -> level i-1, reaching (radius + 1) texels of i. */
	for (uint32_t i = levels; i >= 1; i--) {
		reach += (radius + 1.0) * texel_span(base, i);
	}
	/* One source pixel for a fractional origin: the region being blurred need
	 * not start on a texel boundary, and the composite that consumes it need
	 * not either. */
	return reach + 1.0;
}

struct avk_blur_support avk_blur_support_of(
		const struct avk_blur_params *params, uint32_t width,
		uint32_t height) {
	struct avk_blur_support s = {0};
	if (params == NULL || params->levels == 0 || width == 0 || height == 0) {
		return s;
	}
	uint32_t levels = effective_levels(params, width, height);
	if (levels == 0) {
		return s;
	}
	double x = support_axis(params, width, levels);
	double y = support_axis(params, height, levels);
	/*
	 * Symmetric, because the dual-Kawase kernel is: every tap has a mirror at
	 * the same offset. Written as four fields anyway so an asymmetric effect
	 * changes numbers rather than every caller's type.
	 */
	s.left = (float)x;
	s.right = (float)x;
	s.top = (float)y;
	s.bottom = (float)y;
	return s;
}

/*
 * The forward reach. Equal to the reverse one by the derivation in the header,
 * and written as its own function so a caller has to say which direction it
 * meant. It is deliberately NOT `#define avk_blur_forward_support_of
 * avk_blur_support_of`: the equality is a derived property of a symmetric
 * kernel over an affine level mapping, not a definition, and an asymmetric
 * effect would make this body differ while every call site stayed correct.
 */
struct avk_blur_support avk_blur_forward_support_of(
		const struct avk_blur_params *params, uint32_t width,
		uint32_t height) {
	struct avk_blur_support s = avk_blur_support_of(params, width, height);
	/* Mirrored, because a forward step traverses the same kernel backwards:
	 * what reaches LEFT to read is reached FROM the right. Identical while the
	 * kernel is symmetric, which is exactly the assumption worth writing down
	 * rather than relying on. */
	struct avk_blur_support f = {
		.left = s.right, .right = s.left,
		.top = s.bottom, .bottom = s.top,
	};
	return f;
}

uint32_t avk_blur_forward_support_max(const struct avk_blur_params *params,
		uint32_t width, uint32_t height) {
	struct avk_blur_support s = avk_blur_forward_support_of(params, width,
		height);
	double m = s.left;
	if (s.right > m) { m = s.right; }
	if (s.top > m) { m = s.top; }
	if (s.bottom > m) { m = s.bottom; }
	return (uint32_t)ceil(m);
}

uint32_t avk_blur_support_bound(const struct avk_blur_params *params) {
	if (params == NULL || params->levels == 0) {
		return 0;
	}
	uint32_t levels = params->levels;
	if (levels > AVK_BLUR_MAX_LEVELS) {
		levels = AVK_BLUR_MAX_LEVELS;
	}
	double radius = params->radius > 0.0f ? (double)params->radius : 1.0;
	/* span(base, i) <= 2^i + (2^i - 1)/2, for every extent a level can run
	 * at -- see the header. Everything else is support_axis() unchanged. */
	double reach = 0.0;
	for (uint32_t i = 1; i <= levels; i++) {
		double two = (double)(1u << (i - 1));
		reach += (0.5 * radius + 1.0) * (two + (two - 1.0) / 2.0);
	}
	for (uint32_t i = levels; i >= 1; i--) {
		double two = (double)(1u << i);
		reach += (radius + 1.0) * (two + (two - 1.0) / 2.0);
	}
	return (uint32_t)ceil(reach + 1.0);
}

uint32_t avk_blur_support_max(const struct avk_blur_params *params,
		uint32_t width, uint32_t height) {
	struct avk_blur_support s = avk_blur_support_of(params, width, height);
	double m = s.left;
	if (s.right > m) { m = s.right; }
	if (s.top > m) { m = s.top; }
	if (s.bottom > m) { m = s.bottom; }
	/* Outward at the physical-pixel boundary, once, at the end -- rounding
	 * each term would compound the error across the chain. */
	return (uint32_t)ceil(m);
}

bool avk_blur_regions_of(struct avk_blur_regions *out,
		const struct avk_box *write, const struct avk_blur_params *params,
		const struct avk_box *clamp) {
	if (out == NULL || write == NULL || params == NULL
			|| write->width <= 0 || write->height <= 0) {
		return false;
	}
	memset(out, 0, sizeof(*out));
	out->write = *write;

	struct avk_blur_support s = avk_blur_support_of(params,
		(uint32_t)write->width, (uint32_t)write->height);
	/* Outward on every edge, independently: the support type is four numbers so
	 * an asymmetric kernel needs no new code here. */
	int32_t l = (int32_t)ceil((double)s.left);
	int32_t r = (int32_t)ceil((double)s.right);
	int32_t t = (int32_t)ceil((double)s.top);
	int32_t b = (int32_t)ceil((double)s.bottom);

	int32_t x0 = write->x - l, y0 = write->y - t;
	int32_t x1 = write->x + write->width + r;
	int32_t y1 = write->y + write->height + b;
	if (clamp != NULL) {
		if (x0 < clamp->x) { x0 = clamp->x; }
		if (y0 < clamp->y) { y0 = clamp->y; }
		if (x1 > clamp->x + clamp->width) { x1 = clamp->x + clamp->width; }
		if (y1 > clamp->y + clamp->height) { y1 = clamp->y + clamp->height; }
	}
	if (x1 <= x0 || y1 <= y0) {
		return false;
	}
	out->dependency = (struct avk_box){ x0, y0, x1 - x0, y1 - y0 };

	/*
	 * ALIGNMENT GROWS, NEVER SHIFTS. The origin moves down to even and the
	 * extent grows by exactly as much, so the far edge is where it was.
	 */
	int32_t ax = x0, ay = y0;
	uint32_t aw = (uint32_t)(x1 - x0), ah = (uint32_t)(y1 - y0);
	avk_render_segment_align_origin(&ax, &ay, &aw, &ah);
	out->capture = (struct avk_box){ ax, ay, (int32_t)aw, (int32_t)ah };

	/* The invariant, checked rather than trusted. A capture that failed to
	 * contain its dependency is a stale fringe on the far edge of every
	 * blurred window, visible only when the dependency starts odd. */
	if (out->capture.x > out->dependency.x || out->capture.y > out->dependency.y
			|| out->capture.x + out->capture.width
				< out->dependency.x + out->dependency.width
			|| out->capture.y + out->capture.height
				< out->dependency.y + out->dependency.height) {
		avk_log(AVK_ERROR, "avk blur: aligned capture %d,%d %dx%d does not "
			"contain dependency %d,%d %dx%d",
			out->capture.x, out->capture.y, out->capture.width,
			out->capture.height, out->dependency.x, out->dependency.y,
			out->dependency.width, out->dependency.height);
		return false;
	}
	return true;
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
	/*
	 * ── TEST-ONLY: RENDER ONLY THIS RECTANGLE ─────────────────────────────
	 *
	 * Zero width means "the whole level", which is what production always
	 * does: M4F.2D has NOT implemented regional filter execution. This exists
	 * for one purpose -- proving that the derived required region is
	 * NECESSARY and not merely sufficient.
	 *
	 * With AZ_TRANSIENT_POISON=1 every transient starts as garbage, so
	 * scissoring a pass to region R leaves poison everywhere else at that
	 * level. Render the DERIVED region and the final image must be unchanged
	 * (the region is sufficient); render one strip LESS and the final image
	 * must change (the region is necessary). Either half alone proves nothing.
	 */
	int32_t scissor_x, scissor_y;
	uint32_t scissor_w, scissor_h;
	/* LOAD rather than DONT_CARE, because the darken clamp's whole trick is that
	 * the destination still holds the unblurred source. Only ever true on the
	 * last upsample of a chain whose params ask for it. */
	bool load;
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
		 *
		 * EXCEPT under the darken clamp, where the destination is exactly what
		 * the pass needs: level 0 still holds the unblurred source, and
		 * VK_BLEND_OP_MIN reads it. DONT_CARE there would clamp against garbage
		 * -- which on this driver is usually the right answer by accident,
		 * because the memory was that same image a moment ago.
		 */
		.loadOp = p->load ? VK_ATTACHMENT_LOAD_OP_LOAD
			: VK_ATTACHMENT_LOAD_OP_DONT_CARE,
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
	if (p->scissor_w > 0 && p->scissor_h > 0) {
		scissor = (VkRect2D){
			{ p->scissor_x, p->scissor_y },
			{ p->scissor_w, p->scissor_h },
		};
	}
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
	pc.inner_corners[1] = p->effects.linear_src ? 1.0f : 0.0f;

	vkCmdPushConstants(cb, p->pipes->layout,
		VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
		sizeof(pc), &pc);
	vkCmdDraw(cb, 4, 1, 0, 0);
	vkCmdEndRendering(cb);
}

/*
 * ── THE REQUIRED-REGION FALSIFIER ─────────────────────────────────────────
 *
 * AZ_BLUR_REQ_SCISSOR=<what>[,<shrink>]   TEST ONLY.
 *
 *   what    "down1".."down6", "up0".."up5", or "all"
 *   shrink  pixels to remove from each edge of the derived region, default 0
 *
 * Scissors the named pass(es) to the region avk_blur_work_of() says they must
 * render. Combined with AZ_TRANSIENT_POISON=1 -- which fills every transient
 * with garbage before use -- this turns the derivation into an experiment with
 * two halves:
 *
 *   shrink=0   everything outside the derived region is poison, and the final
 *              image must be UNCHANGED. The region is SUFFICIENT.
 *   shrink=1   one strip of the derived region is poison instead, and the
 *              final image must CHANGE. The region is NECESSARY there.
 *
 * A derivation that only ever passes the first half is a region that is big
 * enough, which is not the same claim as the one the denominator makes.
 *
 * Production is unaffected: with the variable unset every pass renders its
 * whole level, exactly as M4E left it. M4F.2D has NOT implemented regional
 * filter execution and this is not a step towards doing so behind the
 * milestone's back -- it is how the number in the report is checked.
 */
struct blur_req_break {
	bool on;
	bool all;
	bool down;          /* true: down pass, false: up pass */
	uint32_t level;
	int32_t shrink;
};

static struct blur_req_break blur_req_break(void) {
	static struct blur_req_break cached;
	static bool read;
	if (read) {
		return cached;
	}
	read = true;
	const char *env = getenv("AZ_BLUR_REQ_SCISSOR");
	if (env == NULL || env[0] == '\0') {
		return cached;
	}
	char what[32] = {0};
	int shrink = 0;
	if (sscanf(env, "%31[^,],%d", what, &shrink) < 1) {
		avk_log(AVK_ERROR, "AZ_BLUR_REQ_SCISSOR=%s is not <what>[,<shrink>]",
			env);
		return cached;
	}
	cached.shrink = shrink;
	if (strcmp(what, "all") == 0) {
		cached.on = true;
		cached.all = true;
	} else if (strncmp(what, "down", 4) == 0) {
		cached.on = true;
		cached.down = true;
		cached.level = (uint32_t)atoi(what + 4);
	} else if (strncmp(what, "up", 2) == 0) {
		cached.on = true;
		cached.down = false;
		cached.level = (uint32_t)atoi(what + 2);
	} else {
		avk_log(AVK_ERROR, "AZ_BLUR_REQ_SCISSOR: %s is not down<N>/up<N>/all",
			what);
		return cached;
	}
	avk_log(AVK_WARN, "AZ_BLUR_REQ_SCISSOR=%s -- blur passes render only their "
		"DERIVED REQUIRED region, shrunk by %d. This build is a measurement "
		"instrument, not a renderer.", env, shrink);
	return cached;
}

/*
 * ── M4F.2D.2: THE UP0-ONLY SCISSOR ────────────────────────────────────────
 *
 * The FINAL, full-resolution upsample renders only the region
 * avk_blur_work_of() derived for it -- and nothing else in the chain changes.
 * ON BY DEFAULT; AZ_BLUR_UP0_SCISSOR=0 restores the baseline, which renders
 * every pass whole exactly as it always has. Both sides are the same binary
 * and differ in one scissor, because an A/B needs both in one build.
 *
 * WHY THIS PASS AND ONLY THIS PASS. Measured on this hardware at levels=3
 * r=5: the up chain is 71% of blur GPU time and the final upsample alone is
 * 58-60% of the whole chain -- it rasterises at the full capture extent while
 * every other pass is a quarter of it or less. Its required region is also the
 * one the falsifier proved TIGHT (shrink 1 px/edge -> 851 wrong pixels); the
 * down-chain regions are conservative by at least 8 px/edge, so scissoring
 * them would be acting on a bound that cannot be defended.
 *
 * WHAT IS NOT CHANGED: the target image, its extent, the UV basis, the filter
 * grid, the level extents, the tap count, the pass topology, the barriers.
 * Same image, same coordinates, fewer fragments.
 *
 * COORDINATE SPACE. work.up[0] is in CAPTURE-LOCAL pixels, because
 * avk_blur_work_of() was handed the result region already translated by the
 * capture origin and level 0 is the capture. The final upsample's destination
 * IS the capture-sized transient, and vkCmdSetScissor takes that image's own
 * pixels. The two are the same space, so there is no conversion -- which is
 * the reason this pass was the cheap one to try.
 */
/*
 * QUALIFIED. The derived region is sufficient (rendering exactly it is
 * pixel-identical) and necessary (shrinking it by one pixel per edge breaks
 * the frame), and every measured workload removed exactly the predicted number
 * of fragments with the graph and resource topology unchanged.
 */
static bool blur_up0_scissor_enabled(void) {
	static int cached = -1;
	if (cached < 0) {
		const char *env = getenv("AZ_BLUR_UP0_SCISSOR");
		cached = !(env != NULL && env[0] == '0');
		if (!cached) {
			avk_log(AVK_INFO, "avk blur: AZ_BLUR_UP0_SCISSOR=0 -- the final "
				"upsample renders its whole level, as it did before M4F.2D.2");
		}
	}
	return cached != 0;
}

bool blur_up0_scissor_on(void) {
	return blur_up0_scissor_enabled();
}

/* The derived region for one pass, with the break's shrink applied. Returns
 * false when this pass is not the one being scissored. */
static bool blur_req_scissor(const struct avk_blur_work *work, bool down,
		uint32_t level, int32_t *x, int32_t *y, uint32_t *w, uint32_t *h) {
	struct blur_req_break b = blur_req_break();
	if (!b.on || work == NULL) {
		return false;
	}
	if (!b.all && (b.down != down || b.level != level)) {
		return false;
	}
	const struct avk_blur_level_work *lw = down ? &work->down[level]
		: &work->up[level];
	int32_t rx = (int32_t)lw->req_x + b.shrink;
	int32_t ry = (int32_t)lw->req_y + b.shrink;
	int32_t rw = (int32_t)lw->req_w - 2 * b.shrink;
	int32_t rh = (int32_t)lw->req_h - 2 * b.shrink;
	if (rw <= 0 || rh <= 0) {
		return false;
	}
	*x = rx;
	*y = ry;
	*w = (uint32_t)rw;
	*h = (uint32_t)rh;
	return true;
}

bool avk_blur_declare(struct avk_graph *graph, struct avk_transient_pool *pool,
		struct avk_pipelines *pipes, struct avk_blur_stats *stats,
		uint32_t src_resource, uint32_t dst_resource,
		uint32_t width, uint32_t height, VkFormat format,
		const struct avk_blur_params *params,
		const struct avk_blur_marks *marks,
		const struct avk_blur_work *work) {
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

	/*
	 * A CALLER THAT DOES NOT WANT THE ACCOUNTING PASSES NULL.
	 *
	 * Every other use in this file guards with `if (stats)`, and two of them
	 * -- the processed_pixels lines added in 61fb98e -- did not. The unit test
	 * passes NULL because it is testing the filter, not the counters, so
	 * avk_blur_declare segfaulted on the second call. A per-use guard is the
	 * fragile pattern: it has to be remembered at every new counter, and it
	 * was not. Pointing `stats` at a scratch removes the class instead.
	 */
	struct avk_blur_stats scratch;
	if (stats == NULL) {
		memset(&scratch, 0, sizeof(scratch));
		stats = &scratch;
	}

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
		blur_req_scissor(work, true, i, &p->scissor_x, &p->scissor_y,
			&p->scissor_w, &p->scissor_h);
		/* Rectangle arithmetic, not a per-pixel loop: what this pass WILL
		 * process, so M4F.2D can weigh a per-level scissor against the result
		 * area it would keep. */
		stats->processed_pixels += (uint64_t)dw * (uint64_t)dh;

		uint32_t from = i == 1 ? src_resource : level_res[i - 1];
		avk_graph_pass_begin(graph, i == 1 ? "blur_down_0" : "blur_down",
			record_blur_pass, p);
		avk_graph_use(graph, from, AVK_USE_SAMPLED_READ, NULL);
		avk_graph_use(graph, level_res[i], AVK_USE_COLOR_WRITE, NULL);
		/* The LAST downsample carries the phase mark, so the pair measures the
		 * whole downsample chain rather than one level of it. */
		if (marks != NULL && i == levels) {
			avk_graph_pass_time(graph, AVK_TS_NONE, marks->down_end);
		}
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

		/*
		 * THE DARKEN CLAMP, and it is only ever the last pass.
		 *
		 * `src_resource == dst_resource` on the live path -- the chain writes its
		 * final upsample back into the prefix transient it read -- so at this
		 * pass the destination still holds the UNBLURRED prefix. That is the same
		 * property the reference relies on (blur2.comp's comment: "the chain
		 * walks away from mip 0 and only returns on this last pass"), and it is
		 * what makes min() free.
		 *
		 * If a caller ever blurs A into a different image B, the destination is
		 * not the source and there is nothing meaningful to clamp against. Asked
		 * here rather than assumed, because "usually the same image" is exactly
		 * the kind of premise that stops holding without anyone noticing.
		 */
		bool darken = last && params->darken && src_resource == dst_resource;
		if (last && params->darken && !darken) {
			avk_log(AVK_WARN, "avk blur: darken asked for on a chain whose "
				"destination is not its source; there is no unblurred source to "
				"clamp against, so the clamp is skipped");
		}

		struct blur_pass *p = &g_passes[g_pass_len++];
		memset(p, 0, sizeof(*p));
		p->pipes = pipes;
		p->pipeline = darken ? pipes->blur_up_darken : pipes->blur_up;
		p->load = darken;
		p->src = level_img[i];
		p->dst = last ? graph->resources[dst_resource].image
			: level_img[i - 1];
		p->dst_w = dw;
		p->dst_h = dh;
		blur_uv(p, src_w, src_h, radius);
		/*
		 * THE ONE OPTIMISATION UNDER EVALUATION. The falsifier's own switch
		 * takes precedence when set, because it is the diagnostic that proved
		 * this region and must be able to override the thing it validates.
		 */
		uint32_t rendered_w = dw, rendered_h = dh;
		if (!blur_req_scissor(work, false, i - 1, &p->scissor_x, &p->scissor_y,
					&p->scissor_w, &p->scissor_h)
				&& last && work != NULL && blur_up0_scissor_enabled()
				&& work->up[0].req_w > 0 && work->up[0].req_h > 0) {
			p->scissor_x = (int32_t)work->up[0].req_x;
			p->scissor_y = (int32_t)work->up[0].req_y;
			p->scissor_w = work->up[0].req_w;
			p->scissor_h = work->up[0].req_h;
		}
		if (p->scissor_w > 0 && p->scissor_h > 0) {
			/* WHAT WILL ACTUALLY BE SHADED. Counting the full extent here
			 * while a scissor removes most of it would make the accounting
			 * describe a frame that did not happen. */
			rendered_w = p->scissor_w;
			rendered_h = p->scissor_h;
			/*
			 * THE COORDINATE MAPPING, SAID OUT LOUD ONCE.
			 *
			 * up0's derived region is CAPTURE-LOCAL and vkCmdSetScissor takes
			 * TARGET-LOCAL pixels; for this pass the destination is the
			 * capture-sized transient, so the two are the same space and the
			 * numbers below must be identical. A fixture whose capture happens
			 * to start at 0,0 cannot tell a correct mapping from one that
			 * forgot to subtract the origin -- so the origin is printed with
			 * them and a test asserts on a non-zero one.
			 */
			/*
			 * BOUNDS, ASSERTED RATHER THAN CLAMPED. A required region outside
			 * the target means the derivation or the coordinate conversion is
			 * wrong, and silently clamping it would render a correct-looking
			 * frame from a broken premise -- which is the failure mode this
			 * whole milestone exists to stop.
			 */
			if (p->scissor_x < 0 || p->scissor_y < 0
					|| (uint32_t)p->scissor_x + p->scissor_w > dw
					|| (uint32_t)p->scissor_y + p->scissor_h > dh) {
				avk_log(AVK_ERROR, "avk blur: up0 scissor %ux%u at %d,%d is "
					"outside its %ux%u target -- DERIVATION OR CONVERSION IS "
					"WRONG", p->scissor_w, p->scissor_h, p->scissor_x,
					p->scissor_y, dw, dh);
			}
			static bool said;
			if (!said && last) {
				said = true;
				avk_log(AVK_INFO, "avk blur: up0 scissor: target %ux%u, "
					"req %ux%u at %u,%u -> VkRect2D %dx%d at %d,%d",
					dw, dh, work->up[0].req_w, work->up[0].req_h,
					work->up[0].req_x, work->up[0].req_y,
					p->scissor_w, p->scissor_h, p->scissor_x, p->scissor_y);
			}
		}
		stats->processed_pixels += (uint64_t)rendered_w * (uint64_t)rendered_h;
		if (last) {
			/* The final full-resolution upsample, on its own. The scissor only
			 * ever touches this pass, so this is the ONLY quantity that can be
			 * compared against gpu_blur_up0 -- reporting the reduction as a
			 * share of the whole chain would understate it by the amount of
			 * work the prototype never claimed to remove. */
			stats->up0_pixels +=
				(uint64_t)rendered_w * (uint64_t)rendered_h;
		}
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
		/* The final upsample is the last blur work in the frame for THIS
		 * chain; the caller decides whether it is the last one overall. */
		if (marks != NULL && last) {
			avk_graph_pass_time(graph, AVK_TS_NONE, marks->up_end);
		} else if (marks != NULL && i == 2) {
			/* The pass writing level 1: the last one before the final,
			 * full-resolution upsample. */
			avk_graph_pass_time(graph, AVK_TS_NONE, marks->up_penult_end);
		}
		avk_graph_pass_end(graph);
		if (stats) { stats->passes++; }
	}

	if (stats) { stats->chains++; }
	return true;
}

/*
 * ── ACTUAL VS REQUIRED, PER PASS ──────────────────────────────────────────
 *
 * See the header for the derivation. The only thing worth repeating here is
 * which constants are used, because using a second set would make this a
 * different filter's arithmetic and the ratio meaningless:
 *
 *     down: (0.5 * radius + 1) texels of the level being READ
 *     up:   (radius + 1)       texels of the level being READ
 *
 * -- exactly the two terms support_axis() sums, and level_extent()/texel_span()
 * for the mapping between levels.
 */

/* A box in one level's texels, clamped to that level's extent. Half-open. */
struct az_lvl_box {
	double x0, y0, x1, y1;
};

static struct az_lvl_box lvl_clamp(struct az_lvl_box b, uint32_t w, uint32_t h)
{
	if (b.x0 < 0.0) { b.x0 = 0.0; }
	if (b.y0 < 0.0) { b.y0 = 0.0; }
	if (b.x1 > (double)w) { b.x1 = (double)w; }
	if (b.y1 > (double)h) { b.y1 = (double)h; }
	if (b.x1 < b.x0) { b.x1 = b.x0; }
	if (b.y1 < b.y0) { b.y1 = b.y0; }
	return b;
}

/* level `from` texels -> level `to` texels, through source pixels. Exact for
 * odd extents because both directions go through texel_span(). */
static struct az_lvl_box lvl_map(struct az_lvl_box b, uint32_t base_w,
		uint32_t base_h, uint32_t from, uint32_t to)
{
	double sx_from = texel_span(base_w, from), sy_from = texel_span(base_h, from);
	double sx_to = texel_span(base_w, to), sy_to = texel_span(base_h, to);
	struct az_lvl_box r = {
		.x0 = b.x0 * sx_from / sx_to,
		.y0 = b.y0 * sy_from / sy_to,
		.x1 = b.x1 * sx_from / sx_to,
		.y1 = b.y1 * sy_from / sy_to,
	};
	return r;
}

static struct az_lvl_box lvl_dilate(struct az_lvl_box b, double t)
{
	b.x0 -= t; b.y0 -= t; b.x1 += t; b.y1 += t;
	return b;
}

static struct az_lvl_box lvl_union(struct az_lvl_box a, struct az_lvl_box b)
{
	if (a.x1 <= a.x0 || a.y1 <= a.y0) { return b; }
	if (b.x1 <= b.x0 || b.y1 <= b.y0) { return a; }
	struct az_lvl_box r = {
		.x0 = a.x0 < b.x0 ? a.x0 : b.x0,
		.y0 = a.y0 < b.y0 ? a.y0 : b.y0,
		.x1 = a.x1 > b.x1 ? a.x1 : b.x1,
		.y1 = a.y1 > b.y1 ? a.y1 : b.y1,
	};
	return r;
}

/* Whole texels: a fractional edge means that texel is partly needed, and a
 * partly needed texel is needed. Floor the near edge, ceil the far one. */
static void lvl_snap(struct az_lvl_box b, uint32_t w, uint32_t h,
		uint32_t *x, uint32_t *y, uint32_t *ow, uint32_t *oh)
{
	b = lvl_clamp(b, w, h);
	double x0 = floor(b.x0), y0 = floor(b.y0);
	double x1 = ceil(b.x1), y1 = ceil(b.y1);
	if (x0 < 0.0) { x0 = 0.0; }
	if (y0 < 0.0) { y0 = 0.0; }
	if (x1 > (double)w) { x1 = (double)w; }
	if (y1 > (double)h) { y1 = (double)h; }
	*x = (uint32_t)x0;
	*y = (uint32_t)y0;
	*ow = x1 > x0 ? (uint32_t)(x1 - x0) : 0;
	*oh = y1 > y0 ? (uint32_t)(y1 - y0) : 0;
}

bool avk_blur_work_of(const struct avk_blur_params *params, uint32_t width,
		uint32_t height, const struct avk_box *result,
		struct avk_blur_work *out)
{
	if (out == NULL) {
		return false;
	}
	memset(out, 0, sizeof(*out));
	if (params == NULL || width == 0 || height == 0 || result == NULL) {
		return false;
	}
	uint32_t levels = effective_levels(params, width, height);
	if (levels == 0) {
		return false;
	}
	out->levels = levels;
	double radius = params->radius > 0.0f ? (double)params->radius : 1.0;
	const double down_reach = 0.5 * radius + 1.0;   /* texels of the level read */
	const double up_reach = radius + 1.0;

	/* O(0): the demanded result, in level-0 texels. */
	struct az_lvl_box O[AVK_BLUR_MAX_LEVELS + 1];
	struct az_lvl_box U[AVK_BLUR_MAX_LEVELS + 1];
	struct az_lvl_box D[AVK_BLUR_MAX_LEVELS + 1];
	O[0] = (struct az_lvl_box){
		.x0 = (double)result->x, .y0 = (double)result->y,
		.x1 = (double)result->x + (double)result->width,
		.y1 = (double)result->y + (double)result->height,
	};
	O[0] = lvl_clamp(O[0], width, height);

	/* Up chain, walked from the top down: what each level must supply. */
	for (uint32_t i = 1; i <= levels; i++) {
		struct az_lvl_box m = lvl_map(O[i - 1], width, height, i - 1, i);
		U[i] = lvl_clamp(lvl_dilate(m, up_reach),
			level_extent(width, i), level_extent(height, i));
		O[i] = U[i];
	}

	/* Down chain, from the deepest level back: each level must ALSO supply
	 * what the next down pass reads from it. */
	D[levels] = U[levels];
	for (uint32_t i = levels; i >= 2; i--) {
		struct az_lvl_box m = lvl_map(D[i], width, height, i, i - 1);
		struct az_lvl_box need = lvl_dilate(m, down_reach);
		D[i - 1] = lvl_clamp(lvl_union(U[i - 1], need),
			level_extent(width, i - 1), level_extent(height, i - 1));
	}
	/* And level 0 -- the capture -- must supply the first down pass. */
	{
		struct az_lvl_box m = lvl_map(D[1], width, height, 1, 0);
		struct az_lvl_box need = lvl_clamp(lvl_dilate(m, down_reach),
			width, height);
		lvl_snap(need, width, height, &out->capture_req_x, &out->capture_req_y,
			&out->capture_req_w, &out->capture_req_h);
	}

	for (uint32_t i = 1; i <= levels; i++) {
		uint32_t ew = level_extent(width, i), eh = level_extent(height, i);
		struct avk_blur_level_work *w = &out->down[i];
		w->level = i;
		w->extent_w = ew;
		w->extent_h = eh;
		w->actual_px = (uint64_t)ew * (uint64_t)eh;
		lvl_snap(D[i], ew, eh, &w->req_x, &w->req_y, &w->req_w, &w->req_h);
		w->required_px = (uint64_t)w->req_w * (uint64_t)w->req_h;
		/* What it reads from level i-1 to render that. */
		{
			struct az_lvl_box m = lvl_map(D[i], width, height, i, i - 1);
			struct az_lvl_box in = lvl_dilate(m, down_reach);
			w->in_level = i - 1;
			lvl_snap(in, level_extent(width, i - 1), level_extent(height, i - 1),
				&w->in_x, &w->in_y, &w->in_w, &w->in_h);
		}
		out->actual_px += w->actual_px;
		out->required_px += w->required_px;
	}
	for (uint32_t i = levels; i >= 1; i--) {
		uint32_t ew = level_extent(width, i - 1), eh = level_extent(height, i - 1);
		struct avk_blur_level_work *w = &out->up[i - 1];
		w->level = i - 1;
		w->extent_w = ew;
		w->extent_h = eh;
		w->actual_px = (uint64_t)ew * (uint64_t)eh;
		lvl_snap(O[i - 1], ew, eh, &w->req_x, &w->req_y, &w->req_w, &w->req_h);
		w->required_px = (uint64_t)w->req_w * (uint64_t)w->req_h;
		/* What it reads from level i -- U(i), the dilated mapping. This is the
		 * one that is wider than the demand; the rendered region is not. */
		w->in_level = i;
		lvl_snap(U[i], level_extent(width, i), level_extent(height, i),
			&w->in_x, &w->in_y, &w->in_w, &w->in_h);
		out->actual_px += w->actual_px;
		out->required_px += w->required_px;
	}
	return true;
}
