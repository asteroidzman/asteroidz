#ifndef AVK_RENDER_H
#define AVK_RENDER_H

#include "../command/avk_command.h"
#include "../command/avk_retire.h"
#include "../command/avk_timestamp.h"
#include "../graph/avk_graph.h"
#include "../effect/avk_blur.h"
#include "../graph/avk_transient.h"
#include "../pipeline/avk_gradient.h"
#include "../pipeline/avk_pipeline.h"
#include "avk_oracle.h"
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

/*
 * One blur command's finished result for this frame.
 *
 * The capture box travels with the image because the result covers the CAPTURE
 * region, not the write box -- the composite needs both to know where the write
 * box sits inside it.
 */
struct avk_blur_result {
	struct avk_image *image;
	struct avk_box capture;
};

/*
 * ── ONE BLUR'S DAMAGE, AS SIX DISTINCT REGIONS ────────────────────────────
 *
 * Collapsing any two of these still renders a picture, and the picture is wrong
 * in a way that only shows when something moves. They are kept apart, named
 * apart, and counted apart.
 *
 *   write            where the result is composited: the node's box, clipped by
 *                    its own clip region. Not a bounding box -- a client's
 *                    two-rectangle region stays two rectangles.
 *
 *   dependency       source pixels that can reach `write`: write dilated by the
 *                    REVERSE support, bounded by the scene. The blur's source
 *                    domain if everything were being recomputed.
 *
 *   source_damage    prefix damage landing inside `dependency`. The only source
 *                    change that can matter to this blur.
 *
 *   output_damage    result pixels that may differ: source_damage dilated by
 *                    the FORWARD support, intersected with `write`, unioned
 *                    with any material-only invalidation. This is what gets
 *                    composited and what later blurs inherit.
 *
 *   prefix_rebuild   source pixels that must be reconstructed to compute
 *                    output_damage: output_damage dilated by the REVERSE
 *                    support again, bounded by the capture.
 *
 *                    NOT the same as source_damage, and bigger than it: the
 *                    filter needs a neighbourhood around every pixel it
 *                    recomputes. Rendering only the source-damaged pixels into
 *                    the transient is the mistake this field exists to name.
 *
 *   aligned          prefix_rebuild's extents, grown to the even origin the
 *                    prefix segment's derivative quads need.
 *
 * The invariant, and it is asserted rather than believed:
 *
 *     output_damage  subset-of  write
 *     source_damage  subset-of  dependency
 *     prefix_rebuild subset-of  capture
 */
struct avk_blur_damage {
	pixman_region32_t write;
	pixman_region32_t dependency;
	pixman_region32_t source_damage;
	pixman_region32_t output_damage;
	pixman_region32_t result_region;
	pixman_region32_t prefix_rebuild;
	struct avk_blur_regions regions;
	/* False when nobody needs a pixel of this blur's result this frame. Its
	 * command stays in the stream -- the scene is not rewritten -- but no
	 * capture, no chain and no composite are declared for it, and its
	 * blur_results entry stays NULL, which is the same state a chain that
	 * could not be built leaves behind. */
	bool active;
};

