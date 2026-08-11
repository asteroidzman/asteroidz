/*
 * wlcursor — a client that sets its OWN cursor image, via wl_pointer.set_cursor.
 *
 * This is the third cursor source from §5.4g of
 * docs/vulkan-native-architecture.md, and the only one that broke when AVK
 * stopped giving wl_compositor a renderer. xcursor and cursor-shape both end
 * at wlr_xcursor_image_get_buffer() and hand wlroots a wlr_buffer; a client
 * cursor went through wlr_surface_get_texture(), which reads the
 * wlr_client_buffer wrapper that only exists when the compositor has a
 * renderer. With no renderer the texture was NULL, the cursor was disabled
 * outright, and NOTHING LOGGED IT.
 *
 * Nothing else in the tree sets a cursor surface. Every test client in
 * contrib/ leaves the pointer to the compositor's own xcursor theme, so the
 * entire suite ran green while client cursors were invisible. That is what
 * this exists to make impossible.
 *
 * Usage:
 *     wlcursor [--size WxH] [--colour RRGGBB]
 *              [--cursor-size WxH] [--cursor-colour RRGGBB]
 *              [--hotspot X,Y] [--cursor-scale N]
 *              [--animate-ms N] [--animate-once] [--reuse-buffer]
 *              [--no-cursor]
 *
 * The window is a flat colour so a screenshot can tell window from cursor by
 * pixel count alone. The cursor is a different flat colour with a one-pixel
 * marker at its hotspot, so a test can assert not just "a cursor is on screen"
 * but "its hotspot is where the pointer is" — the two are different claims and
 * a hotspot bug passes the first one.
 *
 *   --animate-ms N    commit a new cursor colour every N ms. The cursor is a
 *                     surface like any other and an animated one has to keep
 *                     working.
 *   --animate-once    make exactly one transition and then stop, so a capture
 *                     taken at any later moment sees the same image. A cycling
 *                     animation makes an assertion depend on catching the
 *                     right instant, which is a race, not a test.
 *   --reuse-buffer    animate by rewriting ONE wl_buffer instead of rotating
 *                     through two. This is wlreuse's trick applied to the
 *                     cursor: a compositor that decides "same wl_buffer, so
 *                     same pixels" freezes on the first colour, and both
 *                     wlroots' own wlr_cursor_set_buffer() and any renderer
 *                     cache keyed on the pointer do exactly that unless
 *                     something forces the re-import.
 *   --no-cursor       never call set_cursor. The compositor's own xcursor
 *                     stays visible, which is the control case: it proves a
 *                     test that sees a cursor is seeing THIS client's cursor
 *                     and not the theme's.
 *
 * It runs until killed. A test moves a virtual pointer (contrib/wlvptr) over
 * the window to make the compositor send wl_pointer.enter, which is the only
 * moment a client is allowed to set a cursor.
 */
#define _GNU_SOURCE   /* memfd_create */

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include "xdg-shell-client-protocol.h"

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct xdg_wm_base *wm_base;
static struct wl_pointer *pointer;

static struct wl_surface *surface;
static struct xdg_surface *xdg_surface;
static struct xdg_toplevel *toplevel;

/* The cursor is a surface of its own, with its own buffers. */
static struct wl_surface *cursor_surface;
static struct wl_buffer *cursor_buffers[2];
static uint32_t *cursor_pixels[2];
static int cursor_slot;

static int width = 600, height = 400;
static uint32_t window_colour = 0xff202020u;
static int cursor_w = 32, cursor_h = 32;
static uint32_t cursor_colours[8];
static int cursor_colour_count;
static int cursor_colour_index;
static int hotspot_x = 4, hotspot_y = 4;
static int cursor_scale = 1;
static int animate_ms;
static bool reuse_buffer;
static bool animate_once;
static bool no_cursor;

static bool running = true;
static uint32_t enter_serial;
static bool have_enter;
static bool animation_started;
static uint64_t animation_epoch;

static uint64_t now_ms(void);

static void wm_base_ping(void *data, struct xdg_wm_base *base, uint32_t serial) {
	(void)data;
	xdg_wm_base_pong(base, serial);
}
static const struct xdg_wm_base_listener wm_base_listener = {
	.ping = wm_base_ping,
};

