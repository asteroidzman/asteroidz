# Headless regression test harness

`contrib/regression/` is a data-driven, assertion-based test suite for
asteroidz's window-management/IPC dispatch surface — the counterpart to
[`contrib/anim-test.sh`](./anim-testing.md), which is for *visual* rendering
regressions instead. Where the anim harness produces a montage/recording for
a human to inspect, this one boots one isolated headless compositor instance
and runs TAP-style pass/fail assertions against it via `amsg`, so it can gate
a change automatically.

## Usage

```sh
bash contrib/regression/run.sh              # every module
bash contrib/regression/run.sh layouts tags  # just these modules
```

There is a second, much faster layer beside it: `meson test -C build` runs unit
tests for the pure helpers — the ones whose failure mode is not "the screen
looks wrong" but a corrupted file on disk or a reply that arrives with its tail
missing. Those cannot be reached by driving a compositor, because the bug is
invisible at the sizes and shapes a running compositor produces:

| Target | Covers |
| :--- | :--- |
| `kdl-edit` | rewriting an `output NAME { … }` block without disturbing the comments around it |
| `kdl-write` | editing an arbitrary nested path; includes a 500-edit corpus run over the shipped `assets/config.kdl` |
| `ipc-out` | queued socket writes, against a socketpair with `SO_SNDBUF` shrunk to force the partial write a normal reply never triggers |
| `config-schema` | `asteroidz -S`: every schema entry's default, clamp, offset and type, against the real parser |
| `config-schema-coverage` | the other direction — keys `parse_option` handles that the schema is missing |
| `dispatch-actions-coverage` | the same both-ways check for the dispatch-action table a keybind editor reads |
| `rule-schema-coverage` | window-rule keys `parse_option` handles that `rule_schema[]` is missing — the direction `-S` structurally cannot see |
| `shipped-config` | `assets/config.kdl` parses. It becomes `/etc/asteroidz/config.kdl` and is what every new user copies; nothing was checking it, and it had drifted to the point where `asteroidz -p` exited 1 |
| `bar-icons` | every vendored SVG parses and rasterises to non-empty ink |

Three of them are not "pure helper" tests at all — `avk-core`, `avk-render` and
`avk-gradient` drive a real GPU, render into an image and read the pixels back.
They live here because what they assert is **numerical**. A headless compositor
screenshot can say a window has a ramp across it; only a readback can say the
ramp has five bands and not four segments, that band three begins at 0.4 rather
than 0.375, or that a seventeen-stop gradient did not silently truncate at
eight. Each skips (exit 77) with no DRM render node.

Run both before pushing. Neither subsumes the other.

There is a third, one-purpose script beside them: `contrib/signal-exit-test.sh`
asserts that SIGTERM and SIGINT each exit the compositor within five seconds. It
cannot live in `run.sh`, because every module there shares one compositor and
this one's whole job is to kill it. It exists because `handlesig()` once called
`quit()` — the *asking* one — so a signal raised the exit-confirmation prompt and
then waited for a keystroke: logout stalled until systemd's SIGKILL, and every
regression module hung after printing its summary and leaked its compositor.
`hl_stop` now bounds its own wait and says so loudly rather than hanging.

`contrib/titlebar-sharpness-test.sh` is the other one-purpose script. It asserts
that a titlebar is *rasterised* at its output's scale rather than drawn at
logical size and resampled up, which no IPC assertion can see — the geometry is
identical either way and only the pixels differ. It measures soft ramps per
steep edge across the titlebar band: text rendered at the panel's own
resolution goes background-to-ink in one step and scores about 0.8, while the
same text resampled up arrives as a ramp across two or three pixels and scores
about 16. Counting mid-tones instead does not separate the two — the background,
borders and icon supply plenty either way, and the first version of this
measured 22.2 against 20.5 and could not tell the builds apart.

