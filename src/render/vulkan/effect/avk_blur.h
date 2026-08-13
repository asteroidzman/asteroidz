#ifndef AVK_BLUR_H
#define AVK_BLUR_H

#include "../command/avk_timestamp.h"
#include "../graph/avk_graph.h"
#include "../graph/avk_transient.h"
#include "../pipeline/avk_pipeline.h"

/*
 * Dual-Kawase blur, expressed as graph passes over pooled transients.
 *
 * WHAT THIS FILE OWNS: how many levels, how big each one is, which transient
 * each pass reads and writes, and the kernel step at each level.
 *
 * WHAT IT DOES NOT OWN, and must never acquire: synchronisation. There is no
 * vkCmdPipelineBarrier2 in this file, no layout transition helper, and no
 * wait of any kind. It declares `avk_graph_use()` and the barrier compiler
 * derives the rest -- which is the whole point of M4E and the reason a blur
 * with eight passes needs no more synchronisation knowledge than a blur with
 * two.
 *
 * THE SHAPE, for `levels = 2`:
 *
 *     source  (full res)
 *        | down                       BARRIER color-write -> sampled-read
 *     L1  (1/2)
 *        | down                       BARRIER
 *     L2  (1/4)                       <- the smallest level
 *        | up                         BARRIER
 *     L1' (1/2)
 *        | up                         BARRIER
 *     result (full res)
 *
 * 2N draws for N levels, and the fragment count is
 * 1/4 + 1/16 + ... + 1/4 + 1 ~= 1.7x one full-resolution pass -- because each
 * level is a quarter of the one above it. That is the reason the radius is
 * bought with resolution rather than with taps: doubling the support costs a
 * quarter as much each time instead of twice as much.
 *
 * NO INTERMEDIATE IS CLEARED. Every level is fully written by the pass that
 * produces it, using a REPLACING pipeline, so a clear would be a full-target
 * write thrown away. That is also why the pool handing back a transient with
 * another window's blur in it is harmless: nothing reads a level before its
 * producing pass has written every pixel of it.
 */

/* Levels the config may ask for. Beyond this the smallest level is a handful
 * of pixels and further passes buy no visible radius, only draws. */
#define AVK_BLUR_MAX_LEVELS 6

struct avk_blur_params {
	/* Dual-Kawase level count -- the compositor's `passes`. 0 disables. */
	uint32_t levels;
	/* The reference's `radius`: a multiplier on the half-texel sampling step,
	 * NOT a pixel count. Kept under that name because asteroidz's config is
	 * expressed in it and reinterpreting it would change every existing
	 * desktop. */
	float radius;

	/* Folded into the final upsample; see blur.glsl. */
	float brightness, contrast, saturation, noise;
	bool apply_effects;

	/*
	 * THE DARKEN CLAMP: the result may never come out LIGHTER than the source it
	 * replaced, per channel.
	 *
	 *     out.rgb = min(blurred.rgb, source.rgb)      out.a = blurred.a
	 *
	 * WHY IT EXISTS, and it is not polish. A blur is an average, and averaging
	 * bright detail over a dark ground RAISES the mean wherever the ground is
	 * dark. A terminal is mostly dark with sparse bright text, so its blur comes
	 * out lighter than it was and the shadow that shows it reads as a GLOW. A
	 * photograph is already smooth and barely moves, which is why this only ever
	 * appeared over windows -- and why a flat-colour fixture cannot see it at
	 * all: the blur of a flat field is the same flat field.
	 *
	 * WHERE IT IS APPLIED. The LAST upsample only, after the post-effects, and
	 * against the chain's own level 0 -- which still holds the unblurred source
	 * at that moment, because the chain walks away from it and only returns on
	 * that pass. So the clamp costs one attachment read and no memory.
	 *
	 * HOW. As blend state, not in the shader: a fragment shader cannot read the
	 * attachment it writes. See AZ_BLEND_DARKEN in avk_pipeline.c.
	 *
	 * DIVERGENCE FROM THE REFERENCE, stated once here and argued in
	 * docs/avk-effects.md. blur2.comp skips the clamp inside the sample_exclude
	 * box, because there its "source" is a synthetic stretched-and-mirrored fill
	 * rather than real backdrop and min() against a fabrication preserves the
	 * fabrication's hard structure. AVK's source is the current-frame scene
	 * prefix at every pixel of the capture -- there is no fill and no region
	 * where the premise fails -- so the clamp applies throughout.
	 */
	bool darken;
};

