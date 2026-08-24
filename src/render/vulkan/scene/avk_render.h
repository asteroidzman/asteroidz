#ifndef AVK_RENDER_H
#define AVK_RENDER_H

#include "../command/avk_command.h"
#include "../command/avk_retire.h"
#include "../command/avk_stat.h"
#include "../command/avk_timestamp.h"
#include "../graph/avk_graph.h"
#include "../effect/avk_blur.h"
#include "../graph/avk_transient.h"
#include "../pipeline/avk_gradient.h"
#include "../pipeline/avk_output_encode.h"
#include "../pipeline/avk_pipeline.h"
#include "avk_oracle.h"
#include "avk_scene.h"

/*
 * The frame renderer: an avk_scene in, GPU work out.
 *
 * This is the function that exists instead of wlr_renderer_begin_buffer_pass()
 * + wlr_render_pass_add_texture() + wlr_render_pass_submit(). Nothing in here
 * or below it touches a wlr_renderer, a wlr_render_pass or a wlr_texture, and
 * The meson source list is what keeps it
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
 * ── THE PATH-A CEILING KNEE (C7 / F5) ─────────────────────────────────────
 *
 * Where a >1-capable source starts rolling off on an output that has no encode
 * pass. Path B never uses it: there the output pass tone-maps the COMPOSITED
 * value, which is the whole of ADR-008's argument for doing it once at the end.
 *
 * F5 left this number unchosen and said whoever picks it owns a falsifier.
 * 0.75 is picked here, and the reasoning is the whole of it:
 *
 *   - it must be strictly below 1.0, or the curve has no interval to work in
 *     and everything above SDR white clips -- which is the defect F5 exists to
 *     record;
 *   - the interval [knee, 1] is what an unbounded input is compressed into, so
 *     too high a knee crushes highlights into a few codes; a quarter of the
 *     range is enough for the extended-Reinhard tail to stay smooth;
 *   - everything below it is UNTOUCHED, so the lower it goes the more ordinary
 *     picture content it moves. 0.75 leaves three quarters of the range exactly
 *     where ADR-009 promises it.
 *
 * THE TWO PROPERTIES IT OWES:
 *
 *   an SDR source (scale <= 1) is BIT-IDENTICAL with the ceiling reachable and
 *   with it compiled out -- the knee is simply never set for it;
 *
 *   a >1 source on Path A does NOT hard-clip at white: distinct scene values
 *   above the knee must come back as DISTINCT codes below 1.0, which is
 *   precisely what the struck sentence of ADR-007 promised and the identity
 *   curve could not deliver.
 */
#define AVK_PATH_A_CEILING_KNEE 0.75f


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
 * ── THE MONITOR BACKGROUND BLUR RESULT CACHE (M4I) ────────────────────────
 *
 * One finished, output-sized, already-blurred picture of everything below the
 * scene's optimized-blur node -- in asteroidz, the wallpaper and nothing else.
 * It is built when its source changes and reused until its source changes
 * again, and the whole point is that FRAME DAMAGE IS NOT A SOURCE CHANGE. A
 * tag transition damages every pixel of the output while the wallpaper behind
 * it is the same wallpaper, and every version of this renderer before M4I
 * rebuilt the background blur anyway -- once per consuming node, per frame.
 *
 * ── VALIDITY IS AN EQUALITY, NOT A JUDGEMENT ──────────────────────────────
 *
 * The cache is usable exactly when every field below still describes the frame
 * asking for it. There is no "probably unchanged" anywhere in this contract:
 * if the renderer cannot prove validity it rebuilds, because a stale backdrop
 * is a wrong picture that persists until something unrelated disturbs it, and
 * that is the worst failure mode a cache can have.
 *
 *   generation  the compositor's dirty counter at the time this was built.
 *               Monotonic; a boolean cannot survive mark/render/mark/clear.
 *   width/height, origin  the extent it was built at. An output resize, a
 *               scale change and a transform change all land here.
 *   params      the kernel it was built with. A radius or level-count change
 *               produces a different picture from the same source.
 *   format      the VkFormat, which is also (M5) the colour domain. A cache
 *               built in one working space may not be sampled in another.
 *
 * ── LIFETIME ──────────────────────────────────────────────────────────────
 *
 * The image is cross-frame, so it cannot come from the transient pool: a
 * transient is recycled the moment its frame retires, which is precisely the
 * property this must not have. It is owned here and handed to the retire queue
 * on replacement, so a frame still sampling the old image keeps it alive
 * without a CPU wait. `slot` alternates so a rebuild never writes the image an
 * in-flight frame is reading.
 */
