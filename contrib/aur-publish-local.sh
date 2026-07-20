#!/usr/bin/env bash
# aur-publish-local.sh REPO_DIR VERSION -- publish a package to the AUR
# directly from this machine, no GitHub Actions involved.
#
# Bumps PKGBUILD's pkgver (and resets pkgrel=1) to VERSION, test-builds it
# for real via makepkg IN AN ISOLATED TEMP COPY (never in REPO_DIR itself --
# makepkg's own $srcdir/$pkgname build tree can collide with a repo's own
# top-level src/ directory and rm -rf it; this bit waybar-sysmon once,
# recovered cleanly via git checkout, but must never happen again), commits
# the bump back to REPO_DIR's own git repo, then pushes PKGBUILD + a freshly
# generated .SRCINFO to the AUR git repo (ssh://aur@aur.archlinux.org/
# <pkgname>.git -- reads pkgname from PKGBUILD).
#
# Needs: an SSH key already registered on your AUR account, present in the
# default ssh-agent/identity (same one used for `ssh aur@aur.archlinux.org`).
set -euo pipefail

REPO_DIR="${1:?usage: aur-publish-local.sh REPO_DIR VERSION}"
VERSION="${2:?usage: aur-publish-local.sh REPO_DIR VERSION}"
VERSION="${VERSION#v}"

REPO_DIR="$(cd "$REPO_DIR" && pwd)"
[ -f "$REPO_DIR/PKGBUILD" ] || { echo "no PKGBUILD in $REPO_DIR" >&2; exit 1; }

PKGNAME="$(sed -n 's/^pkgname=//p' "$REPO_DIR/PKGBUILD")"
echo "==> $PKGNAME -> $VERSION"

BUILD_DIR="$(mktemp -d)"
AUR_DIR="$(mktemp -d)"
trap 'rm -rf "$BUILD_DIR" "$AUR_DIR"' EXIT

# PKGBUILD's source=() is a fresh git+URL fetch of the release tag, so
# nothing else from REPO_DIR is needed here -- only the PKGBUILD itself.
# This also means BUILD_DIR has nothing to collide with (see header note).
echo "==> isolated build dir: $BUILD_DIR"
cp "$REPO_DIR/PKGBUILD" "$BUILD_DIR/PKGBUILD"

cd "$BUILD_DIR"
sed -i "s/^pkgver=.*/pkgver=${VERSION}/" PKGBUILD
sed -i "s/^pkgrel=.*/pkgrel=1/" PKGBUILD

echo "==> test-building $PKGNAME $VERSION locally (makepkg -f, isolated)"
makepkg -f --noconfirm --nocheck --clean

echo "==> preparing AUR repo for $PKGNAME"
if ! GIT_SSH_COMMAND="ssh -o BatchMode=yes" git clone -q "ssh://aur@aur.archlinux.org/$PKGNAME.git" "$AUR_DIR/repo" 2>/dev/null; then
  echo "  (new package, AUR repo is empty -- initializing)"
  mkdir -p "$AUR_DIR/repo"
  git -C "$AUR_DIR/repo" init -q -b master
  git -C "$AUR_DIR/repo" remote add origin "ssh://aur@aur.archlinux.org/$PKGNAME.git"
fi

cp PKGBUILD "$AUR_DIR/repo/"
(
  cd "$AUR_DIR/repo"
  makepkg --printsrcinfo > .SRCINFO
  git add PKGBUILD .SRCINFO
  git -c user.name="asteroidzman" -c user.email="asteroidzman@users.noreply.github.com" \
    commit -q -m "Update to $VERSION" || echo "  (nothing changed on the AUR side)"
  GIT_SSH_COMMAND="ssh -o BatchMode=yes" git push -q origin HEAD:master
)

echo "==> committing the pkgver bump back to $REPO_DIR"
cd "$REPO_DIR"
sed -i "s/^pkgver=.*/pkgver=${VERSION}/" PKGBUILD
sed -i "s/^pkgrel=.*/pkgrel=1/" PKGBUILD
git add PKGBUILD
if ! git diff --cached --quiet; then
  git commit -q -m "PKGBUILD: bump to $VERSION"
  git push -q
else
  echo "  (PKGBUILD already at $VERSION, nothing to commit)"
fi

echo "==> done: https://aur.archlinux.org/packages/$PKGNAME"
