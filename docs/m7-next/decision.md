# Post-M6A direction — the next milestone decision

Written against HEAD ccc02fc after M6A's closure, from the code, the M5
package, and this machine's own configuration — not from the milestone
summaries. The operator's default preference (ICC first) was checked against
the tree and the hardware rather than accepted; the verdict is at D1 and is
**confirmed in direction, refuted in content**.

The single most load-bearing fact in this document is not in the repo. It is
in `~/.config/asteroidz/monitors.kdl`:

```
// AVK test: DP-1's colour management is off so the native Vulkan engine will
// take the output at all -- it refuses any output needing an ICC or HDR
// transform (colour management is M6). This display IS calibrated; put these
// back when you are done looking:
//     icc-profile "/home/ralf/FI32U.icm"; hdr 1;
```

The user disabled his own display calibration to let AVK drive his display,
and left himself a note to put it back. That is a standing, this-machine,
user-stated requirement, and it is the demand signal the M4/M6A rule asks for
before anything gets built. `/home/ralf/FI32U.icm` exists (10,516 bytes,
AORUS FI32U factory characterisation) and its tag table settles the scope:
**rXYZ/gXYZ/bXYZ matrix columns, single-gamma rTRC/gTRC/bTRC curves, and a
vcgt calibration table. It is a matrix-shaper profile. There is no cLUT tag
in it, and no colorimeter or profiling tool is installed on this machine to
ever produce one.**

---

## The nine questions

**Q1 — What significant capabilities remain after M6A?**

1. **Display characterisation** — applying an ICC display profile inside AVK.
   Today `mon_load_icc_profile()` (asteroidz.c:5627) genuinely loads and
   parses the profile via `wlr_color_transform_init_linear_to_icc`, but AVK
   refuses any output carrying one: C3's derive table checks `has_icc`
   *before* `hdr` and returns FALLBACK (az_output_color.h:162), and
   `az_output_may_drive` refuses FALLBACK outright. The transform is applied
   only by the SceneFX fallback, and only in SDR mode
   (`az_output_color_transform`, az_output.h:86).
2. **Additional encodings** — HLG as a source transfer function (C1 has
   `az_hlg_eotf` decode-only; C7 has no variant; the wp-cm advertisement
   deliberately omits it), and EXT_LINEAR advertisement (same status, same
   reason: no variant, no test).
3. **Adaptive / dynamic tone mapping** — ADR-009's curve is static per
   output. Nothing measures scene content per frame.
4. **Production closure of M5 itself** — Path A still env-gated off;
   per-surface preferred image description never sent; HDR/SDR transition
   and hotplug behaviour of the colour state qualified only incidentally.
5. **Observability leftovers** — the oracle's FP16 taps stand down on Path B
   (C6 finding, contracts.md); `VK_EXT_calibrated_timestamps` already
   dispositioned by M6A as optional, not reopened here.

**Q2 — Which are dependencies of others?** Characterisation is a dependency
of every *accuracy claim* the project ever wants to make about a real panel —
that is the sound half of the operator's "foundation" argument. It is **not**
a dependency of HLG (a source-side decode variant; the output side never sees
it) or of adaptive tone mapping (a policy on top of ADR-009 that works,
characterised or not). Preferred-image-description depends only on machinery
that already exists (`wlr_color_manager_v1_set_surface_preferred_image_description`
is in the linked wlroots and nothing calls it). Path A promotion depends on a
live quality pass and on choosing F5's knee value — one push constant. HLG
and adaptive tone mapping are leaves: nothing depends on them.

**Q3 — Which have actual current use ON THIS MACHINE?**