/*
 * TWO PICTURES OF THE SAME BACKGROUND, and the second is not a variant -- it is
 * a different image that a different set of nodes needs.
 *
 * PLAIN  blur(background). What a window's own backdrop shows.
 * DARK   min(blur(background), background), per channel. What a SHADOW's
 *        backdrop shows.
 *
 * The clamp exists because a blur is an average, and averaging bright detail
 * over a dark ground raises the mean -- so a shadow's backdrop over a terminal
 * came out LIGHTER than the thing it replaced and read as a glow around every
 * window. Serving a shadow from the plain image would reintroduce that defect
 * exactly, and serving a window from the dark one would darken every backdrop
 * on the desktop. Both are needed or neither node can be cached.
 *
 * They are both pure functions of the background, which is the whole reason
 * this works: with the source fixed, min(blur(bg), bg) is as cacheable as
 * blur(bg) is. On the LIVE path the clamp compares against the node's own
 * reconstructed prefix, which is why it has to be applied at build time here
 * rather than at the composite -- see avk_blur.c, where darken is a
 * VK_BLEND_OP_MIN against a destination that still holds the unblurred source,
 * and is therefore only available when the chain writes back into its own
 * source image.
 */
enum avk_blur_cache_kind {
	AVK_BLUR_CACHE_PLAIN = 0,
	AVK_BLUR_CACHE_DARK,
};
#define AVK_BLUR_CACHE_KINDS 2

static inline const char *avk_blur_cache_kind_name(enum avk_blur_cache_kind k) {
	return k == AVK_BLUR_CACHE_DARK ? "dark" : "plain";
}

struct avk_blur_cache_image {
	/*
	 * ONE image per kind, not two -- and the second one was removed on evidence.
	 *
	 * The reasoning that first produced two slots was: a rebuild must not write
	 * the image the previous frame is still sampling, so alternate. That is a
	 * true statement about an unordered write and a false one about this write.
	 * avk_graph.c persists an image's layout on the avk_image across frames, so
	 * declaring the cache as COLOR_WRITE while it sits in
	 * SHADER_READ_ONLY_OPTIMAL emits a layout transition, and a barrier's first
	 * synchronisation scope covers everything previously submitted to the same
	 * queue -- of which this device has exactly one. The graph was already
	 * ordering it, and the alternation was buying nothing but an
	 * output-resolution image per kind.
	 *
	 */
	struct avk_image *image;
	uint64_t image_bytes;
	bool valid;
	/* THE KERNEL THIS IMAGE WAS BUILT WITH, per kind and not shared.
	 *
	 * A single shared `params` cannot validate two images whose kernels differ
	 * in `darken` -- the check compared the dark image against a stored kernel
	 * stamped from the plain one, found a difference every single frame, and
	 * rebuilt: 383 output-sized rebuilds in 13 seconds of an idle desktop,
	 * reported honestly as PARAMS by the reason table. The fix is to stop
	 * having one field describe two images. */
	struct avk_blur_params params;
	/*
	 * ── AND THE SOURCE THIS IMAGE WAS BUILT FROM, ALSO PER KIND ───────────
	 *
	 * The same argument as `params` above, and it was left half-applied. These
	 * describe the SOURCE, which really is one thing shared by both images --
	 * but "what the source is now" and "what THIS image was built from" are
	 * different facts, and only the second one can validate an image.
	 *
	 * The two kinds are built INDEPENDENTLY: a kind is only rebuilt when
	 * something asked for it that frame (`want[k] > 0`, which is gated on the
	 * frame damage reaching a consumer of that kind), so an ordinary frame
	 * rebuilds one and leaves the other alone. With one shared record, the
	 * kind that rebuilt stamped the NEW identity over the record the untouched
	 * kind is validated against -- and the untouched kind, still holding the
	 * previous wallpaper, then compared equal on every field and was served as
	 * a hit for the rest of the session.
	 *
	 * That is the "blurred backdrop of a wallpaper several rotations old"
	 * defect returning by a second road, after `source_hash` closed the first
	 * one. A digest of the source cannot help if it is stamped on behalf of an
	 * image that was never rebuilt from that source.
	 *
	 * `generation` and `source_hash` are the two halves of the source identity
	 * (a notification and a fact -- see the comment on avk_blur_cache); the
	 * extent, origin and format are the target's shape, which the consumer also
	 * reads back as the capture box, so a stale image must not be sampled with
	 * a fresh box either.
	 */
	uint64_t generation;
	uint64_t source_hash;
	int32_t origin_x, origin_y;
	uint32_t width, height;
	VkFormat format;
	/* Built only where something asked for it. A desktop with no shadows never
	 * allocates the dark image, and one with no window backdrops never
	 * allocates the plain one -- which matters, because each is a full
	 * output-sized image and DP-1's is 33MB. */
	bool wanted;
};

