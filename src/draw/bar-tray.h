#ifndef ASTEROIDZ_BAR_TRAY_H
#define ASTEROIDZ_BAR_TRAY_H

/* StatusNotifierItem tray, on the session bus the compositor already owns and
 * already pumps from the Wayland event loop (ipc/session-bus.h).
 *
 * Two roles, and which one we get is decided at runtime:
 *
 *   WATCHER  We own org.kde.StatusNotifierWatcher and applications register
 *            their items with us directly. This is the normal case once the
 *            native bar has replaced waybar.
 *
 *   CLIENT   Something else (waybar's tray, or a leftover snixembed) already
 *            owns that name. Grabbing it away is not possible and queueing for
 *            it would leave the tray dead until the other process exits, so we
 *            instead register as a plain HOST with the incumbent watcher and
 *            mirror its item list. Both bars then show the same tray, which is
 *            exactly what is wanted while migrating from one to the other.
 *
 * Every bus call is async. sd_bus_call() blocks for up to 25 seconds by
 * default, and a wedged tray application must never be able to stall the
 * compositor -- this is the same rule the media module follows.
 *
 * NOT implemented: the DBusMenu context menu. It needs a popup surface with
 * keyboard grab and its own hit-testing, which the bar has no layer for yet;
 * right-click falls back to the item's own SecondaryActivate, which is what
 * most applications map to "show me the menu" anyway.
 */

#include <systemd/sd-bus.h>

#define BAR_TRAY_MAX_ITEMS 16
#define BAR_TRAY_WATCHER_NAME "org.kde.StatusNotifierWatcher"
#define BAR_TRAY_WATCHER_PATH "/StatusNotifierWatcher"
#define BAR_TRAY_WATCHER_IFACE "org.kde.StatusNotifierWatcher"
#define BAR_TRAY_ITEM_IFACE "org.kde.StatusNotifierItem"
/* The same interface under the name the freedesktop draft uses. Nothing in the
 * wild asks for it today -- every real client (Steam, Discord, Electron's
 * appindicator, nm-applet, blueman) looks up the org.kde.* name, which is a
 * plain bus name and needs no part of KDE installed or running. Owning both
 * costs one extra name and one extra vtable export, and means a client that
 * ever does prefer the vendor-neutral spelling finds us. */
#define BAR_TRAY_FDO_WATCHER_NAME "org.freedesktop.StatusNotifierWatcher"
#define BAR_TRAY_FDO_WATCHER_IFACE "org.freedesktop.StatusNotifierWatcher"

typedef struct {
	char service[128]; /* unique or well-known bus name of the item */
	char path[128];    /* object path, usually /StatusNotifierItem */
	char id[128];      /* Id property: stable, app-chosen */
	char title[192];   /* Title property: what the tooltip would say */
	char status[32];   /* Passive / Active / NeedsAttention */
	char icon_key[192]; /* what to hand _set_icon: a name or a cache key */
	/* Object path of the item's com.canonical.dbusmenu, from the SNI `Menu`
	 * property. Empty when the item ships no menu, which is the case that
	 * still falls back to SecondaryActivate. */
	char menu_path[128];
	/* Properties are fetched once when the item registers, but an application
	 * can register before it is ready to answer -- quickshell's does -- and a
	 * reply that arrives empty leaves the item with no artwork and no reason
	 * to ever ask again. Bounded retries, cleared as soon as anything renders. */
	int32_t icon_retries;
	bool used;
} BarTrayItem;

static BarTrayItem bar_tray_items[BAR_TRAY_MAX_ITEMS];
static int32_t bar_tray_nitems = 0;
static sd_bus_slot *bar_tray_watcher_slot = NULL;  /* our vtable, watcher role */
static sd_bus_slot *bar_tray_fdo_slot = NULL;      /* same vtable, fd.o name */
static bool bar_tray_is_fdo_watcher = false;
static sd_bus_slot *bar_tray_signal_slots[4] = {0};
static bool bar_tray_is_watcher = false;
static bool bar_tray_started = false;
static char bar_tray_host_name[128];

static void bar_update_all(void);

/* Emit a watcher signal on BOTH interface names, so a listener bound to
 * either spelling sees it. */
