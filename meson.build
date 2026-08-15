project('asteroidz', ['c'],
  version : '0.21.1',
)

subdir('protocols')

is_nixos = false
os_release = run_command('cat', '/etc/os-release', check: false)
if os_release.returncode() == 0
    if os_release.stdout().contains('ID=nixos')
        is_nixos = true
    endif
endif

prefix = get_option('prefix')
sysconfdir = get_option('sysconfdir')

# 如果 sysconfdir 以 prefix 开头，去掉 prefix
if sysconfdir.startswith(prefix) and not is_nixos
  sysconfdir = sysconfdir.substring(prefix.length())
  # 确保 sysconfdir 是绝对路径
  if not sysconfdir.startswith('/')
    sysconfdir = '/' + sysconfdir
  endif
endif

cc = meson.get_compiler('c')

# Which rendering architectures to build.
#
# During the migration both are built: GLES via SceneFX is the known-good
# recovery path and stays selectable, while the asteroidz-owned Vulkan engine
# grows underneath it. The end state is -Drenderer=vulkan with GLES demoted;
# until then, dropping either one has to be something you asked for.
build_vulkan = get_option('renderers').contains('vulkan')
build_gles2 = get_option('renderers').contains('gles2')
if not build_vulkan and not build_gles2
  error('-Drenderers needs at least one of gles2, vulkan')
endif
if build_vulkan
  subdir('src/render/vulkan')
endif

libm = cc.find_library('m')
libdrm = dependency('libdrm')
xcb = dependency('xcb', required : get_option('xwayland'))
xlibs = dependency('xcb-icccm', required : get_option('xwayland'))
wayland_server_dep = dependency('wayland-server',version: '>=1.23.1')
# >= 0.20.2, not 0.20.0. The vendored scenefx reads wlr_surface_output.suspended,
# which 0.20.0 does not have -- so a build against it got 116 files in and then
# failed with a struct-member error, which is a poor way to learn a version
# requirement. Caught at configure time now.
wlroots_dep = dependency('wlroots-0.20',version: '>=0.20.2')
xkbcommon_dep = dependency('xkbcommon')
libinput_dep = dependency('libinput',version: '>=1.27.1')
libwayland_client_dep = dependency('wayland-client')
pcre2_dep = dependency('libpcre2-8')
# asteroidz-scenefx lives in-tree at subprojects/asteroidz-scenefx (git
# subtree, full history -- `git subtree pull --prefix=subprojects/asteroidz-
# scenefx scenefx-src main` to track it). asteroidz cannot run without it, so
# it is built as a static subproject and linked in rather than resolved as an
# external package: one binary, no libasteroidz-scenefx-0.5.so to keep in sync
# with it, and -- the reason this was done -- ONE Tracy client in the process
# instead of a private static copy inside the library that asteroidz's own
# instrumentation could not reach.
#
# default_library=static is forced here rather than left to the top-level
# option so a shared asteroidz build doesn't silently produce a .so that has
# to be installed alongside; nothing else consumes this copy.
#
# `version:` intentionally omitted: the constraint would be checked against
# the .pc Version field, which mirrors asteroidz's own release tag -- with the
# source in-tree the two can no longer disagree, so there is nothing to assert.
libscenefx_proj = subproject(
  'asteroidz-scenefx',
  default_options: [
    'default_library=static',
    'examples=false',
    # Must match what the asteroidz-scenefx package built: its PKGBUILD passes
    # -Drenderers=gles2,vulkan. The subproject default is 'auto', which is
    # GLES2 ONLY -- taking it would quietly drop fx_vk, i.e. the renderer
    # WLR_RENDERER=vulkan actually selects, and the failure would surface as a
    # missing renderer at runtime rather than as a build error.
    'renderers=gles2,vulkan',
    'tracy_enable=' + (get_option('tracy') ? 'true' : 'false'),
  ],
)
libscenefx_dep = libscenefx_proj.get_variable('scenefx')
pixman_dep = dependency('pixman-1')
# M6B/D2: ICC matrix-shaper ingest. Already in the dependency closure via
# wlroots' colour management; named explicitly so the ingest unit can be built
# and unit-tested without a renderer.
lcms2_dep = dependency('lcms2')
cjson_dep = dependency('libcjson')
pangocairo_dep = dependency('pangocairo')
gdkpixbuf_dep = dependency('gdk-pixbuf-2.0')
libsystemd_dep = dependency('libsystemd')

# version info
git = find_program('git', required : false)
is_git_repo = false

