---
title: Theming
description: Customize the visual appearance of borders, colors, and the cursor.
---

## Dimensions

Control the sizing of window borders and gaps.

| Setting | Default | Description |
| :--- | :--- | :--- |
| `borderpx` | `4` | Border width in pixels. |
| `gappih` | `5` | Horizontal inner gap (between windows). |
| `gappiv` | `5` | Vertical inner gap. |
| `gappoh` | `10` | Horizontal outer gap (between windows and screen edges). |
| `gappov` | `10` | Vertical outer gap. |
| `smartgaps` | `0` | Drop every gap, inner and outer, when a tag holds only one window, so a lone window uses the whole screen. |

## Colors

Colors are defined in `0xRRGGBBAA` hex format.

```kdl
misc {
    rootcolor 0x323232ff
    dropcolor 0x8FBA7C55
    splitcolor 0xEB441EFF
}
layout {
    border {
        color 0x444444ff
        focus-color 0xc66b25ff
        urgent-color 0xad401fff
    }
}
```

### State-Specific Colors

The focused window's border can carry a different colour while it is in a
particular state:

| State | Config Key |
| :--- | :--- |
| Maximized | `layout/border/maximize-color` |
| Scratchpad | `layout/border/scratchpad-color` |
| Global | `layout/border/global-color` |
| Overlay | `layout/border/overlay-color` |

```kdl
layout {
    border {
        focus-color 0xffb86fff
        maximize-color 0x89aa61ff
    }
}
```

**All four default to `focus-color`**, and that is deliberate. They apply only
to a window that is already focused, so they are a variant of the focused
colour rather than a colour in their own right — and a state nobody has
assigned a colour to should not invent one. A generated palette (matugen, say)
sets `color`, `focus-color` and `urgent-color`; if these four defaulted to
fixed hues, a carefully themed border would turn green the moment you maximized
the window, with nothing in the config to point at.

> These keys are new. They previously existed only as internal names
> (`maximizescreencolor` and friends) with no KDL spelling at all, which made
> their hardcoded defaults unreachable — the colours were documented here but
> could not actually be set. If you tried the old names and nothing happened,
> that is why.

> **Tip:** For scratchpad window sizing, see [Scratchpad](/docs/window-management/scratchpad) configuration.

### Theme

Every native UI overlay — the monocle layout's tab bar, per-window titlebars,
overview jump-mode letter labels, and the
`screenshot_ui` size badge and selection border — shares this single style,
set in the `theme { ... }` block. There's no separate theming for any of
them; changing a `theme_*` key restyles all of them at once.

Dimensions and behavior specific to the monocle layout's own tab strip (not
shared with the other native overlays) are still their own keys:

| Setting | Default | Description |
| :--- | :--- | :--- |
| `monocle_tab_max_width` | `0` | Cap each tab's width; `0` lets tabs split the full row width. |

### Titlebar

An optional server-side titlebar, reserving real space above each tiled window's content (the window doesn't grow to compensate — enabling this shrinks the usable content area by the titlebar's height). Off by default. Uses the same shared `theme` block as everything else in this section. Drag the title area to move/re-tile the window; click the "×" to close it.

**Its height is not a setting.** It is the line height of `theme { font }` plus `theme { padding { y } }` above and below — so the bar is always exactly as tall as the text it holds, and changing the theme font resizes it to match instead of putting bigger text into a box that stayed where it was. `theme { font "Ubuntu 17"; padding { y 4 } }` gives a 35px titlebar; `"Ubuntu 10"` gives 24px.

Sizes here are logical pixels at 96 dpi, like everything else in the config: a display running at `scale 1.75` gets a titlebar 1.75× larger in real pixels, without a number changing anywhere. The text in it is *rasterised* at that scale too, not drawn small and scaled up, so it is as sharp as the panel allows.

| Setting | Default | Description |
| :--- | :--- | :--- |
| `enable_titlebar` | `0` | Show a titlebar on tiled windows (1 = enable). |

```kdl
layout {
    titlebar {
        enable 1
    }
}
```

| Setting | Default | Description |
| :--- | :--- | :--- |
| `theme_fg_color` | `0xc4939dff` | text color. |
| `theme_bg_color` | `0x323232ff` | background color. |
| `theme_focus_fg_color` | `0xeda6b4ff` | text color for focus. |
| `theme_focus_bg_color` | `0x4e453cff` | background color for focus. |
| `theme_urgent_color` | `0xffb4abff` | attention accent (matugen error). |
| `theme_border_color` | `0x8BAA9Bff` | border color. |
| `theme_border_width` | `4` | border width. |
| `theme_corner_radius` | `5` | corner radius (`-1` = full pill shape). |
| `theme_padding_x` | `0` | horizontal padding. |
| `theme_padding_y` | `0` | vertical padding. |
| `theme_font_desc` | `monospace Bold 16` | font set. |

```kdl
theme {
    font "Ubuntu 18"
    corner-radius 8
    border-width 0
    padding { x 16; y 4 }
    bg-color 0x252a33ff
    fg-color 0xdee2efff
    focus-bg-color 0xa6c8ffff
    focus-fg-color 0x00315fff
    urgent-color 0xffb4abff
}
```

## Borders

Width, and an optional two-tone gradient across the focused border. The
colours themselves are above, under [Colors](#colors).

```kdl
layout {
    border {
        width 2
        gradient { enable; angle 45; color2 0x88ceffff }
    }
}
```

`gradient` takes `enable`, an `angle` in degrees, and `color2`. In the flat form
these are `border_gradient`, `border_gradient_angle` and
`border_gradient_color2`.

`color2` is the far end of the ramp; the near end is `focus-color`,
so a gradient follows the theme without a second colour having to be kept in
step with it. Only the focused border is drawn as a gradient — an unfocused
one is flat `color`, which is what keeps the focused window obvious in a row
of tiled ones.

## Cursor Theme

Set the `size` and `theme` of your mouse cursor — `cursor_size` and
`cursor_theme` in the flat form. Size is in pixels and clamps to 4–512; the
theme is an installed cursor theme's name.

```kdl
input {
    cursor {
        size 24
        theme Adwaita
    }
}
```
