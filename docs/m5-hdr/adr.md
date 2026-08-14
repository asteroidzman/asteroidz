# M5 Architecture Decision Records

Each ADR: CONTEXT / OPTIONS / DECISION / REJECTED ALTERNATIVES / CORRECTNESS
CONSEQUENCES / PERFORMANCE CONSEQUENCES / FUTURE COMPATIBILITY / FALSIFIER.
Terminology is exact throughout; the words used here are the words the code
must use.

**Vocabulary fixed for all of M5** (no synonyms permitted in code or docs):

- *scene value* — a premultiplied, linear-light RGB value in the working
  space of ADR-001/002/003. Never "brightness", never "HDR value".
- *electrical value* — a transfer-function-encoded value (sRGB signal, PQ
  signal). Never "gamma value".
- *EOTF* — electrical → optical (decode). *inverse EOTF* — optical →
  electrical (encode). "OETF" is not used; every encode in this project is
  the inverse of a display EOTF.
- *SDR reference white* — the luminance, in cd/m², that scene value 1.0
  represents (ADR-003). Never "paper white" in code.
- *output peak* — the maximum luminance, in cd/m², the output transform is
  allowed to emit.

---

## ADR-000 — Scope of M5

**CONTEXT.** "Colour" could absorb arbitrarily much. The milestone table
(`docs/vulkan-native-architecture.md:405`) lists FP16 linear pipeline,
PQ/HLG, tone/gamut mapping, LUT/ICC, 10-bit output. Some of that is better
placed elsewhere, and the audit shows some of it has no consumer today.

**DECISION.** M5 delivers, in AVK: the scene-linear working space (ADR-001..005),
per-window luminance domains (ADR-006), SDR outputs (ADR-007), HDR10/PQ
outputs (ADR-008), static tone mapping (ADR-009), matrix gamut mapping
(ADR-010), output-stage dither (ADR-011), simultaneous mixed outputs
(ADR-012). It does **not** deliver:

- **ICC / 3D-LUT outputs.** They stay on the fx_vk fallback exactly as today
  (`az_avk_output_supported` keeps refusing `color_transform != NULL`); the
  refusal predicate merely stops refusing `image_description != NULL` once
  ADR-008 ships. ICC-in-AVK is M6, as the code comment already says
  (`src/render/az_avk.h:2408-2410`).

  > **AMENDED 2026-08-14 (M6B/D2+G2).** Half of this is now delivered and the
  > half matters. A **matrix-shaper** profile is absorbed by AVK: C3 derives
  > `B-encode` + `AZ_TF_LUT1D`, the encode pass applies the profile's matrix
  > and its measured curve, and `az_output_color_transform()` withholds the
  > wlroots transform so the profile is applied exactly once. A **cLUT**
  > profile is still refused, still by `color_transform != NULL`, and still to
  > fx_vk — D2's revival condition (a real cLUT profile for a connected
  > display existing on this machine) is unchanged. The predicate above is
  > therefore no longer the whole story: the refusal is now decided by whether
  > the profile reduces, not by whether one is present.
- **HLG.** No source on this machine emits it (gamescope emits PQ; wp-cm
  advertises what we choose). The colour-math library (C1) includes HLG
  decode functions for completeness of the library's test surface, but no
  pipeline work consumes them in M5.
- **Adaptive/scene-analysing tone mapping**, dynamic metadata (HDR10+),
  and `set_luminances` support (upstream wlroots rejects it).
- **Direct scanout colour policy changes.** Scanout bypass of composition is
  untouched; a PQ fullscreen surface scanning out directly keeps today's
  behaviour and metadata forwarding (`mon_state_apply_color`).

**FALSIFIER.** If a real client on this machine is found emitting HLG or
requiring ICC-managed HDR during M5, the scope line moves and this ADR is
revised — not silently worked around.

---

## ADR-001 — Internal working representation and format

**CONTEXT.** AVK composites in the encoded domain into the scanout buffer
directly (audit §1). M5 needs a working representation in which blending,
blur, gradients and shadows are arithmetic on light, and which can hold HDR
headroom (values > 1) and wide-gamut values (components < 0, see ADR-002).
The performance floor: fx_vk already pays a whole-output FP16 intermediate +
output pass on colour-managed outputs; GLES pays encoded single-pass on SDR.
AVK must not regress either comparison. Two different output classes have
two different cheapest-correct paths.

**OPTIONS.**

1. **One universal FP16 intermediate + output pass on every output.**
   Simple, uniform; but SDR-only outputs pay +8 B/px write and a full extra
   pass they don't need, and the SDR floor (GLES/AVK-today: composite
   straight into the 4 B/px scanout buffer) is lost.
2. **Per-output path selection**: outputs that need a non-trivial output
   transform (PQ, 10-bit SDR encode, later ICC) composite into a working
   intermediate and run one output-encode pass; plain 8-bit sRGB SDR outputs
   composite scene-linearly *directly into the scanout buffer through an
   `_SRGB` image view*, using hardware decode on sampled sources and
   hardware encode + linear-domain fixed-function blending on the
   attachment (Vulkan guarantees blending happens on linear values for
   `_SRGB` attachments; framebuffer reads are decoded before blending).
3. Keep encoded compositing and convert per effect when an effect wants
   linear. Violates the decode-once invariant; rejected without a scorecard.

**DECISION.** Option 2. Precisely:

- **Path A (direct-sRGB)** — used when the output's render format is
  `DRM_FORMAT_XRGB8888`, the output has no image description and no ICC
  transform, **and** the scanout buffer's modifier supports a mutable
  `_SRGB` view (probed, C5). All draws sample sources decoded to linear
  (ADR-004/005), blend in linear light via the `_SRGB` attachment view, and
  the hardware encodes on write. Storage stays 8-bit sRGB-coded — the same
  quantisation as today, so precision is not regressed and not improved.
