int32_t bind_to_view(const Arg *arg) {
	if (!selmon)
		return 0;
	uint32_t target = arg->ui;

	if (config.view_current_to_back && selmon->pertag->curtag &&
		(target & TAGMASK) == (selmon->tagset[selmon->seltags])) {
		if (selmon->pertag->prevtag)
			target = 1 << (selmon->pertag->prevtag - 1);
		else
			target = 0;
	}

	if (!config.view_current_to_back &&
		(target & TAGMASK) == (selmon->tagset[selmon->seltags])) {
		return 0;
	}

	if ((int32_t)target == INT_MIN && selmon->pertag->curtag == 0) {
		if (config.view_current_to_back && selmon->pertag->prevtag)
			target = 1 << (selmon->pertag->prevtag - 1);
		else
			target = 0;
	}

	if (target == 0 || (int32_t)target == INT_MIN) {
		view(&(Arg){.ui = ~0 & TAGMASK, .i = arg->i}, false);
	} else {
		view(&(Arg){.ui = target, .i = arg->i}, true);
	}
	return 0;
}

int32_t chvt(const Arg *arg) {
	struct timespec ts;

	allow_frame_scheduling = false;

	if (selmon) {
		chvt_backup_tag = selmon->pertag->curtag;
		strncpy(chvt_backup_selmon, selmon->wlr_output->name,
				sizeof(chvt_backup_selmon) - 1);
	}

	wlr_session_change_vt(session, arg->ui);

	ts.tv_sec = 0;
	ts.tv_nsec = 100000000;
	nanosleep(&ts, NULL);

	allow_frame_scheduling = true;
	return 1;
}

int32_t create_virtual_output(const Arg *arg) {
	if (!wlr_backend_is_multi(backend)) {
		wlr_log(WLR_ERROR, "Expected a multi backend");
		return 0;
	}

	bool done = false;
	wlr_multi_for_each_backend(backend, create_output, &done);

	if (!done) {
		wlr_log(WLR_ERROR, "Failed to create virtual output");
		return 0;
	}

	wlr_log(WLR_INFO, "Virtual output created");
	return 0;
}

int32_t destroy_all_virtual_output(const Arg *arg) {
	if (!wlr_backend_is_multi(backend)) {
		wlr_log(WLR_ERROR, "Expected a multi backend");
		return 0;
	}

	Monitor *m, *tmp;
	wl_list_for_each_safe(m, tmp, &mons, link) {
		if (wlr_output_is_headless(m->wlr_output)) {
			wlr_output_destroy(m->wlr_output);
			wlr_log(WLR_INFO, "Virtual output destroyed");
		}
	}
	return 0;
}

int32_t defaultgaps(const Arg *arg) {
	setgaps(config.gappoh, config.gappov, config.gappih, config.gappiv);
	return 0;
}

int32_t exchange_client(const Arg *arg) {
	if (!selmon)
		return 0;
	Client *c = arg->tc ? arg->tc : selmon->sel;
	if (!c || c->isfloating)
		return 0;

	if ((c->isfullscreen || c->ismaximizescreen) && !is_scroller_layout(c->mon))
		return 0;

	Client *tc = direction_select(arg);
	tc = get_focused_stack_client(tc, arg->tc);

	if (!tc)
		return 0;

	exchange_two_client(c, tc);
	return 0;
}

int32_t exchange_stack_client(const Arg *arg) {
	if (!selmon)
		return 0;

	Client *c = arg->tc ? arg->tc : selmon->sel;
	Client *tc = NULL;
	if (!c || c->isfloating || c->isfullscreen || c->ismaximizescreen)
		return 0;
	if (arg->i == NEXT) {
		tc = get_next_stack_client(c, false);
	} else {
		tc = get_next_stack_client(c, true);
	}
	if (tc)
		exchange_two_client(c, tc);
	return 0;
}

int32_t focusdir(const Arg *arg) {

	if (!selmon)
		return 0;

	Client *c = NULL;
	c = direction_select(arg);

	if (!selmon->isoverview)
		c = get_focused_stack_client(c, arg->tc);
	if (c) {
		focusclient(c, 1);
		if (config.warpcursor)
			warp_cursor(c);
	} else {
		if (config.focus_cross_tag) {
			if (arg->i == LEFT || arg->i == UP)
				viewtoleft_have_client(&(Arg){0});
			if (arg->i == RIGHT || arg->i == DOWN)
				viewtoright_have_client(&(Arg){0});
		} else if (config.focus_cross_monitor) {
			focusmon(arg);
		}
	}
	return 0;
}

int32_t focuslast(const Arg *arg) {
	Client *c = NULL;
	Client *tc = NULL;
	bool begin = false;
	uint32_t target = 0;

	wl_list_for_each(c, &fstack, flink) {
		if (c->iskilling || c->isminimized || c->isunglobal ||
			!client_surface(c)->mapped || client_is_unmanaged(c) ||
			client_is_x11_popup(c))
			continue;

		if (selmon && !selmon->sel) {
			tc = c;
			break;
		}

		if (selmon && c == selmon->sel && !begin) {
			begin = true;
			continue;
		}

		if (begin) {
			tc = c;
			break;
		}
	}

	if (!tc || !client_surface(tc)->mapped)
		return 0;

	if ((int32_t)tc->tags > 0) {
		focusclient(tc, 1);
		target = get_tags_first_tag(tc->tags);
		view(&(Arg){.ui = target}, true);
	}
	return 0;
}

int32_t toggle_trackpad_enable(const Arg *arg) {
	config.disable_trackpad = !config.disable_trackpad;
	return 0;
}

int32_t focusmon(const Arg *arg) {
	Client *c = NULL;
	Monitor *m = NULL;
	Monitor *tm = NULL;

	if (arg->i != UNDIR) {
		tm = dirtomon(arg->i);
	} else if (arg->v) {
		wl_list_for_each(m, &mons, link) {
			if (!m->wlr_output->enabled) {
				continue;
			}
			if (match_monitor_spec(arg->v, m)) {
				tm = m;
				break;
			}
		}
	} else {
		return 0;
	}

	if (!tm || !tm->wlr_output->enabled || tm == selmon)
		return 0;

	selmon = tm;
	if (config.warpcursor) {
		warp_cursor_to_selmon(selmon);
	}
	c = arg->tc ? arg->tc : focustop(selmon);
	if (!c) {
		selmon->sel = NULL;
		wlr_seat_pointer_notify_clear_focus(seat);
		wlr_seat_keyboard_notify_clear_focus(seat);
		focusclient(NULL, 0);
	} else
		focusclient(c, 1);

	return 0;
}

int32_t focusstack(const Arg *arg) {
	Client *sel = arg->tc ? arg->tc : focustop(selmon);
	Client *tc = NULL;

	if (!sel)
		return 0;
	if (arg->i == NEXT) {
		tc = get_next_stack_client(sel, false);
	} else {
		tc = get_next_stack_client(sel, true);
	}

	if (!tc)
		return 0;

	focusclient(tc, 1);
	if (config.warpcursor)
		warp_cursor(tc);
	return 0;
}

int32_t pin(const Arg *arg) {
	if (!selmon)
		return 0;

	Client *c = arg->tc ? arg->tc : selmon->sel;
	if (!c || !c->mon || c->iskilling)
		return 0;

	c->ispinned = !c->ispinned;

	if (c->ispinned) {
		if (!c->isfloating)
			setfloating(c, 1);
		wlr_scene_node_raise_to_top(&c->scene->node);
	}

	arrange(c->mon, false, false);
	printstatus(IPC_WATCH_CLIENT);
	return 0;
}

int32_t incnmaster(const Arg *arg) {
	if (!arg || !selmon)
		return 0;
	selmon->pertag->nmasters[selmon->pertag->curtag] =
		ASTEROIDZ_MAX(selmon->pertag->nmasters[selmon->pertag->curtag] + arg->i, 0);
	arrange(selmon, false, false);
	return 0;
}

int32_t incgaps(const Arg *arg) {
	if (!selmon)
		return 0;
	setgaps(selmon->gappoh + arg->i, selmon->gappov + arg->i,
			selmon->gappih + arg->i, selmon->gappiv + arg->i);
	return 0;
}

int32_t incigaps(const Arg *arg) {
	if (!selmon)
		return 0;
	setgaps(selmon->gappoh, selmon->gappov, selmon->gappih + arg->i,
			selmon->gappiv + arg->i);
	return 0;
}

int32_t incogaps(const Arg *arg) {
	if (!selmon)
		return 0;
	setgaps(selmon->gappoh + arg->i, selmon->gappov + arg->i, selmon->gappih,
			selmon->gappiv);
	return 0;
}

int32_t incihgaps(const Arg *arg) {
	if (!selmon)
		return 0;
	setgaps(selmon->gappoh, selmon->gappov, selmon->gappih + arg->i,
			selmon->gappiv);
	return 0;
}

int32_t incivgaps(const Arg *arg) {
	if (!selmon)
		return 0;
	setgaps(selmon->gappoh, selmon->gappov, selmon->gappih,
			selmon->gappiv + arg->i);
	return 0;
}

int32_t incohgaps(const Arg *arg) {
	if (!selmon)
		return 0;
	setgaps(selmon->gappoh + arg->i, selmon->gappov, selmon->gappih,
			selmon->gappiv);
	return 0;
}

int32_t incovgaps(const Arg *arg) {
	if (!selmon)
		return 0;
	setgaps(selmon->gappoh, selmon->gappov + arg->i, selmon->gappih,
			selmon->gappiv);
	return 0;
}

int32_t setmfact(const Arg *arg) {
	float f;
	Client *c = NULL;

	if (!arg || !selmon ||
		!selmon->pertag->ltidxs[selmon->pertag->curtag]->arrange)
		return 0;
	f = arg->f < 1.0 ? arg->f + selmon->pertag->mfacts[selmon->pertag->curtag]
					 : arg->f - 1.0;
	if (f < 0.1 || f > 0.9)
		return 0;

	selmon->pertag->mfacts[selmon->pertag->curtag] = f;
	wl_list_for_each(c, &clients, link) {
		if (VISIBLEON(c, selmon) && ISTILED(c)) {
			c->master_mfact_per = f;
		}
	}
	arrange(selmon, false, false);
	return 0;
}

int32_t killclient(const Arg *arg) {
	Client *c = arg->tc ? arg->tc : (selmon ? selmon->sel : NULL);
	if (c) {
		if (arg->i == FORCE) {
			client_pending_force_kill(c);
		} else {
			pending_kill_client(c);
		}
	}
	return 0;
}

/* Shared by moveresize() (mod+drag, target resolved via xytonode at the
 * cursor) and titlebar drag-to-move (target already known from the clicked
 * node, which may sit outside the client's own surface bounds so xytonode
 * can't be used to (re)resolve it). Assumes `target` is already validated
 * (managed, not fullscreen/maximized) and cursor_mode is CurNormal/CurPressed. */
int32_t begin_move_or_resize(Client *target, uint32_t mode);

int32_t moveresize(const Arg *arg) {
	if (cursor_mode != CurNormal && cursor_mode != CurPressed)
		return 0;
	xytonode(cursor->x, cursor->y, NULL, &grabc, NULL, NULL, NULL);
	if (!grabc || client_is_unmanaged(grabc) || grabc->isfullscreen ||
		grabc->ismaximizescreen) {
		grabc = NULL;
		return 0;
	}
	/* a hidden monocle window resolved via its background segment tab: just
	 * focus it (matches the is_monocle_bg_tab click behaviour) -- never yank
	 * an invisible window into a floating drag */
	if (grabc->mon && is_monocle_layout(grabc->mon) && grabc->is_monocle_hide) {
		Client *fc = grabc;
		grabc = NULL;
		focusclient(fc, 1);
		return 0;
	}
	return begin_move_or_resize(grabc, arg->ui);
}

int32_t begin_move_or_resize(Client *target, uint32_t mode) {
	const char *cursors[] = {"nw-resize", "ne-resize", "sw-resize",
							 "se-resize"};

	grabc = target;
	if (grabc->isfloating == 0 && mode == CurMove) {
		grabc->drag_to_tile = true;
		exit_scroller_stack(grabc);
		setfloating(grabc, 1);
		grabc->drag_tile_float_backup_geom = grabc->float_geom;
		grabc->old_stack_inner_per = 0.0f;
		grabc->old_master_inner_per = 0.0f;
		set_size_per(grabc->mon, grabc);
	}

	if (grabc && grabc->drag_to_tile && config.drag_tile_small) {
		grabc->geom.x = cursor->x - 150;
		grabc->geom.y = cursor->y - 150;
		grabc->geom.width = 300;
		grabc->geom.height = 300;
		resize(grabc, grabc->geom, 1);
	}

	switch (cursor_mode = mode) {
	case CurMove:
		grabcx = cursor->x - grabc->geom.x;
		grabcy = cursor->y - grabc->geom.y;
		az_cursor_set_xcursor("grab");
		break;
	case CurResize:
		if (grabc->isfloating) {
			rzcorner = config.drag_corner;
			grabcx = (int)round(cursor->x);
			grabcy = (int)round(cursor->y);
			if (rzcorner == 4)
				rzcorner = (grabcx - grabc->geom.x <
									grabc->geom.x + grabc->geom.width - grabcx
								? 0
								: 1) +
						   (grabcy - grabc->geom.y <
									grabc->geom.y + grabc->geom.height - grabcy
								? 0
								: 2);

			if (config.drag_warp_cursor) {
				grabcx = rzcorner & 1 ? grabc->geom.x + grabc->geom.width
									  : grabc->geom.x;
				grabcy = rzcorner & 2 ? grabc->geom.y + grabc->geom.height
									  : grabc->geom.y;
				wlr_cursor_warp_closest(cursor, NULL, grabcx, grabcy);
			}

			az_cursor_set_xcursor(cursors[rzcorner]);
		} else {
			az_cursor_set_xcursor("grab");
		}
		break;
	}
	return 0;
}

int32_t movewin(const Arg *arg) {
	Client *c = arg->tc ? arg->tc : (selmon ? selmon->sel : NULL);
	if (!c || c->isfullscreen)
		return 0;
	if (!c->isfloating)
		setfloating(c, 1);

	switch (arg->ui) {
	case NUM_TYPE_MINUS:
		c->geom.x -= arg->i;
		break;
	case NUM_TYPE_PLUS:
		c->geom.x += arg->i;
		break;
	default:
		c->geom.x = arg->i;
		break;
	}

	switch (arg->ui2) {
	case NUM_TYPE_MINUS:
		c->geom.y -= arg->i2;
		break;
	case NUM_TYPE_PLUS:
		c->geom.y += arg->i2;
		break;
	default:
		c->geom.y = arg->i2;
		break;
	}

	c->iscustomsize = 1;
	c->float_geom = c->geom;
	resize(c, c->geom, 0);
	return 0;
}

