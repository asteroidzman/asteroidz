---
title: Architecture
description: How AVK — asteroidz's own Vulkan renderer — is put together.
---

# Architecture

asteroidz renders with **AVK**, a Vulkan renderer it owns outright. There is no
second renderer to fall back to: `az_avk_build_frame()` builds every frame, and
when it cannot, it aborts rather than degrade.

This page is the shape of the thing. The decisions it obeys are in
[decisions](/docs/decisions); how it came to look like this is in
[history](/docs/history); the code is the authority for anything below.

## The frame path

```
Wayland client
  │  wl_buffer  (wl_shm | zwp_linux_dmabuf_v1 v5)
  ▼
wlr_buffer ──► avk_image        AVK's own DMA-BUF import
  │
  ▼
wlr_scene node tree             src/scene/wlr_scene.c — asteroidz source
  │   layers[NUM_LAYERS], per-client trees, shadow trees, blur nodes,
  │   text nodes. Absorbed from scenefx; its render half was deleted
  │   with it, because nothing could reach it once AVK built the frame.
  ▼
az_avk_build_frame()            src/render/az_output.h
  │   AVK walks the tree itself: damage, occlusion culling, direct scanout
  ▼
GPU work → output buffer (wlr_swapchain, wlr_allocator/gbm)
  ▼
wlr_output_state { .buffer, .signal_timeline, HDR image_description }
  ▼
wlroots DRM backend → atomic KMS commit → connector
```

The compositor's coupling to the renderer is deliberately thin — one
`wlr_renderer *`, one `wlr_allocator *`, and a short list of creation and
commit sites. The GPU architecture lives behind `wlr_scene_output_build_state`,
not in `asteroidz.c`.

## Layout

```
src/render/vulkan/
  device/    instance, device, caps, queue, DRM device matching
  memory/    allocator, persistent staging ring, budget
  image/     images, views, explicit layout tracking
  dmabuf/    import, modifiers, format table, feedback
  sync/      timelines, fence FDs, syncobj
  command/   pools, command-buffer ring, deferred destroy, timestamps
  pipeline/  pipelines, cache, descriptors
  shader/    GLSL sources and SPIR-V build rules
  scene/     scene snapshot, draw commands, the render graph
  effect/    rounded corners, shadow, blur, gradient
  encode/    Vulkan Video H.265, MP4 and HEIF containers
  color/     transfer functions, gamut, tone map, LUT/ICC
```

**Hard rule, enforced by Meson:** nothing under `src/render/vulkan/` may include
an EGL, GLES or `wlr_renderer` header.

## DMA-BUF import

Own importer: per-plane FDs, offsets and strides, explicit modifiers, disjoint
and multi-plane images, 8- and 10-bit RGB. The capability table is built from
`VK_EXT_image_drm_format_modifier`, with per-modifier probing so it records
actual limits rather than assumptions.

`DRM_FORMAT_MOD_INVALID` gets an ordered ladder, and **never** "assume LINEAR" —
that displays corrupt tiled memory as though it were pixels:

1. **Recover the real modifier** through `gbm_bo_import` + `gbm_bo_get_modifier`.
   Measured and it does not work on Mesa: a tiled buffer round-tripped through
   the legacy path comes back reporting `MOD_INVALID`, because the implicit
   import discards the modifier rather than re-deriving it. So this rung is
   probed at startup with a real allocation and used only where it works —
   capability detection, not a vendor branch.
2. **Copy through a driver-detiled mapping** — `gbm_bo_map()` with
   `GBM_BO_TRANSFER_READ` gives a linear view, then a staged
   `vkCmdCopyBufferToImage2`. Costs a surface read plus an upload, and works
   regardless of what rung 1 says.
3. **Prevent it at the source** — DMA-BUF feedback tranches computed from what
   this importer can actually import, so conforming clients never allocate an
   unimportable buffer. This is the real fix; the first two are for clients that
   ignore feedback.

## Colour

Two output paths, chosen per output and resolved on output-state change rather
than per frame:

- **Path A (direct sRGB)** — `XRGB8888`, no image description, no ICC transform,
  and a scanout modifier that admits a mutable `_SRGB` view. Composition goes
  straight through that view, so scene → wire is identity for content in [0,1].
- **Path B (encode)** — everything else, and always for HDR. A per-pixel encode
  pass: tone map → gamut matrix → clamp → inverse EOTF → dither → write.

The command stream never mentions an output. See ADR-001 through ADR-012.

## Presentation

Each output owns a `struct az_presenter`, embedded in `Monitor`. It predicts the
instant the frame being built will turn into light, in one of two regimes fixed
per epoch — FIXED predicts the next vblank from the observed period, VRR cannot
because the panel free-runs.

One sample instant per render pass is threaded explicitly down the draw path;
animation code may not read a clock. A settled desktop costs zero presenter
wakeups — it owns no event source at all. See ADR-600 through ADR-617.
