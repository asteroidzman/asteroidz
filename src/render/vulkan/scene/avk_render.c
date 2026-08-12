#define _POSIX_C_SOURCE 200809L

#include "avk_render.h"

#include <stdlib.h>
#include "../debug/avk_debug.h"

#include <inttypes.h>
#include <string.h>
#include <time.h>

bool avk_renderer_init(struct avk_renderer *renderer, struct avk_device *dev,
		VkFormat format) {
	memset(renderer, 0, sizeof(*renderer));
	renderer->dev = dev;
	renderer->format = format;

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

	/* M4A breaks, read once. Each restores a specific wrong implementation
	 * rather than merely disabling the feature -- "single radius" and "scaled
	 * twice" are the two mistakes that render plausibly and are therefore the
	 * ones worth having a falsifier for. */
	renderer->break_rounded_off = getenv("AZ_ROUNDED_OFF") != NULL;
	renderer->break_rounded_single = getenv("AZ_ROUNDED_SINGLE_RADIUS") != NULL;
	renderer->break_bottom_swap = getenv("AZ_ROUNDED_BOTTOM_SWAP") != NULL;
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
	/* Before the retire queue is drained: growth pushes old gradient buffers
	 * onto that queue, and finishing the store destroys only the buffers the
	 * slots still hold. */
	avk_gradient_store_finish(&renderer->gradients);
	avk_timestamps_finish(&renderer->timestamps);
	avk_retire_finish(&renderer->retire, renderer->dev);
	avk_cmd_ring_finish(&renderer->ring);
	avk_pipelines_finish(&renderer->pipes);
	memset(renderer, 0, sizeof(*renderer));
}

