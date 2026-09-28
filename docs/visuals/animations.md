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

### `asteroid` and `shatter` — breaking glass

Both names select the glass close effect. `asteroid` remains the default.
The window cracks outward from an impact point into irregular triangular
shards. The pieces keep the window's image, separate briefly along the cracks,
then tumble, catch small glints and fall under gravity. They fade during the
latter part of the animation.

```kdl
animations {
    window-close {
        type asteroid       // shatter selects the same effect
        duration 350
        shatter-fragments 6 // fracture density (2–12, default 6)
    }
}
```

`shatter-fragments` controls the density of radial and crosswise cracks, not a
square tile grid. The default makes 72 shards; the range makes 8–336. The same
setting applies to both names. Motion is evaluated from elapsed time, so it
does not speed up with refresh rate or with the number of monitors.

Fragments sample the existing window buffer through Vulkan; no per-frame
snapshot copies or CPU rasterization are needed. Debris disappears when it
crosses into a neighboring monitor or leaves the visible desktop.

### `fall` — a grid of tiles

`fall` keeps the rectangular tile effect: pieces move outward without rotating.
Its column and row settings are independent of the glass effect.

```kdl
animations {
    window-close {
        type fall
        duration 250
        fall-columns 4   // tiles across (1–12, default 4)
        fall-rows 3      // tiles down (1–12, default 3)
    }
}
```

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

**`duration` is an upper bound a spring may never reach.** The spring is
evaluated over *normalised* time, so `frequency` says how many radians it
travels per configured duration, not per second — a higher frequency converges
sooner and stops, leaving the rest of the duration unused. Measured at
`frequency 10`, every move configured for 500ms finished in about 202ms; at
`frequency 22`, in 23% of its duration. If a spring animation feels shorter
than you asked for, the lever is `frequency`, not `duration`: roughly 4–5 keeps
it in motion for most of the configured time.

## Tag Animation Direction

Control the direction of tag switch animations.

| Setting | Default | Description |
| :--- | :--- | :--- |
| `tag_animation_direction` | `1` | Tag animation direction (1: horizontal, 0: vertical) |