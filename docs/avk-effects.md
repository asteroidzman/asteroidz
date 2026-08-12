# AVK effects — the M4 audit

What SceneFX renders today, what AVK does with it now, and what M4 owes each
one. Traced from the code and the shaders, not from documentation: several
entries below contradict what the names suggest.

The architectural split M4 preserves:

```text
SceneFX / scene graph      decides WHAT effect exists
AVK                        decides HOW it is rendered
```

## Where AVK stood when this audit was written, and where it stands now

The four gaps this audit opened with, all in the scene walker
(`src/render/az_avk.h`), and what closed each one. The middle column is the
M4A-era state and is kept because the rest of this document argues from it.

| gap | state at the audit | now |
|---|---|---|
| rounded corners | `corners` never read on any node type | **M4A.** Per-corner, scale-aware, no damage growth |
| border inner edge | cut out as a **square** pixman subtraction, not a rounded one | **M4B.** The annulus is one primitive; both edges are the same SDF |
| gradients | detected, drawn as **the first colour only** | **M4C.** Arbitrary stops, linear and conic, one pipeline |
| shadow | recognised and **skipped** | **M4D.** Analytic, per-corner, sigma in output pixels |
| blur | recognised and **skipped** | **M4F.2A.3.** Emitted in place by the walker; current-frame scene-prefix source, darken, edge softness, node-local clip |
| optimized blur | recognised and **skipped** | still skipped, and deliberately: it is the bottom-layer CACHE node, and AVK has no cache for it to populate (M4F.2E) |

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

## M4E.0 — the current frame, audited from source

M4E builds a render graph. Before deciding what the graph should own, this is
what actually exists at `b9d7115`, re-read from source rather than taken from
the milestone summaries. Every path below was traced through the file that
implements it.

### The frame, in order

`az_avk_build_frame()` (`src/render/az_avk.h`) is the whole entry point, called
once per output per frame from the commit path.

```text
 1  az_avk_output_supported()          declines colour transforms, zoom, ...
                                       -> fallback_frames, SceneFX renders it
 2  resolve width/height/fourcc        from wlr_output_state when it changes them
 3  swapchain (re)create               only on size or format change
 4  az_avk_renderer_for(vk_format)     -> a renderer slot, PER FORMAT (see below)
 5  az_avk_present_sync_prepare()      can this frame be handed over with a fence?
 6  wlr_swapchain_acquire()            -> wlr_buffer
 7  az_avk_target_for_buffer()         addon-cached az_avk_target -> avk_image
 8  az_avk_target_acquire()            -> 0..2 VkSemaphoreSubmitInfo waits
 9  avk_scene_init + damage ring       rotate PER BUFFER, not per frame
10  az_avk_walk_children()             the snapshot: scene tree -> flat avk_cmd[]
11  az_avk_emit_cursors()              last, so it is above everything
12  avk_render_frame()                 see below
13  az_avk_present_handover()          export sync_file -> drm_syncobj or dma-buf
14  avk_renderer_collect()             retire + timestamps, neither blocks
15  avk_dmabuf_importer_collect()
16  wlr_output_state_set_buffer()
```

Inside `avk_render_frame()` (`scene/avk_render.c`):

```text
  avk_cmd_ring_begin()                 slot = frame % 3; may stall (counted)
  avk_timestamps_begin()               reads the OUTGOING result, then resets
  avk_timestamps_mark(FRAME_BEGIN)     TOP_OF_PIPE
  count gradient demand                one pass over the commands
  avk_gradient_store_begin(slot, n)    may grow; old buffer retired by timeline
  build the acquire batch              target + every sampled image
  vkCmdPipelineBarrier2                ONE call, before the rendering instance
  vkCmdBeginRendering                  loadOp LOAD, storeOp STORE, 1 attachment
  vkCmdSetViewport                     once
  bind gradient set                    once, only if the frame has gradients
  clear                                as a normal scissored draw command
  for each command:
      region = dst n clip n damage     empty -> skipped entirely
      push constants (128 B)
      vkCmdBindPipeline                only when it changes
      for each damage rect:
          vkCmdSetScissor
          vkCmdDraw(4, 1, 0, 0)
  vkCmdEndRendering
  vkCmdPipelineBarrier2                the release batch (foreign images)
  avk_timestamps_mark(FRAME_END)       BOTTOM_OF_PIPE
  avk_cmd_ring_submit()                vkQueueSubmit2, one graphics queue
  avk_gradient_store_submitted()
  avk_timestamps_submitted()
```

**Two barrier calls per frame, maximum.** Both are `vkCmdPipelineBarrier2` with
`VkImageMemoryBarrier2`; there are no buffer barriers and no global memory
barriers anywhere on the frame path. Both batches are capped at
`AVK_MAX_BARRIERS` (64) and deduplicated by `VkImage`, because two commands
sampling one surface is ordinary and a second barrier whose `oldLayout` has
already been consumed is invalid.

**One rendering instance per frame.** There is no point at which AVK stops
rendering, does something else, and starts again — which is precisely the shape
M4F needs and precisely the shape this audit exists to establish.

### The renderer is per FORMAT, not per output

This is the finding that most affects M4E's design, and it was not written down
before.

`az_avk_renderer_for(VkFormat)` returns a slot from a fixed array keyed on
format, creating one on first use. `struct az_avk_output` (per monitor) holds a
*pointer* to that slot. So on this machine:

```text
DP-1        3840x2160@144  XR24 (0x34325258)  ->  VkFormat 44
HDMI-A-1    1920x1080@60   XR24 (0x34325258)  ->  VkFormat 44
```

both outputs share **one** `avk_renderer`. The live log confirms it: exactly one
`AVK: renderer ready for VkFormat 44`, followed by two `now composited at` lines.

What that shares, concretely:

| Shared across outputs | Per output |
|---|---|
| `avk_cmd_ring` — 3 slots, 3 pools, 3 command buffers | `avk_sync` (export semaphore + wait slots) |
| `avk_gradient_store` — 3 buffers, 3 descriptor sets | `wlr_swapchain` and its targets |
| `avk_timestamps` — one query pool, 3 slots | `az_avk_output` geometry/format state |
| `avk_retire_queue` | the damage ring (on the scene output) |
| `avk_pipelines` — pipelines, layouts, samplers, descriptor pools | |

Three consequences that M4E has to respect:

1. **~204 frames/s pass through 3 ring slots** (144 + 60). The ring is not
   "three frames of one output deep"; it is three submissions deep across the
   machine. `cpu_sync_waits` has stayed 0 regardless, because a slot is released
   by GPU completion and not by output identity.
2. **A transient pool hung off the renderer is shared by both outputs.** Two
   outputs wanting a 1920x1080 transient in the same period must receive
   *different* images. Timeline-keyed reuse gives that for free — output B's
   acquire sees output A's transient still in flight and allocates rather than
   waits — which is also exactly the "no artificial cross-output waits"
   requirement. Reuse keyed on frame index would not.
3. **`gpu_frame_us_avg` is a mean over both outputs' frames.** M4D reported
   122.6 µs and framed it as "1.8% of DP-1's 6944 µs budget". The number is
   right as a per-frame mean across the machine; attributing it to DP-1's budget
   was loose, because a 1920x1080 frame and a 3840x2160 frame are both in that
   mean. M4E.5 measures per output.

### Resource categories

Every Vulkan resource AVK holds, classified. `TRANSIENT` is listed last because
the answer is the interesting one.

| Resource | Category | Owner | Freed by |
|---|---|---|---|
| scan-out `VkImage` (imported dma-buf) | EXTERNAL, PER-OUTPUT | `az_avk_target` addon on the `wlr_buffer` | addon destroy |
| client surface `VkImage` (dma-buf import) | EXTERNAL, PER-SURFACE | `az_avk_buffer` addon on the `wlr_buffer` | addon destroy -> retire |
| SHM-uploaded `VkImage` | PERSISTENT, PER-SURFACE | `az_avk_buffer` | addon destroy -> retire |
| `avk_upload` staging buffer | PERSISTENT, PER-SURFACE | `az_avk_buffer.upload` | retire by `last_use` |
| gradient storage buffer + descriptor set | PER-FRAME (ring of 3) | `avk_gradient_store` | grow -> retire by `last_use` |
| command pool + primary buffer | PER-FRAME (ring of 3) | `avk_cmd_ring` | renderer finish |
| timestamp `VkQueryPool` | PERSISTENT | `avk_timestamps` | renderer finish |
| pipelines, layout, set layouts, samplers | PERSISTENT | `avk_pipelines` | renderer finish |
| sampler descriptor sets (nearest, linear) | PER-SURFACE, cached ON the image | `avk_image.sampler_set[2]` | with the pool |
| descriptor pools | PERSISTENT, grown in blocks | `avk_pipelines` | renderer finish |
| device timeline semaphore | PERSISTENT | `avk_device` | device destroy |
| export binary semaphore, wait slot ring | PER-OUTPUT | `avk_sync` in `az_avk_output` | output finish |
| target `VkImageView` | lazily created, cached on the image | `avk_image.view` | with the image |
| **transient anything** | **NONE EXIST** | — | — |

**There is not one transient resource in AVK today.** Every image is either
external (a client's or KMS's) or cached for a surface's whole lifetime. Nothing
is created for a frame and thrown away. That is *why* the direct path is cheap,
and it is exactly the gap M4E fills — so the risk M4E must not realise is
introducing per-frame allocation into a path that currently has none.

### Layout state, and where it lives

`avk_image.layout` is the single source of truth, updated by `batch_add()` as it
emits the transition. Nothing infers a layout from context. The rules already
encoded:

- A **foreign** image (`AVK_IMAGE_DMABUF_EXPLICIT` / `_RECOVERED`) is acquired
  from `VK_QUEUE_FAMILY_FOREIGN_EXT` and released back, and lives in
  `VK_IMAGE_LAYOUT_GENERAL` between frames. `batch_add()` sets
  `image->layout = GENERAL` when it queues the release, so the *next* frame
  acquires from the right layout.
- The **scan-out target is foreign too**, so it renders in `GENERAL` rather than
  `COLOR_ATTACHMENT_OPTIMAL` — the optimal layout would have to be released back
  to `GENERAL` anyway, and dynamic rendering accepts `GENERAL`.
- Barriers may not carry a layout transition inside a rendering instance
  (`VUID-vkCmdPipelineBarrier2-oldLayout-01181`), which is why everything
  sampled must reach `SHADER_READ_ONLY_OPTIMAL` before `vkCmdBeginRendering`.
- `loadOp LOAD` is a `COLOR_ATTACHMENT_READ`, not only a write, so the target's
  acquire carries both access bits or synchronization validation reports a
  read-after-write hazard at `vkCmdBeginRendering`.

All four are load-bearing and all four were learned from validation rather than
from reading the spec first. The barrier compiler in M4E.3 must reproduce them,
not rediscover them.

### Submission and lifetime

One graphics queue. One `VkSemaphore` timeline for the whole device
(`avk_device.timeline`), `timeline_next` reserved per submission. Every lifetime
question in AVK is already answered by comparing against it:

```text
avk_cmd_slot.timeline_value      when the slot's commands can be re-recorded
avk_gradient_slot.last_use       when the buffer can be replaced
avk_upload.last_use              when staging can be destroyed
avk_image.last_use               when the image can be destroyed
avk_ts_slot.timeline_value       when the queries can be read without waiting
avk_retire_entry.timeline_value  when the destructor may run
```

M4E adds exactly one more row to that table (`transient.last_use`) and no new
mechanism. There is a dedicated transfer family and a dedicated compute family
on this device, both recorded in `avk_device.caps` and **deliberately not turned
into queues** — the existing comment says an unused `VkQueue` would be
infrastructure claiming to exist before anything drives it, and M4E does not
change that.

Uploads go through a **second** command ring (`avk_dmabuf_importer.upload_ring`),
not the frame ring, so an SHM re-upload is a separate submission on the same
queue and is ordered by queue submission order rather than by a barrier.

### Dynamic rendering: already in use

`avk_pipelines_init()` builds every pipeline with
`VkPipelineRenderingCreateInfo`; the frame uses `vkCmdBeginRendering` /
`vkCmdEndRendering`. **There is no `VkRenderPass` and no `VkFramebuffer` object
anywhere in AVK.** The reasoning is recorded in `pipeline/avk_pipeline.h` and is
the M4F-relevant one: fx_vk spent an entire restructure on the fact that its
two-subpass render pass could not be split mid-frame to sample what it had
drawn. Dynamic rendering has no such shape to be trapped by.

So the "should we migrate to dynamic rendering" question is already answered by
the code: **USE NOW, no migration, no work.** The transient attachments M4F
needs are `VkImageView` + `VkRenderingAttachmentInfo` and nothing else — no
framebuffer object per size, no render-pass compatibility rules, no cache to
invalidate on mode change.

### Descriptors: no frame-path churn today

- **Set 0** (sampled texture) is allocated on first use and cached *on the
  image*, two sets per image (nearest and linear). A client surface costs one
  descriptor write for its whole lifetime, not one per frame.
- **Set 1** (gradient storage) is 3 sets, one per ring slot, written only when
  the buffer grows.
- Pools grow in blocks and are never reset.

The frame path therefore performs **zero** descriptor allocations and zero
descriptor writes in steady state. M4F sampling a transient breaks that unless
the transient's descriptor is cached with the transient — which is the design
constraint M4E.2 inherits, and the reason the pool must own views and sets, not
just images.

### Damage, as it exists

Damage is per **buffer**, not per frame: `wlr_damage_ring_rotate_buffer()` keyed
on the `wlr_buffer`, so a target that last held frame N−3 gets everything
changed since N−3. Commands are all present in the scene; the renderer
intersects each command's `dst` with its `clip` (the node's visible region after
occlusion) and with the frame's `damage`, and issues **one draw per resulting
rectangle**.

Three regions therefore already exist in the current design and are already
distinct — but only two of them are named:

```text
cmd->dst        where the command lands              WRITE extent
cmd->clip       what is left after occlusion         WRITE mask
scene.damage    what must be repainted this frame    WRITE mask
```

There is **no read/dependency region**, because no current primitive reads
anything outside its own destination. `avk_scene.h` already says so in the
comment on `damage`: *"clipping to damage is the renderer's job, because an
effect in M4 may need to read outside the damaged area to write inside it."*
That sentence has been true and unexercised since M3. M4E.4 is where it stops
being unexercised.

### Where the graph already is

The audit's actual conclusion is that `avk_render_frame()` is already a render
graph, executed once, with a hardcoded topology:

```text
DECLARE   the loop that walks scene->cmds building the acquire batch
COMPILE   batch_add(), deriving old->new layout from image->layout
EXECUTE   vkCmdBeginRendering + the draw loop
```

M4E.1 is therefore a **refactor into an explicit form**, not a new
architecture — which is what makes "the direct path must stay essentially the
current renderer" achievable rather than aspirational. The one-pass frame must
come out the far side as one pass, one barrier call, one rendering instance, and
no transient images.

### What the graph must NOT own

Named here because the boundary is easy to erode: window focus, layout
decisions, Wayland state, animation state, monitor ownership. The graph sees
`avk_image`, `avk_box`, `pixman_region32_t` and callbacks. It does not include a
wlroots header — `tests/check-vulkan-isolation.py` fails the build if anything
under `src/render/vulkan/` does, and M4E does not weaken that.

