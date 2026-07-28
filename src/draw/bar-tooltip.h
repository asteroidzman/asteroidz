#ifndef ASTEROIDZ_BAR_TOOLTIP_H
#define ASTEROIDZ_BAR_TOOLTIP_H

/* Hover text for the bar's pills.
 *
 * The bar is deliberately terse: metric pills carry no number, the bell no
 * count, the title is capped, the now-playing string ellipsises. Every one of
 * those is the right call for something you see all day and read at a glance
 * -- and every one of them leaves a question the pill cannot answer. A tooltip
 * is where the answer goes, because it costs nothing until you ask for it.
 *
 * Built from the same pieces as a popover -- a rounded rect, a shadow, one
 * tab-bar node for the text -- so it inherits the theme and the per-output
 * scale, and needs no surface and no client.
 *
 * It is NOT a popover: there is no interaction, nothing to click, and it never
 * takes a grab. It appears after a delay on a pill the pointer has settled on,
 * and goes away the moment the pointer moves off, a button goes down, or a
 * popover opens. Anything the pointer can act on belongs in a popover instead.
 */

typedef struct {
	struct wlr_scene_tree *tree;
	struct wlr_scene_rect *bg;
	struct wlr_scene_shadow *shadow;
	struct asteroidz_tab_bar_node *label;
	AsteroidzNodeData *hit;
	Monitor *mon;
	/* the pill it belongs to, as an identity check only -- never dereferenced
	 * after the bar has been rebuilt, which is why it is cleared from
	 * bar_tooltip_hide() on every refresh */
	const BarPill *pill;
	struct wl_event_source *timer;
	/* where the pointer was when the timer was armed, so the tooltip appears
	 * under the thing you stopped on rather than wherever you have got to */
	double at_x, at_y;
	bool visible;
} BarTooltip;

static BarTooltip bar_tooltip;

static void bar_tooltip_hide(void) {
	if (bar_tooltip.timer)
		wl_event_source_timer_update(bar_tooltip.timer, 0);
	bar_tooltip.pill = NULL;
	if (!bar_tooltip.tree)
		return;
	/* the label and its hit tag are children, so this takes them with it */
	if (bar_tooltip.label) {
		asteroidz_tab_bar_node_destroy(bar_tooltip.label);
		bar_tooltip.label = NULL;
		bar_tooltip.hit = NULL;
	}
	wlr_scene_node_destroy(&bar_tooltip.tree->node);
	bar_tooltip.tree = NULL;
	bar_tooltip.bg = NULL;
	bar_tooltip.shadow = NULL;
	bar_tooltip.visible = false;
	bar_tooltip.mon = NULL;
}

/* ─── what a pill has to say ──────────────────────────────────────────────── */

static void bar_tooltip_bytes(char *out, size_t len, double bytes) {
	static const char *unit[] = {"B", "K", "M", "G", "T"};
	int u = 0;
	while (bytes >= 1024.0 && u < 4) {
		bytes /= 1024.0;
		u++;
	}
	snprintf(out, len, u == 0 ? "%.0f %s" : "%.1f %s", bytes, unit[u]);
}

/* The line to show for `p`, or false for "nothing worth saying".
 *
 * Returning false matters as much as the text does: a tooltip that repeats the
 * pill is worse than none, because it covers something to tell you what you
 * are already looking at. So a module that shows its whole state on the bar --
 * the layout chip, an unellipsised title -- gets no hover at all. */
