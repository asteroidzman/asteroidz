/* Should the compositor draw server-side chrome (titlebar, border, corner
 * squaring) for this client? X11 windows have no CSD negotiation -- always
 * decorate. An xdg client that never bound xdg-decoration is drawing its own
 * decorations per the protocol default (CSD popups like the DMS spotlight:
 * a rounded pill inside a transparent-margined buffer) -- wrapping those in a
 * border/titlebar paints stray lines around the buffer rect. Clients that DID
 * bind get forced to server-side unless an allow_csd rule lets their request
 * through (mirrors requestdecorationmode). */
bool client_wants_ssd(Client *c) {
	if (client_is_x11(c))
		return true;
	if (c->force_ssd) /* window rule: decorate even decoration-oblivious apps
					   * (SDL/GLFW games etc. that bind neither protocol) */
		return true;
	if (!c->decoration)
		/* never bound xdg-decoration: CSD by default; misc/prefer-no-csd
		 * decorates these too (allow_csd rule stays the escape hatch).
		 * NB: apps that paint their own header regardless (GTK headerbars)
		 * will show both -- the compositor can't stop client-side paint. */
		return config.prefer_no_csd && !c->allow_csd;
	if (!c->allow_csd)
		return true;
	return c->decoration->requested_mode ==
		   WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE;
}

bool check_hit_no_border(Client *c) {
	bool hit_no_border = false;

	if (!c->mon)
		return false;

	if (c->tags <= 0)
		return false;

	if (!render_border) {
		hit_no_border = true;
	}

	/* client-side-decorated windows draw their own chrome; a server border
	 * around their (often transparent-margined) buffer reads as a stray ring */
	if (!client_wants_ssd(c)) {
		hit_no_border = true;
	}

	if (c->mon && !c->mon->isoverview &&
		c->mon->pertag->no_render_border[get_tags_first_tag_num(c->tags)]) {
		hit_no_border = true;
	}

	if (config.no_border_when_single && c && c->mon &&
		((ISSCROLLTILED(c) && c->mon->visible_scroll_tiling_clients == 1) ||
		 c->mon->visible_clients == 1)) {
		hit_no_border = true;
	}
	return hit_no_border;
}

Client *termforwin(Client *w) {
	Client *c = NULL;

	if (!w->pid || w->isterm || w->noswallow)
		return NULL;

	wl_list_for_each(c, &fstack, flink) {
		if (c->isterm && !c->swallowing && c->pid &&
			isdescprocess(c->pid, w->pid)) {
			return c;
		}
	}

	return NULL;
}
Client *get_client_by_id_or_title(const char *arg_id, const char *arg_title) {
	Client *target_client = NULL;
	const char *appid, *title;
	Client *c = NULL;
	wl_list_for_each(c, &clients, link) {
		if (!config.scratchpad_cross_monitor && c->mon != selmon) {
			continue;
		}

		if (c->swallowedby) {
			appid = client_get_appid(c->swallowedby);
			title = client_get_title(c->swallowedby);
		} else {
			appid = client_get_appid(c);
			title = client_get_title(c);
		}

		if (!appid) {
			appid = broken;
		}

		if (!title) {
			title = broken;
		}

		if (arg_id && strncmp(arg_id, "none", 4) == 0)
			arg_id = NULL;

		if (arg_title && strncmp(arg_title, "none", 4) == 0)
			arg_title = NULL;

		if ((arg_title && regex_match(arg_title, title) && !arg_id) ||
			(arg_id && regex_match(arg_id, appid) && !arg_title) ||
			(arg_id && regex_match(arg_id, appid) && arg_title &&
			 regex_match(arg_title, title))) {
			target_client = c;
			break;
		}
	}
	return target_client;
}
struct wlr_box // compute centered client coordinates
setclient_coordinate_center(Client *c, Monitor *tm, struct wlr_box geom,
							int32_t offsetx, int32_t offsety) {
	struct wlr_box tempbox;
	int32_t offset = 0;
	int32_t len = 0;
	Monitor *m = tm ? tm : selmon;

	if (!m)
		return geom;

	uint32_t cbw = c && check_hit_no_border(c) ? c->bw : 0;

	if ((!c || !c->no_force_center) && m) {
		tempbox.x = m->w.x + (m->w.width - geom.width) / 2;
		tempbox.y = m->w.y + (m->w.height - geom.height) / 2;
	} else {
		tempbox.x = geom.x;
		tempbox.y = geom.y;
	}

	tempbox.width = geom.width;
	tempbox.height = geom.height;

	if (offsetx != 0) {
		len = (m->w.width - tempbox.width - 2 * m->gappoh) / 2;
		offset = len * (offsetx / 100.0);
		tempbox.x += offset;

		// clamp the window inside the screen
		if (tempbox.x < m->m.x) {
			tempbox.x = m->m.x - cbw;
		}
		if (tempbox.x + tempbox.width > m->m.x + m->m.width) {
			tempbox.x = m->m.x + m->m.width - tempbox.width + cbw;
		}
	}
	if (offsety != 0) {
		len = (m->w.height - tempbox.height - 2 * m->gappov) / 2;
		offset = len * (offsety / 100.0);
		tempbox.y += offset;

		// clamp the window inside the screen
		if (tempbox.y < m->m.y) {
			tempbox.y = m->m.y - cbw;
		}
		if (tempbox.y + tempbox.height > m->m.y + m->m.height) {
			tempbox.y = m->m.y + m->m.height - tempbox.height + cbw;
		}
	}

	return tempbox;
}
/* Helper: Check if rule matches client. At least one matcher must be set,
 * and every set matcher must match. */
