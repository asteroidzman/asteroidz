---
title: Window Effects
description: Add visual polish with blur, shadows, and opacity.
---

## Blur

Blur creates a frosted glass effect for transparent windows.

| Setting | Default | Description |
| :--- | :--- | :--- |
| `blur` | `0` | Enable blur for windows. |
| `blur_layer` | `0` | Enable blur for layer surfaces (like bars/docks). |
| `blur_optimized` | `1` | Caches the wallpaper and blur background, significantly reducing GPU usage. Disabling it will significantly increase GPU consumption and may cause rendering lag. **Highly recommended.** |
| `blur_params_radius` | `5` | The strength (radius) of the blur. |
| `blur_params_num_passes` | `2` | Number of passes. Higher = smoother but more expensive. `1` leaves visible bleed-through of background detail; `2` is the minimum that reads as a proper blur. |
| `blur_params_noise` | `0.02` | Blur noise level. |
| `blur_params_brightness` | `0.9` | Blur brightness adjustment. |
| `blur_params_contrast` | `0.9` | Blur contrast adjustment. |
| `blur_params_saturation` | `1.2` | Blur saturation adjustment. |
| `blur_unfocused_strength` | `1` | Blur strength for unfocused windows, animated back up to full on focus, so focus reads as depth. Values **below** `1` weaken it — `0.5` blurs an unfocused window half as much. `1` and above are ignored, which is what makes `1` mean "off". |

> **Warning:** Blur has a relatively high impact on performance. If your hardware is limited, it is not recommended to enable it. If you experience lag with blur on, ensure `blur_optimized=1` — disabling it will significantly increase GPU consumption and may cause rendering lag. To disable blur entirely, set `blur=0`.

---

## Blend space

```kdl
effects { blend-space "srgb" }   // or "linear" (default)
```

Which space client alpha is composited in, on the Vulkan renderer. The option is
`blend-space` in KDL, `srgb_blending` in the flat form.

`linear` is the default and is physically correct: the renderer composites into
a 16-bit float buffer in linear light, which is what colour management and HDR
need. It also means alpha behaves differently here than on compositors that
blend encoded values — 5% of a bright backdrop through a dark window is 5% of
its **light**, and sRGB spends most of its code range on darks, so it lands far
up the visible scale. Measured, over a `#e0e0e0` backdrop behind a `#16130b`
window:

| `background_opacity` | linear | srgb |
| :--- | :--- | :--- |
| 0.90 | (70, 69, 68) | (36, 33, 26) |
| 0.95 | (53, 52, 50) | (29, 26, 18) |
| 0.98 | (38, 37, 34) | (25, 22, 14) |

At 0.95 the linear result puts a legible photograph of the wallpaper through a
terminal, which is not what anyone means by "95% opaque". Application authors
pick alpha by eye on compositors that blend encoded values, so `srgb` makes
translucent surfaces look the way their authors intended.

Chasing the number instead does not work: to match encoded 0.95 you would need
about 0.99 for a dark window over a bright backdrop and about 0.90 for a bright
window over a dark one. The required correction depends on the destination
pixel, which the shader never sees — only the blend space fixes it.

`srgb` is **ignored** wherever real colour management is in play: a PQ (HDR)
source, or an output carrying a colour transform. Those need linear light, and
an appearance preference must not be able to break them — so a `force_hdr`
window keeps the correct path while everything else blends the expected way.

The GLES renderer already blends encoded values; the setting is a no-op there.

---

## Shadows

Drop shadows help distinguish floating windows from the background.

