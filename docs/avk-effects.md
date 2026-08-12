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

## M4C.3H — the repaint storm, and the invariant it violated

The `border_gradient` hang recorded above is fixed. It was **not** a gradient
rendering fault and **not** an animation that failed to terminate — both of
which are the natural first guesses, and both wrong.

### The loop, traced

```text
rendermon
  → client_draw_frame(c)                     every client, every frame
  → client_apply_focus_opacity(c)
  → (c == selmon->sel) client_set_border_fill(c, border_color)
  → wlr_scene_rect_set_gradient(...)         UNCONDITIONAL
  → free + malloc + scene_node_update()      → damage
  → the output needs a frame                 → rendermon …
```

`client_set_border_fill()` runs on **every rendered frame** for the focused
window, animating or not — that is its job, because `focusclient()` does not
recompute a titlebar's focus state for tile layouts. Its solid-colour branch
goes through `wlr_scene_rect_set_color()`, which `memcmp`s and returns early.
Its gradient branch had no such check.

**35 of the 41 `wlr_scene_*_set_*` setters already held that line**;
`set_corner_radii` compares through `fx_corner_radii_eq()`, `set_clipped_region`
through `wlr_box_equal()`, `set_size` compares its ints. The comment above
`client_apply_focus_opacity`'s caller *already asserted* the invariant —
"set_focus and set_border_fill are dirty-checked, so this is a no-op when
unchanged" — and it was true of one branch and false of the other.

### What it was not

**Not an animation-termination bug.** Focus progress is
`passed_time / duration` on a wall clock with a `>= 1.0` terminal, and there is
no asymptotic `current += (target - current) * k` anywhere in this path. More
pointedly, `animation_duration_focus` **defaults to 0**, which completes on the
first tick. The endless frames came from the *settled* branch that runs forever
afterwards, not from an animation that never ended.

**Not renderer-specific**, which is what made it diagnosable: identical rates on
AVK and GLES, and on a build predating the Vulkan gradient work.

### The fix

An exact identical-write check in `wlr_scene_rect_set_gradient()`, matching its
siblings. **Exact, not epsilon** — the values arrive from the same computation
every frame, so an unchanged gradient is bit-identical, while an epsilon would
quantize the focus animation instead. During a real animation the colour differs
every tick, so the check passes straight through.

### Measured

```text
                        BREAK (bug restored)      FIXED
frames, 5 s idle        unanswerable (IPC dead)   0
RSS over 5 s            1220 → 1917 MB            106 → 106 MB
CPU                     259% of a core            0.0%
amsg round trip         8 s timeout               0.001 s
```

Focus animation, `animation_duration_focus 400`, four alternating switches:
**+27 frames each while animating, +0 over the following 3 s**, every time. Six
IPC dispatches during focus churn: 8 ms total. An animated gradient performs
151 buffer uploads and **0** allocations.

Duration-driven, not frame-driven, measured with quiet clients:

```text
duration  200 ms → settled after  268 ms, 15 frames
duration 1000 ms → settled after 1064 ms, 65 frames
```

Settle time tracks the configured duration; the frame count is an *output*.

### For the future animation milestone — recorded, not implemented

Presentation-aware per-output timing · frame-rate-independent animation ·
interruptible animations · retargeting with velocity continuity ·
spring/critically-damped motion · fractional geometry · damage-efficient
animation · no CPU waits. SceneFX behaviour is a baseline, not the target.

### `AZ_GRADIENT_NOOP_DAMAGE=1`

Removes the check, restoring the storm exactly.
`contrib/avk-idle-convergence-test.sh` must fail against it — and getting it to
*fail* took two corrections, both recorded in `docs/regression-testing.md`: the
suite first **hung** before reaching an assertion (the library's untimed client
wait), and then **passed** its two headline assertions against a wedged,
leaking compositor, because a timed-out `amsg` yields an empty string rather
than the `TIMEOUT` sentinel and two empty strings subtract to zero.

## M4C.4 — damage, interactions, and what a gradient costs

### Property-change damage matrix

Seven properties — colour contents, colour count, type, degree, origin, blend
mode, node opacity — each asserted four ways: the change is **visible**, the new
value **wins completely** inside a damaged region, the old content is
**untouched** outside it, **no stale pixel** survives where it was redrawn, and
re-rendering identical inputs **changes nothing**.

Two of them needed the fixture corrected before they meant anything, and both
are the same shape of trap:

- **blend mode read the same colour in both modes.** At `x = W/4` a five-stop
  gradient is inside band 1 whether banded (`1/5`) or interpolated (`1/4`), so a
  single sample there reports that changing the mode changed nothing. The
  comparison counts differing pixels over the **whole frame** now.
- **node opacity was invisible by construction.** The partial-damage step
  originally skipped the clear, so a half-opaque redraw composited over the same
  gradient it had already drawn: `0.5g + g(1 − 0.5) = g`, exactly the original
  colour. The clear is damage-clipped — an ordinary scissored command, not a
  full-screen wipe — so enabling it models what really happens to a damaged
  rectangle and makes opacity observable.

### Scale, and which space a gradient lives in