| capability | this machine |
|---|---|
| ICC matrix-shaper on the encode pass | **YES** — FI32U.icm exists, the config note demands its restoration, and restoring it today throws DP-1 off AVK entirely (both modes: `has_icc` fires even when HDR would make the transform inert) |
| Path A promotion | **YES** — HDMI-A-1 is an 8-bit SDR output compositing un-colour-managed today; Path A is its only zero-cost linear path |
| preferred image description | **YES** — DP-1 presents HDR10 and wp-cm clients (mpv, §5.22) get no compositor-driven preference; a client honouring the default tone-maps itself to SDR |
| HDR/SDR transition soundness | **YES** — `hdr 1` is live on DP-1, screenshot and per-window paths toggle it, blur cache and encode intermediate both carry cross-domain state |
| cLUT / 3D LUT profiles | **NO** — no such profile exists here and no tool to make one is installed |
| HLG source | **NO** — no client has been observed attaching it; it is not advertised, so none can; mpv converts HLG media itself |
| adaptive tone mapping | **NO** — §5.22 measured the static curve at worst 2 codes end-to-end on this panel and no live complaint names content it fails on |

**Q4 — the next milestone** is D1 below: **M6B — the colour pipeline reaches
the display the machine actually has.** ICC matrix-shaper in the AVK encode
pass, Path A promoted, preferred image description wired, and the transition
behaviour of all of it qualified as gates rather than as a separate
workstream.

**Q5 — explicitly NOT in it**: D7. HLG, adaptive tone mapping, 3D-LUT/cLUT
infrastructure, wp-cm ICC-file feature, source-side ICC, FP16 oracle taps,
and anything M6A closed.

**Q6 — acceptance gates**: G1–G6 under D8, each with its falsifier named.

**Q7 — overlap with the M5 colour architecture.** None of M5's ordering
moves: source decode → scene-linear composition → per-output tone/gamut →
encode → dither → KMS stands, and characterisation lands entirely inside the
"per-output tone/gamut → encode" slot that C3/C6 already own — the profile
matrix takes the matrix slot `az_output_color_state` already has, and the
per-channel curve takes the `encode_tf` slot. Exactly two M5 statements are
amended, both flagged in M5's own text as M6's to amend: "ICC outputs return
`path = FALLBACK`" (C3/ADR-000; az_output_color.h says "M6 absorbs it" at the
`has_icc` field) and Path A's opt-in status (az_avk.h: "off because it has
not been qualified live, not because it is wrong"). Nothing touches the
decode half, the luminance domains, or the composition contract.

**Q8 — new Vulkan capabilities.** One: the encode pass samples a small
per-channel 1D LUT (a 256×1 `R16G16B16A16_UNORM`-class texture, ~2 KB),
selected by a specialisation constant so non-ICC outputs compile it out.
No extension, no new sync, no frame-path allocation: the LUT is built at
derive time (output-state change), its descriptor set cached exactly like
`avk_image.sampler_set`, honouring M5's "no allocation, no descriptor-set
update, no CPU wait on the steady-state frame path" and M3.5's permanent
guarantees untouched. For the record, since the question was posed: even the
*rejected* 3D form is not rejected on cost — a 33³ RGBA16F LUT is 287 KB of
VRAM and its per-pixel trilinear fetch is L2-resident against an encode pass
already moving 66.4 MB read + 33.2 MB written per full 4K frame; added DRAM
traffic is noise. It is rejected because no profile on this machine, or
producible on this machine, needs it (D7).

**Q9 — independently implementable and testable.**

- *ICC ingest* (profile → matrix + composed curve): pure CPU, oracle is
  lcms2 evaluating the same profile; no device, no compositor.
- *Encode LUT variant*: GPU unit fixture with a synthetic curve, against a
  C4 extension; needs no real profile and no compositor.
- *Derive-table change*: `tests/test-output-color.c` extension; no device.
- *Preferred image description*: headless compositor + a wp-cm client
  fixture; independent of everything above.
- *Path A promotion*: `contrib/avk-m5-path-a-test.sh` already exists (12/12,
  layer on); the remaining step is a live pass, operator-scheduled.

