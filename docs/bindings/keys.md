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
    Super+Q { killclient; }
    Super+L { spawn swaylock; }
    Alt+code:24 { killclient; }
    code:64+code:24 { killclient; }
    code:64+code:133+code:24 { killclient; }
    NONE+XF86MonBrightnessUp { spawn "brightnessctl set +5%"; }
    alt+shift_l { switch_keyboard_layout; }
}
```

## Key Modes (Submaps)

You can divide key bindings into named modes. Rules:

1. Set `keymode=<name>` before a group of `bind` lines — those binds only apply in that mode.
2. If no `keymode` is set before a bind, it belongs to the `default` mode.
3. The special `common` keymode applies its binds **across all modes**.

Use `setkeymode` to switch modes, and `amsg get keymode` to query the current mode.

```kdl
misc {
    keymode resize
}

binds {
    Super+r { reload_config; }
    Alt+Return { spawn foot; }
    Super+F { setkeymode resize; }
    NONE+Left { resizewin -10 0; }
    NONE+Right { resizewin +10 0; }
    NONE+Escape { setkeymode default; }
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
| `killclient` | `force` | Close the focused window. If `force` is specified, sends `SIGKILL`. |
| `togglefloating` | - | Toggle floating state. |
| `toggle_all_floating` | - | Toggle all visible clients floating state. |
| `togglefullscreen` | - | Toggle fullscreen. |
| `togglefakefullscreen` | - | Toggle "fake" fullscreen (remains constrained). |
| `togglemaximizescreen` | - | Maximize window (keep decoration/bar). |
| `toggleglobal` | - | Pin window to all tags. |
| `pin` | - | Toggle pinned state: the window is forced floating, kept on top and stays visible on every tag of its monitor (it does not slide with tag-switch animations). |
| `toggle_render_border` | - | Toggle border rendering. |
| `centerwin` | - | Center the floating window. |
| `minimized` | - | Minimize window to scratchpad. |
| `restore_minimized` | - | Restore window from scratchpad. |
| `toggle_scratchpad` | - | Toggle scratchpad. |
| `toggle_named_scratchpad` | `appid,title,cmd` | Toggle named scratchpad. Launches app if not running, otherwise shows/hides it. |
| `togglespecialworkspace` | `name` | Toggle the named [special workspace](/docs/window-management/special-workspaces) `name` on the focused monitor: slides it in on top of the current tag, or slides it back out if it is already showing. Opening one implicitly closes any other special workspace already showing on that monitor. |
| `movetospecialworkspace` | `name` (optional) | Move the focused window into named special workspace `name`. Called with no name (or an empty one), moves the window back out to the normal tag it came from. |

### Focus & Movement

| Command | Param | Description |
| :--- | :--- | :--- |
| `focusid` | - | Focus window (can target any window via IPC: `amsg dispatch focusid client,<id>`) |
| `focusdir` | `left/right/up/down` | Focus window in direction. |
| `focusstack` | `next/prev` | Cycle focus within the stack. |
| `focuslast` | - | Focus the previously active window. |
| `exchange_client` | `left/right/up/down` | Swap window with neighbor in direction. |
| `exchange_stack_client` | `next/prev` | Exchange window position in stack. |
| `zoom` | - | Swap focused window with Master. |

### Window Groups

| Command | Param | Description |
| :--- | :--- | :--- |
| `groupjoin` | `left/right/up/down` | Join the group of (or form a group with) the window in the given direction. |
| `groupleave` | - | Leave the current group. |
| `groupfocus` | `next/prev` | Focus the next/previous member of the current group. |
| `grouplock` | - | Toggle the group lock. A locked group refuses new members and keeps its current ones (`groupjoin` into or out of it is denied). |
| `movegroupwindow` | `next/prev` | Swap the focused window with its next/previous neighbor inside the group bar (no wraparound). |

> **Tip:** Scrolling the mouse wheel over a group bar also cycles through the group members. Windows matched by the `deny_group` window rule can never be pulled into a group.

### Tags & Monitors

| Command | Param | Description |
| :--- | :--- | :--- |
| `view` | `-1/0/1-9` or `mask [,synctag]` | View tag. `-1` = previous tagset, `0` = all tags, `1-9` = specific tag, mask e.g. `1\|3\|5`. Optional `synctag` (0/1) syncs the action to all monitors. |
| `viewtoleft` | `[synctag]` | View previous tag. Optional `synctag` (0/1) syncs to all monitors. |
| `viewtoright` | `[synctag]` | View next tag. Optional `synctag` (0/1) syncs to all monitors. |
| `viewtoleft_have_client` | `[synctag]` | View left tag and focus client if present. Optional `synctag` (0/1). |
| `viewtoright_have_client` | `[synctag]` | View right tag and focus client if present. Optional `synctag` (0/1). |
| `viewcrossmon` | `tag,monitor_spec` | View specified tag on specified monitor. |
| `tag` | `1-9 [,synctag]` | Move window to tag. Optional `synctag` (0/1) syncs to all monitors. |
| `tagsilent` | `1-9` | Move window to tag without focusing it. |
| `tagtoleft` | `[synctag]` | Move window to left tag. Optional `synctag` (0/1). |
| `tagtoright` | `[synctag]` | Move window to right tag. Optional `synctag` (0/1). |
| `tagcrossmon` | `tag,monitor_spec` | Move window to specified tag on specified monitor. |
| `toggletag` | `0-9` | Toggle tag on window (0 means all tags). |
| `toggleview` | `1-9` | Toggle tag view. |
| `comboview` | `1-9` | View multi tags pressed simultaneously. |
| `focusmon` | `left/right/up/down/monitor_spec` | Focus monitor by direction or [monitor spec](/docs/configuration/monitors#monitor-spec-format). |
| `tagmon` | `left/right/up/down/monitor_spec,[keeptag]` | Move window to monitor by direction or [monitor spec](/docs/configuration/monitors#monitor-spec-format). `keeptag` is 0 or 1. |

### Layouts

| Command | Param | Description |
| :--- | :--- | :--- |
| `setlayout` | `name` | Switch to layout (e.g., `scroller`, `tile`). |
| `switch_layout` | - | Cycle through available layouts. |
| `incnmaster` | `+1/-1` | Increase/Decrease number of master windows. |
| `setmfact` | `+0.05` | Increase/Decrease master area size. |
| `set_proportion` | `float` | Set scroller window proportion (0.0–1.0). |
| `switch_proportion_preset` | `[next/prev]` | Cycle proportion presets of scroller window forwards or backwards (default `next`). |
| `scroller_stack` | `left/right/up/down` | Move window inside/outside scroller stack by direction. |
| `scroller_consume` | - | Pull the next column's window into the focused window's scroller stack. No-op for pinned or grouped windows. |
| `scroller_expel` | - | Pop the focused window out of its scroller stack into its own column placed after the current one. No-op for pinned or grouped windows. |
| `incgaps` | `+/-value` | Adjust gap size. |
| `togglegaps` | - | Toggle gaps. |
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
| `toggleoverview` | - | Toggle overview mode. |
| `togglejump` | - | Toggle overview with jump mode. |
| `create_virtual_output` | - | Create a headless monitor (for VNC/Sunshine). |
| `destroy_all_virtual_output` | - | Destroy all virtual monitors. |
| `toggleoverlay` | - | Toggle overlay state for the focused window. |
| `toggle_trackpad_enable` | - | Toggle trackpad enable. |
| `setkeymode` | `mode` | Set keymode. |
| `switch_keyboard_layout` | `[index]` | Switch keyboard layout. Optional index (0, 1, 2...) to switch to specific layout. |
| `setoption` | `key,value` | Set config option temporarily. |
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
| `smartmovewin` | `left/right/up/down` | Move floating window by snap distance. |
| `smartresizewin` | `left/right/up/down` | Resize floating window by snap distance. |
| `movewin` | `(x,y)` | Move floating window. |
| `resizewin` | `(width,height)` | Resize window. |
