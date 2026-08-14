/*
 * Damped spring step response, with an optional initial velocity.
 * zeta >= 1 is critically damped (no overshoot), lower values bounce.
 *
 * `v0` is dy/dt at t = 0, in the same normalised time as t. Zero gives the
 * ordinary step response a fresh animation wants -- the window is at rest and
 * starts moving. Non-zero is what makes a RETARGET continuous in velocity
 * (M6A/ADR-608): a target arriving mid-flight starts a new spring that is
 * already moving at the speed the old one had reached, instead of stopping
 * dead and setting off again.
 *
 * Both closed forms below reduce EXACTLY to the previous ones at v0 == 0 --
 * that is the property to preserve when touching this, because every existing
 * animation on the desktop is the v0 == 0 case.
 *
 *   critically damped   y = 1 - e^-wt (1 + (w - v0) t)
 *   underdamped         y = 1 - e^-zwt (cos wd t + C sin wd t),
 *                       C = (zw - v0) / wd
 */
static struct dvec2 calculate_spring_curve_at_v(double t, double v0) {
	struct dvec2 point = {.x = t, .y = 1.0};
	double zeta = config.spring_damping;
	double omega = config.spring_frequency;

	if (t >= 1.0)
		return point;
	if (zeta >= 1.0) {
		point.y = 1.0 - exp(-omega * t) * (1.0 + (omega - v0) * t);
	} else {
		double wd = omega * sqrt(1.0 - zeta * zeta);
		point.y = 1.0 - exp(-zeta * omega * t) *
			(cos(wd * t) + ((zeta * omega - v0) / wd) * sin(wd * t));
	}
	return point;
}

/*
 * dy/dt of the SAME curve at v0 == 0, which is what a retarget must sample to
 * know how fast the outgoing motion was going.
 *
 * Analytic rather than a finite difference of stored integers: the geometry it
 * would difference is rounded to whole pixels, so at a 144Hz sample spacing the
 * quotient is dominated by the rounding rather than the motion.
 */
static double spring_curve_velocity_at(double t) {
	double zeta = config.spring_damping;
	double omega = config.spring_frequency;
	if (t >= 1.0 || t < 0.0)
		return 0.0;
	if (zeta >= 1.0)
		return omega * omega * t * exp(-omega * t);
	double wd = omega * sqrt(1.0 - zeta * zeta);
	return (omega * omega / wd) * exp(-zeta * omega * t) * sin(wd * t);
}

static struct dvec2 calculate_spring_curve_at(double t, int32_t type) {
	(void)type;
	return calculate_spring_curve_at_v(t, 0.0);
}

struct dvec2 calculate_animation_curve_at(double t, int32_t type) {
	struct dvec2 point;
	double *animation_curve;

	/* springs animate geometry; opacity fades stay on bezier so they
	 * never overshoot out of [0,1]. CLOSE is also excluded: springs model
	 * arrival at a target, but a closing window is being destroyed — its
	 * shrink should decay monotonically alongside the opacity fade, never
	 * bounce back toward the viewer */
	if (config.animation_curve_spring &&
		(type == MOVE || type == OPEN || type == TAG)) {
		return calculate_spring_curve_at(t, type);
	}
	if (type == MOVE) {
		animation_curve = config.animation_curve_move;
	} else if (type == OPEN) {
		animation_curve = config.animation_curve_open;
	} else if (type == TAG) {
		animation_curve = config.animation_curve_tag;
	} else if (type == CLOSE) {
		animation_curve = config.animation_curve_close;
	} else if (type == FOCUS) {
		animation_curve = config.animation_curve_focus;
	} else if (type == OPAFADEIN) {
		animation_curve = config.animation_curve_opafadein;
	} else if (type == OPAFADEOUT) {
		animation_curve = config.animation_curve_opafadeout;
	} else {
		animation_curve = config.animation_curve_move;
	}

	point.x = 3 * t * (1 - t) * (1 - t) * animation_curve[0] +
			  3 * t * t * (1 - t) * animation_curve[2] + t * t * t;

	point.y = 3 * t * (1 - t) * (1 - t) * animation_curve[1] +
			  3 * t * t * (1 - t) * animation_curve[3] + t * t * t;

	return point;
}

void handle_snapshot_meta_destroy(struct wl_listener *listener, void *data) {
	SnapshotMetadata *meta = wl_container_of(listener, meta, destroy);
	wl_list_remove(&meta->destroy.link); // Safely remove the listener
	free(meta);
}

