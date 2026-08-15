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
/*
 * M6B/D6. A FROG OBSERVER ON A SURFACE THAT DEMONSTRABLY MAPS.
 *
 * The colour-management fixtures need a client whose surface really becomes a
 * window on a real output -- a preferred-metadata assertion made against an
 * unmapped surface proves nothing, because an unmapped surface is on no output
 * and the compositor correctly says nothing about it. This client already maps
 * reliably and is already trusted by the blur fixtures, so it observes frog
 * rather than a second client being written for the purpose.
 *
 * Observation only: one object, one listener, one line per event. Nothing here
 * changes what this client renders.
 */
#include "frog-color-management-v1-client-protocol.h"
/*
 * AND A wp-color-management-v1 OBSERVER, BESIDE THE FROG ONE.
 *
 * The two protocols are serialized from ONE policy (src/render/az_preferred.h),
 * so a fixture that reads only one of them cannot tell a correct policy with a
 * broken serializer from a broken policy with a correct one. Both observers
 * live in this client for the same reason the frog one does: its surface
 * demonstrably maps, and an unmapped surface is on no output.
 *
 * This one exists specifically to catch a SECOND WRITER. scenefx has a
 * preferred-description writer of its own whose policy is "max preference
 * across every output the surface touches", against az_preferred's "the
 * surface's own output" -- and the two only disagree for a surface that
 * touches two outputs in different colour states. Nothing inside the
 * compositor can see which of them reached the client; only the client can.
 */
#include "color-management-v1-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"

static struct wl_compositor *compositor = NULL;
static struct wl_shm *shm = NULL;
static struct frog_color_management_factory_v1 *frog_factory = NULL;
static struct frog_color_managed_surface *frog_surface = NULL;
static int frog_events = 0;
static struct wp_color_manager_v1 *cm_manager = NULL;
static struct wp_color_management_surface_feedback_v1 *cm_feedback = NULL;
static int cm_events = 0;

/* One line per preferred_metadata event, numbered. The NUMBER is half the
 * assertion: "the metadata is correct" and "nothing was ever sent" carry the
 * same values, and only a count separates them. */
static void frog_preferred_metadata(void *data,
		struct frog_color_managed_surface *s, uint32_t tf,
		uint32_t rx, uint32_t ry, uint32_t gx, uint32_t gy,
		uint32_t bx, uint32_t by, uint32_t wx, uint32_t wy,
		uint32_t max_lum, uint32_t min_lum, uint32_t max_fall) {
	(void)data; (void)s;
	frog_events++;
	printf("frog[%d]: tf=%u primaries=%u,%u/%u,%u/%u,%u white=%u,%u "
		"maxlum=%u minlum=%u maxfall=%u\n", frog_events, tf,
		rx, ry, gx, gy, bx, by, wx, wy, max_lum, min_lum, max_fall);
	fflush(stdout);
}
/*
 * The description's own values, one line per read. `tf` and `primaries` are the
 * two the compositor can currently put on this wire; the luminances are printed
 * too so that the day they start arriving, the fixture reading this does not
 * have to change to notice.
 */
static uint32_t cm_tf = 0, cm_prim = 0;
static void cm_info_tf_named(void *d, struct wp_image_description_info_v1 *i,
		uint32_t tf) { (void)d; (void)i; cm_tf = tf; }
static void cm_info_primaries_named(void *d,
		struct wp_image_description_info_v1 *i, uint32_t p) {
	(void)d; (void)i; cm_prim = p;
}
static void cm_info_done(void *d, struct wp_image_description_info_v1 *i) {
	(void)d;
	cm_events++;
	printf("wpcm[%d]: tf=%u primaries=%u\n", cm_events, cm_tf, cm_prim);
	fflush(stdout);
	wp_image_description_info_v1_destroy(i);
}
static void cm_info_icc(void *d, struct wp_image_description_info_v1 *i,
		int32_t fd, uint32_t size) { (void)d; (void)i; (void)size; close(fd); }
static void cm_info_primaries(void *d, struct wp_image_description_info_v1 *i,
		int32_t rx, int32_t ry, int32_t gx, int32_t gy, int32_t bx, int32_t by,
		int32_t wx, int32_t wy) {
	(void)d; (void)i; (void)rx; (void)ry; (void)gx; (void)gy;
	(void)bx; (void)by; (void)wx; (void)wy;
}
static void cm_info_tf_power(void *d, struct wp_image_description_info_v1 *i,
		uint32_t e) { (void)d; (void)i; (void)e; }
