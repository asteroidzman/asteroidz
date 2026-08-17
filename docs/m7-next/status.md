# M6B — COLOUR MANAGEMENT — **CLOSED** 2026-08-15

**M6A: CLOSED** (`../m6-presentation/status.md`). Its knowingly-unbuilt items —
the ADR-611 refactor, ADR-612's GPU half, `VK_EXT_calibrated_timestamps`,
ADR-613/614's oracles, the VT-switch reset — are closure decisions, not
outstanding M6A work, and do not reopen without a regression or a new measured
requirement.

Closed at `48de472b`, qualified once and completely, with `source == installed
== running` and a clean tree throughout.

| | |
|---|---|
| AVK required set | **52/52 fixtures, 979/979 assertions**, 0 `FAIL` lines |
| regression suite | **557/557** |
| on-GPU unit fixtures | render 111, icc-shaper 14, output-color 80, color-pipeline 39 — 0 failures |
| G6 live, DP-1 | **34/34**, 20 cycles / 40 transitions / 80 modesets |

The upstream wp-cm mastering gap is *decided*, not open, and was never a
blocker.

## What the milestone actually cost, said plainly

M6B did not run green to closure and the record should not read as though it
did. Four things went wrong that were worth more than the features:

1. **A plausible fix caused a second-order regression that reached closure
   qualification.** `a52650f` correctly made `hdr_configured` a tri-state;
   `setmon` still assigned it into a boolean, and `-1` is truthy. Twelve
   fixtures moved. It survived because the only observable was a bool that
   reported the broken value as `true` — wrong pixels were visible, wrong
   intent was not.
2. **A live gate's own precondition false-passed.** It asserted
   `validation_enabled` — the guard against a vacuous `validation_errors: 0` —
   against a session with no validation layer, because `amsg` answered from a
   leftover headless instance that sets `ASTEROIDZ_VK_DEBUG=1` itself. Every
   amsg-derived number in that run described a different compositor.
3. **The residual was in the model, not the renderer.** AVK un-premultiplies in
   the encoded domain, decodes, and re-premultiplies in linear — more correct
   than the reference first written to check it, and than the live screenshot
   model that raised the 2–3%.
4. **Three separate runners reported on nothing.** `avk-suite.sh` invoked bare
   (it is a register; `--run required` is what executes), a runner built from a
   reaped scratch file, and a fixture whose premise read another test's
   leftover state. Only the last of these was caught by an assertion; the other
   two by output being implausibly short.

The common shape: **a green result whose instrument was not measuring what its
name said.** Every fix in this milestone was cheaper than the detection.

## Closure conditions, each met

- **Build** — source `48de472b` == installed == running, tree clean, verified
  by ELF build-id rather than path or version.
- **Headless** — retained gates green, falsifiers known red beside each.
- **G6 live** — validation genuinely loaded on the identified instance, 40
  transitions, 0 VUIDs, 0 lifecycle violations, 0 CPU sync waits, 0
  presentation waits, state restored, `monitors.kdl` byte-identical, one
  pipeline compile across 40 transitions. `fallback_frames` = 20 accepted by
  operator decision, recorded above as a deviation.
- **frog** — per-surface output correct, HDR toggle resends exactly once per
  transition, no redundant churn.
- **wp-cm** — preferred description correct for the fields wlroots serializes;
  the mastering limitation documented as upstream.
- **Blend** — layer-aware fixture resolved the residual: outcome B, the
  modelling error identified by name.

## After closure — the two-writer defect, found by the wp-cm audit

A bounded audit of native wp-color-management ownership found something M6B
did not: **two writers were authoring the preferred description**, with
different policies.

| | |
|---|---|
| asteroidz | `az_preferred.h` — the surface's own output. Sends `WLR_COLOR_TRANSFER_FUNCTION_SRGB`, which wlroots maps to protocol `COMPOUND_POWER_2_4` = **14** |
| scenefx | `types/scene/surface.c:164` — max preference across every output the surface touches. Defaults to `GAMMA22` = **2** |

