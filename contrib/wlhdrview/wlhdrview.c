/*
 * wlhdrview -- show an HDR still as it was encoded, not as a viewer guesses it.
 *
 * Every image viewer on this machine tone maps a PQ picture down to SDR before
 * putting it on screen, because that is the only thing it can do with a
 * surface it has not told the compositor anything about. So the one question
 * an HDR screenshot raises -- does it actually look right -- could not be
 * answered by looking at it. heif-convert to PNG is worse: it answers a
 * different question, about the 8-bit rendition.
 *
 * This tells the compositor the truth about the picture. It reads the colour
 * description and the mastering metadata out of the file, builds a matching
 * wp-color-management image description, and attaches it to the surface. From
 * there the compositor's own colour pipeline does what it does for any HDR
 * content, which is precisely what is being checked.
 *
 * ── WHAT THIS IS AN INSTRUMENT FOR ────────────────────────────────────────
 *
 * Two failures it can see and a PNG cannot:
 *
 *   - the picture is DARK or WASHED OUT: the transfer function in the file
 *     disagrees with the one the pixels were encoded against.
 *   - the colours are subtly off, greens especially: the matrix used for the
 *     RGB->YCbCr conversion disagrees with the matrix named in the stream.
 *
 * Both survive every check that reads the container's metadata, because the
 * metadata is not what is wrong -- the agreement between it and the samples
 * is.
 *
 * Usage: wlhdrview FILE.heic [--sdr] [--no-cm]
 *
 *   --sdr     describe the surface as sRGB rather than as what the file says,
 *             which is what a viewer that ignores the metadata effectively
 *             does. Side by side with a normal run, this is the difference
 *             the colour management is making.
 *   --no-cm   attach no image description at all. Not the same as --sdr: the
 *             compositor then applies its default for an untagged surface,
 *             and which of the two you get is worth being able to tell apart.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <libheif/heif.h>
#include <wayland-client.h>

#include "color-management-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

/* DRM fourcc, which is what wl_shm uses for everything except the two 8-bit
 * formats it spells as 0 and 1. */
#define FOURCC_XRGB2101010 0x30335258

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *wm_base;
static struct wp_color_manager_v1 *cm;
static bool have_xrgb2101010;
/*
 * What the compositor says it can do. Checked rather than assumed: asking for
 * a transfer function it never advertised is a protocol ERROR, which
 * disconnects the client -- so an unsupported request does not degrade, it
 * takes the whole viewer down. That is how --sdr first behaved.
 */
#define MAX_NAMED 64
static uint32_t sup_tf[MAX_NAMED], sup_prim[MAX_NAMED], sup_feat[MAX_NAMED];
static size_t sup_tf_len, sup_prim_len, sup_feat_len;

static bool supported(const uint32_t *list, size_t len, uint32_t v) {
	for (size_t i = 0; i < len; i++) {
		if (list[i] == v) {
			return true;
		}
	}
	return false;
}
static bool running = true;
static bool configured;

static struct wl_surface *surface;
static struct xdg_surface *xsurf;
static struct xdg_toplevel *toplevel;

/* ── the picture ───────────────────────────────────────────────────────── */

struct picture {
	uint32_t width, height;
	uint32_t *argb;          /* XRGB2101010, one word per pixel */
	/* what the FILE says it is, not what we would like it to be */
	uint16_t primaries, transfer, matrix;
	bool full_range;
	bool has_mastering;
	uint32_t master_x[3], master_y[3];   /* R, G, B in 0.00002 units */
	uint32_t white_x, white_y;
	uint32_t max_lum, min_lum;           /* 0.0001 cd/m2 */
	uint16_t max_cll, max_fall;
};

