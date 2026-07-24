/*
 * org.freedesktop.impl.portal.GlobalShortcuts backend, so portal-aware
 * clients (Discord, ashpd apps, ...) get global shortcuts / push-to-talk
 * on asteroidz. Sessions and shortcuts are registered over D-Bus; matching
 * key presses/releases emit Activated/Deactivated and are consumed.
 *
 * Registered as the "asteroidz" portal backend via
 * /usr/share/xdg-desktop-portal/portals/asteroidz.portal.
 */
#include <stdio.h>
#include <strings.h>
#include <sys/stat.h>
#include <systemd/sd-bus.h>

#define GS_BUS_NAME "org.freedesktop.impl.portal.desktop.asteroidz"
#define GS_OBJ_PATH "/org/freedesktop/portal/desktop"
#define GS_IFACE "org.freedesktop.impl.portal.GlobalShortcuts"
#define GS_SESSION_IFACE "org.freedesktop.impl.portal.Session"
#define GS_SESSION_PREFIX "/org/freedesktop/portal/desktop/session"
#define GS_MOD_MASK                                                           \
	(WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT | WLR_MODIFIER_SHIFT |               \
	 WLR_MODIFIER_LOGO)

typedef struct GlobalShortcut {
	struct wl_list link;
	char *id;
	char *description;
	char *trigger; // preferred_trigger as given by the client
	uint32_t mods;
	xkb_keysym_t keysym;
	bool has_trigger;
	bool activated;
} GlobalShortcut;

typedef struct GlobalShortcutsSession {
	struct wl_list link;
	char *path;
	char *app_id;
	struct wl_list shortcuts; // GlobalShortcut.link
} GlobalShortcutsSession;

static sd_bus *gs_bus;
static uint32_t gs_portal_version = 1;
static sd_bus_slot *gs_slot, *gs_session_slot;
static struct wl_event_source *gs_bus_source;
static struct wl_list gs_sessions;

/* Interactive key-picker state. asteroidz's portal binds the app's
 * preferred_trigger directly (no picker of its own), so when an app binds a
 * shortcut with NO usable trigger we prompt the user to press a key: a modal
 * compositor overlay grabs the next chord and that becomes the binding. Picked
 * bindings persist in ~/.config/asteroidz/global-shortcuts and override the
 * app's preferred_trigger on later binds. */
static struct {
	bool active;
	GlobalShortcutsSession *session;
	GlobalShortcut *shortcut;
	struct wlr_scene_tree *tree;
	struct asteroidz_jump_label_node *label;
	struct wl_event_source *timeout;
} gs_pick;

static bool gs_pick_begin(GlobalShortcutsSession *s, GlobalShortcut *sc);
static void gs_pick_finish(uint32_t mods, xkb_keysym_t sym, bool cancelled);

/* ── persisted user bindings, keyed by app_id + shortcut id ── */
static const char *gs_config_path(void) {
	static char path[512];
	const char *home = getenv("HOME");
	if (!home)
		return NULL;
	char dir[400];
	snprintf(dir, sizeof(dir), "%s/.config/asteroidz", home);
	mkdir(dir, 0755);
	snprintf(path, sizeof(path), "%s/global-shortcuts", dir);
	return path;
}

/* saved trigger for (app_id, id) or NULL; caller frees */
static char *gs_load_saved(const char *app_id, const char *id) {
	const char *path = gs_config_path();
	if (!path)
		return NULL;
	FILE *f = fopen(path, "r");
	if (!f)
		return NULL;
	char line[512], *found = NULL;
	while (fgets(line, sizeof(line), f)) {
		char *nl = strchr(line, '\n');
		if (nl)
			*nl = '\0';
		char copy[512];
		snprintf(copy, sizeof(copy), "%s", line);
		char *a = strtok(copy, "\t");
		char *i = strtok(NULL, "\t");
		char *t = strtok(NULL, "\t");
		if (a && i && t && !strcmp(a, app_id) && !strcmp(i, id)) {
			found = strdup(t);
			break;
		}
	}
	fclose(f);
	return found;
}