Both called `wlr_color_manager_v1_set_surface_preferred_image_description` on
the same surface. Whichever fired last won.

**It was not a multi-monitor edge case, though the audit and I both first
called it one.** 2 and 14 differ on an ordinary SDR surface on one output.
Measured against a client before the fix: **7 of 7 descriptions read 2** — every
SDR window was being told the wrong transfer function.
`m6b-preferred-desc-test.sh` passed throughout because it reads once, at a
moment when asteroidz's writer had fired last; a fixture that reads once cannot
see a race.

Fixed by not calling `wlr_scene_set_color_manager_v1` — scenefx's writer is
that field's only consumer, so the scene keeps a NULL manager and **zero
scenefx changes were needed**. A second defect surfaced while proving it: the
map-time send was gated on `s->mapped` inside `setmon`, which runs while the
surface is still unmapped, so a client learned nothing at startup and kept
wlroots' default until something moved the window. Now sent from `mapnotify`,
the first moment the surface is both mapped and on an output.

`contrib/cm-two-writer-test.sh` holds it, with the pre-fix binary as its
falsifier. It settles **which writer**, not **which output** — with
`AZ_BREAK_FROG_FIRST_HDR_OUTPUT` it stays green, because two headless SDR
outputs describe identical colour. That claim stays with
`m6b-frog-metadata-test.sh`, which reads the resolved output directly.

## What is NOT closed by this

- The upstream wp-cm mastering serialization (decided: no patch, no fork).
- **NEXT — NATIVE WP-CM OWNERSHIP RE-AUDIT.** Not a milestone and not named
  M6C until a re-derived verdict says it earns one. The previous audit predates
  `az_preferred.h`, which is the contract such an implementation would
  serialize, so its estimate cannot be planned against.

The milestone defined in `decision.md`. This records what is closed, what it
measured, and what is not closed. Measurements are quoted from the runs that
produced them, not from the plan that asked for them.

**Suite state at `38ab5f0f`** — the only currently valid counts:

| | |
|---|---|
| AVK required set | **59/59 fixtures, 1097/1097 assertions**, zero `FAIL` |
| regression suite | **557/557** |

> **These counts describe a different renderer from the ones they replace.**
> `contrib/lib/headless.sh` defaulted `WLR_RENDERER` to gles2 and never set
> `ASTEROIDZ_RENDERER`, so `az_renderer` stayed `AZ_RENDERER_WLR` and every
> fixture that did not set it itself composited through **SceneFX** — a path
> production cannot reach, because an AVK session aborts rather than
> compositing with it. The regression suite was validating a renderer that
> does not run.
>
> It surfaced as two effects failures reading `not-darker-0` and
> `baseline-not-flat-172`, which look like shadow bugs and are not: under AVK
> the same three assertions pass with nothing else changed. The harness now
> defaults to `avk`. The earlier `51/51, 961/961` and `557/557` figures were
> therefore partly measurements of SceneFX, and are not comparable to these.

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

cLUT profiles are refused **by classification**, before anything is read — the
reduction refuses them, and M6C below carries them by another route entirely.
D2's revival condition (a real cLUT profile for a connected display) was
overtaken by a different one: with SceneFX being removed, `FALLBACK` stopped
meaning "the other renderer handles it" and started meaning "abort".

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
| wired | `preferred: set tf=14 primaries=1 minlum=2000 maxlum=203 reflum=203 maxcll=0 maxfall=203 have=11101` |
| broken (`AZ_BREAK_CM_NO_PREFERRED`) | `preferred: none` |

The mask read `have=11100` until 2026-08-17. Two digits moved, and both are
fixes rather than drift: position 3 is `luminances`, which asteroidz had
**stopped sending entirely** when native wp-cm took ownership — a regression
against wlroots, and the only event carrying `reference`, the SDR white level
(`reflum=203`, `config.sdr_reference_luminance`). Position 4 is
`target_max_cll`, now deliberately absent: it was being filled from the
*display's* peak luminance, and max_cll is a **content** light level that a
preferred description has no business claiming.

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

