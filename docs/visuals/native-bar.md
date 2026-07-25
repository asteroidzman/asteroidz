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
    height 30
    position "top"          // or "bottom"
    margin { x 8; y 4 }
    spacing 6
    pill-min-width 28

    modules-left   "tags,layout,title"
    modules-center "clock"
    modules-right  "cpu,memory,network"

    clock { format "%H:%M" }
}
```

| Key | Default | Meaning |
|---|---|---|
| `enable` | `false` | draw the bar at all |
| `height` | `28` | pill height in logical pixels (8–200) |
| `position` | `"top"` | `"top"` or `"bottom"` |
| `margin.x` | `8` | gap between the panels and the output's left/right edge |
| `margin.y` | `9` | gap between the panels and the output's top/bottom edge |
| `spacing` | `6` | gap between adjacent pills |
| `pill-min-width` | `28` | floor width, so single-glyph pills stay legible |
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
| `modules-right` | *(empty)* | " (the tray will live here) |
| `clock.format` | `"%H:%M:%S"` | `strftime` format |
| `interval` | `2` | seconds between `/proc` + `/sys` metric samples |
| `title-width` | `320` | pinned width of the title pill (`0` = size to content) |
| `icon-dir` | `/usr/share` | root of the Waybar plugin asset trees the pill icons come from |

Changes take effect on `reload_config` — including changes to the module
lists themselves, which rebuild the bars rather than just refreshing them.

An unknown module name is reported on stderr and skipped, so a config written
for a newer build still starts.

## Modules

| Name | Shows | Click |
|---|---|---|
| `tags` | the ship logo, then one pill per selected or occupied tag — custom tag names (`tag N { name … }`, `set_tag_name`) plus up to `tag-icons` app icons for what is running there | views that tag |
| `clock` | `strftime` of `clock.format` | — |
| `title` | the focused window's title, with its app icon | focuses that window |
| `layout` | the current layout, as its Waybar SVG | cycles `circle_layout` |
| `cpu` | total CPU load, from `/proc/stat` deltas | — |
| `memory` | used memory, from `/proc/meminfo` | — |
| `network` | link state and ↓/↑ throughput, from `/sys/class/net` | — |
| `idle` | manual idle-inhibit state ("keep awake") | toggles it |

By default the `tags` module hides tags that are both empty and unselected, so
a nine-tag setup does not permanently spend nine pills on tags holding nothing.
The side effect is that a fresh session shows a *single* pill, which reads as
broken rather than tidy — set `show-all-tags true` for the dwm/dwl behaviour of
always drawing every tag. An urgent tag is drawn in the theme's `urgent-color`.

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
their installed asset trees under `icon-dir` — `waybar-sysmon/cpu.svg`,
`waybar-network/ethernet.svg`,
`waybar-asteroidz-workspaces/layouts/<layout>.svg`, and so on. A missing file
means no icon, never an error, so an incomplete asset install degrades to
text.

Every pill whose content changes shape is **pinned to the width of its widest
possible content**, so the bar never reflows and a pill never moves out from
under the pointer: percentages to `100%`, throughput to `↓999.9M ↑999.9M`, the
layout pill to a square, the title to `title-width`, and the clock to the
widest rendering of its own `strftime` format (probed once per month name, so
`%a`/`%b` length variation is covered).

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
routing, space reservation, and the seven modules above — everything that needs
no external process and no popover.

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

Not implemented yet: popovers (so no click-through panels), the system tray,
and the modules that genuinely need a subprocess or a D-Bus session — volume
(wpctl/pactl), weather (HTTP), media (MPRIS). Those need async plumbing before
they can move in-process, and stay in Waybar until then.

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