struct avk_blur_cache {
	struct avk_blur_cache_image img[AVK_BLUR_CACHE_KINDS];

	uint64_t generation;
	/*
	 * ── WHAT THE CACHED PICTURE WAS ACTUALLY BUILT FROM ───────────────────
	 *
	 * A digest of the prefix commands: for each one its type, its destination
	 * box, its opacity, and -- for a texture -- the image it samples and the
	 * source rectangle it samples from. That is everything that decides the
	 * background's pixels.
	 *
	 * `generation` alone was the validity rule and it was not sufficient. It
	 * counts an EDGE (wlr_scene_optimized_blur.dirty) observed once per output
	 * per frame, so it is only as good as the claim that every way the
	 * background can change funnels through wlr_scene_optimized_blur_mark_dirty
	 * -- and on this desktop it demonstrably did not: a live capture showed the
	 * blurred backdrop rendering a photograph that had not been the wallpaper
	 * for several rotations, while the unblurred wallpaper beside it was
	 * correct and the generation had not moved.
	 *
	 * This does not replace the generation and does not try to find the missing
	 * producer. It makes the rule state a fact about the SOURCE rather than a
	 * fact about the notification, so a background that changed without telling
	 * anyone still cannot be served from here. It can only ever cause a rebuild
	 * -- it is an additional disagreement, never a way to agree -- so the worst
	 * case of a hash that moves too eagerly is work, not a wrong picture.
	 *
	 * Cost is one pass over the prefix per frame. The prefix is the background:
	 * a wallpaper and, at most, a handful of nodes beneath the blur layer.
	 */
	uint64_t source_hash;
	int32_t origin_x, origin_y;
	uint32_t width, height;
	VkFormat format;
	/*
	 * ── EVERYTHING ABOVE IS TELEMETRY, NOT VALIDITY ───────────────────────
	 *
	 * It records what the LAST rebuild of any kind was built from, which is
	 * what a trace line and avk-stats want to print. Nothing validates against
	 * it: an image is validated against its OWN copy in avk_blur_cache_image,
	 * because the two kinds rebuild independently and a shared record is
	 * stamped by whichever kind happened to rebuild. See the comment there --
	 * validating against this is how a stale backdrop came to be certified
	 * valid forever. `params` was moved per-kind for the same reason and this
	 * is the rest of that move.
	 */

	/* Telemetry. Requests are consumer asks; hits are asks served from here;
	 * rebuilds are producer runs. saved_* is the work a hit avoided, priced in
	 * the same units the uncached path reports, so the two are comparable. */
	uint64_t requests, hits, rebuilds, invalidations;
	uint64_t saved_prefix_draws, saved_prefix_px;
	uint64_t saved_chains, saved_blur_px;
	uint64_t bytes;
	/* Split by kind, so "the shadow backdrops are being served" is a reading
	 * rather than a deduction from the total. */
	uint64_t hits_by_kind[AVK_BLUR_CACHE_KINDS];
	uint64_t rebuilds_by_kind[AVK_BLUR_CACHE_KINDS];
	/* Why the last rebuild happened, and how many of each kind there have
	 * been. A cache that rebuilds unexpectedly must say why in one reading. */
	uint64_t inv_generation, inv_geometry, inv_params, inv_format,
	         inv_never_built, inv_forced, inv_source;
};

enum avk_blur_cache_reason {
	AVK_BLUR_CACHE_OK = 0,
	AVK_BLUR_CACHE_NEVER_BUILT,
	AVK_BLUR_CACHE_GENERATION,
	AVK_BLUR_CACHE_SOURCE,
	AVK_BLUR_CACHE_GEOMETRY,
	AVK_BLUR_CACHE_PARAMS,
	AVK_BLUR_CACHE_FORMAT,
	AVK_BLUR_CACHE_FORCED,
};

