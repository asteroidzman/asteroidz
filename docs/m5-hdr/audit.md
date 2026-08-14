# Colour-pipeline audit at `2c1c015`

Every claim is classified **OBSERVED / OBSERVED-LIVE / CONFIGURED / ASSUMED /
UNKNOWN** (see README.md). File paths are relative to the repository root;
`scenefx/` abbreviates `subprojects/asteroidz-scenefx/`.

There are two renderers in play and they have *different* colour models:

- **AVK** (`ASTEROIDZ_RENDERER=avk`, the active engine): composites entirely
  in the encoded (electrical) domain and does no colour management at all.
- **fx_vk** (the SceneFX Vulkan renderer, `WLR_RENDERER=vulkan`): the
  per-output fallback AVK hands a frame to whenever the output carries an ICC
  transform or an HDR image description. It has a real, working scene-linear
  two-pass colour pipeline.

The live session therefore runs a *split* pipeline today: SDR outputs are
AVK-composited in encoded sRGB; the moment an output goes HDR (or gains an ICC
profile) every frame on that output is built by fx_vk instead. CONFIGURED
(live env) + OBSERVED (the dispatch below).

---

## 1. AVK: every piece of arithmetic is in the encoded domain

### 1.1 Sampling: no decode, ever

Client images are imported with `image->format = fmt->vk`, always the UNORM
Vulkan format (`src/render/vulkan/dmabuf/avk_dmabuf.c:356`,
`src/render/vulkan/dmabuf/avk_drm_format.c:22-56`). The sampled image view is
created with `.format = image->format`
(`src/render/vulkan/pipeline/avk_pipeline.c:494-509`). No `_SRGB` view is
ever created, so the sampler returns raw encoded (electrical) values.
**OBSERVED.**

The machinery for hardware decode already half-exists and is dormant: dma-buf
images are created with `VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT` and a
`VkImageFormatListCreateInfo` naming both the UNORM and `_SRGB` formats when
the modifier supports it (`avk_dmabuf.c:388-401`), and the format table probes
and records `srgb_mutable` per modifier (`avk_format_table.c:160-186`,
`avk_format_table.h:38-40`). Nothing consumes it. **OBSERVED** (a deliberate
M5 hook, per the header comment).

Whether `srgb_mutable` is actually true for the modifiers that matter on this
GPU — in particular AMD DCC-compressed modifiers on client buffers and on
scanout buffers — is **UNKNOWN** until the probe log is read on this machine.
The SDR fast path in ADR-001/ADR-007 depends on it and has a fallback if it
is false.

### 1.2 Blending: fixed-function over encoded values

All composite pipelines blend `(ONE, ONE_MINUS_SRC_ALPHA)`
(`avk_pipeline.c:134-139`) into a target view created with the target's own
UNORM format (`src/render/vulkan/scene/avk_render.c:1301-1318`). Since both
source and destination are sRGB-encoded electrical values, **every blend in
AVK is arithmetic on encoded values**. `docs/vulkan-native-architecture.md:349-352`
states this as a deliberate M3/M4 choice, to be changed by M5 "deliberately
rather than by accident". **OBSERVED.**

The alpha convention is premultiplied throughout — client buffers arrive
premultiplied (in the encoded domain, per Wayland convention), solid colours
are premultiplied, opacity multiplies the whole vec4
(`src/render/vulkan/shader/src/texture.frag:27-43`, `quad.frag:6-8`).
`wlr_scene` rect colours arrive premultiplied and AVK un-premultiplies them
into its command's straight colour (`src/render/az_avk.h:1992-2000`).
**OBSERVED.**

X-format alpha (XRGB8888 etc.) is forced to 1.0 in the shader via
`params.y` (`texture.frag:29`, `avk_image.h:52-54`). **OBSERVED.**

### 1.3 Effects: encoded-domain, with one explicit linearisation island

- **Blur** (dual-Kawase, 5-tap down / 8-tap up,
  `src/render/vulkan/shader/src/blur.glsl`, `blur_down.frag`, `blur_up.frag`):
  taps average *encoded* values. Blur transients are allocated at
  `renderer->format` — the output's own UNORM format
  (`avk_render.c:1719,1898-1901`, `src/render/vulkan/effect/avk_blur.c:634`).
  A blur of encoded values is mathematically a different filter than a blur of
  light; the shadow-glow saga (mean of encoded text-on-dark rises under blur;
  fixed by the darken-only clamp, commit f8be42c) is a direct consequence.
  **OBSERVED.**