Band boundaries land at **20/40/60/80 % of the box** at both 80 px and 120 px,
and the conic centre does not move. The semantics live in the rect's own
normalised box, so an output scale change resizes the box and nothing else.

`degree` is in **output raster space**, traced rather than assumed: SceneFX sets
`range = dst_box`, which is in output coordinates after the output transform,
and derives its coordinate from `gl_FragCoord`, which is also output. AVK does
the same. So the ramp belongs to the box — moving a window does not slide the
gradient through it — while its *direction* is fixed relative to the output.

### Cross-output

A 948-px-wide floating window straddling the seam at `x = 1920`
(`1470 … 2418`), border gradient at 0°:

```text
939 samples along the top border   median step 0   max step 2 (at x=2394)
end-to-end span 510                = two full channels, a complete ramp
```

No discontinuity at the seam. The end-to-end span is asserted beside the
continuity check because **a nearly-flat border would pass a continuity test
precisely by having no discontinuity** — continuity alone cannot tell a ramp
from a fill.

This fixture only became possible once M4C.3H landed.

### Performance — GPU time NOT MEASURED

AVK has no timestamp query pool; `avk_phys.c` reads `timestampPeriod` and
nothing uses it. The only timing available is CPU wall-clock around recording
and submitting, which is a different quantity in a different place — the stat
is now named `cpu_record_ns` rather than `gpu_submit_ns` so it cannot be
misread as GPU cost.

60 frames each, 128×128 target:

```text
                          CPU/frame   draws   upload/frame   Vulkan allocs
solid rects, no gradient     15 us       2         0 B            0
5-colour linear              17 us       2       112 B            0
17-colour linear             16 us       2       304 B            0
5-colour conic               16 us       2       112 B            0
16 gradient windows          19 us      17      1792 B            0
animated 2-stop border       16 us       2        64 B            0
```

A gradient costs **1–4 µs of CPU and a few hundred bytes** over a solid fill,
and allocates nothing. Per the standing rule that every advanced Vulkan
technique must justify its complexity with measurement, this is a measurement
**against** introducing descriptor indexing, GPU-driven batching or a render
graph for gradients: there is nothing here for them to save.

## M4D.0 — the shadow audit, traced from source

Traced against HEAD `925bc91`, not from the M4 overview's summary. Two of the
summary's claims did not survive.

### What the summary got wrong

**"offset/spread are not explicit shadow fields; node geometry expresses
these effects."** True of `wlr_scene_shadow`, and misleading about asteroidz.
The compositor has `shadows_position_x/y` and `shadows_size` as first-class
config, and `client_draw_one_shadow()` turns them into node geometry. Offset
and spread are explicit *at the producer*; the node is where they have already
been resolved.

**The bigger one: asteroidz ALREADY renders a directional dual-lobe shadow.**
`Client` carries two shadow nodes, not one:

```c
struct wlr_scene_shadow *shadow;          /* ambient */
struct wlr_scene_shadow *contact_shadow;  /* contact */
```

with a full config surface for each, and defaults that are already tuned for
light from above. M4D.2 as briefed — "intentionally move beyond SceneFX's
generic shadow appearance" by introducing two lobes — describes something the
compositor shipped before M4 began. What is missing is not the model. It is
that **AVK throws both nodes away**:

```c
case WLR_SCENE_NODE_SHADOW:
case WLR_SCENE_NODE_OPTIMIZED_BLUR:
case WLR_SCENE_NODE_BLUR:
	/* M4. Recognised and skipped, with one warning */
```

So M4D's real content is M4D.1: make AVK render a shadow node analytically and
correctly. The directional look then follows from the producer that already
exists. This is the same shape of finding as M4C's "stop positions are
implicit" — the reference is simpler than the brief assumed, and the honest
implementation is smaller.

### The default profile, as shipped

| lobe | enable | size | blur (σ field) | offset x | offset y | colour |
|---|---|---|---|---|---|---|
| ambient | `shadows` | `shadows_size` **24** | `shadows_blur` **24** | **0** | **+10** | `0x00000066` |
| contact | `shadows_contact` **1** | `shadows_contact_size` **8** | `shadows_contact_blur` **9** | **0** | **+2** | `0x0000004d` |

Broad + weak, offset well downward; tight + stronger, offset slightly downward.
Horizontal bias is already zero. `shadows_position_y`'s own schema help text
reads "A positive value casts downwards, which is what reads as light from
above." The DoD's directional target is the shipped default, and M4D.2's job is
to prove it renders that way rather than to invent it.

Two further scalars modulate both lobes: `shadows_tiled_scale` (tiled windows
get a compact shadow so it does not spill across gaps onto neighbours) and
`shadows_unfocused_scale` (applied to `color[3]` by
`client_apply_focus_effects`, so the focused window sits above its siblings).

### Field table

`SOURCE` is where the value is authoritative. `CURRENT AVK STATE` is what AVK
does with it at `925bc91` — uniformly nothing.

