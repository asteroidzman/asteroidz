# M5 implementation contracts (Fable → Opus handoff)

Each contract: FEATURE / SCENE-LINEAR WORKING TARGET / INPUTS / OUTPUT /
FORMAT / INVARIANTS / PERFORMANCE CONSTRAINTS / TESTS / FILES / CONFLICT
STATUS WITH PERFORMANCE AGENT.

Conventions binding on all contracts:

- "scene value", "electrical value", EOTF direction, and the luminance
  vocabulary are ADR-defined; code identifiers use them.
- Every decode clamps its electrical input to [0,1] per channel before the
  EOTF (the PQ-NaN rule, ADR-005).
- No contract may add an allocation, a descriptor-set update, or a CPU wait
  to the steady-state frame path.
- Test-first discipline applies (project memory: run every new test against
  a broken build first; a test that cannot fail is a suite failure). Do
  **not** run the headless compositor suites while the performance agent's
  runs are active — unit tests (`meson test` targets that create their own
  device or none) are fine; full harness runs wait.

---

## C1 — Colour-math library

**FEATURE.** A pure, dependency-free C library of colour primitives, plus a
GLSL twin, that every other contract consumes. No Vulkan, no wlroots types
in the C API (a small adapter may translate `wlr_color_primaries`).

**SCENE-LINEAR WORKING TARGET.** Defines it: linear-light BT.709/D65
premultiplied scene values, 1.0 = SDR reference white (ADR-002/003).

**INPUTS.** Electrical values in [0,1] (sRGB, gamma-2.2, BT.1886, PQ
signals); linear scene values (unbounded, signed); CIE xy primaries sets.

**OUTPUT.** Functions (C, `float`):

```
az_srgb_eotf / az_srgb_ieotf        piecewise IEC 61966-2-1, exact constants
az_gamma22_eotf / _ieotf            pure power 2.2
az_bt1886_eotf / _ieotf             Lmin 0.01, Lmax 100 (match fx_vk's)
az_pq_eotf / az_pq_ieotf            ST 2084, m1=0.1593017578125,
                                    m2=78.84375, c1=0.8359375,
                                    c2=18.8515625, c3=18.6875;
                                    eotf output 1.0 == 10000 cd/m²
az_hlg_eotf                         decode only (ADR-000 scope)
az_mat_from_primaries(src,dst,out9) absolute-colorimetric 3×3 via XYZ,
                                    Bradford NOT applied (both D65 in M5)
AZ_MAT_709_TO_2020 / _2020_TO_709   precomputed constants, tested against
                                    az_mat_from_primaries
az_tonemap(v3, knee, peak)          ADR-009 curve, single scale on max
                                    channel, C¹ at knee
az_ign(x, y)                        interleaved gradient noise, matching
                                    dither.glsl bit-for-bit semantics
```

**FORMAT.** `src/render/color/az_color.h` + `az_color.c` (compiled into
asteroidz and into tests standalone); `src/render/vulkan/shader/src/color.glsl`
with the same functions for shaders. The GLSL twin is hand-written, not
generated, but the parity test (below) makes drift a test failure.

**INVARIANTS.** Pure functions; no I/O; no state; alpha never appears in
any signature (transfer functions take RGB only, ADR-005); every `*_eotf`
clamps input to [0,1]; `az_tonemap` is the identity for max(v) ≤ knee and
for peak ≤ 1.

**PERFORMANCE CONSTRAINTS.** None at the library level (CPU use is
tests/screenshot); GLSL twin: each decode ≤ ~12 ALU, PQ ≤ 2 pow(); no
texture fetches.

**TESTS** (`tests/test-color-math.c`, meson unit test, no GPU):
round-trip |x − ieotf(eotf(x))| < 1e-6 over 4096 samples per tf; anchors:
`az_pq_ieotf(203/10000) == 0.5806888` ± 1e-4 (BT.2408), `az_srgb_eotf(0.5)
== 0.21404` ± 1e-5, matrix identity M₇₀₉→₂₀₂₀·M₂₀₂₀→₇₀₉ = I ± 1e-6;
monotonicity per tf; tone-map: identity below knee, C¹ continuity at knee
(numeric derivative), f(peak-ε) < peak, hue preservation (ratios exact).
GLSL parity: a compute-shader fixture evaluates the GLSL twin over the same
sample grid and compares ≤ 1 ULP-of-FP16 (this one needs a device; keep it
in the AVK unit-test family, `tests/test-avk-*.c` style).

