#!/usr/bin/env bash
# install-ubuntu.sh — build and install asteroidz and asteroidz-bar on Ubuntu 26.04.
#
# There is no .deb. This builds from the released tags, the same ones the AUR
# PKGBUILDs use, so "which version is installed" has an answer.
#
# The one thing that makes this more than `apt install`: Ubuntu 26.04 ships
# wlroots 0.19 and asteroidz needs 0.20, so wlroots is built from source into
# /usr/local. Everything else is packaged.
#
# Verified against a clean ubuntu:26.04 container. Re-runnable: each step skips
# itself if it has already been done, so a failure part-way can be fixed and the
# script run again.

set -euo pipefail

ASTEROIDZ_TAG="${ASTEROIDZ_TAG:-0.21.0}"
BAR_TAG="${BAR_TAG:-0.1.5}"
# 0.20.2, not 0.20.0. asteroidz-scenefx reads wlr_surface_output.suspended, which
# arrived after the .0 release -- and 0.20.0 otherwise looks like a perfectly good
# match, right up to a struct-member error a hundred files into the build.
WLROOTS_TAG="${WLROOTS_TAG:-0.20.2}"
SRC="${SRC:-$HOME/src}"
JOBS="${JOBS:-$(nproc)}"

# Where the compositor and bar go. /usr, not /usr/local, and not by taste:
# asteroidz-bar's launcher bakes its QML directory in at install time, and the
# compositor's autostart resolves `asteroidz-bar` through PATH -- so an install
# under /usr/local that shadows one under /usr silently runs the older copy.
PREFIX="${PREFIX:-/usr}"

# wlroots is the exception. It goes to /usr/local so it sits beside Ubuntu's
# libwlroots-0.19 rather than fighting it: they are different sonames and
# different pkg-config names, and anything else on the system that wants 0.19
# keeps working.
WLROOTS_PREFIX="${WLROOTS_PREFIX:-/usr/local}"

step() { printf '\n\033[1;34m==>\033[0m \033[1m%s\033[0m\n' "$*"; }
note() { printf '    %s\n' "$*"; }
die()  { printf '\n\033[1;31m==> %s\033[0m\n' "$*" >&2; exit 1; }

[ "$(id -u)" -eq 0 ] && die "run this as your normal user; it calls sudo where it needs to"
command -v sudo >/dev/null || die "sudo is required"

if [ -r /etc/os-release ]; then
	. /etc/os-release
	[ "${ID:-}" = "ubuntu" ] || note "warning: this is written for Ubuntu, found ${ID:-unknown}"
	case "${VERSION_ID:-}" in
		26.04) ;;
		*) note "warning: written for 26.04, found ${VERSION_ID:-unknown} -- package names may differ" ;;
	esac
fi

# ── packages ────────────────────────────────────────────────────────────────
#
# Split by what needs them, so a failure names the thing that will not build.

WLROOTS_DEPS=(
	# wlroots 0.20's own. libseat/libdisplay-info/libliftoff/hwdata are the DRM
	# backend's; lcms2 is colour management, which asteroidz uses directly
	# (wlr_color_manager_v1) and which is OFF by default -- an HDR compositor
	# built against a wlroots without it does not compile.
	libseat-dev libdisplay-info-dev libliftoff-dev liblcms2-dev hwdata
	libxcb-errors-dev libxcb-render-util0-dev libxcb-xfixes0-dev
	libxcb-composite0-dev libxcb-render0-dev libxcb-res0-dev libxcb-ewmh-dev
	libpng-dev libudev-dev
)

COMPOSITOR_DEPS=(
	libwayland-dev wayland-protocols libinput-dev libxkbcommon-dev
	libpixman-1-dev libdrm-dev libgbm-dev libegl-dev libgles-dev
	libpcre2-dev libcjson-dev libpango1.0-dev libgdk-pixbuf-2.0-dev
	libsystemd-dev libxcb1-dev libxcb-icccm4-dev
	# The Vulkan renderer. asteroidz builds both renderers into one binary and
	# ships a second session file for the Vulkan one, so these are not optional.
	libvulkan-dev glslang-tools
)

