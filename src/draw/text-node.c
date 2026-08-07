#include "text-node.h"

#include <cairo.h>
#include <drm_fourcc.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib.h>
#include <math.h>
#include <pango/pangocairo.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wlr/interfaces/wlr_buffer.h>

static GHashTable *font_desc_cache = NULL;

/* app icon cache: icon name -> cairo surface (NULL = negative entry) */
static GHashTable *icon_cache = NULL;
static char icon_theme_name[64] = "hicolor";

static void icon_surface_free(gpointer p) {
	if (p)
		cairo_surface_destroy((cairo_surface_t *)p);
}

void asteroidz_text_node_set_icon_theme(const char *theme) {
	if (!theme || !*theme)
		theme = "hicolor";
	if (strcmp(icon_theme_name, theme) == 0)
		return;
	snprintf(icon_theme_name, sizeof(icon_theme_name), "%s", theme);
	if (icon_cache) {
		g_hash_table_destroy(icon_cache);
		icon_cache = NULL;
	}
}

static char *resolve_icon_path_named(const char *name);

/* The `Icon=` of <name>.desktop, or NULL.
 *
 * An application's Wayland app-id is not required to be the name of an icon,
 * and often is not: Vivaldi reports `vivaldi-stable` and ships `vivaldi`.
 * The desktop entry is the mapping between the two, which is exactly what it
 * is for -- so when the app-id names no icon, ask it. */
static char *icon_name_from_desktop(const char *app_id) {
	const char *const *dirs = (const char *const *)g_get_system_data_dirs();
	char *result = NULL;
	for (int pass = 0; pass < 2 && !result; pass++) {
		for (int i = 0; !result; i++) {
			char *dir;
			if (pass == 0) {
				dir = g_build_filename(g_get_user_data_dir(), "applications",
									   NULL);
				if (i > 0) {
					g_free(dir);
					break;
				}
			} else {
				if (!dirs || !dirs[i])
					break;
				dir = g_build_filename(dirs[i], "applications", NULL);
			}
			char *file = g_strdup_printf("%s/%s.desktop", dir, app_id);
			g_free(dir);
			char *text = NULL;
			if (g_file_get_contents(file, &text, NULL, NULL)) {
				char **lines = g_strsplit(text, "\n", -1);
				for (int l = 0; lines[l] && !result; l++) {
					if (!strncmp(lines[l], "Icon=", 5) && lines[l][5])
						result = g_strdup(g_strstrip(lines[l] + 5));
				}
				g_strfreev(lines);
			}
			g_free(text);
			g_free(file);
		}
	}
	return result;
}

static char *resolve_icon_path(const char *name) {
	char *direct = resolve_icon_path_named(name);
	if (direct)
		return direct;
	/* nothing is installed under the app-id itself: let its desktop entry say
	 * what the icon is actually called */
	char *mapped = icon_name_from_desktop(name);
	if (!mapped)
		return NULL;
	char *path = strcmp(mapped, name) ? resolve_icon_path_named(mapped) : NULL;
	g_free(mapped);
	return path;
}

#define LENGTH_TN(X) (sizeof(X) / sizeof((X)[0]))

static char *resolve_icon_path_named(const char *name) {
	if (name[0] == '/')
		return g_file_test(name, G_FILE_TEST_EXISTS) ? g_strdup(name) : NULL;

	const char *home = g_get_home_dir();
	char *bases[2] = {g_build_filename(home, ".local/share/icons", NULL),
					  g_strdup("/usr/share/icons")};
	const char *themes[2] = {icon_theme_name, "hicolor"};
	/* Size dirs cover Papirus/hicolor (48x48/apps) and breeze (apps/48).
	 *
	 * Large first, because the bar scales down and downscaling looks better
	 * than up -- but the SMALL sizes have to be here too. Papirus ships
	 * vivaldi-stable at 16, 22 and 24 only, so a list that stopped at 48 found
	 * nothing for it and the tag pill drew no icon at all, for an icon that
	 * was installed the whole time. */
	const char *sizes[] = {"48x48", "64x64", "128x128", "scalable", "48", "64",
						   "32x32", "24x24", "22x22", "16x16",
						   "32",	"24",	 "22",	  "16"};
	const char *exts[] = {"svg", "png"};
	/* Icon theme CATEGORIES, in the order a bar is likely to want them.
	 *
	 * `apps` alone was not enough. A StatusNotifierItem's IconName is a plain
	 * theme name and nothing says it has to be an application icon -- the ones
	 * that are not are exactly the ones a tray shows: nm-applet reports
	 * network-wireless-* (status), a battery or bluetooth applet reports a
	 * device name (devices), and dialog-information -- what contrib/snitem
	 * serves -- is status too. Searched apps-first so an application icon
	 * still wins when both exist, which is the common case. */
	const char *cats[] = {"apps", "status", "devices", "actions",
						  "categories", "emblems", "mimetypes", "panel"};
	char *found = NULL;

	for (size_t t = 0; t < 2 && !found; t++) {
		if (t == 1 && strcmp(themes[0], themes[1]) == 0)
			break;
		for (size_t b = 0; b < 2 && !found; b++) {
			for (size_t c = 0; c < LENGTH_TN(cats) && !found; c++) {
				for (size_t z = 0; z < LENGTH_TN(sizes) && !found; z++) {
					for (size_t e = 0; e < 2 && !found; e++) {
						/* both layouts in the wild: <size>/<category> for
						 * hicolor and Papirus, <category>/<size> for breeze */
						char *path = g_strdup_printf("%s/%s/%s/%s/%s.%s",
							bases[b], themes[t], sizes[z], cats[c], name,
							exts[e]);
						if (g_file_test(path, G_FILE_TEST_EXISTS))
							found = path;
						else
							g_free(path);
						if (found)
							break;
						path = g_strdup_printf("%s/%s/%s/%s/%s.%s", bases[b],
							themes[t], cats[c], sizes[z], name, exts[e]);
						if (g_file_test(path, G_FILE_TEST_EXISTS))
							found = path;
						else
							g_free(path);
					}
				}
			}
		}
	}
	for (size_t e = 0; e < 2 && !found; e++) {
		char *path =
			g_strdup_printf("/usr/share/pixmaps/%s.%s", name, exts[e]);
		if (g_file_test(path, G_FILE_TEST_EXISTS))
			found = path;
		else
			g_free(path);
	}
	g_free(bases[0]);
	g_free(bases[1]);
	return found;
}

/* Font rendering options for every layout this file creates.
 *
 * Pango-on-cairo used directly inherits NOTHING from the desktop's font
 * settings -- that plumbing lives in GTK, which is why a GTK bar and this one
 * rendered the same font differently and the compositor's text looked coarser
 * beside it. Set them explicitly instead of taking cairo's defaults.
 *
 * Grayscale rather than subpixel: fontconfig on this desktop advertises RGB,
 * but the desktop setting (org.gnome.desktop.interface font-antialiasing) says
 * grayscale, and that is what every GTK client actually renders with. Subpixel
 * also assumes a fixed physical pixel order, which is wrong the moment a
 * surface is scaled or rotated -- and bar pills are rendered at the output's
 * scale factor.
 *
 * Slight hinting keeps stems crisp without distorting advance widths, which
 * matters here because widths are MEASURED and then pinned: full hinting moves
 * glyph advances enough that a pinned pill can end up a pixel short. */
static const cairo_font_options_t *asteroidz_font_options(void) {
	static cairo_font_options_t *opts = NULL;
	if (!opts) {
		opts = cairo_font_options_create();
		cairo_font_options_set_antialias(opts, CAIRO_ANTIALIAS_GRAY);
		cairo_font_options_set_hint_style(opts, CAIRO_HINT_STYLE_SLIGHT);
		cairo_font_options_set_hint_metrics(opts, CAIRO_HINT_METRICS_ON);
	}
	return opts;
}

/* Trim an ARGB32 surface to its alpha bounding box, taking ownership: returns
 * either the original (already tight, or not croppable) or a new, smaller one
 * with the original released.
 *
 * The bar gives every icon a box and lays those boxes out at a fixed
 * separation, so transparent margins inside the artwork are indistinguishable
 * from spacing -- a bell that is 20 units wide on a 24-unit canvas reads as
 * 2 extra pixels of gap on BOTH sides of that module, and only that module.
 * Measured on a rendered bar, that put 16px around the bell where the cpu and
 * memory icons beside it sat at 12.
 *
 * Cropping makes the surface's extent equal what you can see, which is the
 * assumption the layout and the vertical centring both already make. It is
 * the same trim asteroidz_icon_cache_put_argb32 does to tray pixmaps and
 * contrib/normalize-bar-icons.py does to our own art at build time -- doing it
 * here as well covers the non-square case those two cannot: normalising can
 * only fill the LONG axis of a square canvas, so a tall glyph keeps a margin
 * either side no matter how the file is written. */
static cairo_surface_t *surface_crop_to_ink(cairo_surface_t *src) {
	if (!src || cairo_surface_status(src) != CAIRO_STATUS_SUCCESS ||
		cairo_image_surface_get_format(src) != CAIRO_FORMAT_ARGB32)
		return src;
	cairo_surface_flush(src);
	int w = cairo_image_surface_get_width(src);
	int h = cairo_image_surface_get_height(src);
	const unsigned char *data = cairo_image_surface_get_data(src);
	int stride = cairo_image_surface_get_stride(src);
	if (w <= 0 || h <= 0 || !data)
		return src;

	int x0 = w, y0 = h, x1 = -1, y1 = -1;
	for (int y = 0; y < h; y++) {
		const uint32_t *row = (const uint32_t *)(data + (size_t)y * stride);
		for (int x = 0; x < w; x++) {
			if ((row[x] >> 24) == 0) /* fully transparent */
				continue;
			if (x < x0) x0 = x;
			if (x > x1) x1 = x;
			if (y < y0) y0 = y;
			if (y > y1) y1 = y;
		}
	}
	if (x1 < x0 || y1 < y0)
		return src; /* nothing drawn at all -- leave it alone */
	int cw = x1 - x0 + 1, ch = y1 - y0 + 1;
	if (cw == w && ch == h)
		return src;

	cairo_surface_t *dst =
		cairo_image_surface_create(CAIRO_FORMAT_ARGB32, cw, ch);
	if (cairo_surface_status(dst) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(dst);
		return src;
	}
	int dstride = cairo_image_surface_get_stride(dst);
	unsigned char *ddata = cairo_image_surface_get_data(dst);
	for (int y = 0; y < ch; y++) {
		memcpy(ddata + (size_t)y * dstride,
			   data + (size_t)(y + y0) * stride + (size_t)x0 * 4,
			   (size_t)cw * 4);
	}
	cairo_surface_mark_dirty(dst);
	cairo_surface_destroy(src);
	return dst;
}