/*
 * ── DAMAGE FORWARD, DEMAND BACKWARD ───────────────────────────────────────
 *
 * Two sweeps over the same scene order, in opposite directions, computing two
 * different things. Conflating them is the bug this comment exists to prevent:
 * they answer "what CHANGED" and "what must be RECOMPUTED", and neither
 * contains the other.
 *
 * SWEEP 1, INCREASING SCENE ORDER -- DAMAGE.
 *
 *     prefix_damage = the frame's damage
 *     for each blur k in increasing order:
 *         source_damage(k) = prefix_damage  ∩  dependency(k)
 *         output_damage(k) = dilate_forward(source_damage(k))  ∩  write(k)
 *         prefix_damage   ∪= output_damage(k)      <- later blurs see it
 *         frame_damage    ∪= output_damage(k)      <- the output must show it
 *
 * A blur's own output joins the prefix damage only AFTER its own source damage
 * has been read, which is the code-level reason a blur can never feed itself.
 * An ordinary command needs no step of its own: it draws only where it is
 * already damaged, so it cannot enlarge the damage.
 *
 * ONE PASS SUFFICES, and this is a property of the architecture rather than
 * luck. A blur at index k samples exactly [0, k), so every dependency edge runs
 * from a lower scene index to a higher one: the dependency graph is a DAG whose
 * topological order IS the scene order. Visiting in increasing index therefore
 * reaches a fixed point in one sweep, and an iterative solver would be a loop
 * that always ran exactly once. If a feedback edge ever appears -- a blur
 * sampling something drawn after it -- this stops being true, which is one more
 * reason BREAK=blur-scene-after is kept alive.
 *
 * SWEEP 2, DECREASING SCENE ORDER -- DEMAND.
 *
 *     demand = frame_damage           <- what the OUTPUT still needs composited
 *     for each blur k in decreasing order:
 *         result_region(k)  = demand ∩ write(k)
 *         prefix_rebuild(k) = dilate_reverse(result_region(k)) ∩ capture(k)
 *         demand           ∪= prefix_rebuild(k)   <- earlier commands owe it
 *
 * Demand flows the other way because a blur's PREFIX is a composite of
 * everything before it: asking for a blur's result over some region asks every
 * earlier command for its contribution over a LARGER region -- larger by the
 * filter's support, which is what prefix_rebuild is.
 *
 * This is what makes an earlier blur's result available to a later one's
 * prefix. Skipping a blur because its own source did not change would be wrong
 * exactly when a later blur is rebuilding prefix pixels that lie inside it; the
 * demand sweep is what turns that into a region rather than a special case.
 *
 * Both sweeps are single-pass and neither iterates. Damage never flows
 * backwards, and demand never flows forwards.
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
	/*
	 * M4F. What the blur node's material actually did, as opposed to what the
	 * scene asked for. Counted at the DRAW, so a fixture can tell "the soft-edge
	 * pipeline ran" from "edge_softness was set in the command" -- which are two
	 * different claims and only one of them is about the renderer.
	 */
	uint64_t blur_draws;
	uint64_t blur_soft_draws;
	uint64_t blur_darken_passes;
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
	/*
	 * M4F.2A.2 break. Replays [0, scene->len) into the prefix instead of
	 * [0, k) -- the WHOLE scene, including everything drawn after the blur
	 * node.
	 *
	 * That is precisely the reference's failure class: SceneFX's live blur
	 * samples the previous frame's FINAL COMPOSITE, so the owning window's own
	 * pixels land in its source and spread outward as a halo in the window's
	 * own colour. sample_exclude exists there to repair it by edge extension.
	 *
	 * AVK has no such repair, deliberately: the break must expose the defect
	 * rather than be papered over, because the whole argument for prefix
	 * capture is that the defect becomes structurally impossible.
	 */
	bool break_blur_scene_after;
	/*
	 * M4F.2A.3 breaks.
	 *
	 * `blur_ignore_darken` drops the clamp, leaving the plain average. On a dark
	 * surface with bright high-frequency detail -- a terminal -- the average is
	 * LIGHTER than what it replaced, which is the glow the clamp exists to stop.
	 * On a flat or a photographic backdrop it changes almost nothing, which is
	 * why the fixture has to be the difficult one.
	 *
	 * `blur_ignore_clip` drops the command's clip for blur commands only, so the
	 * composite covers the whole node instead of the region the client (or the
	 * compositor) restricted it to. A blur node is usually larger than its clip,
	 * so this paints backdrop over pixels that should have been left alone.
	 *
	 * `blur_edge_logical_sigma` divides the soft edge's sigma back out by the
	 * output scale -- restoring the reference's inconsistency, in which a blur
	 * node's edge is scaled to output pixels and the shadow it is drawn to match
	 * is not. At scale 1.0 it is a no-op by construction; at 1.5 the two edges
	 * fade over different distances.
	 */
	bool break_blur_ignore_darken;
	bool break_blur_ignore_clip;
	bool break_blur_edge_logical_sigma;
	/*
	 * M4F.2B breaks.
	 *
	 * `blur_under_damage` removes one mathematically required layer of
	 * expansion: the FORWARD dilation that turns a changed source pixel into
	 * the wider set of result pixels it can reach. Damage still propagates --
	 * the blur is still recomputed, the counters still move -- so nothing but
	 * the picture can catch it, and what it leaves is a ring of stale blur
	 * around every change, which is the classic under-damage artifact.
	 *
	 * THERE IS NO break_blur_no_transitive_damage, AND THAT IS A MEASUREMENT.
	 * One was written -- it stopped a blur's output damage from joining the
	 * prefix damage a later blur reads -- and it could not be made to leave
	 * more than 4 wrong pixels at 1 code in any geometry tried. The reason is
	 * not the implementation: a dual-Kawase blur PRESERVES THE LOCAL MEAN, so
	 * blurring an already-blurred field gives very nearly what blurring the raw
	 * field would. Removing blur 1 from a two-blur scene ENTIRELY, replacing
	 * 120 of the 136 columns of blur 2's source, moved blur 2's output by ONE
	 * CODE. The region a missing transitive edge leaves stale is additionally
	 * at kernel-tail distance in BOTH blurs, so the error is a product of two
	 * decays. The edge is still mathematically required and is asserted
	 * STRUCTURALLY instead, through blur_transitive_damage_pixels below. See
	 * docs/avk-effects.md.
	 *
	 * `blur_full_damage_always` is not a break but the ORACLE: it forces the
	 * complete dependency rebuild and the complete write-region recomposition
	 * that M4F.2A.3 did unconditionally, WITHOUT changing blur semantics and
	 * without touching the scene-prefix architecture. A partial frame that
	 * differs from this by one pixel has a damage bug.
	 */
	bool break_blur_under_damage;
	/*
	 * M4F.2C break. Clamps a blur's source reconstruction to the OUTPUT's own
	 * bounds, which is what a renderer that never thought about a second
	 * monitor does. On a single output it changes nothing at all -- the source
	 * bounds and the presentation bounds are the same box -- and on a window
	 * spanning a seam it replaces the source that lies across the join with the
	 * capture's edge-clamped colour. The result is a visible discontinuity down
	 * the seam, which is the defect the halo exists to prevent.
	 */
	bool break_blur_source_output_clip;
	bool blur_full_damage;
	/* What to divide by under break_blur_edge_logical_sigma. The renderer has no
	 * scale of its own -- geometry arrives already in output pixels -- so the
	 * break is told, exactly as break_rounded_double_scale is. */
	float break_blur_edge_scale;
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
	/*
	 * M4E. What the frame touches and what that implies, made explicit.
	 *
	 * ONE graph on the renderer rather than one per output, because a frame is
	 * built, executed and finished inside avk_render_frame() before the next
	 * output's begins -- there is never a second graph outstanding. Its arrays
	 * are reset, not freed, so a stable scene stops allocating after warmup;
	 * `graph.stats.allocs` is what proves that rather than asserts it.
	 *
	 * Note that a renderer is shared by every output using its VkFormat (see
	 * az_avk_renderer_for), so this is per FORMAT and the per-output claim is
	 * about the graph's CONTENT, which describes exactly one output's frame.
	 */
	struct avk_graph graph;
	/*
	 * M4E.2. Present, collected every frame, and ACQUIRED FROM BY NOTHING YET
	 * -- M4F's blur is the first consumer.
	 *
	 * It lives here rather than waiting for that because the alternative is
	 * shipping the pool at the same moment as the first thing that stresses it,
	 * which is how a lifetime bug and an effect bug arrive together and get
	 * debugged as one. An empty pool costs one branch in
	 * avk_renderer_collect().
	 *
	 * An output resize needs no special handling as a result: entries keyed on
	 * the old extent are simply never asked for again and retire on the idle
	 * path like any other size. avk_transient_pool_drop_all() exists for the
	 * case where that is too slow to wait for.
	 */
	struct avk_transient_pool transients;
	/*
	 * M4F. Each blur command's finished result for this frame, indexed by
	 * COMMAND INDEX so any segment that reaches the command can find it. Reset
	 * per frame; grown, never freed.
	 *
	 * The capture box travels with the image because the result covers the
	 * CAPTURE region, not the write box -- the composite needs both to know
	 * where the write box sits inside it.
	 */
	struct avk_blur_result *blur_results;
	size_t blur_results_cap;
	struct avk_blur_stats blur_stats;
	/*
	 * Cheap counters for what prefix replay costs, before any decision about
	 * caching is taken. A blur at command index k replays k commands, so N
	 * blurs are quadratic in the worst case -- which is exactly the number a
	 * cache would be bought with, and it should be measured rather than
	 * assumed.
	 */
	/*
	 * WHAT THE LAST FRAME ACTUALLY REDREW, in output pixels.
	 *
	 * The damage the caller handed in, unioned with every blur's output damage.
	 * A blur turns a small source change into a wider result change, and the
	 * compositor has to tell the backend about the wider one -- reporting only
	 * what the client damaged leaves a blurred fringe on screen until something
	 * unrelated happens to damage it.
	 *
	 * Valid until the next avk_render_frame() on this renderer. That is exactly
	 * long enough for its one caller, which reads it before rendering anything
	 * else, and it is a deliberate alternative to writing back through the
	 * `const struct avk_scene *` the frame was given.
	 */
	pixman_region32_t frame_damage;
	/*
	 * M4F.2C.4c. Off unless AZ_FRAME_ORACLE=1; see avk_oracle.h. Lives on the
	 * renderer because its taps are graph passes in this renderer's frame and
	 * its reference target must match this renderer's format.
	 */
	struct avk_oracle oracle;
	uint64_t blur_prefix_replays;
	uint64_t blur_prefix_commands;
	uint64_t blur_prefix_pixels;
	/*
	 * M4F.2B. What damage propagation actually bought, as areas rather than as
	 * a belief. Every one of these is a pixel count summed over the run, and
	 * they are stated in pairs so a ratio is available without a second
	 * instrument:
	 *
	 *   source_damage / full_dependency   how much of the source moved
	 *   output_damage / full_write        how much of the result may differ
	 *   prefix_rebuild / full_capture     how much had to be reconstructed
	 *
	 * blur_damage_saved_pixels is the last pair's difference, kept explicitly
	 * so "we saved 0" is a number rather than the absence of one.
	 */
	uint64_t blur_source_damage_pixels;
	uint64_t blur_output_damage_pixels;
	uint64_t blur_prefix_rebuild_pixels;
	uint64_t blur_full_dependency_pixels;
	uint64_t blur_full_write_pixels;
	uint64_t blur_full_capture_pixels;
	uint64_t blur_damage_saved_pixels;
	uint64_t blur_damage_nodes_touched;
	uint64_t blur_damage_nodes_skipped;
	/*
	 * SOURCE DAMAGE ONE BLUR INHERITED FROM ANOTHER -- the forward sweep's one
	 * edge, counted, because it cannot be seen in the picture.
	 *
	 * Exactly the pixels of a blur's source damage that came from an EARLIER
	 * blur's output rather than from the frame's own input damage. Zero in
	 * every single-blur frame and positive whenever one blur's result lies in
	 * another's dependency, so an omission moves it to zero and a test notices
	 * -- which is the only way this edge can be falsified. See the note beside
	 * the breaks above for why a pixel-level falsifier is not available.
	 */
	uint64_t blur_transitive_damage_pixels;
	/* Conservative collapses to a whole region. Explicit and counted, because
	 * a fallback nobody can see becomes the only path. */
	uint64_t blur_damage_fallbacks;
	/* The most rectangles any one blur's rebuild region arrived in this run,
	 * and the CPU cost of both sweeps. Region complexity is the thing that
	 * would justify a tile scheme later; it is measured before anything is
	 * built on the assumption. */
	uint64_t blur_damage_rects_max;
	uint64_t blur_damage_build_ns;
	/*
	 * M4F.2C/.2D accounting, by rectangle arithmetic and never by a per-pixel
	 * loop.
	 *
	 * `blur_halo_pixels`     source area reconstructed OUTSIDE this output's
	 *                        own bounds -- the price of a seamless seam.
	 * `blur_capture_pixels`  the capture extents blur chains actually ran on.
	 * `blur_result_pixels`   the result area anything needed.
	 * `blur_processed_pixels` fragments the down/up chain will process, summed
	 *                        over every level. M4F.2B left the chain running on
	 *                        the full capture while only the prefix replay
	 *                        became regional, so the difference between this
	 *                        and the result area is exactly the work a
	 *                        per-level scissor would save -- which is M4F.2D's
	 *                        decision to take, on this number.
	 */
	uint64_t blur_halo_pixels;
	uint64_t blur_capture_pixels;
	uint64_t blur_result_pixels;
	uint64_t blur_processed_pixels;
	VkFormat format;

	struct avk_renderer_stats stats;

	/* Said once per renderer rather than once per frame: a command asking for
	 * an effect M3 does not implement is worth exactly one warning. */
	bool warned_unimplemented_effect;
};

