---
title: Key Bindings
description: Define keyboard shortcuts and modes.
---

## Syntax

Key bindings live in a `binds` block. Each bind is a keychord node whose child
is the action:

```kdl
binds {
    Modifiers+Key { command parameters...; }
}
```

- **Modifiers**: `SUPER`, `CTRL`, `ALT`, `SHIFT`, `NONE` (combine with `+`, e.g. `SUPER+CTRL+ALT`).
- **Key**: Key name (from `xev` or `wev`) or keycode (e.g., `code:24` for `q`).

> **Info:** `bind` automatically converts keysym to keycode for comparison. This makes it compatible with all keyboard layouts, but the matching may not always be precise. If a key combination doesn't work on your keyboard layout, use a keycode instead (e.g., `code:24` instead of `q`).

### Flags

- `l`: Works even when screen is locked.
- `s`: Uses keysym instead of keycode to bind.
- `r`: Triggers on key release instead of press.
- `p`: Pass key event to client.

**Examples:**

```kdl
binds {
    Super+Q { kill_client; }
    Super+L { spawn swaylock; }
    Alt+code:24 { kill_client; }
    code:64+code:24 { kill_client; }
    code:64+code:133+code:24 { kill_client; }
    NONE+XF86MonBrightnessUp { spawn "brightnessctl set +5%"; }
    alt+shift_l { switch_keyboard_layout; }
}
```

## Key Modes (Submaps)

You can divide key bindings into named modes. Rules:

1. Set `keymode=<name>` before a group of `bind` lines — those binds only apply in that mode.
2. If no `keymode` is set before a bind, it belongs to the `default` mode.
3. The special `common` keymode applies its binds **across all modes**.

Use `set_key_mode` to switch modes, and `amsg get keymode` to query the current mode.

```kdl
misc {
    keymode resize
}

binds {
    Super+r { reload_config; }
    Alt+Return { spawn foot; }
    Super+F { set_key_mode resize; }
    NONE+Left { resize_window -10 0; }
    NONE+Right { resize_window +10 0; }
    NONE+Escape { set_key_mode default; }
}
```

### Single Modifier Key Binding

When binding a modifier key itself, use `NONE` for press and the modifier name for release:

```kdl
binds {
    none+Super_L { spawn "rofi -show run"; }
    Super+Super_L { spawn "rofi -show run"; }
}
```

## Dispatchers List

### Window Management

| Command | Param | Description |
| :--- | :--- | :--- |
| `kill_client` | `force` | Close the focused window. If `force` is specified, sends `SIGKILL`. |
| `toggle_floating` | - | Toggle floating state. |
| `toggle_all_floating` | - | Toggle all visible clients floating state. |
| `toggle_fullscreen` | - | Toggle fullscreen. |
| `toggle_fake_fullscreen` | - | Toggle "fake" fullscreen (remains constrained). |
| `toggle_maximize` | - | Maximize window (keep decoration/bar). |
| `toggle_global` | - | Pin window to all tags. |
| `pin` | - | Toggle pinned state: the window is forced floating, kept on top and stays visible on every tag of its monitor (it does not slide with tag-switch animations). |
| `toggle_render_border` | - | Toggle border rendering. |
| `center_window` | - | Center the floating window. |
| `minimize` | - | Minimize window to scratchpad. |
| `restore_minimized` | - | Restore window from scratchpad. |
| `toggle_scratchpad` | - | Toggle scratchpad. |
| `toggle_named_scratchpad` | `appid,title,cmd` | Toggle named scratchpad. Launches app if not running, otherwise shows/hides it. |
| `toggle_special_workspace` | `name` | Toggle the named [special workspace](/docs/window-management/special-workspaces) `name` on the focused monitor: slides it in on top of the current tag, or slides it back out if it is already showing. Opening one implicitly closes any other special workspace already showing on that monitor. |
| `move_to_special_workspace` | `name` (optional) | Move the focused window into named special workspace `name`. Called with no name (or an empty one), moves the window back out to the normal tag it came from. |

### Focus & Movement

| Command | Param | Description |
| :--- | :--- | :--- |
| `focus_id` | - | Focus window (can target any window via IPC: `amsg dispatch focus_id client,<id>`) |
| `focus_direction` | `left/right/up/down` | Focus window in direction. |
| `focus_stack` | `next/prev` | Cycle focus within the stack. |
| `focus_last` | - | Focus the previously active window. |
| `exchange_client` | `left/right/up/down` | Swap window with neighbor in direction. |
| `exchange_stack_client` | `next/prev` | Exchange window position in stack. |
| `zoom` | - | Swap focused window with Master. |