/* How much horizontal room one icon takes in a row of them.
 *
 * Artwork is fitted into a square of the text area's height BY ITS LONG AXIS,
 * so a glyph taller than it is wide does not fill that square -- and reserving
 * the square anyway is transparent padding that the eye reads as spacing. A
 * bell 53x64 in a 29px row therefore advances 24px, not 29, and the gap either
 * side of it matches the gap between two square icons.
 *
 * A missing surface still advances the full square: the slot is reserved so a
 * failed icon load shifts nothing around it. */
static double asteroidz_icon_advance(cairo_surface_t *ic, double box) {
	if (!ic)
		return box;
	int w = cairo_image_surface_get_width(ic);
	int h = cairo_image_surface_get_height(ic);
	if (w <= 0 || h <= 0 || w >= h)
		return box;
	return box * (double)w / (double)h;
}

static cairo_surface_t *pixbuf_to_cairo_surface(GdkPixbuf *pixbuf) {
	int w = gdk_pixbuf_get_width(pixbuf);
	int h = gdk_pixbuf_get_height(pixbuf);
	gboolean has_alpha = gdk_pixbuf_get_has_alpha(pixbuf);
	int channels = has_alpha ? 4 : 3;
	int src_stride = gdk_pixbuf_get_rowstride(pixbuf);
	const guchar *src = gdk_pixbuf_read_pixels(pixbuf);

	cairo_surface_t *surf =
		cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
	if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(surf);
		return NULL;
	}
	int dst_stride = cairo_image_surface_get_stride(surf);
	unsigned char *dst = cairo_image_surface_get_data(surf);

	for (int y = 0; y < h; y++) {
		uint32_t *drow = (uint32_t *)(dst + y * dst_stride);
		const guchar *srow = src + y * src_stride;
		for (int x = 0; x < w; x++) {
			guchar r = srow[x * channels + 0];
			guchar g = srow[x * channels + 1];
			guchar b = srow[x * channels + 2];
			guchar a = has_alpha ? srow[x * channels + 3] : 0xFF;
			drow[x] = ((uint32_t)a << 24) | ((uint32_t)(r * a / 255) << 16) |
					  ((uint32_t)(g * a / 255) << 8) | (uint32_t)(b * a / 255);
		}
	}
	cairo_surface_mark_dirty(surf);
	return surf;
}

static cairo_surface_t *get_cached_icon(const char *name) {
	if (!icon_cache)
		icon_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
										   icon_surface_free);

	gpointer value = NULL;
	if (g_hash_table_lookup_extended(icon_cache, name, NULL, &value))
		return (cairo_surface_t *)value;

	char *path = resolve_icon_path(name);
	if (!path) {
		char *lower = g_ascii_strdown(name, -1);
		if (strcmp(lower, name) != 0)
			path = resolve_icon_path(lower);
		g_free(lower);
	}

	cairo_surface_t *surf = NULL;
	if (path) {
		GdkPixbuf *pixbuf =
			gdk_pixbuf_new_from_file_at_size(path, 64, 64, NULL);
		if (pixbuf) {
			surf = surface_crop_to_ink(pixbuf_to_cairo_surface(pixbuf));
			g_object_unref(pixbuf);
		}
		g_free(path);
	}
	g_hash_table_insert(icon_cache, g_strdup(name), surf);
	return surf;
}

bool asteroidz_icon_cache_put_surface(const char *key,
									  cairo_surface_t *surf) {
	if (!key || !*key || !surf)
		return false;
	if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(surf);
		return false;
	}
	if (!icon_cache)
		icon_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
										   icon_surface_free);
	/* takes ownership: the cache's destroy notify owns it from here, and
	 * replacing a key frees whatever it displaces */
	g_hash_table_insert(icon_cache, g_strdup(key), surf);
	return true;
}

bool asteroidz_icon_cache_put_argb32(const char *key, const uint8_t *argb_be,
									 int32_t w, int32_t h) {
	if (!key || !*key || !argb_be || w <= 0 || h <= 0)
		return false;
	if (!icon_cache)
		icon_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
										   icon_surface_free);

	/* Trim to the ink before caching.
	 *
	 * Applications ship tray icons with wildly different transparent margins:
	 * one centres a 22px drawing in a 24px canvas, another leaves four blank
	 * rows along the top. The draw path centres the SURFACE, so that margin
	 * became a vertical offset -- measured on a real bar, two tray icons sat
	 * 0.5px and 2px below the compositor's own icons beside them, which reads
	 * exactly as "the tray is not aligned".
	 *
	 * Cropping to the alpha bounding box makes centring the surface the same
	 * thing as centring what you see. It is the load-time equivalent of what
	 * contrib/normalize-bar-icons.py does to our own vendored art, which we
	 * cannot do ahead of time for pixels an application hands us at runtime. */
	int32_t x0 = w, y0 = h, x1 = -1, y1 = -1;
	for (int32_t y = 0; y < h; y++) {
		const uint8_t *srow = argb_be + (size_t)y * w * 4;
		for (int32_t x = 0; x < w; x++) {
			if (srow[x * 4 + 0] == 0) /* fully transparent */
				continue;
			if (x < x0) x0 = x;
			if (x > x1) x1 = x;
			if (y < y0) y0 = y;
			if (y > y1) y1 = y;
		}
	}
	if (x1 >= x0 && y1 >= y0) {
		argb_be += ((size_t)y0 * w + x0) * 4;
		/* the row stride stays the ORIGINAL width; only the extent changes */
		int32_t cw = x1 - x0 + 1, ch = y1 - y0 + 1;
		if (cw != w || ch != h) {
			cairo_surface_t *cs =
				cairo_image_surface_create(CAIRO_FORMAT_ARGB32, cw, ch);
			if (cairo_surface_status(cs) == CAIRO_STATUS_SUCCESS) {
				int cstride = cairo_image_surface_get_stride(cs);
				unsigned char *cdst = cairo_image_surface_get_data(cs);
				for (int32_t y = 0; y < ch; y++) {
					uint32_t *drow = (uint32_t *)(cdst + y * cstride);
					const uint8_t *srow = argb_be + (size_t)y * w * 4;
					for (int32_t x = 0; x < cw; x++) {
						uint8_t a = srow[x * 4 + 0];
						uint8_t r = srow[x * 4 + 1];
						uint8_t g = srow[x * 4 + 2];
						uint8_t b = srow[x * 4 + 3];
						drow[x] = ((uint32_t)a << 24) |
								  ((uint32_t)(r * a / 255) << 16) |
								  ((uint32_t)(g * a / 255) << 8) |
								  (uint32_t)(b * a / 255);
					}
				}
				cairo_surface_mark_dirty(cs);
				g_hash_table_insert(icon_cache, g_strdup(key), cs);
				return true;
			}
			cairo_surface_destroy(cs);
		}
	}

	cairo_surface_t *surf =
		cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
	if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(surf);
		return false;
	}
	int dst_stride = cairo_image_surface_get_stride(surf);
	unsigned char *dst = cairo_image_surface_get_data(surf);
	for (int32_t y = 0; y < h; y++) {
		uint32_t *drow = (uint32_t *)(dst + y * dst_stride);
		const uint8_t *srow = argb_be + (size_t)y * w * 4;
		for (int32_t x = 0; x < w; x++) {
			/* the wire format is ARGB32 in NETWORK byte order, byte by byte;
			 * cairo wants host-order words with the colour PREMULTIPLIED */
			uint8_t a = srow[x * 4 + 0];
			uint8_t r = srow[x * 4 + 1];
			uint8_t g = srow[x * 4 + 2];
			uint8_t b = srow[x * 4 + 3];
			drow[x] = ((uint32_t)a << 24) | ((uint32_t)(r * a / 255) << 16) |
					  ((uint32_t)(g * a / 255) << 8) | (uint32_t)(b * a / 255);
		}
	}
	cairo_surface_mark_dirty(surf);
	/* replaces any previous entry under this key, and the cache's destroy
	 * notify frees the old surface -- a tray item that pushes a new pixmap
	 * (an unread badge appearing) must not leak the old one */
	g_hash_table_insert(icon_cache, g_strdup(key), surf);
	return true;
}

static PangoFontDescription *get_cached_font_desc(const char *font_desc) {
	if (!font_desc_cache) {
		font_desc_cache =
			g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
								  (GDestroyNotify)pango_font_description_free);
	}

	PangoFontDescription *desc =
		g_hash_table_lookup(font_desc_cache, font_desc);
	if (!desc) {
		desc = pango_font_description_from_string(font_desc);
		g_hash_table_insert(font_desc_cache, g_strdup(font_desc), desc);
	}
	return desc;
}

/* A one-pixel scratch surface kept alive for the sake of its Pango context.
 * Measuring needs a context and a context needs a cairo target; nothing is ever
 * drawn into it. Pinned at 96 dpi -- see the header. */
static cairo_surface_t *metrics_surface = NULL;
static cairo_t *metrics_cr = NULL;
static PangoContext *metrics_context = NULL;

int32_t asteroidz_font_line_height(const char *font_desc) {
	if (!font_desc || !*font_desc)
		return 0;

	if (!metrics_context) {
		metrics_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
		metrics_cr = cairo_create(metrics_surface);
		metrics_context = pango_cairo_create_context(metrics_cr);
		pango_cairo_context_set_resolution(metrics_context, 96.0);
	}

	PangoFontMetrics *m = pango_context_get_metrics(
		metrics_context, get_cached_font_desc(font_desc), NULL);
	if (!m)
		return 0;

	/* Rounded UP. Half a pixel short of the ascent clips the tops of capitals,
	 * and the box this sizes has no way to grow afterwards. */
	int32_t h = (pango_font_metrics_get_ascent(m) +
				 pango_font_metrics_get_descent(m) + PANGO_SCALE - 1) /
				PANGO_SCALE;
	pango_font_metrics_unref(m);
	return h > 0 ? h : 0;
}

void asteroidz_text_global_finish(void) {
	if (font_desc_cache) {
		g_hash_table_destroy(font_desc_cache);
		font_desc_cache = NULL;
	}
	if (metrics_context) {
		g_object_unref(metrics_context);
		metrics_context = NULL;
	}
	if (metrics_cr) {
		cairo_destroy(metrics_cr);
		metrics_cr = NULL;
	}
	if (metrics_surface) {
		cairo_surface_destroy(metrics_surface);
		metrics_surface = NULL;
	}
}

static void text_buffer_destroy(struct wlr_buffer *wlr_buffer) {
	struct asteroidz_text_buffer *buf = wl_container_of(wlr_buffer, buf, base);
	/* the buffer holds its own reference on the surface (see the
	 * cairo_surface_reference() at every buf->surface assignment) -- drop it
	 * here so the surface outlives node->surface whenever the scene is still
	 * holding this wrapper. */
	cairo_surface_destroy(buf->surface);
	free(buf);
}

