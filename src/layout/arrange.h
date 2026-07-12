void set_size_per(Monitor *m, Client *c) {
	Client *fc = NULL;
	bool found = false;

	if (!m || !c)
		return;

	wl_list_for_each(fc, &clients, link) {
		if (VISIBLEON(fc, m) && ISTILED(fc) && fc != c) {
			c->master_mfact_per = fc->master_mfact_per;
			c->master_inner_per = fc->master_inner_per;
			c->stack_inner_per = fc->stack_inner_per;
			found = true;
			break;
		}
	}

	if (!found || c->isfloating) {
		c->master_mfact_per = m->pertag->mfacts[m->pertag->curtag];
		c->master_inner_per = 1.0f;
		c->stack_inner_per = 1.0f;
	}

	if (!c->iscustom_scroller_proportion) {
		c->scroller_proportion =
			m->pertag->scroller_default_proportion[m->pertag->curtag];
	}

	if (!c->iscustom_scroller_proportion_single) {
		c->scroller_proportion_single =
			m->pertag->scroller_default_proportion_single[m->pertag->curtag];
	}
}

void monocle_set_focus(Client *c, bool focused) {

	if (!c || !c->mon)
		return;

	c->is_monocle_hide = !focused;
	wlr_scene_node_set_enabled(&c->scene->node, focused);

	if (!focused) {
		c->animation.current = c->animainit_geom = c->animation.initial =
			c->pending = c->current = c->geom;
	}
}

void resize_tile_dwindle(Client *grabc, bool isdrag, int32_t offsetx,
						 int32_t offsety, uint32_t time, bool isvertical) {

	if (!isdrag) {
		dwindle_resize_client_step(grabc->mon, grabc, offsetx, offsety);
		return;
	}

	if (last_apply_drap_time == 0 ||
		time - last_apply_drap_time > config.drag_tile_refresh_interval) {
		dwindle_resize_client(grabc->mon, grabc);
		last_apply_drap_time = time;
	}
}