| Setting | Default | Description |
| :--- | :--- | :--- |
| `shadows` | `0` | Enable shadows. |
| `layer_shadows` | `0` | Enable shadows for layer surfaces. |
| `shadow_only_floating` | `1` | Only draw shadows for floating windows (saves performance). |
| `shadows_size` | `24` | Size of the shadow. |
| `shadows_blur` | `24` | Shadow blur amount. |
| `shadows_position_x` | `0` | Shadow X offset. |
| `shadows_position_y` | `10` | Shadow Y offset. The default's downward offset (light source from directly above) gives floating windows a macOS-style "raised" look instead of a symmetric halo. |
| `shadows_contact` | `1` | Enable the macOS-style tight "contact" shadow layered above the ambient one |
| `shadows_contact_size` | `8` | Contact shadow spread in px |
| `shadows_contact_blur` | `9` | Contact shadow blur sigma |
| `shadows_contact_position_x` | `0` | Contact shadow X offset |
| `shadows_contact_position_y` | `2` | Contact shadow Y offset |
| `shadowscolor_contact` | `0x0000004d` | Contact shadow color (RGBA hex) |
| `shadows_unfocused_scale` | `0.45` | Shadow opacity multiplier for unfocused windows (macOS-style dimming) |
| `shadows_tiled_scale` | `0.3` | Multiplies `shadows_blur` for tiled windows, which sit flush against their neighbours and have far less room for a shadow than a floating one does. |
| `shadowscolor` | `0x00000066` | Color of the shadow. Kept well below opaque: a near-opaque peak alpha reads as a hard drop shadow once spread this wide. |
| `shadows_blur_background` | `0` | Blur what is under the shadow as well as darkening it. Costs a blur pass per shadowed window, so it is off by default. |
| `shadows_blur_background_strength` | `0.5` | Opacity of that blur, so it can be mixed with the plain tint rather than replacing it. No effect unless `shadows_blur_background` is `1`. |
| `shadows_blur_background_darken` | `1` | Clamp that blur against the unblurred backdrop so a shadow can never brighten what it covers (see [A shadow may only ever darken](#a-shadow-may-only-ever-darken)). Set to `0` to get the plain blur back. |

`shadows_blur_background` keeps the shadowed window's own box out of what the
blur samples. It has to: the blur's box is the *shadow's* box, which is the
window plus its spread, so the sampled region covers the window itself — and
the scene image holds the previous frame there, because the blur draws beneath
the window and an undamaged region is never re-rendered. Without the exclusion
the blur picks the window's own pixels up and spreads them outward: a halo in
the window's own colour, a glow rather than a shadow on a dark backdrop
(measured on black: 13 levels of stray light on GLES, 71 on Vulkan, 0 with it).

Under the window the true backdrop is unknowable, and what goes in its place is
what the kernel spreads outward — so it cannot be a stand-in for the backdrop,
it has to be a continuation of it. The excluded box is filled from its own
edges: the strips above and below it are stretched over a half each, covering
it between them, and the strips to either side are then stretched over the
quarter nearest them so that a vertical edge's own content wins where its reach
is. Near an edge, which is the only place the blur's reach matters, the source
then holds more of the very thing being blurred.

That stretch is only ever a base coat. Within the blur's REACH of each edge —
the only depth that can influence a pixel outside the hole at all — the fill is
a **reflection** of the real content across that edge instead. Mirroring is the
standard boundary mode for convolution because it preserves the neighbourhood's
statistics; clamp-to-edge replaces them with a single sample, and a single
sample of structured content is not a sample of anything. Measured with a
six-pixel bright band running under a floating window, the stretch turned it
into two hundred and fifty pixels of solid saturated colour inside the source,
which the blur duly spread out as a coloured halo. On a terminal that "band" is
a line of text, so the halo took the colour of whatever sat on that one scanline
and changed as the text did. Past the reach the stretch stands and does not
matter: nothing that deep can reach the outside, so it only has to not be the
window.

Two further details are load-bearing, and both were got wrong first:

- The stretch samples with **nearest**, not linear. Every source strip is one
  pixel wide, and a Vulkan blit's filter samples the source *image* rather than
  the source rectangle — so with linear filtering each destination texel came
  out a blend of the ring pixel and the pixel just inside the hole, which is the
  window's own edge, ramping to about half the window's colour by the far end of
  the stretch. That is a gradient back into exactly the content the exclusion
  exists to remove.
- The rows cover the hole **between them**. They used to fill a quarter from
  each of the four sides and leave the middle, on the reasoning that the middle
  is further from every edge than the kernel can reach. That holds for a large
  window and fails for a small one: the fill reaches a quarter of the way in and
  the blur reaches whatever its padded region is, so once a window is narrower
  than roughly eight times that, the untouched middle — pure window — is inside
  the kernel's reach. A dialog or a notification card is exactly that window.

None of these shows up on a screenshot, because the blur averages the evidence
away before anything reaches the screen. See
[Dumping a blur's source](#dumping-a-blurs-source).

### A shadow may only ever darken

Getting the source right is not enough, and for a long time this looked like the
same bug wearing a different hat. It is a separate one, and it is not in the
exclusion at all.

A blur is an average, and averaging bright detail over a dark ground **raises
the mean wherever the ground is dark**. A terminal is mostly dark with sparse
bright text, so its blur genuinely comes out lighter than the thing it replaced.
The shadow's own alpha ramps up toward the window, so the closer to the edge the
more of that lifted version shows: a smooth brightening gradient running into
the window. A photograph is already smooth and its mean barely moves, which is
why this was only ever visible over other *windows* and never over a wallpaper.

So the shadow's blurred backdrop is clamped per pixel against its own unblurred
source, and can never come out lighter than what it replaced. The clamp is free:
the mip chain walks away from mip 0 and only returns on the final upsample, so
mip 0 still holds the untouched source until that pass overwrites it — one image
read, no extra buffer and no copy. It applies to shadow backdrop blurs and
nothing else, because a frosted panel is *supposed* to be able to come out
lighter than what is behind it.

Compute path only. The GLES renderer and the Vulkan graphics ping-pong fallback
have both overwritten the source by the time the last pass runs.

`contrib/shadow-darken-test.sh` holds the line, against fine bright lines on
black — the structure of text without needing a terminal to make it. Measured
there: 60 levels of stray light in the shadow band before the clamp, 0 after.

**The clamp stops at the hole.** Its whole premise is that the source it takes
the minimum against is the real backdrop, and inside the excluded window box
that is false: what sits there is the fill described above — a stretched pixel
row and a mirrored band, a fabrication that only has to keep the window from
bleeding out of its own shadow. Clamping against it drags the region down toward
the fill's own darkest structure. Behind an opaque window nobody can tell; a
translucent one shows a dark patch shaped exactly like itself.

So the clamp is skipped inside that box, and the plain blur stands there. It is
the same principle as the clamp itself, applied in the other direction: a
minimum is only meaningful against something true.

`contrib/shadow-exclude-clamp-test.sh` asserts it, and needs two renderings of
one scene to do so — inside the hole they must agree, below the window they must
not, the second being what proves the clamp was running at all. `FX_BLUR_NO_
DARKEN_CLAMP=1` turns the clamp off for a process so that comparison can be
made; it exists for that test and is not otherwise useful. Measured: 9 levels of
difference inside the hole through a 15%-opaque window before the fix, 0 after.

The first attempt substituted the unblurred wallpaper snapshot there instead,
on the reasoning that the wallpaper is what lies under everything. It is, but a
floating window usually sits over *another window* rather than over the
wallpaper, and a wallpaper brighter than that window bled out as a halo of its
own — the same artefact wearing different colours (measured on a dark window
under a bright wallpaper: 8 levels on GLES, 28 on Vulkan, 0 with edge
extension). See `wlr_scene_blur_set_sample_exclude()` in scenefx.

```kdl
effects {
    shadow {
        enable 1
        layer 1
        only-floating 1
        size 12
        blur 15
        color 0x000000ff
        position {
            x 0
            y 0
        }
    }
}
```

### Dumping a blur's source

A backdrop blur's *source* is a scratch image that never reaches the screen: the
scene so far, copied aside and then patched to keep the shadowed window out of
it. Every question about a halo around a floating window — is the hole filled,
with what, does the fill reach far enough — is a question about that image, and
a screenshot cannot answer any of them, because the blur has already averaged
the evidence away by the time anything is visible.

So it can be written out. Vulkan only; the GLES path patches the hole in a
shader with no equivalent image to read back.

```sh
# at startup, before anything else has drawn
FX_BLUR_DUMP=/tmp/blur FX_BLUR_DUMP_FRAMES=3 asteroidz

# or on a session that is already running, which is the point:
# a restart severs every client, which is a steep price for three frames
amsg dispatch 'dump_blur_source,/tmp/blur,3'
amsg dispatch dump_blur_source          # no argument disarms
```

Each armed frame writes `/tmp/blur-<n>-staged.pam` (the source as copied, hole
still full of the window) and `/tmp/blur-<n>-patched.pam` (after the fill), plus
a `.txt` sidecar giving the blur region and the excluded box in screen
coordinates so a measurement taken in the crop can be stated on screen. PAM
rather than PNG because the source is premultiplied and the alpha is part of the
evidence; ImageMagick reads it.

It stops on its own after the requested frames. It has to: each armed frame ends
in a full device wait, so leaving it on turns a session into a slideshow.

`contrib/blur-exclusion-test.sh` is this facility as an assertion — that no
pixel of a window survives anywhere in its own shadow's blur source.

---

## Opacity & Corner Radius

Control the transparency and roundness of your windows.

| Setting | Default | Description |
| :--- | :--- | :--- |
| `border_radius` | `0` | Window corner radius in pixels. |
| `border_radius_location_default` | `15` | Corner-location BITMASK for the radius: `1` top-left, `2` top-right, `4` bottom-right, `8` bottom-left; combine by adding (`15` = all, `0` = none). |
| `no_radius_when_single` | `0` | Disable radius if only one window is visible. |
| `focused_opacity` | `1.0` | Opacity for the active window (0.0 - 1.0). |
| `unfocused_opacity` | `1.0` | Opacity for inactive windows (0.0 - 1.0). |

```kdl
misc {
    border_radius 0
    border_radius_location_default 15
    no_radius_when_single 0
    focused_opacity 1.0
    unfocused_opacity 1.0
}
```
