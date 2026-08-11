/*
 * wlrotate -- a wl_shm client that rotates a pool of buffers.
 *
 * WHY THIS EXISTS
 *
 * wl_surface.damage_buffer states what changed since the PREVIOUS COMMIT of
 * the surface. It says nothing about how far any particular wl_buffer has
 * drifted from the last time the compositor saw that buffer.
 *
 * While a client reuses one buffer those are the same region, and
 * contrib/wlreuse -- which holds one buffer for its whole life -- cannot tell
 * them apart. Rotate two buffers and they come apart immediately:
 *
 *     commit 1   buffer A   draws mark 0            damage = mark 0
 *     commit 2   buffer B   draws marks 0 and 1     damage = mark 1
 *     commit 3   buffer A   draws marks 0, 1 and 2  damage = mark 2
 *
 * Commit 3 is entirely correct Wayland: buffer A really does contain all
 * three marks, and mark 2 really is all that changed since commit 2. But a
 * compositor caching one image per buffer last saw A at commit 1, so applying
 * only "mark 2" to its copy leaves mark 1 missing. Present A and B alternately
 * and the client's own recently-drawn pixels blink.
 *
 * That is the bug this client was written to catch, found on a real desktop
 * as KDE applications flickering on their status bar and hover highlight for
 * the first seconds of their life, and invisible to a suite whose only
 * buffer-reusing client kept exactly one buffer.
 *
 * WHAT IT DRAWS
 *
 * A background, and a row of marks added one per generation. Each mark, once
 * drawn, is permanent: every subsequent buffer is repainted with every mark so
 * far before being committed, which is the age-correct repaint a real toolkit
 * performs. Damage covers only the newest mark.
 *
 * So the assertion available to a test is simply: every mark that has been
 * drawn is on screen, in every frame, no matter which buffer is showing.
 *
 *   --buffers N     how many buffers to rotate (default 2, max 8). 1 makes
 *                   this client behave like wlreuse, and the bug it exists to
 *                   catch cannot occur -- useful for proving the test is
 *                   measuring rotation and not something else.
 *   --marks N       how many marks to draw (default 4)
 *   --size WxH      surface size (default 400x300)
 *   --hold-ms MS    pause between commits (default 120)
 *   --idle-commits N  after the marks are drawn, keep committing this many
 *                   times with a tiny damage rectangle in a corner, so the
 *                   displayed buffer keeps rotating with no new content.
 *                   This is what makes a stale cache visible as a flicker
 *                   rather than a single wrong frame.
 *   --full-damage   report the whole buffer every commit. Correct, wasteful,
 *                   and immune to the bug -- the control case.
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

#include "xdg-shell-client-protocol.h"

#define MAX_BUFFERS 8

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *wm_base;

static struct wl_surface *surface;
static struct xdg_surface *xdg_surface;
static struct xdg_toplevel *toplevel;

static struct wl_buffer *buffers[MAX_BUFFERS];
static uint32_t *pixels[MAX_BUFFERS];

static int width = 400, height = 300;
static size_t stride;
static int nbuffers = 2;
static int nmarks = 4;
static int hold_ms = 120;
static int idle_commits = 12;
static bool full_damage = false;
static bool configured;

static const uint32_t BACKGROUND = 0xff202020u;
static const uint32_t MARK = 0xffff4040u;

/* Marks sit in a row, well clear of the edges so a test can find them. */
static void mark_box(int index, int *x, int *y, int *w, int *h) {
	*w = 24;
	*h = 24;
	*x = 20 + index * 40;
	*y = height / 2;
}

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

/*
 * Repaint a buffer so it holds every mark drawn so far.
 *
 * This is the age-correct repaint: a real toolkit tracks how old the buffer it
 * just acquired is and redraws everything that has changed since. Doing it the
 * lazy way here -- redrawing the lot -- is a superset of that and keeps the
 * client unambiguously correct, which matters because the whole point is that
 * the COMPOSITOR is wrong.
 */
static void repaint(int slot, int marks_so_far) {
	uint32_t *base = pixels[slot];
	for (int r = 0; r < height; r++) {
		uint32_t *row = (uint32_t *)((uint8_t *)base + (size_t)r * stride);
		for (int c = 0; c < width; c++) {
			row[c] = BACKGROUND;
		}
	}
	for (int m = 0; m < marks_so_far; m++) {
		int x, y, w, h;
		mark_box(m, &x, &y, &w, &h);
		for (int r = y; r < y + h && r < height; r++) {
			uint32_t *row = (uint32_t *)((uint8_t *)base + (size_t)r * stride);
			for (int c = x; c < x + w && c < width; c++) {
				row[c] = MARK;
			}
		}
	}
}