- **Path B (intermediate + encode pass)** — used for every other AVK-eligible
  output (HDR10/PQ, 10-bit SDR, and Path-A outputs whose `_SRGB` view probe
  failed). Composition renders into a persistent whole-output working image;
  one final damage-scissored output-encode pass (contract C6) writes the
  scanout buffer.

**Working-format selection criteria** (Path B intermediate and Path B blur
transients). The format must satisfy all of:

  (a) `VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT` and
      `VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT` on optimal tiling
      (probed at device init, C5);
  (b) an alpha channel — the premultiplied compositing model, prefix-replay
      blur of translucent stacks, and the shadow-exclusion logic all carry
      meaningful alpha;
  (c) relative precision ≤ 2⁻¹⁰ across [0, output-peak/REF] so a 10-bit PQ
      encode is not banded by the intermediate (PQ's most sensitive region
      needs ~11 significand bits; FP16 has 11);
  (d) representable range ≥ 10000/203 ≈ 49.3 with headroom;
  (e) negative-component capable (ADR-002 wide-gamut encoding);
  (f) minimal bytes/px subject to (a)–(e).

`R16G16B16A16_SFLOAT` (8 B/px) satisfies (a)–(e) and is the only *standard*
Vulkan format that does: `B10G11R11_UFLOAT` fails (b) and (e) and has a
5-bit-significand blue channel failing (c); `A2B10G10R10_UNORM` fails (d)
and (e); `R16G16B16A16_UNORM` fails (d) and (e); `R32G32B32A32_SFLOAT`
doubles (f) for no requirement. **The decision is the criteria, not the
format**: the implementing agent probes (a) and measures (c) via the C4
oracle and the banding tests, and records the measured bandwidth cost. If
measurement contradicts (c) for FP16 — it will not — the criteria pick the
next format up, not a prose debate.

**REJECTED ALTERNATIVES.** Option 1 (uniform FP16) — fails the GLES floor on
SDR outputs for zero correctness gain (Path A is equally linear). Encoded
compositing with per-effect conversion — repeated conversions, the exact
anti-pattern the invariants name. Shared-exponent RGB9E5 — not blendable as
an attachment on most hardware, no alpha.

**CORRECTNESS CONSEQUENCES.** Blending, blur, AA-coverage arithmetic and
gradient interpolation become arithmetic on light on every AVK output. Known
visible changes, to be validated deliberately: dark-fringe reduction on AA
edges; blur of high-frequency bright-on-dark content gets darker (physically
correct — and the darken-only shadow clamp of f8be42c must be re-tested
against linear-domain blur, since its calibration story was encoded-domain,
see C6 tests); gradient midpoints shift. Path A quantises intermediate
composite results to 8-bit sRGB codes exactly as today (equal precision,
no regression, no gain).

**PERFORMANCE CONSEQUENCES.** Path A: zero additional passes, zero
additional bytes, zero new allocations vs today — decode/encode are
free-function hardware view operations; the only cost is losing the
UNORM-view sampler descriptor cache entry in favour of an `_SRGB`-view one
(one-time per image, C7). Path B at 3840×2160: one persistent 66.4 MB
intermediate per output (8.29 Mpx × 8 B); per frame, draws write 8 B/px
instead of 4 over the damaged region, plus the encode pass reads 8 B/px +
writes 4 B/px over damage. Full-frame worst case ≈ 133 MB additional traffic
per 4K frame vs today's direct path; at 144 Hz fullscreen-damage that is
≈ 19 GB/s against Navi 31's ≳ 800 GB/s — and it is exactly the cost fx_vk
already pays for the same frames today, so the fallback-parity floor holds
by construction. Descriptor cost: one cached set for the intermediate
(sampled by the encode pass), no per-frame updates.

**FUTURE COMPATIBILITY.** Path B's encode pass is where M6's ICC 3D LUT
slots in (one more sampled descriptor + LUT lookup in the same shader).
Direct scanout of client buffers is untouched by either path.

**FALSIFIER.** (i) If the C5 probe shows scanout dmabufs on this GPU's
modifiers cannot carry `_SRGB` views, Path A is dead code and all outputs
take Path B — the design survives, the SDR floor claim must then be
re-measured. (ii) If measured Path-B frame time on a 4K SDR-10-bit output
exceeds the fx_vk fallback's frame time for the same scene, the intermediate
or the encode pass is misimplemented — the architecture predicts parity or
better.

---

## ADR-002 — Working primaries and white point

**CONTEXT.** A working space needs fixed primaries. Sources: overwhelmingly
sRGB/BT.709; HDR video is BT.2020-contained (usually P3-mastered). Outputs:
sRGB SDR and BT.2020 PQ. fx_vk blends in sRGB-primaries linear
(audit §3.5); its per-texture matrix converts source primaries → sRGB
absolute-colorimetrically.

**OPTIONS.** (1) BT.709/sRGB primaries, D65, components unbounded above and
**below zero** (the scRGB convention: wide-gamut colours are valid negative
coordinates). (2) BT.2020 primaries, D65, all physical colours non-negative.
(3) XYZ / a perceptual space — not blendable meaningfully, rejected.

**DECISION.** **BT.709 primaries, D65 white point, unbounded scene values
(scRGB-style)**. Formally: scene RGB is linear-light BT.709/D65; any real
colour is representable, wide-gamut colours carry negative components; the
working format (ADR-001) is signed float precisely to hold them.

**REJECTED ALTERNATIVES.** BT.2020 working primaries: every sRGB source —
which is nearly every pixel of every frame — would need a 3×3 on decode
*and* Path A's free hardware decode would become impossible (hardware sRGB
decode yields 709-primaries values; a further matrix needs a shader, and
fixed-function blending into the scanout buffer could then never be used).
The BT.2020 benefit (non-negative wide gamut) buys nothing measurable:
FP16 holds negatives exactly as well as positives, and blur/blend arithmetic
is linear so negative components compose correctly.