/* ── a modal prompt, on every output ────────────────────────────────────────
 *
 * The dimmed screen with a bordered label in the middle of it, which the global
 * shortcuts picker put up first and the exit confirmation now shares. Extracted
 * rather than copied: "looks like the other prompt" is a requirement that a
 * second copy of the theme block satisfies until somebody edits one of them.
 *
 * EVERY output, not the focused one. Both of these prompts take the keyboard
 * away from everything: nothing else will answer a key until they are dismissed.
 * Dimming one screen said the opposite -- the other monitor kept a window that
 * looked live, was not, and was where the eyes already were. On a multi-head
 * desk the question can easily be asked on the screen nobody is looking at.
 *
 * It draws and nothing else. Who may dismiss it, what dismisses it, and what
 * happens then belong to the caller -- the picker waits for any key, the exit
 * confirmation waits for exactly two, and neither is a property of a rectangle.
 */
typedef struct {
	struct wlr_scene_tree *tree;
	/* One per output. The scene nodes die with the tree, but each label owns a
	 * pango layout and a buffer of its own, so every one has to be destroyed --
	 * keeping only the focused monitor's would leak the rest per prompt. */
	struct asteroidz_jump_label_node **labels;
	size_t n_labels;
} AsteroidzPrompt;

static bool asteroidz_prompt_show(AsteroidzPrompt *p, const char *msg,
								  bool markup) {
	if (!selmon)
		return false;
	struct wlr_scene_tree *tree = wlr_scene_tree_create(layers[LyrOverlay]);
	if (!tree)
		return false;

	p->tree = tree;
	p->labels = NULL;
	p->n_labels = 0;

	size_t n = 0;
	Monitor *m;
	wl_list_for_each(m, &mons, link) {
		if (m->wlr_output && m->wlr_output->enabled)
			n++;
	}
	if (n > 0)
		p->labels = calloc(n, sizeof(*p->labels));

	float dim[4] = {0.f, 0.f, 0.f, 0.55f};

	/* Deliberately NOT config.theme. This is a compositor prompt over whatever
	 * is on screen, and it has to stay legible against a palette the user is in
	 * the middle of changing -- including one where fg and bg have just been set
	 * to the same colour. */
	AsteroidzTheme th = {0};
	th.fg_color[0] = th.fg_color[1] = th.fg_color[2] = th.fg_color[3] = 1.f;
	th.bg_color[0] = 0.08f;
	th.bg_color[1] = 0.08f;
	th.bg_color[2] = 0.10f;
	th.bg_color[3] = 0.97f;
	th.border_color[0] = 1.f;
	th.border_color[1] = 0.70f;
	th.border_color[2] = 0.73f;
	th.border_color[3] = 1.f;
	th.border_width = 2;
	th.corner_radius = 12;
	th.padding_x = 28;
	th.padding_y = 18;
	/* The FONT is the theme's, unlike the colours above. Nothing about a
	 * palette being mid-change makes a font unreadable, and a prompt drawn in
	 * a typeface that appears nowhere else on the desktop reads as something
	 * other than asteroidz asking. Falls back to the old hardcoded string when
	 * the theme sets none. */
	th.font_desc = config.theme.font_desc ? config.theme.font_desc
										  : "monospace Bold 18";

	wl_list_for_each(m, &mons, link) {
		if (!m->wlr_output || !m->wlr_output->enabled)
			continue;

		/* m->m, not m->w: the dim covers the whole output, bars included.
		 * Leaving a lit strip of bar above a dimmed desktop would look like
		 * the bar was still taking clicks, which it is not. */
		struct wlr_scene_rect *bg =
			wlr_scene_rect_create(tree, m->m.width, m->m.height, dim);
		if (bg)
			wlr_scene_node_set_position(&bg->node, m->m.x, m->m.y);

		if (!p->labels || p->n_labels >= n)
			continue;
		struct asteroidz_jump_label_node *label =
			asteroidz_jump_label_node_create(tree, th);
		if (!label)
			continue;
		p->labels[p->n_labels++] = label;

		/* Before the first update, which is what measures and draws it. */
		asteroidz_jump_label_node_set_markup(label, markup);
		/* Each output's OWN scale. A prompt rendered once at the focused
		 * monitor's scale and reused would be soft on a monitor that does not
		 * share it, which is the general case for a laptop beside a desktop
		 * display -- and it is text whose whole purpose is to be read. */
		float scale = m->wlr_output->scale;
		asteroidz_jump_label_node_update(label, msg, scale);
		/* logical_*, not the buffer's pixel dimensions. The buffer is drawn at
		 * the output's scale while this position is in layout coordinates, so
		 * the two agree only at scale 1 -- which is why centring off the
		 * buffer looked right here and would have put the prompt up and to the
		 * left of centre on any scaled output. */
		int lw = label->logical_width > 0 ? label->logical_width : 360;
		int lh = label->logical_height > 0 ? label->logical_height : 90;
		wlr_scene_node_set_position(&label->scene_buffer->node,
									m->m.x + (m->m.width - lw) / 2,
									m->m.y + (m->m.height - lh) / 2);
	}

	wlr_scene_node_raise_to_top(&tree->node);
	return true;
}

static void asteroidz_prompt_hide(AsteroidzPrompt *p) {
	for (size_t i = 0; i < p->n_labels; i++) {
		if (p->labels[i])
			asteroidz_jump_label_node_destroy(p->labels[i]);
	}
	free(p->labels);
	p->labels = NULL;
	p->n_labels = 0;
	if (p->tree) {
		wlr_scene_node_destroy(&p->tree->node);
		p->tree = NULL;
	}
}

/* ── confirm before exiting ─────────────────────────────────────────────────
 *
 * Exiting takes every client with it -- editors, terminals, a game mid-session
 * -- and the compositor cannot ask them whether they mind. So the one question
 * it can ask, it asks.
 *
 * Enter exits, Escape does not, and there is no third answer: anything else is
 * swallowed and the prompt stays. A prompt that dismissed itself on a stray key
 * would be worse than none, because it would train the reflex of pressing
 * through it.
 *
 * Only the DISPATCH is confirmed. SIGTERM and SIGINT go straight to
 * wl_display_terminate through quit_now(): a session manager shutting the
 * machine down is not asking, and a compositor that stalled a reboot behind a
 * dialogue nobody is at the keyboard to answer would be a fault, not a
 * safeguard.
 */
static struct {
	bool active;
	AsteroidzPrompt prompt;
} quit_confirm;

static void quit_confirm_hide(void) {
	quit_confirm.active = false;
	asteroidz_prompt_hide(&quit_confirm.prompt);
}

/* Called from keypress() before anything else looks at the key. Returns true
 * when the event was consumed -- which the caller must honour, or the Enter
 * that confirms the exit is also delivered to whatever had focus. */
bool quit_confirm_handle_key(uint32_t state, xkb_keysym_t sym) {
	if (!quit_confirm.active)
		return false;
	if (state != WL_KEYBOARD_KEY_STATE_PRESSED)
		return true; /* swallow the releases of keys we swallowed the press of */
	if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
		quit_confirm_hide();
		/* Ending is emitted from inhibit_portal_finish(), which runs in
		 * cleanup() and so covers the signal path too -- not here, where it
		 * would only cover the one exit a human confirmed. */
		wl_display_terminate(dpy);
		return true;
	}
	if (sym == XKB_KEY_Escape) {
		wlr_log(WLR_INFO, "exit cancelled");
		quit_confirm_hide();
		inhibit_portal_set_session_state(1 /* Running */);
		return true;
	}
	return true; /* everything else: swallowed, prompt stays up */
}

/* Exit with no questions. The signal handlers' path, and the way out for
 * anything scripted. */
int32_t quit_now(const Arg *arg) {
	wl_display_terminate(dpy);
	return 0;
}

int32_t quit(const Arg *arg) {
	if (quit_confirm.active) {
		/* Dispatched twice. Reading the second one as confirmation would let a
		 * key held down on a repeating bind exit the session, so it is the
		 * keyboard's answer or nothing. */
		return 0;
	}
	/* ENTER in the theme's urgent colour, in caps, bold: it is the one key
	 * here that ends the session, and a prompt where both answers look alike
	 * is a prompt people learn to dismiss without reading. Escape stays plain
	 * -- it is the harmless one, and colouring both would say nothing.
	 *
	 * The colour is read from the theme rather than pinned like the prompt's
	 * own palette above, because "urgent" is the one entry whose whole job is
	 * to be noticed, and it is legible on this prompt's fixed dark panel
	 * whatever matugen makes of it. */
	/* An application that took a logout inhibition through the portal gets a
	 * line of its own here, naming it. The compositor is the session, so
	 * there is no session manager to veto the exit -- and refusing outright
	 * would be the wrong answer anyway, since the person at the keyboard
	 * outranks a background request. What they were missing is the fact, and
	 * the fact is what this puts in front of them: the unsaved editor gets
	 * named instead of dying quietly behind a prompt that said nothing.
	 * See ipc/inhibit-portal.h. */
	char busy[256];
	bool inhibited = inhibit_portal_logout_summary(busy, sizeof(busy));

	char msg[512];
	snprintf(msg, sizeof(msg),
			 "Exit asteroidz?\n"
			 "%s%s"
			 "<b><span foreground=\"#%02x%02x%02x\">ENTER</span></b> to exit, "
			 "Esc to stay",
			 inhibited ? busy : "", inhibited ? "\n" : "",
			 (unsigned)lround(config.theme.urgent_color[0] * 255.0f),
			 (unsigned)lround(config.theme.urgent_color[1] * 255.0f),
			 (unsigned)lround(config.theme.urgent_color[2] * 255.0f));

	if (!asteroidz_prompt_show(&quit_confirm.prompt, msg, true)) {
		/* No output to draw on -- headless, or mid-teardown. Asking a question
		 * nobody can see and then waiting for an answer would hang. */
		wl_display_terminate(dpy);
		return 0;
	}
	quit_confirm.active = true;
	/* Query End, for anything monitoring the session: the spec's own name for
	 * "the session is being asked to end and has not yet". Nothing waits on
	 * the acknowledgement -- what the exit waits for is a keypress. */
	inhibit_portal_set_session_state(2 /* Query End */);
	wlr_log(WLR_INFO, "exit requested: waiting for confirmation%s",
			inhibited ? " (an application asked not to be interrupted)" : "");
	return 0;
}

/* Flip this output's HDR baseline. A convenience over set_output_hdr, and
 * nothing more.
 *
 * It used to assign m->hdr directly, which is a DERIVED value -- hdr_resolve()
 * recomputes it from the policy ladder and the Monitor struct reserves it to
 * hdr_resolve alone. So the toggle was thrown away by the next resolve pass,
 * which any config reload or client change triggers: from outside, HDR
 * "bounced back" on its own and the dispatch appeared to do nothing.
 *
 * An imperative toggle is also the odd one out in what is otherwise a
 * declarative model (global policy -> per-output baseline -> per-window
 * override), so it no longer has a mechanism of its own: it reads the current
 * baseline, inverts it, and hands over. That means it persists to the config
 * like any other baseline change, instead of being a runtime-only flip that
 * vanished on reload. */
int32_t toggle_hdr(const Arg *arg) {
	Monitor *m = selmon;
	if (!m || !m->wlr_output || !m->wlr_output->name)
		return 0;

	Arg forward = {0};
	forward.v = strdup(m->wlr_output->name);
	if (!forward.v)
		return 0;
	forward.i = m->hdr_configured > 0 ? 0 : 1;
	int32_t ret = set_output_hdr(&forward);
	free(forward.v);
	return ret;
}

int32_t set_sdr_luminance(const Arg *arg) {
	float base = config.sdr_reference_luminance > 0
		? config.sdr_reference_luminance
		: 203.0f;
	float value = arg->i ? base + arg->f : arg->f;

	config.sdr_reference_luminance = CLAMP_FLOAT(value, 80.0f, 1000.0f);
	wlr_scene_set_sdr_reference_luminance(scene,
										  config.sdr_reference_luminance);
	wlr_log(WLR_INFO, "SDR reference luminance: %.0f cd/m2",
			config.sdr_reference_luminance);
	/*
	 * ── AND SAY SO. THE SCENE WAS THE ONLY THING BEING TOLD. ─────────────
	 *
	 * config.sdr_reference_luminance IS the SDR branch of
	 * az_preferred_resolve() -- max_luminance and max_fall both come from it,
	 * and it is hashed into the identity -- and it is the `reference` argument
	 * of wp-cm's luminances event, which has no other source anywhere in the
	 * protocol. Moving it changed what every SDR surface should be told and
	 * told none of them: clients kept the value they were handed at startup
	 * for the rest of the session.
	 *
	 * Global, so every output: there is no monitor to scope this to. Surfaces
	 * on an HDR output reach the identity comparison, match, and emit nothing.
	 */
	mon_send_preferred_descriptions_all();
	printstatus(IPC_WATCH_ARRANGGE);
	return 0;
}

int32_t restart(const Arg *arg) {
	/* Re-exec immediately instead of after teardown: every fd (DRM
	 * master, seat, client sockets, the wayland socket lock) is
	 * CLOEXEC, so the kernel releases them atomically and the fresh
	 * instance re-acquires the seat and reclaims wayland-0. If exec
	 * fails the session simply keeps running. */
	int logfd = open("/tmp/asteroidz-restart.log",
					 O_CREAT | O_WRONLY | O_APPEND, 0600);
	if (logfd >= 0) {
		dprintf(logfd, "== asteroidz re-exec: %s\n", restart_argv[0]);
		dup2(logfd, STDERR_FILENO);
		if (logfd != STDERR_FILENO)
			close(logfd);
	}
	setenv("ASTEROIDZ_RESTARTED", "1", 1);
	/* scrub the session env this instance exported for its clients:
	 * with WAYLAND_DISPLAY/DISPLAY set, wlroots would autodetect a
	 * nested wayland/x11 backend and die connecting to our own dead
	 * socket instead of re-acquiring the DRM session */
	unsetenv("WAYLAND_DISPLAY");
	unsetenv("DISPLAY");
	unsetenv("ASTEROIDZ_INSTANCE_SIGNATURE");
	execvp(restart_argv[0], restart_argv);
	/* argv[0] may not resolve via PATH depending on how the session was
	 * started; fall back to the installed binary */
	execv("/usr/bin/asteroidz", restart_argv);
	wlr_log_errno(WLR_ERROR, "in-place restart failed, continuing");
	return 0;
}

