/* asteroidz-trayd -- a StatusNotifierItem host, outside the compositor.
 *
 * WHY THIS EXISTS
 *
 * The compositor's built-in `tray` module is the riskiest thing on the bar,
 * and not because drawing pills is hard. It is because a tray host is the
 * endpoint of a protocol driven by every application you happen to have
 * installed: they hand it raw ARGB pixmaps of their choosing over D-Bus, and
 * the host decodes them. In `bar-tray.h` that decode runs in a D-Bus reply
 * callback on the compositor's own event loop, with no upper bound on the
 * dimensions an application may send -- so a badly packaged app shipping one
 * oversized icon can stall the compositor, which means the whole desktop.
 *
 * Moving the host into its own process fixes that by construction. This
 * program owns the bus name, decodes pixmaps, and hands the bar a list of
 * items whose artwork is a FILE PATH. The compositor loads a bounded PNG from
 * disk -- the same thing it already does for every other icon -- and never
 * parses anything an application sent it.
 *
 * It is an asteroidz bar plugin, so it speaks that contract:
 *
 *   stdout   one JSON object per update: {"items":[{id,icon,text,tooltip}...]}
 *   stdin    one JSON event per line: {"event":"click","button":...,"item":...}
 *
 *   bar {
 *       modules-right "custom/tray,volume,clock"
 *       custom "tray" { exec "asteroidz-trayd"; continuous true }
 *   }
 *
 * WHAT IT DOES NOT DO YET: the DBusMenu context menu. A right-click falls back
 * to the item's own SecondaryActivate, which is what most applications map to
 * "show me my menu" anyway -- the same fallback the built-in module shipped
 * with. Until a plugin can ask the compositor for a menu, the built-in `tray`
 * module remains the one with real context menus, and the two can be swapped
 * by changing one word in the config.
 *
 * The D-Bus logic is a port of bar-tray.h's, which is proven against real
 * applications (Steam, Discord, Electron appindicators, nm-applet, blueman,
 * quickshell). The comments explaining WHY each oddity is handled live there;
 * this file keeps the short form and the differences.
 */

#define _POSIX_C_SOURCE 200809L

#include <cairo/cairo.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <systemd/sd-bus.h>
#include <systemd/sd-event.h>
#include <unistd.h>

#define LENGTH(X) (sizeof(X) / sizeof(X)[0])

#define MAX_ITEMS 16
#define WATCHER_NAME "org.kde.StatusNotifierWatcher"
#define WATCHER_PATH "/StatusNotifierWatcher"
#define WATCHER_IFACE "org.kde.StatusNotifierWatcher"
#define FDO_WATCHER_NAME "org.freedesktop.StatusNotifierWatcher"
#define FDO_WATCHER_IFACE "org.freedesktop.StatusNotifierWatcher"
#define ITEM_IFACE "org.kde.StatusNotifierItem"

/* Largest pixmap we will look at, per side.
 *
 * This is the bound whose absence is the entire reason for this program. The
 * decode is O(w*h) and an application picks w and h, so without a cap a 4096
 * square icon is 16.7 million pixels of work driven by untrusted input. 512 is
 * far above any real tray icon (applications ship 16/22/24/32/48) while still
 * being a quarter of a megapixel rather than sixteen. Anything larger is
 * ignored, not clamped: an application sending it is broken, and the smaller
 * pixmaps it almost certainly also sent will be used instead. */
#define MAX_PIXMAP 512
/* What we write out. Downscaling here rather than in the compositor keeps the
 * cost on this side of the fence too. */
#define ICON_SIZE 64

typedef struct {
	char service[128];
	char owner[64];
	char path[128];
	char id[128];
	char title[192];
	char status[32];
	/* An absolute PNG path we wrote, or a bare icon NAME for the theme to
	 * resolve. The compositor accepts either; a name is preferable when the
	 * application gave one, because it follows the user's icon theme. */
	char icon[256];
	char menu_path[128];
	int32_t icon_retries;
	bool used;
} Item;

