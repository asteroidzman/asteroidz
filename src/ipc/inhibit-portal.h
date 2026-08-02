/*
 * org.freedesktop.impl.portal.Inhibit backend.
 *
 * The portal an application uses to say "not now": a video player asking the
 * screen not to blank, an installer asking not to be logged out from under
 * itself, a download asking the machine to stay awake. Without a backend for
 * it, xdg-desktop-portal answers those requests with "no such interface" and
 * every one of them silently does nothing -- which is how a full-screen video
 * ends up dimming halfway through on a desktop where the same app's Wayland
 * idle-inhibitor would have worked, because a portal-sandboxed app has no
 * Wayland idle-inhibitor to take.
 *
 * There are four flags, and asteroidz can honestly enforce two of them:
 *
 *   1 Logout       recorded, and SHOWN. The compositor has no session manager
 *                  to veto -- it IS the session -- but exiting already asks a
 *                  question (see quit() in dispatch/bind_define.h), and the
 *                  right answer to "an app says now is a bad time" is to put
 *                  that in front of the person answering, not to refuse them.
 *   2 User switch  recorded, not enforced. asteroidz has no user switching, so
 *                  there is nothing to inhibit and nothing to pretend about.
 *   4 Suspend      folded into idle inhibition -- see below.
 *   8 Idle         enforced: held against wlr_idle_notifier_v1 exactly like a
 *                  client's own idle-inhibitor, so everything downstream of
 *                  idling (blanking, locking, an idle daemon's suspend) is
 *                  held off for as long as the request lives.
 *
 * Suspend is deliberately the same lever as idle rather than a logind sleep
 * block. Idle-driven suspend is what an app asking not to be suspended almost
 * always means, and it is the half a compositor is genuinely in charge of.
 * Taking an inhibitor lock on logind instead would also override the user's
 * own `systemctl suspend` and their laptop lid -- so one leaked request from
 * one crashed application would turn closing the lid into a no-op, with
 * nothing on screen to say why. What is enforced is the part that can be
 * withdrawn by closing the app.
 *
 * Leaks are the failure mode worth engineering against, because an inhibition
 * nobody can see is a machine that quietly never sleeps. Three things clear
 * one: the Request's own Close, xdg-desktop-portal dropping off the bus
 * (watched here), and compositor shutdown. And `get idle` lists every live one
 * with the app that asked and the reason it gave, so it is never invisible.
 */
#include <systemd/sd-bus.h>

#define INHIBIT_IFACE "org.freedesktop.impl.portal.Inhibit"

#define INHIBIT_LOGOUT (1u << 0)
#define INHIBIT_USER_SWITCH (1u << 1)
#define INHIBIT_SUSPEND (1u << 2)
#define INHIBIT_IDLE (1u << 3)
#define INHIBIT_ALL_FLAGS                                                     \
	(INHIBIT_LOGOUT | INHIBIT_USER_SWITCH | INHIBIT_SUSPEND | INHIBIT_IDLE)

/* session-state, as the spec numbers it */
#define INHIBIT_STATE_RUNNING 1u
#define INHIBIT_STATE_QUERY_END 2u
#define INHIBIT_STATE_ENDING 3u

typedef struct PortalInhibition {
	struct wl_list link;
	char *handle; /* the Request object path xdg-desktop-portal chose */
	char *owner;  /* unique bus name that asked, so a death can drop it */
	char *app_id;
	char *reason;
	uint32_t flags;
	sd_bus_slot *slot; /* the Request object exported at `handle` */
} PortalInhibition;

typedef struct PortalInhibitMonitor {
	struct wl_list link;
	char *handle; /* the Session object path */
	char *owner;
	char *app_id;
} PortalInhibitMonitor;

static uint32_t inhibit_portal_version = 3;
static sd_bus_slot *inhibit_slot, *inhibit_owner_slot;
static struct wl_list inhibitions;
static struct wl_list inhibit_monitors;
static uint32_t inhibit_session_state = INHIBIT_STATE_RUNNING;
/* Bumped whenever the set of live inhibitions changes. `watch idle` pushes on
 * change, and the effective boolean is not enough to change on: a second app
 * taking an inhibitor while one already holds it moves nothing about whether
 * the machine sleeps, but it does move the list the bar draws. */
static uint32_t inhibit_generation;

static void inhibit_state_changed(void);

/* ── what the compositor asks this file ─────────────────────────────────── */

/* A wl_list is only walkable once wl_list_init has run, and everything below
 * is reachable from paths that do not care when the portals came up:
 * checkidleinhibitor() runs on every arrange, and the IPC answers whoever
 * asks. Cheaper to say "empty" here than to depend on an init order. */
