void tile(Monitor *m) {
	int32_t i, n = 0, h, r, ie = enablegaps, mw, my, ty;
	Client *c = NULL;
	Client *fc = NULL;
	double mfact = 0;
	int32_t master_num = 0;
	int32_t stack_num = 0;

	n = m->visible_fake_tiling_clients;
	master_num = m->pertag->nmasters[m->pertag->curtag];
	master_num = n > master_num ? master_num : n;
	stack_num = n - master_num;

	if (n == 0)
		return;

	int32_t cur_gappiv = enablegaps ? m->gappiv : 0;
	int32_t cur_gappih = enablegaps ? m->gappih : 0;
	int32_t cur_gappov = enablegaps ? m->gappov : 0;
	int32_t cur_gappoh = enablegaps ? m->gappoh : 0;

	cur_gappiv = config.smartgaps && m->visible_fake_tiling_clients == 1
					 ? 0
					 : cur_gappiv;
	cur_gappih = config.smartgaps && m->visible_fake_tiling_clients == 1
					 ? 0
					 : cur_gappih;
	cur_gappov = config.smartgaps && m->visible_fake_tiling_clients == 1
					 ? 0
					 : cur_gappov;
	cur_gappoh = config.smartgaps && m->visible_fake_tiling_clients == 1
					 ? 0
					 : cur_gappoh;

	wl_list_for_each(fc, &clients, link) {
		if (VISIBLEON(fc, m) && ISFAKETILED(fc))
			break;
	}

	mfact = fc->master_mfact_per > 0.0f ? fc->master_mfact_per
										: m->pertag->mfacts[m->pertag->curtag];

	if (n > m->pertag->nmasters[m->pertag->curtag])
		mw = m->pertag->nmasters[m->pertag->curtag]
				 ? (m->w.width + cur_gappih * ie) * mfact
				 : 0;
	else
		mw = m->w.width - 2 * cur_gappoh + cur_gappih * ie;

	i = 0;
	my = ty = cur_gappov;

	int32_t master_surplus_height =
		(m->w.height - 2 * cur_gappov - cur_gappiv * ie * (master_num - 1));
	float master_surplus_ratio = 1.0;

	int32_t slave_surplus_height =
		(m->w.height - 2 * cur_gappov - cur_gappiv * ie * (stack_num - 1));
	float slave_surplus_ratio = 1.0;

	wl_list_for_each(c, &clients, link) {
		if (!VISIBLEON(c, m) || !ISFAKETILED(c))
			continue;
		if (i < m->pertag->nmasters[m->pertag->curtag]) {
			r = ASTEROIDZ_MIN(n, m->pertag->nmasters[m->pertag->curtag]) - i;
			if (c->master_inner_per > 0.0f) {
				h = master_surplus_height * c->master_inner_per /
					master_surplus_ratio;
				master_surplus_height = master_surplus_height - h;
				master_surplus_ratio =
					master_surplus_ratio - c->master_inner_per;
				c->master_mfact_per = mfact;
			} else {
				h = (m->w.height - my - cur_gappov -
					 cur_gappiv * ie * (r - 1)) /
					r;
				c->master_inner_per = h / (m->w.height - my - cur_gappov -
										   cur_gappiv * ie * (r - 1));
				c->master_mfact_per = mfact;
			}
			client_tile_resize(c,
							   (struct wlr_box){.x = m->w.x + cur_gappoh,
												.y = m->w.y + my,
												.width = mw - cur_gappih * ie,
												.height = h},
							   0);
			my += h + cur_gappiv * ie; // accumulate using the theoretical height
		} else {
			r = n - i;
			if (c->stack_inner_per > 0.0f) {
				h = slave_surplus_height * c->stack_inner_per /
					slave_surplus_ratio;
				slave_surplus_height = slave_surplus_height - h;
				slave_surplus_ratio = slave_surplus_ratio - c->stack_inner_per;
				c->master_mfact_per = mfact;
			} else {
				h = (m->w.height - ty - cur_gappov -
					 cur_gappiv * ie * (r - 1)) /
					r;
				c->stack_inner_per = h / (m->w.height - ty - cur_gappov -
										  cur_gappiv * ie * (r - 1));
				c->master_mfact_per = mfact;
			}

			client_tile_resize(
				c,
				(struct wlr_box){.x = m->w.x + mw + cur_gappoh,
								 .y = m->w.y + ty,
								 .width = m->w.width - mw - 2 * cur_gappoh,
								 .height = h},
				0);
			ty += h + cur_gappiv * ie; // accumulate using the theoretical height
		}
		i++;
	}
}