| FIELD | SOURCE | UNITS | COORDINATE SPACE | DEFAULT | REFERENCE USE | CURRENT AVK |
|---|---|---|---|---|---|---|
| `node.x/y` | producer | px | parent tree, logical | — | `dst_box` origin | dropped |
| `width`/`height` | `wlr_scene_shadow` | px | node-local | 0 | `dst_box` extent = **envelope** | dropped |
| `corners` | `wlr_scene_shadow` | px | node-local, **clockwise tl,tr,br,bl** | `corner_radii_all(border_radius)` | caster arcs | dropped |
| `corner_radius` | `wlr_scene_shadow` | px | — | `config.border_radius` | legacy scalar; `set_corner_radius` overwrites `corners` | dropped |
| `blur_sigma` | `wlr_scene_shadow` | px | node-local, **scaled by `data->scale`? NO** | 24 | inset + σ (see below) | dropped |
| `color[4]` | producer | 0..1 | — | `0x00000066` | `v_color`; `.a` is opacity | dropped |
| `clipped_region.area` | producer | px | node-local → root | window box | interior cut-out | dropped |
| `clipped_region.corners` | producer | px | clockwise | `border_radius - 1` | cut-out arcs | dropped |

### The shader's actual geometry — three things that are not obvious

`box_shadow.frag`, at the call site:

```glsl
roundedBoxShadow(position + blur_sigma,
                 position + size - blur_sigma,
                 gl_FragCoord.xy, blur_sigma * 0.5, ...)
```

1. **The node box is the ENVELOPE, not the caster.** The caster is the node box
   inset by `blur_sigma` on every side. `client_draw_one_shadow()` is the other
   half of that contract: it grows the box by `delta = size + bw` on every side
   and *then* shifts by `pos_x/pos_y`, so the falloff has room on all four
   sides. Its comment records the bug from getting this wrong — the box used to
   only grow right and down, and there was no shadow on the left or top
   whatever the offset said.

   **Consequence for the DoD's damage requirement:** "derive a conservative
   cutoff `extent = offset + K*sigma`" is already done, by the producer, in
   `delta`. The shadow's damage envelope *is* its node box, which generic scene
   damage already handles. AVK must not compute a second envelope; doing so
   would be the double-scaling bug the DoD warns about elsewhere.

2. **The effective Gaussian σ is `blur_sigma * 0.5`,** not `blur_sigma`. A
   direct transcription of the field name into a Gaussian is off by 2×.

3. **It is a separable approximation, not a 2D Gaussian.** A closed-form
   `erf()` integral across x, numerically integrated over y with **4 samples**
   weighted by a 1D Gaussian, sampling only ±3σ. The per-scanline corner radius
   is chosen by `sy < 0.0 ? top : bottom` — a real 2D SDF would not do this.
   AVK reproducing "a Gaussian-blurred rounded rect" from first principles
   would be *more* correct and would not match. The CPU oracle must transcribe
   **this** formula, exactly as M4C's oracle transcribed `gradient.frag`.

Also traced: `gl_FragColor = vec4(v_color.rgb, shadow_alpha) * clip_corner_alpha`
does **not** premultiply `rgb` by `shadow_alpha`, while the blend mode is
`PREMULTIPLIED`. For the default black shadow `rgb == 0` and the two agree
exactly; for any non-black `shadowscolor` the reference over-brightens. Same
class of latent defect as M4C's terminal-index read, and to be handled the same
way — do the correct thing, document the divergence, do not call the bug parity.

### Enable/disable policy — preserve exactly

`client_draw_shadow()`:

```c
if (c->iskilling || !client_surface(c)->mapped || c->isnoshadow) return;
bool active = config.shadows && !c->isfullscreen &&
              (c->isfloating || !config.shadow_only_floating);
```

| surface | shadow? | why |
|---|---|---|
| floating | **yes** | full-size lobes (`state_scale = 1.0`) |
| tiled | only if `shadow_only_floating 0` (**default 1 ⇒ no**) | a tiled shadow lands on the neighbour, not the wallpaper |
| tiled, when enabled | yes, scaled by `shadows_tiled_scale` | compact so it does not spill across gaps |
| fullscreen | **never** | `!c->isfullscreen` |
| `isnoshadow` window rule | **never** | early return |
| unmapped / killing | never | early return |
| layer-shell | only if `layer_shadows` (**default 0**) | `asteroidz.c:4246` |
| overview cells/main/strip | yes, own nodes | `overview.h` |
| titlebar / close button | yes | `asteroidz_tab_bar_node_set_shadow`, tiled scale |
| tag-animation snapshots | yes, cloned | `common.h:275` |

The default desktop therefore shows shadows on **floating windows only**. A
headless fixture that tiles its client and then asserts on a shadow is
asserting on nothing — the M4A trap in a new costume.

### Ordering and clipping

A tiled window's shadow tree is reparented to `LyrTileShadow`, *beneath every
tiled window*, so one window's shadow cannot land on top of another's content;
a floating or overview-laid-out window keeps its shadow inside its own tree.
`client_sync_shadow_tree()` owns this, and also mirrors the client tree's
`enabled` flag — without which every hidden tag's shadows would stay on screen.

