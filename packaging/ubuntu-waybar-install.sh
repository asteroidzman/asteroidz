#!/usr/bin/env bash
# Install waybar + the asteroidz CFFI plugins ("pills") on Ubuntu 26.04.
#
# The pills are small GTK3 shared libraries (libNAME.so) loaded by waybar's
# `cffi` module. They build against GTK3 + gtk-layer-shell + json-glib and a
# single shared header (waybar-plugin-common/wbcommon.h). Each installs to
# ~/.local/lib/waybar/ with assets in ~/.local/share/waybar-NAME/.
#
# IMPORTANT — two things to know:
#  1. The pills need waybar built WITH cffi support. Ubuntu's package usually has
#     it; if the pills fail to load ("unknown module cffi/..."), re-run with
#     BUILD_WAYBAR=1 to compile waybar from source with -Dcffi=enabled.
#  2. Three pills are NOT on GitHub (claude-usage, discord-voice, nordvpn).
#     Copy those repos to this machine first, e.g.:
#         rsync -a ~/src/waybar-{claude-usage,discord-voice,nordvpn} \
#                  user@host:~/src/
#     then run with LOCAL_SRC=~/src (the default). Missing ones are skipped.
#
# Usage:
#   LOCAL_SRC=~/src ./ubuntu-waybar-install.sh
#   BUILD_WAYBAR=1  ./ubuntu-waybar-install.sh      # also build waybar from source
#   INSTALL_CONFIG=0 ./ubuntu-waybar-install.sh     # skip installing the dotfiles
set -euo pipefail

LOCAL_SRC="${LOCAL_SRC:-$HOME/src}"          # where local-only pill repos live
COMMON_URL="https://github.com/asteroidzman/waybar-plugin-common.git"
WORK="${WORK:-$(mktemp -d)}"
GH="https://github.com/asteroidzman"

# name|git-url   (empty url = local-only: must exist under $LOCAL_SRC)
PLUGINS=(
  "waybar-asteroidz-workspaces|$GH/waybar-asteroidz-workspaces.git"
  "waybar-media-cava|$GH/waybar-media-cava.git"
  "waybar-weather|$GH/waybar-weather.git"
  "waybar-medication|$GH/waybar-medication.git"
  "waybar-volume|$GH/waybar-volume.git"
  "waybar-display|$GH/waybar-display.git"
  "waybar-sysinfo|$GH/waybar-sysinfo.git"
  "waybar-claude-usage|"
  "waybar-discord-voice|"
  "waybar-nordvpn|"
)

SUDO=""; [ "$(id -u)" -eq 0 ] || SUDO="sudo"
log()  { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }
warn() { printf '\033[1;33m!!  %s\033[0m\n' "$*" >&2; }

# --- dependencies ------------------------------------------------------------
log "Installing dependencies (apt)"
$SUDO apt-get update
$SUDO apt-get install -y --no-install-recommends \
  build-essential make pkg-config git \
  libgtk-3-dev libgtk-layer-shell-dev libjson-glib-dev libglib2.0-dev \
  waybar \
  cava playerctl network-manager \
  pipewire pipewire-pulse wireplumber python3 curl

# --- optional: build waybar from source with cffi ----------------------------
if [ "${BUILD_WAYBAR:-0}" = "1" ]; then
  log "Building waybar from source with -Dcffi=enabled"
  $SUDO apt-get install -y meson ninja-build
  $SUDO apt-get build-dep -y waybar || warn "build-dep failed (enable deb-src / 'Types: deb deb-src')"
  git clone --depth 1 https://github.com/Alexays/Waybar.git "$WORK/Waybar"
  meson setup "$WORK/Waybar/build" "$WORK/Waybar" --prefix=/usr -Dcffi=enabled
  ninja -C "$WORK/Waybar/build"
  $SUDO ninja -C "$WORK/Waybar/build" install
fi

# --- shared header (build all pills against one copy) ------------------------
log "Fetching shared header (waybar-plugin-common)"
git clone --depth 1 "$COMMON_URL" "$WORK/common"
COMMON="$WORK/common"                        # contains wbcommon.h

# --- build + install each pill ----------------------------------------------
installed=(); skipped=()
for entry in "${PLUGINS[@]}"; do
  name="${entry%%|*}"; url="${entry#*|}"
  if [ -d "$LOCAL_SRC/$name" ]; then
    dir="$LOCAL_SRC/$name"                   # prefer a local copy
  elif [ -n "$url" ]; then
    git clone --depth 1 "$url" "$WORK/$name"; dir="$WORK/$name"
  else
    warn "skip $name — local-only and not found in $LOCAL_SRC (copy it over)"
    skipped+=("$name"); continue
  fi
  log "Building $name"
  make -C "$dir" WBCOMMON="$COMMON" clean >/dev/null 2>&1 || true
  make -C "$dir" WBCOMMON="$COMMON"
  make -C "$dir" WBCOMMON="$COMMON" install
  installed+=("$name")
done

# --- install the waybar config (dotfiles), unless INSTALL_CONFIG=0 -----------
if [ "${INSTALL_CONFIG:-1}" = "1" ]; then
  log "Installing waybar config (waybar-config -> ~/.config/waybar)"
  if [ -d "$LOCAL_SRC/waybar-config" ]; then
    cfg="$LOCAL_SRC/waybar-config"
  else
    git clone --depth 1 "$GH/waybar-config.git" "$WORK/waybar-config"
    cfg="$WORK/waybar-config"
  fi
  ( cd "$cfg" && ./install.sh )        # copies config.jsonc/style.css/colors.css, expands @HOME@
else
  warn "Skipped waybar config install (INSTALL_CONFIG=0)"
fi

# --- summary -----------------------------------------------------------------
log "Done."
printf 'Installed (%d): %s\n' "${#installed[@]}" "${installed[*]:-none}"
[ "${#skipped[@]}" -gt 0 ] && warn "Skipped local-only: ${skipped[*]} (copy to $LOCAL_SRC and re-run)"
cat <<'EOF'

Plugins are in ~/.local/lib/waybar/  (assets in ~/.local/share/waybar-*/).
The config was installed to ~/.config/waybar (unless INSTALL_CONFIG=0). Restart
waybar to load it. Trim any pill modules you skipped from config.jsonc.

Runtime tools some pills expect: cava + playerctl (media), wireplumber/pactl
(volume), NetworkManager/nmcli (sysinfo). The nordvpn pill needs the `nordvpn`
CLI (install from NordVPN's own repo — not in apt). claude-usage needs python3.
If a pill fails to load with "unknown module cffi/...", rebuild waybar:
    BUILD_WAYBAR=1 ./ubuntu-waybar-install.sh
EOF