/* persist trigger for (app_id, id), replacing any existing entry */
static void gs_save(const char *app_id, const char *id, const char *trigger) {
	const char *path = gs_config_path();
	if (!path)
		return;
	char (*keep)[512] = NULL;
	size_t n = 0, cap = 0;
	FILE *f = fopen(path, "r");
	if (f) {
		char line[512];
		while (fgets(line, sizeof(line), f)) {
			char copy[512];
			snprintf(copy, sizeof(copy), "%s", line);
			char *nl = strchr(copy, '\n');
			if (nl)
				*nl = '\0';
			char *a = strtok(copy, "\t");
			char *i = strtok(NULL, "\t");
			if (a && i && !strcmp(a, app_id) && !strcmp(i, id))
				continue; /* drop the old entry we're replacing */
			if (n == cap) {
				cap = cap ? cap * 2 : 8;
				keep = realloc(keep, cap * sizeof(*keep));
			}
			snprintf(keep[n++], sizeof(keep[0]), "%s", line);
		}
		fclose(f);
	}
	f = fopen(path, "w");
	if (f) {
		for (size_t k = 0; k < n; k++)
			fprintf(f, "%s\n", keep[k]);
		fprintf(f, "%s\t%s\t%s\n", app_id, id, trigger);
		fclose(f);
	}
	free(keep);
}

static void gs_shortcut_destroy(GlobalShortcut *shortcut) {
	wl_list_remove(&shortcut->link);
	free(shortcut->id);
	free(shortcut->description);
	free(shortcut->trigger);
	free(shortcut);
}

static void gs_session_destroy(GlobalShortcutsSession *session) {
	GlobalShortcut *shortcut, *tmp;
	if (gs_pick.active && gs_pick.session == session)
		gs_pick_finish(0, XKB_KEY_NoSymbol, true); /* session gone: cancel picker */
	wl_list_for_each_safe(shortcut, tmp, &session->shortcuts, link)
		gs_shortcut_destroy(shortcut);
	wl_list_remove(&session->link);
	free(session->path);
	free(session->app_id);
	free(session);
}

static GlobalShortcutsSession *gs_session_find(const char *path) {
	GlobalShortcutsSession *session;
	wl_list_for_each(session, &gs_sessions, link) {
		if (strcmp(session->path, path) == 0)
			return session;
	}
	return NULL;
}

/* Parse an XDG shortcuts trigger like "CTRL+ALT+t" or "LOGO+F1" */
static bool gs_parse_trigger(const char *trigger, uint32_t *mods_out,
							 xkb_keysym_t *sym_out) {
	char buf[128], *token, *saveptr = NULL;
	uint32_t mods = 0;
	xkb_keysym_t sym = XKB_KEY_NoSymbol;

	if (!trigger || !*trigger)
		return false;
	snprintf(buf, sizeof(buf), "%s", trigger);

	for (token = strtok_r(buf, "+", &saveptr); token;
		 token = strtok_r(NULL, "+", &saveptr)) {
		if (!strcasecmp(token, "CTRL") || !strcasecmp(token, "CONTROL")) {
			mods |= WLR_MODIFIER_CTRL;
		} else if (!strcasecmp(token, "ALT")) {
			mods |= WLR_MODIFIER_ALT;
		} else if (!strcasecmp(token, "SHIFT")) {
			mods |= WLR_MODIFIER_SHIFT;
		} else if (!strcasecmp(token, "LOGO") || !strcasecmp(token, "SUPER") ||
				   !strcasecmp(token, "META")) {
			mods |= WLR_MODIFIER_LOGO;
		} else {
			sym = xkb_keysym_from_name(token, XKB_KEYSYM_CASE_INSENSITIVE);
		}
	}

	if (sym == XKB_KEY_NoSymbol)
		return false;
	*mods_out = mods;
	*sym_out = xkb_keysym_to_lower(sym);
	return true;
}