BAR_DEPS=(
	qt6-base-dev qt6-declarative-dev qt6-5compat-dev
	# asteroidzbg, the wallpaper, linked into the bar's QML plugin. libjxl and
	# libavif are what make it an HDR wallpaper renderer rather than a PNG one.
	libcairo2-dev libjxl-dev libavif-dev
)

RUNTIME=(
	xwayland
)

TOOLS=(build-essential meson ninja-build pkgconf git ca-certificates)

step "Installing build dependencies"
sudo apt-get update -qq
sudo apt-get install -y --no-install-recommends \
	"${TOOLS[@]}" "${WLROOTS_DEPS[@]}" "${COMPOSITOR_DEPS[@]}" \
	"${BAR_DEPS[@]}" "${RUNTIME[@]}"

# ── quickshell ──────────────────────────────────────────────────────────────
#
# The bar is a quickshell configuration, not a program: without the runtime there
# is nothing to run it. It is not in the Ubuntu archive, so it comes from the
# quickshell-git PPA.
step "Checking for quickshell"
HAVE_QUICKSHELL=1
if apt-cache policy quickshell 2>/dev/null | grep -q 'Candidate: [^(]'; then
	sudo apt-get install -y --no-install-recommends quickshell
	note "quickshell $(qs --version 2>/dev/null | head -1)"
elif command -v qs >/dev/null; then
	note "already installed outside apt: $(qs --version 2>/dev/null | head -1)"
else
	# A WARNING, not a failure. quickshell is a runtime dependency: the bar is a
	# quickshell configuration and builds against Qt alone, so everything here
	# still compiles and installs. Refusing to continue would also refuse to
	# install the compositor, which does not need it at all.
	HAVE_QUICKSHELL=0
	note "NOT FOUND -- continuing; the bar will install but will not run."
	note "It is not in the Ubuntu archive; add the quickshell-git PPA you use,"
	note "then: sudo apt-get update && sudo apt-get install quickshell"
fi

# ── wlroots 0.20 ────────────────────────────────────────────────────────────
step "wlroots $WLROOTS_TAG"
if pkg-config --atleast-version=0.20.2 wlroots-0.20 2>/dev/null; then
	note "already present: $(pkg-config --modversion wlroots-0.20)"
else
	mkdir -p "$SRC"
	if [ ! -d "$SRC/wlroots/.git" ]; then
		git clone --depth 1 --branch "$WLROOTS_TAG" \
			https://gitlab.freedesktop.org/wlroots/wlroots.git "$SRC/wlroots"
	fi
	cd "$SRC/wlroots"
	# xwayland and color-management are both explicitly enabled rather than left
	# to auto-detect: they are the two that silently come out NO when a header is
	# missing, and asteroidz fails to compile against a wlroots without either.
	meson setup build --wipe --prefix="$WLROOTS_PREFIX" --buildtype=release \
		-Dexamples=false -Dwerror=false \
		-Dxwayland=enabled -Dcolor-management=enabled
	meson compile -C build -j "$JOBS"
	sudo meson install -C build
	sudo ldconfig
fi

# Everything below looks for wlroots-0.20 through pkg-config, and on Ubuntu
# /usr/local's pkgconfig directory is not always on the default search path.
export PKG_CONFIG_PATH="$WLROOTS_PREFIX/lib/$(dpkg-architecture -qDEB_HOST_MULTIARCH 2>/dev/null || echo x86_64-linux-gnu)/pkgconfig:$WLROOTS_PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
pkg-config --exists wlroots-0.20 || die "wlroots-0.20 installed but pkg-config cannot see it (PKG_CONFIG_PATH=$PKG_CONFIG_PATH)"
note "wlroots $(pkg-config --modversion wlroots-0.20) at $WLROOTS_PREFIX"