void right_tile(Monitor *m) {
	int32_t i, n = 0, h, r, ie = enablegaps, mw, my, ty;
	Client *c = NULL;
	Client *fc = NULL;
	double mfact = 0;
	int32_t master_num = 0;
	int32_t stack_num = 0;

	n = m->visible_fake_tiling_clients;
	master_num = m->pertag->nmasters[m->pertag->curtag];
	master_num = n > master_num ? master_num : n;
	stack_num = n - master_num;

	if (n == 0)
		return;

	int32_t cur_gappiv = enablegaps ? m->gappiv : 0;
	int32_t cur_gappih = enablegaps ? m->gappih : 0;
	int32_t cur_gappov = enablegaps ? m->gappov : 0;
	int32_t cur_gappoh = enablegaps ? m->gappoh : 0;

	cur_gappiv = config.smartgaps && m->visible_fake_tiling_clients == 1
					 ? 0
					 : cur_gappiv;
	cur_gappih = config.smartgaps && m->visible_fake_tiling_clients == 1
					 ? 0
					 : cur_gappih;
	cur_gappov = config.smartgaps && m->visible_fake_tiling_clients == 1
					 ? 0
					 : cur_gappov;
	cur_gappoh = config.smartgaps && m->visible_fake_tiling_clients == 1
					 ? 0
					 : cur_gappoh;

	wl_list_for_each(fc, &clients, link) {
		if (VISIBLEON(fc, m) && ISFAKETILED(fc))
			break;
	}

	mfact = fc->master_mfact_per > 0.0f ? fc->master_mfact_per
										: m->pertag->mfacts[m->pertag->curtag];

	if (n > m->pertag->nmasters[m->pertag->curtag])
		mw = m->pertag->nmasters[m->pertag->curtag]
				 ? (m->w.width + cur_gappih * ie) * mfact
				 : 0;
	else
		mw = m->w.width - 2 * cur_gappoh + cur_gappih * ie;

	i = 0;
	my = ty = cur_gappov;

	int32_t master_surplus_height =
		(m->w.height - 2 * cur_gappov - cur_gappiv * ie * (master_num - 1));
	float master_surplus_ratio = 1.0;

	int32_t slave_surplus_height =
		(m->w.height - 2 * cur_gappov - cur_gappiv * ie * (stack_num - 1));
	float slave_surplus_ratio = 1.0;

	wl_list_for_each(c, &clients, link) {
		if (!VISIBLEON(c, m) || !ISFAKETILED(c))
			continue;
		if (i < m->pertag->nmasters[m->pertag->curtag]) {
			r = ASTEROIDZ_MIN(n, m->pertag->nmasters[m->pertag->curtag]) - i;
			if (c->master_inner_per > 0.0f) {
				h = master_surplus_height * c->master_inner_per /
					master_surplus_ratio;
				master_surplus_height = master_surplus_height - h;
				master_surplus_ratio =
					master_surplus_ratio - c->master_inner_per;
				c->master_mfact_per = mfact;
			} else {
				h = (m->w.height - my - cur_gappov -
					 cur_gappiv * ie * (r - 1)) /
					r;
				c->master_inner_per = h / (m->w.height - my - cur_gappov -
										   cur_gappiv * ie * (r - 1));
				c->master_mfact_per = mfact;
			}
			client_tile_resize(c,
							   (struct wlr_box){.x = m->w.x + m->w.width - mw -
													 cur_gappoh +
													 cur_gappih * ie,
												.y = m->w.y + my,
												.width = mw - cur_gappih * ie,
												.height = h},
							   0);
			my += h + cur_gappiv * ie; // accumulate using the theoretical height
		} else {
			r = n - i;
			if (c->stack_inner_per > 0.0f) {
				h = slave_surplus_height * c->stack_inner_per /
					slave_surplus_ratio;
				slave_surplus_height = slave_surplus_height - h;
				slave_surplus_ratio = slave_surplus_ratio - c->stack_inner_per;
				c->master_mfact_per = mfact;
			} else {
				h = (m->w.height - ty - cur_gappov -
					 cur_gappiv * ie * (r - 1)) /
					r;
				c->stack_inner_per = h / (m->w.height - ty - cur_gappov -
										  cur_gappiv * ie * (r - 1));
				c->master_mfact_per = mfact;
			}

			client_tile_resize(
				c,
				(struct wlr_box){.x = m->w.x + cur_gappoh,
								 .y = m->w.y + ty,
								 .width = m->w.width - mw - 2 * cur_gappoh,
								 .height = h},
				0);
			ty += h + cur_gappiv * ie; // accumulate using the theoretical height
		}
		i++;
	}
}