/* Read one a(sa{sv}) shortcuts array from msg into the session */
static int gs_read_shortcuts(sd_bus_message *msg,
							 GlobalShortcutsSession *session) {
	int ret = sd_bus_message_enter_container(msg, 'a', "(sa{sv})");
	if (ret < 0)
		return ret;

	while ((ret = sd_bus_message_enter_container(msg, 'r', "sa{sv}")) > 0) {
		const char *id = NULL;
		GlobalShortcut *shortcut;

		ret = sd_bus_message_read(msg, "s", &id);
		if (ret < 0)
			return ret;

		shortcut = ecalloc(1, sizeof(*shortcut));
		shortcut->id = strdup(id);
		wl_list_insert(&session->shortcuts, &shortcut->link);

		ret = sd_bus_message_enter_container(msg, 'a', "{sv}");
		if (ret < 0)
			return ret;
		while ((ret = sd_bus_message_enter_container(msg, 'e', "sv")) > 0) {
			const char *key = NULL;
			ret = sd_bus_message_read(msg, "s", &key);
			if (ret < 0)
				return ret;
			if (key && strcmp(key, "description") == 0) {
				const char *val = NULL;
				sd_bus_message_enter_container(msg, 'v', "s");
				sd_bus_message_read(msg, "s", &val);
				sd_bus_message_exit_container(msg);
				free(shortcut->description);
				shortcut->description = val ? strdup(val) : NULL;
			} else if (key && strcmp(key, "preferred_trigger") == 0) {
				const char *val = NULL;
				sd_bus_message_enter_container(msg, 'v', "s");
				sd_bus_message_read(msg, "s", &val);
				sd_bus_message_exit_container(msg);
				free(shortcut->trigger);
				shortcut->trigger = val ? strdup(val) : NULL;
			} else {
				sd_bus_message_skip(msg, "v");
			}
			sd_bus_message_exit_container(msg);
		}
		sd_bus_message_exit_container(msg); /* a{sv} */
		sd_bus_message_exit_container(msg); /* (sa{sv}) */

		/* An explicit empty preferred_trigger ("") is the app asking us to
		 * (re)assign interactively — skip the saved binding in that case so the
		 * picker fires. Otherwise a persisted user pick wins over the app's
		 * preferred_trigger. */
		bool wants_pick =
			shortcut->trigger && shortcut->trigger[0] == '\0';
		if (!wants_pick) {
			char *saved = gs_load_saved(session->app_id, shortcut->id);
			if (saved) {
				free(shortcut->trigger);
				shortcut->trigger = saved;
			}
		}
		shortcut->has_trigger = gs_parse_trigger(
			shortcut->trigger, &shortcut->mods, &shortcut->keysym);
		if (!shortcut->has_trigger)
			wlr_log(WLR_INFO,
					"global shortcut '%s' has no usable trigger ('%s') "
					"- will prompt for a key",
					shortcut->id, shortcut->trigger ? shortcut->trigger : "");
	}
	return sd_bus_message_exit_container(msg); /* a(sa{sv}) */
}

/* Append (u response, a{sv} results{"shortcuts": a(sa{sv})}) */
static int gs_reply_shortcuts(sd_bus_message *msg,
							  GlobalShortcutsSession *session) {
	sd_bus_message *reply = NULL;
	GlobalShortcut *shortcut;
	int ret = sd_bus_message_new_method_return(msg, &reply);
	if (ret < 0)
		return ret;

	sd_bus_message_append(reply, "u", 0);
	sd_bus_message_open_container(reply, 'a', "{sv}");
	sd_bus_message_open_container(reply, 'e', "sv");
	sd_bus_message_append(reply, "s", "shortcuts");
	sd_bus_message_open_container(reply, 'v', "a(sa{sv})");
	sd_bus_message_open_container(reply, 'a', "(sa{sv})");
	wl_list_for_each(shortcut, &session->shortcuts, link) {
		sd_bus_message_open_container(reply, 'r', "sa{sv}");
		sd_bus_message_append(reply, "s", shortcut->id);
		sd_bus_message_open_container(reply, 'a', "{sv}");
		if (shortcut->description) {
			sd_bus_message_open_container(reply, 'e', "sv");
			sd_bus_message_append(reply, "s", "description");
			sd_bus_message_append(reply, "v", "s", shortcut->description);
			sd_bus_message_close_container(reply);
		}
		if (shortcut->trigger) {
			sd_bus_message_open_container(reply, 'e', "sv");
			sd_bus_message_append(reply, "s", "trigger_description");
			sd_bus_message_append(reply, "v", "s", shortcut->trigger);
			sd_bus_message_close_container(reply);
		}
		sd_bus_message_close_container(reply); /* a{sv} */
		sd_bus_message_close_container(reply); /* (sa{sv}) */
	}
	sd_bus_message_close_container(reply); /* a(sa{sv}) */
	sd_bus_message_close_container(reply); /* v */
	sd_bus_message_close_container(reply); /* e */
	sd_bus_message_close_container(reply); /* a{sv} */

	ret = sd_bus_send(NULL, reply, NULL);
	sd_bus_message_unref(reply);
	return ret < 0 ? ret : 1;
}

