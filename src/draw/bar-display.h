#ifndef ASTEROIDZ_BAR_DISPLAY_H
#define ASTEROIDZ_BAR_DISPLAY_H

#include "../common/kdl-edit.h"

/* Monitor status and per-output controls.
 *
 * The waybar plugin this replaces shells out to `amsg get all-monitors` to
 * learn the topology and `amsg dispatch` / a rewritten monitors.kdl +
 * reload_config to change it. None of that indirection applies here: we ARE
 * the compositor. The monitor list is `mons`, the fields are on Monitor, and a
 * toggle is a field write plus a scheduled frame -- no JSON, no subprocess, no
 * round trip through our own IPC.
 *
 * What this covers: the pill (focused output, its mode) and a popover listing
 * every output, drilling into per-output HDR, resolution and scale, with SDR
 * white and the drag-to-arrange canvas on the top level -- everything the
 * plugin offered.
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

/* ─── arrangement ─────────────────────────────────────────────────────────── */

/* Move an output to a new layout position, live.
 *
 * This is the whole apply path: wlroots owns the layout, and updatemons()
 * recomputes every Monitor's geometry, re-arranges its clients and rebuilds
 * its bar from it. Exactly what the wlr-output-management path does when
 * wlr-randr moves a head, so a drag and a `wlr-randr --pos` end in the same
 * place. */
static void bar_display_move(Monitor *m, int32_t x, int32_t y) {
	if (!m || !m->wlr_output || !m->wlr_output->enabled)
		return;
	if (m->m.x == x && m->m.y == y)
		return;
	wlr_output_layout_add(output_layout, m->wlr_output, x, y);
	updatemons(NULL, NULL);
}

/* Pull `box` onto its neighbours' edges when it lands close to one.
 *
 * Two monitors a few pixels apart is not an arrangement anyone wants: the gap
 * is dead space the pointer crosses instantly and no window can occupy, and an
 * overlap is worse. The threshold is in LAYOUT pixels and generous, because
 * the canvas is drawn at a fraction of real size -- a pixel of drag is tens of
 * layout pixels, so without snapping flush is a value you can only hit by
 * luck.
 *
 * Both axes are considered independently, and for each the candidate edges are
 * the neighbour's opposite side (butt them together) and its matching side
 * (line them up). */
static void bar_display_snap(const Monitor *self, struct wlr_box *box) {
	const int32_t threshold = 120;
	int32_t best_dx = threshold + 1, best_dy = threshold + 1;
	int32_t snap_x = box->x, snap_y = box->y;

	Monitor *o = NULL;
	wl_list_for_each(o, &mons, link) {
		if (o == self || !o->wlr_output || !o->wlr_output->enabled)
			continue;
		int32_t cand_x[4] = {o->m.x + o->m.width, o->m.x - box->width, o->m.x,
							 o->m.x + o->m.width - box->width};
		for (size_t i = 0; i < LENGTH(cand_x); i++) {
			int32_t d = box->x - cand_x[i];
			if (d < 0)
				d = -d;
			if (d < best_dx) {
				best_dx = d;
				snap_x = cand_x[i];
			}
		}
		int32_t cand_y[4] = {o->m.y + o->m.height, o->m.y - box->height, o->m.y,
							 o->m.y + o->m.height - box->height};
		for (size_t i = 0; i < LENGTH(cand_y); i++) {
			int32_t d = box->y - cand_y[i];
			if (d < 0)
				d = -d;
			if (d < best_dy) {
				best_dy = d;
				snap_y = cand_y[i];
			}
		}
	}
	if (best_dx <= threshold)
		box->x = snap_x;
	if (best_dy <= threshold)
		box->y = snap_y;
}

/* ─── persisting an arrangement ───────────────────────────────────────────── */

/* Write every enabled output's current position back into whichever config
 * file already declares it.
 *
 * Explicit, never automatic: a drag applies live, and only this saves. Moving
 * a monitor and rewriting the user's config in the same gesture would mean a
 * misdrag edits a file, and the compositor has no undo to offer.
 *
 * An output with no block anywhere is SKIPPED rather than invented. Where a
 * rule should live is a question about how someone has chosen to organise
 * their config, and guessing it wrong writes a rule into a file that a sourced
 * one then overrides -- a setting that does nothing, for reasons invisible in
 * the file you are looking at.
 *
 * Returns how many outputs were written; `missing` (optional) gets how many
 * had nowhere to go. */
static int32_t bar_display_save_positions(int32_t *missing) {
	int32_t saved = 0, absent = 0;
	Monitor *m = NULL;
	wl_list_for_each(m, &mons, link) {
		if (!m->wlr_output || !m->wlr_output->enabled || !m->wlr_output->name)
			continue;
		bool placed = false;
		for (int32_t i = 0; i < nconfig_files && !placed; i++) {
			FILE *f = fopen(config_files[i], "re");
			if (!f)
				continue;
			fseek(f, 0, SEEK_END);
			long sz = ftell(f);
			fseek(f, 0, SEEK_SET);
			if (sz < 0 || sz > 4 * 1024 * 1024) {
				fclose(f);
				continue;
			}
			char *text = malloc((size_t)sz + 1);
			if (!text) {
				fclose(f);
				continue;
			}
			size_t rd = fread(text, 1, (size_t)sz, f);
			text[rd] = '\0';
			fclose(f);

			char *doc = kdl_rewrite_output_pos(text, m->wlr_output->name,
												m->m.x, m->m.y);
			free(text);
			if (!doc)
				continue;

			/* temp + fsync + rename, like the medication store: a config
			 * truncated by a crash mid-write is a session that will not start */
			char tmp[1088];
			snprintf(tmp, sizeof(tmp), "%s.tmp", config_files[i]);
			FILE *out = fopen(tmp, "we");
			if (out) {
				size_t len = strlen(doc);
				bool ok = fwrite(doc, 1, len, out) == len;
				if (ok)
					ok = fflush(out) == 0 && fsync(fileno(out)) == 0;
				fclose(out);
				if (ok && rename(tmp, config_files[i]) == 0) {
					placed = true;
					saved++;
				} else {
					unlink(tmp);
				}
			}
			free(doc);
		}
		if (!placed) {
			absent++;
			wlr_log(WLR_INFO,
					"bar: no `output %s` block in any config file; its "
					"position was not saved",
					m->wlr_output->name);
		}
	}
	if (missing)
		*missing = absent;
	wlr_log(WLR_INFO, "bar: saved %d output position(s), %d had nowhere to go",
			saved, absent);
	return saved;
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