/*
 * ONE CONTIGUOUS COMMAND INTERVAL, RENDERED INTO ONE TARGET.
 *
 * This is the primitive the whole renderer draws through, and it is general on
 * purpose: the output frame is the case where the range is the whole scene, the
 * target is the scan-out image and the origin is 0,0. A blur's scene prefix is
 * the case where the range is [0, k), the target is a pooled transient and the
 * origin is the dependency region's top-left.
 *
 * THE COORDINATE CONTRACT. Every command's geometry stays in SCENE coordinates
 * and is not rewritten -- a command does not know which target it lands in.
 * Exactly two things translate:
 *
 *     the scissor, here, at the draw
 *     the vertex position, in quad.vert, via AZ_TARGET_ORIGIN
 *
 * and fragment shaders convert back with az_frag_global() wherever they measure
 * against a geometry field. That keeps gradients from restarting and the shadow
 * dither from shifting phase at a regional target's edge -- both of which still
 * look plausible when wrong.
 */
struct avk_render_segment {
	struct avk_renderer *renderer;
	const struct avk_scene *scene;

	struct avk_image *target;
	/* The ATTACHMENT's extent -- not the scene's, and not the allocation's
	 * where a pooled transient is larger. */
	uint32_t width, height;
	VkImageLayout layout;
	/*
	 * Where this target's top-left sits in scene space.
	 *
	 *     target_pixel = scene_pixel - origin
	 *
	 * MUST BE EVEN IN BOTH AXES for bit-exact equivalence with rendering the
	 * same commands into the output, and this is measured rather than assumed:
	 * at origin 37,53 a rounded border and an annulus differ from the reference
	 * in 82 of 12288 pixels by up to 5 codes; at 36,52 the same scene differs in
	 * ZERO.
	 *
	 * WHY. rounded.glsl antialiases with fwidth(dist), which is a 2x2 QUAD
	 * derivative, and the quad grid is aligned to the ATTACHMENT's pixel grid.
	 * An odd origin shifts every derivative quad by one pixel relative to the
	 * output, so an edge's antialiasing band is computed over a different
	 * neighbourhood. Nothing else in the renderer is sensitive to it -- a
	 * gradient, a dithered shadow and a cropped texture all match exactly at an
	 * odd origin, because they read az_frag_global() and nothing else.
	 *
	 * Rounding a capture region's origin down to even costs at most one pixel
	 * per axis. avk_render_segment_align_origin() does it.
	 */
	int32_t origin_x, origin_y;

