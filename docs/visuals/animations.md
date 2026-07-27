---
title: Animations
description: Configure smooth transitions for windows and layers.
---

## Enabling Animations

asteroidz supports animations for both standard windows and layer shell surfaces (like bars and notifications).

```kdl
misc {
    animations 1
    layer_animations 1
}
```

## Animation Types

You can define different animation styles for opening and closing windows and layer surfaces.

Available types: `slide`, `zoom`, `fade`, `none`, plus `asteroid` and `fall`
for closing windows.

```kdl
animations {
    window-open {
        type zoom
    }
    window-close {
        type slide
    }
}
misc {
    layer_animation_type_open slide
    layer_animation_type_close slide
}
```

### `asteroid` — the default close animation

The window comes apart the way a rock does in the arcade game: it is replaced
by a jagged **vector outline** of the same size which splits into four smaller
rocks, each tumbling and drifting straight out from the centre, with a handful
of line streaks thrown off alongside. They fade as they go and are gone inside
the close duration.

```kdl
animations {
    window-close {
        type asteroid
        duration 250
    }
}
```

Not the window's own pixels. The 1979 machine drew everything as white line
loops, and a rock breaking up is that loop becoming smaller loops — so the
pixels go and the outline takes over. Slicing the snapshot into moving tiles is
a different effect, and it is still here under its old name:

### `fall` — the window's own pixels, in pieces

`fall` breaks the closing window into a grid of tiles that fly out from the
centre and fade. It reads as breaking glass rather than as an explosion,
because the pieces carry photographic content and — this is the constraint that
shapes both animations — **cannot rotate**. A scene node has a position, a size
and a crop, and no transform.

```kdl
animations {
    window-close {
        type fall
        duration 250
        fall-columns 4   // tiles across (1–12, default 4)
        fall-rows 3      // tiles down  (1–12, default 3)
    }
}
```

`fall-columns` and `fall-rows` apply to this one only; `asteroid` always makes
four rocks, because that is what a rock does in the game and a dozen pieces of
a window read as confetti.

Neither uses gravity, an arc, or any settling: debris in that game leaves the
wreck in a straight line and fades before it gets anywhere. Distance eases out
in both, putting most of the travel in the first third, which is what sells a
burst at a duration short enough to stay out of the way.

**No shader is involved, and none is needed.** Rotation is why `asteroid` is
drawn rather than sliced: a tumbling fragment has to be re-drawn at its current
angle every frame, so each one is a handful of stroked cairo paths into its own
small `wlr_buffer` — the same thing the UFO easter egg, every text node and
every icon in this compositor already do. A dozen polygons a frame is work a
CPU does not notice, and the scene graph only ever sees an ARGB buffer, so it
renders identically on the GLES and Vulkan backends. A GPU pass would mean
renderer-specific code twice over for no visible difference. Per-fragment
buffers rather than one screen-sized surface, too: a fullscreen window would
otherwise mean an 11 MB allocation every frame.

Debris also never lands on a screen it did not come from. These are nodes in a
global layer, so a piece thrown past the edge of its monitor would otherwise
turn up on the neighbouring one. A fragment is dropped on **entering** another
monitor rather than on leaving its own — a maximised window's outer pieces
start flush against their own edge, so the stricter test would blink the whole
outer ring out on the first frame. Flying off the outside edge of the desk is
fine; there is nothing out there to pollute.

## Fade Settings

Control the fade-in and fade-out effects for animations.

```kdl
misc {
    animation_fade_in 1
    animation_fade_out 1
}
animations {
    window-open {
        fade-begin-opacity 0.5
    }
    window-close {
        fade-begin-opacity 0.5
    }
}
```

- `animation_fade_in` — Enable fade-in effect (0: disable, 1: enable)
- `animation_fade_out` — Enable fade-out effect (0: disable, 1: enable)
- `fadein_begin_opacity` — Starting opacity for fade-in animations (0.0–1.0)
- `fadeout_begin_opacity` — Starting opacity for fade-out animations (0.0–1.0)

## Zoom Settings

Adjust the zoom ratios for zoom animations.

```kdl
misc {
    zoom_initial_ratio 0.4
    zoom_end_ratio 0.8
}
```

- `zoom_initial_ratio` — Initial zoom ratio
- `zoom_end_ratio` — End zoom ratio

## Durations

Control the speed of animations (in milliseconds).

| Setting | Type | Default | Description |
| :--- | :--- | :--- | :--- |
| `animation_duration_move` | integer | `500` | Move animation duration (ms) |
| `animation_duration_open` | integer | `400` | Open animation duration (ms) |
| `animation_duration_tag` | integer | `300` | Tag animation duration (ms) |
| `animation_duration_close` | integer | `300` | Close animation duration (ms) |
| `animation_duration_focus` | integer | `0` | Focus change (opacity transition) animation duration (ms) |

```kdl
misc {
    animation_duration_move 500
    animation_duration_tag 300
    animation_duration_focus 0
}
animations {
    window-open {
        duration 400
    }
    window-close {
        duration 300
    }
}
```

## Custom Bezier Curves

Bezier curves determine the "feel" of an animation (e.g., linear vs. bouncy). The format is `x1,y1,x2,y2`.

You can visualize and generate curve values using online tools like [cssportal.com](https://www.cssportal.com/css-cubic-bezier-generator/) or [easings.net](https://easings.net).

| Setting | Type | Default | Description |
| :--- | :--- | :--- | :--- |
| `animation_curve_open` | string | `0.46,1.0,0.29,0.99` | Open animation bezier curve |
| `animation_curve_move` | string | `0.46,1.0,0.29,0.99` | Move animation bezier curve |
| `animation_curve_tag` | string | `0.46,1.0,0.29,0.99` | Tag animation bezier curve |
| `animation_curve_close` | string | `0.46,1.0,0.29,0.99` | Close animation bezier curve |
| `animation_curve_focus` | string | `0.46,1.0,0.29,0.99` | Focus change (opacity transition) animation bezier curve |
| `animation_curve_opafadein` | string | `0.46,1.0,0.29,0.99` | Open opacity animation bezier curve |
| `animation_curve_opafadeout` | string | `0.5,0.5,0.5,0.5` | Close opacity animation bezier curve |

```kdl
misc {
    animation_curve_open 0.46,1.0,0.29,0.99
    animation_curve_move 0.46,1.0,0.29,0.99
    animation_curve_tag 0.46,1.0,0.29,0.99
    animation_curve_close 0.46,1.0,0.29,0.99
    animation_curve_focus 0.46,1.0,0.29,0.99
    animation_curve_opafadein 0.46,1.0,0.29,0.99
    animation_curve_opafadeout 0.5,0.5,0.5,0.5
}
```

## Tag Animation Direction

Control the direction of tag switch animations.

| Setting | Default | Description |
| :--- | :--- | :--- |
| `tag_animation_direction` | `1` | Tag animation direction (1: horizontal, 0: vertical) |