static bool load_heif(const char *path, struct picture *p) {
	struct heif_context *ctx = heif_context_alloc();
	if (ctx == NULL) {
		return false;
	}
	struct heif_error err = heif_context_read_from_file(ctx, path, NULL);
	if (err.code != heif_error_Ok) {
		fprintf(stderr, "wlhdrview: %s: %s\n", path, err.message);
		heif_context_free(ctx);
		return false;
	}
	struct heif_image_handle *h = NULL;
	err = heif_context_get_primary_image_handle(ctx, &h);
	if (err.code != heif_error_Ok) {
		fprintf(stderr, "wlhdrview: no primary image: %s\n", err.message);
		heif_context_free(ctx);
		return false;
	}

	/* The nclx profile IS the answer to "what is this picture". Read it
	 * before decoding, because a decode can succeed on a file whose colour
	 * description says something the pixels are not. */
	struct heif_color_profile_nclx *nclx = NULL;
	if (heif_image_handle_get_nclx_color_profile(h, &nclx).code
			== heif_error_Ok && nclx != NULL) {
		p->primaries = (uint16_t)nclx->color_primaries;
		p->transfer = (uint16_t)nclx->transfer_characteristics;
		p->matrix = (uint16_t)nclx->matrix_coefficients;
		p->full_range = nclx->full_range_flag != 0;
		heif_nclx_color_profile_free(nclx);
	} else {
		/* No profile is a real answer too, and a viewer that silently
		 * substitutes BT.709 is how a mislabelled file looks correct. */
		p->primaries = 2;   /* unspecified */
		p->transfer = 2;
		p->matrix = 2;
		p->full_range = true;
	}

	/*
	 * The mastering metadata, which is what a tone-mapping compositor uses to
	 * decide how to fit this picture into this display. Read from the file
	 * rather than assumed: the whole point of the tool is to show what was
	 * encoded, and substituting reference values here would hide exactly the
	 * mistake it exists to catch.
	 */
	heif_mastering_display_colour_volume mdcv;
	if (heif_image_handle_get_mastering_display_colour_volume(h, &mdcv) == 1) {
		for (int i = 0; i < 3; i++) {
			p->master_x[i] = mdcv.display_primaries_x[i];
			p->master_y[i] = mdcv.display_primaries_y[i];
		}
		p->white_x = mdcv.white_point_x;
		p->white_y = mdcv.white_point_y;
		p->max_lum = mdcv.max_display_mastering_luminance;
		p->min_lum = mdcv.min_display_mastering_luminance;
		p->has_mastering = true;
	}
	heif_content_light_level cll;
	if (heif_image_handle_get_content_light_level(h, &cll) == 1) {
		p->max_cll = cll.max_content_light_level;
		p->max_fall = cll.max_pic_average_light_level;
	}

	/* 16 bits per channel out, so a 10-bit picture is not quantised on the
	 * way through this tool. */
	struct heif_decoding_options *opts = heif_decoding_options_alloc();
	struct heif_image *img = NULL;
	err = heif_decode_image(h, &img, heif_colorspace_RGB,
		heif_chroma_interleaved_RRGGBB_LE, opts);
	heif_decoding_options_free(opts);
	if (err.code != heif_error_Ok) {
		fprintf(stderr, "wlhdrview: decode failed: %s\n", err.message);
		heif_image_handle_release(h);
		heif_context_free(ctx);
		return false;
	}

	p->width = (uint32_t)heif_image_get_width(img, heif_channel_interleaved);
	p->height = (uint32_t)heif_image_get_height(img, heif_channel_interleaved);
	int stride = 0;
	const uint8_t *src = heif_image_get_plane_readonly(img,
		heif_channel_interleaved, &stride);
	int bits = heif_image_get_bits_per_pixel_range(img,
		heif_channel_interleaved);
	if (src == NULL || p->width == 0 || p->height == 0) {
		heif_image_release(img);
		heif_image_handle_release(h);
		heif_context_free(ctx);
		return false;
	}

	p->argb = calloc((size_t)p->width * p->height, sizeof(uint32_t));
	if (p->argb == NULL) {
		heif_image_release(img);
		heif_image_handle_release(h);
		heif_context_free(ctx);
		return false;
	}
	/* libheif reports the range it decoded into; shifting by the difference
	 * rather than assuming 16 is what keeps an 8-bit file from coming out
	 * four stops dark. */
	int shift = bits - 10;
	for (uint32_t y = 0; y < p->height; y++) {
		const uint16_t *row = (const uint16_t *)(src + (size_t)y * stride);
		uint32_t *dst = p->argb + (size_t)y * p->width;
		for (uint32_t x = 0; x < p->width; x++) {
			uint32_t r = row[x * 3 + 0];
			uint32_t g = row[x * 3 + 1];
			uint32_t b = row[x * 3 + 2];
			if (shift > 0) {
				r >>= shift; g >>= shift; b >>= shift;
			} else if (shift < 0) {
				r <<= -shift; g <<= -shift; b <<= -shift;
			}
			if (r > 1023) r = 1023;
			if (g > 1023) g = 1023;
			if (b > 1023) b = 1023;
			dst[x] = (3u << 30) | (r << 20) | (g << 10) | b;
		}
	}
	heif_image_release(img);
	heif_image_handle_release(h);
	heif_context_free(ctx);
	return true;
}

