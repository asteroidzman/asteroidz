#define _POSIX_C_SOURCE 200809L

#include "avk_render.h"

#include <stdlib.h>
#include "../debug/avk_debug.h"

#include <inttypes.h>
#include <math.h>
#include <string.h>
#include <time.h>

bool avk_renderer_init(struct avk_renderer *renderer, struct avk_device *dev,
		VkFormat format) {
	memset(renderer, 0, sizeof(*renderer));
	pixman_region32_init(&renderer->frame_damage);
	renderer->dev = dev;
	renderer->format = format;
	avk_oracle_init(&renderer->oracle, dev);

	if (!avk_pipelines_init(&renderer->pipes, dev, format)) {
		return false;
	}
	if (!avk_cmd_ring_init(&renderer->ring, dev, "avk frame")) {
		avk_pipelines_finish(&renderer->pipes);
		return false;
	}
	avk_retire_init(&renderer->retire, "renderer");
	renderer->ring.retire = &renderer->retire;
	if (!avk_gradient_store_init(&renderer->gradients, dev,
			renderer->pipes.gradient_set_layout, &renderer->retire)) {
		avk_retire_finish(&renderer->retire, dev);
		avk_cmd_ring_finish(&renderer->ring);
		avk_pipelines_finish(&renderer->pipes);
		return false;
	}

	/* M4D.P. A device that cannot measure itself must still draw, so a false
	 * return here is recorded and ignored rather than failing init. */
	avk_timestamps_init(&renderer->timestamps, dev);
	avk_graph_init(&renderer->graph, dev);
	avk_transient_pool_init(&renderer->transients, dev, &renderer->retire);

	/* M4A breaks, read once. Each restores a specific wrong implementation
	 * rather than merely disabling the feature -- "single radius" and "scaled
	 * twice" are the two mistakes that render plausibly and are therefore the
	 * ones worth having a falsifier for. */
	renderer->break_rounded_off = getenv("AZ_ROUNDED_OFF") != NULL;
	renderer->break_rounded_single = getenv("AZ_ROUNDED_SINGLE_RADIUS") != NULL;
	renderer->break_bottom_swap = getenv("AZ_ROUNDED_BOTTOM_SWAP") != NULL;
	renderer->break_shadow_single_radius =
		getenv("AZ_SHADOW_SINGLE_RADIUS") != NULL;
	renderer->break_shadow_symmetric =
		getenv("AZ_SHADOW_SYMMETRIC") != NULL;
	renderer->break_shadow_no_dither =
		getenv("AZ_SHADOW_NO_DITHER") != NULL;
	renderer->break_blur_scene_after =
		getenv("AZ_BLUR_SCENE_AFTER") != NULL;
	if (renderer->break_blur_scene_after) {
		avk_log(AVK_ERROR, "M4F break switch active: a blur's source is the "
			"WHOLE scene, not the prefix behind it -- content drawn after the "
			"blur node will contaminate it");
	}
	renderer->break_blur_ignore_darken =
		getenv("AZ_BLUR_IGNORE_DARKEN") != NULL;
	renderer->break_blur_ignore_clip = getenv("AZ_BLUR_IGNORE_CLIP") != NULL;
	const char *edge_logical = getenv("AZ_BLUR_EDGE_LOGICAL_SIGMA");
	renderer->break_blur_edge_logical_sigma = edge_logical != NULL;
	renderer->break_blur_edge_scale = edge_logical != NULL
		? (float)atof(edge_logical) : 1.0f;
	if (renderer->break_blur_edge_scale <= 0.0f) {
		renderer->break_blur_edge_scale = 1.5f;
	}
	renderer->break_blur_under_damage =
		getenv("AZ_BLUR_UNDER_DAMAGE") != NULL;
	if (renderer->break_blur_under_damage) {
		avk_log(AVK_ERROR, "M4F.2B break switch active: a blur's source damage "
			"is NOT expanded by the filter support -- expect a stale ring "
			"around everything that changes");
	}
	/*
	 * THE ORACLE, not a break. Forces the full dependency rebuild and the full
	 * write-region recomposition, using the same current-frame prefix
	 * architecture and the same material -- so a partial frame that differs
	 * from it differs because of damage and for no other reason. This is NOT
	 * the reference's historical-source path and does not restore it.
	 */
	renderer->blur_full_damage = getenv("AZ_BLUR_FULL_DAMAGE") != NULL;
	renderer->break_blur_source_output_clip =
		getenv("AZ_BLUR_SOURCE_OUTPUT_CLIP") != NULL;
	if (renderer->break_blur_source_output_clip) {
		avk_log(AVK_ERROR, "M4F.2C break switch active: a blur's source is "
			"clamped to its own output -- expect a seam through every window "
			"that spans two displays");
	}
	if (renderer->break_blur_ignore_darken || renderer->break_blur_ignore_clip
			|| renderer->break_blur_edge_logical_sigma) {
		avk_log(AVK_ERROR, "M4F.2A.3 break switch active: blur material is "
			"deliberately wrong (no_darken=%d no_clip=%d edge_logical=%d @ %.3f)",
			renderer->break_blur_ignore_darken,
			renderer->break_blur_ignore_clip,
			renderer->break_blur_edge_logical_sigma,
			renderer->break_blur_edge_scale);
	}
	/*
	 * Derived from the ATTACHMENT's precision, once, at init -- so a 10-bit
	 * output gets a quarter of the amplitude and an FP16 one gets none,
	 * without the shader knowing what it is drawing into.
	 *
	 * AZ_SHADOW_DITHER_AMP overrides it in 1/255 units, which is what the
	 * amplitude sweep in test_dither_breaks_banding() drives.
	 */
	renderer->dither_hash = getenv("AZ_DITHER_HASH") != NULL;
	const char *amp = getenv("AZ_SHADOW_DITHER_AMP");
	renderer->shadow_dither = amp != NULL
		? (float)atof(amp) / 255.0f : avk_dither_amplitude(format);
	const char *dbl = getenv("AZ_ROUNDED_DOUBLE_SCALE");
	renderer->break_rounded_double_scale = dbl != NULL;
	renderer->break_scale_hint = dbl != NULL ? (float)atof(dbl) : 1.0f;
	if (renderer->break_scale_hint <= 0.0f) {
		renderer->break_scale_hint = 1.5f;
	}
	if (renderer->break_rounded_off || renderer->break_rounded_single ||
			renderer->break_rounded_double_scale || renderer->break_bottom_swap) {
		avk_log(AVK_ERROR, "M4A break switch active: rounded clipping is "
			"deliberately wrong (off=%d single=%d double_scale=%d "
			"bottom_swap=%d)",
			renderer->break_rounded_off, renderer->break_rounded_single,
			renderer->break_rounded_double_scale, renderer->break_bottom_swap);
	}
	return true;
}

/*
 * Destroy the renderer's resources. The GPU must already be idle -- the caller
 * establishes that with avk_device_wait_idle(), because it is the caller that
 * knows the order in which several subsystems are coming down. The wait that
 * used to be the first line here was correct about this renderer's own
 * resources and silent about everything destroyed before it.
 */
void avk_renderer_finish(struct avk_renderer *renderer) {
	if (renderer->dev == NULL) {
		return;
	}
	pixman_region32_fini(&renderer->frame_damage);
	avk_oracle_finish(&renderer->oracle);
	/* Before the retire queue is drained: growth pushes old gradient buffers
	 * onto that queue, and finishing the store destroys only the buffers the
	 * slots still hold. */
	free(renderer->blur_results);
	renderer->blur_results = NULL;
	avk_gradient_store_finish(&renderer->gradients);
	/* Before the retire queue is drained, like the gradient store and for the
	 * same reason: dropping a pool entry pushes its image onto that queue. */
	avk_transient_pool_finish(&renderer->transients);
	avk_graph_finish(&renderer->graph);
	avk_timestamps_finish(&renderer->timestamps);
	avk_retire_finish(&renderer->retire, renderer->dev);
	avk_cmd_ring_finish(&renderer->ring);
	avk_pipelines_finish(&renderer->pipes);
	memset(renderer, 0, sizeof(*renderer));
}

void avk_renderer_collect(struct avk_renderer *renderer) {
	/* Before the retire collect, so anything the pool retires this frame is
	 * eligible on the next one rather than sitting a frame longer. */
	avk_transient_pool_collect(&renderer->transients);
	avk_retire_collect(&renderer->retire, renderer->dev);
	/* Reads back only frames the timeline says are finished, so this adds no
	 * wait to a path whose whole point is that it has none. */
	avk_timestamps_collect(&renderer->timestamps);
}

/* ── geometry ───────────────────────────────────────────────────────────────
 *
 * Output pixels in, normalised device coordinates out. Vulkan's Y axis already
 * points down in NDC, which matches the output's top-left origin -- so unlike
 * the GLES path there is no flip here, and no FLIPPED_180 projection to
 * remember. (That projection is exactly what made fx_vk's rounded corners
 * round the wrong edges: gl_FragCoord was in flipped space and only agreed
 * with box space for vertically centred boxes.)
 */
/*
 * Clamp per-corner radii to something the SDF can express.
 *
 * Two independent limits, and both are needed:
 *
 *   - no radius may exceed half the box, or the corner arc would wrap past
 *     the centre and the distance field folds back on itself;
 *   - two radii sharing an edge may not sum to more than that edge, or the
 *     two arcs overlap and the boundary between them is undefined.
 *
 * The second is the one that bites in practice: a 40px radius on both left
 * corners of a 60px-tall window is individually legal and jointly impossible.
 * The fix is the CSS rule -- scale every radius by the worst offending edge's
 * ratio, so the shape shrinks proportionally instead of one corner winning.
 * SceneFX clamps only against the box (corner_radius_clamp is a range check on
 * the config value, not a geometric one) and relies on the shader degrading
 * gracefully; doing it here is a strictly narrower set of shapes, and never
 * produces a boundary the max-of-four SDF cannot evaluate.
 */
static void az_corner_normalise(const float in[4], float w, float h,
		float out[4]) {
	float half = 0.5f * (w < h ? w : h);
	if (w <= 0.0f || h <= 0.0f || half <= 0.0f) {
		out[0] = out[1] = out[2] = out[3] = 0.0f;
		return;
	}
	/* clockwise: tl, tr, br, bl */
	for (int i = 0; i < 4; i++) {
		out[i] = in[i] < 0.0f ? 0.0f : (in[i] > half ? half : in[i]);
	}
	float edges[4] = {
		out[0] + out[1],   /* top:    tl + tr */
		out[1] + out[2],   /* right:  tr + br */
		out[2] + out[3],   /* bottom: br + bl */
		out[3] + out[0],   /* left:   bl + tl */
	};
	float lengths[4] = { w, h, w, h };
	float scale = 1.0f;
	for (int i = 0; i < 4; i++) {
		if (edges[i] > lengths[i] && edges[i] > 0.0f) {
			float s = lengths[i] / edges[i];
			if (s < scale) {
				scale = s;
			}
		}
	}
	if (scale < 1.0f) {
		for (int i = 0; i < 4; i++) {
			out[i] *= scale;
		}
	}
}

/*
 * A box as x0, y0, x1, y1 in OUTPUT PIXELS -- the one space AVK's geometry
 * lives in. The vertex shader turns it into NDC using the viewport it is
 * handed in params.zw, so the rectangle a command covers and the rectangle its
 * signed distance field measures are the same four numbers by construction.
 */
static void box_to_px(const struct avk_box *box, float out[4]) {
	out[0] = (float)box->x;
	out[1] = (float)box->y;
	out[2] = (float)(box->x + box->width);
	out[3] = (float)(box->y + box->height);
}