static void bar_tray_emit(const char *member, const char *sig,
						  const char *arg) {
	if (!session_bus)
		return;
	if (bar_tray_is_watcher) {
		if (sig && arg)
			sd_bus_emit_signal(session_bus, BAR_TRAY_WATCHER_PATH,
							   BAR_TRAY_WATCHER_IFACE, member, sig, arg);
		else
			sd_bus_emit_signal(session_bus, BAR_TRAY_WATCHER_PATH,
							   BAR_TRAY_WATCHER_IFACE, member, "");
	}
	if (bar_tray_is_fdo_watcher) {
		if (sig && arg)
			sd_bus_emit_signal(session_bus, BAR_TRAY_WATCHER_PATH,
							   BAR_TRAY_FDO_WATCHER_IFACE, member, sig, arg);
		else
			sd_bus_emit_signal(session_bus, BAR_TRAY_WATCHER_PATH,
							   BAR_TRAY_FDO_WATCHER_IFACE, member, "");
	}
}

/* ─── item table ──────────────────────────────────────────────────────────── */

static BarTrayItem *bar_tray_find(const char *service) {
	if (!service || !*service)
		return NULL;
	for (int32_t i = 0; i < bar_tray_nitems; i++)
		if (bar_tray_items[i].used &&
			strcmp(bar_tray_items[i].service, service) == 0)
			return &bar_tray_items[i];
	return NULL;
}

static BarTrayItem *bar_tray_add(const char *service, const char *path) {
	BarTrayItem *it = bar_tray_find(service);
	if (it)
		return it;
	if (bar_tray_nitems >= BAR_TRAY_MAX_ITEMS) {
		wlr_log(WLR_INFO, "tray: ignoring %s, already tracking %d items",
				service, BAR_TRAY_MAX_ITEMS);
		return NULL;
	}
	it = &bar_tray_items[bar_tray_nitems++];
	*it = (BarTrayItem){0};
	it->used = true;
	snprintf(it->service, sizeof(it->service), "%s", service);
	snprintf(it->path, sizeof(it->path), "%s",
			 path && *path ? path : "/StatusNotifierItem");
	return it;
}

static void bar_tray_remove(const char *service) {
	if (!service || !*service)
		return;
	/* accepts the same three spellings bar_tray_register does: a plain bus
	 * name (what NameOwnerChanged gives) or a name with the object path glued
	 * on (what a watcher's Unregistered signal gives) */
	char key[128];
	const char *slash = service[0] == '/' ? NULL : strchr(service, '/');
	if (slash) {
		size_t n = (size_t)(slash - service);
		if (n >= sizeof(key))
			n = sizeof(key) - 1;
		memcpy(key, service, n);
		key[n] = '\0';
		service = key;
	}
	BarTrayItem *it = bar_tray_find(service);
	if (!it)
		return;
	bar_tray_emit("StatusNotifierItemUnregistered", "s", service);
	int32_t idx = (int32_t)(it - bar_tray_items);
	/* compact rather than leaving a hole: the pills are laid out by index and
	 * a gap would render as a blank slot in the middle of the tray */
	for (int32_t i = idx; i + 1 < bar_tray_nitems; i++)
		bar_tray_items[i] = bar_tray_items[i + 1];
	bar_tray_items[--bar_tray_nitems] = (BarTrayItem){0};
	bar_update_all();
}

/* ─── property reading ────────────────────────────────────────────────────── */

/* Pick the pixmap closest to (but not below) the size we draw at, falling back
 * to the largest available. The array is a(iiay): width, height, ARGB32 bytes,
 * and applications commonly ship 16/22/24/32/48 all at once. */
static bool bar_tray_read_pixmap(sd_bus_message *m, BarTrayItem *it) {
	if (sd_bus_message_enter_container(m, 'a', "(iiay)") <= 0)
		return false;

	const uint8_t *best = NULL;
	size_t best_len = 0;
	int32_t best_w = 0, best_h = 0;
	bool ok = false;

	while (sd_bus_message_enter_container(m, 'r', "iiay") > 0) {
		int32_t w = 0, h = 0;
		const void *data = NULL;
		size_t len = 0;
		if (sd_bus_message_read(m, "ii", &w, &h) > 0 &&
			sd_bus_message_read_array(m, 'y', &data, &len) > 0 && w > 0 &&
			h > 0 && data && len >= (size_t)w * h * 4) {
			/* prefer the smallest that still covers the draw size, else the
			 * biggest we saw -- upscaling a 16px icon to 28 looks far worse
			 * than downscaling a 32px one */
			bool better = !best || (best_w < 32 && w > best_w) ||
						  (w >= 32 && best_w >= 32 && w < best_w);
			if (better) {
				best = data;
				best_len = len;
				best_w = w;
				best_h = h;
			}
		}
		sd_bus_message_exit_container(m);
	}
	sd_bus_message_exit_container(m);

	if (best && best_len) {
		/* keyed on the bus name so a second item cannot collide with this
		 * one's pixels, and so a NewIcon simply overwrites in place */
		char key[192];
		snprintf(key, sizeof(key), "sni-pixmap:%s", it->service);
		if (asteroidz_icon_cache_put_argb32(key, best, best_w, best_h)) {
			snprintf(it->icon_key, sizeof(it->icon_key), "%s", key);
			ok = true;
		}
	}
	return ok;
}

