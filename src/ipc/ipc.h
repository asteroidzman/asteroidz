#include <cjson/cJSON.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "ipc-config.h"
#include "ipc-rules.h"
#include "ipc-out.h"

struct ipc_watch_client {
	struct wl_list link;
	int fd;
	struct wl_event_source *source;
	enum ipc_watch_type type;
	struct ipc_out out;
	union {
		struct {
			char name[64];
		} monitor;
		struct {
			uint32_t id;
		} client;
		struct {
			char mon_name[64];
		} tags;
	} target;
};

static struct wl_list watch_clients;

struct ipc_client_state {
	int fd;
	struct wl_event_source *source;
	struct wl_event_loop *loop;
	char *buf;
	size_t buf_len;
	size_t buf_cap;
	struct ipc_out out;
	/* The reply has been queued and the connection closes once it is out. */
	bool closing;
	/* The handler answered nothing and the connection must STAY OPEN.
	 *
	 * For `capture-chord`, whose reply arrives when a key is pressed rather than
	 * when the handler returns. Without it the normal path sees an empty output
	 * queue, concludes the reply is out, and closes -- so the client would get
	 * EOF instead of a chord. */
	bool deferred;
};

static void ipc_remove_watch_client(struct ipc_watch_client *wc);
static void ipc_notify_json_to_fd(int fd, cJSON *json);

/* Answer a request whose handler returned without answering.
 *
 * The other half of `deferred`: queue the reply now, arm the writable handler,
 * and let the ordinary drain-then-close path finish the connection. Nothing here
 * closes the fd itself -- doing so would race whatever is still in the queue,
 * which is the bug the output queue exists to have fixed. */
static void ipc_capture_reply(struct ipc_client_state *c, const char *json) {
	if (!c)
		return;
	if (!ipc_out_append(&c->out, json, strlen(json)) ||
		!ipc_out_append(&c->out, "\n", 1)) {
		return;
	}
	c->deferred = false;
	c->closing = true;
	if (!ipc_out_flush(c->fd, &c->out))
		return;
	wl_event_source_fd_update(c->source, WL_EVENT_WRITABLE | WL_EVENT_HANGUP |
											 WL_EVENT_ERROR);
}

/* Included HERE rather than beside the other ipc-* headers: it calls
 * ipc_capture_reply above and takes an ipc_client_state, so both have to exist
 * first. */
#include "ipc-capture.h"

/* ---------- utility functions ---------- */

static Monitor *monitor_by_name(const char *name) {
	Monitor *m;
	wl_list_for_each(m, &mons, link) {
		if (strcmp(m->wlr_output->name, name) == 0)
			return m;
	}
	return NULL;
}

static Client *client_by_id(uint32_t id) {
	Client *c;
	wl_list_for_each(c, &clients, link) {
		if (c->id == id)
			return c;
	}
	return NULL;
}

static const char *ipc_get_layout_str(void) {
	struct wlr_keyboard *keyboard = &kb_group->wlr_group->keyboard;
	xkb_layout_index_t current = xkb_state_serialize_layout(
		keyboard->xkb_state, XKB_STATE_LAYOUT_EFFECTIVE);
	static char layout[32];
	const char *name = xkb_keymap_layout_get_name(keyboard->keymap, current);
	snprintf(layout, sizeof(layout), "%s", name ? name : "");
	return layout;
}

static cJSON *tags_mask_to_array(uint32_t tagmask) {
	cJSON *arr = cJSON_CreateArray();
	for (int i = 0; i < LENGTH(tags); i++)
		if (tagmask & (1 << i))
			cJSON_AddItemToArray(arr, cJSON_CreateNumber(i + 1));
	return arr;
}

static cJSON *build_tags_json(Monitor *m) {
	cJSON *tags_array = cJSON_CreateArray();
	Client *c = NULL;
	for (int tag = 1; tag <= LENGTH(tags); tag++) {
		int numclients = 0;
		bool is_active = false, is_urgent = false;
		uint32_t tagmask = 1 << (tag - 1);
		if (tagmask & m->tagset[m->seltags])
			is_active = true;
		wl_list_for_each(c, &clients, link) {
			if (c->mon != m)
				continue;
			if (!(c->tags & tagmask & TAGMASK))
				continue;
			if (c->isurgent)
				is_urgent = true;
			numclients++;
		}
		char tagname[64];
		tag_display_name(m, tag, tagname, sizeof(tagname));
		cJSON *tag_obj = cJSON_CreateObject();
		cJSON_AddNumberToObject(tag_obj, "index", tag);
		cJSON_AddStringToObject(tag_obj, "name", tagname);
		cJSON_AddBoolToObject(tag_obj, "is_active", is_active);
		cJSON_AddBoolToObject(tag_obj, "is_urgent", is_urgent);
		cJSON_AddStringToObject(tag_obj, "layout",
								m->pertag->ltidxs[tag]->symbol);
		cJSON_AddNumberToObject(tag_obj, "client_count", numclients);
		cJSON_AddItemToArray(tags_array, tag_obj);
	}
	return tags_array;
}

static cJSON *monitor_active_client(Monitor *m) {
	cJSON *obj = cJSON_CreateObject();
	if (!m->sel) {
		cJSON_AddNullToObject(obj, "id");
		cJSON_AddNullToObject(obj, "title");
		cJSON_AddNullToObject(obj, "appid");
		return obj;
	}
	Client *c = m->sel;
	cJSON_AddNumberToObject(obj, "id", c->id);
	cJSON_AddStringToObject(obj, "title", client_get_title(c));
	cJSON_AddStringToObject(obj, "appid", client_get_appid(c));
	return obj;
}

static cJSON *monitor_active_tags(Monitor *m) {
	cJSON *arr = cJSON_CreateArray();
	uint32_t tagset;
	if (m->isoverview) {
		cJSON_AddItemToArray(arr, cJSON_CreateNumber(0));
		return arr;
	}
	tagset = m->tagset[m->seltags];
	for (int i = 0; i < LENGTH(tags); i++)
		if (tagset & (1 << i))
			cJSON_AddItemToArray(arr, cJSON_CreateNumber(i + 1));
	return arr;
}

static cJSON *build_client_json(Client *c) {
	cJSON *obj = cJSON_CreateObject();

	cJSON_AddNumberToObject(obj, "id", c->id);
	cJSON_AddNumberToObject(obj, "pid", c->pid);
	cJSON_AddStringToObject(obj, "foreign_toplevel_id",
							c->ext_foreign_toplevel
								? c->ext_foreign_toplevel->identifier
								: "");
	cJSON_AddStringToObject(obj, "title", client_get_title(c));
	cJSON_AddStringToObject(obj, "appid", client_get_appid(c));
	cJSON_AddBoolToObject(obj, "is_xwayland", client_is_x11(c));
	cJSON_AddStringToObject(obj, "icon", c->icon_name ? c->icon_name : "");
	cJSON_AddStringToObject(obj, "monitor",
							c->mon ? c->mon->wlr_output->name : "");
	cJSON_AddItemToObject(obj, "tags", tags_mask_to_array(c->tags));
	cJSON_AddBoolToObject(obj, "is_focused", c->isfocused);
	cJSON_AddBoolToObject(obj, "is_fullscreen", c->isfullscreen);
	/* "vrr" is the honest per-client answer to "is this app under variable
	 * refresh right now": VRR is an output-wide hardware state, so it's the
	 * committed adaptive-sync status of the client's monitor (same query the
	 * monitor JSON uses, not our own intent bookkeeping). "vrr_only_fullscreen"
	 * is the per-client window rule that makes this app *drive* VRR while
	 * fullscreen -- the "why" behind the flag. */
	cJSON_AddBoolToObject(obj, "vrr",
						  c->mon && c->mon->wlr_output->adaptive_sync_status ==
										WLR_OUTPUT_ADAPTIVE_SYNC_ENABLED);
	cJSON_AddBoolToObject(obj, "vrr_only_fullscreen",
						  c->vrr_only_fullscreen > 0);
	cJSON_AddBoolToObject(obj, "is_floating", c->isfloating);
	cJSON_AddBoolToObject(obj, "is_maximized", c->ismaximizescreen);
	cJSON_AddBoolToObject(obj, "is_global", c->isglobal);
	cJSON_AddBoolToObject(obj, "is_unglobal", c->isunglobal);
	cJSON_AddBoolToObject(obj, "is_overlay", c->isoverlay);
	cJSON_AddBoolToObject(obj, "is_fakefullscreen", c->isfakefullscreen);
	cJSON_AddBoolToObject(obj, "is_minimized", c->isminimized);
	cJSON_AddBoolToObject(obj, "is_urgent", c->isurgent);
	cJSON_AddBoolToObject(obj, "is_scratchpad", c->is_in_scratchpad);
	cJSON_AddBoolToObject(obj, "is_namedscratchpad", c->isnamedscratchpad);
	cJSON_AddStringToObject(obj, "special_workspace",
							c->special_name ? c->special_name : "");
	cJSON_AddBoolToObject(obj, "pinned", c->ispinned);
	cJSON_AddNumberToObject(obj, "x", c->geom.x);
	cJSON_AddNumberToObject(obj, "y", c->geom.y);
	cJSON_AddNumberToObject(obj, "width", c->geom.width);
	cJSON_AddNumberToObject(obj, "height", c->geom.height);
	cJSON_AddNumberToObject(obj, "scroller_proportion",
							(double)c->scroller_proportion);
	return obj;
}

