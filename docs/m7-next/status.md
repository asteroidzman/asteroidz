# M6B — status

**M6A: CLOSED** (`../m6-presentation/status.md`). Its knowingly-unbuilt items —
the ADR-611 refactor, ADR-612's GPU half, `VK_EXT_calibrated_timestamps`,
ADR-613/614's oracles, the VT-switch reset — are closure decisions, not
outstanding M6A work, and do not reopen without a regression or a new measured
requirement.

**M6B: HEADLESS GATES GREEN. MILESTONE NOT CLOSED.** Two technical items
remain: the G6 live HDR↔SDR gate in a validation session, and the blend-domain
residual. The upstream wp-cm mastering gap is *decided*, not open, and is not a
blocker.

The milestone defined in `decision.md`. This records what is closed, what it
measured, and what is not closed. Measurements are quoted from the runs that
produced them, not from the plan that asked for them.

**Suite state at `aa17a47a`** — the only currently valid counts:

| | |
|---|---|
| AVK required set | **51/51 fixtures, 961/961 assertions**, zero `FAIL` |
| regression suite | **557/557** |

Counted from the run logs, not read off the summary lines. That distinction is
not pedantry here: an earlier attempt at this same qualification invoked
`avk-suite.sh` bare, which is a *register* that runs nothing without
`--run required`. It printed a clean audit over 51 suites in nine lines and
exited 0. A batch runner must assert that work happened — count executed
fixtures and refuse to report below a floor — because an exit status of 0 is
not evidence that anything ran.

**Standing rule in force throughout:** a green oracle is not trustworthy until
its falsifier has been observed red. Every number below has a break beside it
that was run and seen to fail.

---

## Closed

### D2 + D4 — ICC matrix-shaper ingest (`src/render/color/az_icc.{h,c}`)

Reads a display profile into the two slots C6's encode pass already had: a 3×3
scene-BT.709-linear → device-linear matrix with Bradford adaptation, and a
per-channel encode curve with the vcgt composed on. Pure CPU, no Vulkan and no
wlroots, so it is unit-testable without a renderer.

cLUT profiles are refused **by classification**, before anything is read. D2's
revival condition stands: a real cLUT profile for a connected display existing
on this machine.

**A uniform LUT index cannot represent an encode curve.** γ⁻¹ has infinite slope
at zero, so a uniform grid mistracks it near black however large it is — 5.81
codes of interpolation error at 256 taps, 2.51 at 1024. The taps are on a
**squared index** (tap *i* holds the curve at `(i/(N-1))²`, read with `sqrt`),
which brings 256 taps to **0.01 codes**. The warp is part of the table's
contract, not a private detail, so nothing can sample it without it.

### D3 — a profile must not cost an HDR output its renderer

`hdr` is decided **before** `has_icc`. The operator's own `monitors.kdl` carried
the consequence, commented out beside a note that the display is calibrated:
restoring `icc-profile … hdr 1` used to take DP-1 off AVK entirely — losing HDR
and the renderer together. On an HDR output the profile is now inert by design
(the connector presents its own image description), not a reason to abandon the
output.

### G1 — ingest vs lcms2 (`tests/test-icc-shaper.c`, no device)

| | |
|---|---|
| premise: the profile moves a 16³ grid | **100 codes**, 94.4% of samples |
| matrix+curve vs lcms2's own transform | **1.92 codes** (gate < 2) |
| D4: composed curve *is* `vcgt(TRC⁻¹(x))` | **0.0039 codes** at every tap |
| premise: the vcgt is non-trivial | 3.00 codes |
| falsifier: perturbed `matrix[0]` by 1e-2 | **2.71 codes — red** |

**The premise is not decoration.** On the neutral axis alone this profile moves
9 codes against 100 over the full grid — an oracle built on greys would have
understated it elevenfold. Every row of a plausible wrong matrix sums to 1, so
grey cannot see it.

**The three refusals are now executed, not merely reasoned about.** Until M6B
the only profile on this desk was an RGB display matrix-shaper, so every
refusing path had never run. `contrib/icc-synth.c` synthesises the missing
profiles, and the cLUT one **carries colorants and TRCs** — a cLUT profile
without them would be refused by an implementation that merely failed to find a
matrix, and would prove nothing about classification.