static Item items[MAX_ITEMS];
static int32_t nitems = 0;
static sd_bus *bus = NULL;
static sd_event *event = NULL;
static sd_bus_slot *watcher_slot = NULL;
static sd_bus_slot *fdo_slot = NULL;
static sd_bus_slot *signal_slots[5] = {0};
static bool is_watcher = false;
static char host_name[128];
static char icon_dir[256];
static bool dirty = false;

/* ─── output ──────────────────────────────────────────────────────────────── */

static void json_escape(const char *in, char *out, size_t len) {
	size_t o = 0;
	for (const char *p = in; *p && o + 7 < len; p++) {
		unsigned char c = (unsigned char)*p;
		if (c == '"' || c == '\\') {
			out[o++] = '\\';
			out[o++] = (char)c;
		} else if (c < 0x20) {
			o += (size_t)snprintf(out + o, len - o, "\\u%04x", c);
		} else {
			out[o++] = (char)c;
		}
	}
	out[o] = '\0';
}

/* Emit the current item list, but only when it actually changed.
 *
 * Property fetches arrive one reply at a time and several land per item, so
 * without this a tray settling after login would emit a dozen identical
 * updates. The compositor gates its own redraw on a digest anyway; this just
 * stops us waking it to compute one. */
static void emit(void) {
	char buf[8192];
	size_t o = 0;
	o += (size_t)snprintf(buf + o, sizeof(buf) - o, "{\"items\":[");
	bool first = true;
	for (int32_t i = 0; i < nitems && o < sizeof(buf) - 512; i++) {
		Item *it = &items[i];
		if (!it->used)
			continue;
		/* Passive means "I have nothing to say right now" and the spec says a
		 * host may hide it. Hiding is right for a bar: an application that is
		 * running but idle should not spend a slot. */
		if (!strcmp(it->status, "Passive"))
			continue;
		/* Nothing to draw yet -- the properties have not come back. Better an
		 * absent pill for a moment than a blank one that is clickable. */
		if (!it->icon[0])
			continue;
		char eid[256], eicon[512], etip[512];
		json_escape(it->service, eid, sizeof(eid));
		json_escape(it->icon, eicon, sizeof(eicon));
		json_escape(it->title[0] ? it->title : it->id, etip, sizeof(etip));
		o += (size_t)snprintf(buf + o, sizeof(buf) - o,
							  "%s{\"id\":\"%s\",\"icon\":\"%s\","
							  "\"tooltip\":\"%s\"%s}",
							  first ? "" : ",", eid, eicon, etip,
							  !strcmp(it->status, "NeedsAttention")
								  ? ",\"class\":\"urgent\""
								  : "");
		first = false;
	}
	snprintf(buf + o, sizeof(buf) - o, "]}");

	static char last[8192];
	if (!strcmp(buf, last))
		return;
	snprintf(last, sizeof(last), "%s", buf);
	printf("%s\n", buf);
	fflush(stdout);
}

static void mark_dirty(void) { dirty = true; }

/* ─── items ───────────────────────────────────────────────────────────────── */

static Item *find(const char *service) {
	if (!service)
		return NULL;
	for (int32_t i = 0; i < nitems; i++)
		if (items[i].used && !strcmp(items[i].service, service))
			return &items[i];
	return NULL;
}

static Item *find_by_bus_name(const char *name) {
	if (!name)
		return NULL;
	for (int32_t i = 0; i < nitems; i++) {
		if (!items[i].used)
			continue;
		if (!strcmp(items[i].service, name) || !strcmp(items[i].owner, name))
			return &items[i];
	}
	return NULL;
}

static int on_owner(sd_bus_message *m, void *user, sd_bus_error *err) {
	(void)err;
	char *service = user;
	Item *it = find(service);
	const char *owner = NULL;
	if (it && m && !sd_bus_message_is_method_error(m, NULL) &&
		sd_bus_message_read(m, "s", &owner) > 0 && owner)
		snprintf(it->owner, sizeof(it->owner), "%s", owner);
	free(service);
	return 0;
}

/* A signal's sender is always the UNIQUE name, whatever the item registered
 * itself as, so an item under a well-known name never matches its own change
 * signals unless we resolve this once. */