# check if current directory is a git repo
if git.found()
  git_status = run_command(git, 'rev-parse', '--is-inside-work-tree', check : false)
  if git_status.returncode() == 0 and git_status.stdout().strip() == 'true'
    is_git_repo = true
  endif
endif

if is_git_repo
  # if current directory is a git repo, get commit hash and latest tag.
  # latest_tag is derived from git itself (not the project()-declared
  # version above, which is a plain string literal nobody remembers to
  # bump at release time -- it silently fell a version behind for 0.17.0,
  # so asteroidz -v / amsg get version kept reporting 0.16.7 even in
  # 0.17.0 builds). --abbrev=0 always returns just the nearest tag name,
  # never a "-N-gHASH" suffix, so this stays a clean "TAG(hash)" string
  # exactly like before. Falls back to the literal only if no tag is
  # reachable at all (e.g. a shallow clone with no tags fetched).
  commit_hash = run_command(git, 'rev-parse', '--short', 'HEAD', check : false).stdout().strip()
  tag_result = run_command(git, 'describe', '--tags', '--abbrev=0', check : false)
  if tag_result.returncode() == 0 and tag_result.stdout().strip() != ''
    latest_tag = tag_result.stdout().strip()
  else
    latest_tag = meson.project_version()
  endif
  version_with_hash = '@0@(@1@)'.format(latest_tag, commit_hash)
else
  # if not a git repo, use project version and "release" string
  commit_hash = 'release'
  latest_tag = meson.project_version()
  version_with_hash = '@0@(@1@)'.format(latest_tag, commit_hash)
endif

# define compilation args
c_args = [
  '-g',
  '-Wno-unused-function',
  '-DWLR_USE_UNSTABLE',
  '-D_POSIX_C_SOURCE=200809L',
  '-DVERSION="@0@"'.format(version_with_hash),
  '-DSYSCONFDIR="@0@"'.format(sysconfdir),
  # where install_subdir puts the bar artwork, so the default icon search
  # path finds our own copy first regardless of prefix
  '-DASTEROIDZ_PREFIX="@0@"'.format(prefix),
]

# add debug args only when debug option is enabled
if get_option('asan')
  c_args += [
    '-fsanitize=address',
    '-fno-omit-frame-pointer',
    '-fno-optimize-sibling-calls'
  ]
endif

if xcb.found() and xlibs.found()
  c_args += '-DXWAYLAND'
endif

if get_option('tracy')
  # The subproject sets these with add_project_arguments, which is scoped to
  # ITS targets only -- without repeating them here every TRACY_* macro in the
  # header asteroidz includes expands to nothing and the build looks fine while
  # collecting no asteroidz zones at all.
  #
  # ON_DEMAND matches the subproject: no data is gathered, and no queue is
  # filled, until a viewer actually attaches -- so an enabled build can be run
  # as the daily driver and profiled when something looks wrong, rather than
  # being a separate build you have to reproduce the problem under.
  c_args += ['-DTRACY_ENABLE', '-DTRACY_ON_DEMAND']
  if get_option('buildtype') != 'debugoptimized'
    warning('Profiling builds should set --buildtype=debugoptimized')
  endif
endif

# define link args
link_args = []
if get_option('asan')
  link_args += '-fsanitize=address'
endif

# ── colour maths (M5, contract C1) ──────────────────────────────────────────
#
# Its own static library rather than a file in the asteroidz source list, for
# one reason: the unit tests and the CPU pipeline reference must be able to
# link it WITHOUT the compositor, without wlroots and without Vulkan. A colour
# primitive that can only be exercised inside a running compositor is a
# primitive nobody can pin to a published constant, and M5's whole method is
# that every number in it is checkable against a standard.
#
# Dependencies: libm. That is the complete list and it should stay that way --
# see the header's opening comment.
az_color_lib = static_library('az-color',
                              'src/render/color/az_color.c',
                              dependencies : [libm],
                              include_directories : include_directories('src'))
az_color_dep = declare_dependency(link_with : az_color_lib,
                                  dependencies : [libm],
                                  include_directories : include_directories('src'))

# The primitives, pinned to ST 2084 / IEC 61966-2-1 / BT.2087 / BT.2408. No
# GPU, no compositor: it runs on a build machine with no DRM node at all. The
# source root is passed because the GLSL-twin parity check reads both sources
# and compares their shared constants -- the numerical GPU parity fixture C1
# also specifies needs a device and is deferred.
test_color_math = executable('test-color-math', 'tests/test-color-math.c',
                             dependencies : [az_color_dep],
                             build_by_default : false)
test('color-math', test_color_math, args : [meson.project_source_root()],
     timeout : 30)