static void cm_info_luminances(void *d, struct wp_image_description_info_v1 *i,
		uint32_t min, uint32_t max, uint32_t ref) {
	(void)d; (void)i; (void)min; (void)max; (void)ref;
}
static void cm_info_target_primaries(void *d,
		struct wp_image_description_info_v1 *i, int32_t rx, int32_t ry,
		int32_t gx, int32_t gy, int32_t bx, int32_t by, int32_t wx, int32_t wy) {
	(void)d; (void)i; (void)rx; (void)ry; (void)gx; (void)gy;
	(void)bx; (void)by; (void)wx; (void)wy;
}
static void cm_info_target_luminance(void *d,
		struct wp_image_description_info_v1 *i, uint32_t min, uint32_t max) {
	(void)d; (void)i; (void)min; (void)max;
}
static void cm_info_target_max_cll(void *d,
		struct wp_image_description_info_v1 *i, uint32_t v) {
	(void)d; (void)i; (void)v;
}
static void cm_info_target_max_fall(void *d,
		struct wp_image_description_info_v1 *i, uint32_t v) {
	(void)d; (void)i; (void)v;
}
static const struct wp_image_description_info_v1_listener cm_info_listener = {
	.done = cm_info_done,
	.icc_file = cm_info_icc,
	.primaries = cm_info_primaries,
	.primaries_named = cm_info_primaries_named,
	.tf_power = cm_info_tf_power,
	.tf_named = cm_info_tf_named,
	.luminances = cm_info_luminances,
	.target_primaries = cm_info_target_primaries,
	.target_luminance = cm_info_target_luminance,
	.target_max_cll = cm_info_target_max_cll,
	.target_max_fall = cm_info_target_max_fall,
};

static void cm_mgr_intent(void *d, struct wp_color_manager_v1 *m, uint32_t v) {
	(void)d; (void)m; printf("cmcap intent %u\n", v);
}
static void cm_mgr_feature(void *d, struct wp_color_manager_v1 *m, uint32_t v) {
	(void)d; (void)m; printf("cmcap feature %u\n", v);
}
static void cm_mgr_tf(void *d, struct wp_color_manager_v1 *m, uint32_t v) {
	(void)d; (void)m; printf("cmcap tf %u\n", v);
}
static void cm_mgr_prim(void *d, struct wp_color_manager_v1 *m, uint32_t v) {
	(void)d; (void)m; printf("cmcap primaries %u\n", v);
}
static void cm_mgr_done(void *d, struct wp_color_manager_v1 *m) {
	(void)d; (void)m; printf("cmcap done\n"); fflush(stdout);
}
static const struct wp_color_manager_v1_listener cm_mgr_listener = {
	.supported_intent = cm_mgr_intent,
	.supported_feature = cm_mgr_feature,
	.supported_tf_named = cm_mgr_tf,
	.supported_primaries_named = cm_mgr_prim,
	.done = cm_mgr_done,
};

static void cm_desc_ready(void *d, struct wp_image_description_v1 *desc,
		uint32_t identity) {
	(void)d; (void)identity;
	struct wp_image_description_info_v1 *info =
		wp_image_description_v1_get_information(desc);
	wp_image_description_info_v1_add_listener(info, &cm_info_listener, NULL);
}
static void cm_desc_failed(void *d, struct wp_image_description_v1 *desc,
		uint32_t cause, const char *msg) {
	(void)d; (void)desc; (void)cause;
	printf("wpcm_failed: %s\n", msg ? msg : "?");
	fflush(stdout);
}
/* v2 replaced `ready` with `ready2`. BOTH must be in the listener: libwayland
 * dispatches by opcode and aborts the client on a NULL slot for an event the
 * bound version can deliver -- "listener function for opcode 2 of
 * wp_image_description_v1 is NULL" is the whole client gone, not a warning. */
static void cm_desc_ready2(void *d, struct wp_image_description_v1 *desc,
		uint32_t lo, uint32_t hi) {
	(void)hi;
	cm_desc_ready(d, desc, lo);
}
static const struct wp_image_description_v1_listener cm_desc_listener = {
	.failed = cm_desc_failed,
	.ready = cm_desc_ready,
	.ready2 = cm_desc_ready2,
};

/* The compositor says the preferred description changed; ask what it is now.
 * The event carries no values -- it is a hint to re-read, which is why the
 * event count alone cannot stand in for the description. */
