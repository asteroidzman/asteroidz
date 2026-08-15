/*
 * Attempt to consolidate unavoidable suck into one file, away from dwl.c.  This
 * file is not meant to be pretty.  We use a .h file with static inline
 * functions instead of a separate .c module, or function pointers like sway, so
 * that they will simply compile out if the chosen #defines leave them unused.
 */

/* Leave these functions first; they're used in the others */
static inline int32_t client_is_x11(Client *c) {
#ifdef XWAYLAND
	return c->type == X11;
#endif
	return 0;
}

static inline struct wlr_surface *client_surface(Client *c) {
#ifdef XWAYLAND
	if (client_is_x11(c))
		return c->surface.xwayland->surface;
#endif
	return c->surface.xdg->surface;
}

static inline int32_t toplevel_from_wlr_surface(struct wlr_surface *s,
												Client **pc,
												LayerSurface **pl) {
	struct wlr_xdg_surface *xdg_surface, *tmp_xdg_surface;
	struct wlr_surface *root_surface;
	struct wlr_layer_surface_v1 *layer_surface;
	Client *c = NULL;
	LayerSurface *l = NULL;
	int32_t type = -1;
#ifdef XWAYLAND
	struct wlr_xwayland_surface *xsurface;
#endif

	if (!s)
		return -1;
	root_surface = wlr_surface_get_root_surface(s);

#ifdef XWAYLAND
	if ((xsurface = wlr_xwayland_surface_try_from_wlr_surface(root_surface))) {
		c = xsurface->data;
		type = c->type;
		goto end;
	}
#endif

	if ((layer_surface =
			 wlr_layer_surface_v1_try_from_wlr_surface(root_surface))) {
		l = layer_surface->data;
		type = LayerShell;
		goto end;
	}

	xdg_surface = wlr_xdg_surface_try_from_wlr_surface(root_surface);
	while (xdg_surface) {
		tmp_xdg_surface = NULL;
		switch (xdg_surface->role) {
		case WLR_XDG_SURFACE_ROLE_POPUP:
			if (!xdg_surface->popup || !xdg_surface->popup->parent)
				return -1;

			tmp_xdg_surface = wlr_xdg_surface_try_from_wlr_surface(
				xdg_surface->popup->parent);

			if (!tmp_xdg_surface)
				return toplevel_from_wlr_surface(xdg_surface->popup->parent, pc,
												 pl);

			xdg_surface = tmp_xdg_surface;
			break;
		case WLR_XDG_SURFACE_ROLE_TOPLEVEL:
			c = xdg_surface->data;
			type = c->type;
			goto end;
		case WLR_XDG_SURFACE_ROLE_NONE:
			return -1;
		}
	}

end:
	if (pl)
		*pl = l;
	if (pc)
		*pc = c;
	return type;
}

/* The others */
static inline void client_activate_surface(struct wlr_surface *s,
										   int32_t activated) {
	struct wlr_xdg_toplevel *toplevel;
#ifdef XWAYLAND
	struct wlr_xwayland_surface *xsurface;
	if ((xsurface = wlr_xwayland_surface_try_from_wlr_surface(s))) {
		if (activated && xsurface->minimized)
			wlr_xwayland_surface_set_minimized(xsurface, false);
		wlr_xwayland_surface_activate(xsurface, activated);
		return;
	}
#endif
	if ((toplevel = wlr_xdg_toplevel_try_from_wlr_surface(s)))
		wlr_xdg_toplevel_set_activated(toplevel, activated);
}

static inline const char *client_get_appid(Client *c) {
#ifdef XWAYLAND
	if (client_is_x11(c))
		return c->surface.xwayland->class ? c->surface.xwayland->class
										  : "broken";
#endif
	return c->surface.xdg->toplevel->app_id ? c->surface.xdg->toplevel->app_id
											: "broken";
}

static inline int32_t client_get_pid(Client *c) {
	pid_t pid;
#ifdef XWAYLAND
	if (client_is_x11(c))
		return c->surface.xwayland->pid;
#endif
	wl_client_get_credentials(c->surface.xdg->client->client, &pid, NULL, NULL);
	return pid;
}

