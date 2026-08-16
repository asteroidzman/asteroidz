/*
 * wlsync — a dma-buf client that speaks wp_linux_drm_syncobj_v1, and nothing
 * else in this tree does.
 *
 * WHY IT EXISTS
 *
 * asteroidz advertises wp_linux_drm_syncobj_manager_v1. Every serious GPU
 * client on this machine takes that offer -- gamescope logs "Using explicit
 * sync when available" and then uses it for every single frame it hands over.
 * The protocol is a two-sided contract:
 *
 *   set_acquire_point   the compositor MUST NOT read the buffer until this
 *                       timeline point signals. It is the client saying "my
 *                       GPU is still writing this".
 *   set_release_point   the compositor MUST signal this once it has finished
 *                       reading, and not before. It is the client's licence
 *                       to draw into that buffer again.
 *
 * Both halves are invisible in a screenshot. A compositor that honours
 * neither renders a perfect-looking frame on nearly every attempt, and fails
 * as tearing, as pixel noise, or as one frame of the previous contents, only
 * when the client's GPU work happens to still be in flight. That is exactly
 * the failure this machine has already seen once from gamescope, and it is
 * why the assertions that go with this client are on counters and on the
 * client's own timeline rather than on pixels.
 *
 * WHAT IT DOES
 *
 *   - allocates LINEAR dma-bufs through GBM on a render node, so the pixels
 *     can be written from the CPU and the buffer is still a real dma-buf that
 *     takes the compositor's zero-copy import path
 *   - imports one DRM syncobj timeline into the compositor
 *   - commits with an acquire point and a release point on every frame
 *   - waits for the compositor to signal each release point before reusing
 *     that buffer, which is what the protocol says a client may do and what
 *     every explicit-sync client actually does
 *
 * The last one matters more than it looks: a compositor that advertises the
 * global and never signals a release point does not corrupt anything, it
 * DEADLOCKS the client on its second buffer rotation. Reporting the number of
 * release points that came back, and how long each took, is therefore a
 * liveness assertion and not a statistic.
 *
 * Usage:
 *     wlsync [--frames N] [--size WxH] [--buffers N] [--hold-ms N]
 *            [--acquire-delay-ms N] [--no-release-point] [--no-acquire-point]
 *            [--device PATH] [--quiet]
 *
 *   --acquire-delay-ms N   signal the acquire point N ms AFTER committing,
 *                          rather than before. A conforming compositor shows
 *                          the previous frame for those N ms. Nothing here can
 *                          see the screen, so what this actually exercises is
 *                          that the compositor does not fall over, does not
 *                          drop the commit, and still signals the release.
 *   --no-release-point     commit a buffer with an acquire point and no
 *                          release point. That is a protocol error
 *                          (no_release_point) and the compositor MUST
 *                          disconnect us. Used to prove the compositor
 *                          validates at all -- a compositor that accepts this
 *                          is not implementing the protocol, it is ignoring
 *                          it.
 *   --no-acquire-point     the mirror: a buffer with a release point and no
 *                          acquire point, which must raise no_acquire_point.
 *
 * Exit status:
 *     0  ran to completion, every release point came back
 *     1  usage / setup failure
 *     2  the compositor never signalled a release point (client would hang)
 *     3  a protocol error arrived that the run did not ask for
 *     4  the run asked for a protocol error and did not get one
 *
 * The final line on stdout is always machine-readable:
 *
 *     wlsync: commits=N releases=N release_us_max=N release_us_avg=N proto_err=NAME
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <gbm.h>
#include <xf86drm.h>

#include <wayland-client.h>

#include "xdg-shell-client-protocol.h"
#include "linux-dmabuf-v1-client-protocol.h"
#include "linux-drm-syncobj-v1-client-protocol.h"

#define MAX_BUFFERS 4

struct sbuf {
	struct gbm_bo *bo;
	struct wl_buffer *wl;
	void *map;
	void *map_data;
	uint32_t stride;
	/* the release point this buffer is waiting on, 0 when free */
	uint64_t pending_release;
	uint64_t committed_at_us;
};

static struct {
	struct wl_display *dpy;
	struct wl_registry *reg;
	struct wl_compositor *comp;
	struct xdg_wm_base *wm;
	struct zwp_linux_dmabuf_v1 *dmabuf;
	struct wp_linux_drm_syncobj_manager_v1 *syncmgr;