struct avk_blur_stats {
	uint64_t chains;        /* blur chains built */
	uint64_t passes;        /* down + up passes declared */
	uint64_t transients;    /* transient acquires made for blur levels */
	uint64_t skipped;       /* chains declined -- too small, or 0 levels */
	/* Fragments every declared pass will process, summed over the chain. The
	 * chain still runs on the whole capture while only the prefix replay is
	 * regional, so this against the result area is the size of the prize a
	 * per-level scissor would win -- measured before it is built, in M4F.2D. */
	uint64_t processed_pixels;
	/* Of those, the ones shaded by the FINAL full-resolution upsample. The
	 * up0 scissor prototype can only change this one, so it is the numerator
	 * the gpu_blur_up0 span has to be read against. */
	uint64_t up0_pixels;
};

/*
 * Declare a blur of `src` into `dst` on `graph`.
 *
 * `src` must already be declared on the graph (the caller knows whether it is
 * an external import or something a previous pass wrote); `dst` likewise. Both
 * are resource indices, not images, so this cannot accidentally introduce a
 * second state tracker for an image the graph already knows.
 *
 * `width`/`height` are the region being blurred, in `src` pixels. The chain's
 * levels are derived from them.
 *
 * Returns false if the chain could not be built -- no transient, no room in
 * the graph -- having logged why. A false return means the caller must not
 * draw the result, not that it may draw something else.
 */
/*
 * OPTIONAL GPU TIMING MARKS for one chain.
 *
 * `down_end` is written after the chain's LAST downsample pass and `up_end`
 * after its final upsample. Both may be AVK_TS_NONE, which is what every chain
 * but the first passes: with N chains the stream is prefix0, chain0, prefix1,
 * chain1... so "all the downsamples" is not a contiguous range and cannot be
 * one timestamp pair. See avk_timestamp.h.
 */
/* Whether the up0-only scissor prototype is enabled (AZ_BLUR_UP0_SCISSOR=1).
 * Exposed so the renderer can refuse the production-baseline switch while it
 * is on: a scissor without its derived region is not a baseline. */
bool blur_up0_scissor_on(void);

struct avk_blur_marks {
	enum avk_ts_mark down_end;
	enum avk_ts_mark up_end;
	/* End of the PENULTIMATE upsample, so the caller can bracket the final
	 * full-resolution one on its own. AVK_TS_NONE when not wanted, and
	 * ignored on a chain with fewer than two levels, where there is no
	 * penultimate pass to end. */
	enum avk_ts_mark up_penult_end;
};

/* Declared in full further down; the falsifier hook takes a pointer to it. */
struct avk_blur_work;

bool avk_blur_declare(struct avk_graph *graph, struct avk_transient_pool *pool,
	struct avk_pipelines *pipes, struct avk_blur_stats *stats,
	uint32_t src_resource, uint32_t dst_resource,
	uint32_t width, uint32_t height, VkFormat format,
	const struct avk_blur_params *params, const struct avk_blur_marks *marks,
	const struct avk_blur_work *work);

/*
 * HOW FAR A BLUR'S INFLUENCE REACHES, in source pixels, per edge.
 *
 * Four numbers rather than one radius. Dual-Kawase is symmetric so all four are
 * currently equal, and the type exists anyway: an asymmetric effect -- a
 * directional blur, a drop shadow's own offset kernel -- must be expressible
 * without redesigning every damage path that consumes this.
 */
struct avk_blur_support {
	float left, right, top, bottom;
};