static inline void client_get_clip(Client *c, struct wlr_box *clip) {
	*clip = (struct wlr_box){
		.x = 0,
		.y = 0,
		.width = c->geom.width - 2 * c->bw,
		.height = c->geom.height - 2 * c->bw,
	};

#ifdef XWAYLAND
	if (client_is_x11(c))
		return;
#endif

	clip->x = c->surface.xdg->geometry.x;
	clip->y = c->surface.xdg->geometry.y;
}

/*
 * ── THE X11 SCALE, AND THE FOUR PLACES IT IS ALLOWED TO EXIST ────────────
 *
 * X11 has no fractional output scale and no way to be told about one. An X
 * client asks for, and is told, a number of PIXELS. Everything else in this
 * compositor -- c->geom, tiling, snapping, borders, animations -- works in
 * LOGICAL units, and c->geom stays the single source of truth for a window's
 * position and size whatever kind of client it is.
 *
 * So the two spaces meet at exactly four boundaries, and nowhere else:
 *
 *   1. configure-out   client_x11_configure() below: logical x scale, rounded
 *   2. geometry-in     client_get_geometry() below: the same, divided back
 *   3. presentation    wlr_scene_surface_set_view_scale(), in the scene
 *   4. input           the view scale again, in the scene's hit test
 *
 * A fifth place would be a bug. The whole reason this is affordable is that
 * nothing between those four has to know an X11 window is different.
 *
 * `c->x11_scale` holds the factor, recomputed when the client's monitor is
 * decided and whenever it changes. It is 1 for every Wayland client, for
 * every X11 client while the option is off, and -- deliberately -- for every
 * output at scale 1 or below. Below 1 there is nothing to gain (the buffer
 * would be asked to be SMALLER than the logical box) and something to lose:
 * the round trip logical -> pixels -> logical is only lossless while the
 * scale is at least 1, and a value that does not survive it would make
 * geometry drift by a pixel every time a window was configured.
 */
static inline float client_x11_scale(Client *c) {
	if (!client_is_x11(c) || c->x11_scale < 1.0f) {
		return 1.0f;
	}
	return c->x11_scale;
}

/* Logical -> raw X11 pixels, and back. Rounded, not truncated: truncation
 * biases every conversion the same way, so a window would lose a pixel per
 * round trip rather than land back where it started. */
static inline int32_t client_x11_from_logical(Client *c, int32_t v) {
	float s = client_x11_scale(c);
	return s == 1.0f ? v : (int32_t)lroundf((float)v * s);
}

static inline int32_t client_x11_to_logical(Client *c, int32_t v) {
	float s = client_x11_scale(c);
	return s == 1.0f ? v : (int32_t)lroundf((float)v / s);
}

#ifdef XWAYLAND
/* BOUNDARY 1. Every wlr_xwayland_surface_configure() in the tree goes through
 * here, because a single missed one sizes a window in the wrong space and the
 * symptom -- a window that is 1.25x too big, or a popup 1.25x off its
 * parent -- looks nothing like "somebody forgot a conversion". */
static inline void client_x11_configure(Client *c, int32_t x, int32_t y,
										int32_t w, int32_t h) {
	wlr_xwayland_surface_configure(
		c->surface.xwayland, (int16_t)client_x11_from_logical(c, x),
		(int16_t)client_x11_from_logical(c, y),
		(uint16_t)client_x11_from_logical(c, w),
		(uint16_t)client_x11_from_logical(c, h));
}
#endif

/* BOUNDARY 1, second half. The surface clip is expressed in SURFACE
 * coordinates, which for an X11 client are the same raw pixels its configure
 * is in -- while every clip box in this tree is computed from c->geom and is
 * therefore logical. Handing a logical clip to a surface committing pixels
 * crops the window to 1/scale of itself AND makes the scene present the crop
 * at 1/scale again: the window loses its right-hand fifth and what is left is
 * shrunk. Every caller keeps working in logical units and converts here. */
static inline void client_set_surface_clip(Client *c, struct wlr_box *clip) {
#ifdef XWAYLAND
	if (client_is_x11(c) && client_x11_scale(c) != 1.0f) {
		struct wlr_box px = {
			.x = client_x11_from_logical(c, clip->x),
			.y = client_x11_from_logical(c, clip->y),
			.width = client_x11_from_logical(c, clip->width),
			.height = client_x11_from_logical(c, clip->height),
		};
		wlr_scene_subsurface_tree_set_clip(&c->scene_surface->node, &px);
		return;
	}
#endif
	wlr_scene_subsurface_tree_set_clip(&c->scene_surface->node, clip);
}