int32_t resizewin(const Arg *arg) {
	Client *c = arg->tc ? arg->tc : (selmon ? selmon->sel : NULL);
	int32_t offsetx = 0, offsety = 0;

	if (!c || c->isfullscreen || c->ismaximizescreen)
		return 0;

	int32_t animations_state_backup = config.animations;
	if (!c->isfloating)
		config.animations = 0;

	if (ISTILED(c)) {
		switch (arg->ui) {
		case NUM_TYPE_MINUS:
			offsetx = -arg->i;
			break;
		case NUM_TYPE_PLUS:
			offsetx = arg->i;
			break;
		default:
			offsetx = arg->i;
			break;
		}

		switch (arg->ui2) {
		case NUM_TYPE_MINUS:
			offsety = -arg->i2;
			break;
		case NUM_TYPE_PLUS:
			offsety = arg->i2;
			break;
		default:
			offsety = arg->i2;
			break;
		}
		resize_tile_client(c, false, offsetx, offsety, 0);
		config.animations = animations_state_backup;
		return 0;
	}

	switch (arg->ui) {
	case NUM_TYPE_MINUS:
		c->geom.width -= arg->i;
		break;
	case NUM_TYPE_PLUS:
		c->geom.width += arg->i;
		break;
	default:
		c->geom.width = arg->i;
		break;
	}

	switch (arg->ui2) {
	case NUM_TYPE_MINUS:
		c->geom.height -= arg->i2;
		break;
	case NUM_TYPE_PLUS:
		c->geom.height += arg->i2;
		break;
	default:
		c->geom.height = arg->i2;
		break;
	}

	c->iscustomsize = 1;
	c->float_geom = c->geom;
	resize(c, c->geom, 0);
	config.animations = animations_state_backup;
	return 0;
}

int32_t restore_minimized(const Arg *arg) {
	Client *c = arg->tc ? arg->tc : (selmon ? selmon->sel : NULL);

	if (selmon && selmon->isoverview)
		return 0;

	if (c && c->is_in_scratchpad && c->is_scratchpad_show) {
		client_pending_minimized_state(c, 0);
		c->is_scratchpad_show = 0;
		c->is_in_scratchpad = 0;
		c->isnamedscratchpad = 0;
		setborder_color(c);
		return 0;
	}

	wl_list_for_each(c, &clients, link) {
		if (c->isminimized && !c->isnamedscratchpad) {
			c->is_scratchpad_show = 0;
			c->is_in_scratchpad = 0;
			c->isnamedscratchpad = 0;
			show_hide_client(c);
			setborder_color(c);
			arrange(c->mon, false, false);
			focusclient(c, 0);
			warp_cursor(c);
			return 0;
		}
	}
	return 0;
}

int32_t setlayout(const Arg *arg) {
	int32_t jk;
	if (!selmon)
		return 0;

	for (jk = 0; jk < LENGTH(layouts); jk++) {
		if (strcmp(layouts[jk].name, arg->v) == 0) {
			selmon->pertag->ltidxs[selmon->pertag->curtag] = &layouts[jk];
			clear_fullscreen_and_maximized_state(selmon);
			arrange(selmon, false, false);
			printstatus(IPC_WATCH_ARRANGGE);
			return 0;
		}
	}
	return 0;
}

int32_t setkeymode(const Arg *arg) {
	snprintf(keymode.mode, sizeof(keymode.mode), "%.27s", arg->v);
	if (strcmp(keymode.mode, "default") == 0) {
		keymode.isdefault = true;
	} else {
		keymode.isdefault = false;
	}
	printstatus(IPC_WATCH_KEYMODE);
	return 1;
}

int32_t set_proportion(const Arg *arg) {
	if (!selmon)
		return 0;

	if (selmon->isoverview || !is_scroller_layout(selmon))
		return 0;

	if (selmon->visible_tiling_clients == 1 &&
		!config.scroller_ignore_proportion_single)
		return 0;

	Client *tc = arg->tc ? arg->tc : selmon->sel;
	if (!tc)
		return 0;

	tc = scroll_get_stack_head_client(tc);
	if (!tc)
		return 0;

	Monitor *m = tc->mon;
	uint32_t tag = m->pertag->curtag;
	struct TagScrollerState *st = m->pertag->scroller_state[tag];
	struct ScrollerStackNode *node = NULL;

	if (st)
		node = find_scroller_node(st, tc);

	if (node)
		node->scroller_proportion = arg->f;
	tc->scroller_proportion = arg->f;

	uint32_t max_client_width =
		m->w.width - 2 * config.scroller_structs - config.gappih;
	tc->geom.width = max_client_width * arg->f;

	arrange(m, false, false);
	return 0;
}

int32_t switch_proportion_preset(const Arg *arg) {
	float target_proportion = 0;
	if (!selmon)
		return 0;

	if (config.scroller_proportion_preset_count == 0)
		return 0;

	if (selmon->isoverview || !is_scroller_layout(selmon))
		return 0;

	if (selmon->visible_tiling_clients == 1 &&
		!config.scroller_ignore_proportion_single)
		return 0;

	Client *tc = arg->tc ? arg->tc : selmon->sel;
	if (!tc)
		return 0;

	tc = scroll_get_stack_head_client(tc);
	if (!tc)
		return 0;

	Monitor *m = tc->mon;
	uint32_t tag = m->pertag->curtag;
	struct TagScrollerState *st = m->pertag->scroller_state[tag];
	struct ScrollerStackNode *node = NULL;

	if (st)
		node = find_scroller_node(st, tc);

	float current_proportion =
		node ? node->scroller_proportion : tc->scroller_proportion;

	for (int32_t i = 0; i < config.scroller_proportion_preset_count; i++) {
		if (config.scroller_proportion_preset[i] == current_proportion) {
			if (arg->i == NEXT) {
				if (i == config.scroller_proportion_preset_count - 1)
					target_proportion = config.scroller_proportion_preset[0];
				else
					target_proportion =
						config.scroller_proportion_preset[i + 1];
			} else {
				if (i == 0)
					target_proportion =
						config.scroller_proportion_preset
							[config.scroller_proportion_preset_count - 1];
				else
					target_proportion =
						config.scroller_proportion_preset[i - 1];
			}
			break;
		}
	}

	if (target_proportion == 0.0f)
		target_proportion = config.scroller_proportion_preset[0];

	if (node)
		node->scroller_proportion = target_proportion;
	tc->scroller_proportion = target_proportion;

	uint32_t max_client_width =
		m->w.width - 2 * config.scroller_structs - config.gappih;
	tc->geom.width = max_client_width * target_proportion;

	arrange(m, false, false);
	return 0;
}

int32_t smartmovewin(const Arg *arg) {
	Client *c = NULL, *tc = NULL;
	int32_t nx, ny;
	int32_t buttom, top, left, right, tar;
	if (!selmon)
		return 0;
	c = arg->tc ? arg->tc : selmon->sel;
	if (!c || c->isfullscreen)
		return 0;
	if (!c->isfloating)
		setfloating(c, true);
	nx = c->geom.x;
	ny = c->geom.y;

	switch (arg->i) {
	case UP:
		tar = -99999;
		top = c->geom.y;
		ny -= c->mon->w.height / 4;

		wl_list_for_each(tc, &clients, link) {
			if (!VISIBLEON(tc, selmon) || !tc->isfloating || tc == c)
				continue;
			if (c->geom.x + c->geom.width < tc->geom.x ||
				c->geom.x > tc->geom.x + tc->geom.width)
				continue;
			buttom = tc->geom.y + tc->geom.height + config.gappiv;
			if (top > buttom && ny < buttom) {
				tar = ASTEROIDZ_MAX(tar, buttom);
			};
		}

		ny = tar == -99999 ? ny : tar;
		ny = ASTEROIDZ_MAX(ny, c->mon->w.y + c->mon->gappov);
		break;
	case DOWN:
		tar = 99999;
		buttom = c->geom.y + c->geom.height;
		ny += c->mon->w.height / 4;

		wl_list_for_each(tc, &clients, link) {
			if (!VISIBLEON(tc, selmon) || !tc->isfloating || tc == c)
				continue;
			if (c->geom.x + c->geom.width < tc->geom.x ||
				c->geom.x > tc->geom.x + tc->geom.width)
				continue;
			top = tc->geom.y - config.gappiv;
			if (buttom < top && (ny + c->geom.height) > top) {
				tar = ASTEROIDZ_MIN(tar, top - c->geom.height);
			};
		}
		ny = tar == 99999 ? ny : tar;
		ny = ASTEROIDZ_MIN(ny, c->mon->w.y + c->mon->w.height - c->geom.height -
							   c->mon->gappov);
		break;
	case LEFT:
		tar = -99999;
		left = c->geom.x;
		nx -= c->mon->w.width / 6;

		wl_list_for_each(tc, &clients, link) {
			if (!VISIBLEON(tc, selmon) || !tc->isfloating || tc == c)
				continue;
			if (c->geom.y + c->geom.height < tc->geom.y ||
				c->geom.y > tc->geom.y + tc->geom.height)
				continue;
			right = tc->geom.x + tc->geom.width + config.gappih;
			if (left > right && nx < right) {
				tar = ASTEROIDZ_MAX(tar, right);
			};
		}

		nx = tar == -99999 ? nx : tar;
		nx = ASTEROIDZ_MAX(nx, c->mon->w.x + c->mon->gappoh);
		break;
	case RIGHT:
		tar = 99999;
		right = c->geom.x + c->geom.width;
		nx += c->mon->w.width / 6;
		wl_list_for_each(tc, &clients, link) {
			if (!VISIBLEON(tc, selmon) || !tc->isfloating || tc == c)
				continue;
			if (c->geom.y + c->geom.height < tc->geom.y ||
				c->geom.y > tc->geom.y + tc->geom.height)
				continue;
			left = tc->geom.x - config.gappih;
			if (right < left && (nx + c->geom.width) > left) {
				tar = ASTEROIDZ_MIN(tar, left - c->geom.width);
			};
		}
		nx = tar == 99999 ? nx : tar;
		nx = ASTEROIDZ_MIN(nx, c->mon->w.x + c->mon->w.width - c->geom.width -
							   c->mon->gappoh);
		break;
	}

	c->float_geom = (struct wlr_box){
		.x = nx, .y = ny, .width = c->geom.width, .height = c->geom.height};
	c->iscustomsize = 1;
	resize(c, c->float_geom, 1);
	return 0;
}

int32_t smartresizewin(const Arg *arg) {
	Client *c = NULL, *tc = NULL;
	int32_t nw, nh;
	int32_t buttom, top, left, right, tar;
	if (!selmon)
		return 0;
	c = arg->tc ? arg->tc : selmon->sel;
	if (!c || c->isfullscreen)
		return 0;
	if (!c->isfloating)
		setfloating(c, true);
	nw = c->geom.width;
	nh = c->geom.height;

	switch (arg->i) {
	case UP:
		nh -= selmon->w.height / 8;
		nh = ASTEROIDZ_MAX(nh, selmon->w.height / 10);
		break;
	case DOWN:
		tar = -99999;
		buttom = c->geom.y + c->geom.height;
		nh += selmon->w.height / 8;

		wl_list_for_each(tc, &clients, link) {
			if (!VISIBLEON(tc, selmon) || !tc->isfloating || tc == c)
				continue;
			if (c->geom.x + c->geom.width < tc->geom.x ||
				c->geom.x > tc->geom.x + tc->geom.width)
				continue;
			top = tc->geom.y - config.gappiv;
			if (buttom < top && (nh + c->geom.y) > top) {
				tar = ASTEROIDZ_MAX(tar, top - c->geom.y);
			};
		}
		nh = tar == -99999 ? nh : tar;
		if (c->geom.y + nh + config.gappov > selmon->w.y + selmon->w.height)
			nh = selmon->w.y + selmon->w.height - c->geom.y - config.gappov;
		break;
	case LEFT:
		nw -= selmon->w.width / 16;
		nw = ASTEROIDZ_MAX(nw, selmon->w.width / 10);
		break;
	case RIGHT:
		tar = 99999;
		right = c->geom.x + c->geom.width;
		nw += selmon->w.width / 16;
		wl_list_for_each(tc, &clients, link) {
			if (!VISIBLEON(tc, selmon) || !tc->isfloating || tc == c)
				continue;
			if (c->geom.y + c->geom.height < tc->geom.y ||
				c->geom.y > tc->geom.y + tc->geom.height)
				continue;
			left = tc->geom.x - config.gappih;
			if (right < left && (nw + c->geom.x) > left) {
				tar = ASTEROIDZ_MIN(tar, left - c->geom.x);
			};
		}

		nw = tar == 99999 ? nw : tar;
		if (c->geom.x + nw + config.gappoh > selmon->w.x + selmon->w.width)
			nw = selmon->w.x + selmon->w.width - c->geom.x - config.gappoh;
		break;
	}

	c->float_geom = (struct wlr_box){
		.x = c->geom.x, .y = c->geom.y, .width = nw, .height = nh};
	c->iscustomsize = 1;
	resize(c, c->float_geom, 1);
	return 0;
}

int32_t centerwin(const Arg *arg) {
	Client *c = arg->tc ? arg->tc : (selmon ? selmon->sel : NULL);

	if (!c || c->isfullscreen || c->ismaximizescreen)
		return 0;

	if (c->isfloating) {
		c->float_geom = setclient_coordinate_center(c, c->mon, c->geom, 0, 0);
		c->iscustomsize = 1;
		resize(c, c->float_geom, 1);
		return 0;
	}

	if (!is_scroller_layout(selmon))
		return 0;

	Client *stack_head = scroll_get_stack_head_client(c);
	if (selmon->pertag->ltidxs[selmon->pertag->curtag]->id == SCROLLER) {
		stack_head->geom.x =
			selmon->w.x + (selmon->w.width - stack_head->geom.width) / 2;
	} else {
		stack_head->geom.y =
			selmon->w.y + (selmon->w.height - stack_head->geom.height) / 2;
	}

	arrange(selmon, false, false);
	return 0;
}

int32_t spawn_shell(const Arg *arg) {
	if (!arg->v)
		return 0;

	if (fork() == 0) {
		signal(SIGSEGV, SIG_DFL);
		signal(SIGABRT, SIG_DFL);
		signal(SIGILL, SIG_DFL);
		signal(SIGCHLD, SIG_DFL);

		int fd_max = sysconf(_SC_OPEN_MAX);
		for (int i = 3; i < fd_max; i++) {
			close(i);
		}

		dup2(STDERR_FILENO, STDOUT_FILENO);
		setsid();

		execlp("sh", "sh", "-c", arg->v, (char *)NULL);
		execlp("bash", "bash", "-c", arg->v, (char *)NULL);

		wlr_log(WLR_DEBUG,
				"asteroidz: failed to execute command '%s' with shell: %s\n",
				(char *)arg->v, strerror(errno));
		_exit(EXIT_FAILURE);
	}
	return 0;
}

int32_t spawn(const Arg *arg) {
	if (!arg->v)
		return 0;

	if (fork() == 0) {
		signal(SIGSEGV, SIG_DFL);
		signal(SIGABRT, SIG_DFL);
		signal(SIGILL, SIG_DFL);
		signal(SIGCHLD, SIG_DFL);

		// close all file descriptors inherited from the parent process to
		// prevent IPC handle leakage that can block clients
		int fd_max = sysconf(_SC_OPEN_MAX);
		for (int i = 3; i < fd_max; i++) {
			close(i);
		}

		dup2(STDERR_FILENO, STDOUT_FILENO);
		setsid();

		wordexp_t p;
		if (wordexp(arg->v, &p, 0) != 0) {
			wlr_log(WLR_DEBUG, "asteroidz: wordexp failed for '%s'\n",
					(char *)arg->v);
			_exit(EXIT_FAILURE);
		}

		execvp(p.we_wordv[0], p.we_wordv);

		wlr_log(WLR_DEBUG, "asteroidz: execvp '%s' failed: %s\n", p.we_wordv[0],
				strerror(errno));
		wordfree(&p);
		_exit(EXIT_FAILURE);
	}
	return 0;
}