# ── asteroidz ───────────────────────────────────────────────────────────────
step "asteroidz $ASTEROIDZ_TAG"
mkdir -p "$SRC"
if [ ! -d "$SRC/asteroidz/.git" ]; then
	git clone --branch "$ASTEROIDZ_TAG" \
		https://github.com/asteroidzman/asteroidz.git "$SRC/asteroidz"
else
	git -C "$SRC/asteroidz" fetch --tags --quiet
	git -C "$SRC/asteroidz" checkout --quiet "$ASTEROIDZ_TAG"
fi
cd "$SRC/asteroidz"
# asteroidz-scenefx is vendored in-tree and linked statically, so there is no
# separate library to fetch or keep in version lockstep.
#
# -Dtracy=false is the default and is stated anyway: a profiling build tries to
# fetch a subproject over the network.
meson setup build --wipe --prefix="$PREFIX" --sysconfdir=/etc \
	--buildtype=debugoptimized -Db_lto=true -Dtracy=false
meson compile -C build -j "$JOBS"

# The schema self-check, before installing rather than after. It drives the real
# parser and asserts every default, clamp, offset and type against it -- so a
# build that passes it is one whose config layer agrees with itself.
note "self-check: $(./build/asteroidz -S | tail -1)"
sudo meson install -C build

# ── asteroidz-bar ───────────────────────────────────────────────────────────
step "asteroidz-bar $BAR_TAG"
if [ ! -d "$SRC/asteroidz-bar/.git" ]; then
	git clone --branch "$BAR_TAG" \
		https://github.com/asteroidzman/asteroidz-bar.git "$SRC/asteroidz-bar"
else
	git -C "$SRC/asteroidz-bar" fetch --tags --quiet
	git -C "$SRC/asteroidz-bar" checkout --quiet "$BAR_TAG"
fi
cd "$SRC/asteroidz-bar"
meson setup build --wipe --prefix="$PREFIX" --buildtype=debugoptimized
meson compile -C build -j "$JOBS"
sudo meson install -C build

# ── done ────────────────────────────────────────────────────────────────────
sudo ldconfig
step "Installed"
note "compositor  $("$PREFIX/bin/asteroidz" -v)"
note "bar         $PREFIX/share/asteroidz-bar/shell.qml"
note "sessions    $(ls "$PREFIX/share/wayland-sessions"/asteroidz*.desktop 2>/dev/null | xargs -n1 basename | tr '\n' ' ')"

if [ "$HAVE_QUICKSHELL" -eq 0 ]; then
	printf '\n\033[1;33m==> The bar is installed but cannot run: quickshell is missing.\033[0m\n'
	printf '    Add the quickshell-git PPA, then: sudo apt-get install quickshell\n'
fi

cat <<EOF

Next:

  1. Log out and pick "Asteroidz" from your display manager's session list.
     A second entry runs the experimental Vulkan renderer.

  2. Copy the default config AND its palette before editing:

         mkdir -p ~/.config/asteroidz
         cp /etc/asteroidz/config.kdl /etc/asteroidz/colors.kdl ~/.config/asteroidz/

     Both, not just the first: config.kdl sources colors.kdl, and a source it
     cannot open is a fatal error rather than a warning -- so copying one and
     not the other gives you a session that will not start.

  3. Check the config parses at any time, without starting a session:

         asteroidz -p -c ~/.config/asteroidz/config.kdl

  4. The bar is started by the compositor's autostart. If it does not appear,
     run it by hand inside the session to see why:

         asteroidz-bar

  5. For wallpaper-driven theming, install matugen and wire up the template:

         see /usr/share/asteroidz-bar/matugen/README.md

     Until then the palette is whatever colors.kdl says, which is a real
     Material palette rather than a placeholder.

Because wlroots went to $WLROOTS_PREFIX rather than into a package, an Ubuntu
update that changes libwayland or libinput can leave it stale. If asteroidz stops
starting after an update, rebuild it:

    cd $SRC/wlroots && meson compile -C build && sudo meson install -C build
EOF