/*
 * The maximum finite support of the actual sampling chain, DERIVED.
 *
 * Damage correctness is BINARY: if a source texel can mathematically contribute
 * to an output texel, it belongs in the region. So this is not a threshold on
 * where the contribution becomes visually negligible, and it is not fitted to
 * an observed transition width -- a dual-Kawase chain has a finite footprint
 * and it can be computed exactly. (A true Gaussian would need a stated cutoff
 * policy instead; this does not.)
 *
 * THE DERIVATION, per axis, in texels of the level being sampled.
 *
 *   DOWNSAMPLE, level i-1 -> level i. The 5-tap kernel offsets by
 *   `step = 0.5/allocation * radius` in UV, which is `0.5 * radius` texels of
 *   the source level. Each tap is BILINEAR, and a bilinear fetch at position p
 *   draws on texel centres within [p-1, p+1] -- so one more texel each way:
 *
 *       A = 0.5 * radius + 1        texels of level i-1
 *
 *   UPSAMPLE, level i -> level i-1. The 8-tap kernel's axis taps offset by
 *   `2 * step`, i.e. `radius` texels of level i -- twice the downsample's
 *   reach, because the axis taps are at 2h where the diagonals are at h. Plus
 *   the same bilinear texel:
 *
 *       B = radius + 1              texels of level i
 *
 * A level-i texel spans `width / level_extent(width, i)` SOURCE pixels, which
 * is 2^i exactly when the extent halves cleanly and slightly more when it does
 * not -- 129 px over 64 texels is 2.016. Computed rather than assumed, because
 * assuming 2^i under-covers on every odd extent, and an under-covered support
 * is a stale fringe that only appears on a moving window.
 *
 * Summing the chain (down 1..N, then up N..1) and adding one pixel for a
 * fractional source origin gives the bound. It is an upper bound because offsets
 * compose additively along the chain and every tap is additionally clamped into
 * the valid region by the shader.
 *
 * Exposed because the compositor computes damage before it ever calls
 * avk_blur_declare(), and a second implementation of this arithmetic elsewhere
 * is exactly how the two drift apart.
 */
struct avk_blur_support avk_blur_support_of(
	const struct avk_blur_params *params, uint32_t width, uint32_t height);

/* The largest of the four edges, rounded outward -- for callers that want one
 * number and can afford the symmetric case. */
uint32_t avk_blur_support_max(const struct avk_blur_params *params,
	uint32_t width, uint32_t height);

/*
 * THE OTHER DIRECTION: which OUTPUT pixels one changed SOURCE pixel can affect.
 *
 * avk_blur_support_of() answers "what source pixels can affect this output
 * pixel" -- the question a CAPTURE region asks. Damage propagation asks the
 * inverse, and the two are not the same question even when they turn out to be
 * the same number. Naming the direction at the call site is the point: a region
 * dilated by the wrong one is a stale fringe nobody sees until a window moves.
 *
 * THE DERIVATION, and it does come out equal.
 *
 * Work in SOURCE PIXELS throughout and track the half-width `w` of the affected
 * interval. One downsample step, level i-1 -> i: a level-i texel reads level
 * i-1 within `A = 0.5*radius + 1` texels OF LEVEL i-1 of its own centre. So a
 * level-(i-1) position c is read by exactly those level-i texels whose centres
 * lie within `A * span(i-1)` source pixels of c -- and that is the same
 * constant, in the same units, as the reverse step adds. The mapping between
 * two levels is affine (centre(p) = (p + 0.5) * span, an affine function of the
 * index) and the kernel is symmetric about its centre, so dilating an interval
 * through it costs the same whichever way it is traversed. Upsampling is the
 * same argument with `B = radius + 1`.
 *
 * Summing the chain gives the identical total, so:
 *
 *     forward reach  ==  reverse reach,  exactly, per axis, in source pixels
 *
 * WHAT IS NOT SHARED is the rounding. Reverse maps an output interval to source
 * pixels; forward maps a source interval to output pixels. Both round OUTWARD,
 * so both are conservative -- but they round different quantities, which is why
 * this is a separate entry point rather than a comment telling callers to reuse
 * the other one.
 *
 * tests/test-avk-blur-damage.c checks the equality against a per-level interval
 * walk that composes the steps explicitly instead of summing them, at odd and
 * even extents alike. If someone changes one kernel's reach and not the other,
 * that test fails rather than a window growing a fringe.
 */