/* An anonymous file to share with the compositor. */
static int anon_file(size_t size) {
	int fd = memfd_create("wlcursor", MFD_CLOEXEC | MFD_ALLOW_SEALING);
	if (fd < 0) {
		return -1;
	}
	if (ftruncate(fd, (off_t)size) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

/* One shm buffer, filled with a flat colour. */
static struct wl_buffer *make_buffer(int w, int h, uint32_t colour,
		uint32_t **out_pixels) {
	size_t stride = (size_t)w * 4;
	size_t size = stride * (size_t)h;
	int fd = anon_file(size);
	if (fd < 0) {
		return NULL;
	}
	uint32_t *px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (px == MAP_FAILED) {
		close(fd);
		return NULL;
	}
	for (size_t i = 0; i < size / 4; i++) {
		px[i] = colour;
	}
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)size);
	struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, w, h,
		(int32_t)stride, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	if (out_pixels != NULL) {
		*out_pixels = px;
	}
	return buf;
}

/*
 * Paint one cursor image: a flat colour, plus a single opaque-black pixel at
 * the hotspot.
 *
 * The marker is what makes hotspot testable. "The cursor is on screen" and
 * "the cursor is positioned correctly" are different assertions, and a
 * compositor that ignores the hotspot entirely passes the first one perfectly
 * — the image is all there, just offset by the hotspot. A test that looks for
 * the marker pixel at the pointer's coordinates cannot be fooled that way.
 */
static void paint_cursor(uint32_t *px, uint32_t colour) {
	for (int i = 0; i < cursor_w * cursor_h; i++) {
		px[i] = colour;
	}
	int hx = hotspot_x * cursor_scale;
	int hy = hotspot_y * cursor_scale;
	if (hx >= 0 && hx < cursor_w && hy >= 0 && hy < cursor_h) {
		px[hy * cursor_w + hx] = 0xff000000u;
	}
}

static void commit_cursor(void) {
	int slot = reuse_buffer ? 0 : cursor_slot;
	uint32_t colour = cursor_colours[cursor_colour_index];

	paint_cursor(cursor_pixels[slot], colour);
	wl_surface_attach(cursor_surface, cursor_buffers[slot], 0, 0);
	wl_surface_set_buffer_scale(cursor_surface, cursor_scale);
	wl_surface_damage_buffer(cursor_surface, 0, 0, cursor_w, cursor_h);
	wl_surface_commit(cursor_surface);

	if (!reuse_buffer) {
		cursor_slot = (cursor_slot + 1) % 2;
	}
}

/*
 * Claim the pointer.
 *
 * set_cursor takes the serial of the enter event, so this can only be done
 * from inside (or after) an enter, and only while the pointer is still ours.
 * The hotspot is in SURFACE-local coordinates, which is why it is divided by
 * nothing here and multiplied by the scale when painting: a scale-2 cursor
 * surface has a 64x64 buffer and a hotspot still expressed in the 32x32
 * surface's units.
 */
static void set_cursor(void) {
	if (no_cursor || !have_enter) {
		return;
	}
	commit_cursor();
	wl_pointer_set_cursor(pointer, enter_serial, cursor_surface,
		hotspot_x, hotspot_y);
}

static void pointer_enter(void *data, struct wl_pointer *p, uint32_t serial,
		struct wl_surface *surf, wl_fixed_t sx, wl_fixed_t sy) {
	(void)data; (void)p; (void)surf; (void)sx; (void)sy;
	enter_serial = serial;
	have_enter = true;
	/* The animation clock starts here, not at startup. A cursor nobody can
	 * see has no business animating, and a test that captures it needs the
	 * sequence to be deterministic relative to the moment it became visible
	 * rather than to process start. */
	if (!animation_started) {
		animation_started = true;
		animation_epoch = now_ms();
	}
	set_cursor();
}
static void pointer_leave(void *data, struct wl_pointer *p, uint32_t serial,
		struct wl_surface *surf) {
	(void)data; (void)p; (void)serial; (void)surf;
	have_enter = false;
}
static void pointer_motion(void *data, struct wl_pointer *p, uint32_t time,
		wl_fixed_t sx, wl_fixed_t sy) {
	(void)data; (void)p; (void)time; (void)sx; (void)sy;
}
static void pointer_button(void *data, struct wl_pointer *p, uint32_t serial,
		uint32_t time, uint32_t button, uint32_t state) {
	(void)data; (void)p; (void)serial; (void)time; (void)button; (void)state;
}
static void pointer_axis(void *data, struct wl_pointer *p, uint32_t time,
		uint32_t axis, wl_fixed_t value) {
	(void)data; (void)p; (void)time; (void)axis; (void)value;
}
/* wl_pointer 5 adds four more events, and libwayland aborts the client on the
 * first one whose listener slot is NULL rather than ignoring it. */
