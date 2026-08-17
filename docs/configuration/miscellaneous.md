---
title: Miscellaneous
description: Advanced settings for XWayland, focus behavior, and system integration.
---

## System & Hardware

| Setting | Default | Description |
| :--- | :--- | :--- |
| `xwayland_persistence` | `1` | Keep XWayland running even when no X11 apps are open (reduces startup lag). |
| `xwayland_force_scale_one` | `1` | Size X11 windows in raw output pixels instead of logical units. X11 has no fractional scaling, so without this an X window on a 1.25x display commits a buffer 1.25x too small and the compositor magnifies it — a fullscreen game renders 3072x1728 on a 4K screen. With it, the window is asked for the real pixel count and its buffer is presented 1:1. Two costs: an app drawing its UI at a fixed pixel size comes out physically smaller, and absolute pointer position is clamped in the outer `1 − 1/scale` of a window because Xwayland's X screen stays the logical size. Games that grab the pointer read relative motion and are unaffected. See below. |
| `syncobj_enable` | `0` | Enable `drm_syncobj` timeline support (helps with gaming stutter/lag). **Requires restart.** |
| `primary_selection` | `1` | Advertise the middle-click "copy on select" clipboard. Set to `0` for one clipboard only: the global is not bound, so toolkits stop publishing on select and middle-click paste does nothing, and XWayland's X `PRIMARY` is refused too. **Requires restart.** |
| `render_late` | `0` | Adaptive render-late scheduling: defer each frame's render toward the next vblank so input is sampled fresher (cuts up to a frame of input latency). `2` additionally logs per-frame timing for tuning. |
| `render_late_margin_us` | `3000` | Safety margin (µs) subtracted from the render-late deferral so the render never misses its vblank. |
| `render_late_backoff` | `0.6` | Multiplier applied to the deferral fraction when a vblank is missed. |
| `render_late_climb_step` | `0.03` | Added to the deferral fraction after a run of clean frames. |
| `render_late_climb_frames` | `20` | How many clean frames before the fraction climbs. |
| `render_late_cap` | `0.65` | Ceiling on the deferral fraction. |
| `allow_lock_transparent` | `0` | Allow the lock screen to be transparent. |
| `allow_shortcuts_inhibit` | `1` | Allow shortcuts to be inhibited by clients. |
| `vrr` | - | Set via [monitor rule](/docs/configuration/monitors#monitor-rules). |

Render-late is a feedback loop: the deferral fraction climbs by
`render_late_climb_step` after `render_late_climb_frames` clean frames and is
multiplied by `render_late_backoff` whenever a vblank is missed, bounded by
`render_late_cap`. The four knobs exist so the law can be measured and adjusted
without a rebuild; the defaults are what live measurement says is correct
(the fraction reaches its cap ~74% of the time, with well under one slip per
thousand frames), so there is normally no reason to touch them.

The fraction is also floored so that a deferral can always still be *armed*.
Arming requires the computed delay to be at least 1 ms, and the loop only
adapts on frames where a deferral was armed — so without the floor, a fraction
small enough to stop arming is a state the loop can never climb out of, and
render-late silently stops working with no log and no recovery short of a
restart. The floor scales with the refresh interval, because the trap does:
the threshold is `1 / interval_ms`, which is 0.06 at 60 Hz but 0.24 at 240 Hz.

### X11 windows on a fractional-scale display

`xwayland_force_scale_one` exists because X11 has no fractional scaling and no
way to be told about it. A Wayland client negotiates its buffer scale with the
compositor; an X11 client asks for, and is told, a number of *pixels*, and has
no protocol to learn that a pixel on this display is worth 0.8 of a logical
one.

**It is on by default**, which is a deliberate trade rather than an obvious
one. A blurred fullscreen game is visible to everyone all the time; the cost
below — absolute pointer position clamped in the outer band of an oversized
window — is invisible to pointer-grabbing games, which are the case this is
for, and reachable only by X11 apps that both exceed the logical screen and
use absolute positioning. Set it to `0` if you have one of those and would
rather have the stretch.

With the option **off**, an X window on a 1.25× output is configured in logical
units, commits a buffer of that many pixels, and the renderer magnifies it to
fill the space. On a 4K display at 1.25×, a fullscreen game renders 3072×1728
and is stretched to 3840×2160. That stretch is the blur.

With it **on**, the window is asked for the real pixel count instead, and the
compositor presents that buffer across the logical box it belongs in — so the
1:1 mapping the display can actually show is the one it gets. The window's
position and size in the layout do not change: everything else in the
compositor still works in logical units, and only four things convert.

**Windows come out smaller.** An X11 window's size is now counted in device
pixels, so an app laying its interface out at a fixed pixel size is physically
smaller on screen. That is inherent — the same trade Hyprland's
`force_zero_scaling` makes, for the same reason. Toolkit apps that honour
`Xft.dpi` or `GDK_SCALE` can be told to compensate; a game rendering at the
panel's native resolution is the case this is for.

**How exact it is.** Sampling is pinned to nearest-neighbour for these
windows, since a buffer that is already the right size should never be
interpolated. Where a window's logical edges do not land on whole device
pixels — possible at 1.25× or 1.5× for a window at an odd position — the
renderer's edge rounding can leave the window one device pixel wider or
narrower than its buffer, which shows as a single duplicated or dropped row of
texels at one edge. A fullscreen window starts at 0 and is exact.

**Absolute pointer position is wrong in the outer band of a window.** This is
the real cost, and it is measured rather than estimated.

Xwayland sizes its X screen from the outputs' *logical* geometry — 1536×864
for a 1920×1080 display at 1.25× — because it derives that from the Wayland
output, not from anything asteroidz configures. This option sizes windows in
device pixels, so a fullscreen window is 1920×1080 inside a 1536×864 screen.
X11 requires the pointer to be inside the root window, so any position beyond
the screen is clamped to its edge before the client is told:

```text
scale 1.25, fullscreen         option OFF        option ON
  logical (600,400)       ->   600,400           750,500     correct
  logical (900,500)       ->   900,500           1125,625    correct
  logical (1300,700)      ->   1300,700          1535,863    clamped
  logical (1450,800)      ->   1450,800          1535,863    clamped
```

1535,863 is exactly one pixel inside the X screen. **Games that lock or grab
the pointer are unaffected** — they read relative motion, which carries no
position and is never clamped — and that is the case this option exists for.
An X11 app that uses absolute pointer position in the outer `1 − 1/scale` of
its window is not.

There is no fix available from inside the compositor: wlroots exposes no way
to tell Xwayland a different screen size, and reporting the outputs at scale 1
to enlarge it would take fractional scaling away from every Wayland client.
The limitation is pinned by the `1.25-screen-clamp` arm of
`contrib/xw-scale-test.sh`, which asserts the clamped values deliberately — if
it ever goes red, the screen got big enough and this section is obsolete.

This is why the option is **off by default**.

## Focus & Input

| Setting | Default | Description |
| :--- | :--- | :--- |
| `focus_on_activate` | `1` | Automatically focus windows when they request activation. |
| `sloppyfocus` | `1` | Focus follows the mouse cursor. |
| `float_click_to_focus` | `1` | In the floating layout, change focus only on click (not on pointer hover), so overlapping floating windows don't steal focus/auto-raise as the cursor crosses them. Set `0` to make `sloppyfocus` apply in float layout too. KDL: `layout { floating { click-to-focus 0 } }`. |
| `warpcursor` | `1` | Warp the cursor to the center of the window when focus changes via keyboard. |
| `cursor_hide_timeout` | `0` | Hide the cursor after `N` seconds of inactivity (`0` to disable). |
| `cursor_hide_on_keypress` | `0` | Hide the cursor on keypress. |
| `cursor_zoom_rigid` | `0` | Cursor zoom follow mode: `0` = the zoomed view lazily chases the cursor, `1` = the view is locked exactly to the cursor. See [`zoom_in`/`zoom_out`/`zoom_reset`](/docs/bindings/keys#cursor-zoom). |
| `cursor_zoom_step` | `0.1` | Default step added/removed per `zoom_in`/`zoom_out` call when no explicit step is given. |
| `drag_tile_to_tile` | `0` | Allow dragging a tiled window onto another to swap their positions. |
| `drag_tile_small` | `1` | Allow dragging a tiled window temporarily to small size.|
| `drag_corner` | `3` | Corner for drag-to-tile detection (0: none, 1–3: corners, 4: auto-detect). |
| `drag_warp_cursor` | `1` | Warp cursor when dragging windows to tile. |
| `axis_bind_apply_timeout` | `100` | Timeout (ms) for detecting consecutive scroll events for axis bindings. |

## Multi-Monitor & Tags

| Setting | Default | Description |
| :--- | :--- | :--- |
| `focus_cross_monitor` | `0` | Allow directional focus to cross monitor boundaries. |
| `exchange_cross_monitor` | `0` | Allow exchanging clients across monitor boundaries. |
| `focus_cross_tag` | `0` | Allow directional focus to cross into other tags. |
| `view_current_to_back` | `0` | Toggling the current tag switches back to the previously viewed tag. |
| `scratchpad_cross_monitor` | `0` | Share the scratchpad pool across all monitors. |
| `single_scratchpad` | `1` | Only allow one scratchpad (named or standard) to be visible at a time. |

## Window Behavior

| Setting | Default | Description |
| :--- | :--- | :--- |
| `enable_floating_snap` | `0` | Snap floating windows to edges or other windows. |
| `snap_distance` | `30` | Max distance (pixels) to trigger floating snap. |
| `no_border_when_single` | `0` | Remove window borders when only one window is visible on the tag. |
| `idleinhibit_ignore_visible` | `0` | Allow invisible clients (e.g., background audio players) to inhibit idle. |
| `tag_carousel` | `0` | Enable tag carousel (cycling through tags). |
| `drag_tile_refresh_interval` | `8.0` | Interval (1.0–16.0) to refresh tiled window resize during drag. Too small may cause application lag. |
| `drag_floating_refresh_interval` | `8.0` | Interval (1.0–16.0) to refresh floating window resize during drag. Too small may cause application lag. |

## GPU

| Setting | Default | Description |
| :--- | :--- | :--- |
| `gpu` | *(empty)* | Which GPU to drive, as a DRM node (`/dev/dri/card1`) or a PCI address (`0000:03:00.0`). Empty lets wlroots choose. |

```kdl
gpu "0000:03:00.0"
```

**Only relevant with more than one GPU.** On a single-GPU machine there is
nothing to choose and this should stay empty.

Left empty, wlroots picks the DRM device by `boot_vga` and udev enumeration
order, promoting the "primary" device to the front. That is a heuristic, an
ordering, and a startup race stacked together: a device that cannot be opened is
skipped silently, so a discrete card whose driver has not finished coming up
leaves the integrated one first in the list. A machine with two AMD GPUs has been
observed compositing on the integrated one while both displays hung off the
discrete card — every frame then crosses PCIe to reach the screen. Correct
behaviour from the renderer, which always renders on the device it presents on,
and a bad outcome.

Naming the device replaces all of that with a fact.

**A PCI address is the better spelling.** `cardN` numbering depends on probe
order and can move between boots; a PCI address does not. Find yours with
`lspci | grep -i vga`, or check which card your displays are on:

```bash
for c in /sys/class/drm/card*-*/status; do
  [ "$(cat "$c")" = connected ] && echo "$(basename "$(dirname "$c")")"
done
```

**A wrong value costs the preference, not the session.** An address matching no
card, or a node that cannot be opened, logs the reason and lets wlroots choose —
there is no way to fix a config file from a desktop that will not start.

`WLR_DRM_DEVICES` in the environment wins over this setting: someone who set it
is debugging exactly this question, and a config file silently overriding the
environment is how a debugging session stops meaning anything.

`amsg get avk-stats | jq -r '.physical_device, .drm_device'` reports the GPU
actually in use.
