// wlbgeffect -- an xdg-shell toplevel that supplies a real
// ext-background-effect-v1 BLUR REGION, for headless regression testing.
//
// WHAT IT EXISTS TO CATCH. asteroidz turns a client's background-effect region
// into a wlr_scene_blur's `clip_region`, in the NODE'S OWN coordinates, and the
// AVK walker converts that to output pixels and intersects it into the blur
// command's clip. Two things about that conversion are easy to get wrong and
// both still render a plausible picture:
//
//   - a blur node's clipped_region/clip_region is INTERSECTED (it is where the
//     blur may appear), while a rect's or a shadow's is SUBTRACTED (it is the
//     window-shaped hole). Same field name, same C type, opposite operation.
//   - the region is node-local and must be translated and scaled exactly once.
//
// Nothing else in contrib/ sets one. `clipped_region_get_default()` is an EMPTY
// box, so a plain window's blur node carries no clip at all -- which was
// measured, via avk.blur_nodes_clipped reading 0 on a fixture that was supposed
// to be testing clips. That counter is why this client exists.
//
// TWO SEPARATED RECTANGLES, deliberately. A single rectangle cannot tell an
// implementation that preserves a region's shape from one that takes its
// bounding box; two with a gap between them can, and the gap is the assertion.
//
// Output on stdout, flushed immediately so a test can poll while this runs:
//
//   ready <w>x<h>            mapped, buffer attached, region committed
//   noeffect                 the compositor does not advertise the protocol
//   close                    compositor asked us to close
//
// Usage: wlbgeffect <app_id> <hold_seconds>
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
#include "ext-background-effect-v1-client-protocol.h"

static struct wl_compositor *compositor = NULL;
static struct wl_shm *shm = NULL;
static struct xdg_wm_base *wm_base = NULL;
static struct ext_background_effect_manager_v1 *bg_manager = NULL;
static bool configured = false;

/* The two rectangles and the gap between them, in SURFACE-LOCAL coordinates.
 * Printed by the test rather than hardcoded there twice. */
#define SURF_W 400
#define SURF_H 300
#define R1_X 20
#define R1_Y 30
#define R1_W 120
#define R1_H 200
#define GAP_W 60
#define R2_X (R1_X + R1_W + GAP_W)
#define R2_Y R1_Y
#define R2_W 120
#define R2_H 200

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
		wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 6);
		xdg_wm_base_add_listener(wm_base, &wm_base_listener, NULL);
	} else if (!strcmp(interface,
			ext_background_effect_manager_v1_interface.name)) {
		bg_manager = wl_registry_bind(registry, name,
			&ext_background_effect_manager_v1_interface, 1);
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

static void toplevel_configure(void *data, struct xdg_toplevel *t, int32_t w,
							   int32_t h, struct wl_array *states) {
	(void)data; (void)t; (void)w; (void)h; (void)states;
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
	char path[] = "/tmp/wlbgeffect-shm-XXXXXX";
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
		fprintf(stderr, "wlbgeffect: cannot connect to WAYLAND_DISPLAY\n");
		return 1;
	}
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);

	if (!compositor || !shm || !wm_base) {
		fprintf(stderr, "wlbgeffect: missing compositor/shm/xdg_wm_base\n");
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

	/*
	 * TRANSLUCENT, and it has to be. A fully opaque surface can never show its
	 * backdrop, so asteroidz refuses to keep a blur node behind one
	 * (client_update_blur) -- and then there would be nothing for the region to
	 * clip. 0x80 alpha, and no wl_surface_set_opaque_region call.
	 */
	struct wl_buffer *buf = make_buffer(SURF_W, SURF_H, 0x80203040);
	wl_surface_attach(surface, buf, 0, 0);
	wl_surface_damage(surface, 0, 0, SURF_W, SURF_H);

	if (bg_manager != NULL) {
		/*
		 * TWO RECTANGLES WITH A GAP. Committed with the buffer, in the same
		 * transaction, so the compositor never sees a region for a surface that
		 * has no size yet.
		 */
		struct ext_background_effect_surface_v1 *effect =
			ext_background_effect_manager_v1_get_background_effect(bg_manager,
				surface);
		struct wl_region *region = wl_compositor_create_region(compositor);
		wl_region_add(region, R1_X, R1_Y, R1_W, R1_H);
		wl_region_add(region, R2_X, R2_Y, R2_W, R2_H);
		ext_background_effect_surface_v1_set_blur_region(effect, region);
		wl_region_destroy(region);
	} else {
		printf("noeffect\n");
		fflush(stdout);
	}
	wl_surface_commit(surface);
	wl_display_roundtrip(display);

	printf("ready %dx%d rects=%d,%d %dx%d and %d,%d %dx%d gap=%d\n",
		SURF_W, SURF_H, R1_X, R1_Y, R1_W, R1_H, R2_X, R2_Y, R2_W, R2_H, GAP_W);
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
