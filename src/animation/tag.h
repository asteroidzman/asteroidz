void set_tagin_animation(Monitor *m, Client *c) {
	if (c->animation.running) {
		c->animainit_geom.x = c->animation.current.x;
		c->animainit_geom.y = c->animation.current.y;
		return;
	}

	if ((c->isglobal || c->isunglobal || c->ispinned) ||
		(c->tags & (1 << (m->pertag->prevtag - 1)) &&
		 c->tags & (1 << (m->pertag->curtag - 1)))) {
		c->animation.tagouting = false;
		c->animation.tagouted = false;
		c->animation.tagining = false;
		c->animation.action = MOVE;
		return;
	}

	bool going_forward = m->carousel_anim_dir
							 ? m->carousel_anim_dir > 0
							 : m->pertag->curtag > m->pertag->prevtag;

	if (going_forward) {

		c->animainit_geom.x = config.tag_animation_direction == VERTICAL
								  ? c->animation.current.x
								  : ASTEROIDZ_MAX(c->mon->m.x + c->mon->m.width,
											  c->geom.x + c->mon->m.width);
		c->animainit_geom.y = config.tag_animation_direction == VERTICAL
								  ? ASTEROIDZ_MAX(c->mon->m.y + c->mon->m.height,
											  c->geom.y + c->mon->m.height)
								  : c->animation.current.y;

	} else {

		c->animainit_geom.x = config.tag_animation_direction == VERTICAL
								  ? c->animation.current.x
								  : ASTEROIDZ_MIN(m->m.x - c->geom.width,
											  c->geom.x - c->mon->m.width);
		c->animainit_geom.y = config.tag_animation_direction == VERTICAL
								  ? ASTEROIDZ_MIN(m->m.y - c->geom.height,
											  c->geom.y - c->mon->m.height)
								  : c->animation.current.y;
	}
}

/* Named special workspace equivalent of set_tagin_animation(): a special
 * workspace always slides in from the same fixed edge (no prevtag/curtag
 * bookkeeping applies, since it overlays whatever tag is selected rather
 * than replacing it). Pinned/global/unglobal clients are exempted exactly
 * like the tag-switch animation exempts them, since they are always
 * visible and should just settle in place instead of sliding. */
void set_special_in_animation(Monitor *m, Client *c) {
	if (c->animation.running) {
		c->animainit_geom.x = c->animation.current.x;
		c->animainit_geom.y = c->animation.current.y;
		return;
	}

	if (c->isglobal || c->isunglobal || c->ispinned) {
		c->animation.tagouting = false;
		c->animation.tagouted = false;
		c->animation.tagining = false;
		c->animation.action = MOVE;
		return;
	}

	c->animainit_geom.x =
		config.tag_animation_direction == VERTICAL
			? c->animation.current.x
			: ASTEROIDZ_MAX(c->mon->m.x + c->mon->m.width, c->geom.x + c->mon->m.width);
	c->animainit_geom.y =
		config.tag_animation_direction == VERTICAL
			? ASTEROIDZ_MAX(c->mon->m.y + c->mon->m.height, c->geom.y + c->mon->m.height)
			: c->animation.current.y;
}

void set_arrange_visible(Monitor *m, Client *c, bool want_animation) {

	/* Overview: overview_tags/overview_arrange_main fully own window geometry
	 * AND visibility (mirror cells, exposé grid, strip hiding). Resizing to
	 * desktop geometry or starting tag in/out animations here would fight the
	 * cell layout every arrange -- and a set tagouting/tagining flag makes
	 * buffer_set_effect skip the OV thumbnail dest-size scaling, rendering the
	 * window UNSCALED over its neighbours (the overlapping-titlebars bug when
	 * previewing a non-selected tag). Just clear the flags and stand down. */
	if (m->isoverview) {
		c->animation.tagining = false;
		c->animation.tagouting = false;
		c->animation.tagouted = false;
		return;
	}

	if (!ISTILED(c) || ((!c->is_clip_to_hide || !is_scroller_layout(c->mon)) &&
						(!c->is_monocle_hide || !is_monocle_layout(c->mon)))) {
		c->is_clip_to_hide = false;
		c->is_monocle_hide = false;
		wlr_scene_node_set_enabled(&c->scene->node, true);
		wlr_scene_node_set_enabled(&c->scene_surface->node, true);
	}

	if (m->special_transitioning && want_animation && config.animations) {
		c->animation.tagining = true;
		set_special_in_animation(m, c);
	} else if (!c->animation.tag_from_rule && want_animation &&
			   m->pertag->prevtag != 0 && m->pertag->curtag != 0 &&
			   config.animations) {
		c->animation.tagining = true;
		set_tagin_animation(m, c);
	} else {
		c->animainit_geom.x = c->animation.current.x;
		c->animainit_geom.y = c->animation.current.y;
	}

	c->animation.tag_from_rule = false;
	c->animation.tagouting = false;
	c->animation.tagouted = false;
	resize(c, c->geom, 0);
}

