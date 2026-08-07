# Headless regression test harness

`contrib/regression/` is a data-driven, assertion-based test suite for
asteroidz's window-management/IPC dispatch surface — the counterpart to
[`contrib/anim-test.sh`](./anim-testing.md), which is for *visual* rendering
regressions instead. Where the anim harness produces a montage/recording for
a human to inspect, this one boots one isolated headless compositor instance
and runs TAP-style pass/fail assertions against it via `amsg`, so it can gate
a change automatically.

## Usage

```sh
bash contrib/regression/run.sh              # every module
bash contrib/regression/run.sh layouts tags  # just these modules
```

There is a second, much faster layer beside it: `meson test -C build` runs unit
tests for the pure helpers — the ones whose failure mode is not "the screen
looks wrong" but a corrupted file on disk or a reply that arrives with its tail
missing. Those cannot be reached by driving a compositor, because the bug is
invisible at the sizes and shapes a running compositor produces:

| Target | Covers |
| :--- | :--- |
| `kdl-edit` | rewriting an `output NAME { … }` block without disturbing the comments around it |
| `kdl-write` | editing an arbitrary nested path; includes a 500-edit corpus run over the shipped `assets/config.kdl` |
| `ipc-out` | queued socket writes, against a socketpair with `SO_SNDBUF` shrunk to force the partial write a normal reply never triggers |
| `config-schema` | `asteroidz -S`: every schema entry's default, clamp, offset and type, against the real parser |
| `config-schema-coverage` | the other direction — keys `parse_option` handles that the schema is missing |
| `dispatch-actions-coverage` | the same both-ways check for the dispatch-action table a keybind editor reads |
| `rule-schema-coverage` | window-rule keys `parse_option` handles that `rule_schema[]` is missing — the direction `-S` structurally cannot see |
| `shipped-config` | `assets/config.kdl` parses. It becomes `/etc/asteroidz/config.kdl` and is what every new user copies; nothing was checking it, and it had drifted to the point where `asteroidz -p` exited 1 |
| `bar-icons` | every vendored SVG parses and rasterises to non-empty ink |

Run both before pushing. Neither subsumes the other.

There is a third, one-purpose script beside them: `contrib/signal-exit-test.sh`
asserts that SIGTERM and SIGINT each exit the compositor within five seconds. It
cannot live in `run.sh`, because every module there shares one compositor and
this one's whole job is to kill it. It exists because `handlesig()` once called
`quit()` — the *asking* one — so a signal raised the exit-confirmation prompt and
then waited for a keystroke: logout stalled until systemd's SIGKILL, and every
regression module hung after printing its summary and leaked its compositor.
`hl_stop` now bounds its own wait and says so loudly rather than hanging.

`contrib/titlebar-sharpness-test.sh` is the other one-purpose script. It asserts
that a titlebar is *rasterised* at its output's scale rather than drawn at
logical size and resampled up, which no IPC assertion can see — the geometry is
identical either way and only the pixels differ. It measures soft ramps per
steep edge across the titlebar band: text rendered at the panel's own
resolution goes background-to-ink in one step and scores about 0.8, while the
same text resampled up arrives as a ramp across two or three pixels and scores
about 16. Counting mid-tones instead does not separate the two — the background,
borders and icon supply plenty either way, and the first version of this
measured 22.2 against 20.5 and could not tell the builds apart.

### The schema, checked from both ends

`src/config/config-schema.h` describes every settable option — type, range, enum
members, default, and a human label and description — so a settings UI does not
have to carry a hardcoded mirror of the compositor's parser. It is hand-written,
because the failure modes are not symmetric: a wrong *generator* produces a wrong
schema silently and the UI then writes wrong values into someone's config, where
a wrong *checker* produces a red test.

So there are two checkers, and they cover opposite directions.

`asteroidz -S` runs without a compositor, the same way `-p` does, and drives the
real `set_value_default`, `override_config` and `parse_option`. It parses no C at
all. For every entry it asserts the default matches what the code produces, that
a value past each bound lands on the bound, that poisoning the field and writing
the default actually changes it (which catches a renamed key — the `if/else`
chain silently ignores one it does not know), and that a value survives a round
trip. On its first run against a hand-written table it found 21 wrong defaults
and three wrong types, including a `float` field described as an `int`.

Two details in there are load-bearing and easy to get wrong:

- **Clamps are derived from behaviour, not from where they are written.** They
  live in both `parse_option` (`blur_transparency_threshold`) and
  `override_config` (`borderpx`), so a check that looked in one place would
  report half the table as unclamped.
