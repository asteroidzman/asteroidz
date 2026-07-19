// wllayer — tiny wlr-layer-shell-unstable-v1 client for headless testing.
//
// Maps a configurable layer-shell surface (layer, anchor, exclusive zone,
// keyboard-interactivity, size) so contrib/regression tests can exercise
// layer-shell edge cases (exclusive-zone reservation shrinking tiled window
// geometry, stacking across layers, resize-in-place) that no existing test
// client (kitty, wlvptr, wlvkbd) can reach -- none of those are layer-shell
// surfaces. Optionally resizes itself in place after a delay, to pin the
// need_output_flush shadow-redraw regression (see asteroidz.c
// commitlayersurfacenotify) directly instead of only via the waybar popup.
//
// Usage: wllayer <layer> <anchor> <exclusive_zone> <width> <height>
//                <kb_interactivity> <hold_seconds> [<resize_after>,<new_w>,<new_h>]
//   layer            background|bottom|top|overlay
//   anchor           comma-separated subset of top,bottom,left,right, or none
//   exclusive_zone   integer (0 = none, -1 = ignore anchored exclusive zones)
//   width, height    surface size (0 lets anchor opposite edges size it --
//                    not used here, always pass explicit non-zero values)
//   kb_interactivity none|exclusive|ondemand
//   hold_seconds     stay mapped this long before exiting
//   resize_after     optional "delay,new_w,new_h" -- after `delay` seconds
//                    (from map), set_size to new_w/new_h and recommit with a
//                    freshly-painted buffer (same buffer content, just a
//                    resize -- the exact "content-fit popup resizes in
//                    place" scenario the shadow bug needed)
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

static struct wl_compositor *compositor = NULL;
static struct wl_shm *shm = NULL;
static struct zwlr_layer_shell_v1 *layer_shell = NULL;
static bool configured = false;
static uint32_t last_serial = 0;

static void registry_global(void *data, struct wl_registry *registry,
							 uint32_t name, const char *interface,
							 uint32_t version) {
	(void)data;
	(void)version;
	if (!strcmp(interface, wl_compositor_interface.name)) {
		compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
	} else if (!strcmp(interface, wl_shm_interface.name)) {
		shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (!strcmp(interface, zwlr_layer_shell_v1_interface.name)) {
		layer_shell = wl_registry_bind(registry, name,
									   &zwlr_layer_shell_v1_interface, 4);
	}
}
static void registry_global_remove(void *data, struct wl_registry *registry,
									uint32_t name) {
	(void)data; (void)registry; (void)name;
}
static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

static void layer_surface_configure(void *data,
		struct zwlr_layer_surface_v1 *surface, uint32_t serial,
		uint32_t w, uint32_t h) {
	(void)data;
	last_serial = serial;
	configured = true;
	printf("wllayer: configure serial=%u size=%ux%u\n", serial, w, h);
	fflush(stdout);
	zwlr_layer_surface_v1_ack_configure(surface, serial);
}
static void layer_surface_closed(void *data,
		struct zwlr_layer_surface_v1 *surface) {
	(void)data; (void)surface;
	fprintf(stderr, "wllayer: layer surface closed by compositor\n");
	exit(1);
}
static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
	.configure = layer_surface_configure,
	.closed = layer_surface_closed,
};

// Minimal wl_shm ARGB8888 buffer, solid opaque colour -- content doesn't
// matter for what these tests check (geometry/exclusive-zone/stacking).
static struct wl_buffer *make_buffer(int w, int h, uint32_t argb) {
	int stride = w * 4;
	int size = stride * h;
	char path[] = "/tmp/wllayer-shm-XXXXXX";
	int fd = mkstemp(path);
	if (fd < 0) { perror("mkstemp"); exit(1); }
	unlink(path);
	if (ftruncate(fd, size) < 0) { perror("ftruncate"); exit(1); }
	uint32_t *pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (pixels == MAP_FAILED) { perror("mmap"); exit(1); }
	for (int i = 0; i < w * h; i++) pixels[i] = argb;
	munmap(pixels, size);

	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
	struct wl_buffer *buf = wl_shm_pool_create_buffer(
		pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	return buf;
}

static uint32_t parse_layer(const char *s) {
	if (!strcmp(s, "background")) return ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND;
	if (!strcmp(s, "bottom")) return ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM;
	if (!strcmp(s, "top")) return ZWLR_LAYER_SHELL_V1_LAYER_TOP;
	if (!strcmp(s, "overlay")) return ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY;
	fprintf(stderr, "wllayer: bad layer '%s'\n", s);
	exit(1);
}
static uint32_t parse_anchor(const char *s) {
	if (!strcmp(s, "none") || !*s) return 0;
	uint32_t a = 0;
	char buf[64]; strncpy(buf, s, sizeof buf - 1); buf[sizeof buf - 1] = '\0';
	for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
		if (!strcmp(tok, "top")) a |= ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP;
		else if (!strcmp(tok, "bottom")) a |= ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
		else if (!strcmp(tok, "left")) a |= ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT;
		else if (!strcmp(tok, "right")) a |= ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
		else { fprintf(stderr, "wllayer: bad anchor token '%s'\n", tok); exit(1); }
	}
	return a;
}
static uint32_t parse_kb(const char *s) {
	if (!strcmp(s, "none")) return ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE;
	if (!strcmp(s, "exclusive")) return ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE;
	if (!strcmp(s, "ondemand")) return ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND;
	fprintf(stderr, "wllayer: bad kb_interactivity '%s'\n", s);
	exit(1);
}