**FILES.** New: `src/render/color/az_color.{h,c}`,
`src/render/vulkan/shader/src/color.glsl`, `tests/test-color-math.c`,
meson additions. Touches no existing file except `meson.build` and
`tests/meson.build`(-equivalent).

**CONFLICT STATUS.** NONE — all new files. **READY TO IMPLEMENT NOW.**

---

## C2 — Luminance-domain data model and resolver

**FEATURE.** `struct az_lum_domain` (ADR-006) and the pure resolver that
produces one from a surface's colour description + window rules + defaults.

**SCENE-LINEAR WORKING TARGET.** The domain is the *compiled recipe* for
taking one source's electrical premultiplied pixels into scene values.

**INPUTS.** (i) the scenefx scene-buffer colour fields
(`transfer_function`, `primaries`, `color_encoding`, `color_range`,
`max_cll` — all already populated by scenefx from wp-cm with the frog
fallback; see `scenefx/types/scene/wlr_scene.c:2884-2955`); (ii) the
client's resolved window rules (`sdr_white_scale` float default 1.0,
`hdr_gain` float default 1.0 — **new rules**, added to `rule-schema.h`
following the `force_hdr` pattern at `src/config/rule-schema.h:344-347`);
(iii) `scene_reference_luminance` (ADR-003).

**OUTPUT.** `struct az_lum_domain { enum az_tf tf; enum az_primaries
primaries; float scale; float content_peak; }` — 16 bytes, POD, no
pointers. Scale semantics per ADR-006 (SDR: sdr_white_scale; PQ:
10000/ref × hdr_gain; EXT_LINEAR: 80/ref × hdr_gain). Untagged → `{AZ_TF_SRGB,
AZ_PRIM_BT709, sdr_white_scale, 0}` (ADR-004). `content_peak` in scene
units from max_cll when present, else 0 (= unknown; consumers treat as
"no per-source ceiling").

**FORMAT.** `src/render/az_lum.h` (header-only or +.c), consumed later by
the scene walk. The resolver signature takes plain values, not wlr types:
`az_lum_domain az_lum_resolve(const struct az_lum_source_desc *, const
struct az_lum_rules *, float scene_ref_nits)` so it unit-tests without a
compositor.

**INVARIANTS.** Resolution happens at surface-commit/rule-apply time, never
per frame. A domain never encodes anything about an output. `scale > 0`
always; tf/primaries always valid enums (unknown protocol values collapse
to the untagged default, logged once per surface).

**PERFORMANCE CONSTRAINTS.** Zero frame-path work beyond copying 16 bytes
into a command; no allocation.

**TESTS** (`tests/test-lum-domain.c`, no GPU): table-driven — untagged,
sRGB-tagged, GAMMA22-tagged, PQ with/without max_cll, EXT_LINEAR, each ×
{no rules, sdr_white_scale 2.0, hdr_gain 0.5}; assert exact `scale` values
(e.g. PQ at ref 203 → 49.2611 ± 1e-4); reference-luminance sweep asserts
PQ scale ∝ 1/ref while SDR scale is invariant (ADR-003 falsifier's unit
form). Premise test: feed a garbage tf enum, assert fallback + single log.

**FILES.** New: `src/render/az_lum.h` (+`.c` if needed),
`tests/test-lum-domain.c`. Additive: `src/config/rule-schema.h` (two new
rules), `src/config/parse_config.h` (rule fields, following
`force_hdr`/`hdr_max_luminance` patterns at `parse_config.h:77,133-135,
2536-2543`), `src/asteroidz.c` `APPLY_INT_PROP`-adjacent float apply
(`asteroidz.c:2621` area), docs + manpage for the new rules (project rule:
docs in the same commit).

