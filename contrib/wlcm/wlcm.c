/*
 * wlcm -- ask the compositor what image description it prefers for a surface,
 * and print the answer.
 *
 * ── WHY THIS EXISTS ──────────────────────────────────────────────────────
 *
 * M6B/D6 wires `wlr_color_manager_v1_set_surface_preferred_image_description`,
 * which had no callers anywhere in the tree. The consequence was invisible
 * from inside the compositor: every wp-cm client asking DP-1 what it would
 * like was told the compositor default -- SDR -- and correctly tone-mapped its
 * HDR content down to meet it. Nothing errors and nothing logs; the picture is
 * merely wrong on the one content type HDR exists for.
 *
 * So the assertion has to be made FROM THE CLIENT SIDE. There is no way to ask
 * the compositor "what did you tell that surface": the answer lives in the
 * protocol object, and only a client holds one.
 *
 * ── WHAT IT PRINTS ───────────────────────────────────────────────────────
 *
 *     preferred: tf=<n> primaries=<n> minlum=<n> maxlum=<n> reflum=<n>
 *                maxcll=<n> maxfall=<n>
 *
 * or `preferred: none` when the compositor never sent one -- which is the
 * pre-D6 behaviour and what AZ_BREAK_CM_NO_PREFERRED must restore.
 *
 * THE TWO ARE DIFFERENT ANSWERS, deliberately. "the compositor set a
 * description and it was SDR" and "the compositor never set one" are the same
 * picture and completely different bugs, so a fixture must be able to tell them
 * apart. `none` means no preferred_changed event arrived at all.
 *
 * xdg-shell rather than layer-shell: a preferred description is a property of
 * the OUTPUT the surface is on, and an ordinary toplevel is what a video player
 * is. It maps, waits for the compositor to place it, then reads.
 */
#define _POSIX_C_SOURCE 200809L

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include "color-management-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"
/*
 * FROG TOO, in the same client and on the SAME surface.
 *
 * The two protocols must agree about which display a surface is on -- that is
 * the whole point of asteroidz's shared preferred-colour policy -- and the only
 * way to catch them disagreeing is to hold both objects for one wl_surface and
 * compare what each was told. Two separate clients could always be explained
 * away as having landed on different outputs.
 */
#include "frog-color-management-v1-client-protocol.h"

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct frog_color_management_factory_v1 *frog;
static struct frog_color_managed_surface *frog_surf;
static int frog_events;
static struct xdg_wm_base *wm_base;
static struct wp_color_manager_v1 *cm;
static struct wl_surface *surface;
static struct wp_color_management_surface_feedback_v1 *feedback;

static int dbg_ready;
static int got_preferred;   /* a preferred_changed event arrived */
static int info_done;       /* the description's info stream completed */
static int configured;

static uint32_t v_tf, v_prim, v_minlum, v_maxlum, v_reflum;
static uint32_t v_maxcll, v_maxfall;
static int have_tf, have_prim, have_lum, have_cll, have_fall;

/* ── xdg-shell: the minimum to get a mapped, configured toplevel ───────── */

static void wm_base_ping(void *d, struct xdg_wm_base *b, uint32_t serial) {
	(void)d;
	xdg_wm_base_pong(b, serial);
}
static const struct xdg_wm_base_listener wm_base_listener = {
	.ping = wm_base_ping,
};

static void surf_configure(void *d, struct xdg_surface *s, uint32_t serial) {
	(void)d;
	xdg_surface_ack_configure(s, serial);
	configured = 1;
}
static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = surf_configure,
};

static void top_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h,
		struct wl_array *states) {
	(void)d; (void)t; (void)w; (void)h; (void)states;
}
static void top_close(void *d, struct xdg_toplevel *t) { (void)d; (void)t; }
static const struct xdg_toplevel_listener xdg_toplevel_listener = {
	.configure = top_configure,
	.close = top_close,
};

/* ── the image description's own information stream ────────────────────── */