### M4F semantics, re-read from source

Verified against `subprojects/asteroidz-scenefx/include/scenefx/types/wlr_scene.h`
and `include/scenefx/render/pass.h` at HEAD, not from the M4D notes.

`struct wlr_scene_blur` carries: `width`, `height`, `corners`,
`clipped_region`, `strength`, `alpha`, `should_only_blur_bottom_layer`,
`has_sample_exclude` + `sample_exclude`, `darken`,
`transparency_mask_source`, `clip_region` + `has_clip_region`, and
`edge_softness`. `struct blur_data` carries the kernel itself: `num_passes`,
`radius`, `noise`, `brightness`, `contrast`, `saturation`,
`transparency_threshold`.

The four that constrain M4E's architecture:

- **`sample_exclude`** — a box, in the node's own space, whose contents must not
  reach the blur's *source*; the unblurred bottom-layer snapshot is substituted
  inside it. It exists because a shadow's backdrop blur covers its own window,
  and the scene image holds the *previous* frame there. This is the requirement
  that forbids a graph in which blur can only sample the already-composed
  output: the graph must be able to express *background source* as a resource
  distinct from *final composition*.
- **`darken`** — the blurred backdrop is clamped against its own unblurred
  source so it can never come out lighter. Needs no graph feature, but the
  material stage must be able to receive the unblurred source as a second
  input, which is a dependency edge and therefore is the graph's business.
- **`edge_softness`** — 0 means the node's edge is a hard rounded-rect SDF; > 0
  means it fades with the same analytic Gaussian falloff `wlr_scene_shadow`
  uses. So the *visible* region and the *evaluated* region differ, which is the
  padded-dependency-region case.
- **`clip_region`** — a pixman region in node-local coordinates (the client's
  ext-background-effect region), taking precedence over `clipped_region`'s
  bounding box when set. The graph must not force a blur to produce a whole
  intermediate and discard most of it.

Together those are the reason M4E declares three regions and refuses to collapse
them:

```text
WRITE REGION      the pixels the pass produces        (clip_region n damage)
READ REGION       the source pixels needed for them   (write dilated by radius)
RESOURCE EXTENT   the backing image's dimensions      (>= read region)
```

For a separable Gaussian of radius r at pass count n the read region is the
write region dilated by roughly `n * r` in one axis per pass. Collapsing these
into one rectangle is correct for every primitive AVK draws today and wrong for
every one M4F adds.

### High-frequency fixtures are mandatory for M4F

Recorded here permanently because it is the lesson that cost the most and is
the easiest to lose.

A flat-colour blur fixture **cannot falsify `sample_exclude` or `darken`.**
Blurring a flat field returns the same flat field, so a blur that samples its
own window's pixels, or one that comes out lighter than its source, is
indistinguishable from a correct one. The same blindness produced the shadow
glow (`f8be42c`) and the shadow-hole bug — both invisible to every flat-backdrop
test that existed at the time.

M4F validation must therefore include: checkerboards; bright thin text-like
blocks on dark grounds; mixed high-frequency content; and sharp luminance
edges. Not as extra cases — as the *primary* ones.

### Advanced Vulkan, first pass

Positions taken at M4E.0 from the audit, to be re-tested against measurement in
M4E.5.

| Technique | Position | Reason |
|---|---|---|
| dynamic rendering | **USE NOW** | already in use; no `VkRenderPass`/`VkFramebuffer` exists to migrate |
| synchronization2 | **USE NOW** | already the only barrier API in the codebase |
| timeline semaphores | **USE NOW** | already the entire lifetime model |
| descriptor indexing | **DEFER** | frame path already allocates zero descriptors; nothing to fix yet |
| secondary command buffers | **DEFER** | one rendering instance, single-threaded recording; no reuse to exploit |
| indirect draws | **REJECT for this workload** | draws are per damage rect with per-command push constants and scissors; there is no uniform batch to indirect |
| memory aliasing | **DEFER** | requires transients to exist and to be measured large first |
| async compute | **DEFER** | needs a measured overlap opportunity; M4F is measured on the graphics queue first |
| pipeline libraries | **DEFER** | 4 pipelines, none compiled on the frame path |

None of these is adopted in M4E on grounds of novelty. Each stays `DEFER` until
a measurement says otherwise.

## M4E.1 — the graph, and what it cost

`src/render/vulkan/graph/avk_graph.{h,c}`, 4 usage classes, ~470 lines.

### The model

```c
avk_graph_add_image(graph, image, foreign, exit)   -> resource index
avk_graph_pass_begin(graph, label, record, user)
avk_graph_use(graph, resource, usage, region)
avk_graph_pass_end(graph)
avk_graph_execute(graph, cb, timestamps, slot)
```

Four usage classes and no more: `COLOR_WRITE`, `SAMPLED_READ`, `TRANSFER_READ`,
`TRANSFER_WRITE`. Each maps to exactly one `(stage, access, layout)` triple in
**one table** in `avk_graph.c`, so a wrong row is wrong everywhere at once and
a right one is right everywhere at once — which is the entire argument for
having a graph rather than barriers at call sites. The public API never mentions
a `VkAccessFlags2`.

Passes run in **declaration order**. There is no dependency solver, no
reordering, no pass culling, no aliasing. The compositor already knows the order
it wants; the graph's job is to make the transitions between them correct and
printable.

### Where the direct path went

`avk_render_frame()` declares one pass, `compose_scene`, writing the target and
reading each distinct sampled image, and hands the rendering instance over as a
callback. `batch_add()` / `batch_submit()` are gone. The rules they encoded did
not change — they moved into `az_barrier_for()` and are now applied to every
pass rather than to the one that happened to exist.

`avk_graph_add_image()` interns by `VkImage`, so two commands sampling one
surface produce one resource and one barrier. That used to be an explicit
duplicate check in the batch builder; it is now a property of the model.

### Cost, measured

| | pre-graph (`b9d7115`) | with the graph |
|---|---|---|
| framebuffer | — | **0 of 6220800 bytes differ** |
| `cpu_frame_us` p50 / p95 | 80–100 / 200 | 100 / 200 |
| `gpu_frame_us_avg` | 1627 µs | 1462 µs |
| `graph_build_ns` | — | **2876 ns** |
| graph allocations after warmup | — | **0** |
| barrier *calls* per frame | 2 | 2 |

`graph_build_ns` is the graph's own work with each pass's record callback
**subtracted**. The first version of that counter included the callbacks and
read 21 450 ns — a real and alarming number, if it had meant what it said. It
did not; it was the draw loop.

The headless GPU figures are unthrottled and differ run to run by more than the
gap between them, so the honest reading is "no measurable change", not "faster".
The assertions in `contrib/avk-graph-test.sh` are order-of-magnitude bounds for
exactly that reason, and the threshold was not chosen before the data.

### The one deliberate behaviour difference

The pre-graph renderer skipped a barrier entirely for a **non-foreign** image
already in the layout it wanted (`batch_add`: `if (!foreign && image->layout ==
new_layout) return;`). The graph re-acquires it each frame.

That skip was an assumption about a producer outside the renderer's knowledge —
a client commit, an SHM upload landing on the *other* command ring, KMS
finishing with a buffer. It happened to hold. Not making it costs one extra
`VkImageMemoryBarrier2` inside a `vkCmdPipelineBarrier2` that was happening
anyway; the number of barrier **calls**, which is what costs a pipeline flush,
is unchanged at 2. Every real client surface here is an imported dma-buf and
therefore foreign, where the old path emitted the barrier too — so on this
machine the difference is unobservable.

### `avk_box` moved

To `avk.h`, from `avk_scene.h`. It is the vocabulary two subsystems that do not
know about each other both need: the scene says where a command lands, the graph
says which part of a resource a pass touches. Neither should include the other
to say "rectangle".

### Three regions, carried and not collapsed

`struct avk_graph_use` holds a region, and `test_regions()` declares a pass that
reads a 56 px box and writes a 24 px one out of a 64 px image. Nothing consumes
those yet — M4E does not implement blur — but the model can **carry** them,
which is the difference between M4F gaining a field and M4F changing the shape
of the graph.

### The dump

```text
BARRIER
    R0 external -> color-write
PASS produce
    WRITE R0 color-write
BARRIER
    R0 color-write -> sampled-read
BARRIER
    R1 external -> color-write
PASS consume
    READ R0 sampled-read
    WRITE R1 color-write
```

Barriers are **logged as they are emitted**, not reconstructed afterwards. A
test that re-derives the answer from the rules the implementation used cannot
fail.

### The break

`BREAK=graph-missing-write-read` (`AZ_GRAPH_NO_WRITE_READ=1`) fails **1 of 46**
unit checks, and fails the one it exists for: *"the write→read barrier exists,
names the colour write as its source"*.

Its first version was **inert**. It skipped the barrier when the layouts already
matched — which for a write→read pair they never do, so it scored a clean 46/46
on the case it was supposed to falsify. The fix was to remove the **dependency**
and keep the **transition**: the barrier is still emitted, the image still
reaches `SHADER_READ_ONLY_OPTIMAL` so the read stays legal, and what goes is the
source scope naming the write it must wait for. That is a missing edge, which is
the bug being modelled; a missing transition is a different one.

It is deliberately **not** listed as a falsifier for `avk-graph-test.sh`: the
direct path has one pass and therefore no inter-pass edge to remove.

### Four ways this fixture measured itself instead of the renderer

Recorded because all four looked exactly like a renderer regression, and three
of them produced a *failing* test for a correct build.

1. **The colour palette is process-global.** `hl_spawn_kitty` walks
   `HL_SPAWN_COLORS` and the index is not reset, so a script running the fixture
   twice got colours 1,2,3 then 4,5,6. Every window background differed:
   **3 214 556 of 6 220 800 bytes**. `hl_reset_spawn_colors()` now exists.
2. **A terminal's text cursor blinks**, so a "settled" desktop is not a still
   image. `HL_KITTY_EXTRA="-o cursor_blink_interval=0"` now exists.
3. **The IPC dispatch names were wrong.** `togglefloating` and `moveresize` are
   *C function* names; the IPC names are `toggle_floating`, `move_window`,
   `resize_window`. `hl_dispatch` reports nothing for an unknown name, so the
   window never floated, no shadow was ever drawn, and the window sat wherever
   the tiling race left it — the residual ~1000 scattered pixels. Same trap as
   the `focusdir` false alarm.
4. **The allocation check ran over zero frames.** A settled headless desktop
   composites nothing, so reading `graph_allocs` twice across an idle gap
   asserted that an unchanging number had not changed.

The one thing that caught all of them was **comparing the current binary against
itself in the same run**. That measurement is now part of the suite: the pixel
assertion is `diff(old, new) <= diff(new, new)`, self-calibrating, with no
tolerance written into the script. When the fixture is reproducible the floor is
0 and the assertion demands exact byte equality — which is what it currently
gets.

## M4E.2 — the transient pool

`src/render/vulkan/graph/avk_transient.{h,c}`.

### The lifetime rule

```c
reusable  <=>  entry->last_use <= avk_device_timeline_value(dev)
```

And when nothing reusable is available, the pool **allocates**. It never waits.
A pool that blocked to save memory would put a CPU wait on the frame path — the
one invariant AVK has kept since M3.5 — and would convert a memory problem into
a latency problem, which is much harder to find afterwards.

The two-outputs case falls out of this for free. A renderer is shared by every
output using its `VkFormat` (M4E.0), so both monitors draw from one pool; when
output B acquires while output A's frame is in flight, A's transient is not yet
past its timeline point and B gets a different image. **A pool keyed on a frame
index would have handed out the same one.**

### It hands out `struct avk_image`

Because everything else already works on one: `avk_pipelines_texture_set()`
caches a sampler descriptor **on the image**, so a reused transient keeps its
descriptor set and the frame path still allocates zero descriptors;
`avk_image_destroy()` is already retire-queue shaped; and the graph takes an
`avk_image` and does not care where it came from. The view is created once with
the image and cached on it, for the same reason the image is: a `vkCreateImageView`
per frame is a per-frame driver allocation, smaller than a `VkImage` and just as
much on the deadline path.

### The key

`format`, `width`, `height`, `usage`, `samples`, `type`, `tiling` — the
properties Vulkan checks when an image is used, and nothing else. Not what it
was last used for, not which output asked, not its current layout (the graph
transitions from whatever it finds).

`usage` is the one an extent check cannot substitute for: an image created
without `SAMPLED_BIT` has the right dimensions and the right format and still
cannot be sampled, and reusing it that way is a validation error rather than a
slow path. `test_key()` asserts exactly that.

### Extent granularity: measured, not assumed

A resize animation asks for 800×500, 810×505, 820×510… With exact extents each
is a different key and therefore a new `VkImage`. The pool rounds the **backing**
extent up to a multiple of 64 and the pass renders into the top-left of a
possibly-larger image using its own viewport and scissor.

Both were run in one process over the same 80-frame resize sweep
(`test_resize_storm`), because arguing about it without the numbers is what the
brief asks not to do:

| | exact extents | granularity 64 |
|---|---|---|
| `VkImage` creates over 80 resize frames | **41** | **6** |
| peak memory | 96 976 KiB | **14 048 KiB** |

Nearly 7× fewer allocations *and* 6.9× less peak memory. The intuition that
rounding up wastes memory is wrong here for a specific reason: the whole sweep
collapses onto a handful of buckets, where exact extents accumulate 80 distinct
images none of which is ever asked for twice. Per-image waste at 1920×1080 is
1920×1088 = **0.7%**.

`AZ_TRANSIENT_GRANULARITY=1` restores exact extents.

### Retirement and budget

Idle entries retire after `AVK_TRANSIENT_IDLE_FRAMES` (120 ≈ 2 s at 60 Hz) —
long enough that a window which stops and restarts an animation does not
reallocate, short enough that a size nothing wants any more does not sit there
for the session. Over budget (256 MB default, `AZ_TRANSIENT_BUDGET_MB`), idle
entries go **oldest-first**, not largest-first: the least recently wanted is the
least likely to be wanted again, whereas the largest is very often the
full-screen intermediate every frame needs.

Everything retires **through the retire queue against its own `last_use`**,
never destroyed in place. `test_drop_in_flight()` drops the whole pool with a
frame still in flight — the output-mode-change case — and asserts the entries
landed on the queue, which is the M3.5 staging lesson applied to a third
resource.

Measured under a deliberately tight 4 MiB budget with 10 distinct sizes: 1 live,
2336 KiB held, peak 5936 KiB, **0 unsafe reuses** — it stayed inside its budget
by retiring, not by refusing to allocate.

### Pressure

500 frames × 4 transients, unthrottled, no waits:

```text
2000 acquires   1981 reuses   19 creates   16 live   peak 4096 KiB
unsafe_reuses = 0
```

19 creates for 2000 acquires: the pool grew to the CPU's lead over the GPU and
stopped. That growth is the allocate-rather-than-wait rule working, not churn —
and the fact that it happened is what makes `unsafe_reuses == 0` worth
something, because the CPU really was running ahead.

### How "never reused early" is checked

**Not by looking for corruption.** The pool increments `unsafe_reuses` as it
commits the violation, and the tests assert on that counter — the same remedy as
the cursor-lifetime invariant: assert the ownership rule directly rather than
waiting for undefined behaviour to become visible.