void center_tile(Monitor *m) {
	int32_t i, n = 0, h, r, ie = enablegaps, mw, mx, my, oty, ety, tw;
	Client *c = NULL;
	Client *fc = NULL;
	double mfact = 0;
	int32_t master_num = 0;
	int32_t stack_num = 0;

	n = m->visible_fake_tiling_clients;
	master_num = m->pertag->nmasters[m->pertag->curtag];
	master_num = n > master_num ? master_num : n;
	stack_num = n - master_num;

	if (n == 0)
		return;

	// get the first visible tiled client to derive the master area width percentage
	wl_list_for_each(fc, &clients, link) {
		if (VISIBLEON(fc, m) && ISFAKETILED(fc))
			break;
	}

	// handle gap parameters
	int32_t cur_gappiv = enablegaps ? m->gappiv : 0; // inner vertical gap
	int32_t cur_gappih = enablegaps ? m->gappih : 0; // inner horizontal gap
	int32_t cur_gappov = enablegaps ? m->gappov : 0; // outer vertical gap
	int32_t cur_gappoh = enablegaps ? m->gappoh : 0; // outer horizontal gap

	// handle smart gaps
	cur_gappiv = config.smartgaps && m->visible_fake_tiling_clients == 1
					 ? 0
					 : cur_gappiv;
	cur_gappih = config.smartgaps && m->visible_fake_tiling_clients == 1
					 ? 0
					 : cur_gappih;
	cur_gappov = config.smartgaps && m->visible_fake_tiling_clients == 1
					 ? 0
					 : cur_gappov;
	cur_gappoh = config.smartgaps && m->visible_fake_tiling_clients == 1
					 ? 0
					 : cur_gappoh;

	int32_t nmasters = m->pertag->nmasters[m->pertag->curtag];
	mfact = fc->master_mfact_per > 0.0f ? fc->master_mfact_per
										: m->pertag->mfacts[m->pertag->curtag];

	// initialize areas
	mw = m->w.width;
	mx = cur_gappoh;
	my = cur_gappov;
	tw = mw;

	// determine whether the master area should overspread
	int32_t should_overspread =
		config.center_master_overspread && (n <= nmasters);

	int32_t master_surplus_height =
		(m->w.height - 2 * cur_gappov - cur_gappiv * ie * (master_num - 1));
	float master_surplus_ratio = 1.0;

	int32_t slave_left_surplus_height =
		(m->w.height - 2 * cur_gappov - cur_gappiv * ie * (stack_num / 2 - 1));
	float slave_left_surplus_ratio = 1.0;

	int32_t slave_right_surplus_height =
		(m->w.height - 2 * cur_gappov -
		 cur_gappiv * ie * ((stack_num + 1) / 2 - 1));
	float slave_right_surplus_ratio = 1.0;

	if (n > nmasters || !should_overspread) {
		// compute the master area width (centered mode)
		mw = nmasters ? (m->w.width - 2 * cur_gappoh - cur_gappih * ie) * mfact
					  : 0;

		if (n - nmasters > 1) {
			// multiple stack windows: master area centered, with a stack area on each side
			tw = (m->w.width - mw) / 2 - cur_gappoh - cur_gappih * ie;
			mx = cur_gappoh + tw + cur_gappih * ie;
		} else if (n - nmasters == 1) {
			// handle the single stack window case
			if (config.center_when_single_stack) {
				// stack on the right, master centered, left side empty
				tw = (m->w.width - mw) / 2 - cur_gappoh - cur_gappih * ie;
				mx = cur_gappoh + tw + cur_gappih * ie;
			} else {
				// stack on the right, master on the left
				tw = m->w.width - mw - 2 * cur_gappoh - cur_gappih * ie;
				mx = cur_gappoh;
			}
		} else {
			// only a master window: display it centered
			tw = (m->w.width - mw) / 2 - cur_gappoh - cur_gappih * ie;
			mx = cur_gappoh + tw + cur_gappih * ie;
		}
	} else {
		// master area overspread mode (only when there's a master window)
		mw = m->w.width - 2 * cur_gappoh;
		mx = cur_gappoh;
		tw = 0;
	}

	oty = cur_gappov;
	ety = cur_gappov;

	i = 0;
	wl_list_for_each(c, &clients, link) {
		if (!VISIBLEON(c, m) || !ISFAKETILED(c))
			continue;

		if (i < nmasters) {
			// master area window
			r = ASTEROIDZ_MIN(n, nmasters) - i;
			if (c->master_inner_per > 0.0f) {
				h = master_surplus_height * c->master_inner_per /
					master_surplus_ratio;
				master_surplus_height = master_surplus_height - h;
				master_surplus_ratio =
					master_surplus_ratio - c->master_inner_per;
				c->master_mfact_per = mfact;
			} else {
				h = (m->w.height - my - cur_gappov -
					 cur_gappiv * ie * (r - 1)) /
					r;
				c->master_inner_per = h / (m->w.height - my - cur_gappov -
										   cur_gappiv * ie * (r - 1));
				c->master_mfact_per = mfact;
			}

			client_tile_resize(c,
							   (struct wlr_box){.x = m->w.x + mx,
												.y = m->w.y + my,
												.width = mw,
												.height = h},
							   0);
			my += h + cur_gappiv * ie; // accumulate using the theoretical height
		} else {
			// stack area window
			int32_t stack_index = i - nmasters;

			if (n - nmasters == 1) {
				// single stack window
				r = n - i;
				if (c->stack_inner_per > 0.0f) {
					h = (m->w.height - 2 * cur_gappov -
						 cur_gappiv * ie * (stack_num - 1)) *
						c->stack_inner_per;
					c->master_mfact_per = mfact;
				} else {
					h = (m->w.height - ety - cur_gappov -
						 cur_gappiv * ie * (r - 1)) /
						r;
					c->stack_inner_per = h / (m->w.height - ety - cur_gappov -
											  cur_gappiv * ie * (r - 1));
					c->master_mfact_per = mfact;
				}

				int32_t stack_x;
				if (config.center_when_single_stack) {
					stack_x = m->w.x + mx + mw + cur_gappih * ie;
				} else {
					stack_x = m->w.x + mx + mw + cur_gappih * ie;
				}

				client_tile_resize(c,
								   (struct wlr_box){.x = stack_x,
													.y = m->w.y + ety,
													.width = tw,
													.height = h},
								   0);
				ety += h + cur_gappiv * ie; // accumulate using the theoretical height
			} else {
				// multiple stack windows: alternate placement on left and right
				r = (n - i + 1) / 2;

				if ((stack_index % 2) ^ (n % 2 == 0)) {
					// right-side stack window
					if (c->stack_inner_per > 0.0f) {
						h = slave_right_surplus_height * c->stack_inner_per /
							slave_right_surplus_ratio;
						slave_right_surplus_height =
							slave_right_surplus_height - h;
						slave_right_surplus_ratio =
							slave_right_surplus_ratio - c->stack_inner_per;
						c->master_mfact_per = mfact;
					} else {
						h = (m->w.height - ety - cur_gappov -
							 cur_gappiv * ie * (r - 1)) /
							r;
						c->stack_inner_per =
							h / (m->w.height - ety - cur_gappov -
								 cur_gappiv * ie * (r - 1));
						c->master_mfact_per = mfact;
					}

					int32_t stack_x = m->w.x + mx + mw + cur_gappih * ie;

					client_tile_resize(c,
									   (struct wlr_box){.x = stack_x,
														.y = m->w.y + ety,
														.width = tw,
														.height = h},
									   0);
					ety += h + cur_gappiv * ie; // accumulate using the theoretical height
				} else {
					// left-side stack window
					if (c->stack_inner_per > 0.0f) {
						h = slave_left_surplus_height * c->stack_inner_per /
							slave_left_surplus_ratio;
						slave_left_surplus_height =
							slave_left_surplus_height - h;
						slave_left_surplus_ratio =
							slave_left_surplus_ratio - c->stack_inner_per;
						c->master_mfact_per = mfact;
					} else {
						h = (m->w.height - oty - cur_gappov -
							 cur_gappiv * ie * (r - 1)) /
							r;
						c->stack_inner_per =
							h / (m->w.height - oty - cur_gappov -
								 cur_gappiv * ie * (r - 1));
						c->master_mfact_per = mfact;
					}

					int32_t stack_x = m->w.x + cur_gappoh;
					client_tile_resize(c,
									   (struct wlr_box){.x = stack_x,
														.y = m->w.y + oty,
														.width = tw,
														.height = h},
									   0);
					oty += h + cur_gappiv * ie; // accumulate using the theoretical height
				}
			}
		}
		i++;
	}
}

