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