int32_t spawn_on_empty(const Arg *arg) {
	bool is_empty = true;
	Client *c = NULL;

	wl_list_for_each(c, &clients, link) {
		if (arg->ui & c->tags && c->mon == selmon) {
			is_empty = false;
			break;
		}
	}
	if (!is_empty) {
		view(arg, true);
		return 0;
	} else {
		view(arg, true);
		spawn_shell(arg);
	}
	return 0;
}

int32_t switch_keyboard_layout(const Arg *arg) {
	if (!kb_group || !kb_group->wlr_group || !seat) {
		wlr_log(WLR_ERROR, "Invalid keyboard group or seat");
		return 0;
	}

	struct wlr_keyboard *keyboard = &kb_group->wlr_group->keyboard;
	if (!keyboard || !keyboard->keymap) {
		wlr_log(WLR_ERROR, "Invalid keyboard or keymap");
		return 0;
	}

	xkb_layout_index_t current = xkb_state_serialize_layout(
		keyboard->xkb_state, XKB_STATE_LAYOUT_EFFECTIVE);
	const int32_t num_layouts = xkb_keymap_num_layouts(keyboard->keymap);
	if (num_layouts < 2) {
		wlr_log(WLR_INFO, "Only one layout available");
		return 0;
	}

	xkb_layout_index_t next = 0;
	if (arg->i > 0 && arg->i <= num_layouts) {
		next = arg->i - 1;
	} else {
		next = (current + 1) % num_layouts;
	}

	uint32_t depressed = keyboard->modifiers.depressed;
	uint32_t latched = keyboard->modifiers.latched;
	uint32_t locked = keyboard->modifiers.locked;

	wlr_keyboard_notify_modifiers(keyboard, depressed, latched, locked, next);

	wlr_seat_set_keyboard(seat, keyboard);
	wlr_seat_keyboard_notify_modifiers(seat, &keyboard->modifiers);

	InputDevice *id;
	wl_list_for_each(id, &inputdevices, link) {
		if (id->wlr_device->type != WLR_INPUT_DEVICE_KEYBOARD) {
			continue;
		}

		struct wlr_keyboard *tkb = (struct wlr_keyboard *)id->device_data;

		wlr_keyboard_notify_modifiers(tkb, depressed, latched, locked, next);
		wlr_seat_set_keyboard(seat, tkb);
		wlr_seat_keyboard_notify_modifiers(seat, &tkb->modifiers);
	}

	printstatus(IPC_WATCH_KB_LAYOUT);
	return 0;
}

/* Flip the compositor's manual idle inhibit. Unlike the idle_inhibit_v1
 * protocol this needs no surface, so a bar module (or a keybind) can hold the
 * screen awake without owning a window. `arg->i` forces a state when it is 0
 * or 1; anything else toggles. */
int32_t toggle_idle_inhibit(const Arg *arg) {
	if (arg && (arg->i == 0 || arg->i == 1))
		idle_inhibit_manual = arg->i == 1;
	else
		idle_inhibit_manual = !idle_inhibit_manual;
	checkidleinhibitor(NULL);
	printstatus(IPC_WATCH_ARRANGGE);
	return 1;
}

int32_t switch_layout(const Arg *arg) {

	int32_t jk, ji;
	char *target_layout_name = NULL;
	uint32_t len;

	if (!selmon)
		return 0;

	if (config.circle_layout_count != 0) {
		for (jk = 0; jk < config.circle_layout_count; jk++) {

			len = ASTEROIDZ_MAX(
				strlen(config.circle_layout[jk]),
				strlen(selmon->pertag->ltidxs[selmon->pertag->curtag]->name));

			if (strncmp(config.circle_layout[jk],
						selmon->pertag->ltidxs[selmon->pertag->curtag]->name,
						len) == 0) {
				target_layout_name = jk == config.circle_layout_count - 1
										 ? config.circle_layout[0]
										 : config.circle_layout[jk + 1];
				break;
			}
		}

		if (!target_layout_name) {
			target_layout_name = config.circle_layout[0];
		}

		for (ji = 0; ji < LENGTH(layouts); ji++) {
			len =
				ASTEROIDZ_MAX(strlen(layouts[ji].name), strlen(target_layout_name));
			if (strncmp(layouts[ji].name, target_layout_name, len) == 0) {
				selmon->pertag->ltidxs[selmon->pertag->curtag] = &layouts[ji];

				break;
			}
		}
		clear_fullscreen_and_maximized_state(selmon);
		arrange(selmon, false, false);
		printstatus(IPC_WATCH_ARRANGGE);
		return 0;
	}

	for (jk = 0; jk < LENGTH(layouts); jk++) {
		if (strcmp(layouts[jk].name,
				   selmon->pertag->ltidxs[selmon->pertag->curtag]->name) == 0) {
			selmon->pertag->ltidxs[selmon->pertag->curtag] =
				jk == LENGTH(layouts) - 1 ? &layouts[0] : &layouts[jk + 1];
			clear_fullscreen_and_maximized_state(selmon);
			arrange(selmon, false, false);
			printstatus(IPC_WATCH_ARRANGGE);
			return 0;
		}
	}
	return 0;
}

int32_t tag(const Arg *arg) {
	if (!selmon)
		return 0;
	Client *target_client = arg->tc ? arg->tc : selmon->sel;
	tag_client(arg, target_client);
	return 0;
}

int32_t tagmon(const Arg *arg) {
	Monitor *m = NULL, *cm = NULL, *oldmon = NULL;
	if (!selmon)
		return 0;
	Client *c = arg->tc ? arg->tc : focustop(selmon);

	if (!c)
		return 0;

	oldmon = c->mon;

	if (arg->i != UNDIR) {
		m = dirtomon(arg->i);
	} else if (arg->v) {
		wl_list_for_each(cm, &mons, link) {
			if (!cm->wlr_output->enabled) {
				continue;
			}
			if (match_monitor_spec(arg->v, cm)) {
				m = cm;
				break;
			}
		}
	} else {
		return 0;
	}

	if (!m || !m->wlr_output->enabled)
		return 0;

	uint32_t newtags = arg->ui ? arg->ui : arg->i2 ? c->tags : 0;
	uint32_t target;

	if (c->mon == m) {
		view(&(Arg){.ui = newtags}, true);
		return 0;
	}

	if (c == oldmon->sel) {
		oldmon->sel = NULL;
	}

	setmon(c, m, newtags, true);
	client_update_oldmonname_record(c, m);

	reset_foreign_tolevel(c, oldmon, c->mon);

	c->float_geom.width =
		(int32_t)(c->float_geom.width * c->mon->w.width / oldmon->w.width);
	c->float_geom.height =
		(int32_t)(c->float_geom.height * c->mon->w.height / oldmon->w.height);
	selmon = c->mon;
	c->float_geom = setclient_coordinate_center(c, c->mon, c->float_geom, 0, 0);

	if (c->isfloating) {
		c->geom = c->float_geom;
		target = get_tags_first_tag(c->tags);
		view(&(Arg){.ui = target}, true);
		focusclient(c, 1);
		resize(c, c->geom, 1);
	} else {
		selmon = c->mon;
		target = get_tags_first_tag(c->tags);
		view(&(Arg){.ui = target}, true);
		focusclient(c, 1);
		arrange(selmon, false, false);
	}
	if (config.warpcursor) {
		warp_cursor_to_selmon(c->mon);
	}
	return 0;
}

int32_t tagsilent(const Arg *arg) {
	Client *fc = NULL;
	Client *target_client = arg->tc ? arg->tc : (selmon ? selmon->sel : NULL);

	if (!target_client)
		return 0;

	target_client->tags = arg->ui & TAGMASK;
	wl_list_for_each(fc, &clients, link) {
		if (fc && fc != target_client && target_client->tags & fc->tags &&
			ISFULLSCREEN(fc) && !target_client->isfloating) {
			clear_fullscreen_flag(fc);
		}
	}
	focusclient(focustop(selmon), 1);
	arrange(target_client->mon, false, false);
	return 0;
}

int32_t tagtoleft(const Arg *arg) {
	if (!selmon)
		return 0;

	Client *sel = arg->tc ? arg->tc : selmon->sel;
	if (sel != NULL &&
		__builtin_popcount(selmon->tagset[selmon->seltags] & TAGMASK) == 1) {
		uint32_t target = selmon->tagset[selmon->seltags] >> 1;

		if (target == 0) {
			if (!config.tag_carousel)
				return 0;
			target = (1 << (LENGTH(tags) - 1)) & TAGMASK;
			selmon->carousel_anim_dir = -1;
		}

		Arg a = {.ui = target & TAGMASK, .i = arg->i, .tc = sel};
		tag(&a);
		selmon->carousel_anim_dir = 0;
	}
	return 0;
}

int32_t tagtoright(const Arg *arg) {
	if (!selmon)
		return 0;

	Client *sel = arg->tc ? arg->tc : selmon->sel;
	if (sel != NULL &&
		__builtin_popcount(selmon->tagset[selmon->seltags] & TAGMASK) == 1) {
		uint32_t target = selmon->tagset[selmon->seltags] << 1;

		if (!(target & TAGMASK)) {
			if (!config.tag_carousel)
				return 0;
			target = 1;
			selmon->carousel_anim_dir = 1;
		}

		Arg a = {.ui = target & TAGMASK, .i = arg->i, .tc = sel};
		tag(&a);
		selmon->carousel_anim_dir = 0;
	}
	return 0;
}

int32_t toggle_named_scratchpad(const Arg *arg) {
	Client *target_client = NULL;
	char *arg_id = arg->v;
	char *arg_title = arg->v2;

	if (selmon && selmon->isoverview)
		return 0;

	target_client = get_client_by_id_or_title(arg_id, arg_title);

	if (!target_client && arg->v3) {
		Arg arg_spawn = {.v = arg->v3};
		spawn_shell(&arg_spawn);
		return 0;
	}

	target_client->isnamedscratchpad = 1;
	apply_named_scratchpad(target_client);
	return 0;
}

int32_t toggle_render_border(const Arg *arg) {
	if (!selmon)
		return 0;
	render_border = !render_border;
	arrange(selmon, false, false);
	return 0;
}

int32_t toggle_scratchpad(const Arg *arg) {
	Client *c = NULL;
	bool hit = false;
	Client *tmp = NULL;

	if (selmon && selmon->isoverview)
		return 0;

	wl_list_for_each_safe(c, tmp, &clients, link) {
		if (!config.scratchpad_cross_monitor && c->mon != selmon) {
			continue;
		}

		if (config.single_scratchpad && c->isnamedscratchpad &&
			!c->isminimized) {
			set_minimized(c);
			continue;
		}

		if (c->isnamedscratchpad)
			continue;

		if (hit)
			continue;

		hit = switch_scratchpad_client_state(c);
	}
	return 0;
}

/* Toggle the named special workspace `arg->v` on the focused monitor.
 * If it is already the one showing there, slide it back out; otherwise
 * slide it in, implicitly closing any other special workspace that may
 * already be showing on that monitor (a monitor only ever shows one at a
 * time). The workspace does not need to contain any client yet: toggling
 * an empty name just opens/closes an empty overlay that clients can later
 * be moved into with movetospecialworkspace. */
int32_t togglespecialworkspace(const Arg *arg) {
	char *interned = NULL;

	if (!selmon || selmon->isoverview)
		return 0;

	interned = intern_special_workspace_name(arg->v);
	if (!interned)
		return 0;

	if (selmon->active_special == interned)
		close_special_workspace(selmon, true);
	else
		open_special_workspace(selmon, interned, true);

	return 0;
}

/* Move the focused client into named special workspace `arg->v`. With an
 * empty/NULL name, move the client back out into its normal tag view
 * (its existing Client::tags is untouched the whole time it is in a
 * special workspace, so this just needs to clear special_name for the
 * client to reappear wherever it already was tagged). */
int32_t movetospecialworkspace(const Arg *arg) {
	Client *c = NULL;
	char *interned = NULL;

	if (!selmon || selmon->isoverview)
		return 0;

	c = arg->tc ? arg->tc : selmon->sel;
	if (!c || !c->mon)
		return 0;

	if (!arg->v || !arg->v[0]) {
		if (!c->special_name)
			return 0;
		c->special_name = NULL;
		arrange(c->mon, false, false);
		focusclient(c, true);
		printstatus(IPC_WATCH_ARRANGGE);
		return 0;
	}

	interned = intern_special_workspace_name(arg->v);
	if (!interned)
		return 0;

	c->special_name = interned;
	arrange(c->mon, false, false);
	printstatus(IPC_WATCH_ARRANGGE);
	return 0;
}

int32_t togglefakefullscreen(const Arg *arg) {
	if (!selmon)
		return 0;
	Client *sel = arg->tc ? arg->tc : focustop(selmon);
	if (sel)
		setfakefullscreen(sel, !sel->isfakefullscreen);
	return 0;
}

int32_t togglefloating(const Arg *arg) {
	if (!selmon)
		return 0;

	Client *sel = arg->tc ? arg->tc : focustop(selmon);

	if (selmon && selmon->isoverview)
		return 0;

	if (!sel)
		return 0;

	bool isfloating = sel->isfloating;

	if ((sel->isfullscreen || sel->ismaximizescreen)) {
		isfloating = 1;
	} else {
		isfloating = !sel->isfloating;
	}

	setfloating(sel, isfloating);
	return 0;
}

int32_t togglefullscreen(const Arg *arg) {
	if (!selmon)
		return 0;

	Client *sel = arg->tc ? arg->tc : focustop(selmon);
	if (!sel)
		return 0;

	sel->is_scratchpad_show = 0;
	sel->is_in_scratchpad = 0;
	sel->isnamedscratchpad = 0;

	if (sel->isfullscreen)
		setfullscreen(sel, 0, true);
	else
		setfullscreen(sel, 1, true);
	return 0;
}

int32_t toggleglobal(const Arg *arg) {
	if (!selmon)
		return 0;

	Client *c = arg->tc ? arg->tc : selmon->sel;
	if (!c)
		return 0;

	if (c->is_in_scratchpad) {
		c->is_in_scratchpad = 0;
		c->is_scratchpad_show = 0;
		c->isnamedscratchpad = 0;
	}
	c->isglobal ^= 1;
	setborder_color(c);
	return 0;
}

int32_t togglegaps(const Arg *arg) {
	if (!selmon)
		return 0;

	enablegaps ^= 1;
	arrange(selmon, false, false);
	return 0;
}

