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

static char *resolve_icon_path(const char *name) {
	if (name[0] == '/')
		return g_file_test(name, G_FILE_TEST_EXISTS) ? g_strdup(name) : NULL;

	const char *home = g_get_home_dir();
	char *bases[2] = {g_build_filename(home, ".local/share/icons", NULL),
					  g_strdup("/usr/share/icons")};
	const char *themes[2] = {icon_theme_name, "hicolor"};
	/* size dirs cover Papirus/hicolor (48x48/apps) and breeze (apps/48) */
	const char *sizes[] = {"48x48", "64x64", "128x128", "scalable", "48", "64"};
	const char *exts[] = {"svg", "png"};
	char *found = NULL;

	for (size_t t = 0; t < 2 && !found; t++) {
		if (t == 1 && strcmp(themes[0], themes[1]) == 0)
			break;
		for (size_t b = 0; b < 2 && !found; b++) {
			for (size_t z = 0; z < sizeof(sizes) / sizeof(sizes[0]) && !found;
				 z++) {
				for (size_t e = 0; e < 2 && !found; e++) {
					char *path = g_strdup_printf("%s/%s/%s/apps/%s.%s",
						bases[b], themes[t], sizes[z], name, exts[e]);
					if (g_file_test(path, G_FILE_TEST_EXISTS))
						found = path;
					else
						g_free(path);
					if (found)
						break;
					path = g_strdup_printf("%s/%s/apps/%s/%s.%s", bases[b],
						themes[t], sizes[z], name, exts[e]);
					if (g_file_test(path, G_FILE_TEST_EXISTS))
						found = path;
					else
						g_free(path);
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
			surf = pixbuf_to_cairo_surface(pixbuf);
			g_object_unref(pixbuf);
		}
		g_free(path);
	}
	g_hash_table_insert(icon_cache, g_strdup(name), surf);
	return surf;
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

void asteroidz_text_global_finish(void) {
	if (font_desc_cache) {
		g_hash_table_destroy(font_desc_cache);
		font_desc_cache = NULL;
	}
}

static void text_buffer_destroy(struct wlr_buffer *wlr_buffer) {
	struct asteroidz_text_buffer *buf = wl_container_of(wlr_buffer, buf, base);
	free(buf);
}

static bool text_buffer_begin_data_ptr_access(struct wlr_buffer *wlr_buffer,
											  uint32_t flags, void **data,
											  uint32_t *format,
											  size_t *stride) {
	(void)flags;
	struct asteroidz_text_buffer *buf = wl_container_of(wlr_buffer, buf, base);
	*data = cairo_image_surface_get_data(buf->surface);
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

	if (node->surface) {
		cairo_surface_destroy(node->surface);
		node->surface = NULL;
	}
	node->surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
	if (cairo_surface_status(node->surface) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(node->surface);
		node->surface = NULL;
		return false;
	}

	cairo_t *cr = cairo_create(node->surface);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.0);
	cairo_paint(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	cairo_scale(cr, (double)size / iw, (double)size / ih);
	cairo_set_source_surface(cr, icon, 0, 0);
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
	cairo_paint(cr);
	cairo_surface_flush(node->surface);
	cairo_destroy(cr);

	if (node->buffer) {
		wlr_buffer_drop(&node->buffer->base);
		node->buffer = NULL;
	}
	struct asteroidz_text_buffer *buf = calloc(1, sizeof(*buf));
	if (!buf)
		return false;
	wlr_buffer_init(&buf->base, &text_buffer_impl, size, size);
	buf->surface = node->surface;
	node->buffer = buf;
	wlr_scene_buffer_set_buffer(node->scene_buffer, &buf->base);
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
	pango_layout_set_text(node->measure_layout, text, -1);

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
	g_free(node->cached_text);
	node->cached_text = g_strdup(text);
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

	bool surface_size_changed = (!node->surface) ||
								(node->surface_pixel_w != required_pixel_w) ||
								(node->surface_pixel_h != required_pixel_h);

	if (surface_size_changed) {
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
	}

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
	pango_cairo_context_set_resolution(ctx, 96.0 * scale);
	PangoLayout *layout = pango_layout_new(ctx);
	PangoFontDescription *desc = get_cached_font_desc(node->font_desc);
	pango_layout_set_font_description(layout, desc);
	pango_layout_set_text(layout, text, -1);

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

	if (node->buffer) {
		wlr_buffer_drop(&node->buffer->base);
		node->buffer = NULL;
	}

	struct asteroidz_text_buffer *buf = calloc(1, sizeof(*buf));
	if (!buf) {
		return;
	}
	wlr_buffer_init(&buf->base, &text_buffer_impl, node->surface_pixel_w,
					node->surface_pixel_h);
	buf->surface = node->surface;
	node->buffer = buf;

	wlr_scene_buffer_set_buffer(node->scene_buffer, &buf->base);

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

void asteroidz_tab_bar_node_set_icon(struct asteroidz_tab_bar_node *node,
								 const char *icon_name) {
	if (!node)
		return;
	cairo_surface_t *surf = NULL;
	if (icon_name && *icon_name)
		surf = get_cached_icon(icon_name);
	if (surf == node->icon_surface)
		return;
	/* the icon cache hands out a borrowed pointer shared across every node
	 * using that icon name; take our own reference so clearing/replacing
	 * the cache (icon theme change) can't leave this node with a dangling
	 * pointer to a surface it doesn't actually own. */
	if (surf)
		cairo_surface_reference(surf);
	if (node->icon_surface)
		cairo_surface_destroy(node->icon_surface);
	node->icon_surface = surf;
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

	if (node->icon_surface) {
		cairo_surface_destroy(node->icon_surface);
		node->icon_surface = NULL;
	}

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

void asteroidz_tab_bar_node_update(struct asteroidz_tab_bar_node *node,
							   const char *text, float scale) {
	if (!node || !text)
		return;
	if (scale <= 0.0f)
		scale = 1.0f;

	char *safe_text = g_strdup(text);

	g_free(node->last_text);
	node->last_text = safe_text; // ownership transferred
	node->last_scale = scale;

	// dirty check, includes focused
	if (node->cached_scale == scale && node->cached_font_desc &&
		strcmp(node->cached_font_desc, node->font_desc) == 0 &&
		node->cached_text && strcmp(node->cached_text, safe_text) == 0 &&
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
		node->cached_icon == node->icon_surface &&
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

	// update cache
	g_free(node->cached_text);
	node->cached_text = g_strdup(safe_text);

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
	node->cached_icon = node->icon_surface;
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

	bool surface_size_changed = (!node->surface) ||
								(node->surface_pixel_w != required_pixel_w) ||
								(node->surface_pixel_h != required_pixel_h);

	if (surface_size_changed) {
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
	}

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
		pango_layout_set_text(layout, safe_text, -1);

		pango_layout_set_wrap(layout, PANGO_WRAP_NONE);
		pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);

		/* icon + title centered as a group; text alone stays centered */
		double icon_px = 0.0, icon_gap = 0.0;
		if (node->icon_surface) {
			icon_px = text_area_h;
			icon_gap = 6.0 * cs * scale;
		}
		double avail_text_w = text_area_w - icon_px - icon_gap;
		if (avail_text_w < 0)
			avail_text_w = 0;
		bool align_left = node->text_align_left || node->icon_surface;
		pango_layout_set_alignment(layout, align_left ? PANGO_ALIGN_LEFT
													  : PANGO_ALIGN_CENTER);
		pango_layout_set_width(layout, (int)(avail_text_w * PANGO_SCALE));

		int text_pixel_w, text_pixel_h;
		pango_layout_get_pixel_size(layout, &text_pixel_w, &text_pixel_h);
		double y_offset = (text_area_h - text_pixel_h) / 2.0;
		if (y_offset < 0)
			y_offset = 0;

		if (node->icon_surface) {
			double group_w = icon_px + icon_gap +
							 (text_pixel_w < avail_text_w ? text_pixel_w
														  : avail_text_w);
			double group_x = node->text_align_left
								 ? text_x
								 : text_x + (text_area_w - group_w) / 2.0;
			if (group_x < text_x)
				group_x = text_x;

			int icon_w = cairo_image_surface_get_width(node->icon_surface);
			int icon_h = cairo_image_surface_get_height(node->icon_surface);
			if (icon_w > 0 && icon_h > 0) {
				double icon_scale = icon_px / (icon_w > icon_h ? icon_w
															   : icon_h);
				cairo_save(cr);
				cairo_translate(cr, group_x,
								text_y + (text_area_h - icon_h * icon_scale) /
											 2.0);
				cairo_scale(cr, icon_scale, icon_scale);
				cairo_set_source_surface(cr, node->icon_surface, 0, 0);
				cairo_pattern_set_filter(cairo_get_source(cr),
										 CAIRO_FILTER_BILINEAR);
				cairo_paint(cr);
				cairo_restore(cr);
			}
			cairo_translate(cr, group_x + icon_px + icon_gap,
							text_y + y_offset);
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

	if (node->buffer) {
		wlr_buffer_drop(&node->buffer->base);
		node->buffer = NULL;
	}

	struct asteroidz_text_buffer *buf = calloc(1, sizeof(*buf));
	if (!buf)
		return;

	wlr_buffer_init(&buf->base, &text_buffer_impl, node->surface_pixel_w,
					node->surface_pixel_h);
	buf->surface = node->surface;
	node->buffer = buf;

	wlr_scene_buffer_set_buffer(node->scene_buffer, &buf->base);

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