static bool text_buffer_begin_data_ptr_access(struct wlr_buffer *wlr_buffer,
											  uint32_t flags, void **data,
											  uint32_t *format,
											  size_t *stride) {
	(void)flags;
	struct asteroidz_text_buffer *buf = wl_container_of(wlr_buffer, buf, base);
	/* cairo_image_surface_get_data() returns NULL for a surface in an
	 * error status (e.g. created with a degenerate/zero size) without
	 * crashing itself -- but returning success (true) here with *data
	 * still NULL let the renderer memcpy from it downstream instead.
	 * Real crash: SIGSEGV in write_pixels with vdata=0x0, confirmed via
	 * coredumpctl on a 280x36 buffer (a titlebar tab/tag-pill size). */
	if (cairo_surface_status(buf->surface) != CAIRO_STATUS_SUCCESS)
		return false;
	*data = cairo_image_surface_get_data(buf->surface);
	if (!*data)
		return false;
	*format = DRM_FORMAT_ARGB8888;
	*stride = cairo_image_surface_get_stride(buf->surface);
	return true;
}

static void text_buffer_end_data_ptr_access(struct wlr_buffer *wlr_buffer) {}

static const struct wlr_buffer_impl text_buffer_impl = {
	.destroy = text_buffer_destroy,
	.begin_data_ptr_access = text_buffer_begin_data_ptr_access,
	.end_data_ptr_access = text_buffer_end_data_ptr_access,
};

struct asteroidz_icon_node *
asteroidz_icon_node_create(struct wlr_scene_tree *parent) {
	struct asteroidz_icon_node *node = calloc(1, sizeof(*node));
	if (!node)
		return NULL;
	node->scene_buffer = wlr_scene_buffer_create(parent, NULL);
	if (!node->scene_buffer) {
		free(node);
		return NULL;
	}
	node->size = -1;
	return node;
}

bool asteroidz_icon_node_set(struct asteroidz_icon_node *node,
							 const char *icon_name, int32_t size) {
	if (!node || !icon_name || !*icon_name || size < 1)
		return false;

	/* already rendered at this name/size */
	if (node->buffer && node->size == size && node->cached_name &&
		strcmp(node->cached_name, icon_name) == 0)
		return true;

	cairo_surface_t *icon = get_cached_icon(icon_name);
	if (!icon)
		return false;
	int iw = cairo_image_surface_get_width(icon);
	int ih = cairo_image_surface_get_height(icon);
	if (iw < 1 || ih < 1)
		return false;

	cairo_surface_t *new_surface =
		cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
	if (cairo_surface_status(new_surface) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(new_surface);
		return false;
	}

	cairo_t *cr = cairo_create(new_surface);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.0);
	cairo_paint(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	cairo_scale(cr, (double)size / iw, (double)size / ih);
	cairo_set_source_surface(cr, icon, 0, 0);
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
	cairo_paint(cr);
	cairo_surface_flush(new_surface);
	cairo_destroy(cr);

	struct asteroidz_text_buffer *buf = calloc(1, sizeof(*buf));
	if (!buf) {
		cairo_surface_destroy(new_surface);
		return false;
	}
	wlr_buffer_init(&buf->base, &text_buffer_impl, size, size);
	/* the buffer takes its OWN reference: new_surface is also stored in
	 * node->surface below, and that one is destroyed on the next update. A
	 * wrapper the scene is still holding (wlr_buffer_drop only releases our
	 * reference -- the renderer can keep it alive) would otherwise be left
	 * with a dangling buf->surface. */
	buf->surface = cairo_surface_reference(new_surface);

	/* attach the new buffer before dropping/destroying the old one -- unlike
	 * every other node type in this file, this path used to destroy
	 * node->surface (line freed here) while node->buffer (still the scene's
	 * live buffer) held the very same pointer via its own ->surface field,
	 * so a re-texture of the still-attached old buffer could read freed
	 * cairo surface memory. */
	wlr_scene_buffer_set_buffer(node->scene_buffer, &buf->base);
	if (node->buffer)
		wlr_buffer_drop(&node->buffer->base);
	if (node->surface)
		cairo_surface_destroy(node->surface);
	node->buffer = buf;
	node->surface = new_surface;
	wlr_scene_buffer_set_dest_size(node->scene_buffer, size, size);

	g_free(node->cached_name);
	node->cached_name = g_strdup(icon_name);
	node->size = size;
	return true;
}

void asteroidz_icon_node_destroy(struct asteroidz_icon_node *node) {
	if (!node)
		return;
	if (node->buffer) {
		wlr_buffer_drop(&node->buffer->base);
		node->buffer = NULL;
	}
	if (node->surface) {
		cairo_surface_destroy(node->surface);
		node->surface = NULL;
	}
	if (node->scene_buffer)
		wlr_scene_node_destroy(&node->scene_buffer->node);
	g_free(node->cached_name);
	free(node);
}

struct asteroidz_jump_label_node *
asteroidz_jump_label_node_create(struct wlr_scene_tree *parent,
							 AsteroidzTheme data) {
	struct asteroidz_jump_label_node *node = calloc(1, sizeof(*node));
	if (!node)
		return NULL;

	node->scene_buffer = wlr_scene_buffer_create(parent, NULL);
	if (!node->scene_buffer) {
		free(node);
		return NULL;
	}

	memcpy(node->fg_color, data.fg_color, sizeof(node->fg_color));
	memcpy(node->bg_color, data.bg_color, sizeof(node->bg_color));
	memcpy(node->focus_fg_color, data.focus_fg_color,
		   sizeof(node->focus_fg_color));
	memcpy(node->focus_bg_color, data.focus_bg_color,
		   sizeof(node->focus_bg_color));
	memcpy(node->border_color, data.border_color, sizeof(node->border_color));
	node->border_width = data.border_width;
	node->corner_radius = data.corner_radius;
	node->padding_x = data.padding_x;
	node->padding_y = data.padding_y;
	node->font_desc =
		g_strdup(data.font_desc ? data.font_desc : "monospace Bold 16");

	node->cached_text = NULL;
	node->cached_scale = -1.0f;
	node->cached_font_desc = NULL;
	node->focused = false;
	node->cached_focused = false;

	node->measure_surface =
		cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
	node->measure_cr = cairo_create(node->measure_surface);
	node->measure_context = pango_cairo_create_context(node->measure_cr);
	pango_cairo_context_set_font_options(node->measure_context,
										 asteroidz_font_options());
	node->measure_layout = pango_layout_new(node->measure_context);
	node->measure_scale = 1.0f;

	node->scene_buffer->node.data = NULL;

	return node;
}

void asteroidz_jump_label_node_destroy(struct asteroidz_jump_label_node *node) {
	if (!node)
		return;

	if (node->buffer) {
		wlr_buffer_drop(&node->buffer->base);
		node->buffer = NULL;
	}

	if (node->surface) {
		cairo_surface_destroy(node->surface);
		node->surface = NULL;
	}

	if (node->measure_layout)
		g_object_unref(node->measure_layout);
	if (node->measure_context)
		g_object_unref(node->measure_context);
	if (node->measure_cr)
		cairo_destroy(node->measure_cr);
	if (node->measure_surface)
		cairo_surface_destroy(node->measure_surface);

	wlr_scene_node_destroy(&node->scene_buffer->node);

	g_free(node->font_desc);
	g_free(node->cached_text);
	g_free(node->cached_font_desc);

	free(node);
}

void asteroidz_jump_label_node_set_background(struct asteroidz_jump_label_node *node,
										  float r, float g, float b, float a) {
	if (!node)
		return;
	node->bg_color[0] = r;
	node->bg_color[1] = g;
	node->bg_color[2] = b;
	node->bg_color[3] = a;
}

void asteroidz_jump_label_node_set_border(struct asteroidz_jump_label_node *node,
									  float r, float g, float b, float a,
									  int32_t width, int32_t radius) {
	if (!node)
		return;
	node->border_color[0] = r;
	node->border_color[1] = g;
	node->border_color[2] = b;
	node->border_color[3] = a;
	node->border_width = width > 0 ? width : 0;
	node->corner_radius = radius;
}

void asteroidz_jump_label_node_set_padding(struct asteroidz_jump_label_node *node,
									   int32_t pad_x, int32_t pad_y) {
	if (!node)
		return;
	node->padding_x = pad_x >= 0 ? pad_x : 0;
	node->padding_y = pad_y >= 0 ? pad_y : 0;
}

static void get_text_pixel_size(struct asteroidz_jump_label_node *node,
								const char *text, float scale, int32_t *out_w,
								int32_t *out_h) {
	if (node->measure_scale != scale) {
		pango_cairo_context_set_resolution(node->measure_context, 96.0 * scale);
		node->measure_scale = scale;
	}

	PangoFontDescription *desc = get_cached_font_desc(node->font_desc);
	pango_layout_set_font_description(node->measure_layout, desc);
	/* Measured the same way it is drawn, or the box is sized for the markup's
	 * tags and comes out far too wide. */
	if (node->markup) {
		pango_layout_set_markup(node->measure_layout, text, -1);
	} else {
		pango_layout_set_text(node->measure_layout, text, -1);
	}

	pango_layout_get_pixel_size(node->measure_layout, out_w, out_h);
}

static void draw_rounded_rect(cairo_t *cr, double x, double y, double w,
							  double h, double r) {
	double degrees = G_PI / 180.0;
	cairo_new_sub_path(cr);
	cairo_arc(cr, x + w - r, y + r, r, -90 * degrees, 0 * degrees);
	cairo_arc(cr, x + w - r, y + h - r, r, 0 * degrees, 90 * degrees);
	cairo_arc(cr, x + r, y + h - r, r, 90 * degrees, 180 * degrees);
	cairo_arc(cr, x + r, y + r, r, 180 * degrees, 270 * degrees);
	cairo_close_path(cr);
}

/* like draw_rounded_rect, but only rounds the corners set in `mask`; masked-out
 * corners are drawn square. Used for titlebar segments, where two adjacent
 * pills (close button, title tab) should only round their outermost corner. */
static void draw_rounded_rect_masked(cairo_t *cr, double x, double y, double w,
									 double h, double r,
									 enum corner_location mask) {
	double degrees = G_PI / 180.0;
	double tr = (mask & CORNER_LOCATION_TOP_RIGHT) ? r : 0;
	double br = (mask & CORNER_LOCATION_BOTTOM_RIGHT) ? r : 0;
	double bl = (mask & CORNER_LOCATION_BOTTOM_LEFT) ? r : 0;
	double tl = (mask & CORNER_LOCATION_TOP_LEFT) ? r : 0;

	cairo_new_sub_path(cr);
	cairo_arc(cr, x + w - tr, y + tr, tr, -90 * degrees, 0 * degrees);
	cairo_arc(cr, x + w - br, y + h - br, br, 0 * degrees, 90 * degrees);
	cairo_arc(cr, x + bl, y + h - bl, bl, 90 * degrees, 180 * degrees);
	cairo_arc(cr, x + tl, y + tl, tl, 180 * degrees, 270 * degrees);
	cairo_close_path(cr);
}

