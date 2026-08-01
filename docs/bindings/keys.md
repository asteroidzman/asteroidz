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

Four modifiers on how a bind fires, written as **properties on the chord**:

| Property | Effect |
| :--- | :--- |
| `lock=#true` | Still fires while the screen is locked. |
| `keysym=#true` | Match by keysym rather than by keycode. |
| `release=#true` | Fire on release instead of press. |
| `pass=#true` | Also pass the key through to the focused client. |

```kdl
binds {
    Super+L lock=#true { spawn swaylock; }
    Super+Shift+P release=#true { spawn "grim -g \"$(slurp)\""; }
    Super+Tab pass=#true { focus_stack next; }
}
```

Absent is off, and `#false` is off too — so a generated config can say "not this
one" without deleting the node. Both KDL boolean spellings work: `#true` (KDL v2)
and bare `true` (v1).

Flags apply to **keyboard binds only**. `mousebind` has nowhere to put them, and
a mouse chord carrying one logs a warning rather than dropping it silently.

> These were reachable only from the legacy `bindlr=` line format until
> 0.20.9 — `kdl_binds` always passed the bare `bind`, so a `binds` block could
> not express a release binding at all and the flag was quietly discarded.
> `amsg get binds` reports the four per bind, which is how that is now pinned.

**Examples:**

```kdl
binds {
    Super+Q { kill_client; }
    Super+L { spawn swaylock; }
    Alt+code:24 { kill_client; }
    NONE+XF86MonBrightnessUp { spawn "brightnessctl set +5%"; }
    alt+shift_l { switch_keyboard_layout; }
}
```

`code:<n>` names a raw keycode and can be used for **the key**, not for a
modifier: `parse_mod` only understands modifier names, so `code:64+code:24` is
rejected with "Unknown modifier". Modifiers are joined with `+` and nothing else —
`SUPER SHIFT+S` is not two modifiers, it is a node called `SUPER` with an argument,
because a space separates nodes from their arguments in KDL.

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
| `view` | `-1/0/1-9` or `mask` | View tag on the focused monitor only. `-1` = previous tagset, `0` = all tags, `1-9` = specific tag, mask e.g. `1\|3\|5`. Tags are strictly per-monitor: this never affects any other monitor's tagset. |
| `view_to_left` | - | View previous tag on the focused monitor only. |
| `view_to_right` | - | View next tag on the focused monitor only. |
| `view_to_left_occupied` | - | View left tag and focus client if present (focused monitor only). |
| `view_to_right_occupied` | - | View right tag and focus client if present (focused monitor only). |
| `view_cross_monitor` | `tag,monitor_spec` | View specified tag on specified monitor. |
| `tag` | `1-9` | Move window to tag (on its own monitor only). |
| `tag_silent` | `1-9` | Move window to tag without focusing it. |
| `tag_to_left` | - | Move window to left tag. |
| `tag_to_right` | - | Move window to right tag. |
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
| `quit` | - | Exit asteroidz, after a confirmation prompt. |
| `restart` | - | Restart asteroidz in place (re-exec, keeps the login session; running clients are restarted). |
| `toggle_overview` | - | Toggle overview mode. |
| `toggle_overview` | `jump` | Open the overview in jump mode: every window gets a letter hint; pressing it focuses that window. |
| `create_virtual_output` | - | Create a headless monitor (for VNC/Sunshine). |
| `destroy_all_virtual_output` | - | Destroy all virtual monitors. |
| `toggle_overlay` | - | Toggle overlay state for the focused window. |
| `toggle_trackpad_enable` | - | Toggle trackpad enable. |
| `set_key_mode` | `mode` | Set keymode. |
| `switch_keyboard_layout` | `[index]` | Switch keyboard layout. Optional index (0, 1, 2...) to switch to specific layout. |
| `set_option` | `key,value` | Set config option temporarily — in memory only; see the note below. |
| `dpms_off_monitor` | `monitor_spec` | Power off monitor without removing it from the layout. |
| `dpms_on_monitor` | `monitor_spec` | Power monitor back on. |
| `dpms_toggle_monitor` | `monitor_spec` | Toggle monitor power without removing it. |
| `disable_monitor` | `monitor_spec` | Shutdown monitor. Accepts a [monitor spec](/docs/configuration/monitors#monitor-spec-format). |
| `enable_monitor` | `monitor_spec` | Power on monitor. Accepts a [monitor spec](/docs/configuration/monitors#monitor-spec-format). |
| `toggle_monitor` | `monitor_spec` | Toggle monitor power. Accepts a [monitor spec](/docs/configuration/monitors#monitor-spec-format). |
| `chvt` | `1-9` | Change virtual terminal (tty, equivalent to using ctrl+alt+Fkeys) |
| `screenshot_ui` | `[screen/region/window]` | Compositor-native screenshot UI (defaults to `region`). Freezes the focused output and shows it full-screen while you pick what to capture; saves to `~/Pictures/Screenshots/screenshot_<timestamp>.png` and copies it to the clipboard (requires `wl-copy`). `region` lets you drag a selection rectangle (Escape cancels, release confirms); `window` captures whatever window you click; `screen` captures the whole focused output immediately, with no interaction. |

#### The overlay owns the pointer

While the region or window overlay is up, the pointer is confined to the output
the capture froze, and pushing past an edge pins it to the last row or column
rather than crossing to the next screen — overshooting is how you select the
last pixel, and the selection was already clamped to that output, so a pointer
that could leave only ever left the crosshair somewhere the rectangle could not
follow. It is released the moment the shot is taken or cancelled. A client
holding a pointer *lock* (a game, typically) does not keep it while the overlay
is up, or the crosshair could not move at all.

`screenshot_ui` also works **from the overview**, which is otherwise modal and
drops every bind that is not its own. The spread of every window on a tag is
not something you can capture any other way, and closing the overview to take
the picture destroys the thing being pictured. Escape while the overlay is up
cancels the *screenshot*; the overview stays open.

### Exiting

`quit` puts a prompt on the focused output rather than exiting immediately:
**Enter** exits, **Escape** stays, and anything else is swallowed while the
prompt is up. Dispatching `quit` a second time is not an answer either — a
keybind that repeats would otherwise confirm its own prompt.

The confirmation is on the *dispatch* only. `SIGTERM` and `SIGINT` exit at once,
so a session manager shutting the machine down is never held up by a question
nobody is at the keyboard to answer; scripts that mean it should signal the
process rather than dispatch. With no output to draw on, `quit` exits rather
than waiting for an answer to a prompt nobody can see.

The overlay is the same one the global-shortcuts key picker uses, so the two
look alike by construction.

```kdl
binds {
    Super+S { screenshot_ui region; }
    Super+Shift+S { screenshot_ui window; }
    Super+Ctrl+S { screenshot_ui screen; }
}
```

#### `set_option` is temporary, and quiet

`set_option` takes the **internal** option name — the one `parse_option` matches
(`bordercolor`, `blur_params_radius`), not the nested KDL path
(`layout/border/color`). It changes the value in memory and nothing else: it
writes no file, and the next `reload_config` discards it. It also answers
`{"success":true}` for a key that does not exist, because the dispatch reply is
fixed and the return value is not consulted.

It no longer re-runs your `spawn` entries. It used to: applying one option went
through the full reload path, which respawns every `spawn` command in the
config, so ten adjustments launched your whole startup list ten times. A reload
still runs it — that is what a reload is — but one option changing is not a
reload. See `config_apply_live()` in `src/config/parse_config.h`, and
`contrib/regression/tests/set-option.sh`, which holds the split in place because
the symptom is invisible in a single call.

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