**This reading is wlroots', not asteroidz's** — it predates `a5334182`, where
native wp-cm took ownership of the protocol. It is kept because the finding
below is about wlroots and is still true of it; it is not evidence about the
implementation now in the tree.

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
| synthetic cLUT | **`B-encode`** | **clut3d** | false (cube: true) | **0**, 3 encode draws |

27/27, with `validation_enabled` asserted first and 0 validation errors.
Falsifiers seen red: removing the cLUT classification (the synthetic profile is
then **accepted as a matrix-shaper**, 4 assertions red — which is the trap its
colorants exist to catch), making the derivation ignore the shaper (7 red), and
— M6C — disabling the compositor-side cube build (**7 red**, the output falling
back to `fallback`/`srgb` with 3 refused frames).

**The third row used to read `fallback` / SceneFX drives it, and the arm
changed sides in M6C.** The comparison at the end of the fixture changed with
it: two of the three arms are now `B-encode`, so comparing *paths* would pass
while the compositor treated a cLUT profile exactly like a matrix-shaper one.
It compares `(path, curve)`.

---

## M6C — the profiles that do not reduce

`FALLBACK` was a safe answer only while a second renderer existed. Removing
SceneFX makes a refusal an abort, and the profile a colorimeter produces is a
cLUT profile — so "AVK refuses cLUT" and "a user who calibrates their display
cannot start the compositor" became the same sentence.

### The domain, which is the whole of the correctness

The cube's input axis is **scene-linear** BT.709 light and its output is
**device code** — electrical, post-TRC. That is not a choice: wlroots builds its
ICC transform (`render/color_lcms2.c`) from an lcms2 source profile with
**gamma-1.0 TRCs**, so the domain is linear by construction, and the
destination is the display profile in `TYPE_RGB_FLT`, which is what the
scan-out buffer holds.

Two consequences, both load-bearing:

- The cube **replaces both the gamut matrix and the inverse EOTF**, because it
  already contains them. C3 therefore derives the **identity** matrix for this
  path. Filling it with the profile's 709→device — the obvious thing to do by
  analogy with `LUT1D` one branch above — applies the colorant transform twice.
- Sampling with the sRGB-**encoded** value instead of the linear one is the
  classic version of this project's recurring mistake. It is smooth, plausible
  and wrong everywhere except 0 and 1, which is why it has its own break.

### 65³ on a squared index, and why not wlroots' uniform 33³

γ⁻¹ has infinite slope at zero in three dimensions for the same reason it does
in one, so D2's warp applies again. Measured against lcms2's own transform for
the synthesised cLUT profile (γ 2.6, wide primaries), worst error over a 41³
off-grid sweep:

| dim | index | worst | memory |
|---|---|---|---|
| 33 | uniform | 13.82 codes | 0.3 MB | ← wlroots' choice |
| 65 | uniform | 5.91 | 2.1 MB |
| 33 | squared | 3.99 | 0.3 MB |
| 45 | squared | 2.90 | 0.7 MB |
| **65** | **squared** | **1.60** | **2.1 MB** | ← this |

The warp is worth more than the memory — a squared 33 beats a uniform 65 at an
eighth of the size — and both are taken because 2.1 MB against the 66 MB
Path-B intermediate the same output already owns is not a trade. Cost at load:
274625 lcms2 evaluations, **59 ms**, on a monitor rule change and nothing else.

**AVK's cube is therefore not SceneFX's cube.** The two sample the same lcms2
transform at different resolutions. Anything comparing the two renderers pixel
for pixel must budget for that.

### The shader is a second module, not a fourth branch

The 1D table is a `sampler2D` and the cube is a `sampler3D`, and both live on
set 1 because the renderer's shared texture layout has exactly one binding. Two
declarations of one binding are **both statically used** — a property of the
SPIR-V that specialisation does not remove, which this pass already learned via
`VUID-vkCmdDraw-None-08600`. So `output_encode.frag` is compiled twice, once
with `-DAZ_ENCODE_CLUT=1`. Building the cLUT variant from the 2D module instead
was run deliberately and produces exactly the predicted
`VUID-vkCmdDraw-viewType-07752`, plus 73 codes of wrong picture.