`BREAK=transient-early-reuse` (`AZ_TRANSIENT_EARLY_REUSE=1`) fails **6 of 33**,
including its own dedicated case. That case is deterministic by construction and
not by racing: the first acquire is released against a timeline point one
million past the device's next, which is **never signalled**, so "has the GPU
finished with it" has a definite answer on any hardware under any load.

### A pre-existing leak this surfaced

`tests/test-avk-render.c` allocated its images with `calloc`, leaving `life` at
0 — and `avk_image_destroy()` correctly **refuses** to destroy an image that
does not read as `AVK_IMAGE_LIVE`. So every image that fixture made leaked, and
`vkDestroyDevice` reported 21 objects still alive. The suite passed throughout,
because nothing it asserted was about teardown. Confirmed present on the
pre-graph build at `b9d7115` with the identical count, so it is not an M4E
regression; fixed here because M4E is the milestone that made validation
cleanliness a stated result.

Whole AVK battery under validation + synchronization validation, after the fix:

```text
transient 33/33   graph 46/46   render 53   shadow 82/82   gradient 177   core 45
0 VUID   0 SYNC-HAZARD   0 leaked objects
```

## M4E.3 — the barrier compiler

The compiler shipped inside M4E.1 (`az_barrier_for()`); this records what it
does and what it deliberately does not.

### Three cases, and the middle one is the requirement

```text
WRITE after anything     always a barrier -- the hazard is real
READ after READ          NOTHING, when the layout already matches
READ after WRITE         a barrier -- the edge M4F cannot do without
```

Read-after-read within a frame emits nothing at all. Two passes sampling one
image are not ordered with respect to each other and do not need to be; the
barrier that made the image visible was emitted before the first of them.
`test_read_after_read()` draws one surface three times and asserts **one**
barrier for it, and that the frame still has **one** `vkCmdPipelineBarrier2`
call. That used to be an explicit duplicate check in the batch builder; it is
now a property of the model.

### Write-after-read is an execution dependency

```c
if (is_write && res->read_only_since_write) {
	src_stage = res->read_stages;   /* every reader since the last write */
	src_access = 0;                 /* readers need no cache flush */
}
```

`srcAccessMask` stays 0 deliberately: a write-after-read hazard requires the
reads to *finish* before the write starts, and nothing needs flushing out of a
reader's cache. Reads are accumulated so one barrier names every reader instead
of one barrier per reader.

### External state is stated, never guessed

- **Entry layout** is read from `avk_image.layout`, which is what the *previous*
  frame's exit barrier wrote. A graph that guessed `COLOR_ATTACHMENT_OPTIMAL`
  would be right about an image it had just rendered into and wrong about every
  one it had not.
- **Entry scope** is `ALL_COMMANDS` / `MEMORY_WRITE`. The previous writer might
  have been an upload on the *other* command ring, a previous frame, or a
  client's own submission arriving through an imported fence. Narrowing it would
  be a guess about a producer outside the graph.
- **Exit** is declared per resource: `AVK_EXIT_FOREIGN` releases to
  `VK_QUEUE_FAMILY_FOREIGN_EXT` in `GENERAL`; `AVK_EXIT_KEEP` leaves it where the
  last pass put it. The image's own `layout` is written back either way, so the
  next frame's entry state is the truth.

### One narrow layout exception

A **foreign colour attachment** stays in `GENERAL` — it is a scan-out buffer,
KMS has to be able to read it, and entering `COLOR_ATTACHMENT_OPTIMAL` would mean
transitioning in and straight back out. A foreign **sampled** image does go to
`SHADER_READ_ONLY_OPTIMAL`: the read covers the whole surface every frame the
window is visible, so the two transitions pay for themselves. This is also
exactly what the pre-graph renderer did, which is why the direct path's barriers
are unchanged in kind and in count.

### Counters

```text
graph_passes  graph_resources  graph_uses  graph_barriers
graph_image_transitions  graph_buffer_barriers  graph_allocs
graph_frames  graph_build_ns_avg
```

all on `amsg get avk-stats`. `graph_barriers` counts `vkCmdPipelineBarrier2`
**calls** — the thing that costs a pipeline flush — while
`graph_image_transitions` counts the `VkImageMemoryBarrier2` structures inside
them.

`graph_buffer_barriers` is **structurally zero**, and is reported anyway. The
two buffers AVK owns are already ordered without one: the gradient storage
buffer and the command buffer both live in a per-frame slot that
`avk_cmd_ring_begin()` has already waited on, so there is no hazard for a
barrier to resolve, and SHM staging is ordered by the submission that reads it on
a different ring. A counter reading zero says that on purpose; no counter would
make the absence something a reader has to notice, and would leave a future
buffer resource silently unaccounted for.

### No fake barriers

Every barrier corresponds to a hazard or a layout transition. Nothing was added
to quiet validation — the direct path emits **2** calls per frame on a real
output (acquire, then the foreign release), the same as before M4E, and the
topology dump is readable by inspection:

```text
BARRIER
    R0 external -> color-write
PASS produce
    WRITE R0 color-write
BARRIER
    R0 color-write -> sampled-read
```

### Same queue

Everything stays on the single graphics queue. No async compute, no cross-queue
ownership transfers. The device has a dedicated transfer family and a dedicated
compute family, both recorded in `avk_device.caps` and deliberately not turned
into queues — an unused `VkQueue` is infrastructure claiming to exist before
anything drives it. M4F is measured on the graphics queue first.

## M4E.4 — a multipass frame, before anything needs one

`tests/test-avk-multipass.c`. **Test infrastructure only.** Nothing here ships:
M4E is architecture, and a user-visible "M4E effect" would be a feature nobody
asked for that has to be maintained forever.

### The fixture

```text
PASS A  "pattern"     fill transient R0 with four known quadrant colours
BARRIER               R0 color-write -> sampled-read
PASS B  "composite"   sample R0's TOP-RIGHT quadrant into R1's BOTTOM-LEFT
```

Source and destination rectangles differ on purpose. A pass that ignored one of
the two would still produce a plausible picture if they matched, so every wrong
implementation gets a **nameable** wrong answer:

| what went wrong | what the readback shows |
|---|---|
| sampled the wrong quadrant | red, blue or yellow instead of green |
| ignored the destination rectangle | the whole target covered |
| ran before pass A | black where green should be |
| transient handed out early | black — pass A's clear races the sample |
| region off by a pixel | a hard edge in the wrong place |

Measured: green `(0,255,0)` at the destination, `(0,0,0)` everywhere else,
corner to corner, stopping exactly at the edge.

### Topology

```text
BARRIER
    R0 external -> color-write
PASS pattern
    WRITE R0 color-write
BARRIER
    R0 color-write -> sampled-read
BARRIER
    R1 external -> color-write
PASS composite
    READ R0 sampled-read
    WRITE R1 color-write
```

Two passes, two resources, three uses, **two barrier calls — one per pass**.
Not one per resource, and not one up-front batch: pass B's dependency cannot be
emitted before pass A has been recorded. The `READ` and `WRITE` regions are
declared separately on the same resource, which is the shape M4F needs and which
no primitive AVK draws today produces.

### Reuse and pressure

| | |
|---|---|
| 120 paced multipass frames | **0** new images, 0 unsafe reuses, pixels still right |
| 400 unthrottled frames | 521 acquires, 4 creates, 4 live, **0 unsafe reuses** |

### The breaks

| break | fails | how |
|---|---|---|
| `graph-missing-write-read` | **2 of 19** | the topology assertion on the barrier's source scope, **and** 10 `SYNC-HAZARD-WRITE-AFTER-WRITE` from synchronization validation |
| `transient-early-reuse` | **1 of 18** | 399 of 400 acquires counted as unsafe |

Two independent signals for the graph break: one from what the graph *decided*,
one from what the GPU driver's validation says about the result. Neither depends
on corruption happening to be visible.

Across the suites: `graph-missing-write-read` fails 1/47 unit + 2/19 multipass;
`transient-early-reuse` fails 6/33 unit + 1/18 multipass.

### A premise that only held with the layers off

`test_pressure()` first asserted `ring.stalls > 0` — the CPU outrunning the GPU
— as proof that transients were genuinely in flight when the next frame asked
for one. It read **224 stalls normally and 0 under validation layers**, because
validation slows the CPU enough that the GPU keeps up.

A premise that holds only when the layers are off stops holding in exactly the
run where correctness is checked hardest. It was replaced with a volume premise
(the loop ran), and the in-flight case is proved deterministically elsewhere
instead — `test_two_in_one_frame()` acquires twice within one frame and asserts
the images differ, and the break's own case releases against a timeline point
that is **never signalled**, so "has the GPU finished" has a definite answer on
any hardware under any load.

## M4E.5 — what it costs, and a comparison that was not one

### A measurement error, and the correction

M4E.1's commit reported the desktop as byte-identical to the pre-graph binary.
**That measurement was invalid**, and it is worth recording how.

`contrib/lib/headless.sh` resolves the compositor binary once, at *source* time:

```sh
HL_ASTEROIDZ="${ASTEROIDZ:-$HL_REPO/build/asteroidz}"
```

So `ASTEROIDZ=<old> hl_start` — which is what both new fixtures did — sets a
variable nothing reads again. Both runs launched the **current** build. The
comparison was the new binary against itself, and it reported a clean result
that said nothing whatever about the old one.

It was caught by `record_us_avg`: an M4E-only field that the pre-graph binary
cannot expose, and which was nonetheless present in the "pre" column of the
perf table. A missing field showing a number is a fact about the fixture, not
about the code.

Fixed by setting `HL_ASTEROIDZ` directly, by having `hl_start` record what it
launched (`hl_binary()`), and by **asserting the premise**:

```text
ok - the comparison really used the pre-graph binary
```

Re-run with the correct binaries, the answer is the same — but now it is an
answer:

```text
noise floor (this binary vs itself):   112 of 6220800 bytes
pre-graph vs graph:                      0 of 6220800 bytes
```

The graph differs from the pre-M4E renderer by **less than the current binary
differs from itself between runs**. `record_us_avg` now reads `n/a` in the pre
column, which is itself evidence the right binary ran.

### Cost, on four scenes

Each scene run on both binaries in one invocation, alternating, so machine state
and thermal drift land on both rather than on whichever went second.

| scene | resources | `graph_build_ns` | cpu p50 pre→graph | cpu p95 pre→graph | gpu µs pre→graph |
|---|---|---|---|---|---|
| idle (1 window) | 5 | **2270** | 60→80 | 160→140 | 1811→1873 |
| many (16 windows) | 50 | **7239** | 320→480 | 4300→2020 | 2001→1986 |
| gradient borders | 26 | **5339** | 120→140 | 880→840 | 1955→1997 |
| shadows (6 floating) | 8 | **2492** | 100→180 | 180→180 | 1421→1435 |

Every scene: **1 pass, 2 barrier calls.** A frame with no multipass effect has
not become a pipeline, however many windows it contains.

`graph_build_ns` is linear in declared resources — about **110 ns per resource
plus 1.7 µs** — and is the only column here that is stable enough to quote. At
144 Hz the 16-window worst case is 7.2 µs, **0.10% of a 6944 µs budget**.

The `cpu_frame_us` columns move in both directions between runs by more than
the gap between binaries (an earlier sweep of the same scenes read 540→240 where
this one reads 320→480), so the honest statement is *no measurable change*, not
a win or a loss in either direction. That is why the assertions are
order-of-magnitude bounds and why no threshold was chosen before the data.

The **gradient scene is a stress case, not a calm one**: `border_gradient 1`
triggers a known repaint storm — `client_set_border_fill()` has no dirty check,
so it damages the border node every tick — which reproduces on both renderers
and on pre-M4C builds and is documented in `contrib/avk-gradient-test.sh`. It is
used anyway because it is the only way to get gradients into the renderer
headlessly. Both binaries storm identically, so the comparison holds; the
absolute numbers should be read knowing that.

### Cross-output independence

`avk-graph-test.sh` under `HL_OUTPUTS=2`: **20/20**, with one pass, two barrier
calls and a byte-identical framebuffer while two outputs share one renderer —
and therefore one command ring, one graph, one transient pool and one timestamp
pool. No cross-output waits were added and none was needed: the timeline orders
the two outputs' submissions and nothing else has to.

### The pool is in the binary, and nothing acquires from it

`avk_renderer` now owns a `struct avk_transient_pool`, initialised at renderer
init, collected in `avk_renderer_collect()`, and released against the frame's
timeline point in `avk_render_frame()`. **M4F is its first consumer**; every
`transient_*` counter reads 0 today.

It ships now rather than with the first effect that needs it because the
alternative is shipping a lifetime mechanism at the same moment as the thing
that stresses it, which is how a lifetime bug and an effect bug arrive together
and get debugged as one. An empty pool costs one branch in `collect()`.

An **output resize or mode change** therefore needs no special handling: entries
keyed on the old extent are never asked for again and retire on the idle path
like any other size. `avk_transient_pool_drop_all()` exists for the case where
waiting 120 frames is too slow, and is tested with a frame still in flight.

### Regression

```text
UNIT       graph 47   transient 33   multipass 18   shadow 82   gradient 177
           render 53  core 45   timestamp 21   corner-permute 19   dmabuf   isolation

HEADLESS   border 118   shadow 28   crossoutput 18   frame 17   idle-convergence 14
           damage 12   teardown 11   rounded 10   sync 10   cursor 10
           persist(BORDER=6) 9   shm-partial 23   shm-cache 7   dmabuf-feedback 7
           rounded-alpha 6   gradient-crossoutput 5   damage-domains(x2) 5
           clip-policy 4   graph 20   graph(x2 outputs) 20   graph-perf 20

VALIDATION 0 VUID   0 SYNC-HAZARD   0 leaked objects
```

`avk-gradient-test.sh` reports **0/0 and says so**: a headless overview creates
no vignettes and `border_gradient` storms, so the suite claims no coverage
rather than manufacturing a pass. Pre-existing, unchanged by M4E.

### Breaks

| break | unit | multipass | headless |
|---|---|---|---|
| `graph-missing-write-read` | 1/47 | 2/19 | not listed — the direct path has one pass and no inter-pass edge |
| `transient-early-reuse` | 6/33 | 1/18 | not listed — nothing in production acquires yet |

Both are listed as *not applicable* to the headless suite rather than quietly
omitted, because a break that cannot fail a suite must not be claimed as
coverage for it.

### Advanced Vulkan, re-tested against the measurements

| technique | decision | why |
|---|---|---|
| dynamic rendering | **USE NOW** | already in use; no `VkRenderPass`/`VkFramebuffer` exists anywhere in AVK |
| synchronization2 | **USE NOW** | already the only barrier API |
| timeline semaphores | **USE NOW** | already the entire lifetime model, and what makes transient reuse cross-output-safe |
| descriptor indexing | **DEFER** | the frame path allocates and writes **zero** descriptors; a transient's set is cached on its `avk_image` like any other. Nothing to fix |
| secondary command buffers | **DEFER** | one rendering instance, single-threaded recording. Graph build is 2–7 µs; there is no recording cost to parallelise yet |
| indirect draws | **REJECT for this workload** | draws are per damage rect with per-command push constants and scissors. There is no uniform batch to indirect |
| memory aliasing | **DEFER** | peak transient memory in the resize stress test is 14 MB. Aliasing solves a problem that does not exist |
| async compute | **DEFER** | needs a measured overlap opportunity. M4F is measured on the graphics queue first, and the dedicated compute family stays unqueued |
| pipeline libraries | **DEFER** | 4 pipelines, none compiled on the frame path |