Cross-output clipping goes through the **same** `client_clips_to_monitor()`
predicate M4B.1 fixed and M4B.2 audited: a floating window's shadow is *not*
cropped to `c->mon`, so it crosses the seam with its window. There is no
shadow-specific clip path, which is why the DoD's optional
`BREAK=shadow-owner-monitor-clip` would duplicate the existing clip-policy
break — it is the same code. Not adding it.

### Damage and idle convergence

Every one of the six shadow setters is dirty-checked at HEAD
(`set_size`, `set_corner_radius`, `set_corner_radii`, `set_blur_sigma`,
`set_color`, `set_clipped_region`), and `set_size` additionally damages its
**old** bounds before shrinking. So the M4C.3H storm has no shadow equivalent
— which matters, because `client_apply_focus_effects()` calls
`wlr_scene_shadow_set_color()` on every focus-animation tick for both lobes,
the exact call pattern that made `wlr_scene_rect_set_gradient` melt the event
loop. It is safe *because* of the `memcmp` on line 1185. Worth an assertion, not
worth a fix.

`scene_node_get_size()` reports the node's `width`/`height` for a shadow, and
`scene_node_opaque()` returns without claiming any opaque region. Both correct
for an envelope-sized translucent primitive.

### What this means for the stages

- **M4D.P** stands as briefed: no timestamp infrastructure exists.
- **M4D.1** is the substance: one analytic shadow material, per-corner radii,
  interior cut-out, envelope semantics as above.
- **M4D.2** is *verification and tuning* of a directional model that already
  exists, plus the question the audit raises on its own: the two lobes are two
  full-envelope draws, and whether fusing them into one material is worth it is
  a measurement, not a preference. Deferred to timestamps.

## M4D.P — GPU timestamps, and the frame that measured itself away

`avk_timestamp.{h,c}`. One `VkQueryPool`, `AVK_TS_MARKS` queries per frame
slot, indexed by the **command ring's** slot — the same index the gradient
store uses and for the same reason: `avk_cmd_ring_begin()` has already waited
for that slot's previous submission, so resetting its queries is safe without
any synchronisation of the query pool's own.

### Two marks, not four

The briefed design had a content/effects phase split. It is not implementable
honestly here, and the reason is worth keeping. **A timestamp pair measures a
contiguous span of the command stream.** AVK draws in scene order and a
window's shadow is drawn immediately beneath that window, so shadow draws are
interleaved with content draws throughout the frame rather than gathered into a
phase. Bracketing the first and last shadow draw would measure "the span of the
frame containing shadow work", which is very nearly the whole frame — and
quoting that as shadow cost is the same class of error as calling CPU recording
time GPU time, which is what this file exists to stop.

Isolating an interleaved primitive's cost honestly means **differencing two
frames that differ only in whether it is drawn**. That is how M4C measured a
gradient and how M4D measures a shadow. M4F's blur *is* a separable pass with
its own barriers, and can add a phase pair then, driven by code that needs it.

### The bug the test found

The first implementation drained slots only from the per-frame
`avk_timestamps_collect()`. A compositor drawing faster than its GPU retires
loses that race every time: collect runs, the slot is still in flight so it is
skipped; the CPU comes round the ring, `avk_cmd_ring_begin()` waits for that
exact slot, and the result becomes available a microsecond before
`avk_timestamps_begin()` reset the pool over it. Measured on the first run:
**23 samples and 37 drops out of 60 frames.**

The fix is to read the outgoing result at `begin()`, before the reset. The
moment the ring hands a slot back is the moment its results are guaranteed
complete — that wait *is* the ring's contract — so it is the one place a read
can never be premature and never has to block. 60 of 60 samples, 0 dropped.

### Not waiting, and proving it

`vkGetQueryPoolResults` is called with `64_BIT | WITH_AVAILABILITY_BIT` and
**no `WAIT_BIT`**. The timeline argument is the correct one; the availability
bit is what makes correctness not depend on my reading of the spec.

The stall assertion had to be rewritten, and the first version of it was the
thing that was wrong. An unthrottled 60-frame loop stalls the ring 44 times
because it submits faster than the GPU drains — that is the ring's designed
backpressure and has nothing to do with timestamps. So the unthrottled case
asserts `cpu_sync_waits == ring.stalls` (every wait is still ring
backpressure; timestamps added no second category), and a **second, paced**
loop — what a vblank-throttled compositor actually does — asserts the ring
never stalls at all and every one of its 12 frames yields a sample.

### Valid bits, and a test that could not have run on this GPU

`timestampPeriod` says the *device* has a counter; `timestampValidBits` says
the *queue family* can write it, and the two are independent — a family can
report 0 on a device whose period is fine. Both are checked, and
`timestamp_valid_bits` is now read in `query_queue_families()` beside the
family it describes rather than beside the device limit it is not.

The upper bits of a raw query result are **undefined, not zero**, so a
duration must be computed on masked values. This GPU reports **64 valid bits**
— masking is the identity here, and at 10 ns/tick the counter wraps in about
585 years. Every masking and wrap case would therefore pass against real
queries on this machine whether the code masked or not: coverage by
coincidence. `avk_ts_ticks_between()` is exported for exactly that reason and
tested on constructed 36-bit values, each with its premise
(`premise: unmasked subtraction of those two is NOT 5000`).