static void resolve_owner(Item *it) {
	if (!it || it->service[0] == ':')
		return;
	char *service = strdup(it->service);
	if (!service)
		return;
	if (sd_bus_call_method_async(bus, NULL, "org.freedesktop.DBus",
								 "/org/freedesktop/DBus", "org.freedesktop.DBus",
								 "GetNameOwner", on_owner, service, "s",
								 it->service) < 0)
		free(service);
}

static Item *add(const char *service, const char *path) {
	Item *it = find(service);
	if (it)
		return it;
	if (nitems >= MAX_ITEMS)
		return NULL;
	it = &items[nitems++];
	memset(it, 0, sizeof(*it));
	snprintf(it->service, sizeof(it->service), "%s", service);
	snprintf(it->path, sizeof(it->path), "%s",
			 path && *path ? path : "/StatusNotifierItem");
	snprintf(it->status, sizeof(it->status), "Active");
	it->used = true;
	return it;
}

static void remove_item(const char *service) {
	for (int32_t i = 0; i < nitems; i++) {
		if (!items[i].used || strcmp(items[i].service, service))
			continue;
		/* keep the array dense so indices stay meaningful */
		for (int32_t j = i; j < nitems - 1; j++)
			items[j] = items[j + 1];
		nitems--;
		memset(&items[nitems], 0, sizeof(items[nitems]));
		mark_dirty();
		return;
	}
}

/* ─── pixmaps ─────────────────────────────────────────────────────────────── */

/* Write one ARGB32 (network byte order, as the spec says) pixmap to a PNG,
 * downscaled to ICON_SIZE, and return the path in `out`.
 *
 * Everything about this function that looks paranoid is the point: it is the
 * only place in the tray path that touches bytes an application chose, and it
 * is deliberately not in the compositor. */
static bool write_pixmap(const char *service, const uint8_t *argb, int32_t w,
						 int32_t h, char *out, size_t outlen) {
	if (w <= 0 || h <= 0 || w > MAX_PIXMAP || h > MAX_PIXMAP)
		return false;

	cairo_surface_t *src =
		cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
	if (cairo_surface_status(src) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(src);
		return false;
	}
	int stride = cairo_image_surface_get_stride(src);
	unsigned char *dst = cairo_image_surface_get_data(src);
	for (int32_t y = 0; y < h; y++) {
		uint32_t *drow = (uint32_t *)(dst + (size_t)y * stride);
		const uint8_t *srow = argb + (size_t)y * w * 4;
		for (int32_t x = 0; x < w; x++) {
			uint8_t a = srow[x * 4 + 0];
			uint8_t r = srow[x * 4 + 1];
			uint8_t g = srow[x * 4 + 2];
			uint8_t b = srow[x * 4 + 3];
			/* cairo's ARGB32 is premultiplied; the wire format is not */
			drow[x] = ((uint32_t)a << 24) |
					  ((uint32_t)(r * a / 255) << 16) |
					  ((uint32_t)(g * a / 255) << 8) | (uint32_t)(b * a / 255);
		}
	}
	cairo_surface_mark_dirty(src);

	cairo_surface_t *scaled =
		cairo_image_surface_create(CAIRO_FORMAT_ARGB32, ICON_SIZE, ICON_SIZE);
	bool ok = false;
	if (cairo_surface_status(scaled) == CAIRO_STATUS_SUCCESS) {
		cairo_t *cr = cairo_create(scaled);
		cairo_scale(cr, (double)ICON_SIZE / w, (double)ICON_SIZE / h);
		cairo_set_source_surface(cr, src, 0, 0);
		cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BEST);
		cairo_paint(cr);
		cairo_destroy(cr);

		/* Named for the bus name, so a NewIcon overwrites in place and two
		 * applications cannot collide. */
		char safe[128];
		size_t o = 0;
		for (const char *p = service; *p && o + 1 < sizeof(safe); p++)
			safe[o++] = (*p == '/' || *p == '.' || *p == ':') ? '_' : *p;
		safe[o] = '\0';
		snprintf(out, outlen, "%s/%s.png", icon_dir, safe);
		ok = cairo_surface_write_to_png(scaled, out) == CAIRO_STATUS_SUCCESS;
	}
	cairo_surface_destroy(scaled);
	cairo_surface_destroy(src);
	return ok;
}