static void info_done_cb(void *d, struct wp_image_description_info_v1 *i) {
	(void)d;
	wp_image_description_info_v1_destroy(i);
	info_done = 1;
}
static void info_icc(void *d, struct wp_image_description_info_v1 *i,
		int32_t fd, uint32_t size) {
	(void)d; (void)i; (void)size;
	close(fd);
}
static void info_primaries(void *d, struct wp_image_description_info_v1 *i,
		int32_t rx, int32_t ry, int32_t gx, int32_t gy, int32_t bx, int32_t by,
		int32_t wx, int32_t wy) {
	(void)d; (void)i; (void)rx; (void)ry; (void)gx; (void)gy;
	(void)bx; (void)by; (void)wx; (void)wy;
}
static void info_primaries_named(void *d, struct wp_image_description_info_v1 *i,
		uint32_t p) {
	(void)d; (void)i;
	v_prim = p;
	have_prim = 1;
}
static void info_tf_power(void *d, struct wp_image_description_info_v1 *i,
		uint32_t e) {
	(void)d; (void)i; (void)e;
}
static void info_tf_named(void *d, struct wp_image_description_info_v1 *i,
		uint32_t tf) {
	(void)d; (void)i;
	v_tf = tf;
	have_tf = 1;
}
static void info_luminances(void *d, struct wp_image_description_info_v1 *i,
		uint32_t minl, uint32_t maxl, uint32_t refl) {
	(void)d; (void)i;
	v_minlum = minl; v_maxlum = maxl; v_reflum = refl;
	have_lum = 1;
}
static void info_target_primaries(void *d, struct wp_image_description_info_v1 *i,
		int32_t rx, int32_t ry, int32_t gx, int32_t gy, int32_t bx, int32_t by,
		int32_t wx, int32_t wy) {
	(void)d; (void)i; (void)rx; (void)ry; (void)gx; (void)gy;
	(void)bx; (void)by; (void)wx; (void)wy;
}
static void info_target_luminance(void *d, struct wp_image_description_info_v1 *i,
		uint32_t minl, uint32_t maxl) {
	(void)d; (void)i; (void)minl; (void)maxl;
}
static void info_target_max_cll(void *d, struct wp_image_description_info_v1 *i,
		uint32_t v) {
	(void)d; (void)i;
	v_maxcll = v;
	have_cll = 1;
}
static void info_target_max_fall(void *d, struct wp_image_description_info_v1 *i,
		uint32_t v) {
	(void)d; (void)i;
	v_maxfall = v;
	have_fall = 1;
}
static const struct wp_image_description_info_v1_listener info_listener = {
	.done = info_done_cb,
	.icc_file = info_icc,
	.primaries = info_primaries,
	.primaries_named = info_primaries_named,
	.tf_power = info_tf_power,
	.tf_named = info_tf_named,
	.luminances = info_luminances,
	.target_primaries = info_target_primaries,
	.target_luminance = info_target_luminance,
	.target_max_cll = info_target_max_cll,
	.target_max_fall = info_target_max_fall,
};

/* ── the description object itself ─────────────────────────────────────── */

static void desc_failed(void *d, struct wp_image_description_v1 *i,
		uint32_t cause, const char *msg) {
	(void)d; (void)i;
	fprintf(stderr, "wlcm: description failed: cause=%u %s\n", cause,
		msg != NULL ? msg : "");
	info_done = 1;
}
static void desc_ready(void *d, struct wp_image_description_v1 *i,
		uint32_t identity) {
	(void)d; (void)identity;
	dbg_ready = 1;
	struct wp_image_description_info_v1 *info =
		wp_image_description_v1_get_information(i);
	wp_image_description_info_v1_add_listener(info, &info_listener, NULL);
}
/* v2 replaces `ready` with `ready2`, splitting the 64-bit identity across two
 * words. BOTH entries must be filled: libwayland aborts the client on a NULL
 * listener function for an event the compositor actually sends, so an omitted
 * one is not "an event ignored" but a dead client -- observed, as an empty
 * output line and `listener function for opcode 2 ... is NULL` on stderr. */
static void desc_ready2(void *d, struct wp_image_description_v1 *i,
		uint32_t hi, uint32_t lo) {
	(void)hi; (void)lo;
	desc_ready(d, i, 0);
}
static const struct wp_image_description_v1_listener desc_listener = {
	.failed = desc_failed,
	.ready = desc_ready,
	.ready2 = desc_ready2,
};

/* ── the feedback object ───────────────────────────────────────────────── */

static void fb_preferred_changed(void *d,
		struct wp_color_management_surface_feedback_v1 *f, uint32_t identity) {
	(void)d; (void)f; (void)identity;
	got_preferred = 1;
}
/* v2 splits the identity across two words -- a 64-bit id in a protocol with
 * 32-bit integers. Neither half is read here; only that the event arrived. */
static void fb_preferred_changed2(void *d,
		struct wp_color_management_surface_feedback_v1 *f, uint32_t hi,
		uint32_t lo) {
	(void)d; (void)f; (void)hi; (void)lo;
	got_preferred = 1;
}
static const struct wp_color_management_surface_feedback_v1_listener fb_listener = {
	.preferred_changed = fb_preferred_changed,
	.preferred_changed2 = fb_preferred_changed2,
};

/* ── registry ──────────────────────────────────────────────────────────── */