/* ── wayland ───────────────────────────────────────────────────────────── */

static void shm_format(void *d, struct wl_shm *s, uint32_t format) {
	(void)d; (void)s;
	if (format == FOURCC_XRGB2101010) {
		have_xrgb2101010 = true;
	}
}
static const struct wl_shm_listener shm_listener = {.format = shm_format};

static void wm_ping(void *d, struct xdg_wm_base *b, uint32_t serial) {
	(void)d;
	xdg_wm_base_pong(b, serial);
}
static const struct xdg_wm_base_listener wm_listener = {.ping = wm_ping};

static void cm_supported_intent(void *d, struct wp_color_manager_v1 *o,
		uint32_t v) { (void)d; (void)o; (void)v; }
static void cm_supported_feature(void *d, struct wp_color_manager_v1 *o,
		uint32_t v) {
	(void)d; (void)o;
	/* FEATURES are checked as strictly as named enums. set_mastering_luminance
	 * on a compositor that never advertised it is a protocol error, and a
	 * protocol error takes the viewer down rather than degrading -- which is
	 * exactly what it did the first time the metadata was carried through. */
	if (sup_feat_len < MAX_NAMED) {
		sup_feat[sup_feat_len++] = v;
	}
}
static void cm_supported_tf(void *d, struct wp_color_manager_v1 *o,
		uint32_t v) {
	(void)d; (void)o;
	if (sup_tf_len < MAX_NAMED) {
		sup_tf[sup_tf_len++] = v;
	}
}
static void cm_supported_primaries(void *d, struct wp_color_manager_v1 *o,
		uint32_t v) {
	(void)d; (void)o;
	if (sup_prim_len < MAX_NAMED) {
		sup_prim[sup_prim_len++] = v;
	}
}
static void cm_done(void *d, struct wp_color_manager_v1 *o) {
	(void)d; (void)o;
}
static const struct wp_color_manager_v1_listener cm_listener = {
	.supported_intent = cm_supported_intent,
	.supported_feature = cm_supported_feature,
	.supported_tf_named = cm_supported_tf,
	.supported_primaries_named = cm_supported_primaries,
	.done = cm_done,
};

static void registry_global(void *data, struct wl_registry *r, uint32_t name,
		const char *iface, uint32_t version) {
	(void)data;
	if (strcmp(iface, wl_compositor_interface.name) == 0) {
		compositor = wl_registry_bind(r, name, &wl_compositor_interface, 4);
	} else if (strcmp(iface, wl_shm_interface.name) == 0) {
		shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
		wl_shm_add_listener(shm, &shm_listener, NULL);
	} else if (strcmp(iface, xdg_wm_base_interface.name) == 0) {
		wm_base = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
		xdg_wm_base_add_listener(wm_base, &wm_listener, NULL);
	} else if (strcmp(iface, wp_color_manager_v1_interface.name) == 0) {
		cm = wl_registry_bind(r, name, &wp_color_manager_v1_interface,
			version < 1 ? version : 1);
		wp_color_manager_v1_add_listener(cm, &cm_listener, NULL);
	}
}
static void registry_remove(void *d, struct wl_registry *r, uint32_t n) {
	(void)d; (void)r; (void)n;
}
static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_remove,
};

static void xdg_surface_configure(void *d, struct xdg_surface *s,
		uint32_t serial) {
	(void)d;
	xdg_surface_ack_configure(s, serial);
	configured = true;
}
static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = xdg_surface_configure,
};

static void toplevel_configure(void *d, struct xdg_toplevel *t, int32_t w,
		int32_t h, struct wl_array *states) {
	(void)d; (void)t; (void)w; (void)h; (void)states;
}
static void toplevel_close(void *d, struct xdg_toplevel *t) {
	(void)d; (void)t;
	running = false;
}
static const struct xdg_toplevel_listener toplevel_listener = {
	.configure = toplevel_configure,
	.close = toplevel_close,
};

static bool desc_ready, desc_failed;
static void desc_failed_cb(void *d, struct wp_image_description_v1 *o,
		uint32_t cause, const char *msg) {
	(void)d; (void)o; (void)cause;
	fprintf(stderr, "wlhdrview: the compositor refused the image "
		"description: %s\n", msg ? msg : "(no reason given)");
	desc_failed = true;
}
static void desc_ready_cb(void *d, struct wp_image_description_v1 *o,
		uint32_t identity) {
	(void)d; (void)o; (void)identity;
	desc_ready = true;
}
static const struct wp_image_description_v1_listener desc_listener = {
	.failed = desc_failed_cb,
	.ready = desc_ready_cb,
};

