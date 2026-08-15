# M6A — PRESENTATION OWNERSHIP — **CLOSED**

Closed by operator decision on the evidence below. Not to be reopened without a
regression or a new measured requirement.

**Decisions taken at closure:**

- **ADR-611** — per-output presentation evaluation ENFORCED. Cross-output
  temporal skew is legitimate by design; never restore a global animation tick
  to hide it.
- **ADR-612** — Model A retained. GPU evaluation of animation progress
  REJECTED ON EVIDENCE, not deferred as unfinished work. Reopen only if
  profiling shows CPU evaluation, state upload or geometry mutation becoming
  meaningful.
- **Velocity continuity** — enabled as the production default.
  `AZ_BREAK_ANIM_RETARGET_ZERO_VELOCITY` is a diagnostic falsifier, never the
  default.
- **`VK_EXT_calibrated_timestamps`** — optional observability enhancement, not
  a blocker. `UNKNOWN` remains the correct verdict without it.
- **Burst-from-idle (~2.7 ms on DP-1)** — observed, within the 6.944 ms budget,
  NO ACTION. Do not resurrect DPM prewarm, clock pinning, fake GPU activity or
  idle keepalive on account of it.
- **Spring tuning** — user validation pending. Do not adjust constants without
  live evidence.

## The standing rule this milestone earned

**A green oracle is not trustworthy until its falsifier has been observed red.**

Ten instruments in this milestone looked convincing and were wrong. What makes
the results below trustworthy is not that they went green — it is that premise
assertions, working falsifiers and independent derivations caught the broken
measurements. For an important new invariant: prove the fixture exercises the
mechanism, break the mechanism deliberately, observe the expected failure,
restore it, observe green, and where possible confirm with an independent
measurement.

This is not a mandate to run large suites constantly. It is a mandate to make
the tests that do run informative.

## What is built

Companion to `adr.md` (the design) and `audit.md` (the ground truth it was
designed against).

## Built and verified

| ADR | thing | evidence |
|---|---|---|
| 601–603 | presentation feedback in production | `amsg get presentation`, per output |
| 603 | the stamp's clock domain, **proven** | DP-1 −56.089 ms from monotonic vs −1.786 × 10¹⁵ ms from realtime; reported as `stamp_clock`, degrades to a coarse predictor if a backend ever says `neither` |
| 602/605 | `struct az_presenter`, two regimes | DP-1 `vrr` / 6944.492 µs, HDMI-A-1 `fixed` / 16666.666 µs |
| 605 | prediction, with its error measured | continuous **0 µs** (HDMI) and **11 µs** (DP-1) mean; idle 1823 / 6696 µs |
| 605 | `t_pipe` = 3000 µs on VRR, chosen by sweep | see the table in `az_presenter_impl.h` |
| 604 | epoch resets: creation, output-manager commit, request-state, DPMS both directions | `presenter.epoch`, `resets[]` |
| 606 | one sample instant per pass, threaded | I1 counter reads 0 violations across thousands of passes |
| 606 | **G2 closed** — animation samples predicted presentation time | `az_frame_sample_ns()` returns the armed target |
| 606 | **G3 closed** — no clock reads in animation sampling | `tests/check-animation-clocks.py` |
| 607 | completion is wall-clock, not frame-count | 708 ms @ 204 Hz of ticks vs 720 ms @ 120 Hz |
| 608 | retarget **position** continuity | 42 px largest step across the seam, against 1027 px broken |
| 610 | idle stays idle | 0 presents / 0 frames over 12 s with a window mapped |
| 611 | the semantic/presentation boundary, **observable** | `x` vs `anim_x` on the client IPC |
| 611 | the trajectory is a **pure function** | `anim_eval_at`; ADR-607 statement 1 holds by construction |
| 608 | retarget anchors at the last **sampled** instant | the defect G2 introduced, caught by the ADR before the code |
| 608 | retarget **velocity** continuity, spring-only | 60 px overshoot through the turn, against 0 px broken |
| 609 | misses that name their evidence | 46 misses, all UNKNOWN with commit margin: "compositor too slow" disproven |
| 609 | miss ≠ prediction spread | DP-1 38 phantom misses → 1; spread named `prediction_exceeded` |

## Falsifiers, each shown red and green

| # | switch | correct | broken |
|---|---|---|---|
| I1 | `AZ_BREAK_PRESENT_SAMPLE_NOW` | 0 of 367 | 369 of 369 |
| I4 | `AZ_BREAK_ANIM_FRAME_STEP` | 12 ms apart | 150 ms apart |
| I5 | `AZ_BREAK_PRESENT_IDLE_WAKE` | 0 presents | 12 presents |
| I10 | static, no switch | passes | red on a reintroduced `clock_gettime` |
| I12 | `AZ_BREAK_ANIM_RETARGET_POSITION_RESET` | 42 px | 1027 px |
| I6 | `AZ_BREAK_ANIM_RETARGET_ZERO_VELOCITY` | 60 px overshoot | 0 px |
| P1 | `AZ_BREAK_ANIM_SPRING_SCALAR_V0` | y enters at 0.089 of its peak | 0.777 |