### Measured

`10.00 ns/tick, 64 valid bits, 6 queries`. A 128×128 target with 24 full-screen
translucent rects: **9.7 µs** mean GPU frame time. `gpu_frame_us_avg` is
reported through `amsg get avk-stats` as a **mean of completed samples**, and
is `null` — not 0 — where the device cannot measure, because a test asserting
on 0 there would be asserting the frame took no time.

## M4D.1 — the analytic shadow

`shadow.frag`, `AVK_CMD_SHADOW`, one pipeline. A shadow is push constants and
a quad: the envelope in `round_box`, the caster radii in `corners`, the cut-out
in `inner_box`/`inner_corners`, the colour in `color`, and **`blur_sigma` in
`params.y`** — the slot the texture pipeline uses for its alpha mask and the
gradient pipeline for its record index, which never draw the same command.

**Push constants, not a storage buffer**, and the audit of size is the reason
rather than a preference: the block is exactly 128 bytes and a shadow leaves
`uv_org_dx` and `uv_dy` — eight floats — entirely unused. M4C needed an SSBO
because a gradient carries an unbounded colour list; a shadow carries nine
scalars. Different workloads, different data paths.

### The three geometry facts

1. **The node's box is the envelope**, the caster is that box inset by
   `blur_sigma`. The producer already grew the window by `size + border` and
   then offset it.
2. **The Gaussian's sigma is `blur_sigma * 0.5`.** The field name is not the
   sigma.
3. **Corners arrive clockwise** (tl, tr, br, bl) and the reference's helper
   takes (tl, tr, bl, br).

### Two reference bugs fixed

**REFERENCE BUG FIXED IN AVK: blur sigma is scaled to output pixels** like the
box and radii it is measured against. SceneFX scales `dst_box` and
`fx_corner_radii_scale`s the radii, and passes `blur_sigma` through in logical
units. On this desk — DP-1 at 1.5, HDMI-A-1 at 1.0 — that gives the same window
a visibly tighter shadow on one monitor than the other, and changes its shape
as it crosses the seam.

**REFERENCE BUG FIXED IN AVK: the shadow colour is premultiplied by its
coverage.** `box_shadow.frag` emits `vec4(v_color.rgb, shadow_alpha)` into a
`PREMULTIPLIED` blend. For the default black shadow `rgb` is 0 and the two
agree exactly, which is why it has never been seen; a blue shadow at half alpha
would emit blue at `0.6*255 + 0.5*255`, brighter than the paper it is cast on.
`test_coloured_shadow_does_not_glow` measures r=128 g=128 **b=204** over white.

### The cut-out is now one function

`az_avk_clip_out_region()`. A border is a filled rect with the client's
interior taken out; a shadow is a filled envelope with the same interior taken
out. Same two halves — exact scissor subtraction for what a region can express,
radii to the shader for the arcs it cannot — and the M4A wedge would have been
a second identical bug to find.

### The oracle, and four assertions that were wrong

`tests/test-avk-shadow.c` transcribes `box_shadow.frag` in double precision and
compares **all 43264 pixels** of the envelope: worst **0.6/255**, mean
**0.002/255**.

Four of the first draft's assertions failed, and in each case the test was
wrong rather than the renderer:

- **The oracle must model the envelope's truncation.** Its worst disagreement
  was at `x = 31`, one pixel outside an envelope starting at 32 — a correctly
  clipped shadow scored as a 5.2/255 rendering error.
- **The truncation is 2.3%, and that is arithmetic, not slop.** With size and
  blur both 24 the envelope's edge is 24 px from the caster against an
  effective sigma of 12, so it cuts at **2 sigma**. The assertion demanded
  under 2% and measured 0.0235; the erf tail at 2σ is 0.0228.
- **Corner radii are NOT separable in this approximation.** The obvious test
  expects coverage to fall as the radius grows; bottom-left at r=37 read
  *higher* than bottom-right at r=19 while the frame matched the oracle to
  0.6/255. The x direction is a closed form across a whole **scanline**, so
  coverage near the bottom-left corner is a function of the bottom-left and
  bottom-right radii together.

So per-corner correctness is asserted by **discrimination** instead: the frame
must match the oracle built from its own four radii (0.6/255) while clearly
failing three that stand for real bugs — bottom corners swapped (**48.3/255**),
all corners squared (**63.8/255**), all corners rounded to 37 (**63.7/255**).

## M4D.2 — the directional model was already there

### The finding that reshaped the stage

M4D.2 was briefed as "intentionally move beyond SceneFX's generic shadow
appearance" by introducing two lobes evaluated in one material. The audit found
both lobes already in the tree, as two `wlr_scene_shadow` nodes with a full
config surface each and defaults already tuned for light from above. Nothing
about the look needed inventing. What needed proving is that AVK renders it,
and what needed deciding is whether to fuse the two.

### Where the directionality lives

