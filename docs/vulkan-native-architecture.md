# A Vulkan-native rendering architecture for asteroidz

This is the design document for making Vulkan the *canonical* rendering
architecture of asteroidz rather than an alternate renderer squeezed through an
abstraction that was designed around GLES.

`docs/vulkan-journey.md` is the historical record of the fx_vk effort. This
document supersedes its architectural conclusions. Where the two disagree, the
code was consulted and this document says so explicitly.

**Method.** Every claim below was checked against the tree at
`697e5b9`, not taken from the journey doc. File:line receipts are given so the
next reader can re-check them instead of trusting either document.

---

## 1. The current path, as it actually is

```
Wayland client
  │
  │  wl_buffer  (wl_shm | zwp_linux_dmabuf_v1 v5)
  ▼
wlr_buffer  ─────────────────────────────────────────────┐
  │                                                      │
  │  wlr_client_buffer / wlr_texture_from_buffer         │ import
  ▼                                                      │
wlr_texture  ◄── fx_vk_texture ◄── fx_vulkan_import_dmabuf
  │                                    (texture.c:504)
  ▼
wlr_scene node tree  (scenefx types/scene/wlr_scene.c, 4322 lines)
  │      asteroidz builds this: layers[NUM_LAYERS], per-client
  │      scene trees, shadow trees, blur nodes, text nodes, ufo nodes
  ▼
wlr_scene_output_build_state()          wlr_scene.c:3667
  │      damage accumulation, direct-scanout attempt, node culling
  │
  ├─► wlr_renderer_begin_buffer_pass()  wlr_scene.c:3913   ◄══ THE SEAM
  │        │
  │        ▼
  │   fx_render_pass (scenefx)  ── or ──  fx_vk_render_pass
  │        │   fx_pass.c 1876 ln              pass.c 3418 ln
  │        │   GLES2 + EGL                    Vulkan
  │        ▼
  │   wlr_render_pass_add_texture / _add_rect  (the wlroots vocabulary)
  │   fx_render_pass_add_blur / _box_shadow / _rounded_rect (the scenefx
  │   vocabulary, only reachable by downcasting the wlr_render_pass)
  │        │
  │        ▼
  │   GPU work → output buffer (wlr_swapchain, wlr_allocator/gbm)
  │
  ▼
wlr_output_state { .buffer, .signal_timeline, HDR image_description }
  │
  ▼
wlroots DRM backend → atomic KMS commit → connector
```

Asteroidz's own coupling to the renderer is **thin**. The entire surface is:

| Site | File:line | What it does |
|---|---|---|
| `drw` | `src/asteroidz.c:1405` | the one `struct wlr_renderer *` |
| `alloc` | `src/asteroidz.c:1406` | the one `struct wlr_allocator *` |
| create | `src/asteroidz.c:8783` | `fx_renderer_create(backend)` |
| shm | `src/asteroidz.c:8794` | `wlr_renderer_init_wl_shm` |
| dmabuf | `src/asteroidz.c:8796-8800` | `wlr_drm_create` + `wlr_linux_dmabuf_v1_create_with_renderer(dpy, 5, drw)` |
| syncobj | `src/asteroidz.c:8802-8804` | gated on `drw->features.timeline` |
| allocator | `src/asteroidz.c:8807` | `wlr_allocator_autocreate(backend, drw)` |
| compositor | `src/asteroidz.c:8814` | `wlr_compositor_create(dpy, 6, drw)` |
| device loss | `src/asteroidz.c:2164-2188` | `gpureset()` — recreates renderer + allocator wholesale |
| frame | `src/asteroidz.c:7724` | `wlr_scene_output_commit` |
| frame (capture) | `src/asteroidz.c:7654` | `wlr_scene_output_build_state` |
| frame (HDR) | `src/asteroidz.c:7707` | `wlr_scene_output_build_state` |
| frame (tearing) | `src/ext-protocol/tearing.h:101` | `custom_wlr_scene_output_commit` |
| readback | `src/dispatch/bind_define.h:3137,3236` | `wlr_texture_from_buffer` + `wlr_texture_read_pixels` |

That is the *whole* renderer dependency of a 10,535-line compositor. The GPU
architecture does not live in asteroidz; it lives behind
`wlr_scene_output_build_state`. **This is very good news for the migration** and
it dictates the strategy in §4: we do not need to rewrite asteroidz. We need to
own what happens on the other side of one function call.

---

## 2. Verified facts (and corrections to `vulkan-journey.md`)

### 2.1 The `DRM_FORMAT_MOD_INVALID` import failure — CONFIRMED, and the mechanism is exact

The journey doc says "wlroots' Vulkan renderer cannot import implicit-modifier
DMABUFs". That is true, and the mechanism is narrower and more fixable than the
prose suggests:

- `fx_vulkan_import_dmabuf()` (`vulkan/texture.c:504`) resolves the buffer's
  modifier through `fx_vulkan_format_props_find_modifier()`
  (`vulkan/pixel_format.c:612`), which is a **linear scan for exact equality**
  against the modifier list.
- That list is built by querying `VK_EXT_image_drm_format_modifier`, which
  enumerates only *real* modifiers. `DRM_FORMAT_MOD_INVALID` (0x00ff…ff) is by
  construction never in it.
- So the lookup returns `NULL`, the import returns `VK_NULL_HANDLE`
  (`texture.c:534`), texture creation fails, and the client renders nothing.

The failure is therefore not "Vulkan cannot import implicit buffers". It is
"this importer has no path for a buffer whose modifier it was not told". Vulkan
itself has three usable answers (§5.2) and fx_vk implements none of them. This
is an **asteroidz/scenefx implementation limitation**, not a Vulkan limitation
and not a wlroots-upstream bug a version bump would fix.

The GLES path survives because EGL's `EGL_EXT_image_dma_buf_import` accepts a
buffer with no modifier attributes at all and lets the driver recover the layout
(`render/egl.c:745,803`). That is a *driver-side* implicit-modifier path, which
is exactly what §5.2 option 2 reconstructs without EGL.

### 2.2 The renderer seam is one function, not a diffuse dependency

`wlr_scene_output_build_state()` (`wlr_scene.c:3667`) is the only place the
scene enters the renderer, at `wlr_scene.c:3913`. Everything after that call —
node iteration, `wlr_render_pass_add_texture`, damage handling, the fx downcast
for blur/shadow — happens inside that one function's dynamic extent.

**Consequence for M3:** an Asteroidz-native submission path does not require
forking `wlr_scene`. It requires an alternative to *one* function, taking the
same inputs (`wlr_scene_output`, `wlr_output_state`) and producing the same
output (a committed buffer + damage + timeline). The scene graph can remain as
window-management bookkeeping while ceasing to be the renderer's instruction
format.

### 2.3 fx_vk is 9,200 lines of vendored wlroots, not a small shim

`vulkan/renderer.c` 3,904 · `vulkan/pass.c` 3,418 · `vulkan/texture.c` 848 ·
`vulkan/vulkan.c` 747 · `vulkan/pixel_format.c` 634, plus blur_debug, dmabuf_sync,
rect_union, util. It is a fork of wlroots `render/vulkan/` with every symbol
renamed. It has no upstream to track. **Nothing about upgrading wlroots
improves it** — the journey doc is right about this, and it is the strongest
single argument for owning the code rather than continuing to patch a fork.

### 2.4 The hardware this must work on

Two RADV devices, which is why the multi-GPU section is not hypothetical:

| | Device | DRM node | Role |
|---|---|---|---|
| GPU0 | AMD Radeon RX 7900 XT (RADV NAVI31), `0x1002:0x744c`, discrete | `renderD128` | display + primary render |
| GPU1 | Ryzen 7 7800X3D iGPU (RADV RAPHAEL_MENDOCINO), `0x1002:0x164e` | `renderD129` | secondary |

Vulkan 1.4.357 loader, Mesa 26.1.6, `VK_LAYER_KHRONOS_validation` present,
`glslc` and `glslangValidator` present. The instance advertises
`VK_EXT_acquire_drm_display`, `VK_EXT_debug_utils`, `VK_KHR_display`.

A client that renders on GPU1 and presents to GPU0 hands the compositor a buffer
the compositor's device may not be able to import — which is the *other* half of
the blank-window class, and the reason §5.3 is a first-class requirement rather
than an optimisation.

### 2.5 Where the journey doc is stale

- "**HDR10 not yet verified end-to-end**" — still true. Nothing in this tree
  verifies the chain past the shader. Keep it open.
- "**>2-stop gradients fall back to the first stop**" — still true, and it is
  a push-constant-budget artefact of reusing wlroots' pipeline layout, not
  anything intrinsic. It disappears with our own descriptor model (§5.5).
