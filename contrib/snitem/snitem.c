// snitem — a stand-in StatusNotifierItem, for testing the bar's tray.
//
// Owns the conventional well-known name an application's tray icon takes
// (org.kde.StatusNotifierItem-<pid>-1), exports a minimal item object at
// /StatusNotifierItem, and sits there until it is killed.
//
// That is deliberately all it does. A watcher can only ever see two things
// about an item -- that the name is owned, and what GetAll on the item
// interface returns -- so this is a complete item as far as anything under
// test is concerned, with no Discord, no Steam and no tray applet needed.
//
// Usage: snitem [--register] [--passive-then-active] [--pixmap N] [--log F]
//   --register  also call RegisterStatusNotifierItem on the watcher, i.e.
//               behave like an application that starts AFTER the compositor.
//               Without it the name simply exists, which is the state an
//               application is left in when the compositor restarts under it.
//   --passive-then-active
//               start Passive, and go Active on SIGUSR1 -- announcing it the
//               STANDARD way, org.freedesktop.DBus.Properties.
//               PropertiesChanged, and NOT the legacy NewStatus signal. That
//               is what a modern item does (quickshell annotates its
//               properties `emits-change`), and a host that only listens for
//               the legacy signals keeps the stale Passive -- which it hides,
//               so the application looks like it fell out of the tray.
//               On a signal rather than a timer so a test can decide when it
//               happens instead of racing one.
//   --pixmap N  serve an N x N IconPixmap instead of relying on IconName.
//               This is the one thing a tray host takes from an application
//               that is UNBOUNDED: the host decodes whatever dimensions are
//               sent. Pass a large N to check that a host refuses rather than
//               decoding it -- an unbounded one will happily chew through
//               however many megapixels it is handed, which in-compositor is
//               a stalled desktop and is why asteroidz-trayd exists.
//   --log F     append each Activate/SecondaryActivate/ContextMenu call to F,
//               as "<method> <x> <y>", so a test can assert that a click made
//               it all the way from the bar to the item.
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <systemd/sd-bus.h>
#include <unistd.h>

static const char *ITEM_IFACE = "org.kde.StatusNotifierItem";

/* Mutable so --passive-then-active can flip it under a live connection. */
static const char *item_status = "Active";
static volatile sig_atomic_t go_active = 0;
static int pixmap_size = 0;
static const char *click_log = NULL;

static void on_sigusr1(int sig) {
	(void)sig;
	go_active = 1;
}

static int prop_str(sd_bus *bus, const char *path, const char *iface,
					const char *prop, sd_bus_message *reply, void *user,
					sd_bus_error *err) {
	(void)bus; (void)path; (void)iface; (void)user; (void)err;
	const char *v = "snitem";
	if (!strcmp(prop, "Status"))
		v = item_status;
	else if (!strcmp(prop, "IconName"))
		/* Empty when a pixmap was asked for: a host prefers a NAME over a
		   pixmap (it is themed and scalable), so an item offering both would
		   never exercise the decode path at all. */
		v = pixmap_size > 0 ? "" : "dialog-information";
	else if (!strcmp(prop, "Category"))
		v = "ApplicationStatus";
	return sd_bus_message_append(reply, "s", v);
}

/* A solid opaque square. Content does not matter -- what is being tested is
   whether a host decodes it at all, and at what size it declines to. */
static int prop_pixmap(sd_bus *bus, const char *path, const char *iface,
					   const char *prop, sd_bus_message *reply, void *user,
					   sd_bus_error *err) {
	(void)bus; (void)path; (void)iface; (void)prop; (void)user; (void)err;
	int n = pixmap_size;
	int r = sd_bus_message_open_container(reply, 'a', "(iiay)");
	if (r < 0)
		return r;
	if (n > 0) {
		size_t bytes = (size_t)n * n * 4;
		unsigned char *px = malloc(bytes);
		if (!px)
			return -ENOMEM;
		memset(px, 0xFF, bytes);
		if ((r = sd_bus_message_open_container(reply, 'r', "iiay")) >= 0) {
			sd_bus_message_append(reply, "ii", n, n);
			sd_bus_message_append_array(reply, 'y', px, bytes);
			sd_bus_message_close_container(reply);
		}
		free(px);
		if (r < 0)
			return r;
	}
	return sd_bus_message_close_container(reply);
}

