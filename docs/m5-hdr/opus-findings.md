# M5 implementation findings (Opus → Fable)

Deviations from the ADRs/contracts, evidence for each, and the things that
could not be settled without a GPU or without a performance-owned file.
Written as they were found, newest last. Nothing here is a silent change: if a
contract said one thing and the code does another, it is in this file with the
reason.

---

## F1 — ADR-009's sketched tone curve is not the curve the ADR needs (C1)

**What the ADR sketches.** ADR-009 gives

```
f(m) = m·(1 + m/peak²)/(1 + m)   — the extended-Reinhard tail
```

"rescaled to be C¹ at the knee; exact algebra lives in C1".

**Why it cannot be taken literally.** That is the *extended Reinhard* form, in
which `peak` is the **content white point that is to be mapped onto output
1.0** — f(peak) = 1 by construction. ADR-003 defines `peak` as something else
entirely: the **output ceiling expressed in scene units**,
`hdr_max_luminance / scene_reference_luminance` (≈ 4.926 for a 1000-nit panel
at the 203-nit default). Substituting one for the other maps the panel's own
ceiling to SDR white — a 1000-nit panel would render its brightest
representable value at 203 cd/m², i.e. the tone map would darken HDR content by
a factor of five. It also fails ADR-009's own stated properties: the ADR asserts
`f(peak-ε) < peak`, which the literal form does not satisfy for peak > 1.

**What was implemented instead.**

```
x = m − knee,  P = peak − knee
f(m) = knee + P·x/(P + x)
```

with the identity segment for m ≤ knee or peak ≤ 1, and one common scale
`f(m)/m` applied to all three channels.

**Why this one.** It satisfies every property the ADR and contract C1 actually
assert, and the test file asserts each of them separately:

| property | ADR-009 / C1 says | this curve |
|---|---|---|
| identity below the knee | required | exact, bit-for-bit (test: 1001 samples, 0 moved) |
| identity for peak ≤ 1 | required | exact (test: 101 samples, 0 moved) |
| f(knee) = knee | required | exact |
| C¹ at the knee | required | f′(knee) = P²/(P+x)²∣ₓ₌₀ = 1; measured one-sided slopes 1 ± 2e-4 |
| f < peak for all finite m | required (`f(peak−ε) < peak`) | asymptotic; verified at peak, 100·peak and 1e6 |
| hue preserved | required | one common scale; R:G ratio held to 6.6e-7 relative |
| monotonic | implied | 0 inversions over 20000 samples |

**Consequence for the rest of M5.** `peak` keeps ADR-003's meaning everywhere —
it is an output ceiling, never a content white point — so no other contract
moves. If Fable wants the literal extended-Reinhard shape it needs a *second*
parameter (content white) and ADR-003's `peak_scene` is not it.

**Falsifier if this is wrong.** Feed the curve `peak = 1000/203` and a scene
value equal to `peak`. This implementation returns 0.75·peak-ish, still below
the panel ceiling; the literal form would return 1.0, i.e. SDR white. Look at a
1000-nit highlight on the panel: if it renders at paper-white brightness, the
literal form is in use and is wrong.

---

## F2 — Two encode endpoints are not algebraically zero (C1)

Recorded because both look like bugs and neither is.

- **PQ.** `az_pq_ieotf(0) = c1^m2 = 0.8359375^78.84375 = 7.3096e-7`, exactly what
  ST 2084 says. That is 7.5e-4 of a 10-bit code, so it quantises to code 0, but
  it is not zero and a test asserting zero would be asserting against the
  standard. The decode direction *is* exactly zero (the `max(num, 0)` clamp
  makes it so), so the pair is not mutually inverse at the origin — by
  construction.
- **BT.1886.** `az_bt1886_ieotf(0) = −1.86e-9`. `pow(Lmin/a, 1/2.4)` and `b` agree
  to seven significant digits and the subtraction cancels them; the sign of the
  residue is float rounding. A UNORM attachment saturates it before anything
  can observe it.

Both are asserted at a bound below a thousandth of a 10-bit code rather than at
zero, and the reason is written at the assertion.

---

## F3 — PQ round-trip in 32-bit float does not reach 1e-6 (C1)

C1 asks for `|x − ieotf(eotf(x))| < 1e-6` over 4096 samples per transfer
function. Measured worst cases:

| tf | worst \|d\| | at x |
|---|---|---|
| sRGB | 8.9e-8 | 0.486 |
| gamma 2.2 | 3.0e-8 | 0.397 |
| BT.1886 | 6.0e-8 | 0.552 |
| **PQ** | **1.4e-5** | 0.889 |

