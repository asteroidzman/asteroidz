#ifndef ASTEROIDZ_ACTION_OUTPUT_H
#define ASTEROIDZ_ACTION_OUTPUT_H

#include "../common/kdl-edit.h"
#include "../common/kdl-file.h"
#include <wlr/util/transform.h>

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

/* Write a setting back to whichever config file declares this output.
 *
 * Applying a mode and REMEMBERING it are two different features, and only the
 * first one existed: every dispatch here took effect immediately and was gone
 * at the next `reload_config`. The panel that drives them looked like it was
 * configuring a display and was really only nudging it until something else
 * touched the output.
 *
 * Which file is not a guess -- parse_config records every path it reads,
 * `source "./monitors.kdl"` included, precisely so a writer can find the one
 * that already declares a setting. Rewriting the rule into the main config
 * while a sourced file still sets it would produce a setting that silently
 * loses to the one you cannot see.
 *
 * The edit is SURGICAL (see kdl-edit.h): it replaces the bytes that hold these
 * values and copies everything else through, so comments, spacing and
 * everything the compositor does not model survive. A config regenerated from
 * the Monitor struct would be correct and would still be a betrayal of a file
 * someone maintains by hand.
 *
 * An output with no block anywhere is applied but NOT persisted, and says so.
 * Inventing a block means choosing a file and a place in it, and guessing that
 * about a hand-maintained config is worse than a warning that tells you to add
 * three words yourself. */
static bool output_persist(Monitor *m, const char *const *keys,
						   const char *const *vals, size_t nkeys) {
	if (!m || !m->wlr_output || !m->wlr_output->name)
		return false;
	const char *name = m->wlr_output->name;

	for (int32_t i = 0; i < nconfig_files; i++) {
		char *text = kdl_file_slurp(config_files[i]);
		if (!text)
			continue;

		char *out = kdl_rewrite_output_props(text, name, keys, vals, nkeys);
		free(text);
		if (!out)
			continue; /* this file does not declare that output */

		/* No backup here. An output setting is one number the user just chose
		 * on purpose, from a panel that shows them the current value -- there
		 * is nothing to undo that they cannot simply undo by choosing again.
		 * The settings write path asks for one, because applying a batch of
		 * changes across several options is a different kind of act. */
		bool ok = kdl_file_replace(config_files[i], out, false);
		if (!ok)
			wlr_log(WLR_ERROR, "output_persist: could not write %s",
					config_files[i]);
		free(out);
		return ok;
	}

	wlr_log(WLR_INFO,
			"output_persist: %s applied but not saved -- no `output %s { }` "
			"block in any config file",
			name, name);
	return false;
}

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
/* Move an output and remember where it went. No-op if it is already there --
 * output_persist rewrites a config file, and a reflow that touched nothing
 * should not rewrite anything either. */
static void output_place(Monitor *m, int32_t x, int32_t y) {
	if (!m || !m->wlr_output)
		return;
	wlr_output_layout_add(output_layout, m->wlr_output, x, y);

	char xs[16], ys[16];
	snprintf(xs, sizeof(xs), "%d", x);
	snprintf(ys, sizeof(ys), "%d", y);
	const char *keys[] = {"x", "y"};
	const char *vals[] = {xs, ys};
	output_persist(m, keys, vals, 2);
}

/* Keep the layout coherent when an output's LOGICAL SIZE changes.
 *
 * `x` is an absolute logical coordinate in monitors.kdl, and an output's
 * logical width is its mode divided by its scale. Those two facts do not
 * survive each other. Taking a real case: DP-1 is a 3840-wide panel, so it is
 * 2194 logical pixels at scale 1.75 and 2560 at 1.5, and HDMI-A-1 sitting
 * flush to its right belongs at a different x in each case. Nothing revisited
 * it, so changing the scale left HDMI-A-1 pinned at the previous scale's
 * number: 366 columns of overlap going one way, 366 of dead gap going the
 * other, and a different wrong number for every scale.
 *
 * Overlap is the bad half. Two outputs owning the same pixel is not a state
 * anything downstream can render: the wallpaper composited twice at two
 * scales with a seam down it, both bars drawing into the strip, and one
 * output's blur cache sampling the other's contents.
 *
 * ADJACENCY, not repacking. Everything past the resized output's old edge
 * shifts by the same delta, so an output that was flush stays flush and an
 * output someone deliberately offset keeps its offset. Repacking the layout
 * left-to-right would also have fixed the overlap, and would silently
 * flatten an arrangement that was chosen.
 *
 * Only outputs actually BESIDE the resized one move -- one stacked above or
 * below it shares no rows with it and has no reason to care how wide it got.
 *
 * Returns whether anything moved. */
