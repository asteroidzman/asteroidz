#ifndef AZ_SCANOUT_H
#define AZ_SCANOUT_H

/*
 * ── DIRECT SCANOUT, AND WHY IT DID NOT HAPPEN ─────────────────────────────
 *
 * M13B. Handing a client's buffer straight to the display, with no composition
 * pass at all: no read of the client's pixels, no write of ours, no GPU work
 * for a frame that would have been a copy. For a fullscreen game at 4K144 that
 * is the difference between the compositor costing something and costing
 * nothing.
 *
 * It has not happened in this compositor since the scene absorption.
 * scene_entry_try_direct_scanout() lives in src/scene/wlr_scene.c with no
 * caller, because every frame goes through az_output_build_frame() ->
 * az_avk_build_frame(), which composites unconditionally. So this is a REVIVAL
 * in AVK's own frame seam rather than a port: the decision is re-derived here
 * from the compositor's state, and the orphan stays orphaned.
 *
 * ── EVERY REFUSAL HAS A NAME ──────────────────────────────────────────────
 *
 * The scene version returns SCANOUT_INELIGIBLE from eleven different places.
 * That is the failure this milestone exists to fix: "why is my game not
 * scanning out" is unanswerable when every answer is the same constant. Each
 * rejection here carries a reason, and the reason is what the inspector prints.
 *
 * ── WHAT IS DELIBERATELY REFUSED ──────────────────────────────────────────
 *
 * Composition is not bypassed unless the result is provably identical. Anything
 * the compositor would have DONE to those pixels -- a border, a rounded corner,
 * blur, a shadow, opacity, a tone map, an ICC transform -- means the scanned
 * buffer is not what the user should see, so scanout is refused rather than
 * approximated. A faster wrong frame is still wrong.
 */

#include <stdbool.h>
#include <stdint.h>

enum az_scanout_verdict {
	AZ_SCANOUT_ACCEPTED = 0,
	/* No single fullscreen client owns this output. */
	AZ_SCANOUT_NO_CANDIDATE,
	/* The `no-scanout` window rule. Kept because it is the operator's
	 * documented escape from a driver bug -- see the gamescope note below. */
	AZ_SCANOUT_RULE_DISABLED,
	AZ_SCANOUT_NO_BUFFER,
	/* wl_shm cannot be scanned out: the display controller reads from the
	 * GPU's own memory, and a shm buffer is a CPU mapping we upload from. */
	AZ_SCANOUT_NOT_DMABUF,
	/* The buffer does not cover the output exactly. Scaling it would be the
	 * composition we are trying to skip. */
	AZ_SCANOUT_GEOMETRY,
	AZ_SCANOUT_TRANSFORM_MISMATCH,
	/* Border, rounded corners, blur, shadow or opacity: the compositor owes
	 * this surface pixels that are not in its buffer. */
	AZ_SCANOUT_EFFECTS,
	/* The output carries a measured display profile in the encode pass. The
	 * scanout path has no encode pass to carry it. */
	AZ_SCANOUT_OUTPUT_ICC,
	/* The source's colour volume is not what the connector is presenting, so
	 * the compositor must map between them. This is the HDR case: a PQ buffer
	 * on an SDR output, or an SDR buffer on an HDR one. */
	AZ_SCANOUT_TONE_MAP_REQUIRED,
	/* A mode, enable or format change is in this commit. Legacy DRM will
	 * explode if a modeset carries a client buffer. */
	AZ_SCANOUT_MODESET_PENDING,
	/* The backend tested the state and said no -- a modifier the display
	 * controller cannot read, a plane constraint, a bandwidth limit. The one
	 * verdict that requires KMS to answer. */
	AZ_SCANOUT_KMS_REFUSED,
	AZ_SCANOUT_VERDICT_COUNT,
};

