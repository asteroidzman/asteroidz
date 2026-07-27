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

Available types: `slide`, `zoom`, `fade`, `none`, plus `asteroid` for closing
windows.

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

`asteroid` breaks the closing window into a grid of tiles that fly straight
out from its centre and fade — debris leaving a rock in the arcade game this
compositor is named after. It is the default for `window-close`; set another
type to opt out.

```kdl
animations {
    window-close {
        type asteroid
        duration 250
        fall-columns 4   // tiles across (1–12, default 4)
        fall-rows 3      // tiles down  (1–12, default 3)
    }
}
```

`fall` still selects it: that was the name when the pieces dropped under
gravity instead of flying outward, and an existing config should not break
over a rename. The grid keys keep their old spelling for the same reason.

What makes the arcade version read the way it does is what it lacks — no
gravity, no arc, no settling. Each tile's direction is the line from the
window's centre through the tile's own centre, normalised so that speed comes
from jitter rather than from how far out a tile happened to start (otherwise
corner pieces leave twice as fast as edge ones and the cloud comes out
diamond-shaped). Distance eases out, putting most of the travel in the first
third, which is what sells a burst at a duration short enough to stay out of
the way.

The tiles are ordinary scene nodes, so this costs no more than the other close
animations and renders identically on both the GLES and Vulkan renderers. The
pieces stay axis-aligned rather than tumbling: a scene buffer cannot be
rotated without renderer-level support. A small perpendicular kick per tile
stands in for it, so the cloud shears as it expands instead of moving like a
rigid diagram.

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