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
# AVK is the only renderer. There is no option because there is no choice:
# a build without it is a compositor that cannot draw, and the GLES recovery
# path it used to select was removed once AVK carried the desktop.
subdir('src/render/vulkan')

libm = cc.find_library('m')
libdrm = dependency('libdrm')
xcb = dependency('xcb', required : get_option('xwayland'))
xlibs = dependency('xcb-icccm', required : get_option('xwayland'))
wayland_server_dep = dependency('wayland-server',version: '>=1.23.1')
# >= 0.20.2, not 0.20.0. The absorbed scene graph (src/scene/) reads
# wlr_surface_output.suspended, which 0.20.0 does not have -- so a build against
# it got 116 files in and then failed with a struct-member error, which is a poor
# way to learn a version requirement. Caught at configure time now.
wlroots_dep = dependency('wlroots-0.20',version: '>=0.20.2')
xkbcommon_dep = dependency('xkbcommon')
libinput_dep = dependency('libinput',version: '>=1.27.1')
libwayland_client_dep = dependency('wayland-client')
pcre2_dep = dependency('libpcre2-8')
# The scene graph lives in src/scene/ now. It was scenefx's, and scenefx is
# gone: AVK composites every frame, so the renderer that graph was coupled to
# had no work to do, and the coupling was the only reason the dependency
# survived. See src/scene/wlr_scene.c.
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
  # The scene graph came from scenefx, which set these on its own targets. It
  # is our source now, so we set them: without WLR_PRIVATE= the wlroots headers
  # hide the struct internals the graph reaches into.
  '-DWLR_PRIVATE=',
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


# M6B/G3b. Generates the ICC profiles this machine does not have -- above all a
# cLUT display profile, without which D2's refusal has no falsifier. Not a test:
# a tool the headless fixture runs. See contrib/icc-synth.c for why the cLUT one
# carries colorants as well.
icc_synth = executable('icc-synth', 'contrib/icc-synth.c',
                       dependencies : [lcms2_dep],
                       build_by_default : false)


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
    lcms2_dep,
]
asteroidz_deps += avk_dep

asteroidz_exe = executable('asteroidz',
  'src/asteroidz.c',
  # M6B/G2. The ICC matrix-shaper ingest, now that the compositor applies the
  # reduction itself rather than only handing the file to wlroots.
  'src/render/color/az_icc.c',
  'src/common/util.c',
  'src/draw/text-node.c',
  'src/draw/ufo-node.c',
  'src/ext-protocol/wlr_ext_workspace_v1.c',
  'src/scene/wlr_scene.c',
  'src/scene/surface.c',
  'src/scene/subsurface_tree.c',
  'src/scene/xdg_shell.c',
  'src/scene/linked_node.c',
  # scenefx's util/, which the graph uses for array growth, env parsing and
  # timespec maths. Three small files, brought along rather than reimplemented.
  'src/scene/array.c',
  'src/scene/env.c',
  'src/scene/time.c',
  'src/scene/matrix.c',
  # The graph's own value types and the two helpers it links against:
  # colour-transform composition and the output's pending image description.
  'src/scene/clipped_region.c',
  'src/scene/blur_data.c',
  'src/scene/color.c',
  # The lcms2 backing, not color_fallback.c: asteroidz already links lcms2 for
  # its own ICC ingest, so taking the stub would silently downgrade ICC
  # handling that currently works.
  'src/scene/color_lcms2.c',
  'src/scene/output.c',
  wayland_sources,
  dependencies : asteroidz_deps,
  # src/scene/include carries the scene graph's own headers (<scene/wlr_scene.h>
  # and friends). It used to arrive through the scenefx subproject's dependency;
  # the graph is ours now, so the path is ours to state.
  include_directories : include_directories('src', 'src/scene/include'),
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


# asteroidz-avk-probe: run the engine on real hardware and say what it can
# do. Installed, because until AVK renders the desktop (M3b) this is the
# only way to exercise it on the machine it will eventually run on -- and it
# touches nothing the compositor owns, so it is safe to run in a live
# session.
executable('asteroidz-avk-probe', 'tools/avk-probe.c',
           dependencies : [avk_dep, dependency('cairo')],
           install : true,
           c_args : c_args)


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
# The AVK sessions set WLR_RENDERER=vulkan deliberately rather than leaving it
# unset. It has nothing to do with composition -- wlroots still needs a
# renderer for shm formats, the allocator and screencopy, none of which are
# composition -- and it used to be pinned to gles2 for exactly that reason.
#
# It is vulkan now because gles2 meant a GL context, GL buffers and GL calls
# existed in a session whose desktop is composited entirely by AVK, and the one
# thing they were still used for -- the DRM cursor plane's 64x64 buffer -- was
# indistinguishable, from outside, from AVK having fallen back. Nothing in this
# compositor composites with GL any more, and the compatibility renderer no
# longer offers a way for it to start.
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