PQ decode raises a ratio to the power 1/m1 = 6.277, so a 1-ulp error in that
ratio leaves the decode multiplied by 6.277, and the encode does not undo it in
32-bit float. 1.4e-5 is 0.014 of a 10-bit code — far below anything
observable — but it is 14× the contract's bound, so the test asserts **3e-5 for
PQ** and 1e-6 for the rest, with the reason at the assertion rather than a
widened shared tolerance. Computing the library in double would meet 1e-6 and
was rejected: the CPU library is the reference *for a 32-bit shader*, and a
reference more precise than the thing it certifies moves the error into a
parity bound nobody can interpret.

---

## F4 — The C1 GPU parity fixture is DEFERRED (needs a device)

C1 specifies a compute-shader fixture evaluating the GLSL twin over the same
sample grid, compared at ≤ 1 ULP-of-FP16. That needs a Vulkan device, and this
phase was instructed not to run GPU work. What ships instead:

- **source-text parity** on every shared constant (11 `#define`s compared across
  `az_color.h` and `color.glsl` *and* against the published value — so the two
  files agreeing on a wrong number still fails);
- a presence check for all 11 twin functions;
- an **alpha-unrepresentability** check: after stripping comments, `color.glsl`
  contains no 4-component type at all, so ADR-005's "no transfer function ever
  touches alpha" is enforced by the type system rather than by review.

The numerical fixture remains the right test and should be written when GPU
runs are allowed again. It is the only C1 item outstanding.

---

## F5 — ADR-007 and ADR-009 cannot both hold on Path A (C4) — **RESOLVED 2026-08-14**

**The two statements.**

- ADR-007: on an 8-bit SDR output (Path A), a draw whose domain can exceed 1.0
  "applies the shared rolloff curve (ADR-009) with ceiling 1.0 *inside the
  decode shader*, **so the attachment never clamps**."
- ADR-009: the curve's knee is 1.0, and it is "the identity for … peak ≤ 1".
  C1 restates this as an invariant and the unit test asserts it over 101
  samples.

**Why they conflict.** On an SDR output `peak_scene` is exactly 1.0 (ADR-003,
and C3 asserts it bit-exactly). With knee = 1.0 and peak = 1.0 the curve has no
interval to work in: `P = peak − knee = 0`. It is the identity, every value
above 1.0 survives to the attachment unchanged, and the attachment clamps —
which is precisely what ADR-007 says must not happen.

There is no assignment of the two parameters that satisfies both statements: a
rolloff onto a ceiling of 1.0 requires knee < 1.0, and knee < 1.0 contradicts
ADR-009's "everything at or below SDR white is untouched".

**What the implementation does.** The CPU reference applies the curve exactly as
specified — `az_tonemap(rgb, knee, peak_scene)` at decode for >1-capable
domains on Path A — so the consequence is *measurable* rather than papered
over: with the ADR's knee of 1.0, HDR content on an 8-bit SDR output hard-clips
at SDR white. `struct az_ref_output` carries `knee` as a parameter (contract C6
puts it on the encode pass's push constants anyway, not on the per-output
state), so the alternative is one line to try.

**The decision Fable owns**, stated as the three options:

1. **Accept the clip on Path A.** HDR sources on an 8-bit SDR output clip per
   channel. Cheapest, and it is what happens today.
2. **A knee below 1.0 for the >1-capable pipeline variant only.** ADR-007
   already says SDR-domain draws skip the curve entirely as a compile-time
   variant, so ordinary SDR content is untouched and the regression floor holds
   — only a PQ/scRGB/`sdr-white-scale > 1` source sees the lower knee. This
   looks like the intended design and needs one number chosen.
3. **A different curve family for the ceiling-1.0 case**, e.g. a
   content-white-point extended Reinhard (which is what ADR-009's *sketch* was;
   see F1).

Option 2 is the smallest change that makes ADR-007's sentence true. Nothing in
M5 is blocked on it — it changes one argument.

**RESOLUTION (user, 2026-08-14): option 2, and ADR-007's sentence is struck.**

The instruction was to make the architectural distinction reflect reality: the
physical attachment clips to its extent, while logical out-of-bounds source
support is reconstructed from scene semantics — and not to preserve an ADR that
implementation evidence has falsified.

Applied here that reads: an 8-bit SDR attachment clamps to its representable
extent, full stop. That is a property of the attachment and no ADR sentence can
make it untrue, so **"so the attachment never clamps" comes out of ADR-007**.
Handling values above SDR white is a SCENE-SEMANTIC job done in the shader
before the attachment ever sees them — which is exactly option 2: a knee below
1.0, in the >1-capable pipeline variant only, so ordinary SDR content keeps
ADR-009's identity guarantee and the regression floor holds.

