/*
 * org.freedesktop.impl.portal.GlobalShortcuts backend, so portal-aware
 * clients (Discord, ashpd apps, ...) get global shortcuts / push-to-talk
 * on mango. Sessions and shortcuts are registered over D-Bus; matching
 * key presses/releases emit Activated/Deactivated and are consumed.
 *
 * Registered as the "mango" portal backend via
 * /usr/share/xdg-desktop-portal/portals/mango.portal.
 */
#include <strings.h>
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

static void gs_shortcut_destroy(GlobalShortcut *shortcut) {
	wl_list_remove(&shortcut->link);
	free(shortcut->id);
	free(shortcut->description);
	free(shortcut->trigger);
	free(shortcut);
}

static void gs_session_destroy(GlobalShortcutsSession *session) {
	GlobalShortcut *shortcut, *tmp;
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

		shortcut->has_trigger = gs_parse_trigger(
			shortcut->trigger, &shortcut->mods, &shortcut->keysym);
		if (!shortcut->has_trigger)
			wlr_log(WLR_INFO,
					"global shortcut '%s' has no usable trigger ('%s')",
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

	return gs_reply_shortcuts(msg, session);
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

/* Called from keypress(): returns true when the key was consumed by a
 * registered global shortcut. Push-to-talk relies on both edges: press
 * matches mods+keysym, release matches the keysym of an active shortcut
 * (modifiers may already have been released). */
bool global_shortcuts_handle_key(uint32_t state, uint32_t mods,
								 xkb_keysym_t sym, uint32_t time_msec) {
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
