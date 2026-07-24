#!/usr/bin/env bash
# Install asteroidz on Ubuntu 26.04 from source.
#
# Ubuntu packages neither wlroots 0.20 nor the asteroidz-scenefx fork, so this
# builds and installs all three (wlroots -> asteroidz-scenefx -> asteroidz) into
# /usr. Mirrors packaging/deb/build.sh, minus the .deb/fpm packaging step.
#
# Usage:   ./ubuntu-install.sh
# Pin refs (optional):
#   WLROOTS_TAG=0.20.1 SCENEFX_REF=main ASTEROIDZ_REF=main ./ubuntu-install.sh
# Build against a local asteroidz checkout instead of cloning:
#   ASTEROIDZ_SRC=/path/to/asteroidz ./ubuntu-install.sh
set -euo pipefail

WLROOTS_TAG="${WLROOTS_TAG:-0.20.1}"
SCENEFX_REPO="${SCENEFX_REPO:-https://github.com/asteroidzman/asteroidz-scenefx.git}"
SCENEFX_REF="${SCENEFX_REF:-main}"
ASTEROIDZ_REPO="${ASTEROIDZ_REPO:-https://github.com/asteroidzman/asteroidz.git}"
ASTEROIDZ_REF="${ASTEROIDZ_REF:-main}"
ASTEROIDZ_SRC="${ASTEROIDZ_SRC:-}"          # set to a local checkout to skip cloning
WORK="${WORK:-$(mktemp -d)}"

SUDO=""; [ "$(id -u)" -eq 0 ] || SUDO="sudo"
log() { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }

# --- sanity ------------------------------------------------------------------
. /etc/os-release 2>/dev/null || true
[ "${ID:-}" = "ubuntu" ] || echo "warning: not Ubuntu (ID=${ID:-?}); package names assume Ubuntu 26.04." >&2

# --- 0. dependencies ---------------------------------------------------------
log "Installing build + runtime dependencies (apt)"
$SUDO apt-get update
$SUDO apt-get install -y --no-install-recommends \
  build-essential meson ninja-build pkg-config cmake git python3 \
  wayland-protocols libwayland-bin glslang-tools hwdata \
  libwayland-dev libdrm-dev libxkbcommon-dev libpixman-1-dev libudev-dev \
  libgbm-dev libseat-dev libinput-dev libdisplay-info-dev \
  libvulkan-dev libegl-dev libgles-dev \
  libpcre2-dev libcjson-dev libpango1.0-dev libcairo2-dev \
  libgdk-pixbuf-2.0-dev libsystemd-dev \
  libxcb1-dev libxcb-composite0-dev libxcb-icccm4-dev libxcb-render0-dev \
  libxcb-res0-dev libxcb-xfixes0-dev libxcb-errors-dev \
  xwayland

# build "$srcdir" into "$srcdir/build" with the given meson args, then install
build_install() {
  local src="$1"; shift
  rm -rf "$src/build"
  meson setup "$src/build" "$src" --prefix=/usr "$@"
  ninja -C "$src/build"
  $SUDO meson install -C "$src/build"
  $SUDO ldconfig
}

# --- 1. wlroots 0.20 ---------------------------------------------------------
log "Building wlroots $WLROOTS_TAG"
git clone --depth 1 --branch "$WLROOTS_TAG" \
  https://gitlab.freedesktop.org/wlroots/wlroots.git "$WORK/wlroots"
build_install "$WORK/wlroots" \
  --buildtype=release -Dexamples=false -Dwerror=false -Dxwayland=enabled

# --- 2. asteroidz-scenefx ----------------------------------------------------
log "Building asteroidz-scenefx ($SCENEFX_REF)"
git clone --depth 1 --branch "$SCENEFX_REF" "$SCENEFX_REPO" "$WORK/scenefx"
build_install "$WORK/scenefx" \
  --buildtype=release -Db_lto=true -Drenderers=gles2,vulkan -Dexamples=false

# --- 3. asteroidz ------------------------------------------------------------
log "Building asteroidz ($ASTEROIDZ_REF)"
if [ -n "$ASTEROIDZ_SRC" ]; then
  src="$ASTEROIDZ_SRC"
else
  git clone --depth 1 --branch "$ASTEROIDZ_REF" "$ASTEROIDZ_REPO" "$WORK/asteroidz"
  src="$WORK/asteroidz"
fi
build_install "$src" --sysconfdir=/etc --buildtype=release -Db_lto=true

log "Done."
cat <<'EOF'

Installed: asteroidz + amsg, both wayland sessions (Asteroidz [GLES2, default]
and Asteroidz (Vulkan, experimental)), and the GlobalShortcuts portal.

Next:
  * Log out and pick "Asteroidz" from your display manager's session list
    (or run `dbus-run-session asteroidz` from a TTY).
  * The default config was installed to /etc/asteroidz/config.kdl; copy it to
    ~/.config/asteroidz/config.kdl to customise.
  * The bar (waybar + CFFI plugins) and dotfiles are separate — see the repo
    README for the plugin list.
EOF
