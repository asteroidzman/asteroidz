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
| `icon` | string or array | artwork; an absolute path, or a name resolved against `bar { icon-dir }` like the built-in modules' icons. An array draws up to four side by side — a state glyph next to a logo, say |
| `tint` | string | recolours the icon — see below |
| `class` | string | `flat` (default), `active`, `urgent`, `occupied`, `empty`, `sunken` — the same looks the built-in pills use |
| `tooltip` | string | shown on hover; omit it for no hover at all |
| `hidden` | bool | render nothing this update |
| `items` | array | draw a **row** of pills instead of one — see below |

### Rows

A plugin normally draws one pill. `items` draws up to 16, which is what a tray
needs: icons appearing and vanishing as applications come and go.

```json
{"items":[
  {"id":"org.kde.StatusNotifierItem-1234-1","icon":"/run/user/1000/x.png","tooltip":"Steam"},
  {"id":"nm-applet","icon":"network-wired","class":"urgent"}
]}
```

Each item takes the same fields as a single pill, plus `id` — which is opaque
to the compositor and handed straight back in the click event, so the plugin
decides what identifies an item. An item with neither `text` nor `icon` is
dropped rather than drawn as an invisible-but-clickable pill.

The two forms are exclusive per update: an update with `items` ignores the
top-level `text`/`icon`, and one without clears any row.

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

### Events, for streaming plugins

A `continuous` plugin gets a **writable stdin**, and a click on one of its
pills arrives there as one JSON line:

```json
{"event":"click","button":"left","item":"nm-applet","x":137,"y":19}
```

`button` is `left`, `right` or `middle`. `item` is the `id` of the pill that
was hit, empty for a single-pill plugin. `x` and `y` are **screen**
coordinates.

Those coordinates are the reason this channel exists rather than only
`on-click`: a tray item's `Activate(x, y)` takes the click position so the
application can place its own window next to its icon, and no shell command can
carry that.

Delivery is best-effort. A plugin that has stopped reading its stdin gets the
event dropped — never queued, never retried, and never allowed to block the
compositor. If the write fails, the configured `on-click` command runs instead,
so a wedged plugin does not silently swallow clicks too.

Interval plugins get no stdin: the command has already exited by the time
anyone could click its pill. They use `on-click` / `on-click-right`.

## Menus

A plugin can ask the compositor to draw a popover menu. It sends rows; the
compositor renders them into the same panel the built-in menus use, with the
same scrolling, keyboard navigation and dismiss behaviour.

```json
{"menu":{"item":"nm-applet","rows":[
  {"text":"Enable Wi-Fi","value":"wifi","selected":true},
  {"text":"Connections","value":"conns","submenu":true},
  {"separator":true},
  {"text":"Quit","value":"quit"},
  {"text":"Busy","value":"x","enabled":false}
]}}
```

| Field | Effect |
|---|---|
| `text` | the row's label |
| `value` | opaque; handed straight back when the row is activated |
| `enabled` | `false` greys it — the click is swallowed, the menu stays up |
| `separator` | a rule; unclickable, and needs no `text` |
| `submenu` | draws a `›` and keeps the menu open when activated |
| `selected` | drawn as checked |
| `icon` | artwork, resolved like a pill's |

### The flow

1. The user clicks a pill. The plugin gets the usual click event.
2. The plugin decides that means "menu", goes and fetches whatever it needs,
   and sends `{"menu":{...}}` — possibly seconds later.
3. The compositor opens the popover **when the rows arrive**, anchored where
   the click landed.
4. Activating a row sends `{"event":"menu","item":"…","value":"…"}` back.

**Submenus need no special support.** A row marked `submenu` keeps the panel
open when activated; the plugin replies with that level's rows, which replace
the panel in place. The compositor never models a menu tree — it only ever
knows the list it is currently drawing.

An empty `rows` array closes the menu, which is the right answer after acting
on a leaf.

### Why it opens late rather than early

The plugin has to go and fetch the menu — a tray item's DBusMenu is two bus
round trips away. Opening on the click would put an empty panel on screen for
as long as that takes, and closing it if nothing came back would flash. So
nothing appears until there is something to show.