/*
 * Fold a source crop and a transform into a UV origin and two edge vectors.
 *
 * The quad's parametric coordinate p runs (0,0) to (1,1) across the
 * DESTINATION. This produces the mapping
 *
 *     uv = origin + p.x * dx + p.y * dy
 *
 * so a rotation is a permutation of which corner each edge vector points from,
 * and a flip is a sign. Doing it here rather than in the shader means the
 * eight transforms are eight lines of table rather than eight shader branches,
 * and they can be tested by reading the numbers.
 */
static void transform_uv(const struct avk_fbox *src, uint32_t image_width,
		uint32_t image_height, enum avk_transform transform,
		float origin[2], float dx[2], float dy[2]) {
	/* Normalised source rectangle. */
	float x0 = (float)(src->x / image_width);
	float y0 = (float)(src->y / image_height);
	float w = (float)(src->width / image_width);
	float h = (float)(src->height / image_height);

	/* The four corners of the source rect, in source space. */
	float x1 = x0 + w;
	float y1 = y0 + h;

	/* For each transform, where does destination (0,0) map, and which way do
	 * the destination's x and y axes run in source space? */
	float ox, oy, ax, ay, bx, by;
	switch (transform) {
	/*
	 * ── 90 AND 270 WERE THE WRONG WAY ROUND ───────────────────────────────
	 *
	 * wl_output_transform names the transform from the BUFFER to the SCREEN,
	 * and wlroots' own renderer resolves WL_OUTPUT_TRANSFORM_90 by putting the
	 * source's TOP-RIGHT quadrant at the destination's top-left
	 * (wlr_matrix_project_box + the [[0,1],[-1,0]] matrix). This table had 90
	 * putting the source's BOTTOM-LEFT there, which is the 270 answer -- so
	 * every texture drawn on a 90 or 270 degree output was rotated 180 degrees
	 * inside its own box.
	 *
	 * The six other transforms are their own inverses, so they were unaffected;
	 * only the two that are not could be wrong, and both were.
	 *
	 * WHY NOTHING SAW IT. Every window in the eight-transform pixel oracle's
	 * fixture was a single FLAT COLOUR, and a solid colour rotated by any
	 * amount is the same solid colour. The oracle reported 0 differing pixels
	 * at all eight transforms while a third of the screen was upside down. The
	 * same fixture with four-quadrant windows (WLBGEFFECT_QUAD=1) reports
	 * 167400 of 480000 pixels wrong at 90 and 270, and nothing at all at the
	 * other six. It surfaced through the CURSOR, the first non-uniform texture
	 * anything had ever compared on a rotated output.
	 *
	 * tests/test-avk-render.c asserted the old mapping, agreed with it, and
	 * therefore could not fail on it. Its table now states the wlroots answer.
	 *
	 * The CALLERS were right all along and are unchanged: a command's transform
	 * is compose(invert(buffer transform), output transform), which is exactly
	 * what SceneFX's scene_entry_render() hands its render pass and what
	 * wlroots' own software-cursor path hands its. Only this table disagreed
	 * with them, and only for the two transforms that are not their own
	 * inverse.
	 */
	case AVK_TRANSFORM_90:
		/* dst +x runs along src +y, dst +y along src -x */
		ox = x1; oy = y0; ax = 0; ay = h; bx = -w; by = 0;
		break;
	case AVK_TRANSFORM_180:
		ox = x1; oy = y1; ax = -w; ay = 0; bx = 0; by = -h;
		break;
	case AVK_TRANSFORM_270:
		/* dst +x runs along src -y, dst +y along src +x */
		ox = x0; oy = y1; ax = 0; ay = -h; bx = w; by = 0;
		break;
	case AVK_TRANSFORM_FLIPPED:
		ox = x1; oy = y0; ax = -w; ay = 0; bx = 0; by = h;
		break;
	case AVK_TRANSFORM_FLIPPED_90:
		ox = x0; oy = y0; ax = 0; ay = h; bx = w; by = 0;
		break;
	case AVK_TRANSFORM_FLIPPED_180:
		ox = x0; oy = y1; ax = w; ay = 0; bx = 0; by = -h;
		break;
	case AVK_TRANSFORM_FLIPPED_270:
		ox = x1; oy = y1; ax = 0; ay = -h; bx = -w; by = 0;
		break;
	case AVK_TRANSFORM_NORMAL:
	default:
		ox = x0; oy = y0; ax = w; ay = 0; bx = 0; by = h;
		break;
	}

	origin[0] = ox;
	origin[1] = oy;
	dx[0] = ax;
	dx[1] = ay;
	dy[0] = bx;
	dy[1] = by;
}

/* ── the frame ──────────────────────────────────────────────────────────── */

/*
 * WHERE THE BARRIERS WENT (M4E.1).
 *
 * They used to be built here, by hand, into two batches -- an acquire before
 * the rendering instance and a release after it. They are now DERIVED, by
 * graph/avk_graph.c, from the usages this file declares. The rules they encode
 * did not change and are worth restating because every one of them was learned
 * from validation rather than from the spec, and the graph now owns them:
 *
 *  - INSIDE a dynamic-rendering instance, vkCmdPipelineBarrier2 may carry only
 *    memory barriers, and a layout transition is forbidden outright
 *    (VUID-vkCmdPipelineBarrier2-oldLayout-01181). Sampled surfaces therefore
 *    have to reach SHADER_READ_ONLY_OPTIMAL before vkCmdBeginRendering, which
 *    is why the graph emits a pass's barriers at the pass boundary and not on
 *    demand.
 *  - loadOp LOAD is a COLOR_ATTACHMENT_READ of the target, not only a write, so
 *    AVK_USE_COLOR_WRITE carries both access bits. A barrier covering only the
 *    write leaves the load unsynchronised against whatever produced the previous
 *    contents, reported at vkCmdBeginRendering rather than at the draw.
 *  - A foreign image is ACQUIRED from VK_QUEUE_FAMILY_FOREIGN_EXT and RELEASED
 *    back. The release is what makes a frame visible: without it a scan-out
 *    buffer on a compressed AMD modifier is handed to KMS in a state the display
 *    engine cannot interpret, and the monitor comes up flat white with every
 *    window rendered correctly inside it. No headless test can catch that,
 *    because nothing scans a headless buffer out.
 *
 * What this file kept is the decision of WHICH images the frame touches and
 * HOW; what it gave up is deciding what that implies.
 */

/*
 * BREAK SWITCH: stop stamping last_use on the images a frame SAMPLES, which is
 * what the renderer did until M4F.2C.4e. A client that releases a buffer while
 * the frame reading it is still in flight then has its VkImage destroyed under
 * the GPU. It is a race, so it does not fail every run -- which is exactly why
 * it needs the validation layers rather than a pixel assertion.
 */
static bool avk_no_sampled_last_use(void) {
	static int cached = -1;
	if (cached < 0) {
		const char *env = getenv("AVK_NO_SAMPLED_LAST_USE");
		cached = env != NULL && env[0] == '1';
		if (cached) {
			avk_log(AVK_ERROR, "AVK_NO_SAMPLED_LAST_USE=1 -- images this frame "
				"only READ keep their old last_use, so one destroyed now may "
				"still be in flight. This build is deliberately broken.");
		}
	}
	return cached != 0;
}

/* Read once. Never true in a session anybody is using -- see the loadOp. */
static bool avk_no_load_preserve(void) {
	static int cached = -1;
	if (cached < 0) {
		const char *env = getenv("AVK_NO_LOAD_PRESERVE");
		cached = env != NULL && env[0] == '1';
		if (cached) {
			avk_log(AVK_ERROR, "AVK_NO_LOAD_PRESERVE=1 -- the target's "
				"previous contents are being discarded, so every frame that "
				"redraws less than the whole output will be wrong");
		}
	}
	return cached != 0;
}

/*
 * AVK_NO_FOREIGN_ACQUIRE=1 turns the foreign transfer off -- both halves. On a
 * real output that means a white screen, so it is a diagnostic, not a tuning
 * knob.
 */
static bool az_foreign(const struct avk_image *image) {
	static int no_foreign = -1;
	if (no_foreign < 0) {
		const char *env = getenv("AVK_NO_FOREIGN_ACQUIRE");
		no_foreign = env != NULL && env[0] == '1';
	}
	return !no_foreign && avk_image_is_foreign(image);
}

/*
 * ── REGION ARITHMETIC FOR DAMAGE ──────────────────────────────────────────
 *
 * Two small helpers rather than a dependency. wlroots has wlr_region_expand()
 * and it does exactly this, but nothing under src/render/vulkan/ links against
 * wlroots -- tests/check-vulkan-isolation.py enforces that -- and one function
 * is a smaller price than the exception would be.
 */

/* Dilate every rectangle of `src` by a per-edge support, into an already
 * initialised `dst`, which may not alias `src`. Rounded OUTWARD on every edge:
 * a damage region rounded inward is a stale pixel. */
static void az_region_expand(pixman_region32_t *dst,
		const pixman_region32_t *src, const struct avk_blur_support *by) {
	int32_t l = (int32_t)ceilf(by->left);
	int32_t t = (int32_t)ceilf(by->top);
	int32_t r = (int32_t)ceilf(by->right);
	int32_t b = (int32_t)ceilf(by->bottom);

	int count = 0;
	const pixman_box32_t *rects = pixman_region32_rectangles(
		(pixman_region32_t *)src, &count);
	pixman_region32_clear(dst);
	for (int i = 0; i < count; i++) {
		pixman_region32_union_rect(dst, dst, rects[i].x1 - l, rects[i].y1 - t,
			(unsigned)(rects[i].x2 - rects[i].x1 + l + r),
			(unsigned)(rects[i].y2 - rects[i].y1 + t + b));
	}
}

static uint64_t az_region_area(const pixman_region32_t *region) {
	if (region == NULL) {
		return 0;
	}
	int count = 0;
	const pixman_box32_t *rects = pixman_region32_rectangles(
		(pixman_region32_t *)region, &count);
	uint64_t area = 0;
	for (int i = 0; i < count; i++) {
		area += (uint64_t)(rects[i].x2 - rects[i].x1)
			* (uint64_t)(rects[i].y2 - rects[i].y1);
	}
	return area;
}

/*
 * Past this many rectangles a blur's rebuild region collapses to its extents.
 * Chosen to match wlroots' own damage-ring limit, which collapses at 20: a
 * region that has fragmented further than the compositor's own damage tracking
 * would keep is not one worth walking per level.
 */
#define AVK_BLUR_DAMAGE_MAX_RECTS 20

/* One blur's damage regions, plus what the declaration loop needs to build its
 * chain. Held in a per-frame array so the two sweeps and the declaration can
 * each see what the others computed. */
struct az_blur_slot {
	size_t index;
	struct avk_blur_params params;
	struct avk_blur_damage damage;
};

static void az_blur_damage_init(struct avk_blur_damage *d) {
	pixman_region32_init(&d->write);
	pixman_region32_init(&d->dependency);
	pixman_region32_init(&d->source_damage);
	pixman_region32_init(&d->output_damage);
	pixman_region32_init(&d->result_region);
	pixman_region32_init(&d->prefix_rebuild);
	d->active = false;
}

static void az_blur_damage_finish(struct avk_blur_damage *d) {
	pixman_region32_fini(&d->write);
	pixman_region32_fini(&d->dependency);
	pixman_region32_fini(&d->source_damage);
	pixman_region32_fini(&d->output_damage);
	pixman_region32_fini(&d->result_region);
	pixman_region32_fini(&d->prefix_rebuild);
}

/* The rectangles a command may actually touch: its own clip, intersected with
 * the frame's damage. Returned as a region the caller iterates -- one draw per
 * rectangle, which is what keeps damage meaningful rather than collapsing it
 * to a bounding box that covers the whole screen the moment two corners of the
 * display update. */
/*
 * All of this is in SCENE coordinates. The segment's own extent is expressed
 * there too -- as `bounds` -- so a regional target clips exactly as the output
 * does, and the only place the two spaces meet is the scissor translation at
 * the draw.
 */
