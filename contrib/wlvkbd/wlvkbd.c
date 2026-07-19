// wlvkbd — tiny zwp_virtual_keyboard_unstable_v1 client for headless
// testing. Companion to contrib/wlvptr (pointer): together they let a
// headless test simulate real keybind-level and mouse-modifier-gesture
// interactions (e.g. Super+drag move/resize), not just IPC dispatch calls.
//
// Like wlvptr, this talks directly to whichever compositor WAYLAND_DISPLAY
// points at -- never touches uinput/the live seat.
//
// A virtual keyboard needs an XKB keymap uploaded before it can send
// anything; this compiles a default ("us"/evdev) keymap via libxkbcommon
// and tracks its own xkb_state locally so it can compute the correct
// mods_depressed mask to send via zwp_virtual_keyboard_v1.modifiers()
// (the compositor does NOT derive modifiers from key codes on its own for
// a virtual keyboard -- the client is expected to track and forward them,
// same as any other Wayland keyboard implementation).
//
// Usage:
//   wlvkbd press KEY1 [KEY2 ...]
//     Chord: press all in order, release in reverse. One-shot -- the
//     process exits immediately after, so this is for a plain keypress/
//     shortcut test (e.g. Escape closing a popup), not for holding a
//     modifier across a separate pointer action (the connection closing
//     drops the held state).
//   wlvkbd hold KEY1 [KEY2 ...] -- COMMAND [ARGS...]
//     Press and hold the given keys, run COMMAND (inheriting this
//     process's environment, so e.g. a nested `wlvptr ... drag:x,y` call
//     shares WAYLAND_DISPLAY and lands in the same compositor), wait for
//     it to exit, then release the keys in reverse and disconnect. This
//     is the primitive for testing a Super+drag-style mouse binding.
//
// Key names are linux/input-event-codes.h KEY_* macros with the "KEY_"
// prefix optional (e.g. "LEFTMETA" or "KEY_LEFTMETA" both work).

#define _GNU_SOURCE
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include <linux/input-event-codes.h>
#include "virtual-keyboard-unstable-v1-client-protocol.h"