void set_tagout_animation(Monitor *m, Client *c) {

	if ((c->isglobal || c->isunglobal || c->ispinned) ||
		(c->tags & (1 << (m->pertag->prevtag - 1)) &&
		 c->tags & (1 << (m->pertag->curtag - 1)))) {
		c->animation.tagouting = false;
		c->animation.tagouted = false;
		c->animation.tagining = false;
		c->animation.action = MOVE;
		return;
	}

	bool going_forward = m->carousel_anim_dir
							 ? m->carousel_anim_dir > 0
							 : m->pertag->curtag > m->pertag->prevtag;
	if (going_forward) {
		c->pending = c->geom;
		c->pending.x = config.tag_animation_direction == VERTICAL
						   ? c->animation.current.x
						   : ASTEROIDZ_MIN(c->mon->m.x - c->geom.width,
									   c->geom.x - c->mon->m.width);
		c->pending.y = config.tag_animation_direction == VERTICAL
						   ? ASTEROIDZ_MIN(c->mon->m.y - c->geom.height,
									   c->geom.y - c->mon->m.height)
						   : c->animation.current.y;

		resize(c, c->geom, 0);
	} else {
		c->pending = c->geom;
		c->pending.x = config.tag_animation_direction == VERTICAL
						   ? c->animation.current.x
						   : ASTEROIDZ_MAX(c->mon->m.x + c->mon->m.width,
									   c->geom.x + c->mon->m.width);
		c->pending.y = config.tag_animation_direction == VERTICAL
						   ? ASTEROIDZ_MAX(c->mon->m.y + c->mon->m.height,
									   c->geom.y + c->mon->m.height)
						   : c->animation.current.y;
		resize(c, c->geom, 0);
	}
}

/* Named special workspace equivalent of set_tagout_animation(): exits to
 * the same fixed edge that set_special_in_animation() enters from, so a
 * special workspace feels like a drawer that opens and closes from one
 * side. Used both when a special workspace is closed (its own clients
 * animate out) and when one is opened or switched (the normal-tag clients
 * underneath, or the previously active special workspace, animate out). */
void set_special_out_animation(Monitor *m, Client *c) {
	if (c->isglobal || c->isunglobal || c->ispinned) {
		c->animation.tagouting = false;
		c->animation.tagouted = false;
		c->animation.tagining = false;
		c->animation.action = MOVE;
		return;
	}

	c->pending = c->geom;
	c->pending.x =
		config.tag_animation_direction == VERTICAL
			? c->animation.current.x
			: ASTEROIDZ_MAX(c->mon->m.x + c->mon->m.width, c->geom.x + c->mon->m.width);
	c->pending.y =
		config.tag_animation_direction == VERTICAL
			? ASTEROIDZ_MAX(c->mon->m.y + c->mon->m.height, c->geom.y + c->mon->m.height)
			: c->animation.current.y;
	resize(c, c->geom, 0);
}

void set_arrange_hidden(Monitor *m, Client *c, bool want_animation) {

	/* Overview: see set_arrange_visible -- a "hidden" (non-selected-tag) window
	 * may be SHOWN in the main area as the previewed tag, so neither disable
	 * its scene nor start a desktop-geometry tag-out slide from here. The
	 * overview code (arrange_main / the strip loop) owns show/hide. */
	if (m->isoverview) {
		c->animation.tagining = false;
		c->animation.tagouting = false;
		c->animation.tagouted = false;
		return;
	}

	if (m->special_transitioning && c->scene->node.enabled && want_animation &&
		config.animations) {
		c->animation.tagouting = true;
		c->animation.tagining = false;
		set_special_out_animation(m, c);
	} else if ((c->tags & (1 << (m->pertag->prevtag - 1))) &&
			   m->pertag->prevtag != 0 && m->pertag->curtag != 0 &&
			   config.animations) {
		c->animation.tagouting = true;
		c->animation.tagining = false;
		set_tagout_animation(m, c);
	} else {
		c->animation.running = false;
		wlr_scene_node_set_enabled(&c->scene->node, false);
		c->animainit_geom = c->current = c->pending = c->animation.current =
			c->geom;
	}
}