	/* Half-open, in scene order. Order is SEMANTIC and is never rearranged. */
	size_t begin, end;

	/* Scene-space region this segment may touch. NULL means the whole target.
	 * The output frame passes the frame's damage; a prefix capture passes the
	 * dependency region, or NULL to render all of it. */
	const pixman_region32_t *active;

	/* Draw the scene's background clear first. False for a regional capture
	 * whose base comes from the commands themselves. */
	bool clear;
	/* loadOp LOAD rather than DONT_CARE. True for the output, whose pixels
	 * outside the damage must survive; false for a target this segment writes
	 * whole. */
	bool load;

	/* Set by avk_render_frame() before recording. */
	VkDescriptorSet gradient_set;
};

/*
 * ── WHAT ONE COMMAND CONSUMES ───────────────────────────────────────────────
 *
 * There is exactly ONE place in AVK that answers "which images does this
 * command sample", and everything that needs to know asks it: the barrier
 * declaration for a segment, and any future pass that replays a command range.
 *
 * WHY IT IS A FUNCTION AND NOT A LIST AT EACH CALL SITE. The first version of
 * avk_render_declare_segment() derived its uses inline, and the derivation was
 * `if (cmd->type == AVK_CMD_TEXTURE)`. When AVK_CMD_BLUR arrived it sampled its
 * own finished result, nobody added it to that condition, and the missing use
 * was a missing barrier -- which does not fail, it renders. Validation caught it
 * (VUID-vkCmdDraw-imageLayout-00344); the driver would not have.
 *
 * A second call site would have had to remember the same thing again. So the
 * knowledge moved here, once, and the callers ask.
 *
 * OMISSION IS MADE LOUD, THREE WAYS:
 *
 *   1. A _Static_assert on AVK_CMD_TYPE_COUNT sits directly above the switch, so
 *      adding a command type FAILS THE BUILD at the line that has to learn about
 *      it. The switch also has no `default:`, which makes -Wall's own -Wswitch
 *      fire on the same change -- but a warning is not a guard, and the assert
 *      is there because the warning alone is not enough.
 *   2. Every case must set `declared`. A case that falls through without
 *      deciding returns false and logs, rather than reporting "no resources".
 *   3. NO RESOURCE IS NOT UNKNOWN RESOURCE. A rect and a shadow genuinely
 *      sample nothing, and they say so explicitly, with a comment saying why.
 *      They do not share a code path with "we have never thought about this
 *      command type", because that is the state that must be impossible.
 */

