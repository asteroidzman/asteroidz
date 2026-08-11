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

avk_sources = files(
  'avk_log.c',
  'command/avk_command.c',
  'command/avk_retire.c',
  'debug/avk_debug.c',
  'dmabuf/avk_dmabuf.c',
  'dmabuf/avk_drm_format.c',
  'dmabuf/avk_format_table.c',
  'device/avk_device.c',
  'device/avk_instance.c',
  'device/avk_phys.c',
  'image/avk_image.c',
)

avk_lib = static_library('avk',
  avk_sources,
  # Deliberately short: vulkan and libc. No wayland, no wlroots, no scenefx,
  # no EGL. Anything added here should have to be argued for.
  # Each file declares the feature-test macro it needs at the top, so nothing
  # is set here: avk_device.c wants _GNU_SOURCE for asprintf() and a
  # command-line _POSIX_C_SOURCE would just have to be undefined again.
  dependencies : [vulkan_dep, avk_libdrm_dep, avk_gbm_dep],
  include_directories : include_directories('../..'),
)

avk_dep = declare_dependency(
  link_with : avk_lib,
  dependencies : [vulkan_dep, avk_libdrm_dep, avk_gbm_dep],
  include_directories : include_directories('../..'),
)
