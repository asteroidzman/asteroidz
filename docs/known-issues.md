# Known issues

Open defects with what has been established and, as importantly, what has been
*ruled out*. The point of this file is that the next attempt starts where the
last one stopped instead of re-deriving it.

## Tag-in animation sometimes does not run

**Symptom (operator, 2026-08-17).** Switching tags — 2 → 1 on DP-1 — sometimes
animates and sometimes does not. When it does not, kitty windows appear
"squished against the edge of the screen". Intermittent; no known trigger.

**Context.** DP-1 tag 1 is monocle, tag 2 is scroller, and the kitty windows
live on the scroller tag. A scroller legitimately parks windows partly off-strip
at negative x, so "squished against the edge" is very likely the *normal*
appearance of a scrolled window — seen abruptly because the slide-in did not
play, rather than damage in its own right.

**Most plausible mechanism, NOT demonstrated.** `set_tagin_animation()`
(src/animation/tag.h) early-returns when `c->animation.running` is already true,
leaving the window with no slide. The configured spring (`damping 0.8`,
`frequency 10`) converges at ~40% of its duration but the animation stays marked
running for longer, so a tag switch arriving inside that window would silently
skip the slide. This fits the intermittency. It has not been proven.

### Ruled out — do not re-check these

1. **Character-cell quantisation.** kitty's buffer is pixel-exact for its box:
   1796x2031 for a 1201x1358 logical box at scale 1.5 with a 2px border.
2. **The animation start position.** Two fixes were written against
   `set_tagin_animation()`'s off-screen `animainit_geom` — placing hidden
   windows at their current position, then at their layout position. Neither
   changed the outcome. Both were reverted.
3. **A corrupted `c->geom`.** `resize()` was instrumented to log any call with
   `geo.x < -100`. Across the full reproduction attempt it never fired once, so
   nothing is writing an off-screen geometry through that path.
4. **The first "reproduction" was invalid and must not be reused.** It put the
   test windows on the SCROLLER tag and found one at x=-1480, which is a scroller
   parking a window off-strip — correct behaviour reported as a bug. Repeating it
   with the windows on the monocle tag shows both correctly at x=10 after six
   round-trips, with zero off-screen writes.

### What a next attempt should do

Reproduce against the layout the operator actually uses: a **scroller** tag with
several windows, switched away from and back to, asking whether the slide RAN —
not where the windows ended up. Geometry is the wrong instrument here, because
the resting positions are legitimately off-screen; the question is whether the
animation played at all.

## Spring duration is coupled to spring frequency

Not a defect, and documented in `contrib/anim-pace-test.sh`'s header, but it
surprises people and it is what "the animation falls short" turned out to mean.

The spring is evaluated over NORMALISED time, so `frequency` says how many
radians it travels per configured duration, not per second. Measured with
`frequency 10`: every MOVE animation configured for 500ms completed in ~202ms
(`closed=completed`, `dead_tail` ~0) — the spring converges at t≈0.4 and stops.
An earlier measurement at `frequency 22` finished in 23% of its duration.

`duration` is therefore an upper bound the spring may never reach. The lever is
`frequency`; roughly 4-5 keeps it in motion for most of the configured time.
