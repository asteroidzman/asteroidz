/*
 * The one connection and the one bus name asteroidz's portal backends share.
 *
 * A .portal file names a single D-Bus service and lists every interface
 * xdg-desktop-portal should route to it, so a compositor implementing two
 * backends does not get two names to own -- it gets one, with two interfaces
 * exported on the same object. Each backend owning its own sd_bus and calling
 * sd_bus_request_name() would mean the second one loses the race and silently
 * does nothing, which is the failure this file exists to make impossible.
 *
 * Ordering is the other reason it is separate. The name must be taken AFTER
 * every vtable is registered: xdg-desktop-portal watches for the name and can
 * call a method the moment it appears, and a method arriving at a path nothing
 * is exported on is an error reply the client has no reason to expect. So
 * bringing the portals up is three steps -- connect, let each backend export
 * itself, then publish -- rather than one per backend.
 *
 * The Session interface is shared for the same reason as the name. Sessions
 * live under one path prefix chosen by xdg-desktop-portal, not by us, so a
 * per-backend fallback vtable on that prefix would collide; instead there is
 * one Close handler here and each backend registers a closer that recognises
 * its own paths.
 */
#include <systemd/sd-bus.h>

#define PORTAL_BUS_NAME "org.freedesktop.impl.portal.desktop.asteroidz"
#define PORTAL_OBJ_PATH "/org/freedesktop/portal/desktop"
#define PORTAL_SESSION_IFACE "org.freedesktop.impl.portal.Session"
#define PORTAL_REQUEST_IFACE "org.freedesktop.impl.portal.Request"
#define PORTAL_SESSION_PREFIX "/org/freedesktop/portal/desktop/session"

static sd_bus *portal_bus;
static struct wl_event_source *portal_bus_source;
static sd_bus_slot *portal_session_slot;
static uint32_t portal_session_version = 1;

/* Close the session at `path`, if it is mine. Returning false means "not
 * mine", not "failed" -- the next backend gets asked. */
typedef bool (*portal_session_close_fn)(const char *path);
static portal_session_close_fn portal_session_closers[4];
static size_t portal_n_session_closers;

static void portal_bus_add_session_closer(portal_session_close_fn fn) {
	if (portal_n_session_closers <
		sizeof(portal_session_closers) / sizeof(portal_session_closers[0]))
		portal_session_closers[portal_n_session_closers++] = fn;
}

static int portal_handle_session_close(sd_bus_message *msg, void *data,
									   sd_bus_error *err) {
	const char *path = sd_bus_message_get_path(msg);
	for (size_t i = 0; i < portal_n_session_closers; i++) {
		if (portal_session_closers[i](path))
			break;
	}
	/* Reply either way. A session the compositor has already forgotten -- a
	 * client that closed twice, or one dropped when its owner vanished -- is
	 * not an error the caller can do anything about, and an error reply here
	 * is one xdg-desktop-portal logs and passes on to an application that was
	 * only tidying up. */
	return sd_bus_reply_method_return(msg, "");
}

static const sd_bus_vtable portal_session_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("Close", "", "", portal_handle_session_close,
				  SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_PROPERTY("version", "u", NULL, 0, SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_VTABLE_END,
};

static int portal_bus_dispatch(int fd, uint32_t mask, void *data) {
	while (sd_bus_process(portal_bus, NULL) > 0)
		;
	return 0;
}

/* Step one: the connection and the shared Session object. False means there is
 * no session bus, in which case no backend should try to export anything. */
static bool portal_bus_connect(void) {
	int ret = sd_bus_default_user(&portal_bus);
	if (ret < 0) {
		wlr_log(WLR_INFO, "portals: no session bus (%s), backends disabled",
				strerror(-ret));
		portal_bus = NULL;
		return false;
	}
	portal_n_session_closers = 0;
	sd_bus_add_fallback_vtable(portal_bus, &portal_session_slot,
							   PORTAL_SESSION_PREFIX, PORTAL_SESSION_IFACE,
							   portal_session_vtable, NULL,
							   &portal_session_version);
	return true;
}

/* Step three: take the name and start dispatching. Losing the name is not
 * fatal and not even unusual -- it means another backend for this desktop is
 * already running -- so it drops the connection and leaves everything the
 * backends registered inert rather than half-live. */
static void portal_bus_publish(void) {
	if (!portal_bus)
		return;
	int ret = sd_bus_request_name(portal_bus, PORTAL_BUS_NAME, 0);
	if (ret < 0) {
		wlr_log(WLR_ERROR, "portals: cannot own %s (%s)", PORTAL_BUS_NAME,
				strerror(-ret));
		sd_bus_slot_unref(portal_session_slot);
		portal_session_slot = NULL;
		sd_bus_unref(portal_bus);
		portal_bus = NULL;
		return;
	}
	portal_bus_source =
		wl_event_loop_add_fd(event_loop, sd_bus_get_fd(portal_bus),
							 WL_EVENT_READABLE, portal_bus_dispatch, NULL);
	while (sd_bus_process(portal_bus, NULL) > 0)
		;
	wlr_log(WLR_INFO, "portal backends published at %s", PORTAL_BUS_NAME);
}

static void portal_bus_finish(void) {
	if (!portal_bus)
		return;
	if (portal_bus_source) {
		wl_event_source_remove(portal_bus_source);
		portal_bus_source = NULL;
	}
	sd_bus_slot_unref(portal_session_slot);
	portal_session_slot = NULL;
	portal_n_session_closers = 0;
	sd_bus_release_name(portal_bus, PORTAL_BUS_NAME);
	sd_bus_unref(portal_bus);
	portal_bus = NULL;
}