void resize_tile_scroller(Client *grabc, bool isdrag, int32_t offsetx,
						  int32_t offsety, uint32_t time, bool isvertical) {

	if (!grabc || grabc->isfullscreen || grabc->ismaximizescreen)
		return;
	if (grabc->mon->isoverview)
		return;

	Monitor *m = grabc->mon;
	uint32_t tag = m->pertag->curtag;
	struct TagScrollerState *st = m->pertag->scroller_state[tag];
	if (!st)
		return;

	struct ScrollerStackNode *curnode = find_scroller_node(st, grabc);
	if (!curnode)
		return;

	struct ScrollerStackNode *headnode = curnode;
	while (headnode->prev_in_stack)
		headnode = headnode->prev_in_stack;

	Client *stack_head_client = headnode->client;

	if (m->visible_scroll_tiling_clients == 1 &&
		!config.scroller_ignore_proportion_single)
		return;

	float delta_x, delta_y;
	float new_scroller_proportion;
	float new_stack_proportion;

	if (!start_drag_window && isdrag) {
		drag_begin_cursorx = cursor->x;
		drag_begin_cursory = cursor->y;
		start_drag_window = true;

		headnode->client->old_scroller_pproportion =
			headnode->scroller_proportion;
		grabc->old_stack_proportion = curnode->stack_proportion;

		grabc->cursor_in_left_half =
			cursor->x < grabc->geom.x + grabc->geom.width / 2;
		grabc->cursor_in_upper_half =
			cursor->y < grabc->geom.y + grabc->geom.height / 2;
		grabc->drag_begin_geom = grabc->geom;
	} else {
		if (isdrag) {
			offsetx = cursor->x - drag_begin_cursorx;
			offsety = cursor->y - drag_begin_cursory;
		} else {
			grabc->old_master_mfact_per = grabc->master_mfact_per;
			grabc->old_master_inner_per = grabc->master_inner_per;
			grabc->old_stack_inner_per = grabc->stack_inner_per;
			grabc->drag_begin_geom = grabc->geom;
			stack_head_client->old_scroller_pproportion =
				headnode->scroller_proportion;
			grabc->old_stack_proportion = curnode->stack_proportion;
			grabc->cursor_in_upper_half = false;
			grabc->cursor_in_left_half = false;
		}

		if (isvertical) {
			delta_y = (float)(offsety) *
					  (headnode->client->old_scroller_pproportion) /
					  grabc->drag_begin_geom.height;
			delta_x = (float)(offsetx) * (grabc->old_stack_proportion) /
					  grabc->drag_begin_geom.width;
		} else {
			delta_x = (float)(offsetx) *
					  (headnode->client->old_scroller_pproportion) /
					  grabc->drag_begin_geom.width;
			delta_y = (float)(offsety) * (grabc->old_stack_proportion) /
					  grabc->drag_begin_geom.height;
		}

		bool moving_up, moving_down, moving_left, moving_right;
		if (!isdrag) {
			moving_up = offsety < 0;
			moving_down = offsety > 0;
			moving_left = offsetx < 0;
			moving_right = offsetx > 0;
		} else {
			moving_up = cursor->y < drag_begin_cursory;
			moving_down = cursor->y > drag_begin_cursory;
			moving_left = cursor->x < drag_begin_cursorx;
			moving_right = cursor->x > drag_begin_cursorx;
		}

		if ((grabc->cursor_in_upper_half && moving_up) ||
			(!grabc->cursor_in_upper_half && moving_down)) {
			delta_y = fabsf(delta_y);
		} else {
			delta_y = -fabsf(delta_y);
		}

		if ((grabc->cursor_in_left_half && moving_left) ||
			(!grabc->cursor_in_left_half && moving_right)) {
			delta_x = fabsf(delta_x);
		} else {
			delta_x = -fabsf(delta_x);
		}

		if (isvertical) {
			if (!curnode->next_in_stack && curnode->prev_in_stack && !isdrag) {
				delta_x = delta_x * -1.0f;
			}
			if (!curnode->next_in_stack && curnode->prev_in_stack && isdrag) {
				if (moving_right)
					delta_x = -fabsf(delta_x);
				else
					delta_x = fabsf(delta_x);
			}
			if (!curnode->prev_in_stack && curnode->next_in_stack && isdrag) {
				if (moving_left)
					delta_x = -fabsf(delta_x);
				else
					delta_x = fabsf(delta_x);
			}
			if (isdrag) {
				if (moving_up)
					delta_y = -fabsf(delta_y);
				else
					delta_y = fabsf(delta_y);
			}
		} else {
			if (!curnode->next_in_stack && curnode->prev_in_stack && !isdrag) {
				delta_y = delta_y * -1.0f;
			}
			if (!curnode->next_in_stack && curnode->prev_in_stack && isdrag) {
				if (moving_down)
					delta_y = -fabsf(delta_y);
				else
					delta_y = fabsf(delta_y);
			}
			if (!curnode->prev_in_stack && curnode->next_in_stack && isdrag) {
				if (moving_up)
					delta_y = -fabsf(delta_y);
				else
					delta_y = fabsf(delta_y);
			}
			if (isdrag) {
				if (moving_left)
					delta_x = -fabsf(delta_x);
				else
					delta_x = fabsf(delta_x);
			}
		}

		if (isvertical) {
			new_scroller_proportion =
				headnode->client->old_scroller_pproportion + delta_y;
			new_stack_proportion = grabc->old_stack_proportion + delta_x;
		} else {
			new_scroller_proportion =
				headnode->client->old_scroller_pproportion + delta_x;
			new_stack_proportion = grabc->old_stack_proportion + delta_y;
		}

		new_scroller_proportion =
			fmaxf(0.1f, fminf(1.0f, new_scroller_proportion));
		new_stack_proportion = fmaxf(0.1f, fminf(0.9f, new_stack_proportion));

		// keep the total at 1 so the later arrange normalization doesn't eat the offset
		if (isdrag) {
			float current_other_sum = 1.0f - curnode->stack_proportion;
			float new_other_sum = 1.0f - new_stack_proportion;
			if (current_other_sum > 0.001f) {
				float scale = new_other_sum / current_other_sum;
				for (struct ScrollerStackNode *tc = headnode; tc;
					 tc = tc->next_in_stack) {
					if (tc != curnode) {
						tc->stack_proportion *= scale;
					}
				}
			}
		} else {
			// keyboard stepping
			if (grabc->old_stack_proportion != 1.0f) {
				for (struct ScrollerStackNode *tc = headnode; tc;
					 tc = tc->next_in_stack) {
					if (tc != curnode) {
						tc->stack_proportion =
							(1.0f - new_stack_proportion) /
							(1.0f - grabc->old_stack_proportion) *
							tc->stack_proportion;
					}
				}
			}
		}

		curnode->stack_proportion = new_stack_proportion;
		headnode->scroller_proportion = new_scroller_proportion;

		/* sync back to the global fields */
		sync_scroller_state_to_clients(m, tag);

		if (!isdrag) {
			arrange(m, false, false);
			return;
		}

		if (last_apply_drap_time == 0 ||
			time - last_apply_drap_time > config.drag_tile_refresh_interval) {
			arrange(m, false, false);
			last_apply_drap_time = time;
		}
	}
}

