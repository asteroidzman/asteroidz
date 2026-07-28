---
title: Bar Plugins
description: Extending the native bar with out-of-process custom modules.
---

The [native bar](./native-bar.md) ships a fixed set of modules. A **plugin**
adds one of your own: a command the compositor runs, whose output becomes a
pill.

```kdl
bar {
    enable true
    modules-right "custom/mail,volume,clock"

    custom "mail" {
        exec     "~/.config/asteroidz/bar/mail"
        interval 60
        on-click "thunderbird"
    }
}
```

The name in the module list is `custom/` plus the block's name. A `custom`
block that no module list mentions is inert — its command is never run.

## A plugin is a process, never a library

There is no `dlopen` path and there will not be one. A shared object loaded
here would run on the compositor's event loop with the compositor's lifetime:
one blocking `read()` is a frozen screen, one bad `free()` is a lost session.
Waybar can afford that bargain because a crashed bar is a crashed bar; this is
the whole desktop.

The corollary is that plugins are cheap to get wrong. A plugin that hangs,
crashes, floods or was never installed costs you a missing pill and nothing
else.

## Output

Write **one JSON object per update** on stdout:

```json
{"text":"12","icon":"mail.svg","tint":"urgent","tooltip":"12 unread"}
```

Anything that is not JSON is taken as the text itself, so this is a complete,
valid plugin:

```sh
#!/bin/sh
echo "$(find ~/Mail/new -type f | wc -l) unread"
```

Every field is optional.

| Field | Type | Effect |
|---|---|---|
| `text` | string | the pill's label |
| `icon` | string | artwork; an absolute path, or a name resolved against `bar { icon-dir }` like the built-in modules' icons |
| `tint` | string | recolours the icon — see below |
| `class` | string | `flat` (default), `active`, `urgent`, `occupied`, `empty`, `sunken` — the same looks the built-in pills use |
| `tooltip` | string | shown on hover; omit it for no hover at all |
| `hidden` | bool | render nothing this update |

A plugin that has not answered yet, that sets `hidden`, or that supplies
neither `text` nor `icon`, **draws nothing at all** — no placeholder, no error
pill, and no width reserved. This is deliberate: the common failure is a config
copied to a machine where the script is not installed, and a permanent `?` in
the corner of every screen is worse than silence. Check the log if a pill you
expect is not there.

Unparseable JSON is logged at `WLR_DEBUG` and the pill keeps whatever it last
showed, so a plugin with an occasional hiccup is briefly stale rather than
flickering.

### Tint

Most icon sets worth using are monochrome silhouettes — including the ones the
built-in modules draw — which paint as an invisible black blob on a dark panel
unless they are recoloured. `tint` takes a **theme token** by preference:

| Token | Colour |
|---|---|
| `accent` (or `focus`) | the theme's focus colour |
| `urgent` | the theme's urgent colour |
| `fg` | the theme's foreground |
| `dim` | the foreground at 45% — an unlit indicator |

A `#rrggbb` / `#rrggbbaa` literal is also accepted, for the cases a token
cannot express. Prefer the tokens: a plugin that hardcodes a hex value is wrong
the moment you change theme.

## How it runs

Two modes, chosen per plugin.

### `interval N`

The command is run every `N` seconds and its whole output taken as one update.
This is the right mode for anything that wraps a CLI.

```kdl
custom "gpu" {
    exec     "cat /sys/class/drm/card1/device/gpu_busy_percent"
    interval 5
}
```

The countdown runs on the bar's shared metrics tick, so `interval` is rounded
up to a multiple of `bar { interval }` (default 1s). `interval 0` runs the
command **once** and never again — for something whose answer cannot change.

Runs never overlap: if a command is still going when its next slot comes
round, that slot is skipped rather than starting a second copy.

### `continuous true`

The command is started once and stays running, emitting **one JSON object or
line of text per update**. This is the right mode for anything event-driven —
a `dbus-monitor`, an `inotifywait` loop, a daemon's event stream — because it
costs no process per update and reacts immediately.

```kdl
custom "vpn2" {
    exec       "~/.config/asteroidz/bar/vpn-watch"
    continuous true
}
```

Flush after every line, or your updates sit in libc's buffer. In a shell script
`echo` is already unbuffered; in Python use `print(..., flush=True)`; with
`grep` in the pipeline add `--line-buffered`.

The child is started when the plugin is on a bar and stopped when it is not —
including when you remove it from a module list and reload.

## Clicks

```kdl
custom "mail" {
    exec           "~/.config/asteroidz/bar/mail"
    interval       60
    on-click       "thunderbird"
    on-click-right "notify-send 'Mail' \"$(mailcount)\""
}
```

Both run through `/bin/sh -c`, fire-and-forget. Nothing waits for the command,
so a slow one costs you nothing; the plugin's next update reports whatever
changed.

`exec` is run the same way, which is why pipes, `~`, `$VARS` and quoting all
behave as they do in a shell. It is also why `exec "my script.sh"` with a space
in the path needs quoting like any other shell word.

## What plugins deliberately cannot do

Not oversights — each one is load-bearing:

- **No drawing surface, and no frame callback.** A pill is one scene node and
  hit testing is per node, so sub-widgets a plugin drew itself would be
  decoration that lies about where you may click. Generated artwork —
  meters, spectra — is drawn compositor-side where it can be cached on
  quantised state; the network arrows and the media visualiser work exactly
  this way.
- **No control over redraw timing.** An animating bar element damages the
  output every frame, which recomposites the screen for as long as it animates.
  That budget belongs to the compositor, not to a script.
- **No menus yet.** Popover menus need a bidirectional transport so a click can
  be delivered back; that is planned alongside a socket-based plugin mode.

## Limits

| | |
|---|---|
| Plugins per config | 12 |
| Label length | 256 bytes, truncated |
| Modules on a bar, all sections | 24 |

Plugins are also the **first thing shed** when a bar does not fit its output —
ahead of every built-in. See
[Fitting a narrow output](./native-bar.md#fitting-a-narrow-output).

## Debugging

A plugin that does not appear is almost always one of:

1. **No `custom` block for that name.** Logged as
   `bar: no custom block named 'x'`.
2. **The command is not executable**, or the path is wrong. The pill stays
   invisible; run the command in a shell to check it.
3. **It printed nothing**, or only whitespace.
4. **JSON with neither `text` nor `icon`.** Valid, and renders nothing.

Plugins name themselves in profiler traces, so a slow one is identifiable by
name rather than showing up as an anonymous `custom` zone — see
[Cost](./native-bar.md#cost).
