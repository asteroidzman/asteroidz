/*
 * Every xdg-desktop-portal backend asteroidz implements, brought up together.
 *
 * Three steps rather than one call per backend, because the bus name has to be
 * taken last: xdg-desktop-portal can dispatch a method the instant the name
 * appears, and a call landing on a path nothing is exported on is an error the
 * application never had a way to avoid. See portal-bus.h.
 */
#include "portal-bus.h"

#include "global-shortcuts-portal.h"
#include "inhibit-portal.h"

static void portals_init(void) {
	bool have_bus = portal_bus_connect();
	/* Called even with no bus: each backend still initialises its own lists,
	 * and everything that reads them -- keypress(), checkidleinhibitor(), the
	 * IPC -- runs whether or not a session bus exists. */
	global_shortcuts_portal_init();
	inhibit_portal_init();
	if (have_bus)
		portal_bus_publish();
}

static void portals_finish(void) {
	global_shortcuts_portal_finish();
	inhibit_portal_finish();
	portal_bus_finish();
}