The knee VALUE is still unchosen and is one push-constant argument. Whoever
picks it owns a falsifier: with the >1 variant selected and the lower knee, a
PQ source on an 8-bit output must NOT hard-clip at SDR white, and an ordinary
SDR source must be bit-identical to a build with the curve compiled out.

Recorded by the performance agent at M4 handoff. This is a reading of a
one-sentence instruction about a conflict whose other half (F6) was not
addressed — if the intent was narrower, F5 is the item to re-open.

---

## F6 — ADR-003's falsifier is not satisfiable together with ADR-009 (C4) — **RESOLVED 2026-08-14**

**ADR-003's falsifier** says: sweep `set_sdr_luminance` 80 → 400 with a PQ
pattern on screen; "the 1000-nit patch must hold constant … the UI white must
track the sweep."

**Measured, end to end through the CPU reference** (HDR output, 1000-nit panel,
a 400 cd/m² PQ patch and SDR white, gate 6):

| scene reference | 50 cd/m² patch | SDR white | 400 cd/m² patch |
|---|---|---|---|
| 80 | 50.00 | 80.00 | **317.42** |
| 120 | 50.00 | 120.00 | **332.41** |
| 203 | 50.00 | 203.00 | **360.96** |
| 300 | 50.00 | 300.01 | **387.50** |
| 400 | 50.00 | 399.99 | **399.99** |

Below the knee the invariance is exact — the decode's 10000/ref and the
encode's ref/10000 cancel, and 50 cd/m² holds to four figures at every
reference. **Above the knee it cannot hold**, because ADR-009 anchors the
knee at scene 1.0, which *is* the reference: lowering the reference lowers the
knee in absolute terms, so the same absolute luminance is compressed harder.
At ref 80 a 400-nit patch is scene 5.0 against a ceiling of 12.5 and comes back
as 317 cd/m².

This is not a bug in the implementation — it is what the two ADRs jointly
specify. But the falsifier as written would fail on correct code, which makes
it a falsifier for the wrong thing.

**Suggested repair**, for Fable to accept or replace: state the falsifier as
"an HDR patch **below the reference** must hold its absolute luminance while
SDR white tracks the sweep; a patch above it must move monotonically with the
reference and never exceed its own mastered luminance." That is what gate 6
asserts now, and it still catches every break — including the two that
previously only gate 6 caught.

**RESOLUTION (Fable, 2026-08-14): the repair is accepted, with one
tightening.** "Below the reference" is ambiguous *during a sweep*: a 203-nit
patch is below the reference at ref 300 and above it at ref 80, so across
80→400 it holds in one half of the sweep and moves in the other — pick it as
the invariant patch and you have rebuilt a falsifier that fails on correct
code, which is the exact defect being repaired. The rewritten ADR-003
falsifier therefore pins the invariant patch **below the lowest reference in
the sweep** (50 cd/m² for 80→400, the same patch the table above uses), and
keeps a diagnosis for each direction of failure: an invariant patch that
moves means the anchor is applied on the wrong side of the split; an
above-reference patch that holds constant means tone mapping is not being
driven by the reference-relative scene value. The old 1000-nit form is
retained in the ADR as a parenthetical *anti*-example, so nobody
reintroduces it as an "obvious" strengthening.

The invariance is claimed at the reference boundary, not at "the knee"
in the abstract, and that is deliberate given F5: F5's sub-1.0 knee lives in
the Path-A >1-capable *decode* variant only, and F11 puts every HDR output on
Path B, where ADR-009's output-encode knee stays at scene 1.0 — so on the HDR
output this falsifier measures, knee and reference coincide. If the F5 knee
ever migrated to Path B, the invariance region would shrink to
knee × reference and the falsifier's patch bound must move with it; whoever
chooses the knee value owns that one-line consequence.

Gate 6 needs no change: it already asserts the repaired statement — 50 cd/m²
invariant within 1 cd/m² at every reference, SDR white tracking within
2 cd/m², the 400 cd/m² patch monotone non-decreasing and never above
400 — and its own header comment already states the finding.

---

## F7 — the breaks are a runtime parameter, not `#ifdef` fixtures (C4)

C4 asks for the broken pipeline variants to be kept as `#ifdef` fixtures. They
are a runtime `enum az_ref_break` instead, and every break runs in every run of
the ordinary test binary.

The reason is the rule the contract is enforcing. An `#ifdef` variant is
compiled only when someone remembers to configure a second build; a break that
is never built is exactly the break that silently stops breaking, which this
project has been bitten by twice. As a runtime parameter there is no second
build to forget.