- **Blur saturation** is the one place AVK linearises anything: it does
  `pow(rgb, 2.2)` → Oklab chroma scale → `pow(rgb, 1/2.2)`
  (`blur.glsl:117-135`). Note it assumes **pure gamma 2.2**, not piecewise
  sRGB, and the comment says the round-trip becomes identity under M5.
  **OBSERVED.**
- **Blur brightness/contrast/noise** are applied to encoded values, shaped to
  keep black at black precisely so they survive an HDR output transform
  (`blur.glsl:107-135`). **OBSERVED.**
- **Shadows** are analytic coverage (an alpha ramp); colour is the shadow's
  premultiplied encoded colour. The M4D dither is an *alpha-domain* dither
  whose amplitude is calibrated against an encoded reference backdrop
  `AVK_DITHER_REF_BACKDROP = 45/255` (`avk_render.h:27-100`). The amplitude
  function returns 0 for `R16G16B16A16_SFLOAT`, explicitly anticipating M5
  (`avk_render.h:87-91`). **OBSERVED.**
- **Gradients** interpolate premultiplied encoded colours
  (`az_avk.h:1955-1988`). **OBSERVED.**

### 1.4 AVK's colour-capability boundary

`az_avk_output_supported()` refuses any output whose frame needs a colour
transform: `color_transform != NULL` (ICC) **or**
`output->image_description != NULL` (HDR) sends the frame to the SceneFX
path, logged once as "colour management is M6"
(`src/render/az_avk.h:2394-2413`). So today **AVK never renders an HDR
frame**; the HDR path below is entirely fx_vk. **OBSERVED.**

AVK maintains one renderer instance per output VkFormat
(`az_avk.h:1076-1099`); the formats that occur are `B8G8R8A8_UNORM`
(XRGB8888) and `A2R10G10B10_UNORM_PACK32` (XRGB2101010, from the
`bitdepth:10` rule). A 10-bit SDR output is therefore composited by AVK in
encoded values at 10 bits, dithered accordingly (`avk_render.h:71-100`).
**OBSERVED.**

### 1.5 AVK colour state model: none

`avk_scene.h` / the `avk_cmd` struct carry **no transfer function, no
primaries, no luminance fields of any kind** (grep over
`src/render/vulkan/` and `az_avk.h`: zero hits for
`transfer_function|primaries|color_encoding` outside the dmabuf format
table). The scene walk never reads `wlr_scene_buffer`'s colour description
fields. YCbCr formats are importable (NV12 etc.,
`avk_drm_format.c:68-71`) but deliberately not advertised because
range/matrix/transfer are unimplemented
(`docs/vulkan-native-architecture.md:1683`). **OBSERVED.**

### 1.6 Dithering in AVK, summarised

Shadow-local IGN dither only (`shadow.frag`, `dither.glsl`), amplitude from
the attachment format, zero for FP16. `docs/avk-effects.md:1709-1730` already
records the M5 intent: display-quantisation dither belongs at the final
output-encode stage, `az_dither_alpha()` moves there unchanged, and
`AVK_DITHER_REF_BACKDROP` is deleted. **OBSERVED.**

---

## 2. What `blend-space "srgb"` actually does

Config: `effects/blend-space` = `linear` (default) | `srgb`, stored as
`config.srgb_blending` (`src/config/config-schema.h:107-110,465-471`,
`src/config/parse_config.h:368-374,1802-1806,4934-4936`). Applied via
`fx_renderer_set_srgb_blending(drw, …)` at startup and on reload
(`parse_config.h:5240`, `src/asteroidz.c:9682`).

- For **GLES** it is a no-op — GLES already blends encoded values
  (`scenefx/render/fx_renderer/vulkan/renderer.c:75-90`, comment). **OBSERVED.**
- For **AVK** it is never consulted (no reference under `src/render/vulkan/`).
  AVK always blends encoded regardless of the setting. **OBSERVED.**
- For **fx_vk** it selects encoded-domain blending *only when the pass carries
  no colour transform*: `pass_blends_encoded = srgb_blending &&
  color_transform == NULL` (`scenefx/render/fx_renderer/vulkan/pass.c:95-103`),
  mirrored on the texture side (`pass.c:984-996`) and the output side
  (`pass.c:383-388`). On an HDR/ICC output the preference is overridden and
  fx_vk linearises anyway. **OBSERVED.**