static inline const char *az_scanout_verdict_name(enum az_scanout_verdict v) {
	switch (v) {
	case AZ_SCANOUT_ACCEPTED:            return "accepted";
	case AZ_SCANOUT_NO_CANDIDATE:        return "no-candidate";
	case AZ_SCANOUT_RULE_DISABLED:       return "rule-disabled";
	case AZ_SCANOUT_NO_BUFFER:           return "no-buffer";
	case AZ_SCANOUT_NOT_DMABUF:          return "not-dmabuf";
	case AZ_SCANOUT_GEOMETRY:            return "geometry";
	case AZ_SCANOUT_TRANSFORM_MISMATCH:  return "transform-mismatch";
	case AZ_SCANOUT_EFFECTS:             return "effects-active";
	case AZ_SCANOUT_OUTPUT_ICC:          return "output-icc";
	case AZ_SCANOUT_TONE_MAP_REQUIRED:   return "tone-map-required";
	case AZ_SCANOUT_MODESET_PENDING:     return "modeset-pending";
	case AZ_SCANOUT_KMS_REFUSED:         return "kms-refused";
	case AZ_SCANOUT_VERDICT_COUNT:       break;
	}
	return "?";
}

/* The sentence a user should read. Deliberately says what to DO where there is
 * something to do, because "transform-mismatch" is a label and not an answer. */
static inline const char *az_scanout_verdict_why(enum az_scanout_verdict v) {
	switch (v) {
	case AZ_SCANOUT_ACCEPTED:
		return "the client's buffer went straight to the display";
	case AZ_SCANOUT_NO_CANDIDATE:
		return "no single fullscreen window owns this output";
	case AZ_SCANOUT_RULE_DISABLED:
		return "a no-scanout window rule forbids it for this client";
	case AZ_SCANOUT_NO_BUFFER:
		return "the client has not attached a buffer";
	case AZ_SCANOUT_NOT_DMABUF:
		return "the client committed shared memory, which a display "
		       "controller cannot read";
	case AZ_SCANOUT_GEOMETRY:
		return "the buffer does not cover the output exactly, so it would "
		       "have to be scaled";
	case AZ_SCANOUT_TRANSFORM_MISMATCH:
		return "the buffer's rotation does not match the output's";
	case AZ_SCANOUT_EFFECTS:
		return "the compositor owes this window pixels that are not in its "
		       "buffer (border, corners, blur, shadow or opacity)";
	case AZ_SCANOUT_OUTPUT_ICC:
		return "this output's colour is carried by the encode pass, and "
		       "scanout has no encode pass";
	case AZ_SCANOUT_TONE_MAP_REQUIRED:
		return "the source's colour volume differs from what the display is "
		       "presenting, so the compositor must map between them";
	case AZ_SCANOUT_MODESET_PENDING:
		return "this commit changes the mode, and a modeset must not carry a "
		       "client buffer";
	case AZ_SCANOUT_KMS_REFUSED:
		return "the display controller refused the buffer (modifier, plane "
		       "or bandwidth)";
	case AZ_SCANOUT_VERDICT_COUNT:
		break;
	}
	return "?";
}

/*
 * ── ELIGIBILITY, WITHOUT TOUCHING KMS ─────────────────────────────────────
 *
 * Everything decidable from compositor state. The one verdict this cannot
 * reach is KMS_REFUSED, which needs a test commit -- so the inspector can call
 * this on any surface at any time, for free, and get every answer but that one.
 *
 * `out_c` receives the candidate when there is one, so a caller that goes on to
 * attempt the scanout does not resolve it twice.
 */
