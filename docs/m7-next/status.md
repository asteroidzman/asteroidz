# M6B — status

The milestone defined in `decision.md`. This records what is closed, what it
measured, and what is not closed. Measurements are quoted from the runs that
produced them, not from the plan that asked for them.

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

## Open — and all of it needs the operator, not more code

Every gate that a machine can settle is settled. What remains needs a display
that can present HDR and a person who can look at it.

- **G6's HDR↔SDR half.** A headless output cannot enter HDR, so twenty toggles
  on DP-1 is the only place this runs — and the gate requires
  `validation_enabled` asserted first, which means the `asteroidz-avk-debug`
  session and therefore a logout, not a restart. Each toggle is a retrain (two
  modesets and a visible flash; see `project_hdr_pending_commit_fails`), so
  twenty cycles is forty of them.

  The profile-toggle half **is** green and exercises the same lifecycle:
  intermediate allocated and returned, blur cache invalidated across the domain
  change, no per-cycle recompile, zero refused frames, zero VUIDs. What the HDR
  half adds is the KMS modeset and image-description commit path. Worth doing;
  not worth doing without the operator choosing the moment.

- **D6's mastering values** are blocked upstream (above), not by this tree.

- **The blend-domain residual, newly sharpened.** On the live A/B, ~2–3% of
  pixels sit beyond 1 code from the model, concentrated on translucent UI.
  Encoded-space and linear-space compositing must differ there, and in the
  direction observed — but *must differ* is weaker than *differs by this much*,
  and **a screenshot pair cannot close it**: the model sees only the composited
  result, never the layers that made it. Settling it needs the same content
  composited both ways with no profile in force, which is a fixture and not a
  capture. An attempt to isolate it by 3×3 flatness **failed** — a translucent
  bar over a uniform backdrop is flat in the output and still a blend.