# The luminance-domain resolver (M5, contract C2). Header-only, so this links
# nothing but libc -- which is the point: the resolver is the piece most worth
# exercising, and a signature mentioning wlroots types would drag a wayland
# display into the test binary and the test would stop being written.
test_lum_domain = executable('test-lum-domain', 'tests/test-lum-domain.c',
                             include_directories : include_directories('src'),
                             dependencies : [libm],
                             build_by_default : false)
test('lum-domain', test_lum_domain, timeout : 30)

# The output-colour-state decision table (M5, contract C3). Five outcomes, four
# of which render a plausible picture when chosen wrongly -- which is why the
# table asserts the WHOLE state per row rather than the field it is about.
test_icc_shaper = executable('test-icc-shaper',
                             ['tests/test-icc-shaper.c',
                              'src/render/color/az_icc.c'],
                             # contrib/icc-synth.c is #included by the test, not
                             # listed here: it carries a main() behind a guard.
                             dependencies : [lcms2_dep, libm],
                             build_by_default : false)
test('icc-shaper', test_icc_shaper, timeout : 30)

# M6B/G3b. Generates the ICC profiles this machine does not have -- above all a
# cLUT display profile, without which D2's refusal has no falsifier. Not a test:
# a tool the headless fixture runs. See contrib/icc-synth.c for why the cLUT one
# carries colorants as well.
icc_synth = executable('icc-synth', 'contrib/icc-synth.c',
                       dependencies : [lcms2_dep],
                       build_by_default : false)

test_output_color = executable('test-output-color',
                               'tests/test-output-color.c',
                               dependencies : [az_color_dep],
                               build_by_default : false)
test('output-color', test_output_color, timeout : 30)

# THE M5 GATE (contract C4). The CPU reference pipeline and every gate it
# supports, including the breaks -- which run in the ordinary test binary
# rather than behind an #ifdef, so a break that has stopped breaking shows up
# as a failure here rather than as nothing at all.
#
# az_color_ref.c is compiled into the test and NOT into asteroidz: it
# allocates, it keeps every intermediate image alive at once, and it is written
# to be read against the ADRs. The compositor has no use for it.
test_color_pipeline = executable('test-color-pipeline',
                                 ['tests/test-color-pipeline.c',
                                  'src/render/color/az_color_ref.c'],
                                 dependencies : [az_color_dep],
                                 build_by_default : false)
test('color-pipeline', test_color_pipeline,
     args : [meson.project_source_root()], timeout : 60)

# The native Vulkan engine is compiled into the compositor when it is built.
# A separate flag from -Drenderers so the guard in the source says what it
# means: "this build has AVK", not "this build was configured a certain way".
asteroidz_deps = [
    az_color_dep,
    # M6B/G2: az_icc.c reads the profile with lcms2 rather than reimplementing
    # an ICC parser, which is the one part of this nobody should write twice.
    lcms2_dep,
    libm,
    libdrm,
    xcb,
    xlibs,
    libscenefx_dep,
    wayland_server_dep,
    wlroots_dep,
    xkbcommon_dep,
    libinput_dep,
    libwayland_client_dep,
    pcre2_dep,
    pixman_dep,
    cjson_dep,
    pangocairo_dep,
    gdkpixbuf_dep,
    libsystemd_dep,
]
if build_vulkan
  asteroidz_deps += avk_dep
  c_args += '-DAZ_HAVE_VULKAN'
endif

asteroidz_exe = executable('asteroidz',
  'src/asteroidz.c',
  # M6B/G2. The ICC matrix-shaper ingest, now that the compositor applies the
  # reduction itself rather than only handing the file to wlroots.
  'src/render/color/az_icc.c',
  'src/common/util.c',
  'src/draw/text-node.c',
  'src/draw/ufo-node.c',
  'src/ext-protocol/wlr_ext_workspace_v1.c',
  wayland_sources,
  dependencies : asteroidz_deps,
  install : true,
  c_args : c_args,
  link_args : link_args,
)

# build amsg
dwl_ipc_protocol = 'protocols/dwl-ipc-unstable-v2.xml'

wayland_scanner_client_header = generator(
  wayland_scanner,
  output: '@BASENAME@-protocol.h', 
  arguments: ['client-header', '@INPUT@', '@OUTPUT@']
)

wayland_scanner_private_code = generator(
  wayland_scanner,
  output: '@BASENAME@-protocol.c',
  arguments: ['private-code', '@INPUT@', '@OUTPUT@']
)

