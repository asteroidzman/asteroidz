---
title: Special Workspaces
description: Named, per-monitor overlay workspaces with full tiling support (Hyprland-style).
---

Named special workspaces are per-monitor overlays that slide in on top of whatever tag is
currently active. Unlike the [scratchpad](/docs/window-management/scratchpad), which only holds
floating windows one at a time, a special workspace can hold any number of windows and tiles them
using a normal master/stack layout — so it behaves like an extra, hidden tag that you can pop open
over your current view rather than switching away from it.

Each monitor tracks at most one open special workspace at a time (opening a different one
implicitly closes the one already showing), but different monitors can each have their own special
workspace open independently. Windows keep the tag they were on before entering a special
workspace, so moving them back out returns them to where they were.

## Toggling a Special Workspace

```ini
bind=SUPER,s,togglespecialworkspace,term
```

Calling `togglespecialworkspace` with the same name again slides it back out. The workspace does
not need to contain any window yet — toggling an unused name just opens/closes an empty overlay
that you can assign windows to afterwards.

## Assigning Windows

**At launch, via a window rule:**

```ini
windowrule=special_workspace:term,appid:kitty
```

Any window matching this rule is placed in the `term` special workspace as soon as it maps. It
stays hidden until a monitor's `term` special workspace is toggled open.

**At runtime, for the focused window:**

```ini
bind=SUPER+SHIFT,s,movetospecialworkspace,term

# Move the focused window back out to its normal tag
bind=SUPER+CTRL,s,movetospecialworkspace
```

`movetospecialworkspace` called with no name (or an empty one) moves the focused window back out
to whichever tag it already belonged to.

## Tiling

Windows inside a special workspace tile among themselves with a master/stack layout, independent
of whichever layout (`tile`, `scroller`, `dwindle`, ...) is set on the underlying tag; the master
count and ratio follow the global `default_nmaster` / `default_mfact` settings. Floating windows
assigned to a special workspace keep floating, exactly as they would on a normal tag.

## Animation

Opening and closing a special workspace slides it in using the same spring-based slide animation
as switching tags (`animation_curve_tag`, `tag_animation_direction`, etc. all apply). Pinned and
global windows are exempted the same way they are exempted from tag-switch animations: they stay
visible and simply settle in place instead of sliding.

## IPC

The name of the special workspace currently showing on a monitor (empty string if none) is exposed
as `active_special` in `amsg get monitors` / `amsg get monitor <name>`. See [IPC](/docs/ipc) for
details.