/* GetAll(org.kde.StatusNotifierItem) reply. Everything is optional: an item
 * that answers with nothing useful still gets a pill, drawn with its Id as the
 * label, rather than disappearing. */
static int bar_tray_on_props(sd_bus_message *m, void *user, sd_bus_error *err) {
	(void)err;
	char *service = user;
	BarTrayItem *it = bar_tray_find(service);
	if (!it || !m || sd_bus_message_is_method_error(m, NULL)) {
		free(service);
		return 0;
	}

	char icon_name[128] = {0};
	char icon_theme_path[256] = {0};
	char attention_icon[128] = {0};
	bool have_pixmap = false;

	if (sd_bus_message_enter_container(m, 'a', "{sv}") > 0) {
		while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
			const char *key = NULL;
			if (sd_bus_message_read(m, "s", &key) <= 0 || !key) {
				sd_bus_message_exit_container(m);
				break;
			}
			char *dst = NULL;
			size_t dstlen = 0;
			if (!strcmp(key, "Id")) {
				dst = it->id;
				dstlen = sizeof(it->id);
			} else if (!strcmp(key, "Title")) {
				dst = it->title;
				dstlen = sizeof(it->title);
			} else if (!strcmp(key, "Status")) {
				dst = it->status;
				dstlen = sizeof(it->status);
			} else if (!strcmp(key, "IconName")) {
				dst = icon_name;
				dstlen = sizeof(icon_name);
			} else if (!strcmp(key, "AttentionIconName")) {
				dst = attention_icon;
				dstlen = sizeof(attention_icon);
			} else if (!strcmp(key, "IconThemePath")) {
				dst = icon_theme_path;
				dstlen = sizeof(icon_theme_path);
			}

			/* Menu is an object path ('o'), not a string, so it needs its own
			 * variant type -- read as "s" it silently came back empty and
			 * every item looked like it had no menu. */
			if (!dst && !strcmp(key, "Menu")) {
				const char *mp = NULL;
				if (sd_bus_message_enter_container(m, 'v', "o") > 0) {
					if (sd_bus_message_read(m, "o", &mp) > 0 && mp)
						snprintf(it->menu_path, sizeof(it->menu_path), "%s", mp);
					sd_bus_message_exit_container(m);
				} else {
					sd_bus_message_skip(m, "v");
				}
				sd_bus_message_exit_container(m);
				continue;
			}

			if (dst) {
				const char *v = NULL;
				if (sd_bus_message_enter_container(m, 'v', "s") > 0) {
					if (sd_bus_message_read(m, "s", &v) > 0 && v)
						snprintf(dst, dstlen, "%s", v);
					sd_bus_message_exit_container(m);
				} else {
					sd_bus_message_skip(m, "v");
				}
			} else if (!strcmp(key, "IconPixmap") ||
					   !strcmp(key, "AttentionIconPixmap")) {
				if (sd_bus_message_enter_container(m, 'v', "a(iiay)") > 0) {
					/* a name always wins: it is themed, scalable, and follows
					 * the user's icon theme, where a pixmap is whatever size
					 * the app happened to ship */
					if (!have_pixmap && bar_tray_read_pixmap(m, it))
						have_pixmap = true;
					sd_bus_message_exit_container(m);
				} else {
					sd_bus_message_skip(m, "v");
				}
			} else {
				sd_bus_message_skip(m, "v");
			}
			sd_bus_message_exit_container(m);
		}
		sd_bus_message_exit_container(m);
	}

	/* NeedsAttention swaps in the attention artwork, which is the whole point
	 * of that status (an unread badge, a pending update) */
	const char *want = icon_name;
	if (attention_icon[0] && !strcmp(it->status, "NeedsAttention"))
		want = attention_icon;

	if (want && *want) {
		if (icon_theme_path[0]) {
			/* Items shipping their own theme dir (Electron apps, Steam) name
			 * an icon that is in NO installed theme, so the normal lookup
			 * cannot find it. Try the directory they gave us as a plain file
			 * first; resolve_icon_path takes absolute paths as-is. */
			static const char *exts[] = {"png", "svg"};
			bool found = false;
			for (size_t e = 0; e < LENGTH(exts) && !found; e++) {
				char abs[512];
				snprintf(abs, sizeof(abs), "%s/%s.%s", icon_theme_path, want,
						 exts[e]);
				if (access(abs, R_OK) == 0) {
					snprintf(it->icon_key, sizeof(it->icon_key), "%s", abs);
					found = true;
				}
			}
			if (!found && !have_pixmap)
				snprintf(it->icon_key, sizeof(it->icon_key), "%s", want);
		} else {
			snprintf(it->icon_key, sizeof(it->icon_key), "%s", want);
		}
	} else if (!have_pixmap) {
		it->icon_key[0] = '\0';
	}

	/* Nothing to draw yet: ask again shortly rather than leaving a permanently
	 * blank slot. An item that genuinely has no artwork stops after a few
	 * tries and falls back to its Id in the pill. */
	if (!it->icon_key[0] && it->icon_retries < 5)
		it->icon_retries++;
	else if (it->icon_key[0])
		it->icon_retries = 0;

	free(service);
	bar_update_all();
	return 0;
}