void init_baked_points(void) {
	baked_points_move = calloc(BAKED_POINTS_COUNT, sizeof(*baked_points_move));
	baked_points_open = calloc(BAKED_POINTS_COUNT, sizeof(*baked_points_open));
	baked_points_tag = calloc(BAKED_POINTS_COUNT, sizeof(*baked_points_tag));
	baked_points_close =
		calloc(BAKED_POINTS_COUNT, sizeof(*baked_points_close));
	baked_points_focus =
		calloc(BAKED_POINTS_COUNT, sizeof(*baked_points_focus));
	baked_points_opafadein =
		calloc(BAKED_POINTS_COUNT, sizeof(*baked_points_opafadein));
	baked_points_opafadeout =
		calloc(BAKED_POINTS_COUNT, sizeof(*baked_points_opafadeout));

	for (int32_t i = 0; i < BAKED_POINTS_COUNT; i++) {
		baked_points_move[i] = calculate_animation_curve_at(
			(double)i / (BAKED_POINTS_COUNT - 1), MOVE);
	}
	for (int32_t i = 0; i < BAKED_POINTS_COUNT; i++) {
		baked_points_open[i] = calculate_animation_curve_at(
			(double)i / (BAKED_POINTS_COUNT - 1), OPEN);
	}
	for (int32_t i = 0; i < BAKED_POINTS_COUNT; i++) {
		baked_points_tag[i] = calculate_animation_curve_at(
			(double)i / (BAKED_POINTS_COUNT - 1), TAG);
	}
	for (int32_t i = 0; i < BAKED_POINTS_COUNT; i++) {
		baked_points_close[i] = calculate_animation_curve_at(
			(double)i / (BAKED_POINTS_COUNT - 1), CLOSE);
	}
	for (int32_t i = 0; i < BAKED_POINTS_COUNT; i++) {
		baked_points_focus[i] = calculate_animation_curve_at(
			(double)i / (BAKED_POINTS_COUNT - 1), FOCUS);
	}
	for (int32_t i = 0; i < BAKED_POINTS_COUNT; i++) {
		baked_points_opafadein[i] = calculate_animation_curve_at(
			(double)i / (BAKED_POINTS_COUNT - 1), OPAFADEIN);
	}
	for (int32_t i = 0; i < BAKED_POINTS_COUNT; i++) {
		baked_points_opafadeout[i] = calculate_animation_curve_at(
			(double)i / (BAKED_POINTS_COUNT - 1), OPAFADEOUT);
	}
}

double find_animation_curve_at(double t, int32_t type) {
	int32_t down = 0;
	int32_t up = BAKED_POINTS_COUNT - 1;

	int32_t middle = (up + down) / 2;
	struct dvec2 *baked_points;
	if (type == MOVE) {
		baked_points = baked_points_move;
	} else if (type == OPEN) {
		baked_points = baked_points_open;
	} else if (type == TAG) {
		baked_points = baked_points_tag;
	} else if (type == CLOSE) {
		baked_points = baked_points_close;
	} else if (type == FOCUS) {
		baked_points = baked_points_focus;
	} else if (type == OPAFADEIN) {
		baked_points = baked_points_opafadein;
	} else if (type == OPAFADEOUT) {
		baked_points = baked_points_opafadeout;
	} else {
		baked_points = baked_points_move;
	}

	while (up - down != 1) {
		if (baked_points[middle].x <= t) {
			down = middle;
		} else {
			up = middle;
		}
		middle = (up + down) / 2;
	}

	/* interpolate between the two bracketing samples instead of snapping to
	 * the upper one: 256 samples is coarse relative to how many real frames
	 * a slow fade spans, and nearest-sample lookup was visibly steppy */
	double x0 = baked_points[down].x, x1 = baked_points[up].x;
	double y0 = baked_points[down].y, y1 = baked_points[up].y;
	double span = x1 - x0;
	double frac = span > 0.0 ? (t - x0) / span : 0.0;
	frac = ASTEROIDZ_MAX(0.0, ASTEROIDZ_MIN(1.0, frac));
	return y0 + (y1 - y0) * frac;
}