int32_t togglemaximizescreen(const Arg *arg) {
	if (!selmon)
		return 0;

	Client *sel = arg->tc ? arg->tc : focustop(selmon);
	if (!sel)
		return 0;

	sel->is_scratchpad_show = 0;
	sel->is_in_scratchpad = 0;
	sel->isnamedscratchpad = 0;

	if (sel->ismaximizescreen)
		setmaximizescreen(sel, 0, true);
	else
		setmaximizescreen(sel, 1, true);

	setborder_color(sel);
	return 0;
}

int32_t toggleoverlay(const Arg *arg) {
	if (!selmon)
		return 0;

	Client *c = arg->tc ? arg->tc : selmon->sel;
	if (!c || !c->mon || c->isfullscreen) {
		return 0;
	}

	c->isoverlay ^= 1;

	if (c->isoverlay) {
		wlr_scene_node_reparent(&c->scene->node, layers[LyrOverlay]);
		wlr_scene_node_raise_to_top(&c->scene->node);
	} else if (client_should_overtop(c) && c->isfloating) {
		wlr_scene_node_reparent(&c->scene->node, layers[LyrTop]);
	} else {
		wlr_scene_node_reparent(&c->scene->node,
								layers[c->isfloating ? LyrTop : LyrTile]);
	}

	setborder_color(c);
	return 0;
}

int32_t toggletag(const Arg *arg) {
	if (!selmon)
		return 0;

	uint32_t newtags;
	Client *sel = arg->tc ? arg->tc : focustop(selmon);
	if (!sel)
		return 0;

	if ((int32_t)arg->ui == INT_MIN && sel->tags != (~0 & TAGMASK)) {
		newtags = ~0 & TAGMASK;
	} else if ((int32_t)arg->ui == INT_MIN && sel->tags == (~0 & TAGMASK)) {
		newtags = 1 << (sel->mon->pertag->curtag - 1);
	} else {
		newtags = sel->tags ^ (arg->ui & TAGMASK);
	}

	if (newtags) {
		sel->tags = newtags;
		focusclient(focustop(selmon), 1);
		arrange(selmon, false, false);
	}
	printstatus(IPC_WATCH_ARRANGGE);
	return 0;
}

int32_t toggleview(const Arg *arg) {
	if (!selmon)
		return 0;

	uint32_t newtagset;
	uint32_t target;
	Client *c = NULL;

	target = arg->ui == 0 ? ~0 & TAGMASK : arg->ui;

	newtagset = selmon->tagset[selmon->seltags] ^ (target & TAGMASK);

	if (newtagset) {
		selmon->tagset[selmon->seltags] = newtagset;
		focusclient(focustop(selmon), 1);
		wl_list_for_each(c, &clients, link) {
			if (VISIBLEON(c, selmon) && ISTILED(c)) {
				set_size_per(selmon, c);
			}
		}
		arrange(selmon, false, false);
	}
	printstatus(IPC_WATCH_ARRANGGE);
	return 0;
}

int32_t viewtoleft(const Arg *arg) {
	if (!selmon)
		return 0;

	if (selmon->isoverview || selmon->pertag->curtag == 0)
		return 0;

	uint32_t target = selmon->tagset[selmon->seltags];
	target >>= 1;

	if (target == 0) {
		if (!config.tag_carousel)
			return 0;
		target = (1 << (LENGTH(tags) - 1)) & TAGMASK;
		selmon->carousel_anim_dir = -1;
	}

	if (target == selmon->tagset[selmon->seltags])
		return 0;

	view(&(Arg){.ui = target & TAGMASK, .i = arg->i}, true);
	selmon->carousel_anim_dir = 0;
	return 0;
}

int32_t viewtoright(const Arg *arg) {
	if (!selmon)
		return 0;

	if (selmon->isoverview || selmon->pertag->curtag == 0)
		return 0;

	uint32_t target = selmon->tagset[selmon->seltags];
	target <<= 1;

	if (!(target & TAGMASK)) {
		if (!config.tag_carousel)
			return 0;
		target = 1;
		selmon->carousel_anim_dir = 1;
	}

	if (target == selmon->tagset[selmon->seltags])
		return 0;

	view(&(Arg){.ui = target & TAGMASK, .i = arg->i}, true);
	selmon->carousel_anim_dir = 0;
	return 0;
}

int32_t viewtoleft_have_client(const Arg *arg) {
	if (!selmon)
		return 0;

	if (selmon->isoverview)
		return 0;

	uint32_t n;
	uint32_t current = get_tags_first_tag_num(selmon->tagset[selmon->seltags]);
	bool found = false;
	bool wrapped = false;

	for (n = current - 1; n >= 1; n--) {
		if (get_tag_status(n, selmon)) {
			found = true;
			break;
		}
	}

	if (!found && config.tag_carousel) {
		for (n = LENGTH(tags); n > current; n--) {
			if (get_tag_status(n, selmon)) {
				found = true;
				wrapped = true;
				break;
			}
		}
	}

	if (found) {
		if (wrapped)
			selmon->carousel_anim_dir = -1;
		view(&(Arg){.ui = (1 << (n - 1)) & TAGMASK, .i = arg->i}, true);
		selmon->carousel_anim_dir = 0;
	}
	return 0;
}

int32_t viewtoright_have_client(const Arg *arg) {
	if (!selmon)
		return 0;

	if (selmon->isoverview)
		return 0;

	uint32_t n;
	uint32_t current = get_tags_first_tag_num(selmon->tagset[selmon->seltags]);
	bool found = false;
	bool wrapped = false;

	for (n = current + 1; n <= LENGTH(tags); n++) {
		if (get_tag_status(n, selmon)) {
			found = true;
			break;
		}
	}

	if (!found && config.tag_carousel) {
		for (n = 1; n < current; n++) {
			if (get_tag_status(n, selmon)) {
				found = true;
				wrapped = true;
				break;
			}
		}
	}

	if (found) {
		if (wrapped)
			selmon->carousel_anim_dir = 1;
		view(&(Arg){.ui = (1 << (n - 1)) & TAGMASK, .i = arg->i}, true);
		selmon->carousel_anim_dir = 0;
	}
	return 0;
}

int32_t viewcrossmon(const Arg *arg) {
	if (!selmon)
		return 0;

	focusmon(&(Arg){.v = arg->v, .i = UNDIR});
	view_in_mon(arg, true, selmon, true);
	return 0;
}

int32_t tagcrossmon(const Arg *arg) {
	if (!selmon)
		return 0;

	Client *c = arg->tc ? arg->tc : selmon->sel;
	if (!c)
		return 0;

	if (match_monitor_spec(arg->v, selmon)) {
		tag_client(arg, c);
		return 0;
	}

	Arg a = {.ui = arg->ui, .i = UNDIR, .v = arg->v, .tc = c};
	tagmon(&a);
	return 0;
}

int32_t comboview(const Arg *arg) {
	uint32_t newtags = arg->ui & TAGMASK;

	if (!newtags || !selmon)
		return 0;

	if (tag_combo) {
		selmon->tagset[selmon->seltags] |= newtags;
		focusclient(focustop(selmon), 1);
		arrange(selmon, false, false);
	} else {
		tag_combo = true;
		view(&(Arg){.ui = newtags}, false);
	}

	printstatus(IPC_WATCH_ARRANGGE);
	return 0;
}

int32_t zoom(const Arg *arg) {
	Client *c = NULL, *sel = arg->tc ? arg->tc : focustop(selmon);

	if (!sel || !selmon ||
		!selmon->pertag->ltidxs[selmon->pertag->curtag]->arrange ||
		sel->isfloating)
		return 0;

	wl_list_for_each(c, &clients,
					 link) if (VISIBLEON(c, selmon) && !c->isfloating) {
		if (c != sel)
			break;
		sel = NULL;
	}

	if (&c->link == &clients)
		return 0;

	if (!sel)
		sel = c;
	wl_list_remove(&sel->link);
	wl_list_insert(&clients, &sel->link);

	focusclient(sel, 1);
	arrange(selmon, false, false);
	return 0;
}

int32_t zoom_in(const Arg *arg) {
	float step = arg->f > 0.0f ? arg->f : config.cursor_zoom_step;
	cursor_zoom_set_factor(cursor_zoom_factor + step);
	return 0;
}

int32_t zoom_out(const Arg *arg) {
	float step = arg->f > 0.0f ? arg->f : config.cursor_zoom_step;
	cursor_zoom_set_factor(cursor_zoom_factor - step);
	return 0;
}

int32_t zoom_reset(const Arg *arg) {
	cursor_zoom_set_factor(1.0f);
	return 0;
}

int32_t setoption(const Arg *arg) {
	parse_option(&config, arg->v, arg->v2);
	override_config();
	/* Not reset_option(): that respawns every `spawn` entry, which is right for
	 * a reload and wrong for one option changing. See config_apply_live(). */
	config_apply_live();
	/* Memory-only, but still a change someone watching should see -- and the
	 * push carries source.kind == "runtime", which is how a panel can show that
	 * this value will not survive a reload. */
	ipc_notify_config("set");
	return 0;
}

int32_t minimized(const Arg *arg) {
	if (!selmon)
		return 0;

	if (selmon && selmon->isoverview)
		return 0;

	Client *c = arg->tc ? arg->tc : selmon->sel;
	if (c && !c->isminimized) {
		set_minimized(c);
	}
	return 0;
}

void fix_mon_tagset_from_overview(Monitor *m) {
	if (m->tagset[m->seltags] == (m->ovbk_prev_tagset & TAGMASK)) {
		m->tagset[m->seltags ^ 1] = m->ovbk_current_tagset;
		m->pertag->prevtag = get_tags_first_tag_num(m->ovbk_current_tagset);
	} else {
		m->tagset[m->seltags ^ 1] = m->ovbk_prev_tagset;
		m->pertag->prevtag = get_tags_first_tag_num(m->ovbk_prev_tagset);
	}
}

int32_t toggleoverview(const Arg *arg) {
	Client *c = NULL;
	if (!selmon)
		return 0;

	Client *sel = arg->tc ? arg->tc : selmon->sel;

	if (selmon->isoverview && config.ov_tab_mode && !selmon->is_jump_mode &&
		arg->i != 1 && sel) {
		focusstack(&(Arg){.i = 1});
		return 0;
	}

	selmon->isoverview ^= 1;
	uint32_t target;
	uint32_t visible_client_number = 0;

	if (!selmon->isoverview && selmon->is_jump_mode) {
		finish_jump_mode(selmon);
	}

	if (selmon->isoverview) {
		wl_list_for_each(c, &clients, link) if (c && c->mon == selmon &&
												!client_is_unmanaged(c) &&
												!client_is_x11_popup(c) &&
												!c->isminimized &&
												!c->isunglobal) {
			visible_client_number++;
		}
		if (visible_client_number > 0) {
			selmon->ovbk_current_tagset = selmon->tagset[selmon->seltags];
			selmon->ovbk_prev_tagset = selmon->tagset[selmon->seltags ^ 1];
			selmon->ov_preview_tag = 0; /* start previewing the current tag */
			selmon->ov_scroll_tag = 0;  /* recompute the big-area scroll offset */
			target = ~0 & TAGMASK;
		} else {
			selmon->isoverview ^= 1;
			return 0;
		}
	} else if (!selmon->isoverview && sel) {
		target = get_tags_first_tag(sel->tags);
	} else if (!selmon->isoverview && !sel) {
		target = (1 << (selmon->pertag->prevtag - 1));
		view(&(Arg){.ui = target}, false);
		fix_mon_tagset_from_overview(selmon);
		refresh_monitors_workspaces_status(selmon);
		return 0;
	}

	if (selmon->isoverview) {
		wlr_seat_pointer_clear_focus(seat);

		if (cursor_hidden) {
			handlecursoractivity();
		} else {
			az_cursor_set_xcursor("default");
		}

		wl_list_for_each(c, &clients, link) {
			if (c && c->mon == selmon && !client_is_unmanaged(c) &&
				!client_is_x11_popup(c) && !c->isunglobal && !c->isminimized &&
				client_surface(c)->mapped) {
				c->animation.overining = true;
				overview_backup(c);
			}
		}
	} else {

		selmon->tagset[selmon->seltags] = target;
		wl_list_for_each(c, &clients, link) {
			if (c && c->mon == selmon && !c->iskilling &&
				!client_is_unmanaged(c) && !c->isunglobal && !c->isminimized &&
				!client_is_x11_popup(c) && client_surface(c)->mapped) {
				overview_restore(c, &(Arg){.ui = target});
			}
		}
	}

	/* kick off the chrome zoom-fade (in on open, out on close). Set before
	 * view()->arrange() so a close-fade keeps the chrome nodes alive. */
	overview_anim_start(selmon, selmon->isoverview);

	view(&(Arg){.ui = target}, false);
	fix_mon_tagset_from_overview(selmon);
	refresh_monitors_workspaces_status(selmon);

	return 0;
}

int32_t togglejump(const Arg *arg) {
	if (!selmon)
		return 0;

	if (!selmon->isoverview) {
		begin_jump_mode(selmon);
		toggleoverview(arg);
		return 0;
	}

	if (selmon->isoverview) {
		toggleoverview(arg);
	}

	return 0;
}

/* Rename the currently-selected tag on selmon. arg->v is the new name
 * (empty/NULL clears it back to the tag number). Reachable from a keybind
 * or `amsg dispatch set_tag_name,<name>`. */
int32_t set_tag_name(const Arg *arg) {
	if (!selmon || !selmon->pertag)
		return 0;
	uint32_t tag = selmon->pertag->curtag;
	if (tag > LENGTH(tags))
		return 0;

	free(selmon->pertag->names[tag]);
	if (arg && arg->v && *(const char *)arg->v)
		selmon->pertag->names[tag] = strdup((const char *)arg->v);
	else
		selmon->pertag->names[tag] = NULL;

	/* refresh the overview label (if showing) and notify the bar/IPC */
	arrange(selmon, false, false);
	printstatus(IPC_WATCH_ARRANGGE);
	return 0;
}

int32_t disable_monitor(const Arg *arg) {
	Monitor *m = NULL;
	bool matched = false;
	wl_list_for_each(m, &mons, link) {
		if (match_monitor_spec(arg->v, m)) {
			struct wlr_output_state state = {0};
			wlr_output_state_set_enabled(&state, false);
			wlr_output_commit_state(m->wlr_output, &state);
			/* full disable: remove from layout (asleep keeps it) */
			m->asleep = 0;
			matched = true;
		}
	}
	if (matched)
		updatemons(NULL, NULL);
	return 0;
}

/* power off only (DPMS): output stays in the layout, windows stay put */
int32_t dpms_off_monitor(const Arg *arg) {
	Monitor *m = NULL;
	bool matched = false;
	wl_list_for_each(m, &mons, link) {
		if (match_monitor_spec(arg->v, m)) {
			struct wlr_output_state state = {0};
			wlr_output_state_set_enabled(&state, false);
			wlr_output_commit_state(m->wlr_output, &state);
			m->asleep = 1;
			matched = true;
		}
	}
	if (matched)
		updatemons(NULL, NULL);
	return 0;
}