static void bar_tray_fetch_props(BarTrayItem *it) {
	if (!session_bus || !it)
		return;
	/* the callback runs after this item may have been removed, so it looks the
	 * item back up by name rather than holding a pointer into the table */
	char *service = strdup(it->service);
	if (!service)
		return;
	int r = sd_bus_call_method_async(
		session_bus, NULL, it->service, it->path,
		"org.freedesktop.DBus.Properties", "GetAll", bar_tray_on_props, service,
		"s", BAR_TRAY_ITEM_IFACE);
	if (r < 0) {
		wlr_log(WLR_DEBUG, "tray: GetAll on %s: %s", it->service, strerror(-r));
		free(service);
		return;
	}
	sd_bus_flush(session_bus);
}

/* Re-ask any item that still has no artwork. Driven from the bar's existing
 * metrics tick, so it costs one bus call per unresolved item per interval and
 * nothing at all once everything has an icon. */
static void bar_tray_retry_icons(void) {
	for (int32_t i = 0; i < bar_tray_nitems; i++) {
		BarTrayItem *it = &bar_tray_items[i];
		if (it->used && !it->icon_key[0] && it->icon_retries > 0 &&
			it->icon_retries < 5)
			bar_tray_fetch_props(it);
	}
}

/* ─── item lifecycle ──────────────────────────────────────────────────────── */

/* The spec lets an application register either its bus name or an object path,
 * and real applications do both. A path means "the item lives at this path on
 * *my* connection", so the sender is the service. */
static void bar_tray_register(const char *arg, const char *sender) {
	if (!arg || !*arg)
		return;
	char service[128], path[128];
	const char *slash = strchr(arg, '/');
	if (arg[0] == '/') {
		if (!sender || !*sender)
			return;
		snprintf(service, sizeof(service), "%s", sender);
		snprintf(path, sizeof(path), "%s", arg);
	} else if (slash) {
		/* The third form, which is not in the spec but is what every real
		 * watcher hands back from RegisteredStatusNotifierItems: the bus name
		 * and the object path glued together with no separator, e.g.
		 * ":1.15335/org/ayatana/NotificationItem/steam". Parsed as a plain bus
		 * name it addressed a service that does not exist, so every item
		 * inherited from an existing watcher silently failed to load. */
		size_t n = (size_t)(slash - arg);
		if (n >= sizeof(service))
			n = sizeof(service) - 1;
		memcpy(service, arg, n);
		service[n] = '\0';
		snprintf(path, sizeof(path), "%s", slash);
	} else {
		snprintf(service, sizeof(service), "%s", arg);
		snprintf(path, sizeof(path), "/StatusNotifierItem");
	}

	BarTrayItem *it = bar_tray_add(service, path);
	if (!it)
		return;
	bar_tray_fetch_props(it);
	bar_update_all();
}

/* An item's own change signals. All three mean the same thing to us -- go and
 * re-read the properties -- because the signals carry no payload. */
static int bar_tray_on_item_changed(sd_bus_message *m, void *user,
									sd_bus_error *err) {
	(void)user;
	(void)err;
	const char *sender = sd_bus_message_get_sender(m);
	BarTrayItem *it = bar_tray_find(sender);
	if (it)
		bar_tray_fetch_props(it);
	return 0;
}

/* An application quitting does NOT send an unregister -- it just drops off the
 * bus. Without this the tray kept a pill for every application ever run. */