static bool output_reflow(Monitor *resized, struct wlr_box before) {
	if (!resized || !resized->wlr_output)
		return false;

	struct wlr_box after;
	wlr_output_layout_get_box(output_layout, resized->wlr_output, &after);
	int32_t dx = (after.x + after.width) - (before.x + before.width);
	int32_t dy = (after.y + after.height) - (before.y + before.height);
	if (dx == 0 && dy == 0)
		return false;

	bool moved = false;
	Monitor *o;
	wl_list_for_each(o, &mons, link) {
		if (o == resized || !o->wlr_output || !o->wlr_output->enabled)
			continue;
		struct wlr_box b;
		wlr_output_layout_get_box(output_layout, o->wlr_output, &b);
		int32_t nx = b.x, ny = b.y;

		/* Past the right edge, and sharing rows with it. */
		if (dx != 0 && b.x >= before.x + before.width &&
			b.y < before.y + before.height && b.y + b.height > before.y)
			nx = b.x + dx;
		/* Past the bottom edge, and sharing columns with it. */
		if (dy != 0 && b.y >= before.y + before.height &&
			b.x < before.x + before.width && b.x + b.width > before.x)
			ny = b.y + dy;

		if (nx != b.x || ny != b.y) {
			output_place(o, nx, ny);
			moved = true;
		}
	}

	/* Backstop. The shift above keeps a coherent layout coherent; it cannot
	 * repair one that was not. A hand-edited monitors.kdl can put two outputs
	 * on the same pixels, and then growing one of them leaves them there --
	 * so anything still overlapping the resized output is pushed clear of it,
	 * whatever the file asked for. An unrenderable layout is not a preference
	 * worth honouring. */
	wl_list_for_each(o, &mons, link) {
		if (o == resized || !o->wlr_output || !o->wlr_output->enabled)
			continue;
		struct wlr_box b;
		wlr_output_layout_get_box(output_layout, o->wlr_output, &b);
		if (b.x >= after.x + after.width || b.x + b.width <= after.x)
			continue;
		if (b.y >= after.y + after.height || b.y + b.height <= after.y)
			continue;
		wlr_log(WLR_INFO, "output: %s overlapped %s; moving it clear",
				o->wlr_output->name, resized->wlr_output->name);
		output_place(o, after.x + after.width, b.y);
		moved = true;
	}

	return moved;
}

/* Make the whole layout renderable, wherever it came from.
 *
 * output_reflow above keeps things coherent when an output is RESIZED, and it
 * cannot help here: a config is applied to every output at once and nothing
 * was resized, so an overlap written in a file survives untouched. Its own
 * backstop only repairs outputs that overlap the one being resized, which on a
 * config load is none of them.
 *
 * That gap is how this was actually hit, and the sequence is worth keeping
 * because none of the steps is a mistake. Setting DP-1 to scale 1.75 through
 * the settings page reflows HDMI-A-1 to x=2194 and PERSISTS it, which is
 * correct -- 2194 is where flush is at that scale. Editing the scale back to
 * 1.5 by hand in monitors.kdl then makes DP-1 2560 logical pixels wide again,
 * while HDMI-A-1 is still at the 1.75-era 2194. Reported as "the adjacency
 * failed and hdmi was inside dp-1", which is exactly what it is: 366 columns of
 * two outputs owning the same pixels.
 *
 * A left-to-right sweep, so it is IDEMPOTENT: a layout with no overlap comes
 * out of this untouched, which matters because output_place persists, and a
 * pass that nudged things would rewrite monitors.kdl on every config reload.
 * Only the outputs that actually collide move, and each moves the minimum --
 * flush to the right edge of what it overlapped, keeping its own row.
 *
 * Overlap is resolved along X only. Two outputs sharing rows is the case that
 * arises from a scale change, and pushing on both axes at once turns one
 * collision into a diagonal shuffle whose result nobody can predict from the
 * file they just edited.
 *
 * Returns whether anything moved. */