static bool inhibit_lists_ready(void) { return inhibitions.next != NULL; }

/* Is anything holding idling off through the portal? Read by
 * checkidleinhibitor(), alongside the Wayland protocol's own inhibitors and
 * the manual flag. */
bool inhibit_portal_holds_idle(void) {
	PortalInhibition *inh;
	if (!inhibit_lists_ready())
		return false;
	wl_list_for_each(inh, &inhibitions, link) {
		if (inh->flags & (INHIBIT_IDLE | INHIBIT_SUSPEND))
			return true;
	}
	return false;
}

uint32_t inhibit_portal_generation(void) { return inhibit_generation; }

size_t inhibit_portal_count(void) {
	return inhibit_lists_ready() ? (size_t)wl_list_length(&inhibitions) : 0;
}

/* Indexed rather than exposing the list, so ipc.h can build JSON out of this
 * without the struct: ipc.h is included before this file is. */
bool inhibit_portal_get(size_t index, const char **app_id, const char **reason,
						uint32_t *flags) {
	PortalInhibition *inh;
	size_t i = 0;
	if (!inhibit_lists_ready())
		return false;
	wl_list_for_each(inh, &inhibitions, link) {
		if (i++ != index)
			continue;
		*app_id = inh->app_id ? inh->app_id : "";
		*reason = inh->reason ? inh->reason : "";
		*flags = inh->flags;
		return true;
	}
	return false;
}

/* ── the exported interface ─────────────────────────────────────────────── */

static void inhibition_destroy(PortalInhibition *inh) {
	wl_list_remove(&inh->link);
	if (inh->slot)
		sd_bus_slot_unref(inh->slot);
	free(inh->handle);
	free(inh->owner);
	free(inh->app_id);
	free(inh->reason);
	free(inh);
	inhibit_generation++;
}

static void inhibit_monitor_destroy(PortalInhibitMonitor *mon) {
	wl_list_remove(&mon->link);
	free(mon->handle);
	free(mon->owner);
	free(mon->app_id);
	free(mon);
}

static PortalInhibition *inhibition_find(const char *handle) {
	PortalInhibition *inh;
	wl_list_for_each(inh, &inhibitions, link) {
		if (strcmp(inh->handle, handle) == 0)
			return inh;
	}
	return NULL;
}

/* Close on the Request object: the application is done asking. This is the
 * ordinary end of an inhibition, and the only one an app controls. */
static int inhibit_handle_request_close(sd_bus_message *msg, void *data,
										sd_bus_error *err) {
	PortalInhibition *inh = inhibition_find(sd_bus_message_get_path(msg));
	if (inh) {
		wlr_log(WLR_INFO, "inhibit: %s released (flags 0x%x)",
				inh->app_id && *inh->app_id ? inh->app_id : "an application",
				inh->flags);
		inhibition_destroy(inh);
		checkidleinhibitor(NULL);
	}
	return sd_bus_reply_method_return(msg, "");
}

static const sd_bus_vtable inhibit_request_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("Close", "", "", inhibit_handle_request_close,
				  SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_VTABLE_END,
};

/* Read the one option the spec defines. Unknown keys are skipped rather than
 * refused: options is where this interface grows, and an app sending a key
 * from a newer spec must not have its inhibition rejected over it. */
static char *inhibit_read_reason(sd_bus_message *msg) {
	char *reason = NULL;
	if (sd_bus_message_enter_container(msg, 'a', "{sv}") < 0)
		return NULL;
	while (sd_bus_message_enter_container(msg, 'e', "sv") > 0) {
		const char *key = NULL;
		sd_bus_message_read(msg, "s", &key);
		if (key && strcmp(key, "reason") == 0) {
			const char *val = NULL;
			sd_bus_message_enter_container(msg, 'v', "s");
			sd_bus_message_read(msg, "s", &val);
			sd_bus_message_exit_container(msg);
			free(reason);
			reason = val ? strdup(val) : NULL;
		} else {
			sd_bus_message_skip(msg, "v");
		}
		sd_bus_message_exit_container(msg);
	}
	sd_bus_message_exit_container(msg);
	return reason;
}