So on this machine, with AVK active: `blend-space` affects only fx_vk
fallback frames on non-colour-managed outputs — i.e. almost nothing.
**OBSERVED + CONFIGURED.**

---

## 3. The existing HDR path (fx_vk), end to end

### 3.1 Policy: who turns HDR on

`hdr_resolve()` (`src/asteroidz.c:10021-10055`) is the single writer of
`m->hdr`. Precedence, highest first: capture fallback OFF → `hdr_mode off`
OFF → capability-failure OFF → `hdr_mode on` ON → visible `force_hdr` client
ON → per-output `hdr` baseline. `force_hdr` is a per-window rule
(`src/config/rule-schema.h:344-347`), held ON while the client is *visible*
(not merely focused) to avoid modeset strobing on alt-tab
(`asteroidz.c:10007-10019`). **OBSERVED.**

Live configuration: HDR off globally, `force_hdr` on mpv only (per project
memory, shipped b030cf0). DP-1 is HDR-capable but currently SDR.
**CONFIGURED.**

### 3.2 Output state: `mon_state_apply_color()`

`src/asteroidz.c:5224-5337`. When `m->hdr`:

1. Requires the renderer feature `output_color_transform`, else HDR is
   refused ("sRGB content would be shown unconverted") and latched failed.
2. Requires the output to support BT.2020 primaries + ST2084 PQ.
3. Builds a `wlr_output_image_description`: BT.2020 + PQ, mastering
   luminance / MaxCLL / MaxFALL taken from the panel's values
   (`m->hdr_min_luminance`, `m->hdr_max_luminance`, `m->hdr_max_fall`, all
   per-output monitor-rule config, `parse_config.h:133-135`) — unless exactly
   one fullscreen client is visible and declares PQ metadata, in which case
   that client's own mastering/MaxCLL/MaxFALL is forwarded instead
   (`mon_hdr_scanout_candidate`, `asteroidz.c:5200-5218,5265-5287`; real
   wp-color-management first, frog as fallback).
4. HDR (or `bitdepth:10`) implies `DRM_FORMAT_XRGB2101010` render format,
   tested once and falling back to 8-bit (`asteroidz.c:5309-5327`).

**OBSERVED.**

### 3.3 The transition commit

An HDR flip is folded into the next frame's own commit with
`allow_reconfiguration = true` (blocking), because image description and
render format are modeset-only properties and the non-blocking commit was
rejected 100% of the time (`asteroidz.c:8054-8103`, comment block). On
failure it retrains (two modesets, visible flash). Project memory records
that on DP-1 the fold-in *still* always fails and every transition retrains.
**OBSERVED** (code) / **OBSERVED-LIVE** (the always-fails part).

### 3.4 Frame build: which transform goes in

`az_output_color_transform()` returns the ICC transform only when the output
carries **no** image description (`src/render/az_output.h:86-88`) — an HDR
output gets `color_transform = NULL` and scenefx builds the PQ transform
itself from `output->image_description`. AVK refuses (§1.4), so
`wlr_scene_output_build_state` → fx_vk renders the frame. **OBSERVED.**

### 3.5 scenefx scene layer: the luminance model

`scenefx/types/scene/wlr_scene.c`:

- `get_luminance_multiplier(src, dst) = (dst.ref / src.ref) * (src.max /
  dst.max)` (`wlr_scene.c:2592-2594`).
- wlroots 0.20.2 default luminances (`wlroots/render/color.c:408-433`):
  PQ `{min 0.005, max 10000, ref 203}`; BT1886 `{0.01, 100, 100}`; everything
  else including sRGB and GAMMA22 `{0.2, 80, 80}`. **OBSERVED** (system
  wlroots source, `/tmp/wlroots-0.20.2`).
- Per scene-buffer, the texture options get `luminance_multiplier =
  get_luminance_multiplier(src_lum, srgb_lum)`; for PQ buffers `src_lum.ref`
  is overridden to the output's `blend_reference_luminance`
  (`wlr_scene.c:2896-2911`).