static void cm_preferred_changed(void *d,
		struct wp_color_management_surface_feedback_v1 *fb, uint32_t identity) {
	(void)d; (void)identity;
	struct wp_image_description_v1 *desc =
		wp_color_management_surface_feedback_v1_get_preferred(fb);
	wp_image_description_v1_add_listener(desc, &cm_desc_listener, NULL);
}
static void cm_preferred_changed2(void *d,
		struct wp_color_management_surface_feedback_v1 *fb,
		uint32_t lo, uint32_t hi) {
	(void)hi;
	cm_preferred_changed(d, fb, lo);
}
static const struct wp_color_management_surface_feedback_v1_listener
		cm_feedback_listener = {
	.preferred_changed = cm_preferred_changed,
	.preferred_changed2 = cm_preferred_changed2,
};

static const struct frog_color_managed_surface_listener frog_surface_listener = {
	.preferred_metadata = frog_preferred_metadata,
};
static struct xdg_wm_base *wm_base = NULL;
static struct zxdg_decoration_manager_v1 *decoration_manager = NULL;
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
/* 24, deliberately narrow. A wide gap survives a sloppy region operation that a
 * narrow one does not: the whole point of the fixture is that the hole between
 * the two rectangles is never filled in, and the easiest way for that to happen
 * is a dilation somewhere that a 60 px gap would absorb without complaint. */