void deck(Monitor *m) {
	int32_t mw, my;
	int32_t i, n = 0;
	Client *c = NULL;
	Client *fc = NULL;
	float mfact;
	uint32_t nmasters = m->pertag->nmasters[m->pertag->curtag];

	int32_t cur_gappih = enablegaps ? m->gappih : 0;
	int32_t cur_gappoh = enablegaps ? m->gappoh : 0;
	int32_t cur_gappov = enablegaps ? m->gappov : 0;

	cur_gappih = config.smartgaps && m->visible_fake_tiling_clients == 1
					 ? 0
					 : cur_gappih;
	cur_gappoh = config.smartgaps && m->visible_fake_tiling_clients == 1
					 ? 0
					 : cur_gappoh;
	cur_gappov = config.smartgaps && m->visible_fake_tiling_clients == 1
					 ? 0
					 : cur_gappov;

	n = m->visible_fake_tiling_clients;

	if (n == 0)
		return;

	wl_list_for_each(fc, &clients, link) {
		if (VISIBLEON(fc, m) && ISFAKETILED(fc))
			break;
	}

	mfact = fc->master_mfact_per > 0.0f ? fc->master_mfact_per
										: m->pertag->mfacts[m->pertag->curtag];

	if (n > nmasters)
		mw = nmasters ? round((m->w.width - 2 * cur_gappoh) * mfact) : 0;
	else
		mw = m->w.width - 2 * cur_gappoh;

	i = my = 0;
	wl_list_for_each(c, &clients, link) {
		if (!VISIBLEON(c, m) || !ISFAKETILED(c))
			continue;
		if (i < nmasters) {
			c->master_mfact_per = mfact;
			int32_t h = (m->w.height - 2 * cur_gappov - my) /
						(ASTEROIDZ_MIN(n, nmasters) - i);
			client_tile_resize(c,
							   (struct wlr_box){.x = m->w.x + cur_gappoh,
												.y = m->w.y + cur_gappov + my,
												.width = mw,
												.height = h},
							   0);
			my += h;
		} else {
			// Stack area clients
			c->master_mfact_per = mfact;
			client_tile_resize(
				c,
				(struct wlr_box){.x = m->w.x + mw + cur_gappoh + cur_gappih,
								 .y = m->w.y + cur_gappov,
								 .width = m->w.width - mw - 2 * cur_gappoh -
										  cur_gappih,
								 .height = m->w.height - 2 * cur_gappov},
				0);
			if (c == focustop(m))
				wlr_scene_node_raise_to_top(&c->scene->node);
		}
		i++;
	}
}