/* BOUNDARY 2. */
static inline void client_get_geometry(Client *c, struct wlr_box *geom) {
#ifdef XWAYLAND
	if (client_is_x11(c)) {
		geom->x = client_x11_to_logical(c, c->surface.xwayland->x);
		geom->y = client_x11_to_logical(c, c->surface.xwayland->y);
		geom->width = client_x11_to_logical(c, c->surface.xwayland->width);
		geom->height = client_x11_to_logical(c, c->surface.xwayland->height);
		return;
	}
#endif
	*geom = c->surface.xdg->geometry;
}

static inline Client *client_get_parent(Client *c) {
	Client *p = NULL;
#ifdef XWAYLAND
	if (client_is_x11(c)) {
		if (c->surface.xwayland->parent)
			toplevel_from_wlr_surface(c->surface.xwayland->parent->surface, &p,
									  NULL);
		return p;
	}
#endif
	if (c->surface.xdg->toplevel->parent)
		toplevel_from_wlr_surface(
			c->surface.xdg->toplevel->parent->base->surface, &p, NULL);
	return p;
}

static inline int32_t client_has_children(Client *c) {
#ifdef XWAYLAND
	if (client_is_x11(c))
		return !wl_list_empty(&c->surface.xwayland->children);
#endif
	/* surface.xdg->link is never empty because it always contains at least the
	 * surface itself. */
	return wl_list_length(&c->surface.xdg->link) > 1;
}

static inline const char *client_get_title(Client *c) {
#ifdef XWAYLAND
	if (client_is_x11(c))
		return c->surface.xwayland->title ? c->surface.xwayland->title
										  : "broken";
#endif
	return c->surface.xdg->toplevel->title ? c->surface.xdg->toplevel->title
										   : "broken";
}

/* Modal dialog: declared via xdg-dialog-v1 (Wayland) or the modal hint
 * (X11). Modals float, carry no tab, and take focus in place of their
 * parent (see focusclient). */
static inline int32_t client_is_modal(Client *c) {
#ifdef XWAYLAND
	if (client_is_x11(c))
		return c->surface.xwayland->modal;
#endif
	struct wlr_xdg_dialog_v1 *dialog =
		wlr_xdg_dialog_v1_try_from_wlr_xdg_toplevel(c->surface.xdg->toplevel);
	return dialog != NULL && dialog->modal;
}

static inline int32_t client_is_float_type(Client *c) {
	struct wlr_xdg_toplevel *toplevel;
	struct wlr_xdg_toplevel_state state;

#ifdef XWAYLAND
	if (client_is_x11(c)) {
		struct wlr_xwayland_surface *surface = c->surface.xwayland;
		xcb_size_hints_t *size_hints = surface->size_hints;

		if (!size_hints)
			return 0;

		if (surface->modal)
			return 1;

		if (wlr_xwayland_surface_has_window_type(
				surface, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_DIALOG) ||
			wlr_xwayland_surface_has_window_type(
				surface, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_SPLASH) ||
			wlr_xwayland_surface_has_window_type(
				surface, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_TOOLBAR) ||
			wlr_xwayland_surface_has_window_type(
				surface, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_UTILITY)) {
			return 1;
		}

		return size_hints && size_hints->min_width > 0 &&
			   size_hints->min_height > 0 &&
			   (size_hints->max_width == size_hints->min_width ||
				size_hints->max_height == size_hints->min_height);
	}
#endif

	toplevel = c->surface.xdg->toplevel;
	state = toplevel->current;
	return toplevel->parent || client_is_modal(c) ||
		   (state.min_width != 0 && state.min_height != 0 &&
			(state.min_width == state.max_width ||
			 state.min_height == state.max_height));
}

static inline int32_t client_is_rendered_on_mon(Client *c, Monitor *m) {
	/* This is needed for when you don't want to check formal assignment,
	 * but rather actual displaying of the pixels.
	 * Usually VISIBLEON suffices and is also faster. */
	struct wlr_surface_output *s;
	int32_t unused_lx, unused_ly;
	if (!wlr_scene_node_coords(&c->scene->node, &unused_lx, &unused_ly))
		return 0;
	wl_list_for_each(s, &client_surface(c)->current_outputs,
					 link) if (s->output == m->wlr_output) return 1;
	return 0;
}