struct avk_blur_support avk_blur_forward_support_of(
	const struct avk_blur_params *params, uint32_t width, uint32_t height);

uint32_t avk_blur_forward_support_max(const struct avk_blur_params *params,
	uint32_t width, uint32_t height);

/*
 * AN UPPER BOUND ON THE SUPPORT THAT DOES NOT NEED TO KNOW THE EXTENT.
 *
 * The compositor has to decide which scene commands to KEEP -- how far outside
 * an output's own bounds a blur on that output could reach for its source --
 * before it has walked the scene and therefore before it knows how big any blur
 * node is. avk_blur_support_of() needs the extent; this does not.
 *
 * THE DERIVATION, and it is a bound rather than a guess.
 *
 * The only extent-dependent term is the texel span,
 * `span(base, i) = base / (base >> i)`. Write `base = q*2^i + r` with
 * `0 <= r < 2^i`, so `base >> i == q` and `span = 2^i + r/q`. A level is only
 * used while it has at least two texels (see effective_levels), so `q >= 2`,
 * and therefore
 *
 *     span(base, i)  <=  2^i + (2^i - 1)/2
 *
 * exactly. Substituting that for every span in support_axis() gives a bound
 * that holds for every extent at once. It is about 1.5x the support a large
 * extent actually produces, which is the price of not knowing the extent -- and
 * it is only ever used to widen a RETENTION region, never to size a capture or
 * a damage region, both of which use the exact per-node support.
 */
uint32_t avk_blur_support_bound(const struct avk_blur_params *params);

/*
 * THE FOUR REGIONS OF ONE BLUR, kept apart because collapsing any two of them
 * is a bug that still renders a plausible picture.
 *
 *   write        where the blur's result is composited. The node's own box.
 *   dependency   source pixels that can reach it: write dilated by support.
 *   capture      dependency, outward-aligned to an EVEN origin so the
 *                fwidth() derivative quads of a regional target line up with
 *                the output's (see avk_render_segment_align_origin).
 *   allocation   what the transient pool actually handed out, >= capture.
 *
 * The hard invariant, asserted rather than assumed:
 *
 *     write  subset-of  dependency  subset-of  capture  subset-of  allocation
 *
 * Alignment may only ever GROW the capture. Shifting the origin down without
 * growing the extent would drop the far edge, which is a stale fringe on the
 * right and bottom of every blurred window -- and one that only appears when
 * the dependency happens to start on an odd coordinate.
 */
struct avk_blur_regions {
	struct avk_box write;
	struct avk_box dependency;
	struct avk_box capture;
};

/*
 * Derive the three from a blur node's box and its parameters.
 *
 * `clamp` bounds every region to the scene (an output's extent, typically);
 * pass NULL for unbounded. Returns false if the result is empty, in which case
 * there is nothing to blur.
 */
bool avk_blur_regions_of(struct avk_blur_regions *out,
	const struct avk_box *write, const struct avk_blur_params *params,
	const struct avk_box *clamp);

/* Drop the frame's blur-pass arena. Called once per frame beside
 * avk_graph_reset(); the passes it holds are referenced by graph callbacks and
 * may not be reused while a graph still names them. */
void avk_blur_frame_reset(void);