static void reg_global(void *d, struct wl_registry *r, uint32_t name,
		const char *iface, uint32_t version) {
	(void)d;
	if (strcmp(iface, wl_compositor_interface.name) == 0) {
		compositor = wl_registry_bind(r, name, &wl_compositor_interface, 4);
	} else if (strcmp(iface, wl_shm_interface.name) == 0) {
		shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
	} else if (strcmp(iface, xdg_wm_base_interface.name) == 0) {
		wm_base = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
		xdg_wm_base_add_listener(wm_base, &wm_base_listener, NULL);
	} else if (strcmp(iface, frog_color_management_factory_v1_interface.name) == 0) {
		frog = wl_registry_bind(r, name,
			&frog_color_management_factory_v1_interface, 1);
	} else if (strcmp(iface, wp_color_manager_v1_interface.name) == 0) {
		uint32_t v = version < 2 ? version : 2;
		cm = wl_registry_bind(r, name, &wp_color_manager_v1_interface, v);
	}
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t name) {
	(void)d; (void)r; (void)name;
}
static const struct wl_registry_listener reg_listener = {
	.global = reg_global,
	.global_remove = reg_remove,
};

/* One 64x64 ARGB buffer in an anonymous shm file. The smallest thing that
 * makes a surface real. */
static struct wl_buffer *create_buffer(int w, int h) {
	int stride = w * 4;
	int size = stride * h;
	char name[] = "/wlcm-XXXXXX";
	int fd = -1;
	for (int tries = 0; tries < 16 && fd < 0; tries++) {
		for (int i = 6; i < 12; i++) {
			name[i] = 'a' + (rand() % 26);
		}
		fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
	}
	if (fd < 0) {
		return NULL;
	}
	shm_unlink(name);
	if (ftruncate(fd, size) < 0) {
		close(fd);
		return NULL;
	}
	void *px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (px == MAP_FAILED) {
		close(fd);
		return NULL;
	}
	memset(px, 0x40, size);
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
	struct wl_buffer *b = wl_shm_pool_create_buffer(pool, 0, w, h, stride,
		WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	munmap(px, size);
	close(fd);
	return b;
}

/* Every event, numbered and printed as it arrives. A transition oracle needs to
 * distinguish "the metadata is right" from "nothing was ever sent and the
 * client is reporting its own initial state", and from "it was re-sent five
 * times when nothing changed". Only a running count can do that. */
static void frog_preferred(void *d, struct frog_color_managed_surface *s,
		uint32_t tf, uint32_t rx, uint32_t ry, uint32_t gx, uint32_t gy,
		uint32_t bx, uint32_t by, uint32_t wx, uint32_t wy,
		uint32_t max_lum, uint32_t min_lum, uint32_t max_fall) {
	(void)d; (void)s;
	frog_events++;
	printf("frog[%d]: tf=%u primaries=%u,%u/%u,%u/%u,%u white=%u,%u "
		"maxlum=%u minlum=%u maxfall=%u\n", frog_events, tf,
		rx, ry, gx, gy, bx, by, wx, wy, max_lum, min_lum, max_fall);
	fflush(stdout);
}
static const struct frog_color_managed_surface_listener frog_listener = {
	.preferred_metadata = frog_preferred,
};

static long now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int main(int argc, char **argv) {
	int wait_ms = argc > 1 ? atoi(argv[1]) : 3000;

	struct wl_display *dpy = wl_display_connect(NULL);
	if (dpy == NULL) {
		fprintf(stderr, "wlcm: cannot connect to a Wayland display\n");
		return 1;
	}
	struct wl_registry *reg = wl_display_get_registry(dpy);
	wl_registry_add_listener(reg, &reg_listener, NULL);
	wl_display_roundtrip(dpy);

	if (compositor == NULL || wm_base == NULL || shm == NULL) {
		fprintf(stderr, "wlcm: no wl_compositor / xdg_wm_base / wl_shm\n");
		return 1;
	}
	if (cm == NULL) {
		/* NOT the same as "no preferred description". The compositor does not
		 * implement the protocol at all, and a fixture must not read that as a
		 * D6 failure. */
		printf("preferred: no-protocol\n");
		return 0;
	}

	surface = wl_compositor_create_surface(compositor);
	struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(wm_base, surface);
	xdg_surface_add_listener(xs, &xdg_surface_listener, NULL);
	struct xdg_toplevel *top = xdg_surface_get_toplevel(xs);
	xdg_toplevel_add_listener(top, &xdg_toplevel_listener, NULL);
	xdg_toplevel_set_title(top, "wlcm");
	/* Both contrib clients that map reliably set one; asteroidz's window rules
	 * key on app_id and a surface without one is an unusual shape to hand a
	 * compositor. */
	xdg_toplevel_set_app_id(top, "wlcm");
	/*
	 * FEEDBACK BEFORE THE FIRST COMMIT. The compositor sends the initial
	 * preferred description when the surface becomes known to it, and a
	 * feedback object created afterwards would miss that first event and read
	 * as "never told" -- a fixture failing on its own ordering rather than on
	 * the compositor.
	 */
	if (frog != NULL) {
		frog_surf = frog_color_management_factory_v1_get_color_managed_surface(
			frog, surface);
		frog_color_managed_surface_add_listener(frog_surf, &frog_listener,
			NULL);
	}
	feedback = wp_color_manager_v1_get_surface_feedback(cm, surface);
	wp_color_management_surface_feedback_v1_add_listener(feedback, &fb_listener,
		NULL);
	wl_surface_commit(surface);

	/*
	 * WAIT FOR THE INITIAL CONFIGURE, AND ACK IT, BEFORE ATTACHING.
	 *
	 * xdg-shell requires commit-with-no-buffer -> configure -> ack -> attach.
	 * A single roundtrip is not enough: if the configure has not arrived, the
	 * attach happens out of sequence and the toplevel NEVER MAPS. The
	 * compositor then has no Client for it, the surface is on no output, and
	 * both colour protocols correctly say nothing -- which reads exactly like
	 * a compositor that forgot to send, and cost an hour of looking in the
	 * wrong place.
	 */
	/* wl_display_dispatch, blocking on the fd -- the same construct every
	 * client in contrib/ that maps reliably uses. A polling loop of roundtrips
	 * was not equivalent here: the toplevel never mapped, the compositor never
	 * made a Client for it, and both colour protocols then correctly described
	 * nothing. */
	while (!configured && wl_display_dispatch(dpy) != -1) {
		;
	}
	if (!configured) {
		fprintf(stderr, "wlcm: no xdg_surface.configure arrived\n");
		return 1;
	}

	/*
	 * A REAL BUFFER, because a surface with none NEVER MAPS -- and an unmapped
	 * surface is not a window, gets no output, and is therefore told nothing.
	 * The first version of this committed an empty surface and reported
	 * `preferred: none` against a correctly wired compositor, which is a
	 * fixture failing on its own client rather than on the thing under test.
	 * Nobody here looks at the pixels; what matters is that the surface exists
	 * on an output.
	 */
	struct wl_buffer *buf = create_buffer(64, 64);
	if (buf == NULL) {
		fprintf(stderr, "wlcm: could not create a buffer\n");
		return 1;
	}
	wl_surface_attach(surface, buf, 0, 0);
	wl_surface_damage_buffer(surface, 0, 0, 64, 64);
	wl_surface_commit(surface);
	wl_display_roundtrip(dpy);

	long deadline = now_ms() + wait_ms;
	while (now_ms() < deadline && !got_preferred) {
		if (wl_display_dispatch_pending(dpy) < 0) {
			break;
		}
		wl_display_flush(dpy);
		wl_display_roundtrip(dpy);
		struct timespec ts = {0, 50 * 1000000};
		nanosleep(&ts, NULL);
	}

	/*
	 * WATCH MODE: argv[2] = seconds to stay alive after the first read, so a
	 * fixture can toggle HDR or move the window and see whether the SAME
	 * object is updated. A client that has to reconnect to learn the new state
	 * proves nothing about re-sending.
	 */
	int watch = argc > 2 ? atoi(argv[2]) : 0;

	if (!got_preferred) {
		printf("preferred: none\n");
		if (watch > 0) {
			long end = now_ms() + watch * 1000;
			while (now_ms() < end) {
				if (wl_display_roundtrip(dpy) < 0) { break; }
				struct timespec ts = {0, 100 * 1000000};
				nanosleep(&ts, NULL);
			}
			printf("frog events: %d\n", frog_events);
		}
		return 0;
	}

	struct wp_image_description_v1 *desc =
		wp_color_management_surface_feedback_v1_get_preferred_parametric(
			feedback);
	wp_image_description_v1_add_listener(desc, &desc_listener, NULL);

	deadline = now_ms() + wait_ms;
	while (now_ms() < deadline && !info_done) {
		wl_display_roundtrip(dpy);
		struct timespec ts = {0, 50 * 1000000};
		nanosleep(&ts, NULL);
	}

	/*
	 * ONE LINE, with an explicit `set` marker and a `have` mask beside the
	 * values. A zero in `tf` could mean "the compositor said 0" or "no tf_named
	 * event arrived", and those are different findings -- the mask is what lets
	 * a fixture assert the second without inferring it from the first.
	 */
	if (getenv("WLCM_DEBUG") != NULL) {
		fprintf(stderr, "wlcm: got_preferred=%d ready=%d info_done=%d\n",
			got_preferred, dbg_ready, info_done);
	}
	printf("preferred: set tf=%u primaries=%u minlum=%u maxlum=%u reflum=%u "
		"maxcll=%u maxfall=%u have=%d%d%d%d%d\n",
		v_tf, v_prim, v_minlum, v_maxlum, v_reflum, v_maxcll, v_maxfall,
		have_tf, have_prim, have_lum, have_cll, have_fall);
	return 0;
}