// Available output modes, for a config UI's resolution/refresh picker.
// refresh is wlroots-native millihertz (divide by 1000 for Hz).
static cJSON *build_modes_json(Monitor *m) {
	cJSON *arr = cJSON_CreateArray();
	struct wlr_output_mode *mode;
	wl_list_for_each(mode, &m->wlr_output->modes, link) {
		cJSON *o = cJSON_CreateObject();
		cJSON_AddNumberToObject(o, "width", mode->width);
		cJSON_AddNumberToObject(o, "height", mode->height);
		cJSON_AddNumberToObject(o, "refresh", mode->refresh);
		cJSON_AddBoolToObject(o, "current", mode == m->wlr_output->current_mode);
		cJSON_AddBoolToObject(o, "preferred", mode->preferred);
		cJSON_AddItemToArray(arr, o);
	}
	return arr;
}

static cJSON *build_monitor_json(Monitor *m) {
	cJSON *resp = cJSON_CreateObject();
	cJSON_AddStringToObject(resp, "name", m->wlr_output->name);
	cJSON_AddBoolToObject(resp, "active", m == selmon);
	cJSON_AddBoolToObject(resp, "enabled", m->wlr_output->enabled);
	cJSON_AddBoolToObject(resp, "asleep", (bool)m->asleep);
	cJSON_AddNumberToObject(resp, "x", m->m.x);
	cJSON_AddNumberToObject(resp, "y", m->m.y);
	cJSON_AddNumberToObject(resp, "width", m->m.width);
	cJSON_AddNumberToObject(resp, "height", m->m.height);
	cJSON_AddNumberToObject(resp, "scale", m->wlr_output->scale);
	cJSON_AddNumberToObject(resp, "layout_index",
							m->pertag->ltidxs[m->pertag->curtag] - layouts);
	cJSON_AddStringToObject(resp, "layout_symbol",
							m->pertag->ltidxs[m->pertag->curtag]->symbol);
	cJSON_AddStringToObject(resp, "last_open_surface", m->last_open_surface);
	/* "hdr"/"vrr" report the actual, currently-committed hardware state
	 * (queried straight from wlroots, not our own intent bookkeeping) --
	 * a commit can be pending, rejected, or mid-retrain, so m->hdr / the
	 * configured vrr flag alone can briefly (or, if something's stuck,
	 * indefinitely) disagree with what the display is really doing.
	 * "*_enabled" is the persisted/intended setting (what a config UI
	 * should read/write); "*_capable" is a static hardware capability
	 * check. */
	cJSON_AddBoolToObject(resp, "hdr", m->wlr_output->image_description != NULL);
	cJSON_AddBoolToObject(resp, "hdr_enabled", m->hdr);
	cJSON_AddBoolToObject(resp, "hdr_capable",
						  (m->wlr_output->supported_primaries &
						   WLR_COLOR_NAMED_PRIMARIES_BT2020) &&
							  (m->wlr_output->supported_transfer_functions &
							   WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ));
	/* The bit depth in USE, not the one configured. m->bitdepth is the
	 * config value where 0 means "auto" -- reporting that verbatim answered
	 * "what depth is this output running at?" with 0, which is not a depth.
	 * Same split as hdr/hdr_enabled above: the plain name is what the output
	 * is really doing, *_enabled is what was asked for. */
	cJSON_AddNumberToObject(
		resp, "bitdepth",
		m->wlr_output->render_format == DRM_FORMAT_XRGB2101010 ? 10 : 8);
	cJSON_AddNumberToObject(resp, "bitdepth_enabled", m->bitdepth);
	cJSON_AddNumberToObject(resp, "hdr_max_luminance", m->hdr_max_luminance);
	cJSON_AddNumberToObject(resp, "hdr_min_luminance", m->hdr_min_luminance);
	cJSON_AddNumberToObject(resp, "hdr_max_fall", m->hdr_max_fall);
	cJSON_AddStringToObject(resp, "icc_profile", m->icc_path);
	cJSON_AddNumberToObject(resp, "sdr_luminance",
							config.sdr_reference_luminance > 0
								? config.sdr_reference_luminance
								: 203);
	cJSON_AddBoolToObject(resp, "vrr",
						  m->wlr_output->adaptive_sync_status ==
							  WLR_OUTPUT_ADAPTIVE_SYNC_ENABLED);
	cJSON_AddBoolToObject(resp, "vrr_enabled", m->vrr_global_enable);
	cJSON_AddBoolToObject(resp, "vrr_capable", m->wlr_output->adaptive_sync_supported);
	cJSON_AddItemToObject(resp, "modes", build_modes_json(m));
	if (m->wlr_output->current_mode) {
		cJSON_AddNumberToObject(resp, "mode_width",
								m->wlr_output->current_mode->width);
		cJSON_AddNumberToObject(resp, "mode_height",
								m->wlr_output->current_mode->height);
		cJSON_AddNumberToObject(resp, "mode_refresh",
								m->wlr_output->current_mode->refresh);
	}
	cJSON_AddItemToObject(resp, "tags", build_tags_json(m));
	cJSON_AddItemToObject(resp, "active_tags", monitor_active_tags(m));
	cJSON_AddItemToObject(resp, "active_client", monitor_active_client(m));
	cJSON_AddItemToObject(resp, "keymode", cJSON_CreateString(keymode.mode));
	cJSON_AddItemToObject(resp, "keyboardlayout",
						  cJSON_CreateString(ipc_get_layout_str()));
	cJSON_AddStringToObject(resp, "active_special",
							m->active_special ? m->active_special : "");
	return resp;
}

/* Whether idling is being held off, and by whom.
 *
 * Two flags rather than one, because a client that wants to DRAW this state
 * and a client that wants to change it need different answers. `inhibited` is
 * what the idle notifier was actually told and is the honest answer to "will
 * this machine sleep"; `manual` is the flag `toggle_idle_inhibit` owns, and is
 * the only one a bar's toggle may show as its own -- a pill lit because mpv
 * holds an inhibitor would go dark when the user clicked it, having in fact
 * changed nothing they can see.
 *
 * `portal` is the third answer, to a question the first two cannot settle:
 * WHO. A request that arrives over org.freedesktop.portal.Inhibit has no
 * window and no surface, so there is nothing on screen to point at when the
 * machine will not sleep -- which is exactly the shape of the failure that
 * matters, a laptop awake in a bag with no visible cause. Each entry carries
 * the app that asked and the reason it gave; `flags` is the portal's own
 * bitmask (1 logout, 2 user-switch, 4 suspend, 8 idle), so an entry that
 * appears here without raising `inhibited` is one asteroidz recorded and does
 * not enforce. See src/ipc/inhibit-portal.h. */
static cJSON *build_idle_response(void) {
	cJSON *resp = cJSON_CreateObject();
	cJSON_AddBoolToObject(resp, "inhibited", idle_inhibited);
	cJSON_AddBoolToObject(resp, "manual", idle_inhibit_manual);

	cJSON *portal = cJSON_AddArrayToObject(resp, "portal");
	const char *app_id, *reason;
	uint32_t flags;
	for (size_t i = 0; inhibit_portal_get(i, &app_id, &reason, &flags); i++) {
		cJSON *entry = cJSON_CreateObject();
		cJSON_AddStringToObject(entry, "app_id", app_id);
		cJSON_AddStringToObject(entry, "reason", reason);
		cJSON_AddNumberToObject(entry, "flags", flags);
		cJSON_AddBoolToObject(entry, "logout", flags & 1);
		cJSON_AddBoolToObject(entry, "user_switch", flags & 2);
		cJSON_AddBoolToObject(entry, "suspend", flags & 4);
		cJSON_AddBoolToObject(entry, "idle", flags & 8);
		cJSON_AddItemToArray(portal, entry);
	}
	return resp;
}

static cJSON *build_all_tags_entry(Monitor *m) {
	cJSON *entry = cJSON_CreateObject();
	cJSON_AddStringToObject(entry, "monitor", m->wlr_output->name);
	cJSON_AddItemToObject(entry, "tags", build_tags_json(m));
	return entry;
}

static cJSON *build_all_tags_response(void) {
	cJSON *arr = cJSON_CreateArray();
	Monitor *m;
	wl_list_for_each(m, &mons, link)
		cJSON_AddItemToArray(arr, build_all_tags_entry(m));
	cJSON *resp = cJSON_CreateObject();
	cJSON_AddItemToObject(resp, "all_tags", arr);
	return resp;
}

