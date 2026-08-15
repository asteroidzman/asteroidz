---
title: Animations
description: Configure smooth transitions for windows and layers.
---

## Enabling Animations

asteroidz supports animations for both standard windows and layer shell surfaces (like bars and notifications). `animations` covers windows; `layer_animations` covers layer-shell surfaces and is off by default, so a bar or notification appears without one until you turn it on.

```kdl
misc {
    animations 1
    layer_animations 1
}
```

## Animation Types

You can define different animation styles for opening and closing windows and layer surfaces.

Available types: `slide`, `zoom`, `fade`, `none`, plus `asteroid`, `fall` and
`shatter` for closing windows.

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

### `shatter` — the same pixels, tumbling, under gravity

`fall`'s note above — that the pieces **cannot rotate**, because a scene node
has a position, a size and a crop and no transform — was a statement about the
renderer, not about the animation. `shatter` is what the animation looks like
once that stops being true.

The window breaks into a square grid of fragments which are thrown outward,
**rotate as they go**, and **fall under gravity**, arcing over instead of
travelling in a straight line. It reads as glass hitting a floor rather than as
a burst.

```kdl
animations {
    window-close {
        type shatter
        duration 350
        shatter-fragments 6   // fragments PER AXIS (2–12, default 6)
    }
}
```

One number and not a column/row pair: the grid is square. Gravity, launch speed
and spin are **not settings**. They are internal constants with deterministic
per-fragment jitter, because the three are not independent — the launch speed
that reads as *thrown* depends on the gravity that reads as *falling*, and a
spin rate that can be set can be set to something that does not look like
anything.

**It needs the Vulkan renderer.** Rotation is an
[`AVK_CMD_TEXTURE_QUAD`](../avk-effects.md#p2--the-arbitrary-corner-textured-quad),
a primitive whose four destination corners are placed independently; the
SceneFX/GLES path has no such primitive and falls back to `fall`. The two
renderers are allowed to differ here rather than the better one being held back
to what the older one can express.

The trajectory is a **closed form in wall-clock time** — `p(t) = p₀ + v₀t +
½gt²`, `θ(t) = θ₀ + ωt` — evaluated at each output's own presentation instant,
never advanced once per frame. That is what keeps a close finishing at the same
moment on a 60 Hz and a 144 Hz screen, and on a window spanning both.

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
| `animation_duration_focus` | integer | `1` | Focus change (opacity transition) animation duration (ms) |

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

## Spring Curves

Overshoot alone is not the difference — a bezier overshoots too, if you put a
control point outside `0..1`, which is what the "back" easings on easings.net
are. But a cubic can only overshoot *once* before it has to settle. A spring with
low damping **rings**: it crosses the target, comes back past it, and does that
several times on the way to rest. No cubic bezier produces that shape.

The parameters are the other half of it. Two numbers with physical meaning,
instead of four control points whose effect you have to see plotted to predict.

The defaults ring gently: `0.75`/`18` overshoots by about 3% and crosses the
target four times before settling. `0.2`/`40` overshoots by half the distance and
crosses it twelve times, which is the "bouncy" end. At `damping 1` and above
there is no overshoot at all.

`spring` does **not** replace every curve above. It applies to **move**, **open**
and **tag** only; close, focus and both opacity fades stay on their bezier
whatever this is set to, and for two different reasons. The fades stay because a
spring overshoots and opacity has nowhere to overshoot to — it would have to
leave the 0–1 range. Close stays because a spring models arriving at a target,
and a closing window is not arriving anywhere; springing it would bounce a
window back toward the viewer on its way out.

| Setting | Default | Description |
| :--- | :--- | :--- |
| `animation_curve_type` | `bezier` | `bezier` follows the curves above; `spring` uses the two values below, for move/open/tag. |
| `spring_damping` | `0.75` | How quickly the spring settles, from `0.1` to `2`. Below `1` it overshoots and springs back; at `1` and above it eases in without overshooting at all, which is a slower bezier by another name. |
| `spring_frequency` | `18` | How fast the spring moves, from `4` to `60`. Higher is snappier. |

```kdl
animations {
    curve "spring"
    spring {
        damping 0.75
        frequency 18
    }
}
```

## Tag Animation Direction

Control the direction of tag switch animations.

| Setting | Default | Description |
| :--- | :--- | :--- |
| `tag_animation_direction` | `1` | Tag animation direction (1: horizontal, 0: vertical) |