**CONFLICT STATUS.** LOW — `rule-schema.h`, `parse_config.h`,
`asteroidz.c` rule-application are not perf-owned. The *consumption* of
the domain (attaching to `avk_cmd` in `avk_scene.h` + the walk in
`az_avk.h`) is **DEFERRED — PERFORMANCE OWNED** (`az_avk.h`;
`avk_scene.h` forces owned-file recompiles, ADR-013). **READY TO
IMPLEMENT NOW** up to and including the resolver + rules; stop before the
command stream.

---

## C3 — Output-colour-state model

**FEATURE.** `struct az_output_color_state` (ADR-012) and the pure
derivation from monitor state.

**SCENE-LINEAR WORKING TARGET.** The per-output half of the pipeline: how
scene values leave the scene.

**INPUTS.** Per output: `m->hdr` (0/1), render format (`XRGB8888` /
`XRGB2101010`), ICC presence (`m->icc_transform != NULL`),
`m->hdr_max_luminance` (cd/m², 0 = unset → 1000 default, matching
`frog-color-management.h:211`), `config.sdr_reference_luminance` (0 →
203), `config.sdr_saturation`, and the C5 probe result
(`scanout_srgb_view_ok`).

**OUTPUT.** The ADR-012 struct: `path` (A/B per the ADR-001 decision
table), `encode_tf`, 3×3 `matrix` (identity for SDR; M₇₀₉→₂₀₂₀ ×
saturation matrix for HDR — saturation composed exactly as
`scenefx wlr_scene.c:4104-4126` does, reimplemented via C1), `ref_nits`,
`peak_scene = hdr_max_luminance / ref_nits` (1.0 for SDR), `dither_q`.
ICC outputs return `path = FALLBACK` (AVK keeps refusing, ADR-000).

**FORMAT.** `src/render/az_output_color.h` (header, pure function +
struct), no Vulkan objects in it — the Path-B intermediate image handle
lives renderer-side (C6), keyed by output, NOT in this struct (keeps the
model testable without a device).

**INVARIANTS.** Derivation runs on output-state change
(`mon_state_apply_color` call sites, hotplug, config reload), never per
frame. `path` never changes within a frame. SDR output ⇒ `peak_scene ==
1.0` exactly. The struct never stores PQ-encoded anything.

**PERFORMANCE CONSTRAINTS.** Derivation ≤ a few µs (matrix maths), off the
frame path.

**TESTS** (`tests/test-output-color.c`, no GPU): table — SDR 8-bit + probe
ok → Path A; SDR 8-bit + probe fail → Path B/sRGB/dither 1/255; SDR
10-bit → Path B/sRGB/1/1023; HDR → Path B/PQ/M₇₀₉→₂₀₂₀/1/1023, peak_scene
= 1000/203 at defaults; ICC → FALLBACK; saturation ≠ 1 changes only the
matrix; assert matrix rows against C1 constants.

**FILES.** New: `src/render/az_output_color.h`,
`tests/test-output-color.c`. Wiring the derivation call into
`mon_state_apply_color`/`createmon` is additive in `asteroidz.c` (not
perf-owned) but pointless before C6 integration — implement the model +
tests now, wire when C6 integrates.

**CONFLICT STATUS.** NONE for the model. Consumption in `avk_render.c` /
`az_avk.h` **DEFERRED — PERFORMANCE OWNED**. **READY TO IMPLEMENT NOW.**

---

## C4 — CPU reference implementation (the oracle and the SDR gate)

**FEATURE.** A CPU implementation of the *entire* pixel pipeline — source
electrical premultiplied → decode (C2 domain) → composite (source-over,
premultiplied linear) → output transform (C3 state) → electrical output —
used as the oracle for every GPU result, and the executable form of "prove
SDR before HDR".

**SCENE-LINEAR WORKING TARGET.** The normative definition. When GPU and
oracle disagree beyond stated bounds, the GPU is wrong (or the oracle's
test is, which the break-test discipline flushes out).

**INPUTS.** Test scenes: small (≤ 256²) synthetic layer stacks — each
layer: pixels (electrical, premultiplied), an `az_lum_domain`, dst box,
opacity; plus an `az_output_color_state`.

**OUTPUT.** An electrical output image (pre-dither, float), plus every
named intermediate (post-decode scene image per layer, post-composite
scene image) exposed for step-wise assertions (the ADR-008 falsifier needs
the intermediates).