`contrib/blur-exclusion-test.sh` is the third. It asserts that no pixel of a
window survives anywhere in its own shadow's backdrop-blur source — the scratch
image the blur samples, which covers the window because a shadow is the window
plus its spread. It reads that image directly, through the `FX_BLUR_DUMP`
facility (see [effects](./visuals/effects.md#dumping-a-blurs-source)), instead of
looking for the consequence on screen, because whether the consequence is
*visible* depends on the window's size, its colour against the backdrop and the
blur's reach. The two shadow scenes in `contrib/regression/tests/effects.sh`
happen to sit where it is not: both passed on a build whose fill left 7575 of
30800 hole pixels holding the window's own colour. Vulkan only — the GLES path
patches the hole in a shader, with no equivalent image to read back.

It also has to run its own compositor, because the dump is armed from the
environment at startup and every module in `run.sh` shares one instance. (It can
be armed at runtime too — `amsg dispatch dump_blur_source` — but that only helps
a compositor that already has the dispatch, which is to say not the one you are
trying to diagnose after a fix.)

`contrib/shadow-darken-test.sh` is the fourth, and it exists because of what the
other three could not see. A blur is an average, and averaging bright detail
over a dark ground raises the mean where the ground is dark — so a shadow's
blurred backdrop over a terminal comes out *lighter* than the backdrop it
replaced, and reads as a halo brightening toward the window. Every shadow scene
in `effects.sh` uses flat single-colour windows on a flat wallpaper, and a blur
of a flat field is the same flat field: there is no high-frequency detail for
the average to redistribute, so all three of them pass on a build with the bug,
including the one actually named `a shadow over a dark window only ever darkens
it`. This one uses fine bright lines on black instead — the structure of text
without needing a terminal to produce it — and asserts the plain rule the
feature is named for: nothing under the shadow may end up brighter than the same
wallpaper well away from it. 60 levels of stray light before the clamp, 0 after.

The lesson is worth more than the test. A scene built to be *easy to measure* —
flat colours, no texture — can be systematically blind to a whole class of
fault, and it will keep passing while looking like real coverage.

`contrib/shadow-exclude-clamp-test.sh` is the fifth, and it covers the clamp's
own blind spot: the clamp takes a minimum against the unblurred source, and
inside the window box the shadow excluded there is no unblurred source, only the
synthetic fill the exclusion left behind. Clamping against a fabrication drags
the region toward the fabrication's darkest structure, which is invisible behind
an opaque window and a dark window-shaped patch through a translucent one — so
the scene here puts a 15%-opaque terminal over the fine-lines wallpaper.

Its shape is different from the other four: the assertion is about *two*
renderings of one scene, which no single capture can make, so the script
re-invokes itself and runs the compositor twice — once normally, once with
`FX_BLUR_NO_DARKEN_CLAMP=1`. Inside the hole the two captures must agree; below
the window they must not, and that second check is not decoration. Run against a
binary predating the environment hook, the clamp stayed on for both captures,
the two images were identical inside the hole, and the real assertion passed for
entirely the wrong reason. A test whose premise can silently stop holding needs
that premise asserted next to it.


`contrib/shadow-hole-visible-test.sh` is the sixth, and it covers what all five
of the others share: they use opaque windows. A shadow's backdrop blur fills the
excluded window box with a fabrication — a stretched strip, a mirrored band —
and behind an opaque window that is unobservable by construction. Through a
terminal at 0.98 opacity it is two percent of every pixel, which is how it was
found: on a real desktop, not here.

It renders one scene twice, with `shadows_blur_background` on and off, and
asserts that inside the window the two are identical (the blur must contribute
nothing there) while the shadow band outside them differs (which proves the
feature was running). Measured: 70.0 levels of change inside the window before
the composite was clipped, 0.011 after, with the band at 4.56 in both.

Two earlier versions of this measurement were wrong in instructive ways.
Correlating what shows through the window against the bare wallpaper is
confounded — a translucent window has its *own* backdrop blur, so what shows
through is a blurred wallpaper and does not track a sharp one; that scored 0.76
on a correct build against 0.57 on a broken one. And the first wallpaper put its
detail in the centre and left the shadow band over a flat field, where a blur
returns the field unchanged and nothing can be detected at all — the premise
check caught it and refused to certify the scene.


`contrib/shadow-tiled-neighbour-test.sh` is the seventh, and it is the first to
put two windows on screen at once. Every other shadow scene here renders a
single floating window, and a shadow that spills onto a neighbour needs a
neighbour to spill onto. Reported as "the blended shadow currently will clip
through and affect adjacent windows".

Two tiled windows with a 26px gap and a 60px shadow, rendered with shadows on
and off, asserting that nothing changes *inside* either window. A window's own
shadow is already clipped out of its own box, so anything that moves there came
from the other window's shadow being drawn over it.

The first version of this measurement passed against the build that had the
bug, and the reason is worth keeping. It averaged over each window's whole
interior — and the fault is a 40px band along one edge of a 921px-wide window,
so a 15-level darkening that is obvious to look at arrived as **0.11 levels**
once spread across the rest. Narrowing the sample to the strip facing the
neighbour put it at 4.35 before the fix and 0.003 after, with the gap between
the windows measuring an identical 25.35 in both: the shadow is exactly as
visible where it belongs, and gone where it is not.

The scene is also the first to need `shadows_blur_background 1` and
`blur_optimized 0` to reproduce. With the optimized path a tiled window's shadow
blur draws the cached wallpaper rather than sampling live, so it never picks the
neighbour up — the same fault, invisible to a test that leaves the default on.

`contrib/avk-frame-test.sh` is the eighth, and the only one that compares two
*renderers* rather than two settings. It boots a real compositor twice with the
same config and the same clients — once on SceneFX, once on asteroidz's own
Vulkan engine (`ASTEROIDZ_RENDERER=avk`) — and asserts the frames agree.

```bash
ASTEROIDZ=build-vk/asteroidz bash contrib/avk-frame-test.sh
BREAK=border ASTEROIDZ=build-vk/asteroidz bash contrib/avk-frame-test.sh  # must FAIL
```

The critical detail is that it leaves `WLR_RENDERER=gles2`. If the two switches
were coupled the test would be comparing the Vulkan renderer against itself;
composited by Vulkan *while* wlroots holds GLES2 is the entire claim.

The assertions are on named colours at named places, because the obvious weaker
version — "AVK produced a frame" — passed on the build where every window
rendered as a flat block of its own border colour. `BREAK=border` puts that bug
back and the run must fail; it is the only break switch here that breaks
anything, and the header comment says why the other one does not.

One difference is deliberately not asserted equal: effects, which the scene
config turns off because they are M4. The wallpaper used to be the other one and
is now asserted to the exact pixel — see
[`docs/vulkan-native-architecture.md`](./vulkan-native-architecture.md) §5.4c
for why it was missing and what fixed it.

`contrib/avk-sync-test.sh` is the ninth, and it asserts on nothing you can see.

```bash
ASTEROIDZ=build-vk/asteroidz bash contrib/avk-sync-test.sh
BREAK=presentsync ASTEROIDZ=build-vk/asteroidz bash contrib/avk-sync-test.sh  # must FAIL
```

The question it answers is whether a finished frame reaches the display with a
fence attached, and it is counters rather than pixels because an unsynchronised
frame *looks correct nearly always* — which is exactly why the missing fence
survived a whole milestone. `present_sync_timeline + present_sync_dmabuf` must
equal `frames`, and `present_sync_none` must be zero.

Its limitation is stated in its own header and worth repeating: **the headless
backend has no DRM device, so this test can only exercise the dma-buf route.**
The drm_syncobj timeline route — the one a real monitor takes — is covered at
the primitive level by `test-avk-core`'s round trip and at the compositor level
by nothing but a person reading `amsg get avk-stats` on a display.

`contrib/avk-damage-test.sh` is the tenth, and it is another two-run comparison
— but of the same renderer against itself with damage tracking turned off.

```bash
ASTEROIDZ=build-vk/asteroidz bash contrib/avk-damage-test.sh
BREAK=preserve ASTEROIDZ=build-vk/asteroidz bash contrib/avk-damage-test.sh  # must FAIL
BREAK=stale    ASTEROIDZ=build-vk/asteroidz bash contrib/avk-damage-test.sh  # must FAIL
```

`AZ_AVK_FULL_DAMAGE=1` produces the reference, because a frame that redraws
everything cannot be wrong. The assertions are on regions that never change —
wallpaper, decorations, window interiors — which is exactly the part of the
buffer partial damage never touches.

Two premises are checked first, and neither is optional. The partial run has to
have genuinely redrawn less, or the two runs agree by construction; and the
frames the *screenshot* came from have to have been partial redraws, which the
test establishes by resetting the counters immediately before capturing. A
capture taken just after a full redraw looks perfect however broken preservation
is. `BREAK=stale` runs both passes with full damage and must fail on exactly
those premise assertions — it breaks the test rather than the code, which is the
point.

The first version of `BREAK=preserve` used `loadOp DONT_CARE` and **the whole
suite passed with it set**: a driver may leave "undefined" contents alone, and
RADV does. It clears to magenta now.

### Cross-output decoration (M4B.1)

`contrib/avk-crossoutput-border-test.sh` runs two adjacent headless outputs and
places a floating window across the seam so its **right outer border lands on
the second output**. That assertion is the point: everything else about the bug
is arguable as a clipping preference; a window whose actual outer edge is not
drawn is not.

```bash
bash contrib/avk-crossoutput-border-test.sh
ENGINE=gles bash contrib/avk-crossoutput-border-test.sh
BREAK=border-owner-monitor-clip bash contrib/avk-crossoutput-border-test.sh  # must FAIL
```

Eighteen assertions across four groups: both outputs' geometry, cross-output
damage counters, drag/release invariance, and scroller preservation. Three
things it learned the hard way:

- **The border palette is fixed, not sampled.** Reading the reference off
  output 1 made the whole probe abort under the break — which removes the
  border from exactly that output — so every assertion failed together and none
  of them said which. With a fixed palette the break's signature is readable:
  `o1_client_to_seam 412` with `o1_left_border 0`, client present, border gone.
- **Releasing a drag does not recompute the decoration.** The drag/release pair
  passed against the broken build until a same-geometry re-layout nudge was
  added after the release. `apply_border()` has to run again for the policy to
  bite; the button coming up is not itself an event.
- **Which output owns the scroller row is not assumed.** It lands wherever
  focus left it and overflows whichever way the layout scrolls; assuming output
  1 owned it failed against a *correct* build, with the row on output 2 and
  output 1 legitimately clean.

`BREAK=border-owner-monitor-clip` restores the old policy and fails four
assertions — the far output's outer border, seam continuity, seam rounding, and
drag/release invariance. **GLES fails identically against it**, which is how we
know the defect was compositor-side and shared rather than an AVK rendering
fault.

#### M4B.1 live acceptance — PASS, by observation

Super+drag of a **server-decorated** window across the DP-1/HDMI-A-1 seam,
paused straddling, released straddling, on `e049d1d`. Confirmed by watching
both screens: the border followed the client onto the far output, nothing
changed on release, no stretch of client without border, no duplication, no
rounding at the seam.

**This is a visual acceptance, not a measurement.** No trustworthy automated
live pixel result was obtained, and four attempts failed for reasons worth
recording because each would recur:

- **The first run measured a CSD window.** Dolphin — and KDE apps generally —
  draw their own decorations, so `check_hit_no_border()` gives them *no server
  border at all*. It reads as "border missing on the far output" everywhere,
  including fully on the owning monitor. A live decoration test must assert its
  window has a server border before concluding anything. Corollary worth
  knowing: this defect was invisible to anyone using mostly CSD applications.
- **Whole-screen colour scans do not work on a real desktop.** Dark content
  matches a dark client fill (the fill bbox came back as the entire screen) and
  light content matches a light border (31,624 false border pixels). The
  headless fixture gets away with it because it has a flat wallpaper and one
  window.
- **Mid-drag IPC geometry lags the render**, so coordinate-derived probes land
  off the window and report wallpaper as the border reference.
- **`move_window` and `focus_id` act on the FOCUSED client.** Dispatches meant
  for the test window went to a terminal instead, and the "border" found at
  that position belonged to it.

The deterministic evidence is `contrib/avk-crossoutput-border-test.sh`, which
is unaffected by any of the above.

### rounded-bottom-swap: TESTED LIVE — PASS

The one M4A break that no headless fixture can reach, closed on the real
desktop 2026-08-11. A bottom-corner swap is only visible when `BR != BL`, and
the only thing that makes them differ is the edge rule squaring one side —
which needs a floating window overhanging a monitor edge by at least the
radius. Tiling never produces that state.

```text
producer   floating window over DP-1's RIGHT edge
geometry   x=1700  width=900  radius=9   DP-1 logical right edge 2560
condition  x + width - radius >= right edge
           1700 + 900 - 9 = 2591 >= 2560          -> RIGHT corners squared
producer state   TR = 0    BR = 0    TL = r    BL = r
observable partner   BL
measured         TL ~8 logical px     BL ~8 logical px
square control   a fullscreen window's four corners measured 0
result           BL is rounded, therefore BR/BL are NOT swapped
```

**Limitation, recorded rather than glossed.** When the edge rule fires, the
squared BR arc is *necessarily* off-output: the rule only triggers once the
window overhangs by at least the radius, which puts that arc entirely past the
screen boundary. So the live proof observes the **rounded partner** and relies
on the producer's measured geometry and state for what BR must be.

`BREAK=rounded-bottom-swap` was **not itself run live** — no broken-build
falsifier was executed on the desktop, and none is claimed. The deterministic
falsifier for bottom-corner ordering remains the renderer-level `0/7/19/37`
per-corner test, which does run against a broken build.

`contrib/avk-rounded-persist-test.sh` asks a different question about damage:
not "did the renderer redraw enough of this frame" but "does a region the
window does not cover keep being redrawn at all". A rounded corner is
transparent, the background behind it is live, and a corner excluded from
damage shows the previous frame forever while everything around it moves on.
That is what a user reports as flicker.

```bash
cd contrib/wlrepaint && make          # once
ASTEROIDZ=build/asteroidz bash contrib/avk-rounded-persist-test.sh
BREAK=damage-hole ASTEROIDZ=build/asteroidz bash contrib/avk-rounded-persist-test.sh  # must FAIL
ENGINE=gles       ASTEROIDZ=build/asteroidz bash contrib/avk-rounded-persist-test.sh
BORDER=6          ASTEROIDZ=build/asteroidz bash contrib/avk-rounded-persist-test.sh
BREAK=border-square-inner BORDER=6 ASTEROIDZ=build/asteroidz bash contrib/avk-rounded-persist-test.sh  # must FAIL
```

`contrib/avk-border-test.sh` (M4B) is the border's own suite: seventeen
configurations of radius, border width, output scale, transform, opacity and
focus colour, each asserting that the ring between the border's outer arc and
the client's edge is CONTINUOUS.

```bash
bash contrib/avk-border-test.sh                              # all cases
CASES="base scale15 titlebar" bash contrib/avk-border-test.sh
ENGINE=gles  bash contrib/avk-border-test.sh                 # the reference
BREAK=border-square-inner bash contrib/avk-border-test.sh    # must FAIL
VKDEBUG=1    bash contrib/avk-border-test.sh                 # + sync validation
BORDER_DEBUG=1 CASES=base bash contrib/avk-border-test.sh    # log the geometry
```

The probe is deliberately **coordinate-free**: it detects the window in the
capture and measures the radii off the diagonal instead of mapping logical
geometry through the output transform. Reimplementing `wlr_box_transform()` in
a test is a second chance to get the permutation wrong, and it would agree with
the renderer whenever both were wrong. The invariant it checks needs no arc
centre at all — any ray outward from inside the window must run client, then
border, then wallpaper, and must never come back.

Three of its premises exist because each one caught a wrong conclusion:

- the client **settled** before the capture. wlrepaint reallocates on every
  configure, so between the resize and the client catching up the border is
  drawn at the new size over a surface still at the old one and the ring really
  is open — for a frame. Captured mid-flight this reads as a renderer bug, and
  as a *different* one each run: measured corner radii swung between 30 and 57
  on identical input before this wait existed.
- the **rounded-border path was taken**, read from `get avk-stats`
  (`rounded_border_draws`, and `asymmetric_border_draws` for the titlebar
  case). A build that quietly stopped rounding the inner edge scores a perfect
  zero wedge, because a square ring on a square hole has no seam to open.
- the window is clear of every screen edge, at **every scale**. At scale 1.5 a
  1920x1080 output is 1280x720 logical; the first geometry hung 80px off the
  bottom, the edge rule squared those corners, and the fixture reported a
  correct compositor as a broken renderer.

Two cases are honestly **skipped**, not passed: transforms 90 and 270, where
grim returns no capture from a rotated headless output. Transform 180 does
capture and carries the asymmetric-corner coverage.

The final section moves, resizes and unfocuses the window and asserts nothing
of the old ring survives, with the real `full_redraw_frames` /
`partial_redraw_frames` counters showing it happened under partial damage (1
full frame in 249). Those four assertions are **not falsified** and claim no
coverage of their own. `AZ_AVK_DAMAGE_HOLE` over the vacated band does not
break them: the hole applies from compositor start, so the region is frozen
before the window ever paints a border into it, and "no stale border survives"
comes out true because no border was ever there. A break that makes a test pass
for the wrong reason is worse than none, so it is not wired up. Move-damage
correctness is carried by `avk-rounded-alpha-test.sh` and `avk-damage-test.sh`,
which do have proven falsifiers.

Every pixel in a corner box is classified against the geometry rather than
counted: outside the arc it must be background *of the current generation*,
inside it must be the window or its border, and the antialiased band between is
not judged. Counting cannot separate a stale pixel from a correct one, and in
motion neither can an eye.

Four premises are asserted before any of that, and each of them has already
caught a run that would otherwise have passed:

- the background client committed new generations while capturing (its own log,
  which capture timing cannot fake);
- a generation flip really changed ≥75% of the background around the window
  (the pixels, which a client that logs without presenting cannot fake);
- the frames were **partial** redraws. The background client started out tiled
  and full-screen, so its whole-surface damage was whole-*output* damage and
  every frame counted as a full redraw — under which a corner survives because
  the scene is redrawn entire, which is the one thing this fixture may not
  conclude. It is floating and smaller than the output for that reason: 29 full
  frames to 175 partial;
- at `BORDER > 0`, a border is actually being painted — see `wlrepaint --ssd`.

`BREAK=damage-hole` sets `AZ_AVK_DAMAGE_HOLE=x,y,w,h`, which subtracts that
rectangle from every frame's damage *after* the ring has been rotated, so the
region counts as acknowledged and is never redrawn again. That is the exact
shape of the bug — a region wrongly believed current — and the fixture must
fail against it. It reports 271 unpainted pixels in the corner, black, in every
capture.

**`BORDER=6` fails today and is meant to.** It is the first M4B regression: AVK
cuts the border's inner edge as a square where SceneFX cuts it rounded, leaving
a wedge of background inside the corner (104 pixels per corner at radius 40,
against GLES's 0, unchanged by `AZ_AVK_FULL_DAMAGE=1`). It is not part of the
green suite — the default `BORDER=0` run is — and it must not be made to pass
by weakening what it expects. See `docs/avk-effects.md`.

`contrib/avk-shm-cache-test.sh` is the eleventh, and it is the only one with a
purpose-built client behind it.

```bash
cd contrib/wlreuse && make            # once
ASTEROIDZ=build-vk/asteroidz bash contrib/avk-shm-cache-test.sh
BREAK=lookup   ASTEROIDZ=build-vk/asteroidz bash contrib/avk-shm-cache-test.sh  # must FAIL
BREAK=identity ASTEROIDZ=build-vk/asteroidz bash contrib/avk-shm-cache-test.sh  # must FAIL
```

It asks whether a CPU-backed buffer is uploaded because its pixels changed or
because something looked at it, and there are two opposite ways to get that
wrong. `BREAK=lookup` is too much — 3,723,720 bytes per frame, the wallpaper,
recopied every frame.

**That break silently stopped breaking anything, and was caught months later
by an unrelated test.** `AZ_AVK_UPLOAD_ON_LOOKUP` restores the unconditional
upload *call*, but D.1 Phase 2 made the copy itself damage-driven — so the call
began finding no pending damage and `az_avk_upload_shm()` returned at
`rect_count == 0` having copied nothing. The break run came back **7/7**, which
is indistinguishable from having no coverage. It now sets `AZ_AVK_SOURCE_FULL`
alongside it and produces the documented 3,723,720 B/frame again.

The lesson generalises past this one switch: a break test is only a break while
the code it subverts still works the way it did when the switch was written.
Changing the *implementation* a switch pokes at can neutralise it without
touching the switch, the test, or anything that would show up in a diff. Break
tests need re-running on the schedule the real tests do, and a break run that
comes back green has to be treated as a failure of the suite rather than a
curiosity.

### Auditing every break at once

Finding that one prompted running all of them. As of 2026-08-11, on the AVK
suite:

| break | result |
| :--- | :--- |
| `avk-cursor` `cursor-texture` / `cursor-command` / `cursor-damage` / `cursor-upload` | fail correctly |
| `avk-cursor-content` `cursor-generation` / `cursor-hotspot` | fail correctly |
| `avk-shm-cache` `lookup` / `identity` | fail correctly *(after the repair above)* |
| `avk-shm-partial` `source-full` 18/23, `omit-region` 22/23 | fail correctly |
| `avk-sync` `presentsync` 8/10 | fails correctly |
| `avk-damage` `preserve` 6/12, `stale` 9/12 | fail correctly |
| `avk-damage-domains` `no-cull` | fails correctly *(after the harness change below)* |
| `avk-shm-rotate` `one-buffer` / `source-full` | fail correctly |
| `avk-cursor-lifetime` `cursor-stale-xcursor` | fails correctly |
| `avk-teardown` `destroy-before-idle` | fails correctly, every cycle |
| `avk-cursor-owner` `wlroots-move-resize` / `wlroots-xcursor` | fail correctly |
| `avk-dmabuf-feedback` `dmabuf-feedback-gles` | fails correctly — **on the source only**, see below |
| `avk-shm-partial` `unsafe-reuse` | **NOT TESTED** — documented, could not be made observable |

`no-cull` failed differently from `lookup`, and the distinction is worth
keeping. `lookup` was a working break that an implementation change
neutralised. `no-cull` never *could* have failed: the suite ran **one output**,
a node is only culled for being entirely on *another* one, and
`nodes_output_culled_before_resolve` measured 0 with the switch on and off
alike. Not weak coverage — none, from the day it was written, while looking
exactly like a break test.

### A second output, and why several counters needed one

`HL_OUTPUTS=2` makes the backend create `HEADLESS-2` and places it immediately
to the **right** of `HEADLESS-1`. Side by side is the whole point: a pair
stacked at the same layout origin makes every node touch both outputs, so the
thing a second output is usually added to test — that work belonging to one
monitor is not done for the other — stays untestable.

With it, `avk-damage-domains-test.sh` asserts the cull directly, and the break
finally fails:

| | culled | resolved |
| :--- | ---: | ---: |
| normal | 6 | 24 |
| `BREAK=no-cull` | 0 | 40 |

It is not the default. A second output costs another output's worth of frames
and GPU memory on every test that does not need it.

One counter stays at 0 and should: **`cursor_culled` is defensive only.**
`wlr_output_cursor.visible` is per-output and wlroots already computes it, so a
cursor on the other monitor is skipped as not-visible before the cull test is
ever reached. The check exists so that a cursor which *is* visible but lands
outside the buffer cannot produce a draw; nothing in normal operation reaches
it. That is a different thing from untested coverage and is recorded here so it
is not mistaken for one. `BREAK=identity` is too little — cache on the buffer
pointer, never re-upload, and a client that reuses a `wl_buffer` freezes on its
first frame. **Neither break is caught by the other's assertions**, which is why
both are here.

The second needs `contrib/wlreuse`, and nothing else in the tree can stand in
for it: every ordinary toolkit rotates through a pool of two or three buffers,
so a pointer-identity cache looks perfectly correct against kitty, against
swaybg and against every other client in this suite. wlreuse allocates one
`wl_shm_pool`, one `wl_buffer` and one mapping for the life of the process and
changes only the bytes — making buffer identity a lie on purpose.

One thing this test had to learn: an idle desktop now produces **zero** frames,
so "static buffer, thousands of frames, no uploads" needs something animating
to be measurable at all. A terminal's blinking cursor supplies the frames while
the wallpaper underneath stays static.

`contrib/avk-shm-partial-test.sh` (twelfth, `BREAK=source-full`,
`BREAK=omit-region`, `BREAK=unsafe-reuse`) and
`contrib/avk-damage-domains-test.sh` (thirteenth, `BREAK=no-cull`) complete the
M3.5D.1 set: the first asks whether a partial upload copies the right bytes,
the second whether source damage and scene damage are genuinely separate
domains. `BREAK=unsafe-reuse` is documented in its own header as **NOT TESTED**
rather than passing — it could not be made to fail observably, and saying so is
better than a checklist that lies.

`contrib/avk-cursor-test.sh` is the fourteenth, and it needed **two** new
clients because the suite was structurally incapable of seeing the bug it
covers.

```bash
cd contrib/wlcursor && make           # once
cd contrib/wlshot   && make           # once
ASTEROIDZ=build-vk/asteroidz bash contrib/avk-cursor-test.sh
BREAK=cursor-texture ASTEROIDZ=build-vk/asteroidz bash contrib/avk-cursor-test.sh  # must FAIL
BREAK=cursor-command ASTEROIDZ=build-vk/asteroidz bash contrib/avk-cursor-test.sh  # must FAIL
BREAK=cursor-damage  ASTEROIDZ=build-vk/asteroidz bash contrib/avk-cursor-test.sh  # must FAIL
```

The three breaks fail on different assertions, which is the point of having
three. `cursor-texture` loses the image entirely — and produces *no frames at
all*, because with no cursor there is no cursor damage. `cursor-command` keeps
the eight frames of cursor damage and draws nothing into them, which is the
only thing that establishes AVK rather than something else put the pointer
there. `cursor-damage` fails only the cost assertion, at 921,600 of 921,600
pixels; it breaks damage globally rather than only for the cursor, and says so,
because the damage arrives from wlroots and nothing narrower exists to break.

A client that sets its own cursor image had it silently dropped under AVK, and
**nothing in contrib/ sets one** — every test client leaves the pointer to the
compositor's xcursor theme, so the entire suite ran green through an invisible
cursor. `contrib/wlcursor` is the only client that calls
`wl_pointer.set_cursor`.

Two things about observing a cursor at all are worth knowing before writing any
further cursor test:

- **grim cannot see cursors.** It asks screencopy for `overlay_cursor = 0`, so
  its captures never contain one and any assertion about a cursor drawn from a
  grim screenshot is vacuous. `contrib/wlshot` asks for the cursor.
- **A headless output is blind to them by construction.** The headless backend
  implements `set_cursor()` as `return true;` and does nothing, so it believes
  it has a hardware plane and every cursor goes to a plane that does not exist.
  Asking screencopy for the cursor calls `wlr_output_lock_software_cursors()`,
  which is what forces it to be composited into the frame — so requesting the
  cursor in the capture *is* the request to exercise the software path, with no
  debug switch in the compositor.

Capture cursorless **first**. wlroots deliberately keeps software cursors on
after a lock drops (`output/cursor.c:75`), so a cursorless capture taken second
still contains the cursor and the control becomes a copy of the experiment.

The hotspot assertion is the one that carries weight. wlcursor paints a single
opaque-black pixel at its hotspot, because a compositor that ignores the
hotspot entirely draws the whole image, correctly, one hotspot away — and
passes every pixel-count assertion ever written.

`contrib/avk-cursor-content-test.sh` is the fifteenth, and it exists because
everything before it ran the cursor at scale 1.

```bash
ASTEROIDZ=build-vk/asteroidz bash contrib/avk-cursor-content-test.sh
BREAK=cursor-generation ASTEROIDZ=build-vk/asteroidz bash contrib/avk-cursor-content-test.sh  # must FAIL
BREAK=cursor-hotspot    ASTEROIDZ=build-vk/asteroidz bash contrib/avk-cursor-content-test.sh  # must FAIL
```

It found a real bug on its first run. `wlr_cursor_set_buffer()` takes the
hotspot in *logical* units while its sibling `wlr_output_cursor_set_buffer()`
takes it in *buffer pixels* — one multiplies by the output scale downstream and
the other divides. The two are identical at scale 1, so nothing before this
could see it; at scale 2 the cursor landed offset by half of itself.

Two mistakes in writing it are worth more than the test:

**An assertion about a change has to name both endpoints.** The content check
first read "capture 2 is blue". Under `BREAK=cursor-generation` the cursor
freezes on whatever colour was current when the image was first uploaded —
which, with an animation already running, was blue. The assertion passed
perfectly while the bug it existed for was fully present. It now reads "capture
1 was red and not blue, and capture 2 is blue and not red".

**A cycling animation is a race.** At 2000 ms spacing a capture three seconds
later landed on the *second* transition and read the original colour back, and
the test failed for a reason that had nothing to do with the compositor.
`wlcursor --animate-once` makes one transition and stops, so any later capture
sees the same thing. The same applies to `--reuse-buffer`: gating its commits
on the pointer being inside made the result depend on how long `contrib/wlvptr`
happens to live.

One incidental finding from `BREAK=cursor-hotspot`: a cursor drawn 8 px from
where wlroots believes it is comes back **24×24 instead of 32×32**, scissored
by a damage region computed around the real position. That is independent
confirmation that cursor damage is tight rather than a full-output repaint.

`contrib/avk-cursor-hide-test.sh` is the sixteenth, and it is entirely
differential.

```bash
ASTEROIDZ=build-vk/asteroidz bash contrib/avk-cursor-hide-test.sh
BREAK=cursor-command    ASTEROIDZ=build-vk/asteroidz bash contrib/avk-cursor-hide-test.sh  # must FAIL
BREAK=cursor-generation ASTEROIDZ=build-vk/asteroidz bash contrib/avk-cursor-hide-test.sh  # must FAIL
```

"The cursor went away" has four failure modes and three of them survive a
screenshot taken a moment too late: a **ghost** (the pixels stay because
nothing damaged the rectangle), **over-damage** (repainting the whole output to
remove 32×32 — correct, invisible to any pixel comparison, ruinous at pointer
rates), **collateral** (the rectangle repainted with the wrong thing), and a
**stale image** (the cursor returns or changes and the old one appears).

So every assertion compares frames rather than inspecting one: the strongest is
that hide-then-show is **byte-identical** to before the hide (0 px differ), and
that hiding changes exactly 1024 pixels at exactly the cursor's rectangle with
0 full-output redraws.

It needed `hold:<ms>` on `contrib/wlvptr`. That tool moves the pointer and
exits, which destroys the virtual pointer device and the seat's pointer
capability with it — and `set_cursor` carries an enter serial the compositor
checks against the currently focused pointer client, which by then is nobody.
Every hide/unset step would have been silently dropped.

There is deliberately **no `BREAK=cursor-old-damage`**. AVK does not compute
old-position damage; wlroots emits it and scenefx feeds it into the ring AVK
reads. A switch to disable it would mean punching a hole in AVK to suppress
wlroots' work, which tests a fabrication rather than the compositor.

### The seventeenth, and a bug the whole suite was built not to see

`contrib/avk-shm-rotate-test.sh` exists because every SHM test before it used a
client that keeps **one** buffer.

```bash
cd contrib/wlrotate && make           # once
ASTEROIDZ=build-vk/asteroidz bash contrib/avk-shm-rotate-test.sh
BREAK=one-buffer  ASTEROIDZ=build-vk/asteroidz bash contrib/avk-shm-rotate-test.sh  # must FAIL
BREAK=source-full ASTEROIDZ=build-vk/asteroidz bash contrib/avk-shm-rotate-test.sh  # must FAIL
```

`wl_surface.damage_buffer` states what changed since the previous **commit of
the surface**. A renderer caching one image per `wl_buffer` needs a different
quantity: what changed since it last saw *that buffer*. Those are the same
region for as long as a client reuses one buffer, and `contrib/wlreuse` holds
exactly one for its whole life — so `avk-shm-partial` passed 23/23 against a
desktop on which every KDE application visibly flickered. Rotate a pool and the
two quantities come apart on the second commit.

The instructive part is what happened to the first version of this test, which
asserted the obvious thing — every mark the client drew is on screen in every
capture. **It passed against the pre-fix binary.** The marks survive a
screenshot on a build that flickered a real desktop; something restores the
displayed content before a capture lands. It was committed with a
`STATUS: NOT YET VALID COVERAGE` header rather than counted, because a test that
cannot fail is worse than no test — it is a green checkmark over a live bug.

What does falsify is the upload accounting, which measures the mechanism
directly instead of its consequences. Per-buffer stats come from `get avk-stats`
→ `shm_sources[]`, and every mark commit reports exactly one 24×24 rectangle, so
bytes convert straight into "how many marks' worth of damage did this buffer
receive". With four buffers and eight marks each buffer carries two mark
commits of its own, so anything above two marks' worth can only have come from
commits it did not carry:

| | partial bytes per buffer | in marks | bound |
| :--- | ---: | ---: | ---: |
| pre-fix `a00a911` | 2312 | 1.00 | 2 |
| post-fix `ef45327` | 9224 | 4.00 | 2 |

The bound is deliberately generous to the broken build: it counts the buffer's
*first* mark commit, which is always a full upload and contributes no partial
damage at all. Upload **counts** are identical on both builds (1 full + 3
partial per buffer, same generations) — only the bytes differ, which is why a
counter-based assertion would have been as blind as the pixel one.

Both breaks remove the condition rather than the fix — `one-buffer` stops the
rotation, `source-full` stops partial uploads — so both land on the
accumulation assertion with nothing left to measure. Neither is a falsifier for
the fix itself; the pre-`ef45327` binary is, and that is the one that matters.

### A test for a bug that would not crash on demand

`contrib/avk-cursor-lifetime-test.sh` covers the use-after-free that killed a
live desktop during M3.5E, and it is the clearest example in this document of
why "reproduce the crash" is sometimes the wrong goal.

```bash
ASTEROIDZ=build-vk/asteroidz bash contrib/avk-cursor-lifetime-test.sh
BREAK=cursor-stale-xcursor ASTEROIDZ=build-vk/asteroidz bash contrib/avk-cursor-lifetime-test.sh  # must FAIL
```

M3.5E stage 1 kept the resolved `struct wlr_xcursor *` as durable cursor
identity. That memory belongs to a theme inside `wlr_xcursor_manager`, and
`reapply_cursor_style()` destroys and rebuilds the whole manager on **any**
live config change — a settings slider, `set-config`, a reload. Two paths
replayed the cached pointer with no re-resolve: `az_cursor_show()`, the
idle-hide restore, and `az_cursor_xcursor_tick()`, the animation timer.

**The crash was never reproduced headlessly.** Roughly 110 iterations across
four reproducer shapes, under ASan, with mixed scales, output crossings,
hide/restore and repeated manager rebuilds — the pre-fix build never faulted
and the sanitizer never fired. Freed heap usually stays mapped and usually
still holds plausible bytes, so the old code mostly read garbage quietly. The
live SIGSEGV was the rare case where the wild pointer happened to be unmapped.
Two of those attempts failed for reasons worth knowing: `cursor_hide_timeout 0`
means `handlecursoractivity()` returns before the replay path, and
`config_apply_live()` un-hides the cursor *before* it destroys the manager, so
the hide has to come afterwards.

Waiting for undefined behaviour to happen to fault is not coverage. So the test
asserts the **invariant** one step before the dereference:

> a borrowed xcursor may only be used while the manager generation that
> produced it is still current

`cursor_mgr_generation` increments on every rebuild; `cursor_stale_xcursor`
counts borrowed resolutions used after theirs expired, and zero is the only
correct value. `BREAK=cursor-stale-xcursor` restores the shipped model — keep
the pointer, replay it — and the run comes back with **57** stale uses instead
of 0, deterministically, without depending on allocator behaviour or timing.

The instrumentation is diagnostic and does not change the production model: the
durable state is the name and scale, and the pointer is resolved at the point
of use and never stored. Rebuilding that as "cache the pointer plus a
generation number" would keep the wrong ownership and merely detect it.

### A break that can only protect a label, and the test that protects the rule

`contrib/avk-dmabuf-feedback-test.sh` checks that DMA-BUF feedback describes
what AVK can import, and `BREAK=dmabuf-feedback-gles` restores the shipped
behaviour of describing the GLES compatibility renderer instead.

```bash
ASTEROIDZ=build-vk/asteroidz bash contrib/avk-dmabuf-feedback-test.sh
BREAK=dmabuf-feedback-gles ASTEROIDZ=build-vk/asteroidz bash contrib/avk-dmabuf-feedback-test.sh  # must FAIL
meson test -C build-vk dmabuf-feedback
```

The break fails, but be clear about **why**: it fails because the reported
`source` changes from `avk` to `wlr_renderer`. It does **not** fail on the
format set, because on this machine the GLES and Vulkan tables overlap enough
that the wrong set still passes every subset check — which is precisely how
the bug survived the whole of M3.5. A break that only moves a label is
protecting a label.

So the rule itself is tested in `tests/test-dmabuf-feedback.c`, on capabilities
the test invents:

```text
AVK can import    A B C
other renderer    A B D
must advertise    A B C        C present, D absent
```

plus MOD_INVALID filtering, render-only modifiers not leaking into a set that
describes sampling, YCbCr withheld, an all-withheld table refused rather than
advertised as empty, and — with two real render nodes opened and made to
disagree — the main device following AVK rather than the other engine. The
live compositor can never exercise that last one here: both engines select the
same node, so the disagreement has to be constructed.

Verified against a broken model before being trusted. Changing the builder to
read `render_mods` instead of `texture_mods` takes it from 12/12 to 8/12,
including *"a modifier only the other renderer supports is NOT advertised"*.

The general point, which cost a milestone: **a test whose two possible answers
happen to coincide on your hardware is not coverage, however green it is.**

### Three harness bugs that made tests pass by doing nothing

Found while chasing a live cursor defect, and worth more than the defect. All
three had the same shape: a call that looked like it did something, did
nothing, and said nothing.

**`hl_move`/`hl_click` past the first output went nowhere.** wlvptr's
coordinate space is the LAYOUT bounding box, and `HL_PTR_EXTENT_W/H` default
to one monitor's size. `hl_sync_pointer_extent()` exists to correct that and
**nothing called it** — so on a two-output layout `hl_move 2880 555` was
scaled into the first output and landed nowhere near its target. A test could
drive a window that was never under the pointer and report a pass.
`hl_start` now calls it.

**`"$HL_WLVPTR" move:X,Y` is not wlvptr syntax.** wlvptr takes
`x y extent_w extent_h [action]`; the `move:` form prints usage and exits 1,
and every call site had `>/dev/null 2>&1` over it. Fourteen calls across three
AVK tests were silent no-ops. Use `hl_move`.

**`monitorrule { scale 1.5 }` is silently ignored.** `monitorrule` parses
colon-separated `key:value` pairs, so the KDL-child form is accepted and
discarded. Two tests claimed mixed scales and ran both outputs at scale 1 —
including the P0 cursor-lifetime test, whose "mixed scales and output
crossings" therefore never happened. Per-output scale belongs in the `output`
block; `HL_SCALE1`/`HL_SCALE2` now set it, and tests that depend on mixed
scales **assert the scales** before relying on them.

The last one is the sharpest lesson. Mixed scales are not decoration: at equal
scales a cursor sized per-output by wlroots and one sized at the sharpest
scale by asteroidz come out identical, so an entire class of ownership bug is
invisible. A test that silently loses its mixed-scale setup does not get
weaker, it goes blind — and it keeps printing "ok".

### A cursor with two owners

`contrib/avk-cursor-owner-test.sh` covers the defect those three hid.

```bash
ASTEROIDZ=build-vk/asteroidz bash contrib/avk-cursor-owner-test.sh
BREAK=wlroots-move-resize ASTEROIDZ=build-vk/asteroidz bash contrib/avk-cursor-owner-test.sh  # must FAIL
BREAK=wlroots-xcursor     ASTEROIDZ=build-vk/asteroidz bash contrib/avk-cursor-owner-test.sh  # must FAIL
```

Seven call sites bypassed `az_cursor_set_xcursor()` for wlroots' own, which
selects a per-output image at native scale — so `az_avk_emit_cursors()` drew
asteroidz's pixels into wlroots' box. See
`docs/vulkan-native-architecture.md` §5.4m.

It needs three things at once and asserts all three as premises: software
composition (`software_cursor_frames > 0`, `hardware_cursor_frames == 0`),
outputs at genuinely different scales, and a window actually under the pointer
— `moveresize()` resolves its target with `xytonode(cursor->x, cursor->y)` and
returns 0 when it finds nothing, while the IPC call still answers
`{"success":true}`. The window centre is read from `get all-clients` rather
than hard-coded.

`cursor_size` is **pinned to 28**. The two ownership models only produce
different pixel sizes when the theme's nearest available size differs between
`base * sharpest_scale` and `base * output_scale`; at the harness default they
coincide, both models agree, and the size half of the bug is invisible — a
test passing because two answers agree, not because either is right.

Two breaks, because one is not enough:

| break | what it restores | how it fails |
| :--- | :--- | :--- |
| `wlroots-move-resize` | the shipped defect exactly — only move/resize shapes go to wlroots | `cursor_geometry_mismatch` **50**, plus both shape assertions |
| `wlroots-xcursor` | the whole ownership model handed to wlroots | `cursor_no_image` **95** — AVK has nothing to draw |

The second was written first and left `cursor_geometry_mismatch` **asserted
but never falsified**: with every selection going to wlroots, `az_cursor` ends
up with no image, AVK composites nothing, and a counter over zero frames is
trivially zero. A break that fails loudly for the wrong reason still leaves an
assertion untested.

### A teardown test, and how the first one managed to prove nothing

`contrib/avk-teardown-test.sh` covers AVK's destruction path.

```bash
ASTEROIDZ=build-asan/asteroidz CONFIG=all CYCLES=10 bash contrib/avk-teardown-test.sh
BREAK=destroy-before-idle ASTEROIDZ=build-asan/asteroidz bash contrib/avk-teardown-test.sh  # must FAIL
```

The first attempt at this ran ten headless cycles, reported all ten clean, and
was worthless. `headless.sh` resolves `HL_ASTEROIDZ` from `$ASTEROIDZ` at
**source** time, so `ASTEROIDZ=x hl_start` — a per-command assignment — arrives
far too late and silently launches `build/asteroidz`, which on this machine was
a fortnight-old build with no Vulkan in it. `az_avk_finish()` returned at its
first line and the thing under test never executed. **No crash is also what
code that never ran produces**, and nothing in that harness could tell the two
apart.

That is why the compositor now prints four markers from inside the path:

```
CLEANUP_BEGIN          cleanup() entered
AVK_TEARDOWN_BEGIN     az_avk_finish() entered, and whether AVK was active
AVK_TEARDOWN_END       it returned having destroyed everything
CLEANUP_END            cleanup() returned
```

and a cycle missing any of them — or reporting `active=no` — is counted as
**INVALID**, not as a pass. The stats line `avk.shm_commits=` is not a
substitute: it is skipped when AVK is inactive, so its absence is ambiguous
between three different failures.

A related correction: the earlier note that a headless `hl_stop` kills the
compositor before `cleanup()` runs is **wrong**. `handlesig()` routes
SIGINT/SIGTERM to `quit_now()` → `wl_display_terminate()` → `run()` returns →
`cleanup()`, and the markers prove it every cycle. SIGTERM is a real graceful
exit here. The dispatch `quit` deliberately is not — it raises the confirmation
prompt and waits for a keystroke — so a signal is both correct and the only
option a harness has.

`CONFIG` selects what there is to tear down, because a teardown test of an
empty renderer tests nothing: `bare`, `shm` (wlrotate's rotating pool),
`dmabuf` (a GPU client), `mixed`, `cursor-xcursor`, `cursor-software`,
`cursor-churn`, or `all`. Hardware cursor planes are absent by construction —
a headless output has no plane — and are left to live acceptance rather than
faked.

The assertions that matter are the ownership ones, not the absence of a crash:
`lifecycle_violations` at 0, and every AVK-owned device child at 0 in the
census logged immediately before `vkDestroyDevice`. Those fire *at* the
violation. A glibc abort fires somewhere else entirely, or — three times in
four, on the desktop this was found on — not at all.

`BREAK=destroy-before-idle` removes the device-idle wait that now precedes
output destruction, which is the shipped ordering restored. It fails every
cycle with `VUID-vkDestroySemaphore-semaphore-05149`: the per-output present
fence being destroyed while a submitted batch still refers to it. Normal runs
report zero. Note what this break does **not** claim — see
`docs/vulkan-native-architecture.md` §5.4l: the original live abort was never
reproduced headlessly, and 46 graceful exits of the unmodified pre-fix build
produced no validation error at all.

One assertion in this test was wrong on its first run and is worth recording,
because it failed against the *fixed* build: it demanded the cursor be
re-selected after a manager rebuild. It must not be. The buffer asteroidz holds
stays valid across the rebuild — `readonly_data_buffer_drop()` copies the
pixels into `saved_data` whenever locks are held — so re-importing on every
settings change would be pure waste. The test now asserts the opposite.

Deliberately **not** asserted: that a *different* cursor name re-selects
because identity is compared by name rather than by pointer. That failure needs
the allocator to return the same address for a different cursor, which a test
cannot arrange on demand; asserting it anyway would pass for the wrong reason.
The name comparison is exercised by `avk-cursor-content`, which changes shapes.

### The schema, checked from both ends

`src/config/config-schema.h` describes every settable option — type, range, enum
members, default, and a human label and description — so a settings UI does not
have to carry a hardcoded mirror of the compositor's parser. It is hand-written,
because the failure modes are not symmetric: a wrong *generator* produces a wrong
schema silently and the UI then writes wrong values into someone's config, where
a wrong *checker* produces a red test.

So there are two checkers, and they cover opposite directions.

`asteroidz -S` runs without a compositor, the same way `-p` does, and drives the
real `set_value_default`, `override_config` and `parse_option`. It parses no C at
all. For every entry it asserts the default matches what the code produces, that
a value past each bound lands on the bound, that poisoning the field and writing
the default actually changes it (which catches a renamed key — the `if/else`
chain silently ignores one it does not know), and that a value survives a round
trip. On its first run against a hand-written table it found 21 wrong defaults
and three wrong types, including a `float` field described as an `int`.

Two details in there are load-bearing and easy to get wrong:

- **Clamps are derived from behaviour, not from where they are written.** They
  live in both `parse_option` (`blur_transparency_threshold`) and
  `override_config` (`borderpx`), so a check that looked in one place would
  report half the table as unclamped.
- **The poison value has to be inside the valid range.** An out-of-range
  sentinel gets clamped by `override_config`, which changes the field on its
  own — so the reachability check passed for a deliberately renamed key, and only
  the round-trip assertions noticed. Renaming a key in the table is now caught
  by the check that exists for it.

`tests/check-config-schema.py` covers what `-S` structurally cannot: a key that
exists in `parse_option` and is simply **absent** from the table, which has no
entry to run a check against. It takes the described keys from the binary
(`asteroidz -L`) rather than by regex over the C table — an over-matching pattern
reported 100 keys where there were 95, which would have hidden five omissions —
and the handled keys by parsing `parse_option` at brace depth 1, so the sub-keys
inside the `windowrule`/`monitorrule`/`tagrule` branches are not mistaken for
standalone options.

Exemptions live in `tests/schema-exempt.txt` and must sit under a `## reason:`
heading; a bare list of exempt keys is an escape hatch nobody has to justify,
which is not one. The checker also fails on a key that is both described and
exempt, and on an exemption for a key `parse_option` no longer handles — either
would let the next real omission hide behind a stale line.

Coverage today: `parse_option` handles 231 keys, of which 95 are described. Of
the 136 exempt, 17 are structural (rules, binds, lists, directives) and **119
are simply not described yet** — listed key by key so the gap is auditable and
so adding one is deliberate. The file shrinking is the measure of progress, and
the largest single drop so far was not describing anything: 55 of those keys
configured a bar the compositor stopped drawing, and deleting them beat
documenting them.

Env: `ASTEROIDZ` (binary under test, default `build/asteroidz` next to the
repo, falling back to `/usr/bin/asteroidz`), `HL_OUTDIR`, `HL_WIDTH`/
`HL_HEIGHT`.

`HL_RENDERER` picks the renderer — `gles2` (default) or `vulkan`. gles2 is
the default because it is the one renderer present on every machine that runs
this suite, so results stay comparable; set `vulkan` to exercise the `fx_vk`
path, which is what a real session here actually uses. Editing the launch line
in `headless.sh` by hand does **not** work once the file is sourced: the
function body is already parsed, and doing so silently produced a second gles2
run labelled as vulkan.

`HL_ENV` passes `NAME=VALUE` pairs through to the compositor. The launch uses
`env -i` so a test instance can never inherit the caller's session — which
also means an exported variable does not reach it. `FX_VK_VALIDATION` and
`MESA_VK_TRACE` both looked like the driver was ignoring them when in fact
they had never arrived:

```sh
HL_RENDERER=vulkan HL_ENV="FX_VK_VALIDATION=1" bash contrib/regression/run.sh
```

Set `HL_ALLOW_DESTRUCTIVE=1` to also run
`destroy-virtual-output.sh`, which is skipped by default (it destroys every
headless output, including the original one — safe in isolation, but not
worth risking in a shared run).

## Live-session mode (extreme caution)

`HL_LIVE=1` attaches to the *caller's own already-running* compositor instead
of launching an isolated instance (`hl_start_live` in `contrib/lib/headless.sh`,
requires a valid `ASTEROIDZ_INSTANCE_SIGNATURE` already in the environment).
By default every dispatch is still confined to a fresh virtual/headless
output this creates on the fly — real outputs are never touched. Set
`HL_LIVE_MON=<name>` (e.g. `DP-1`) to instead run directly against that
*real, physically-connected* monitor, disturbing whatever's actually
displayed there for the duration.

This is not a routine testing mode. Real-world experience running it:
a full test run against a live session found and fixed a genuine
use-after-free segfault (`asteroidz_icon_node_set`, `src/draw/text-node.c`)
and, separately, a frame-scheduling bug (`monitor_check_skip_frame_timeout`,
`src/asteroidz.c`) that could freeze the compositor's real output and leak
memory unboundedly under continuously-updating real content (e.g. video)
with `animations` disabled — the second one froze the whole display and
required a hard reboot to recover, on real monitor hardware, in a matter of
seconds. Neither bug reproduced headlessly under any combination tried.
`hl_notify()` fires desktop notifications (via `notify-send`, live mode
only) at start/per-module/finish so a live run is never silent — this
does not make it safe, only visible. Treat any live-mode run, real-monitor
or virtual, as needing fresh explicit sign-off every single time, never a
standing permission, and prefer chasing anything it turns up via
`coredumpctl`/static review afterward (as with both bugs above) rather than
reproducing it live again.

### A live test that is not a live *run*: software cursor acceptance

`contrib/avk-software-cursor-acceptance.sh` is a different animal from
`HL_LIVE=1`, and the distinction is worth naming. It attaches to the real
session, but it **drives nothing**: the user moves the pointer and watches the
screen, the script reads counters at phase boundaries, and the only write in
it is an optional `dispatch reset_avk_stats`, which sets counters to zero and
touches no rendering or window state. There is no virtual output, no synthetic
input, no window created or destroyed. That is why it can be run on a working
desktop when `HL_LIVE=1` cannot.

It exists because **a headless output is structurally incapable of measuring
what it measures.** The headless backend implements `output_set_cursor()` as
`return true;`, so it believes it always has a hardware plane — a fresh
headless AVK instance reports `hardware_cursor_frames: 3, software_cursor_frames: 0`
without a cursor plane existing anywhere. Forced software and hardware-planed
are the same code path there. On real KMS they are not: one hands 64×64 to a
plane, the other puts the compositor in the frame path for every pointer
motion, at pointer rates, indefinitely.

It needs a session started from
`/usr/share/wayland-sessions/asteroidz-avk-swcursor.desktop`
(`ASTEROIDZ_AVK_FORCE_SOFTWARE_CURSOR=1`). There is deliberately no runtime
toggle: promotion and demotion are startup decisions, and a switch would let a
test claim a transition the compositor never makes. The script checks
`/proc/<pid>/environ` rather than the counters, because a counter cannot
distinguish "forced software" from "software because something demoted it a
moment ago", and refuses to run otherwise.

Five phases: 30 seconds of pointer motion over static content (the invariant —
localized damage, no full redraws, no CPU waits, p95 inside a frame budget),
shapes, output crossings, the client matrix, and hide/restore. Each ends with a
question only a pair of eyes can answer, asked and **counted** at the end — a
correct-looking cursor is invisible to every counter, which can see a missing
one and not a wrong one. An acceptance run that quietly drops its visual half
is not an acceptance run.

## How it works

`contrib/lib/headless.sh` is the shared library: `hl_start` launches one
fully isolated headless compositor instance (own `XDG_RUNTIME_DIR`, Wayland
socket, `ASTEROIDZ_INSTANCE_SIGNATURE`) plus a flat-color `swaybg` wallpaper,
never touching your real session. `hl_dispatch`/`hl_get` wrap `amsg
dispatch`/`amsg get` scoped to that instance; `hl_watch_start` backgrounds an
`amsg watch ...` stream for asserting on IPC notifications. `hl_spawn_kitty`/
`hl_spawn_wllayer`/`hl_spawn_wlkeys`/`hl_spawn_wlstates` spawn tracked,
throwaway test clients; `hl_sandbox_globals` runs one to completion.
`hl_reset` kills
spawned windows and returns to a known state (tag 1, tile layout, `HEADLESS-1`
focused) between test cases so they can't leak state into one another.
`hl_assert`/`hl_assert_eq`/`hl_assert_true`/`hl_assert_false` are the
pass/fail primitives, tallied globally; `hl_summary` prints totals.

`hl_start` also brings up a **private `dbus-daemon --session`** and points the
compositor at it, because the portal backends (global shortcuts, inhibit) live
on D-Bus and without a bus they are not there to test. The real user bus is the
wrong answer twice over: your live session already owns
`org.freedesktop.impl.portal.desktop.asteroidz`, so a test instance could never
take the name — and if it somehow did, a headless compositor with no screen
would be the thing answering Discord's push-to-talk. `hl_busctl` runs `busctl`
against that private bus, and returns non-zero when there is none, which is
what lets `inhibit-portal` skip itself on a machine with no `dbus-daemon`
installed rather than fail. The daemon is killed by recorded PID in `hl_stop`,
never by pattern.

A test that needs a daemon the bar talks to should **stand one in** rather
than let the real one start: the voice-menu test serves a synthetic snapshot
over the instance's own socket with `socat` and sets `discord { daemon-cmd
"" }`, because the module otherwise spawns `discord-voiced` — with your real
token — into every isolated instance that has no socket. Skip cleanly
(`command -v socat` etc.) when an optional tool is missing.

`contrib/regression/run.sh` boots one shared instance and runs every
`test_*` function from `contrib/regression/tests/*.sh` against it, in file
order, with `hl_reset` between each. Extend coverage by adding a new
`tests/<area>.sh` file with `test_*` functions, not a bespoke one-off script.

## Custom test clients

None of `kitty`/`wlvptr`/`wlvkbd` can reach every corner of the compositor,
so the harness includes a few small purpose-built Wayland clients:

- **`contrib/wlvptr`** — `wlr-virtual-pointer-unstable-v1` client for
  synthetic pointer input (click/scroll/drag), scoped to whichever
  compositor `WAYLAND_DISPLAY` points at (unlike `ydotool`, which is
  uinput/kernel-level and routes to whatever seat is active system-wide —
  not safe to use against a headless test instance).
- **`contrib/wlvkbd`** — `zwp_virtual_keyboard_unstable_v1` client for real
  key press/hold/release sequences (`wlvkbd hold KEY... -- COMMAND`), for
  testing input-path behavior that bare IPC dispatch can't reach — e.g. a
  chord/combo keybind whose state only resets on a genuine key-release
  event, or a Super+drag mouse binding (hold a modifier while a nested
  `wlvptr ... drag:x,y` runs).
- **`contrib/wllayer`** — a minimal `wlr-layer-shell-unstable-v1` client
  (layer/anchor/exclusive-zone/keyboard-interactivity/size all configurable
  via CLI args, plus an optional scripted resize-in-place) for layer-shell
  edge cases: exclusive-zone reservation, stacking across layers, and
  regression-pinning past bugs (a DPMS/disabled-monitor layer-configure bug,
  and the original stale-shadow-after-resize bug) directly instead of only
  inferring them through the waybar popup harness.
- **`contrib/wlkeys`** — an `xdg-shell` toplevel that REPORTS keyboard events
  back out (one line per `enter`/`leave`/`key`/`modifiers`, raw evdev
  keycodes). Every other client here drives input *in*; this is the only one
  that observes what the compositor *sent*, which is the only way to assert on
  `wl_keyboard.enter`'s held-key array. That array is how a tag-switch binding
  used to leave a Proton game repeating a key forever: the client is told the
  bound key is held at the moment focus arrives, and the release that would
  stop it is deliberately swallowed. Use `hl_spawn_wlkeys` and
  `hl_wlkeys_last_enter`.
- **`contrib/wlstates`** — an `xdg-shell` toplevel that reports the state array
  from every `xdg_toplevel.configure`, spelled out by name. It binds
  `xdg_wm_base` at **version 6** on purpose, because that is the floor for
  `suspended` and a compositor must withhold that state from older clients.
  `WAYLAND_DEBUG` is not a substitute: it renders the array as `array[20]` and
  never its contents, and a tag hide does not even change the array's *length*
  (the window loses `activated` exactly as it gains `suspended`), so a byte
  count reads identically whether suspension is implemented or not. Use
  `hl_spawn_wlstates` and `hl_wlstates_last`.
- **`contrib/wlrotate`** — a `wl_shm` client that rotates a **pool**, which no
  other client here does. It draws a row of marks, one added per commit, and
  repaints every buffer it acquires with every mark so far — the age-correct
  repaint a real toolkit performs — while damaging only the newest mark. That
  is entirely correct Wayland and it is the exact shape of client that broke
  AVK's per-buffer image cache. `--buffers 1` collapses it into a `wlreuse`
  lookalike, which is how a test proves it is measuring rotation and not
  something else; `--full-damage` is the wasteful control that is immune to the
  bug by construction.
- **`contrib/wlrepaint`** — a `wl_shm` client that repaints its **whole**
  surface every generation, alternating every pixel between two four-colour
  checkers (`red`/`green`, then `blue`/`yellow`). It exists because a
  persistence test is only as good as the change it tests against, and nothing
  else here changes enough: a settled terminal repaints nothing, and
  `wlrotate` draws its marks and then idles on a one-pixel damage rectangle —
  164 and then 72 changed pixels across a whole screen, which is not a
  background a stale region can be stale *against*. Both fixtures built on
  those failed their own premise check, and would have reported a clean corner
  otherwise.

  The palette is the measurement. One background surface means one buffer means
  one generation visible at a time, so parity is recoverable from a **single**
  capture: a background pixel of the wrong colour pair is showing an older
  frame, with no pairing of screenshots and no theory about when the flip
  happened. `--solid RRGGBB` with `--frames N` makes the *foreground* of such a
  test instead: flat, stationary, and completely quiet once placed — a window
  that keeps re-committing identical content damages its own box every frame
  and repaints the very region under test.

  `--ssd` is not optional when borders matter. A client that never binds
  `xdg-decoration` is CSD by default (`client_wants_ssd`), so asteroidz draws
  it **no border** while still insetting its surface by `borderpx`: the window
  looks bordered and the ring is empty. Measured without it, both renderers
  "failed" a border test with background where the border should be.
- **`contrib/wlsandbox`** — a `security-context-v1` client: it creates a real
  security context over a listening socket of its own, connects a *second*
  display through it, and reports the globals the compositor is willing to show
  a sandboxed client. The privileged deny list in `modern.h` is interface-name
  strings matched with `strcmp`, so an entry naming no real global is not a
  build error and not a runtime error — just a line that never fires. That is
  exactly how `wlr_export_dmabuf_manager_v1` (missing the `zwlr_` prefix) sat
  in the list handing full-screen capture to every Flatpak. Enumerating the
  registry from inside the sandbox is the only way to see it. Use
  `hl_sandbox_globals`, and assert the *whole* list — see
  `tests/security-context.sh`, which also asserts the mirror image (an ordinary
  client still sees the global) so a compositor that simply stopped
  advertising it can't read as a pass.
- **`contrib/portal-inhibit-client.py`** — not a Wayland client at all: a D-Bus
  one that takes an `org.freedesktop.impl.portal.Inhibit` request and then
  *stays connected*. `busctl` cannot test that interface, because an inhibition
  lives only as long as the connection that asked for it — asteroidz watches
  for the owner dropping off the bus so a crashed `xdg-desktop-portal` cannot
  leave the machine awake forever, and `busctl` exits the instant its call
  returns, so every inhibition it takes is reclaimed a millisecond later,
  correctly and uselessly. It speaks the *impl* interface directly, standing in
  for what `xdg-desktop-portal` would send, so no portal daemon has to be
  installed or raced and the object paths are the test's to choose. `--monitor`
  prints each `StateChanged` as a JSON line.

## Module coverage

Thirty-four modules as of writing: `layouts`,
`window-states`, `tags`, `tag-rules-ipc`, `focus`, `scratchpad`, `geometry`,
`dwindle`, `overview`, `multimonitor`, `mousebind`, `hdr`, `scroller`,
`animations`, `layer-shell`, `ipc-watch`, `keybind-combo`, `set-option`,
`config-ipc`, `config-write`, `rules-ipc`, `border-colors`, `idle`,
`inhibit-portal`, `prompt`, `titlebar`, `output-reflow`,
`output`, `vrr`, `effects`, `floating`, `quit-confirm`, `screenshot-ui`,
`fullscreen-bleed`, plus `destroy-virtual-output` (gated
behind `HL_ALLOW_DESTRUCTIVE=1`).

`screenshot-ui` and `fullscreen-bleed` both need **two** monitors and skip
themselves on one, the way `multimonitor` does — each is about a boundary, and
with no neighbour every assertion in them would pass by default. `screenshot-ui`
measures the overlay's pointer confinement, which doubles as the only handle on
"is the overlay up" the socket offers; `fullscreen-bleed` compares pixels on the
neighbouring output before and after a fullscreen round trip, because the
geometry can be correct while the surface is drawn past it.

`inhibit-portal` is the only module that leaves Wayland entirely: it drives
`org.freedesktop.impl.portal.Inhibit` over the private bus described above, and
skips itself when there is no `dbus-daemon` or no PyGObject. Its assertions are
weighted towards **release** rather than acquisition, because a stub that
accepts an inhibition and does nothing and a backend that takes one and never
gives it back fail in the same direction — silently, hours later, on a flat
battery. So it closes a request while its client stays connected (the
application's path), `SIGKILL`s a client that is still holding one (the crash
path), and checks that releasing one client's inhibition does not release
another's. It also carries the one test for the refactor that let two backends
share a bus name: a global-shortcuts session must still close through the now
shared `Session` object, which is a failure that would otherwise stay invisible
until an app tried to close one.

`bar` is the pattern to copy for anything that needs a **different config**
than the shared one: it never turns the bar on globally (that would
shrink the usable area and silently break every geometry assertion in the
other modules), but rewrites `$HL_CONFIG` from a pristine copy and calls
`reload_config` per test, restoring it afterwards. It also skips itself when
the binary under test was built with `-Dbar-config=false`, probing the binary
rather than assuming — an unknown config key is only warned about, so a
feature-off build would otherwise silently "pass" by doing nothing.

`config-ipc` covers the settings-UI read surface — `get config-schema`,
`get config`, `get dispatch-actions` and `watch config` — through the socket,
because that is how a client sees them. `asteroidz -S` and `-L`/`-D` check the
tables against the code, but none of them go through IPC. Most of its assertions
are about whether the reply says something **true** rather than whether a field
exists: a schema with wrong defaults still parses, provenance naming the wrong
file still parses, and a watch that pushes the whole config every time still
parses. So it checks the reported line number really holds that setting, that a
colour's hex and floats agree on every channel, and that a no-op change pushes
nothing at all.

`rules-ipc` covers the same read surface for window rules and keybinds —
`get window-rule-schema`, `get window-rules`, `get binds`. What makes it worth a
module of its own is that both structures are **lossy once parsed**: a
`KeyBinding` is a function pointer and a union by the time it exists, and a
`ConfigWinRule` cannot say whether the file wrote `0` or wrote nothing. So those
verbs are served from records captured while reading, and the thing to check is
that the records say what the file said.

It covers the write path too — `set-window-rules` and `set-binds` — where the
sharpest assertion is that **a batch of edits does not shift itself apart**. Every
splice moves every offset after it, so applying edits in the order they arrive
leaves the second span pointing into the middle of what the first edit produced.
The result still parses, which is exactly what makes it a bug that ships. Edits go
back to front; reversing the sort turns that test red.

Its sharpest read assertion is that a rule which sets one field reports **exactly**
that field. A serialiser emitting all 53 would look correct in a diff and would
leave a rule editor unable to tell "leave blur alone" from "turn blur off" — and
would write the latter for every field on the first save.

It covers `capture-chord` too, whose sharpest assertion is that **a chord which is
already bound is still captured, and does not fire**. Both halves matter: the
first is the whole reason capture lives in the compositor rather than in the
settings window, and the second is what stops a captured `Super+Q` from also
closing the window you are editing from.

That test was order-dependent twice before it settled, which is worth recording.
It started on the harness's `F11 -> combo_view`, whose chord flag is
process-global and cleared only by a real key release — so whether it changed the
tag depended on whether `keybind-combo` had run first. Rebinding to `view` did not
help, because `view` ORs rather than replaces while that flag is set. It observes
a **config value** now, set through `set_option`, which has no state behind it at
all. Both earlier versions passed when the module was run alone.

It also found two silent bugs while being written, both of the same shape:

- **`kdl_binds` always passed the bare `bind`**, so `parse_bind_flags`' `s`, `l`,
  `r` and `p` were reachable only from the legacy line format. A `binds` block
  could not express a release binding at all, and the flag was discarded without
  a word. The test asserts the flag by **contrast** — one chord with
  `release=#true` and one without — because "release is true" alone would pass
  against a build that reported true for everything.
- **`#true` was not a boolean.** `#` is a legal bare-word character, so KDL v2's
  spelling parsed as the *string* `"#true"` and every consumer ran it through
  `atoi` and got `0`. Nothing in the tree writes v2 spelling, which is the only
  reason it never bit. Both spellings are accepted now and both are asserted.
- **The legacy comma form was swallowed.** `windowrule "appid:x,isfloating:1"` —
  what an old `windowrule=` line becomes — reached a handler that only read a
  node's children, so it produced a rule with no matchers. A rule with no matchers
  matches every window.

`config-write` covers `set-config`, which is the half that makes a setting a
setting rather than a preview. It **writes to `$HL_CONFIG`** and so restores a
pristine copy after every test, the way `bar.sh` does — modules run in name
order, which puts it ahead of `geometry`, whose assertions depend on gaps and
border widths a test here could have left changed.

Its sharpest case is the corpus one: writing *every* described option, group by
group, then asserting the config still parses **and** that a reload logs no
`Unknown keyword`. Writing one option proves the mechanism; writing all of them
proves the schema. That is what caught `theme/border-color` and
`animations/enable` claiming nested KDL paths that `kdl_key_map` had no entry for
— the write succeeded and the *next reload* rejected the file. `asteroidz -S` had
not caught it because every check there went through `parse_option` with the
internal key and none went `path → key`; it now does.

Its most valuable case was added afterwards, by the settings window:
`test_set_config_a_preview_does_not_lose_the_declaration`. `set-config` with
`persist:false` is the live-preview path, and it used to erase the file, line and
path a key was declared at — one line in `config_source_note`. Three separate bugs
came out of that, and all three only bite a caller that previews before it saves,
which is exactly what a settings UI does:

- a persisting write after a preview found no declaration, fell back to the
  canonical path in the main config, and appended a **second** one — leaving the
  user's `misc { border_radius 9 }` dead and a duplicate winning by position;
- a previewed removal had no line left to delete and reported success with the
  setting still in the file;
- a previewed `colors.kdl` key lost the origin that makes it read-only, so the next
  write went to `config.kdl` **without `override:true` ever being asked for** —
  the guard defeated by the thing it guards.

Only the second had a visible symptom, and it surfaced in a bar test rather than
here. Provenance now carries "set in memory" as a flag beside the file rather than
instead of it; `asteroidz -P` shows both as separate columns for the same reason.

Two traps it walked into while being written, both worth knowing:

- **An assertion must not change global state.** Proving a described action is
  really dispatchable started as `dispatch toggle_gaps` — which turns gaps off
  *globally* and stays off, and `hl_reset` does not restore it. The whole suite
  then ran with no gaps, and `geometry`'s `adjust_gaps` test failed three modules
  later having changed a gap size that was no longer being drawn. Nothing in that
  failure pointed back at the cause. It now dispatches `zoom_reset`, which sets
  the cursor zoom to a value that is already the default, and asserts by
  **contrast**: a real name returns `{"success":true}` and an invented one
  `{"error":"unknown function"}`. "The compositor still answers" would have
  passed with a table full of typos.
- **`jq`'s `tonumber` is decimal only.** The colour round-trip assertion parsed
  `"0x44" | tonumber`, which is an error, so the comparison ran against nothing
  and failed against a build whose colours were exactly right. It compares in
  python now.

Real gaps found by building this out (not just harness bugs — documented
inline in the relevant test files too):
- `set_master_factor`/`adjust_master_count` are dead code: they write to
  `selmon->pertag->{mfacts,nmasters}[curtag]`, but no current layout (tile/
  scroller/float) reads either value.
- `switch_keyboard_layout`/`dwindle_split_horizontal`/`switch_proportion_preset`
  are genuine no-ops without the right config present (a second keyboard
  layout, `dwindle_manual_split 1`, a configured proportion preset list
  respectively) — `hl_start`'s shared config enables all three specifically
  so these dispatches are actually observable.
- IPC's client geometry (`x`/`y`/`width`/`height`) is always the logical
  target (`c->geom`), never the interpolated value the renderer actually
  draws from — there's no dispatch-and-poll sequence that can tell "snapped
  instantly" from "mid-animation" this way. Real animation verification
  needs pixel/frame capture (`anim-test.sh`), a fundamentally different
  kind of tool.
- Two real bugs only surfaced via live-session mode, neither reproducible
  headlessly: a use-after-free in `asteroidz_icon_node_set` (destroyed the
  old cairo surface before the `wlr_buffer` wrapping it was dropped/detached
  from the scene — every other node type in `text-node.c` does the reverse
  order), and a frame-scheduling bug in `monitor_check_skip_frame_timeout`
  (a 100ms "give up and force a commit" safety timer that reset on every new
  resize/configure event instead of enforcing a hard deadline, letting a
  busy real client starve the monitor's actual output commit indefinitely —
  a frozen display and an unbounded memory leak as the same root cause).
  Both were ultimately root-caused via `coredumpctl`/static review, not by
  reproducing them live a second time. See "Live-session mode" above.
- `dispatch set_option` respawned every `spawn` entry in the config, once per
  call, because it applied the change through the full reload path. One extra
  short-lived process is invisible; a settings panel with a live-preview
  control sends `set_option` per frame and would fork per frame. Split into
  `config_apply_live()` and `reset_option()`; `set-option.sh` pins both halves,
  because a test that only asserts "set_option is quiet" passes just as well on
  a build where `run_exec` was deleted outright.

Two things about writing a test that uses `spawn`, both of which produced a
*passing* assertion that proved nothing before being noticed. A path must be
**quoted** — a bare leading `/` starts a KDL comment, so `spawn /tmp/x.sh` is a
parse error that takes the rest of the config with it, and the reload then
silently does nothing. And an inline `sh -c "..."` does not survive: the KDL
handler joins the node's argv tokens with spaces before handing the result to
`spawn_shell`, which runs it as `sh -c <string>`, so the inner quotes are gone
by then. Point `spawn` at a script.

## A separate layer: the tray host

It runs against its own `dbus-daemon` (via `dbus-run-session`) and its own
`XDG_RUNTIME_DIR`, so it never touches the live session's tray, and uses
`contrib/snitem` as a stand-in item rather than needing a real tray
application. Each case re-enters the script on a fresh bus, because a watcher
name can only be claimed once and a stale claim would test the wrong role.

The assertion worth knowing about is the **pixmap cap**. A tray host decodes
pixmaps whose dimensions the application chooses, which is unbounded work
driven by whatever the user happens to have installed — in the compositor that
is a stalled desktop. `snitem --pixmap N` serves an N×N icon, and the test
checks that 48 and 512 are decoded while 4096 is refused outright and the item
simply does not appear. Point it at any host to see whether that host is
bounded; the built-in `tray` module is not, which is why trayd exists.

## A separate, complementary layer: waybar plugin unit tests

The three-plus custom waybar CFFI plugins (`waybar-display`, `waybar-weather`,
`waybar-sysmon`, and others, in separate repos under `src/`) each have their
own `tests/test_<name>.c` + `make test` — plain C unit tests that `#include`
the plugin's own source to reach its pure, GTK-independent logic (icon/text
mapping, JSON parsing, scheduling math) directly, with no GTK init, no
Wayland socket, and no live compositor at all. These live in each plugin's
own repo rather than here, since the code under test does too.
