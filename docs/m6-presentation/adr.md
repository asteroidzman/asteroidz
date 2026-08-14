# M6A Architecture Decision Records — Presentation Ownership / Frame Timing

Ground truth for every CONTEXT block is `docs/m6-presentation/audit.md`
(written from HEAD `cc59538`); the five gaps G1–G5 are cited by name. ADRs
are numbered in the 600 series so that a bare "ADR-005" always means M5.

Scope note: by the operator's mid-flight direction, M6A covers **both** sides
of one problem — *when* the frame represents the scene (ADR-601..610) and
*where* the final presentation transform is evaluated (ADR-611..617). They
are one document on purpose; the sampling contract of ADR-606 is the hinge
between them.

Facts measured during drafting (no longer assumptions; details in the
final section): `wlr_output_event_present.when` is proven CLOCK_MONOTONIC on
both DRM outputs; presentation feedback is already wired unconditionally
with an `amsg get presentation` surface; the display period must be derived
from vblank-**sequence** deltas, never presented-frame gaps; and **DP-1 runs
with VRR enabled** — its vblank interval tracks content (11.9 ms idle,
7.5 ms busy, against a 6.944 ms minimum), which reshapes ADR-605 entirely.

**Vocabulary fixed for all of M6** (no synonyms permitted in code or docs):

- **frame event** — `wlr_output.events.frame`, per output, ~vblank-aligned.
- **render pass** — one `render_monitor` execution for one output: animation
  ticks, scene walk, AVK snapshot, record, submit, commit.
- **presenter** — the per-output timing owner defined in ADR-602
  (`struct az_presenter`).
- **target presentation time / `target_ns`** — the presenter's prediction of
  when the frame currently being built will turn into light (ADR-601).
- **sample instant / `sample_ns`** — the single instant animation state is
  evaluated at for one render pass. Invariant I1: `sample_ns == target_ns`.
- **presentation timestamp / `present_ns`** — measured page-flip time from
  `wlr_output_event_present.when`, accepted only under ADR-603's rules.
- **nominal period** — `1e12 / wlr_output->refresh` ns, from the committed
  mode (`refresh` is mHz; a 1000× units error here already shipped once and
  was caught by the instrument — the conversion is written exactly once, in
  the presenter). DP-1's mode is 143999 mHz = 143.999 Hz, never "144". On a
  VRR output the nominal period is the **minimum** period (maximum refresh),
  not the interval.
- **observed period** — `Δwhen / Δseq` across accepted presents of one
  epoch: the vblank-sequence-derived display period. Never the gap between
  presented frames: this compositor is damage-driven, consecutive
  presentations are often several vblanks apart, and the frame-gap method
  measured DP-1 at 7673 µs against a 6944 µs mode — a slow display that
  wasn't. **cadence** — the per-present `Δseq` class (x1/x2/x3+).
- **regime** — the presenter's per-epoch prediction mode: `FIXED` (vblank
  grid) or `VRR` (presentation follows the commit); ADR-605.
- **epoch** — one lifetime of a presenter's numeric state; bumped by every
  reset trigger in ADR-604. Cross-epoch arithmetic is forbidden.
- **semantic animation state** — the CPU-owned description of a motion
  (ADR-611): existence, type, endpoints, semantic start time, motion
  parameters, output reach. **presentation animation state** — the per-
  output per-frame evaluation of that motion at `sample_ns` (ADR-611):
  a float transform and the float box it implies, immutable for the pass.
- **presentation transform** — the fractional translation/scale/opacity a
  draw applies at vertex placement time, carried in the frame snapshot
  (ADR-612).
- **verdict** — a per-miss classification per ADR-609: `CPU_LATE`,
  `GPU_LATE`, `KMS_COMMIT_LATE`, `PRESENTATION_SCHEDULING`, `UNKNOWN`.
- **trajectory `X(t)`** — the deterministic function from wall-clock time to
  animated geometry implied by (curve, endpoints, `time_started_ns`,
  `duration`). Wall-clock progress (audit: "what already holds") means
  `X(t)` exists independently of any output; M6A changes only *which t* each
  output evaluates it at.

Break switches follow the project convention (`AZ_` prefix, cf.
`AZ_ANIM_TRUNCATE`): the operator's proposed names are kept with the prefix,
e.g. `AZ_BREAK_PRESENT_SAMPLE_NOW`. The full invariant→falsifier register is
the last section.

---

## ADR-600 — Scope of M6A

**CONTEXT.** "Frame timing" could absorb the render-late controller, VRR
policy, client feedback hints, a frame-scheduler thread, and an adaptive
predictor. The operator's brief is narrower: close G1–G5 with the simplest
measurable model, per output, without touching M3.5's sync guarantees or M5's
colour pipeline.

**DECISION.** M6A delivers exactly: production presentation feedback with
clock-domain validation (ADR-603, since ratified by implementation); the
per-output presenter and its reset semantics (ADR-602/604); the two-regime
deterministic predictor (ADR-605); one sample instant per render pass
threaded to every animated object (ADR-606); the mixed-refresh correctness
contract and its oracle (ADR-607); retarget continuity semantics (ADR-608);
strict miss classification (ADR-609); the idle guarantee (ADR-610); and the
presentation-transform architecture (ADR-611..617): semantic/presentation
state split, the CPU-evaluates/GPU-applies boundary, the damage contract,
window coherence, the colour-domain boundary, the motion-model
requirements, and mixed HDR/SDR straddle semantics. Instrumentation
extends AZ_PACE (`src/common/pace.h`) and the stats surfaces — it does not
replace them.

Explicitly **not** in M6A: any adaptive/filtered predictor (deferred until
the error distribution of ADR-605 is measured); rewriting the render-late
controller in `rendermon` (it keeps its frame-gap adaptation; ADR-605 only
gives it a better period to read, and consuming ADR-609 verdicts is a
follow-up); frame-done timestamp changes toward clients (stage 8 of the
audit is untouched); a broad VRR project (ADR-605 handles the VRR regime's
*prediction*; VRR policy, ranges and dropout behaviour are measured, not
engineered); threads (the brief forbids a CPU threading project — no
multithreaded command recording); moving any compositor semantics onto the
GPU (target, scene membership, focus, tags, lifecycle, damage, retarget all
stay CPU, ADR-612); a compute-shader animation system (hard NO); a tag
snapshot / precomposed layer cache (deferred, not forbidden — reopened only
on new measured evidence); reopening any closed theory (tag-overlap, DPM
prewarm, transient churn, prefix copy, rectangle cap, old blur-cache
theories).

**REJECTED ALTERNATIVE.** Folding render-late and the presenter into one
controller now. Render-late works, is live-tuned, and has its own failure
history (the arm-floor trap documented at its site). Coupling a new
predictor to a proven controller risks both; the presenter is measurement
first, authority second.

**FALSIFIER.** Scope ADRs are falsified by diffstat: an M6A change that
touches `src/render/vulkan/sync/`, the M5 colour files, or the render-late
adaptation block beyond reading the presenter's interval is out of scope and
must be reverted or re-argued.

---

## ADR-601 — What "target presentation time" means, and what it guarantees

**CONTEXT.** G2: animation is sampled at the instant the CPU walks the node,
which is always earlier than the light it produces — a meaningful fraction of
the 6.944 ms budget on DP-1 (143.999 Hz). Fixing that requires a defined
future instant, and the brief forbids pretending that instant is known.

**DECISION.** `target_ns` is **a prediction of the CLOCK_MONOTONIC instant
at which the frame being built in this render pass will be presented**
(turn into light) on this output. Its contract:

1. It is computed **once per output frame**, at frame-event time, by that
   output's presenter and no other component (ADR-605 gives the rule).
2. It is a *prediction*, never asserted as fact. Its error,
   `present_ns − target_ns` for the frame it armed, is a first-class
   measured quantity (recorded per accepted present, aggregated per output:
   count, p50, p95, max, signed mean).
3. It is derived only from this output's presenter state (epoch-local
   phase/latency state, period, regime) plus CPU `now`. Never from another
   output, never from a frame counter, never from a global tick.
4. Within one epoch, successive targets are strictly increasing.
5. When the render pass runs deferred (render-late) or overruns, the armed
   target is **not** recomputed. We aimed at that instant; if we miss it,
   the miss machinery (ADR-609) reports it. Resampling forward would erase
   the evidence.

Guarantee 2 is the load-bearing one: the model is chosen so that its failure
is a number in a log, not a feeling of "laggy".

**REJECTED ALTERNATIVES.** Sampling at "now + fixed latency constant": hides
its own error, per-output constants rot, and it degenerates to G2 with extra
steps. Sampling at "next frame event time": frame events are input to the
prediction, not presentation; on a loaded system they drift from flips in
exactly the way G4 already cannot see.

**FALSIFIER.** Invariant I1 (`sample_ns == target_ns`) under
`AZ_BREAK_PRESENT_SAMPLE_NOW` (sample at CPU now): the trace-consistency
oracle (ADR-606) must fail deterministically on the first animated frame.
Guarantee 3 under `AZ_BREAK_PRESENT_GLOBAL_CLOCK`: see ADR-607's falsifier.

---

## ADR-602 — Ownership: `struct az_presenter`, embedded in Monitor