void avk_renderer_collect(struct avk_renderer *renderer) {
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
	case AVK_TRANSFORM_90:
		/* dst +x runs along src -y, dst +y along src +x */
		ox = x0; oy = y1; ax = 0; ay = -h; bx = w; by = 0;
		break;
	case AVK_TRANSFORM_180:
		ox = x1; oy = y1; ax = -w; ay = 0; bx = 0; by = -h;
		break;
	case AVK_TRANSFORM_270:
		ox = x1; oy = y0; ax = 0; ay = h; bx = -w; by = 0;
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
 * All of a frame's layout transitions, in ONE barrier, BEFORE the rendering
 * instance begins.
 *
 * Both halves of that sentence are requirements rather than preferences, and
 * validation is what taught them -- the pixels were already correct with the
 * barriers in the wrong place, which is exactly the class of bug that works on
 * the machine it was written on and corrupts on the next driver:
 *
 *  - INSIDE a dynamic-rendering instance, vkCmdPipelineBarrier2 may carry only
 *    memory barriers, and a layout transition is forbidden outright
 *    (VUID-vkCmdPipelineBarrier2-oldLayout-01181). Sampled surfaces therefore
 *    have to reach SHADER_READ_ONLY_OPTIMAL before vkCmdBeginRendering, not on
 *    demand when the command that samples them comes up.
 *  - loadOp LOAD is a COLOR_ATTACHMENT_READ of the target, not only a write.
 *    A barrier whose dstAccessMask covers only the write leaves the load
 *    unsynchronised against whatever produced the previous frame's contents,
 *    which synchronization validation reports as a read-after-write hazard at
 *    vkCmdBeginRendering.
 *
 * Batching them is then free: one call, one pipeline flush, however many
 * images the frame touches.
 */
#define AVK_MAX_BARRIERS 64

struct avk_barrier_batch {
	VkImageMemoryBarrier2 barriers[AVK_MAX_BARRIERS];
	uint32_t count;
};

/*
 * Queue an acquire for `image`, and its matching release if the image is
 * foreign.
 *
 * The foreign pair is not optional bookkeeping. A client's dma-buf belongs to
 * the client's device as far as Vulkan is concerned, and it has to be acquired
 * from VK_QUEUE_FAMILY_FOREIGN_EXT before it is read and released back
 * afterwards -- see avk_image_is_foreign(). The acquire is what makes the
 * pixels visible; the release is what leaves the buffer in a state its real
 * owner can use again.
 *
 * The release is not optional and not defensive: without it a real display
 * shows a flat white screen. See avk_image_is_foreign() for how that was
 * learned and why no headless test could have told us.
 */
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

static void batch_add(struct avk_barrier_batch *acquire,
		struct avk_barrier_batch *release, struct avk_device *dev,
		struct avk_image *image, VkImageLayout new_layout,
		VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
	/* AVK_NO_FOREIGN_ACQUIRE=1 turns the whole transfer off -- both halves.
	 * On a real output that means a white screen, so it is a diagnostic, not
	 * a tuning knob. */
	static int no_foreign = -1;
	if (no_foreign < 0) {
		const char *env = getenv("AVK_NO_FOREIGN_ACQUIRE");
		no_foreign = env != NULL && env[0] == '1';
	}
	bool foreign = !no_foreign && avk_image_is_foreign(image);
	if (!foreign && image->layout == new_layout) {
		return;
	}
	if (acquire->count >= AVK_MAX_BARRIERS) {
		return;
	}
	/* Guard against listing the same image twice -- two commands sampling one
	 * surface is completely ordinary, and a duplicate barrier whose oldLayout
	 * has already been consumed is invalid. */
	for (uint32_t i = 0; i < acquire->count; i++) {
		if (acquire->barriers[i].image == image->image) {
			return;
		}
	}

	acquire->barriers[acquire->count++] = (VkImageMemoryBarrier2){
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
		/* ALL_COMMANDS on the source side: the previous writer might have been
		 * an upload, a previous frame, or a client's own submission reaching
		 * us through an imported fence, and this layer does not know which. */
		.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
		.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
		.dstStageMask = dst_stage,
		.dstAccessMask = dst_access,
		.oldLayout = image->layout,
		.newLayout = new_layout,
		.srcQueueFamilyIndex = foreign ? VK_QUEUE_FAMILY_FOREIGN_EXT
			: VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = foreign ? dev->caps.graphics_family
			: VK_QUEUE_FAMILY_IGNORED,
		.image = image->image,
		.subresourceRange = {
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.levelCount = 1,
			.layerCount = 1,
		},
	};

	if (foreign && release->count < AVK_MAX_BARRIERS) {
		release->barriers[release->count++] = (VkImageMemoryBarrier2){
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
			.srcStageMask = dst_stage,
			.srcAccessMask = dst_access,
			/* NONE, not BOTTOM_OF_PIPE: the second scope of a release is
			 * ignored, and synchronization2 wants NONE said explicitly. */
			.dstStageMask = VK_PIPELINE_STAGE_2_NONE,
			.dstAccessMask = 0,
			.oldLayout = new_layout,
			/* Back to the layout the foreign owner expects. THIS IS WHAT
			 * MAKES THE FRAME VISIBLE. Without the release, the scan-out
			 * buffer is never handed back to KMS in a state the display
			 * engine can read, and on a compressed AMD modifier the monitor
			 * shows a flat white screen -- windows and all, rendered
			 * perfectly, into an image nothing can interpret. A headless test
			 * cannot catch it, because nothing scans a headless buffer out. */
			.newLayout = VK_IMAGE_LAYOUT_GENERAL,
			.srcQueueFamilyIndex = dev->caps.graphics_family,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT,
			.image = image->image,
			.subresourceRange = {
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.levelCount = 1,
				.layerCount = 1,
			},
		};
		/* The image ends the frame back in the foreign owner's hands, so that
		 * is what the next frame must acquire from. */
		image->layout = VK_IMAGE_LAYOUT_GENERAL;
	} else {
		image->layout = new_layout;
	}
}

static void batch_submit(struct avk_barrier_batch *batch, VkCommandBuffer cb,
		struct avk_renderer *renderer) {
	if (batch->count == 0) {
		return;
	}
	VkDependencyInfo dep = {
		.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
		.imageMemoryBarrierCount = batch->count,
		.pImageMemoryBarriers = batch->barriers,
	};
	vkCmdPipelineBarrier2(cb, &dep);
	renderer->stats.barriers += batch->count;
	batch->count = 0;
}

/* The rectangles a command may actually touch: its own clip, intersected with
 * the frame's damage. Returned as a region the caller iterates -- one draw per
 * rectangle, which is what keeps damage meaningful rather than collapsing it
 * to a bounding box that covers the whole screen the moment two corners of the
 * display update. */
static void command_region(const struct avk_cmd *cmd,
		const pixman_region32_t *damage, uint32_t width, uint32_t height,
		pixman_region32_t *out) {
	pixman_region32_init_rect(out, 0, 0, width, height);
	pixman_region32_intersect_rect(out, out, cmd->dst.x, cmd->dst.y,
		(unsigned)cmd->dst.width, (unsigned)cmd->dst.height);
	if (cmd->has_clip) {
		pixman_region32_intersect(out, out, (pixman_region32_t *)&cmd->clip);
	}
	pixman_region32_intersect(out, out, (pixman_region32_t *)damage);
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

	VkCommandBuffer cb = avk_cmd_ring_begin(&renderer->ring);
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

	/* Every layout transition this frame needs, decided up front. */
	struct avk_barrier_batch batch = { .count = 0 };
	struct avk_barrier_batch release = { .count = 0 };
	/* A scan-out buffer is foreign too -- KMS is its other owner -- so it is
	 * rendered in GENERAL rather than COLOR_ATTACHMENT_OPTIMAL. The optimal
	 * layout would have to be released back to GENERAL anyway, and dynamic
	 * rendering is perfectly happy with GENERAL. */
	VkImageLayout target_layout = avk_image_is_foreign(target)
		? VK_IMAGE_LAYOUT_GENERAL
		: VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	batch_add(&batch, &release, dev, target, target_layout,
		VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
		VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT
		| VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
	for (size_t i = 0; i < scene->len; i++) {
		const struct avk_cmd *cmd = &scene->cmds[i];
		if (cmd->type == AVK_CMD_TEXTURE && cmd->image != NULL) {
			batch_add(&batch, &release, dev, cmd->image,
				VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
				VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
		}
	}
	batch_submit(&batch, cb, renderer);

	/*
	 * loadOp LOAD, always.
	 *
	 * A damaged frame redraws only part of the target and the rest must
	 * survive; CLEAR here would be a full-screen clear masquerading as
	 * damage tracking, which looks correct on a full-damage frame and
	 * destroys everything else. The background clear is a normal draw
	 * command, scissored to the damage like everything else.
	 *
	 * AVK_NO_LOAD_PRESERVE=1 replaces it with CLEAR to magenta, which is the
	 * break test for partial damage: it is precisely the mistake described
	 * above, and everything outside the damage becomes a colour nothing else
	 * on a desktop produces.
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
		.imageView = VK_NULL_HANDLE,   /* filled below */
		.imageLayout = target_layout,
		.loadOp = break_preserve
			? VK_ATTACHMENT_LOAD_OP_CLEAR
			: VK_ATTACHMENT_LOAD_OP_LOAD,
		.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		.clearValue = { .color = { .float32 = { 1.0f, 0.0f, 1.0f, 1.0f } } },
	};

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
	color.imageView = target->view;

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
	if (scene->has_clear) {
		struct avk_cmd clear = {
			.type = AVK_CMD_RECT,
			.dst = { 0, 0, (int32_t)width, (int32_t)height },
			.opacity = 1.0f,
		};
		memcpy(clear.color, scene->clear_color, sizeof(clear.color));

		pixman_region32_t region;
		command_region(&clear, &scene->damage, width, height, &region);
		int count = 0;
		const pixman_box32_t *rects =
			pixman_region32_rectangles(&region, &count);
		if (count > 0) {
			vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
				renderer->pipes.rect);
			bound = renderer->pipes.rect;

			struct avk_push_constants pc = {0};
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
				VkRect2D scissor = {
					{ rects[i].x1, rects[i].y1 },
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

	for (size_t i = 0; i < scene->len; i++) {
		const struct avk_cmd *cmd = &scene->cmds[i];

		if ((cmd->has_shadow || cmd->has_blur)
				&& !renderer->warned_unimplemented_effect) {
			/* Loud once, and still loud: rounded corners are implemented
			 * (M4A) but shadows and blur are not, and a compositor that
			 * quietly renders an incomplete desktop is worse than one that
			 * says so. The warning goes when the effect arrives, not before.
			 */
			avk_log(AVK_WARN, "avk: this scene asks for shadow or blur, which "
				"is not implemented yet -- rendering without them (M4D/M4F)");
			renderer->warned_unimplemented_effect = true;
		}

		pixman_region32_t region;
		command_region(cmd, &scene->damage, width, height, &region);
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
		if (cmd->type == AVK_CMD_TEXTURE) {
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
			VkRect2D scissor = {
				{ rects[r].x1, rects[r].y1 },
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

	/* Hand every foreign image back to its real owner: the client that will
	 * draw into its buffer again, and KMS, which is about to scan this frame
	 * out. */
	batch_submit(&release, cb, renderer);

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
	/* The point whose passing means this frame's marks can be read without
	 * waiting for anything. */
	avk_timestamps_submitted(&renderer->timestamps, ts_slot, value);

	/* The barriers above have already recorded where the target ended up --
	 * GENERAL for a scan-out buffer, COLOR_ATTACHMENT_OPTIMAL for one of our
	 * own. Whoever presents or reads it next transitions from what the image
	 * says, not from an assumption. */
	target->last_use = value;
	renderer->stats.frames++;

	struct timespec end;
	clock_gettime(CLOCK_MONOTONIC, &end);
	renderer->stats.cpu_record_ns += (uint64_t)(end.tv_sec - start.tv_sec)
		* 1000000000ULL + (uint64_t)(end.tv_nsec - start.tv_nsec);

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