void resize_tile_client(Client *grabc, bool isdrag, int32_t offsetx,
						int32_t offsety, uint32_t time) {

	if (!grabc || grabc->isfullscreen || grabc->ismaximizescreen)
		return;

	if (grabc->mon->isoverview)
		return;

	const Layout *current_layout =
		grabc->mon->pertag->ltidxs[grabc->mon->pertag->curtag];
	if (current_layout->id == SCROLLER) {
		resize_tile_scroller(grabc, isdrag, offsetx, offsety, time, false);
	} else if (current_layout->id == DWINDLE) {
		resize_tile_dwindle(grabc, isdrag, offsetx, offsety, time, true);
	}
}

/* If there are no calculation omissions,
these two functions will never be triggered.
Just in case to facilitate the final investigation*/

void check_size_per_valid(Client *c) {
	if (c->ismaster) {
		assert(c->master_inner_per > 0.0f && c->master_inner_per <= 1.0f);
	} else {
		assert(c->stack_inner_per > 0.0f && c->stack_inner_per <= 1.0f);
	}
}

void reset_size_per_mon(Monitor *m, int32_t tile_cilent_num,
						double total_left_stack_hight_percent,
						double total_right_stack_hight_percent,
						double total_stack_hight_percent,
						double total_master_inner_percent, int32_t master_num,
						int32_t stack_num) {
	Client *c = NULL;
	int32_t i = 0;
	uint32_t stack_index = 0;
	uint32_t nmasters = m->pertag->nmasters[m->pertag->curtag];

	(void)tile_cilent_num;
	(void)total_left_stack_hight_percent;
	(void)total_right_stack_hight_percent;
	(void)stack_index;

	wl_list_for_each(c, &clients, link) {
		if (VISIBLEON(c, m) && ISFAKETILED(c)) {

			if (total_master_inner_percent > 0.0 && i < nmasters) {
				c->ismaster = true;
				c->stack_inner_per = stack_num ? 1.0f / stack_num : 1.0f;
				c->master_inner_per =
					c->master_inner_per / total_master_inner_percent;
			} else {
				c->ismaster = false;
				c->master_inner_per =
					master_num > 0 ? 1.0f / master_num : 1.0f;
				c->stack_inner_per =
					total_stack_hight_percent
						? c->stack_inner_per / total_stack_hight_percent
						: 1.0f;
			}
			i++;

			check_size_per_valid(c);
		}
	}
}

