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

The third was `contrib/blur-exclusion-test.sh`, **removed with the renderer it
asserted**. It read a shadow's backdrop-blur source image directly, through
SceneFX's `FX_BLUR_DUMP`, and asserted that no pixel of the window survived
anywhere in it — because whether the halo is *visible* depends on the window's
size, its colour against the backdrop and the blur's reach, and the two shadow
scenes in `contrib/regression/tests/effects.sh` happen to sit where it is not:
both passed on a build whose fill left 7575 of 30800 hole pixels holding the
window's own colour.

AVK has no such stage. Its blur source is the scene *prefix* — the commands
below the blur node — so the window is never in its own source, there is no
fill, and the invariant is structural rather than asserted. The script's own
header said it would go with `fx_vk`, and it has. AVK's equivalent facility is
`AZ_BLUR_DUMP` / `amsg dispatch dump_blur_source` (see
[effects](./visuals/effects.md#dumping-a-blurs-source)); it writes the same kind
of image for a different set of stages — one per live blur node, plus the
monitor background cache's own source on any frame that rebuilt it.

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

The fifth was `contrib/shadow-exclude-clamp-test.sh`, **removed for the same
reason as the third**. It covered the clamp's own blind spot under SceneFX: the
clamp takes a minimum against the unblurred source, and inside the window box
the shadow excluded there was no unblurred source, only the synthetic fill the
exclusion left behind. Clamping against a fabrication drags the region toward
the fabrication's darkest structure — invisible behind an opaque window, a dark
window-shaped patch through a translucent one.

Under AVK there is no fill to clamp against. `sample_exclude` exists in the
scene structure but AVK's live path ignores it (`avk_render.c`, "sample_exclude
is NOT a disqualification"), because prefix capture already keeps the window out
of its own source. The defect class is structurally absent, so the assertion
could no longer fail on its subject.

Its second lesson outlived it, and is why the fixtures around it are built the
way they are. The script rendered one scene twice — clamp on, clamp off — and
run against a binary predating the environment hook the clamp stayed on for
*both* captures: the two images agreed inside the hole and the real assertion
passed for entirely the wrong reason. A test whose premise can silently stop
holding needs that premise asserted next to it.


The sixth was `contrib/shadow-hole-visible-test.sh`, **removed with the third
and the fifth**, and it covered what all five of the others shared: they use
opaque windows. A shadow's backdrop blur fills the
excluded window box with a fabrication — a stretched strip, a mirrored band —
and behind an opaque window that is unobservable by construction. Through a
terminal at 0.98 opacity it is two percent of every pixel, which is how it was
found: on a real desktop, not here.

It rendered one scene twice, with `shadows_blur_background` on and off, and
asserted that inside the window the two are identical (the fabricated fill must
contribute nothing there) while the shadow band outside them differs (which
proves the feature was running). Measured: 70.0 levels of change inside the
window before the composite was clipped, 0.011 after, with the band at 4.56 in
both.

**Its premise inverted under AVK.** The fix it guarded lived entirely in
`asteroidz-scenefx`, which is no longer in the build. AVK fabricates no fill, so
what shows through a translucent window there is the *real* blurred backdrop —
and a real backdrop is allowed to differ between blur on and blur off. Run
against AVK the fixture failed honestly (8.539 levels inside the window, premise
holding at 4.781 in the band) while asserting something that should no longer be
true. It was removed rather than re-aimed: "does a translucent window look right
over a blurred shadow backdrop" is a question for the eye, not for this
measurement.

Two earlier versions of this measurement were wrong in instructive ways.
Correlating what shows through the window against the bare wallpaper is
confounded — a translucent window has its *own* backdrop blur, so what shows
through is a blurred wallpaper and does not track a sharp one; that scored 0.76
on a correct build against 0.57 on a broken one. And the first wallpaper put its
detail in the centre and left the shadow band over a flat field, where a blur
returns the field unchanged and nothing can be detected at all — the premise
check caught it and refused to certify the scene.


`contrib/blur-sync-validation.sh` sits outside the numbered set, registered
`manual` because it needs `vulkan-validation-layers` installed and gates
nothing. It is the only fixture in the tree that asks Vulkan's
*synchronization* validation about the blur cache: the twenty-odd fixtures that
read `validation_errors` are reading the ordinary layer, which does not see a
missing barrier. It changes the wallpaper three times — the trigger that marks
the per-monitor cache dirty and makes it re-render mid-session, on a queue that
is already busy — and counts `SYNC-HAZARD` reports. Zero on the current build.

Its guard is the interesting part. A run where sync validation silently failed
to come up reports zero hazards and looks exactly like a pass, so the script
refuses to certify one: it asserts `sync_validation=on` from
`avk_instance_log_caps` and exits 2, INCONCLUSIVE, if it cannot find it. That
guard used to look for a string (`validation layer enabled`) that nothing ever
logged, so it fired on every run and the script could only ever exit 2 — inert
for as long as it was in the tree, and indistinguishable from a missing package.
`SYNC_ENV=` runs the same scene with validation off and must reach that exit,
which is how the guard itself is kept honest.

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

`contrib/avk-frame-test.sh` is the eighth. It **used to** compare two
*renderers* — the same scene rendered by SceneFX and by AVK, asserting the two
frames agreed. That oracle expired with SceneFX. The reference arm's last act
was to fail two assertions by rendering every window as a flat block of
titlebar colour, which is exactly the symptom the fixture exists to catch,
reported against the oracle instead of the subject.

It now boots **once**, and its oracle is arithmetic, computed from the
compositor's own reported geometry:

```bash
bash contrib/avk-frame-test.sh
BREAK=border   bash contrib/avk-frame-test.sh  # must FAIL
BREAK=noborder bash contrib/avk-frame-test.sh  # must FAIL
BREAK=hole     bash contrib/avk-frame-test.sh  # must FAIL
BREAK=wrapper  bash contrib/avk-frame-test.sh  # must FAIL
```

`get all-clients` gives each window's box and `borderpx` is pinned in the
fixture's own config; the surface rectangle and the border ring follow exactly.
Twenty assertions, every one of them an exact pixel count or an exact zero,
with no tolerance anywhere — at `border_radius 0` with effects off, every edge
in the frame is on an integer coordinate. The claims are: each window's own
colour fills its surface rect and appears nowhere else; the ring is painted all
the way round with no wallpaper showing through it; no trim colour is painted
inside the surface (the original defect, stated directly rather than as a pixel
count copied off another renderer); and every pixel outside every window is
exactly the wallpaper.

Each of those has a falsifier and each falsifier's reach is written down in the
fixture header — including that `BREAK=border` reaches the **unfocused window
only**, because the scene walk emits its border rect after its content and the
focused window's before, so with the clip gone the first fills over its surface
and the second fills under it. That was read off `AVK_SCENE_DUMP`, not
inferred. One assertion ("the window's own colour appears nowhere outside it")
has **no** falsifier and claims no coverage: `AZ_BREAK_AVK_QUAD_SWAP_CORNERS`
was tried for it and the suite came back 20/20.

Effects stay off, but no longer because they were M4 — because shadow, blur and
a corner radius each paint outside a window's box and would turn "is this pixel
wallpaper" into a judgement call. They have their own fixtures.

`contrib/avk-sync-test.sh` is the ninth, and it asserts on nothing you can see.

```bash
bash contrib/avk-sync-test.sh
BREAK=presentsync bash contrib/avk-sync-test.sh  # must FAIL
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
bash contrib/avk-damage-test.sh
BREAK=preserve bash contrib/avk-damage-test.sh  # must FAIL
BREAK=stale    bash contrib/avk-damage-test.sh  # must FAIL
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
BORDER=6          ASTEROIDZ=build/asteroidz bash contrib/avk-rounded-persist-test.sh
BREAK=border-square-inner BORDER=6 ASTEROIDZ=build/asteroidz bash contrib/avk-rounded-persist-test.sh  # must FAIL
```

`contrib/avk-idle-convergence-test.sh` (M4C.3H) pins one rule:

> A visually stable compositor state must converge to **zero** self-generated
> repaint work.

Not "less". Zero. Once animations have finished and no client has committed,
the compositor must generate no further frames of its own.

```bash
bash contrib/avk-idle-convergence-test.sh
BREAK=gradient-noop-damage bash contrib/avk-idle-convergence-test.sh   # must FAIL
```

Every client in it paints **once** and then goes quiet (`--frames 1`), which is
what makes the number mean anything — a client repainting continuously supplies
frames of its own and "frames per second" stops being a statement about the
compositor.

Two premises exist because each already caught a run that would have passed:

- **a window is focused.** The per-frame border path only runs for
  `selmon->sel`; with nothing focused the scene converges trivially.
- **focus actually moved, per switch.** The first version always dispatched
  `left`, so with two windows switches 2–4 moved nothing and reported a
  perfectly settled compositor that had never been asked to do anything. It
  alternates left/right now and compares the focused title before and after.

Every query is wrapped in `timeout`. Under the break the compositor's event
loop never returns to its clients, so an untimed `amsg` blocks forever — the
difference between a red test and a wedged machine. `hl_wait_client_count()`
goes through the library's untimed `amsg` and had to be replaced with a bounded
wait for exactly that reason: the first break run hung before reaching a single
assertion.

**And then the break run came back green on the two assertions that matter.**
An `amsg` that times out produces *no output*, and `jq`'s `// "TIMEOUT"`
fallback never fires because `jq` is handed an empty stream and emits nothing
too. So the reading was `""`, `"" != "TIMEOUT"` was true, the starvation
assertion passed — and `$((F2 - F1))` on two empty strings evaluated to `0`, so
the convergence assertion passed with it. Both headline claims reported success
against a compositor that was wedged and leaking half a gigabyte. The emptiness
is caught once, in `frames()`, and the convergence assertion is now evaluated
unconditionally rather than skipped when the query failed — skipping it is how a
wedged compositor scores a clean result.

The suite also asserts the gradient is still *drawn* and still a *ramp*: a
dirty check that suppressed every write into oblivion would also converge to
zero frames, and would be a worse bug than the one being fixed.

`contrib/avk-gradient-test.sh` (M4C) **is gone.** It compared AVK's overview
vignette against the same scene rendered by SceneFX, and both halves of that
have expired: SceneFX no longer exists as a renderer, and the vignette it
wanted to measure is never created headlessly — it is guarded by the overview
wallpaper node, and with the overview open, rects go 41 → 153 with **not one**
carrying `has_gradient`. Re-measured on the AVK-only build: `gradient_draws`
reads 0 with the overview closed *and* open. The fixture skipped with 0/0 for
its entire life and had no oracle left to restore.

The gradient coverage it was meant to provide is carried by two things that do
not need a second renderer:

- `tests/test-avk-gradient.c` — a CPU model. It compares AVK against an
  independent C transcription of `gradient.frag` at every count, angle, origin
  and mode. Stricter than the pixel comparison ever was, with the known
  weakness that a transcription can be wrong the same way twice.
- `contrib/avk-gradient-border-test.sh` — the pixel oracle, on the gradient an
  actual desktop turns on. Self-consistent (off arm vs on arm, same build),
  with three falsifiers of its own including the repaint-storm one.

The deleted fixture nearly reported a pass, and the reason is worth keeping:
its edge-darkening premise measured 21.0 on **both** engines with no gradient
on screen at all — the overview dims its own background, that dimming is a
plain rect, and the two renderers agreed about it exactly. Only the
`gradient_draws == 0` counter caught it. A premise that a non-gradient
satisfies is not a premise.

`contrib/avk-border-test.sh` (M4B) is the border's own suite: seventeen
configurations of radius, border width, output scale, transform, opacity and
focus colour, each asserting that the ring between the border's outer arc and
the client's edge is CONTINUOUS.

```bash
bash contrib/avk-border-test.sh                              # all cases
CASES="base scale15 titlebar" bash contrib/avk-border-test.sh
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
bash contrib/avk-shm-cache-test.sh
BREAK=lookup   bash contrib/avk-shm-cache-test.sh  # must FAIL
BREAK=identity bash contrib/avk-shm-cache-test.sh  # must FAIL
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

`contrib/avk-visible-clip-test.sh` (`BREAK=no-repair`, `BREAK=no-clip`) asks the
next question: not whether a copy carries the right bytes, but whether it should
have happened at all. Its fixture is built so that the cost and the correctness
can disagree — the client goes **quiet** after committing eight generations, of
which the last five were hidden, and only then is the window above it closed.
Nothing the client will ever send again can fix what appears. `BREAK=no-repair`
passes every byte assertion in the file and shows the wrong picture, which is
what that arm exists to demonstrate.

The suite also carries the decoration-frame shape directly: an opaque
`wl_subsurface` over most of a large `wl_shm` parent, nothing covering the
window at all, and most of every copy still waste. A fixture built only out of
overlapping windows cannot see that case, and it is the one real clients hit.

`contrib/avk-cursor-test.sh` is the fourteenth, and it needed **two** new
clients because the suite was structurally incapable of seeing the bug it
covers.

```bash
cd contrib/wlcursor && make           # once
cd contrib/wlshot   && make           # once
bash contrib/avk-cursor-test.sh
BREAK=cursor-texture bash contrib/avk-cursor-test.sh  # must FAIL
BREAK=cursor-command bash contrib/avk-cursor-test.sh  # must FAIL
BREAK=cursor-damage  bash contrib/avk-cursor-test.sh  # must FAIL
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
bash contrib/avk-cursor-content-test.sh
BREAK=cursor-generation bash contrib/avk-cursor-content-test.sh  # must FAIL
BREAK=cursor-hotspot    bash contrib/avk-cursor-content-test.sh  # must FAIL
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
bash contrib/avk-cursor-hide-test.sh
BREAK=cursor-command    bash contrib/avk-cursor-hide-test.sh  # must FAIL
BREAK=cursor-generation bash contrib/avk-cursor-hide-test.sh  # must FAIL
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
bash contrib/avk-shm-rotate-test.sh
BREAK=one-buffer  bash contrib/avk-shm-rotate-test.sh  # must FAIL
BREAK=source-full bash contrib/avk-shm-rotate-test.sh  # must FAIL
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
bash contrib/avk-cursor-lifetime-test.sh
BREAK=cursor-stale-xcursor bash contrib/avk-cursor-lifetime-test.sh  # must FAIL
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
bash contrib/avk-dmabuf-feedback-test.sh
BREAK=dmabuf-feedback-gles bash contrib/avk-dmabuf-feedback-test.sh  # must FAIL
meson test -C build dmabuf-feedback
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
bash contrib/avk-cursor-owner-test.sh
BREAK=wlroots-move-resize bash contrib/avk-cursor-owner-test.sh  # must FAIL
BREAK=wlroots-xcursor     bash contrib/avk-cursor-owner-test.sh  # must FAIL
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

### M4E: the render graph, and comparing two builds

Three suites arrived with M4E and one of them is a lesson about the harness.

```text
tests/test-avk-graph.c        47   topology, derived barriers, allocation
tests/test-avk-transient.c    33   pooled images, timeline-safe reuse
tests/test-avk-multipass.c    18   a two-pass frame with deterministic pixels
contrib/avk-graph-test.sh     16   the compositor's own frame, and pixel equivalence
contrib/avk-graph-perf.sh     20   four scenes, both binaries, alternating
```

### M4F: the blur, its material, and the walker

```text
tests/test-avk-blur.c            50   the dual-Kawase primitive and its support
tests/test-avk-blur-scene.c      24   a blur's source is the CURRENT-frame prefix
tests/test-avk-segment.c         18   any command range, any regional target
tests/test-avk-blur-material.c   30   darken, edge_softness, clip, alpha, corners
tests/test-avk-cmd-uses.c        21   every command kind states what it samples
contrib/avk-blur-walker-test.sh  29   real WLR_SCENE_NODE_BLUR through the walker
contrib/wlbgeffect/              --   a client that supplies a real blur region
```

**`contrib/avk-blur-damage-test.sh`** is the compositor-level damage oracle:
settle, screenshot, `amsg dispatch damage_all`, screenshot, and the two must be
identical. It takes a CONTROL pair of screenshots first, because the fixture was
not static: kitty blinks a cursor, and a two-terminal desktop moved 1513 pixels
at 197 codes between any two frames — larger than the 1458 the oracle then
reported, and it would have been read as staleness. `HL_KITTY_EXTRA` turns the
blink off.

Two more things that phase taught, both of them the same shape:

```text
counters after a     a settled desktop renders NOTHING, so reading avk-stats two
reset                seconds after resetting them reports 0 blur nodes for a
                     desktop full of them. damage_all forces exactly one frame.

`move`, unverified   a `move` dispatch was added to place a floating overlay and
                     every counter in the phase came back byte-identical with and
                     without it. It is gone: a dispatch whose effect no
                     measurement can see makes a fixture look more specific than
                     it is.
```

**That script deliberately has no BREAK mode.** `AZ_BLUR_UNDER_DAMAGE=1` passed
it 12/12 in every geometry tried — a window move, a border toggle behind a
floating overlay, the overlay inside one terminal and across the seam between
two. A compositor's real damage is the wrong shape to expose it: a move damages
the whole node so the demand sweep recomputes the blur anyway, and a small change
strands a region one support away, where a dual-Kawase kernel's energy is in its
tail and the error is below one 8-bit code. The break is falsified at the
RENDERER instead, where a 24×24 block at full brightness on a near-black ground
leaves 5261 wrong pixels at 71 codes and fails 20 of 65 checks.

### The first-divergence oracle

`AZ_FRAME_ORACLE=1` renders every frame **twice** — the production partial render
into the real scan-out buffer, and an independent full render of the same
immutable snapshot into an image nothing presents — reads both back on the GPU
and reports the first frame where they differ. Three boundaries are tapped:

```text
PREFIX   the regional scene-prefix transient, after the segment and before the
         blur chain samples it        -> segmented source reconstruction
BLUR     the same image after the chain
                                      -> the chain's own coordinate mapping
OUTPUT   the target after compositing, before presentation
                                      -> damage, scissor, buffer history
```

A tap is a graph pass declaring `AVK_USE_TRANSFER_READ`, which is what makes
reading the *foreign* scan-out target possible without hand-rolling a
queue-family transfer — the graph already owns the acquire and release.

**On Path B the PREFIX and BLUR taps are DECLINED, and say so once.** Every
stride and every compare in the oracle is four-bytes-a-pixel; Path B's prefix
and blur images are scene-linear FP16 at eight, so a tap would read half of each
row and report the difference between two misaligned pictures as a divergence.
The OUTPUT tap is unaffected — that is still the scan-out buffer, in the
scan-out format — so the oracle still names a divergent frame, it just cannot
say which boundary produced it. Adapting it would mean giving it a second notion
of difference: its whole vocabulary is "N pixels differ by M *codes*", and a
code is a property of a quantised format, not of half-floats.

```text
contrib/avk-oracle-test.sh   10   premise + control; MODE=fixture adds 180 deg
```

```sh
bash contrib/avk-oracle-test.sh
MODE=fixture bash contrib/avk-oracle-test.sh
MODE=transforms bash contrib/avk-oracle-test.sh
bash contrib/avk-transform-test.sh
./build/test-transform-math
SEAM_RR1=2 BREAK=halo-damage-raw bash contrib/avk-blur-seam-test.sh  # must FAIL
```

`avk-oracle-test.sh` **asserts the premise first**: a build with
`AZ_AVK_DAMAGE_HOLE=300,200,120,90` must be caught, and it is, on frame 0 with
the mismatch bounding box equal to the hole and a worst channel error of 255. An
oracle that has never reported a divergence is not evidence.

Two things this mode is not:

```text
counters are DOUBLED   every frame goes through avk_render_frame() twice, so
                       renderer counters include the reference render. The mode
                       says so, loudly, once. Never assert on avk-stats in an
                       oracle run.

it must compare        `dropped` and `invalidated` are printed on every line and
                       asserted at zero. The readback buffer used to grow
                       mid-frame, which frees the memory the production taps
                       recorded into -- reading back as every pixel differing at
                       full amplitude, indistinguishable from a catastrophic
                       renderer bug.
```

The prefix and blur taps compare only the region production **claims**:
`prefix_rebuild` and `result_region`. Comparing the whole capture reports a
difference for every pixel the design leaves undefined, which is most of it.

`avk-oracle-runs.sh` ran that minimal trigger repeatedly — two adjacent outputs,
the first rotated 180°, a blurred window straddling the seam, and a new window
appearing on the *second* output at the join — reporting per run the stale pixels
after the interaction, the static control, the first divergent frame, the buffer
slot, the halo-record count and the boundary classification. It was removed once
the transform work closed; `avk-oracle-test.sh` is the assertion that remains.
Its lesson does not go with it: three consecutive runs cannot tell determinism
from luck at a 2-in-3 failure rate, and that mistake was made once and retracted.

### The transform pixel oracle

`amsg dispatch capture_output` writes the Vulkan **attachment** to a binary PPM
— after compositing, before presentation — at any output transform. It replaces
grim as the correctness oracle, because grim captures *nothing* from a 90° or
270° output on this backend, and a rotation nobody can look at is a rotation
nothing can test. The first capture of a 90° frame found the bottom 200 rows had
never been written.

```text
contrib/avk-transform-test.sh   38   the pixel oracle at every TRANSFORM
tests/test-transform-math.c     33   round trip, area, extent swap, half-open
tests/test-extent-space.c       34   the two extents, as two types
contrib/lib/ppm.py              --   read / diff / bbox / canon / verify / grid
```

A rotated output has a different **logical shape**, so it is compared against an
unrotated output of the *same logical shape*, each capture inverse-transformed
into logical orientation first:

```text
mode 800x600 rr 180  vs  mode 800x600 rr 0     0 px differ
mode 800x600 rr  90  vs  mode 600x800 rr 0     0 px differ
mode 800x600 rr 270  vs  mode 600x800 rr 0     0 px differ
```

Three things had to leave the fixture before that was exact, and each was
measured rather than assumed:

```text
text        a terminal lays its glyphs out for the output it was told about
            -- 7183 differing pixels, every one inside a text row
the cursor  a property of the pointer, not the transform; parked at a fixed
            LAYOUT coordinate instead
dither      a hash of the DEVICE pixel position, so it legitimately differs
            when the device grid rotates -- 134389 px at 180 degrees
```

`AZ_SHADOW_DITHER_AMP=0` for the last. Rounded corners, shadows and blur are
kept out of the geometric fixture for the same reason: their analytic
antialiasing samples at mirrored positions and rounds a code differently (1247
px with effects on, 1163 of them differing by exactly 1). Asserting equality on
a fixture with no sub-pixel content is stronger than choosing a tolerance on one
that has.

The falsifier runs at every rotation: `AZ_AVK_DAMAGE_HOLE=240,180,200,160`
changes exactly **32 000 px = 200×160** at 0°, 90°, 180°, 270° and on the tall
reference. One wrong turn worth keeping: a 140×110 hole changed *nothing* with
blur on, because blur damage propagation dilates by the kernel's 126-pixel
support and closes any hole smaller than it. That is correct behaviour, and it
is why the falsification fixture has no blur.

`MODE=transforms bash contrib/avk-oracle-test.sh` runs the damage matrix — nine
positions, from the interior to a one-pixel column — at **all eight**
`wl_output_transform`s and requires partial == forced-full on every frame of
every one.

### The pacing trace: AZ_PACE

"Animations feel laggy" is at least four claims — the motion takes too long, it
starts too late, it advances unevenly, or it advances evenly and is not
*presented* evenly — and none of them can be told apart from a frame-rate
counter. `AZ_PACE=1` records the timeline itself as raw events on one monotonic
clock, and leaves every conclusion to `contrib/pace-analyse.py`.

```text
anim start   a semantic change gave a client a new target, and whether the
             previous one was still in flight (retarget=1)
anim tick    one interpolation step: the clock it read, the eased factor, the
             real-valued position the curve asked for, and the INTEGER one the
             scene node stored. `fv=` carries the FOUR per-axis curve factors
             (x, y, width, height) behind it, and `ideal=` is each coordinate
             on its own curve; `factor=` stays one settled-ness number, the
             minimum across the four
shatter tick one step of a `shatter` close: its normalised progress, how many
             fragments are still live, the cloud's bounding box (which is what
             bounds the frame's damage) and the fade
tag cost     one completed TAG transition: how long it lasted, how many
             frames it took, the blur prefix pixels it rebuilt, what it
             damaged, and the p50/p95 of its per-frame render cost
present      per output, from the backend's own presentation feedback
render       one render_monitor pass: its cost, whether it committed anything,
             and the damage area and extents it committed
```

The tick line carries the ideal position *and* the stored one on purpose: the
gap between "the curve says 240.6" and "the node says 240" is the whole
quantisation question, and a trace that logged only one of them could not tell
"the curve is uneven" from "the curve is smooth and truncation made it uneven".

The present listener is **not wired at all** unless `AZ_PACE=1`; nothing else in
the compositor needs presentation feedback.

```text
contrib/anim-pace-test.sh    --   six workloads x refresh/blur/scene/scheduler
contrib/pace-analyse.py      --   PRESENT / RENDER / ANIM sections, --since=NS
contrib/avk-blur-count-matrix.sh  --  cost per blur chain at N = 1,2,4,5,8
```

```sh
WORKLOADS=move BLUR=1 WINDOWS=5 bash contrib/anim-pace-test.sh
HZ1=144 HZ2=60 OUTS=2 WORKLOADS=tag bash contrib/anim-pace-test.sh
COUNTS="1 2 4 5 8" bash contrib/avk-blur-count-matrix.sh
python3 contrib/pace-analyse.py TRACE --since=$(...)   # monotonic ns
```

**Three traps this fixture had to be built around.**

**The renderer used to be selectable, and this fixture was built when it was.**
`ASTEROIDZ_RENDERER=avk` no longer exists: AVK composites unconditionally and
scenefx's renderers are gone from the build. The trap is recorded because it
cost real work — the first pass of the pacing work ran through scenefx/GLES,
whose blur damage path (`apply_blur_region` and the saved-pixels compensation)
is not the one the live session runs, and its damage figures were discarded.
The fixtures carried the dead variable in `HL_ENV` for a while afterwards,
where it read as a renderer pin and was a no-op; it has been removed.

**The nominal refresh is not the observed refresh.** The headless backend's
frame timer is whole milliseconds, so `HL_HZ1=144` free-runs at 1000/6 =
166.7 Hz and `HL_HZ2=60` at 1000/16 = 62.5 Hz. The analyser classifies intervals
against each output's *observed* period for exactly this reason; against the
nominal one it would report a permanent 15% surplus that is an artefact of the
fixture. The two rates still differ by 2.7x, which is all the
refresh-independence question needs.

**Headless present timestamps are not vblank timestamps.** The backend sends
presentation feedback at commit — measured end-of-render to present is 4 µs at
p50 — so effective-refresh collapse (§ the 6.9/13.9/6.9 cadence) is a question
only the live DRM backend can answer. Headless can prove the animation engine is
refresh-independent; it cannot prove the display kept up.

### M5: the two colour paths, and asserting the premise of a premise

```text
contrib/avk-m5-path-a-test.sh   12   decode on sample + _SRGB encode on write
contrib/avk-m5-path-b-test.sh   13   FP16 intermediate + the output-encode pass
./build/test-avk-render          -   both paths on a device, plus PQ vs the
                                     CPU reference at 10 bits
```

Both fixtures render a **wallpaper-only** frame, and that is not a shortcut:
the whole point of either path is to move composition into linear light, so a
*blended* pixel is expected to come out different — that is ADR-005 and it is
the feature. A frame with nothing blended in it must round-trip exactly, so
any difference at all is a defect rather than a judgement call about how much
linear compositing should move a pixel. Both report **0 differing pixels** on
the real scan-out buffer.

`avk-m5-path-b-test.sh` runs the SAME configuration twice before it compares
anything, and asserts those two are identical. Without that control, "the
direct and Path-B frames agree" is also what a capture path that always returns
the same bytes — or always fails the same way — would report.

**Both fixtures run every arm under `ASTEROIDZ_VK_DEBUG=1`, and assert
`validation_enabled` before they assert `validation_errors`.** The counter only
increments from the validation layer's callback, so without the layer it reads
0 whatever the frame did. `avk-m5-path-a-test.sh` asserted it that way for a
whole milestone while Path A attached the scan-out's `_SRGB` view to pipelines
declaring the UNORM twin — twenty VUIDs a run, invisible to every headless
fixture. Most fixtures under `contrib/` that assert `validation_errors` still
never set the variable; that is worth fixing where it matters, but the rule to
carry forward is the general one: **an assertion on a counter must be preceded
by an assertion that the counter can move.**

Path B is reached with `AZ_M5_PATH_B=force`, which puts an output C3 assigned
to Path A onto Path B instead. That is a test instrument and not a setting —
Path A is strictly cheaper where it exists. The force is what makes the
comparison mean anything: there is no pre-M5 10-bit picture for a genuine
Path-B output to be compared against, so the gate has to run on an 8-bit
output that has one.

### M4F.2C.4e: the rest of the transform surface

The four rotations are not the transform surface. `flipped-90` and `90` have the
same extents and different anchors, so a sign error in one is invisible in the
other; the cursor is drawn through its own `wlr_box_transform()` call; and a
transform set in a config file never produces the frame where the pending and
committed transforms disagree.

```text
contrib/avk-transform-test.sh          38   all eight, pixel-exact + hole
contrib/avk-transform-live-test.sh     --   MODE=oracle|pixel|modeset
contrib/avk-cursor-transform-test.sh   --   seven pointer positions x eight
contrib/avk-scale-transform-test.sh    --   1 / 1.25 / 1.5 / 1.75 / 2 x transform
contrib/avk-dither-domain-test.sh      --   which raster the noise is anchored to
contrib/avk-capture-layout-test.sh     --   the readback, at awkward extents
tests/test-extent-space.c              34   attachment vs presentation extent
```

**The oracle elects its own canonicalisation.** `ppm.py verify` maps a capture
back through all eight candidate transforms and requires the expected one to be
the unique zero against the reference. A transcription error in either direction
loses that election — and a fixture too symmetric to tell two candidates apart
fails it as well, which no amount of "0 px" can say on its own.

**Two tests that passed by doing nothing**, both caught here:

```text
the headless backend believes it has a cursor plane. set_cursor() is
`return true;` and does nothing, so no cursor reaches the frame at all --
seven captures with the pointer in seven places were seven identical files,
and every position assertion built on them measured nothing. The whole
cross-transform half of the cursor matrix passed 0 px on frames with no
cursor in them. ASTEROIDZ_AVK_FORCE_SOFTWARE_CURSOR=1 is the fix.

`local a="$1" b="x$a"` declares every name before assigning any of them, so
under `set -u` the second initialiser reads an unset variable. A whole
matrix run died in its first function and reported nothing.
```

**A flat fixture cannot test sampling.** The eight-transform oracle was three
windows of three solid colours, and it reported 0 differing pixels at every
transform while every texture on a 90° or 270° output was being drawn rotated
180° inside its own box. A solid colour rotated by any amount is the same solid
colour. `WLBGEFFECT_QUAD=1` paints four quadrants — still deterministic between
runs, no text and no client-side layout — and the same defect measures 167 400 of
480 000 pixels at 90° and 270° and exactly 0 at the other six transforms, which
are all involutions. It is now the oracle's default; `QUAD=0` restores the flat
fixture, and exists only to reproduce that result.

**A row-pitch bug shears rather than corrupts**, so
`contrib/avk-capture-layout-test.sh` measures a straight vertical edge at the
top, middle and bottom of the frame at 799, 801, 1023 and 1365 pixels wide
rather than trusting the extents. `AZ_AVK_CAPTURE_ROW_SKEW=1` is its falsifier.

**`amsg dispatch dump_scene`** logs one line per emitted AVK command with the
index it landed at. That index is the `k` a blur's source prefix is replayed
for, so scene order becomes a fact about the stream rather than something
inferred from a picture — and a fixture can tell a blur in the wrong PLACE from
a blur with the wrong SOURCE, which no screenshot can.

It is armed rather than scheduled, and that was learned: `AVK_SCENE_DUMP` names
a frame NUMBER, and nothing outside the compositor knows which frame a window
will be on. 5 fired before the client existed; 200 never arrived on an idle
desktop. Both produce an empty dump, which reads exactly like "the walker
emitted nothing".

`test-avk-cmd-uses` needs no GPU: the resolver it exercises is pure. It makes no
synchronisation claim either — Vulkan's validation layers stay the oracle for
barriers, because they see the actual ones. Its whole job is to notice an
omission earlier than a validation run would, and it exists because the first
version of `avk_render_declare_segment()` listed only texture commands and the
blur command that samples its own result was a missing barrier that RENDERED.

Breaks, all four confirmed failing:

```text
AZ_BLUR_SCENE_AFTER=1           blur-scene   16/24   walker 21/23
AZ_BLUR_IGNORE_DARKEN=1         material     27/30   walker 22/23
AZ_BLUR_IGNORE_CLIP=1           material     25/30
AZ_BLUR_EDGE_LOGICAL_SIGMA=1.5  material     29/30
```

`AZ_BLUR_EDGE_LOGICAL_SIGMA` fails exactly one check, and that is correct rather
than weak: it divides the soft edge's sigma back out by the output scale, which
restores the reference's own inconsistency (the reference scales a blur node's
edge softness to output pixels and does NOT scale the shadow's blur sigma). The
only thing that can see it is the check that the two fade over the same
distance — the invariant the feature exists for. It measures 0.3433 of coverage
disagreement, which is a visible ring.

**Three premises in this fixture were false, and each was measured rather than
assumed.** Every one of them produced a plausible-looking pass or a
plausible-looking failure:

```text
only-floating          defaults to 1, so a TILED window has no shadow, hence no
                       backdrop blur, hence no blur node. Measured
                       blur_nodes_seen = 0 and read as a broken walker.

opaque surfaces        client_update_blur() refuses a blur node behind a fully
                       opaque surface -- it could never show its backdrop. kitty
                       is opaque, so the window's OWN blur node (the one that
                       carries a clipped_region) never existed and the clip was
                       being "tested" against nodes that had none.
                       avk.blur_nodes_clipped exists to say so, and read 0.

clip=Nrects over the   a SHADOW's window-shaped hole is the standard
whole dump             two-rectangle cross, so "the most rectangles any clip
                       arrived in" measured 4 and passed the multi-rect
                       assertion. Right answer, wrong command.

>= 1 rectangle         restricted to BLUR lines the number was 1, and the
                       assertion said ">= 1" -- which a BOUNDING BOX satisfies
                       forever. It was stating the producer's collapse as
                       though it were the test's subject. M4F.2B.0 fixed the
                       producer and the assertion now says == 2, so the gap
                       between the client's two rectangles is what is checked.
```

**A blur-enabled default config changed what other fixtures measure.** The
harness's default turns the shadow backdrop blur on and the wallpaper is a layer
surface with a layer shadow, so from the moment the walker honoured blur nodes,
`avk-graph-test.sh` measured 6 passes and 7 barriers while calling it the direct
path. Any fixture asserting on pass or barrier counts has to state that it has
no blur in it, and assert that it has none.

**Comparing two builds needs `HL_ASTEROIDZ`, not `ASTEROIDZ`.**
`contrib/lib/headless.sh` resolves the binary once, at source time:

```sh
HL_ASTEROIDZ="${ASTEROIDZ:-$HL_REPO/build/asteroidz}"
```

so `ASTEROIDZ=<old> hl_start` sets a variable nothing reads again. Two M4E
fixtures did exactly that, both ran the current build twice, and both reported
a clean comparison — a byte-identical framebuffer and a matching cost table,
neither of which said anything about the other binary.

`hl_start` now records what it launched and `hl_binary()` reports it, so a
fixture can assert it got the build it asked for:

```sh
HL_ASTEROIDZ="$PREGRAPH" fixture
hl_assert "the comparison really used the pre-graph binary" \
	"$(hl_binary)" "$PREGRAPH"
```

**A fixture compared against itself is the premise, not a formality.** The
pixel assertion in `avk-graph-test.sh` is self-calibrating with no tolerance
written in:

```text
diff(old, new)  <=  diff(new, new)
```

When the fixture is reproducible the floor is 0 and it demands exact byte
equality. Getting the floor to 0 took three harness fixes, each of which
produced a *failing* test for a correct build:

| cause | cost |
|---|---|
| `HL_SPAWN_COLORS` index is process-global, so a second fixture in one script got different window colours | 3 214 556 of 6 220 800 bytes |
| a terminal's text cursor blinks, so a settled desktop is not a still image | — |
| IPC dispatch names are not the C function names (`toggle_floating`, not `togglefloating`) so the window never floated | ~1000 scattered pixels |

`hl_reset_spawn_colors()` and `HL_KITTY_EXTRA` exist for the first two.

**A premise that only holds with the validation layers off is not a premise.**
`test_pressure()` in the multipass suite first asserted `ring.stalls > 0` as
proof the CPU had outrun the GPU: 224 stalls normally, **0 under validation**,
because the layers slow the CPU enough that the GPU keeps up. It now asserts
volume, and the in-flight case is proved by construction elsewhere — two
acquires in one frame must return different images, and the transient break's
own case releases against a timeline point that is never signalled.

**Break applicability is stated, not assumed.** `graph-missing-write-read` and
`transient-early-reuse` are both listed as *not applicable* to
`avk-graph-test.sh` — the direct path has one pass and no inter-pass edge, and
nothing in production acquires a transient yet. A break that cannot fail a
suite must not be counted as coverage for it.

## XWayland on a fractional-scale output

`contrib/xw-scale-test.sh` asks one question: does an X11 window reach a
1.25x output at its native pixel size, or does the renderer magnify it?

X11 has no notion of a fractional output scale. An X window is configured in
**logical** units, commits a buffer of that many pixels, and the scene presents
it at its buffer size — so on a 1.25x output a fullscreen game renders
1536x864 and is stretched to 1920x1080. `xwayland_force_scale_one` is the
option that stops that; this fixture is how it is measured.

```bash
flock -w 3600 -o /tmp/asteroidz-headless.lock ./contrib/xw-scale-test.sh
```

Three boundaries are asserted separately, because they fail independently:

| boundary | how it is read | what a failure means |
| --- | --- | --- |
| configure | the client's own `configure W H` line | the compositor sized the X window in the wrong units. Needs no renderer, so it fails first and points straight at the configure path |
| presentation | `checker.py verdict` over a region of the capture | the buffer is being magnified on the way to the screen |
| input | the client's own `button X Y` line | a click lands somewhere other than where it was aimed. **This one has no visual symptom** — with presentation fixed and the input transform missing, the picture is perfect and every click is off by the scale factor |

**The click probe is at (900,500) logical and not at any origin or centre.**
`0 x 1.25` is still `0`, so a probe at the window origin passes at every scale
and against every implementation, right or wrong. (900,500) maps to (1125,625)
raw and to nothing else, so a wrong answer has to differ.

**The arms are a premise, a falsifier and the fix.** `scale-1` says only that
the instrument recognises a 1:1 presentation when given one — without it a
`scaled` verdict could equally mean the capture path resamples or the region
missed the window. `1.25-off` is the bug itself, measured, and it stays in the
suite permanently: an oracle whose failing case has stopped failing has stopped
testing. `1.25-on` is the fix.

**Six arms, and each one exists because the others cannot see what it sees.**
`scale-1` and `1.25-off` are the premise and the falsifier described above;
`1.25-on` is the fix. `1.25-tiled` is the only arm with a border, a non-zero
origin and a surface clip that covers less than the whole surface — the clip
is expressed in *surface* coordinates, which for these windows are raw pixels,
while every clip box in the compositor is logical, and left unconverted the
window is cropped to 1/scale of itself. That arm asserts zero desktop pixels
inside the window's content box rather than a `native` verdict, because a
tiled window's edges need not land on whole device pixels and demanding
bit-exactness there would assert more than the option claims. `1.25-anim` and
`1.25-overview` cover the two paths that write the scene buffer's destination
size *themselves*, per frame: a wrong value there wins over the view scale,
and only while something is moving, so a fixture measuring a still desktop
cannot see it. `1.25-screen` and its falsifier cover the X screen itself — see below.

**One pair of arms runs identical probes and expects two different answers.**
Xwayland used to size its X screen from the outputs' *logical* geometry while
this option sized windows in device pixels, so a fullscreen window overflowed
the screen it lived on and X11 clamped the pointer to the root window: at
1.25×, logical (1450,800) arrived at 1535,863 — the screen's last pixel —
instead of 1812,1000. Hiding xdg-output from the Xwayland client gives it a
device-pixel screen and removes that, so `1.25-screen` asserts 1920×1080 and
1812,1000. On its own that arm cannot distinguish a fixed root from a probe
that stopped reaching the overflow band, so `1.25-screen-clamped` re-runs it
verbatim under `AZ_BREAK_X11_ROOT_SIZE=1`, which restores xdg-output and must
reproduce 1536×864 and 1535,863. Both arms keep a green gate at logical
(900,500) → 1125,625, which is correct either way: if that one goes red the
break broke the input path rather than the root size. The arms take their
assertion *labels* from their caller for the same reason — a falsifier
printing "the X screen is sized in device pixels" in green while asserting the
logical desktop would be worse than no label at all.

**Both premises are load-bearing and both have been observed red.** A window
that never painted has no greys, so a gate counting only those calls a blank
black rectangle `native` — the equal-neighbour count is what catches it
(`X11CHECK_BREAK_NOPAINT=1`: `grey=0 dup_h=399500`, gate red). And any region
that misses the window reports `scaled`, including a region of plain
wallpaper, which would let the falsifier arm pass while measuring nothing — so
the fixture repaints the wallpaper pure red and asserts zero red pixels inside
the sampled region. Measured on a region that deliberately missed: 25 600 red
pixels and a `scaled` verdict, exactly the false pass that assertion exists to
stop.

### Two displays, two scales

`contrib/xw-mixed-test.sh` is the same question asked where it can have two
different answers.

With one output there is only one scale, so a per-client scale, a global
scale, the sharpest scale in the layout and "whatever `selmon` is" all produce
the same number — a single-output fixture passes against all four. The layout
here is mismatched in both resolution and scale:

```text
HEADLESS-1   1920x1080 at 1.25   logical 1536x864    layout x = 0
HEADLESS-2   2880x1620 at 1.5    logical 1920x1080   layout x = 1536
```

A fullscreen X window must be configured 1920×1080 on the first and 2880×1620
on the second. Those differ in both axes and neither is derivable from the
other, so an implementation sharing one scale across the layout fails on
whichever output it is not tracking. The fixture moves one window between them
with `tag_monitor` and asserts the configure both times, plus a click on each
(logical 900 into the window is raw 1125 at 1.25×; 600 is 900 at 1.5×) and a
capture of each output.

**The numbers are chosen so that `logical × scale` is a whole number** —
1536 × 1.25 = 1920 and 1920 × 1.5 = 2880 exactly. Not to flatter the feature:
so that the fixture measures monitor *attribution* rather than rounding. A
mode like 2560×1440 at 1.5 has a logical width of 1706.67, which wlroots
rounds to 1707, and 1707 × 1.5 is 2560.5 — the configure comes back a pixel
wide and the assertion fails for a reason that has nothing to do with which
monitor was picked.

**Every geometry assertion is monitor-relative.** The second output starts at
logical x = 1536, so anything written against an origin of 0,0 would pass on
one output and fail here — which is the same trap live multi-monitor runs hit.

**The click probe on the second output is at logical 900, and it used to be at
600 because of a limitation rather than a tuning choice.** That window sits at
X 2304 and is 2880 wide, and the whole X screen used to be only 3456 — so it
ran off the right at local x = 1152 and X11 clamped the pointer to the root
window, reporting 1151 where 1350 was expected. Xwayland now gets a
device-pixel xdg-output of its own, which puts the second output's X zone at
2304..5184 and leaves nothing to clamp against, so the probe sits in the band
that used to be unreachable. It is the assertion no single-output fixture can
make: the second output's zone *origin* has to be right, not just its scale.
`AZ_BREAK_X11_ROOT_SIZE=1` is its falsifier and reproduces 1151 exactly.

`AZ_BREAK_X11_MON_MIGRATE=1` is its falsifier: never re-evaluate the scale when
a window changes monitor, so it keeps the units of the display it opened on.
That is the most plausible way to get this wrong and it is invisible on a
single output. Observed red at 13/17, with the first output still green and
the second reporting a 2400×1350 configure — the second output's logical box
times the *first* output's scale.

**The fixture's own first run was a false negative.** It assumed the window
would open on `HEADLESS-1`; it opened on `HEADLESS-2` — whichever `selmon`
happens to be — so the "on mon1" half measured a window that was never there
and the "moved to mon2" half measured a move that never happened. Both halves
reported on the same monitor while four assertions failed in ways that pointed
at the compositor. The window is now placed explicitly, with a premise
assertion that it really is where the fixture thinks.

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

It needs a session with `ASTEROIDZ_AVK_FORCE_SOFTWARE_CURSOR=1` set at
startup. **That session is no longer installed.** It shipped as
`asteroidz-avk-swcursor.desktop` for M3.5E and was removed once that milestone
closed, along with three other experiment-scoped entries — every one of them
appears in the greeter the operator actually uses, so the list is not free.
Re-running this needs the entry restored to `meson.build` first (see the
session block there) and a logout; `restart` re-execs with the same environ and
cannot add one. There is deliberately no runtime
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
so the harness includes a few small purpose-built clients — all Wayland except
`x11check`, which has to be an X11 client to test the XWayland path at all:

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
- **`contrib/x11check`** — the only **X11** client here, and the only one that
  can say whether a window reached the screen 1:1. It is a raw XCB client that
  fills itself with a **one-pixel checkerboard** and reports, on stdout, every
  `configure` the compositor sends it (in X11's own units, which are raw
  pixels) and the window-local coordinates of every button press.

  Both halves matter and neither substitutes for the other. The `configure`
  line covers the size boundary with no renderer involved at all; the
  checkerboard covers presentation, because a magnified *flat* colour is the
  same flat colour and any fixture built on ordinary window content is blind to
  scaling in the window's interior. The pattern is read back by
  `contrib/lib/checker.py`, which counts **greys** (a bilinear tap between
  black and white) and **equal neighbours** (a nearest tap repeating a source
  texel) — one for each of the two ways a magnified window can look, so
  neither filter mode can pass as native. `X11CHECK_BREAK_NOPAINT=1` leaves the
  window unpainted, which is how a fixture shows its pixel gate is about the
  content and not merely about a window existing. Use `hl_spawn_x11check`,
  `hl_x11check_last_configure`, `hl_x11check_last_button` and `hl_xdisplay`.
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

### M4C live acceptance — 2026-08-12, AVK, 433ea52

Installed binary verified against HEAD *before* anything was claimed: the build
was reporting `3236ce3` because the commit hash is baked at **configure** time,
so `meson setup --reconfigure` was required first. Without it the installed
binary would have carried a stale hash and "live-tested at HEAD" would have
been false.

Gradient borders were enabled with `amsg dispatch set_option,border_gradient,1`
— **memory only, discarded at the next reload**, so nothing was written to the
user's config. The script that did it sampled RSS and the frame counter every
second and would have reverted itself on >50 MB of growth or a runaway frame
rate, rather than depending on someone noticing.

**The former repaint storm, on the real desktop:**

```text
                    old build (measured earlier)   433ea52
RSS over 12 s       +54 MB/s (~650 MB)             flat, +516 kB once
amsg round trip     20 s timeout                   0.001 s
frames/s            unbounded                      14 (ON) vs 17 (OFF) vs 16
```

The ON-vs-OFF comparison is the one that matters: **enabling the gradient does
not change the frame rate**. The ~15/s is the desktop's own activity — a bar
clock, the cursor, clients repainting — and had it not been measured both ways,
"15 frames/s with gradients on" could have been read as a residual storm.

Invariants over the session: `cpu_sync_waits`, `present_sync_failures`,
`target_state_violations`, `lifecycle_violations`, `fallback_frames`,
`dmabuf_import_failures` — **all 0**. 961 gradient draws, 1922 colours, **0
buffer growths**, 60 KB uploaded in total, RSS +628 kB.

Visual acceptance was the user's, on DP-1 @ scale 1.5 and HDMI-A-1 @ scale 1.0.
Confirmed: **the gradients are visible and rendering, and the behaviour is
healthy** — no stutter, no lag, nothing that felt like the old storm. That is an
observation, not a measurement, and is recorded as one.

**Cross-output continuity was confirmed separately**, and asked for separately
rather than folded into the general "looks good" — reading a specific result
into a general one is how a claim gets made that nobody actually checked. The
user performed the seam drag on the real desk, floating a window across the
DP-1 / HDMI-A-1 boundary with a gradient border, and reported it behaving as
expected: one unbroken ramp through the seam, no restart, no corner where the
outputs meet.

That agrees with the headless fixture, which is the numeric half of the same
claim — `avk-gradient-crossoutput-test.sh`: 939 samples along the border,
median step 0, max step 2, end-to-end span 510.

**What live acceptance CANNOT cover, and why.** The compositor only ever
creates two-stop linear interpolated borders and five-stop vignettes. There is
no configuration that produces a conic gradient, a banded one, or an off-centre
origin, so those three rest entirely on the CPU oracle in
`tests/test-avk-gradient.c`. That is a property of the compositor's feature set,
not a gap in the testing.

## M4D live acceptance (e775b26, 2026-08-12)

Installed binary `0.24.0(e775b26)`, matching the tested implementation commit
exactly; the compositor was restarted into it (process start 10:46:05 against
an install at 10:45:05).

**Shadows accepted** on the first live run, with `blur-background` skipped —
that is an M4F feature and the user's judgement was explicitly conditioned on
it: "the blur part is skipped, in that case the shadows are accepted".

**Anti-banding accepted** on the second. The reported defect was concentric
rings around every window on a flat grey settings panel, measured from the
user's own screenshot at 9 output codes across 130 px in runs of
41,27,15,11,11,9,8,3,2 px. After M4D.4: "very good".

Both are recorded as **observations**, not measurements. The pixel measurements
are `tests/test-avk-shadow.c` (82 checks) and `contrib/avk-shadow-test.sh`
(28); what the live session contributes is that the thing on screen is the
thing the fixtures describe.

Live state at acceptance:

```text
gpu_frame_us_avg        122.6   over 1729 samples, 0 dropped
                                = 1.8% of DP-1's 6944us budget (3840x2160@144)
IPC round trip          1-2 ms
RSS                     201 MB
shadow_draws            1677, all rounded, 8 asymmetric
cpu_sync_waits          0       present_sync_failures   0
target_state_violations 0       lifecycle_violations    0
fallback_frames         0       dmabuf_import_failures  0
```

The user's own profile — `size 72`, `blur 72`, `position y 18`,
`color 0x00000050`, contact `size 2 / blur 3` — is much broader than the
shipped default the fixtures use, so the live run exercised a shape no
deterministic test covers.

**Not claimed**: an idle frame delta. The live desktop shows 82 frames over 5
idle seconds, which is a bar clock and a cursor rather than a shadow repaint
storm; the shadow-specific idle convergence is asserted headlessly, where
nothing else is drawing (delta 0).

## M4E live acceptance (6dfcf66, 2026-08-12)

**VERDICT: ACCEPTED.** *"nothing looks different, response is good, no issues
found"* — which is the intended result. M4E moved where barriers are decided and
added a resource pool nothing acquires from yet; it changes no pixel and no
timing a person can perceive, and a visible difference would have been a defect
rather than a feature.

This is an OBSERVATION, not a measurement. The measurements are below.

```text
HEAD       6dfcf66      tree clean
installed  /usr/bin/asteroidz 0.24.0(6dfcf66)
           md5 331eb9f4a411b5c0aaf228b249b05545 == build/asteroidz
binary installed 12:28:05   session started 12:32:38   -> the session IS the tested build
```

Two restarts. The first ran `d03cc75`, whose `graph_build_ns` still folded the
barrier calls into the graph's own cost and read 42.5 µs; `6dfcf66` splits them.

### Topology and invariants

```text
graph_passes              1        graph_barriers            2
graph_resources           4        graph_image_transitions   4
graph_uses                4        graph_buffer_barriers     0
graph_allocs              5   (stopped rising)

cpu_sync_waits            0        present_sync_failures     0
target_state_violations   0        lifecycle_violations      0
fallback_frames           0        dmabuf_import_failures    0
present_sync_none         0        late_imports              0
cursor_no_image           0        gpu_dropped               0 of 1371 samples

transient_acquires        0   (nothing acquires until M4F)
transient_unsafe_reuses   0
```

### Cost, incremental over 250 frames (startup excluded)

```text
graph_build_ns        2 820      what M4E added          1.0% of frame recording
graph_barrier_ns     45 147      vkCmdPipelineBarrier2 x2, pre-existing
record_ns           268 564      whole frame recording   3.9% of a 6944 us budget

gpu_frame_us_avg      162.6      1371 samples, 0 dropped
cpu_frame_us          p50 240   p95 500   p99 540
damage_ratio          0.376      RSS 201 MB
```

### Validation, live

`ASTEROIDZ_VK_DEBUG=1` is set in this session, so the **validation layers are
loaded**, and the current session's log contains **0 VUID, 0 SYNC-HAZARD, 0
ERROR and 0 WARN**. That is stronger than the headless result: validation
cleanliness on the real KMS path, with real client dma-bufs and real scan-out.

> **Fixed at the source.** The log no longer accumulates: each boot renames
> the previous session to `asteroidz.log.old` and truncates, so the live file
> is one session by construction. The scoping below is no longer needed for a
> live log, and is kept because it still applies to `.old`, to a log copied off
> a machine, and to any harness log a run appends to. The failure it describes
> happened twice.

The log used to **accumulate across sessions** — `~/.local/state/asteroidz/
asteroidz.log` is where stderr is dup2'd, and its timestamps are per-session
uptime. Grepping the whole file found 6 VUID hits from an older run and briefly
looked like a regression. Scope to the last `renderer ready for VkFormat` line:

```sh
L=~/.local/state/asteroidz/asteroidz.log
tail -n +"$(grep -an 'renderer ready for VkFormat' "$L" | tail -1 | cut -d: -f1)" "$L" \
  | grep -acE 'VUID|SYNC-HAZARD'
```

### 45 µs of barriers — INVALIDATED MEASUREMENT

> **This entire subsection records a measurement that was wrong.** It is kept
> for the reasoning, not for the number. M4F.3/.5 measured
> `vkCmdPipelineBarrier2` directly, one variable at a time:
>
> ```text
> 1 barrier / 1 call      69 ns        DCC modifier      46 ns
> 4 barriers / 1 call    198 ns        non-DCC           46 ns
> 4 barriers / 4 calls   256 ns        ratio           0.99x
> FOREIGN -> graphics     44 ns        (vs 51 ns without)
> ```
>
> **DCC HYPOTHESIS: DISPROVED.** The modifier tested is the exact one this
> session scans out.
>
> **The 44 µs was measurement contamination**, not Vulkan barrier execution
> cost: a per-call `clock_gettime` pair costs ~37 ns around a ~60 ns operation,
> so a scheduler slice landing in that window is charged entirely to it and the
> *mean* becomes a measure of preemption. Nothing below describes a real driver
> cost.
>
> The only transition with materially higher CPU recording cost in the
> controlled test is `UNDEFINED -> COLOR_ATTACHMENT` at **~311 ns** — still
> sub-microsecond, and not a reason to restructure anything.

### The original (invalidated) reasoning

`vkCmdPipelineBarrier2` costs **45 µs live** against **1.9 µs headless at the
same 3840x2160**. Three explanations were tested and two were eliminated:

| hypothesis | test | result |
|---|---|---|
| debug labels | read `avk_debug.c` | no-ops on a NULL pointer; and they are cheap even when live |
| validation layer overhead | headless 4K **with** `ASTEROIDZ_VK_DEBUG=1` | 1859 ns — indistinguishable from without. **Eliminated** |
| DCC-compressed scan-out | compared allocator modifiers | see below — **leading explanation** |

```text
live scan-out    XR24  GFX11,256KB_R_X,...,DCC,DCC_RETILE,DCC_INDEPENDENT_64B/128B
headless target  XR24  GFX10_RBPLUS,64KB_R_X,PIPE_XOR_BITS=2,PACKERS=0     no DCC
```

A queue-family ownership transfer (`FOREIGN` ↔ graphics) on a DCC-compressed
image makes the driver record decompression and metadata handling; on a
non-compressed one it is nearly free. Circumstantial rather than proven — it was
not isolated by forcing a non-DCC live modifier — so it is recorded as the
leading explanation, not as fact.

> **DISPROVED in M4F.3/.5.** `tests/test-avk-barrier-cost.c` measured the exact
> live modifier `0x0200000028a37f04` (DCC, `DCC_RETILE`, three planes) at
> **46 ns**, against **46 ns** for the same-size non-DCC modifier — ratio 0.99×.
> The foreign queue-family transfer is free too (44 ns vs 51 ns without).
>
> **The 44 µs was the instrument, not the call.** A bracketing `clock_gettime`
> pair costs ~37 ns around a ~60 ns event, so any scheduler slice landing inside
> that window is charged entirely to it; on a loaded desktop the *mean* of such
> a sample measures preemption. The same technique in a quiet loop reads
> p50 60 / p95 70 / mean 61 ns.
>
> Per-call barrier timing has been removed and `graph_build_ns` is now reported
> as a distribution. See `docs/avk-effects.md`, "M4F.3/.5 — what
> `vkCmdPipelineBarrier2` actually costs".

**It is not attributable to M4E either way.** The pre-graph renderer made the
same two calls, in the same places, with the same contents; the headless
comparison against a `b9d7115` build is byte-identical with no measurable cost
change. What M4E did was make the cost visible for the first time.

**Relevant to M4F**, which adds passes and therefore barriers: at 45 µs a call
on this hardware, a blur that adds four barrier flushes costs ~90 µs of
recording before it draws anything. Worth measuring there rather than assuming.

> **Measured in M4F, and the concern was unfounded.** Four flushes cost
> **~250 ns** of recording, not ~90 µs. Barriers are not a constraint on the
> blur's design.

### Confirmed over a longer run

Re-read on the same session at **8 349 frames**, six times the sample above:

```text
graph_build_ns        2 645  (was 2 820)      graph_allocs   6, stable
graph_barrier_ns     44 439  (was 45 147)     graph_passes   1
record_ns           263 091  (was 268 564)    graph_barriers 2

gpu_dropped   0 of 8348 samples       cpu_frame_us  p50 240  p95 500
all six invariants 0                  VUID/SYNC-HAZARD 0     ERROR/WARN 0
RSS 201 MB -- unchanged from the 1372-frame reading
```

Every figure reproduces within noise, and **RSS is identical over six times the
frames**, which is the leak signal that matters for a pool and three arenas that
are reset rather than freed. `graph_allocs` moved 5 → 6 once, when the scene
gained a resource it had not seen before, and then stopped — which is the shape
the flat-array design promises: bounded growth to the scene's demand, not
per-frame churn.

## Reproducing an SHM upload stall without the client that caused it

The stall class that produced "libinput: event processing lagging behind by
47ms" comes from one shape: a client committing a very large `wl_shm` buffer,
with a fresh buffer identity every generation. A 56 MB block streams at about
1 GB/s because it blows every cache level, so the copy takes ~50 ms, and a
frame that waits for it stalls for three vblanks.

The browser that caused it cannot be relied on to reproduce it. A freshly
started Firefox on this machine renders through **dmabuf** — `shm_commits`
stayed at 0 across 3056 frames — and only emits 56 MB shm buffers once it has
fallen into a software-rendering path. Vivaldi does the same thing. Waiting
for that state to recur is not a test.

`contrib/wlrepaint` reproduces it deliberately, live or headless:

```sh
wlrepaint --title big --size 5200x2800 --solid 3060a0 \
          --frames 1500 --hold-ms 0 --churn
```

`--churn` is the load-bearing flag: it gives every generation a **new**
`wl_buffer`, which is what leaves a surface with no previously-uploaded buffer
to fall back on. Without it the compositor caches by buffer identity and warms
up after two generations. `--size` has to be large enough that one copy
outlasts a frame; at 2560x1360 the copy takes ~7 ms and the worker always wins,
which makes the test pass by never entering the code under test.

What to read afterwards, and what each answer means:

| | |
|---|---|
| `shm_worker_pack_us_max` ≈ 50000 | the premise: copies really are that slow |
| `shm_async_join_waits` | frames that blocked. Should be 0 |
| `shm_stale_frames` | frames that drew a previous generation instead |
| `handler_over_30ms` | stalls the user would have felt |

**Assert the premise.** A run where `shm_worker_pack_us_max` is small never
provoked anything, and every assertion below it passes for free.

## A late copy owes a repaint to every output it was drawn on

`avk-stale-multioutput-test.sh`, and three ways it was green while testing
nothing. All three were caught by assertions, not by inspection, which is the
only reason the fixture is worth keeping.

When a `wl_shm` client's copy has not finished, AVK draws the newest generation
it already holds and records that the frame owes a repaint once the copy lands.
The debt lived in `entry->stale_output`, **one** `wlr_output *`. A surface
straddling a monitor edge draws stale on both outputs in the same frame cycle,
so the second draw overwrote the first and one output was never repainted. Its
pixels stayed as they were indefinitely — until something unrelated damaged
them. Live, that was a wallpaper reappearing on one monitor after a wallpaper
change and staying there until a window was closed over it.

The symptom pointed away from the cause for most of a session. Every stale draw
in the trace read `0 behind`: the content was always the correct generation, so
every content-side theory was a dead end. What named it was "closing a window
fixes it" — meaning the pixels were right and simply never asked for again.

### The two hooks it needs

| | |
|---|---|
| `AZ_AVK_SLOW_UPLOAD_US` | stall the upload worker this many microseconds after each pack, capped at 100000. The mirror of `AZ_AVK_NO_STALE`: that one forbids the stale path, this one guarantees it. **Not a break** — it makes a real condition occur on demand |
| `AZ_BREAK_STALE_ONE_OUTPUT` | the falsifier. Restores single-output payment. Must drive `shm_stale_multi_output_repaints` to 0 while the premises stay true |

### Two hooks for the copy itself

Both are diagnostics rather than breaks, and both exist because the copy that
was 13x too slow looked exactly like a copy that was the right speed on a busy
machine.

| | |
|---|---|
| `AZ_AVK_PACK_CONTROL` | after any pack that trips the slow-pack log, time three more copies of the same size on the same thread at the same instant: heap→heap, client shm→heap, heap→staging. The first says whether the MOMENT is slow; the other two say which MAPPING is. This is what showed 14.5GB/s and 17.6GB/s for the two halves of a copy that ran at 1.08GB/s together — each mapping fine, the pairing not — which is the signature of first-touch faults rather than bandwidth. It re-runs the real pack afterwards, so the frame stays correct |
| `AZ_AVK_PREFAULT_STAGING` | write every page of a staging buffer at allocation instead of leaving the first copy to fault them. Not a fix — it only moves 52ms from the worker thread to the event loop — but it is the proof: with it set, every slow pack disappears. Keep it as the falsifier for the warm-buffer cache: if that cache regresses, this switch masks the symptom, which is how you tell this cause from any other |

### The two hooks for not copying it at all

| | |
|---|---|
| `AZ_AVK_NO_VISIBLE_CLIP` | the control arm. Nothing is clipped to visibility and every copy is the size it always was. Both `avk-visible-clip-test.sh` cohorts run with and without it, because "we saved bytes" needs something to be smaller than |
| `AZ_AVK_REFUSE_UNDEFINED_PARTIAL` | refuse every clipped FIRST copy, which is what the copy path did before it was told when there is nothing to preserve. A churning client then saves nothing and is not drawn |
| `AZ_AVK_NO_VISIBLE_REPAIR` | **the break that matters.** Clip the copies and then decline to repair what the clip got wrong. Every cost assertion still passes — fewer bytes, the same counters — and a window that becomes visible shows whatever its image was allocated with. A suite that cannot tell this build from the real one is asserting on cost and calling it correctness |

The counters are `visible_clipped`, `visible_saved_px`, `plan_no_hint`, and
`visible_repairs` / `visible_repair_px` / `visible_repair_deferred` /
`visible_repair_failed`. `visible_saved_px` is the whole claim; the repair
counters are what it costs when the hint was too small. A build where repairs
approach the number of frames has a hint that is not working, however good the
saving looks, and `visible_repair_failed` above zero means a texel was drawn
that nothing ever copied.

A failure is split three ways, because the three need different answers and one
number for all of them is a number nobody can act on:

| | |
|---|---|
| `visible_repair_unreadable` | wlroots had taken the client's buffer back; the pixels exist only in a texture this renderer may not touch. Nothing on this path can fix it |
| `visible_repair_nothing` | the plan said there was nothing to copy while the visibility pass said otherwise. The two disagree, which is a bug in one of them |
| `visible_repair_short` | the copy happened and did not cover the shortfall. Likewise |

The breakdown is what found the bug. Every live failure was `repair_short`, and
the one-line description the first of them now prints named it in one field:
`submit=2`. `submit_copy()` refuses a partial write into an image whose layout
is still `UNDEFINED`, and a client that allocates a fresh `wl_buffer` per redraw
has a brand-new image on every one — so every clipped first copy was refused.
The plan now says when there is nothing outside the rectangles to lose.

`BREAK=refuse` (`AZ_AVK_REFUSE_UNDEFINED_PARTIAL=1`) is that build in one
switch, and it is the arm to read if this ever regresses:

| | clipped | refused |
|---|---|---|
| bytes copied | 122.9 MB | **60.0 MB** |
| submit failures | 0 | 59 |
| wallpaper visible | 208896 px | **921600 px** |

The broken build copies half as much and the window is **not on screen at all**.
A cohort asserting only on bytes would have called that an improvement, which is
why the last row is an assertion and not a note.

### Three vacuous versions, and what each assertion said

1. **A copy that never went late.** 1600x1000 packs in under a millisecond, so
   1202 commits produced **0 stale frames** and the claim was green on a scene
   that never reached the code. The premise assertion is what said so, and
   `AZ_AVK_SLOW_UPLOAD_US` is the answer — not a bigger window, which only
   changes the odds.
2. **Placement dispatches that silently did nothing.** The fixture said
   `togglefloating` and `move:X,Y`. Neither exists: the IPC names are
   `toggle_floating` and `move_window,<x>,<y>`, and `hl_dispatch` swallows an
   unknown name without a word. The window never left its tile, so 624 stale
   draws produced a debt on one output because there only ever *was* one
   output. Fixed by reading the geometry back and asserting it straddles the
   seam — `amsg` dispatch names are not C function names, and this project has
   now lost time to that twice.
3. **A loader stealing the placement.** `move_window` acts on the focused
   window, so spawning the load clients before positioning the spanner would
   place a loader instead. The spanner is positioned first, deliberately.

With all three fixed: 1250 stale frames, 207 multi-output repaints, window
spanning 120..2040 across a seam at 1920. Under the break, on the same build and
the same scene: 1175 stale frames, **0** multi-output repaints, and the ordinary
assertion goes red. That difference is the coverage.