### Gates

| gate | where | result |
|---|---|---|
| cube vs lcms2, off-grid | `tests/test-icc-shaper.c` (no device) | **0.90 codes** (gate < 2) |
| premise: the profile is non-identity | same | **133.6 codes** from a plain sRGB encode |
| `AZ_TF_CLUT3D` encode vs `az_icc_clut_apply` | `tests/test-avk-render.c` | **worst 1 code** (gate ≤ 1) |
| premise: it moves the fixture's patches | same | **73 codes** |
| derivation, interlock, both-forms precedence | `tests/test-output-color.c` | 13 rows |
| driven by a real compositor | `contrib/m6b-icc-drive-test.sh` | arm 3 |

Falsifiers, each run and seen red:

| break | red by |
|---|---|
| `AZ_BREAK_CLUT_IDENTITY` (identity cube, everything else intact) | **137 codes** |
| `AZ_BREAK_CLUT_DOMAIN` (sample with the encoded value) | **68 codes** |
| shader index warp removed (uniform axis) | **66 codes** |
| shader samples the cube with transposed axes | **152 codes** |
| cLUT variant built from the `sampler2D` module | 73 codes **+ 3 validation errors** |
| C3's cLUT branch disabled | 4 red in `test-output-color`, 1 premise red on the GPU |
| compositor-side cube build disabled | 7 red in the headless fixture |
| `clut_encoded_domain` dropped in the record path | the domain break collapses to **1 code — red** |

**One thing the GPU gate provably cannot catch**, established by running it:
transposing the axes in `az_icc_clut_build` leaves `test-avk-render` at 135/135,
because the CPU reference reads the same array the same way and the GPU samples
the same texels — reference and subject are transposed together. The lcms2
comparison in `test-icc-shaper` goes red on it. The GPU gate answers "does the
shader *sample* the table correctly"; the CPU gate answers "was the table
*built* correctly". Neither can do the other's job.

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

## The colour-protocol boundary — closed by owning it

Asteroidz speaks two colour-management protocols to clients, and now
**implements both**. It did not always; the asymmetry below is written in the
past tense because closing it is the work.

| | frog-color-management-v1 | wp-color-management-v1 |
|---|---|---|
| implemented by | **asteroidz**, in this tree | **asteroidz**, in this tree (was: wlroots 0.20.2) |
| primaries | yes | yes |
| transfer function | yes | yes |
| max luminance | **yes** | **yes** (was: not serialized) |
| min luminance | **yes** | **yes** (was: not serialized) |
| max FALL | **yes** | **yes** (was: not serialized) |
| used by | gamescope | mpv, kodi, browsers |

The gap had been one function: `image_desc_handle_get_information` in wlroots'
`types/wlr_color_management_v1.c` sends the transfer function's *default*
luminances and carries two literal `TODO`s where the real mastering values
belong. Asteroidz supplied them correctly; wlroots discarded them on the way
out.

**No local wlroots patch, no fork, no vendored overlay, no cherry-pick.** That
standing decision is what shaped the fix. Filling an upstream implementation gap
with a carried patch buys one feature and a permanent dependency-maintenance
liability, so the alternative taken was to implement the protocol here:
`src/ext-protocol/wp-color-management.h`, built on `wlr_surface_synced` and the
generated protocol bindings, both of which are public API. wlroots stays a
system package, unmodified. An upstream contribution is welcome and
non-blocking; the tree must never depend on an unmerged one.

**There is no second implementation behind a switch.** `wlr_color_manager_v1`
is not created at all. It has no destroy function — it lives until display
teardown — so two managers could never have shared a session anyway: both
globals would appear in the registry and clients would bind whichever they saw
first. The capability set both were handed is built once, in
`src/ext-protocol/az_cm_caps.h`, from wlroots' *own* enums, so every advertised
value round-trips through `_to_wlr()`, the function that aborts on anything it
cannot map. `contrib/cm-native-caps-test.sh` pins the advertisement against the
byte-for-byte recording taken while both implementations still existed.

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

