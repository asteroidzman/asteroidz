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
 * How far a blur's influence reaches, in SOURCE pixels, for a given level
 * count and radius.
 *
 * This is the number damage expansion needs and it is a property of the kernel,
 * not a guess. Each downsample doubles the texel size, and the kernel reaches
 * 2 texels at the level it runs on, so level i contributes 2 * 2^i source
 * pixels on the way down and the same on the way up:
 *
 *     support = radius * 2 * sum(2^i, i=0..levels) * 2
 *
 * Exposed rather than kept private because the compositor computes damage
 * before it ever calls avk_blur_declare(), and a second implementation of this
 * arithmetic somewhere else is how a blur ends up with a one-pixel stale edge
 * that only shows on a moving window.
 */
uint32_t avk_blur_support(const struct avk_blur_params *params);

/* Drop the frame's blur-pass arena. Called once per frame beside
 * avk_graph_reset(); the passes it holds are referenced by graph callbacks and
 * may not be reused while a graph still names them. */
void avk_blur_frame_reset(void);

#endif /* AVK_BLUR_H */
