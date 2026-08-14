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

**The live quality pass D5 asks for is OUTSTANDING and is the operator's.** What
Path A changes is where the encode happens, not whether; blended pixels do
change, and that is ADR-005 rather than a regression — but it is also the one
thing no fixture can call correct on its own.

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

**The HDR arm is not covered headlessly and is not faked**: a headless output
cannot present an HDR image description. Live-only, on DP-1.

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

## Open — and all of it needs the operator, not more code

Every gate that a machine can settle is settled. What remains needs a display
that can present HDR and a person who can look at it.

- **D5's live quality pass.** Path A is now the default; what it changes is
  blended pixels, and no fixture can call that correct. One watched session.
- **G4's HDR arm.** A headless output cannot present an HDR image description,
  so `preferred: set tf=PQ primaries=BT2020` with DP-1's mastering values can
  only be read on DP-1. `contrib/wlcm` is the instrument and takes seconds.
- **G6's HDR↔SDR half.** Same reason: twenty toggles on DP-1, with
  `validation_enabled` asserted first. The profile-toggle half is green and
  exercises the same lifecycle.

All three fit in **one** live session, which is what D5 asked for in the first
place. Nothing about them is blocked on further work.

- **The blend-domain residual, newly sharpened.** On the live A/B, ~2–3% of
  pixels sit beyond 1 code from the model, concentrated on translucent UI.
  Encoded-space and linear-space compositing must differ there, and in the
  direction observed — but *must differ* is weaker than *differs by this much*,
  and **a screenshot pair cannot close it**: the model sees only the composited
  result, never the layers that made it. Settling it needs the same content
  composited both ways with no profile in force, which is a fixture and not a
  capture. An attempt to isolate it by 3×3 flatness **failed** — a translucent
  bar over a uniform backdrop is flat in the output and still a blend.