### G2 — the encode LUT, on the GPU (`tests/test-avk-render.c`)

| | |
|---|---|
| premise: C3 puts a profiled SDR output on B/LUT1D | ✓ |
| premise: the profile moves the fixture's patches | **31 codes** from a plain sRGB encode |
| `AZ_TF_LUT1D` encode vs `az_icc_apply` | **worst 1 code** (gate ≤ 1) |
| falsifier: `AZ_BREAK_ICC_LUT_IDENTITY` | **72 codes — red** |

The patches have their channels apart, for G1's reason.

The curve is a 256×1 `R16G16B16A16_UNORM` image per output, uploaded when the
profile's serial moves and never on a frame path. Sixteen bits and not the
output's eight: the pass dithers against one output code, and a curve quantised
to the output's own depth would sit at the dither's own amplitude.

### G3a — the decision table (`tests/test-output-color.c`, no device)

SDR + shaper → B/LUT1D with **the profile's own matrix**; HDR + profile → B/PQ,
profile inert; a profile that does not reduce → FALLBACK; `peak_scene` still
exactly 1.0 and `dither_q` still one output code. Falsifiers seen red: dropping
the shaper's matrix, removing the LUT1D interlock, ignoring the shaper entirely.

### D5 + G5 — Path A promoted

`AZ_M5_PATH_A` now has the same shape as `AZ_M5_PATH_B`: unset = on wherever C3
chose it, `=0` = the bisect handle restoring pre-M5 blending exactly. It was
opt-in for a milestone on the stated grounds that it had not been *watched*
rather than that it was wrong, and "not yet watched" never ends on its own.

The promotion broke its own fixture, which is the only reason it was noticed:
the control arm was spelled `run off` with no environment at all, so after the
inversion both arms ran the same configuration. It failed on its premises
rather than silently comparing a thing to itself. 12/12 with the arm fixed.

**D5's live quality pass: PASSED 2026-08-14.** The operator ran the promoted
default on HDMI-A-1 and accepted it. That is the whole of the gate — what Path A
changes is blended pixels (transparency, shadows, antialiased text edges), and
no fixture can call that correct or incorrect on its own. Opaque pixels are
bit-identical by the on-GPU round trip, so there was nothing else to judge.

### D6 + G4 — the preferred image description (`contrib/m6b-preferred-desc-test.sh`)

`wlr_color_manager_v1_set_surface_preferred_image_description` had no callers,
so every wp-cm client asking DP-1 what it preferred was told the compositor
default — SDR — and correctly tone-mapped its HDR down to meet it. Each mapped
surface is now told its output's own description, on map, on output-enter, and
**after** an HDR state change commits (not in `hdr_resolve`, which would
announce the description the output is about to leave).

| | |
|---|---|
| wired | `preferred: set tf=14 primaries=1 … have=11100` |
| broken (`AZ_BREAK_CM_NO_PREFERRED`) | `preferred: none` |

`tf=14` **is** sRGB: wlroots maps its own `WLR_..._SRGB` to the protocol's
`COMPOUND_POWER_2_4`, not to the protocol's `SRGB`. Asserting 14 asserts that
the value went out through wlroots' mapping rather than being written by hand.

The assertion had to come from a client — nothing inside the compositor can tell
whether it said anything. `contrib/wlcm` distinguishes `set` / `none` /
`no-protocol`, because "told SDR" and "told NOTHING" look identical on screen
and are unrelated defects.

**Two crashes, both mine, both in code that looked obviously right:**

1. **NULL is not "the default".** wlroots dereferences `data` unconditionally,
   so passing NULL for an SDR output is a null dereference the moment any wp-cm
   client holds a feedback object.
2. **A hard-coded protocol constant killed the compositor.** `_to_wlr()` has no
   case for protocol `SRGB` and falls through to `abort()` — SIGABRT, core
   dumped, observed. This file already carried the rule (start from wlroots'
   enums, map outward with `_from_wlr()`); I reached for the obvious constant
   instead.

**The HDR arm: PASSED LIVE on DP-1, 2026-08-14** — `contrib/wlcm` against the
running session read

    preferred: set tf=11 primaries=6 ... have=11100

