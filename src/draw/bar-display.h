#ifndef ASTEROIDZ_BAR_DISPLAY_H
#define ASTEROIDZ_BAR_DISPLAY_H

/* Monitor status and per-output controls.
 *
 * The waybar plugin this replaces shells out to `amsg get all-monitors` to
 * learn the topology and `amsg dispatch` / a rewritten monitors.kdl +
 * reload_config to change it. None of that indirection applies here: we ARE
 * the compositor. The monitor list is `mons`, the fields are on Monitor, and a
 * toggle is a field write plus a scheduled frame -- no JSON, no subprocess, no
 * round trip through our own IPC.
 *
 * What this covers today: the pill (focused output, its mode) and a popover
 * listing every output, drilling into per-output HDR, resolution and scale,
 * with SDR white on the top level. What it does NOT cover is the plugin's
 * layout canvas -- dragging monitors around needs a genuine drag surface, not
 * a row, so it is tracked separately rather than faked with rows that only
 * look like controls.
 */

/* Refresh in Hz from wlroots' millihertz, rounded the way a spec sheet would
 * print it (59.951 -> 60). */
static int32_t bar_display_hz(const Monitor *m) {
	if (!m || !m->wlr_output || m->wlr_output->refresh <= 0)
		return 0;
	return (int32_t)((m->wlr_output->refresh + 500) / 1000);
}

static Monitor *bar_display_by_name(const char *name) {
	Monitor *m = NULL;
	if (!name || !*name)
		return NULL;
	wl_list_for_each(m, &mons, link) {
		if (m->wlr_output && m->wlr_output->name &&
			strcmp(m->wlr_output->name, name) == 0)
			return m;
	}
	return NULL;
}

/* One line describing an output, as the popover row and the pill both want it. */
static void bar_display_summary(const Monitor *m, char *out, size_t len) {
	if (!m || !m->wlr_output) {
		snprintf(out, len, "%s", "no output");
		return;
	}
	int32_t hz = bar_display_hz(m);
	if (hz > 0)
		snprintf(out, len, "%s  %dx%d@%d", m->wlr_output->name, m->m.width,
				 m->m.height, hz);
	else
		snprintf(out, len, "%s  %dx%d", m->wlr_output->name, m->m.width,
				 m->m.height);
}

/* Apply a mode and/or a scale to one output.
 *
 * TESTED before it is committed. A mode the panel cannot take, or a scale that
 * makes the logical size degenerate, fails the test and we leave the output
 * exactly as it was -- a popover that can pick a value must not be able to
 * black out a display, and a rejected commit is the retrain-and-blank path.
 *
 * Committed out of band rather than folded into the next frame, unlike the HDR
 * toggle: a modeset is a deliberate, rare, user-initiated action, and this is
 * the same thing wlr-randr does through outputmgrapplyortest(). The HDR path is
 * deferred because it fires often enough to collide with an in-flight
 * page-flip; picking a resolution does not.
 *
 * `mode` NULL means "leave the mode alone", `scale <= 0` means "leave the scale
 * alone", so the same function serves both pickers. */
static bool bar_display_apply(Monitor *m, struct wlr_output_mode *mode,
							  float scale) {
	if (!m || !m->wlr_output || (!mode && scale <= 0.0f))
		return false;

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	if (mode)
		wlr_output_state_set_mode(&state, mode);
	if (scale > 0.0f)
		wlr_output_state_set_scale(&state, scale);

	bool ok = wlr_output_test_state(m->wlr_output, &state);
	if (ok)
		ok = wlr_output_commit_state(m->wlr_output, &state);
	wlr_output_state_finish(&state);

	if (!ok) {
		wlr_log(WLR_ERROR, "bar: %s rejected %dx%d@%d scale %.2f",
				m->wlr_output->name, mode ? mode->width : 0,
				mode ? mode->height : 0,
				mode ? (int32_t)((mode->refresh + 500) / 1000) : 0,
				(double)scale);
		/* same safety net the output-management path uses: a failed commit can
		 * leave a DSC panel wedged, and a retrain is cheaper than a dead
		 * output */
		monitor_start_retrain(m, 50);
		return false;
	}
	wlr_log(WLR_INFO, "bar: %s -> %dx%d@%d scale %.2f", m->wlr_output->name,
			m->wlr_output->width, m->wlr_output->height,
			(int32_t)((m->wlr_output->refresh + 500) / 1000),
			(double)m->wlr_output->scale);
	printstatus(IPC_WATCH_ARRANGGE);
	return true;
}

/* The scales a picker offers. Fractional scaling is real here (this desktop
 * runs an output at 0.75), so the list spans below 1 as well as above, and
 * stays short enough to read at a glance rather than enumerating every
 * hundredth. */
static const float bar_display_scales[] = {0.75f, 1.0f, 1.25f, 1.5f,
										   1.75f, 2.0f};

/* Flip HDR on a SPECIFIC output.
 *
 * Deliberately not toggle_hdr(): that dispatcher acts on selmon by design, and
 * a popover row names the output it belongs to -- clicking "HDMI-A-1" while
 * DP-1 is focused must not flip DP-1. Same three steps it performs, applied to
 * the monitor the row identifies.
 *
 * The commit is DEFERRED to the next frame rather than issued here: an
 * out-of-band commit races an in-flight page-flip and gets rejected by the DRM
 * backend, which is exactly the retrain-and-blank path seen live. */
static void bar_display_toggle_hdr(Monitor *m) {
	if (!m || !m->wlr_output)
		return;
	m->hdr_forced_off_for_capture = false;
	m->hdr = !m->hdr;
	m->hdr_pending_change = true;
	wlr_output_schedule_frame(m->wlr_output);
	wlr_log(WLR_INFO, "HDR %s on %s (bar)", m->hdr ? "enabled" : "disabled",
			m->wlr_output->name);
	printstatus(IPC_WATCH_ARRANGGE);
}

#endif /* ASTEROIDZ_BAR_DISPLAY_H */
