#!/usr/bin/env bash
# waybar-popup-test.sh — headless test harness for waybar + the custom CFFI
# plugins (waybar-sysmon, waybar-weather, waybar-display): launches a second,
# fully isolated asteroidz instance plus a waybar pointed at freshly-built
# plugin .so's, locates each module's on-screen pill via the plugins' own
# WBTEST_DUMP_GEOM hook (no hardcoded coordinates -- works no matter how the
# bar is laid out), clicks it with a real Wayland virtual-pointer event
# (contrib/wlvptr -- NOT ydotool, which goes through uinput/the live seat and
# would leak into whichever session is actually active, not this headless
# one), screenshots the open popup, and leaves everything under $OUTDIR.
#
# Requires each plugin to be built with wbpop_enable_geom_dump() wired up
# (already the case for sysmon/weather/display as of the WBTEST_GEOM commit)
# and contrib/wlvptr/wlvptr built (cd contrib/wlvptr && make).
#
# Usage: contrib/waybar-popup-test.sh [module...]
#   module   one or more of: sysmon weather display (default: all three)
# Env:
#   OUTDIR      work dir (default /tmp/asteroidz-waybar-test)
#   ASTEROIDZ   compositor binary (default: build/asteroidz next to this
#               repo, falling back to /usr/bin/asteroidz)
#   PLUGIN_DIR_<module>  override the .so search dir for one module, e.g.
#               PLUGIN_DIR_sysmon=/path/to/waybar-sysmon
set -u

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUTDIR="${OUTDIR:-/tmp/asteroidz-waybar-test}"
ASTEROIDZ="${ASTEROIDZ:-$REPO/build/asteroidz}"
[ -x "$ASTEROIDZ" ] || ASTEROIDZ=/usr/bin/asteroidz
WLVPTR="$REPO/contrib/wlvptr/wlvptr"
OUTW=1600
OUTH=1000