int32_t dpms_on_monitor(const Arg *arg) {
	Monitor *m = NULL;
	bool matched = false;
	wl_list_for_each(m, &mons, link) {
		if (match_monitor_spec(arg->v, m)) {
			struct wlr_output_state state = {0};
			wlr_output_state_set_enabled(&state, true);
			bool committed = wlr_output_commit_state(m->wlr_output, &state);
			m->asleep = 0;
			matched = true;
			/* mirrors powermgrsetmode: some DSC panels come back with a
			 * corrupted decoder after DPMS, so this needs the same
			 * mode-cycle recovery as the wlr_output_power_manager_v1 path */
			if (config.dpms_wake_retrain || !committed)
				monitor_start_retrain(m, committed ? 700 : 50);
		}
	}
	if (matched)
		updatemons(NULL, NULL);
	return 0;
}

/* force a DSC/link retrain via a mode cycle (replaces the VT-switch trick) */
int32_t retrain_monitor(const Arg *arg) {
	Monitor *m = NULL;
	wl_list_for_each(m, &mons, link) {
		if (match_monitor_spec(arg->v, m))
			monitor_start_retrain(m, 1);
	}
	return 0;
}

int32_t dpms_toggle_monitor(const Arg *arg) {
	Monitor *m = NULL;
	bool matched = false;
	wl_list_for_each(m, &mons, link) {
		if (match_monitor_spec(arg->v, m)) {
			struct wlr_output_state state = {0};
			wlr_output_state_set_enabled(&state, !m->wlr_output->enabled);
			wlr_output_commit_state(m->wlr_output, &state);
			m->asleep = m->wlr_output->enabled ? 0 : 1;
			matched = true;
		}
	}
	if (matched)
		updatemons(NULL, NULL);
	return 0;
}

int32_t enable_monitor(const Arg *arg) {
	Monitor *m = NULL;
	bool matched = false;
	wl_list_for_each(m, &mons, link) {
		if (match_monitor_spec(arg->v, m)) {
			struct wlr_output_state state = {0};
			wlr_output_state_set_enabled(&state, true);
			bool committed = wlr_output_commit_state(m->wlr_output, &state);
			m->asleep = 0;
			matched = true;
			if (config.dpms_wake_retrain || !committed)
				monitor_start_retrain(m, committed ? 700 : 50);
		}
	}
	if (matched)
		updatemons(NULL, NULL);
	return 0;
}

int32_t toggle_monitor(const Arg *arg) {
	Monitor *m = NULL;
	bool matched = false;
	wl_list_for_each(m, &mons, link) {
		if (match_monitor_spec(arg->v, m)) {
			struct wlr_output_state state = {0};
			wlr_output_state_set_enabled(&state, !m->wlr_output->enabled);
			wlr_output_commit_state(m->wlr_output, &state);
			m->asleep = !m->wlr_output->enabled;
			matched = true;
		}
	}
	if (matched)
		updatemons(NULL, NULL);
	return 0;
}

int32_t scroller_apply_stack(Client *c, Client *target_client,
							 int32_t direction) {
	if (!c || !c->mon || c->isfloating || !is_scroller_layout(c->mon))
		return 0;

	Monitor *m = c->mon;
	uint32_t tag = m->pertag->curtag;

	bool is_horizontal = (m->pertag->ltidxs[tag]->id == SCROLLER);

	if (is_horizontal && (direction == UP || direction == DOWN))
		return 0;
	if (!is_horizontal && (direction == LEFT || direction == RIGHT))
		return 0;

	struct TagScrollerState *st = ensure_scroller_state(m, tag);

	struct ScrollerStackNode *cnode = find_scroller_node(st, c);

	if (!cnode)
		return 0;

	struct ScrollerStackNode *tnode =
		target_client ? find_scroller_node(st, target_client) : NULL;

	if (direction == UNDIR && target_client && target_client->mon == c->mon) {
		scroller_insert_stack(c, target_client, false);
		return 0;
	}

	if (cnode->prev_in_stack || cnode->next_in_stack) {
		struct ScrollerStackNode *move_out_refer_node =
			cnode->prev_in_stack ? cnode->prev_in_stack : cnode->next_in_stack;
		scroller_node_remove(st, cnode);

		update_scroller_state(c->mon);

		Client *stack_head =
			scroll_get_stack_head_client(move_out_refer_node->client);
		Client *stack_tail =
			scroll_get_stack_tail_client(move_out_refer_node->client);

		if (direction == LEFT || direction == UP) {
			if (c != stack_head) {
				wl_list_safe_reinsert_prev(&stack_head->link, &c->link);
			}
		} else if (direction == RIGHT || direction == DOWN) {
			if (c != stack_tail) {
				wl_list_safe_reinsert_next(&stack_tail->link, &c->link);
			}
		}
		sync_scroller_state_to_clients(m, tag);
		arrange(m, false, false);
		return 0;
	}

	if (!tnode || target_client->mon != c->mon)
		return 0;

	struct ScrollerStackNode *tail = tnode;
	while (tail->next_in_stack)
		tail = tail->next_in_stack;

	scroller_insert_stack(c, tail->client, false);

	if (c != tail->client) {
		wl_list_remove(&c->link);
		wl_list_insert(&tail->client->link, &c->link);
	}
	return 0;
}

int32_t scroller_stack(const Arg *arg) {
	if (!selmon)
		return 0;
	Client *c = arg->tc ? arg->tc : selmon->sel;
	if (!c || !c->mon || c->isfloating || !is_scroller_layout(selmon))
		return 0;

	Client *target_client = find_client_by_direction(c, arg, false);

	return scroller_apply_stack(c, target_client, arg->i);
}

/* pull the head window of the next column into the tail of the stack containing the focused window */
int32_t scroller_consume(const Arg *arg) {
	Client *c, *tc, *next_head = NULL;
	if (!selmon || selmon->isoverview || !is_scroller_layout(selmon))
		return 0;

	c = arg->tc ? arg->tc : selmon->sel;
	if (!c || !c->mon || c->isfloating || !ISSCROLLTILED(c) ||
		!VISIBLEON(c, selmon))
		return 0;

	/* pinned windows don't participate in stacking */
	if (c->ispinned)
		return 0;

	Monitor *m = c->mon;
	uint32_t tag = m->pertag->curtag;
	struct TagScrollerState *st = ensure_scroller_state(m, tag);
	if (!find_scroller_node(st, c))
		return 0;

	Client *tail = scroll_get_stack_tail_client(c);

	/* the stack is contiguous in the client list,
	 * so the first visible tiled window after the tail is the head of the next column */
	bool passed = false;
	wl_list_for_each(tc, &clients, link) {
		if (tc == tail) {
			passed = true;
			continue;
		}
		if (!passed || !VISIBLEON(tc, m) || !ISSCROLLTILED(tc))
			continue;
		next_head = tc;
		break;
	}

	if (!next_head || next_head->ispinned)
		return 0;

	scroller_apply_stack(next_head, tail, UNDIR);
	return 0;
}

/* pop the focused window out of the stack, making it its own column right after the current one */
int32_t scroller_expel(const Arg *arg) {
	Client *c;
	if (!selmon || selmon->isoverview || !is_scroller_layout(selmon))
		return 0;

	c = arg->tc ? arg->tc : selmon->sel;
	if (!c || !c->mon || c->isfloating || !ISSCROLLTILED(c) ||
		!VISIBLEON(c, selmon))
		return 0;

	/* pinned windows don't participate in stacking */
	if (c->ispinned)
		return 0;

	Monitor *m = c->mon;
	uint32_t tag = m->pertag->curtag;
	struct TagScrollerState *st = ensure_scroller_state(m, tag);
	struct ScrollerStackNode *cnode = find_scroller_node(st, c);

	/* nothing to do if it's not in a stack */
	if (!cnode || (!cnode->prev_in_stack && !cnode->next_in_stack))
		return 0;

	struct ScrollerStackNode *refer_node =
		cnode->prev_in_stack ? cnode->prev_in_stack : cnode->next_in_stack;
	scroller_node_remove(st, cnode);

	update_scroller_state(m);

	/* after popping, place it after the original stack's tail, making it the next column */
	Client *stack_tail = scroll_get_stack_tail_client(refer_node->client);
	if (c != stack_tail) {
		wl_list_safe_reinsert_next(&stack_tail->link, &c->link);
	}

	sync_scroller_state_to_clients(m, tag);
	arrange(m, false, false);
	return 0;
}

int32_t toggle_all_floating(const Arg *arg) {
	if (!selmon)
		return 0;

	Client *ref = arg->tc ? arg->tc : selmon->sel;
	if (!ref)
		return 0;

	bool should_floating = !ref->isfloating;

	Client *c;
	wl_list_for_each(c, &clients, link) {
		if (VISIBLEON(c, selmon)) {
			if (c->isfloating && !should_floating) {
				c->old_master_inner_per = 0.0f;
				c->old_stack_inner_per = 0.0f;
				set_size_per(selmon, c);
			}

			if (c->isfloating != should_floating) {
				setfloating(c, should_floating);
			}
		}
	}
	return 0;
}

int32_t dwindle_set_split_direction(Client *c, bool istoggle, bool horizontal) {
	const Layout *layout = c->mon->pertag->ltidxs[c->mon->pertag->curtag];

	if (layout->id != DWINDLE)
		return 0;

	DwindleNode **root = &selmon->pertag->dwindle_root[selmon->pertag->curtag];
	DwindleNode *leaf = dwindle_find_leaf(*root, c);

	if (!leaf)
		return 0;

	if (istoggle) {
		leaf->custom_leaf_split_h = !leaf->custom_leaf_split_h;
	} else if (horizontal) {
		leaf->custom_leaf_split_h = true;
	} else {
		leaf->custom_leaf_split_h = false;
	}
	bool hit_no_border = check_hit_no_border(c);
	apply_split_border(c, hit_no_border);
	return 0;
}

int32_t dwindle_toggle_split_direction(const Arg *arg) {
	if (!selmon)
		return 0;

	Client *c = arg->tc ? arg->tc : selmon->sel;
	if (!c || !c->mon || c->isfloating)
		return 0;
	return dwindle_set_split_direction(c, true, false);
}

int32_t dwindle_split_horizontal(const Arg *arg) {
	if (!selmon)
		return 0;

	Client *c = arg->tc ? arg->tc : selmon->sel;
	if (!c || !c->mon || c->isfloating)
		return 0;
	return dwindle_set_split_direction(c, false, true);
}

int32_t dwindle_split_vertical(const Arg *arg) {
	if (!selmon)
		return 0;

	Client *c = arg->tc ? arg->tc : selmon->sel;
	if (!c || !c->mon || c->isfloating)
		return 0;
	return dwindle_set_split_direction(c, false, false);
}

int32_t focusid(const Arg *arg) {
	if (!selmon || !arg->tc)
		return 0;

	Client *c = arg->tc;
	client_active(c);
	return 0;
}

/* ------------------------------------------------------------------------ *
 * screenshot_ui: compositor-native screenshot UI.
 *
 * Flow: the dispatcher only arms a capture request (want_capture); rendermon()
 * fulfills it the next time it actually renders that monitor, handing us a
 * locked reference to that frame's fully-rendered buffer before it is handed
 * to the output (see the screenshot_ui.want_capture branch in rendermon()).
 * That buffer becomes a full-screen scene_buffer in a dedicated top-most
 * scene layer (LyrScreenshot), which is how the desktop appears "paused"
 * while everything keeps rendering underneath, unseen. Region/window
 * selection is drawn with plain scene rects (dim mask + border) driven from
 * buttonpress()/motionnotify()/keypress() while shotui.active is set; on
 * confirm the selected area is cropped out of the frozen buffer via
 * wlr_texture_read_pixels(), handed to cairo for PNG encoding, saved under
 * ~/Pictures/Screenshots, and piped into wl-copy for the clipboard.
 * ------------------------------------------------------------------------ */

static double screenshot_ui_clampd(double v, double lo, double hi) {
	return v < lo ? lo : (v > hi ? hi : v);
}

/* Keep the pointer on the monitor the capture froze.
 *
 * The overlay is one output's worth of frozen pixels and the selection is
 * clamped to that output, so a cursor free to walk onto the next screen leaves
 * the crosshair somewhere the drag cannot follow: the rectangle stops dead at
 * the shared edge while the pointer keeps travelling, and the pixels it is
 * over belong to a live desktop that is not part of this shot. Confining is
 * also what makes the far edge reachable at all -- overshooting is how you
 * select the last column, and on a multi-monitor layout overshooting used to
 * mean landing on the neighbour.
 *
 * A pointer constraint is the protocol-level way to do this, but constraints
 * belong to a client surface and this overlay has none, so it is a warp
 * applied after every move. */
static void screenshot_ui_confine_cursor(void) {
	Monitor *m = shotui.mon;
	if (!shotui.active || !m)
		return;

	/* The far edges are exclusive: at exactly m.x + m.width the pointer is
	 * already on the next output. A hair inside still rounds up to the full
	 * width in screenshot_ui_update_selection(), so the last pixel column and
	 * row stay selectable. */
	const double edge = 0.01;
	double x = screenshot_ui_clampd(cursor->x, m->m.x,
									m->m.x + m->m.width - edge);
	double y = screenshot_ui_clampd(cursor->y, m->m.y,
									m->m.y + m->m.height - edge);

	if (x != cursor->x || y != cursor->y)
		wlr_cursor_warp_closest(cursor, NULL, x, y);
}

static void screenshot_ui_reset_cursor(void) {
	az_cursor_set_xcursor("default");
	wlr_seat_pointer_clear_focus(seat);
	motionnotify(0, NULL, 0, 0, 0, 0);
}

static void screenshot_ui_teardown(void) {
	if (shotui.label) {
		asteroidz_jump_label_node_destroy(shotui.label);
		shotui.label = NULL;
	}
	if (shotui.tree) {
		wlr_scene_node_destroy(&shotui.tree->node);
		shotui.tree = NULL;
	}
	shotui.frame_node = NULL;
	for (int32_t i = 0; i < 4; i++) {
		shotui.dim[i] = NULL;
		shotui.border[i] = NULL;
	}
	if (shotui.frame) {
		wlr_buffer_unlock(shotui.frame);
		shotui.frame = NULL;
	}
	free(shotui.snap);
	shotui.snap = NULL;
	shotui.snap_len = 0;
	shotui.mon = NULL;
	shotui.active = false;
	shotui.dragging = false;

	screenshot_ui_reset_cursor();
}

static void screenshot_ui_cancel(void) {
	if (!shotui.active) {
		/* also cancel an armed-but-not-yet-fulfilled capture request */
		shotui.want_capture = false;
		shotui.capture_mon = NULL;
		return;
	}
	screenshot_ui_teardown();
}

/* position/size the dim mask (outside the selection) and border (framing
 * it) from shotui.sel; both are expressed in the overlay tree's local
 * coordinates, i.e. monitor-relative */