static cJSON *build_monitor_tags_response(Monitor *m) {
	cJSON *resp = cJSON_CreateObject();
	cJSON_AddStringToObject(resp, "monitor", m->wlr_output->name);
	cJSON_AddItemToObject(resp, "tags", build_tags_json(m));
	cJSON_AddItemToObject(resp, "active_tags", monitor_active_tags(m));
	return resp;
}

#ifdef ASTEROIDZ_NATIVE_BAR
/* Colours as [r,g,b,a] floats, not "#rrggbbaa".
 *
 * Hex would have to pick a byte order, and the two obvious ones disagree:
 * CSS reads #RRGGBBAA, Qt reads #AARRGGBB, and a string that parses under both
 * conventions but means different things is the kind of bug that shows up as
 * "the bar is slightly the wrong colour" months later. Four floats in the
 * range the compositor already stores them in cannot be misread. */
static void bar_cfg_color(cJSON *o, const char *key, const float c[4]) {
	cJSON *arr = cJSON_CreateArray();
	for (int i = 0; i < 4; i++)
		cJSON_AddItemToArray(arr, cJSON_CreateNumber((double)c[i]));
	cJSON_AddItemToObject(o, key, arr);
}

/* Everything an out-of-process bar needs to draw itself the way the native one
 * does: the RESOLVED values, after defaults, clamping and the theme file.
 *
 * Deliberately not "here is the config file, parse it yourself" -- that is two
 * KDL readers that agree until one of them gains a default, and the palette is
 * rewritten at runtime by matugen anyway. The compositor is the only process
 * that knows what the theme currently IS. */
static cJSON *build_bar_config_response(void) {
	cJSON *resp = cJSON_CreateObject();

	cJSON *bar = cJSON_CreateObject();
	cJSON_AddBoolToObject(bar, "enable", config.bar_enable);
	cJSON_AddNumberToObject(bar, "height", config.bar_height);
	cJSON_AddStringToObject(bar, "position",
							config.bar_position_bottom ? "bottom" : "top");
	cJSON_AddNumberToObject(bar, "spacing", config.bar_spacing);
	cJSON_AddNumberToObject(bar, "margin_x", config.bar_margin_x);
	cJSON_AddNumberToObject(bar, "margin_y", config.bar_margin_y);
	cJSON_AddNumberToObject(bar, "pill_min_width", config.bar_pill_min_width);
	cJSON_AddNumberToObject(bar, "pill_inset", config.bar_pill_inset);
	cJSON_AddNumberToObject(bar, "pill_padding", config.bar_pill_padding);
	cJSON_AddNumberToObject(bar, "tag_padding", config.bar_tag_padding);
	cJSON_AddNumberToObject(bar, "module_spacing", config.bar_module_spacing);
	cJSON_AddNumberToObject(bar, "tray_spacing", config.bar_tray_spacing);
	cJSON_AddBoolToObject(bar, "tooltip_enable", config.bar_tooltip_enable);
	cJSON_AddNumberToObject(bar, "tooltip_delay", config.bar_tooltip_delay);
	cJSON_AddNumberToObject(bar, "interval", config.bar_interval);
	cJSON_AddNumberToObject(bar, "title_width", config.bar_title_width);
	cJSON_AddNumberToObject(bar, "media_width", config.bar_media_width);
	cJSON_AddNumberToObject(bar, "media_bars", config.bar_media_bars);
	cJSON_AddNumberToObject(bar, "media_fps", config.bar_media_fps);
	cJSON_AddNumberToObject(bar, "media_viz", config.bar_media_viz);
	cJSON_AddBoolToObject(bar, "show_all_tags", config.bar_show_all_tags);
	cJSON_AddNumberToObject(bar, "min_tags", config.bar_min_tags);
	cJSON_AddBoolToObject(bar, "show_logo", config.bar_show_logo);
	cJSON_AddNumberToObject(bar, "tag_icons", config.bar_tag_icons);
	cJSON_AddNumberToObject(bar, "volume_step", config.bar_volume_step);
	cJSON_AddNumberToObject(bar, "weather_interval",
							config.bar_weather_interval);
	cJSON_AddStringToObject(bar, "weather_location",
							config.bar_weather_location);
	cJSON_AddNumberToObject(bar, "net_max_down", config.bar_net_max_down);
	cJSON_AddNumberToObject(bar, "net_max_up", config.bar_net_max_up);
	cJSON_AddStringToObject(bar, "clock_format", config.bar_clock_format);
	cJSON_AddStringToObject(bar, "icon_dir", config.bar_icon_dir);
	cJSON_AddStringToObject(bar, "modules_left", config.bar_modules_left);
	cJSON_AddStringToObject(bar, "modules_center", config.bar_modules_center);
	cJSON_AddStringToObject(bar, "modules_right", config.bar_modules_right);
	cJSON_AddStringToObject(bar, "modules_left_monitor",
							config.bar_modules_left_monitor);
	cJSON_AddStringToObject(bar, "modules_center_monitor",
							config.bar_modules_center_monitor);
	cJSON_AddStringToObject(bar, "modules_right_monitor",
							config.bar_modules_right_monitor);
	cJSON_AddItemToObject(resp, "bar", bar);

	/* Idle, which the BAR carries out: it is the Wayland client that can hold
	 * an ext-idle-notify timer, and the compositor is the thing it dispatches
	 * DPMS to. Sent even when disabled, so turning it on is a reload rather
	 * than a restart. */
	cJSON *idle = cJSON_CreateObject();
	cJSON_AddBoolToObject(idle, "enable", config.bar_idle_enable);
	cJSON_AddNumberToObject(idle, "dpms_timeout", config.bar_idle_dpms_timeout);
	cJSON_AddNumberToObject(idle, "lock_timeout", config.bar_idle_lock_timeout);
	cJSON_AddNumberToObject(idle, "suspend_timeout",
							config.bar_idle_suspend_timeout);
	cJSON_AddBoolToObject(idle, "lock_before_suspend",
						  config.bar_idle_lock_before_suspend);
	cJSON_AddBoolToObject(idle, "respect_inhibitors",
						  config.bar_idle_respect_inhibitors);
	cJSON_AddStringToObject(idle, "lock_command", config.bar_idle_lock_command);
	cJSON_AddStringToObject(idle, "on_idle", config.bar_idle_on_idle);
	cJSON_AddStringToObject(idle, "on_resume", config.bar_idle_on_resume);
	cJSON_AddItemToObject(resp, "idle", idle);

	cJSON *panel = cJSON_CreateObject();
	cJSON_AddBoolToObject(panel, "enable", config.bar_panel_enable);
	cJSON_AddNumberToObject(panel, "radius", config.bar_panel_radius);
	cJSON_AddNumberToObject(panel, "padding", config.bar_panel_padding);
	cJSON_AddBoolToObject(panel, "blur", config.bar_panel_blur);
	cJSON_AddBoolToObject(panel, "shadow", config.bar_panel_shadow);
	cJSON_AddNumberToObject(panel, "shadow_size", config.bar_panel_shadow_size);
	cJSON_AddNumberToObject(panel, "shadow_blur", config.bar_panel_shadow_blur);
	bar_cfg_color(panel, "color", config.bar_panel_color);
	bar_cfg_color(panel, "shadow_color", config.bar_panel_shadow_color);
	cJSON_AddItemToObject(resp, "panel", panel);

	cJSON *pop = cJSON_CreateObject();
	cJSON_AddNumberToObject(pop, "width", config.bar_popover_width);
	cJSON_AddNumberToObject(pop, "row_height", config.bar_popover_row_height);
	cJSON_AddNumberToObject(pop, "spacing", config.bar_popover_spacing);
	cJSON_AddNumberToObject(pop, "padding", config.bar_popover_padding);
	cJSON_AddNumberToObject(pop, "gap", config.bar_popover_gap);
	bar_cfg_color(pop, "color", config.bar_popover_color);
	cJSON_AddItemToObject(resp, "popover", pop);

	/* The shared UI theme, not a bar-private one: titlebars, the overview and
	 * the bar have always drawn from this, and a bar in another process must
	 * keep doing so or it stops matching the desktop on the next matugen run. */
	cJSON *theme = cJSON_CreateObject();
	bar_cfg_color(theme, "fg", config.theme.fg_color);
	bar_cfg_color(theme, "bg", config.theme.bg_color);
	bar_cfg_color(theme, "focus_fg", config.theme.focus_fg_color);
	bar_cfg_color(theme, "focus_bg", config.theme.focus_bg_color);
	bar_cfg_color(theme, "urgent", config.theme.urgent_color);
	bar_cfg_color(theme, "border", config.theme.border_color);
	cJSON_AddNumberToObject(theme, "border_width", config.theme.border_width);
	cJSON_AddNumberToObject(theme, "corner_radius", config.theme.corner_radius);
	cJSON_AddNumberToObject(theme, "padding_x", config.theme.padding_x);
	cJSON_AddNumberToObject(theme, "padding_y", config.theme.padding_y);
	/* The same fallback the renderers use. set_value_default now assigns it, so
	 * this only catches the window between a reload freeing the string and the
	 * new one being parsed -- but reporting "" there told the bar to pick its
	 * own font while every native overlay drew at monospace Bold 16. */
	cJSON_AddStringToObject(theme, "font",
							config.theme.font_desc ? config.theme.font_desc
												   : "monospace Bold 16");
	cJSON_AddItemToObject(resp, "theme", theme);

	cJSON *custom = cJSON_CreateArray();
	for (int32_t i = 0; i < config.bar_custom_count; i++) {
		const ConfigBarCustom *c = &config.bar_custom[i];
		cJSON *o = cJSON_CreateObject();
		cJSON_AddStringToObject(o, "name", c->name);
		cJSON_AddStringToObject(o, "exec", c->exec);
		cJSON_AddStringToObject(o, "icon", c->icon);
		cJSON_AddStringToObject(o, "on_click", c->on_click);
		cJSON_AddStringToObject(o, "on_click_right", c->on_click_right);
		cJSON_AddNumberToObject(o, "interval", c->interval);
		cJSON_AddBoolToObject(o, "continuous", c->continuous);
		cJSON_AddItemToArray(custom, o);
	}
	cJSON_AddItemToObject(resp, "custom", custom);
	return resp;
}
#endif /* ASTEROIDZ_NATIVE_BAR */

