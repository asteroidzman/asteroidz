#ifndef AZ_OUTPUT_H
#define AZ_OUTPUT_H

struct az_frame_options {
	struct wlr_color_transform *color_transform;
};

/* Feedback is owned from sampling until the matching output presentation.
 * Surface lifetime is independent: wlroots owns the feedback resources. */
struct az_output_feedback {
	struct wl_list link;
	struct wlr_presentation_feedback *feedback;
	uint32_t commit_seq;
	bool committed;
	bool zero_copy;
};

static void az_output_sample_surface(Monitor *m, struct wlr_surface *surface,
		bool zero_copy) {
	struct wlr_presentation_feedback *feedback =
		wlr_presentation_surface_sampled(surface);
	if (feedback == NULL)
		return;
	struct az_output_feedback *sample = calloc(1, sizeof(*sample));
	if (sample == NULL) {
		wlr_presentation_feedback_destroy(feedback);
		return;
	}
	sample->feedback = feedback;
	sample->zero_copy = zero_copy;
	wl_list_insert(m->presentation_feedback.prev, &sample->link);
}

static void az_output_feedback_discard(Monitor *m, bool all) {
	struct az_output_feedback *sample, *tmp;
	wl_list_for_each_safe(sample, tmp, &m->presentation_feedback, link) {
		if (!all && sample->committed)
			continue;
		wl_list_remove(&sample->link);
		wlr_presentation_feedback_destroy(sample->feedback);
		free(sample);
	}
}

static void az_output_feedback_present(Monitor *m,
		const struct wlr_output_event_present *event) {
	struct az_output_feedback *sample, *tmp;
	wl_list_for_each_safe(sample, tmp, &m->presentation_feedback, link) {
		if (!sample->committed || sample->commit_seq != event->commit_seq)
			continue;
		if (event->presented) {
			struct wlr_presentation_event presented;
			wlr_presentation_event_from_output(&presented, event);
			if (!sample->zero_copy)
				presented.flags &= ~WLR_OUTPUT_PRESENT_ZERO_COPY;
			wlr_presentation_feedback_send_presented(sample->feedback, &presented);
		}
		wl_list_remove(&sample->link);
		wlr_presentation_feedback_destroy(sample->feedback);
		free(sample);
	}
}

static inline bool az_log_decade(uint64_t n) {
	while (n >= 10 && n % 10 == 0)
		n /= 10;
	return n == 1;
}

/* Bound retries when no successful commit will produce another frame event.
 * Reuse the output's render timer; its callback requests a backend frame. */
static void az_output_retry_frame(Monitor *m) {
	if (!m->wlr_output->enabled || m->iscleanuping)
		return;
	if (m->render_timer != NULL) {
		int delay_ms = m->wlr_output->refresh > 0
			? (int)ceil(1000000.0 / m->wlr_output->refresh) : 16;
		/* A persistent allocation/import failure must not spin at refresh rate. */
		unsigned shift = m->frame_build_failures > 5 ? 5
			: m->frame_build_failures > 0 ? m->frame_build_failures - 1 : 0;
		delay_ms = ASTEROIDZ_MIN(delay_ms << shift, 250);
		m->frame_retry_pending = true;
		m->render_late_pending = true;
		wl_event_source_timer_update(m->render_timer, delay_ms > 0 ? delay_ms : 1);
	} else {
		wlr_output_schedule_frame(m->wlr_output);
	}
}

static inline void az_output_commit_failed(Monitor *m) {
	if (m->scene_output != NULL)
		wlr_damage_ring_add_whole(&m->scene_output->damage_ring);
	az_output_retry_frame(m);
}

static inline bool az_output_build_frame(Monitor *m,
		struct wlr_output_state *state, const struct az_frame_options *opts) {
	/* A previous build may have been abandoned without a commit. */
	az_output_feedback_discard(m, false);
	if (!avk_device_lost() && az_avk_build_frame(m, state, opts->color_transform)) {
		m->frame_build_failures = 0;
		return true;
	}
	az_output_feedback_discard(m, false);
	if (avk_device_lost()) {
		static bool announced;
		if (!announced) {
			announced = true;
			wlr_log(WLR_ERROR, "the GPU was lost; ending the session");
			quit_now(NULL);
		}
		return false;
	}
	m->frame_build_failures++;
	if (az_log_decade(m->frame_build_failures)) {
		wlr_log(WLR_ERROR, "AVK could not build a frame for %s; retaining the "
			"displayed buffer and retrying (%" PRIu64 " consecutive failures)",
			m->wlr_output->name, m->frame_build_failures);
	}
	az_output_commit_failed(m);
	return false;
}

/* One commit boundary for composition and direct scanout. Failed commits
 * discard only this attempt's feedback and never claim backend ownership. */
static bool az_output_commit_frame(Monitor *m, struct wlr_output_state *state) {
	wlr_scene_output_prepare_gamma(m->scene_output, state);
	uint32_t seq = m->wlr_output->commit_seq + 1;
	struct az_output_feedback *sample, *tmp;
	wl_list_for_each(sample, &m->presentation_feedback, link) {
		if (!sample->committed) {
			sample->commit_seq = seq;
			sample->committed = true;
		}
	}
	bool landed = wlr_output_commit_state(m->wlr_output, state);
	/* Direct-scanout buffers have no AVK target addon. */
	struct wlr_addon *addon = state->buffer
		? wlr_addon_find(&state->buffer->addons, &avk, &az_avk_target_addon_impl) : NULL;
	if (addon != NULL) {
		struct az_avk_target *target = wl_container_of(addon, target, addon);
		target->state = landed ? AZ_AVK_TARGET_IN_FLIGHT : AZ_AVK_TARGET_RENDERED;
		target->release_point = landed ? state->signal_point : 0;
	}
	if (landed && !state->tearing_page_flip && m->tear_retry_synced) {
		m->tear_busy_synced++;
		m->tear_retry_synced = false;
	}
	if (!landed) {
		wl_list_for_each_safe(sample, tmp, &m->presentation_feedback, link) {
			if (sample->committed && sample->commit_seq == seq) {
				wl_list_remove(&sample->link);
				wlr_presentation_feedback_destroy(sample->feedback);
				free(sample);
			}
		}
	}
	return landed;
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