which is ST2084_PQ / BT.2020. A wp-cm client now learns the display is HDR
instead of being told the SDR default, which is the defect D6 exists for.

**BUT THE MASTERING VALUES DO NOT REACH THE CLIENT, AND THAT IS UPSTREAM.** The
same reading shows `minlum=50 maxlum=10000 reflum=203, maxcll=0 maxfall=0` --
wlroots' *default* luminances for PQ, not DP-1's rule (`max-luminance 400`,
`min 0.4`, `max-fall 250`). The compositor supplies them; wlroots 0.20.2 never
sends them:

    types/wlr_color_management_v1.c:162  // TODO: send mastering display
                                         //       primaries and luminances ...
    types/wlr_color_management_v1.c:171  // TODO: send target_max_cll and
                                         //       target_max_fall

**THE VALUES ARE NOT LOST, AND THE FIRST VERSION OF THIS NOTE WAS WRONG TO
SHRUG.** They reach clients through **frog-color-management**, which asteroidz
implements itself and which gamescope actually uses:
`frog_surface_send_preferred_metadata` sends the monitor rule's own
`max-luminance`, `min-luminance` and `max-fall`, plus BT.2020 primaries, in the
protocol's own units. wp-cm is the path that drops them; frog is the path that
carries them, and it is entirely ours.

Auditing it under D6's own principle found **two defects of exactly the kind D6
exists to fix, one protocol object over**:

1. **It described the wrong display.** The monitor was chosen as "the first
   enabled HDR output, else selmon" -- so a window living entirely on the SDR
   panel was handed the HDR panel's BT.2020 primaries and 400-nit ceiling, and
   told to tone-map for a display it is not on. Now resolved from the surface's
   own client and its own monitor.
2. **It was sent once and never again.** A client that connected before an HDR
   toggle kept tone-mapping for the display state it was told about at startup,
   forever. There is now a registry of live frog surfaces, re-sent at the same
   two moments as the wp-cm description: after an HDR state change commits, and
   when a surface changes output.

So the mastering half of D6 is delivered on the path that can carry it, and is
blocked only on `wp_color_management`, where the data is correct on our side of
the call and discarded inside wlroots. Closing that half means patching wlroots
-- a decision, not a task.

### G6 — transitions (`contrib/m6b-transition-test.sh`)

Twenty profile on/off cycles, moving an 8-bit SDR output between Path A and
Path B — the same resource lifecycle an HDR toggle uses.

| | |
|---|---|
| validation errors across 20 cycles | **0** (layer asserted present first) |
| frames refused | **0** |
| pipeline compiles | **1**, not 20 — the keying holds |
| intermediate images after | **0** — returned, not leaked |
| blur cache rebuilds | **80** (≥ 2 per cycle) |

The blur assertion had to earn its place twice: first it was measuring nothing
(the M4I cache has no backdrop without a blurring shadow, so it read 0/0
forever), then "greater than zero" was still satisfied by ordinary damage.
**Falsifier, run and seen red:** removing `format` from the cache's validity
test drops rebuilds from 80 to **zero** while hits stay at 80 — the cache is
served straight across every domain change.

**Why not HDR↔SDR, which is what G6 named:** a headless output does not support
BT.2020 + PQ, so an HDR toggle there would be twenty no-ops reporting success.

### G3b — driven by AVK, headless (`contrib/m6b-icc-drive-test.sh`)

Three arms differing in one config line, landing in three states:

| arm | path | tf | shaper | fallback frames |
|---|---|---|---|---|
| no profile | `A-direct-srgb` | srgb | false | 0 |
| FI32U.icm | **`B-encode`** | **lut1d** | true | **0**, 3 encode draws |
| synthetic cLUT | `fallback` | srgb | false | 3 (SceneFX drives it) |

19/19, with `validation_enabled` asserted first and 0 validation errors.
Falsifiers seen red: removing the cLUT classification (the synthetic profile is
then **accepted as a matrix-shaper**, 4 assertions red — which is the trap its
colorants exist to catch), and making the derivation ignore the shaper (7 red).

---

## A plausible fix that regressed, and got as far as closure qualification

This is the most instructive thing that happened in M6B and the record must not
compress it to "bug fixed, tests pass."