It paid for itself immediately. With nine breaks wired into the per-break
tally, **gate 1 — the bit-exact SDR round trip, the headline M5 gate — caught
none of them.** An opaque single layer is insensitive to how blending,
premultiplication, alpha, tone mapping and dither are handled; the only thing
that can move it is a decode/encode pair that is not self-inverse. So a tenth
break was added (`sRGB decode, gamma-2.2 encode` — the most likely real bug on
this path, since the stack today genuinely mixes the two SDR curves) and gate 1
now fails on 645 of 768 channels against it. Without the break tally the gate
would have shipped green and untestable.

Two other breaks are caught by exactly one gate each and by nothing else:
`encoded-domain blur` only by gate 9, and `dither applied before the inverse
EOTF` only by gate 8 — and gate 8 only after its oracle was corrected. Its
first version compared the dithered mean against the *undithered quantised*
output, which is the true value rounded, up to half a code away by
construction; that scored correct dither as a 0.24-code error and the break as
0.03. Measured against the *unquantised* value, correct dither is 0.0003 codes
out. The break is caught by the second property instead — ADR-011 asks for
noise that is zero-mean *and* one code peak-to-peak, and dithering the scene
sprays three codes at PQ black while keeping the mean nearly right. A mean-only
oracle scores it as correct.

---

## F8 — gate 9's fixture must have contrast, and the direction of the error

The encoded-domain-blur break is measured on a one-pixel black/white
checkerboard, and the test asserts — as a check on itself — that the same two
blurs over a **flat** patch are bit-identical (|d| = 0). A flat-backdrop
fixture cannot detect the bug in either direction; that is the f8be42c lesson,
asserted rather than remembered.

Measured: linear blur mean 0.50000 (the true light mean of half white, half
black), encoded blur mean 0.21416 — 43% of the light. Note the direction:
averaging *codes* and reading the average as light comes out **darker** here,
because the sRGB EOTF is convex. The f8be42c episode was the mirror image of
the same identity (values already encoded, blurred as though they were light,
so the result came out *lighter*). Which way the error runs depends on which
side of the curve the blur is standing; that it is large depends only on the
content having contrast.

---

## F9 — C5's scanout answer is DEFERRED; the predicate is not

C5 says the scanout `_SRGB`-view answer on the live GPU is the audit's biggest
UNKNOWN and asks for it to be recorded when first run. **It has not been run.**
This phase was instructed not to start a compositor or a headless device
fixture, and the answer is a property of the modifier KMS actually selects for
a scanout buffer — which only the compositor knows.

What shipped instead, so that the answer is one log line away:

- **`avk_color_caps.h`** — the three questions as pure predicates over
  `VkFormatFeatureFlags`, with the bit sets and the reason for each bit written
  at the definition.
- **The live probes** — `query_color_formats()` in `avk_phys.c` for
  `fp16_attach_blend_sample` and `rgb10a2_attach`, and a second
  `VkFormatProperties2` query in `avk_format_table.c` for the `_SRGB` twin's
  **own** per-modifier features, ANDed with the existing mutable probe.
- **Two log lines**, emitted at startup with no HDR work enabled:

```
avk caps: color fp16=1 (features 0x…) rgb10a2=1 (features 0x…)
avk formats: color scanout_srgb_attachment=N of M render pairs
```

  and, at `AVK_DEBUG`, a per-render-modifier line carrying `srgb_attach=`.
  A zero on the second line means **Path A does not exist on this device** and
  ADR-001's falsifier (i) has fired.
- **`avk_format_table_scanout_srgb_ok(table, fourcc, modifier)`** — the
  per-output form, which is exactly C3's `scanout_srgb_view_ok` input.

**The distinction the probe exists to make**, because it is easy to miss: the
table already recorded `srgb_mutable`, which says the mutable image can be
*created* with an `_SRGB` view in its format list. It says nothing about what
that view may be *used for*. Format features are per (format, modifier) and the
`_SRGB` format has its own set — a driver may allow the mutable image and allow
sampling the `_SRGB` view while refusing to attach it. Asking only the first
question produces a Path A that passes every startup probe and fails at the
first frame on the live desktop. `avk_scanout_srgb_attach_ok()` requires both,
and the test asserts that exactly one of the eight
(mutable × attach × blend) combinations is a yes.

**What is untested until a device is allowed**: that the live probe passes the
*right* flags to the predicate — specifically that it queries the `_SRGB`
twin's modifier list and not the UNORM one. The predicates are covered by
synthetic feature sets (19 checks, 6 breaks, all failing as required); the
wiring is not.

**To get the answer**, with no HDR behaviour enabled and nothing consuming the
result: the two `AVK_INFO` lines appear at the default log level, so a single
ordinary start prints them; the per-modifier `srgb_attach=` breakdown needs
`ASTEROIDZ_VK_DEBUG=1` (already set in the live GDM line). C5 changes no
behaviour by construction.

---

## F10 — C6: the shader landed, the pipeline object did not