/*
 * ── WHAT THE CHAIN ACTUALLY PROCESSES, AND WHAT IT WOULD HAVE TO ──────────
 *
 * M4F.2D. The per-level scissor decision needs one number:
 *
 *     actual blur-pass work
 *     -------------------------------------------------
 *     proven required blur-pass work, footprints included
 *
 * and it is NOT processed pixels over the composite's read region. A texel
 * shaded at level 0 and a texel shaded at level 1 are separate GPU work, so
 * both sides are SUMMED PER PASS. And the required side is not the result
 * region either: every level must produce enough for the next pass's filter
 * footprint to read, or the result is wrong.
 *
 * ── THE DERIVATION, USING THE SAME CONSTANTS THE PASSES USE ───────────────
 *
 * Nothing here is a second approximation of the filter. It is the same
 * arithmetic support_axis() sums, applied per level instead of accumulated:
 *
 *   a DOWN pass writing level i reads level i-1 over (0.5*radius + 1) texels
 *   of level i-1 around each destination texel;
 *   an UP pass writing level i-1 reads level i over (radius + 1) texels of
 *   level i.
 *
 * Level extents come from level_extent() and the mapping between levels from
 * texel_span(), so odd extents are handled by the same floor-and-measure the
 * passes use rather than by assuming 2^i.
 *
 * Walking it:
 *
 *   O(0) = the demanded result region
 *   U(i) = the up pass writing level i-1 needs this much of level i:
 *          map O(i-1) down into level i, dilate by (radius + 1)
 *   O(i) = U(i)                       -- what the up pass at level i writes
 *   D(L) = U(L)                       -- deepest level, only the up chain reads it
 *   D(i) = U(i) UNION (map D(i+1) up into level i, dilated by 0.5*radius + 1)
 *
 * required = SUM over down passes of area(D(i))
 *          + SUM over up passes of area(O(i-1))
 * actual   = SUM over every pass of its full destination extent
 *
 * ── WHAT THIS IS NOT ──────────────────────────────────────────────────────
 *
 * It is RECTANGLE arithmetic. A multi-rectangle result region is treated as
 * its bounding box, which makes `required` an OVER-estimate and therefore the
 * amplification an UNDER-estimate: the error is in the direction that
 * understates the case for optimising, which is the safe direction for a
 * decision about whether to optimise.
 *
 * It is also NOT a GPU speedup estimate. Pass overhead, raster setup,
 * barriers, bandwidth and sampling are not proportional to shaded texels. This
 * answers "how much pixel work could regional execution theoretically avoid",
 * and nothing else.
 */
struct avk_blur_level_work {
	uint32_t level;
	/* The image at this level: what a transient is allocated at. */
	uint32_t extent_w, extent_h;
	/* What the pass writing this level actually renders today: all of it. */
	uint64_t actual_px;
	/*
	 * REQUIRED OUTPUT: what this pass would have to RENDER for the demanded
	 * result to be correct. This is the region a scissor would set.
	 */
	uint32_t req_x, req_y, req_w, req_h;
	uint64_t required_px;
	/*
	 * REQUIRED INPUT: what it must be able to SAMPLE from the level it reads,
	 * which is the output region mapped into that level and dilated by this
	 * pass's own filter footprint. A different quantity in a different level's
	 * texels, and keeping them in one rectangle is how a scissor ends up one
	 * strip short.
	 *
	 * The final upsample makes the distinction concrete: for a 16x16 demand it
	 * RENDERS 16x16 and READS 20x20 of level 1. A test that expected its
	 * rendered region to be wider than the demand was asserting the wrong one.
	 */
	uint32_t in_level;
	uint32_t in_x, in_y, in_w, in_h;
};

struct avk_blur_work {
	uint32_t levels;
	/* down[i] describes the pass writing level i, i = 1..levels.
	 * up[i] describes the pass writing level i, i = 0..levels-1. */
	struct avk_blur_level_work down[AVK_BLUR_MAX_LEVELS + 1];
	struct avk_blur_level_work up[AVK_BLUR_MAX_LEVELS + 1];
	uint64_t actual_px;
	uint64_t required_px;
	/* What level 0 -- the capture -- must contain for the first down pass to
	 * read. Not a pass, but it is what a prefix replay has to reconstruct. */
	uint32_t capture_req_x, capture_req_y, capture_req_w, capture_req_h;
};

/*
 * Derive both sides for a chain of `width` x `height` whose composite will
 * read `result` (in capture-local pixels). Returns false if the configuration
 * runs no levels, in which case there is no blur work to account for.
 */
bool avk_blur_work_of(const struct avk_blur_params *params, uint32_t width,
	uint32_t height, const struct avk_box *result, struct avk_blur_work *out);

#endif /* AVK_BLUR_H */
