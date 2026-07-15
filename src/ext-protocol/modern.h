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
	"wlr_export_dmabuf_manager_v1",
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
	wl_display_set_global_filter(display, security_context_global_filter,
								 NULL);
}
