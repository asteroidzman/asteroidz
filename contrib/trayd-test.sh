#!/usr/bin/env bash
# trayd-test.sh — asteroidz-trayd, the out-of-process StatusNotifierItem host.
#
# Runs against its OWN dbus-daemon and XDG_RUNTIME_DIR, so it never touches the
# live session's tray, and needs no tray application installed --
# contrib/snitem stands in for one. No compositor is involved: trayd is a plain
# program that reads clicks on stdin and writes items on stdout, which is
# exactly what makes it testable without one.
#
# What is pinned here:
#
#   ADOPTION   an item already on the bus when trayd starts must be picked up
#              without the application doing anything, because plenty of
#              applications register exactly once at startup and never again.
#
#   THE CAP    the reason this program exists. A tray host decodes pixmaps
#              whose dimensions the APPLICATION chooses; in the compositor that
#              is an unbounded amount of work on the event loop, driven by
#              whatever you happen to have installed. An oversized pixmap must
#              be refused rather than decoded, and the item must simply not
#              appear rather than taking the host down with it.
#
#   CLICKS     a click on stdin has to reach the item as Activate with the
#              screen coordinates intact -- the spec passes them so the
#              application can place its own window next to the icon, and that
#              is why clicks travel over the event channel rather than being a
#              shell command.
set -u

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TRAYD="${TRAYD:-$REPO/build/asteroidz-trayd}"
SNITEM="$REPO/contrib/snitem/snitem"
PASS=0
FAIL=0

ok() { echo "  ok - $1"; PASS=$((PASS + 1)); }
no() { echo "  FAIL - $1"; FAIL=$((FAIL + 1)); }
check() { if [ "$1" = true ]; then ok "$2"; else no "$2"; fi; }

[ -x "$TRAYD" ] || { echo "no $TRAYD -- build it first"; exit 1; }
[ -x "$SNITEM" ] || { echo "building snitem"; make -C "$REPO/contrib/snitem" >/dev/null || exit 1; }
command -v dbus-run-session >/dev/null || { echo "dbus-run-session not found"; exit 1; }

# Everything below runs inside one private session bus per case, because a
# watcher name can only be claimed once and a stale claim would make the next
# case test the wrong role.
run_case() { dbus-run-session -- "$0" --case "$@"; }

# --- the cases, each re-entered with its own bus ---------------------------
if [ "${1:-}" = "--case" ]; then
	shift
	CASE="$1"; shift
	D="$(mktemp -d)"
	trap 'rm -rf "$D"' EXIT
	export XDG_RUNTIME_DIR="$D"

	case "$CASE" in
	adopt|pixmap|toobig)
		args=()
		[ "$CASE" = pixmap ] && args=(--pixmap 48)
		[ "$CASE" = toobig ] && args=(--pixmap 4096)
		# Started BEFORE trayd and without --register: the item is only a name
		# on the bus, which is the state an application is left in when the
		# host restarts under it.
		"$SNITEM" "${args[@]}" >/dev/null 2>&1 &
		item=$!
		sleep 1.5
		timeout 8 "$TRAYD" </dev/null >"$D/out" 2>"$D/err" &
		trayd=$!
		sleep 3
		tail -1 "$D/out"
		echo "---PNG---"
		ls "$D/asteroidz-tray"/*.png 2>/dev/null | wc -l
		kill $trayd $item 2>/dev/null
		;;
	click)
		"$SNITEM" --log "$D/clicks" >/dev/null 2>&1 &
		item=$!
		sleep 1.5
		mkfifo "$D/in"
		timeout 8 "$TRAYD" <"$D/in" >"$D/out" 2>"$D/err" &
		trayd=$!
		exec 9>"$D/in"
		sleep 2.5
		id="$(grep -o '"id":"[^"]*"' "$D/out" | tail -1 | cut -d'"' -f4)"
		printf '{"event":"click","button":"left","item":"%s","x":137,"y":19}\n' "$id" >&9
		sleep 1
		printf '{"event":"click","button":"right","item":"%s","x":137,"y":19}\n' "$id" >&9
		sleep 1
		exec 9>&-
		cat "$D/clicks" 2>/dev/null
		kill $trayd $item 2>/dev/null
		;;
	esac
	wait 2>/dev/null
	exit 0
fi

# --- assertions ------------------------------------------------------------
echo "-- adoption"
out="$(run_case adopt)"
line="$(echo "$out" | head -1)"
check "$(echo "$line" | grep -q '"id":"org.kde.StatusNotifierItem-' && echo true || echo false)" \
	"an item already on the bus is adopted without registering ($line)"
check "$(echo "$line" | grep -q '"icon":"dialog-information"' && echo true || echo false)" \
	"and its IconName is passed through as a theme name for the bar to resolve"

echo "-- pixmap decode"
out="$(run_case pixmap)"
line="$(echo "$out" | head -1)"
pngs="$(echo "$out" | tail -1)"
check "$(echo "$line" | grep -q '"icon":"/' && echo true || echo false)" \
	"a 48x48 IconPixmap becomes a PNG on disk, named by absolute path"
check "$([ "${pngs:-0}" -eq 1 ] && echo true || echo false)" \
	"and exactly one file was written ($pngs)"

echo "-- the size cap"
out="$(run_case toobig)"
line="$(echo "$out" | head -1)"
pngs="$(echo "$out" | tail -1)"
# 4096x4096 is 67MB of ARGB and 16.7 million pixels of decode. Refused
# outright: the item shows nothing rather than the host doing the work.
check "$(echo "$line" | grep -q '"items":\[\]' && echo true || echo false)" \
	"a 4096x4096 IconPixmap is refused, not decoded ($line)"
check "$([ "${pngs:-0}" -eq 0 ] && echo true || echo false)" \
	"and nothing was written for it ($pngs files)"

echo "-- clicks"
out="$(run_case click)"
check "$(echo "$out" | grep -q '^Activate 137 19$' && echo true || echo false)" \
	"a left click arrives as Activate with the screen position intact"
check "$(echo "$out" | grep -q '^SecondaryActivate 137 19$' && echo true || echo false)" \
	"and a right click as SecondaryActivate"

echo "----"
echo "$PASS/$((PASS + FAIL)) assertions passed"
[ "$FAIL" -eq 0 ]
