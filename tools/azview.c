/*
 * azview -- look at a picture, including the ones a viewer would guess at.
 *
 * Every other viewer on this machine tone maps a PQ picture down to SDR before
 * putting it on screen, because that is all it can do with a surface it has
 * told the compositor nothing about. So the one question an HDR still raises
 * -- does it actually look right -- could not be answered by looking at it.
 * Converting to PNG is worse: it answers a different question, about the
 * 8-bit rendition.
 *
 * This tells the compositor the truth about the picture. It reads the colour
 * description and the mastering metadata out of the file, builds a matching
 * wp-color-management image description, and attaches it to the surface. From
 * there the compositor's own colour pipeline does what it does for any HDR
 * content, which is precisely what is being looked at.
 *
 * ── ONE BUFFER FORMAT, TWO DECODERS ───────────────────────────────────────
 *
 * HEIF and AVIF come through libheif, which is the only one of the two that
 * carries a colour description worth honouring. Everything else -- PNG, JPEG,
 * GIF, BMP, TIFF, WebP, whatever else has a loader installed -- comes through
 * gdk-pixbuf, which the compositor already depends on.
 *
 * Both fill the same 10-bit buffer. An 8-bit source is widened rather than
 * converted: the surface is described as sRGB and BT.709, which is what those
 * files are, so the compositor's pipeline treats them exactly as it treats an
 * untagged desktop window. The alternative -- a second buffer format and a
 * second path through the colour code -- would be two things to keep in step
 * for no gain, since a 10-bit buffer holds an 8-bit picture perfectly.
 *
 * Usage: azview FILE
 *
 *   q or Escape   quit
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <libheif/heif.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "color-management-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "xdg-shell-client-protocol.h"

/* DRM fourcc, which is what wl_shm uses for everything except the two 8-bit
 * formats it spells as 0 and 1. */
#define FOURCC_XRGB2101010 0x30335258

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *wm_base;
static struct wp_color_manager_v1 *cm;
static struct wl_seat *seat;
static struct wl_subcompositor *subcomp;
static struct wp_viewporter *viewporter;
/*
 * TWO SURFACES, SO THE PICTURE KEEPS ITS PROPORTIONS.
 *
 * A toplevel must fill the size it is configured with -- on a tiling
 * compositor that size is not negotiable -- but a picture stretched to an
 * arbitrary rectangle is the wrong picture. So the toplevel is a black
 * background at exactly the configured size, and the image is a subsurface
 * scaled to fit inside it and centred.
 *
 * The scaling is a viewport, which means the COMPOSITOR resamples: on the GPU,
 * in its own colour pipeline, from the original 10-bit samples. Doing it here
 * would mean resampling PQ-coded values in C, which is both slower and wrong
 * -- interpolating between two PQ codes is not interpolating between two
 * luminances.
 */
static struct wl_surface *img_surface;
static struct wl_subsurface *img_sub;
static struct wp_viewport *img_vp, *bg_vp;
static int32_t win_w, win_h;      /* what the compositor configured */
/*
 * 1.0 is "fit the window". Above that the picture is magnified and only part
 * of it is visible; below, it is shrunk inside the window. Bounded so a
 * flurry of wheel clicks cannot reach a scale where the arithmetic stops
 * meaning anything -- a 1-pixel source rectangle, or a destination wider than
 * the compositor will accept.
 */
static double zoom = 1.0;
#define ZOOM_MIN 0.05
#define ZOOM_MAX 40.0
/* Per wheel detent. Gentle enough that a click is a nudge rather than a jump,
 * and a touchpad's fractional scrolling comes out continuous. */
#define ZOOM_STEP 1.10

/* Defined below, beside the buffers it attaches; the input handlers above it
 * all end in a redraw. */