bool output_resolve_overlaps(void) {
	/* Ascending x, so each output is only ever compared against ones already
	 * settled. wl_list order is creation order, which is not layout order. */
	Monitor *sorted[32];
	size_t n = 0;
	Monitor *m;
	wl_list_for_each(m, &mons, link) {
		if (!m->wlr_output || !m->wlr_output->enabled)
			continue;
		if (n == LENGTH(sorted))
			break;
		sorted[n++] = m;
	}
	for (size_t i = 1; i < n; i++) {
		Monitor *key = sorted[i];
		struct wlr_box kb;
		wlr_output_layout_get_box(output_layout, key->wlr_output, &kb);
		size_t j = i;
		while (j > 0) {
			struct wlr_box pb;
			wlr_output_layout_get_box(output_layout, sorted[j - 1]->wlr_output,
									  &pb);
			if (pb.x <= kb.x)
				break;
			sorted[j] = sorted[j - 1];
			j--;
		}
		sorted[j] = key;
	}

	bool moved = false;
	for (size_t i = 0; i < n; i++) {
		struct wlr_box b;
		wlr_output_layout_get_box(output_layout, sorted[i]->wlr_output, &b);

		/* The furthest right edge of anything already placed that this one
		 * runs into. Taking the furthest rather than the first keeps one move
		 * enough when an output overlaps two at once. */
		int32_t push = b.x;
		for (size_t k = 0; k < i; k++) {
			struct wlr_box p;
			wlr_output_layout_get_box(output_layout, sorted[k]->wlr_output, &p);
			if (b.x >= p.x + p.width || b.x + b.width <= p.x)
				continue;
			if (b.y >= p.y + p.height || b.y + b.height <= p.y)
				continue;
			if (p.x + p.width > push)
				push = p.x + p.width;
		}
		if (push != b.x) {
			wlr_log(WLR_INFO,
					"output: %s overlapped the layout at x=%d; moving to %d",
					sorted[i]->wlr_output->name, b.x, push);
			output_place(sorted[i], push, b.y);
			moved = true;
		}
	}
	return moved;
}

/* WHAT ONE COMMIT MAY CARRY.
 *
 * A struct rather than four parameters because M4F.2C.4e needs the mode and the
 * transform to change TOGETHER: a frame whose pending state carries both is the
 * one where the attachment extent and the presentation extent come from
 * different fields of the same wlr_output_state, and that is precisely the
 * combination M4F.2C.4d got wrong. Applying them as two commits never produces
 * that frame. */
struct output_change {
	struct wlr_output_mode *mode; /* NULL: leave the mode alone */
	int32_t custom_w, custom_h;	  /* >0: a custom mode, for outputs with no
								   * mode list (the headless backend) */
	float scale;				  /* <=0: leave the scale alone */
	int32_t transform;			  /* <0: leave the transform alone */
};