#define GAP_W 24
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
	} else if (strcmp(interface, wp_color_manager_v1_interface.name) == 0) {
		/* The capability advertisement, printed verbatim in arrival order.
		 * The gate that compares the native manager against wlroots' compares
		 * THESE LINES -- so the client must not sort, dedupe or interpret
		 * them: two implementations that advertise the same set in a different
		 * order are not the same advertisement to a client that reads the
		 * first one it recognises. */
		/*
		 * VERSION 2, and version 1 is not good enough here.
		 *
		 * The compositor describes an SDR output with COMPOUND_POWER_2_4
		 * (protocol 14), which the XML marks `since="2"`. Bound at 1, this
		 * observer received `failed: unhandled value for tf_named` instead of
		 * a description -- correct protocol behaviour, and it meant the
		 * fixture could not read the very value it exists to assert.
		 *
		 * Worth keeping in mind beyond this client: a REAL wp-cm client that
		 * binds version 1 hits the same wall against this compositor.
		 */
		cm_manager = wl_registry_bind(registry, name,
			&wp_color_manager_v1_interface, 2);
		wp_color_manager_v1_add_listener(cm_manager, &cm_mgr_listener, NULL);
	} else if (strcmp(interface,
			frog_color_management_factory_v1_interface.name) == 0) {
		frog_factory = wl_registry_bind(registry, name,
			&frog_color_management_factory_v1_interface, 1);
	} else if (!strcmp(interface, xdg_wm_base_interface.name)) {
		wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 6);
		xdg_wm_base_add_listener(wm_base, &wm_base_listener, NULL);
	} else if (!strcmp(interface,
			ext_background_effect_manager_v1_interface.name)) {
		bg_manager = wl_registry_bind(registry, name,
			&ext_background_effect_manager_v1_interface, 1);
	} else if (!strcmp(interface,
			zxdg_decoration_manager_v1_interface.name)) {
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

/*
 * QUADRANTS, for the transform pixel oracle.
 *
 * A FLAT surface cannot detect a texture-orientation error at all: a solid
 * colour rotated by any amount is the same solid colour, so a fixture built
 * entirely out of flat windows reports 0 differing pixels whether the sampler
 * is oriented correctly or not. That is precisely how a wrong texture
 * transform at 90 and 270 degrees survived a pixel-exact eight-transform
 * oracle -- every window in it was one colour.
 *
 * Four distinct quadrants are the smallest fixture that is deterministic
 * between runs (no text, no layout, no client-side rounding) and still tells
 * every rotation and every mirror apart.
 */
static int quad_mode;

/*
 * ── A DETERMINISTIC SMALL UPDATE ──────────────────────────────────────────
 *
 * WLBGEFFECT_PULSE=WxH@X,Y:PERIOD_MS paints a rectangle of exactly that
 * geometry, at that position in the surface, alternating between two colours
 * every PERIOD_MS -- and damages ONLY that rectangle.
 *
 * M4F.2D needs a small source change behind a blur whose geometry is known
 * exactly. The obvious candidate, a terminal's cursor blink, is not usable:
 * the harness pins cursor_blink_interval=0, so the "small update" workload
 * measured a desktop where nothing changed at all and reported
 * processed=0 result=0 as though it were a performance result.
 *
 * TWO BUFFERS, alternating on wl_buffer.release, because re-committing one
 * buffer the compositor may still be reading is a race that shows up as a
 * torn or stale rectangle exactly where the measurement is looking.
 */
static struct { int w, h, x, y, period_ms, count; int on; int fired; } pulse;
static struct wl_buffer *pulse_buf[2];
static uint32_t *pulse_px[2];
static int pulse_free[2] = { 1, 1 };
static int pulse_phase;

static void buffer_release(void *data, struct wl_buffer *b) {
	(void)b;
	int idx = (int)(intptr_t)data;
	if (idx >= 0 && idx < 2) {
		pulse_free[idx] = 1;
	}
}
static const struct wl_buffer_listener buffer_listener = {
	.release = buffer_release,
};

static void pulse_parse(void) {
	const char *env = getenv("WLBGEFFECT_PULSE");
	if (env == NULL || env[0] == '\0') {
		return;
	}
	/*
	 * WxH@X,Y:MS[:COUNT]. COUNT stops the mutation after that many pulses and
	 * leaves the window where it is.
	 *
	 * Without it the surface keeps changing THROUGH the forced-full oracle
	 * comparison, so the two captures are of different desktops and the
	 * baseline "differs from a full redraw" by exactly one pulse rectangle --
	 * measured, 256 px for a 16x16 pulse. A settled scene is a precondition
	 * for that oracle, not a detail.
	 */
	int n = sscanf(env, "%dx%d@%d,%d:%d:%d", &pulse.w, &pulse.h, &pulse.x,
		&pulse.y, &pulse.period_ms, &pulse.count);
	if (n < 5 || pulse.w <= 0 || pulse.h <= 0) {
		fprintf(stderr, "wlbgeffect: WLBGEFFECT_PULSE=%s is not "
			"WxH@X,Y:MS[:COUNT]\n", env);
		return;
	}
	if (n < 6) {
		pulse.count = 0;   /* 0 = forever */
	}
	pulse.on = 1;
	/* An explicit, greppable startup line: the fixture must be able to ASSERT
	 * that pulse mode was enabled rather than infer it from a timing shape. */
	fprintf(stderr, "wlbgeffect: pulse enabled geometry=%dx%d@%d,%d "
		"period_ms=%d count=%d\n", pulse.w, pulse.h, pulse.x, pulse.y,
		pulse.period_ms, pulse.count);
	fflush(stderr);
}

/* Paint the pulse rectangle into one buffer. `bright` picks the phase. */
static void pulse_paint(uint32_t *px, int stride_px, int bright) {
	uint32_t c = bright ? 0xffffffffu : 0xff000000u;
	for (int y = 0; y < pulse.h; y++) {
		int py = pulse.y + y;
		if (py < 0 || py >= SURF_H) {
			continue;
		}
		for (int x = 0; x < pulse.w; x++) {
			int pxx = pulse.x + x;
			if (pxx < 0 || pxx >= SURF_W) {
				continue;
			}
			px[py * stride_px + pxx] = c;
		}
	}
}

/* When `keep` is non-NULL the mapping is handed back instead of unmapped, so
 * the caller can repaint part of it later. */
static struct wl_buffer *make_buffer_keep(int w, int h, uint32_t argb,
										  uint32_t **keep);

static struct wl_buffer *make_buffer(int w, int h, uint32_t argb) {
	return make_buffer_keep(w, h, argb, NULL);
}

static struct wl_buffer *make_buffer_keep(int w, int h, uint32_t argb,
										  uint32_t **keep) {
	int stride = w * 4;
	int size = stride * h;
	char path[] = "/tmp/wlbgeffect-shm-XXXXXX";
	int fd = mkstemp(path);
	if (fd < 0) { perror("mkstemp"); exit(1); }
	unlink(path);
	if (ftruncate(fd, size) < 0) { perror("ftruncate"); exit(1); }
	uint32_t *pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (pixels == MAP_FAILED) { perror("mmap"); exit(1); }
	if (quad_mode) {
		/* Derived from the requested colour so that two windows asked for
		 * different colours still differ, and so that no two quadrants of one
		 * window are the same. */
		const uint32_t q[4] = {
			argb,
			(argb & 0xff000000u) | ((argb & 0x00ffffffu) ^ 0x00f00000u),
			(argb & 0xff000000u) | ((argb & 0x00ffffffu) ^ 0x0000f000u),
			(argb & 0xff000000u) | ((argb & 0x00ffffffu) ^ 0x000000f0u),
		};
		for (int y = 0; y < h; y++) {
			for (int x = 0; x < w; x++) {
				int i = (y >= h / 2 ? 2 : 0) + (x >= w / 2 ? 1 : 0);
				pixels[y * w + x] = q[i];
			}
		}
	} else {
		for (int i = 0; i < w * h; i++) pixels[i] = argb;
	}
	if (keep != NULL) {
		*keep = pixels;
	} else {
		munmap(pixels, size);
	}

	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
	struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, w, h, stride,
													 WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	return buf;
}

int main(int argc, char **argv) {
	if (argc < 3) {
		fprintf(stderr, "usage: %s <app_id> <hold_seconds> [argb_hex]\n",
				argv[0]);
		return 1;
	}
	const char *app_id = argv[1];
	double hold = atof(argv[2]);
	/*
	 * An optional colour, for the transform pixel oracle.
	 *
	 * That oracle compares the same logical desktop rendered at two output
	 * rotations, so every pixel of the fixture has to be reproducible between
	 * two RUNS of the compositor. A terminal is not: comparing a 600x800
	 * output at 0 degrees against an 800x600 output at 90 degrees -- the same
	 * logical desktop -- left 7183 differing pixels, all of them inside the
	 * TEXT rows, because the client lays its glyphs out for the output it was
	 * told about. Flat surfaces of distinct colours have nothing to lay out.
	 */
	uint32_t argb = 0x80203040;
	/* WLBGEFFECT_QUAD=1 paints four quadrants instead of one flat colour. */
	quad_mode = getenv("WLBGEFFECT_QUAD") != NULL;
	pulse_parse();
	if (argc >= 4) {
		argb = (uint32_t)strtoul(argv[3], NULL, 16);
	}

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
	/*
	 * WLBGEFFECT_SSD=1 -- ask for a server-side border.
	 *
	 * asteroidz treats an xdg-toplevel that never negotiates decorations as
	 * client-decorated (client_wants_ssd), draws no border rect for it, and
	 * still reserves borderpx around the surface -- so the window LOOKS
	 * bordered and the ring is simply empty. A fixture that prices border
	 * fragment cost against such a client measures a border that was never
	 * drawn and reports it as free. That is not hypothetical: the M4H
	 * decoration audit's first run recorded border_draws = 0 with borderpx 4
	 * configured, and the ledger showed no border command in a 17-command
	 * frame.
	 */
	if (getenv("WLBGEFFECT_SSD") != NULL) {
		if (decoration_manager == NULL) {
			fprintf(stderr, "wlbgeffect: WLBGEFFECT_SSD but no "
					"xdg-decoration -- this window will have NO border\n");
		} else {
			struct zxdg_toplevel_decoration_v1 *dec =
				zxdg_decoration_manager_v1_get_toplevel_decoration(
					decoration_manager, toplevel);
			zxdg_toplevel_decoration_v1_set_mode(dec,
				ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
			fprintf(stderr, "wlbgeffect: requested server-side decoration\n");
		}
	}
	wl_surface_commit(surface);

	while (!configured && wl_display_dispatch(display) != -1)
		;

	/*
	 * TRANSLUCENT, and it has to be. A fully opaque surface can never show its
	 * backdrop, so asteroidz refuses to keep a blur node behind one
	 * (client_update_blur) -- and then there would be nothing for the region to
	 * clip. 0x80 alpha, and no wl_surface_set_opaque_region call.
	 */
	struct wl_buffer *buf;
	if (pulse.on) {
		/* Two, identical except for the pulse rectangle, so a change is
		 * exactly that rectangle and nothing else. */
		for (int i = 0; i < 2; i++) {
			pulse_buf[i] = make_buffer_keep(SURF_W, SURF_H, argb, &pulse_px[i]);
			pulse_paint(pulse_px[i], SURF_W, i);
			wl_buffer_add_listener(pulse_buf[i], &buffer_listener,
								   (void *)(intptr_t)i);
		}
		buf = pulse_buf[0];
		pulse_free[0] = 0;
		pulse_phase = 0;
	} else {
		buf = make_buffer(SURF_W, SURF_H, argb);
	}
	wl_surface_attach(surface, buf, 0, 0);
	wl_surface_damage(surface, 0, 0, SURF_W, SURF_H);

	/*
	 * WLBGEFFECT_NO_BLUR=1 -- a surface with NO blur region.
	 *
	 * The benchmark's backdrop window must be blur SOURCE, not a second blur
	 * PRODUCER: with two chains in the frame the up phase and the final
	 * upsample cannot be attributed by timestamps at all (the span between
	 * the marks would contain the other chain), and gpu_blur_up0 collected
	 * zero samples for exactly that reason.
	 */
	if (getenv("WLBGEFFECT_NO_BLUR") != NULL) {
		fprintf(stderr, "wlbgeffect: no blur region (source only)\n");
		fflush(stderr);
	} else if (bg_manager != NULL) {
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
	if (cm_manager != NULL) {
		cm_feedback = wp_color_manager_v1_get_surface_feedback(cm_manager,
			surface);
		wp_color_management_surface_feedback_v1_add_listener(cm_feedback,
			&cm_feedback_listener, NULL);
	}

	wl_surface_commit(surface);
	wl_display_roundtrip(display);

	/* AFTER the surface is mapped and committed, which is the point of putting
	 * the observer in this client: the object is created against a surface the
	 * compositor already has a window for, so a NULL effective output would be
	 * a compositor answer rather than a client artefact. */
	if (frog_factory != NULL) {
		frog_surface = frog_color_management_factory_v1_get_color_managed_surface(
			frog_factory, surface);
		frog_color_managed_surface_add_listener(frog_surface,
			&frog_surface_listener, NULL);
		wl_display_roundtrip(display);
	}
	if (cm_feedback != NULL) {
		/*
		 * READ AFTER MAP, but the feedback object itself was created BEFORE it
		 * (see above). A feedback created after the compositor has already
		 * sent its preferred description starts at wlroots' default and only
		 * learns the real answer on the next CHANGE -- so a fixture built that
		 * way reads gamma22 first and looks like a compositor defect. Creating
		 * it early is also what a real client does: you ask for feedback when
		 * you make the surface, not after you have drawn to it.
		 */
		struct wp_image_description_v1 *desc =
			wp_color_management_surface_feedback_v1_get_preferred(cm_feedback);
		wp_image_description_v1_add_listener(desc, &cm_desc_listener, NULL);
		wl_display_roundtrip(display);
		wl_display_roundtrip(display);
	}
	printf("frog_bound %d\n", frog_factory != NULL ? 1 : 0);
	printf("wpcm_bound %d\n", cm_manager != NULL ? 1 : 0);
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

		if (pulse.on && (pulse.count == 0 || pulse.fired < pulse.count)) {
			static double last;
			if (elapsed - last >= (double)pulse.period_ms / 1000.0) {
				int next = pulse_phase ^ 1;
				if (pulse_free[next]) {
					last = elapsed;
					pulse_free[next] = 0;
					pulse_phase = next;
					wl_surface_attach(surface, pulse_buf[next], 0, 0);
					/* ONLY the rectangle: the whole point is that the
					 * compositor sees a small, exactly known source change. */
					wl_surface_damage_buffer(surface, pulse.x, pulse.y,
											 pulse.w, pulse.h);
					wl_surface_commit(surface);
					/* One line per mutation, so "did the intended change
					 * happen" is a count and not an inference. */
					pulse.fired++;
					fprintf(stderr, "wlbgeffect: pulse %d %dx%d@%d,%d\n",
							next, pulse.w, pulse.h, pulse.x, pulse.y);
					if (pulse.count > 0 && pulse.fired >= pulse.count) {
						fprintf(stderr, "wlbgeffect: pulse done after %d\n",
								pulse.fired);
					}
					fflush(stderr);
				}
			}
		}

		while (wl_display_prepare_read(display) != 0)
			wl_display_dispatch_pending(display);
		wl_display_flush(display);

		struct pollfd pfd = {
			.fd = wl_display_get_fd(display),
			.events = POLLIN,
		};
		/*
		 * THE POLL TIMEOUT HAS TO RESPECT THE PULSE PERIOD.
		 *
		 * It used to be the whole remaining hold -- up to 300 SECONDS -- so
		 * with a static surface producing no Wayland events the loop blocked
		 * in poll() for the entire run and the pulse never fired once. The
		 * client printed "pulse enabled" and then did nothing, which is
		 * exactly the shape of failure the benchmark's premise gate caught:
		 * 0 pulses, 0 source damage, 0 blur chains.
		 */
		int timeout_ms = (int)((hold - elapsed) * 1000);
		if (pulse.on && pulse.period_ms > 0 && timeout_ms > pulse.period_ms) {
			timeout_ms = pulse.period_ms;
		}
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
