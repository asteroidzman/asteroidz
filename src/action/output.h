#ifndef ASTEROIDZ_ACTION_OUTPUT_H
#define ASTEROIDZ_ACTION_OUTPUT_H

/* Output configuration as dispatches.
 *
 * This is the native bar's display popover, turned inside out. That code
 * (draw/bar-display.h) could change a mode, a scale or a position because it
 * ran INSIDE the compositor and could call wlroots directly; a bar in another
 * process cannot, and "the settings panel moved out" must not mean "the
 * settings stopped working".
 *
 * So the apply paths move here rather than being deleted with the bar: same
 * test-then-commit, same retrain safety net, same reasons -- reachable now
 * from `amsg dispatch` and therefore from anything.
 *
 *   dispatch set_output_mode,DP-1,2560x1440@144
 *   dispatch set_output_scale,DP-1,1.25
 *   dispatch set_output_position,DP-1,1920,0
 *   dispatch set_output_vrr,DP-1,1
 *   dispatch set_output_icc,DP-1,/path/to/profile.icm
 *
 * Every one names its output. The bar's versions acted on "the monitor the
 * popover was opened against", which is a piece of UI state; a dispatch has no
 * such context and guessing selmon would make `set_output_scale` do something
 * different depending on where the pointer happened to be.
 */

static Monitor *output_by_name_or_focus(const char *name) {
	Monitor *m;
	if (!name || !*name)
		return selmon;
	wl_list_for_each(m, &mons, link) {
		if (m->wlr_output && m->wlr_output->name &&
			strcmp(m->wlr_output->name, name) == 0)
			return m;
	}
	return NULL;
}

/* Apply a mode and/or a scale to one output.
 *
 * TESTED before it is committed. A mode the panel cannot take, or a scale that
 * makes the logical size degenerate, fails the test and the output is left
 * exactly as it was -- a picker must not be able to black out a display, and a
 * rejected commit is the retrain-and-blank path.
 *
 * Committed out of band rather than folded into the next frame, unlike the HDR
 * toggle: a modeset is a deliberate, rare, user-initiated action, and this is
 * the same thing wlr-randr does through outputmgrapplyortest(). The HDR path is
 * deferred because it fires often enough to collide with an in-flight
 * page-flip; picking a resolution does not.
 *
 * `mode` NULL means "leave the mode alone", `scale <= 0` means "leave the
 * scale alone", so one function serves both. */
static bool output_apply(Monitor *m, struct wlr_output_mode *mode,
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
		wlr_log(WLR_ERROR, "output: %s rejected %dx%d@%d scale %.2f",
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
	wlr_log(WLR_INFO, "output: %s -> %dx%d@%d scale %.2f", m->wlr_output->name,
			m->wlr_output->width, m->wlr_output->height,
			(int32_t)((m->wlr_output->refresh + 500) / 1000),
			(double)m->wlr_output->scale);
	printstatus(IPC_WATCH_ARRANGGE);
	return true;
}

/* "2560x1440@144" or "2560x1440". Matched against the modes the output
 * actually reports rather than synthesised: a mode wlroots did not advertise
 * cannot be committed, and inventing one only moves the failure later. */
static struct wlr_output_mode *output_find_mode(Monitor *m, const char *spec) {
	if (!m || !m->wlr_output || !spec)
		return NULL;
	int32_t w = 0, h = 0, hz = 0;
	if (sscanf(spec, "%dx%d@%d", &w, &h, &hz) < 2)
		return NULL;

	struct wlr_output_mode *mode, *best = NULL;
	wl_list_for_each(mode, &m->wlr_output->modes, link) {
		if (mode->width != w || mode->height != h)
			continue;
		if (hz > 0) {
			/* the reported refresh is in mHz and rarely round: 143999 is
			 * "144", so compare at whole Hz rather than demanding equality */
			if ((int32_t)((mode->refresh + 500) / 1000) != hz)
				continue;
			return mode;
		}
		/* no rate asked for: the highest this resolution offers */
		if (!best || mode->refresh > best->refresh)
			best = mode;
	}
	return best;
}

int32_t set_output_mode(const Arg *arg) {
	if (!arg || !arg->v)
		return 0;
	Monitor *m = output_by_name_or_focus((const char *)arg->v);
	if (!m)
		return 0;
	struct wlr_output_mode *mode = output_find_mode(m, (const char *)arg->v2);
	if (!mode) {
		wlr_log(WLR_ERROR, "set_output_mode: %s has no mode %s",
				m->wlr_output->name, arg->v2 ? (const char *)arg->v2 : "");
		return 0;
	}
	return output_apply(m, mode, 0.0f) ? 1 : 0;
}

int32_t set_output_scale(const Arg *arg) {
	if (!arg || !arg->v)
		return 0;
	Monitor *m = output_by_name_or_focus((const char *)arg->v);
	if (!m || arg->f <= 0.0f)
		return 0;
	return output_apply(m, NULL, arg->f) ? 1 : 0;
}

/* Move an output to a new layout position, live.
 *
 * This is the whole apply path: wlroots owns the layout, and updatemons()
 * recomputes every Monitor's geometry and re-arranges its clients from it.
 * Exactly what the wlr-output-management path does when wlr-randr moves a
 * head, so a drag in a settings panel and a `wlr-randr --pos` end in the same
 * place. */
int32_t set_output_position(const Arg *arg) {
	if (!arg || !arg->v)
		return 0;
	Monitor *m = output_by_name_or_focus((const char *)arg->v);
	if (!m || !m->wlr_output || !m->wlr_output->enabled)
		return 0;
	if (m->m.x == arg->i && m->m.y == arg->i2)
		return 1; /* already there; not a failure */
	wlr_output_layout_add(output_layout, m->wlr_output, arg->i, arg->i2);
	updatemons(NULL, NULL);
	printstatus(IPC_WATCH_ARRANGGE);
	return 1;
}

/* Adaptive sync, tested like everything else here: a panel that claims VRR and
 * then refuses the commit leaves the output alone rather than half-configured.
 * enable_adaptive_sync() already does that test itself. */
int32_t set_output_vrr(const Arg *arg) {
	if (!arg || !arg->v)
		return 0;
	Monitor *m = output_by_name_or_focus((const char *)arg->v);
	if (!m || !m->wlr_output)
		return 0;

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	if (arg->i)
		enable_adaptive_sync(m, &state);
	else
		disable_adaptive_sync(m, &state);
	m->vrr_global_enable = arg->i ? 1 : 0;
	bool ok = wlr_output_commit_state(m->wlr_output, &state);
	wlr_output_state_finish(&state);
	if (ok)
		printstatus(IPC_WATCH_ARRANGGE);
	return ok ? 1 : 0;
}

/* An ICC profile for SDR output. An empty path clears it, which is the only
 * way back to the untransformed pipeline once one is loaded. */
int32_t set_output_icc(const Arg *arg) {
	if (!arg || !arg->v)
		return 0;
	Monitor *m = output_by_name_or_focus((const char *)arg->v);
	if (!m)
		return 0;
	const char *path = arg->v2 ? (const char *)arg->v2 : "";
	mon_load_icc_profile(m, *path ? path : NULL);
	printstatus(IPC_WATCH_ARRANGGE);
	return 1;
}

#endif /* ASTEROIDZ_ACTION_OUTPUT_H */