void pre_caculate_before_arrange(Monitor *m, bool want_animation,
								 bool from_view, bool only_caculate) {
	Client *c = NULL;
	double total_stack_inner_percent = 0;
	double total_master_inner_percent = 0;
	double total_right_stack_hight_percent = 0;
	double total_left_stack_hight_percent = 0;
	int32_t i = 0;
	int32_t nmasters = 0;
	int32_t stack_index = 0;
	int32_t master_num = 0;
	int32_t stack_num = 0;

	m->visible_clients = 0;
	m->visible_tiling_clients = 0;
	m->visible_scroll_tiling_clients = 0;
	m->visible_fake_tiling_clients = 0;

	uint32_t tag = m->pertag->curtag;
	struct TagScrollerState *st = m->pertag->scroller_state[tag];

	const Layout *cur_layout = m->pertag->ltidxs[m->pertag->curtag];
	if (cur_layout->id == SCROLLER) {
		update_scroller_state(m);
	}

	wl_list_for_each(c, &clients, link) {

		if (from_view && (c->isglobal || c->isunglobal)) {
			set_size_per(m, c);
		}

		if (m->is_jump_mode && !c->jump_label_node) {
			client_add_jump_label_node(c);
		}

		/* Per-window tabs are nested inside c->scene and hide with it, but
		 * monocle SEGMENT tabs live on the global LyrDecorate (a hidden
		 * monocle window's segment must stay visible), so switching away
		 * from a tag doesn't implicitly hide those. Reset ALL to hidden here
		 * (cheap, shadow-synced); the ones still shown get re-enabled in
		 * client_draw_titlebar()/client_draw_monocle_titlebar_segment(). */
		if (c->titlebar_node && c->mon == m) {
			asteroidz_tab_bar_node_set_enabled(c->titlebar_node, false);
		}
		if (c->titlebar_close_node && c->mon == m) {
			asteroidz_tab_bar_node_set_enabled(c->titlebar_close_node, false);
		}

		if (c->mon == m && (c->isglobal || c->isunglobal)) {
			c->tags = m->tagset[m->seltags];
		}

		if (from_view && m->sel == NULL && c->isglobal && VISIBLEON(c, m)) {
			focusclient(c, 1);
		}

		if (VISIBLEON(c, m)) {
			if (from_view && !client_only_in_one_tag(c)) {
				set_size_per(m, c);
			}

			if (!c->isunglobal)
				m->visible_clients++;

			if (ISTILED(c)) {
				m->visible_tiling_clients++;

				/* update the visible scroll-tiling client count */
				if (st) {
					struct ScrollerStackNode *n = find_scroller_node(st, c);
					if (n && !n->prev_in_stack) /* is the stack head */
						m->visible_scroll_tiling_clients++;
				} else if (ISSCROLLTILED(c)) {
					m->visible_scroll_tiling_clients++;
				}
			}

			if (ISFAKETILED(c)) {
				m->visible_fake_tiling_clients++;
			}
		}
	}

	nmasters = m->pertag->nmasters[m->pertag->curtag];

	wl_list_for_each(c, &clients, link) {
		if (c->iskilling)
			continue;

		if (c->mon == m) {
			if (VISIBLEON(c, m)) {
				if (ISFAKETILED(c)) {
					if (i < nmasters) {
						master_num++;
						total_master_inner_percent += c->master_inner_per;
					} else {
						stack_num++;
						total_stack_inner_percent += c->stack_inner_per;
						stack_index = i - nmasters;
						if ((stack_index % 2) ^
							(m->visible_tiling_clients % 2 == 0)) {
							c->isleftstack = false;
							total_right_stack_hight_percent +=
								c->stack_inner_per;
						} else {
							c->isleftstack = true;
							total_left_stack_hight_percent +=
								c->stack_inner_per;
						}
					}
					i++;
				}

				if (!only_caculate)
					set_arrange_visible(m, c, want_animation);
			} else {
				if (!only_caculate)
					set_arrange_hidden(m, c, want_animation);
			}
		}

		if (!only_caculate && c->mon == m && c->ismaximizescreen &&
			!c->animation.tagouted && !c->animation.tagouting &&
			VISIBLEON(c, m)) {
			reset_maximizescreen_size(c);
		}
	}

	reset_size_per_mon(
		m, m->visible_tiling_clients, total_left_stack_hight_percent,
		total_right_stack_hight_percent, total_stack_inner_percent,
		total_master_inner_percent, master_num, stack_num);
}