Nothing was adopted on grounds of novelty, and every `DEFER` now has a number
attached rather than an intention.

## A counter that had to be split twice

`graph_build_ns` said something false twice, and both times the number looked
like a finding.

**First: it included the pass record callbacks.** 21 450 ns a frame. That is the
draw loop — work the frame was always going to do. Subtracted, it fell to
~2500 ns.

**Second: it included `vkCmdPipelineBarrier2`.** Headless it read 2270–7239 ns
and looked fine. On the live session it read **35 330 ns average, 42 517 ns
incrementally** — 20% of frame recording, and 6–20× the headless figure. That
reads exactly like a graph that falls apart on real hardware.

It was not. The pre-graph renderer made **the same two
`vkCmdPipelineBarrier2` calls, in the same places, with the same contents**;
what M4E changed is that they are derived rather than hand-written. Their cost
simply had nowhere to be attributed before, so it had never been looked at.

Split out as `graph_barrier_ns`:

| | graph bookkeeping | `vkCmdPipelineBarrier2` |
|---|---|---|
| unit test, 64×64 | **329 ns** | ~2 100 ns |
| headless, 3840×2160 | **1 334 ns** | 1 796 ns |

So the part M4E actually added is **~0.3–1.3 µs a frame**, and the part that
dominates is a driver call that predates it.

The debug labels were ruled out first, by reading `avk_debug.c`: they return on
a NULL function pointer when `ASTEROIDZ_VK_DEBUG` is unset, so they cost a
branch.

**Why the live figure is so much larger than 4K headless.** 4K headless does not
reproduce it — combined it reads ~3.1 µs there against ~42 µs live. The live
session's `record_us_avg` is 180–215 µs against ~48 µs headless, so the whole
frame path is roughly 4× slower on a loaded desktop; `clock_gettime` measures
wall time, so scheduler preemption inside the timed region is counted. The
useful statements are the ratio (graph + barriers ≈ 20% of frame recording) and
the budget (record time is ~180 µs of 6944 µs, **2.6%**), not the absolute
nanoseconds.

### And a capture that failed silently

`pngdiff` let a `zlib.error` from a truncated grim capture through, producing
empty `FLOOR`/`FTOTAL` values which awk then compared without complaining — a
comparison against nothing, reported as a pass. It now returns empty on an
unreadable file and the caller **skips with a reason** rather than proceeding.
grim writes a truncated PNG under load often enough that this matters.

## M4F.3/.5 — what `vkCmdPipelineBarrier2` actually costs

`tests/test-avk-barrier-cost.c`. One variable at a time, same GPU, same
process, back to back — because a live compositor varies in all of them at once
and can only ever produce another hypothesis.

### The answer

```text
1 barrier,  1 call                    69 ns
4 barriers, 1 call                   198 ns      (50 ns per barrier)
4 barriers, 4 calls                  256 ns      (64 ns per call)

dma-buf, IGNORED queue families       51 ns
dma-buf, FOREIGN -> graphics          44 ns      <- ownership transfer is FREE

non-DCC  0x0200000028a01f04           46 ns
DCC      0x0200000028a37f04           46 ns      <- ratio 0.99x
                                                    (the exact live modifier)
3840x2160                             61 ns
256x256                               51 ns

GENERAL -> GENERAL                    47 ns
GENERAL -> SHADER_READ_ONLY           59 ns
COLOR_ATTACHMENT -> SHADER_READ_ONLY  58 ns
UNDEFINED -> COLOR_ATTACHMENT        311 ns      <- the only expensive one
```

**A barrier costs tens of nanoseconds.** Not microseconds.

### DCC HYPOTHESIS: DISPROVED

M4E reported ~1.9 µs headless and ~44 µs live for the same two calls, and named
DCC-compressed scan-out as the leading explanation on the strength of the two
paths using different modifiers.

The exact live modifier — `0x0200000028a37f04`, DCC with `DCC_RETILE`, three
planes — measures **46 ns against 46 ns** for the same-size non-DCC modifier on
the same device. Compression makes no difference to barrier recording cost.
Neither does the foreign queue-family transfer, which was the other candidate.

### So what was the 44 µs?

**The instrument.** A bracketing `clock_gettime` pair costs ~37 ns around a
~60 ns event, so the measurement perturbs its subject by more than half — and
any scheduler slice landing inside that window is charged entirely to it. On a
loaded desktop the *mean* of such a sample is a measure of preemption.

The same per-call technique in a quiet loop reads:

```text
p50 60 ns   p95 70 ns   p99 71 ns   max 200 ns   mean 61 ns
```

Mean ≈ median, so the technique is sound and the environment was not.
`clock_gettime` itself was ruled out first: 18.7 ns via vDSO on a TSC
clocksource, not a syscall.

### What changed

- Per-call barrier timing is **removed** from production. An instrument that
  perturbs its subject by 60% and whose mean measures the scheduler is not an
  instrument.
- `graph_build_ns` folds barrier emission back in — at this granularity they are
  inseparable and both are graph work — and is reported as a **distribution**:
  `graph_build_ns_p50` / `_p95` / `_p99`, in 250 ns buckets.
- `avk-graph-test.sh` asserts on the **median**, not the mean.

### Consequences for M4F

Barriers are effectively free. A blur adding four barrier flushes costs
**~250 ns of recording**, not the ~90 µs M4E's figure implied. Synchronisation
is not a constraint on the blur's design, and the batching question (M4F.4) is
worth ~58 ns per four barriers — real, measured, and not worth contorting the
graph for. `UNDEFINED -> COLOR_ATTACHMENT` at 311 ns is the one transition
worth avoiding, which argues for reusing a transient in a known layout rather
than letting it come back as `UNDEFINED`.

### Four ways this benchmark measured nothing before it measured something

Each produced a confident "no DCC modifier available on this device" while the
live compositor was scanning one out.

1. **Filtered on `plane_count == 1`.** Every DCC modifier here carries 2–3
   planes — the compression metadata — so the filter rejected exactly the case
   under test.
2. **Took the first matching modifier and gave up when gbm refused it**, with
   five more in the list. `gbm_bo_create_with_modifiers2` takes an array
   because it is meant to choose.
3. **Asked for `GBM_BO_USE_SCANOUT`**, which needs the KMS primary node; the
   test opens a render node so it can run with no display.
4. **Printed the diagnostics before the section header**, so a `sed` window over
   the results hid them.

## M4F.0 — the production effect boundary

### What M4F is

`docs/avk-effects.md`'s own stage table has said so since M4A:

```text
M4F  blur   dual-Kawase, source-region expansion, and every one of the
            per-node fields above
```

So M4F is **blur**, and nothing else. Not bloom, not local tone mapping, not
glass — those are named in the M4E brief as *possible future consumers* of the
graph, and adding one here would be expanding the milestone.

### Every blur producer in the compositor

Seven call sites create a blur node. They are two different things wearing one
name.

| producer | node type | what it blurs |
|---|---|---|
| `ensure_monitor_blur_node()` | `OPTIMIZED_BLUR` | one per monitor: a **cached** full-output blur of the bottom (wallpaper) layer, invalidated by `mark_dirty` |
| `client_update_blur()` | `BLUR` | a window's backdrop, below its own surface |
| `client.h:680` | `BLUR` | `c->shadow_blur` — the backdrop inside a shadow's footprint |
| `layer_update_blur()` | `BLUR` | a layer surface's backdrop |
| `layer.h:221` | `BLUR` | `l->shadow_blur` |
| `asteroidz.c:4121` | `BLUR` | an xdg popup's backdrop |
| `overview.h:1309` | `BLUR` | the overview strip |

`OPTIMIZED_BLUR` is a **cache**, with its own dirty tracking and its own
lifetime; `BLUR` is a **live** effect that samples the scene as composed *at
that point in the command stream*. A node with
`should_only_blur_bottom_layer` reads the cache instead of sampling live, which
is why the two are entangled rather than independent.

### The live configuration, which decides what is actually visible

```text
blur { enable 1; layer 0; optimized 1; passes 2; radius 6;
       unfocused-strength 0.7; transparency-threshold 0.5
       params { noise 0.02; brightness 0.9; contrast 0.9; saturation 1.2 } }
shadow { blur-background 1; blur-background-strength 0.55 }
```

So on this desktop the visible blur is: the **cached** monitor blur, sampled by
every **non-floating** window (`should_only_blur_bottom_layer = blur_optimized
&& !isfloating`), plus **live** blur for floating windows, plus a **live**
shadow-backdrop blur with the darken clamp. Layer-shell blur is off.

> The config's own comment argues for `optimized 0` and then sets `optimized 1`.
> Worth resolving separately; it does not change what M4F must implement.

### The dependency map

```text
scene / window
      |  client_update_blur(), layer_update_blur(), shadow producers
      v
wlr_scene_blur / wlr_scene_optimized_blur node
      |  fields: corners, clipped_region, strength, alpha, sample_exclude,
      |          darken, clip_region, edge_softness, transparency_mask_source,
      |          should_only_blur_bottom_layer
      v
az_avk_walk_node()                       <-- TODAY: recognised and DROPPED
      |
      v
avk_cmd  (AVK_CMD_BLUR, does not exist yet; `has_blur` is a reserved flag)
      |
      v
avk_graph_add_image / avk_graph_pass_begin / avk_graph_use
      |
      v
avk_transient_pool  (down/up chain)
      |
      v
barrier compiler  -> vkCmdPipelineBarrier2
      |
      v
output
```

### What bypasses `avk_graph` today

**Nothing inside AVK.** M4E put the whole frame through the graph: there is one
pass, its barriers are derived, and `batch_add`/`batch_submit` are gone. The
only Vulkan work outside the graph is the **SHM upload path**
(`avk_dmabuf_importer.upload_ring`), which is a separate submission on a
separate command ring, ordered by the semaphores its caller passes. That is an
audited external boundary, not a parallel synchronisation system, and M4F does
not touch it.

So M4F adds a consumer; it does not have to reclaim anything first.

### The one hard architectural question

A live blur samples **the target as composed so far**. AVK's frame is a single
`vkCmdBeginRendering`…`vkCmdEndRendering` instance, and an image cannot be an
attachment and a sampled source at once. Three shapes are possible:

```text
A  split the rendering instance at each blur node
       end rendering, transition target -> sampled, blur into transients,
       transition back, resume. N blur nodes = N splits.

B  render the background into a transient first, then compose forward
       one split, but every window below a blur is drawn twice or the
       composition order changes.

C  cache the bottom layer once per output (what OPTIMIZED_BLUR already is)
       and have consumers sample it -- no split at all in the common case.
```

The barrier measurement (M4F.3/.5) matters here: a split costs **~130 ns** of
recording, so A is not expensive *in synchronisation*. What A does cost is a
full-target read per blur node. C is what the live config already selects for
non-floating windows, and it is a cache rather than a pass.

`sample_exclude` exists because of a specific failure in shape A: the blur node
is drawn *below* its own window, so the target does not yet contain the window
— **except in undamaged regions, which still hold the previous frame, where it
does.** That is the halo SceneFX's field comment describes, and it is the reason
the region a blur samples cannot simply be "whatever is in the target".

## M4F.1 — the blur primitive

`src/render/vulkan/effect/avk_blur.{h,c}` plus `blur.glsl`, `blur_down.frag`,
`blur_up.frag`. Two pipelines, both **replacing** rather than blending.

### Dual-Kawase, and why not a separable Gaussian

A separable Gaussian of radius R is 2 full-resolution passes and ~2(2R+1)
samples per pixel, growing **linearly in R**. Dual-Kawase buys radius with
**resolution**: each level halves the image with a fixed 5-tap kernel and each
upsample doubles it with a fixed 8-tap one, so support doubles per level while
cost falls geometrically. Four draws total ≈ **1.7×** one full-resolution pass,
for a support a Gaussian would need ~30 taps to match.

The decisive argument is not performance though — it is that asteroidz's config
is *already expressed in dual-Kawase terms* (`passes`, `radius`). A Gaussian
would have to reinterpret both and change every existing desktop.

Stated tradeoff: it is an approximation, slightly boxier, with faint ringing on
an impulse. **Measured** (0 codes of non-monotonicity at 2 levels) rather than
asserted away.

### It owns no synchronisation

There is no `vkCmdPipelineBarrier2` in `avk_blur.c`, no layout helper, no wait.
It declares `avk_graph_use()` and the compiler derives everything — which is why
a 3-level blur (6 passes, 6 barrier calls) needs no more synchronisation
knowledge than a 1-level one. Asserted, not grepped:

```text
six passes (6)   six barrier calls, one per pass (6)   no buffer barriers
every intermediate came from the transient pool
60 further blurs allocated ZERO new images (3 -> 3)
```

### The bug the high-frequency fixtures caught

**The transient pool rounds allocations up to 64 px.** A 32×32 blur level lives
in a 64×64 image whose other three quarters were never written — and sampling
the full `[0,1]` UV range averages in unwritten memory.

```text
                       before          after
flat 100               19              100
lines mean (in 31.9)   7.7             32.0
checkerboard mean      —               128.0  (input 127.5)
level reach 1/2/3      4 / 0 / 0 px    4 / 7 / 9 px
```

A factor of four of energy gone, and levels 2 and 3 producing *nothing*.
`CLAMP_TO_EDGE` does not help — it clamps to the **allocation** edge, which is
the padding. Three things had to change together:

- the quad spans `logical/allocation` of the UV range, not all of it;
- a texel is `1/allocation` in UV, so the half-texel step uses that;
- every tap is clamped to the last valid texel centre, `(logical−0.5)/allocation`.

This is exactly M4F.8's "the shader must know the logical valid extent
separately from the allocation extent", and it is why that requirement exists.

**The flat-colour fixture passed the broken build at every stage until the
energy loss became total.** The checkerboard, the impulse and the line fixtures
caught it immediately. That is the M4E lesson holding.

### Measured behaviour

| fixture | result |
|---|---|
| 1 px checkerboard | spread 255 → **0**, mean **128.0** (input 127.5) |
| impulse | symmetric to **0 codes** in both axes; worst non-monotonic rise **0** |
| hard edge | **0** drops; 10–90% transition **8 px** |
| 1 px lines / 8 px | spread **0**, mean **32.0** (input 31.9) |
| levels 1/2/3 | reach **4 / 7 / 9 px** — radius really does come from levels |
| flat (secondary) | 100 → **100** |
| brightness 0.5 | 128 → **64**, and black stays exactly black |

`avk_blur_support()` reports **25 px** where the edge measures 8 — deliberately
conservative, because it feeds damage expansion and under-covering leaves a
stale fringe that only appears on a moving window.

### Not yet wired

No scene node produces a blur command yet: `WLR_SCENE_NODE_BLUR` and
`OPTIMIZED_BLUR` are still dropped by the walker. The primitive is validated
standalone first, deliberately — the alternative is debugging a kernel bug and a
scene-integration bug as one.

## M4F support — derived, not fitted

`avk_blur_support_of()` returns four edges (`left/right/top/bottom`). Dual-Kawase
is symmetric so all four are equal today; the type exists so a directional effect
changes numbers rather than every caller's signature.

### The derivation

Per axis, in texels of the level being sampled:

```text
DOWNSAMPLE  level i-1 -> i     A = 0.5 * radius + 1   texels of level i-1
UPSAMPLE    level i -> i-1     B =       radius + 1   texels of level i
```