static bool bar_pill_tooltip(const BarPill *p, char *out, size_t len) {
	if (!p || !p->module)
		return false;
	Monitor *m = p->module->mon;

	switch (p->module->kind) {
	case BAR_MODULE_CLOCK: {
		/* the long date, which no sane bar format has room for */
		time_t now = time(NULL);
		struct tm tm;
		localtime_r(&now, &tm);
		if (strftime(out, len, "%A, %e %B %Y", &tm) == 0)
			return false;
		return true;
	}
	case BAR_MODULE_CPU:
		snprintf(out, len, "CPU %d%%", bar_metrics.cpu_pct);
		return true;
	case BAR_MODULE_MEMORY: {
		snprintf(out, len, "Memory %d%%", bar_metrics.mem_pct);
		return true;
	}
	case BAR_MODULE_NETWORK: {
		char down[24], up[24];
		if (!bar_metrics.link_up) {
			snprintf(out, len, "%s", "network down");
			return true;
		}
		bar_tooltip_bytes(down, sizeof(down), bar_metrics.rx_rate);
		bar_tooltip_bytes(up, sizeof(up), bar_metrics.tx_rate);
		snprintf(out, len, "%s   ↓%s/s  ↑%s/s",
				 bar_metrics.iface[0] ? bar_metrics.iface : "network", down,
				 up);
		return true;
	}
	case BAR_MODULE_WEATHER: {
		if (!bar_weather.valid)
			return false;
		const char *desc = bar_wmo_text(bar_weather.code);
		if (!*desc)
			return false;
		snprintf(out, len, "%s, %d°C", desc, bar_weather.temp_c);
		return true;
	}
	case BAR_MODULE_NOTIFY: {
		/* The count is deliberately not on the bar -- this is where it went.
		 * The glyph answers "is anything waiting"; the number is the follow-up
		 * question, which is exactly what a hover is for. */
		if (bar_notify.inhibited)
			snprintf(out, len, "%s", "notifications inhibited");
		else if (bar_notify.dnd)
			snprintf(out, len, "do not disturb · %u unread",
					 bar_notify.count);
		else if (bar_notify.count == 0)
			snprintf(out, len, "%s", "no notifications");
		else
			snprintf(out, len, "%u unread notification%s", bar_notify.count,
					 bar_notify.count == 1 ? "" : "s");
		return true;
	}
	case BAR_MODULE_VOLUME:
		if (!bar_volume.have)
			return false;
		snprintf(out, len, "volume %d%%%s", bar_volume.pct,
				 bar_volume.muted ? " · muted" : "");
		return true;
	case BAR_MODULE_IDLE:
		snprintf(out, len, "%s", p->arg ? "staying awake"
										: "sleep allowed");
		return true;
	case BAR_MODULE_TITLE: {
		/* only when the pill could not show it all -- otherwise the hover
		 * would cover the title to tell you the title */
		Client *sel = m ? m->sel : NULL;
		const char *full = sel ? client_get_title(sel) : NULL;
		if (!full || !*full)
			return false;
		if (p->fixed_width <= 0 ||
			bar_template_width((BarPill *)p, full, bar_pill_height()) <=
				p->width)
			return false;
		snprintf(out, len, "%s", full);
		return true;
	}
	case BAR_MODULE_MEDIA: {
		if (!bar_media.have || !bar_media.title[0])
			return false;
		if (bar_media.artist[0])
			snprintf(out, len, "%s — %s", bar_media.title, bar_media.artist);
		else
			snprintf(out, len, "%s", bar_media.title);
		return true;
	}
	case BAR_MODULE_DISPLAY: {
		Monitor *f = m ? m : NULL;
		if (!f || !f->wlr_output)
			return false;
		char sum[BAR_TEXT_MAX];
		bar_display_summary(f, sum, sizeof(sum));
		snprintf(out, len, "%s%s", sum, f->hdr ? "  ·  HDR" : "");
		return true;
	}
	case BAR_MODULE_VPN:
		if (bar_vpn.state == BAR_VPN_CONNECTED)
			snprintf(out, len, "%s%s%s", bar_vpn.country,
					 bar_vpn.server[0] ? " · " : "", bar_vpn.server);
		else if (bar_vpn.state == BAR_VPN_UNAVAILABLE)
			snprintf(out, len, "%s", "nordvpn not available");
		else
			snprintf(out, len, "%s", "VPN disconnected");
		return true;
	case BAR_MODULE_DISCORD:
		if (bar_dv.error[0])
			snprintf(out, len, "%s", bar_dv.error);
		else
			snprintf(out, len, "%s%s%s", bar_dv_label(),
					 bar_dv.muted ? " · muted" : "",
					 bar_dv.ptt_active ? " · talking" : "");
		return true;
	case BAR_MODULE_MEDICATION: {
		if (!bar_med.have)
			return false;
		if (bar_med.ndue > 0) {
			/* name the doses actually waiting -- the pill only counts them */
			int32_t shown = 0;
			size_t o = 0;
			out[0] = '\0';
			for (int32_t i = 0; i < bar_med.ndoses && shown < 3; i++) {
				BarMedDose *d = &bar_med.doses[i];
				if (strcmp(d->status, "pending"))
					continue;
				o += (size_t)snprintf(out + o, o < len ? len - o : 0, "%s%s %s",
									  shown ? ", " : "", d->time, d->name);
				shown++;
			}
			return out[0] != '\0';
		}
		if (!bar_med.next_time[0])
			return false;
		snprintf(out, len, "next dose %s%s",
				 bar_med.next_is_today ? "at " : "on a later day, ",
				 bar_med.next_time);
		return true;
	}
	case BAR_MODULE_TRAY: {
		/* the SNI Title property, which is literally what the spec says a
		 * tray tooltip is for -- already fetched, never shown until now */
		if (p->arg >= (uint32_t)bar_tray_nitems)
			return false;
		BarTrayItem *it = &bar_tray_items[p->arg];
		if (!it->used || !it->title[0])
			return false;
		snprintf(out, len, "%s", it->title);
		return true;
	}
	case BAR_MODULE_CUSTOM: {
		/* Whatever the plugin put in its "tooltip" field, and nothing if it
		 * put nothing there -- a plugin that wants no hover simply omits it,
		 * which is the same rule the built-ins follow when their whole state
		 * already fits on the bar. */
		int32_t ci = p->module->custom;
		if (ci < 0 || ci >= config.bar_custom_count)
			return false;
		const char *t = bar_custom_state[ci].tooltip;
		if (!t[0])
			return false;
		snprintf(out, len, "%s", t);
		return true;
	}
	default:
		return false;
	}
}