static inline int32_t client_is_unmanaged(Client *c) {
#ifdef XWAYLAND
	if (client_is_x11(c))
		return c->surface.xwayland->override_redirect;
#endif
	return 0;
}

/* Hand a newly focused surface the keys that are physically down MINUS the ones
 * a compositor binding ate.
 *
 * wl_keyboard.enter's key array means "these are held right now", and a client
 * that is told a key is held will REPEAT it until it sees the release. A
 * tag-switch binding fires on press, moves focus, and this enter goes out while
 * the bound key is still down -- so the incoming client starts repeating a key
 * it never saw pressed. Its release is then swallowed (see KeyboardGroup's
 * `consumed`, which must be: the client saw no press), and nothing ever stops
 * the repeat. Under XWayland the X server holds that state itself, so a Proton
 * game receives the key forever.
 *
 * Swallowing the release and reporting the key as held are only correct
 * TOGETHER with this filter: the client must hear nothing at all about a key
 * the compositor consumed. Filtering here rather than at the call sites because
 * every path into a focus change goes through this function.
 *
 * Note the +8: `consumed` holds xkb keycodes (as keypress() translates them),
 * while wlr_keyboard.keycodes and the enter array are raw evdev codes. */
static inline void client_notify_enter(struct wlr_surface *s,
									   struct wlr_keyboard *kb) {
	if (!kb) {
		wlr_seat_keyboard_notify_enter(seat, s, NULL, 0, NULL);
		return;
	}

	struct wlr_keyboard_group *wlr_group =
		wlr_keyboard_group_from_wlr_keyboard(kb);
	KeyboardGroup *group = wlr_group ? wlr_group->data : NULL;
	if (!group || (group->nconsumed == 0 && !group->dispatching)) {
		wlr_seat_keyboard_notify_enter(seat, s, kb->keycodes, kb->num_keycodes,
									   &kb->modifiers);
		return;
	}

	uint32_t keys[WLR_KEYBOARD_KEYS_CAP];
	size_t n = 0;
	for (size_t i = 0; i < kb->num_keycodes && n < LENGTH(keys); i++) {
		uint32_t xkb_code = kb->keycodes[i] + 8;
		/* `dispatching` covers the key whose own binding is running RIGHT NOW,
		 * which is the common case here and is not yet in `consumed`. */
		bool eaten = group->dispatching == xkb_code;
		for (int32_t j = 0; j < group->nconsumed && !eaten; j++)
			eaten = group->consumed[j] == xkb_code;
		if (!eaten)
			keys[n++] = kb->keycodes[i];
	}
	wlr_seat_keyboard_notify_enter(seat, s, keys, n, &kb->modifiers);
}

static inline void client_send_close(Client *c) {
#ifdef XWAYLAND
	if (client_is_x11(c)) {
		wlr_xwayland_surface_close(c->surface.xwayland);
		return;
	}
#endif
	wlr_xdg_toplevel_send_close(c->surface.xdg->toplevel);
}

static inline void client_set_border_color(Client *c,
										   const float color[static 4]) {
	wlr_scene_rect_set_color(c->border, color);
}

static inline void client_set_fullscreen(Client *c, int32_t fullscreen) {
#ifdef XWAYLAND
	if (client_is_x11(c)) {
		wlr_xwayland_surface_set_fullscreen(c->surface.xwayland, fullscreen);
		return;
	}
#endif
	wlr_xdg_toplevel_set_fullscreen(c->surface.xdg->toplevel, fullscreen);
}

static inline void client_set_scale(struct wlr_surface *s, float scale) {
	wlr_fractional_scale_v1_notify_scale(s, scale);
	wlr_surface_set_preferred_buffer_scale(s, (int32_t)ceilf(scale));
}

