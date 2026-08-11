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
| `get cursorpos` | Returns the global pointer position (`x`, `y`), the monitor under it, and what the pointer currently looks like: `cursor-shape` is the [`wp_cursor_shape_v1`](https://wayland.app/protocols/cursor-shape-v1) name the focused client last asked for (`"default"`, `"pointer"`, `"text"`, …, or `"unset"` if none has), and `cursor-surface` is `true` when the client supplied a surface of its own instead, in which case there is no shape to name. |
| `get idle` | Returns `inhibited` (what the idle notifier was told — will this machine sleep), `manual` (the flag `toggle_idle_inhibit` owns), and `portal` (every live [Inhibit portal](./configuration/xdg-portals.md#inhibit) request, with the app that asked, its reason and its flags). Separate fields because a client's own inhibitor is not something a toggle can clear, and a portal request has no window to point at. |
| `get keymode` | Returns the current active keyboard mode (e.g., normal, insert). |
| `get keyboardlayout` | Returns the active XKB layout (abbreviated). |
| `get monitor <name>` | Returns full JSON details for a specific monitor. |
| `get focused-client` | Returns full JSON details for the client currently in focus. |
| `get client <id>` | Returns full JSON details for a client with the given ID. |
| `get tag <mon> <idx>` | Queries status of a specific tag on a monitor. |
| `get tags <mon>` | Returns a JSON object containing the status of all tags on a monitor. |
| `get all-clients` | Returns a JSON array of all active clients. |
| `get all-monitors` | Returns a JSON array of all connected monitors. |
| `get avk-stats` | Returns the native Vulkan engine's live counters (see below). |
| `get all-tags` | Returns a JSON object containing the status of all tags. |
| `get last_open_surface [<mon>]` | Returns the last focused surface name for a monitor,if the mon not set, it will get current monitor. |
| `get bar-config` | Returns the resolved theme -- palette, font, border and corner metrics -- for an out-of-process bar. |
| `get config-schema [<group>]` | Describes every settable option: type, range, enum members, default, label and explanation. |
| `get config-schema-digest` | Just the schema's digest, so a cached copy can be revalidated in one round trip. |
| `get config [<group>]` | The current value of every described option, with where it came from. |
| `get dispatch-actions` | Every dispatchable action and the kinds of its arguments. |

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

#### The config, for a settings UI

Four read verbs, split because they change at four different rates.

`get config-schema` describes every settable option — type, range, enum members
with their names, the default, a label and a one-line explanation. It is
compile-time constant, so **cache it**: it carries a `digest`, and
`get config-schema-digest` is a ~60 byte reply that revalidates that cache in one
round trip across a compositor restart. Both accept an optional group
(`get config-schema effects`) for a cheap per-page fetch.

This exists so a client does not have to carry a hardcoded mirror of the
compositor's parser. Types, ranges and enum members are inline in
`parse_option`'s if/else chain and defaults are 440 lines of assignments in
`set_value_default`; a mirror of that drifts the first time either side gains a
default, silently, and then writes wrong values into a hand-maintained config.
The table is hand-written and checked from both ends — see
[regression testing](/docs/regression-testing#the-schema-checked-from-both-ends).

`get config` is the current value of each of them, plus **where it came from**:

```json
"bordercolor": {
  "value": "0x2c2c2cff",
  "rgba": [0.173, 0.173, 0.173, 1.0],
  "is_default": false,
  "source": { "kind": "file", "file": "…/colors.kdl", "line": 11,
              "path": "layout/border/color",
              "writable": false, "reason": "matugen" }
}
```

Three things in there are load-bearing:

- **Colours carry both forms.** `value` is the `0xRRGGBBAA` a user types and a
  writer round-trips; `rgba` is what a UI paints with. Neither side has to guess
  the other's byte order. The conversion rounds rather than truncates — a
  truncating round trip turns `0x2c` into `0x2b`, so a settings app would appear
  to darken the theme a little every time it saved.
- **`source.file` and `line` are recorded as the value is applied**, not derived
  afterwards, because they are not derivable. `source` is applied in place, so a
  key set in `config.kdl` and again in a sourced `colors.kdl` is won by
  `colors.kdl` — while `config_files[]` is in the order files were *opened*,
  which puts `config.kdl` first. A writer that scanned that list would edit a
  declaration something invisible then overrules.
- **`source.path` is where it was actually written**, which for most keys is not
  the canonical path. `misc { border_radius 9 }` is how this config spells a key
  whose canonical spelling is a bare top-level node, because the leaf lookup
  falls back to the node's own name. A writer that assumed the canonical path
  would append a second declaration that silently wins by position.

`writable` is decided **per file**, not per key, from a marker in the file's
header — matugen's *"Auto-generated file. Do not edit directly."*, the display
plugin's *"Managed by the waybar-display plugin"*, or a new
`asteroidz:generated-by <tool>` convention. Per-key would be a list that is wrong
the moment a generator writes a tenth key, and says nothing about a generator
nobody here has heard of. `get config` also returns the file list with the same
flag, so a UI can explain itself without walking every key.

`kind` is `file`, `default`, or `runtime`, and it answers exactly one question:
**will a reload change this value?**

| `kind` | meaning |
|---|---|
| `file` | the running value came from the declaration named by `file`/`line`/`path` |
| `runtime` | it was set in memory — `dispatch set_option`, or `set-config` with `persist:false` — and a reload will undo it |
| `default` | nothing on disk sets it and it is at its compiled-in default |

`runtime` **still carries `file`, `line` and `path` when a declaration exists.**
Both facts are true after a `persist:false` write: the running value was set in
memory, *and* there is a line in a file that still says something else. `kind`
answers "is it saved"; `file`/`line`/`path` answer "where does it live". They are
different questions, and collapsing them broke three things at once — a
`persist:false` write used to erase the file fields, so afterwards:

- a persisting write of the same key found no declaration, fell back to the
  canonical path in the main config, and appended a **second** declaration —
  leaving `misc { border_radius 9 }` dead and a duplicate winning by position;
- a previewed removal had no line left to delete and reported success with the
  setting still in the file;
- a previewed key owned by `colors.kdl` lost the origin that makes it read-only,
  so the next persisting write went to `config.kdl` **without `override:true`
  ever being asked for**.

All three are pinned by
`contrib/regression/tests/config-write.sh:test_set_config_a_preview_does_not_lose_the_declaration`.
They matter because a live-preview UI previews *every* edit, so every save in one
goes through this path.

Conversely, a memory-only write that lands on the value the key would have had
anyway reports `default`, not `runtime`: the state is indistinguishable from never
having been touched, and `runtime` would promise a change that a reload will not
make.

`get dispatch-actions` lists every dispatchable action with the *kinds* of its
arguments (`direction`, `tag-index`, `layout-name`, `option-key`, …), so a
keybind editor can offer the right control rather than a text box. `option-key`
composes with the schema: it means "offer the option list from
`get config-schema`".

#### Window rules and binds

```
get window-rule-schema      get window-rules      get binds
```

`get window-rule-schema` is the option schema one level down: all 53 window-rule
fields with their type, range, group and explanation. Two of its flags are the
whole reason it exists, because a UI gets both wrong by default:

- **`regex: true`** on `app-id`, `title` and `toplevel-tag`. A plain text field
  there produces rules that never match, because a `.` in an app id is a wildcard
  and the user meant a dot.
- **`tristate: true`** on most of the rest. These are `-1` unset, `0` off, `1`
  on. A checkbox cannot express three states, and one drawn against them turns
  every field the rule never mentioned into an explicit `0` the moment it is
  saved.

Every field carries both spellings: `nice` is the canonical dashed KDL name a
writer should emit, `key` is the bare name the legacy `windowrule=` form uses and
what everything else is keyed by. Both parse.

`get window-rules` serves the current rules, and reports **only the fields each
rule actually sets**. That is not a size optimisation — it is the difference
between "this rule says nothing about blur" and "this rule turns blur off", which
`ConfigWinRule` can express and a serialiser that emitted all 53 fields could not.
Values come back in the spelling they were written in, so `tags 4` reports `4`
and not the `1 << 3` the field holds.

`get binds` is served from records captured **while the config is read**, not from
the parsed `KeyBinding` array, because that parse is lossy in exactly the place an
editor needs:

```
focus_stack next     becomes    func = focusstack, arg.i = NEXT
tag_silent 3         becomes    func = tagsilent,  arg.ui = 1 << 2
```

There is no way back — two dispatch names can share a function, `Arg` is a union
whose meaning depends on which function reads it, and `1 << 2` is a plausible
value for four different fields. So each bind reports the chord verbatim, the
dispatch name, the raw argument strings, the keymode it was declared in, and the
four `lock`/`keysym`/`release`/`pass` flags.

`source.editable` is the honest field. A bind written in the legacy `bind=` line
form has no KDL node and therefore no byte span to replace; it is still listed —
hiding binds a UI cannot write is how someone edits around one they cannot see —
but it cannot be rewritten. `not_listed` names the kinds that have no KDL block
handler at all (`axisbind`, `switchbind`, `gesturebind`), so a bind list is not
quietly claiming those do not exist.

#### capture-chord, for a keybind editor

```
capture-chord
```

Press a key combination; get it back as a chord string. `amsg capture-chord`
blocks until you press something and then prints
`{"ok":true,"chord":"SUPER+SHIFT+Q"}`.

**A deferred reply, not a watch.** The request arrives, nothing is sent, and the
answer goes out when a key is pressed — so an ordinary request/reply client works
unchanged. That the reply *can* be deferred is a property of the output queue:
replies are queued and the connection closes when the queue drains, not when the
handler returns.

It has to be here and not in the client. **The compositor takes bindings before
the focused surface sees them**, so a settings window reading its own key events
would receive everything *except* the combinations that are already bound — which
is precisely the set you reach for when rebinding. `Super+Q` would close a window
instead of being captured.

- A bare modifier keeps the capture open. Holding Super is the beginning of a
  chord, not one.
- The captured chord is **swallowed**: a captured `Super+Q` that also ran
  `kill_client` would be a keybind editor that closes the window you are editing
  from. The binding works again as soon as the capture ends.
- Unmodified `Escape` cancels, answering `{"ok":false,"error":"cancelled"}`.
  `Shift+Escape` is a chord you can still bind.
- A second client gets `busy` rather than being queued.
- The key name is whatever xkb calls it (`Delete`, `XF86AudioRaiseVolume`), which
  is exactly what `parse_key` reads back — so the string round-trips by
  construction rather than through a table kept in step by hand. A key with no
  keysym under the current layout comes back as `code:<n>`, which is a spelling
  the parser already accepts.

#### set-window-rules and set-binds, for writing those back

```
set-window-rules {"changes":[{"op":"update","index":3,
                              "fields":{"appid":"mpv","force_hdr":"1"}},
                             {"op":"add","fields":{"appid":"kitty","isterm":"1"}},
                             {"op":"remove","index":5}]}

set-binds {"changes":[{"op":"add","chord":"Super+X","action":"spawn",
                       "args":["kitty"],"flags":{"release":true}}]}
```

`index` is the position reported by `get window-rules` / `get binds`. An `update`
replaces the declaration wholesale, so it must carry every field that should
survive — there is no merge, because a merge cannot express "remove this field".

**Nested KDL is written and the existing reader flattens it.** The writer never
learns the legacy `windowrule=` comma form: a `window-rule { … }` block goes into
the file, and on the next read the existing `kdl_window_rule` →
`windowrule=` → `parse_option` chain consumes it. One parsing path, and the thing
written is the thing read.

Edits are span replacements, so comments survive and a removal takes the comment
lines directly above it — those are its explanation, and orphaning "// keep mpv on
top" above an unrelated rule is worse than losing it.

**Several edits to one file are applied back to front.** Every splice moves every
offset after it, so front-to-back would leave the second span pointing into the
middle of whatever the first edit produced — and the result still parses, which is
what makes it a bug that ships rather than one that is caught.

Errors: `unknown-field`, `unknown-action` (a bind naming a dispatch the compositor
does not know is refused rather than written into a config that fails at the next
*login*), `not-editable` (no KDL node, so no span to replace), `read-only-source`,
`would-not-parse`, `write-failed`. The batch is all-or-nothing and a change that
never ran reports `not-applied` rather than success.

There is no `persist` flag. A window rule takes effect when a window maps and a
keybind is a lookup, so there is nothing to preview; and the arrays behind both
are rebuilt wholesale on read, so "apply in memory" would mean reimplementing the
reader. The config is re-read afterwards **without** respawning the `spawn` list —
`reload_config` does that because a reload is a reload, but saving a keybind is
not, and on a real machine the exec list is an `xrdb -load`.

#### set-config, for writing it back

```
set-config {"changes":[{"path":"effects/blur/radius","value":"8"},
                       {"key":"cursor_theme","value":null}],
            "persist":true,"override":false}
```

A **verb**, not a dispatch, for three reasons visible in `handle_command`: it
rewrites every `,` to a space in its `cmd[1024]` copy and the dispatch branch
then tokenises on commas with a six-token cap, while values legitimately contain
commas; `cmd[1024]` truncates and a batch of twenty changes is a couple of
kilobytes; and dispatch's reply is a hardcoded `{"success":true}` with the return
value discarded, when real errors are the entire point.

`dispatch set_option` still exists and still works. It changes a value in memory
and writes nothing, so a reload discards it — which makes it a preview, not a
setting.

A **batch**, because that is how a settings panel works: you touch six controls
and press Apply, and either all six take effect or none do. So the order is
*resolve everything, validate everything, and only then touch anything.* A change
that never ran reports `error: "not-applied"` rather than `ok` — a UI told "ok"
for something that did not happen will display the wrong value.

Out-of-range values are **refused, not clamped**, and the reply carries the
bounds. A UI has the schema and can bound its own controls; clamping silently is
how a panel ends up showing 9999 while the compositor runs 200.

`persist: false` is the live-preview path: applied in memory, nothing written, and
`source.kind` comes back as `runtime` so a panel can show that it will not
survive. `persist` defaults to **true** — a caller that means preview says so.

It is designed to be sent at interactive rates. A previewed key keeps its file
provenance (see above), so the eventual `persist:true` edits the declaration in
force rather than a fresh one, and a previewed generated key is still refused.
Undo a batch of previews with `dispatch reload_config`, not by writing the old
values back — writing them back leaves the values right and the provenance wrong,
so a key that *is* saved reads as `runtime` from then on.

`value: null` removes the declaration and reverts to the compiled-in default.
Removing it rather than writing the default value matters: a declaration set to
the default is still a declaration, and would still win over anything `source`d
after it. With `persist: false` it previews the removal — the value becomes the
default in memory while the line stays in the file — so a later `persist: true`
for the same key is what actually deletes it.

**A key whose value comes from a generated file is refused**, with
`error: "read-only-source"` and the reason. `override: true` then appends a
shadowing block to the *main* config — never to the generated file — with a
comment saying what it shadows and why it wins. Refusing forever is a dead end;
overriding silently is worse.

Writes are surgical, so comments survive being written around, and every staged
document is **re-parsed before anything is renamed into place**: "Apply must never
leave me with a config that does not load" is what this owes a file someone
maintains by hand. A `<file>.bak` is kept, because one Apply can rewrite a dozen
options across two files and "undo the last apply" should not require having
thought about it first.

True atomicity *across* files is not achievable with `rename(2)` and a journal is
not worth it here: a crash between two renames leaves one file updated, which is
survivable, where an unparseable config is not.

Errors: `bad-request`, `unknown-key`, `bad-value`, `out-of-range` (with `min`
and `max`), `read-only-source` (with `file` and a hint), `no-writable-file`,
`would-not-parse`, `write-failed`, `not-applied`.

`amsg set-config @-` reads the body from stdin, which avoids quoting JSON through
a shell and lifts amsg's 4KB argument cap:

```bash
printf '{"changes":[{"key":"borderpx","value":"3"}]}' | amsg set-config @-
```

### WATCH (Event Subscription)
Subscribes the client to real-time updates. When the state changes, the server pushes a new JSON object to the output stream.

* `watch monitor <name>`
* `watch focused-client`
* `watch client <id>`
* `watch tags <mon_name>`
### `get avk-stats`

Live counters for the AVK renderer (`ASTEROIDZ_RENDERER=avk`). Returns
`{"backend":"scenefx","active":false}` when AVK is not the renderer.

```bash
amsg get avk-stats | jq
amsg dispatch reset_avk_stats     # zero the counters without restarting
```

The reset exists because benchmarking a workload should not require restarting
the compositor, which destroys the workload. It zeroes accumulating counters
only; `client_images_cached` and `output_targets_in_flight` describe the
present rather than an interval and are left alone.

Fields worth knowing:

| field | meaning |
|---|---|
| `wl_compositor_has_renderer` | false in AVK mode — wlroots uploads no client buffers |
| `dmabuf_zero_copy` / `dmabuf_implicit_fallback` | imports that cost nothing, versus imports that cost a copy |
| `implicit_copy_bytes` / `implicit_copy_us` | what that copy actually costs |
| `commit_imports` / `late_imports` | content taken at commit, versus discovered at a frame. `late_imports` must be 0 |
| `damage_ratio` | `damage_pixels / output_pixels` over the run. 1.0 means every frame is a full redraw |
| `cpu_sync_waits` | must be 0 — a nonzero value means the frame path blocks on the GPU |
| `present_sync_timeline` / `present_sync_dmabuf` | frames handed to the display with a fence, by which route |
| `present_sync_none` | **must be 0.** Frames handed over unsynchronised — only reachable via `AZ_AVK_NO_PRESENT_SYNC=1` |
| `present_sync_failures` | a fence could not be attached, so the frame was dropped and SceneFX rendered it instead |
| `presentation_waits` | GPU-side waits before reusing a target. Not a stall — the CPU returns immediately. Rare on the timeline route; ~1 per frame on the dma-buf route, where the buffer's fence list also holds our own last write |
| `target_state_violations` | must be 0 — the swapchain handed back a buffer the display had not released |
| `validation_errors` | Vulkan validation errors seen this run (needs `ASTEROIDZ_VK_DEBUG=1`) |

`present_sync_timeline + present_sync_dmabuf` should equal `frames`. Which of
the two is nonzero depends on the backend, not on a setting: a DRM backend with
`DRM_CAP_SYNCOBJ_TIMELINE` uses timelines, and everything else — including every
headless test run — puts the fence on the target's own dma-buf. Both are
correct; only the first lets KMS wait on the fence in the atomic commit itself.

Anything not yet measured is reported as `null`, not `0`. A zero is a
measurement; a null is an admission. `gpu_frame_us` is null today because
GPU timestamp queries are not yet recorded.

* `watch all-monitors`
* `watch all-tags`
* `watch all-clients`
* `watch idle`
* `watch keymode`
* `watch keyboardlayout`
* `watch last_open_surface [<mon_name>]`
* `watch bar-config`
* `watch config`

*Example:*
```bash
# watch all monitors
amsg watch all-monitors
# watch all tags
amsg watch all-tags
```

#### watch config, and why it is a diff

`watch config` pushes only what **changed**, as `{"reason": ..., "changed": {...},
"count": n}`. The first push after subscribing is everything, with
`reason: "initial"` — a client joining mid-stream has nothing for a diff to apply
to. After that, `reason` is `reload` or `set`.

A diff rather than the whole set because matugen fires on every wallpaper change
and rewrites nine keys out of ninety-five. And a change that changes nothing
pushes **nothing at all**: the palette often lands on the same colours, and
waking a settings panel to tell it so is the difference between a push and a
poll.

The previous values are compared **through the schema**, field by field — never
as a `memcmp` of the `Config` struct, which holds heap pointers and padding and
would report a change on every reload for neither reason.

The snapshot advances whether or not anyone is listening. Otherwise the first
subscriber after a quiet period would be diffed against a state from before
however many changes happened while nobody was watching, and told about all of
them as if they had just occurred.

#### bar-config, for a bar that is not the compositor

`bar-config` answers with what the compositor **resolved** -- after defaults,
after clamping, after the theme file -- rather than with the config text:

```json
{
  "theme": { "fg": [1,1,1,1], "bg": [...], "focus_bg": [...], "urgent": [...],
             "border": [...], "border_width": 4, "corner_radius": 5,
             "padding_x": 16, "padding_y": 0, "font": "Ubuntu 16" }
}
```

It used to carry the bar's whole appearance too -- its height, its pills, its
panel, its popovers, its module lists, its idle timeouts, its plugins. Sixty-two
values the compositor stored, clamped, described and served, and never once
read, left behind when the drawing moved out of this process. They live in
`~/.config/asteroidz-bar/config.kdl` now, and the `bar {}` block is gone from
the compositor's config entirely.

Colours are `[r,g,b,a]` floats, not hex strings: CSS reads `#RRGGBBAA` and Qt
reads `#AARRGGBB`, and a string that parses under both conventions while
meaning different things is a bug that surfaces months later as "the bar is
slightly the wrong colour".

The theme stays here because it is genuinely shared -- titlebars and the
overview draw from the same palette -- and because handing a bar the config
file to parse would be two KDL readers that agree until one of them gains a
default, and it still would not see the palette, which matugen rewrites at
runtime whenever the wallpaper changes. The compositor is the only process that
knows what the theme currently *is*.

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

Three pieces do this, and they are separate because only the first is specific
to outputs:

| | |
| :--- | :--- |
| `src/common/kdl-edit.h` | finds an `output NAME { … }` block as text and rewrites entries in it |
| `src/common/kdl-write.h` | the same idea at an arbitrary nested path (`layout/border/width`), locating with the real parser rather than a text scan |
| `src/common/kdl-file.h` | slurp, and replace-by-rename with both the file and its directory fsync'd |

`kdl-write.h` **parses to locate and edits bytes to mutate**. The parser hands
back a byte span per node (`KdlSpan` in `src/config/kdl.h`), so the writer never
has to reason about comments, `/-` slashdash, `;` terminators or nesting — the
thing that already knows how to read KDL does the reading. It resolves a
duplicated path to the **last** declaration, because `source` is applied in
place and later declarations win, so the last one is the value actually in
force. And it honours the bare-name fallback the config front-end uses, so
`misc { border_radius 9 }` is edited where it sits rather than duplicated at the
canonical top-level path.

Nothing calls `kdl-write.h` yet; it is the foundation for writing arbitrary
options, not a feature on its own. `tests/test-kdl-write.c` is a `meson test`
and includes a corpus case that applies 500 edits to the shipped
`assets/config.kdl`, asserting after each one that the document re-parses *and*
that the value written reads back — "it still parses" alone would pass on a
writer that dropped the edit.

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