`0.5 * radius` and `radius` because the 5-tap kernel offsets by `step` and the
8-tap kernel's *axis* taps offset by `2 * step`, where
`step = 0.5/allocation * radius` — which is `0.5 * radius` texels. The `+1` is
the **bilinear footprint**: a filtered fetch at position `p` draws on texel
centres within `[p−1, p+1]`.

A level-`i` texel spans `width / level_extent(width, i)` **source** pixels —
computed, not assumed to be `2^i`. At 129 px over 64 texels that is 2.016, and
assuming the power of two under-covers on every odd extent.

Summed along the chain (down 1..N, up N..1), plus one pixel for a fractional
origin, rounded outward **once** at the end. It is an upper bound because
offsets compose additively and every tap is additionally clamped into the valid
region by the shader.

### Old vs new vs measured

| levels | radius | old | **derived** | measured | margin |
|---|---|---|---|---|---|
| 1 | 1 | 9 | **7** | 3 | 4 |
| 2 | 1 | 25 | **18** | 8 | 10 |
| 3 | 1 | 57 | **40** | 18 | 22 |
| 2 | 3 | — | **33** | 21 | 12 |
| 2 | 6 | — | **55** | 40 | 15 |

**28–30% tighter than the old guess**, and now parameter-dependent.

### `BREAK=blur-support-minus-1` cannot fail, and that is the correct answer

The tightest margin is **4 px**, so a one-pixel shrink still covers. That is a
fact about the bound rather than a weakness in the test:

- the **derived** support is a mathematical upper bound on which source texels
  can contribute;
- the **measured** reach is where that contribution still survives 8-bit
  quantisation.

Those are different questions and the first is legitimately larger. Damage
correctness is binary — *if a texel can contribute, it belongs in the region* —
so the bound stays. The brief's own alternative applies: the falsifier is
insufficient, not the bound.

What replaced it is a check that **can** fail and runs every time: declare a
support one pixel *inside the measured reach* and require the same coverage
assertion to reject it. A coverage test that cannot detect an undersized region
is not a coverage test.

### Measuring reach needs a block, not an impulse

An impulse is the mathematically ideal probe and the wrong practical one. One
lit pixel spread over a 55 px support falls below 1/255 long before it reaches
the edge of its own footprint, so the measurement floor is the **quantiser**.
Measured that way, radius 3 and radius 6 both reported a reach of **zero** —
which reads as a perfectly tight bound and is in fact a blind instrument. A
24×24 block carries ~576× the energy with the same sharp boundary.

## M4F padding — poison, and the extents that expose it

`AZ_TRANSIENT_POISON=1` fills every newly created transient with **magenta**
across the whole allocation, including the padding a caller's logical extent
never covers. Relying on that memory being zero is relying on an accident: it is
whatever the driver last left there, which on a reused image is another window's
blur.

`TRANSFER_DST` is added to the VkImage under poison only, and **not to the pool
key** — an image with more usage is still substitutable, so a poisoned run pools
exactly as a normal one does.

Extents tested, at 1, 2 and 3 levels each — **30 combinations**:

```text
63  64  65  100  127  128  129  192  17  8
```

63 and 65 straddle the 64 px granularity; 129 allocates 192 and leaves 63
columns of padding; 17 and 8 are the narrow cases where the level count reduces.
A blur of pure black must come back pure black.

```text
30 extent/level combinations, 0 leaking pixels
0 VUID, 0 SYNC-HAZARD with poison + synchronization validation
```

Four concepts are now kept distinct and must stay so: **allocation extent**,
**valid extent**, **sampling extent**, **logical effect extent**. The shader
never infers the valid bound from the allocation size.

## M4F.2A.0 — blur node semantics, re-audited

Re-read from source at HEAD, not from the M4F.0 notes.

### The node

```c
struct wlr_scene_blur {
    struct wlr_scene_node node;
    int width, height;                    /* node-local extent */
    struct fx_corner_radii corners;
    struct clipped_region clipped_region; /* box + radii */
    float strength;                       /* 1.0 -> 0.0, relative to base */
    float alpha;
    bool should_only_blur_bottom_layer;   /* read the cache instead */
    bool has_sample_exclude;
    struct wlr_box sample_exclude;        /* NODE-LOCAL */
    bool darken;
    struct linked_node transparency_mask_source;
    pixman_region32_t clip_region;        /* NODE-LOCAL, pixel-accurate */
    bool has_clip_region;
    float edge_softness;                  /* 0 = hard SDF edge */
};
```

Twelve setters exist. **What asteroidz actually calls**, traced:

| setter | caller | value |
|---|---|---|
| `set_size` | every producer | node-local extent |
| `set_corner_radii` | shadow backdrop | the window's own radii |
| `set_sample_exclude` | `client.h:569` | the window's box, node-local |
| `set_edge_softness` | `client.h:581`, `layer.h:331` | the shadow's own `blur_sigma` |
| `set_darken` | `client.h:700`, `layer.h:233` | `shadows_blur_background_darken` |
| `set_alpha` / `set_strength` | open animation, unfocused | 0..1 |
| `set_region` | `asteroidz.c:4066/4138` | the client's ext-background-effect region |
| `set_should_only_blur_bottom_layer` | `client_update_blur` | `blur_optimized && !isfloating` |

`transparency_mask_source` is set by `layer_update_blur` only. No producer calls
`set_clipped_region` with anything but the default except `client_update_blur`,
which derives it from the effect region's **extents**.

### The finding that changes the design

`client.h:552` states it outright, in the producer:

> *"The blur's box is the shadow's, which is the window plus its spread — so the
> region it samples covers the window itself, and the scene image holds the
> PREVIOUS frame there (this node draws beneath the window, and an undamaged
> area is never re-rendered). Without this the blur picks up the window's own
> pixels and spreads them outward: a halo in the window's own colour."*

So in the reference, **a live blur samples the output framebuffer, and in
undamaged regions that framebuffer holds the previous frame.** The source is not
the current scene prefix — it is last frame's *final composite*, window and all.

`sample_exclude` exists to paper over exactly that: it overwrites the window's
box with an unblurred bottom-layer snapshot before blurring, because the real
content there is a frame old and wrong.

**This is frame history, and the brief forbids it as AVK's canonical path.**

### What that means for AVK

If AVK captures the **true current-frame scene prefix** — everything below the
blur node, rendered this frame — then the window is genuinely not in the source,
because it has not been drawn yet. There is no halo to exclude and no snapshot
to substitute.

That makes the two implementations differ in a way worth stating precisely:

```text
SceneFX     source = previous frame's FINAL COMPOSITE
            sample_exclude = a repair for content that should not be there

AVK         source = this frame's SCENE PREFIX
            sample_exclude = a genuine semantic: a box whose content must not
                             contribute even though it IS legitimately behind
```

`sample_exclude` is therefore still implemented — a producer may legitimately
want a window below the blur node excluded — but its *primary* job in the
reference disappears, and the failure it was invented to fix becomes
structurally impossible rather than repaired.

**That also relocates the falsifier.** A `sample_exclude` test against the AVK
path cannot fail by producing the reference's halo, because the halo has no
source. The break that must exist instead is one that captures the **final
composited output** rather than the prefix — `BREAK=blur-scene-after` — which
reintroduces exactly the reference's defect and must show the halo on a
high-frequency dark fixture.

### Coordinate spaces, pinned

```text
width/height        node-local, logical
sample_exclude      node-local  (client.h subtracts the node origin explicitly)
clip_region         node-local, pixel-accurate
clipped_region      node-local box + radii, the bounding-box form
corners             node-local
edge_softness       a SIGMA, matched to the shadow's blur_sigma
strength, alpha     unitless 0..1
```

`edge_softness` is set to the shadow's own `blur_sigma`, which M4D established
is a **logical** value the reference leaves unscaled while scaling the box —
and which AVK deliberately scales to output pixels (M4D divergence 1). The same
divergence must apply here or a blur's edge and its shadow's edge will disagree
on a fractional-scale output.

### The capture architecture this implies

```text
PASS 0    compose commands [0, k)          the scene prefix
BARRIER   target -> sampled-read
PASS      down 1 samples THE TARGET directly   (no separate capture copy)
          down 2..N, up N..1  -> transient R
BARRIER   R -> sampled-read
PASS 1    composite R with corners/clip/alpha/edge_softness,
          then commands (k, n)
```

The first downsample reads the target itself rather than a captured copy, which
removes a full-resolution read *and* write per blur node — the capture is the
downsample. Only the blur's **dependency region** is read, not the output.

## `sample_exclude` — the producer table

The brief requires every producer enumerated before the field's treatment is
finalised. There is exactly **one**.

| | |
|---|---|
| **producer** | `src/animation/client.h:569`, in the shadow-backdrop blur setup |
| **value** | `{ client_box.x − (shadow_box.x + left_offset), client_box.y − (shadow_box.y + top_offset), client_box.width, client_box.height }` |
| **owning node** | `blur_backdrop`, positioned at `shadow_box + offsets`, sized to the shadow box minus those offsets |
| **relationship** | the box is **exactly the owning window's own footprint**, expressed in the blur node's local coordinates by subtracting the node origin |
| **reason** | stated in the producer: the blur's box is the shadow's — window plus spread — so what it samples covers the window, and *"the scene image holds the PREVIOUS frame there (this node draws beneath the window, and an undamaged area is never re-rendered)"* |

No other call site sets it. `layer.h` sets `darken`, `alpha` and
`edge_softness` on its shadow blur but **never** `sample_exclude`.

### What the reference actually does with it

`fx_pass.c:1500` — and this corrects my own earlier reading:

> *"EDGE EXTENSION, not substitution — see `blur_exclude_from_source()` in
> vulkan/pass.c for why the wallpaper snapshot that used to go here was its own
> halo."*

It stages a patched **copy** of the screen and fills the excluded box **from its
own edges**. An earlier version substituted the wallpaper snapshot and that was
itself a halo. And it is gated on `current_buffer == pass->buffer` — *"Only for
the live path — a cache-backed blur never samples the screen in the first
place."*

So the field is, in every existing use, **a repair for sampling a
previous-frame final composite**. Nothing uses it to remove legitimate
background. The audited interpretation holds and generalising from the name
would have been wrong.

### Decision, per the brief: (b), qualified

```text
AVK LIVE prefix path:

sample_exclude is compatibility metadata whose intended self-exclusion
invariant is already guaranteed by scene ordering. No additional
source-region removal is performed.
```

It is carried in the snapshot, not discarded. A debug assertion checks that a
producer's exclude box still corresponds to the owning surface's footprint, so
that a **future** producer using the field for something else is detected rather
than silently treated as already-satisfied.

---

## Source validity — the invariant my first architecture did not satisfy

I proposed sampling the output target directly in the first downsample. **That
is not history-free under partial damage**, and the correction is right.

At scene index `k` the target contains the current-frame prefix **only over the
damaged region**. Everywhere else it still holds the *previous frame's final
composite* — window, foreground, last frame's blur. A blur whose dependency
region extends past the damage would sample exactly that.

### The four regions, named

```text
OUTPUT DIRTY REGION    blur pixels that must change this frame
DEPENDENCY REGION      source pixels required to compute them
                       = dirty region dilated by avk_blur_support_of()
PREFIX-VALID REGION    where the source really holds current-frame [0, k)
RESOURCE EXTENT        physical backing dimensions
```

Hard invariant, checked before the first downsample:

```text
DEPENDENCY REGION  ⊆  PREFIX-VALID REGION
```

### Why Option A cannot win, derived rather than measured

**Option A — direct target source.** To satisfy the invariant, the prefix must
be rebuilt into the target over `dependency ∖ damage`. But those pixels held
*correct final-composite* content, and rebuilding the prefix destroys it — so
the suffix `(k, n)` must then be replayed over that area too.

**Option B — regional prefix transient.** Render `[0, k)` into a transient
covering the dependency region; blur from it; composite the result over the
blur's write region. The target outside the damage is never touched.

```text
A  =  prefix replay over (dependency \ damage)
    + suffix replay over (dependency \ damage)
    + the transient copy it was supposed to save

B  =  prefix replay over dependency
```

A's work is a strict superset of B's. It cannot be cheaper, and it is the one
that amplifies damage — the opposite of what eliminating a copy was meant to
buy. **Option B is chosen**, on that derivation; the measurement in M4F.2D will
report what B actually costs rather than being used to choose it.

Option B is also history-free *by construction* rather than by an argument about
what the target happens to contain: the transient is written fresh, this frame,
over the whole dependency region, before anything reads it.

### Multiple blur nodes

Scene order stays authoritative. For

```text
scene A · blur 1 · scene B · blur 2 · scene C
```

blur 2's prefix is `A · blur1's composited result · B`. Since each blur's prefix
transient replays commands `[0, k)` **including the earlier blur's composite**,
an earlier blur legitimately contributes to a later one when the scene says so.
Nothing globally defines "blur source = the background before all blur".

## M4F.2A.1 — segmented composition

One statement, proved on pixels:

> `avk_render_frame` can replay any exact scene prefix into an arbitrary
> regional target without changing the semantics of the commands it renders.

### The audit, before the refactor

Every assumption the draw loop made:

| assumption | where | now |
|---|---|---|
| target is the output image | `az_record_compose` | `seg->target` |
| target origin is 0,0 | scissors, `command_region` | `seg->origin_x/y` |
| target extent is the output's | `pc.params.zw`, viewport | `seg->width/height` |
| command range is the whole scene | the draw loop | `seg->begin/end` |
| damage is the frame's | `command_region` | `seg->active` |
| `loadOp` is always LOAD | attachment info | `seg->load` |
| the clear covers the output | clear command | `seg->clear`, `dst = bounds` |

And every `gl_FragCoord` user, classified:

| user | wants | why |
|---|---|---|
| `rounded.glsl` | **global** | measures against `round_box`, which is global |
| `shadow.frag` | **global** | the caster's SDF is a global box |
| `gradient.glsl` | **global** | normalises within its box — a local origin **restarts** the ramp |
| `dither.glsl` | **global** | deliberately output-raster anchored — a local origin **shifts the phase** |

All four want the same space, so there is one helper and no per-shader
arithmetic. The last two are the dangerous ones: both still produce a plausible
picture when wrong, and both differ *only* where a regional target begins.

### The coordinate contract

```text
scene/global    round_box, inner_box, corners -- ALWAYS. A command does not
                know which target it lands in.
target-local    gl_FragCoord, viewport, scissor, params.zw

target_pixel = scene_pixel - AZ_TARGET_ORIGIN
```

Exactly **two** translations: the vertex position in `quad.vert`, and the
scissor at the draw. Fragment shaders convert back with `az_frag_global()`.
`AZ_TARGET_ORIGIN` lives in `uv_dy.zw`, the only push-constant slot free in all
five pipelines.

### Results

```text
gradient (37 deg, 2 stops)                        0 of 12288 pixels differ
shadow + dither, corners 0/7/19/37                0
rounded 0/7/19/37 + radius-40/border-6 annulus    0
cropped, scaled client texture                    0
all three materials in one scene                  0

