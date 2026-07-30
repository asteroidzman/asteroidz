---
title: Basic Configuration
description: Learn how to configure asteroidz with its KDL config file, environment variables, and autostart.
---

## Configuration File

asteroidz is configured with a nested [KDL](https://kdl.dev) file, structured
like Niri's config. By default it looks for `~/.config/asteroidz/config.kdl`.

1. **Locate Default Config**

   A fallback configuration is provided at `/etc/asteroidz/config.kdl`. Use it
   as a reference.

2. **Create User Config**

   ```bash
   mkdir -p ~/.config/asteroidz
   cp /etc/asteroidz/config.kdl ~/.config/asteroidz/config.kdl
   ```

3. **Launch with Custom Config (Optional)**

   ```bash
   asteroidz -c /path/to/your_config.kdl
   ```

Settings are grouped into sections. A small example:

```kdl
layout {
    titlebar { enable; height 36 }
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
asteroidz -S    # check that table against the parser it describes
```

`-L` is the list a settings UI reads. `-S` is what keeps it honest: it drives
the real parser and asserts every default, clamp and type against it, so a table
entry cannot drift from the code without a test going red. Neither flag reads
your config — `-L` and `-S` report the compiled-in defaults.

Not every option is described yet. `tests/schema-exempt.txt` lists the ones that
are not, each under a stated reason, so the gap is visible rather than implied.

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