static void pointer_frame(void *data, struct wl_pointer *p) {
	(void)data; (void)p;
}
static void pointer_axis_source(void *data, struct wl_pointer *p,
		uint32_t source) {
	(void)data; (void)p; (void)source;
}
static void pointer_axis_stop(void *data, struct wl_pointer *p, uint32_t time,
		uint32_t axis) {
	(void)data; (void)p; (void)time; (void)axis;
}
static void pointer_axis_discrete(void *data, struct wl_pointer *p,
		uint32_t axis, int32_t discrete) {
	(void)data; (void)p; (void)axis; (void)discrete;
}
static const struct wl_pointer_listener pointer_listener = {
	.enter = pointer_enter,
	.leave = pointer_leave,
	.motion = pointer_motion,
	.button = pointer_button,
	.axis = pointer_axis,
	.frame = pointer_frame,
	.axis_source = pointer_axis_source,
	.axis_stop = pointer_axis_stop,
	.axis_discrete = pointer_axis_discrete,
};

static void seat_capabilities(void *data, struct wl_seat *s, uint32_t caps) {
	(void)data;
	if ((caps & WL_SEAT_CAPABILITY_POINTER) && pointer == NULL) {
		pointer = wl_seat_get_pointer(s);
		wl_pointer_add_listener(pointer, &pointer_listener, NULL);
	}
}
static void seat_name(void *data, struct wl_seat *s, const char *name) {
	(void)data; (void)s; (void)name;
}
static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_capabilities,
	.name = seat_name,
};

static void registry_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version) {
	(void)data; (void)version;
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		compositor = wl_registry_bind(registry, name,
			&wl_compositor_interface, 4);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, wl_seat_interface.name) == 0) {
		seat = wl_registry_bind(registry, name, &wl_seat_interface, 5);
		wl_seat_add_listener(seat, &seat_listener, NULL);
	} else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
		wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
		xdg_wm_base_add_listener(wm_base, &wm_base_listener, NULL);
	}
}
static void registry_global_remove(void *data, struct wl_registry *r,
		uint32_t n) {
	(void)data; (void)r; (void)n;
}
static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

static void xdg_surface_configure(void *data, struct xdg_surface *xs,
		uint32_t serial) {
	(void)data;
	xdg_surface_ack_configure(xs, serial);
}
static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = xdg_surface_configure,
};

static void toplevel_configure(void *data, struct xdg_toplevel *tl,
		int32_t w, int32_t h, struct wl_array *states) {
	(void)data; (void)tl; (void)states; (void)w; (void)h;
}
static void toplevel_close(void *data, struct xdg_toplevel *tl) {
	(void)data; (void)tl;
	running = false;
}
static const struct xdg_toplevel_listener toplevel_listener = {
	.configure = toplevel_configure,
	.close = toplevel_close,
};

uint64_t now_ms(void);
static uint64_t now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

