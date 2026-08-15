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

## P3 — the shatter close

A `shatter` close breaks the window into a square grid of fragments that are
thrown outward, rotate, and fall under gravity. It is the first caller of P2's
arbitrary-corner quad, and the first close animation whose pieces can turn.

**The trajectory is a closed form in wall-clock time** (ADR-616), evaluated at
each output's own presentation instant and never advanced per frame:

```
p(t) = p0 + v0*t + 0.5*g*t^2        theta(t) = theta0 + omega*t
```

Gravity, launch speed and spin are internal constants with hash-seeded
per-fragment jitter — never `rand()`, because a window has to come apart the
same way twice or the purity gate cannot replay it. Only the fragment count is
configurable (`shatter-fragments`, 2–12, default 6).

**One scene node, N quads.** `fall` gives every tile its own scene tree because
a scene node cannot rotate; a shatter fragment can, so the scene holds a single
marker node in `LyrFadeOut` and the AVK walker expands it into one
`AVK_CMD_TEXTURE_QUAD` per fragment, all sampling the window's existing
snapshot image through per-fragment source rects. No capture pass, one buffer
import, and 36 fragments cost one node to position and damage rather than 36.

The renderer recognises the marker through a **registry of live emitters**, not
a tag in `node->data` — `data` already carries a `Client *` for other node
kinds, so reading a magic number out of it would be reinterpreting whatever
that pointer happens to be.

**It ends when the cloud is spent**, not when the clock runs out: once every
fragment has left every screen or faded below visibility, the animation
finishes. This is the same finding the spring work measured on `move` — 26–27
consecutive ticks with the position unchanged, 43–100% of an animation's
committed frames carrying no damage — applied before it could happen again.

**Not on SceneFX.** Rotation is an AVK primitive; on the other renderer the
close falls back to `fall`. The renderers are allowed to differ.

### Gates

Three are arithmetic and live in `tests/test-anim-shatter.c` (2779 checks, run
on every build): purity replay, gravity recovered from the trajectory's second
difference, and the speed bound `|v| <= |v0| + g*t`. Two are compositor
behaviour and live in `contrib/anim-shatter-test.sh`.

| # | switch | correct | broken |
|---|---|---|---|
| P3a | `AZ_BREAK_SHATTER_FRAME_STEP` | 2779/2779 checks; bound approached 0.944 | 2416 fail; bound 2174399 |
| P3b | `AZ_BREAK_SHATTER_DAMAGE_FULL` | damage 732576px of 2073600 | full output |

### The fixture trap this cost, recorded because it passed 3/3 first

The damage gate was originally run against a **tiled** window. A maximised
window's fragment cloud *is* the output, so "damage fits inside the cloud" was
satisfied by a full-output repaint — the assertion passed while the trace
showed `damage_px` of exactly 2073600. The fixture now floats and shrinks the
window, and asserts as a **premise** that the cloud is smaller than the output
before it makes any claim about damage.

The second version then failed for a real reason: every fragment was retired on
the first tick. `Client.mon` and where a window's pixels actually are can
disagree — a window moved across outputs keeps its assigned monitor — so the
trespass rule read "not home" for the whole cloud. The emitter now resolves its
home monitor from the window's own geometry, and visibility is tested against
the whole layout rather than one monitor.

## P4 — what a tag slide costs

Instrumentation first, and its own commit, because the lever it exists to
justify should be argued from a number rather than from an intuition.

A **transition** is the interval during which at least one client is running a
TAG animation. `az_tag_cost.h` accumulates over it: frames, committed frames,
blur prefix rebuild pixels, damage pixels, and the per-frame render cost as a
sample array so p50 and p95 are real order statistics. It closes on the first
frame that sees no TAG animation, emits an `azpace tag cost` line, and keeps
the result for `amsg get avk-stats` to report under `tag_cost`.

p95 rather than a maximum, deliberately: one pathological frame and a
uniformly slow slide are different facts, and a max cannot tell them apart —
the pacing work already made that mistake with a decaying-max estimator that
threw the distribution away.

