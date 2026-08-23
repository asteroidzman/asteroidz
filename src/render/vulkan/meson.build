# avk -- asteroidz's own Vulkan engine.
#
# Built as a static library rather than compiled straight into the asteroidz
# executable so that a) the tests can link it without dragging in the
# compositor, and b) its dependency list is a thing meson prints. The second
# reason is the important one: `meson introspect --dependencies` and the
# include check below are what keep the promise in
# docs/architecture.md that this subsystem does not depend on
# EGL, GLES or wlr_renderer. A promise nothing checks is a comment.

# libdrm and gbm are NOT renderer abstractions -- they are the kernel's own
# format/modifier vocabulary and the buffer allocator every driver on this
# platform speaks. gbm in particular is what recovers the tiling of a buffer
# whose producer never stated one, which is the difference between an Electron
# window and a blank rectangle. Neither drags in EGL: gbm's EGL integration
# lives in the EGL library, not in libgbm.
vulkan_dep = dependency('vulkan')
avk_libdrm_dep = dependency('libdrm')
avk_gbm_dep = dependency('gbm')
# pixman for regions. Not a renderer abstraction -- it is the region algebra
# the whole graphics stack already speaks, and damage handling that invented
# its own would have to be converted at every boundary.
avk_pixman_dep = dependency('pixman-1')
# libm: avk_blur.c derives the blur's support bound and rounds it outward, which
# is ceil(). Not a renderer abstraction; the C standard library's own maths.
avk_libm_dep = meson.get_compiler('c').find_library('m', required : false)
# pthreads: the SHM upload's memcpy runs on a worker thread so that a client's
# commit does not hold the compositor's event loop out of poll() -- see
# image/avk_upload_worker.h. Not a renderer abstraction either; the one
# dependency this engine has ever had a latency reason to take.
avk_threads_dep = dependency('threads')

# ── shaders ────────────────────────────────────────────────────────────────
# GLSL -> SPIR-V at build time, then embedded as a C array. Compiling at build
# time means a shader syntax error is a build failure rather than a compositor
# that starts and renders nothing; embedding means there is no install path to
# get wrong and no runtime file to be missing.
glslang = find_program('glslangValidator')
avk_embed = find_program('shader/embed.py')

