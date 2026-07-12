---
title: Layouts
description: Configure and switch between different window layouts.
---

## Supported Layouts

asteroidz keeps a deliberately small set of layouts, assignable per tag:

- `tile` — a manual-control binary-tree tiler (i3-like: you choose where the next window splits)
- `scroller` — a scrollable strip of columns, similar to PaperWM
- `monocle` — one window fullscreen-ish at a time, with a tab bar for the rest

Fullscreen is a per-window state (`toggle_fullscreen`), independent of layout, and works in any of them.

---

## Scroller Layout

The Scroller layout positions windows in a scrollable strip, similar to PaperWM.

### Configuration

| Setting | Default | Description |
| :--- | :--- | :--- |
| `scroller_structs` | `20` | Width reserved on sides when window ratio is 1. |
| `scroller_default_proportion` | `0.9` | Default width proportion for new windows. |
| `scroller_focus_center` | `0` | Center the focused window: `0` = never, `1` = always, `2` = only on overflow (center only when the focused column cannot fit fully on screen next to the previously focused column). |
| `scroller_prefer_center` | `0` | Center focused window only if it was outside the view. |
| `scroller_center_single` | `0` | Center the column when it is the only one visible on the tag, regardless of its proportion (1 = enable). |
| `scroller_prefer_overspread` | `1` | Allow windows to overspread when there's extra space. |
| `edge_scroller_pointer_focus` | `1` | Focus windows even if partially off-screen. |
| `edge_scroller_focus_allow_speed` | `0.0` | Allow pointer focus to happen if the pointer moves at a speed greater than this value. |
| `scroller_proportion_preset` | `0.5,0.8,1.0` | Presets for cycling window widths. |
| `scroller_ignore_proportion_single` | `1` | Ignore proportion adjustments for single windows. |
| `scroller_default_proportion_single` | `1.0` | Default proportion for single windows in scroller. **Requires `scroller_ignore_proportion_single=0` to take effect.** |
| `scroller_edge_scroll` | `0` | Advance to the next/prev column by hovering the pointer against a screen edge (left/right), instead of only via keybinds (1 = enable). |
| `scroller_edge_scroll_size` | `15` | Width in pixels of the hot edge zone that triggers `scroller_edge_scroll`. |
| `scroller_edge_scroll_delay` | `500` | Milliseconds the pointer must dwell at the edge before advancing; repeats at the same interval while it stays there. |

> **Warning:** `scroller_prefer_overspread`, `scroller_focus_center`, and `scroller_prefer_center` interact with each other. Their priority order is:
>
> **scroller_prefer_overspread > scroller_focus_center > scroller_prefer_center**
>
> To ensure a lower-priority setting takes effect, you must set all higher-priority options to `0`.

```kdl
layout {
    scroller {
        structs 20
        default-proportion 0.9
        focus-center 0
        prefer-center 0
        default-proportion-single 1.0
        preset 0.5,0.8,1.0
        edge-scroll {
            pointer-focus 1
            allow-speed 0.0
            enable 0
            size 15
            delay 500
        }
    }
}
misc {
    scroller_center_single 0
    scroller_prefer_overspread 1
}
```

---

## Tile Layout

`tile` arranges windows as a binary tree of recursive splits (internally "dwindle"). Each new window splits the focused window's container. By default the split axis is chosen automatically; with `dwindle_manual_split=1` it becomes an i3-like manual tiler instead — you explicitly set the split direction for the focused container, and it sticks for any window opened there until you change it again.

### Configuration

| Setting | Default | Description |
| :--- | :--- | :--- |
| `dwindle_split_ratio` | `0.5` | Ratio used for new splits (`0.05`–`0.95`). |
| `dwindle_smart_split` | `0` | Pick the split axis from the cursor's position inside the focused window. The new window appears on the cursor's side. |
| `dwindle_hsplit` | `1` | Side-by-side splits: where the new window goes. `0` = follow cursor, `1` = right, `2` = left. |
| `dwindle_vsplit` | `1` | Top/bottom splits: where the new window goes. `0` = follow cursor, `1` = below, `2` = above. |
| `dwindle_preserve_split` | `0` | Keep the sibling's split orientation when a window is closed. |
| `dwindle_smart_resize` | `0` | When dragging to resize, move the split toward the cursor regardless of which side was grabbed. |
| `dwindle_drop_simple_split` | `1` | Drag-to-tile drop preview. `1` = 2-zone preview matching `dwindle_split_ratio`, `0` = 4-quadrant preview. |
| `dwindle_manual_split` | `0` | i3-like manual split mode: the focused container's split direction is set explicitly (see below) rather than auto-picked, and persists for future windows opened there. |

```kdl
misc {
    dwindle_split_ratio 0.5
    dwindle_smart_split 0
    dwindle_hsplit 0
    dwindle_vsplit 0
    dwindle_preserve_split 0
    dwindle_smart_resize 0
    dwindle_drop_simple_split 1
}
```

### Manual split direction (i3-like)

With `dwindle_manual_split=1`, bind these dispatches to control where the *next* window opened on the focused container will go — matching i3's `split h`/`split v`:

| Command | Description |
| :--- | :--- |
| `dwindle_split_horizontal` | Set the focused container to split left/right. |
| `dwindle_split_vertical` | Set the focused container to split top/bottom. |
| `dwindle_toggle_split_direction` | Toggle the focused container's split direction. |

```kdl
misc {
    dwindle_manual_split 1
}

binds {
    Super+b { dwindle_split_horizontal; }
    Super+Shift+b { dwindle_split_vertical; }
}
```

---

## Switching Layouts
| Setting | Default | Description |
| :--- | :--- | :--- |
| `circle_layout` | - | A comma-separated list of layouts `switch_layout` cycles through,the value sample:`tile,scroller`. |

You can switch layouts dynamically or set a default for specific tags using [Tag Rules](/docs/window-management/rules#tag-rules).

**Keybinding Examples:**

```kdl
misc {
    circle_layout monocle,scroller,tile
}

binds {
    Super+n { switch_layout; }
    Super+t { set_layout tile; }
    Super+s { set_layout scroller; }
}
```