static inline const char *avk_blur_cache_reason_name(
		enum avk_blur_cache_reason r) {
	switch (r) {
	case AVK_BLUR_CACHE_OK:          return "OK";
	case AVK_BLUR_CACHE_NEVER_BUILT: return "NEVER_BUILT";
	case AVK_BLUR_CACHE_GENERATION:  return "GENERATION";
	case AVK_BLUR_CACHE_SOURCE:      return "SOURCE";
	case AVK_BLUR_CACHE_GEOMETRY:    return "GEOMETRY";
	case AVK_BLUR_CACHE_PARAMS:      return "PARAMS";
	case AVK_BLUR_CACHE_FORMAT:      return "FORMAT";
	case AVK_BLUR_CACHE_FORCED:      return "FORCED";
	}
	return "?";
}

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

/* The primitive classes the draw ledger and the px_* counters are bucketed by.
 * One enumeration for both, so a class cannot be counted under one name and
 * reported under another. */
enum avk_prim_class {
	AVK_PRIM_CLEAR = 1u << 0,
	AVK_PRIM_CONTENT = 1u << 1,
	AVK_PRIM_SHADOW = 1u << 2,
	AVK_PRIM_BORDER = 1u << 3,
	AVK_PRIM_BLUR = 1u << 4,
	AVK_PRIM_GRADIENT = 1u << 5,
	AVK_PRIM_RECT = 1u << 6,
	AVK_PRIM_ROUND = 1u << 7,   /* not a primitive: the coverage evaluation */
};

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
	 * P2. Textured quads drawn through quad_free.vert. Counted at the DRAW so
	 * a fixture can tell "the free-corner pipeline ran" from "a quad command
	 * was recorded" -- a quad whose image failed to resolve is skipped, and
	 * the two claims are different.
	 */
	uint64_t quads;
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
	/* M5/C7. Draws that took a decode variant. Zero with decode off, and zero
	 * WITH it on would mean no source asked for one -- two different readings
	 * that a pixel comparison alone conflates. */
	uint64_t decode_draws;
	/* M5/C7. Segments that rendered into an _SRGB attachment view. Counted
	 * because "the encode is on" has so far been INFERRED from the size of a
	 * pixel error, and an inference is what a counter is for. */
	uint64_t srgb_attach_segments;
	/* M5/C7. Decode draws BY VARIANT. A total says the decode ran; only the
	 * breakdown says WHICH curve a source was decoded with, and a source
	 * decoded with the wrong one of two similar curves is off by about a code
	 * -- which is indistinguishable from rounding until it is counted. */
	uint64_t decode_by_variant[AVK_DECODE_COUNT];
	/*
	 * M5/C6 (Path B). The encode pass, counted at the draw and in fragments.
	 *
	 * `encode_draws` is damage rectangles, `encode_px` their area. Both zero on
	 * Path A, which is the point: "Path B ran" and "Path B ran and encoded
	 * nothing" are different statements, and a pixel comparison alone conflates
	 * them with "Path B produced the same picture".
	 */
	uint64_t encode_draws;
	uint64_t encode_px;
	uint64_t draws;          /* commands x damage rects */
	/*
	 * M4H. FRAGMENT AREA, BY PRIMITIVE CLASS, IN TWO BUCKETS.
	 *
	 * Draw COUNTS cannot tell an expensive frame from a cheap one: one shadow
	 * over a 4K envelope and one shadow over a 200x100 window are both
	 * `shadow_draws = 1`. These are the sum of the SCISSOR areas actually
	 * submitted -- clip, interior cut-out and frame damage all already applied
	 * -- so they are what the rasteriser was asked to shade, not what the scene
	 * asked for.
	 *
	 * TWO BUCKETS BECAUSE A DECORATION IS RASTERISED TWICE. The output pass
	 * composites the desktop; every blur chain first replays the scene prefix
	 * behind its node into a capture target, through this same function. A
	 * shadow that lies behind two blur nodes is shaded three times, and a
	 * single total would report that as one large number with no way to see
	 * which half of it is the blur's. `prefix` is every regional capture,
	 * `out` is the output segment, and the discriminator is the segment's own
	 * `load` flag -- the output preserves what it does not damage, a capture
	 * target is written whole.
	 *
	 * The pair `shadow` / `shadow_env` is the point of the shadow fields: the
	 * envelope is what a naive implementation would shade, the first is what
	 * this one does, and their ratio is the answer to "is the shadow region
	 * oversized". Same for `border` against `border_outer`.
	 *
	 * `target` is the attachment area, summed once per segment, so it is the
	 * denominator for that bucket and nothing else.
	 */
	struct avk_prim_px {
		uint64_t clear;        /* the background rect */
		uint64_t content;      /* client textures */
		uint64_t shadow;       /* scissored: envelope minus the caster */
		uint64_t shadow_env;   /* the same shadows' full envelopes */
		uint64_t border;       /* border annulus, scissored */
		uint64_t border_outer; /* the same borders' full outer rects */
		uint64_t blur_comp;    /* blur result composited back */
		uint64_t rect;         /* solid rects that are not borders */
		uint64_t gradient;     /* of which shaded by the gradient pipeline */
		uint64_t target;       /* attachment area, once per segment */
	} px_out, px_prefix;
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
	/*
	 * The same quantity as a DISTRIBUTION. A mean over a run hides exactly the
	 * frames that miss a vblank, and M4F.2D asks what a blur costs at p95 and
	 * p99, not on average. Fed once per frame with that frame's own record
	 * time; see avk_stat.h for the bucketing and what a percentile off it does
	 * and does not claim.
	 */
	struct avk_hist record_hist;
	/*
	 * WAITING, kept apart from working.
	 *
	 * avk_cmd_ring_begin() blocks when the CPU has run three frames ahead of
	 * the GPU. That is backpressure, not recording cost, and it used to be
	 * inside cpu_record_ns -- where it produced samples past a 40 ms ceiling
	 * on an almost idle fixture. cpu_sync_waits counts the stalls; these
	 * measure them.
	 */
	uint64_t cpu_ring_wait_ns;
	struct avk_hist ring_wait_hist;
};

