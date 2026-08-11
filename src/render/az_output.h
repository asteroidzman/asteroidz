#ifndef AZ_OUTPUT_H
#define AZ_OUTPUT_H

/*
 * One place a frame is built.
 *
 * Before this file, asteroidz called wlr_scene_output_build_state() from four
 * places -- the ordinary frame, the screenshot capture, the HDR pending-change
 * fold-in, and the tearing path -- each with its own colour-transform
 * expression and its own error handling. That is four places a new renderer
 * would have to be taught about, and four places for them to disagree.
 *
 * az_output_build_frame() is the single seam. Which engine builds the frame is
 * decided here and nowhere else, and the callers keep doing what they were
 * doing with the resulting wlr_output_state.
 */

enum az_renderer_backend {
	/* SceneFX on top of wlroots' renderer. The known-good path, and still the
	 * default. */
	AZ_RENDERER_WLR,
	/* asteroidz's own Vulkan engine, src/render/vulkan/. */
	AZ_RENDERER_AVK,
};

static enum az_renderer_backend az_renderer = AZ_RENDERER_WLR;

struct az_frame_options {
	/* The colour transform the frame should be built with, exactly as the
	 * caller would have put it in wlr_scene_output_state_options. */
	struct wlr_color_transform *color_transform;
};

/*
 * Build one frame of `m` into `state`.
 *
 * Returns false if no frame could be built, which the callers treat the same
 * way they always have. Note what this does NOT mean: an AVK-mode output that
 * cannot be composited by AVK does not fail here -- it falls back to the
 * SceneFX path inside this function and returns true. The distinction matters
 * because a fallback is a slower frame while a failure is no frame.
 */
static inline bool az_output_build_frame(Monitor *m,
		struct wlr_output_state *state, const struct az_frame_options *opts) {
#ifdef AZ_HAVE_VULKAN
	if (az_renderer == AZ_RENDERER_AVK &&
			az_avk_build_frame(m, state, opts->color_transform)) {
		return true;
	}
#endif
	struct wlr_scene_output_state_options scene_options = {
		.color_transform = opts->color_transform,
	};
	return wlr_scene_output_build_state(m->scene_output, state,
		&scene_options);
}

/*
 * A commit that was built but did not land.
 *
 * wlr_scene_output_commit() calls wlr_damage_ring_add_whole() when the commit
 * fails, and asteroidz replicates that function by hand -- so it has to
 * replicate this too. Building a frame rotates the damage ring, which records
 * the damage as having been drawn into that buffer. It *was* drawn; the buffer
 * simply never reached the screen. Without trashing the ring, the next frame
 * inherits a region nobody will ever repaint, and the result is a rectangle of
 * stale pixels that survives until something else happens to damage it.
 *
 * This did nothing while AVK redrew everything every frame, which is exactly
 * why it is easy to leave out and hard to find afterwards.
 */
static inline void az_output_commit_failed(Monitor *m) {
	if (m->scene_output != NULL) {
		wlr_damage_ring_add_whole(&m->scene_output->damage_ring);
	}
}

/*
 * The colour transform for an ordinary frame on `m`.
 *
 * The expression was written out at each of the four call sites and had to
 * stay in step between them: an output that carries its own image description
 * is already colour-managed by the connector, so applying the ICC transform on
 * top would apply it twice.
 */
static inline struct wlr_color_transform *az_output_color_transform(Monitor *m) {
	return m->wlr_output->image_description == NULL ? m->icc_transform : NULL;
}

#endif /* AZ_OUTPUT_H */