static int inhibit_handle_inhibit(sd_bus_message *msg, void *data,
								  sd_bus_error *err) {
	const char *handle = NULL, *app_id = NULL, *window = NULL;
	uint32_t flags = 0;

	/* The trailing a{sv} is left in the message for inhibit_read_reason(). */
	if (sd_bus_message_read(msg, "ossu", &handle, &app_id, &window, &flags) < 0)
		return sd_bus_error_set(err, SD_BUS_ERROR_INVALID_ARGS,
								"malformed Inhibit call");

	if (flags == 0 || (flags & ~INHIBIT_ALL_FLAGS)) {
		/* The spec's flags are a closed set. A caller asking for a bit that
		 * does not exist has misunderstood something, and quietly honouring
		 * the bits we recognise would hide it from them. */
		return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS,
								 "unsupported inhibit flags 0x%x", flags);
	}
	if (inhibition_find(handle))
		return sd_bus_reply_method_return(msg, "");

	PortalInhibition *inh = ecalloc(1, sizeof(*inh));
	inh->handle = strdup(handle);
	const char *sender = sd_bus_message_get_sender(msg);
	inh->owner = sender ? strdup(sender) : NULL;
	inh->app_id = strdup(app_id ? app_id : "");
	inh->reason = inhibit_read_reason(msg);
	inh->flags = flags;
	wl_list_insert(&inhibitions, &inh->link);
	inhibit_generation++;

	/* The Request object IS the handle on the inhibition: the spec has no
	 * "uninhibit" call, only Close on this path. Exporting it after the entry
	 * is in the list means a Close racing the reply still finds it. */
	if (sd_bus_add_object_vtable(portal_bus, &inh->slot, handle,
								 PORTAL_REQUEST_IFACE, inhibit_request_vtable,
								 NULL) < 0) {
		inhibition_destroy(inh);
		return sd_bus_error_set(err, SD_BUS_ERROR_FAILED,
								"could not export the request object");
	}

	wlr_log(WLR_INFO, "inhibit: %s asked to block%s%s%s%s%s%s",
			*inh->app_id ? inh->app_id : "an application",
			flags & INHIBIT_LOGOUT ? " logout" : "",
			flags & INHIBIT_USER_SWITCH ? " user-switch" : "",
			flags & INHIBIT_SUSPEND ? " suspend" : "",
			flags & INHIBIT_IDLE ? " idle" : "", inh->reason ? " -- " : "",
			inh->reason ? inh->reason : "");

	checkidleinhibitor(NULL);
	return sd_bus_reply_method_return(msg, "");
}

static int inhibit_handle_create_monitor(sd_bus_message *msg, void *data,
										 sd_bus_error *err) {
	const char *handle = NULL, *session_handle = NULL, *app_id = NULL,
			   *window = NULL;
	if (sd_bus_message_read(msg, "ooss", &handle, &session_handle, &app_id,
							&window) < 0)
		return sd_bus_error_set(err, SD_BUS_ERROR_INVALID_ARGS,
								"malformed CreateMonitor call");

	PortalInhibitMonitor *mon = ecalloc(1, sizeof(*mon));
	mon->handle = strdup(session_handle);
	const char *sender = sd_bus_message_get_sender(msg);
	mon->owner = sender ? strdup(sender) : NULL;
	mon->app_id = strdup(app_id ? app_id : "");
	wl_list_insert(&inhibit_monitors, &mon->link);
	wlr_log(WLR_INFO, "inhibit: %s is watching the session state",
			*mon->app_id ? mon->app_id : "an application");

	int ret = sd_bus_reply_method_return(msg, "u", 0u);
	/* The spec says a monitor "will receive StateChanged signals with updates
	 * on the session state" -- with no separate way to ask what the state is
	 * right now. So the first update is sent immediately: a monitor created
	 * while the screen is already locked otherwise believes it is not until
	 * the next unlock, and reports the opposite of the truth for as long as
	 * the lock lasts. */
	inhibit_state_changed();
	return ret;
}

/* The acknowledgement a monitor sends after a Query End. asteroidz does not
 * gate anything on it, and says so rather than implying a timeout it does not
 * run: Query End here means a person is looking at the exit prompt, and the
 * exit waits for their keypress, not for a bus round trip. The ack is logged
 * because "did the app even hear us" is the question it can answer. */
static int inhibit_handle_query_end_response(sd_bus_message *msg, void *data,
											 sd_bus_error *err) {
	const char *session_handle = NULL;
	if (sd_bus_message_read(msg, "o", &session_handle) < 0)
		return sd_bus_error_set(err, SD_BUS_ERROR_INVALID_ARGS,
								"malformed QueryEndResponse call");
	PortalInhibitMonitor *mon;
	wl_list_for_each(mon, &inhibit_monitors, link) {
		if (strcmp(mon->handle, session_handle) == 0) {
			wlr_log(WLR_DEBUG, "inhibit: %s acknowledged the query-end",
					*mon->app_id ? mon->app_id : "an application");
			break;
		}
	}
	return sd_bus_reply_method_return(msg, "");
}