static void command_region(const struct avk_cmd *cmd,
		const pixman_region32_t *damage, const struct avk_box *bounds,
		bool ignore_clip, pixman_region32_t *out) {
	pixman_region32_init_rect(out, bounds->x, bounds->y,
		(unsigned)bounds->width, (unsigned)bounds->height);
	pixman_region32_intersect_rect(out, out, cmd->dst.x, cmd->dst.y,
		(unsigned)cmd->dst.width, (unsigned)cmd->dst.height);
	if (cmd->has_clip && !ignore_clip) {
		pixman_region32_intersect(out, out, (pixman_region32_t *)&cmd->clip);
	}
	if (damage != NULL) {
		pixman_region32_intersect(out, out, (pixman_region32_t *)damage);
	}
}

/*
 * The one pass the direct path has: the whole scene, composited into the
 * target.
 *
 * A CALLBACK rather than inline code, because the graph emits this pass's
 * barriers immediately before calling it and the frame's exit barriers
 * immediately after -- and that ordering is the thing that has to be
 * structural. A rendering instance opened by hand between two hand-written
 * batches was correct here for three milestones and would stop being correct
 * the first time a second pass was added above it.
 */
static void az_record_compose(VkCommandBuffer cb, void *user) {
	struct avk_render_segment *ctx = user;
	struct avk_renderer *renderer = ctx->renderer;
	const struct avk_scene *scene = ctx->scene;
	struct avk_image *target = ctx->target;
	const uint32_t width = ctx->width;
	const uint32_t height = ctx->height;
	const VkImageLayout target_layout = ctx->layout;
	VkDescriptorSet gradient_set = ctx->gradient_set;
	/*
	 * The segment's own extent, in SCENE coordinates. Every clip below happens
	 * there; the single translation into attachment coordinates is the scissor,
	 * and the matching one for geometry is AZ_TARGET_ORIGIN in quad.vert.
	 */
	const struct avk_box bounds = {
		ctx->origin_x, ctx->origin_y, (int32_t)width, (int32_t)height,
	};
	const size_t begin = ctx->begin;
	const size_t end = ctx->end > scene->len ? scene->len : ctx->end;
	const pixman_region32_t *damage = ctx->active;

	/*
	 * loadOp LOAD, always.
	 *
	 * A damaged frame redraws only part of the target and the rest must
	 * survive; CLEAR here would be a full-screen clear masquerading as damage
	 * tracking, which looks correct on a full-damage frame and destroys
	 * everything else. The background clear is a normal draw command,
	 * scissored to the damage like everything else.
	 *
	 * AVK_NO_LOAD_PRESERVE=1 replaces it with CLEAR to magenta, which is the
	 * break test for partial damage: it is precisely the mistake described
	 * above, and everything outside the damage becomes a colour nothing else on
	 * a desktop produces.
	 *
	 * DONT_CARE was tried first and MEASURED USELESS as a break. It means the
	 * contents become undefined, and a driver is entitled to leave them alone
	 * -- which is exactly what RADV does on a desktop GPU, where there are no
	 * tiles to avoid loading. The whole test suite passed with it set. A break
	 * switch the hardware is allowed to ignore is not a break switch.
	 */
	bool break_preserve = avk_no_load_preserve();
	VkRenderingAttachmentInfo color = {
		.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
		.imageView = target->view,
		.imageLayout = target_layout,
		/* A REGIONAL target is written whole by the segment that owns it, so
		 * there is nothing to preserve; the output target must preserve
		 * everything outside this frame's damage. */
		.loadOp = break_preserve
			? VK_ATTACHMENT_LOAD_OP_CLEAR
			: (ctx->load ? VK_ATTACHMENT_LOAD_OP_LOAD
				: VK_ATTACHMENT_LOAD_OP_DONT_CARE),
		.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		.clearValue = { .color = { .float32 = { 1.0f, 0.0f, 1.0f, 1.0f } } },
	};

	VkRenderingInfo rendering = {
		.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
		.renderArea = { { 0, 0 }, { width, height } },
		.layerCount = 1,
		.colorAttachmentCount = 1,
		.pColorAttachments = &color,
	};
	vkCmdBeginRendering(cb, &rendering);

	VkViewport viewport = {
		.x = 0, .y = 0,
		.width = (float)width, .height = (float)height,
		.minDepth = 0.0f, .maxDepth = 1.0f,
	};
	vkCmdSetViewport(cb, 0, 1, &viewport);

	/* Once, for the whole frame. Every gradient in it reads the same buffer at
	 * a different offset, so there is one descriptor bind however many
	 * gradients are drawn -- and none at all in a frame that draws none. */
	if (gradient_set != VK_NULL_HANDLE) {
		vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
			renderer->pipes.layout, 1, 1, &gradient_set, 0, NULL);
	}

	VkPipeline bound = VK_NULL_HANDLE;

	/* The background, as a command like any other, so it is damage-clipped
	 * rather than clearing the world. */
	if (scene->has_clear && ctx->clear) {
		struct avk_cmd clear = {
			.type = AVK_CMD_RECT,
			.dst = bounds,
			.opacity = 1.0f,
		};
		memcpy(clear.color, scene->clear_color, sizeof(clear.color));

		pixman_region32_t region;
		command_region(&clear, damage, &bounds, false, &region);
		int count = 0;
		const pixman_box32_t *rects =
			pixman_region32_rectangles(&region, &count);
		if (count > 0) {
			vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
				renderer->pipes.rect);
			bound = renderer->pipes.rect;

			struct avk_push_constants pc = {0};
			pc.uv_dy[2] = (float)ctx->origin_x;
			pc.uv_dy[3] = (float)ctx->origin_y;
			box_to_px(&clear.dst, pc.round_box);
			/* Premultiply: the blend state is (ONE, 1-SRC_ALPHA), so the
			 * shader must receive colour already scaled by alpha. */
			for (int i = 0; i < 3; i++) {
				pc.color[i] = clear.color[i] * clear.color[3];
			}
			pc.color[3] = clear.color[3];
			pc.params[0] = 1.0f;
			pc.params[2] = (float)width;
			pc.params[3] = (float)height;
			vkCmdPushConstants(cb, renderer->pipes.layout,
				VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
				sizeof(pc), &pc);

			for (int i = 0; i < count; i++) {
				/* SCENE -> ATTACHMENT. The clear needs this exactly as much as
				 * a command does: without it the scissor lands at scene
				 * coordinates inside a regional attachment, so a target at
				 * origin 37,53 clears only its bottom-right corner and every
				 * earlier frame's content survives everywhere else. */
				VkRect2D scissor = {
					{ rects[i].x1 - ctx->origin_x,
						rects[i].y1 - ctx->origin_y },
					{ (uint32_t)(rects[i].x2 - rects[i].x1),
						(uint32_t)(rects[i].y2 - rects[i].y1) },
				};
				vkCmdSetScissor(cb, 0, 1, &scissor);
				vkCmdDraw(cb, 4, 1, 0, 0);
				renderer->stats.draws++;
			}
		}
		pixman_region32_fini(&region);
	}

	for (size_t i = begin; i < end; i++) {
		const struct avk_cmd *cmd = &scene->cmds[i];

		if (cmd->has_blur && !renderer->warned_unimplemented_effect) {
			/* Loud once, and still loud: a compositor that quietly renders an
			 * incomplete desktop is worse than one that says so. Shadows came
			 * off this list in M4D; blur goes when M4F lands. */
			avk_log(AVK_WARN, "avk: this scene asks for blur, which is not "
				"implemented yet -- rendering without it (M4F)");
			renderer->warned_unimplemented_effect = true;
		}

		pixman_region32_t region;
		/* AZ_BLUR_IGNORE_CLIP drops the clip for blur commands only:
		 * the composite then covers the whole node instead of the region
		 * the client or the compositor restricted it to. */
		command_region(cmd, damage, &bounds,
			cmd->type == AVK_CMD_BLUR && renderer->break_blur_ignore_clip,
			&region);
		int count = 0;
		const pixman_box32_t *rects =
			pixman_region32_rectangles(&region, &count);
		if (count == 0) {
			pixman_region32_fini(&region);
			continue;
		}

		struct avk_push_constants pc = {0};
		pc.params[0] = cmd->opacity;
		pc.params[2] = (float)width;
		pc.params[3] = (float)height;
		/* The one place a command learns which target it is going to. Its own
		 * geometry stays in scene coordinates; see AZ_TARGET_ORIGIN. */
		pc.uv_dy[2] = (float)ctx->origin_x;
		pc.uv_dy[3] = (float)ctx->origin_y;
		/*
		 * The rounding rectangle is the destination, in OUTPUT PIXELS -- the
		 * same space gl_FragCoord is in. Not NDC: a signed distance is only
		 * meaningful where the units are uniform, and NDC's are not.
		 *
		 * Radii arrive already scaled to output pixels (the compositor scales
		 * them where it scales the box, so the two cannot disagree), and are
		 * CLOCKWISE: tl, tr, br, bl.
		 */
		box_to_px(&cmd->dst, pc.round_box);
		/*
		 * BREAK SWITCHES, all three at the point the radii become shader
		 * input, because that is the narrowest place that can express them.
		 *
		 *   rounded-clip          no rounding at all
		 *   rounded-single-radius all four corners take the first one, which
		 *                         is what a single-scalar implementation does
		 *                         and what the audit found SceneFX does NOT do
		 *   rounded-double-scale  scale applied twice, the fractional-scale
		 *                         mistake that looks fine at scale 1.0
		 */
		float radii[4] = { cmd->corners[0], cmd->corners[1],
			cmd->corners[2], cmd->corners[3] };
		if (renderer->break_bottom_swap) {
			/* The exact bug the M4A audit found waiting to happen:
			 * fx_corner_radii is CLOCKWISE (tl, tr, br, bl) and SceneFX's own
			 * shader helper takes (tl, tr, bl, br). Handing the struct
			 * straight over swaps the two bottom corners -- which is only
			 * visible when they differ, i.e. never in a symmetric test. */
			float t = radii[2];
			radii[2] = radii[3];
			radii[3] = t;
		}
		if (renderer->break_rounded_single) {
			radii[1] = radii[2] = radii[3] = radii[0];
		}
		if (renderer->break_rounded_double_scale) {
			for (int i = 0; i < 4; i++) {
				radii[i] *= renderer->break_scale_hint;
			}
		}
		if (renderer->break_rounded_off) {
			radii[0] = radii[1] = radii[2] = radii[3] = 0.0f;
		}
		az_corner_normalise(radii, (float)cmd->dst.width,
			(float)cmd->dst.height, pc.corners);
		if (pc.corners[0] > 0.0f || pc.corners[1] > 0.0f ||
				pc.corners[2] > 0.0f || pc.corners[3] > 0.0f) {
			renderer->stats.rounded_clip_draws++;
			if (pc.corners[0] != pc.corners[1] ||
					pc.corners[1] != pc.corners[2] ||
					pc.corners[2] != pc.corners[3]) {
				renderer->stats.rounded_asymmetric_draws++;
			}
		}

		/*
		 * The INNER edge of the annulus. Every break above is deliberately
		 * NOT reapplied here: those describe how a single rounded rectangle
		 * can be got wrong, and the inner edge is a rounded rectangle in its
		 * own right whose radii the compositor computed separately (outer
		 * radius minus border width minus one). Re-deriving them from the
		 * outer ones would put a rule in the renderer that already lives in
		 * apply_border(), and the two would drift.
		 *
		 * Normalised against the INNER box, not the outer one -- a hole is
		 * clamped by its own size, and using the outer dimensions lets a
		 * radius exceed half the hole on a narrow window.
		 */
		if (cmd->has_inner) {
			box_to_px(&cmd->inner, pc.inner_box);
			float inner[4] = { cmd->inner_corners[0], cmd->inner_corners[1],
				cmd->inner_corners[2], cmd->inner_corners[3] };
			az_corner_normalise(inner, (float)cmd->inner.width,
				(float)cmd->inner.height, pc.inner_corners);
			renderer->stats.border_draws++;
			if (pc.inner_corners[0] > 0.0f || pc.inner_corners[1] > 0.0f ||
					pc.inner_corners[2] > 0.0f || pc.inner_corners[3] > 0.0f) {
				renderer->stats.rounded_border_draws++;
				if (pc.inner_corners[0] != pc.inner_corners[1] ||
						pc.inner_corners[1] != pc.inner_corners[2] ||
						pc.inner_corners[2] != pc.inner_corners[3]) {
					renderer->stats.asymmetric_border_draws++;
				}
			}
		}

		VkPipeline want;
		if (cmd->type == AVK_CMD_SHADOW) {
			want = renderer->pipes.shadow;
			/*
			 * STRAIGHT rgb, not premultiplied, and this is the one command
			 * type that hands the shader an unpremultiplied colour on purpose.
			 * A shadow's final alpha is not known until the shader has
			 * evaluated its coverage, so the premultiply has to happen there;
			 * doing it here as well would apply the caster's alpha twice.
			 */
			for (int c = 0; c < 4; c++) {
				pc.color[c] = cmd->color[c];
			}
			/* Same slot the texture pipeline uses for its alpha mask and the
			 * gradient pipeline for its record index. They never draw the same
			 * command; see push.glsl. */
			pc.params[1] = cmd->blur_sigma;
			/*
			 * M4D.4. Peak-to-peak dither, in the slot a shadow does not use
			 * for anything else. Tuned by measurement, not derived: the step
			 * a viewer sees is in the FRAMEBUFFER, and a black shadow
			 * composites as dst*(1-alpha), so the amplitude that hides a band
			 * scales with the backdrop -- which the shader cannot read. Dark
			 * backdrops both show shadows best and have fewest levels to draw
			 * them in, so the constant is chosen there and checked not to be
			 * visible as noise on light ones.
			 */
			pc.uv_org_dx[0] = renderer->break_shadow_no_dither
				? 0.0f : renderer->shadow_dither;
			pc.uv_dy[0] = renderer->dither_hash ? 1.0f : 0.0f;

			if (renderer->break_shadow_symmetric && cmd->has_inner) {
				/* Slide the envelope until its centre is the window's. The
				 * size is untouched, so the falloff is the same shape -- only
				 * its direction is gone. */
				float win_cx = (float)cmd->inner.x
					+ (float)cmd->inner.width * 0.5f;
				float win_cy = (float)cmd->inner.y
					+ (float)cmd->inner.height * 0.5f;
				float env_cx = (pc.round_box[0] + pc.round_box[2]) * 0.5f;
				float env_cy = (pc.round_box[1] + pc.round_box[3]) * 0.5f;
				pc.round_box[0] += win_cx - env_cx;
				pc.round_box[2] += win_cx - env_cx;
				pc.round_box[1] += win_cy - env_cy;
				pc.round_box[3] += win_cy - env_cy;
			}
			if (renderer->break_shadow_single_radius) {
				/* Applied to the push constants and not to the command, so
				 * the stats below still report what the SCENE asked for --
				 * a break that also hid its own effect from the counters
				 * would be much harder to recognise in a failure. */
				pc.corners[1] = pc.corners[2] = pc.corners[3] =
					pc.corners[0];
			}
			renderer->stats.shadow_draws++;
			if (cmd->corners[0] > 0.0f || cmd->corners[1] > 0.0f ||
					cmd->corners[2] > 0.0f || cmd->corners[3] > 0.0f) {
				renderer->stats.rounded_shadow_draws++;
				if (cmd->corners[0] != cmd->corners[1] ||
						cmd->corners[1] != cmd->corners[2] ||
						cmd->corners[2] != cmd->corners[3]) {
					renderer->stats.asymmetric_shadow_draws++;
				}
			}
		} else if (cmd->type == AVK_CMD_BLUR) {
			/*
			 * The blurred result, as an ordinary textured quad.
			 *
			 * By the time any segment reaches this command its chain has
			 * already run -- avk_render_frame declares every prefix capture and
			 * blur chain, in increasing scene order, before it declares the
			 * pass that draws them. So a LATER blur's prefix replay, which
			 * covers this command, finds a finished result here and composites
			 * it exactly as the output pass does. That is what makes an earlier
			 * blur legitimately contribute to a later one without any recursion
			 * and without blur being special-cased out of prefix replay.
			 */
			const struct avk_blur_result *br = i < renderer->blur_results_cap
				? &renderer->blur_results[i] : NULL;
			struct avk_image *result = br != NULL ? br->image : NULL;
			if (result == NULL) {
				/* No chain ran for it -- zero levels, or a region too small.
				 * Drawing nothing is correct; the backdrop shows through. */
				pixman_region32_fini(&region);
				continue;
			}
			/*
			 * TWO EDGES, ONE COMPOSITE. `edge_softness` decides which coverage
			 * function shades the same quad with the same UVs -- nothing else
			 * about the draw changes, and the blur passes that produced the
			 * image know about neither.
			 */
			if (cmd->blur_edge_softness > 0.0f) {
				want = renderer->pipes.blur_soft;
				/* The same slot a shadow's sigma uses, for the same quantity;
				 * see push.glsl. Set AFTER the block below, which writes 1.0
				 * there for the hard-edged case. */
				renderer->stats.blur_soft_draws++;
			} else {
				want = renderer->pipes.texture;
			}
			/*
			 * The result transient covers the CAPTURE region and may be larger
			 * than the write box, so the UV mapping is the write box's position
			 * within it -- in the transient's own pixels, normalised by the
			 * ALLOCATION, which is larger again where the pool rounded up.
			 * Three extents, and using the wrong one shifts the blur.
			 */
			float aw = (float)result->extent.width;
			float ah = (float)result->extent.height;
			float ox = (float)(cmd->dst.x - br->capture.x);
			float oy = (float)(cmd->dst.y - br->capture.y);
			pc.uv_org_dx[0] = ox / aw;
			pc.uv_org_dx[1] = oy / ah;
			pc.uv_org_dx[2] = (float)cmd->dst.width / aw;
			pc.uv_org_dx[3] = 0.0f;
			pc.uv_dy[0] = 0.0f;
			pc.uv_dy[1] = (float)cmd->dst.height / ah;
			/* Hard edge: 1.0 keeps the sampled alpha, exactly as a texture
			 * command's alpha-mask flag does. Soft edge: the same slot is the
			 * falloff's sigma, in output pixels. Two pipelines, one field, and
			 * they never draw the same command. */
			float edge = cmd->blur_edge_softness;
			if (renderer->break_blur_edge_logical_sigma && edge > 0.0f) {
				/* Back to LOGICAL units, which is what the reference passes a
				 * shadow. The blur node keeps its scaled edge in the reference
				 * and the shadow does not, so undoing the scale HERE is what
				 * makes the two disagree by exactly the factor the reference
				 * disagrees by. */
				edge /= renderer->break_blur_edge_scale;
			}
			pc.params[1] = edge > 0.0f ? edge : 1.0f;

			VkDescriptorSet set = avk_pipelines_texture_set(&renderer->pipes,
				result, false);
			if (set == VK_NULL_HANDLE) {
				pixman_region32_fini(&region);
				continue;
			}
			vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
				renderer->pipes.layout, 0, 1, &set, 0, NULL);
			renderer->stats.surfaces++;
			renderer->stats.blur_draws++;
		} else if (cmd->type == AVK_CMD_TEXTURE) {
			if (cmd->image == NULL) {
				pixman_region32_fini(&region);
				continue;
			}
			want = renderer->pipes.texture;

			float origin[2], dx[2], dy[2];
			transform_uv(&cmd->src, cmd->image->extent.width,
				cmd->image->extent.height, cmd->transform, origin, dx, dy);
			pc.uv_org_dx[0] = origin[0];
			pc.uv_org_dx[1] = origin[1];
			pc.uv_org_dx[2] = dx[0];
			pc.uv_org_dx[3] = dx[1];
			pc.uv_dy[0] = dy[0];
			pc.uv_dy[1] = dy[1];
			/* 1.0 keeps the sampled alpha, 0.0 forces opaque -- the DRM X
			 * formats have a fourth channel that means nothing. */
			pc.params[1] = cmd->image->has_alpha ? 1.0f : 0.0f;

			VkDescriptorSet set = avk_pipelines_texture_set(&renderer->pipes,
				cmd->image, cmd->filter_linear);
			if (set == VK_NULL_HANDLE) {
				pixman_region32_fini(&region);
				continue;
			}

			vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
				renderer->pipes.layout, 0, 1, &set, 0, NULL);
			renderer->stats.surfaces++;
		} else {
			want = renderer->pipes.rect;
			for (int c = 0; c < 3; c++) {
				pc.color[c] = cmd->color[c] * cmd->color[3];
			}
			pc.color[3] = cmd->color[3];
			renderer->stats.rects++;

			/*
			 * GRADIENT IS A MATERIAL, NOT A GEOMETRY.
			 *
			 * Everything above -- the destination, the outer arcs, the inner
			 * cut-out that makes a border an annulus -- has already been filled
			 * in and is untouched by this. All that changes is which pipeline
			 * shades the same quad, and which colour it uses. That is what lets
			 * a gradient BORDER be M4B's annulus with a different fill instead
			 * of a second border path, and it is why the reference's gradient
			 * rects and gradient borders are the same draw call there too.
			 *
			 * `color` stays exactly as computed: the gradient replaces the rgb,
			 * and the alpha still gates the shape, which is what
			 * quad_grad_round.frag does with v_color.a.
			 */
			const struct avk_gradient *g = &cmd->gradient;
			if (g->type != AVK_GRADIENT_NONE && g->color_count > 0 &&
					gradient_set != VK_NULL_HANDLE) {
				uint32_t rec = avk_gradient_store_push(&renderer->gradients, g,
					scene->gradient_colors + (size_t)g->color_offset * 4);
				if (rec != UINT32_MAX) {
					want = renderer->pipes.gradient;
					pc.params[1] = (float)rec;
				}
			}
		}

		if (bound != want) {
			vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, want);
			bound = want;
		}
		vkCmdPushConstants(cb, renderer->pipes.layout,
			VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
			sizeof(pc), &pc);

		for (int r = 0; r < count; r++) {
			/* SCENE -> ATTACHMENT, and the only such translation in the draw
			 * loop. Everything above is in scene coordinates. */
			VkRect2D scissor = {
				{ rects[r].x1 - ctx->origin_x, rects[r].y1 - ctx->origin_y },
				{ (uint32_t)(rects[r].x2 - rects[r].x1),
					(uint32_t)(rects[r].y2 - rects[r].y1) },
			};
			vkCmdSetScissor(cb, 0, 1, &scissor);
			vkCmdDraw(cb, 4, 1, 0, 0);
			renderer->stats.draws++;
		}
		pixman_region32_fini(&region);
	}

	vkCmdEndRendering(cb);
}