/* Build an OPEN path tracing the top edge plus (optionally) the left and/or
 * right edges of a (possibly top-rounded) box -- never the bottom -- inset
 * by lw/2 so a stroke of width lw sits fully inside [x,y,w,h]. Used for
 * titlebar pills: the bottom is flush against the window, and adjacent
 * pills omit the border on their touching internal seam via the
 * draw_left/draw_right flags. */
static void draw_titlebar_border_path(cairo_t *cr, double x, double y, double w,
									  double h, double r,
									  enum corner_location mask, double lw,
									  bool draw_left, bool draw_right) {
	double degrees = G_PI / 180.0;
	double hw = lw * 0.5;
	double top = y + hw;
	double bottom = y + h; /* bottom edge is open, so not inset */
	double left_edge = x + hw;
	double right_edge = x + w - hw;
	double rr = r - hw;
	if (rr < 0.0)
		rr = 0.0;
	/* only round a top corner if its edge is actually drawn */
	double tl = (draw_left && (mask & CORNER_LOCATION_TOP_LEFT)) ? rr : 0;
	double tr = (draw_right && (mask & CORNER_LOCATION_TOP_RIGHT)) ? rr : 0;
	/* the top edge insets by hw only on a side that has its own vertical
	 * border; on an unbordered (internal-seam) side it runs to the full
	 * extent so it meets the neighbouring segment's top edge with no gap. */
	double top_left = draw_left ? left_edge : x;
	double top_right = draw_right ? right_edge : x + w;

	cairo_new_path(cr);
	if (draw_left) {
		cairo_move_to(cr, left_edge, bottom);
		cairo_line_to(cr, left_edge, top + tl);
		if (tl > 0.0)
			cairo_arc(cr, left_edge + tl, top + tl, tl, 180 * degrees,
					  270 * degrees);
	} else {
		cairo_move_to(cr, top_left, top);
	}
	cairo_line_to(cr, top_right - tr, top);
	if (draw_right) {
		if (tr > 0.0)
			cairo_arc(cr, right_edge - tr, top + tr, tr, -90 * degrees,
					  0 * degrees);
		cairo_line_to(cr, right_edge, bottom);
	}
}

void asteroidz_jump_label_node_set_markup(
		struct asteroidz_jump_label_node *node, bool markup) {
	if (!node || node->markup == markup) {
		return;
	}
	node->markup = markup;
	/* The cached text was measured and drawn under the old rule, so the next
	 * update must not take the "same text, nothing to do" early return. */
	g_free(node->cached_text);
	node->cached_text = NULL;
}

void asteroidz_jump_label_node_update(struct asteroidz_jump_label_node *node,
								  const char *text, float scale) {
	if (!node || !text)
		return;
	if (scale <= 0.0f)
		scale = 1.0f;

	/* dirty check, includes focused state */
	if (node->cached_scale == scale && node->cached_font_desc &&
		strcmp(node->cached_font_desc, node->font_desc) == 0 &&
		node->cached_text && strcmp(node->cached_text, text) == 0 &&
		memcmp(node->cached_fg_color, node->fg_color, sizeof(node->fg_color)) ==
			0 &&
		memcmp(node->cached_bg_color, node->bg_color, sizeof(node->bg_color)) ==
			0 &&
		memcmp(node->cached_focus_fg_color, node->focus_fg_color,
			   sizeof(node->focus_fg_color)) == 0 &&
		memcmp(node->cached_focus_bg_color, node->focus_bg_color,
			   sizeof(node->focus_bg_color)) == 0 &&
		memcmp(node->cached_border_color, node->border_color,
			   sizeof(node->border_color)) == 0 &&
		node->cached_border_width == node->border_width &&
		node->cached_corner_radius == node->corner_radius &&
		node->cached_padding_x == node->padding_x &&
		node->cached_padding_y == node->padding_y &&
		node->cached_focused == node->focused) {
		return;
	}

	/* update cache */
	/* asteroidz_jump_label_node_set_focus() re-enters here passing
	 * node->cached_text ITSELF as `text`, so the old cache must not be freed
	 * until the copy exists -- otherwise g_strdup() reads the pointer that was
	 * just freed. Confirmed live under ASAN: heap-use-after-free in g_strdup
	 * via asteroidz_jump_label_node_update <- ..._set_focus <-
	 * overview_draw_cell_label. */
	char *new_cached_text = g_strdup(text);
	g_free(node->cached_text);
	node->cached_text = new_cached_text;
	/* ...and `text` is now dangling for exactly the same reason: the g_free
	 * above released the buffer it aliases. Guarding only the g_strdup left
	 * every LATER read of `text` pointing at freed memory -- including the
	 * pango_layout_set_text() that draws the label, which rendered whatever
	 * the allocator had put there: tag pills full of tofu and stray letters
	 * in the overview, on every cell whose focus state had just changed
	 * (i.e. the ones that are not the current tag) while the current tag,
	 * whose focus was unchanged and so returned early above, drew correctly.
	 *
	 * Re-point at the copy that is guaranteed live. Same string either way
	 * when the caller passed its own buffer; the difference only matters on
	 * the aliasing path, which is the one that was broken. */
	text = node->cached_text;
	g_free(node->cached_font_desc);
	node->cached_font_desc = g_strdup(node->font_desc);
	node->cached_scale = scale;
	memcpy(node->cached_fg_color, node->fg_color, sizeof(node->fg_color));
	memcpy(node->cached_bg_color, node->bg_color, sizeof(node->bg_color));
	memcpy(node->cached_focus_fg_color, node->focus_fg_color,
		   sizeof(node->focus_fg_color));
	memcpy(node->cached_focus_bg_color, node->focus_bg_color,
		   sizeof(node->focus_bg_color));
	memcpy(node->cached_border_color, node->border_color,
		   sizeof(node->border_color));
	node->cached_border_width = node->border_width;
	node->cached_corner_radius = node->corner_radius;
	node->cached_padding_x = node->padding_x;
	node->cached_padding_y = node->padding_y;
	node->cached_focused = node->focused;

	int32_t text_pixel_w, text_pixel_h;
	get_text_pixel_size(node, text, scale, &text_pixel_w, &text_pixel_h);

	if (text_pixel_w <= 0 || text_pixel_h <= 0) {
		wlr_scene_buffer_set_buffer(node->scene_buffer, NULL);
		if (node->buffer) {
			wlr_buffer_drop(&node->buffer->base);
			node->buffer = NULL;
		}
		if (node->surface) {
			cairo_surface_destroy(node->surface);
			node->surface = NULL;
		}
		node->logical_width = 0;
		node->logical_height = 0;
		wlr_scene_buffer_set_dest_size(node->scene_buffer, 0, 0);
		return;
	}

	int32_t logical_text_w = (int32_t)(text_pixel_w / scale + 0.5f);
	int32_t logical_text_h = (int32_t)(text_pixel_h / scale + 0.5f);
	int32_t box_logical_w = logical_text_w + 2 * node->padding_x;
	int32_t box_logical_h = logical_text_h + 2 * node->padding_y;

	int32_t required_pixel_w =
		(int32_t)((box_logical_w + 2 * node->border_width) * scale + 0.5f);
	int32_t required_pixel_h =
		(int32_t)((box_logical_h + 2 * node->border_width) * scale + 0.5f);
	if (required_pixel_w < 1)
		required_pixel_w = 1;
	if (required_pixel_h < 1)
		required_pixel_h = 1;

	/* always detach + drop the old wrapper + destroy the old surface + create
	 * a fresh one, even when the pixel size didn't change -- this used to be
	 * gated behind a surface_size_changed check, reusing node->surface as-is
	 * (just redrawn in place) while still allocating a BRAND NEW wlr_buffer
	 * wrapper around that SAME surface pointer every call. That left two
	 * different wrapper "generations" sharing one surface object across a
	 * buffer-swap boundary -- exactly the kind of shared-mutable-state-
	 * across-generations pattern behind the buffer-swap-ordering bugs fixed
	 * elsewhere in this file. A live crash traced to this exact function
	 * (garbage data pointer in write_pixels, the crashed buffer's wlr_buffer
	 * showing dropped=true while still the scene's CURRENT buffer -- a state
	 * that's impossible under a single, non-overlapping call to this
	 * function) is consistent with two update calls landing close enough
	 * together to corrupt each other's node->buffer bookkeeping via that
	 * shared surface. Pairing a fresh surface with every fresh wrapper,
	 * always, removes the sharing entirely regardless of the exact trigger.
	 * Costs one extra small cairo_image_surface_create per redraw (this
	 * path already does one for icons; jump labels/tab bars are small). */
	wlr_scene_buffer_set_buffer(node->scene_buffer, NULL);
	if (node->buffer) {
		wlr_buffer_drop(&node->buffer->base);
		node->buffer = NULL;
	}
	if (node->surface) {
		cairo_surface_destroy(node->surface);
		node->surface = NULL;
	}

	node->surface = cairo_image_surface_create(
		CAIRO_FORMAT_ARGB32, required_pixel_w, required_pixel_h);
	node->surface_pixel_w = required_pixel_w;
	node->surface_pixel_h = required_pixel_h;

	cairo_t *cr = cairo_create(node->surface);

	cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.0);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_paint(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

	double border = node->border_width * scale;
	double bg_x = border;
	double bg_y = border;
	double bg_w = box_logical_w * scale;
	double bg_h = box_logical_h * scale;

	double radius;
	if (node->corner_radius < 0) {
		radius = (bg_w < bg_h ? bg_w : bg_h) / 2.0;
	} else {
		radius = node->corner_radius * scale;
	}
	if (radius > bg_w / 2.0)
		radius = bg_w / 2.0;
	if (radius > bg_h / 2.0)
		radius = bg_h / 2.0;

	const float *active_bg =
		node->focused ? node->focus_bg_color : node->bg_color;
	const float *active_fg =
		node->focused ? node->focus_fg_color : node->fg_color;

	bool draw_bg = (active_bg[3] > 0.0f); // use active_bg
	bool draw_border =
		(node->border_width > 0) && (node->border_color[3] > 0.0f);

	if (draw_bg) {
		cairo_set_source_rgba(cr, active_bg[0], active_bg[1], active_bg[2],
							  active_bg[3]);
		if (radius > 0.0) {
			draw_rounded_rect(cr, bg_x, bg_y, bg_w, bg_h, radius);
			cairo_fill(cr);
		} else {
			cairo_rectangle(cr, bg_x, bg_y, bg_w, bg_h);
			cairo_fill(cr);
		}
	}

	cairo_save(cr);
	double text_x = (node->border_width + node->padding_x) * scale;
	double text_y = (node->border_width + node->padding_y) * scale;
	cairo_translate(cr, text_x, text_y);

	PangoContext *ctx = pango_cairo_create_context(cr);
	pango_cairo_context_set_font_options(ctx, asteroidz_font_options());
	pango_cairo_context_set_resolution(ctx, 96.0 * scale);
	PangoLayout *layout = pango_layout_new(ctx);
	PangoFontDescription *desc = get_cached_font_desc(node->font_desc);
	pango_layout_set_font_description(layout, desc);
	if (node->markup) {
		pango_layout_set_markup(layout, text, -1);
	} else {
		pango_layout_set_text(layout, text, -1);
	}

	cairo_set_source_rgba(cr, active_fg[0], active_fg[1], active_fg[2],
						  active_fg[3]);
	pango_cairo_show_layout(cr, layout);

	g_object_unref(layout);
	g_object_unref(ctx);
	cairo_restore(cr);

	if (draw_border) {
		cairo_set_source_rgba(cr, node->border_color[0], node->border_color[1],
							  node->border_color[2], node->border_color[3]);
		cairo_set_line_width(cr, border);

		double half_lw = border * 0.5;
		double bx = bg_x - half_lw;
		double by = bg_y - half_lw;
		double bw = bg_w + border;
		double bh = bg_h + border;

		if (radius > 0.0) {
			double outer_radius = radius + half_lw;
			if (outer_radius < 0.0)
				outer_radius = 0.0;
			draw_rounded_rect(cr, bx, by, bw, bh, outer_radius);
		} else {
			cairo_rectangle(cr, bx, by, bw, bh);
		}
		cairo_stroke(cr);
	}

	cairo_surface_flush(node->surface);
	cairo_destroy(cr);

	struct asteroidz_text_buffer *buf = calloc(1, sizeof(*buf));
	if (!buf) {
		return;
	}
	wlr_buffer_init(&buf->base, &text_buffer_impl, node->surface_pixel_w,
					node->surface_pixel_h);
	/* own a reference rather than borrowing node->surface -- see the matching
	 * comment in asteroidz_icon_node_set and in text_buffer_destroy. */
	buf->surface = cairo_surface_reference(node->surface);

	/* attach the new buffer before dropping the old one -- the scene node
	 * must never be left pointing at an already-freed wlr_buffer, even
	 * momentarily (see the matching fix + comment in
	 * asteroidz_icon_node_set for the exact same bug). */
	wlr_scene_buffer_set_buffer(node->scene_buffer, &buf->base);
	if (node->buffer)
		wlr_buffer_drop(&node->buffer->base);
	node->buffer = buf;

	node->logical_width = box_logical_w + 2 * node->border_width;
	node->logical_height = box_logical_h + 2 * node->border_width;
	wlr_scene_buffer_set_dest_size(node->scene_buffer, node->logical_width,
								   node->logical_height);
}

