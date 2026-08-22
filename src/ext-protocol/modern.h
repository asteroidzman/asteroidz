/*
 * Small modern-protocol integrations:
 *  - content-type-v1: clients tag surfaces as game/video; game surfaces are
 *    treated like an explicit tearing hint
 *  - color-representation-v1: YCbCr encoding/range hints, consumed by the
 *    scenefx scene graph (pairs with color-management/HDR)
 *  - ext-foreign-toplevel-list-v1: modern toplevel list for shells, kept in
 *    sync with the wlr foreign-toplevel handles
 *  - security-context-v1: privileged globals are hidden from sandboxed
 *    (e.g. Flatpak) clients
 *  - xdg-toplevel-icon-v1: icon name stored per client
 *  - xdg-toplevel-tag-v1: client-set tag, matchable from windowrules
 *  - xdg-system-bell-v1: bell marks the client urgent
 */

#ifdef XWAYLAND
#include "xdg-output-unstable-v1-protocol.h"
#endif

static struct wlr_content_type_manager_v1 *content_type_manager;
static struct wlr_ext_foreign_toplevel_list_v1 *ext_foreign_toplevel_list;
static struct wlr_security_context_manager_v1 *security_context_manager;
static struct wlr_xdg_toplevel_icon_manager_v1 *toplevel_icon_manager;
static struct wlr_xdg_toplevel_tag_manager_v1 *toplevel_tag_manager;
static struct wlr_xdg_system_bell_v1 *system_bell;
static struct wl_listener toplevel_icon_set_icon;
static struct wl_listener toplevel_tag_set_tag;
static struct wl_listener system_bell_ring;

static inline bool client_content_type_is_game(Client *c) {
	struct wlr_surface *surface = client_surface(c);
	return content_type_manager && surface &&
		wlr_surface_get_content_type_v1(content_type_manager, surface) ==
			WP_CONTENT_TYPE_V1_TYPE_GAME;
}

static inline bool client_content_type_is_video(Client *c) {
	struct wlr_surface *surface = client_surface(c);
	return content_type_manager && surface &&
		wlr_surface_get_content_type_v1(content_type_manager, surface) ==
			WP_CONTENT_TYPE_V1_TYPE_VIDEO;
}

void client_add_ext_foreign_toplevel(Client *c) {
	struct wlr_ext_foreign_toplevel_handle_v1_state state = {
		.app_id = client_get_appid(c),
		.title = client_get_title(c),
	};

	if (!ext_foreign_toplevel_list || c->ext_foreign_toplevel)
		return;
	c->ext_foreign_toplevel =
		wlr_ext_foreign_toplevel_handle_v1_create(ext_foreign_toplevel_list,
												  &state);
}

void client_remove_ext_foreign_toplevel(Client *c) {
	if (!c->ext_foreign_toplevel)
		return;
	wlr_ext_foreign_toplevel_handle_v1_destroy(c->ext_foreign_toplevel);
	c->ext_foreign_toplevel = NULL;
}

void client_update_ext_foreign_toplevel(Client *c) {
	struct wlr_ext_foreign_toplevel_handle_v1_state state = {
		.app_id = client_get_appid(c),
		.title = client_get_title(c),
	};

	if (!c->ext_foreign_toplevel)
		return;
	wlr_ext_foreign_toplevel_handle_v1_update_state(c->ext_foreign_toplevel,
													&state);
}

static void handle_toplevel_icon_set_icon(struct wl_listener *listener,
										  void *data) {
	struct wlr_xdg_toplevel_icon_manager_v1_set_icon_event *event = data;
	Client *c = NULL;

	toplevel_from_wlr_surface(event->toplevel->base->surface, &c, NULL);
	if (!c)
		return;

	free(c->icon_name);
	c->icon_name = event->icon && event->icon->name
		? strdup(event->icon->name)
		: NULL;
}

static void handle_toplevel_tag_set_tag(struct wl_listener *listener,
										void *data) {
	struct wlr_xdg_toplevel_tag_manager_v1_set_tag_event *event = data;
	Client *c = NULL;

	toplevel_from_wlr_surface(event->toplevel->base->surface, &c, NULL);
	if (!c)
		return;

	free(c->toplevel_tag);
	c->toplevel_tag = event->tag ? strdup(event->tag) : NULL;
}

static void handle_system_bell_ring(struct wl_listener *listener, void *data) {
	struct wlr_xdg_system_bell_v1_ring_event *event = data;
	Client *c = NULL;

	if (!event->surface)
		return;
	toplevel_from_wlr_surface(event->surface, &c, NULL);
	if (!c || !selmon || c == focustop(selmon))
		return;

	c->isurgent = 1;
	if (client_surface(c) && client_surface(c)->mapped)
		setborder_color(c);
	printstatus(IPC_WATCH_ARRANGGE);
}