/* The one-shot client whose command is being served right now.
 *
 * handle_command reaches its replies through a dozen paths (`send_static_json`
 * early-outs, the dispatch fast path, the cJSON tail) and threading a client
 * pointer through all of them would be a large diff for no gain: the IPC socket
 * is serviced from the compositor's own event loop, one command at a time, and
 * there is no reentrancy for it to get wrong. */
static struct ipc_client_state *ipc_serving;

static void send_static_json(int fd, const char *json_str) {
	(void)fd;
	if (!ipc_serving)
		return;
	ipc_out_append(&ipc_serving->out, json_str, strlen(json_str));
}

/* The cursor-shape protocol's names, as the protocol spells them.
 *
 * Its own names rather than any of ours: a caller comparing against "pointer"
 * is comparing against wp_cursor_shape_device_v1, which is also what the client
 * asked for, so there is no third vocabulary in between to keep in step. The
 * two DND-only shapes and the resize family are all here for the same reason --
 * a table that covers most of an enum is one somebody has to check. */
static const char *cursor_shape_name(enum wp_cursor_shape_device_v1_shape s) {
	switch (s) {
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT: return "default";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CONTEXT_MENU: return "context-menu";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_HELP: return "help";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER: return "pointer";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_PROGRESS: return "progress";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_WAIT: return "wait";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CELL: return "cell";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CROSSHAIR: return "crosshair";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT: return "text";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_VERTICAL_TEXT: return "vertical-text";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALIAS: return "alias";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COPY: return "copy";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_MOVE: return "move";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NO_DROP: return "no-drop";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NOT_ALLOWED: return "not-allowed";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRAB: return "grab";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRABBING: return "grabbing";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_E_RESIZE: return "e-resize";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_N_RESIZE: return "n-resize";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NE_RESIZE: return "ne-resize";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NW_RESIZE: return "nw-resize";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_S_RESIZE: return "s-resize";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SE_RESIZE: return "se-resize";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SW_RESIZE: return "sw-resize";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_W_RESIZE: return "w-resize";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE: return "ew-resize";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NS_RESIZE: return "ns-resize";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NESW_RESIZE: return "nesw-resize";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NWSE_RESIZE: return "nwse-resize";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COL_RESIZE: return "col-resize";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ROW_RESIZE: return "row-resize";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALL_SCROLL: return "all-scroll";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ZOOM_IN: return "zoom-in";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ZOOM_OUT: return "zoom-out";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DND_ASK: return "dnd-ask";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALL_RESIZE: return "all-resize";
	}
	/* 0 is not a member: it is what the tracker holds before any client has
	 * named a shape, and after one attached a surface instead. */
	return "unset";
}