struct avk_renderer {
	/* M4A break switches; see avk_render.c. Read once at init, never in the
	 * draw loop, so a break costs nothing when it is off. */
	/*
	 * P2. Swaps two of a textured quad's four destination corners at record
	 * time, which folds the quad into a bow tie: the same four points, wound
	 * wrongly. Both P2 gates must go red under it -- the degenerate quad stops
	 * matching the texture command it should equal, and the rotated quad stops
	 * matching the transform-enum render it should equal.
	 */
	/*
	 * M4D break. Restores a single-radius shadow -- every corner taking the
	 * top-left's -- which is what an implementation that carried one scalar
	 * radius produces. It renders plausibly on any window whose corners
	 * match, which is most of them, and is wrong on every titlebar-joined
	 * one.
	 */
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
	/*
	 * M4D.4 break. Turns the dither off, restoring the banded shadow the live
	 * session showed: a smooth falloff quantised to nine 8-bit levels reads as
	 * concentric halos on a flat backdrop.
	 */
	/* AZ_AVK_CMD_DUMP=N: log the draw ledger for the first N segments, then
	 * stop for good. `dump_seg` is the running segment number. */
	uint32_t cmd_dump;
	uint32_t dump_seg;
	/*
	 * M5/C7. Select a decode variant from each source's luminance domain.
	 *
	 * OFF BY DEFAULT, and it must stay off until the ENCODE side exists.
	 * Decoding without encoding moves composition into linear light and then
	 * writes those values as though they were still electrical -- every window
	 * on screen washes out. The two halves are one change and the M5 SDR gate
	 * is what says so: it is bit-exact with both off, must FAIL with only this
	 * on, and must return to within a code once the encode lands.
	 */
	bool decode_enabled;
	/*
	 * M5/C7 (Path A). Render into the target's _SRGB view, so the hardware
	 * applies the inverse sRGB EOTF on every write.
	 *
	 * The other half of decode_enabled, and the two are only correct together:
	 * decode alone composites in linear and writes it as though it were
	 * electrical (everything washes out), encode alone applies an inverse curve
	 * to values that were never decoded (everything darkens). The M5 SDR gate
	 * is bit-exact with both off and must be within a code with both on.
	 *
	 * Silently inert when the target has no _SRGB view -- see
	 * avk_image_srgb_view(). That is the honest fallback: an output whose
	 * scanout buffer is 10-bit has no such view on any conformant device (F11)
	 * and belongs on Path B.
	 */
	bool encode_srgb;
	/*
	 * ── PATH A NEEDS ITS OWN PIPELINES, AND THAT IS NOT AN OPTIMISATION ───
	 *
	 * Dynamic rendering bakes the colour-attachment format into the pipeline,
	 * and the spec requires the format the pipeline declares to MATCH the view
	 * the render-pass instance attaches:
	 *
	 *   VUID-vkCmdDraw-dynamicRenderingUnusedAttachments-08910
	 *
	 * Path A attaches the target's _SRGB view to pipelines built for the UNORM
	 * twin, which violates exactly that -- undefined behaviour that RADV
	 * happens to execute correctly, so it produced a right picture and a clean
	 * A/B while being invalid. It went unnoticed because the compositor
	 * fixtures assert `validation_errors == 0` WITHOUT loading the validation
	 * layer, which is a counter that cannot move; the on-GPU unit fixture under
	 * ASTEROIDZ_VK_DEBUG reported it 20 times on the first run.
	 *
	 * So Path A's output segment draws with a second pipeline set declaring the
	 * _SRGB format. Built lazily, on the first frame that takes the fast path:
	 * a desktop with no Path-A output never pays for it.
	 *
	 * ONLY THE PIPELINES DIFFER. The descriptor sets, samplers and pools still
	 * come from this set through the same per-image cache, because the two
	 * pipeline layouts are created from identically defined set layouts and
	 * identical push-constant ranges -- which is precisely the spec's
	 * definition of layout compatibility. Duplicating the descriptor cache to
	 * avoid using it would double every surface's descriptors for nothing.
	 */
	struct avk_pipelines pipes_srgb;
	bool pipes_srgb_ok;
	VkFormat pipes_srgb_format;
	/*
	 * ── M5/C6 (PATH B). THE OUTPUT-ENCODE PASS ────────────────────────────
	 *
	 * Lent per frame by the caller, exactly like `blur_cache` above and for
	 * exactly the same reason: a renderer is shared by every output using its
	 * VkFormat, and both the intermediate and the encode constants belong to ONE
	 * output. Leaving them attached would encode DP-1's frame with HDMI-A-1's
	 * curve on whichever rendered second.
	 *
	 * When `encode_intermediate` is set, the scene composites into IT and this
	 * pass writes the frame's `target`. The renderer's own `format` is then the
	 * INTERMEDIATE's format (scene-linear FP16), which is what makes the blur
	 * transients and the M4I cache follow the path without a line of their own
	 * -- ADR-012 falls out of the existing per-format renderer selection.
	 *
	 * `encode_full_frame` forces a whole-output frame. Required exactly once
	 * per intermediate: a freshly created image holds nothing, and the frame
	 * damage handed in describes the age of the SCANOUT buffer, which rotates
	 * through several while the intermediate does not.
	 */
	struct avk_image *encode_intermediate;
	struct avk_encode_params encode_params;
	bool encode_full_frame;
	/*
	 * The encode pipelines, per (target format, curve). On the RENDERER because
	 * that is where the pipeline layout's descriptor set layout lives -- the
	 * intermediate is sampled through avk_pipelines' texture set, so its
	 * descriptor is cached on the image like every other sampled image and the
	 * frame path allocates none.
	 */
	struct avk_output_encode encode;
	/* Per-segment draw regions, computed top-down. Grown, never freed per
	 * frame; see avk_render_reserve_regions(). */
	pixman_region32_t *region_scratch;
	size_t region_scratch_len;
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
	 * A MISSING TRANSITIVE EDGE IS NEARLY INVISIBLE, AND THAT IS A MEASUREMENT.
	 * Stopping a blur's output damage from joining the prefix damage a later
	 * blur reads could not be made to leave more than 4 wrong pixels at 1 code
	 * in any geometry tried. The reason is
	 * not the implementation: a dual-Kawase blur PRESERVES THE LOCAL MEAN, so
	 * blurring an already-blurred field gives very nearly what blurring the raw
	 * field would. Removing blur 1 from a two-blur scene ENTIRELY, replacing
	 * 120 of the 136 columns of blur 2's source, moved blur 2's output by ONE
	 * CODE. The region a missing transitive edge leaves stale is additionally
	 * at kernel-tail distance in BOTH blurs, so the error is a product of two
	 * decays. The edge is still mathematically required and is asserted
	 * STRUCTURALLY instead, through blur_transitive_damage_pixels below. See
	 * docs/architecture.md.
	 *
	 * `blur_full_damage_always` is not a break but the ORACLE: it forces the
	 * complete dependency rebuild and the complete write-region recomposition
	 * that M4F.2A.3 did unconditionally, WITHOUT changing blur semantics and
	 * without touching the scene-prefix architecture. A partial frame that
	 * differs from this by one pixel has a damage bug.
	 */
	bool blur_full_damage;
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
	/*
	 * M4H.6. Of blur_prefix_rebuild_pixels, how many lie inside the frame's
	 * arriving damage -- which is exactly the set a copy from the output
	 * attachment could serve, because that is where the attachment is current
	 * rather than holding the previous frame. The quotient of the two is the
	 * premise for replacing prefix replay with a copy, and it has to be
	 * measured on the SLOW frames rather than assumed from the architecture.
	 */
	uint64_t blur_prefix_copyable_pixels;
	/* The bounding-box collapse, priced. rects is the fragment count at the
	 * moment of collapse; before/after are the region's area and its bounding
	 * box's, so after-before is the fill the collapse invented. */
	uint64_t blur_fallback_rects;
	uint64_t blur_fallback_area_before;
	uint64_t blur_fallback_area_after;
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
	/*
	 * M4I. THE SAME QUANTITIES, SPLIT BY ROLE (enum avk_blur_role).
	 *
	 * Every aggregate above sums a tooltip's backdrop and a maximised window's
	 * into one mean, and that mean is what produced "chains=3 costs more than
	 * chains=4" -- a true statement about the aggregate and a useless one about
	 * the renderer. Cost belongs to a KIND of chain, so it is counted per kind.
	 *
	 * Indexed by enum avk_blur_role. A role's entry moving while the total does
	 * not is the shape of every attribution question left in this milestone.
	 */
	uint64_t blur_role_chains[AVK_BLUR_ROLE_COUNT];
	uint64_t blur_role_capture_px[AVK_BLUR_ROLE_COUNT];
	uint64_t blur_role_rebuild_px[AVK_BLUR_ROLE_COUNT];
	uint64_t blur_role_prefix_cmds[AVK_BLUR_ROLE_COUNT];
	uint64_t blur_role_result_px[AVK_BLUR_ROLE_COUNT];
	/* Per-chain geometry + role, one log line each, under
	 * AZ_BLUR_CHAIN_TRACE=1 or `amsg dispatch set_blur_chain_trace,1`. Runtime
	 * toggle because the interesting frames are live ones and a restart to
	 * enable a trace changes the workload being traced. */
	uint32_t blur_chain_trace;
	/*
	 * M4I. The OUTPUT's monitor background blur cache, borrowed for the frame.
	 *
	 * A pointer and not a member: a renderer is shared by every output using
	 * the same VkFormat, and this resource is per output. The caller sets it
	 * before recording and it is read only during recording, so it never
	 * becomes mutable state consulted after the snapshot.
	 */
	struct avk_blur_cache *blur_cache;
	/* The producer segments' active regions, owned for the frame. Per kind:
	 * both may be rebuilt in one frame, and one shared region would be reset
	 * under the first segment's feet while the graph still held the pointer. */
	pixman_region32_t blur_cache_region[AVK_BLUR_CACHE_KINDS];
	/*
	 * ── FALSIFIERS ────────────────────────────────────────────────────────
	 *
	 * Each one must make a specific oracle FAIL. A break that leaves every
	 * assertion green is a break that proves the assertion tests nothing, and
	 * two of those were found doing exactly that in M4H.
	 *
	 * off            AZ_BLUR_CACHE=0. The A/B arm: same binary, no cache, the
	 *                pre-M4I path in full.
	 * always_dirty   rebuild every frame. Hit rate must go to zero and the
	 *                frame cost must return to the uncached baseline. If the
	 *                cache still claims a win with this on, the attribution is
	 *                wrong, not the cache.
	 * ignore_dirty   never invalidate on generation. Changing the wallpaper
	 *                must then leave stale pixels and fail the dirty oracle.
	 * stale_geometry ignore an extent change. A resize must fail the oracle.
	 * stale_params   ignore a kernel change. A radius change must fail it.
	 *
	 */
	bool break_blur_cache_off;
	/*
	 * breaks above hide an invalidation the check already found; this one
	 * arranges the ORDINARY frame in which a kind is simply not asked for, so
	 * that the rule about what the other kind's rebuild does to it can be
	 * stated at all. See avk_render_set_blur_cache_starve().
	 */
	/*
	 * WHAT THE CHAIN WOULD HAVE TO PROCESS, summed per pass, with every
	 * filter footprint included -- avk_blur_work_of(). The denominator of the
	 * per-level scissor question, and deliberately NOT the composite's read
	 * region: a texel shaded at level 0 and a texel shaded at level 1 are
	 * separate GPU work, and every level must produce enough for the next
	 * pass to sample.
	 *
	 * Rectangle arithmetic, so a multi-rectangle result region is treated as
	 * its bounding box: `required` comes out too LARGE and the amplification
	 * therefore too SMALL, which is the safe direction for a decision about
	 * whether to optimise.
	 */
	uint64_t blur_required_work_pixels;
	/*
	 * ── WHAT EACH CANDIDATE STRATEGY WOULD REMOVE ─────────────────────────
	 *
	 * M4F.2D asks which is the SIMPLEST implementation that captures most of
	 * the opportunity, so the opportunity is accumulated per candidate rather
	 * than as one global figure. Each is (actual - required) summed over the
	 * passes that strategy would scissor:
	 *
	 *   up0        the final full-resolution upsample only
	 *   up01       that plus the one below it
	 *   up_chain   every upsample
	 *   (all)      every pass = processed - required, already derivable
	 *
	 * Pixel work only. None of these is a GPU time estimate; pass overhead,
	 * raster setup, barriers and bandwidth are not proportional to shaded
	 * texels.
	 */
	/* The most chains any single frame declared. The up-phase and final-
	 * upsample timestamp spans are only attributable when this is 1. */
	uint64_t blur_max_slots;
	uint64_t blur_removable_up0_pixels;
	uint64_t blur_removable_up01_pixels;
	uint64_t blur_removable_up_pixels;
	/*
	 * WHAT DERIVING ALL OF THAT COSTS ON THE CPU, per frame, as one batch --
	 * not per rectangle operation. Even a simple scissor is not free, and an
	 * estimator that costs more than the GPU work it removes is not an
	 * optimisation.
	 */
	struct avk_hist blur_region_build_hist;
	VkFormat format;