/* Append, or complain. Truncating silently would reintroduce exactly the class
 * of bug this whole path exists to remove. */
static void az_add_sampled(struct avk_cmd_uses *out, struct avk_image *image) {
	if (image == NULL) {
		return;
	}
	if (out->sampled_len >= AVK_CMD_MAX_SAMPLED) {
		avk_log(AVK_ERROR, "avk: a command samples more than %d images; "
			"AVK_CMD_MAX_SAMPLED needs raising", AVK_CMD_MAX_SAMPLED);
		return;
	}
	out->sampled[out->sampled_len++] = image;
}

bool avk_cmd_graph_uses(const struct avk_scene *scene, size_t index,
		const struct avk_cmd_use_ctx *ctx, struct avk_cmd_uses *out) {
	if (out == NULL || scene == NULL || index >= scene->len) {
		return false;
	}
	memset(out, 0, sizeof(*out));
	const struct avk_cmd *cmd = &scene->cmds[index];

	/*
	 * THE ONE SWITCH. No `default:`, deliberately -- see avk_render.h. `declared`
	 * is the runtime half of the same guard: a case that forgets to set it is a
	 * false return and a log line, never a silent "samples nothing".
	 */
	bool declared = false;
	/* Guard 1 of 3: adding a command type changes AVK_CMD_TYPE_COUNT and stops
	 * the build HERE, at the switch that has to learn about it, rather than at a
	 * missing barrier six months later. (-Wall's -Wswitch also fires on the
	 * default-less switch below; the assert is the one that is not a warning.) */
	_Static_assert(AVK_CMD_TYPE_COUNT == 4,
		"a new avk_cmd_type needs a case in avk_cmd_graph_uses(): state its "
		"sampled images, or state explicitly that it has none");
	switch (cmd->type) {
	case AVK_CMD_RECT:
		/* NOTHING, and it is a decision rather than an omission: a rect's whole
		 * input is its colour or its gradient, and a gradient's colours live in
		 * a storage buffer the renderer writes once per frame and binds for the
		 * whole segment -- not a per-command resource. */
		declared = true;
		break;
	case AVK_CMD_SHADOW:
		/* NOTHING. M4D's shadow is analytic: a signed-distance field evaluated
		 * in the fragment shader, with no blurred texture to sample. That is the
		 * entire reason it costs one draw. */
		declared = true;
		break;
	case AVK_CMD_TEXTURE:
		az_add_sampled(out, cmd->image);
		declared = true;
		break;
	case AVK_CMD_BLUR:
		/*
		 * Its own finished result, produced earlier in this frame by the chain
		 * avk_render_frame() declared for it. NULL where no chain ran -- zero
		 * levels, a region too small, a transient that could not be had -- and
		 * the draw loop skips the command for the same reason.
		 */
		if (ctx != NULL && ctx->blur_results != NULL
				&& index < ctx->blur_results_len) {
			az_add_sampled(out, ctx->blur_results[index].image);
		}
		declared = true;
		break;
	}

	if (!declared) {
		avk_log(AVK_ERROR, "avk: command type %d has no declared graph-use "
			"behaviour; a missing use is a missing barrier, so this frame is "
			"refused rather than rendered", (int)cmd->type);
		return false;
	}
	return true;
}