**Not in the shader.** `client_draw_one_shadow()` grows the window box by
`size + border` on every side and *then* shifts it by `position_y`, so after the
shader insets by sigma the caster is **the window displaced downward**. There is
more envelope below it than above, and that is the entire mechanism. The shader
knows nothing about "macOS" or about light; it implements a directional
analytic shadow and the profile in `config-schema.h` asks for a particular one.

Measured, at equal distance from the window on all four sides:

| distance | top | left | right | bottom |
|---|---|---|---|---|
| 4 px | 0.0706 | 0.1922 | 0.1922 | **0.3490** |
| 10 px | 0.0196 | 0.0863 | 0.0863 | **0.2157** |
| 18 px | 0.0000 | 0.0275 | 0.0275 | **0.1059** |

Bottom beats the sides by 1.8×, the sides beat the top by 2.7×, and the top is
**not** bare (0.07 at 4 px) — a trace above is what attaches the window to the
scene instead of letting it float. Left and right agree to **0.00/255** over
2–20 px, which is the assertion that would catch an asymmetry introduced by a
coordinate bug rather than by the profile; every other fixture is symmetric in
x by construction and would not notice.

The falloff below, pinned rather than described: **0.431** at 1 px, 0.377 at 3,
0.294 at 6, 0.180 at 12, 0.051 at 24, 0.000 at 36 — and the darkest point is
**at** the window, not adrift below it, which is the difference between a
contact shadow and a detached blob.

### Composition needs no clamping

Two translucent nodes drawn in order give `1 - (1-a)(1-c)`, which *is* the
coverage of a union of independent occluders. Measured: ambient 0.2980, contact
0.1098, together **0.3765** against a predicted 0.3751 — while naive addition
would give 0.4078. Nothing needs to add two alphas and clamp; the scene graph
already does the right thing.

### Fusing the lobes: rejected, with the measurement

| scene | GPU | CPU | shadows |
|---|---|---|---|
| no shadows | 3.6 µs | 11.1 µs | 0 |
| 1 window, ambient only | 5.2 µs | 11.2 µs | 1 |
| 1 window, both lobes | 5.2 µs | 11.7 µs | 2 |
| 10 windows, both lobes | 14.6 µs | 14.1 µs | 20 |
| 16 windows, both lobes | 20.7 µs | 15.4 µs | 32 |

First lobe **1.53 µs**; **second lobe 0.07 µs**; 32 lobes across 16 windows
**17.0 µs** over baseline. Zero Vulkan allocations throughout.

Fusion would evaluate the contact term over the *ambient* envelope — size 8
against size 24, so roughly 48% of the area becoming 100% of it — doubling its
fragment count to save one draw call and one blend that together cost 0.07 µs.
It would also have to assume the two nodes are adjacent siblings with nothing
between them, which the scene graph does not promise and which reordering to
obtain would break translucent ordering. **Rejected on the number, not the
preference.**

### Breaks

```text
AZ_SHADOW_SINGLE_RADIUS   56/58 -- matches the all-square oracle to 0.6/255
                                   while missing the right one by 63.7/255
AZ_SHADOW_SYMMETRIC       51/58 -- re-centres the envelope on the window;
                                   top == bottom == sides, exactly
```

`shadow-owner-monitor-clip` was **not** added. There is no shadow-specific
cross-output clip path — a shadow goes through the same
`client_clips_to_monitor()` predicate M4B.1 fixed — so the break would be a
duplicate of the existing clip-policy one.

## M4D.3 — shadows in a running compositor

`contrib/avk-shadow-test.sh`, 22 assertions, 6 cases. What it adds over the
unit suite is everything the unit suite structurally cannot see: whether the
compositor ever *asks* for a shadow, whether it asks on the right windows,
whether the answer survives a fractional output scale, and whether a shadow
crosses a monitor seam with its window.

### The trap it is built around

**asteroidz shows shadows on FLOATING windows only.** `shadow_only_floating`
defaults to 1, because a tiled window's shadow lands on its neighbour rather
than on the wallpaper. A fixture that spawns a client, leaves it tiled and then
asserts on a shadow is asserting on nothing, and would score identically
against a renderer that dropped every shadow node — which is exactly what AVK
did until M4D.1. Every case checks `shadow_draws` as a premise first, and the
suite opens by asserting the policy in both directions: **0 draws tiled, 2
after `toggle_floating`.**

### Measured on screen

Luma 6 px out from a real capture, against a background of 128:

| | top | left | right | bottom |
|---|---|---|---|---|
| darkening | 0.086 | 0.273 | 0.273 | **0.547** |

Bottom is 2× the sides, the sides 3.2× the top, and left/right agree exactly.

**Scale 1.5 is where the sigma fix shows.** The shadow's reach below the window
measures **29 device px at scale 1.0 and 44 at scale 1.5** — a ratio of 1.517.
An unscaled sigma would have left the falloff tight against a box that grew,
and the reach would have stayed near 29.

**The seam**: a 600 px window straddling x=1920, sampled along the row 6 px
below it across both outputs — 583 samples, max step **2.00/255**, shadow depth
70/255. A shadow clipped to its own monitor would stop dead at the seam.

