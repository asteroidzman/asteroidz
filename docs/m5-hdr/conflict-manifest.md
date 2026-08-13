# M5 integration conflict manifest

PERFORMANCE OWNERSHIP is per the current freeze list (blur execution, prefix
reuse, damage regions, per-level scissors, multi-rect execution,
passes/barriers, cross-output rounded corners). HDR STATUS: IMPLEMENTED
(exists at 2c1c015) / ISOLATED (new M5 code that may land now without
touching owned paths) / DEFERRED (must wait for the performance freeze).

| PATH | HDR REQUIREMENT | PERF-OWNED | RISK | HDR STATUS | FABLE CONTRACT READY | OPUS READY |
|---|---|---|---|---|---|---|
| `src/render/color/az_color.{h,c}` (new) | colour-math library (C1) | NO | LOW | ISOLATED | YES (C1) | YES |
| `src/render/vulkan/shader/src/color.glsl` (new) | GLSL twin of C1 | NO | LOW | ISOLATED | YES (C1) | YES |
| `src/render/color/az_color_ref.{h,c}` + `tests/test-color-pipeline.c` (new) | CPU reference + SDR gate (C4) | NO | LOW | ISOLATED | YES (C4) | YES |
| `src/render/az_lum.h` (new) + `tests/test-lum-domain.c` | luminance-domain model (C2) | NO | LOW | ISOLATED | YES (C2) | YES |
| `src/config/rule-schema.h`, `src/config/parse_config.h` | `sdr-white-scale` / `hdr-gain` window rules | NO | LOW | ISOLATED | YES (C2) | YES |
| `src/render/az_output_color.h` (new) + `tests/test-output-color.c` | output-colour-state model (C3) | NO | LOW | ISOLATED | YES (C3) | YES |
| `src/render/vulkan/device/avk_phys.c`, `avk_device.{h,c}` | capability probes (C5) | NO | LOW | ISOLATED | YES (C5) | YES |
| `src/render/vulkan/dmabuf/avk_format_table.{c,h}` | scanout `_SRGB`-view probe (C5) | NO | LOW–MED (headers included by owned files) | ISOLATED | YES (C5) | YES |
| `src/render/vulkan/pipeline/avk_output_encode.{c,h}` + `shader/src/output_encode.frag` (new) | Path-B encode pass (C6) | NO | LOW | ISOLATED | YES (C6) | YES |
| `src/render/vulkan/pipeline/avk_pipeline.{c,h}` | texture decode variants, `_SRGB` descriptor pair (C7) | NO (additive API only) | MEDIUM | ISOLATED | YES (C7) | YES |
| `src/render/vulkan/image/avk_image.h` | second (`_SRGB`) view + descriptor cache | NO | MEDIUM | ISOLATED | YES (C7) | YES |
| `src/render/vulkan/shader/src/texture.frag` | decode variants | NO | MEDIUM (interlocks with avk_render pipeline selection) | ISOLATED (compile-tested, unwired) | YES (C7) | YES |
| `src/render/vulkan/scene/avk_scene.h` | `az_lum_domain` field on texture commands | NO, but forces owned-file recompiles | MEDIUM | **DEFERRED** | YES (C2) | blocked on freeze |
| `src/render/az_avk.h` | fill domains in scene walk; remove `image_description` refusal (az_avk.h:2405-2413) | **YES** | HIGH | **DEFERRED — PERFORMANCE OWNED** | YES (C2/C6 integration steps) | blocked on freeze |
| `src/render/vulkan/scene/avk_render.c` | Path A/B target selection, per-draw variant selection, encode-pass call, dither retarget | **YES** | HIGH | **DEFERRED — PERFORMANCE OWNED** | YES (C6/C7 + ADR-013 order) | blocked on freeze |
| `src/render/vulkan/effect/avk_blur.c` | blur-transient format follows output path (FP16 on Path B) | **YES** | HIGH | **DEFERRED — PERFORMANCE OWNED** | YES (ADR-012) | blocked on freeze |
| `src/render/vulkan/command/*` | timestamp zones for the encode pass (optional) | **YES** | MEDIUM | **DEFERRED — PERFORMANCE OWNED** | YES (trivial) | blocked on freeze |
| `src/render/vulkan/scene/avk_oracle.c` | oracle learns the two-stage frame | NO (not on the owned list) but exercised by owned code | MEDIUM | **DEFERRED** (pointless before avk_render wiring) | YES (C6 note) | blocked on freeze |
| `src/render/az_output.h` | none in M5 (frame options already carry what is needed; ICC predicate unchanged) | **YES** | LOW | IMPLEMENTED (no change needed) | n/a | n/a |
| `src/asteroidz.c` — `mon_state_apply_color`, `hdr_resolve`, fold-in commit | unchanged connector policy; later: call C3 derivation on state change | NO | LOW | IMPLEMENTED (policy) / DEFERRED (C3 wiring, trivial) | YES (C3) | YES (model), wiring with step 2 |
| `subprojects/asteroidz-scenefx/types/scene/wlr_scene.c` | none — scenefx already populates scene-buffer colour fields C2 reads | **YES** | LOW | IMPLEMENTED | n/a | n/a |
| `src/dispatch/bind_define.h` — screenshot tonemap | add missing 2020→709 matrix via C1 (audit §4) | NO | LOW | ISOLATED (nice-to-have) | YES (ADR-008 note) | YES |
| fx_vk (`scenefx/render/fx_renderer/vulkan/*`) | remains the ICC + pre-integration HDR fallback; no M5 edits | NO (but M9 deletes it) | LOW | IMPLEMENTED | n/a | n/a |
| `contrib/` harnesses (`anim-test.sh`, `render-matrix-test.sh`, regression suite) | SDR/HDR matrix already exists; add M5 scenes when integration lands | NO | LOW | DEFERRED (with step 5/7) | partial (tests named in C4/C6) | blocked on freeze |

Summary: **everything in contracts C1–C7 except the final wiring is
implementable immediately without touching a performance-owned file.** The
deferred set is exactly: `avk_scene.h` field, `az_avk.h` walk + refusal
removal, `avk_render.c` path/variant/pass wiring, `avk_blur.c` transient
format, oracle extension, harness scenes — sequenced in contracts.md
"Integration order".
