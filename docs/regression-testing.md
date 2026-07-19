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
`HL_HEIGHT`. Set `HL_ALLOW_DESTRUCTIVE=1` to also run
`destroy-virtual-output.sh`, which is skipped by default (it destroys every
headless output, including the original one — safe in isolation, but not
worth risking in a shared run).

## How it works

`contrib/lib/headless.sh` is the shared library: `hl_start` launches one
fully isolated headless compositor instance (own `XDG_RUNTIME_DIR`, Wayland
socket, `ASTEROIDZ_INSTANCE_SIGNATURE`) plus a flat-color `swaybg` wallpaper,
never touching your real session. `hl_dispatch`/`hl_get` wrap `amsg
dispatch`/`amsg get` scoped to that instance; `hl_watch_start` backgrounds an
`amsg watch ...` stream for asserting on IPC notifications. `hl_spawn_kitty`/
`hl_spawn_wllayer` spawn tracked, throwaway test clients. `hl_reset` kills
spawned windows and returns to a known state (tag 1, tile layout, `HEADLESS-1`
focused) between test cases so they can't leak state into one another.
`hl_assert`/`hl_assert_eq`/`hl_assert_true`/`hl_assert_false` are the
pass/fail primitives, tallied globally; `hl_summary` prints totals.

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

## Module coverage

Twenty modules as of writing (118 assertions): `layouts`, `window-states`,
`tags`, `focus`, `scratchpad`, `geometry`, `dwindle`, `overview`,
`multimonitor`, `mousebind`, `hdr`, `scroller`, `animations`, `layer-shell`,
`ipc-watch`, `keybind-combo`, plus `destroy-virtual-output` (gated behind
`HL_ALLOW_DESTRUCTIVE=1`).

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

## A separate, complementary layer: waybar plugin unit tests

The three-plus custom waybar CFFI plugins (`waybar-display`, `waybar-weather`,
`waybar-sysmon`, and others, in separate repos under `src/`) each have their
own `tests/test_<name>.c` + `make test` — plain C unit tests that `#include`
the plugin's own source to reach its pure, GTK-independent logic (icon/text
mapping, JSON parsing, scheduling math) directly, with no GTK init, no
Wayland socket, and no live compositor at all. These live in each plugin's
own repo rather than here, since the code under test does too.