/*
 * A segment, as a graph pass.
 *
 * The uses are derived from the command range itself rather than passed in, so
 * a caller cannot declare a range and forget one of the textures it samples --
 * which would be a missing barrier that renders correctly on this driver.
 */
bool avk_render_declare_segment(struct avk_graph *graph,
		struct avk_render_segment *seg, uint32_t target_resource) {
	if (graph == NULL || seg == NULL || target_resource == AVK_GRAPH_INVALID) {
		return false;
	}
	if (!avk_graph_pass_begin(graph, "compose_scene", az_record_compose, seg)) {
		return false;
	}
	avk_graph_use(graph, target_resource, AVK_USE_COLOR_WRITE, NULL);

	const struct avk_scene *scene = seg->scene;
	struct avk_renderer *renderer = seg->renderer;
	struct avk_cmd_use_ctx use_ctx = {
		.blur_results = renderer != NULL ? renderer->blur_results : NULL,
		.blur_results_len = renderer != NULL ? renderer->blur_results_cap : 0,
	};
	size_t end = seg->end > scene->len ? scene->len : seg->end;
	for (size_t i = seg->begin; i < end; i++) {
		/* ASKED, not decided here. There is one authoritative answer to "what
		 * does this command sample" and this is a consumer of it, not a second
		 * copy -- see avk_cmd_graph_uses(). */
		struct avk_cmd_uses uses;
		if (!avk_cmd_graph_uses(scene, i, &use_ctx, &uses)) {
			avk_graph_pass_end(graph);
			return false;
		}
		for (uint8_t u = 0; u < uses.sampled_len; u++) {
			struct avk_image *sampled = uses.sampled[u];
			bool foreign = az_foreign(sampled);
			uint32_t r = avk_graph_add_image(graph, sampled, foreign,
				foreign ? AVK_EXIT_FOREIGN : AVK_EXIT_KEEP);
			if (r != AVK_GRAPH_INVALID) {
				avk_graph_use(graph, r, AVK_USE_SAMPLED_READ, NULL);
			}
		}
	}
	avk_graph_pass_end(graph);
	return true;
}

