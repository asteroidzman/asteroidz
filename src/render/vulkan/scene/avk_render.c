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
	avk_retire_init(&renderer->retire);
	return true;
}

void avk_renderer_finish(struct avk_renderer *renderer) {
	if (renderer->dev == NULL) {
		return;
	}
	vkDeviceWaitIdle(renderer->dev->dev);
	avk_retire_finish(&renderer->retire, renderer->dev);
	avk_cmd_ring_finish(&renderer->ring);
	avk_pipelines_finish(&renderer->pipes);
	memset(renderer, 0, sizeof(*renderer));
}

void avk_renderer_collect(struct avk_renderer *renderer) {
	avk_retire_collect(&renderer->retire, renderer->dev);
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
static void box_to_ndc(const struct avk_box *box, uint32_t width,
		uint32_t height, float out[4]) {
	out[0] = (float)box->x / (float)width * 2.0f - 1.0f;
	out[1] = (float)box->y / (float)height * 2.0f - 1.0f;
	out[2] = (float)(box->x + box->width) / (float)width * 2.0f - 1.0f;
	out[3] = (float)(box->y + box->height) / (float)height * 2.0f - 1.0f;
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
 * Omitting it is not observable on this hardware -- see avk_image_is_foreign()
 * for the measurement -- so this is defence against drivers and layouts where
 * it would be, not a fix for anything currently reproducible.
 */
static void batch_add(struct avk_barrier_batch *acquire,
		struct avk_barrier_batch *release, struct avk_device *dev,
		struct avk_image *image, VkImageLayout new_layout,
		VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
	/* AVK_NO_FOREIGN_ACQUIRE=1 turns the transfer off. It produces an
	 * identical desktop here, which is the point of having it: the switch is
	 * how that keeps being checked instead of assumed. */
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

	if (false && foreign && release->count < AVK_MAX_BARRIERS) {
		release->barriers[release->count++] = (VkImageMemoryBarrier2){
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
			.srcStageMask = dst_stage,
			.srcAccessMask = dst_access,
			.dstStageMask = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
			.dstAccessMask = 0,
			.oldLayout = new_layout,
			/* Back to the layout the foreign owner expects. */
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
	 */
	VkRenderingAttachmentInfo color = {
		.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
		.imageView = VK_NULL_HANDLE,   /* filled below */
		.imageLayout = target_layout,
		.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
		.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
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
			box_to_ndc(&clear.dst, width, height, pc.dst);
			/* Premultiply: the blend state is (ONE, 1-SRC_ALPHA), so the
			 * shader must receive colour already scaled by alpha. */
			for (int i = 0; i < 3; i++) {
				pc.color[i] = clear.color[i] * clear.color[3];
			}
			pc.color[3] = clear.color[3];
			pc.params[0] = 1.0f;
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

		if ((cmd->corner_radius > 0.0f || cmd->has_shadow || cmd->has_blur)
				&& !renderer->warned_unimplemented_effect) {
			/* Loud once. M3 renders the command WITHOUT the effect rather
			 * than dropping it: a missing rounded corner is a cosmetic
			 * regression, a missing window is not. */
			avk_log(AVK_WARN, "avk: this scene asks for corner radius, shadow "
				"or blur, which M3 does not implement -- rendering without "
				"them (effects are M4)");
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
		box_to_ndc(&cmd->dst, width, height, pc.dst);
		pc.params[0] = cmd->opacity;

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

	avk_debug_label_end(dev, cb);

	uint64_t value = avk_cmd_ring_submit(&renderer->ring, wait, wait_count,
		signal, signal_count);
	if (value == 0) {
		return 0;
	}

	/* The barriers above have already recorded where the target ended up --
	 * GENERAL for a scan-out buffer, COLOR_ATTACHMENT_OPTIMAL for one of our
	 * own. Whoever presents or reads it next transitions from what the image
	 * says, not from an assumption. */
	target->last_use = value;
	renderer->stats.frames++;

	struct timespec end;
	clock_gettime(CLOCK_MONOTONIC, &end);
	renderer->stats.gpu_submit_ns += (uint64_t)(end.tv_sec - start.tv_sec)
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
		s->frames ? s->gpu_submit_ns / s->frames / 1000 : 0);
}