/* Pick the pixmap closest to what we draw at, then write it. */
static bool read_pixmap(sd_bus_message *m, Item *it) {
	if (sd_bus_message_enter_container(m, 'a', "(iiay)") <= 0)
		return false;

	const uint8_t *best = NULL;
	int32_t best_w = 0, best_h = 0;
	bool ok = false;

	while (sd_bus_message_enter_container(m, 'r', "iiay") > 0) {
		int32_t w = 0, h = 0;
		const void *data = NULL;
		size_t len = 0;
		if (sd_bus_message_read(m, "ii", &w, &h) > 0 &&
			sd_bus_message_read_array(m, 'y', &data, &len) > 0 && w > 0 &&
			h > 0 && w <= MAX_PIXMAP && h <= MAX_PIXMAP && data &&
			len >= (size_t)w * h * 4) {
			bool better = !best || (best_w < 32 && w > best_w) ||
						  (w >= 32 && best_w >= 32 && w < best_w);
			if (better) {
				best = data;
				best_w = w;
				best_h = h;
			}
		}
		sd_bus_message_exit_container(m);
	}
	sd_bus_message_exit_container(m);

	if (best) {
		char path[256];
		if (write_pixmap(it->service, best, best_w, best_h, path, sizeof(path))) {
			snprintf(it->icon, sizeof(it->icon), "%s", path);
			ok = true;
		}
	}
	return ok;
}

/* ─── properties ──────────────────────────────────────────────────────────── */

static int on_props(sd_bus_message *m, void *user, sd_bus_error *err) {
	(void)err;
	char *service = user;
	Item *it = find(service);
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

			/* Menu is an object path, not a string: read as "s" it comes back
			 * empty and every item looks like it has no menu. */
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
					if (!have_pixmap && read_pixmap(m, it))
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

	/* NeedsAttention swaps in the attention artwork -- that is the point of
	 * the status. A NAME beats a pixmap: it is themed and scalable, where a
	 * pixmap is whatever the application happened to ship. */
	const char *want = icon_name;
	if (attention_icon[0] && !strcmp(it->status, "NeedsAttention"))
		want = attention_icon;

	if (want && *want) {
		if (icon_theme_path[0]) {
			/* Applications shipping their own theme dir (Electron, Steam) name
			 * an icon in no installed theme, so hand over an absolute path. */
			static const char *exts[] = {"png", "svg"};
			bool found = false;
			for (size_t e = 0; e < LENGTH(exts) && !found; e++) {
				char abs[512];
				snprintf(abs, sizeof(abs), "%s/%s.%s", icon_theme_path, want,
						 exts[e]);
				if (access(abs, R_OK) == 0) {
					/* explicit truncation: an icon path longer than this is
					 * unusable anyway, and silently cutting it is better than
					 * refusing the item */
					snprintf(it->icon, sizeof(it->icon), "%.*s",
							 (int)sizeof(it->icon) - 1, abs);
					found = true;
				}
			}
			if (!found && !have_pixmap)
				snprintf(it->icon, sizeof(it->icon), "%s", want);
		} else {
			snprintf(it->icon, sizeof(it->icon), "%s", want);
		}
	} else if (!have_pixmap) {
		it->icon[0] = '\0';
	}

	/* An application can register before it is ready to answer; a reply that
	 * arrives empty must not leave the item with no artwork and no reason to
	 * ask again. Bounded, and cleared as soon as anything resolves. */
	if (!it->icon[0] && it->icon_retries < 5)
		it->icon_retries++;
	else if (it->icon[0])
		it->icon_retries = 0;

	free(service);
	mark_dirty();
	return 0;
}

static void fetch_props(Item *it) {
	if (!bus || !it)
		return;
	/* the reply may arrive after this item is gone, so look it up by name */
	char *service = strdup(it->service);
	if (!service)
		return;
	if (sd_bus_call_method_async(bus, NULL, it->service, it->path,
								 "org.freedesktop.DBus.Properties", "GetAll",
								 on_props, service, "s", ITEM_IFACE) < 0)
		free(service);
}