int main(int argc, char **argv) {
	if (argc < 8) {
		fprintf(stderr, "usage: %s <layer> <anchor> <exclusive_zone> <width> "
						"<height> <kb_interactivity> <hold_seconds> "
						"[<resize_after>,<new_w>,<new_h>]\n", argv[0]);
		return 1;
	}
	uint32_t layer = parse_layer(argv[1]);
	uint32_t anchor = parse_anchor(argv[2]);
	int32_t exclusive_zone = atoi(argv[3]);
	int32_t width = atoi(argv[4]);
	int32_t height = atoi(argv[5]);
	uint32_t kb = parse_kb(argv[6]);
	int hold_seconds = atoi(argv[7]);
	int resize_after = -1, new_w = 0, new_h = 0;
	if (argc > 8) {
		if (sscanf(argv[8], "%d,%d,%d", &resize_after, &new_w, &new_h) != 3) {
			fprintf(stderr, "wllayer: bad resize spec '%s'\n", argv[8]);
			return 1;
		}
	}

	struct wl_display *display = wl_display_connect(NULL);
	if (!display) {
		fprintf(stderr, "wllayer: could not connect to a Wayland display "
						"(check WAYLAND_DISPLAY/XDG_RUNTIME_DIR)\n");
		return 1;
	}
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);
	if (!compositor || !shm || !layer_shell) {
		fprintf(stderr, "wllayer: compositor missing wl_compositor/wl_shm/"
						"zwlr_layer_shell_v1\n");
		return 1;
	}

	struct wl_surface *surface = wl_compositor_create_surface(compositor);
	struct zwlr_layer_surface_v1 *layer_surface =
		zwlr_layer_shell_v1_get_layer_surface(layer_shell, surface, NULL,
											   layer, "wllayer");
	zwlr_layer_surface_v1_add_listener(layer_surface, &layer_surface_listener, NULL);
	zwlr_layer_surface_v1_set_size(layer_surface, (uint32_t)width, (uint32_t)height);
	zwlr_layer_surface_v1_set_anchor(layer_surface, anchor);
	zwlr_layer_surface_v1_set_exclusive_zone(layer_surface, exclusive_zone);
	zwlr_layer_surface_v1_set_keyboard_interactivity(layer_surface, kb);
	wl_surface_commit(surface);

	while (!configured) wl_display_dispatch(display);

	struct wl_buffer *buf = make_buffer(width, height, 0xff3050c0);
	wl_surface_attach(surface, buf, 0, 0);
	wl_surface_damage_buffer(surface, 0, 0, width, height);
	wl_surface_commit(surface);
	wl_display_roundtrip(display);
	printf("wllayer: mapped (layer=%s %dx%d)\n", argv[1], width, height);
	fflush(stdout);

	int elapsed = 0;
	bool resized = false;
	while (elapsed < hold_seconds) {
		sleep(1);
		elapsed++;
		wl_display_dispatch_pending(display);
		wl_display_flush(display);
		if (!resized && resize_after >= 0 && elapsed >= resize_after) {
			resized = true;
			configured = false;
			zwlr_layer_surface_v1_set_size(layer_surface, (uint32_t)new_w, (uint32_t)new_h);
			wl_surface_commit(surface);
			while (!configured) wl_display_dispatch(display);
			struct wl_buffer *buf2 = make_buffer(new_w, new_h, 0xffc05030);
			wl_surface_attach(surface, buf2, 0, 0);
			wl_surface_damage_buffer(surface, 0, 0, new_w, new_h);
			wl_surface_commit(surface);
			wl_display_roundtrip(display);
			printf("wllayer: resized in place to %dx%d\n", new_w, new_h);
			fflush(stdout);
		}
	}

	zwlr_layer_surface_v1_destroy(layer_surface);
	wl_surface_destroy(surface);
	wl_display_disconnect(display);
	return 0;
}