`output_encode.frag` and `output_encode.vert` exist, compile to SPIR-V in the
ordinary build, and pass `spirv-val`. The **pipeline object**
(`avk_output_encode.{c,h}`) is DEFERRED.

**Why the split.** The shader is the part with the decisions in it — ADR-008's
step order, where the luminance anchor sits relative to the matrix, which
transfer function is a specialisation constant rather than a branch — and the
build type-checks it. The pipeline object is descriptor layouts and
`VkGraphicsPipelineCreateInfo` filling: mechanical, untestable without a device
this phase was told not to create, and its only caller (`avk_render.c`) is
performance-owned and deferred regardless. Writing several hundred lines of
pipeline construction that nobody can execute, against a call site that does
not exist yet, is how an integration lands with three bugs in it at once.

**What the shader commits to**, so the pipeline object has no decisions left:

- **Its own vertex stage and its own push-constant block.** `push.glsl` is
  exactly 128 bytes — the guaranteed minimum `maxPushConstantsSize` — and is
  already dual-purpose across four pipelines; a 3×3 matrix plus four scalars
  does not fit, and overlaying them onto fields that mean something else in a
  texture command is how an overlay table stops being maintainable. This pass
  shares nothing with the commands before it, so it takes a separate 64-byte
  block: three matrix rows with a scalar in each `w`, plus one `vec4` of dither
  quantum and target origin.
- **A fullscreen triangle**, not a quad: a quad's diagonal seam is rasterised
  by both primitives, which is a real cost on a pass covering every damaged
  pixel of a 4K output. No vertex buffers; `vkCmdDraw(cb, 3, 1, 0, 0)`.
- **Damage is a scissor, not geometry**, and the attachment is LOADed — exactly
  the existing blur-pass pattern.
- **`AZ_ENCODE_TF` is `layout(constant_id = 0)`**, so each output's pipeline has
  the other curve compiled out. "The PQ code is not present in an SDR
  pipeline" is a stronger statement than "the branch is not taken".
- Matrix rows are taken as three `vec3`s, never a `mat3`: GLSL's `mat3`
  constructor is column-major, and nine row-major floats read as a `mat3` are
  transposed — which still renders a plausible picture.

**Cost, measured without a GPU** (`spirv-dis` on the unoptimised module, both
encode branches present before specialisation):

| metric | value |
|---|---|
| arithmetic + ExtInst ops | 59 |
| `Pow` | 3 (2 for PQ, 1 for sRGB — one branch is folded out per pipeline) |
| `Fract` | 2 (the dither) |
| `FClamp` / `FMax` / `Step` / `FMix` | 4 / 6 / 2 / 1 |
| SPIR-V module | 6212 bytes frag, 1160 bytes vert |
| `spirv-val` | clean, both stages |

C6's budget is ≈40 ALU/px (matrix 9 MAD + tonemap 8 + PQ pow×2 + dither 4).
The 59 unoptimised ops cover both encode branches and every clamp glslang
emits without folding; the shape is right and the real number needs a driver.

**One thing the compile caught that review would not have**: `active` is a
reserved word in GLSL, and `az_tonemap`'s branchless selector was using it. The
CPU twin compiled fine. This is the argument for wiring a shader into the build
the day it is written rather than the day it is called.

**Also landed with C6**: gate 7 now **scans** the shader directory instead of
checking a hardcoded list, and asserts that exactly two files are exempt
(`output_encode.frag`, which calls the PQ encode, and `color.glsl`, which
defines it). A hardcoded list is a gate that stops covering the next shader
somebody adds — and adding a shader is exactly when a PQ encode appears
somewhere it should not. The gate also asserts that `output_encode.frag` *does*
call `az_pq_ieotf`, so it cannot pass by scanning nothing.

---

## C7 — not started

Not attempted this phase. It is the highest-conflict contract (MEDIUM in the
manifest: `avk_pipeline.{c,h}`, `avk_image.h`, `texture.frag`, all interlocking
with per-draw variant selection in the performance-owned `avk_render.c`), its
variants cannot be exercised without a device, and C1–C6's isolated work is
what the rest of M5 is built on. The decode ordering it must implement is
already pinned by the CPU reference and by gate 2, so when it is written there
is an oracle waiting for it.

---

## F11 — Path A is an 8-BIT path, and the audit's UNKNOWN is closed (C5)

The audit left one UNKNOWN: can the swapchain image be created MUTABLE with an
`_SRGB` view usable as a colour attachment, **for the modifiers KMS actually
selects**? C5's contract says to record the observed value from the live GPU
when first run, because the modifier is a per-output runtime choice.

**It is not a per-output question on this GPU, or on any device.** Run at
DEBUG, the probe reports `scanout_srgb_attachment=20 of 70 render pairs`, and
the 20 fall out along one axis only:

| format | Path-A-capable render modifiers |
|---|---|
| `AR24` `XR24` `AB24` `XB24` (8-bit) | **5 of 5** |
| `AR30` `XR30` `AB30` `XB30` (10-bit) | 0 of 5 |
| `AB48` `XB48` `AB4H` `XB4H` `RG16` (FP16) | 0 of 6 |

Every 8-bit format supports it on **every** modifier the device advertises for
render — LINEAR, `GFX9,64KB_S`, and the three `GFX10_RBPLUS` tilings including
the DCC pair. So whichever modifier KMS picks for an 8-bit scanout buffer, the
answer is yes; and for a 10-bit or FP16 buffer the answer is no regardless.

**The reason is in the Vulkan format enumeration, not in this driver.**
`avk_drm_format.c` maps `DRM_FORMAT_ARGB2101010` and the FP16 formats to
`VK_FORMAT_UNDEFINED` for `vk_srgb` because **there is no sRGB variant of a
10-bit or half-float Vulkan format** — sRGB variants exist only at 8 bits per
channel. `avk_scanout_srgb_attach_ok()` is therefore false by construction
there, and would be on any conformant implementation.

### What this means for the ADRs

**Path A and Path B are not a device-capability split. They are an 8-bit /
deep-colour split, and it is decided by the output's bit depth.**

- An 8-bit SDR output: Path A is available, unconditionally.
- A 10-bit output: Path B, always. **Including a 10-bit SDR output**, which is
  a configuration a user can select without asking for HDR at all.
- An HDR output: 10-bit or FP16 by definition, therefore Path B, always.

The last two are worth stating plainly because the ADRs discuss Path A/B as
though the capability probe might go either way per machine. It cannot. The
probe stays — a device that advertises no 8-bit render modifier at all would
still be caught, and answering false costs Path A and nothing else — but the
per-output `scanout_srgb_view_ok` call now has a predictable answer and should
not be described as an unknown.

**Consequence for C6/ADR-012 sizing.** If a 10-bit SDR output is on Path B,
then the intermediate target and the FP16 blur transients are not an
HDR-only cost — they are the cost of any deep-colour output. The M4I background
blur cache (C8) sits behind that decision too: two output-resolution images per
output, 66.4 MB at 4 bytes on DP-1 and 132.7 MB at 8. Whoever sizes Path B
should know that turning on 10-bit SDR doubles it.

Measured headlessly on the same physical GPU the session uses; no live install
required, which is why this closed without one.

---

## F12 — untagged arrives as GAMMA22, and that alone made Path A wrong (C2/C7)

Path A on a real output was **one code high on every pixel of the display**: a
flat grey wallpaper at 128 came back as 129, while the GPU fixture round-tripped
all 256 codes at zero. Same GPU, same shaders, same formats.

**Ruled out by experiment, in this order:** blending (still 1 with blur, shadows
and layer shadows disabled), output scale (still 1 at scale 1.0, so not linear
filtering of a scaled layer), dither (still 1 at `AZ_SHADOW_DITHER_AMP=0`), and
a full-screen background rect under the wallpaper (added to the fixture from an
`AZ_AVK_CMD_DUMP` reading of a live frame — still 0 there).

Two of my hypotheses died on the way: that the shader's `pow()` EOTF and the
hardware's sRGB table are not exact inverses, and that linear filtering was
responsible. Both were tested rather than argued, and both were wrong.

**A counter found it in one reading.** Splitting decode draws by variant:

    m5_decode_srgb=0  m5_decode_gamma22=2  m5_decode_bt1886=0

Every source on the desktop was being decoded with the 2.2 power curve.

**Why.** scenefx's surface adapter — an otherwise exact copy of wlroots 0.20.2's
`surface_reconfigure` — initialises the transfer function to
`WLR_COLOR_TRANSFER_FUNCTION_GAMMA22` and overwrites it only when the surface
carries an image description (`types/scene/surface.c:304`). So a surface that
has said NOTHING about its colour is indistinguishable, downstream, from one
that explicitly declared 2.2 — and on a real desktop essentially every surface
is the former.

The arithmetic is exact:

    srgb_ieotf(gamma22_eotf(128/255)) * 255 = 128.95  ->  129
    srgb_ieotf(srgb_eotf   (128/255)) * 255 = 128.00  ->  128

**Why it matters more on Path A than anywhere else.** Path A's encode is the
hardware's `_SRGB` attachment conversion. It cannot be selected. So a source
decoded with 2.2 and encoded with sRGB has no way to round-trip through Path A
at all — the mismatch is structural, not a rounding artefact, and it applied to
the entire desktop.

