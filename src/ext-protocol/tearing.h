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

/*
 * ── DOES THIS CLIENT ASK TO TEAR? ─────────────────────────────────────────
 *
 * M13. Per-client and nothing else: no focus in it, no output, no global
 * setting. Extracted because check_tearing_frame_allow() answers a different
 * question -- "is the compositor going to tear THIS frame" -- which depends on
 * which window is focused, and the inspector needs the per-window half to
 * report anything truthful about a window that is not currently focused.
 *
 * VIDEO NEVER TEARS, in both modes. It used to be excluded only in the
 * fullscreen-only mode: with `allow_tearing 1` a video client that set a
 * tearing hint would tear, despite the comment two lines away saying video
 * players must never. Unifying the two branches fixes that, and it is a
 * behaviour change rather than a refactor -- stated here because it is one.
 *
 * An explicit force_tearing still wins over everything, including VIDEO: it is
 * the operator overriding the classification on purpose.
 */
static inline bool client_tearing_eligible(Client *c) {
	if (c == NULL) {
		return false;
	}
	if (c->force_tearing != STATE_UNSPECIFIED) {
		return c->force_tearing == STATE_ENABLED;
	}
	enum az_present_class pc = az_present_class_of(c, NULL);
	if (pc == AZ_PRESENT_CLASS_VIDEO) {
		return false;
	}
	/* A tearing hint is the client asking through the protocol; GAME is the
	 * class asking on its behalf. Either counts as a request. */
	return c->tearing_hint || pc == AZ_PRESENT_CLASS_GAME;
}

bool check_tearing_frame_allow(Monitor *m) {
	/* never allow tearing when disabled */
	if (!config.allow_tearing) {
		return false;
	}

	Client *c = selmon != NULL ? selmon->sel : NULL;

	/* tearing is only allowed for the output with the active client */
	if (!c || c->mon != m) {
		return false;
	}

	/* TEARING_FULLSCREEN_ONLY additionally requires fullscreen; TEARING_ENABLED
	 * allows any window that asks. */
	if (config.allow_tearing != TEARING_ENABLED && !c->isfullscreen) {
		return false;
	}

	return client_tearing_eligible(c);
}

/*
 * ── A TORN FRAME, BUILT BY AVK ────────────────────────────────────────────
 *
 * M13. This aborted. Since 13254aad removed the SceneFX composition it could no
 * longer route to, `allow_tearing` plus any window that asked to tear killed
 * the compositor -- a crash reachable from configuration alone, and M13's GAME
 * class widens who reaches it, since a game no longer needs a per-app rule.
 *
 * The abort was the honest thing to leave at the time: the old path composited
 * every torn frame on SceneFX's GLES renderer even in an AVK session, silently,
 * and only for the applications a rule marked force_tearing -- exactly the ones
 * being watched for frame-timing problems. Deleting it beat leaving it.
 *
 * What replaces it is the ordinary AVK frame with one bit set. There is no
 * second renderer and no separate tearing pipeline: az_output_build_frame() is
 * the same seam every other frame goes through, and `tearing_page_flip` asks
 * the backend to flip without waiting for vblank.
 *
 * THE FLIP IS TESTED BEFORE IT IS COMMITTED, and refused gracefully. Not every
 * state is tearable -- a modeset, a format change, or a backend without
 * immediate flips will reject it -- and a torn frame that cannot be torn should
 * be presented on the next vblank, not dropped. Losing the tear is a latency
 * regression; losing the frame is a visible stall.
 */
void apply_tear_state(Monitor *m) {
	if (m == NULL || m->wlr_output == NULL || m->scene_output == NULL) {
		return;
	}
	if (!wlr_scene_output_needs_frame(m->scene_output)) {
		return;
	}

	struct wlr_output_state state;
	wlr_output_state_init(&state);

	/*
	 * ── A TORN FRAME CAN SCAN OUT TOO ─────────────────────────────────────
	 *
	 * M13B shipped with these mutually exclusive: rendermon takes the tearing
	 * branch before the branch that tries scanout, and returns. So a window
	 * that tears never scanned out -- and a tearing fullscreen game is exactly
	 * the case that most wants it. Both want the same thing, latency, and
	 * getting one silently cost the other.
	 *
	 * Scanout first, because it is the cheaper frame: if the client's buffer IS
	 * the picture, there is nothing to composite and the torn flip carries that
	 * buffer directly.
	 */
	enum az_scanout_verdict sv = AZ_SCANOUT_NOT_EVALUATED;
	bool scanned_out = az_scanout_try(m, &state, &sv);
	m->scanout_verdict = (int32_t)sv;
	if (scanned_out) {
		m->scanout_frames++;
	} else {
		struct az_frame_options frame_options = {
			.color_transform = az_output_color_transform(m),
		};
		if (!az_output_build_frame(m, &state, &frame_options)) {
			wlr_log(WLR_ERROR, "tearing: failed to build frame for %s",
				m->wlr_output->name);
			wlr_output_state_finish(&state);
			return;
		}
	}

	state.tearing_page_flip = true;
	if (!wlr_output_test_state(m->wlr_output, &state)) {
		/* Present it on the vblank instead. Once per state change rather than
		 * per frame would be quieter, but a torn frame silently not tearing is
		 * exactly the kind of thing that gets measured and disbelieved. */
		wlr_log(WLR_DEBUG, "tearing: %s refused a torn flip; presenting synced",
			m->wlr_output->name);
		state.tearing_page_flip = false;
	}

	if (!wlr_output_commit_state(m->wlr_output, &state)) {
		wlr_log(WLR_ERROR, "tearing: failed to commit frame for %s",
			m->wlr_output->name);
		az_output_commit_failed(m);
	}
	wlr_output_state_finish(&state);

	/* Same debt the composition path settles: scanout leaves the scene's
	 * pending damage untouched, and an output that never stops needing a frame
	 * re-scans-out at the panel's maximum rate forever. */
	if (scanned_out) {
		pixman_region32_clear(&m->scene_output->pending_commit_damage);
		struct timespec sdone;
		clock_gettime(CLOCK_MONOTONIC, &sdone);
		wlr_scene_output_send_frame_done(m->scene_output, &sdone);
	}
}
