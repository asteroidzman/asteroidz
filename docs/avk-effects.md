# AVK effects — the M4 audit

What SceneFX renders today, what AVK does with it now, and what M4 owes each
one. Traced from the code and the shaders, not from documentation: several
entries below contradict what the names suggest.

The architectural split M4 preserves:

```text
SceneFX / scene graph      decides WHAT effect exists
AVK                        decides HOW it is rendered
```

## Where AVK stands today

Four gaps, all in `az_avk_emit_node()` (`src/render/az_avk.h`):

| gap | current behaviour | line |
|---|---|---|
| shadow, blur, optimized blur | node types recognised and **skipped**, one warning per session | ~1687 |
| gradients | detected, drawn as **the first colour only**, one warning | ~1585 |
| rounded corners | `corners` never read on any node type | — |
| border inner edge | cut out as a **square** pixman subtraction, not a rounded one | ~1569 |

`az_avk_output_supported()` does **not** reject a frame for having effects — so
effects are dropped per node, not per output, and a desktop with shadows
configured renders silently without them.

The border case deserves its history. A window border is not four rectangles:
SceneFX draws it as ONE filled rect with the window's interior clipped out.
Ignoring that clip does not produce a subtly wrong border — the border sits
above the surface, so every window renders as a flat block of border colour.
It was the first thing AVK got visibly wrong on a real desktop and it looked
like a texture bug for far longer than it should have. `AVK_NO_BORDER_CLIP=1`
restores it deliberately.

## The reference shaders

`subprojects/asteroidz-scenefx/render/fx_renderer/shaders/`

```text
quad.frag              solid fill
quad_round.frag        solid fill, rounded
quad_grad.frag         gradient fill
quad_grad_round.frag   gradient fill, rounded
tex.frag               texture
tex_soft_edge.frag     texture with a soft edge
corner_alpha.frag      corner coverage as alpha
box_shadow.frag        analytic box shadow
blur1.frag             dual-Kawase down
blur2.frag             dual-Kawase up
gradient.frag          gradient evaluation
color_transform.frag   M5's, not M4's
```

The pairing (`quad` / `quad_round`, `quad_grad` / `quad_grad_round`) is the
shape M4's pipelines should keep: a small number of coherent pipelines rather
than one shader branching on every effect.

## Per-effect audit

### Rounded corners

**State.** `struct fx_corner_radii { uint16_t top_left, top_right,
bottom_right, bottom_left; }` (`fx/clipped_region.h`), `CORNER_RADIUS_MAX =
UINT16_MAX`. Present on `wlr_scene_rect.corners`, `wlr_scene_buffer.corners`
(set via `wlr_scene_buffer_set_corner_radius/radii`), `wlr_scene_shadow`, and
`wlr_scene_blur`.

**Per-corner, not one radius.** Anything that stores a single `int radius` will
be wrong for asteroidz's titlebar-joined windows. Note `wlr_scene_shadow` has
BOTH a legacy `int corner_radius` and an appended `corners` — the appended one
is authoritative.

**Coordinate space.** Radii are logical; the output scale (1.5 on DP-1) must be
applied where the destination box is computed, not baked into the config value.

**Damage.** Must NOT expand damage. Rounding removes coverage, it never adds
any. A rounded window must keep D.1 partial damage exactly.

**AVK status.** Not implemented. M4A.

### Borders

**State.** A `wlr_scene_rect` whose `clipped_region.area` is the window's
interior, plus `corners`. Colour is **premultiplied** in `wlr_scene_rect.color`
— AVK's command takes a straight colour and premultiplies itself, so the value
is divided by alpha on the way in. Getting that backwards darkens every
translucent panel in the overview by exactly its own alpha, which reads as a
theming choice rather than a bug.

**AVK status.** Geometry correct via clip subtraction; the inner edge is square
where it should be rounded. M4B. **Reproduced in pixels** — see the border mask
audit below.

#### The border mask (M4B: fixed)

Three masks, and the defect was entirely in the second one.

```text
        BEFORE (M4A, defective)                    NOW (M4B)
OUTER   cmd->dst, rounded by cmd->corners   (SDF)  unchanged
INNER   clipped_region.area, a SQUARE    (pixman)  .area  (pixman, shrunk)
                                                 + .corners        (SDF)
CLIENT  the surface, rounded by its own corners (SDF)  unchanged

BORDER = OUTER - INNER
```

The fix is that AVK now reads `clipped_region.corners`, carries them on the
command as `inner`/`inner_corners`, and the fragment shader evaluates

```text
border_coverage = outer_coverage * (1 - inner_coverage)
```

as one primitive — `az_rounded_coverage(..., is_cutout = true)` being that
complement. The scissor region keeps only the part it can express exactly: a
box shrunk on each edge by `ceil(0.3 * radius) + 1`, SceneFX's own rule from
`apply_clip_region()`, rounded up. Rounding it down is not a rounding
preference — a box inset by `k` has its corner `(r-k)*sqrt(2)` from the arc
centre, so staying inside the hole needs `k >= 0.2929r`; truncating `0.3*5` to
`1` puts that corner at 5.66 from a centre 5 away and pixman deletes a pixel
the arc wanted.