/* ─── drawing ─────────────────────────────────────────────────────────────── */

static void bar_tooltip_show(void) {
	const BarPill *p = bar_tooltip.pill;
	Monitor *m = p && p->module ? p->module->mon : NULL;
	char text[BAR_TEXT_MAX];
	if (!m || !m->wlr_output || !bar_pill_tooltip(p, text, sizeof(text)) ||
		!text[0]) {
		bar_tooltip_hide();
		return;
	}

	float scale = m->wlr_output->scale > 0 ? m->wlr_output->scale : 1.0f;
	int32_t pad = config.bar_popover_padding;
	int32_t h = config.bar_popover_row_height;

	if (!bar_tooltip.tree) {
		bar_tooltip.tree = wlr_scene_tree_create(layers[LyrOverlay]);
		if (!bar_tooltip.tree)
			return;
	}
	if (!bar_tooltip.label) {
		bar_tooltip.hit = ecalloc(1, sizeof(AsteroidzNodeData));
		/* a type nothing dispatches on: the tooltip is not a target, and the
		 * pointer is over the BAR whenever one is up, so it never sits under
		 * the cursor anyway */
		bar_tooltip.hit->type = ASTEROIDZ_BAR_NODE;
		bar_tooltip.hit->node_data = NULL;
		bar_tooltip.label = asteroidz_tab_bar_node_create(
			bar_tooltip.hit, bar_tooltip.tree, config.theme, 0, 0);
		if (!bar_tooltip.label) {
			free(bar_tooltip.hit);
			bar_tooltip.hit = NULL;
			return;
		}
	}

	asteroidz_tab_bar_node_apply_config(bar_tooltip.label, &config.theme);
	asteroidz_tab_bar_node_set_padding(bar_tooltip.label, pad,
									   config.theme.padding_y);
	asteroidz_tab_bar_node_set_text_align_left(bar_tooltip.label, true);
	/* text only: the panel behind it is the tooltip's own rounded rect, and a
	 * pill's resting background drawn on top of that is a second, differently
	 * coloured box inside the first */
	{
		static const float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
		asteroidz_tab_bar_node_set_focus(bar_tooltip.label, false);
		asteroidz_tab_bar_node_set_colors(bar_tooltip.label,
										  config.theme.fg_color, clear);
	}
	int32_t w = asteroidz_tab_bar_node_measure_width(bar_tooltip.label, text, h);
	int32_t max_w = m->m.width / 2;
	if (w > max_w)
		w = max_w;

	/* Under the bar and centred on the pointer, clamped to the output -- the
	 * same shape a popover takes, for the same reason: a pill near the edge
	 * would otherwise hang its tooltip off the screen. */
	int32_t x = (int32_t)bar_tooltip.at_x - w / 2;
	int32_t min_x = m->m.x + config.bar_margin_x;
	int32_t max_x = m->m.x + m->m.width - config.bar_margin_x - w;
	if (x > max_x)
		x = max_x;
	if (x < min_x)
		x = min_x;
	int32_t y = config.bar_position_bottom
					? m->m.y + m->m.height - config.bar_margin_y -
						  config.bar_height - config.bar_popover_gap - h
					: m->m.y + config.bar_margin_y + config.bar_height +
						  config.bar_popover_gap;

	int32_t radius = config.bar_panel_radius;
	if (config.bar_panel_shadow && config.shadows) {
		int32_t delta = config.bar_panel_shadow_size;
		if (!bar_tooltip.shadow)
			bar_tooltip.shadow = wlr_scene_shadow_create(
				bar_tooltip.tree, w + 2 * delta, h + 2 * delta, radius + delta,
				config.bar_panel_shadow_blur, config.bar_panel_shadow_color);
		if (bar_tooltip.shadow) {
			wlr_scene_shadow_set_size(bar_tooltip.shadow, w + 2 * delta,
									  h + 2 * delta);
			wlr_scene_node_set_position(&bar_tooltip.shadow->node, x - delta,
										y - delta + delta / 3);
			wlr_scene_node_set_enabled(&bar_tooltip.shadow->node, true);
		}
	}
	if (!bar_tooltip.bg)
		bar_tooltip.bg = wlr_scene_rect_create(bar_tooltip.tree, w, h,
											   config.bar_popover_color);
	if (bar_tooltip.bg) {
		wlr_scene_rect_set_size(bar_tooltip.bg, w, h);
		wlr_scene_rect_set_color(bar_tooltip.bg, config.bar_popover_color);
		wlr_scene_rect_set_corner_radius(bar_tooltip.bg, radius);
		wlr_scene_node_set_position(&bar_tooltip.bg->node, x, y);
		wlr_scene_node_set_enabled(&bar_tooltip.bg->node, true);
	}

	asteroidz_tab_bar_node_set_size(bar_tooltip.label, w, h);
	asteroidz_tab_bar_node_set_position(bar_tooltip.label, x, y);
	asteroidz_tab_bar_node_set_enabled(bar_tooltip.label, true);
	asteroidz_tab_bar_node_update(bar_tooltip.label, text, scale);

	/* Label on top, backdrop under it.
	 *
	 * The rect and the shadow are created lazily on this pass -- AFTER the
	 * label -- so on one shared tree they land last and paint straight over
	 * the text. That is not a hypothetical: it rendered the tooltip as an
	 * empty dark box, the text visible only as a stain through a 95%-opaque
	 * fill. The popover splits panel_tree from row_tree for exactly this
	 * reason; one node is few enough to just restack. */
	if (bar_tooltip.label->scene_buffer)
		wlr_scene_node_raise_to_top(&bar_tooltip.label->scene_buffer->node);
	if (bar_tooltip.shadow)
		wlr_scene_node_lower_to_bottom(&bar_tooltip.shadow->node);
	wlr_scene_node_raise_to_top(&bar_tooltip.tree->node);

	bar_tooltip.mon = m;
	bar_tooltip.visible = true;
}