void asteroidz_jump_label_node_set_focus(struct asteroidz_jump_label_node *node,
									 bool focused) {
	if (!node || node->focused == focused)
		return;
	node->focused = focused;
	// trigger a redraw using the cached text and scale (skip if no text)
	if (node->cached_text && node->cached_scale > 0.0f) {
		asteroidz_jump_label_node_update(node, node->cached_text,
									 node->cached_scale);
	}
}

struct asteroidz_tab_bar_node *
asteroidz_tab_bar_node_create(void *asteroidz_node_data, struct wlr_scene_tree *parent,
						  AsteroidzTheme data, int32_t width,
						  int32_t height) {
	struct asteroidz_tab_bar_node *node = calloc(1, sizeof(*node));
	if (!node)
		return NULL;

	node->scene_buffer = wlr_scene_buffer_create(parent, NULL);
	if (!node->scene_buffer) {
		free(node);
		return NULL;
	}
	node->parent_tree = parent;

	memcpy(node->fg_color, data.fg_color, sizeof(node->fg_color));
	memcpy(node->bg_color, data.bg_color, sizeof(node->bg_color));
	memcpy(node->focus_fg_color, data.focus_fg_color,
		   sizeof(node->focus_fg_color));
	memcpy(node->focus_bg_color, data.focus_bg_color,
		   sizeof(node->focus_bg_color));
	memcpy(node->border_color, data.border_color, sizeof(node->border_color));
	node->border_width = data.border_width;
	node->corner_radius = data.corner_radius;
	node->padding_x = data.padding_x;
	node->padding_y = data.padding_y;
	node->icon_scale = 1.0;
	node->cached_icon_scale = 1.0;
	node->font_desc =
		g_strdup(data.font_desc ? data.font_desc : "monospace Bold 16");

	node->target_width = width;
	node->target_height = height;
	node->focused = false;
	node->cached_focused = false;
	node->corner_mask = CORNER_LOCATION_ALL;
	node->text_align_left = false;

	node->measure_surface =
		cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
	node->measure_cr = cairo_create(node->measure_surface);
	node->measure_context = pango_cairo_create_context(node->measure_cr);
	pango_cairo_context_set_font_options(node->measure_context,
										 asteroidz_font_options());
	node->measure_layout = pango_layout_new(node->measure_context);
	node->measure_scale = 1.0f;

	node->cached_scale = -1.0f;
	node->content_scale = 1.0f;
	node->cached_content_scale = -1.0f;
	node->last_text = NULL;
	node->last_scale = 0.0f;
	node->scene_buffer->node.data = asteroidz_node_data;

	return node;
}

static void tab_bar_shadow_sync(struct asteroidz_tab_bar_node *node) {
	if (!node->shadow)
		return;
	int32_t pad = (int32_t)ceilf(node->shadow_sigma * 0.5f);
	int32_t radius = node->corner_radius < 0 ? node->target_height / 2
											 : node->corner_radius;
	int32_t sh_w = node->target_width + 2 * pad;
	int32_t sh_h = node->target_height + 2 * pad;
	int32_t sh_y = node->last_y - pad + node->shadow_offset_y;
	wlr_scene_shadow_set_size(node->shadow, sh_w, sh_h);
	wlr_scene_shadow_set_corner_radius(node->shadow, radius + pad);
	wlr_scene_node_set_position(&node->shadow->node, node->last_x - pad, sh_y);
	/* the tab sits flush on the window's top edge; per-window tabs live
	 * INSIDE the client scene (above the surface), so clip away the part of
	 * the shadow that would extend below the tab's bottom edge and paint a
	 * dark band across the window content. */
	int32_t below = (sh_y + sh_h) - (node->last_y + node->target_height);
	struct clipped_region clip = {0};
	if (below > 0) {
		clip.area = (struct wlr_box){
			.x = 0,
			.y = sh_h - below,
			.width = sh_w,
			.height = below,
		};
	}
	wlr_scene_shadow_set_clipped_region(node->shadow, clip);
	wlr_scene_node_set_enabled(&node->shadow->node,
							   node->scene_buffer->node.enabled &&
								   node->target_width > 0 &&
								   node->target_height > 0);
}

void asteroidz_tab_bar_node_set_enabled(struct asteroidz_tab_bar_node *node,
									bool enabled) {
	if (!node)
		return;
	wlr_scene_node_set_enabled(&node->scene_buffer->node, enabled);
	if (node->shadow)
		wlr_scene_node_set_enabled(&node->shadow->node,
								   enabled && node->target_width > 0 &&
									   node->target_height > 0);
}

void asteroidz_tab_bar_node_set_position(struct asteroidz_tab_bar_node *node,
									 int32_t x, int32_t y) {
	if (!node)
		return;
	node->last_x = x;
	node->last_y = y;
	wlr_scene_node_set_position(&node->scene_buffer->node, x, y);
	tab_bar_shadow_sync(node);
}

void asteroidz_tab_bar_node_set_shadow(struct asteroidz_tab_bar_node *node,
								   bool enabled, float sigma, int32_t offset_y,
								   const float color[4]) {
	if (!node)
		return;
	if (!enabled || sigma <= 0.0f) {
		if (node->shadow) {
			wlr_scene_node_destroy(&node->shadow->node);
			node->shadow = NULL;
		}
		return;
	}
	node->shadow_sigma = sigma;
	node->shadow_offset_y = offset_y;
	memcpy(node->shadow_color, color, sizeof(node->shadow_color));
	if (!node->shadow) {
		node->shadow = wlr_scene_shadow_create(node->parent_tree, 0, 0, 0,
											   sigma, color);
		if (!node->shadow)
			return;
		wlr_scene_node_place_below(&node->shadow->node,
								   &node->scene_buffer->node);
	} else {
		wlr_scene_shadow_set_blur_sigma(node->shadow, sigma);
		wlr_scene_shadow_set_color(node->shadow, color);
	}
	tab_bar_shadow_sync(node);
}

void asteroidz_tab_bar_node_set_icons(struct asteroidz_tab_bar_node *node,
								  const char *const *icon_names,
								  int32_t count) {
	if (!node)
		return;
	cairo_surface_t *next[ASTEROIDZ_TAB_MAX_ICONS] = {NULL};
	int32_t n = 0;
	for (int32_t i = 0; i < count && n < ASTEROIDZ_TAB_MAX_ICONS; i++) {
		if (!icon_names[i] || !*icon_names[i])
			continue;
		cairo_surface_t *surf = get_cached_icon(icon_names[i]);
		if (surf)
			next[n++] = surf;
	}

	bool same = n == node->nicons;
	for (int32_t i = 0; same && i < n; i++)
		same = next[i] == node->icons[i];
	if (same)
		return;

	/* the icon cache hands out a borrowed pointer shared across every node
	 * using that icon name; take our own reference so clearing/replacing
	 * the cache (icon theme change) can't leave this node with a dangling
	 * pointer to a surface it doesn't actually own. */
	for (int32_t i = 0; i < node->nicons; i++) {
		if (node->icons[i])
			cairo_surface_destroy(node->icons[i]);
		node->icons[i] = NULL;
	}
	for (int32_t i = 0; i < n; i++)
		node->icons[i] = cairo_surface_reference(next[i]);
	node->nicons = n;

	if (node->last_text)
		asteroidz_tab_bar_node_update(node, node->last_text,
								  node->last_scale > 0 ? node->last_scale
													   : 1.0f);
}

void asteroidz_tab_bar_node_set_icon(struct asteroidz_tab_bar_node *node,
								 const char *icon_name) {
	const char *one[1] = {icon_name};
	asteroidz_tab_bar_node_set_icons(node, one, icon_name && *icon_name ? 1 : 0);
}