P1's switch restores the projected-scalar retarget velocity that per-axis
seeding replaced. Its fixture is `contrib/anim-vector-continuity-test.sh`,
which runs both arms in one invocation and fails if the broken one comes out
green. `contrib/m6a-retarget-test.sh` (I6, I12) was re-run against the
per-axis implementation and still flips both ways: 7/7.

## Not built

- **ADR-611's refactor.** The boundary is observable but not enforced:
  `rendermon` still walks every client on every output's pass and mutates the
  one shared `c->animation.current`, so a window straddling two outputs is
  last-writer-wins. Not a regression — the two passes always ran at different
  instants — but it is what per-output sampling needs before it is more than
  per-output *targets*.
- **ADR-612's GPU half.** Model A was chosen (CPU evaluates, GPU applies) and
  the CPU half is what exists today; no presentation transform has moved into
  the vertex path, and no measurement yet says it should.
- **`VK_EXT_calibrated_timestamps`.** Without a real GPU completion instant,
  `GPU_LATE` and `PRESENTATION_SCHEDULING` are unreachable and every miss that
  committed with margin lands `UNKNOWN`. Wiring it is what would turn those
  into verdicts. `gpu_ts_available` reports the limitation rather than hiding
  it.
- **ADR-613/614's damage and coherence oracles.** Untested.
- **The VT-switch reset** (audit G6) — unwireable; there is no session object.

## Numbers worth keeping

Prediction error, DP-1, by regime — the reason both regimes survive:

```
              idle mean / abs      continuous mean / abs
  t_pipe 0      5745 / 5745                31 / 31
  t_pipe 3000   1612 / 2509                19 / 25
  t_pipe 6000   1763 / 3812                14 / 17
  t_pipe 9050   -910 / 3939             -2025 / 2072
```

9050 µs is the idle arm-to-photons mean — the "obvious" seed — and it degrades
the loaded case by two orders of magnitude. Idle *abs* never falls below
~2500 µs at any setting: that residual is panel-readiness variance on a
stretched VRR panel, and no constant removes it.

**Burst-from-idle on DP-1 remains ~2.7 ms of error** against ~23 µs under
sustained load. That is the first frame of an animation started from a
stretched panel. Whether it is *visible* is a question for the live quality
pass, not for a tighter constant.

## ADR-611 — ENFORCED, and it already was

**Operator decision (accepted):** per-output presentation evaluation is
REQUIRED; cross-output temporal skew is legitimate by design. A window spanning
independently refreshed outputs is ONE semantic object that may have DIFFERENT
presentation samples on each output. That is temporal presentation skew — not
geometry corruption, not animation divergence, not state divergence. It must
not be "fixed" later by restoring a global animation tick.

**And the enforcement was already in force.** This document previously listed
it as not built; that was wrong. `rendermon` ticks every client with THAT
output's sample instant and then composites THAT output, so each output has
always composited its own evaluation. Measured directly from the `anim tick`
trace, two outputs animating one window:

```
  257 ticks across 2 outputs (HEADLESS-1=165, HEADLESS-2=92)
  same window, same animation, geometry difference between outputs:
    median 43px   p95 179px   max 218px
```

(That pairing used a 20 ms tolerance, wider than the true per-frame skew, so
the magnitude is inflated. The target-instant measurement below — median
19.4 px, max 60.8 px — is the accurate figure.)

What was never per-output is the SHARED scene-node position read outside a
render pass: input hit-testing and protocol-visible geometry take whichever
pass ran last. ADR-611 anticipated exactly that and rules it acceptable, both
already tolerating one-period-bounded staleness.

**Skew magnitude, from the target instants:**

```
  skew between targets   median 4.80ms   p95 13.17ms   max 15.05ms
  window speed           4.04 px/ms
  implied disagreement   median 19.4px   p95 53.2px    max 60.8px
```

The max sits just under HDMI-A-1's 16.67 ms period, which is the theoretical
bound — a measurement agreeing with its own bound is what says it is measuring
the right thing. An earlier attempt reported 72 ms median and 362 ms max, which
is impossible; it was measuring the staleness of an output M4G had correctly
declined to wake.

**Single-output windows are unaffected.** M4G's frame reach means an output
never woken by motion it cannot see never ticks the client: 0 backward steps
across 105 moves on one output and 121 on two.
