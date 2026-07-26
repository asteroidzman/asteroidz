/* asteroidz-xembed — XEmbed system tray bridge.
 *
 * X11 applications that predate StatusNotifierItem (older Qt/GTK apps, some
 * games, anything built on GtkStatusIcon) publish their tray icon over the
 * freedesktop XEmbed protocol: they look for an owner of the
 * _NET_SYSTEM_TRAY_S<screen> selection and ask it to dock a window. asteroidz'
 * own tray is an SNI host and speaks no X11 at all, so those icons have nowhere
 * to go and simply never appear.
 *
 * NOT Discord, despite what this file originally said. Discord was the symptom
 * that prompted this, and the diagnosis was wrong: it has no libappindicator
 * mapped, which was taken to mean XEmbed, but Electron implements SNI itself
 * over D-Bus. Discord's icon was missing because the tray MODULE had been
 * silently dropped by a module-cap bug, so no watcher existed when Discord
 * started -- and it never retries. With a watcher present at startup it
 * registers an SNI item like anything else.
 *
 * The bridge remains correct and is verified working end to end, but it is for
 * real XEmbed-only clients, which are rarer than the original claim implied.
 *
 * This owns that selection, embeds the icon windows, and re-exports each one as
 * a StatusNotifierItem. The compositor's tray then displays them with no idea
 * they were ever X11 windows.
 *
 * WHY A SEPARATE PROCESS, when the compositor happily hosts the D-Bus side
 * in-process: this half is X11. It composites foreign windows, reads their
 * pixels, and synthesises input into them, all driven by clients that can
 * misbehave. In the compositor a fault there takes the entire session down with
 * it. The same reasoning keeps discord-voiced out of the bar. One extra process
 * at login is a cheap price for a tray bug that cannot log you out.
 *
 * Deliberately NOT a KDE dependency: xembedsniproxy does this job, but it is a
 * Plasma component, and the tray was built specifically so that asteroidz needs
 * no part of KDE or GNOME installed.
 *
 * Protocol references: the freedesktop System Tray Protocol and XEMBED
 * specifications; org.kde.StatusNotifierItem as implemented by the compositor's
 * own watcher.
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <systemd/sd-bus.h>
#include <xcb/composite.h>
#include <xcb/damage.h>
#include <xcb/xcb.h>

#define MAX_ICONS 32
/* The size icons are asked to render at. The tray scales whatever it gets, but
 * asking for something sane keeps toolkits from picking a 1x1 or a 256x256. */
#define ICON_SIZE 24

/* System Tray Protocol */
#define SYSTEM_TRAY_REQUEST_DOCK 0
/* XEMBED */
#define XEMBED_EMBEDDED_NOTIFY 0
#define XEMBED_WINDOW_ACTIVATE 1
#define XEMBED_VERSION 0
#define XEMBED_MAPPED (1 << 0)

typedef struct {
	xcb_window_t win;       /* the client's icon window */
	xcb_window_t container; /* our wrapper, parked offscreen */
	xcb_damage_damage_t damage;
	uint32_t *argb;         /* last captured pixels, ARGB32 premultiplied */
	int w, h;
	char path[64];          /* our object path for this item */
	char title[128];
	bool used;
	bool dirty;
} Icon;

static xcb_connection_t *conn;
static xcb_screen_t *screen;
static xcb_window_t selection_win;
static xcb_atom_t atom_tray_selection, atom_tray_opcode, atom_tray_orientation,
	atom_tray_visual, atom_xembed, atom_xembed_info, atom_manager,
	atom_wm_name, atom_utf8;
static uint8_t damage_event_base;
static Icon icons[MAX_ICONS];
static sd_bus *bus;
static bool running = true;

/* ─── x11 helpers ─────────────────────────────────────────────────────────── */

static xcb_atom_t intern(const char *name) {
	xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(
		conn, xcb_intern_atom(conn, 0, (uint16_t)strlen(name), name), NULL);
	xcb_atom_t a = r ? r->atom : XCB_ATOM_NONE;
	free(r);
	return a;
}