static inline enum az_scanout_verdict az_scanout_eligible(Monitor *m,
		const struct wlr_output_state *state, Client **out_c) {
	if (out_c != NULL) {
		*out_c = NULL;
	}
	if (m == NULL || m->wlr_output == NULL) {
		return AZ_SCANOUT_NO_CANDIDATE;
	}

	/*
	 * A mode/enable/format change first, because it is a property of the
	 * COMMIT rather than of any client: a modeset carrying a client buffer is
	 * how legacy DRM explodes, and asking about the client first would report
	 * a client-shaped reason for an output-shaped refusal.
	 */
	if (state != NULL && (state->committed & (WLR_OUTPUT_STATE_MODE
			| WLR_OUTPUT_STATE_ENABLED | WLR_OUTPUT_STATE_RENDER_FORMAT))) {
		return AZ_SCANOUT_MODESET_PENDING;
	}

	/*
	 * THE SAME CANDIDATE THE HDR METADATA PATH USES. One definition of "what is
	 * covering this output", so the window whose colour volume is forwarded to
	 * the connector and the window whose buffer is scanned out cannot be two
	 * different windows.
	 */
	Client *c = mon_hdr_scanout_candidate(m);
	if (c == NULL) {
		return AZ_SCANOUT_NO_CANDIDATE;
	}
	if (out_c != NULL) {
		*out_c = c;
	}
	if (c->noscanout) {
		return AZ_SCANOUT_RULE_DISABLED;
	}

	/*
	 * ── EFFECTS, AND WHY THIS IS SHORTER THAN IT LOOKS ────────────────────
	 *
	 * Anything the compositor would draw that is not in the client's buffer
	 * makes the buffer the wrong picture. But a scanout candidate is fullscreen
	 * BY DEFINITION -- mon_hdr_scanout_candidate() requires it -- and being
	 * fullscreen already turns off all three decorations:
	 *
	 *   border   setfullscreen() sets c->bw = 0 directly
	 *   corners  client.h:416 clears the corner location when c->isfullscreen
	 *   shadow   client_draw_shadow()'s `active` is gated on !c->isfullscreen
	 *
	 * So testing the per-window OVERRIDE flags here -- isnoradius, isnoshadow --
	 * would be testing the wrong thing twice over: they say whether a rule
	 * disabled an effect, not whether the effect is drawn, and with no rule set
	 * they read as "effect enabled" and refuse every candidate forever. The
	 * first version of this did exactly that, and refused a window on a config
	 * with borders, shadows and blur all globally off.
	 *
	 * `bw` is still checked, because it is cheap and because a border on a
	 * fullscreen window would mean an invariant broke somewhere else.
	 */
	if (c->bw > 0) {
		return AZ_SCANOUT_EFFECTS;
	}
	/*
	 * OPACITY IS THE ONE THAT SURVIVES FULLSCREEN. A translucent fullscreen
	 * window is composited against what is behind it, and its own buffer is not
	 * that composite.
	 */
	float opacity = c == focustop(m) ? c->focused_opacity : c->unfocused_opacity;
	if (opacity > 0.0f && opacity < 1.0f) {
		return AZ_SCANOUT_EFFECTS;
	}

	struct wlr_surface *surface = client_surface(c);
	if (surface == NULL) {
		return AZ_SCANOUT_NO_BUFFER;
	}
	struct wlr_buffer *buf = az_surface_buffer(surface);
	if (buf == NULL) {
		return AZ_SCANOUT_NO_BUFFER;
	}
	struct wlr_dmabuf_attributes dmabuf;
	if (!wlr_buffer_get_dmabuf(buf, &dmabuf)) {
		return AZ_SCANOUT_NOT_DMABUF;
	}

	/* The buffer must be the output's own pixels, one for one. */
	if (surface->current.transform != m->wlr_output->transform) {
		return AZ_SCANOUT_TRANSFORM_MISMATCH;
	}
	int ow = 0, oh = 0;
	wlr_output_transformed_resolution(m->wlr_output, &ow, &oh);
	if (buf->width != ow || buf->height != oh) {
		return AZ_SCANOUT_GEOMETRY;
	}

	/*
	 * COLOUR. The encode pass is what applies this output's profile and this
	 * output's transfer function; scanout has no encode pass, so anything the
	 * encode pass was carrying is a refusal.
	 */
	/*
	 * ── A LOADED PROFILE IS NOT AN APPLIED PROFILE ────────────────────────
	 *
	 * This tested `m->icc_transform != NULL` and was wrong for the operator's
	 * own display. DP-1 carries an ICC profile AND runs HDR, and M6B/D3 makes
	 * the profile INERT there -- the connector presents its own image
	 * description, so stacking an SDR characterisation on top would be two
	 * transforms on one pixel. Its encode_tf is PQ, not LUT1D.
	 *
	 * So the question is not "is a profile loaded" but "is the encode pass
	 * carrying one", because the encode pass is the thing scanout skips. Same
	 * predicate the inspector prints as `icc_applied`.
	 */
	if (m->color_state.encode_tf == AZ_TF_LUT1D
			|| m->color_state.encode_tf == AZ_TF_CLUT3D) {
		return AZ_SCANOUT_OUTPUT_ICC;
	}
	/*
	 * The source's declared colour must equal what the connector presents.
	 * az_source_desc_from_wlr() is the same translation the renderer uses, so
	 * "PQ" here means exactly what it means in the draw path.
	 */
	const struct wlr_image_description_v1_data *img =
		wlr_surface_get_image_description_v1_data(surface);
	if (img == NULL) {
		img = az_cm_surface_description(surface);
	}
	bool src_hdr = false;
	if (img != NULL) {
		struct az_lum_source_desc sd = az_source_desc_from_wlr(
			wlr_color_manager_v1_transfer_function_to_wlr(img->tf_named),
			wlr_color_manager_v1_primaries_to_wlr(img->primaries_named),
			img->max_cll);
		src_hdr = sd.tagged && az_lum_tf_is_hdr(sd.tf);
	}
	if (src_hdr != (m->hdr > 0)) {
		return AZ_SCANOUT_TONE_MAP_REQUIRED;
	}

	return AZ_SCANOUT_ACCEPTED;
}