#define KEYNAME(x)                                                            \
	{ #x, KEY_##x }
static const struct {
	const char *name;
	int code;
} key_table[] = {
	KEYNAME(LEFTMETA), KEYNAME(RIGHTMETA), KEYNAME(LEFTSHIFT),
	KEYNAME(RIGHTSHIFT), KEYNAME(LEFTCTRL), KEYNAME(RIGHTCTRL),
	KEYNAME(LEFTALT), KEYNAME(RIGHTALT), KEYNAME(TAB), KEYNAME(ESC),
	KEYNAME(ENTER), KEYNAME(SPACE), KEYNAME(BACKSPACE), KEYNAME(DELETE),
	KEYNAME(HOME), KEYNAME(END), KEYNAME(PAGEUP), KEYNAME(PAGEDOWN),
	KEYNAME(UP), KEYNAME(DOWN), KEYNAME(LEFT), KEYNAME(RIGHT),
	KEYNAME(A), KEYNAME(B), KEYNAME(C), KEYNAME(D), KEYNAME(E), KEYNAME(F),
	KEYNAME(G), KEYNAME(H), KEYNAME(I), KEYNAME(J), KEYNAME(K), KEYNAME(L),
	KEYNAME(M), KEYNAME(N), KEYNAME(O), KEYNAME(P), KEYNAME(Q), KEYNAME(R),
	KEYNAME(S), KEYNAME(T), KEYNAME(U), KEYNAME(V), KEYNAME(W), KEYNAME(X),
	KEYNAME(Y), KEYNAME(Z), KEYNAME(0), KEYNAME(1), KEYNAME(2), KEYNAME(3),
	KEYNAME(4), KEYNAME(5), KEYNAME(6), KEYNAME(7), KEYNAME(8), KEYNAME(9),
	KEYNAME(F1), KEYNAME(F2), KEYNAME(F3), KEYNAME(F4), KEYNAME(F5),
	KEYNAME(F6), KEYNAME(F7), KEYNAME(F8), KEYNAME(F9), KEYNAME(F10),
	KEYNAME(F11), KEYNAME(F12),
};

static int key_code_for(const char *name) {
	if (!strncmp(name, "KEY_", 4))
		name += 4;
	for (size_t i = 0; i < sizeof(key_table) / sizeof(key_table[0]); i++)
		if (!strcasecmp(key_table[i].name, name))
			return key_table[i].code;
	return -1;
}

static struct zwp_virtual_keyboard_manager_v1 *manager = NULL;
static struct wl_seat *seat = NULL;

static void registry_global(void *data, struct wl_registry *registry,
							 uint32_t name, const char *interface,
							 uint32_t version) {
	(void)data;
	if (!strcmp(interface, zwp_virtual_keyboard_manager_v1_interface.name)) {
		manager = wl_registry_bind(
			registry, name, &zwp_virtual_keyboard_manager_v1_interface, 1);
	} else if (!strcmp(interface, wl_seat_interface.name)) {
		seat = wl_registry_bind(registry, name, &wl_seat_interface,
								version < 7 ? version : 7);
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

// current time in ms, for the protocol's timestamp args (arbitrary base,
// just needs to be monotonically non-decreasing across one connection)
static uint32_t now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static struct xkb_context *xkb_ctx;
static struct xkb_keymap *xkb_keymap;
static struct xkb_state *xkb_state;
static struct zwp_virtual_keyboard_v1 *vk;
static uint32_t last_mods_depressed = 0xffffffff; // force first send

static void send_modifiers_if_changed(void) {
	uint32_t depressed = xkb_state_serialize_mods(xkb_state, XKB_STATE_MODS_DEPRESSED);
	uint32_t latched = xkb_state_serialize_mods(xkb_state, XKB_STATE_MODS_LATCHED);
	uint32_t locked = xkb_state_serialize_mods(xkb_state, XKB_STATE_MODS_LOCKED);
	uint32_t group = xkb_state_serialize_layout(xkb_state, XKB_STATE_LAYOUT_EFFECTIVE);
	if (depressed == last_mods_depressed)
		return;
	last_mods_depressed = depressed;
	zwp_virtual_keyboard_v1_modifiers(vk, depressed, latched, locked, group);
}

static void press_key(struct wl_display *display, int code) {
	xkb_state_update_key(xkb_state, code + 8, XKB_KEY_DOWN);
	send_modifiers_if_changed();
	zwp_virtual_keyboard_v1_key(vk, now_ms(), code, WL_KEYBOARD_KEY_STATE_PRESSED);
	wl_display_flush(display);
}
static void release_key(struct wl_display *display, int code) {
	xkb_state_update_key(xkb_state, code + 8, XKB_KEY_UP);
	send_modifiers_if_changed();
	zwp_virtual_keyboard_v1_key(vk, now_ms(), code, WL_KEYBOARD_KEY_STATE_RELEASED);
	wl_display_flush(display);
}

int main(int argc, char **argv) {
	if (argc < 3 || (strcmp(argv[1], "press") && strcmp(argv[1], "hold"))) {
		fprintf(stderr,
				"usage: %s press KEY1 [KEY2 ...]\n"
				"       %s hold KEY1 [KEY2 ...] -- COMMAND [ARGS...]\n",
				argv[0], argv[0]);
		return 1;
	}
	bool is_hold = !strcmp(argv[1], "hold");

	int nkeys = 0;
	int keys[16];
	int cmd_argv_start = -1;
	for (int i = 2; i < argc; i++) {
		if (is_hold && !strcmp(argv[i], "--")) {
			cmd_argv_start = i + 1;
			break;
		}
		if (nkeys >= 16) {
			fprintf(stderr, "wlvkbd: too many keys (max 16)\n");
			return 1;
		}
		int code = key_code_for(argv[i]);
		if (code < 0) {
			fprintf(stderr, "wlvkbd: unknown key name: %s\n", argv[i]);
			return 1;
		}
		keys[nkeys++] = code;
	}
	if (is_hold && cmd_argv_start < 0) {
		fprintf(stderr, "wlvkbd: hold requires -- COMMAND [ARGS...]\n");
		return 1;
	}
	if (nkeys == 0) {
		fprintf(stderr, "wlvkbd: no keys given\n");
		return 1;
	}

	struct wl_display *display = wl_display_connect(NULL);
	if (!display) {
		fprintf(stderr, "wlvkbd: could not connect to a Wayland display "
						"(check WAYLAND_DISPLAY/XDG_RUNTIME_DIR)\n");
		return 1;
	}
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);

	if (!manager) {
		fprintf(stderr, "wlvkbd: compositor does not advertise "
						"zwp_virtual_keyboard_manager_v1\n");
		return 1;
	}
	if (!seat) {
		fprintf(stderr, "wlvkbd: no wl_seat advertised\n");
		return 1;
	}

	xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_rule_names names = {0};
	xkb_keymap = xkb_keymap_new_from_names(xkb_ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (!xkb_keymap) {
		fprintf(stderr, "wlvkbd: failed to compile a default xkb keymap\n");
		return 1;
	}
	xkb_state = xkb_state_new(xkb_keymap);

	char *keymap_str = xkb_keymap_get_as_string(xkb_keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
	size_t keymap_size = strlen(keymap_str) + 1;
	int fd = memfd_create("wlvkbd-keymap", 0);
	if (fd < 0 || ftruncate(fd, (off_t)keymap_size) < 0) {
		fprintf(stderr, "wlvkbd: failed to create keymap memfd\n");
		return 1;
	}
	void *map = mmap(NULL, keymap_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	memcpy(map, keymap_str, keymap_size);
	munmap(map, keymap_size);
	free(keymap_str);

	vk = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(manager, seat);
	zwp_virtual_keyboard_v1_keymap(vk, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, keymap_size);
	wl_display_roundtrip(display);
	close(fd);

	for (int i = 0; i < nkeys; i++)
		press_key(display, keys[i]);

	if (is_hold) {
		pid_t pid = fork();
		if (pid == 0) {
			execvp(argv[cmd_argv_start], &argv[cmd_argv_start]);
			fprintf(stderr, "wlvkbd: exec failed: %s\n", argv[cmd_argv_start]);
			_exit(127);
		} else if (pid > 0) {
			int status = 0;
			waitpid(pid, &status, 0);
		} else {
			fprintf(stderr, "wlvkbd: fork failed\n");
		}
	} else {
		struct timespec ts = {.tv_sec = 0, .tv_nsec = 30 * 1000000};
		nanosleep(&ts, NULL);
	}

	for (int i = nkeys - 1; i >= 0; i--)
		release_key(display, keys[i]);
	wl_display_roundtrip(display);

	zwp_virtual_keyboard_v1_destroy(vk);
	wl_display_disconnect(display);
	return 0;
}