static void screenshot_ui_layout_dim_and_border(void) {
	Monitor *m = shotui.mon;
	if (!m || !shotui.tree)
		return;

	int32_t sx = shotui.sel.x - m->m.x;
	int32_t sy = shotui.sel.y - m->m.y;
	int32_t sw = shotui.sel.width;
	int32_t sh = shotui.sel.height;
	int32_t mw = m->m.width;
	int32_t mh = m->m.height;

	wlr_scene_node_set_position(&shotui.dim[0]->node, 0, 0);
	wlr_scene_rect_set_size(shotui.dim[0], mw, sy);

	wlr_scene_node_set_position(&shotui.dim[1]->node, 0, sy + sh);
	wlr_scene_rect_set_size(shotui.dim[1], mw, mh - (sy + sh));

	wlr_scene_node_set_position(&shotui.dim[2]->node, 0, sy);
	wlr_scene_rect_set_size(shotui.dim[2], sx, sh);

	wlr_scene_node_set_position(&shotui.dim[3]->node, sx + sw, sy);
	wlr_scene_rect_set_size(shotui.dim[3], mw - (sx + sw), sh);

	const int32_t bw =
		config.theme.border_width > 0 ? config.theme.border_width : 2;
	bool have_sel = sw > 0 && sh > 0;

	wlr_scene_node_set_position(&shotui.border[0]->node, sx, sy);
	wlr_scene_rect_set_size(shotui.border[0], sw, bw);

	wlr_scene_node_set_position(&shotui.border[1]->node, sx, sy + sh - bw);
	wlr_scene_rect_set_size(shotui.border[1], sw, bw);

	wlr_scene_node_set_position(&shotui.border[2]->node, sx, sy);
	wlr_scene_rect_set_size(shotui.border[2], bw, sh);

	wlr_scene_node_set_position(&shotui.border[3]->node, sx + sw - bw, sy);
	wlr_scene_rect_set_size(shotui.border[3], bw, sh);

	for (int32_t i = 0; i < 4; i++)
		wlr_scene_node_set_enabled(&shotui.border[i]->node, have_sel);
}

static void screenshot_ui_update_label(void) {
	Monitor *m = shotui.mon;
	if (!shotui.label || !m)
		return;

	char text[32];
	snprintf(text, sizeof(text), "%d x %d", shotui.sel.width,
			shotui.sel.height);
	asteroidz_jump_label_node_update(shotui.label, text, 1.0f);

	int32_t lx = shotui.sel.x - m->m.x;
	int32_t ly = shotui.sel.y - m->m.y - shotui.label->logical_height - 8;
	if (ly < 0)
		ly = shotui.sel.y - m->m.y + shotui.sel.height + 8;
	wlr_scene_node_set_position(&shotui.label->scene_buffer->node, lx, ly);

	bool have_sel = shotui.sel.width > 0 && shotui.sel.height > 0;
	wlr_scene_node_set_enabled(&shotui.label->scene_buffer->node, have_sel);
}

static void screenshot_ui_update_selection(double cx, double cy) {
	Monitor *m = shotui.mon;
	if (!m)
		return;

	double lo_x = m->m.x, hi_x = m->m.x + m->m.width;
	double lo_y = m->m.y, hi_y = m->m.y + m->m.height;

	double ax = screenshot_ui_clampd(shotui.start_x, lo_x, hi_x);
	double ay = screenshot_ui_clampd(shotui.start_y, lo_y, hi_y);
	double bx = screenshot_ui_clampd(cx, lo_x, hi_x);
	double by = screenshot_ui_clampd(cy, lo_y, hi_y);

	int32_t left = (int32_t)round(fmin(ax, bx));
	int32_t top = (int32_t)round(fmin(ay, by));
	int32_t right = (int32_t)round(fmax(ax, bx));
	int32_t bottom = (int32_t)round(fmax(ay, by));

	shotui.sel = (struct wlr_box){
		.x = left, .y = top, .width = right - left, .height = bottom - top};

	screenshot_ui_layout_dim_and_border();
	screenshot_ui_update_label();
}

/* Windows the frozen frame actually shows on this monitor -- the set window
 * mode may pick from.
 *
 * In the overview that is not "visible on the active tag": the big area
 * mirrors whichever tag is being PREVIEWED, so a tag test excludes exactly the
 * windows on screen and includes ones that are not. What is on the overview
 * desktop is what overview_arrange_main left enabled, and it resized each of
 * those to its tile with client_tile_resize(), so the geometry the snapshot
 * reads is already the drawn rectangle. */
static bool screenshot_ui_snapable(Client *sc, Monitor *m) {
	if (client_is_unmanaged(sc) || sc->isminimized || sc->iskilling)
		return false;
	if (m->isoverview)
		return sc->mon == m && !sc->is_overview_hidden && sc->scene &&
			   sc->scene->node.enabled;
	return VISIBLEON(sc, m);
}

/* hit-test against the freeze-time snapshot (topmost/focus order), never
 * the live scene -- see the snap comment in ScreenshotUI */
static bool screenshot_ui_snap_box_at(double x, double y,
									  struct wlr_box *out) {
	for (int32_t i = 0; i < shotui.snap_len; i++) {
		const struct wlr_box *b = &shotui.snap[i].box;
		if (x >= b->x && y >= b->y && x < b->x + b->width &&
				y < b->y + b->height) {
			*out = *b;
			return true;
		}
	}
	return false;
}

static void screenshot_ui_hover_window(double cx, double cy) {
	Monitor *m = shotui.mon;
	if (!m)
		return;

	struct wlr_box box;
	if (!screenshot_ui_snap_box_at(cx, cy, &box)) {
		box = (struct wlr_box){.x = (int32_t)round(cx), .y = (int32_t)round(cy),
								.width = 0, .height = 0};
	}

	int32_t x2 = box.x + box.width, y2 = box.y + box.height;
	if (box.x < m->m.x)
		box.x = m->m.x;
	if (box.y < m->m.y)
		box.y = m->m.y;
	if (x2 > m->m.x + m->m.width)
		x2 = m->m.x + m->m.width;
	if (y2 > m->m.y + m->m.height)
		y2 = m->m.y + m->m.height;
	box.width = x2 > box.x ? x2 - box.x : 0;
	box.height = y2 > box.y ? y2 - box.y : 0;

	shotui.sel = box;
	screenshot_ui_layout_dim_and_border();
	screenshot_ui_update_label();
}

/* ~/Pictures/Screenshots/screenshot_<timestamp>.png, matching the naming
 * used by the DMS shell's own screenshot tool */
static char *screenshot_ui_build_path(void) {
	const char *home = getenv("HOME");
	if (!home || !*home)
		home = "/tmp";

	char *dir = string_printf("%s/Pictures/Screenshots", home);
	if (!dir)
		return NULL;

	for (char *p = dir + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			mkdir(dir, 0755);
			*p = '/';
		}
	}
	mkdir(dir, 0755);

	time_t now = time(NULL);
	struct tm tm_now;
	localtime_r(&now, &tm_now);
	char stamp[32];
	strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H-%M-%S", &tm_now);

	char *path = string_printf("%s/screenshot_%s.png", dir, stamp);
	free(dir);
	return path;
}

static void screenshot_ui_copy_to_clipboard(const char *path) {
	char *cmd = string_printf("wl-copy --type image/png < '%s'", path);
	if (!cmd)
		return;
	spawn_shell(&(Arg){.v = cmd});
	free(cmd);
}

/* Desktop notification for a finished capture: kind, pixel size, file size
 * and path, with the shot itself as the icon. Sent straight over the
 * session bus (see ipc/session-bus.h); no daemon simply means an error
 * reply -- the capture already succeeded, so nothing surfaces here. */
static void screenshot_ui_notify(ScreenshotMode mode, int32_t px_w,
								 int32_t px_h, const char *path) {
	static const char *kinds[] = {
		[ShotScreen] = "screen", [ShotRegion] = "region",
		[ShotWindow] = "window"};

	struct stat st;
	char fsize[32] = "";
	if (stat(path, &st) == 0) {
		if (st.st_size >= 1024 * 1024)
			snprintf(fsize, sizeof(fsize), " · %.1f MiB",
					 st.st_size / (1024.0 * 1024.0));
		else
			snprintf(fsize, sizeof(fsize), " · %.0f KiB", st.st_size / 1024.0);
	}

	char *body = string_printf("%s · %dx%d%s\n%s", kinds[mode], px_w, px_h,
							   fsize, path);
	if (!body)
		return;
	notify_send("Screenshot saved", body, path);
	free(body);
}

/* SMPTE ST 2084 (PQ) EOTF: normalized code value -> linear light, in units
 * where 1.0 == 10000 nits. */
static float screenshot_pq_eotf(float v) {
	const float m1 = 0.1593017578125f;
	const float m2 = 78.84375f;
	const float c1 = 0.8359375f;
	const float c2 = 18.8515625f;
	const float c3 = 18.6875f;
	if (v <= 0.0f)
		return 0.0f;
	float vp = powf(v, 1.0f / m2);
	float num = fmaxf(vp - c1, 0.0f);
	float den = c2 - c3 * vp;
	if (den <= 0.0f)
		return 0.0f;
	return powf(num / den, 1.0f / m1);
}

static float screenshot_srgb_oetf(float l) {
	l = fmaxf(0.0f, fminf(1.0f, l));
	if (l <= 0.0031308f)
		return 12.92f * l;
	return 1.055f * powf(l, 1.0f / 2.4f) - 0.055f;
}

/* screenshot_ui reads back the raw composited buffer directly (not through
 * wlr-screencopy/ext-image-copy-capture). asteroidz used to answer the
 * washed-out-capture problem by dropping the whole output to SDR for the
 * duration of a capture; that is gone, because it flashed the real display and
 * could take 1-1.5s on a retrain fallback -- a bad trade for a freeze-frame,
 * and a worse one for a recording. This is the answer instead, and it is the
 * better one: it costs nothing on screen. Since PQ-encoded samples carry no
 * colorimetry metadata, writing them straight to a PNG makes it look flat
 * and washed out (the same caveat documented for external capture tools).
 * Decode PQ -> linear, normalize against the configured SDR reference
 * white, and re-encode as sRGB instead, entirely in software, so the
 * screenshot looks correct without ever touching the monitor's live HDR
 * state. */
static void screenshot_tonemap_pq_to_srgb(uint8_t *pixels, int32_t width,
										  int32_t height, int32_t stride) {
	float reference_nits = config.sdr_reference_luminance > 0.0f
								? config.sdr_reference_luminance
								: 203.0f;

	for (int32_t y = 0; y < height; y++) {
		uint32_t *row = (uint32_t *)(pixels + (size_t)y * stride);
		for (int32_t x = 0; x < width; x++) {
			uint32_t px = row[x];
			/* A "screen"/direct-scanout-eligible buffer's alpha bits are
			 * often meaningless padding (the DRM format's "X" prefix, e.g.
			 * XBGR2101010): the producer never writes them as opaque
			 * because nothing is supposed to read them. Treat a==0 here as
			 * fully opaque rather than skip -- skipping left the pixel's
			 * ORIGINAL (still-PQ-encoded, un-tonemapped) alpha=0 in the
			 * output, so the whole capture came back fully transparent,
			 * rendering as a blank image in any viewer despite having real
			 * (if un-tonemapped) colour underneath. Genuinely
			 * semi-transparent content (window opacity rules) never stores
			 * literal 0, so this doesn't touch it. */
			uint8_t a = (px >> 24) & 0xFF;
			if (a == 0)
				a = 0xFF;
			uint8_t r = (px >> 16) & 0xFF;
			uint8_t g = (px >> 8) & 0xFF;
			uint8_t b = px & 0xFF;

			float fa = a / 255.0f;
			float fr = (r / 255.0f) / fa;
			float fg = (g / 255.0f) / fa;
			float fb = (b / 255.0f) / fa;

			fr = screenshot_srgb_oetf(
				screenshot_pq_eotf(fr) * 10000.0f / reference_nits);
			fg = screenshot_srgb_oetf(
				screenshot_pq_eotf(fg) * 10000.0f / reference_nits);
			fb = screenshot_srgb_oetf(
				screenshot_pq_eotf(fb) * 10000.0f / reference_nits);

			r = (uint8_t)(CLAMP_FLOAT(fr * fa, 0.0f, 1.0f) * 255.0f + 0.5f);
			g = (uint8_t)(CLAMP_FLOAT(fg * fa, 0.0f, 1.0f) * 255.0f + 0.5f);
			b = (uint8_t)(CLAMP_FLOAT(fb * fa, 0.0f, 1.0f) * 255.0f + 0.5f);

			row[x] = ((uint32_t)a << 24) | ((uint32_t)r << 16) |
					 ((uint32_t)g << 8) | (uint32_t)b;
		}
	}
}

/* crop [sel] (layout coords) out of [frame] and save+copy it as a PNG */
static bool screenshot_ui_save_and_copy(struct wlr_buffer *frame, Monitor *m,
										struct wlr_box sel,
										ScreenshotMode mode) {
	if (!frame || !m || sel.width <= 0 || sel.height <= 0)
		return false;

	double sx = m->m.width > 0 ? (double)frame->width / m->m.width : 1.0;
	double sy = m->m.height > 0 ? (double)frame->height / m->m.height : 1.0;

	int32_t rel_x = sel.x - m->m.x;
	int32_t rel_y = sel.y - m->m.y;
	if (rel_x < 0)
		rel_x = 0;
	if (rel_y < 0)
		rel_y = 0;
	int32_t rel_x2 = rel_x + sel.width;
	int32_t rel_y2 = rel_y + sel.height;
	if (rel_x2 > m->m.width)
		rel_x2 = m->m.width;
	if (rel_y2 > m->m.height)
		rel_y2 = m->m.height;
	if (rel_x2 <= rel_x || rel_y2 <= rel_y)
		return false;

	int32_t px_x = (int32_t)round(rel_x * sx);
	int32_t px_y = (int32_t)round(rel_y * sy);
	int32_t px_w = (int32_t)round((rel_x2 - rel_x) * sx);
	int32_t px_h = (int32_t)round((rel_y2 - rel_y) * sy);
	if (px_x < 0)
		px_x = 0;
	if (px_y < 0)
		px_y = 0;
	if (px_x + px_w > frame->width)
		px_w = frame->width - px_x;
	if (px_y + px_h > frame->height)
		px_h = frame->height - px_y;
	if (px_w <= 0 || px_h <= 0)
		return false;

	struct wlr_texture *tex = wlr_texture_from_buffer(drw, frame);
	if (!tex) {
		wlr_log(WLR_ERROR, "screenshot_ui: failed to import frame as texture");
		return false;
	}

	int32_t stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, px_w);
	uint8_t *pixels = stride > 0 ? calloc((size_t)stride, (size_t)px_h) : NULL;
	if (!pixels) {
		wlr_texture_destroy(tex);
		return false;
	}

	struct wlr_texture_read_pixels_options opts = {
		.data = pixels,
		.format = DRM_FORMAT_ARGB8888,
		.stride = (uint32_t)stride,
		.dst_x = 0,
		.dst_y = 0,
		.src_box = (struct wlr_box){
			.x = px_x, .y = px_y, .width = px_w, .height = px_h},
	};
	bool read_ok = wlr_texture_read_pixels(tex, &opts);
	wlr_texture_destroy(tex);

	if (!read_ok) {
		wlr_log(WLR_ERROR, "screenshot_ui: failed to read back pixels");
		free(pixels);
		return false;
	}

	if (m->hdr)
		screenshot_tonemap_pq_to_srgb(pixels, px_w, px_h, stride);

	/* A screenshot captures the already-composited, already-opaque display
	 * appearance -- there is no such thing as a transparent pixel on an
	 * actual monitor. But an opaque/scanout-eligible source buffer's alpha
	 * bits are often meaningless padding (a DRM "X"-prefixed format like
	 * XBGR2101010) that the producer never writes as opaque, since nothing
	 * is supposed to read them. If that padding happens to be 0, the whole
	 * PNG comes back fully transparent -- renders as blank in any viewer,
	 * even though the RGB channels hold real data. Force full opacity
	 * unconditionally rather than only on the HDR path above, since the
	 * same all-zero-alpha buffer can reach this function via any mode
	 * (screen/region/window) and with HDR off. */
	for (int32_t y = 0; y < px_h; y++) {
		uint32_t *row = (uint32_t *)(pixels + (size_t)y * stride);
		for (int32_t x = 0; x < px_w; x++)
			row[x] |= 0xFF000000;
	}

	cairo_surface_t *surf = cairo_image_surface_create_for_data(
		pixels, CAIRO_FORMAT_ARGB32, px_w, px_h, stride);
	bool ok = surf && cairo_surface_status(surf) == CAIRO_STATUS_SUCCESS;

	char *path = ok ? screenshot_ui_build_path() : NULL;
	if (ok && path) {
		ok = cairo_surface_write_to_png(surf, path) == CAIRO_STATUS_SUCCESS;
		if (ok) {
			wlr_log(WLR_INFO, "screenshot_ui: saved %s", path);
			screenshot_ui_copy_to_clipboard(path);
			screenshot_ui_notify(mode, px_w, px_h, path);
		} else {
			wlr_log(WLR_ERROR, "screenshot_ui: failed to write %s", path);
		}
	} else {
		ok = false;
	}

	if (surf)
		cairo_surface_destroy(surf);
	free(pixels);
	free(path);
	return ok;
}