static inline uint32_t client_set_size(Client *c, uint32_t width,
									   uint32_t height) {
#ifdef XWAYLAND
	if (client_is_x11(c)) {
		struct wlr_xwayland_surface *surface = c->surface.xwayland;
		struct wlr_surface_state *state = &surface->surface->current;

		int32_t width = c->geom.width - 2 * c->bw;
		int32_t height = c->geom.height - 2 * c->bw;

		/* The short circuit compares against what the X SERVER holds, which is
		 * pixels -- state->width is a surface size at buffer scale 1, and
		 * xwayland->x/y are X coordinates. So the logical side is converted
		 * up rather than the pixel side down: a comparison made in logical
		 * units would find 1536 != 1920 on every single call at 1.25x and
		 * reconfigure the window forever. */
		if (client_x11_from_logical(c, width) == (int32_t)state->width &&
			client_x11_from_logical(c, height) == (int32_t)state->height &&
			client_x11_from_logical(c, c->geom.x + (int32_t)c->bw) ==
				(int32_t)c->surface.xwayland->x &&
			client_x11_from_logical(c, c->geom.y + (int32_t)c->bw) ==
				(int32_t)c->surface.xwayland->y) {
			return 0;
		}

		/* Size hints are the client's own numbers, so they are in pixels too.
		 * Brought into logical units here rather than applied in pixels,
		 * because everything downstream of this branch -- including
		 * client_x11_configure -- is logical. */
		xcb_size_hints_t *size_hints = surface->size_hints;
		int32_t min_w =
			size_hints ? client_x11_to_logical(c, size_hints->min_width) : 0;
		int32_t min_h =
			size_hints ? client_x11_to_logical(c, size_hints->min_height) : 0;

		/* overview shrinks windows into thumbnails; honouring a large min-size
		 * hint there snaps the window back to full size and overflows its cell */
		bool ov = c->mon && c->mon->isoverview;
		if (!ov && size_hints && width < min_w)
			width = min_w;
		if (!ov && size_hints && height < min_h)
			height = min_h;

		client_x11_configure(c, c->geom.x + c->bw, c->geom.y + c->bw, width,
							 height);
		return 1;
	}
#endif
	if ((int32_t)width == c->surface.xdg->toplevel->current.width &&
		(int32_t)height == c->surface.xdg->toplevel->current.height)
		return 0;
	return wlr_xdg_toplevel_set_size(c->surface.xdg->toplevel, (int32_t)width,
									 (int32_t)height);
}

static inline void client_set_minimized(Client *c, bool minimized) {
#ifdef XWAYLAND
	if (client_is_x11(c)) {
		wlr_xwayland_surface_set_minimized(c->surface.xwayland, minimized);
		return;
	}
#endif

	return;
}

static inline void client_set_maximized(Client *c, bool maximized) {
	struct wlr_xdg_toplevel *toplevel;

#ifdef XWAYLAND
	if (client_is_x11(c)) {
		wlr_xwayland_surface_set_maximized(c->surface.xwayland, maximized,
										   maximized);
		return;
	}
#endif
	toplevel = c->surface.xdg->toplevel;
	wlr_xdg_toplevel_set_maximized(toplevel, maximized);
	return;
}

static inline void client_set_tiled(Client *c, uint32_t edges) {
	struct wlr_xdg_toplevel *toplevel;
#ifdef XWAYLAND
	if (client_is_x11(c) && c->force_fakemaximize) {
		wlr_xwayland_surface_set_maximized(c->surface.xwayland,
										   edges != WLR_EDGE_NONE,
										   edges != WLR_EDGE_NONE);
		return;
	}
#endif

	toplevel = c->surface.xdg->toplevel;

	if (wl_resource_get_version(c->surface.xdg->toplevel->resource) >=
		XDG_TOPLEVEL_STATE_TILED_RIGHT_SINCE_VERSION) {
		wlr_xdg_toplevel_set_tiled(c->surface.xdg->toplevel, edges);
	}

	if (c->force_fakemaximize) {
		wlr_xdg_toplevel_set_maximized(toplevel, edges != WLR_EDGE_NONE);
	}
}

/* xdg-shell's suspended state: tells a toplevel its content isn't visible to
 * the user, so a browser or player can throttle rendering and decoding. Only
 * flip it when it actually changes -- each call schedules a configure.
 *
 * X11 has no equivalent, and wlroots asserts if the client bound wm_base below
 * v6, hence the version guard (same shape as client_set_tiled above). */
static inline void client_set_suspended(Client *c, bool suspended) {
#ifdef XWAYLAND
	if (client_is_x11(c))
		return;
#endif
	if (c->issuspended == suspended)
		return;
	if (wl_resource_get_version(c->surface.xdg->toplevel->resource) <
		XDG_TOPLEVEL_STATE_SUSPENDED_SINCE_VERSION)
		return;

	c->issuspended = suspended;
	wlr_xdg_toplevel_set_suspended(c->surface.xdg->toplevel, suspended);
}

