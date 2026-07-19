// wlvptr — tiny wlr-virtual-pointer-unstable-v1 client for headless testing.
//
// Injects a synthetic pointer move (+ optional click/scroll) directly into
// whichever compositor WAYLAND_DISPLAY/XDG_RUNTIME_DIR point at, via the
// Wayland protocol -- unlike ydotool (uinput/kernel-level, routes to
// whichever seat is currently active system-wide, which is NOT scoped to a
// headless test compositor and can leak into a live session), this only
// ever reaches the one compositor instance named by the environment.
//
// Usage: wlvptr <x> <y> <extent_w> <extent_h> [click|rclick|mclick|scroll:<amount>|drag:<x2>,<y2>|rdrag:<x2>,<y2>]
//   x,y            starting position (pixels) within a 0..extent_w/h space --
//                  pass the real output width/height as the extent so x,y
//                  are plain absolute pixel coordinates.
//   click          left-button press + release
//   rclick         right-button press + release
//   mclick         middle-button press + release
//   scroll:<amt>   vertical scroll (fixed-point amount, positive = down)
//   drag:<x2>,<y2>   left-button press at x,y, move in steps to x2,y2, release
//                    -- for testing a Super+drag-style mouse binding, run
//                    this via `wlvkbd hold LEFTMETA -- wlvptr ... drag:...`
//                    so the modifier is actually held during the button
//                    press (a real drag needs intermediate motion events
//                    between press and release, not just an instant jump,
//                    for the compositor's move/resize grab to track it).
//   rdrag:<x2>,<y2>  same, right button (for testing a resize binding)

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

#define BTN_LEFT 0x110
#define BTN_RIGHT 0x111
#define BTN_MIDDLE 0x112

static struct zwlr_virtual_pointer_manager_v1 *manager = NULL;

static void registry_global(void *data, struct wl_registry *registry,
							 uint32_t name, const char *interface,
							 uint32_t version) {
	(void)data;
	(void)version;
	if (!strcmp(interface, zwlr_virtual_pointer_manager_v1_interface.name)) {
		manager = wl_registry_bind(registry, name,
								   &zwlr_virtual_pointer_manager_v1_interface, 1);
	}
}

static void registry_global_remove(void *data, struct wl_registry *registry,
									uint32_t name) {
	(void)data;
	(void)registry;
	(void)name;
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

int main(int argc, char **argv) {
	if (argc < 5) {
		fprintf(stderr,
				"usage: %s <x> <y> <extent_w> <extent_h> [click|rclick|mclick|scroll:<amt>]\n",
				argv[0]);
		return 1;
	}
	uint32_t x = (uint32_t)atoi(argv[1]);
	uint32_t y = (uint32_t)atoi(argv[2]);
	uint32_t ew = (uint32_t)atoi(argv[3]);
	uint32_t eh = (uint32_t)atoi(argv[4]);
	const char *action = argc > 5 ? argv[5] : NULL;

	struct wl_display *display = wl_display_connect(NULL);
	if (!display) {
		fprintf(stderr, "wlvptr: could not connect to a Wayland display "
						"(check WAYLAND_DISPLAY/XDG_RUNTIME_DIR)\n");
		return 1;
	}

	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);

	if (!manager) {
		fprintf(stderr, "wlvptr: compositor does not advertise "
						"zwlr_virtual_pointer_manager_v1\n");
		return 1;
	}

	struct zwlr_virtual_pointer_v1 *ptr =
		zwlr_virtual_pointer_manager_v1_create_virtual_pointer(manager, NULL);

	uint32_t t = 0;
	zwlr_virtual_pointer_v1_motion_absolute(ptr, t, x, y, ew, eh);
	zwlr_virtual_pointer_v1_frame(ptr);
	wl_display_roundtrip(display);

	if (action) {
		if (!strcmp(action, "click") || !strcmp(action, "rclick") ||
			!strcmp(action, "mclick")) {
			uint32_t button = !strcmp(action, "click")	 ? BTN_LEFT
							  : !strcmp(action, "rclick") ? BTN_RIGHT
														   : BTN_MIDDLE;
			zwlr_virtual_pointer_v1_button(ptr, ++t, button,
											WL_POINTER_BUTTON_STATE_PRESSED);
			zwlr_virtual_pointer_v1_frame(ptr);
			wl_display_roundtrip(display);
			zwlr_virtual_pointer_v1_button(ptr, ++t, button,
											WL_POINTER_BUTTON_STATE_RELEASED);
			zwlr_virtual_pointer_v1_frame(ptr);
			wl_display_roundtrip(display);
		} else if (!strncmp(action, "scroll:", 7)) {
			double amt = atof(action + 7);
			zwlr_virtual_pointer_v1_axis(ptr, ++t, WL_POINTER_AXIS_VERTICAL_SCROLL,
										  wl_fixed_from_double(amt));
			zwlr_virtual_pointer_v1_frame(ptr);
			wl_display_roundtrip(display);
		} else if (!strncmp(action, "drag:", 5) || !strncmp(action, "rdrag:", 6)) {
			bool right = !strncmp(action, "rdrag:", 6);
			const char *coords = action + (right ? 6 : 5);
			int32_t x2 = 0, y2 = 0;
			if (sscanf(coords, "%d,%d", &x2, &y2) != 2) {
				fprintf(stderr, "wlvptr: bad drag coords: %s\n", coords);
				return 1;
			}
			uint32_t button = right ? BTN_RIGHT : BTN_LEFT;

			zwlr_virtual_pointer_v1_button(ptr, ++t, button,
											WL_POINTER_BUTTON_STATE_PRESSED);
			zwlr_virtual_pointer_v1_frame(ptr);
			wl_display_roundtrip(display);

			const int steps = 10;
			for (int i = 1; i <= steps; i++) {
				int32_t sx = (int32_t)x + ((x2 - (int32_t)x) * i) / steps;
				int32_t sy = (int32_t)y + ((y2 - (int32_t)y) * i) / steps;
				zwlr_virtual_pointer_v1_motion_absolute(
					ptr, ++t, (uint32_t)sx, (uint32_t)sy, ew, eh);
				zwlr_virtual_pointer_v1_frame(ptr);
				wl_display_roundtrip(display);
			}

			zwlr_virtual_pointer_v1_button(ptr, ++t, button,
											WL_POINTER_BUTTON_STATE_RELEASED);
			zwlr_virtual_pointer_v1_frame(ptr);
			wl_display_roundtrip(display);
		}
	}

	zwlr_virtual_pointer_v1_destroy(ptr);
	wl_display_disconnect(display);
	return 0;
}