uint64_t avk_render_frame(struct avk_renderer *renderer,
		struct avk_image *target, const struct avk_scene *scene,
		const VkSemaphoreSubmitInfo *wait, uint32_t wait_count,
		const VkSemaphoreSubmitInfo *signal, uint32_t signal_count) {
	struct avk_device *dev = renderer->dev;
	const uint32_t width = target->extent.width;
	const uint32_t height = target->extent.height;

	struct timespec start;
	clock_gettime(CLOCK_MONOTONIC, &start);

	/*
	 * ── THE RING WAIT IS NOT RECORDING TIME ───────────────────────────────
	 *
	 * avk_cmd_ring_begin() BLOCKS when the slot it is about to hand out is
	 * still in flight: three frames deep, so it only happens when the CPU has
	 * run ahead of the GPU. That wait was inside record_ns, and it is not CPU
	 * work -- it is the CPU standing still. It is what produced record samples
	 * past the 40.96 ms histogram ceiling on a fixture doing almost nothing,
	 * and widening the histogram would have preserved the lie in more buckets.
	 *
	 * Timed separately and subtracted. `cpu_sync_waits` already counted these
	 * stalls; now their DURATION is reported too, because a counter that says
	 * "it happened" cannot say "it cost 40 ms".
	 */
	struct timespec ring0;
	clock_gettime(CLOCK_MONOTONIC, &ring0);
	VkCommandBuffer cb = avk_cmd_ring_begin(&renderer->ring);
	struct timespec ring1;
	clock_gettime(CLOCK_MONOTONIC, &ring1);
	uint64_t ring_wait_ns = (uint64_t)(ring1.tv_sec - ring0.tv_sec)
		* 1000000000ULL + (uint64_t)(ring1.tv_nsec - ring0.tv_nsec);
	if (cb == VK_NULL_HANDLE) {
		return 0;
	}
	avk_debug_label_begin(dev, cb, "avk frame %" PRIu64,
		renderer->stats.frames);

	/*
	 * The slot, captured now: ring.recording goes back to -1 at submit, and
	 * the timestamp bookkeeping needs it afterwards. Query reset must happen
	 * outside a render pass, which is why it is here and not beside the first
	 * mark.
	 */
	uint32_t ts_slot = (uint32_t)renderer->ring.recording;
	avk_timestamps_begin(&renderer->timestamps, cb, ts_slot);
	avk_timestamps_mark(&renderer->timestamps, cb, ts_slot,
		AVK_TS_FRAME_BEGIN, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT);

	/*
	 * The frame's gradient data, sized BEFORE anything is recorded.
	 *
	 * Sizing up front is what makes the buffer safe to grow: once a draw has
	 * been recorded referring to an index in it, the buffer behind that index
	 * may not be replaced. So the demand is counted in one pass over the
	 * commands, the slot is grown at most once, and the packing that follows
	 * can only ever fit.
	 *
	 * The slot is the COMMAND RING's slot, deliberately. avk_cmd_ring_begin()
	 * above has already waited for that slot's previous submission to complete,
	 * which is precisely the condition for overwriting the buffer and for
	 * updating its descriptor -- so gradient data needs no synchronisation of
	 * its own, and adds no CPU wait.
	 */
	uint32_t gradient_vec4s = 0;
	for (size_t i = 0; i < scene->len; i++) {
		const struct avk_gradient *g = &scene->cmds[i].gradient;
		if (g->type != AVK_GRADIENT_NONE && g->color_count > 0) {
			gradient_vec4s += 2 + g->color_count;
		}
	}
	VkDescriptorSet gradient_set = VK_NULL_HANDLE;
	if (gradient_vec4s > 0) {
		gradient_set = avk_gradient_store_begin(&renderer->gradients,
			(uint32_t)renderer->ring.recording, gradient_vec4s);
		if (gradient_set == VK_NULL_HANDLE) {
			/* Loud, and then draw the frame without them. A gradient rect
			 * whose record could not be written would sample whatever the
			 * previous frame left in the buffer, which is worse than a solid
			 * colour and much harder to recognise. */
			avk_log(AVK_ERROR, "avk: no room for %u vec4s of gradient data; "
				"this frame's gradients are drawn as solid colour",
				gradient_vec4s);
		}
	}

	/*
	 * WHAT THE FRAME TOUCHES, DECLARED.
	 *
	 * The target is written as a colour attachment; every distinct sampled image
	 * is read. That is the entire dependency structure of a direct composition,
	 * and stating it is all this file does -- the barriers, the layouts and the
	 * foreign acquire/release that follow from it are the graph's to derive.
	 *
	 * avk_graph_add_image() interns by VkImage, so two commands sampling one
	 * surface produce one resource and one barrier. That used to be an explicit
	 * duplicate check in the batch builder; it is now a property of the model.
	 */
	avk_graph_reset(&renderer->graph);
	avk_blur_frame_reset();
	struct avk_graph *graph = &renderer->graph;

	bool target_foreign = az_foreign(target);
	uint32_t r_target = avk_graph_add_image(graph, target, target_foreign,
		target_foreign ? AVK_EXIT_FOREIGN : AVK_EXIT_KEEP);
	if (r_target == AVK_GRAPH_INVALID) {
		avk_cmd_ring_abandon(&renderer->ring);
		return 0;
	}
	/* GENERAL for a scan-out buffer, COLOR_ATTACHMENT_OPTIMAL for one of our
	 * own -- the graph's rule, asked for here because the rendering instance
	 * has to name the layout its attachment is in. */
	VkImageLayout target_layout = target_foreign
		? VK_IMAGE_LAYOUT_GENERAL
		: VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	if (target->view == VK_NULL_HANDLE) {
		VkImageViewCreateInfo view_info = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			.image = target->image,
			.viewType = VK_IMAGE_VIEW_TYPE_2D,
			.format = target->format,
			.subresourceRange = {
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.levelCount = 1,
				.layerCount = 1,
			},
		};
		if (!avk_check(vkCreateImageView(dev->dev, &view_info, NULL,
				&target->view), "vkCreateImageView (target)")) {
			avk_cmd_ring_abandon(&renderer->ring);
			return 0;
		}
		AVK_LIVE_INC(dev, image_views);
	}

	/*
	 * ── LIVE BLUR: every chain, declared before anything draws ────────────
	 *
	 * For each AVK_CMD_BLUR at index k, in INCREASING scene order:
	 *
	 *     acquire a transient covering the aligned capture region
	 *     declare a segment [0, k) into it   <- the current-frame scene prefix
	 *     declare the dual-Kawase chain on it
	 *
	 * Increasing order is what makes an earlier blur available to a later one:
	 * blur 2's replay of [0, k2) contains blur 1's command, and by then blur 1's
	 * result exists, so it composites exactly as it will in the output. No
	 * recursion, and blur is never special-cased out of prefix replay.
	 *
	 * NO PIXEL OF THIS COMES FROM THE OUTPUT TARGET. The target holds the
	 * current frame's prefix only inside this frame's damage and the PREVIOUS
	 * frame's final composite everywhere else, so borrowing from it would make
	 * a blur's source depend on what happened to be on screen last frame. The
	 * transient is written fresh, over its whole capture region, before
	 * anything reads it.
	 */
	size_t blur_count = 0;
	for (size_t i = 0; i < scene->len; i++) {
		if (scene->cmds[i].type == AVK_CMD_BLUR) {
			blur_count++;
		}
	}
	struct avk_render_segment *prefix_segs = NULL;
	struct az_blur_slot *slots = NULL;
	/*
	 * The damage state the two sweeps carry. Initialised even with no blur in
	 * the frame -- both sweeps then run over zero slots and `frame_damage` goes
	 * back to `scene->damage` unchanged, which is the direct path costing three
	 * region operations rather than a branch nobody tests.
	 */
	pixman_region32_t prefix_damage, frame_damage, blur_generated;
	pixman_region32_init(&prefix_damage);
	pixman_region32_init(&frame_damage);
	/* Damage this frame's blurs produced, as opposed to damage that arrived
	 * with it. Only used to count the transitive edge. */
	pixman_region32_init(&blur_generated);
	pixman_region32_copy(&prefix_damage, &scene->damage);
	pixman_region32_copy(&frame_damage, &scene->damage);
	if (blur_count > 0) {
		slots = calloc(blur_count, sizeof(*slots));
		if (slots == NULL) {
			pixman_region32_fini(&prefix_damage);
			pixman_region32_fini(&frame_damage);
   pixman_region32_fini(&blur_generated);
			avk_cmd_ring_abandon(&renderer->ring);
			return 0;
		}
		if (renderer->blur_results_cap < scene->len) {
			void *grown = realloc(renderer->blur_results,
				scene->len * sizeof(*renderer->blur_results));
			if (grown == NULL) {
				free(slots);
	pixman_region32_fini(&frame_damage);
				pixman_region32_fini(&prefix_damage);
				pixman_region32_fini(&frame_damage);
    pixman_region32_fini(&blur_generated);
				avk_cmd_ring_abandon(&renderer->ring);
				return 0;
			}
			renderer->blur_results = grown;
			renderer->blur_results_cap = scene->len;
		}
		memset(renderer->blur_results, 0,
			renderer->blur_results_cap * sizeof(*renderer->blur_results));
		/* One segment per blur, alive until avk_graph_execute() has recorded
		 * them -- the graph holds the pointer. */
		prefix_segs = calloc(blur_count, sizeof(*prefix_segs));
		if (prefix_segs == NULL) {
			free(slots);
			pixman_region32_fini(&prefix_damage);
			pixman_region32_fini(&frame_damage);
   pixman_region32_fini(&blur_generated);
			avk_cmd_ring_abandon(&renderer->ring);
			return 0;
		}
	}

	/*
	 * WHERE PIXELS MAY BE PRESENTED, and -- separately -- where source may be
	 * RECONSTRUCTED. See struct avk_scene.source_bounds. They differ only on a
	 * multi-output desktop, and only for blur.
	 */
	const struct avk_box present_bounds = {
		0, 0, (int32_t)width, (int32_t)height,
	};
	struct avk_box scene_bounds = present_bounds;
	if (scene->source_bounds.width > 0 && scene->source_bounds.height > 0
			&& !renderer->break_blur_source_output_clip) {
		scene_bounds = scene->source_bounds;
	}
	renderer->blur_halo_pixels +=
		(uint64_t)scene_bounds.width * (uint64_t)scene_bounds.height
		- (uint64_t)present_bounds.width * (uint64_t)present_bounds.height;

	/*
	 * ── THE TWO DAMAGE SWEEPS ─────────────────────────────────────────────
	 *
	 * Damage forward, demand backward; see the long comment on
	 * struct avk_blur_damage in avk_render.h for why they are two sweeps and
	 * why neither of them iterates.
	 */
	struct timespec damage_start;
	clock_gettime(CLOCK_MONOTONIC, &damage_start);

	size_t slot_len = 0;
	/* How many of those slots produced actual blur passes. See the cohort
	 * comment at avk_timestamps_blur_active() below: slots are candidates,
	 * chains are GPU work, and only chains write timestamp marks. */
	size_t declared_chains = 0;
	for (size_t i = 0; i < scene->len && blur_count > 0; i++) {
		const struct avk_cmd *cmd = &scene->cmds[i];
		if (cmd->type != AVK_CMD_BLUR || cmd->blur_levels == 0) {
			continue;
		}
		struct az_blur_slot *slot = &slots[slot_len];
		slot->index = i;
		slot->params = (struct avk_blur_params){
			.levels = cmd->blur_levels,
			.radius = cmd->blur_radius,
			.brightness = cmd->blur_brightness,
			.contrast = cmd->blur_contrast,
			.saturation = cmd->blur_saturation,
			.noise = cmd->blur_noise,
			.apply_effects = cmd->blur_apply_effects,
			.darken = cmd->blur_darken
				&& !renderer->break_blur_ignore_darken,
		};
		struct avk_blur_damage *d = &slot->damage;
		if (!avk_blur_regions_of(&d->regions, &cmd->dst, &slot->params,
				&scene_bounds)) {
			continue;
		}
		az_blur_damage_init(d);
		slot_len++;

		/*
		 * WRITE: the node's box intersected with its own clip -- the region,
		 * not its bounding box. This is the same geometry the composite draws,
		 * computed the same way, because a damage region that disagreed with
		 * the draw would be right about a rectangle the draw never touches.
		 */
		pixman_region32_init_rect(&d->write, d->regions.write.x,
			d->regions.write.y, (unsigned)d->regions.write.width,
			(unsigned)d->regions.write.height);
		if (cmd->has_clip && !renderer->break_blur_ignore_clip) {
			pixman_region32_intersect(&d->write, &d->write,
				(pixman_region32_t *)&cmd->clip);
		}

		/*
		 * DEPENDENCY: write dilated by the REVERSE support -- what this blur
		 * could read if everything were recomputed. Bounded by the scene, since
		 * a source pixel off the output does not exist.
		 */
		struct avk_blur_support rev = avk_blur_support_of(&slot->params,
			(uint32_t)d->regions.write.width,
			(uint32_t)d->regions.write.height);
		az_region_expand(&d->dependency, &d->write, &rev);
		pixman_region32_intersect_rect(&d->dependency, &d->dependency,
			scene_bounds.x, scene_bounds.y, (unsigned)scene_bounds.width,
			(unsigned)scene_bounds.height);

		/*
		 * SOURCE DAMAGE: the prefix damage so far, landing inside the
		 * dependency.
		 *
		 * `prefix_damage` starts as the frame's damage, which over-approximates
		 * in one direction worth naming: it includes changes caused by commands
		 * drawn AFTER this blur, which cannot be in its source. Conservative,
		 * cheap, and a frame in which that matters is one where the window
		 * above the blur moved -- so the blur is being recomposited anyway.
		 */
		pixman_region32_intersect(&d->source_damage, &prefix_damage,
			&d->dependency);
		/* How much of it came from an EARLIER BLUR rather than from the frame's
		 * own input damage. The forward sweep's one edge, and the only way it
		 * can be falsified -- see blur_transitive_damage_pixels. */
		if (pixman_region32_not_empty(&blur_generated)) {
			pixman_region32_t inherited;
			pixman_region32_init(&inherited);
			pixman_region32_intersect(&inherited, &d->source_damage,
				&blur_generated);
			renderer->blur_transitive_damage_pixels +=
				az_region_area(&inherited);
			pixman_region32_fini(&inherited);
		}

		/*
		 * OUTPUT DAMAGE: source damage dilated by the FORWARD support, clipped
		 * to what this blur actually writes.
		 *
		 * This also covers a demand that is easy to miss: every pixel inside the
		 * frame's damage is rebuilt from the clear upward, so a blur whose write
		 * region is damaged must produce pixels there even if its own source did
		 * not move. It needs no separate term -- write is a subset of dependency,
		 * so `frame_damage ∩ write` is already inside source_damage, and
		 * dilation only grows it.
		 */
		if (renderer->break_blur_under_damage) {
			/* The break: skip the forward dilation entirely. The blur is still
			 * recomputed, the counters still move, and a ring of stale result
			 * one support wide is left around every change. */
			pixman_region32_copy(&d->output_damage, &d->source_damage);
			pixman_region32_intersect(&d->output_damage, &d->output_damage,
				&d->write);
		} else {
			struct avk_blur_support fwd = avk_blur_forward_support_of(
				&slot->params, (uint32_t)d->regions.write.width,
				(uint32_t)d->regions.write.height);
			az_region_expand(&d->output_damage, &d->source_damage, &fwd);
			pixman_region32_intersect(&d->output_damage, &d->output_damage,
				&d->write);
		}
		if (renderer->blur_full_damage) {
			/* The oracle: everything this blur writes, every frame. */
			pixman_region32_copy(&d->output_damage, &d->write);
		}

		/*
		 * FORWARD, and only now. The blur's own output joins the prefix damage
		 * AFTER its own source damage has been read, which is the line that
		 * makes a blur feeding itself structurally impossible rather than
		 * merely avoided.
		 */
		pixman_region32_union(&prefix_damage, &prefix_damage,
			&d->output_damage);
		pixman_region32_union(&frame_damage, &frame_damage, &d->output_damage);
		pixman_region32_union(&blur_generated, &blur_generated,
			&d->output_damage);
	}

	/*
	 * THE FRAME'S DAMAGE, GROWN BY EVERY BLUR THAT CHANGED.
	 *
	 * Published on the renderer rather than written back through `scene`, which
	 * is const here and belongs to the caller. Valid until the next
	 * avk_render_frame() on this renderer, which is exactly long enough: the
	 * compositor reads it immediately afterwards to tell the backend what
	 * changed, and telling the backend LESS than was redrawn is how a blurred
	 * fringe survives on screen for as long as nothing else damages it.
	 */
	/*
	 * CLIPPED TO THE OUTPUT, because this is the PRESENTATION damage: what the
	 * backend is told and what the output segment is allowed to draw. The
	 * damage that arrived may reach outside the output -- a change on the
	 * monitor next door, inside this output's blur halo, is carried here in
	 * this output's own pixel coordinates and is therefore negative or past the
	 * far edge. It is real source damage and the sweeps above have already used
	 * it; it is not a pixel this output can present.
	 */
	pixman_region32_intersect_rect(&frame_damage, &frame_damage,
		present_bounds.x, present_bounds.y, (unsigned)present_bounds.width,
		(unsigned)present_bounds.height);
	pixman_region32_copy(&renderer->frame_damage, &frame_damage);

	/*
	 * ── SWEEP 2: DEMAND, BACKWARD ─────────────────────────────────────────
	 *
	 * `demand` is what somebody still needs composited. It starts as the output
	 * frame's damage and grows as each blur's prefix asks earlier commands for
	 * a wider region than the output ever wanted.
	 */
	pixman_region32_t demand;
	pixman_region32_init(&demand);
	pixman_region32_copy(&demand, &frame_damage);
	for (size_t s = slot_len; s-- > 0; ) {
		struct az_blur_slot *slot = &slots[s];
		struct avk_blur_damage *d = &slot->damage;

		pixman_region32_intersect(&d->result_region, &demand, &d->write);
		if (!pixman_region32_not_empty(&d->result_region)) {
			renderer->blur_damage_nodes_skipped++;
			continue;
		}
		d->active = true;

		struct avk_blur_support rev = avk_blur_support_of(&slot->params,
			(uint32_t)d->regions.write.width,
			(uint32_t)d->regions.write.height);
		az_region_expand(&d->prefix_rebuild, &d->result_region, &rev);
		pixman_region32_intersect_rect(&d->prefix_rebuild, &d->prefix_rebuild,
			d->regions.capture.x, d->regions.capture.y,
			(unsigned)d->regions.capture.width,
			(unsigned)d->regions.capture.height);

		/*
		 * THE FALLBACK, explicit and counted. A region that has fragmented past
		 * this collapses to its own extents -- still inside the capture, never
		 * outside it, so the result stays correct and only the work grows.
		 * Silently collapsing every complex region is how a damage system
		 * becomes a full redraw nobody notices.
		 */
		int rects = 0;
		pixman_region32_rectangles(&d->prefix_rebuild, &rects);
		if ((uint64_t)rects > renderer->blur_damage_rects_max) {
			renderer->blur_damage_rects_max = (uint64_t)rects;
		}
		if (rects > AVK_BLUR_DAMAGE_MAX_RECTS) {
			pixman_box32_t ext = *pixman_region32_extents(&d->prefix_rebuild);
			pixman_region32_fini(&d->prefix_rebuild);
			pixman_region32_init_rect(&d->prefix_rebuild, ext.x1, ext.y1,
				(unsigned)(ext.x2 - ext.x1), (unsigned)(ext.y2 - ext.y1));
			renderer->blur_damage_fallbacks++;
		}

		pixman_region32_union(&demand, &demand, &d->prefix_rebuild);
	}
	pixman_region32_fini(&demand);
	pixman_region32_fini(&prefix_damage);
	pixman_region32_fini(&blur_generated);
	/* frame_damage outlives this: the output segment points AT it, and a
	 * segment's active region has to survive until avk_graph_execute() has
	 * recorded the pass that reads it. */

	struct timespec damage_end;
	clock_gettime(CLOCK_MONOTONIC, &damage_end);
	renderer->blur_damage_build_ns +=
		(uint64_t)(damage_end.tv_sec - damage_start.tv_sec) * 1000000000ULL
		+ (uint64_t)(damage_end.tv_nsec - damage_start.tv_nsec);

	size_t seg_at = 0;
	for (size_t s = 0; s < slot_len; s++) {
		struct az_blur_slot *slot = &slots[s];
		struct avk_blur_damage *d = &slot->damage;
		const size_t i = slot->index;
		const struct avk_blur_regions rg = d->regions;
		struct avk_blur_params params = slot->params;

		/* Areas, whether or not this blur runs: a skipped blur's saving is only
		 * meaningful against what it would otherwise have cost. */
		renderer->blur_full_write_pixels +=
			(uint64_t)rg.write.width * (uint64_t)rg.write.height;
		renderer->blur_full_dependency_pixels +=
			(uint64_t)rg.dependency.width * (uint64_t)rg.dependency.height;
		renderer->blur_full_capture_pixels +=
			(uint64_t)rg.capture.width * (uint64_t)rg.capture.height;
		renderer->blur_source_damage_pixels +=
			az_region_area(&d->source_damage);
		renderer->blur_output_damage_pixels +=
			az_region_area(&d->output_damage);
		uint64_t rebuild_px = az_region_area(&d->prefix_rebuild);
		renderer->blur_prefix_rebuild_pixels += rebuild_px;
		uint64_t full_px = (uint64_t)rg.capture.width
			* (uint64_t)rg.capture.height;
		renderer->blur_damage_saved_pixels +=
			full_px > rebuild_px ? full_px - rebuild_px : 0;

		/*
		 * THE SIX REGIONS, PER BLUR, ON DEMAND.
		 *
		 * AZ_BLUR_DUMP_REGIONS=1 logs every blur's geometry for one frame:
		 * where it writes, what it must produce, and what source it rebuilds to
		 * produce it. A stale strip on screen can then be matched against the
		 * blur that owns those pixels and asked whether its result region ever
		 * covered them -- which rectangle-free logging cannot answer and which
		 * is the difference between a damage bug and a source bug.
		 */
		if (getenv("AZ_BLUR_DUMP_REGIONS") != NULL) {
			pixman_box32_t we = *pixman_region32_extents(&d->write);
			pixman_box32_t re = *pixman_region32_extents(&d->result_region);
			pixman_box32_t pe = *pixman_region32_extents(&d->prefix_rebuild);
			/* The TARGET's extent identifies which output's frame this is.
			 * Without it a two-output dump is a list of rectangles with no way
			 * to tell whose buffer they are in, and the negative write regions
			 * of the monitor next door look like a defect. */
			avk_log(AVK_ERROR, "blur[%zu] tgt=%ux%u write=%d,%d..%d,%d "
				"result=%d,%d..%d,%d rebuild=%d,%d..%d,%d "
				"cap=%d,%d %dx%d active=%d", i, width, height,
				we.x1, we.y1, we.x2, we.y2, re.x1, re.y1, re.x2, re.y2,
				pe.x1, pe.y1, pe.x2, pe.y2, rg.capture.x, rg.capture.y,
				rg.capture.width, rg.capture.height, (int)d->active);
		}
		if (!d->active) {
			continue;
		}
		renderer->blur_damage_nodes_touched++;
		renderer->blur_capture_pixels +=
			(uint64_t)rg.capture.width * (uint64_t)rg.capture.height;
		renderer->blur_result_pixels += az_region_area(&d->result_region);
		renderer->blur_processed_pixels = renderer->blur_stats.processed_pixels;
		if (params.darken) {
			renderer->stats.blur_darken_passes++;
		}

		/* TRANSFER_SRC only under the oracle: the pool keys on usage, so
		 * adding it unconditionally would give every normal run a differently
		 * keyed pool from the one the milestone measured. */
		VkImageUsageFlags prefix_usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
			| VK_IMAGE_USAGE_SAMPLED_BIT;
		if (renderer->oracle.enabled) {
			prefix_usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
		}
		struct avk_image *prefix = avk_transient_acquire(&renderer->transients,
			renderer->format, (uint32_t)rg.capture.width,
			(uint32_t)rg.capture.height, prefix_usage);
		if (prefix == NULL) {
			continue;
		}
		uint32_t r_prefix = avk_graph_add_image(graph, prefix, false,
			AVK_EXIT_KEEP);
		if (r_prefix == AVK_GRAPH_INVALID) {
			continue;
		}

		struct avk_render_segment *seg = &prefix_segs[seg_at++];
		*seg = (struct avk_render_segment){
			.renderer = renderer,
			.scene = scene,
			.target = prefix,
			.width = (uint32_t)rg.capture.width,
			.height = (uint32_t)rg.capture.height,
			.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			.origin_x = rg.capture.x,
			.origin_y = rg.capture.y,
			.begin = 0,
			/* [0, k) -- the blur's own command is NOT in its own source.
			 * The break makes it [0, len), which is the reference's defect. */
			.end = renderer->break_blur_scene_after ? scene->len : i,
			/*
			 * ONLY THE PIXELS THE CHAIN WILL READ. The transient's EXTENT is
			 * still the whole capture and deliberately so: a dual-Kawase level
			 * grid is derived from the image's extent, so a smaller capture
			 * would sample at different positions and produce a different --
			 * not wrong, but different -- blur. Pixel-identity with a forced
			 * full render is the milestone's oracle, so the grid is held fixed
			 * and the REGION is what shrinks.
			 *
			 * Everything outside prefix_rebuild is left undefined (loadOp
			 * DONT_CARE, and under AZ_TRANSIENT_POISON it is a colour nothing
			 * produces). Nothing samples it: prefix_rebuild is result_region
			 * dilated by the full chain support, so every tap taken for a pixel
			 * of result_region lands inside it.
			 */
			.active = &d->prefix_rebuild,
			.clear = true,
			.load = false,
			.gradient_set = gradient_set,
		};
		if (!avk_render_declare_segment(graph, seg, r_prefix)) {
			continue;
		}
		/*
		 * BLUR_BEGIN goes on the FIRST prefix replay, which is the first blur
		 * work in the frame: every chain is declared before the output
		 * segment, so BLUR_BEGIN..BLUR_END is one contiguous range of the
		 * command stream containing all the blur work and nothing else.
		 * PREFIX_END closes the first chain's replay phase.
		 */
		if (s == 0) {
			avk_graph_pass_time(graph, AVK_TS_BLUR_BEGIN,
				AVK_TS_BLUR_PREFIX_END);
		}
		renderer->blur_prefix_replays++;
		/*
		 * WHAT WAS REPLAYED, not what was meant to be.
		 *
		 * This used to add `i` -- the blur's own command index, which is the
		 * prefix length the design calls for. It is the same number on the
		 * shipped path and a DIFFERENT one under AZ_BLUR_SCENE_AFTER, where the
		 * range is [0, len). So the break widened the replay and the counter
		 * went on reporting the narrow figure, and a test asserting "the source
		 * range is a prefix" passed with the break on.
		 *
		 * Read from the segment, so the counter cannot describe a range the
		 * renderer did not use.
		 */
		renderer->blur_prefix_commands += seg->end - seg->begin;
		/*
		 * AND THE SAME RULE FOR THE AREA. This was the capture's full extent,
		 * which stopped being what the replay writes the moment the segment
		 * acquired an active region -- it would have gone on reporting M4F.2A's
		 * figure while the renderer did a fraction of the work, and the saving
		 * this milestone exists to produce would have been invisible in the one
		 * counter named after it. Read from the region the segment was given.
		 */
		renderer->blur_prefix_pixels += az_region_area(seg->active);

		/*
		 * BOUNDARY 1: the reconstructed scene prefix, before anything blurs it.
		 * The tap is a graph pass, so the barrier from colour-write to
		 * transfer-read and back to sampled-read is the graph's to emit and the
		 * chain that follows is unaffected.
		 */
		const struct avk_box tap_box = {
			0, 0, rg.capture.width, rg.capture.height,
		};
		if (renderer->oracle.enabled) {
			/*
			 * The mask is what production CLAIMS: prefix_rebuild for the source
			 * and result_region for the blur, both translated out of output
			 * pixels into the transient's own. Outside them the transient is
			 * loadOp DONT_CARE and nothing samples it -- comparing there would
			 * report undefined memory as a defect.
			 */
			pixman_region32_t m;
			pixman_region32_init(&m);
			pixman_region32_copy(&m, &d->prefix_rebuild);
			pixman_region32_translate(&m, -rg.capture.x, -rg.capture.y);
			avk_oracle_tap(&renderer->oracle, graph, r_prefix, prefix, tap_box,
				AVK_TAP_PREFIX, i, &m);
			pixman_region32_fini(&m);
		}

		/* The chain writes its final upsample back into the prefix transient:
		 * nothing reads the unblurred prefix after the first downsample, so a
		 * second full-size image would be allocated only to be thrown away. */
		/*
		 * PHASE MARKS FOR THE FIRST CHAIN ONLY. With N chains the command
		 * stream is prefix0, chain0, prefix1, chain1..., so "all the
		 * downsamples" is not a contiguous range and a pair around it would
		 * measure everything in between. The first chain is contiguous by
		 * construction and is the one that gets them.
		 */
		const struct avk_blur_marks chain_marks = {
			.down_end = (s == 0) ? AVK_TS_BLUR_DOWN_END : AVK_TS_NONE,
			/* NOT bound to the last SLOT: a slot whose chain is declined never
			 * becomes a pass, so the mark would be lost. Moved onto whichever
			 * chain actually declared last, below. */
			.up_end = AVK_TS_NONE,
			.up_penult_end = (slot_len == 1)
				? AVK_TS_BLUR_UP_PENULT_END : AVK_TS_NONE,
		};
		/*
		 * WHAT THIS CHAIN WOULD HAVE HAD TO PROCESS, derived BEFORE it is
		 * declared -- because the falsifier needs the regions in hand to
		 * scissor a pass to them, and because accumulating the pair here keeps
		 * actual and required over the same frames and the same chains. A
		 * ratio taken across different denominators is the mistake this
		 * pairing exists to prevent.
		 *
		 * The demanded region is the composite's own read region, in
		 * capture-local pixels, as a bounding box. See
		 * blur_required_work_pixels for why that bias is the safe one.
		 */
		struct avk_blur_work work;
		bool have_work = false;
		if (pixman_region32_not_empty(&d->result_region)) {
			struct timespec rb0, rb1;
			clock_gettime(CLOCK_MONOTONIC, &rb0);
			pixman_box32_t rb = *pixman_region32_extents(&d->result_region);
			struct avk_box res_local = {
				rb.x1 - rg.capture.x, rb.y1 - rg.capture.y,
				rb.x2 - rb.x1, rb.y2 - rb.y1,
			};
			have_work = avk_blur_work_of(&params, (uint32_t)rg.capture.width,
				(uint32_t)rg.capture.height, &res_local, &work);
			clock_gettime(CLOCK_MONOTONIC, &rb1);
			/* ONE bracket around the whole derivation for this chain, not one
			 * per rectangle: a timer per operation would cost more than the
			 * arithmetic it measures and would be measuring itself. */
			avk_hist_add(&renderer->blur_region_build_hist,
				(uint64_t)(rb1.tv_sec - rb0.tv_sec) * 1000000000ULL
				+ (uint64_t)(rb1.tv_nsec - rb0.tv_nsec));
		}
		if (avk_blur_declare(graph, &renderer->transients, &renderer->pipes,
				&renderer->blur_stats, r_prefix, r_prefix,
				(uint32_t)rg.capture.width, (uint32_t)rg.capture.height,
				renderer->format, &params, &chain_marks,
				have_work ? &work : NULL)) {
			renderer->blur_results[i] = (struct avk_blur_result){
				.image = prefix, .capture = rg.capture,
			};
			declared_chains++;
			/* THIS chain declared, so it is the last one so far. */
			avk_graph_pass_time_move_end(graph, AVK_TS_BLUR_END);
			if (have_work) {
				renderer->blur_required_work_pixels += work.required_px;
				/* And what each candidate strategy would remove, from the same
				 * derivation and over the same frames. */
				uint64_t up0 = work.up[0].actual_px - work.up[0].required_px;
				renderer->blur_removable_up0_pixels += up0;
				uint64_t up01 = up0;
				if (work.levels >= 2) {
					up01 += work.up[1].actual_px - work.up[1].required_px;
				}
				renderer->blur_removable_up01_pixels += up01;
				uint64_t upc = 0;
				for (uint32_t lv = 0; lv < work.levels; lv++) {
					upc += work.up[lv].actual_px - work.up[lv].required_px;
				}
				renderer->blur_removable_up_pixels += upc;
			}
			/* BOUNDARY 2: the same image, after the chain has written its
			 * result back into it -- compared only where the composite will
			 * read it. */
			if (renderer->oracle.enabled) {
				pixman_region32_t m;
				pixman_region32_init(&m);
				pixman_region32_copy(&m, &d->result_region);
				pixman_region32_translate(&m, -rg.capture.x, -rg.capture.y);
				avk_oracle_tap(&renderer->oracle, graph, r_prefix, prefix,
					tap_box, AVK_TAP_BLUR, i, &m);
				pixman_region32_fini(&m);
			}
		}
	}

	/*
	 * The output frame, expressed as the general primitive: the whole command
	 * range, the scan-out target, origin 0,0, clipped to the frame's damage.
	 * There is no separate "full frame" path to drift away from the regional
	 * one.
	 */
	struct avk_render_segment ctx = {
		.renderer = renderer,
		.scene = scene,
		.target = target,
		.width = width,
		.height = height,
		.layout = target_layout,
		.origin_x = 0,
		.origin_y = 0,
		.begin = 0,
		.end = scene->len,
		/* The frame's damage AFTER blur propagation: a blur whose source moved
		 * changes result pixels over a wider area than the source damage
		 * covers, and those pixels have to be composited. */
		.active = &frame_damage,
		.clear = true,
		.load = true,
		.gradient_set = gradient_set,
	};
	/*
	 * The up phase is only separable when the frame had exactly ONE chain:
	 * with two, BLUR_DOWN_END..BLUR_END is "up0 + prefix1 + chain1", which is
	 * not an upsample cost and must not be recorded as one.
	 */
	avk_timestamps_single_chain(&renderer->timestamps, ts_slot, slot_len == 1);
	/*
	 * The cohort this frame's gpu_frame sample belongs to. Recorded here, with
	 * the frame, because the sample is read back later.
	 *
	 * DECLARED CHAINS, NOT BLUR SLOTS. A slot is a blur node the walker found;
	 * a chain is one avk_blur_declare() that actually built passes, and
	 * avk_blur_declare declines -- too small a capture, zero levels. The marks
	 * this cohort exists to pair with are written by those passes, so a frame
	 * with slots but no successful declare runs no blur on the GPU and belongs
	 * in the idle population.
	 *
	 * Measured, on the sparse-pulse fixture: 75 frames all had slots, but only
	 * 53 declared. Classifying on slot_len put all 75 in the blur cohort,
	 * cohort_idle_frames read 0 on a fixture built to contain idle frames, and
	 * the cohort was indistinguishable from gpu_frame -- the exact failure the
	 * cohort was added to prevent, reintroduced one level up.
	 */
	avk_timestamps_blur_active(&renderer->timestamps, ts_slot,
		declared_chains > 0);
	if ((uint64_t)slot_len > renderer->blur_max_slots) {
		renderer->blur_max_slots = (uint64_t)slot_len;
	}

	if (!avk_render_declare_segment(graph, &ctx, r_target)) {
		avk_cmd_ring_abandon(&renderer->ring);
		return 0;
	}

	/*
	 * BOUNDARY 3: the frame's target, after the output segment and before
	 * presentation -- so this is the GPU's own result, not what a screenshot
	 * saw later. Declared as a graph use, which is what makes reading a foreign
	 * scan-out buffer possible at all: the acquire from and release back to
	 * VK_QUEUE_FAMILY_FOREIGN_EXT are already the graph's job.
	 */
	{
		const struct avk_box out_box = { 0, 0, (int32_t)width, (int32_t)height };
		avk_oracle_tap(&renderer->oracle, graph, r_target, target, out_box,
			AVK_TAP_OUTPUT, 0, NULL);
	}

	/* Compile and record: barriers, the pass, then the exit transitions that
	 * hand every foreign image back to its real owner. */
	avk_graph_execute(graph, cb, &renderer->timestamps, ts_slot);
	/* Recorded; the graph no longer holds these -- nor the regions the segments
	 * pointed at, which live in the slots. */
	free(prefix_segs);
	for (size_t s = 0; s < slot_len; s++) {
		az_blur_damage_finish(&slots[s].damage);
	}
	free(slots);
	renderer->stats.barriers += graph->stats.image_transitions
		+ graph->stats.memory_barriers;

	/* BOTTOM_OF_PIPE against the frame's TOP_OF_PIPE: the pair brackets
	 * everything this command buffer does, including the release barriers,
	 * which is what "GPU frame time" should mean. */
	avk_timestamps_mark(&renderer->timestamps, cb, ts_slot,
		AVK_TS_FRAME_END, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);

	avk_debug_label_end(dev, cb);

	uint64_t value = avk_cmd_ring_submit(&renderer->ring, wait, wait_count,
		signal, signal_count);
	if (value == 0) {
		return 0;
	}
	/* The submission that reads this frame's gradient buffer, so a later growth
	 * knows what to retire the old one against. */
	avk_gradient_store_submitted(&renderer->gradients, value);
	/* Whatever this frame acquired becomes reusable when the GPU passes this
	 * point -- and not when recording ended. Harmless with an empty pool, and
	 * the one line M4F must not have to remember to add. */
	avk_transient_release_frame(&renderer->transients, value);
	/* The point whose passing means this frame's marks can be read without
	 * waiting for anything. */
	avk_timestamps_submitted(&renderer->timestamps, ts_slot, value);

	/*
	 * ── EVERY IMAGE THIS FRAME READ, NOT JUST THE ONE IT WROTE ────────────
	 *
	 * `last_use` is what avk_retire_push() defers a destruction against, so an
	 * image whose last_use is never advanced is an image that can be destroyed
	 * the instant its owner lets go -- while a submitted command buffer is
	 * still sampling it.
	 *
	 * That was true of every client texture. The upload path stamps last_use
	 * (avk_upload.c) and so does the transient pool, but a DMA-BUF import that
	 * is only ever SAMPLED had nothing to advance it: az_avk_buffer_destroy()
	 * would push it with last_use = 0, the collector would find 0 <= completed
	 * immediately, and vkDestroyImage would run on an image the GPU was
	 * reading. Vulkan validation says so directly:
	 *
	 *     VUID-vkDestroyImage-image-01000: vkDestroyImage(): can't be called
	 *     on VkImage [client buffer 776x546] that is currently in use by
	 *     VkCommandBuffer [avk frame cb 0]
	 *
	 * Caught by the M4F.2C.4e validation-layer qualification run, on a
	 * fixture where a client resized early enough to release a buffer inside
	 * the first frame's lifetime. Every other run had simply not raced.
	 *
	 * MAX, not assignment: submissions complete in order on one queue, but an
	 * image sampled by an earlier frame that is still in flight must keep the
	 * later point, and taking the maximum makes that true without depending on
	 * the ordering.
	 */
	if (!avk_no_sampled_last_use()) {
		for (size_t i = 0; i < scene->len; i++) {
			struct avk_image *img = scene->cmds[i].image;
			if (img != NULL && img->last_use < value) {
				img->last_use = value;
			}
		}
	}

	/* The barriers above have already recorded where the target ended up --
	 * GENERAL for a scan-out buffer, COLOR_ATTACHMENT_OPTIMAL for one of our
	 * own. Whoever presents or reads it next transitions from what the image
	 * says, not from an assumption. */
	target->last_use = value;
	renderer->stats.frames++;

	struct timespec end;
	clock_gettime(CLOCK_MONOTONIC, &end);
	uint64_t frame_record_ns = (uint64_t)(end.tv_sec - start.tv_sec)
		* 1000000000ULL + (uint64_t)(end.tv_nsec - start.tv_nsec);
	/* MINUS the backpressure wait: record_ns describes work, not waiting. */
	frame_record_ns = frame_record_ns > ring_wait_ns
		? frame_record_ns - ring_wait_ns : 0;
	renderer->stats.cpu_ring_wait_ns += ring_wait_ns;
	avk_hist_add(&renderer->stats.ring_wait_hist, ring_wait_ns);
	renderer->stats.cpu_record_ns += frame_record_ns;
	/* And the same frame's own value into the distribution, because a mean
	 * over a run hides the frames that miss a vblank -- which are the only
	 * ones a frame budget is about. */
	avk_hist_add(&renderer->stats.record_hist, frame_record_ns);

	/* Backpressure stalls in the command ring are the only CPU waits the
	 * frame path can incur, so they are reported as exactly that. */
	renderer->stats.cpu_sync_waits = renderer->ring.stalls;
	return value;
}

