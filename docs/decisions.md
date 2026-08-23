---
title: Decisions
description: Every architectural decision the code still cites, one entry each.
---

# Decisions

Source comments cite these 166 times. That is what this file is for: the code
says "ADR-605" and this says what ADR-605 decided. Everything else about how
each was arrived at — the options weighed, the audits, the experiments that
went nowhere — was 16,000 lines of milestone narrative and is gone. What
survives is the part that still binds.

Numbering is historical and is kept because the code uses it: `ADR-0xx` are
colour (M5), `ADR-6xx` are presentation (M6A). There are no others, and new
work should not add to either series — see [history](/docs/history) for why the
milestone apparatus was retired.

## Colour — the scene-linear pipeline

| | |
|---|---|
| **ADR-000** | Scope of M5: scene-linear working space, per-window luminance domains, SDR and HDR10/PQ outputs, static tone mapping, matrix gamut mapping, output-stage dither, simultaneous mixed outputs. |
| **ADR-001** | Two output paths. **Path A (direct-sRGB)** when the render format is `XRGB8888`, the output has no image description and no ICC transform, and the scanout modifier admits a mutable `_SRGB` view. **Path B (encode)** otherwise. Draws always sample decoded to linear. |
| **ADR-002** | Working space is **BT.709 primaries, D65 white, unbounded** (scRGB-style). Wide-gamut colours carry negative components, which is why the working format is signed float. BT.2020 working primaries were rejected: every sRGB source would pay a matrix it does not need. |
| **ADR-003** | Scene 1.0 **is SDR reference white** — dimensionless. One global `scene_reference_luminance` gives it a nit value only at absolute↔relative boundaries: `config.sdr_reference_luminance` when set, else 203 cd/m². |
| **ADR-004** | Untagged and sRGB-tagged surfaces decode with the **piecewise sRGB EOTF**, not pure 2.2. Surfaces explicitly tagged GAMMA22 or BT.1886 through wp-cm decode with their declared curve. This is what makes Path A a hardware-exact round trip, so the SDR→SDR identity gate can demand bit-identity. |
| **ADR-005** | Canonical scene pixel is **premultiplied linear**. Decode order for translucent sources: un-premultiply → EOTF on RGB only → luminance-domain scale. |
| **ADR-006** | Every texture draw carries a resolved **luminance domain** (`struct az_lum_domain`): a transfer function, primaries, and a scale. Nothing downstream guesses. |
| **ADR-007** | SDR output: Path A composites straight through the `_SRGB` attachment view, so scene → wire is identity for content in [0,1]. Values above 1 are tone-mapped at source decode. |
| **ADR-008** | An HDR output is **always Path B**. The encode pass runs, per pixel and in this order: sample → tone map to the output's peak → gamut matrix → clamp → inverse EOTF → dither → write. |
| **ADR-009** | One tone curve for the whole project: **static, hue-preserving, max-channel-driven extended Reinhard with an identity segment**. Not per-frame, not adaptive. |
| **ADR-010** | Gamut mapping is **relative-colorimetric matrix + post-tone-map per-channel clamp**. No perceptual compression. On BT.2020 outputs the 709→2020 matrix leaves BT.709 and P3 content non-negative, so the clamp only touches genuinely out-of-container values. |
| **ADR-011** | Dither lives **wherever quantisation happens**, at that target's quantum, on electrical values, RGB only. Path B adds IGN of one target code after the inverse EOTF, before the write. |
| **ADR-012** | The command stream is **output-agnostic** — scene values and luminance domains never mention an output. Per-output state lives in `struct az_output_color_state`, resolved on output-state change rather than per frame. |
| **ADR-013** | The M5 split between isolated and deferred work. Historical; the integration it governed is long done. |

## Presentation — the presenter and animation

| | |
|---|---|
| **ADR-600** | Scope of M6A: production presentation feedback, the per-output presenter, the two-regime predictor, one sample instant per pass, the mixed-refresh contract, retarget continuity. |
| **ADR-601** | `target_ns` is **a prediction of the CLOCK_MONOTONIC instant the frame will turn into light** on this output. Computed once per output frame, at frame-event time, by that output's presenter and nothing else. |
| **ADR-602** | `struct az_presenter` is a **value type embedded in `Monitor`**, not pointed to. It refuses to answer outside an armed pass. |
| **ADR-603** | Presentation feedback is **production input, not a debug path** — the listener is wired unconditionally. The clock domain is proven per sample, never assumed. |
| **ADR-604** | Lifecycle is `UNSYNCED → SYNCED` on the first accepted present. The **regime is chosen once per epoch** and never changes within one, because an adaptive-sync toggle arrives as a commit that is itself a reset trigger. |
| **ADR-605** | Two regimes, different in kind. FIXED predicts the next vblank from the observed period; VRR cannot, because the panel free-runs. Tearing suspends prediction as a per-frame overlay. |
| **ADR-606** | **One sample instant per pass**, threaded explicitly down the draw path. Clocks are banned from animation code: an animation that reads the time itself cannot be refresh-independent. |
| **ADR-607** | One semantic trajectory `X(t)`, **sampled per output at that output's own instant**. Two monitors animating one window agree on the motion and disagree on the moment, which is correct. |
| **ADR-608** | Position continuity across a retarget is a **required invariant**; velocity continuity is spring-only and deferred. |
| **ADR-609** | A miss is a frame that did not make its next presentation opportunity. **Verdicts require timestamps or they are UNKNOWN** — an honest unknown rather than a guess. |
| **ADR-610** | **Idle stays idle.** The presenter executes only inside existing edges and owns no `wl_event_source` at all, so a settled desktop costs zero wakeups. |
| **ADR-611** | Semantic animation state (CPU-owned, one per animation, never per frame) and presentation state (per pass, per output) are separated by a **one-way boundary**. |
| **ADR-612** | **CPU evaluates the motion, GPU applies the transform.** |
| **ADR-613** | Damage is the pixel-grid expansion of the union of the previously presented box and the currently evaluated one, dilated by the window's effect halo — shadow radius, blur reach, border, and the damage-ring in-bounds rule. |
| **ADR-614** | **One evaluated box per window per pass.** Adjunct geometry takes that box as a parameter; no adjunct may read the scene node's integer coordinates mid-pass. The rule is constructional — it has to be unrepresentable, not merely avoided. |
| **ADR-615** | Transforms move **scene-linear** pixels. Ordering: decode → composition including the transform → per-output tone and gamut → encode → dither → KMS. No transform ever touches an encoded representation, blur caches included. |
| **ADR-616** | Motion must be **refresh-independent by construction** — `X(t)` a pure function of wall-clock time, identical at 48 through 240 Hz and across dropped frames. This rules out frame-based integration outright. |
| **ADR-617** | On a mixed HDR/SDR straddle each output runs its own colour transform. **Encoded presentation state is never shared between outputs**, not as pixels and not as a cached intermediate. No optimisation may reintroduce sharing without reopening this. |