static const sd_bus_vtable inhibit_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("Inhibit", "ossua{sv}", "", inhibit_handle_inhibit,
				  SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("CreateMonitor", "ooss", "u", inhibit_handle_create_monitor,
				  SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("QueryEndResponse", "o", "", inhibit_handle_query_end_response,
				  SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_SIGNAL("StateChanged", "oa{sv}", 0),
	SD_BUS_PROPERTY("version", "u", NULL, 0, SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_VTABLE_END,
};

static bool inhibit_monitor_close(const char *path) {
	PortalInhibitMonitor *mon;
	wl_list_for_each(mon, &inhibit_monitors, link) {
		if (strcmp(mon->handle, path) != 0)
			continue;
		wlr_log(WLR_INFO, "inhibit: %s stopped watching the session state",
				*mon->app_id ? mon->app_id : "an application");
		inhibit_monitor_destroy(mon);
		return true;
	}
	return false;
}

/* ── pushing state out ──────────────────────────────────────────────────── */

/* One StateChanged per monitoring session. Both fields go in every signal:
 * the vardict is a snapshot, not a delta, and a monitor that only ever hears
 * about the field that moved has to remember the other one across a signal it
 * may have subscribed too late to see. */
static void inhibit_state_changed(void) {
	PortalInhibitMonitor *mon;
	if (!portal_bus || wl_list_empty(&inhibit_monitors))
		return;
	wl_list_for_each(mon, &inhibit_monitors, link) {
		sd_bus_message *sig = NULL;
		if (sd_bus_message_new_signal(portal_bus, &sig, PORTAL_OBJ_PATH,
									  INHIBIT_IFACE, "StateChanged") < 0)
			continue;
		sd_bus_message_append(sig, "o", mon->handle);
		sd_bus_message_open_container(sig, 'a', "{sv}");
		sd_bus_message_open_container(sig, 'e', "sv");
		sd_bus_message_append(sig, "s", "screensaver-active");
		sd_bus_message_append(sig, "v", "b", locked ? 1 : 0);
		sd_bus_message_close_container(sig);
		sd_bus_message_open_container(sig, 'e', "sv");
		sd_bus_message_append(sig, "s", "session-state");
		sd_bus_message_append(sig, "v", "u", inhibit_session_state);
		sd_bus_message_close_container(sig);
		sd_bus_message_close_container(sig);
		sd_bus_send(NULL, sig, NULL);
		sd_bus_message_unref(sig);
	}
	sd_bus_flush(portal_bus);
}

/* The session lock came up or went away. Called from locksession() and
 * destroylock(); `locked` is already current when it runs. */
void inhibit_portal_screensaver_changed(void) { inhibit_state_changed(); }

/* Running / Query End / Ending. The exit prompt drives the first two, and
 * shutdown drives the last. */
void inhibit_portal_set_session_state(uint32_t state) {
	if (inhibit_session_state == state)
		return;
	inhibit_session_state = state;
	inhibit_state_changed();
}

/* ── who is holding the session open, in words ──────────────────────────── */

/* The exit prompt is Pango markup, and both of these strings come from an
 * application: an app_id with a `<` in it would be parsed rather than shown,
 * or refused outright, taking the whole prompt with it -- the same trap the
 * global-shortcuts picker documents. */
static void inhibit_markup_escape(const char *in, char *out, size_t cap) {
	size_t o = 0;
	for (const char *p = in; *p && o + 7 < cap; p++) {
		const char *rep = NULL;
		switch (*p) {
		case '<': rep = "&lt;"; break;
		case '>': rep = "&gt;"; break;
		case '&': rep = "&amp;"; break;
		case '"': rep = "&quot;"; break;
		default: break;
		}
		if (rep) {
			size_t n = strlen(rep);
			memcpy(out + o, rep, n);
			o += n;
		} else {
			out[o++] = *p;
		}
	}
	out[o] = '\0';
}

/* One line for the exit prompt naming who asked not to be logged out, or
 * false when nobody did. Markup-escaped; see above.
 *
 * It names them rather than just counting: "an application is busy" is a
 * sentence that cannot be acted on, and the whole value of honouring the
 * logout flag at all is that the person about to end the session finds out
 * which window they have not saved. */
bool inhibit_portal_logout_summary(char *buf, size_t cap) {
	PortalInhibition *inh;
	char names[3][128];
	size_t n = 0, extra = 0;
	const char *reason = NULL;

	if (!inhibit_lists_ready())
		return false;
	wl_list_for_each(inh, &inhibitions, link) {
		if (!(inh->flags & INHIBIT_LOGOUT))
			continue;
		if (n < 3) {
			const char *who = inh->app_id && *inh->app_id ? inh->app_id
														  : "an application";
			inhibit_markup_escape(who, names[n], sizeof(names[n]));
			if (n == 0 && inh->reason && *inh->reason)
				reason = inh->reason;
			n++;
		} else {
			extra++;
		}
	}
	if (n == 0)
		return false;

	char tail[192] = "";
	if (n == 1 && reason) {
		char esc[160];
		inhibit_markup_escape(reason, esc, sizeof(esc));
		snprintf(tail, sizeof(tail), " (%s)", esc);
	} else if (extra) {
		snprintf(tail, sizeof(tail), " and %zu more", extra);
	}

	if (n == 1)
		snprintf(buf, cap, "%s asked not to be interrupted%s", names[0], tail);
	else if (n == 2)
		snprintf(buf, cap, "%s and %s asked not to be interrupted%s", names[0],
				 names[1], tail);
	else
		snprintf(buf, cap, "%s, %s and %s asked not to be interrupted%s",
				 names[0], names[1], names[2], tail);
	return true;
}

/* ── owners that go away ────────────────────────────────────────────────── */

/* Every call here arrives from xdg-desktop-portal, which owns the lifetime of
 * both the Request and the Session objects and closes them when the app that
 * asked disconnects. If xdp itself dies, nothing ever closes them -- and an
 * idle inhibition that outlives the process that asked for it is a machine
 * that never sleeps again this session, with no window on screen to blame.
 * So the owner's death is watched directly. */
static int inhibit_name_owner_changed(sd_bus_message *msg, void *data,
									  sd_bus_error *err) {
	const char *name = NULL, *old_owner = NULL, *new_owner = NULL;
	if (sd_bus_message_read(msg, "sss", &name, &old_owner, &new_owner) < 0)
		return 0;
	if (!name || (new_owner && *new_owner))
		return 0; /* acquired or handed over, not lost */

	PortalInhibition *inh, *itmp;
	PortalInhibitMonitor *mon, *mtmp;
	bool dropped = false;
	wl_list_for_each_safe(inh, itmp, &inhibitions, link) {
		if (inh->owner && strcmp(inh->owner, name) == 0) {
			wlr_log(WLR_INFO,
					"inhibit: dropping %s's inhibition -- the portal it came "
					"through is gone",
					*inh->app_id ? inh->app_id : "an application");
			inhibition_destroy(inh);
			dropped = true;
		}
	}
	wl_list_for_each_safe(mon, mtmp, &inhibit_monitors, link) {
		if (mon->owner && strcmp(mon->owner, name) == 0)
			inhibit_monitor_destroy(mon);
	}
	if (dropped)
		checkidleinhibitor(NULL);
	return 0;
}

/* ── lifecycle ──────────────────────────────────────────────────────────── */

void inhibit_portal_init(void) {
	wl_list_init(&inhibitions);
	wl_list_init(&inhibit_monitors);
	if (!portal_bus)
		return;
	sd_bus_add_object_vtable(portal_bus, &inhibit_slot, PORTAL_OBJ_PATH,
							 INHIBIT_IFACE, inhibit_vtable,
							 &inhibit_portal_version);
	portal_bus_add_session_closer(inhibit_monitor_close);
	sd_bus_match_signal(portal_bus, &inhibit_owner_slot, "org.freedesktop.DBus",
						"/org/freedesktop/DBus", "org.freedesktop.DBus",
						"NameOwnerChanged", inhibit_name_owner_changed, NULL);
}

void inhibit_portal_finish(void) {
	PortalInhibition *inh, *itmp;
	PortalInhibitMonitor *mon, *mtmp;

	if (!portal_bus)
		return;
	/* Ending, while the bus is still up. A monitor that learns the session is
	 * over only by its connection dropping cannot tell that from a crash. */
	inhibit_portal_set_session_state(INHIBIT_STATE_ENDING);

	wl_list_for_each_safe(inh, itmp, &inhibitions, link)
		inhibition_destroy(inh);
	wl_list_for_each_safe(mon, mtmp, &inhibit_monitors, link)
		inhibit_monitor_destroy(mon);
	sd_bus_slot_unref(inhibit_owner_slot);
	inhibit_owner_slot = NULL;
	sd_bus_slot_unref(inhibit_slot);
	inhibit_slot = NULL;
}