	struct avk_renderer_stats stats;

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
/* M4H diagnostic: override the blur damage rectangle cap for every renderer,
 * from now on. 0 restores AZ_BLUR_DAMAGE_MAX_RECTS or the built-in default.
 * Runtime rather than env because the A/B has to run without restarting the
 * session that produces the workload. */
void avk_render_set_damage_rect_cap(int cap);
/* M4I. Enable or disable the monitor background blur cache at runtime, on one
 * renderer. See the note at the definition for why this is not env-only. */
void avk_render_set_blur_cache_enabled(struct avk_renderer *renderer, bool on);
uint64_t avk_render_frame(struct avk_renderer *renderer,
	struct avk_image *target, const struct avk_scene *scene,
	const VkSemaphoreSubmitInfo *wait, uint32_t wait_count,
	const VkSemaphoreSubmitInfo *signal, uint32_t signal_count);

/*
 * ── WHICH TEXELS OF EACH IMAGE THIS FRAME WILL ACTUALLY SAMPLE ──────────────
 *
 * The same top-down opaque accumulation the draw uses, run over the whole
 * command list and reported instead of drawn. The compositor asks so it can
 * stop COPYING what nothing samples: a client's decoration frame can be tens of
 * megabytes of wl_shm with an opaque subsurface over all but its border, and
 * the copy of the covered part is pure cost.
 *
 * DIFFERENCES FROM THE DRAW'S OWN PASS, each deliberate:
 *
 *   no damage clip   the question is "can this ever be sampled this frame",
 *                    not "does it need redrawing"; a command outside the
 *                    damage still holds pixels the next frame may show.
 *   no segments      a blur replays the prefix behind it, so everything below
 *                    a blur is drawn again into its capture and cannot be
 *                    treated as covered. The accumulator is CLEARED at each
 *                    blur going down, which over-reports visibility -- the
 *                    only safe direction, since under-reporting means not
 *                    copying a pixel that is then shown.
 *
 * Reports AVK_CMD_TEXTURE and AVK_CMD_TEXTURE_QUAD commands with a non-empty
 * region, in the same coordinates as cmd->dst.
 */
typedef void (*avk_visibility_fn)(void *user, const struct avk_cmd *cmd,
	const pixman_region32_t *visible);

void avk_scene_visibility(const struct avk_renderer *renderer,
	const struct avk_scene *scene, const struct avk_box *bounds,
	avk_visibility_fn fn, void *user);

/* Retire whatever the GPU has finished with. Once per frame; never blocks. */
void avk_renderer_collect(struct avk_renderer *renderer);

void avk_renderer_log_stats(const struct avk_renderer *renderer);

#endif /* AVK_RENDER_H */