/*
 * ── THE ATTEMPT ───────────────────────────────────────────────────────────
 *
 * Builds a candidate state, asks the backend, and on success leaves the
 * client's buffer in `state` for the caller to commit. Returns false with a
 * verdict otherwise, and `state` is untouched in that case -- the caller
 * composites exactly as before, so a refusal costs one test and nothing else.
 *
 * ── THE ACQUIRE FENCE IS NOT OPTIONAL ─────────────────────────────────────
 *
 * This is the bug that produced RGB noise across gamescope's window on this
 * machine, diagnosed as "a missing explicit-sync race in direct scanout" and
 * worked around with a per-app no-scanout rule. Composition reads the client's
 * buffer through AVK, which waits on the acquire point; scanout hands the
 * buffer to the DISPLAY, which waits on nothing unless told to. Without the
 * wait timeline the display can scan a buffer the client's GPU work has not
 * finished writing -- and what that looks like is exactly the noise that was
 * seen.
 *
 * So a client that speaks linux-drm-syncobj gets its acquire point attached to
 * the commit. A client that does not speaks implicit sync, where the kernel
 * carries the dependency on the buffer itself and the display honours it.
 */
static inline bool az_scanout_try(Monitor *m, struct wlr_output_state *state,
		enum az_scanout_verdict *out_why) {
	Client *c = NULL;
	enum az_scanout_verdict why = az_scanout_eligible(m, state, &c);
	if (why != AZ_SCANOUT_ACCEPTED) {
		if (out_why != NULL) {
			*out_why = why;
		}
		return false;
	}

	struct wlr_surface *surface = client_surface(c);
	struct wlr_buffer *buf = az_surface_buffer(surface);

	struct wlr_output_state pending;
	wlr_output_state_init(&pending);
	if (!wlr_output_state_copy(&pending, state)) {
		wlr_output_state_finish(&pending);
		if (out_why != NULL) {
			*out_why = AZ_SCANOUT_KMS_REFUSED;
		}
		return false;
	}

	wlr_output_state_set_buffer(&pending, buf);

	/* See above: without this the display can scan a half-written frame. */
	struct wlr_linux_drm_syncobj_surface_v1_state *sync =
		wlr_linux_drm_syncobj_v1_get_surface_state(surface);
	if (sync != NULL && sync->acquire_timeline != NULL) {
		wlr_output_state_set_wait_timeline(&pending, sync->acquire_timeline,
			sync->acquire_point);
	}

	if (!wlr_output_test_state(m->wlr_output, &pending)) {
		wlr_output_state_finish(&pending);
		if (out_why != NULL) {
			*out_why = AZ_SCANOUT_KMS_REFUSED;
		}
		return false;
	}

	wlr_output_state_copy(state, &pending);
	wlr_output_state_finish(&pending);
	if (out_why != NULL) {
		*out_why = AZ_SCANOUT_ACCEPTED;
	}
	return true;
}

#endif /* AZ_SCANOUT_H */
