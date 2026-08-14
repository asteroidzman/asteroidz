# M6A — what is built, what is not

Companion to `adr.md` (the design) and `audit.md` (the ground truth it was
designed against). This file says only which parts exist, so that "M6A is in
progress" is a readable state rather than an impression.

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

## Falsifiers, each shown red and green

| # | switch | correct | broken |
|---|---|---|---|
| I1 | `AZ_BREAK_PRESENT_SAMPLE_NOW` | 0 of 367 | 369 of 369 |
| I4 | `AZ_BREAK_ANIM_FRAME_STEP` | 12 ms apart | 150 ms apart |
| I5 | `AZ_BREAK_PRESENT_IDLE_WAKE` | 0 presents | 12 presents |
| I10 | static, no switch | passes | red on a reintroduced `clock_gettime` |
| I12 | `AZ_BREAK_ANIM_RETARGET_POSITION_RESET` | 42 px | 1027 px |

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
- **ADR-608's velocity continuity.** The clock restarts at zero on retarget, so
  velocity resets. Spring-only by decision; beziers excluded because the curve
  family has no state to inject.
- **ADR-609 miss classification.** No verdicts yet. `GPU_LATE` is unreachable
  without `VK_EXT_calibrated_timestamps`, and the ADR is explicit that the
  honest early output is mostly `UNKNOWN`.
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

## ADR-611's enforcement half: the evidence, and why it is a product call

The derivation half landed (`anim_eval_at`). The enforcement half — each output
evaluating the trajectory at its *own* instant instead of sharing one box —
was measured before being built, and the measurement says it is a trade rather
than a fix.

**The skew is bounded and real.** Per-output target instants during an
animation, both outputs driven:

```
  skew between targets   median 4.80ms   p95 13.17ms   max 15.05ms
  window speed           4.04 px/ms
  implied disagreement   median 19.4px   p95 53.2px    max 60.8px
```

The max sits just under HDMI-A-1's 16.67 ms period, which is the theoretical
bound — a measurement agreeing with its own bound is what says it is measuring
the right thing. The first attempt reported 72 ms median and 362 ms max, which
is impossible; it was measuring the *staleness* of an output that M4G had
correctly declined to wake, not a skew.

**It affects straddling windows only.** The worry that a single-output window
is ticked with the other output's instant — `rendermon` walks every client on
every output's pass with no monitor filter — does not materialise: M4G's frame
reach means the other output is never woken by motion it cannot see, so it
never ticks the client at all. Measured: **0 backward steps** across 105 moves
on one output and 121 on two. A monotone trajectory sampled at monotone
instants cannot go backwards, so a non-monotonic step would have been the
defect showing itself. There is none.

**And the error does not disappear either way.** Today both screens draw the
same box and one of them is up to ~61 px stale for its own presentation
instant. Enforced, each screen is correct for itself and they disagree by that
much at the seam. Same magnitude; lag traded for disagreement.

So the decision is: *is up to 61 px of seam disagreement during a fast tag
switch preferable to up to 61 px of temporal lag on one of the two screens?*
ADR-607 rules the disagreement legitimate. It is still a visible change on
adjacent monitors, and "no new cross-output artifacts" is an operator
judgement rather than a measurement — so it is left un-enforced with the
evidence recorded, not deferred for lack of effort.
