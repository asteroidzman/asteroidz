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
