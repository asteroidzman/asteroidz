/*
 * wlreuse — a wl_shm client that reuses ONE buffer for every commit.
 *
 * This exists to catch one specific wrong implementation of a renderer-side
 * pixel cache: keying it on buffer identity.
 *
 * A compositor that caches "I have already uploaded this wlr_buffer" and skips
 * later commits of the same pointer is fast, plausible, and renders this
 * client permanently stuck on its first colour. Nothing else in the test suite
 * catches it, because every ordinary toolkit rotates through a pool of two or
 * three buffers and a pointer-identity cache happens to look correct: each new
 * pointer is a cache miss, so the pixels get through anyway.
 *
 * So this client does the thing the protocol allows and toolkits mostly do not:
 *
 *     wl_shm_pool_create_buffer()   ONCE
 *     loop:
 *         memset the mapping to the next colour
 *         wl_surface_attach(SAME wl_buffer)
 *         wl_surface_damage_buffer(whole)
 *         wl_surface_commit()
 *
 * It never releases and never reallocates. Every generation has the same
 * wl_buffer, the same wl_shm_pool, the same file descriptor and the same
 * address. Only the bytes differ.
 *
 * Usage:
 *     wlreuse --colour RRGGBB [--colour RRGGBB ...] [--hold-ms N] [--size WxH]
 *            [--stride-pad N] [--damage WxH+X+Y] [--two-regions] [--no-damage]
 *
 * The later options serve the partial-upload path:
 *
 *   --damage WxH+X+Y   after the first generation, change and report ONLY
 *                      this rectangle. Everything outside it keeps the
 *                      previous colour, so "the rest is still correct" is a
 *                      real assertion rather than a tautology.
 *   --two-regions      a second rectangle in the opposite corner. A
 *                      bounding-box implementation repaints between them; an
 *                      omitted region leaves one stale.
 *   --stride-pad N     rows N bytes further apart than width * 4. A copy that
 *                      treats a rectangle as contiguous, or computes the
 *                      source row as y * width * bpp, shears or offsets.
 *   --many-regions N   N disjoint 8x8 rectangles. Past the compositor's
 *                      packing limit this is damage it cannot represent, which
 *                      must become ONE full upload rather than a wrong partial
 *                      one.
 *   --no-damage        commit a new generation and say nothing about what
 *                      changed. Per the protocol that means "nothing changed",
 *                      and the correct response is to upload nothing at all --
 *                      which is what wlroots' own texture path does too.
 *
 * It cycles through the colours, one per commit, holding each for --hold-ms
 * (default 1500), and runs until killed. A test screenshots between commits
 * and asserts the colour on screen followed the commits rather than freezing
 * on the first one.
 *
 * Deliberately NOT using a buffer pool, double buffering, or frame callbacks
 * gated on wl_buffer.release: waiting for a release would be the correct thing
 * for a real application and would defeat the entire purpose here. The
 * compositor holds a lock on the committed buffer, so no release arrives until
 * the buffer is replaced -- which never happens -- and a well-behaved client
 * would simply stop drawing. Writing into an attached buffer is what makes
 * this a test rather than an application.
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
static struct xdg_wm_base *wm_base;

static struct wl_surface *surface;
static struct xdg_surface *xdg_surface;
static struct xdg_toplevel *toplevel;
static struct wl_buffer *buffer;
static uint32_t *pixels;
static int width = 400, height = 300;
static int stride_pad = 0;      /* extra bytes per row beyond width * 4 */
static int damage_w = 0, damage_h = 0, damage_x = 0, damage_y = 0;
static bool two_regions = false;
static bool no_damage = false;
static int many_regions = 0;
static bool configured;
static bool running = true;

static void wm_base_ping(void *data, struct xdg_wm_base *base, uint32_t serial) {
	(void)data;
	xdg_wm_base_pong(base, serial);
}
static const struct xdg_wm_base_listener wm_base_listener = {
	.ping = wm_base_ping,
};

static void registry_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version) {
	(void)data; (void)version;
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
		wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
		xdg_wm_base_add_listener(wm_base, &wm_base_listener, NULL);
	}
}
static void registry_global_remove(void *data, struct wl_registry *r, uint32_t n) {
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
	configured = true;
}
static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = xdg_surface_configure,
};

