# Maintainer: ralf <ralf.wierzbicki@gmail.com>
pkgname=asteroidz
pkgver=0.15.3
pkgrel=1
pkgdesc='wlroots compositor with a Vulkan renderer, HDR10, and dwm-style tags'
arch=('x86_64')
url='https://github.com/asteroidzman/asteroidz'
license=('custom')
depends=(
  'wlroots0.20' 'wayland' 'libinput' 'libxkbcommon' 'pcre2' 'pixman'
  'cjson' 'pango' 'gdk-pixbuf2' 'libdrm' 'systemd-libs'
  'vulkan-icd-loader'
  'xcb-util-wm' 'libxcb'
  'asteroidz-scenefx'
)
makedepends=('meson' 'ninja' 'wayland-protocols' 'vulkan-headers' 'glslang' 'git')
optdepends=(
  'xorg-xwayland: run X11 applications under XWayland'
  'asteroidz-dms: matching Material 3 shell/bar'
)
# Default session runs the Vulkan renderer; an "Asteroidz (GLES fallback)"
# session (WLR_RENDERER=gles2) is also installed. One binary, both renderers.
source=("git+$url.git#tag=$pkgver")
sha256sums=('SKIP')

build() {
  arch-meson "$pkgname" build \
    -Db_lto=true
  meson compile -C build
}

package() {
  meson install -C build --destdir "$pkgdir"
  install -Dm644 "$pkgname/LICENSE" "$pkgdir/usr/share/licenses/$pkgname/LICENSE"
  # forked-from license texts (dwl/dwm/sway/wlroots/tinywl)
  for l in "$srcdir/$pkgname"/LICENSE.*; do
    install -Dm644 "$l" "$pkgdir/usr/share/licenses/$pkgname/$(basename "$l")"
  done
}
