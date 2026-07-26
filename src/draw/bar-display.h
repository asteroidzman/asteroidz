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
 * listing every output, drilling into per-output HDR. What it does NOT cover
 * is the plugin's layout canvas, resolution picker, scale and luminance
 * fields -- those need a drag surface, dropdowns and sliders, and the popover
 * layer has exactly one widget: a clickable text row. Those are widget work
 * on the popover layer, not module work, and are tracked separately rather
 * than faked with rows that only look like controls.
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
