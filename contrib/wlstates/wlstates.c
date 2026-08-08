// wlstates — an xdg-shell toplevel that REPORTS the state array the compositor
// configures it with, for headless regression testing.
//
// What it exists to catch: xdg_toplevel.suspended tells a client its content
// isn't visible so it can stop rendering and decoding. Nothing else in contrib/
// can observe it. WAYLAND_DEBUG is not enough -- it prints the configure's
// state array as "array[20]" and never its contents, and the array does not
// even change LENGTH on a tag hide (the window loses `activated` exactly as it
// gains `suspended`), so a byte count reads identical whether the compositor
// implements suspension or not. The first attempt to verify this by log-diffing
// kitty was blind for that reason. Only a client that unpacks the array can say.
//
// Binds xdg_wm_base at version 6 deliberately: `suspended` is
// XDG_TOPLEVEL_STATE_SUSPENDED_SINCE_VERSION == 6, and a compositor is required
// to withhold it from older clients. Binding lower would make this silently
// untestable; see the version guard in client_set_suspended().
//
// Output is one line per configure on stdout, flushed immediately so a test can
// poll the file while this still runs:
//
//   ready                     mapped and ready
//   configure <w>x<h> states=<name,name,...>   ("-" when the array is empty)
//   close                     compositor asked us to close
//
// State names are the xdg_toplevel.state enum spelled out, so a test greps for
// "suspended" rather than for the magic value 9.
//
// Usage: wlstates <app_id> <hold_seconds>
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
static bool configured = false;

static void wm_base_ping(void *data, struct xdg_wm_base *b, uint32_t serial) {
	(void)data;
	xdg_wm_base_pong(b, serial);
}
static const struct xdg_wm_base_listener wm_base_listener = {
	.ping = wm_base_ping,
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
		// 6, not 1: anything lower and the compositor must never send
		// `suspended`, which is the whole point of this client.
		wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 6);
		xdg_wm_base_add_listener(wm_base, &wm_base_listener, NULL);
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

static const char *state_name(uint32_t state) {
	switch (state) {
	case XDG_TOPLEVEL_STATE_MAXIMIZED:    return "maximized";
	case XDG_TOPLEVEL_STATE_FULLSCREEN:   return "fullscreen";
	case XDG_TOPLEVEL_STATE_RESIZING:     return "resizing";
	case XDG_TOPLEVEL_STATE_ACTIVATED:    return "activated";
	case XDG_TOPLEVEL_STATE_TILED_LEFT:   return "tiled_left";
	case XDG_TOPLEVEL_STATE_TILED_RIGHT:  return "tiled_right";
	case XDG_TOPLEVEL_STATE_TILED_TOP:    return "tiled_top";
	case XDG_TOPLEVEL_STATE_TILED_BOTTOM: return "tiled_bottom";
	case XDG_TOPLEVEL_STATE_SUSPENDED:    return "suspended";
	default:                              return "unknown";
	}
}

static void toplevel_configure(void *data, struct xdg_toplevel *t, int32_t w,
							   int32_t h, struct wl_array *states) {
	(void)data; (void)t;
	printf("configure %dx%d states=", w, h);
	uint32_t *state;
	bool first = true;
	wl_array_for_each(state, states) {
		printf("%s%s", first ? "" : ",", state_name(*state));
		first = false;
	}
	// an empty array is the initial configure; print a placeholder so a test
	// can tell "no states" from a truncated line
	printf("%s\n", first ? "-" : "");
	fflush(stdout);
}
static void toplevel_close(void *data, struct xdg_toplevel *t) {
	(void)data; (void)t;
	printf("close\n");
	fflush(stdout);
	exit(0);
}

// configure_bounds (v4) and wm_capabilities (v5) are not optional to HANDLE:
// libwayland aborts the client outright on a NULL listener slot for an event
// the compositor sends, and binding wm_base at 6 opts into both. wlkeys gets
// away with a two-field listener only because it binds at version 1.
// Reported rather than ignored -- they are cheap to print and are themselves
// part of what a compositor is expected to tell a toplevel.
static void toplevel_configure_bounds(void *data, struct xdg_toplevel *t,
									  int32_t w, int32_t h) {
	(void)data; (void)t;
	printf("bounds %dx%d\n", w, h);
	fflush(stdout);
}

static void toplevel_wm_capabilities(void *data, struct xdg_toplevel *t,
									 struct wl_array *caps) {
	(void)data; (void)t;
	printf("wm_capabilities n=%zu\n", caps->size / sizeof(uint32_t));
	fflush(stdout);
}

static const struct xdg_toplevel_listener toplevel_listener = {
	.configure = toplevel_configure,
	.close = toplevel_close,
	.configure_bounds = toplevel_configure_bounds,
	.wm_capabilities = toplevel_wm_capabilities,
};

static struct wl_buffer *make_buffer(int w, int h, uint32_t argb) {
	int stride = w * 4;
	int size = stride * h;
	char path[] = "/tmp/wlstates-shm-XXXXXX";
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
		fprintf(stderr, "wlstates: cannot connect to WAYLAND_DISPLAY\n");
		return 1;
	}
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);

	if (!compositor || !shm || !wm_base) {
		fprintf(stderr, "wlstates: missing compositor/shm/xdg_wm_base(>=6)\n");
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

	// Same prepare/read handshake as wlkeys: honour the deadline even when no
	// event ever arrives, so a client that outlives its timeout can't hang the
	// suite.
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

		struct pollfd pfd = {
			.fd = wl_display_get_fd(display),
			.events = POLLIN,
		};
		int timeout_ms = (int)((hold - elapsed) * 1000);
		if (poll(&pfd, 1, timeout_ms > 0 ? timeout_ms : 0) > 0) {
			wl_display_read_events(display);
			wl_display_dispatch_pending(display);
		} else {
			wl_display_cancel_read(display);
		}
	}

	wl_display_disconnect(display);
	return 0;
}