# use generator in amsg target
executable('amsg',
  'amsg/amsg.c',
  wayland_scanner_private_code.process(dwl_ipc_protocol),
  wayland_scanner_client_header.process(dwl_ipc_protocol),
  dependencies: [
    libwayland_client_dep,
    # --require-* parses `get instance`. Scanning that response with strstr
    # would make the identity check -- the one thing that must not be
    # approximately right -- the least rigorous parse in the tree.
    cjson_dep,
  ],
  install: true,
  c_args: [
    '-g',
    '-Wno-unused-function',
  ],
)

# Unit tests for the pure helpers -- the ones whose failure mode is not
# "the screen looks wrong" (which contrib/regression covers by driving a real
# compositor) but a corrupted file on disk, or a reply that arrives with its
# tail missing.
test_kdl_edit = executable('test-kdl-edit', 'tests/test-kdl-edit.c',
                           build_by_default : false)
test('kdl-edit', test_kdl_edit)

test_ipc_out = executable('test-ipc-out', 'tests/test-ipc-out.c',
                          build_by_default : false)
test('ipc-out', test_ipc_out)

# ── the Vulkan engine ───────────────────────────────────────────────────────
#
# M1 of the migration in docs/vulkan-native-architecture.md. Built whenever
# 'vulkan' is in -Drenderers, which it is by default; the compositor does not
# use it yet (that is M3), so this is a library plus its tests and nothing in
# asteroidz.c links against it.
if build_vulkan
  test('avk-isolation', find_program('tests/check-vulkan-isolation.py'))

  # A real GPU test: it opens a render node, creates a device, drives the
  # command ring and the retire queue. Exits 77 (skip) where there is no GPU,
  # so a build machine without DRM reports "skipped" rather than a red X for
  # hardware it does not have.
  test_avk_core = executable('test-avk-core', 'tests/test-avk-core.c',
                             dependencies : [avk_dep],
                             build_by_default : false)
  test('avk-core', test_avk_core, timeout : 60)

  # The capability model, driven with SYNTHETIC format sets. The live test
  # cannot cover the rule this protects: on a machine whose GLES and Vulkan
  # tables overlap, advertising the wrong one still passes every subset check,
  # which is exactly how the shipped bug survived M3.5. Here the two sets are
  # made to disagree on purpose, so "which engine did the answer come from" has
  # an observable consequence that does not depend on this GPU or this Mesa.
  test_dmabuf_feedback = executable('test-dmabuf-feedback',
                                    'tests/test-dmabuf-feedback.c',
                                    dependencies : [avk_dep, wlroots_dep],
                                    include_directories : include_directories('src'),
                                    c_args : ['-DWLR_USE_UNSTABLE', '-DAZ_HAVE_VULKAN'],
                                    build_by_default : false)
  test('dmabuf-feedback', test_dmabuf_feedback, timeout : 30)

  # Corner permutation under output transforms. A rotated output cannot be
  # built in the headless fixture (monitorrule's key:value format is not
  # emittable from harness KDL), so the production helper is driven directly
  # rather than leaving the invariant untested.
  test_corner_permute = executable('test-corner-permute',
                                   'tests/test-corner-permute.c',
                                   dependencies : [wlroots_dep, wayland_server_dep],
                                   include_directories : include_directories('src'),
                                   c_args : ['-DWLR_USE_UNSTABLE'],
                                   build_by_default : false)
  test('corner-permute', test_corner_permute, timeout : 30)

  # The rectangle/region contract every rotated-output quantity depends on.
  # Needs no GPU: the mapping is pure integer arithmetic, and the point is that
  # a change to it fails HERE rather than as a rotated desktop nobody can
  # capture.
  test_transform_math = executable('test-transform-math',
                                   'tests/test-transform-math.c',
                                   dependencies : [wlroots_dep, wayland_server_dep,
                                                   pixman_dep],
                                   include_directories : include_directories('src'),
                                   c_args : ['-DWLR_USE_UNSTABLE'],
                                   build_by_default : false)
  test('transform-math', test_transform_math, timeout : 30)

  # The attachment/presentation extent contract, and the old conflated
  # implementation run through the same predicate so that the contract is shown
  # to reject it. This is where M4F.2C.4d's defect becomes a failing test
  # instead of a rotated desktop nobody can capture.
  test_extent_space = executable('test-extent-space',
                                 'tests/test-extent-space.c',
                                 dependencies : [wlroots_dep, wayland_server_dep],
                                 include_directories : include_directories('src'),
                                 c_args : ['-DWLR_USE_UNSTABLE'],
                                 build_by_default : false)
  test('extent-space', test_extent_space, timeout : 30)

  # The one that reproduces the blank-window class: a real GBM buffer whose
  # modifier is thrown away before import, which is what wl_drm and
  # modifier-less Electron/Chromium hand over.
  test_avk_dmabuf = executable('test-avk-dmabuf', 'tests/test-avk-dmabuf.c',
                               dependencies : [avk_dep],
                               build_by_default : false)
  test('avk-dmabuf', test_avk_dmabuf, timeout : 60)

  # M3: AVK composites a scene snapshot and the pixels are read back and
  # compared. These assert on the framebuffer, not on functions returning
  # true -- a renderer with the wrong blend state or a mis-folded transform
  # succeeds at every call it makes and draws the wrong picture.
  # az_color_ref.c is linked in for M5's Path-B tests: the PQ encode is
  # compared against C4's CPU reference, which was written from the ADRs rather
  # than from the shader. A second copy of the shader's own arithmetic would be
  # a tautology.
  test_avk_render = executable('test-avk-render',
                               ['tests/test-avk-render.c',
                                'src/render/color/az_color_ref.c',
                                # M6B/G2: the gate compares the LUT1D encode
                                # against az_icc_apply, the same reduction G1
                                # checked against lcms2.
                                'src/render/color/az_icc.c'],
                               dependencies : [avk_dep, az_color_dep, lcms2_dep,
                                               libm],
                               build_by_default : false)
  test('avk-render', test_avk_render, timeout : 60)

  # M4C: gradients. Separate from avk-render because a gradient is a NUMERICAL
  # claim -- which band, starting where, out of how many -- and the fixtures
  # need a bigger target and their own colour scheme to say so exactly.
  # libm: the fixtures carry an independent CPU implementation of the
  # reference gradient formula, trigonometry and all, as the oracle AVK's
  # output is compared against.
  test_avk_gradient = executable('test-avk-gradient',
                                 'tests/test-avk-gradient.c',
                                 dependencies : [avk_dep, libm],
                                 build_by_default : false)
  test('avk-gradient', test_avk_gradient, timeout : 120)

  # M4D.P: GPU timestamps. Half of this runs with no GPU at all -- the wrap
  # and masking arithmetic is tested on constructed values, because this
  # machine's 64 valid bits make masking the identity and a real wrap would
  # take centuries.
  test_avk_timestamp = executable('test-avk-timestamp',
                                  'tests/test-avk-timestamp.c',
                                  dependencies : [avk_dep],
                                  build_by_default : false)
  test('avk-timestamp', test_avk_timestamp, timeout : 120)

  # M4E: the render graph. Asserts on the barriers the graph ACTUALLY EMITTED
  # -- it records them as it emits them -- rather than on a re-derivation from
  # the same rules the implementation used, which could not fail. The usage
  # table is restated independently in the test for the same reason.
  test_avk_graph = executable('test-avk-graph',
                              'tests/test-avk-graph.c',
                              dependencies : [avk_dep],
                              build_by_default : false)
  test('avk-graph', test_avk_graph, timeout : 120)

  # M4E.2: the transient pool. Asserts on `unsafe_reuses` -- a counter the
  # pool increments as it commits the violation -- rather than on whether
  # corruption happened to appear, which is the only way a lifetime rule can
  # be checked deterministically.
  test_avk_transient = executable('test-avk-transient',
                                  'tests/test-avk-transient.c',
                                  dependencies : [avk_dep],
                                  build_by_default : false)
  test('avk-transient', test_avk_transient, timeout : 180)

  # M4E.4: a synthetic multipass frame -- render into a transient, then sample
  # it. The effect has no visual purpose and is deliberately NOT shipped: M4E
  # is architecture, and a user-visible "M4E effect" would be a feature nobody
  # asked for that has to be maintained forever. What it proves is the write ->
  # sample dependency, transient reuse, and deterministic pixels.
  test_avk_multipass = executable('test-avk-multipass',
                                  'tests/test-avk-multipass.c',
                                  dependencies : [avk_dep],
                                  build_by_default : false)
  test('avk-multipass', test_avk_multipass, timeout : 180)

  # M4F.3/.5: what vkCmdPipelineBarrier2 actually costs, and why. Isolates one
  # variable at a time -- barriers per call, queue-family transfer, DCC vs
  # non-DCC modifier, image size, layout pair -- on the same device in the same
  # process, because a live compositor varies in all of them at once and can
  # only ever produce another hypothesis.
  test_avk_barrier_cost = executable('test-avk-barrier-cost',
                                     'tests/test-avk-barrier-cost.c',
                                     dependencies : [avk_dep],
                                     build_by_default : false)
  test('avk-barrier-cost', test_avk_barrier_cost, timeout : 180)

  # M4F.1/.10/.11: the dual-Kawase blur. Every PRIMARY fixture has high spatial
  # frequency -- a 1px checkerboard, an impulse, a hard edge, thin lines --
  # because blurring a flat field returns the same flat field whatever the
  # kernel does, so a flat fixture passes for a blur that samples the wrong
  # level, uses the wrong step, or runs no passes at all. libm for the
  # statistics.
  test_avk_blur = executable('test-avk-blur', 'tests/test-avk-blur.c',
                             dependencies : [avk_dep, libm],
                             build_by_default : false)
  test('avk-blur', test_avk_blur, timeout : 180)

  # M4F.2D: actual vs REQUIRED blur-pass work, the denominator of the per-level
  # scissor decision. Needs no GPU -- it is rectangle arithmetic over the same
  # level_extent()/texel_span() the passes use, and the point is that a wrong
  # denominator fails HERE rather than becoming a performance conclusion.
  test_avk_blur_work = executable('test-avk-blur-work',
                                  'tests/test-avk-blur-work.c',
                                  dependencies : [avk_dep, libm],
                                  build_by_default : false)
  test('avk-blur-work', test_avk_blur_work, timeout : 30)

  # M4F.2A.1: segmented composition. Proves that any contiguous command range
  # renders into any regional target with identical pixels -- on PIXELS, because
  # a struct-inspection test would pass for a renderer that silently restarted
  # every gradient at the transient's edge.
  test_avk_segment = executable('test-avk-segment', 'tests/test-avk-segment.c',
                                dependencies : [avk_dep],
                                build_by_default : false)
  test('avk-segment', test_avk_segment, timeout : 180)

  # M4F.2A.2: a blur's source is the current-frame scene PREFIX. Proved on
  # pixels against independently constructed references -- scenes containing
  # only what should have contributed -- so the prefix path is never its own
  # oracle. The background is dark and high-frequency and the foreground is
  # near-white, so contamination is tens of codes rather than a subtle shift.
  test_avk_blur_scene = executable('test-avk-blur-scene',
                                   'tests/test-avk-blur-scene.c',
                                   dependencies : [avk_dep],
                                   build_by_default : false)
  test('avk-blur-scene', test_avk_blur_scene, timeout : 240)

  # M5/C2: every command carries a VALID luminance domain. avk_scene_add()
  # memsets, so a field nobody fills defaults to zero -- and a scale of zero is
  # a black surface, not a neutral default. Four command sites would each have
  # had to remember; the default lives in one place and this asserts it for
  # every type, including ones added after it was written.
  test_lum_command = executable('test-lum-command',
                                'tests/test-lum-command.c',
                                dependencies : [avk_dep],
                                build_by_default : false)
  test('lum-command', test_lum_command)

  # M4F.2A.3: the blur node's MATERIAL. darken is checked against a CPU minimum
  # of two SEPARATELY MEASURED renders (clamp off, and no blur node at all), so
  # the production shader is never its own oracle; the soft edge is checked
  # against a real shadow's coverage profile at the same sigma, which is the
  # invariant the feature exists for rather than a restatement of the shader.
  test_avk_blur_material = executable('test-avk-blur-material',
                                      'tests/test-avk-blur-material.c',
                                      dependencies : [avk_dep],
                                      build_by_default : false)
  test('avk-blur-material', test_avk_blur_material, timeout : 240)

  # M4F.2B: a partially damaged frame must contain the same pixels as a
  # completely redrawn one. The forced-full path borrows nothing from the
  # partial one -- it is the same scene and the same current-frame prefix
  # architecture given full damage -- so a difference is a damage bug and can
  # be nothing else. The two breaks are set on the renderer directly, so one
  # process renders the same scene correct and broken and diffs the pictures.
  test_avk_blur_damage = executable('test-avk-blur-damage',
                                    'tests/test-avk-blur-damage.c',
                                    dependencies : [avk_dep],
                                    build_by_default : false)
  test('avk-blur-damage', test_avk_blur_damage, timeout : 300)

  # M4F.2A.3: every command kind states what it samples. No GPU -- the resolver
  # is pure -- and no synchronisation claim: Vulkan validation stays the oracle
  # for barriers. This only has to notice an omission earlier than that would.
  test_avk_cmd_uses = executable('test-avk-cmd-uses',
                                 'tests/test-avk-cmd-uses.c',
                                 dependencies : [avk_dep],
                                 build_by_default : false)
  test('avk-cmd-uses', test_avk_cmd_uses, timeout : 30)

  # M4D: shadows. libm because the fixtures carry an independent CPU
  # transcription of the reference's erf-and-four-samples formula, which is
  # what AVK's readback is compared against -- not GLES, and not AVK's own
  # shader.
  # M5/C5: the colour capability predicates, driven with SYNTHETIC feature
  # sets. No device: this machine's RADV answers yes to every question these
  # ask, so a live test would pass for a predicate that returned true
  # unconditionally -- and the interesting answers are the PARTIAL ones no
  # single GPU can be made to produce on demand.
  test_avk_color_caps = executable('test-avk-color-caps',
                                   'tests/test-avk-color-caps.c',
                                   dependencies : [vulkan_dep],
                                   include_directories : include_directories('src'),
                                   build_by_default : false)
  test('avk-color-caps', test_avk_color_caps, timeout : 30)

  test_avk_shadow = executable('test-avk-shadow',
                               'tests/test-avk-shadow.c',
                               dependencies : [avk_dep, libm],
                               build_by_default : false)
  test('avk-shadow', test_avk_shadow, timeout : 120)

  # asteroidz-avk-probe: run the engine on real hardware and say what it can
  # do. Installed, because until AVK renders the desktop (M3b) this is the
  # only way to exercise it on the machine it will eventually run on -- and it
  # touches nothing the compositor owns, so it is safe to run in a live
  # session.
  executable('asteroidz-avk-probe', 'tools/avk-probe.c',
             dependencies : [avk_dep, dependency('cairo')],
             install : true,
             c_args : c_args)
