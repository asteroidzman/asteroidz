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

void mango_text_node_set_icon_theme(const char *theme) {
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

void mango_text_global_finish(void) {
	if (font_desc_cache) {
		g_hash_table_destroy(font_desc_cache);
		font_desc_cache = NULL;
	}
}

static void text_buffer_destroy(struct wlr_buffer *wlr_buffer) {
	struct mango_text_buffer *buf = wl_container_of(wlr_buffer, buf, base);
	free(buf);
}

static bool text_buffer_begin_data_ptr_access(struct wlr_buffer *wlr_buffer,
											  uint32_t flags, void **data,
											  uint32_t *format,
											  size_t *stride) {
	(void)flags;
	struct mango_text_buffer *buf = wl_container_of(wlr_buffer, buf, base);
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

struct mango_jump_label_node *
mango_jump_label_node_create(struct wlr_scene_tree *parent,
							 DecorateDrawData data) {
	struct mango_jump_label_node *node = calloc(1, sizeof(*node));
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

void mango_jump_label_node_destroy(struct mango_jump_label_node *node) {
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

void mango_jump_label_node_set_background(struct mango_jump_label_node *node,
										  float r, float g, float b, float a) {
	if (!node)
		return;
	node->bg_color[0] = r;
	node->bg_color[1] = g;
	node->bg_color[2] = b;
	node->bg_color[3] = a;
}

void mango_jump_label_node_set_border(struct mango_jump_label_node *node,
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

void mango_jump_label_node_set_padding(struct mango_jump_label_node *node,
									   int32_t pad_x, int32_t pad_y) {
	if (!node)
		return;
	node->padding_x = pad_x >= 0 ? pad_x : 0;
	node->padding_y = pad_y >= 0 ? pad_y : 0;
}

static void get_text_pixel_size(struct mango_jump_label_node *node,
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

void mango_jump_label_node_update(struct mango_jump_label_node *node,
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

	struct mango_text_buffer *buf = calloc(1, sizeof(*buf));
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

void mango_jump_label_node_set_focus(struct mango_jump_label_node *node,
									 bool focused) {
	if (!node || node->focused == focused)
		return;
	node->focused = focused;
	// trigger a redraw using the cached text and scale (skip if no text)
	if (node->cached_text && node->cached_scale > 0.0f) {
		mango_jump_label_node_update(node, node->cached_text,
									 node->cached_scale);
	}
}

struct mango_tab_bar_node *
mango_tab_bar_node_create(void *mango_node_data, struct wlr_scene_tree *parent,
						  DecorateDrawData data, int32_t width,
						  int32_t height) {
	struct mango_tab_bar_node *node = calloc(1, sizeof(*node));
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

	node->measure_surface =
		cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
	node->measure_cr = cairo_create(node->measure_surface);
	node->measure_context = pango_cairo_create_context(node->measure_cr);
	node->measure_layout = pango_layout_new(node->measure_context);
	node->measure_scale = 1.0f;

	node->cached_scale = -1.0f;
	node->last_text = NULL;
	node->last_scale = 0.0f;
	node->scene_buffer->node.data = mango_node_data;

	return node;
}

static void tab_bar_shadow_sync(struct mango_tab_bar_node *node) {
	if (!node->shadow)
		return;
	int32_t pad = (int32_t)ceilf(node->shadow_sigma * 0.5f);
	int32_t radius = node->corner_radius < 0 ? node->target_height / 2
											 : node->corner_radius;
	wlr_scene_shadow_set_size(node->shadow, node->target_width + 2 * pad,
							  node->target_height + 2 * pad);
	wlr_scene_shadow_set_corner_radius(node->shadow, radius + pad);
	wlr_scene_node_set_position(&node->shadow->node, node->last_x - pad,
								node->last_y - pad + node->shadow_offset_y);
	wlr_scene_node_set_enabled(&node->shadow->node,
							   node->scene_buffer->node.enabled &&
								   node->target_width > 0 &&
								   node->target_height > 0);
}

void mango_tab_bar_node_set_enabled(struct mango_tab_bar_node *node,
									bool enabled) {
	if (!node)
		return;
	wlr_scene_node_set_enabled(&node->scene_buffer->node, enabled);
	if (node->shadow)
		wlr_scene_node_set_enabled(&node->shadow->node,
								   enabled && node->target_width > 0 &&
									   node->target_height > 0);
}

void mango_tab_bar_node_set_position(struct mango_tab_bar_node *node,
									 int32_t x, int32_t y) {
	if (!node)
		return;
	node->last_x = x;
	node->last_y = y;
	wlr_scene_node_set_position(&node->scene_buffer->node, x, y);
	tab_bar_shadow_sync(node);
}

void mango_tab_bar_node_set_shadow(struct mango_tab_bar_node *node,
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

void mango_tab_bar_node_set_icon(struct mango_tab_bar_node *node,
								 const char *icon_name) {
	if (!node)
		return;
	cairo_surface_t *surf = NULL;
	if (icon_name && *icon_name)
		surf = get_cached_icon(icon_name);
	if (surf == node->icon_surface)
		return;
	node->icon_surface = surf;
	if (node->last_text)
		mango_tab_bar_node_update(node, node->last_text,
								  node->last_scale > 0 ? node->last_scale
													   : 1.0f);
}

void mango_tab_bar_node_destroy(struct mango_tab_bar_node *node) {
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

void mango_tab_bar_node_set_size(struct mango_tab_bar_node *node, int32_t width,
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

	mango_tab_bar_node_update(node, redraw_text, redraw_scale);
}

void mango_tab_bar_node_update(struct mango_tab_bar_node *node,
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
			draw_rounded_rect(cr, bg_x, bg_y, bg_w, bg_h, radius);
			cairo_fill(cr);
		} else {
			cairo_rectangle(cr, bg_x, bg_y, bg_w, bg_h);
			cairo_fill(cr);
		}
	}

	int32_t text_area_logical_w = box_logical_w - 2 * node->padding_x;
	int32_t text_area_logical_h = box_logical_h - 2 * node->padding_y;
	if (text_area_logical_w > 0 && text_area_logical_h > 0) {
		cairo_save(cr);

		double text_x = (node->border_width + node->padding_x) * scale;
		double text_y = (node->border_width + node->padding_y) * scale;
		double text_area_w = text_area_logical_w * scale;
		double text_area_h = text_area_logical_h * scale;

		PangoContext *ctx = pango_cairo_create_context(cr);
		pango_cairo_context_set_resolution(ctx, 96.0 * scale);
		PangoLayout *layout = pango_layout_new(ctx);
		PangoFontDescription *desc = get_cached_font_desc(node->font_desc);
		pango_layout_set_font_description(layout, desc);
		pango_layout_set_text(layout, safe_text, -1);

		pango_layout_set_wrap(layout, PANGO_WRAP_NONE);
		pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);

		/* icon + title centered as a group; text alone stays centered */
		double icon_px = 0.0, icon_gap = 0.0;
		if (node->icon_surface) {
			icon_px = text_area_h;
			icon_gap = 6.0 * scale;
		}
		double avail_text_w = text_area_w - icon_px - icon_gap;
		if (avail_text_w < 0)
			avail_text_w = 0;
		pango_layout_set_alignment(layout, node->icon_surface
											   ? PANGO_ALIGN_LEFT
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
			double group_x = text_x + (text_area_w - group_w) / 2.0;
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

	struct mango_text_buffer *buf = calloc(1, sizeof(*buf));
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

void mango_tab_bar_node_set_focus(struct mango_tab_bar_node *node,
								  bool focused) {
	if (!node || node->focused == focused)
		return;
	node->focused = focused;
	if (node->last_text) {
		float scale = node->last_scale > 0.0f ? node->last_scale : 1.0f;
		mango_tab_bar_node_update(node, node->last_text, scale);
	}
}

void mango_tab_bar_node_set_colors(struct mango_tab_bar_node *node,
								   const float fg[4], const float bg[4]) {
	if (!node)
		return;

	memcpy(node->fg_color, fg, sizeof(node->fg_color));
	memcpy(node->bg_color, bg, sizeof(node->bg_color));

	if (!node->focused && node->last_text) {
		float scale = node->last_scale > 0.0f ? node->last_scale : 1.0f;
		mango_tab_bar_node_update(node, node->last_text, scale);
	}
}

void mango_jump_label_node_apply_config(struct mango_jump_label_node *node,
										const DecorateDrawData *data) {
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
		mango_jump_label_node_update(node, node->cached_text,
									 node->cached_scale);
	}
}

void mango_tab_bar_node_apply_config(struct mango_tab_bar_node *node,
									 const DecorateDrawData *data) {
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
		mango_tab_bar_node_update(node, node->last_text, scale);
	}
}
MangoGroupBar *mango_group_bar_create(void *cdata, uint32_t type,
									  struct wlr_scene_tree *parent,
									  DecorateDrawData data, int32_t width,
									  int32_t height) {
	MangoGroupBar *mangobar = calloc(1, sizeof(*mangobar));
	if (!mangobar)
		return NULL;

	mangobar->scene_buffer = wlr_scene_buffer_create(parent, NULL);
	if (!mangobar->scene_buffer) {
		free(mangobar);
		return NULL;
	}

	memcpy(mangobar->fg_color, data.fg_color, sizeof(mangobar->fg_color));
	memcpy(mangobar->bg_color, data.bg_color, sizeof(mangobar->bg_color));
	memcpy(mangobar->focus_fg_color, data.focus_fg_color,
		   sizeof(mangobar->focus_fg_color));
	memcpy(mangobar->focus_bg_color, data.focus_bg_color,
		   sizeof(mangobar->focus_bg_color));
	memcpy(mangobar->border_color, data.border_color,
		   sizeof(mangobar->border_color));
	mangobar->border_width = data.border_width;
	mangobar->corner_radius = data.corner_radius;
	mangobar->padding_x = data.padding_x;
	mangobar->padding_y = data.padding_y;
	mangobar->font_desc =
		g_strdup(data.font_desc ? data.font_desc : "monospace Bold 16");

	mangobar->target_width = width;
	mangobar->target_height = height;
	mangobar->focused = false;
	mangobar->cached_focused = false;

	mangobar->measure_surface =
		cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
	mangobar->measure_cr = cairo_create(mangobar->measure_surface);
	mangobar->measure_context =
		pango_cairo_create_context(mangobar->measure_cr);
	mangobar->measure_layout = pango_layout_new(mangobar->measure_context);
	mangobar->measure_scale = 1.0f;

	mangobar->cached_scale = -1.0f;
	mangobar->last_text = NULL;
	mangobar->last_scale = 0.0f;

	mangobar->type = type;
	mangobar->node_data = cdata;

	mangobar->scene_buffer->node.data = mangobar;

	return mangobar;
}

void mango_group_bar_destroy(MangoGroupBar *node) {
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

	wlr_scene_node_destroy(&node->scene_buffer->node);

	g_free(node->font_desc);
	g_free(node->cached_text);
	g_free(node->cached_font_desc);
	g_free(node->last_text);
	free(node);
}

void mango_group_bar_set_size(MangoGroupBar *node, int32_t width,
							  int32_t height) {
	if (!node)
		return;

	if (width < 0)
		width = 0;
	if (height < 0)
		height = 0;

	if (node->target_width == width && node->target_height == height)
		return;

	node->target_width = width;
	node->target_height = height;

	const char *redraw_text = node->last_text ? node->last_text : "";
	float redraw_scale = node->last_scale > 0.0f ? node->last_scale : 1.0f;

	mango_group_bar_update(node, redraw_text, redraw_scale);
}

void mango_group_bar_update(MangoGroupBar *node, const char *text,
							float scale) {
	if (!node || !text)
		return;
	if (scale <= 0.0f)
		scale = 1.0f;

	char *safe_text = g_strdup(text);

	g_free(node->last_text);
	node->last_text = safe_text; // ownership transferred
	node->last_scale = scale;

	// dirty check
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

	// surface pixel size includes the border, to avoid the border drawing out of bounds
	int32_t required_pixel_w = (int32_t)(node->target_width * scale + 0.5f);
	int32_t required_pixel_h = (int32_t)(node->target_height * scale + 0.5f);
	// if the border would extend past the target area, grow the surface to fully contain it
	double border_phys = node->border_width * scale;
	if (border_phys > 0.0) {
		// the border stroke extends outward by half the line width, so extra space is needed
		int extra_w = (int32_t)ceil(border_phys * 0.5);
		int extra_h = (int32_t)ceil(border_phys * 0.5);
		required_pixel_w += extra_w;
		required_pixel_h += extra_h;
	}
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
	if (radius < 0.0)
		radius = 0.0;

	const float *active_bg =
		node->focused ? node->focus_bg_color : node->bg_color;
	const float *active_fg =
		node->focused ? node->focus_fg_color : node->fg_color;

	bool draw_bg = (active_bg[3] > 0.0f) && (bg_w > 0.0 && bg_h > 0.0);
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

	// only proceed if the background area has room
	if (bg_w > 0.0 && bg_h > 0.0) {
		int32_t text_area_logical_w = box_logical_w - 2 * node->padding_x;
		int32_t text_area_logical_h = box_logical_h - 2 * node->padding_y;
		if (text_area_logical_w > 0 && text_area_logical_h > 0) {
			cairo_save(cr);

			double text_x = (node->border_width + node->padding_x) * scale;
			double text_y = (node->border_width + node->padding_y) * scale;
			double text_area_w = text_area_logical_w * scale;
			double text_area_h = text_area_logical_h * scale;

			PangoContext *ctx = pango_cairo_create_context(cr);
			pango_cairo_context_set_resolution(ctx, 96.0 * scale);
			PangoLayout *layout = pango_layout_new(ctx);
			PangoFontDescription *desc = get_cached_font_desc(node->font_desc);
			pango_layout_set_font_description(layout, desc);
			pango_layout_set_text(layout, safe_text, -1);

			pango_layout_set_wrap(layout, PANGO_WRAP_NONE);
			pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
			pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
			pango_layout_set_width(layout, (int)(text_area_w * PANGO_SCALE));

			int text_pixel_w, text_pixel_h;
			pango_layout_get_pixel_size(layout, &text_pixel_w, &text_pixel_h);
			double y_offset = (text_area_h - text_pixel_h) / 2.0;
			if (y_offset < 0)
				y_offset = 0;

			cairo_translate(cr, text_x, text_y + y_offset);

			cairo_set_source_rgba(cr, active_fg[0], active_fg[1], active_fg[2],
								  active_fg[3]);
			pango_cairo_show_layout(cr, layout);

			g_object_unref(layout);
			g_object_unref(ctx);
			cairo_restore(cr);
		}
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

		// ensure the border rect stays in bounds with positive width/height
		if (bx < 0.0) {
			bw += bx;
			bx = 0.0;
		}
		if (by < 0.0) {
			bh += by;
			by = 0.0;
		}
		if (bx + bw > (double)node->surface_pixel_w)
			bw = (double)node->surface_pixel_w - bx;
		if (by + bh > (double)node->surface_pixel_h)
			bh = (double)node->surface_pixel_h - by;
		if (bw < 0.0)
			bw = 0.0;
		if (bh < 0.0)
			bh = 0.0;

		if (bw > 0.0 && bh > 0.0) {
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
	}

	cairo_surface_flush(node->surface);
	cairo_destroy(cr);

	if (node->buffer) {
		wlr_buffer_drop(&node->buffer->base);
		node->buffer = NULL;
	}

	struct mango_text_buffer *buf = calloc(1, sizeof(*buf));
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

void mango_group_bar_set_focus(MangoGroupBar *node, bool focused) {
	if (!node || node->focused == focused)
		return;
	node->focused = focused;
	if (node->last_text) {
		float scale = node->last_scale > 0.0f ? node->last_scale : 1.0f;
		mango_group_bar_update(node, node->last_text, scale);
	}
}

void mango_group_bar_set_colors(MangoGroupBar *node, const float fg[4],
								const float bg[4]) {
	if (!node)
		return;

	memcpy(node->fg_color, fg, sizeof(node->fg_color));
	memcpy(node->bg_color, bg, sizeof(node->bg_color));

	if (!node->focused && node->last_text) {
		float scale = node->last_scale > 0.0f ? node->last_scale : 1.0f;
		mango_group_bar_update(node, node->last_text, scale);
	}
}


void mango_group_bar_apply_config(MangoGroupBar *node,
								  const DecorateDrawData *data) {
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
		mango_group_bar_update(node, node->last_text, scale);
	}
}