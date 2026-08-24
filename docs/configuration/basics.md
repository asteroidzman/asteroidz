---
title: Basic Configuration
description: Learn how to configure asteroidz with its KDL config file, environment variables, and autostart.
---

## Configuration File

asteroidz is configured with a nested [KDL](https://kdl.dev) file, structured
like Niri's config. By default it looks for `~/.config/asteroidz/config.kdl`.

1. **Locate Default Config**

   A fallback configuration is provided at `/etc/asteroidz/config.kdl`, with its
   palette at `/etc/asteroidz/colors.kdl`. Copy both — the first sources the
   second, and a `source` that cannot be opened is fatal. Use them
   as a reference.

2. **Create User Config**

   ```bash
   mkdir -p ~/.config/asteroidz
   cp /etc/asteroidz/config.kdl /etc/asteroidz/colors.kdl ~/.config/asteroidz/
   ```

   `/etc/asteroidz/autostart.sh` starts the bar and whatever else a session
   needs. Copy it too only if you want to change that — the config falls back to
   the shipped one:

   ```bash
   cp /etc/asteroidz/autostart.sh ~/.config/asteroidz/
   ```

3. **Launch with Custom Config (Optional)**

   ```bash
   asteroidz -c /path/to/your_config.kdl
   ```

Settings are grouped into sections. A small example:

```kdl
layout {
    titlebar { enable }
    border { width 2; gradient { enable; angle 45 } }
}
effects {
    blur { enable; passes 2; radius 6 }
    shadow { enable; size 5 }
}
```

A bare node is an on flag (`titlebar { enable }` == `enable true`); a node with
a value sets it (`height 36`). Booleans are `true`/`false` or `1`/`0`.
Comments are `//` (line) or `/* … */` (block) — KDL has no `#`.

### Sub-Configuration

Split your config across files and pull them in with `source`:

```kdl
// Import keybindings from a separate file
source "~/.config/asteroidz/binds.kdl"

// Relative paths work too (relative to the main config)
source "./theme.kdl"

// Optional: ignore if the file doesn't exist (useful for shared configs)
source-optional "~/.config/asteroidz/optional.kdl"
```

### Validate Configuration

Check your configuration for errors without starting asteroidz:

```bash
asteroidz -c /path/to/config.kdl -p
```

### Inspect the option schema

Every settable option is also described in a table the compositor carries —
type, range, enum members, default, and a one-line explanation — so that tools
do not have to re-implement the parser to know what an option accepts.

```bash
asteroidz -L    # list every described option, tab separated
asteroidz -R    # list every window-rule field, tab separated
asteroidz -S    # check both tables against the parsers they describe
```

`-L` and `-R` are the lists a settings UI reads — one for options, one for
[window-rule fields](/docs/window-management/rules). `-S` is what keeps both
honest: it drives the real parsers and asserts every default, clamp, offset and
type against them, so a table entry cannot drift from the code without a test
going red. For rules that means writing each field through the real `windowrule`
branch and reading it back through its own `offsetof`, which is what makes a
copy-pasted offset a red test rather than a rule that silently sets its
neighbour. None of these flags read your config — they report the compiled-in
defaults.

### Does the option actually do anything?

`-S` proves a key parses, clamps and lands in the right field. That is not the
same question as whether anything downstream reads it — `srgb_blending` did the
first perfectly for a whole release while its only consumer had already left with
SceneFX.

```bash
asteroidz -T    # check what the keys and dispatches actually do
```

`-T` starts a real compositor on the headless backend, in its own process, and
walks the same two tables — every option in `-L` and every dispatch in `-D`. For
each one it applies the setting, lets the frame settle, and then reads the
**result**: composited pixels from the renderer's own capture, or the geometry the
layout produced. It never reads back the field it just wrote; that is `-S`'s job
and is true even of a compositor that ignores the value.

```
-T: 2 window(s) on HEADLESS-1 1280x720
OK    gappoh                       left edge 44 = w.x 0 + 44
OK    borderpx                     11 px of border at y=360
OK    border_radius                100 of 576 corner pixels cut (a square corner cuts 0)
OK    toggle_fullscreen            1280x720+0+0 covers the 1280x720 output
SKIP  blur                         no check
99 options, 111 dispatches, 12 checks, 128 assertions, 198 skipped, 0 failures
```

An entry with no assertion prints `SKIP`, so coverage is in the output of every
run rather than in a list that goes stale — a key added to the schema starts life
visibly unchecked. It exits non-zero on any failure.

It runs against the compiled-in defaults, not your config, so the answer does not
depend on whose machine it is. Windows come from `contrib/wlbgeffect` unless you
pass your own `-s '<command>'`; without a client, every check that needs a window
says so and skips rather than quietly passing. Animations are off for the run,
which is why the `animations` group cannot be checked this way, and headless has
no connector, so `vrr`, `bitdepth` and `hdr` skip permanently.

To see where the values you are actually running came from:

```bash
asteroidz -P -c ~/.config/asteroidz/config.kdl
```

One line per key: the file and line the declaration is in, the path it was
*written* at (often not the canonical one — `misc { border_radius 9 }` is a legal
spelling of a top-level `border_radius`), whether that file may be written to, and
whether the running value was last set in memory rather than read from the file.

The last two are separate columns on purpose. A file is unwritable when it carries
a generator's marker — matugen's `colors.kdl` is rewritten on every wallpaper
change, so an edit there is not refused, it is silently reverted. And "set in
memory" is independent of "declared in a file": both are true after a live preview,
and a tool that collapses them loses the declaration it needs to edit.

## Environment Variables

Define environment variables in an `environment` block. They are set before
the window manager initializes.

> **Warning:** Environment variables defined here are **reset** every time you
> reload the configuration.

```kdl
environment {
    QT_IM_MODULES "wayland;fcitx"
    XMODIFIERS "@im=fcitx"
}
```

## Autostart

asteroidz can run commands at startup:

| Node | Behavior | Use Case |
| :--- | :--- | :--- |
| `spawn-at-startup` | Runs **only once** when asteroidz starts. | Status bars, wallpapers, notification daemons |
| `spawn` | Runs **every time** the config is reloaded. | Scripts that refresh settings |

### Example Setup

```kdl
// Start the status bar once
spawn-at-startup "asteroidz-bar"

// Set the wallpaper (each argv token is its own string)
spawn-at-startup "swaybg" "-i" "~/.config/asteroidz/wallpaper/room.png"

// Re-run a script on every config reload
spawn "bash" "~/.config/asteroidz/reload-settings.sh"
```