**Resolution: the adapter treats GAMMA22 as untagged**, which is ADR-004's
answer applied to the only information available. Measured after: a wallpaper-
only frame is **0 differing pixels**, and a full desktop with blur, shadows and
a client window is **94.21% of pixels identical**, with the remainder confined
to blended regions at 5-12 codes — which is exactly what ADR-005 says moving
composition into linear light should change, and nothing else.

**What this costs, stated rather than hidden.** A client that genuinely means
2.2 and declared it through a protocol path this cannot distinguish will be
decoded as sRGB, an error bounded by the difference between the two curves --
about one code in the midtones. That is the trade ADR-004 already chose, and it
is strictly better than the alternative, which was being wrong for everything.

**The better fix belongs upstream and is not made here:** scenefx could carry a
distinct "untagged" value so the two cases stop being the same value. That is a
subproject change with an ABI surface, it is not needed for Path A to be
correct, and the decision is the ADR owner's. Until then this adapter is the one
place the ambiguity is resolved, and it says so.

---

## F13 — Path A is invalid usage, and it shipped a milestone that way (C7)

**The defect.** Path A attaches the target's `_SRGB` view to pipelines created
with the UNORM twin as their colour-attachment format. Dynamic rendering bakes
that format into the pipeline and the spec requires it to match the attached
view:

    VUID-vkCmdDraw-dynamicRenderingUnusedAttachments-08910: vkCmdDraw():
    VkRenderingInfo::pColorAttachments[0].imageView format
    (VK_FORMAT_B8G8R8A8_SRGB) must match the corresponding format in
    VkPipelineRenderingCreateInfo::pColorAttachmentFormats[0]
    (VK_FORMAT_B8G8R8A8_UNORM).

**Why it looked right.** RADV executes it correctly. Path A produced the right
picture, a clean A/B against the direct path, and a 0-code round trip on both
the unit fixture and a real output — while being undefined behaviour on every
draw of every frame. It fired twenty times in a single run of
`tests/test-avk-render.c` the first time that fixture was run under the
validation layer, which was during M5.5. It is present on the unmodified M5.4
tree; this is not a regression introduced by Path B.

**The fix.** A second `avk_pipelines` declaring the `_SRGB` format, built lazily
on the first frame that takes the fast path — so a desktop with no Path-A output
never pays for it, and the twin format is discovered from the target rather than
guessed at init. Only the pipeline objects differ: descriptor sets, samplers and
pools still come from the primary set through the same per-image cache, because
the two pipeline layouts are created from identically defined set layouts and
identical push-constant ranges, which is the spec's definition of layout
compatibility. Duplicating the descriptor cache to avoid using it would double
every surface's descriptors for nothing.

**After:** 0 VUIDs in the unit fixture, and Path A's compositor round trip is
still 0 differing pixels.

**The alternative that was not taken.** Path A could instead have selected the
renderer keyed on the `_SRGB` format outright, the way Path B selects the FP16
one. That is arguably cleaner, but it would also make every blur transient and
the whole M4I cache `_SRGB`-encoded storage — a real change to what a blur
averages, needing its own measurement. Correcting invalid usage should not
smuggle in a change to what the blur does.

---

## F14 — every `validation_errors == 0` assertion without the layer was empty

**The mechanism.** `validation_errors` increments in exactly one place: the
validation layer's message callback (`avk_log.c`). The layer is only loaded when
`ASTEROIDZ_VK_DEBUG` is set. So in every fixture that does not set it, the
counter reads 0 whatever the frame did.

`avk-m5-path-a-test.sh` asserted it that way for the whole of M5.4, over a Path
A that was emitting twenty VUIDs a run. It is not the only one: of the fixtures
under `contrib/` that assert `validation_errors`, most never set the variable.
The live session does have it set (it is in the GDM line), which is why the live
qualification's 0-VUID column was real and the headless ones were not.

**The fix is a premise, not a habit.** `avk-stats` now reports
**`validation_enabled`**, taken from `avk_instance.validation_enabled` — what
the loader actually gave us, not what the environment asked for. A fixture that
asserts the count asserts that first, and both M5 fixtures now run every arm
under the layer:

| fixture | before | after |
|---|---|---|
| `avk-m5-path-a-test.sh` | 10/10, layer off | **12/12, layer on** |
| `avk-m5-path-b-test.sh` | — | **13/13, layer on** |

**The general shape**, which this project has now hit four times (two dead
breaks in M4H, the rejected config in M4I, the vacuous multi-output assertions,
and this): an instrument that reports success by measuring nothing. The rule
that falls out is narrow enough to be worth writing down — *an assertion on a
counter must be preceded by an assertion that the counter can move.*

---

## F15 — C6 landed, and the falsifier was inert on grey

