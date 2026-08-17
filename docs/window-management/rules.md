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

**Two spellings, both accepted.** Every parameter below has a dashed KDL name and
a bare key — `no-blur` and `noblur`, `vrr-only-fullscreen` and
`vrr_only_fullscreen`. The dashed one is canonical and is what a writer should
emit; the bare key is what the legacy `windowrule=` line format uses and what
existing configs are written with, and it keeps working. `asteroidz -R` lists
both:

```bash
asteroidz -R    # every rule field: key, KDL name, group, type, range
```

The compositor carries this table itself (`src/config/rule-schema.h`), which is
what `get window-rule-schema` serves to a rule editor and what
`asteroidz -S` checks against the real parser. Writing it turned up six
parameters the parser has always accepted and this page did not mention, and one
this page listed that is not a window rule at all — `single_scratchpad` is a
global option, documented under
[Scratchpads](/docs/window-management/special-workspaces).

**The legacy comma form still reads**, written as an argument:

```kdl
windowrule "appid:mpv,isfullscreen:1"
```

which is what an old `windowrule=` line becomes. It used to be swallowed: the KDL
handler only looked at a node's *children*, so a node with an argument and no
block produced a rule with no matchers — and a rule with no matchers matches every
window. Silent, and reported as a successful parse. An empty `window-rule { }` is
now ignored with a warning for the same reason.

**Matching is by regex**, not by literal text. A `.` or a `+` in an app id is a
wildcard, so `org.gnome.Nautilus` also matches `orgXgnomeXNautilus`. Harmless in
practice, worth knowing before reaching for `^…$`.

**Most parameters are tri-state**, not boolean: unset, `0`, or `1`. Unset means
"this rule says nothing, use the global setting", which is a different thing from
`0`. A rule that sets `no-blur 0` turns blur *on* for that window even where the
global setting is off.

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
| `isoverlay` | integer | `0` / `1` | Make it always in top layer |
| `isopensilent` | integer | `0` / `1` | Open without focus |
| `istagsilent` | integer | `0` / `1` | Don't focus if client is not in current view tag |
| `force_fakemaximize` | integer | `0` / `1` (default 1) | The state of client set to fake maximized |
| `ignore_maximize` | integer | `0` / `1` (default 1) | Don't handle maximize request from client |
| `ignore_minimize` | integer | `0` / `1` (default 1) | Don't handle minimize request from client |
| `force_tiled_state` | integer | `0` / `1` | Deceive the window into thinking it is tiling, so it better adheres to assigned dimensions |
| `allow_shortcuts_inhibit` | integer | `0` / `1` (default 1) | Allow shortcuts to be inhibited by clients |
| `idleinhibit_when_focus` | integer | `0` / `1` (default 0) | Automatically keep idle inhibit active when this window is focused |
| `nofocus` / `no-focus` | integer | `0` / `1` | Refuse focus entirely. For overlays and pets that should never take the keyboard — distinct from `isopensilent`, which only declines focus at map |

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
| `isnotitlebar` / `no-titlebar` | integer | `0` / `1` | Draw no titlebar for this window, whatever the global titlebar setting is |
| `sdr_white_scale` / `sdr-white-scale` | float | `0` – `10` | Multiply this window's SDR white. `1.0` is unchanged, `1.5` makes an SDR application 50% brighter on an HDR output. `0` leaves it alone. No effect on HDR (PQ or scRGB) content |
| `hdr_gain` / `hdr-gain` | float | `0` – `10` | Multiply this window's HDR content. `1.0` is unchanged, `0.5` halves the absolute luminance of a PQ video. `0` leaves it alone. No effect on SDR content |
| `luminance_domain` / `luminance-domain` | enum | `sdr-ui`, `sdr-normal`, `sdr-extended`, `hdr-content` | What this window's content is *for*. Unset derives it from what the client declared |

> **These rules did nothing before 2026-08-17.** `sdr_white_scale` and
> `hdr_gain` were parsed, validated and stored on the window, and read by no
> part of the renderer — the config accepted them and the picture never
> changed. They are live as of M12. If you wrote either of them earlier and
> concluded it had no effect, you were right, and it does now.

## Luminance domains

A transfer function says how a source is *encoded*. It does not say what the
content is *for*, and on an HDR display those come apart: a terminal and a film
are both sRGB and should not share a white.

| domain | for | effect |
|---|---|---|
| `sdr-ui` | terminal, panel, launcher — desktop furniture | holds white at 203 cd/m², however bright the desktop reference is |
| `sdr-normal` | ordinary SDR applications | the desktop reference; changes nothing |
| `sdr-extended` | wide-gamut SDR, e.g. photography | reserved; no luminance change yet |
| `hdr-content` | HDR video, games, stills | absolute luminance semantics |

**You rarely need to write one.** HDR content is recognised automatically from
the image description a client attaches, and so is wide-gamut SDR (BT.2020
primaries with an SDR transfer). The one that *cannot* be derived is `sdr-ui`:
nothing in any protocol distinguishes a terminal from a film, so asking for a
restrained white is a decision you make, not one the compositor guesses.

Untagged windows stay `sdr-normal` deliberately. Defaulting them to `sdr-ui`
would dim every ordinary window on an HDR output the moment you upgraded, with
no rule written — so the automatic answer is the one that changes nothing.

