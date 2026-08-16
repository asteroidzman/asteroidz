/*
 * wlrepaint -- a wl_shm client that repaints its WHOLE surface, every frame.
 *
 * WHY THIS EXISTS
 *
 * A persistence test needs a background that genuinely changes underneath the
 * thing being tested. Two earlier attempts at one used a terminal and then
 * contrib/wlrotate, and both failed their own premise check: a settled terminal
 * repaints nothing at all, and wlrotate draws 24x24 marks and then idles on a
 * single-pixel damage rectangle. Across a whole screenshot that came to 164 and
 * then 72 changed pixels -- far too little for a stale region to be stale
 * AGAINST. Without the premise assertion both runs would have reported a clean
 * corner and a passing test.
 *
 * So this client does the one thing those cannot: every generation it fills
 * every pixel of its surface with a different colour, damages the whole buffer
 * and commits. If a pixel of visible background does not follow, it is stale,
 * and there is no third explanation.
 *
 * THE PALETTE IS THE MEASUREMENT
 *
 * The two generations alternate between four fixed, widely separated colours:
 *
 *     generation even ("A")   RED    255,0,0     GREEN  0,255,0
 *     generation odd  ("B")   BLUE   0,0,255     YELLOW 255,255,0
 *
 * Every pixel changes between A and B, and -- the part that matters -- the
 * parity of any single pixel is recoverable from its colour alone. A test does
 * not have to pair up screenshots or reason about when a flip happened: in ONE
 * capture, a background region showing B with a strip of A left in it is
 * displaying a previous frame, and that is a stale pixel proven in a single
 * image. None of the four is a colour a wallpaper, a border or an unpainted
 * output is likely to be, so "not background" is separable from "wrong parity".
 *
 * The checker makes small errors legible: a one-pixel or thin stale strip cuts
 * across cell boundaries and shows up as a broken checker rather than as a
 * slightly-off flat field.
 *
 * BUFFER OWNERSHIP
 *
 * Buffers are only drawn into while the compositor does not hold them --
 * wl_buffer.release is tracked per buffer and a busy one is never touched. The
 * point of this client is deterministic change, so it must itself be beyond
 * suspicion; a client scribbling into an attached buffer produces exactly the
 * tearing artifact a persistence test is trying to attribute to the compositor.
 *
 *   --width W        initial surface width  (default 900)
 *   --height H       initial surface height (default 1000)
 *   --size WxH       both at once
 *   --fixed          keep that size whatever the compositor configures. By
 *                    default the surface follows its configure, so a tiled
 *                    wlrepaint actually fills its tile -- a background client
 *                    that stays 900x1000 inside a 1920x1080 tile leaves the
 *                    area under test unpainted, which is the same broken
 *                    premise in a new disguise.
 *   --cell N         checker cell in pixels (default 16)
 *   --solid RRGGBB   ignore the palette and paint this one colour forever.
 *                    For the FOREGROUND of a persistence test: stationary,
 *                    visually flat and not a terminal, so the only thing
 *                    changing in the scene is the background.
 *   --pattern P      checker (default) or flat. flat still changes every pixel
 *                    every generation; it exists as the control that shows a
 *                    stale region can be missed when there is no fine detail.
 *   --frames N       after N generations stop committing entirely and go quiet
 *                    (default 0 = forever). A settled client still answers a
 *                    configure -- it just does not damage itself for nothing,
 *                    which is what a stationary foreground has to do for a
 *                    persistence test to mean anything.
 *   --title NAME     wl_surface title and app_id, so a fixture running two of
 *                    these can tell them apart in `get all-clients`
 *   --ssd            ask for server-side decorations, which is what makes the
 *                    compositor draw a BORDER around this window at all
 *   --hold-ms MS     pause after each frame callback (default 100, so ~10
 *                    generations a second: fast enough that any two captures a
 *                    quarter-second apart straddle a flip, slow enough that the
 *                    fixture is not itself the load being measured)
 *   --buffers N      pool size (default 2, max 4)
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#define MAX_BUFFERS 4

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *wm_base;
static struct zxdg_decoration_manager_v1 *decoration_manager;

static struct wl_surface *surface;
static struct xdg_surface *xdg_surface;
static struct xdg_toplevel *toplevel;

struct slot {
	struct wl_buffer *buffer;
	uint32_t *pixels;
	bool busy;
};
static struct slot slots[MAX_BUFFERS];

static int width = 900, height = 1000;
static int pending_width, pending_height;
static size_t stride;
static uint8_t *map;
static size_t map_size;
static int nbuffers = 2;
static int cell = 16;
static int nframes = 0;
static int hold_ms = 100;
static bool flat = false;
static bool fixed_size = false;
static bool want_ssd = false;
static bool have_solid = false;
static uint32_t solid = 0xff202020u;
static bool configured;
static bool frame_done = true;
static const char *title = "wlrepaint";

/*
 * The four colours, as a test reading a screenshot has to know them. Keep these
 * in step with docs/regression-testing.md and with any fixture that classifies
 * pixels by parity -- they are an interface, not decoration.
 */
