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
### `get dmabuf-feedback`

What the compositor advertises to clients, and the two sets it is derived
from, so the subset invariants can be asserted from outside rather than taken
on trust:

```json
{ "source": "avk",                 // or "wlr_renderer" in GLES mode
  "main_device": "226:128",        // AVK's DRM node, not the renderer's
  "advertised_pairs": 123, "withheld_pairs": 15,
  "avk_texture_pairs_probed": 138,
  "advertised_composition": ["XR24 (0x34325258):0x0200000028a01b04", ...],
  "avk_importable":         [...],  // rebuilt from the table, not a copy
  "outputs": [{"name":"DP-1","kms_scanout":[...],"advertised_scanout":[...]}] }
```

The invariants a test should hold it to:

```text
advertised_composition  ⊆  avk_importable
advertised_scanout      ⊆  avk_importable ∩ kms_scanout
advertised + withheld   =  avk_texture_pairs_probed
DRM_FORMAT_MOD_INVALID  ∉  advertised_composition
```

`avk_importable` is rebuilt from the format table at query time rather than
copied from what was advertised — otherwise the subset check would be
comparing a set with itself.

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
| `full_redraw_frames` / `partial_redraw_frames` | how many frames redrew the whole output versus part of it |
| `damage_rects_max` | most rectangles one frame's damage arrived in. Pinned at 20 means the ring is collapsing damage to a bounding box rather than tracking it |
| `shm_commits` | content generations committed for CPU-backed buffers |
| `shm_full_uploads` / `shm_partial_uploads` / `shm_upload_skips` | whole-buffer copies, damaged-region copies, and lookups that found the GPU copy already current. On a mostly-static desktop skips should dwarf both |
| `shm_damage_pixels` | source pixels clients said they changed. `x4` is the minimum bytes a correct implementation could copy |
| `buffer_resolve_attempts` / `nodes_output_culled_before_resolve` | buffers the walker resolved, versus nodes discarded first because they cannot touch this output. The second reads 0 on a single-output setup by definition |
| `cpu_frame_us_p50` / `_p95` / `_p99` | the distribution, not the mean. At 144 Hz the budget is 6944us. Reported as a histogram bucket's upper edge, so accurate to within 20us |
| `shm_sources` | the largest CPU-backed sources by bytes moved, for finding a pathological client. **Lifetime counters — `reset_avk_stats` does not clear them**, unlike everything else here, because they describe a buffer's life rather than an interval |
| `software_cursor_frames` / `hardware_cursor_frames` | frames AVK composited a cursor into, versus frames the hardware plane carried it. The plane is preferred and costs AVK nothing, so on an ordinary desktop the second should dominate |
| `cursor_commands` | cursor draws emitted. At most one per output frame |
| `cursor_no_image` | **must be 0.** wlroots says a cursor is enabled and visible and asteroidz has no picture to draw for it — the exact fingerprint of the regression M3.5E fixed, where a client's own cursor image was silently dropped |
| `cursor_import_failures` | the cursor image would not go to the GPU. Also expected to be 0 |
| `cursor_culled` | cursors discarded as entirely outside this output. Defensive only — `wlr_output_cursor.visible` is per-output and wlroots computes it, so a cursor on another monitor is skipped as not-visible before this test is reached. A permanent 0 is correct, not missing coverage |
| `cursor_moves` | frames whose cursor box differs from the previous frame's. **Not** a count of pointer motion events — several can land inside one frame — because what matters here is what drives damage |
| `record_us_avg` | CPU wall clock `avk_render_frame()` spends recording and submitting, per frame. **Not GPU time** — that is `gpu_frame_us_avg`, and the two are separate fields because one number would understate a shader-bound frame and overstate a submission-bound one |
| `gpu_frame_us_avg` | mean GPU frame time over completed timestamp samples, or null where the queue family cannot write timestamps. A mean over *every output's* frames, since a renderer is shared by every output using its `VkFormat` |
| `gpu_samples` / `gpu_dropped` | timestamp pairs read back, and pairs lost because their ring slot came round before anyone collected them. A gap in statistics, not a correctness problem |
| `graph_passes` / `graph_resources` / `graph_uses` | the render graph of the **last frame each renderer built**, not a run total — these are reset per frame. On the direct path `graph_passes` is 1, however many windows are on screen |
| `graph_barriers` | `vkCmdPipelineBarrier2` **calls** — the thing that costs a pipeline flush. 2 on a real output: the acquire batch before the pass, and the foreign release after it |
| `graph_image_transitions` | `VkImageMemoryBarrier2` structures inside those calls that change a layout |
| `graph_buffer_barriers` | **zero by construction.** The graph declares image resources only; the gradient buffer and the command buffer live in a per-frame slot the command ring has already waited on, so there is no hazard for a barrier to resolve. Reported so the absence is a stated fact rather than an omission |
| `graph_allocs` | cumulative heap allocations by graph construction. **Must stop rising once the scene settles** — the flat-array design exists to make that true, and reading this twice a few seconds apart is the whole test |
| `graph_build_ns_avg` | the graph's **own bookkeeping** per frame: walking uses, comparing state, deriving barrier structs. Both the pass record callbacks *and* `vkCmdPipelineBarrier2` are subtracted, because neither is work the graph added |
| `graph_barrier_ns_avg` | CPU time inside `vkCmdPipelineBarrier2`. **Not graph overhead** — the pre-graph renderer made the same two calls and their cost had nowhere to be attributed. Reported separately because it dominates, and because it varies with what the barrier *does*: a queue-family ownership transfer on a 4K scan-out buffer costs far more than a layout transition on a headless one |
| `transient_acquires` / `_reuses` / `_creates` | the transient image pool. **Zero until M4F**, which is its first consumer; the pool is present and collected every frame so that a lifetime bug and an effect bug cannot arrive together |
| `transient_unsafe_reuses` | **must be 0.** Images handed out before the GPU finished with them. The only path that can raise it is the `transient-early-reuse` break |
| `transient_live` / `_bytes` / `_peak_bytes` / `_retires` | pool occupancy, memory held, high-water mark, and entries retired for being idle or over budget |
| `cursor_damage_pixels` | area AVK drew for the cursor. Two 32x32 boxes per moving frame is the expected shape |
| `cursor_hw_to_sw` / `cursor_sw_to_hw` | plane handovers in each direction. A recorder starting and stopping produces one of each |
| `cursor_client_surface_sets` / `cursor_shape_sets` / `cursor_xcursor_sets` | which of the three sources asked for the image. They are three different paths through wlroots and only one was ever broken, so a session with 0 client sets has not exercised what M3.5E fixed |
| `cursor_unsets` | `set_cursor(NULL)` — a client asking for no pointer image. Distinct from the idle-timeout hide, which is the compositor's decision and reversible without a client request |
| `cursor_forced_reimports` | same buffer, new pixels, so the image had to be cleared before `wlr_cursor_set_buffer()` would take it again. Nonzero only for a client animating a cursor into one reused `wl_buffer` |
| `cursor_client_no_buffer` | a client called `wl_pointer.set_cursor` with a surface carrying no buffer. Legal — it is one of the ways to hide a pointer — but it was the leading suspect for XWayland's cursor never changing shape, and measuring it at **0** across 88 seconds of real work is what ruled that out. Distinct from `cursor_unsets`, which is a NULL *surface* |
| `cursor_force_software` | whether `ASTEROIDZ_AVK_FORCE_SOFTWARE_CURSOR=1` is in effect |
| `cursor_geometry_mismatch` | **must be 0.** Frames where `wlr_output_cursor`'s box did not describe the image AVK was about to draw into it — `image_size / az_cursor.scale * output_scale`. Nonzero means something selected a cursor through wlroots' own `wlr_cursor_set_xcursor()`, so the pixels come from one owner and the geometry from another. Only observable with software composition and only visible at all on a layout with **mixed scales**, which is why it shipped: dragging a window showed no grab cursor and resizing one on the coarser output made the arrow bigger |
| `pointer_enters` / `pointer_motions` / `pointer_focus_clears` | `wl_pointer.enter`, `.motion` and clear-focus notifications sent to clients. Not renderer counters at all: they exist because a flicker that looks like a compositing bug can equally be a client being told the pointer moved 20 times a second while it sits still, and nothing else in the system distinguishes those. Expect `pointer_enters` to track `pointer_motions` **exactly** — the code enters then moves every time, and wlroots no-ops the enter when the surface already has focus — so a divergence is the interesting reading, not the equality |
| `pointer_notify_internal` | pointer notifications the compositor generated for itself (`motionnotify()` with no input event behind it), as `arrange()` does after a layout change. The one number that separates "the user moved the mouse" from "the compositor said they did"; it stayed flat through a flicker burst and killed a whole line of investigation |
| `cursor_source_commits` / `cursor_source_uploads` / `cursor_source_upload_bytes` / `cursor_source_upload_skips` | the current cursor image's own upload history, read straight off the same per-buffer record every other client buffer uses. This is "position changed != pixels changed" in four numbers: dragging a cursor for thirty seconds must move the skip count and leave commits, uploads and bytes where they were. Absent when the cursor has no image |
| `cpu_sync_waits` | must be 0 — a nonzero value means the frame path blocks on the GPU |
| `present_sync_timeline` / `present_sync_dmabuf` | frames handed to the display with a fence, by which route |
| `present_sync_none` | **must be 0.** Frames handed over unsynchronised — only reachable via `AZ_AVK_NO_PRESENT_SYNC=1` |
| `present_sync_failures` | a fence could not be attached, so the frame was dropped and SceneFX rendered it instead |
| `presentation_waits` | GPU-side waits before reusing a target. Not a stall — the CPU returns immediately. Rare on the timeline route; ~1 per frame on the dma-buf route, where the buffer's fence list also holds our own last write |
| `target_state_violations` | must be 0 — the swapchain handed back a buffer the display had not released |
| `validation_errors` | Vulkan validation errors seen this run (needs `ASTEROIDZ_VK_DEBUG=1`) |
| `lifecycle_violations` | **must be 0.** Every AVK resource caught being double-owned or double-destroyed: an image destroyed twice, a pointer handed to the retire queue while already in it, a queue drained with the GPU still behind its entries. This is the counter to assert on, because the alternative — asserting that no glibc abort happened — is also satisfied by code that never ran |
| `retire_duplicate_pushes` | pushes refused because the resource was already queued for destruction. A subset of `lifecycle_violations`, broken out because it names the specific failure that produces a double free |
| `live_images` / `live_image_views` / `live_device_memory` / `live_buffers` / `live_descriptor_pools` / `live_command_pools` / `live_semaphores` | a running census of the Vulkan objects AVK owns. Read mid-session they say what is outstanding right now; at `vkDestroyDevice` every one of them must be 0, which the compositor logs rather than reports here (nothing can query a device that is being destroyed). **Signed**, deliberately: a negative count is a double destruction, and a double destruction of a driver object is a double free of the driver's host allocation |
| `live_avk_images` / `live_avk_uploads` / `retire_entries_live` | the same census for AVK's own host-side wrappers |

`frames` counts **output** frames, not compositor-wide ones: a two-monitor
desktop increments it twice per refresh. Divide byte counters by it accordingly.

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