int main(int argc, char **argv) {
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
			sscanf(argv[++i], "%dx%d", &width, &height);
		} else if (strcmp(argv[i], "--colour") == 0 && i + 1 < argc) {
			window_colour = 0xff000000u |
				(uint32_t)strtoul(argv[++i], NULL, 16);
		} else if (strcmp(argv[i], "--cursor-size") == 0 && i + 1 < argc) {
			sscanf(argv[++i], "%dx%d", &cursor_w, &cursor_h);
		} else if (strcmp(argv[i], "--cursor-colour") == 0 && i + 1 < argc) {
			if (cursor_colour_count < 8) {
				cursor_colours[cursor_colour_count++] = 0xff000000u |
					(uint32_t)strtoul(argv[++i], NULL, 16);
			}
		} else if (strcmp(argv[i], "--hotspot") == 0 && i + 1 < argc) {
			sscanf(argv[++i], "%d,%d", &hotspot_x, &hotspot_y);
		} else if (strcmp(argv[i], "--cursor-scale") == 0 && i + 1 < argc) {
			cursor_scale = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--animate-ms") == 0 && i + 1 < argc) {
			animate_ms = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--reuse-buffer") == 0) {
			reuse_buffer = true;
		} else if (strcmp(argv[i], "--animate-once") == 0) {
			/* Advance to the next colour once and then stop, so a capture
			 * taken any time afterwards sees the same thing. A cycling
			 * animation makes the assertion depend on catching the right
			 * moment: at 2000ms spacing a capture three seconds later landed
			 * on the SECOND transition and read back the original colour, and
			 * the test failed for a reason that had nothing to do with the
			 * compositor. */
			animate_once = true;
		} else if (strcmp(argv[i], "--no-cursor") == 0) {
			no_cursor = true;
		} else {
			fprintf(stderr, "wlcursor: unknown option '%s'\n", argv[i]);
			return 1;
		}
	}
	if (cursor_colour_count == 0) {
		cursor_colours[cursor_colour_count++] = 0xff00ff00u;  /* green */
	}

	struct wl_display *display = wl_display_connect(NULL);
	if (display == NULL) {
		fprintf(stderr, "wlcursor: cannot connect to a Wayland display\n");
		return 1;
	}
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);
	wl_display_roundtrip(display);   /* seat capabilities */

	if (compositor == NULL || shm == NULL || wm_base == NULL) {
		fprintf(stderr, "wlcursor: compositor is missing wl_compositor, "
			"wl_shm or xdg_wm_base\n");
		return 1;
	}
	/*
	 * Deliberately NOT an error when the seat has no pointer yet.
	 *
	 * A headless compositor has no input devices at all until something
	 * creates one, and the thing that creates one in this suite is
	 * contrib/wlvptr, which runs after this client is already up. wl_seat
	 * re-sends capabilities when they change, so the seat listener picks the
	 * pointer up whenever it appears; bailing out here would make the test
	 * order-dependent for no reason.
	 */
	if (pointer == NULL && !no_cursor) {
		fprintf(stderr, "wlcursor: no pointer on the seat yet; waiting for "
			"one to appear\n");
	}

	surface = wl_compositor_create_surface(compositor);
	xdg_surface = xdg_wm_base_get_xdg_surface(wm_base, surface);
	xdg_surface_add_listener(xdg_surface, &xdg_surface_listener, NULL);
	toplevel = xdg_surface_get_toplevel(xdg_surface);
	xdg_toplevel_add_listener(toplevel, &toplevel_listener, NULL);
	xdg_toplevel_set_title(toplevel, "wlcursor");
	xdg_toplevel_set_app_id(toplevel, "wlcursor");
	wl_surface_commit(surface);
	wl_display_roundtrip(display);

	uint32_t *window_pixels = NULL;
	struct wl_buffer *window_buffer =
		make_buffer(width, height, window_colour, &window_pixels);
	if (window_buffer == NULL) {
		fprintf(stderr, "wlcursor: cannot allocate the window buffer\n");
		return 1;
	}
	wl_surface_attach(surface, window_buffer, 0, 0);
	wl_surface_damage_buffer(surface, 0, 0, width, height);
	wl_surface_commit(surface);

	if (!no_cursor) {
		cursor_surface = wl_compositor_create_surface(compositor);
		/* Two buffers even when only one is used, so --reuse-buffer differs
		 * from the default in exactly one way: which of them is committed. */
		for (int i = 0; i < 2; i++) {
			cursor_buffers[i] = make_buffer(cursor_w, cursor_h,
				cursor_colours[0], &cursor_pixels[i]);
			if (cursor_buffers[i] == NULL) {
				fprintf(stderr, "wlcursor: cannot allocate a cursor buffer\n");
				return 1;
			}
		}
	}

	fprintf(stderr, "wlcursor: window %dx%d, cursor %dx%d hotspot %d,%d "
		"scale %d, %d colour(s)%s%s\n", width, height, cursor_w, cursor_h,
		hotspot_x, hotspot_y, cursor_scale, cursor_colour_count,
		reuse_buffer ? ", reusing one buffer" : "",
		no_cursor ? ", NOT setting a cursor" : "");

	uint64_t next_animation = 0;
	while (running && wl_display_dispatch_pending(display) != -1) {
		if (animation_started && next_animation == 0) {
			next_animation = animation_epoch + (uint64_t)animate_ms;
		}
		if (wl_display_flush(display) < 0 && errno != EAGAIN) {
			break;
		}
		if (animate_ms > 0 && cursor_colour_count > 1 && animation_started &&
				now_ms() >= next_animation) {
			cursor_colour_index =
				(cursor_colour_index + 1) % cursor_colour_count;
			/*
			 * Committed whether or not the pointer is still inside.
			 *
			 * A real client animating a cursor does it on a timer, not out of
			 * an enter handler, and the surface stays the pointer's cursor
			 * until something replaces it. Gating this on have_enter also made
			 * the test depend on how long contrib/wlvptr's virtual pointer
			 * happens to live -- it moves the pointer and exits, so the leave
			 * arrives seconds before the first animation tick and the cursor
			 * appeared frozen for a reason that had nothing to do with the
			 * compositor.
			 */
			commit_cursor();
			fprintf(stderr, "wlcursor: animation tick -> colour %d\n",
				cursor_colour_index);
			next_animation = now_ms() + animate_ms;
			if (animate_once) {
				animate_ms = 0;
			}
		}
		if (wl_display_prepare_read(display) == 0) {
			wl_display_flush(display);
			struct timespec sleep_for = { .tv_nsec = 10 * 1000000 };
			nanosleep(&sleep_for, NULL);
			wl_display_read_events(display);
		}
		wl_display_dispatch_pending(display);
	}

	(void)window_pixels;
	wl_display_disconnect(display);
	return 0;
}
