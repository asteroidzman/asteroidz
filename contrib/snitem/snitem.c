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
// Usage: snitem [--register]
//   --register  also call RegisterStatusNotifierItem on the watcher, i.e.
//               behave like an application that starts AFTER the compositor.
//               Without it the name simply exists, which is the state an
//               application is left in when the compositor restarts under it.
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <systemd/sd-bus.h>
#include <unistd.h>

static const char *ITEM_IFACE = "org.kde.StatusNotifierItem";

static int prop_str(sd_bus *bus, const char *path, const char *iface,
					const char *prop, sd_bus_message *reply, void *user,
					sd_bus_error *err) {
	(void)bus; (void)path; (void)iface; (void)user; (void)err;
	const char *v = "snitem";
	if (!strcmp(prop, "Status"))
		v = "Active";
	else if (!strcmp(prop, "IconName"))
		v = "dialog-information";
	else if (!strcmp(prop, "Category"))
		v = "ApplicationStatus";
	return sd_bus_message_append(reply, "s", v);
}

static const sd_bus_vtable item_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_PROPERTY("Id", "s", prop_str, 0, SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_PROPERTY("Title", "s", prop_str, 0, SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_PROPERTY("Status", "s", prop_str, 0, SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_PROPERTY("Category", "s", prop_str, 0, SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_PROPERTY("IconName", "s", prop_str, 0, SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_VTABLE_END,
};

int main(int argc, char **argv) {
	bool do_register = argc > 1 && !strcmp(argv[1], "--register");

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

	for (;;) {
		int r = sd_bus_process(bus, NULL);
		if (r < 0)
			break;
		if (r > 0)
			continue;
		if (sd_bus_wait(bus, (uint64_t)-1) < 0)
			break;
	}
	sd_bus_slot_unref(slot);
	sd_bus_unref(bus);
	return 0;
}