void avk_renderer_log_stats(const struct avk_renderer *renderer) {
	const struct avk_renderer_stats *s = &renderer->stats;
	avk_log(AVK_INFO, "avk.frames=%" PRIu64 " avk.surfaces=%" PRIu64
		" avk.rects=%" PRIu64 " avk.draws=%" PRIu64 " avk.barriers=%" PRIu64,
		s->frames, s->surfaces, s->rects, s->draws, s->barriers);
	avk_log(AVK_INFO, "avk.cpu_sync_waits=%" PRIu64
		" avk.record_us_avg=%" PRIu64, s->cpu_sync_waits,
		s->frames ? s->cpu_record_ns / s->frames / 1000 : 0);
	/* Kept on a separate line from record_us_avg, and named gpu_, so the two
	 * cannot be read as the same measurement. */
	const struct avk_timestamps *ts = &renderer->timestamps;
	if (ts->supported) {
		avk_log(AVK_INFO, "avk.gpu_frame_us_avg=%" PRIu64
			" avk.gpu_samples=%" PRIu64 " avk.gpu_dropped=%" PRIu64,
			ts->samples ? ts->gpu_frame_ns_total / ts->samples / 1000 : 0,
			ts->samples, ts->dropped);
	} else {
		avk_log(AVK_INFO, "avk.gpu_frame_us_avg=UNSUPPORTED");
	}
}