/* ---------- one-shot command handling ---------- */
static void handle_command(int client_fd, const char *cmd_raw) {
	cJSON *resp = NULL;
	char *json_str = NULL;
	char cmd[1024];

	strncpy(cmd, cmd_raw, sizeof(cmd) - 1);
	cmd[sizeof(cmd) - 1] = '\0';
	for (char *p = cmd; *p; p++)
		if (*p == ',')
			*p = ' ';

	if (strcmp(cmd, "get version") == 0) {
		resp = cJSON_CreateObject();
		cJSON_AddStringToObject(resp, "version", VERSION);
	} else if (strcmp(cmd, "get cursorpos") == 0) {
		resp = cJSON_CreateObject();
		cJSON_AddNumberToObject(resp, "x", cursor->x);
		cJSON_AddNumberToObject(resp, "y", cursor->y);
		Monitor *m = xytomon(cursor->x, cursor->y);
		if (m)
			cJSON_AddStringToObject(resp, "monitor", m->wlr_output->name);
		else
			cJSON_AddNullToObject(resp, "monitor");
		/* What the pointer currently LOOKS like, which is the only way to check
		 * a client's hover cursors from outside it: the cursor is drawn by the
		 * compositor and does not appear in a screenshot, so a test driving a
		 * bar or a settings window has nothing else to assert against. Tracked
		 * here already -- setcursorshape() and the set_cursor request both
		 * write it -- and simply never reported.
		 *
		 * A client may name a shape or attach a surface of its own, and those
		 * are different answers rather than degrees of one: `shape` is null
		 * when a surface is in use. */
		if (last_cursor.surface) {
			cJSON_AddNullToObject(resp, "cursor-shape");
			cJSON_AddBoolToObject(resp, "cursor-surface", true);
		} else {
			cJSON_AddStringToObject(resp, "cursor-shape",
									cursor_shape_name(last_cursor.shape));
			cJSON_AddBoolToObject(resp, "cursor-surface", false);
		}
	} else if (strcmp(cmd, "get idle") == 0) {
		resp = build_idle_response();
	} else if (strcmp(cmd, "get keymode") == 0) {
		resp = cJSON_CreateObject();
		cJSON_AddStringToObject(resp, "keymode", keymode.mode);
	} else if (strcmp(cmd, "get keyboardlayout") == 0) {
		resp = cJSON_CreateObject();
		cJSON_AddStringToObject(resp, "layout", ipc_get_layout_str());
	} else if (strcmp(cmd, "get last_open_surface") == 0 ||
			   strncmp(cmd, "get last_open_surface ", 22) == 0) {
		Monitor *m;
		if (cmd[21] == '\0') { // exactly "get last_open_surface"
			m = selmon;
		} else {
			m = monitor_by_name(cmd + 22);
		}
		if (!m) {
			send_static_json(client_fd, "{\"error\":\"monitor not found\"}\n");
			return;
		}
		resp = cJSON_CreateObject();
		cJSON_AddStringToObject(resp, "monitor", m->wlr_output->name);
		cJSON_AddStringToObject(resp, "last_open_surface",
								m->last_open_surface);
	} else if (strncmp(cmd, "get monitor ", 12) == 0) {
		Monitor *m = monitor_by_name(cmd + 12);
		if (!m) {
			send_static_json(client_fd, "{\"error\":\"monitor not found\"}\n");
			return;
		}
		resp = build_monitor_json(m);
	} else if (strcmp(cmd, "get focused-client") == 0) {
		if (selmon && selmon->sel) {
			resp = build_client_json(selmon->sel);
		} else {
			send_static_json(client_fd, "{\"error\":\"no focused client\"}\n");
			return;
		}
	} else if (strncmp(cmd, "get client ", 11) == 0) {
		Client *c = client_by_id((uint32_t)atoi(cmd + 11));
		if (!c) {
			send_static_json(client_fd, "{\"error\":\"client not found\"}\n");
			return;
		}
		resp = build_client_json(c);
	} else if (strncmp(cmd, "get tag ", 8) == 0) {
		char mon_name[64];
		int ext_tag_idx;
		if (sscanf(cmd + 8, "%63s %d", mon_name, &ext_tag_idx) != 2) {
			send_static_json(
				client_fd,
				"{\"error\":\"usage: get tag <monitor> <index>\"}\n");
			return;
		}
		int tag_idx = ext_tag_idx - 1;
		Monitor *m = monitor_by_name(mon_name);
		if (!m || tag_idx < 0 || tag_idx >= LENGTH(tags)) {
			send_static_json(client_fd,
							 "{\"error\":\"invalid monitor or tag index\"}\n");
			return;
		}
		uint32_t tagmask = 1 << tag_idx;
		int numclients = 0, focused_client = 0;
		bool is_active = false, is_urgent = false;
		if (tagmask & m->tagset[m->seltags])
			is_active = true;

		Client *c, *focused = focustop(m);
		wl_list_for_each(c, &clients, link) {
			if (c->mon != m || !(c->tags & tagmask))
				continue;
			if (c == focused)
				focused_client = 1;
			if (c->isurgent)
				is_urgent = true;
			numclients++;
		}
		resp = cJSON_CreateObject();
		cJSON_AddStringToObject(resp, "monitor", m->wlr_output->name);
		cJSON_AddNumberToObject(resp, "tag_index", ext_tag_idx);
		cJSON_AddBoolToObject(resp, "is_active", is_active);
		cJSON_AddBoolToObject(resp, "is_urgent", is_urgent);
		cJSON_AddNumberToObject(resp, "client_count", numclients);
		cJSON_AddBoolToObject(resp, "focused_client", focused_client);
	} else if (strcmp(cmd, "get all-clients") == 0) {
		cJSON *arr = cJSON_CreateArray();
		Client *c;
		wl_list_for_each(c, &clients, link)
			cJSON_AddItemToArray(arr, build_client_json(c));
		resp = cJSON_CreateObject();
		cJSON_AddItemToObject(resp, "clients", arr);
	} else if (strcmp(cmd, "get all-monitors") == 0) {
		cJSON *arr = cJSON_CreateArray();
		Monitor *m;
		wl_list_for_each(m, &mons, link)
			cJSON_AddItemToArray(arr, build_monitor_json(m));
		resp = cJSON_CreateObject();
		cJSON_AddItemToObject(resp, "monitors", arr);
	} else if (strcmp(cmd, "get all-tags") == 0) {
		resp = build_all_tags_response();
#ifdef ASTEROIDZ_NATIVE_BAR
	} else if (strcmp(cmd, "get bar-config") == 0) {
		resp = build_bar_config_response();
#endif
	} else if (strcmp(cmd, "get config-schema") == 0) {
		resp = build_config_schema_response(NULL);
	} else if (strncmp(cmd, "get config-schema ", 18) == 0) {
		resp = build_config_schema_response(cmd + 18);
	} else if (strcmp(cmd, "get config-schema-digest") == 0) {
		resp = build_config_schema_digest_response();
	} else if (strcmp(cmd, "get config") == 0) {
		resp = build_config_response(NULL);
	} else if (strncmp(cmd, "get config ", 11) == 0) {
		resp = build_config_response(cmd + 11);
	} else if (strcmp(cmd, "get dispatch-actions") == 0) {
		resp = build_dispatch_actions_response();
	} else if (strcmp(cmd, "get window-rule-schema") == 0) {
		resp = build_rule_schema_response();
	} else if (strcmp(cmd, "get window-rules") == 0) {
		resp = build_window_rules_response();
	} else if (strcmp(cmd, "get tag-rule-schema") == 0) {
		resp = build_tag_rule_schema_response();
	} else if (strcmp(cmd, "get tag-rules") == 0) {
		resp = build_tag_rules_response();
	} else if (strcmp(cmd, "get binds") == 0) {
		resp = build_binds_response();
	} else if (strcmp(cmd, "capture-chord") == 0) {
		/* No reply now. The next key press is the reply. */
		if (chord_capture.active) {
			send_static_json(client_fd,
							 "{\"ok\":false,\"error\":\"busy\","
							 "\"detail\":\"another client is capturing\"}\n");
			return;
		}
		chord_capture.active = true;
		chord_capture.client = ipc_serving;
		chord_capture.fd = client_fd;
		if (ipc_serving)
			ipc_serving->deferred = true;
		return;
	} else if (strncmp(cmd, "get tags ", 9) == 0) {
		Monitor *m = monitor_by_name(cmd + 9);
		if (!m) {
			send_static_json(client_fd, "{\"error\":\"monitor not found\"}\n");
			return;
		}
		resp = build_monitor_tags_response(m);
	} else if (strncmp(cmd_raw, "set-tag-rules ", 14) == 0) {
		resp = handle_set_tag_rules(cmd_raw + 14);
	} else if (strncmp(cmd_raw, "set-window-rules ", 17) == 0) {
		resp = handle_set_window_rules(cmd_raw + 17);
	} else if (strncmp(cmd_raw, "set-binds ", 10) == 0) {
		resp = handle_set_binds(cmd_raw + 10);
	} else if (strncmp(cmd_raw, "set-config ", 11) == 0) {
		/* cmd_raw, not cmd: the copy has had every comma turned into a space,
		 * and this body is JSON. */
		resp = handle_set_config(cmd_raw + 11);
	} else if (strncmp(cmd, "dispatch ", 9) == 0) {
		char *dispatch_copy = strdup(cmd_raw + 9);
		char *out = dispatch_copy, *ptr = dispatch_copy;
		int client_id = -1;
		while (*ptr) {
			while (*ptr == ' ' || *ptr == '\t')
				*out++ = *ptr++;
			if (strncmp(ptr, "client,", 7) == 0) {
				char *end;
				long id = strtol(ptr + 7, &end, 10);
				if (id > 0 && end > ptr + 7 && (*end == '\0' || *end == ',')) {
					client_id = (int)id;
					ptr = end;
					if (*ptr == ',')
						ptr++;
					continue;
				}
			}
			*out++ = *ptr++;
		}
		*out = '\0';

		char *tokens[6] = {NULL};
		int token_count = 0;
		char *saveptr;
		char *token = strtok_r(dispatch_copy, ",", &saveptr);
		while (token && token_count < 6) {
			while (*token == ' ' || *token == '\t')
				token++;
			char *end = token + strlen(token) - 1;
			while (end >= token && (*end == ' ' || *end == '\t'))
				*end-- = '\0';
			tokens[token_count++] = token;
			token = strtok_r(NULL, ",", &saveptr);
		}

		Arg arg = {0};
		int32_t (*func)(const Arg *) = parse_func_name(
			token_count > 0 ? tokens[0] : "", &arg,
			token_count > 1 ? tokens[1] : "", token_count > 2 ? tokens[2] : "",
			token_count > 3 ? tokens[3] : "", token_count > 4 ? tokens[4] : "",
			token_count > 5 ? tokens[5] : "");

		if (func && client_id > 0)
			arg.tc = client_by_id((uint32_t)client_id);

		if (func) {
			/* NOTE: "success" means the dispatch NAME parsed and the function
			 * was called -- not that it did anything. The return value is
			 * discarded here, so `set_output_mode` refusing an unsupported
			 * mode, or `toggle_hdr` being overridden by `hdr-mode`, both
			 * answer success:true.
			 *
			 * Not fixed in place because the return conventions differ across
			 * dispatches -- plenty return 0 unconditionally, so reporting
			 * `ret != 0` would report failure for most of the working ones and
			 * break every consumer at once. It needs an audit of all of them
			 * first; until then, the log is the honest channel. */
			func(&arg);
			send_static_json(client_fd, "{\"success\":true}\n");
		} else {
			send_static_json(client_fd, "{\"error\":\"unknown function\"}\n");
		}

		if (arg.v)
			free(arg.v);
		if (arg.v2)
			free(arg.v2);
		if (arg.v3)
			free(arg.v3);
		free(dispatch_copy);
		return; // Fast path exit
	} else {
		send_static_json(client_fd, "{\"error\":\"unknown command\"}\n");
		return;
	}

	if (resp) {
		json_str = cJSON_PrintUnformatted(resp);
		if (json_str) {
			size_t len = strlen(json_str);
			char *msg = malloc(len + 2);
			if (msg) {
				snprintf(msg, len + 2, "%s\n", json_str);
				if (ipc_serving)
					ipc_out_append(&ipc_serving->out, msg, len + 1);
				free(msg);
			}
			free(json_str);
		}
		cJSON_Delete(resp);
	}
	(void)client_fd;
}

/* ---------- watch mode support ---------- */

/* Queue one already-newline-terminated message on a watcher, dropping it if
 * the write fails outright or its backlog has run away. */
static void ipc_watch_send(struct ipc_watch_client *wc, const char *msg,
						   size_t len) {
	if (!ipc_out_append(&wc->out, msg, len) ||
		!ipc_out_flush(wc->fd, &wc->out)) {
		ipc_remove_watch_client(wc);
		return;
	}
	wl_event_source_fd_update(wc->source,
							  WL_EVENT_READABLE | WL_EVENT_HANGUP |
								  WL_EVENT_ERROR |
								  (ipc_out_pending(&wc->out) ? WL_EVENT_WRITABLE
															 : 0));
}

static void ipc_notify_json_to_fd(int fd, cJSON *json) {
	struct ipc_watch_client *wc, *tmp, *found = NULL;
	wl_list_for_each_safe(wc, tmp, &watch_clients, link) {
		if (wc->fd == fd) {
			found = wc;
			break;
		}
	}
	if (!found)
		return;
	char *str = cJSON_PrintUnformatted(json);
	if (!str)
		return;
	size_t len = strlen(str);
	char *msg = malloc(len + 2);
	if (!msg) {
		free(str);
		return;
	}
	snprintf(msg, len + 2, "%s\n", str);
	ipc_watch_send(found, msg, len + 1);
	free(msg);
	free(str);
}

static void ipc_remove_watch_client(struct ipc_watch_client *wc) {
	wl_list_remove(&wc->link);
	wl_event_source_remove(wc->source);
	close(wc->fd);
	ipc_out_reset(&wc->out);
	free(wc);
}