static const uint32_t PALETTE[2][2] = {
	{ 0xffff0000u, 0xff00ff00u },  /* generation A: red,  green  */
	{ 0xff0000ffu, 0xffffff00u },  /* generation B: blue, yellow */
};

static void wm_base_ping(void *data, struct xdg_wm_base *base, uint32_t serial) {
	(void)data;
	xdg_wm_base_pong(base, serial);
}
static const struct xdg_wm_base_listener wm_base_listener = {
	.ping = wm_base_ping,
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
	(void)data; (void)t; (void)states;
	if (fixed_size || w <= 0 || h <= 0) {
		return;
	}
	pending_width = w;
	pending_height = h;
}
static void toplevel_close(void *data, struct xdg_toplevel *t) {
	(void)data; (void)t;
	exit(0);
}
static const struct xdg_toplevel_listener toplevel_listener = {
	.configure = toplevel_configure,
	.close = toplevel_close,
};

static void buffer_release(void *data, struct wl_buffer *b) {
	(void)b;
	((struct slot *)data)->busy = false;
}
static const struct wl_buffer_listener buffer_listener = {
	.release = buffer_release,
};

static void frame_callback(void *data, struct wl_callback *cb, uint32_t time) {
	(void)data; (void)time;
	wl_callback_destroy(cb);
	frame_done = true;
}
static const struct wl_callback_listener frame_listener = {
	.done = frame_callback,
};

static void registry_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version) {
	(void)data; (void)version;
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		compositor = wl_registry_bind(registry, name,
			&wl_compositor_interface, 4);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
		wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
		xdg_wm_base_add_listener(wm_base, &wm_base_listener, NULL);
	} else if (strcmp(interface,
			zxdg_decoration_manager_v1_interface.name) == 0) {
		decoration_manager = wl_registry_bind(registry, name,
			&zxdg_decoration_manager_v1_interface, 1);
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

/* Every pixel, every time. The checker phase is fixed to the buffer, not to the
 * generation, so a stale strip stays aligned with its neighbours and is read as
 * the wrong COLOUR rather than as a shifted pattern. */
static void repaint(struct slot *s, int generation) {
	const uint32_t *pair = PALETTE[generation & 1];
	for (int r = 0; r < height; r++) {
		uint32_t *row = (uint32_t *)((uint8_t *)s->pixels + (size_t)r * stride);
		int rband = flat ? 0 : (r / cell) & 1;
		for (int c = 0; c < width; c++) {
			row[c] = have_solid ? solid :
				flat ? pair[0] : pair[rband ^ (((c / cell) & 1))];
		}
	}
}

static int alloc_pool(void) {
	stride = (size_t)width * 4;
	size_t one = stride * (size_t)height;
	map_size = one * (size_t)nbuffers;

	char name[] = "/wlrepaint-XXXXXX";
	int fd = -1;
	for (int attempt = 0; attempt < 16 && fd < 0; attempt++) {
		for (char *p = name + 11; *p; p++) {
			*p = 'a' + (rand() % 26);
		}
		fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
	}
	if (fd < 0) {
		fprintf(stderr, "wlrepaint: shm_open failed: %s\n", strerror(errno));
		return -1;
	}
	shm_unlink(name);
	if (ftruncate(fd, (off_t)map_size) != 0) {
		fprintf(stderr, "wlrepaint: ftruncate failed: %s\n", strerror(errno));
		close(fd);
		return -1;
	}
	map = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED) {
		fprintf(stderr, "wlrepaint: mmap failed: %s\n", strerror(errno));
		close(fd);
		return -1;
	}

	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)map_size);
	for (int i = 0; i < nbuffers; i++) {
		slots[i].buffer = wl_shm_pool_create_buffer(pool,
			(int32_t)(one * (size_t)i), width, height, (int32_t)stride,
			WL_SHM_FORMAT_ARGB8888);
		slots[i].pixels = (uint32_t *)(map + one * (size_t)i);
		slots[i].busy = false;
		wl_buffer_add_listener(slots[i].buffer, &buffer_listener, &slots[i]);
	}
	wl_shm_pool_destroy(pool);
	close(fd);
	return 0;
}