static bool output_apply_change(Monitor *m, const struct output_change *ch) {
	if (!m || !m->wlr_output || !ch)
		return false;
	if (!ch->mode && ch->custom_w <= 0 && ch->scale <= 0.0f &&
		ch->transform < 0)
		return false;
	struct wlr_output_mode *mode = ch->mode;
	float scale = ch->scale;

	/* Where it sat before, so the reflow below can tell what its edges moved
	 * by. Read from the LAYOUT rather than m->m, which updatemons() rewrites
	 * as a side effect of the commit that is about to happen. */
	struct wlr_box before;
	wlr_output_layout_get_box(output_layout, m->wlr_output, &before);

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	if (mode)
		wlr_output_state_set_mode(&state, mode);
	else if (ch->custom_w > 0 && ch->custom_h > 0)
		wlr_output_state_set_custom_mode(&state, ch->custom_w, ch->custom_h, 0);
	if (scale > 0.0f)
		wlr_output_state_set_scale(&state, scale);
	if (ch->transform >= 0)
		wlr_output_state_set_transform(&state,
									   (enum wl_output_transform)ch->transform);

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
	/* BOTH EXTENTS, because "the output is now 800x600" is only half an answer
	 * on a rotated output and the half that is missing is the one the renderer
	 * walks the scene through. */
	int32_t tw = m->wlr_output->width, th = m->wlr_output->height;
	wlr_output_transform_coords(m->wlr_output->transform, &tw, &th);
	wlr_log(WLR_INFO,
			"output: %s -> attachment %dx%d presentation %dx%d @%d scale %.2f "
			"transform %d",
			m->wlr_output->name, m->wlr_output->width, m->wlr_output->height,
			tw, th, (int32_t)((m->wlr_output->refresh + 500) / 1000),
			(double)m->wlr_output->scale, (int32_t)m->wlr_output->transform);

	/* The commit changed this output's logical size, so every output placed
	 * relative to it is now measured against a number that no longer holds. */
	if (output_reflow(m, before))
		updatemons(NULL, NULL);

	printstatus(IPC_WATCH_ARRANGGE);
	return true;
}

static bool output_apply(Monitor *m, struct wlr_output_mode *mode,
						 float scale) {
	struct output_change ch = {
		.mode = mode, .scale = scale, .transform = -1,
	};
	return output_apply_change(m, &ch);
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
	if (!output_apply(m, mode, 0.0f))
		return 0;

	char w[16], h[16], hz[16];
	snprintf(w, sizeof(w), "%d", mode->width);
	snprintf(h, sizeof(h), "%d", mode->height);
	/* whole Hz, the way the file writes it: the panel reports 143999 mHz as
	 * "144" and a config saying `refresh 143999` would match no mode at all */
	snprintf(hz, sizeof(hz), "%d", (mode->refresh + 500) / 1000);
	const char *keys[] = {"width", "height", "refresh"};
	const char *vals[] = {w, h, hz};
	output_persist(m, keys, vals, 3);
	return 1;
}

/*
 * ── OUTPUT TRANSFORM, LIVE ────────────────────────────────────────────────
 *
 * `dispatch set_output_transform,DP-1,1` -- 0..7, the wl_output_transform
 * values: normal, 90, 180, 270, flipped, flipped-90, flipped-180,
 * flipped-270.
 *
 * It exists because a transform set at startup and a transform CHANGED while
 * a desktop is running are different code paths, and only the second one
 * produces the frame where the pending transform and the committed one
 * disagree. That frame is where M4F.2C.4d's defect lived; nothing could
 * reach it from the config file alone.
 */
int32_t set_output_transform(const Arg *arg) {
	if (!arg || !arg->v)
		return 0;
	Monitor *m = output_by_name_or_focus((const char *)arg->v);
	if (!m || arg->i < 0 || arg->i > 7) {
		wlr_log(WLR_ERROR, "set_output_transform: %d is not a "
				"wl_output_transform (0-7)", arg->i);
		return 0;
	}
	struct output_change ch = { .transform = arg->i, .scale = 0.0f };
	if (!output_apply_change(m, &ch))
		return 0;

	char tr[8];
	snprintf(tr, sizeof(tr), "%d", arg->i);
	const char *keys[] = {"rr"};
	const char *vals[] = {tr};
	output_persist(m, keys, vals, 1);
	return 1;
}