static inline int32_t client_should_ignore_focus(Client *c) {

#ifdef XWAYLAND
	if (client_is_x11(c)) {
		struct wlr_xwayland_surface *surface = c->surface.xwayland;

		if (!surface->hints)
			return 0;

		return !surface->hints->input;
	}
#endif
	return 0;
}

static inline int32_t client_is_x11_popup(Client *c) {

#ifdef XWAYLAND
	if (client_is_x11(c)) {
		struct wlr_xwayland_surface *surface = c->surface.xwayland;
		// window types that should never receive focus
		const uint32_t no_focus_types[] = {
			WLR_XWAYLAND_NET_WM_WINDOW_TYPE_COMBO,
			WLR_XWAYLAND_NET_WM_WINDOW_TYPE_DND,
			WLR_XWAYLAND_NET_WM_WINDOW_TYPE_DROPDOWN_MENU,
			WLR_XWAYLAND_NET_WM_WINDOW_TYPE_MENU,
			WLR_XWAYLAND_NET_WM_WINDOW_TYPE_NOTIFICATION,
			WLR_XWAYLAND_NET_WM_WINDOW_TYPE_POPUP_MENU,
			WLR_XWAYLAND_NET_WM_WINDOW_TYPE_SPLASH,
			WLR_XWAYLAND_NET_WM_WINDOW_TYPE_TOOLTIP,
			WLR_XWAYLAND_NET_WM_WINDOW_TYPE_UTILITY};
		// check whether the window type should be denied focus
		for (size_t i = 0;
			 i < sizeof(no_focus_types) / sizeof(no_focus_types[0]); ++i) {
			if (wlr_xwayland_surface_has_window_type(surface,
													 no_focus_types[i])) {
				return 1;
			}
		}
	}
#endif
	return 0;
}

/* Splash screens announce themselves only through the X11 window type --
 * xdg-shell has no splash concept, so this is always false for Wayland
 * clients. Used to keep compositor chrome (the titlebar tab) off windows
 * that exist for a few seconds while an app loads. */
static inline int32_t client_is_splash(Client *c) {
#ifdef XWAYLAND
	if (client_is_x11(c))
		return wlr_xwayland_surface_has_window_type(
			c->surface.xwayland, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_SPLASH);
#endif
	return 0;
}

/* Every titlebar decision site (draw, space reservation, corner squaring,
 * border-color pairing) must agree, so they all ask this one question.
 * Dialog-likes never get a tab: everything client_is_float_type()
 * auto-floats for (parents/transients, modals, X11 dialog/splash/toolbar/
 * utility types, fixed min==max size hints -- Steam's parentless CEF
 * popups float via that last one). Whatever the compositor floats
 * because it is dialog-ish, it also leaves tab-less; user-toggled
 * floating is unaffected. The no-titlebar window rule opts anything
 * else out. */
static inline int32_t client_no_titlebar(Client *c) {
	return c->isnotitlebar || client_is_splash(c) || client_is_float_type(c);
}

static inline int32_t client_should_global(Client *c) {

#ifdef XWAYLAND
	if (client_is_x11(c)) {
		struct wlr_xwayland_surface *surface = c->surface.xwayland;

		if (surface->sticky)
			return 1;
	}
#endif
	return 0;
}

static inline int32_t client_should_overtop(Client *c) {

#ifdef XWAYLAND
	if (client_is_x11(c)) {
		struct wlr_xwayland_surface *surface = c->surface.xwayland;
		if (surface->above)
			return 1;
	}
#endif
	return 0;
}

static inline int32_t client_wants_focus(Client *c) {
#ifdef XWAYLAND
	return client_is_unmanaged(c) &&
		   wlr_xwayland_surface_override_redirect_wants_focus(
			   c->surface.xwayland) &&
		   wlr_xwayland_surface_icccm_input_model(c->surface.xwayland) !=
			   WLR_ICCCM_INPUT_MODEL_NONE;
#endif
	return 0;
}

static inline int32_t client_wants_fullscreen(Client *c) {
#ifdef XWAYLAND
	if (client_is_x11(c))
		return c->surface.xwayland->fullscreen;
#endif
	return c->surface.xdg->toplevel->requested.fullscreen;
}

