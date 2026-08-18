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
	if (!scanned_out) {
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

	bool asked_torn = true;
	state.tearing_page_flip = true;
	if (!wlr_output_test_state(m->wlr_output, &state)) {
		/* Present it on the vblank instead. A torn frame silently not tearing
		 * is exactly the kind of thing that gets measured and disbelieved, so
		 * it is still said out loud -- but by a counter plus a decade of log
		 * lines, not by one line per frame. */
		asked_torn = false;
		state.tearing_page_flip = false;
		m->tear_test_refused++;
		if (az_log_decade(m->tear_test_refused)) {
			wlr_log(WLR_DEBUG, "tearing: %s refused a torn flip; presenting "
				"synced (%" PRIu64 " so far)",
				m->wlr_output->name, m->tear_test_refused);
		}
	}

	bool landed = wlr_output_commit_state(m->wlr_output, &state);

	/*
	 * ── A TEST THAT PASSED IS NOT A COMMIT THAT WILL ──────────────────────
	 *
	 * The test above answers "is this state tearable", and the kernel's async
	 * check asks a second question it cannot: is the PREVIOUS flip on this
	 * plane finished in hardware. If it is not, an immediate flip comes back
	 * EBUSY -- and since a torn frame is submitted without waiting for vblank,
	 * that overlap is a normal consequence of going fast, not a broken state.
	 * wlroots documents exactly this gap: a passing wlr_output_test_state()
	 * promises only that the commit can still fail "due to a runtime error".
	 *
	 * So retry the same frame synced, which is what the refusal path above
	 * already does for the case a test CAN see. Losing the tear is a latency
	 * regression; losing the frame is a visible stall -- this file has said so
	 * since it was written, and then dropped 24943 frames in one session for
	 * want of these four lines. The state is untouched by a failed commit
	 * (wlr_output_commit_state() is atomic), so it can simply go again.
	 */
	if (!landed && asked_torn) {
		state.tearing_page_flip = false;
		landed = wlr_output_commit_state(m->wlr_output, &state);
		if (landed) {
			m->tear_busy_synced++;
			if (az_log_decade(m->tear_busy_synced)) {
				wlr_log(WLR_DEBUG, "tearing: %s refused the torn flip at "
					"commit; presented synced (%" PRIu64 " so far)",
					m->wlr_output->name, m->tear_busy_synced);
			}
		}
	}

	bool torn = landed && state.tearing_page_flip;
	if (!landed) {
		m->tear_dropped++;
		if (az_log_decade(m->tear_dropped)) {
			wlr_log(WLR_ERROR, "tearing: failed to commit frame for %s "
				"(%" PRIu64 " dropped)",
				m->wlr_output->name, m->tear_dropped);
		}
		az_output_commit_failed(m);
	} else {
		if (torn) {
			m->tear_torn++;
		}
		if (scanned_out) {
			m->scanout_frames++;
		}
	}
	wlr_output_state_finish(&state);

	/*
	 * Same debt the composition path settles: scanout leaves the scene's
	 * pending damage untouched, and an output that never stops needing a frame
	 * re-scans-out at the panel's maximum rate forever.
	 *
	 * ONLY WHEN THE FRAME LANDED. Settling it for a commit that failed pays a
	 * debt out of a frame nobody saw: the damage is forgotten, so the dropped
	 * picture is never redrawn, and frame-done tells the client its buffer is
	 * finished with when the display never took it. A client that tracks its
	 * own buffer lifetimes -- gamescope does, and says "compositor released us
	 * but we were not acquired" when the accounting disagrees -- is entitled to
	 * be confused by that.
	 */
	if (landed && scanned_out) {
		pixman_region32_clear(&m->scene_output->pending_commit_damage);
		struct timespec sdone;
		clock_gettime(CLOCK_MONOTONIC, &sdone);
		wlr_scene_output_send_frame_done(m->scene_output, &sdone);
	}
}
