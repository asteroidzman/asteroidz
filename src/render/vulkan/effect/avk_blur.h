#ifndef AVK_BLUR_H
#define AVK_BLUR_H

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
};

struct avk_blur_stats {
	uint64_t chains;        /* blur chains built */
	uint64_t passes;        /* down + up passes declared */
	uint64_t transients;    /* transient acquires made for blur levels */
	uint64_t skipped;       /* chains declined -- too small, or 0 levels */
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
bool avk_blur_declare(struct avk_graph *graph, struct avk_transient_pool *pool,
	struct avk_pipelines *pipes, struct avk_blur_stats *stats,
	uint32_t src_resource, uint32_t dst_resource,
	uint32_t width, uint32_t height, VkFormat format,
	const struct avk_blur_params *params);

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

/* Drop the frame's blur-pass arena. Called once per frame beside
 * avk_graph_reset(); the passes it holds are referenced by graph callbacks and
 * may not be reused while a graph still names them. */
void avk_blur_frame_reset(void);

#endif /* AVK_BLUR_H */