The whole of the old text below is kept because it is the record of what the
artifact was, and the fixture that failed by design now passes.

`az_avk.h`'s rect case reads `rect->clipped_region.area` and **ignores
`rect->clipped_region.corners`**, which is a `struct fx_corner_radii` sitting
right beside it in `struct clipped_region`. `apply_border()` fills it in with
`border_radius - bw - 1` per corner, deliberately 1px tighter than the content
arc so the seam between two independently rasterised arcs lands on border paint
rather than on the wallpaper. AVK never asks.

The visible consequence, on the diagonal of a corner: the square cut removes
the client's whole interior box, including the part of it the CLIENT's own arc
has already cut away — so between the border's outer arc and the client's inner
arc, nobody paints, and whatever is behind the window shows through. It is a
wedge, it is inside the rounded corner, and it flickers because what is behind
it is live content.

Measured at `radius 40, borderpx 6` on a 700x500 window, per corner:

```text
AVK    104 background pixels inside the outer arc
GLES     0
```

Unchanged under `AZ_AVK_FULL_DAMAGE=1`, and every one of those pixels showed the
CURRENT background generation, so it was coverage geometry and not damage. The
fixture is `contrib/avk-rounded-persist-test.sh BORDER=6`; it now passes, and
`BREAK=border-square-inner` restores exactly the 104-pixel signature.

#### The undefined derivative M4B uncovered

`az_rounded_coverage()` computed `fwidth(dist)` **after** a per-pixel early
return for fragments outside the rectangle. `fwidth` is a 2x2-quad derivative
and is undefined if any invocation in that quad skipped the value, so along the
edge of every primitive some invocations took the branch and the survivors read
a derivative their neighbours never produced.

It rendered correctly for the whole of M4A. Calling the same function a SECOND
time for the inner edge changed the shader's layout enough to turn the garbage
derivative into a real `aa`, which fed `smoothstep()` and painted a one-pixel
column of border down the OUTSIDE of a corner, where the arc had long since
curved away — visible at `radius 9` and at `radius 40, scale 1.5`, absent at
`radius 40, scale 1`, and varying run to run on identical input. That variance
is the signature; a geometry bug does not move.

The early return was only an optimisation, and a redundant one: the expression
is a true signed distance, so it already reports "outside" for every point
outside. It is gone. The all-radii-zero test stays, because a push constant is
uniform across the draw and that branch cannot split a quad.

Anything added to this shader later must keep every path to `fwidth()` free of
per-pixel branching.

### Gradients

**State.** Fork extension on `wlr_scene_rect`: `has_gradient`,
`gradient_degree`, `gradient_linear` (1 = linear, 2 = conic), `gradient_blend`,
`gradient_origin[2]`, `gradient_count`, `gradient_colors` (**4 floats per
colour**, owned by the node). The range always covers the rect's box.

**AVK status.** First colour only. Note the existing limitation is not "two
stops" as the M4 brief supposed — it is *one*. M4C.

### Shadows

**State.** `wlr_scene_shadow { width, height, corner_radius, color[4],
blur_sigma, clipped_region, corners }`. `box_shadow.frag` is **analytic** —
a signed-distance box with a gaussian falloff, not a blurred texture. There is
no separate offset or spread field: the node is positioned and sized by the
compositor, so "offset" and "spread" are expressed as node geometry, and
`blur_sigma` is the falloff width.

**Damage.** The only effect so far that expands it. Footprint is the node box
grown by the gaussian's reach.

**AVK status.** Skipped. M4D.

### Blur

The most stateful effect by a wide margin, and most of the state exists because
of bugs found on a real desktop.

