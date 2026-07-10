#!/usr/bin/env bash
# Build a .deb of asteroidz on Ubuntu, along with the two prerequisite .debs it
# needs (wlroots-0.20 and asteroidz-scenefx), neither of which Ubuntu packages.
# All three land in $OUT (default: ./dist), so the set is installable as one.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO"

WLROOTS_TAG="${WLROOTS_TAG:-0.20.1}"
SCENEFX_REPO="${SCENEFX_REPO:-https://github.com/asteroidzman/asteroidz-scenefx.git}"
SCENEFX_REF="${SCENEFX_REF:-main}"
ARCH="$(dpkg --print-architecture)"
OUT="${OUT:-$REPO/dist}"; mkdir -p "$OUT"
MAINTAINER="${MAINTAINER:-ralf <ralf.wierzbicki@gmail.com>}"
SUDO=""; [ "$(id -u)" -eq 0 ] || SUDO="sudo"   # no sudo needed inside a root container

meson_version() { meson introspect "$1" --projectinfo | python3 -c 'import sys,json;print(json.load(sys.stdin)["version"])'; }

# --- 1. wlroots 0.20 ---------------------------------------------------------
echo "::group::build wlroots $WLROOTS_TAG"
rm -rf /tmp/wlroots
git clone --depth 1 --branch "$WLROOTS_TAG" \
  https://gitlab.freedesktop.org/wlroots/wlroots.git /tmp/wlroots
meson setup /tmp/wlroots/build /tmp/wlroots --prefix=/usr \
  --buildtype=release -Dexamples=false -Dwerror=false -Dxwayland=enabled
ninja -C /tmp/wlroots/build
DESTDIR=/tmp/wlroots/pkgroot meson install -C /tmp/wlroots/build
fpm -s dir -t deb -f -n wlroots-0.20 -v "$WLROOTS_TAG" --iteration 1 -a "$ARCH" \
  -m "$MAINTAINER" --license MIT \
  --url https://gitlab.freedesktop.org/wlroots/wlroots \
  --description "wlroots 0.20 library, built from source (dependency of asteroidz)" \
  -d libwayland-server0 -d libdrm2 -d libxkbcommon0 -d libpixman-1-0 \
  -d libinput10 -d libgbm1 -d libseat1 -d libvulkan1 -d libegl1 \
  -p "$OUT/wlroots-0.20_${WLROOTS_TAG}-1_${ARCH}.deb" \
  -C /tmp/wlroots/pkgroot usr
$SUDO meson install -C /tmp/wlroots/build
$SUDO ldconfig
echo "::endgroup::"

# --- 2. asteroidz-scenefx (build dependency of asteroidz) --------------------
echo "::group::build asteroidz-scenefx"
rm -rf /tmp/scenefx
git clone --depth 1 --branch "$SCENEFX_REF" "$SCENEFX_REPO" /tmp/scenefx
meson setup /tmp/scenefx/build /tmp/scenefx --prefix=/usr \
  --buildtype=release -Db_lto=true -Drenderers=gles2,vulkan -Dexamples=false
ninja -C /tmp/scenefx/build
SFX_VER="$(meson_version /tmp/scenefx/build)"
DESTDIR=/tmp/scenefx/pkgroot meson install -C /tmp/scenefx/build
fpm -s dir -t deb -f -n asteroidz-scenefx -v "$SFX_VER" --iteration 1 -a "$ARCH" \
  -m "$MAINTAINER" --license MIT \
  --url https://github.com/asteroidzman/asteroidz-scenefx \
  --description "scenefx fork for asteroidz — wlroots effects library (GLES2 + Vulkan/fx_vk)" \
  -d "wlroots-0.20 (>= ${WLROOTS_TAG})" \
  -d libwayland-server0 -d libdrm2 -d libxkbcommon0 -d libpixman-1-0 \
  -d libvulkan1 -d libegl1 -d libgles2 \
  -p "$OUT/asteroidz-scenefx_${SFX_VER}-1_${ARCH}.deb" \
  -C /tmp/scenefx/pkgroot usr
$SUDO meson install -C /tmp/scenefx/build
$SUDO ldconfig
echo "::endgroup::"

# --- 3. asteroidz ------------------------------------------------------------
echo "::group::build asteroidz"
rm -rf build pkgroot
meson setup build --prefix=/usr --sysconfdir=/etc --buildtype=release -Db_lto=true
ninja -C build
VER="$(meson_version build)"
DESTDIR="$REPO/pkgroot" meson install -C build
fpm -s dir -t deb -f -n asteroidz -v "$VER" --iteration 1 -a "$ARCH" \
  -m "$MAINTAINER" --license custom \
  --url https://github.com/asteroidzman/asteroidz \
  --description "wlroots compositor with a Vulkan renderer, HDR10, and dwm-style tags" \
  -d "asteroidz-scenefx (>= ${SFX_VER})" -d "wlroots-0.20 (>= ${WLROOTS_TAG})" \
  -d libwayland-server0 -d libinput10 -d libxkbcommon0 -d libpcre2-8-0 \
  -d libpixman-1-0 -d libcjson1 -d libpango-1.0-0 -d libpangocairo-1.0-0 \
  -d libgdk-pixbuf-2.0-0 -d libsystemd0 -d libdrm2 -d libvulkan1 \
  -d libxcb1 -d libxcb-icccm4 \
  --deb-recommends xwayland --deb-suggests asteroidz-dms \
  -p "$OUT/asteroidz_${VER}-1_${ARCH}.deb" \
  -C "$REPO/pkgroot" usr etc
echo "::endgroup::"

echo "Built packages:"; ls -1 "$OUT"/*.deb
