// wlkeys — an xdg-shell toplevel that REPORTS what the compositor tells it
// about the keyboard, for headless regression testing.
//
// Every other client in contrib/ drives input INTO the compositor (wlvkbd,
// wlvptr). None observes what comes back out, which left a whole class of
// keyboard bugs unassertable: the first fix for "a bound key's release reaches
// the focused client" shipped with a test that openly admitted it could not
// observe the thing it named. This is that missing observer.
//
// What it exists to catch, specifically: wl_keyboard.enter carries the array of
// keys held AT THE MOMENT OF FOCUS, and a client told a key is held will repeat
// it until it sees a release. A tag-switch binding fires on press, moves focus,
// and the incoming client is handed the still-held bound key -- whose release
// the compositor then swallows, because the client never saw its press. Nothing
// stops the repeat. Under XWayland the X server keeps that state itself, so a
// Proton game receives the key forever. Only the client can say whether it was
// told, so only a client can test it.
//
// Output is one line per event on stdout, flushed immediately so a test can
// poll the file while this still runs:
//
//   ready                     mapped and ready for input
//   enter keys=<a,b,c>        focus gained; evdev keycodes held, empty if none
//   leave                     focus lost
//   key <code> pressed        a key event actually delivered to this client
//   key <code> released
//   mods d=<n> l=<n> lk=<n> g=<n>
//
// Keycodes are printed RAW (evdev), exactly as they cross the protocol -- no
// +8 xkb translation -- so a test compares against the same numbers wlvkbd
// sends.
//
// Usage: wlkeys <app_id> <hold_seconds>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"

static struct wl_compositor *compositor = NULL;
static struct wl_shm *shm = NULL;
static struct xdg_wm_base *wm_base = NULL;
static struct wl_seat *seat = NULL;
static struct wl_keyboard *keyboard = NULL;
static bool configured = false;

static void wm_base_ping(void *data, struct xdg_wm_base *b, uint32_t serial) {
	(void)data;
	xdg_wm_base_pong(b, serial);
}
static const struct xdg_wm_base_listener wm_base_listener = {
	.ping = wm_base_ping,
};

// ─── keyboard ───────────────────────────────────────────────────────────────

static void kb_keymap(void *data, struct wl_keyboard *kb, uint32_t format,
					  int32_t fd, uint32_t size) {
	(void)data; (void)kb; (void)format; (void)size;
	// not interpreted: this reports raw protocol traffic, not symbols. The fd
	// still must be closed or the compositor leaks one per client.
	close(fd);
}

static void kb_enter(void *data, struct wl_keyboard *kb, uint32_t serial,
					 struct wl_surface *surface, struct wl_array *keys) {
	(void)data; (void)kb; (void)serial; (void)surface;
	printf("enter keys=");
	uint32_t *code;
	bool first = true;
	wl_array_for_each(code, keys) {
		printf("%s%u", first ? "" : ",", *code);
		first = false;
	}
	printf("\n");
	fflush(stdout);
}

static void kb_leave(void *data, struct wl_keyboard *kb, uint32_t serial,
					 struct wl_surface *surface) {
	(void)data; (void)kb; (void)serial; (void)surface;
	printf("leave\n");
	fflush(stdout);
}

static void kb_key(void *data, struct wl_keyboard *kb, uint32_t serial,
				   uint32_t time, uint32_t key, uint32_t state) {
	(void)data; (void)kb; (void)serial; (void)time;
	printf("key %u %s\n", key,
		   state == WL_KEYBOARD_KEY_STATE_PRESSED ? "pressed" : "released");
	fflush(stdout);
}

static void kb_modifiers(void *data, struct wl_keyboard *kb, uint32_t serial,
						 uint32_t depressed, uint32_t latched, uint32_t locked,
						 uint32_t group) {
	(void)data; (void)kb; (void)serial;
	printf("mods d=%u l=%u lk=%u g=%u\n", depressed, latched, locked, group);
	fflush(stdout);
}

static void kb_repeat_info(void *data, struct wl_keyboard *kb, int32_t rate,
						   int32_t delay) {
	(void)data; (void)kb; (void)rate; (void)delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
	.keymap = kb_keymap,
	.enter = kb_enter,
	.leave = kb_leave,
	.key = kb_key,
	.modifiers = kb_modifiers,
	.repeat_info = kb_repeat_info,
};

static void seat_capabilities(void *data, struct wl_seat *s, uint32_t caps) {
	(void)data;
	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !keyboard) {
		keyboard = wl_seat_get_keyboard(s);
		wl_keyboard_add_listener(keyboard, &keyboard_listener, NULL);
	}
}
static void seat_name(void *data, struct wl_seat *s, const char *name) {
	(void)data; (void)s; (void)name;
}
static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_capabilities,
	.name = seat_name,
};

// ─── registry / surface ─────────────────────────────────────────────────────

