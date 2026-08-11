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
- M3 — native scene submission: **the renderer half is done; the compositor
  half is not.**
  - **Done.** `src/render/vulkan/scene/` + `pipeline/` + `shader/`: an
    Asteroidz-owned command IR (`avk_scene`), two pipelines over one
    push-constant block, dynamic rendering, premultiplied source-over,
    source crop, destination scale, all eight Wayland transforms, per-command
    visible-region clipping, per-command opacity, damage-scissored drawing,
    and the X-format opaque-alpha fix. `tests/test-avk-render.c` asserts all
    of it on read-back pixels (37 checks). No `wlr_render_pass` anywhere on
    this path, enforced by `check-vulkan-isolation.py`.
  - **Not done.** `az_output_build_frame()`, the `ASTEROIDZ_RENDERER` switch,
    the wlr_scene → snapshot builder, the client-buffer cache keyed to
    `wlr_buffer` lifetime, the render-completion → sync_file → KMS
    `IN_FENCE_FD` bridge, software cursor, output-target lifecycle against
    presentation, DMA-BUF feedback from AVK's capability set, and the live
    session boot. **AVK does not yet render a desktop.**
- M4-M10: not started

## Pre-existing test failures (not caused by this work)

`meson test` reports `config-schema` with 22 failures: values above the
maximum are not clamped for `animation_duration_tag`, `animation_duration_focus`,
`overviewgappi`, `overviewgappo`, `hotarea_size`, `cursor_size`,
`cursor_hide_timeout` and others. **Verified against the pre-AVK tree** by
stashing this work and rebuilding: same 22 failures, same 1022 checks. Nothing
in the Vulkan work touches config parsing. Recorded here so scope attribution
stays clean; deliberately not fixed as part of the renderer migration.
