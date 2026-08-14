# M6A.1 — the presentation path as it exists at `cc59538`

Read from HEAD, not from M3.5/M4 assumptions. Every claim below names the file
and line it came from.

## The stages, in order

| # | stage | owner | context | time source | per-output? | can block? |
|---|---|---|---|---|---|---|
| 1 | damage / animation request | compositor (`request_fresh_for_box`, `request_fresh_all_monitors`) | main | none | yes (M4G reach box) | no |
| 2 | `wlr_output.events.frame` → `rendermon` | wlroots DRM backend | main | ~vblank-aligned event | yes | no |
| 3 | render-late deferral | `m->render_timer` | main (event loop timer) | `CLOCK_MONOTONIC` (`asteroidz.c:8515`) | yes | no |
| 4 | **animation sampling** | each animated object | main, inside the scene walk | **its own** `CLOCK_MONOTONIC` read | no — global wall clock | no |
| 5 | AVK snapshot + record | `az_record_compose` | main | `render_t0` (`asteroidz.c:8130`) | yes | no |
| 6 | GPU submit | AVK | main | timeline semaphore | yes | **no** (M3.5) |
| 7 | KMS commit | `wlr_output_commit_state` | main | syncobj timeline | yes | no |
| 8 | frame-done to clients | `wlr_scene_output_send_frame_done` | main | fresh `CLOCK_MONOTONIC` (`asteroidz.c:8425`) | yes | no |
| 9 | **presentation feedback** | `events.present` → `pacepresent` | main | `ev->when` (page-flip) | yes | no |

## The five gaps

### G1 — there is no production presentation feedback

`asteroidz.c:5687`:

```c
/* Only wired when the operator asked for the trace: nothing else in the
 * compositor needs presentation feedback ... */
if (az_pace_on())
    LISTEN(&wlr_output->events.present, &m->pace_present, pacepresent);
```

Outside `AZ_PACE=1` the compositor **never learns when a frame reached the
screen**. `wlr_presentation_create()` (`asteroidz.c:9708`) exists, so *clients*
receive presentation feedback that the compositor itself discards.

The loop M6A is defined by — predict, present, correct — has no input today.
This is the first thing that must exist, and it is not a redesign: it is
promoting an existing listener out of a debug gate.

### G2 — animation is sampled at "now", not at a target presentation time

`src/animation/client.h:2323`:

```c
clock_gettime(CLOCK_MONOTONIC, &now);
uint64_t now_ns = ...;
double passed_ms = (now_ns - c->animation.time_started_ns) / 1.0e6;
```

`now` is the instant the CPU walked this node. Everything downstream — record,
submit, commit, page flip — happens *after* it, so the frame displays a state
that was already stale when it was computed. At 144 Hz the whole budget is
6.944 ms and that lag is a meaningful fraction of one frame.

### G3 — there is no single per-frame sample instant

Every animated object calls `clock_gettime` **for itself**:
`animation/client.h:2324`, `client.h:2510`, `animation/layer.h:370`,
`layer.h:438`, `draw/ufo-node.c:98`. Two windows animating in one frame sample
at different instants, separated by however long the walk between them took.

This is invisible on a still desktop and cannot be fixed by choosing a better
*target* — a target must be computed once per output per frame and passed down,
or the coherence problem simply moves.

### G4 — a "miss" is inferred from frame-event spacing, not from presentation

`asteroidz.c:8522`:

```c
uint64_t gap = now_ns - m->render_late_last_ns;
if (gap > interval_ns * 3)              { /* idle, ignore */ }
else if (gap > interval_ns + interval_ns / 2) { /* missed a vblank */ }
```

The adaptation is real and it works, but its evidence is the arrival pattern of
*frame events*. It cannot separate CPU-late from GPU-late from commit-late, and
it cannot see a frame that was rendered inside budget and still missed its
vblank because it arrived after the flip deadline. That distinction is
explicitly in M6A's scope.

### G5 — refresh is nominal

Period comes from `m->wlr_output->refresh` (mHz), e.g. `1.0e6 / refresh` ms.
DP-1 reports 143.999 Hz, not 144. Any predictor built on the nominal figure
accumulates phase error against the observed series, and the harness already
records nominal-vs-observed Hz as a fixture trap.

## Clock domains

Every compositor-side read is `CLOCK_MONOTONIC` — seven independent sites, no
conversions, no mixing. `wlr_output_event_present.when` is the page-flip
timestamp and is `CLOCK_MONOTONIC` on the DRM backend, but that is an
assumption inherited from wlroots and **must be proven rather than believed**
before a predictor subtracts one from the other; the backend may report
`CLOCK_REALTIME` where the driver lacks monotonic page-flip timestamps.

`ev->presented` is already honoured (`asteroidz.c:8481`): a dropped update
fires the signal too, and folding one into an interval series invents a refresh
that never happened.

## G6 — the session guards are dead code, and one reset trigger rests on them

`static struct wlr_session *session;` (`asteroidz.c:1690`) is declared and
**never assigned** — nothing in the tree calls `wlr_backend_get_session()`. So
it is permanently NULL, and every guard spelled `session && !session->active`
(rendermon's early return, the render-late eligibility test) is unreachable:
the condition is always false and the permissive branch always taken.

That is harmless today — the defaults are the permissive ones — but ADR-604's
reset trigger 4 includes "session active transitions (VT switch back)", and
that half **cannot be wired**: there is no session object to listen to. It is
recorded here rather than implemented, because adding session tracking is a
compositor-lifecycle change and not presentation work.

The DPMS half of trigger 4 *is* wired, in both directions.

Consequence to keep in mind: after a VT switch back, an output's presenter may
still hold a `last_present_ns` from before the switch. The FIXED lattice
re-anchors on the first accepted present, so the damage is bounded to one
frame's prediction; it is not a wedged state. Worth fixing when session
tracking exists, not worth inventing a session for.

## What already holds and must not regress

- **No CPU waits.** M3.5's explicit GPU→KMS sync via syncobj timelines. The
  live counters read `cpu_sync_waits=0`, `presentation_waits=0`.
- **Wall-clock animation.** Progress is elapsed real time over a duration, not
  a frame counter, so it is already refresh-independent. Preserve this; a
  target-time model refines *which* instant is sampled, it does not reintroduce
  frame stepping.
- **Idle convergence.** Frames are requested by damage and by
  `need_more_frames`, never by a periodic timer. A predictor must not add a
  wakeup with nothing to display.
- **Per-output frame reach.** `request_fresh_for_box` already limits which
  outputs a motion wakes (M4G).