static void redraw(void);
static uint32_t pic_w, pic_h;     /* the picture's own size */
static struct wl_keyboard *keyboard;
static struct wl_pointer *pointer;
static struct xkb_context *xkb_ctx;
static struct xkb_keymap *xkb_map;
static struct xkb_state *xkb_st;
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
		/* Silent: load_image() tries this first for EVERY file, so "not a
		 * HEIF" is the ordinary path for a PNG and not a thing to report. A
		 * file that IS one and fails later still complains, below. */
		heif_context_free(ctx);
		return false;
	}
	struct heif_image_handle *h = NULL;
	err = heif_context_get_primary_image_handle(ctx, &h);
	if (err.code != heif_error_Ok) {
		fprintf(stderr, "azview: no primary image: %s\n", err.message);
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
		fprintf(stderr, "azview: decode failed: %s\n", err.message);
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

/*
 * Everything libheif does not open.
 *
 * gdk-pixbuf is already a dependency of the compositor, and it brings whatever
 * loaders are installed -- PNG, JPEG, GIF, BMP, TIFF, WebP and the rest -- so
 * "the common formats" is a question answered by the system rather than by a
 * list kept here.
 *
 * The result is widened into the same 10-bit buffer the HEIF path fills, and
 * described as sRGB/BT.709 because that is what these files are. Widening is
 * a shift, not a conversion: 8-bit 255 becomes 1023 exactly, so nothing is
 * invented and nothing is lost.
 */
static bool load_pixbuf(const char *path, struct picture *p) {
	GError *err = NULL;
	GdkPixbuf *pb = gdk_pixbuf_new_from_file(path, &err);
	if (pb == NULL) {
		fprintf(stderr, "azview: %s: %s\n", path,
			err != NULL ? err->message : "cannot load");
		if (err != NULL) {
			g_error_free(err);
		}
		return false;
	}
	/* Flattened onto black rather than left alpha-premultiplied: the buffer
	 * is XRGB and a viewer that showed the alpha channel as colour would be
	 * showing something the file does not contain. */
	if (gdk_pixbuf_get_has_alpha(pb)) {
		GdkPixbuf *flat = gdk_pixbuf_composite_color_simple(pb,
			gdk_pixbuf_get_width(pb), gdk_pixbuf_get_height(pb),
			GDK_INTERP_NEAREST, 255, 8, 0x000000, 0x000000);
		g_object_unref(pb);
		if (flat == NULL) {
			return false;
		}
		pb = flat;
	}
	p->width = (uint32_t)gdk_pixbuf_get_width(pb);
	p->height = (uint32_t)gdk_pixbuf_get_height(pb);
	const int stride = gdk_pixbuf_get_rowstride(pb);
	const int nch = gdk_pixbuf_get_n_channels(pb);
	const guchar *src = gdk_pixbuf_get_pixels(pb);

	p->argb = calloc((size_t)p->width * p->height, sizeof(*p->argb));
	if (p->argb == NULL) {
		g_object_unref(pb);
		return false;
	}
	for (uint32_t y = 0; y < p->height; y++) {
		const guchar *row = src + (size_t)y * stride;
		for (uint32_t x = 0; x < p->width; x++) {
			const guchar *px = row + (size_t)x * nch;
			/* 8 bits to 10 by replication: v * 1023 / 255 is v << 2 | v >> 6,
			 * which keeps full scale exact at both ends. */
			uint32_t r = ((uint32_t)px[0] << 2) | (px[0] >> 6);
			uint32_t g = ((uint32_t)px[1] << 2) | (px[1] >> 6);
			uint32_t b = ((uint32_t)px[2] << 2) | (px[2] >> 6);
			p->argb[(size_t)y * p->width + x] =
				(3u << 30) | (r << 20) | (g << 10) | b;
		}
	}
	g_object_unref(pb);
	/* H.273 code points, the same ones the HEIF path reports. */
	p->primaries = 1;   /* BT.709 */
	p->transfer = 13;   /* sRGB */
	p->matrix = 1;      /* BT.709 -- unused, the buffer is already RGB */
	p->full_range = true;
	p->has_mastering = false;
	return true;
}

/* HEIF first, because it is the only one that carries a colour description
 * worth honouring; anything it declines is gdk-pixbuf's. */
static bool load_image(const char *path, struct picture *p) {
	if (load_heif(path, p)) {
		return true;
	}
	return load_pixbuf(path, p);
}

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

/*
 * A KEYBOARD, ONLY SO THAT q QUITS.
 *
 * The compositor sends keycodes and a keymap, not characters, so the keymap
 * has to be compiled to know which key that is -- a viewer that hardcoded a
 * scancode would quit on a different key under Dvorak. xkbcommon is already a
 * dependency here.
 */
static void kb_keymap(void *d, struct wl_keyboard *k, uint32_t fmt, int32_t fd,
		uint32_t size) {
	(void)d; (void)k;
	if (fmt != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
		close(fd);
		return;
	}
	char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	if (map == MAP_FAILED) {
		close(fd);
		return;
	}
	if (xkb_ctx == NULL) {
		xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	}
	xkb_keymap_unref(xkb_map);
	xkb_state_unref(xkb_st);
	xkb_map = xkb_keymap_new_from_string(xkb_ctx, map,
		XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
	xkb_st = xkb_map != NULL ? xkb_state_new(xkb_map) : NULL;
	munmap(map, size);
	close(fd);
}
static void kb_key(void *d, struct wl_keyboard *k, uint32_t serial,
		uint32_t time, uint32_t key, uint32_t state) {
	(void)d; (void)k; (void)serial; (void)time;
	if (state != WL_KEYBOARD_KEY_STATE_PRESSED || xkb_st == NULL) {
		return;
	}
	/* +8: libinput keycodes are offset from xkb's by the X11 convention. */
	xkb_keysym_t sym = xkb_state_key_get_one_sym(xkb_st, key + 8);
	if (sym == XKB_KEY_q || sym == XKB_KEY_Q || sym == XKB_KEY_Escape) {
		running = false;
	} else if (sym == XKB_KEY_0 || sym == XKB_KEY_KP_0) {
		/* Back to fit. Wheeling out to find it again is guesswork; a picture
		 * you have zoomed into needs one key that means "start over". */
		zoom = 1.0;
		redraw();
	}
}
static void kb_enter(void *d, struct wl_keyboard *k, uint32_t s,
		struct wl_surface *sf, struct wl_array *keys) {
	(void)d; (void)k; (void)s; (void)sf; (void)keys;
}
static void kb_leave(void *d, struct wl_keyboard *k, uint32_t s,
		struct wl_surface *sf) { (void)d; (void)k; (void)s; (void)sf; }
static void kb_modifiers(void *d, struct wl_keyboard *k, uint32_t s,
		uint32_t dep, uint32_t lat, uint32_t lock, uint32_t group) {
	(void)d; (void)k; (void)s;
	if (xkb_st != NULL) {
		xkb_state_update_mask(xkb_st, dep, lat, lock, 0, 0, group);
	}
}
static void kb_repeat(void *d, struct wl_keyboard *k, int32_t rate,
		int32_t delay) { (void)d; (void)k; (void)rate; (void)delay; }
static const struct wl_keyboard_listener kb_listener = {
	.keymap = kb_keymap, .enter = kb_enter, .leave = kb_leave,
	.key = kb_key, .modifiers = kb_modifiers, .repeat_info = kb_repeat,
};

/*
 * A POINTER, ONLY SO THE WHEEL ZOOMS.
 *
 * Both spellings of a wheel click are handled: axis_value120 is what a modern
 * compositor sends and carries the fraction of a detent, and plain axis is the
 * fallback for one that does not. Taking both without care would zoom twice
 * per click, so value120 wins for a frame and the axis event for that frame is
 * dropped.
 */
static double axis_pending;
static bool axis_from_120;

static void zoom_by(double factor) {
	double z = zoom * factor;
	if (z < ZOOM_MIN) z = ZOOM_MIN;
	if (z > ZOOM_MAX) z = ZOOM_MAX;
	if (z == zoom) {
		return;
	}
	zoom = z;
	redraw();
}

static void ptr_axis(void *d, struct wl_pointer *p, uint32_t time,
		uint32_t axis, wl_fixed_t value) {
	(void)d; (void)p; (void)time;
	if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL || axis_from_120) {
		return;
	}
	/*
	 * A DETENT IS NOT 1.0 HERE.
	 *
	 * wl_pointer.axis carries surface-relative scroll distance, which for one
	 * wheel click is conventionally 10 -- the same number libinput uses and
	 * every toolkit divides by. Treating it as a count of clicks made one
	 * notch of the wheel a factor of 1.15^10, near enough 4x, which is what
	 * "very rapid, big step" looks like.
	 */
	axis_pending += wl_fixed_to_double(value) / 10.0;
}
static void ptr_axis_value120(void *d, struct wl_pointer *p, uint32_t axis,
		int32_t v120) {
	(void)d; (void)p;
	if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL) {
		return;
	}
	axis_from_120 = true;
	axis_pending += v120 / 120.0;
}
static void ptr_frame(void *d, struct wl_pointer *p) {
	(void)d; (void)p;
	if (axis_pending != 0.0) {
		/* Scrolling DOWN is a positive value in both spellings, and down is
		 * out -- the same direction every other viewer uses. */
		zoom_by(pow(ZOOM_STEP, -axis_pending));
		axis_pending = 0.0;
	}
}
static void ptr_enter(void *d, struct wl_pointer *p, uint32_t s,
		struct wl_surface *sf, wl_fixed_t x, wl_fixed_t y) {
	(void)d; (void)p; (void)s; (void)sf; (void)x; (void)y;
}
static void ptr_leave(void *d, struct wl_pointer *p, uint32_t s,
		struct wl_surface *sf) { (void)d; (void)p; (void)s; (void)sf; }