	struct wl_surface *surf;
	struct xdg_surface *xsurf;
	struct xdg_toplevel *top;
	struct wp_linux_drm_syncobj_surface_v1 *ssurf;

	int drm_fd;
	struct gbm_device *gbm;
	uint32_t syncobj;

	bool configured;
	bool running;
	const char *proto_err;

	uint64_t commits;
	uint64_t releases;
	uint64_t release_us_max;
	uint64_t release_us_total;
} A;

static uint64_t now_us(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static void die(const char *what) {
	fprintf(stderr, "wlsync: %s: %s\n", what, strerror(errno));
	exit(1);
}

/* ── registry ─────────────────────────────────────────────────────────── */

static void wm_ping(void *d, struct xdg_wm_base *wm, uint32_t serial) {
	(void)d;
	xdg_wm_base_pong(wm, serial);
}
static const struct xdg_wm_base_listener wm_listener = {.ping = wm_ping};

static void reg_global(void *d, struct wl_registry *r, uint32_t name,
		const char *iface, uint32_t ver) {
	(void)d;
	if (strcmp(iface, wl_compositor_interface.name) == 0) {
		A.comp = wl_registry_bind(r, name, &wl_compositor_interface,
			ver < 4 ? ver : 4);
	} else if (strcmp(iface, xdg_wm_base_interface.name) == 0) {
		A.wm = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
		xdg_wm_base_add_listener(A.wm, &wm_listener, NULL);
	} else if (strcmp(iface, zwp_linux_dmabuf_v1_interface.name) == 0) {
		A.dmabuf = wl_registry_bind(r, name, &zwp_linux_dmabuf_v1_interface,
			ver < 3 ? ver : 3);
	} else if (strcmp(iface,
			wp_linux_drm_syncobj_manager_v1_interface.name) == 0) {
		A.syncmgr = wl_registry_bind(r, name,
			&wp_linux_drm_syncobj_manager_v1_interface, 1);
	}
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t n) {
	(void)d; (void)r; (void)n;
}
static const struct wl_registry_listener reg_listener = {
	.global = reg_global, .global_remove = reg_remove,
};

/* ── xdg ──────────────────────────────────────────────────────────────── */

static void xsurf_configure(void *d, struct xdg_surface *s, uint32_t serial) {
	(void)d;
	xdg_surface_ack_configure(s, serial);
	A.configured = true;
}
static const struct xdg_surface_listener xsurf_listener = {
	.configure = xsurf_configure,
};

static void top_configure(void *d, struct xdg_toplevel *t, int32_t w,
		int32_t h, struct wl_array *st) {
	(void)d; (void)t; (void)w; (void)h; (void)st;
}
static void top_close(void *d, struct xdg_toplevel *t) {
	(void)d; (void)t;
	A.running = false;
}
static const struct xdg_toplevel_listener top_listener = {
	.configure = top_configure, .close = top_close,
};

/* ── dmabuf params ────────────────────────────────────────────────────── */

static void params_created(void *data, struct zwp_linux_buffer_params_v1 *p,
		struct wl_buffer *buf) {
	struct wl_buffer **out = data;
	*out = buf;
	zwp_linux_buffer_params_v1_destroy(p);
}
static void params_failed(void *data, struct zwp_linux_buffer_params_v1 *p) {
	struct wl_buffer **out = data;
	*out = NULL;
	zwp_linux_buffer_params_v1_destroy(p);
}
static const struct zwp_linux_buffer_params_v1_listener params_listener = {
	.created = params_created, .failed = params_failed,
};

/*
 * NO wl_buffer.release LISTENER, ON PURPOSE.
 *
 * Under explicit synchronisation the release point replaces it, and a client
 * that quietly falls back to wl_buffer.release would hide a compositor that
 * never signals the timeline -- which is the single most likely way to get
 * this wrong and the thing this file is built to catch.
 */

static bool make_buffer(struct sbuf *b, int w, int h, uint32_t fmt) {
	b->bo = gbm_bo_create(A.gbm, (uint32_t)w, (uint32_t)h, fmt,
		GBM_BO_USE_LINEAR | GBM_BO_USE_RENDERING);
	if (b->bo == NULL) {
		fprintf(stderr, "wlsync: gbm_bo_create failed\n");
		return false;
	}
	int fd = gbm_bo_get_fd(b->bo);
	if (fd < 0) {
		fprintf(stderr, "wlsync: gbm_bo_get_fd failed\n");
		return false;
	}
	b->stride = gbm_bo_get_stride(b->bo);
	uint64_t mod = gbm_bo_get_modifier(b->bo);

	struct zwp_linux_buffer_params_v1 *p =
		zwp_linux_dmabuf_v1_create_params(A.dmabuf);
	zwp_linux_buffer_params_v1_add(p, fd, 0, gbm_bo_get_offset(b->bo, 0),
		b->stride, (uint32_t)(mod >> 32), (uint32_t)(mod & 0xffffffff));
	zwp_linux_buffer_params_v1_add_listener(p, &params_listener, &b->wl);
	zwp_linux_buffer_params_v1_create(p, w, h, fmt, 0);
	wl_display_roundtrip(A.dpy);
	close(fd);
	if (b->wl == NULL) {
		fprintf(stderr, "wlsync: the compositor refused a LINEAR dma-buf; it "
			"advertised a format set it cannot import\n");
		return false;
	}
	return true;
}

static void fill(struct sbuf *b, int w, int h, uint32_t argb) {
	uint32_t stride = 0;
	void *map = gbm_bo_map(b->bo, 0, 0, (uint32_t)w, (uint32_t)h,
		GBM_BO_TRANSFER_WRITE, &stride, &b->map_data);
	if (map == NULL) {
		return;
	}
	for (int y = 0; y < h; y++) {
		uint32_t *row = (uint32_t *)((char *)map + (size_t)y * stride);
		for (int x = 0; x < w; x++) {
			row[x] = argb;
		}
	}
	gbm_bo_unmap(b->bo, b->map_data);
}

/* ── main ─────────────────────────────────────────────────────────────── */

static void usage(void) {
	fprintf(stderr,
		"usage: wlsync [--frames N] [--size WxH] [--buffers N] [--hold-ms N]\n"
		"              [--acquire-delay-ms N] [--no-release-point]\n"
		"              [--no-acquire-point] [--device PATH] [--quiet]\n");
}

int main(int argc, char **argv) {
	int frames = 120, w = 320, h = 240, nbuf = 2, hold_ms = 8;
	int acquire_delay_ms = 0;
	bool no_release = false, no_acquire = false, quiet = false;
	const char *device = "/dev/dri/renderD128";

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
			frames = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
			if (sscanf(argv[++i], "%dx%d", &w, &h) != 2) { usage(); return 1; }
		} else if (strcmp(argv[i], "--buffers") == 0 && i + 1 < argc) {
			nbuf = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--hold-ms") == 0 && i + 1 < argc) {
			hold_ms = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--acquire-delay-ms") == 0 && i + 1 < argc) {
			acquire_delay_ms = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--no-release-point") == 0) {
			no_release = true;
		} else if (strcmp(argv[i], "--no-acquire-point") == 0) {
			no_acquire = true;
		} else if (strcmp(argv[i], "--device") == 0 && i + 1 < argc) {
			device = argv[++i];
		} else if (strcmp(argv[i], "--quiet") == 0) {
			quiet = true;
		} else {
			usage();
			return 1;
		}
	}
	if (nbuf < 1 || nbuf > MAX_BUFFERS) {
		usage();
		return 1;
	}

	A.dpy = wl_display_connect(NULL);
	if (A.dpy == NULL) {
		fprintf(stderr, "wlsync: no Wayland display\n");
		return 1;
	}
	A.reg = wl_display_get_registry(A.dpy);
	wl_registry_add_listener(A.reg, &reg_listener, NULL);
	wl_display_roundtrip(A.dpy);

	if (A.comp == NULL || A.wm == NULL || A.dmabuf == NULL) {
		fprintf(stderr, "wlsync: missing wl_compositor / xdg_wm_base / "
			"zwp_linux_dmabuf_v1\n");
		return 1;
	}
	/*
	 * THE PREMISE OF EVERY TEST THAT USES THIS CLIENT.
	 *
	 * If the compositor does not offer the global there is nothing here to
	 * exercise, and a test built on top of this must fail rather than pass
	 * quietly. Announced on stdout so the harness can assert it saw it.
	 */
	if (A.syncmgr == NULL) {
		printf("wlsync: no wp_linux_drm_syncobj_manager_v1 global\n");
		fflush(stdout);
		fprintf(stderr, "wlsync: the compositor does not advertise "
			"wp_linux_drm_syncobj_manager_v1; nothing to test\n");
		return 1;
	}
	printf("wlsync: wp_linux_drm_syncobj_manager_v1 present\n");

	A.drm_fd = open(device, O_RDWR | O_CLOEXEC);
	if (A.drm_fd < 0) {
		die(device);
	}
	A.gbm = gbm_create_device(A.drm_fd);
	if (A.gbm == NULL) {
		die("gbm_create_device");
	}
	if (drmSyncobjCreate(A.drm_fd, 0, &A.syncobj) != 0) {
		die("drmSyncobjCreate");
	}
	int sfd = -1;
	if (drmSyncobjHandleToFD(A.drm_fd, A.syncobj, &sfd) != 0 || sfd < 0) {
		die("drmSyncobjHandleToFD");
	}

	A.surf = wl_compositor_create_surface(A.comp);
	A.xsurf = xdg_wm_base_get_xdg_surface(A.wm, A.surf);
	xdg_surface_add_listener(A.xsurf, &xsurf_listener, NULL);
	A.top = xdg_surface_get_toplevel(A.xsurf);
	xdg_toplevel_add_listener(A.top, &top_listener, NULL);
	xdg_toplevel_set_app_id(A.top, "wlsync");
	xdg_toplevel_set_title(A.top, "wlsync");
	wl_surface_commit(A.surf);
	wl_display_roundtrip(A.dpy);
	while (!A.configured && wl_display_dispatch(A.dpy) != -1) {
	}

	struct wp_linux_drm_syncobj_timeline_v1 *tl =
		wp_linux_drm_syncobj_manager_v1_import_timeline(A.syncmgr, sfd);
	close(sfd);
	A.ssurf = wp_linux_drm_syncobj_manager_v1_get_surface(A.syncmgr, A.surf);

	struct sbuf bufs[MAX_BUFFERS] = {0};
	for (int i = 0; i < nbuf; i++) {
		if (!make_buffer(&bufs[i], w, h, GBM_FORMAT_ARGB8888)) {
			return 1;
		}
	}

	A.running = true;
	uint64_t point = 0;
	int cur = 0;

	for (int f = 0; f < frames && A.running; f++) {
		struct sbuf *b = &bufs[cur];

		/*
		 * WAIT FOR OUR OWN RELEASE POINT before touching the buffer again.
		 *
		 * This is the liveness half of the contract, and it is the reason
		 * this loop cannot be replaced by "commit in a tight loop": a
		 * compositor that never signals stops here, which is precisely what
		 * it would do to gamescope.
		 */
		if (b->pending_release != 0) {
			uint32_t first = 0;
			uint64_t t0 = now_us();
			/* the deadline is an absolute CLOCK_MONOTONIC nanosecond count,
			 * not a duration -- getting that wrong reads as an instant
			 * timeout and would make this loop report a hang that is its own */
			struct timespec ts;
			clock_gettime(CLOCK_MONOTONIC, &ts);
			int64_t deadline = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec +
				3000000000LL;
			int r = drmSyncobjTimelineWait(A.drm_fd, &A.syncobj,
				&b->pending_release, 1, deadline,
				DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL, &first);
			if (r != 0) {
				printf("wlsync: commits=%" PRIu64 " releases=%" PRIu64
					" release_us_max=%" PRIu64 " release_us_avg=%" PRIu64
					" proto_err=%s\n",
					A.commits, A.releases, A.release_us_max,
					A.releases ? A.release_us_total / A.releases : 0,
					A.proto_err ? A.proto_err : "none");
				fprintf(stderr, "wlsync: the compositor never signalled "
					"release point %" PRIu64 " -- an explicit-sync client "
					"hangs here\n", b->pending_release);
				return 2;
			}
			uint64_t us = now_us() - t0;
			A.releases++;
			A.release_us_total += us + (t0 - b->committed_at_us);
			uint64_t total = now_us() - b->committed_at_us;
			if (total > A.release_us_max) {
				A.release_us_max = total;
			}
			b->pending_release = 0;
		}

		fill(b, w, h, 0xff000000u | (uint32_t)((f * 8) & 0xff) << 16 |
			(uint32_t)((f * 3) & 0xff) << 8 | (uint32_t)((f * 5) & 0xff));

		uint64_t acquire = ++point;
		uint64_t release = ++point;

		if (acquire_delay_ms <= 0 && !no_acquire) {
			/* signalled BEFORE the commit: the ordinary case, and the only
			 * one where the compositor may legally sample immediately */
			drmSyncobjTimelineSignal(A.drm_fd, &A.syncobj, &acquire, 1);
		}

		wl_surface_attach(A.surf, b->wl, 0, 0);
		wl_surface_damage_buffer(A.surf, 0, 0, w, h);
		if (!no_acquire) {
			wp_linux_drm_syncobj_surface_v1_set_acquire_point(A.ssurf, tl,
				(uint32_t)(acquire >> 32), (uint32_t)acquire);
		}
		if (!no_release) {
			wp_linux_drm_syncobj_surface_v1_set_release_point(A.ssurf, tl,
				(uint32_t)(release >> 32), (uint32_t)release);
			b->pending_release = release;
		}
		b->committed_at_us = now_us();
		wl_surface_commit(A.surf);
		A.commits++;

		if (wl_display_flush(A.dpy) < 0) {
			break;
		}
		if (acquire_delay_ms > 0 && !no_acquire) {
			struct timespec ts = {
				.tv_sec = acquire_delay_ms / 1000,
				.tv_nsec = (long)(acquire_delay_ms % 1000) * 1000000L,
			};
			nanosleep(&ts, NULL);
			drmSyncobjTimelineSignal(A.drm_fd, &A.syncobj, &acquire, 1);
			wl_display_flush(A.dpy);
		}
		if (hold_ms > 0) {
			struct timespec ts = {
				.tv_sec = hold_ms / 1000,
				.tv_nsec = (long)(hold_ms % 1000) * 1000000L,
			};
			nanosleep(&ts, NULL);
		}

		/* drain without blocking: a protocol error arrives here */
		while (wl_display_prepare_read(A.dpy) != 0) {
			wl_display_dispatch_pending(A.dpy);
		}
		wl_display_flush(A.dpy);
		wl_display_read_events(A.dpy);
		if (wl_display_dispatch_pending(A.dpy) < 0) {
			int err = wl_display_get_error(A.dpy);
			uint32_t code = 0;
			const struct wl_interface *iface = NULL;
			uint32_t id = 0;
			if (err == EPROTO) {
				code = wl_display_get_protocol_error(A.dpy, &iface, &id);
				static char buf[128];
				snprintf(buf, sizeof(buf), "%s.%u(obj %u)",
					iface ? iface->name : "?", code, id);
				A.proto_err = buf;
			} else {
				A.proto_err = "disconnect";
			}
			break;
		}
		cur = (cur + 1) % nbuf;
	}

	printf("wlsync: commits=%" PRIu64 " releases=%" PRIu64
		" release_us_max=%" PRIu64 " release_us_avg=%" PRIu64
		" proto_err=%s\n",
		A.commits, A.releases, A.release_us_max,
		A.releases ? A.release_us_total / A.releases : 0,
		A.proto_err ? A.proto_err : "none");
	fflush(stdout);

	if (no_release || no_acquire) {
		/* the run asked to be disconnected */
		if (A.proto_err == NULL) {
			fprintf(stderr, "wlsync: expected a protocol error and got "
				"none -- the compositor is not validating\n");
			return 4;
		}
		if (!quiet) {
			fprintf(stderr, "wlsync: got the expected protocol error: %s\n",
				A.proto_err);
		}
		return 0;
	}
	if (A.proto_err != NULL) {
		fprintf(stderr, "wlsync: unexpected protocol error: %s\n",
			A.proto_err);
		return 3;
	}
	if (A.releases == 0 && A.commits > 1) {
		fprintf(stderr, "wlsync: not one release point came back\n");
		return 2;
	}
	return 0;
}