/* Globals a sandboxed client (one connecting through security-context-v1,
 * e.g. Flatpak) must not see: screen capture, clipboard managers, output
 * and input control, shell-level protocols and the compositor IPC. */
static const char *const privileged_global_interfaces[] = {
	"wp_security_context_manager_v1",
	"zwlr_screencopy_manager_v1",
	"zwlr_export_dmabuf_manager_v1",
	"ext_image_copy_capture_manager_v1",
	"ext_output_image_capture_source_manager_v1",
	"ext_foreign_toplevel_image_capture_source_manager_v1",
	"zwlr_data_control_manager_v1",
	"ext_data_control_manager_v1",
	"zwlr_gamma_control_manager_v1",
	"zwlr_output_manager_v1",
	"zwlr_output_power_manager_v1",
	"zwp_virtual_keyboard_manager_v1",
	"zwlr_virtual_pointer_manager_v1",
	"zwp_input_method_manager_v2",
	"zwlr_foreign_toplevel_manager_v1",
	"ext_foreign_toplevel_list_v1",
	"ext_workspace_manager_v1",
	"zwlr_layer_shell_v1",
	"ext_session_lock_manager_v1",
	"wp_drm_lease_device_v1",
	"zdwl_ipc_manager_v2",
	"ext_background_effect_manager_v1",
};

/* defined in frog-color-management.h (same TU, included later): hides
 * wp-color-management from gamescope so it falls back to frog */
static bool frog_wp_color_manager_visible(const struct wl_client *client,
										  const struct wl_global *global);

/* ── THE X SCREEN IS SIZED BY WHAT XWAYLAND IS TOLD, NOT BY WHAT IT DRAWS ──
 *
 * xwayland_force_scale_one sizes X11 windows in DEVICE PIXELS, but Xwayland
 * sizes its X screen from the outputs' LOGICAL geometry, so on a 1.5x output
 * every window is 1.5x taller than the screen it lives in. X11 requires the
 * pointer to be inside the root window, so every position past the edge is
 * clamped before the client is told: the picture stays pixel-perfect and the
 * bottom third of every X11 window becomes unclickable, every click landing
 * on the same row. Discord's mute/deafen/settings row sits exactly there.
 *
 * There is no API to resize that screen. RandR is the obvious try and it is
 * refused -- `xrandr --fb 3840x2160` against Xwayland returns success and
 * changes nothing, because Xwayland owns the screen size and recomputes it
 * from its Wayland outputs. Xwayland 24.1 has no -scale, and wlroots 0.20
 * passes no extra argv to the server.
 *
 * WHAT DECIDES IT IS WHICH PROTOCOL XWAYLAND LEARNS THE OUTPUTS FROM.
 * xwayland-output.c takes the size from xwl_output->width/height, and those
 * are written by whichever source is present:
 *
 *   with xdg-output    xdg_output_handle_logical_size()  -> 2560x1440
 *   without xdg-output output_handle_mode()              -> 3840x2160
 *
 * output_get_new_size() consults neither wl_output.scale nor anything else;
 * output_handle_scale() only stores a field nothing reads for the screen.
 *
 * MERELY HIDING XDG-OUTPUT IS NOT ENOUGH, AND IT IS WRONG IN A WAY THAT ONLY
 * A SECOND OUTPUT REVEALS. wlroots sends wl_output.geometry with x = 0, y = 0
 * for every output -- a wl_output does not know its own place in the layout,
 * and the position travels ONLY as xdg-output's logical_position. Take
 * xdg-output away and Xwayland stacks every output at the origin: the X
 * screen shrinks to the widest single mode instead of spanning the desktop,
 * and windows on the second output overflow it far worse than before. The
 * mon2 probe in contrib/xw-mixed-test.sh caught exactly that -- a click that
 * should have landed at local x 900 came back 575, the screen's last pixel.
 *
 * So Xwayland gets an xdg-output of its own instead, bound to nobody else:
 * the same protocol carrying the same two numbers, measured in device pixels
 * rather than logical units. Position and size are both the monitor's logical
 * box times x11_scale_of_mon(), which is the identical conversion
 * client_x11_configure() uses to place the windows and client_x11_monitor()
 * uses to attribute them -- so the screen, the windows in it and the zones
 * that name them are finally all in one unit. Version 2 on purpose: at 3 the
 * `done` event is deprecated and Xwayland waits for wl_output.done instead,
 * which wlroots emits on a schedule of its own. Nothing else about the client
 * changes: wl_output v4 still carries the output NAME, so RandR keeps
 * reporting DP-1 rather than a generic XWAYLAND0.
 *
 * This is a property of the SERVER, not of a window, so it follows the global
 * default rather than the per-window xwayland-scale-one rule, and it is
 * decided when Xwayland binds its globals -- a reload cannot move it, only a
 * restart. A window that opts OUT still works under a pixel-sized screen: it
 * is sized and positioned in logical units, which simply makes it a smaller
 * window in a larger root, with no edge to clamp against.
 *
 * BREAK: AZ_BREAK_X11_ROOT_SIZE=1 puts wlroots' own xdg-output back in front
 * of Xwayland and withholds ours, which restores the clamp exactly and
 * changes nothing else. It is what contrib/xw-scale-test.sh's root-size
 * assertions are falsified against, and what contrib/xw-mixed-test.sh checks
 * its own X-screen reasoning against.
 */