static bool scene_node_snapshot(struct wlr_scene_node *node, int32_t lx,
								int32_t ly,
								struct wlr_scene_tree *snapshot_tree) {
	if (!node->enabled && node->type != WLR_SCENE_NODE_TREE) {
		return true;
	}

	lx += node->x;
	ly += node->y;

	struct wlr_scene_node *snapshot_node = NULL;
	switch (node->type) {
	case WLR_SCENE_NODE_BLUR:
		/* blur nodes are not copied into snapshots */
		break;
	case WLR_SCENE_NODE_TREE: {
		struct wlr_scene_tree *scene_tree = wlr_scene_tree_from_node(node);

		struct wlr_scene_node *child;
		wl_list_for_each(child, &scene_tree->children, link) {
			scene_node_snapshot(child, lx, ly, snapshot_tree);
		}
		break;
	}
	case WLR_SCENE_NODE_RECT: {
		// struct wlr_scene_rect *scene_rect = wlr_scene_rect_from_node(node);

		// struct wlr_scene_rect *snapshot_rect =
		// 	wlr_scene_rect_create(snapshot_tree, scene_rect->width,
		// 						  scene_rect->height, scene_rect->color);
		// snapshot_rect->node.data = scene_rect->node.data;
		// if (snapshot_rect == NULL) {
		// 	return false;
		// }

		// wlr_scene_rect_set_clipped_region(scene_rect,
		// 								  snapshot_rect->clipped_region);
		// wlr_scene_rect_set_backdrop_blur(scene_rect, false);
		// wlr_scene_rect_set_backdrop_blur_optimized(
		// 	scene_rect, snapshot_rect->backdrop_blur_optimized);
		// wlr_scene_rect_set_corner_radius(
		// 	scene_rect, snapshot_rect->corner_radius, snapshot_rect->corners);
		// wlr_scene_rect_set_color(scene_rect, snapshot_rect->color);

		// snapshot_node = &snapshot_rect->node;
		break;
	}
	case WLR_SCENE_NODE_BUFFER: {
		struct wlr_scene_buffer *scene_buffer =
			wlr_scene_buffer_from_node(node);

		//  Create an intermediate wrapper tree node
		struct wlr_scene_tree *wrapper = wlr_scene_tree_create(snapshot_tree);
		if (wrapper == NULL) {
			return false;
		}
		snapshot_node = &wrapper->node; // Position offset is applied on the outer wrapper box

		// Collect the surface state and save it as metadata
		SnapshotMetadata *meta = calloc(1, sizeof(SnapshotMetadata));
		if (meta == NULL) {
			wlr_scene_node_destroy(&wrapper->node);
			return false;
		}
		meta->orig_width = scene_buffer->dst_width;
		meta->orig_height = scene_buffer->dst_height;
		meta->type = Snapshot;

		struct wlr_scene_surface *scene_surface =
			wlr_scene_surface_try_from_buffer(scene_buffer);
		if (scene_surface != NULL) {
			meta->is_subsurface =
				!!wlr_subsurface_try_from_wlr_surface(scene_surface->surface);
		}

		// Hook up the destroy callback listener, freeing the memory when the
		// wrapper node is destroyed
		meta->destroy.notify = handle_snapshot_meta_destroy;
		wl_signal_add(&wrapper->node.events.destroy, &meta->destroy);
		wrapper->node.data = meta;

		// Attach the real buffer under the wrapper (at relative coordinates 0,0)
		struct wlr_scene_buffer *snapshot_buffer =
			wlr_scene_buffer_create(wrapper, NULL);
		if (snapshot_buffer == NULL) {
			wlr_scene_node_destroy(&wrapper->node);
			return false;
		}

		// Preserve the original data pointer (e.g. Client*), so event dispatch
		// and focus lookup still work
		snapshot_buffer->node.data = scene_buffer->node.data;

		wlr_scene_buffer_set_dest_size(snapshot_buffer, scene_buffer->dst_width,
									   scene_buffer->dst_height);
		wlr_scene_buffer_set_opaque_region(snapshot_buffer,
										   &scene_buffer->opaque_region);
		wlr_scene_buffer_set_source_box(snapshot_buffer,
										&scene_buffer->src_box);
		wlr_scene_buffer_set_transform(snapshot_buffer,
									   scene_buffer->transform);
		wlr_scene_buffer_set_filter_mode(snapshot_buffer,
										 scene_buffer->filter_mode);

		// Effects
		wlr_scene_buffer_set_opacity(snapshot_buffer, scene_buffer->opacity);
		wlr_scene_buffer_set_corner_radii(snapshot_buffer,
										  scene_buffer->corners);

		if (scene_surface != NULL &&
				az_surface_buffer(scene_surface->surface) != NULL) {
			wlr_scene_buffer_set_buffer(snapshot_buffer,
										az_surface_buffer(scene_surface->surface));
		} else {
			wlr_scene_buffer_set_buffer(snapshot_buffer, scene_buffer->buffer);
		}
		break;
	}
	case WLR_SCENE_NODE_SHADOW: {
		struct wlr_scene_shadow *scene_shadow =
			wlr_scene_shadow_from_node(node);

		struct wlr_scene_shadow *snapshot_shadow = wlr_scene_shadow_create(
			snapshot_tree, scene_shadow->width, scene_shadow->height,
			scene_shadow->corner_radius, scene_shadow->blur_sigma,
			scene_shadow->color);
		if (snapshot_shadow == NULL) {
			return false;
		}
		snapshot_node = &snapshot_shadow->node;

		wlr_scene_shadow_set_clipped_region(snapshot_shadow,
											scene_shadow->clipped_region);
		wlr_scene_shadow_set_corner_radii(snapshot_shadow,
										  scene_shadow->corners);

		snapshot_shadow->node.data = scene_shadow->node.data;

		wlr_scene_node_set_enabled(&snapshot_shadow->node, false);

		break;
	}
	case WLR_SCENE_NODE_OPTIMIZED_BLUR:
		return true;
	}

	if (snapshot_node != NULL) {
		wlr_scene_node_set_position(snapshot_node, lx, ly);
	}

	return true;
}

