# asteroidz on Vulkan — decisions, state, and how to switch

Living record of the Vulkan effort so we can switch renderers at will and never
re-litigate what we already learned. Keep this updated as decisions change.

**GLES (config A) is the primary, everyday driver. Vulkan (config B) is
experimental.** Config B has reached near-parity for what asteroidz actually
renders (see the table below) and is stable enough to run as a daily session,
but it's still the WIP path — GLES is where correctness/stability bugs get
triaged first, and Vulkan-only issues (Electron blank windows, HDR10 not yet
declared verified end-to-end) are accepted trade-offs, not blockers for using
GLES.

## Why Vulkan
Goal: run asteroidz on a Vulkan renderer for HDR10 output, native explicit
(timeline) sync, compute-shader blur, a cleaner color pipeline — parity with
sway 1.12's Vulkan/HDR10 path — **and raw performance**: FPS/GPU-efficiency
improvements are an explicit target (2026-07-12 revision; the original "not
for raw FPS" framing is obsolete).

## The two build configurations (how to switch)

| Config | Renderer | scenefx? | wlroots | Colors/effects | Status |
|--------|----------|----------|---------|----------------|--------|
| **A. scenefx + GLES** (original) | scenefx GLES2 | yes | 0.20 | full (blur/shadow/rounded/SDR color) | rock solid — the daily driver |
| **B. scenefx + fx_vk (Vulkan)** | scenefx `fx_vk` (vendored wlroots Vulkan) | yes | 0.20 (0.21 no longer required — see below) | **rounded corners (2.1) + box shadow (2.2) + blur (2.3) + 2-stop gradients + SDR colour all working; partial damage tracking now correct (2026-07-14).** Remaining gaps: >2-stop gradients (first-stop fallback), colour-LUT (unused by asteroidz anyway), Electron/native-Wayland clients render blank (dmabuf modifier import limitation). HDR10 is under active hardening (several PQ-correctness fixes landed 2026-07-13/15) but not yet declared verified end-to-end. | near-parity with config A for everything asteroidz actually uses day-to-day; Electron apps are the one concrete dealbreaker if used |

Config C (wlroots built-in Vulkan, no scenefx, `nofx.h`) was **dropped**. It was
only ever a reference point; asteroidz always builds against scenefx
(`meson.build` deps `scenefx-0.5` unconditionally) and there is no `nofx.h` in
the tree. Removing it let us revert the border z-order workaround (below) — the
only reason for it was config C's no-op clip.

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

3. **Unfocused windows blank (the long one)** — NOT a renderer bug, and now
   **REVERTED** (the workaround is gone). `Client.border` is a single
   **full-window solid `wlr_scene_rect`** carved hollow via
   `wlr_scene_rect_set_clipped_region()`. This only blanked on **config C**
   (`nofx.h`), which no-oped the clip → raising the solid rect on focus-out
   covered the client. The interim fix lowered the border instead of raising it.
   Once `fx_vk` gained the rounded-rect clip cutout (step 2.1) the border renders
   as a proper hollow ring on config B, and config C was dropped — so the
   `src/animation/client.h` unfocus path was **reverted to the original
   `wlr_scene_node_raise_to_top`**. Verified on config B: unfocused windows stay
   visible (the border is a hollow ring, so raising it paints only the frame).
   (History only; nothing to re-chase.)

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

## Vulkan has gone as far as it can go on the current wlroots
`fx_vk` is a **vendored, renamed fork** of wlroots' `render/vulkan/` (see "Key
renderer facts" below), not a live dependency on upstream wlroots — it stopped
tracking wlroots' own Vulkan renderer the moment it was copied in and
scenefx-ified. That has two consequences worth being explicit about:

- **No wlroots version upgrade fixes anything remaining here.** The two open
  gaps (Electron/native-Wayland clients rendering blank via a dmabuf modifier
  `fx_vulkan_import_dmabuf` can't import, and HDR10 not yet declared verified
  end-to-end) are bugs/gaps in the vendored-and-since-modified fx_vk code
  itself, not upstream wlroots bugs a newer release would carry a fix for.
  Any further correctness work has to happen in scenefx directly.
- **The multi-threading question is closed for the same reason** (see the
  2026-07-17 investigation in the progress log): wlroots upstream has zero
  threading anywhere in its codebase, no roadmap item for it, and nothing
  about "updating wlroots" would change that — there's no version to update
  *to*. Combined with the profiling data (CPU-side recording is nowhere near
  frame-budget-bound even under load), this is a closed line of inquiry, not
  a pending one.

Net: Vulkan's remaining headroom is bounded by scenefx's own fx_vk
maintenance from here on, not by anything a wlroots bump would unlock. This
is consistent with config B being the experimental path (see the top of this
doc) — GLES stays the daily driver.

## wlroots version notes
- Config A/C: wlroots **0.20**. Config B (`fx_vk`) was originally re-vendored from
  wlroots **0.21-dev** during initial development, requiring both scenefx's
  `build-vulkan` and asteroidz to build against `wlroots-0.21` (the only API delta
  being `wlr_xdg_decoration_manager_v1_create` gaining a 2nd (version) arg in 0.21).
- **Update (2026-07-17):** this is no longer accurate. The live daily-driver session
  now runs config B against the stock **wlroots0.20.2** package — asteroidz's
  `wlr_xdg_decoration_manager_v1_create(dpy)` call site is still the 1-arg 0.20
  signature — and `fx_vk` initializes and renders correctly (confirmed via the
  running compositor's own log: `render/fx_renderer/vulkan/renderer.c` init
  messages, `fx_vk: compute dual-Kawase blur enabled`, etc.). fx_vk is fully
  vendored/renamed (no symbol clash with libwlroots' own unused Vulkan renderer),
  so it never actually needed 0.21 as an ongoing build dependency — only
  transiently, to source the initial code snapshot.

## TODO to make Vulkan a real daily driver
1. Port scenefx effect shaders to `fx_vk` (SPIR-V): rounded corners → box shadow →
   blur → gradients → color LUT. **Rounded corners DONE (2.1), box shadow DONE
   (2.2), blur DONE (2.3), 2-stop gradient DONE (scenefx `f57fe75`, verified).**
   Only remaining: colour-LUT effect (not used by asteroidz) + blur polish
   (blur_strength + ignore_transparent now DONE; >2-stop gradients still fall
   back to first-stop). Electron/native-Wayland dmabuf import is a separate
   known Vulkan limitation (see step 2.3 notes).
2. ~~Port the SDR color pipeline to `fx_vk`~~ — NOT needed; SDR (reference
   luminance + saturation) already applies via the renderer-agnostic output
   colour-transform matrix (see root cause #4). Verified matching config A.
3. ~~Revisit the full-damage workaround once partial damage is correct.~~ **DONE
   (scenefx `4d3711e`, 2026-07-14).** The vk path was assumed to render
   full-damage and skipped the GLES blur damage compensation; it does not — a
   live blur node samples the two-pass blend image, which retains previous
   frames' content where the current frame has no damage, so it could blur its
   own stale content back into itself. Frame damage is now expanded by each
   live blur node's padded sample region when the node will re-render.
4. **HDR10 on config B — in progress, not yet declared verified end-to-end.**
   Real work has landed since this was last "no progress-log entry": PQ decode
   clamped to its valid codeword range to stop NaN propagation on extreme
   un-premultiplied edges (scenefx `545681d`), luminance-neutral compositing
   for PQ-tagged buffers so composited HDR10 no longer reads 38% brighter than
   direct scanout (scenefx `dfcfe43`), and HDR-safe (multiplicative, black-pinned)
   blur brightness/contrast (scenefx `964fb07`, `0dc168f`). This is real,
   verified-in-isolation progress on specific correctness bugs, not a
   confirmed "10-bit output + HDR metadata + PQ tone mapping all correct on
   the real DP-1 HDR10 monitor" sign-off — that end-to-end verification still
   hasn't happened. Still the next big item, just further along than it looks.
   (The old item here — border lower-to-bottom workaround — was already
   resolved and reverted; see root cause #3.)

## Perf & scheduling backlog (from the 2026-07-11 threading/latency review)
asteroidz is single-threaded by design (standard wlroots model); threaded
command recording was explicitly ruled out (frames aren't CPU-record-bound and
it fights wlroots). The worthwhile items are latency/GPU-efficiency/robustness:

| # | Item | Status |
|---|------|--------|
| 1 | Render-late scheduling (adaptive input-to-photon latency cut) | **DONE** — asteroidz `4d9aeea` + `cc1ade4`; validate vblank behaviour on real HW |
| 2 | Verify explicit sync is stall-free end-to-end (no main-thread fence waits between submit and KMS commit; out-fences/syncobj timelines used) | **DONE (code audit, 2026-07-11)** — stall-free on this stack; see progress log |
| 3 | Persist `VkPipelineCache` to disk (first-use hitch) | **DONE** — scenefx `2c5a4ef` |
| 4 | Staging discipline: no per-frame staging allocs; SHM uploads via persistent ring, damage-region-only | open |
| 5 | Scope overview blur cost (`should_only_blur_bottom_layer(false)` forces multi-layer blur per OV window; grew in the 2026-07-11 OV work) | open |
| 6 | Damage-scope the overview fade; spot-check `need_more_frames` drops at fade end | open |
| 7 | `VK_ERROR_DEVICE_LOST` recovery (renderer analogue of KMS `monitor_start_retrain`) | open |
| 8 | Command/descriptor pool reuse (reset pools, no per-frame `vkAllocate*`) | open |

## Progress log

### Explicit-sync stall audit — backlog #2 (2026-07-11, code inspection)
Question: does anything on the frame path CPU-block between `vkQueueSubmit` and
the KMS commit, or is it out-fences/timelines end-to-end? **Answer: stall-free
on this stack.** The full chain, with file/line receipts:

- **Render → KMS (out-fence):** `wlr_scene_output_create` creates
  `in_timeline`/`out_timeline` iff `backend->features.timeline &&
  renderer->features.timeline` (scenefx `wlr_scene.c:2735`). fx_vk sets
  `features.timeline = sync_file_import_export && DRM_CAP_SYNCOBJ_TIMELINE`
  (`renderer.c:3289`) — true on RADV + amdgpu. Every frame the scene passes
  `.signal_timeline = in_timeline` to `wlr_renderer_begin_buffer_pass`
  (`wlr_scene.c:3675`). `render_pass_submit` signals an exportable binary
  semaphore in the same `vkQueueSubmit2` (`pass.c:591-613`), and
  `fx_vulkan_sync_render_pass_release` exports it as a sync_file and imports it
  into that timeline (`renderer.c:1356`) — **no CPU wait**. The scene commit
  hands the timeline to the output as wait/signal state (`wlr_scene.c:3132-3140`);
  the DRM backend converts it to `IN_FENCE_FD`. GPU→KMS handoff is fence-based.
- **Client → render (in-fence):** client explicit-sync `wait_timeline`s are
  exported as sync_files and attached as **GPU-side wait semaphores** in the
  render submit (`pass.c:442-470`); implicit-sync clients go through
  `sync_foreign_texture_acquire` (dmabuf fence export) the same way. No CPU wait.
- **The blocking fallback** (`renderer.c:1334`, "no choice but to block") fires
  only when `!implicit_sync_interop && signal_timeline == NULL`. On this box
  both escape hatches are open: `implicit_sync_interop` requires semaphore
  sync_file import+export + dmabuf sync_file ioctls (`vulkan.c:537`) — all
  supported by RADV/amdgpu — and `signal_timeline` is provided anyway
  (timeline feature true). Double-covered.
- **SHM texture uploads** are recorded into the stage command buffer and
  submitted **together with the render** in one `vkQueueSubmit2`
  (`pass.c:630`) — asynchronous. (Per-frame staging *allocation* discipline is
  a separate item, backlog #4.)
- **Genuine CPU waits found — all off the frame path:**
  `fx_vulkan_read_pixels` → `submit_stage_wait` (screenshots/screencopy,
  inherently synchronous); `get_command_buffer` blocks only when all **64**
  command buffers are in flight (unreachable backpressure bound);
  `destroy_render_buffer` → `vkQueueWaitIdle` (output unplug/mode change,
  upstream has the same TODO); `vkDeviceWaitIdle` on renderer destroy.

Caveat: this is a code audit. Runtime confirmation = run the Vulkan session
with debug logging and check for `Implicit sync interop supported`
(`vulkan.c:540`, WLR_DEBUG level); the compositor's default log level (ERROR)
hides it. If that line ever says "falling back to blocking", re-open this item.

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
  - Polish DONE: `blur_strength` partial mixing (scenefx `002779c` — unfocused
    windows re-blur `optimized_no_blur` at reduced strength via a per-node scene
    split) and `ignore_transparent` transparency mask (scenefx `5a1001b` — a
    2-descriptor-set masked-texture pipeline zeroes blur where the mask surface
    alpha < threshold; the ignore_transparent path is runtime-untested but
    isolated). Still simplified: blur only on the two-pass path; >2-stop
    gradients fall back to first-stop. None block the daily-driver look.
  - **LIVE blur fix (scenefx `b2319cb`, 2026-07-12):** `add_blur` composited the
    optimized bottom-layer cache for ALL nodes, so live nodes
    (`should_only_blur_bottom_layer == false` — layer-shell panels/popups,
    overview windows) blurred the raw WALLPAPER instead of the content beneath
    them: dim scrims/windows were missing from the blur, showing as the DMS
    spotlight's region glowing undimmed at its 1px edges over dimmed screens
    (desktop DMS-dim and the overview's ov_dim). Live nodes now split the
    scene pass and blur the current scene image (same machinery as
    add_optimized_blur). Verified headless (fake-HDR output to force two-pass):
    seam gone, blur includes the dim.
  - **Window-open flicker fix (asteroidz `a5fc7ca9`):** the open animation's
    `set_strength(blur_p<1)` triggered the per-frame re-blur split, adding
    latency and a lingering blurred rectangle before content. Fixed by holding
    `strength = 1` during the open (fade only alpha); the blur node stays
    enabled so steady-state blur behind translucent windows is preserved. (An
    earlier enable/disable approach regressed that steady-state blur and was
    superseded.)
  - **Known limitation — electron/native-Wayland windows blank on fx_vk.** Not
    blur-related (persists with blur off; XWayland works). Their GPU dmabuf uses
    a format/modifier the Vulkan renderer can't import
    (`fx_vulkan_import_dmabuf` -> "can't be used with modifier"): the same
    fewer-importable-formats limitation as the other Vulkan root causes. Known
    Linux/Vulkan issue, deprioritized.

### 2-stop gradient border on `fx_vk` (scenefx `f57fe75`, verified)
- New `quad_grad_round.frag`: ports GLES `gradient.frag` + `quad_grad_round.frag`
  (linear/conic + blend) × rounded-corner/clip coverage. Uses the `box_pos`
  varying, not `gl_FragCoord`, so FLIPPED_180 doesn't invert the gradient.
- Fits the shared frag push range (80..224): params @80, corner block @128,
  2 colour stops @192 (linear+premultiplied on the CPU) — no new pipeline layout.
- `fx_vk_render_pass_add_rounded_rect_grad` uses it for `count == 2` (the
  focused-border focus→tertiary gradient); other counts keep the first-stop
  solid fallback. Verified: focused border shows the 2-tone gradient, correct
  ring shape.

### Border/pill colour sharing (DMS, not renderer)
- Window borders now share the DMS pill palette: matugen template
  `asteroidz-colors.kdl` maps the resting border `color` to
  `surface_container_high` (= pill `bg-color`) and `focus-color` to `primary`
  (= pill `focus-bg-color`). Border `width` knob added to `config.kdl`
  (`layout/border/width`); colours stay in the generated `dms/colors.kdl`.

### Shader enhancement pass (scenefx `744fade`, 2026-07-12, verified)
Eight coordinated fx_vk improvements, merged to scenefx main, packaged as
`asteroidz-scenefx 0.5.0-5`:
- **Compute dual-Kawase blur** (`blur1/2.comp`): the blur ping-pong runs as
  compute dispatches (no render-pass begin/end per Kawase level) when the
  queue family has compute and 16F supports storage images. Effect images
  gained STORAGE usage + compute-visible sampled/storage descriptor sets;
  the graphics path stays as the fallback and is pixel-equivalent (verified
  via SPIR-V offset dump + RMSE compare). Look for
  `fx_vk: compute dual-Kawase blur enabled` in the log.
- **Blur post effects folded into the final upsample** (graphics AND compute)
  — the separate `blur_effects` fullscreen pass is gone; effects run in
  perceptual (gamma 2.2) space on unpremultiplied rgb because the legacy
  matrix controls assume gamma-encoded values (0.5 mid-grey pivot) — applied
  in linear they crush shadows. NOT for GLES parity: per the 2026-07-12
  directive, **GLES look parity is a non-goal** — fx_vk decisions optimize
  Vulkan quality/performance on their own merits and the renderers may
  legitimately diverge visually.
- **fwidth() SDF anti-aliasing** in the 5 rounded/shadow frag shaders.
- **IGN dithering** in the output pass at one quantum of the target encoding
  (1/255, 1/1023; none for 16F/16-bit) — kills dark-gradient banding.
- **Tetrahedral 3D-LUT interpolation** in the output pass. Gotcha caught in
  review: the sampler coord (texel centers) maps to lattice space as
  `pos * n - 0.5`, NOT `pos * (n-1)` — the latter shifts black/white by half
  a LUT cell.
- **Push-budget hardening**: `frag_push_end` (shared-layout end, capped 224)
  + `max_push_size` (raw device limit) gate every effect draw with graceful
  degradation; shared layout clamps with a 152-byte floor. Gotcha: the
  244-byte masked-blur layout must check `max_push_size`, not the capped
  `frag_push_end`, or it self-disables on every device (and anything
  init'd after it never runs).
- **Box shadow batching**: clip ∩ shadow box up front — one draw for a fully
  visible shadow instead of one per damage rect on the whole output.
- Compute-barrier note: WAW hazards need the write access in BOTH scopes
  (dst needs `SHADER_WRITE`/`TRANSFER_WRITE` too, not just READ).
- Verified: glslang + spirv-val on all 14 shaders; headless 4K fake-HDR
  two-pass run under full validation layers (optimized + live + masked blur,
  shadows, overview) — zero VUIDs. Pre-existing, unrelated:
  `wlr_ext_image_copy_capture_v1.c:673` teardown assert fires on compositor
  SIGTERM after a screencopy client ran (also on 0.5.0-4 / old lib).

### Perf series (scenefx `84a9c02`, 2026-07-12, pkgrel 6)
Approved items 1-5, quality/perf-first (GLES parity non-goal):
- **Region-limited live blur**: single-node blurs copy + blur only the node
  box padded by `blur_data_calc_size` — >90% less blur work for
  spotlight-sized nodes at 4K. Optimized cache still full-frame (NULL region).
- **fp16 compute kernels** (`shaderFloat16`, -DUSE_FP16 variants) — log says
  `compute dual-Kawase blur enabled (fp16 kernels)`.
- **Mip-chain compute blur**: single mipped chain image walked in place;
  per-level halfpixel = canonical dual-Kawase; fragment DS mip-0-restricted;
  graphics ping-pong remains the fallback. Gotchas: transition ALL mips at
  creation (change_layout helper covers one level); clamp level rects to
  floor-scaled mip extents (ceil overshoot = UB imageStore).
- **SPD-style single-dispatch chain: evaluated, REJECTED** — Kawase taps
  need cross-tile halos (= the barriers we have); skipping them makes tile
  seams, and post region+mips the barrier cost is negligible.
- **Oklab saturation** in blur effects (linear-space chroma scale; no hue
  shift at saturation > 1). Deliberate visual divergence from GLES.
- Also this date: teardown assert fixed in asteroidz (`3db80ef`) — the
  ext-image-copy-capture new_session listener was never removed.

### Multi-threaded command recording — investigated and closed, again (2026-07-17)
Question: should fx_vk record command buffers on worker threads (Vulkan is
explicitly designed for this — per-thread command pools, single-thread
submit)? Re-opened because "Vulkan can do it" is true and the earlier ruling
undersold *why* that doesn't mean it's worth doing here.

- **wlroots itself has zero threading anywhere** — no `pthread_create`,
  `thrd_create`, or mutexes in the whole tree (core, backend, all three
  renderers), confirmed by direct grep against current upstream git. Not a
  version gap: no GitLab issue/MR/roadmap item about multi-threading the
  render loop or command recording was found anywhere in the wlroots project
  (checked via web research). Not a stated design principle either — it's an
  emergent consequence of every core structure (`wlr_output`, `wlr_buffer`,
  `wlr_scene`) assuming unsynchronized single-threaded access. Precedent:
  KWin's own developers flag this as an *unsolved* problem they want but
  haven't built, not something anyone has shipped.
- **Measured it instead of assuming.** Added temporary profiling
  (`ASTEROIDZ_VK_PROFILE=1`, since reverted) timing from
  `fx_vulkan_begin_render_pass` to right before `vkQueueSubmit2KHR` — i.e.
  actual CPU-side recording cost, excluding the async/fence-based submit.
  Headless, full effects (blur+shadow+rounded corners), against
  `LD_LIBRARY_PATH`-loaded local builds so the live session was never
  touched:
  - 4 windows, tiled, 4K: avg 87-181us, max 110-218us.
  - 12 windows, scroller, 4K: avg 127-132us, max 147-286us (barely moved
    despite 3x the windows — cost isn't scene-complexity-bound here).
  - Against a 60Hz budget (16,667us) that's under 1.1%; against the real
    monitor's 144Hz budget (6,944us), still only ~2-4% at the high end.
- **Conclusion unchanged, now with receipts:** frames genuinely aren't
  CPU-record-bound on this workload. Multi-threading would shave low-single-
  digit microseconds off a multi-millisecond budget, in exchange for
  building and maintaining an unsupported-by-upstream locking layer against
  wlroots' entire object model. Not worth it. Closing this for real this
  time — re-open only if a profile on a *meaningfully* heavier scene (many
  more live effect nodes, higher resolution/refresh) ever shows recording
  time actually competing for frame budget.
- **Side effect of this audit:** discovered this doc had gone stale relative
  to actual progress — see the wlroots-version-notes update above (config B
  no longer needs wlroots 0.21, confirmed via the live session already
  running it against 0.20.2) and TODO items 3-4 above (damage-tracking fix
  landed `4d3711e`; HDR10 correctness work landed `545681d`/`dfcfe43`/
  `964fb07`/`0dc168f`, though not yet declared end-to-end verified).

### GLES parity port (scenefx `06efd65`/`d2915fd`, 2026-07-17, pkgrel 22)
Checked whether GLES needed the same fixes that had landed on `fx_vk` over
the preceding days, since GLES is the daily driver and had quietly fallen
behind.

- **PQ-decode NaN clamp (`545681d`) — investigated, does NOT apply to
  GLES.** Confirmed three ways: no `pq_color_to_linear`-equivalent anywhere
  in GLES's texture shaders; no `color_encoding`/`transfer_function`/
  `WLR_COLOR_TRANSFER` reference anywhere in GLES's renderer C code; the
  shared `wlr_render_texture_options.transfer_function`/`.primaries` fields
  are only ever read in `render/fx_renderer/vulkan/pass.c`. GLES applies its
  whole HDR transform as one fullscreen 3D-LUT pass over the
  already-composited image, never decoding a client's PQ tag per-texture —
  architecturally immune to this specific failure mode. **But this also
  means GLES cannot correctly composite a real HDR10-tagged client buffer**
  (an actual HDR video/game) — only apply a boosted-brightness/BT.2020+PQ
  signal to standard SDR-authored desktop content. A real, structural
  GLES/Vulkan capability gap, not a bug — this is the concrete reason the
  Vulkan effort exists at all, made precise instead of hand-wavy.
- **Blur brightness/contrast/noise HDR-safety (`964fb07`/`0dc168f`) — DID
  apply, ported.** `blur_effects.frag` still had the pre-fix affine
  `brightnessMatrix()`/`contrastMatrix()` (constant offset that lifts/
  crushes black — invisible in 8-bit SDR, visible as a glow/dark band under
  PQ). GLES doesn't decode per-client PQ, but its *output* still goes
  through PQ encoding via the LUT when HDR is active, so the same bug
  applied to GLES's own blur math. Replaced with the same power-law
  contrast / pure-gain brightness / ratio-scaled noise form.
- **Oklab saturation (`4df3ff6`) — ported**, wrapped in a gamma<->linear
  round-trip Vulkan's version doesn't need (GLES's blur buffer is a plain
  8-bit gamma-encoded RGBA — `fx_framebuffer.c` — unlike Vulkan's linear 16F
  working buffer).
- **fwidth()-scaled corner AA (`744fade`) — ported** to GLES's shared
  `corner_alpha.frag` (used by rounded rects/textures/gradients and the
  box-shadow clip cutout) in one place, vs. five separate Vulkan shader
  files, since GLES already shares this via runtime string concatenation.
- **Tetrahedral 3D-LUT + IGN dither (`744fade`) — ported** to
  `color_transform.frag`, the highest-risk change (the universal per-frame
  output-compositing pass). GLSL ES 1.00 (what these shaders compile as —
  no `#version` pragma anywhere) has no `texelFetch`/`textureSize`, so this
  derives the LUT lattice size algebraically from the existing `lut_offset`
  uniform (`n = 0.5/lut_offset`) and does unfiltered lattice reads via
  NEAREST-filtered `texture3D()` at exact texel-center coordinates instead —
  no new uniforms, no C-side struct changes beyond flipping the LUT
  sampler's filter mode from LINEAR to NEAREST (`fx_pass.c`).
- **Caused a real incident:** the first attempt put
  `#extension GL_OES_standard_derivatives : enable` inside `corner_alpha.frag`
  itself. Since that file gets concatenated *after* other shader text at
  several call sites (quad_round, texture_round, gradient_round,
  box_shadow), the directive landed mid-shader — GLSL rejects that
  outright ("#extension directive is not allowed in the middle of a
  shader"), every fragment shader failed to compile, the FX renderer failed
  to initialize, and the live compositor **crashed on restart**, dropping
  to the GDM greeter. Root cause: headless testing can't catch this class
  of bug particularly well, and the change was installed live without a
  live-restart safety net. Fixed (`d2915fd`) by having `compile_shader()`
  unconditionally prepend the extension line as its own leading string via
  `glShaderSource`'s multi-string support (GLSL treats it as plain
  concatenation) for every fragment shader — harmless for shaders that
  don't use it, and immune to concatenation-order issues by construction.
  Verified via a fresh headless run (zero shader/link errors) before
  reinstalling, then confirmed live: compositor starts cleanly, HDR still
  negotiates on the real DP-1 monitor, blur/corner/shadow rendering visibly
  correct (soft blended tones under a translucent window, not flat/crushed
  black).
- Verified overall: headless render-matrix harness (GLES2, effects on, SDR
  + fake-HDR two-pass) clean both before and after the incident/fix; live
  HDR10 negotiation and visual blur/AA output confirmed after reinstall.

### frog-color-management-v1 on GLES + HDR10 metadata gap-closing (asteroidz `af8d94d`/`da6214c`/`6ed1913`, scenefx `81bc0fd`/`764a861`/`f6dd7f6`, 2026-07-18/19)
Vulkan already had gamescope HDR passthrough via `frog-color-management-v1`
(gamescope can't use `wp_color_management_v1` — needs six manager features,
wlroots implements two); GLES didn't. Closed that gap, then found and closed
two further gaps in the result.

- **Per-surface source color management, GLES (`81bc0fd`).** `tex.frag`
  gained `apply_source_color_management()`: decode the surface's declared
  transfer function (SRGB/PQ/GAMMA22/BT1886) to linear, apply the
  luminance-scale + primaries matrix `wlr_scene` already computed (the same
  values Vulkan's `texture.frag` uses), re-encode to gamma 2.2 so it blends
  consistently with this single-pass pipeline's other content. Narrower
  than Vulkan's linear-intermediate-buffer architecture by design — decode
  + re-encode per surface, not a whole-scene linear compositing pass.
  **Caused a real incident before ever shipping:** grew `tex.frag` past
  `link_tex_program`'s fixed 4096/8192-byte stack buffers, `snprintf`
  silently truncated it mid-shader, and the GLES compiler error ("syntax
  error, unexpected end of file") gave zero indication it was a buffer-size
  issue rather than a real shader bug. Fixed (`764a861`) with real headroom
  (16384/24576), not just resized to fit exactly. Caught before shipping,
  unlike the concatenation-order incident above.
- **Verified against real gamescope traffic**, not just headless: temporary
  debug logging in `frog-color-management.h` confirmed a live gamescope/
  ASKA HDR session actually declares `tf_named=ST2084_PQ`,
  `primaries_named=BT2020` every frame and the GLES path processes it
  (`active=1`) — this genuinely exercises real content, not dead code.
  Logging reverted after verification (exact inverse diff).
- **HDR10 static metadata (MaxCLL/MaxFALL/mastering luminance) was parsed
  but never consumed anywhere** — traced through `scenefx`'s `surface.c`,
  which only ever read `tf_named`/`primaries_named` off the parsed
  `wlr_image_description_v1_data`. Two fixes, both compositor-side (no
  system wlroots changes — the KMS blob is built in `mon_state_apply_color`,
  and `fx_render_texture_options` is scenefx's own wrapper, not wlroots'):
  - `mon_hdr_scanout_candidate()` (asteroidz `af8d94d`) forwards the sole
    fullscreen client's own declared metadata into the real KMS
    `HDR_OUTPUT_METADATA` blob instead of always synthesizing from the
    display's own configured ceiling. Checks real `wp_color_management_v1`
    data before frog (`da6214c`) — the original version only checked frog,
    silently missing any HDR client that isn't gamescope.
  - scenefx's GLES `tex.frag` (`f6dd7f6`) replaced a hard clip to 1.0 above
    SDR-reference with a per-channel Extended-Reinhard rolloff
    (`content_peak` uniform = `max_cll / src_lum.reference`, or the transfer
    function's own absolute peak when no MaxCLL was declared — a strict
    improvement, never a behavior change, for surfaces that don't declare
    one). Both are no-ops for direct-scanout content (gamescope/ASKA
    declares `max_cll=0`, and is scanned out directly anyway); verified with
    `haasn/hdr-tests` (a small curated HDR clip set from mpv/libplacebo's
    own color-management maintainer) played windowed (forced out of
    fullscreen so it hits the composited path) via
    `mpv --target-colorspace-hint-mode=source toobright.hevc` (real
    MaxCLL=1000/MaxFALL=400), user-confirmed reasonable-looking live.
- **Vulkan checked, found NOT to need the same rolloff fix.** A `clamp(color`
  grep hit in Vulkan's `texture.frag` initially looked like the same bug,
  but it's `pq_color_to_linear`'s *input* codeword safety clamp (NaN
  avoidance before EOTF decode), not a post-tonemap clip — Vulkan's
  `texture.frag` has zero clamping after `luminance_multiplier`, consistent
  with its whole-scene-linear-then-single-output-encode architecture. The
  only output-side clamp (`output.frag`'s `linear_color_to_pq`) clips to
  PQ's own 10000-nit ceiling, which is correct/unavoidable, not the
  ~203-nit SDR-reference mistake GLES had.
- Added `misc.frog-color-management` config flag (`6ed1913`, default on) to
  disable gamescope HDR passthrough entirely if wanted.
- Verified: full headless regression suite (118 assertions) clean
  throughout; render-matrix harness (GLES2 + Vulkan, effects on/off, SDR +
  HDR) clean.