static int bar_tooltip_tick(void *data) {
	(void)data;
	bar_tooltip_show();
	return 0;
}

/* ─── hover routing ───────────────────────────────────────────────────────── */

/* Called from motionnotify for every pointer move. Cheap when the pointer is
 * nowhere near the bar, which is almost always: the pill lookup only happens
 * once the y coordinate is inside a bar strip. */
static void bar_tooltip_motion(double cx, double cy) {
	if (!config.bar_enable || !config.bar_tooltip_enable) {
		if (bar_tooltip.visible || bar_tooltip.pill)
			bar_tooltip_hide();
		return;
	}

	const BarPill *hovered = NULL;
	struct wlr_scene_node *node =
		wlr_scene_node_at(&layers[LyrBottom]->node, cx, cy, NULL, NULL);
	if (!node)
		node = wlr_scene_node_at(&layers[LyrTop]->node, cx, cy, NULL, NULL);
	if (node && node->data) {
		AsteroidzNodeData *d = (AsteroidzNodeData *)node->data;
		if (d->type == ASTEROIDZ_BAR_NODE && d->node_data)
			hovered = (const BarPill *)d->node_data;
	}

	if (!hovered) {
		if (bar_tooltip.visible || bar_tooltip.pill)
			bar_tooltip_hide();
		return;
	}
	if (hovered == bar_tooltip.pill) {
		/* still on the same pill: leave a shown tooltip alone rather than
		 * re-arming, or it would never settle while the pointer drifts */
		return;
	}

	/* moved onto a different pill: drop what is up and start the wait again */
	bool was_visible = bar_tooltip.visible;
	bar_tooltip_hide();
	bar_tooltip.pill = hovered;
	bar_tooltip.at_x = cx;
	bar_tooltip.at_y = cy;

	if (!bar_tooltip.timer)
		bar_tooltip.timer =
			wl_event_loop_add_timer(event_loop, bar_tooltip_tick, NULL);
	if (bar_tooltip.timer) {
		/* Moving along a row of pills with one already up should not make you
		 * wait again for each -- that is the behaviour every toolkit has, and
		 * it is why hovering a toolbar feels responsive rather than sticky. */
		int32_t delay = was_visible ? 60 : config.bar_tooltip_delay;
		wl_event_source_timer_update(bar_tooltip.timer, delay);
	}
}

#endif /* ASTEROIDZ_BAR_TOOLTIP_H */