Only G6 (transitions) requires the pieces together, which is why it is the
closing gate rather than a deliverable.

---

## Decisions

**D1 — The next milestone is M6B: display characterisation for the profile
class in hand, plus the two M5 loose ends that share its qualification.**
The operator's ICC-first ordering is **confirmed in direction** — colour
management before HLG and before adaptive tone mapping, for the reason the
config note makes concrete — and **refuted in content**: the foundation to
build is *not* 3D-LUT infrastructure. The only profile this machine has or
can produce is matrix-shaper, which the existing `az_output_color_state` and
C6 encode pass express with a matrix (slot exists) and a per-channel curve
(one new 1D LUT). Building the 3D form first would be exactly the
architectural-completeness spend ADR-612's GPU half was rejected for; the
honest accounting above shows its cost was never the objection and demand is
the only test it fails.

**D2 — Ingest: lcms2, matrix-shaper class only; cLUT profiles keep
FALLBACK.** A new pure-CPU unit (`src/render/color/az_icc.{h,c}` shape, C1's
discipline: no Vulkan, no wlroots types) uses lcms2 — already in the
dependency closure via wlroots — to produce, from a profile: (a) the 3×3
scene-BT.709-linear → device-linear matrix, chromatic adaptation included,
and (b) three encode-direction curves sampled to 256 taps. A profile with
cLUT tags (A2B/B2A) is *refused by classification*, and the refusal keeps
today's behaviour: FALLBACK, SceneFX drives the output, one log line names
the reason. Revival condition for the cLUT class, stated so this does not
run forever: a real cLUT profile for a connected display existing on this
machine. Not "someday someone might".

**D3 — Where it sits: the derive table changes, the ordering does not.**
`az_output_color_derive` re-orders so `hdr` is decided before `has_icc`:

- HDR output, profile present → **Path B / PQ, profile inert**, logged once.
  An output presenting its own image description is colour-managed by the
  connector; applying an SDR characterisation on top would apply two
  transforms (the existing `az_output_color_transform` rule, kept). This is
  also what makes `icc-profile … hdr 1` restorable at all: today that exact
  line loses AVK in both modes.
- SDR output, matrix-shaper profile → **Path B**, matrix := profile matrix
  (saturation composed as ADR-008 already orders it), `encode_tf` :=
  `AZ_TF_LUT1D` referencing the curve. Path B, not A, is forced by physics:
  Path A's encode is the fixed-function `_SRGB` attachment conversion, which
  cannot express a profile TRC — the same shape as F11's 8-bit/deep-colour
  split, decided by the output, not probed.
- SDR output, cLUT profile → FALLBACK (D2).
- No profile → exactly today's table, bit for bit.

**D4 — vcgt is composed into the curve, not sent to KMS.** The profile's
measurements are valid only on the calibrated device state, so shipping the
matrix without vcgt would apply a characterisation of a display that is not
the one on the cable. The 256-tap curve is `vcgt(TRC⁻¹(x))` — encode first,
calibration on device values — built at ingest and unit-tested against the
tag's own numbers. One application point; the DRM GAMMA_LUT is not touched.
(Interaction with `wlr_gamma_control_manager_v1` clients is out of scope and
recorded as such: a gamma-control client stacking on an ICC'd output is two
calibration authorities, and refusing to arbitrate them this milestone is a
decision, not an oversight.)

**D5 — Path A: PROMOTE, gated on one live quality pass; not deleted, not
left gated forever.** The 0-code qualification passed with the layer on; the
code's own comment says it is off only because nobody has watched it. The
disposition: it becomes the default for 8-bit SDR outputs in M6B, after the
operator watches it in the milestone's single live session (live-session
rules apply: warned, visible, never backgrounded). `AZ_M5_PATH_A` inverts to
the same shape as `AZ_M5_PATH_B` — unset = on where C3 chose it, `=0` = the
bisect handle that restores pre-M5 blending exactly. Promotion carries F5's
one outstanding number: the sub-1.0 knee for the >1-capable decode variant,
whose falsifier F5 already states (a PQ source on an 8-bit output must not
hard-clip at SDR white; an ordinary SDR source must be bit-identical with
the curve compiled out). If the live pass finds a quality regression, the
finding — not the gate — reopens the decision.