- `content_peak` (tone-map ceiling, in reference-normalised units) prefers the
  buffer's MaxCLL — **unreachable today**: wlroots rejects
  `set_luminances`, so `scene_buffer->max_cll` is always 0 (comment,
  `wlr_scene.c:2921-2940`) — then the *output's* MaxCLL/mastering peak, then
  the TF's absolute max (`wlr_scene.c:2941-2954`). **OBSERVED.**

Net effect — the fx_vk **blend space** is: *linear light, sRGB primaries,
premultiplied alpha, scene value 1.0 ≡ the output's blend reference
(= `sdr_reference_luminance`, default 203 cd/m²)*. An SDR white pixel decodes
to exactly 1.0; a 1000-nit PQ pixel decodes to 1000/203 ≈ 4.93. **OBSERVED**
(derivation: PQ decode yields 1.0 = 10000 nits; multiplier (80/ref)·(10000/80)
= 10000/ref).

### 3.6 scenefx output layer

`scene_output_combine_color_transforms()` (`wlr_scene.c:4072-4160`): builds
[3×3 sRGB→output-primaries, absolute colorimetric, scaled by
`get_luminance_multiplier(srgb, dst)` = ref/10000 for PQ] ∘ [optional
`sdr_saturation` Rec.709-luma matrix] ∘ [inverse EOTF] ∘ [optional user gamma
LUT], and records `blend_reference_luminance = dst.ref`
(`sdr_reference_luminance` when configured). Config:
`misc/sdr/reference-luminance` (0 = 203 default) and `misc/sdr/saturation`
(`parse_config.h:362-363,1784-1787`), plus a live keybind
`set_sdr_luminance` clamped 80–1000 (`src/dispatch/bind_define.h:786-800`).
**OBSERVED / CONFIGURED.**

### 3.7 fx_vk render pass: where the maths runs

Pathway selection (`scenefx/render/fx_renderer/vulkan/pass.c:3264-3345`):
no/other transform → **two-pass** (the default even for plain SDR);
`EXT_LINEAR` → one-pass linear; `SRGB` + existing sRGB framebuffer →
one-pass hardware-encode. Two-pass allocates a whole-output
`VK_FORMAT_R16G16B16A16_SFLOAT` blend image (`renderer.c:740-870`, format at
`renderer.c:785`). **OBSERVED.**

Texture draw (`pass.c:973-1100`, shader `shaders/texture.frag`):
un-premultiply → per-TF decode (piecewise sRGB / PQ (with the RADV-NaN clamp)
/ pure 2.2 / BT.1886) → × `luminance_multiplier` → 3×3 source-primaries→sRGB
(absolute colorimetric) → **hue-preserving extended-Reinhard highlight
rolloff driven by the max channel** against `content_peak`
(`texture.frag:72-96`) → re-premultiply → × opacity. Blend `(ONE,
1-SRC_ALPHA)` into the FP16 buffer. Note: **tone mapping happens per-source,
before compositing**, in this design. **OBSERVED.** (The project-memory note
that "the Vulkan path has NO rolloff and hard-clips" is stale — the rolloff
exists in this tree.)

Output draw (`pass.c:340-460`, shader `shaders/output.frag`): the combined
3×3 (or a 33³ 3D LUT for ICC) → inverse EOTF (PQ / gamma2.2 / sRGB / BT.1886
/ identity) → **IGN dither at one output-encoding quantum** (1/1023 for
10-bit, 1/255 for 8-bit, 0 for float targets, `pass.c:356-373`) →
damage-scissored draws into the scanout buffer. **OBSERVED.**

Untagged surfaces: `transfer_function == 0` defaults to **GAMMA22**
(`pass.c:973-975`); the frog shim maps a client's declared *sRGB* to GAMMA22
deliberately, citing the frog spec (`src/ext-protocol/frog-color-management.h:84-89`).
So on colour-managed outputs today, ordinary SDR content is decoded as pure
2.2 power. On AVK outputs it is not decoded at all. **OBSERVED.**

YCbCr in fx_vk: `VkSamplerYcbcrConversion` handles matrix/range; the decoded
R'G'B' then goes through the same per-TF decode. **OBSERVED** (pipeline key,
`pass.c:1023-1060`).

### 3.8 Blur in fx_vk HDR frames