static int gs_handle_create_session(sd_bus_message *msg, void *data,
									sd_bus_error *err) {
	const char *handle, *session_path, *app_id;
	GlobalShortcutsSession *session;

	int ret = sd_bus_message_read(msg, "oos", &handle, &session_path, &app_id);
	if (ret < 0)
		return ret;

	if (gs_session_find(session_path))
		return sd_bus_reply_method_return(msg, "ua{sv}", 2, 0);

	session = ecalloc(1, sizeof(*session));
	session->path = strdup(session_path);
	session->app_id = strdup(app_id);
	wl_list_init(&session->shortcuts);
	wl_list_insert(&gs_sessions, &session->link);
	wlr_log(WLR_INFO, "global shortcuts session created for %s", app_id);

	/* the "shortcuts" option may carry an initial binding set */
	ret = sd_bus_message_enter_container(msg, 'a', "{sv}");
	if (ret >= 0) {
		while (sd_bus_message_enter_container(msg, 'e', "sv") > 0) {
			const char *key = NULL;
			sd_bus_message_read(msg, "s", &key);
			if (key && strcmp(key, "shortcuts") == 0) {
				sd_bus_message_enter_container(msg, 'v', "a(sa{sv})");
				gs_read_shortcuts(msg, session);
				sd_bus_message_exit_container(msg);
			} else {
				sd_bus_message_skip(msg, "v");
			}
			sd_bus_message_exit_container(msg);
		}
		sd_bus_message_exit_container(msg);
	}

	return gs_reply_shortcuts(msg, session);
}

static int gs_handle_bind_shortcuts(sd_bus_message *msg, void *data,
									sd_bus_error *err) {
	const char *handle, *session_path;
	GlobalShortcutsSession *session;
	GlobalShortcut *shortcut, *tmp;

	int ret = sd_bus_message_read(msg, "oo", &handle, &session_path);
	if (ret < 0)
		return ret;

	session = gs_session_find(session_path);
	if (!session)
		return sd_bus_reply_method_return(msg, "ua{sv}", 2, 0);

	/* replace the shortcut set */
	wl_list_for_each_safe(shortcut, tmp, &session->shortcuts, link)
		gs_shortcut_destroy(shortcut);

	ret = gs_read_shortcuts(msg, session);
	if (ret < 0)
		return ret;
	wlr_log(WLR_INFO, "global shortcuts bound for %s (%d entries)",
			session->app_id, wl_list_length(&session->shortcuts));

	int reply = gs_reply_shortcuts(msg, session);
	/* Any shortcut with no usable trigger gets an interactive key prompt; the
	 * app learns the result via ShortcutsChanged. */
	GlobalShortcut *need = NULL;
	wl_list_for_each(shortcut, &session->shortcuts, link) {
		if (!shortcut->has_trigger) {
			need = shortcut;
			break;
		}
	}
	if (need)
		gs_pick_begin(session, need);
	return reply;
}

static int gs_handle_list_shortcuts(sd_bus_message *msg, void *data,
									sd_bus_error *err) {
	const char *handle, *session_path;
	GlobalShortcutsSession *session;

	int ret = sd_bus_message_read(msg, "oo", &handle, &session_path);
	if (ret < 0)
		return ret;
	session = gs_session_find(session_path);
	if (!session)
		return sd_bus_reply_method_return(msg, "ua{sv}", 2, 0);
	return gs_reply_shortcuts(msg, session);
}

static int gs_handle_session_close(sd_bus_message *msg, void *data,
								   sd_bus_error *err) {
	GlobalShortcutsSession *session =
		gs_session_find(sd_bus_message_get_path(msg));
	if (session) {
		wlr_log(WLR_INFO, "global shortcuts session closed for %s",
				session->app_id);
		gs_session_destroy(session);
	}
	return sd_bus_reply_method_return(msg, "");
}