### Measured, headless, 1 output at 144Hz, 6 transitions per arm

| | blur off | blur on |
|---|---|---|
| blur **priced** px / transition | 0 | 1 200 940 |
| blur **rebuilds** / transition | 0 | **0** |
| blur cache hits / transition | 0 | 12 |
| damage px / transition | 339 000 | 12 417 336 |
| frames / transition | 28 | 28 |
| p50 frame ms | 0.071 | 0.076 |
| p95 frame ms | 0.113 | 0.106 |

The blur-off arm reading exactly zero is the negative control: the counter is
not merely always-nonzero, so the blur-on figure is measuring what it claims.

**Two things worth saying about these numbers before anyone optimises against
them.** The pixel counters move enormously — a blurred tag slide rebuilds 1.2M
prefix pixels and damages 36x what an unblurred one does. The frame-time
percentiles barely move at all, 0.062 → 0.065ms, because a headless run on a
trivial scene is dominated by CPU record time and never pays the GPU cost the
pixels represent. So **the pixel counters are the headless signal and the
percentiles are not**; a claim that the freeze lever makes slides faster has to
be made against a live measurement, not this fixture.

`contrib/tag-cost-test.sh` runs the same slide twice, blur off and on, and
fails if the two are close — a fixture that measured a scene with no blur in it
would report a beautiful zero that reads as "tag slides are already cheap".

## P4b — the blur-freeze lever, DECLINED ON EVIDENCE

The plan's lever was: during a TAG animation, serve a blurred window's backdrop
from the transition's first-frame blur output, translated with the window,
instead of re-rendering it per frame. **It is not built, because the work it
would avoid is already not being done.**

### What the first measurement actually said

P4a reported "blur rebuild px / transition = 1 200 320" and that number is
real, but the label was wrong — and the mistake is worth keeping because it is
an easy one to repeat. `blur_prefix_rebuild_pixels` is accumulated for **every
blur slot whether or not its chain runs**; the comment at its increment site
says so explicitly, because a skipped blur's saving is only meaningful against
what it would otherwise have cost. It is a **price list, not an invoice**.

### The invoice

Probing the per-role and per-cache counters across twelve tag transitions with
a blurred window on screen:

```
blur_role_MONITOR_BACKGROUND_rebuild_px  2073600  before  ->  2073600  after
blur_role_WINDOW_BACKDROP_rebuild_px           0  before  ->        0  after
blur_cache_requests 106   blur_cache_hits 106   blur_cache_rebuilds 1
```

One rebuild, at startup. **106 requests, 106 hits, a 100% hit rate across every
slide.** The M4I background-blur cache already does what the lever proposed,
and does it better: it is keyed on source identity (generation *and* a source
hash) rather than on "a tag animation is running", so it stays correct when the
wallpaper changes mid-slide, which a freeze keyed on TAG motion would not.

`blur_role_WINDOW_BACKDROP_rebuild_px` is zero because the only blurred surface
in this scene takes the bottom-only path. A window blurring *other windows*
would not be cacheable — that is a real remaining case, but it is not what a
tag slide does, and no measurement here shows it costing anything.

### What this leaves

The 36x damage amplification (339 000 → 12 417 336 px per transition) is real
and is **not** blur rebuild cost — it is damage area, i.e. how much of the
output the slide repaints. That is a separate lever with a separate argument,
and it is not the one P4 specified.

`contrib/tag-cost-test.sh` now asserts `blur_rebuilds == 0` for a slide, with
`hits > 0` as the premise that zero means *served* rather than *skipped*. If a
future change starts rebuilding the blur mid-slide it fails there, in the
fixture that knows what the number means.

**No `AZ_SLIDE_NO_BLUR_FREEZE` and no `AZ_BREAK_SLIDE_FREEZE_STALE`** were
added: a control env and a staleness falsifier for a lever that does not exist
would be dead switches, and the project's own rule is that a break which cannot
fail is a suite failure waiting to happen.
