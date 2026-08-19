/*
 * See LICENSE file for copyright and license details.
 */
#include "wlr-layer-shell-unstable-v1-protocol.h"
#include "wlr/util/box.h"
#include "wlr/util/edges.h"
#include <cairo.h>
#include <fcntl.h>
#include <getopt.h>
#include <drm_fourcc.h>
#include <libinput.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <math.h>
/* The blur source dump, whose dispatch lives in dispatch/bind_define.h -- which
 * is included long before render/az_avk.h, so its one AVK header comes in
 * here. It depends on nothing of the compositor's. */
#include "render/vulkan/scene/avk_blur_dump.h"
#include <scene/fx/blur_data.h>
#include <scene/fx/clipped_region.h>
#include <scene/wlr_scene.h>
#include "common/corner_location.h"
/* Early, because the animation and overview code below asks what buffer a
 * surface is showing and must not answer it with a renderer wrapper. */
#include "render/az_output_color.h"
/* The one wlroots-colour -> az_lum_source_desc translation, shared by the
 * renderer's per-buffer path and the inspector's per-surface one. */
#include "render/az_source_desc.h"
#include "render/color/az_icc.h"
#include "render/az_surface.h"
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wayland-util.h>
#include <wlr/backend.h>
#include <wlr/backend/headless.h>
#include <wlr/backend/libinput.h>
#include <wlr/backend/multi.h>
#include <wlr/backend/wayland.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_alpha_modifier_v1.h>
#include <wlr/render/color.h>
#include <wlr/render/gles2.h>
#include <wlr/render/pixman.h>
#include <wlr/render/vulkan.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_color_management_v1.h>
#include <wlr/types/wlr_color_representation_v1.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_content_type_v1.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_drm.h>
#include <wlr/types/wlr_drm_lease_v1.h>
#include <wlr/types/wlr_export_dmabuf_v1.h>
#include <wlr/types/wlr_ext_data_control_v1.h>
#include <wlr/types/wlr_ext_foreign_toplevel_list_v1.h>
#include <wlr/types/wlr_ext_image_capture_source_v1.h>
#include <wlr/types/wlr_ext_image_copy_capture_v1.h>
/* privacy shield: count live capture sessions to know when to cover
 * privacy_shield surfaces */
#include <wlr/types/wlr_fixes.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_gamma_control_v1.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_keyboard_group.h>
#include <wlr/types/wlr_keyboard_shortcuts_inhibit_v1.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/types/wlr_linux_drm_syncobj_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_output_management_v1.h>
#include <wlr/types/wlr_output_power_management_v1.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_pointer_gestures_v1.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_security_context_v1.h>
#include <wlr/types/wlr_server_decoration.h>
#include <wlr/types/wlr_session_lock_v1.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_switch.h>
#include <wlr/types/wlr_tablet_pad.h>
#include <wlr/types/wlr_tablet_tool.h>
#include <wlr/types/wlr_tablet_v2.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_virtual_pointer_v1.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_dialog_v1.h>
#include <wlr/types/wlr_xdg_foreign_registry.h>
#include <wlr/types/wlr_xdg_foreign_v1.h>
#include <wlr/types/wlr_xdg_foreign_v2.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_xdg_system_bell_v1.h>
#include <wlr/types/wlr_xdg_toplevel_icon_v1.h>
#include <wlr/types/wlr_xdg_toplevel_tag_v1.h>
#include <wlr/util/addon.h>
#include <wlr/util/log.h>
#include <wlr/util/region.h>
#include <wordexp.h>
#include <xkbcommon/xkbcommon.h>
#ifdef XWAYLAND
#include <X11/Xlib.h>
#include <wlr/xwayland.h>
#include <xcb/xcb_icccm.h>
#endif
#include "common/pace.h"
#include "common/util.h"
#include "draw/text-node.h"
#include "draw/ufo-node.h"
#include "present/az_presenter.h"

/* macros */
#define ASTEROIDZ_MAX(A, B) ((A) > (B) ? (A) : (B))
#define ASTEROIDZ_MIN(A, B) ((A) < (B) ? (A) : (B))
#define GEZERO(A) ((A) >= 0 ? (A) : 0)
#define CLEANMASK(mask) (mask & ~WLR_MODIFIER_CAPS)
#define INSIDEMON(A)                                                           \
	(A->geom.x >= A->mon->m.x && A->geom.y >= A->mon->m.y &&                   \
	 A->geom.x + A->geom.width <= A->mon->m.x + A->mon->m.width &&             \
	 A->geom.y + A->geom.height <= A->mon->m.y + A->mon->m.height)
#define GEOMINSIDEMON(A, M)                                                    \
	(A->x >= M->m.x && A->y >= M->m.y &&                                       \
	 A->x + A->width <= M->m.x + M->m.width &&                                 \
	 A->y + A->height <= M->m.y + M->m.height)
#define ISTILED(A)                                                             \
	(A && !(A)->isfloating && !(A)->isminimized && !(A)->iskilling &&          \
	 !(A)->ismaximizescreen && !(A)->isfullscreen && !(A)->isunglobal)
#define ISNORMAL(A)                                                            \
	(A && !(A)->isminimized && !(A)->iskilling && !(A)->isunglobal)
#define ISSCROLLTILED(A)                                                       \
	(A && !(A)->isfloating && !(A)->isminimized && !(A)->iskilling &&          \
	 !(A)->isunglobal)
#define ISFAKETILED(A)                                                         \
	(A && !(A)->isfloating && !(A)->isminimized && !(A)->iskilling &&          \
	 !(A)->isunglobal)
/* Named special workspaces (Hyprland-style) overlay whatever tag is active
 * on a monitor: (M)->active_special is NULL when no special workspace is
 * showing on that monitor, or the interned name of the one that is. Clients
 * carry the interned name of the special workspace they belong to in
 * (C)->special_name (NULL for ordinary clients). A client whose
 * special_name is set is only visible while its monitor's active_special
 * points at that same interned string; every other (non-special) client is
 * hidden while any special workspace is showing, exactly like switching to
 * an overlay tag. Pinned/global/unglobal clients keep their previous
 * "always visible" exemption so they still show through on top of a special
 * workspace, matching how they are exempted from tag-switch animations. */
#define VISIBLEON(C, M)                                                       \
	((C) && (M) && (C)->mon == (M) &&                                          \
	 ((C)->isglobal || (C)->isunglobal || (C)->ispinned ||                     \
	  ((C)->special_name != NULL                                              \
		   ? (C)->special_name == (M)->active_special                         \
		   : ((M)->active_special == NULL &&                                  \
			  ((C)->tags & (M)->tagset[(M)->seltags])))))

#define TAGMATCH(C, M)                                                         \
	((C) && (M) && (C)->mon == (M) && (((C)->tags & (M)->tagset[(M)->seltags])))

#define LENGTH(X) (sizeof X / sizeof X[0])
#define END(A) ((A) + LENGTH(A))
#define TAGMASK ((1 << LENGTH(tags)) - 1)
/* upper bound on overview tag-cells; `tags` (LENGTH(tags)) isn't defined
 * until preset.h is included below the Monitor struct, so per-monitor
 * overview arrays use this fixed cap and clamp to LENGTH(tags) at runtime */
#define OV_TAG_CELLS 32
#define OV_STRIP_WINS 64 /* window snapshots shown across all strip tiles */
#define LISTEN(E, L, H) wl_signal_add((E), ((L)->notify = (H), (L)))
#define ISFULLSCREEN(A)                                                        \
	((A)->isfullscreen || (A)->ismaximizescreen ||                             \
	 (A)->overview_ismaximizescreenbak || (A)->overview_isfullscreenbak)
#define LISTEN_STATIC(E, H)                                                    \
	do {                                                                       \
		struct wl_listener *_l = ecalloc(1, sizeof(*_l));                      \
		_l->notify = (H);                                                      \
		wl_signal_add((E), _l);                                                \
	} while (0)

#define APPLY_INT_PROP(obj, rule, prop)                                        \
	if (rule->prop >= 0)                                                       \
	obj->prop = rule->prop

#define APPLY_FLOAT_PROP(obj, rule, prop)                                      \
	if (rule->prop > 0.0f)                                                     \
	obj->prop = rule->prop

#define APPLY_STRING_PROP(obj, rule, prop)                                     \
	if (rule->prop != NULL)                                                    \
	obj->prop = rule->prop

#define BAKED_POINTS_COUNT 256

#define IPC_WATCH_ARRANGGE                                                     \
	IPC_WATCH_MONITOR | IPC_WATCH_CLIENT | IPC_WATCH_TAGS |                    \
		IPC_WATCH_ALL_MONITORS | IPC_WATCH_ALL_TAGS | IPC_WATCH_ALL_CLIENTS |  \
		IPC_WATCH_LAST_OPEN_SURFACE | IPC_WATCH_FOCUSED_CLIENT

/* enums */
enum { TOP_LEFT, TOP_RIGHT, BOTTOM_LEFT, BOTTOM_RIGHT };

enum { VERTICAL, HORIZONTAL };
enum { SWIPE_UP, SWIPE_DOWN, SWIPE_LEFT, SWIPE_RIGHT };
enum { CurNormal, CurPressed, CurMove, CurResize }; /* cursor */
enum {
	XDGShell,
	LayerShell,
	X11,
	Snapshot,
	XdgPopup,
	XdgImPopup
}; /* client types (the tag lives in the first struct member so generic
	  scene node.data walks in xytonode can identify the owner) */
enum { AxisUp, AxisDown, AxisLeft, AxisRight }; // scroll wheel direction
enum {
	LyrBg,
	LyrBlur,
	LyrBottom,
	LyrDecorate,
	/* Every TILED window's shadow, below every tiled window.
	 *
	 * A shadow used to live at the bottom of its own client's tree, which
	 * is only "below" that one window -- and a shadow is the window's box
	 * plus its spread, so it reaches into whatever is beside it. Between
	 * two tiles that neighbour is another window, drawn from a sibling tree
	 * under LyrTile, so whichever window was raised last had its shadow
	 * painted over the other one: the dark edge moved from window to window
	 * as focus changed, and the backdrop blur sampled the neighbour's own
	 * pixels and smeared them along the seam.
	 *
	 * The fix is stacking, not arithmetic. With every tiled shadow beneath
	 * every tiled window, no shadow can reach another window's pixels no
	 * matter what order the windows are in, and what survives is what
	 * should: the gaps between tiles, and the outside edge of the layout
	 * where the wallpaper actually is. It also makes the backdrop blur
	 * correct for free -- it now samples a frame with no tiled window drawn
	 * into it yet, which is what "the backdrop" means.
	 *
	 * FLOATING windows keep their shadow inside their own tree, where it
	 * belongs: a floating window really is above the tiles, and its shadow
	 * really does fall on them. client_sync_shadow_tree() is what moves a
	 * window's shadow between the two as it floats and tiles. */
	LyrTileShadow,
	LyrTile,
	LyrMaximize,
	LyrTop,
	LyrFS, /* fullscreen clients: always above panels/bars on LyrTop,
			* always below LyrOverlay (notifications, lockscreen) */
	LyrFadeOut,
	LyrOverlay,
	LyrIMPopup, // text-input layer
	LyrBlock,
	LyrScreenshot, // in-compositor screenshot overlay, always topmost
	NUM_LAYERS
}; /* scene layers */

/* values must not collide with the client-type enum above: tab bar nodes
 * store a AsteroidzNodeData whose first member is this enum, and xytonode now
 * reads the first uint32_t of node.data generically */
enum asteroidz_node_type {
	ASTEROIDZ_TITLE_NODE = 100,
	ASTEROIDZ_jump_label_node,
	ASTEROIDZ_TITLEBAR_NODE,
	ASTEROIDZ_TITLEBAR_CLOSE_NODE
};

#ifdef XWAYLAND
enum {
	NetWMWindowTypeDialog,
	NetWMWindowTypeSplash,
	NetWMWindowTypeToolbar,
	NetWMWindowTypeUtility,
	NetLast
}; /* EWMH atoms */
#endif
enum { UP, DOWN, LEFT, RIGHT, UNDIR }; /* smartmovewin */
enum { NONE, OPEN, MOVE, CLOSE, TAG, FOCUS, OPAFADEIN, OPAFADEOUT, OVERVIEW };
enum { UNFOLD, FOLD, INVALIDFOLD };
enum { PREV, NEXT };
enum { STATE_UNSPECIFIED = 0, STATE_ENABLED, STATE_DISABLED };
enum { FORCE, UNFORCE };

enum tearing_mode {
	TEARING_DISABLED = 0,
	TEARING_ENABLED,
	TEARING_FULLSCREEN_ONLY,
};

enum seat_config_shortcuts_inhibit {
	SHORTCUTS_INHIBIT_DISABLE,
	SHORTCUTS_INHIBIT_ENABLE,
};

enum ipc_watch_type {
	IPC_WATCH_NONE = 0,
	IPC_WATCH_MONITOR = 1 << 0,
	IPC_WATCH_CLIENT = 1 << 1,
	IPC_WATCH_TAGS = 1 << 2,
	IPC_WATCH_ALL_MONITORS = 1 << 3,
	IPC_WATCH_ALL_TAGS = 1 << 4,
	IPC_WATCH_ALL_CLIENTS = 1 << 5,
	IPC_WATCH_KEYMODE = 1 << 6,
	IPC_WATCH_KB_LAYOUT = 1 << 7,
	IPC_WATCH_LAST_OPEN_SURFACE = 1 << 8,
	IPC_WATCH_FOCUSED_CLIENT = 1 << 9,
	/* The resolved bar geometry and theme, for an out-of-process bar.
	 *
	 * A bar that lives in another process still has to look like it belongs to
	 * this compositor: the same palette, the same font, the same pill
	 * geometry. Handing it the config file to parse would be two KDL readers
	 * that agree until the day they do not, and matugen rewrites the palette
	 * at runtime anyway. So the compositor serves what it RESOLVED -- after
	 * defaults, after clamping, after the theme file -- and pushes it again
	 * whenever the config is reloaded. */
	IPC_WATCH_BAR_CONFIG = 1 << 10,
	/* Every option in the schema, pushed as a DIFF when anything changes.
	 *
	 * A settings panel showing the live value of an option has to hear about a
	 * change it did not make -- a matugen palette rewrite, `amsg dispatch
	 * set_option`, someone editing the file and reloading -- or it sits there
	 * displaying something that stopped being true. A diff rather than the whole
	 * set because a palette reload touches nine keys out of ninety-five. */
	IPC_WATCH_CONFIG = 1 << 11,
	/* Whether idling is being held off, and by whom.
	 *
	 * `toggle_idle_inhibit` was write-only: a bar could flip it and had no way
	 * to ask what it was, so the pill that flips it had to REMEMBER what it had
	 * done. That guess is wrong after a bar restart, after a keybind flips the
	 * same state, and after any client takes an inhibitor out -- and being
	 * wrong here means the machine never sleeps while the icon says it will. */
	IPC_WATCH_IDLE = 1 << 12,
};

typedef struct Pertag Pertag;
typedef struct Monitor Monitor;
typedef struct Client Client;

struct dvec2 {
	double x, y;
};

struct ivec2 {
	int32_t x, y, width, height;
};

typedef struct {
	int32_t i;
	int32_t i2;
	float f;
	float f2;
	char *v;
	char *v2;
	char *v3;
	uint32_t ui;
	uint32_t ui2;
	Client *tc;
} Arg;

typedef struct {
	enum asteroidz_node_type type;
	void *node_data;
} AsteroidzNodeData;

/* xytonode/handle_buttonpress read the FIRST uint32 of any scene node.data as
 * a type tag, punned across Client, LayerSurface and
 * AsteroidzNodeData -- pin the convention at compile time. */
_Static_assert(offsetof(AsteroidzNodeData, type) == 0,
			   "node.data type tag must be the first member");

typedef struct {
	uint32_t mod;
	uint32_t button;
	int32_t (*func)(const Arg *);
	const Arg arg;
} Button; // mouse button

typedef struct {
	char mode[28];
	bool isdefault;
} KeyMode;

typedef struct {
	uint32_t mod;
	uint32_t dir;
	int32_t (*func)(const Arg *);
	const Arg arg;
} Axis;

typedef struct {
	struct wl_list link;
	struct wlr_input_device *wlr_device;
	struct libinput_device *libinput_device;
	struct wl_listener destroy_listener;
	void *device_data;
} InputDevice;

typedef struct {
	struct wl_list link;
	struct wlr_switch *wlr_switch;
	struct wl_listener toggle;
	InputDevice *input_dev;
} Switch;

/*
 * ── THE FOUR AXES A GEOMETRY ANIMATION MOVES ALONG ────────────────────────
 *
 * A window's animated geometry is four independent scalars, and a spring
 * retarget has to be able to carry a different initial velocity into each of
 * them (ADR-608). The order is the one dwl_animation.spring_v0 is indexed by
 * and the one the AZ_PACE trace prints; it is not otherwise meaningful.
 */
enum anim_axis {
	ANIM_AXIS_X = 0,
	ANIM_AXIS_Y,
	ANIM_AXIS_W,
	ANIM_AXIS_H,
	ANIM_AXIS_COUNT
};

struct dwl_animation {
	bool should_animate;
	bool running;
	bool tagining;
	bool tagouted;
	bool tagouting;
	bool begin_fade_in;
	bool tag_from_rule;
	bool overining;
	uint32_t time_started;
	/*
	 * The same instant in NANOSECONDS, and the one the interpolator reads.
	 *
	 * time_started is milliseconds, so at 165Hz two ticks land in the same
	 * millisecond often enough to matter and at 240Hz a millisecond is a
	 * quarter of a frame. Both read the same progress, compute the same
	 * position, and produce a committed frame with no damage at all -- the
	 * same class of defect as truncating the geometry, one level up. The ms
	 * field stays because other code reads it; nothing that decides where a
	 * window is does.
	 */
	uint64_t time_started_ns;
	uint32_t duration;
	struct wlr_box initial;
	struct wlr_box current;
	int32_t action;
	/*
	 * AZ_BREAK_ANIM_FRAME_STEP only (falsifier I4): how many ticks this
	 * animation has received. The break derives progress from this instead of
	 * from elapsed time, which is what a frame-stepped animation actually is.
	 * Zero and unread in every normal build.
	 */
	uint64_t break_ticks;
	/*
	 * The instant this animation was most recently EVALUATED at (ADR-608).
	 *
	 * A retarget seeds the new segment with the position the old one had
	 * reached -- and under target-time sampling that position was computed for
	 * a moment in the FUTURE, not for now. Starting the new clock at CPU-now
	 * while seeding with X(s) claims the window is at X(s) at time now, when
	 * it will not be there until s: the lead interval gets counted twice and
	 * the window jumps forward by up to one frame's travel. Anchoring the new
	 * segment at `s` instead is the one place presentation-time sampling
	 * changes retarget arithmetic.
	 */
	uint64_t last_sample_ns;
	/*
	 * The spring's initial velocity, in normalised curve units (dy/dt at
	 * t == 0). Zero for a fresh animation -- the window is at rest.
	 *
	 * Non-zero only after a RETARGET, where it carries the speed the outgoing
	 * motion had reached so the new segment does not stop dead and set off
	 * again (ADR-608). Beziers ignore it: that curve family has no state to
	 * inject, and substituting a spring under a configured bezier would change
	 * motion the operator chose.
	 *
	 * ── ONE PER AXIS, AND WHY THE SCALAR WAS NOT ENOUGH ───────────────────
	 *
	 * This was a single scalar, and the seeding code projected the outgoing
	 * velocity onto the new direction of travel -- so the component ACROSS
	 * that direction was dropped. A 90-degree retarget is entirely that
	 * component: a window travelling east, redirected north, was projected
	 * onto north with an outgoing velocity that had no north in it, seeded
	 * with v0 == 0, and started the new segment from rest. The stall the
	 * velocity-continuity work existed to remove survived in exactly the turn
	 * that shows it most.
	 *
	 * Four axes, four independent springs, four v0s. Each axis carries its own
	 * outgoing speed and none of them is projected away. The old comment here
	 * named the fix and declined it ("that needs a factor per axis"); this is
	 * that factor.
	 */
	double spring_v0[ANIM_AXIS_COUNT];
	/*
	 * Where this segment was heading. `initial` is where it began; the target
	 * lives in Client.current, which is overwritten with the NEW target before
	 * a retarget gets a chance to read it -- so the outgoing segment's
	 * direction is unrecoverable without keeping it here.
	 *
	 * That is not hypothetical: the first version of the velocity-continuity
	 * code computed the outgoing velocity from `current - initial` at retarget
	 * time and got the NEW segment's direction, producing a v0 of the wrong
	 * sign that accelerated into the turn instead of carrying through it.
	 */
	struct wlr_box target;
};

struct dwl_opacity_animation {
	bool running;
	float current_opacity;
	float target_opacity;
	float initial_opacity;
	/* 1.0 = focused look, 0.0 = unfocused: drives shadow dim + blur
	 * strength together with the opacity/border transition */
	float initial_effect;
	float target_effect;
	float current_effect;
	uint32_t time_started;
	uint32_t duration;
	float current_border_color[4];
	float target_border_color[4];
	float initial_border_color[4];
};

typedef struct {
	float width_scale;
	float height_scale;
	int32_t width;
	int32_t height;
	enum corner_location corner_location;
	bool should_scale;
	bool ov_live; /* overview live thumbnail: allow down-scaling the surface */
	/* overview viewport-edge crop: visible fraction of the root surface
	 * (0..1 each); crop_active applies it as the buffer source box. NB: do NOT
	 * use wlr_scene_subsurface_tree_set_clip for this -- asteroidz-scenefx does
	 * not implement it, so the call binds to vanilla wlroots' walker, which
	 * mangles scenefx's scene structs (UB; the surface just disappears). */
	bool crop_active;
	bool crop_clear; /* a previous crop must be removed (don't touch otherwise:
					  * clearing unconditionally would stomp viewporter source
					  * boxes of normal clients, e.g. video players) */
	float crop_l, crop_t, crop_w, crop_h;
	uint32_t bw; /* client's actual border width, for content-radius inset --
				  * set by buffer_set_effect(), appended at the end so it
				  * doesn't shift existing positional-initializer call sites */
} BufferData;

/* one tile of the "fall" close animation; defined in animation/client.h */
struct FalloutShard;
struct ShatterEmitter;
/* the vector break-up of the "asteroid" close animation */
typedef struct AsteroidBreak AsteroidBreak;

struct Client {
	/* Must keep these three elements in this order */
	uint32_t type; /* XDGShell or X11* */
	struct wlr_box geom, pending, float_geom, animainit_geom,
		overview_backup_geom, current,
		drag_begin_geom; /* layout-relative, includes border */
	Monitor *mon;
	struct wlr_scene_tree *scene;
	struct wlr_scene_rect *border; /* top, bottom, left, right */
	struct wlr_scene_rect *droparea;
	struct wlr_scene_rect *shield; /* covers content during captures */
	struct wlr_scene_rect *splitindicator[4];
	struct wlr_scene_shadow *shadow;
	struct wlr_scene_shadow *contact_shadow;
	/* blurs the backdrop within the ambient shadow's own footprint (opt-in,
	 * shadows_blur_background), so the shadow reads as soft even over a
	 * detailed wallpaper instead of just tinting its sharp detail darker.
	 * Created/destroyed on demand like blur_node, not just enabled/disabled
	 * like shadow/contact_shadow -- it has the same real GPU cost. */
	struct wlr_scene_blur *shadow_blur;
	/* Holds shadow, contact_shadow and shadow_blur -- all three together,
	 * so moving a window's shadows between LyrTileShadow and its own tree
	 * is one reparent rather than three, and their order among themselves
	 * survives the move.
	 *
	 * Positioned to match c->scene while it sits on LyrTileShadow, and at
	 * the origin while it sits inside c->scene. That is what lets the three
	 * nodes keep the SAME local coordinates in both cases: every box in
	 * client_draw_one_shadow() is relative to the window's own origin, and
	 * none of that arithmetic has to know where the tree currently lives. */
	struct wlr_scene_tree *shadow_tree;
	struct wlr_scene_blur *blur_node;
	struct wlr_scene_tree *scene_surface;
	struct wlr_scene_tree *overview_scene_surface;
	struct asteroidz_jump_label_node *jump_label_node;
	struct asteroidz_tab_bar_node *titlebar_node;
	struct asteroidz_tab_bar_node *titlebar_close_node;
	struct asteroidz_icon_node *ov_icon; /* app icon on overview thumbnail */
	struct asteroidz_jump_label_node *ov_title; /* title under the overview icon */
	struct wlr_buffer *ov_snap_buf; /* frozen surface buffer for strip snapshot */
	struct wlr_box ov_clip;         /* overview big-area surface crop (viewport) */
	bool ov_clip_active;            /* ov_clip should override the ov_live clip */
	bool ov_crop_set; /* a source-box crop is applied and must be cleared later */
	struct wl_list link;
	struct wl_list flink;
	struct wl_list fadeout_link;
	/* "fall" close animation only, and only on the throwaway fadeout client
	 * init_fadeout_client() allocates: the window snapshot sliced into a grid
	 * of tiles that scatter and fall. NULL for every real client. */
	struct FalloutShard *shards;
	int32_t nshards;
	/* "asteroid" close animation only, on the throwaway fadeout client: the
	 * window replaced by a vector rock that splits and tumbles. NULL for every
	 * real client, and never set at the same time as `shards` -- the two are
	 * different animations, not two halves of one. */
	AsteroidBreak *rocks;
	/* "shatter" close animation only, on the throwaway fadeout client: the
	 * window's own pixels as a grid of fragments that tumble and fall. NULL
	 * for every real client, and never set at the same time as `shards` or
	 * `rocks` -- the three are different animations. Needs AVK; the SceneFX
	 * path cannot rotate a primitive and falls back to "fall". */
	struct ShatterEmitter *shatter;
	union {
		struct wlr_xdg_surface *xdg;
		struct wlr_xwayland_surface *xwayland;
	} surface;
	struct wlr_ext_foreign_toplevel_handle_v1 *ext_foreign_toplevel;
	char *icon_name;	/* xdg-toplevel-icon-v1, may be NULL */
	char *toplevel_tag; /* xdg-toplevel-tag-v1, may be NULL */
	struct wl_listener commit;
	struct wl_listener map;
	struct wl_listener maximize;
	struct wl_listener minimize;
	struct wl_listener unmap;
	struct wl_listener destroy;
	struct wl_listener set_title;
	struct wl_listener fullscreen;
#ifdef XWAYLAND
	struct wl_listener activate;
	struct wl_listener associate;
	struct wl_listener dissociate;
	struct wl_listener configure;
	struct wl_listener set_hints;
	struct wl_listener set_geometry;
	struct wl_listener commmitx11;
#endif
	uint32_t bw;
	/* X11 ONLY, and 1 for everything else. How many of this client's own
	 * pixels go into one logical pixel -- see the long note above
	 * client_x11_scale() in src/client/client.h. Recomputed when the client's
	 * monitor is decided and whenever it changes; never read directly, always
	 * through client_x11_scale(), which is what enforces the "1 at or below
	 * scale 1" rule in one place. */
	float x11_scale;
	uint32_t tags, oldtags, mini_restore_tag;
	bool dirty;
	uint32_t configure_serial;
	struct wlr_foreign_toplevel_handle_v1 *foreign_toplevel;
	int32_t isfloating, isurgent, isfullscreen, isfakefullscreen,
		need_float_size_reduce, isminimized, isoverlay, isnosizehint,
		ignore_maximize, ignore_minimize, idleinhibit_when_focus;
	int32_t ismaximizescreen;
	/* last xdg_toplevel.suspended state we configured, so the sweep in
	 * update_client_suspended() only sends a configure when it flips */
	bool issuspended;
	int32_t overview_backup_bw;
	int32_t fullscreen_backup_x, fullscreen_backup_y, fullscreen_backup_w,
		fullscreen_backup_h;
	int32_t overview_isfullscreenbak, overview_ismaximizescreenbak,
		overview_isfloatingbak;

	struct wlr_xdg_toplevel_decoration_v1 *decoration;
	struct wl_listener foreign_activate_request;
	struct wl_listener foreign_fullscreen_request;
	struct wl_listener foreign_close_request;
	struct wl_listener foreign_destroy;
	struct wl_listener foreign_minimize_request;
	struct wl_listener foreign_maximize_request;
	struct wl_listener set_decoration_mode;
	struct wl_listener destroy_decoration;

	const char *animation_type_open;
	const char *animation_type_close;
	int32_t is_in_scratchpad;
	int32_t iscustomsize;
	int32_t iscustompos;
	int32_t autofloated; /* floated by the float layout, not the user: cleared
						  * (and re-tiled) when the tag leaves that layout */
	int32_t cascaded;	 /* float layout: cascade slot assigned (float_geom
						  * x/y); assigned once, reapplied as the client's
						  * real size settles */
	int32_t iscustom_scroller_proportion;
	int32_t iscustom_scroller_proportion_single;
	int32_t is_scratchpad_show;
	int32_t isglobal;
	int32_t isnoborder;
	int32_t isnoshadow;
	int32_t isnotitlebar;
	int32_t noscanout;
	int32_t vrr_only_fullscreen;
	int32_t force_hdr;
	int32_t privacy_shield;
	int32_t isnoradius;
	int32_t isnoanimation;
	int32_t isopensilent;
	int32_t istagsilent;
	int32_t iskilling;
	int32_t istagswitching;
	int32_t isnamedscratchpad;
	char *special_name; /* interned name of the named special workspace this
						 * client belongs to, NULL if not in one */
	bool is_monocle_hide;
	bool is_pending_open_animation;
	bool is_restoring_from_ov;
	float scroller_proportion;
	float stack_proportion;
	float old_stack_proportion;
	bool need_output_flush;
	struct dwl_animation animation;
	struct dwl_opacity_animation opacity_animation;
	int32_t isterm, noswallow;
	int32_t allow_csd;
	int32_t force_ssd;
	int32_t force_fakemaximize;
	int32_t force_tiled_state;
	pid_t pid;
	Client *swallowing, *swallowedby;
	bool is_clip_to_hide;
	/* overview: window is off the viewport (scrolled away) so it's hidden
	 * from its Mission-Control cell and counted in the "+N" badge; cleared on
	 * overview exit. applybounds() clamps off-screen geometry back on-screen,
	 * so the only reliable way to hide it is disabling its scene node. */
	bool is_overview_hidden;
	bool drag_to_tile;
	bool scratchpad_switching_mon;
	bool fake_no_border;
	int32_t nofocus;
	int32_t nofadein;
	int32_t nofadeout;
	int32_t no_force_center;
	int32_t isunglobal;
	float focused_opacity;
	float unfocused_opacity;
	/* M5, ADR-006: the per-window luminance levers. 1.0 is "unchanged", not
	 * 0 -- these are multipliers into the scene, and a zero would make the
	 * window black rather than leave it alone. The rule spells unset as 0 and
	 * APPLY_FLOAT_PROP only overwrites above zero, so the two conventions meet
	 * without either having to know about the other. */
	float sdr_white_scale;
	float hdr_gain;
	/* M12: the luminance class this window was given by rule, as a string;
	 * NULL or "" means none was, and the class is derived from the source. */
	const char *luminance_domain;
	/* M13: the presentation class by rule; NULL/"" means derive it. */
	const char *presentation_class;
	/*
	 * M13: HOW FAST THIS CLIENT COMMITS, which is not how fast the output
	 * presents. A 23.976fps film on a 144Hz panel is the case the VIDEO class
	 * exists for, and the first question about it -- is the content's cadence
	 * even reaching the compositor cleanly -- had no instrument. Two adds and a
	 * subtract per commit; no allocation, nothing per frame.
	 *
	 * Kept as sum/count rather than an EMA so the mean is a mean: an EMA of a
	 * cadence that alternates 5 and 7 vblanks reports the average and hides
	 * that it never once hit 6.
	 */
	uint64_t commit_count;
	uint64_t commit_last_ns;
	uint64_t commit_interval_sum_ns;
	uint64_t commit_interval_n;
	char oldmonname[128];
	uint32_t oldmontags; /* tagset oldmonname's monitor had active when this
						  * client landed there; used to restore the client
						  * onto a sane tag if oldmonname later reconnects as
						  * a brand-new Monitor (fresh pertag, default tag) */
	int32_t noblur;
	double master_mfact_per, master_inner_per, stack_inner_per;
	double old_master_mfact_per, old_master_inner_per, old_stack_inner_per;
	double old_scroller_pproportion;
	bool ismaster;
	bool old_ismaster;
	bool cursor_in_upper_half, cursor_in_left_half;
	bool isleftstack;
	int32_t tearing_hint;
	int32_t force_tearing;
	int32_t allow_shortcuts_inhibit;
	float scroller_proportion_single;
	bool isfocused;
	char jump_char;
	bool enable_drop_area_draw;
	int32_t drop_direction;
	struct wlr_box drag_tile_float_backup_geom;
	float grid_col_per;
	float grid_row_per;
	float old_grid_col_per;
	float old_grid_row_per;
	int32_t grid_col_idx;
	int32_t grid_row_idx;
	uint32_t id;
	int32_t ispinned;
	/* When this client last lost focus (ms). Used for focus-stealing
	 * prevention: an X11 app that re-fires request_activate right after the
	 * user switched away from it must not yank the view back to its tag. */
	uint32_t last_unfocus_ms;
	/* 0 = no grace period active; otherwise a get_now_in_ms() deadline until
	 * which direct scanout stays force-disabled after a fullscreen client's
	 * enter/exit animation finishes. A just-settled fullscreen client isn't
	 * necessarily stable enough yet to safely hand its buffer straight to a
	 * KMS plane; the animation-tick-scoped prevention alone left a narrow
	 * window right at the edges of the transition. */
	uint32_t scanout_grace_until_ms;
};

typedef struct {
	struct wl_list link;
	struct wl_resource *resource;
	Monitor *mon;
} DwlIpcOutput;

typedef struct {
	uint32_t mod;
	xkb_keysym_t keysym;
	int32_t (*func)(const Arg *);
	const Arg arg;
} Key;

typedef struct {
	struct wlr_keyboard_group *wlr_group;

	int32_t nsyms;
	const xkb_keysym_t *keysyms; /* invalid if nsyms == 0 */
	uint32_t mods;				 /* invalid if nsyms == 0 */
	uint32_t keycode;
	struct wl_event_source *key_repeat_source;

	struct wl_listener modifiers;
	struct wl_listener key;
	struct wl_listener destroy;

	/* Keycodes whose PRESS a compositor binding consumed, so their RELEASE
	 * can be swallowed too.
	 *
	 * A press-only bind (the normal kind) matches on press, sets `handled`
	 * and returns before forwarding -- but the release matches nothing, falls
	 * through, and reaches the focused client as a release with no
	 * corresponding press. Most toolkits discard that; Proton/Windows games
	 * under gamescope track raw key state and act on it, which is how Super+1
	 * ended up reaching a running game while also switching tags.
	 *
	 * Small and fixed: a chord is a handful of keys, and the worst case for a
	 * lost entry is one release that is forwarded when it need not be. */
	uint32_t consumed[16];
	int32_t nconsumed;

	/* The keycode currently being offered to the binding tables, or 0.
	 *
	 * Whether a press is consumed is only known once keybinding() returns --
	 * but a binding that switches tags changes FOCUS while it runs, and the
	 * wl_keyboard.enter it emits is built right then, before anything could
	 * have been added to `consumed`. So the enter's held-key array must also
	 * exclude the key still in flight.
	 *
	 * Filtering a key that turns out NOT to be bound is harmless: an enter can
	 * only be emitted inside this window by a binding that acted on this very
	 * key, and if none acted, no focus change happened at all. Press only --
	 * on release wlroots has already dropped the key from keycodes[], so
	 * there is nothing left to filter. */
	uint32_t dispatching;

	uint32_t layout_index;
} KeyboardGroup;

typedef struct {
	struct wlr_keyboard_shortcuts_inhibitor_v1 *inhibitor;
	struct wl_listener destroy;
	struct wl_list link;
} KeyboardShortcutsInhibitor;

typedef struct {
	/* Must keep these three elements in this order */
	uint32_t type; /* LayerShell */
	struct wlr_box geom, current, pending, animainit_geom;
	Monitor *mon;
	struct wlr_scene_tree *scene;
	struct wlr_scene_tree *popups;
	struct wlr_scene_shadow *shadow;
	struct wlr_scene_blur *shadow_blur; /* see Client's shadow_blur: blurs the
										 * backdrop within the shadow's own
										 * footprint (shadows_blur_background) */
	struct wlr_scene_blur *blur_node;
	struct wlr_scene_layer_surface_v1 *scene_layer;
	struct wl_list link;
	struct wl_list fadeout_link;
	int32_t mapped;
	struct wlr_layer_surface_v1 *layer_surface;

	struct wl_listener destroy;
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener surface_commit;

	struct dwl_animation animation;
	bool dirty;
	int32_t noblur;
	int32_t forceblur;
	int32_t privacy_shield;
	struct wlr_scene_rect *shield;
	int32_t noanim;
	int32_t noshadow;
	int32_t forceshadow;
	char *animation_type_open;
	char *animation_type_close;
	/* M12/M13: the luminance class by rule; NULL means derive it. */
	const char *luminance_domain;
	bool need_output_flush;
	bool being_unmapped;
} LayerSurface;

typedef struct {
	uint32_t type; // must be first in struct
	struct wlr_xdg_popup *wlr_popup;
	struct wl_listener destroy;
	struct wl_listener commit;
	struct wl_listener reposition;
	/* Blur, for a popup that asks for it via ext-background-effect-v1.
	 * `commit` above is a one-shot -- it removes itself after the initial
	 * commit -- so the region and size need a listener that lasts. */
	struct wlr_scene_blur *blur_node;
	struct wl_listener surface_commit;
	bool watching_surface;
} Popup;

typedef struct {
	const char *symbol;
	void (*arrange)(Monitor *);
	const char *name;
	uint32_t id;
} Layout;

/* Defined in src/render/az_avk.h, which cannot be included this early because
 * it needs Monitor. A monitor only ever holds a pointer to it. */
struct az_avk_output;

struct Monitor {
	struct wl_list link;
	struct wlr_output *wlr_output;
	struct wlr_scene_output *scene_output;
	/* Per-frame output state owned by the tearing-control path
	 * (ext-protocol/tearing.h), which inits it, commits it and re-inits it
	 * each frame. Nothing else may stage state here expecting it to survive:
	 * requestmonstate used to, and apply_tear_state's wlr_output_state_init
	 * silently threw the staged resize away every frame. */
	struct wlr_output_state pending;
	struct wl_listener frame;
	/* AZ_PACE=1 only (see common/pace.h) -- not wired otherwise. */
	struct wl_listener pace_present;
	uint64_t pace_last_present_ns;
	/*
	 * M6A.1. PRESENTATION FEEDBACK, IN PRODUCTION.
	 *
	 * Wired unconditionally, unlike pace_present above. M6A's whole subject is
	 * the loop "predict a presentation time, present, correct from what
	 * actually happened", and until now the compositor discarded the only
	 * signal that closes it -- while wlr_presentation_create() handed the same
	 * feedback to every CLIENT.
	 *
	 * Nothing here predicts anything yet. These are the raw observed facts, so
	 * that the model built on top of them can be checked against something
	 * that was measured rather than assumed.
	 */
	struct wl_listener present;
	/* M6A/ADR-602. The per-output timing owner. Embedded by value, never
	 * pointed to: a pointer member invites exactly the stale-across-hotplug
	 * class this milestone exists to make impossible. */
	struct az_presenter presenter;
	uint64_t present_last_ns;   /* ev->when of the last PRESENTED frame */
	uint64_t present_last_seq;
	uint64_t present_count;     /* frames that actually reached the screen */
	uint64_t present_dropped;   /* ev->presented false: signal, but no photons */
	uint64_t present_no_stamp;  /* presented, but the backend gave no time */
	/*
	 * OBSERVED interval, from the presented series, in nanoseconds. Kept
	 * beside the nominal figure rather than instead of it: DP-1 reports
	 * 143.999 Hz and a predictor built on the round number accrues phase error
	 * against reality (audit G5).
	 */
	uint64_t present_interval_ns;
	/* M13B: the last frame's scanout verdict (enum az_scanout_verdict) and how
	 * many frames went straight to the display. The verdict is kept per output
	 * rather than per client because it answers a question about the OUTPUT --
	 * "did this display composite this frame" -- and because a refusal often
	 * has no client to hang it on. */
	int32_t scanout_verdict;
	uint64_t scanout_frames;
	/*
	 * WHAT THE TORN-FLIP PATH DID WITH THE FRAMES IT WAS HANDED.
	 *
	 * `tear_torn` landed as an immediate flip. `tear_test_refused` never asked
	 * for one, because the backend's test said this state is not tearable.
	 * `tear_busy_synced` asked, was refused AT COMMIT TIME -- which no test can
	 * predict, see apply_tear_state() -- and landed on the vblank instead.
	 * `tear_dropped` did not land at all.
	 *
	 * Counted rather than logged per frame: one session on DP-1 produced 24943
	 * commit refusals in forty minutes, an 8MB log, and no number anywhere that
	 * said what fraction of frames that was.
	 */
	uint64_t tear_torn;
	uint64_t tear_test_refused;
	uint64_t tear_busy_synced;
	uint64_t tear_dropped;
	/* How long to leave a busy CRTC alone, and how many frames that spared.
	 * See the backoff in apply_tear_state(): the refusals arrive in bursts a
	 * fraction of a frame long, and asking again inside one is free of any
	 * chance of succeeding. */
	uint64_t tear_busy_until_ns;
	uint64_t tear_backoff;
	/*
	 * CADENCE, FROM PRESENTATION RATHER THAN FROM GPU TIMING.
	 *
	 * The vblank-sequence delta between one presented frame and the next: 1
	 * means the very next vblank, 2 means one went by. A GPU frame comfortably
	 * inside budget can still land here as 2 because it arrived after the flip
	 * deadline, and that distinction is invisible to any duration-based
	 * measure. On a damage-driven desktop a large delta is usually just an idle
	 * gap, so these count occurrences and are not a quality score on their own.
	 */
	uint64_t present_cadence_1x, present_cadence_2x, present_cadence_3x;
	/* Periods that did not resemble the mode at all -- a jumped sequence, a
	 * mode change, or a backend that does not count vblanks. Counted rather
	 * than averaged in, because a silently wrong period poisons everything
	 * built on it. */
	uint64_t present_interval_rejected;
	/*
	 * DOES THIS BACKEND COUNT VBLANKS AT ALL?
	 *
	 * wlr_output_event_present.seq is documented "zero if unavailable", and
	 * the headless backend never fills it. Every derived period and every
	 * cadence figure here comes from a SEQUENCE delta, so on such a backend
	 * they legitimately stay zero while frames are being presented perfectly
	 * well -- 2305 and 874 presents across a 144Hz/60Hz pair, with observed
	 * period and cadence both reading 0.
	 *
	 * Stated explicitly because the alternative is a reader concluding from
	 * `observed_interval_us: 0` that nothing presented. Oracles that need a
	 * real period or a real cadence belong on hardware; ones that need only
	 * wall-clock timing run headless.
	 */
	bool present_seq_available;
	/* AZ_BREAK_PRESENT_IDLE_WAKE only (falsifier I5). Null in every normal
	 * build, which is the point: the guarantee is that no such object
	 * exists. */
	struct wl_event_source *break_idle_wake_timer;

	/*
	 * ── M-8. ARM-TO-PHOTONS AND COMMIT-TO-PHOTONS ─────────────────────────
	 *
	 * ADR-605 needs one number to make the VRR predictor honest: `t_pipe`,
	 * the time from the frame event through render, commit and scanout to
	 * light. Under adaptive sync there is no vblank lattice to project onto --
	 * presentation follows the commit -- so what the compositor must predict
	 * is its own latency, and until this is measured the ADR's placeholder is
	 * a documented bias.
	 *
	 * Correlated by commit_seq, which wlr_output_event_present carries for
	 * exactly this purpose. Only the most recent armed frame is tracked: the
	 * pipeline is one commit deep in the ordinary case, and a present whose
	 * commit_seq does not match is counted rather than guessed at, so a
	 * deeper pipeline shows up as unmatched samples instead of as wrong
	 * latencies.
	 */
	uint64_t m8_arm_ns;      /* frame event entered rendermon */
	uint64_t m8_commit_ns;   /* wlr_output_commit_state returned */
	uint32_t m8_commit_seq;  /* what to match ev->commit_seq against */
	bool m8_armed;
	uint64_t m8_samples, m8_unmatched;
	uint64_t m8_arm_sum_ns, m8_arm_min_ns, m8_arm_max_ns;
	uint64_t m8_commit_sum_ns, m8_commit_min_ns, m8_commit_max_ns;
	/*
	 * COMMIT-TO-PHOTONS AS A DISTRIBUTION, NOT THREE NUMBERS.
	 *
	 * `t_pipe` (ADR-605) is the latency when the pipeline is NOT waiting for
	 * the display to become ready -- the queueing case is already modelled by
	 * the predictor's `last_present + P_min` floor, so seeding from the mean
	 * would count the wait twice and put every target a frame late. The mean
	 * is therefore the wrong estimator and the minimum is a single sample of
	 * an extreme, which is no better.
	 *
	 * So: a histogram, and read a low percentile off it. 100us buckets to
	 * 12.8ms, which brackets both a 60Hz and a 144Hz period, plus one overflow
	 * bucket so a long tail is visible as a count rather than distorting the
	 * scale.
	 */
#define AZ_M8_BUCKETS 129 /* 128 x 100us, then overflow */
#define AZ_M8_BUCKET_NS 100000ull
	uint32_t m8_hist[AZ_M8_BUCKETS];
	/* The kernel's own guess at when the next refresh may occur, from
	 * ev->refresh. Zero when unavailable. Under VRR this is the closest thing
	 * to a period the hardware will tell us, so it is worth reading rather
	 * than deriving. */
	uint64_t present_hw_refresh_ns;
	/*
	 * ── WHICH CLOCK ev->when IS IN, PROVEN RATHER THAN BELIEVED ───────────
	 *
	 * wlroots' DRM backend reports the page-flip timestamp, and the natural
	 * assumption is CLOCK_MONOTONIC. It is only an assumption: the kernel
	 * reports a realtime-based stamp where the driver cannot supply a
	 * monotonic one, and a predictor that subtracts a realtime stamp from a
	 * monotonic one is not slightly wrong, it is wrong by the machine's uptime.
	 *
	 * So the first presented frame on each output records the distance from
	 * ev->when to BOTH clocks read in the handler, and `present_clock` states
	 * which one it landed on. Anything that does arithmetic across this
	 * boundary must consult it instead of assuming.
	 */
	int64_t present_skew_mono_ns;
	int64_t present_skew_real_ns;
	enum {
		PRESENT_CLOCK_UNKNOWN = 0,
		PRESENT_CLOCK_MONOTONIC,
		PRESENT_CLOCK_REALTIME,
		PRESENT_CLOCK_NEITHER,
	} present_clock;
	struct wl_listener destroy;
	struct wl_listener request_state;
	struct wl_listener destroy_lock_surface;
	struct wlr_session_lock_surface_v1 *lock_surface;
	struct wl_event_source *skip_frame_timeout;
	/* render-late scheduling (config.render_late): defer the render from the
	 * frame event toward the next vblank to cut input latency. Adaptive: defer
	 * render_late_frac of the interval, back off on a detected vblank miss
	 * (frame events spaced ~2 intervals apart), reclaim slowly when on time.
	 * render_dur_ms is a decaying max of recent render+commit cost (a floor on
	 * how early we must start). */
	struct wl_event_source *render_timer;
	double render_dur_ms;
	double render_late_frac;    /* fraction of the interval to defer (adaptive) */
	uint64_t render_late_last_ns; /* timestamp of the previous frame event */
	bool render_late_deferred;  /* did we defer the previous frame? */
	bool render_late_pending;   /* a deferred render is armed (ignore new frames) */
	int32_t render_late_good;   /* consecutive on-time deferred frames */
	struct wlr_box m;		  /* monitor area, layout-relative */
	struct wlr_box w;		  /* window area, layout-relative */
	uint32_t cascade_idx;	  /* float layout: next cascade placement slot */
	struct wl_list layers[4]; /* LayerSurface::link */
	uint32_t seltags;
	uint32_t tagset[2];
	bool skiping_frame;
	uint32_t resizing_count_pending;
	uint32_t resizing_count_current;

	struct wl_list dwl_ipc_outputs;
	int32_t gappih; /* horizontal gap between windows */
	int32_t gappiv; /* vertical gap between windows */
	int32_t gappoh; /* horizontal outer gaps */
	int32_t gappov; /* vertical outer gaps */
	Pertag *pertag;
	uint32_t ovbk_current_tagset;
	uint32_t ovbk_prev_tagset;
	Client *sel, *prevsel;
	int32_t isoverview;
	int32_t is_jump_mode;
	/* macOS-Mission-Control-style overview chrome: a dim backdrop, and per
	 * tag-cell a translucent background panel + a tag-name label. The
	 * overview groups windows by tag, each tag drawn in its own cell with
	 * its layout preserved (scaled down). */
	struct wlr_scene_rect *ov_dim;
	struct wlr_scene_rect *ov_cell_bg[OV_TAG_CELLS];
	struct wlr_scene_buffer *ov_cell_wp[OV_TAG_CELLS]; /* per-tile wallpaper */
	struct wlr_scene_shadow *ov_cell_shadow[OV_TAG_CELLS];
	struct asteroidz_jump_label_node *ov_cell_label[OV_TAG_CELLS];
	float ov_strip_scroll;                  /* Mission Control: strip h-scroll */
	struct wlr_scene_blur *ov_strip_blur;     /* xray blur behind the top strip */
	struct wlr_scene_rect *ov_strip_bg;       /* translucent tint over the blur */
	struct wlr_scene_shadow *ov_strip_shadow; /* shadow under the top strip */
	struct wlr_scene_buffer
		*ov_snap[OV_STRIP_WINS]; /* active tag's window snapshots (strip tile) */
	/* strip tile hit-test rects + hover-preview state (Mission Control) */
	float ov_tile_x[OV_TAG_CELLS];
	uint32_t ov_tile_tag[OV_TAG_CELLS];
	int32_t ov_tile_count;
	float ov_tile_y, ov_tile_w, ov_tile_h;
	uint32_t ov_preview_tag; /* tag shown in the main area (0 = the current tag) */
	struct wlr_scene_buffer *ov_main_wp;   /* wallpaper backdrop of the big area */
	struct wlr_scene_shadow *ov_main_shadow; /* drop shadow under the OV desktop */
	struct wlr_scene_rect *ov_vignette[4]; /* edge-darkening vignette gradients */
	/* open/close zoom-fade of the overview chrome (driven from rendermon) */
	bool ov_anim_running;      /* a fade is currently in progress */
	bool ov_anim_open;         /* true = fading in, false = fading out */
	uint32_t ov_anim_start_ms; /* start timestamp of the current fade */
	float ov_anim_t;           /* current chrome visibility (0..1) */
	struct wlr_scene_rect *ov_main_border;   /* subtle 1px edge on the OV desktop */
	struct wlr_scene_rect *ov_hover_hl;    /* highlight around the hovered window */
	float ov_main_scroll;                  /* big-area horizontal scroll (scroller) */
	uint32_t ov_scroll_tag;                /* tag the scroll offset currently applies to */
	uint32_t ov_main_tag;                  /* tag currently laid out in the big area */
	float ov_main_x, ov_main_y, ov_main_w, ov_main_h; /* big-area rect (cached) */
	float ov_vp_x, ov_vp_y, ov_vp_w, ov_vp_h; /* mirrored-viewport rect (border crop) */
	struct wlr_scene_buffer *ov_main_crop[16]; /* cropped live buffers for edge windows */
	struct wlr_scene_rect *ov_main_bord[16];   /* their borders, clipped to viewport */
	struct wlr_scene_rect *ov_void[4];         /* dark frame masking window overhang */
	float ov_avail_y;                          /* content-region top (below strip) */
	bool ov_main_more_l, ov_main_more_r;   /* more windows off left/right in big area */
	struct wlr_scene_rect *ov_main_chevron_l, *ov_main_chevron_r; /* scroll hints */
	int32_t is_in_hotarea;
	int32_t asleep;
	bool vrr_global_enable; // monitorrule vrr:1 = VRR always on
	bool is_vrr_opening;	// VRR currently enabled on the output
	/* DSC/link retrain: cycle through an alternate mode and back to fully
	 * reinit the sink's DSC decoder (same effect as a VT switch) */
	struct wl_event_source *retrain_timer;
	struct wlr_output_mode *retrain_restore_mode;
	int retrain_phase;
	uint32_t visible_clients;
	uint32_t visible_tiling_clients;
	uint32_t visible_scroll_tiling_clients;
	uint32_t visible_fake_tiling_clients;
	/* hdr is the EFFECTIVE, currently-committed colour state. hdr_configured
	 * is the base intent from monitors.kdl. They differ whenever an override
	 * is active -- a force_hdr client pushing an SDR output into HDR, or the
	 * capture fallback pulling an HDR output down to SDR. Everything that
	 * renders or reports reads hdr; only hdr_resolve() may write it. */
	int32_t hdr;
	/*
	 * TRI-STATE, and the third state is the whole point:
	 *
	 *   -1  no explicit per-output choice has been made
	 *    0  explicitly OFF   (a rule said `hdr 0`, or a dispatch turned it off)
	 *    1  explicitly ON
	 *
	 * It used to be 0/1 with no way to tell "the operator turned this output's
	 * HDR off" from "the operator never mentioned it", so hdr_resolve() could
	 * only treat 0 as absence -- and `hdr-mode on` therefore outranked every
	 * per-output request. set_output_hdr and toggle_hdr wrote a baseline that
	 * was overridden inside the same call, IPC answered success, and the
	 * dispatch had never once worked on a desktop configured that way.
	 */
	int32_t hdr_configured;
	/* Sticky: mon_state_apply_color() refused HDR because the output or
	 * renderer can't do BT.2020+PQ. Without this, hdr_resolve() would keep
	 * re-asserting HDR every frame and we'd retrain in a loop. Cleared when
	 * the output is reconfigured (hotplug can change what's supported). */
	bool hdr_capability_failed;
	int32_t bitdepth;
	float hdr_max_luminance, hdr_min_luminance, hdr_max_fall;
	/*
	 * M5/C3. The derived colour state for this output: which path it takes,
	 * the encode transfer function, the primaries+saturation matrix, the scene
	 * reference and the tone map's ceiling.
	 *
	 * DERIVED, NEVER SET DIRECTLY. mon_state_apply_color() is the only writer,
	 * and it recomputes the whole struct from the state it is about to commit
	 * -- so there is no partial update and no field that can go stale on its
	 * own. Nothing renders from it yet (C6/C7 wire that), which is deliberate:
	 * having it observable BEFORE anything reads it is what lets the model be
	 * checked against real outputs while a wrong answer still costs nothing.
	 */
	struct az_output_color_state color_state;
	struct wlr_color_transform *icc_transform;
	char icc_path[256];
	/*
	 * ── M6B/G2. THE SAME PROFILE, REDUCED TO WHAT AVK CAN APPLY ───────────
	 *
	 * `icc_transform` is wlroots' object and belongs to the SceneFX path; this
	 * is the matrix-shaper reduction the AVK encode pass applies itself. Both
	 * are loaded from the same file and exactly one of them may be in force --
	 * az_output_color_transform() is the interlock, and applying both would be
	 * the profile twice.
	 *
	 * `icc_serial` is bumped on every load and every clear, and it is what the
	 * renderer's LUT upload is keyed on. A counter rather than the path,
	 * because re-loading the same path after the file changed on disk is a new
	 * curve with an identical name -- the same identity-versus-content
	 * distinction avk_image.content_seq exists for.
	 */
	struct az_icc_shaper icc_shaper;
	bool icc_shaper_ok;
	enum az_icc_reject icc_reject;
	/*
	 * ── M6C. THE SAME PROFILE AGAIN, FOR THE ONES THAT DO NOT REDUCE ──────
	 *
	 * Built ONLY when `icc_shaper_ok` is false, and built by evaluating
	 * `icc_transform` itself on a 65-cube -- so AVK and SceneFX sample the same
	 * numbers from the same lcms2 transform rather than two readings of one
	 * file. That is why this is derived from the wlroots object and the shaper
	 * is derived from the bytes: the shaper is a REDUCTION and has to be
	 * checkable against lcms2 independently, and this is a SAMPLING and has to
	 * be identical to it.
	 *
	 * Heap, because it is 1.6MB and there are as many of these as there are
	 * monitors. Freed on every load and on destroy; NULL is the ordinary state.
	 */
	struct az_icc_clut *icc_clut;
	uint64_t icc_serial;
	struct wlr_scene_optimized_blur *blur;
	float cursor_zoom;		 /* output magnification, 1.0 = off */
	double zoom_cx, zoom_cy; /* zoom view center, output-local coords */
	bool zoom_cursor_locked; /* software cursors forced while zoomed */
	char last_open_surface[256];
	struct wlr_ext_workspace_group_handle_v1 *ext_group;
	bool iscleanuping;
	int8_t carousel_anim_dir;
	char *active_special;	   /* interned name of the named special workspace
								* currently shown on top of this monitor, NULL
								* if none */
	bool special_transitioning; /* true while an arrange() call is animating a
								 * special-workspace open/close/switch, used to
								 * pick the slide animation in animation/tag.h */

	/* an HDR-affecting commit issued out-of-band (outside the normal frame
	 * cycle) can race an in-flight page-flip and get rejected by the DRM
	 * backend. Instead, set this and let rendermon() fold the color-state
	 * change into the next regular frame's own commit. */
	bool hdr_pending_change;

	/*
	 * THE CONTENT METADATA CURRENTLY FOLDED INTO THIS CONNECTOR.
	 *
	 * mon_state_apply_color() overrides the panel's own HDR10 static metadata
	 * with the sole fullscreen client's declared values, and nothing used to
	 * notice when those values CHANGED without the fullscreen state changing
	 * with them -- mpv advancing from a 1000-nit title to a 4000-nit one left
	 * the connector describing 1000 for as long as it stayed fullscreen.
	 *
	 * An identity rather than a boolean, so "did this change" is answered by
	 * comparing the numbers that actually reach the connector rather than by a
	 * flag somebody has to remember to clear. The comparison is what keeps this
	 * from being a modeset storm: see mon_content_metadata_changed().
	 */
	uint64_t content_metadata_identity;

	/* AVK's per-output state: its own swapchain and the renderer built for
	 * this output's colour format. NULL under AVK, which is always, and
	 * still NULL on an output AVK has decided it cannot render correctly --
	 * see az_avk_output_supported() in src/render/az_avk.h. */
	struct az_avk_output *avk;
};

typedef struct {
	struct wlr_pointer_constraint_v1 *constraint;
	struct wl_listener destroy;
} PointerConstraint;

typedef struct {
	struct wlr_scene_tree *scene;

	struct wlr_session_lock_v1 *lock;
	struct wl_listener new_surface;
	struct wl_listener unlock;
	struct wl_listener destroy;
} SessionLock;

typedef struct DwindleNode DwindleNode;
struct DwindleNode {
	bool is_split;
	bool split_h;
	bool split_locked;
	bool custom_leaf_split_h;
	float ratio;
	float drag_init_ratio;
	int32_t container_x;
	int32_t container_y;
	int32_t container_w;
	int32_t container_h;
	DwindleNode *parent;
	DwindleNode *first;
	DwindleNode *second;
	Client *client;
};

struct ScrollerStackNode {
	Client *client;
	float scroller_proportion;
	float stack_proportion;
	float scroller_proportion_single;

	struct ScrollerStackNode *next_in_stack;
	struct ScrollerStackNode *prev_in_stack;
	struct ScrollerStackNode *all_next;
};

struct TagScrollerState {
	struct ScrollerStackNode *all_first; /* singly-linked list head of all nodes */
	int count;
};

typedef struct {
	uint32_t type; // must be first in struct
	int32_t orig_width;
	int32_t orig_height;
	bool is_subsurface;
	struct wl_listener destroy;
} SnapshotMetadata;

/* function declarations */
static void applybounds(
	Client *c,
	struct wlr_box *bbox); // apply bounds rules, giving some windows a more suitable size
static void applyrules(Client *c); // apply window rules, applies the window rules defined in config.h
static void arrange(Monitor *m, bool want_animation,
					bool from_view); // layout function, moves/resizes windows according to the tiling rules
static void arrangelayer(Monitor *m, struct wl_list *list,
						 struct wlr_box *usable_area, int32_t exclusive);
static void arrangelayers(Monitor *m);
static void handle_print_status(struct wl_listener *listener, void *data);
static void axisnotify(struct wl_listener *listener,
					   void *data); // scroll wheel event handling
static void buttonpress(struct wl_listener *listener,
						void *data); // mouse button event handling
static bool handle_buttonpress(struct wlr_pointer_button_event *event);
static int32_t ongesture(struct wlr_pointer_swipe_end_event *event);
static void swipe_begin(struct wl_listener *listener, void *data);
static void swipe_update(struct wl_listener *listener, void *data);
static void swipe_end(struct wl_listener *listener, void *data);
static void pinch_begin(struct wl_listener *listener, void *data);
static void pinch_update(struct wl_listener *listener, void *data);
static void pinch_end(struct wl_listener *listener, void *data);
static void hold_begin(struct wl_listener *listener, void *data);
static void hold_end(struct wl_listener *listener, void *data);
static void checkidleinhibitor(struct wlr_surface *exclude);
static void cleanup(void);										  // exit cleanup
static void cleanupmon(struct wl_listener *listener, void *data); // exit cleanup
static void pacepresent(struct wl_listener *listener, void *data); // AZ_PACE=1
static void closemon(Monitor *m);
static void cleanuplisteners(void);
static void toggle_hotarea(int32_t x_root, int32_t y_root); // trigger hot corner
static void maplayersurfacenotify(struct wl_listener *listener, void *data);
static void commitlayersurfacenotify(struct wl_listener *listener, void *data);
static void commitnotify(struct wl_listener *listener, void *data);
static void createdecoration(struct wl_listener *listener, void *data);
static void createidleinhibitor(struct wl_listener *listener, void *data);
static void createkeyboard(struct wlr_keyboard *keyboard);
static void requestmonstate(struct wl_listener *listener, void *data);
static void createlayersurface(struct wl_listener *listener, void *data);
static void createlocksurface(struct wl_listener *listener, void *data);
static void createmon(struct wl_listener *listener, void *data);
static void createnotify(struct wl_listener *listener, void *data);
static void createpointer(struct wlr_pointer *pointer);
static void configure_pointer(struct libinput_device *device);
static void destroyinputdevice(struct wl_listener *listener, void *data);
static void createswitch(struct wlr_switch *switch_device);
static void switch_toggle(struct wl_listener *listener, void *data);
static void createpointerconstraint(struct wl_listener *listener, void *data);
static void cursorconstrain(struct wlr_pointer_constraint_v1 *constraint);
static void commitpopup(struct wl_listener *listener, void *data);
static void createpopup(struct wl_listener *listener, void *data);
static void createdialog(struct wl_listener *listener, void *data);
static void cursorframe(struct wl_listener *listener, void *data);
static void cursorwarptohint(void);
static void destroydecoration(struct wl_listener *listener, void *data);
static void destroydragicon(struct wl_listener *listener, void *data);
static void destroyidleinhibitor(struct wl_listener *listener, void *data);
static void destroylayernodenotify(struct wl_listener *listener, void *data);
static void destroylock(SessionLock *lock, int32_t unlocked);
static void destroylocksurface(struct wl_listener *listener, void *data);
static void destroynotify(struct wl_listener *listener, void *data);
static void destroypointerconstraint(struct wl_listener *listener, void *data);
static void destroysessionlock(struct wl_listener *listener, void *data);
static void destroykeyboardgroup(struct wl_listener *listener, void *data);
static Monitor *dirtomon(enum wlr_direction dir);
static void setcursorshape(struct wl_listener *listener, void *data);

static void focusclient(Client *c, int32_t lift);

static void setborder_color(Client *c);
static Client *focustop(Monitor *m);
static void fullscreennotify(struct wl_listener *listener, void *data);
static void gpureset(struct wl_listener *listener, void *data);

static int32_t keyrepeat(void *data);

static void inputdevice(struct wl_listener *listener, void *data);
static int32_t keybinding(uint32_t state, bool locked, uint32_t mods,
						  xkb_keysym_t sym, uint32_t keycode);
static void keypress(struct wl_listener *listener, void *data);
static void keypressmod(struct wl_listener *listener, void *data);
static bool keypressglobal(struct wlr_surface *last_surface,
						   struct wlr_keyboard *keyboard,
						   struct wlr_keyboard_key_event *event, uint32_t mods,
						   xkb_keysym_t keysym, uint32_t keycode);
static void locksession(struct wl_listener *listener, void *data);
static void mapnotify(struct wl_listener *listener, void *data);
static void maximizenotify(struct wl_listener *listener, void *data);
static void minimizenotify(struct wl_listener *listener, void *data);
static void motionabsolute(struct wl_listener *listener, void *data);
static void motionnotify(uint32_t time, struct wlr_input_device *device,
						 double sx, double sy, double sx_unaccel,
						 double sy_unaccel);
static void motionrelative(struct wl_listener *listener, void *data);
static void cursor_zoom_set_factor(float factor);
static void cursor_zoom_update(void);
static void cursor_zoom_frame(Monitor *m);

static void reset_foreign_tolevel(Client *c, Monitor *oldmon, Monitor *newmon);
static void add_foreign_topleve(Client *c);
static void exchange_two_client(Client *c1, Client *c2);
static void outputmgrapply(struct wl_listener *listener, void *data);
static void outputmgrapplyortest(struct wlr_output_configuration_v1 *output_config,
								 int32_t test);
static void outputmgrtest(struct wl_listener *listener, void *data);
static void pointerfocus(Client *c, struct wlr_surface *surface, double sx,
						 double sy, uint32_t time);
static void printstatus(enum ipc_watch_type type);
/* Move the focus to another output and TELL anyone watching.
 *
 * `active` on a monitor is `m == selmon`, and a client that wants to act on the
 * focused screen -- the bar's wallpaper keybind, for one -- has no other way to
 * learn it. The dispatch path pushed already, because focusmon() ends in
 * focusclient() and that notifies; the pointer paths assign selmon directly and
 * said nothing, so moving the pointer onto an empty area of the other monitor
 * changed the focus without a single watcher hearing about it. The bar went on
 * believing the old output was focused until something else happened to push. */
static void set_selmon(Monitor *m);
/* ipc/ipc.h is included later still, and a reload has to push the new palette
 * to the bar, which runs out of process. */
void ipc_notify_bar_config(void);
void ipc_notify_config(const char *reason);
void ipc_notify_idle(void);
/* ipc/portals.h is later still, and three things that come before it need to
 * ask the Inhibit backend something: checkidleinhibitor() whether anything is
 * holding idling off, the exit prompt who asked not to be interrupted, and
 * the session lock that it changed. */
bool inhibit_portal_holds_idle(void);
uint32_t inhibit_portal_generation(void);
size_t inhibit_portal_count(void);
bool inhibit_portal_get(size_t index, const char **app_id, const char **reason,
						uint32_t *flags);
bool inhibit_portal_logout_summary(char *buf, size_t cap);
void inhibit_portal_screensaver_changed(void);
void inhibit_portal_set_session_state(uint32_t state);
static void powermgrsetmode(struct wl_listener *listener, void *data);
static void wake_monitor(Monitor *m);
static void wake_sleeping_monitors(void);
static void rendermon(struct wl_listener *listener, void *data);
static void presentmon(struct wl_listener *listener, void *data);
static void requestdecorationmode(struct wl_listener *listener, void *data);
static void requestdrmlease(struct wl_listener *listener, void *data);
static void requeststartdrag(struct wl_listener *listener, void *data);
static void resize(Client *c, struct wlr_box geo, int32_t interact);
static void run(char *startup_cmd);
static void set_activation_env(void);
static void setcursor(struct wl_listener *listener, void *data);
static void setfloating(Client *c, int32_t floating);
static void setfakefullscreen(Client *c, int32_t fakefullscreen);
static void setfullscreen(Client *c, int32_t fullscreen, bool rearrange);
static void setmaximizescreen(Client *c, int32_t maximizescreen,
							  bool rearrange);
static void reset_maximizescreen_size(Client *c);
static void setgaps(int32_t oh, int32_t ov, int32_t ih, int32_t iv);

static void setmon(Client *c, Monitor *m, uint32_t newtags, bool focus);
/* M6B/D6. Defined beside setmon, called from mapnotify and from the colour
 * derivation, both of which come earlier in this file. */
static void surface_send_preferred_description(struct wlr_surface *surface,
	Monitor *m);
static void mon_send_preferred_descriptions(Monitor *m);
/* The frog half of the same statement. Declared here because the config
 * reload -- parse_config.h, included well before the protocol frontends -- has
 * to re-announce through both of them when a monitor rule changes a mastering
 * value, and a resend through only one frontend is the drift az_preferred.h
 * exists to prevent. */
static void frog_send_preferred_metadata_all(Monitor *m);
/* Every output's surfaces, for a change that is not scoped to one display:
 * config.sdr_reference_luminance is global and feeds every SDR answer. */
static void mon_send_preferred_descriptions_all(void);
/* A surface changed the HDR10 static metadata it declares. Defined beside
 * mon_hdr_scanout_candidate, called from the xdg commit handler and from frog's
 * own setter -- both of which come earlier in this file, or in a header
 * included earlier. */
static void mon_content_metadata_changed(struct wlr_surface *surface);
/* M13B: the scanout evaluation needs these and is included before either is
 * defined, same as above. One definition of "what covers this output", and one
 * of "what colour did this surface declare". */
static Client *mon_hdr_scanout_candidate(Monitor *m);
static const struct wlr_image_description_v1_data *az_cm_surface_description(
	struct wlr_surface *surface);
/*
 * How many times a content-metadata change has armed a connector update.
 *
 * Here rather than beside the code that increments it because ipc.h is
 * included well before that point and `get cm-stats` reports it. It is the one
 * counter that BOUNDS THE COST of the arming path: the flag it sets is folded
 * in with allow_reconfiguration, which in this wlroots means a blocking full
 * modeset, and a live session once logged 58 spurious ones with libinput
 * complaining of 42-51ms of lag inside the densest burst. A number that can be
 * read from outside is the difference between "arming is gated" as a claim and
 * as a measurement.
 */
static uint64_t az_content_metadata_arms = 0;
static void setpsel(struct wl_listener *listener, void *data);
static void setsel(struct wl_listener *listener, void *data);
static void setup(void);
static void startdrag(struct wl_listener *listener, void *data);

static void unlocksession(struct wl_listener *listener, void *data);
static void unmaplayersurfacenotify(struct wl_listener *listener, void *data);
static void unmapnotify(struct wl_listener *listener, void *data);
static void updatemons(struct wl_listener *listener, void *data);
static void updatetitle(struct wl_listener *listener, void *data);
static void urgent(struct wl_listener *listener, void *data);
static void view(const Arg *arg, bool want_animation);

static void handlesig(int32_t signo);
static void
handle_keyboard_shortcuts_inhibit_new_inhibitor(struct wl_listener *listener,
												void *data);
static void virtualkeyboard(struct wl_listener *listener, void *data);
static void virtualpointer(struct wl_listener *listener, void *data);
static void warp_cursor(const Client *c);
static Monitor *xytomon(double x, double y);
static void xytonode(double x, double y, struct wlr_surface **psurface,
					 Client **pc, LayerSurface **pl, double *nx, double *ny);
static void clear_fullscreen_flag(Client *c);
static pid_t getparentprocess(pid_t p);
static int32_t isdescprocess(pid_t p, pid_t c);
static Client *termforwin(Client *w);
static void client_replace(Client *c, Client *w);

static void warp_cursor_to_selmon(Monitor *m);
uint32_t want_restore_fullscreen(Client *target_client);
static void overview_restore(Client *c, const Arg *arg);
static void overview_backup(Client *c);
static void set_minimized(Client *c);

static void show_scratchpad(Client *c);
static void show_hide_client(Client *c);
static void tag_client(const Arg *arg, Client *target_client);

static struct wlr_box setclient_coordinate_center(Client *c, Monitor *m,
												  struct wlr_box geom,
												  int32_t offsetx,
												  int32_t offsety);
static uint32_t get_tags_first_tag(uint32_t tags);

static struct wlr_output_mode *
get_nearest_output_mode(struct wlr_output *output, int32_t width,
						int32_t height, float refresh);

static void client_commit(Client *c);
static void layer_commit(LayerSurface *l);
static void apply_border(Client *c);
static void client_set_opacity(Client *c, double opacity);
static void client_set_prevent_scanout(Client *c, bool prevent);
static void init_baked_points(void);
static void scene_buffer_apply_opacity(struct wlr_scene_buffer *buffer,
									   int32_t sx, int32_t sy, void *data);
static void scene_buffer_apply_prevent_scanout(struct wlr_scene_buffer *buffer,
												int32_t sx, int32_t sy,
												void *data);

static Client *direction_select(const Arg *arg);
static void view_in_mon(const Arg *arg, bool want_animation, Monitor *m,
						bool changefocus);
static void ensure_monitor_blur_node(Monitor *m);

static void buffer_set_effect(Client *c, BufferData buffer_data);
static void snap_scene_buffer_apply_effect(struct wlr_scene_buffer *buffer,
										   int32_t sx, int32_t sy, void *data);
static void client_set_pending_state(Client *c);
static void layer_set_pending_state(LayerSurface *l);
static void set_rect_size(struct wlr_scene_rect *rect, int32_t width,
						  int32_t height);
static Client *center_tiled_select(Monitor *m);
static void handlecursoractivity(void);
static int32_t hidecursor(void *data);
static void check_scroller_edge_scroll(int32_t x_root, int32_t y_root);
static int32_t scroller_edge_scroll_timeout(void *data);
static bool check_hit_no_border(Client *c);
static bool client_wants_ssd(Client *c);
static void reset_keyboard_layout(void);
static void client_update_oldmonname_record(Client *c, Monitor *m);
static void pending_kill_client(Client *c);
static uint32_t get_tags_first_tag_num(uint32_t source_tags);
static void set_layer_open_animaiton(LayerSurface *l, struct wlr_box geo);
static void init_fadeout_layers(LayerSurface *l);
static void layer_actual_size(LayerSurface *l, int32_t *width, int32_t *height);
static void get_layer_target_geometry(LayerSurface *l,
									  struct wlr_box *target_box);
static void scene_buffer_apply_effect(struct wlr_scene_buffer *buffer,
									  int32_t sx, int32_t sy, void *data);
static double find_animation_curve_at(double t, int32_t type);
/* render/az_output.h owns the renderer selector and is included AFTER
 * animation/client.h, which asks the question -- the same reason
 * find_animation_curve_at is declared up here. */
/* animation/common.h is included AFTER animation/client.h, which uses these --
 * the same reason find_animation_curve_at is declared here. */
static struct dvec2 calculate_spring_curve_at_v(double t, double v0);
static double spring_curve_velocity_at(double t);
static double spring_curve_velocity_at_v(double t, double v0);

static void apply_opacity_to_rect_nodes(Client *c, struct wlr_scene_node *node,
										double animation_passed);
static enum corner_location set_client_corner_location(Client *c);
static double all_output_frame_duration_ms();
static struct wlr_scene_tree *
wlr_scene_tree_snapshot(struct wlr_scene_node *node,
						struct wlr_scene_tree *parent);
static bool is_scroller_layout(Monitor *m);
static bool is_monocle_layout(Monitor *m);
static bool is_float_layout(Monitor *m);
static void tag_display_name(Monitor *m, uint32_t tag, char *buf, size_t len);
void overview_hide_chrome(Monitor *m);
void overview_anim_start(Monitor *m, bool open);
bool overview_anim_frame(Monitor *m);
void overview_pointer_preview(Monitor *m, double px, double py);
void overview_hover_highlight(Monitor *m, Client *c);
uint32_t overview_tile_at(Monitor *m, double px, double py);
void overview_main_scroll(Monitor *m, double px, double py, int32_t dir);
static void create_output(struct wlr_backend *backend, void *data);
static void get_layout_abbr(char *abbr, const char *full_name);
static void apply_named_scratchpad(Client *target_client);
static Client *get_client_by_id_or_title(const char *arg_id,
										 const char *arg_title);
static bool switch_scratchpad_client_state(Client *c);
static char *intern_special_workspace_name(const char *name);
static void close_special_workspace(Monitor *m, bool want_animation);
static void open_special_workspace(Monitor *m, char *interned,
								   bool want_animation);
static bool check_trackpad_disabled(struct wlr_pointer *pointer);
static uint32_t get_tag_status(uint32_t tag, Monitor *m);
static void enable_adaptive_sync(Monitor *m, struct wlr_output_state *state);
static void disable_adaptive_sync(Monitor *m, struct wlr_output_state *state);
static Client *get_next_stack_client(Client *c, bool reverse);
static void set_float_malposition(Client *tc);
static void set_size_per(Monitor *m, Client *c);
static void resize_tile_client(Client *grabc, bool isdrag, int32_t offsetx,
							   int32_t offsety, uint32_t time);
static void refresh_monitors_workspaces_status(Monitor *m);
static void init_client_properties(Client *c);
static float *get_border_color(Client *c);
static void clear_fullscreen_and_maximized_state(Monitor *m);
static void request_fresh_all_monitors(void);
void request_fresh_for_box(const struct wlr_box *box, int32_t pad);
void anim_client_reach(Client *c, struct wlr_box *out);

/*
 * ── M4G: THE REACH OF THIS FRAME'S ANIMATIONS ─────────────────────────────
 *
 * Accumulated across render_monitor()'s client loop and consumed at the
 * bottom, where the next frame is scheduled. Reset per pass: each pass walks
 * the same clients and rebuilds it from scratch, so it can never carry a box
 * from a previous frame.
 *
 * `all` is the safe answer and the default for anything that cannot describe
 * its own extent -- fadeouts, layer animations, overview chrome, cursor zoom.
 * Getting this too big costs a wakeup, which is what M4G is removing; getting
 * it too small drops a frame the user can see.
 */
static struct wlr_box az_frame_reach;
static bool az_frame_reach_valid;
static bool az_frame_reach_all;

static void az_frame_reach_reset(void) {
	az_frame_reach = (struct wlr_box){0};
	az_frame_reach_valid = false;
	az_frame_reach_all = false;
}

static void az_frame_reach_add(const struct wlr_box *b) {
	if (b == NULL || b->width <= 0 || b->height <= 0) {
		az_frame_reach_all = true;
		return;
	}
	if (!az_frame_reach_valid) {
		az_frame_reach = *b;
		az_frame_reach_valid = true;
		return;
	}
	int32_t x1 = az_frame_reach.x < b->x ? az_frame_reach.x : b->x;
	int32_t y1 = az_frame_reach.y < b->y ? az_frame_reach.y : b->y;
	int32_t ax2 = az_frame_reach.x + az_frame_reach.width;
	int32_t ay2 = az_frame_reach.y + az_frame_reach.height;
	int32_t bx2 = b->x + b->width, by2 = b->y + b->height;
	int32_t x2 = ax2 > bx2 ? ax2 : bx2;
	int32_t y2 = ay2 > by2 ? ay2 : by2;
	az_frame_reach = (struct wlr_box){ .x = x1, .y = y1,
		.width = x2 - x1, .height = y2 - y1 };
}

/*
 * The padding applied to that box before it is tested against an output.
 *
 * It has to cover everything a window's pixels can reach beyond its own
 * geometry: the drop shadow and its spread, and the blur kernel's support at
 * the output's scale. Rather than recompute each of those here -- and get one
 * of them wrong on the day a radius or a scale changes -- this is a single
 * constant chosen to exceed all of them at every configurable setting: the
 * schema caps shadow size and blur radius well inside it. A too-large pad
 * wakes a neighbouring output occasionally; a too-small one leaves a blur
 * fringe stale along a seam, which is the failure M4F.2C existed to fix.
 */
#define AZ_FRAME_REACH_PAD 256

/*
 * M4I. One line per frame of a tag transition; see the trace in render_monitor.
 *
 * Runtime-settable as well as env-settable. AZ_TAGTRACE is fine for a headless
 * fixture, but `restart` re-execs with the same environ, so on the live session
 * an env-only switch can never be turned on --- and the live session is the
 * only place the interesting distribution exists.
 */
static int az_tagtrace_runtime = -1;
static inline bool az_tagtrace_on(void) {
	if (az_tagtrace_runtime >= 0) {
		return az_tagtrace_runtime != 0;
	}
	static int cached = -1;
	if (cached < 0) {
		const char *env = getenv("AZ_TAGTRACE");
		cached = (env != NULL && env[0] == '1' && env[1] == '\0');
	}
	return cached != 0;
}
static Client *find_client_by_direction(Client *tc, const Arg *arg,
										bool findfloating);
static void exit_scroller_stack(Client *c);
static Client *scroll_get_stack_head_client(Client *c);
static bool client_only_in_one_tag(Client *c);
static Client *get_focused_stack_client(Client *sc,
										Client *custom_focus_client);
static bool client_is_in_same_stack(Client *sc, Client *tc, Client *fc);
static void monitor_stop_skip_frame_timer(Monitor *m);
static int monitor_skip_frame_timeout_callback(void *data);
static void render_monitor(Monitor *m);
static int render_timer_cb(void *data);
static Monitor *get_monitor_nearest_to(int32_t lx, int32_t ly);
static bool match_monitor_spec(char *spec, Monitor *m);
static void last_cursor_surface_destroy(struct wl_listener *listener,
										void *data);
static int32_t keep_idle_inhibit(void *data);
static void schedule_float_focus_raise(Client *c);
static void check_keep_idle_inhibit(Client *c);
static void check_vrr_enable(Client *c);
static int monitor_retrain_step(void *data);
void monitor_start_retrain(Monitor *m, uint32_t delay_ms);
static void hdr_resolve(Monitor *m);
static void hdr_resolve_all(void);
static void handle_image_copy_capture_new_session(struct wl_listener *listener,
												  void *data);
static void refresh_shielded_surfaces(void);
void layer_draw_shield(LayerSurface *l);
void client_draw_shield(Client *c, struct wlr_box clip_box);
static int active_capture_count = 0;
static struct wl_listener ext_image_copy_capture_new_session = {
	.notify = handle_image_copy_capture_new_session};
static void pre_caculate_before_arrange(Monitor *m, bool want_animation,
										bool from_view, bool only_caculate);
static void client_pending_fullscreen_state(Client *c, int32_t isfullscreen);
static void client_pending_maximized_state(Client *c, int32_t ismaximized);
static void client_pending_minimized_state(Client *c, int32_t isminimized);
static void scroller_insert_stack(Client *c, Client *target_client,
								  bool insert_before);
static void dwindle_move_client(DwindleNode **root, Client *c, Client *target,
								float ratio, int32_t dir);
static void dwindle_resize_client_step(Monitor *m, Client *c, int32_t dx,
									   int32_t dy);
static void dwindle_resize_client(Monitor *m, Client *c);

static struct TagScrollerState *ensure_scroller_state(Monitor *m, uint32_t tag);
static struct ScrollerStackNode *find_scroller_node(struct TagScrollerState *st,
													Client *c);
static void sync_scroller_state_to_clients(Monitor *m, uint32_t tag);
static void scroller_node_remove(struct TagScrollerState *st,
								 struct ScrollerStackNode *target);
static struct ScrollerStackNode *
scroller_node_create(struct TagScrollerState *st, Client *c);
static void update_scroller_state(Monitor *m);
Client *scroll_get_stack_tail_client(Client *c);
static DwindleNode *dwindle_find_leaf(DwindleNode *node, Client *c);
static void overview_backup_surface(Client *c);

static void create_jump_hints(Monitor *m);
static void finish_jump_mode(Monitor *m);
static void begin_jump_mode(Monitor *m);
static void client_apply_decoration_config(Client *c);
void client_change_mon(Client *c, Monitor *m, uint32_t newtags);

#include "data/static_keymap.h"
#include "dispatch/bind_declare.h"
#include "layout/layout.h"

/* variables */
static const char broken[] = "broken";
static pid_t child_pid = -1;
static char **restart_argv;
static int32_t locked;
static uint32_t locked_mods = 0;
static void *exclusive_focus;
static struct wl_display *dpy;
static struct wl_event_loop *event_loop;
static struct wlr_backend *backend;
static struct wlr_backend *headless_backend;
static struct wlr_scene *scene;
static struct wlr_scene_tree *layers[NUM_LAYERS];

/* Show or hide a client -- its shadow included.
 *
 * While a window is TILED its shadow tree is a sibling of c->scene on
 * LyrTileShadow, not a child of it, so enabling and disabling no longer reach
 * the shadow through the parent. Every place that hides a window has to hide
 * the shadow with it, or switching tags leaves the shadows of the tag you left
 * painted on the wallpaper -- and there are seventeen such places, in tag
 * switching, monocle, the overview and the animation ticks.
 *
 * One function rather than seventeen edits that have to keep agreeing: the
 * pairing is the invariant, and the next place that hides a window should not
 * have to know why.
 *
 * A FLOATING window's shadow is inside c->scene and inherits as it always did,
 * which is what the parent check is for. And a window whose tree does not
 * exist yet -- this runs once during map, before the shadow is created -- is
 * simply not a case: the creation code takes its initial state from c->scene. */
static inline void client_set_scene_enabled(Client *c, bool enabled) {
	wlr_scene_node_set_enabled(&c->scene->node, enabled);
	if (c->shadow_tree && c->shadow_tree->node.parent != c->scene)
		wlr_scene_node_set_enabled(&c->shadow_tree->node, enabled);
}
static struct wlr_renderer *drw;

/*
 * The compatibility renderer must be the Vulkan one, and nothing else.
 *
 * wlroots needs a wlr_renderer for shm formats, the allocator, wl_drm and
 * screencopy -- none of which is composition, all of which take one as a
 * parameter with no variant that does not. So the object exists. What it must
 * never be is a GL renderer: a GLES2 context in this process means GL buffers,
 * GL textures and a second way for a frame to reach the screen, and the only
 * thing that ever used it -- the DRM cursor plane's 64x64 buffer -- was
 * indistinguishable from outside from AVK having quietly fallen back.
 *
 * Checked rather than assumed. The session pins WLR_RENDERER=vulkan, but a
 * session file is a string in a desktop entry: it can be edited, inherited
 * from a stale copy in /usr, or overridden in the environment, and the result
 * would be a working desktop that is wrong in precisely the way this whole
 * change exists to prevent. Pixman is refused for the same reason -- it is a
 * software renderer, and a silent fall to it is a slideshow nobody diagnosed.
 */
/*
 * Create it, rather than ask the environment for it.
 *
 * wlr_renderer_autocreate() picks from WLR_RENDERER, and its default order
 * starts at GL. That made the session file's `env WLR_RENDERER=vulkan` load
 * bearing: a string in a desktop entry, which can be edited, inherited from a
 * stale copy under /usr, or overridden per-launch, deciding whether a GL
 * context exists in this process. There is no switch now -- it is Vulkan or
 * the compositor does not start.
 */
static struct wlr_renderer *az_create_renderer(struct wlr_backend *b) {
	/*
	 * wlr_vk_renderer_create_with_drm_fd() is the direct call and it was tried
	 * first, but it needs a DRM fd and the HEADLESS backend has none -- which
	 * is every fixture in contrib/. So the choice is forced into the variable
	 * wlr_renderer_autocreate() reads, and the result is checked.
	 *
	 * Note what this is not: it is not a switch. Nothing outside this function
	 * can express a preference -- the session entries carry no WLR_RENDERER,
	 * and an inherited value is overwritten here rather than honoured, so a
	 * stale environment cannot put a GL context in this process. The guard
	 * below turns "somehow not Vulkan" into an abort rather than a fallback.
	 */
	setenv("WLR_RENDERER", "vulkan", 1);
	return wlr_renderer_autocreate(b);
}

static void az_require_vulkan_renderer(struct wlr_renderer *r) {
	if (r == NULL || wlr_renderer_is_vk(r)) {
		return;
	}
	wlr_log(WLR_ERROR,
		"the wlroots compatibility renderer is %s, not Vulkan. This "
		"compositor composites with AVK and will not run beside a %s "
		"renderer. Set WLR_RENDERER=vulkan.",
		wlr_renderer_is_gles2(r) ? "GLES2"
			: (wlr_renderer_is_pixman(r) ? "pixman" : "an unknown backend"),
		wlr_renderer_is_gles2(r) ? "GL" : "software");
	abort();
}
static struct wlr_allocator *alloc;
static struct wlr_compositor *compositor;

static struct wlr_xdg_shell *xdg_shell;
static struct wlr_xdg_activation_v1 *activation;
static struct wlr_xdg_decoration_manager_v1 *xdg_decoration_mgr;
static struct wl_list clients; /* tiling order */
static struct wl_list fstack;  /* focus order */
static struct wl_list fadeout_clients;
static struct wl_list fadeout_layers;
static struct wlr_idle_notifier_v1 *idle_notifier;
static struct wlr_idle_inhibit_manager_v1 *idle_inhibit_mgr;
/* Manual "keep the screen awake" override, independent of the client-driven
 * idle_inhibit_v1 protocol. waybar's idle_inhibitor module achieves this by
 * creating a protocol inhibitor of its own; a native bar module has no
 * surface to attach one to, so the compositor holds the flag instead. */
static bool idle_inhibit_manual = false;
/* What was last handed to the idle notifier -- the manual flag OR a client's
 * protocol inhibitor. Kept so `watch idle` can push on change rather than on
 * every call: checkidleinhibitor() runs on every arrange and every map. */
static bool idle_inhibited = false;
static struct wlr_layer_shell_v1 *layer_shell;
static struct wlr_output_manager_v1 *output_mgr;
static struct wlr_virtual_keyboard_manager_v1 *virtual_keyboard_mgr;
static struct wlr_keyboard_shortcuts_inhibit_manager_v1
	*keyboard_shortcuts_inhibit;
static struct wlr_virtual_pointer_manager_v1 *virtual_pointer_mgr;
static struct wlr_output_power_manager_v1 *power_mgr;
static struct wlr_pointer_gestures_v1 *pointer_gestures;
static struct wlr_drm_lease_v1_manager *drm_lease_manager;
struct asteroidz_print_status_manager *print_status_manager;

static struct wlr_cursor *cursor;
static struct wlr_xcursor_manager *cursor_mgr;
static struct wlr_session *session;
static float cursor_zoom_factor = 1.0f; /* runtime state, not config */

static struct wlr_scene_rect *root_bg;
static struct wlr_session_lock_manager_v1 *session_lock_mgr;
static struct wlr_scene_rect *locked_bg;
static struct wlr_session_lock_v1 *cur_lock;
static const int32_t layermap[] = {LyrBg, LyrBottom, LyrTop, LyrOverlay};
static struct wlr_scene_tree *drag_icon;
static struct wlr_cursor_shape_manager_v1 *cursor_shape_mgr;
static struct wlr_pointer_constraints_v1 *pointer_constraints;
static struct wlr_relative_pointer_manager_v1 *relative_pointer_mgr;
static struct wlr_pointer_constraint_v1 *active_constraint;

static struct wlr_seat *seat;
static KeyboardGroup *kb_group;
static struct wl_list inputdevices;
static struct wl_list keyboard_shortcut_inhibitors;
static uint32_t cursor_mode;
static Client *grabc, *dropc;
static int32_t rzcorner;
static int32_t grabcx, grabcy;						   /* client-relative */
static int32_t drag_begin_cursorx, drag_begin_cursory; /* client-relative */
static bool start_drag_window = false;
static int32_t last_apply_drap_time = 0;

static struct wlr_output_layout *output_layout;
static struct wlr_box sgeom;
static struct wl_list mons;
static Monitor *selmon;

/* asteroidz easter egg: random UFO-vs-Asteroids-ship fly-by over the bar */
static struct ufo_egg *ufo_egg;
static bool ufo_bar_geometry(void *ud, int32_t *x, int32_t *y, int32_t *w,
							 int32_t *h) {
	(void)ud;
	if (!selmon)
		return false;
	/* never fly over fullscreen content: the overlay would break the
	 * surface's direct scanout for the whole animation, and for HDR (PQ
	 * passthrough) video the composite path visibly brightens the picture.
	 * Skipping also just avoids doodling over movies/games. */
	Client *fc;
	wl_list_for_each(fc, &clients, link) {
		if (fc->isfullscreen && VISIBLEON(fc, selmon) && !fc->isminimized &&
				!fc->iskilling)
			return false;
	}
	int32_t bar_h = selmon->w.y - selmon->m.y; /* top exclusive zone = bar */
	if (bar_h < 8)
		bar_h = 44; /* no top bar: fly across the top strip anyway */
	*x = selmon->m.x;
	*y = selmon->m.y;
	*w = selmon->m.width;
	*h = bar_h;
	return true;
}

/* compositor-native screenshot UI (screenshot_ui dispatcher): freezes the
 * focused output's most recently rendered frame into a full-screen overlay,
 * then lets the user pick a region/window/whole-screen to save + copy. */
typedef enum {
	ShotScreen,
	ShotRegion,
	ShotWindow,
	ShotRawHDR, /* dumps the raw 10-bit composited buffer for external HDR10
				 * video encoding -- see screenshot_ui_save_raw_hdr */
} ScreenshotMode;

typedef struct {
	/* armed by the dispatcher; fulfilled by rendermon() the next time it
	 * renders capture_mon, which hands us a locked reference to that
	 * frame's fully-rendered buffer before it is committed to the output */
	bool want_capture;
	Monitor *capture_mon;
	ScreenshotMode capture_mode;

	/* live overlay state once a frame has actually been captured */
	bool active;
	ScreenshotMode mode;
	Monitor *mon;
	struct wlr_buffer *frame; /* frozen frame, locked for our own use */

	struct wlr_scene_tree *tree;		 /* root of the overlay, in LyrScreenshot */
	struct wlr_scene_buffer *frame_node; /* the frozen frame, full-screen */
	struct wlr_scene_rect *dim[4];		 /* dim mask: top, bottom, left, right */
	struct wlr_scene_rect *border[4];	 /* selection border: same order */
	struct asteroidz_jump_label_node *label; /* "WxH" dimension tooltip */

	bool dragging;
	double start_x, start_y; /* layout coords, region drag anchor */
	struct wlr_box sel;		 /* layout coords, current selection/hover box */

	/* ShotWindow hit-testing runs against boxes SNAPSHOTTED when the frame
	 * was frozen, not the live scene: windows that keep animating (or get
	 * re-arranged) under the overlay would otherwise drift away from the
	 * frozen pixels, putting the selection rectangle -- and the final crop
	 * -- in the wrong place. Focus order (topmost first). */
	struct {
		Client *c;
		struct wlr_box box;
	} *snap;
	int32_t snap_len;
} ScreenshotUI;

/* named shotui, not screenshot_ui: that name is the dispatcher function */
static ScreenshotUI shotui = {0};

static int32_t enablegaps = 1; /* enables gaps, used by togglegaps */
static int32_t axis_apply_time = 0;
static int32_t axis_apply_dir = 0;
static int32_t scroller_focus_lock = 0;

static uint32_t swipe_fingers = 0;
static double swipe_dx = 0;
static double swipe_dy = 0;

bool render_border = true;

uint32_t chvt_backup_tag = 0;
bool allow_frame_scheduling = true;
char chvt_backup_selmon[32] = {0};

struct dvec2 *baked_points_move;
struct dvec2 *baked_points_open;
struct dvec2 *baked_points_tag;
struct dvec2 *baked_points_close;
struct dvec2 *baked_points_focus;
struct dvec2 *baked_points_opafadein;
struct dvec2 *baked_points_opafadeout;

static struct wl_event_source *hide_cursor_source;
static struct wl_event_source *keep_idle_inhibit_source;

/* float layout raises the newly-focused client automatically (see
 * focusclient() below) -- debounced through this timer instead of
 * instantly, so focus moving quickly across several overlapping floating
 * windows doesn't flicker-restack on every transient stop. Only ONE raise
 * can ever be pending at a time (a fresh focus change reschedules it), so
 * a single static pair is enough; cleared on the client's destroy so the
 * timer callback can never fire against a freed Client (see destroynotify). */
static struct wl_event_source *float_focus_raise_timer;
static Client *float_focus_raise_pending;
static struct wl_event_source *scroller_edge_scroll_source;
static int32_t scroller_edge_scroll_dir = UNDIR;
static bool cursor_hidden = false;
static bool tag_combo = false;
static const char *cli_config_path = NULL;
static bool cli_debug_log = false;
static bool cli_check_config = false;
static bool cli_check_schema = false;
static bool cli_list_schema = false;
static bool cli_dump_source = false;
static bool cli_list_dispatch = false;
static bool cli_list_rules = false;
static KeyMode keymode = {
	.mode = {'d', 'e', 'f', 'a', 'u', 'l', 't', '\0'},
	.isdefault = true,
};

static char *env_vars[] = {"DISPLAY",
						   "WAYLAND_DISPLAY",
						   "XDG_CURRENT_DESKTOP",
						   "XDG_SESSION_TYPE",
						   "XCURSOR_THEME",
						   "XCURSOR_SIZE",
						   "ASTEROIDZ_INSTANCE_SIGNATURE",
						   NULL};
static struct {
	enum wp_cursor_shape_device_v1_shape shape;
	struct wlr_surface *surface;
	int32_t hotspot_x;
	int32_t hotspot_y;
} last_cursor;

#include "client/client.h"
#ifdef XWAYLAND
/* Declared here rather than with the other XWayland statics below, because
 * config/parse_config.h is included before them and config_apply_live() has
 * to reach it -- flipping xwayland_force_scale_one must re-measure the
 * windows that are already open. */
static void client_update_x11_scale(Client *c);
#endif
#include "config/preset.h"
struct Pertag {
	uint32_t curtag, prevtag;
	/* optional user-facing name per tag (NULL = fall back to the tag
	 * number). Set from `tagrule=id:N,name:...` or the set_tag_name
	 * dispatcher; index 0 is the overview pseudo-tag. */
	char *names[LENGTH(tags) + 1];
	int32_t nmasters[LENGTH(tags) + 1];
	float mfacts[LENGTH(tags) + 1];
	int32_t no_hide[LENGTH(tags) + 1];
	int32_t no_render_border[LENGTH(tags) + 1];
	int32_t open_as_floating[LENGTH(tags) + 1];
	float scroller_default_proportion[LENGTH(tags) + 1];
	float scroller_default_proportion_single[LENGTH(tags) + 1];
	int32_t scroller_ignore_proportion_single[LENGTH(tags) + 1];
	struct DwindleNode *dwindle_root[LENGTH(tags) + 1];
	const Layout *ltidxs[LENGTH(tags) + 1];
	struct TagScrollerState *scroller_state[LENGTH(tags) + 1];
};
/* Defined after render/az_avk.h is available; named by the dispatch table in
 * parse_config.h, which is included first. */
static int32_t reset_avk_stats(const Arg *arg);
static int32_t reset_presentation(const Arg *arg);
static int32_t set_t_pipe(const Arg *arg);
static int32_t set_blur_rect_cap(const Arg *arg);
static int32_t set_blur_chain_trace(const Arg *arg);
static int32_t set_blur_cache(const Arg *arg);
static int32_t set_blur_cache_starve(const Arg *arg);
static int32_t set_frame_trace(const Arg *arg);
static int32_t dump_scene(const Arg *arg);
static int32_t damage_all(const Arg *arg);
static int32_t capture_output(const Arg *arg);
/* Same reason: reapply_cursor_style() lives in parse_config.h and destroys the
 * xcursor manager, which every borrowed cursor pointer depends on. Defined in
 * render/az_cursor.h, included later. */
static void az_cursor_manager_replaced(void);
/*
 * The ONE way to select a themed cursor.
 *
 * Forward-declared here because parse_config.h and bind_define.h are included
 * before render/az_cursor.h and both need it. They used to call wlroots'
 * wlr_cursor_set_xcursor() instead, which is not an equivalent -- see the
 * comment on az_cursor_set_xcursor() itself. Anything reaching for the
 * wlroots function from compositor code is a bug.
 */
static void az_cursor_set_xcursor(const char *name);
/* The xcursor manager was rebuilt: re-establish the image from the new one
 * without re-selecting a shape. */
static void az_cursor_theme_replaced(void);
#include "config/parse_config.h"
/* After parse_config.h: the self-check drives set_value_default,
 * override_config and parse_option directly. */
#include "config/config-schema-check.h"

static struct wl_signal asteroidz_print_status;

static struct wl_listener print_status_listener = {.notify =
													   handle_print_status};
static struct wl_listener cursor_axis = {.notify = axisnotify};
static struct wl_listener cursor_button = {.notify = buttonpress};
static struct wl_listener cursor_frame = {.notify = cursorframe};
static struct wl_listener cursor_motion = {.notify = motionrelative};
static struct wl_listener cursor_motion_absolute = {.notify = motionabsolute};
static struct wl_listener gpu_reset = {.notify = gpureset};
static struct wl_listener layout_change = {.notify = updatemons};
static struct wl_listener new_idle_inhibitor = {.notify = createidleinhibitor};
static struct wl_listener new_input_device = {.notify = inputdevice};
static struct wl_listener new_virtual_keyboard = {.notify = virtualkeyboard};
static struct wl_listener new_virtual_pointer = {.notify = virtualpointer};
static struct wl_listener new_pointer_constraint = {
	.notify = createpointerconstraint};
static struct wl_listener new_output = {.notify = createmon};
static struct wl_listener new_xdg_toplevel = {.notify = createnotify};
static struct wl_listener new_xdg_popup = {.notify = createpopup};
static struct wl_listener new_xdg_dialog = {.notify = createdialog};
static struct wl_listener new_xdg_decoration = {.notify = createdecoration};
static struct wl_listener new_layer_surface = {.notify = createlayersurface};
static struct wl_listener output_mgr_apply = {.notify = outputmgrapply};
static struct wl_listener output_mgr_test = {.notify = outputmgrtest};
static struct wl_listener output_power_mgr_set_mode = {.notify =
														   powermgrsetmode};
static struct wl_listener request_activate = {.notify = urgent};
static struct wl_listener request_cursor = {.notify = setcursor};
static struct wl_listener request_set_psel = {.notify = setpsel};
static struct wl_listener request_set_sel = {.notify = setsel};
static struct wl_listener request_set_cursor_shape = {.notify = setcursorshape};
static struct wl_listener request_start_drag = {.notify = requeststartdrag};
static struct wl_listener start_drag = {.notify = startdrag};
static struct wl_listener new_session_lock = {.notify = locksession};
static struct wl_listener drm_lease_request = {.notify = requestdrmlease};
static struct wl_listener keyboard_shortcuts_inhibit_new_inhibitor = {
	.notify = handle_keyboard_shortcuts_inhibit_new_inhibitor};
static struct wl_listener last_cursor_surface_destroy_listener = {
	.notify = last_cursor_surface_destroy};

#ifdef XWAYLAND
static void fix_xwayland_coordinate(struct wlr_box *geom);
static int32_t synckeymap(void *data);
static void activatex11(struct wl_listener *listener, void *data);
static void configurex11(struct wl_listener *listener, void *data);
static void createnotifyx11(struct wl_listener *listener, void *data);
static void dissociatex11(struct wl_listener *listener, void *data);
static void commitx11(struct wl_listener *listener, void *data);
static void associatex11(struct wl_listener *listener, void *data);
static void sethints(struct wl_listener *listener, void *data);
static void xwaylandready(struct wl_listener *listener, void *data);
static void setgeometrynotify(struct wl_listener *listener, void *data);
static struct wl_listener new_xwayland_surface = {.notify = createnotifyx11};
static struct wl_listener xwayland_ready = {.notify = xwaylandready};
static struct wlr_xwayland *xwayland;
static struct wl_event_source *sync_keymap;
#endif

/*
 * Pointer focus traffic, counted where wlroots is actually told.
 *
 * Not cursor state and not renderer state, and reported through avk-stats only
 * because that is the channel a running session can be read from. They answer
 * a question nothing else can: when a client's hover state changes while the
 * pointer and the window are both stationary, is the client being told the
 * pointer moved?
 *
 * Declared here rather than beside the cursor's own counters because
 * animation/client.h re-enters the pointer too and is included first.
 *
 * Monotonic on purpose -- reset_avk_stats does not clear them, so a sample
 * taken across a reproduction still has a usable baseline on either side.
 */
static uint64_t az_pointer_enters;
static uint64_t az_pointer_focus_clears;
static uint64_t az_pointer_motions;
/*
 * The ones nobody asked for: motionnotify(time == 0) is the compositor
 * re-deciding what is under a pointer that did not move. arrange() ends with
 * one, and a client that reconfigures itself several times while it starts up
 * drives several arranges -- each re-running the hit test against geometry
 * that is still moving.
 *
 * Separate from az_pointer_motions because that one cannot tell a synthetic
 * call from a real mouse: a 1000Hz mouse and a re-notification storm look
 * identical in it, which is exactly the ambiguity that made the first
 * measurement unfalsifiable.
 */
static uint64_t az_pointer_notify_internal;

#include "action/client.h"
#include "action/output.h"
#include "animation/shatter.h"
#include "animation/client.h"
#include "animation/common.h"
#include "animation/layer.h"
#include "animation/tag.h"
#include "ipc/session-bus.h"
#include "dispatch/bind_define.h"
/* Cursor image ownership, before AVK: AVK draws a software cursor from this
 * state, so it has to exist by the time az_avk.h is compiled. */
#include "render/az_cursor.h"
/* The rendering seam, before anything that builds a frame: ext-protocol's
 * tearing path is one of az_output_build_frame()'s four callers. */
#include "present/az_presenter_impl.h"

/*
 * The instant this pass's frame is meant to represent, for the animation code
 * to sample against (ADR-606).
 *
 * Currently the presenter's armed target where one exists. It falls back to
 * the arm instant rather than reading a clock, so there is exactly one time
 * source per pass either way -- the fallback changes WHICH instant, never how
 * many.
 *
 * NOTE, and it is the reason the value is not yet the target everywhere:
 * rendermon walks EVERY client on EVERY output's pass and mutates the client's
 * one shared animation state. Handing each pass its own output's target would
 * therefore make a window straddling two outputs alternate between two
 * instants up to ~10ms apart, frame by frame. Per-output sampling needs the
 * semantic/presentation state split (ADR-611) first; this threading is the
 * plumbing for it and is deliberately behaviour-neutral until then.
 */
static uint64_t az_sample_not_target;   /* I1: must stay 0 */
static uint64_t az_sample_total;

/*
 * ── AZ_BREAK_PRESENT_IDLE_WAKE (falsifier I5) ─────────────────────────────
 *
 * ADR-610's guarantee is STRUCTURAL: the presenter owns no wl_event_source, so
 * there is no object through which it could wake the loop, and a settled
 * desktop therefore executes none of it. A structural guarantee is worth
 * exactly as much as the test that tries to violate it -- so this break adds
 * the thing the design says does not exist: a 1s timer that schedules a frame,
 * the "just one refresh tick" every predictor invites.
 *
 * A healthy build presents nothing across a settled window. This one presents
 * about once a second, which is the difference between an idle laptop and a
 * warm one.
 */
static int32_t az_break_idle_wake_cb(void *data) {
	Monitor *m = data;
	if (m != NULL && m->wlr_output != NULL) {
		wlr_output_schedule_frame(m->wlr_output);
	}
	if (m != NULL && m->break_idle_wake_timer != NULL) {
		wl_event_source_timer_update(m->break_idle_wake_timer, 1000);
	}
	return 0;
}

static inline uint64_t az_frame_sample_ns(Monitor *m) {
	uint64_t t = az_presenter_sample_ns(m);
	if (t == 0) {
		/* No armed target -- a pass outside the presenter's knowledge. Fall
		 * back to the arm instant rather than to a clock read, so there is
		 * still exactly one time source for the pass. */
		t = m != NULL ? m->m8_arm_ns : 0;
	}
	/*
	 * ── AZ_BREAK_PRESENT_SAMPLE_NOW (falsifier I1) ────────────────────────
	 *
	 * Substitutes the CPU's "now" for the predicted presentation time -- the
	 * pre-M6A behaviour, and the defect ADR-606 exists to remove. A frame then
	 * shows the state it had when the CPU walked it rather than the state it
	 * should have when it lights up.
	 *
	 * The invariant this breaks is checkable without inferring anything from
	 * pixels: `sample_ns == target_ns` for every pass. az_sample_not_target
	 * counts violations and must be zero, so the break makes a counter move
	 * rather than making a picture subtly wrong.
	 */
	static int break_now = -1;
	if (break_now < 0) {
		break_now = getenv("AZ_BREAK_PRESENT_SAMPLE_NOW") != NULL;
		if (break_now) {
			wlr_log(WLR_ERROR, "M6A break: AZ_BREAK_PRESENT_SAMPLE_NOW -- "
				"animations sample CPU-now instead of the predicted "
				"presentation time; this is the pre-M6A staleness, restored");
		}
	}
	az_sample_total++;
	if (break_now) {
		struct timespec bn;
		clock_gettime(CLOCK_MONOTONIC, &bn);
		uint64_t now = (uint64_t)bn.tv_sec * 1000000000ull + (uint64_t)bn.tv_nsec;
		if (now != t) {
			az_sample_not_target++;
		}
		return now;
	}
	return t;
}
/* M12: the one luminance-rule precedence, between client.h (it reads a rule
 * off Client) and the renderer that applies it. */
#include "render/az_lum_rules.h"
#include "render/az_avk.h"
#include "present/az_tag_cost.h"
#include "render/az_dmabuf_caps.h"
#include "render/az_output.h"

/*
 * `amsg dispatch reset_avk_stats` -- zero the AVK counters in place.
 *
 * Benchmarking a workload should not require restarting the compositor, which
 * destroys the workload.
 */
static int32_t reset_avk_stats(const Arg *arg) {
	(void)arg;
	az_avk_stats_reset();
	wlr_log(WLR_INFO, "AVK: statistics reset");
	return 0;
}
/*
 * `amsg dispatch reset_presentation` -- zero the per-output presentation
 * counters in place.
 *
 * Measuring a REGIME rather than a session. The M-8 latency series mixes idle
 * and continuous frames, and those are not one population: on a VRR output the
 * mean is dominated by waiting for the display to be ready, which is queueing
 * and not pipeline latency. Separating them needs a reset that does not
 * restart the compositor, because restarting destroys the workload being
 * measured.
 *
 * The clock-domain proof is deliberately NOT cleared: it is a fact about the
 * backend, established once, and re-deriving it per measurement would make it
 * a repeated assumption again.
 */
/*
 * `amsg dispatch set_t_pipe,<microseconds>` -- ADR-605's VRR pipeline
 * constant, settable so it can be MEASURED rather than argued about.
 *
 * The value is not obvious and the obvious value is wrong: seeding it from the
 * idle arm-to-photons mean (9050us on DP-1) fixes the idle bias and wrecks the
 * loaded case, because under load the frame event fires essentially at the
 * previous present, so `now + t_pipe` overtakes `last_present + P_min` and the
 * max() picks the term that does not apply. Being able to set it live turns
 * that from an argument into a measurement.
 */
static int32_t set_t_pipe(const Arg *arg) {
	uint64_t us = arg != NULL && arg->i > 0 ? (uint64_t)arg->i : 0;
	Monitor *m;
	wl_list_for_each(m, &mons, link) {
		m->presenter.t_pipe_ns = us * 1000ull;
	}
	wlr_log(WLR_INFO, "M6A: t_pipe set to %llu us on every output",
		(unsigned long long)us);
	return 0;
}
static int32_t reset_presentation(const Arg *arg) {
	(void)arg;
	Monitor *m;
	wl_list_for_each(m, &mons, link) {
		m->present_count = m->present_dropped = m->present_no_stamp = 0;
		m->present_interval_ns = 0;
		m->present_interval_rejected = 0;
		m->present_cadence_1x = m->present_cadence_2x = m->present_cadence_3x
			= 0;
		m->present_last_ns = 0;
		m->present_last_seq = 0;
		m->m8_samples = m->m8_unmatched = 0;
		m->m8_arm_sum_ns = m->m8_arm_min_ns = m->m8_arm_max_ns = 0;
		m->m8_commit_sum_ns = m->m8_commit_min_ns = m->m8_commit_max_ns = 0;
		m->m8_armed = false;
		memset(m->m8_hist, 0, sizeof(m->m8_hist));
		/* The presenter's own series too, so a regime measurement covers both
		 * views. Not a full epoch reset: the mode has not changed, so the
		 * phase and the proven clock domain stay valid. */
		m->presenter.err_count = 0;
		m->presenter.err_sum_ns = 0;
		m->presenter.err_abs_sum_ns = 0;
		m->presenter.err_min_ns = m->presenter.err_max_ns = 0;
		m->presenter.obs_when_sum_ns = 0;
		m->presenter.obs_seq_sum = 0;
	}
	wlr_log(WLR_INFO, "M6A: presentation counters reset");
	return 0;
}
/*
 * `amsg dispatch set_blur_rect_cap,<n>` -- the blur damage rectangle cap.
 *
 * DIAGNOSTIC. Past this many rectangles a blur's rebuild region collapses to
 * its bounding box, which is conservative --- the box contains the region, so
 * the result is identical and only the work grows. Live, on a tag transition,
 * that fires on a fifth of all blur chains at 1.74x inflation, and it has never
 * fired on any headless fixture.
 *
 * A dispatch rather than the environment because `restart` re-execs with the
 * same environ, so an env-only knob cannot be A/B'd against a running session
 * --- and restarting into a new value destroys the workload that fragments the
 * region in the first place.
 */
/*
 * `amsg dispatch set_frame_trace,<0|1>` -- per-frame tracing, live.
 *
 * Turns on both the AVK timestamp READ line and the tag-transition trace, which
 * together give one frame's GPU cost beside that frame's transition progress
 * and visible areas. That pairing is the only thing that can answer "what does
 * a transition frame cost" without going through a percentile --- and the
 * percentiles are aggregated across outputs of different size and refresh, so
 * they have now sent this investigation the wrong way three times.
 *
 * DIAGNOSTIC: it logs several lines per frame at ERROR.
 */
static int32_t set_frame_trace(const Arg *arg) {
	bool on = arg != NULL && arg->i != 0;
	az_tagtrace_runtime = on ? 1 : 0;
	az_avk_set_frame_trace(on);
	wlr_log(WLR_INFO, "frame trace %s", on ? "ON" : "off");
	return 0;
}
/*
 * `amsg dispatch set_blur_chain_trace,<0|1>` -- one log line per blur chain.
 *
 * A chain's cost is not a property of the frame it is in; it is a property of
 * WHAT THE CHAIN IS FOR. Aggregating a tooltip's backdrop with a maximised
 * window's produced the "chains=3 is slower than chains=4" reading, which is
 * true of the aggregate and says nothing about the renderer. This prints the
 * role and the geometry, so a slow frame can be decomposed into the chains that
 * made it slow rather than into a chain COUNT.
 *
 * DIAGNOSTIC: several lines per frame on a populated desktop.
 */
static int32_t set_blur_chain_trace(const Arg *arg) {
	bool on = arg != NULL && arg->i != 0;
	az_avk_set_blur_chain_trace(on);
	wlr_log(WLR_INFO, "AVK: blur chain trace %s", on ? "ON" : "off");
	return 0;
}
/*
 * `amsg dispatch set_blur_cache,<0|1>` -- the monitor background blur cache.
 *
 * The A/B arm, live. With it off every backdrop blur reconstructs the
 * background for itself, which is what AVK did before M4I; with it on the
 * background is built when its source changes and reused until it changes
 * again. Same binary, same session, same windows -- which is the only way to
 * compare two arms without also comparing two GPU thermal states and two
 * different sets of animations.
 */
static int32_t set_blur_cache(const Arg *arg) {
	bool on = arg != NULL && arg->i != 0;
	az_avk_set_blur_cache(on);
	wlr_log(WLR_INFO, "AVK: monitor background blur cache %s", on ? "ON" : "off");
	return 0;
}
/*
 * `amsg dispatch set_blur_cache_starve,<0|1|2>` -- 0 none, 1 plain, 2 dark.
 *
 * The starved kind is treated as having no damaged consumer, which is what an
 * ordinary frame does to whichever kind the damage missed. It exists so a
 * fixture can put the cache in that state ON PURPOSE and then ask what the
 * other kind's rebuild did to it.
 *
 * 1-BASED, WITH 0 MEANING NONE, so that a dispatch with a missing or
 * unparseable argument (which arrives as 0) turns the instrument OFF rather
 * than silently starving the plain image on a session nobody meant to break.
 */
static int32_t set_blur_cache_starve(const Arg *arg) {
	int v = arg != NULL ? arg->i : 0;
	az_avk_set_blur_cache_starve(v - 1);
	wlr_log(WLR_INFO, "AVK: blur cache starve -> %s",
		v == 1 ? "plain" : v == 2 ? "dark" : "none");
	return 0;
}
static int32_t set_blur_rect_cap(const Arg *arg) {
	int cap = arg != NULL ? arg->i : 0;
	avk_render_set_damage_rect_cap(cap);
	wlr_log(WLR_INFO, "AVK: blur damage rectangle cap -> %d%s", cap,
		cap >= 1 ? "" : " (default)");
	return 0;
}
/*
 * `amsg dispatch dump_scene` -- log the next frame's AVK command stream.
 *
 * Armed rather than scheduled: AVK_SCENE_DUMP names a frame NUMBER, and a
 * headless test cannot know which frame its window will be on. See
 * az_avk_dumping().
 */
static int32_t dump_scene(const Arg *arg) {
	(void)arg;
	az_avk_dump_armed = true;
	wlr_log(WLR_INFO, "AVK: the next frame's scene and command stream will be "
		"logged");
	return 0;
}

/*
 * `amsg dispatch damage_all` -- mark every output entirely damaged.
 *
 * THE DAMAGE ORACLE, and the reason it is a dispatch rather than an environment
 * variable: a build-time switch can only compare two RUNS, and two runs of a
 * compositor do not place windows, lay out text or schedule frames identically.
 * This compares two FRAMES of one run.
 *
 *     settle -> screenshot -> damage_all -> screenshot
 *
 * The second frame reconstructs every pixel from the clear upward. If damage
 * tracking left anything stale -- a blur fringe outside the region its source
 * change was reported in, most of all -- the two screenshots differ, and the
 * difference is exactly the staleness. Identical screenshots are the strongest
 * statement available that a partially damaged desktop is the desktop.
 *
 * It costs one full redraw and nothing afterwards, so it is safe to leave in a
 * shipping build; contrib/avk-blur-damage-test.sh is its caller.
 */
static int32_t damage_all(const Arg *arg) {
	(void)arg;
	Monitor *m;
	wl_list_for_each(m, &mons, link) {
		if (m->scene_output == NULL) {
			continue;
		}
		wlr_damage_ring_add_whole(&m->scene_output->damage_ring);
		wlr_output_schedule_frame(m->wlr_output);
	}
	wlr_log(WLR_INFO, "every output marked fully damaged");
	return 0;
}

/*
 * `amsg dispatch capture_output` -- write every output's next finished frame to
 * AZ_AVK_CAPTURE_DIR as a binary PPM.
 *
 * THE ONLY WAY TO SEE A ROTATED FRAME. grim captures nothing at all from a 90
 * or 270 degree output on the headless backend, so every pixel assertion at
 * those transforms had to be skipped with a stated reason -- and a rotation
 * nobody can look at is a rotation nothing can test. This reads back the actual
 * scan-out attachment AVK rendered, after compositing and before presentation,
 * at any transform.
 *
 * It damages the output as well as arming the capture. An output with nothing
 * to redraw renders no frame, so an armed capture on a settled desktop would
 * simply never fire -- the same trap `damage_all` exists for.
 *
 * TEST TOOL. The capture path waits for the GPU, so it must never be armed on a
 * frame anybody is waiting to see.
 */
static int32_t capture_output(const Arg *arg) {
	(void)arg;
	Monitor *m;
	int armed = 0;
	wl_list_for_each(m, &mons, link) {
		if (m->scene_output == NULL || m->avk == NULL
				|| m->avk->slot == NULL) {
			continue;
		}
		m->avk->capture_pending = true;
		avk_oracle_arm_capture(&m->avk->slot->renderer.oracle);
		wlr_damage_ring_add_whole(&m->scene_output->damage_ring);
		wlr_output_schedule_frame(m->wlr_output);
		armed++;
	}
	wlr_log(WLR_INFO, "capture armed on %d output(s)", armed);
	return 0;
}

/* M13B: direct scanout eligibility and the attempt. BEFORE ext-protocol/all.h
 * because tearing.h is inside it and a torn frame can scan out too -- see the
 * note in apply_tear_state(). */
#include "render/az_scanout.h"
#include "ext-protocol/all.h"
/* M6B/D6. The ONE preferred-colour policy, before either protocol frontend
 * that serializes it -- so neither can invent its own. */
#include "render/az_preferred.h"
#include "ext-protocol/az_cm_caps.h"
#include "ext-protocol/frog-color-management.h"
#include "ext-protocol/wp-color-management.h"
/* M11. The per-surface intent snapshot, after the colour frontends because it
 * reads the same description multiplexer they register. */
#include "render/az_intent.h"
#include "fetch/fetch.h"
#include "ipc/ipc.h"
#include "ipc/portals.h"
#include "layout/floating.h"
#include "layout/arrange.h"
#include "layout/dwindle.h"
#include "layout/horizontal.h"
#include "common/async-spawn.h"
#include "layout/overview.h"
#include "layout/scroll.h"

void client_change_mon(Client *c, Monitor *m, uint32_t newtags) {
	setmon(c, m, newtags, true);
	if (c->isfloating) {
		c->float_geom = c->geom =
			setclient_coordinate_center(c, c->mon, c->geom, 0, 0);
	}
}

void applybounds(Client *c, struct wlr_box *bbox) {
	/* set minimum possible */
	c->geom.width = ASTEROIDZ_MAX(1 + 2 * (int32_t)c->bw, c->geom.width);
	c->geom.height = ASTEROIDZ_MAX(1 + 2 * (int32_t)c->bw, c->geom.height);

	/* i3-style: keep a grabbable strip of a floating window on-screen so it
	 * can't be dragged fully off an edge and lost. keep px are clamped to the
	 * window's own size (a window smaller than the strip stays fully visible).
	 * float_keep_onscreen == 0 restores the old touch-the-edge behavior. */
	int32_t keep = c->isfloating ? config.float_keep_onscreen : 0;
	if (keep > 0) {
		int32_t kx = ASTEROIDZ_MIN(keep, c->geom.width);
		int32_t ky = ASTEROIDZ_MIN(keep, c->geom.height);
		if (c->geom.x > bbox->x + bbox->width - kx)
			c->geom.x = bbox->x + bbox->width - kx;
		if (c->geom.y > bbox->y + bbox->height - ky)
			c->geom.y = bbox->y + bbox->height - ky;
		if (c->geom.x + c->geom.width < bbox->x + kx)
			c->geom.x = bbox->x + kx - c->geom.width;
		if (c->geom.y + c->geom.height < bbox->y + ky)
			c->geom.y = bbox->y + ky - c->geom.height;
		return;
	}

	if (c->geom.x >= bbox->x + bbox->width)
		c->geom.x = bbox->x + bbox->width - c->geom.width;
	if (c->geom.y >= bbox->y + bbox->height)
		c->geom.y = bbox->y + bbox->height - c->geom.height;
	if (c->geom.x + c->geom.width <= bbox->x)
		c->geom.x = bbox->x;
	if (c->geom.y + c->geom.height <= bbox->y)
		c->geom.y = bbox->y;
}

void clear_fullscreen_and_maximized_state(Monitor *m) {
	Client *fc = NULL;
	wl_list_for_each(fc, &clients, link) {
		if (fc && VISIBLEON(fc, m) && ISFULLSCREEN(fc)) {
			clear_fullscreen_flag(fc);
		}
	}
}

/* clear the fullscreen flag, restoring the border that was zeroed while fullscreen */
void clear_fullscreen_flag(Client *c) {

	if (c->mon->pertag->ltidxs[get_tags_first_tag_num(c->tags)]->id ==
			SCROLLER &&
		!c->isfloating) {
		return;
	}

	if (c->isfullscreen) {
		setfullscreen(c, false, true);
	}

	if (c->ismaximizescreen) {
		setmaximizescreen(c, 0, true);
	}
}

void client_pending_fullscreen_state(Client *c, int32_t isfullscreen) {
	const bool was_fullscreen = c->isfullscreen != 0;
	const bool now_fullscreen = isfullscreen != 0;

	c->isfullscreen = isfullscreen;

	if (c->foreign_toplevel && !c->iskilling)
		wlr_foreign_toplevel_handle_v1_set_fullscreen(c->foreign_toplevel,
													  isfullscreen);

	/* becoming/ceasing to be fullscreen changes whether this client is a
	 * candidate for mon_state_apply_color's per-content HDR metadata
	 * forwarding (see mon_hdr_scanout_candidate) -- fold a refresh into the
	 * monitor's next commit so it picks up (or drops) this client without
	 * waiting for an unrelated HDR toggle/retrain.
	 *
	 * ── ONLY ON AN ACTUAL TRANSITION ─────────────────────────────────────
	 *
	 * This used to arm on every call, without comparing. That is not cheap:
	 * the frame handler folds the flag in with allow_reconfiguration, which
	 * in this wlroots means .modeset = true -- a BLOCKING full modeset,
	 * whether or not the mode changed. The comment there accepts the cost as
	 * "one blocking commit on a deliberate, rare HDR change"; on a display
	 * that is permanently HDR it was neither deliberate nor rare.
	 *
	 * Most callers pass a state the client is already in. setfloating and
	 * setmaximizescreen clear a flag that is usually already clear, the
	 * scratchpad and unmap paths do the same, and the overview clears it for
	 * every window it lays out. Each of those was costing a modeset. A live
	 * session logged 58 of them, every one re-setting the SAME
	 * 3840x2160@143.999 mode, in bursts of up to eight in 1.3 seconds -- and
	 * libinput's "event processing lagging behind by 42-51ms" complaints fall
	 * inside the densest burst, which is a blocked event loop being felt as a
	 * pointer that freezes and jumps.
	 *
	 * A call that does not change the flag cannot change whether this client
	 * is a scanout candidate, so there is nothing for the commit to pick up.
	 */
	if (c->mon && c->mon->hdr && was_fullscreen != now_fullscreen)
		c->mon->hdr_pending_change = true;
}

void client_pending_maximized_state(Client *c, int32_t ismaximized) {
	c->ismaximizescreen = ismaximized;
	if (c->foreign_toplevel && !c->iskilling)
		wlr_foreign_toplevel_handle_v1_set_maximized(c->foreign_toplevel,
													 ismaximized);
}

void client_pending_minimized_state(Client *c, int32_t isminimized) {
	c->isminimized = isminimized;
	if (c->foreign_toplevel && !c->iskilling)
		wlr_foreign_toplevel_handle_v1_set_minimized(c->foreign_toplevel,
													 isminimized);
}

void show_scratchpad(Client *c) {
	c->is_scratchpad_show = 1;
	if (c->isfullscreen || c->ismaximizescreen) {
		client_pending_fullscreen_state(c, 0);
		client_pending_maximized_state(c, 0);
		c->bw = c->isnoborder ? 0 : config.borderpx;
	}

	/* return if fullscreen */
	if (!c->isfloating) {
		setfloating(c, 1);
		c->geom.width = c->iscustomsize
							? c->float_geom.width
							: c->mon->w.width * config.scratchpad_width_ratio;
		c->geom.height =
			c->iscustomsize ? c->float_geom.height
							: c->mon->w.height * config.scratchpad_height_ratio;
		// recompute the centered coordinates
		c->float_geom = c->geom = c->animainit_geom = c->animation.current =
			setclient_coordinate_center(c, c->mon, c->geom, 0, 0);
		c->iscustomsize = 1;
		resize(c, c->geom, 0);
	}

	c->oldtags = c->mon->tagset[c->mon->seltags];
	wl_list_remove(&c->link);					  // remove from its old position
	wl_list_insert(clients.prev->next, &c->link); // insert at the head
	show_hide_client(c);
	setborder_color(c);
}

void client_update_oldmonname_record(Client *c, Monitor *m) {
	if (!c || c->iskilling || !client_surface(c)->mapped)
		return;
	memset(c->oldmonname, 0, sizeof(c->oldmonname));
	strncpy(c->oldmonname, m->wlr_output->name, sizeof(c->oldmonname) - 1);
	c->oldmonname[sizeof(c->oldmonname) - 1] = '\0';
	/* the tag c is landing on here, not c->tags -- callers invoke this
	 * both before and after actually remapping c->tags to m's active tag,
	 * but they all converge on m->tagset[m->seltags] either way. */
	c->oldmontags = m->tagset[m->seltags];
}

void client_replace(Client *c, Client *w) {
	c->bw = w->bw;
	c->isfloating = w->isfloating;
	c->isurgent = w->isurgent;
	c->is_in_scratchpad = w->is_in_scratchpad;
	c->is_scratchpad_show = w->is_scratchpad_show;
	c->special_name = w->special_name;
	c->tags = w->tags;
	c->geom = w->geom;
	c->float_geom = w->float_geom;
	c->stack_inner_per = w->stack_inner_per;
	c->master_inner_per = w->master_inner_per;
	c->master_mfact_per = w->master_mfact_per;
	c->scroller_proportion = w->scroller_proportion;
	c->isglobal = w->isglobal;
	c->overview_backup_geom = w->overview_backup_geom;
	c->animation.current = w->animation.current;
	c->stack_proportion = w->stack_proportion;

	if (w->overview_scene_surface) {

		wlr_scene_node_reparent(&w->shield->node, w->overview_scene_surface);
		wlr_scene_node_raise_to_top(&w->shield->node);

		wlr_scene_node_destroy(&w->scene_surface->node);
		w->scene_surface = w->overview_scene_surface;
		w->overview_scene_surface = NULL;
	}

	if (c->mon && c->mon->isoverview && config.ov_no_resize) {
		overview_backup_surface(c);
	}

	if (w->titlebar_node) {
		asteroidz_tab_bar_node_set_enabled(w->titlebar_node, false);
	}
	if (w->titlebar_close_node) {
		asteroidz_tab_bar_node_set_enabled(w->titlebar_close_node, false);
	}

	/* global list swap: the fork keeps replaced/hidden clients OUT of the
	 * client lists, so use guarded remove/insert instead of upstream's
	 * is_logic_hide-based wl_list_safe_reinsert_* form */
	if (c->link.prev && c->link.next && c->link.prev != &c->link) {
		wl_list_remove(&c->link);
	}
	wl_list_init(&c->link);

	if (c->flink.prev && c->flink.next && c->flink.prev != &c->flink) {
		wl_list_remove(&c->flink);
	}
	wl_list_init(&c->flink);

	if (w->link.prev && w->link.next && w->link.prev != &w->link) {
		wl_list_insert(w->link.prev, &c->link);
		wl_list_remove(&w->link);
		wl_list_init(&w->link);
	}

	if (w->flink.prev && w->flink.next && w->flink.prev != &w->flink) {
		if (selmon && c == selmon->sel) {
			wl_list_insert(&fstack, &c->flink);
		} else {
			wl_list_insert(w->flink.prev, &c->flink);
		}
		wl_list_remove(&w->flink);
		wl_list_init(&w->flink);
	}

	client_remove_ext_foreign_toplevel(w);
	if (w->foreign_toplevel) {
		/* the handle outlives the monitor: it is created while w->mon is set,
		 * but an output going away clears w->mon without destroying it */
		if (w->mon)
			wlr_foreign_toplevel_handle_v1_output_leave(w->foreign_toplevel,
														w->mon->wlr_output);
		wlr_foreign_toplevel_handle_v1_destroy(w->foreign_toplevel);
		w->foreign_toplevel = NULL;
	}

	client_set_scene_enabled(w, false);
	client_set_scene_enabled(c, true);
	wlr_scene_node_set_enabled(&c->scene_surface->node, true);

	if (!c->foreign_toplevel && c->mon)
		add_foreign_toplevel(c);
	else if (c->foreign_toplevel && c->mon) {
		wlr_foreign_toplevel_handle_v1_output_enter(c->foreign_toplevel,
													c->mon->wlr_output);
	}

	client_pending_fullscreen_state(c, w->isfullscreen);
	client_pending_maximized_state(c, w->ismaximizescreen);
	client_pending_minimized_state(c, w->isminimized);

	if (!w->mon)
		return;

	const Layout *layout = w->mon->pertag->ltidxs[w->mon->pertag->curtag];

	if (layout->id == DWINDLE || layout->id == SCROLLER) {

		for (uint32_t t = 0; t < LENGTH(tags) + 1; t++) {
			/* dwindle */

			if (layout->id == DWINDLE) {

				DwindleNode **root = &w->mon->pertag->dwindle_root[t];
				dwindle_remove(root, c);
				DwindleNode *dnode = dwindle_find_leaf(*root, w);
				if (dnode)
					dnode->client = c;
			}

			// scroller
			if (layout->id == SCROLLER) {
				struct TagScrollerState *st = w->mon->pertag->scroller_state[t];
				if (!st)
					continue;
				/* first remove c's old node in any tag */
				struct ScrollerStackNode *cn = find_scroller_node(st, c);
				if (cn)
					scroller_node_remove(st, cn);

				/* transfer w's node (if it exists) to c */
				struct ScrollerStackNode *wn = find_scroller_node(st, w);
				if (wn)
					wn->client = c;
			}
		}
	}

	/* sync the global client fields for the currently active tag */
	if (layout->id == SCROLLER) {
		sync_scroller_state_to_clients(w->mon, w->mon->pertag->curtag);
	}
}

bool switch_scratchpad_client_state(Client *c) {

	if (config.scratchpad_cross_monitor && selmon && c->mon != selmon &&
		c->is_in_scratchpad) {
		// save the original monitor for size calculations
		Monitor *oldmon = c->mon;
		c->scratchpad_switching_mon = true;
		c->mon = selmon;
		reset_foreign_tolevel(c, oldmon, c->mon);
		client_update_oldmonname_record(c, selmon);

		// adjust window size for the new monitor
		c->float_geom.width =
			(int32_t)(c->float_geom.width * c->mon->w.width / oldmon->w.width);
		c->float_geom.height = (int32_t)(c->float_geom.height *
										 c->mon->w.height / oldmon->w.height);

		c->float_geom =
			setclient_coordinate_center(c, c->mon, c->float_geom, 0, 0);

		// only a visible scratchpad needs to be focused and return true
		if (c->is_scratchpad_show) {
			c->tags = get_tags_first_tag(selmon->tagset[selmon->seltags]);
			resize(c, c->float_geom, 0);
			arrange(selmon, false, false);
			focusclient(c, true);
			c->scratchpad_switching_mon = false;
			return true;
		} else {
			resize(c, c->float_geom, 0);
			c->scratchpad_switching_mon = false;
		}
	}

	if (c->is_in_scratchpad && c->is_scratchpad_show &&
		(c->mon->tagset[c->mon->seltags] & c->tags) == 0) {
		c->tags = c->mon->tagset[c->mon->seltags];
		arrange(c->mon, false, false);
		focusclient(c, true);
		return true;
	} else if (c->is_in_scratchpad && c->is_scratchpad_show &&
			   (c->mon->tagset[c->mon->seltags] & c->tags) != 0) {
		set_minimized(c);
		return true;
	} else if (c && c->is_in_scratchpad && !c->is_scratchpad_show) {
		show_scratchpad(c);
		return true;
	}

	return false;
}

void apply_named_scratchpad(Client *target_client) {
	Client *c = NULL;
	wl_list_for_each(c, &clients, link) {

		if (!config.scratchpad_cross_monitor && c->mon != selmon) {
			continue;
		}

		if (config.single_scratchpad && c->is_in_scratchpad &&
			c->is_scratchpad_show && c != target_client) {
			set_minimized(c);
		}
	}

	if (!target_client->is_in_scratchpad) {
		set_minimized(target_client);
		switch_scratchpad_client_state(target_client);
	} else
		switch_scratchpad_client_state(target_client);
}

/* ---------------- named special workspaces (Hyprland-style) -------------
 *
 * Unlike the (floating, single-instance-per-name) scratchpad above, a named
 * special workspace is a per-monitor overlay that can hold any number of
 * tiled or floating clients and slides in on top of whatever tag is
 * currently selected. Membership is tracked with Client::special_name
 * (NULL == not in a special workspace); visibility is driven purely by
 * comparing that against Monitor::active_special inside VISIBLEON(), so the
 * normal arrange()/layout code hides/shows/tiles special-workspace clients
 * for free. Names are interned so membership checks are pointer
 * comparisons instead of repeated strcmp() calls inside the hot VISIBLEON()
 * macro. */

#define MAX_SPECIAL_WORKSPACE_NAMES 64
static char *special_workspace_names[MAX_SPECIAL_WORKSPACE_NAMES];
static int32_t special_workspace_name_count = 0;

char *intern_special_workspace_name(const char *name) {
	if (!name || !name[0])
		return NULL;

	for (int32_t i = 0; i < special_workspace_name_count; i++) {
		if (strcmp(special_workspace_names[i], name) == 0)
			return special_workspace_names[i];
	}

	if (special_workspace_name_count >= MAX_SPECIAL_WORKSPACE_NAMES) {
		fprintf(stderr,
				"\033[1m\033[31m[ERROR]:\033[33m too many distinct named "
				"special workspaces (max %d), ignoring \"%s\"\n",
				MAX_SPECIAL_WORKSPACE_NAMES, name);
		return NULL;
	}

	special_workspace_names[special_workspace_name_count] = strdup(name);
	return special_workspace_names[special_workspace_name_count++];
}

/* Fill `buf` with tag `tag`'s user-facing name: the custom name if one was
 * set (via tagrule name: or set_tag_name), otherwise the tag number. */
static void tag_display_name(Monitor *m, uint32_t tag, char *buf, size_t len) {
	if (len == 0)
		return;
	if (m && m->pertag && tag <= LENGTH(tags) && m->pertag->names[tag] &&
		m->pertag->names[tag][0]) {
		snprintf(buf, len, "%s", m->pertag->names[tag]);
		return;
	}
	snprintf(buf, len, "%u", tag);
}

/* Slides the special workspace currently showing on `m` (if any) back out
 * and returns to the plain tag view underneath. */
void close_special_workspace(Monitor *m, bool want_animation) {
	if (!m || !m->active_special)
		return;

	m->active_special = NULL;
	m->special_transitioning = want_animation && config.animations;
	arrange(m, want_animation, false);
	m->special_transitioning = false;
	focusclient(focustop(m), 1);
	printstatus(IPC_WATCH_ARRANGGE);
}

/* Shows named special workspace `interned` on top of whatever tag is
 * currently active on `m`. A monitor can only show one special workspace
 * at a time, so if another one is already showing it is implicitly closed
 * (slid out) in the same arrange() pass. */
void open_special_workspace(Monitor *m, char *interned, bool want_animation) {
	Client *c = NULL;

	if (!m || !interned || m->active_special == interned)
		return;

	m->active_special = interned;
	m->special_transitioning = want_animation && config.animations;
	arrange(m, want_animation, false);
	m->special_transitioning = false;

	wl_list_for_each(c, &clients, link) {
		if (c->mon == m && c->special_name == interned) {
			focusclient(c, true);
			break;
		}
	}

	printstatus(IPC_WATCH_ARRANGGE);
}

void gpureset(struct wl_listener *listener, void *data) {
	struct wlr_renderer *old_drw = drw;
	struct wlr_allocator *old_alloc = alloc;
	struct Monitor *m = NULL;

	wlr_log(WLR_DEBUG, "gpu reset");

	if (!(drw = az_create_renderer(backend)))
		die("couldn't recreate renderer");
	az_require_vulkan_renderer(drw);

	if (!(alloc = wlr_allocator_autocreate(backend, drw)))
		die("couldn't recreate allocator");

	wl_list_remove(&gpu_reset.link);
	wl_signal_add(&drw->events.lost, &gpu_reset);

	/*
	 * The wl_compositor's renderer is a choice, not a copy of `drw`. In AVK
	 * mode it was deliberately set to NULL at startup so that wlroots does its
	 * protocol bookkeeping and nothing else, and client buffers reach AVK
	 * intact instead of arriving as a wlr_client_buffer that can report
	 * neither a dma-buf nor readable pixels -- see the long comment at the
	 * creation site. Handing `drw` back here would silently reinstate the
	 * wrapper topology for every commit after a GPU reset, and the failure
	 * would look like content disappearing rather than like a renderer
	 * change: exactly the wallpaper bug, resurrected by a code path nobody
	 * associates with it.
	 */
	wlr_compositor_set_renderer(compositor,
		NULL);

	wl_list_for_each(m, &mons, link) {
		wlr_output_init_render(m->wlr_output, alloc, drw);
	}

	wlr_allocator_destroy(old_alloc);
	wlr_renderer_destroy(old_drw);
}

void handlesig(int32_t signo) {
	if (signo == SIGCHLD)
		while (waitpid(-1, NULL, WNOHANG) > 0)
			;
	else if (signo == SIGINT || signo == SIGTERM)
		/* quit_now, NOT quit: a signal is not a question. quit() raises the
		 * exit confirmation and waits for a keystroke, so a compositor sent
		 * SIGTERM by a session manager, a shutdown, or a test harness put a
		 * prompt on screen and then sat there until it was SIGKILLed -- which
		 * is how a regression run started leaving a live compositor behind on
		 * every single module. This is the ONE installed handler; the
		 * quitsignal() this file also used to carry was never wired to
		 * anything, so fixing that one fixed nothing. */
		quit_now(NULL);
}

void toggle_hotarea(int32_t x_root, int32_t y_root) {
	// bottom-left hot area coordinate calculation, multi-monitor aware
	Arg arg = {0};

	// right at startup selmon may be NULL, but the pointer might already be
	// in the hot area, so we must guard against that to avoid a crash
	if (!selmon)
		return;

	if (grabc)
		return;

	// compute the hot area coordinates based on the configured corner
	unsigned hx, hy;

	switch (config.hotarea_corner) {
	case BOTTOM_RIGHT: // bottom-right
		hx = selmon->m.x + selmon->m.width - config.hotarea_size;
		hy = selmon->m.y + selmon->m.height - config.hotarea_size;
		break;
	case TOP_LEFT: // top-left
		hx = selmon->m.x + config.hotarea_size;
		hy = selmon->m.y + config.hotarea_size;
		break;
	case TOP_RIGHT: // top-right
		hx = selmon->m.x + selmon->m.width - config.hotarea_size;
		hy = selmon->m.y + config.hotarea_size;
		break;
	case BOTTOM_LEFT: // bottom-left (default)
	default:
		hx = selmon->m.x + config.hotarea_size;
		hy = selmon->m.y + selmon->m.height - config.hotarea_size;
		break;
	}

	// check whether the pointer is within the hot area
	int in_hotarea = 0;

	switch (config.hotarea_corner) {
	case BOTTOM_RIGHT: // bottom-right
		in_hotarea = (y_root > hy && x_root > hx &&
					  x_root <= (selmon->m.x + selmon->m.width) &&
					  y_root <= (selmon->m.y + selmon->m.height));
		break;
	case TOP_LEFT: // top-left
		in_hotarea = (y_root < hy && x_root < hx && x_root >= selmon->m.x &&
					  y_root >= selmon->m.y);
		break;
	case TOP_RIGHT: // top-right
		in_hotarea = (y_root < hy && x_root > hx &&
					  x_root <= (selmon->m.x + selmon->m.width) &&
					  y_root >= selmon->m.y);
		break;
	case BOTTOM_LEFT: // bottom-left (default)
	default:
		in_hotarea = (y_root > hy && x_root < hx && x_root >= selmon->m.x &&
					  y_root <= (selmon->m.y + selmon->m.height));
		break;
	}

	if (config.enable_hotarea == 1 && selmon->is_in_hotarea == 0 &&
		in_hotarea) {
		/* Mission Control: the hot corner only OPENS overview; moving the
		 * cursor (even back into the corner) never closes it -- dismissal is by
		 * click / keybind. */
		if (!selmon->isoverview)
			toggleoverview(&arg);
		selmon->is_in_hotarea = 1;
	} else if (config.enable_hotarea == 1 && selmon->is_in_hotarea == 1 &&
			   !in_hotarea) {
		selmon->is_in_hotarea = 0;
	}
}

/* hovering the pointer against a screen edge for scroller_edge_scroll_delay
 * ms advances to the next/prev column; it keeps advancing at that same
 * interval while the pointer stays put */
void check_scroller_edge_scroll(int32_t x_root, int32_t y_root) {
	int32_t new_dir = UNDIR;

	if (config.scroller_edge_scroll && selmon && !grabc &&
		!selmon->isoverview && is_scroller_layout(selmon)) {
		int32_t size = config.scroller_edge_scroll_size;

		if (x_root >= selmon->m.x && x_root < selmon->m.x + size)
			new_dir = LEFT;
		else if (x_root < selmon->m.x + selmon->m.width &&
				 x_root >= selmon->m.x + selmon->m.width - size)
			new_dir = RIGHT;
	}

	if (new_dir == scroller_edge_scroll_dir)
		return;

	scroller_edge_scroll_dir = new_dir;
	wl_event_source_timer_update(scroller_edge_scroll_source,
								 new_dir == UNDIR
									 ? 0
									 : (uint32_t)config.scroller_edge_scroll_delay);
}

int32_t scroller_edge_scroll_timeout(void *data) {
	if (scroller_edge_scroll_dir == UNDIR || !selmon || grabc ||
		selmon->isoverview || !is_scroller_layout(selmon)) {
		scroller_edge_scroll_dir = UNDIR;
		return 0;
	}

	Arg arg = {.i = scroller_edge_scroll_dir};
	Client *c = direction_select(&arg);
	if (!c) {
		/* reached the last column/row: stop until the pointer leaves and
		 * re-enters the edge */
		scroller_edge_scroll_dir = UNDIR;
		return 0;
	}

	focusclient(c, 1);
	if (config.warpcursor)
		warp_cursor(c);
	wl_event_source_timer_update(scroller_edge_scroll_source,
								 (uint32_t)config.scroller_edge_scroll_delay);
	return 0;
}

static void apply_rule_properties(Client *c, const ConfigWinRule *r) {
	APPLY_INT_PROP(c, r, isterm);
	APPLY_INT_PROP(c, r, allow_csd);
	APPLY_INT_PROP(c, r, force_ssd);
	APPLY_INT_PROP(c, r, force_fakemaximize);
	APPLY_INT_PROP(c, r, force_tiled_state);
	APPLY_INT_PROP(c, r, force_tearing);
	APPLY_INT_PROP(c, r, noswallow);
	APPLY_INT_PROP(c, r, nofocus);
	APPLY_INT_PROP(c, r, nofadein);
	APPLY_INT_PROP(c, r, nofadeout);
	APPLY_INT_PROP(c, r, no_force_center);
	APPLY_INT_PROP(c, r, isfloating);
	APPLY_INT_PROP(c, r, isfullscreen);
	APPLY_INT_PROP(c, r, isfakefullscreen);
	APPLY_INT_PROP(c, r, isnoborder);
	APPLY_INT_PROP(c, r, isnoshadow);
	APPLY_INT_PROP(c, r, isnotitlebar);
	APPLY_INT_PROP(c, r, noscanout);
	APPLY_INT_PROP(c, r, vrr_only_fullscreen);
	APPLY_INT_PROP(c, r, force_hdr);
	APPLY_INT_PROP(c, r, privacy_shield);
	APPLY_INT_PROP(c, r, ispinned);
	APPLY_INT_PROP(c, r, isnoradius);
	APPLY_INT_PROP(c, r, isnoanimation);
	APPLY_INT_PROP(c, r, isopensilent);
	APPLY_INT_PROP(c, r, istagsilent);
	APPLY_INT_PROP(c, r, isnamedscratchpad);
	APPLY_INT_PROP(c, r, isglobal);
	APPLY_INT_PROP(c, r, isoverlay);
	APPLY_INT_PROP(c, r, ignore_maximize);
	APPLY_INT_PROP(c, r, ignore_minimize);
	APPLY_INT_PROP(c, r, isnosizehint);
	APPLY_INT_PROP(c, r, idleinhibit_when_focus);
	APPLY_INT_PROP(c, r, isunglobal);
	APPLY_INT_PROP(c, r, noblur);
	APPLY_INT_PROP(c, r, allow_shortcuts_inhibit);

	APPLY_FLOAT_PROP(c, r, scroller_proportion);
	APPLY_FLOAT_PROP(c, r, scroller_proportion_single);
	APPLY_FLOAT_PROP(c, r, focused_opacity);
	APPLY_FLOAT_PROP(c, r, unfocused_opacity);
	APPLY_FLOAT_PROP(c, r, sdr_white_scale);
	APPLY_FLOAT_PROP(c, r, hdr_gain);

	APPLY_STRING_PROP(c, r, luminance_domain);
	APPLY_STRING_PROP(c, r, presentation_class);
	APPLY_STRING_PROP(c, r, animation_type_open);
	APPLY_STRING_PROP(c, r, animation_type_close);
}

void set_float_malposition(Client *tc) {
	Client *c = NULL;
	int32_t x, y, offset, xreverse, yreverse;
	x = tc->geom.x;
	y = tc->geom.y;
	xreverse = 1;
	yreverse = 1;
	offset = ASTEROIDZ_MIN(tc->mon->w.width / 20, tc->mon->w.height / 20);

	wl_list_for_each(c, &clients, link) {
		if (c->isfloating && c != tc && VISIBLEON(c, tc->mon) &&
			abs(x - c->geom.x) < offset && abs(y - c->geom.y) < offset) {

			x = c->geom.x + offset * xreverse;
			y = c->geom.y + offset * yreverse;
			if (x < tc->mon->w.x) {
				x = x + offset;
				xreverse = 1;
			}

			if (y < tc->mon->w.y) {
				y = y + offset;
				yreverse = 1;
			}

			if (x + tc->geom.width > tc->mon->w.x + tc->mon->w.width) {
				x = x - offset;
				xreverse = -1;
			}

			if (y + tc->geom.height > tc->mon->w.y + tc->mon->w.height) {
				y = y - offset;
				yreverse = -1;
			}
		}
	}

	tc->float_geom.x = tc->geom.x = x;
	tc->float_geom.y = tc->geom.y = y;
}

void client_reset_mon_tags(Client *c, Monitor *mon, uint32_t newtags) {
	if (!newtags && mon && !mon->isoverview) {
		c->tags = mon->tagset[mon->seltags];
	} else if (!newtags && mon && mon->isoverview) {
		c->tags = mon->ovbk_current_tagset;
	} else if (newtags) {
		c->tags = newtags;
	} else {
		c->tags = mon->tagset[mon->seltags];
	}
}

void check_match_tag_floating_rule(Client *c, Monitor *mon) {
	if (c->tags && !c->isfloating && mon && !c->swallowedby &&
		mon->pertag->open_as_floating[get_tags_first_tag_num(c->tags)]) {
		c->isfloating = 1;
	}
}

void applyrules(Client *c) {
	/* rule matching */
	const char *appid, *title;
	uint32_t i, newtags = 0;
	const ConfigWinRule *r;
	Monitor *m = NULL;
	Client *fc = NULL;
	Client *parent = NULL;

	if (!c)
		return;

	parent = client_get_parent(c);

	Monitor *mon = parent && parent->mon ? parent->mon : selmon;

	c->isfloating = client_is_float_type(c) || parent;

#ifdef XWAYLAND
	if (c->isfloating && client_is_x11(c)) {
		fix_xwayland_coordinate(&c->geom);
		c->float_geom = c->geom;
	}
#endif

	if (!(appid = client_get_appid(c)))
		appid = broken;
	if (!(title = client_get_title(c)))
		title = broken;

	for (i = 0; i < config.window_rules_count; i++) {

		r = &config.window_rules[i];

		// rule matching
		if (!is_window_rule_matches(r, c, appid, title))
			continue;

		// set general properties
		apply_rule_properties(c, r);

		// // set tags
		if (r->tags > 0) {
			newtags |= r->tags;
		} else if (parent) {
			newtags = parent->tags;
		}

		// set monitor of client
		wl_list_for_each(m, &mons, link) {
			if (match_monitor_spec(r->monitor, m)) {
				mon = m;
			}
		}

		if (c->isnamedscratchpad) {
			c->isfloating = 1;
		}

		// assign to a named special workspace (tiled or floating, unlike the
		// scratchpad above)
		if (r->special_workspace && r->special_workspace[0]) {
			c->special_name = intern_special_workspace_name(r->special_workspace);
		}

		// pinned windows are always floating
		if (c->ispinned) {
			c->isfloating = 1;
		}

		if (r->scroller_proportion > 0.0f) {
			c->iscustom_scroller_proportion = 1;
		}

		if (r->scroller_proportion_single > 0.0f) {
			c->iscustom_scroller_proportion_single = 1;
		}

		// set geometry of floating client

		if (r->width > 1)
			c->float_geom.width = r->width;
		else if (r->width > 0 && r->width <= 1)
			c->float_geom.width = round(mon->m.width * r->width);
		if (r->height > 1)
			c->float_geom.height = r->height;
		else if (r->height > 0 && r->height <= 1)
			c->float_geom.height = round(mon->m.height * r->height);

		if (r->width > 0 || r->height > 0) {
			c->iscustomsize = 1;
		}

		if (r->offsetx || r->offsety) {
			c->iscustompos = 1;
			c->float_geom = c->geom = setclient_coordinate_center(
				c, mon, c->float_geom, r->offsetx, r->offsety);
		}
		if (c->isfloating) {
			c->geom = c->float_geom.width > 0 && c->float_geom.height > 0
						  ? c->float_geom
						  : c->geom;
			if (!c->isnosizehint)
				client_set_size_bound(c);
		}
	}

	/* float layout: windows on such a tag open floating */
	if (mon && !c->isfloating && !c->isfullscreen &&
		mon->pertag
				->ltidxs[c->tags ? get_tags_first_tag_num(c->tags)
								 : mon->pertag->curtag]
				->id == FLOATING) {
		c->isfloating = 1;
		c->autofloated = 1;
	}

	if (mon)
		set_size_per(mon, c);

	// if no geom rule hit and is normal winodw, use the center pos and record
	// the hit size
	if (!c->iscustompos &&
		(!client_is_x11(c) || (c->geom.x == 0 && c->geom.y == 0))) {
		struct wlr_box pending_center_geom =
			c->iscustomsize ? c->float_geom : c->geom;
		/* auto-floated windows cascade instead of stacking dead-center (unless
		 * float_center_new, i3-style, which falls through to the centering
		 * branch below). The slot is burned once; applyrules runs again at map
		 * (after mapnotify re-read the surface geometry, dropping our x/y), so
		 * the stored slot is re-applied onto the now-real size. */
		if (c->autofloated && mon && !config.float_center_new) {
			if (!c->cascaded) {
				struct wlr_box slot =
					floating_cascade_box(mon, pending_center_geom);
				c->float_geom.x = slot.x;
				c->float_geom.y = slot.y;
				c->cascaded = 1;
			}
			pending_center_geom.x = c->float_geom.x;
			pending_center_geom.y = c->float_geom.y;
			c->float_geom = c->geom = pending_center_geom;
		} else {
			c->float_geom = c->geom = setclient_coordinate_center(
				c, mon, pending_center_geom, 0, 0);
		}
	} else if (!c->iscustomsize) {
		c->float_geom = c->geom;
	}

	/*-----------------------apply rule action-------------------------*/

	// rule action only apply after map not apply in the init commit
	struct wlr_surface *surface = client_surface(c);
	if (!surface || !surface->mapped)
		return;

	// apply swallow rule
	c->pid = client_get_pid(c);
	if (!c->noswallow && !c->isfloating && !client_is_float_type(c) &&
		!c->surface.xdg->initial_commit) {
		Client *p = termforwin(c);
		if (p && !p->isminimized) {
			c->swallowedby = p;
			p->swallowing = c;

			client_replace(c, p);

			mon = p->mon;
			newtags = p->tags;
		}
	}

	int32_t fullscreen_state_backup =
		c->isfullscreen || client_wants_fullscreen(c);

	bool should_init_get_focus =
		!c->isopensilent &&
		!(client_is_x11_popup(c) && client_should_ignore_focus(c)) && mon &&
		(!c->istagsilent || !newtags || newtags & mon->tagset[mon->seltags]);

	if (!should_init_get_focus) {
		if (c->flink.prev && c->flink.next && c->flink.prev != &c->flink) {
			wl_list_remove(&c->flink);
			wl_list_init(&c->flink);
		}
		wl_list_insert(fstack.prev, &c->flink);
	}

	setmon(c, mon, newtags, should_init_get_focus);

	if (!c->isfloating) {
		c->old_stack_inner_per = c->stack_inner_per;
		c->old_master_inner_per = c->master_inner_per;
	}

	if (c->mon &&
		!(c->mon == selmon && c->tags & c->mon->tagset[c->mon->seltags]) &&
		!c->isopensilent && !c->istagsilent) {
		c->animation.tag_from_rule = true;
		view_in_mon(&(Arg){.ui = c->tags}, true, c->mon, true);
	}

	setfullscreen(c, fullscreen_state_backup, true);

	if (c->isfakefullscreen) {
		setfakefullscreen(c, 1);
	}

	/*
	if there is a new non-floating window in the current tag, the fullscreen
	window in the current tag will exit fullscreen and participate in tiling
	*/
	wl_list_for_each(fc, &clients,
					 link) if (fc && fc != c && c->tags & fc->tags && c->mon &&
							   VISIBLEON(fc, c->mon) && ISFULLSCREEN(fc) &&
							   !c->isfloating) {
		clear_fullscreen_flag(fc);
		arrange(c->mon, false, false);
	}

	if (c->isfloating && !c->iscustompos && !c->isnamedscratchpad) {
		if (c->link.prev && c->link.next && c->link.prev != &c->link) {
			wl_list_remove(&c->link);
			wl_list_init(&c->link);
		}
		wl_list_insert(clients.prev, &c->link);
		set_float_malposition(c);
	}

	// apply named scratchpad rule
	if (c->isnamedscratchpad) {
		apply_named_scratchpad(c);
	}

	// apply overlay rule
	if (c->isoverlay && c->scene) {
		wlr_scene_node_reparent(&c->scene->node, layers[LyrOverlay]);
		wlr_scene_node_raise_to_top(&c->scene->node);
	}

	// apply pin rule: keep pinned windows above their siblings
	if (c->ispinned && c->scene) {
		wlr_scene_node_raise_to_top(&c->scene->node);
	}
}

void arrangelayer(Monitor *m, struct wl_list *list, struct wlr_box *usable_area,
				  int32_t exclusive) {
	LayerSurface *l = NULL;
	struct wlr_box full_area = m->m;

	wl_list_for_each(l, list, link) {
		struct wlr_layer_surface_v1 *layer_surface = l->layer_surface;

		if (exclusive != (layer_surface->current.exclusive_zone > 0) ||
			!layer_surface->initialized)
			continue;

		if (l->being_unmapped)
			continue;

		wlr_scene_layer_surface_v1_configure(l->scene_layer, &full_area,
											 usable_area);
		wlr_scene_node_set_position(&l->popups->node, l->scene->node.x,
									l->scene->node.y);
	}
}

void apply_window_snap(Client *c) {
	int32_t snap_up = 99999, snap_down = 99999, snap_left = 99999,
			snap_right = 99999;
	int32_t snap_up_temp = 0, snap_down_temp = 0, snap_left_temp = 0,
			snap_right_temp = 0;
	int32_t snap_up_screen = 0, snap_down_screen = 0, snap_left_screen = 0,
			snap_right_screen = 0;
	int32_t snap_up_mon = 0, snap_down_mon = 0, snap_left_mon = 0,
			snap_right_mon = 0;

	uint32_t cbw = !render_border || c->fake_no_border ? config.borderpx : 0;
	uint32_t tcbw;
	uint32_t cx, cy, cw, ch, tcx, tcy, tcw, tch;
	cx = c->geom.x + cbw;
	cy = c->geom.y + cbw;
	cw = c->geom.width - 2 * cbw;
	ch = c->geom.height - 2 * cbw;

	Client *tc = NULL;
	if (!c || !c->mon || !client_surface(c)->mapped || c->iskilling)
		return;

	if (!c->isfloating || !config.enable_floating_snap)
		return;

	wl_list_for_each(tc, &clients, link) {
		if (tc && tc->isfloating && !tc->iskilling &&
			client_surface(tc)->mapped && VISIBLEON(tc, c->mon)) {

			tcbw = !render_border || tc->fake_no_border ? config.borderpx : 0;
			tcx = tc->geom.x + tcbw;
			tcy = tc->geom.y + tcbw;
			tcw = tc->geom.width - 2 * tcbw;
			tch = tc->geom.height - 2 * tcbw;

			snap_left_temp = cx - tcx - tcw;
			snap_right_temp = tcx - cx - cw;
			snap_up_temp = cy - tcy - tch;
			snap_down_temp = tcy - cy - ch;

			if (snap_left_temp < snap_left && snap_left_temp >= 0) {
				snap_left = snap_left_temp;
			}
			if (snap_right_temp < snap_right && snap_right_temp >= 0) {
				snap_right = snap_right_temp;
			}
			if (snap_up_temp < snap_up && snap_up_temp >= 0) {
				snap_up = snap_up_temp;
			}
			if (snap_down_temp < snap_down && snap_down_temp >= 0) {
				snap_down = snap_down_temp;
			}
		}
	}

	snap_left_mon = cx - c->mon->m.x;
	snap_right_mon = c->mon->m.x + c->mon->m.width - cx - cw;
	snap_up_mon = cy - c->mon->m.y;
	snap_down_mon = c->mon->m.y + c->mon->m.height - cy - ch;

	if (snap_up_mon >= 0 && snap_up_mon < snap_up)
		snap_up = snap_up_mon;
	if (snap_down_mon >= 0 && snap_down_mon < snap_down)
		snap_down = snap_down_mon;
	if (snap_left_mon >= 0 && snap_left_mon < snap_left)
		snap_left = snap_left_mon;
	if (snap_right_mon >= 0 && snap_right_mon < snap_right)
		snap_right = snap_right_mon;

	snap_left_screen = cx - c->mon->w.x;
	snap_right_screen = c->mon->w.x + c->mon->w.width - cx - cw;
	snap_up_screen = cy - c->mon->w.y;
	snap_down_screen = c->mon->w.y + c->mon->w.height - cy - ch;

	if (snap_up_screen >= 0 && snap_up_screen < snap_up)
		snap_up = snap_up_screen;
	if (snap_down_screen >= 0 && snap_down_screen < snap_down)
		snap_down = snap_down_screen;
	if (snap_left_screen >= 0 && snap_left_screen < snap_left)
		snap_left = snap_left_screen;
	if (snap_right_screen >= 0 && snap_right_screen < snap_right)
		snap_right = snap_right_screen;

	if (snap_left < snap_right && snap_left < config.snap_distance) {
		c->geom.x = c->geom.x - snap_left;
	}

	if (snap_right <= snap_left && snap_right < config.snap_distance) {
		c->geom.x = c->geom.x + snap_right;
	}

	if (snap_up < snap_down && snap_up < config.snap_distance) {
		c->geom.y = c->geom.y - snap_up;
	}

	if (snap_down <= snap_up && snap_down < config.snap_distance) {
		c->geom.y = c->geom.y + snap_down;
	}

	c->float_geom = c->geom;
	resize(c, c->geom, 0);
}

void focuslayer(LayerSurface *l) {
	focusclient(NULL, 0);
	dwl_im_relay_set_focus(dwl_input_method_relay, l->layer_surface->surface);
	client_notify_enter(l->layer_surface->surface, wlr_seat_get_keyboard(seat));
}

void reset_exclusive_layers_focus(Monitor *m) {
	LayerSurface *l = NULL;
	int32_t i;
	bool neet_change_focus_to_client = false;
	uint32_t layers_above_shell[] = {
		ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
		ZWLR_LAYER_SHELL_V1_LAYER_TOP,
		ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM,
	};

	if (!m)
		return;

	for (i = 0; i < (int32_t)LENGTH(layers_above_shell); i++) {
		wl_list_for_each(l, &m->layers[layers_above_shell[i]], link) {
			if (l == exclusive_focus &&
				l->layer_surface->current.keyboard_interactive !=
					ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE) {

				exclusive_focus = NULL;

				neet_change_focus_to_client = true;
			}

			if (l->layer_surface->surface ==
					seat->keyboard_state.focused_surface &&
				l->being_unmapped) {
				neet_change_focus_to_client = true;
			}

			if (l->layer_surface->current.keyboard_interactive ==
					ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE &&
				l->layer_surface->surface ==
					seat->keyboard_state.focused_surface) {
				neet_change_focus_to_client = true;
			}

			if (locked ||
				l->layer_surface->current.keyboard_interactive !=
					ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE ||
				l->being_unmapped)
				continue;
			/* Deactivate the focused client. */
			exclusive_focus = l;
			neet_change_focus_to_client = false;
			if (l->layer_surface->surface !=
				seat->keyboard_state.focused_surface)
				focuslayer(l);
			return;
		}
	}

	if (neet_change_focus_to_client) {
		focusclient(focustop(selmon), 1);
	}
}

void arrangelayers(Monitor *m) {
	int32_t i;
	struct wlr_box usable_area = m->m;

	/* A DPMS-asleep monitor is disabled but keeps its last-known-good m->m/
	 * m->w (updatemons() skips zeroing them while m->asleep -- only a real
	 * removal does that), so it's still safe -- and necessary -- to arrange
	 * layers against it. Bailing out unconditionally on !enabled meant a
	 * layer-shell client whose surface's initial commit happened to land
	 * while its output was DPMS-off (e.g. waybar respawning during a long
	 * sleep) never got a configure event at all; after its own internal
	 * timeout it fell back to some client-side default width ("Timed out
	 * waiting for initial .configure" in waybar's log) and stayed wrong
	 * even after the output woke back up, until something else forced yet
	 * another reconfigure. Only truly-removed/never-enabled outputs (whose
	 * m->m may be zero/stale) still need to skip this. */
	if (!m->wlr_output->enabled && !m->asleep)
		return;

	if (m->iscleanuping)
		return;

	/* Arrange exclusive surfaces from top->bottom */
	for (i = 3; i >= 0; i--)
		arrangelayer(m, &m->layers[i], &usable_area, 1);

	if (!wlr_box_equal(&usable_area, &m->w)) {
		m->w = usable_area;
		arrange(m, false, false);
	}

	/* Arrange non-exlusive surfaces from top->bottom */
	for (i = 3; i >= 0; i--)
		arrangelayer(m, &m->layers[i], &usable_area, 0);
}

bool pointer_is_trackpad(struct wlr_pointer *pointer) {
	struct libinput_device *device;

	if (wlr_input_device_is_libinput(&pointer->base) &&
		(device = wlr_libinput_get_device_handle(&pointer->base))) {
		if (libinput_device_config_tap_get_finger_count(device) > 0) {
			return true;
		}
	}

	return false;
}

void // mouse scroll wheel event
axisnotify(struct wl_listener *listener, void *data) {
	/* This event is forwarded by the cursor when a pointer emits an axis event,
	 * for example when you move the scroll wheel. */
	struct wlr_pointer_axis_event *event = data;
	struct wlr_keyboard *keyboard, *hard_keyboard;
	uint32_t mods, hard_mods;
	AxisBinding *a;
	int32_t ji;
	uint32_t adir;
	double target_scroll_factor;
	// IDLE_NOTIFY_ACTIVITY;
	handlecursoractivity();
	wlr_idle_notifier_v1_notify_activity(idle_notifier, seat);
	wake_sleeping_monitors();

	if (check_trackpad_disabled(event->pointer)) {
		return;
	}

	hard_keyboard = &kb_group->wlr_group->keyboard;
	hard_mods = hard_keyboard ? wlr_keyboard_get_modifiers(hard_keyboard) : 0;

	keyboard = wlr_seat_get_keyboard(seat);
	mods = keyboard ? wlr_keyboard_get_modifiers(keyboard) : 0;

	mods = mods | hard_mods;

	/* overview: consume the scroll wheel entirely so it can't leak to the real
	 * scroller layout (which would desync the overview); pan the big area if a
	 * scroller preview enabled it */
	if (selmon && selmon->isoverview) {
		if (selmon->ov_scroll_tag != 0 && event->delta != 0)
			overview_main_scroll(selmon, cursor->x, cursor->y,
								 event->delta > 0 ? 1 : -1);
		return;
	}

	if (event->orientation == WL_POINTER_AXIS_VERTICAL_SCROLL)
		adir = event->delta > 0 ? AxisDown : AxisUp;
	else
		adir = event->delta > 0 ? AxisRight : AxisLeft;

	for (ji = 0; ji < config.axis_bindings_count; ji++) {
		if (config.axis_bindings_count < 1)
			break;
		a = &config.axis_bindings[ji];
		if (CLEANMASK(mods) == CLEANMASK(a->mod) && // modifiers match
			adir == a->dir && a->func) { // scroll direction matches and a handler exists
			if (event->time_msec - axis_apply_time >
					config.axis_bind_apply_timeout ||
				axis_apply_dir * event->delta < 0) {
				a->func(&a->arg);
				axis_apply_time = event->time_msec;
				axis_apply_dir = event->delta > 0 ? 1 : -1;
				return; // on a successful match, don't forward this scroll event to the client
			} else {
				axis_apply_dir = event->delta > 0 ? 1 : -1;
				axis_apply_time = event->time_msec;
				return;
			}
		}
	}

	/* TODO: allow usage of scroll whell for mousebindings, it can be
	 * implemented checking the event's orientation and the delta of the event
	 */
	/* Notify the client with pointer focus of the axis event. */

	target_scroll_factor = pointer_is_trackpad(event->pointer)
							   ? config.trackpad_scroll_factor
							   : config.axis_scroll_factor;

	wlr_seat_pointer_notify_axis(
		seat, // forward the scroll event to the client, i.e. the window
		event->time_msec, event->orientation,
		event->delta * target_scroll_factor,
		roundf(event->delta_discrete * target_scroll_factor), event->source,
		event->relative_direction);
}

int32_t ongesture(struct wlr_pointer_swipe_end_event *event) {
	struct wlr_keyboard *keyboard, *hard_keyboard;
	uint32_t mods, hard_mods;
	const GestureBinding *g;
	uint32_t motion;
	uint32_t adx = (int32_t)round(fabs(swipe_dx));
	uint32_t ady = (int32_t)round(fabs(swipe_dy));
	int32_t handled = 0;
	int32_t ji;

	if (event->cancelled) {
		return handled;
	}

	// Require absolute distance movement beyond a small thresh-hold
	if (adx * adx + ady * ady <
		config.swipe_min_threshold * config.swipe_min_threshold) {
		return handled;
	}

	if (adx > ady) {
		motion = swipe_dx < 0 ? SWIPE_LEFT : SWIPE_RIGHT;
	} else {
		motion = swipe_dy < 0 ? SWIPE_UP : SWIPE_DOWN;
	}

	hard_keyboard = &kb_group->wlr_group->keyboard;
	hard_mods = hard_keyboard ? wlr_keyboard_get_modifiers(hard_keyboard) : 0;

	keyboard = wlr_seat_get_keyboard(seat);
	mods = keyboard ? wlr_keyboard_get_modifiers(keyboard) : 0;

	mods = mods | hard_mods;

	for (ji = 0; ji < config.gesture_bindings_count; ji++) {
		if (config.gesture_bindings_count < 1)
			break;
		g = &config.gesture_bindings[ji];
		if (CLEANMASK(mods) == CLEANMASK(g->mod) &&
			swipe_fingers == g->fingers_count && motion == g->motion &&
			g->func) {
			g->func(&g->arg);
			handled = 1;
		}
	}
	return handled;
}

void swipe_begin(struct wl_listener *listener, void *data) {
	struct wlr_pointer_swipe_begin_event *event = data;

	if (config.disable_trackpad)
		return;

	// Forward swipe begin event to client
	wlr_pointer_gestures_v1_send_swipe_begin(pointer_gestures, seat,
											 event->time_msec, event->fingers);
}

void swipe_update(struct wl_listener *listener, void *data) {
	struct wlr_pointer_swipe_update_event *event = data;

	if (config.disable_trackpad)
		return;

	swipe_fingers = event->fingers;
	// Accumulate swipe distance
	swipe_dx += event->dx;
	swipe_dy += event->dy;

	// Forward swipe update event to client
	wlr_pointer_gestures_v1_send_swipe_update(
		pointer_gestures, seat, event->time_msec, event->dx, event->dy);
}

void swipe_end(struct wl_listener *listener, void *data) {
	struct wlr_pointer_swipe_end_event *event = data;

	if (config.disable_trackpad)
		return;
	ongesture(event);
	swipe_dx = 0;
	swipe_dy = 0;
	// Forward swipe end event to client
	wlr_pointer_gestures_v1_send_swipe_end(pointer_gestures, seat,
										   event->time_msec, event->cancelled);
}

void pinch_begin(struct wl_listener *listener, void *data) {
	struct wlr_pointer_pinch_begin_event *event = data;

	if (config.disable_trackpad)
		return;

	// Forward pinch begin event to client
	wlr_pointer_gestures_v1_send_pinch_begin(pointer_gestures, seat,
											 event->time_msec, event->fingers);
}

void pinch_update(struct wl_listener *listener, void *data) {
	struct wlr_pointer_pinch_update_event *event = data;

	if (config.disable_trackpad)
		return;

	// Forward pinch update event to client
	wlr_pointer_gestures_v1_send_pinch_update(
		pointer_gestures, seat, event->time_msec, event->dx, event->dy,
		event->scale, event->rotation);
}

void pinch_end(struct wl_listener *listener, void *data) {
	struct wlr_pointer_pinch_end_event *event = data;

	if (config.disable_trackpad)
		return;

	// Forward pinch end event to client
	wlr_pointer_gestures_v1_send_pinch_end(pointer_gestures, seat,
										   event->time_msec, event->cancelled);
}

void hold_begin(struct wl_listener *listener, void *data) {
	struct wlr_pointer_hold_begin_event *event = data;

	if (config.disable_trackpad)
		return;

	// Forward hold begin event to client
	wlr_pointer_gestures_v1_send_hold_begin(pointer_gestures, seat,
											event->time_msec, event->fingers);
}

void hold_end(struct wl_listener *listener, void *data) {
	struct wlr_pointer_hold_end_event *event = data;

	if (config.disable_trackpad)
		return;

	// Forward hold end event to client
	wlr_pointer_gestures_v1_send_hold_end(pointer_gestures, seat,
										  event->time_msec, event->cancelled);
}

Client *find_closest_tiled_client(Client *c) {
	Client *tc, *closest = NULL;
	long min_dist = LONG_MAX;
	Monitor *cursor_mon = xytomon(cursor->x, cursor->y);

	wl_list_for_each(tc, &clients, link) {
		if (tc == c || !ISTILED(tc) || !VISIBLEON(tc, cursor_mon))
			continue;

		if (cursor->x >= tc->geom.x &&
			cursor->x < tc->geom.x + tc->geom.width &&
			cursor->y >= tc->geom.y &&
			cursor->y < tc->geom.y + tc->geom.height) {
			return tc;
		}

		int32_t dx = tc->geom.x + (int32_t)(tc->geom.width / 2) - cursor->x;
		int32_t dy = tc->geom.y + (int32_t)(tc->geom.height / 2) - cursor->y;
		long dist = (long)dx * dx + (long)dy * dy;

		if (dist < min_dist) {
			min_dist = dist;
			closest = tc;
		}
	}

	return closest;
}

void place_drag_tile_client(Client *c) {
	Client *closest = find_closest_tiled_client(c);

	if (closest && closest->mon) {
		const Layout *layout =
			closest->mon->pertag->ltidxs[closest->mon->pertag->curtag];

		if (closest->drop_direction == UNDIR) {
			setfloating(c, 0);
			wl_list_remove(&c->link);
			wl_list_insert(closest->link.prev, &c->link);
			arrange(closest->mon, false, false);
			return;
		}

		if (layout->id == SCROLLER) {
			scroller_drop_tile(c, closest, 0);
			return;
		}
		if (layout->id == DWINDLE) {
			uint32_t tag = c->mon->pertag->curtag;
			bool insert_before = (closest->drop_direction == LEFT ||
								  closest->drop_direction == UP);
			bool split_h = (closest->drop_direction == LEFT ||
							closest->drop_direction == RIGHT);
			dwindle_insert(&c->mon->pertag->dwindle_root[tag], c, closest,
						   config.dwindle_split_ratio, insert_before, split_h,
						   !config.dwindle_drop_simple_split);
			setfloating(c, 0);
			return;
		}

		if (closest->drop_direction == LEFT || closest->drop_direction == UP) {
			wl_list_remove(&c->link);
			wl_list_insert(closest->link.prev, &c->link);
		} else {
			wl_list_remove(&c->link);
			wl_list_insert(&closest->link, &c->link);
		}
	}

	setfloating(c, 0);
}

bool check_trackpad_disabled(struct wlr_pointer *pointer) {
	if (!config.disable_trackpad) {
		return false;
	}

	return pointer_is_trackpad(pointer);
}

void // mouse button event
buttonpress(struct wl_listener *listener, void *data) {
	struct wlr_pointer_button_event *event = data;

	if (shotui.active) {
		screenshot_ui_handle_button(event);
		return;
	}

	if (!handle_buttonpress(event))
		wlr_seat_pointer_notify_button(seat, event->time_msec, event->button,
									   event->state);
}

bool handle_buttonpress(struct wlr_pointer_button_event *event) {
	struct wlr_keyboard *hard_keyboard, *keyboard;
	uint32_t hard_mods, mods;
	Client *c = NULL;
	LayerSurface *l = NULL;
	struct wlr_surface *surface;
	Client *tmpc = NULL;
	int32_t ji;
	const MouseBinding *m;
	struct wlr_surface *old_pointer_focus_surface =
		seat->pointer_state.focused_surface;

	handlecursoractivity();
	wlr_idle_notifier_v1_notify_activity(idle_notifier, seat);
	if (event->state == WL_POINTER_BUTTON_STATE_PRESSED)
		wake_sleeping_monitors();

	if (event->pointer && check_trackpad_disabled(event->pointer)) {
		return true;
	}

	switch (event->state) {
	case WL_POINTER_BUTTON_STATE_PRESSED:
		cursor_mode = CurPressed;
		set_selmon(xytomon(cursor->x, cursor->y));
		if (locked)
			break;

		xytonode(cursor->x, cursor->y, &surface, NULL, NULL, NULL, NULL);
		if (toplevel_from_wlr_surface(surface, &c, &l) >= 0) {
			if (c && c->scene->node.enabled &&
				(!client_is_unmanaged(c) || client_wants_focus(c)))
				focusclient(c, 1);

			if (surface != old_pointer_focus_surface) {
				wlr_seat_pointer_notify_clear_focus(seat);
				motionnotify(0, NULL, 0, 0, 0, 0);
			}

			// focus an on-demand-interactive layer, but must not steal focus from an exclusive-focus layer
			if (l && !exclusive_focus &&
				l->layer_surface->current.keyboard_interactive ==
					ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND) {
				focuslayer(l);
			}
		}

		// in overview mode: clicking a strip tile switches to (displays) that tag
		if (selmon && selmon->isoverview && event->button == BTN_LEFT) {
			uint32_t ttag = overview_tile_at(selmon, cursor->x, cursor->y);
			if (ttag >= 1 && ttag <= LENGTH(tags)) {
				toggleoverview(&(Arg){.i = 1});
				view(&(Arg){.ui = (1u << (ttag - 1))}, false);
				return true;
			}
		}

		// in overview mode, left click jumps to the window, right click closes it
		if (selmon && selmon->isoverview && event->button == BTN_LEFT && c) {
			toggleoverview(&(Arg){.i = 1});
			return true;
		}

		if (selmon && selmon->isoverview && event->button == BTN_RIGHT && c) {
			pending_kill_client(c);
			return true;
		}

		// handle click on a titlebar/title node. Search per-layer top-down
		// (like xytonode): per-window tabs live inside their client's scene
		// tree (LyrTile/LyrTop), monocle segments on LyrDecorate -- this
		// finds both AND respects stacking, so a covered tab can't be
		// clicked through. LyrFadeOut/LyrScreenshot are SKIPPED: close-fade
		// snapshots carry node.data pointers that are freed with the client,
		// so a click on a fading ghost must never dispatch through them.
		struct wlr_scene_node *node = NULL;
		for (int32_t li = NUM_LAYERS - 1; li >= 0 && !node; li--) {
			if (li == LyrFadeOut || li == LyrScreenshot)
				continue;
			node = wlr_scene_node_at(&layers[li]->node, cursor->x, cursor->y,
									 NULL, NULL);
		}
		if (node && node->data) {
			AsteroidzNodeData *nodedata = (AsteroidzNodeData *)node->data;
			if (nodedata->type == ASTEROIDZ_TITLE_NODE) {
				Client *c = nodedata->node_data;
				focusclient(c, 1);
			} else if (nodedata->type == ASTEROIDZ_TITLEBAR_CLOSE_NODE) {
				Client *c = nodedata->node_data;
				if (event->button == BTN_LEFT)
					pending_kill_client(c);
			} else if (nodedata->type == ASTEROIDZ_TITLEBAR_NODE) {
				Client *c = nodedata->node_data;
				/* a monocle background tab (not currently shown) should
				 * just switch focus, not start a move grab: it isn't
				 * really floating under the pointer, it's stacked behind
				 * the visible window. */
				bool is_monocle_bg_tab =
					c->mon && is_monocle_layout(c->mon) && c->is_monocle_hide;
				focusclient(c, 1);
				if (event->button == BTN_LEFT && !is_monocle_bg_tab &&
					(cursor_mode == CurNormal || cursor_mode == CurPressed) &&
					!client_is_unmanaged(c) && !c->isfullscreen &&
					!c->ismaximizescreen) {
					begin_move_or_resize(c, CurMove);
				}
			}
		}

		// while pointer focus is on a layer, don't check the virtual keyboard's mod state,
		// to avoid the layer's virtual keyboard getting stuck with a mod key held
		hard_keyboard = &kb_group->wlr_group->keyboard;
		hard_mods =
			hard_keyboard ? wlr_keyboard_get_modifiers(hard_keyboard) : 0;

		keyboard = wlr_seat_get_keyboard(seat);
		mods = keyboard && !l ? wlr_keyboard_get_modifiers(keyboard) : 0;

		mods = mods | hard_mods;

		for (ji = 0; ji < config.mouse_bindings_count; ji++) {
			if (config.mouse_bindings_count < 1)
				break;
			m = &config.mouse_bindings[ji];

			if (CLEANMASK(mods) == CLEANMASK(m->mod) &&
				event->button == m->button && m->func &&
				(CLEANMASK(m->mod) != 0 ||
				 (event->button != BTN_LEFT && event->button != BTN_RIGHT))) {
				m->func(&m->arg);
				return true;
			}
		}
		break;
	case WL_POINTER_BUTTON_STATE_RELEASED:
		/* If you released any buttons, we exit interactive move/resize mode. */
		if (!locked && cursor_mode != CurNormal && cursor_mode != CurPressed) {
			cursor_mode = CurNormal;
			/* Clear the pointer focus, this way if the cursor is over a surface
			 * we will send an enter event after which the client will provide
			 * us a cursor surface */
			wlr_seat_pointer_clear_focus(seat);
			motionnotify(0, NULL, 0, 0, 0, 0);
			/* Drop the window off on its new monitor */
			if (grabc == selmon->sel) {
				selmon->sel = NULL;
			}
			selmon = xytomon(cursor->x, cursor->y);
			client_update_oldmonname_record(grabc, selmon);
			setmon(grabc, selmon, 0, true);
			selmon->prevsel = ISTILED(selmon->sel) ? selmon->sel : NULL;
			selmon->sel = grabc;
			tmpc = grabc;
			grabc = NULL;
			start_drag_window = false;
			last_apply_drap_time = 0;
			if (tmpc->drag_to_tile && config.drag_tile_to_tile) {
				place_drag_tile_client(tmpc);
				tmpc->float_geom = tmpc->drag_tile_float_backup_geom;
			} else {
				apply_window_snap(tmpc);
			}
			tmpc->drag_to_tile = false;
			if (dropc) {
				dropc->enable_drop_area_draw = false;
				client_set_drop_area(dropc);
				dropc = NULL;
			}
			return true;
		} else {
			cursor_mode = CurNormal;
		}
		break;
	}
	/* If the event wasn't handled by the compositor, return false */
	return false;
}

void checkidleinhibitor(struct wlr_surface *exclude) {
	int32_t inhibited = 0;
	Client *c = NULL;
	struct wlr_surface *surface = NULL;
	struct wlr_idle_inhibitor_v1 *inhibitor;

	wl_list_for_each(inhibitor, &idle_inhibit_mgr->inhibitors, link) {
		surface = wlr_surface_get_root_surface(inhibitor->surface);

		if (exclude == surface) {
			continue;
		}

		toplevel_from_wlr_surface(inhibitor->surface, &c, NULL);

		if (config.idleinhibit_ignore_visible) {
			inhibited = 1;
			break;
		}

		struct wlr_scene_tree *tree = surface->data;
		if (!tree || (tree->node.enabled && (!c || !c->animation.tagouting))) {
			inhibited = 1;
			break;
		}
	}

	if (idle_inhibit_manual)
		inhibited = 1;

	/* A portal client -- a sandboxed video player, an installer -- has no
	 * surface to hang an idle_inhibit_v1 inhibitor on, so it asks over D-Bus
	 * instead. Same lever, different door: see ipc/inhibit-portal.h. */
	if (inhibit_portal_holds_idle())
		inhibited = 1;

	wlr_idle_notifier_v1_set_inhibited(idle_notifier, inhibited);

	/* Every half is compared, not just the effective one: with a video
	 * player already holding an inhibitor, flipping the manual flag changes
	 * nothing about whether idling happens, but it is still what the pill
	 * showing that flag has to redraw from. The portal generation is here for
	 * the same reason -- a second app taking an inhibition while one already
	 * holds it moves nothing about sleeping, and everything about the list of
	 * who is holding it that `get idle` hands out. */
	static bool pushed_manual = false;
	static uint32_t pushed_portal_gen = 0;
	if ((bool)inhibited != idle_inhibited ||
		idle_inhibit_manual != pushed_manual ||
		inhibit_portal_generation() != pushed_portal_gen) {
		idle_inhibited = inhibited;
		pushed_manual = idle_inhibit_manual;
		pushed_portal_gen = inhibit_portal_generation();
		ipc_notify_idle();
	}
}

void last_cursor_surface_destroy(struct wl_listener *listener, void *data) {
	last_cursor.surface = NULL;
	wl_list_remove(&listener->link);
}

void setcursorshape(struct wl_listener *listener, void *data) {
	struct wlr_cursor_shape_manager_v1_request_set_shape_event *event = data;
	if (cursor_mode != CurNormal && cursor_mode != CurPressed)
		return;
	/* This can be sent by any client, so we check to make sure this one is
	 * actually has pointer focus first. If so, we can tell the cursor to
	 * use the provided cursor shape. */
	if (event->seat_client == seat->pointer_state.focused_client) {
		/* Remove surface destroy listener if active */
		if (last_cursor.surface &&
			last_cursor_surface_destroy_listener.link.prev != NULL)
			wl_list_remove(&last_cursor_surface_destroy_listener.link);

		last_cursor.shape = event->shape;
		last_cursor.surface = NULL;
		/* cursor-shape resolves to an xcursor name, so the image path is the
		 * theme's -- but the REQUEST came from a client, and the two are worth
		 * counting apart when asking what a session exercised. */
		az_cursor.sets_shape++;
		if (!cursor_hidden)
			az_cursor_set_xcursor(wlr_cursor_shape_v1_name(event->shape));
	}
}

void cleanuplisteners(void) {
	wl_list_remove(&ext_manager_commit_listener.link); // 0.7
	wl_list_remove(&print_status_listener.link);
	wl_list_remove(&cursor_axis.link);
	wl_list_remove(&cursor_button.link);
	wl_list_remove(&cursor_frame.link);
	wl_list_remove(&cursor_motion.link);
	wl_list_remove(&cursor_motion_absolute.link);
	wl_list_remove(&tablet_tool_proximity.link);
	wl_list_remove(&tablet_tool_axis.link);
	wl_list_remove(&tablet_tool_button.link);
	wl_list_remove(&tablet_tool_tip.link);
	wl_list_remove(&gpu_reset.link);
	wl_list_remove(&new_idle_inhibitor.link);
	wl_list_remove(&layout_change.link);
	wl_list_remove(&new_input_device.link);
	wl_list_remove(&new_virtual_keyboard.link);
	wl_list_remove(&new_virtual_pointer.link);
	wl_list_remove(&new_pointer_constraint.link);
	wl_list_remove(&new_output.link);
	wl_list_remove(&new_xdg_toplevel.link);
	wl_list_remove(&new_xdg_decoration.link);
	wl_list_remove(&new_xdg_popup.link);
	wl_list_remove(&new_xdg_dialog.link);
	wl_list_remove(&new_layer_surface.link);
	wl_list_remove(&output_mgr_apply.link);
	wl_list_remove(&output_mgr_test.link);
	wl_list_remove(&output_power_mgr_set_mode.link);
	wl_list_remove(&request_activate.link);
	wl_list_remove(&request_cursor.link);
	wl_list_remove(&request_set_psel.link);
	wl_list_remove(&request_set_sel.link);
	wl_list_remove(&request_set_cursor_shape.link);
	wl_list_remove(&request_start_drag.link);
	wl_list_remove(&start_drag.link);
	wl_list_remove(&new_session_lock.link);
	wl_list_remove(&ext_image_copy_capture_new_session.link);
	wl_list_remove(&tearing_new_object.link);
	wl_list_remove(&keyboard_shortcuts_inhibit_new_inhibitor.link);
	if (drm_lease_manager) {
		wl_list_remove(&drm_lease_request.link);
	}
#ifdef XWAYLAND
	wl_list_remove(&new_xwayland_surface.link);
	wl_list_remove(&xwayland_ready.link);
#endif
}

void cleanup(void) {
	/* Paired with CLEANUP_END at the bottom. A shutdown that crashes leaves
	 * BEGIN with no END, which is the difference between "teardown was tested
	 * and passed" and "teardown was never reached" -- and a harness that
	 * cannot tell those apart reports the second as the first. */
	wlr_log(WLR_INFO, "CLEANUP_BEGIN");
	allow_frame_scheduling = false;

	ufo_egg_destroy(ufo_egg);
	ufo_egg = NULL;

	ipc_cleanup();
	session_bus_finish();
	cleanuplisteners();
	modern_protocols_finish();
	portals_finish();
#ifdef XWAYLAND
	wlr_xwayland_destroy(xwayland);
	xwayland = NULL;
#endif

	wl_display_destroy_clients(dpy);
	if (child_pid > 0) {
		kill(-child_pid, SIGTERM);
		waitpid(child_pid, NULL, 0);
	}
	/* Before the theme goes: az_cursor holds a lock on an image the manager
	 * owns, and its animation timer points into the event loop. */
	az_cursor_finish();
	wlr_xcursor_manager_destroy(cursor_mgr);

	destroykeyboardgroup(&kb_group->destroy, NULL);

	/* BEFORE the backend, because destroying the outputs destroys AVK's
	 * per-output synchronisation objects, and the last frame's submission
	 * still refers to them. See az_avk_quiesce(). */
	az_avk_quiesce();

	/* If it's not destroyed manually it will cause a use-after-free of
	 * wlr_seat. Destroy it until it's fixed in the wlroots side */
	wlr_backend_destroy(backend);

	/* MUST come after wlr_backend_destroy(): tearing down the outputs runs
	 * cleanupmon() -> closemon() -> focusclient(), which calls
	 * dwl_im_relay_set_focus() on this relay. Finishing it first left that
	 * path reading a freed relay -- confirmed live under ASAN as a
	 * heap-use-after-free at text-input.h:588, on every session exit. */
	dwl_im_relay_finish(dwl_input_method_relay);
	dwl_input_method_relay = NULL;

	/* Before the display: wlr_compositor asserts that its new_surface signal
	 * has no listeners left when it is torn down. */
	az_avk_detach();
	az_surface_detach();

	wl_display_destroy(dpy);
	/* Destroy after the wayland display (when the monitors are already
	   destroyed) to avoid destroying them with an invalid scene output. */
	wlr_scene_node_destroy(&scene->tree.node);

	/* After the scene and the outputs, because both of them hand images back
	 * on the way down and az_avk_finish() must be the last owner standing.
	 *
	 * It waits for the device to go idle FIRST and then destroys, in that
	 * order -- which this comment used to claim while the only wait was
	 * buried inside avk_device_destroy(), the very last call. Every resource
	 * freed before it was freed with no wait in front of it at all. */
	az_avk_finish();

	asteroidz_text_global_finish();
	wlr_log(WLR_INFO, "CLEANUP_END");
}

void cleanupmon(struct wl_listener *listener, void *data) {
	Monitor *m = wl_container_of(listener, m, destroy);
	LayerSurface *l = NULL, *tmp = NULL;
	uint32_t i;

	/*
	 * A wp-cm output object may outlive the output it describes -- a client
	 * still holds the resource and may still call get_image_description on it.
	 * The object is made inert rather than destroyed: answering from a stale
	 * wlr_output pointer is a use-after-free, and destroying the client's
	 * object out from under it is a protocol violation.
	 */
	az_wpcm_output_gone(m->wlr_output);

	/* don't leave the screenshot UI pointing at a monitor that's going away */
	if (shotui.capture_mon == m) {
		shotui.want_capture = false;
		shotui.capture_mon = NULL;
	}
	if (shotui.mon == m)
		screenshot_ui_cancel();

	m->iscleanuping = true;

	/* First: destroying the swapchain destroys its buffers, and each of those
	 * carries the addon that retires this output's target image. Leaving it
	 * until after the output is gone would mean rendering into a swapchain
	 * sized for a monitor that no longer exists. */
	az_avk_output_finish(m->avk);
	m->avk = NULL;

	/* Before the scene nodes below are torn down: the bar owns scene buffers
	 * and heap-allocated hit-test tags parented to this monitor's tree. */

	/* m->layers[i] are intentionally not unlinked */
	for (i = 0; i < LENGTH(m->layers); i++) {
		wl_list_for_each_safe(l, tmp, &m->layers[i], link)
			wlr_layer_surface_v1_destroy(l->layer_surface);
	}

	// clean ext-workspaces grouplab
	wlr_ext_workspace_group_handle_v1_output_leave(m->ext_group, m->wlr_output);
	wlr_ext_workspace_group_handle_v1_destroy(m->ext_group);
	cleanup_workspaces_by_monitor(m);

	wl_list_remove(&m->destroy.link);
	wl_list_remove(&m->frame.link);
	if (az_pace_on())
		wl_list_remove(&m->pace_present.link);
	wl_list_remove(&m->present.link);
	wl_list_remove(&m->link);
	wl_list_remove(&m->request_state.link);
	if (m->lock_surface)
		destroylocksurface(&m->destroy_lock_surface, NULL);
	m->wlr_output->data = NULL;
	wlr_output_layout_remove(output_layout, m->wlr_output);
	wlr_scene_output_destroy(m->scene_output);

	closemon(m);
	if (m->blur) {
		wlr_scene_node_destroy(&m->blur->node);
		m->blur = NULL;
	}
	if (m->skip_frame_timeout) {
		monitor_stop_skip_frame_timer(m);
		wl_event_source_remove(m->skip_frame_timeout);
		m->skip_frame_timeout = NULL;
	}
	if (m->render_timer) {
		wl_event_source_remove(m->render_timer);
		m->render_timer = NULL;
	}
	if (m->retrain_timer) {
		wl_event_source_remove(m->retrain_timer);
		m->retrain_timer = NULL;
	}
	m->wlr_output->data = NULL;

	cleanup_monitor_dwindle(m);
	cleanup_monitor_scroller(m);

	if (m->ov_dim) {
		wlr_scene_node_destroy(&m->ov_dim->node);
		m->ov_dim = NULL;
	}
	if (m->ov_strip_blur) {
		wlr_scene_node_destroy(&m->ov_strip_blur->node);
		m->ov_strip_blur = NULL;
	}
	if (m->ov_strip_bg) {
		wlr_scene_node_destroy(&m->ov_strip_bg->node);
		m->ov_strip_bg = NULL;
	}
	if (m->ov_strip_shadow) {
		wlr_scene_node_destroy(&m->ov_strip_shadow->node);
		m->ov_strip_shadow = NULL;
	}
	for (int32_t oi = 0; oi < OV_STRIP_WINS; oi++) {
		if (m->ov_snap[oi]) {
			wlr_scene_node_destroy(&m->ov_snap[oi]->node);
			m->ov_snap[oi] = NULL;
		}
	}
	for (int32_t oi = 0; oi < 16; oi++) {
		if (m->ov_main_crop[oi]) {
			wlr_scene_node_destroy(&m->ov_main_crop[oi]->node);
			m->ov_main_crop[oi] = NULL;
		}
		if (m->ov_main_bord[oi]) {
			wlr_scene_node_destroy(&m->ov_main_bord[oi]->node);
			m->ov_main_bord[oi] = NULL;
		}
	}
	for (int32_t oi = 0; oi < 4; oi++) {
		if (m->ov_void[oi]) {
			wlr_scene_node_destroy(&m->ov_void[oi]->node);
			m->ov_void[oi] = NULL;
		}
	}
	if (m->ov_main_wp) {
		wlr_scene_node_destroy(&m->ov_main_wp->node);
		m->ov_main_wp = NULL;
	}
	if (m->ov_main_shadow) {
		wlr_scene_node_destroy(&m->ov_main_shadow->node);
		m->ov_main_shadow = NULL;
	}
	for (int32_t vi = 0; vi < 4; vi++) {
		if (m->ov_vignette[vi]) {
			wlr_scene_node_destroy(&m->ov_vignette[vi]->node);
			m->ov_vignette[vi] = NULL;
		}
	}
	if (m->ov_main_border) {
		wlr_scene_node_destroy(&m->ov_main_border->node);
		m->ov_main_border = NULL;
	}
	if (m->ov_hover_hl) {
		wlr_scene_node_destroy(&m->ov_hover_hl->node);
		m->ov_hover_hl = NULL;
	}
	if (m->ov_main_chevron_l) {
		wlr_scene_node_destroy(&m->ov_main_chevron_l->node);
		m->ov_main_chevron_l = NULL;
	}
	if (m->ov_main_chevron_r) {
		wlr_scene_node_destroy(&m->ov_main_chevron_r->node);
		m->ov_main_chevron_r = NULL;
	}
	for (int32_t oi = 0; oi < OV_TAG_CELLS; oi++) {
		if (m->ov_cell_bg[oi]) {
			wlr_scene_node_destroy(&m->ov_cell_bg[oi]->node);
			m->ov_cell_bg[oi] = NULL;
		}
		if (m->ov_cell_wp[oi]) {
			wlr_scene_node_destroy(&m->ov_cell_wp[oi]->node);
			m->ov_cell_wp[oi] = NULL;
		}
		if (m->ov_cell_shadow[oi]) {
			wlr_scene_node_destroy(&m->ov_cell_shadow[oi]->node);
			m->ov_cell_shadow[oi] = NULL;
		}
		if (m->ov_cell_label[oi]) {
			asteroidz_jump_label_node_destroy(m->ov_cell_label[oi]);
			m->ov_cell_label[oi] = NULL;
		}
	}
	for (uint32_t ti = 0; ti <= LENGTH(tags); ti++)
		free(m->pertag->names[ti]);

	/*
	 * M6C. The cube is this monitor's, 1.6MB of it, and az_avk_output_finish()
	 * above has already released the GPU copy -- so nothing can still be
	 * sampling it. Freed here rather than in mon_load_icc_profile's clear path
	 * because a monitor going away never passes through that.
	 */
	az_icc_clut_free(m->icc_clut);
	m->icc_clut = NULL;

	free(m->pertag);
	free(m);
}

void closemon(Monitor *m) {
	/* update selmon if needed and
	 * move closed monitor's clients to the focused one */
	Client *c = NULL;
	int32_t i = 0, nmons = wl_list_length(&mons);
	if (!nmons) {
		selmon = NULL;
	} else if (m == selmon) {
		do /* don't switch to disabled mons */
			selmon = wl_container_of(mons.next, selmon, link);
		while (!selmon->wlr_output->enabled && i++ < nmons);

		if (!selmon->wlr_output->enabled)
			selmon = NULL;
	}

	wl_list_for_each(c, &clients, link) {
		if (c->mon == m) {
			bool was_focused = c->isfocused;

			if (selmon == NULL) {
				client_remove_ext_foreign_toplevel(c);
				if (c->foreign_toplevel) {
					wlr_foreign_toplevel_handle_v1_output_leave(
						c->foreign_toplevel, c->mon->wlr_output);
					wlr_foreign_toplevel_handle_v1_destroy(c->foreign_toplevel);
					c->foreign_toplevel = NULL;
				}

				c->mon = NULL;
			} else {
				/* newtags 0: land on selmon's current active tag so the
				 * client is actually visible, rather than carrying over a
				 * tag number that may not be shown on selmon at all. */
				client_change_mon(c, selmon, 0);

				/* a scroller/tile layout can easily leave a displaced
				 * client scrolled off-screen behind existing windows with
				 * no visible sign it's there; flag it the same way an
				 * xdg-activation request does, rather than silently
				 * stealing focus from whatever's already selected. */
				if (c != focustop(selmon)) {
					c->isurgent = 1;
					if (client_surface(c)->mapped)
						setborder_color(c);
				}
			}

			/* m is already unlinked from mons by the time we get here, so
			 * neither this loop nor the focusclient() call below (which
			 * only clears isfocused on clients of monitors still in mons)
			 * ever reaches a client that was focused on the removed
			 * monitor -- without this its border/opacity/foreign-toplevel
			 * activation state stays stuck "focused" forever. */
			if (was_focused && c->isfocused) {
				c->isfocused = false;
				client_set_unfocused_opacity_animation(c);
				if (c->foreign_toplevel)
					wlr_foreign_toplevel_handle_v1_set_activated(
						c->foreign_toplevel, false);
			}

			// record the oldmonname which is used to restore
			if (c->oldmonname[0] == '\0') {
				client_update_oldmonname_record(c, m);
			}
		}
	}
	if (selmon) {
		focusclient(focustop(selmon), 1);
		printstatus(IPC_WATCH_ARRANGGE);
	}
}

static void iter_layer_scene_buffers(struct wlr_scene_buffer *buffer,
									 int32_t sx, int32_t sy, void *user_data) {
	LayerSurface *l = user_data;
	struct wlr_scene_surface *scene_surface =
		wlr_scene_surface_try_from_buffer(buffer);
	if (!scene_surface || !l->blur_node) {
		return;
	}

	/* only mask the blur with the main layer surface, not subsurfaces */
	if (scene_surface->surface == l->layer_surface->surface) {
		wlr_scene_blur_set_transparency_mask_source(l->blur_node, buffer);
	}
}

/* scenefx 0.5 replaced per-buffer backdrop blur with explicit blur nodes:
 * keep one node per layer surface, placed below its surface tree. A
 * client-supplied ext-background-effect-v1 region refines the default. */
void layer_update_blur(LayerSurface *l) {
	struct wlr_layer_surface_v1 *layer_surface;
	struct background_effect_surface *effect;
	bool want;

	if (!l || !l->scene || !l->scene_layer)
		return;

	layer_surface = l->layer_surface;
	effect = background_effect_try_from_surface(layer_surface->surface);
	want = config.blur && (config.blur_layer || l->forceblur) && !l->noblur &&
		layer_surface->current.layer != ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM &&
		layer_surface->current.layer != ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND;

	/* a client region is an explicit opt-in, an empty one an opt-out */
	if (effect && effect->has_region) {
		want = config.blur && !l->noblur &&
			pixman_region32_not_empty(&effect->current_region);
	}

	if (!want) {
		if (l->blur_node) {
			wlr_scene_node_destroy(&l->blur_node->node);
			l->blur_node = NULL;
		}
		return;
	}

	if (!l->blur_node) {
		l->blur_node = wlr_scene_blur_create(l->scene, 0, 0);
		if (!l->blur_node)
			return;
		/* l->scene IS the layer surface's tree, so the blur node is a
		 * sibling of the surface buffer inside it: drop it to the bottom
		 * of that tree to render below the surface (place_below against
		 * the tree itself would assert: different parents) */
		wlr_scene_node_lower_to_bottom(&l->blur_node->node);
	}

	/* this runs on every layer-surface commit: only touch the scene when
	 * something actually changed. Layer surfaces (panels, popups) float
	 * above windows, so the cached bottom-layer blur would show wallpaper
	 * instead of the windows beneath: always blur live. */
	if (l->blur_node->should_only_blur_bottom_layer)
		wlr_scene_blur_set_should_only_blur_bottom_layer(l->blur_node,
														 false);
	/* actual_width/height is the layer-shell PROTOCOL's server-assigned size
	 * (only updated by an explicit compositor configure — see
	 * wlr_layer_surface_v1_configure in wlroots) — NOT the client's real
	 * buffer size. A content-sized/auto popup (e.g. gtk-layer-shell's
	 * WbPop-style popups) picks its own size and just commits a differently
	 * sized buffer without ever getting a fresh configure, so actual_width/
	 * height never changes and this comparison silently never re-triggers on
	 * that popup shrinking/growing (surviving until the surface is
	 * destroyed). surface->current.width/height is the real committed
	 * buffer size in surface-local coordinates and updates on every commit
	 * regardless of configure — use that instead. */
	if (l->blur_node->width != layer_surface->surface->current.width ||
		l->blur_node->height != layer_surface->surface->current.height) {
		wlr_scene_blur_set_size(l->blur_node,
								layer_surface->surface->current.width,
								layer_surface->surface->current.height);
		/* the surface resized while mapped: the once-only transparency mask
		 * below is now stale (sized for the old extent), so re-snapshot it —
		 * otherwise blur is clipped/overhangs until the surface is destroyed
		 * (e.g. a popup or notification panel growing/shrinking its content) */
		wlr_scene_node_for_each_buffer(&l->scene_layer->tree->node,
									   iter_layer_scene_buffers, l);
	}

	/* pass the client's region verbatim: it carries the rounded corners,
	 * so clipping by its bounding box would leave square blur "ears"
	 * poking out at the card corners */
	if (effect && effect->has_region) {
		wlr_scene_blur_set_region(l->blur_node, &effect->current_region);
	} else {
		wlr_scene_blur_set_region(l->blur_node, NULL);
	}

	/* ignore transparent regions: only blur where the surface draws; the
	 * mask is stable once set, so skip the buffer traversal afterwards */
	if (!wlr_scene_blur_get_transparency_mask_source(l->blur_node))
		wlr_scene_node_for_each_buffer(&l->scene_layer->tree->node,
									   iter_layer_scene_buffers, l);
}

/* Blur for a popup.
 *
 * A popup is neither a toplevel nor a layer surface, so nothing here used to
 * offer it one: a client could ask for background blur on a menu through
 * ext-background-effect-v1, get no error, and be handed a plain translucent
 * surface. asteroidz-bar's popovers are exactly that case -- they hang off the
 * bar's layer surface as xdg popups, and the frost stopped at the bar.
 *
 * Unlike a panel there is no sensible default here, so this is opt-in only: a
 * popup blurs when it asks to and not otherwise, because blurring every menu
 * on the desktop is not a default anyone chose.
 */
void popup_update_blur(Popup *popup) {
	struct background_effect_surface *effect;
	struct wlr_surface *surface;
	struct wlr_scene_tree *tree;
	bool want;

	if (!popup || !popup->wlr_popup || !popup->wlr_popup->base)
		return;

	surface = popup->wlr_popup->base->surface;
	if (!surface)
		return;
	/* commitpopup parks the scene tree here; without it there is nothing to
	 * put a blur node inside. */
	tree = surface->data;
	if (!tree)
		return;

	effect = background_effect_try_from_surface(surface);
	want = config.blur && effect && effect->has_region &&
		pixman_region32_not_empty(&effect->current_region);

	if (!want) {
		if (popup->blur_node) {
			wlr_scene_node_destroy(&popup->blur_node->node);
			popup->blur_node = NULL;
		}
		return;
	}

	if (!popup->blur_node) {
		popup->blur_node = wlr_scene_blur_create(tree, 0, 0);
		if (!popup->blur_node)
			return;
		/* below the surface buffer, inside the popup's own tree */
		wlr_scene_node_lower_to_bottom(&popup->blur_node->node);
		/* a popup floats above windows, so the cached bottom-layer blur
		 * would show it the wallpaper instead of what is actually under it */
		wlr_scene_blur_set_should_only_blur_bottom_layer(popup->blur_node,
														 false);
	}

	if (popup->blur_node->width != surface->current.width ||
		popup->blur_node->height != surface->current.height)
		wlr_scene_blur_set_size(popup->blur_node, surface->current.width,
								surface->current.height);

	/* verbatim, corners and all -- see layer_update_blur */
	wlr_scene_blur_set_region(popup->blur_node, &effect->current_region);
}

/* Reached from the ext-background-effect commit handler, which only has a
 * wl_surface: a popup resolves UP to its parent through
 * toplevel_from_wlr_surface, so without this a popover's region would be
 * applied to the bar's layer surface, where it means something else entirely.
 * Returns true when the surface was a popup and has been dealt with. */
bool popup_update_blur_from_surface(struct wlr_surface *surface) {
	struct wlr_xdg_popup *wlr_popup =
		wlr_xdg_popup_try_from_wlr_surface(surface);

	if (!wlr_popup || !wlr_popup->base || !wlr_popup->base->data)
		return false;

	popup_update_blur(wlr_popup->base->data);
	return true;
}

/* Lazily creates the monitor's shared optimized-blur cache node (the
 * producer wlr_scene_optimized_blur_create/WLR_SCENE_NODE_OPTIMIZED_BLUR
 * node that should_only_blur_bottom_layer consumers -- regular window blur
 * AND shadow_blur -- sample from). Idempotent: no-op if it already exists.
 * Needed because a consumer can start existing later than monitor creation
 * (blur/shadows_blur_background toggled on via a live config reload, not
 * just at startup), and createmon only creates it once, at that time. */
static void ensure_monitor_blur_node(Monitor *m) {
	if (!m || m->blur)
		return;
	m->blur = wlr_scene_optimized_blur_create(&scene->tree, 0, 0);
	wlr_scene_node_set_position(&m->blur->node, m->m.x, m->m.y);
	wlr_scene_node_reparent(&m->blur->node, layers[LyrBlur]);
	wlr_scene_optimized_blur_set_size(m->blur, m->m.width, m->m.height);
}

void layer_flush_blur_background(LayerSurface *l) {
	if (!config.blur && !config.shadows_blur_background)
		return;

	// if the background layer changed, mark the optimized blur background cache dirty
	if (l->layer_surface->current.layer ==
		ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND) {
		if (l->mon) {
			ensure_monitor_blur_node(l->mon);
			wlr_scene_optimized_blur_mark_dirty(l->mon->blur);
		}
	}
}

void maplayersurfacenotify(struct wl_listener *listener, void *data) {
	LayerSurface *l = wl_container_of(listener, l, map);
	struct wlr_layer_surface_v1 *layer_surface = l->layer_surface;
	int32_t ji;
	ConfigLayerRule *r;

	l->mapped = 1;

	if (!l->mon)
		return;
	strncpy(l->mon->last_open_surface, layer_surface->namespace,
			sizeof(l->mon->last_open_surface) - 1); // copy at most 255 characters
	l->mon->last_open_surface[sizeof(l->mon->last_open_surface) - 1] =
		'\0'; // ensure the string is null-terminated

	// initialize geometry
	get_layer_target_geometry(l, &l->geom);

	l->noanim = 0;
	l->dirty = false;
	l->noblur = 0;
	l->forceshadow = 0;
	l->shadow = NULL;
	l->shadow_blur = NULL;
	l->privacy_shield = 0;
	l->luminance_domain = NULL;
	l->need_output_flush = true;

	// apply layer rules
	for (ji = 0; ji < config.layer_rules_count; ji++) {
		if (config.layer_rules_count < 1)
			break;
		if (regex_match(config.layer_rules[ji].layer_name,
						l->layer_surface->namespace)) {

			r = &config.layer_rules[ji];
			APPLY_INT_PROP(l, r, noblur);
			APPLY_INT_PROP(l, r, forceblur);
			APPLY_INT_PROP(l, r, privacy_shield);
			APPLY_INT_PROP(l, r, noanim);
			APPLY_INT_PROP(l, r, noshadow);
			APPLY_INT_PROP(l, r, forceshadow);
			APPLY_STRING_PROP(l, r, animation_type_open);
			APPLY_STRING_PROP(l, r, animation_type_close);
			APPLY_STRING_PROP(l, r, luminance_domain);
		}
	}

	l->shield =
		wlr_scene_rect_create(l->scene, 0, 0, (float[4]){0, 0, 0, 0xff});
	l->shield->node.data = l;
	wlr_scene_node_lower_to_bottom(&l->shield->node);
	wlr_scene_node_set_enabled(&l->shield->node, false);

	// initialize the shadow: non-reserving surfaces (launchers etc.), plus
	// layers opted in by rule (forceshadow -- e.g. exclusive_zone -1 popups)
	if ((layer_surface->current.exclusive_zone == 0 || l->forceshadow) &&
		!l->noshadow &&
		layer_surface->current.layer != ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM &&
		layer_surface->current.layer != ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND) {
		l->shadow =
			wlr_scene_shadow_create(l->scene, 0, 0, config.border_radius,
									config.shadows_blur, config.shadowscolor);
		wlr_scene_node_lower_to_bottom(&l->shadow->node);
		wlr_scene_node_set_enabled(&l->shadow->node, true);
	}

	// initialize the animation
	if (config.animations && config.layer_animations && !l->noanim) {
		l->animation.duration = config.animation_duration_open;
		l->animation.action = OPEN;
		layer_set_pending_state(l);
	}
	// re-arrange the layout so windows react to the exclude_zone change and exclusive surfaces get set
	arrangelayers(l->mon);
	reset_exclusive_layers_focus(l->mon);
}

void commitlayersurfacenotify(struct wl_listener *listener, void *data) {
	LayerSurface *l = wl_container_of(listener, l, surface_commit);
	struct wlr_layer_surface_v1 *layer_surface = l->layer_surface;
	struct wlr_scene_tree *scene_layer =
		layers[layermap[layer_surface->current.layer]];
	struct wlr_layer_surface_v1_state old_state;
	struct wlr_box box;

	if (l->layer_surface->initial_commit) {
		client_set_scale(layer_surface->surface, l->mon->wlr_output->scale);

		/* Temporarily set the layer's current state to pending
		 * so that we can easily arrange it */
		old_state = l->layer_surface->current;
		l->layer_surface->current = l->layer_surface->pending;
		arrangelayers(l->mon);
		l->layer_surface->current = old_state;
		/* an on-demand-interactive layer gets focus once, on map. Read the
		 * PENDING state: on this initial commit `current` was just restored
		 * to its pre-commit default (NONE), so checking it here made
		 * focus-on-map dead code and on-demand layers (swaync's control
		 * center) never got the keyboard until clicked. */
		if (!exclusive_focus &&
			l->layer_surface->pending.keyboard_interactive ==
				ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND) {
			focuslayer(l);
		}
		return;
	}

	// check whether the surface has a buffer
	// an empty buffer just means hidden, it doesn't change the mapped state
	if (l->mapped && !wlr_surface_has_buffer(layer_surface->surface)) {
		wlr_scene_node_set_enabled(&l->scene->node, false);
		return;
	} else {
		wlr_scene_node_set_enabled(&l->scene->node, true);
	}

	get_layer_target_geometry(l, &box);

	/* layer_draw_frame (the per-frame render path that draws the shadow,
	 * shadow_blur and shield) only runs when need_output_flush is true, and
	 * this was the ONLY place that ever set it back to true post-map -- but
	 * it lived inside the layer_animations-gated branch below. With
	 * layer_animations off (the default, and this compositor's own live
	 * config), a content-fit popup that resizes itself in place (e.g. a
	 * multi-tab settings panel switching to a shorter tab, which commits a
	 * new buffer without ever going through this function's animation path)
	 * never got another draw pass, leaving the shadow/shadow_blur frozen at
	 * whatever size they were the first time -- reproduced headlessly via
	 * contrib/waybar-popup-test.sh (waybar-display's Display -> Wallpaper
	 * tab switch). Flush on every commit for a visible top/overlay layer
	 * regardless of the animation config; only the MOVE-animation trigger
	 * below still needs layer_animations and an actual geometry change. */
	if (l->mapped &&
		layer_surface->current.layer != ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM &&
		layer_surface->current.layer != ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND) {
		l->need_output_flush = true;
	}

	if (config.animations && config.layer_animations && !l->noanim &&
		l->mapped &&
		layer_surface->current.layer != ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM &&
		layer_surface->current.layer != ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND &&
		!wlr_box_equal(&box, &l->geom)) {

		l->geom.x = box.x;
		l->geom.y = box.y;
		l->geom.width = box.width;
		l->geom.height = box.height;
		l->animation.action = MOVE;
		l->animation.duration = config.animation_duration_move;
		layer_set_pending_state(l);
	}

	layer_update_blur(l);

	layer_flush_blur_background(l);

	if (layer_surface->current.committed == 0 &&
		l->mapped == layer_surface->surface->mapped)
		return;
	l->mapped = layer_surface->surface->mapped;

	if (layer_surface->current.committed & WLR_LAYER_SURFACE_V1_STATE_LAYER) {
		if (scene_layer != l->scene->node.parent) {
			wlr_scene_node_reparent(&l->scene->node, scene_layer);
			wl_list_remove(&l->link);
			wl_list_insert(&l->mon->layers[layer_surface->current.layer],
						   &l->link);
			wlr_scene_node_reparent(
				&l->popups->node,
				(layer_surface->current.layer < ZWLR_LAYER_SHELL_V1_LAYER_TOP
					 ? layers[LyrTop]
					 : scene_layer));
		}
	}

	arrangelayers(l->mon);

	if (layer_surface->current.committed &
		WLR_LAYER_SURFACE_V1_STATE_KEYBOARD_INTERACTIVITY) {
		reset_exclusive_layers_focus(l->mon);
	}
}


/*
 * ── M13: RECORD WHAT CADENCE A CLIENT IS COMMITTING AT ────────────────────
 *
 * Called from BOTH commit listeners -- the xdg one and commitx11 -- because
 * there are two and forgetting the second is exactly what F10 was. A video
 * player under XWayland is not a hypothetical.
 *
 * The FIRST commit establishes a timestamp and no interval: an interval needs
 * two commits, and counting the gap between "client mapped" and "first frame"
 * as a cadence would report every client as impossibly slow for its first
 * sample.
 */
static void client_note_commit(Client *c) {
	if (c == NULL || c->iskilling) {
		return;
	}
	uint64_t now = az_pace_now_ns();
	if (c->commit_last_ns != 0 && now > c->commit_last_ns) {
		uint64_t d = now - c->commit_last_ns;
		/* A gap longer than a second is a client that stopped and started --
		 * a paused video, a window uncovered -- not a cadence. Including it
		 * would drag the mean toward a number nothing ever presented at. */
		if (d < 1000000000ull) {
			c->commit_interval_sum_ns += d;
			c->commit_interval_n++;
		}
	}
	c->commit_last_ns = now;
	c->commit_count++;
}

void commitnotify(struct wl_listener *listener, void *data) {
	Client *c = wl_container_of(listener, c, commit);
	struct wlr_box *new_geo;

	if (c->surface.xdg->initial_commit) {
		// xdg client will first enter this before mapnotify
		init_client_properties(c);
		applyrules(c);
		if (c->mon) {
			client_set_scale(client_surface(c), c->mon->wlr_output->scale);
		}
		setmon(c, NULL, 0,
			   true); /* Make sure to reapply rules in mapnotify() */

		uint32_t serial = wlr_xdg_surface_schedule_configure(c->surface.xdg);
		if (serial > 0) {
			c->configure_serial = serial;
		}

		uint32_t wm_caps = WLR_XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN;

		if (!c->ignore_minimize)
			wm_caps |= WLR_XDG_TOPLEVEL_WM_CAPABILITIES_MINIMIZE;

		if (!c->ignore_maximize)
			wm_caps |= WLR_XDG_TOPLEVEL_WM_CAPABILITIES_MAXIMIZE;

		wlr_xdg_toplevel_set_wm_capabilities(c->surface.xdg->toplevel, wm_caps);

		if (c->mon) {
			wlr_xdg_toplevel_set_bounds(c->surface.xdg->toplevel,
										c->mon->w.width - 2 * c->bw,
										c->mon->w.height - 2 * c->bw);
		}

		if (c->decoration)
			requestdecorationmode(&c->set_decoration_mode, c->decoration);
		return;
	}

	/*
	 * wp-cm's set_image_description is DOUBLE-BUFFERED surface state -- it
	 * lands through wlr_surface_synced, which wlroots applies before it emits
	 * events.commit -- so the commit is the first moment the new metadata is
	 * readable, and this is the only hook that sees it. Cheap and gated three
	 * ways; see mon_content_metadata_changed() for why a per-frame call site
	 * does not become a per-frame modeset.
	 *
	 * Before the animation early-outs below deliberately: a client mid-tag
	 * animation is still the thing on screen, and its colour volume is not an
	 * animation property.
	 */
	if (c && !c->iskilling)
		mon_content_metadata_changed(client_surface(c));
	client_note_commit(c);

	if (!c || c->iskilling || c->animation.tagouting || c->animation.tagouted ||
		c->animation.tagining)
		return;

	if (c->configure_serial &&
		c->configure_serial <= c->surface.xdg->current.configure_serial)
		c->configure_serial = 0;

	if (!c->dirty) {
		new_geo = &c->surface.xdg->geometry;
		c->dirty = new_geo->width != c->geom.width - 2 * c->bw ||
				   new_geo->height != c->geom.height - 2 * c->bw ||
				   new_geo->x != 0 || new_geo->y != 0;
	}

	if (c == grabc || !c->dirty)
		return;

	resize(c, c->geom, 0);

	new_geo = &c->surface.xdg->geometry;
	c->dirty = new_geo->width != c->geom.width - 2 * c->bw ||
			   new_geo->height != c->geom.height - 2 * c->bw ||
			   new_geo->x != 0 || new_geo->y != 0;
}

void destroydecoration(struct wl_listener *listener, void *data) {
	Client *c = wl_container_of(listener, c, destroy_decoration);

	wl_list_remove(&c->destroy_decoration.link);
	wl_list_remove(&c->set_decoration_mode.link);
	/* client_wants_ssd() reads c->decoration per frame -- clear the pointer
	 * (wlroots frees the decoration right after this) and re-arrange so the
	 * titlebar/border/reserved space follow the CSD flip immediately */
	c->decoration = NULL;
	if (c->mon && client_surface(c)->mapped)
		arrange(c->mon, false, false);
}

static bool popup_unconstrain(Popup *popup) {
	struct wlr_xdg_popup *wlr_popup = popup->wlr_popup;
	Client *c = NULL;
	LayerSurface *l = NULL;

	if (!wlr_popup || !wlr_popup->parent) {
		return false;
	}

	struct wlr_scene_node *parent_node = wlr_popup->parent->data;
	if (!parent_node) {
		wlr_log(WLR_ERROR, "Popup parent has no scene node");
		return false;
	}

	/* called for the out-params; the returned type is no longer consulted --
	 * see the comment below on branching by pointer instead */
	toplevel_from_wlr_surface(wlr_popup->base->surface, &c, &l);
	/* Resolving to NEITHER is its own case, not covered by the two below:
	 * toplevel_from_wlr_surface returns -1 with both out-params left NULL (a
	 * parent chain ending in ROLE_NONE, a popup whose parent is going away),
	 * and -1 is not LayerShell -- so the ternary would take the c branch and
	 * dereference a NULL c. Both `l && ...` and `c && ...` are false in that
	 * state, so neither guards it. */
	if (!c && !l) {
		return true;
	}
	if ((l && !l->mon) || (c && !c->mon)) {
		return true;
	}

	/* Branch on the POINTER, not on type -- here and in the block below, which
	 * must agree with this line about which of the two it is using.
	 *
	 * They match in the normal case, but type is LayerShell as soon as a layer
	 * surface is found, while l is that surface's ->data, which is NULL until
	 * we attach our own wrapper -- so type can say LayerShell with l still
	 * NULL, and the type form would then dereference it. Combined with the
	 * !c && !l guard above, "l ? l : c" cannot dereference NULL either way. */
	struct wlr_box usable = l ? l->mon->m : c->mon->w;

	int lx, ly;
	struct wlr_box constraint_box;

	if (l) {
		wlr_scene_node_coords(&l->scene_layer->tree->node, &lx, &ly);
		constraint_box.x = usable.x - lx;
		constraint_box.y = usable.y - ly;
		constraint_box.width = usable.width;
		constraint_box.height = usable.height;
	} else {
		constraint_box.x =
			usable.x - (c->geom.x + c->bw - c->surface.xdg->current.geometry.x);
		constraint_box.y =
			usable.y - (c->geom.y + c->bw - c->surface.xdg->current.geometry.y);
		constraint_box.width = usable.width;
		constraint_box.height = usable.height;
	}

	wlr_xdg_popup_unconstrain_from_box(wlr_popup, &constraint_box);
	return false;
}

static void destroypopup(struct wl_listener *listener, void *data) {
	Popup *popup = wl_container_of(listener, popup, destroy);
	wl_list_remove(&popup->destroy.link);
	wl_list_remove(&popup->reposition.link);
	if (popup->watching_surface)
		wl_list_remove(&popup->surface_commit.link);
	/* the blur node belongs to the popup's scene tree, which wlroots tears
	 * down with the surface -- only the pointer is ours to drop */
	popup->blur_node = NULL;
	if (popup->wlr_popup && popup->wlr_popup->base)
		popup->wlr_popup->base->data = NULL;
	free(popup);
}

/* Every commit, for as long as the popup lives: the region and the size both
 * change under us -- a menu that grows a submenu recommits at a new size, and
 * the client can set a new blur region at any time. */
static void popup_handle_surface_commit(struct wl_listener *listener,
										void *data) {
	Popup *popup = wl_container_of(listener, popup, surface_commit);
	popup_update_blur(popup);
}

static void commitpopup(struct wl_listener *listener, void *data) {
	Popup *popup = wl_container_of(listener, popup, commit);

	struct wlr_surface *surface = data;
	bool should_destroy = false;
	struct wlr_xdg_popup *wlr_popup =
		wlr_xdg_popup_try_from_wlr_surface(surface);

	if (!wlr_popup->base->initial_commit)
		return;

	if (!wlr_popup->parent || !wlr_popup->parent->data) {
		should_destroy = true;
		goto cleanup_popup_commit;
	}

	wlr_scene_node_raise_to_top(wlr_popup->parent->data);

	wlr_popup->base->surface->data =
		wlr_scene_xdg_surface_create(wlr_popup->parent->data, wlr_popup->base);

	popup->wlr_popup = wlr_popup;
	/* so a wl_surface can be traced back here; nothing else uses base->data */
	wlr_popup->base->data = popup;

	if (!popup->watching_surface) {
		popup->surface_commit.notify = popup_handle_surface_commit;
		wl_signal_add(&wlr_popup->base->surface->events.commit,
					  &popup->surface_commit);
		popup->watching_surface = true;
	}
	popup_update_blur(popup);

	should_destroy = popup_unconstrain(popup);

cleanup_popup_commit:

	wl_list_remove(&popup->commit.link);
	popup->commit.notify = NULL;

	if (should_destroy) {
		wlr_xdg_popup_destroy(wlr_popup);
	}
}

static void repositionpopup(struct wl_listener *listener, void *data) {
	Popup *popup = wl_container_of(listener, popup, reposition);
	(void)popup_unconstrain(popup);
}

static void createpopup(struct wl_listener *listener, void *data) {
	struct wlr_xdg_popup *wlr_popup = data;

	Popup *popup = calloc(1, sizeof(Popup));
	if (!popup)
		return;

	popup->type = XdgPopup;

	popup->destroy.notify = destroypopup;
	wl_signal_add(&wlr_popup->events.destroy, &popup->destroy);

	popup->commit.notify = commitpopup;
	wl_signal_add(&wlr_popup->base->surface->events.commit, &popup->commit);

	popup->reposition.notify = repositionpopup;
	wl_signal_add(&wlr_popup->events.reposition, &popup->reposition);
}

/* xdg-dialog-v1: clients declare a toplevel as a (modal) dialog. Modality
 * is queried on demand via wlr_xdg_dialog_v1_try_from_wlr_xdg_toplevel()
 * (client_is_modal); this wrapper only exists to react to set_modal
 * commits, which can arrive after map and must re-run the float/titlebar
 * classification. */
typedef struct {
	struct wlr_xdg_dialog_v1 *dialog;
	struct wl_listener set_modal;
	struct wl_listener destroy;
} XdgDialog;

static void xdgdialogsetmodal(struct wl_listener *listener, void *data) {
	XdgDialog *d = wl_container_of(listener, d, set_modal);
	Client *c = d->dialog->xdg_toplevel->base->data;
	if (!c || !c->mon || !client_surface(c)->mapped)
		return;
	if (d->dialog->modal && !c->isfloating)
		setfloating(c, 1);
	else
		arrange(c->mon, false, false);
}

static void xdgdialogdestroy(struct wl_listener *listener, void *data) {
	XdgDialog *d = wl_container_of(listener, d, destroy);
	wl_list_remove(&d->set_modal.link);
	wl_list_remove(&d->destroy.link);
	free(d);
}

void createdialog(struct wl_listener *listener, void *data) {
	struct wlr_xdg_dialog_v1 *dialog = data;

	XdgDialog *d = calloc(1, sizeof(*d));
	if (!d)
		return;

	d->dialog = dialog;
	d->set_modal.notify = xdgdialogsetmodal;
	wl_signal_add(&dialog->events.set_modal, &d->set_modal);
	d->destroy.notify = xdgdialogdestroy;
	wl_signal_add(&dialog->events.destroy, &d->destroy);
}

void createdecoration(struct wl_listener *listener, void *data) {
	struct wlr_xdg_toplevel_decoration_v1 *deco = data;
	Client *c = deco->toplevel->base->data;
	c->decoration = deco;

	LISTEN(&deco->events.request_mode, &c->set_decoration_mode,
		   requestdecorationmode);
	LISTEN(&deco->events.destroy, &c->destroy_decoration, destroydecoration);

	/* a client may bind xdg-decoration after mapping: client_wants_ssd just
	 * flipped, and requestdecorationmode() below re-arranges when mapped so
	 * the titlebar/border/reserved space follow immediately */
	requestdecorationmode(&c->set_decoration_mode, deco);
}

void createidleinhibitor(struct wl_listener *listener, void *data) {
	struct wlr_idle_inhibitor_v1 *idle_inhibitor = data;
	LISTEN_STATIC(&idle_inhibitor->events.destroy, destroyidleinhibitor);

	checkidleinhibitor(NULL);
}

void createkeyboard(struct wlr_keyboard *keyboard) {

	struct libinput_device *device = NULL;

	if (wlr_input_device_is_libinput(&keyboard->base) &&
		(device = wlr_libinput_get_device_handle(&keyboard->base))) {

		InputDevice *input_dev = calloc(1, sizeof(InputDevice));
		input_dev->wlr_device = &keyboard->base;
		input_dev->libinput_device = device;
		input_dev->device_data = keyboard;

		input_dev->destroy_listener.notify = destroyinputdevice;
		wl_signal_add(&keyboard->base.events.destroy,
					  &input_dev->destroy_listener);

		wl_list_insert(&inputdevices, &input_dev->link);
	}

	/* Set the keymap to match the group keymap */
	wlr_keyboard_set_keymap(keyboard, kb_group->wlr_group->keyboard.keymap);

	wlr_keyboard_notify_modifiers(keyboard, 0, 0, locked_mods, 0);

	/* Add the new keyboard to the group */
	wlr_keyboard_group_add_keyboard(kb_group->wlr_group, keyboard);
}

KeyboardGroup *createkeyboardgroup(void) {
	KeyboardGroup *group = ecalloc(1, sizeof(*group));
	struct xkb_context *context;
	struct xkb_keymap *keymap;

	group->wlr_group = wlr_keyboard_group_create();
	group->wlr_group->data = group;

	context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (!(keymap = xkb_keymap_new_from_names(context, &config.xkb_rules,
											 XKB_KEYMAP_COMPILE_NO_FLAGS)))
		die("failed to compile keymap");

	wlr_keyboard_set_keymap(&group->wlr_group->keyboard, keymap);

	if (config.numlockon) {
		xkb_mod_index_t mod_index =
			xkb_keymap_mod_get_index(keymap, XKB_MOD_NAME_NUM);
		if (mod_index != XKB_MOD_INVALID)
			locked_mods |= (uint32_t)1 << mod_index;
	}

	if (config.capslock) {
		xkb_mod_index_t mod_index =
			xkb_keymap_mod_get_index(keymap, XKB_MOD_NAME_CAPS);
		if (mod_index != XKB_MOD_INVALID)
			locked_mods |= (uint32_t)1 << mod_index;
	}

	if (locked_mods)
		wlr_keyboard_notify_modifiers(&group->wlr_group->keyboard, 0, 0,
									  locked_mods, 0);

	xkb_keymap_unref(keymap);
	xkb_context_unref(context);

	wlr_keyboard_set_repeat_info(&group->wlr_group->keyboard,
								 config.repeat_rate, config.repeat_delay);

	/* Set up listeners for keyboard events */
	LISTEN(&group->wlr_group->keyboard.events.key, &group->key, keypress);
	LISTEN(&group->wlr_group->keyboard.events.modifiers, &group->modifiers,
		   keypressmod);

	group->key_repeat_source =
		wl_event_loop_add_timer(event_loop, keyrepeat, group);

	/* A seat can only have one keyboard, but this is a limitation of the
	 * Wayland protocol - not wlroots. We assign all connected keyboards to the
	 * same wlr_keyboard_group, which provides a single wlr_keyboard interface
	 * for all of them. Set this combined wlr_keyboard as the seat keyboard.
	 */
	wlr_seat_set_keyboard(seat, &group->wlr_group->keyboard);
	return group;
}

void createlayersurface(struct wl_listener *listener, void *data) {
	struct wlr_layer_surface_v1 *layer_surface = data;
	LayerSurface *l = NULL;
	struct wlr_surface *surface = layer_surface->surface;
	struct wlr_scene_tree *scene_layer =
		layers[layermap[layer_surface->pending.layer]];

	if (!layer_surface->output &&
		!(layer_surface->output = selmon ? selmon->wlr_output : NULL)) {
		wlr_layer_surface_v1_destroy(layer_surface);
		return;
	}

	l = layer_surface->data = ecalloc(1, sizeof(*l));
	l->type = LayerShell;
	LISTEN(&surface->events.map, &l->map, maplayersurfacenotify);
	LISTEN(&surface->events.commit, &l->surface_commit,
		   commitlayersurfacenotify);
	LISTEN(&surface->events.unmap, &l->unmap, unmaplayersurfacenotify);

	l->layer_surface = layer_surface;
	l->mon = layer_surface->output->data;
	l->scene_layer =
		wlr_scene_layer_surface_v1_create(scene_layer, layer_surface);
	l->scene = l->scene_layer->tree;
	l->popups = surface->data = wlr_scene_tree_create(
		layer_surface->current.layer < ZWLR_LAYER_SHELL_V1_LAYER_TOP
			? layers[LyrTop]
			: scene_layer);
	l->scene->node.data = l->popups->node.data = l;

	LISTEN(&l->scene->node.events.destroy, &l->destroy, destroylayernodenotify);

	wl_list_insert(&l->mon->layers[layer_surface->pending.layer], &l->link);
}

void createlocksurface(struct wl_listener *listener, void *data) {
	SessionLock *lock = wl_container_of(listener, lock, new_surface);
	struct wlr_session_lock_surface_v1 *lock_surface = data;
	Monitor *m = lock_surface->output->data;
	struct wlr_scene_tree *scene_tree = lock_surface->surface->data =
		wlr_scene_subsurface_tree_create(lock->scene, lock_surface->surface);
	m->lock_surface = lock_surface;

	wlr_scene_node_set_position(&scene_tree->node, m->m.x, m->m.y);
	wlr_session_lock_surface_v1_configure(lock_surface, m->m.width,
										  m->m.height);

	LISTEN(&lock_surface->events.destroy, &m->destroy_lock_surface,
		   destroylocksurface);

	if (m == selmon)
		client_notify_enter(lock_surface->surface, wlr_seat_get_keyboard(seat));
}

struct wlr_output_mode *get_nearest_output_mode(struct wlr_output *output,
												int32_t width, int32_t height,
												float refresh) {
	struct wlr_output_mode *mode, *nearest_mode = NULL;
	float min_diff = 99999.0f;

	wl_list_for_each(mode, &output->modes, link) {
		if (mode->width == width && mode->height == height) {
			float mode_refresh = mode->refresh / 1000.0f;
			float diff = fabsf(mode_refresh - refresh);

			if (diff < min_diff) {
				min_diff = diff;
				nearest_mode = mode;
			}
		}
	}

	return nearest_mode;
}

void enable_adaptive_sync(Monitor *m, struct wlr_output_state *state) {
	wlr_output_state_set_adaptive_sync_enabled(state, true);
	if (!wlr_output_test_state(m->wlr_output, state)) {
		wlr_output_state_set_adaptive_sync_enabled(state, false);
		wlr_log(WLR_DEBUG, "failed to enable adaptive sync for output %s",
				m->wlr_output->name);
	} else {
		m->is_vrr_opening = true;
		wlr_log(WLR_INFO, "adaptive sync enabled for output %s",
				m->wlr_output->name);
	}
}

void disable_adaptive_sync(Monitor *m, struct wlr_output_state *state) {
	wlr_output_state_set_adaptive_sync_enabled(state, false);
	m->is_vrr_opening = false;
}

bool monitor_matches_rule(Monitor *m, const ConfigMonitorRule *rule) {
	if (rule->name != NULL && !regex_match(rule->name, m->wlr_output->name))
		return false;
	if (rule->make != NULL && (m->wlr_output->make == NULL ||
							   strcmp(rule->make, m->wlr_output->make) != 0))
		return false;
	if (rule->model != NULL && (m->wlr_output->model == NULL ||
								strcmp(rule->model, m->wlr_output->model) != 0))
		return false;
	if (rule->serial != NULL &&
		(m->wlr_output->serial == NULL ||
		 strcmp(rule->serial, m->wlr_output->serial) != 0))
		return false;
	return true;
}

/* Fold every matching monitorrule into one effective rule. Rules apply in
 * config order and later rules override earlier ones per option, so a
 * partial rule (e.g. hdr-only, kept in a user-owned file) composes with a
 * generated one (e.g. from DMS) matching the same output. Returns whether
 * any rule matched. */
bool monitor_merge_rules(Monitor *m, ConfigMonitorRule *out) {
	const ConfigMonitorRule *r;
	int32_t ji;
	bool matched = false;

	memset(out, 0, sizeof(*out));
	out->x = out->y = INT32_MAX;
	out->width = out->height = -1;
	out->refresh = 0.0f;
	out->scale = 0.0f;
	out->rr = -1;
	out->vrr = -1;
	out->custom = -1;
	out->hdr = -1;
	out->disable = -1;
	out->bitdepth = -1;
	out->hdr_max_luminance = -1.0f;
	out->hdr_min_luminance = -1.0f;
	out->hdr_max_fall = -1.0f;
	out->icc_profile = NULL;

	for (ji = 0; ji < config.monitor_rules_count; ji++) {
		r = &config.monitor_rules[ji];
		if (!monitor_matches_rule(m, r))
			continue;
		matched = true;
		if (r->width > 0)
			out->width = r->width;
		if (r->height > 0)
			out->height = r->height;
		if (r->refresh > 0)
			out->refresh = r->refresh;
		if (r->x != INT32_MAX)
			out->x = r->x;
		if (r->y != INT32_MAX)
			out->y = r->y;
		if (r->scale > 0)
			out->scale = r->scale;
		if (r->rr >= 0)
			out->rr = r->rr;
		if (r->vrr >= 0)
			out->vrr = r->vrr;
		if (r->custom >= 0)
			out->custom = r->custom;
		if (r->hdr >= 0)
			out->hdr = r->hdr;
		if (r->disable >= 0)
			out->disable = r->disable;
		if (r->bitdepth >= 0)
			out->bitdepth = r->bitdepth;
		if (r->hdr_max_luminance >= 0)
			out->hdr_max_luminance = r->hdr_max_luminance;
		if (r->hdr_min_luminance >= 0)
			out->hdr_min_luminance = r->hdr_min_luminance;
		if (r->hdr_max_fall >= 0)
			out->hdr_max_fall = r->hdr_max_fall;
		if (r->icc_profile)
			out->icc_profile = r->icc_profile;
	}
	return matched;
}

/* apply the display parameters from the rule to wlr_output_state, returning whether a custom mode was set */
bool apply_rule_to_state(Monitor *m, const ConfigMonitorRule *rule,
						 struct wlr_output_state *state, int vrr, int custom) {
	bool mode_set = false;
	if (rule->width > 0 && rule->height > 0 && rule->refresh > 0) {
		struct wlr_output_mode *internal_mode = get_nearest_output_mode(
			m->wlr_output, rule->width, rule->height, rule->refresh);
		if (internal_mode) {
			/* Only modeset on an actual change. reapply_monitor_rules runs on
			 * every reload_config (a matugen wallpaper change re-applies the
			 * unchanged config), and an unconditional modeset re-commit
			 * transients the shared PCIe/power enough to drop the S/PDIF DAC's
			 * lock — sound cutting out on every reload. A same-mode commit is
			 * a no-op here, so skip it. */
			if (internal_mode != m->wlr_output->current_mode) {
				wlr_output_state_set_mode(state, internal_mode);
				mode_set = true;
			}
		} else if (custom || wlr_output_is_headless(m->wlr_output) ||
				   wl_list_empty(&m->wlr_output->modes)) {
			/* An output with no mode list at all can ONLY take a custom mode
			 * -- that is every virtual backend, not just headless: the nested
			 * Wayland backend and X11 backend both present a window with no
			 * enumerable modes. Restricting this to headless meant a nested
			 * session fell through to the preferred-mode path below, which
			 * hands set_mode() a NULL, fails the commit, and leaves the
			 * output disabled. See the comment there for what that then
			 * cost. */
			wlr_output_state_set_custom_mode(
				state, rule->width, rule->height,
				(int32_t)roundf(rule->refresh * 1000));
			mode_set = true;
		}
	}
	m->vrr_global_enable = vrr;
	if (vrr) {
		enable_adaptive_sync(m, state);
	} else {
		disable_adaptive_sync(m, state);
	}
	if (rule->scale > 0)
		wlr_output_state_set_scale(state, rule->scale);
	if (rule->rr >= 0)
		wlr_output_state_set_transform(state, rule->rr);
	return mode_set;
}

/* Load an ICC profile as the output's color transform. It is applied by
 * the renderer in SDR mode only: with HDR active the BT.2020/PQ image
 * description drives the pipeline instead. */
/*
 * M6C. az_icc_clut_build's evaluator, and the only wlroots in the cLUT path.
 *
 * wlr_color_transform_eval maps LINEAR sRGB-primaries values to the profile's
 * DEVICE encoding -- wlroots builds the lcms2 transform from a source profile
 * with gamma-1.0 TRCs (render/color_lcms2.c), which is what makes the input
 * axis linear. That is the domain contract az_icc.h states and the encode pass
 * relies on; it is stated in three places on purpose, because it is the one
 * thing that cannot be noticed by looking at the picture.
 */
static void mon_icc_clut_eval(void *user, const float in[3], float out[3]) {
	wlr_color_transform_eval(user, out, in);
}

void mon_load_icc_profile(Monitor *m, const char *path) {
	char *data;
	long size;
	FILE *f;
	struct wlr_color_transform *transform;

	if (!path || !*path) {
		if (m->icc_transform) {
			wlr_color_transform_unref(m->icc_transform);
			m->icc_transform = NULL;
			m->icc_path[0] = '\0';
			/* The shaper goes with it, and the serial moves so the renderer
			 * cannot keep encoding through a curve for a profile that is no
			 * longer configured. */
			m->icc_shaper_ok = false;
			m->icc_reject = AZ_ICC_OK;
			az_icc_clut_free(m->icc_clut);
			m->icc_clut = NULL;
			m->icc_serial++;
		}
		return;
	}
	if (strcmp(m->icc_path, path) == 0)
		return;

	f = fopen(path, "rb");
	if (!f) {
		wlr_log(WLR_ERROR, "cannot open ICC profile %s", path);
		return;
	}
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (size <= 0 || size > 16 * 1024 * 1024) {
		fclose(f);
		return;
	}
	data = ecalloc(1, size);
	if (fread(data, 1, size, f) != (size_t)size) {
		fclose(f);
		free(data);
		return;
	}
	fclose(f);

	transform = wlr_color_transform_init_linear_to_icc(data, size);
	if (!transform) {
		wlr_log(WLR_ERROR, "failed to parse ICC profile %s", path);
		free(data);
		return;
	}
	/*
	 * ── M6B/G2: THE SAME BYTES, REDUCED FOR AVK ───────────────────────────
	 *
	 * Read here rather than at derive time because this is the only place the
	 * file's CONTENTS exist -- the wlroots transform is opaque and cannot be
	 * asked what matrix it holds. A rejection is not a failure of the load:
	 * the wlroots transform stays, C3 keeps FALLBACK for the output, and
	 * SceneFX applies the profile exactly as it does today. What is refused is
	 * AVK's shortcut, not the operator's calibration.
	 */
	struct az_icc_shaper shaper;
	enum az_icc_reject rc = az_icc_load_shaper(data, (size_t)size, true,
		&shaper);
	free(data);
	if (m->icc_transform)
		wlr_color_transform_unref(m->icc_transform);
	m->icc_transform = transform;
	m->icc_reject = rc;
	m->icc_shaper_ok = rc == AZ_ICC_OK;
	if (m->icc_shaper_ok)
		m->icc_shaper = shaper;
	/*
	 * ── M6C: THE CUBE, ONLY WHEN THE REDUCTION FAILED ─────────────────────
	 *
	 * Built here, once per load, and NOT for a profile that reduced. The
	 * matrix-shaper form is measured, cheaper and better resolved (az_icc.h),
	 * so building both would be 1.6MB and 274625 lcms2 evaluations spent to have
	 * a second answer nothing would consult.
	 *
	 * 35937 evaluations is tens of milliseconds. It happens on a monitor rule
	 * change and on nothing else -- the same event that reallocates swapchains
	 * and re-derives every output's colour state.
	 */
	az_icc_clut_free(m->icc_clut);
	m->icc_clut = NULL;
	if (!m->icc_shaper_ok)
		m->icc_clut = az_icc_clut_build(AZ_ICC_CLUT_DIM, mon_icc_clut_eval,
				transform);
	m->icc_serial++;
	snprintf(m->icc_path, sizeof(m->icc_path), "%s", path);
	wlr_log(WLR_INFO, "loaded ICC profile %s for output %s "
			"(AVK carries it as %s; reduction: %s)",
			path, m->wlr_output->name,
			m->icc_shaper_ok ? "a matrix and a curve"
			: m->icc_clut ? "a 3D table" : "NOTHING -- the output is refused",
			az_icc_reject_name(rc));
}

/* The sole fullscreen client visible on m, if there's exactly one -- used to
 * forward that client's own declared HDR10 static metadata (frog-color-
 * management-v1 / wp-color-management) to the real display instead of always
 * describing the panel's own ceiling. NULL when there's more than one (or
 * zero) fullscreen candidates, since mixing another window's content into a
 * single-surface metadata guess would be worse than the panel-derived
 * default. */
static Client *mon_hdr_scanout_candidate(Monitor *m) {
	Client *candidate = NULL, *fc;
	wl_list_for_each(fc, &clients, link) {
		if (!fc->isfullscreen || fc->isminimized || fc->iskilling ||
			!VISIBLEON(fc, m))
			continue;
		if (candidate)
			return NULL;
		candidate = fc;
	}
	return candidate;
}

/*
 * ── THE HDR10 STATIC METADATA THIS OUTPUT WILL PRESENT ────────────────────
 *
 * The panel's own rule, overridden by the sole fullscreen client's declared
 * metadata when it has some -- that is what the content was actually graded
 * for, and a tone-mapper can use it more accurately than a value derived from
 * the display's ceiling alone.
 *
 * Extracted from mon_state_apply_color() so that "what would we fold in" can be
 * asked WITHOUT building an output state and committing it. The identity it
 * returns is over exactly the four values that reach the connector, so a
 * comparison against m->content_metadata_identity answers "has the picture
 * changed" and nothing wider: a repaint, a damage event, or a commit that
 * restates the same metadata all hash equal.
 *
 * ── real wp-color-management FIRST, frog ONLY AS A FALLBACK ──────────────
 *
 * This mirrors the scene's own precedence. az_cm_surface_description() is the
 * same one slot the scene consults, so the scanout path and the composited path
 * cannot disagree about what a surface is. NOT wlroots'
 * wlr_surface_get_image_description_v1_data(): that reads the addon wlroots'
 * own wp-cm implementation attaches, and under native ownership that
 * implementation does not exist, so it returns NULL forever.
 */
static uint64_t mon_content_metadata(Monitor *m, double *out_min,
		double *out_max, double *out_cll, double *out_fall) {
	double mastering_min = m->hdr_min_luminance;
	double mastering_max = m->hdr_max_luminance;
	double max_cll = m->hdr_max_luminance;
	double max_fall = m->hdr_max_fall;

	Client *candidate = mon_hdr_scanout_candidate(m);
	struct wlr_surface *candidate_surface =
		candidate ? client_surface(candidate) : NULL;
	const struct wlr_image_description_v1_data *content_desc =
		candidate_surface ? az_cm_surface_description(candidate_surface) : NULL;
	if (content_desc && content_desc->tf_named ==
			WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ) {
		if (content_desc->has_mastering_luminance) {
			mastering_min = content_desc->mastering_luminance.min;
			mastering_max = content_desc->mastering_luminance.max;
		}
		if (content_desc->max_cll > 0)
			max_cll = content_desc->max_cll;
		if (content_desc->max_fall > 0)
			max_fall = content_desc->max_fall;
	}
	*out_min = mastering_min;
	*out_max = mastering_max;
	*out_cll = max_cll;
	*out_fall = max_fall;

	/* FNV-1a over the four, the same construction az_preferred uses and for
	 * the same reason: the values are floats that a struct comparison would
	 * have to spell out field by field, and this is the shape already proven
	 * here. Never 0 -- that is the "never computed" value on a fresh Monitor,
	 * and it must not collide with a real answer. */
	uint64_t h = 1469598103934665603ULL;
	az_preferred_mix(&h, out_min, sizeof(*out_min));
	az_preferred_mix(&h, out_max, sizeof(*out_max));
	az_preferred_mix(&h, out_cll, sizeof(*out_cll));
	az_preferred_mix(&h, out_fall, sizeof(*out_fall));
	return h != 0 ? h : 1;
}

/*
 * ── A CLIENT CHANGED ITS CONTENT METADATA. DOES THE CONNECTOR CARE? ───────
 *
 * mon_state_apply_color() folds a fullscreen client's declared HDR10 metadata
 * into the connector, but the only two writers of hdr_pending_change were a
 * fullscreen TRANSITION and hdr_resolve(). Neither fires when a client that is
 * already fullscreen changes the numbers -- mpv advancing to the next file in a
 * playlist without leaving fullscreen -- so the connector kept describing the
 * previous title's mastering values, and no wp-cm output client was told
 * anything either.
 *
 * ── AND THIS IS WHY IT IS NOT A MODESET STORM ────────────────────────────
 *
 * The flag is folded in with allow_reconfiguration, which in this wlroots means
 * .modeset = true: a BLOCKING full modeset whether or not the mode changed. The
 * comment on client_pending_fullscreen_state() records what happens when that
 * is armed carelessly -- 58 of them in one session, in bursts of eight in 1.3
 * seconds, with libinput complaining of 42-51ms of lag inside the densest
 * burst. Content metadata arrives on EVERY COMMIT of a client that sets it,
 * which is every frame of a video, so arming unconditionally here would be
 * strictly worse than the defect it fixes.
 *
 * Three gates, cheapest first:
 *   1. the output is not in HDR              -- nothing folds content metadata
 *   2. this surface is not the scanout candidate for its own output
 *   3. the four values hash to what the connector already carries
 *
 * (3) is the load-bearing one and it is why this is an identity rather than a
 * boolean: a client committing a thousand frames with unchanged metadata
 * reaches it a thousand times and arms zero modesets, and that is a property
 * that can be read off the counter rather than argued about.
 */
static void mon_content_metadata_changed(struct wlr_surface *surface) {
	if (surface == NULL) {
		return;
	}
	Monitor *m = az_surface_effective_output(surface);
	if (m == NULL || m->wlr_output == NULL || !m->hdr) {
		return;
	}
	/* OUTPUT-SCOPED. A background window changing its mastering metadata must
	 * not touch the connector -- only what is actually being scanned out can
	 * describe what the display is showing. */
	Client *candidate = mon_hdr_scanout_candidate(m);
	if (candidate == NULL || client_surface(candidate) != surface) {
		return;
	}
	double min, max, cll, fall;
	uint64_t identity = mon_content_metadata(m, &min, &max, &cll, &fall);
	if (identity == m->content_metadata_identity) {
		return;
	}
	m->hdr_pending_change = true;
	az_content_metadata_arms++;
	wlr_output_schedule_frame(m->wlr_output);
}

/* Apply the color pipeline of an output: an optional 10-bit framebuffer
 * (bitdepth:10 rule, implied by HDR to avoid PQ banding) and an optional
 * HDR mode (BT.2020 primaries with the PQ transfer function). Falls back
 * gracefully when the output or backend refuses. */
/*
 * ── M5/C3: DERIVE THE OUTPUT'S COLOUR STATE ───────────────────────────────
 *
 * SEPARATE FROM mon_state_apply_color(), AND CALLED FOR EVERY OUTPUT.
 *
 * That function is skipped for headless/virtual outputs because COMMITTING
 * HDR and colour state to something with no real connector can fail the commit
 * or crash. Deriving a struct cannot. Folding the derivation in there left
 * every virtual output with a zero-initialised colour state -- which reads as
 * Path A with ref_nits 0 and peak_scene 0, a poison value that a consumer
 * would divide by -- and, incidentally, made the whole thing invisible to the
 * headless fixtures, which is how it was found.
 *
 * Called at the END of a state build, because everything before it can still
 * change the answer: `m->hdr` may have just been refused, and the render
 * format may have just fallen back from 10-bit to 8-bit.
 *
 * NOTHING RENDERS FROM THIS YET. It is derived and logged so the model can be
 * checked against real outputs -- including live HDR transitions -- before any
 * pixel depends on it being right.
 */
static void mon_derive_color_state(Monitor *m,
		const struct wlr_output_state *state) {
	struct wlr_output *wlr_output = m->wlr_output;
	/*
	 * The format comes from the STATE when the state carries one and from the
	 * live output otherwise: mon_state_apply_color() only sets it when it
	 * DIFFERS, so an unchanged 10-bit output has no format in its state and
	 * would otherwise be derived as 8-bit.
	 */
	uint32_t fmt = (state != NULL
			&& (state->committed & WLR_OUTPUT_STATE_RENDER_FORMAT))
		? state->render_format : wlr_output->render_format;
	struct az_output_desc desc = {
		.bits_per_channel = (fmt == DRM_FORMAT_XRGB2101010
			|| fmt == DRM_FORMAT_ARGB2101010) ? 10 : 8,
		.hdr = m->hdr != 0,
		.has_icc = m->icc_transform != NULL,
		.hdr_max_nits = m->hdr_max_luminance,
		.scene_ref_nits = config.sdr_reference_luminance,
		.sdr_saturation = config.sdr_saturation,
		/* Per FORMAT, not per modifier: a modifier belongs to a swapchain
		 * buffer and is not known here, and F11 established the answer does
		 * not vary by modifier. False whenever AVK is not the renderer, which
		 * is correct -- Path A is an AVK path. */
		.scanout_srgb_view_ok = az_avk_scanout_srgb_format_ok(fmt),
		/*
		 * ── M6B/G2: OFFERED ONLY WHEN AVK CAN ACTUALLY APPLY IT ───────────
		 *
		 * A shaper here makes C3 choose LUT1D, which takes the profile away
		 * from SceneFX (az_output_color_transform) on the promise that the AVK
		 * encode pass will apply it instead. Offer it to an output AVK is not
		 * driving and the profile is applied by nobody -- an uncalibrated
		 * picture on a display the operator measured, arrived at by a change
		 * meant to honour the measurement.
		 *
		 * So both halves of the promise are checked HERE, where the answer is
		 * known, rather than assumed by the pure table: AVK is the renderer,
		 * and the encode pass it needs is enabled in this session.
		 */
		.icc_shaper = (m->icc_shaper_ok && az_avk_is_active()
				&& az_avk_encode_pass_enabled(NULL))
			? &m->icc_shaper : NULL,
		/*
		 * M6C, gated on exactly the same two conditions and for exactly the
		 * same reason: offering a table only AVK can sample to an output AVK is
		 * not driving would take the profile away from the renderer that was
		 * applying it and hand it to nobody.
		 */
		.icc_clut = (m->icc_clut != NULL && az_avk_is_active()
				&& az_avk_encode_pass_enabled(NULL))
			? m->icc_clut : NULL,
	};
	struct az_output_color_state prev = m->color_state;
	m->color_state = az_output_color_derive(&desc);
	if (prev.path != m->color_state.path
			|| prev.encode_tf != m->color_state.encode_tf
			|| prev.ref_nits != m->color_state.ref_nits
			|| prev.peak_scene != m->color_state.peak_scene) {
		wlr_log(WLR_INFO, "M5 color: %s path=%s tf=%s %dbpc ref=%.0f "
			"peak_scene=%.3f dither_q=%.5f srgb_view=%d",
			wlr_output->name,
			az_output_path_name(m->color_state.path),
			az_tf_name(m->color_state.encode_tf),
			desc.bits_per_channel, m->color_state.ref_nits,
			m->color_state.peak_scene, m->color_state.dither_q,
			(int)desc.scanout_srgb_view_ok);
	}
}

void mon_state_apply_color(Monitor *m, struct wlr_output_state *state) {
	struct wlr_output *wlr_output = m->wlr_output;

	if (m->hdr) {
		if (!drw->features.output_color_transform) {
			wlr_log(WLR_ERROR,
					"renderer cannot apply output color transforms, "
					"refusing HDR (sRGB content would be shown unconverted)");
			m->hdr = 0;
			m->hdr_capability_failed = true;
		} else if (!(wlr_output->supported_primaries &
					 WLR_COLOR_NAMED_PRIMARIES_BT2020) ||
				   !(wlr_output->supported_transfer_functions &
					 WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ)) {
			wlr_log(WLR_ERROR, "output %s does not support HDR (BT.2020 + PQ)",
					wlr_output->name);
			m->hdr = 0;
			m->hdr_capability_failed = true;
		} else {
			/* default: the display's real luminance capabilities, so the
			 * panel doesn't tone-map for 10000-nit content it will never
			 * receive. Overridden below with the actual title's own
			 * mastering/max_cll/max_fall when the sole fullscreen client on
			 * this monitor declares some -- that's what the content was
			 * actually graded for, which the panel's tone-mapper can use
			 * more accurately than a value derived purely from its own
			 * ceiling. */
			double mastering_min, mastering_max, max_cll, max_fall;
			/*
			 * THE SAME RESOLVER mon_content_metadata_changed() ASKS. Sharing
			 * it is what makes the arming decision honest: the identity it
			 * compares against is recorded here, from the values that are
			 * about to be handed to the connector, so "unchanged" cannot mean
			 * "unchanged according to a second, drifting copy of this logic".
			 */
			m->content_metadata_identity = mon_content_metadata(m,
				&mastering_min, &mastering_max, &max_cll, &max_fall);

			const struct wlr_output_image_description image_description = {
				.primaries = WLR_COLOR_NAMED_PRIMARIES_BT2020,
				.transfer_function = WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ,
				.mastering_luminance = {
					.min = mastering_min,
					.max = mastering_max,
				},
				.max_cll = max_cll,
				.max_fall = max_fall,
			};
			if (!wlr_output_state_set_image_description(state,
														&image_description)) {
				m->hdr = 0;
				m->hdr_capability_failed = true;
			}
		}
	} else if (wlr_output->image_description) {
		wlr_output_state_set_image_description(state, NULL);
	}

	/* Only (re)set the render format when it actually differs from the
	 * live one. Re-testing an unchanged 10-bit format on every HDR toggle
	 * (capture fallback flips several times a session) is timing-sensitive:
	 * with a page-flip in flight the test can fail SPURIOUSLY, the code
	 * then "falls back" to 8-bit -- a real format change -- and the commit
	 * has to rebuild the swapchain, whose modifier-less fallback the Vulkan
	 * renderer cannot provide. Net effect: a failed frame build and a full
	 * retrain (visible flash) for what should be a colorimetry-only commit. */
	if ((m->bitdepth == 10 || m->hdr) &&
			wlr_output->render_format != DRM_FORMAT_XRGB2101010) {
		wlr_output_state_set_render_format(state, DRM_FORMAT_XRGB2101010);
		if (!wlr_output_test_state(wlr_output, state)) {
			wlr_log(WLR_INFO,
					"10-bit framebuffer not supported on output %s, "
					"falling back to 8-bit",
					wlr_output->name);
			wlr_output_state_set_render_format(state, DRM_FORMAT_XRGB8888);
		}
	}

	/* Deliberately no extra wlr_output_test_state() pre-check here: the
	 * static capability check above is enough, and testing this exact
	 * state again separately just doubles exposure to backend timing
	 * sensitivities around swapchain reconfiguration (seen in practice
	 * as spurious "swapchain failed test" rejections that only affect
	 * the HDR-enable direction, since disable never had this extra
	 * check). The real commit right after this call is the actual
	 * source of truth; its caller falls back to a retrain on failure. */
}


void createmon(struct wl_listener *listener, void *data) {
	/* This event is raised by the backend when a new output (aka a display or
	 * monitor) becomes available. */
	struct wlr_output *wlr_output = data;
	uint32_t i;
	int32_t vrr, custom;
	struct wlr_output_state state;
	Monitor *m = NULL;
	bool custom_monitor_mode = false;
	bool prefer_disable = false;


	if (!wlr_output_init_render(wlr_output, alloc, drw))
		return;

	/*
	 * ASTEROIDZ_AVK_FORCE_SOFTWARE_CURSOR=1 -- never use the cursor plane.
	 *
	 * There is no new backend behaviour here and deliberately so:
	 * wlr_output_lock_software_cursors() already exists, is what screencopy's
	 * overlay_cursor uses, and is the same mechanism a screen recorder trips.
	 * Taking a permanent lock at output creation means every frame on this
	 * output composites its cursor through az_avk_emit_cursors(), which is the
	 * path that otherwise only runs while something is capturing.
	 *
	 * Said out loud, once per output, because a session where the cursor is
	 * quietly costing a composite instead of a plane is exactly the kind of
	 * thing that should never be inferred from a frame rate.
	 */
	if (az_cursor_force_software()) {
		wlr_output_lock_software_cursors(wlr_output, true);
		wlr_log(WLR_INFO, "cursor: %s locked to SOFTWARE cursors "
				"(ASTEROIDZ_AVK_FORCE_SOFTWARE_CURSOR=1); the hardware plane "
				"will not be used and AVK composites the cursor into every "
				"frame", wlr_output->name);
	}

	if (wlr_output->non_desktop) {
		if (drm_lease_manager) {
			wlr_drm_lease_v1_manager_offer_output(drm_lease_manager,
												  wlr_output);
		}
		return;
	}

	struct wl_event_loop *loop = wl_display_get_event_loop(dpy);
	m = wlr_output->data = ecalloc(1, sizeof(*m));

	m->iscleanuping = false;
	m->skip_frame_timeout =
		wl_event_loop_add_timer(loop, monitor_skip_frame_timeout_callback, m);
	m->render_timer = wl_event_loop_add_timer(loop, render_timer_cb, m);
	m->render_dur_ms = 0.0;
	m->render_late_frac = 0.30; /* start conservative; adapts up when stable */
	m->render_late_last_ns = 0;
	m->render_late_deferred = false;
	m->render_late_pending = false;
	m->render_late_good = 0;
	m->retrain_timer = wl_event_loop_add_timer(loop, monitor_retrain_step, m);
	m->skiping_frame = false;
	m->resizing_count_pending = 0;
	m->resizing_count_current = 0;
	m->carousel_anim_dir = 0;

	m->wlr_output = wlr_output;
	m->wlr_output->data = m;

	wl_list_init(&m->dwl_ipc_outputs);

	for (i = 0; i < LENGTH(m->layers); i++)
		wl_list_init(&m->layers[i]);

	m->gappih = config.gappih;
	m->gappiv = config.gappiv;
	m->gappoh = config.gappoh;
	m->gappov = config.gappov;
	m->isoverview = 0;
	m->ov_dim = NULL;
	for (int32_t oi = 0; oi < OV_TAG_CELLS; oi++) {
		m->ov_cell_bg[oi] = NULL;
		m->ov_cell_label[oi] = NULL;
	}
	m->sel = NULL;
	m->is_in_hotarea = 0;
	m->m.x = INT32_MAX;
	m->m.y = INT32_MAX;
	float scale = 1;
	enum wl_output_transform rr = WL_OUTPUT_TRANSFORM_NORMAL;

	wlr_output_state_init(&state);
	wlr_output_state_set_scale(&state, scale);
	wlr_output_state_set_transform(&state, rr);

	ConfigMonitorRule merged_rule;
	if (monitor_merge_rules(m, &merged_rule)) {
		m->m.x = merged_rule.x;
		m->m.y = merged_rule.y;
		/* -1 when the rule is silent, so a later dispatch is an EXPLICIT
		 * choice rather than indistinguishable from the default. */
		m->hdr_configured = merged_rule.hdr >= 0 ? merged_rule.hdr : -1;
		/* a reconfigure may be a hotplug onto a different panel, so re-test
		 * capability rather than staying latched off from the old one */
		m->hdr_capability_failed = false;
		/*
		 * hdr_configured is TRI-STATE; hdr is a committed BOOLEAN. Assigning
		 * one to the other leaks -1 ("never mentioned") into the effective
		 * state, and -1 is truthy: every `if (m->hdr)` -- the PQ branch in
		 * mon_derive_color_state, the HDR commit in mon_state_apply_color, the
		 * 10-bit format choice -- reads an unconfigured output as HDR. Worse,
		 * hdr_resolve() compares `want == (m->hdr > 0)`, so want=false matches
		 * and it returns before ever normalising the value: the output stays
		 * at -1 for its whole life. Headless outputs came up on PQ + Path B
		 * and refused their first frames; 12 fixtures moved.
		 */
		m->hdr = m->hdr_configured > 0 ? 1 : 0;
		m->bitdepth = merged_rule.bitdepth > 0 ? merged_rule.bitdepth : 0;
		m->hdr_max_luminance =
			merged_rule.hdr_max_luminance > 0 ? merged_rule.hdr_max_luminance : 0;
		m->hdr_min_luminance =
			merged_rule.hdr_min_luminance > 0 ? merged_rule.hdr_min_luminance : 0;
		m->hdr_max_fall =
			merged_rule.hdr_max_fall > 0 ? merged_rule.hdr_max_fall : 0;
		mon_load_icc_profile(m, merged_rule.icc_profile);
		vrr = merged_rule.vrr >= 0 ? merged_rule.vrr : 0;
		custom = merged_rule.custom >= 0 ? merged_rule.custom : 0;
		prefer_disable = merged_rule.disable > 0;

		if (apply_rule_to_state(m, &merged_rule, &state, vrr, custom)) {
			custom_monitor_mode = true;
		}
	}

	if (!custom_monitor_mode) {
		struct wlr_output_mode *preferred = wlr_output_preferred_mode(wlr_output);
		if (preferred) {
			wlr_output_state_set_mode(&state, preferred);
		} else {
			/* No mode list => wlr_output_preferred_mode() returns NULL, and
			 * committing a NULL fixed mode FAILS. The failure used to be
			 * silent and the consequences were not: the output stayed
			 * !enabled, so the very next updatemons -- which runs as the
			 * output_layout change listener, i.e. from inside
			 * wlr_output_layout_add() below -- took the "remove disabled
			 * output from the layout" branch and freed the
			 * wlr_output_layout_output that wlroots was about to hand to the
			 * layout `add` signal. wlr_xdg_output_manager_v1 then read that
			 * freed struct, and the nested/X11 backends segfaulted on every
			 * startup.
			 *
			 * A virtual output's size is whatever the backend gave the
			 * window, so adopt that; refresh 0 means "unspecified". */
			int32_t w = wlr_output->width > 0 ? wlr_output->width : 1280;
			int32_t h = wlr_output->height > 0 ? wlr_output->height : 720;
			wlr_output_state_set_custom_mode(&state, w, h, 0);
		}
	}

	/* Set up event listeners */
	LISTEN(&wlr_output->events.frame, &m->frame, rendermon);
	/* Only wired when the operator asked for the trace: nothing else in the
	 * compositor needs presentation feedback, and a listener that exists only
	 * to be a no-op is still a listener on a hot signal. */
	if (az_pace_on())
		LISTEN(&wlr_output->events.present, &m->pace_present, pacepresent);
	/* M6A.1: unconditional, unlike the trace listener above. See presentmon --
	 * the compositor cannot own presentation while discarding the only signal
	 * that says when presentation happened. */
	LISTEN(&wlr_output->events.present, &m->present, presentmon);
	LISTEN(&wlr_output->events.destroy, &m->destroy, cleanupmon);
	LISTEN(&wlr_output->events.request_state, &m->request_state,
		   requestmonstate);

	wlr_output_state_set_enabled(&state, !prefer_disable);
	if (prefer_disable)
		m->asleep = 0;
	/* headless (virtual) outputs have no real connector: skip HDR/color
	 * state, it can crash or fail the commit */
	if (!wlr_output_is_headless(wlr_output))
		mon_state_apply_color(m, &state);
	/* EVERY output, virtual ones included -- see mon_derive_color_state(). */
	mon_derive_color_state(m, &state);
	if (!wlr_output_commit_state(wlr_output, &state)) {
		/* A rejected initial commit is not survivable if ignored, and it was
		 * ignored: the output stays !enabled, and the very next updatemons --
		 * which runs as the output_layout `change` listener, i.e. from inside
		 * the wlr_output_layout_add() a few lines below -- takes its "remove
		 * a disabled output from the layout" branch and frees the
		 * wlr_output_layout_output that wlroots is, at that moment, still
		 * about to pass to the layout `add` signal. wlr_xdg_output_manager_v1
		 * then dereferences the freed struct. That use-after-free segfaulted
		 * every nested (WLR_BACKENDS=wayland) and X11 startup, because a
		 * virtual output has no mode list and so ends up committing a mode
		 * the backend rejects.
		 *
		 * Retry with nothing but `enabled`, letting the backend keep whatever
		 * mode it came up with. A monitor at the wrong resolution is a far
		 * better outcome than a crash, and reapply_monitor_rules will have
		 * another go once the output is live. */
		wlr_log(WLR_ERROR,
				"output %s: initial commit rejected, retrying without a mode",
				wlr_output->name);
		wlr_output_state_finish(&state);
		wlr_output_state_init(&state);
		wlr_output_state_set_enabled(&state, !prefer_disable);
		if (!wlr_output_commit_state(wlr_output, &state))
			wlr_log(WLR_ERROR, "output %s: fallback commit also rejected",
					wlr_output->name);
	}
	wlr_output_state_finish(&state);

	/*
	 * ADR-604 trigger 1, and it has to be HERE rather than at listener setup.
	 *
	 * The presenter derives its nominal period and its regime from the output,
	 * so resetting before the mode and adaptive-sync state were committed read
	 * whatever the backend came up with: DP-1 took HDMI's 16666.666us period
	 * onto a 6944us display and reported regime=fixed on a VRR panel. Every
	 * number downstream then followed it -- the observed-period guard rejected
	 * every real sample as out of range and accepted only idle gaps, and the
	 * predictor aimed a 16.7ms lattice at a 6.9ms display for a -9266us mean
	 * error. One wrong field at construction, and nothing after it was right.
	 */
	az_presenter_reset(m, AZ_PRESENT_RESET_CREATE);
	if (getenv("AZ_BREAK_PRESENT_IDLE_WAKE") != NULL) {
		wlr_log(WLR_ERROR, "M6A break: AZ_BREAK_PRESENT_IDLE_WAKE -- a 1s "
			"timer now schedules frames on %s; idle is no longer idle",
			wlr_output->name);
		m->break_idle_wake_timer = wl_event_loop_add_timer(
			wl_display_get_event_loop(dpy), az_break_idle_wake_cb, m);
		wl_event_source_timer_update(m->break_idle_wake_timer, 1000);
	}

	wl_list_insert(&mons, &m->link);

	m->pertag = calloc(1, sizeof(Pertag));
	for (int i = 0; i < LENGTH(tags) + 1; i++)
		m->pertag->scroller_state[i] = NULL;

	if (chvt_backup_tag &&
		regex_match(chvt_backup_selmon, m->wlr_output->name)) {
		m->tagset[0] = m->tagset[1] = (1 << (chvt_backup_tag - 1)) & TAGMASK;
		m->pertag->curtag = m->pertag->prevtag = chvt_backup_tag;
		chvt_backup_tag = 0;
		memset(chvt_backup_selmon, 0, sizeof(chvt_backup_selmon));
	} else {
		m->tagset[0] = m->tagset[1] = 1;
		m->pertag->curtag = m->pertag->prevtag = 1;
	}

	for (i = 0; i <= LENGTH(tags); i++) {
		m->pertag->nmasters[i] = config.default_nmaster;
		m->pertag->mfacts[i] = config.default_mfact;
		m->pertag->ltidxs[i] = &layouts[DWINDLE];
	}

	// apply tag rule
	parse_tagrule(m);

	/* The xdg-protocol specifies:
	 *
	 * If the fullscreened surface is not opaque, the compositor must make
	 * sure that other screen content not part of the same surface tree (made
	 * up of subsurfaces, popups or similarly coupled surfaces) are not
	 * visible below the fullscreened surface.
	 *
	 */

	/* Adds this to the output layout in the order it was configured.
	 *
	 * The output layout utility automatically adds a wl_output global to the
	 * display, which Wayland clients can see to find out information about the
	 * output (such as DPI, scale factor, manufacturer, etc).
	 */
	m->scene_output = wlr_scene_output_create(scene, wlr_output);
	m->cursor_zoom = 1.0f;
	if (m->m.x == INT32_MAX || m->m.y == INT32_MAX)
		wlr_output_layout_add_auto(output_layout, wlr_output);
	else
		wlr_output_layout_add(output_layout, wlr_output, m->m.x, m->m.y);

	if (config.blur || config.shadows_blur_background) {
		ensure_monitor_blur_node(m);
	}
	m->ext_group = wlr_ext_workspace_group_handle_v1_create(
		ext_manager, EXT_WORKSPACE_ENABLE_CAPS);
	wlr_ext_workspace_group_handle_v1_output_enter(m->ext_group, m->wlr_output);

	for (i = 1; i <= LENGTH(tags); i++) {
		add_workspace_by_tag(i, m);
	}

	/* Deliberately last: VISIBLEON needs the tagset initialised above. On a
	 * hotplug a force_hdr client can already be visible on the new output. */
	hdr_resolve(m);

	printstatus(IPC_WATCH_ARRANGGE);
}

void // fix for 0.5
createnotify(struct wl_listener *listener, void *data) {
	/* This event is raised when wlr_xdg_shell receives a new xdg surface from a
	 * client, either a toplevel (application window) or popup,
	 * or when wlr_layer_shell receives a new popup from a layer.
	 * If you want to do something tricky with popups you should check if
	 * its parent is wlr_xdg_shell or wlr_layer_shell */
	struct wlr_xdg_toplevel *toplevel = data;
	Client *c = NULL;

	/* Allocate a Client for this surface */
	c = toplevel->base->data = ecalloc(1, sizeof(*c));
	c->surface.xdg = toplevel->base;
	c->bw = config.borderpx;

	LISTEN(&toplevel->base->surface->events.commit, &c->commit, commitnotify);
	LISTEN(&toplevel->base->surface->events.map, &c->map, mapnotify);
	LISTEN(&toplevel->base->surface->events.unmap, &c->unmap, unmapnotify);
	LISTEN(&toplevel->events.destroy, &c->destroy, destroynotify);
	LISTEN(&toplevel->events.request_fullscreen, &c->fullscreen,
		   fullscreennotify);
	LISTEN(&toplevel->events.request_maximize, &c->maximize, maximizenotify);
	LISTEN(&toplevel->events.request_minimize, &c->minimize, minimizenotify);
	LISTEN(&toplevel->events.set_title, &c->set_title, updatetitle);
}

void destroyinputdevice(struct wl_listener *listener, void *data) {
	InputDevice *input_dev =
		wl_container_of(listener, input_dev, destroy_listener);

	// clean up device-specific data
	if (input_dev->device_data) {
		// perform cleanup specific to the device type
		switch (input_dev->wlr_device->type) {
		case WLR_INPUT_DEVICE_SWITCH: {
			Switch *sw = (Switch *)input_dev->device_data;
			// remove the toggle listener
			wl_list_remove(&sw->toggle.link);
			// free the Switch memory
			free(sw);
			break;
		}
		// cleanup code for other device types can be added here
		default:
			break;
		}
		input_dev->device_data = NULL;
	}

	// remove from the device list
	wl_list_remove(&input_dev->link);
	// remove the destroy listener
	wl_list_remove(&input_dev->destroy_listener.link);
	// free the memory
	free(input_dev);
}

void pointer_set_accel(struct libinput_device *device, bool natural_scrolling,
					   uint32_t mouse_accel_profile, double mouse_accel_speed) {
	libinput_device_config_scroll_set_natural_scroll_enabled(device,
															 natural_scrolling);
	if (mouse_accel_profile &&
		libinput_device_config_accel_is_available(device)) {
		libinput_device_config_accel_set_profile(device, mouse_accel_profile);
		libinput_device_config_accel_set_speed(device, mouse_accel_speed);
	} else {
		// profile cannot be directly applied to 0, need to set to 1 first
		libinput_device_config_accel_set_profile(device, 1);
		libinput_device_config_accel_set_profile(device, 0);
		libinput_device_config_accel_set_speed(device, 0);
	}
}

void configure_pointer(struct libinput_device *device) {
	if (libinput_device_config_tap_get_finger_count(device)) {
		libinput_device_config_tap_set_enabled(device, config.tap_to_click);
		libinput_device_config_tap_set_drag_enabled(device,
													config.tap_and_drag);
		libinput_device_config_tap_set_drag_lock_enabled(device,
														 config.drag_lock);
		libinput_device_config_tap_set_button_map(device, config.button_map);
		pointer_set_accel(device, config.trackpad_natural_scrolling,
						  config.trackpad_accel_profile,
						  config.trackpad_accel_speed);
	} else {
		pointer_set_accel(device, config.mouse_natural_scrolling,
						  config.mouse_accel_profile, config.mouse_accel_speed);
	}

	if (libinput_device_config_dwt_is_available(device))
		libinput_device_config_dwt_set_enabled(device,
											   config.disable_while_typing);

	if (libinput_device_config_left_handed_is_available(device))
		libinput_device_config_left_handed_set(device, config.left_handed);

	if (libinput_device_config_middle_emulation_is_available(device))
		libinput_device_config_middle_emulation_set_enabled(
			device, config.middle_button_emulation);

	if (libinput_device_config_scroll_get_methods(device) !=
		LIBINPUT_CONFIG_SCROLL_NO_SCROLL)
		libinput_device_config_scroll_set_method(device, config.scroll_method);
	if (libinput_device_config_scroll_get_methods(device) ==
		LIBINPUT_CONFIG_SCROLL_ON_BUTTON_DOWN)
		libinput_device_config_scroll_set_button(device, config.scroll_button);

	if (libinput_device_config_click_get_methods(device) !=
		LIBINPUT_CONFIG_CLICK_METHOD_NONE)
		libinput_device_config_click_set_method(device, config.click_method);

	if (libinput_device_config_send_events_get_modes(device))
		libinput_device_config_send_events_set_mode(device,
													config.send_events_mode);
}

void createpointer(struct wlr_pointer *pointer) {

	struct libinput_device *device = NULL;

	if (wlr_input_device_is_libinput(&pointer->base) &&
		(device = wlr_libinput_get_device_handle(&pointer->base))) {

		configure_pointer(device);

		InputDevice *input_dev = calloc(1, sizeof(InputDevice));
		input_dev->wlr_device = &pointer->base;
		input_dev->libinput_device = device;

		input_dev->destroy_listener.notify = destroyinputdevice;
		wl_signal_add(&pointer->base.events.destroy,
					  &input_dev->destroy_listener);

		wl_list_insert(&inputdevices, &input_dev->link);
	}
	wlr_cursor_attach_input_device(cursor, &pointer->base);
}

void switch_toggle(struct wl_listener *listener, void *data) {
	// get the struct containing the listener
	Switch *sw = wl_container_of(listener, sw, toggle);

	// handle the toggle event
	struct wlr_switch_toggle_event *event = data;
	SwitchBinding *s;
	int32_t ji;

	for (ji = 0; ji < config.switch_bindings_count; ji++) {
		if (config.switch_bindings_count < 1)
			break;
		s = &config.switch_bindings[ji];
		if (event->switch_state == s->fold && s->func) {
			s->func(&s->arg);
			return;
		}
	}
}

void createswitch(struct wlr_switch *switch_device) {

	struct libinput_device *device = NULL;

	if (wlr_input_device_is_libinput(&switch_device->base) &&
		(device = wlr_libinput_get_device_handle(&switch_device->base))) {

		InputDevice *input_dev = calloc(1, sizeof(InputDevice));
		input_dev->wlr_device = &switch_device->base;
		input_dev->libinput_device = device;
		input_dev->device_data = NULL; // initialize to NULL

		input_dev->destroy_listener.notify = destroyinputdevice;
		wl_signal_add(&switch_device->base.events.destroy,
					  &input_dev->destroy_listener);

		// create Switch-specific data
		Switch *sw = calloc(1, sizeof(Switch));
		sw->wlr_switch = switch_device;
		sw->toggle.notify = switch_toggle;
		sw->input_dev = input_dev;

		// save the Switch pointer into input_device
		input_dev->device_data = sw;

		// add the toggle listener
		wl_signal_add(&switch_device->events.toggle, &sw->toggle);

		// add to the global list
		wl_list_insert(&inputdevices, &input_dev->link);
	}
}

void createpointerconstraint(struct wl_listener *listener, void *data) {
	PointerConstraint *pointer_constraint =
		ecalloc(1, sizeof(*pointer_constraint));
	pointer_constraint->constraint = data;
	LISTEN(&pointer_constraint->constraint->events.destroy,
		   &pointer_constraint->destroy, destroypointerconstraint);

	if (!selmon || !selmon->sel)
		return;

	struct wlr_surface *focused_surface = client_surface(selmon->sel);
	if (focused_surface &&
		focused_surface == pointer_constraint->constraint->surface) {
		cursorconstrain(pointer_constraint->constraint);
	}
}

void cursorconstrain(struct wlr_pointer_constraint_v1 *constraint) {
	if (active_constraint == constraint)
		return;

	if (active_constraint) {
		if (constraint == NULL) {
			cursorwarptohint();
		}
		wlr_pointer_constraint_v1_send_deactivated(active_constraint);
	}

	active_constraint = constraint;

	if (constraint) {
		wlr_pointer_constraint_v1_send_activated(constraint);
	}
}

void cursorframe(struct wl_listener *listener, void *data) {
	/* This event is forwarded by the cursor when a pointer emits an frame
	 * event. Frame events are sent after regular pointer events to group
	 * multiple events together. For instance, two axis events may happen at
	 * the same time, in which case a frame event won't be sent in between.
	 */
	/* Notify the client with pointer focus of the frame event. */
	wlr_seat_pointer_notify_frame(seat);
}

void cursorwarptohint(void) {
	Client *c = NULL;
	double sx = active_constraint->current.cursor_hint.x;
	double sy = active_constraint->current.cursor_hint.y;

	toplevel_from_wlr_surface(active_constraint->surface, &c, NULL);
	if (c && active_constraint->current.cursor_hint.enabled) {
		/* BOUNDARY 4 again, in the other direction. The hint is where the
		 * CLIENT wants the pointer, in its own surface coordinates; the
		 * cursor is warped in layout coordinates, and the seat is told the
		 * surface ones unchanged. `s` is 1 for every client but an X11 one
		 * being sized in raw pixels. */
		float s = client_x11_scale(c);
		wlr_cursor_warp(cursor, NULL, sx / s + c->geom.x + c->bw,
						sy / s + c->geom.y + c->bw);
		wlr_seat_pointer_warp(active_constraint->seat, sx, sy);
	}
}

void destroydragicon(struct wl_listener *listener, void *data) {
	/* Focus enter isn't sent during drag, so refocus the focused node. */
	focusclient(focustop(selmon), 1);
	motionnotify(0, NULL, 0, 0, 0, 0);
	wl_list_remove(&listener->link);
	free(listener);
}

void destroyidleinhibitor(struct wl_listener *listener, void *data) {
	/* `data` is the wlr_surface of the idle inhibitor being destroyed,
	 * at this point the idle inhibitor is still in the list of the manager
	 */
	checkidleinhibitor(wlr_surface_get_root_surface(data));
	wl_list_remove(&listener->link);
	free(listener);
}

void destroylayernodenotify(struct wl_listener *listener, void *data) {
	LayerSurface *l = wl_container_of(listener, l, destroy);

	wl_list_remove(&l->link);
	wl_list_remove(&l->destroy.link);
	wl_list_remove(&l->map.link);
	wl_list_remove(&l->unmap.link);
	wl_list_remove(&l->surface_commit.link);
	wlr_scene_node_destroy(&l->popups->node);
	free(l);
}

void destroylock(SessionLock *lock, int32_t unlock) {
	wlr_seat_keyboard_notify_clear_focus(seat);
	if ((locked = !unlock))
		goto destroy;

	if (locked_bg->node.enabled) {
		wlr_scene_node_set_enabled(&locked_bg->node, false);
	}

	focusclient(focustop(selmon), 0);
	motionnotify(0, NULL, 0, 0, 0, 0);

destroy:
	wl_list_remove(&lock->new_surface.link);
	wl_list_remove(&lock->unlock.link);
	wl_list_remove(&lock->destroy.link);

	wlr_scene_node_destroy(&lock->scene->node);
	cur_lock = NULL;
	free(lock);
	/* After the goto's target, so both ways out are covered: a lock client
	 * that crashed without unlocking leaves `locked` set, and a monitor told
	 * the screensaver had gone away when it had not would be worse than not
	 * being told at all. */
	inhibit_portal_screensaver_changed();
}

void destroylocksurface(struct wl_listener *listener, void *data) {
	Monitor *m = wl_container_of(listener, m, destroy_lock_surface);
	struct wlr_session_lock_surface_v1 *surface,
		*lock_surface = m->lock_surface;

	m->lock_surface = NULL;
	wl_list_remove(&m->destroy_lock_surface.link);

	if (lock_surface->surface != seat->keyboard_state.focused_surface) {
		if (exclusive_focus && !locked) {
			reset_exclusive_layers_focus(m);
		}
		return;
	}

	if (locked && cur_lock && !wl_list_empty(&cur_lock->surfaces)) {
		surface = wl_container_of(cur_lock->surfaces.next, surface, link);
		client_notify_enter(surface->surface, wlr_seat_get_keyboard(seat));
	} else if (!locked) {
		reset_exclusive_layers_focus(selmon);
	} else {
		wlr_seat_keyboard_clear_focus(seat);
	}
}

void // 0.7 custom
destroynotify(struct wl_listener *listener, void *data) {
	/* Called when the xdg_toplevel is destroyed. */
	Client *c = wl_container_of(listener, c, destroy);
	/* Never leave the debounced float-raise timer pointing at this Client
	 * past this point -- it's about to be freed below. */
	if (float_focus_raise_pending == c)
		float_focus_raise_pending = NULL;
	wl_list_remove(&c->destroy.link);
	wl_list_remove(&c->set_title.link);
	wl_list_remove(&c->fullscreen.link);
	wl_list_remove(&c->maximize.link);
	wl_list_remove(&c->minimize.link);
#ifdef XWAYLAND
	if (c->type != XDGShell) {
		wl_list_remove(&c->activate.link);
		wl_list_remove(&c->associate.link);
		wl_list_remove(&c->configure.link);
		wl_list_remove(&c->dissociate.link);
		wl_list_remove(&c->set_hints.link);
	} else
#endif
	{
		wl_list_remove(&c->commit.link);
		wl_list_remove(&c->map.link);
		wl_list_remove(&c->unmap.link);
	}
	/* The xdg-decoration outlives the toplevel: wlroots emits the toplevel's
	 * destroy signal FIRST and destroys the decoration afterwards, so leaving
	 * these two listeners registered lets destroydecoration() run against a
	 * Client that's already been freed here. Confirmed live under ASAN as a
	 * heap-use-after-free (the `c->decoration = NULL` write in
	 * destroydecoration), reproducing 10/10 times. c->decoration is non-NULL
	 * exactly when createdecoration registered these, so it gates both.
	 *
	 * Only the ABRUPT-disconnect path hits this, which is why it never shows
	 * up in normal use: a client closing gracefully destroys its decoration
	 * object before its toplevel, so the ordering is already safe. It's
	 * wl_client_destroy() -- the client process being killed or crashing --
	 * that tears every resource down in one sweep with the toplevel first.
	 * The regression suite force-kills its windows, so it reproduces there
	 * every run; a real browser/game crash reaches it the same way. */
	if (c->decoration) {
		wl_list_remove(&c->destroy_decoration.link);
		wl_list_remove(&c->set_decoration_mode.link);
		c->decoration = NULL;
	}
	free(c->icon_name);
	free(c->toplevel_tag);
	free(c);
}

void destroypointerconstraint(struct wl_listener *listener, void *data) {
	PointerConstraint *pointer_constraint =
		wl_container_of(listener, pointer_constraint, destroy);

	if (active_constraint == pointer_constraint->constraint) {
		cursorwarptohint();
		active_constraint = NULL;
	}

	wl_list_remove(&pointer_constraint->destroy.link);
	free(pointer_constraint);
}

void destroysessionlock(struct wl_listener *listener, void *data) {
	SessionLock *lock = wl_container_of(listener, lock, destroy);
	destroylock(lock, 0);
}

void destroykeyboardgroup(struct wl_listener *listener, void *data) {
	KeyboardGroup *group = wl_container_of(listener, group, destroy);
	wl_event_source_remove(group->key_repeat_source);
	wl_list_remove(&group->key.link);
	wl_list_remove(&group->modifiers.link);
	wl_list_remove(&group->destroy.link);
	wlr_keyboard_group_destroy(group->wlr_group);
	free(group);
}

/* Most recent time any X11 client lost focus. All XWayland windows share one
 * X server, so when a focused X11 window is defocused the server hands X focus
 * to a sibling, which then fires request_activate. That sibling was never
 * itself unfocused, so a per-client check misses it — this global lets
 * activatex11 recognise the sibling's activate as part of the same steal. */
static uint32_t last_x11_unfocus_ms = 0;

/* Most recent time the user deliberately switched the viewed tag(s) (view()).
 * The per-unfocus steal guards above key off when the *app* fires
 * request_activate relative to losing focus — but an Electron app (e.g.
 * TradingView under XWayland) can re-fire it on a delay or repeatedly, and the
 * scroller layout's animated arrange pushes that activate past the per-unfocus
 * cooldown, so the view gets yanked back to the app's tag. Keying the guard on
 * the user's own tag switch instead is robust to that timing: for a short
 * cooldown after a deliberate switch, no activate may change the viewed tag.
 * focus_on_activate still works normally outside that window. */
static uint32_t last_user_view_ms = 0;

#define FOCUS_ACTIVATE_STEAL_MS 1000u
#define FOCUS_VIEW_STEAL_MS 1500u

/* the mapped, visible modal xdg dialog of p, if any (X11 modals manage
 * their own focus client-side, so this is Wayland-only) */
static Client *client_modal_child(Client *p) {
	Client *c;
	if (!p || client_is_x11(p))
		return NULL;
	wl_list_for_each(c, &clients, link) {
		if (c != p && !c->iskilling && !client_is_x11(c) &&
			client_surface(c)->mapped && c->mon && VISIBLEON(c, c->mon) &&
			c->surface.xdg->toplevel->parent == p->surface.xdg->toplevel &&
			client_is_modal(c))
			return c;
	}
	return NULL;
}

void focusclient(Client *c, int32_t lift) {

	Client *last_focus_client = NULL;
	Monitor *um = NULL;

	struct wlr_surface *old_keyboard_focus_surface =
		seat->keyboard_state.focused_surface;

	if (locked)
		return;

	if (c && c->iskilling)
		return;

	if (c && !client_surface(c)->mapped)
		return;

	/* a window with an open modal dialog yields focus to the dialog (walk
	 * chained modals, bounded against pathological parent cycles) */
	for (int32_t depth = 0; c && depth < 4; depth++) {
		Client *modal = client_modal_child(c);
		if (!modal)
			break;
		c = modal;
	}

	if (c && client_should_ignore_focus(c) && client_is_x11_popup(c))
		return;

	if (c && c->nofocus)
		return;

	/* float layout raises on any focus, not only lift-requesting callers --
	 * debounced (schedule_float_focus_raise), not instant: an immediate
	 * raise on every transient focus change reads as flickery when focus
	 * moves quickly across several overlapping floating windows. An
	 * explicit lift request (e.g. clicking a window directly) stays
	 * instant below. */
	bool auto_float_raise = false;
	if (c && !lift && c->isfloating && c->mon && is_float_layout(c->mon))
		auto_float_raise = true;

	/* Raise client in stacking order if requested */
	if (c && lift) {
		wlr_scene_node_raise_to_top(&c->scene->node); // raise the view to the top
	} else if (auto_float_raise) {
		schedule_float_focus_raise(c);
	}

	if (c && client_surface(c) == old_keyboard_focus_surface && selmon &&
		selmon->sel)
		return;

	if (selmon && selmon->sel && selmon->sel != c &&
		selmon->sel->foreign_toplevel) {
		wlr_foreign_toplevel_handle_v1_set_activated(
			selmon->sel->foreign_toplevel, false);
	}

	if (c && !c->iskilling && !client_is_unmanaged(c) && c->mon) {

		/* selmon is NULL-checked twice immediately above; it can be NULL here
		 * too, and this is the line that reassigns it from c->mon. */
		last_focus_client = selmon ? selmon->sel : NULL;
		selmon = c->mon;
		selmon->prevsel = selmon->sel;
		selmon->sel = c;
		c->isfocused = true;

		check_keep_idle_inhibit(c);
		check_vrr_enable(c);
		hdr_resolve_all();

		if (last_focus_client && !last_focus_client->iskilling &&
			last_focus_client != c) {
			last_focus_client->isfocused = false;
			last_focus_client->last_unfocus_ms = get_now_in_ms();
			if (last_focus_client->type == X11)
				last_x11_unfocus_ms = last_focus_client->last_unfocus_ms;
			client_set_unfocused_opacity_animation(last_focus_client);
		}

		client_set_focused_opacity_animation(c);

		// decide whether need to re-arrange

		// change focus link position
		wl_list_remove(&c->flink);
		wl_list_insert(&fstack, &c->flink);

		if (c && selmon->prevsel &&
			(selmon->prevsel->tags & selmon->tagset[selmon->seltags]) &&
			(c->tags & selmon->tagset[selmon->seltags]) && !c->isfloating &&
			(is_scroller_layout(selmon) || is_monocle_layout(selmon))) {
			arrange(selmon, false, false);
		}

		// change border color
		c->isurgent = 0;
	}

	// update other monitor focus disappear
	wl_list_for_each(um, &mons, link) {
		if (um->wlr_output->enabled && um != selmon && um->sel &&
			!um->sel->iskilling && um->sel->isfocused) {

			um->sel->isfocused = false;
			client_set_unfocused_opacity_animation(um->sel);

			if (um->sel->foreign_toplevel) {
				wlr_foreign_toplevel_handle_v1_set_activated(
					um->sel->foreign_toplevel, false);
			}
		}
	}

	if (c && !c->iskilling && c->foreign_toplevel)
		wlr_foreign_toplevel_handle_v1_set_activated(c->foreign_toplevel, true);

	/* Deactivate old client if focus is changing */
	if (old_keyboard_focus_surface &&
		(!c || client_surface(c) != old_keyboard_focus_surface)) {
		/* If an exclusive_focus layer is focused, don't focus or activate
		 * the client, but only update its position in fstack to render its
		 * border with focuscolor and focus it after the exclusive_focus
		 * layer is closed. */
		Client *w = NULL;
		LayerSurface *l = NULL;
		int32_t type =
			toplevel_from_wlr_surface(old_keyboard_focus_surface, &w, &l);
		if (type == LayerShell && l->scene->node.enabled &&
			l->layer_surface->current.layer >= ZWLR_LAYER_SHELL_V1_LAYER_TOP &&
			l == exclusive_focus) {
			return;
		} else if (w && w == exclusive_focus && client_wants_focus(w)) {
			return;
			/* Don't deactivate old_keyboard_focus_surface client if the new
			 * one wants focus, as this causes issues with winecfg and
			 * probably other clients */
		} else if (w && !client_is_unmanaged(w) &&
				   (!c || !client_wants_focus(c))) {
			client_activate_surface(old_keyboard_focus_surface, 0);
		}
	}
	printstatus(IPC_WATCH_ARRANGGE);

	if (!c) {

		if (selmon && selmon->sel &&
			(!VISIBLEON(selmon->sel, selmon) || selmon->sel->iskilling ||
			 !client_surface(selmon->sel)->mapped))
			selmon->sel = NULL;

		// clear text input focus state
		dwl_im_relay_set_focus(dwl_input_method_relay, NULL);
		wlr_seat_keyboard_notify_clear_focus(seat);
		check_vrr_enable(NULL);
		hdr_resolve_all();
		if (active_constraint) {
			cursorconstrain(NULL);
		}
		return;
	}

	/* Change cursor surface */
	motionnotify(0, NULL, 0, 0, 0, 0);

	// set text input focus
	// must before client_notify_enter,
	// otherwise the position of text_input will be wrong.
	dwl_im_relay_set_focus(dwl_input_method_relay, client_surface(c));

	/* Have a client, so focus its top-level wlr_surface */
	client_notify_enter(client_surface(c), wlr_seat_get_keyboard(seat));

	/* Activate the new client */
	client_activate_surface(client_surface(c), 1);

	if (active_constraint && active_constraint->surface != client_surface(c)) {
		cursorconstrain(NULL);
	}

	struct wlr_pointer_constraint_v1 *constraint;
	wl_list_for_each(constraint, &pointer_constraints->constraints, link) {
		if (constraint->surface == client_surface(c)) {
			cursorconstrain(constraint);
			break;
		}
	}
}

void // 0.6
fullscreennotify(struct wl_listener *listener, void *data) {
	Client *c = wl_container_of(listener, c, fullscreen);

	if (!c || c->iskilling)
		return;

	setfullscreen(c, client_wants_fullscreen(c), true);
}

void requestmonstate(struct wl_listener *listener, void *data) {
	/* This ensures nested backends can be resized */
	Monitor *m = wl_container_of(listener, m, request_state);
	const struct wlr_output_event_request_state *event = data;

	/* The mode branch here used to stage the requested size into `m->pending`
	 * and never commit it -- and nothing else ever read that field, so it was
	 * written twice and used nowhere. The visible result: resizing the host
	 * window of a nested session moved the window but left the output at its
	 * old size, so only the top-left of the frame was ever painted and the
	 * host's own wallpaper showed through the rest. Commit the state the
	 * backend actually asked for. */
	/* ADR-604 trigger 3: a backend-initiated state change. Reset AFTER the
	 * commit, so the period is re-derived from what landed. */
	if (!wlr_output_commit_state(m->wlr_output, event->state)) {
		wlr_log(WLR_ERROR, "output %s: requested state could not be applied",
				m->wlr_output->name);
		return;
	}
	az_presenter_reset(m, AZ_PRESENT_RESET_REQUEST_STATE);

	/* A size change has to re-run the layout: m->m/m->w, the bar strip and
	 * every tiled window are all derived from the output geometry. Bitmask
	 * test, not equality -- the backend may commit the mode alongside other
	 * fields, which the old `==` silently skipped. */
	if (event->state->committed & WLR_OUTPUT_STATE_MODE) {
		updatemons(NULL, NULL);
		wlr_output_schedule_frame(m->wlr_output);
	}
}

void inputdevice(struct wl_listener *listener, void *data) {
	/* This event is raised by the backend when a new input device becomes
	 * available.
	 * when the backend is a headless backend, this event will never be
	 * triggered.
	 */
	struct wlr_input_device *device = data;
	uint32_t caps;

	switch (device->type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		createkeyboard(wlr_keyboard_from_input_device(device));
		break;
	case WLR_INPUT_DEVICE_TABLET:
		createtablet(device);
		break;
	case WLR_INPUT_DEVICE_TABLET_PAD:
		createtabletpad(device);
		break;
	case WLR_INPUT_DEVICE_POINTER:
		createpointer(wlr_pointer_from_input_device(device));
		break;
	case WLR_INPUT_DEVICE_SWITCH:
		createswitch(wlr_switch_from_input_device(device));
		break;
	default:
		/* TODO handle other input device types */
		break;
	}

	/* We need to let the wlr_seat know what our capabilities are, which is
	 * communiciated to the client. In dwl we always have a cursor, even if
	 * there are no pointer devices, so we always include that capability.
	 */
	/* TODO do we actually require a cursor? */
	caps = WL_SEAT_CAPABILITY_POINTER;
	if (!wl_list_empty(&kb_group->wlr_group->devices))
		caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	wlr_seat_set_capabilities(seat, caps);
}

int32_t keyrepeat(void *data) {
	KeyboardGroup *group = data;
	int32_t i;
	if (!group->nsyms || group->wlr_group->keyboard.repeat_info.rate <= 0)
		return 0;

	wl_event_source_timer_update(
		group->key_repeat_source,
		1000 / group->wlr_group->keyboard.repeat_info.rate);

	for (i = 0; i < group->nsyms; i++)
		keybinding(WL_KEYBOARD_KEY_STATE_PRESSED, false, group->mods,
				   group->keysyms[i], group->keycode);

	return 0;
}

bool is_keyboard_shortcut_inhibitor(struct wlr_surface *surface) {
	KeyboardShortcutsInhibitor *kbsinhibitor;

	wl_list_for_each(kbsinhibitor, &keyboard_shortcut_inhibitors, link) {
		if (kbsinhibitor->inhibitor->surface == surface) {
			return true;
		}
	}
	return false;
}

int32_t // 17
keybinding(uint32_t state, bool locked, uint32_t mods, xkb_keysym_t sym,
		   uint32_t keycode) {
	/*
	 * Here we handle compositor keybindings. This is when the compositor is
	 * processing keys, rather than passing them on to the client for its
	 * own processing.
	 */
	int32_t handled = 0;
	const KeyBinding *k;
	int32_t ji;
	int32_t isbreak = 0;

	if (is_keyboard_shortcut_inhibitor(seat->keyboard_state.focused_surface)) {
		return false;
	}

	/* Escape closes the overview (and jump mode) */
	if (state == WL_KEYBOARD_KEY_STATE_PRESSED && sym == XKB_KEY_Escape &&
		selmon && selmon->isoverview) {
		toggleoverview(&(Arg){.i = 1});
		return true;
	}

	for (ji = 0; ji < config.key_bindings_count; ji++) {
		if (config.key_bindings_count < 1)
			break;

		if (locked && config.key_bindings[ji].islockapply == false)
			continue;

		if (state == WL_KEYBOARD_KEY_STATE_RELEASED &&
			config.key_bindings[ji].isreleaseapply == false)
			continue;

		if (state == WL_KEYBOARD_KEY_STATE_PRESSED &&
			config.key_bindings[ji].isreleaseapply == true)
			continue;

		if (state != WL_KEYBOARD_KEY_STATE_PRESSED &&
			state != WL_KEYBOARD_KEY_STATE_RELEASED)
			continue;

		k = &config.key_bindings[ji];

		/* the overview is modal: while it's open, only Escape (handled above)
		 * and the binds that operate the overview itself (toggle to close it,
		 * jump mode for its labels) are honoured -- tag switches, window ops,
		 * spawns etc. are suppressed until it closes.
		 *
		 * screenshot_ui is the exception: it changes nothing, it photographs
		 * whatever is on the screen, and the overview is a view worth
		 * photographing like any other -- the spread of every window on a tag
		 * is not something you can capture any other way. Closing the overview
		 * to take the picture destroys the thing being pictured. */
		if (selmon && selmon->isoverview && k->func != toggleoverview &&
			k->func != togglejump && k->func != screenshot_ui)
			continue;

		if ((k->iscommonmode || (k->isdefaultmode && keymode.isdefault) ||
			 (strcmp(keymode.mode, k->mode) == 0)) &&
			CLEANMASK(mods) == CLEANMASK(k->mod) &&
			((k->keysymcode.type == KEY_TYPE_SYM &&
			  xkb_keysym_to_lower(sym) ==
				  xkb_keysym_to_lower(k->keysymcode.keysym)) ||
			 (k->keysymcode.type == KEY_TYPE_CODE &&
			  (keycode == k->keysymcode.keycode.keycode1 ||
			   keycode == k->keysymcode.keycode.keycode2 ||
			   keycode == k->keysymcode.keycode.keycode3))) &&
			k->func) {

			if (!k->ispassapply)
				handled = 1;
			else
				handled = 0;

			isbreak = k->func(&k->arg);

			if (isbreak)
				break;
		}
	}
	return handled;
}

bool keypressglobal(struct wlr_surface *last_surface,
					struct wlr_keyboard *keyboard,
					struct wlr_keyboard_key_event *event, uint32_t mods,
					xkb_keysym_t keysym, uint32_t keycode) {
	Client *c = NULL, *lastc = focustop(selmon);
	uint32_t keycodes[32] = {0};
	int32_t reset = false;
	const char *appid = NULL;
	const char *title = NULL;
	int32_t ji;
	const ConfigWinRule *r;

	for (ji = 0; ji < config.window_rules_count; ji++) {
		if (config.window_rules_count < 1)
			break;
		r = &config.window_rules[ji];

		if (!r->globalkeybinding.mod ||
			(!r->globalkeybinding.keysymcode.keysym &&
			 !r->globalkeybinding.keysymcode.keycode.keycode1 &&
			 !r->globalkeybinding.keysymcode.keycode.keycode2 &&
			 !r->globalkeybinding.keysymcode.keycode.keycode3))
			continue;

		/* match key only (case insensitive) ignoring mods */
		if (((r->globalkeybinding.keysymcode.type == KEY_TYPE_SYM &&
			  r->globalkeybinding.keysymcode.keysym == keysym) ||
			 (r->globalkeybinding.keysymcode.type == KEY_TYPE_CODE &&
			  (r->globalkeybinding.keysymcode.keycode.keycode1 == keycode ||
			   r->globalkeybinding.keysymcode.keycode.keycode2 == keycode ||
			   r->globalkeybinding.keysymcode.keycode.keycode3 == keycode))) &&
			r->globalkeybinding.mod == mods) {
			wl_list_for_each(c, &clients, link) {
				if (c && c != lastc) {
					appid = client_get_appid(c);
					title = client_get_title(c);

					if ((r->title && regex_match(r->title, title) && !r->id) ||
						(r->id && regex_match(r->id, appid) && !r->title) ||
						(r->id && regex_match(r->id, appid) && r->title &&
						 regex_match(r->title, title))) {
						reset = true;
						wlr_seat_keyboard_enter(seat, client_surface(c),
												keycodes, 0,
												&keyboard->modifiers);
						wlr_seat_keyboard_send_key(seat, event->time_msec,
												   event->keycode,
												   event->state);
						goto done;
					}
				}
			}
		}
	}

done:
	if (reset)
		wlr_seat_keyboard_enter(seat, last_surface, keycodes, 0,
								&keyboard->modifiers);
	return reset;
}

void keypress(struct wl_listener *listener, void *data) {
	int32_t i;
	/* This event is raised when a key is pressed or released. */
	KeyboardGroup *group = wl_container_of(listener, group, key);
	struct wlr_keyboard_key_event *event = data;

	struct wlr_surface *last_surface = seat->keyboard_state.focused_surface;
	struct wlr_xdg_surface *xdg_surface =
		last_surface ? wlr_xdg_surface_try_from_wlr_surface(last_surface)
					 : NULL;
	int32_t pass = 0;
	bool hit_global = false;
#ifdef XWAYLAND
	struct wlr_xwayland_surface *xsurface =
		last_surface ? wlr_xwayland_surface_try_from_wlr_surface(last_surface)
					 : NULL;
#endif

	/* Translate libinput keycode -> xkbcommon */
	uint32_t keycode = event->keycode + 8;
	/* Get a list of keysyms based on the keymap for this keyboard */
	const xkb_keysym_t *syms;
	int32_t nsyms = xkb_state_key_get_syms(group->wlr_group->keyboard.xkb_state,
										   keycode, &syms);

	int32_t handled = 0;
	uint32_t mods = wlr_keyboard_get_modifiers(&group->wlr_group->keyboard);

	wlr_idle_notifier_v1_notify_activity(idle_notifier, seat);
	if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED)
		wake_sleeping_monitors();

	/* A keybind editor is asking what you just pressed.
	 *
	 * BEFORE the binding loop, and swallowing the event either way. That order is
	 * the whole point of capturing here rather than in the client: the compositor
	 * takes bindings before the focused surface sees them, so a client reading
	 * its own key events receives everything except the combinations that are
	 * already bound -- exactly the ones you reach for when rebinding. And a
	 * captured Super+Q that also ran kill_client would close the window you were
	 * editing from. */
	if (chord_capture_handle(event->state, mods, nsyms > 0 ? syms[0] : 0,
							 keycode))
		return;

	/* the screenshot overlay owns the keyboard outright while active:
	 * Escape cancels it, everything else is swallowed so global shortcuts
	 * and clients underneath the frozen frame don't react to it */
	if (shotui.active) {
		if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
			for (i = 0; i < nsyms; i++)
				screenshot_ui_handle_key(syms[i]);
		}
		/* the key repeat timer armed by the very press that opened this
		 * overlay (handled *before* shotui.active went true on the next
		 * frame) never reaches the disarm logic below while we keep
		 * early-returning here. Left alone it fires forever via
		 * keyrepeat(), which calls keybinding() directly and re-triggers
		 * screenshot_ui() the instant teardown() clears shotui.active --
		 * an infinite relaunch loop that eats all keyboard input. */
		group->nsyms = 0;
		wl_event_source_timer_update(group->key_repeat_source, 0);
		return;
	}

	/* The exit confirmation outranks everything, including the global
	 * shortcuts below. It is a modal question with an exclusive grab in all but
	 * name: while it is up, a push-to-talk key that still reached its bridge
	 * would open a microphone the user cannot see they have opened. */
	{
		bool quit_handled = false;
		for (i = 0; i < nsyms; i++)
			quit_handled |= quit_confirm_handle_key(event->state, syms[i]);
		if (quit_handled) {
			/* Disarm the repeat, exactly as the overlay above does, and for
			 * exactly the reason its comment gives.
			 *
			 * The press that DISPATCHED quit armed the repeat timer for that
			 * chord, and every early return here skips the disarm below. So
			 * keyrepeat() kept calling keybinding() -> quit(), which did
			 * nothing while the prompt was up -- and the moment Escape cleared
			 * `active`, the next repeat put the prompt straight back. Escape
			 * appeared not to work and the only way out was to confirm the
			 * exit, which is the worst possible failure for this particular
			 * dialogue. */
			group->nsyms = 0;
			wl_event_source_timer_update(group->key_repeat_source, 0);
			return;
		}
	}

	/* xdg-desktop-portal global shortcuts get first pick (push-to-talk
	 * needs both key edges, and matched keys are not forwarded) */
	if (!locked) {
		bool gs_handled = false;
		for (i = 0; i < nsyms; i++)
			gs_handled |= global_shortcuts_handle_key(event->state, mods,
					syms[i], event->time_msec);
		if (gs_handled)
			return;
	}

	// ov tab mode detect moe key release
	if (config.ov_tab_mode && !selmon->is_jump_mode && !locked &&
		group == kb_group && event->state == WL_KEYBOARD_KEY_STATE_RELEASED &&
		(keycode == 133 || keycode == 37 || keycode == 64 || keycode == 50 ||
		 keycode == 134 || keycode == 105 || keycode == 108 || keycode == 62) &&
		selmon && selmon->sel) {
		if (selmon->isoverview && selmon->sel) {
			toggleoverview(&(Arg){.i = 1});
		}
	}

	if (config.cursor_hide_on_keypress && !cursor_hidden &&
		event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		hidecursor(NULL);
	}

	/* On _press_ if there is no active screen locker,
	 * attempt to process a compositor keybinding. */
	if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED)
		group->dispatching = keycode;
	for (i = 0; i < nsyms; i++)
		handled =
			keybinding(event->state, locked, mods, syms[i], keycode) || handled;
	group->dispatching = 0;

	if (event->state == WL_KEYBOARD_KEY_STATE_RELEASED) {
		tag_combo = false;
	}

	if (handled && group->wlr_group->keyboard.repeat_info.delay > 0) {
		group->mods = mods;
		group->keysyms = syms;
		group->keycode = keycode;
		group->nsyms = nsyms;
		wl_event_source_timer_update(
			group->key_repeat_source,
			group->wlr_group->keyboard.repeat_info.delay);
	} else {
		group->nsyms = 0;
		wl_event_source_timer_update(group->key_repeat_source, 0);
	}

	/* Remember a consumed press, and swallow its matching release.
	 *
	 * Placed AFTER the binding loop so release-binds still get their chance
	 * (a matching one sets `handled` and returns above), and after the
	 * global-shortcuts block, which returns earlier still -- push-to-talk
	 * needs BOTH edges and must never land here. */
	if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		/* Drop entries for keys that are no longer physically down. A release
		 * can be missed outright -- a VT switch or a grab eats one -- and a
		 * stale entry does lasting damage now that client_notify_enter() also
		 * filters on this list: it would hide a genuinely held key from every
		 * future focus change. wlroots updates keycodes[] before emitting this
		 * event, so the key being pressed right now is already in there. */
		struct wlr_keyboard *wlr_kb = &group->wlr_group->keyboard;
		for (i = 0; i < group->nconsumed;) {
			bool held = false;
			for (size_t j = 0; j < wlr_kb->num_keycodes && !held; j++)
				held = wlr_kb->keycodes[j] + 8 == group->consumed[i];
			if (held)
				i++;
			else
				group->consumed[i] = group->consumed[--group->nconsumed];
		}
		if (handled) {
			bool known = false;
			for (i = 0; i < group->nconsumed && !known; i++)
				known = group->consumed[i] == keycode;
			if (!known && group->nconsumed < (int32_t)LENGTH(group->consumed))
				group->consumed[group->nconsumed++] = keycode;
		}
	} else if (event->state == WL_KEYBOARD_KEY_STATE_RELEASED) {
		for (i = 0; i < group->nconsumed; i++) {
			if (group->consumed[i] != keycode)
				continue;
			group->consumed[i] = group->consumed[--group->nconsumed];
			/* the client never saw the press; it must not see the release */
			return;
		}
	}

	if (handled)
		return;

	if (selmon && selmon->is_jump_mode &&
		event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		for (i = 0; i < nsyms; i++) {
			xkb_keysym_t sym = xkb_keysym_to_lower(syms[i]);
			if (sym >= XKB_KEY_a && sym <= XKB_KEY_z) {
				char c_char = 'A' + (sym - XKB_KEY_a);
				Client *c;
				wl_list_for_each(c, &clients, link) {
					if (c->mon == selmon && c->jump_char == c_char) {
						focusclient(c, 1);
						toggleoverview(&(Arg){.i = 1});
						return;
					}
				}
			} else if (sym == XKB_KEY_Escape) {
				togglejump(&(Arg){.i = 0});
				return;
			}
		}
	}

	/* don't pass when popup is focused
	 * this is better than having popups (like fuzzel or wmenu) closing
	 * while typing in a passed keybind */
	pass = (xdg_surface && xdg_surface->role != WLR_XDG_SURFACE_ROLE_POPUP) ||
		   !last_surface
#ifdef XWAYLAND
		   || xsurface
#endif
		;
	/* passed keys don't get repeated */
	if (pass && syms)
		hit_global = keypressglobal(last_surface, &group->wlr_group->keyboard,
									event, mods, syms[0], keycode);

	if (hit_global) {
		return;
	}
	if (!dwl_im_keyboard_grab_forward_key(group, event)) {
		wlr_seat_set_keyboard(seat, &group->wlr_group->keyboard);
		/* Pass unhandled keycodes along to the client. */
		wlr_seat_keyboard_notify_key(seat, event->time_msec, event->keycode,
									 event->state);
	}
}

void keypressmod(struct wl_listener *listener, void *data) {
	/* This event is raised when a modifier key, such as shift or alt, is
	 * pressed. We simply communicate this to the client. */
	KeyboardGroup *group = wl_container_of(listener, group, modifiers);

	if (!dwl_im_keyboard_grab_forward_modifiers(group)) {

		wlr_seat_set_keyboard(seat, &group->wlr_group->keyboard);
		/* Send modifiers to the client. */
		wlr_seat_keyboard_notify_modifiers(
			seat, &group->wlr_group->keyboard.modifiers);
	}

	xkb_layout_index_t current = xkb_state_serialize_layout(
		group->wlr_group->keyboard.xkb_state, XKB_STATE_LAYOUT_EFFECTIVE);

	if (current != group->layout_index) {
		group->layout_index = current;
		printstatus(IPC_WATCH_KB_LAYOUT);
	}
}

void pending_kill_client(Client *c) {
	if (!c || c->iskilling)
		return;
	client_send_close(c);
}

void locksession(struct wl_listener *listener, void *data) {
	struct wlr_session_lock_v1 *session_lock = data;
	SessionLock *lock;
	if (!config.allow_lock_transparent) {
		wlr_scene_node_set_enabled(&locked_bg->node, true);
	}
	if (cur_lock) {
		wlr_session_lock_v1_destroy(session_lock);
		return;
	}
	lock = session_lock->data = ecalloc(1, sizeof(*lock));
	focusclient(NULL, 0);

	lock->scene = wlr_scene_tree_create(layers[LyrBlock]);
	cur_lock = lock->lock = session_lock;
	locked = 1;
	inhibit_portal_screensaver_changed();

	LISTEN(&session_lock->events.new_surface, &lock->new_surface,
		   createlocksurface);
	LISTEN(&session_lock->events.destroy, &lock->destroy, destroysessionlock);
	LISTEN(&session_lock->events.unlock, &lock->unlock, unlocksession);

	wlr_session_lock_v1_send_locked(session_lock);
}

/* scenefx 0.5 replaced per-buffer backdrop blur with explicit blur nodes:
 * keep one node per client, placed below its surface tree. Geometry is kept
 * in sync with the surface in apply_border(). A client-supplied
 * ext-background-effect-v1 region refines the config-driven default. */
/*
 * The opaque area of a surface AND everything it composes from, in the
 * toplevel's coordinates.
 *
 * A client's own opaque_region describes that surface alone. For a client
 * whose content is a SUBSURFACE the toplevel is the client-side decoration
 * frame -- transparent by construction, since it carries the drop shadow and
 * the rounded corners -- and asking it whether the window is opaque gets the
 * answer "not at all" for a window that is opaque everywhere it has pixels.
 */
static void client_accumulate_opaque(struct wlr_surface *s, int sx, int sy,
		void *data) {
	pixman_region32_t *out = data;
	pixman_region32_t r;
	pixman_region32_init(&r);
	pixman_region32_copy(&r, &s->opaque_region);
	/* Clipped to the surface's own extent first: opaque_region is client
	 * input and is not required to fit inside the surface. */
	pixman_region32_intersect_rect(&r, &r, 0, 0, s->current.width,
		s->current.height);
	pixman_region32_translate(&r, sx, sy);
	pixman_region32_union(out, out, &r);
	pixman_region32_fini(&r);
}

void client_update_blur(Client *c) {
	struct background_effect_surface *effect;
	bool want;

	/* the scene tree only exists once the client is mapped; a client may
	 * commit an effect region before that */
	if (!c || !c->scene || !c->scene_surface)
		return;

	effect = background_effect_try_from_surface(client_surface(c));
	want = config.blur && !c->noblur && !c->iskilling;

	/* a fully OPAQUE surface can never show its backdrop: don't keep a blur
	 * node behind it (a CSD window whose buffer doesn't cover the node --
	 * e.g. Electron with allow_csd -- exposed it as a glowing blurred band).
	 * A client effect region or a compositor opacity rule re-enables it. */
	struct wlr_surface *wls = client_surface(c);
	if (want && wls != NULL && (!effect || !effect->has_region) &&
			c->focused_opacity >= 1.0f && c->unfocused_opacity >= 1.0f) {
		/*
		 * ── THE WINDOW, NOT THE SURFACE ───────────────────────────────────
		 *
		 * xdg_surface.set_window_geometry is the client saying which part of
		 * its surface is the window and which part is decoration it drew
		 * around it. Firefox says (20, 20, 3816, 2136) on a 3856x2176
		 * surface: a 20px drop-shadow margin on every side.
		 *
		 * Testing the whole SURFACE for opacity therefore asks whether the
		 * client's own drop shadow is opaque. It is not -- a shadow is a
		 * gradient to nothing -- so the test never fired, and an opaque
		 * browser kept a backdrop blur whose only visible effect was blur
		 * showing through that shadow and through any strip the content had
		 * not repainted yet during a resize. Which is the "glowing blurred
		 * band" this check exists to prevent, arriving by the one route it
		 * did not cover.
		 */
		struct wlr_box wgeo = { 0, 0, wls->current.width,
			wls->current.height };
		if (!client_is_x11(c) && c->surface.xdg != NULL
				&& c->surface.xdg->geometry.width > 0
				&& c->surface.xdg->geometry.height > 0) {
			wgeo = c->surface.xdg->geometry;
		}
		pixman_region32_t whole;
		pixman_region32_init_rect(&whole, wgeo.x, wgeo.y, wgeo.width,
								  wgeo.height);
		/*
		 * The WHOLE surface tree, not just the toplevel. Firefox is the case
		 * that made this necessary: its xdg_toplevel is a transparent CSD
		 * frame and every pixel of page content is a dma-buf subsurface
		 * underneath it, so the toplevel's own opaque_region is nearly empty
		 * and this test kept a blur node behind an opaque browser. Resizing
		 * made it visible -- the frame grows before the content does, and the
		 * strip between them showed the desktop blurred, which is the same
		 * "glowing blurred band" this check was added to stop.
		 */
		pixman_region32_t opaque;
		pixman_region32_init(&opaque);
		wlr_surface_for_each_surface(wls, client_accumulate_opaque, &opaque);
		pixman_region32_t uncovered;
		pixman_region32_init(&uncovered);
		pixman_region32_subtract(&uncovered, &whole, &opaque);
		pixman_region32_fini(&opaque);
		/*
		 * ROUNDED CORNERS ARE NOT A TRANSLUCENT INTERIOR. Firefox's opaque
		 * region insets its top and bottom bands by 8px on each side, so
		 * even against the window geometry a few hundred pixels of corner
		 * remain uncovered. A window whose interior is opaque everywhere
		 * except its corners has no backdrop to show, and the allowance is
		 * expressed against the window's own area so it cannot grow into
		 * one: a genuinely glassy client leaves the whole interior
		 * uncovered, not a percent of it.
		 */
		uint64_t geo_px = (uint64_t)wgeo.width * (uint64_t)wgeo.height;
		uint64_t open_px = 0;
		int nrects = 0;
		const pixman_box32_t *ur =
			pixman_region32_rectangles(&uncovered, &nrects);
		for (int i = 0; i < nrects; i++) {
			open_px += (uint64_t)(ur[i].x2 - ur[i].x1)
				* (uint64_t)(ur[i].y2 - ur[i].y1);
		}
		if (geo_px > 0 && open_px * 100 < geo_px)
			want = false; /* opaque but for its corners -> blur invisible */
		pixman_region32_fini(&uncovered);
		pixman_region32_fini(&whole);
	}

	/* an explicitly empty client region disables blur for this surface */
	if (effect && effect->has_region &&
		!pixman_region32_not_empty(&effect->current_region))
		want = false;

	if (!want) {
		if (c->blur_node) {
			wlr_scene_node_destroy(&c->blur_node->node);
			c->blur_node = NULL;
		}
		return;
	}

	if (!c->blur_node) {
		c->blur_node = wlr_scene_blur_create(c->scene, 0, 0);
		if (!c->blur_node)
			return;
		wlr_scene_node_place_below(&c->blur_node->node,
								   &c->scene_surface->node);
	}

	wlr_scene_blur_set_should_only_blur_bottom_layer(
		c->blur_node, config.blur_optimized && !c->isfloating);
	if (config.animations && c->is_pending_open_animation) {
		/* Show the backdrop blur at full immediately so an opening translucent
		 * window materialises FROSTED rather than flashing the sharp, bright
		 * wallpaper behind it (the window's own opacity still fades in over the
		 * frost). Strength stays at 1 -- same cost as the old alpha ramp, which
		 * already blurred at strength 1 every frame. */
		wlr_scene_blur_set_strength(c->blur_node, 1.0f);
		wlr_scene_blur_set_alpha(c->blur_node, 1.0f);
	}
	wlr_scene_node_set_position(&c->blur_node->node, c->bw, c->bw);
	wlr_scene_blur_set_size(c->blur_node, GEZERO(c->geom.width - 2 * c->bw),
							GEZERO(c->geom.height - 2 * c->bw));

	/* Verbatim, corners and all -- see layer_update_blur.
	 *
	 * This used to take pixman_region32_extents() and hand the bounding box to
	 * wlr_scene_blur_set_clipped_region(). The two other producers of the very
	 * same protocol data -- layer_update_blur and popup_update_blur -- have
	 * always passed the region itself, and the blur node documents clip_region
	 * as "e.g. the client's ext-background-effect region": a toplevel was the
	 * only surface kind whose region was thrown away, and nothing here ever
	 * said why. The bounding box is what puts square blur "ears" outside a
	 * rounded card, and a toplevel has the same corners a layer surface does.
	 *
	 * clipped_region is reset either way: it is the LOWER-precedence field, so
	 * leaving a stale box in it would decide the clip for every later frame in
	 * which the client withdraws its region. */
	wlr_scene_blur_set_clipped_region(c->blur_node,
									  clipped_region_get_default());
	wlr_scene_blur_set_region(
		c->blur_node,
		effect && effect->has_region ? &effect->current_region : NULL);
}

void init_client_properties(Client *c) {
	c->grid_col_per = 1.0f;
	c->grid_row_per = 1.0f;
	c->is_monocle_hide = false;
	c->jump_label_node = NULL;
	c->titlebar_node = NULL;
	c->titlebar_close_node = NULL;
	c->ov_icon = NULL;
	c->ov_title = NULL;
	c->ov_snap_buf = NULL;
	c->ov_clip_active = false;
	c->blur_node = NULL;
	c->shadow_blur = NULL;
	c->shadow_tree = NULL;
	c->overview_scene_surface = NULL;
	c->drop_direction = UNDIR;
	c->enable_drop_area_draw = false;
	c->isfocused = false;
	c->isfloating = 0;
	c->isfakefullscreen = 0;
	c->isnoanimation = 0;
	c->isopensilent = 0;
	c->istagsilent = 0;
	c->noswallow = 0;
	c->isterm = 0;
	c->noblur = 0;
	c->tearing_hint = 0;
	c->overview_isfullscreenbak = 0;
	c->overview_ismaximizescreenbak = 0;
	c->overview_isfloatingbak = 0;
	c->pid = 0;
	c->swallowing = NULL;
	c->swallowedby = NULL;
	c->ismaster = 0;
	c->old_ismaster = 0;
	c->isleftstack = 0;
	c->ismaximizescreen = 0;
	c->isfullscreen = 0;
	c->need_float_size_reduce = 0;
	c->iskilling = 0;
	c->istagswitching = 0;
	c->isglobal = 0;
	c->isminimized = 0;
	c->isoverlay = 0;
	c->isunglobal = 0;
	c->is_in_scratchpad = 0;
	c->isnamedscratchpad = 0;
	c->is_scratchpad_show = 0;
	c->special_name = NULL;
	c->need_float_size_reduce = 0;
	c->is_clip_to_hide = 0;
	c->is_overview_hidden = false;
	c->is_restoring_from_ov = 0;
	c->isurgent = 0;
	c->need_output_flush = 0;
	c->scroller_proportion = config.scroller_default_proportion;
	c->is_pending_open_animation = true;
	c->drag_to_tile = false;
	c->scratchpad_switching_mon = false;
	c->fake_no_border = false;
	c->focused_opacity = config.focused_opacity;
	c->unfocused_opacity = config.unfocused_opacity;
	/* No global setting to read: ADR-006 forbids one. A window's luminance is
	 * either 1.0 or something a rule said about that window. */
	c->sdr_white_scale = 1.0f;
	c->hdr_gain = 1.0f;
	c->luminance_domain = NULL;
	c->presentation_class = NULL;
	c->commit_count = 0;
	c->commit_last_ns = 0;
	c->commit_interval_sum_ns = 0;
	c->commit_interval_n = 0;
	c->nofocus = 0;
	c->nofadein = 0;
	c->nofadeout = 0;
	c->no_force_center = 0;
	c->isnoborder = 0;
	c->isnosizehint = 0;
	c->isnoradius = 0;
	c->isnoshadow = 0;
	c->noscanout = 0;
	c->ignore_maximize = 1;
	c->ignore_minimize = 1;
	c->iscustomsize = 0;
	c->iscustompos = 0;
	c->iscustom_scroller_proportion = 0;
	c->iscustom_scroller_proportion_single = 0;
	c->master_mfact_per = 0.0f;
	c->master_inner_per = 0.0f;
	c->stack_inner_per = 0.0f;
	c->old_stack_inner_per = 0.0f;
	c->old_master_inner_per = 0.0f;
	c->old_master_mfact_per = 0.0f;
	c->isterm = 0;
	c->allow_csd = 0;
	c->force_ssd = 0;
	c->force_fakemaximize = 0;
	c->force_tiled_state = 1;
	c->force_tearing = 0;
	c->allow_shortcuts_inhibit = SHORTCUTS_INHIBIT_ENABLE;
	c->idleinhibit_when_focus = 0;
	c->scroller_proportion_single = 0.0f;
	c->float_geom.width = 0;
	c->float_geom.height = 0;
	c->float_geom.x = 0;
	c->float_geom.y = 0;
	c->stack_proportion = 0.0f;
	memset(c->oldmonname, 0, sizeof(c->oldmonname));
	memcpy(c->opacity_animation.initial_border_color, config.bordercolor,
		   sizeof(c->opacity_animation.initial_border_color));
	memcpy(c->opacity_animation.current_border_color, config.bordercolor,
		   sizeof(c->opacity_animation.current_border_color));
	c->opacity_animation.initial_opacity = c->unfocused_opacity;
	c->opacity_animation.current_opacity = c->unfocused_opacity;
}

void // old fix to 0.5
mapnotify(struct wl_listener *listener, void *data) {
	/* Called when the surface is mapped, or ready to display on-screen. */
	Client *at_client = NULL;
	Client *c = wl_container_of(listener, c, map);
	int32_t i = 0;

	c->id = generate_client_id();

	/* Create scene tree for this client and its border */
	c->scene = client_surface(c)->data = wlr_scene_tree_create(layers[LyrTile]);
	wlr_scene_node_set_enabled(&c->scene->node, c->type != XDGShell);
	c->scene_surface =
		c->type == XDGShell
			? wlr_scene_xdg_surface_create(c->scene, c->surface.xdg)
			: wlr_scene_subsurface_tree_create(c->scene, client_surface(c));
	c->scene->node.data = c->scene_surface->node.data = c;

	/* BEFORE client_get_geometry, not after. The X window's current size is
	 * in the client's own units, and this is what decides whether those units
	 * are logical or raw pixels -- so reading the geometry first would take a
	 * pixel size for a logical one and open the window 1.25x too large. The
	 * client has no monitor yet, so this uses selmon; setmon calls it again
	 * once the real one is known.
	 *
	 * This is also why an X11 app comes out physically smaller with the
	 * option on: it asked for 400 of what it believes are screen units, and
	 * with the option on those are device pixels. That is the same trade
	 * Hyprland's force_zero_scaling makes, and it is stated in the option's
	 * own description rather than hidden. */
	client_update_x11_scale(c);

	client_get_geometry(c, &c->geom);

	if (client_is_x11(c))
		init_client_properties(c);

	// set special window properties
	if (client_is_unmanaged(c) || client_is_x11_popup(c)) {
		c->bw = 0;
		c->isnoborder = 1;
	} else {
		c->bw = config.borderpx;
	}

	if (client_should_global(c)) {
		c->isunglobal = 1;
	}

	// init client geom
	c->geom.width += 2 * c->bw;
	c->geom.height += 2 * c->bw;
	c->overview_backup_geom = c->geom;

	/* Handle unmanaged clients first so we can return prior create borders
	 */
#ifdef XWAYLAND
	if (client_is_unmanaged(c)) {
		/* Unmanaged clients always are floating */
		fix_xwayland_coordinate(&c->geom);
		wlr_scene_node_set_position(&c->scene->node, c->geom.x, c->geom.y);
		client_x11_configure(c, c->geom.x, c->geom.y, c->geom.width,
							 c->geom.height);
		LISTEN(&c->surface.xwayland->events.set_geometry, &c->set_geometry,
			   setgeometrynotify);
		wlr_scene_node_reparent(&c->scene->node, layers[LyrOverlay]);
		if (client_wants_focus(c)) {
			focusclient(c, 1);
			exclusive_focus = c;
		}
		return;
	}
#endif
	// extra node

	for (i = 0; i < 2; i++) {
		c->splitindicator[i] = wlr_scene_rect_create(
			c->scene, 0, 0,
			c->isurgent ? config.urgentcolor : config.splitcolor);
		c->splitindicator[i]->node.data = c;
		wlr_scene_node_lower_to_bottom(&c->splitindicator[i]->node);
		wlr_scene_node_set_enabled(&c->splitindicator[i]->node, false);
	}

	client_add_titlebar(c);

	c->droparea = wlr_scene_rect_create(c->scene, 0, 0, config.dropcolor);
	wlr_scene_node_lower_to_bottom(&c->droparea->node);
	wlr_scene_node_set_position(&c->droparea->node, 0, 0);
	wlr_scene_node_set_enabled(&c->droparea->node, false);

	c->shield =
		wlr_scene_rect_create(c->scene, 0, 0, (float[4]){0, 0, 0, 0xff});
	c->shield->node.data = c;
	wlr_scene_node_lower_to_bottom(&c->shield->node);
	wlr_scene_node_set_enabled(&c->shield->node, false);

	c->border = wlr_scene_rect_create(
		c->scene, 0, 0, c->isurgent ? config.urgentcolor : config.bordercolor);
	wlr_scene_node_lower_to_bottom(&c->border->node);
	wlr_scene_node_set_position(&c->border->node, 0, 0);
	/* Start disabled: the border rect has no clipped_region until apply_border
	 * runs, so if it rendered on the window's first frame it would fill the
	 * whole window with the border/focus colour for one frame (the open/close
	 * "flash"). apply_border enables it once the interior cut-out is set. */
	wlr_scene_node_set_enabled(&c->border->node, false);
	wlr_scene_rect_set_corner_radii(
		c->border,
		corner_radii_from_location(config.border_radius,
								   config.border_radius_location_default));
	wlr_scene_node_set_enabled(&c->border->node, true);

	/* On LyrTileShadow to begin with -- a window is tiled until something
	 * says otherwise, and client_sync_shadow_tree() moves it into c->scene
	 * on the first draw if it turns out to be floating. */
	c->shadow_tree = wlr_scene_tree_create(layers[LyrTileShadow]);
	wlr_scene_node_set_position(&c->shadow_tree->node, c->scene->node.x,
								c->scene->node.y);
	wlr_scene_node_set_enabled(&c->shadow_tree->node, c->scene->node.enabled);

	c->shadow =
		wlr_scene_shadow_create(c->shadow_tree, 0, 0, config.border_radius,
								config.shadows_blur, config.shadowscolor);

	wlr_scene_node_lower_to_bottom(&c->shadow->node);
	wlr_scene_node_set_enabled(&c->shadow->node, true);

	/* tight dark contact layer above the soft ambient shadow */
	c->contact_shadow = wlr_scene_shadow_create(
		c->shadow_tree, 0, 0, config.border_radius, config.shadows_contact_blur,
		config.shadowscolor_contact);
	wlr_scene_node_lower_to_bottom(&c->contact_shadow->node);
	wlr_scene_node_place_above(&c->contact_shadow->node, &c->shadow->node);
	wlr_scene_node_set_enabled(&c->contact_shadow->node,
							   config.shadows_contact);

	if (config.new_is_master && selmon && !is_scroller_layout(selmon))
		// tile at the top
		wl_list_insert(&clients, &c->link); // new window is master, push at the head
	else if (selmon && is_scroller_layout(selmon) &&
			 selmon->visible_scroll_tiling_clients > 0) {

		if (selmon->sel && ISSCROLLTILED(selmon->sel) &&
			VISIBLEON(selmon->sel, selmon)) {
			at_client = scroll_get_stack_tail_client(selmon->sel);
		} else {
			at_client = center_tiled_select(selmon);
		}

		if (at_client) {
			wl_list_insert(&at_client->link, &c->link);
		} else {
			wl_list_insert(clients.prev, &c->link); // push at the tail
		}
	} else
		wl_list_insert(clients.prev, &c->link); // push at the tail

	wl_list_insert(&fstack, &c->flink);

	applyrules(c);
	client_set_prevent_scanout(c, c->noscanout);

	if (!c->isfloating || c->force_tiled_state) {
		client_set_tiled(c, WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT |
								WLR_EDGE_RIGHT);
	}

	// apply buffer effects of client
	client_update_blur(c);
	wlr_scene_node_set_position(&c->scene_surface->node, c->bw, c->bw);

	// set border color
	setborder_color(c);

	if (c->mon && c->mon->isoverview && config.ov_no_resize) {
		overview_backup_surface(c);
	}

	// make sure the animation is open type
	c->is_pending_open_animation = true;
	resize(c, c->geom, 0);

	/*
	 * ── AND TELL IT WHAT ITS DISPLAY PREFERS, NOW THAT IT IS MAPPED ───────
	 *
	 * setmon() already does this, but setmon runs while the surface is still
	 * unmapped, and the send there is gated on `s->mapped` -- correctly, since
	 * an unmapped surface is on no output. The consequence was that a client
	 * learned NOTHING at startup: it read whatever wlroots defaults to
	 * (GAMMA22) and kept it until something moved the window to another output
	 * and setmon fired again. Measured with a wp-cm observer: the first
	 * description read after map was tf=2, and only a move produced the
	 * correct tf=14.
	 *
	 * Here is the first moment both halves are true -- the surface is mapped
	 * and it has an output -- so this is where the answer is finally sayable.
	 */
	if (c->mon != NULL) {
		struct wlr_surface *s = client_surface(c);
		if (s != NULL && s->mapped) {
			surface_send_preferred_description(s, c->mon);
		}
	}
	printstatus(IPC_WATCH_ARRANGGE);
}

void maximizenotify(struct wl_listener *listener, void *data) {

	Client *c = wl_container_of(listener, c, maximize);

	if (!c || !c->mon || c->iskilling || c->ignore_maximize)
		return;

	if (!client_is_x11(c) && !c->surface.xdg->initialized) {
		return;
	}

	if (client_request_maximize(c, data)) {
		setmaximizescreen(c, 1, true);
	} else {
		setmaximizescreen(c, 0, true);
	}
}

void unminimize(Client *c) {
	if (c && c->is_in_scratchpad && c->is_scratchpad_show) {
		client_pending_minimized_state(c, 0);
		c->is_scratchpad_show = 0;
		c->is_in_scratchpad = 0;
		c->isnamedscratchpad = 0;
		setborder_color(c);
		return;
	}

	if (c && c->isminimized) {
		show_hide_client(c);
		c->is_scratchpad_show = 0;
		c->is_in_scratchpad = 0;
		c->isnamedscratchpad = 0;
		setborder_color(c);
		arrange(c->mon, false, false);
		return;
	}
}

void set_minimized(Client *c) {

	if (!c || !c->mon)
		return;

	c->isglobal = 0;
	c->ispinned = 0;
	c->oldtags = c->mon->tagset[c->mon->seltags];
	c->mini_restore_tag = c->tags;
	c->tags = 0;
	client_pending_minimized_state(c, 1);
	c->is_in_scratchpad = 1;
	c->is_scratchpad_show = 0;
	focusclient(focustop(selmon), 1);
	arrange(c->mon, false, false);

	if (c->foreign_toplevel)
		wlr_foreign_toplevel_handle_v1_set_activated(c->foreign_toplevel,
													 false);

	wl_list_remove(&c->link);				// remove from its old position
	wl_list_insert(clients.prev, &c->link); // insert at the tail
}

void minimizenotify(struct wl_listener *listener, void *data) {

	Client *c = wl_container_of(listener, c, minimize);

	if (!c || !c->mon || c->iskilling || c->isminimized)
		return;

	if (client_request_minimize(c, data) && !c->ignore_minimize) {
		if (!c->isminimized)
			set_minimized(c);
		client_set_minimized(c, true);
	} else {
		if (c->isminimized)
			unminimize(c);
		client_set_minimized(c, false);
	}
}

void motionabsolute(struct wl_listener *listener, void *data) {
	/* This event is forwarded by the cursor when a pointer emits an
	 * _absolute_ motion event, from 0..1 on each axis. This happens, for
	 * example, when wlroots is running under a Wayland window rather than
	 * KMS+DRM, and you move the mouse over the window. You could enter the
	 * window from any edge, so we have to warp the mouse there. There is
	 * also some hardware which emits these events. */
	struct wlr_pointer_motion_absolute_event *event = data;
	double lx, ly, dx, dy;

	if (check_trackpad_disabled(event->pointer)) {
		return;
	}

	if (!event->time_msec) /* this is 0 with virtual pointer */
		wlr_cursor_warp_absolute(cursor, &event->pointer->base, event->x,
								 event->y);

	wlr_cursor_absolute_to_layout_coords(cursor, &event->pointer->base,
										 event->x, event->y, &lx, &ly);
	dx = lx - cursor->x;
	dy = ly - cursor->y;
	motionnotify(event->time_msec, &event->pointer->base, dx, dy, dx, dy);
}

void resize_floating_window(Client *grabc) {
	int cdx = (int)round(cursor->x) - grabcx;
	int cdy = (int)round(cursor->y) - grabcy;

	cdx = !(rzcorner & 1) && grabc->geom.width - 2 * (int)grabc->bw - cdx < 1
			  ? 0
			  : cdx;
	cdy = !(rzcorner & 2) && grabc->geom.height - 2 * (int)grabc->bw - cdy < 1
			  ? 0
			  : cdy;

	const struct wlr_box box = {
		.x = grabc->geom.x + (rzcorner & 1 ? 0 : cdx),
		.y = grabc->geom.y + (rzcorner & 2 ? 0 : cdy),
		.width = grabc->geom.width + (rzcorner & 1 ? cdx : -cdx),
		.height = grabc->geom.height + (rzcorner & 2 ? cdy : -cdy)};

	grabc->float_geom = box;

	resize(grabc, box, 1);
	grabcx += cdx;
	grabcy += cdy;
}

static void cursor_zoom_apply(Monitor *m) {
	if (!m->scene_output)
		return;
	/*
	 * ── AVK DOES NOT IMPLEMENT OUTPUT MAGNIFICATION ──────────────────────
	 *
	 * az_avk_output_supported() refuses an output whose scene zoom is above 1,
	 * and a refusal is fatal now -- so setting the zoom here would end the
	 * session on the next frame, from a keybind, with the reason three layers
	 * away from the thing the user pressed.
	 *
	 * Declined at the input instead, and said out loud. This is NOT the GLES
	 * fallback returning by another door: nothing is composited by anyone
	 * else, the zoom simply does not happen. The honest fix is magnification
	 * in AVK; until that exists, "asteroidz cannot do this" is a better answer
	 * than a dead desktop.
	 */
	if (m->cursor_zoom > 1.0f) {
		static bool warned = false;
		if (!warned) {
			warned = true;
			wlr_log(WLR_ERROR, "output magnification is not implemented by AVK "
				"-- the zoom request on %s is ignored",
				m->wlr_output != NULL ? m->wlr_output->name : "(output)");
		}
		m->cursor_zoom = 1.0f;
		wlr_scene_output_set_zoom(m->scene_output, 1.0f, m->zoom_cx, m->zoom_cy);
		return;
	}
	wlr_scene_output_set_zoom(m->scene_output, m->cursor_zoom, m->zoom_cx,
							  m->zoom_cy);
}

/* Apply the runtime zoom factor to the monitor under the cursor and
 * release every other one */
void cursor_zoom_update(void) {
	Monitor *m, *zoom_mon = NULL;
	bool active, was_active;

	if (cursor_zoom_factor > 1.0f)
		zoom_mon = xytomon(cursor->x, cursor->y);

	wl_list_for_each(m, &mons, link) {
		active = m == zoom_mon;
		was_active = m->cursor_zoom > 1.0f;

		if (active != m->zoom_cursor_locked) {
			/* hardware cursor planes are not magnified */
			wlr_output_lock_software_cursors(m->wlr_output, active);
			m->zoom_cursor_locked = active;
		}

		if (!active) {
			m->cursor_zoom = 1.0f;
			cursor_zoom_apply(m);
			continue;
		}

		/* snap the view to the cursor; when non-rigid the per-frame lerp
		 * in rendermon chases it instead */
		if (!was_active || config.cursor_zoom_rigid) {
			m->zoom_cx = cursor->x - m->m.x;
			m->zoom_cy = cursor->y - m->m.y;
		}
		m->cursor_zoom = cursor_zoom_factor;
		cursor_zoom_apply(m);
	}
}

void cursor_zoom_set_factor(float factor) {
	cursor_zoom_factor = CLAMP_FLOAT(factor, 1.0f, 8.0f);
	cursor_zoom_update();
}

/* Per-frame view update: rigid views stick to the cursor, lazy views
 * chase it a step each frame until they converge */
void cursor_zoom_frame(Monitor *m) {
	double tx, ty, dx, dy;

	if (m->cursor_zoom <= 1.0f)
		return;

	tx = cursor->x - m->m.x;
	ty = cursor->y - m->m.y;

	if (config.cursor_zoom_rigid) {
		m->zoom_cx = tx;
		m->zoom_cy = ty;
	} else {
		dx = tx - m->zoom_cx;
		dy = ty - m->zoom_cy;
		if (fabs(dx) < 0.5 && fabs(dy) < 0.5) {
			m->zoom_cx = tx;
			m->zoom_cy = ty;
		} else {
			m->zoom_cx += dx * 0.2;
			m->zoom_cy += dy * 0.2;
		}
	}
	cursor_zoom_apply(m);
}

void motionnotify(uint32_t time, struct wlr_input_device *device, double dx,
				  double dy, double dx_unaccel, double dy_unaccel) {
	double sx = 0, sy = 0, sx_confined, sy_confined;
	Client *c = NULL, *w = NULL;
	Client *closet_drop_client = NULL;
	LayerSurface *l = NULL;
	struct wlr_surface *surface = NULL;
	bool should_lock = false;

	/* time is 0 in internal calls meant to restore pointer focus. */
	if (time == 0) {
		az_pointer_notify_internal++;
	}
	if (time) {
		wlr_relative_pointer_manager_v1_send_relative_motion(
			relative_pointer_mgr, seat, (uint64_t)time * 1000, dx, dy,
			dx_unaccel, dy_unaccel);

		/* Not while the screenshot overlay is up. Opening it does not clear
		 * pointer focus, so a client that had the pointer LOCKED -- a game,
		 * typically -- still holds it, and the early return below would pin
		 * the crosshair in place for as long as the overlay lived. The
		 * overlay's own confinement replaces it for the duration. */
		if (active_constraint && !shotui.active && cursor_mode != CurResize &&
			cursor_mode != CurMove) {
			if (active_constraint->surface ==
				seat->pointer_state.focused_surface) {

				if (active_constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED)
					return;

				toplevel_from_wlr_surface(active_constraint->surface, &c, NULL);
				if (c) {
					/* BOUNDARY 4, the half that has no picture attached to
					 * it. The confine region belongs to the SURFACE and is in
					 * its coordinates -- raw pixels for an X11 client being
					 * sized in them -- while the cursor and the deltas are
					 * layout-logical. Left unconverted, a game's confinement
					 * rectangle would cover 1/1.25 of the window it was meant
					 * for and the pointer would stop 20% short of the right
					 * and bottom edges. `s` is 1 for every other client, so
					 * this is arithmetic-neutral everywhere else. */
					float s = client_x11_scale(c);
					sx = (cursor->x - c->geom.x - c->bw) * s;
					sy = (cursor->y - c->geom.y - c->bw) * s;
					if (wlr_region_confine(&active_constraint->region, sx, sy,
										   sx + dx * s, sy + dy * s,
										   &sx_confined, &sy_confined)) {
						dx = (sx_confined - sx) / s;
						dy = (sy_confined - sy) / s;
					}
				}
			}
		}

		wlr_cursor_move(cursor, device, dx, dy);
		handlecursoractivity();
		wlr_idle_notifier_v1_notify_activity(idle_notifier, seat);
		wake_sleeping_monitors();

		/* Update selmon (even while dragging a window) -- but not under the
		 * screenshot overlay, which is modal: nothing below it is focusable,
		 * and letting the pointer's monitor lead selmon there would leave the
		 * selection behind on the captured output and, worse, aim the NEXT
		 * screenshot at a monitor the user never focused. */
		if (config.sloppyfocus && !shotui.active)
			set_selmon(xytomon(cursor->x, cursor->y));

		/* overview: hovering a strip tile previews that tag in the main area */
		if (selmon && selmon->isoverview)
			overview_pointer_preview(selmon, cursor->x, cursor->y);
	}

	/* After every mover, not inside the block above: the tablet path warps the
	 * cursor itself and then calls in here with time 0, so a clamp under
	 * `if (time)` would let a stylus walk straight off the captured output. */
	screenshot_ui_confine_cursor();

	cursor_zoom_update();

	if (shotui.active) {
		if (time)
			screenshot_ui_handle_motion();
		return;
	}

	/* Find the client under the pointer and send the event along. */
	xytonode(cursor->x, cursor->y, &surface, &c, NULL, &sx, &sy);

	/* overview: outline the big-area window under the pointer */
	if (selmon && selmon->isoverview)
		overview_hover_highlight(selmon, c);

	if (cursor_mode == CurPressed && !seat->drag &&
		surface != seat->pointer_state.focused_surface &&
		toplevel_from_wlr_surface(seat->pointer_state.focused_surface, &w,
								  &l) >= 0) {
		c = w;
		surface = seat->pointer_state.focused_surface;
		sx = cursor->x - (l ? l->scene->node.x : w->geom.x);
		sy = cursor->y - (l ? l->scene->node.y : w->geom.y);
	}

	/* Update drag icon's position */
	wlr_scene_node_set_position(&drag_icon->node, (int32_t)round(cursor->x),
								(int32_t)round(cursor->y));

	/* If we are currently grabbing the mouse, handle and return */
	if (cursor_mode == CurMove) {
		/* Move the grabbed client to the new position. */
		grabc->iscustomsize = 1;
		grabc->float_geom =
			(struct wlr_box){.x = (int32_t)round(cursor->x) - grabcx,
							 .y = (int32_t)round(cursor->y) - grabcy,
							 .width = grabc->geom.width,
							 .height = grabc->geom.height};
		if (config.drag_tile_to_tile && grabc->drag_to_tile) {
			closet_drop_client = find_closest_tiled_client(grabc);
			if (closet_drop_client && dropc && closet_drop_client != dropc) {
				dropc->enable_drop_area_draw = false;
				client_set_drop_area(dropc);
				dropc = closet_drop_client;
				dropc->enable_drop_area_draw = true;
				client_set_drop_area(dropc);
			} else if (closet_drop_client) {
				dropc = closet_drop_client;
				dropc->enable_drop_area_draw = true;
				client_set_drop_area(dropc);
			} else if (dropc) {
				dropc->enable_drop_area_draw = false;
				client_set_drop_area(dropc);
				dropc = NULL;
			}
		}
		resize(grabc, grabc->float_geom, 1);
		return;
	} else if (cursor_mode == CurResize) {
		if (grabc->isfloating) {
			grabc->iscustomsize = 1;
			if (last_apply_drap_time == 0 ||
				time - last_apply_drap_time >
					config.drag_floating_refresh_interval) {
				resize_floating_window(grabc);
				last_apply_drap_time = time;
			}
			return;
		} else {
			resize_tile_client(grabc, true, 0, 0, time);
		}
	}

	/* If there's no client surface under the cursor, set the cursor image
	 * to a default. This is what makes the cursor image appear when you
	 * move it off of a client or over its border. */
	if (!surface && !seat->drag && !cursor_hidden)
		az_cursor_set_xcursor("default");

	if (c && c->mon && !c->animation.running &&
		(INSIDEMON(c) || !ISSCROLLTILED(c))) {
		scroller_focus_lock = 0;
	}

	should_lock = false;
	double speed = 0.0f;

	if (config.edge_scroller_pointer_focus) {
		speed = sqrt(dx * dx + dy * dy);
	}

	/* overview: focus the window under the cursor so its border shows the theme
	 * focus colour (the subtle hover highlight); bypass the scroller edge-focus
	 * lock that otherwise suppresses focus in a scroller layout */
	if (selmon && selmon->isoverview) {
		pointerfocus(c, surface, sx, sy, time);
		return;
	}

	if (!scroller_focus_lock || !(c && c->mon && !INSIDEMON(c))) {
		if (c && c->mon && ISSCROLLTILED(c) && is_scroller_layout(c->mon) &&
			!INSIDEMON(c)) {
			should_lock = true;
		}

		if (!((!config.edge_scroller_pointer_focus ||
			   speed < config.edge_scroller_focus_allow_speed) &&
			  c && c->mon && ISSCROLLTILED(c) && is_scroller_layout(c->mon) &&
			  !INSIDEMON(c))) {
			pointerfocus(c, surface, sx, sy, time);
		}

		if (should_lock && c && c->mon && ISTILED(c) && c == c->mon->sel) {
			scroller_focus_lock = 1;
		}
	}
}

void motionrelative(struct wl_listener *listener, void *data) {
	/* This event is forwarded by the cursor when a pointer emits a
	 * _relative_ pointer motion event (i.e. a delta) */
	struct wlr_pointer_motion_event *event = data;
	/* The cursor doesn't move unless we tell it to. The cursor
	 * automatically handles constraining the motion to the output layout,
	 * as well as any special configuration applied for the specific input
	 * device which generated the event. You can pass NULL for the device if
	 * you want to move the cursor around without any input. */

	if (check_trackpad_disabled(event->pointer)) {
		return;
	}

	motionnotify(event->time_msec, &event->pointer->base, event->delta_x,
				 event->delta_y, event->unaccel_dx, event->unaccel_dy);
	toggle_hotarea(cursor->x, cursor->y);
	check_scroller_edge_scroll((int32_t)cursor->x, (int32_t)cursor->y);
}

void outputmgrapply(struct wl_listener *listener, void *data) {
	struct wlr_output_configuration_v1 *config = data;
	outputmgrapplyortest(config, 0);
}

void // 0.7 custom
outputmgrapplyortest(struct wlr_output_configuration_v1 *output_config, int32_t test) {
	/*
	 * Called when a client such as wlr-randr requests a change in output
	 * configuration. This is only one way that the layout can be changed,
	 * so any Monitor information should be updated by updatemons() after an
	 * output_layout.change event, not here.
	 */
	struct wlr_output_configuration_head_v1 *config_head;
	int32_t ok = 1;

	wl_list_for_each(config_head, &output_config->heads, link) {
		struct wlr_output *wlr_output = config_head->state.output;
		Monitor *m = wlr_output->data;
		struct wlr_output_state state;

		/* Ensure displays previously disabled by
		 * wlr-output-power-management-v1 are properly handled. A disable
		 * through *this* (output-management config) protocol is a deliberate
		 * "keep this output off" action, not a DPMS sleep -- so asleep stays
		 * 0 and wake-on-input does NOT bring it back. DPMS-style idle-off
		 * that should wake on input goes through the dpms_off_monitor
		 * dispatcher instead, which sets asleep=1. */
		m->asleep = 0;

		wlr_output_state_init(&state);
		wlr_output_state_set_enabled(&state, config_head->state.enabled);
		if (!config_head->state.enabled)
			goto apply_or_test;

		if (config_head->state.mode)
			wlr_output_state_set_mode(&state, config_head->state.mode);
		else
			wlr_output_state_set_custom_mode(
				&state, config_head->state.custom_mode.width,
				config_head->state.custom_mode.height,
				config_head->state.custom_mode.refresh);

		wlr_output_state_set_transform(&state, config_head->state.transform);
		wlr_output_state_set_scale(&state, config_head->state.scale);
		wlr_output_state_set_adaptive_sync_enabled(
			&state, config_head->state.adaptive_sync_enabled);

	apply_or_test: {
		bool head_committed = test ? wlr_output_test_state(wlr_output, &state)
								  : wlr_output_commit_state(wlr_output, &state);
		ok &= head_committed;
		/* ADR-604 trigger 2: a successful commit carrying mode, scale,
		 * transform or adaptive sync. A TEST commits nothing and must not
		 * reset -- the epoch would then bump on a query. */
		if (!test && head_committed && m != NULL) {
			az_presenter_reset(m, AZ_PRESENT_RESET_MODE);
		}

		/* clients like DMS's own idle-monitor-off feature (and wlr-randr)
		 * enable/disable outputs through wlr-output-management-v1 instead
		 * of wlr_output_power_manager_v1, bypassing powermgrsetmode()
		 * entirely; give this path the same DSC-decoder recovery safety
		 * net so a DMS-driven sleep/wake doesn't leave the panel wedged */
		if (!test) {
			if (config_head->state.enabled &&
				(config.dpms_wake_retrain || !head_committed))
				monitor_start_retrain(m, head_committed ? 700 : 50);
			else if (!config_head->state.enabled && !head_committed)
				monitor_start_retrain(m, 50);
		}
	}

		/* Don't move monitors if position wouldn't change, this to avoid
		 * wlroots marking the output as manually configured.
		 * wlr_output_layout_add does not like disabled outputs */
		if (!test && wlr_output->enabled &&
			(m->m.x != config_head->state.x || m->m.y != config_head->state.y))
			wlr_output_layout_add(output_layout, wlr_output,
								  config_head->state.x, config_head->state.y);

		wlr_output_state_finish(&state);
	}

	if (ok)
		wlr_output_configuration_v1_send_succeeded(output_config);
	else
		wlr_output_configuration_v1_send_failed(output_config);
	wlr_output_configuration_v1_destroy(output_config);

	/* https://codeberg.org/dwl/dwl/issues/577 */
	updatemons(NULL, NULL);
}

void outputmgrtest(struct wl_listener *listener, void *data) {
	struct wlr_output_configuration_v1 *config = data;
	outputmgrapplyortest(config, 1);
}

void pointerfocus(Client *c, struct wlr_surface *surface, double sx, double sy,
				  uint32_t time) {
	struct timespec now;

	/* Float layout defaults to click-to-focus (float_click_to_focus, on by
	 * default; set layout/floating/click-to-focus 0 to opt out): overlapping
	 * floating windows make focus-follows-mouse maddening — crossing the
	 * pointer over a stack keeps stealing focus and, per focusclient(),
	 * auto-raising. Suppress the sloppy pointer-focus here; a click still
	 * focuses+raises via handle_buttonpress. Pointer enter/motion events
	 * below are unaffected. */
	if (config.sloppyfocus && !start_drag_window && c && time && c->scene &&
		c->scene->node.enabled && !c->animation.tagining &&
		!(config.float_click_to_focus && c->mon && is_float_layout(c->mon)) &&
		(surface != seat->pointer_state.focused_surface ||
		 (selmon && selmon->isoverview && selmon->sel != c)) &&
		!client_is_unmanaged(c) && VISIBLEON(c, c->mon))
		focusclient(c, 0);

	/* If surface is NULL, clear pointer focus */
	if (!surface) {
		az_pointer_focus_clears++;
		wlr_seat_pointer_notify_clear_focus(seat);
		return;
	}

	if (!time) {
		clock_gettime(CLOCK_MONOTONIC, &now);
		time = now.tv_sec * 1000 + now.tv_nsec / 1000000;
	}

	/* Let the client know that the mouse cursor has entered one
	 * of its surfaces, and make keyboard focus follow if desired.
	 * wlroots makes this a no-op if surface is already focused */

	if (!c || !c->mon || !c->mon->isoverview) {
		// don't let window get pointer focus,
		// avoid game window force grab pointer in overview mode
		az_pointer_enters++;
		wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
	}

	az_pointer_motions++;
	wlr_seat_pointer_notify_motion(seat, time, sx, sy);
}

// modified printstatus function to accept a mask parameter
void set_selmon(Monitor *m) {
	if (m == selmon)
		return;
	selmon = m;
	/* ALL_MONITORS rather than a narrower type: what changed is which output
	 * is `active`, which is a property of every monitor in the reply, not of
	 * the focused client or of any tag. */
	printstatus(IPC_WATCH_ALL_MONITORS);
}

void printstatus(enum ipc_watch_type type) {
	wl_signal_emit(&asteroidz_print_status, &type);
}

static int monitor_retrain_step(void *data) {
	Monitor *m = data;
	struct wlr_output_state state;
	struct wlr_output_mode *cur, *alt = NULL, *mode;

	/* Deliberately not gated on m->wlr_output->enabled: this can run as a
	 * recovery attempt after a DPMS-on commit that itself failed to bring
	 * the output back (see powermgrsetmode()), in which case the output
	 * may still be reporting disabled. */
	if (!m || !m->wlr_output || m->iscleanuping)
		return 0;

	wlr_output_state_init(&state);
	if (m->retrain_phase == 0) {
		/* phase 0: modeset to the lowest-refresh mode at the same
		 * resolution, like the console does on a VT switch */
		cur = m->wlr_output->current_mode;
		if (!cur) {
			wlr_log(WLR_ERROR,
					"monitor_retrain_step: %s has no current_mode, "
					"cannot retrain",
					m->wlr_output->name);
			wlr_output_state_finish(&state);
			return 0;
		}
		wl_list_for_each(mode, &m->wlr_output->modes, link) {
			if (mode == cur || mode->width != cur->width ||
				mode->height != cur->height)
				continue;
			if (!alt || mode->refresh < alt->refresh)
				alt = mode;
		}
		if (!alt) {
			wlr_log(WLR_ERROR,
					"monitor_retrain_step: %s has no alternate mode at "
					"%dx%d to cycle through, cannot retrain",
					m->wlr_output->name, cur->width, cur->height);
			wlr_output_state_finish(&state);
			return 0;
		}
		m->retrain_restore_mode = cur;
		wlr_output_state_set_mode(&state, alt);
		wlr_output_commit_state(m->wlr_output, &state);
		m->retrain_phase = 1;
		wl_event_source_timer_update(m->retrain_timer, 350);
	} else {
		/* phase 1: restore the real mode with full HDR/color state */
		if (m->retrain_restore_mode)
			wlr_output_state_set_mode(&state, m->retrain_restore_mode);
		mon_state_apply_color(m, &state);
		mon_derive_color_state(m, &state);
		wlr_output_commit_state(m->wlr_output, &state);
		m->retrain_phase = 0;
		m->retrain_restore_mode = NULL;
		wlr_output_schedule_frame(m->wlr_output);
		/* the phase-0/1 modeset cycle is its own commit sequence, entirely
		 * separate from the DPMS enable/disable paths (wake_monitor(),
		 * powermgrsetmode()) that already call updatemons() -- without this,
		 * layer-shell surfaces (e.g. a bar) never get rearranged/reconfigured
		 * against the restored mode, so a wake with dpms-wake-retrain
		 * enabled could leave one stuck at stale geometry indefinitely. */
		updatemons(NULL, NULL);
	}
	wlr_output_state_finish(&state);
	return 0;
}

void monitor_start_retrain(Monitor *m, uint32_t delay_ms) {
	if (!m || !m->retrain_timer)
		return;
	m->retrain_phase = 0;
	wl_event_source_timer_update(m->retrain_timer, delay_ms ? delay_ms : 1);
}

/* re-enable a DPMS'd-off output. Shared by the wlr_output_power_manager_v1
 * path and by wake-on-input-activity, since neither wlr-dpms nor any other
 * client re-issues a wake automatically on keyboard/pointer activity --
 * unlike sway/Hyprland/niri, asteroidz has no idle daemon built in, so
 * without this an output put to sleep via DPMS never comes back on its own. */
void wake_monitor(Monitor *m) {
	struct wlr_output_state state = {0};

	if (!m->asleep)
		return;

	wlr_output_state_set_enabled(&state, true);
	bool committed = wlr_output_commit_state(m->wlr_output, &state);
	if (!committed)
		wlr_log(WLR_ERROR, "wake_monitor: failed to commit enabled=1 for %s",
				m->wlr_output->name);

	m->asleep = 0;
	/* ADR-604 trigger 4: the panel was not scanning out, so every phase fact
	 * about it is void -- last present, sequence, observed period. Nothing
	 * derived from before the sleep may survive it. */
	az_presenter_reset(m, AZ_PRESENT_RESET_DPMS);
	updatemons(NULL, NULL);

	/* some sinks (DSC panels) come back with a corrupted decoder after
	 * DPMS; schedule a mode-cycle once the panel electronics are awake.
	 * Always attempt this on a failed wake commit even without
	 * dpms_wake_retrain: that option is for the cosmetic flicker case,
	 * but a commit that outright failed needs the recovery attempt
	 * regardless. */
	if (config.dpms_wake_retrain || !committed)
		monitor_start_retrain(m, committed ? 700 : 50);
}

void wake_sleeping_monitors(void) {
	Monitor *m;
	wl_list_for_each(m, &mons, link) {
		if (m->asleep)
			wake_monitor(m);
	}
}

void powermgrsetmode(struct wl_listener *listener, void *data) {
	struct wlr_output_power_v1_set_mode_event *event = data;
	Monitor *m = event->output->data;

	if (!m)
		return;

	if (event->mode) {
		wake_monitor(m);
		return;
	}

	struct wlr_output_state state = {0};
	wlr_output_state_set_enabled(&state, false);
	if (!wlr_output_commit_state(m->wlr_output, &state))
		wlr_log(WLR_ERROR, "powermgrsetmode: failed to commit enabled=0 for %s",
				m->wlr_output->name);

	m->asleep = 1;
	/* Reset on the way DOWN as well as up: an output that is about to stop
	 * scanning out must not leave a last_present behind that a wake would
	 * project a lattice from. */
	az_presenter_reset(m, AZ_PRESENT_RESET_DPMS);
	updatemons(NULL, NULL);
}

void scene_buffer_apply_opacity(struct wlr_scene_buffer *buffer, int32_t sx,
								int32_t sy, void *data) {
	wlr_scene_buffer_set_opacity(buffer, *(double *)data);
}

void client_set_opacity(Client *c, double opacity) {
	opacity = CLAMP_FLOAT(opacity, 0.0f, 1.0f);
	wlr_scene_node_for_each_buffer(&c->scene_surface->node,
								   scene_buffer_apply_opacity, &opacity);
}

void scene_buffer_apply_prevent_scanout(struct wlr_scene_buffer *buffer,
										 int32_t sx, int32_t sy, void *data) {
	wlr_scene_buffer_set_prevent_scanout(buffer, *(bool *)data);
}

void client_set_prevent_scanout(Client *c, bool prevent) {
	wlr_scene_node_for_each_buffer(&c->scene_surface->node,
								   scene_buffer_apply_prevent_scanout, &prevent);
}

void monitor_stop_skip_frame_timer(Monitor *m) {
	if (m->skip_frame_timeout)
		wl_event_source_timer_update(m->skip_frame_timeout, 0);
	m->skiping_frame = false;
	m->resizing_count_pending = 0;
	m->resizing_count_current = 0;
}

static int monitor_skip_frame_timeout_callback(void *data) {
	Monitor *m = data;
	Client *c, *tmp;

	wl_list_for_each_safe(c, tmp, &clients, link) { c->configure_serial = 0; }

	monitor_stop_skip_frame_timer(m);
	wlr_output_schedule_frame(m->wlr_output);

	return 1;
}

void monitor_check_skip_frame_timeout(Monitor *m) {
	if (m->skiping_frame) {
		/* Already in a skip window -- let its 100ms deadline run its course
		 * instead of resetting it here. This used to re-arm the timer to
		 * 100ms from NOW whenever resizing_count_pending had moved on since
		 * the last check, which turned the "give up and force a commit"
		 * safety net into a debounce that never fires: a client generating
		 * configure events faster than every 100ms (not just an actual
		 * human drag-resize) kept pushing the deadline out indefinitely, so
		 * the monitor never reached wlr_scene_output_commit below -- it
		 * kept sending frame_done and letting the client submit ever more
		 * buffers nothing ever consumed or released. Bookkeeping is still
		 * updated so monitor_skip_frame_timeout_callback's reset stays
		 * accurate once its already-running timer does fire. */
		m->resizing_count_current = m->resizing_count_pending;
		return;
	}

	if (m->skip_frame_timeout) {
		m->resizing_count_current = m->resizing_count_pending;
		m->skiping_frame = true;
		wl_event_source_timer_update(m->skip_frame_timeout, 100); // 100ms
	}
}

static void render_monitor(Monitor *m) {
	Client *c = NULL, *tmp = NULL;
	LayerSurface *l = NULL, *tmpl = NULL;
	int32_t i;
	struct wl_list *layer_list;
	bool frame_allow_tearing = false;
	bool pace_needed_frame = false, pace_committed = false;
	uint64_t pace_damage_px = 0;
	int pace_damage_rects = 0;
	pixman_box32_t pace_damage_ext = {0, 0, 0, 0};
	struct timespec now;
	bool need_more_frames = false;
	struct timespec render_t0;
	clock_gettime(CLOCK_MONOTONIC, &render_t0);
	/* P4. The blur chain's rebuild counter before this pass; the delta after
	 * it is what this output's frame cost the chain. See az_tag_cost.h. */
	uint64_t tag_cost_prefix0 = 0, tag_cost_reb0 = 0, tag_cost_hit0 = 0;
	tag_cost_prefix0 = az_avk_blur_prefix_px();
	az_avk_blur_cache_counts(&tag_cost_reb0, &tag_cost_hit0);
	/* M-8: the arm instant. This is the moment ADR-605's `t_pipe` is measured
	 * FROM -- a VRR predictor asks "if I start now, when does it light up?",
	 * and the answer has to include this frame's own render, not just the
	 * commit that follows it. */
	m->m8_arm_ns = (uint64_t)render_t0.tv_sec * 1000000000ull
		+ (uint64_t)render_t0.tv_nsec;
	/* ADR-605: state this pass's target presentation time, once, here. Every
	 * animated object must sample against THIS instant rather than reading
	 * its own clock (audit G3). */
	az_presenter_arm(m, m->m8_arm_ns);

	if (session && !session->active) {
		return;
	}

	if (!m->wlr_output->enabled || !allow_frame_scheduling)
		return;

	/* Opened after the early returns so a bailed-out frame doesn't show up as
	 * a zero-cost render -- those are not frames, and counting them would drag
	 * the visible distribution toward zero. */

	frame_allow_tearing = check_tearing_frame_allow(m);

	/*
	 * M13B: every frame starts having answered nothing about scanout. The
	 * branches below that consider it overwrite this; the ones that cannot --
	 * a screenshot capture, which needs composited pixels by definition, and a
	 * folded HDR state change -- leave it, so the dump says "not-evaluated"
	 * instead of repeating whatever the last evaluating frame decided.
	 */
	m->scanout_verdict = (int32_t)AZ_SCANOUT_NOT_EVALUATED;

	/* Everything from here to the commit is asteroidz's own per-frame work --
	 * layer/client animation ticks, fadeouts, cursor zoom, overview chrome.
	 * render_dur_ms lumps it in with the commit, so a frame that misses its
	 * deadline gives no clue which half was responsible. */
	az_pace_mon = m->wlr_output->name;
	az_frame_reach_reset();

	// draw layers and fade-out effects
	for (i = 0; i < LENGTH(m->layers); i++) {
		layer_list = &m->layers[i];
		wl_list_for_each_safe(l, tmpl, layer_list, link) {
			if (layer_draw_frame(l, az_frame_sample_ns(m))) {
				need_more_frames = true;
				az_frame_reach_all = true;
			}
		}
	}

	wl_list_for_each_safe(c, tmp, &fadeout_clients, fadeout_link) {
		if (client_draw_fadeout_frame(c, az_frame_sample_ns(m))) {
			/* A fadeout owns a shard tree whose extent is not the client's
			 * box; it does not describe itself, so it gets all of them. */
			need_more_frames = true;
			az_frame_reach_all = true;
		}
	}

	wl_list_for_each_safe(l, tmpl, &fadeout_layers, fadeout_link) {
		if (layer_draw_fadeout_frame(l, az_frame_sample_ns(m))) {
			need_more_frames = true;
			az_frame_reach_all = true;
		}
	}

	// cursor zoom view tracking (set_zoom handles its own damage and frame scheduling)
	cursor_zoom_frame(m);

	/*
	 * ── M4I: WHAT A TAG TRANSITION ACTUALLY EXPOSES, PER FRAME ────────────
	 *
	 * AZ_TAGTRACE=1 logs one line per frame in which any client is sliding in
	 * or out of a tag: how much of this output each side actually covers, and
	 * how many windows each contributes.
	 *
	 * The whole architectural question -- is the cost inherent to showing two
	 * tags at once, or is it inefficient recomposition -- turns on a number
	 * nobody has: the simultaneous VISIBLE area of the two populations. A
	 * design argument about push versus cover versus clipped slide is an
	 * argument about that number, and it has so far been made from the shape
	 * of the animation rather than from measurement.
	 *
	 * Visible means clipped to THIS output. A window sliding out is mostly
	 * off-screen for most of the transition, and counting its full box would
	 * report an overlap that the rasteriser never sees.
	 */
	uint64_t tag_px_out = 0, tag_px_in = 0;
	int tag_n_out = 0, tag_n_in = 0;
	bool tag_any = false;
	// draw clients
	wl_list_for_each(c, &clients, link) {
		if (az_tagtrace_on() &&
				(c->animation.tagouting || c->animation.tagining)) {
			struct wlr_box vis = c->animation.current;
			struct wlr_box mon = m->m;
			int32_t x0 = vis.x > mon.x ? vis.x : mon.x;
			int32_t y0 = vis.y > mon.y ? vis.y : mon.y;
			int32_t x1 = vis.x + vis.width < mon.x + mon.width
				? vis.x + vis.width : mon.x + mon.width;
			int32_t y1 = vis.y + vis.height < mon.y + mon.height
				? vis.y + vis.height : mon.y + mon.height;
			uint64_t px = (x1 > x0 && y1 > y0)
				? (uint64_t)(x1 - x0) * (uint64_t)(y1 - y0) : 0;
			tag_any = true;
			if (c->animation.tagouting) {
				tag_px_out += px;
				tag_n_out++;
			} else {
				tag_px_in += px;
				tag_n_in++;
			}
		}
		if (client_draw_frame(c, az_frame_sample_ns(m))) {
			need_more_frames = true;
			/* An opacity-only animation ticks without moving, so
			 * client_animation_next_tick() never ran and never contributed a
			 * reach. Its pixels are still its own box. */
			az_frame_reach_add(&c->animation.current);
		}
		if (!config.animations && !grabc && c->configure_serial &&
			client_is_rendered_on_mon(c, m)) {
			monitor_check_skip_frame_timeout(m);
			goto skip;
		}
	}

	if (tag_any) {
		/* Against the OUTPUT's own logical area, so the two ratios are
		 * comparable between a 4K display and a 1080p one and a reader can
		 * add them: 1.30 means the two populations together cover 130% of
		 * the screen, i.e. they overlap by 30%. */
		uint64_t mon_px = (uint64_t)m->m.width * (uint64_t)m->m.height;
		wlr_log(WLR_ERROR, "aztag mon=%s out_px=%" PRIu64 " in_px=%" PRIu64
			" out_frac=%.3f in_frac=%.3f sum=%.3f n_out=%d n_in=%d",
			m->wlr_output->name, tag_px_out, tag_px_in,
			mon_px ? (double)tag_px_out / (double)mon_px : 0.0,
			mon_px ? (double)tag_px_in / (double)mon_px : 0.0,
			mon_px ? (double)(tag_px_out + tag_px_in) / (double)mon_px : 0.0,
			tag_n_out, tag_n_in);
	}

	if (m->skiping_frame) {
		monitor_stop_skip_frame_timer(m);
	}

	/* advance the overview open/close chrome fade (before the commit below, so
	 * the frame we build already reflects this tick's opacity) */
	if (m->ov_anim_running && overview_anim_frame(m)) {
		/* The overview chrome spans the whole output and, in a multi-output
		 * overview, more than one. */
		need_more_frames = true;
		az_frame_reach_all = true;
	}


	/* The commit: build the output state and hand it to the backend. On the
	 * ordinary path this is wlr_scene_output_commit, which is where scenefx's
	 * own zones (fx_pass, and the frame mark itself) live -- so this zone is
	 * the seam between asteroidz's frame and the renderer's. */

	// only build and commit state when a frame is actually needed
	if (config.allow_tearing && frame_allow_tearing) {
		apply_tear_state(m);
	} else if (shotui.want_capture && shotui.capture_mon == m) {
		wlr_log(WLR_DEBUG, "screenshot_ui: fulfilling capture on %s",
			m->wlr_output->name);
		/* screenshot_ui asked to freeze this monitor: build+commit this
		 * frame ourselves (unconditionally, ignoring needs_frame) so we
		 * can grab a locked reference to its rendered buffer before it's
		 * handed to the output */
		struct wlr_output_state state;
		wlr_output_state_init(&state);
		struct az_frame_options frame_options = {
			.color_transform = az_output_color_transform(m),
		};
		struct wlr_buffer *captured = NULL;
		if (az_output_build_frame(m, &state, &frame_options)) {
			if (state.buffer)
				captured = wlr_buffer_lock(state.buffer);
			if (!wlr_output_commit_state(m->wlr_output, &state)) {
				wlr_log(WLR_ERROR,
						"screenshot_ui: failed to commit capture frame on %s",
						m->wlr_output->name);
				az_output_commit_failed(m);
			}
		} else {
			wlr_log(WLR_ERROR,
					"screenshot_ui: failed to build capture state for %s",
					m->wlr_output->name);
		}
		wlr_output_state_finish(&state);

		ScreenshotMode captured_mode = shotui.capture_mode;
		shotui.want_capture = false;
		shotui.capture_mon = NULL;
		if (captured)
			screenshot_ui_on_captured(m, captured_mode, captured);
	} else if (m->hdr_pending_change) {
		/* fold a pending HDR/color-state change into this frame's own
		 * commit (which carries a fresh buffer) instead of issuing a
		 * separate out-of-band commit that could race an in-flight
		 * page-flip and get rejected by the DRM backend */
		struct wlr_output_state state;
		wlr_output_state_init(&state);
		/* A COLOUR-STATE change needs a modeset, and the kernel says so by
		 * refusing the commit outright.
		 *
		 * mon_state_apply_color() rewrites the connector's image description
		 * and can flip render_format to XRGB2101010. Those are modeset-only
		 * properties on DRM, and wlroots only passes ALLOW_MODESET when the
		 * state asks for reconfiguration (backend/drm/drm.c: `.modeset =
		 * state->allow_reconfiguration`). A freshly initialised state does not
		 * -- only set_mode/set_enabled turn it on -- so every HDR transition
		 * committed with PAGE_FLIP_EVENT | ATOMIC_NONBLOCK and came back
		 * `Atomic commit failed: Invalid argument`, 100% of the time.
		 *
		 * The fallback then retrained the output, which is two full modesets
		 * (23.976Hz, then back to 60) and a visible flash -- so the path
		 * written to AVOID an out-of-band commit was taking the most
		 * disruptive route available on every single toggle. Allowing
		 * reconfiguration makes this commit blocking, which is the right
		 * trade: one blocking commit on a deliberate, rare HDR change beats a
		 * guaranteed failure plus a retrain. */
		state.allow_reconfiguration = true;
		mon_state_apply_color(m, &state);
		mon_derive_color_state(m, &state);
		struct az_frame_options frame_options = {
			/* The state being committed may not have reached
			 * wlr_output->image_description yet, so `m->hdr` stands in for
			 * that half; the rest of the ownership rule is the shared one. */
			.color_transform = m->hdr ? NULL : az_output_color_transform(m),
		};
		if (az_output_build_frame(m, &state, &frame_options)) {
			if (!wlr_output_commit_state(m->wlr_output, &state)) {
				wlr_log(WLR_ERROR,
						"HDR pending-change commit failed on %s, retraining",
						m->wlr_output->name);
				az_output_commit_failed(m);
				monitor_start_retrain(m, 50);
			}
		} else {
			wlr_log(WLR_ERROR,
					"HDR pending-change: failed to build state for %s, retraining",
					m->wlr_output->name);
			/* don't silently give up: without the retrain, the output is
			 * left rendering the OLD color state indefinitely, while
			 * m->hdr already reports the new (unapplied) value */
			monitor_start_retrain(m, 50);
		}
		wlr_output_state_finish(&state);
		m->hdr_pending_change = false;
		/*
		 * M6B/D6. HERE, AND NOT IN hdr_resolve(). What a surface should prefer
		 * is read off `wlr_output->image_description`, which only becomes
		 * current when the state COMMITS -- hdr_resolve merely sets the pending
		 * flag. Announcing from there would state the description the output is
		 * about to leave, which is worse than silence: a client would retarget
		 * to PQ at the moment the output dropped out of it.
		 *
		 * Unconditional on reaching here, failure branches included: a failed
		 * commit leaves the output on its OLD description, and re-stating the
		 * truth costs a comparison inside wlroots (it suppresses an identical
		 * description) while a missed update leaves a client tone-mapping for a
		 * display state that no longer exists.
		 */
		mon_send_preferred_descriptions(m);
		/* M6B/D6. And the frog path, which carries what wp-cm currently
		 * cannot: wlroots drops a preferred description's mastering
		 * luminances and max_cll/max_fall, so this is the only route by which
		 * a client learns the panel's actual ceiling. Same moment, same
		 * reason. */
		frog_send_preferred_metadata_all(m);
	} else if (wlr_scene_output_needs_frame(m->scene_output)) {
		pace_needed_frame = true;
		/* What wlr_scene_output_commit() does, written out, because the
		 * build step in the middle has to be az_output_build_frame(). The
		 * needs-frame check stays: scene_output clears its own pending damage
		 * from the committed state's damage region, so it goes quiet again
		 * whatever built the frame. */
		struct wlr_output_state state;
		wlr_output_state_init(&state);
		struct az_frame_options frame_options = {
			.color_transform = az_output_color_transform(m),
		};
		/*
		 * ── M13B: TRY THE DISPLAY FIRST ───────────────────────────────────
		 *
		 * A fullscreen client whose buffer IS this output's picture does not
		 * need compositing -- it needs handing to the display. The attempt
		 * costs one KMS test when it is eligible at all, and eligibility is
		 * decided from compositor state before any test happens, so an
		 * ordinary desktop frame pays a candidate lookup and nothing more.
		 *
		 * The verdict is recorded either way, because "why is my game not
		 * scanning out" is the question this milestone exists to answer, and
		 * an answer only available when the answer is yes is not one.
		 */
		enum az_scanout_verdict sv = AZ_SCANOUT_NO_CANDIDATE;
		bool scanned_out = az_scanout_try(m, &state, &sv);
		m->scanout_verdict = (int32_t)sv;
		if (scanned_out) {
			bool landed = wlr_output_commit_state(m->wlr_output, &state);
			if (!landed) {
				wlr_log(WLR_ERROR, "scanout: commit failed on %s",
					m->wlr_output->name);
				az_output_commit_failed(m);
			} else {
				/* Counted where it lands, not where it is attempted: a
				 * scanout_frames that includes frames the display refused
				 * reads as "the fast path is working" while the screen
				 * stutters. */
				m->scanout_frames++;
			}
			wlr_output_state_finish(&state);
			/*
			 * ── THE DAMAGE IS SETTLED HERE, OR THE OUTPUT NEVER SLEEPS ────
			 *
			 * wlr_scene_output_needs_frame() is true while
			 * pending_commit_damage is non-empty, and it is the COMPOSITION
			 * path that normally empties it. Skipping composition without
			 * clearing it leaves the output permanently needing a frame: the
			 * compositor would re-scan-out at the panel's maximum rate forever,
			 * including with the game paused and nothing changing.
			 *
			 * Clearing outright rather than subtracting is right for scanout
			 * specifically: the whole output was replaced by the client's
			 * buffer, so there is no partial region left owing.
			 */
			/* And only when it landed. A failed commit that clears the
			 * damage anyway forgets a picture nobody saw, and the frame-done
			 * below tells the client the display is finished with a buffer it
			 * never took. See the same guard in apply_tear_state(). */
			if (landed) {
				pixman_region32_clear(
					&m->scene_output->pending_commit_damage);
				/* The scene still owes frame-done to everything it would have
				 * drawn: a client that never hears back stops rendering. */
				struct timespec sdone;
				clock_gettime(CLOCK_MONOTONIC, &sdone);
				wlr_scene_output_send_frame_done(m->scene_output, &sdone);
			}
			goto scanout_done;
		}
		if (az_output_build_frame(m, &state, &frame_options)) {
			/* The damage this frame actually committed, in output-buffer
			 * pixels, before wlr_output_state_finish() frees the region.
			 * "Damage amplification" is a ratio and the denominator is the
			 * geometry that moved -- so the numerator has to be the region
			 * the compositor really asked the GPU to redraw, not the one the
			 * scene graph was asked to accumulate. */
			if (az_pace_on() && (state.committed & WLR_OUTPUT_STATE_DAMAGE)) {
				int nrects = 0;
				const pixman_box32_t *rects =
					pixman_region32_rectangles(&state.damage, &nrects);
				uint64_t px = 0;
				for (int r = 0; r < nrects; r++)
					px += (uint64_t)(rects[r].x2 - rects[r].x1) *
						(uint64_t)(rects[r].y2 - rects[r].y1);
				pace_damage_px = px;
				pace_damage_rects = nrects;
				/* The union's bounding box, alongside the area. 1.7Mpx in
				 * three rects is a different defect depending on whether they
				 * span the output or sit on top of each other, and area alone
				 * cannot tell the two apart. */
				const pixman_box32_t *ext =
					pixman_region32_extents(&state.damage);
				pace_damage_ext = *ext;
			}
			/* ADR-609 needs the commit CALL instant as well as its return:
			 * together they separate "the pass was not ready in time" from
			 * "the atomic commit itself consumed the margin", and a verdict
			 * that cannot tell those apart is the reflex this table exists to
			 * stop. */
			struct timespec cc;
			clock_gettime(CLOCK_MONOTONIC, &cc);
			uint64_t commit_call_ns =
				(uint64_t)cc.tv_sec * 1000000000ull + (uint64_t)cc.tv_nsec;
			if (!wlr_output_commit_state(m->wlr_output, &state)) {
				wlr_log(WLR_ERROR, "Failed to commit frame on %s",
						m->wlr_output->name);
				az_output_commit_failed(m);
			} else {
				pace_committed = true;
				/* M-8: the commit that this frame's presentation will report
				 * back. Stamped AFTER the commit returns, so the interval to
				 * `when` is genuinely commit-to-photons and contains no
				 * compositor work. */
				struct timespec ct;
				clock_gettime(CLOCK_MONOTONIC, &ct);
				m->m8_commit_ns =
					(uint64_t)ct.tv_sec * 1000000000ull + (uint64_t)ct.tv_nsec;
				m->m8_commit_seq = m->wlr_output->commit_seq;
				m->m8_armed = true;
				/* How long the event loop was not in poll(). See the counters'
				 * comment in az_avk.h: cpu_frame_us covers AVK's record phase
				 * only, so a commit that blocks is invisible to every number
				 * this compositor reports -- while libinput complains that
				 * input is 50ms late. */
				{
					uint64_t c_ns = m->m8_commit_ns > commit_call_ns
						? m->m8_commit_ns - commit_call_ns : 0;
					uint64_t h_ns = m->m8_commit_ns > m->m8_arm_ns
						? m->m8_commit_ns - m->m8_arm_ns : 0;
					avk.commit_samples++;
					avk.commit_ns_sum += c_ns;
					if (c_ns > avk.commit_ns_max)
						avk.commit_ns_max = c_ns;
					avk.handler_ns_sum += h_ns;
					if (h_ns > avk.handler_ns_max)
						avk.handler_ns_max = h_ns;
					if (h_ns > 10000000ull)
						avk.handler_over_10ms++;
					if (h_ns > 30000000ull) {
						avk.handler_over_30ms++;
						/*
						 * NAME THE PARTS OF A STALL, once it is a stall.
						 *
						 * handler_over_30ms and cpu_frame_us_p99 disagree
						 * about how often a frame runs long -- 3.3% against
						 * 1% -- and no aggregate can say where the missing
						 * time went. Guessing produced a fix for a cause
						 * that was not there once already.
						 *
						 * `record` is AVK's record phase, `join` is what it
						 * spent inside that waiting for a client's SHM copy,
						 * `commit` is the KMS commit. `unaccounted` is the
						 * rest of the frame handler: the animation ticks,
						 * fadeouts, cursor zoom and per-client work between
						 * the arm and the build. That last one is the
						 * suspect precisely because nothing has ever
						 * measured it.
						 *
						 * Deliberately not rate-limited. libinput caps its
						 * own complaint at five per hour, so the log went
						 * quiet while the stalls continued -- and the stalls
						 * arrive in bursts, which is a shape only one line
						 * each can show.
						 */
						uint64_t rec_ns = avk.frame_record_us * 1000ull;
						wlr_log(WLR_ERROR,
							"frame stall on %s: handler=%" PRIu64 "us "
							"record=%" PRIu64 "us join=%" PRIu64 "us "
							"commit=%" PRIu64 "us unaccounted=%" PRIu64 "us",
							m->wlr_output->name, h_ns / 1000,
							avk.frame_record_us, avk.frame_join_ns / 1000,
							c_ns / 1000,
							h_ns > rec_ns + c_ns
								? (h_ns - rec_ns - c_ns) / 1000 : 0);
					}
				}
				az_presenter_committed(m, m->wlr_output->commit_seq,
					commit_call_ns, m->m8_commit_ns);
			}
		} else {
			wlr_log(WLR_ERROR, "Failed to build frame for %s",
					m->wlr_output->name);
		}
		wlr_output_state_finish(&state);
scanout_done:
		;
	}


skip:
	// send the frame-done notification
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(m->scene_output, &now);

	// if more frames are needed, make sure the next frame is scheduled --
	// on the outputs the motion can reach, and no others (M4G).
	if (need_more_frames && allow_frame_scheduling) {
		if (az_frame_reach_all || !az_frame_reach_valid) {
			request_fresh_all_monitors();
		} else {
			request_fresh_for_box(&az_frame_reach, AZ_FRAME_REACH_PAD);
		}
	}
	AZ_PACE("sched mon=%s more=%d all=%d valid=%d reach=%d,%d,%dx%d",
		m->wlr_output->name, need_more_frames ? 1 : 0,
		az_frame_reach_all ? 1 : 0, az_frame_reach_valid ? 1 : 0,
		az_frame_reach.x, az_frame_reach.y, az_frame_reach.width,
		az_frame_reach.height);

	// EMA of render+commit cost, used to size the render-late deferral delay
	struct timespec render_t1;
	clock_gettime(CLOCK_MONOTONIC, &render_t1);
	double dur_ms = (render_t1.tv_sec - render_t0.tv_sec) * 1000.0 +
					(render_t1.tv_nsec - render_t0.tv_nsec) / 1.0e6;
	/* decaying max: track the recent worst case (a deadline wants the peak, not
	 * the average -- an EMA hides spikes that then miss the vblank) */
	m->render_dur_ms = dur_ms > m->render_dur_ms ? dur_ms
												 : m->render_dur_ms * 0.95;

	/* The raw per-frame cost, not the decaying max the scheduler runs on. The
	 * estimator deliberately throws the distribution away to keep the peak;
	 * plotting the real value is how you find out whether that peak is one
	 * pathological frame or the shape of the whole run. */
	/* needed=0 committed=0 is the frame that cost a wakeup and produced
	 * nothing -- the shape of a scheduler that keeps asking for frames after
	 * the motion has stopped. It is invisible in every present-side metric,
	 * because there is no present. */
	/*
	 * ── P4: WHAT THIS FRAME COST A TAG TRANSITION ────────────────────────
	 *
	 * `in_tag` is the compositor's own answer -- a client with a TAG animation
	 * still running -- rather than a guess from the geometry, because a slide
	 * and a move look identical from the outside and only one of them is the
	 * transition being measured.
	 */
	{
		bool in_tag = false;
		Client *tc = NULL;
		wl_list_for_each(tc, &clients, link) {
			if (tc->animation.running
					&& tc->animation.action == TAG) {
				in_tag = true;
				break;
			}
		}
		uint64_t prefix_d = 0, reb_d = 0, hit_d = 0;
		uint64_t px = az_avk_blur_prefix_px(), reb = 0, hit = 0;
		az_avk_blur_cache_counts(&reb, &hit);
		prefix_d = px > tag_cost_prefix0 ? px - tag_cost_prefix0 : 0;
		reb_d = reb > tag_cost_reb0 ? reb - tag_cost_reb0 : 0;
		hit_d = hit > tag_cost_hit0 ? hit - tag_cost_hit0 : 0;
		az_tag_cost_frame(in_tag, az_pace_now_ns(), dur_ms, pace_committed,
			(uint64_t)pace_damage_px, prefix_d, reb_d, hit_d);
	}

	AZ_PACE("render mon=%s dur_us=%lld needed=%d committed=%d more=%d "
		"damage_px=%llu damage_rects=%d damage_ext=%d,%d,%dx%d t_ns=%llu",
		m->wlr_output->name,
		(long long)(dur_ms * 1000.0), pace_needed_frame ? 1 : 0,
		pace_committed ? 1 : 0, need_more_frames ? 1 : 0,
		(unsigned long long)pace_damage_px, pace_damage_rects,
		pace_damage_ext.x1, pace_damage_ext.y1,
		pace_damage_ext.x2 - pace_damage_ext.x1,
		pace_damage_ext.y2 - pace_damage_ext.y1,
		(unsigned long long)az_pace_now_ns());
}

/*
 * M6A.1. PRESENTATION FEEDBACK IN PRODUCTION -- observed facts only.
 *
 * No prediction, no correction, no policy. This exists so that the timing
 * model M6A is about can be built against measurements instead of against the
 * arrival pattern of frame events, which is what render-late currently infers
 * misses from (audit G4) and which cannot tell a late CPU from a late flip.
 *
 * The clock-domain proof runs once per output, on its first PRESENTED frame,
 * and is the reason this handler reads two clocks. It is cheap exactly once
 * and never repeated -- see Monitor.present_clock.
 */
static void presentmon(struct wl_listener *listener, void *data) {
	Monitor *m = wl_container_of(listener, m, present);
	const struct wlr_output_event_present *ev = data;

	/* A dropped update still fires this signal. Folding one into the interval
	 * series would invent a refresh that never happened. */
	if (!ev->presented) {
		m->present_dropped++;
		return;
	}
	if (!ev->when.tv_sec && !ev->when.tv_nsec) {
		m->present_no_stamp++;
		return;
	}

	uint64_t when_ns =
		(uint64_t)ev->when.tv_sec * 1000000000ull + (uint64_t)ev->when.tv_nsec;

	{
		/* ADR-603. The presenter's own view of this event: it applies its
		 * epoch and clock gates independently, because the raw counters below
		 * answer "what did the display do" and the presenter answers "was our
		 * prediction any good", and those must not share a filter. */
		struct timespec pn;
		clock_gettime(CLOCK_MONOTONIC, &pn);
		az_presenter_present(m, ev,
			(uint64_t)pn.tv_sec * 1000000000ull + (uint64_t)pn.tv_nsec);
	}

	if (m->present_clock == PRESENT_CLOCK_UNKNOWN) {
		struct timespec mono, real;
		clock_gettime(CLOCK_MONOTONIC, &mono);
		clock_gettime(CLOCK_REALTIME, &real);
		uint64_t mono_ns =
			(uint64_t)mono.tv_sec * 1000000000ull + (uint64_t)mono.tv_nsec;
		uint64_t real_ns =
			(uint64_t)real.tv_sec * 1000000000ull + (uint64_t)real.tv_nsec;
		m->present_skew_mono_ns = (int64_t)when_ns - (int64_t)mono_ns;
		m->present_skew_real_ns = (int64_t)when_ns - (int64_t)real_ns;
		/*
		 * A page flip that has already happened is at most a few frames behind
		 * the handler and never ahead of it, so the right clock is the one the
		 * stamp is within a second of. The two clocks are separated by the
		 * machine's uptime, so this is not a close call and a tolerance far
		 * wider than any plausible latency still cannot confuse them.
		 */
		const int64_t near = 1000000000ll; /* 1s */
		int64_t dm = m->present_skew_mono_ns < 0 ? -m->present_skew_mono_ns
		                                         : m->present_skew_mono_ns;
		int64_t dr = m->present_skew_real_ns < 0 ? -m->present_skew_real_ns
		                                         : m->present_skew_real_ns;
		m->present_clock = dm < near   ? PRESENT_CLOCK_MONOTONIC
		                   : dr < near ? PRESENT_CLOCK_REALTIME
		                               : PRESENT_CLOCK_NEITHER;
		wlr_log(WLR_INFO,
			"M6A: %s presentation stamp is %s (mono %+.3fms, real %+.3fms)",
			m->wlr_output->name,
			m->present_clock == PRESENT_CLOCK_MONOTONIC   ? "CLOCK_MONOTONIC"
			: m->present_clock == PRESENT_CLOCK_REALTIME  ? "CLOCK_REALTIME"
			                                              : "NEITHER CLOCK",
			(double)m->present_skew_mono_ns / 1.0e6,
			(double)m->present_skew_real_ns / 1.0e6);
	}

	/*
	 * ── THE DISPLAY'S PERIOD, AND THE CADENCE, ARE TWO DIFFERENT NUMBERS ──
	 *
	 * The gap between consecutive PRESENTED frames is not the refresh period.
	 * This compositor is damage-driven: it presents when there is something to
	 * show, so on a quiet desktop consecutive presentations are several vblanks
	 * apart. Measured that way DP-1 read 7673us against a 6944us mode and
	 * looked like a display running slow, which it was not.
	 *
	 * `ev->seq` is the vblank counter, so the period is the time delta divided
	 * by the SEQUENCE delta -- correct whether or not frames were skipped. The
	 * sequence delta is itself the cadence: 1 means the next vblank, 2 means
	 * one was missed or nothing was drawn for it, and that is the 1x/2x/3x
	 * accounting derived from actual presentation rather than inferred from GPU
	 * timing.
	 */
	if (m->present_last_ns && when_ns > m->present_last_ns
			&& ev->seq > m->present_last_seq) {
		uint64_t d = when_ns - m->present_last_ns;
		uint64_t nseq = ev->seq - m->present_last_seq;
		uint64_t per = d / nseq;
		/* refresh is in mHz: the period in ns is 1e12/refresh. rendermon()
		 * spells the same conversion 1.0e6/refresh for milliseconds. An
		 * earlier 1e15 here made this guard unreachable and folded a 468ms
		 * idle gap into a 60Hz output's period, which is how it announced
		 * itself on the very first run. */
		uint64_t nominal = m->wlr_output->refresh > 0
			? (uint64_t)(1.0e12 / (double)m->wlr_output->refresh)
			: 0;
		/* A per-vblank period that is not near the mode's is not a period --
		 * it is a sequence counter that jumped, an output that changed mode,
		 * or a backend that does not count vblanks. Reject rather than
		 * average, and count it, because a silently-wrong period would poison
		 * every prediction built on it. */
		if (nominal == 0 || (per > nominal / 2 && per < nominal * 2)) {
			m->present_interval_ns = m->present_interval_ns
				? (m->present_interval_ns * 7 + per) / 8
				: per;
			if (nseq == 1) {
				m->present_cadence_1x++;
			} else if (nseq == 2) {
				m->present_cadence_2x++;
			} else {
				m->present_cadence_3x++;
			}
		} else {
			m->present_interval_rejected++;
		}
	}
	/* The hardware's own guess at the next refresh. Under VRR this is the
	 * closest thing to a period the display will state, rather than one
	 * derived from history. */
	if (ev->refresh > 0) {
		m->present_hw_refresh_ns = (uint64_t)ev->refresh;
	}
	if (ev->seq != 0) {
		m->present_seq_available = true;
	}

	/*
	 * M-8. Matched strictly on commit_seq. An unmatched present means the
	 * pipeline was deeper than the single slot tracked here, and it is COUNTED
	 * rather than attributed to whatever was in the slot -- a latency series
	 * quietly built from mismatched pairs would look plausible and be fiction.
	 */
	if (m->m8_armed && ev->commit_seq == m->m8_commit_seq) {
		m->m8_armed = false;
		if (when_ns > m->m8_arm_ns && when_ns > m->m8_commit_ns) {
			uint64_t a = when_ns - m->m8_arm_ns;
			uint64_t c = when_ns - m->m8_commit_ns;
			m->m8_samples++;
			m->m8_arm_sum_ns += a;
			m->m8_commit_sum_ns += c;
			if (!m->m8_arm_min_ns || a < m->m8_arm_min_ns)
				m->m8_arm_min_ns = a;
			if (a > m->m8_arm_max_ns)
				m->m8_arm_max_ns = a;
			if (!m->m8_commit_min_ns || c < m->m8_commit_min_ns)
				m->m8_commit_min_ns = c;
			if (c > m->m8_commit_max_ns)
				m->m8_commit_max_ns = c;
			uint64_t b = c / AZ_M8_BUCKET_NS;
			m->m8_hist[b < AZ_M8_BUCKETS - 1 ? b : AZ_M8_BUCKETS - 1]++;
		}
	} else if (m->m8_armed) {
		m->m8_unmatched++;
	}

	m->present_last_ns = when_ns;
	m->present_last_seq = ev->seq;
	m->present_count++;
}

/* Presentation feedback, recorded per output. Wired only under AZ_PACE=1. */
static void pacepresent(struct wl_listener *listener, void *data) {
	Monitor *m = wl_container_of(listener, m, pace_present);
	struct wlr_output_event_present *ev = data;
	/* `when` is the moment the content turned into light, and it is only
	 * meaningful when the frame was actually presented -- a dropped update
	 * still fires this signal, and folding one into the interval series would
	 * invent a refresh that never happened. Fall back to now only when the
	 * backend gave no timestamp at all. */
	if (!ev->presented)
		return;
	uint64_t when_ns = (ev->when.tv_sec || ev->when.tv_nsec)
		? (uint64_t)ev->when.tv_sec * 1000000000ull + (uint64_t)ev->when.tv_nsec
		: az_pace_now_ns();
	az_pace_present(m->wlr_output->name, &m->pace_last_present_ns, ev->seq,
		when_ns);
}

// Frame (vblank) event. By default render immediately. With config.render_late
// on, defer the render toward the next vblank -- present lands on the same
// vblank either way, but input sampled during the deferral is ~a frame fresher.
// Still exactly one render per vblank (no GPU pinning); skipped for tearing/VRR
// where fixed-interval deferral doesn't apply.
void rendermon(struct wl_listener *listener, void *data) {
	Monitor *m = wl_container_of(listener, m, frame);

	bool eligible = config.render_late && m->render_timer && !m->skiping_frame &&
					!m->is_vrr_opening && allow_frame_scheduling &&
					(!session || session->active) && m->wlr_output->enabled &&
					m->wlr_output->refresh > 0 && !check_tearing_frame_allow(m);

	if (eligible) {
		/* A deferred render is already armed -- ignore this extra frame event.
		 * Otherwise continuous input (e.g. dragging) fires frame events that
		 * each re-arm (reset) the timer, so it never fires and the render is
		 * starved -> the display freezes until input stops. */
		if (m->render_late_pending)
			return;

		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		uint64_t now_ns = (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
		double interval_ms = 1.0e6 / (double)m->wlr_output->refresh;
		uint64_t interval_ns = (uint64_t)(interval_ms * 1.0e6);

		/* Adapt from how the previous deferred frame landed. Frame events are
		 * ~vblank-aligned, so a ~2-interval gap means we missed a vblank (the
		 * page flip slipped a frame) -- back off. A ~1-interval gap while
		 * rendering continuously means we made it -- reclaim latency slowly.
		 * A large gap = the output was idle; don't judge that. */
		if (m->render_late_deferred && m->render_late_last_ns) {
			uint64_t gap = now_ns - m->render_late_last_ns;
			if (gap > interval_ns * 3) {
				/* idle, ignore */
			} else if (gap > interval_ns + interval_ns / 2) {
				/* A missed vblank is the event the whole loop exists to avoid,
				 * and it is invisible in a duration timeline -- the frame that
				 * slipped looks normal, the cost was the deadline it blew. As
				 * a plot the misses line up against the frac they happened at,
				 * which is what says whether the loop is tuned too tight. */
				m->render_late_frac *= config.render_late_backoff;
				/* Never cut below the point where a deferral can still be
				 * ARMED. Arming requires delay_ms >= 1.0 below; miss it and
				 * render_late_deferred stays false -- and the adaptation this
				 * block belongs to only runs when that flag is true. So a frac
				 * small enough to stop arming is a state the loop can never
				 * climb out of: render-late silently stops deferring, with no
				 * log and no recovery short of a restart.
				 *
				 * Measured on a live desk: frozen at exactly 0.040 for 11900
				 * consecutive samples, deferring 0.66ms where the healthy run
				 * deferred 10.83ms. The trap widens with refresh rate --
				 * frac < 1.0/interval_ms is 0.06 at 60Hz but 0.24 at 240Hz.
				 *
				 * 1.5 rather than 1.0 so the cap_ms clamp below cannot shave
				 * it back under the threshold on a frame that renders slowly. */
				double arm_floor = 1.5 / interval_ms;
				if (arm_floor > config.render_late_cap)
					arm_floor = config.render_late_cap;
				if (m->render_late_frac < arm_floor)
					m->render_late_frac = arm_floor;
				m->render_late_good = 0;
			} else if (++m->render_late_good >= config.render_late_climb_frames) {
				m->render_late_frac += config.render_late_climb_step;
				if (m->render_late_frac > config.render_late_cap)
					m->render_late_frac = config.render_late_cap;
				m->render_late_good = 0;
			}
		}
		m->render_late_last_ns = now_ns;

		double margin_ms = config.render_late_margin_us / 1000.0;
		double delay_ms = interval_ms * m->render_late_frac;
		/* never defer into the (worst-case) render itself */
		double cap_ms = interval_ms - m->render_dur_ms - margin_ms;
		if (delay_ms > cap_ms)
			delay_ms = cap_ms;

		/* render-late 2 = enabled + per-frame log. Logged at ERROR level so it
		 * is visible under the default log level (this is an explicit opt-in
		 * debug mode, not normal INFO chatter). */
		if (config.render_late >= 2)
			wlr_log(WLR_ERROR,
					"render-late %s: frac=%.2f delay=%.1f dur=%.1f interval=%.1f",
					m->wlr_output->name, m->render_late_frac, delay_ms,
					m->render_dur_ms, interval_ms);

		/* The same four numbers render-late 2 logs per frame, as curves. The
		 * loop is a controller: frac climbs by 0.03 every 20 clean frames and
		 * is cut to 0.6x on a slip, and delay is clamped by the render cost.
		 * Whether it is settling or oscillating is a question about the shape
		 * over time, which a log line per frame cannot answer. */

		if (delay_ms >= 1.0) {
			wl_event_source_timer_update(m->render_timer, (int)delay_ms);
			m->render_late_deferred = true;
			m->render_late_pending = true;
			return; // render_timer_cb renders when the deadline arrives
		}
	}

	m->render_late_deferred = false;
	m->render_late_pending = false;
	render_monitor(m);
}

static int render_timer_cb(void *data) {
	Monitor *m = data;
	m->render_late_pending = false; /* deadline reached; render now */
	render_monitor(m);
	return 0;
}

void requestdecorationmode(struct wl_listener *listener, void *data) {
	Client *c = wl_container_of(listener, c, set_decoration_mode);
	struct wlr_xdg_toplevel_decoration_v1 *deco = data;

	if (c->surface.xdg->initialized) {
		/* single source of truth: tell the client exactly what the drawing
		 * paths (titlebar/border/corner) will do. Also never forwards a NONE
		 * request verbatim like the old code did. */
		wlr_xdg_toplevel_decoration_v1_set_mode(
			c->decoration,
			client_wants_ssd(c)
				? WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE
				: WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
		/* an allow_csd client can flip its request at runtime */
		if (c->mon && client_surface(c)->mapped)
			arrange(c->mon, false, false);
	}
}

static void requestdrmlease(struct wl_listener *listener, void *data) {
	struct wlr_drm_lease_request_v1 *req = data;
	struct wlr_drm_lease_v1 *lease = wlr_drm_lease_request_v1_grant(req);

	if (!lease) {
		wlr_log(WLR_ERROR, "Failed to grant lease request");
		wlr_drm_lease_request_v1_reject(req);
	}
}

void requeststartdrag(struct wl_listener *listener, void *data) {
	struct wlr_seat_request_start_drag_event *event = data;

	if (wlr_seat_validate_pointer_grab_serial(seat, event->origin,
											  event->serial))
		wlr_seat_start_pointer_drag(seat, event->drag, event->serial);
	else
		wlr_data_source_destroy(event->drag->source);
}

void client_update_shadow_focus(Client *c) {
	if (!c->shadow)
		return;
	/* while a focus animation runs, the per-tick interpolation drives
	 * the shadow/blur look instead of snapping */
	if (config.animations && c->opacity_animation.running)
		return;
	client_apply_focus_effects(c, (c->mon && c == c->mon->sel) ? 1.0f : 0.0f);
}

void setborder_color(Client *c) {
	if (!c || !c->mon)
		return;

	float *border_color = get_border_color(c);
	memcpy(c->opacity_animation.target_border_color, border_color,
		   sizeof(c->opacity_animation.target_border_color));
	client_set_border_fill(c, border_color);
	client_update_shadow_focus(c);
}

void exchange_two_client(Client *c1, Client *c2) {
	if (c1 == NULL || c2 == NULL ||
		(!config.exchange_cross_monitor && c1->mon != c2->mon)) {
		return;
	}

	Monitor *m1 = c1->mon;
	Monitor *m2 = c2->mon;
	const Layout *layout1 = m1->pertag->ltidxs[m1->pertag->curtag];
	const Layout *layout2 = m2->pertag->ltidxs[m2->pertag->curtag];

	if (layout1->id == SCROLLER || layout2->id == SCROLLER) {
		exchange_two_scroller_clients(c1, c2);
		return;
	}

	if (layout1->id == DWINDLE && layout2->id == DWINDLE) {
		dwindle_swap_clients(c1, c2);
		return;
	}

	client_swap_layout_properties(c1, c2);

	wl_list_swap(&c1->link, &c2->link);

	if (m1 != m2) {
		client_swap_monitors_and_tags(c1, c2);
	}

	finish_exchange_arrange_and_focus(c1, c2, m1, m2);
}

static void set_activation_env(void) {
	if (!getenv("DBUS_SESSION_BUS_ADDRESS")) {
		wlr_log(WLR_INFO, "Not updating dbus execution environment: "
						  "DBUS_SESSION_BUS_ADDRESS not set");
		return;
	}

	wlr_log(WLR_INFO, "Updating dbus execution environment");

	/* env_vars is a fixed built-in list; systemd --user units (e.g.
	 * dms.service, which isn't a child of ours and so never inherits our
	 * process environment) only ever see what we hand them here. Append
	 * the user's own `env = NAME,VALUE` config entries so things like
	 * QT_SCALE_FACTOR/GDK_DPI_SCALE reach those services too. */
	size_t nfixed = LENGTH(env_vars) - 1;
	char **keys = calloc(nfixed + (size_t)config.env_count + 1, sizeof(char *));
	if (!keys) {
		wlr_log(WLR_ERROR, "Failed to allocate activation-env key list");
		return;
	}
	size_t n = 0;
	for (size_t i = 0; i < nfixed; i++)
		keys[n++] = env_vars[i];
	for (int32_t i = 0; i < config.env_count; i++)
		keys[n++] = config.env[i]->type;
	keys[n] = NULL;

	char *env_keys = join_strings(keys, " ");
	free(keys);
	if (!env_keys) {
		wlr_log(WLR_ERROR, "Failed to allocate activation-env key string");
		return;
	}

	// first command: dbus-update-activation-environment
	const char *arg1 = env_keys;
	char *cmd1 = string_printf("dbus-update-activation-environment %s", arg1);
	if (!cmd1) {
		wlr_log(WLR_ERROR, "Failed to allocate command string");
		goto cleanup;
	}
	spawn(&(Arg){.v = cmd1});
	free(cmd1);

	// second command: systemctl --user
	const char *action = "import-environment";
	char *cmd2 = string_printf("systemctl --user %s %s", action, env_keys);
	if (!cmd2) {
		wlr_log(WLR_ERROR, "Failed to allocate command string");
		goto cleanup;
	}
	spawn(&(Arg){.v = cmd2});
	free(cmd2);

cleanup:
	free(env_keys);
}

void // 17
run(char *startup_cmd) {

	set_env();

	/* Add a Unix socket to the Wayland display. */
	const char *socket = wl_display_add_socket_auto(dpy);
	if (!socket)
		die("startup: display_add_socket_auto");
	setenv("WAYLAND_DISPLAY", socket, 1);

	/* Start the backend. This will enumerate outputs and inputs, become the
	 * DRM master, etc */
	if (!wlr_backend_start(backend))
		die("startup: backend_start");

	/* Now that the socket exists and the backend is started, run the
	 * startup command */

	if (startup_cmd) {
		int32_t piperw[2];
		if (pipe(piperw) < 0)
			die("startup: pipe:");
		if ((child_pid = fork()) < 0)
			die("startup: fork:");
		if (child_pid == 0) {
			setsid();
			dup2(piperw[0], STDIN_FILENO);
			close(piperw[0]);
			close(piperw[1]);
			execl("/bin/sh", "/bin/sh", "-c", startup_cmd, NULL);
			die("startup: execl:");
		}
		dup2(piperw[1], STDOUT_FILENO);
		close(piperw[1]);
		close(piperw[0]);
	}

	/* Mark stdout as non-blocking to avoid people who does not close stdin
	 * nor consumes it in their startup script getting dwl frozen */
	if (fd_set_nonblock(STDOUT_FILENO) < 0)
		close(STDOUT_FILENO);

	printstatus(IPC_WATCH_ARRANGGE);

	/* At this point the outputs are initialized, choose initial selmon
	 * based on cursor position, and set default cursor image */
	selmon = xytomon(cursor->x, cursor->y);

	/* TODO hack to get cursor to display in its initial location (100, 100)
	 * instead of (0, 0) and then jumping. still may not be fully
	 * initialized, as the image/coordinates are not transformed for the
	 * monitor when displayed here */
	wlr_cursor_warp_closest(cursor, NULL, cursor->x, cursor->y);
	az_cursor_set_xcursor("left_ptr");
	handlecursoractivity();

	set_activation_env();

	run_exec();
	run_exec_once();

	/* Run the Wayland event loop. This does not return until you exit the
	 * compositor. Starting the backend rigged up all of the necessary event
	 * loop configuration to listen to libinput events, DRM events, generate
	 * frame events at the refresh rate, and so on. */

	wl_display_run(dpy);
}

void setcursor(struct wl_listener *listener, void *data) {
	/* This event is raised by the seat when a client provides a cursor
	 * image */
	struct wlr_seat_pointer_request_set_cursor_event *event = data;
	/* If we're "grabbing" the cursor, don't use the client's image, we will
	 * restore it after "grabbing" sending a leave event, followed by a
	 * enter event, which will result in the client requesting set the
	 * cursor surface
	 */
	if (cursor_mode != CurNormal && cursor_mode != CurPressed)
		return;
	/* This can be sent by any client, so we check to make sure this one is
	 * actually has pointer focus first. If so, we can tell the cursor to
	 * use the provided surface as the cursor image. It will set the
	 * hardware cursor on the output that it's currently on and continue to
	 * do so as the cursor moves between outputs. */
	if (event->seat_client == seat->pointer_state.focused_client) {
		/* Clear previous surface destroy listener if any */
		if (last_cursor.surface &&
			last_cursor_surface_destroy_listener.link.prev != NULL)
			wl_list_remove(&last_cursor_surface_destroy_listener.link);

		last_cursor.shape = 0;
		last_cursor.surface = event->surface;
		last_cursor.hotspot_x = event->hotspot_x;
		last_cursor.hotspot_y = event->hotspot_y;

		/* Track surface destruction to avoid dangling pointer */
		if (event->surface)
			wl_signal_add(&event->surface->events.destroy,
						  &last_cursor_surface_destroy_listener);

		if (!cursor_hidden)
			az_cursor_set_surface(event->surface, event->hotspot_x,
								  event->hotspot_y);
	}
}

void // 0.5
setfloating(Client *c, int32_t floating) {

	Client *fc = NULL;
	struct wlr_box target_box;
	int32_t old_floating_state = c->isfloating;
	c->isfloating = floating;
	bool window_size_outofrange = false;

	if (!c || !c->mon || !client_surface(c)->mapped || c->iskilling)
		return;

	// pinned windows are forced floating; tiling one drops the pin
	if (!floating && c->ispinned)
		c->ispinned = 0;

	target_box = c->geom;

	if (floating == 1 && c != grabc) {

		if (c->isfullscreen) {
			client_pending_fullscreen_state(c, 0);
			client_set_fullscreen(c, 0);
		}

		client_pending_maximized_state(c, 0);
		exit_scroller_stack(c);

		// recompute the centered coordinates
		if (!client_is_x11(c) && !c->iscustompos)
			target_box =
				setclient_coordinate_center(c, c->mon, target_box, 0, 0);
		else
			target_box = c->geom;

		// restore to the memeroy geom
		if (c->float_geom.width > 0 && c->float_geom.height > 0) {
			if (c->mon &&
				c->float_geom.width >= c->mon->w.width - config.gappoh) {
				c->float_geom.width = c->mon->w.width * 0.9;
				window_size_outofrange = true;
			}
			if (c->mon &&
				c->float_geom.height >= c->mon->w.height - config.gappov) {
				c->float_geom.height = c->mon->w.height * 0.9;
				window_size_outofrange = true;
			}
			if (window_size_outofrange) {
				c->float_geom =
					setclient_coordinate_center(c, c->mon, c->float_geom, 0, 0);
			}
			resize(c, c->float_geom, 0);
		} else {
			resize(c, target_box, 0);
		}

		c->need_float_size_reduce = 0;
	} else if (c->isfloating && c == grabc) {
		c->need_float_size_reduce = 0;
	} else {
		c->need_float_size_reduce = 1;
		c->is_scratchpad_show = 0;
		c->is_in_scratchpad = 0;
		c->isnamedscratchpad = 0;
		// make fullscreen windows on the current tag exit fullscreen and join the tiling
		wl_list_for_each(fc, &clients,
						 link) if (fc && fc != c && VISIBLEON(fc, c->mon) &&
								   c->tags & fc->tags && ISFULLSCREEN(fc) &&
								   old_floating_state) {
			clear_fullscreen_flag(fc);
		}
	}

	if (c->isoverlay) {
		wlr_scene_node_reparent(&c->scene->node, layers[LyrOverlay]);
	} else if (client_should_overtop(c) && c->isfloating) {
		wlr_scene_node_reparent(&c->scene->node, layers[LyrTop]);
	} else {
		wlr_scene_node_reparent(&c->scene->node,
								layers[c->isfloating ? LyrTop : LyrTile]);
	}

	if (c->isfloating) {
		set_size_per(c->mon, c);
	}

	if (!c->force_fakemaximize)
		client_set_maximized(c, false);

	if (!c->isfloating || c->force_tiled_state) {
		client_set_tiled(c, WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT |
								WLR_EDGE_RIGHT);
	} else {
		client_set_tiled(c, WLR_EDGE_NONE);
	}

	arrange(c->mon, false, false);

	if (!c->isfloating) {
		c->old_master_inner_per = c->master_inner_per;
		c->old_stack_inner_per = c->stack_inner_per;
	}

	setborder_color(c);
	printstatus(IPC_WATCH_ARRANGGE);
}

void reset_maximizescreen_size(Client *c) {
	struct wlr_box geom;
	geom.x = c->mon->w.x + config.gappoh;
	geom.y = c->mon->w.y + config.gappov;
	geom.width = c->mon->w.width - 2 * config.gappoh;
	geom.height = c->mon->w.height - 2 * config.gappov;

	resize(c, geom, 0);
}

void exit_scroller_stack(Client *c) {
	if (!c || !c->mon)
		return;

	uint32_t tag = c->mon->pertag->curtag;
	struct TagScrollerState *st = c->mon->pertag->scroller_state[tag];
	if (st) {
		struct ScrollerStackNode *n = find_scroller_node(st, c);
		if (n) {
			scroller_node_remove(st, n);
			return; /* node removed; the client pointer was already cleared inside the function */
		}
	}
}

void setmaximizescreen(Client *c, int32_t maximizescreen, bool rearrange) {
	struct wlr_box maximizescreen_box;
	if (!c || !c->mon || !client_surface(c)->mapped || c->iskilling)
		return;

	if (c->mon->isoverview)
		return;

	client_pending_maximized_state(c, maximizescreen);

	if (maximizescreen) {

		if (c->isfullscreen) {
			client_pending_fullscreen_state(c, 0);
			client_set_fullscreen(c, 0);
		}

		exit_scroller_stack(c);

		maximizescreen_box.x = c->mon->w.x + config.gappoh;
		maximizescreen_box.y = c->mon->w.y + config.gappov;
		maximizescreen_box.width = c->mon->w.width - 2 * config.gappoh;
		maximizescreen_box.height = c->mon->w.height - 2 * config.gappov;

		wlr_scene_node_raise_to_top(&c->scene->node);
		if (!is_scroller_layout(c->mon) || c->isfloating)
			resize(c, maximizescreen_box, 0);
	} else {
		c->bw = c->isnoborder ? 0 : config.borderpx;
		if (c->isfloating)
			setfloating(c, 1);
	}

	wlr_scene_node_reparent(&c->scene->node,
							layers[c->ismaximizescreen ? LyrMaximize
								   : c->isfloating	   ? LyrTop
													   : LyrTile]);

	if (!c->force_fakemaximize && !c->ismaximizescreen) {
		client_set_maximized(c, false);
	} else if (!c->force_fakemaximize && c->ismaximizescreen) {
		client_set_maximized(c, true);
	}

	if (rearrange)
		arrange(c->mon, false, false);
}

void setfakefullscreen(Client *c, int32_t fakefullscreen) {
	c->isfakefullscreen = fakefullscreen;
	if (!c->mon)
		return;

	if (c->isfullscreen)
		setfullscreen(c, 0, true);

	client_set_fullscreen(c, fakefullscreen);
}

void setfullscreen(Client *c, int32_t fullscreen,
				   bool rearrange) // use the custom fullscreen proxy's own fullscreen
{

	if (!c || !c->mon || !client_surface(c)->mapped || c->iskilling)
		return;

	if (c->mon->isoverview)
		return;

	c->isfullscreen = fullscreen;

	client_set_fullscreen(c, fullscreen);
	client_pending_fullscreen_state(c, fullscreen);

	if (fullscreen) {

		if (c->ismaximizescreen && !c->force_fakemaximize) {
			client_set_maximized(c, false);
		}

		client_pending_maximized_state(c, 0);

		exit_scroller_stack(c);
		c->isfakefullscreen = 0;

		c->bw = 0;
		wlr_scene_node_raise_to_top(&c->scene->node); // raise the view to the top
		if (!is_scroller_layout(c->mon) || c->isfloating)
			resize(c, c->mon->m, 1);

	} else {
		c->bw = c->isnoborder ? 0 : config.borderpx;
		if (c->isfloating)
			setfloating(c, 1);
	}

	if (c->isoverlay) {
		wlr_scene_node_reparent(&c->scene->node, layers[LyrOverlay]);
	} else if (fullscreen) {
		/* dedicated layer above LyrTop: a bar (re)mapping on the top layer
		 * can never end up stacked over a fullscreen window */
		wlr_scene_node_reparent(&c->scene->node, layers[LyrFS]);
	} else if (client_should_overtop(c) && c->isfloating) {
		wlr_scene_node_reparent(&c->scene->node, layers[LyrTop]);
	} else {
		wlr_scene_node_reparent(&c->scene->node,
								layers[c->isfloating ? LyrTop : LyrTile]);
	}

	check_vrr_enable(c);
	hdr_resolve_all();

	if (rearrange)
		arrange(c->mon, false, false);
}

void setgaps(int32_t oh, int32_t ov, int32_t ih, int32_t iv) {
	selmon->gappoh = ASTEROIDZ_MAX(oh, 0);
	selmon->gappov = ASTEROIDZ_MAX(ov, 0);
	selmon->gappih = ASTEROIDZ_MAX(ih, 0);
	selmon->gappiv = ASTEROIDZ_MAX(iv, 0);
	arrange(selmon, false, false);
}

void reset_keyboard_layout(void) {
	if (!kb_group || !kb_group->wlr_group || !seat) {
		wlr_log(WLR_ERROR, "Invalid keyboard group or seat");
		return;
	}

	struct wlr_keyboard *keyboard = &kb_group->wlr_group->keyboard;
	if (!keyboard || !keyboard->keymap) {
		wlr_log(WLR_ERROR, "Invalid keyboard or keymap");
		return;
	}

	// Get current layout
	xkb_layout_index_t current = xkb_state_serialize_layout(
		keyboard->xkb_state, XKB_STATE_LAYOUT_EFFECTIVE);
	const int32_t num_layouts = xkb_keymap_num_layouts(keyboard->keymap);
	if (num_layouts < 1) {
		wlr_log(WLR_INFO, "No layouts available");
		return;
	}

	// Create context
	struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (!context) {
		wlr_log(WLR_ERROR, "Failed to create XKB context");
		return;
	}

	struct xkb_keymap *new_keymap = xkb_keymap_new_from_names(
		context, &config.xkb_rules, XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (!new_keymap) {
		// this should theoretically never fail, since it was already validated above
		wlr_log(WLR_ERROR,
				"Unexpected failure to create keymap after validation");
		goto cleanup_context;
	}

	// verify the new keymap has layouts
	const int32_t new_num_layouts = xkb_keymap_num_layouts(new_keymap);
	if (new_num_layouts < 1) {
		wlr_log(WLR_ERROR, "New keymap has no layouts");
		xkb_keymap_unref(new_keymap);
		goto cleanup_context;
	}

	// make sure the current layout index is valid in the new keymap
	if (current >= new_num_layouts) {
		wlr_log(WLR_INFO,
				"Current layout index %u out of range for new keymap, "
				"resetting to 0",
				current);
		current = 0;
	}

	// Apply the new keymap
	uint32_t depressed = keyboard->modifiers.depressed;
	uint32_t latched = keyboard->modifiers.latched;
	uint32_t locked = keyboard->modifiers.locked;

	wlr_keyboard_set_keymap(keyboard, new_keymap);

	wlr_keyboard_notify_modifiers(keyboard, depressed, latched, locked, 0);
	keyboard->modifiers.group = current; // Keep the same layout index

	// Update seat
	wlr_seat_set_keyboard(seat, keyboard);
	wlr_seat_keyboard_notify_modifiers(seat, &keyboard->modifiers);

	InputDevice *id;
	wl_list_for_each(id, &inputdevices, link) {
		if (id->wlr_device->type != WLR_INPUT_DEVICE_KEYBOARD) {
			continue;
		}

		struct wlr_keyboard *tkb = (struct wlr_keyboard *)id->device_data;

		wlr_keyboard_set_keymap(tkb, keyboard->keymap);
		wlr_keyboard_notify_modifiers(tkb, depressed, latched, locked, 0);
		tkb->modifiers.group = 0;

		// 7. update seat
		wlr_seat_set_keyboard(seat, tkb);
		wlr_seat_keyboard_notify_modifiers(seat, &tkb->modifiers);
	}

	// Cleanup
	xkb_keymap_unref(new_keymap);

cleanup_context:
	xkb_context_unref(context);
}

void setmon(Client *c, Monitor *m, uint32_t newtags, bool focus) {
	Monitor *oldmon = c->mon;

	if (oldmon == m)
		return;

	if (oldmon && oldmon->sel == c) {
		oldmon->sel = NULL;
	}

	if (oldmon && oldmon->prevsel == c) {
		oldmon->prevsel = NULL;
	}

	c->mon = m;

	/* The monitor decides the X11 scale, so this is the moment it can change.
	 * Before the resize below, so the configure that follows is already in
	 * the new unit rather than one frame behind it.
	 *
	 * BREAK: AZ_BREAK_X11_MON_MIGRATE=1 skips it, leaving a window measured
	 * in the units of the display it OPENED on. That is the most plausible
	 * way to get this wrong, and on a single-output layout it is
	 * indistinguishable from correct -- which is why
	 * contrib/xw-mixed-test.sh exists and runs two outputs at two scales. */
	{
		static int break_migrate = -1;
		if (break_migrate < 0) {
			const char *e = getenv("AZ_BREAK_X11_MON_MIGRATE");
			break_migrate = e && *e && *e != '0';
		}
		if (!break_migrate) {
			client_update_x11_scale(c);
		}
	}

	/* Scene graph sends surface leave/enter events on move and resize */
	if (oldmon)
		arrange(oldmon, false, false);
	if (m) {
		/* Make sure window actually overlaps with the monitor */
		reset_foreign_tolevel(c, oldmon, m);
		resize(c, c->geom, 0);
		client_reset_mon_tags(c, m, newtags);
		check_match_tag_floating_rule(c, m);
		setfloating(c, c->isfloating);
		setfullscreen(c, c->isfullscreen,
					  true); /* This will call arrange(c->mon) */
	}

	if (focus && !client_is_x11_popup(c)) {
		focusclient(focustop(selmon), 1);
	}

	/* M6B/D6. The surface has changed output, so what its display prefers may
	 * have changed with it -- an SDR panel and an HDR one side by side are the
	 * whole reason this is said per output rather than once. */
	if (m != NULL) {
		struct wlr_surface *s = client_surface(c);
		if (s != NULL && s->mapped) {
			surface_send_preferred_description(s, m);
		}
		/* The frog half of the same statement. A surface that moved to another
		 * display must be told that display's metadata, not the one it was
		 * created on. */
		frog_send_preferred_metadata_all(m);
	}
}

/*
 * ── M6B/D6: TELL A SURFACE WHAT ITS OUTPUT PREFERS ────────────────────────
 *
 * `wlr_color_manager_v1_set_surface_preferred_image_description` had no callers
 * anywhere in the tree, which means every wp-cm client asking DP-1 what it
 * would like got the compositor default -- SDR -- and correctly tone-mapped its
 * HDR content down to meet it. The panel is in HDR10, the client has HDR10, and
 * the handshake in between says "this display is SDR". That is the same failure
 * shape as the capability list one protocol object over: nothing errors,
 * nothing logs, the picture is merely wrong in the one place HDR was for.
 *
 * WHAT IS PREFERRED IS THE OUTPUT'S OWN DESCRIPTION, not a guess. When the
 * output is presenting HDR that is PQ/BT.2020 carrying the monitor rule's
 * mastering values; otherwise it is the default, and NULL is how this protocol
 * says "the default" -- not an omission.
 *
 * NOT ON EVERY FRAME. The description changes when an output's colour state
 * changes or when a surface moves to a different output, and both are rare
 * events. wlroots additionally suppresses a repeat of an identical description
 * (`last_image_desc_identity`), so a redundant call here costs a comparison
 * rather than a protocol event -- but the calls are still placed at the three
 * moments the answer can change rather than sprayed.
 */
static void surface_send_preferred_description(struct wlr_surface *surface,
		Monitor *m) {
	if (az_wpcm_global == NULL || surface == NULL) {
		return;
	}
	/*
	 * THE SHARED POLICY DECIDES, not the caller. `m` is taken as a hint only
	 * and is re-derived: the whole point of az_preferred.h is that frog and
	 * wp-cm cannot answer "which display is this surface on" differently, and
	 * a caller that passed the wrong monitor would reintroduce exactly that.
	 */
	struct az_preferred pref;
	az_preferred_resolve(surface, &pref);
	m = pref.mon;
	/*
	 * BREAK: AZ_BREAK_CM_NO_PREFERRED restores the pre-M6B behaviour exactly --
	 * the compositor never tells a surface anything, and the client falls back
	 * to the default. G4's falsifier: with this set, a client that observed
	 * PQ must observe the default instead.
	 */
	static int suppressed = -1;
	if (suppressed < 0) {
		suppressed = getenv("AZ_BREAK_CM_NO_PREFERRED") != NULL;
	}
	if (suppressed) {
		return;
	}
	/*
	 * ── WHAT IS LOGGED IS WHAT IS SENT ────────────────────────────────────
	 *
	 * This function used to build a full wlr_image_description_v1_data here --
	 * an SDR default, or the output's HDR values -- log it under AZ_DEBUG_CM,
	 * and then never use it. It was the argument to
	 * `wlr_color_manager_v1_set_surface_preferred_image_description`, and when
	 * native wp-cm took ownership the call went away while its argument stayed.
	 *
	 * That left a diagnostic that DISAGREED with the wire. az_wpcm_send_
	 * preferred() re-resolves through az_preferred_resolve() and serializes its
	 * own struct, so the log printed the output's max_cll where the wire
	 * carried the monitor rule's max_luminance. A log whose numbers are not the
	 * numbers sent is worse than no log: it is a confident wrong answer, and
	 * the first thing anyone reaches for when the picture is wrong.
	 *
	 * The shared policy is now the only source, and it is the same one both
	 * frontends serialize.
	 */
	if (getenv("AZ_DEBUG_CM") != NULL) {
		wlr_log(WLR_INFO, "D6: surface %p mon=%s hdr=%d bt2020=%d "
			"minlum=%.4f maxlum=%.0f maxfall=%.0f identity=%" PRIu64,
			(void *)surface,
			m != NULL && m->wlr_output != NULL && m->wlr_output->name != NULL
				? m->wlr_output->name : "-",
			pref.hdr, pref.bt2020, pref.min_luminance, pref.max_luminance,
			pref.max_fall, pref.identity);
	}
	/*
	 * ONE POLICY, TWO SERIALIZERS -- and here is the second one. The feedback
	 * objects are told directly, and they carry the mastering values wlroots'
	 * implementation dropped: this compositor's own wp-cm is the reason those
	 * numbers reach a client at all.
	 */
	az_wpcm_send_preferred(surface);
}

/* Every mapped client on `m`. Called when the output's colour state changes,
 * which is the one case where nothing about the surfaces themselves moved. */
static void mon_send_preferred_descriptions(Monitor *m) {
	/* The output's own object, if any client asked for one. Separate from the
	 * per-surface sends below: a client may be watching the OUTPUT without
	 * having a surface on it. */
	if (m != NULL && m->wlr_output != NULL) {
		az_wpcm_output_changed(m->wlr_output);
	}
	Client *c;
	wl_list_for_each(c, &clients, link) {
		if (c->mon != m || !c->mon) {
			continue;
		}
		struct wlr_surface *s = client_surface(c);
		if (s != NULL && s->mapped) {
			surface_send_preferred_description(s, m);
		}
	}
}

/*
 * ── A CHANGE THAT IS NOT SCOPED TO ONE OUTPUT ─────────────────────────────
 *
 * config.sdr_reference_luminance is global and is the SDR branch of
 * az_preferred_resolve(), so moving it changes what EVERY surface on EVERY
 * non-HDR output should be told. There is no monitor to scope the walk to.
 *
 * Cheap for the same reason the per-output version is: the identity comparison
 * inside each frontend decides whether anything actually goes on the wire, so
 * surfaces on an HDR output reach here, compare equal, and cost one hash.
 */
static void mon_send_preferred_descriptions_all(void) {
	Monitor *m;
	wl_list_for_each(m, &mons, link) {
		mon_send_preferred_descriptions(m);
		frog_send_preferred_metadata_all(m);
	}
}

void setpsel(struct wl_listener *listener, void *data) {
	/* This event is raised by the seat when a client wants to set the
	 * selection, usually when the user copies something. wlroots allows
	 * compositors to ignore such requests if they so choose, but in dwl we
	 * always honor
	 */
	struct wlr_seat_request_set_primary_selection_event *event = data;
	// XWayland syncs the X PRIMARY selection onto the seat through this same
	// signal, so dropping the Wayland global is not enough on its own: refuse
	// here too and the primary selection stays empty for everyone.
	if (!config.primary_selection) {
		return;
	}
	wlr_seat_set_primary_selection(seat, event->source, event->serial);
}

void setsel(struct wl_listener *listener, void *data) {
	/* This event is raised by the seat when a client wants to set the
	 * selection, usually when the user copies something. wlroots allows
	 * compositors to ignore such requests if they so choose, but in dwl we
	 * always honor
	 */
	struct wlr_seat_request_set_selection_event *event = data;
	wlr_seat_set_selection(seat, event->source, event->serial);
}

void show_hide_client(Client *c) {
	uint32_t target = 1;

	set_size_per(c->mon, c);
	target = get_tags_first_tag(c->oldtags);

	if (!c->is_in_scratchpad) {
		tag_client(&(Arg){.ui = target}, c);
	} else {
		c->tags = c->oldtags;
		arrange(c->mon, false, false);
	}
	client_pending_minimized_state(c, 0);
	focusclient(c, 1);

	if (c->foreign_toplevel)
		wlr_foreign_toplevel_handle_v1_set_activated(c->foreign_toplevel, true);
}

void create_output(struct wlr_backend *backend, void *data) {
	bool *done = data;
	if (*done) {
		return;
	}

	if (wlr_backend_is_wl(backend)) {
		wlr_wl_output_create(backend);
		*done = true;
	} else if (wlr_backend_is_headless(backend)) {
		wlr_headless_add_output(backend, 1920, 1080);
		*done = true;
	}
#if WLR_HAS_X11_BACKEND
	else if (wlr_backend_is_x11(backend)) {
		wlr_x11_output_create(backend);
		*done = true;
	}
#endif
}

// modified signal-handling function to accept a mask parameter
void handle_print_status(struct wl_listener *listener, void *data) {

	enum ipc_watch_type type = *(enum ipc_watch_type *)data;

	if (type & IPC_WATCH_KEYMODE) {
		ipc_notify_keymode();
	}
	if (type & IPC_WATCH_KB_LAYOUT) {
		ipc_notify_kb_layout();
	}
	if (type & IPC_WATCH_FOCUSED_CLIENT) {
		ipc_notify_focused_client();
	}
	if (type & IPC_WATCH_ALL_TAGS) {
		ipc_notify_all_tags();
	}
	if (type & IPC_WATCH_ALL_CLIENTS) {
		ipc_notify_all_clients();
	}
	if (type &
		(IPC_WATCH_ALL_MONITORS | IPC_WATCH_KEYMODE | IPC_WATCH_KB_LAYOUT |
		 IPC_WATCH_FOCUSED_CLIENT | IPC_WATCH_TAGS)) {
		ipc_notify_all_monitors();
	}

	if (type & IPC_WATCH_CLIENT) {
		Client *c = NULL;
		wl_list_for_each(c, &clients, link) {
			if (c->iskilling)
				continue;
			ipc_notify_client(c);
		}
	}

	Monitor *m = NULL;
	wl_list_for_each(m, &mons, link) {
		if (!m->wlr_output->enabled) {
			continue;
		}

		if (type & IPC_WATCH_MONITOR) {
			ipc_notify_monitor(m);
		}
		if (type & IPC_WATCH_TAGS) {
			ipc_notify_tags(m);
		}

		if (type & IPC_WATCH_LAST_OPEN_SURFACE) {
			ipc_notify_last_surface_ws_name(m);
		}

		dwl_ext_workspace_printstatus(m);
		dwl_ipc_output_printstatus(m);
	}
}

/* Same formatting as wlroots' own default stderr logger (util/log.c's
 * log_stderr, which we can't call directly since it's static), except it
 * drops a small denylist of upstream DEBUG lines that are pure noise with
 * no diagnostic value here -- e.g. wlr_text_input_v3.c's "Text input commit
 * received without focus", which fires continuously during normal
 * input-method use and floods the log. cli_debug_log (and therefore
 * WLR_DEBUG) is on by default after any in-place restart, not just -d, so
 * this noise shows up far more often than a one-off manual debug run. */
static void asteroidz_log_callback(enum wlr_log_importance verbosity,
									const char *fmt, va_list args) {
	static const char *denylist[] = {
		"Text input commit received without focus",
	};

	char msg[1024];
	vsnprintf(msg, sizeof(msg), fmt, args);

	for (size_t i = 0; i < LENGTH(denylist); i++) {
		if (strstr(msg, denylist[i])) {
			return;
		}
	}

	static struct timespec start_time = {-1, 0};
	if (start_time.tv_sec < 0) {
		clock_gettime(CLOCK_MONOTONIC, &start_time);
	}

	struct timespec ts = {0};
	clock_gettime(CLOCK_MONOTONIC, &ts);
	ts.tv_sec -= start_time.tv_sec;
	ts.tv_nsec -= start_time.tv_nsec;
	if (ts.tv_nsec < 0) {
		ts.tv_sec--;
		ts.tv_nsec += 1000000000L;
	}

	static const char *verbosity_colors[] = {
		[WLR_SILENT] = "",
		[WLR_ERROR] = "\x1B[1;31m",
		[WLR_INFO] = "\x1B[1;34m",
		[WLR_DEBUG] = "\x1B[1;90m",
	};
	static const char *verbosity_headers[] = {
		[WLR_SILENT] = "",
		[WLR_ERROR] = "[ERROR]",
		[WLR_INFO] = "[INFO]",
		[WLR_DEBUG] = "[DEBUG]",
	};
	unsigned c = (verbosity < WLR_LOG_IMPORTANCE_LAST) ? verbosity
														: WLR_LOG_IMPORTANCE_LAST - 1;

	fprintf(stderr, "%02d:%02d:%02d.%03ld ", (int)(ts.tv_sec / 60 / 60),
			(int)(ts.tv_sec / 60 % 60), (int)(ts.tv_sec % 60),
			ts.tv_nsec / 1000000);

	bool colored = isatty(STDERR_FILENO);
	if (colored) {
		fprintf(stderr, "%s%s\x1B[0m\n", verbosity_colors[c], msg);
	} else {
		fprintf(stderr, "%s %s\n", verbosity_headers[c], msg);
	}
}

void setup(void) {

	setenv("XDG_CURRENT_DESKTOP", "asteroidz", 1);
	setenv("XDG_SESSION_TYPE", "wayland", 1);
	setenv("_JAVA_AWT_WM_NONREPARENTING", "1", 1);

	parse_config();
	if (cli_debug_log) {
		config.log_level = WLR_DEBUG;
	}
	init_baked_points();

	int32_t drm_fd, i;
	int32_t sig[] = {SIGCHLD, SIGINT,
					 SIGTERM}; // don't set SIGPIPE, since an ipc send failure shouldn't affect the main program
	struct sigaction sa = {.sa_flags = SA_RESTART, .sa_handler = handlesig};
	sigemptyset(&sa.sa_mask);

	for (i = 0; i < LENGTH(sig); i++)
		sigaction(sig[i], &sa, NULL);

	// separately set SIGPIPE to be ignored
	struct sigaction sa_pipe = {.sa_flags = 0, .sa_handler = SIG_IGN};
	sigemptyset(&sa_pipe.sa_mask);
	sigaction(SIGPIPE, &sa_pipe, NULL);

	wlr_log_init(config.log_level, asteroidz_log_callback);

	/* The Wayland display is managed by libwayland. It handles accepting
	 * clients from the Unix socket, manging Wayland globals, and so on. */
	dpy = wl_display_create();
	event_loop = wl_display_get_event_loop(dpy);
	portals_init();

	ipc_init(event_loop);
	session_bus_init();

	tablet_mgr = wlr_tablet_v2_create(dpy);
	/* The backend is a wlroots feature which abstracts the underlying input
	 * and output hardware. The autocreate option will choose the most
	 * suitable backend based on the current environment, such as opening an
	 * X11 window if an X11 server is running. The NULL argument here
	 * optionally allows you to pass in a custom renderer if wlr_renderer
	 * doesn't meet your needs. The backend uses the renderer, for example,
	 * to fall back to software cursors if the backend does not support
	 * hardware cursors (some older GPUs don't). */
	/*
	 * ── WHICH GPU, IF THE OPERATOR SAID ───────────────────────────────────
	 *
	 * wlroots picks the DRM device by `boot_vga` and udev enumeration order,
	 * swapping the "primary" to index 0 (backend/session/session.c). That is a
	 * heuristic, plus an ordering, plus whatever raced at boot -- and a device
	 * that fails to open is silently skipped, so a discrete card whose driver
	 * has not finished coming up leaves the integrated one at index 0. This
	 * machine has been observed rendering on the iGPU with both displays on the
	 * discrete card: correct behaviour from AVK, which renders where it
	 * presents, and a bad outcome.
	 *
	 * `gpu` replaces all of that with a name. WLR_DRM_DEVICES is wlroots' own
	 * documented override and it is checked before any enumeration happens, so
	 * this needs no patch and no fallback logic of ours.
	 *
	 * AN EXISTING WLR_DRM_DEVICES WINS. Someone who set it on the command line
	 * is debugging exactly this, and a config file silently overriding the
	 * environment is how a debugging session stops meaning anything.
	 */
	if (config.gpu[0] != '\0' && getenv("WLR_DRM_DEVICES") == NULL) {
		char node[128];
		if (config.gpu[0] == '/') {
			snprintf(node, sizeof(node), "%s", config.gpu);
		} else {
			/* A PCI address: resolve it to whichever card claims it, so the
			 * operator can write the stable identifier rather than a cardN
			 * whose number depends on probe order. */
			node[0] = '\0';
			for (int i = 0; i < 8; i++) {
				char link[128], real[PATH_MAX];
				snprintf(link, sizeof(link), "/sys/class/drm/card%d/device", i);
				ssize_t n = readlink(link, real, sizeof(real) - 1);
				if (n <= 0) {
					continue;
				}
				real[n] = '\0';
				const char *base = strrchr(real, '/');
				if (base != NULL && strcmp(base + 1, config.gpu) == 0) {
					snprintf(node, sizeof(node), "/dev/dri/card%d", i);
					break;
				}
			}
			if (node[0] == '\0') {
				wlr_log(WLR_ERROR, "gpu \"%s\": no DRM card has that PCI "
					"address; letting wlroots choose", config.gpu);
			}
		}
		if (node[0] != '\0') {
			if (access(node, R_OK | W_OK) != 0) {
				/* Not fatal. A wrong `gpu` should cost the preference, not the
				 * session -- there is no way to fix a config from a desktop
				 * that will not start. */
				wlr_log(WLR_ERROR, "gpu \"%s\" -> %s: %s; letting wlroots "
					"choose", config.gpu, node, strerror(errno));
			} else {
				setenv("WLR_DRM_DEVICES", node, 1);
				wlr_log(WLR_INFO, "gpu: driving %s (from `gpu %s`)", node,
					config.gpu);
			}
		}
	}

	if (!(backend = wlr_backend_autocreate(event_loop, &session)))
		die("couldn't create backend");

	headless_backend = wlr_headless_backend_create(event_loop);
	if (!headless_backend) {
		wlr_log(WLR_ERROR, "Failed to create secondary headless backend");
	} else {
		wlr_multi_backend_add(backend, headless_backend);
	}

	/* Initialize the scene graph used to lay out windows */
	scene = wlr_scene_create();
	root_bg = wlr_scene_rect_create(&scene->tree, 0, 0, config.rootcolor);
	for (i = 0; i < NUM_LAYERS; i++)
		layers[i] = wlr_scene_tree_create(&scene->tree);
	drag_icon = wlr_scene_tree_create(&scene->tree);
	wlr_scene_node_place_below(&drag_icon->node, &layers[LyrBlock]->node);

	/* easter egg: topmost overlay so the fly-by renders over the bar */
	ufo_egg = ufo_egg_create(event_loop, layers[LyrScreenshot],
							 ufo_bar_geometry, NULL);
	if (ufo_egg) {
		ufo_egg_set_accent(ufo_egg, config.focuscolor[0], config.focuscolor[1],
						   config.focuscolor[2]);
		ufo_egg_set_enabled(ufo_egg, config.ufo_easter_egg);
	}

	/* Create a renderer with the default implementation */
	if (!(drw = az_create_renderer(backend)))
		die("couldn't create renderer");
	az_require_vulkan_renderer(drw);

	wl_signal_add(&drw->events.lost, &gpu_reset);

	/* Create shm, drm and linux_dmabuf interfaces by ourselves.
	 * The simplest way is call:
	 *      wlr_renderer_init_wl_display(drw);
	 * but we need to create manually the linux_dmabuf interface to
	 * integrate it with wlr_scene. */
	wlr_renderer_init_wl_shm(drw, dpy);

	if (wlr_renderer_get_texture_formats(drw, WLR_BUFFER_CAP_DMABUF)) {
		/* wl_drm is the legacy, pre-feedback interface and is renderer-shaped
		 * by definition; it stays with drw. linux-dmabuf is created further
		 * down, once it is known which engine will be importing. */
		wlr_drm_create(dpy, drw);
	}

	if (config.syncobj_enable && (drm_fd = wlr_renderer_get_drm_fd(drw)) >= 0 &&
		drw->features.timeline && backend->features.timeline)
		wlr_linux_drm_syncobj_manager_v1_create(dpy, 1, drm_fd);

	/* Create a default allocator */
	if (!(alloc = wlr_allocator_autocreate(backend, drw)))
		die("couldn't create allocator");

	/* ── the engine that composites ──────────────────────────────────────
	 *
	 * AVK, unconditionally. There is no selection here any more: the GLES
	 * recovery path it used to choose between is gone from the build, and a
	 * switch with one position is not a switch.
	 *
	 * It remains deliberately independent of WLR_RENDERER. wlroots still needs
	 * a renderer for the things that are not compositing -- shm formats, the
	 * allocator, screencopy -- and which one that is has no bearing on whether
	 * AVK builds the frame. `WLR_RENDERER=gles2` still yields a
	 * Vulkan-composited desktop with GLES2 sitting alongside, touching none of
	 * it, and that remains the test that proves the separation.
	 */
	avk.requested = true;
	/* The renderer's DRM node, not the backend's: this is the device the
	 * allocator allocates output buffers on, and AVK has to be on the same one
	 * to import them without a copy. */
	int32_t avk_fd = wlr_renderer_get_drm_fd(drw);
	if (avk_fd < 0)
		avk_fd = wlr_backend_get_drm_fd(backend);
	if (avk_fd < 0)
		wlr_log(WLR_ERROR, "AVK: no DRM node to bind to; a device will be "
				"picked instead, which is only appropriate in tests");
	if (!az_avk_init(avk_fd))
		die("the Vulkan engine could not start, and there is no other "
			"renderer to fall back to");
	wlr_log(WLR_INFO, "Asteroidz rendering backend: AVK native Vulkan");
	wlr_log(WLR_INFO, "wlroots compatibility renderer: %s -- protocols, "
			"allocation and screencopy only, no part of composition",
			getenv("WLR_RENDERER") ? getenv("WLR_RENDERER")
								   : "GLES2 (default)");

	/*
	 * linux-dmabuf, built AFTER the engine is chosen, because what a client
	 * should allocate depends on who is going to import it.
	 *
	 * In AVK mode the source of truth is avk_format_table -- probed with
	 * vkGetPhysicalDeviceImageFormatProperties2, not inferred -- and the main
	 * device is AVK's own DRM node. In wlr mode nothing changes: the same
	 * wlr_linux_dmabuf_v1_create_with_renderer() as before, so the recovery
	 * path neither gains a dependency on AVK nor needs it to have started.
	 *
	 * See src/render/az_dmabuf_caps.h for why this moved.
	 */
	if (wlr_renderer_get_texture_formats(drw, WLR_BUFFER_CAP_DMABUF)) {
		struct wlr_linux_dmabuf_v1 *linux_dmabuf = NULL;
		if (!az_dmabuf_break_use_gles()) {
			linux_dmabuf = az_dmabuf_create_from_avk(dpy);
			if (linux_dmabuf == NULL) {
				die("AVK is compositing but could not describe its own "
					"DMA-BUF capabilities; advertising the compatibility "
					"renderer's instead would tell clients to allocate "
					"buffers AVK may not be able to import");
			}
		}
		if (linux_dmabuf == NULL) {
			linux_dmabuf =
				wlr_linux_dmabuf_v1_create_with_renderer(dpy, 5, drw);
			wlr_log(WLR_INFO, "dmabuf: feedback source: wlroots renderer");
		}
		wlr_scene_set_linux_dmabuf_v1(scene, linux_dmabuf);
	}

	/* This creates some hands-off wlroots interfaces. The compositor is
	 * necessary for clients to allocate surfaces and the data device
	 * manager handles the clipboard. Each of these wlroots interfaces has
	 * room for you to dig your fingers in and play with their behavior if
	 * you want. Note that the clients cannot set the selection directly
	 * without compositor approval, see the setsel() function. */
	/*
	 * The renderer the wl_compositor global uploads client buffers with -- and
	 * in AVK mode, deliberately none.
	 *
	 * Passing a renderer here makes wlroots wrap every client buffer in a
	 * wlr_client_buffer, upload it into a wlr_texture, and then let go of the
	 * original. That is the right thing when a wlr_renderer draws the frame.
	 * It is exactly wrong when asteroidz draws the frame: the upload is a copy
	 * made by a renderer that will not be used, and the wrapper it produces
	 * can afterwards report neither a dma-buf nor readable pixels, so the
	 * client's actual content becomes unreachable. That is what made wallpapers
	 * vanish under AVK.
	 *
	 * With NULL, wlroots does its protocol bookkeeping and nothing else: the
	 * client's buffer stays locked in wlr_surface.current until the next
	 * commit, SceneFX hands it to the scene as-is (see the raw-buffer path in
	 * subprojects/asteroidz-scenefx/types/scene/surface.c), and AVK imports it
	 * directly. No texture is created and none is needed.
	 *
	 * `drw` itself stays. wlroots still wants a renderer for shm format
	 * advertisement, the allocator, screencopy and output cursors -- none of
	 * which are composition. This is chosen once, at startup, before any client
	 * can connect: switching it later would leave already-committed surfaces
	 * split between two ownership models.
	 */
	struct wlr_renderer *compositor_renderer = drw;
	/* AZ_AVK_COMPOSITOR_RENDERER=1 puts the wrapper topology back in AVK mode,
	 * so the test that proves AVK does not depend on wlroots' texture upload
	 * can be run against a build where it must fail. Without a break switch
	 * that assertion is unfalsifiable: the wallpaper renders, and nothing
	 * shows whether it renders *because* of the topology or in spite of it. */
	const char *force_wrapper = getenv("AZ_AVK_COMPOSITOR_RENDERER");
	if (force_wrapper == NULL || force_wrapper[0] != '1') {
		compositor_renderer = NULL;
		wlr_log(WLR_INFO, "wl_compositor renderer: none -- client buffers are "
				"imported by AVK, not uploaded by wlroots");
	}
	compositor = wlr_compositor_create(dpy, 6, compositor_renderer);
	/* Track what buffer each surface is showing, in BOTH renderer modes --
	 * see src/render/az_surface.h for why wlr_surface.buffer is not that. */
	wl_signal_add(&compositor->events.new_surface, &az_new_surface_listener);
	az_new_surface_attached = true;
	/* AVK takes ownership of a client's content at commit, which is the
	 * only moment it is guaranteed to be valid -- see the comment above
	 * az_avk_surface_commit(). Registered immediately after the global is
	 * created, so no surface can exist without the hook. */
	wl_signal_add(&compositor->events.new_surface,
				  &az_avk_new_surface_listener);
	az_avk_new_surface_attached = true;
	wlr_export_dmabuf_manager_v1_create(dpy);
	wlr_screencopy_manager_v1_create(dpy);
	struct wlr_ext_image_copy_capture_manager_v1 *img_copy_mgr =
		wlr_ext_image_copy_capture_manager_v1_create(dpy, 1);
	wl_signal_add(&img_copy_mgr->events.new_session,
				  &ext_image_copy_capture_new_session);
	wlr_ext_output_image_capture_source_manager_v1_create(dpy, 1);
	/* the per-window capture source, i.e. what a portal binds for "share a
	 * window" rather than "share a screen". Both source managers gate on the
	 * security-context filter in modern.h. */
	wlr_ext_foreign_toplevel_image_capture_source_manager_v1_create(dpy, 1);
	wlr_data_control_manager_v1_create(dpy);
	wlr_data_device_manager_create(dpy);
	// The middle-click "copy on select" buffer is a second, invisible
	// clipboard: selecting text overwrites it without anyone asking, and a
	// stray middle click pastes whatever it happens to hold. Not advertising
	// the protocol is what actually turns it off -- toolkits offer the
	// selection only when the compositor binds the global, so with this off
	// GTK/Qt clients stop publishing on select and middle-click paste does
	// nothing, leaving exactly one clipboard.
	if (config.primary_selection) {
		wlr_primary_selection_v1_device_manager_create(dpy);
	}
	wlr_viewporter_create(dpy);
	/* wl_fixes: gives clients wl_registry.destroy. Without it every registry a
	 * client creates is leaked server-side until it disconnects. */
	wlr_fixes_create(dpy, 1);
	wlr_single_pixel_buffer_manager_v1_create(dpy);
	wlr_fractional_scale_manager_v1_create(dpy, 1);
	wlr_presentation_create(dpy, backend, 2);
	wlr_subcompositor_create(dpy);
	wlr_alpha_modifier_v1_create(dpy);
	wlr_ext_data_control_manager_v1_create(dpy, 1);
	background_effect_manager_create(dpy);

	// inside the setup function
	wl_signal_init(&asteroidz_print_status);
	wl_signal_add(&asteroidz_print_status, &print_status_listener);

	/* Initializes the interface used to implement urgency hints */
	activation = wlr_xdg_activation_v1_create(dpy);

	struct wlr_xdg_wm_dialog_v1 *wm_dialog = wlr_xdg_wm_dialog_v1_create(dpy, 1);
	wl_signal_add(&wm_dialog->events.new_dialog, &new_xdg_dialog);
	wl_signal_add(&activation->events.request_activate, &request_activate);

	wlr_scene_set_gamma_control_manager_v1(
		scene, wlr_gamma_control_manager_v1_create(dpy));

	/* color-management-v1: advertise the color spaces and transfer
	 * functions the renderer can handle, so HDR clients can attach
	 * parametric (e.g. BT.2020 + PQ) image descriptions */
	{
		/*
		 * ── THE LIST DESCRIBES WHATEVER DECODES THE SURFACE ────────────────
		 *
		 * This is what a client may TAG ITS SURFACE WITH, so it has to describe
		 * the renderer that will decode that surface. Derived from `drw` it
		 * describes the wlroots compatibility renderer, which composites
		 * nothing -- the startup log a few hundred lines above says so in as
		 * many words: "protocols, allocation and screencopy only, no part of
		 * composition".
		 *
		 * That was not a small inaccuracy. The wlroots renderer's list came
		 * back EMPTY, so `wayland-info` reported no named transfer functions
		 * and no named primaries at all and NO CLIENT COULD ATTACH AN IMAGE
		 * DESCRIPTION OF ANY KIND. mpv would announce "transfer: pq,
		 * primaries: bt.2020", fail to create it, tone-map its own HDR10 down
		 * to SDR and hand over gamma2.2/BT.709 -- which the compositor then
		 * re-encoded to PQ for the panel. AVK's PQ decode was unreachable for
		 * that reason alone and nothing in the renderer could have fixed it.
		 *
		 * ── AND WHY THIS IS BUILT FROM wlroots ENUMS, NOT PROTOCOL ONES ────
		 *
		 * wlr_color_manager_v1_transfer_function_to_wlr() ABORTS when a
		 * protocol value has no matching wlroots entry, and scenefx calls it on
		 * whatever a client attaches (surface.c's surface_reconfigure). So
		 * advertising a protocol value wlroots cannot map does not degrade --
		 * it kills the compositor as soon as any client uses it, which at login
		 * is immediately.
		 *
		 * Starting from wlroots' OWN enum values and mapping outward with
		 * _from_wlr() (which has no such abort) makes that impossible by
		 * construction: every value advertised came from a wlroots entry, so a
		 * matching entry necessarily exists on the way back. The defensive
		 * alternative -- calling _to_wlr() to check -- would be calling the
		 * aborting function to find out whether it aborts.
		 */
		struct az_cm_caps caps;
		az_cm_caps_build(&caps, drw);

		/*
		 * THE COMPOSITOR'S OWN wp-color-management, and the only one.
		 *
		 * wlroots' implementation is not created and not kept as a fallback. It
		 * cannot serialize the mastering-luminance or content-light-level
		 * events -- two literal TODOs in its image_desc_handle_get_information
		 * -- which is the entire reason this exists; and it has no destroy
		 * function, so it lives until display teardown and could never have
		 * shared a session with a second manager anyway. Two live
		 * wp_color_manager_v1 globals would both appear in the registry with
		 * clients binding whichever they saw first.
		 *
		 * The capability set is built once, in az_cm_caps.h, from wlroots' own
		 * enums -- so every value advertised round-trips through _to_wlr(), the
		 * function that aborts on anything it cannot map. The lists are read on
		 * every bind for the life of the session and are deliberately never
		 * freed.
		 */
		if (!az_wpcm_create(dpy, &caps)) {
			wlr_log(WLR_ERROR, "colour management: the manager global could not "
					"be created; clients will not be able to describe their colour");
		}

		/* gamescope HDR passthrough: it can't use our wp-color-management
		 * (needs six features, wlroots implements two), but its frog path
		 * enables HDR as soon as we answer PQ. misc.frog-color-management
		 * (default on) lets this be turned off entirely -- e.g. to compare
		 * against, or if a user doesn't want gamescope HDR passthrough. */
		if (config.frog_color_management) {
			frog_color_management_init();
			/*
			 * gamescope's frog path is enabled by us answering PQ, and its
			 * wp-cm path needs six features we do not implement -- so wp-cm is
			 * hidden from it and frog is not. The filter keys on the GLOBAL,
			 * which used to be wlroots' and is now ours; leaving it pointing
			 * at the old one would have shown gamescope a manager it cannot
			 * use and taken away the one it can.
			 */
			filtered_wp_color_manager_global = az_wpcm_global;
		}
		/*
		 * ONE FALLBACK, BOTH PROTOCOLS, REGISTERED AFTER BOTH INITS.
		 *
		 * wlr_scene_set_surface_color_description_fallback takes a single
		 * callback and frog used to claim it directly. Native wp-cm needs to
		 * answer through the same slot, so registering here -- once, after
		 * both -- makes precedence a decision instead of a question of which
		 * init ran last.
		 */
		wlr_scene_set_surface_color_description_fallback(
			az_cm_surface_description);
	}

	modern_protocols_init(dpy, drw);

	wlr_scene_set_sdr_reference_luminance(scene,
										  config.sdr_reference_luminance);

	power_mgr = wlr_output_power_manager_v1_create(dpy);
	wl_signal_add(&power_mgr->events.set_mode, &output_power_mgr_set_mode);

	tearing_control = wlr_tearing_control_manager_v1_create(dpy, 1);
	tearing_new_object.notify = handle_tearing_new_object;
	wl_signal_add(&tearing_control->events.new_object, &tearing_new_object);

	/* Creates an output layout, which a wlroots utility for working with an
	 * arrangement of screens in a physical layout. */
	output_layout = wlr_output_layout_create(dpy);
	wl_signal_add(&output_layout->events.change, &layout_change);
	wlr_xdg_output_manager_v1_create(dpy, output_layout);

	/* Configure a listener to be notified when new outputs are available on
	 * the backend. */
	wl_list_init(&mons);
	wl_signal_add(&backend->events.new_output, &new_output);

	/* Set up our client lists and the xdg-shell. The xdg-shell is a
	 * Wayland protocol which is used for application windows. For more
	 * detail on shells, refer to the article:
	 *
	 * https://drewdevault.com/2018/07/29/Wayland-shells.html
	 */
	wl_list_init(&clients);
	wl_list_init(&fstack);
	wl_list_init(&fadeout_clients);
	wl_list_init(&fadeout_layers);

	idle_notifier = wlr_idle_notifier_v1_create(dpy);

	idle_inhibit_mgr = wlr_idle_inhibit_v1_create(dpy);
	wl_signal_add(&idle_inhibit_mgr->events.new_inhibitor, &new_idle_inhibitor);

	keep_idle_inhibit_source = wl_event_loop_add_timer(
		wl_display_get_event_loop(dpy), keep_idle_inhibit, NULL);

	/* v5 for set_exclusive_edge: a surface anchored to three edges can say
	 * which one its exclusive zone applies to instead of leaving us to guess.
	 * wlr_scene_layer_surface_v1_configure() honours it for us. */
	layer_shell = wlr_layer_shell_v1_create(dpy, 5);
	wl_signal_add(&layer_shell->events.new_surface, &new_layer_surface);

	xdg_shell = wlr_xdg_shell_create(dpy, 7);
	wl_signal_add(&xdg_shell->events.new_toplevel, &new_xdg_toplevel);
	wl_signal_add(&xdg_shell->events.new_popup, &new_xdg_popup);

	session_lock_mgr = wlr_session_lock_manager_v1_create(dpy);
	wl_signal_add(&session_lock_mgr->events.new_lock, &new_session_lock);

	locked_bg =
		wlr_scene_rect_create(layers[LyrBlock], sgeom.width, sgeom.height,
							  (float[4]){0.1, 0.1, 0.1, 1.0});
	wlr_scene_node_set_enabled(&locked_bg->node, false);

	/* Use decoration protocols to negotiate server-side decorations */
	wlr_server_decoration_manager_set_default_mode(
		wlr_server_decoration_manager_create(dpy),
		WLR_SERVER_DECORATION_MANAGER_MODE_SERVER);
	xdg_decoration_mgr = wlr_xdg_decoration_manager_v1_create(dpy);
	wl_signal_add(&xdg_decoration_mgr->events.new_toplevel_decoration,
				  &new_xdg_decoration);

	pointer_constraints = wlr_pointer_constraints_v1_create(dpy);
	wl_signal_add(&pointer_constraints->events.new_constraint,
				  &new_pointer_constraint);

	relative_pointer_mgr = wlr_relative_pointer_manager_v1_create(dpy);

	/*
	 * Creates a cursor, which is a wlroots utility for tracking the cursor
	 * image shown on screen.
	 */
	cursor = wlr_cursor_create();
	wlr_cursor_attach_output_layout(cursor, output_layout);

	/* Creates an xcursor manager, another wlroots utility which loads up
	 * Xcursor themes to source cursor images from and makes sure that
	 * cursor images are available at all scale factors on the screen
	 * (necessary for HiDPI support). Scaled cursors will be loaded with
	 * each output. */

	set_xcursor_env();

	cursor_mgr =
		wlr_xcursor_manager_create(config.cursor_theme, config.cursor_size);
	/*
	 * wlr_cursor *only* displays an image on screen. It does not move
	 * around when the pointer moves. However, we can attach input devices
	 * to it, and it will generate aggregate events for all of them. In
	 * these events, we can choose how we want to process them, forwarding
	 * them to clients and moving the cursor around. More detail on this
	 * process is described in my input handling blog post:
	 *
	 * https://drewdevault.com/2018/07/17/Input-handling-in-wlroots.html
	 *
	 * And more comments are sprinkled throughout the notify functions
	 * above.
	 */
	wl_signal_add(&cursor->events.motion, &cursor_motion);
	wl_signal_add(&cursor->events.motion_absolute, &cursor_motion_absolute);
	wl_signal_add(&cursor->events.button, &cursor_button);
	wl_signal_add(&cursor->events.axis, &cursor_axis);
	wl_signal_add(&cursor->events.frame, &cursor_frame);
	wl_signal_add(&cursor->events.tablet_tool_proximity,
				  &tablet_tool_proximity);
	wl_signal_add(&cursor->events.tablet_tool_axis, &tablet_tool_axis);
	wl_signal_add(&cursor->events.tablet_tool_button, &tablet_tool_button);
	wl_signal_add(&cursor->events.tablet_tool_tip, &tablet_tool_tip);

	// these two lines make the cursor disappear inside OBS windows; unclear what commenting them out would affect
	cursor_shape_mgr = wlr_cursor_shape_manager_v1_create(dpy, 2);
	wl_signal_add(&cursor_shape_mgr->events.request_set_shape,
				  &request_set_cursor_shape);
	hide_cursor_source = wl_event_loop_add_timer(wl_display_get_event_loop(dpy),
												 hidecursor, cursor);
	scroller_edge_scroll_source = wl_event_loop_add_timer(
		wl_display_get_event_loop(dpy), scroller_edge_scroll_timeout, NULL);
	/*
	 * Configures a seat, which is a single "seat" at which a user sits and
	 * operates the computer. This conceptually includes up to one keyboard,
	 * pointer, touch, and drawing tablet device. We also rig up a listener
	 * to let us know when new input devices are available on the backend.
	 */
	wl_list_init(&inputdevices);
	wl_list_init(&tablets);
	wl_list_init(&tablet_pads);
	wl_list_init(&keyboard_shortcut_inhibitors);
	wl_signal_add(&backend->events.new_input, &new_input_device);
	virtual_keyboard_mgr = wlr_virtual_keyboard_manager_v1_create(dpy);
	wl_signal_add(&virtual_keyboard_mgr->events.new_virtual_keyboard,
				  &new_virtual_keyboard);
	virtual_pointer_mgr = wlr_virtual_pointer_manager_v1_create(dpy);
	wl_signal_add(&virtual_pointer_mgr->events.new_virtual_pointer,
				  &new_virtual_pointer);

	pointer_gestures = wlr_pointer_gestures_v1_create(dpy);
	LISTEN_STATIC(&cursor->events.swipe_begin, swipe_begin);
	LISTEN_STATIC(&cursor->events.swipe_update, swipe_update);
	LISTEN_STATIC(&cursor->events.swipe_end, swipe_end);
	LISTEN_STATIC(&cursor->events.pinch_begin, pinch_begin);
	LISTEN_STATIC(&cursor->events.pinch_update, pinch_update);
	LISTEN_STATIC(&cursor->events.pinch_end, pinch_end);
	LISTEN_STATIC(&cursor->events.hold_begin, hold_begin);
	LISTEN_STATIC(&cursor->events.hold_end, hold_end);

	seat = wlr_seat_create(dpy, "seat0");

	wl_list_init(&last_cursor_surface_destroy_listener.link);
	wl_signal_add(&seat->events.request_set_cursor, &request_cursor);
	wl_signal_add(&seat->events.request_set_selection, &request_set_sel);
	wl_signal_add(&seat->events.request_set_primary_selection,
				  &request_set_psel);
	wl_signal_add(&seat->events.request_start_drag, &request_start_drag);
	wl_signal_add(&seat->events.start_drag, &start_drag);

	kb_group = createkeyboardgroup();
	wl_list_init(&kb_group->destroy.link);

	keyboard_shortcuts_inhibit = wlr_keyboard_shortcuts_inhibit_v1_create(dpy);
	wl_signal_add(&keyboard_shortcuts_inhibit->events.new_inhibitor,
				  &keyboard_shortcuts_inhibit_new_inhibitor);

	output_mgr = wlr_output_manager_v1_create(dpy);
	wl_signal_add(&output_mgr->events.apply, &output_mgr_apply);
	wl_signal_add(&output_mgr->events.test, &output_mgr_test);

	wlr_scene_set_blur_data(
		scene, config.blur_params.num_passes, config.blur_params.radius,
		config.blur_params.noise, config.blur_params.brightness,
		config.blur_params.contrast, config.blur_params.saturation,
		config.blur_params.transparency_threshold);

	/* Plot appearance, declared once. Without this the viewer picks defaults
	 * per plot and the render-late curves come out as unconnected points at
	 * wildly different scales, which is unreadable for a control loop. Step
	 * (not smoothed) is the honest shape: these values change at discrete
	 * frames, they do not interpolate between them. */

	/* create text_input-, and input_method-protocol relevant globals */
	input_method_manager = wlr_input_method_manager_v2_create(dpy);
	text_input_manager = wlr_text_input_manager_v3_create(dpy);

	dwl_input_method_relay = dwl_im_relay_create();

	drm_lease_manager = wlr_drm_lease_v1_manager_create(dpy, backend);
	if (drm_lease_manager) {
		wl_signal_add(&drm_lease_manager->events.request, &drm_lease_request);
	} else {
		wlr_log(WLR_DEBUG, "Failed to create wlr_drm_lease_device_v1.");
		wlr_log(WLR_INFO, "VR will not be available.");
	}

	wl_global_create(dpy, &zdwl_ipc_manager_v2_interface, 2, NULL,
					 dwl_ipc_manager_bind);

	// create the toplevel management handle
	foreign_toplevel_manager = wlr_foreign_toplevel_manager_v1_create(dpy);
	struct wlr_xdg_foreign_registry *foreign_registry =
		wlr_xdg_foreign_registry_create(dpy);
	wlr_xdg_foreign_v1_create(dpy, foreign_registry);
	wlr_xdg_foreign_v2_create(dpy, foreign_registry);

	// ext-workspace protocol
	workspaces_init();
#ifdef XWAYLAND
	/*
	 * Initialise the XWayland X server.
	 * It will be started when the first X client is started.
	 */
	xwayland =
		wlr_xwayland_create(dpy, compositor, !config.xwayland_persistence);
	if (xwayland) {
		wl_signal_add(&xwayland->events.ready, &xwayland_ready);
		wl_signal_add(&xwayland->events.new_surface, &new_xwayland_surface);

		setenv("DISPLAY", xwayland->display_name, 1);
	} else {
		fprintf(stderr,
				"failed to setup XWayland X server, continuing without it\n");
	}
	sync_keymap = wl_event_loop_add_timer(wl_display_get_event_loop(dpy),
										  synckeymap, NULL);
#endif
}

void startdrag(struct wl_listener *listener, void *data) {
	struct wlr_drag *drag = data;
	if (!drag->icon)
		return;

	drag->icon->data = &wlr_scene_drag_icon_create(drag_icon, drag->icon)->node;
	LISTEN_STATIC(&drag->icon->events.destroy, destroydragicon);
}

void tag_client(const Arg *arg, Client *target_client) {
	Client *fc = NULL;
	if (target_client && arg->ui & TAGMASK) {

		target_client->tags = arg->ui & TAGMASK;
		target_client->istagswitching = 1;

		wl_list_for_each(fc, &clients, link) {
			if (fc && fc != target_client && target_client->tags & fc->tags &&
				ISFULLSCREEN(fc) && !target_client->isfloating) {
				clear_fullscreen_flag(fc);
			}
		}
		view(&(Arg){.ui = arg->ui, .i = arg->i}, true);

	} else {
		view(arg, true);
	}

	focusclient(target_client, 1);
	printstatus(IPC_WATCH_ARRANGGE);
}

// return 0 if another window shares the same tag as the target window
uint32_t want_restore_fullscreen(Client *target_client) {
	Client *c = NULL;
	wl_list_for_each(c, &clients, link) {
		if (c && c != target_client && c->tags == target_client->tags &&
			c == selmon->sel &&
			c->mon->pertag->ltidxs[get_tags_first_tag_num(c->tags)]->id !=
				SCROLLER) {
			return 0;
		}
	}

	return 1;
}

void overview_backup_surface(Client *c) {
	/* Overview thumbnails show the LIVE surface, scaled down into its cell
	 * (see the ov_no_resize live-scaling path in client_apply_clip), rather
	 * than a static snapshot. A snapshot froze each thumbnail at the instant
	 * the overview opened and cut the app's frame callbacks; keeping the real
	 * surface means video, terminals, etc. keep updating in the overview.
	 * Left as a no-op (rather than deleting call sites) so the overview
	 * enter/map paths that call it stay simple. */
	(void)c;
}

// save the window's old state when switching from the normal view to overview
void overview_backup(Client *c) {
	c->overview_isfloatingbak = c->isfloating;
	c->overview_isfullscreenbak = c->isfullscreen;
	c->overview_ismaximizescreenbak = c->ismaximizescreen;
	c->overview_isfullscreenbak = c->isfullscreen;
	c->animation.tagining = false;
	c->animation.tagouted = false;
	c->animation.tagouting = false;
	c->overview_backup_geom = c->geom;
	c->overview_backup_bw = c->bw;
	/* freeze the window's current pixels for the strip thumbnail: a static
	 * screenshot of the tag as it was when the overview opened */
	if (c->ov_snap_buf) {
		wlr_buffer_unlock(c->ov_snap_buf);
		c->ov_snap_buf = NULL;
	}
	{
		struct wlr_surface *ov_surf = client_surface(c);
		if (ov_surf && ov_surf->buffer)
			c->ov_snap_buf = wlr_buffer_lock(&ov_surf->buffer->base);
	}
	if (c->isfloating) {
		c->isfloating = 0;
		/* floating windows live on LyrTop (or LyrOverlay), which overview
		 * disables to hide the bar -- move them to LyrTile so they show */
		if (c->scene)
			wlr_scene_node_reparent(&c->scene->node, layers[LyrTile]);
	}

	if (config.ov_no_resize) {
		overview_backup_surface(c);
	}

	if (c->isfullscreen || c->ismaximizescreen) {
		client_pending_fullscreen_state(c, 0); // clear the window's fullscreen flag
		client_pending_maximized_state(c, 0);
		/* a fullscreen window lives on LyrFS (maximized on LyrMaximize);
		 * the overview lays its mirror out on LyrTile, and clearing the flag
		 * doesn't reparent the node, so do it here (as we do for floating). */
		if (c->scene)
			wlr_scene_node_reparent(&c->scene->node, layers[LyrTile]);
	}
	c->bw = c->isnoborder ? 0 : config.borderpx;

	client_set_tiled(c, WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT |
							WLR_EDGE_RIGHT);
}

// restore the window's state when switching from overview back to the normal view
void overview_restore(Client *c, const Arg *arg) {
	/* undo any overview scene-node hide; the following view()/arrange re-runs
	 * the normal visibility logic (clip_to_hide / tag show) */
	if (c->is_overview_hidden) {
		c->is_overview_hidden = false;
		client_set_scene_enabled(c, true);
	}
	if (c->ov_icon)
		wlr_scene_node_set_enabled(&c->ov_icon->scene_buffer->node, false);
	if (c->ov_title)
		wlr_scene_node_set_enabled(&c->ov_title->scene_buffer->node, false);
	if (c->ov_snap_buf) {
		wlr_buffer_unlock(c->ov_snap_buf);
		c->ov_snap_buf = NULL;
	}
	c->ov_clip_active = false;
	c->isfloating = c->overview_isfloatingbak;
	/* undo the overview reparent: send floating windows back to their layer */
	if (c->isfloating && c->scene)
		wlr_scene_node_reparent(&c->scene->node,
								layers[c->isoverlay ? LyrOverlay : LyrTop]);
	c->isfullscreen = c->overview_isfullscreenbak;
	c->ismaximizescreen = c->overview_ismaximizescreenbak;
	c->overview_isfloatingbak = 0;
	c->overview_isfullscreenbak = 0;
	c->overview_ismaximizescreenbak = 0;
	c->geom = c->overview_backup_geom;
	c->bw = c->overview_backup_bw;
	c->animation.tagining = false;
	c->is_restoring_from_ov = (arg->ui & c->tags & TAGMASK) == 0 ? true : false;

	if (c->overview_scene_surface) {
		wlr_scene_node_reparent(&c->shield->node, c->overview_scene_surface);
		wlr_scene_node_raise_to_top(&c->shield->node);
		wlr_scene_node_destroy(&c->scene_surface->node);
		c->scene_surface = c->overview_scene_surface;
		c->overview_scene_surface = NULL;
	}

	if (c->isfloating) {
		// XRaiseWindow(dpy, c->win); // raise the floating window to the top
		resize(c, c->overview_backup_geom, 0);
	} else if (c->isfullscreen || c->ismaximizescreen) {
		if (want_restore_fullscreen(c) && c->ismaximizescreen) {
			setmaximizescreen(c, 1, false);
		} else if (want_restore_fullscreen(c) && c->isfullscreen) {
			setfullscreen(c, 1, false);
		} else {
			client_pending_fullscreen_state(c, 0);
			client_pending_maximized_state(c, 0);
			setfullscreen(c, false, false);
		}
	} else {
		if (c->is_restoring_from_ov) {
			c->is_restoring_from_ov = false;
			resize(c, c->overview_backup_geom, 0);
		}
	}

	if (c->bw == 0 &&
		!c->isfullscreen) { // if this window was created while in overview mode, it has no recorded bw
		c->bw = c->isnoborder ? 0 : config.borderpx;
	}

	if (c->isfloating && !c->force_tiled_state) {
		client_set_tiled(c, WLR_EDGE_NONE);
	}
}

void handlecursoractivity(void) {
	wl_event_source_timer_update(hide_cursor_source,
								 config.cursor_hide_timeout * 1000);

	if (!cursor_hidden)
		return;

	cursor_hidden = false;

	if (last_cursor.shape)
		az_cursor_set_xcursor(wlr_cursor_shape_v1_name(last_cursor.shape));
	else if (last_cursor.surface)
		az_cursor_set_surface(last_cursor.surface, last_cursor.hotspot_x,
							  last_cursor.hotspot_y);
	else
		az_cursor_show();
}

int32_t hidecursor(void *data) {
	az_cursor_hide();
	cursor_hidden = true;
	return 1;
}

struct capture_session_tracker {
	struct wlr_ext_image_copy_capture_session_v1 *session;
	struct wl_listener session_destroy;
	Monitor *mon; /* resolved at session start; NULL if not an output source */
};

/* re-evaluate every shielded surface's cover state */
void refresh_shielded_surfaces(void) {
	Client *c = NULL;
	LayerSurface *l = NULL;
	Monitor *m = NULL;

	wl_list_for_each(c, &clients, link) {
		if (c->privacy_shield && !c->iskilling && c->mon &&
			VISIBLEON(c, c->mon)) {
			arrange(c->mon, false, false);
		}
	}
	wl_list_for_each(m, &mons, link) {
		if (!m->wlr_output->enabled)
			continue;
		for (size_t i = 0; i < 4; i++) {
			wl_list_for_each(l, &m->layers[i], link) {
				if (l->privacy_shield)
					layer_draw_shield(l);
			}
		}
	}
}

static void handle_capture_session_destroy(struct wl_listener *listener,
										   void *data) {
	struct capture_session_tracker *tracker =
		wl_container_of(listener, tracker, session_destroy);
	active_capture_count--;
	wl_list_remove(&tracker->session_destroy.link);
	refresh_shielded_surfaces();
	wlr_log(WLR_DEBUG, "capture session ended, active count: %d",
			active_capture_count);

	free(tracker);
}

void handle_image_copy_capture_new_session(struct wl_listener *listener,
										   void *data) {
	struct wlr_ext_image_copy_capture_session_v1 *session = data;

	struct capture_session_tracker *tracker = calloc(1, sizeof(*tracker));
	if (!tracker)
		return;
	tracker->session = session;
	tracker->session_destroy.notify = handle_capture_session_destroy;
	wl_signal_add(&session->events.destroy, &tracker->session_destroy);

	struct wlr_output *capture_output =
		wlr_output_try_from_ext_image_capture_source_v1(session->source);
	tracker->mon = capture_output ? capture_output->data : NULL;

	active_capture_count++;
	refresh_shielded_surfaces();
	wlr_log(WLR_DEBUG, "capture session started, active count: %d",
			active_capture_count);

}

/* True while any client carrying the force_hdr window rule is visible on m.
 * "Visible" (not focused, not fullscreen) is deliberate: video keeps its HDR
 * treatment when you click away from it, so alt-tabbing doesn't strobe the
 * display through a modeset each way. */
static bool mon_has_force_hdr_client(Monitor *m) {
	Client *c;
	wl_list_for_each(c, &clients, link) {
		if (c->force_hdr > 0 && c->mon == m && !c->iskilling &&
			VISIBLEON(c, m))
			return true;
	}
	return false;
}

/* Single writer for m->hdr. Folds the three inputs -- global policy, the
 * output's own configured intent, and the two overrides -- into one effective
 * value, then schedules a commit only when it actually changed.
 *
 * The precedence itself is stated at the branch below, where it can be read
 * against the code that implements it. */
static void hdr_resolve(Monitor *m) {
	if (!m || !m->wlr_output || !m->wlr_output->enabled)
		return;

	/*
	 * ── AN EXPLICIT PER-OUTPUT CHOICE OUTRANKS THE GLOBAL DEFAULT ─────────
	 *
	 * `hdr-mode` is a POLICY for outputs that have not been spoken for. It used
	 * to be an absolute override, which made `hdr-mode on` silently defeat
	 * set_output_hdr and toggle_hdr entirely: the dispatch set the baseline,
	 * hdr_resolve immediately re-asserted HDR, and the only trace was a log
	 * line saying the request was overridden "for now". On a desktop with
	 * `hdr-mode on` the toggle had never worked at all.
	 *
	 * Order now:
	 *   1. the output cannot do BT.2020+PQ    -> off, and nothing overrides it
	 *   2. `hdr-mode off`                     -> off; a kill switch is absolute
	 *   3. an explicit per-output choice      -> honoured, either way
	 *   4. a force_hdr client                 -> on
	 *   5. `hdr-mode on`                      -> on, as the default
	 *   6. otherwise                          -> off
	 */
	bool want;
	if (m->hdr_capability_failed || config.hdr_mode == 0) {
		want = false;
	} else if (m->hdr_configured >= 0) {
		want = m->hdr_configured > 0 || mon_has_force_hdr_client(m);
	} else if (mon_has_force_hdr_client(m)) {
		want = true;
	} else {
		want = config.hdr_mode == 2;
	}

	if (want == (m->hdr > 0))
		return;

	m->hdr = want ? 1 : 0;
	m->hdr_pending_change = true;
	wlr_output_schedule_frame(m->wlr_output);
	wlr_log(WLR_INFO, "HDR %s on %s", want ? "enabled" : "disabled",
			m->wlr_output->name);
	printstatus(IPC_WATCH_ARRANGGE);
}

/* Re-evaluate every output. Cheap (a handful of outputs, one client walk
 * each) and only commits where the effective value actually moved. */
static void hdr_resolve_all(void) {
	Monitor *m;
	wl_list_for_each(m, &mons, link)
		hdr_resolve(m);
}

static void commit_vrr_state(Monitor *m, bool enable) {
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	if (enable)
		enable_adaptive_sync(m, &state);
	else
		disable_adaptive_sync(m, &state);
	wlr_output_commit_state(m->wlr_output, &state);
	wlr_output_state_finish(&state);
}

/* dynamic VRR: follow focus. vrr_global_enable keeps it always on; a
 * vrr_only_fullscreen window rule turns it on while that client is
 * fullscreen; focusing nothing (empty tag) turns it back off. */
void check_vrr_enable(Client *c) {
	Monitor *m = c && c->mon ? c->mon : selmon;

	if (!m || !m->wlr_output || !m->wlr_output->enabled)
		return;

	if (!c) {
		if (m->is_vrr_opening && !m->vrr_global_enable)
			commit_vrr_state(m, false);
		return;
	}

	/* m, not c->mon, for the rest of this function. The two are the same
	 * whenever the client has a monitor; when it does not, m is the selmon
	 * fallback resolved above and c->mon is NULL. Dereferencing c->mon here
	 * crashed: setfakefullscreen() calls this before the client is placed --
	 * the arrange(c->mon, ...) four lines below its call already treats that
	 * monitor as possibly-NULL -- and the guard above only rejects the case
	 * where selmon is ALSO gone, so a running session with an unplaced client
	 * walked straight into it. */
	/*
	 * ── M13: A GAME GETS VRR WITHOUT NEEDING A RULE PER GAME ──────────────
	 *
	 * vrr_only_fullscreen still means exactly what it meant, and a rule that
	 * sets it still works. What is generalised is the OTHER way in: a window
	 * whose presentation class is GAME -- because it said so through
	 * wp-content-type, or because a presentation-class rule says so -- wants
	 * VRR fullscreen for the same reason, and having to name every game in the
	 * config was the gap.
	 *
	 * STILL GATED ON FULLSCREEN, for both. A windowed game shares the output
	 * with a blinking cursor and a clock, and letting it drive the refresh rate
	 * makes everything else on that display stutter. Fullscreen is what makes
	 * "this client's cadence is the output's cadence" true.
	 */
	bool wants_vrr = c->isfullscreen
		&& (c->vrr_only_fullscreen
			|| az_present_class_of(c, NULL) == AZ_PRESENT_CLASS_GAME);

	if (VISIBLEON(c, m) && wants_vrr && !m->is_vrr_opening) {
		commit_vrr_state(m, true);
		return;
	}

	if (!m->is_vrr_opening && m->vrr_global_enable) {
		commit_vrr_state(m, true);
	} else if (m->is_vrr_opening && !m->vrr_global_enable && !wants_vrr) {
		commit_vrr_state(m, false);
	}
}

static int32_t float_focus_raise_timeout(void *data) {
	(void)data;
	if (float_focus_raise_pending && !float_focus_raise_pending->iskilling &&
		client_surface(float_focus_raise_pending)->mapped) {
		wlr_scene_node_raise_to_top(&float_focus_raise_pending->scene->node);
	}
	float_focus_raise_pending = NULL;
	return 0;
}

/* Schedules (or reschedules, if one was already pending for a different
 * client) the debounced float-layout auto-raise-on-focus for [c] -- see the
 * comment on float_focus_raise_timer's declaration. */
static void schedule_float_focus_raise(Client *c) {
	if (!float_focus_raise_timer) {
		float_focus_raise_timer = wl_event_loop_add_timer(
			wl_display_get_event_loop(dpy), float_focus_raise_timeout, NULL);
	}
	float_focus_raise_pending = c;
	wl_event_source_timer_update(float_focus_raise_timer, 250);
}

void check_keep_idle_inhibit(Client *c) {
	if (c && c->idleinhibit_when_focus && keep_idle_inhibit_source) {
		wl_event_source_timer_update(keep_idle_inhibit_source, 1000);
	}
}

int32_t keep_idle_inhibit(void *data) {

	if (!idle_inhibit_mgr) {
		wl_event_source_timer_update(keep_idle_inhibit_source, 0);
		return 1;
	}

	if (session && !session->active) {
		wl_event_source_timer_update(keep_idle_inhibit_source, 0);
		return 1;
	}

	if (!selmon || !selmon->sel || !selmon->sel->idleinhibit_when_focus) {
		wl_event_source_timer_update(keep_idle_inhibit_source, 0);
		return 1;
	}

	if (seat && idle_notifier) {
		wlr_idle_notifier_v1_notify_activity(idle_notifier, seat);
		wl_event_source_timer_update(keep_idle_inhibit_source, 1000);
	}
	return 1;
}

void unlocksession(struct wl_listener *listener, void *data) {
	SessionLock *lock = wl_container_of(listener, lock, unlock);
	destroylock(lock, 1);
}

void unmaplayersurfacenotify(struct wl_listener *listener, void *data) {
	LayerSurface *l = wl_container_of(listener, l, unmap);

	l->mapped = 0;
	l->being_unmapped = true;

	init_fadeout_layers(l);

	wlr_scene_node_set_enabled(&l->scene->node, false);

	if (l == exclusive_focus)
		exclusive_focus = NULL;

	if (l->layer_surface->output && (l->mon = l->layer_surface->output->data))
		arrangelayers(l->mon);

	reset_exclusive_layers_focus(l->mon);

	motionnotify(0, NULL, 0, 0, 0, 0);
	layer_flush_blur_background(l);
	wlr_scene_node_destroy(&l->shadow->node);
	l->shadow = NULL;
	if (l->shadow_blur) {
		wlr_scene_node_destroy(&l->shadow_blur->node);
		l->shadow_blur = NULL;
	}
	l->being_unmapped = false;
}

void unmapnotify(struct wl_listener *listener, void *data) {
	/* Called when the surface is unmapped, and should no longer be shown.
	 */
	Client *c = wl_container_of(listener, c, unmap);
	Monitor *m = NULL;
	Client *nextfocus = NULL;
	c->iskilling = 1;
	struct ScrollerStackNode *target_node =
		c->mon ? find_scroller_node(
					 c->mon->pertag->scroller_state[c->mon->pertag->curtag], c)
			   : NULL;
	struct ScrollerStackNode *prev_node =
		target_node ? target_node->prev_in_stack : NULL;
	struct ScrollerStackNode *next_node =
		target_node ? target_node->next_in_stack : NULL;

	if (config.animations && !c->is_clip_to_hide && !c->isminimized &&
		(!c->mon || VISIBLEON(c, c->mon)))
		init_fadeout_client(c);

	// If the client is in a stack, remove it from the stack

	if (c->swallowedby) {
		c->swallowedby->mon = c->mon;
		client_replace(c->swallowedby, c);
	} else {
		scroller_remove_client(c);
		dwindle_remove_client(c);
	}

	if (c == grabc) {
		cursor_mode = CurNormal;
		grabc = NULL;
	}

	if (c == dropc) {
		dropc = NULL;
	}

	wl_list_for_each(m, &mons, link) {
		if (!m->wlr_output->enabled) {
			continue;
		}
		if (c == m->sel) {
			m->sel = NULL;
		}
		if (c == m->prevsel) {
			m->prevsel = NULL;
		}
	}

	if (c->mon && c->mon == selmon) {
		if (next_node && !c->swallowedby) {
			nextfocus = next_node->client;
		} else if (prev_node && !c->swallowedby) {
			nextfocus = prev_node->client;
		} else {
			nextfocus = focustop(selmon);
		}

		if (nextfocus) {
			focusclient(nextfocus, 0);
		}

		if (!nextfocus && selmon->isoverview) {
			Arg arg = {0};
			toggleoverview(&arg);
		}
	}

	if (client_is_unmanaged(c)) {
#ifdef XWAYLAND
		if (client_is_x11(c)) {
			if (c->set_geometry.link.prev && c->set_geometry.link.next &&
				c->set_geometry.link.prev != &c->set_geometry.link) {
				wl_list_remove(&c->set_geometry.link);
				wl_list_init(&c->set_geometry.link);
			}
		}
#endif
		if (c == exclusive_focus)
			exclusive_focus = NULL;
		if (client_surface(c) == seat->keyboard_state.focused_surface)
			focusclient(focustop(selmon), 1);
	} else {

		if (!c->swallowing) {
			if (c->link.prev && c->link.next && c->link.prev != &c->link) {
				wl_list_remove(&c->link);
				wl_list_init(&c->link);
			}
		}
		setmon(c, NULL, 0, true);
		if (!c->swallowing) {
			if (c->flink.prev && c->flink.next && c->flink.prev != &c->flink) {
				wl_list_remove(&c->flink);
				wl_list_init(&c->flink);
			}
		}
	}

	client_remove_ext_foreign_toplevel(c);
	if (c->foreign_toplevel) {
		wlr_foreign_toplevel_handle_v1_destroy(c->foreign_toplevel);
		c->foreign_toplevel = NULL;
	}

	if (c->swallowedby) {
		setmaximizescreen(c->swallowedby, c->ismaximizescreen, true);
		setfullscreen(c->swallowedby, c->isfullscreen, true);
		c->swallowedby->swallowing = NULL;
		c->swallowedby = NULL;
	}

	if (c->swallowing) {
		c->swallowing->swallowedby = NULL;
		c->swallowing = NULL;
	}

	c->stack_proportion = 0.0f;

	if (c->jump_label_node) {
		asteroidz_jump_label_node_destroy(c->jump_label_node);
		c->jump_label_node = NULL;
	}
	if (c->titlebar_node) {
		asteroidz_tab_bar_node_destroy(c->titlebar_node);
		c->titlebar_node = NULL;
	}
	if (c->titlebar_close_node) {
		asteroidz_tab_bar_node_destroy(c->titlebar_close_node);
		c->titlebar_close_node = NULL;
	}
	if (c->ov_icon) {
		asteroidz_icon_node_destroy(c->ov_icon);
		c->ov_icon = NULL;
	}
	if (c->ov_title) {
		asteroidz_jump_label_node_destroy(c->ov_title);
		c->ov_title = NULL;
	}
	if (c->ov_snap_buf) {
		wlr_buffer_unlock(c->ov_snap_buf);
		c->ov_snap_buf = NULL;
	}

	/* Before c->scene, and unconditionally: the shadow tree is a sibling of
	 * the window's tree rather than a child of it while the window is tiled,
	 * so destroying c->scene does not take it with it. Missing this leaves a
	 * window's shadow on screen after the window is gone. Destroying the tree
	 * destroys shadow, contact_shadow and shadow_blur with it -- the pointers
	 * are cleared so nothing between here and free(c) can follow them. */
	if (c->shadow_tree) {
		wlr_scene_node_destroy(&c->shadow_tree->node);
		c->shadow_tree = NULL;
		c->shadow = NULL;
		c->contact_shadow = NULL;
		c->shadow_blur = NULL;
	}

	wlr_scene_node_destroy(&c->scene->node);
	printstatus(IPC_WATCH_ARRANGGE);
	motionnotify(0, NULL, 0, 0, 0, 0);
}

void updatemons(struct wl_listener *listener, void *data) {
	/*
	 * Called whenever the output layout changes: adding or removing a
	 * monitor, changing an output's mode or position, etc. This is where
	 * the change officially happens and we update geometry, window
	 * positions, focus, and the stored configuration in wlroots'
	 * output-manager implementation.
	 */
	struct wlr_output_configuration_v1 *output_config =
		wlr_output_configuration_v1_create();
	Client *c = NULL;
	struct wlr_output_configuration_head_v1 *config_head;
	Monitor *m = NULL;
	int32_t mon_pos_offsetx, mon_pos_offsety, oldx, oldy;

	/* First remove from the layout the disabled monitors */
	wl_list_for_each(m, &mons, link) {
		if (m->wlr_output->enabled || m->asleep)
			continue;
		config_head = wlr_output_configuration_head_v1_create(output_config,
															  m->wlr_output);
		config_head->state.enabled = 0;
		/* Remove this output from the layout to avoid cursor enter inside
		 * it */
		wlr_output_layout_remove(output_layout, m->wlr_output);
		closemon(m);
		m->m = m->w = (struct wlr_box){0};
	}
	/* Insert outputs that need to */
	wl_list_for_each(m, &mons, link) {
		if (m->wlr_output->enabled &&
			!wlr_output_layout_get(output_layout, m->wlr_output))
			wlr_output_layout_add_auto(output_layout, m->wlr_output);
	}

	/* Now that we update the output layout we can get its box */
	wlr_output_layout_get_box(output_layout, NULL, &sgeom);

	wlr_scene_node_set_position(&root_bg->node, sgeom.x, sgeom.y);
	wlr_scene_rect_set_size(root_bg, sgeom.width, sgeom.height);

	/* Make sure the clients are hidden when dwl is locked */
	wlr_scene_node_set_position(&locked_bg->node, sgeom.x, sgeom.y);
	wlr_scene_rect_set_size(locked_bg, sgeom.width, sgeom.height);

	wl_list_for_each(m, &mons, link) {
		if (!m->wlr_output->enabled)
			continue;
		config_head = wlr_output_configuration_head_v1_create(output_config,
															  m->wlr_output);

		oldx = m->m.x;
		oldy = m->m.y;
		/* Get the effective monitor geometry to use for surfaces */
		wlr_output_layout_get_box(output_layout, m->wlr_output, &m->m);
		m->w = m->m;
		mon_pos_offsetx = m->m.x - oldx;
		mon_pos_offsety = m->m.y - oldy;

		wl_list_for_each(c, &clients, link) {
			// floating window position auto adjust the change of monitor
			// position
			if (c->isfloating && c->mon == m) {
				c->geom.x += mon_pos_offsetx;
				c->geom.y += mon_pos_offsety;
				c->float_geom = c->geom;
				if (VISIBLEON(c, m))
					resize(c, c->geom, 1);
			}

			// restore window to old monitor, on its original tag. Use the
			// recorded oldmontags, not c->tags: if this monitor was ever
			// disconnected, c->tags was already remapped (closemon ->
			// client_change_mon newtags=0) to whatever tag was active on
			// the temporary monitor it landed on meanwhile, and m -- if
			// reconnecting -- is a brand-new Monitor with a fresh pertag
			// defaulting to tag 1, so c->tags would very likely no longer
			// match any tag actually shown here.
			if (c->mon && c->mon != m && client_surface(c)->mapped &&
				strcmp(c->oldmonname, m->wlr_output->name) == 0) {
				client_change_mon(c, m, c->oldmontags);
			}
		}

		/*
		 must put it under the floating window position adjustment,
		 Otherwise, incorrect floating window calculations will occur here.
		 */
		wlr_scene_output_set_position(m->scene_output, m->m.x, m->m.y);

		if (m->blur) {
			wlr_scene_node_set_position(&m->blur->node, m->m.x, m->m.y);
			wlr_scene_optimized_blur_set_size(m->blur, m->m.width, m->m.height);
		}

		if (m->lock_surface) {
			struct wlr_scene_tree *scene_tree = m->lock_surface->surface->data;
			wlr_scene_node_set_position(&scene_tree->node, m->m.x, m->m.y);
			wlr_session_lock_surface_v1_configure(m->lock_surface, m->m.width,
												  m->m.height);
		}

		/* Calculate the effective monitor geometry to use for clients */
		arrangelayers(m);
		/* Don't move clients to the left output when plugging monitors */
		arrange(m, false, false);
		/* make sure fullscreen clients have the right size */
		if ((c = focustop(m)) && c->isfullscreen)
			resize(c, m->m, 0);

		config_head->state.x = m->m.x;
		config_head->state.y = m->m.y;

		if (!selmon)
			selmon = m;
	}

	if (selmon && selmon->wlr_output->enabled) {
		wl_list_for_each(c, &clients, link) {
			if (!c->mon && client_surface(c)->mapped) {
				c->mon = selmon;
				reset_foreign_tolevel(c, NULL, c->mon);
			}
			if (c->tags == 0 && !c->is_in_scratchpad) {
				c->tags = selmon->tagset[selmon->seltags];
				set_size_per(selmon, c);
			}
		}
		focusclient(focustop(selmon), 1);
		if (selmon->lock_surface) {
			client_notify_enter(selmon->lock_surface->surface,
								wlr_seat_get_keyboard(seat));
			client_activate_surface(selmon->lock_surface->surface, 1);
		}
	}

	/* FIXME: figure out why the cursor image is at 0,0 after turning all
	 * the monitors on.
	 * Move the cursor image where it used to be. It does not generate a
	 * wl_pointer.motion event for the clients, it's only the image what
	 * it's at the wrong position after all. */
	wlr_cursor_move(cursor, NULL, 0, 0);

	wlr_output_manager_v1_set_configuration(output_mgr, output_config);
}

void updatetitle(struct wl_listener *listener, void *data) {
	Client *c = wl_container_of(listener, c, set_title);

	if (!c || c->iskilling)
		return;

	const char *title;
	title = client_get_title(c);
	/* keep the tab's CURRENT font scale: a title change must not stomp a draw
	 * that was made at a different scale moments earlier. Falls back to the
	 * output's, not to 1.0 -- 1.0 rasterises at logical resolution and leaves
	 * the title soft on any scaled output (see client_render_scale). */
	float title_scale = (c->titlebar_node && c->titlebar_node->last_scale > 0.0f)
							? c->titlebar_node->last_scale
							: client_render_scale(c);
	asteroidz_tab_bar_node_update(c->titlebar_node, title, title_scale);
	if (title && c->foreign_toplevel)
		wlr_foreign_toplevel_handle_v1_set_title(c->foreign_toplevel, title);
	client_update_ext_foreign_toplevel(c);
	if (c == focustop(c->mon))
		printstatus(IPC_WATCH_ARRANGGE);
}

void // 17 fix to 0.5
urgent(struct wl_listener *listener, void *data) {
	struct wlr_xdg_activation_v1_request_activate_event *event = data;
	Client *c = NULL;
	toplevel_from_wlr_surface(event->surface, &c, NULL);

	if (!c || !c->foreign_toplevel)
		return;

	/* Same focus-steal guard as activatex11: a deliberate tag switch (or a
	 * just-defocused client) must not be undone by an activate that lands
	 * right after it -- flag urgent instead. */
	uint32_t now_ms = get_now_in_ms();
	bool activate_is_steal =
		now_ms - c->last_unfocus_ms < FOCUS_ACTIVATE_STEAL_MS ||
		now_ms - last_x11_unfocus_ms < FOCUS_ACTIVATE_STEAL_MS ||
		now_ms - last_user_view_ms < FOCUS_VIEW_STEAL_MS;

	if (config.focus_on_activate && !c->istagsilent && c != selmon->sel &&
			!activate_is_steal) {
		if (!(c->mon == selmon && c->tags & c->mon->tagset[c->mon->seltags]))
			view_in_mon(&(Arg){.ui = c->tags}, true, c->mon, true);
		focusclient(c, 1);
	} else if (c != focustop(selmon)) {
		c->isurgent = 1;
		if (client_surface(c)->mapped)
			setborder_color(c);
		printstatus(IPC_WATCH_ARRANGGE);
	}
}

void view_in_mon(const Arg *arg, bool want_animation, Monitor *m,
				 bool changefocus) {
	uint32_t i, tmptag;

	if (!m || (arg->ui != (~0 & TAGMASK) && m->isoverview)) {
		return;
	}

	if (arg->ui == 0) {
		return;
	}

	if (arg->ui == UINT32_MAX) {
		if (m->tagset[0] != m->tagset[1]) {
			m->pertag->prevtag = get_tags_first_tag_num(m->tagset[m->seltags]);
			m->seltags ^= 1; /* toggle sel tagset */
			m->pertag->curtag = get_tags_first_tag_num(m->tagset[m->seltags]);
			goto toggleseltags;
		} else {
			return;
		}
	}

	if ((m->tagset[m->seltags] & arg->ui & TAGMASK) != 0) {
		want_animation = false;
	}

	m->seltags ^= 1; /* toggle sel tagset */

	if (arg->ui & TAGMASK) {
		m->tagset[m->seltags] = arg->ui & TAGMASK;
		tmptag = m->pertag->curtag;

		if (arg->ui == (~0 & TAGMASK))
			m->pertag->curtag = 0;
		else {
			for (i = 0; !(arg->ui & 1 << i) && i < LENGTH(tags) && arg->ui != 0;
				 i++)
				;
			m->pertag->curtag = i >= LENGTH(tags) ? LENGTH(tags) : i + 1;
		}

		m->pertag->prevtag =
			tmptag == m->pertag->curtag ? m->pertag->prevtag : tmptag;
	} else {
		tmptag = m->pertag->prevtag;
		m->pertag->prevtag = m->pertag->curtag;
		m->pertag->curtag = tmptag;
	}

toggleseltags:

	if (changefocus)
		focusclient(focustop(m), 1);
	arrange(m, want_animation, true);
	printstatus(IPC_WATCH_ARRANGGE);
}

void view(const Arg *arg, bool want_animation) {
	/* Record this deliberate tag switch so a window's request_activate can't
	 * immediately yank the view back (see last_user_view_ms). */
	last_user_view_ms = get_now_in_ms();
	/* tags are strictly per-monitor: a tag switch only ever affects
	 * selmon, never any other monitor's tagset/pertag state. arg->i used
	 * to be an opt-in "sync this switch to every other monitor too" flag
	 * (the documented `synctag` argument on view/view_to_left/
	 * view_to_right); removed so there is no path, opt-in or otherwise,
	 * by which switching tags on one monitor can touch another. */
	view_in_mon(arg, want_animation, selmon, true);
}

static void
handle_keyboard_shortcuts_inhibitor_destroy(struct wl_listener *listener,
											void *data) {
	KeyboardShortcutsInhibitor *inhibitor =
		wl_container_of(listener, inhibitor, destroy);

	wlr_log(WLR_DEBUG, "Removing keyboard shortcuts inhibitor");

	wl_list_remove(&inhibitor->link);
	wl_list_remove(&inhibitor->destroy.link);
	free(inhibitor);
}

void handle_keyboard_shortcuts_inhibit_new_inhibitor(
	struct wl_listener *listener, void *data) {

	struct wlr_keyboard_shortcuts_inhibitor_v1 *inhibitor = data;

	if (config.allow_shortcuts_inhibit == SHORTCUTS_INHIBIT_DISABLE) {
		return;
	}

	// per-view, seat-agnostic config via criteria
	Client *c = NULL;
	LayerSurface *l = NULL;

	int32_t type = toplevel_from_wlr_surface(inhibitor->surface, &c, &l);

	if (type < 0)
		return;

	if (type != LayerShell && c && !c->allow_shortcuts_inhibit) {
		return;
	}

	wlr_log(WLR_DEBUG, "Adding keyboard shortcuts inhibitor");

	KeyboardShortcutsInhibitor *kbsinhibitor =
		calloc(1, sizeof(KeyboardShortcutsInhibitor));

	kbsinhibitor->inhibitor = inhibitor;

	kbsinhibitor->destroy.notify = handle_keyboard_shortcuts_inhibitor_destroy;
	wl_signal_add(&inhibitor->events.destroy, &kbsinhibitor->destroy);

	wl_list_insert(&keyboard_shortcut_inhibitors, &kbsinhibitor->link);

	wlr_keyboard_shortcuts_inhibitor_v1_activate(inhibitor);
}

void virtualkeyboard(struct wl_listener *listener, void *data) {
	struct wlr_virtual_keyboard_v1 *kb = data;
	/* virtual keyboards shouldn't share keyboard group */
	wlr_seat_set_capabilities(seat,
							  seat->capabilities | WL_SEAT_CAPABILITY_KEYBOARD);
	KeyboardGroup *group = createkeyboardgroup();
	/* Set the keymap to match the group keymap */
	wlr_keyboard_set_keymap(&kb->keyboard, group->wlr_group->keyboard.keymap);
	LISTEN(&kb->keyboard.base.events.destroy, &group->destroy,
		   destroykeyboardgroup);

	/* Add the new keyboard to the group */
	wlr_keyboard_group_add_keyboard(group->wlr_group, &kb->keyboard);
}

void warp_cursor(const Client *c) {
	if (INSIDEMON(c)) {
		wlr_cursor_warp_closest(cursor, NULL, c->geom.x + c->geom.width / 2.0,
								c->geom.y + c->geom.height / 2.0);
		motionnotify(0, NULL, 0, 0, 0, 0);
	}
}

void warp_cursor_to_selmon(Monitor *m) {

	wlr_cursor_warp_closest(cursor, NULL, m->w.x + m->w.width / 2.0,
							m->w.y + m->w.height / 2.0);
	az_cursor_set_xcursor("default");
	handlecursoractivity();
}

void virtualpointer(struct wl_listener *listener, void *data) {
	struct wlr_virtual_pointer_v1_new_pointer_event *event = data;
	struct wlr_input_device *device = &event->new_pointer->pointer.base;
	wlr_seat_set_capabilities(seat,
							  seat->capabilities | WL_SEAT_CAPABILITY_POINTER);
	wlr_cursor_attach_input_device(cursor, device);
	if (event->suggested_output)
		wlr_cursor_map_input_to_output(cursor, device, event->suggested_output);

	handlecursoractivity();
}

#ifdef XWAYLAND
void fix_xwayland_coordinate(struct wlr_box *geom) {
	if (!selmon)
		return;

	// 1. if the window is already within the currently active monitor, return immediately
	if (geom->x >= selmon->m.x && geom->x <= selmon->m.x + selmon->m.width &&
		geom->y >= selmon->m.y && geom->y <= selmon->m.y + selmon->m.height)
		return;

	geom->x = selmon->m.x + (selmon->m.width - geom->width) / 2;
	geom->y = selmon->m.y + (selmon->m.height - geom->height) / 2;
}

/* ── BOUNDARY 3: PRESENTATION ─────────────────────────────────────────────
 *
 * Tell the scene how many of this surface's pixels go into one logical pixel,
 * and pin the sampler while we are there.
 *
 * NEAREST, not the default bilinear. The whole point is that the buffer is
 * already the right number of pixels, so no filtering should be needed at
 * all -- and where the renderer's edge rounding leaves a fractional pixel
 * (see below), nearest duplicates one texel row while bilinear smears the
 * entire image. A filter that is never asked to interpolate costs nothing;
 * one that is asked to interpolate everything is exactly the blur this
 * option exists to remove. It is set per buffer and survives a reconfigure,
 * so it does not have to be re-applied on every commit.
 *
 * ── THE HONEST CAVEAT ────────────────────────────────────────────────────
 *
 * 1:1 is exact only where both edges of the window's logical box land on
 * whole device pixels. AVK converts a logical box to the output raster by
 * rounding each EDGE and subtracting (az_avk_box_to_output), which is what
 * makes adjacent windows meet without a seam -- but at 1.25x or 1.5x it also
 * means a window at an odd logical position can come out one device pixel
 * wider or narrower than its buffer. With NEAREST that is a single
 * duplicated or dropped texel row at one edge, which is what Hyprland's
 * force_zero_scaling does too. A fullscreen window -- the case this option
 * is for -- starts at 0 and is exact. A dst-snap refinement in AVK could
 * remove the remainder; it is deliberately not attempted here.
 */
static void x11_scale_apply_iter(struct wlr_scene_buffer *buffer, int32_t sx,
								 int32_t sy, void *data) {
	struct wlr_scene_surface *scene_surface =
		wlr_scene_surface_try_from_buffer(buffer);
	if (scene_surface == NULL) {
		return;
	}
	float scale = *(float *)data;
	/* BREAK: AZ_BREAK_X11_VIEW_SCALE=1 sizes the X window in pixels and then
	 * does NOT tell the scene about it, which is the state the feature is
	 * half-implemented in. The buffer is magnified exactly as before, so the
	 * pixel gate in contrib/xw-scale-test.sh must report `scaled` even with
	 * the option on. Its falsifier: if that gate still passes, it is not
	 * measuring presentation. */
	static int break_view_scale = -1;
	if (break_view_scale < 0) {
		const char *e = getenv("AZ_BREAK_X11_VIEW_SCALE");
		break_view_scale = e && *e && *e != '0';
	}
	if (!break_view_scale) {
		wlr_scene_surface_set_view_scale(scene_surface, scale);
	}
	wlr_scene_buffer_set_filter_mode(buffer, scale == 1.0f
												 ? WLR_SCALE_FILTER_BILINEAR
												 : WLR_SCALE_FILTER_NEAREST);
}

/* The scale of a monitor, as an X11 client would be measured in it.
 *
 * At or below 1 there is nothing to force: the buffer would have to be
 * SMALLER than the logical box, and the logical -> pixel -> logical round trip
 * stops being lossless below 1, which would drift geometry a pixel at a time.
 * See client_x11_scale(). */
static float x11_scale_of_mon(Monitor *m) {
	if (!m || !m->wlr_output) {
		return 1.0f;
	}
	float s = (float)m->wlr_output->scale;
	return s > 1.0f ? s : 1.0f;
}

/*
 * ── WHICH MONITOR AN X11 WINDOW IS MEASURED IN ───────────────────────────
 *
 * For a managed client this is not a question: c->mon is decided by setmon
 * and everything follows from it. An override-redirect window -- a menu, a
 * tooltip, a drag icon, a splash -- never goes through setmon at all and has
 * no monitor of its own, so it has to be attributed.
 *
 * THE OBVIOUS ANSWER, LOOKING UP ITS POSITION, IS AMBIGUOUS BY CONSTRUCTION.
 * With this option on, each monitor's X11 zone is its logical box multiplied
 * by its OWN scale, and those products overlap on a mixed-DPI layout: a
 * 1920x1080 output at 1.25 is logical 0..1536 and X11 0..1920, while the
 * output starting at logical 1536 with scale 1 is X11 1536..3456. X11
 * coordinate 1700 is inside both zones and there is no arithmetic that can
 * say which one meant it.
 *
 * A POPUP'S PARENT IS NOT AMBIGUOUS. It is a managed window with a monitor,
 * and a menu belongs to the window that opened it. That is also the only
 * answer that keeps parent and popup in the SAME unit -- the client computed
 * the offset between them itself, and it is only correct if both are measured
 * the same way. So the parent is asked first, and the zone lookup is the
 * fallback for a window that has none. Its first match wins, in monitor
 * order: a stated, repeatable rule rather than whichever one happened to be
 * checked first.
 */
static Monitor *client_x11_monitor(Client *c) {
	if (c->mon) {
		return c->mon;
	}

	struct wlr_xwayland_surface *parent = c->surface.xwayland->parent;
	while (parent != NULL) {
		Client *pc = parent->data;
		if (pc && pc->mon) {
			return pc->mon;
		}
		/* A tooltip on a menu on a window: keep walking rather than give up
		 * at the first unmapped link, which has no monitor either. */
		parent = parent->parent;
	}

	int32_t x = c->surface.xwayland->x, y = c->surface.xwayland->y;
	Monitor *m;
	wl_list_for_each(m, &mons, link) {
		if (!m->wlr_output || !m->wlr_output->enabled) {
			continue;
		}
		float s = config.xwayland_force_scale_one ? x11_scale_of_mon(m) : 1.0f;
		struct wlr_box zone = {
			.x = (int32_t)lroundf((float)m->m.x * s),
			.y = (int32_t)lroundf((float)m->m.y * s),
			.width = (int32_t)lroundf((float)m->m.width * s),
			.height = (int32_t)lroundf((float)m->m.height * s),
		};
		if (wlr_box_contains_point(&zone, x, y)) {
			return m;
		}
	}

	return selmon;
}

/* The scale an X11 client should be sized in.
 *
 * Not the sharpest output in the layout, and not a global: two displays at
 * different scales are the reason this is per client at all.
 */
static float client_x11_target_scale(Client *c) {
	if (!config.xwayland_force_scale_one || !client_is_x11(c)) {
		return 1.0f;
	}
	return x11_scale_of_mon(client_x11_monitor(c));
}

void client_update_x11_scale(Client *c) {
	if (!c || !client_is_x11(c)) {
		return;
	}

	float want = client_x11_target_scale(c);
	if (want == c->x11_scale) {
		return;
	}
	c->x11_scale = want;

	if (c->scene_surface) {
		wlr_scene_node_for_each_buffer(&c->scene_surface->node,
									   x11_scale_apply_iter, &want);
	}

	/* The window is now being measured in a different unit, so whatever it
	 * was last told is wrong by exactly that factor. Ask again -- the short
	 * circuit in client_set_size compares in X11's space, so this is a no-op
	 * when nothing actually changed.
	 *
	 * Skipped while c->geom is still empty. The first call happens in
	 * mapnotify BEFORE the geometry has been read, deliberately (see there),
	 * and configuring a window to 0x0 at that point would be the only lasting
	 * effect of it.
	 *
	 * NOT for an override-redirect window. Those place and size themselves;
	 * the compositor telling one what size to be is how a menu ends up
	 * fighting its own client for its geometry. Its presentation and its
	 * geometry-in conversion have already changed above, which is what makes
	 * it read correctly, and its next self-configure settles the rest. */
	if (!client_is_unmanaged(c) && c->geom.width > 0 && c->geom.height > 0 &&
		client_surface(c) && client_surface(c)->mapped) {
		client_set_size(c, c->geom.width, c->geom.height);
	}
}

int32_t synckeymap(void *data) {
	reset_keyboard_layout();
	// we only need to sync keymap once
	wlr_log(WLR_INFO, "timer to synckeymap done");
	wl_event_source_timer_update(sync_keymap, 0);
	return 0;
}

void activatex11(struct wl_listener *listener, void *data) {
	Client *c = wl_container_of(listener, c, activate);
	bool need_arrange = false;

	if (!c || c->iskilling || !c->foreign_toplevel || client_is_unmanaged(c))
		return;

	if (c && c->swallowing)
		return;

	if (c->isminimized) {
		client_pending_minimized_state(c, 0);
		c->tags = c->mini_restore_tag;
		c->is_scratchpad_show = 0;
		c->is_in_scratchpad = 0;
		c->isnamedscratchpad = 0;
		setborder_color(c);
		if (VISIBLEON(c, c->mon)) {
			need_arrange = true;
		}
	}

	/* Focus-stealing prevention: some X11 apps (Electron) re-fire
	 * request_activate the instant they lose focus, so switching away from a
	 * focused one would immediately yank the view back to its tag. Ignore an
	 * activate that arrives within this window of the client being defocused;
	 * mark it urgent instead. Genuine later activations still switch. */
	uint32_t now_ms = get_now_in_ms();
	bool activate_is_steal =
		now_ms - c->last_unfocus_ms < FOCUS_ACTIVATE_STEAL_MS ||
		now_ms - last_x11_unfocus_ms < FOCUS_ACTIVATE_STEAL_MS ||
		now_ms - last_user_view_ms < FOCUS_VIEW_STEAL_MS;

	/* While overview is open, honouring an activate (view_in_mon + focusclient +
	 * arrange) would pull the window out of its scaled thumbnail and snap it
	 * back to full size. Never let a window grab focus/view during overview --
	 * just flag it urgent. */
	bool in_overview = (c->mon && c->mon->isoverview) ||
					   (selmon && selmon->isoverview);

	/* selmon is NULL with no enabled output -- the line above already guards it
	 * for exactly that reason, and an X11 activate arriving during output
	 * teardown reaches this. The guard covers the body too: c->mon == selmon
	 * is TRUE when both are NULL, which would then walk into c->mon->tagset. */
	if (config.focus_on_activate && !c->istagsilent && selmon &&
			c != selmon->sel && !activate_is_steal && !in_overview) {
		if (!(c->mon == selmon && c->tags & c->mon->tagset[c->mon->seltags]))
			view_in_mon(&(Arg){.ui = c->tags}, true, c->mon, true);
		wlr_xwayland_surface_activate(c->surface.xwayland, 1);
		focusclient(c, 1);
		need_arrange = true;
	} else if (c != focustop(selmon)) {
		c->isurgent = 1;
		if (client_surface(c)->mapped)
			setborder_color(c);
	}

	if (need_arrange) {
		arrange(c->mon, false, false);
	}

	printstatus(IPC_WATCH_ARRANGGE);
}

void configurex11(struct wl_listener *listener, void *data) {
	Client *c = wl_container_of(listener, c, configure);
	struct wlr_xwayland_surface_configure_event *event = data;
	struct wlr_box new_geo;
	/* BOUNDARY 2. The client asked in ITS units; everything below -- the
	 * monitor clamp in fix_xwayland_coordinate, resize(), the scene node's
	 * position -- is logical. */
	new_geo.x = client_x11_to_logical(c, event->x);
	new_geo.y = client_x11_to_logical(c, event->y);
	new_geo.width = client_x11_to_logical(c, event->width);
	new_geo.height = client_x11_to_logical(c, event->height);
	fix_xwayland_coordinate(&new_geo);

	if (!client_surface(c) || !client_surface(c)->mapped) {

		client_x11_configure(c, new_geo.x, new_geo.y, new_geo.width,
							 new_geo.height);
		return;
	}

	if (client_is_unmanaged(c)) {
		wlr_scene_node_set_position(&c->scene->node, new_geo.x, new_geo.y);
		client_x11_configure(c, new_geo.x, new_geo.y, new_geo.width,
							 new_geo.height);
		return;
	}

	if (c->isfloating && c != grabc) {
		new_geo.x = new_geo.x - c->bw;
		new_geo.y = new_geo.y - c->bw;
		new_geo.width = new_geo.width + c->bw * 2;
		new_geo.height = new_geo.height + c->bw * 2;
		fix_xwayland_coordinate(&new_geo);

		resize(c,
			   (struct wlr_box){.x = new_geo.x,
								.y = new_geo.y,
								.width = new_geo.width,
								.height = new_geo.height},
			   0);
	} else {
		arrange(c->mon, false, false);
	}
}

void createnotifyx11(struct wl_listener *listener, void *data) {
	struct wlr_xwayland_surface *xsurface = data;
	Client *c = NULL;

	/* Allocate a Client for this surface */
	c = xsurface->data = ecalloc(1, sizeof(*c));
	c->surface.xwayland = xsurface;
	c->type = X11;
	/* Explicit rather than left at ecalloc's 0. client_x11_scale() reads
	 * anything below 1 as 1, so both behave the same -- but a scale of zero
	 * sitting in the struct is a division waiting to happen. */
	c->x11_scale = 1.0f;
	/* Listen to the various events it can emit */
	LISTEN(&xsurface->events.associate, &c->associate, associatex11);
	LISTEN(&xsurface->events.destroy, &c->destroy, destroynotify);
	LISTEN(&xsurface->events.dissociate, &c->dissociate, dissociatex11);
	LISTEN(&xsurface->events.request_activate, &c->activate, activatex11);
	LISTEN(&xsurface->events.request_configure, &c->configure, configurex11);
	LISTEN(&xsurface->events.request_fullscreen, &c->fullscreen,
		   fullscreennotify);
	LISTEN(&xsurface->events.set_hints, &c->set_hints, sethints);
	LISTEN(&xsurface->events.set_title, &c->set_title, updatetitle);
	LISTEN(&xsurface->events.request_maximize, &c->maximize, maximizenotify);
	LISTEN(&xsurface->events.request_minimize, &c->minimize, minimizenotify);
}

void commitx11(struct wl_listener *listener, void *data) {
	Client *c = wl_container_of(listener, c, commmitx11);
	struct wlr_surface_state *state = &c->surface.xwayland->surface->current;

	/*
	 * ── F10: AN XWAYLAND SURFACE'S COLOUR VOLUME CHANGES ON COMMIT TOO ────
	 *
	 * wp-cm is a wl_surface protocol, so an XWayland client's Wayland surface
	 * can carry an image description exactly like an xdg one -- and commit is
	 * the first moment the double-buffered state is readable, which is why
	 * commitnotify() calls this. There are two commit listeners in this
	 * compositor and only the xdg one had the call, so an XWayland client that
	 * changed its mastering metadata mid-stream never re-armed the connector:
	 * DP-1 kept describing the volume from whenever the last xdg client last
	 * committed, or from map.
	 *
	 * The preferred-description half of the handshake was never missing here.
	 * That is sent from mapnotify() and setmon(), both of which XWayland
	 * already shares, so this one call is the whole of the gap.
	 *
	 * Same gate and same placement as commitnotify(): before any early-out,
	 * because a client mid-animation is still the thing on screen and its
	 * colour volume is not an animation property.
	 */
	if (c && !c->iskilling) {
		mon_content_metadata_changed(client_surface(c));
	}
	client_note_commit(c);

	/* Compared in X11's space, for the same reason as the short circuit in
	 * client_set_size: state->width is the surface's own pixel count and
	 * xwayland->x/y are X coordinates, while c->geom is logical. In logical
	 * units these would never match at a fractional scale, and the client
	 * would be treated as permanently mid-resize. */
	if (client_x11_from_logical(c, (int32_t)c->geom.width - 2 * (int32_t)c->bw) ==
			(int32_t)state->width &&
		client_x11_from_logical(c, (int32_t)c->geom.height -
									   2 * (int32_t)c->bw) ==
			(int32_t)state->height &&
		client_x11_from_logical(c, (int32_t)c->geom.x + (int32_t)c->bw) ==
			(int32_t)c->surface.xwayland->x &&
		client_x11_from_logical(c, (int32_t)c->geom.y + (int32_t)c->bw) ==
			(int32_t)c->surface.xwayland->y) {
		c->configure_serial = 0;
	}

	/* In overview, an XWayland app that repaints commits a fresh full-size
	 * buffer; wlroots' scene commit handler (which runs after this one) then
	 * resets the scene surface to that natural size, undoing the thumbnail
	 * down-scale. The xdg path re-runs resize() on commit (which sets
	 * need_output_flush) so rendermon re-clips it; commitx11 never did. Flag it
	 * here and let rendermon's client_draw_frame re-apply client_apply_clip at
	 * frame time -- after wlroots' commit handler -- so the dest-size sticks. */
	if (c->mon && c->mon->isoverview) {
		c->need_output_flush = true;
		wlr_output_schedule_frame(c->mon->wlr_output);
	}
}

void associatex11(struct wl_listener *listener, void *data) {
	Client *c = wl_container_of(listener, c, associate);

	LISTEN(&client_surface(c)->events.map, &c->map, mapnotify);
	LISTEN(&client_surface(c)->events.unmap, &c->unmap, unmapnotify);
	LISTEN(&client_surface(c)->events.commit, &c->commmitx11, commitx11);
}

void dissociatex11(struct wl_listener *listener, void *data) {
	Client *c = wl_container_of(listener, c, dissociate);
	wl_list_remove(&c->map.link);
	wl_list_remove(&c->unmap.link);
	wl_list_remove(&c->commmitx11.link);
}

void sethints(struct wl_listener *listener, void *data) {
	Client *c = wl_container_of(listener, c, set_hints);
	struct wlr_surface *surface = client_surface(c);
	if (c == focustop(selmon) || !c || !c->surface.xwayland->hints)
		return;

	c->isurgent = xcb_icccm_wm_hints_get_urgency(c->surface.xwayland->hints);
	printstatus(IPC_WATCH_ARRANGGE);

	if (c->isurgent && surface && surface->mapped)
		setborder_color(c);
}

void xwaylandready(struct wl_listener *listener, void *data) {
	struct wlr_xcursor *xcursor;

	/* assign the one and only seat */
	wlr_xwayland_set_seat(xwayland, seat);

	/* Set the default XWayland cursor to match the rest of dwl.
	 *
	 * The load is not redundant. wlr_xcursor_manager_get_xcursor() returns
	 * NULL for a scale that was never loaded -- it does not load on demand --
	 * and this asks for scale 1 regardless of what any output is running at.
	 * When asteroidz took over choosing cursor images it stopped loading every
	 * output's scale as a side effect, so on a layout whose sharpest output is
	 * 1.5 this lookup returned NULL, XWayland was never given a cursor, and
	 * every X11 window showed the X server's own 'X' root cursor with nothing
	 * logged anywhere. */
	if (getenv("AZ_CURSOR_ONE_SCALE") == NULL) {
		wlr_xcursor_manager_load(cursor_mgr, 1);
	}
	if ((xcursor = wlr_xcursor_manager_get_xcursor(cursor_mgr, "default", 1))) {
		struct wlr_buffer *xcursor_buffer =
			wlr_xcursor_image_get_buffer(xcursor->images[0]);
		if (xcursor_buffer)
			wlr_xwayland_set_cursor(xwayland, xcursor_buffer,
									xcursor->images[0]->hotspot_x,
									xcursor->images[0]->hotspot_y);
		else
			wlr_log(WLR_ERROR, "xwayland: the 'default' cursor has no buffer; "
					"X11 windows will show the X server's own cursor");
	} else {
		/* Said out loud, because the failure mode is a wrong-looking pointer
		 * in X11 windows only, which reads as an XWayland problem. */
		wlr_log(WLR_ERROR, "xwayland: no 'default' cursor at scale 1 in theme "
				"'%s'; X11 windows will show the X server's own 'X' cursor",
				config.cursor_theme ? config.cursor_theme : "(default)");
	}
	/* xwayland can't auto sync the keymap, so we do it manually
	  and we need to wait the xwayland completely inited
	*/
	wl_event_source_timer_update(sync_keymap, 500);
}

static void setgeometrynotify(struct wl_listener *listener, void *data) {
	Client *c = wl_container_of(listener, c, set_geometry);

	/* An override-redirect window never reaches setmon, so moving one to
	 * another display is the ONLY way its scale can change -- and it is a
	 * real case: a menu opened near a seam can be placed on the neighbouring
	 * monitor. Re-attributed before the position is read, so both halves of
	 * this function agree about which unit they are in. */
	client_update_x11_scale(c);

	/* BOUNDARY 2. A scene node's position is logical; the X window's is not. */
	wlr_scene_node_set_position(
		&c->scene->node, client_x11_to_logical(c, c->surface.xwayland->x),
		client_x11_to_logical(c, c->surface.xwayland->y));
	motionnotify(0, NULL, 0, 0, 0, 0);
}
#endif

/* Compositor stdout/stderr normally land on the raw VT (tty1) and are lost
 * once the console scrolls past them. Mirror stderr into a small rotating
 * log file so post-mortem debugging (e.g. a monitor that never came back
 * from DPMS) has something to look at after the fact. */
static void init_persistent_log(void) {
	const char *home = getenv("HOME");
	if (!home || !*home)
		return;

	/* XDG_STATE_HOME first, per the basedir spec, falling back to the
	 * hardcoded default. Not pedantry: the log is keyed on HOME alone, so
	 * every headless test instance -- which shares HOME by design, to find the
	 * user's fonts and icons -- appended to the LIVE session's log. A day of
	 * regression runs left 46 compositor start markers in it, and reading it
	 * to diagnose the real session meant picking those out first. A harness
	 * can now point somewhere else with one variable. */
	const char *state = getenv("XDG_STATE_HOME");
	char *dir = (state && *state)
					? string_printf("%s/asteroidz", state)
					: string_printf("%s/.local/state/asteroidz", home);
	if (!dir)
		return;

	for (char *p = dir + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			mkdir(dir, 0755);
			*p = '/';
		}
	}
	mkdir(dir, 0755);

	char *path = string_printf("%s/asteroidz.log", dir);
	free(dir);
	if (!path)
		return;

	/*
	 * One session per file, always.
	 *
	 * This used to append, and rotate only past 5MB. The result was a log
	 * holding many boots with nothing but a relative timestamp to separate
	 * them -- and since those restart at 00:00:00 every session, `grep`
	 * followed by `head` reads whichever boot happens to come first, which is
	 * almost never the one being investigated. Two separate diagnoses have
	 * been made against the wrong session that way.
	 *
	 * The previous session is kept as .old rather than discarded: when a
	 * session dies the interesting log is the one that just ended, and the
	 * next boot is exactly when somebody comes looking for it.
	 */
	char *old_path = string_printf("%s.old", path);
	if (old_path) {
		rename(path, old_path);
		free(old_path);
	}

	int32_t fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	free(path);
	if (fd < 0)
		return;

	dup2(fd, STDERR_FILENO);
	close(fd);

	time_t now = time(NULL);
	struct tm tm_now;
	localtime_r(&now, &tm_now);
	char stamp[32];
	strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm_now);
	fprintf(stderr, "\n===== asteroidz " VERSION " starting (%s) =====\n",
			stamp);
}

int32_t main(int32_t argc, char *argv[]) {
	char *startup_cmd = NULL;
	int32_t c;

	restart_argv = argv;
	if (getenv("ASTEROIDZ_RESTARTED")) {
		cli_debug_log = true;
		/* belt and braces: never let a previous instance's session env
		 * steer backend autodetection after an in-place restart */
		unsetenv("WAYLAND_DISPLAY");
		unsetenv("DISPLAY");
	}

	while ((c = getopt(argc, argv, "s:c:hdvpSLPDR")) != -1) {
		if (c == 's') {
			startup_cmd = optarg;
		} else if (c == 'd') {
			cli_debug_log = true;
		} else if (c == 'v') {
			printf("asteroidz " VERSION "\n");
			return EXIT_SUCCESS;
		} else if (c == 'c') {
			cli_config_path = optarg;
		} else if (c == 'p') {
			/* defer until ALL flags are parsed: `-p -c file` used to check
			 * the DEFAULT config (getopt hadn't reached -c yet) and report
			 * success while the named file had errors */
			cli_check_config = true;
		} else if (c == 'S') {
			cli_check_schema = true;
		} else if (c == 'L') {
			cli_list_schema = true;
		} else if (c == 'P') {
			cli_dump_source = true;
		} else if (c == 'D') {
			cli_list_dispatch = true;
		} else if (c == 'R') {
			cli_list_rules = true;
		} else {
			goto usage;
		}
	}
	if (optind < argc)
		goto usage;
	/* Before -p, and before any config is read: the schema check wants the
	 * compiled-in defaults, not whatever the user's file says. */
	if (cli_list_schema) {
		config_schema_list();
		return EXIT_SUCCESS;
	}
	if (cli_list_dispatch) {
		dispatch_actions_list();
		return EXIT_SUCCESS;
	}
	if (cli_list_rules) {
		rule_schema_list();
		return EXIT_SUCCESS;
	}
	if (cli_dump_source) {
		if (!parse_config())
			return EXIT_FAILURE;
		config_source_dump();
		return EXIT_SUCCESS;
	}
	if (cli_check_schema)
		return config_schema_self_check() ? EXIT_FAILURE : EXIT_SUCCESS;
	if (cli_check_config) {
		/* NO stderr redirect here: -p's whole job is showing parse errors on
		 * the terminal, not burying them in the persistent log (which is
		 * exactly what init_persistent_log's fd-2 redirect used to do) */
		if (parse_config()) {
			printf("config OK\n");
			return EXIT_SUCCESS;
		}
		return EXIT_FAILURE;
	}
	/* after flag parsing, so -p (above) keeps stderr on the terminal */
	init_persistent_log();

	/* Wayland requires XDG_RUNTIME_DIR for creating its communications
	 * socket
	 */
	if (!getenv("XDG_RUNTIME_DIR"))
		die("XDG_RUNTIME_DIR must be set");
	setup();
	run(startup_cmd);
	cleanup();

	return EXIT_SUCCESS;
usage:
	printf("Usage: asteroidz [OPTIONS]\n"
		   "\n"
		   "Options:\n"
		   "  -v             Show asteroidz version\n"
		   "  -d             Enable debug log\n"
		   "  -c <file>      Use custom configuration file\n"
		   "  -s <command>   Execute startup command\n"
		   "  -p             Check configuration file error\n"
		   "  -S             Check the config schema against the parser\n"
		   "  -L             List the config schema, one option per line\n"
		   "  -P             Show where each config value came from\n"
		   "  -D             List the dispatch actions, one per line\n"
		   "  -R             List the window-rule schema, one field per line\n");
	return EXIT_SUCCESS;
}