/* raw-frame output dir + monotonic counter for ShotRawHDR captures -- a
 * fixed, simple location an external tool (contrib/hdr-record.sh) owns: it
 * clears the directory before starting a capture session and assembles the
 * numbered frames into an HDR10-tagged video afterward via ffmpeg. */
#define RAWHDR_DIR "/tmp/asteroidz-rawhdr"
static uint32_t rawhdr_frame_seq = 0;

/* Dumps [frame] (the full composited output buffer, no crop/selection) at
 * its native 10-bit precision, unlike screenshot_ui_save_and_copy's usual
 * tonemapped 8-bit PNG path -- for external HDR10 video encoding.
 *
 * GLES2 can't read back DRM_FORMAT_XRGB2101010, the format asteroidz
 * actually renders HDR outputs in (wlr_output_state_set_render_format);
 * only XBGR2101010 (R/B-swapped, same 10-bit precision) is in wlroots'
 * GLES2 readback format table (render/gles2/pixel_format.c) -- Vulkan has
 * no such gap. Read back XBGR2101010 here; the consuming tool must tag its
 * ffmpeg pixel format as x2bgr10le (not x2rgb10le) to match, or the R/B
 * channels come out swapped in the encoded video. Requires
 * GL_EXT_texture_type_2_10_10_10_REV (near-universal on Mesa/RadeonSI). */
static bool screenshot_ui_save_raw_hdr(struct wlr_buffer *frame) {
	if (!frame)
		return false;

	struct wlr_texture *tex = wlr_texture_from_buffer(drw, frame);
	if (!tex) {
		wlr_log(WLR_ERROR, "screenshot_ui: rawhdr: failed to import frame as texture");
		return false;
	}

	int32_t w = frame->width, h = frame->height;
	int32_t stride = w * 4; /* XBGR2101010 is 4 bytes/px, no extra padding */
	uint8_t *pixels = calloc((size_t)stride, (size_t)h);
	if (!pixels) {
		wlr_texture_destroy(tex);
		return false;
	}

	struct wlr_texture_read_pixels_options opts = {
		.data = pixels,
		.format = DRM_FORMAT_XBGR2101010,
		.stride = (uint32_t)stride,
		.dst_x = 0,
		.dst_y = 0,
		.src_box = (struct wlr_box){.x = 0, .y = 0, .width = w, .height = h},
	};
	bool read_ok = wlr_texture_read_pixels(tex, &opts);
	wlr_texture_destroy(tex);

	if (!read_ok) {
		wlr_log(WLR_ERROR, "screenshot_ui: rawhdr: failed to read back pixels "
						   "(is GL_EXT_texture_type_2_10_10_10_REV missing?)");
		free(pixels);
		return false;
	}

	mkdir(RAWHDR_DIR, 0755);
	char *path = string_printf("%s/frame_%06u.raw", RAWHDR_DIR, rawhdr_frame_seq);
	bool ok = false;
	if (path) {
		FILE *f = fopen(path, "wb");
		if (f) {
			ok = fwrite(pixels, (size_t)stride * (size_t)h, 1, f) == 1;
			fclose(f);
			if (ok) {
				wlr_log(WLR_DEBUG, "screenshot_ui: rawhdr: wrote %s (%dx%d)",
					path, w, h);
				rawhdr_frame_seq++;
			}
		}
		if (!ok)
			wlr_log(WLR_ERROR, "screenshot_ui: rawhdr: failed to write %s", path);
		free(path);
	}
	free(pixels);
	return ok;
}

static void screenshot_ui_confirm(void) {
	screenshot_ui_save_and_copy(shotui.frame, shotui.mon, shotui.sel,
								shotui.mode);
	screenshot_ui_teardown();
}

/* called from rendermon() once the requested capture has actually landed;
 * frame is already locked for our use (see rendermon()) */
static void screenshot_ui_on_captured(Monitor *m, ScreenshotMode mode,
									  struct wlr_buffer *frame) {
	if (!frame)
		return;

	wlr_log(WLR_DEBUG, "screenshot_ui: captured frame on %s (mode %d)",
		m->wlr_output->name, (int)mode);

	if (mode == ShotScreen) {
		screenshot_ui_save_and_copy(frame, m, m->m, ShotScreen);
		wlr_buffer_unlock(frame);
		return;
	}

	if (mode == ShotRawHDR) {
		screenshot_ui_save_raw_hdr(frame);
		wlr_buffer_unlock(frame);
		return;
	}

	shotui.mode = mode;
	shotui.mon = m;
	shotui.frame = frame;
	shotui.dragging = false;

	shotui.tree = wlr_scene_tree_create(layers[LyrScreenshot]);
	wlr_scene_node_set_position(&shotui.tree->node, m->m.x, m->m.y);

	/* Freeze the window geometry alongside the pixels: hit-test boxes are
	 * animation.current AS OF THIS FRAME (what the frozen buffer actually
	 * shows), in focus order so overlapping windows resolve topmost-first.
	 * The live scene keeps animating underneath and must not be consulted. */
	shotui.snap = NULL;
	shotui.snap_len = 0;
	if (mode == ShotWindow) {
		Client *sc;
		int32_t count = 0;
		wl_list_for_each(sc, &fstack, flink) {
			if (screenshot_ui_snapable(sc, m))
				count++;
		}
		if (count > 0)
			shotui.snap = ecalloc(count, sizeof(*shotui.snap));
		if (shotui.snap) {
			wl_list_for_each(sc, &fstack, flink) {
				if (!screenshot_ui_snapable(sc, m))
					continue;
				shotui.snap[shotui.snap_len].c = sc;
				shotui.snap[shotui.snap_len].box = (struct wlr_box){
					.x = (int32_t)round(sc->animation.current.x),
					.y = (int32_t)round(sc->animation.current.y),
					.width = (int32_t)round(sc->animation.current.width),
					.height = (int32_t)round(sc->animation.current.height),
				};
				if (++shotui.snap_len >= count)
					break;
			}
		}
	}

	shotui.frame_node = wlr_scene_buffer_create(shotui.tree, frame);
	wlr_scene_buffer_set_dest_size(shotui.frame_node, m->m.width, m->m.height);

	/* border and label both pull from config.theme so the overlay matches
	 * the native UI theme. The selection border uses the FOCUS accent
	 * (focus_bg_color = the matugen primary, same accent as focused window
	 * borders): theme.border_color is a legacy default the generated
	 * colors.kdl never sets (theme border-width is 0), so it doesn't follow
	 * the colour style. */
	static const float dim_color[4] = {0.0f, 0.0f, 0.0f, 0.55f};
	for (int32_t i = 0; i < 4; i++) {
		shotui.dim[i] = wlr_scene_rect_create(shotui.tree, 0, 0, dim_color);
		shotui.border[i] = wlr_scene_rect_create(shotui.tree, 0, 0,
												 config.theme.focus_bg_color);
	}

	shotui.label = asteroidz_jump_label_node_create(shotui.tree, config.theme);
	if (shotui.label) {
		asteroidz_jump_label_node_set_focus(shotui.label, true);
		wlr_scene_node_set_enabled(&shotui.label->scene_buffer->node, false);
	}

	shotui.active = true;

	/* Bring the pointer onto the captured output before anything reads it.
	 * Without sloppyfocus the cursor and selmon can be on different monitors
	 * altogether, and the crosshair would open on a screen the overlay is not
	 * even drawn on; from here on it cannot leave. */
	screenshot_ui_confine_cursor();

	shotui.start_x = cursor->x;
	shotui.start_y = cursor->y;

	if (mode == ShotWindow)
		screenshot_ui_hover_window(cursor->x, cursor->y);
	else
		screenshot_ui_update_selection(cursor->x, cursor->y);

	az_cursor_set_xcursor("crosshair");
}

/* returns true if the event was consumed by the screenshot UI */
bool screenshot_ui_handle_button(struct wlr_pointer_button_event *event) {
	if (!shotui.active)
		return false;

	if (shotui.mode == ShotRegion) {
		if (event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
			if (event->button != BTN_LEFT) {
				screenshot_ui_cancel();
				return true;
			}
			shotui.dragging = true;
			shotui.start_x = cursor->x;
			shotui.start_y = cursor->y;
			screenshot_ui_update_selection(cursor->x, cursor->y);
		} else if (event->state == WL_POINTER_BUTTON_STATE_RELEASED &&
				  shotui.dragging) {
			shotui.dragging = false;
			screenshot_ui_confirm();
		}
	} else if (shotui.mode == ShotWindow) {
		if (event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
			if (event->button != BTN_LEFT) {
				screenshot_ui_cancel();
				return true;
			}
			screenshot_ui_hover_window(cursor->x, cursor->y);
			if (shotui.sel.width > 0 && shotui.sel.height > 0)
				screenshot_ui_confirm();
			else
				screenshot_ui_cancel();
		}
	}
	return true;
}

/* called on every pointer move while the UI is active */
void screenshot_ui_handle_motion(void) {
	if (!shotui.active)
		return;
	if (shotui.mode == ShotRegion) {
		if (shotui.dragging)
			screenshot_ui_update_selection(cursor->x, cursor->y);
	} else if (shotui.mode == ShotWindow) {
		screenshot_ui_hover_window(cursor->x, cursor->y);
	}
}

/* called for every keysym on key press while the UI is active */
void screenshot_ui_handle_key(xkb_keysym_t sym) {
	if (sym == XKB_KEY_Escape)
		screenshot_ui_cancel();
}

int32_t screenshot_ui(const Arg *arg) {
	if (locked || shotui.active || shotui.want_capture)
		return 0;

	Monitor *m = selmon;
	if (!m || !m->wlr_output || !m->wlr_output->enabled || !m->scene_output)
		return 0;

	ScreenshotMode mode = ShotRegion;
	if (arg && arg->v && *arg->v) {
		if (!strcmp(arg->v, "screen"))
			mode = ShotScreen;
		else if (!strcmp(arg->v, "window"))
			mode = ShotWindow;
		else if (!strcmp(arg->v, "rawhdr"))
			mode = ShotRawHDR;
	}

	shotui.capture_mode = mode;
	shotui.capture_mon = m;
	shotui.want_capture = true;
	wlr_output_schedule_frame(m->wlr_output);
	return 0;
}
int32_t ufo_easter_egg(const Arg *arg) {
	(void)arg;
	ufo_egg_trigger(ufo_egg);
	return 0;
}

/* Write the next few frames' backdrop-blur SOURCE images to disk.
 *
 * A diagnostic, for the one question that cannot be answered from a
 * screenshot: what a window's shadow is about to blur, before it blurs it. The
 * source is a scene replay that never reaches the screen, and the blur averages
 * away whatever was wrong with it, so the artefact and its cause never appear
 * in the same picture.
 *
 * Arg: "<prefix>" or "<prefix>,<frames>"; no argument disarms. Frames are
 * clamped to 1..60 and default to three. Each armed frame waits for its own
 * submission before reading it back, so this stutters by design -- and stops on
 * its own, because a forgotten arming would otherwise be a permanent one.
 *
 * WHAT IS WRITTEN, and where the pictures come from, is AVK's:
 * render/vulkan/scene/avk_blur_dump.h. One `live<k>.pam` per blur node plus a
 * `cache-plain`/`cache-dark` pair on any frame that rebuilt the monitor
 * background cache, each with a `.txt` sidecar naming its boxes in output
 * coordinates.
 *
 * Writes wherever it is pointed, with this compositor's privileges: it is
 * reachable over the same IPC socket as every other dispatch, which is already
 * the boundary that decides who may drive this session.
 */
int32_t dump_blur_source(const Arg *arg) {
	const char *spec = arg != NULL ? arg->v : NULL;
	if (spec == NULL || *spec == '\0') {
		avk_blur_dump_arm(NULL, 0);
		return 0;
	}
	char prefix[512];
	int frames = 3;
	const char *comma = strrchr(spec, ',');
	if (comma != NULL && comma[1] != '\0' && atoi(comma + 1) > 0) {
		size_t n = (size_t)(comma - spec);
		if (n >= sizeof(prefix)) {
			n = sizeof(prefix) - 1;
		}
		memcpy(prefix, spec, n);
		prefix[n] = '\0';
		frames = CLAMP_INT(atoi(comma + 1), 1, 60);
	} else {
		snprintf(prefix, sizeof(prefix), "%s", spec);
	}
	avk_blur_dump_arm(prefix, frames);
	/* Nothing is captured until something redraws, and a settled desktop does
	 * not. Damage every output so the next frames actually happen. */
	Monitor *m;
	wl_list_for_each(m, &mons, link) {
		if (m->wlr_output != NULL && m->wlr_output->enabled) {
			wlr_output_schedule_frame(m->wlr_output);
		}
	}
	return 0;
}