static void ptr_motion(void *d, struct wl_pointer *p, uint32_t t,
		wl_fixed_t x, wl_fixed_t y) { (void)d; (void)p; (void)t; (void)x; (void)y; }
static void ptr_button(void *d, struct wl_pointer *p, uint32_t s, uint32_t t,
		uint32_t b, uint32_t st) { (void)d; (void)p; (void)s; (void)t; (void)b; (void)st; }
static void ptr_axis_source(void *d, struct wl_pointer *p, uint32_t src) {
	(void)d; (void)p; (void)src;
}
static void ptr_axis_stop(void *d, struct wl_pointer *p, uint32_t t,
		uint32_t axis) { (void)d; (void)p; (void)t; (void)axis; }
static void ptr_axis_discrete(void *d, struct wl_pointer *p, uint32_t axis,
		int32_t disc) { (void)d; (void)p; (void)axis; (void)disc; }
static void ptr_axis_relative_direction(void *d, struct wl_pointer *p,
		uint32_t axis, uint32_t dir) { (void)d; (void)p; (void)axis; (void)dir; }
static const struct wl_pointer_listener ptr_listener = {
	.enter = ptr_enter, .leave = ptr_leave, .motion = ptr_motion,
	.button = ptr_button, .axis = ptr_axis, .frame = ptr_frame,
	.axis_source = ptr_axis_source, .axis_stop = ptr_axis_stop,
	.axis_discrete = ptr_axis_discrete, .axis_value120 = ptr_axis_value120,
	.axis_relative_direction = ptr_axis_relative_direction,
};

