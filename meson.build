project('asteroidz', ['c'],
  version : '0.17.2',
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
libscenefx_dep = dependency('asteroidz-scenefx-0.5',version: '>=0.5.0')
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

mandir = get_option('mandir')
desktop_install_dir = join_paths(prefix, 'share/wayland-sessions')
portal_install_dir = join_paths(prefix, 'share/xdg-desktop-portal')
install_data('assets/asteroidz.desktop', install_dir : desktop_install_dir)
install_data('assets/asteroidz-vulkan.desktop', install_dir : desktop_install_dir)
install_data('assets/asteroidz-portals.conf', install_dir : portal_install_dir)
install_data('assets/asteroidz.portal', install_dir : join_paths(prefix, 'share/xdg-desktop-portal/portals'))
install_data('assets/config.kdl', install_dir : join_paths(sysconfdir, 'asteroidz'))
install_data('amsg/amsg.1', install_dir : join_paths(mandir, 'man1'))