## G6 live — PASSED 2026-08-15, on DP-1, 34/34

`contrib/m6b-hdr-transition-live.sh`, 20 cycles / 40 transitions / 80 modesets.
The run that counts, and the one below records why the previous attempt did not.

**Identity first, before any modeset.** pid 46513, build `9faa8687`, session
`asteroidz-avk-debug`, backend `drm`, `validation_enabled true`, **0** other
candidate sockets — and `source == installed == running` at `a210adb4` with a
clean tree. Every query pinned by `AMSG_REQUIRE_PID/BUILD/VALIDATION`. The same
pid answered the first query and the last, and the fixture's own sha256 was
unchanged at the end.

| gate | result |
|---|---|
| validation errors | **0**, with the layer proven loaded on *this* instance |
| lifecycle violations | 0 |
| CPU sync waits | 0 |
| presentation waits | 0 |
| pipeline compiles | **1 over 40 transitions** |
| intermediate images / bytes | 1 → 1, 67174400 → 67174400 |
| blur cache (same-domain) | 2 → 2, correctly preserved |
| monitors.kdl | restored byte for byte |
| frog resends | 1 → 41: exactly one per transition, no churn |

**The per-cycle record says more than the totals**, which is why it is kept
(`cycles.tsv`):

- **Every SDR half landed on `B-encode/lut1d`** — the ICC profile activating,
  because DP-1 carries one and it is inert only in HDR. So the run exercised
  the display-profile path live twenty times as a side effect of toggling HDR,
  and D3's claim (a profile must not cost an output its renderer) held on every
  one.
- **The single extra pipeline compile happened at the first SDR entry and never
  again** across nineteen more. That is the keying invariant proven rather than
  bounded — "1 over 40" could otherwise have been one compile late in the run.
- **Refused frames increment once per CYCLE, on the HDR→SDR direction only**
  (1,1,2,2,3,3…), never on SDR→HDR. Sharper than "bounded by 20".

### `fallback_frames` = 20 — ACCEPTED BY OPERATOR DECISION, 2026-08-15

The gate as written asked for zero. The run produced twenty, and that was
surfaced as a **deviation** rather than folded into a pass; the operator
accepted it.

One frame per HDR→SDR transition is refused by `az_output_may_drive()` while
the committed image description and the derived colour state are momentarily
out of step — the description is committed by KMS, the state derived from
`m->hdr`. SceneFX draws that frame instead of AVK writing scene-linear values
into a PQ buffer. **Refusing is the designed behaviour and the alternative is a
visibly wrong frame.**

What is bounded and proven, from `cycles.tsv`, is the RATE rather than the
total: exactly one per cycle, on the HDR→SDR direction only, never on SDR→HDR,
across twenty cycles. A change that made this per-*transition* would double it
and the existing bound would catch that.

**The gate keeps its `<= CYCLES` bound and does not become `== 0`.** Rewriting
the assertion to expect twenty would make it pass on a compositor that refused
twenty frames for an entirely different reason.

**The picture-unchanged assertion is weak and should not be read as more.**
The control pair was captured seconds apart (29056 px of churn, worst 253); the
final comparison spans the whole two-minute run (72718 px). A desktop with an
animating bar accumulates more difference over minutes than over seconds, so
the two are not comparable quantities and the 4× limit is generous by
construction. It is a smoke test for gross corruption, not a claim that the
frame is identical.

**The shared preferred-colour policy moved and returned**: `DP-1 true 400` →
`DP-1 false 280` → `DP-1 true 400`, identity `2357112514714281158` ↔
`908963075078007682`, with the surface's output never drifting. That part
stands: it is `az_preferred_resolve`'s own answer and it is what the run
measured.

**THE WIRE VALUES QUOTED HERE ARE FROG'S, NOT wp-cm's — CORRECTED 2026-08-17.**
This section used to present

    tf=3 primaries=35400,14600 maxlum=400 minlum=4000 maxfall=250