A menu that arrives when nothing asked for one is ignored: a plugin is a
process the compositor does not control, and a popup nobody opened appearing
under the pointer would be worse than a missing one.

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
- **No menu of its own.** A plugin cannot draw a popover — it has no scene
  tree, no hit testing and no keyboard. It sends *rows* and the compositor
  renders them; see [Menus](#menus). The plugin never learns where the panel
  is or how tall it got.

## A worked example: Discord voice

`contrib/bar-plugins/discord-voice` reimplements the built-in `discord` module
as a plugin, against the same `discord-voiced` socket:

```kdl
custom "discord" {
    exec       "~/asteroidz/contrib/bar-plugins/discord-voice"
    continuous true
}
```

It is worth reading because it is the honest case. Discord converts well for
one reason: **all the real work already lives in the daemon.** songbird, DAVE,
the audio thread and the gateway connection are `discord-voiced`'s; the
compositor's built-in module is only an IPC client reading newline-JSON off a
socket. Moving that client out moves no work — it just stops the compositor
being the process that has to survive it.

What the plugin reproduces exactly: state and channel name, push-to-talk as the
`active` look, mute as a second icon plus a dimmed tint, errors as `urgent`,
and hiding itself when no daemon is installed.

What it **cannot** reproduce: the popover menu. Joining a channel,
disconnecting, toggling mute and starting the daemon are rows in a menu the
compositor draws, and plugins have no way to ask for one. Mute is still
reachable by writing to the daemon socket from `on-click-right`; joining a
channel would need an external picker. That is the current ceiling, not a
defect in the script.

## The tray, and `asteroidz-trayd`

A tray looks like the thing that cannot be a plugin, and for a long time it
was: it needs N pills appearing and vanishing at runtime, raw ARGB pixmaps
over D-Bus, the click's screen coordinates so an application can place its own
window, and a live nested DBusMenu per item.

Three of those four now work, which is what `items`, the icon array and the
click event channel are for. `asteroidz-trayd` is the result:

```kdl
bar {
    modules-right "custom/tray,volume,clock"
    custom "tray" { exec "asteroidz-trayd"; continuous true }
}
```

### Why it is worth moving

Not because drawing pills is expensive. Because a tray host is the endpoint of
a protocol driven by **every application you happen to have installed**, and
one of the things they hand it is a raw pixmap of their own choosing.

The built-in `tray` module decodes those in a D-Bus reply callback on the
compositor's event loop, and accepts any dimensions an application sends. The
decode is `O(w × h)`. A badly packaged application shipping one oversized icon
can therefore stall the compositor — which is the whole desktop, not a bar.

`asteroidz-trayd` caps pixmaps at **512 px a side**, decodes them in its own
process, and writes a 64 px PNG to `$XDG_RUNTIME_DIR/asteroidz-tray/`. The bar
receives a file path and loads a bounded PNG from disk, the same as any other
icon. Nothing an application sent is ever parsed inside the compositor.

Anything above the cap is **refused, not clamped** — an application sending a
4096² pixmap is broken, and the smaller sizes it almost certainly also sent get
used instead.

### What it does and does not do

Everything the built-in module does, except context menus:

| | |
|---|---|
| Watcher or host role | both, decided at runtime — it mirrors an incumbent watcher rather than fighting it |
| Adopting items already on the bus | yes, which is what stops a restart losing applications that register only once |
| `IconName`, `IconThemePath`, `IconPixmap` | yes; a theme name is passed through for the bar to resolve, since a name follows your icon theme and a pixmap does not |
| `NeedsAttention` | yes, drawn `urgent` |
| `Passive` items | hidden, as the spec allows |
| Left click | `Activate(x, y)` |
| Right / middle click | `SecondaryActivate(x, y)` |
| DBusMenu context menus | yes — right-click, including submenus, separators, checked and disabled entries |

An item that ships no menu at all still falls back to `SecondaryActivate` on
right-click, which many applications wire to "show my menu" themselves.

The two hosts coexist, and switching is one word:

```kdl
modules-right "tray,volume,clock"          // built-in: decodes pixmaps in-process
modules-right "custom/tray,volume,clock"   // trayd: bounded, out-of-process
```

### Testing it

`contrib/trayd-test.sh` runs the daemon against its own `dbus-daemon` and a
stand-in item (`contrib/snitem`), so it needs no tray application installed and
never touches the live session's tray. `snitem --pixmap N` is what exercises
the cap: point it at any host and watch whether an absurd `N` gets decoded.

### The rule of thumb

If the work can live in a daemon and the bar only needs to *display* the
result, it can be a plugin. If the bar itself has to be the endpoint of a
protocol, it needs a helper that is the endpoint instead — which is what trayd
is, and why it is a separate binary rather than a shell script.

## Limits

| | |
|---|---|
| Plugins per config | 12 |
| Items per plugin (`items`) | 16 |
| Icons per pill (`icon` array) | 4 |
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