/*
 * MODE AND TRANSFORM IN ONE COMMIT.
 *
 * `dispatch set_output_mode_transform,HEADLESS-1,1024x768,1`
 *
 * Two separate dispatches produce two frames, each with one field pending.
 * This produces the ONE frame whose state carries both -- where the attachment
 * extent must come from state->mode and the presentation extent from
 * state->transform, and reading either from the committed output is a frame
 * drawn half in the configuration it is leaving. A custom mode is accepted for
 * outputs that advertise no mode list, which is every headless output.
 */
int32_t set_output_mode_transform(const Arg *arg) {
	if (!arg || !arg->v || !arg->v2)
		return 0;
	Monitor *m = output_by_name_or_focus((const char *)arg->v);
	if (!m || arg->i < 0 || arg->i > 7)
		return 0;
	struct output_change ch = { .transform = arg->i, .scale = 0.0f };
	ch.mode = output_find_mode(m, (const char *)arg->v2);
	if (!ch.mode) {
		int32_t w = 0, h = 0;
		if (sscanf((const char *)arg->v2, "%dx%d", &w, &h) != 2 || w <= 0 ||
			h <= 0) {
			wlr_log(WLR_ERROR, "set_output_mode_transform: %s is not WxH",
					(const char *)arg->v2);
			return 0;
		}
		ch.custom_w = w;
		ch.custom_h = h;
	}
	if (!output_apply_change(m, &ch))
		return 0;

	char ws[16], hs[16], tr[8];
	snprintf(ws, sizeof(ws), "%d", m->wlr_output->width);
	snprintf(hs, sizeof(hs), "%d", m->wlr_output->height);
	snprintf(tr, sizeof(tr), "%d", arg->i);
	const char *keys[] = {"width", "height", "rr"};
	const char *vals[] = {ws, hs, tr};
	output_persist(m, keys, vals, 3);
	return 1;
}

int32_t set_output_scale(const Arg *arg) {
	if (!arg || !arg->v)
		return 0;
	Monitor *m = output_by_name_or_focus((const char *)arg->v);
	if (!m || arg->f <= 0.0f)
		return 0;
	if (!output_apply(m, NULL, arg->f))
		return 0;

	/* %g so 1.0 persists as `scale 1` and 0.75 as `scale 0.75`, which is how
	 * the file already reads -- `scale 1.000000` is the same number and looks
	 * like a machine got at it */
	char sc[32];
	snprintf(sc, sizeof(sc), "%g", (double)arg->f);
	const char *keys[] = {"scale"};
	const char *vals[] = {sc};
	output_persist(m, keys, vals, 1);
	return 1;
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

	char xs[16], ys[16];
	snprintf(xs, sizeof(xs), "%d", arg->i);
	snprintf(ys, sizeof(ys), "%d", arg->i2);
	const char *keys[] = {"x", "y"};
	const char *vals[] = {xs, ys};
	output_persist(m, keys, vals, 2);
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

	m->vrr_global_enable = arg->i ? 1 : 0;
	/* The shared commit, not a second copy of it: this one resets the
	 * presenter's epoch when the status actually moves, which the copy that
	 * used to live here did not. */
	bool ok = commit_vrr_state(m, arg->i != 0);
	if (ok) {
		printstatus(IPC_WATCH_ARRANGGE);
		const char *keys[] = {"vrr"};
		const char *vals[] = {arg->i ? "1" : "0"};
		output_persist(m, keys, vals, 1);
	}
	return ok ? 1 : 0;
}

/* This output's HDR BASELINE -- "does the desktop run in HDR on this display".
 *
 * Declarative and per-output, which is the level the question actually lives
 * at: a global tri-state cannot say "HDR on the capable panel, SDR on the one
 * beside it", and that is the ordinary two-monitor case.
 *
 * It writes hdr_configured, the INPUT to hdr_resolve(), never m->hdr -- that is
 * derived, and the Monitor struct reserves it to hdr_resolve alone. So this
 * stays subordinate to the policy above it: `misc { hdr-mode off }` still wins,
 * a force_hdr client still wins, and an output that cannot do BT.2020+PQ is
 * still refused. Setting a baseline the policy overrides is not an error --
 * the baseline is remembered and takes effect when the policy allows it.
 *
 * Persisted as `hdr 1`, or removed entirely when off. Not written as a bare
 * `hdr` flag: the parser reads it with atoi(val), so the value form is the one
 * that is unambiguous both to write and to read back. */