/* Two, not one: a command sampling a second image (a transparency mask, say) is
 * a real future case, and the overflow path logs rather than truncating. */
#define AVK_CMD_MAX_SAMPLED 2

struct avk_cmd_uses {
	struct avk_image *sampled[AVK_CMD_MAX_SAMPLED];
	uint8_t sampled_len;
};

/*
 * Resources a command consumes that do not live in the command itself.
 *
 * A blur's source is its own finished result, which the renderer produced this
 * frame and holds by command index. Passing it in rather than reaching for a
 * renderer keeps avk_cmd_graph_uses() testable with no device.
 */
struct avk_cmd_use_ctx {
	const struct avk_blur_result *blur_results;
	size_t blur_results_len;
};

/*
 * Fill `out` with everything command `index` samples.
 *
 * Returns false ONLY when the command type has no declared use behaviour, which
 * is a bug in this file and is logged as one. An empty `out` with a true return
 * means "this command samples nothing", which is a different statement.
 */
bool avk_cmd_graph_uses(const struct avk_scene *scene, size_t index,
	const struct avk_cmd_use_ctx *ctx, struct avk_cmd_uses *out);

/*
 * Declare `seg` as a graph pass: it writes its target and reads every distinct
 * image the commands in its range sample.
 *
 * `target_resource` must already be declared on the graph, because only the
 * caller knows whether the target arrived from outside (a scan-out buffer) or
 * was produced by a previous pass (a transient). `seg` must outlive
 * avk_graph_execute() -- the graph records the pass later and holds the pointer.
 *
 * Returns false if the pass could not be declared, having logged why.
 */
bool avk_render_declare_segment(struct avk_graph *graph,
	struct avk_render_segment *seg, uint32_t target_resource);

/*
 * Round a capture region's origin DOWN to even in both axes, growing the extent
 * to compensate so the requested area is still covered.
 *
 * See the note on `origin_x` for why: an odd origin misaligns the 2x2 derivative
 * quads that rounded.glsl's antialiasing is computed over. Any caller building a
 * regional target from a scene rectangle should pass it through here rather than
 * rediscovering the constraint.
 */
static inline void avk_render_segment_align_origin(int32_t *x, int32_t *y,
		uint32_t *width, uint32_t *height) {
	if ((*x & 1) != 0) { (*x)--; if (width != NULL) { (*width)++; } }
	if ((*y & 1) != 0) { (*y)--; if (height != NULL) { (*height)++; } }
}

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