**FORMAT.** `src/render/color/az_color_ref.{h,c}` — compiled into tests
only (not linked into asteroidz); double precision internally.

**INVARIANTS.** Implements exactly ADR-005 ordering and ADR-008 step
order; shares C1 for all primitives (so a C1 bug cannot hide — anchor
tests in C1 are against published constants, not against itself).

**PERFORMANCE CONSTRAINTS.** None (test-time only).

**TESTS** (`tests/test-color-pipeline.c` — the M5 gate):
1. **SDR round-trip gate**: opaque sRGB source, identity domain, Path-A
   state → output == input bit-exactly (float); through the GPU fixture
   (when C6/C7 exist) ≤ 1 8-bit code. **HDR10 output work may not be
   enabled in the compositor until this gate is green on-GPU.**
2. Translucent gate: 50%-alpha grey over black vs closed-form (ADR-005
   falsifier numbers).
3. PQ anchors: 203-nit and 1000-nit patches through HDR state → expected
   PQ codes ± 1/1023 (ADR-008 falsifier).
4. Straddling-window bound: same stack through SDR state and HDR state,
   decode both, assert divergence structure (identical below scene 1.0).
5. Tone-map and gamut vectors (ADR-009/010 falsifiers).
6. Reference-luminance sweep (ADR-003 falsifier, unit form).
7. **PQ-never-internal grep gate**: fail if PQ-encode identifiers appear
   in any shader source other than the output pass file list.
8. Break tests: each of 1–5 first run against a deliberately broken
   pipeline variant (e.g. decode skipped, premultiply order swapped) and
   must FAIL — per the tests-must-fail rule; keep the broken variants as
   `#ifdef` fixtures, and re-run breaks in every full run (a green break
   run is a suite failure).

**FILES.** New: `src/render/color/az_color_ref.{h,c}`,
`tests/test-color-pipeline.c`, small PPM/raw fixture writer shared with
the existing oracle's PPM code style (do not touch `avk_oracle.c`).

**CONFLICT STATUS.** NONE — new files only. **READY TO IMPLEMENT NOW.**

---

## C5 — Capability probing

**FEATURE.** Device- and format-level probes M5 decisions depend on,
recorded once at init, logged in one line, readable at runtime.

**INPUTS.** The Vulkan physical device; the dmabuf format/modifier table.

**OUTPUT.** Extend `struct avk_caps`-equivalent
(`src/render/vulkan/device/avk_device.h:55-63` area) with:
`fp16_attach_blend_sample` (bool: `R16G16B16A16_SFLOAT` has
COLOR_ATTACHMENT_BLEND + SAMPLED_FILTER_LINEAR, optimal tiling),
`rgb10a2_attach` (bool), and per-modifier `srgb_mutable` **for scanout
buffers** — the existing table records it for sampling
(`avk_format_table.c:160-186`); the probe must answer the *attachment*
question: can the swapchain image be created MUTABLE with an `_SRGB` view
usable as a colour attachment for the modifiers KMS actually selects.
Result surfaced per-output at first frame as `scanout_srgb_view_ok` (C3
input).

**FORMAT.** Additions to `device/avk_phys.c`, `device/avk_device.c` (log
line), `dmabuf/avk_format_table.c` (attachment-usage variant of the
existing probe). One `AVK_INFO` line:
`avk: color caps fp16=1 rgb10a2=1 scanout_srgb(mod=XXXX)=1`.

**INVARIANTS.** Probes run at init/import time only; a probe failure
selects a fallback path (C3), never an error. No behaviour change in this
contract alone — pure information.

**PERFORMANCE CONSTRAINTS.** Init-time only; zero frame-path cost.

**TESTS.** Unit: probe result struct is populated (non-UNKNOWN) for every
format the table advertises; headless device fixture asserts fp16 probe
true on RADV (and the test *skips*, not passes, on devices without
Vulkan). The scanout-modifier answer on the live GPU is the audit's
UNKNOWN — record the observed value in the M5 log when first run.

