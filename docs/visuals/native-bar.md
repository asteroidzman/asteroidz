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

    modules-left   "tags,title"
    modules-center "clock"
    modules-right  "layout"

    clock { format "%H:%M" }
}
```

| Key | Default | Meaning |
|---|---|---|
| `enable` | `false` | draw the bar at all |
| `height` | `28` | pill height in logical pixels (8–200) |
| `position` | `"top"` | `"top"` or `"bottom"` |
| `margin.x` | `8` | inset from the output's left/right edge |
| `margin.y` | `4` | inset from the output's top/bottom edge |
| `spacing` | `6` | gap between adjacent pills |
| `pill-min-width` | `28` | floor width, so single-glyph pills stay legible |
| `show-all-tags` | `false` | `true` draws every configured tag; `false` draws only selected or occupied ones |
| `panel.enable` | `true` | draw a backdrop panel behind each non-empty slot |
| `panel.color` | `0x0a0a0cd9` | panel fill (RGBA; the default is ~85% opaque) |
| `panel.radius` | `9` | panel corner radius |
| `panel.padding` | `6` | inset between the panel edge and its pills |
| `panel.blur` | `true` | blur behind the panel (needs `effects.blur.enable`) |
| `panel.shadow` | `true` | drop shadow under the panel (needs `shadows`) |
| `modules-left` | `"tags,title"` | comma-separated module list |
| `modules-center` | `"clock"` | " |
| `modules-right` | `"layout"` | " |
| `clock.format` | `"%H:%M"` | `strftime` format |

Changes take effect on `reload_config` — including changes to the module
lists themselves, which rebuild the bars rather than just refreshing them.

An unknown module name is reported on stderr and skipped, so a config written
for a newer build still starts.

## Modules

| Name | Shows | Click |
|---|---|---|
| `tags` | one pill per selected or occupied tag, using custom tag names (`tag N { name … }`, `set_tag_name`) when set | views that tag |
| `clock` | `strftime` of `clock.format` | — |
| `title` | the focused window's title, with its app icon | focuses that window |
| `layout` | the current layout's symbol | — |

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

Blur and shadow reuse the same scenefx nodes as the overview's top strip, and
each defers to the global `effects.blur.enable` / `shadows` settings — asking
for panel blur in a build with blur off costs nothing and draws nothing.

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

Implemented: the bar frame, per-monitor layout in three panelled slots, click routing,
space reservation, and the four modules above — everything that needs no
external process and no popover.

Not implemented yet: popovers (so no click-through panels), the system tray,
and modules that wrap a CLI or D-Bus service (volume, network, weather,
media, …). Those stay in Waybar for now.

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