MODULES=("$@")
[ ${#MODULES[@]} -eq 0 ] && MODULES=(sysmon weather display)

so_for() { # $1=module -> prints the .so path to use
	local m="$1" override_var="PLUGIN_DIR_$m" dir
	dir="${!override_var:-$HOME/src/waybar-$m}"
	echo "$dir/lib$m.so"
}

for t in grim convert swaybg; do
	command -v "$t" >/dev/null || { echo "missing tool: $t" >&2; exit 1; }
done
[ -x "$ASTEROIDZ" ] || { echo "no asteroidz binary at $ASTEROIDZ" >&2; exit 1; }
[ -x "$WLVPTR" ] || { echo "wlvptr not built -- run: cd contrib/wlvptr && make" >&2; exit 1; }
FAIL=0
for m in "${MODULES[@]}"; do
	so="$(so_for "$m")"
	[ -f "$so" ] || { echo "missing plugin .so for '$m': $so" >&2; FAIL=1; }
done
[ "$FAIL" = 1 ] && exit 1

mkdir -p "$OUTDIR"

# --- compositor config: blur + shadow + layer_shadows all on, so this
# exercises the same effects path waybar popups hit live. ---
CONFIG="$OUTDIR/config.kdl"
cat > "$CONFIG" <<'EOF'
border_radius 10
borderpx 0
shadows 1
layer_shadows 1
shadows_contact 0
shadows_size 32
shadows_blur 32
shadowscolor 0x00000090
shadows_blur_background 1
shadows_blur_background_strength 0.55
effects {
	blur { enable 1; optimized 1; passes 2; radius 6;
		params { noise 0.02; brightness 0.9; contrast 0.9; saturation 1.2; } }
}
theme { bg-color 0x101010ff; fg-color 0x00315fff; focus-bg-color 0x101010ff; focus-fg-color 0x00315fff }
output HEADLESS-1 { width 1600; height 1000; refresh 60 }
misc {
	layerrule layer_name:waybar-popup,forceshadow:1
}
layout { titlebar { enable 0 } }
tag 1 { layout monocle; name t1 }
EOF

# --- waybar config: just the modules under test, pointed at freshly-built
# .so's (never the installed ~/.local/lib/waybar copies -- this must never
# touch what the live bar is running). ---
WBCONFIG="$OUTDIR/waybar-config.jsonc"
{
	printf '{ "layer": "top", "position": "top", "height": 40,\n'
	printf '  "modules-left": [], "modules-center": [],\n'
	printf '  "modules-right": ['
	first=1
	for m in "${MODULES[@]}"; do
		[ "$first" = 1 ] && first=0 || printf ', '
		printf '"cffi/%s"' "$m"
	done
	printf '],\n'
	for m in "${MODULES[@]}"; do
		printf '  "cffi/%s": { "module_path": "%s", "icon-size": 24 },\n' "$m" "$(so_for "$m")"
	done
	printf '  "_dummy": null }\n'
} > "$WBCONFIG"

WBSTYLE="$HOME/.config/waybar/style.css"
[ -f "$WBSTYLE" ] || { WBSTYLE="$OUTDIR/empty-style.css"; : > "$WBSTYLE"; }

# --- striped high-frequency test wallpaper: blur/shadow bugs (unblurred
# rectangles, stale-size shadows) are visually obvious against stripes but
# easy to miss against a flat colour. ---
WALLPAPER="$OUTDIR/wallpaper.png"
[ -f "$WALLPAPER" ] || convert -size "${OUTW}x${OUTH}" 'xc:' \
	-fill white -draw "rectangle 0,0 ${OUTW},${OUTH}" \
	\( -size "${OUTW}x${OUTH}" tile:pattern:HS45 -fill '#ff2a8f' -opaque white \) \
	-compose over -composite "$WALLPAPER" 2>/dev/null || \
	convert -size "${OUTW}x${OUTH}" plasma:fractal "$WALLPAPER"

pkill -f "asteroidz -c $CONFIG" 2>/dev/null
pkill -f "waybar -c $WBCONFIG" 2>/dev/null
sleep 0.5
TESTRT="$OUTDIR/xdg"
rm -rf "$TESTRT"; mkdir -p "$TESTRT"; chmod 700 "$TESTRT"

env -i HOME="$HOME" PATH="$PATH" XDG_RUNTIME_DIR="$TESTRT" \
	WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=gles2 \
	"$ASTEROIDZ" -c "$CONFIG" > "$OUTDIR/comp-stdout.log" 2>&1 &
COMP=$!

SOCK=""
for _ in $(seq 1 40); do
	sleep 0.2
	SOCK="$(ls "$TESTRT"/wayland-* 2>/dev/null | grep -v '\.lock$' | head -1 | xargs -r basename)"
	[ -n "$SOCK" ] && break
done
if [ -z "$SOCK" ]; then
	echo "test compositor did not create a socket; see $OUTDIR/comp-stdout.log" >&2
	kill "$COMP" 2>/dev/null
	exit 1
fi
export XDG_RUNTIME_DIR="$TESTRT" WAYLAND_DISPLAY="$SOCK"

# monitor offset (0,0 for a single headless output, but don't assume it)
read -r MON_X MON_Y <<<"$(amsg get all-monitors 2>/dev/null | python3 -c '
import json,sys
d=json.load(sys.stdin)
m=d["monitors"][0]
print(m["x"], m["y"])
' 2>/dev/null)"
MON_X="${MON_X:-0}"
MON_Y="${MON_Y:-0}"

# swaybg, NOT mpvpaper: a video wallpaper's per-frame damage defeats the
# shadow render path entirely (verified headlessly -- a continuously
# re-decoded video buffer behind a shadowed window left the shadow
# consistently invisible across many frames, while a static swaybg image
# showed it immediately; root cause not fully chased, but it's a wallpaper-
# client artifact, not a compositor bug -- steer clear of video wallpapers
# in any headless effects verification).
swaybg -o HEADLESS-1 -i "$WALLPAPER" -m fill > "$OUTDIR/swaybg.log" 2>&1 &
MPV=$!
sleep 1

GEOMLOG="$OUTDIR/waybar-stderr.log"
: > "$GEOMLOG"
WBTEST_DUMP_GEOM=1 waybar -c "$WBCONFIG" -s "$WBSTYLE" > "$GEOMLOG" 2>&1 &
WB=$!

# give waybar+plugins time to map + do their first layout pass
sleep 1.5

teardown() {
	for pid in "${MPV:-}" "${WB:-}"; do
		[ -n "$pid" ] && kill "$pid" 2>/dev/null
	done
	pkill -f "asteroidz -c $CONFIG" 2>/dev/null
}
trap teardown EXIT

# wait_stable_geom <module> -- right-aligned modules-right/group boxes get
# reflowed a few times after the bar first maps (initial alloc is often a
# transient (0,0), before waybar's right-alignment settles), so poll until
# the SAME line repeats twice in a row instead of trusting the first one.
wait_stable_geom() {
	local m="$1" prev="" cur="" i
	for i in $(seq 1 40); do
		cur="$(grep "WBTEST_GEOM name=$m " "$GEOMLOG" | tail -1)"
		if [ -n "$cur" ] && [ "$cur" = "$prev" ]; then
			echo "$cur"
			return 0
		fi
		prev="$cur"
		sleep 0.2
	done
	echo "$cur"
}

# latest_geom <module> -- fresh (not cached) read of the module's current
# pill geometry, for re-reading right before each click attempt: a periodic
# data refresh (e.g. sysmon's 3s interval) can reflow a pill's width right
# as we click, so a coordinate computed even a second earlier can go stale.
latest_geom() { grep "WBTEST_GEOM name=$1 " "$GEOMLOG" | tail -1; }

click_center_of() { # $1=geom line -> clicks its pill's center
	local ln="$1"
	local x y w h
	x=$(sed -n 's/.*\bx=\([0-9-]*\).*/\1/p' <<<"$ln")
	y=$(sed -n 's/.*\by=\([0-9-]*\).*/\1/p' <<<"$ln")
	w=$(sed -n 's/.*\bw=\([0-9-]*\).*/\1/p' <<<"$ln")
	h=$(sed -n 's/.*\bh=\([0-9-]*\).*/\1/p' <<<"$ln")
	"$WLVPTR" "$(( MON_X + x + w/2 ))" "$(( MON_Y + y + h/2 ))" "$OUTW" "$OUTH" click
}

RESULT=0
PREV_MODULE=""
LAST_SHOT_SUM=""
for m in "${MODULES[@]}"; do
	line="$(wait_stable_geom "$m")"
	if [ -z "$line" ]; then
		echo "[$m] FAIL: no WBTEST_GEOM line seen -- module didn't map, or geom-dump hook missing (rebuild the plugin)" >&2
		RESULT=1
		continue
	fi

	# dismiss whatever popup a PREVIOUS module left open -- singleton popup
	# logic means last_open_surface can still read "waybar-popup" from a
	# stale prior popup even if THIS module's click missed entirely, which
	# would otherwise read as a false OK. A click on empty bar/background
	# space does NOT reliably close it (the popup only closes on Escape or
	# on actually LOSING keyboard focus to another focusable surface, and a
	# background layer surface isn't focusable) -- toggle it closed the same
	# way a user would: click that same module's own pill again.
	if [ -n "$PREV_MODULE" ]; then
		prev_line="$(latest_geom "$PREV_MODULE")"
		[ -n "$prev_line" ] && click_center_of "$prev_line"
		sleep 0.5
	fi

	opened=0
	for attempt in 1 2 3; do
		line="$(latest_geom "$m")"
		echo "[$m] attempt $attempt: pill geom $line"
		click_center_of "$line"
		sleep 1
		surf="$(amsg get last_open_surface 2>/dev/null)"
		if [[ "$surf" == *waybar-popup* ]]; then
			opened=1
			break
		fi
		echo "[$m] attempt $attempt missed (last_open_surface=$surf), retrying" >&2
	done

	grim "$OUTDIR/popup-$m.png" 2>/dev/null
	# last_open_surface only updates on MAP, never on unmap, so it can still
	# read "waybar-popup" from a stale prior module's popup that was toggled
	# closed above but never overwritten -- it can't tell "opened just now"
	# from "opened at some point, still stale". Cross-check against the
	# actual screenshot: an identical image to a still-open PREVIOUS module's
	# popup means nothing actually changed on screen, whatever last_open_surface
	# claims.
	if [ "$opened" = 1 ] && [ -n "$LAST_SHOT_SUM" ] && \
			[ "$(md5sum < "$OUTDIR/popup-$m.png")" = "$LAST_SHOT_SUM" ]; then
		echo "[$m] FAIL: last_open_surface said waybar-popup, but the screenshot is byte-identical to the previous module's -- click missed, stale popup still showing" >&2
		opened=0
	fi
	LAST_SHOT_SUM="$(md5sum < "$OUTDIR/popup-$m.png")"

	if [ "$opened" = 1 ]; then
		echo "[$m] OK: popup opened, screenshot at $OUTDIR/popup-$m.png"
		PREV_MODULE="$m"
	else
		echo "[$m] FAIL: popup never opened after 3 attempts; screenshot saved anyway" >&2
		RESULT=1
		PREV_MODULE=""
	fi
done

echo "artifacts in $OUTDIR (config.kdl, waybar-config.jsonc, waybar-stderr.log, popup-*.png)"
exit "$RESULT"