**D6 — Preferred image description: wire it; it is the protocol surface's
missing half.** Nothing in the tree calls
`wlr_color_manager_v1_set_surface_preferred_image_description`; wp-cm
clients asking DP-1 what it prefers get the compositor default and correctly
tone-map their HDR down to SDR — the exact failure shape §5.21 fixed on the
capability list, one protocol object over. M6B sets each surface's preferred
description to its primary output's current one — PQ/BT.2020 with DP-1's
actual mastering values (max-luminance 400 is already in the monitor rule)
when that output presents HDR, default otherwise — updated on map, on
output-enter, and on HDR state change. Small by design; the description
plumbing all exists on the output side.

**D7 — Explicitly out, each with its revival condition:**

- **HLG**: revived by a client observed wanting to attach it (it cannot
  today — not advertised — so the observation is a user playing HLG content
  through something that refuses to convert). When revived it is one C7
  spec-constant decode variant plus one advertisement entry: a small
  contract, which is exactly why it does not need a milestone slot in
  advance. "Another transfer function" — the operator's phrase — is correct.
- **Adaptive tone mapping**: policy on a pipeline measured at worst 2 codes
  end-to-end (§5.22). Revived by a named live complaint about specific
  content, with a capture. It would also be the first per-frame
  content-dependent state in the encode path — histogram/reduction passes,
  temporal smoothing — which M3.5's guarantees make genuinely expensive to
  do right; it does not enter on a hunch.
- **3D-LUT / cLUT infrastructure and colorimeter workflows**: D2's revival
  condition.
- **wp-cm ICC feature (client-supplied ICC image descriptions)** stays off;
  source-side ICC is a different animal from display characterisation and
  nothing requests it.
- **FP16 oracle taps**: the OUTPUT tap still names divergent frames on Path
  B; the prefix/blur taps standing down is an accepted, recorded gap
  (contracts.md C6). Revived by a Path-B defect the OUTPUT tap localises too
  coarsely to act on.
- **Everything M6A closed** stays closed on its own terms (status.md).

> **CLOSED 2026-08-15** at `48de472b`. Every gate green with its falsifier
> observed red; the live half — D5's quality pass, G4's HDR arm, G6's HDR↔SDR
> half — passed on DP-1 in a validation session, 34/34, with the answering
> instance identified by pid and ELF build-id rather than assumed.
>
> Qualified once and completely: 52/52 AVK fixtures (979 assertions), 557/557
> regression, 0 failures across four on-GPU unit fixtures, `source ==
> installed == running`.
>
> `status.md` carries the measurements, the defects found, and — deliberately
> — what went wrong on the way: a fix that regressed, a precondition that
> false-passed, a residual that was the model rather than the renderer, and
> three runners that reported on nothing.

**D8 — Acceptance gates.** Per the standing rule, no gate counts until its
falsifier has been seen red; every break below runs in the ordinary suite,
and a green break run is a suite failure.

- **G1 (ingest, no device).** For FI32U.icm: the matrix+curve
  representation matches lcms2's own linear→device transform within 1e-4
  over a 4096-sample grid, and the vcgt composition matches the tag table's
  values at every tap. *Premise assertion first*: the profile is measurably
  non-identity — assert the worst-case code delta of the full transform
  against identity and record it, because a gate that would also pass on an
  identity transform is the F15 coincidence again, and G2's falsifier reuses
  the recorded delta as its expected red. *Falsifier*: perturb one matrix element by 1e-3 and
  drop the vcgt composition; each must go red independently.
