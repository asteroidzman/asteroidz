# avk -- asteroidz's own Vulkan engine.
#
# Built as a static library rather than compiled straight into the asteroidz
# executable so that a) the tests can link it without dragging in the
# compositor, and b) its dependency list is a thing meson prints. The second
# reason is the important one: `meson introspect --dependencies` and the
# include check below are what keep the promise in
# docs/vulkan-native-architecture.md that this subsystem does not depend on
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

# ── shaders ────────────────────────────────────────────────────────────────
# GLSL -> SPIR-V at build time, then embedded as a C array. Compiling at build
# time means a shader syntax error is a build failure rather than a compositor
# that starts and renders nothing; embedding means there is no install path to
# get wrong and no runtime file to be missing.
glslang = find_program('glslangValidator')
avk_embed = find_program('shader/embed.py')

avk_shader_headers = []
foreach shader : [['quad.vert', 'quad_vert_spv'],
                  ['quad.frag', 'quad_frag_spv'],
                  ['texture.frag', 'texture_frag_spv'],
                  ['gradient.frag', 'gradient_frag_spv'],
                  ['shadow.frag', 'shadow_frag_spv']]
  spv = custom_target(shader[0] + '.spv',
    input : 'shader/src' / shader[0],
    output : shader[0] + '.spv',
    # Every include, listed: push.glsl in particular is the single declaration
    # of the push-constant block, so a change to it must rebuild all four.
    depend_files : files('shader/src/rounded.glsl',
                         'shader/src/push.glsl',
                         'shader/src/gradient.glsl',
                         'shader/src/dither.glsl'),
    command : [glslang, '-V', '--target-env', 'vulkan1.3',
               '-I' + meson.current_source_dir() / 'shader/src',
               '@INPUT@', '-o', '@OUTPUT@'])
  avk_shader_headers += custom_target(shader[1] + '.h',
    input : spv,
    output : shader[0].split('.')[0] + '_' + shader[0].split('.')[1] + '.spv.h',
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
  'graph/avk_graph.c',
  'image/avk_image.c',
  'image/avk_upload.c',
  'pipeline/avk_gradient.c',
  'pipeline/avk_pipeline.c',
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
  dependencies : [vulkan_dep, avk_libdrm_dep, avk_gbm_dep, avk_pixman_dep],
  include_directories : include_directories('../..'),
)

avk_dep = declare_dependency(
  link_with : avk_lib,
  dependencies : [vulkan_dep, avk_libdrm_dep, avk_gbm_dep, avk_pixman_dep],
  include_directories : include_directories('../..'),
)