void monocle(Monitor *m) {
	Client *c = NULL, *fc = NULL;
	struct wlr_box geom;
	int32_t cur_gappov = enablegaps ? m->gappov : 0;
	int32_t cur_gappoh = enablegaps ? m->gappoh : 0;
	int32_t cur_gapiv = enablegaps ? m->gappiv : 0;
	int32_t cur_gapih = enablegaps ? m->gappih : 0;

	if (config.smartgaps && m->visible_fake_tiling_clients == 1) {
		cur_gappov = cur_gappoh = cur_gapiv = cur_gapih = 0;
	}

	int n = m->visible_fake_tiling_clients;
	if (n == 0)
		return;

	wl_list_for_each(c, &fstack, flink) {
		if (c->iskilling || c->isunglobal || !ISFAKETILED(c))
			continue;
		if (VISIBLEON(c, m)) {
			fc = c;
			break;
		}
	}

	if (n == 1) {
		geom.x = m->w.x + cur_gappoh;
		geom.y = m->w.y + cur_gappov;
		geom.width = m->w.width - 2 * cur_gappoh;
		geom.height = m->w.height - 2 * cur_gappov;
		client_tile_resize(fc, geom, 0);
		monocle_set_focus(fc, true);
		return;
	}

	int monocle_tab_height = config.monocle_tab_height;

	int monocle_tab_inner_gap_height =
		config.monocle_tab_height > 0 ? 2 * cur_gapiv : 0;
	int monocle_tab_y_offset = config.monocle_tab_height > 0 ? cur_gapiv : 0;

	int tab_y = m->w.y + cur_gappov;
	int main_y = tab_y + monocle_tab_height + monocle_tab_y_offset;
	int main_height = m->w.height - 2 * cur_gappov - monocle_tab_inner_gap_height -
					  monocle_tab_height;

	int tab_area_width = m->w.width - 2 * cur_gappoh;

	int total_gaps = (n - 1) * cur_gapih;
	int base_width = (tab_area_width - total_gaps) / n;
	int remainder = (tab_area_width - total_gaps) % n;

	int tab_x = m->w.x + cur_gappoh;

	/* cap tab width so pills don't stretch across the whole screen; the
	 * capped row is centered in the tab area */
	if (config.monocle_tab_max_width > 0 &&
		base_width > config.monocle_tab_max_width) {
		base_width = config.monocle_tab_max_width;
		remainder = 0;
		tab_x += (tab_area_width - (n * base_width + total_gaps)) / 2;
	}
	int idx = 0;

	wl_list_for_each(c, &clients, link) {
		if (!VISIBLEON(c, m) || !ISFAKETILED(c))
			continue;

		if (c == fc) {
			monocle_set_focus(c, true);
		} else {
			monocle_set_focus(c, false);
		}

		geom.x = m->w.x + cur_gappoh;
		geom.y = main_y;
		geom.width = m->w.width - 2 * cur_gappoh;
		geom.height = main_height;
		client_tile_resize(c, geom, 0);

		int tw = base_width + (idx < remainder ? 1 : 0);
		global_draw_tab_bar(c, tab_x, tab_y, tw, monocle_tab_height);

		tab_x += tw + cur_gapih;
		idx++;
	}
}

