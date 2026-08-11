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

# The native Vulkan engine is compiled into the compositor when it is built.
# A separate flag from -Drenderers so the guard in the source says what it
# means: "this build has AVK", not "this build was configured a certain way".
asteroidz_deps = [
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
    libwayland_client_dep
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
  test_avk_render = executable('test-avk-render', 'tests/test-avk-render.c',
                               dependencies : [avk_dep],
                               build_by_default : false)
  test('avk-render', test_avk_render, timeout : 60)

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
install_data('assets/asteroidz.desktop', install_dir : desktop_install_dir)
install_data('assets/asteroidz-vulkan.desktop', install_dir : desktop_install_dir)
# The AVK session sets WLR_RENDERER=gles2 deliberately rather than leaving it
# unset. ASTEROIDZ_RENDERER is independent of it -- wlroots still needs a
# renderer for shm formats, the allocator and screencopy, none of which are
# composition -- and pinning it to GLES2 makes the session exactly the
# configuration contrib/avk-frame-test.sh asserts against.
install_data('assets/asteroidz-avk.desktop', install_dir : desktop_install_dir)

# Acceptance sessions. Installed rather than left as hand-written files under
# /usr/share, because an acceptance run whose environment is a local artefact
# cannot be reproduced later -- and "validation was on" is a claim the session
# entry has to be able to back up. Both are deliberately slower than the plain
# AVK session and say so in their own Comment.
install_data('assets/asteroidz-avk-debug.desktop',
	install_dir : desktop_install_dir)
install_data('assets/asteroidz-avk-swcursor-debug.desktop',
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