int32_t set_output_hdr(const Arg *arg) {
	if (!arg || !arg->v)
		return 0;
	Monitor *m = output_by_name_or_focus((const char *)arg->v);
	if (!m || !m->wlr_output)
		return 0;

	m->hdr_configured = arg->i ? 1 : 0;
	/* a manual choice outranks the capture-triggered fallback; do not let a
	 * later capture session end flip it back */
	hdr_resolve(m);

	/*
	 * "0", NOT ABSENCE. Turning HDR off used to REMOVE the key, which leaves
	 * the output unconfigured -- and an unconfigured output takes the global
	 * `hdr-mode` default, so with `hdr-mode on` a reload turned HDR straight
	 * back on and the operator's explicit "off" evaporated. An explicit choice
	 * has to survive as an explicit choice.
	 */
	const char *keys[] = {"hdr"};
	const char *vals[] = {m->hdr_configured > 0 ? "1" : "0"};
	output_persist(m, keys, vals, 1);

	if ((m->hdr > 0) != (m->hdr_configured > 0)) {
		/* Now only reachable for reasons that genuinely outrank an explicit
		 * request: the output cannot do BT.2020+PQ, the global kill switch is
		 * set, or a force_hdr client is holding it on. `hdr-mode on` is no
		 * longer among them -- it is a default for outputs nobody has spoken
		 * for, not an override of the ones they have. */
		wlr_log(WLR_INFO,
				"set_output_hdr: %s saved as the baseline for %s, but %s "
				"overrides it for now (hdr is %s)",
				m->hdr_configured > 0 ? "on" : "off", m->wlr_output->name,
				m->hdr_capability_failed ? "this output's lack of BT.2020+PQ"
				: config.hdr_mode == 0	 ? "`misc { hdr-mode off }`"
										 : "a force_hdr client",
				m->hdr > 0 ? "on" : "off");
	}
	printstatus(IPC_WATCH_ARRANGGE);
	return 1;
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
	/*
	 * ── AND RE-DERIVE, AND REPAINT ────────────────────────────────────────
	 *
	 * Loading the profile is half of it. Since M6B/G2 a profile that reduces to
	 * a matrix-shaper changes the output's PATH and its encode curve, so the
	 * colour state has to be derived again or the profile sits loaded and
	 * unapplied -- logged, reported by `amsg get all-monitors`, and doing
	 * nothing. NULL state: nothing about the output's format or mode is
	 * changing here, only what happens to the pixels on their way out.
	 *
	 * Then the whole output, because NOTHING ELSE WILL DAMAGE IT. Every pixel's
	 * encoding just changed and not one of them is in anybody's damage region;
	 * without this the new state applies to whatever happens to be redrawn next
	 * and the rest of the screen keeps the old encoding until something moves
	 * over it. Also the A<->B transition: Path B composites into an
	 * intermediate that holds nothing yet, and the first frame into a fresh one
	 * is forced full anyway.
	 */
	mon_derive_color_state(m, NULL);
	if (m->scene_output != NULL) {
		wlr_damage_ring_add_whole(&m->scene_output->damage_ring);
		wlr_output_schedule_frame(m->wlr_output);
	}
	printstatus(IPC_WATCH_ARRANGGE);

	/* An empty path REMOVES the entry rather than writing `icc-profile ""`:
	 * that would be a profile whose path is the empty string, and the way back
	 * to the untransformed pipeline is for the setting not to be there. */
	char quoted[1024];
	snprintf(quoted, sizeof(quoted), "\"%s\"", path);
	const char *keys[] = {"icc-profile"};
	const char *vals[] = {*path ? quoted : NULL};
	output_persist(m, keys, vals, 1);
	return 1;
}

#endif /* ASTEROIDZ_ACTION_OUTPUT_H */