as evidence that "the wp-cm half moved and returned". It is not a wp-cm line.
It is `contrib/wlcm`'s **frog** printer (`wlcm.c`, `frog_preferred`), and three
things say so independently:

- `tf=3` is `FROG_COLOR_MANAGED_SURFACE_TRANSFER_FUNCTION_ST2084_PQ`. On the
  wp-cm side PQ is `11`, which is what this document's own live HDR reading
  above quotes.
- the primaries are in **frog's** 1/50000 CIE xy units (`35400,14600` is
  BT.2020 red, straight out of `frog_surface_send_preferred_metadata`'s
  literals). wp-cm's coordinate events are in 1/1000000.
- `minlum=4000` is frog's 1/10000 encoding of the rule's `0.4`.

It could not have been the wp-cm line, because `wlcm` sources `minlum`/
`maxlum`/`reflum` from `wp_image_description_info_v1.luminances` — **an event
asteroidz did not send at all** until 2026-08-17. Every wp-cm reading in this
section was structurally incapable of carrying a mastering value.

**And native wp-cm was never running for this measurement.** The G6 live run
was against source `a210adb4`, which is **eight commits before** native wp-cm
ownership landed in `a5334182`. So every wp-cm number in this section describes
**wlroots' 0.20.2 implementation**, not asteroidz's. Native wp-cm has never been
exercised live, and nothing in this document should be read as evidence that it
has been.

What the run does establish is that the frog path carried the operator's rule
values correctly across an HDR↔SDR transition — `maxlum=400 minlum=4000
maxfall=250` in HDR and `tf=2 primaries=32000,16500 maxlum=280 minlum=2000
maxfall=280` in SDR — and that the shared policy behind both frontends moved
and returned without the surface's output drifting.

## The FIRST attempt — CONTAMINATED, NOT EVIDENCE

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

### RESOLVED — outcome B, and the modelling error has a name

`test_blend_domain` in `tests/test-avk-render.c`. Both arms render the same
scene; each is checked against its own independently-computed model.

| | |
|---|---|
| Path A (linear), vs model | **worst 1 code** |
| UNORM arm (encoded), vs model | **worst 1 code** |
| the two domains disagree here | 31 codes, 12 of 12 samples — the premise |

**The renderer is correct, and it is more correct than the reference first
written for it.** AVK un-premultiplies in the ENCODED domain, decodes, then
re-premultiplies in LINEAR before the `over`. That is the right order:
premultiplication is applied to encoded values by the client, so decoding the
premultiplied value directly — the common approximation — is wrong by up to 49
codes at these inputs.

**That approximation is what the live screenshot model did, and it is the
2–3% residual.** The first version of this fixture's own reference made the
identical assumption and reported Path A as 51 codes wrong; the renderer was
right and the model was wrong, in exactly the way the live A/B had been. So the
residual closes as a **NON-DIAGNOSTIC FINAL-IMAGE MODEL ERROR**, with the error
identified rather than merely retired.

Each wrong form is ruled out **by the measured data**, not by comparison
against the correct model — the claim is "the data excludes this", so it is
made from the data:

| wrong form | ruled out by |
|---|---|
| straight alpha used as premultiplied | 125 codes |
| decoding the premultiplied value directly | 49 codes |
| double premultiplication | 47 codes |
| missing source decode | 46 codes |
| encode-before-blend ordering | 26 codes |
| gamma-domain blend | 31 codes |

**Two things this found on the way.** Every alpha assertion in the suite until
now rendered into the shared `B8G8R8A8_UNORM` target, so it composited on
encoded codes — `test_overlap_alpha` asserts `0x80 + 255*(1-0.5) = 255`, which
is only true if `0x80` is a code rather than a light value. The domain that
ships had no blend coverage at all, and the residual sat exactly there. And the
first colour pair chosen here was a near-mirror (`FG 200,80,30` over
`BG 30,90,200`), which makes red and blue land on the same value at α=0.5 —
two channels that stop being two samples.