static bool is_window_rule_matches(const ConfigWinRule *r, Client *c,
								   const char *appid, const char *title) {
	if (!r->title && !r->id && !r->toplevel_tag)
		return false;
	if (r->title && !regex_match(r->title, title))
		return false;
	if (r->id && !regex_match(r->id, appid))
		return false;
	if (r->toplevel_tag &&
		(!c->toplevel_tag || !regex_match(r->toplevel_tag, c->toplevel_tag)))
		return false;
	return true;
}

Client *center_tiled_select(Monitor *m) {
	Client *c = NULL;
	Client *target_c = NULL;
	int64_t mini_distance = -1;
	int32_t dirx, diry;
	int64_t distance;
	wl_list_for_each(c, &clients, link) {
		if (c && VISIBLEON(c, m) && ISSCROLLTILED(c) &&
			client_surface(c)->mapped && !c->isfloating &&
			!client_is_unmanaged(c)) {
			dirx = c->geom.x + c->geom.width / 2 - (m->w.x + m->w.width / 2);
			diry = c->geom.y + c->geom.height / 2 - (m->w.y + m->w.height / 2);
			distance = dirx * dirx + diry * diry;
			if (distance < mini_distance || mini_distance == -1) {
				mini_distance = distance;
				target_c = c;
			}
		}
	}
	return target_c;
}