**`a52650f` fixed a real defect.** The operator reported that `amsg toggle_hdr`
had never worked. It hadn't: `hdr-mode` was an absolute *override* rather than a
default, so on a desktop configured `hdr-mode on` the dispatch wrote the
baseline, `hdr_resolve` immediately re-asserted HDR, IPC answered success, and
the only trace was a log line saying the request was overridden "for now". The
fix made `hdr_configured` a tri-state — `-1` nobody has spoken for this output,
`0` explicitly off, `1` explicitly on — so an explicit per-output choice could
outrank the global default.

**`aa17a47a` fixed what that fix broke.** `setmon` still did
`m->hdr = m->hdr_configured`, copying the new tri-state straight into the
effective boolean. **`-1` is truthy.** Every `if (m->hdr)` — the PQ branch in
`mon_derive_color_state`, the HDR commit in `mon_state_apply_color`, the 10-bit
format choice — read an output nobody had configured as HDR. And `hdr_resolve`
compares `want == (m->hdr > 0)`, so `want=false` matched and it returned
*before* normalising: the output stayed at `-1` for its entire life.

Headless outputs came up on PQ + Path B and refused their first frames.
**Twelve fixtures moved in the closure run**, `m6b-icc-drive` from 19/19 to
9/19 with all three arms collapsed onto the same `path=B-encode tf=pq` — three
arms that must differ reporting identically being the tell, because it means
none of them chose the state they were in.

**Why it survived that far.** The only observable was a bool. `hdr_enabled` is
`m->hdr` through `cJSON_AddBoolToObject`, which reports `-1` as `true` —
indistinguishable from correct. A fixture could see the wrong PIXELS but never
the wrong INTENT, and pixel damage reads as a renderer problem, which is where
the twelve moved fixtures pointed. IPC now carries `hdr_configured` as the
tri-state it is; the `hdr` regression module's header had said the policy inputs
were not headlessly assertable, and that was true until then.

**Three further things this cost, all worth keeping:**

1. **Three fixtures were nearly written off as baseline.**
   `avk-crossoutput-round` (5/7), `avk-oracle` (5/6) and `avk-scale-transform`
   (41/47) failed *identically* in two consecutive full runs, which is exactly
   the signature of known baseline noise. They were not: the first run's
   failures were Path-A-promotion fallout fixed by `f462a316`, the second's were
   this regression. Two different causes producing the same numbers. All three
   are green now. Identical failures across runs are not evidence of a stable
   baseline.
2. **The guard's first version was non-discriminating.** It read
   `hdr_configured` expecting `-1` but ran *after* the toggle test and read that
   test's leftover `1`. It went red on the broken build — for a reason unrelated
   to the bug — and would have failed identically on the fixed one. `-1` is a
   write-once observation: `set_output_hdr` takes a boolean, so nothing can
   restore "never mentioned". It now runs first and asserts its premise.
3. **The assertion that catches this was already in the repo.**
   `contrib/regression/tests/hdr.sh` has asserted `starts with hdr_enabled
   false` all along. The module simply was not run after `a52650f` changed HDR
   resolution. The suite was not missing a test; the test was not run.

## Defects this milestone found, which its own gates did not

Recorded because all three were green-at-the-time and none was found by the
gate that should have owned it.

1. **The encode pass drew with descriptor set 1 unbound on every Path-B
   frame** — every HDR frame on DP-1 included, with no profile in force
   anywhere. The reasoning that a specialisation constant makes the descriptor
   "not statically used" is wrong: that is a property of the SPIR-V module, not
   of what the driver folds away. Found by validation, not by 100 green checks.
   `tests/test-avk-render.c` now sets `ASTEROIDZ_VK_DEBUG` **itself** and
   asserts zero VUIDs, with the layer's presence asserted first.
2. **Config reload never re-derived the output colour state.** Every other path
   that builds an output state pairs apply with derive; this one did not.
   Harmless until M6B made `icc-profile` a reload-changeable input.
3. **`set_output_icc` had the same gap**, in the route a user is more likely to
   take, and additionally never damaged the output — so even a correct state
   would have reached only whatever happened to be redrawn next.

---

## Verified live

On HDMI-A-1 (an LG FHD; the profile characterises DP-1's panel, so this was a
**path test, not a calibration**), through both the config route and
`amsg dispatch set_output_icc`:

- state flips to `B-encode`/`lut1d`, 0 fallback frames, clears cleanly
- **99.89% of pixels moved against a 0.014% control** — a 7,094× ratio
- the neutral axis matched the CPU model on 17 levels from 0 to 255, **worst 1
  code** (`13 → 22,19,19`, `128 → 128,127,127`, `255 → 255,252,254`)

**The eye was not a valid instrument here and said so.** 42% of that screen is
pure black, which maps 0→0 exactly, and the greys move ≤1 code; the operator
correctly reported "nothing changed at all" while the profile was being applied
perfectly. Only saturated colour moves visibly (`255,0,0 → 241,56,25`).

---

## The colour-protocol boundary — a decision, not unfinished work

Asteroidz speaks two colour-management protocols to clients. **They do not have
identical expressive power, and pretending otherwise would be the dishonest
part.**

| | frog-color-management-v1 | wp-color-management-v1 |
|---|---|---|
| implemented by | **asteroidz**, in this tree | wlroots 0.20.2 |
| primaries | yes | yes |
| transfer function | yes | yes |
| max luminance | **yes** | not serialized upstream |
| min luminance | **yes** | not serialized upstream |
| max FALL | **yes** | not serialized upstream |
| used by | gamescope | mpv, kodi, browsers |

The gap is one function: `image_desc_handle_get_information` in
`types/wlr_color_management_v1.c` sends the transfer function's *default*
luminances and carries two literal `TODO`s where the real mastering values
belong. Asteroidz supplies them correctly; wlroots discards them.

**Say it precisely.** Not "asteroidz loses mastering metadata" — asteroidz
retains it internally and exposes it through frog; the wlroots wp-cm frontend
does not yet serialize the mastering-luminance / content-light-level events.

**No local wlroots patch, no fork, no vendored overlay, no cherry-pick.** That
is a standing decision: filling an upstream implementation gap with a carried
patch buys one feature and a permanent dependency-maintenance liability. An
upstream contribution is welcome and non-blocking; the tree must never depend on
an unmerged one.

### One policy, two serializers

The two frontends previously disagreed, which is why `src/render/az_preferred.h`
now exists. frog resolved its output as *"the first enabled HDR monitor, else
selmon"* while wp-cm resolved the surface's own — so a window living entirely on
the SDR panel was handed the HDR panel's BT.2020 primaries and its 400-nit
ceiling, and told to tone-map for a display it was not on.

`az_surface_effective_output()` is now the single answer: `c->mon`, which
`setmon()` alone writes and which every other per-output decision in the
compositor already follows. A straddling surface therefore gets an answer that
agrees with where its borders are drawn and where its blur is clipped. **There
is no `selmon` fallback** — a surface not on an output has no preferred display,
and inventing one is the defect this file ended.

`az_preferred_resolve()` returns the resolved description plus an **identity**
(FNV-1a over output name, HDR state, primaries, max/min luminance, max FALL).
A frontend caches the identity it last *sent* and re-sends only when it changes,
so an HDR toggle on another monitor, a hotplug elsewhere, or a layout change
that did not move the surface all cost exactly one comparison.

Values cross the boundary **unnormalised**: DP-1's rule says 400 / 0.4 / 250 and
each frontend converts only to its own wire units. The upstream limitation is
visible as *a frontend cannot currently emit this*, never as *the value
disappeared before reaching the frontend*.

## The attempted live G6 run — CONTAMINATED, NOT EVIDENCE

An HDR↔SDR run was attempted on 2026-08-14 and **does not count**. It is
recorded here rather than deleted, because the way it false-passed is the
finding.

**Validation was never loaded.** The session was `asteroidz-avk`, which carries
no `ASTEROIDZ_VK_DEBUG`; `avk_debug_enabled()` reads only that variable, so the
Vulkan validation layer was absent for the whole run.

**And the precondition written to prevent exactly that FALSE-PASSED.** The
fixture asserted `validation_enabled` first — the guard against a vacuous
`validation_errors: 0` — and the assertion passed. It passed because `amsg`
resolves its socket from `ASTEROIDZ_INSTANCE_SIGNATURE`, **falling back to
scanning `XDG_RUNTIME_DIR`**, and with several compositors alive it answered
from a leftover headless test instance. Every M6B headless fixture sets
`ASTEROIDZ_VK_DEBUG=1`, so the wrong respondent reported exactly the value the
precondition wanted to see.

