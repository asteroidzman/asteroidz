#include <wlr/types/wlr_tearing_control_v1.h>

struct tearing_controller {
	struct wlr_tearing_control_v1 *tearing_control;
	struct wl_listener set_hint;
	struct wl_listener destroy;
};

struct wlr_tearing_control_manager_v1 *tearing_control;
struct wl_listener tearing_new_object;

static void handle_controller_set_hint(struct wl_listener *listener,
									   void *data) {
	struct tearing_controller *controller =
		wl_container_of(listener, controller, set_hint);
	Client *c = NULL;

	toplevel_from_wlr_surface(controller->tearing_control->surface, &c, NULL);

	if (c) {
		/*
		 * tearing_control->current is actually an enum:
		 * WP_TEARING_CONTROL_V1_PRESENTATION_HINT_VSYNC = 0
		 * WP_TEARING_CONTROL_V1_PRESENTATION_HINT_ASYNC = 1
		 *
		 * Using it as a bool here allows us to not ship the XML.
		 */
		c->tearing_hint = controller->tearing_control->current;
	}
}

static void handle_controller_destroy(struct wl_listener *listener,
									  void *data) {
	struct tearing_controller *controller =
		wl_container_of(listener, controller, destroy);
	wl_list_remove(&controller->set_hint.link);
	wl_list_remove(&controller->destroy.link);
	free(controller);
}

void handle_tearing_new_object(struct wl_listener *listener, void *data) {
	struct wlr_tearing_control_v1 *new_tearing_control = data;

	enum wp_tearing_control_v1_presentation_hint hint =
		wlr_tearing_control_manager_v1_surface_hint_from_surface(
			tearing_control, new_tearing_control->surface);
	wlr_log(WLR_DEBUG, "New presentation hint %d received for surface %p", hint,
			new_tearing_control->surface);

	struct tearing_controller *controller =
		ecalloc(1, sizeof(struct tearing_controller));
	controller->tearing_control = new_tearing_control;

	controller->set_hint.notify = handle_controller_set_hint;
	wl_signal_add(&new_tearing_control->events.set_hint, &controller->set_hint);

	controller->destroy.notify = handle_controller_destroy;
	wl_signal_add(&new_tearing_control->events.destroy, &controller->destroy);
}

bool check_tearing_frame_allow(Monitor *m) {
	/* never allow tearing when disabled */
	if (!config.allow_tearing) {
		return false;
	}

	Client *c = selmon->sel;

	/* tearing is only allowed for the output with the active client */
	if (!c || c->mon != m) {
		return false;
	}

	/* allow tearing for any window when requested or forced; surfaces
	 * tagged as games via content-type-v1 count as a request */
	if (config.allow_tearing == TEARING_ENABLED) {
		if (c->force_tearing == STATE_UNSPECIFIED) {
			return c->tearing_hint || client_content_type_is_game(c);
		} else {
			return c->force_tearing == STATE_ENABLED;
		}
	}

	/* remaining tearing options apply only to full-screen windows */
	if (!c->isfullscreen) {
		return false;
	}

	if (c->force_tearing == STATE_UNSPECIFIED) {
		/* only tear for windows that ask for it (tearing hint or a game
		 * content-type); video players like mpv must never tear */
		if (client_content_type_is_video(c))
			return false;
		return c->tearing_hint || client_content_type_is_game(c);
	}

	/* honor tearing as requested by action */
	return c->force_tearing == STATE_ENABLED;
}

bool custom_wlr_scene_output_commit(struct wlr_scene_output *scene_output,
									struct wlr_output_state *state) {
	struct wlr_output *wlr_output = scene_output->output;
	Monitor *m = wlr_output->data;

	// check whether a frame is needed
	if (!wlr_scene_output_needs_frame(scene_output)) {
		wlr_log(WLR_DEBUG, "No frame needed for output %s", wlr_output->name);
		return true;
	}

	// build the output state
	struct wlr_scene_output_state_options icc_options = {
		.color_transform = wlr_output->image_description == NULL
			? m->icc_transform
			: NULL,
	};
	if (!wlr_scene_output_build_state(scene_output, state, &icc_options)) {
		wlr_log(WLR_ERROR, "Failed to build output state for %s",
				wlr_output->name);
		return false;
	}

	// test the tearing page flip
	if (state->tearing_page_flip) {
		if (!wlr_output_test_state(wlr_output, state)) {
			state->tearing_page_flip = false;
		}
	}

	// attempt to commit
	bool committed = wlr_output_commit_state(wlr_output, state);

	// if tearing page flip is enabled but the commit fails, retry with it disabled
	if (!committed && state->tearing_page_flip) {
		wlr_log(WLR_DEBUG, "Retrying commit without tearing for %s",
				wlr_output->name);
		state->tearing_page_flip = false;
		committed = wlr_output_commit_state(wlr_output, state);
	}

	// handle state cleanup
	if (committed) {
		wlr_log(WLR_DEBUG, "Successfully committed output %s",
				wlr_output->name);
		if (state == &m->pending) {
			wlr_output_state_finish(&m->pending);
			wlr_output_state_init(&m->pending);
		}
	} else {
		wlr_log(WLR_ERROR, "Failed to commit output %s", wlr_output->name);
		// clean up state even on commit failure, to avoid buildup
		if (state == &m->pending) {
			wlr_output_state_finish(&m->pending);
			wlr_output_state_init(&m->pending);
		}
		return false;
	}

	return true;
}

void apply_tear_state(Monitor *m) {
	if (wlr_scene_output_needs_frame(m->scene_output)) {
		wlr_output_state_init(&m->pending);
		struct wlr_scene_output_state_options icc_options = {
			.color_transform =
				m->wlr_output->image_description == NULL ? m->icc_transform
														 : NULL,
		};
		if (wlr_scene_output_build_state(m->scene_output, &m->pending,
										 &icc_options)) {
			struct wlr_output_state *pending = &m->pending;
			pending->tearing_page_flip = true;

			if (!custom_wlr_scene_output_commit(m->scene_output, pending)) {
				wlr_log(WLR_ERROR, "Failed to commit output %s",
						m->scene_output->output->name);
			}
		} else {
			wlr_log(WLR_ERROR, "Failed to build state for output %s",
					m->scene_output->output->name);
			wlr_output_state_finish(&m->pending);
		}
	}
}