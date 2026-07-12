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

You can also color-code windows based on their state:

| State | Config Key | Default Color |
| :--- | :--- | :--- |
| Maximized | `maximizescreencolor` | `0x89aa61ff` |
| Scratchpad | `scratchpadcolor` | `0x516c93ff` |
| Global | `globalcolor` | `0xb153a7ff` |
| Overlay | `overlaycolor` | `0x14a57cff` |

> **Tip:** For scratchpad window sizing, see [Scratchpad](/docs/window-management/scratchpad) configuration.

### Theme

Every native UI overlay — the monocle layout's tab bar, per-window titlebars,
overview jump-mode letter labels, and the
`screenshot_ui` size badge and selection border — shares this single style,
set in the `theme { ... }` block. There's no separate theming for any of
them; changing a `theme_*` key restyles all of them at once.

> **Deprecated:** this block was previously spelled `pill { ... }` (keys
> `pill_decorate_*`), after the DMS pill widgets it started with. The old
> spelling still works as an alias and logs a one-time notice.

Dimensions and behavior specific to the monocle layout's own tab strip (not
shared with the other native overlays) are still their own keys:

| Setting | Default | Description |
| :--- | :--- | :--- |
| `monocle_tab_max_width` | `0` | Cap each tab's width; `0` lets tabs split the full row width. |

### Titlebar

An optional server-side titlebar, reserving real space above each tiled window's content (the window doesn't grow to compensate — enabling this shrinks the usable content area by `titlebar_height`). Off by default. Uses the same shared `theme` block as everything else in this section. Drag the title area to move/re-tile the window; click the "×" to close it.

| Setting | Default | Description |
| :--- | :--- | :--- |
| `enable_titlebar` | `0` | Show a titlebar on tiled windows (1 = enable). |
| `titlebar_height` | `28` | Titlebar height in pixels. |

```kdl
layout {
    titlebar {
        enable 1
        height 28
    }
}
```

| Setting | Default | Description |
| :--- | :--- | :--- |
| `theme_fg_color` | `0xc4939dff` | text color. |
| `theme_bg_color` | `0x201b14ff` | background color. |
| `theme_focus_fg_color` | `0x201b14ff` | text color for focus. |
| `theme_focus_bg_color` | `0xc4939dff` | background color for focus. |
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

Control the appearance of window borders.

## Cursor Theme

Set the size and theme of your mouse cursor.

```kdl
input {
    cursor {
        size 24
        theme Adwaita
    }
}
```