endif

# Runs from the source root so the corpus case can open assets/config.kdl --
# editing the config we actually ship is the closest thing to editing the
# user's, and the only case that exercises real comments and real nesting.
test_kdl_write = executable('test-kdl-write', 'tests/test-kdl-write.c',
                            build_by_default : false)
test('kdl-write', test_kdl_write, workdir : meson.project_source_root())

# The config schema, checked in both directions against the code it describes.
#
# `-S` drives the real parse_option, set_value_default and override_config and
# asserts every default, clamp, offset and type. It cannot see a key that is
# MISSING from the table, because there is no entry to check -- that is the other
# script's job, and it is the direction that matters as the parser grows.
test('config-schema', asteroidz_exe, args : ['-S'])
test('animation-clocks', find_program('tests/check-animation-clocks.py'),
     workdir: meson.project_source_root())
test('config-schema-coverage', find_program('tests/check-config-schema.py'),
     args : [asteroidz_exe])
test('dispatch-actions-coverage',
     find_program('tests/check-dispatch-actions.py'), args : [asteroidz_exe])
test('rule-schema-coverage',
     find_program('tests/check-rule-schema.py'), args : [asteroidz_exe])
# The third direction: the docs.
#
# `-S` proves the schema matches the code and says nothing about docs/, where the
# default column of every option table is maintained by hand. Four rows disagreed
# with the running code, and `animation_duration_focus` shows how: it was
# documented as `0`, the value set_value_default assigns, but override_config
# clamps it to [1, 50000] on the next line, so `0` never once took effect.
#
# This CHECKS rather than generates. The schema's `desc` is a one-line tooltip;
# the docs descriptions carry links, KDL snippets and bitmask tables, and
# generating over them would trade all of that for the tooltip.
test('config-docs', find_program('tests/check-config-docs.py'),
     args : [asteroidz_exe])