Client *find_client_by_direction(Client *tc, const Arg *arg,
								 bool findfloating) {
	Client *c = NULL;
	Client *tempFocusClients = NULL;
	Client *tempSameMonitorFocusClients = NULL;
	int64_t distance = LLONG_MAX;
	int64_t same_monitor_distance = LLONG_MAX;

	int32_t tc_l = tc->geom.x;
	int32_t tc_r = tc->geom.x + tc->geom.width;
	int32_t tc_t = tc->geom.y;
	int32_t tc_b = tc->geom.y + tc->geom.height;
	int32_t tc_cx = tc_l + tc->geom.width / 2;
	int32_t tc_cy = tc_t + tc->geom.height / 2;

	for (int32_t step = 0; step < 2; step++) {
		if (step == 1 && tempFocusClients)
			break;

		wl_list_for_each(c, &clients, link) {
			if (!c || !c->mon || c == tc)
				continue;
			if (!findfloating && c->isfloating)
				continue;
			if (c->is_monocle_hide)
				continue;
			if (c->isunglobal)
				continue;
			if (!config.focus_cross_monitor && c->mon != tc->mon)
				continue;
			if (!(c->tags & c->mon->tagset[c->mon->seltags]))
				continue;

			int32_t c_l = c->geom.x;
			int32_t c_r = c->geom.x + c->geom.width;
			int32_t c_t = c->geom.y;
			int32_t c_b = c->geom.y + c->geom.height;
			int32_t c_cx = c_l + c->geom.width / 2;
			int32_t c_cy = c_t + c->geom.height / 2;

			int64_t main_dist = 0;
			int64_t orth_dist = 0;
			bool match_dir = false;

			switch (arg->i) {
			case LEFT:
				if (c_cx < tc_cx || (c_cx == tc_cx && c_l < tc_l)) {
					match_dir = true;
					main_dist = tc_l - c_r;
					orth_dist = (c_b < tc_t)
									? (tc_t - c_b)
									: ((c_t > tc_b) ? (c_t - tc_b) : 0);
				}
				break;
			case RIGHT:
				if (c_cx > tc_cx || (c_cx == tc_cx && c_l > tc_l)) {
					match_dir = true;
					main_dist = c_l - tc_r;
					orth_dist = (c_b < tc_t)
									? (tc_t - c_b)
									: ((c_t > tc_b) ? (c_t - tc_b) : 0);
				}
				break;
			case UP:
				if (c_cy < tc_cy || (c_cy == tc_cy && c_t < tc_t)) {
					match_dir = true;
					main_dist = tc_t - c_b;
					orth_dist = (c_r < tc_l)
									? (tc_l - c_r)
									: ((c_l > tc_r) ? (c_l - tc_r) : 0);
				}
				break;
			case DOWN:
				if (c_cy > tc_cy || (c_cy == tc_cy && c_t > tc_t)) {
					match_dir = true;
					main_dist = c_t - tc_b;
					orth_dist = (c_r < tc_l)
									? (tc_l - c_r)
									: ((c_l > tc_r) ? (c_l - tc_r) : 0);
				}
				break;
			default:
				continue;
			}

			if (!match_dir)
				continue;

			if (step == 0) {
				if (c->mon != tc->mon)
					continue;
				if (!tc->mon->isoverview &&
					!client_is_in_same_stack(tc, c, NULL))
					continue;
				if (orth_dist != 0)
					continue;
			}

			int64_t penalty = 0;
			if (main_dist < 0) {
				penalty = 10000000000LL; // huge penalty for overlap in the main direction (wrong side)
				main_dist = -main_dist;
			}

			// penalty for no overlap in the orthogonal direction, prefer windows in the same row/column
			int64_t no_overlap_penalty = 0;
			if (orth_dist > 0) {
				// for LEFT/RIGHT, orth_dist is the vertical gap; >0 means no vertical overlap
				// for UP/DOWN, orth_dist is the horizontal gap; >0 means no horizontal overlap
				no_overlap_penalty = 10000000LL;
			}

			int64_t tmp_distance = penalty + no_overlap_penalty +
								   (main_dist * main_dist) +
								   (orth_dist * orth_dist);

			if (tmp_distance < distance) {
				distance = tmp_distance;
				tempFocusClients = c;
			}
			if (c->mon == tc->mon && tmp_distance < same_monitor_distance) {
				same_monitor_distance = tmp_distance;
				tempSameMonitorFocusClients = c;
			}
		}
	}

	if (tempSameMonitorFocusClients)
		return tempSameMonitorFocusClients;
	return tempFocusClients;
}

Client *direction_select(const Arg *arg) {

	Client *tc = arg->tc ? arg->tc : selmon->sel;

	if (!tc)
		return NULL;

	if (tc && (tc->isfullscreen || tc->ismaximizescreen) &&
		(!is_scroller_layout(selmon) || tc->isfloating)) {
		return NULL;
	}

	return find_client_by_direction(tc, arg, true);
}

