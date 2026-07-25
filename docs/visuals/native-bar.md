---
title: Native Bar
description: The compositor's own built-in status bar.
---

asteroidz can draw its own status bar, with no external process and no
layer-shell client involved. It is built from the same pill widget as window
titlebars and monocle tab strips, so it inherits the [theme](./theming.md)
automatically and renders at each output's own scale.

This is **off by default**, and it is not a replacement for
[Waybar](./status-bar.md) yet — see [Scope](#scope) below for exactly what it
does and does not do today. The two can run side by side.

## Enabling it

```kdl
bar {
    enable true
    height 48
    position "top"          // or "bottom"
    margin { x 8; y 9 }
    spacing 8
    pill-inset 6

    modules-left   "tags,layout,title"
    modules-center "media,clock,weather,idle"
    modules-right  "cpu,memory,network"

    clock { format "%H:%M:%S  ·  %a %d %b" }
}
```

| Key | Default | Meaning |
|---|---|---|
| `enable` | `false` | draw the bar at all |
| `height` | `48` | strip height in logical pixels (8–200) |
| `position` | `"top"` | `"top"` or `"bottom"` |
| `margin.x` | `8` | gap between the panels and the output's left/right edge |
| `margin.y` | `9` | gap between the panels and the output's top/bottom edge |
| `spacing` | `8` | gap between adjacent pills, applied only where a *chip* module (`tags`, `layout`) is on one side |
| `pill-min-width` | `28` | floor width, so single-glyph pills stay legible |
| `pill-inset` | `6` | vertical inset of the pill row inside the strip, so chips sit *in* the panel rather than spanning it |
| `pill-padding` | `6` | horizontal padding inside a status pill |
| `tag-padding` | `16` | horizontal padding inside a workspace/layout chip |
| `icon-spacing` | `5` | exact gap between two adjacent icon-only status glyphs (`cpu`, `memory`), which carry no padding of their own |
| `volume-step` | `5` | percentage points the `volume` pill moves per scroll notch |
| `min-tags` | `3` | pad the visible tag set up to this many with empty tags |
| `show-logo` | `true` | leading asteroidz ship pill on the workspace group |
| `tag-icons` | `3` | app icons drawn inside each tag pill (0 disables, max 4) |
| `show-all-tags` | `false` | `true` draws every configured tag; `false` draws only selected or occupied ones |
| `panel.enable` | `true` | draw a backdrop panel behind each non-empty slot |
| `panel.color` | `0x0a0a0cd9` | panel fill (RGBA; the default is ~85% opaque) |
| `panel.radius` | `9` | panel corner radius |
| `panel.padding` | `6` | **horizontal** inset between the panel edge and its pills |
| `panel.blur` | `true` | blur behind the panel (needs `effects.blur.enable`) |
| `panel.shadow` | `true` | drop shadow under the panel (needs `shadows`) |
| `modules-left` | `"tags,layout,title"` | comma-separated module list |
| `modules-center` | `"clock"` | " |
| `modules-right` | *(empty)* | " |
| `clock.format` | `"%H:%M:%S"` | `strftime` format |
| `media-width` | `280` | pinned width of the now-playing pill |
| `weather.interval` | `15` | minutes between forecast fetches |
| `weather.location` | *(empty)* | city name; empty means IP geolocation |
| `interval` | `2` | seconds between `/proc` + `/sys` metric samples |
| `title-width` | `320` | **cap** on the title pill's width (`0` = uncapped); a shorter title gets a shorter pill |
| `icon-dir` | `~/.local/share:/usr/share` | colon-separated search path of Waybar plugin asset roots; first readable hit wins |

Changes take effect on `reload_config` — including changes to the module
lists themselves, which rebuild the bars rather than just refreshing them.

An unknown module name is reported on stderr and skipped, so a config written
for a newer build still starts.

## Modules

| Name | Shows | Click |
|---|---|---|
| `tags` | the ship logo, then one chip per selected or occupied tag, labelled `N` or `N: name` (`tag N { name … }`, `set_tag_name`), followed by up to `tag-icons` app icons for what is running there | views that tag |
| `clock` | `strftime` of `clock.format` | — |
| `title` | the focused window's title, with its app icon | focuses that window |
| `layout` | the current layout, as its Waybar SVG | cycles `circle_layout` |
| `cpu` | total CPU load, from `/proc/stat` deltas — **icon only**, tinted by load | — |
| `memory` | used memory, from `/proc/meminfo` — **icon only**, tinted by load | — |
| `network` | link state and ↓/↑ throughput, from `/sys/class/net` | — |
| `idle` | manual idle-inhibit state ("keep awake") | toggles it |
| `weather` | current temperature and condition, from open-meteo | — |
| `media` | now playing (title • artist), from MPRIS | play/pause |
| `volume` | default sink level, with a speaker icon (also accepts `vol`) | click toggles mute; scroll steps by `volume-step` |
| `tray` | one icon per StatusNotifierItem (also accepts `systray`) | left: `Activate`; right/middle: `SecondaryActivate` |

The `tags` module mirrors the Waybar workspace module it replaces: it shows
every tag that is **selected or holds a window**, then pads with the
lowest-numbered empty tags up to `min-tags` (its `min-pills`). So a fresh
session shows three pills rather than one, and a nine-tag setup never spends
nine pills of width on tags holding nothing. `show-all-tags true` overrides
this with the dwm/dwl behaviour of always drawing every tag — but on a narrow
output those extra pills cost enough width to squeeze the centre section out.
An urgent tag is drawn in the theme's `urgent-color`.

## System tray

The `tray` module is a **StatusNotifierItem host**, on the same session bus the
compositor already pumps from its event loop. No helper process, no
`snixembed`, no XEmbed.

Despite the name, `org.kde.StatusNotifierWatcher` needs **no part of KDE
installed or running**. It is a plain D-Bus name, and the compositor owns and
serves it itself; the `org.kde.` prefix is only historical, from KDE having
authored the spec before it went to freedesktop. It is what Steam, Discord,
Electron's AppIndicator, nm-applet, blueman, Syncthing and Nextcloud all
publish to. asteroidz additionally owns
`org.freedesktop.StatusNotifierWatcher`, the vendor-neutral spelling of the
same interface, exported from the same object.

The role is decided at startup:

- **Watcher** — we own the watcher name and applications register with us
  directly. This is the normal case.
- **Client** — another shell (a running waybar, say) already owns it. Taking
  the name away is not possible and queueing for it would leave the tray dead
  until that process exits, so asteroidz instead registers as a plain *host*
  with the incumbent watcher and mirrors its item list. Both bars then show
  the same tray, which is what you want while migrating from one to the other.

Icons come from `IconName` (resolved through the icon theme, and through the
item's own `IconThemePath` when it ships one — Electron apps and Steam name
icons that exist in no installed theme), falling back to the raw `IconPixmap`
pixels, which are decoded from wire-order ARGB32 into the shared icon cache.
`NeedsAttention` swaps in the attention artwork and fills the pill in the
theme's urgent colour. A `Passive` item is hidden: that status means "nothing
to say right now", and drawing it anyway is how a tray becomes a row of
identical grey squares.

Every call is async, so a wedged tray application cannot stall the compositor,
and an application that exits without unregistering is dropped on
`NameOwnerChanged` rather than leaving a dead pill behind.

**Not implemented: the DBusMenu context menu.** It needs a popup surface with a
keyboard grab and its own hit-testing, and the bar has no layer for that yet.
Right-click sends the item's own `SecondaryActivate`, which most applications
wire to "show my menu" regardless.

## Panels

The bar has **no background of its own** — it is fully transparent. What you
see is one rounded translucent panel per non-empty slot, so the bar reads as
three floating groups rather than a full-width strip. That is the same shape as
the grouped panels in a typical Waybar config.

With panels on, a resting pill draws no background either; only the selected
tag (and any urgent one) is filled, so each panel reads as a single surface.
Turn panels off with `panel { enable false }` and every pill carries the
theme's resting colours instead, giving a row of separate pills.

The margin is measured to the *panel* edge, and panel padding is horizontal
only — the panel is exactly as tall as the bar strip, so the gap above and
below comes from `margin.y` alone. This mirrors the `margin: 9px 4px` /
`padding: 0 6px` of the Waybar groups it replaces.

Blur and shadow reuse the same scenefx nodes as the overview's top strip, and
each defers to the global `effects.blur.enable` / `shadows` settings — asking
for panel blur in a build with blur off costs nothing and draws nothing.

## Icons and fixed widths

Tag pills carry real **application icons** for the windows on that tag,
resolved through the same icon-theme lookup the titlebars use. Duplicate
app-ids collapse, so three terminals on one tag read as "terminals live here"
rather than filling the pill with the same glyph three times. Tag pills are
deliberately *not* width-pinned: a tag gaining a window is a real layout
change, not the per-tick jitter the pinning exists to suppress.

Pill icons are the **same SVGs the Waybar plugins use**, loaded straight from
their installed asset trees under `icon-dir` — `waybar-sysinfo/cpu.svg`,
`waybar-sysinfo/ethernet.svg`,
`waybar-asteroidz-workspaces/layouts/<layout>.svg`, and so on. `icon-dir` is a
**search path**, not a single directory, because the plugins do not share a
prefix: some are packaged into `/usr/share`, others land in
`~/.local/share` from a plain `make install`. A missing file means no icon,
never an error, so an incomplete asset install degrades to text.

The status-plugin SVGs are **monochrome stencils** (a solid `#000`
silhouette), meant to be recoloured by the widget drawing them — painted as-is
they are an invisible black blob on a dark panel. The bar therefore paints its
tint *through* the icon's alpha, the same thing Waybar's `wb_themed_pixbuf`
does. Application icons and the ship logo are real artwork and are never
tinted.

`cpu` and `memory` are **icon only**: the colour is the reading, in four steps
— resting (foreground at 45% alpha), working (theme accent) from 20%, heavy
(amber) from 60%, saturated (theme urgent) from 85%. The exact figure is not
shown; that belongs in a popover, which the native bar does not have yet.

Those two pills are laid out as one run: no padding of their own, `icon-spacing`
between them, and no `pill-min-width` floor (that floor exists to keep a
single-glyph *label* readable, and on an icon it only pads slack around the
artwork). They therefore sit exactly `icon-spacing` apart, which padding —
being symmetric — could never express as an odd number of pixels.

Every pill whose content changes shape is **pinned to the width of its widest
possible content**, so the bar never reflows and a pill never moves out from
under the pointer: throughput to `↓999.9M ↑999.9M`, the icon-only pills to
their artwork, and the clock to the widest rendering of its own `strftime`
format (probed once per month name, so `%a`/`%b` length variation is covered).
The title is the exception: `title-width` is a **cap**, not a pin, so a short
title takes a short pill.

### When it does not all fit

The left and right slots are anchored to their edges; only the centre has
anywhere to go — and the whole point of the centre slot is that it is
*centred*, so it never yields first. Pills carrying ellipsisable text (the
title, the now-playing string) are marked flexible and give width back in two
passes: once if the three slots over-subscribe the output outright, and again
if the left slot reaches where the centred slot wants to start. A window title
therefore ellipsises as it approaches the clock rather than pushing the clock
off centre.

If the centre still cannot fit between its neighbours after that, it is
**hidden** rather than drawn through them — priority is workspaces, then
status, then the clock. A configuration that over-subscribes the width even
after that (a dozen pills on a 1000px output) will still overlap left against
right; there is nothing left to give.

## How it reserves space

The bar claims its strip from the output's usable area *before* layer-shell
surfaces are arranged. An external bar's exclusive zone therefore stacks
below it rather than fighting it, which is what lets you move modules across
one at a time while Waybar keeps the rest.

The footprint taken is `height + 2 × margin.y`.

## Building without it

The bar is a compile-time feature. With it off, none of the code reaches the
binary — no scene nodes, no timer, no config keys:

```sh
meson setup build -Dnative-bar=false
```

The `bar { }` config keys become unknown keys in that build, which the parser
warns about and ignores.

## Scope

Implemented: the bar frame, per-monitor layout in three panelled slots, click
routing, space reservation, and the twelve modules above — everything that
needs no popover.

`idle` reflects a **compositor-level** override, not the client-driven
`idle_inhibit_v1` protocol state — a bar module has no surface to attach a
protocol inhibitor to, the way Waybar's module does. The same override is
available as `amsg dispatch toggle_idle_inhibit` (append `,1`/`,0` to force a
state), so it can be bound to a key as well.

The metric modules read `/proc` and `/sys` directly, on a shared timer, once
per machine rather than once per monitor. That is deliberate and it is the
line between what is cheap to run in-compositor and what is not: the Waybar
plugins they replace fork `wpctl`/`nmcli`/`curl` on their main loop, which is
an invisible stutter in a bar process but a dropped frame and an input hitch
in a compositor.

`media` reads MPRIS over the session bus asteroidz already owns and pumps
from its event loop, with **no `playerctl` subprocess** — the bus answers
directly. Every call is asynchronous: `sd_bus_call()` blocks for up to its
25-second default timeout, and one unresponsive media player must never be
able to freeze the compositor. An already-followed player keeps priority over
newly-appearing ones so the pill does not flap between two open players.

`weather` is the first module that talks to the network. It shells out to
`curl` through an **async** helper (`src/common/async-spawn.h`): fork, keep the
read end of a pipe in the compositor's event loop, deliver the output in a
callback. It is never synchronous — a 3-second DNS stall on the compositor's
own event loop is a 3-second freeze of the whole session. Only one request is
in flight at a time, so a slow network cannot queue up a `curl` per tick. Same
open-meteo source and the same WMO-code→artwork mapping as the Waybar plugin,
so the pill is indistinguishable from it.

`tray` is a full StatusNotifierItem host — see [System tray](#system-tray) —
minus the DBusMenu context menu, which needs a popup layer that does not exist
yet.

`volume` is **event-driven**, which is the only way it belongs in a
compositor. One long-lived `pactl subscribe` reports every mixer change on
stdout, and each relevant event triggers a single asynchronous
`wpctl get-volume`; there is no poll and no fork per tick for state that
changes a few times an hour. `wpctl`/`pactl` rather than a libpulse or
libpipewire dependency: the round trip is off the event loop either way, and
linking a sound-server client into the compositor to render one glyph is not a
trade worth making.

With no sound server the subscriber exits immediately and the level never
arrives. Since the module is started from the refresh path — which runs on
every arrange — an unguarded restart would fork two processes per refresh, so
attempts are backed off to one every ten seconds. That still reconnects on its
own if pipewire is restarted underneath the session, and the regression suite
pins it by counting the compositor's children after a burst of dispatches.

Scroll is routed to whatever pill is under the pointer, and consumed there —
so scrolling the volume readout changes the volume even when the same wheel is
bound to switching tags. Both input families are handled, which matters more
than it sounds: a mouse wheel reports a discrete notch count, while a trackpad
or a high-resolution wheel reports only a stream of small continuous deltas
and no notch at all. Keying off the notch count alone leaves the pills dead
under a trackpad, so continuous motion is accumulated until it adds up to a
notch. The tray forwards scrolls to the item as the spec's
`Scroll(delta, orientation)`, letting a mixer applet take volume and a pager
take workspaces.

Not implemented yet: popovers, so no click-through panels, no tray context
menus, and no volume slider or output picker — those stay in Waybar.

## Developing against it

A nested session is the safe way to iterate on the bar without restarting your
real one:

```sh
WLR_BACKENDS=wayland ./build/asteroidz -c /path/to/test.kdl
```

Give the test config no `output` block (or one with no `width`/`height`): a
nested output has no mode list, so it adopts the size of its host window and
follows it on resize. Naming an explicit mode there is likely to be rejected by
the backend, in which case the output falls back to the host window's size and
logs the rejection.

Do not point a nested instance at your real config — it would re-run every
`spawn-at-startup` entry into the live session.

## Cost

`bar_update` runs off the same internal broadcast that drives IPC event
watchers, which fires on every arrange — including each step of a window
drag. It hashes what the bar currently displays and returns early when
nothing changed, so an arrange that does not alter the bar costs one walk of
the client list and no redraw. The clock timer aligns to the next boundary
its format can actually show: a format without `%S` wakes once a minute, not
once a second.