**CONTEXT.** Timing state today is scattered: `pace_last_present_ns` (trace
only), `render_late_last_ns` / `render_late_frac` / `render_dur_ms`
(controller), five independent `clock_gettime` sites in animation code (G3),
and nominal refresh read ad hoc (G5). Nothing owns "what time is it on this
output". The milestone's phrasing is "AVK should own the relationship", but
sampling, KMS commit and output lifecycle are main-loop compositor concerns;
what AVK uniquely owns is GPU execution timing.

**DECISION.** One value-type struct, embedded (not pointed to) in `Monitor`:

```c
struct az_presenter {
    uint32_t epoch;              /* bumped by az_presenter_reset() */
    enum az_present_regime regime; /* FIXED | VRR — fixed per epoch */
    enum az_present_sync sync;   /* UNSYNCED | SYNCED (+ per-frame TEARING
                                    overlay, ADR-605) */
    enum az_present_clock clock; /* UNKNOWN | MONOTONIC | FOREIGN */
    uint64_t nominal_period_ns;  /* from the committed mode; the MINIMUM
                                    period when regime == VRR */
    uint64_t t_pipe_ns;          /* VRR regime: commit-to-photons estimate,
                                    seeded from measurement M-8 (ADR-605) */
    uint64_t last_present_ns;    /* meaningful only when SYNCED */
    uint32_t last_seq;           /* per-epoch, delta use only */
    uint32_t reset_commit_seq;   /* wlr_output->commit_seq at reset */
    uint64_t armed_target_ns;    /* 0 = no frame armed */
    /* observed-period accumulator: sum(Δwhen), sum(Δseq) — ADR-605 */
    /* small ring of {commit_seq, target_ns, t_commit_ret_ns, gpu_done_ns}
     * for frames in flight (depth 4) — feeds ADR-609 */
    /* error aggregates + verdict counters (ADR-609) */
};
```

The shipped `amsg get presentation` surface (per output: presented,
dropped, no_stamp, last_seq, nominal_refresh_mhz, observed_interval_us,
interval_rejected, cadence x1/x2/x3plus, stamp_clock, both clock skews) is
the read model of this struct and grows with it (regime, error aggregates,
verdicts); it does not fork into a second bookkeeping path.

Location: `src/present/az_presenter.{h,c}`, compositor core. AVK's
contribution is confined to filling `gpu_done_ns` (ADR-609) — the presenter
has no Vulkan types in its API. This is a deliberate reading of "AVK owns
the relationship": the *relationship* spans compositor and GPU, and its
state must live where frame events, commits and output lifecycle already
live — the Monitor.

Mutation is limited to exactly four functions; everything else, including
all of AVK, animation code and the render-late controller, is read-only:

- `az_presenter_reset(m, reason)` — every trigger in ADR-604.
- `az_presenter_arm(m, now_ns)` — frame-event time; computes `target_ns`.
- `az_presenter_committed(m, t_commit_ret_ns)` — after a successful
  `wlr_output_commit_state`; records the in-flight entry keyed by the
  output's commit seq.
- `az_presenter_present(m, ev, now_ns)` — the present listener (ADR-603).

Readers get accessors that **refuse to return stale numbers**: e.g.
`az_presenter_sample_ns()` aborts (debug) / logs-once-and-falls-back
(release) if no frame is armed; phase state is unreadable while `UNSYNCED`.
`pace_last_present_ns` is subsumed (the trace reads the presenter);
render-late state stays where it is (ADR-600) but reads
`az_presenter_period_ns()` instead of recomputing from `refresh`.

**REJECTED ALTERNATIVES.** A global presenter array indexed by output: the
Monitor already has the right lifetime, and embedding-by-value makes reset
by construction trivial (ADR-604). Ownership inside `az_avk_output`: ties
timing to the renderer; the scenefx fallback path (BROKEN outputs, M3.5C)
would then have no timing owner at all. A pointer member: invites a stale
pointer across hotplug — the exact class M3.5's history warns about.

**FALSIFIER.** Static: a build-time test greps that `->presenter.` writes
occur only in `az_presenter.c` (same spirit as the no-clock grep oracle,
ADR-606). Runtime: the accessors' refuse-stale behaviour is exercised by
the epoch falsifier in ADR-604.

---

## ADR-603 — Presentation feedback is production input; the clock domain is proven, not assumed

**CONTEXT.** G1: at `cc59538`, outside `AZ_PACE=1` the present listener was
not even wired — the compositor never learned when a frame reached the
screen. Separately, the audit flagged `ev->when`'s clock domain as an
assumption. Both are now settled by implementation and measurement
(mid-M6A): the listener is wired unconditionally with the
`amsg get presentation` surface, and the clock domain is **proven
CLOCK_MONOTONIC** on both DRM outputs by recording the stamp's distance to
both clocks on the first presented frame — DP-1: 56.089 ms behind
CLOCK_MONOTONIC vs ~1.8×10¹⁵ ms from CLOCK_REALTIME; HDMI-A-1: 7.818 ms
behind CLOCK_MONOTONIC. Nine orders of magnitude of separation. This ADR
records the contract that implementation ratified, because the guard must
outlive this hardware: a future backend may still report something else,
and the shipped probe already answers "neither" for that case.
`ev->presented` false is known to fire for dropped updates.

**DECISION.** The present listener is wired **unconditionally** at monitor
setup (done); `pacepresent`'s trace output stays gated behind `az_pace_on()`
but feedback processing does not. This adds no idle cost: present events
are edge-triggered by our own commits (ADR-610).

`az_presenter_present()` accepts a sample into phase state only if **all**
of the following hold; otherwise it updates counters and returns:

1. `ev->presented` is true. A drop never anchors phase and never enters the
   interval series (audit: folding one in invents a refresh that never
   happened). A drop does *not* demote `SYNCED` — the previous real present
   remains a valid phase anchor.