int main(int argc, char **argv) {
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--buffers") == 0 && i + 1 < argc) {
			nbuffers = atoi(argv[++i]);
			if (nbuffers < 1) nbuffers = 1;
			if (nbuffers > MAX_BUFFERS) nbuffers = MAX_BUFFERS;
		} else if (strcmp(argv[i], "--marks") == 0 && i + 1 < argc) {
			nmarks = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
			sscanf(argv[++i], "%dx%d", &width, &height);
		} else if (strcmp(argv[i], "--hold-ms") == 0 && i + 1 < argc) {
			hold_ms = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--idle-commits") == 0 && i + 1 < argc) {
			idle_commits = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--full-damage") == 0) {
			full_damage = true;
		} else {
			fprintf(stderr, "wlrotate: unknown option %s\n", argv[i]);
			return 1;
		}
	}

	display = wl_display_connect(NULL);
	if (display == NULL) {
		fprintf(stderr, "wlrotate: cannot connect to a Wayland display\n");
		return 1;
	}
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);
	if (compositor == NULL || shm == NULL || wm_base == NULL) {
		fprintf(stderr, "wlrotate: compositor is missing wl_shm or xdg_shell\n");
		return 1;
	}

	stride = (size_t)width * 4;
	size_t one = stride * (size_t)height;
	size_t total = one * (size_t)nbuffers;

	char name[] = "/wlrotate-XXXXXX";
	int fd = -1;
	for (int attempt = 0; attempt < 16 && fd < 0; attempt++) {
		for (char *p = name + 10; *p; p++) {
			*p = 'a' + (rand() % 26);
		}
		fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
	}
	if (fd < 0) {
		fprintf(stderr, "wlrotate: shm_open failed: %s\n", strerror(errno));
		return 1;
	}
	shm_unlink(name);
	if (ftruncate(fd, (off_t)total) != 0) {
		fprintf(stderr, "wlrotate: ftruncate failed: %s\n", strerror(errno));
		return 1;
	}
	uint8_t *map = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED) {
		fprintf(stderr, "wlrotate: mmap failed: %s\n", strerror(errno));
		return 1;
	}

	/* One pool, N buffers inside it -- exactly how a toolkit lays out a
	 * rotating pool, and how the compositor comes to hold N cached images
	 * that go out of date at different rates. */
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)total);
	for (int i = 0; i < nbuffers; i++) {
		buffers[i] = wl_shm_pool_create_buffer(pool, (int32_t)(one * (size_t)i),
			width, height, (int32_t)stride, WL_SHM_FORMAT_ARGB8888);
		pixels[i] = (uint32_t *)(map + one * (size_t)i);
	}
	wl_shm_pool_destroy(pool);
	close(fd);

	surface = wl_compositor_create_surface(compositor);
	xdg_surface = xdg_wm_base_get_xdg_surface(wm_base, surface);
	xdg_surface_add_listener(xdg_surface, &xdg_surface_listener, NULL);
	toplevel = xdg_surface_get_toplevel(xdg_surface);
	xdg_toplevel_add_listener(toplevel, &toplevel_listener, NULL);
	xdg_toplevel_set_title(toplevel, "wlrotate");
	xdg_toplevel_set_app_id(toplevel, "wlrotate");
	wl_surface_commit(surface);
	while (!configured && wl_display_dispatch(display) != -1) {
	}

	int total_commits = nmarks + idle_commits;
	for (int gen = 0; gen < total_commits; gen++) {
		int slot = gen % nbuffers;
		int marks_so_far = gen < nmarks ? gen + 1 : nmarks;
		repaint(slot, marks_so_far);

		wl_surface_attach(surface, buffers[slot], 0, 0);
		if (full_damage) {
			wl_surface_damage_buffer(surface, 0, 0, width, height);
		} else if (gen < nmarks) {
			/* Only the newest mark: everything else was already reported on
			 * the commit that introduced it. Correct, and minimal. */
			int x, y, w, h;
			mark_box(gen, &x, &y, &w, &h);
			wl_surface_damage_buffer(surface, x, y, w, h);
		} else {
			/* Nothing new to say. A single pixel in a corner keeps the
			 * surface committing -- and the buffers rotating -- without
			 * touching any mark. */
			wl_surface_damage_buffer(surface, 0, 0, 1, 1);
		}
		wl_surface_commit(surface);
		wl_display_flush(display);

		printf("wlrotate: commit %d slot %d marks %d\n", gen + 1, slot,
			marks_so_far);
		fflush(stdout);

		struct timespec ts = {
			.tv_sec = hold_ms / 1000,
			.tv_nsec = (long)(hold_ms % 1000) * 1000000L,
		};
		nanosleep(&ts, NULL);
		wl_display_dispatch_pending(display);
	}

	/* Stay up so a test can screenshot a settled surface. */
	while (wl_display_dispatch(display) != -1) {
	}
	return 0;
}