static int ipc_watch_data_handler(int fd, uint32_t mask, void *data) {
	struct ipc_watch_client *wc = data;
	if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
		ipc_remove_watch_client(wc);
		return 0;
	}
	if (mask & WL_EVENT_WRITABLE) {
		if (!ipc_out_flush(fd, &wc->out)) {
			ipc_remove_watch_client(wc);
			return 0;
		}
		if (!ipc_out_pending(&wc->out))
			wl_event_source_fd_update(wc->source, WL_EVENT_READABLE |
													  WL_EVENT_HANGUP |
													  WL_EVENT_ERROR);
	}
	if (mask & WL_EVENT_READABLE) {
		char buf[64];
		ssize_t n = recv(fd, buf, sizeof(buf), 0);
		if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
			ipc_remove_watch_client(wc);
		}
	}
	return 0;
}

static bool handle_watch_command(int fd, const char *cmd,
								 struct ipc_client_state *client) {
	enum ipc_watch_type type = IPC_WATCH_NONE;
	const char *arg = NULL;
	uint32_t client_id = 0;

	if (strncmp(cmd, "watch monitor ", 14) == 0) {
		type = IPC_WATCH_MONITOR;
		arg = cmd + 14;
	} else if (strcmp(cmd, "watch focused-client") == 0) {
		type = IPC_WATCH_FOCUSED_CLIENT;
	} else if (strncmp(cmd, "watch client ", 13) == 0) {
		type = IPC_WATCH_CLIENT;
		client_id = (uint32_t)atoi(cmd + 13);
	} else if (strncmp(cmd, "watch tags ", 11) == 0) {
		type = IPC_WATCH_TAGS;
		arg = cmd + 11;
	} else if (strcmp(cmd, "watch all-monitors") == 0) {
		type = IPC_WATCH_ALL_MONITORS;
	} else if (strcmp(cmd, "watch all-tags") == 0) {
		type = IPC_WATCH_ALL_TAGS;
	} else if (strcmp(cmd, "watch all-clients") == 0) {
		type = IPC_WATCH_ALL_CLIENTS;
#ifdef ASTEROIDZ_NATIVE_BAR
	} else if (strcmp(cmd, "watch config") == 0) {
		type = IPC_WATCH_CONFIG;
	} else if (strcmp(cmd, "watch bar-config") == 0) {
		type = IPC_WATCH_BAR_CONFIG;
#endif
	} else if (strcmp(cmd, "watch idle") == 0) {
		type = IPC_WATCH_IDLE;
	} else if (strcmp(cmd, "watch keymode") == 0) {
		type = IPC_WATCH_KEYMODE;
	} else if (strcmp(cmd, "watch keyboardlayout") == 0) {
		type = IPC_WATCH_KB_LAYOUT;
	} else if (strcmp(cmd, "watch last_open_surface") == 0 ||
			   strncmp(cmd, "watch last_open_surface ", 24) == 0) {
		type = IPC_WATCH_LAST_OPEN_SURFACE;
		if (cmd[24] != '\0') { // has argument after the space
			arg = cmd + 24;
		} else {
			arg = NULL; // default to selmon
		}
	}

	if (type == IPC_WATCH_NONE)
		return false;

	struct ipc_watch_client *wc = calloc(1, sizeof(*wc));
	wc->fd = fd;
	wc->type = type;

	if ((type == IPC_WATCH_MONITOR || type == IPC_WATCH_LAST_OPEN_SURFACE) &&
		arg)
		snprintf(wc->target.monitor.name, sizeof(wc->target.monitor.name), "%s",
				 arg);
	else if (type == IPC_WATCH_TAGS && arg)
		snprintf(wc->target.tags.mon_name, sizeof(wc->target.tags.mon_name),
				 "%s", arg);
	else if (type == IPC_WATCH_CLIENT)
		wc->target.client.id = client_id;

	wl_event_source_remove(client->source);
	wc->source = wl_event_loop_add_fd(
		client->loop, fd, WL_EVENT_READABLE | WL_EVENT_HANGUP | WL_EVENT_ERROR,
		ipc_watch_data_handler, wc);
	wl_list_insert(&watch_clients, &wc->link);

	/* push the initial state */
	cJSON *json = NULL;
	switch (type) {
	case IPC_WATCH_MONITOR: {
		Monitor *m = monitor_by_name(arg);
		if (m)
			json = build_monitor_json(m);
		break;
	}
	case IPC_WATCH_LAST_OPEN_SURFACE: {
		Monitor *m = NULL;
		if (arg) {
			m = monitor_by_name(arg);
		} else {
			m = selmon;
		}
		if (m) {
			json = cJSON_CreateObject();
			cJSON_AddStringToObject(json, "monitor", m->wlr_output->name);
			cJSON_AddStringToObject(json, "last_open_surface",
									m->last_open_surface);
		}
		break;
	}
	case IPC_WATCH_FOCUSED_CLIENT: {
		if (selmon && selmon->sel) {
			json = build_client_json(selmon->sel);
		} else {
			json = cJSON_CreateObject();
			cJSON_AddNullToObject(json, "id");
			cJSON_AddNullToObject(json, "title");
			cJSON_AddNullToObject(json, "appid");
		}
		break;
	}
	case IPC_WATCH_CLIENT: {
		Client *c = client_by_id(client_id);
		if (c)
			json = build_client_json(c);
		break;
	}
	case IPC_WATCH_TAGS: {
		Monitor *m = monitor_by_name(arg);
		if (m)
			json = build_monitor_tags_response(m);
		break;
	}
	case IPC_WATCH_ALL_MONITORS: {
		cJSON *arr = cJSON_CreateArray();
		Monitor *m;
		wl_list_for_each(m, &mons, link)
			cJSON_AddItemToArray(arr, build_monitor_json(m));
		json = cJSON_CreateObject();
		cJSON_AddItemToObject(json, "monitors", arr);
		break;
	}
	case IPC_WATCH_ALL_TAGS: {
		json = build_all_tags_response();
		break;
	}
	case IPC_WATCH_ALL_CLIENTS: {
		cJSON *arr = cJSON_CreateArray();
		Client *c;
		wl_list_for_each(c, &clients, link)
			cJSON_AddItemToArray(arr, build_client_json(c));
		json = cJSON_CreateObject();
		cJSON_AddItemToObject(json, "clients", arr);
		break;
	}
#ifdef ASTEROIDZ_NATIVE_BAR
	case IPC_WATCH_CONFIG: {
		/* Everything, once, so the subscriber starts from a known state -- every
		 * push after this one is a diff and a client that joined mid-stream
		 * would otherwise have nothing to apply them to. */
		json = build_config_diff_response("initial", true);
		config_snapshot();
		break;
	}
	case IPC_WATCH_BAR_CONFIG: {
		json = build_bar_config_response();
		break;
	}
#endif
	case IPC_WATCH_IDLE: {
		json = build_idle_response();
		break;
	}
	case IPC_WATCH_KEYMODE: {
		json = cJSON_CreateObject();
		cJSON_AddStringToObject(json, "keymode", keymode.mode);
		break;
	}
	case IPC_WATCH_KB_LAYOUT: {
		json = cJSON_CreateObject();
		cJSON_AddStringToObject(json, "layout", ipc_get_layout_str());
		break;
	}
	default:
		break;
	}

	if (json) {
		ipc_notify_json_to_fd(fd, json);
		cJSON_Delete(json);
	}

	/* The one-shot state is discarded: the fd now belongs to the watch client,
	 * which has an output queue of its own. Nothing can be queued on this one
	 * yet, but freeing it without resetting would be a leak the moment
	 * anything ever is. */
	ipc_out_reset(&client->out);
	free(client->buf);
	free(client);
	return true;
}