- **G2 (encode LUT, GPU unit fixture).** The `AZ_TF_LUT1D` encode variant
  vs a C4 extension carrying the same curve: ≤ 1 8-bit code pre-dither on a
  non-neutral fixture (channels offset — F15's lesson; every row of a
  plausible wrong matrix sums to 1 and neutrals cannot see it).
  *Falsifier*: `AZ_BREAK_ICC_LUT_IDENTITY` bypasses the LUT; the measured
  delta must equal the profile's own known non-identity from G1.
- **G3 (derive + drive, unit + headless).** The extended decision table in
  `tests/test-output-color.c`: SDR+shaper → B/LUT1D; HDR+profile → B/PQ,
  profile inert; cLUT → FALLBACK; no-profile rows bit-identical to today's.
  Headless: an output with the FI32U profile is *driven by AVK* —
  `az_output_may_drive` true, `path=B-encode` in the M5 colour log line, no
  SceneFX fallback message. *Falsifier*: the synthetic cLUT profile must
  still produce the fallback log line — the refusal is itself a tested
  behaviour, not a leftover.
- **G4 (preferred description, headless).** A wp-cm client fixture reads
  its preferred image description: PQ/BT.2020 with the rule's mastering
  values on the HDR output, default on the SDR output, and an update arrives
  on HDR toggle. *Premise*: the client asserts it received *any*
  compositor-set description before asserting which. *Falsifier*:
  `AZ_BREAK_CM_NO_PREFERRED` suppresses the calls; the client must observe
  the default where it observed PQ.
- **G5 (Path A, live).** The existing headless gate stays green
  (12/12, layer on), then one operator-watched live session on HDMI-A-1
  under D5's rules. The falsifier for the headless half already exists and
  has been seen red (the fixture's own break arms); the live half's
  instrument is the operator, which is the point of it.
- **G6 (transitions, closing gate).** With everything landed: an
  HDR↔SDR toggle cycle on DP-1 (per-window and rule-driven), N ≥ 20 cycles,
  layer confirmed on (`validation_enabled` asserted first — F14): 0 VUIDs,
  the first frame after each toggle colour-correct (probe a known patch, not
  a whole-frame compare), the encode intermediate returned on every B-exit
  (`avk-stats` byte accounting flat across cycles), and the blur cache
  invalidated across the domain change (`AZ_BLUR_CACHE_IGNORE_DIRTY` +
  `_IGNORE_SOURCE` arms stay able to reproduce their defect — a break that
  stops breaking has stopped looking).

**D9 — Docs and closure.** ADR-000's "ICC is M6" and az_output_color.h's
`has_icc` comment are amended in the same commits that change them; the
monitors.kdl restoration line is the milestone's definition of done: the
user uncomments his own note, and both halves of it — the profile and
`hdr 1` — hold on AVK simultaneously.

---

## Where the operator's framing was wrong, said plainly

1. **"3D LUT infrastructure is foundational" is backwards for this
   machine.** The foundation is the *slot* — and M5 already built it:
   `az_output_color_state`'s matrix + encode_tf, consumed by one encode
   pass. The profile in hand fills that slot with 9 floats and 768 taps. A
   3D LUT is a *representation*, and adopting the general representation
   before any profile requires it is the completeness argument in different
   clothes. When a cLUT profile exists, it lands in the same slot without
   re-architecture.
2. **"Production hardening" is not a separate candidate milestone** — as a
   standalone it has no subject, which M6A showed is what makes a milestone
   work. Its real content (transitions, lifecycle, soak) appears here as
   G6, the gate everything else must pass through, and release closure
   follows the milestone rather than being scheduled inside it.
3. **The loose ends were about to run forever by being small.** Path A has
   been "qualified but unwatched" for a milestone; preferred-description has
   been "exists upstream, uncalled" since M5.6. Both are one-session items
   and both are in scope precisely so they stop being ambient.