static void send_xembed(xcb_window_t win, uint32_t message, uint32_t detail,
						uint32_t data1, uint32_t data2) {
	xcb_client_message_event_t ev = {0};
	ev.response_type = XCB_CLIENT_MESSAGE;
	ev.format = 32;
	ev.window = win;
	ev.type = atom_xembed;
	ev.data.data32[0] = XCB_CURRENT_TIME;
	ev.data.data32[1] = message;
	ev.data.data32[2] = detail;
	ev.data.data32[3] = data1;
	ev.data.data32[4] = data2;
	xcb_send_event(conn, 0, win, XCB_EVENT_MASK_NO_EVENT, (const char *)&ev);
}

static Icon *icon_by_window(xcb_window_t win) {
	for (int i = 0; i < MAX_ICONS; i++)
		if (icons[i].used && icons[i].win == win)
			return &icons[i];
	return NULL;
}

static Icon *icon_by_damage(xcb_damage_damage_t d) {
	for (int i = 0; i < MAX_ICONS; i++)
		if (icons[i].used && icons[i].damage == d)
			return &icons[i];
	return NULL;
}

/* Read the embedded window's pixels.
 *
 * The container is composite-redirected, so the window renders into offscreen
 * storage rather than onto the root -- which is the whole point: the icon must
 * never actually appear on screen, only in the bar. GetImage on a redirected
 * window reads that storage. */
static bool icon_capture(Icon *ic) {
	xcb_get_geometry_reply_t *geo =
		xcb_get_geometry_reply(conn, xcb_get_geometry(conn, ic->win), NULL);
	if (!geo)
		return false;
	int w = geo->width, h = geo->height;
	uint8_t depth = geo->depth;
	free(geo);
	if (w <= 0 || h <= 0 || w > 512 || h > 512)
		return false;

	xcb_generic_error_t *err = NULL;
	xcb_get_image_reply_t *img = xcb_get_image_reply(
		conn,
		xcb_get_image(conn, XCB_IMAGE_FORMAT_Z_PIXMAP, ic->win, 0, 0,
					  (uint16_t)w, (uint16_t)h, ~0u),
		&err);
	if (!img) {
		free(err);
		return false;
	}
	uint8_t *data = xcb_get_image_data(img);
	int len = xcb_get_image_data_length(img);
	if (len < w * h * 4) {
		free(img);
		return false;
	}

	uint32_t *px = calloc((size_t)w * h, sizeof(uint32_t));
	if (!px) {
		free(img);
		return false;
	}
	for (int i = 0; i < w * h; i++) {
		uint32_t v;
		memcpy(&v, data + i * 4, 4);
		/* A depth-24 icon carries no alpha; the byte is undefined rather than
		 * opaque, and taking it literally renders the icon fully transparent. */
		if (depth == 24)
			v |= 0xff000000u;
		px[i] = v;
	}
	free(img);
	free(ic->argb);
	ic->argb = px;
	ic->w = w;
	ic->h = h;
	return true;
}

static void icon_read_title(Icon *ic) {
	xcb_get_property_reply_t *r = xcb_get_property_reply(
		conn,
		xcb_get_property(conn, 0, ic->win, atom_wm_name, XCB_ATOM_ANY, 0, 64),
		NULL);
	ic->title[0] = '\0';
	if (r) {
		int len = xcb_get_property_value_length(r);
		if (len > 0) {
			if (len >= (int)sizeof(ic->title))
				len = (int)sizeof(ic->title) - 1;
			memcpy(ic->title, xcb_get_property_value(r), (size_t)len);
			ic->title[len] = '\0';
		}
		free(r);
	}
	if (!ic->title[0])
		snprintf(ic->title, sizeof(ic->title), "tray icon 0x%x", ic->win);
}

/* ─── StatusNotifierItem export ───────────────────────────────────────────── */

