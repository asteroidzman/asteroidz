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

/*
 * THE REFERENCE BACKDROP, and why a dither amplitude needs one at all.
 *
 * WHY +-1/255 OF COVERAGE IS NOT +-1 OUTPUT CODE. AVK composites with
 * srcColor*1 + dstColor*(1 - srcAlpha) into an 8-bit UNORM attachment, and a
 * black shadow's premultiplied source colour is zero, so the framebuffer value
 * is exactly
 *
 *     out = dst * (1 - alpha)      =>      d(out)/d(alpha) = -dst
 *
 * The perturbation the viewer sees is the perturbation of ALPHA times the
 * BACKDROP. On the mid-grey where the rings were reported, dst = 45/255, so a
 * whole 1/255 of coverage moves the output by 45/255 * 1/255 -- about
 * 0.18 of one output code. Under a fifth of a step: it would change nothing at
 * all. This is the calculation the naive `coverage += noise/255.0` skips.
 *
 * Inverting it for one code peak-to-peak:
 *
 *     amplitude = 1 / (max_code * dst_ref)
 *               = 1 / (255 * 45/255)  =  1/45  =  5.7/255
 *
 * A SHADOW SHADER CANNOT READ ITS DESTINATION, so `dst_ref` has to be a
 * constant, and this is the honest limitation of doing anti-banding here
 * rather than at an output-encoding stage that knows the composed value. The
 * choice of 45/255 is not arbitrary: a fixed alpha-domain dither produces an
 * output excursion PROPORTIONAL to the backdrop, which is exactly backwards
 * from what is wanted -- dark backdrops band worst and receive least. So the
 * constant is calibrated for the dark end, where banding is visible, and the
 * grain it produces on light backdrops (where there was no banding to fix) is
 * measured rather than assumed. See test_dither_breaks_banding().
 *
 * dst_ref is a BACKDROP RATIO, deliberately, not a luminance in nits. M5
 * introduces per-window luminance domains and "SDR white is X" stops being
 * globally true; a ratio against the attachment's own range does not care.
 */
#define AVK_DITHER_REF_BACKDROP (45.0f / 255.0f)

/*
 * Peak-to-peak dither for one attachment format, in alpha units.
 *
 * Derived from the target's precision rather than hardcoded for 8 bits, so a
 * 10-bit scanout automatically gets a quarter of the amplitude and a
 * floating-point intermediate gets none. Returning 0 disables the dither
 * entirely in the shader.
 */
static inline float avk_dither_amplitude(VkFormat format) {
	float max_code;
	switch (format) {
	case VK_FORMAT_B8G8R8A8_UNORM:
	case VK_FORMAT_R8G8B8A8_UNORM:
	case VK_FORMAT_B8G8R8A8_SRGB:
	case VK_FORMAT_R8G8B8A8_SRGB:
		max_code = 255.0f;
		break;
	case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
	case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
		max_code = 1023.0f;
		break;
	case VK_FORMAT_R16G16B16A16_UNORM:
		max_code = 65535.0f;
		break;
	case VK_FORMAT_R16G16B16A16_SFLOAT:
		/* No uniform quantisation to decorrelate, and M5's scene-linear
		 * intermediate will be exactly this. Injecting noise into a
		 * high-precision path would be pure loss. */
		return 0.0f;
	default:
		/* Unknown precision: assume the worst, because an 8-bit target left
		 * undithered is the defect this exists to fix, whereas a little
		 * unnecessary dither on a deeper one is invisible. */
		max_code = 255.0f;
		break;
	}
	return 1.0f / (max_code * AVK_DITHER_REF_BACKDROP);
}

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
	/*
	 * M4D.2 break. Re-centres the shadow's envelope on the window it belongs
	 * to, which is precisely how a symmetric centred glow differs from a
	 * directional shadow.
	 *
	 * The directionality does NOT live in the shader -- it lives in where the
	 * compositor puts the envelope. client_draw_one_shadow() grows the window
	 * box by (size + border) on every side and THEN shifts it by
	 * shadows_position_y, so after the shader insets by sigma the caster is
	 * the window displaced downward. There is more room below it than above,
	 * and that is the whole mechanism.
	 *
	 * So the falsifier is to undo the displacement, which the renderer can do
	 * because `inner` is the window's own footprint. It restores the look
	 * M4D.2 exists to move away from rather than merely turning shadows off.
	 */
	bool break_shadow_symmetric;
	/*
	 * M4D.4 break. Turns the dither off, restoring the banded shadow the live
	 * session showed: a smooth falloff quantised to nine 8-bit levels reads as
	 * concentric halos on a flat backdrop.
	 */
	bool break_shadow_no_dither;
	float shadow_dither;
	bool dither_hash;
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