[0,2) [2,4) [1,3)      exactly the named commands, nothing else
draw order             translucent overlap resolves by scene order, both ways
active region          renders inside, leaves outside untouched
```

### The origin must be EVEN, and that is measured

```text
origin 36,52  (even)   0 of 12288 pixels differ
origin 37,53  (odd)   84 of 12288 differ, worst 5 codes
```

`rounded.glsl` antialiases with `fwidth(dist)` — a **2×2 quad derivative** whose
grid is aligned to the *attachment's* pixel grid. An odd origin shifts every
derivative quad one pixel relative to the output, so an edge's AA band is
computed over a different neighbourhood.

Nothing else is sensitive: the gradient, the dithered shadow and the cropped
texture all match **exactly** at an odd origin, because they read
`az_frag_global()` and nothing else.

`avk_render_segment_align_origin()` rounds a capture region's origin down to
even, growing the extent to compensate — at most one pixel per axis. The odd
case is kept as a test that *reports* the difference, so the constraint stays
measured rather than becoming folklore.

### Two bugs the pixel tests caught

**The clear's scissor was never translated.** A regional target at origin 37,53
cleared only its bottom-right corner, so every earlier frame's content survived
everywhere else — which showed up as `[2,4)` rendering quadrants 0 and 1 from
the *previous* range. My edit had matched the command loop and silently not the
clear's own loop. A struct-inspection test could not have seen it.

**My fixture lied about an image's layout.** It set
`tex->layout = SHADER_READ_ONLY_OPTIMAL` after an upload that really left it in
`TRANSFER_DST_OPTIMAL`; the graph then saw an entry layout that already matched,
emitted no transition, and validation reported `VUID-vkCmdDraw-None-09600` at
submit. The image's `layout` field is the cross-frame source of truth and
nothing may write it except whatever actually transitioned the image.

## M4F.2A.2 — the current-frame scene prefix, wired

### Topology

For each `AVK_CMD_BLUR` at index *k*, in **increasing scene order**:

```text
acquire transient over the aligned capture region
PASS  prefix_k      segment [0, k)  ->  prefix transient
PASS  blur_down_1..N, blur_up_N..1  ->  back into the prefix transient
```

then **one unchanged output pass** over the whole command range, in which a blur
command draws its finished result as an ordinary textured quad.

Increasing order is the whole trick. Blur 2's replay of `[0, k₂)` *contains*
blur 1's command, and by then blur 1's result exists, so it composites there
exactly as it will in the output. **No recursion**, termination is trivial
(chains are declared before the pass that reads them), and blur is never
special-cased out of prefix replay.

**No pixel comes from the output target.** The target holds this frame's prefix
only inside this frame's damage and the *previous* frame's final composite
everywhere else. The transient is written fresh, over its whole capture region,
before anything reads it.

The final upsample writes back into the prefix transient: nothing reads the
unblurred prefix after the first downsample, so a second full-size image would
be allocated only to be discarded.

### The four regions

```text
write        the node's box
dependency   write dilated by avk_blur_support_of()
capture      dependency, outward-aligned to an EVEN origin
allocation   what the pool handed out, >= capture
```

`avk_blur_regions_of()` derives them and **asserts containment**
(`write ⊆ dependency ⊆ capture`). Alignment may only *grow*: the origin moves
down to even and the extent grows by exactly as much, so the far edge stays put.
Shifting without growing would drop the right and bottom edges — a stale fringe
that appears only when the dependency happens to start odd.

### Results

| | |
|---|---|
| `A · BLUR · B` | **0** blur pixels differ from a no-foreground reference; B sharp on top (247,247,240) |
| `A · B · BLUR` | blur mean **20.42 → 64.48**, Δ **+44.06** |
| `sample_exclude` on vs off | **0** pixels differ |
| remove B | **65.30 → 21.22 immediately**; first frame already equals the settled value |
| two identical frames | **0** pixels differ |
| 30 static repeats | **0** frames differed, **0** new transients, **0** CPU waits |
| two blurs, overlapping | 2 prefix captures; blur2 region mean **21.36 → 243.43** with blur1 + foreground before it |

The reversed-order result is the one that matters: **the same object, the same
geometry, the same blur — only its scene position changed**, and that alone
decided whether it was in the source. A renderer keyed on node type, window type
or a foreground flag would have answered identically both times.

Every expected value comes from an **independently constructed** scene
containing only what should have contributed, blurred by the same primitive. The
prefix path is never its own oracle.

### `BREAK=blur-scene-after`

`AZ_BLUR_SCENE_AFTER=1` replays `[0, scene->len)` instead of `[0, k)` — the
whole scene, including everything after the blur node. **Fails 8 of 24**, with
precisely the reference's failure class:

```text
scene order        10306 wrong pixels, worst 109 codes
sample_exclude     10400 pixels -- no longer neutral, because now the owner
                   IS in the source and a repair path WOULD be needed
frame history      101.44 -> 96.63 -> 91.60, still drifting
two identical frames                19006 pixels differ
30 static repeats  30 of 30 differed
its own case       mean +40.89, 10119 wrong pixels, worst 101 codes
```

The drift and the 30/30 are the important ones: they are **feedback
accumulation**, the halo that grows rather than appearing at once — because the
blur's own command is now inside its own source. There is no `sample_exclude`
repair to rescue it, deliberately: the break exists to show why prefix capture
is better, and a repair path would hide that.

### Direct path, unchanged

```text
build    cpu50  cpu95  cpu99   rec_us    gb50    gb95    gb99    gpu_us  tr  pass  barr  creates
before     120    400   1520     86.0    3500   12000   16000    1488.8   0     1     2        0
after      120    460   1060     72.5    4250    9500   11250    1492.7   0     1     2        0
```

With no blur node: **0 transient acquires, 0 creates, 1 pass, 2 barrier calls.**

### Prefix replay cost, recorded not optimised

```text
two-blur frame: 2 replays, 81 commands replayed, 2 357 946 pixels
```

Quadratic in blur count by construction — which is exactly the number a cache
would be bought with, so it is measured now and left alone.

### The bug validation caught

`avk_render_declare_segment()` derived its sampled-image uses from the command
range, but listed **only texture commands**. A blur command samples its own
finished result, so a segment containing one depends on that image exactly as it
does on a client surface — and the chain leaves it in
`COLOR_ATTACHMENT_OPTIMAL`. Nothing transitioned it, and validation reported
`VUID-vkCmdDraw-imageLayout-00344` at the draw.

That is precisely what "derive the uses from the range itself" exists to prevent,
and the first version still got it wrong — because the derivation was a list of
command types, and a list is a thing you can forget to add to.

---

## M4F.2A.3 — the blur node's material, re-read from source

Before any of this was implemented the four remaining fields were traced to the
lines that consume them, rather than to their names or to the earlier audit.
**Where the source disagreed with the M4F.2A.0 audit, the source won**, and the
one place it did is recorded below.

### `darken` — `min(blurred, unblurred)`, per channel

`render/fx_renderer/vulkan/shaders/blur2.comp:158`, the LAST upsample only:

```glsl
if (data.clamp_darken > 0.5 && !(
        all(greaterThanEqual(ivec2(px), data.skip_min)) &&
        all(lessThan(ivec2(px), data.skip_max)))) {
    vec4 src = imageLoad(out_img, ivec2(px));
    color.rgb = min(color.rgb, src.rgb);
}
imageStore(out_img, ivec2(px), color);
```

Four properties, all load-bearing:

| | |
|---|---|
| **rgb only** | alpha comes from the blur untouched |
| **after effects** | `apply_blur_effects()` runs at `:132`, the clamp at `:158` |
| **last pass only** | `pass.c:1824` — `if (i == 0 && darken_except != NULL)` |
| **free** | the chain walks away from mip 0 and only returns on that pass, so mip 0 still holds the unblurred source at the moment it is read |

Gated twice at the producer: `pass.c:2943` passes `darken_except` only when
`options->darken_only`, and `wlr_scene.c:2793` sets `darken_only = blur->darken`
**inside `if (blur->has_sample_exclude)`**. The comment on
`wlr_scene.h:237` says why: `has_sample_exclude` is what identifies a shadow's
backdrop, and the clamp is skipped inside the excluded box itself because there
the "source" is the substitute fill rather than real backdrop.

**AVK diverges on the skip box, and only on the skip box.** There is no
substitute fill in AVK — the prefix transient holds real current-frame backdrop
at every pixel of the capture — so the premise that made a region unclampable
does not exist and the clamp applies over the whole write region.
`has_sample_exclude` still gates whether darken is *consulted*, because that is
what identifies the producer; see "`sample_exclude` — the producer table".

**How AVK evaluates it.** A fragment shader cannot read the attachment it
writes, and the reference's `imageLoad` is a compute-path affordance. AVK gets
the identical arithmetic from fixed-function blending instead: the final
upsample runs a pipeline with `VK_BLEND_OP_MIN` on the colour channels and
`VK_BLEND_OP_ADD (ONE, ZERO)` on alpha, into an attachment loaded with
`LOAD_OP_LOAD`. Blending is applied after the fragment shader, so effects are
folded in first, exactly as at `blur2.comp:132`. Min/max blend ops ignore their
blend factors, so there is one behaviour and no factor to get wrong. Cost is
the same as the reference's: one attachment read, no extra memory, no extra
pass.

### `edge_softness` — the shadow's falloff, on the blur's own alpha

`render/fx_renderer/vulkan/shaders/texture_soft_edge.frag`, and it is
`box_shadow.frag`'s coverage verbatim:

```glsl
float coverage = roundedBoxShadow(
    data.position + data.blur_sigma,
    data.position + data.size - data.blur_sigma,
    frag_layout, data.blur_sigma * 0.5,
    data.radius.x, data.radius.y, data.radius.z, data.radius.w);
out_color = color * data.alpha * coverage * clip_corner_alpha;
```

Same caster inset (`+sigma`), same half-sigma Gaussian, same 4-sample
integration. `edge_softness` **is** a `blur_sigma`; the fields are two names for
one quantity, and asteroidz's producer says so directly —
`src/animation/client.h:757` passes `shadow_blur_sigma` as the blur node's
`blur_edge_sigma`, and `layer.h:331` uses `config.shadows_blur` for both.

`corners` changes meaning when `edge_softness > 0`: it stops being a hard SDF
radius and becomes the soft box's own corner radii. It is the same number from
the producer either way.

#### CORRECTION TO THE M4F.2A.0 AUDIT

The audit recorded edge softness as sharing the shadow's *unscaled* sigma. It
does not. `types/scene/wlr_scene.c:2778`:

```c
.edge_softness = blur->edge_softness * data->scale,
```

The reference **scales edge softness to output pixels** and does **not** scale a
shadow's `blur_sigma` (`avk-effects.md`, "REFERENCE BUG FIXED IN AVK"). So the
reference is internally inconsistent: on DP-1 at 1.5 the blur's edge fades over
1.5× the distance the shadow's tint does, from one producer value.

AVK scales both, at the walker, beside the box and the radii. That is not a new
policy — it is M4D's policy applied to the second field that needed it, and it
makes the two edges agree. It also means AVK and the reference agree on
`edge_softness` itself and differ only on the shadow it is meant to match.

### `clip_region` — a KEEP region, not a cutout

`types/scene/wlr_scene.c:2710`:

```c
if (blur->has_clip_region) {
    copy, translate by (entry - logical), logical_to_buffer_coords
    pixman_region32_intersect(&render_region, &render_region, &blur_clip_region);
} else if (!wlr_box_empty(&blur->clipped_region.area)) {
    rect from clipped_region.area, same translation
    pixman_region32_intersect(&render_region, &render_region, &blur_clip_region);
}
```

**Both are INTERSECTIONS, and for a blur node only.** A rect's or a shadow's
`clipped_region` is SUBTRACTED — that is the window-shaped hole M4A/M4D
implement as `inner_box` + `inner_corners`. A blur's is the opposite: it is the
region the blur is *allowed* to appear in. `blur_options.tex_options` never sets
`.clipped_region` at all, so the cutout the soft-edge shader supports is
inert on this path and AVK carries no `inner` for a blur command.

Getting this backwards is not subtle in a screenshot but is entirely plausible
in code, because the field name and the C type are shared with the cutout.

Both are **node-local**, both take the same road, and `clip_region` takes
precedence when present. Being a pixman region, `clip_region` may hold more than
one rectangle — `ext-background-effect-v1` gives the client an arbitrary region
— and the reference preserves its shape exactly, because intersecting regions
is all it does. AVK converts once, in the walker, into the command's own
`clip`, which the draw loop already walks rectangle by rectangle as scissors. No
bounding box is taken anywhere, and no shader learns about it.

### `alpha` and `corners`

`.alpha = &blur->alpha` on the base texture options, and
`.corners = fx_corner_radii_scale(blur_corners, data->scale)` after
`fx_corner_radii_transform`. Both are the ordinary texture path's fields with
ordinary meanings; AVK's `opacity` and `corners` already carry them.

### The composite, assembled

```text
out = blurred_sample
        * alpha
        * coverage          rounded SDF        (edge_softness == 0)
                            gaussian falloff   (edge_softness  > 0)
      , scissored to  damage  n  clip_region-or-clipped_region
```

with `darken` having already been folded into `blurred_sample` by the chain that
produced it. Kernel and material stay apart: the blur passes know nothing about
corners, alpha or clips, and the composite knows nothing about levels or taps.

### What was measured

`tests/test-avk-blur-material.c`, 30/30. The oracles are separate measurements,
never the production shader:

```text
darken
  premise    the unclamped blur is BRIGHTER than its source on 12975 of 23040
             pixels, worst +67 codes -- the glow the clamp exists to remove
  assertion  clamped == min(unclamped, source) on every channel of every pixel,
             0 samples outside tolerance, worst 0 codes
  effect     mean 64.76 -> 29.71
  blindness  on a FLAT backdrop the same test measures 0.00 difference, so a
             flat fixture would pass with the feature removed

edge_softness
  hard 0.80 px, sigma 12 -> 15.58 px, sigma 24 -> 31.00 px (10-90% transition)
  largest single-pixel coverage step inside the fade 0.0333 -- no seam
  sigma 16 -> 20.67 px, sigma 24 -> 31.00 px, ratio 1.500
             1.5x, not 2.25x: the scale is applied exactly once
  blur soft edge vs a real SHADOW at the same sigma: mean 0.0012, worst 0.0050
             coverage -- the same falloff, which is the whole point of the field

clip_region
  whole node / offset+smaller / odd origin+odd extent: in each case 0 pixels
  outside the clip differ from the same scene with no blur node. Bit-identical.
  two rectangles with a 24 px gap: 8000 px changed inside, 0 outside, 0 IN THE
  GAP -- no bounding box is taken anywhere

alpha
  0 leaves the backdrop exactly as it was; 0.5 lands on 140 where the exact
  midpoint of 80 and 200 is 140.0 -- no dark fringe, no double premultiply

corners
  radii 0 / 7 / 19 / 37 reach 0 / 2 / 5 / 10 px along their own diagonals,
  against a predicted 0.293r of 0 / 2 / 5 / 10. Worst error 0 px.

combined (darken + edge_softness + clip + asymmetric corners, dark fixture)
  0 pixels outside the clip, 3391 inside, worst brightening +0 codes