avk_shader_headers = []
# [source, symbol, extra glslang args]. The third element is what lets ONE
# source produce two modules: M6C's cLUT variant is output_encode.frag compiled
# with AZ_ENCODE_CLUT defined, so that set 1 is declared as a sampler3D there
# and a sampler2D everywhere else. Two files would be two copies of ADR-008's
# step order to keep in step; see the comment in output_encode.frag.
foreach shader : [['quad.vert', 'quad_vert_spv'],
                  # P2: the same quad with its four corners placed
                  # independently, for primitives that are not axis-aligned.
                  # Its own vertex stage and deliberately NOT its own fragment
                  # stage -- it shares texture.frag, which is what makes a
                  # degenerate quad provably identical to a texture command.
                  ['quad_free.vert', 'quad_free_vert_spv'],
                  ['quad.frag', 'quad_frag_spv'],
                  ['texture.frag', 'texture_frag_spv'],
                  ['gradient.frag', 'gradient_frag_spv'],
                  ['shadow.frag', 'shadow_frag_spv'],
                  ['blur_down.frag', 'blur_down_frag_spv'],
                  ['blur_up.frag', 'blur_up_frag_spv'],
                  ['blur_soft.frag', 'blur_soft_frag_spv'],
                  # M5/C6: the output-encode pass. Its own vertex stage because
                  # it is the one pipeline with a different push-constant
                  # layout -- push.glsl's block is exactly the 128-byte
                  # guaranteed minimum and is already dual-purpose across four
                  # pipelines.
                  ['output_encode.vert', 'output_encode_vert_spv'],
                  ['output_encode.frag', 'output_encode_frag_spv'],
                  # M6C: the same source, one define apart.
                  ['output_encode.frag', 'output_encode_clut_frag_spv',
                   '-DAZ_ENCODE_CLUT=1', 'output_encode_clut.frag'],
                  # M14A: the only compute shader in the engine. RGB -> P010
                  # for the video encoder, because the driver extension that
                  # would have taken RGB directly does not work on this Mesa.
                  ['rgb_to_p010.comp', 'rgb_to_p010_comp_spv']]
  # The output basename, so the two output_encode.frag variants do not collide
  # in the build directory.
  base = shader.length() > 3 ? shader[3] : shader[0]
  extra = shader.length() > 2 ? [shader[2]] : []
  spv = custom_target(base + '.spv',
    input : 'shader/src' / shader[0],
    output : base + '.spv',
    # Every include, listed: push.glsl in particular is the single declaration
    # of the push-constant block, so a change to it must rebuild all four.
    depend_files : files('shader/src/rounded.glsl',
                         'shader/src/push.glsl',
                         'shader/src/gradient.glsl',
                         'shader/src/dither.glsl',
                         'shader/src/shadow.glsl',
                         'shader/src/blur.glsl',
                         # C1's GLSL twin. Listed for the same reason as
                         # push.glsl: it is a shared declaration, and a change
                         # to a transfer-function constant must rebuild every
                         # shader that includes it rather than leaving a stale
                         # SPIR-V with the old curve embedded.
                         'shader/src/color.glsl'),
    command : [glslang, '-V', '--target-env', 'vulkan1.3',
               '-I' + meson.current_source_dir() / 'shader/src'] + extra + [
               '@INPUT@', '-o', '@OUTPUT@'])
  avk_shader_headers += custom_target(shader[1] + '.h',
    input : spv,
    output : base.split('.')[0] + '_' + base.split('.')[1] + '.spv.h',
    command : [avk_embed, '@INPUT@', '@OUTPUT@', shader[1]])
endforeach

avk_sources = files(
  'avk_log.c',
  'command/avk_command.c',
  'command/avk_retire.c',
  'command/avk_timestamp.c',
  'debug/avk_debug.c',
  'dmabuf/avk_dmabuf.c',
  'dmabuf/avk_drm_format.c',
  'dmabuf/avk_format_table.c',
  'device/avk_device.c',
  'device/avk_instance.c',
  'device/avk_phys.c',
  'effect/avk_blur.c',
  'effect/avk_blur_cache.c',
  'graph/avk_graph.c',
  'graph/avk_transient.c',
  'image/avk_image.c',
  'image/avk_upload.c',
  'image/avk_upload_worker.c',
  'encode/avk_encode.c',
  'encode/avk_heif.c',
  'encode/avk_mp4.c',
  'pipeline/avk_gradient.c',
  'pipeline/avk_output_encode.c',
  'pipeline/avk_pipeline.c',
  'scene/avk_blur_dump.c',
  'scene/avk_oracle.c',
  'scene/avk_render.c',
  'scene/avk_scene.c',
  'sync/avk_sync.c',
)

avk_lib = static_library('avk',
  avk_sources,
  avk_shader_headers,
  # Deliberately short: vulkan and libc. No wayland, no wlroots, no scenefx,
  # no EGL. Anything added here should have to be argued for.
  # Each file declares the feature-test macro it needs at the top, so nothing
  # is set here: avk_device.c wants _GNU_SOURCE for asprintf() and a
  # command-line _POSIX_C_SOURCE would just have to be undefined again.
  dependencies : [vulkan_dep, avk_libdrm_dep, avk_gbm_dep, avk_pixman_dep,
                  avk_libm_dep, avk_threads_dep],
  include_directories : include_directories('../..'),
)

avk_dep = declare_dependency(
  link_with : avk_lib,
  dependencies : [vulkan_dep, avk_libdrm_dep, avk_gbm_dep, avk_pixman_dep,
                  avk_libm_dep, avk_threads_dep],
  include_directories : include_directories('../..'),
)
