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
| **B. scenefx + fx_vk (Vulkan)** | scenefx `fx_vk` (vendored wlroots Vulkan) | yes | 0.21 | **rounded corners (2.1) + box shadow (2.2) + blur (2.3) + SDR colour all working; only real-gradient still NO-OP** | boots, renders windows + rounded corners/borders + drop shadows + blur; SDR colours match config A |
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

4. **Colors too bright / wrong on Vulkan — RESOLVED / was never an fx_vk gap.**
   (Historic note; superseded.) The SDR settings (`reference-luminance 280`,
   `saturation 1.25`) are NOT a GLES-only shader path. `scene_output_combine_color_transforms`
   (`types/scene/wlr_scene.c` ~3289-3343) bakes the SDR reference-luminance
   multiplier and the saturation matrix into the **output colour-transform
   matrix** (`wlr_color_transform_init_matrix`) — renderer-agnostic. `fx_vk`
   applies it in the two-pass **output resolve** (`output.frag` `mat3(matrix)*rgb`
   + `pass->color_transform` in `render_pass_submit`, `pass.c` ~301), and applies
   the per-texture `luminance_multiplier` in `render_texture` (`pass.c` ~951).
   Verified visually: SDR colours on config B match config A. No fx_vk work
   needed. (The earlier "GLES-only no-op" belief predated asteroidz moving SDR
   into the colour-transform matrix.)

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
   blur → gradients → color LUT. **Rounded corners DONE (2.1), box shadow DONE
   (2.2), blur DONE (2.3).** Next: real gradient shader → color LUT.