/* ---------- socket event handling ---------- */
static int ipc_handle_client_data(int fd, uint32_t mask, void *data) {
	struct ipc_client_state *client = data;
	if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR))
		goto cleanup;

	/* The rest of a reply that did not fit the socket buffer first time. */
	if (mask & WL_EVENT_WRITABLE) {
		if (!ipc_out_flush(fd, &client->out))
			goto cleanup;
		if (ipc_out_pending(&client->out))
			return 0;
		if (client->closing)
			goto cleanup;
		wl_event_source_fd_update(client->source, WL_EVENT_READABLE |
													  WL_EVENT_HANGUP |
													  WL_EVENT_ERROR);
		return 0;
	}

	if (mask & WL_EVENT_READABLE) {
		size_t available = client->buf_cap - client->buf_len;
		if (available < 4096) {
			size_t new_cap = client->buf_cap ? client->buf_cap * 2 : 8192;
			char *new_buf = realloc(client->buf, new_cap);
			if (!new_buf) {
				wlr_log(WLR_ERROR, "IPC: out of memory");
				goto cleanup;
			}
			client->buf = new_buf;
			client->buf_cap = new_cap;
			available = client->buf_cap - client->buf_len;
		}

		ssize_t n = recv(fd, client->buf + client->buf_len, available - 1, 0);
		if (n <= 0)
			goto cleanup;

		client->buf_len += n;
		client->buf[client->buf_len] = '\0';

		char *nl = memchr(client->buf, '\n', client->buf_len);
		if (!nl) {
			if (client->buf_len > 1024 * 1024)
				goto cleanup;
			return 0;
		}
		*nl = '\0';
		char *cmd = client->buf;

		bool is_watch = handle_watch_command(fd, cmd, client);
		if (is_watch)
			return 0;

		ipc_serving = client;
		client->deferred = false;
		handle_command(fd, cmd);
		ipc_serving = NULL;

		/* A handler that answers later keeps its connection. The consumed
		 * command is dropped from the buffer first, so a second line already in
		 * flight is not re-read as a fresh request. */
		if (client->deferred) {
			size_t consumed = (size_t)(nl - client->buf) + 1;
			memmove(client->buf, client->buf + consumed,
					client->buf_len - consumed);
			client->buf_len -= consumed;
			return 0;
		}

		/* The connection closes when the REPLY IS OUT, not when the handler
		 * returns. Closing here dropped whatever the socket buffer could not
		 * take -- which for everything served until now was nothing, and for
		 * a config schema is most of it. */
		if (!ipc_out_flush(fd, &client->out))
			goto cleanup;
		if (!ipc_out_pending(&client->out))
			goto cleanup;
		client->closing = true;
		wl_event_source_fd_update(client->source,
								  WL_EVENT_WRITABLE | WL_EVENT_HANGUP |
									  WL_EVENT_ERROR);
		return 0;
	}
	return 0;

cleanup:
	/* Before the struct is freed, or a chord captured afterwards would be
	 * written to a file descriptor number that has already been handed to
	 * somebody else. */
	chord_capture_cancel(client);
	close(client->fd);
	wl_event_source_remove(client->source);
	ipc_out_reset(&client->out);
	free(client->buf);
	free(client);
	return 0;
}

static int ipc_handle_connection(int fd, uint32_t mask, void *data) {
	struct wl_event_loop *loop = data;
	int client_fd = accept(fd, NULL, NULL);
	if (client_fd < 0)
		return 0;

	// set O_NONBLOCK
	int flags = fcntl(client_fd, F_GETFL, 0);
	fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);
	// set FD_CLOEXEC
	flags = fcntl(client_fd, F_GETFD, 0);
	fcntl(client_fd, F_SETFD, flags | FD_CLOEXEC);

	struct ipc_client_state *client = calloc(1, sizeof(*client));
	client->fd = client_fd;
	client->loop = loop;
	client->source = wl_event_loop_add_fd(
		loop, client_fd, WL_EVENT_READABLE | WL_EVENT_HANGUP | WL_EVENT_ERROR,
		ipc_handle_client_data, client);
	return 0;
}

/* ---------- external notification interface ---------- */

void ipc_notify_monitor(Monitor *m) {
	char *json_str = NULL;
	size_t len = 0;
	struct ipc_watch_client *wc, *tmp;
	wl_list_for_each_safe(wc, tmp, &watch_clients, link) {
		if (wc->type == IPC_WATCH_MONITOR &&
			strcmp(m->wlr_output->name, wc->target.monitor.name) == 0) {
			if (!json_str) {
				cJSON *json = build_monitor_json(m);
				char *raw = cJSON_PrintUnformatted(json);
				cJSON_Delete(json);
				if (!raw)
					return;
				len = strlen(raw);
				json_str = malloc(len + 2);
				snprintf(json_str, len + 2, "%s\n", raw);
				free(raw);
			}
			ipc_watch_send(wc, json_str, len + 1);
		}
	}
	if (json_str)
		free(json_str);
}

void ipc_notify_last_surface_ws_name(Monitor *m) {
	char *json_str = NULL;
	size_t len = 0;
	struct ipc_watch_client *wc, *tmp;
	wl_list_for_each_safe(wc, tmp, &watch_clients, link) {
		if (wc->type != IPC_WATCH_LAST_OPEN_SURFACE)
			continue;

		bool match = false;
		if (wc->target.monitor.name[0] == '\0') {
			if (m == selmon)
				match = true;
		} else {
			if (strcmp(m->wlr_output->name, wc->target.monitor.name) == 0)
				match = true;
		}

		if (!match)
			continue;

		if (!json_str) {
			cJSON *json = cJSON_CreateObject();
			cJSON_AddStringToObject(json, "monitor", m->wlr_output->name);
			cJSON_AddStringToObject(json, "last_open_surface",
									m->last_open_surface);
			char *raw = cJSON_PrintUnformatted(json);
			cJSON_Delete(json);
			if (!raw)
				return;
			len = strlen(raw);
			json_str = malloc(len + 2);
			snprintf(json_str, len + 2, "%s\n", raw);
			free(raw);
		}
		ipc_watch_send(wc, json_str, len + 1);
	}
	free(json_str);
}

void ipc_notify_focused_client(void) {
	char *json_str = NULL;
	size_t len = 0;
	struct ipc_watch_client *wc, *tmp;
	wl_list_for_each_safe(wc, tmp, &watch_clients, link) {
		if (wc->type == IPC_WATCH_FOCUSED_CLIENT) {
			if (!json_str) {
				cJSON *json = NULL;
				if (selmon && selmon->sel) {
					json = build_client_json(selmon->sel);
				} else {
					json = cJSON_CreateObject();
					cJSON_AddNullToObject(json, "id");
					cJSON_AddNullToObject(json, "title");
					cJSON_AddNullToObject(json, "appid");
				}
				char *raw = cJSON_PrintUnformatted(json);
				cJSON_Delete(json);
				if (!raw)
					return;
				len = strlen(raw);
				json_str = malloc(len + 2);
				snprintf(json_str, len + 2, "%s\n", raw);
				free(raw);
			}
			ipc_watch_send(wc, json_str, len + 1);
		}
	}
	free(json_str);
}

void ipc_notify_client(Client *c) {
	char *json_str = NULL;
	size_t len = 0;
	struct ipc_watch_client *wc, *tmp;
	wl_list_for_each_safe(wc, tmp, &watch_clients, link) {
		if (wc->type == IPC_WATCH_CLIENT && c->id == wc->target.client.id) {
			if (!json_str) {
				cJSON *json = build_client_json(c);
				char *raw = cJSON_PrintUnformatted(json);
				cJSON_Delete(json);
				if (!raw)
					return;
				len = strlen(raw);
				json_str = malloc(len + 2);
				snprintf(json_str, len + 2, "%s\n", raw);
				free(raw);
			}
			ipc_watch_send(wc, json_str, len + 1);
		}
	}
	if (json_str)
		free(json_str);
}

void ipc_notify_tags(Monitor *m) {
	char *json_str = NULL;
	size_t len = 0;
	struct ipc_watch_client *wc, *tmp;
	wl_list_for_each_safe(wc, tmp, &watch_clients, link) {
		if (wc->type == IPC_WATCH_TAGS &&
			strcmp(m->wlr_output->name, wc->target.tags.mon_name) == 0) {
			if (!json_str) {
				cJSON *json = build_monitor_tags_response(m);
				char *raw = cJSON_PrintUnformatted(json);
				cJSON_Delete(json);
				if (!raw)
					return;
				len = strlen(raw);
				json_str = malloc(len + 2);
				snprintf(json_str, len + 2, "%s\n", raw);
				free(raw);
			}
			ipc_watch_send(wc, json_str, len + 1);
		}
	}
	if (json_str)
		free(json_str);
}

void ipc_notify_all_monitors(void) {
	char *json_str = NULL;
	size_t len = 0;
	struct ipc_watch_client *wc, *tmp;
	wl_list_for_each_safe(wc, tmp, &watch_clients, link) {
		if (wc->type == IPC_WATCH_ALL_MONITORS) {
			if (!json_str) {
				cJSON *arr = cJSON_CreateArray();
				Monitor *m;
				wl_list_for_each(m, &mons, link)
					cJSON_AddItemToArray(arr, build_monitor_json(m));
				cJSON *json = cJSON_CreateObject();
				cJSON_AddItemToObject(json, "monitors", arr);
				char *raw = cJSON_PrintUnformatted(json);
				cJSON_Delete(json);
				if (!raw)
					return;
				len = strlen(raw);
				json_str = malloc(len + 2);
				snprintf(json_str, len + 2, "%s\n", raw);
				free(raw);
			}
			ipc_watch_send(wc, json_str, len + 1);
		}
	}
	if (json_str)
		free(json_str);
}