**FILES.** `src/render/vulkan/device/avk_phys.c`, `avk_device.{h,c}`,
`src/render/vulkan/dmabuf/avk_format_table.{c,h}`, test in the
`tests/test-avk-*` family.

**CONFLICT STATUS.** LOW — `device/` and `dmabuf/` are **not** in the
perf-owned list; changes are additive fields + one probe function.
**READY TO IMPLEMENT NOW** (coordinate merge timing anyway since owned
files include these headers transitively).

---

## C6 — Output-encode pass (Path B), pipeline + shader

**FEATURE.** The single final transform: working intermediate → scanout
buffer. Shader `output_encode.frag` + pipeline creation in `pipeline/`,
callable as one function given a command buffer, source view, target view,
`az_output_color_state`, and a damage region.

**SCENE-LINEAR WORKING TARGET.** Consumes it; this is the only place scene
values become output electrical values on Path B.

**INPUTS.** Sampled working intermediate (premultiplied scene values;
treated as opaque — composition is complete); `az_output_color_state`
(push constants: 3×3 matrix, ref_nits/10000, peak_scene, knee, dither_q,
encode-tf selector as a specialisation constant so each output's pipeline
is branch-free); damage rects (scissored draws, exactly the fx_vk pattern
`pass.c:438-448`).

**OUTPUT.** Encoded pixels in the scanout view: ADR-008 step order
(tone map → matrix+clamp → luminance anchor → inverse EOTF → IGN dither →
write). SDR-10-bit variant: identity matrix, peak 1, sRGB inverse EOTF,
1/1023 dither.

**FORMAT.** New files: `src/render/vulkan/pipeline/avk_output_encode.{c,h}`,
`src/render/vulkan/shader/src/output_encode.frag` (includes `color.glsl`);
uses the existing embed/build machinery (`shader/embed.py`, `meson.build`
shader list). Fullscreen-triangle vertex reuse or a 4-vert strip matching
`quad.vert` conventions — implementer's choice, but no vertex buffers.

**INVARIANTS.** Exactly one such pass per Path-B output per frame; it
reads only the intermediate and writes only the scanout target; it is the
only shader outside C1's test fixtures containing a PQ inverse EOTF (C4
gate 7 enforces). Alpha channel of the target: written as 1.0 (X formats).
No allocation, no descriptor-set creation per frame (the intermediate's
sampled set is cached on the image exactly like every other
`avk_image.sampler_set`).

**PERFORMANCE CONSTRAINTS.** One render-pass instance; draws = number of
damage rects (bounded by the existing damage machinery); bytes = 8 read +
4 written per damaged pixel; ALU budget ≤ ~40/px (decode-free: matrix 9
MAD + tonemap 8 + PQ pow×2 + dither 4); zero barriers beyond the graph's
attachment→sampled transition of the intermediate (declared via the
existing `avk_graph` model when integrated).

**TESTS.** Headless fixture (own device, `tests/test-avk-*` family — NOT
the compositor harness): render known scene-value fixtures through the
pass, compare against C4 oracle intermediates: PQ anchors ± 1/1023
pre-dither; dither zero-mean + banding-run metrics (ADR-011 falsifier);
scissor test: pixels outside damage untouched (LOAD semantics); break
test: run with matrix/anchor swapped and assert the oracle catches it.

**FILES.** New files above + `meson.build` shader/embed additions. The
**call** into the frame (`avk_render.c`: render into intermediate instead
of target, then invoke this pass; `az_avk.h`: stop refusing
`image_description`; `avk_oracle.c`: teach the oracle the second stage) is
**DEFERRED — PERFORMANCE OWNED**.

**CONFLICT STATUS.** Pass itself: NONE (new files, unowned dirs).
Integration: **DEFERRED — PERFORMANCE OWNED** (`avk_render.c`,
`az_avk.h`, `avk_oracle.c` timing; also blur-transient format switch in
`avk_blur.c`/`avk_render.c`). **SHADER+PIPELINE READY TO IMPLEMENT NOW.**

---

## C7 — Source decode path (texture pipeline variants)

**FEATURE.** The decode side of ADR-004/005/006 in AVK's texture pipeline:
per-domain pipeline variants and the `_SRGB`-view sampling fast path.