void asteroidz_tab_bar_node_set_icons_after_text(
	struct asteroidz_tab_bar_node *node, bool after) {
	if (!node || node->icons_after_text == after)
		return;
	node->icons_after_text = after;
	if (node->last_text)
		asteroidz_tab_bar_node_update(node, node->last_text,
								  node->last_scale > 0 ? node->last_scale
													   : 1.0f);
}

void asteroidz_tab_bar_node_set_icon_scale(struct asteroidz_tab_bar_node *node,
									   double scale) {
	if (!node)
		return;
	/* Below a quarter the icon is a smudge, above 1 it spills out of the pill
	 * it is measured against. */
	if (scale < 0.25)
		scale = 0.25;
	if (scale > 1.0)
		scale = 1.0;
	if (node->icon_scale == scale)
		return;
	node->icon_scale = scale;
	if (node->last_text)
		asteroidz_tab_bar_node_update(node, node->last_text,
								  node->last_scale > 0 ? node->last_scale
													   : 1.0f);
}

void asteroidz_tab_bar_node_set_icon_tint(struct asteroidz_tab_bar_node *node,
									  const float rgba[4]) {
	if (!node)
		return;
	bool want = rgba != NULL;
	if (node->icon_tinted == want &&
		(!want || memcmp(node->icon_tint, rgba, sizeof(node->icon_tint)) == 0))
		return;
	node->icon_tinted = want;
	if (want)
		memcpy(node->icon_tint, rgba, sizeof(node->icon_tint));
	if (node->last_text)
		asteroidz_tab_bar_node_update(node, node->last_text,
								  node->last_scale > 0 ? node->last_scale
													   : 1.0f);
}

void asteroidz_tab_bar_node_set_corner_mask(struct asteroidz_tab_bar_node *node,
										enum corner_location mask) {
	if (!node || node->corner_mask == mask)
		return;
	node->corner_mask = mask;
	if (node->last_text)
		asteroidz_tab_bar_node_update(node, node->last_text,
								  node->last_scale > 0 ? node->last_scale
													   : 1.0f);
}

void asteroidz_tab_bar_node_set_text_align_left(struct asteroidz_tab_bar_node *node,
											bool align_left) {
	if (!node || node->text_align_left == align_left)
		return;
	node->text_align_left = align_left;
	if (node->last_text)
		asteroidz_tab_bar_node_update(node, node->last_text,
								  node->last_scale > 0 ? node->last_scale
													   : 1.0f);
}

void asteroidz_tab_bar_node_reparent(struct asteroidz_tab_bar_node *node,
								 struct wlr_scene_tree *parent) {
	if (!node || !parent || node->parent_tree == parent)
		return;
	node->parent_tree = parent;
	wlr_scene_node_reparent(&node->scene_buffer->node, parent);
	if (node->shadow) {
		wlr_scene_node_reparent(&node->shadow->node, parent);
		wlr_scene_node_place_below(&node->shadow->node,
								   &node->scene_buffer->node);
	}
}

void asteroidz_tab_bar_node_set_padding(struct asteroidz_tab_bar_node *node,
									int32_t padding_x, int32_t padding_y) {
	if (!node)
		return;
	padding_x = padding_x >= 0 ? padding_x : 0;
	padding_y = padding_y >= 0 ? padding_y : 0;
	if (node->padding_x == padding_x && node->padding_y == padding_y)
		return;
	node->padding_x = padding_x;
	node->padding_y = padding_y;
	if (node->last_text)
		asteroidz_tab_bar_node_update(node, node->last_text,
								  node->last_scale > 0 ? node->last_scale
													   : 1.0f);
}

void asteroidz_tab_bar_node_set_content_scale(struct asteroidz_tab_bar_node *node,
										  float content_scale) {
	if (!node)
		return;
	if (content_scale <= 0.0f)
		content_scale = 1.0f;
	if (node->content_scale == content_scale)
		return;
	node->content_scale = content_scale;
	if (node->last_text)
		asteroidz_tab_bar_node_update(node, node->last_text,
								  node->last_scale > 0 ? node->last_scale
													   : 1.0f);
}

void asteroidz_tab_bar_node_set_titlebar_border(struct asteroidz_tab_bar_node *node,
											int32_t width, bool border_left,
											bool border_right) {
	if (!node)
		return;
	if (width < 0)
		width = 0;
	if (node->titlebar_border_width == width &&
		node->titlebar_border_left == border_left &&
		node->titlebar_border_right == border_right)
		return;
	node->titlebar_border_width = width;
	node->titlebar_border_left = border_left;
	node->titlebar_border_right = border_right;
	if (node->last_text)
		asteroidz_tab_bar_node_update(node, node->last_text,
								  node->last_scale > 0 ? node->last_scale
													   : 1.0f);
}

void asteroidz_tab_bar_node_set_titlebar_separator(
	struct asteroidz_tab_bar_node *node, bool separator_right) {
	if (!node || node->titlebar_separator_right == separator_right)
		return;
	node->titlebar_separator_right = separator_right;
	if (node->last_text)
		asteroidz_tab_bar_node_update(node, node->last_text,
								  node->last_scale > 0 ? node->last_scale
													   : 1.0f);
}

void asteroidz_tab_bar_node_destroy(struct asteroidz_tab_bar_node *node) {
	if (!node)
		return;

	for (int32_t i = 0; i < node->nicons; i++) {
		if (node->icons[i])
			cairo_surface_destroy(node->icons[i]);
		node->icons[i] = NULL;
	}
	node->nicons = 0;

	if (node->buffer) {
		wlr_buffer_drop(&node->buffer->base);
		node->buffer = NULL;
	}

	if (node->surface) {
		cairo_surface_destroy(node->surface);
		node->surface = NULL;
	}
	if (node->measure_surface) {
		cairo_surface_destroy(node->measure_surface);
		node->measure_surface = NULL;
	}

	if (node->measure_layout)
		g_object_unref(node->measure_layout);
	if (node->measure_context)
		g_object_unref(node->measure_context);
	if (node->measure_cr)
		cairo_destroy(node->measure_cr);

	void *data = node->scene_buffer->node.data;
	if (node->shadow)
		wlr_scene_node_destroy(&node->shadow->node);
	wlr_scene_node_destroy(&node->scene_buffer->node);

	g_free(node->font_desc);
	g_free(node->cached_text);
	g_free(node->cached_font_desc);
	g_free(node->last_text);
	free(data);
	free(node);
}

void asteroidz_tab_bar_node_set_size(struct asteroidz_tab_bar_node *node, int32_t width,
								 int32_t height) {
	if (!node)
		return;

	if (width < 0)
		width = 0;
	if (height < 0)
		height = 0;

	if (node->target_width == width && node->target_height == height) {
		tab_bar_shadow_sync(node);
		return;
	}

	node->target_width = width;
	node->target_height = height;
	tab_bar_shadow_sync(node);

	const char *redraw_text = node->last_text ? node->last_text : "";
	float redraw_scale = node->last_scale > 0.0f ? node->last_scale : 1.0f;

	asteroidz_tab_bar_node_update(node, redraw_text, redraw_scale);
}

static bool bar_icons_unchanged(const struct asteroidz_tab_bar_node *node) {
	if (node->cached_nicons != node->nicons ||
		node->cached_icons_after_text != node->icons_after_text ||
		node->cached_icon_scale != node->icon_scale ||
		node->cached_icon_tinted != node->icon_tinted ||
		(node->icon_tinted &&
		 memcmp(node->cached_icon_tint, node->icon_tint,
				sizeof(node->icon_tint)) != 0))
		return false;
	for (int32_t i = 0; i < node->nicons; i++)
		if (node->cached_icons[i] != node->icons[i])
			return false;
	return true;
}

int32_t asteroidz_tab_bar_node_measure_width(struct asteroidz_tab_bar_node *node,
										 const char *text, int32_t height) {
	if (!node || !text || height <= 0)
		return 0;

	/* Mirror the geometry the draw path derives in _update(), but in logical
	 * units at scale 1.0 -- the caller sizes in logical pixels and the
	 * HiDPI scale is applied later, at render time. Keep the two in step:
	 * if the padding/icon-gap arithmetic there changes, it changes here. */
	float cs = node->content_scale > 0.0f ? node->content_scale : 1.0f;
	double pad_x = node->padding_x * cs;
	double pad_y = node->padding_y * cs;
	int32_t box_logical_h = height - 2 * node->border_width;
	int32_t text_area_h = (int32_t)(box_logical_h - 2.0 * pad_y);
	if (text_area_h < 0)
		text_area_h = 0;

	if (node->measure_scale != 1.0f) {
		pango_cairo_context_set_resolution(node->measure_context, 96.0);
		node->measure_scale = 1.0f;
	}
	PangoFontDescription *desc = get_cached_font_desc(node->font_desc);
	PangoFontDescription *scaled_desc = NULL;
	if (cs != 1.0f) {
		scaled_desc = pango_font_description_copy(desc);
		int32_t fsz = pango_font_description_get_size(scaled_desc);
		if (fsz > 0)
			pango_font_description_set_size(scaled_desc,
											(int32_t)(fsz * cs + 0.5f));
		desc = scaled_desc;
	}
	pango_layout_set_font_description(node->measure_layout, desc);
	/* measure the UNCONSTRAINED width: -1 undoes any ellipsizing width a
	 * previous _update() left on this shared layout object. */
	pango_layout_set_width(node->measure_layout, -1);
	pango_layout_set_text(node->measure_layout, text, -1);
	int text_w = 0, text_h = 0;
	pango_layout_get_pixel_size(node->measure_layout, &text_w, &text_h);
	if (scaled_desc)
		pango_font_description_free(scaled_desc);

	/* Icons are fitted into the text area's height, with a 6px gap BETWEEN
	 * them and one more before the text -- but only when there is text. A
	 * trailing gap on an icon-only pill is dead space that no padding setting
	 * can remove, and on a row of icon-only status pills it read as the pills
	 * being spaced apart when they were not. */
	double icon_gap = 6.0 * cs;
	double icon_w = 0.0;
	if (node->nicons > 0) {
		double icon_px = text_area_h * node->icon_scale;
		for (int32_t i = 0; i < node->nicons; i++)
			icon_w += asteroidz_icon_advance(node->icons[i], icon_px);
		icon_w += (node->nicons - 1) * icon_gap + (*text ? icon_gap : 0.0);
	}

	/* One pixel of slack.
	 *
	 * This measures a LOGICAL width, but the pill is rendered into a surface
	 * of width*scale physical pixels and pango lays the text out there. On a
	 * fractional scale those two roundings do not agree: at scale 0.75 a pill
	 * pinned to its own measured width came up a pixel short and pango
	 * ellipsised, so "100%" rendered as "10...". A pill is free-floating on
	 * the bar and one extra pixel costs nothing; a truncated readout is a
	 * visible fault. */
	int32_t width = (int32_t)(2.0 * pad_x + icon_w + text_w + 0.5) +
					2 * node->border_width + 1;
	return width > 0 ? width : 0;
}

