project('asteroidz', ['c'],
  version : '0.20.7',
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
libm = cc.find_library('m')
libdrm = dependency('libdrm')
xcb = dependency('xcb', required : get_option('xwayland'))
xlibs = dependency('xcb-icccm', required : get_option('xwayland'))
wayland_server_dep = dependency('wayland-server',version: '>=1.23.1')
wlroots_dep = dependency('wlroots-0.20',version: '>=0.20.0')
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

if get_option('bar-config')
  c_args += '-DASTEROIDZ_NATIVE_BAR'
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

executable('asteroidz',
  'src/asteroidz.c',
  'src/common/util.c',
  'src/draw/text-node.c',
  'src/draw/ufo-node.c',
  'src/ext-protocol/wlr_ext_workspace_v1.c',
  wayland_sources,
  dependencies : [
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
  ],
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
# compositor) but a corrupted file on disk.
test_kdl_edit = executable('test-kdl-edit', 'tests/test-kdl-edit.c',
                           build_by_default : false)
test('kdl-edit', test_kdl_edit)

mandir = get_option('mandir')
desktop_install_dir = join_paths(prefix, 'share/wayland-sessions')
portal_install_dir = join_paths(prefix, 'share/xdg-desktop-portal')
install_data('assets/asteroidz.desktop', install_dir : desktop_install_dir)
install_data('assets/asteroidz-vulkan.desktop', install_dir : desktop_install_dir)
install_data('assets/asteroidz-portals.conf', install_dir : portal_install_dir)
install_data('assets/asteroidz.portal', install_dir : join_paths(prefix, 'share/xdg-desktop-portal/portals'))
install_data('assets/config.kdl', install_dir : join_paths(sysconfdir, 'asteroidz'))
install_data('amsg/amsg.1', install_dir : join_paths(mandir, 'man1'))

# Bar artwork. Vendored from the waybar plugin repos and mostly normalised to
# one square canvas (contrib/normalize-bar-icons.py), so the bar no longer
# depends on which plugins happen to be installed -- and so the icons are
# tracked and reviewable here rather than living only in ~/.local/share.
# asteroidz-bar/logo.svg is the deliberate exception to the normalising; it
# says why in its own header.
install_subdir('assets/bar-icons', install_dir : join_paths(prefix, 'share/asteroidz'),
               strip_directory : false)

# ...and they are checked, because a bar icon fails SILENTLY: a missing file and
# an unparseable one both come out as a blank pill. See the script's header for
# the two that actually shipped.
test('bar-icons', find_program('tests/check-bar-icons.py'))