static void bar_tray_claim_watcher(void);
static void bar_tray_adopt_existing(void);

static int bar_tray_on_name_owner_changed(sd_bus_message *m, void *user,
										  sd_bus_error *err) {
	(void)user;
	(void)err;
	const char *name = NULL, *old_owner = NULL, *new_owner = NULL;
	if (sd_bus_message_read(m, "sss", &name, &old_owner, &new_owner) <= 0)
		return 0;
	if (!name || !new_owner || *new_owner)
		return 0; /* still owned by someone: not a disappearance */

	/* The incumbent watcher just exited. Take the name over: while it is
	 * unowned NOTHING serves the tray, so every application that starts from
	 * then on has nobody to register its item with and simply never appears.
	 * Seen live the moment waybar was killed -- asteroidz was in client mode
	 * behind it, so org.kde.StatusNotifierWatcher went unowned and stayed
	 * that way. Items already known keep working; it is new ones that are
	 * silently lost, which is the worst kind of failure to debug. */
	if (strcmp(name, BAR_TRAY_WATCHER_NAME) == 0 && !bar_tray_is_watcher) {
		bar_tray_claim_watcher();
		return 0;
	}
	bar_tray_remove(name);
	return 0;
}

/* ─── watcher role ────────────────────────────────────────────────────────── */

static int bar_tray_method_register_item(sd_bus_message *m, void *user,
										 sd_bus_error *err) {
	(void)user;
	(void)err;
	const char *arg = NULL;
	if (sd_bus_message_read(m, "s", &arg) <= 0)
		return sd_bus_reply_method_return(m, "");
	bar_tray_register(arg, sd_bus_message_get_sender(m));

	/* the spec requires the signal; a second host (waybar, running alongside)
	 * relies on it to learn about the item at all */
	bar_tray_emit("StatusNotifierItemRegistered", "s", arg);
	return sd_bus_reply_method_return(m, "");
}

static int bar_tray_method_register_host(sd_bus_message *m, void *user,
										 sd_bus_error *err) {
	(void)user;
	(void)err;
	bar_tray_emit("StatusNotifierHostRegistered", NULL, NULL);
	return sd_bus_reply_method_return(m, "");
}

static int bar_tray_prop_items(sd_bus *bus, const char *path,
							   const char *interface, const char *property,
							   sd_bus_message *reply, void *user,
							   sd_bus_error *err) {
	(void)bus;
	(void)path;
	(void)interface;
	(void)property;
	(void)user;
	(void)err;
	int r = sd_bus_message_open_container(reply, 'a', "s");
	if (r < 0)
		return r;
	for (int32_t i = 0; i < bar_tray_nitems; i++)
		if (bar_tray_items[i].used)
			sd_bus_message_append(reply, "s", bar_tray_items[i].service);
	return sd_bus_message_close_container(reply);
}

/* Both of these MUST have real getters.
 *
 * sd_bus_vtable's NULL getter does not mean "no value" -- it means "read the
 * value straight out of `userdata + offset`". The vtable is exported with a
 * NULL userdata and offset 0, so a NULL getter made sd-bus dereference address
 * zero and take the whole compositor down with it, inside its own dispatch
 * loop where no frame of ours appears in the backtrace.
 *
 * It stayed hidden because reading these is optional: quickshell and Steam
 * only ever call RegisterStatusNotifierItem. Every libappindicator client
 * checks for a host FIRST -- nm-applet, blueman-applet and friends -- so the
 * crash was one common tray application away, on any session, at any time. */
static int bar_tray_prop_host_registered(sd_bus *bus, const char *path,
										 const char *interface,
										 const char *property,
										 sd_bus_message *reply, void *user,
										 sd_bus_error *err) {
	(void)bus; (void)path; (void)interface; (void)property; (void)user;
	(void)err;
	/* We are the host whenever we own the watcher: the two live in the same
	 * process, so there is no window where one exists without the other. A
	 * client asking this is deciding whether its icon would go anywhere. */
	return sd_bus_message_append(reply, "b", bar_tray_is_watcher ? 1 : 0);
}

static int bar_tray_prop_protocol_version(sd_bus *bus, const char *path,
										  const char *interface,
										  const char *property,
										  sd_bus_message *reply, void *user,
										  sd_bus_error *err) {
	(void)bus; (void)path; (void)interface; (void)property; (void)user;
	(void)err;
	return sd_bus_message_append(reply, "i", 0); /* the spec's only version */
}