- "**Multi-threading is closed because wlroots has no threading**" — the
  *conclusion* (don't thread yet) is right; the *reason* is now wrong. Once we
  own submission, wlroots' single-threadedness stops being the constraint. The
  correct reason is "we are not CPU-record-bound; measure before threading",
  which is what §9 sets up.
- "**Config C dropped**" — accurate, and there is no `nofx.h` in the tree.

---

## 3. Dependency inventory: KEEP / WRAP / REPLACE / REMOVE

The rule: *wlroots may provide services to asteroidz; wlroots may not dictate
asteroidz's GPU architecture.*

### Rendering and GPU (the target of this work)

| Dependency | Class | Rationale |
|---|---|---|
| `wlr_renderer` (as asteroidz's renderer) | **REPLACE** | The abstraction is GLES-shaped: a texture is opaque, a pass is a list of rects and textures, there is no render graph, no explicit image state, no way to express an HDR intermediate. |
| `wlr_render_pass` (normal composition) | **REPLACE** | Composition must not be expressible only as `add_texture`/`add_rect`. Replaced by the avk render graph (§5.5). |
| `wlr_texture` | **REPLACE** | Becomes `avk_client_buffer` + `avk_image`, which know their modifier, plane layout, colour state and GPU timeline. |
| fx_vk (`vulkan/*.c`, 9.2 kloc) | **REMOVE** at M9 | Vendored fork with no upstream. Its useful knowledge is ported, not kept. |
| SceneFX GLES renderer (`fx_pass.c`, shaders) | **KEEP** during migration, demote at M10 | The known-good recovery path. It is what the desktop falls back to when the Vulkan path breaks, and it stays buildable. |
| EGL / GLES / `render/egl.c` | **REMOVE from the Vulkan path** | Must not be linked into the Vulkan renderer at all; `ldd` and the Meson dep graph must show it (§8). |
| wlroots DMA-BUF importer | **REPLACE** | §5.2. The one that produces blank Electron windows. |
| `wlr_allocator` (output buffers) | **REPLACE** eventually, **WRAP** first | We must control format/modifier choice for 10-bit and scanout-compatible allocation. Wrapped at M1-M5, replaced at M6. |
| `wlr_color_transform` | **REPLACE** | A 3x3 matrix + optional LUT cannot express PQ/HLG, tone mapping, gamut mapping and an ICC pipeline. §5.6. |
| wlroots output swapchain | **WRAP** | Behind `avk_output`; replaced only if it blocks a required format/modifier. Do not rewrite speculatively. |

### Presentation

| Dependency | Class | Rationale |
|---|---|---|
| wlroots DRM/KMS backend | **WRAP** | It already does atomic modeset, `IN_FENCE_FD`, out-fences, VRR, tearing, HDR metadata, hotplug. Wrap behind `avk_output`; replace only the parts that demonstrably block us. Rewriting KMS speculatively is how this project dies. |
| Direct scanout (`wlr_scene`) | **WRAP** then **REPLACE** | Eligibility must consider effects, capture, privacy shield, colour state — things `wlr_scene` cannot see. |
| asteroidz monitor retrain (`monitor_start_retrain`) | **KEEP** | Hard-won display resilience, orthogonal to the renderer. Must survive untouched. |
| Render-late scheduling | **KEEP** | Already done and validated. A regression here is a project failure. |

### Everything else

| Dependency | Class |
|---|---|
| Wayland protocol impl, xdg-shell, layer-shell, seat/input, libinput, XWayland, protocol bookkeeping | **KEEP** — mature, does not touch GPU architecture |
| `wlr_scene` as window-management bookkeeping and hit-testing | **KEEP** (demoted) — it stops being the renderer's instruction format at M3, it does not stop being the scene |
| `wlr_scene` as the renderer's instruction format | **REPLACE** — §5.4 |
| SHM upload path | **REPLACE** — persistent staging ring, damage-region-only (§5.2) |
| screencopy / `ext-image-copy-capture` protocol plumbing | **KEEP**; readback **REPLACE** (§5.7) |
| privacy shield / `shield_when_capture` | **KEEP** — compositor policy, renderer-independent |
| tags, layouts, scroller, overview, scratchpads, swallowing, animations, rules, IPC, hot reload, restart, GlobalShortcuts | **KEEP, UNTOUCHED** — this is a renderer migration |

---

## 4. Strategy: own the seam, not the compositor

From §1 and §2.2 the migration has an unusually clean shape:

1. Build a complete, independent Vulkan engine (`src/render/vulkan/`) that knows
   nothing about wlroots' renderer abstraction. It can be developed and tested
   **headlessly, without a compositor**, because its inputs are DRM FDs, dmabuf
   FDs and pixel data — not wlroots objects.
2. Introduce an Asteroidz scene *snapshot* (§5.4): a flat, explicit command list
   produced from the scene graph.
3. Replace exactly one call — `wlr_scene_output_build_state` — with an asteroidz
   implementation that walks the scene, emits the snapshot, and hands it to the
   engine.
4. Keep GLES buildable and selectable the whole way, so there is always a
   working desktop.

No step removes tested compositor infrastructure. The compositor's window
management never learns that the renderer changed.

---

## 5. Target architecture

### 5.1 Layout

```
src/render/vulkan/
  device/    avk_instance, avk_device, avk_caps, avk_queue, avk_phys (DRM matching)
  memory/    avk_allocator, avk_staging (persistent ring), avk_budget
  image/     avk_image, avk_image_view, avk_layout (explicit state tracking)
  dmabuf/    avk_dmabuf_import, avk_modifiers, avk_format_table, avk_feedback
  sync/      avk_sync, avk_timeline, avk_fence_fd, avk_syncobj
  command/   avk_command_pool, avk_command_buffer (ring), avk_retire (deferred destroy)
  pipeline/  avk_pipeline, avk_pipeline_cache, avk_descriptor
  shader/    avk_shader, *.comp/*.frag/*.vert + SPIR-V build rules
  scene/     avk_scene_snapshot, avk_draw_cmd
  effects/   rounded, shadow, blur, gradient
  color/     avk_color_pipeline, transfer functions, gamut, tone map, LUT/ICC
  output/    avk_output, avk_frame, avk_present, scanout eligibility
  readback/  avk_readback (capture, screenshots)
  debug/     avk_debug (names, labels, validation), avk_stats (profiling)
```

Hard rule, enforced by Meson (§8): **nothing under `src/render/vulkan/` may
include an EGL, GLES, or `wlr_renderer` header.**

### 5.2 DMA-BUF import (the blank-window fix)

Own importer. Per-plane FDs, offsets, strides; explicit modifiers; disjoint
images; multi-plane; 8/10-bit RGB; useful YUV. Capability table built from
`VK_EXT_image_drm_format_modifier` via
`vkGetPhysicalDeviceFormatProperties2` + `VkDrmFormatModifierPropertiesListEXT`,
with per-modifier `vkGetPhysicalDeviceImageFormatProperties2` probing so we
record *actual* max extents and usage, not assumptions.

For `DRM_FORMAT_MOD_INVALID`, the ordered ladder — **never** "assume LINEAR",
which silently displays corrupt tiled memory as if it were pixels:

1. **Recover the real modifier**, via `gbm_bo_import(GBM_BO_IMPORT_FD)` +
   `gbm_bo_get_modifier()`, and import zero-copy on the normal path.
   **Measured, and it does not work on Mesa.** A buffer allocated with
   `GFX11,256KB_R_X,PIPE_XOR_BITS=5,PACKERS=5` (`0x0200000028A01F04`),
   exported and re-imported through the legacy path, comes back reporting
   `MOD_INVALID`; Mesa's implicit import discards the modifier rather than
   re-deriving it, and no usage-flag combination changes that. (Asking for
   `GBM_BO_USE_SCANOUT` on a render-only buffer makes the import fail
   outright — usage is a layout constraint, not a hint.) So this rung is
   **probed at startup** with a real round-tripped allocation and used only
   where it works. Capability detection, not a vendor branch and not a
   hopeful assumption.
2. **Copy through a driver-detiled mapping.** `gbm_bo_map()` with
   `GBM_BO_TRANSFER_READ` returns a linear view — the driver detiles on the
   way, so the layout never has to be *named* to be read correctly — then a
   staged `vkCmdCopyBufferToImage2` into an OPTIMAL image we own. Costs a
   surface read plus an upload per import. **This is what actually fixes the
   blank window**, and it works regardless of what rung 1 says.
3. **Prevent it at the source.** DMA-BUF feedback tranches computed from what
   *this* importer can actually import, so conforming clients never allocate an
   unimportable buffer in the first place. This is the real fix; 1-2 are for
   clients that ignore feedback.

Every failure logs format, modifier (both as names), plane count, per-plane
size/offset/stride, the target device, and which rungs of the ladder were tried
and why each failed. **A blank window is a bug. A slow window is a fallback.**

Correctness here is asserted on *pixels*, not on the import succeeding:
`tests/test-avk-dmabuf.c` writes a coordinate-derived pattern into a genuinely
tiled buffer, blanks the modifier, imports, reads the `VkImage` back and
compares all 32768 pixels. An import that guessed LINEAR, or that computed its
row pitch in bytes where Vulkan wants pixels, succeeds and returns garbage —
only the readback separates the two.

### 5.3 Multi-GPU

Device identity by `dev_t` of the DRM render node, matched to a Vulkan physical
device via `VK_EXT_physical_device_drm` (`primaryMajor/Minor`,
`renderMajor/Minor`) — not by name, not by index, not by `WLR_DRM_DEVICES`.
Per-device format/modifier capability sets. Feedback tranches per client scope
with a preferred-device tranche. Cross-GPU import where the modifier permits;
explicit cross-GPU copy where it does not. The chosen device and *why* must be
readable in the log.

### 5.4 Scene snapshot

The compositor emits a flat command list; the renderer consumes it. Commands:
`surface`, `texture`, `solid_rect`, `rounded_rect`, `gradient_rect`
(arbitrary stops), `shadow`, `blur_region`, `clip_push/pop`, `transform`,
`opacity`, `colour_state`. Each carries its damage contribution and colour
metadata. This decouples window management from GPU evolution: the scene can
gain an effect without the renderer gaining a downcast.

### 5.4a What M3 actually built

The snapshot is `struct avk_scene`: a flat, immutable array of `avk_cmd`, each
carrying its destination in output pixels, a visible region, an opacity, and
for surfaces an `avk_image` the compositor-side cache has already resolved. It
holds no `wlr_render_pass`, no `wlr_texture` and no scene node — so the
renderer can never walk a live tree while a client commit mutates it.

Two decisions worth recording because they are not obvious:

- **Transforms are folded into a UV origin plus two edge vectors on the CPU**,
  not passed as an enum to be branched on in the shader. All eight Wayland
  transforms, the flips, and a source crop become the same three vec2s, so
  there is one code path instead of eight and the numbers can be read in a
  test. (fx_vk's rounded corners rounded the wrong edges precisely because
  `gl_FragCoord` disagreed with box space under a flipped projection; there is
  no flipped projection here — Vulkan's NDC Y already points down.)
- **`loadOp` is always LOAD, never CLEAR.** A damaged frame redraws part of the
  target and the rest must survive; the background is a normal draw command,
  scissored to damage like everything else. CLEAR would be a full-screen clear
  wearing damage tracking's clothes — correct on a full-damage frame and
  destructive on every other.

Composition is in the surfaces' own 8-bit sRGB-encoded, premultiplied
representation. That is what SDR compositors do today, it makes the tests exact
integers, and M5 will change it to linear FP16 deliberately rather than by
accident.

### 5.5 Render graph

A lightweight pass scheduler over: scene composition → HDR/linear intermediate →
blur input capture → downsample → blur → upsample → shadows → borders/gradients
→ colour transform → tone map → output conversion → capture copies. It tracks
image state, read/write dependencies, usage, damage regions, transient
resources, and emits `synchronization2` barriers from the dependency graph
rather than from hand-placed calls. No `vkCmdNextSubpass` special cases; no
"end the pass early to blur" contortion (the exact problem fx_vk Phase C spent
a restructure on).

### 5.6 Colour

FP16 linear compositing space. Explicit per-surface colour state: primaries,
transfer function, reference white, and whether it is scene-referred or
display-referred. Transfer functions as real code with tests: PQ EOTF/OETF,
HLG OETF/OOTF, sRGB, BT.709, linear. BT.2020↔BT.709 matrices. SDR→HDR and
HDR→HDR composition, tone mapping and gamut mapping as graph passes, 3D LUT and
ICC as a pipeline stage. Configurable SDR reference luminance. 10-bit output
formats. HDR metadata to the connector.

`amsg dispatch hdr_state <output>` must print the whole chain — client image
descriptions in, composite space, transform, framebuffer format, connector
metadata — so "HDR works" becomes a thing you read rather than a thing you
believe.

### 5.7 Sync, resources, readback

Timeline semaphores internally; `synchronization2` everywhere; sync_file
import/export; DRM syncobj; KMS `IN_FENCE_FD` and out-fences. **Zero CPU waits
in the steady-state frame path** — no `vkQueueWaitIdle`, no `vkDeviceWaitIdle`,
`vkWaitForFences` only for genuine ring backpressure. Resource lifetime keyed to
timeline progress via a retire queue. Reused command pools/buffers, reused
descriptor pools, per-frame arenas, a persistent staging ring with
damage-region-only SHM uploads, and a disk pipeline cache. Readback is
GPU copy → staging → fenced completion; capture may block, **the frame path may
not**.

---

## 6. Milestones

Each milestone names the limitation it removes, and each leaves the compositor
usable.

| M | Removes | Ships |
|---|---|---|
| **M1** | asteroidz has no Vulkan of its own; every device decision is wlroots' | `avk_instance`/`avk_device`/caps/queues/command ring/timeline/retire/pipeline cache/debug names. Headless, testable, zero EGL. |
| **M2** | fx_vk cannot import implicit-modifier buffers; SHM re-allocates per frame | Own format/modifier table, dmabuf import with the §5.2 ladder, staging ring, feedback derived from real capability |
| **M3** | composition is expressible only as `wlr_render_pass` | Scene snapshot + native submission; windows and rects render without `wlr_render_pass` |
| **M4** | effects live in a fork; >2 gradient stops unsupported | Rounded corners, arbitrary-stop gradients, shadows, dual-Kawase blur, native |
| **M5** | colour is a 3x3 matrix | FP16 linear pipeline, PQ/HLG, tone/gamut mapping, LUT/ICC, 10-bit output, verified end-to-end |
| **M6** | presentation is wlroots' to decide | Explicit fences, scanout eligibility, VRR, tearing, presentation timing |
| **M7** | unknown client compatibility | The §7 matrix, green |
| **M8** | no device-loss recovery; unmeasured performance | `VK_ERROR_DEVICE_LOST` recovery, profiling facility, measurements |
| **M9** | fx_vk still exists | Delete fx_vk and the Vulkan dependence on SceneFX/wlroots renderers |
| **M10** | GLES defines the architecture | Vulkan default; GLES demoted to recovery renderer |

## 7. Test matrix

Clients: foot, kitty, Firefox, Chromium, Electron, Qt6, GTK3/4, mpv, native
Vulkan apps, OpenGL apps, XWayland, games.
Buffers: SHM, dmabuf explicit modifiers, implicit/legacy, multi-plane, 8-bit,
10-bit, HDR, YUV.
GPUs: RADV (here), ANV, NVIDIA — by capability detection, no vendor branches.

Headless coverage extends `contrib/anim-test.sh`, `contrib/render-matrix-test.sh`
and `contrib/regression/`. New Vulkan-core tests are plain binaries under
`meson test` and need no compositor.

## 8. Build system

`-Drenderers=vulkan,gles2` (default during migration), narrowing to
`-Drenderer=vulkan`. The Vulkan renderer must not pull EGL/GLES: verifiable by
`ldd`, by Meson dependency output, and by a source-include check in CI.

## 9. Risks

- **Scope.** M1-M2 are self-contained. M3 is the one irreversible-feeling step;
  it lands behind a runtime switch with GLES intact.
- **HDR verification needs the real monitor**, and grim on the live HDR output
  forces modesets. Reproduce headlessly; verify on DP-1 deliberately, rarely.
- **Multi-GPU asymmetry** is real hardware here, so it gets tested, not assumed.
- **Do not thread.** Measure first (§9 of the prompt, and the profiling data
  already in the journey doc says we are not record-bound).

---

## Status

- Phase 0 audit: **done** (this document)
- M1 — Vulkan core: **done**. `src/render/vulkan/`, `tests/test-avk-core.c`
  (32 checks), `tests/check-vulkan-isolation.py`.
- M2 — image/DMA-BUF import: **substantially done**.
  `tests/test-avk-dmabuf.c` (23 checks). Format/modifier capability table
  probed per modifier; explicit-modifier zero-copy import with disjoint and
  multi-plane handling; the implicit-modifier ladder above, verified
  pixel-exact. **Not yet done in M2:** the persistent staging *ring* (each
  copy currently allocates and retires its own staging buffer — correct, and
  retired on the GPU timeline, but one allocation per import), damage-region
  SHM upload, YUV sampler-conversion sampling, and DMA-BUF feedback tranches
  (which need protocol wiring, so they land with M3).
- M3 — native scene submission: **done. AVK renders the desktop.**
  - **M3a, the renderer.** `src/render/vulkan/scene/` + `pipeline/` +
    `shader/`: an Asteroidz-owned command IR (`avk_scene`), two pipelines over
    one push-constant block, dynamic rendering, premultiplied source-over,
    source crop, destination scale, all eight Wayland transforms, per-command
    visible-region clipping, per-command opacity, damage-scissored drawing,
    and the X-format opaque-alpha fix. `tests/test-avk-render.c` asserts all
    of it on read-back pixels (37 checks). No `wlr_render_pass` anywhere on
    this path, enforced by `check-vulkan-isolation.py`.
  - **M3b, the compositor.** `src/render/az_output.h` (the single build seam,
    used by all four callers), `src/render/az_avk.h` (the scene walker, the
    buffer cache, the per-output swapchain), and the `ASTEROIDZ_RENDERER=avk`
    switch. `contrib/avk-frame-test.sh` boots a real compositor twice and
    compares the frames; see §5.4b for what it does and does not cover.
- M4-M10: not started

## Pre-existing test failures (not caused by this work)

`meson test` reports `config-schema` with 22 failures: values above the
maximum are not clamped for `animation_duration_tag`, `animation_duration_focus`,
`overviewgappi`, `overviewgappo`, `hotarea_size`, `cursor_size`,
`cursor_hide_timeout` and others. **Verified against the pre-AVK tree** by
stashing this work and rebuilding: same 22 failures, same 1022 checks. Nothing
in the Vulkan work touches config parsing. Recorded here so scope attribution
stays clean; deliberately not fixed as part of the renderer migration.

---

## 5.4b What M3b built, and what it refuses to do

`ASTEROIDZ_RENDERER=avk` is read once in `setup()`, independently of
`WLR_RENDERER`. The pairing that proves the two are unrelated is
`WLR_RENDERER=gles2 ASTEROIDZ_RENDERER=avk`: a Vulkan-composited desktop with
GLES2 sitting alongside, touching no part of composition. That is what
`contrib/avk-frame-test.sh` runs.

The frame path, in one sentence: `az_output_build_frame()` (src/render/
az_output.h) replaces the four scattered `wlr_scene_output_build_state()` calls
with one seam; in AVK mode it walks the live `wlr_scene` tree into a flat
`avk_scene` command list, resolves each `wlr_buffer` to an `avk_image` cached
on the buffer via `wlr_addon`, and renders into a buffer from a per-output
`wlr_swapchain` we own. `wlr_render_pass`, `wlr_renderer_begin_buffer_pass()`
and `wlr_texture_from_buffer()` are not on it.

### What it refuses, per output, per frame

`az_avk_output_supported()` hands a frame back to SceneFX rather than render it
wrongly. Each of these is a thing M3 does not implement, and each is logged
once:

| condition | why | lands in |
|---|---|---|
| ICC transform or an output image description | AVK does no colour management | M6 |
| output magnification (`cursor_zoom`) | not implemented | later |
| a visible software cursor and no hardware cursor plane | the only wlroots API for it takes a `wlr_render_pass` | M3 follow-up |

On the DRM backend with a working cursor plane — a real desktop — none of these
fire, and `avk.fallback_frames` stays 0.

Shadow, blur and rounded-corner nodes are *recognised and skipped* with one
warning rather than silently dropped. Gradients render as their first colour.
All of that is M4.

### Known AVK-mode regressions, deliberate and logged

- **Full damage every frame.** ~~The scene's damage ring is maintained by
  `wlr_scene` for its own build path.~~ **FIXED in M3.5D — see §5.4e.**
- **Implicit synchronisation.** ~~The frame is submitted without exporting a
  sync_file for KMS.~~ **FIXED in M3.5C — see §5.4d.**
- **No direct scan-out, no gamma LUT, no DMA-BUF feedback from AVK's table.**
  Absent rather than half-present.

### The render target's first transition

A scan-out buffer is imported with `layout = VK_IMAGE_LAYOUT_UNDEFINED`, not
`GENERAL`, even though every other foreign image claims `GENERAL`. The
difference is whether the contents matter: a client buffer arrives full of
pixels somebody else wrote, and UNDEFINED would license the driver to discard
them; a render target arrives empty, and its first `UNDEFINED -> ...`
transition is where the driver initialises the image's compression metadata.
Claiming `GENERAL` for it skips that.

### SHM surfaces and the wlr_client_buffer problem — SOLVED in M3.5A

*What follows described M3. It is kept because the reasoning is the reasoning
that produced the fix; the fix itself is under "M3.5A" below.*

### The M3 problem

The one class of client AVK cannot draw correctly, and the reason is
structural rather than a bug to be fixed in this file.

When the compositor is created with a `wlr_renderer`, wlroots wraps each
client buffer in a `wlr_client_buffer`, uploads it into a `wlr_texture`, and
lets go of the original. From that point the wrapper answers *"not a dma-buf"*
and *"not CPU-readable"* to everything AVK can ask: the only surviving copy of
the pixels is inside a `wlr_texture`, which is precisely the object this
renderer may not touch. Measured, not assumed — it is the one remaining entry
in `avk.buffer_import_fails` in the harness run, and it is `swaybg`.

AVK's mitigation is to keep the copy it made while the source was still
readable, so static content stays correct. That leaves exactly one broken case:
a CPU-drawing client whose content changes *and* whose buffer wlroots has
already reclaimed. A wallpaper drawn once and never redrawn is the common
shape, and it is the one the harness hits.

`wlr_surface.current.buffer` was tried as a way round it and does not work —
wlroots has released that too by the time the frame is built.

The real fix, which is M3 follow-up work and touches the vendored SceneFX:
create the compositor with a **NULL renderer**, so wlroots uploads nothing and
`wlr_surface.current.buffer` stays locked, and teach
`subprojects/asteroidz-scenefx/types/scene/surface.c` to feed the scene the raw
buffer when there is no client buffer. `scene_buffer_get_texture()` already
falls back to `wlr_texture_from_buffer()` for a non-client buffer, so the
SceneFX path keeps working — the open question there is stale-texture
invalidation when a client re-commits the same buffer pointer with new content.

## 5.4c M3.5A — the client-buffer topology

AVK no longer has any dependency on a renderer-created `wlr_texture`. Three
changes, and the third is the one that was not obvious.

**1. The `wl_compositor` global gets no renderer in AVK mode.**
`wlr_compositor_create(dpy, 6, NULL)`, chosen once at startup before any client
can connect. wlroots then does protocol bookkeeping and no upload: no
`wlr_client_buffer` wrapper is made, and `wlr_surface.current.buffer` — the
client's real buffer — stays locked for the duration of the commit event.
`drw` itself stays, because wlroots still wants a renderer for shm format
advertisement, the allocator, screencopy and output cursors. None of those are
composition.

**2. SceneFX hands the scene the raw buffer.** `surface_reconfigure()` uses
`surface->buffer ? &surface->buffer->base : surface->current.buffer`, and the
scene locks whatever it is given, so it persists. The subtle half: that
function is *also* called from `scene_surface_create()` and
`scene_surface_set_clip()`, neither of which is a commit, and at those moments
`current.buffer` is NULL. Clearing the node there unmapped perfectly mapped
windows — every clip change blanked its surface. It now only unmaps when
`wlr_surface_has_buffer()` says the surface genuinely has no content.

**3. `surface->buffer` was being used as "does this surface have content".**
Six places in asteroidz — the layer-shell enable test, two overview snapshot
paths, an animation snapshot — asked `if (surface->buffer)` and meant "does it
have pixels". With no wrapper that silently became "is wlroots doing the
rendering", and the layer-shell one disabled every layer surface on the
desktop. That is why the wallpaper was missing: not an import failure, a
disabled node.

`src/render/az_surface.h` now answers the question properly. It records the
committed buffer per surface and holds a lock on it — the persistence the
`wlr_texture` used to provide by accident — and is installed in **both**
renderer modes, because "which buffer is this surface showing" should not have
two answers depending on who is drawing.

### When AVK takes ownership

At the commit that produced the content, never later.
`az_avk_surface_commit()` runs on `wlr_surface.events.commit`, which is the one
moment `current.buffer` is guaranteed valid, and imports it: a dma-buf import
dup()s every file descriptor, a CPU buffer is copied. After that the client may
destroy its `wl_buffer` immediately and AVK still has the content.

`avk.late_imports` counts surface buffers that reach a frame without having
been taken at commit. It must be 0, it is asserted by
`contrib/avk-frame-test.sh`, and `BREAK=wrapper` — which restores the wrapper
topology — drives it to 43. Note what that break test demonstrates: the pixels
still come out right. Ownership is not visible in a screenshot, so a counter is
the only way to test it at all.

### Two bugs this cost, both worth remembering

1. **Borders are one rect with the interior clipped out.** SceneFX draws a
   window border as a single filled rect carrying a `clipped_region` that
   removes the window's inside — that is how a rounded border gets a rounded
   inner edge. A walker that ignores the clip fills the whole window with the
   border colour, and since the border sits *above* the surface in the scene,
   every window renders as a flat block. It looked like a texture bug for far
   longer than it should have. `AVK_NO_BORDER_CLIP=1` puts it back, and
   `BREAK=border contrib/avk-frame-test.sh` must fail.
2. **A white screen on both monitors, from a diagnostic left switched off.**
   Imported buffers are acquired from `VK_QUEUE_FAMILY_FOREIGN_EXT` and
   released back. While testing whether the *acquire* mattered, the *release*
   was disabled with a `false &&` — and that shipped, because every headless
   assertion still passed. On a real display it did not: both outputs came up
   flat white, with every window rendered correctly inside a scan-out buffer
   KMS could not interpret.

   The lesson is not "do not leave debug code in". It is that **headless
   testing proves composition and can say nothing about presentation.**
   Nothing scans a headless buffer out, so the barrier that hands a finished
   frame back to the display engine is invisible to the entire suite. A
   passing `contrib/avk-frame-test.sh` is necessary and it is not sufficient;
   the last step is always a monitor.

   Also corrected here: the acquire half genuinely has no measurable effect on
   this GPU, but the earlier note claiming `AVK_NO_FOREIGN_ACQUIRE=1` gives a
   "pixel-identical desktop" was measured with the release already disabled.
   The switch turns off both halves and turns a real display white.

## 5.4d M3.5C — the frame reaches the display with a fence

Before this, `az_avk_build_frame()` submitted the render and then called
`wlr_output_state_set_buffer()`. Nothing between those two lines told the
display engine that the GPU had not finished writing the buffer. It worked,
and it worked for a reason that is not a guarantee: the amdgpu kernel driver
attaches implicit fences to the buffers in a submission, so the atomic commit
happened to wait. AVK also releases its scan-out target back to
`VK_QUEUE_FAMILY_FOREIGN_EXT`, which is precisely the operation that says
"another owner takes it from here" — relying on the driver to fence it anyway
is relying on the thing being contradicted.

### The chain

```
avk_render_frame()  signals a binary semaphore alongside the device timeline
      |
vkGetSemaphoreFdKHR(SYNC_FD)              src/render/vulkan/sync/avk_sync.c
      |    a sync_file: a kernel fence with a file descriptor
      +---> wlr_drm_syncobj_timeline_import_sync_file(in_timeline, point)
      |     wlr_output_state_set_wait_timeline(state, in_timeline, point)
      |         KMS waits on it in the atomic commit's IN_FENCE_FD
      |
      +---> DMA_BUF_IOCTL_IMPORT_SYNC_FILE on every plane of the target
                any implicitly synchronised consumer finds it there
```

and the release direction, so a target is not rendered into while it is still
being read:

```
wlr_output_state_set_signal_timeline(state, out_timeline, point)
      KMS signals when it has finished with the buffer
            |
      checked at the next acquire; if unsignalled, exported as a sync_file and
      imported as a VkSemaphore the render waits on -- a GPU wait, not a stall
```

### Which route, and why it is not a preference

`AZ_AVK_PRESENT_SYNC_TIMELINE` needs `wlr_backend_get_drm_fd() >= 0` **and**
`backend->features.timeline`. The headless backend satisfies the second and not
the first, and — separately — its `SUPPORTED_OUTPUT_STATE` excludes both
timeline fields, so a commit carrying one is rejected outright. So headless
always takes the dma-buf route and a real DRM backend always takes the timeline
route. `AZ_AVK_NO_TIMELINE=1` forces the fallback on hardware that would not
otherwise use it, because an untested fallback is not a fallback.

If neither route works the output is marked `BROKEN` and stays on SceneFX
permanently. There is deliberately no third option: presenting anyway and
hoping the driver fences it is the assumption this section removes.

### Target states

`az_avk_target` now carries `FREE` / `RENDERED` / `IN_FLIGHT`.
`RENDERED` — submitted but never committed, because the output state was
rejected — is safe to render into again without a wait, since both submissions
are on one queue and a queue orders its own work. `IN_FLIGHT` is not, and
`avk.target_state_violations` counts every time the swapchain hands back a
buffer whose release cannot be established.

One subtlety: a rejected commit leaves a release point that will never
materialise, and waiting on it would hang the output forever. The acquire asks
`DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE` before waiting, which distinguishes
"still in use" from "never submitted".

### What is tested, and what is not

| | headless | a monitor |
|---|---|---|
| dma-buf route end to end | `contrib/avk-sync-test.sh` | — |
| syncobj timeline round trip | `test-avk-core` (primitive level, no compositor) | — |
| timeline route end to end | **not covered** | `amsg get avk-stats` |

`present_sync_timeline` can only ever be 0 in a headless run. A build that broke
the timeline path entirely would pass the whole suite. That is the same shape of
gap that shipped a white screen once, and it is written here rather than
discovered again.

---

## 5.4e M3.5D — redrawing only what changed

The renderer was already damage-aware and had been since M3: `loadOp` is
`LOAD`, the background clear is an ordinary scissored draw, and every command
is clipped to `scene->damage`. What it never received was any damage but the
whole output. So this is almost entirely a compositor-side change.

### Whose damage

`wlr_scene`'s, and specifically `scene_output->damage_ring` — not a second
ring of AVK's own. The ring is keyed on the **`wlr_buffer`**, which is the
detail that makes sharing correct rather than merely convenient:

- A target that last held frame N-3 needs everything damaged since frame N-3.
  "What changed since the last frame" is right only for a swapchain one buffer
  deep, and `WLR_SWAPCHAIN_CAP` is 4.
- A buffer the ring has never seen comes back fully damaged, which is exactly
  right for a target whose contents are undefined.
- A frame that falls back to SceneFX renders into a *different* buffer, and the
  ring accounts for both, because it tracks buffers rather than frames. Two
  rings would mean each path silently forgetting the other's frames.

`wlr_damage_ring_rotate_buffer()` **mutates** the ring — it moves `current` into
this buffer's entry — so it happens after every check that can still decline
the frame, and every failure below it calls `wlr_damage_ring_add_whole()`.
Rotating and then not rendering is how a region gets recorded as drawn without
ever being drawn.

### The commit-failure hole this opened

`wlr_scene_output_commit()` trashes the ring when the commit fails. asteroidz
replicates that function by hand across four call sites and did not replicate
that part — which did nothing while AVK redrew everything, and became a source
of permanently stale rectangles the moment it did not.
`az_output_commit_failed()` in `src/render/az_output.h` is that line, now called
from all four.

### Output damage versus redraw damage

They are different questions. The redraw region is "what differs from what this
buffer last held"; output damage is "what differs from what is on screen now".
`wlr_scene` keeps the minimal answer in a `WLR_PRIVATE` field, and reaching into
one of those to save a few pixels of blit is not a trade worth making. The
redraw region is the honest superset — the on-screen buffer was presented more
recently, so it contains the minimal answer — and over-reporting output damage
costs the backend a little work and is never wrong.

Containing it matters for a second reason: `wlr_scene` subtracts the committed
damage from its pending region and `wlr_scene_output_needs_frame()` is true
while that region is non-empty. A report that did not cover it would leave the
compositor rendering frames forever on an idle desktop.

### What the test does, and one thing it had to learn

`contrib/avk-damage-test.sh` runs the same scene twice — once normally, once
with `AZ_AVK_FULL_DAMAGE=1` — and asserts the pictures agree. The full-damage
run is the reference precisely because it cannot be wrong.

Two premises are asserted before any of that counts, and both were needed:

1. The partial run must actually have redrawn less (`damage_ratio < 0.9`,
   `partial_redraw_frames > 0`), or the two runs agree by construction.
2. **The frames the screenshot came from must have been partial redraws.** The
   counters are reset immediately before the capture to establish this. Without
   it, a capture taken just after a full redraw looks correct however badly
   preservation is broken.

And a measured correction worth keeping: the first break switch made `loadOp`
`DONT_CARE`, and **the entire suite passed with it set**. `DONT_CARE` means the
contents become undefined, and a driver is entitled to leave them alone — which
is what RADV does on a desktop GPU, where there are no tiles to avoid loading.
A break switch the hardware is allowed to ignore is not a break switch. It
clears to magenta instead, which is the mistake the `loadOp` comment actually
warns about, and it fails six assertions.

---

## 5.4f M3.5D.1 — a buffer is uploaded because it changed, not because it was looked at

`az_avk_image_for_buffer()` re-uploaded every CPU-backed buffer on every frame
that drew it. On the live dual-monitor desktop that was measured at **41.5 MB
per output frame** — exactly `3840x2160x4 + 1920x1080x4`, one full-screen
wallpaper per output — **924 GB** over one session, and essentially all of a
6.6 ms CPU frame time. GPU submit over the same run averaged 0.034 ms.

The cause was a conflated question. These are five different things and only
the first two are about pixels:

```
BUFFER IDENTITY     which storage object is this?
CONTENT GENERATION  which committed version of its pixels is this?
CONTENT DAMAGE      which pixel regions changed in this generation?
SCENE DAMAGE        which output regions need recompositing?
PRESENTATION        which output target is being displayed?
```

A window moving across the desktop produces enormous scene damage and zero new
client pixels. The cache now keys on a **content generation**, not on traversal
and not on the buffer pointer.

### Where a generation comes from

Two events, and between them they cover every buffer the scene can hold:

- **`wlr_surface.events.commit` carrying `WLR_SURFACE_STATE_BUFFER`** — every
  client surface. This is the same condition wlroots itself uses to decide
  whether to re-upload a `wlr_texture` (`invalid_buffer` in
  `surface_commit_state()`), and `surface->buffer_damage` is already computed
  when the signal fires (`wlr_compositor.c:532`, emitted at 565).
- **`wlr_scene_set_buffer_content_observer()`**, a small addition to the
  SceneFX fork hung off `wlr_scene_buffer_set_buffer_with_options()` — the one
  place in the scene where a buffer's contents are declared current, already
  carrying buffer-local damage, and already covering both surfaces and the
  compositor's own cairo buffer nodes.

Surfaces are skipped by the observer, because the commit hook already handled
them and bumping twice would upload twice. That split is ordering-independent
in both directions, which matters because listener order between AVK and the
scene is not guaranteed.

**Not** an increment on: position, opacity, output damage, another window
exposing it, or another frame being scheduled. Those need recompositing and no
upload.

### Why not a pointer comparison

`buffer == cached_buffer` is not "the contents are unchanged". A client may
reuse a `wl_buffer` for a later commit — the protocol allows it — and a cache
that skips on identity renders such a client permanently frozen on its first
frame. It also *looks* correct against every ordinary toolkit, because they
rotate through a pool of two or three buffers and each new pointer is a cache
miss.

`contrib/wlreuse` exists solely to make buffer identity a lie: one
`wl_shm_pool`, one `wl_buffer`, one mapping, for the life of the process, with
only the bytes changing between commits. It is the only client in the tree that
does this, and `BREAK=identity` freezes it on red while every other assertion in
the suite still passes.

### Damage belongs to the surface, not to the buffer that carried it

The cache holds one image per `wl_buffer`, so it is tempting to apply a
commit's damage to the buffer that commit attached. That is wrong, and it was
wrong in shipped code until `ef45327`.

`wl_surface.damage_buffer` states what changed since the **previous commit of
the surface**. It says nothing about how far any particular `wl_buffer` has
drifted from the last time the compositor uploaded *it*. Those two quantities
are identical for exactly as long as the client reuses one buffer, and diverge
on the second commit of a rotating pool:

```
commit 1   buffer A   marks 0            damage = mark 0
commit 2   buffer B   marks 0,1          damage = mark 1
commit 3   buffer A   marks 0,1,2        damage = mark 2
```

Commit 3 is entirely correct: A really does contain all three marks, and mark 2
really is all that changed since commit 2. But the cache last uploaded A at
commit 1, so applying only "mark 2" leaves **mark 1 missing from its copy** —
and presenting A and B alternately blinks the client's own recently drawn
pixels.

So a surface keeps the pool of buffers it has committed, and a commit's damage
is noted against **every** buffer in it. Each buffer's pending damage is
cleared when that buffer is uploaded, so the cost is bounded by pool size and
paid only for buffers that are actually presented again.

This shipped as KDE applications flickering on their status bar and hover
highlight for the first seconds of their life — every Qt/KDE app rotates a
pool, `kitty` and `contrib/wlreuse` do not, and the entire SHM suite was built
on clients that do not. Full **output** damage does not mask it: it faithfully
recomposites the wrong pixels, which is why `AZ_AVK_FULL_DAMAGE=1` came back
negative and nearly closed the investigation, while `AZ_AVK_SOURCE_FULL=1`
— upload every buffer whole — made it vanish. `contrib/wlrotate` and
`contrib/avk-shm-rotate-test.sh` exist to keep it fixed; see
`docs/regression-testing.md` for why the obvious pixel assertion does not
falsify it and the upload accounting does.

### Why deferring the upload to lookup is safe

The copy happens at the next lookup rather than at the commit, which raises the
question of whether the client may have scribbled over the buffer in between.
It may not: `wlr_scene_buffer_set_buffer_with_options()` takes a lock on the
buffer (`wlr_scene.c:944`) and holds it until the buffer is replaced, so
`wl_buffer.release` is not sent and the client is protocol-bound not to touch
it. Release therefore happens when the next commit replaces the buffer — which
is a consequence of the raw-buffer path from M3.5A, not of this change.

### GPU lifetime

A new generation may arrive while an earlier one is still being sampled by an
in-flight frame. `az_avk_upload_shm()` already handled this and still does: the
upload submission waits on the device timeline at `image->last_use`, which is a
GPU-side ordering edge and costs the CPU nothing. There is no `vkQueueWaitIdle`
and no `vkDeviceWaitIdle` on this path.

### Measured, headless

| | uploads | bytes |
|---|---|---|
| static wallpaper, 7 frames, 21 lookups | **0** | **0** |
| `BREAK=lookup` (upload on lookup) | 21 | 3,723,720 **per frame** |
| reused buffer, red then blue | +1 | one buffer |
| `BREAK=identity` | 0 | frozen on red |

### Phase 2 — only the rectangles that changed

Source damage comes from `wlr_surface.buffer_damage`, which wlroots has
already made buffer-local, clipped to the buffer, and corrected for the
surface's scale, transform and viewport (`surface_update_damage()`, run before
the commit signal). Redoing that arithmetic here would be a second
implementation to keep in step with the first.

`avk_upload_image_write_regions()` packs the damaged rectangles tightly into
staging and issues **one** `vkCmdCopyBufferToImage2` with N regions, in one
submission. Rows are copied one at a time from `base + y * stride + x * bpp`,
because a rectangle's rows are not contiguous unless it spans the full width —
copying `width * height * bpp` as a block is the obvious shortcut and shears
the result. `bufferOffset` is aligned to lcm(bpp, 4), computed rather than
assumed 4.

Falls back to one full upload per generation when the damage is unknown
(`pending_full`) or has more rectangles than the packed path takes. Correct
once beats wrong cheaply, and it is still nothing like once per frame.

Measured headless, 512x512 buffer:

| case | uploaded | changed area |
|---|---|---|
| one 64x64 rectangle | **16,384 B** | 64x64x4 = 16,384 |
| two 64x64 rectangles | **32,768 B** | 2 x 16,384 |
| the same with 512 bytes of row padding | **16,384 B** | unchanged |
| 40 rectangles (past the packing limit) | 1 full upload, then 0 | — |
| `AZ_AVK_SOURCE_FULL=1` | 1,048,576 B | — |

Zero amplification: the copy is exactly the damaged area.

And the domains stay separate. Dragging a static terminal across the desktop:
**4,182,480 damaged output pixels, 21 buffer lookups, 0 uploads, 0 bytes.**

### Cull before resolve

The walk starts at the scene root, so it visits every node for every output —
including a 4K wallpaper belonging to a monitor 3840 pixels away. Resolving it
costs an import or a copy and produces a command the renderer then scissors to
nothing. The output-intersection test is now applied *before*
`az_avk_image_for_buffer()` rather than after, which is the same test the
renderer would have applied anyway. `avk.nodes_output_culled_before_resolve`
counts it.

### Why the same-queue path is correct, written out

The production dependency chain for updating a cached SHM image that a frame
may still be reading, with every mask named. Everything below happens on
`dev->graphics_queue` -- one queue, for both the frame renderer's ring and the
importer's upload ring.

```
frame N  (avk_render.c)
  samples the image
    stage  FRAGMENT_SHADER          access  SHADER_SAMPLED_READ
    layout SHADER_READ_ONLY_OPTIMAL
        |
        |  submission order on one queue: the upload is submitted after
        |  the frame that read the image, never before
        v
generation N+1  (avk_upload_image_write_regions)
  barrier: read -> transfer write
    srcStageMask   VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT
    srcAccessMask  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT
    dstStageMask   VK_PIPELINE_STAGE_2_COPY_BIT
    dstAccessMask  VK_ACCESS_2_TRANSFER_WRITE_BIT
    oldLayout      SHADER_READ_ONLY_OPTIMAL   (image->layout, observed)
    newLayout      TRANSFER_DST_OPTIMAL
        |
  vkCmdCopyBufferToImage2, N packed regions
        |
  barrier: transfer write -> shader read
    srcStageMask   VK_PIPELINE_STAGE_2_COPY_BIT
    srcAccessMask  VK_ACCESS_2_TRANSFER_WRITE_BIT
    dstStageMask   VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT
    dstAccessMask  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT
    oldLayout      TRANSFER_DST_OPTIMAL
    newLayout      SHADER_READ_ONLY_OPTIMAL
        |
  submitted with a wait on the device timeline at image->last_use
        |
        v
frame N+1
  samples the image, no barrier needed: image->layout already matches, and
  batch_add() skips a transition that would be a no-op
```

Three things make it correct, and it is worth being clear about which does
what:

- **The first barrier's `src` half** is the read-before-write dependency. It is
  what makes overwriting the image ordered against the sampling in earlier
  submissions on this queue. It is the load-bearing one.
- **The layout is read from `image->layout`, never assumed.** A barrier that
  transitions from the layout the code *believes* the image is in produces
  garbage on one driver and nothing on another; the field exists so the
  transition is emitted from observed state.
- **The timeline wait on `image->last_use`** is a second, coarser expression of
  the same ordering. On one queue it is redundant with the barrier. It is kept
  because it is the dependency that survives the upload moving to a transfer
  queue, where submission order no longer relates the two.

The image is never in `UNDEFINED` on this path: a partial update into an image
with no previous contents would discard everything outside the damage, and
`avk_upload_image_write_regions()` refuses rather than doing it.

`cpu_sync_waits` stays 0 throughout. Nothing here waits on the host.

### Live results — dual monitor, DP-1 3840x2160 + HDMI-A-1 1920x1080

Baseline is the same session before D.1, over 40530 output frames.
`avk.frames` counts **output** frames: a two-monitor desktop increments it
twice per refresh.

| | before | after (idle) |
|---|---|---|
| cpu_frame_us avg | **8860** | **83** |
| cpu_frame_us p50 | — | 80 |
| cpu_frame_us p95 | — | **160** |
| cpu_frame_us p99 | — | 180 |
| cpu_frame_us max | 42300 | **207** |
| SHM per output frame | **41.5 MB** | **0 B** |
| SHM total | 1680 GB / 40530 frames | 0 B / 1335 frames |
| damage_ratio | 1.0 | 0.293 |

**107x less CPU time per frame, and the SHM traffic is gone entirely.**

At 144 Hz the budget is 6944us. Before, the *average* frame was 8860us --
128% of budget, so the average frame missed. After, p95 is **2.3% of budget**
and the worst frame in sixty seconds was 207us, **3.0%**.

Three workloads, which together are the architectural proof that source damage
and scene damage are separate systems:

| | output damage | SHM upload |
|---|---|---|
| A: idle, static wallpapers | 0 full redraws, ratio 0.29 | **0 B**, 1335 skips |
| B: static terminal dragged | **20,125,548,488 px** | **0 B**, 0 commits |
| C: small damage, 392 generations | ratio ~0.5 | **7.45 MB**, amplification **1.0000** |

Workload C is the only one that exercises the partial path live: 392
generations of a 64x64 rectangle in a 512x512 buffer copied 7,454,720 bytes,
which is exactly `shm_damage_pixels * 4`. Uploading whole buffers would have
been 411 MB -- **55x more**.

Per-source attribution over the whole session, which is the wallpaper invariant
stated as a measurement:

| buffer | uploads | bytes | lookups |
|---|---|---|---|
| 3840x2160 wallpaper | **1** | 33,177,600 = 3840x2160x4 | 13,674 |
| 1920x1080 wallpaper | **1** | 8,294,400 = 1920x1080x4 | 1,969 |

One upload each for the life of the session, then fifteen thousand lookups
that cost nothing.

`nodes_output_culled_before_resolve` reads **2970 of 8010 node visits** at idle
and 11089 of 27205 during the drag. It read 0 in every headless run, because
one output means nothing is exclusive to another one -- the cull is only
observable on real hardware with two monitors side by side.

Every correctness counter stayed at zero throughout: `cpu_sync_waits`,
`target_state_violations`, `present_sync_failures`, `validation_errors`,
`fallback_frames`, `late_imports`. `present_sync_timeline` equalled `frames`.

Two things worth recording rather than smoothing over:

- The terminal dragged in workload B is **dma-buf** backed, not SHM. What that
  workload proves is that sweeping a window across the **wallpapers** -- which
  are SHM -- re-composited them 5190 and 251 times for zero generations and
  zero bytes. The invariant holds; the subject is the wallpaper, not the
  terminal.
- The 3840x2160 wallpaper is looked up roughly three times per DP-1 frame, so
  that buffer is referenced by more than one scene node. It costs an addon hash
  lookup and no bytes, but it is unexplained and worth understanding before M4.

### One break test that could not be made to fail

`BREAK=unsafe-reuse` removes both protections around updating an image a frame
in flight may still be sampling: the timeline wait on `image->last_use`, and
the barrier's read-before-write dependency. **Synchronisation validation
reports nothing**, with the layer demonstrably loaded, including in a case
where 221 generations and 82 partial uploads genuinely overlap frames.

The likely reason is that uploads and frames are submitted to the same
`VkQueue`, each upload ordered after the frame that read the image, so there is
no concurrency for validation to object to.

Stated so it cannot be misread later:

> **`BREAK=unsafe-reuse` could not be made observationally failing on the
> current single-queue architecture.** Removing the read→write image barrier
> also did not produce a validation failure, so synchronization validation is
> **not accepted as proof** for this hazard. The barrier and the timeline
> dependency remain in production. **Cross-queue upload work must add a
> deterministic synchronization test before a transfer queue is enabled.**

This case is *not tested*. That is preferable to false coverage.

Worth noting how it was narrowed: removing only the timeline wait changed
nothing, which is what led to removing the barrier's dependency as well. The
barrier, not the semaphore, is what orders this today.

---

## 5.4g M3.5E — the cursor audit, before anything is changed

Three cursor sources reach the screen, and they do **not** share a path. Two
survived the NULL-renderer topology and one did not.

Line references below are the `~/wlroots` working tree, which is checked out at
`0.21.0-dev`, while asteroidz links **0.20.2**. Every line quoted here was
re-read from `git show 0.20.2:` and is byte-identical in the version actually
linked; the two trees do diverge elsewhere, so anything else read from that
checkout has to be checked against the tag before it is relied on.

### The three sources

```
compositor xcursor / cursor-shape          client wl_surface
  wlr_cursor_set_xcursor()                   wl_pointer.set_cursor
  asteroidz.c:8160, 7204, 3535               -> setcursor(), asteroidz.c:8175
        |                                          |
  output_cursor_set_xcursor_image()          wlr_cursor_set_surface()
  wlr_cursor.c:497                           wlr_cursor.c:687
        |                                          |
  wlr_xcursor_image_get_buffer()             wlr_surface_get_texture()
  -> a wlr_buffer          <-- WORKS         -> a wlr_texture   <-- BROKEN
        |                                          |
  wlr_output_cursor_set_buffer()             output_cursor_set_texture()
  wlr_cursor.c:502                           wlr_cursor.c:578
        \____________________  ____________________/
                             \/
                    wlr_output_cursor
                             |
             output_cursor_attempt_hardware()   output/cursor.c:291
                    /                 \
        hardware plane          software: cursor->texture
        (a wlr_buffer)          wlr_output_add_software_cursors_to_render_pass()
                                output/cursor.c:91
```

**Where AVK broke it, exactly:** `wlr_cursor.c:560`,
`wlr_texture *texture = wlr_surface_get_texture(surface)`. That reads
`surface->buffer`, the `wlr_client_buffer` wrapper wlroots creates only when
the compositor was given a renderer. AVK gives `wl_compositor` NULL, so the
wrapper never exists, the texture is NULL, and the client's cursor has no
image. Nothing logs it.

**Why xcursor and cursor-shape did NOT break.** Both end at
`wlr_xcursor_image_get_buffer()`, which returns a **`wlr_buffer`** and never
touches the compositor's renderer. asteroidz maps cursor-shape onto xcursor at
`asteroidz.c:3535`, so `wp_cursor_shape_v1` inherits that immunity.

**A second, separate breakage that has not fired yet.** `wlr_cursor.c:530-531`
does `struct wlr_renderer *renderer = output->renderer; assert(renderer != NULL);`
on the `wlr_cursor_set_buffer()` path. `output->renderer` is the GLES
compatibility renderer, which is still non-NULL, so the assert holds today —
but it is a renderer dependency reachable from the cursor path and it is why
"the compositor is NULL-renderer" and "the output is NULL-renderer" must not be
conflated.

**And a third thing, which is why the desktop still works at all.**
`az_avk_output_supported()` already declines any output that needs a software
cursor, so those frames silently fall back to SceneFX. AVK has never composited
a cursor. The regression is therefore visible only where the *hardware* plane
carries the cursor and the image itself is missing — i.e. client-set cursors.

### The five concepts, kept apart

| | owner today | owner after E |
|---|---|---|
| **cursor image ownership** | `wlr_texture` (client) / `wlr_buffer` (xcursor) | AVK image cache, content-generation keyed |
| **cursor position** | `wlr_cursor` + `wlr_output_cursor` | unchanged — wlroots |
| **cursor hotspot** | `wlr_output_cursor.hotspot_x/y` | asteroidz cursor state, applied by AVK |
| **hardware-plane presentation** | `output_cursor_attempt_hardware()` | unchanged — wlroots, still preferred |
| **software composition** | `wlr_output_add_software_cursors_to_render_pass()` | AVK render command |

### What can be reused rather than reinvented

- **Forcing software mode**: `wlr_output_lock_software_cursors()`
  (`output/cursor.c:60`) already exists and already disables the hardware
  cursor. No new backend behaviour is needed for the test switch.
- **Knowing which mode is live**: `output->hardware_cursor` names the cursor
  currently on the plane, and the software helper skips exactly that one
  (`output/cursor.c:99`). That is the state to read rather than re-deriving
  hardware capability.
- **Getting the pixels**: `az_surface_buffer()` from M3.5A already holds each
  surface's committed buffer, locked, with no dependence on a wrapper — which
  is precisely what the client cursor path needs. Cursor content can go through
  the same content-generation cache D.1 built.

---

## 5.4h M3.5E stage 1 — asteroidz owns the cursor image

`src/render/az_cursor.h`. The audit's prediction held: the client cursor was
invisible, and it is now drawn.

### What changed

asteroidz stops asking wlroots to choose a cursor image and chooses one itself,
always expressing the answer as a **`wlr_buffer`** — the one representation
both consumers can take. `wlr_cursor_set_xcursor()` and
`wlr_cursor_set_surface()` are gone from `asteroidz.c`; all seven call sites go
through `az_cursor_set_xcursor()` / `az_cursor_set_surface()` /
`az_cursor_hide()` / `az_cursor_show()`, which end at `wlr_cursor_set_buffer()`.

That single change fixes the regression, because `wlr_cursor_set_buffer()`
imports through `output->renderer` — the GLES compatibility renderer, which is
non-NULL — rather than through `wlr_surface_get_texture()`, which needs the
`wlr_client_buffer` wrapper a NULL-renderer `wl_compositor` never creates.

**Nothing about position moved.** `wlr_cursor` and `wlr_output_cursor` still
own the per-output box, scale, transform and visibility, and the hardware plane
is still wlroots' and still preferred, for all three sources.

### What it costs, stated plainly

- **xcursor scale is now global, not per-output.** wlroots picked an image per
  output at that output's scale with a per-output animation timer; asteroidz
  picks one at the largest scale in the layout, the same rule wlroots itself
  applies to client surfaces. On a mixed-scale layout the cursor is drawn from
  the sharper output's buffer and scaled down on the other. This is not
  laziness: `wlr_cursor_output_cursor` is private, so its per-output animation
  index cannot be read, and an AVK that guessed would draw a different
  animation frame than the plane.
- **A same-pointer content change costs one extra cursor commit.**
  `wlr_cursor_set_buffer()` early-returns when the buffer pointer, hotspot and
  scale all match (`wlr_cursor.c:520`) — which is the M3.5D.1 bug, in wlroots,
  on the cursor path: a client animating a cursor by rewriting one `wl_buffer`
  would freeze on its first image. `az_cursor_push()` clears the image first to
  force the re-import, but only when the content generation actually moved
  under an unchanged pointer, and counts it in `forced_reimports`.

### What this stage does NOT do

AVK still does not composite a cursor. `az_avk_output_supported()` continues to
decline any frame that needs a software cursor, so those frames fall back to
SceneFX — which is why the test below passes today. Making that fallback
unnecessary is stage 2, and the same test has to keep passing once it is gone.

### How it is tested, and why it could not have been before

`contrib/avk-cursor-test.sh`, with two new clients, because the existing suite
was structurally incapable of seeing this bug:

- **`contrib/wlcursor`** is the only client in the tree that calls
  `wl_pointer.set_cursor`. Everything else leaves the pointer to the
  compositor's theme, so the whole suite ran green while client cursors were
  invisible. It paints one opaque-black pixel at its hotspot, which is what
  makes *placement* assertable rather than merely presence — a compositor that
  ignores the hotspot draws the complete image one hotspot away and passes
  every pixel-count assertion.
- **`contrib/wlshot`** is a screencopy client that asks for
  `overlay_cursor = 1`. grim always asks for `0`, so its captures contain no
  cursor at all and every assertion on one would be vacuous. This matters
  doubly on headless, where the backend implements `set_cursor()` as
  `return true;` and does nothing (`backend/headless/output.c:82`): it believes
  it has a plane, so cursors go to a plane that does not exist and a headless
  screenshot is *structurally* blind to them. `overlay_cursor` calls
  `wlr_output_lock_software_cursors()`, so asking for the cursor in the capture
  IS the request to composite it — no debug environment variable and no
  test-only branch in the compositor.

Captures are taken **cursorless first**. wlroots deliberately does not
re-enable the hardware cursor when a software lock drops (`output/cursor.c:75`),
so a cursorless capture taken after a cursored one still contains the cursor,
and the control would silently become a copy of the experiment.

Verified three independent ways, because one of them alone is not evidence:

| | client cursor |
|---|---|
| pre-M3.5E binary (`2a4bad1`), built from a clean worktree | **absent** |
| `BREAK=cursor-texture` (`AZ_CURSOR_LEGACY_SURFACE=1`) | **absent**, with `wlr_surface_get_texture()` observed returning NULL |
| this build | 1023 of 1023 px, hotspot marker at exactly the pointer |

`AZ_CURSOR_LEGACY_SURFACE=1` is not a simulated failure — it is the original
`wlr_cursor_set_surface()` call, restored.

---

## 5.4i M3.5E stage 2 — AVK composites the cursor

`az_avk_emit_cursors()` in `src/render/az_avk.h`. The software-cursor bail in
`az_avk_output_supported()` is gone: a frame that needs a cursor is no longer a
frame that falls back.

### The split, in code

Geometry is read from `wlr_output_cursor`, whose fields are all public and
already in output-buffer pixels — `wlr_output_cursor_move()` scales x/y by the
output scale and `output_cursor_set_texture()` scales the size and hotspot to
match. The image comes from `az_cursor`, because `wlr_output_cursor.texture` is
a `wlr_texture` belonging to the GLES compatibility renderer and AVK may not
touch it. Reimplementing the position arithmetic would be a second copy of it
that could disagree with the hardware plane about where the pointer is.

The draw is emitted **after** the scene walk, so the cursor is above
layer-shell overlays, fullscreen windows, the lock screen and the overview
alike. At most one is drawn per output: asteroidz has exactly one `wlr_cursor`,
so drawing every entry in `output->cursors` would mean drawing *our* image at
someone else's coordinates if another ever appeared.

**The hardware plane is still preferred and still free.** When
`output->hardware_cursor` names the cursor, AVK emits nothing and counts
`hardware_cursor_frames`.

### Damage is not done here, and that is deliberate

When a software cursor moves, wlroots emits `wlr_output.events.damage` for the
rectangle it left and the one it entered, and scenefx feeds that straight into
`scene_output->damage_ring` (`wlr_scene.c:3008`) — the same ring this frame's
damage was rotated out of. The old and new cursor rectangles are therefore
already in the damage AVK is drawing. Adding more here would only enlarge the
frame.

Measured, on a 1280×720 headless output with the pointer moving: **2048 damaged
pixels per frame**, which is exactly two 32×32 boxes, against a full output of
921,600. A compositor that repaints the whole screen per pointer motion is
correct and unusable, and the counter is the only thing that tells them apart.

### Counters

`software_cursor_frames`, `hardware_cursor_frames`, `cursor_commands`,
`cursor_no_image`, `cursor_import_failures`, `cursor_culled`, all in
`amsg get avk-stats`. `cursor_no_image` is the regression's fingerprint:
wlroots says a cursor is enabled and visible, and asteroidz has no picture for
it.

### Three break tests, each failing a different set

| break | what it does | what fails |
|---|---|---|
| `cursor-texture` | the original `wlr_cursor_set_surface()` | every image assertion; also *no frames at all*, since with no cursor there is no cursor damage |
| `cursor-command` | `AZ_AVK_NO_CURSOR=1`, emit no draw | the image assertions **and** `software_cursor_frames` 0 of 8 — while 8 frames of cursor damage still occur. This is the ownership discriminator |
| `cursor-damage` | `AZ_AVK_FULL_DAMAGE=1` | only the damage assertion, at 921,600 of 921,600 |

`cursor-command` is the one that matters. Before this stage the whole output
fell back to SceneFX the moment a software cursor existed, so a cursor appeared
in every capture and *nothing established who drew it*. With AVK compositing
and the emission disabled, nothing else is drawing into the frame, so the
pointer disappears.

`cursor-damage` is honest about its scope: it breaks damage globally rather
than only for the cursor. It is here because the per-frame damage assertion has
to be falsifiable by something, and nothing narrower exists — the damage
arrives from wlroots, not from AVK.

### Not covered yet, and not claimed

- **Animated and same-buffer cursors.** `az_cursor`'s `forced_reimports` path
  exists and is reasoned about, but no test drives `wlcursor --reuse-buffer
  --animate-ms` yet. The claim that a cursor rewriting one `wl_buffer` keeps
  updating is currently an argument, not a measurement.
- **Cursor scale and output transform.** `wlcursor --cursor-scale` exists;
  nothing runs it.
- **Multi-output.** `cursor_culled` reads 0 headlessly for the same reason
  `nodes_output_culled_before_resolve` does — one output means nothing is
  exclusive to another.
- **Hide/unset damage**, and the hardware↔software transition in both
  directions.
- **Live acceptance** on DP-1 + HDMI-A-1.

*(Stage 4 closed hide/unset; see §5.4k. Stage 3 closed the first three of these.
Multi-output became testable once
the harness grew `HL_OUTPUTS=2` — see below. Hide/unset damage and live
acceptance still stand.)*

**Multi-output, resolved.** `nodes_output_culled_before_resolve` reads 0 on a
single output by construction, so both the counter and the break test that
flips it were unreachable. `contrib/lib/headless.sh` now takes `HL_OUTPUTS=2`
and places `HEADLESS-2` immediately to the right of `HEADLESS-1`;
`avk-damage-domains-test.sh` runs two outputs and asserts the cull, measuring 6
culled / 24 resolved normally against 0 culled / 40 resolved under
`BREAK=no-cull`.

`cursor_culled` stays at 0, and correctly: `wlr_output_cursor.visible` is
per-output and wlroots computes it, so a cursor belonging to the other monitor
is skipped as not-visible long before the cull test. The check is defensive
against a visible cursor landing outside the buffer, which normal operation
does not produce. Recorded so a permanent 0 is not read as missing coverage.

---

## 5.4j M3.5E stage 3 — scale, content and handover

`contrib/avk-cursor-content-test.sh`. Stage 2's test asks whether a cursor
reaches the screen; this one asks the three questions that only arise once it
does, in one compositor run, because one client can exhibit all three: a
scaled, animating, buffer-reusing cursor.

### A real bug, found by the first test that used a scale other than 1

`wlr_cursor_set_buffer()` wants the hotspot in **logical** units, and its
sibling `wlr_output_cursor_set_buffer()` wants it in **buffer pixels**. The
first passes the value through to `output_cursor_set_texture()`, which
*multiplies* by the output scale; the second *divides* by it first. asteroidz
was converting surface-local hotspots up into buffer pixels, which is the
second convention fed to the first function.

The two conventions are identical at scale 1, so every test up to this point
passed, and on an ordinary desktop nothing would ever show it. At scale 2 the
cursor lands offset by half of itself — measured as an origin of 624,344 where
632,352 was required.

Both sources needed opposite fixes, which is what makes the units worth stating
in the header rather than inferring:

- a **client** states its hotspot in surface-local coordinates, which are
  already logical — it passes through untouched;
- an **xcursor image** states its hotspot in buffer pixels, because the theme
  loads a larger image for a larger scale — it is divided by the scale.

### What the three sections establish

| | measured |
|---|---|
| **scale** | a 64×64 buffer at scale 2 occupies **32×32** on screen, with its origin 8 px in — the hotspot in surface units, not buffer units |
| **content** | red → blue under **one** `wl_shm` buffer, one pool, one fd, one mapping. D.1's content model, applied to the cursor |
| **handover** | 32 frames carried by the plane with `software_cursor_frames` 0, then AVK compositing once screencopy locked software cursors |

### Two breaks, each failing exactly one thing

`BREAK=cursor-generation` (`AZ_AVK_CACHE_BY_IDENTITY=1`) fails **only** the
content assertion — the cursor freezes on red while the client goes on
committing. `BREAK=cursor-hotspot` (`AZ_AVK_NO_CURSOR_HOTSPOT=1`) fails **only**
the two placement assertions.

The second break turned up something worth keeping. Drawn without the hotspot
subtracted, the cursor comes back **24×24 instead of 32×32** — because wlroots
computes the cursor's damage from where the cursor actually *is*, so a draw
placed 8 px away falls partly outside the damaged region and is scissored. That
is independent confirmation that the cursor damage really is tight around the
cursor rather than a full-output repaint: if it were not, the misplaced draw
would have come out whole.

### Two test-design mistakes, both of which produced a green run

Recorded because both are the kind that look like coverage:

1. **The content assertion first read "capture 2 is blue".** Under
   `BREAK=cursor-generation` the cursor froze on whatever colour was current at
   first upload — which, with an animation already running, was blue. The
   assertion passed while the bug it existed for was fully present. An
   assertion about a *change* has to name both endpoints, so it now reads
   "capture 1 was red and not blue, and capture 2 is blue and not red".
2. **A cycling animation is a race, not a test.** At 2000 ms spacing, a capture
   three seconds later landed on the *second* transition and read the original
   colour back. `wlcursor --animate-once` makes exactly one transition and
   stops, so a capture at any later moment sees the same thing.

### And one that was not this milestone's fault

Adding "moving a cursor re-uploads nothing" to stage 2's test needed a break to
falsify it, and the obvious one already existed: `AZ_AVK_UPLOAD_ON_LOOKUP`, from
D.1. It did not work — the assertion passed with the switch set.

The reason turned out to matter more than the cursor. That switch restores the
unconditional upload **call**, but D.1 *Phase 2* made the copy damage-driven, so
the call now finds no pending damage and `az_avk_upload_shm()` returns at
`rect_count == 0` having copied nothing. Phase 2 neutralised Phase 1's break
test, in the same milestone, and nothing noticed: running
`BREAK=lookup ./contrib/avk-shm-cache-test.sh` today returns **7/7 passed**,
where the documented behaviour is 3,723,720 bytes per frame.

Both breaks now set `AZ_AVK_SOURCE_FULL` alongside it, and both fail again with
exactly the documented numbers — 3,723,720 B/frame for the wallpaper case,
44,883,968 B for the cursor case.

The general point: a break test stays a break only while the implementation it
subverts still works the way it did when the switch was written. An
implementation change can neutralise a switch without touching the switch, the
test, or anything visible in a diff — so a break run that comes back green is a
failure of the suite, not a curiosity.

### A third, smaller test-design mistake

`wlcursor` originally committed its animation only while
the pointer was inside. `contrib/wlvptr` moves the pointer and exits, so the
leave arrived seconds before the first tick and the cursor appeared frozen for
a reason that had nothing to do with the compositor. A real client animates on
a timer, so it does now too.

---

## 5.4k M3.5E stage 4 — hide, unset, and forcing software

### Hide/unset was already correct. The test says so, rather than fixing it.

`contrib/avk-cursor-hide-test.sh`. Written because "the cursor went away" has
four failure modes and three of them look fine in a screenshot taken a moment
too late: a **ghost** (state cleared, pixels left behind because nothing damaged
the rectangle), **over-damage** (cleaned up by repainting the whole output —
correct, invisible to any pixel comparison, ruinous at pointer rates),
**collateral** (the rectangle repainted with the wrong thing), and a **stale
image** (the cursor returns or changes and the old picture appears).

Every assertion is therefore differential — the same scene with the cursor,
without it, and with it again — because no single capture can express any of
this. Measured:

| | |
|---|---|
| hiding damages | **1024 px**, at exactly 636,356, 32×32 |
| full-output redraws to do it | **0** |
| hide → show versus before the hide | **0 px differ** — byte-identical |
| A → NULL → B | 1023 px of B, **0 px of A** |
| cursor image uploads across the whole sequence | 2, one per distinct `wl_buffer` |

Three sequences, one client, one run: `visible → hidden`,
`hidden → visible (same image)`, and `A → NULL → B`. `contrib/wlvptr` grew
`hold:<ms>` for it — it normally moves the pointer and exits, which destroys the
virtual pointer device, and a `set_cursor` request carries an enter serial the
compositor checks against the focused pointer client, which by then is nobody.

### There is no BREAK=cursor-old-damage, and there should not be

AVK does not compute old-position damage. wlroots emits
`wlr_output.events.damage` for the rectangle a software cursor vacated and
scenefx feeds it into the ring AVK rotates out of. **AVK consumes that damage;
it does not own the calculation.** A switch that disabled it would be disabling
wlroots' work through a hole punched in AVK for the purpose — a test of a
fabrication rather than of the compositor. The assertions above observe the
*result*, which is the honest thing available. The two breaks that do exist,
`cursor-command` and `cursor-generation`, fail on the premise and on the stale
image respectively.

### Forcing software: `ASTEROIDZ_AVK_FORCE_SOFTWARE_CURSOR=1`

Takes a permanent `wlr_output_lock_software_cursors()` at output creation. No
new backend behaviour: that is the same call screencopy's `overlay_cursor`
makes, and the same one a screen recorder trips — this is a permanent version
of a lock normally held for the duration of a capture. Logged per output at
INFO, because a session quietly paying for a composite instead of a plane is
not something anyone should have to infer from a frame rate.

It is a **startup** flag. There is no runtime toggle, so live evidence for
promotion and demotion has to come from a recorder taking and dropping the lock
rather than from flipping a switch.

Which is also why the acceptance run for it is a live one.
`/usr/share/wayland-sessions/asteroidz-avk-swcursor.desktop` starts a session
with the flag set, and `contrib/avk-software-cursor-acceptance.sh` measures it.
A headless output cannot: the headless backend's `output_set_cursor()` is
`return true;`, so it believes it always has a plane — a fresh headless AVK
instance reports `hardware_cursor_frames: 3, software_cursor_frames: 0` with no
cursor plane anywhere in the system. Forced software and hardware-planed are
the same code path there and every cursor test in `contrib/` runs on both
without being able to tell them apart. See `docs/regression-testing.md` for
what the run asks and why half of its questions go to the user rather than to a
counter.

### Who owns an xcursor, and what invalidates it

Read out of wlroots 0.20.2 (the version this machine runs: `wlroots0.20
0.20.2-1.1`, the signed upstream tag with no downstream patches), because a
live desktop was lost to getting this wrong.

| call | ownership consequence |
| :--- | :--- |
| `wlr_xcursor_manager_get_xcursor()` | returns **manager-owned** memory. Borrowed, no refcount, no lock to take |
| `wlr_xcursor_manager_load(scale)` | **additive**. Appends a scaled theme to a list and frees nothing (`wlr_xcursor_manager.c:33-54`), so pointers handed out earlier stay valid |
| `wlr_xcursor_manager_destroy()` | **invalidates everything.** Walks every scaled theme → `xcursor_destroy()` → frees each image, each pixel buffer and the cursor itself (`wlr_xcursor.c:38-47`) |
| `wlr_xcursor_image_get_buffer()` | `&image->readonly_buffer->base`, embedded in the theme's image |
| `readonly_data_buffer_drop()` | if locks are held, **copies the pixels into `saved_data`** first (`readonly_data.c:66-88`), so a locked cursor buffer survives its theme |
| `wlr_cursor_set_buffer()` | takes its **own** `wlr_buffer_lock()` (`wlr_cursor.c:475`). The caller need not keep one |

Two of those corrected a wrong theory during the M3.5E crash investigation, and
both are worth keeping written down. Mixed-scale loading was blamed first — it
cannot invalidate anything. And the buffer contract was blamed second — wlroots
locks correctly, so a wild `wlr_buffer` there was never a lock-balance bug; it
had been read out of an already-freed image.

The rule that follows: **the only durable cursor identity is the name and the
scale.** A `struct wlr_xcursor *` is a temporary answer to "what does the
current manager have for this name", and `reapply_cursor_style()` rebuilds that
manager on any live config change.

### Counters for the live runs

`cursor_moves`, `cursor_damage_pixels`, `cursor_hw_to_sw`, `cursor_sw_to_hw`,
`cursor_client_surface_sets`, `cursor_shape_sets`, `cursor_xcursor_sets`,
`cursor_unsets`, `cursor_forced_reimports`, `cursor_force_software`, and the
cursor image's own `cursor_source_{commits,uploads,upload_bytes,upload_skips}`.

The last four are read straight off D.1's existing per-buffer record rather
than counted again, deliberately: they are the "position changed != pixels
changed" invariant in four numbers, and a separate counter could drift from the
thing it claims to describe — which is the exact class of bug D.1 removed.

---

## 5.5 M3.6 — who decides what a client may allocate

The compositor asked GLES what it could consume and then handed the buffer to
AVK. Three values came from the wrong place:

```text
wlr_linux_dmabuf_v1_create_with_renderer(dpy, 5, drw)   default feedback
SceneFX  .main_renderer = output->renderer              per-surface feedback
```

`wlr_linux_dmabuf_feedback_v1_init_with_options()` derives the main device,
the composition tranche, and the set that scanout tranches are intersected
against — all three from `main_renderer`, which in AVK mode is the GLES2
compatibility renderer that composites nothing.

### The rule

> wlroots implements the protocol; AVK determines the GPU capabilities.

`avk_format_table` is the source, and had claimed the job in its own header
since it was written. Every entry is probed with
`vkGetPhysicalDeviceImageFormatProperties2`, external-memory handle type
chained in — not inferred from what the driver enumerates.

```text
AVK import capability  +  output scanout capability  +  device topology
        ↓
composition set        123 pairs      main device = AVK's DRM node
scanout tranche        composition ∩ wlr_output_get_primary_formats
        ↓
wlr_linux_dmabuf_v1_create(dpy, 5, feedback)      wlroots: protocol only
```

No protocol fork. `wlr_linux_dmabuf_v1_create()` already takes a caller-owned
feedback. Only per-surface feedback needed anything, because
`init_with_options()` asserts a renderer — hence
`wlr_scene_set_linux_dmabuf_capabilities()` in SceneFX, phrased as "the
compositor knows its capabilities" with nothing AVK-specific in it.

### Withheld on purpose

| withheld | why |
|---|---|
| `DRM_FORMAT_MOD_INVALID` | a fallback to cope with, not a modifier to ask a client for. The GBM on this driver cannot recover implicit modifiers, so an implicit buffer takes the copy path — advertising it would request that deliberately |
| render-only modifiers | client content is sampled; `texture_mods` is the right array |
| NV12, YU12, P010 | importable, but range/matrix/transfer are not implemented (M5). Advertising them would advertise a bug |

Live: 123 advertised + 15 withheld = 138 probed, and the test asserts that
equation so an unexplained gap fails rather than passing quietly.

### What this did NOT change, measured

Nothing observable on this machine. `copied` was already 0 on every build in
the log — clients were negotiating explicit modifiers AVK could import all
along — and live, the GLES renderer sits on the same Navi31 node AVK selects,
so the main device would have been identical either way.

M3.6 corrected an ownership inversion and produced no measurable behavioural
difference here. That is the honest result: the value is that the compositor
can no longer advertise a pair AVK would reject, and that a future
configuration where the two engines disagree — a different GPU, a Mesa update,
a multi-GPU layout — is now answered correctly by construction rather than by
coincidence.

That coincidence is also why the live test cannot protect the rule. On
overlapping tables, advertising the wrong source still passes every subset
check; the shipped bug did, for the whole of M3.5. `tests/test-dmabuf-feedback.c`
therefore drives the model with synthetic sets — AVK `{A,B,C}`, other renderer
`{A,B,D}`, expect `{A,B,C}` — and with two real render nodes made to disagree,
neither of which depends on this GPU.

---

## 5.4m The cursor has one owner, and seven call sites did not know it

M3.5E stage 1 is titled "asteroidz owns the cursor image". It did not, quite.
Seven call sites kept calling wlroots' own `wlr_cursor_set_xcursor()`:

```text
bind_define.h:439/464/466   move and resize cursors ("grab", "<corner>-resize")
bind_define.h:2238/2746     "default" resets
bind_define.h:3396          the screenshot UI's "crosshair"
parse_config.h:5362         "left_ptr", inside reapply_cursor_style()
```

That is a different ownership model, not a different spelling:

| | image | scale handed to wlroots |
|---|---|---|
| `az_cursor_set_xcursor()` | ONE, at the **sharpest** scale in the layout | `wlr_cursor_set_buffer(..., scale)` — wlroots divides back to a logical size identical on every output |
| `wlr_cursor_set_xcursor()` | **per output**, at each output's **native** scale | `wlr_output_cursor_set_buffer()` — **no scale argument**, 1:1 |

Both are self-consistent. Mixing them is not, because
`az_avk_emit_cursors()` takes the destination box from `wlr_output_cursor`
and the pixels from `az_cursor.buffer`. After a bypassing call the box
describes wlroots' image and the pixels are asteroidz's.

Found on a real desktop, in forced-software mode, on a 1.5 / 1.0 layout:

- **dragging a window showed no grab cursor at all** — the shape wlroots
  selected was never given to `az_cursor`, so AVK kept drawing the arrow;
- **resizing on the 1.0 output made the arrow bigger** — asteroidz's image is
  36px at scale 1.5, which is 24 output px there, and wlroots' own choice for
  a scale-1.0 output at `cursor_size 28` is larger.

It is invisible with a hardware cursor plane, because there wlroots' image is
what reaches the plane and asteroidz's never enters the frame. The daily
driver uses the plane, so nothing showed for the whole of M3.5E.

The fix routes all seven through `az_cursor_set_xcursor()`.
`reapply_cursor_style()` gets `az_cursor_theme_replaced()` instead, which
re-*pushes* the current cursor against the new manager rather than
re-*selecting* `left_ptr` — the old call discarded whatever shape was showing
on every live config apply, and a re-selection is waste besides, since the
locked buffer survives the rebuild.

`cursor_geometry_mismatch` now counts the disagreement directly:

```text
expected = image_size / az_cursor.scale * output_scale
```

Zero is the only correct value, and it is asserted rather than assumed.

### Three conditions, and no test had all three

The bug needed forced software composition, **two outputs at different
scales**, and a **compositor-driven** move or resize. Every cursor test drove
shapes from clients, which take the `az_cursor` path and therefore agree with
themselves; the mixed-scale tests that existed were not mixed-scale at all
(see docs/regression-testing.md). `contrib/avk-cursor-owner-test.sh` supplies
all three and asserts each premise before relying on it.

---

## 5.4l Teardown: who destroys what, and when the GPU has to be finished

Four real sessions ended in `double free or corruption (out)` or `corrupted
double-linked list`, always at the same place: immediately after the last line
`az_avk_log_stats()` prints, in the destruction that follows it. Four aborts in
sixteen logged exits, across four commits — the earliest of which predates
M3.5E entirely, so this is not a cursor bug that arrived with M3.5E. It is
teardown, and it has been there since AVK first had a teardown path.

### The rule that was not being followed

> A Vulkan object may not be destroyed while a submitted batch still refers to
> it.

`cleanup()` destroys the backend, which destroys the outputs, which runs
`az_avk_output_destroy()` → `avk_sync_finish()` → `vkDestroySemaphore()` on
each output's present fence and its wait slots. The last frame's submission
refers to those semaphores. Nothing in `cleanup()` had waited for it.

The waits that did exist were all inside `az_avk_finish()`'s callees — one at
the top of `avk_renderer_finish()`, one at the top of
`avk_dmabuf_importer_finish()`, one inside `avk_device_destroy()`. Each was
correct about its own resources and silent about everything destroyed before
it, and `az_avk_finish()` runs *after* the outputs are gone. So the first
destruction of teardown had no wait in front of it at all, and the comment at
the call site said the opposite.

A driver asked to destroy an object a pending submission still lists does not
usually fail loudly. It frees the host allocation and leaves the submission's
reference dangling; the damage surfaces at some later, unrelated `free()`.
That is exactly why every abort landed inside `az_avk_finish()` — in the frees
that came *after* the violation, never at the violation.

### The order now

```
cleanup()
    allow_frame_scheduling = false          submissions stop
    az_avk_quiesce()                        <-- ONE wait, before anything dies
    wlr_backend_destroy()                   outputs, swapchains, avk_sync_*
    wl_display_destroy()                    client buffers -> retire queue
    wlr_scene_node_destroy()
    az_avk_finish()
        avk_device_wait_idle()              belt to the braces above
        client images        -> retire queue
        avk_renderer_finish()               ring, pipelines, pools
        avk_dmabuf_importer_finish()        retire queue drained, upload ring
        avk_device_destroy()
            pipeline cache, timeline semaphore
            live-object census              every count must be 0
            vkDestroyDevice()
```

A shutdown wait is not a frame-path wait. `cpu_sync_waits` counts stalls in the
frame path and stays at 0; there is no frame after `az_avk_quiesce()`.

`avk_device_wait_idle()` is now a function of its own rather than the first
line of a destructor, because quiescence is a *precondition* of destruction,
not a step inside it — and a precondition a caller cannot invoke is not one a
caller can rely on. Every caller of a `*_finish()` establishes it explicitly,
including the tests and `avk-probe`.

### The ownership ledger

| resource | created by | stored in | destruction owner | retired? |
|---|---|---|---|---|
| `avk_image` (client content) | `avk_dmabuf_import` / `avk_upload_image_create` | `az_avk_buffer.image`, keyed on the `wlr_buffer` addon | the importer retire queue, after `az_avk_buffer_destroy()` hands it over | yes, at `image->last_use` |
| `avk_image` (output target) | `avk_dmabuf_import(for_render)` | `az_avk_target.image`, addon on the output buffer | same queue, via `az_avk_target_addon_destroy()` | yes, at `image->last_use` |
| `avk_upload` (SHM staging) | `staging_ensure()` | `az_avk_buffer.upload`, by value | the importer retire queue | yes, at `up->last_use` |
| `avk_upload` (one-shot copy) | `import_by_copy()` | heap, owned by the queue from the moment it is pushed | the importer retire queue | yes |
| pipelines, layouts, samplers, descriptor pools | `avk_pipelines_init` / `add_pool` | `avk_renderer.pipes` | `avk_pipelines_finish()` | no |
| command pools and buffers | `avk_cmd_ring_init` | `avk_cmd_ring.slots[]` | `avk_cmd_ring_finish()` | no |
| present fence, wait slots | `avk_sync_init` | `az_avk_output.sync` | `avk_sync_finish()`, at output destroy | no |
| device timeline semaphore, pipeline cache | `avk_device_create` | `avk_device` | `avk_device_destroy()` | no |

The rule the table encodes: **at every instant a resource has exactly one
destruction owner.** References may live in several containers; destruction
ownership may not.

`avk_retire_push()` therefore *transfers* ownership rather than notifying, and
says so in its header. It refuses a pointer already in the queue and counts the
attempt in `lifecycle_violations`, because the opposite reading — "the queue
just notifies, the original owner still frees" — is precisely the double free
this section is about.

### The two retire queues

There are two, and only one is used.

- `avk.importer.retire` ("importer") takes every client image, every output
  target image and every staging buffer. Collected once per frame from
  `avk_dmabuf_importer_collect()`, drained at teardown by
  `avk_dmabuf_importer_finish()`.
- `renderer->retire` ("renderer"), one per format slot, is initialised,
  collected every frame and drained at teardown — and **nothing ever pushes
  into it**. It has been empty for its whole existence.

So no resource can be in both, and no wrapper in one can own a child in the
other. That is worth stating rather than leaving as an accident: it was the
leading hypothesis for the double free and it is false.

### Two fabricated timeline values, now derived

`az_avk_buffer_destroy()` retired the SHM staging buffer at
`avk_device_timeline_value() + 1` — a point no submission owns, meaning "one
more frame from now". During a session that is accidentally safe: uploads and
frames share one queue, so completion follows submission order. At teardown it
is a point nothing will ever signal.

`staging_ensure()` was worse: growing the buffer called `avk_upload_finish()`
on the old one immediately, destroying a `VkBuffer` and its memory that a copy
the GPU had not yet run might still be reading — the one place in AVK that
destroyed in use, in the file that exists to avoid doing so.

Both are fixed by recording the real point. `avk_upload.last_use` is set by the
submission that reads the staging buffer, `staging_ensure()` hands the outgoing
buffer to the retire queue against it, and the destroy path retires against it
too.

### What is instrumented, and why counters rather than assertions

`lifecycle_violations`, a signed live-object census per class, and a
`LIVE`/`DESTROYED` state word on `avk_image` with a stable id. The census is
logged immediately before `vkDestroyDevice`, where every count must be zero — a
positive count is a leak, a **negative** one is a double destruction.

The double-destroy check reads memory the function may already have freed,
which is itself undefined, and that is deliberate: under ASan it is a
use-after-free report with both stacks, which is the best available outcome,
and without ASan a freed chunk essentially never still reads as `AVK_IMAGE_LIVE`.
It is a diagnostic, not a protection, so it counts a violation rather than
quietly carrying on.

### What this does not claim

The live abort was **not** reproduced headlessly. Forty-six headless graceful
exits of the unmodified pre-fix build produced zero validation errors; the
violation is timing-dependent — whether a submission is outstanding at the
instant the outputs are destroyed — and headless timing hides it. What *is*
established: the ordering violation is real and spec-cited
(`VUID-vkDestroySemaphore-semaphore-05149`), it is of the right class and in
the right place for the observed aborts, and it is reproducible on demand via
`BREAK=destroy-before-idle`, which restores the shipped ordering and fails
every cycle.

---

## 5.6 M4A — the large-radius artifact is a border, not a corner

A rounded window at `border_radius 40` on a real desktop showed "the edge of
the square window flickering through" its corner. Three explanations were
available and they are not close to each other: AVK's rounded coverage is
wrong; the corner region stops being damaged; or the border's mask is wrong.
Two of them are now excluded.

**What was built to tell them apart.** `contrib/wlrepaint`, a client that
repaints its entire surface every generation, alternating every pixel between
two four-colour checkers whose generation is recoverable from a single capture.
Behind a stationary, quiet, rounded foreground window, that turns "is this
corner stale" into a question about one screenshot rather than a theory about
frame timing. `contrib/avk-rounded-persist-test.sh` then classifies every pixel
of each corner box against the geometry: outside the arc it must be background
of the CURRENT generation, inside it must be window or border.

**The result.**

| | AVK | GLES |
|---|---|---|
| `border 0`, radius 40 | clean | clean |
| `border 6`, radius 40 | **104 background pixels inside the arc, per corner** | clean |
| `border 6` + `AZ_AVK_FULL_DAMAGE=1` | 104, unchanged | — |

Zero stale and zero unpainted pixels anywhere, on either renderer, at either
border width. Every bad pixel shows the *current* generation, and forcing a full
redraw changes nothing — so this is coverage geometry, and damage is exonerated
by two independent arguments.

**Where it lives.** The rect case in `az_avk.h` subtracts
`rect->clipped_region.area` as a plain pixman box and never reads
`rect->clipped_region.corners`, which sits beside it in the same struct and
which `apply_border()` fills in per corner with `border_radius - bw - 1`. So
the border's outer edge is a rounded SDF and its inner edge is a square hole.
On a corner's diagonal the square hole removes the part of the interior the
CLIENT's own arc has already cut away, and between the two arcs nobody paints.
Full mask algebra and measurements in `docs/avk-effects.md`.

**What this does NOT excuse.** M4A's rounded client path is clean under a live
changing background, but that is a statement about the corner, not about the
window. The visible artifact is real and belongs to M4B, and the fixture that
demonstrates it (`BORDER=6`) fails today on purpose and stays out of the green
suite until the inner cut-out is rounded.

**A break switch narrow enough to be honest.** `AZ_AVK_DAMAGE_HOLE=x,y,w,h`
subtracts a rectangle from every frame's damage after the ring has been
rotated, so the region is acknowledged and never redrawn. §5.4i noted that the
only available damage break was `AZ_AVK_FULL_DAMAGE`, which breaks damage
globally; this one breaks it in one place, which is what a persistence test
needs to prove it can see a stranded region at all.

---

## M3b work plan (the compositor half) — DONE, kept for the record

Written down here rather than left in a conversation, because this was the
queue the working session executed. §5.4b above records what actually landed
and where it differed. Everything below lands **behind
`ASTEROIDZ_RENDERER=avk`, defaulting off** — which is what makes partial
progress safe: an unfinished AVK mode is a flag nobody sets, and the GLES path
stays byte-identical.

### Facts already established (do not re-derive)

- wlroots 0.20 headers: `/usr/include/wlroots-0.20/wlr/`. A 0.21 tree is also
  installed and a source checkout sits at `~/wlroots`; asteroidz builds against
  **0.20**.
- Scene node types are in
  `subprojects/asteroidz-scenefx/include/scenefx/types/wlr_scene.h:60` —
  `TREE`, `RECT`, `BUFFER`, plus scenefx's `SHADOW`, `OPTIMIZED_BLUR`, `BLUR`.
  The last three are M4; M3b must recognise and skip them with one warning, not
  silently drop them.
- Swapchain API (`/usr/include/wlroots-0.20/wlr/render/swapchain.h`):
  `wlr_swapchain_create()`, `wlr_swapchain_acquire()`,
  `wlr_swapchain_destroy()`. This is the least invasive way to get output
  buffers without writing an allocator (§ "Output buffer strategy").
- The three build sites to unify are `src/asteroidz.c:7654` (screenshot),
  `:7707` (HDR pending change) and `:7724` (ordinary), plus
  `src/ext-protocol/tearing.h:101`.

### Order of work

1. **Runtime switch.** `ASTEROIDZ_RENDERER` read once in `setup()`. Log
   `Asteroidz rendering backend: AVK native Vulkan` and
   `wlroots compatibility renderer: GLES2` — the second line must not imply
   GLES participates in composition.
2. **`az_output_build_frame(Monitor *, struct wlr_output_state *,
   const struct az_frame_options *)`** in `src/render/az_output.c`. All four
   sites above call it. GLES backend = today's
   `wlr_scene_output_build_state()`; AVK backend = steps 3-6.
3. **Output target.** Per-monitor `wlr_swapchain`; `wlr_swapchain_acquire()`
   per frame; `wlr_buffer_get_dmabuf()` → import through M2 as a render target
   (`for_render = true`); cache the `avk_image` per `wlr_buffer`;
   `wlr_output_state_set_buffer()`. Assert the target is not reacquired while
   the backend still owns it.
4. **Scene walker.** Recurse the scene tree accumulating x/y, emit
   `AVK_CMD_RECT` and `AVK_CMD_TEXTURE` in tree order. Painter's algorithm is
   *correct* without occlusion culling — occlusion is an optimisation, so the
   first cut may skip it. Same for damage: **full damage per frame is
   acceptable for the first working version** and must be logged as an AVK-mode
   regression, then fixed immediately after.
5. **Client buffer cache.** `wlr_buffer` → `avk_image`, keyed on the buffer
   pointer, with a `wlr_buffer.events.destroy` listener and retirement on the
   AVK timeline. DMA-BUF via M2; SHM via `wlr_buffer_begin_data_ptr_access()`
   and the staging path. **Never `wlr_texture_from_buffer()`.**
6. **Sync.** First cut may rely on implicit sync on the dmabuf — that is not a
   CPU wait and is what wlroots does when timelines are unavailable. The
   explicit bridge (exportable binary semaphore → `vkGetSemaphoreFdKHR`
   sync_file → output wait timeline → KMS `IN_FENCE_FD`) follows.
7. **Software cursor.** `wlr_output_add_software_cursors_to_render_pass()` is
   forbidden in AVK mode. Emit the cursor as an AVK command instead. Do not
   ship a disappearing cursor.
8. **DMA-BUF feedback** rebuilt from AVK's capability table, so clients are not
   told about modifiers AVK cannot import.

### Acceptance gate for the first slice

Compositor boots with `ASTEROIDZ_RENDERER=avk` on a **headless** output and
`contrib/anim-test.sh` captures a correct frame. Only after that does it go
near a real display, and only with the user watching — a new renderer under an
unattended session is exactly what that standing rule exists for.

### Break tests still owed (from the M3 brief)

A: poison `wlr_renderer_begin_buffer_pass()`, AVK frame still correct.
B: disable explicit-modifier import → explicit DMA-BUF test fails.
C: disable the MOD_INVALID fallback → Electron-style test fails.
D: retire an imported client image early → lifetime test fails.
E: reuse an output target before presentation release → assertion fires.
F: remove damage expansion for a moved surface → damage test fails. **(F is
already done and passing at the engine level; it needs redoing at the
compositor level once damage is real.)**

## 5.7 M4B — the border becomes one primitive, and a derivative stops lying

M4A handed over a defect with an address: `az_avk.h` carried the border's inner
box and threw away `clipped_region.corners`. The fix is small and the second
bug it uncovered is not.

**The border is now an annulus, not a rect with a hole punched by pixman.**
`struct avk_cmd` carries `inner` and `inner_corners` beside `dst` and
`corners`; the push constants carry both rectangles and both sets of radii in
output pixels, clockwise TL/TR/BR/BL; and the fragment shader evaluates

```text
border_coverage = outer_coverage * (1 - inner_coverage)
```

with the same SDF and the same derivative-scaled antialiasing on both edges, so
the two arcs of a ring cannot drift apart. The scissor region keeps the part it
can express exactly and nothing else. Geometry and material are deliberately
separate: M4C replaces the solid colour without touching any of this.

**Push constants are now exactly 128 bytes**, the guaranteed minimum, which
took removing the NDC copy of the destination rectangle. The vertex shader
derives NDC from `round_box` and the viewport, so the rectangle a command
covers and the rectangle its distance field measures are the same four numbers
by construction. There is no room left, and that is the right ceiling: M4C's
gradients carry a variable number of stops and want a buffer.

**The inner radius rule is the compositor's, not the renderer's.**
`apply_border()` computes `max(border_radius - bw - 1, 0)` per corner and AVK
consumes it. The `-1` is deliberate. It was tested rather than trusted: undoing
it changed the seam without opening it, so no break switch was added for it —
the brief's own condition. The inner rectangle is likewise taken from
`clipped_region.area` and never re-derived as a symmetric inset, because
`apply_border()` compensates it against the monitor edge.

**The second bug.** `az_rounded_coverage()` computed `fwidth(dist)` after a
per-pixel early return. A quad derivative read across a branch that some
invocations of the quad took is undefined, and it had been undefined for the
whole of M4A while rendering correctly. Calling the function a second time for
the inner edge was enough of a change to make it produce a one-pixel column of
border down the outside of a corner — at radius 9, and at radius 40 on a
scale-1.5 output, but not at radius 40 on a scale-1 one, and differently on
each run of identical input.

That last property is what identified it. A geometry bug does not move between
runs; two consecutive captures of the same scene disagreeing is a statement
about undefined behaviour, not about arcs. Chasing it as geometry cost two
falsified hypotheses first — the scissor's rounding direction, and the `-1`
underlap — both of which were tested and rejected on measurement rather than
argued away.

**Results**, `contrib/avk-border-test.sh`, wallpaper visible inside the ring:

| case | AVK | GLES |
|---|---|---|
| radius 40 / border 6 | 0 | 0 |
| radius 0 (square frame) | 0 | — |
| border width 1, 2, 4 | 0 | — |
| width 18 ≈ radius 20 | 0 | — |
| width 20 > radius 12 | 0 | — |
| radius 9 / border 3 | 0 | 0 |
| radius 120 / border 8 | 0 | — |
| titlebar (TL squared, three rounded) | 0 | — |
| scale 1.5, and scale 1.5 + titlebar | 0 | 0 |
| transform 180 + titlebar | 0 | — |
| opacity 0.75 | 0 | — |
| inactive border colour | 0 | — |

`BREAK=border-square-inner` restores the defect and every rounded case fails
against it — except `width 18 ≈ radius 20`, where the inner radius is 1 and
square and round are the same shape to within a pixel. That case is recorded as
giving no coverage against this break rather than counted as if it did.

Transforms 90 and 270 are **not covered**: grim returns no capture from a
rotated headless output. 180 does capture, and carries titlebar-asymmetric
corners, so a build that permuted the outer radii and not the inner ones would
open its ring there. Outer and inner go through one call to
`az_avk_corners_from_scenefx()`, so they cannot be permuted differently without
editing the line that does both.

## 5.8 M4I — the background is blurred once and kept

The accepted root cause of the transition tail was "a monitor-sized optimized
blur node is recomputed live every frame". Read from the source that could not
be what happened: `az_avk.h` skipped `WLR_SCENE_NODE_OPTIMIZED_BLUR` outright,
so the monitor node cost nothing at all.

The real waste was the other direction. 51,632 of 106,062 blur nodes asked for
the cached bottom layer, were refused, and each privately replayed the scene
prefix and ran its own dual-Kawase chain — 8.93 Mpx of capture and 5.16 Mpx of
replay per frame on an 8.29 Mpx display, all of it reproducing a blur of an
unchanged wallpaper. `blur_optimized 0` had looked exonerating because it stops
nodes ASKING, not the work.

**Two images per output, and no more.**

    PLAIN = blur(background)                       window backdrops
    DARK  = min(blur(background), background)      shadow backdrops

`DARK` exists because a blur is an average, and averaging bright detail over
dark ground raises the mean. The clamp is blend state — `VK_BLEND_OP_MIN`
against the destination — so it is only available while the chain writes back
into the image that still holds its own unblurred source. The rebuild therefore
replays the prefix **directly into the cached image** and chains in place. A
first version chained transient → cache, which would have shipped an unclamped
shadow backdrop: `src != dst`, `avk_blur.c`'s `darken && src_resource ==
dst_resource` silently false, and the glow defect back.

**Validity is identity, never damage.**

    FRAME DAMAGE != CACHE SOURCE DIRT

A monotonic per-output generation, plus geometry, kernel and format. Ten
full-output damage cycles rebuild nothing; a background-layer commit rebuilds
once. That separation is the whole architecture.

**Live, DP-1, interleaved OFF/ON/ON/OFF, 10 tag switches per arm:**

| | p50 | p95 | p99 | max | >6944 |
|---|---|---|---|---|---|
| OFF | 6 | 6249 | 6937 | 10023 | 31 |
| ON | 6 | **3252** | **4017** | **4817** | **0** |

Blur leaves the tail almost entirely: in the slowest 1%, 3972 → 909 µs. `post`
is untouched at 3343 µs and becomes the whole remainder.

### What the milestone's own instruments got wrong

**The alternation was redundant.** Two slots per kind guarded against a rebuild
writing the image the previous frame samples. `AZ_BLUR_CACHE_UNSAFE_REUSE`
removed the alternation and reported ZERO sync hazards with `validate_sync`
demonstrably watching. `avk_graph.c` is why: an image's layout persists on the
`avk_image` across frames, so declaring the cache as COLOR_WRITE out of
SHADER_READ_ONLY_OPTIMAL emits a transition, and a barrier's first
synchronisation scope covers everything already submitted to the same queue —
of which this device has one. One image per kind, and DP-1 went 132.7 → 66.4 MB
of logical texels.

**One shared kernel field validated two images.** The dark image compared
against the plain one's kernel, differed every frame, and rebuilt **383 times in
13 seconds of an idle desktop**. The reason table reported `dark=PARAMS` on
every line, which made it a two-minute diagnosis — and the headless suite passed
18/18 on both sides of the fix, because it had no shadow backdrops to build a
dark image for.

**Every output kept whichever node it walked last.** `layers[LyrBlur]` is one
global layer holding every monitor's node and the walk covers the whole scene,
so HEADLESS-2 built its cache at bounds `(-1280,0)` — monitor 1's area in
monitor 2's pixels. Quieter half: `ob->dirty` is an edge cleared by the first
observer, so the second output never incremented its generation and would blur a
wallpaper it no longer had, indefinitely. Matched by node **identity** now: a box
comparison agrees until two outputs share an origin or a size, which is what a
mirrored pair is.

**Two rebuilds in one frame wrote the same timestamp queries.**
`VUID-vkCmdWriteTimestamp2-None-03864`. The rebuild path now follows the rule
the consumer chains already had: phase marks belong to the first blur work only,
and `BLUR_END` is moved onto whichever chain declared last.

### The fixtures, and four ways they passed while measuring nothing

`contrib/avk-blur-cache-test.sh` (26), `-dirty.sh` (18), `-multi.sh` (21),
`avk-blur-role-split.sh` (6).

- **Tiled windows covered the background layer**, so the mutation under test was
  invisible — premise 0 px.
- **Floating them removed every consumer**: `blur_cached = config.blur_optimized
  && !c->isfloating`.
- **The background was flat**, so `STALE_PARAMS` rendered a pixel-identical
  desktop. Blurring one colour gives that colour at any radius.
- **`shadow only-floating` defaults to 1**, so a tiled fixture has no shadow
  tree, no dark consumer, and 25 green assertions over half a cache.

## 5.9 M4J — the POST audit, and one candidate

Six checks against the per-primitive fragment ledger. Five came back clean with
numbers: shadow shades **6%** of its envelope and border **1%** of its outer
rect (`area` is already `dst ∩ clip ∩ damage`); `implicit_copy_bytes = 0`;
overdraw is 1.13× of target; `az_rounded_coverage` returns 1.0 on a uniform
push-constant test that cannot split a quad.

The sixth is real. `AZ_BLEND_REPLACE` existed for exactly two consumers — the
blur's down and up passes — so every content and rect draw blends, including
ones the renderer already proves opaque each frame for occlusion culling.
Content is 21.3% of post.

`AZ_AVK_OPAQUE_NOBLEND=1` (off by default) sends draws opaque over their WHOLE
footprint to a blend-free pipeline. `az_cmd_fully_opaque()` is deliberately
stricter than `az_cmd_opaque_region()`: that one handles a rounded draw by
insetting the region it reports, and a pipeline bound once for the draw cannot
inset anything. Headless: −0.25%, bit-exact. Retained for one live arm; delete
if it does not clear 5%.

**The pixel oracle was lying.** The first A/B reported 2,574 differing pixels.
Two runs with byte-identical environments differ by the *same* 2,574 pixels in
the same 429×6 strip — the **titlebar text**, present in one capture and absent
from the other. `contrib/lib/headless.sh` sets `titlebar { enable 1 }`, so every
fixture that compares captures and does not override it carries that noise
underneath. With it off, two identical runs differ by 0 px. The fixture now runs
one arm twice as a control and asserts the noise floor first.

Second silent failure from the same chase: a 3840×2160 PPM is 24.9 MB and was
being copied before the compositor finished writing it. `ppm.py` refused the
truncated file and the oracle printed `-1`, which reads as a failure rather than
as "there was no picture".

## 5.10 The GLES floor is accepted as UNVERIFIED

Three instruments, each tried and each measured to be incapable:

1. **GPU timestamps are an AVK instrument.** A matrix that ran GLES arms
   produced a neat, well-formatted `NO TRACE ROWS` for every one.
2. **Client frame callbacks are vblank-scheduled**, not render-completion
   scheduled. `contrib/wlrepaint` reads 61.9 and 62.0 fps — at 8 windows and at
   20, at 60 Hz and at 1000 Hz. Three loads, one number; "AVK / GLES = 1.00×"
   was the frame scheduler read twice.
3. **Compositor CPU is 0.008 s against 0.009 s** over an 18-second run. Not a
   tie: both arms doing so little CPU work that there is nothing to compare,
   consistent with ~80 µs of record per frame. The work is on the GPU.

Establishing it needs the SceneFX path instrumented, which buys a comparison
against a renderer asteroidz no longer uses. **Declined.**
`contrib/avk-gles-floor.sh` is kept and kept FAILING its own premise, as the
record of why rather than as a gate. Do not relax the premise to make it green.

Two artefacts of that chase, recorded so they are not rediscovered:
`utime+stime` from `/proc/<pid>/stat` is in 10 ms ticks and read `0.01` for both
renderers — one tick, a tie that was the counter's resolution (`schedstat` is
nanoseconds); and driving the output at 1000 Hz to unpin the callback rate made
the compositor wake on a 1 ms frame timer and burn 99.8% of a core to complete
66 frames whose GPU time totalled 41 ms — the fixture's own knob, and exactly
the kind of number that gets quoted as a renderer property.

## 5.11 The AVK suite register

`contrib/avk-suite.sh` holds a disposition for all 62 suites (required / perf /
live / manual) and fails on two conditions: a registered suite that is absent or
not executable, and a discovered `avk-*.sh` with no disposition. The second is
the half that keeps working — a static list decays the moment somebody adds a
file.

It exists because `avk-blur-required-test.sh` shipped without its executable bit
and nobody noticed: nothing enumerated these suites, and a suite that cannot
execute is indistinguishable from a suite that was not run. The first audit
found **three** non-executable suites, not one. Both halves were falsified
before being trusted.

### 5.10a The matrix, re-measured on a config that parses

The table in `05b7b74` was withdrawn: it ran while the harness config was being
rejected wholesale, so it measured a desktop with no blur and no shadows. With
the config fixed and `hl_start` guarding against a rejected one:

| scenario | n | p50 | p95 | p99 | max | VUID | sync | waits |
|---|---|---|---|---|---|---|---|---|
| idle | 15 | 15309 | 15371 | 15371 | 15371 | 0 | 0 | 0 |
| move | 527 | 4905 | 5333 | 5394 | 5440 | 0 | 0 | 4 |
| resize | 527 | 10765 | 13855 | 14978 | 15344 | 0 | 0 | 5 |
| tag | 335 | 13548 | 14872 | 14936 | 15327 | 0 | 0 | 0 |
| multiblur | 15 | 17638 | 17762 | 17762 | 17762 | 0 | 0 | 0 |

Twenty to thirty times the withdrawn figures, which is the measure of how much
of that scene was missing.

**These are not budget numbers and the fixture says so on every run.** This GPU
idles near 50MHz; the same work on a clocked display is a fraction of it, and
the `>budget` columns are printed only so that a live run has the same shape to
fill in. What transfers is the ORDERING and the correctness columns:

- `move` is the cheapest scenario, not the most expensive — a floating window
  moving damages a small region, and the damage model is what makes that true.
- `idle` is expensive because it is damage-driven by construction: every cycle
  forces a full-output redraw with every effect on. A genuinely idle compositor
  renders nothing, and a percentile over zero frames is not a measurement.
- `multiblur` is the ceiling, as it should be.
- 0 VUID and 0 sync hazards in every scenario. The 4-5 CPU waits under `move`
  and `resize` are the ring legitimately blocking on a 50MHz GPU doing real
  blur work; the withdrawn run reported 0 because it was doing none.