### Tags & Monitors

| Command | Param | Description |
| :--- | :--- | :--- |
| `view` | `-1/0/1-9` or `mask [,synctag]` | View tag. `-1` = previous tagset, `0` = all tags, `1-9` = specific tag, mask e.g. `1\|3\|5`. Optional `synctag` (0/1) syncs the action to all monitors. |
| `view_to_left` | `[synctag]` | View previous tag. Optional `synctag` (0/1) syncs to all monitors. |
| `view_to_right` | `[synctag]` | View next tag. Optional `synctag` (0/1) syncs to all monitors. |
| `view_to_left_occupied` | `[synctag]` | View left tag and focus client if present. Optional `synctag` (0/1). |
| `view_to_right_occupied` | `[synctag]` | View right tag and focus client if present. Optional `synctag` (0/1). |
| `view_cross_monitor` | `tag,monitor_spec` | View specified tag on specified monitor. |
| `tag` | `1-9 [,synctag]` | Move window to tag. Optional `synctag` (0/1) syncs to all monitors. |
| `tag_silent` | `1-9` | Move window to tag without focusing it. |
| `tag_to_left` | `[synctag]` | Move window to left tag. Optional `synctag` (0/1). |
| `tag_to_right` | `[synctag]` | Move window to right tag. Optional `synctag` (0/1). |
| `tag_cross_monitor` | `tag,monitor_spec` | Move window to specified tag on specified monitor. |
| `toggle_tag` | `0-9` | Toggle tag on window (0 means all tags). |
| `toggle_view` | `1-9` | Toggle tag view. |
| `combo_view` | `1-9` | View multi tags pressed simultaneously. |
| `focus_monitor` | `left/right/up/down/monitor_spec` | Focus monitor by direction or [monitor spec](/docs/configuration/monitors#monitor-spec-format). |
| `tag_monitor` | `left/right/up/down/monitor_spec,[keeptag]` | Move window to monitor by direction or [monitor spec](/docs/configuration/monitors#monitor-spec-format). `keeptag` is 0 or 1. |

### Layouts

| Command | Param | Description |
| :--- | :--- | :--- |
| `set_layout` | `name` | Switch to layout (e.g., `scroller`, `tile`). |
| `switch_layout` | - | Cycle through available layouts. |
| `adjust_master_count` | `+1/-1` | Increase/Decrease number of master windows. |
| `set_master_factor` | `+0.05` | Increase/Decrease master area size. |
| `set_proportion` | `float` | Set scroller window proportion (0.0–1.0). |
| `switch_proportion_preset` | `[next/prev]` | Cycle proportion presets of scroller window forwards or backwards (default `next`). |
| `scroller_stack` | `left/right/up/down` | Move window inside/outside scroller stack by direction. |
| `scroller_consume` | - | Pull the next column's window into the focused window's scroller stack. No-op for pinned or grouped windows. |
| `scroller_expel` | - | Pop the focused window out of its scroller stack into its own column placed after the current one. No-op for pinned or grouped windows. |
| `adjust_gaps` | `+/-value` | Adjust gap size. |
| `toggle_gaps` | - | Toggle gaps. |
|  `dwindle_toggle_split_direction` | - | Toggle split direction in dwindle layout. |
| `dwindle_split_horizontal` | - | Set split window direction to horizontal in dwindle layout. |
| `dwindle_split_vertical` | - | Set split window direction to vertical in dwindle layout. |

Suggested scroller binds (not bound by default — uncomment to use):

```ini
# bind=SUPER,r,switch_proportion_preset,next
# bind=SUPER+SHIFT,r,switch_proportion_preset,prev
# bind=SUPER,i,scroller_consume
# bind=SUPER,o,scroller_expel
```

### System

| Command | Param | Description |
| :--- | :--- | :--- |
| `spawn` | `cmd` | Execute a command. |
| `spawn_shell` | `cmd` | Execute shell command (supports pipes `\|`). |
| `spawn_on_empty` | `cmd,tagnumber` | Open command on empty tag. |
| `reload_config` | - | Hot-reload configuration. |
| `quit` | - | Exit asteroidz. |
| `restart` | - | Restart asteroidz in place (re-exec, keeps the login session; running clients are restarted). |
| `toggle_overview` | - | Toggle overview mode. |
| `toggle_overview` | `jump` | Open the overview in jump mode: every window gets a letter hint; pressing it focuses that window. |
| `create_virtual_output` | - | Create a headless monitor (for VNC/Sunshine). |
| `destroy_all_virtual_output` | - | Destroy all virtual monitors. |
| `toggle_overlay` | - | Toggle overlay state for the focused window. |
| `toggle_trackpad_enable` | - | Toggle trackpad enable. |
| `set_key_mode` | `mode` | Set keymode. |
| `switch_keyboard_layout` | `[index]` | Switch keyboard layout. Optional index (0, 1, 2...) to switch to specific layout. |
| `set_option` | `key,value` | Set config option temporarily. |
| `dpms_off_monitor` | `monitor_spec` | Power off monitor without removing it from the layout. |
| `dpms_on_monitor` | `monitor_spec` | Power monitor back on. |
| `dpms_toggle_monitor` | `monitor_spec` | Toggle monitor power without removing it. |
| `disable_monitor` | `monitor_spec` | Shutdown monitor. Accepts a [monitor spec](/docs/configuration/monitors#monitor-spec-format). |
| `enable_monitor` | `monitor_spec` | Power on monitor. Accepts a [monitor spec](/docs/configuration/monitors#monitor-spec-format). |
| `toggle_monitor` | `monitor_spec` | Toggle monitor power. Accepts a [monitor spec](/docs/configuration/monitors#monitor-spec-format). |
| `chvt` | `1-9` | Change virtual terminal (tty, equivalent to using ctrl+alt+Fkeys) |
| `screenshot_ui` | `[screen/region/window]` | Compositor-native screenshot UI (defaults to `region`). Freezes the focused output and shows it full-screen while you pick what to capture; saves to `~/Pictures/Screenshots/screenshot_<timestamp>.png` and copies it to the clipboard (requires `wl-copy`). `region` lets you drag a selection rectangle (Escape cancels, release confirms); `window` captures whatever window you click; `screen` captures the whole focused output immediately, with no interaction. |

```kdl
binds {
    Super+S { screenshot_ui region; }
    SUPER SHIFT+S { screenshot_ui window; }
    SUPER CTRL+S { screenshot_ui screen; }
}
```

### Cursor Zoom

Output-level magnifier centered on the cursor, similar to Hyprland's `zoom_factor`. The zoom factor is runtime-only state (not saved to config); see [`cursor_zoom_rigid` and `cursor_zoom_step`](/docs/configuration/miscellaneous) for the related config options.

| Command | Param | Description |
| :--- | :--- | :--- |
| `zoom_in` | `[step]` | Increase cursor zoom. Defaults to `cursor_zoom_step`. |
| `zoom_out` | `[step]` | Decrease cursor zoom. Defaults to `cursor_zoom_step`. |
| `zoom_reset` | - | Reset cursor zoom to 1.0 (off). |

```kdl
misc {
    axisbind SUPER,DOWN,zoom_out
}

binds {
    Super+Equal { zoom_in; }
    Super+Minus { zoom_out; }
    Super+0 { zoom_reset; }
}
```

### Media Controls

> **Warning:** Some keyboards don't send standard media keys. Run `wev` and press your key to check the exact key name.

#### Brightness

Requires: `brightnessctl`

```kdl
binds {
    NONE+XF86MonBrightnessUp { spawn "brightnessctl s +2%"; }
    Shift+XF86MonBrightnessUp { spawn "brightnessctl s 100%"; }
    NONE+XF86MonBrightnessDown { spawn "brightnessctl s 2%-"; }
    Shift+XF86MonBrightnessDown { spawn "brightnessctl s 1%"; }
}
```

#### Volume

Requires: `wpctl` (WirePlumber)

```kdl
binds {
    NONE+XF86AudioRaiseVolume { spawn "wpctl set-volume @DEFAULT_SINK@ 5%+"; }
    NONE+XF86AudioLowerVolume { spawn "wpctl set-volume @DEFAULT_SINK@ 5%-"; }
    NONE+XF86AudioMute { spawn "wpctl set-mute @DEFAULT_SINK@ toggle"; }
    Shift+XF86AudioMute { spawn "wpctl set-mute @DEFAULT_SOURCE@ toggle"; }
}
```

#### Playback

Requires: `playerctl`

```kdl
binds {
    NONE+XF86AudioNext { spawn "playerctl next"; }
    NONE+XF86AudioPrev { spawn "playerctl previous"; }
    NONE+XF86AudioPlay { spawn "playerctl play-pause"; }
}
```

### Floating Window Movement

| Command | Param | Description |
| :--- | :--- | :--- |
| `smart_move_window` | `left/right/up/down` | Move floating window by snap distance. |
| `smart_resize_window` | `left/right/up/down` | Resize floating window by snap distance. |
| `move_window` | `(x,y)` | Move floating window. |
| `resize_window` | `(width,height)` | Resize window. |