**Idle**: 11 frames, then 11 frames four seconds later. Delta **0**, IPC round
trip **1 ms**. M4C.3H's invariant holds for shadows, and it holds because all
six shadow setters were already dirty-checked — `client_apply_focus_effects()`
calls `wlr_scene_shadow_set_color()` on every focus tick for both lobes, which
is the exact call pattern that melted the event loop for gradients.

**Move**: worst residual darkening where the shadow used to be, **0.00/255**.

### One trap paid for, again

The scale case first ran with a `monitorrule { scale 1.5 }` block and reported
**scale 1**. That block parses as key:value pairs and is silently ignored — the
same mistake the cursor-lifetime suite once made, claiming mixed scales while
running both outputs at 1. It was caught only because the case asserts the
premise (`the output really is at 1.5`) before measuring anything. `HL_SCALE1`
is the knob that works.

### A break that gives no coverage here — and a claim that was wrong

`BREAK=shadow-single-radius` scores **24 of 24** against this suite. That is
not a pass; it means zero coverage, and it is recorded in the fixture as such.
But the *reason* first written down was wrong, and the correction matters more
than the break.

The fixture originally said asteroidz never produces a shadow with four
different corner radii, reasoning from `corner_radii_from_location()` being
all-or-nothing. **The live session disproved it in one number:
`asymmetric_shadow_draws = 258`.** `client_apply_border()` starts at
`CORNER_LOCATION_ALL` and masks off whichever corners the window is flush
against a screen edge on, plus the titlebar rule that squares the top-left so
the tab and the window read as one shape — and it feeds that mask straight to
both shadow nodes.

The suite now reproduces it (a window with `--ssd` and `titlebar { enable 1 }`,
which gives **4 asymmetric draws of 10**) and asserts on the counter, because
that is the part the unit suite structurally cannot reach: proof that the
*compositor* really hands the renderer corners that differ.

What it still cannot do is photograph one. Those draws are **transient** —
they happen while the tab is being assembled, and by the time the compositor
settles and `grim` can capture, the corners are equal again. Three fixtures
were tried before this was understood:

- a window moved flush to a screen edge — `move_window` clamps it back inside
  the usable area, and the counter read 0;
- a window opened under an animation — also 0;
- this one, probed at all four corners — `tl=119.6 tr=122.2 br=99.4 bl=99.4`
  normally against `tl=119.0 tr=122.2 br=98.2 bl=98.2` under the break.

A 1/255 difference is not a falsifier. (A prior attempt at radius 12 under
blur 24 was worse still and for a physically correct reason: an effective
Gaussian sigma of 12 does not preserve a 12 px corner.)

So the **pixel** proof of per-corner shadow radii lives in
`tests/test-avk-shadow.c`, where a constructed 0/7/19/37 case is compared
against three wrong oracles and fails the break **56 of 58**.

`BREAK=shadow-symmetric` fails this suite **21 of 24**, on the two directional
assertions and the scale one.

## M4D.4 — quantisation, and a dither that is derived rather than guessed

Live acceptance produced a real defect: concentric rings around every window
on a flat backdrop. The shadow was correct — this is what a correct shadow
looks like when it is quantised into eight bits.

### The path, traced rather than assumed

```text
analytic coverage                      fp32 in the shader
  -> premultiplied source              fp32
  -> blend  src*1 + dst*(1 - srcAlpha) VK_BLEND_OP_ADD
  -> attachment                        VK_FORMAT_B8G8R8A8_UNORM
  -> scanout                           DRM XR24 (XRGB8888) -- the SAME buffer
  -> display                           bitdepth 8, 10-bit not enabled
```

Two facts matter and neither is visible from `bitdepth=8` alone. The attachment
is **UNORM, not SRGB** — quantisation is uniform in encoded space. And there is
**no intermediate higher-precision target**: AVK composites directly into the
scanout buffer, so every blend result is rounded to 8 bits immediately.

### Why ±1/255 of coverage would have done nothing

A black shadow's premultiplied source colour is zero, so the framebuffer value
is exactly `out = dst * (1 - alpha)` and

```text
d(out)/d(alpha) = -dst
```

The perturbation a viewer sees is the perturbation of **alpha times the
backdrop**. On the mid-grey where the rings were reported, `dst = 45/255`, so a
whole 1/255 of coverage moves the output by **0.18 of one code** — under a
fifth of a step. The naive `coverage += noise/255.0` would have changed
nothing at all.

Inverting for one code peak-to-peak:

```text
amplitude = 1 / (max_code * dst_ref) = 1 / (255 * 45/255) = 1/45 = 5.67/255
```

`avk_dither_amplitude()` computes exactly that, from the **attachment format**:
8-bit gets 0.02222, 10-bit gets 0.00554 (a quarter, asserted), and
`R16G16B16A16_SFLOAT` gets **zero** — M5's scene-linear intermediate must not
have noise injected into it.

**The honest limitation**: a shadow shader cannot read its destination, so
`dst_ref` is a constant. A fixed alpha-domain dither produces an output
excursion *proportional* to the backdrop, which is backwards — dark backdrops
band worst and receive least. So it is calibrated for the dark end and the
price at the bright end is measured, not assumed: **1.21 codes rms** of grain
on white, where there was no banding to fix.