`blur2.frag:75-79` documents ratio-based (black-preserving) effects because
"HDR PQ gives the darks enormous dynamic range". The fx_vk blur operates on
the FP16 blend buffer contents — linear values — on colour-managed outputs.
**OBSERVED** (not exercised further here; AVK's blur is the one that ships).

---

## 4. Adjacent paths that carry colour

- **Screenshot tonemap** (`src/dispatch/bind_define.h:3010-3095`): when the
  captured output is HDR, a software per-pixel PQ EOTF → ×10000/ref → sRGB
  OETF. It converts the transfer function but **not the primaries** — the
  buffer is BT.2020-encoded and the result is written as if sRGB, so HDR
  screenshots are visibly desaturated. Alpha `0` is treated as opaque
  (X-format padding). **OBSERVED** (the desaturation is arithmetic fact; its
  visibility is ASSUMED, easily confirmed against a saturated test frame).
- **HDR10 recording** exists via `screenshot_ui,rawhdr` + `contrib/hdr-record.sh`
  (genuine PQ capture). **OBSERVED-LIVE** (memory), not re-traced here.
- **frog preferred-metadata** advertises the first HDR-enabled output's PQ
  envelope (or SDR defaults) to gamescope
  (`frog-color-management.h:188-231`); gamescope's PASSTHRU copy-paste bug is
  worked around by clearing tf with primaries
  (`frog-color-management.h:112-129`). **OBSERVED.**
- **wp-color-management** is created with a small feature set; surfaces'
  descriptions reach the scene via scenefx (`asteroidz.c:9480-9507`).
  `set_luminances` is rejected upstream (see §3.5). **OBSERVED.**
- **ICC** per-output profiles load into `m->icc_transform`
  (`asteroidz.c:5150-5198`), applied only when not HDR (§3.4); on fx_vk they
  become a 33³ 3D LUT (`pass.c:3230-3256`). AVK refuses ICC outputs. **OBSERVED.**
- **Cursor plane on an HDR output**: the hardware cursor buffer is rendered
  by wlroots' own cursor machinery outside AVK/fx_vk frame builds. Whether
  its contents are PQ-encoded to match the connector state, or scan out as
  sRGB-encoded values misread as PQ (a too-dark cursor), is **UNKNOWN** —
  not traced, needs a live look when M5 turns HDR on. AVK's software-cursor
  path (`az_avk_emit_cursors`, forced by
  `ASTEROIDZ_AVK_FORCE_SOFTWARE_CURSOR`) would inherit whatever the composite
  does.
- **Frame oracle** (`AZ_FRAME_ORACLE=1`) renders every frame twice through
  the same AVK code and diffs (`src/render/vulkan/scene/avk_oracle.c`); it
  will need to learn any new output-encode pass or it will classify every
  HDR frame as an OUTPUT-stage divergence. **OBSERVED.**

---

## 5. Capability facts about this machine

- GPU: AMD RX 7900 XT (Navi 31), RADV. `R16G16B16A16_SFLOAT` is probed by
  fx_vk for its blend buffer (`renderer.c:3075`) and works — the fallback HDR
  path uses it today. That the same format supports
  `COLOR_ATTACHMENT_BLEND | SAMPLED_IMAGE_FILTER_LINEAR` for **AVK's** usage
  patterns (transients, regional targets) is **ASSUMED** (near-certain on
  RADV; contract C5 probes it anyway).
- FP16 *bandwidth* cost of compositing/blur at 4K on this GPU: **UNKNOWN**,
  to be measured (C5/C6 tests; the perf agent's timestamp infrastructure
  exists: `command/avk_timestamp.*`).
- Live outputs: DP-1 3840×2160@144 (HDR-capable, SDR now), HDMI-A-1
  1920×1080@60. **CONFIGURED.**
- Live session runs with `ASTEROIDZ_VK_DEBUG=1` (validation on); CPU-side
  costs measured live are inflated ~200× vs headless. **OBSERVED-LIVE.**
- Every HDR transition on DP-1 ends in a retrain. **OBSERVED-LIVE.**

---

## 6. The gap M5 closes, in one paragraph

Today the *only* scene-linear compositing this compositor does happens in a
renderer that is being retired (fx_vk), on exactly the outputs AVK refuses,
with tone mapping applied per-source before blending, a per-output FP16
buffer, and encoded-domain AVK everywhere else. M5 moves the scene-linear
model into AVK natively — with the corrections the fallback model needs
(output-stage tone mapping, per-window luminance domains, explicit
untagged-surface policy, output-stage dither) — so that one engine composites
one scene-linear scene and encodes it per output, SDR and HDR alike.
