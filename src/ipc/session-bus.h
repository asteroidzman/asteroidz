/* Session D-Bus connection (sd-bus), pumped by the Wayland event loop.
 * The compositor never spawns a bus -- owning one is the login
 * environment's job. On systemd systems the user bus always exists at
 * $XDG_RUNTIME_DIR/bus and sd_bus_open_user() finds it even when
 * DBUS_SESSION_BUS_ADDRESS is unset (unlike set_activation_env, which
 * needs the variable). Currently carries fire-and-forget
 * org.freedesktop.Notifications calls (screenshot_ui). */
#include <systemd/sd-bus.h>

static sd_bus *session_bus;
static struct wl_event_source *session_bus_source;

static void session_bus_finish(void) {
	if (session_bus_source) {
		wl_event_source_remove(session_bus_source);
		session_bus_source = NULL;
	}
	if (session_bus) {
		sd_bus_flush_close_unref(session_bus);
		session_bus = NULL;
	}
}

static int session_bus_dispatch(int fd, uint32_t mask, void *data) {
	int r;
	while ((r = sd_bus_process(session_bus, NULL)) > 0)
		;
	if (r < 0) {
		wlr_log(WLR_ERROR, "session bus disconnected: %s", strerror(-r));
		session_bus_finish();
	}
	return 0;
}

static void session_bus_init(void) {
	int r = sd_bus_open_user(&session_bus);
	if (r < 0) {
		wlr_log(WLR_INFO, "no session bus, notifications disabled: %s",
				strerror(-r));
		session_bus = NULL;
		return;
	}
	session_bus_source =
		wl_event_loop_add_fd(event_loop, sd_bus_get_fd(session_bus),
							 WL_EVENT_READABLE, session_bus_dispatch, NULL);
	if (!session_bus_source) {
		wlr_log(WLR_ERROR, "failed to watch session bus fd");
		sd_bus_flush_close_unref(session_bus);
		session_bus = NULL;
	}
}

static int notify_send_reply(sd_bus_message *m, void *userdata,
							 sd_bus_error *ret_error) {
	/* an error reply IS the "no notification daemon" case (ServiceUnknown
	 * when nothing owns org.freedesktop.Notifications) -- the action the
	 * notification reported on already succeeded, so debug only */
	const sd_bus_error *e = sd_bus_message_get_error(m);
	if (e)
		wlr_log(WLR_DEBUG, "notification not delivered: %s", e->name);
	return 0;
}

/* Fire-and-forget desktop notification straight over the bus (what
 * notify-send would do, minus the fork/exec and shell quoting). icon may
 * be an absolute image path or a themed icon name, or NULL. */
static void notify_send(const char *summary, const char *body,
						const char *icon) {
	if (!session_bus)
		return;

	int r = sd_bus_call_method_async(
		session_bus, NULL, "org.freedesktop.Notifications",
		"/org/freedesktop/Notifications", "org.freedesktop.Notifications",
		"Notify", notify_send_reply, NULL, "susssasa{sv}i", "asteroidz",
		(uint32_t)0 /* replaces_id */, icon ? icon : "", summary, body,
		0 /* no actions */, 0 /* no hints */, (int32_t)-1 /* default expiry */);
	if (r < 0) {
		wlr_log(WLR_DEBUG, "notify: %s", strerror(-r));
		return;
	}
	/* async call is only queued; push it out now (local socket, instant) */
	sd_bus_flush(session_bus);
}