static inline bool client_request_minimize(Client *c, void *data) {

#ifdef XWAYLAND
	if (client_is_x11(c)) {
		struct wlr_xwayland_minimize_event *event = data;
		return event->minimize;
	}
#endif

	return c->surface.xdg->toplevel->requested.minimized;
}

static inline bool client_request_maximize(Client *c, void *data) {

#ifdef XWAYLAND
	if (client_is_x11(c)) {
		struct wlr_xwayland_surface *surface = c->surface.xwayland;
		return surface->maximized_vert || surface->maximized_horz;
	}
#endif

	return c->surface.xdg->toplevel->requested.maximized;
}

static inline void client_set_size_bound(Client *c) {
	struct wlr_xdg_toplevel *toplevel;
	struct wlr_xdg_toplevel_state state;
	/* in overview, let windows shrink below their min-size into the thumbnail */
	bool ov = c->mon && c->mon->isoverview;

#ifdef XWAYLAND
	if (client_is_x11(c)) {
		struct wlr_xwayland_surface *surface = c->surface.xwayland;
		xcb_size_hints_t *size_hints = surface->size_hints;

		if (!size_hints)
			return;

		/* The hints are the client's own numbers and are therefore in raw X11
		 * pixels; c->geom is logical. Converted, not compared across the two
		 * spaces -- at 1.25x an untranslated min-width of 800 would read as
		 * 800 logical, and every window with a min-size hint would come out a
		 * quarter too large. */
		int32_t min_w = client_x11_to_logical(c, size_hints->min_width);
		int32_t min_h = client_x11_to_logical(c, size_hints->min_height);
		int32_t max_w = client_x11_to_logical(c, size_hints->max_width);
		int32_t max_h = client_x11_to_logical(c, size_hints->max_height);

		/* The comparisons keep their original (unsigned) shape deliberately:
		 * only the numbers being compared against have moved space, and a
		 * change of arithmetic here would alter behaviour for every X11
		 * client whether the option is on or not. */
		if (!ov && (uint32_t)c->geom.width - 2 * c->bw < min_w && min_w > 0)
			c->geom.width = min_w + 2 * c->bw;
		if (!ov && (uint32_t)c->geom.height - 2 * c->bw < min_h && min_h > 0)
			c->geom.height = min_h + 2 * c->bw;
		if ((uint32_t)c->geom.width - 2 * c->bw > max_w && max_w > 0)
			c->geom.width = max_w + 2 * c->bw;
		if ((uint32_t)c->geom.height - 2 * c->bw > max_h && max_h > 0)
			c->geom.height = max_h + 2 * c->bw;
		return;
	}
#endif

	toplevel = c->surface.xdg->toplevel;
	state = toplevel->current;
	if (!ov && (uint32_t)c->geom.width - 2 * c->bw < state.min_width &&
		state.min_width > 0) {
		c->geom.width = state.min_width + 2 * c->bw;
	}
	if (!ov && (uint32_t)c->geom.height - 2 * c->bw < state.min_height &&
		state.min_height > 0) {
		c->geom.height = state.min_height + 2 * c->bw;
	}
	if ((uint32_t)c->geom.width - 2 * c->bw > state.max_width &&
		state.max_width > 0) {
		c->geom.width = state.max_width + 2 * c->bw;
	}
	if ((uint32_t)c->geom.height - 2 * c->bw > state.max_height &&
		state.max_height > 0) {
		c->geom.height = state.max_height + 2 * c->bw;
	}
}

/* The scale a client's native overlays should RASTERISE at: its output's.
 *
 * asteroidz_tab_bar_node_update() takes this as its `scale`, and it decides two
 * things at once -- the pixel size of the surface it draws into
 * (logical * scale) and the Pango resolution (96 * scale) -- while the scene
 * buffer is displayed at the LOGICAL size. Hand it the output's scale and the
 * titlebar is rasterised at the panel's real resolution. Hand it 1.0, as every
 * caller but one did, and it is rasterised at logical resolution and then
 * resampled up by the scene graph: a titlebar that is exactly the right size
 * and visibly soft on any output above scale 1.
 */
static inline float client_render_scale(Client *c) {
	if (c && c->mon && c->mon->wlr_output && c->mon->wlr_output->scale > 0.0f)
		return c->mon->wlr_output->scale;
	return 1.0f;
}