// compute window size and position for the grid layout
void grid(Monitor *m) {
	int32_t i, n;
	int32_t cw, ch;
	int32_t cols, rows, overcols;
	Client *c = NULL;
	n = 0;
	int32_t target_gappo = enablegaps ? config.gappoh : 0;
	int32_t target_gappi = enablegaps ? config.gappih : 0;
	float single_width_ratio = 0.9;
	float single_height_ratio = 0.9;
	struct wlr_box target_geom;

	n = m->visible_fake_tiling_clients;

	if (n == 0)
		return;

	if (n == 1) {
		wl_list_for_each(c, &clients, link) {
			if (c->mon != m)
				continue;
			if (VISIBLEON(c, m) && !c->isunglobal &&
				((m->isoverview && !client_is_x11_popup(c)) ||
				 ISFAKETILED(c))) {
				cw = (m->w.width - 2 * target_gappo) * single_width_ratio;
				ch = (m->w.height - 2 * target_gappo) * single_height_ratio;
				target_geom.x = m->w.x + (m->w.width - cw) / 2;
				target_geom.y = m->w.y + (m->w.height - ch) / 2;
				target_geom.width = cw;
				target_geom.height = ch;
				client_tile_resize(c, target_geom, 0);
				return;
			}
		}
	}

	if (n == 2) {
		float col_pers[2] = {1.0f, 1.0f};
		// first extract the existing column ratios for these two windows
		i = 0;
		wl_list_for_each(c, &clients, link) {
			if (c->mon != m)
				continue;
			if (VISIBLEON(c, m) && !c->isunglobal &&
				((m->isoverview && !client_is_x11_popup(c)) ||
				 ISFAKETILED(c))) {
				if (i < 2)
					col_pers[i] =
						(c->grid_col_per > 0.0f) ? c->grid_col_per : 1.0f;
				i++;
			}
		}

		float sum_col = col_pers[0] + col_pers[1];
		float avail_w = m->w.width - 2 * target_gappo - target_gappi;
		ch =
			(m->w.height - 2 * target_gappo) * 0.65; // still keep the 0.65 aesthetic height

		i = 0;
		wl_list_for_each(c, &clients, link) {
			if (c->mon != m)
				continue;
			if (VISIBLEON(c, m) && !c->isunglobal &&
				((m->isoverview && !client_is_x11_popup(c)) ||
				 ISFAKETILED(c))) {
				c->grid_col_idx = i;
				c->grid_row_idx = 0;
				c->grid_col_per = col_pers[i];
				c->grid_row_per = 1.0f;

				// dynamically compute the current window's width based on its assigned weight
				cw = avail_w * (col_pers[i] / sum_col);

				if (i == 0) {
					target_geom.x = m->w.x + target_gappo;
				} else if (i == 1) {
					// the second window's X coordinate follows right after the first
					float cw0 = avail_w * (col_pers[0] / sum_col);
					target_geom.x = m->w.x + target_gappo + cw0 + target_gappi;
				}
				target_geom.y = m->w.y + (m->w.height - ch) / 2 + target_gappo;
				target_geom.width = cw;
				target_geom.height = ch;
				client_tile_resize(c, target_geom, 0);
				i++;
			}
		}
		return;
	}

	// compute the number of columns and rows
	for (cols = 0; cols <= n / 2; cols++) {
		if (cols * cols >= n)
			break;
	}
	rows = (cols && (cols - 1) * cols >= n) ? cols - 1 : cols;
	overcols = n % cols;

	float col_pers[cols];
	float row_pers[rows];
	for (i = 0; i < cols; i++)
		col_pers[i] = 1.0f;
	for (i = 0; i < rows; i++)
		row_pers[i] = 1.0f;

	// extract the first window's ratio
	i = 0;
	wl_list_for_each(c, &clients, link) {
		if (c->mon != m)
			continue;
		if (VISIBLEON(c, m) && !c->isunglobal &&
			((m->isoverview && !client_is_x11_popup(c)) || ISFAKETILED(c))) {
			int32_t c_idx = i % cols;
			int32_t r_idx = i / cols;
			if (r_idx == 0)
				col_pers[c_idx] =
					(c->grid_col_per > 0.0f) ? c->grid_col_per : 1.0f;
			if (c_idx == 0)
				row_pers[r_idx] =
					(c->grid_row_per > 0.0f) ? c->grid_row_per : 1.0f;
			i++;
		}
	}

	float sum_col = 0.0f, sum_row = 0.0f;
	for (i = 0; i < cols; i++)
		sum_col += col_pers[i];
	for (i = 0; i < rows; i++)
		sum_row += row_pers[i];

	float avail_w = m->w.width - 2 * target_gappo - (cols - 1) * target_gappi;
	float avail_h = m->w.height - 2 * target_gappo - (rows - 1) * target_gappi;

	// assign position and size
	i = 0;
	wl_list_for_each(c, &clients, link) {
		if (c->mon != m)
			continue;
		if (VISIBLEON(c, m) && !c->isunglobal &&
			((m->isoverview && !client_is_x11_popup(c)) || ISFAKETILED(c))) {
			int32_t c_idx = i % cols;
			int32_t r_idx = i / cols;

			// correct the properties and mark the indices
			c->grid_col_per = col_pers[c_idx];
			c->grid_row_per = row_pers[r_idx];
			c->grid_col_idx = c_idx;
			c->grid_row_idx = r_idx;

			// compute the X coordinate and width
			float fl_cx = m->w.x + target_gappo;
			float fl_cw = 0.0f;

			if (overcols && i >= n - overcols) {
				float over_w = 0.0f;
				for (int j = 0; j < overcols; j++)
					over_w += avail_w * (col_pers[j] / sum_col);
				over_w += (overcols - 1) * target_gappi;
				float dx = (m->w.width - over_w) / 2.0f - target_gappo;

				fl_cx += dx;
				for (int j = 0; j < c_idx; j++)
					fl_cx += avail_w * (col_pers[j] / sum_col) + target_gappi;
				fl_cw = avail_w * (col_pers[c_idx] / sum_col);
			} else {
				for (int j = 0; j < c_idx; j++)
					fl_cx += avail_w * (col_pers[j] / sum_col) + target_gappi;
				fl_cw = (c_idx == cols - 1)
							? (m->w.x + m->w.width - target_gappo - fl_cx)
							: avail_w * (col_pers[c_idx] / sum_col);
			}

			// compute the Y coordinate and height
			float fl_cy = m->w.y + target_gappo;
			for (int j = 0; j < r_idx; j++)
				fl_cy += avail_h * (row_pers[j] / sum_row) + target_gappi;
			float fl_ch = (r_idx == rows - 1)
							  ? (m->w.y + m->w.height - target_gappo - fl_cy)
							  : avail_h * (row_pers[r_idx] / sum_row);

			target_geom.x = (int32_t)fl_cx;
			target_geom.y = (int32_t)fl_cy;
			target_geom.width = (int32_t)fl_cw;
			target_geom.height = (int32_t)fl_ch;
			client_tile_resize(c, target_geom, 0);
			i++;
		}
	}
}