**CORRECTNESS CONSEQUENCES.** BT.2020 sources decode through one 3×3
(2020→709, folded into the decode shader they already need for PQ — zero
extra passes) and may produce negative scene components; those survive
blending and blur correctly in float and are gamut-mapped at the output
(ADR-010). Effects that assume non-negative input (the Oklab saturation
cube-root, `blur.glsl:82`) must clamp their *local* copy, as they already do
(`max(rgb, 0)` at `blur.glsl:118`) — the clamp is an effect-local domain
restriction, not a pipeline-wide invariant.

**PERFORMANCE CONSEQUENCES.** Zero for sRGB sources on both paths. One
mat3×vec3 (9 MADs) per texel for BT.2020 sources, folded into the PQ decode
shader (C7). Output side: one 3×3 in the encode pass for BT.2020 outputs
(C6), already required for luminance scaling.

**FUTURE COMPATIBILITY.** P3 outputs (M6+) are one more matrix constant.
scRGB clients (`EXT_LINEAR` frog surfaces, audit §3.7) are natively in this
space already — decode is a pure luminance scale.

**FALSIFIER.** Render the C4 oracle's wide-gamut test card (BT.2020 laser
primaries) through composite + blur onto a PQ output: if hue error vs the
CPU reference exceeds 1 JND (ΔE00 > 2) anywhere, negative-component handling
is broken somewhere in the chain — the working-primaries choice predicts
exact preservation through linear ops.

---

## ADR-003 — Luminance units: what scene 1.0 means

**CONTEXT.** The scene is shared by all outputs (invariant 2), so its unit
cannot be any single output's nits. The fallback model already uses
"1.0 = blend reference" (audit §3.5). Precision demanded: what SDR reference
white, HDR paper white, a 1000-nit highlight, and output peak each map to.

**DECISION.** Scene values are **relative to SDR reference white**:

- Scene value 1.0 ≡ SDR reference white, a dimensionless anchor.
- One scene-global constant `scene_reference_luminance` (cd/m²) gives the
  anchor a nit value **only at absolute↔relative boundaries**. It is
  `config.sdr_reference_luminance` when set, else 203 cd/m² (the ITU-R
  BT.2408 diffuse-white reference, and wlroots' PQ default). It is a *scene*
  property, not a per-output one — two outputs disagreeing about what a nit
  of scene 1.0 means would make PQ sources render at different absolute
  luminance on each, splitting a straddling window's intent.
- **SDR reference white**: an SDR source's electrical 1.0 decodes to scene
  1.0 (× its window's `sdr_white_scale`, ADR-006).
- **HDR paper white**: PQ content's diffuse white, mastered at ~203 cd/m²,
  decodes to 203/`scene_reference_luminance` — exactly 1.0 at the default,
  i.e. HDR paper white and SDR UI white coincide by default and diverge only
  if the user moves `sdr_reference_luminance`.
- **A 1000-nit highlight**: PQ electrical → 1000 cd/m² → scene
  1000/203 ≈ 4.93 (at the default anchor).
- **Output peak**: per-output, in cd/m² (`m->hdr_max_luminance`, panel EDID
  or monitor rule); expressed in scene units as
  `peak_out = hdr_max_luminance / scene_reference_luminance` — the tone-map
  ceiling (ADR-009). For SDR outputs `peak_out = 1.0` by definition.
- **PQ encode** (ADR-008) maps scene v → PQ⁻¹(v ×
  `scene_reference_luminance` / 10000), so scene 1.0 lands at exactly
  `scene_reference_luminance` nits on the wire.

**OPTIONS REJECTED.** Absolute nits as the scene unit (scene 1.0 = 1 cd/m²
or = 10000): forces every SDR source through a luminance multiply on decode
*and* makes Path A impossible (hardware decode yields [0,1], which must then
mean SDR-relative); float precision would also be spent where PQ doesn't
need it. Per-output scene anchors: splits shared-window intent, violates
invariant 2.

**CORRECTNESS CONSEQUENCES.** SDR-only scenes on SDR outputs never touch a
luminance constant at all — decode [0,1], composite, encode [0,1]; the C4
round-trip gate is a pure transfer-function identity. Changing
`sdr_reference_luminance` at runtime (existing keybind) rescales the
PQ-decode constant and the PQ-encode constant *in opposite directions*, so
HDR media's absolute nits are invariant under it while SDR UI moves — the
correct semantic, and it must be tested (C4).

**PERFORMANCE CONSEQUENCES.** One multiply folded into HDR-source decode and
one into the encode pass; zero on SDR paths.

**FUTURE COMPATIBILITY.** HLG's relative system folds in as an OOTF-scaled
decode to the same relative space. `set_luminances`, if wlroots ever ships
it, refines per-surface decode constants (ADR-006 fields already hold them).

**FALSIFIER.** Play a PQ test pattern on the HDR output while sweeping
`set_sdr_luminance` 80→400: a patch mastered below the *lowest* reference
in the sweep (50 cd/m²) must hold its absolute luminance on an external
meter (or in the rawhdr capture) while UI white tracks the sweep — the
decode's 10000/ref and the encode's ref/10000 cancel exactly where the
tone curve is identity, and ADR-009's knee is at scene 1.0, which *is*
the reference. A patch above the reference (400 cd/m² at ref 80) must
NOT hold: it must rise monotonically with the reference and never exceed
its own mastered luminance, because raising the reference raises the knee
in absolute terms and compresses it less. If the below-reference patch
moves, the anchor is applied on the wrong side of the split; if the
above-reference patch holds constant, tone mapping is not being driven by
the reference-relative scene value. (An earlier form demanded a 1000-nit
patch hold constant across the whole sweep; that is unsatisfiable with
ADR-009's knee and fails on correct code — findings F6, gate 6.)

---

## ADR-004 — The untagged-SDR-surface assumption

**CONTEXT.** Almost every surface arrives untagged. Something must be
assumed about its electrical encoding. Today's stack is split: AVK assumes
nothing (identity passthrough), fx_vk/wlroots assume pure gamma 2.2 for
untagged and even for sRGB-tagged (frog maps sRGB→GAMMA22 deliberately,
audit §3.7). Vulkan hardware `_SRGB` views implement the *piecewise sRGB*
EOTF, not 2.2. The two curves agree at 0 and 1 and diverge most near black
(piecewise is lighter in the toe).

**OPTIONS.** (1) Untagged ⇒ piecewise sRGB EOTF, implemented by hardware
`_SRGB` views where possible. (2) Untagged ⇒ pure 2.2 power, implemented in
shader everywhere, matching today's fallback and gamescope's convention.

**DECISION.** **Untagged and sRGB-tagged SDR surfaces decode with the
piecewise sRGB EOTF** (IEC 61966-2-1: linear segment below 0.04045/12.92,
else ((V+0.055)/1.055)^2.4). Surfaces *explicitly* tagged GAMMA22 or BT.1886
via wp-cm decode with their declared curve (shader path, C7).

Rationale, in order of weight: (i) the SDR→SDR identity gate — with
piecewise decode and piecewise encode, Path A is hardware-exact
decode/encode of the *same* curve, and the C4 round-trip test can demand
bit-identity for opaque unscaled content, the strongest possible correctness
anchor for "prove SDR first"; (ii) it is what the hardware gives for free on
both the sampling and attachment side — the 2.2 option forfeits Path A
entirely (hardware cannot encode 2.2); (iii) content authored on sRGB-spec
displays intends the piecewise curve as its EOTF pairing on this machine's
actual SDR panel behaviour — while acknowledging the industry genuinely
disagrees here.

**REJECTED ALTERNATIVE.** GAMMA22 (option 2): keeps near-black appearance on
the HDR output identical to today's fx_vk fallback and to gamescope, but
costs a per-texel `pow()` on every sampled pixel of every frame, forfeits
hardware encode (so *every* output becomes Path B), and forfeits the exact
round-trip gate. The near-black difference is real and this ADR does not
pretend otherwise — it is the price of the identity gate, and the falsifier
below is the escape hatch. Per project policy, matching fx_vk is explicitly
not a design goal.

**CORRECTNESS CONSEQUENCES.** On SDR outputs (Path A): pixel-exact
round-trip, no visible change from today for opaque content. On HDR outputs:
untagged SDR content's shadows render slightly lighter than the same content
did under the fx_vk fallback (piecewise toe vs 2.2 toe; largest relative
difference is below electrical 0.1). The A/B must be looked at on the real
panel before HDR ships (falsifier).

**PERFORMANCE CONSEQUENCES.** Zero for the dominant case (hardware views).
Shader decode only for explicitly-tagged non-sRGB SDR surfaces and for the
translucent-texel path of ADR-005.

**FUTURE COMPATIBILITY.** wp-cm-tagged surfaces already carry their own tf;
nothing here changes for them. If a per-window rule `assume_gamma22` is ever
wanted for a stubborn client, ADR-006's domain struct has the field for it.

**FALSIFIER.** The A/B: same near-black test card (2%–10% grey ramps over
black) composited to the HDR output with decode = piecewise vs decode = 2.2
(a debug env flip in C7). If the piecewise rendering is judged visibly wrong
on this panel against the SDR rendering of the same card, the decision
flips to GAMMA22-for-untagged and Path A is retired — the ADR records the
cost of that flip up front (all outputs → Path B, round-trip gate weakens
to ±1 code).

---

## ADR-005 — Alpha and premultiplication ordering

**CONTEXT.** Wayland buffers are premultiplied **in the electrical domain**:
the texel is (a·R′, a·G′, a·B′, a). The EOTF is non-linear, so
EOTF(a·R′) ≠ a·EOTF(R′): decoding premultiplied electrical values through a
hardware `_SRGB` view is *wrong* wherever 0 < a < 1. fx_vk handles this by
un-premultiplying in shader before decode (`texture.frag:98-131`). Alpha
itself is coverage and must never pass through a transfer function
(invariant 4).

**DECISION.**

- **Canonical scene pixel: premultiplied linear** — (α·R, α·G, α·B, α) with
  R,G,B scene values (ADR-002/003), α coverage in [0,1].
- **Decode order for translucent sources** (shader, C7):
  un-premultiply → EOTF per channel on RGB only → × luminance-domain scale
  (ADR-006) → re-premultiply → × opacity. Exactly fx_vk's order, kept.
- **Opaque fast path**: a draw whose source texel is known opaque — X-format
  (alpha forced 1, `avk_image.h:52-54`), or the draw's rect lies inside the
  surface's opaque region — may sample through the hardware `_SRGB` view
  (a=1 makes hardware decode exact). The per-draw choice is a pipeline
  variant, not a per-pixel branch.
- **Alpha is never transformed.** No EOTF, no luminance scale, no tone map
  touches α. Opacity and AA coverage multiply the premultiplied vec4
  exactly as today (`texture.frag:30-42` keeps its semantics, now in linear).
- **Blend equation unchanged**: `(ONE, ONE_MINUS_SRC_ALPHA)` on both paths
  (fixed-function; on Path A the attachment `_SRGB` view makes the
  fixed-function blend operate on decoded linear values per Vulkan spec
  §"sRGB conversion").

**OPTIONS REJECTED.** (1) Hardware decode for everything, accepting the
translucent error: the error concentrates exactly on AA edges and
translucent UI — the most looked-at pixels on this desktop (the terminal runs
at 0.98 opacity, and the shadow-hole episode proved sub-2% alpha errors get
noticed). (2)
Straight (non-premultiplied) scene storage: breaks the single-blend-state
model and linear-filter correctness of premultiplied sampling.

**CORRECTNESS CONSEQUENCES.** Translucent content decodes exactly; the
un-premultiply divide at low α amplifies quantisation, so the shader clamps
decode input to [0,1] per channel — the PQ NaN lesson
(`scenefx texture.frag:39-50`) is inherited as a hard rule for every decode
function in C1.

**PERFORMANCE CONSEQUENCES.** The translucent path costs ~6 ALU + one
divide + one EOTF per texel over today's shader — only on draws not taken by
the opaque fast path. No extra passes, no extra bandwidth. Estimated
fraction of screen area on this desktop taking the slow path: the
translucent terminal + fading animations ≈ 10–30% typical; measure via the
existing timestamp infra.

**FUTURE COMPATIBILITY.** ICC (M6) slots after decode in the same ordering.
YCbCr sources (when advertised) produce electrical R′G′B′ from the
ycbcr-conversion sampler and enter this pipeline at the un-premultiply step
(they are effectively straight-alpha, α=1).

**FALSIFIER.** C4's translucent test: a 50%-alpha mid-grey over black must
match the CPU reference within 1 8-bit code on Path A and 0.5 code on
Path B. Hardware-decode-everything would miss by several codes; if the
implementation misses similarly, the ordering is being applied on the wrong
side of premultiplication.

---

## ADR-006 — Per-window luminance domains

**CONTEXT.** Invariant 3: per-window luminance is first-class. Today the
only per-window colour lever is the `force_hdr` rule (an output-mode lever,
not a mapping lever) and global `sdr_reference_luminance` /
`sdr_saturation`. The AVK command stream carries no colour state at all
(audit §1.5).

**DECISION.** Every texture draw carries a resolved **luminance domain**,
`struct az_lum_domain` (contract C2):

```
enum az_tf        { AZ_TF_SRGB, AZ_TF_GAMMA22, AZ_TF_BT1886, AZ_TF_PQ, AZ_TF_LINEAR_EXT }
enum az_primaries { AZ_PRIM_BT709, AZ_PRIM_BT2020 }   /* M5 set; extensible */

struct az_lum_domain {
    enum az_tf        tf;          /* electrical encoding of the source     */
    enum az_primaries primaries;
    float             scale;       /* linear gain into scene units, applied
                                      post-EOTF, pre-primaries-matrix       */
    float             content_peak;/* source's declared/derived max, in
                                      scene units; 0 = unknown              */
};
```

`scale` is the entire luminance model collapsed to one number, computed on
the CPU at resolve time:

- SDR (sRGB/G22/BT1886 source): `scale = sdr_white_scale` — the per-window
  rule, default 1.0. This is the per-window replacement for any global
  "hdr_brightness": brightening one SDR app on an HDR output is
  `sdr-white-scale:1.5` on that window, touching nothing else.
- PQ source: `scale = 10000 / scene_reference_luminance` (ADR-003), ×
  optional per-window gain rule (default 1.0).
- EXT_LINEAR (scRGB) source: `scale = 80 / scene_reference_luminance` ×
  optional per-window gain (scRGB 1.0 is defined as 80 cd/m²).

Resolution precedence (pure function, C2): wp-color-management surface
description → frog description (already normalised into wp-cm form,
`frog-color-management.h`) → untagged default (ADR-004) — then window rules
(`sdr-white-scale`, `hdr-gain`, future `assume-*` overrides) applied on top.
Subsurfaces resolve independently (each wlr surface has its own
description); a window rule applies to every surface of the client's tree.

**OPTIONS REJECTED.** A global HDR brightness slider (explicitly forbidden).
Storing raw wp-cm descriptions in the command stream: the renderer would
re-derive per frame what the CPU can resolve once per commit; the domain
struct is the compiled form.

**CORRECTNESS CONSEQUENCES.** Two windows of the same PQ video with
different `hdr-gain` render at different absolute luminance — intended. The
straddling-window invariant holds trivially: the domain is per-window, the
output transform is per-output, and they compose.

**PERFORMANCE CONSEQUENCES.** 16 bytes per texture command; resolution runs
on surface commit, not per frame; the shader consumes it as push-constant
floats already budgeted in C7. Zero allocations on the frame path.

**FUTURE COMPATIBILITY.** `set_luminances` (if ever implemented upstream)
refines `scale`/`content_peak` per surface with no structural change. HLG
adds one `az_tf` value.

**FALSIFIER.** Apply `sdr-white-scale:2.0` to one of two identical SDR
windows on the HDR output: exactly one brightens, by exactly 2× in the
rawhdr capture's linear decode, and the SDR output's rendering of the same
two windows ALSO differs by 2× pre-tone-map (one scene!). If the SDR output
shows them equal, a global path swallowed the domain.

---

## ADR-007 — SDR output mapping

**CONTEXT.** SDR outputs must render the scene-linear scene with today's
appearance for today's content (the regression floor) while defining what
happens to scene values > 1 (HDR media on an SDR output).

**DECISION.**

- **8-bit sRGB SDR output (Path A)**: composite directly through the
  `_SRGB` attachment view; encode is hardware piecewise sRGB; scene → wire
  is identity for [0,1] content. Values > 1: **tone-mapped at source
  decode** — a draw whose domain can produce values > 1 (PQ, EXT_LINEAR, or
  `sdr_white_scale > 1`) applies the shared rolloff curve (ADR-009) with
  ceiling 1.0 *inside the decode shader*, so the attachment never clamps.
  SDR-domain draws skip the curve entirely (compile-time pipeline variant).
- **10-bit SDR output (`bitdepth:10`, Path B)**: composite into the working
  intermediate; the encode pass applies the same piecewise sRGB inverse
  EOTF in shader at 10-bit precision, plus output dither (ADR-011).
  Values > 1 are tone-mapped in the encode pass (ceiling 1.0), not at
  decode — Path B always tone-maps at output (ADR-009).
- Gamut: scene is BT.709 (ADR-002); SDR output matrix is identity; negative
  components clamp at encode (ADR-010).

**OPTIONS REJECTED.** Tone-mapping at output on Path A — there is no output
pass on Path A; adding one erases Path A's reason to exist. Hard-clipping
HDR sources on SDR outputs — walks bright saturated colour to white per
channel; the shared hue-preserving curve costs a handful of ALU in exactly
the shaders that already pay a PQ decode.

**CORRECTNESS CONSEQUENCES.** The one deliberate asymmetry in M5: on Path A,
tone mapping is per-source (composed sums can still exceed 1 and clamp —
accepted: source-over of a tone-mapped-to-1 source over any backdrop stays
≤ 1 wherever α = 1, and AA-edge overshoot clamps by ≤ one blend's worth);
on Path B it is post-composite. A window straddling an 8-bit SDR and an HDR
output therefore gets per-source mapping on one and output mapping on the
other — both from the same curve family with the same ceiling semantics,
and the C4 oracle bounds their divergence (test: divergence < 2 8-bit codes
for α=1 content; documented, not hidden).

**PERFORMANCE CONSEQUENCES.** Path A: unchanged pass/byte structure vs
today. Path B SDR-10-bit: the ADR-001 Path-B costs.

**FUTURE COMPATIBILITY.** An SDR output with an ICC profile keeps falling
back to fx_vk in M5 (ADR-000); in M6 it becomes Path B + LUT stage.

**FALSIFIER.** Regression floor: the render-matrix harness's SDR scenes must
be pixel-identical (≤1 code, dither-off) between pre-M5 and Path A for
opaque SDR content. Any systematic shift means decode/encode curves are not
inverses — the Path-A premise fails and the flip in ADR-004's falsifier is
triggered.

---

## ADR-008 — HDR10/PQ output mapping

**CONTEXT.** Invariant 1: PQ is an output encoding only. Today's connector
policy (BT.2020 + PQ image description, metadata selection, 10-bit format,
modeset fold-in) already exists and works (audit §3.2–3.3); what is missing
is AVK producing the pixels.

**DECISION.** An HDR output is always Path B. The output-encode pass (C6)
computes, per pixel, in this exact order:

1. sample scene value v (premultiplied linear BT.709-relative; α is dead at
   this point — the intermediate is fully composited, treat texel as opaque);
2. tone map: v ← R(v; peak_out) (ADR-009), peak_out from
   `m->hdr_max_luminance / scene_reference_luminance`;
3. gamut matrix: v ← M₇₀₉→₂₀₂₀ · v, then clamp negatives (ADR-010);
4. luminance anchor: v ← v × (scene_reference_luminance / 10000);
5. encode: e = PQ⁻¹(v) (ST 2084 inverse EOTF, exact constants from C1);
6. dither: e ← e + IGN·(1/1023) (ADR-011);
7. write to the `A2R10G10B10_UNORM` scanout view.

Connector-side metadata, format policy, `hdr_resolve`,
`mon_state_apply_color` and the fold-in commit are **unchanged** — M5 only
removes the `image_description != NULL` refusal in
`az_avk_output_supported` once C6 is integrated, making AVK stop falling
back. The `sdr_saturation` matrix (audit §3.6) folds into step 3's matrix
exactly as scenefx composes it — the feature is kept, computed CPU-side
into the pass's push constants.

**PQ-never-internal, enforced**: the only PQ *encode* in the tree is C6's
shader; C1's CPU encode exists for tests and the screenshot path. A grep
gate in the test suite (C4) fails if `pq` encode symbols appear in any
shader other than the output pass — cheap, blunt, effective.

**OPTIONS REJECTED.** Per-source PQ encode with encoded blending (what a
naive port of today's AVK model to HDR would be): violates invariant 1,
blending PQ values is colorimetric nonsense. Per-source tone mapping on
Path B (fx_vk's current shape): the composed value is the thing the curve
must see; fx_vk's shape makes two overlapping HDR windows tone-map
independently then *sum*, overshooting the panel.

**CORRECTNESS CONSEQUENCES.** SDR content on the HDR output renders at
`scene_reference_luminance` nits with correct primaries; HDR content maps
with hue preserved up to the panel ceiling; overlap/blur of HDR content is
physically composited before any curve touches it (fixes the fallback's
independent-rolloff error). Screenshot tonemap (audit §4) should gain the
missing 2020→709 matrix from C1 while it is being touched — small,
non-blocking.

**PERFORMANCE CONSEQUENCES.** The pass is damage-scissored; full-4K worst
case ≈ 100 MB/frame traffic (ADR-001). Push constants only; zero
allocations; one pipeline per (intermediate-format, target-format) pair
cached in the existing per-format renderer slots (`az_avk.h:1076-1099`).

**FUTURE COMPATIBILITY.** HDR10+ dynamic metadata would change connector
metadata only, not this pass. ICC-on-HDR (M6) inserts a LUT between 3 and 4.

**FALSIFIER.** rawhdr capture of a C4 test card through the live path,
decoded on the CPU: PQ signal for the 203-nit patch must equal
PQ⁻¹(203/10000) ± 1/1023. A miss means a luminance constant is on the wrong
side of the matrix — the ordering above is falsifiable at each step because
C4 computes every intermediate.

---

## ADR-009 — Tone mapping

**CONTEXT.** Scene values exceed an output's ceiling (HDR media on any
output; `sdr_white_scale > 1` UI). Something must compress. fx_vk uses
hue-preserving extended Reinhard driven by the max channel, per source
(`scenefx texture.frag:72-96`).

**DECISION.** One curve family for the whole project, **static,
hue-preserving, max-channel-driven extended Reinhard with an identity
segment**:

```
m       = max(r, g, b)
if m <= knee or peak <= 1:  identity
else:   s = f(m) / m,  v *= s
where f maps [knee, ∞) → [knee, peak] as
f(m) = knee + (m - knee) * (1 + (m - knee)/(peak - knee)²·…)   — the
       extended-Reinhard tail  f(m) = m·(1 + m/peak²)/(1 + m)  rescaled to
       be C¹ at the knee; exact algebra lives in C1, one implementation.
knee    = 1.0  (scene SDR white)  — everything at or below SDR white is
          untouched, by construction, on every output.
peak    = output peak in scene units (ADR-003); 1.0 on SDR outputs.
```

Applied: Path B → in the output-encode pass on the composited value
(ADR-008 step 2); Path A → at source decode for >1-capable domains only
(ADR-007). A single common scale on all three channels preserves the
channel ratios exactly — hue and saturation do not walk toward white.

**OPTIONS REJECTED.** BT.2390 EETF (splines in PQ space): operates in the
encoded domain, pulling PQ into the middle of the pipeline — invariant 1
says no; its visual advantage over Reinhard at these peak ratios
(1000-nit panel, 10000-nit signal ceiling already bounded by content ~1000)
is marginal. Per-channel curves: hue shifts, already rejected by fx_vk's
own comment. Adaptive curves: out of scope (ADR-000), and frame-dependent
tone mapping makes the C4 oracle non-deterministic.

**CORRECTNESS CONSEQUENCES.** SDR appearance is bit-unaffected on every
output (identity below knee). The knee at exactly 1.0 means extended-SDR
(`sdr_white_scale 1.5`) content *does* enter the curve on outputs whose
peak it approaches — correct: that is what "the panel cannot show 300 nits"
means.

**PERFORMANCE CONSEQUENCES.** ~8 ALU per pixel in the encode pass; free
(branch taken uniformly) for SDR-only frames.

**FUTURE COMPATIBILITY.** The curve is a C1 function with a registered
name; adding BT.2390 later is a second registered curve selected per
output — the pass interface (C6) takes curve parameters, not a hard-coded
formula.

**FALSIFIER.** (i) Mid-grey invariance: scene 0.18 must encode to exactly
PQ⁻¹(0.18·ref/10000) with tone mapping enabled — any drift means the knee
is below 1. (ii) Hue: a scene (4.0, 0.4, 0.4) red mapped to a 1000-nit peak
must keep r:g:b ratios within FP16 rounding; per-channel clipping would
send it toward (1, 0.4, 0.4)-normalised orange-white.

---

## ADR-010 — Gamut mapping

**CONTEXT.** Colours can leave the destination gamut two ways: wide-gamut
sources (negative scene components) shown on BT.709 outputs, and
out-of-range results after tone mapping.

**DECISION.** M5 ships **relative-colorimetric matrix conversion + post-
tone-map per-channel clamp**: scene (BT.709-relative, possibly negative) →
destination primaries via 3×3 → clamp each channel to [0, 1]-of-range at
encode. No perceptual gamut compression in M5. On BT.2020 outputs the
709→2020 matrix makes all BT.709-and-P3 content non-negative, so clamping
touches only genuinely out-of-container values; on BT.709 outputs
wide-gamut content clips saturation (a P3 red renders as the nearest
709 red at the same channel maxima).

**OPTIONS REJECTED (for M5).** Chroma-compression toward the neutral axis
(desaturate-to-fit): visibly better on laser-primary test content, but it
needs a lightness-preserving space (Oklab/ICtCp) per pixel in the encode
pass and careful tuning; it is additive later (a second registered mapping
in C6) and its absence never *corrupts* — it only clips. Doing it per
source: same objection as per-source tone mapping.

**CORRECTNESS CONSEQUENCES.** The clamp is the last resort, after the tone
map has already bounded the max channel; the residual clip affects only
out-of-gamut chroma, not luminance structure. Documented visible artefact:
BT.2020 saturated primaries on the SDR output flatten (they always have —
today's fallback does the same clip).

**PERFORMANCE CONSEQUENCES.** The matrix is already required (ADR-008); the
clamp is free (output format saturates).

**FUTURE COMPATIBILITY.** The C6 pass declares `gamut_map` as a named stage
so M6 can add compression without re-architecting.

**FALSIFIER.** C4 renders a 709-boundary colour and a 2020-only colour to
both output classes: the 709 colour must survive both bit-exact (pre-dither)
— if it shifts, the matrix is being applied to in-gamut content twice or
with wrong constants.

---

## ADR-011 — Final dither

**CONTEXT.** Quantisation banding. Today: shadow-local alpha-domain IGN
dither in AVK (calibrated against an encoded reference backdrop — a hack
the author documented as such, `avk_render.h:47-60`), output-quantum IGN in
fx_vk. `docs/avk-effects.md:1709-1730` already prescribes the M5 shape.

**DECISION.** Dither lives **wherever quantisation to a limited-depth
target happens**, at that target's quantum, on electrical values, RGB only:

- Path B encode pass: IGN, peak-to-peak one target code (1/1023 or 1/255),
  added after the inverse EOTF, before write (ADR-008 step 6). The composed
  electrical value is known here, so no reference-backdrop constant is
  needed: `AVK_DITHER_REF_BACKDROP` is deleted on this path.
- Path A: there is no encode pass; composition writes 8-bit directly, and
  each write quantises. The M4D shadow-local dither **stays** on Path A,
  unchanged, including its documented limitation. (Its amplitude function
  already returns the right value per format, `avk_render.h:71-100`.)
- Working intermediates (FP16) and blur transients: never dithered
  (`avk_dither_amplitude` already returns 0 for FP16).
- Alpha is never dithered.

IGN anchored to the output raster (existing `az_frag_global()` discipline,
`push.glsl:83-107`) so regional targets don't phase-shift the pattern.

**OPTIONS REJECTED.** Dithering scene values (pre-encode): the quantum is
not uniform in scene units under a non-linear encode — amplitude would be
wrong everywhere except one grey. Blue-noise texture: an extra sampled
descriptor in the hottest pass for marginal quality over IGN at 1-code
amplitude; the M4D measurements already validated IGN.

**CORRECTNESS CONSEQUENCES.** PQ 10-bit banding (the reason `bitdepth:10`
is implied by HDR) is decorrelated at exactly one code. Path A keeps
today's shadow behaviour bit-for-bit.

**PERFORMANCE CONSEQUENCES.** ~4 ALU in the encode pass. Zero new state.

**FUTURE COMPATIBILITY.** If Path A is ever retired (ADR-004 falsifier
firing), the shadow-local dither and its reference constant die with it —
the encode pass covers everything.

**FALSIFIER.** The M4D methodology re-run against the encode pass: a dark
PQ gradient with dither must show max constant-run < 8 px where undithered
shows > 16; and a high-passed patch's mean must be unchanged within 0.1
code (zero-mean check) — the existing `test_dither_breaks_banding` pattern,
retargeted.

---

## ADR-012 — Mixed SDR and HDR outputs, simultaneously

**CONTEXT.** The live desktop is exactly this: DP-1 may be HDR while
HDMI-A-1 stays SDR, and scroller-overflow geometry means one window's
scene nodes genuinely straddle outputs (the blur-bleed memory). Invariant
2 demands one scene intent, per-output transforms.

**DECISION.** The scene (command stream) is output-agnostic: scene values
and luminance domains never mention an output. All per-output state lives
in **`struct az_output_color_state`** (contract C3), resolved once per
output-state change (not per frame):

```
path        : A (direct-sRGB) | B (intermediate+encode)
encode_tf   : AZ_TF_SRGB | AZ_TF_PQ            (M5 set)
matrix      : 3×3 scene-primaries → output primaries (× sdr_saturation)
ref_nits    : scene_reference_luminance (scene-global, cached here)
peak_scene  : output peak in scene units (tone-map ceiling)
dither_q    : 0 | 1/255 | 1/1023
intermediate: working image handle (Path B), persistent per output
```

Frame build for output O renders the same command list with O's state; a
straddling window is drawn twice (once per output's frame, as today) with
identical scene inputs and different final transforms. Damage,
prefix-replay blur and all M4 machinery are per-output already and are not
restructured — blur transients simply take the output's working format:
Path A → the scanout's `_SRGB` view format (linear math, encoded 8-bit
storage, today's bandwidth); Path B → the working format (2× bandwidth,
HDR outputs only).

**Consequence spelled out**: the same blur, on the same window, at the same
moment, is stored 8-bit on the SDR output and FP16 on the HDR output. They
are the same *light* to within 8-bit quantisation; the C4 oracle bounds the
divergence.

**OPTIONS REJECTED.** Rendering the scene once into a shared linear master
and resampling per output: outputs differ in scale/geometry (1.5 vs 1.0
here), so a master costs a resample pass per output *plus* the memory, and
damage tracking across scales is exactly the cross-output complexity the
per-output model avoids. Per-scene (global) colour state: cannot represent
this machine's daily configuration.

**CORRECTNESS CONSEQUENCES.** A window straddling DP-1(HDR)/HDMI-A-1(SDR)
shows one intent, two renderings: SDR side tone-maps at 1.0 ceiling, HDR
side at panel ceiling — by design identical below scene 1.0.

**PERFORMANCE CONSEQUENCES.** Memory: one 66.4 MB intermediate per **HDR**
output only (persistent, like the blur buffers — idle reclaim was already
ruled out for those); 1080p SDR output adds zero. Descriptor churn: none
(cached per output). The per-output path split adds one branch at frame
build, not per draw — pipelines are selected per output frame.

**FUTURE COMPATIBILITY.** Per-output ICC (M6) extends the state struct
(LUT handle); nothing else moves.

**FALSIFIER.** The harness's live multi-output geometry trap
(monitor-relative asserts, HL_OUTPUTS=2): drag a test window across the
seam with HDR forced on one output; capture both. The overlapping strip
decoded to linear must match between captures within tone-map-predicted
bounds computed by C4 — a mismatch beyond bounds means some per-output
state leaked into the scene.

---

## ADR-013 — Performance integration boundary

**CONTEXT.** The performance agent actively owns `avk_blur.c`,
`avk_render.c`, `command/*`, `az_avk.h`, `az_output.h`, `animation/*`,
scenefx `wlr_scene.c`. M5's end state touches several of these; M5's
*Phase 2 implementation* must not.

**DECISION.** M5 implementation is split into **ISOLATED** work (new files,
new tests, additive struct/probe changes in unowned files) and **DEFERRED**
integration (every edit inside perf-owned files), with contracts written so
the deferred edits are mechanical:

ISOLATED now (contracts C1–C6): colour-math library + GLSL twin; luminance
-domain model + resolver; output-colour-state model; CPU reference
implementation and oracle tests; capability probes (`device/`, `dmabuf/`
are not perf-owned); the encode-pass shader and pipeline object
(`pipeline/`, `shader/` are not perf-owned — the pass exists, compiled and
unit-tested against fixtures, but nothing calls it).

DEFERRED (named in each contract's CONFLICT STATUS and in the manifest):
the frame-build wiring in `avk_render.c` (bind intermediate, run encode
pass), the scene-walk wiring in `az_avk.h` (attach `az_lum_domain` to
texture commands, un-refuse HDR outputs), blur-transient format selection
in `avk_blur.c`/`avk_render.c`, `avk_scene.h` command-struct field addition
(technically unowned, but it forces recompiles of owned files —
coordinated timing, treated as deferred), oracle extension in
`avk_oracle.c`, and the `wlr_scene.c` surface-description plumbing if any
additional field is needed (scenefx already exposes what C2 requires).

**Gate for the deferred merge**: perf agent's milestone freeze, then the
integration lands as a short series where each commit is "call a function
that already exists and is already tested".

**CORRECTNESS CONSEQUENCES.** Nothing user-visible changes until the
deferred series lands; the SDR-proof gate (C4) is satisfiable entirely in
the isolated phase (CPU oracle + headless fixture rendering through the
compiled-but-unwired pass).

**PERFORMANCE CONSEQUENCES.** Zero on the live frame path during Phase 2,
by construction.

**FALSIFIER.** `git diff --stat` of the Phase-2 implementation against the
owned-path list must be empty; the manifest is the checklist.