static int method_action(sd_bus_message *m, void *user, sd_bus_error *err) {
	(void)user; (void)err;
	int x = 0, y = 0;
	sd_bus_message_read(m, "ii", &x, &y);
	if (click_log) {
		FILE *f = fopen(click_log, "a");
		if (f) {
			fprintf(f, "%s %d %d\n", sd_bus_message_get_member(m), x, y);
			fclose(f);
		}
	}
	return sd_bus_reply_method_return(m, "");
}

static const sd_bus_vtable item_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_PROPERTY("IconPixmap", "a(iiay)", prop_pixmap, 0,
					SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_METHOD("Activate", "ii", "", method_action,
				  SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("SecondaryActivate", "ii", "", method_action,
				  SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("ContextMenu", "ii", "", method_action,
				  SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_PROPERTY("Id", "s", prop_str, 0, SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_PROPERTY("Title", "s", prop_str, 0, SD_BUS_VTABLE_PROPERTY_CONST),
	/* EMITS_CHANGE, not CONST: this one is allowed to change, and saying so is
	   what makes sd_bus_emit_properties_changed legal on it. */
	SD_BUS_PROPERTY("Status", "s", prop_str, 0,
					SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	SD_BUS_PROPERTY("Category", "s", prop_str, 0, SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_PROPERTY("IconName", "s", prop_str, 0, SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_VTABLE_END,
};

int main(int argc, char **argv) {
	bool do_register = false, passive_first = false;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--register"))
			do_register = true;
		else if (!strcmp(argv[i], "--passive-then-active"))
			passive_first = true;
		else if (!strcmp(argv[i], "--pixmap") && i + 1 < argc)
			pixmap_size = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--log") && i + 1 < argc)
			click_log = argv[++i];
	}
	if (passive_first)
		item_status = "Passive";

	sd_bus *bus = NULL;
	if (sd_bus_open_user(&bus) < 0) {
		fprintf(stderr, "snitem: no session bus\n");
		return 1;
	}

	sd_bus_slot *slot = NULL;
	if (sd_bus_add_object_vtable(bus, &slot, "/StatusNotifierItem", ITEM_IFACE,
								 item_vtable, NULL) < 0) {
		fprintf(stderr, "snitem: cannot export the item object\n");
		return 1;
	}

	char name[128];
	snprintf(name, sizeof(name), "org.kde.StatusNotifierItem-%d-1", (int)getpid());
	if (sd_bus_request_name(bus, name, 0) < 0) {
		fprintf(stderr, "snitem: cannot own %s\n", name);
		return 1;
	}
	printf("%s\n", name);
	fflush(stdout);

	if (do_register) {
		sd_bus_call_method_async(bus, NULL, "org.kde.StatusNotifierWatcher",
								 "/StatusNotifierWatcher",
								 "org.kde.StatusNotifierWatcher",
								 "RegisterStatusNotifierItem", NULL, NULL, "s",
								 name);
		sd_bus_flush(bus);
	}

	/* Go Active on SIGUSR1, announcing it only through PropertiesChanged. */
	if (passive_first)
		signal(SIGUSR1, on_sigusr1);
	for (;;) {
		int r = sd_bus_process(bus, NULL);
		if (r < 0)
			break;
		if (r > 0)
			continue;
		if (passive_first && go_active) {
			go_active = 0;
			passive_first = false;
			item_status = "Active";
			sd_bus_emit_properties_changed(bus, "/StatusNotifierItem",
										   ITEM_IFACE, "Status", NULL);
			sd_bus_flush(bus);
			printf("active\n");
			fflush(stdout);
			continue;
		}
		/* Short slices so the signal above is noticed promptly; sd_bus_wait
		   returns early on EINTR anyway. */
		if (sd_bus_wait(bus, 250 * 1000) < 0 && errno != EINTR)
			break;
	}
	sd_bus_slot_unref(slot);
	sd_bus_unref(bus);
	return 0;
}