/* We probably should change the name of this, it sounds like
 * will focus the topmost client of this mon, when actually will
 * only return that client */
Client *focustop(Monitor *m) {
	Client *c = NULL;
	wl_list_for_each(c, &fstack, flink) {
		if (c->iskilling || c->isunglobal)
			continue;
		if (VISIBLEON(c, m))
			return c;
	}
	return NULL;
}

Client *get_next_stack_client(Client *c, bool reverse) {
	if (!c || !c->mon)
		return NULL;

	Client *next = NULL;
	if (reverse) {
		wl_list_for_each_reverse(next, &c->link, link) {
			if (&next->link == &clients)
				continue; /* wrap past the sentinel node */

			if (next->isunglobal)
				continue;

			if (next != c && next->mon && VISIBLEON(next, c->mon))
				return next;
		}
	} else {
		wl_list_for_each(next, &c->link, link) {
			if (&next->link == &clients)
				continue; /* wrap past the sentinel node */

			if (next->isunglobal)
				continue;

			if (next != c && next->mon && VISIBLEON(next, c->mon))
				return next;
		}
	}
	return NULL;
}

float *get_border_color(Client *c) {

	/* an inactive window sitting under a titlebar should read as one piece
	 * with it, so match the titlebar's own inactive pill color instead of
	 * the generic border color. */
	bool has_titlebar = c->titlebar_node && c->mon && client_wants_ssd(c) &&
					   !client_is_splash(c) &&
					   (config.enable_titlebar || is_monocle_layout(c->mon)) &&
					   ISFAKETILED(c);
	float *inactive_color =
		has_titlebar ? config.theme.bg_color : config.bordercolor;

	if (c->mon != selmon) {
		return inactive_color;
	} else if (c->isurgent) {
		return config.urgentcolor;
	} else if (c->is_in_scratchpad && selmon && c == selmon->sel) {
		return config.scratchpadcolor;
	} else if (c->isglobal && selmon && c == selmon->sel) {
		return config.globalcolor;
	} else if (c->isoverlay && selmon && c == selmon->sel) {
		return config.overlaycolor;
	} else if (c->ismaximizescreen && selmon && c == selmon->sel) {
		return config.maximizescreencolor;
	} else if (selmon && c == selmon->sel) {
		return config.focuscolor;
	} else {
		return inactive_color;
	}
}

int32_t is_single_bit_set(uint32_t x) { return x && !(x & (x - 1)); }

bool client_only_in_one_tag(Client *c) {
	uint32_t masked = c->tags & TAGMASK;
	if (is_single_bit_set(masked)) {
		return true;
	} else {
		return false;
	}
}

bool client_is_in_same_stack(Client *sc, Client *tc, Client *fc) {
	if (!sc || !tc)
		return false;

	uint32_t id = sc->mon->pertag->ltidxs[sc->mon->pertag->curtag]->id;

	if (id != SCROLLER)
		return false;

	Client *source_stack_head = scroll_get_stack_head_client(sc);
	Client *target_stack_head = scroll_get_stack_head_client(tc);
	Client *fc_head = fc ? scroll_get_stack_head_client(fc) : NULL;

	if (fc && fc_head == source_stack_head)
		return false;
	if (source_stack_head == target_stack_head)
		return true;
	else
		return false;
}

Client *get_focused_stack_client(Client *sc, Client *custom_focus_client) {
	if (!sc || sc->isfloating)
		return sc;

	Client *tc = NULL;
	Client *fc = custom_focus_client ? custom_focus_client : focustop(sc->mon);

	if (fc->isfloating || sc->isfloating)
		return sc;

	wl_list_for_each(tc, &fstack, flink) {
		if (tc->iskilling || tc->isunglobal || tc->is_monocle_hide)
			continue;
		if (!VISIBLEON(tc, sc->mon))
			continue;
		if (tc == fc)
			continue;

		if (client_is_in_same_stack(sc, tc, fc)) {
			return tc;
		}
	}
	return sc;
}