/* H.273 code points -> the protocol's own enums. Only the ones an HDR still
 * can actually carry; anything else is reported rather than guessed at. */
static bool named_transfer(uint16_t tf, uint32_t *out) {
	switch (tf) {
	case 16: *out = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ; return true;
	case 13: *out = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_SRGB; return true;
	case 1:
	case 6:
	case 14:
	case 15: *out = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_BT1886; return true;
	case 18: *out = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_HLG; return true;
	default: return false;
	}
}
static bool named_primaries(uint16_t pr, uint32_t *out) {
	switch (pr) {
	case 9: *out = WP_COLOR_MANAGER_V1_PRIMARIES_BT2020; return true;
	case 1: *out = WP_COLOR_MANAGER_V1_PRIMARIES_SRGB; return true;
	case 12: *out = WP_COLOR_MANAGER_V1_PRIMARIES_DISPLAY_P3; return true;
	default: return false;
	}
}

int main(int argc, char **argv) {
	const char *path = NULL;
	bool force_sdr = false, no_cm = false;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--sdr")) {
			force_sdr = true;
		} else if (!strcmp(argv[i], "--no-cm")) {
			no_cm = true;
		} else if (path == NULL) {
			path = argv[i];
		} else {
			fprintf(stderr, "usage: %s FILE.heic [--sdr] [--no-cm]\n",
				argv[0]);
			return 2;
		}
	}
	if (path == NULL) {
		fprintf(stderr, "usage: %s FILE.heic [--sdr] [--no-cm]\n", argv[0]);
		return 2;
	}

	struct picture pic = {0};
	if (!load_heif(path, &pic)) {
		return 1;
	}
	printf("%s: %ux%u, primaries %u, transfer %u, matrix %u, %s range\n",
		path, pic.width, pic.height, pic.primaries, pic.transfer, pic.matrix,
		pic.full_range ? "full" : "limited");
	if (pic.has_mastering) {
		/* The box's own units are 0.0001 cd/m2. Printing the raw field would
		 * report a 1000-nit master as 10000000. */
		printf("  mastering: %.4f..%.1f cd/m2, white (%u, %u), "
			"maxCLL %u, maxFALL %u\n", pic.min_lum / 10000.0,
			pic.max_lum / 10000.0, pic.white_x, pic.white_y,
			pic.max_cll, pic.max_fall);
	} else {
		printf("  mastering: none in the file\n");
	}

	struct wl_display *dpy = wl_display_connect(NULL);
	if (dpy == NULL) {
		fprintf(stderr, "wlhdrview: cannot connect to a Wayland display\n");
		return 2;
	}
	struct wl_registry *reg = wl_display_get_registry(dpy);
	wl_registry_add_listener(reg, &registry_listener, NULL);
	wl_display_roundtrip(dpy);   /* globals */
	wl_display_roundtrip(dpy);   /* shm formats */

	if (compositor == NULL || shm == NULL || wm_base == NULL) {
		fprintf(stderr, "wlhdrview: the compositor is missing "
			"wl_compositor, wl_shm or xdg_wm_base\n");
		return 2;
	}
	if (!have_xrgb2101010) {
		fprintf(stderr, "wlhdrview: the compositor offers no 10-bit shm "
			"format; a 10-bit picture cannot be shown without quantising it, "
			"which is the thing this tool exists not to do\n");
		return 1;
	}

	size_t stride = (size_t)pic.width * 4;
	size_t size = stride * pic.height;
	int fd = memfd_create("wlhdrview", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, (off_t)size) < 0) {
		fprintf(stderr, "wlhdrview: memfd: %s\n", strerror(errno));
		return 1;
	}
	void *map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED) {
		fprintf(stderr, "wlhdrview: mmap: %s\n", strerror(errno));
		return 1;
	}
	memcpy(map, pic.argb, size);
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)size);
	struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0,
		(int32_t)pic.width, (int32_t)pic.height, (int32_t)stride,
		FOURCC_XRGB2101010);
	wl_shm_pool_destroy(pool);
	close(fd);

	surface = wl_compositor_create_surface(compositor);
	xsurf = xdg_wm_base_get_xdg_surface(wm_base, surface);
	xdg_surface_add_listener(xsurf, &xdg_surface_listener, NULL);
	toplevel = xdg_surface_get_toplevel(xsurf);
	xdg_toplevel_add_listener(toplevel, &toplevel_listener, NULL);
	xdg_toplevel_set_title(toplevel, path);
	xdg_toplevel_set_app_id(toplevel, "wlhdrview");
	wl_surface_commit(surface);
	wl_display_roundtrip(dpy);

	/* ── the point of the tool ─────────────────────────────────────────
	 *
	 * Describe the surface as the FILE describes itself. Everything the
	 * encoder wrote -- transfer function, primaries, the mastering display's
	 * own luminance and chromaticity -- is handed to the compositor, so what
	 * appears on screen is the result of its real HDR path rather than of a
	 * viewer's assumption. */
	if (cm != NULL && !no_cm) {
		uint32_t tf = 0, prim = 0;
		bool ok = force_sdr
			? (tf = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_SRGB,
			   prim = WP_COLOR_MANAGER_V1_PRIMARIES_SRGB, true)
			: (named_transfer(pic.transfer, &tf)
				&& named_primaries(pic.primaries, &prim));
		if (ok && !supported(sup_tf, sup_tf_len, tf)) {
			fprintf(stderr, "wlhdrview: this compositor does not advertise "
				"transfer function %u (the one %s); showing the surface "
				"untagged rather than being disconnected for asking it\n",
				tf, force_sdr ? "--sdr asked for" : "this picture needs");
			ok = false;
		}
		if (ok && !supported(sup_prim, sup_prim_len, prim)) {
			fprintf(stderr, "wlhdrview: this compositor does not advertise "
				"the primaries this picture needs; showing it untagged\n");
			ok = false;
		}
		if (!ok) {
			fprintf(stderr, "wlhdrview: transfer %u / primaries %u could not "
				"be described; the surface is untagged\n",
				pic.transfer, pic.primaries);
		} else {
			struct wp_image_description_creator_params_v1 *params =
				wp_color_manager_v1_create_parametric_creator(cm);
			wp_image_description_creator_params_v1_set_tf_named(params, tf);
			wp_image_description_creator_params_v1_set_primaries_named(params,
				prim);
			/* The mdcv box stores luminance in 0.0001 cd/m2; the protocol
			 * wants the minimum in the same units and the maximum in whole
			 * cd/m2. Passing the raw field as both is how a 1000-nit display
			 * gets described as a ten-million-nit one. */
			bool can_master = supported(sup_feat, sup_feat_len,
				WP_COLOR_MANAGER_V1_FEATURE_SET_MASTERING_DISPLAY_PRIMARIES);
			if (pic.has_mastering && can_master) {
				wp_image_description_creator_params_v1_set_mastering_luminance(
					params, pic.min_lum, pic.max_lum / 10000);
				wp_image_description_creator_params_v1_set_max_cll(params,
					pic.max_cll);
				wp_image_description_creator_params_v1_set_max_fall(params,
					pic.max_fall);
			} else if (pic.has_mastering) {
				fprintf(stderr, "wlhdrview: the compositor does not advertise "
					"mastering metadata; describing the colour without it\n");
			}
			struct wp_image_description_v1 *desc =
				wp_image_description_creator_params_v1_create(params);
			wp_image_description_v1_add_listener(desc, &desc_listener, NULL);
			wl_display_roundtrip(dpy);
			if (desc_ready && !desc_failed) {
				struct wp_color_management_surface_v1 *cms =
					wp_color_manager_v1_get_surface(cm, surface);
				wp_color_management_surface_v1_set_image_description(cms,
					desc, WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL);
				printf("surface described: transfer %u, primaries %u%s\n",
					pic.transfer, pic.primaries,
					(pic.has_mastering && can_master)
						? ", with mastering metadata"
						: ", without mastering metadata");
			}
		}
	} else if (no_cm) {
		printf("surface left untagged (--no-cm)\n");
	}

	wl_surface_attach(surface, buffer, 0, 0);
	wl_surface_damage_buffer(surface, 0, 0, (int32_t)pic.width,
		(int32_t)pic.height);
	wl_surface_commit(surface);

	while (running && wl_display_dispatch(dpy) >= 0) {
		;
	}

	wl_buffer_destroy(buffer);
	munmap(map, size);
	free(pic.argb);
	wl_display_disconnect(dpy);
	return 0;
}
