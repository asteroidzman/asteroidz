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

void apply_tear_state(Monitor *m) {
	if (!wlr_scene_output_needs_frame(m->scene_output)) {
		return;
	}
	/*
	 * ── THE TEARING PATH NEVER WENT THROUGH AVK ──────────────────────────
	 *
	 * This used to call wlr_scene_output_build_state() directly, so every torn
	 * frame was composited by SceneFX on wlroots' GLES renderer even in an AVK
	 * session -- silently, and only for the applications a window rule marks
	 * force_tearing, which are exactly the ones being watched for frame-timing
	 * problems.
	 *
	 * AVK implements no tearing page flip, and there is no longer a second
	 * compositor to route this to, so the SceneFX path that used to follow is
	 * gone rather than merely unreachable. Implementing a tearing page flip in
	 * AVK is what makes force_tearing work again; nothing else will.
	 */
	wlr_log(WLR_ERROR,
		"tearing is not implemented by AVK and %s asked for a torn frame. "
		"Remove force_tearing from the window rule, or implement a tearing "
		"page flip in AVK.",
		m->wlr_output != NULL ? m->wlr_output->name : "(output)");
	abort();
}
