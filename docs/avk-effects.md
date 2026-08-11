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
where it should be rounded. M4B.

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

## Traps already paid for

- **Flat-colour test scenes are blind to blur bugs.** Both `sample_exclude` and
  `darken` were invisible to every flat-backdrop scene in the suite; the tests
  that found them use fine bright lines on black.
- **Premultiplied vs straight colour** darkens translucent panels by exactly
  their own alpha and looks deliberate.
- **A test whose right and wrong answers coincide on this GPU is not
  coverage.** Mixed scales and high-frequency content are what make effect
  bugs observable; see `docs/regression-testing.md`.