```

`contrib/avk-blur-walker-test.sh`, 23/23, on a real compositor with the real
shadow-backdrop producer. The command stream, dumped from one frame:

```text
cmd[0]  RECT     0,0 1280x800        the root background
cmd[1]  TEXTURE  0,0 1280x800        the wallpaper        <- A, IN the source
cmd[2]  BLUR     1,34 1274x764       k = 2
cmd[3]  SHADOW   1,34 1274x764
cmd[4]  SHADOW   6,36 1264x754
cmd[5]  RECT     10,40 1260x750      the border
cmd[6]  RECT     1268,52 2x726
cmd[7]  TEXTURE  12,42 1256x746      the window       <- B, NOT in the source
cmd[8]  SHADOW   36,18 288x38        the titlebar
cmd[9]  TEXTURE  40,12 280x30
cmd[10] SHADOW   6,18 39x38
cmd[11] TEXTURE  10,12 31x30
```

That is `A · BLUR · B` and `A · B · BLUR` in one stream: the same window is on
both sides of a blur -- the one below it and the one above it -- and they land
on opposite sides, from the real walker rather than from a hand-built list.

The producer's own material arrives with it: 3 darken chains and 6 soft-edge
composites in the sampled interval, which is the three-link chain (producer
sets, walker snapshots, renderer honours) that no renderer test can see.

### Two premises that were false, and were measured rather than assumed

**`only-floating` defaults to 1**, so a tiled window has no shadow, hence no
backdrop blur, hence no blur node. The first run of the walker test measured
`blur_nodes_seen = 0` and that reads exactly like a walker that emits nothing.
The fixture sets it to 0 and the premise is now asserted.

**The graph test's "direct path" had a blur in it.** The harness's default config
turns the shadow backdrop blur on, and the wallpaper is a layer surface with a
layer shadow -- so from the moment the walker honoured blur nodes,
`contrib/avk-graph-test.sh` measured **6 passes and 7 barriers** while calling it
the direct path. M4E's hard requirement was still being met; the fixture had
simply stopped testing it. The fixture now disables blur explicitly and asserts
`blur_nodes_emitted == 0` beside the one-pass check.

### An instrument that reported the intention

`blur_prefix_commands` added `i`, the blur's command index -- which is the prefix
length the design calls for, and is a DIFFERENT number under
`AZ_BLUR_SCENE_AFTER`, where the range is `[0, len)`. So the break widened the
replay and the counter went on reporting the narrow figure, and the assertion
"the source range is a prefix" passed with the break on: 2 of 12 either way. It
reads `seg->end - seg->begin` now, and the break reports 12 of 12 and fails.

This is the second time in M4F an instrument has been wrong in a way that made a
test agree with it (the first was `graph_build_ns`; see the invalidated
measurement in `docs/regression-testing.md`).

### A limitation, stated rather than discovered later

**asteroidz throws away the shape of a *toplevel's* blur region before the walker
ever sees it.** `client_update_blur()` (`src/asteroidz.c`) takes
`pixman_region32_extents()` of the client's `ext-background-effect-v1` region and
hands the bounding box to `wlr_scene_blur_set_clipped_region()`.

Measured, not assumed: `contrib/wlbgeffect` supplies two separated rectangles and
the emitted blur command's clip arrives with **one**.

This is a producer decision that predates M4F and is left alone *here*. The
protocol gives the client an arbitrary region and the renderer can carry one;
closing the gap is a change to `client_update_blur()`, and belongs with that
decision rather than with the renderer that was made ready for it.

**M4F.2B.0 made that decision and this paragraph is now history.** Two claims in
it were also too broad, and are corrected in the section below: the setter is
`wlr_scene_blur_set_region()`, not `wlr_scene_blur_set_clip_region()`, and the
multi-rectangle path was never unreachable *from this compositor* — only from a
toplevel. Layer surfaces and popups have always passed their regions verbatim.

## M4F.2B.0 — the damage pipeline, and a producer that disagreed with itself

### The producer audit, and why the collapse was accidental

The question the milestone put first: is `client_update_blur()`'s
`pixman_region32_extents()` an intentional semantic policy, a SceneFX
workaround, or accidental information loss?

**Accidental.** Four pieces of source evidence, none of them an inference from
naming:

**One — there are three producers of this protocol's data, and two preserve it.**
`ext-background-effect-v1` regions arrive for toplevels, layer surfaces and
popups. `layer_update_blur()` (`src/asteroidz.c:4084`) and
`popup_update_blur()` (`:4156`) both call `wlr_scene_blur_set_region()` with the
region itself. Only the toplevel path collapsed it. One protocol, one meaning,
three consumers, and the odd one out is the one with no stated reason.

**Two — the layer path states the harm, and it applies identically to a
toplevel.** Its comment: *"pass the client's region verbatim: it carries the
rounded corners, so clipping by its bounding box would leave square blur 'ears'
poking out at the card corners"*. A toplevel drawing a rounded card has the same
corners as a layer surface drawing one.

**Three — the blur node's own documentation names this exact producer as
`clip_region`'s input.** `wlr_scene_blur.clip_region` is documented as
*"Pixel-accurate clip in node-local coords (e.g. the client's
ext-background-effect region). When set it takes precedence over
clipped_region's bounding box."* The field written for this data says so in its
own comment, and the toplevel path wrote the other field.

**Four — the collapse was forced by the API that was called, not chosen.**
`struct clipped_region` is `{ struct wlr_box area; struct fx_corner_radii
corners; }` — one rectangle. It *cannot* hold a region. Taking the extents was
not a decision about blur semantics; it was the only way to call that function.
The toplevel passed `corner_radii_none()`, so the field's one added capability
was unused too.

The history says only that both behaviours arrived together in `44f99ae`
(asteroidz's initial import), so the inconsistency is inherited rather than
introduced — which is why nothing in the repository ever argued for it.

**The fix, and why it is safe.** `client_update_blur()` now calls
`wlr_scene_blur_set_region()` with the region verbatim, and resets
`clipped_region` to its default in both branches — `clip_region` is the
higher-precedence field, so a stale box left in the lower one would silently
decide the clip on every later frame in which the client withdrew its region.
Nothing else consumes a blur node's `clipped_region` in asteroidz, and
`scene_node_get_size()` for a blur returns `blur->width/height` regardless of
either clip field, so the node's size, visibility and damage bookkeeping are
untouched by the swap.

**Proven end to end**, and in two halves that meet at the command:

    producer -> walker -> snapshot -> command
        contrib/avk-blur-walker-test.sh, on the real dump:
        cmd[16] BLUR dst=645,12 623x776 clip=2rects
        asserted == 2, not >= 1: a bounding box is one rectangle and would
        have satisfied >= 1 forever, which is how the collapse survived M4F.2A.3

    command -> composite
        tests/test-avk-blur-material.c test_clip_multi_rect, over the same
        shape: two rectangles, 24 px gap
        inside  A and B: changed
        in the gap:      0 pixels changed

`wlbgeffect`'s gap is now 24 px rather than 60, to match. A wide gap survives a
sloppy region operation that a narrow one does not, and the whole point of the
fixture is that the hole is never filled in.

### Where AVK's damage comes from, traced

| stage | source | space | when |
| --- | --- | --- | --- |
| surface commit | `wlr_surface.buffer_damage` | buffer px | client commit |
| scene node damage | SceneFX `scene_node_update()` → `scene_damage_outputs()` | layout | property/geometry change |
| ring accumulation | `wlr_damage_ring` on `wlr_scene_output` | output px | continuously |
| this frame's damage | `wlr_damage_ring_rotate_buffer(ring, buffer, &damage)` — `az_avk.h:2600` | output px | once per frame, per target buffer |
| the scene's damage | `pixman_region32_copy(&scene.damage, &damage)` — `:2643` | output px | immediately after |
| the output segment | `ctx.active = &scene->damage` — `avk_render.c:1285` | scene px | frame declaration |
| per command | `command_region()` — `:361` intersects bounds ∩ dst ∩ clip ∩ damage | scene px | per command |
| scissor | one translation into attachment coords, at the draw | attachment px | recording |

Two properties of this table matter for M4F.2B. It is keyed on the **buffer**,
not the frame, so a target last used three frames ago comes back with three
frames of damage and one the ring has never seen comes back whole — which is
exactly right and is why `AZ_AVK_FULL_DAMAGE` can be a pure superset rather than
a different code path. And the **only** expansion anywhere in it is none: damage
is intersected at every stage and never dilated.

### The gap M4F.2B exists to close

`wlr_scene_output_build_state()` — which AVK does not call — contains SceneFX's
entire blur damage compensation: `should_blur_node_extend_damage()` and
`apply_blur_region()` (`wlr_scene.c:3590`, `:3623`) expand the frame's damage by
`blur_data_calc_size()` and union the intersection with each blur node's visible
region back into both the render damage and the output state, then keep a
`blur_padding_region` that is copied out and pasted back to hide the seam.

**AVK inherits none of it.** M4F.2A.3 wired real blur nodes into a frame whose
damage is still the untouched ring output, so today a source pixel changing
behind a blur damages its own box and the blur composites only inside that box:
the blurred result outside it is mathematically stale. Nothing has shipped —
that is what "installed remains `0.24.0(6dfcf66)`" has been protecting — and it
is precisely M4F.2B's subject.

The reference's expansion is `pow(2, num_passes + 1) * radius`, one number for
all four edges. AVK already derives the real chain support per edge
(`avk_blur_support_of()`, `M4F support — derived, not fitted` above), and the
padding-copy hack has no counterpart here at all: a prefix capture is written
fresh every frame, so there is no seam to paste over.

## M4F.2B.1/.2 — damage forward, demand backward

### The model, in six regions and two sweeps

`struct avk_blur_damage` keeps six regions apart per blur, because collapsing
any two of them still renders a plausible picture that is wrong only when
something moves:

| region | is | derived from |
| --- | --- | --- |
| `write` | where the result is composited | node box ∩ its own clip region |
| `dependency` | source that can reach it | `write` dilated by the REVERSE support |
| `source_damage` | source that actually moved | prefix damage ∩ `dependency` |
| `output_damage` | result that may differ | `source_damage` dilated FORWARD ∩ `write` |
| `result_region` | result somebody needs | demand ∩ `write` |
| `prefix_rebuild` | source to reconstruct | `result_region` dilated REVERSE ∩ `capture` |

**`prefix_rebuild` is not `source_damage`, and is bigger.** The filter needs a
neighbourhood around every pixel it recomputes. Rendering only the
source-damaged pixels into the transient is the specific mistake the region set
exists to name, and the test asserts the inequality rather than trusting it.

**Two sweeps, opposite directions, one pass each.**

```text
SWEEP 1, increasing scene order — DAMAGE
    prefix_damage = the frame's damage
    for each blur k:
        source_damage(k) = prefix_damage ∩ dependency(k)
        output_damage(k) = dilate_forward(source_damage(k)) ∩ write(k)
        prefix_damage   ∪= output_damage(k)     ← later blurs see it
        frame_damage    ∪= output_damage(k)     ← the output must show it

SWEEP 2, decreasing scene order — DEMAND
    demand = frame_damage
    for each blur k:
        result_region(k)  = demand ∩ write(k)
        prefix_rebuild(k) = dilate_reverse(result_region(k)) ∩ capture(k)
        demand           ∪= prefix_rebuild(k)   ← earlier commands owe it
