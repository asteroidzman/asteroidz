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

## Open

- **G4 / D6** — preferred image description
  (`wlr_color_manager_v1_set_surface_preferred_image_description`, zero callers
  today), with `AZ_BREAK_CM_NO_PREFERRED`.
- **G5 / D5** — Path A promotion. The headless half is green; the live half's
  instrument is the operator watching HDMI-A-1, which is the point of it.
- **G6** — the closing transition gate: ≥20 HDR↔SDR cycles, `validation_enabled`
  asserted first, 0 VUIDs, the encode intermediate returned on every B-exit,
  the blur cache invalidated across the domain change.
- **The blend-domain residual, newly sharpened.** On the live A/B, ~2–3% of
  pixels sit beyond 1 code from the model, concentrated on translucent UI.
  Encoded-space and linear-space compositing must differ there, and in the
  direction observed — but *must differ* is weaker than *differs by this much*,
  and **a screenshot pair cannot close it**: the model sees only the composited
  result, never the layers that made it. Settling it needs the same content
  composited both ways with no profile in force, which is a fixture and not a
  capture. An attempt to isolate it by 3×3 flatness **failed** — a translucent
  bar over a uniform backdrop is flat in the output and still a blend.