void fair(Monitor *m) {
	int32_t i, n = 0;
	Client *c = NULL;

	n = m->visible_fake_tiling_clients;
	if (n == 0)
		return;

	// get the gap configuration
	int32_t cur_gappiv = enablegaps ? m->gappiv : 0;
	int32_t cur_gappih = enablegaps ? m->gappih : 0;
	int32_t cur_gappov = enablegaps ? m->gappov : 0;
	int32_t cur_gappoh = enablegaps ? m->gappoh : 0;

	if (config.smartgaps && n == 1) {
		cur_gappiv = cur_gappih = cur_gappov = cur_gappoh = 0;
	}

	// compute the grid's row and column count
	int32_t cols;
	for (cols = 0; cols <= n; cols++) {
		if (cols * cols >= n)
			break;
	}

	int32_t base_rows = n / cols;
	int32_t remainder = n % cols;
	int32_t first_group_cols = cols - remainder;
	int32_t first_group_count = first_group_cols * base_rows;
	int32_t max_rows = base_rows + (remainder > 0 ? 1 : 0);

	// store the valid clients into an array
	Client *arr[n];
	int32_t arr_idx = 0;
	wl_list_for_each(c, &clients, link) {
		if (VISIBLEON(c, m) && ISFAKETILED(c)) {
			arr[arr_idx++] = c;
			if (arr_idx >= n)
				break; // safety bound
		}
	}

	// initialize the ratio arrays
	float col_pers[cols];
	float row_pers[max_rows];
	for (i = 0; i < cols; i++)
		col_pers[i] = 0.0f;
	for (i = 0; i < max_rows; i++)
		row_pers[i] = 0.0f;

	// lock in ratios with two passes directly over the array
	for (i = 0; i < n; i++) {
		c = arr[i];
		int32_t col_idx =
			(i < first_group_count)
				? (i / base_rows)
				: (first_group_cols + (i - first_group_count) / max_rows);
		int32_t row_idx = (i < first_group_count)
							  ? (i % base_rows)
							  : ((i - first_group_count) % max_rows);

		if (c->grid_col_idx == col_idx && c->grid_col_per > 0.0f)
			col_pers[col_idx] = c->grid_col_per;
		if (c->grid_row_idx == row_idx && c->grid_row_per > 0.0f)
			row_pers[row_idx] = c->grid_row_per;
	}
	for (i = 0; i < n; i++) {
		c = arr[i];
		int32_t col_idx =
			(i < first_group_count)
				? (i / base_rows)
				: (first_group_cols + (i - first_group_count) / max_rows);
		int32_t row_idx = (i < first_group_count)
							  ? (i % base_rows)
							  : ((i - first_group_count) % max_rows);

		if (col_pers[col_idx] == 0.0f && c->grid_col_per > 0.0f)
			col_pers[col_idx] = c->grid_col_per;
		if (row_pers[row_idx] == 0.0f && c->grid_row_per > 0.0f)
			row_pers[row_idx] = c->grid_row_per;
	}

	// fallback strategy and total weight computation
	float sum_col = 0.0f;
	for (i = 0; i < cols; i++) {
		if (col_pers[i] == 0.0f)
			col_pers[i] = 1.0f;
		sum_col += col_pers[i];
	}
	for (i = 0; i < max_rows; i++) {
		if (row_pers[i] == 0.0f)
			row_pers[i] = 1.0f;
	}

	// precompute the X coordinate and width for all columns
	float col_x[cols], col_w[cols];
	float avail_w = m->w.width - 2 * cur_gappoh - (cols - 1) * cur_gappih;
	float next_x = m->w.x + cur_gappoh;
	for (i = 0; i < cols; i++) {
		col_x[i] = next_x;
		col_w[i] = (i == cols - 1) ? (m->w.x + m->w.width - cur_gappoh - next_x)
								   : (avail_w * (col_pers[i] / sum_col));
		next_x += col_w[i] + cur_gappih;
	}

	// precompute two sets of row geometry (handles columns having different row counts)
	float row_y_base[base_rows], row_h_base[base_rows];
	float sum_row_base = 0.0f;
	for (i = 0; i < base_rows; i++)
		sum_row_base += row_pers[i];
	float avail_h_base =
		m->w.height - 2 * cur_gappov - (base_rows - 1) * cur_gappiv;
	float next_y = m->w.y + cur_gappov;
	for (i = 0; i < base_rows; i++) {
		row_y_base[i] = next_y;
		row_h_base[i] = (i == base_rows - 1)
							? (m->w.y + m->w.height - cur_gappov - next_y)
							: (avail_h_base * (row_pers[i] / sum_row_base));
		next_y += row_h_base[i] + cur_gappiv;
	}

	float row_y_max[max_rows], row_h_max[max_rows];
	if (remainder > 0) {
		float sum_row_max = 0.0f;
		for (i = 0; i < max_rows; i++)
			sum_row_max += row_pers[i];
		float avail_h_max =
			m->w.height - 2 * cur_gappov - (max_rows - 1) * cur_gappiv;
		next_y = m->w.y + cur_gappov;
		for (i = 0; i < max_rows; i++) {
			row_y_max[i] = next_y;
			row_h_max[i] = (i == max_rows - 1)
							   ? (m->w.y + m->w.height - cur_gappov - next_y)
							   : (avail_h_max * (row_pers[i] / sum_row_max));
			next_y += row_h_max[i] + cur_gappiv;
		}
	}

	// final layout rendering
	for (i = 0; i < n; i++) {
		c = arr[i];
		int32_t col_idx, row_idx;
		float fl_cx, fl_cy, fl_cw, fl_ch;

		if (i < first_group_count) {
			col_idx = i / base_rows;
			row_idx = i % base_rows;
			fl_cy = row_y_base[row_idx];
			fl_ch = row_h_base[row_idx];
		} else {
			int32_t offset = i - first_group_count;
			col_idx = first_group_cols + (offset / max_rows);
			row_idx = offset % max_rows;
			fl_cy = row_y_max[row_idx];
			fl_ch = row_h_max[row_idx];
		}

		c->grid_col_per = col_pers[col_idx];
		c->grid_row_per = row_pers[row_idx];
		c->grid_col_idx = col_idx;
		c->grid_row_idx = row_idx;

		fl_cx = col_x[col_idx];
		fl_cw = col_w[col_idx];

		client_tile_resize(c,
						   (struct wlr_box){.x = (int32_t)fl_cx,
											.y = (int32_t)fl_cy,
											.width = (int32_t)fl_cw,
											.height = (int32_t)fl_ch},
						   0);
	}
}