**Global parameters** — `struct blur_data` (`fx/blur_data.h`), on the scene:
`num_passes`, `radius`, `noise`, `brightness`, `contrast`, `saturation`,
`transparency_threshold` ("alpha below/at which surface pixels don't count for
the blur transparency mask"). `blur_data_apply_strength()` scales them per node.

**Per-node** — `wlr_scene_blur`:

| field | meaning |
|---|---|
| `strength`, `alpha` | per-node scaling of the global parameters |
| `corners`, `clipped_region` | the region's own rounded shape |
| `should_only_blur_bottom_layer` | the optimized-blur path |
| `has_sample_exclude`, `sample_exclude` | a box whose contents must NOT reach the blur's **source** |
| `darken` | clamp the result against its own unblurred source |
| `transparency_mask_source` | which node supplies the transparency mask |
| `has_clip_region`, `clip_region` | pixel-accurate clip — this is `ext-background-effect-v1` |
| `edge_softness` | 0 = hard rounded-rect cutout with ~1px AA; > 0 = wide analytic gaussian falloff like a shadow's |

**`sample_exclude` and `darken` are not optional polish.** Both were added to
fix shipped visual bugs and the comments record why. A shadow's backdrop blur
covers the window itself, and the scene image holds the *previous* frame there
because the window is drawn after the blur node and undamaged areas are never
re-rendered — so the blur picked up the window's own pixels and spread them
outward as a halo in the window's own colour. `darken` exists because a blur is
an average, and averaging bright detail over a dark ground raises the mean, so
a shadow's backdrop over a terminal read as a glow. **A test on a flat-colour
backdrop is blind to both** — a blur of a flat field is the same flat field.
Use high-frequency content.

**`wlr_scene_optimized_blur`** is separate: `{ width, height, dirty }`, the
cached bottom-layer blur.

**Shaders.** `blur1.frag` / `blur2.frag`, dual-Kawase down/up. Pass count is
`num_passes`, not a fixed number.

**Damage.** The hard one. Output damage must expand by the kernel's dependency
radius to find the *source* region, which is a different quantity from render
damage. M4F.

**AVK status.** Skipped entirely.

### Opacity, clipping, transforms

Already correct in AVK: `wlr_scene_buffer.opacity` is honoured, clips are
scissored per command, and buffer transform is composed with the output's
(`wlr_output_transform_invert(buf->transform)` then the output's). M4 must not
regress these while adding effects on top.

## Ordering

Derived from scene order, which is what SceneFX renders bottom-to-top:

```text
shadow node          (behind the window)
blur node            (backdrop, samples what is below it)
client surface       (rounded via the buffer's own corners)
border rect          (above the surface, interior clipped out)
```

M4 must reproduce this rather than inherit whatever order command insertion
happens to produce.

## What M4 owes, by stage

```text
M4A  rounded clipping     per-corner radii, scale-aware, no damage growth
M4B  borders              rounded inner edge, premultiplied colour preserved
M4C  gradients            arbitrary stops (today: one), linear + conic
M4D  shadows              analytic, and the first damage expansion
M4E  render graph         transient pool, derived synchronisation
M4F  blur                 dual-Kawase, source-region expansion, and every one
                          of the per-node fields above
M4G  interactions         ordering, alpha, and the combinations
```

## Rejected: the opaque-region mismatch (do not re-chase)

**Hypothesis.** AVK normalises radii (clamping to half the box, CSS-style edge
scaling) while SceneFX computes the opaque region from the unnormalised ones, so
a corner pixel could be claimed opaque, skipped by damage, and left stale.

**Rejected at the source.** `create_corner_location_region()`
(`wlr_scene.c:305`) subtracts a full r-by-r **square** at each corner from the
opaque region, on both the buffer path (line 384) and the rect path (line 347).
AVK's visible coverage in that corner is a quarter-disc strictly inside that
square, and normalisation only ever SHRINKS a radius. So:

```text
SceneFX's opaque claim  ⊆  AVK's actual coverage
opaque_but_not_rendered =  0, by construction
```

The disagreement runs the safe way — the background is repainted more often
than strictly necessary, never less — and cannot strand a pixel. Confirmed
empirically afterwards: at `radius 40` with a continuously repainting
background, zero stale and zero unpainted pixels in any corner, on both
renderers.

This one was believable enough to survive a day of instrumentation. Written
down so it does not get rediscovered.

## M4A test-harness traps (do not repeat)

**`border_radius_location_default` cannot force asymmetric client radii.** It
is an integer bitmask read with `atoi()` and is not in the schema, so a name
like `bottom` parses as 0 and the default silently stands. A test built on it
reported the compositor's CORRECT titlebar behaviour as a per-corner bug.
Asymmetry comes from `set_client_corner_location()` and is driven by layout:
a titlebar'd window clear of the screen edges gives `0, r, r, r`.

**A boolean "round or square" probe cannot detect double-scaling.** "Is it
round?" is equally true of a 28px arc and a 63px one, so
`BREAK=rounded-double-scale` passed at BOTH scales until the probe started
measuring. The cut corner has area `r²(1 − π/4)`, so counting background pixels
in a corner box recovers `r` without having to find the arc. Use the RATIO
across scales as the invariant: the estimator undercounts by ~14% because the
antialiased boundary is not background, and that is the estimator being
approximate rather than the renderer being wrong.

**A test client with no border makes both renderers look broken.** A window
whose client never binds `xdg-decoration` is CSD, so `check_hit_no_border()`
gives it no border while `apply_border()` still insets its surface by
`borderpx` — the window looks bordered and the ring is empty background. The
first border-isolation run measured that on BOTH engines and was one step from
being reported as a shared SceneFX border defect. `wlrepaint --ssd`, and the
fixture now asserts a border is actually painted before judging one.

**Two more, both of which produced believable wrong conclusions:** the window's
top corners belong to the TITLEBAR node, not the client, so a probe there reads
the wrong node; and sampling just above the client box hits the titlebar, which
made a correct renderer look like it was painting 90 stray pixels outside its
destination rectangle.

## Traps already paid for

- **Flat-colour test scenes are blind to blur bugs.** Both `sample_exclude` and
  `darken` were invisible to every flat-backdrop scene in the suite; the tests
  that found them use fine bright lines on black.
- **Premultiplied vs straight colour** darkens translucent panels by exactly
  their own alpha and looks deliberate.
- **A test whose right and wrong answers coincide on this GPU is not
  coverage.** Mixed scales and high-frequency content are what make effect
  bugs observable; see `docs/regression-testing.md`.

## M4B.1 — a decoration is a statement about where its client is

**Not an AVK defect.** It predates M4B entirely and lives in compositor-side
clipping policy; both renderers showed it identically.

A client's surface and its decorations were cropped to the owning monitor by
two independent rules:

```text
surface       clip_to_hide()   cropped ONLY for scroll-tiled + tag animations
border        apply_border()   cropped ALWAYS, except when c == grabc
shadow        client_draw_one_shadow()   same
split border  apply_split_border()       same
blur backdrop apply_border()   same (de0b5c5)
```

For the case the cropping was written for — a scroller column scrolled past
its own monitor's edge, which must not paint onto a physically adjacent output
— the two rules agree and everything is correct.

For an **ordinary floating window straddling a monitor seam** they do not. The
surface is not cropped, so the client is visible on both outputs; the border
is, so the far output showed bare client with no decoration at all, missing
even the window's real outer edge. `c->mon` is a window-management fact; an
output boundary is a scissor, not a reason to stop drawing.

The fix is one predicate, `client_clips_to_monitor()`, placed next to
`clip_to_hide()` whose conditions it mirrors, and asked by all four decoration
sites. Scroll-tiled and tag-animating clients crop exactly as before; ordinary
floating ones do not, and their decoration now spans outputs with them.

**`c == grabc` is gone from the policy, not merely moved.** It is transient
interaction state, and it was hiding the defect precisely while the button was
down. Two things worth recording about it:

- A move-drag always calls `setfloating(grabc, 1)` first, so a move-dragged
  window is floating anyway and the new policy gives it the same zero offsets
  `grabc` used to.
- Releasing the button does **not** by itself recompute the decoration —
  `apply_border()` has to run again. So the border does not visibly change at
  the instant of release; it changes at the next layout event. A drag/release
  regression written without forcing a re-layout passes against the broken
  build, and this one did until the nudge was added.

## M4B.2 — the clip-policy audit, finished

Every place that decides whether part of a client is cropped to `c->mon`:

| consumer | floating | floating+grab | scroll-tiled | scroll-tiled+grab | tagining | tagouted | tagouting |
|---|---|---|---|---|---|---|---|
| surface (`clip_to_hide`) | UNCLIPPED | UNCLIPPED | CLIPPED | CLIPPED | CLIPPED | CLIPPED | CLIPPED |
| surface (`resize()` grab path) | UNCLIPPED | UNCLIPPED | — | UNCLIPPED¹ | — | — | — |
| border (`apply_border`) | UNCLIPPED | UNCLIPPED | CLIPPED | CLIPPED | CLIPPED | CLIPPED² | CLIPPED |
| shadow (`client_draw_one_shadow`) | UNCLIPPED | UNCLIPPED | CLIPPED | CLIPPED | CLIPPED | CLIPPED² | CLIPPED |
| split indicator (`apply_split_border`) | UNCLIPPED | UNCLIPPED | CLIPPED | CLIPPED | CLIPPED | CLIPPED² | CLIPPED |
| blur backdrop (`apply_border`, de0b5c5) | UNCLIPPED | UNCLIPPED | CLIPPED | CLIPPED | CLIPPED | CLIPPED² | CLIPPED |
| shield (`client_draw_shield`) | UNCLIPPED | UNCLIPPED | CLIPPED | CLIPPED | CLIPPED | CLIPPED² | CLIPPED |

All decoration rows are now one predicate, `client_clips_to_monitor()`.

¹ **Unreachable, and that is the finding.** `begin_move_or_resize()` prepares a
resize corner only when the client is floating; for a scroll-tiled one
`CurResize` merely sets a cursor. So the grab sets `grabc` and changes no
geometry, `resize()`'s fast path is never driven into an overflowing state, and
the two policies cannot visibly diverge. Verified by contrast — the same held
right-drag resizes a *floating* window from 700x500 to 950x450, so the gesture
itself works. `contrib/avk-clip-policy-test.sh CASE=resizegrab` records this as
a **skip with a reason**, not as coverage and not as a failure.

² **Not rendered.** `tagouted` is entered by `client_set_scene_enabled(c,
false)`, so the client and everything decorating it are disabled. The cell is
academic, which is why `client_draw_shield()` could omit the condition for so
long without anyone noticing. It was folded in anyway: unobservable is not the
same as harmless, and a duplicate definition of client visibility is exactly
what cost the cross-output bug.

### `c == grabc`

Original purpose: keep decorations from being cropped while a window is dragged
past a monitor edge, so a drag looks right. It worked, and it hid the defect —
the border was whole while the button was down and lost its far half at the next
layout event. It is gone from every clipping decision. Two other `grabc` uses
are unrelated and stay: `buffer_set_effect()` disables overview scaling during a
drag, and `resize()` takes a no-animation fast path.

### Border-damage assertions in `avk-border-test.sh`

| assertion | protected invariant | falsifier | status |
|---|---|---|---|
| nothing of the old border survives a move | move damages the vacated ring | none that works¹ | DEFENSIVE ONLY |
| nothing of the old border survives a resize | resize damages the shrunk ring | none that works¹ | DEFENSIVE ONLY |
| nothing of the old border survives unfocus | focus change damages the ring | none that works¹ | DEFENSIVE ONLY |
| no active-colour border survives losing focus | the whole ring is recoloured | none that works¹ | DEFENSIVE ONLY |
| no CPU sync wait / fallback frame introduced | steady-state frame path | counters, always live | VALID |

¹ `AZ_AVK_DAMAGE_HOLE` makes them pass for the wrong reason — it applies from
compositor start, so the band is frozen before a border is ever painted into it
and "no stale border survives" is true because none was ever there. No switch
was written whose only purpose is turning them red. Move-damage correctness is
carried by `avk-rounded-alpha-test.sh` and `avk-damage-test.sh`, which do have
working falsifiers.

### The derivative invariant

`contrib/check-shader-derivatives.sh` enforces it structurally: between a
function's start and its first `fwidth`/`dFdx`/`dFdy` there may be no branch on
`gl_FragCoord` or a varying. It strips comments first — the first version
matched the word `fwidth()` in the explanatory comment above the code, latched
"already seen", and passed against a deliberately reintroduced defect. It now
fails against that injection and passes on the tree. Vulkan validation does not
check this and is not claimed to.

## M4C — gradient reference semantics, traced from source

From `wlr_scene.h` and `render/fx_renderer/shaders/gradient.frag`. **Two of
these contradict what the earlier M4 brief assumed**, so they are recorded
before any implementation.

```text
has_gradient       bool
gradient_degree    float, DEGREES (shader calls radians())
gradient_linear    int, 1 = linear, 2 = conic
gradient_blend     int, see below -- NOT smoothing/premultiply/repeat
gradient_origin[2] float, NORMALISED 0..1 within the rect's box
gradient_count     int
gradient_colors    float*, 4 channels per colour, owned by the node
```

**Stop positions are IMPLICIT, not explicit.** There is no position array. The
shader derives spacing from the count alone. The earlier brief assumed explicit
stop positions; source wins.

**`gradient_blend` selects banded vs interpolated**, and nothing else:

```glsl
if (!blend) {                       // HARD bands, no interpolation
    smooth_fac = 1.0/float(count);
    return colors[int(step/smooth_fac)];
}
smooth_fac = 1.0/float(count - 1);  // smoothstep between neighbours
```

**Step derivation.** `normal = (gl_FragCoord.xy - grad_box)/size`, so the
gradient parameter lives in the rect's own normalised box, not output space.

- *linear*: `uv = normal - origin`, scaled by `1/(|cos|+|sin|)` so the ramp
  still spans the box at any angle, rotated by `degree`, origin added back, and
  `step = rotated.x`.
- *conic*: `uv` rotated by `degree`, then
  `step = -atan(uv.y, uv.x)/PI * 0.5 + 0.5`.

**A latent out-of-range read in the reference.** In the blend path,
`if (ind <= count - 1) color = mix(color, colors[ind + 1], ...)` indexes
`colors[count]` when `ind == count - 1`. AVK must clamp; matching the read
exactly is not the goal, and any comparison fixture should avoid the last band's
final texel until this is settled against SceneFX.

**GLES relinks per stop count** — `link_quad_grad_program(..., count + 1)` when
`max_len <= count`. AVK must not copy that; a storage buffer avoids both the
relink and a pipeline per count.

### Two corrections the implementation forced, and one thing the audit missed

Tracing `quad_grad_round.frag` while wiring M4C.1 turned up a third fact worth
recording, because it decides which box a gradient is normalised against.

**Every gradient rect goes through the ROUNDED path.** `wlr_scene.c:2398`
dispatches `scene_pass_add_rounded_rect_grad()` whenever `has_gradient` is set,
whether or not the rect has any corner radii. There is no plain gradient path
in the scene at all — `fx_render_pass_add_rect_grad()` exists and the scene
never calls it. So a gradient is composed with rounded coverage *and* with the
interior cut-out, which is what makes a gradient BORDER the same draw as a
gradient rect.

**`grad_size` is uploaded and never read.** The rounded gradient shader takes
the `size` uniform — the rect's own box — where the plain one would have taken
`grad_size`. Since the scene sets `range = dst_box`, the two agree and nothing
observable turns on it. It matters only in that AVK normalises against the
command's destination box, which is the value both of them hold.

**`gl_FragCoord` is top-down in BOTH renderers.** SceneFX projects with
`WL_OUTPUT_TRANSFORM_FLIPPED_180` (`fx_pass.c:1864`), which works out to
`ndc = 2p/size - 1` for `p` in top-left output pixels — so `gl_FragCoord.y`
equals output `y` measured from the top, exactly as it does in Vulkan. A
gradient therefore needs no y-flip on the way across. asteroidz's own overview
vignette hedges against one ("a symmetric ramp is invariant to the renderer's
y-flip"), and that hedge is what made this worth checking rather than assuming;
the fixtures pin the direction either way.

## M4C.1 — the gradient snapshot, and where it lives on the GPU

### The snapshot

`struct avk_gradient` (`scene/avk_scene.h`) carries type, degree **in
degrees**, blend, origin, and an offset/count into the scene's packed colour
array. It holds no pointer into SceneFX: `gradient_colors` belongs to the scene
node and is `free()`d by the next `wlr_scene_rect_set_gradient()`, which for a
window border happens on **every focus change** — the first stop is the
animated border colour. The walk copies.

There are no stop positions in it, because there are none in the source.

### One buffer, one binding, one pipeline

```text
set 1, binding 0   readonly buffer AzGradientData { vec4 data[]; }

data[r + 0]   origin.x, origin.y, degree in RADIANS, blend
data[r + 1]   type, colour offset, colour count, --
data[off ..]  the colours, premultiplied
```

A draw carries only `r`, in `params[1]` of the push block — the field the
texture pipeline uses for its alpha mask, which is free here because the two
pipelines never draw the same command. The block is exactly 128 bytes, the
guaranteed minimum `maxPushConstantsSize`, so there was no room to add
anything; `shader/src/push.glsl` is now the single declaration all four shaders
include, so the dual reading cannot drift.

Records and colours share one array so that there is one capacity, one growth
path and one upload. Several gradients in a frame cost one buffer write and one
descriptor bind between them.

**One pipeline, for every gradient.** Not one per stop count: the shader never
names the array's length. That is the whole reason for a storage buffer rather
than a uniform array, and `test-avk-gradient` asserts `pipelines` does not move
across thirty frames.

### Lifetime, without a wait

One buffer per frame in flight, indexed by the **command ring's** slot.
`avk_cmd_ring_begin()` has already waited for that slot's previous submission
before the frame is recorded, so overwriting the buffer is safe by
construction — and so is rewriting its descriptor on growth, which is why there
is a descriptor set per slot rather than one shared set that would be updated
under frames still in flight.

Growth is geometric and hands the old buffer to the retire queue against the
timeline point of the submission that last read it. `cpu_sync_waits` stays 0;
nothing here waits on a fence, a queue or the device.

### Counters

`gradient_draws`, `gradient_linear_draws`, `gradient_conic_draws`,
`gradient_colors_processed` say what the frame **asked for**;
`gradient_buffer_uploads`, `gradient_buffer_upload_bytes`,
`gradient_buffer_reuses`, `gradient_buffer_grows` say what the renderer had to
**allocate**. Keeping them apart is the point: a steady gradient scene is
*expected* to upload every frame (the data goes into a per-frame slot) while
allocating nothing at all, and one merged number would make a growing buffer
indistinguishable from a busy one.

### `AZ_GRADIENT_COLOR_OFFSET=1`

Shifts every record's colour offset by one vec4, so each gradient reads the
next one's colours. Nothing else changes — the buffer is the right size, the
records are all present, and only the indexing is wrong. It fails 12 of
`test-avk-gradient`'s assertions, including the one-stop gradient, which comes
back reading a *record* as a colour. A single-gradient fixture would not
notice, which is why the packing test draws six.

## M4C.2 — linear gradients

### The formula, and what turns on each term

```text
normal = (gl_FragCoord.xy - box_pos) / box_size
uv     = normal - origin
uv    *= 1 / (|cos(rad)| + |sin(rad)|)
step   = uv.x*cos(rad) - uv.y*sin(rad) + origin.x
```

The `1/(|cos| + |sin|)` scale is what keeps the ramp spanning the box at every
angle; without it a 45-degree gradient runs out before the far corner. The box
is the rect's **own** destination box, so a gradient is a property of the
rectangle and moving the window does not move the ramp through it.

**The origin is not conic-only, and it is not a no-op for linear.** Expanding
the last line:

```text
step = k*cos*normal.x - k*sin*normal.y
     + origin.x*(1 - k*cos) + origin.y*(k*sin)
```

so the origin contributes a **constant offset along the ramp**, which vanishes
only at degree 0 (`k = 1`, `cos = 1`, `sin = 0`). At 90 degrees the offset is
`origin.x + origin.y` — which means **(0.2, 0.8) and (0.5, 0.5) are identical
there**, both summing to 1. A fixture built on that pair proves nothing; the
suite uses (0.25, 0.25) against (0.5, 0.5), half a ramp apart.

That offset is also what makes `step` genuinely leave 0..1.

### Orientation, measured rather than assumed

At degree 0 the ramp runs **left to right**: `step = normal.x`, first colour at
the left edge. At 90 degrees `step = origin.x + origin.y - normal.y`, so with a
centred origin it runs **bottom to top** — the last colour at the top. At 45
the ramp runs from the bottom-left corner to the top-right. Increasing degree
rotates the sampling vector, so the picture turns the other way.

### The two modes put the same colours in different places

| | `blend = 0` | `blend = 1` |
|---|---|---|
| segments | `count` bands of `1/count` | `count - 1` of `1/(count-1)` |
| colour *k* sits | filling `[k/count, (k+1)/count)` | **at** `k/(count-1)` |
| between | nothing — a step | `smoothstep` |

With five colours, colour 2 is at 0.5 in interpolated mode and fills
`[0.4, 0.6)` in banded mode. Deriving the interpolated spacing from `count`
instead of `count - 1` is therefore a mistake that still produces a plausible
ramp, and the suite pins the endpoints for exactly that reason.

`smoothstep` rather than a linear mix is worth a fixture of its own: a quarter
of the way into a segment `smoothstep(0.25) = 0.15625`, so on the red-to-green
segment red reads 220 instead of 195. Twenty-five levels, which no rounding
explains.

### `count = 1`, and division by zero

The interpolated path divides by `count - 1`. One colour is special-cased
before that, in the shader and in the oracle alike. It matters because a NaN
reaching a UNORM target is a *colour*, not a crash — it would render as
something and never be noticed.

### The oracle

`tests/test-avk-gradient.c` carries an independent C implementation of the
reference formula and compares every fourth pixel against it, reporting max and
mean channel error. It is written from `gradient.frag`, not from AVK's shader,
or it would agree with whatever AVK does.

Measured, tolerance 2/255:

```text
2 colours, banded      max 0   mean 0.000    5 smooth, degree 45    max 1  mean 0.006
2 colours, smooth      max 1   mean 0.031    5 smooth, degree 90    max 1  mean 0.042
3 colours, banded      max 0   mean 0.000    5 smooth, degree 135   max 1  mean 0.021
3 colours, smooth      max 1   mean 0.021    5 smooth, degree 180   max 1  mean 0.042
5 colours, banded      max 0   mean 0.000    5 smooth, degree 270   max 1  mean 0.052
5 colours, smooth      max 1   mean 0.052    5 banded, degree 45    max 0  mean 0.000
alpha ramp over white  max 1   mean 0.031
```

Banded modes are **exact**; interpolated ones differ by at most one level, which
is fp32 evaluated twice plus UNORM rounding.

### Breaks

| switch | restores | result |
| :--- | :--- | :--- |
| `AZ_GRADIENT_FIRST_COLOR=1` | the shipped AVK behaviour — count forced to 1, every gradient a flat first stop | 36/83 fail |
| `AZ_GRADIENT_BLEND_SWAP=1` | `gradient_blend` read with the wrong polarity | 32/83 fail |
| `AZ_GRADIENT_COLOR_OFFSET=1` | every record pointed at the next gradient's colours | 53/83 fail |

`BLEND_SWAP` was chosen over "ignore the flag" deliberately: swapping fails the
banded **and** the interpolated fixtures, where ignoring it would leave whichever
mode it defaulted to untested.

### Three fixtures that were wrong before the renderer was

Recorded because each looked exactly like a renderer defect:

- **the half pixel.** `gl_FragCoord.x` is `x + 0.5`, so band *k+1* begins at
  `ceil(128(k+1)/5 - 0.5)`. Without the `- 0.5` two of the four boundary probes
  landed one pixel late, both readings fell inside the same band, and the
  failure read as a renderer smearing its boundaries.
- **the terminal-clamp fixture used degree 0**, where the origin offset is
  identically zero — proven three paragraphs above, and then walked into
  anyway. The ramp ran edge to edge, nothing was ever past its end, and the
  test reported a clamp failure that was entirely its own.
- **the packing fixture sampled the middle** of each band row, which was right
  while the lookup returned the first stop and wrong the moment it returned a
  ramp. It samples `x = 2` now: inside band 0 for every count in the spread.

## M4C.3 — conic gradients

### A different coordinate, not the linear one rescaled

```text
uv   = rotate(normal - origin, rad)
step = -atan(uv.y, uv.x)/PI * 0.5 + 0.5
```

No `1/(|cos| + |sin|)` scale and no origin add-back — both are linear-only, and
putting either in the conic branch is how conic quietly becomes
linear-with-extra-steps. There is also no aspect correction: the angle is
measured in the box's own unit square, so a conic gradient on a wide rectangle
covers an ellipse's worth of angle. That is the reference's shape, kept.

### Which way it goes, measured

With a centred origin the coordinate increases **left → down → right → up**,
which on a screen (y downward) is counter-clockwise. With four banded colours
the quadrants *are* the bands, so the mapping is a table rather than an average:

| degree | lower-left | lower-right | upper-right | upper-left |
|---:|:---|:---|:---|:---|
| 0 | 0 | 1 | 2 | 3 |
| 90 | 3 | 0 | 1 | 2 |
| 180 | 2 | 3 | 0 | 1 |
| 270 | 1 | 2 | 3 | 0 |

Each 90 degrees moves every band one quadrant counter-clockwise. The table
catches all four mistakes worth a fixture: a sign error and a direction error
(the sequence reverses), a missing degree offset (nothing moves between rows),
and π where 2π belongs (the pattern repeats twice round and no quadrant is one
colour).

### The seam

`atan2` flips between +π and −π along the **−x ray from the origin**, so the
coordinate jumps from ~1 to ~0 across it. **Neither mode interpolates over it.**
Banded steps from the last band to the first; interpolated does too, because
its last segment *ends* at the last colour and nothing wraps back to the first.

A conic gradient whose first and last stops differ therefore has a hard edge in
it by construction. That is reference behaviour, not an artifact, and it is
asserted in both modes so that "smooth mode is smooth everywhere" — the natural
assumption, and wrong — cannot creep in later.

With origin (0.25, 0.70) the seam moves to row `0.70·128 − 0.5 = 89.1` and to
the left of `x = 31.5`. A shader hard-coded to {0.5, 0.5} puts it at 63/64
instead, and since **every gradient asteroidz itself creates passes
{0.5, 0.5}**, such a shader would render the whole desktop correctly.

### Measured against the oracle

```text
conic 4, banded     max 0  mean 0.000      conic 5, degree 180  max 1  mean 0.036
conic 4, smooth     max 1  mean 0.023      conic 5, degree 270  max 1  mean 0.042
conic 5, smooth     max 1  mean 0.036      conic 5, off-centre  max 1  mean 0.032
conic 5, degree 90  max 1  mean 0.042      conic 17, banded     max 0  mean 0.000
```

### Breaks, and where each one lands

| switch | fails | and correctly does **not** fail |
| :--- | :--- | :--- |
| `AZ_GRADIENT_LINEAR_ONLY=1` | 29/125 — every conic fixture | any linear fixture |
| `AZ_GRADIENT_CENTER_ORIGIN=1` | 7/125 — the linear origin pair, the terminal clamp, the conic origin and seam | everything with a centred origin |
| `AZ_GRADIENT_FIRST_COLOR=1` | 63/125 | the packing and churn tests, which do not depend on the ramp |
| `AZ_GRADIENT_BLEND_SWAP=1` | 61/125 — banded **and** interpolated | — |
| `AZ_GRADIENT_COLOR_OFFSET=1` | 87/125 | — |

`LINEAR_ONLY` is worth its own switch precisely because **nothing on a running
desktop would show it**: both of the compositor's gradient consumers are
linear, so conic support can regress without a single visible symptom.

## A pre-existing bug found while building the GLES comparison

**`border_gradient 1` makes the compositor unresponsive.** Not a rendering
fault and not M4C's: it reproduces on **both renderers** and on a **pre-M4C
build** (`f6d3a61`), at the same rate.

```text
AVK,  border_gradient 1   251 MB -> 1436 MB in 22 s     53.7 MB/s
GLES, border_gradient 1   251 MB -> 1443 MB in 22 s     54.2 MB/s
AVK,  border_gradient 0   114 MB -> 114 MB              flat
pre-M4C f6d3a61, AVK, on  251 MB -> 1416 MB in 22 s     52.9 MB/s

grim   times out at 20 s
amsg   times out at 20 s
```

`/proc/PID/maps` shows a growing `[heap]` with a constant mapping count, so it
is plain `malloc` growth, not leaked buffers.

**Source.** `client_set_border_fill()` (`src/animation/client.h`) calls
`wlr_scene_rect_set_gradient()` **unconditionally** whenever the focused window
has a gradient border. That function frees and re-allocates the colour array
and then calls `scene_node_update()`, which damages the node — so every tick
damages the border, which schedules a frame, which ticks again. The loop never
yields, which is why the event loop never gets back to its clients.

The comment three lines above its caller already states the invariant it
breaks:

> set_focus and set_border_fill are dirty-checked, so this is a no-op when
> unchanged.

The non-gradient branch delegates to `client_set_border_color()`, which *is*
dirty-checked. The gradient branch is not. `border_gradient` defaults to 0,
which is why this has not been hit.

**Not fixed here.** It is in the focus-animation path, not the renderer, and it
predates this milestone. The likely fix is a dirty check on the gradient
branch, matching what the comment already claims.

**Consequence for M4C: the GLES comparison is NOT DELIVERED.** Both routes to a
gradient the compositor can actually draw are closed:

- the **border gradient** hangs the compositor, as above — `grim` and `amsg`
  both time out, so nothing can be captured;
- the **overview vignette** is never created headlessly. It is guarded by
  `m->ov_main_wp && ...->node.enabled` (`overview.h`), the overview wallpaper
  node, which the headless fixture does not produce. Instrumented and
  confirmed: with the overview open, rects go 41 → 153 and **not one of them
  carries `has_gradient`**.

`contrib/avk-gradient-test.sh` is written and skips with that reason rather
than scoring anything. It claims no coverage.

**And it nearly reported a pass.** The fixture's edge-darkening premise
measured 21.0 on both engines with no gradient anywhere on screen — the
overview dims its own background, that dimming is a plain rect, and both
renderers agree about it exactly. Only `gradient_draws == 0` caught it. A
premise a non-gradient satisfies is not a premise.

Until one of those blockers is cleared, reference parity rests on the CPU
oracle in `tests/test-avk-gradient.c`, which is a transcription of
`gradient.frag` and is stricter in every dimension except that it is a reading
of the reference rather than the reference itself.