```kdl
// hold the terminal's white at 203 even though the desktop reference is 280
window-rule { match app-id=kitty; luminance-domain "sdr-ui" }
```

Precedence, most specific first: an explicit `sdr-white-scale` or `hdr-gain`
beats a `luminance-domain`, which beats the domain derived from what the client
declared. A `luminance-domain` the compositor cannot parse keeps the derived
one — a typo does not silently become a policy.

`amsg get surface-intent` reports each window's class, whether it was derived
or ruled, and the multipliers actually applied.

`sdr_white_scale` and `hdr_gain` are **per window on purpose, and there is no
global equivalent of either**. On an HDR output every SDR application is
rendered at the desktop's reference white (`sdr_reference_luminance`, 203 cd/m²
by default); a global "HDR brightness" slider moves all of them at once, along
with the wallpaper and the panel, which is how an HDR desktop ends up looking
washed out. These move one window.

They apply to different classes of source and never to each other's:
`sdr_white_scale` multiplies content encoded as sRGB, gamma 2.2 or BT.1886 —
which is nearly everything — while `hdr_gain` multiplies content a client has
tagged as PQ or scRGB through `wp-color-management` (or frog). Setting
`hdr_gain` on a terminal does nothing; setting `sdr_white_scale` on an mpv
window playing HDR video does nothing.

```kdl
// a chat client that disappears against HDR video beside it
window-rule { match app-id=discord; sdr-white-scale 1.6 }
// a PQ video mastered brighter than this room wants
window-rule { match app-id=mpv; hdr-gain 0.7 }
```

`isnoanimation` also changes how the window LEAVES a tag, not only how it
moves. The tag-out slide parks a window past its monitor's edge and it is the
slide *finishing* that hides it, so a window that never animates would sit
parked there — on the next monitor, in a side-by-side layout — for as long as
its tag was hidden. Windows with this rule are hidden outright on a tag switch
instead, the same as fullscreen ones.

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
| `animation_type_close` | string | asteroid, fall, zoom, slide, fade, none | Set close animation |
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
| `force_tearing` / `force-tearing` | integer | `0` / `1` | Let this window's frames reach the screen without waiting for vblank. Lower latency, visible tearing. See [Tearing](/docs/configuration/monitors#tearing-game-mode) |
| `noscanout` / `no-scanout` | integer | `0` / `1` | Keep this window out of direct scan-out and push it through the render pass. For clients whose buffers are not safe to hand straight to a KMS plane — gamescope without explicit sync tears RGB noise across the screen otherwise |
| `vrr_only_fullscreen` / `vrr-only-fullscreen` | integer | `0` / `1` | Turn variable refresh on while this window is fullscreen and off again afterwards, rather than leaving it on for the whole output |
| `force_hdr` / `force-hdr` | integer | `0` / `1` | Switch the output to HDR while this window is on it. The way to run HDR for one player without an HDR desktop |
| `shield_when_capture` / `shield-when-capture` | integer | `0` / `1` | Cover this window with an opaque shield while a screen capture is running, so it does not appear in recordings or shares |

### Writing them back

```sh
amsg set-tag-rules @- <<'JSON'
{"changes":[{"op":"add","fields":{"id":"7","layout":"monocle","nmaster":"2"}}]}
JSON
```

`add`, `update` (by `index`, from `get tag-rules`) and `remove`. The block is
edited by byte span, so comments, spacing and anything the compositor does not
model survive; a rule that came from a generated file is refused with
`read-only-source`, and a legacy `tagrule=` leaf with `not-editable`, because
there is no block to rewrite.

A change with no `id` is refused rather than written: a `tag` block without one
applies to tag 0 — the `~0` tag — which is never what an editor meant.

### Examples

```kdl
binds {
    alt+h { toggle_named_scratchpad st-yazi none "st -c st-yazi -e yazi"; }
    Super+s { toggle_special_workspace term; }
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

window-rule { match app-id=^gamescope$; no-scanout 1 }

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

Every field also accepts its **hyphenated** spelling — `no-render-border`,
`open-as-floating`, `monitor-name`, `scroller-default-proportion` — which is how
the rest of this config language is written, and which is what the settings
window emits. The underscore forms above keep working.

They did not both work before: a `tag` block passed its child names through
verbatim, so a hyphenated field became a key the parser does not know and was
dropped in silence — the block parsed, the setting simply never applied.

### Reading them back

```sh
amsg get tag-rules          # every tag rule, with the file and line it came from
amsg get tag-rule-schema    # the fields: type, range, enum members, description
```

`get tag-rules` reports **only the fields a rule actually sets**. That is not an
economy: `ConfigTagRule` cannot distinguish a rule that wrote `0` from one that
wrote nothing, so a response listing every field with its default would make
"says nothing about nmaster" and "pins nmaster to its default" identical, and an
editor round-tripping them would turn every silence into an override.

Each rule carries where it came from — file, line, and whether it is `editable`
— so a tool can offer to rewrite the block it was read from rather than guess.
A rule from a generated file reports `writable: false` with the reason; a legacy
`tagrule=` leaf reports `kind: "legacy"` and is not editable, because there is no
block to rewrite.

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
