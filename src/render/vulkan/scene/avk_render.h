#ifndef AVK_RENDER_H
#define AVK_RENDER_H

#include "../command/avk_command.h"
#include "../command/avk_retire.h"
#include "../pipeline/avk_pipeline.h"
#include "avk_scene.h"

/*
 * The frame renderer: an avk_scene in, GPU work out.
 *
 * This is the function that exists instead of wlr_renderer_begin_buffer_pass()
 * + wlr_render_pass_add_texture() + wlr_render_pass_submit(). Nothing in here
 * or below it touches a wlr_renderer, a wlr_render_pass or a wlr_texture, and
 * tests/check-vulkan-isolation.py fails the build if that ever stops being
 * true.
 */

struct avk_renderer_stats {
	/* M4A. Proof the rounded path is being taken at all, and how often it is
	 * taken with corners that differ -- the case a single-radius
	 * implementation renders wrong while looking almost right. */
	uint64_t rounded_clip_draws;
	uint64_t rounded_asymmetric_draws;
	uint64_t frames;
	uint64_t surfaces;
	uint64_t rects;
	uint64_t draws;          /* commands x damage rects */
	uint64_t barriers;
	uint64_t cpu_sync_waits; /* MUST stay 0 on the steady-state frame path */
	uint64_t gpu_submit_ns;
};

struct avk_renderer {
	/* M4A break switches; see avk_render.c. Read once at init, never in the
	 * draw loop, so a break costs nothing when it is off. */
	bool break_rounded_off;
	bool break_rounded_single;
	bool break_bottom_swap;
	bool break_rounded_double_scale;
	float break_scale_hint;
	struct avk_device *dev;
	struct avk_pipelines pipes;
	struct avk_cmd_ring ring;
	struct avk_retire_queue retire;
	VkFormat format;

	struct avk_renderer_stats stats;

	/* Said once per renderer rather than once per frame: a command asking for
	 * an effect M3 does not implement is worth exactly one warning. */
	bool warned_unimplemented_effect;
};

bool avk_renderer_init(struct avk_renderer *renderer, struct avk_device *dev,
	VkFormat format);
void avk_renderer_finish(struct avk_renderer *renderer);

/*
 * Render `scene` into `target`.
 *
 * `target` must be an image created with COLOR_ATTACHMENT usage in the
 * renderer's format. Its `layout` field is read and updated, so the caller
 * never has to remember what state it left the image in.
 *
 * Returns the timeline value the submission will signal -- what to wait on,
 * what to export as a fence, and what to retire resources against -- or 0 on
 * failure. Does NOT block: there is no fence wait, no queue wait and no device
 * wait anywhere on this path.
 *
 * `wait` and `signal` are passed through to the submission, which is how a
 * client's acquire fence gets waited on by the GPU rather than the CPU, and
 * how the render-completion semaphore gets exported for KMS.
 */
uint64_t avk_render_frame(struct avk_renderer *renderer,
	struct avk_image *target, const struct avk_scene *scene,
	const VkSemaphoreSubmitInfo *wait, uint32_t wait_count,
	const VkSemaphoreSubmitInfo *signal, uint32_t signal_count);

/* Retire whatever the GPU has finished with. Once per frame; never blocks. */
void avk_renderer_collect(struct avk_renderer *renderer);

void avk_renderer_log_stats(const struct avk_renderer *renderer);

#endif /* AVK_RENDER_H */