2. `ev->when` is nonzero. A zero timestamp with `presented == true` is
   treated as "no timestamp": the event still proves *a* flip happened
   (usable for `seq` accounting and ADR-609's UNKNOWN bookkeeping) but
   anchors nothing.
3. The event's commit seq is `>= reset_commit_seq` — a flip from a commit
   made before the current epoch is discarded by construction (ADR-604).
4. The clock-domain gate passes, as follows.

**Clock-domain gate, per epoch.** `clock` starts `UNKNOWN`. On each
candidate sample the handler reads CLOCK_MONOTONIC `now` itself and checks
plausibility: a genuine monotonic page-flip timestamp for an event being
delivered *now* satisfies `when <= now` and `now − when < 50 ms` (event-loop
delivery latency, generously bounded; the flip that caused this event just
happened — the measured live skews, 7.8–56 ms, sit inside this window).
First sample passing the check sets `clock = MONOTONIC` for the epoch; a
sample failing it while `UNKNOWN` sets `clock = FOREIGN` (the shipped
probe's "neither" verdict maps here) and logs once per epoch. In `FOREIGN`:
phase/latency anchoring is disabled permanently for the epoch, the
presenter runs the UNSYNCED prediction rule (ADR-605), and period
statistics come from `seq` deltas × nominal only. The failure mode is
chosen deliberately: a monotonic clock misclassified as foreign (event
delivered > 50 ms late) degrades to the conservative fallback; a foreign
clock can never be misclassified as monotonic — the measured margin is nine
orders of magnitude. Re-checked per epoch, so suspend/resume (which resets
the epoch anyway, ADR-604) re-proves it.

**Period derivation rule** (measured, binding): the observed period is
`Δwhen / Δseq` across accepted presents — the vblank-sequence delta — and
never the raw gap between presented frames. A damage-driven compositor
presents sparsely; the frame-gap method read DP-1 at 7673 µs against a
6944 µs mode. `Δseq` per present is also the cadence (x1/x2/x3+), derived
from real presentation rather than inferred from GPU timing.

**Assumptions still open** (Opus: measure during bring-up, record results
in this directory): (a) whether the **headless** backend emits present
events at all, with what `presented`/`when` values — unproven; the design
requires nothing from it (a silent backend simply leaves the presenter
UNSYNCED, which is the honest state); (b) whether `ev->refresh` (the
backend's own until-next-refresh hint) is populated — recorded as a
cross-check series, not consumed by the predictor.

**REJECTED ALTERNATIVES.** Trusting the wlroots contract ("DRM timestamps
are monotonic"): the project has a standing rule against exactly this shape
of inherited assumption. Converting foreign timestamps by sampling both
clocks and offsetting: adds a cross-domain arithmetic path that is wrong by
up to the sampling jitter, is untestable on hardware we don't have, and
exists to rescue a case the fallback already handles honestly.

**FALSIFIER.** `AZ_BREAK_PRESENT_STALE_FEEDBACK` (handler counts the event
and returns before any state update): the presenter must remain `UNSYNCED`
with an empty prediction-error series after N presented frames — oracle
asserts `sync == SYNCED` and `error_count > 0` on the healthy build and
fails on the break. Premise test for the gate: a unit test feeds
`az_presenter_present` a synthetic event carrying a CLOCK_REALTIME-scale
`when` and asserts `clock == FOREIGN`, no phase anchor, one log.

---

## ADR-604 — The presenter state machine and epoch reset semantics

**CONTEXT.** At HEAD, timing-adjacent state is initialised once at monitor
creation (`render_dur_ms`/`render_late_*` at setup) and **never reset** on
mode, refresh, scale or transform changes. A refresh change from 143.999 Hz
to 60 Hz would leave a controller reasoning with a 6.944 ms interval against
16.667 ms reality until it happened to re-derive. The brief requires
staleness to be impossible by construction, not discipline.

**DECISION.** Presenter lifecycle is `UNSYNCED → SYNCED` (first accepted
present of the epoch). The **regime** — `FIXED` or `VRR` — is chosen once
per epoch, at reset, from the adaptive-sync status of the newly committed
state; it never changes within an epoch, because an adaptive-sync toggle
arrives via a commit carrying ADAPTIVE_SYNC_ENABLED, which is itself a
reset trigger. Tearing remains a per-frame overlay (ADR-605): it suspends
anchoring for that frame without touching the epoch. All numeric state is
epoch-local.

`az_presenter_reset(m, reason)` reassigns the entire struct to a fresh
value — `epoch + 1`, regime from the committed state, `UNSYNCED`, `clock
UNKNOWN`, `nominal_period_ns` re-derived from the *newly committed* mode,
`reset_commit_seq = wlr_output->commit_seq`, everything else zero. Because
the struct is embedded by value and reset is total reassignment, "a stale
field survived the reset" is not an expressible state.

Caveat flagged for measurement M-10: if it turns out wlroots/asteroidz can
enter or leave VRR *without* an output commit this compositor makes (the
`is_vrr_opening` flag and the tearing path both exist and their transitions
are unaudited), the trigger list below must grow to cover that path before
the regime-per-epoch claim is trusted.

**Reset triggers** (exhaustive; `reason` is logged and counted):

1. Monitor creation.
2. Any asteroidz-initiated `wlr_output_commit_state` that succeeds and
   whose state mask includes MODE, CUSTOM_MODE, ENABLED, SCALE, TRANSFORM
   or ADAPTIVE_SYNC_ENABLED. Constructional form: the reset call sits in
   the (few) helpers that build such states, immediately after the
   successful commit — not sprinkled at call sites.
3. Backend-initiated state changes: the `request_state` path
   (`requestmonstate`) after its commit, and `updatemons` when it
   enables/disables an output.
4. Session active transitions (VT switch back) and DPMS wake (`asleep`
   clearing) — the display was not scanning out; phase is void.
5. Output destroy is trivially covered (the Monitor dies).

Scale and transform do not change vblank physics; they are included because
they arrive via full output commits that *may* carry a modeset with them,
and the cost of a conservative reset is one UNSYNCED frame (ADR-605's
fallback), which is cheaper than one wrong subtraction. This is written
here so nobody "optimises" the reset away later without meeting the
falsifier.

**In-flight leakage.** A flip from a pre-reset commit can deliver its
present event after the reset. Gate 3 of ADR-603 (`commit_seq <
reset_commit_seq` ⇒ discard) closes this by construction — no discipline
about listener ordering required. `seq` and `last_seq` are compared only
within an epoch, as deltas, so uint32 wrap and backend renumbering across
modesets are non-events.

**REJECTED ALTERNATIVES.** Field-wise invalidation ("clear last_present on
modeset"): the audit of render-late's arm-floor trap shows exactly how a
single surviving field silently wedges a controller; partial resets are the
disease, not the cure. Validating timestamps by age heuristics instead of
epochs ("ignore presents older than 1 s"): turns hotplug races into
probabilistic bugs.

**FALSIFIER.** `AZ_BREAK_PRESENT_NO_EPOCH_RESET` (reset body runs only at
monitor creation): headless fixture commits a custom-mode refresh change
60 Hz → 144 Hz mid-run; oracle asserts the next armed frame's trace line
carries `nominal_period_ns ≈ 6.944 ms` and a bumped epoch. With the break
the line shows the old epoch and 16.667 ms — deterministic failure. (Assumption
flagged for the fixture: the headless backend honours custom-mode refresh
in its frame timer; Opus verifies this before trusting the fixture — see
ADR-607's fixture note.)

---

## ADR-605 — The predictor: two regimes, different in kind

**CONTEXT.** The brief proposed the simplest lattice predictor —
`next_target = last_presented + one_refresh_period` — and demanded its
error be measured before anything adaptive is considered. Measurement
overturned the lattice premise on the output that matters most: **DP-1 runs
with VRR enabled** (`vrr_capable`, `vrr_enabled` both true at HEAD's live
config). Its vblank-sequence-derived period is not a constant: 11 916.8 µs
on an idle desktop, 7 475.4 µs under continuous damage, trending toward the
6 944.5 µs mode floor — with cadence overwhelmingly x1 (3384 × x1 vs 35 ×
x2), so these are genuinely stretched vblanks, not skipped ones. HDMI-A-1
meanwhile is an exact fixed grid: 16 667.3 µs observed vs 16 666.7 nominal.
The two outputs therefore pose prediction problems different **in kind**,
not in parameter: on a fixed grid, presentation lands on vblank slots and
the compositor predicts which slot it will hit; under VRR, presentation
largely *follows the commit*, bounded by the panel's range, so the
compositor substantially **chooses** the presentation time by choosing when
to commit — and what it must predict is its own commit-to-photons latency.
A lattice model applied to DP-1 would be systematically early by up to
~5 ms on an idle desktop: most of a frame at the floor period.

**DECISION.** The regime is fixed per epoch (ADR-604). At frame-event time,
`az_presenter_arm(m, now)`:

**FIXED regime** (HDMI-A-1; any epoch with adaptive sync off):

- **SYNCED, clock MONOTONIC, not tearing:**
  `n = floor((now − last_present_ns) / P) + 1;  target_ns =
  last_present_ns + n·P`, with `P = nominal_period_ns` — the next
  vblank-lattice point strictly after `now`. No guard bands, no
  pipeline-depth constant: the frame event fires just after a vblank and
  the M3.5 pipeline commits before the next one, so the first lattice
  point after `now` *is* the intended flip. If the error series shows a
  consistent one-period bias (commits routinely landing a vblank later),
  the signed mean says so directly and a `+P` correction is a one-line,
  evidence-backed change. Using nominal `P` is now justified by
  measurement, not argument: 0.6 µs error at 60 Hz, and re-anchoring on
  every accepted present bounds nominal-vs-true drift to the projection
  window (≤ 2 periods) — nanoseconds. G5's phase-accumulation warning
  dissolves under re-anchoring; recorded here so nobody builds an interval
  filter against a problem that no longer exists. The observed period
  (Δwhen/Δseq) remains the standing cross-check.
- **UNSYNCED, or clock FOREIGN:** `target_ns = now + P`. Honest and coarse;
  error bounded by about one period, strictly better than G2's
  sample-at-now, and it never poisons phase state.

**VRR regime** (DP-1 today; any epoch with adaptive sync on):

- **SYNCED, clock MONOTONIC, not tearing:**
  `target_ns = max(now + t_pipe_ns, last_present_ns + P_min)`, where
  `P_min = nominal_period_ns` (the max-refresh floor, 6.944 ms on DP-1) and
  `t_pipe_ns` is the arm-to-photons estimate: the time from this instant
  through render, commit and scanout to light. In M6A `t_pipe_ns` is a
  **per-output deterministic constant**, seeded from measurement M-8
  (commit-to-present latency on DP-1, which Opus can instrument now that
  both stamps share a proven clock domain) — it never self-adjusts within
  the milestone. Until M-8 lands, the placeholder is `P_min`, documented as
  a placeholder and expected biased; the error series quantifies exactly
  how biased.
- **UNSYNCED, or clock FOREIGN:** `target_ns = now + t_pipe_ns` (no floor
  available; none needed for one honest frame).
- Error asymmetry, stated so the series is read correctly: on FIXED a
  prediction error is a symmetric slot miss; on VRR, photons can trail but
  never precede the commit, so predicted-too-early shows as one-sided
  positive error bounded below by the floor. The metric stays
  `present − target` in both regimes; its interpretation is per-regime.

**Tearing overlay** (either regime): the frame is neither anchored nor
error-scored; sampling still needs an instant, so `target_ns = now +
P_min` best-effort — a tearing frame presents mid-scanout by intent and
pretending precision would be false knowledge.

**First frame** of an epoch is by definition UNSYNCED → fallback rule.
**After idle, FIXED:** the presenter stays SYNCED with an old
`last_present_ns`; lattice projection with large `n` holds **on the
assumption that vblank phase is continuous across idle scanout** — flagged,
not asserted: the first accepted present after an idle gap lands in a
separate "post-idle" error bucket, so a rephasing panel shows up as a
bimodal post-idle distribution. **After idle, VRR:** the floor almost never
binds (the last present is long past), so `target = now + t_pipe` — and
this idle-burst first frame is precisely the case that matters most on the
live desk, so M-8 must measure it separately from the continuous-damage
steady state (the ~84 Hz idle cadence suggests low-framerate compensation,
whose first-commit-after-quiet behaviour is unknown; M-9). **After a
miss:** nothing special in either regime. The anchors self-correct on the
next accepted present; a special-cased "miss recovery" path would be
untested code on the hottest failure path.

The armed target is stored once (`armed_target_ns`) and consumed by the
render pass whether it runs immediately or via the render-late deferral —
computed at frame-event time, constant for the pass (ADR-601 guarantee 5).

**Error measurement** (the actual deliverable of this ADR): for every
accepted present matching an in-flight entry (ADR-602 ring),
`err = present_ns − target_ns` feeds per-output aggregates (count, signed
mean, p50, p95, max, post-idle bucket, **regime tag**) surfaced through the
presentation IPC and, under AZ_PACE, per-event trace lines. The decision
gate for M6B: adaptive work happens only where the per-regime |p95| says
the deterministic rule is insufficient (working threshold ~0.5 ms), and
only for the term the data indicts (`t_pipe` load-dependence being the
likely candidate on VRR).

**REJECTED ALTERNATIVES.** One parameterised model treating VRR as a
perturbed lattice: measured ~5 ms systematic error on an idle DP-1 — the
regime split is the evidence-backed shape. Predicting "the next vblank"
under VRR: the vblank follows the commit; the quantity does not exist
independent of us. Observed-period filtering as FIXED-regime input: buys
nanoseconds under re-anchoring (above). Frame-gap-derived periods:
measured wrong (7673 µs vs 6944 µs), banned by ADR-603's derivation rule.
Adaptive `t_pipe` estimation in M6A: the constant's measured error is what
justifies or kills adaptivity; building it first inverts the evidence
order. Shaping presentation by scheduling commits (delaying commits to hit
chosen instants): that is a real VRR pacing project with real risks, out
of M6A scope, and the render-late controller already occupies the
commit-timing seat.

**FALSIFIER.** The prediction rules are pure and unit-tested exhaustively
(lattice projection across wrap/idle/epoch boundaries; VRR floor binding
and non-binding cases; placeholder vs seeded `t_pipe`). Regime selection is
falsified by the ADR-604 fixture (adaptive-sync toggle mid-run ⇒ epoch bump
+ regime flip in the trace; fails under `AZ_BREAK_PRESENT_NO_EPOCH_RESET`).
End-to-end honesty is falsified through ADR-603's break (no feedback ⇒ no
error series) and I1's oracle. *Accuracy* is not an invariant — it is the
published, per-regime measured output, and that publication is the
falsifier for any claim that the deterministic rule does or does not
suffice.

---

## ADR-606 — One sample instant per pass, threaded, with clocks banned from animation code

**CONTEXT.** G3: five sites (`animation/client.h:2324`-region and `:2510`-
region, `animation/layer.h` ×2, `draw/ufo-node.c`) each call
`clock_gettime` mid-walk; two windows animating in one frame sample at
instants separated by the walk time between them. This is an ownership
question: no better target helps if every object still asks the clock
itself.

**DECISION.** At the top of each render pass, exactly one call:
`uint64_t sample_ns = az_presenter_sample_ns(&m->presenter);` — returning
the armed `target_ns` (I1) and refusing to answer outside an armed pass
(ADR-602). The value is threaded **as an explicit parameter** down the draw
path: `client_draw_frame(c, sample_ns)` →
`fadeout_client_animation_next_tick(c, sample_ns)` and the layer/ufo
equivalents; deep chains carry it in the existing per-pass argument or a
small pass-context struct if a signature becomes unwieldy. Progress math is
unchanged — `passed = sample_ns − time_started_ns` — which preserves
wall-clock, refresh-independent animation exactly (the hard constraint):
only the *instant* changed, from "when the CPU got here" to "when this
output will show it". The same `sample_ns` is the evaluation instant for
the presentation transform (ADR-611/612): one instant per pass drives both
the integer damage anchor and the fractional draw placement, which is what
makes the coherence invariant of ADR-614 expressible at all.

`clock_gettime`/`az_pace_now_ns` are **banned from animation sampling
code** — `src/animation/*.h` tick paths and `draw/ufo-node.c` — enforced by
a build-time grep test (the static twin of the runtime falsifier). Non-
sampling uses in those files (stamping `time_started_ns` at animation
*start*, which is a semantic event that genuinely happens at CPU now) move
to `client_commit`-side helpers so the banned files contain no clock reads
at all.

Consequence, stated so it is not "discovered": the first presented frame of
an animation shows progress equal to the start→present latency rather than
≈0. That is correct — by the time the frame is light, that much wall time
*has* elapsed — and it is precisely the G2 staleness, removed.

**REJECTED ALTERNATIVES.** A module-global "current frame clock" set around
the pass (the `az_pace_mon` pattern): that is attribution plumbing for a
debug trace, acceptable there, but for correctness-bearing state it is
discipline where the brief demands construction — nothing stops a stray
read outside a pass. Leaving per-object reads but passing a shared base:
keeps five clock sites to audit forever.

**FALSIFIER.** Runtime: `AZ_BREAK_PRESENT_SAMPLE_NOW` substitutes CPU-now
for the returned value; the AZ_PACE tick line gains `sample_ns` and
`target_ns` fields, and the analyser's invariant `sample_ns == target_ns`
for every tick of a pass (all ticks in one pass equal, matching the pass's
armed target) fails on the first animated frame, deterministically, headless.
Static: the grep test fails if any banned file regrows a clock call.

---

## ADR-607 — Mixed-refresh semantics: one trajectory, per-output instants

**CONTEXT.** DP-1 runs 143.999 Hz, HDMI-A-1 60 Hz. Animation ticks run
inside each output's render pass over the global client list, mutating
**shared** scene-node geometry, which each pass then snapshots (audit
stages 4–5; the pace-trace attribution comment in `pace.h` exists because
of exactly this). The brief demands a falsifiable definition of "correct"
for a window straddling both outputs — this is the oracle Opus builds.

**DECISION.** One semantic animation state — one `X(t)` per animation —
sampled per output at that output's own `sample_ns`. Structurally: each
pass evaluates the trajectory at its own sample instant into per-pass
presentation state and snapshots immediately (ADR-611/612); damage derives
from the evaluated float boxes per ADR-613. The shared scene-node integer
position becomes an anchor updated to `round(X(sample_ns))` by whichever
pass ran last — scratch between passes (last writer), consequential only
for input hit-testing and protocol-visible geometry, both of which already
tolerate one-period-bounded staleness today.

**The falsifiable statement.** For every output `o` and every frame `f`
committed on `o` in epoch `e`:

1. The geometry committed in `f` equals `round(X(sample_ns(f)))` per the
   `anim_lerp` llround rule — the committed position lies **on the
   trajectory, evaluated at this frame's own sample instant**, within 1 px
   per axis.
2. `sample_ns(f)` equals the target armed by `o`'s **own** presenter for
   `f` (I1 + ADR-601 guarantee 3).
3. A semantic animation completes (reaches `passed ≥ duration`, or the
   spring settle criterion, ADR-616) at the same wall-clock time on every
   output, within one local period (the minimum period on a VRR output)
   plus that output's prediction error.

**Legitimate and unavoidable** inter-output differences: the two panels
show the window at different positions at any common wall instant, exactly
insofar as their present instants differ — both position sequences are
exact samples of the *same* `X(t)`; the 60 Hz panel shows ~2.4× fewer
intermediate positions; teardown/final-state convergence may differ by up
to one 60 Hz interval (a completion detected during the 144 Hz pass
destroys fadeout nodes that the 60 Hz output then simply no longer shows —
bounded, and the post-completion state is the resting state by
definition). **A bug**, by this contract: any committed geometry off the
trajectory at its own sample instant (e.g. sampled at the other output's
instant, or at CPU-now); any output consuming another presenter's target;
completion time diverging with refresh (frame stepping); the 60 Hz output
receiving *zero* updates while the animation is visible on it (reach
regression).

**The oracle** (extends `contrib/pace-analyse.py`): the `anim start` trace
line gains the curve identity and parameters (bezier points or spring
ζ/ω — all in `config`, all deterministic); the tick line already carries
stored integer geometry and gains `sample_ns`/`target_ns`/`epoch`/`mon`
(ADR-606). The analyser recomputes `X(sample_ns)` per tick and asserts
statements 1–3. This is an internal-consistency oracle: it runs headless
and does not require real vblank hardware. A second, live-only oracle layer
compares against *actual* presents (statement 3 with measured `present_ns`)
and is run on the real desk per the live-testing rules.

**Fixture note (assumption, not assertion).** The mixed-refresh fixture
wants two headless outputs at 60 and 144 Hz custom modes. Whether the
headless backend paces frame events from custom-mode refresh is unproven
at HEAD; Opus verifies it first (trap register: nominal-vs-observed Hz is
already a known fixture trap). If it does not, the fixture falls back to
asserting statements 1–2 headless (they are refresh-independent) and
statement 3 on the live desk.

**REJECTED ALTERNATIVES.** *Persistent* per-output geometry overlays (each
output holding a retained copy of every animated node's position — as
distinct from ADR-611's ephemeral per-pass presentation state and
ADR-613's framebuffer history): forks the scene graph's geometry model and
buys precision for hit-testing that input latency already dwarfs —
rejected as a structure change without a measured defect behind it.
Sampling once globally at the fastest output's target: reintroduces a
global tick by the back door and makes the 60 Hz output present provably
wrong instants.

**FALSIFIER.** `AZ_BREAK_PRESENT_GLOBAL_CLOCK` (every arm copies DP-1's /
output 0's target): statement 2 fails deterministically on the second
output's first animated frame in the headless two-output fixture.
`AZ_BREAK_PRESENT_FRAME_STEP` (progress advances by `tick_count ×
interval` instead of wall clock): statement 3 fails — the slower output
completes the same animation ~2.4× later. Both must also *pass* on the
healthy build in the same run (a green break run is a suite failure, per
the project's break-test rule).

---

## ADR-608 — Retarget: position continuity is an invariant now, velocity continuity is spring-only and deferred

**CONTEXT.** `client_commit` already implements position continuity: on
retarget, `initial` becomes the interpolated current geometry and the clock
restarts (the AZ_PACE `anim start` line was written to expose exactly this).
Velocity is discarded: a bezier restarts at its curve's initial slope, a
spring restarts from rest. The brief: position continuity required,
velocity continuity desired where the motion model supports it, and spring
polish must not block the timing core.

**DECISION.** Two invariants with different standing:

1. **Position continuity (required, M6A):** across a retarget, the
   *presented* position sequence on each output has no discontinuity
   exceeding the trajectory's own motion — formally, consecutive committed
   positions around the boundary differ by at most
   `max(|Ẋ_old|, |Ẋ_new|) · local interval + 1 px`. Defined at
   presentation rather than in state space because presented frames are
   what the eye sees and what the trace records. To keep the seed exact
   under target-time sampling, the retarget anchors the new segment at the
   last *sampled* instant, not at CPU-now: `initial = X_old(s)` and
   `time_started_ns = s`, where `s` is the most recent `sample_ns` at which
   the old segment was evaluated. Anchoring at CPU-now while seeding with a
   position sampled for a future instant would double-count the lead
   interval — this is the one place ADR-606 changes retarget math.
   Multi-output wrinkle, stated as contract: the seed was produced at one
   output's instant (last writer); the discrepancy this injects into the
   other output's sequence is bounded by the same speed × interval term and
   is legitimate.
2. **Velocity continuity (desired, spring-only, M6A.x — may land after the
   timing core):** for spring-eased geometry, retarget captures the
   analytic instantaneous velocity of the old spring state at `s` (from the
   closed form in `animation/common.h`, never by finite-differencing stored
   integers) and starts the new spring with that initial velocity; the
   damped-spring solution with nonzero `v₀` is the defined semantics.
   Bezier curves are **excluded by decision, not omission**: the curve
   family has no state to inject, and silently substituting a spring under
   a user-configured bezier would change chosen motion — out of bounds.

**REJECTED ALTERNATIVES.** Requiring velocity continuity for all curves
(forces the bezier substitution above). Blocking M6A on the spring work
(explicitly against the brief). Seeding retargets from a fresh CPU-now
sample of the old trajectory: re-adds a clock read to animation code,
violating ADR-606's ban.

**FALSIFIER.** Position: the ADR-607 oracle gains a retarget check —
around each `anim start … retarget=1` line, the boundary condition in (1)
must hold on every output's tick series. `AZ_BREAK_ANIM_RETARGET_POSITION_RESET`
(seed the new segment from the animation's *origin* endpoint instead of
`X_old(s)` — the pre-M4-era failure shape) must trip that check
deterministically on a mid-flight retarget fixture; the existing
`AZ_BREAK_PRESENT_SAMPLE_NOW` also perturbs it (seed instant vs sample
instant mismatch) and must trip it too. Velocity, once implemented:
`AZ_BREAK_PRESENT_RETARGET_ZERO_VELOCITY` (drop the injected `v₀`): oracle
finite-differences the *trace* geometry across the boundary and asserts the
pre/post velocity ratio stays above a threshold for a mid-flight spring
retarget fixture; with the break the post-boundary velocity collapses
toward zero — deterministic failure. Until the spring work lands, that
break and oracle are absent, not stubbed green.

---

## ADR-609 — Miss classification: verdicts require timestamps, or they are UNKNOWN

**CONTEXT.** G4: a "miss" today is inferred from frame-event spacing, which
cannot separate CPU-late from GPU-late from commit-late, and every
unexplained miss defaults, culturally, to "GPU too slow". The brief demands
verdicts that name their evidence and an UNKNOWN that is used honestly.

**DECISION.** A **miss** is: an accepted present with
`present_ns − target_ns > tol` for its matched in-flight frame (`tol =
nominal_period_ns / 2` in the FIXED regime; `P_min / 2` initial in the VRR
regime, revisable from the M-8 latency distribution), or — FIXED regime
only — a per-epoch `seq` delta > 1 across a window where a frame was in
flight (under VRR a stretched cadence is the panel following content, not a
slip). Each miss gets exactly one verdict, from the first rule that its
evidence *proves*; margins `δ = 500 µs` initial, config-tunable:

| verdict | required evidence | reading |
|---|---|---|
| `CPU_LATE` | `t_commit_ret ≥ target_ns` | the commit completed after the intended flip instant; no GPU or display facts needed — it provably could not have made it. The in-flight entry records the render span and commit-call span separately so the log shows *which* CPU phase dominated, but the verdict stays CPU_LATE. |
| `KMS_COMMIT_LATE` | `t_commit_call < target_ns − δ` **and** `t_commit_ret ≥ target_ns` | the pass was ready in time and the atomic-commit call itself consumed the margin. A commit that returned in time but was scheduled onto the next vblank by the kernel is **not provable** from CPU timestamps and must not land here. |
| `GPU_LATE` | `t_commit_ret < target_ns − δ` **and** `gpu_done_ns ≥ target_ns − δ` | commit handed over in time; the GPU signalled the in-fence too late for the flip. Requires a real `gpu_done_ns` (below). |
| `PRESENTATION_SCHEDULING` | `t_commit_ret < target_ns − δ` **and** `gpu_done_ns < target_ns − δ` **and** miss anyway | everything provably ready; the display side flipped late. In the VRR regime this verdict also absorbs panel-floor and low-framerate-compensation effects (M-9) — the log records the regime so the two populations never mix. |
| `UNKNOWN` | anything else | including: no `gpu_done_ns` (extension absent, query unavailable this frame, or timestamps disabled), clock `FOREIGN`, no matched in-flight entry, `presented == false` with nothing else, first frame of an epoch. |

`gpu_done_ns` is AVK's sole contribution (ADR-602): a
`vkCmdWriteTimestamp2` at the end of the frame's command buffer, mapped to
CLOCK_MONOTONIC via `VK_EXT_calibrated_timestamps`, read back with
`VK_QUERY_RESULT_WITH_AVAILABILITY_BIT` and **without** the WAIT bit at the
next acquire — a frame whose query is not yet available contributes
`gpu_done_ns = 0` ⇒ its miss (if any) is UNKNOWN. No CPU wait enters the
frame path; M3.5's zero-wait counters remain the regression oracle for
that. If the extension is absent (RADV is expected to have it — verify,
don't assume), GPU_LATE and PRESENTATION_SCHEDULING are simply
unreachable and the stats surface says so explicitly
(`gpu_ts_available: false`) rather than letting an all-UNKNOWN series read
as a mystery. Be prepared for exactly that honest outcome: until calibrated
timestamps are wired and proven, most misses will be UNKNOWN — that is the
point, not a defect.

Verdict counters and the last N verdicts (with their timestamps) are per
output, epoch-tagged, on the stats IPC; AZ_PACE gains a `miss` line per
classified event. The render-late controller keeps its own frame-gap
adaptation unchanged (ADR-600); feeding verdicts into it is a follow-up
that must argue from this data.

**REJECTED ALTERNATIVES.** Inferring GPU lateness from queue-submit-to-
present deltas without GPU timestamps: that is the "GPU too slow" reflex
with more digits. Polling `vkGetSemaphoreCounterValue` timelines for
completion times: a CPU polling loop on the frame path, twice against the
brief. Using the kernel's fence signal timestamps via debugfs: the
DRM-debugfs incident rules that entire approach out on the live machine.

**FALSIFIER.** Strictness: with GPU timestamps force-disabled
(`AZ_AVK_NO_GPU_TS=1`) and a synthetic GPU load inducing misses, the
verdict table must show **zero** GPU_LATE — every such miss lands UNKNOWN.
`AZ_BREAK_PRESENT_GUESS_VERDICT` (classify every miss GPU_LATE, the
pre-M6A reflex codified) must fail that same oracle deterministically.
Positive premise tests, one per verdict, each induced deliberately:
CPU_LATE via an artificial render-path stall (existing skip/stall test
hooks), GPU_LATE via a heavy synthetic pass with timestamps enabled,
each asserting its named evidence appears in the miss line. A verdict
whose premise test cannot induce it stays documented as untestable rather
than trusted.

---

## ADR-610 — Idle stays idle

**CONTEXT.** The audit's "what already holds": frames are requested by
damage and `need_more_frames`, never by a periodic timer. A predictor is a
standing temptation to add "just one" refresh timer, prediction warm-up
tick, or feedback poll. The brief forbids all of it.

**DECISION.** The presenter executes only inside existing edges: `arm` in
the frame event, `committed` after a commit, `present` in the present
event, `reset` in output-state paths. The present event itself only fires
as a consequence of a commit this compositor made, so a settled desktop
delivers zero presenter executions, zero new wakeups, zero timers — the
presenter owns no `wl_event_source` at all, which makes the guarantee
structural: there is no object through which it *could* wake the loop.
Post-idle prediction quality is handled by ADR-605's phase-projection-
across-idle plus its post-idle error bucket — measured, not pre-warmed.

**REJECTED ALTERNATIVE.** A low-frequency "phase maintenance" frame (e.g.
one commit per second to keep feedback flowing): burns power and scanout
bandwidth on 100 % of idle time to improve the first frame of occasional
bursts by, at most, one interval — and the post-idle error bucket will
show whether even that much is being lost.

**FALSIFIER.** `AZ_BREAK_PRESENT_IDLE_WAKE` (presenter registers a 1 s
timer that re-arms prediction and schedules a frame): oracle lets a
headless session settle (existing `needed=0 committed=0` pace signature
marks settle), then asserts zero `render`/`sched`/presenter trace events
across a 10 s window. Healthy build passes; break build shows ~10 events —
deterministic. This doubles as the regression tripwire for any future
"just one timer" patch.

---

## ADR-611 — Semantic animation state vs presentation animation state

**CONTEXT.** The extended brief frames M6A's two halves as one question:
*when* should the frame represent the scene, and *where* is the final
presentation transform evaluated. Answering either requires splitting what
today is one blob — `dwl_animation`'s endpoints, clock and the
integer geometry it writes into shared scene nodes — into the part that is
a compositor decision and the part that is a per-output, per-frame
derivation.

**DECISION.** Two kinds of state, with a one-way boundary:

**Semantic animation state** — CPU-owned, authoritative, exactly one per
animation, mutated only by compositor decisions (map/unmap, tag change,
focus, layout, retarget) and **never per frame, never per output**:
existence and type of the motion; curve identity and motion parameters
(bezier points or spring ζ/ω); start and target geometry (the endpoints);
the semantic start time (`time_started_ns`, stamped at the state change —
a CPU-now instant, correctly so, because the *decision* happens at CPU
time); duration or settle criterion; retarget seeds (ADR-608); output
reach (M4G). Alongside it, everything the extended brief lists stays CPU:
Wayland state, window lifecycle, tag membership, focus, scene ownership,
damage ownership, animation start/end/retarget decisions, output
membership. The compositor remains authoritative; none of this is
expressible on the GPU.

**Presentation animation state** — derived, per output per frame,
immutable for the life of one AVK frame snapshot: `{sample_ns; float
translation (x, y); float scale (sx, sy); float opacity; the float box
they imply}`. Produced during the render pass by evaluating the semantic
trajectory `X(t)` at that output's `sample_ns` (ADR-606), consumed by the
draw (ADR-612) and by damage (ADR-613), then gone. Derivation is strictly
one-way: no presentation value is ever written back into semantic state,
and nothing outside the pass may read it — it lives in the frame snapshot,
so "read after the pass" is unrepresentable rather than forbidden. The one
deliberate exception is per-output *framebuffer history* (the previously
presented box, ADR-613), which is bookkeeping about what was drawn — the
same category as damage itself — not motion state.

The boundary in one line: semantic state answers *what motion is
happening*; presentation state answers *where that motion has reached, on
this output, at this output's photon instant*.

**REJECTED ALTERNATIVES.** Keeping the current shape (per-output passes
mutating shared integer scene geometry as the primary animation state):
G3's incoherence lives exactly there, and it forces the rounding to happen
before the draw, which ADR-614's coherence invariant and M4G's fractional
requirements both indict. Making presentation state persistent and
diffable ("only re-evaluate when progress changed"): an optimisation with
cache-invalidation surface, unjustified by any measurement — evaluation is
a handful of FLOPs per animated window.

**FALSIFIER.** One-way derivation is enforced statically (presentation
state is defined inside the frame-snapshot types; semantic mutation paths
cannot name it) and dynamically by the ADR-607 oracle, which proves every
presentation value is a pure function of (semantic state, `sample_ns`) —
any hidden state feeding the transform would break replay equality.

---

## ADR-612 — CPU evaluates the motion; GPU applies the transform (Model A)

**CONTEXT.** Two candidate splits were mandated for evaluation on merit.
**Model A**: the CPU computes progress from the predicted presentation
time and hands the GPU the final float transform per draw. **Model B**:
the CPU uploads `start_time`, motion parameters and endpoints; the vertex
shader computes the eased transform from a per-frame target-time push
constant. The criteria set: simpler state ownership, lower update churn,
better retarget semantics, equal-or-better performance. The governing
invariant is *sample against presentation time* — not "easing must execute
on the GPU"; moving `lerp(a,b,t)` into a shader is not itself an
objective.

**DECISION. Model A.** During each output's render pass the CPU evaluates
`X(sample_ns)` once per animated window (ADR-611) and the frame snapshot
carries the resulting float transform to the draw — push constants or the
existing per-draw parameter block, whichever AVK already moves per-draw
state in; a handful of floats either way, no new GPU-resident database.
The GPU's ownership is real but narrow, exactly the brief's list:
fractional translation, fractional scale, opacity interpolation between
snapshot values, final vertex placement. Integer coordinates never
quantise the presented geometry (ADR-614, M4G strengthened).

The deciding argument is not aesthetic. **Damage ownership is CPU by hard
constraint, and damage needs the evaluated box every frame** (ADR-613) —
as do blur regions, shadow halos, clipping and input hit-testing. So the
CPU evaluates the trajectory each frame *regardless of model*. Model B
therefore computes everything twice, in two languages, with a permanent
GLSL/C parity obligation (M5's hand-written GLSL twin shows what that
costs: a dedicated compute-shader parity harness to make drift a test
failure), to save uploading a few dozen bytes per animated window per
output per frame — bytes that sit in the noise of a per-draw parameter
stream AVK already submits. Against the criteria: state ownership — A
keeps motion logic in one language, one place; churn — B still pushes a
per-frame target time and re-uploads on every retarget, so its steady-state
saving is the transform floats alone; retarget — A is a pure CPU state
edit (ADR-608), B adds shader-visible segment state and velocity seeds;
performance — the CPU cost A adds is a few FLOPs per animated window
against a measured render budget tracked by `render_dur_ms`, and no
measurement indicts it.

**REJECTED ALTERNATIVES.** Model B (above — rejected on merit, not
dogma; the register records the churn measurement that would reopen it).
A compute-shader animation system: hard NO in the brief, and it is Model
B's problems with a dispatch added. A hybrid (opacity on GPU from
endpoints, geometry on CPU): splits one motion across two owners for no
criterion gain.

**Reopen condition** (so "on merit" stays falsifiable): if instrumented
per-frame snapshot churn attributable to animation transforms ever exceeds
~1 % of frame CPU budget or measurably moves `render_dur_ms`, the decision
is re-argued with those numbers on the table.

**FALSIFIER.** The model itself is falsified by its criteria measurements
(churn bytes and CPU-eval cost land in the presentation stats). Its
correctness rides on ADR-607's oracle (transform = pure function of
semantic state and `sample_ns`) and ADR-614's coherence oracle;
`AZ_BREAK_ANIM_INTEGER_GEOMETRY` (round the per-draw transform to integers
before the GPU sees it) must fail the subpixel oracle of ADR-614
deterministically.

---

## ADR-613 — The damage contract under presentation transforms

**CONTEXT.** Once draws place windows at per-output float positions
evaluated at predicted presentation times, the semantic position and any
output's presented position legitimately differ — by the prediction lead
and by the fractional residual. Damage is CPU-owned and rectangle-based;
it must cover every pixel the transform touches without collapsing to
full-output damage (the known blur amplification, 6.7–10× on moving
blurred windows, punishes over-damage hard).

**DECISION.** Per output, per animated window, per pass: damage is the
pixel-grid expansion (`floor`/`ceil` per edge) of the **union of the
previously presented box and the currently evaluated box**, dilated by the
window's effect halo (shadow radius, blur reach, border — the existing
halo rules, including the damage-ring in-bounds recording rule). The
"previously presented box" is per-output framebuffer history retained
across frames (ADR-611's sanctioned exception), updated only when a pass
actually commits. Semantic geometry is never the damage source for an
animated window — damaging where the window semantically *is* rather than
where it was and will be *drawn on this output* under-damages by exactly
the prediction lead. Full-output damage remains legitimate only for the
events that already use it (whole-output chrome, overview fades); falling
back to it for ordinary window motion is a bug by this contract.
Float-expansion cost is bounded: `ceil` growth is ≤ 1 px per edge over the
integer boxes damaged today.

**REJECTED ALTERNATIVES.** Damaging from semantic integer geometry (the
status quo carried forward): under-damages under lead, and the artifacts
appear only when prediction works — the worst debugging shape. Full-output
damage while any animation runs: measured blur amplification makes this a
regression, and it erases M4G's per-output reach work.

**FALSIFIER.** Under-damage: `AZ_BREAK_PRESENT_STALE_DAMAGE_BOX` (damage
from the semantic box instead of presented∪evaluated) must make the frame
oracle (`AZ_FRAME_ORACLE=1`) name a first divergent frame in an animated
headless run — trailing-edge ghosting is deterministic once the lead is
nonzero. Over-damage: the same fixture asserts per-frame damage area ≤
union-box-plus-halo area (+ε); a full-screen fallback fails the area
bound. Both directions run against the healthy build in the same suite.

---

## ADR-614 — Window coherence: one evaluated box per window per pass

**CONTEXT.** A transformed window is not one rectangle: content, border,
shadow, rounded-corner mask, gradient and blur-related geometry all render
from window geometry. History shows what happens when adjuncts derive
independently: the stale client blur node (output-sized forever), the
shadow hole visible through translucent windows, multi-monitor blur bleed.
Fractional placement multiplies the risk: content moving fractionally over
CPU-rounded shadow/border coordinates is a visible seam that no
full-window test with opaque content catches.

**DECISION.** Invariant: within one render pass, one window has **exactly
one** presentation evaluation — the ADR-611 float transform and its box —
and every adjunct derives from it by pure geometry taking that box as
input. Constructional form: adjunct geometry functions receive the
evaluated box as a parameter; no adjunct path may read the scene node's
integer coordinates during a pass. It must be *unrepresentable* — not
merely reviewed-against — for content to place fractionally while shadow,
border, rounding, gradient or blur geometry sit at independently rounded
coordinates. Corner ownership, clipping and scale are part of the same
single evaluation (this carries into ADR-617's straddle contract).

Fractional geometry requirements folded in (M4G preserved and
strengthened): no integer truncation anywhere in the transform path
(`llround` semantics where quantisation to the pixel grid is genuinely
required, e.g. damage — never in placement); no 200→199→200 oscillation on
settle; no long 1-px stalls at motion tails (the spring dead-tail shape,
ADR-616's settle criterion is the cure on the motion side).

**REJECTED ALTERNATIVES.** Per-adjunct sampling "for accuracy" (shadow at
its own instant): incoherence by design. Rounding the box once and giving
all adjuncts the rounded version: coherent but reintroduces integer
placement, failing M4G and the fractional requirement.

**FALSIFIER.** `AZ_BREAK_ANIM_ADJUNCT_INTEGER` (adjuncts derive from the
rounded box while content uses the float transform): a headless fixture
holds an animated window at a known fractional phase (deterministic via
the trajectory replay) and pixel-samples content edge vs border/shadow
edge; the subpixel offsets must match on the healthy build and diverge
under the break. Fixture traps apply (M4A register: wrong node, wrong
sample point — assert the premise by sampling a knowingly-fractional
phase). Statically: the grep rule of ADR-606 extends to adjunct paths
reading scene-node integer coords mid-pass.

---

## ADR-615 — The colour-domain boundary: transforms move scene-linear pixels

**CONTEXT.** M5 fixed the pipeline: source decode → scene-linear
composition → per-output tone/gamut mapping → SDR/PQ encode → dither →
KMS. M6A inserts the presentation transform and must not smear timing
concerns into colour concerns or vice versa. The specific hazard: applying
a geometric transform to an already-encoded cached representation — a
PQ-encoded intermediate repositioned or rescaled is filtering in a
nonlinear signal domain, which is both visually wrong and a coupling of
animation to one output's encode path.

**DECISION.** The ordering is fixed with the transform's place named:
source decode → scene-linear composition **including the
animation/presentation transform** → per-output tone/gamut → SDR or PQ
encode → dither → KMS. Corollaries, each binding: (1) presentation
transforms are never applied to encoded representations — any cached
intermediate that a transform may move (blur caches PLAIN/DARK included)
must be in the working space (scene-linear BT.709 per ADR-001/002) or be
invalidated for that frame; (2) the domains stay ignorant of each other by
type — semantic and presentation animation state carry no colour fields
(no primaries, no transfer, no luminance), and `az_lum_domain` carries no
timing fields, mirroring C2's "a domain never encodes anything about an
output"; (3) animation is not coupled to Path A/Path B selection — the
transform evaluates identically whichever compose path an output takes,
and a path switch is invisible to motion.

**REJECTED ALTERNATIVES.** Transforming at scanout/encode time ("cheaper —
composite once, move late"): that is precisely the encoded-domain
resampling this ADR exists to forbid, and per-output sampling makes
"composite once" a fiction anyway (each output composes its own instant).
A shared transformed intermediate reused across outputs: dead on arrival —
outputs differ in instant (ADR-607) *and* in colour pipeline (ADR-617).

**FALSIFIER.** `AZ_BREAK_ANIM_TRANSFORM_ENCODED` (apply the transform
after encode on the PQ path): the straddle fixture of ADR-617 asserts that
a window's colour on each output is identical moving and at rest (motion
must never change colour); under the break, the HDR output's moving
samples diverge from its at-rest reference deterministically. Type
separation is enforced statically (the structs cannot name each other's
fields).

---

## ADR-616 — Motion model requirements

**CONTEXT.** The brief: do not preserve the old easing automatically, but
do not tune springs before timing is correct. The project has measured
motion pathology on record: the spring dead tail (frequency 22 finishes in
23 % of its duration, then frozen motion, 1-px jitter, zero-damage
commits) and the M4G truncation jitter.

**DECISION.** Whatever motion model M6A ships must satisfy, as
requirements rather than preferences: **refresh-independent** — `X(t)` is
a pure function of wall-clock time, identical at 48/60/120/144/240 Hz and
across missed frames by construction, which rules out any frame-based
integration; if numeric integration is ever needed it must be
time-delta-based and provably stable, but the strong preference is the
**analytic** closed form (the damped-spring solution in
`animation/common.h` already is one); **interruptible and retargetable**
(ADR-608 semantics); **fractional** (float-valued into ADR-611's
presentation state); **position-continuous** always, velocity-continuous
per ADR-608. No gratuitous overshoot: the default parameterisation is
critically damped or near — the desktop must not be bouncy; overshoot
exists only where a rule explicitly configures it. Completion is a
**settle criterion** for springs — position within threshold *and*
velocity below threshold, both evaluated analytically from `X(t)` so
completion time is itself refresh-independent — replacing the pure
duration horizon that produced the dead tail. Bezier easing keeps
duration-based completion. Old curves are not preserved automatically;
neither are springs tuned in M6A — parameter aesthetics wait until the
timing invariants hold, because tuning motion against a mis-sampled clock
bakes the sampling error into the constants.

**REJECTED ALTERNATIVES.** Per-frame velocity-integration springs (the
common game-loop form): refresh-dependent by construction, forbidden.
Preserving current easing untouched as a requirement: would freeze the
dead-tail defect into M6A's acceptance bar.

**FALSIFIER.** The sampling-grid identity test: evaluate the shipped
`X(t)` on 48 Hz and 240 Hz grids and assert bit-identical values at common
instants — any accumulation hiding in the model fails it; this is the
static twin of `AZ_BREAK_PRESENT_FRAME_STEP` (whose runtime oracle,
ADR-607 statement 3, covers the integrated system). The settle criterion
is falsified by the dead-tail oracle: a spring animation's trace must show
no post-settle tick emitting nonzero damage, and the pre-M6A parameters
that produced the 23 %-then-frozen shape must fail it.

---

## ADR-617 — Mixed HDR/SDR straddle: what is shared, what is legitimate, what is a bug

**CONTEXT.** The live desk is the fixture: one semantic window can span
DP-1 (HDR, VRR, 143.999 Hz max) and HDMI-A-1 (SDR, fixed 60 Hz)
simultaneously. Timing (ADR-607) and colour (M5) each define per-output
divergence; this ADR states their composition so "the window looks
different on the two panels" has a precise legal boundary.

**DECISION.** The window samples at each output's own predicted
presentation time (ADR-607), then runs each output's own colour transform
(M5 per-output tone/gamut/encode). **Final encoded presentation state is
never shared between outputs** — not as pixels, not as a cached
intermediate (ADR-615) — and no future optimisation may reintroduce
sharing without reopening this ADR.

**Legitimate and unavoidable:** temporal skew between the panels (each
shows `X(t)` at its own instant — physically necessary, ADR-607);
different encoded pixel values for the same scene content (SDR vs PQ,
different tone/gamut results); different cadences and different numbers of
intermediate positions.

**A bug, falsifiably:** logical geometry diverging beyond ADR-607's
bounds — the float box on each output must be the *same function* `X(t)`
evaluated at that output's instant, never a per-output fork of the
trajectory; corner-radius ownership differing between outputs (one
rounding geometry, evaluated from the one box, ADR-614); clipping
differing beyond each output's own bounds intersection; scale coherence
broken (the window's scale factor differing between outputs beyond the
outputs' own scale semantics); and colour varying as a function of motion
(ADR-615's oracle).

**REJECTED ALTERNATIVE.** Cross-output reuse of the composed window
("render once at the faster output's instant, reuse on the slower"): wrong
instant for one output by construction *and* wrong colour pipeline —
rejected twice over.

**FALSIFIER.** The straddle fixture (two outputs, one window across the
seam, deterministic trajectory): geometry legality via the ADR-607 oracle
run per output against the same logged semantic state; colour legality via
ADR-615's moving-vs-rest comparison per output; corner/clip coherence by
pixel-sampling the seam edges on both outputs at matching trajectory
phases. `AZ_BREAK_PRESENT_GLOBAL_CLOCK` and `AZ_BREAK_ANIM_TRANSFORM_ENCODED`
each must fail their respective halves; the healthy build passes both in
the same run.

---

## Falsifier register

Every invariant, its break switch, and the oracle that must fail. A break
run in which the named oracle passes is itself a suite failure (project
rule: break tests can stop breaking).

Naming: the operator's two proposed families overlap; where a
`BREAK_ANIM_*` name and a `BREAK_PRESENT_*` name test the same invariant
there is **one** switch — two switches for one invariant is how a green
duplicate hides a dead break. The mapping: `BREAK_ANIM_SAMPLE_CPU_NOW` ≡
`AZ_BREAK_PRESENT_SAMPLE_NOW` (I1); `BREAK_ANIM_GLOBAL_OUTPUT_TIME` ≡
`AZ_BREAK_PRESENT_GLOBAL_CLOCK` (I2); `BREAK_ANIM_FRAME_STEP` ≡
`AZ_BREAK_PRESENT_FRAME_STEP` (I4); `BREAK_ANIM_RETARGET_VELOCITY_RESET` ≡
`AZ_BREAK_PRESENT_RETARGET_ZERO_VELOCITY` (I6).

| # | invariant | break switch | oracle (fixture) |
|---|---|---|---|
| I1 | `sample_ns == target_ns`, once per pass | `AZ_BREAK_PRESENT_SAMPLE_NOW` | trace consistency: every tick's `sample_ns` equals the pass's armed target (headless, ADR-606) |
| I2 | targets are per-output, own presenter only | `AZ_BREAK_PRESENT_GLOBAL_CLOCK` | ADR-607 statement 2 on the 2-output fixture |
| I3 | feedback corrects prediction | `AZ_BREAK_PRESENT_STALE_FEEDBACK` | `sync == SYNCED` and non-empty error series after N frames (ADR-603) |
| I4 | wall-clock progress, refresh-independent | `AZ_BREAK_PRESENT_FRAME_STEP` | ADR-607 statement 3: equal completion wall time across refreshes |
| I5 | idle: zero presenter/render activity when settled | `AZ_BREAK_PRESENT_IDLE_WAKE` | zero trace events in a 10 s settled window (ADR-610) |
| I6 | retarget position continuity at presentation; spring velocity continuity once landed | `AZ_BREAK_PRESENT_RETARGET_ZERO_VELOCITY` (velocity); I1's break also trips the position check | ADR-608 boundary conditions on the tick series |
| I7 | no timing state survives an output state change | `AZ_BREAK_PRESENT_NO_EPOCH_RESET` | epoch bump + new interval on the frame after a headless refresh change (ADR-604) |
| I8 | verdicts require their evidence; else UNKNOWN | `AZ_BREAK_PRESENT_GUESS_VERDICT`; also `AZ_AVK_NO_GPU_TS=1` + load | zero GPU_LATE without GPU timestamps (ADR-609) |
| I9 | M3.5 zero-CPU-wait survives | (none new — existing counters) | `cpu_sync_waits == 0`, `presentation_waits == 0` in the existing suite |
| I10 | no clock reads in animation sampling code | (static) | build-time grep over `src/animation/*.h`, `src/draw/ufo-node.c` (ADR-606) |
| I11 | presented geometry is fractional; integers never quantise placement | `AZ_BREAK_ANIM_INTEGER_GEOMETRY` | ADR-614 subpixel oracle (content edge at a known fractional phase) |
| I12 | retarget seeds from the evaluated position, not an endpoint | `AZ_BREAK_ANIM_RETARGET_POSITION_RESET` | ADR-608 boundary condition on the tick series |
| I13 | all window adjuncts derive from the one evaluated box | `AZ_BREAK_ANIM_ADJUNCT_INTEGER` | ADR-614 content-vs-border/shadow subpixel offset match |
| I14 | damage covers presented ∪ evaluated boxes; never semantic-only, never full-screen fallback | `AZ_BREAK_PRESENT_STALE_DAMAGE_BOX` | frame-oracle first-divergence + damage-area upper bound (ADR-613) |
| I15 | presentation transforms move scene-linear pixels only | `AZ_BREAK_ANIM_TRANSFORM_ENCODED` | moving-vs-rest colour equality per output on the straddle fixture (ADR-615/617) |
| I16 | semantic → presentation derivation is one-way and pure | (static + replay) | frame-snapshot type containment; ADR-607 replay equality (ADR-611) |
| I17 | motion model is analytic / refresh-independent in form | (static twin of I4) | 48 Hz vs 240 Hz sampling-grid identity unit test; spring settle dead-tail oracle (ADR-616) |

Foreign-clock handling (ADR-603) is covered by a unit premise test rather
than a break switch: it needs a synthetic event, not a runtime toggle.
Model A's criteria (ADR-612) are covered by measurement M-7 rather than a
break: a decision on merit is falsified by numbers, not by a toggle.

## Measured facts and open measurements

**Measured during drafting (rely on these; the proofs live in the code and
the presentation IPC):**

- `wlr_output_event_present.when` is CLOCK_MONOTONIC on both DRM outputs
  (skew to monotonic: DP-1 −56.089 ms, HDMI-A-1 −7.818 ms; skew to
  realtime: ~−1.8×10¹⁵ ms — nine orders of magnitude of margin). The
  ADR-603 gate stays as the guard for any future backend; the probe
  reports "neither" → FOREIGN.
- Presentation feedback is wired unconditionally; `amsg get presentation`
  reports per output: presented/dropped/no_stamp, last_seq,
  nominal_refresh_mhz, observed_interval_us, interval_rejected, cadence
  x1/x2/x3plus, stamp_clock, both skews. G1 is closed.
- Display period must come from `Δwhen / Δseq` (vblank-sequence delta),
  never presented-frame gaps: the gap method read DP-1 at 7673 µs vs the
  6944 µs mode on a damage-driven compositor.
- DP-1 nominal refresh is 143 999 mHz (143.999 Hz); period is
  `1e12 / refresh_mHz` ns — a 1000× units error here shipped once and the
  instrument caught it.
- **DP-1 runs with VRR enabled** (`vrr_capable`, `vrr_enabled` true;
  HDMI-A-1 neither). Observed period: 11 916.8 µs idle, 7 475.4 µs under
  continuous damage, trending to the 6 944.5 µs floor; cadence x1=3384,
  x2=35, x3plus=104 — genuinely stretched vblanks, not skips. HDMI-A-1:
  16 667.3 µs observed vs 16 666.7 nominal (0.6 µs). Basis of ADR-605's
  regime split.

**Measurements requested from Opus** (record results in this directory;
M-8 gates a constant, nothing gates the build):

- **M-1** Headless backend present-event behaviour: fires at all?
  `presented`? `when`? (ADR-603 fixtures; the design needs nothing from
  it — a silent backend leaves the presenter honestly UNSYNCED.)
- **M-2** Whether the headless backend paces frame events from custom-mode
  refresh (ADR-604/607 fixtures; if not, mixed-refresh statement 3 moves
  to the live layer).
- **M-3** `VK_EXT_calibrated_timestamps` present and
  CLOCK_MONOTONIC-capable on RADV here (ADR-609): `vulkaninfo` first, then
  the in-frame query. Until proven, GPU_LATE is unreachable and the stats
  must say `gpu_ts_available: false`.
- **M-4** Post-idle phase continuity on the FIXED output (HDMI-A-1): the
  ADR-605 post-idle error bucket answers it; a rephasing panel shows a
  bimodal distribution.
- **M-5** `ev->refresh` population on this backend (cross-check series
  only; never a predictor input).
- **M-6** Signed mean of the FIXED-regime error series ≈ 0 (lattice point
  chosen correctly) vs ≈ −1 period (off-by-one flip → apply the one-line
  `+P` correction with the evidence in hand).
- **M-7** Per-frame snapshot churn and CPU eval cost attributable to
  animation transforms (ADR-612's reopen condition needs its
  instrumentation to exist, or "on merit" is rhetoric).
- **M-8** — **wanted before the VRR `t_pipe_ns` constant is seeded, and
  the answer to "which measurements do you want": this one first.**
  Commit-to-photons latency on DP-1 (KMS commit point → presentation
  stamp, same proven clock), measured separately for (a) continuous-damage
  steady state and (b) the first commit after an idle gap (the LFC
  interaction is the unknown that matters on the live desk). The ADR-605
  VRR rule is parametric on this constant, so implementation proceeds with
  the documented placeholder meanwhile.
- **M-9** The panel's actual VRR range (min/max refresh from DRM) and
  whether the 11.9 ms idle cadence is LFC doubling. Interprets M-8(b) and
  the PRESENTATION_SCHEDULING population; not blocking.
- **M-10** Whether wlroots/asteroidz can enter/leave VRR without an output
  commit this compositor makes (`is_vrr_opening` and the tearing path
  transitions). Validates ADR-604's regime-per-epoch claim; if violated,
  the reset-trigger list grows before the claim is trusted.
