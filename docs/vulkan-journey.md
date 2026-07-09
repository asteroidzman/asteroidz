# asteroidz on Vulkan — decisions, state, and how to switch

Living record of the Vulkan effort so we can switch renderers at will and never
re-litigate what we already learned. Keep this updated as decisions change.

## Why Vulkan
Goal: run asteroidz on a Vulkan renderer for HDR10 output, native explicit
(timeline) sync, compute-shader blur, and a cleaner color pipeline — parity with
sway 1.12's Vulkan/HDR10 path. Not for raw FPS.

## The three build configurations (how to switch)

| Config | Renderer | scenefx? | wlroots | Colors/effects | Status |
|--------|----------|----------|---------|----------------|--------|
| **A. scenefx + GLES** (original) | scenefx GLES2 | yes | 0.20 | full (blur/shadow/rounded/SDR color) | rock solid — the daily driver |
| **B. scenefx + fx_vk (Vulkan)** | scenefx `fx_vk` (vendored wlroots Vulkan) | yes | 0.21 | **rounded corners ported (step 2.1); blur/shadow/gradient + SDR color still NO-OP (WIP)** | boots, renders windows + rounded corners/borders; SDR colors still wrong, no blur/shadow |
| **C. wlroots built-in Vulkan (no scenefx)** | wlroots vulkan | no (nofx.h) | 0.20 | none (nofx stubs) | boots, renders windows; kept only as a reference point |

Switching:
- **A (fall back to the working desktop):** `git checkout main` (scenefx+GLES), rebuild, log into the plain **Asteroidz** session. This is always the safe fallback.
- **B (Vulkan WIP):** the `vulkan` branch; log into **Asteroidz (Vulkan WIP)** which runs `/usr/local/bin/asteroidz-vulkan`.
- Session launcher `/usr/local/bin/asteroidz-vulkan` selects the Vulkan binary + env.

## Root causes found (each cost real debugging — do not re-chase)

1. **Total blank / `MOD_INVALID` flood** — caused by pinning the GPU with
   `WLR_DRM_DEVICES=NAVI31`. On this multi-GPU box (RX 7900 XT / Navi31 = display
   GPU, + Raphael iGPU) the pin makes the compositor single-GPU while Mesa
   clients render on their default GPU → cross-GPU buffers arrive with
   `DRM_FORMAT_MOD_INVALID` (implicit modifiers). **wlroots' Vulkan renderer
   cannot import implicit-modifier DMABUFs** (EGL/GLES can); import fails →
   `[types/buffer/client.c] Failed to create texture` → blank.
   **FIX: do NOT pin the GPU.** Launch like `WLR_RENDERER=vulkan` (sway does),
   let wlroots auto-select; clients + compositor land on the same GPU and
   negotiate explicit modifiers.

2. **`wl_drm` legacy protocol** also hands out implicit-modifier buffers. sway's
   `server_init` does NOT create `wl_drm` and advertises `zwp_linux_dmabuf_v1`
   **v5**. asteroidz created `wl_drm` + advertised v4. Aligning to sway (drop
   `wl_drm`, v5) is the clean setup, though with no GPU pin, `wl_drm`+v4 also
   rendered focused windows. (This change is NOT in the current `vulkan` branch
   tip — see "Current state".)

3. **Unfocused windows blank (the long one)** — NOT a renderer bug. `Client.border`
   is a single **full-window solid `wlr_scene_rect`** (`asteroidz.c` ~5523). In
   the scenefx build `apply_border()` carves it hollow via
   `wlr_scene_rect_set_clipped_region()`. Both `nofx.h` (config C) **and** our
   `fx_vk` dispatch (config B) turn that clip into a **no-op**, so the border
   rect stays **fully solid**. On focus-out asteroidz did
   `wlr_scene_node_raise_to_top(&c->border->node)` → the opaque rect covered the
   whole client → gray/black blank; refocus lowered it. **FIX (kept):
   `src/animation/client.h` unfocus path lowers the border instead of raising
   it**, so content stays visible in both focus states (matches sway, which
   never restacks on focus).

4. **Colors too bright / wrong on Vulkan** — the SDR pipeline
   (`wlr_scene_set_sdr_reference_luminance`, `wlr_scene_set_sdr_saturation`, your
   `reference-luminance 280` / `saturation 1.25`) lives in scenefx's **GLES**
   renderer. `nofx.h` stubs those calls, and our `fx_vk` port no-ops the effect/
   color path, so SDR saturation + reference luminance are **not applied** on any
   Vulkan config yet. Restoring them = porting scenefx's color/effect shaders to
   `fx_vk` (the big remaining work). wlroots 0.20 wlr_scene has no native
   equivalent (scenefx-only feature).

