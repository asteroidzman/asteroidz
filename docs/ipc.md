---
title: IPC
description: Control asteroidz programmatically using amsg.
---

# amsg(1) - User Manual

`amsg` is the command-line interface for asteroidz's Inter-Process Communication (IPC) system. It allows users and scripts to query the state of the compositor or subscribe to real-time events.

## SYNOPSIS
`amsg <command> [arguments...]`

## DESCRIPTION
`amsg` acts as a client that connects to the asteroidz compositor via a Unix domain socket defined by the `ASTEROIDZ_INSTANCE_SIGNATURE` environment variable. It supports two primary modes of operation:
1. **One-shot Request (`get`)**: Sends a query to the compositor, receives a single JSON response, and terminates.
2. **Persistent Stream (`watch`)**: Subscribes to a specific state, receiving continuous JSON updates whenever that state changes.

## ENVIRONMENT VARIABLES
* **`ASTEROIDZ_INSTANCE_SIGNATURE`**: Must be set to the path of the Unix socket created by the running asteroidz instance. This is typically handled automatically when running `amsg` from within a terminal spawned by the compositor.

## COMMANDS

### GET (One-Shot Queries)
| Command | Description |
| :--- | :--- |
| `get version` | Returns the current version of the compositor. |
| `get cursorpos` | Returns the global pointer position (`x`, `y`) and the monitor under it. |
| `get keymode` | Returns the current active keyboard mode (e.g., normal, insert). |
| `get keyboardlayout` | Returns the active XKB layout (abbreviated). |
| `get monitor <name>` | Returns full JSON details for a specific monitor. |
| `get focused-client` | Returns full JSON details for the client currently in focus. |
| `get client <id>` | Returns full JSON details for a client with the given ID. |
| `get tag <mon> <idx>` | Queries status of a specific tag on a monitor. |
| `get tags <mon>` | Returns a JSON object containing the status of all tags on a monitor. |
| `get all-clients` | Returns a JSON array of all active clients. |
| `get all-monitors` | Returns a JSON array of all connected monitors. |
| `get all-tags` | Returns a JSON object containing the status of all tags. |
| `get last_open_surface [<mon>]` | Returns the last focused surface name for a monitor,if the mon not set, it will get current monitor. |
| `get bar-config` | Returns the resolved bar geometry, palette and module lists, for an out-of-process bar. |

*Example:*
```bash
amsg get monitor eDP-1
amsg get all-clients
amsg get all-monitors
amsg get cursorpos
```

#### Large replies

A reply of any size arrives whole. Worth stating because it was not always
true: every IPC connection is non-blocking, and `send()` on a non-blocking
socket writes what fits in `SO_SNDBUF` and returns *that count* — which is not
negative, so a `< 0` error test reports success on a partial write. The
connection was then closed immediately, so anything past the socket buffer
(~208KB on a Linux `AF_UNIX` socket, though the kernel picks the number) was
lost with nothing logged anywhere. What a client saw was JSON ending
mid-string.

Nothing served at the time was large enough to reach it. Replies are now queued
and flushed across as many event-loop cycles as it takes, and a one-shot
connection closes when its reply is out rather than when the handler returns.
See `src/ipc/ipc-out.h`, and `tests/test-ipc-out.c`, which shrinks `SO_SNDBUF`
on a socketpair to force the partial write a normal-sized reply never triggers
— asserting against a normal socket would pass on the broken code.

A subscriber that stops reading is dropped once its backlog passes 4MB, rather
than growing the compositor's memory on its behalf.

### WATCH (Event Subscription)
Subscribes the client to real-time updates. When the state changes, the server pushes a new JSON object to the output stream.

* `watch monitor <name>`
* `watch focused-client`
* `watch client <id>`
* `watch tags <mon_name>`
* `watch all-monitors`
* `watch all-tags`
* `watch all-clients`
* `watch keymode`
* `watch keyboardlayout`
* `watch last_open_surface [<mon_name>]`
* `watch bar-config`

*Example:*
```bash
# watch all monitors
amsg watch all-monitors
# watch all tags
amsg watch all-tags
```

#### bar-config, for a bar that is not the compositor

`bar-config` answers with what the compositor **resolved** -- after defaults,
after clamping, after the theme file -- rather than with the config text:

```json
{
  "bar":     { "height": 48, "position": "top", "margin_x": 8, ... },
  "panel":   { "enable": true, "radius": 9, "blur": true, "color": [...] },
  "popover": { "width": 340, "row_height": 34, ... },
  "theme":   { "fg": [1,1,1,1], "focus_bg": [...], "font": "Ubuntu 16", ... },
  "custom":  [ { "name": "nordvpn", "exec": "asteroidz-bar-nordvpn", ... } ]
}
```

Colours are `[r,g,b,a]` floats, not hex strings: CSS reads `#RRGGBBAA` and Qt
reads `#AARRGGBB`, and a string that parses under both conventions while
meaning different things is a bug that surfaces months later as "the bar is
slightly the wrong colour".

Handing a bar the config file to parse instead would be two KDL readers that
agree until one of them gains a default -- and it would still not see the
palette, which matugen rewrites at runtime whenever the wallpaper changes. The
compositor is the only process that knows what the theme currently *is*.

`watch bar-config` pushes the same object again on every `reload_config`, so a
bar in another process repaints with the new palette instead of waiting for
something else to wake it.

#### Output configuration

| Dispatch | Effect |
| :--- | :--- |
| `set_output_mode,<output>,<WxH[@Hz]>` | Pick a mode the output actually reports. |
| `set_output_scale,<output>,<scale>` | Fractional scales included. |
| `set_output_position,<output>,<x>,<y>` | Move it in the layout, live. |
| `set_output_vrr,<output>,<0\|1>` | Adaptive sync. |
| `set_output_hdr,<output>,<0\|1>` | This display's HDR **baseline** — see below. |
| `set_output_icc,<output>,<path>` | ICC profile for SDR output; empty clears it. |

Every one names its output rather than acting on the focused one: a dispatch
has no pointer context, and guessing would make the same command do different
things depending on where the mouse happened to be.

Mode and scale are **tested before they are committed** (`wlr_output_test_state`),
and a rejected commit starts a retrain -- a picker must not be able to black
out a display. A mode that the output does not advertise is refused rather than
synthesised.

##### HDR is three levels, not a switch

`set_output_hdr` sets one of them. From weakest to strongest:

| | |
|---|---|
| `misc { hdr-mode off\|auto\|on }` | global policy: never / let the levels below decide / always |
| per-output `hdr` | this display's desktop baseline — what `set_output_hdr` writes |
| window-rule `force_hdr` | this app overrides, while it is visible on that output |

A global tri-state cannot express "HDR on the capable panel, SDR on the one
beside it", which is the ordinary two-monitor case — hence the per-output level.
And `auto` is what makes `force_hdr` useful: SDR desktop, until mpv is on
screen. `off` is a kill switch that overrides `force_hdr` too.

`set_output_hdr` writes `hdr_configured`, the **input** to the resolver, never
the resolved `m->hdr`. Setting a baseline the policy currently overrides is not
an error: it is remembered and applies when the policy allows. `toggle_hdr` is a
convenience over this and nothing more — it reads the baseline, inverts it, and
hands over.

Read the result with `hdr` (what the output is really doing),
`hdr_enabled` (the baseline that was asked for) and `hdr_capable` (hardware).

##### They are saved, too

A setting that applies and then vanishes at the next `reload_config` is not a
setting, so each of these writes itself back to the config -- into whichever
file already declares that output. `source "./monitors.kdl"` is the
conventional split, and parse_config records every path it reads precisely so a
writer can find the right one: putting the rule in the main config while a
sourced file still sets it would produce a setting that silently loses to one
you cannot see.

The edit is **textual and surgical**. It replaces the bytes holding those
values and copies every other byte through, so comments, spacing, and anything
the compositor does not model survive being written around -- the file is
hand-maintained as well as machine-written, and regenerating the block from the
`Monitor` struct would be correct and still lose the comment explaining why an
output is configured the way it is. It is written to a temporary and renamed
over, so a crash mid-write cannot leave a config that will not parse.
See `src/common/kdl-edit.h`, and `tests/test-kdl-edit.c`, which is what a
change there answers to.

An output with **no block anywhere** is applied but not saved, and logs that it
was not. Inventing one means choosing a file and a place in it, and guessing
that about a config someone maintains by hand is worse than a line in the log
telling you to add `output NAME { }` yourself.

### DISPATCH
Allows sending commands to the compositor to alter its state.
* `dispatch <func_name>,[args...] [client,<id>]`

*Example:* 
```bash   
# operate specific client by id
amsg dispatch exchange_client,left client,375
# operate current client
amsg dispatch exchange_client,left
````