void asteroidz_tab_bar_node_update(struct asteroidz_tab_bar_node *node,
							   const char *text, float scale) {
	if (!node || !text)
		return;
	if (scale <= 0.0f)
		scale = 1.0f;

	/* This runs every animation tick for every animating window (via
	 * client_apply_clip -> client_draw_titlebar), and during a move the title
	 * text is unchanged. Only re-dup last_text when it actually changed: the
	 * old unconditional g_strdup + g_free was pure per-frame allocator churn
	 * in a hot path (the dirty check below already short-circuits the render). */
	if (!node->last_text || strcmp(node->last_text, text) != 0) {
		g_free(node->last_text);
		node->last_text = g_strdup(text);
	}
	node->last_scale = scale;

	// dirty check, includes focused
	if (node->cached_scale == scale && node->cached_font_desc &&
		strcmp(node->cached_font_desc, node->font_desc) == 0 &&
		node->cached_text && strcmp(node->cached_text, text) == 0 &&
		memcmp(node->cached_fg_color, node->fg_color, sizeof(node->fg_color)) ==
			0 &&
		memcmp(node->cached_bg_color, node->bg_color, sizeof(node->bg_color)) ==
			0 &&
		memcmp(node->cached_focus_fg_color, node->focus_fg_color,
			   sizeof(node->focus_fg_color)) == 0 &&
		memcmp(node->cached_focus_bg_color, node->focus_bg_color,
			   sizeof(node->focus_bg_color)) == 0 &&
		memcmp(node->cached_border_color, node->border_color,
			   sizeof(node->border_color)) == 0 &&
		node->cached_border_width == node->border_width &&
		node->cached_corner_radius == node->corner_radius &&
		node->cached_padding_x == node->padding_x &&
		node->cached_padding_y == node->padding_y &&
		node->cached_target_width == node->target_width &&
		node->cached_target_height == node->target_height &&
		bar_icons_unchanged(node) &&
		node->cached_corner_mask == node->corner_mask &&
		node->cached_titlebar_border_width == node->titlebar_border_width &&
		node->cached_titlebar_border_left == node->titlebar_border_left &&
		node->cached_titlebar_border_right == node->titlebar_border_right &&
		node->cached_titlebar_separator_right ==
			node->titlebar_separator_right &&
		node->cached_content_scale == node->content_scale &&
		node->cached_focused == node->focused) {
		return;
	}

	/* update cache
	 *
	 * Copy before freeing, and then read the copy. This node is not currently
	 * reachable with `text` aliasing cached_text -- its own re-entry path
	 * (set_focus and friends) passes last_text, which is a separate buffer and
	 * is deliberately not freed when the two alias. But the jump-label node
	 * next door was reachable that way and shipped a use-after-free twice
	 * because of this exact ordering, so don't leave the trap armed here. */
	char *new_cached_text = g_strdup(text);
	g_free(node->cached_text);
	node->cached_text = new_cached_text;
	text = node->cached_text;

	g_free(node->cached_font_desc);
	node->cached_font_desc = g_strdup(node->font_desc);
	node->cached_scale = scale;
	memcpy(node->cached_fg_color, node->fg_color, sizeof(node->fg_color));
	memcpy(node->cached_bg_color, node->bg_color, sizeof(node->bg_color));
	memcpy(node->cached_focus_fg_color, node->focus_fg_color,
		   sizeof(node->focus_fg_color));
	memcpy(node->cached_focus_bg_color, node->focus_bg_color,
		   sizeof(node->focus_bg_color));
	memcpy(node->cached_border_color, node->border_color,
		   sizeof(node->border_color));
	node->cached_border_width = node->border_width;
	node->cached_corner_radius = node->corner_radius;
	node->cached_padding_x = node->padding_x;
	node->cached_padding_y = node->padding_y;
	node->cached_target_width = node->target_width;
	node->cached_target_height = node->target_height;
	for (int32_t i = 0; i < ASTEROIDZ_TAB_MAX_ICONS; i++)
		node->cached_icons[i] = i < node->nicons ? node->icons[i] : NULL;
	node->cached_nicons = node->nicons;
	node->cached_icons_after_text = node->icons_after_text;
	node->cached_icon_scale = node->icon_scale;
	node->cached_icon_tinted = node->icon_tinted;
	memcpy(node->cached_icon_tint, node->icon_tint,
		   sizeof(node->cached_icon_tint));
	node->cached_corner_mask = node->corner_mask;
	node->cached_titlebar_border_width = node->titlebar_border_width;
	node->cached_titlebar_border_left = node->titlebar_border_left;
	node->cached_titlebar_border_right = node->titlebar_border_right;
	node->cached_titlebar_separator_right = node->titlebar_separator_right;
	node->cached_content_scale = node->content_scale;
	node->cached_focused = node->focused;

	if (node->target_width <= 0 || node->target_height <= 0) {
		wlr_scene_buffer_set_buffer(node->scene_buffer, NULL);
		if (node->buffer) {
			wlr_buffer_drop(&node->buffer->base);
			node->buffer = NULL;
		}
		if (node->surface) {
			cairo_surface_destroy(node->surface);
			node->surface = NULL;
		}
		node->logical_width = 0;
		node->logical_height = 0;
		wlr_scene_buffer_set_dest_size(node->scene_buffer, 0, 0);
		return;
	}

	int32_t box_logical_w = node->target_width - 2 * node->border_width;
	int32_t box_logical_h = node->target_height - 2 * node->border_width;
	if (box_logical_w < 0)
		box_logical_w = 0;
	if (box_logical_h < 0)
		box_logical_h = 0;

	int32_t required_pixel_w = (int32_t)(node->target_width * scale + 0.5f);
	int32_t required_pixel_h = (int32_t)(node->target_height * scale + 0.5f);
	if (required_pixel_w < 1)
		required_pixel_w = 1;
	if (required_pixel_h < 1)
		required_pixel_h = 1;

	/* always detach + drop the old wrapper + destroy the old surface + create
	 * a fresh one, even when the pixel size didn't change -- see the matching
	 * fix + full explanation in asteroidz_jump_label_node_update for why: the
	 * old size-gated fast path let two different wlr_buffer wrapper
	 * generations share the same underlying cairo surface across a
	 * buffer-swap boundary, which a live crash traced back to this exact
	 * function (tab-bar redraw -- the single most frequently hit path in
	 * this file, since it runs on every title/focus/color change). */
	wlr_scene_buffer_set_buffer(node->scene_buffer, NULL);
	if (node->buffer) {
		wlr_buffer_drop(&node->buffer->base);
		node->buffer = NULL;
	}
	if (node->surface) {
		cairo_surface_destroy(node->surface);
		node->surface = NULL;
	}
	node->surface = cairo_image_surface_create(
		CAIRO_FORMAT_ARGB32, required_pixel_w, required_pixel_h);
	node->surface_pixel_w = required_pixel_w;
	node->surface_pixel_h = required_pixel_h;

	cairo_t *cr = cairo_create(node->surface);

	cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.0);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_paint(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

	double border_phys = node->border_width * scale;
	double bg_x = border_phys;
	double bg_y = border_phys;
	double bg_w = box_logical_w * scale;
	double bg_h = box_logical_h * scale;

	double radius;
	if (node->corner_radius < 0) {
		radius = (bg_w < bg_h ? bg_w : bg_h) / 2.0;
	} else {
		radius = node->corner_radius * scale;
	}
	if (radius > bg_w / 2.0)
		radius = bg_w / 2.0;
	if (radius > bg_h / 2.0)
		radius = bg_h / 2.0;

	const float *active_bg =
		node->focused ? node->focus_bg_color : node->bg_color;
	const float *active_fg =
		node->focused ? node->focus_fg_color : node->fg_color;

	bool draw_bg = (active_bg[3] > 0.0f);
	bool draw_border =
		(node->border_width > 0) && (node->border_color[3] > 0.0f);

	if (draw_bg) {
		cairo_set_source_rgba(cr, active_bg[0], active_bg[1], active_bg[2],
							  active_bg[3]);
		if (radius > 0.0) {
			if (node->corner_mask != CORNER_LOCATION_ALL)
				draw_rounded_rect_masked(cr, bg_x, bg_y, bg_w, bg_h, radius,
										 node->corner_mask);
			else
				draw_rounded_rect(cr, bg_x, bg_y, bg_w, bg_h, radius);
			cairo_fill(cr);
		} else {
			cairo_rectangle(cr, bg_x, bg_y, bg_w, bg_h);
			cairo_fill(cr);
		}
	}

	/* content_scale: shrink font + padding + icon gap together so the whole
	 * content fits a proportionally scaled-down bar (overview titlebars). */
	float cs = node->content_scale > 0.0f ? node->content_scale : 1.0f;
	double pad_x = node->padding_x * cs;
	double pad_y = node->padding_y * cs;
	int32_t text_area_logical_w = (int32_t)(box_logical_w - 2.0 * pad_x);
	int32_t text_area_logical_h = (int32_t)(box_logical_h - 2.0 * pad_y);
	if (text_area_logical_w > 0 && text_area_logical_h > 0) {
		cairo_save(cr);

		double text_x = (node->border_width + pad_x) * scale;
		double text_y = (node->border_width + pad_y) * scale;
		double text_area_w = text_area_logical_w * scale;
		double text_area_h = text_area_logical_h * scale;

		PangoContext *ctx = pango_cairo_create_context(cr);
		pango_cairo_context_set_font_options(ctx, asteroidz_font_options());
	pango_cairo_context_set_font_options(ctx, asteroidz_font_options());
		pango_cairo_context_set_resolution(ctx, 96.0 * scale);
		PangoLayout *layout = pango_layout_new(ctx);
		PangoFontDescription *desc = get_cached_font_desc(node->font_desc);
		PangoFontDescription *scaled_desc = NULL;
		if (cs != 1.0f) {
			scaled_desc = pango_font_description_copy(desc);
			int32_t fsz = pango_font_description_get_size(scaled_desc);
			if (fsz > 0)
				pango_font_description_set_size(
					scaled_desc, (int32_t)(fsz * cs + 0.5f));
			desc = scaled_desc;
		}
		pango_layout_set_font_description(layout, desc);
		pango_layout_set_text(layout, text, -1);

		pango_layout_set_wrap(layout, PANGO_WRAP_NONE);
		pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);

		/* icons + title centered as one group; text alone stays centered */
		/* icons_w is the whole icon block INCLUDING the gap before the text,
		 * which only exists when there is text (see _measure_width) */
		double icon_px = 0.0, icon_gap = 0.0, icons_w = 0.0;
		if (node->nicons > 0) {
			icon_px = text_area_h * node->icon_scale;
			icon_gap = 6.0 * cs * scale;
			for (int32_t i = 0; i < node->nicons; i++)
				icons_w += asteroidz_icon_advance(node->icons[i], icon_px);
			icons_w += (node->nicons - 1) * icon_gap +
					   (*text ? icon_gap : 0.0);
		}
		double avail_text_w = text_area_w - icons_w;
		if (avail_text_w < 0)
			avail_text_w = 0;
		bool align_left = node->text_align_left || node->nicons > 0;
		pango_layout_set_alignment(layout, align_left ? PANGO_ALIGN_LEFT
													  : PANGO_ALIGN_CENTER);
		pango_layout_set_width(layout, (int)(avail_text_w * PANGO_SCALE));

		int text_pixel_w, text_pixel_h;
		pango_layout_get_pixel_size(layout, &text_pixel_w, &text_pixel_h);
		double y_offset = (text_area_h - text_pixel_h) / 2.0;
		if (y_offset < 0)
			y_offset = 0;

		if (node->nicons > 0) {
			double text_w_used =
				text_pixel_w < avail_text_w ? text_pixel_w : avail_text_w;
			double group_w = icons_w + text_w_used;
			double group_x = node->text_align_left
								 ? text_x
								 : text_x + (text_area_w - group_w) / 2.0;
			if (group_x < text_x)
				group_x = text_x;

			/* icons_w reserves one gap per icon. Leading, those gaps sit after
			 * each icon and the text simply starts past the block; trailing,
			 * one of them becomes the text-to-icons gap, so the group occupies
			 * exactly the same width either way. */
			double icons_x = node->icons_after_text
								 ? group_x + text_w_used + icon_gap
								 : group_x;
			double text_start =
				node->icons_after_text ? group_x : group_x + icons_w;

			/* Walk the row by each icon's own advance, the same sum
			 * _measure_width totalled -- indexing by ii * icon_px would put a
			 * narrow glyph's neighbours where a square one's would go. */
			double ix = icons_x;
			for (int32_t ii = 0; ii < node->nicons; ii++) {
				cairo_surface_t *ic = node->icons[ii];
				double advance = asteroidz_icon_advance(ic, icon_px);
				double icon_x = ix;
				ix += advance + icon_gap;
				if (!ic)
					continue;
				int icon_w = cairo_image_surface_get_width(ic);
				int icon_h = cairo_image_surface_get_height(ic);
				if (icon_w <= 0 || icon_h <= 0)
					continue;
				double icon_scale = icon_px / (icon_w > icon_h ? icon_w
															   : icon_h);
				cairo_save(cr);
				cairo_translate(cr, icon_x,
								text_y + (text_area_h - icon_h * icon_scale) /
											 2.0);
				cairo_scale(cr, icon_scale, icon_scale);
				if (node->icon_tinted) {
					/* the icon is a stencil: paint the tint through its alpha,
					 * which is how a monochrome plugin SVG is meant to be
					 * coloured (waybar's wb_themed_pixbuf does the same) */
					cairo_set_source_rgba(cr, node->icon_tint[0],
										  node->icon_tint[1],
										  node->icon_tint[2],
										  node->icon_tint[3]);
					cairo_mask_surface(cr, ic, 0, 0);
				} else {
					cairo_set_source_surface(cr, ic, 0, 0);
					cairo_pattern_set_filter(cairo_get_source(cr),
											 CAIRO_FILTER_BILINEAR);
					cairo_paint(cr);
				}
				cairo_restore(cr);
			}
			cairo_translate(cr, text_start, text_y + y_offset);
		} else {
			cairo_translate(cr, text_x, text_y + y_offset);
		}

		cairo_set_source_rgba(cr, active_fg[0], active_fg[1], active_fg[2],
							  active_fg[3]);
		pango_cairo_show_layout(cr, layout);

		g_object_unref(layout);
		g_object_unref(ctx);
		if (scaled_desc)
			pango_font_description_free(scaled_desc);
		cairo_restore(cr);
	}

	if (draw_border) {
		cairo_set_source_rgba(cr, node->border_color[0], node->border_color[1],
							  node->border_color[2], node->border_color[3]);
		cairo_set_line_width(cr, border_phys);

		double half_lw = border_phys * 0.5;
		double bx = bg_x - half_lw;
		double by = bg_y - half_lw;
		double bw = bg_w + border_phys;
		double bh = bg_h + border_phys;

		if (radius > 0.0) {
			double outer_radius = radius + half_lw;
			if (outer_radius < 0.0)
				outer_radius = 0.0;
			if (node->corner_mask != CORNER_LOCATION_ALL)
				draw_rounded_rect_masked(cr, bx, by, bw, bh, outer_radius,
										 node->corner_mask);
			else
				draw_rounded_rect(cr, bx, by, bw, bh, outer_radius);
		} else {
			cairo_rectangle(cr, bx, by, bw, bh);
		}
		cairo_stroke(cr);
	}

	/* titlebar border: left/top/right only, inset so it aligns with the
	 * window's own border. The focused pill borders in the focused bg color
	 * and the unfocused in the unfocused bg color. Drawn over the bg, which
	 * fills to the bottom edge (border_width stays 0 for these). */
	if (node->titlebar_border_width > 0) {
		double tlw = node->titlebar_border_width * scale;
		const float *tb_border =
			node->focused ? node->focus_bg_color : node->bg_color;
		if (tb_border[3] > 0.0f && tlw > 0.0) {
			cairo_set_source_rgba(cr, tb_border[0], tb_border[1], tb_border[2],
								  tb_border[3]);
			cairo_set_line_width(cr, tlw);
			draw_titlebar_border_path(cr, bg_x, bg_y, bg_w, bg_h, radius,
									  node->corner_mask, tlw,
									  node->titlebar_border_left,
									  node->titlebar_border_right);
			cairo_stroke(cr);
		}
	}

	/* separator dividing this segment from the next one in a monocle strip:
	 * a full-height vertical line at the right edge in the fg/contrast color,
	 * so two adjacent same-colored (inactive) segments don't blend together. */
	if (node->titlebar_separator_right) {
		double slw = node->titlebar_border_width > 0
						 ? node->titlebar_border_width * scale
						 : scale;
		const float *sep =
			node->focused ? node->focus_fg_color : node->fg_color;
		if (sep[3] > 0.0f && slw > 0.0) {
			double sx = bg_x + bg_w - slw * 0.5;
			cairo_set_source_rgba(cr, sep[0], sep[1], sep[2], sep[3]);
			cairo_set_line_width(cr, slw);
			cairo_move_to(cr, sx, bg_y);
			cairo_line_to(cr, sx, bg_y + bg_h);
			cairo_stroke(cr);
		}
	}

	cairo_surface_flush(node->surface);
	cairo_destroy(cr);

	struct asteroidz_text_buffer *buf = calloc(1, sizeof(*buf));
	if (!buf)
		return;

	wlr_buffer_init(&buf->base, &text_buffer_impl, node->surface_pixel_w,
					node->surface_pixel_h);
	/* own a reference rather than borrowing node->surface -- see the matching
	 * comment in asteroidz_icon_node_set and in text_buffer_destroy. */
	buf->surface = cairo_surface_reference(node->surface);

	/* attach the new buffer before dropping the old one -- the scene node
	 * must never be left pointing at an already-freed wlr_buffer, even
	 * momentarily (see the matching fix + comment in
	 * asteroidz_icon_node_set for the exact same bug). */
	wlr_scene_buffer_set_buffer(node->scene_buffer, &buf->base);
	if (node->buffer)
		wlr_buffer_drop(&node->buffer->base);
	node->buffer = buf;

	node->logical_width = node->target_width;
	node->logical_height = node->target_height;
	wlr_scene_buffer_set_dest_size(node->scene_buffer, node->logical_width,
								   node->logical_height);
}