2. ~~Port the SDR color pipeline to `fx_vk`~~ — NOT needed; SDR (reference
   luminance + saturation) already applies via the renderer-agnostic output
   colour-transform matrix (see root cause #4). Verified matching config A.
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

### Step 2.2 — box shadow on `fx_vk` (scenefx `07f5dc2`)
- New SPIR-V `box_shadow.frag` ports the GLES `box_shadow.frag` (Evan Wallace's
  fast rounded-rectangle gaussian) + the `corner_alpha` interior-clip cutout.
- Same `box_pos`-varying fix as 2.1 for the shadow sample point (not
  `gl_FragCoord`). Output is **linear + premultiplied** for the fx_vk
  premultiplied-blend pipeline; GLES worked in straight-alpha sRGB with a
  `SRC_ALPHA` blend, so the colour is linearised and premultiplied by the
  per-pixel shadow mask in the shader.
- `WLR_VK_SHADER_SOURCE_BOX_SHADOW` pipeline + `render_box_shadow` /
  `fx_vk_render_pass_add_box_shadow`; `scene_pass_add_box_shadow` gains the
  `FX_HAS_VULKAN` branch. Reuses the shared fx pipeline layout — `color@80`,
  corner block `@96`, scalar `blur_sigma@160`, all within the reserved 80..224
  fragment push-constant range (no new layout).
- Enabled via `scenefx.kdl` `shadow { enable 1 }`; asteroidz shadow scene nodes
  now render on Vulkan (were a no-op before).

### Step 2.3 — blur on `fx_vk` (IN PROGRESS, phased)
Full-parity blur. The user chose the full multi-pass restructure over the
lighter optimized-only path. Phased; commits on the scenefx `vulkan` branch.

- **Phase A DONE (scenefx `6231ec6`)** — foundation, inert. Ported
  blur1/blur2/blur_effects to SPIR-V; `WLR_VK_SHADER_SOURCE_BLUR1/2/EFFECTS`
  pipelines (tex layout sampler, `BLEND_MODE_NONE`). `fx_vk_effect_image`:
  device-local 16F `COLOR_ATTACHMENT|SAMPLED` image kept in `GENERAL` layout
  (no manual transitions — the 16F effect render pass uses GENERAL init/final
  and its subpass deps guard write→read). `fx_vk_effect_buffers`: per-output
  set (ping/pong + optimized + optimized_no_blur + saved_pixels), cached on the
  wlr_output via an addon, resize-aware.
- **Phase B DONE (scenefx `5271449`)** — `fx_vk_render_pass_blur`, inert.
  Dual-Kawase down/upsample ping-ponging the effect buffers, returning the
  blurred image. Mirrors GLES `get_main_buffer_blur`: dst/src boxes stay
  full-buffer; only the scissor scales (`>>(i+1)` down, `>>i` up); halfpixel is
  constant. Includes the optional blur_effects post pass. Each iteration drives
  its own effect render pass (must run with NO main pass active).
- **THE OBSTACLE (why Phase C is a restructure):** the two-pass HDR render is a
  **single `VkRenderPass` with two subpasses** — subpass 0 draws the scene into
  the 16F blend image, subpass 1 (fired by `vkCmdNextSubpass` in
  `render_pass_submit`) resolves it to output with the colour transform. You
  **cannot end that pass mid-frame** to blur (Vulkan can't sample the attachment
  being written; ending early skips the resolve). Blur fundamentally needs to
  sample "content so far", so the main pass must be splittable.
- **Phase C PLAN (restructure — the chosen path):** convert the two-pass from
  an *input-attachment* resolve to a *sampled scene image*:
  1. `output.frag`: `subpassInput`+`subpassLoad` → `sampler2D`+`texture(,uv)`.
  2. Output DS: `INPUT_ATTACHMENT` → `COMBINED_IMAGE_SAMPLER`
     (`fx_vulkan_alloc_blend_ds` + `output_ds_srgb_layout` binding); blend/scene
     image gains `SAMPLED_BIT`.
  3. Split the 2-subpass render pass into two single-subpass passes: a *scene*
     pass (draws into the scene image, `finalLayout=SHADER_READ_ONLY`) and an
     *output* pass (samples scene image → output). Two framebuffers.
  4. `render_pass_submit`: replace `vkCmdNextSubpass` with end-scene-pass →
     begin-output-pass → bind output pipe + scene-image sampler DS → fullscreen
     draw.
  5. Then blur hook: at `OPTIMIZED_BLUR` (dirty only) end the scene pass, blur
     by sampling the scene image, restart it (`loadOp=LOAD`); `BLUR` nodes
     sample the cached optimized image (no split). Also need fx_vk
     `scene_pass_add_blur` / `_add_optimized_blur` / `scene_pass_has_blur`.
  Steps 1–4 land as ONE coherent unit (rewrites the core output path for ALL
  rendering; can't be partially landed) and need a **restart-test** before the
  blur hook is added — isolates "restructure broke normal rendering?" from
  "does blur work?". Fallback if the restructure misbehaves: scenefx `5271449`
  (blur inert, renderer works as before).
- **Phase C DONE + VERIFIED (scenefx `1a6a8b3` restructure, `ff680ef` wiring;
  merged to `vulkan`).** Built on branch `vulkan-blur-restructure`,
  restart-tested (normal rendering identical after the restructure), blur wiring
  added, restart-tested again (blur renders correctly — no flip/corruption),
  then fast-forward merged into `vulkan`. Blur is live on config B.
  - Restructure (`1a6a8b3`): scene/blend image now `COLOR_ATTACHMENT|SAMPLED` in
    `GENERAL` for life; the 2-subpass pass split into a scene pass
    (`render_setup->render_pass` via `two_pass.scene_framebuffer`) + standalone
    `output_render_pass`; `output.frag` samples `sampler2D` at
    `gl_FragCoord/textureSize` (1:1 with the old subpassLoad); output DS is
    `COMBINED_IMAGE_SAMPLER` + `renderer->output_sampler`; `render_pass_submit`
    ends the scene pass then runs the separate output pass.
  - Blur wiring (`ff680ef`): `fx_vk_render_pass_add_optimized_blur` splits the
    scene pass (end → `vkCmdCopyImage` scene image into `optimized_no_blur` →
    `fx_vk_render_pass_blur` → copy into the `optimized_blur` cache → restart
    with `loadOp=LOAD`); `fx_vk_render_pass_add_blur` draws the cache at the node
    box (`render_effect_image`); fx_vk `scene_pass_*` branches +
    `fx_vk_render_pass_init_blur` (per-output effect buffers, full-damage path so
    no `blur_padding_region` machinery).
  - First-cut simplifications still open (revisit): `blur_strength` partial mixing
    and `ignore_transparent`/transparency-mask stenciling not implemented;
    per-window blur always samples the whole-background cache; blur only on the
    two-pass path. None block the daily-driver look.

### Border/pill colour sharing (DMS, not renderer)
- Window borders now share the DMS pill palette: matugen template
  `asteroidz-colors.kdl` maps the resting border `color` to
  `surface_container_high` (= pill `bg-color`) and `focus-color` to `primary`
  (= pill `focus-bg-color`). Border `width` knob added to `config.kdl`
  (`layout/border/width`); colours stay in the generated `dms/colors.kdl`.