/*
 * Retire the current pool rather than freeing it.
 *
 * The buffer a surface is CURRENTLY showing is never released -- release means
 * "I am finished with this one", and the compositor is not finished with the
 * one on screen until a different buffer replaces it. So a resize path that
 * waits for every buffer to come back before reallocating waits forever, which
 * is exactly what the first version of this did: one generation, one configure,
 * and a client wedged for the rest of the run while the fixture reported a
 * background that never changed.
 *
 * Instead the old pool is held for one full resize generation. By the time a
 * second resize retires it, the surface has long since been showing buffers
 * from a newer pool, and the compositor's own mapping of the fd is its
 * business either way.
 */
static struct wl_buffer *retired_buffers[MAX_BUFFERS];
static uint8_t *retired_map;
static size_t retired_map_size;

static void retire_pool(void) {
	for (int i = 0; i < MAX_BUFFERS; i++) {
		if (retired_buffers[i] != NULL) {
			wl_buffer_destroy(retired_buffers[i]);
			retired_buffers[i] = NULL;
		}
	}
	if (retired_map != NULL) {
		munmap(retired_map, retired_map_size);
		retired_map = NULL;
	}
	for (int i = 0; i < nbuffers; i++) {
		retired_buffers[i] = slots[i].buffer;
		slots[i].buffer = NULL;
		slots[i].pixels = NULL;
		slots[i].busy = false;
	}
	retired_map = map;
	retired_map_size = map_size;
	map = NULL;
}

/* Block until a buffer the compositor no longer holds is available. With two
 * buffers and frame-callback pacing this returns immediately in practice; it
 * exists so that it is never possible for this client to write into a buffer
 * that is on screen. */
static struct slot *acquire(void) {
	for (;;) {
		for (int i = 0; i < nbuffers; i++) {
			if (!slots[i].busy) {
				return &slots[i];
			}
		}
		if (wl_display_dispatch(display) == -1) {
			return NULL;
		}
	}
}