static void register_item(const char *arg, const char *sender) {
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
		/* Not in the spec, but what every real watcher hands back from
		 * RegisteredStatusNotifierItems: bus name and object path glued
		 * together, ":1.15335/org/ayatana/NotificationItem/steam". */
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

	Item *it = add(service, path);
	if (!it)
		return;
	resolve_owner(it);
	fetch_props(it);
}

/* ─── signals ─────────────────────────────────────────────────────────────── */

static int on_item_changed(sd_bus_message *m, void *user, sd_bus_error *err) {
	(void)user;
	(void)err;
	Item *it = find_by_bus_name(sd_bus_message_get_sender(m));
	if (it)
		fetch_props(it);
	return 0;
}

static void claim_watcher(void);
static void adopt_existing(void);

static int on_name_owner_changed(sd_bus_message *m, void *user,
								 sd_bus_error *err) {
	(void)user;
	(void)err;
	const char *name = NULL, *old = NULL, *new = NULL;
	if (sd_bus_message_read(m, "sss", &name, &old, &new) <= 0 || !name)
		return 0;
	if (new && !*new) {
		/* An application quitting does not unregister -- it just drops off the
		 * bus. Without this we keep a pill for every application ever run. */
		Item *it = find_by_bus_name(name);
		if (it)
			remove_item(it->service);
		if (!strcmp(name, WATCHER_NAME) && !is_watcher)
			claim_watcher();
	}
	return 0;
}

/* ─── watcher role ────────────────────────────────────────────────────────── */

static int method_register_item(sd_bus_message *m, void *user,
								sd_bus_error *err) {
	(void)user;
	(void)err;
	const char *arg = NULL;
	if (sd_bus_message_read(m, "s", &arg) > 0 && arg)
		register_item(arg, sd_bus_message_get_sender(m));
	return sd_bus_reply_method_return(m, "");
}

static int method_register_host(sd_bus_message *m, void *user,
								sd_bus_error *err) {
	(void)user;
	(void)err;
	return sd_bus_reply_method_return(m, "");
}

static int prop_items(sd_bus *b, const char *path, const char *iface,
					  const char *prop, sd_bus_message *reply, void *user,
					  sd_bus_error *err) {
	(void)b;
	(void)path;
	(void)iface;
	(void)prop;
	(void)user;
	(void)err;
	int r = sd_bus_message_open_container(reply, 'a', "s");
	if (r < 0)
		return r;
	for (int32_t i = 0; i < nitems; i++)
		if (items[i].used)
			sd_bus_message_append(reply, "s", items[i].service);
	return sd_bus_message_close_container(reply);
}

static int prop_host_registered(sd_bus *b, const char *path, const char *iface,
								const char *prop, sd_bus_message *reply,
								void *user, sd_bus_error *err) {
	(void)b;
	(void)path;
	(void)iface;
	(void)prop;
	(void)user;
	(void)err;
	return sd_bus_message_append(reply, "b", 1);
}

static int prop_protocol_version(sd_bus *b, const char *path,
								 const char *iface, const char *prop,
								 sd_bus_message *reply, void *user,
								 sd_bus_error *err) {
	(void)b;
	(void)path;
	(void)iface;
	(void)prop;
	(void)user;
	(void)err;
	return sd_bus_message_append(reply, "i", 0);
}