static const sd_bus_vtable gs_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("CreateSession", "oosa{sv}", "ua{sv}",
				  gs_handle_create_session, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("BindShortcuts", "ooa(sa{sv})sa{sv}", "ua{sv}",
				  gs_handle_bind_shortcuts, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("ListShortcuts", "oo", "ua{sv}", gs_handle_list_shortcuts,
				  SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_SIGNAL("Activated", "osta{sv}", 0),
	SD_BUS_SIGNAL("Deactivated", "osta{sv}", 0),
	SD_BUS_SIGNAL("ShortcutsChanged", "oa(sa{sv})", 0),
	SD_BUS_PROPERTY("version", "u", NULL,
					0, SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_VTABLE_END,
};

static const sd_bus_vtable gs_session_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("Close", "", "", gs_handle_session_close,
				  SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_PROPERTY("version", "u", NULL, 0, SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_VTABLE_END,
};

static void gs_emit_state(GlobalShortcutsSession *session,
						  GlobalShortcut *shortcut, bool activated,
						  uint32_t time_msec) {
	sd_bus_message *sig = NULL;
	if (sd_bus_message_new_signal(gs_bus, &sig, GS_OBJ_PATH, GS_IFACE,
								  activated ? "Activated" : "Deactivated") < 0)
		return;
	sd_bus_message_append(sig, "ost", session->path, shortcut->id,
						  (uint64_t)time_msec);
	sd_bus_message_open_container(sig, 'a', "{sv}");
	sd_bus_message_close_container(sig);
	sd_bus_send(NULL, sig, NULL);
	sd_bus_message_unref(sig);
	sd_bus_flush(gs_bus);
}

/* Emit ShortcutsChanged so a bound app learns the new trigger after a pick. */
static void gs_emit_changed(GlobalShortcutsSession *session) {
	sd_bus_message *sig = NULL;
	GlobalShortcut *sc;
	if (sd_bus_message_new_signal(gs_bus, &sig, GS_OBJ_PATH, GS_IFACE,
								  "ShortcutsChanged") < 0)
		return;
	sd_bus_message_append(sig, "o", session->path);
	sd_bus_message_open_container(sig, 'a', "(sa{sv})");
	wl_list_for_each(sc, &session->shortcuts, link) {
		sd_bus_message_open_container(sig, 'r', "sa{sv}");
		sd_bus_message_append(sig, "s", sc->id);
		sd_bus_message_open_container(sig, 'a', "{sv}");
		if (sc->trigger) {
			sd_bus_message_open_container(sig, 'e', "sv");
			sd_bus_message_append(sig, "s", "trigger_description");
			sd_bus_message_append(sig, "v", "s", sc->trigger);
			sd_bus_message_close_container(sig);
		}
		sd_bus_message_close_container(sig); /* a{sv} */
		sd_bus_message_close_container(sig); /* (sa{sv}) */
	}
	sd_bus_message_close_container(sig); /* a(sa{sv}) */
	sd_bus_send(NULL, sig, NULL);
	sd_bus_message_unref(sig);
	sd_bus_flush(gs_bus);
}

/* Inverse of gs_parse_trigger: build "CTRL+ALT+F13" from mods + keysym. */
static char *gs_trigger_string(uint32_t mods, xkb_keysym_t sym) {
	char name[128];
	if (xkb_keysym_get_name(sym, name, sizeof(name)) <= 0)
		snprintf(name, sizeof(name), "space");
	char buf[256];
	buf[0] = '\0';
	if (mods & WLR_MODIFIER_CTRL)
		strcat(buf, "CTRL+");
	if (mods & WLR_MODIFIER_ALT)
		strcat(buf, "ALT+");
	if (mods & WLR_MODIFIER_SHIFT)
		strcat(buf, "SHIFT+");
	if (mods & WLR_MODIFIER_LOGO)
		strcat(buf, "LOGO+");
	strncat(buf, name, sizeof(buf) - strlen(buf) - 1);
	return strdup(buf);
}

/* Modifier keysyms are not valid standalone triggers; wait for a real key.
 * (Caps_Lock is intentionally allowed — it's a common push-to-talk key.) */
static bool gs_sym_is_modifier(xkb_keysym_t sym) {
	switch (sym) {
	case XKB_KEY_Shift_L:
	case XKB_KEY_Shift_R:
	case XKB_KEY_Control_L:
	case XKB_KEY_Control_R:
	case XKB_KEY_Alt_L:
	case XKB_KEY_Alt_R:
	case XKB_KEY_Super_L:
	case XKB_KEY_Super_R:
	case XKB_KEY_Meta_L:
	case XKB_KEY_Meta_R:
	case XKB_KEY_Hyper_L:
	case XKB_KEY_Hyper_R:
	case XKB_KEY_ISO_Level3_Shift:
		return true;
	default:
		return false;
	}
}

/* Compositor-native modal prompt: a full-monitor dim rect + a centered label,
 * drawn with the existing jump-label text node — no GTK/Qt. */
static void gs_pick_overlay_show(const char *desc) {
	if (!selmon)
		return;
	gs_pick.tree = wlr_scene_tree_create(layers[LyrOverlay]);
	if (!gs_pick.tree)
		return;
	float dim[4] = {0.f, 0.f, 0.f, 0.55f};
	struct wlr_scene_rect *bg = wlr_scene_rect_create(
		gs_pick.tree, selmon->m.width, selmon->m.height, dim);
	if (bg)
		wlr_scene_node_set_position(&bg->node, selmon->m.x, selmon->m.y);

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
	th.font_desc = "monospace Bold 18";
	gs_pick.label = asteroidz_jump_label_node_create(gs_pick.tree, th);
	if (gs_pick.label) {
		char msg[256];
		snprintf(msg, sizeof(msg),
				 "Press a key for \xe2\x80\x9c%s\xe2\x80\x9d\n(Esc to cancel)",
				 desc && *desc ? desc : "global shortcut");
		float scale = selmon->wlr_output ? selmon->wlr_output->scale : 1.f;
		asteroidz_jump_label_node_update(gs_pick.label, msg, scale);
		int lw = 360, lh = 90;
		if (gs_pick.label->scene_buffer && gs_pick.label->scene_buffer->buffer) {
			lw = gs_pick.label->scene_buffer->buffer->width;
			lh = gs_pick.label->scene_buffer->buffer->height;
		}
		wlr_scene_node_set_position(&gs_pick.label->scene_buffer->node,
									selmon->m.x + (selmon->m.width - lw) / 2,
									selmon->m.y + (selmon->m.height - lh) / 2);
	}
	wlr_scene_node_raise_to_top(&gs_pick.tree->node);
}

static void gs_pick_overlay_hide(void) {
	if (gs_pick.label) {
		asteroidz_jump_label_node_destroy(gs_pick.label);
		gs_pick.label = NULL;
	}
	if (gs_pick.tree) {
		wlr_scene_node_destroy(&gs_pick.tree->node);
		gs_pick.tree = NULL;
	}
}

static void gs_pick_finish(uint32_t mods, xkb_keysym_t sym, bool cancelled) {
	if (!gs_pick.active)
		return;
	GlobalShortcut *sc = gs_pick.shortcut;
	GlobalShortcutsSession *session = gs_pick.session;
	if (!cancelled && sym != XKB_KEY_NoSymbol) {
		free(sc->trigger);
		sc->trigger = gs_trigger_string(mods & GS_MOD_MASK, sym);
		sc->mods = mods & GS_MOD_MASK;
		sc->keysym = xkb_keysym_to_lower(sym);
		sc->has_trigger = true;
		gs_save(session->app_id, sc->id, sc->trigger);
		wlr_log(WLR_INFO, "global shortcut '%s' bound to %s (interactive)",
				sc->id, sc->trigger);
	} else {
		wlr_log(WLR_INFO, "global shortcut '%s' interactive bind cancelled",
				sc->id);
	}
	gs_pick_overlay_hide();
	if (gs_pick.timeout) {
		wl_event_source_remove(gs_pick.timeout);
		gs_pick.timeout = NULL;
	}
	gs_pick.active = false;
	gs_pick.session = NULL;
	gs_pick.shortcut = NULL;
	if (!cancelled)
		gs_emit_changed(session); /* tell the app its new trigger */
}

static int gs_pick_timeout(void *data) {
	(void)data;
	gs_pick_finish(0, XKB_KEY_NoSymbol, true);
	return 0;
}

static bool gs_pick_begin(GlobalShortcutsSession *s, GlobalShortcut *sc) {
	if (gs_pick.active)
		return false;
	gs_pick.active = true;
	gs_pick.session = s;
	gs_pick.shortcut = sc;
	gs_pick_overlay_show(sc->description ? sc->description : sc->id);
	gs_pick.timeout = wl_event_loop_add_timer(event_loop, gs_pick_timeout, NULL);
	if (gs_pick.timeout)
		wl_event_source_timer_update(gs_pick.timeout, 12000);
	wlr_log(WLR_INFO, "global shortcut '%s': waiting for a key press", sc->id);
	return true;
}

/* Called from keypress(): returns true when the key was consumed by a
 * registered global shortcut. Push-to-talk relies on both edges: press
 * matches mods+keysym, release matches the keysym of an active shortcut
 * (modifiers may already have been released). */
bool global_shortcuts_handle_key(uint32_t state, uint32_t mods,
								 xkb_keysym_t sym, uint32_t time_msec) {
	/* Interactive picker: the next non-modifier press becomes the binding. */
	if (gs_pick.active) {
		if (state != WL_KEYBOARD_KEY_STATE_PRESSED)
			return true; /* swallow releases while capturing */
		if (sym == XKB_KEY_Escape) {
			gs_pick_finish(0, XKB_KEY_NoSymbol, true);
			return true;
		}
		if (gs_sym_is_modifier(sym))
			return true; /* wait for a real key */
		gs_pick_finish(mods, sym, false);
		return true; /* consume: the chosen key must not reach clients */
	}
	GlobalShortcutsSession *session;
	GlobalShortcut *shortcut;
	bool consumed = false;
	xkb_keysym_t lsym;

	if (!gs_bus || wl_list_empty(&gs_sessions))
		return false;
	lsym = xkb_keysym_to_lower(sym);

	wl_list_for_each(session, &gs_sessions, link) {
		wl_list_for_each(shortcut, &session->shortcuts, link) {
			if (!shortcut->has_trigger)
				continue;
			if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
				if (!shortcut->activated && shortcut->keysym == lsym &&
					shortcut->mods == (mods & GS_MOD_MASK)) {
					shortcut->activated = true;
					gs_emit_state(session, shortcut, true, time_msec);
					consumed = true;
				}
			} else if (shortcut->activated && shortcut->keysym == lsym) {
				shortcut->activated = false;
				gs_emit_state(session, shortcut, false, time_msec);
				consumed = true;
			}
		}
	}
	return consumed;
}

static int gs_bus_dispatch(int fd, uint32_t mask, void *data) {
	while (sd_bus_process(gs_bus, NULL) > 0)
		;
	return 0;
}

void global_shortcuts_portal_init(void) {
	int ret;

	wl_list_init(&gs_sessions);

	ret = sd_bus_default_user(&gs_bus);
	if (ret < 0) {
		wlr_log(WLR_ERROR, "global shortcuts: no session bus (%s)",
				strerror(-ret));
		gs_bus = NULL;
		return;
	}

	sd_bus_add_object_vtable(gs_bus, &gs_slot, GS_OBJ_PATH, GS_IFACE,
							 gs_vtable, &gs_portal_version);
	sd_bus_add_fallback_vtable(gs_bus, &gs_session_slot, GS_SESSION_PREFIX,
							   GS_SESSION_IFACE, gs_session_vtable, NULL,
							   &gs_portal_version);

	ret = sd_bus_request_name(gs_bus, GS_BUS_NAME, 0);
	if (ret < 0) {
		wlr_log(WLR_ERROR, "global shortcuts: cannot own %s (%s)", GS_BUS_NAME,
				strerror(-ret));
		sd_bus_unref(gs_bus);
		gs_bus = NULL;
		return;
	}

	gs_bus_source = wl_event_loop_add_fd(event_loop, sd_bus_get_fd(gs_bus),
										 WL_EVENT_READABLE, gs_bus_dispatch,
										 NULL);
	while (sd_bus_process(gs_bus, NULL) > 0)
		;
	wlr_log(WLR_INFO, "global shortcuts portal backend at %s", GS_BUS_NAME);
}

void global_shortcuts_portal_finish(void) {
	GlobalShortcutsSession *session, *tmp;

	if (!gs_bus)
		return;
	if (gs_pick.active)
		gs_pick_finish(0, XKB_KEY_NoSymbol, true);
	wl_list_for_each_safe(session, tmp, &gs_sessions, link)
		gs_session_destroy(session);
	if (gs_bus_source)
		wl_event_source_remove(gs_bus_source);
	sd_bus_slot_unref(gs_slot);
	sd_bus_slot_unref(gs_session_slot);
	sd_bus_release_name(gs_bus, GS_BUS_NAME);
	sd_bus_unref(gs_bus);
	gs_bus = NULL;
}
