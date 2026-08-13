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