#ifdef XWAYLAND
/* Defined with the other X11 boundaries in asteroidz.c, which is included
 * after this file. The scale an X11 window on this monitor is measured in. */
static float x11_scale_of_mon(Monitor *m);

struct x11_xdg_output {
	struct wl_resource *resource;
	struct wlr_output *output;
	struct wl_listener output_destroy;
	struct wl_list link;
};

static struct wl_global *x11_xdg_output_manager;
static struct wl_list x11_xdg_outputs;

static void x11_xdg_output_send(struct x11_xdg_output *xo) {
	Monitor *m = xo->output != NULL ? xo->output->data : NULL;
	if (m == NULL)
		return;

	float s = x11_scale_of_mon(m);
	zxdg_output_v1_send_logical_position(xo->resource,
		(int32_t)lroundf((float)m->m.x * s),
		(int32_t)lroundf((float)m->m.y * s));
	zxdg_output_v1_send_logical_size(xo->resource,
		(int32_t)lroundf((float)m->m.width * s),
		(int32_t)lroundf((float)m->m.height * s));
	zxdg_output_v1_send_done(xo->resource);
}

/* called from updatemons(), i.e. on every layout, mode or scale change */
void x11_xdg_outputs_update(void) {
	struct x11_xdg_output *xo;
	wl_list_for_each(xo, &x11_xdg_outputs, link)
		x11_xdg_output_send(xo);
}

static void x11_xdg_output_handle_output_destroy(struct wl_listener *listener,
												 void *data) {
	struct x11_xdg_output *xo = wl_container_of(listener, xo, output_destroy);
	wl_list_remove(&xo->output_destroy.link);
	wl_list_init(&xo->output_destroy.link);
	xo->output = NULL;
}

static void x11_xdg_output_handle_resource_destroy(struct wl_resource *resource) {
	struct x11_xdg_output *xo = wl_resource_get_user_data(resource);
	wl_list_remove(&xo->output_destroy.link);
	wl_list_remove(&xo->link);
	free(xo);
}

