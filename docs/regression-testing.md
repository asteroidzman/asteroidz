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

## Module coverage

Twenty modules as of writing (180 assertions): `layouts`,
`window-states`, `tags`, `focus`, `scratchpad`, `geometry`, `dwindle`,
`overview`, `multimonitor`, `mousebind`, `hdr`, `scroller`, `animations`,
`layer-shell`, `ipc-watch`, `keybind-combo`, `bar`, plus
`destroy-virtual-output` (gated behind `HL_ALLOW_DESTRUCTIVE=1`).

`bar` is the pattern to copy for anything that needs a **different config**
than the shared one: it never enables the native bar globally (that would
shrink the usable area and silently break every geometry assertion in the
other modules), but rewrites `$HL_CONFIG` from a pristine copy and calls
`reload_config` per test, restoring it afterwards. It also skips itself when
the binary under test was built with `-Dnative-bar=false`, probing the binary
rather than assuming — an unknown config key is only warned about, so a
feature-off build would otherwise silently "pass" by doing nothing.

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

## A separate, complementary layer: waybar plugin unit tests

The three-plus custom waybar CFFI plugins (`waybar-display`, `waybar-weather`,
`waybar-sysmon`, and others, in separate repos under `src/`) each have their
own `tests/test_<name>.c` + `make test` — plain C unit tests that `#include`
the plugin's own source to reach its pure, GTK-independent logic (icon/text
mapping, JSON parsing, scheduling math) directly, with no GTK init, no
Wayland socket, and no live compositor at all. These live in each plugin's
own repo rather than here, since the code under test does too.