static void toplevel_configure(void *data, struct xdg_toplevel *tl,
		int32_t w, int32_t h, struct wl_array *states) {
	(void)data; (void)tl; (void)states;
	/* The size is deliberately ignored: the buffer is allocated once and must
	 * never be reallocated, which is the entire point of this client. A
	 * mismatch just means the compositor scales or crops it. */
	(void)w; (void)h;
}
static void toplevel_close(void *data, struct xdg_toplevel *tl) {
	(void)data; (void)tl;
	running = false;
}
static const struct xdg_toplevel_listener toplevel_listener = {
	.configure = toplevel_configure,
	.close = toplevel_close,
};

/* An anonymous file to share with the compositor. */
static int anon_file(size_t size) {
	int fd = memfd_create("wlreuse", MFD_CLOEXEC | MFD_ALLOW_SEALING);
	if (fd < 0) {
		return -1;
	}
	if (ftruncate(fd, (off_t)size) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

int main(int argc, char **argv) {
	uint32_t colours[16];
	int colour_count = 0;
	int hold_ms = 1500;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--colour") == 0 && i + 1 < argc) {
			if (colour_count < 16) {
				colours[colour_count++] =
					0xff000000u | (uint32_t)strtoul(argv[++i], NULL, 16);
			}
		} else if (strcmp(argv[i], "--hold-ms") == 0 && i + 1 < argc) {
			hold_ms = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
			sscanf(argv[++i], "%dx%d", &width, &height);
		} else if (strcmp(argv[i], "--stride-pad") == 0 && i + 1 < argc) {
			/* Rows further apart than width * 4. A copy that treats a
			 * rectangle as one contiguous block, or that computes the source
			 * address as y * width * bpp, shears or offsets the result. */
			stride_pad = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--damage") == 0 && i + 1 < argc) {
			/* Change ONLY this rectangle after the first generation, and
			 * report only it as damage. */
			sscanf(argv[++i], "%dx%d+%d+%d", &damage_w, &damage_h,
				&damage_x, &damage_y);
		} else if (strcmp(argv[i], "--two-regions") == 0) {
			/* A second, widely separated rectangle. Catches a bounding-box
			 * implementation (which would also repaint between them) and an
			 * omitted region (which would leave one of them stale). */
			two_regions = true;
		} else if (strcmp(argv[i], "--many-regions") == 0 && i + 1 < argc) {
			/* N disjoint little rectangles. Past the compositor's packing
			 * limit this is damage it cannot represent as regions, which has
			 * to become one full upload rather than a wrong partial one. */
			many_regions = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--no-damage") == 0) {
			/* Commit a new generation without saying what changed. The
			 * compositor has to fall back to one full upload. */
			no_damage = true;
		} else {
			fprintf(stderr, "usage: %s --colour RRGGBB [--colour RRGGBB ...] "
				"[--hold-ms N] [--size WxH]\n", argv[0]);
			return 1;
		}
	}
	if (colour_count == 0) {
		colours[colour_count++] = 0xffff0000u;  /* red */
		colours[colour_count++] = 0xff0000ffu;  /* blue */
	}

	struct wl_display *display = wl_display_connect(NULL);
	if (display == NULL) {
		fprintf(stderr, "wlreuse: cannot connect to a Wayland display\n");
		return 1;
	}
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);
	if (compositor == NULL || shm == NULL || wm_base == NULL) {
		fprintf(stderr, "wlreuse: compositor, shm or xdg_wm_base missing\n");
		return 1;
	}

	size_t stride = (size_t)width * 4 + (size_t)stride_pad;
	size_t size = stride * (size_t)height;
	int fd = anon_file(size);
	if (fd < 0) {
		fprintf(stderr, "wlreuse: cannot create the shared file: %s\n",
			strerror(errno));
		return 1;
	}
	pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (pixels == MAP_FAILED) {
		fprintf(stderr, "wlreuse: mmap failed: %s\n", strerror(errno));
		return 1;
	}

	/* ONE pool, ONE buffer, for the whole lifetime of the process. */
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)size);
	buffer = wl_shm_pool_create_buffer(pool, 0, width, height,
		(int32_t)stride, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);

	surface = wl_compositor_create_surface(compositor);
	xdg_surface = xdg_wm_base_get_xdg_surface(wm_base, surface);
	xdg_surface_add_listener(xdg_surface, &xdg_surface_listener, NULL);
	toplevel = xdg_surface_get_toplevel(xdg_surface);
	xdg_toplevel_add_listener(toplevel, &toplevel_listener, NULL);
	xdg_toplevel_set_title(toplevel, "wlreuse");
	xdg_toplevel_set_app_id(toplevel, "wlreuse");
	wl_surface_commit(surface);

	while (!configured && wl_display_dispatch(display) != -1) {
	}

	for (int generation = 0; running; generation++) {
		uint32_t colour = colours[generation % colour_count];
		bool partial = generation > 0 && damage_w > 0 && damage_h > 0;
		bool many = generation > 0 && many_regions > 0;
		if (many) {
			for (int k = 0; k < many_regions; k++) {
				int rx = (k * 11) % (width - 8);
				int ry = (k * 13) % (height - 8);
				for (int r = 0; r < 8; r++) {
					uint32_t *row = (uint32_t *)((uint8_t *)pixels
						+ (size_t)(ry + r) * stride);
					for (int c = 0; c < 8; c++) {
						row[rx + c] = colour;
					}
				}
			}
		}

		if (many) {
			/* handled above */
		} else if (partial) {
			/* Only the damaged rectangles change. Everything else keeps the
			 * previous generation's pixels, which is what makes the "the rest
			 * is still correct" assertion meaningful. Written row by row
			 * through the real stride. */
			for (int r = 0; r < damage_h; r++) {
				uint32_t *row = (uint32_t *)((uint8_t *)pixels
					+ (size_t)(damage_y + r) * stride);
				for (int c = 0; c < damage_w; c++) {
					row[damage_x + c] = colour;
				}
			}
			if (two_regions) {
				int x2 = width - damage_w - damage_x;
				int y2 = height - damage_h - damage_y;
				for (int r = 0; r < damage_h; r++) {
					uint32_t *row = (uint32_t *)((uint8_t *)pixels
						+ (size_t)(y2 + r) * stride);
					for (int c = 0; c < damage_w; c++) {
						row[x2 + c] = colour;
					}
				}
			}
		} else {
			for (int r = 0; r < height; r++) {
				uint32_t *row = (uint32_t *)((uint8_t *)pixels
					+ (size_t)r * stride);
				for (int c = 0; c < width; c++) {
					row[c] = colour;
				}
			}
		}

		/* The same wl_buffer object, every single time. */
		wl_surface_attach(surface, buffer, 0, 0);
		if (no_damage && generation > 0) {
			/* Deliberately silent about what changed. */
		} else if (many) {
			for (int k = 0; k < many_regions; k++) {
				wl_surface_damage_buffer(surface, (k * 11) % (width - 8),
					(k * 13) % (height - 8), 8, 8);
			}
		} else if (partial) {
			wl_surface_damage_buffer(surface, damage_x, damage_y,
				damage_w, damage_h);
			if (two_regions) {
				wl_surface_damage_buffer(surface, width - damage_w - damage_x,
					height - damage_h - damage_y, damage_w, damage_h);
			}
		} else {
			wl_surface_damage_buffer(surface, 0, 0, width, height);
		}
		wl_surface_commit(surface);
		wl_display_flush(display);

		printf("wlreuse: generation %d committed as #%06x\n", generation + 1,
			colour & 0xffffffu);
		fflush(stdout);

		struct timespec ts = {
			.tv_sec = hold_ms / 1000,
			.tv_nsec = (long)(hold_ms % 1000) * 1000000L,
		};
		nanosleep(&ts, NULL);
		/* Drain anything the compositor sent while we were asleep, without
		 * blocking: a blocking dispatch would stall forever once the
		 * compositor stops sending events. */
		while (wl_display_prepare_read(display) != 0) {
			wl_display_dispatch_pending(display);
		}
		wl_display_flush(display);
		wl_display_cancel_read(display);
		wl_display_dispatch_pending(display);
	}

	wl_display_disconnect(display);
	return 0;
}