/* Tiling layout used inside a named special workspace. Special-workspace
 * clients are deliberately kept out of the per-tag layout state
 * (m->pertag->{nmasters,mfacts,dwindle_root,scroller_state}[...]) since
 * that state is keyed by the underlying tag that stays selected while the
 * special workspace overlays it; reusing it here would corrupt whichever
 * normal tag is currently hidden underneath. Instead this is a small
 * self-contained master/stack ("tile") layout, using the global
 * config.default_nmaster/config.default_mfact as its master count/ratio,
 * so opening any special workspace always gets a real tiling layout
 * without needing a per-name copy of every layout's persistent state. */
void arrange_special(Monitor *m) {
	Client *c = NULL;
	int32_t i, n = 0, h, r;
	int32_t ie = enablegaps;
	int32_t mw, my, ty;
	int32_t nmaster = config.default_nmaster > 0 ? (int32_t)config.default_nmaster : 1;
	double mfact = config.default_mfact > 0.0f ? config.default_mfact : 0.5;

	n = m->visible_fake_tiling_clients;
	if (n == 0)
		return;

	int32_t cur_gappiv = enablegaps ? m->gappiv : 0;
	int32_t cur_gappih = enablegaps ? m->gappih : 0;
	int32_t cur_gappov = enablegaps ? m->gappov : 0;
	int32_t cur_gappoh = enablegaps ? m->gappoh : 0;

	if (config.smartgaps && n == 1) {
		cur_gappiv = cur_gappih = cur_gappov = cur_gappoh = 0;
	}

	if (n > nmaster)
		mw = nmaster ? (m->w.width + cur_gappih * ie) * mfact : 0;
	else
		mw = m->w.width - 2 * cur_gappoh + cur_gappih * ie;

	i = 0;
	my = ty = cur_gappov;

	wl_list_for_each(c, &clients, link) {
		if (!VISIBLEON(c, m) || !ISFAKETILED(c))
			continue;

		if (i < nmaster) {
			r = ASTEROIDZ_MIN(n, nmaster) - i;
			h = (m->w.height - my - cur_gappov - cur_gappiv * ie * (r - 1)) / r;
			client_tile_resize(c,
							   (struct wlr_box){.x = m->w.x + cur_gappoh,
												.y = m->w.y + my,
												.width = mw - cur_gappih * ie,
												.height = h},
							   0);
			my += h + cur_gappiv * ie;
		} else {
			r = n - i;
			h = (m->w.height - ty - cur_gappov - cur_gappiv * ie * (r - 1)) / r;
			client_tile_resize(
				c,
				(struct wlr_box){.x = m->w.x + mw + cur_gappoh,
								 .y = m->w.y + ty,
								 .width = m->w.width - mw - 2 * cur_gappoh,
								 .height = h},
				0);
			ty += h + cur_gappiv * ie;
		}
		i++;
	}
}

void // 17
arrange(Monitor *m, bool want_animation, bool from_view) {

	if (!m)
		return;

	if (!m->wlr_output->enabled)
		return;

	if (!m->sel) {
		m->sel = focustop(m);
	}

	pre_caculate_before_arrange(m, want_animation, from_view, false);

	/* while a close-fade is in flight keep the chrome nodes alive; the fade in
	 * rendermon tears them down (overview_hide_chrome) when it completes */
	bool ov_closing = m->ov_anim_running && !m->ov_anim_open;

	if (m->isoverview) {
		overviewlayout.arrange(m);
	} else if (m->active_special) {
		if (!ov_closing)
			overview_hide_chrome(m);
		arrange_special(m);
	} else {
		if (!ov_closing)
			overview_hide_chrome(m);
		m->pertag->ltidxs[m->pertag->curtag]->arrange(m);
	}

	if (!start_drag_window) {
		motionnotify(0, NULL, 0, 0, 0, 0);
		checkidleinhibitor(NULL);
	}

	printstatus(IPC_WATCH_ARRANGGE);
}