# The man page's dispatch list, against the compositor's own.
#
# amsg.1 documents all 94 actions, which is a second copy of a table -- and a
# second copy drifts silently: the first action added after it was written would
# simply be absent, with nothing to say so. The block between the BEGIN/END
# markers is generated from `asteroidz -D`; this asserts it still matches, and
# names the actions that differ rather than only reporting that they do.
test('amsg-man', find_program('tests/check-amsg-man.py'),
     args : ['--asteroidz', asteroidz_exe])
# The config we SHIP has to parse.
#
# It is what /etc/asteroidz/config.kdl becomes and what every new user is told to
# copy, and nothing was checking it. It had drifted badly: a `pill { }` section
# renamed to `theme` years ago so five settings inside it were unknown keywords,
# five dispatch names that no longer exist, and seven `source` lines pointing at
# files that only ever existed in one person's home directory. `asteroidz -p`
# exited 1 on it. Found by installing it on a clean machine, which is a poor
# substitute for a test.
test('shipped-config', asteroidz_exe,
     args : ['-p', '-c', meson.current_source_dir() / 'assets/config.kdl'])

mandir = get_option('mandir')
desktop_install_dir = join_paths(prefix, 'share/wayland-sessions')
portal_install_dir = join_paths(prefix, 'share/xdg-desktop-portal')
#
# ── TWO SESSIONS, AND THE GREETER LIST IS PART OF THE PRODUCT ────────────
#
# Every entry here appears in the login screen the operator actually uses, so
# the list is not free: an env-gated experiment left installed is a way to boot
# into a configuration nobody meant to be in, and one of them was DELIBERATELY
# BROKEN. Four were removed at once (fx_vk, two software-cursor variants, and
# the M4A bottom-swap break) because their milestones had closed or they had
# stopped being referenced -- and one of those, asteroidz-avk-swcursor, was
# found installed on the machine while not being shipped from here at all: an
# orphan left by an older build that outlived the source it came from. Sessions
# added for one experiment need removing when that experiment ends.
#
# The GLES2/SceneFX session went the same way. It was kept as the way back in
# if an AVK change made the desktop unusable -- but AVK is the renderer this
# compositor is, the fallback had stopped being tested as a desktop, and there
# are other ways back to a shell that do not cost a permanent greeter entry
# advertising a second-class configuration.
#
# The AVK session sets WLR_RENDERER=gles2 deliberately rather than leaving it
# unset. ASTEROIDZ_RENDERER is independent of it -- wlroots still needs a
# renderer for shm formats, the allocator and screencopy, none of which are
# composition -- and pinning it to GLES2 makes the session exactly the
# configuration contrib/avk-frame-test.sh asserts against.
install_data('assets/asteroidz-avk.desktop', install_dir : desktop_install_dir)