void ipc_notify_all_clients(void) {
	char *json_str = NULL;
	size_t len = 0;
	struct ipc_watch_client *wc, *tmp;
	wl_list_for_each_safe(wc, tmp, &watch_clients, link) {
		if (wc->type == IPC_WATCH_ALL_CLIENTS) {
			if (!json_str) {
				cJSON *arr = cJSON_CreateArray();
				Client *c;
				wl_list_for_each(c, &clients, link)
					cJSON_AddItemToArray(arr, build_client_json(c));
				cJSON *json = cJSON_CreateObject();
				cJSON_AddItemToObject(json, "clients", arr);
				char *raw = cJSON_PrintUnformatted(json);
				cJSON_Delete(json);
				if (!raw)
					return;
				len = strlen(raw);
				json_str = malloc(len + 2);
				snprintf(json_str, len + 2, "%s\n", raw);
				free(raw);
			}
			ipc_watch_send(wc, json_str, len + 1);
		}
	}
	if (json_str)
		free(json_str);
}

void ipc_notify_all_tags(void) {
	char *json_str = NULL;
	size_t len = 0;
	struct ipc_watch_client *wc, *tmp;
	wl_list_for_each_safe(wc, tmp, &watch_clients, link) {
		if (wc->type == IPC_WATCH_ALL_TAGS) {
			if (!json_str) {
				cJSON *json = build_all_tags_response();
				char *raw = cJSON_PrintUnformatted(json);
				cJSON_Delete(json);
				if (!raw)
					return;
				len = strlen(raw);
				json_str = malloc(len + 2);
				snprintf(json_str, len + 2, "%s\n", raw);
				free(raw);
			}
			ipc_watch_send(wc, json_str, len + 1);
		}
	}
	if (json_str)
		free(json_str);
}

#ifdef ASTEROIDZ_NATIVE_BAR
/* Called from reload_config(). A bar in another process cannot see the config
 * being re-read, and polling for it would mean either a lag between the reload
 * and the repaint or a timer that spends all day finding nothing changed. */
void ipc_notify_bar_config(void) {
	char *json_str = NULL;
	size_t len = 0;
	struct ipc_watch_client *wc, *tmp;
	wl_list_for_each_safe(wc, tmp, &watch_clients, link) {
		if (wc->type != IPC_WATCH_BAR_CONFIG)
			continue;
		if (!json_str) {
			cJSON *json = build_bar_config_response();
			char *raw = cJSON_PrintUnformatted(json);
			cJSON_Delete(json);
			if (!raw)
				return;
			len = strlen(raw);
			json_str = malloc(len + 2);
			snprintf(json_str, len + 2, "%s\n", raw);
			free(raw);
		}
		ipc_watch_send(wc, json_str, len + 1);
	}
	if (json_str)
		free(json_str);
}
#endif

/* Push whatever changed since the last push, to anyone watching the config.
 *
 * Called from reload_config, from setoption, and from the write path. The
 * snapshot is updated whether or not anyone is listening: otherwise the first
 * subscriber after a quiet period would be diffed against a state from before
 * however many changes happened while nobody was watching, and would be told
 * about all of them as if they had just occurred.
 *
 * A no-op push is skipped rather than sent as an empty object. A reload that
 * changes nothing -- which is most of them, since matugen fires on every
 * wallpaper change and the palette often lands on the same colours -- should
 * not wake a settings panel to tell it so. */
void ipc_notify_config(const char *reason) {
	bool any = false;
	struct ipc_watch_client *wc;
	wl_list_for_each(wc, &watch_clients, link) {
		if (wc->type == IPC_WATCH_CONFIG) {
			any = true;
			break;
		}
	}
	if (!any) {
		config_snapshot();
		return;
	}

	cJSON *json = build_config_diff_response(reason, false);
	cJSON *count = cJSON_GetObjectItem(json, "count");
	if (!count || count->valueint == 0) {
		cJSON_Delete(json);
		config_snapshot();
		return;
	}
	char *raw = cJSON_PrintUnformatted(json);
	cJSON_Delete(json);
	if (!raw) {
		config_snapshot();
		return;
	}
	size_t len = strlen(raw);
	char *json_str = malloc(len + 2);
	if (json_str) {
		snprintf(json_str, len + 2, "%s\n", raw);
		struct ipc_watch_client *tmp;
		wl_list_for_each_safe(wc, tmp, &watch_clients, link) {
			if (wc->type != IPC_WATCH_CONFIG)
				continue;
			ipc_watch_send(wc, json_str, len + 1);
		}
		free(json_str);
	}
	free(raw);
	config_snapshot();
}

void ipc_notify_idle(void) {
	char *json_str = NULL;
	size_t len = 0;
	struct ipc_watch_client *wc, *tmp;
	wl_list_for_each_safe(wc, tmp, &watch_clients, link) {
		if (wc->type != IPC_WATCH_IDLE)
			continue;
		if (!json_str) {
			cJSON *json = build_idle_response();
			char *raw = cJSON_PrintUnformatted(json);
			cJSON_Delete(json);
			if (!raw)
				return;
			len = strlen(raw);
			json_str = malloc(len + 2);
			snprintf(json_str, len + 2, "%s\n", raw);
			free(raw);
		}
		ipc_watch_send(wc, json_str, len + 1);
	}
	free(json_str);
}

void ipc_notify_keymode(void) {
	char *json_str = NULL;
	size_t len = 0;
	struct ipc_watch_client *wc, *tmp;
	wl_list_for_each_safe(wc, tmp, &watch_clients, link) {
		if (wc->type == IPC_WATCH_KEYMODE) {
			if (!json_str) {
				cJSON *json = cJSON_CreateObject();
				cJSON_AddStringToObject(json, "keymode", keymode.mode);
				char *raw = cJSON_PrintUnformatted(json);
				cJSON_Delete(json);
				if (!raw)
					return;
				len = strlen(raw);
				json_str = malloc(len + 2);
				snprintf(json_str, len + 2, "%s\n", raw);
				free(raw);
			}
			ipc_watch_send(wc, json_str, len + 1);
		}
	}
	if (json_str)
		free(json_str);
}

void ipc_notify_kb_layout(void) {
	char *json_str = NULL;
	size_t len = 0;
	struct ipc_watch_client *wc, *tmp;
	wl_list_for_each_safe(wc, tmp, &watch_clients, link) {
		if (wc->type == IPC_WATCH_KB_LAYOUT) {
			if (!json_str) {
				cJSON *json = cJSON_CreateObject();
				cJSON_AddStringToObject(json, "layout", ipc_get_layout_str());
				char *raw = cJSON_PrintUnformatted(json);
				cJSON_Delete(json);
				if (!raw)
					return;
				len = strlen(raw);
				json_str = malloc(len + 2);
				snprintf(json_str, len + 2, "%s\n", raw);
				free(raw);
			}
			ipc_watch_send(wc, json_str, len + 1);
		}
	}
	if (json_str)
		free(json_str);
}

/* ---------- init and cleanup ---------- */
static int ipc_sock_fd = -1;
static struct wl_event_source *ipc_event_source = NULL;
static char ipc_socket_path[256];

void ipc_init(struct wl_event_loop *event_loop) {
	wl_list_init(&watch_clients);

	const char *xdg_runtime = getenv("XDG_RUNTIME_DIR");
	if (!xdg_runtime)
		return;

	snprintf(ipc_socket_path, sizeof(ipc_socket_path), "%s/asteroidz-%d.sock",
			 xdg_runtime, getpid());

	ipc_sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (ipc_sock_fd < 0)
		return;

	// set FD_CLOEXEC
	int flags = fcntl(ipc_sock_fd, F_GETFD, 0);
	if (flags == -1 || fcntl(ipc_sock_fd, F_SETFD, flags | FD_CLOEXEC) == -1) {
		wlr_log(WLR_ERROR, "failed to set FD_CLOEXEC on IPC socket");
		close(ipc_sock_fd);
		return;
	}
	// set O_NONBLOCK
	flags = fcntl(ipc_sock_fd, F_GETFL, 0);
	if (flags == -1 || fcntl(ipc_sock_fd, F_SETFL, flags | O_NONBLOCK) == -1) {
		wlr_log(WLR_ERROR, "failed to set O_NONBLOCK on IPC socket");
		close(ipc_sock_fd);
		return;
	}

	struct sockaddr_un addr = {.sun_family = AF_UNIX};
	strncpy(addr.sun_path, ipc_socket_path, sizeof(addr.sun_path) - 1);

	unlink(ipc_socket_path);
	if (bind(ipc_sock_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(ipc_sock_fd);
		return;
	}
	listen(ipc_sock_fd, 16);

	setenv("ASTEROIDZ_INSTANCE_SIGNATURE", ipc_socket_path, 1);

	ipc_event_source =
		wl_event_loop_add_fd(event_loop, ipc_sock_fd, WL_EVENT_READABLE,
							 ipc_handle_connection, event_loop);
}

void ipc_cleanup(void) {
	if (ipc_event_source)
		wl_event_source_remove(ipc_event_source);
	if (ipc_sock_fd >= 0)
		close(ipc_sock_fd);
	unlink(ipc_socket_path);
	unsetenv("ASTEROIDZ_INSTANCE_SIGNATURE");

	struct ipc_watch_client *wc, *tmp;
	wl_list_for_each_safe(wc, tmp, &watch_clients, link)
		ipc_remove_watch_client(wc);
}