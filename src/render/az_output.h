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

/* For the callers included before this header; see asteroidz.c. */
static bool az_renderer_is_avk(void) {
	return az_renderer == AZ_RENDERER_AVK;
}

struct az_frame_options {
	/* The colour transform the frame should be built with, exactly as the
	 * caller would have put it in wlr_scene_output_state_options. */
	struct wlr_color_transform *color_transform;
};

/*
 * Build one frame of `m` into `state`.
 *
 * Returns false if no frame could be built, which the callers treat the same
 * way they always have.
 *
 * An AVK-mode output that cannot be composited by AVK no longer falls back:
 * it aborts, here or inside az_avk_build_frame(), naming the reason. A frame
 * that silently came from the other renderer is indistinguishable from a
 * correct one until something else goes wrong, and by then the frame that
 * caused it is long gone.
 */
static inline bool az_output_build_frame(Monitor *m,
		struct wlr_output_state *state, const struct az_frame_options *opts) {
#ifdef AZ_HAVE_VULKAN
	if (az_renderer == AZ_RENDERER_AVK &&
			az_avk_build_frame(m, state, opts->color_transform)) {
		return true;
	}
	/*
	 * ── IN AN AVK SESSION, REACHING SceneFX IS FATAL ─────────────────────
	 *
	 * az_avk_build_frame() aborts on an output it REFUSES, naming the reason.
	 * It also returns false without refusing anything -- when AVK is not
	 * active, or the monitor has no scene output -- and control then arrives
	 * here, where SceneFX composites the frame on wlroots' GLES renderer and
	 * the desktop looks fine.
	 *
	 * That second path is the one worth closing, because it has no diagnostic
	 * at all: not a warning, not a counter, just a frame that came from the
	 * other renderer. An AVK session that composites with GL is either a bug
	 * or a state nobody intended, and both are best investigated at the frame
	 * that did it.
	 *
	 * No escape variable here either, for the same reason it is absent in
	 * az_avk.h: a switch that turns the fallback back on is the fallback.
	 */
	if (az_renderer == AZ_RENDERER_AVK) {
		wlr_log(WLR_ERROR,
			"AVK session reached the SceneFX/GLES compositor for %s "
			"(avk active=%d, scene_output=%p) -- this compositor does not "
			"composite with GL.",
			m->wlr_output != NULL ? m->wlr_output->name : "(output)",
			(int)avk.active, (void *)m->scene_output);
		abort();
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
/*
 * M6B/G2 adds the second half of the same rule. When C3 derived AZ_TF_LUT1D
 * the AVK encode pass is applying the profile itself, from the same file, so
 * handing wlroots the transform as well would apply it twice -- once as a
 * matrix and curve in the encode pass and once as a 3D LUT in SceneFX. Exactly
 * one owner, and the colour state names which.
 */
static inline struct wlr_color_transform *az_output_color_transform(Monitor *m) {
	if (m->wlr_output->image_description != NULL) {
		return NULL;
	}
	/* M6C adds CLUT3D, where the double application would be even more literal:
	 * the same 3D table, built from the same profile, sampled once in AVK's
	 * encode pass and once in SceneFX's. */
	if (m->color_state.encode_tf == AZ_TF_LUT1D
			|| m->color_state.encode_tf == AZ_TF_CLUT3D) {
		return NULL;
	}
	return m->icc_transform;
}

#endif /* AZ_OUTPUT_H */