- **The poison value has to be inside the valid range.** An out-of-range
  sentinel gets clamped by `override_config`, which changes the field on its
  own — so the reachability check passed for a deliberately renamed key, and only
  the round-trip assertions noticed. Renaming a key in the table is now caught
  by the check that exists for it.

`tests/check-config-schema.py` covers what `-S` structurally cannot: a key that
exists in `parse_option` and is simply **absent** from the table, which has no
entry to run a check against. It takes the described keys from the binary
(`asteroidz -L`) rather than by regex over the C table — an over-matching pattern
reported 100 keys where there were 95, which would have hidden five omissions —
and the handled keys by parsing `parse_option` at brace depth 1, so the sub-keys
inside the `windowrule`/`monitorrule`/`tagrule` branches are not mistaken for
standalone options.

Exemptions live in `tests/schema-exempt.txt` and must sit under a `## reason:`
heading; a bare list of exempt keys is an escape hatch nobody has to justify,
which is not one. The checker also fails on a key that is both described and
exempt, and on an exemption for a key `parse_option` no longer handles — either
would let the next real omission hide behind a stale line.

Coverage today: `parse_option` handles 231 keys, of which 95 are described. Of
the 136 exempt, 17 are structural (rules, binds, lists, directives) and **119
are simply not described yet** — listed key by key so the gap is auditable and
so adding one is deliberate. The file shrinking is the measure of progress, and
the largest single drop so far was not describing anything: 55 of those keys
configured a bar the compositor stopped drawing, and deleting them beat
documenting them.

Env: `ASTEROIDZ` (binary under test, default `build/asteroidz` next to the
repo, falling back to `/usr/bin/asteroidz`), `HL_OUTDIR`, `HL_WIDTH`/
`HL_HEIGHT`.

`HL_RENDERER` picks the renderer — `gles2` (default) or `vulkan`. gles2 is
the default because it is the one renderer present on every machine that runs
this suite, so results stay comparable; set `vulkan` to exercise the `fx_vk`
path, which is what a real session here actually uses. Editing the launch line
in `headless.sh` by hand does **not** work once the file is sourced: the
function body is already parsed, and doing so silently produced a second gles2
run labelled as vulkan.

`HL_ENV` passes `NAME=VALUE` pairs through to the compositor. The launch uses
`env -i` so a test instance can never inherit the caller's session — which
also means an exported variable does not reach it. `FX_VK_VALIDATION` and
`MESA_VK_TRACE` both looked like the driver was ignoring them when in fact
they had never arrived:

```sh
HL_RENDERER=vulkan HL_ENV="FX_VK_VALIDATION=1" bash contrib/regression/run.sh
```

Set `HL_ALLOW_DESTRUCTIVE=1` to also run
`destroy-virtual-output.sh`, which is skipped by default (it destroys every
headless output, including the original one — safe in isolation, but not
worth risking in a shared run).

## Live-session mode (extreme caution)

`HL_LIVE=1` attaches to the *caller's own already-running* compositor instead
of launching an isolated instance (`hl_start_live` in `contrib/lib/headless.sh`,
requires a valid `ASTEROIDZ_INSTANCE_SIGNATURE` already in the environment).
By default every dispatch is still confined to a fresh virtual/headless
output this creates on the fly — real outputs are never touched. Set
`HL_LIVE_MON=<name>` (e.g. `DP-1`) to instead run directly against that
*real, physically-connected* monitor, disturbing whatever's actually
displayed there for the duration.

This is not a routine testing mode. Real-world experience running it:
a full test run against a live session found and fixed a genuine
use-after-free segfault (`asteroidz_icon_node_set`, `src/draw/text-node.c`)
and, separately, a frame-scheduling bug (`monitor_check_skip_frame_timeout`,
`src/asteroidz.c`) that could freeze the compositor's real output and leak
memory unboundedly under continuously-updating real content (e.g. video)
with `animations` disabled — the second one froze the whole display and
required a hard reboot to recover, on real monitor hardware, in a matter of
seconds. Neither bug reproduced headlessly under any combination tried.
`hl_notify()` fires desktop notifications (via `notify-send`, live mode
only) at start/per-module/finish so a live run is never silent — this
does not make it safe, only visible. Treat any live-mode run, real-monitor
or virtual, as needing fresh explicit sign-off every single time, never a
standing permission, and prefer chasing anything it turns up via
`coredumpctl`/static review afterward (as with both bugs above) rather than
reproducing it live again.

