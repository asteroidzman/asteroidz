# M5 — scene-linear HDR: architecture package

**Status (2026-08-14): COMPLETE.** C1-C8 implemented, both output paths and the
full decode path qualified on a GPU against a CPU reference written from these
ADRs, and the whole chain measured end to end on a real HDR display with a real
HDR client: **worst 2 codes** from HDR10 file through PQ decode, source gamut
conversion, linear FP16 composition, tone mapping, gamut conversion, luminance
anchor and PQ encode to a 10-bit scan-out.

HDR is on by default wherever C3 chooses Path B, with no environment variable.
Path A (8-bit SDR linear compositing) remains opt-in behind `AZ_M5_PATH_A=1`:
it is correct and qualified, but it changes how every SDR pixel on the desktop
blends and nobody has asked for that.

See `opus-findings.md` for what implementation found that the architecture did
not -- F1 through F16 -- and section 5.12 onward of
`docs/vulkan-native-architecture.md` for the measurements.

This directory is the Phase-1 architecture deliverable for milestone M5
(scene-linear HDR). It was produced by reading the code at `2c1c015`, not from
the milestone summaries. It is written for the implementing agent; nothing in
it is aspirational prose — every claim about the current code carries a
file:line reference and a confidence class.

## Reading order

| File | What it is |
|---|---|
| `audit.md` | Where colour arithmetic actually happens today, in both renderers, with every claim classified OBSERVED / CONFIGURED / ASSUMED / UNKNOWN |
| `adr.md` | The architecture decisions, ADR-000 … ADR-013, each with context, options, decision, consequences and a falsifier |
| `contracts.md` | Implementation contracts C1 … C7 for the implementing agent, in the FEATURE / INPUTS / OUTPUT / FORMAT / INVARIANTS / PERFORMANCE / TESTS / FILES / CONFLICT-STATUS format |
| `conflict-manifest.md` | The integration conflict manifest: every path M5 touches, who owns it, and what may land now versus later |
| `opus-findings.md` | What implementation found that the architecture did not: F1-F15, each with what was believed, what was measured, and what changed as a result |

## Confidence vocabulary (used throughout)

- **OBSERVED** — read from the code in this worktree (or wlroots 0.20.2
  source), with a citation. The strongest class.
- **OBSERVED-LIVE** — established by earlier live measurement on this machine,
  recorded in project memory; the code alone would not tell you.
- **CONFIGURED** — a value or behaviour that holds because of this machine's
  configuration, not because of the code.
- **ASSUMED** — believed on general grounds; not verified here. Every ASSUMED
  entry names what would verify it.
- **UNKNOWN** — could not be determined from the code. Needs measurement
  before anything is allowed to depend on it.

## The invariants this package is built around

1. **HDR10/PQ is an output encoding, never the internal representation.** No
   image in the frame graph ever stores PQ-encoded values except the scanout
   buffer written by the final output-encode pass. No blur, blend, or effect
   ever operates on PQ-encoded values.
2. **One scene, many outputs.** A single scene-linear representation serves
   SDR UI, ordinary SDR apps, extended-range SDR, and HDR media
   simultaneously; each output applies its own final transform. A window
   straddling an SDR and an HDR output has one scene intent and two final
   transforms.
3. **Per-window luminance domains are first-class** (ADR-006). There is no
   global `hdr_brightness`.
4. **Transfer functions apply to RGB, never to alpha** (ADR-005). Alpha is
   coverage; it is linear by definition and has no colorimetry.
5. **Decode once → composite linear → encode once.** No repeated conversions
   through the effect chain.
6. **Performance is a design constraint.** GLES is the project floor; for
   comparable functionality AVK must meet or beat it. Every proposal here
   carries pass/byte/allocation estimates. `R16G16B16A16_SFLOAT` is the
   leading working-format candidate but is chosen by the criteria in ADR-001,
   not by fiat.
7. **SDR before HDR.** The SDR source → scene-linear → SDR output path must be
   proven (contract C4's round-trip gate) before HDR10 output is enabled.

## What M5 deliberately does not do (scope — see ADR-000)

ICC / 3D-LUT outputs stay on the SceneFX fallback (M6). HLG is decode-scoped
only if a source demands it. There is no adaptive (content-analysing) tone
mapping. `wp_color_manager_v1.set_luminances` remains unimplemented upstream
and nothing here depends on it.