static void seat_caps(void *d, struct wl_seat *s, uint32_t caps) {
	(void)d;
	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && keyboard == NULL) {
		keyboard = wl_seat_get_keyboard(s);
		wl_keyboard_add_listener(keyboard, &kb_listener, NULL);
	}
	if ((caps & WL_SEAT_CAPABILITY_POINTER) && pointer == NULL) {
		pointer = wl_seat_get_pointer(s);
		wl_pointer_add_listener(pointer, &ptr_listener, NULL);
	}
}
static void seat_name(void *d, struct wl_seat *s, const char *n) {
	(void)d; (void)s; (void)n;
}
static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_caps, .name = seat_name,
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
	} else if (strcmp(iface, wl_subcompositor_interface.name) == 0) {
		subcomp = wl_registry_bind(r, name, &wl_subcompositor_interface, 1);
	} else if (strcmp(iface, wp_viewporter_interface.name) == 0) {
		viewporter = wl_registry_bind(r, name, &wp_viewporter_interface, 1);
	} else if (strcmp(iface, wl_seat_interface.name) == 0) {
		/* 8, because that is where wl_pointer.axis_value120 appears -- the
		 * high-resolution wheel event. Bound at 5 the client never sees one,
		 * silently falls back to wl_pointer.axis, and misreads its value. */
		seat = wl_registry_bind(r, name, &wl_seat_interface,
			version < 8 ? version : 8);
		wl_seat_add_listener(seat, &seat_listener, NULL);
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

/*
 * Fit the picture inside the window without changing its proportions: one
 * scale factor for both axes, the smaller of the two the window allows, and
 * the remainder split evenly as a margin. Integer arithmetic throughout --
 * a rounded position and a rounded size that disagree leave a one-pixel seam
 * of background down one edge.
 */
static void apply_fit(void) {
	if (img_vp == NULL || bg_vp == NULL || win_w <= 0 || win_h <= 0
			|| pic_w == 0 || pic_h == 0) {
		return;
	}
	wp_viewport_set_destination(bg_vp, win_w, win_h);

	/*
	 * ZOOM IS A SOURCE RECTANGLE, NOT A BIGGER DESTINATION.
	 *
	 * Magnifying by growing the subsurface would push it outside the parent,
	 * and a Wayland subsurface is not clipped to its parent -- the picture
	 * would spill across the rest of the desktop. Cropping the SOURCE instead
	 * keeps the destination inside the window by construction: what is
	 * off-screen is simply not part of the rectangle being sampled.
	 *
	 * At zoom 1 the source is the whole picture and the destination is the
	 * fitted rectangle, which is the letterboxed case unchanged.
	 */
	double fit = (double)win_w / pic_w;
	double by_h = (double)win_h / pic_h;
	if (by_h < fit) {
		fit = by_h;
	}
	const double scale = fit * zoom;

	/* How much of the picture fits on screen at this scale, never more than
	 * the picture itself. */
	double vis_w = (double)win_w / scale;
	double vis_h = (double)win_h / scale;
	if (vis_w > pic_w) vis_w = pic_w;
	if (vis_h > pic_h) vis_h = pic_h;

	int32_t dw = (int32_t)(vis_w * scale + 0.5);
	int32_t dh = (int32_t)(vis_h * scale + 0.5);
	if (dw < 1) dw = 1;
	if (dh < 1) dh = 1;
	if (dw > win_w) dw = win_w;
	if (dh > win_h) dh = win_h;

	/* Centred. A pan would be an offset here, and nothing else. */
	double sx = ((double)pic_w - vis_w) / 2.0;
	double sy = ((double)pic_h - vis_h) / 2.0;
	if (sx < 0) sx = 0;
	if (sy < 0) sy = 0;

	wp_viewport_set_source(img_vp, wl_fixed_from_double(sx),
		wl_fixed_from_double(sy), wl_fixed_from_double(vis_w),
		wl_fixed_from_double(vis_h));
	wp_viewport_set_destination(img_vp, dw, dh);
	wl_subsurface_set_position(img_sub, (win_w - dw) / 2, (win_h - dh) / 2);
}

static struct wl_buffer *img_buffer, *bg_buffer;

/*
 * DRAWING IS DRIVEN BY configure, NOT BY main().
 *
 * xdg-shell says a surface has no size until the compositor gives it one, and
 * attaching before the first configure is acked is attaching to nothing. The
 * first version did exactly that, so the window came up and the picture never
 * did: the viewport values for the real window size were computed and then
 * never committed -- a black rectangle, the background doing its job with
 * nothing on top of it.
 */
/*
 * THE PICTURE IS UPLOADED ONCE.
 *
 * A zoom or a resize changes the viewport, and a viewport is surface state:
 * committing it is enough, the content behind it has not moved a pixel.
 * Attaching the same wl_shm buffer again -- with damage covering it, as the
 * first version did -- instead tells the compositor every pixel is new, so it
 * re-uploads the whole picture. For a 6016x6016 photograph that is 145 MB of
 * texture upload per wheel click, which is exactly as slow as it sounds.
 */
static bool attached;

static void redraw(void) {
	if (img_buffer == NULL || bg_buffer == NULL) {
		return;
	}
	apply_fit();
	if (!attached) {
		wl_surface_attach(img_surface, img_buffer, 0, 0);
		wl_surface_damage_buffer(img_surface, 0, 0, (int32_t)pic_w,
			(int32_t)pic_h);
		wl_surface_attach(surface, bg_buffer, 0, 0);
		wl_surface_damage_buffer(surface, 0, 0, 1, 1);
		attached = true;
	}
	wl_surface_commit(img_surface);
	wl_surface_commit(surface);
}

static void xdg_surface_configure(void *d, struct xdg_surface *s,
		uint32_t serial) {
	(void)d;
	xdg_surface_ack_configure(s, serial);
	configured = true;
	redraw();
}
static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = xdg_surface_configure,
};