## How it works

`contrib/lib/headless.sh` is the shared library: `hl_start` launches one
fully isolated headless compositor instance (own `XDG_RUNTIME_DIR`, Wayland
socket, `ASTEROIDZ_INSTANCE_SIGNATURE`) plus a flat-color `swaybg` wallpaper,
never touching your real session. `hl_dispatch`/`hl_get` wrap `amsg
dispatch`/`amsg get` scoped to that instance; `hl_watch_start` backgrounds an
`amsg watch ...` stream for asserting on IPC notifications. `hl_spawn_kitty`/
`hl_spawn_wllayer`/`hl_spawn_wlkeys` spawn tracked, throwaway test clients.
`hl_reset` kills
spawned windows and returns to a known state (tag 1, tile layout, `HEADLESS-1`
focused) between test cases so they can't leak state into one another.
`hl_assert`/`hl_assert_eq`/`hl_assert_true`/`hl_assert_false` are the
pass/fail primitives, tallied globally; `hl_summary` prints totals.

`hl_start` also brings up a **private `dbus-daemon --session`** and points the
compositor at it, because the portal backends (global shortcuts, inhibit) live
on D-Bus and without a bus they are not there to test. The real user bus is the
wrong answer twice over: your live session already owns
`org.freedesktop.impl.portal.desktop.asteroidz`, so a test instance could never
take the name — and if it somehow did, a headless compositor with no screen
would be the thing answering Discord's push-to-talk. `hl_busctl` runs `busctl`
against that private bus, and returns non-zero when there is none, which is
what lets `inhibit-portal` skip itself on a machine with no `dbus-daemon`
installed rather than fail. The daemon is killed by recorded PID in `hl_stop`,
never by pattern.