static void registry_global(void *data, struct wl_registry *registry,
							uint32_t name, const char *interface,
							uint32_t version) {
	(void)data; (void)version;
	if (!strcmp(interface, wl_compositor_interface.name)) {
		compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
	} else if (!strcmp(interface, wl_shm_interface.name)) {
		shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (!strcmp(interface, xdg_wm_base_interface.name)) {
		wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
		xdg_wm_base_add_listener(wm_base, &wm_base_listener, NULL);
	} else if (!strcmp(interface, wl_seat_interface.name)) {
		seat = wl_registry_bind(registry, name, &wl_seat_interface, 5);
		wl_seat_add_listener(seat, &seat_listener, NULL);
	}
}
static void registry_global_remove(void *data, struct wl_registry *r,
								   uint32_t name) {
	(void)data; (void)r; (void)name;
}
static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

static void xdg_surface_configure(void *data, struct xdg_surface *s,
								  uint32_t serial) {
	(void)data;
	xdg_surface_ack_configure(s, serial);
	configured = true;
}
static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = xdg_surface_configure,
};

static void toplevel_configure(void *data, struct xdg_toplevel *t, int32_t w,
							   int32_t h, struct wl_array *states) {
	(void)data; (void)t; (void)w; (void)h; (void)states;
}
static void toplevel_close(void *data, struct xdg_toplevel *t) {
	(void)data; (void)t;
	exit(0);
}
static const struct xdg_toplevel_listener toplevel_listener = {
	.configure = toplevel_configure,
	.close = toplevel_close,
};

static struct wl_buffer *make_buffer(int w, int h, uint32_t argb) {
	int stride = w * 4;
	int size = stride * h;
	char path[] = "/tmp/wlkeys-shm-XXXXXX";
	int fd = mkstemp(path);
	if (fd < 0) { perror("mkstemp"); exit(1); }
	unlink(path);
	if (ftruncate(fd, size) < 0) { perror("ftruncate"); exit(1); }
	uint32_t *pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (pixels == MAP_FAILED) { perror("mmap"); exit(1); }
	for (int i = 0; i < w * h; i++) pixels[i] = argb;
	munmap(pixels, size);

	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
	struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, w, h, stride,
													 WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	return buf;
}

int main(int argc, char **argv) {
	if (argc < 3) {
		fprintf(stderr, "usage: %s <app_id> <hold_seconds>\n", argv[0]);
		return 1;
	}
	const char *app_id = argv[1];
	double hold = atof(argv[2]);

	struct wl_display *display = wl_display_connect(NULL);
	if (!display) {
		fprintf(stderr, "wlkeys: cannot connect to WAYLAND_DISPLAY\n");
		return 1;
	}
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);
	wl_display_roundtrip(display); // seat capabilities arrive on the second

	if (!compositor || !shm || !wm_base) {
		fprintf(stderr, "wlkeys: missing compositor/shm/xdg_wm_base\n");
		return 1;
	}

	struct wl_surface *surface = wl_compositor_create_surface(compositor);
	struct xdg_surface *xdg_surface =
		xdg_wm_base_get_xdg_surface(wm_base, surface);
	xdg_surface_add_listener(xdg_surface, &xdg_surface_listener, NULL);
	struct xdg_toplevel *toplevel = xdg_surface_get_toplevel(xdg_surface);
	xdg_toplevel_add_listener(toplevel, &toplevel_listener, NULL);
	xdg_toplevel_set_app_id(toplevel, app_id);
	xdg_toplevel_set_title(toplevel, app_id);
	wl_surface_commit(surface);

	while (!configured && wl_display_dispatch(display) != -1)
		;

	struct wl_buffer *buf = make_buffer(200, 120, 0xff203040);
	wl_surface_attach(surface, buf, 0, 0);
	wl_surface_damage(surface, 0, 0, 200, 120);
	wl_surface_commit(surface);
	wl_display_roundtrip(display);

	printf("ready\n");
	fflush(stdout);

	// Dispatch until the hold expires. Uses the prepare/read handshake rather
	// than a blocking dispatch so the deadline is honoured even when no event
	// ever arrives -- a test whose client outlives its own timeout hangs the
	// whole suite.
	struct timespec start;
	clock_gettime(CLOCK_MONOTONIC, &start);
	for (;;) {
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		double elapsed = (double)(now.tv_sec - start.tv_sec) +
						 (double)(now.tv_nsec - start.tv_nsec) / 1e9;
		if (elapsed >= hold)
			break;

		while (wl_display_prepare_read(display) != 0)
			wl_display_dispatch_pending(display);
		wl_display_flush(display);

		struct pollfd pfd = {.fd = wl_display_get_fd(display),
							 .events = POLLIN};
		int n = poll(&pfd, 1, 100);
		if (n > 0)
			wl_display_read_events(display);
		else
			wl_display_cancel_read(display);
		wl_display_dispatch_pending(display);
	}

	wl_display_disconnect(display);
	return 0;
}