int main(int argc, char **argv) {
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--width") == 0 && i + 1 < argc) {
			width = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc) {
			height = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
			sscanf(argv[++i], "%dx%d", &width, &height);
		} else if (strcmp(argv[i], "--cell") == 0 && i + 1 < argc) {
			cell = atoi(argv[++i]);
			if (cell < 1) cell = 1;
		} else if (strcmp(argv[i], "--pattern") == 0 && i + 1 < argc) {
			flat = strcmp(argv[++i], "flat") == 0;
		} else if (strcmp(argv[i], "--solid") == 0 && i + 1 < argc) {
			solid = 0xff000000u | (uint32_t)strtoul(argv[++i], NULL, 16);
			have_solid = true;
		} else if (strcmp(argv[i], "--fixed") == 0) {
			fixed_size = true;
		} else if (strcmp(argv[i], "--ssd") == 0) {
			want_ssd = true;
		} else if (strcmp(argv[i], "--title") == 0 && i + 1 < argc) {
			title = argv[++i];
		} else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
			nframes = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--hold-ms") == 0 && i + 1 < argc) {
			hold_ms = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--buffers") == 0 && i + 1 < argc) {
			nbuffers = atoi(argv[++i]);
			if (nbuffers < 2) nbuffers = 2;
			if (nbuffers > MAX_BUFFERS) nbuffers = MAX_BUFFERS;
		} else {
			fprintf(stderr, "wlrepaint: unknown option %s\n", argv[i]);
			return 1;
		}
	}
	if (width < 1 || height < 1) {
		fprintf(stderr, "wlrepaint: bad surface size\n");
		return 1;
	}

	display = wl_display_connect(NULL);
	if (display == NULL) {
		fprintf(stderr, "wlrepaint: cannot connect to a Wayland display\n");
		return 1;
	}
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);
	if (compositor == NULL || shm == NULL || wm_base == NULL) {
		fprintf(stderr, "wlrepaint: compositor is missing wl_shm or xdg_shell\n");
		return 1;
	}

	if (alloc_pool() != 0) {
		return 1;
	}

	surface = wl_compositor_create_surface(compositor);
	xdg_surface = xdg_wm_base_get_xdg_surface(wm_base, surface);
	xdg_surface_add_listener(xdg_surface, &xdg_surface_listener, NULL);
	toplevel = xdg_surface_get_toplevel(xdg_surface);
	xdg_toplevel_add_listener(toplevel, &toplevel_listener, NULL);
	xdg_toplevel_set_title(toplevel, title);
	xdg_toplevel_set_app_id(toplevel, title);
	/*
	 * A client that never binds xdg-decoration is client-side decorated by
	 * default (src/fetch/client.h, client_wants_ssd), and asteroidz then draws
	 * NO server border for it -- it still reserves borderpx around the surface,
	 * so the window looks bordered and the ring is simply empty. A border test
	 * run against such a client measures that empty ring, finds background
	 * where it expected border, and blames the renderer. Ask for the border.
	 */
	if (want_ssd) {
		if (decoration_manager == NULL) {
			fprintf(stderr, "wlrepaint: --ssd but no xdg-decoration; this "
				"window will have no server border\n");
		} else {
			struct zxdg_toplevel_decoration_v1 *dec =
				zxdg_decoration_manager_v1_get_toplevel_decoration(
					decoration_manager, toplevel);
			zxdg_toplevel_decoration_v1_set_mode(dec,
				ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
		}
	}
	wl_surface_commit(surface);
	while (!configured && wl_display_dispatch(display) != -1) {
	}

	printf("wlrepaint: %dx%d cell %d pattern %s buffers %d\n", width, height,
		cell, have_solid ? "solid" : flat ? "flat" : "checker", nbuffers);
	fflush(stdout);

	int gen = 0;
	bool settled = false;
	for (;;) {
		/* Frame callbacks are the pacing: a client that commits as fast as it
		 * can is not a realistic background and floods the compositor with the
		 * very damage the test is trying to measure. */
		while (!frame_done && wl_display_dispatch(display) != -1) {
		}
		if (settled) {
			/* Block until something happens. A settled foreground has to stop
			 * committing COMPLETELY -- a window that keeps re-committing
			 * identical content damages its own box every frame, which repaints
			 * the very region a persistence test is watching and hides exactly
			 * the bug it was built to find. It must still answer a configure,
			 * hence dispatch rather than sleep. */
			if (wl_display_dispatch(display) == -1) {
				break;
			}
		} else if (hold_ms > 0) {
			struct timespec ts = {
				.tv_sec = hold_ms / 1000,
				.tv_nsec = (long)(hold_ms % 1000) * 1000000L,
			};
			nanosleep(&ts, NULL);
			wl_display_dispatch_pending(display);
		}

		/* Follow the configure. Reallocating needs every buffer back first,
		 * so this waits for the releases rather than assuming them. */
		bool resized = false;
		if (pending_width > 0 &&
				(pending_width != width || pending_height != height)) {
			retire_pool();
			width = pending_width;
			height = pending_height;
			if (alloc_pool() != 0) {
				return 1;
			}
			printf("wlrepaint: resized to %dx%d\n", width, height);
			fflush(stdout);
			resized = true;
		}

		/* A resize always earns a frame, settled or not: the surface has a new
		 * buffer size and nothing on screen for it. */
		if (nframes > 0 && gen >= nframes && !resized) {
			if (!settled) {
				printf("wlrepaint: settled after %d generations\n", gen);
				fflush(stdout);
				settled = true;
			}
			continue;
		}
		settled = false;

		struct slot *s = acquire();
		if (s == NULL) {
			break;
		}
		repaint(s, gen);

		struct wl_callback *cb = wl_surface_frame(surface);
		wl_callback_add_listener(cb, &frame_listener, NULL);
		frame_done = false;

		wl_surface_attach(surface, s->buffer, 0, 0);
		wl_surface_damage_buffer(surface, 0, 0, width, height);
		wl_surface_commit(surface);
		s->busy = true;
		wl_display_flush(display);

		/* The self-check a fixture reads back: the generation advances, the
		 * parity alternates, and the damage really is the whole surface. */
		printf("wlrepaint: gen %d pattern %c damage 0,0 %dx%d\n", gen,
			(gen & 1) ? 'B' : 'A', width, height);
		fflush(stdout);
		gen++;
	}
	return 0;
}