A test that needs a daemon the bar talks to should **stand one in** rather
than let the real one start: the voice-menu test serves a synthetic snapshot
over the instance's own socket with `socat` and sets `discord { daemon-cmd
"" }`, because the module otherwise spawns `discord-voiced` — with your real
token — into every isolated instance that has no socket. Skip cleanly
(`command -v socat` etc.) when an optional tool is missing.

`contrib/regression/run.sh` boots one shared instance and runs every
`test_*` function from `contrib/regression/tests/*.sh` against it, in file
order, with `hl_reset` between each. Extend coverage by adding a new
`tests/<area>.sh` file with `test_*` functions, not a bespoke one-off script.

## Custom test clients

None of `kitty`/`wlvptr`/`wlvkbd` can reach every corner of the compositor,
so the harness includes a few small purpose-built Wayland clients:

- **`contrib/wlvptr`** — `wlr-virtual-pointer-unstable-v1` client for
  synthetic pointer input (click/scroll/drag), scoped to whichever
  compositor `WAYLAND_DISPLAY` points at (unlike `ydotool`, which is
  uinput/kernel-level and routes to whatever seat is active system-wide —
  not safe to use against a headless test instance).
- **`contrib/wlvkbd`** — `zwp_virtual_keyboard_unstable_v1` client for real
  key press/hold/release sequences (`wlvkbd hold KEY... -- COMMAND`), for
  testing input-path behavior that bare IPC dispatch can't reach — e.g. a
  chord/combo keybind whose state only resets on a genuine key-release
  event, or a Super+drag mouse binding (hold a modifier while a nested
  `wlvptr ... drag:x,y` runs).
- **`contrib/wllayer`** — a minimal `wlr-layer-shell-unstable-v1` client
  (layer/anchor/exclusive-zone/keyboard-interactivity/size all configurable
  via CLI args, plus an optional scripted resize-in-place) for layer-shell
  edge cases: exclusive-zone reservation, stacking across layers, and
  regression-pinning past bugs (a DPMS/disabled-monitor layer-configure bug,
  and the original stale-shadow-after-resize bug) directly instead of only
  inferring them through the waybar popup harness.
- **`contrib/wlkeys`** — an `xdg-shell` toplevel that REPORTS keyboard events
  back out (one line per `enter`/`leave`/`key`/`modifiers`, raw evdev
  keycodes). Every other client here drives input *in*; this is the only one
  that observes what the compositor *sent*, which is the only way to assert on
  `wl_keyboard.enter`'s held-key array. That array is how a tag-switch binding
  used to leave a Proton game repeating a key forever: the client is told the
  bound key is held at the moment focus arrives, and the release that would
  stop it is deliberately swallowed. Use `hl_spawn_wlkeys` and
  `hl_wlkeys_last_enter`.
- **`contrib/portal-inhibit-client.py`** — not a Wayland client at all: a D-Bus
  one that takes an `org.freedesktop.impl.portal.Inhibit` request and then
  *stays connected*. `busctl` cannot test that interface, because an inhibition
  lives only as long as the connection that asked for it — asteroidz watches
  for the owner dropping off the bus so a crashed `xdg-desktop-portal` cannot
  leave the machine awake forever, and `busctl` exits the instant its call
  returns, so every inhibition it takes is reclaimed a millisecond later,
  correctly and uselessly. It speaks the *impl* interface directly, standing in
  for what `xdg-desktop-portal` would send, so no portal daemon has to be
  installed or raced and the object paths are the test's to choose. `--monitor`
  prints each `StateChanged` as a JSON line.

## Module coverage

Thirty-four modules as of writing: `layouts`,
`window-states`, `tags`, `tag-rules-ipc`, `focus`, `scratchpad`, `geometry`,
`dwindle`, `overview`, `multimonitor`, `mousebind`, `hdr`, `scroller`,
`animations`, `layer-shell`, `ipc-watch`, `keybind-combo`, `set-option`,
`config-ipc`, `config-write`, `rules-ipc`, `border-colors`, `idle`,
`inhibit-portal`, `prompt`, `titlebar`, `output-reflow`,
`output`, `vrr`, `effects`, `floating`, `quit-confirm`, `screenshot-ui`,
`fullscreen-bleed`, plus `destroy-virtual-output` (gated
behind `HL_ALLOW_DESTRUCTIVE=1`).

`screenshot-ui` and `fullscreen-bleed` both need **two** monitors and skip
themselves on one, the way `multimonitor` does — each is about a boundary, and
with no neighbour every assertion in them would pass by default. `screenshot-ui`
measures the overlay's pointer confinement, which doubles as the only handle on
"is the overlay up" the socket offers; `fullscreen-bleed` compares pixels on the
neighbouring output before and after a fullscreen round trip, because the
geometry can be correct while the surface is drawn past it.

`inhibit-portal` is the only module that leaves Wayland entirely: it drives
`org.freedesktop.impl.portal.Inhibit` over the private bus described above, and
skips itself when there is no `dbus-daemon` or no PyGObject. Its assertions are
weighted towards **release** rather than acquisition, because a stub that
accepts an inhibition and does nothing and a backend that takes one and never
gives it back fail in the same direction — silently, hours later, on a flat
battery. So it closes a request while its client stays connected (the
application's path), `SIGKILL`s a client that is still holding one (the crash
path), and checks that releasing one client's inhibition does not release
another's. It also carries the one test for the refactor that let two backends
share a bus name: a global-shortcuts session must still close through the now
shared `Session` object, which is a failure that would otherwise stay invisible
until an app tried to close one.

`bar` is the pattern to copy for anything that needs a **different config**
than the shared one: it never turns the bar on globally (that would
shrink the usable area and silently break every geometry assertion in the
other modules), but rewrites `$HL_CONFIG` from a pristine copy and calls
`reload_config` per test, restoring it afterwards. It also skips itself when
the binary under test was built with `-Dbar-config=false`, probing the binary
rather than assuming — an unknown config key is only warned about, so a
feature-off build would otherwise silently "pass" by doing nothing.

`config-ipc` covers the settings-UI read surface — `get config-schema`,
`get config`, `get dispatch-actions` and `watch config` — through the socket,
because that is how a client sees them. `asteroidz -S` and `-L`/`-D` check the
tables against the code, but none of them go through IPC. Most of its assertions
are about whether the reply says something **true** rather than whether a field
exists: a schema with wrong defaults still parses, provenance naming the wrong
file still parses, and a watch that pushes the whole config every time still
parses. So it checks the reported line number really holds that setting, that a
colour's hex and floats agree on every channel, and that a no-op change pushes
nothing at all.

`rules-ipc` covers the same read surface for window rules and keybinds —
`get window-rule-schema`, `get window-rules`, `get binds`. What makes it worth a
module of its own is that both structures are **lossy once parsed**: a
`KeyBinding` is a function pointer and a union by the time it exists, and a
`ConfigWinRule` cannot say whether the file wrote `0` or wrote nothing. So those
verbs are served from records captured while reading, and the thing to check is
that the records say what the file said.

It covers the write path too — `set-window-rules` and `set-binds` — where the
sharpest assertion is that **a batch of edits does not shift itself apart**. Every
splice moves every offset after it, so applying edits in the order they arrive
leaves the second span pointing into the middle of what the first edit produced.
The result still parses, which is exactly what makes it a bug that ships. Edits go
back to front; reversing the sort turns that test red.

Its sharpest read assertion is that a rule which sets one field reports **exactly**
that field. A serialiser emitting all 53 would look correct in a diff and would
leave a rule editor unable to tell "leave blur alone" from "turn blur off" — and
would write the latter for every field on the first save.

It covers `capture-chord` too, whose sharpest assertion is that **a chord which is
already bound is still captured, and does not fire**. Both halves matter: the
first is the whole reason capture lives in the compositor rather than in the
settings window, and the second is what stops a captured `Super+Q` from also
closing the window you are editing from.

That test was order-dependent twice before it settled, which is worth recording.
It started on the harness's `F11 -> combo_view`, whose chord flag is
process-global and cleared only by a real key release — so whether it changed the
tag depended on whether `keybind-combo` had run first. Rebinding to `view` did not
help, because `view` ORs rather than replaces while that flag is set. It observes
a **config value** now, set through `set_option`, which has no state behind it at
all. Both earlier versions passed when the module was run alone.

It also found two silent bugs while being written, both of the same shape:

- **`kdl_binds` always passed the bare `bind`**, so `parse_bind_flags`' `s`, `l`,
  `r` and `p` were reachable only from the legacy line format. A `binds` block
  could not express a release binding at all, and the flag was discarded without
  a word. The test asserts the flag by **contrast** — one chord with
  `release=#true` and one without — because "release is true" alone would pass
  against a build that reported true for everything.
- **`#true` was not a boolean.** `#` is a legal bare-word character, so KDL v2's
  spelling parsed as the *string* `"#true"` and every consumer ran it through
  `atoi` and got `0`. Nothing in the tree writes v2 spelling, which is the only
  reason it never bit. Both spellings are accepted now and both are asserted.
- **The legacy comma form was swallowed.** `windowrule "appid:x,isfloating:1"` —
  what an old `windowrule=` line becomes — reached a handler that only read a
  node's children, so it produced a rule with no matchers. A rule with no matchers
  matches every window.

`config-write` covers `set-config`, which is the half that makes a setting a
setting rather than a preview. It **writes to `$HL_CONFIG`** and so restores a
pristine copy after every test, the way `bar.sh` does — modules run in name
order, which puts it ahead of `geometry`, whose assertions depend on gaps and
border widths a test here could have left changed.

Its sharpest case is the corpus one: writing *every* described option, group by
group, then asserting the config still parses **and** that a reload logs no
`Unknown keyword`. Writing one option proves the mechanism; writing all of them
proves the schema. That is what caught `theme/border-color` and
`animations/enable` claiming nested KDL paths that `kdl_key_map` had no entry for
— the write succeeded and the *next reload* rejected the file. `asteroidz -S` had
not caught it because every check there went through `parse_option` with the
internal key and none went `path → key`; it now does.

Its most valuable case was added afterwards, by the settings window:
`test_set_config_a_preview_does_not_lose_the_declaration`. `set-config` with
`persist:false` is the live-preview path, and it used to erase the file, line and
path a key was declared at — one line in `config_source_note`. Three separate bugs
came out of that, and all three only bite a caller that previews before it saves,
which is exactly what a settings UI does:

- a persisting write after a preview found no declaration, fell back to the
  canonical path in the main config, and appended a **second** one — leaving the
  user's `misc { border_radius 9 }` dead and a duplicate winning by position;
- a previewed removal had no line left to delete and reported success with the
  setting still in the file;
- a previewed `colors.kdl` key lost the origin that makes it read-only, so the next
  write went to `config.kdl` **without `override:true` ever being asked for** —
  the guard defeated by the thing it guards.

Only the second had a visible symptom, and it surfaced in a bar test rather than
here. Provenance now carries "set in memory" as a flag beside the file rather than
instead of it; `asteroidz -P` shows both as separate columns for the same reason.

Two traps it walked into while being written, both worth knowing:

- **An assertion must not change global state.** Proving a described action is
  really dispatchable started as `dispatch toggle_gaps` — which turns gaps off
  *globally* and stays off, and `hl_reset` does not restore it. The whole suite
  then ran with no gaps, and `geometry`'s `adjust_gaps` test failed three modules
  later having changed a gap size that was no longer being drawn. Nothing in that
  failure pointed back at the cause. It now dispatches `zoom_reset`, which sets
  the cursor zoom to a value that is already the default, and asserts by
  **contrast**: a real name returns `{"success":true}` and an invented one
  `{"error":"unknown function"}`. "The compositor still answers" would have
  passed with a table full of typos.
- **`jq`'s `tonumber` is decimal only.** The colour round-trip assertion parsed
  `"0x44" | tonumber`, which is an error, so the comparison ran against nothing
  and failed against a build whose colours were exactly right. It compares in
  python now.

Real gaps found by building this out (not just harness bugs — documented
inline in the relevant test files too):
- `set_master_factor`/`adjust_master_count` are dead code: they write to
  `selmon->pertag->{mfacts,nmasters}[curtag]`, but no current layout (tile/
  scroller/float) reads either value.
- `switch_keyboard_layout`/`dwindle_split_horizontal`/`switch_proportion_preset`
  are genuine no-ops without the right config present (a second keyboard
  layout, `dwindle_manual_split 1`, a configured proportion preset list
  respectively) — `hl_start`'s shared config enables all three specifically
  so these dispatches are actually observable.
- IPC's client geometry (`x`/`y`/`width`/`height`) is always the logical
  target (`c->geom`), never the interpolated value the renderer actually
  draws from — there's no dispatch-and-poll sequence that can tell "snapped
  instantly" from "mid-animation" this way. Real animation verification
  needs pixel/frame capture (`anim-test.sh`), a fundamentally different
  kind of tool.
- Two real bugs only surfaced via live-session mode, neither reproducible
  headlessly: a use-after-free in `asteroidz_icon_node_set` (destroyed the
  old cairo surface before the `wlr_buffer` wrapping it was dropped/detached
  from the scene — every other node type in `text-node.c` does the reverse
  order), and a frame-scheduling bug in `monitor_check_skip_frame_timeout`
  (a 100ms "give up and force a commit" safety timer that reset on every new
  resize/configure event instead of enforcing a hard deadline, letting a
  busy real client starve the monitor's actual output commit indefinitely —
  a frozen display and an unbounded memory leak as the same root cause).
  Both were ultimately root-caused via `coredumpctl`/static review, not by
  reproducing them live a second time. See "Live-session mode" above.
- `dispatch set_option` respawned every `spawn` entry in the config, once per
  call, because it applied the change through the full reload path. One extra
  short-lived process is invisible; a settings panel with a live-preview
  control sends `set_option` per frame and would fork per frame. Split into
  `config_apply_live()` and `reset_option()`; `set-option.sh` pins both halves,
  because a test that only asserts "set_option is quiet" passes just as well on
  a build where `run_exec` was deleted outright.

Two things about writing a test that uses `spawn`, both of which produced a
*passing* assertion that proved nothing before being noticed. A path must be
**quoted** — a bare leading `/` starts a KDL comment, so `spawn /tmp/x.sh` is a
parse error that takes the rest of the config with it, and the reload then
silently does nothing. And an inline `sh -c "..."` does not survive: the KDL
handler joins the node's argv tokens with spaces before handing the result to
`spawn_shell`, which runs it as `sh -c <string>`, so the inner quotes are gone
by then. Point `spawn` at a script.

## A separate layer: the tray host

It runs against its own `dbus-daemon` (via `dbus-run-session`) and its own
`XDG_RUNTIME_DIR`, so it never touches the live session's tray, and uses
`contrib/snitem` as a stand-in item rather than needing a real tray
application. Each case re-enters the script on a fresh bus, because a watcher
name can only be claimed once and a stale claim would test the wrong role.

The assertion worth knowing about is the **pixmap cap**. A tray host decodes
pixmaps whose dimensions the application chooses, which is unbounded work
driven by whatever the user happens to have installed — in the compositor that
is a stalled desktop. `snitem --pixmap N` serves an N×N icon, and the test
checks that 48 and 512 are decoded while 4096 is refused outright and the item
simply does not appear. Point it at any host to see whether that host is
bounded; the built-in `tray` module is not, which is why trayd exists.

## A separate, complementary layer: waybar plugin unit tests

The three-plus custom waybar CFFI plugins (`waybar-display`, `waybar-weather`,
`waybar-sysmon`, and others, in separate repos under `src/`) each have their
own `tests/test_<name>.c` + `make test` — plain C unit tests that `#include`
the plugin's own source to reach its pure, GTK-independent logic (icon/text
mapping, JSON parsing, scheduling math) directly, with no GTK init, no
Wayland socket, and no live compositor at all. These live in each plugin's
own repo rather than here, since the code under test does too.