**Therefore every amsg-derived metric from that run is discarded** — the
validation-error count, the fallback-frame count, the monitor states, the
intermediate accounting. None of it may be cited as closure evidence. It was
not measuring the production compositor.

**The frog client-side evidence survives, and only because it did not come
through amsg.** `contrib/wlbgeffect`'s own log file recorded 41 preferred-metadata
events across the transitions, in both HDR states, carrying DP-1's rule values
`400 / 0.4 / 250` on the wire. That observation depended on nothing but the
client's connection to the compositor it was actually rendering on. It is
useful, and final closure should still tie it to an identified build and
identified state transitions rather than stand alone.

**The infrastructure defect is being fixed, not worked around.** Killing stray
compositors before a run would hide it. The live harness must bind its telemetry
to the intended instance — PID, executable, embedded build hash, endpoint,
backend, session — verify all of it before any modeset, and refuse to run when
more than one plausible instance exists or the respondent does not match.

## Open — the exact remaining M6B gates

Two. Neither is the upstream wp-cm mastering gap, which is decided (below) and
is **not** an M6B blocker.

- **G6's HDR↔SDR half, in a validation session.** A headless output cannot
  enter HDR, so DP-1 is the only place this runs, and it needs
  `asteroidz-avk-debug` — a logout, not a restart. Twenty cycles is forty
  modesets (each transition on this output falls back to a retrain; see
  `project_hdr_pending_commit_fails`). That same logout is what aligns
  `running == installed == HEAD`; no closure evidence comes from mixed
  binaries.

  The profile-toggle half **is** green and exercises the same lifecycle:
  intermediate allocated and returned, blur cache invalidated across the domain
  change, no per-cycle recompile, zero refused frames, zero VUIDs. What the HDR
  half adds is the KMS modeset and image-description commit path — plus the one
  frog case a headless output cannot reach: a stationary mapped surface whose
  output changes HDR state, receiving updated metadata on the same protocol
  object.

- **The blend-domain residual** — see below. It needs a layer-aware fixture,
  which is buildable headlessly and is not waiting on the operator.

- **D6's mastering values** are blocked upstream (above), not by this tree, and
  are not counted as an open M6B item.

## The blend-domain residual — the second real closure item

On the live A/B, ~2–3% of pixels sit beyond 1 code from the model, concentrated
on translucent UI. Encoded-space and linear-space compositing must differ there,
and in the direction observed — but *must differ* is weaker than *differs by
this much*.

**A screenshot pair provably cannot close it.** The CPU model sees the
already-composited output, never the independent foreground and background
layers that produced it. No amount of capture analysis recovers the inputs.
Deriving layer-blend correctness from final images is abandoned as a method,
not merely unfinished.

**The 3×3 flatness isolation attempt is recorded as NON-DISCRIMINATING and must
not be cited later as partial evidence.** A translucent flat foreground over a
uniform flat background produces flat composited output whether or not the
blend semantics are correct. It could not have distinguished the two.

**What closes it** is a deterministic fixture that knows its inputs before
composition — background linear RGB, foreground linear RGB, alpha,
premultiplication state, source colour description, output state — compared
against a CPU reference that independently implements source decode, linear
conversion, premultiplication, the `over` operation and output encode. The
reference must not call the production helper; an implementation agreeing with
itself proves nothing.

Swept across alpha (0, low, ~0.25, 0.5, ~0.75, 1) and over patterned or
chromatic content where flat content would let a second bug masquerade as
correct. It must produce a meaningful red against each of: gamma-domain blend,
wrong premultiplication, double premultiplication, straight alpha used as
premultiplied, wrong source decode before blend, and encode-before-blend
ordering where representable.

Two acceptable outcomes, and the residual does not stay open past them:

- **A real blend bug** — locate the first divergence, fix it, re-run the
  targeted fixture, record the final code error.
- **A final-image model artifact** — if the layer-aware fixture agrees within
  tolerance, the 2–3% screenshot residual closes as NON-DIAGNOSTIC, because a
  discriminating instrument answered what a non-discriminating one raised.