# The acceptance session, installed rather than left as a hand-written file
# under /usr/share: an acceptance run whose environment is a local artefact
# cannot be reproduced later, and "validation was on" is a claim the session
# entry has to be able to back up. Deliberately slower than the plain AVK
# session -- ~99x the CPU per frame -- and its Comment says so.
install_data('assets/asteroidz-avk-debug.desktop',
	install_dir : desktop_install_dir)
install_data('assets/asteroidz-portals.conf', install_dir : portal_install_dir)
install_data('assets/asteroidz.portal', install_dir : join_paths(prefix, 'share/xdg-desktop-portal/portals'))
install_data('assets/config.kdl', install_dir : join_paths(sysconfdir, 'asteroidz'))
# The default palette, beside the default config that sources it.
#
# Not optional: a missing `source` is a fatal config error, so shipping a config
# that sources a file nobody installed is a fresh install that refuses to start --
# and the place you find that out is the login screen. matugen overwrites this
# the first time it runs.
install_data('assets/colors.kdl', install_dir : join_paths(sysconfdir, 'asteroidz'))
# The session autostart. Executable, and beside the config that runs it.
#
# Everything a session starts beyond the compositor itself is conditional -- on a
# program being installed, on it not already running -- and KDL can express
# neither. The shipped copy is the fallback the default config falls back TO, so a
# fresh install has a bar without the user having copied anything.
install_data('assets/autostart.sh',
             install_dir : join_paths(sysconfdir, 'asteroidz'),
             install_mode : 'rwxr-xr-x')
