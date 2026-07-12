---
title: Mouse & Gestures
description: Configure mouse buttons, scrolling, gestures, and lid switches.
---

## Mouse Bindings

Assign actions to mouse button presses with optional modifier keys.

### Syntax

```kdl
mouse-binds {
    MODIFIERS+BUTTON { COMMAND PARAMETERS; }
}
```

- **Modifiers**: `SUPER`, `CTRL`, `ALT`, `SHIFT`, `NONE`. Combine with `+` (e.g., `SUPER+CTRL`).
- **Buttons**: Can be specified in one of the following ways:
  - **Standard Names**: `btn_left`, `btn_right`, `btn_middle`, `btn_side`, `btn_extra`, `btn_forward`, `btn_back`, `btn_task`
  - **Hardware Codes**: `code:NUMBER` (e.g., `code:272`, `code:273`, useful for binding non-standard or extra mouse buttons)

> **Warning:** When modifiers are set to `NONE`, only `btn_middle` works in normal mode. `btn_left` and `btn_right` only work in overview mode.


### Examples

```kdl
mouse-binds {
    Super+btn_left { move_resize curmove; }
    Super+btn_right { move_resize curresize; }
    Super+Ctrl+btn_right { kill_client; }
    NONE+code:273 { toggle_maximize 0; }
}
```

---

## Axis Bindings

Map scroll wheel movements to actions for workspace and window navigation.

### Syntax

```kdl
axisbind "MODIFIERS" "DIRECTION" "command" "parameters..."
```

- **Direction**: `UP`, `DOWN`, `LEFT`, `RIGHT`

### Examples

```kdl
misc {
    axisbind SUPER,DOWN,view_to_right_occupied
}
```

---

## Gesture Bindings

Enable touchpad swipe gestures for navigation and window management.

### Syntax

```kdl
gesturebind "MODIFIERS" "DIRECTION" "FINGERS" "command" "parameters..."
```

- **Direction**: `up`, `down`, `left`, `right`
- **Fingers**: `3` or `4`

> **Info:** Gestures require proper touchpad configuration. See [Input Devices](/docs/configuration/input) for touchpad settings like `tap_to_click` and `disable_while_typing`.

### Examples

```kdl
misc {
    gesturebind none,down,4,toggle_overview
}
```

---

## Switch Bindings

Trigger actions on hardware events like laptop lid open/close.

### Syntax

```kdl
switchbind "FOLD_STATE" "command" "parameters..."
```

- **Fold State**: `fold` (lid closed), `unfold` (lid opened)

> **Warning:** Disable system lid handling in `/etc/systemd/logind.conf`:
>
> ```kdl
misc {
    > HandleLidSwitch ignore
    > HandleLidSwitchExternalPower ignore
    > HandleLidSwitchDocked ignore
}
```ini
switchbind=fold,spawn,swaylock -f -c 000000
switchbind=unfold,spawn,wlr-dpms on
```
