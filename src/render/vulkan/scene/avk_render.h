#ifndef AVK_RENDER_H
#define AVK_RENDER_H

#include "../command/avk_command.h"
#include "../command/avk_retire.h"
#include "../command/avk_timestamp.h"
#include "../pipeline/avk_gradient.h"
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
	/* M4B. A border is a command carrying an interior cut-out, so these count
	 * annuli: how many were drawn, how many had rounded inner arcs rather than
	 * a plain square hole, and how many of those had inner arcs that differ
	 * per corner -- the titlebar case a single-radius inner edge renders
	 * wrong while looking almost right. */
	uint64_t border_draws;
	uint64_t rounded_border_draws;
	uint64_t asymmetric_border_draws;
	/* M4D. Same three questions for shadows, and for the same reason: a
	 * single-radius shadow renders plausibly and is wrong on exactly the
	 * windows whose corners differ. */
	uint64_t shadow_draws;
	uint64_t rounded_shadow_draws;
	uint64_t asymmetric_shadow_draws;
	uint64_t frames;
	uint64_t surfaces;
	uint64_t rects;
	uint64_t draws;          /* commands x damage rects */
	uint64_t barriers;
	uint64_t cpu_sync_waits; /* MUST stay 0 on the steady-state frame path */
	/*
	 * CPU wall-clock spent RECORDING and SUBMITTING a frame -- not GPU
	 * execution time, and named so it cannot be read as such. Reporting this
	 * number as GPU cost would understate a shader-bound frame and overstate
	 * a submission-bound one, in opposite directions.
	 *
	 * GPU time is a separate measurement and lives in `timestamps` below
	 * (M4D.P). The two are deliberately not folded into one field.
	 */
	uint64_t cpu_record_ns;
};

struct avk_renderer {
	/* M4A break switches; see avk_render.c. Read once at init, never in the
	 * draw loop, so a break costs nothing when it is off. */
	bool break_rounded_off;
	bool break_rounded_single;
	bool break_bottom_swap;
	bool break_rounded_double_scale;
	float break_scale_hint;
	/*
	 * M4D break. Restores a single-radius shadow -- every corner taking the
	 * top-left's -- which is what an implementation that carried one scalar
	 * radius produces. It renders plausibly on any window whose corners
	 * match, which is most of them, and is wrong on every titlebar-joined
	 * one.
	 */
	bool break_shadow_single_radius;
	struct avk_device *dev;
	struct avk_pipelines pipes;
	struct avk_cmd_ring ring;
	struct avk_retire_queue retire;
	/* M4C. One per renderer, one buffer per frame in flight; see
	 * avk_gradient.h for why the lifetime needs no wait. */
	struct avk_gradient_store gradients;
	/* M4D.P. Generic, not shadow-specific: four marks a frame, read back
	 * without waiting. Disabled and harmless where the device cannot. */
	struct avk_timestamps timestamps;
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