struct wlr_scene_tree *wlr_scene_tree_snapshot(struct wlr_scene_node *node,
											   struct wlr_scene_tree *parent) {
	struct wlr_scene_tree *snapshot = wlr_scene_tree_create(parent);
	if (snapshot == NULL) {
		return NULL;
	}

	// Disable and enable the snapshot tree like so to atomically update
	// the scene-graph. This will prevent over-damaging or other weirdness.
	wlr_scene_node_set_enabled(&snapshot->node, false);

	if (!scene_node_snapshot(node, 0, 0, snapshot)) {
		wlr_scene_node_destroy(&snapshot->node);
		return NULL;
	}

	wlr_scene_node_set_enabled(&snapshot->node, true);

	return snapshot;
}

void request_fresh_all_monitors(void) {
	Monitor *m = NULL;
	wl_list_for_each(m, &mons, link) {
		if (!m->wlr_output->enabled) {
			continue;
		}
		wlr_output_schedule_frame(m->wlr_output);
	}
}

/*
 * ── M4G: WAKE THE OUTPUTS THE MOTION CAN REACH, AND NO OTHERS ─────────────
 *
 * An animation used to ask EVERY output for a fresh frame on every tick, so a
 * window sliding across DP-1 woke HDMI-A-1 sixty times a second to build,
 * commit and page-flip a frame containing no damage at all. Measured on a
 * two-output fixture: 43-100% of the committed frames during an animation
 * carried zero damage, and on the output with no animating client it was
 * 94-100%.
 *
 * THE AFFECTED SET IS GEOMETRIC, NOT c->mon. A client's monitor is where the
 * compositor files it, not where its pixels are: a scroller column can extend
 * past its own monitor's edge, a move can cross a seam, and a blurred window
 * reaches `halo` pixels beyond wherever it ends. So the test is whether an
 * output's own area intersects the box the motion can touch -- which is the
 * union of where the window IS and where it is GOING, because a frame
 * scheduled now will be rendered from a sample somewhere between the two.
 *
 * Conservative in the direction that matters: a box that is too big wakes an
 * output that had nothing to do, which is what this replaces; a box that is
 * too small drops a frame the user can see. So the union is padded, and any
 * caller that cannot describe its own extent asks for all of them.
 */
void request_fresh_for_box(const struct wlr_box *box, int32_t pad) {
	if (box == NULL || box->width <= 0 || box->height <= 0) {
		request_fresh_all_monitors();
		return;
	}
	struct wlr_box reach = {
		.x = box->x - pad,
		.y = box->y - pad,
		.width = box->width + 2 * pad,
		.height = box->height + 2 * pad,
	};
	Monitor *m = NULL;
	wl_list_for_each(m, &mons, link) {
		if (!m->wlr_output->enabled) {
			continue;
		}
		struct wlr_box inter;
		if (wlr_box_intersection(&inter, &reach, &m->m)) {
			wlr_output_schedule_frame(m->wlr_output);
		}
	}
}

/* The union of where an animating client is and where it is heading, in
 * layout coordinates. */
void anim_client_reach(Client *c, struct wlr_box *out) {
	struct wlr_box a = c->animation.current;
	struct wlr_box b = c->current;
	if (a.width <= 0 || a.height <= 0) {
		a = b;
	}
	if (b.width <= 0 || b.height <= 0) {
		b = a;
	}
	int32_t x1 = a.x < b.x ? a.x : b.x;
	int32_t y1 = a.y < b.y ? a.y : b.y;
	int32_t x2 = (a.x + a.width) > (b.x + b.width)
		? (a.x + a.width) : (b.x + b.width);
	int32_t y2 = (a.y + a.height) > (b.y + b.height)
		? (a.y + a.height) : (b.y + b.height);
	*out = (struct wlr_box){ .x = x1, .y = y1,
		.width = x2 - x1, .height = y2 - y1 };
}