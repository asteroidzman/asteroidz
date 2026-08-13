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

## F5 — ADR-007 and ADR-009 cannot both hold on Path A (C4) **needs Fable**

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

---

## F6 — ADR-003's falsifier is not satisfiable together with ADR-009 (C4) **needs Fable**

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