```

A blur's output joins the prefix damage only *after* its own source damage has
been read: that single line of ordering is why a blur can never feed itself, and
it is the same property `BREAK=blur-scene-after` exists to falsify.

**One forward pass is sufficient, and it is an architectural property rather
than luck.** A blur at index *k* samples exactly `[0, k)`, so every dependency
edge runs from a lower scene index to a higher one; the dependency graph is a
DAG whose topological order *is* the scene order. An iterative fixed-point
solver would be a loop that always ran exactly once. The demand sweep is the
same argument reversed — asking for a blur's result asks earlier commands for a
region larger by the support, and that request only ever travels backwards.

**Why a demand sweep is needed at all.** Skipping a blur whose own source did not
change is wrong exactly when a *later* blur is rebuilding prefix pixels that lie
inside it. The demand sweep turns that into a region rather than a special case,
and makes "this blur does nothing this frame" a safe, counted decision.

### Forward support == reverse support, derived

`avk_blur_support_of()` answers *what source can affect this output pixel*;
damage asks the inverse. They are the same number and that is a derivation, not
an assumption: the level mapping is affine (`centre(p) = (p + 0.5) · span`) and
the kernel is symmetric, so dilating an interval through one step costs the same
constant in source pixels whichever way it is traversed. Summing the chain gives
the identical total.

What is *not* shared is the rounding — one maps output→source, the other
source→output — so `avk_blur_forward_support_of()` exists as its own entry point
and both round outward. `test_forward_support_walk` composes the steps level by
level instead of summing them, over 24 (extent, radius) pairs including
63/64/65/127/128/129, and the two routes agree to **0.000003 px** — which is
float storage, not arithmetic.

### The prefix transient keeps its EXTENT and loses only its REGION

The obvious saving — allocate a transient the size of the damage — is wrong, and
the reason is worth stating because it looks like an optimisation:

> A dual-Kawase level grid is derived from the image's extent. `level_extent()`
> floors, so a *smaller capture* has different level extents, different texel
> spans and different sample positions, and produces a **different** blur. Not a
> wrong one — a different one.

Pixel-identity with a forced-full render is this milestone's oracle, so the grid
is held fixed and only the segment's active region shrinks. Everything outside
`prefix_rebuild` is left `DONT_CARE` and is never sampled, because
`prefix_rebuild` is `result_region` dilated by the whole chain support: every tap
taken for a pixel of `result_region` lands inside it. **Under
`AZ_TRANSIENT_POISON=1` the untouched part of a reused transient is a colour
nothing produces, and the whole suite passes 39/39** — which is the hard
invariant ("every sampled source pixel is freshly reconstructed this frame")
tested rather than argued.

### What it saved, and the number that governs it

The saving is not a property of the damage code. It is the ratio of the
**support** to the **node**:

```text
rebuild  ~  (change + 4·support)²  /  (node + 2·support)²
```

| fixture | support | rebuild / capture |
| --- | --- | --- |
| 24×24 change, 160×144 node, radius 2, 3 levels | 57 px | **97 %** |
| 24×24 change, 160×144 node, radius 1, 1 level | 7 px | **9 %** |

At radius 2 and 3 levels the support is 57 *source pixels*, which on a small
node is most of it — so reporting the second row alone would be misleading and
both are measured. On a real desktop the blurred window is much larger than the
kernel's reach, which is the second row's regime.

At the compositor: an idle three-window desktop emits 18 blur nodes and now
**recomputes 9 and skips 9** — no capture, no chain, no composite for half of
them.

### Three things that were measured rather than assumed

**A fixture with no outside left.** The influence test placed its "well outside
the support" change at x=0 — because the default node's dependency reaches 57 px
left of x=48, off the target, and is *clamped* to 0. The change was inside the
clamped dependency and the test measured 202 changed pixels inside the blur: a
correct measurement of the wrong question. It now uses a support of 7, where the
target has room for an outside.

**A tolerance that measured the storage format.** The forward-support walk
asserted agreement to 1e-6 and failed at 0.000003 px. `avk_blur_support_of()`
returns floats; the walk is in double.

**`blur_prefix_pixels` would have reported M4F.2A's figure forever.** It counted
the capture's full extent, which stopped being what the replay writes the moment
the segment acquired an active region. It reads `az_region_area(seg->active)`
now — the same lesson as `blur_prefix_commands` in M4F.2A.3, and the third time
in M4F an instrument has been able to describe work the renderer did not do.

### There is no `BREAK=blur-no-transitive-damage`, and that is a measurement

One was written. It stopped a blur's output damage joining the prefix damage a
later blur reads, and it **could not be made to leave more than 4 wrong pixels at
1 code** in any geometry tried — disjoint blurs, overlapping blurs, a 24×24
block, a 114×190 wall, supports from 7 to 94.

The reason is not the implementation:

- **A dual-Kawase blur preserves the local mean**, so blurring an
  already-blurred field gives very nearly what blurring the raw field would.
  Removing blur 1 from the two-blur scene *entirely* — replacing 120 of the 136
  columns of blur 2's source — moved blur 2's output by **one 8-bit code**.
  Giving blur 1 a brightness of 1.6 (which is what the real compositor does, and
  what breaks the mean-preserving symmetry) raised that to 16 codes.
- The region a missing transitive edge strands is at **kernel-tail distance in
  both blurs**, so the error is the product of two decays.

Per the standing rule that a break switch which does not fail strongly is not a
break switch, it was deleted. The edge is still mathematically required, and is
asserted **structurally** on `blur_transitive_damage_pixels` — the source-damage
pixels one blur inherited from another — which reads **64 600** in the two-blur
fixture and **0** for a lone blur. An omission moves it to zero and the test
fails.

`BREAK=blur-under-damage` is the opposite case and is kept: it removes the
forward dilation and leaves **5 261 wrong pixels, worst 71 codes**, failing 18 of
the 39 checks.

## M4F.2C.0/.1/.2 — the seam is not a blur boundary

### The output-boundary audit

Where AVK clipped scene state to an output, before this milestone:

| stage | space | clipped to the output? |
| --- | --- | --- |
| walker traversal | layout | no — it walks the whole global scene tree |
| `az_avk_box_to_output` | logical → device | no, it only converts |
| RECT emission | device | no (degenerate check only) |
| **BUFFER emission** (`az_avk.h`) | device | **YES** — the earliest loss point |
| SHADOW emission | device | no (degenerate check only) |
| **BLUR emission** | device | **YES** |
| cursor emission | device | yes (correctly — a cursor is presentation only) |
| `avk_blur_regions_of` clamp | device | **YES**, to `{0,0,width,height}` |
| `command_region()` | device | to the segment's own bounds |
| `scene_output_damage` (fork) | device | **YES**, to `0,0,w,h` |
| scissor | attachment | the draw's own region |

Three of those had to change: a texture outside output A can still be *source*
for a blur presented on A, a blur node outside A can still be part of the prefix
another blur on A replays, and damage outside A can still change A's pixels.

### Presentation bounds and source bounds

```text
present_bounds   pixels this output can put on a screen
source_bounds    scene it can RECONSTRUCT = present_bounds dilated by the halo
```

`struct avk_scene.source_bounds` carries the second; the renderer clamps a
blur's dependency to it and clips the *presentation* damage back to the first.
On a single output they are the same box and nothing changes at all.

**The halo is an extent-independent bound.** Retention has to be decided before
the walk finds the blur node that will want the pixels, so
`avk_blur_support_bound()` drops the only extent-dependent term — the texel span
— for its exact upper bound `span(base, i) ≤ 2^i + (2^i − 1)/2`, which follows
from a level never running below two texels. About 1.5× the support a large
extent really produces, and used **only** to widen retention: every capture and
every damage region still uses the exact per-node support.

**Each output rasterises its own halo.** The same global scene region is
reconstructed twice at two pixel densities when two outputs both need it, and
that is correct. Output A never samples output B's framebuffer — that would
couple format, scale, presentation history and, in M5, the colour domain. This
is the property that makes the architecture survive an SDR display beside an HDR
one, and it is why the source is re-rendered rather than copied.

### The kernel is scaled, and it was not before

`radius` multiplies a **half-texel** step, and the texels are the capture's,
which are output pixels. Left unscaled, a blur reaches the same number of
*device* pixels on every output and therefore covers 1/1.5 as much of the
picture on a 1.5× display: the same window dragged across a seam visibly
sharpens. The reference does not scale it either (`blur_data_calc_size()` is
`2^(passes+1) * radius`, applied to damage in buffer coordinates,
`fx_pass.c:1481`), so this is a deliberate divergence and not a regression fix.

It is scaled exactly once, at the walker, beside the box, the corner radii and
`edge_softness` — the policy M4D adopted for a shadow's sigma. `levels` is *not*
scaled: it counts halvings, not lengths, and there is no such thing as 1.5 of a
level; scaling the radius moves the reach continuously, which is what a
fractional scale needs.

### Coordinate model, pinned

| quantity | space | converted where |
| --- | --- | --- |
| blur node box | logical → **device** | `az_avk_box_to_output` |
| `clip_region` | node-local logical → **device** | `az_avk_blur_keep_region` |
| corners | logical → **device** | `az_avk_corners_from_scenefx` |
| `radius` | logical → **device** | the walker, ×scale (M4F.2C) |
| `levels` | **a count** | never scaled |
| `edge_softness` | logical → **device** | the walker, ×scale (M4F.2A.3) |
| support / dependency | **device** | derived from device geometry |
| capture extent, even origin | **device** | after scaling, never before |
| damage | **device** | the fork scales it into buffer coords |
| `source_bounds` | **device** | output pixels, normally negative-origin |

Even-origin alignment happens in device space, after rounding, and
`avk_render_segment_align_origin` decrements — which is correct for a negative
origin under two's complement, where `-41 & 1` is 1 and the origin becomes −42.

### Cross-output damage routing

`scene_output_damage()` intersected damage with `0,0,w,h` and dropped the rest,
so a change wholly on B never reached A and A's near-seam blur went stale. The
fork now also records the part that falls inside A's halo.

**Measured, and the oracle found it:** with the routing missing, a change on B
left output A stale over `x 721..799` — the last 79 columns before the seam,
inside a halo of 126 — and `damage_all` reported 10 860 differing pixels.

**It goes into the MAIN ring.** A separate `wlr_damage_ring` was tried first so
that nothing but AVK could see out-of-bounds damage. It recorded 2 106 regions
and every rotate returned **empty**: a damage ring's per-buffer accounting does
not behave like a private accumulator when only one caller ever rotates it. The
main ring is the one known to work and gives per-buffer delivery for free — a
change seen while drawing buffer 1 still reaches buffer 2. `pending_commit_damage`
stays in bounds, the frame is scheduled explicitly, and
`wlr_scene_output_build_state` now clips what it rotates out so a fallback frame
can never be handed a scissor outside its framebuffer.

### Results

```text
seam step, correct source     0 codes
seam step, BREAK=source-clip  14 codes      ← threshold 4, set by both numbers
halo                          126 px at passes 3 / radius 4
commands retained for it      5
halo source reconstructed     832 608 px    ← 0 under the break
cross-output damage frames    7
damage_all on A and B         0 stale pixels, before and after a cross-seam change
direct path                   halo 0, retained 0, halo-frames 0, replays 0
```

`BREAK=blur-source-output-clip` fails **two** assertions, one of which needs no
threshold at all: the halo source area it reconstructs is exactly 0.

### `blur { enable 0 }` does not disable the kernel

It stops asteroidz *creating* blur nodes (`client_update_blur`,
`layer_update_blur`, `popup_update_blur` all consult `config.blur`) and leaves
the scene's `passes`/`radius` alone, so `is_scene_blur_enabled()` stays true.
Gating the halo on the kernel alone measured a 71 px halo and 5 retained
commands on a desktop that could not contain one blur node. The halo is gated on
`config.blur` *and* the kernel — together they are exactly "can a blur node exist
here".

### `move` is not a dispatch

`hl_dispatch "move 470,180"` changed nothing in any counter, and the reason was
not the fixture: there is no dispatch called `move`, the IPC layer swallows an
unknown name silently, and the line did nothing. It is `move_window,<x>,<y>`.
Second time an invented dispatch name has produced a confident wrong reading
here; the first was `focusdir` for `focus_direction`.

`HL_X2` was also needed: the harness placed the second output at layout
`x = HL_WIDTH`, which is only adjacent to the first while that output is at
scale 1. At 1.5 its logical width is `HL_WIDTH/1.5` and the default leaves a gap
— a "seam test" with no seam in it.

## M4F.2C.3 — scale, transforms, and two findings left open

### The reach really is proportional to the radius

Scaling `radius` is only the right lever for a fractional output scale if the
kernel's reach is proportional to it — otherwise `levels` would have to move,
and `levels` can only move in factors of two. Measured at the renderer, where
scale does not exist:

| levels (radius 2) | measured reach | mathematical bound |
| --- | --- | --- |
| 1 | 5 px | 9 |
| 2 | 13 px | 25 |
| 3 | 26 px | 57 |
| 4 | 49 px | 121 |

The bound contains the measured reach at every level count — which is the
property M4F.2B's damage regions rest on — and sits at roughly twice it, which
is what a conservative footprint against an 8-bit floor should look like.

**A 1.5× radius reaches 1.385× as far** (26 → 36 px). Not exactly 1.5, because
the measurement stops at the quantisation floor and a wider kernel has a flatter
tail; proportional, which is what the lever needs to be.

### The instrument measured a kernel ten times over its bound

The first version reported a reach of **96 px against a bound of 9** — which, if
real, would have meant every damage region in M4F.2B was under-covering. It was
`test_realistic`'s moving-widget loop, which deliberately leaves its own content
in `bg_a` so each step starts from the previous frame. The "unchanged" render
therefore still had a widget in it 140 px along, and the difference between the
two frames included it.

**Two controls now run before any number about the kernel is believed:** the same
scene rendered twice must be identical (0 px), and a scene with *no blur node*
must show no reach at all (−1). The second is what caught it — it measured 91 px
of "reach" with the effect switched off.

### Multi-output results

| configuration | result |
| --- | --- |
| equal scale 1.0/1.0, seam at 800 | **19/19** |
| mixed 1.5/1.0, seam at 533 | **17/17**, halo 126, 1 024 608 halo source px |
| fractional 1.25/1.0, seam at 640 | **17/17**, halo 150 |
| transform 0 | **17/17** |
| transform 90 | 12/12 — pixels skipped, see below |
| transform 180 | 16/17 — **open**, 22 stale pixels |
| transform 270 | 11/12 — **open**, routing did not fire |
| `BREAK=poison` | **19/19** |

### grim cannot capture a rotated output on this backend

A 90° run left **no HEADLESS-1 png on disk at all** while every HEADLESS-2
capture was present. That is a screenshot limitation, not a compositor one, so
the pixel comparisons are skipped **with a stated reason** and every
counter-based assertion — cross-output routing, retention, halo source area,
validation — still runs. 180° captures fine and is the transform case that gets
the full pixel treatment.

### Two findings left OPEN, located but not fixed

**180° leaves 22 stale pixels**, at `x 771..799, y 184..313` — a 29×130 strip at
the buffer's RIGHT edge. Under a 180° transform that edge is output A's *logical
left*, which is the outer edge of the whole desktop and the side furthest from
the seam. The halo rect, the source bounds and the walker's retention are all
symmetric in ±halo on both axes, so this is not a sign error in any of them; it
is unexplained and is not claimed as working.

**270° records 2 271 halo damages and consumes 0**, and the same run reports
**894 blur nodes where every other transform reports 10** — the compositor is
rendering continuously rather than reaching idle. The node count is the larger
anomaly and may not be blur-specific at all; both are recorded rather than
guessed at.

Neither affects `normal`, which is every shipping desktop here and both live
monitors. **Transform closure is therefore NOT claimed for 180° and 270°.**

### The counter that reported the wrong monitor

`blur_halo_px` was last-writer-wins across outputs, so on a mixed-scale desktop
it reported whichever monitor happened to render most recently — the wrong one
half the time, since two outputs at different scales derive different halos from
the same kernel. It is a maximum now.

## M4F.2C.4 — a rotated output rendered 65 empty frames a second

### Classification came first, and moved the bug out of blur

M4F.2C reported "894 blur nodes and no idle" at 270°. Before touching anything,
`contrib/avk-transform-classify.sh` ran four transforms against three controls —
no blur at all, blur away from the seam, blur across it — reporting **per-frame**
figures, because a run total cannot tell a traversal defect from a scheduling
one.

```text
rr   control    frames(6s idle)   emitted   per-frame
0    none             0              0          0
1    none           390              0          0      ← blur DISABLED
2    none             0              0          0
3    none           390              0          0      ← blur DISABLED
1    seam           390           1560          4
```

**With blur disabled entirely, 90° and 270° rendered 390 frames in six static
seconds.** Not a blur bug. And 894 was four nodes per frame over many frames, so
scene traversal was never involved.

### The root cause: a clip in the wrong coordinate space

`scene_output_damage()` receives damage that `output_to_buffer_coords()` has
already converted into **buffer** coordinates — sizing itself with
`wlr_output_transformed_resolution()` — and then clips it against
`output->width/height`, the **raw mode size**. On a 90° or 270° output those two
differ by a transpose.

The consequence was not a lost rectangle but an **immortal** one. On a 90°
output with an 800×600 mode the buffer is 600×800, and the clip admitted damage
out to x = 800: a region `600,0 200×600`, entirely outside the buffer.

```text
nothing can draw a rectangle outside the buffer
    → no commit's damage ever subtracts it
    → wlr_scene_output_needs_frame() is true forever
    → 65 empty frames a second on a static scene
```

**It never showed on the SceneFX path** because that path commits
`pending_commit_damage` itself, so the subtraction cancels whatever it recorded,
right or wrong. AVK computes and reports its own damage and would never include
an out-of-buffer rectangle — so AVK made a latent coordinate-space error
*observable*, it did not cause it.

Both the clip and M4F.2C's halo widening now use the transformed resolution. At
0° and 180° the two resolutions are identical, so the shipping path is
bit-identical. **267 frames in four idle seconds → 0**, and all twelve
classification cases now idle at 0.

### The 180° strip — narrowed to one path, not closed

It is real and persistent. One run came back clean and it was luck; three
consecutive runs measured **22 / 13 / 13** stale pixels.

Three bisections, each a single measurement:

| fixture | stale pixels |
| --- | --- |
| single output, 180°, blur, small change, `damage_all` | **0** (13/13, same as 0°) |
| two outputs, 180°, blurred window **far** from the seam | **0** |
| two outputs, 180°, blurred window **across** the seam | **13** |
| two outputs, 180°, across the seam, `BREAK=blur-source-output-clip` | **0** |

So it needs two outputs *and* a cross-output halo, and turning off the **source
halo** — while leaving damage routing fully on — removes it. **The defect is in
the halo's source reconstruction under a 180° transform, not in damage
routing.**

The stale strip sits at buffer `x 771..794`, which under 180° is logical
`x 6..29`: output A's *logical left*, the far edge from the seam, while the halo
damage from B arrives at buffer `x ∈ [-126, 0)`. That the failure appears
mirror-opposite to the band that changed is the thread to pull next.

`source_bounds`, the walker's retention window and the halo rect are all
symmetric in ±halo on both axes, so none of them can be the asymmetry on its
own. The scene is not symmetric — at 0° the seam-side halo band holds the
neighbouring monitor's content and the far-side band holds nothing; at 180° they
swap — so the next place to look is where a capture near the far edge is clamped
to `source_bounds` and then aligned to an even origin, which is the one
operation in the chain that grows a region on the low side only.

**180° is therefore NOT closed**, and no fix is claimed for it.