static void toplevel_configure_impl(int32_t w, int32_t h) {
	/* A zero means "you choose", which for a viewer means the picture's own
	 * size -- the window then already has the right proportions and the fit
	 * is the identity. */
	win_w = w > 0 ? w : (int32_t)pic_w;
	win_h = h > 0 ? h : (int32_t)pic_h;
	apply_fit();
}

static void toplevel_configure(void *d, struct xdg_toplevel *t, int32_t w,
		int32_t h, struct wl_array *states) {
	(void)d; (void)t; (void)states;
	toplevel_configure_impl(w, h);
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
	fprintf(stderr, "azview: the compositor refused the image "
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
	if (argc != 2 || argv[1][0] == '-') {
		fprintf(stderr,
			"usage: %s FILE\n\n"
			"  Shows a picture as the file says it is: HEIF and AVIF through\n"
			"  libheif with their own colour description, everything else\n"
			"  through gdk-pixbuf as sRGB.\n\n"
			"  q or Escape   quit\n", argv[0]);
		return 2;
	}
	const char *path = argv[1];
	const bool force_sdr = false, no_cm = false;
	(void)force_sdr; (void)no_cm;

	struct picture pic = {0};
	if (!load_image(path, &pic)) {
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
		fprintf(stderr, "azview: cannot connect to a Wayland display\n");
		return 2;
	}
	struct wl_registry *reg = wl_display_get_registry(dpy);
	wl_registry_add_listener(reg, &registry_listener, NULL);
	wl_display_roundtrip(dpy);   /* globals */
	wl_display_roundtrip(dpy);   /* shm formats */

	if (compositor == NULL || shm == NULL || wm_base == NULL) {
		fprintf(stderr, "azview: the compositor is missing "
			"wl_compositor, wl_shm or xdg_wm_base\n");
		return 2;
	}
	if (!have_xrgb2101010) {
		fprintf(stderr, "azview: the compositor offers no 10-bit shm "
			"format; a 10-bit picture cannot be shown without quantising it, "
			"which is the thing this tool exists not to do\n");
		return 1;
	}

	size_t stride = (size_t)pic.width * 4;
	size_t size = stride * pic.height;
	int fd = memfd_create("azview", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, (off_t)size) < 0) {
		fprintf(stderr, "azview: memfd: %s\n", strerror(errno));
		return 1;
	}
	void *map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED) {
		fprintf(stderr, "azview: mmap: %s\n", strerror(errno));
		return 1;
	}
	memcpy(map, pic.argb, size);
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)size);
	struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0,
		(int32_t)pic.width, (int32_t)pic.height, (int32_t)stride,
		FOURCC_XRGB2101010);
	wl_shm_pool_destroy(pool);
	close(fd);

	if (subcomp == NULL || viewporter == NULL) {
		fprintf(stderr, "azview: the compositor is missing wl_subcompositor "
			"or wp_viewporter, so the picture cannot be fitted to the "
			"window without stretching it\n");
		return 2;
	}
	pic_w = pic.width;
	pic_h = pic.height;

	surface = wl_compositor_create_surface(compositor);
	xsurf = xdg_wm_base_get_xdg_surface(wm_base, surface);
	xdg_surface_add_listener(xsurf, &xdg_surface_listener, NULL);
	toplevel = xdg_surface_get_toplevel(xsurf);
	xdg_toplevel_add_listener(toplevel, &toplevel_listener, NULL);
	xdg_toplevel_set_title(toplevel, path);
	xdg_toplevel_set_app_id(toplevel, "azview");

	/* The background is one black pixel stretched by a viewport: a
	 * window-sized buffer of a single colour would be megabytes of memcpy
	 * every resize for a result the compositor can produce from one texel. */
	img_surface = wl_compositor_create_surface(compositor);
	img_sub = wl_subcompositor_get_subsurface(subcomp, img_surface, surface);
	wl_subsurface_set_desync(img_sub);
	bg_vp = wp_viewporter_get_viewport(viewporter, surface);
	img_vp = wp_viewporter_get_viewport(viewporter, img_surface);

	wl_surface_commit(surface);
	wl_display_roundtrip(dpy);

	/* ── the point of the tool ─────────────────────────────────────────
	 *
	 * Describe the surface as the FILE describes itself. Everything the
	 * encoder wrote -- transfer function, primaries, the mastering display's
	 * own luminance and chromaticity -- is handed to the compositor, so what
	 * appears on screen is the result of its real HDR path rather than of a
	 * viewer's assumption. */
	/*
	 * AN sRGB PICTURE IS DESCRIBED BY SAYING NOTHING.
	 *
	 * An untagged surface is sRGB by protocol default, so attaching a
	 * description that says sRGB asks the compositor to confirm what it would
	 * already have assumed -- and adds a way to fail, because a compositor
	 * that does not advertise the named transfer function would see the
	 * request refused. PNG and JPEG take that path every time, and a warning
	 * on every ordinary picture is a warning nobody reads.
	 *
	 * So the description is built only when the file says something the
	 * default does not cover, which in practice is PQ and HLG.
	 */
	const bool needs_description = pic.transfer != 13 && pic.transfer != 0;
	if (cm != NULL && !no_cm && needs_description) {
		uint32_t tf = 0, prim = 0;
		bool ok = force_sdr
			? (tf = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_SRGB,
			   prim = WP_COLOR_MANAGER_V1_PRIMARIES_SRGB, true)
			: (named_transfer(pic.transfer, &tf)
				&& named_primaries(pic.primaries, &prim));
		if (ok && !supported(sup_tf, sup_tf_len, tf)) {
			fprintf(stderr, "azview: this compositor does not advertise "
				"transfer function %u (the one %s); showing the surface "
				"untagged rather than being disconnected for asking it\n",
				tf, force_sdr ? "--sdr asked for" : "this picture needs");
			ok = false;
		}
		if (ok && !supported(sup_prim, sup_prim_len, prim)) {
			fprintf(stderr, "azview: this compositor does not advertise "
				"the primaries this picture needs; showing it untagged\n");
			ok = false;
		}
		if (!ok) {
			fprintf(stderr, "azview: transfer %u / primaries %u could not "
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
				fprintf(stderr, "azview: the compositor does not advertise "
					"mastering metadata; describing the colour without it\n");
			}
			struct wp_image_description_v1 *desc =
				wp_image_description_creator_params_v1_create(params);
			wp_image_description_v1_add_listener(desc, &desc_listener, NULL);
			wl_display_roundtrip(dpy);
			if (desc_ready && !desc_failed) {
				struct wp_color_management_surface_v1 *cms =
					wp_color_manager_v1_get_surface(cm, img_surface);
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

	/* One black texel for the background: a window-sized buffer of a single
	 * colour would be megabytes of memcpy on every resize for something the
	 * compositor can stretch from one pixel. */
	int bgfd = memfd_create("azview-bg", MFD_CLOEXEC);
	uint32_t *bgpx = NULL;
	if (bgfd >= 0 && ftruncate(bgfd, 4) == 0) {
		bgpx = mmap(NULL, 4, PROT_READ | PROT_WRITE, MAP_SHARED, bgfd, 0);
		if (bgpx != MAP_FAILED) {
			bgpx[0] = 3u << 30;   /* opaque, all channels zero */
			struct wl_shm_pool *bgpool = wl_shm_create_pool(shm, bgfd, 4);
			bg_buffer = wl_shm_pool_create_buffer(bgpool, 0, 1, 1, 4,
				FOURCC_XRGB2101010);
			wl_shm_pool_destroy(bgpool);
		}
	}
	if (bg_buffer == NULL) {
		fprintf(stderr, "azview: cannot make the background buffer\n");
		return 1;
	}
	img_buffer = buffer;

	/*
	 * The first configure has usually already arrived by now -- it comes back
	 * on the roundtrip after the surface's first commit -- and redraw() was a
	 * no-op then because these buffers did not exist yet. So draw once here,
	 * and let the configure handler redraw on every resize after that.
	 *
	 * Waiting for another configure instead would wait forever: the
	 * compositor has no reason to send a second one until something changes.
	 */
	if (win_w <= 0 || win_h <= 0) {
		toplevel_configure_impl(0, 0);
	}
	redraw();

	while (running && wl_display_dispatch(dpy) >= 0) {
		;
	}
	/*
	 * A PROTOCOL ERROR IS NOT A CLEAN EXIT, and it used to look like one:
	 * wl_display_dispatch() returns -1, the loop ends, and the program
	 * returned 0 having shown nothing. The compositor's own reason is the
	 * only thing that says which request was wrong.
	 */
	int rc = 0;
	int derr = wl_display_get_error(dpy);
	if (derr != 0) {
		uint32_t code = 0;
		const struct wl_interface *iface = NULL;
		uint32_t id = 0;
		if (derr == EPROTO) {
			code = wl_display_get_protocol_error(dpy, &iface, &id);
			fprintf(stderr, "azview: protocol error %u on %s id %u\n",
				code, iface != NULL ? iface->name : "(unknown)", id);
		} else {
			fprintf(stderr, "azview: display error: %s\n", strerror(derr));
		}
		rc = 1;
	}

	wl_buffer_destroy(buffer);
	munmap(map, size);
	free(pic.argb);
	wl_display_disconnect(dpy);
	return rc;
}