void asteroidz_tab_bar_node_set_focus(struct asteroidz_tab_bar_node *node,
								  bool focused) {
	if (!node || node->focused == focused)
		return;
	node->focused = focused;
	if (node->last_text) {
		float scale = node->last_scale > 0.0f ? node->last_scale : 1.0f;
		asteroidz_tab_bar_node_update(node, node->last_text, scale);
	}
}

void asteroidz_tab_bar_node_set_colors(struct asteroidz_tab_bar_node *node,
								   const float fg[4], const float bg[4]) {
	if (!node)
		return;

	memcpy(node->fg_color, fg, sizeof(node->fg_color));
	memcpy(node->bg_color, bg, sizeof(node->bg_color));

	if (!node->focused && node->last_text) {
		float scale = node->last_scale > 0.0f ? node->last_scale : 1.0f;
		asteroidz_tab_bar_node_update(node, node->last_text, scale);
	}
}

void asteroidz_jump_label_node_apply_config(struct asteroidz_jump_label_node *node,
										const AsteroidzTheme *data) {
	if (!node || !data)
		return;

	memcpy(node->fg_color, data->fg_color, sizeof(node->fg_color));
	memcpy(node->bg_color, data->bg_color, sizeof(node->bg_color));
	memcpy(node->focus_fg_color, data->focus_fg_color,
		   sizeof(node->focus_fg_color));
	memcpy(node->focus_bg_color, data->focus_bg_color,
		   sizeof(node->focus_bg_color));
	memcpy(node->border_color, data->border_color, sizeof(node->border_color));
	node->border_width = data->border_width;
	node->corner_radius = data->corner_radius;
	node->padding_x = data->padding_x;
	node->padding_y = data->padding_y;

	g_free(node->font_desc);
	node->font_desc =
		g_strdup(data->font_desc ? data->font_desc : "monospace Bold 16");

	if (node->cached_text && node->cached_scale > 0.0f) {
		asteroidz_jump_label_node_update(node, node->cached_text,
									 node->cached_scale);
	}
}

void asteroidz_tab_bar_node_apply_config(struct asteroidz_tab_bar_node *node,
									 const AsteroidzTheme *data) {
	if (!node || !data)
		return;

	memcpy(node->fg_color, data->fg_color, sizeof(node->fg_color));
	memcpy(node->bg_color, data->bg_color, sizeof(node->bg_color));
	memcpy(node->focus_fg_color, data->focus_fg_color,
		   sizeof(node->focus_fg_color));
	memcpy(node->focus_bg_color, data->focus_bg_color,
		   sizeof(node->focus_bg_color));
	memcpy(node->border_color, data->border_color, sizeof(node->border_color));
	node->border_width = data->border_width;
	node->corner_radius = data->corner_radius;
	node->padding_x = data->padding_x;
	node->padding_y = data->padding_y;

	g_free(node->font_desc);
	node->font_desc =
		g_strdup(data->font_desc ? data->font_desc : "monospace Bold 16");

	if (node->last_text) {
		float scale = node->last_scale > 0.0f ? node->last_scale : 1.0f;
		asteroidz_tab_bar_node_update(node, node->last_text, scale);
	}
}