static void x11_xdg_output_destroy(struct wl_client *client,
								   struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct zxdg_output_v1_interface x11_xdg_output_impl = {
	.destroy = x11_xdg_output_destroy,
};

static void x11_xdg_output_manager_get(struct wl_client *client,
									   struct wl_resource *manager_resource,
									   uint32_t id,
									   struct wl_resource *output_resource) {
	struct wlr_output *output = wlr_output_from_resource(output_resource);
	struct x11_xdg_output *xo = ecalloc(1, sizeof(*xo));

	xo->resource = wl_resource_create(client, &zxdg_output_v1_interface,
									  wl_resource_get_version(manager_resource),
									  id);
	if (xo->resource == NULL) {
		free(xo);
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(xo->resource, &x11_xdg_output_impl, xo,
								   x11_xdg_output_handle_resource_destroy);

	xo->output = output;
	wl_list_init(&xo->output_destroy.link);
	if (output != NULL) {
		/* An output can outlive nothing here, but it can certainly die first:
		 * a monitor unplugged while Xwayland holds an xdg_output for it would
		 * otherwise leave a dangling pointer to read on the next update. */
		xo->output_destroy.notify = x11_xdg_output_handle_output_destroy;
		wl_signal_add(&output->events.destroy, &xo->output_destroy);
	}
	wl_list_insert(&x11_xdg_outputs, &xo->link);

	if (output != NULL) {
		zxdg_output_v1_send_name(xo->resource, output->name);
		if (output->description != NULL)
			zxdg_output_v1_send_description(xo->resource, output->description);
	}
	x11_xdg_output_send(xo);
}

static void x11_xdg_output_manager_destroy(struct wl_client *client,
										   struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct zxdg_output_manager_v1_interface
	x11_xdg_output_manager_impl = {
		.destroy = x11_xdg_output_manager_destroy,
		.get_xdg_output = x11_xdg_output_manager_get,
};

static void x11_xdg_output_manager_bind(struct wl_client *client, void *data,
										uint32_t version, uint32_t id) {
	struct wl_resource *resource = wl_resource_create(
		client, &zxdg_output_manager_v1_interface, version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &x11_xdg_output_manager_impl, NULL,
								   NULL);
}
#endif

static bool xdg_output_visible_to(const struct wl_client *client,
								  const struct wl_global *global) {
#ifdef XWAYLAND
	bool wlroots_one = xdg_output_manager != NULL &&
					   global == xdg_output_manager->global;
	bool ours = x11_xdg_output_manager != NULL &&
				global == x11_xdg_output_manager;
	if (!wlroots_one && !ours)
		return true;

	/* ours is never for anyone else, whatever the option says */
	bool is_xwayland = xwayland != NULL && xwayland->server != NULL &&
					   client == xwayland->server->client;
	if (!is_xwayland)
		return !ours;

	static int break_root_size = -1;
	if (break_root_size < 0) {
		const char *e = getenv("AZ_BREAK_X11_ROOT_SIZE");
		break_root_size = e && *e && *e != '0';
	}
	/* off, or broken on purpose: Xwayland sees wlroots' logical one and none
	 * of ours, which is the state the pointer clamp was found in */
	if (!config.xwayland_force_scale_one || break_root_size)
		return wlroots_one;
	return ours;
#else
	(void)client;
	(void)global;
	return true;
#endif
}

static bool security_context_global_filter(const struct wl_client *client,
										   const struct wl_global *global,
										   void *data) {
	const struct wlr_security_context_v1_state *security_context;
	const char *name;
	size_t i;

	/* the display has a single global-filter slot, so every per-client
	 * visibility policy funnels through this one function */
	if (!frog_wp_color_manager_visible(client, global))
		return false;
	if (!xdg_output_visible_to(client, global))
		return false;

	security_context = wlr_security_context_manager_v1_lookup_client(
		security_context_manager, client);
	if (!security_context)
		return true;

	name = wl_global_get_interface(global)->name;
	for (i = 0; i < LENGTH(privileged_global_interfaces); i++) {
		if (strcmp(name, privileged_global_interfaces[i]) == 0)
			return false;
	}
	return true;
}

void modern_protocols_finish(void) {
	/* wlroots asserts that no listeners remain on these managers when the
	 * display shuts down */
	wl_list_remove(&toplevel_icon_set_icon.link);
	wl_list_remove(&toplevel_tag_set_tag.link);
	wl_list_remove(&system_bell_ring.link);
}

void modern_protocols_init(struct wl_display *display,
						   struct wlr_renderer *renderer) {
	content_type_manager = wlr_content_type_manager_v1_create(display, 1);
	wlr_color_representation_manager_v1_create_with_renderer(display, 1,
															 renderer);
	ext_foreign_toplevel_list =
		wlr_ext_foreign_toplevel_list_v1_create(display, 1);

	toplevel_icon_manager = wlr_xdg_toplevel_icon_manager_v1_create(display, 1);
	toplevel_icon_set_icon.notify = handle_toplevel_icon_set_icon;
	wl_signal_add(&toplevel_icon_manager->events.set_icon,
				  &toplevel_icon_set_icon);

	toplevel_tag_manager = wlr_xdg_toplevel_tag_manager_v1_create(display, 1);
	toplevel_tag_set_tag.notify = handle_toplevel_tag_set_tag;
	wl_signal_add(&toplevel_tag_manager->events.set_tag, &toplevel_tag_set_tag);

	system_bell = wlr_xdg_system_bell_v1_create(display, 1);
	system_bell_ring.notify = handle_system_bell_ring;
	wl_signal_add(&system_bell->events.ring, &system_bell_ring);

	security_context_manager = wlr_security_context_manager_v1_create(display);
#ifdef XWAYLAND
	/* version 2: at 3 `done` is deprecated and Xwayland waits on
	 * wl_output.done instead. See xdg_output_visible_to(). */
	wl_list_init(&x11_xdg_outputs);
	x11_xdg_output_manager =
		wl_global_create(display, &zxdg_output_manager_v1_interface, 2, NULL,
						 x11_xdg_output_manager_bind);
#endif
	wl_display_set_global_filter(display, security_context_global_filter,
								 NULL);
}