## Key renderer facts
- wlroots' Vulkan renderer (both 0.20.1 and 0.21-dev, byte-identical in the parts
  we vendored) rejects `DRM_FORMAT_MOD_INVALID` DMABUF imports. This is why the
  GPU pin and `wl_drm` both broke rendering.
- The Vulkan renderer's two-pass color-transform path had sync hazards under
  synchronization validation, but that was a red herring for the blank — the
  border rect was the cause. (Left as a note, not a confirmed rendering bug.)
- `fx_vk` = wlroots 0.21 `render/vulkan/` vendored into scenefx under
  `render/fx_renderer/vulkan/`, every `wlr_vk_*`/`vulkan_*` renamed to
  `fx_vk_*`/`fx_vulkan_*` so it doesn't clash with libwlroots. Public entry:
  `fx_vk_renderer_create_with_drm_fd()`. `renderer_autocreate()` dispatches to it
  when `WLR_RENDERER=vulkan`.
- scenefx's scene is renderer-agnostic via `fx_render_pass_try_get()` +
  `scene_pass_add_*` helpers: base surfaces/rects route to the plain
  `wlr_render_pass` on Vulkan. As of step 2.1 **rounded corners/textures render on
  `fx_vk`** (`fx_vk_render_pass_add_rounded_rect`/`_grad`/`add_texture`); shadow,
  blur, real gradients and the SDR color path are still no-ops.
  A full-damage workaround forces a full repaint on the Vulkan path.

## wlroots version notes
- Config A/C: wlroots **0.20**. Config B (`fx_vk`) was re-vendored from wlroots
  **0.21-dev**, so scenefx's `build-vulkan` and asteroidz both build against
  `wlroots-0.21`. Only API delta in asteroidz: `wlr_xdg_decoration_manager_v1_create`
  gained a 2nd (version) arg in 0.21. scenefx's GLES base builds clean against 0.21.

## TODO to make Vulkan a real daily driver
1. Port scenefx effect shaders to `fx_vk` (SPIR-V): rounded corners → box shadow →
   blur → gradients → color LUT. **Rounded corners DONE (step 2.1).** Next: box
   shadow → blur → real gradient shader → color LUT.
2. Port the SDR color pipeline (reference luminance + saturation) to `fx_vk` so
   HDR/SDR colors match config A.
3. Revisit the full-damage workaround once partial damage is correct.
4. Revisit the border lower-to-bottom workaround (root cause #3): now that the
   rounded-rect clip cutout renders on `fx_vk`, the hollow border no longer covers
   content, so the unfocus lower could likely be dropped / focus animations
   re-enabled. Not yet done — verify before removing the workaround.

## Progress log

### Step 2.1 — rounded corners + rounded textures on `fx_vk` (scenefx `5300811`)
- New SPIR-V shaders `quad_round.frag` / `texture_round.frag` mirror the GLES
  `quad_round` + `corner_alpha` and `tex` + `corner_alpha` paths (same SDF, same
  per-pixel fudge, same `is_cutout` interior-clip semantics). Added to the
  vulkan `shaders/meson.build`.
- `pass.c`: `fx_vk_render_pass_add_rounded_rect` / `_grad` / `add_texture` push a
  dedicated corner+clip frag push-constant block; `wlr_scene.c` routes rounded
  scene rects/buffers to them under `FX_HAS_VULKAN`.
- **Corner-coordinate gotcha:** corner geometry must NOT use `gl_FragCoord`. Under
  the `FLIPPED_180` projection `gl_FragCoord` is in flipped framebuffer space and
  only matches box space for vertically-centred boxes — offset boxes (window
  borders, titlebar'd content) rounded the wrong edges (monocle looked fine,
  titlebar'd windows rounded the top instead of the bottom). Fix: a new `box_pos`
  varying from `common.vert` carrying the unit-quad coordinate (layout-top-left
  origin), used for both the quad and the interior-clip geometry.
- **Border colour gotcha:** `scenefx.kdl` enables `border { gradient { enable 1 } }`,
  so the focused window takes the gradient path. `fx_vk` has no gradient shader yet
  and `wlr_scene_rect_set_gradient` never updates `rect->color`, so the fallback
  rendered the rect's stale solid colour (the last inactive border colour) → the
  active border read as the inactive colour. Fix: the `fx_vk` gradient fallback now
  fills with the gradient's **first stop** (the focus colour), giving a solid
  focused border that matches the flat pills.

### Border/pill colour sharing (DMS, not renderer)
- Window borders now share the DMS pill palette: matugen template
  `asteroidz-colors.kdl` maps the resting border `color` to
  `surface_container_high` (= pill `bg-color`) and `focus-color` to `primary`
  (= pill `focus-bg-color`). Border `width` knob added to `config.kdl`
  (`layout/border/width`); colours stay in the generated `dms/colors.kdl`.