static const sd_bus_vtable watcher_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("RegisterStatusNotifierItem", "s", "", method_register_item,
				  SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("RegisterStatusNotifierHost", "s", "", method_register_host,
				  SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_PROPERTY("RegisteredStatusNotifierItems", "as", prop_items, 0,
					SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	SD_BUS_PROPERTY("IsStatusNotifierHostRegistered", "b",
					prop_host_registered, 0,
					SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	SD_BUS_PROPERTY("ProtocolVersion", "i", prop_protocol_version, 0,
					SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_SIGNAL("StatusNotifierItemRegistered", "s", 0),
	SD_BUS_SIGNAL("StatusNotifierItemUnregistered", "s", 0),
	SD_BUS_SIGNAL("StatusNotifierHostRegistered", "", 0),
	SD_BUS_VTABLE_END,
};

static int on_names(sd_bus_message *m, void *user, sd_bus_error *err) {
	(void)user;
	(void)err;
	if (!m || sd_bus_message_is_method_error(m, NULL))
		return 0;
	if (sd_bus_message_enter_container(m, 'a', "s") > 0) {
		const char *name = NULL;
		while (sd_bus_message_read(m, "s", &name) > 0 && name) {
			if ((strncmp(name, "org.kde.StatusNotifierItem-", 27) != 0 &&
				 strncmp(name, "org.freedesktop.StatusNotifierItem-", 35) !=
					 0) ||
				find(name))
				continue;
			register_item(name, NULL);
		}
		sd_bus_message_exit_container(m);
	}
	return 0;
}

/* Adopt items already on the bus. Registering with a watcher is how an item
 * ANNOUNCES itself, not what makes it true -- so when we become the watcher we
 * can go and look, instead of waiting to be told by applications that have
 * already done their telling and will never do it again. */
static void adopt_existing(void) {
	if (!bus)
		return;
	sd_bus_call_method_async(bus, NULL, "org.freedesktop.DBus",
							 "/org/freedesktop/DBus", "org.freedesktop.DBus",
							 "ListNames", on_names, NULL, "");
}

static void emit_signal(const char *member, const char *sig, const char *arg) {
	if (!bus)
		return;
	sd_bus_emit_signal(bus, WATCHER_PATH, WATCHER_IFACE, member, sig, arg);
	sd_bus_emit_signal(bus, WATCHER_PATH, FDO_WATCHER_IFACE, member, sig, arg);
}

static void claim_watcher(void) {
	if (!bus || is_watcher)
		return;
	if (!watcher_slot &&
		sd_bus_add_object_vtable(bus, &watcher_slot, WATCHER_PATH,
								 WATCHER_IFACE, watcher_vtable, NULL) < 0)
		return;
	if (sd_bus_request_name(bus, WATCHER_NAME, 0) < 0)
		return;
	is_watcher = true;
	/* The vendor-neutral spelling too: nothing in the wild asks for it, but it
	 * costs one name and one vtable, and a client that ever does prefer it
	 * finds us. */
	if (sd_bus_add_object_vtable(bus, &fdo_slot, WATCHER_PATH,
								 FDO_WATCHER_IFACE, watcher_vtable, NULL) >= 0)
		sd_bus_request_name(bus, FDO_WATCHER_NAME, 0);
	emit_signal("StatusNotifierHostRegistered", NULL, NULL);
	adopt_existing();
}

/* ─── client role (something else owns the watcher name) ──────────────────── */

static int on_existing_items(sd_bus_message *m, void *user, sd_bus_error *err) {
	(void)user;
	(void)err;
	if (!m || sd_bus_message_is_method_error(m, NULL))
		return 0;
	if (sd_bus_message_enter_container(m, 'v', "as") <= 0)
		return 0;
	if (sd_bus_message_enter_container(m, 'a', "s") > 0) {
		const char *svc = NULL;
		while (sd_bus_message_read(m, "s", &svc) > 0 && svc)
			register_item(svc, NULL);
		sd_bus_message_exit_container(m);
	}
	sd_bus_message_exit_container(m);
	return 0;
}

static int on_watcher_registered(sd_bus_message *m, void *user,
								 sd_bus_error *err) {
	(void)user;
	(void)err;
	const char *arg = NULL;
	if (sd_bus_message_read(m, "s", &arg) > 0 && arg)
		register_item(arg, NULL);
	return 0;
}

static int on_watcher_unregistered(sd_bus_message *m, void *user,
								   sd_bus_error *err) {
	(void)user;
	(void)err;
	const char *arg = NULL;
	if (sd_bus_message_read(m, "s", &arg) > 0 && arg)
		remove_item(arg);
	return 0;
}

/* ─── input: click events from the compositor ─────────────────────────────── */

/* A minimal string-field reader. Pulling in a JSON library for four keys of
 * input we generate ourselves on the other side would be the tail wagging the
 * dog; anything unrecognised is ignored, which is the only behaviour that
 * matters for forward compatibility. */
static bool json_field(const char *line, const char *key, char *out,
					   size_t len) {
	char pat[64];
	snprintf(pat, sizeof(pat), "\"%s\":\"", key);
	const char *p = strstr(line, pat);
	if (!p)
		return false;
	p += strlen(pat);
	size_t o = 0;
	while (*p && *p != '"' && o + 1 < len) {
		if (*p == '\\' && p[1])
			p++;
		out[o++] = *p++;
	}
	out[o] = '\0';
	return true;
}

static int json_int(const char *line, const char *key, int32_t fallback) {
	char pat[64];
	snprintf(pat, sizeof(pat), "\"%s\":", key);
	const char *p = strstr(line, pat);
	if (!p)
		return fallback;
	return atoi(p + strlen(pat));
}

static void activate(Item *it, const char *method, int32_t x, int32_t y) {
	if (!bus || !it)
		return;
	/* Async, and the reply is discarded: a tray application that is wedged
	 * must not be able to hold this process, let alone the bar. The SCREEN
	 * position is what the spec passes so the application can place its own
	 * window next to the icon -- it is the reason clicks have to come back
	 * over the event channel rather than being a shell command. */
	sd_bus_call_method_async(bus, NULL, it->service, it->path, ITEM_IFACE,
							 method, NULL, NULL, "ii", x, y);
}

static int on_stdin(sd_event_source *s, int fd, uint32_t revents, void *user) {
	(void)s;
	(void)revents;
	(void)user;
	static char buf[4096];
	static size_t len = 0;

	ssize_t n = read(fd, buf + len, sizeof(buf) - len - 1);
	if (n <= 0) {
		/* The compositor closed our stdin, which means the bar is gone. There
		 * is nobody left to draw for, so exit rather than linger owning the
		 * watcher name -- an orphan host would keep applications registering
		 * with a tray nothing displays. */
		if (n == 0 || (errno != EAGAIN && errno != EINTR))
			sd_event_exit(event, 0);
		return 0;
	}
	len += (size_t)n;
	buf[len] = '\0';

	char *start = buf;
	for (char *nl; (nl = strchr(start, '\n')); start = nl + 1) {
		*nl = '\0';
		char ev[32], button[16], id[128];
		if (!json_field(start, "event", ev, sizeof(ev)) || strcmp(ev, "click"))
			continue;
		if (!json_field(start, "item", id, sizeof(id)))
			continue;
		if (!json_field(start, "button", button, sizeof(button)))
			continue;
		Item *it = find(id);
		if (!it)
			continue;
		int32_t x = json_int(start, "x", 0), y = json_int(start, "y", 0);
		if (!strcmp(button, "left"))
			activate(it, "Activate", x, y);
		else
			/* Right and middle both fall back to SecondaryActivate, which is
			 * what most applications map to "show my menu". A real DBusMenu
			 * needs the compositor to be able to draw one on a plugin's
			 * behalf, which it cannot yet. */
			activate(it, "SecondaryActivate", x, y);
	}
	/* keep whatever partial line is left */
	len = strlen(start);
	memmove(buf, start, len + 1);
	return 0;
}

/* ─── main ────────────────────────────────────────────────────────────────── */

static int on_tick(sd_event_source *s, uint64_t usec, void *user) {
	(void)user;
	if (dirty) {
		dirty = false;
		emit();
	}
	/* Re-ask any item that still has no artwork. Costs one bus call per
	 * unresolved item per tick and nothing at all once everything resolves. */
	for (int32_t i = 0; i < nitems; i++)
		if (items[i].used && !items[i].icon[0] && items[i].icon_retries > 0 &&
			items[i].icon_retries < 5)
			fetch_props(&items[i]);
	sd_event_source_set_time(s, usec + 1000000);
	/* Re-arm. sd_event_add_time creates a ONESHOT source, and sd-event
	 * disables it the moment it fires -- setting a new time does NOT bring it
	 * back. Without this the tick runs exactly once and the daemon goes
	 * permanently silent: it keeps tracking items and decoding their pixmaps,
	 * and never tells anyone.
	 *
	 * That failure is invisible to a test that starts its item BEFORE the
	 * host, because those items are adopted at startup and caught by the one
	 * tick that does fire. It is the normal real-world ordering -- an
	 * application registering after the host is already running -- that never
	 * reaches the bar. Which is exactly how this shipped and had to be found
	 * on a live desktop. */
	sd_event_source_set_enabled(s, SD_EVENT_ONESHOT);
	return 0;
}

int main(void) {
	const char *rt = getenv("XDG_RUNTIME_DIR");
	snprintf(icon_dir, sizeof(icon_dir), "%s/asteroidz-tray",
			 rt && *rt ? rt : "/tmp");
	if (mkdir(icon_dir, 0700) < 0 && errno != EEXIST) {
		fprintf(stderr, "trayd: cannot create %s: %s\n", icon_dir,
				strerror(errno));
		return 1;
	}

	if (sd_event_default(&event) < 0)
		return 1;
	if (sd_bus_open_user(&bus) < 0) {
		fprintf(stderr, "trayd: no session bus\n");
		return 1;
	}
	if (sd_bus_attach_event(bus, event, SD_EVENT_PRIORITY_NORMAL) < 0)
		return 1;

	snprintf(host_name, sizeof(host_name), "org.kde.StatusNotifierHost-%d",
			 (int)getpid());
	sd_bus_request_name(bus, host_name, 0);

	sd_bus_match_signal(bus, &signal_slots[0], NULL, NULL, ITEM_IFACE, NULL,
						on_item_changed, NULL);
	sd_bus_match_signal(bus, &signal_slots[1], NULL, NULL,
						"org.freedesktop.DBus.Properties", "PropertiesChanged",
						on_item_changed, NULL);
	sd_bus_match_signal(bus, &signal_slots[2], "org.freedesktop.DBus",
						"/org/freedesktop/DBus", "org.freedesktop.DBus",
						"NameOwnerChanged", on_name_owner_changed, NULL);
	sd_bus_match_signal(bus, &signal_slots[3], NULL, WATCHER_PATH,
						WATCHER_IFACE, "StatusNotifierItemRegistered",
						on_watcher_registered, NULL);
	sd_bus_match_signal(bus, &signal_slots[4], NULL, WATCHER_PATH,
						WATCHER_IFACE, "StatusNotifierItemUnregistered",
						on_watcher_unregistered, NULL);

	claim_watcher();
	if (!is_watcher) {
		/* Something else owns the watcher -- waybar's tray, a leftover
		 * snixembed. Taking the name is not possible and queueing for it would
		 * leave the tray dead until that process exits, so register as a plain
		 * host with the incumbent and mirror its list. Both bars then show the
		 * same tray, which is what is wanted while migrating between them. */
		sd_bus_call_method_async(bus, NULL, WATCHER_NAME, WATCHER_PATH,
								 WATCHER_IFACE, "RegisterStatusNotifierHost",
								 NULL, NULL, "s", host_name);
		sd_bus_call_method_async(bus, NULL, WATCHER_NAME, WATCHER_PATH,
								 "org.freedesktop.DBus.Properties", "Get",
								 on_existing_items, NULL, "ss", WATCHER_IFACE,
								 "RegisteredStatusNotifierItems");
	}

	fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK);
	sd_event_add_io(event, NULL, STDIN_FILENO, EPOLLIN, on_stdin, NULL);

	uint64_t now = 0;
	sd_event_now(event, CLOCK_MONOTONIC, &now);
	sd_event_add_time(event, NULL, CLOCK_MONOTONIC, now + 500000, 100000,
					  on_tick, NULL);

	/* An empty list up front, so the bar knows we are alive and drawing
	 * nothing rather than not started yet. */
	emit();
	sd_event_loop(event);

	for (size_t i = 0; i < LENGTH(signal_slots); i++)
		sd_bus_slot_unref(signal_slots[i]);
	sd_bus_slot_unref(watcher_slot);
	sd_bus_slot_unref(fdo_slot);
	sd_bus_unref(bus);
	sd_event_unref(event);
	return 0;
}