static int prop_icon_pixmap(sd_bus *b, const char *path, const char *iface,
							const char *prop, sd_bus_message *reply,
							void *userdata, sd_bus_error *err) {
	(void)b; (void)path; (void)iface; (void)prop; (void)err;
	Icon *ic = userdata;
	/* a(iiay): width, height, ARGB32 network byte order -- the SNI spec's
	 * pixmap form, which is what a window's pixels can be expressed as
	 * without inventing a themed icon name for them */
	int r = sd_bus_message_open_container(reply, 'a', "(iiay)");
	if (r < 0)
		return r;
	if (ic->argb && ic->w > 0 && ic->h > 0) {
		r = sd_bus_message_open_container(reply, 'r', "iiay");
		if (r < 0)
			return r;
		sd_bus_message_append(reply, "ii", ic->w, ic->h);
		size_t n = (size_t)ic->w * ic->h;
		uint32_t *be = malloc(n * 4);
		if (!be)
			return -ENOMEM;
		for (size_t i = 0; i < n; i++) {
			uint32_t v = ic->argb[i];
			be[i] = __builtin_bswap32(v); /* spec says network byte order */
		}
		r = sd_bus_message_append_array(reply, 'y', be, n * 4);
		free(be);
		if (r < 0)
			return r;
		r = sd_bus_message_close_container(reply);
		if (r < 0)
			return r;
	}
	return sd_bus_message_close_container(reply);
}

static int prop_str(sd_bus *b, const char *path, const char *iface,
					const char *prop, sd_bus_message *reply, void *userdata,
					sd_bus_error *err) {
	(void)b; (void)path; (void)iface; (void)err;
	Icon *ic = userdata;
	if (!strcmp(prop, "Id") || !strcmp(prop, "Title"))
		return sd_bus_message_append(reply, "s", ic->title);
	if (!strcmp(prop, "Status"))
		return sd_bus_message_append(reply, "s", "Active");
	if (!strcmp(prop, "Category"))
		return sd_bus_message_append(reply, "s", "ApplicationStatus");
	return sd_bus_message_append(reply, "s", "");
}

/* Click an embedded icon.
 *
 * XEmbed clients expect real button events on their own window. The window is
 * parked offscreen and composited, so there is no pointer to put over it --
 * the events are synthesised and sent directly, which is what every bridge of
 * this kind does. Coordinates are the icon's own centre rather than the
 * screen position of the pill, because the client interprets them in its own
 * coordinate space. */
static void icon_click(Icon *ic, uint8_t button) {
	xcb_button_press_event_t ev = {0};
	ev.response_type = XCB_BUTTON_PRESS;
	ev.detail = button;
	ev.event = ic->win;
	ev.child = XCB_NONE;
	ev.root = screen->root;
	ev.event_x = (int16_t)(ic->w / 2);
	ev.event_y = (int16_t)(ic->h / 2);
	ev.root_x = (int16_t)(ic->w / 2);
	ev.root_y = (int16_t)(ic->h / 2);
	ev.time = XCB_CURRENT_TIME;
	ev.same_screen = 1;
	xcb_send_event(conn, 0, ic->win, XCB_EVENT_MASK_BUTTON_PRESS,
				   (const char *)&ev);
	ev.response_type = XCB_BUTTON_RELEASE;
	xcb_send_event(conn, 0, ic->win, XCB_EVENT_MASK_BUTTON_RELEASE,
				   (const char *)&ev);
	xcb_flush(conn);
}

static int method_activate(sd_bus_message *m, void *userdata,
						   sd_bus_error *err) {
	(void)err;
	icon_click(userdata, 1);
	return sd_bus_reply_method_return(m, "");
}

static int method_secondary(sd_bus_message *m, void *userdata,
							sd_bus_error *err) {
	(void)err;
	icon_click(userdata, 2);
	return sd_bus_reply_method_return(m, "");
}

static int method_context(sd_bus_message *m, void *userdata,
						  sd_bus_error *err) {
	(void)err;
	/* right button: an XEmbed icon has no DBusMenu, its menu is whatever the
	 * client itself pops up in response to the click */
	icon_click(userdata, 3);
	return sd_bus_reply_method_return(m, "");
}

static int method_scroll(sd_bus_message *m, void *userdata, sd_bus_error *err) {
	(void)err;
	int delta = 0;
	const char *orientation = NULL;
	if (sd_bus_message_read(m, "is", &delta, &orientation) < 0)
		return sd_bus_reply_method_return(m, "");
	icon_click(userdata, delta > 0 ? 4 : 5);
	return sd_bus_reply_method_return(m, "");
}