static const sd_bus_vtable bar_tray_watcher_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("RegisterStatusNotifierItem", "s", "",
				  bar_tray_method_register_item, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("RegisterStatusNotifierHost", "s", "",
				  bar_tray_method_register_host, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_PROPERTY("RegisteredStatusNotifierItems", "as", bar_tray_prop_items,
					0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	SD_BUS_PROPERTY("IsStatusNotifierHostRegistered", "b",
					bar_tray_prop_host_registered, 0,
					SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	SD_BUS_PROPERTY("ProtocolVersion", "i", bar_tray_prop_protocol_version, 0,
					SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_SIGNAL("StatusNotifierItemRegistered", "s", 0),
	SD_BUS_SIGNAL("StatusNotifierItemUnregistered", "s", 0),
	SD_BUS_SIGNAL("StatusNotifierHostRegistered", "", 0),
	SD_BUS_VTABLE_END,
};

/* Acquire org.kde.StatusNotifierWatcher and start serving it ourselves. Called
 * at startup, and again if whoever held it exits. */
static void bar_tray_claim_watcher(void) {
	if (!session_bus || bar_tray_is_watcher)
		return;
	if (!bar_tray_watcher_slot &&
		sd_bus_add_object_vtable(session_bus, &bar_tray_watcher_slot,
								 BAR_TRAY_WATCHER_PATH, BAR_TRAY_WATCHER_IFACE,
								 bar_tray_watcher_vtable, NULL) < 0)
		return;
	if (sd_bus_request_name(session_bus, BAR_TRAY_WATCHER_NAME, 0) < 0)
		return;
	bar_tray_is_watcher = true;
	wlr_log(WLR_INFO, "tray: took over %s", BAR_TRAY_WATCHER_NAME);
	/* Tell the world a host exists again, so anything that gave up while the
	 * name was unowned re-registers with us -- and then go and find the ones
	 * that will not. */
	bar_tray_emit("StatusNotifierHostRegistered", NULL, NULL);
	sd_bus_flush(session_bus);
	bar_tray_adopt_existing();
}

/* Adopt the items that are already on the bus.
 *
 * A tray item is a bus name the application still owns and an object it still
 * serves; registering with a watcher is how it ANNOUNCES that, not what makes
 * it true. So when we become the watcher we can go and look, instead of
 * waiting to be told by applications that have already done their telling.
 *
 * Which matters because a compositor restart re-execs: every fd is CLOEXEC, so
 * the D-Bus connection goes with it and the watcher name is released and
 * reclaimed. Whether an application notices and re-registers is entirely up to
 * that application -- Qt and libappindicator watch the name and come back,
 * plenty of others register exactly once at startup and never again. Those
 * were simply gone until they were restarted, which is what "the tray keeps
 * losing clients" is.
 *
 * Matching by the conventional well-known name rather than by asking every
 * name on the bus whether it implements the interface: that would be hundreds
 * of round trips to catch the handful of items that use a unique name instead,
 * and those re-register on the HostRegistered signal anyway. */
static int bar_tray_on_names(sd_bus_message *m, void *user, sd_bus_error *err) {
	(void)user;
	(void)err;
	if (!m || sd_bus_message_is_method_error(m, NULL))
		return 0;
	int32_t adopted = 0;
	if (sd_bus_message_enter_container(m, 'a', "s") > 0) {
		const char *name = NULL;
		while (sd_bus_message_read(m, "s", &name) > 0 && name) {
			if ((strncmp(name, "org.kde.StatusNotifierItem-", 27) != 0 &&
				 strncmp(name, "org.freedesktop.StatusNotifierItem-", 35) !=
					 0) ||
				bar_tray_find(name))
				continue;
			bar_tray_register(name, NULL);
			adopted++;
		}
		sd_bus_message_exit_container(m);
	}
	if (adopted)
		wlr_log(WLR_INFO, "tray: adopted %d item(s) already on the bus",
				adopted);
	return 0;
}

static void bar_tray_adopt_existing(void) {
	if (!session_bus)
		return;
	if (sd_bus_call_method_async(session_bus, NULL, "org.freedesktop.DBus",
								 "/org/freedesktop/DBus", "org.freedesktop.DBus",
								 "ListNames", bar_tray_on_names, NULL, "") < 0)
		return;
	sd_bus_flush(session_bus);
}

/* ─── client role (another watcher already owns the name) ─────────────────── */

static int bar_tray_on_existing_items(sd_bus_message *m, void *user,
									  sd_bus_error *err) {
	(void)user;
	(void)err;
	if (!m || sd_bus_message_is_method_error(m, NULL))
		return 0;
	/* Get() returns a variant wrapping the array */
	if (sd_bus_message_enter_container(m, 'v', "as") <= 0)
		return 0;
	if (sd_bus_message_enter_container(m, 'a', "s") > 0) {
		const char *svc = NULL;
		while (sd_bus_message_read(m, "s", &svc) > 0 && svc)
			bar_tray_register(svc, NULL);
		sd_bus_message_exit_container(m);
	}
	sd_bus_message_exit_container(m);
	return 0;
}

static int bar_tray_on_watcher_registered(sd_bus_message *m, void *user,
										  sd_bus_error *err) {
	(void)user;
	(void)err;
	const char *arg = NULL;
	if (sd_bus_message_read(m, "s", &arg) > 0 && arg)
		bar_tray_register(arg, NULL);
	return 0;
}

static int bar_tray_on_watcher_unregistered(sd_bus_message *m, void *user,
											sd_bus_error *err) {
	(void)user;
	(void)err;
	const char *arg = NULL;
	if (sd_bus_message_read(m, "s", &arg) > 0 && arg)
		bar_tray_remove(arg);
	return 0;
}

/* ─── setup / teardown ────────────────────────────────────────────────────── */

static void bar_tray_finish(void) {
	if (!bar_tray_started)
		return;
	for (size_t i = 0; i < LENGTH(bar_tray_signal_slots); i++) {
		if (bar_tray_signal_slots[i])
			sd_bus_slot_unref(bar_tray_signal_slots[i]);
		bar_tray_signal_slots[i] = NULL;
	}
	if (bar_tray_watcher_slot) {
		sd_bus_slot_unref(bar_tray_watcher_slot);
		bar_tray_watcher_slot = NULL;
	}
	if (bar_tray_fdo_slot) {
		sd_bus_slot_unref(bar_tray_fdo_slot);
		bar_tray_fdo_slot = NULL;
	}
	if (session_bus) {
		if (bar_tray_is_watcher)
			sd_bus_release_name(session_bus, BAR_TRAY_WATCHER_NAME);
		if (bar_tray_is_fdo_watcher)
			sd_bus_release_name(session_bus, BAR_TRAY_FDO_WATCHER_NAME);
		if (bar_tray_host_name[0])
			sd_bus_release_name(session_bus, bar_tray_host_name);
	}
	bar_tray_is_fdo_watcher = false;
	bar_tray_host_name[0] = '\0';
	bar_tray_is_watcher = false;
	bar_tray_started = false;
	bar_tray_nitems = 0;
	memset(bar_tray_items, 0, sizeof(bar_tray_items));
}

/* Idempotent: called whenever a bar gains a tray module, which happens once
 * per monitor and again on every config reload. */
static void bar_tray_start(void) {
	if (bar_tray_started)
		return;
	if (!session_bus) {
		wlr_log(WLR_INFO, "tray: no session bus, tray module will stay empty");
		return;
	}
	bar_tray_started = true;

	/* Host name first. It is per-process by spec, and owning it is what tells
	 * a watcher (ours or somebody else's) that a tray exists at all -- without
	 * a host registered, well-behaved applications do not publish items. */
	snprintf(bar_tray_host_name, sizeof(bar_tray_host_name),
			 "org.kde.StatusNotifierHost-%d", (int)getpid());
	int r = sd_bus_request_name(session_bus, bar_tray_host_name, 0);
	if (r < 0) {
		wlr_log(WLR_INFO, "tray: cannot own %s: %s", bar_tray_host_name,
				strerror(-r));
		bar_tray_host_name[0] = '\0';
	}

	r = sd_bus_add_object_vtable(session_bus, &bar_tray_watcher_slot,
								 BAR_TRAY_WATCHER_PATH, BAR_TRAY_WATCHER_IFACE,
								 bar_tray_watcher_vtable, NULL);
	if (r < 0)
		wlr_log(WLR_ERROR, "tray: cannot export watcher object: %s",
				strerror(-r));

	/* The vendor-neutral spelling of the same thing, exported at the same path
	 * from the same vtable. Independent of the org.kde.* name on purpose: if
	 * another shell holds one of the two we can still serve the other. */
	r = sd_bus_add_object_vtable(session_bus, &bar_tray_fdo_slot,
								 BAR_TRAY_WATCHER_PATH,
								 BAR_TRAY_FDO_WATCHER_IFACE,
								 bar_tray_watcher_vtable, NULL);
	if (r < 0)
		wlr_log(WLR_DEBUG, "tray: cannot export %s: %s",
				BAR_TRAY_FDO_WATCHER_IFACE, strerror(-r));
	r = sd_bus_request_name(session_bus, BAR_TRAY_FDO_WATCHER_NAME, 0);
	bar_tray_is_fdo_watcher = r >= 0;
	if (!bar_tray_is_fdo_watcher && bar_tray_fdo_slot) {
		sd_bus_slot_unref(bar_tray_fdo_slot);
		bar_tray_fdo_slot = NULL;
	}

	r = sd_bus_request_name(session_bus, BAR_TRAY_WATCHER_NAME, 0);
	bar_tray_is_watcher = r >= 0;
	if (bar_tray_is_watcher) {
		/* Go and find the items that already exist. Applications that were
		 * showing an icon before this process started -- which is every one of
		 * them after a compositor restart -- have already announced
		 * themselves, to a watcher that no longer exists. See
		 * bar_tray_adopt_existing. */
		bar_tray_adopt_existing();
	}
	if (!bar_tray_is_watcher) {
		/* -EEXIST: waybar (or another shell) is the watcher. Mirror it rather
		 * than fighting over the name -- see the header comment. */
		wlr_log(WLR_INFO,
				"tray: %s already owned, following the existing watcher",
				BAR_TRAY_WATCHER_NAME);
		if (bar_tray_watcher_slot) {
			sd_bus_slot_unref(bar_tray_watcher_slot);
			bar_tray_watcher_slot = NULL;
		}
		if (bar_tray_host_name[0])
			sd_bus_call_method_async(session_bus, NULL, BAR_TRAY_WATCHER_NAME,
									 BAR_TRAY_WATCHER_PATH,
									 BAR_TRAY_WATCHER_IFACE,
									 "RegisterStatusNotifierHost", NULL, NULL,
									 "s", bar_tray_host_name);
		sd_bus_call_method_async(session_bus, NULL, BAR_TRAY_WATCHER_NAME,
								 BAR_TRAY_WATCHER_PATH,
								 "org.freedesktop.DBus.Properties", "Get",
								 bar_tray_on_existing_items, NULL, "ss",
								 BAR_TRAY_WATCHER_IFACE,
								 "RegisteredStatusNotifierItems");
		sd_bus_match_signal(session_bus, &bar_tray_signal_slots[2], NULL,
							BAR_TRAY_WATCHER_PATH, BAR_TRAY_WATCHER_IFACE,
							"StatusNotifierItemRegistered",
							bar_tray_on_watcher_registered, NULL);
		sd_bus_match_signal(session_bus, &bar_tray_signal_slots[3], NULL,
							BAR_TRAY_WATCHER_PATH, BAR_TRAY_WATCHER_IFACE,
							"StatusNotifierItemUnregistered",
							bar_tray_on_watcher_unregistered, NULL);
	}

	/* Item change signals, from any sender: matching on the interface rather
	 * than per-item means a newly registered item needs no extra match. */
	sd_bus_add_match(session_bus, &bar_tray_signal_slots[0],
					 "type='signal',interface='" BAR_TRAY_ITEM_IFACE "'",
					 bar_tray_on_item_changed, NULL);
	/* and the disappearance of anything we track */
	sd_bus_add_match(session_bus, &bar_tray_signal_slots[1],
					 "type='signal',sender='org.freedesktop.DBus',"
					 "interface='org.freedesktop.DBus',"
					 "member='NameOwnerChanged'",
					 bar_tray_on_name_owner_changed, NULL);

	sd_bus_flush(session_bus);
}

/* ─── activation ──────────────────────────────────────────────────────────── */

/* x/y are the SCREEN coordinates of the click, which the spec passes so the
 * item can position its own popup near the pill that was clicked. */
static void bar_tray_activate(BarTrayItem *it, const char *method, int32_t x,
							  int32_t y) {
	if (!session_bus || !it)
		return;
	sd_bus_call_method_async(session_bus, NULL, it->service, it->path,
							 BAR_TRAY_ITEM_IFACE, method, NULL, NULL, "ii", x,
							 y);
	sd_bus_flush(session_bus);
}

static void bar_tray_scroll(BarTrayItem *it, int32_t delta,
							const char *orientation) {
	if (!session_bus || !it)
		return;
	sd_bus_call_method_async(session_bus, NULL, it->service, it->path,
							 BAR_TRAY_ITEM_IFACE, "Scroll", NULL, NULL, "is",
							 delta, orientation);
	sd_bus_flush(session_bus);
}

#endif /* ASTEROIDZ_BAR_TRAY_H */
