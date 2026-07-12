---
title: Rules
description: Define behavior for specific windows, tags, and layers.
---

## Window Rules

Window rules allow you to set specific properties (floating, opacity, size, animations, etc.) for applications based on their `appid` or `title`. You can set all parameters in one line, and if you both set appid and title, the window will only follow the rules when appid and title both match.

**Format:**

```kdl
window-rule { match title="<regex>"; <action> <value>... }

window-rule { match app-id="<regex>" title="<regex>"; <action> <value>...; <action> }
```

### State & Behavior Parameters

| Parameter | Type | Values | Description |
| :--- | :--- | :--- | :--- |
| `appid` | string | Any | Match by application ID, supports regex |
| `title` | string | Any | Match by window title, supports regex |
| `toplevel_tag` | string | Any | Match by the tag a client set via xdg-toplevel-tag-v1, supports regex |
| `isfloating` | integer | `0` / `1` | Force floating state |
| `isfullscreen` | integer | `0` / `1` | Force fullscreen state |
| `isfakefullscreen` | integer | `0` / `1` | Force fake-fullscreen state (window stays constrained) |
| `isglobal` | integer | `0` / `1` | Open as global window (sticky across tags) |
| `ispinned` | integer | `0` / `1` | Open pinned: forced floating, kept on top and visible on every tag of its monitor |
| `deny_group` | integer | `0` / `1` | Window can never be pulled into a window group |
| `isoverlay` | integer | `0` / `1` | Make it always in top layer |
| `isopensilent` | integer | `0` / `1` | Open without focus |
| `istagsilent` | integer | `0` / `1` | Don't focus if client is not in current view tag |
| `force_fakemaximize` | integer | `0` / `1` (default 1) | The state of client set to fake maximized |
| `ignore_maximize` | integer | `0` / `1` (default 1) | Don't handle maximize request from client |
| `ignore_minimize` | integer | `0` / `1` (default 1) | Don't handle minimize request from client |
| `force_tiled_state` | integer | `0` / `1` | Deceive the window into thinking it is tiling, so it better adheres to assigned dimensions |
| `single_scratchpad` | integer | `0` / `1` (default 1) | Only show one out of named scratchpads or the normal scratchpad |
| `allow_shortcuts_inhibit` | integer | `0` / `1` (default 1) | Allow shortcuts to be inhibited by clients |
| `idleinhibit_when_focus` | integer | `0` / `1` (default 0) | Automatically keep idle inhibit active when this window is focused |

### Geometry & Position