static const sd_bus_vtable sni_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_PROPERTY("Category", "s", prop_str, 0,
					SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	SD_BUS_PROPERTY("Id", "s", prop_str, 0,
					SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	SD_BUS_PROPERTY("Title", "s", prop_str, 0,
					SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	SD_BUS_PROPERTY("Status", "s", prop_str, 0,
					SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	SD_BUS_PROPERTY("IconPixmap", "a(iiay)", prop_icon_pixmap, 0,
					SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	SD_BUS_METHOD("Activate", "ii", "", method_activate,
				  SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("SecondaryActivate", "ii", "", method_secondary,
				  SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("ContextMenu", "ii", "", method_context,
				  SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("Scroll", "is", "", method_scroll,
				  SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_SIGNAL("NewIcon", "", 0),
	SD_BUS_SIGNAL("NewTitle", "", 0),
	SD_BUS_SIGNAL("NewStatus", "s", 0),
	SD_BUS_VTABLE_END,
};

static void icon_register(Icon *ic) {
	if (sd_bus_add_object_vtable(bus, NULL, ic->path,
								 "org.kde.StatusNotifierItem", sni_vtable,
								 ic) < 0)
		return;
	const char *unique = NULL;
	if (sd_bus_get_unique_name(bus, &unique) < 0 || !unique)
		return;
	/* Register as service+path concatenated. One process hosting several items
	 * cannot identify them by bus name alone, so the path carries the
	 * distinction -- the form the compositor's watcher parses. */
	char arg[192];
	snprintf(arg, sizeof(arg), "%s%s", unique, ic->path);
	sd_bus_call_method_async(bus, NULL, "org.kde.StatusNotifierWatcher",
							 "/StatusNotifierWatcher",
							 "org.kde.StatusNotifierWatcher",
							 "RegisterStatusNotifierItem", NULL, NULL, "s",
							 arg);
}

/* ─── docking ─────────────────────────────────────────────────────────────── */

static void icon_undock(Icon *ic) {
	if (!ic->used)
		return;
	if (ic->damage)
		xcb_damage_destroy(conn, ic->damage);
	if (ic->container)
		xcb_destroy_window(conn, ic->container);
	sd_bus_emit_signal(bus, ic->path, "org.kde.StatusNotifierItem", "NewStatus",
					   "s", "Passive");
	free(ic->argb);
	memset(ic, 0, sizeof(*ic));
	xcb_flush(conn);
}

static void dock(xcb_window_t win) {
	if (icon_by_window(win))
		return;
	Icon *ic = NULL;
	for (int i = 0; i < MAX_ICONS; i++)
		if (!icons[i].used) {
			ic = &icons[i];
			break;
		}
	if (!ic)
		return;

	memset(ic, 0, sizeof(*ic));
	ic->used = true;
	ic->win = win;

	/* The wrapper lives far offscreen. It must be MAPPED for the client to
	 * render into it at all, and composite-redirected so those pixels never
	 * reach the root window -- an icon that flickered in the corner of the
	 * screen would be worse than no icon. */
	ic->container = xcb_generate_id(conn);
	uint32_t values[] = {1 /* override_redirect */,
						 XCB_EVENT_MASK_STRUCTURE_NOTIFY |
							 XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY};
	xcb_create_window(conn, XCB_COPY_FROM_PARENT, ic->container, screen->root,
					  -1000, -1000, ICON_SIZE, ICON_SIZE, 0,
					  XCB_WINDOW_CLASS_INPUT_OUTPUT, XCB_COPY_FROM_PARENT,
					  XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK, values);

	uint32_t mask = XCB_EVENT_MASK_STRUCTURE_NOTIFY |
					XCB_EVENT_MASK_PROPERTY_CHANGE;
	xcb_change_window_attributes(conn, win, XCB_CW_EVENT_MASK, &mask);
	xcb_reparent_window(conn, win, ic->container, 0, 0);

	uint32_t geom[] = {ICON_SIZE, ICON_SIZE};
	xcb_configure_window(conn, win,
						 XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
						 geom);

	send_xembed(win, XEMBED_EMBEDDED_NOTIFY, 0, ic->container, XEMBED_VERSION);
	xcb_map_window(conn, win);
	xcb_map_window(conn, ic->container);
	xcb_composite_redirect_window(conn, ic->container,
								  XCB_COMPOSITE_REDIRECT_MANUAL);
	ic->damage = xcb_generate_id(conn);
	xcb_damage_create(conn, ic->damage, win,
					  XCB_DAMAGE_REPORT_LEVEL_NON_EMPTY);
	xcb_flush(conn);

	icon_read_title(ic);
	snprintf(ic->path, sizeof(ic->path), "/StatusNotifierItem/x%u", win);
	icon_capture(ic);
	icon_register(ic);
	fprintf(stderr, "asteroidz-xembed: docked 0x%x (%s)\n", win, ic->title);
}

/* ─── selection ownership ─────────────────────────────────────────────────── */

static bool acquire_selection(int screen_num) {
	char name[64];
	snprintf(name, sizeof(name), "_NET_SYSTEM_TRAY_S%d", screen_num);
	atom_tray_selection = intern(name);

	selection_win = xcb_generate_id(conn);
	uint32_t values[] = {1, XCB_EVENT_MASK_STRUCTURE_NOTIFY |
							   XCB_EVENT_MASK_PROPERTY_CHANGE};
	xcb_create_window(conn, XCB_COPY_FROM_PARENT, selection_win, screen->root,
					  -1, -1, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
					  XCB_COPY_FROM_PARENT,
					  XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK, values);

	uint32_t horizontal = 0;
	xcb_change_property(conn, XCB_PROP_MODE_REPLACE, selection_win,
						atom_tray_orientation, XCB_ATOM_CARDINAL, 32, 1,
						&horizontal);

	xcb_set_selection_owner(conn, selection_win, atom_tray_selection,
							XCB_CURRENT_TIME);
	xcb_get_selection_owner_reply_t *owner = xcb_get_selection_owner_reply(
		conn, xcb_get_selection_owner(conn, atom_tray_selection), NULL);
	bool ok = owner && owner->owner == selection_win;
	free(owner);
	if (!ok)
		return false;

	/* Tell every client the tray exists. Well-behaved toolkits watch for this
	 * and re-dock, so applications started before us get their icon back
	 * without being restarted. */
	xcb_client_message_event_t ev = {0};
	ev.response_type = XCB_CLIENT_MESSAGE;
	ev.format = 32;
	ev.window = screen->root;
	ev.type = atom_manager;
	ev.data.data32[0] = XCB_CURRENT_TIME;
	ev.data.data32[1] = atom_tray_selection;
	ev.data.data32[2] = selection_win;
	xcb_send_event(conn, 0, screen->root, XCB_EVENT_MASK_STRUCTURE_NOTIFY,
				   (const char *)&ev);
	xcb_flush(conn);
	return true;
}

/* ─── main ────────────────────────────────────────────────────────────────── */

static void handle_x_event(xcb_generic_event_t *ev) {
	uint8_t type = ev->response_type & 0x7f;

	if (damage_event_base && type == damage_event_base + XCB_DAMAGE_NOTIFY) {
		xcb_damage_notify_event_t *d = (xcb_damage_notify_event_t *)ev;
		Icon *ic = icon_by_damage(d->damage);
		if (ic) {
			xcb_damage_subtract(conn, ic->damage, XCB_NONE, XCB_NONE);
			if (icon_capture(ic))
				sd_bus_emit_signal(bus, ic->path,
								   "org.kde.StatusNotifierItem", "NewIcon",
								   "");
		}
		return;
	}

	switch (type) {
	case XCB_CLIENT_MESSAGE: {
		xcb_client_message_event_t *cm = (xcb_client_message_event_t *)ev;
		if (cm->type == atom_tray_opcode &&
			cm->data.data32[1] == SYSTEM_TRAY_REQUEST_DOCK)
			dock((xcb_window_t)cm->data.data32[2]);
		break;
	}
	case XCB_DESTROY_NOTIFY: {
		xcb_destroy_notify_event_t *dn = (xcb_destroy_notify_event_t *)ev;
		Icon *ic = icon_by_window(dn->window);
		if (ic) {
			fprintf(stderr, "asteroidz-xembed: undocked 0x%x\n", ic->win);
			icon_undock(ic);
		}
		break;
	}
	case XCB_UNMAP_NOTIFY: {
		xcb_unmap_notify_event_t *un = (xcb_unmap_notify_event_t *)ev;
		Icon *ic = icon_by_window(un->window);
		/* An icon that unmaps itself is asking to be hidden, not destroyed;
		 * the client may map it again. Reported Passive rather than removed. */
		if (ic)
			sd_bus_emit_signal(bus, ic->path, "org.kde.StatusNotifierItem",
							   "NewStatus", "s", "Passive");
		break;
	}
	case XCB_PROPERTY_NOTIFY: {
		xcb_property_notify_event_t *pn = (xcb_property_notify_event_t *)ev;
		Icon *ic = icon_by_window(pn->window);
		if (ic && pn->atom == atom_wm_name) {
			icon_read_title(ic);
			sd_bus_emit_signal(bus, ic->path, "org.kde.StatusNotifierItem",
							   "NewTitle", "");
		}
		break;
	}
	case XCB_SELECTION_CLEAR:
		/* someone else took the tray selection: stand down rather than fight */
		fprintf(stderr, "asteroidz-xembed: lost the tray selection, exiting\n");
		running = false;
		break;
	default:
		break;
	}
}

int main(void) {
	int screen_num = 0;
	conn = xcb_connect(NULL, &screen_num);
	if (!conn || xcb_connection_has_error(conn)) {
		fprintf(stderr, "asteroidz-xembed: cannot connect to X (DISPLAY=%s)\n",
				getenv("DISPLAY") ? getenv("DISPLAY") : "(unset)");
		return 1;
	}
	const xcb_setup_t *setup = xcb_get_setup(conn);
	xcb_screen_iterator_t it = xcb_setup_roots_iterator(setup);
	for (int i = 0; i < screen_num; i++)
		xcb_screen_next(&it);
	screen = it.data;

	atom_tray_opcode = intern("_NET_SYSTEM_TRAY_OPCODE");
	atom_tray_orientation = intern("_NET_SYSTEM_TRAY_ORIENTATION");
	atom_tray_visual = intern("_NET_SYSTEM_TRAY_VISUAL");
	atom_xembed = intern("_XEMBED");
	atom_xembed_info = intern("_XEMBED_INFO");
	atom_manager = intern("MANAGER");
	atom_wm_name = intern("_NET_WM_NAME");
	atom_utf8 = intern("UTF8_STRING");
	(void)atom_tray_visual;
	(void)atom_xembed_info;
	(void)atom_utf8;

	const xcb_query_extension_reply_t *dmg =
		xcb_get_extension_data(conn, &xcb_damage_id);
	if (!dmg || !dmg->present) {
		fprintf(stderr, "asteroidz-xembed: no XDAMAGE, cannot track icons\n");
		return 1;
	}
	damage_event_base = dmg->first_event;
	xcb_damage_query_version(conn, XCB_DAMAGE_MAJOR_VERSION,
							 XCB_DAMAGE_MINOR_VERSION);
	xcb_composite_query_version(conn, 0, 4);

	if (sd_bus_open_user(&bus) < 0) {
		fprintf(stderr, "asteroidz-xembed: no session bus\n");
		return 1;
	}

	if (!acquire_selection(screen_num)) {
		/* Another tray owns it. Two owners would each embed half the icons. */
		fprintf(stderr, "asteroidz-xembed: _NET_SYSTEM_TRAY_S%d already owned; "
						"another tray is running\n",
				screen_num);
		return 1;
	}
	fprintf(stderr, "asteroidz-xembed: owning _NET_SYSTEM_TRAY_S%d\n",
			screen_num);

	struct pollfd fds[2] = {
		{.fd = xcb_get_file_descriptor(conn), .events = POLLIN},
		{.fd = sd_bus_get_fd(bus), .events = POLLIN},
	};

	while (running) {
		/* drain both sides before sleeping, or a burst on one starves the
		 * other and icons stop updating while the bus is busy */
		xcb_generic_event_t *ev;
		while ((ev = xcb_poll_for_event(conn))) {
			handle_x_event(ev);
			free(ev);
		}
		xcb_flush(conn);
		while (sd_bus_process(bus, NULL) > 0)
			;
		if (xcb_connection_has_error(conn)) {
			fprintf(stderr, "asteroidz-xembed: X connection lost\n");
			break;
		}
		if (poll(fds, 2, 1000) < 0 && errno != EINTR)
			break;
	}

	for (int i = 0; i < MAX_ICONS; i++)
		if (icons[i].used)
			icon_undock(&icons[i]);
	sd_bus_flush_close_unref(bus);
	xcb_disconnect(conn);
	return 0;
}