**SCENE-LINEAR WORKING TARGET.** Produces scene values from client pixels;
after this contract, everything downstream of a texture draw is linear.

**INPUTS.** `avk_image` (+ its dormant `srgb_mutable` machinery,
`avk_dmabuf.c:388-401`); the command's `az_lum_domain`; the output's path
(A/B) at pipeline-selection time.

**OUTPUT.** Premultiplied scene values entering the fixed-function blend.
Pipeline variants (specialisation constants, mirroring fx_vk's
`texture_transform` approach, `scenefx pass.c:977-1021`):
- `OPAQUE_SRGB` — sample via `_SRGB` view, α forced 1, no shader decode
  (ADR-005 fast path; requires image `srgb_mutable` and the draw's
  opaque-region test);
- `SDR_SHADER` — un-premultiply → sRGB/G22/BT1886 decode → ×scale →
  re-premultiply;
- `HDR_SHADER` — un-premultiply → PQ decode → ×scale → M₂₀₂₀→₇₀₉ →
  [Path A only: tonemap to 1.0] → re-premultiply.
A second sampled view (`view_srgb`) and its descriptor pair are added to
`avk_image` beside the existing `view`/`sampler_set` cache
(`avk_image.h:39-69`) — created lazily, exactly the existing pattern
(`avk_pipeline.c:488-550`).

**INVARIANTS.** Decode once per sampled texel; no draw decodes twice; the
choice of variant is per-draw at record time, never per-pixel. The debug
env `AZ_DECODE_GAMMA22=1` flips untagged decode to 2.2 for the ADR-004
A/B — temporary, removed when the falsifier is settled (no back-compat
shims).

**PERFORMANCE CONSTRAINTS.** Fast path: identical cost to today ± view
switch. Shader paths: ≤ 12 extra ALU + 1 divide per texel; no extra
passes; descriptor growth ≤ 2 sets per image (the `_SRGB` pair), lazily.
Pipeline count: ≤ 3 variants × existing pipeline axes — verify against
the 128-byte push-constant budget (`push.glsl:10-14`): `scale` rides in a
free slot per the push-constant overlay table; if no slot is free for a
given pipeline, a specialisation constant fallback is acceptable for
Path-A tonemap ceiling (compile-time 1.0).

**TESTS.** Unit fixture (own device): each variant vs C4 oracle per-layer
intermediates (post-decode scene image), ≤ FP16 ULP bounds; the
translucent gate (C4 test 2) on both fast and shader paths — the fast
path must *fail* it if forced onto translucent content (break test
proving the opaque-region guard matters); X-format alpha still forced 1.

**FILES.** New shader variants in `shader/src/texture.frag` (or a new
`texture_hdr.frag` — implementer's choice; `shader/` is unowned);
`pipeline/avk_pipeline.{c,h}` additions (unowned); `image/avk_image.h`
view addition (unowned). The scene-walk attachment of domains and the
per-draw variant selection in `avk_render.c`/`az_avk.h`: **DEFERRED —
PERFORMANCE OWNED**.

**CONFLICT STATUS.** MEDIUM — the files are unowned but `avk_render.c`
constructs pipelines via `avk_pipelines_*`; additive API only (new
functions, no signature changes to existing ones), so the perf agent's
tree keeps compiling. Implement the variants + image-view machinery now;
wire later.

---

## Integration order (for the deferred series, post-freeze)

1. C5 probe results observed on the live GPU (answers the Path-A UNKNOWN).
2. Wire C3 derivation into output state changes.
3. `avk_scene.h`: add `az_lum_domain` to texture commands; `az_avk.h`
   walk fills it (C2 resolver).
4. `avk_render.c`: per-draw variant selection (C7).
5. Path A live; run C4 SDR gate on-GPU. **Gate.**
6. `avk_render.c` Path B: intermediate target + C6 pass; blur transient
   format follows the path; `avk_oracle.c` learns the encode stage.
7. `az_avk.h`: remove the `image_description` refusal. HDR live behind
   `hdr_resolve` exactly as today.
8. Docs + manpages + harness updates in the same commits (project rule);
   live testing only with explicit user warning per the live-session
   rules.