**Status.** C6 is complete: `avk_output_encode.{c,h}` (pipeline per (target
format, curve), fullscreen triangle, damage as scissors, LOAD attachment), the
per-output persistent FP16 intermediate, and the integration in
`avk_render_frame` / `az_avk.h`. F10's deferral is closed.

**ADR-012 needed no code.** The blur transients, the prefix captures and the
M4I cache are all allocated in `renderer->format`, and Path B selects the
renderer keyed on `R16G16B16A16_SFLOAT` — so "the blur transient format follows
the path" is a consequence of the existing per-format renderer selection rather
than a rule anything enforces. The cache's `format` field then invalidates it
across a path change without a line written for that either.

**The falsifier that measured nothing.** The Path-B SDR gate's break replaces
the identity matrix with BT.709→BT.2020, which C3 must never derive for an SDR
output. Run against a grey ramp it reported **0 codes of difference** — because
every row of that matrix sums to 1, so a neutral colour is invariant under it.
The assertion was true about grey and said nothing whatever about the matrix.
With the channels offset by a third of the range each, the same break moves the
worst channel to **61**.

This is the `coverage by coincidence` pattern again, and the tell was the same:
the right answer and the wrong answer agreed, so the test could not tell them
apart.

**What Path B measures.**

| claim | result |
|---|---|
| SDR round trip vs the pre-M5 8-bit picture | worst channel **0** |
| wrong gamut matrix (the break) | worst channel **61** |
| PQ vs `az_ref_encode_scene`, 10-bit | **0** codes at five of six values, 1 at the panel ceiling |
| compositor: wallpaper-only frame, real scan-out | **0** px differ |
| encode pipeline compiles, steady state | **1**, and it stops |
| validation errors, layer confirmed on | **0** |

The one code at scene 4.926 is the FP16 store rather than the encode's
arithmetic: half-float carries about 5e-4 of relative precision and PQ's slope
turns that into rather more than one 10-bit code near the ceiling. It is stated
here instead of being folded into a vaguer tolerance.

---

## F16 — a config colour is a source too, and nothing was decoding it

**The defect.** C7 gives every client BUFFER a luminance domain and decodes it.
A border's colour has no buffer: it is an sRGB hex triple out of a config file
that reaches the renderer as a command field. Nothing decoded it, because until
M5 composition happened in the same encoding the config was written in.

On a linear path that is wrong by a lot. An electrical 0.5 entered the blend as
though it were a scene value and the encode re-encoded it. Measured on the GPU
(`tests/test-avk-render.c`, before the fix):

| asked | direct | linear path | error |
|---|---|---|---|
| 64 | 64 | 137 | **+73** |
| 128 | 128 | 188 | **+60** |
| 192 | 192 | 225 | **+33** |

That is every border, every background rect and every shadow tint on the
desktop — on **both** Path A and Path B, so it was present and unnoticed
through M5.4's gate. The gate did not catch it because the gate draws a
TEXTURE, and so did both compositor fixtures: the harness wallpaper is a PNG,
which is a client buffer and takes C7's path. A frame made entirely of decoded
sources round-trips perfectly while every solid colour on top of it is 60 codes
out.

**The fix** is `az_avk_scene_rgb()` in `az_avk.h` — ADR-004's rule ("what is
untagged is piecewise-sRGB BT.709") applied to the other kind of source in the
scene. Applied at four sites: the rect colour (un-premultiplied first, because
a curve applied to colour-times-coverage is neither), the shadow colour (already
straight), the gradient stops (un-premultiply, decode, re-premultiply — a
gradient whose ends were decoded and whose stops were not is a border that
changes colour along its length), and the frame's clear.

It lives on the compositor side of the boundary, not in the renderer, for the
same reason `az_avk_lum_of()` does: a command still does not know which
attachment it lands in, and colour policy is not the renderer's.

**RGB only.** Alpha is coverage; it is linear by definition and has no
colorimetry (ADR-005). Running it through a transfer function is the mistake
that makes every translucent panel the wrong opacity.

**Asserted in two places, deliberately split.** The renderer's half — given a
SCENE value, the encode puts the right code on screen — is
`test_solid_colour_domain`, with a falsifier that feeds it the electrical value
and requires 188. The compositor's half — the walk supplies scene values — is
stage 2 of `contrib/avk-m5-path-b-test.sh`, which probes a pixel in the middle
of a 12px border ring rather than comparing whole frames, because a window's
antialiased corners and blended edge are *expected* to move on a linear path.
With one test spanning both halves, a fix in either would make it pass and
neither would be pinned.

Run against a build with the walk's decode removed, stage 2 reports the border
at **(228, 173, 106)** where the config asked for **(198, 107, 37)** — and
stage 1, the wallpaper, still passes at 0 px, which is what makes the two
stages genuinely independent probes rather than one claim written twice.