install_data('amsg/amsg.1', install_dir : join_paths(mandir, 'man1'))

# Bar artwork. Vendored from the waybar plugin repos and mostly normalised to
# one square canvas (contrib/normalize-bar-icons.py), so the bar no longer
# depends on which plugins happen to be installed -- and so the icons are
# tracked and reviewable here rather than living only in ~/.local/share.
# asteroidz-bar/logo.svg is the deliberate exception to the normalising; it
# says why in its own header.
install_subdir('assets/bar-icons', install_dir : join_paths(prefix, 'share/asteroidz'),
               strip_directory : false)

# The ship again, this time as an icon-THEME entry.
#
# The settings window is an ordinary xdg toplevel, and xdg-toplevel-icon-v1
# carries an icon NAME rather than pixels -- which anything listing windows then
# resolves through the icon theme. The copy above is on the bar's own private
# search path, which nothing else knows how to look in, so a second copy has to
# live where a theme lookup finds it. Same file, installed twice on purpose.
install_data('assets/bar-icons/asteroidz-bar/logo.svg',
             install_dir : join_paths(prefix, 'share/icons/hicolor/scalable/apps'),
             rename : 'asteroidz-settings.svg')

# ...and they are checked, because a bar icon fails SILENTLY: a missing file and
# an unparseable one both come out as a blank pill. See the script's header for
# the two that actually shipped.
test('bar-icons', find_program('tests/check-bar-icons.py'))

