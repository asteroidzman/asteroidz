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

```ini
# Background color of the root window
rootcolor=0x323232ff

# Inactive window border
bordercolor=0x444444ff

# Drop shadow when dragging windows
dropcolor=0x8FBA7C55

# Split window border color in manual dwindle layout
splitcolor=0xEB441EFF

# Active window border
focuscolor=0xc66b25ff

# Urgent window border (alerts)
urgentcolor=0xad401fff
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

### Pill Theme

Every native "pill" overlay — the monocle layout's tab bar, window-group bars
(`groupjoin`), overview jump-mode letter labels, and the `screenshot_ui` size
badge and selection border — shares this single style. There's no separate
theming for any of them; changing a `pill_decorate_*` key restyles all of
them at once.

Dimensions and behavior specific to the monocle layout's own tab strip (not
shared with the other pill overlays) are still their own keys:

| Setting | Default | Description |
| :--- | :--- | :--- |
| `monocle_tab_height` | `50` | Height of the tab bar for monocle layout. |
| `monocle_tab_max_width` | `0` | Cap each tab's width; `0` lets tabs split the full row width. |
| `monocle_tab_icons` | `0` | Draw app icons in monocle tab pills. |

| Setting | Default | Description |
| :--- | :--- | :--- |
| `pill_decorate_fg_color` | `0xc4939dff` | text color.
| `pill_decorate_bg_color` | `0x201b14ff` | background color.|
| `pill_decorate_focus_fg_color` | `0x201b14ff` | text color for focus. |
| `pill_decorate_focus_bg_color` | `0xc4939dff` | background color for focus.|
| `pill_decorate_border_color` | `0x8BAA9Bff` | border color.|
| `pill_decorate_border_width` | `4` | border width.|
| `pill_decorate_corner_radius` | `5` | corner radius.|
| `pill_decorate_padding_x` | `0` | horizontal padding.|
| `pill_decorate_padding_y` | `0` | vertical padding.|
| `pill_decorate_font_desc` | `monospace Bold 16` | font set.|

## Borders

Control the appearance of window borders.

## Cursor Theme

Set the size and theme of your mouse cursor.

```ini
cursor_size=24
cursor_theme=Adwaita
```