### Before and after

Both frames from one process, by driving `renderer->shadow_dither` — two
processes would compare two GPU states rather than two frames.

| | codes | max run | mean run |
|---|---|---|---|
| undithered | 12 | **17 px** | 8.00 px |
| dithered | 12 | **7 px** | 2.09 px |

The live defect was a 41 px run. A 7-px-wide constant run does not draw a line.

**The mean is untouched**: a 7-px low-pass of the dithered line against the
M4D.1 oracle gives mean |err| **0.072 codes**, max **0.219**, and average
density 40.225 against a predicted 40.196. Zero-mean noise, confirmed on the
pixels rather than asserted from the formula.

### Choosing the noise, by measurement

| | max run | mean run | grain on white | worst long-range |
|---|---|---|---|---|
| interleaved gradient noise | **7 px** | **2.09** | **1.21** | 0.088 codes |
| integer hash (white noise) | 10 px | 3.00 | 1.52 | 0.015 codes |

IGN wins on both numbers that matter and ships. Neither needs a texture, a
binding or a table.

**And the analysis that nearly went the other way.** The first structure test
asserted on the *correlation coefficient* and IGN failed it: +0.489 at the
diagonal lag, against −0.061 for the hash. A control run proved that was real
structure and not an artefact of the test's own high-pass filter. But a
correlation coefficient is relative to the variance it sits in. The residual
here has a standard deviation of about a third of a code, so IGN's structured
component is `0.245 × 0.36 = 0.088 codes` — **a eleventh of the smallest step
the display can show**. Rejecting IGN on the coefficient would have traded an
invisible correlation for a visible 10-px run. The assertion is therefore in
**codes**, not in correlation.

(The first version of that test was wrong in a second way too: it correlated
the raw patch, which is dominated by the shadow's own ramp, and reported +0.95
at every lag.)

### The two modulations

`az_dither_alpha()` tapers to zero at both ends — unconditional noise would
speckle the transparent margin of every envelope and the caster's own cut-out,
a far more obvious artefact than the banding — and tapers off wherever
`fwidth(alpha)` exceeds the amplitude. That second one is the contact-edge
protection: where the ramp already moves further than the dither in one pixel
there is no band to break, and noise would only roughen a clean edge. Measured
roughness across a sharp contact edge: **0.000 codes rms**. It is also why the
headless fixture had to sample the *flat tail* to find any dither at all.

### Everything else it must not break

- **Deterministic**: four renders of the same scene are byte-identical. No
  frame counter, no clock — an idle desktop stays bit-stable, and a partially
  damaged frame cannot fall out of step with the pixels beside it.
- **Screen-space**: at scale 1.5 the penumbra's lag-1 correlation is
  **−0.851**. The sign is the measurement — noise anchored in *logical* space
  would be stretched over 1.5 device pixels and correlate strongly *positive*.
- **Cross-output**: the column residual at the seam is **0.014** against a
  worst-elsewhere of 0.139. The seam is less anomalous than a typical column.
- **Directional model intact**: bottom 0.348, sides 0.194, top 0.072.
- **Idle convergence**: 14/14, unchanged.

The directional and horizontal-bias fixtures had to start **averaging along
each side** rather than sampling one pixel: zero-mean noise only cancels over
an area, and two analytically identical sides read 4/255 apart on single
samples. That is the dither working, and it is also what the eye does.

### Cost

| scene | dither off | dither on | delta |
|---|---|---|---|
| 1 window, both lobes | 5.3 µs | 5.4 µs | +0.1 |
| 16 windows, 32 lobes | 20.7 µs | 21.8 µs | **+1.05 µs** |

Repeated three times; the undithered figure sat at 20.7–20.8 throughout, so
the delta is outside run variance and is reported as real rather than as
"no measurable cost". It is **0.015% of a 6944 µs frame** at 144 Hz. Zero
images, buffers, memory, descriptors, passes or submissions added.

### Break

`BREAK=shadow-no-dither` fails **4 of 82**, and fails them by restoring the
defect: max run back to 17 px, mean run back to 8.00, and the high-passed
patch's variance collapsing to 0.035 because there is no longer a dither to
correlate.

### M5 migration note

`dither.glsl` is a standalone helper with no shadow in it, and that is
deliberate. The architecturally correct home for display-quantisation dither
is the final output-encoding stage:

```text
scene-linear FP16 composition
  -> tone / gamut mapping
  -> output transfer function
  -> FINAL quantisation      <- dither belongs HERE
```

There the value being quantised is the composed colour, `dst` is known so the
amplitude needs no reference constant, and one implementation fixes gradients,
blur, wallpaper, transparency and HDR→SDR mapping at once. M4D dithers
shadow-locally only because composition currently targets 8-bit directly.
`az_dither_alpha()` moves to that stage unchanged; `AVK_DITHER_REF_BACKDROP`
is the only thing that goes away, and `avk_dither_amplitude()` already keys off
format precision rather than any assumption about SDR white — which matters,
because M5's per-window luminance domains make "SDR white is X" untrue.