| Parameter | Type | Values | Description |
| :--- | :--- | :--- | :--- |
| `width` | float | 0-9999 | Window width when it becomes a floating window,if the value below 1, it will be the percentage of the screen width,otherwise it will be the pixel value |
| `height` | float | 0-9999 | Window height when it becomes a floating window,if the value below 1, it will be the percentage of the screen height,otherwise it will be the pixel value |
| `offsetx` | integer | -999-999 | X offset from center (%), 100 is the edge of screen with outer gap |
| `offsety` | integer | -999-999 | Y offset from center (%), 100 is the edge of screen with outer gap |
| `monitor` | string | Any | Assign to monitor by [monitor spec](/docs/configuration/monitors#monitor-spec-format) (name, make, model, or serial) |
| `tags` | integer | 1-9 | Assign to specific tag |
| `no_force_center` | integer | `0` / `1` | Window does not force center |
| `isnosizehint` | integer | `0` / `1` | Don't use min size and max size for size hints |

### Visuals & Decoration

| Parameter | Type | Values | Description |
| :--- | :--- | :--- | :--- |
| `noblur` | integer | `0` / `1` | Window does not have blur effect |
| `isnoborder` | integer | `0` / `1` | Remove window border |
| `isnoshadow` | integer | `0` / `1` | Not apply shadow |
| `isnoradius` | integer | `0` / `1` | Not apply corner radius |
| `isnoanimation` | integer | `0` / `1` | Not apply animation |
| `focused_opacity` | integer | `0` / `1` | Window focused opacity |
| `unfocused_opacity` | integer | `0` / `1` | Window unfocused opacity |
| `allow_csd` | integer | `0` / `1` | Allow client side decoration |
| `force_ssd` | integer | `0` / `1` | Force server-side decorations (titlebar/border) for apps that support neither xdg-decoration nor client-side decorations (e.g. SDL/GLFW games) |

> **Tip:** For detailed visual effects configuration, see the [Window Effects](/docs/visuals/effects) page for blur, shadows, and opacity settings.

### Layout & Scroller

| Parameter | Type | Values | Description |
| :--- | :--- | :--- | :--- |
| `scroller_proportion` | float | 0.1-1.0 | Set scroller proportion |
| `scroller_proportion_single` | float | 0.1-1.0 | Set scroller auto adjust proportion when it is single window |

> **Tip:** For comprehensive layout configuration, see the [Layouts](/docs/window-management/layouts) page for all layout options and detailed settings.

### Animation

| Parameter | Type | Values | Description |
| :--- | :--- | :--- | :--- |
| `animation_type_open` | string | zoom, slide, fade, none | Set open animation |
| `animation_type_close` | string | zoom, slide, fade, none | Set close animation |
| `nofadein` | integer | `0` / `1` | Window ignores fade-in animation |
| `nofadeout` | integer | `0` / `1` | Window ignores fade-out animation |

> **Tip:** For detailed animation configuration, see the [Animations](/docs/visuals/animations) page for available types and settings.

### Terminal & Swallowing

| Parameter | Type | Values | Description |
| :--- | :--- | :--- | :--- |
| `isterm` | integer | `0` / `1` | A new GUI window will replace the isterm window when it is opened |
| `noswallow` | integer | `0` / `1` | The window will not replace the isterm window |

### Global & Special Windows

| Parameter | Type | Values | Description |
| :--- | :--- | :--- | :--- |
| `globalkeybinding` | string | `[mod combination][-][key]` | Global keybinding (only works for Wayland apps) |
| `isunglobal` | integer | `0` / `1` | Open as unmanaged global window (for desktop pets or camera windows) |
| `isnamedscratchpad` | integer | `0` / `1` | 0: disable, 1: named scratchpad |
| `special_workspace` | string | Any | Assign the window to the named [special workspace](/docs/window-management/special-workspaces) on map |

> **Tip:** For scratchpad usage, see the [Scratchpad](/docs/window-management/scratchpad) page for detailed configuration examples. For named, tiled overlay workspaces, see [Special Workspaces](/docs/window-management/special-workspaces).

### Performance & Tearing

| Parameter | Type | Values | Description |
| :--- | :--- | :--- | :--- |
| `force_tearing` | integer | `0` / `1` | Set window to tearing state, refer to [Tearing](/docs/configuration/monitors#tearing-game-mode) |

### Examples

```kdl
binds {
    alt+h { toggle_named_scratchpad st-yazi none "st -c st-yazi -e yazi"; }
    Super+s { togglespecialworkspace term; }
}

window-rule { match app-id=yesplaymusic title=Demons; width 1000; height 900 }

window-rule { match app-id=com.obsproject.Studio; globalkeybinding ctrl+alt-o }

window-rule { match app-id=com.obsproject.Studio; globalkeybinding ctrl+alt-n }

window-rule { match app-id=com.obsproject.Studio; isopensilent 1 }

window-rule { match title=vkcube; force_tearing 1 }

window-rule { match title="Counter-Strike 2"; force_tearing 1 }

window-rule { match app-id=st-yazi; isnamedscratchpad 1; width 1280; height 800 }

window-rule { match app-id=firefox; focused_opacity 0.8 }

window-rule { match app-id=foot; unfocused_opacity 0.6 }

window-rule { match app-id=slurp; no-blur }

window-rule { match app-id=alacritty; offsetx 20; offsety -30; width 800; height 600 }

window-rule { match app-id=discord; tags 9; monitor HDMI-A-1 }

window-rule { match app-id=st; isterm 1 }

window-rule { match app-id=foot; noswallow 1 }

window-rule { match app-id=firefox; allow_csd 1 }

window-rule { match app-id=cheese; isunglobal 1 }

window-rule { match app-id=kitty; special_workspace term }
```

---

## Tag Rules

You can set all parameters in one line. If only `id` is set, the rule is followed when the id matches. If any of `monitor_name`, `monitor_make`, `monitor_model`, or `monitor_serial` are set, the rule is followed only if **all** of the set monitor fields match.

> **Warning:** Layouts set in tag rules have a higher priority than monitor rule layouts.

**Format:**

```kdl
tag "<number>" { <property> <value>; <property> <value> }

tag "<number>" { monitor_name "eDP-1"; layout "<layout>" }

tag "<number>" { monitor_make "<make>"; monitor_model "<model>"; layout "<layout>" }
```

> **Tip:** See [Layouts](/docs/window-management/layouts#supported-layouts) for detailed descriptions of each layout type.

| Parameter | Type | Values | Description |
| :--- | :--- | :--- | :--- |
| `id` | integer | 0-9 | Match by tag id, 0 means the ~0 tag |
| `monitor_name` | string | monitor name | Match by monitor name |
| `monitor_make` | string | monitor make | Match by monitor manufacturer |
| `monitor_model` | string | monitor model | Match by monitor model |
| `monitor_serial` | string | monitor serial | Match by monitor serial number |
| `layout_name` | string | layout name | Layout name to set |
| `name` | string | tag name | User-facing tag name (shown in the overview and exposed to the bar via IPC). Rename at runtime with the `set_tag_name` dispatcher. |
| `no_render_border` | integer | `0` / `1` | Disable render border |
| `open_as_floating` | integer | `0` / `1` | New open window will be floating|
| `no_hide` | integer | `0` / `1` | Not hide even if the tag is empty |
| `nmaster` | integer | 0, 99 | Number of master windows |
| `mfact` | float | 0.1–0.9 | Master area factor |
| `scroller_default_proportion` | float | 0.1-1.0 | Set scroller  default proportion. |
| `scroller_default_proportion_single` | float | 0.1-1.0 | Set scroller auto adjust proportion when it is single window(only apply when set `scroller_ignore_proportion_single` to `0`) |
| `scroller_ignore_proportion_single` | integer | `0` / `1` | Ignore scroller single proportion setting. |

### Examples

```kdl
tag 1 { layout scroller }

tag 2 { layout scroller }

tag 1 { monitor_name eDP-1; layout scroller }

tag 2 { monitor_name eDP-1; layout scroller }

tag 1 { no_hide 1; layout scroller }

tag 2 { no_hide 1; layout scroller }

tag 3 { monitor_name eDP-1; no_hide 1; layout scroller }

tag 4 { monitor_name eDP-1; no_hide 1; layout scroller }

tag 5 { layout tile; nmaster 2; mfact 0.6 }

tag 6 { monitor_name HDMI-A-1; layout monocle; no_render_border 1 }

tag 1 { name web }

tag 2 { name code }

tag 3 { name chat }

tag 1 { layout scroller; scroller_default_proportion_single 0.5; scroller_ignore_proportion_single 0; scroller_default_proportion 0.9; monitor_name HDMI-A-1 }
```

> **Tip:** For Waybar configuration with persistent tags, see [Status Bar](/docs/visuals/status-bar) documentation.

---

## Layer Rules

You can set all parameters in one line. Target "layer shell" surfaces like status bars (`waybar`), launchers (`rofi`), or notification daemons.

**Format:**

```kdl
misc {
    layerrule layer_name:Values,Parameter:Values,Parameter:Values
}
```

> **Tip:** You can use `amsg get last_open_surface` to get the last open layer name for debugging.

| Parameter | Type | Values | Description |
| :--- | :--- | :--- | :--- |
| `layer_name` | string | layer name | Match name of layer, supports regex |
| `animation_type_open` | string | slide, zoom, fade, none | Set open animation |
| `animation_type_close` | string | slide, zoom, fade, none | Set close animation |
| `noblur` | integer | `0` / `1` | Disable blur |
| `noanim` | integer | `0` / `1` | Disable layer animation |
| `noshadow` | integer | `0` / `1` | Disable layer shadow |

> **Tip:** For animation types, see [Animations](/docs/visuals/animations#animation-types). For visual effects, see [Window Effects](/docs/visuals/effects).

### Examples

```kdl
misc {
    layerrule animation_type_open:slide,animation_type_close:fade,noblur:1,layer_name:wofi
}
```
