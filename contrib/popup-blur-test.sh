#!/usr/bin/env bash
# popup-blur-test.sh — does a POPUP get background blur when it asks for one?
#
# ext-background-effect-v1 was wired to layer surfaces and toplevels only. A
# popup is neither: toplevel_from_wlr_surface walks up from one to whatever it
# hangs off, so a menu's blur region was looked for on its parent's surface,
# found missing, and silently did nothing. asteroidz-bar's popovers are exactly
# that case -- they are xdg popups off the bar's layer surface, and the frost
# stopped at the bar.
#
# Two popups, identical but for the region, over a striped wallpaper: the one
# that asked must come out flat, the one that did not must still show stripes.
# A control matters here because a translucent dark panel over stripes looks
# blurred at a glance whether or not anything blurred it.
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${HL_OUTDIR:-/tmp/asteroidz-popblur-$$}"
mkdir -p "$OUT"

command -v quickshell >/dev/null 2>&1 || {
	echo "popup-blur-test: needs quickshell (the client half of the test)" >&2
	exit 77
}

# shellcheck disable=SC1091
. "$HERE/contrib/lib/headless.sh"

cat > "$OUT/shell.qml" <<'QML'
import Quickshell
import Quickshell.Wayland
import QtQuick

ShellRoot {
    PanelWindow {
        anchors { top: true; left: true; right: true }
        implicitHeight: 60
        color: "transparent"
        WlrLayershell.layer: WlrLayer.Top

        Rectangle {
            id: pill
            x: 300; y: 10; width: 120; height: 40
            radius: 9; color: "#d90a0a0c"
        }

        PopupWindow {
            anchor.item: pill
            anchor.edges: Edges.Bottom
            anchor.gravity: Edges.Bottom
            implicitWidth: 240; implicitHeight: 160
            visible: true
            color: "transparent"
            Rectangle {
                id: blurred
                anchors.fill: parent; anchors.margins: 20
                radius: 9; color: "#a60a0a0c"
            }
            BackgroundEffect.blurRegion: Region { item: blurred; radius: 9 }
        }

        PopupWindow {
            anchor.item: pill
            anchor.edges: Edges.Bottom
            anchor.gravity: Edges.Bottom
            anchor.margins.left: 400
            implicitWidth: 240; implicitHeight: 160
            visible: true
            color: "transparent"
            Rectangle {
                anchors.fill: parent; anchors.margins: 20
                radius: 9; color: "#a60a0a0c"
            }
        }
    }
}
QML

hl_start
trap 'hl_stop' EXIT
kill "$HL_SWAYBG_PID" 2>/dev/null

# 20px black/white bands. Blur is measured as the loss of that contrast, so
# the pattern has to be sharper than anything the panel colour does on its own.
magick -size 40x40 xc:white -fill black -draw "rectangle 0,0 40,19" -write mpr:t +delete \
	-size "${HL_WIDTH}x${HL_HEIGHT}" tile:mpr:t "$OUT/wall.png"
swaybg -o '*' -i "$OUT/wall.png" -m tile > /dev/null 2>&1 &
BG=$!
sleep 1

env XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" WAYLAND_DISPLAY="$WAYLAND_DISPLAY" \
	HOME="$HOME" PATH="$PATH" QT_QPA_PLATFORM=wayland QT_FONT_DPI=96 \
	quickshell -p "$OUT/shell.qml" > "$OUT/qs.log" 2>&1 &
QS=$!
sleep 6
grim -o "$HL_MON" "$OUT/popups.png" 2>/dev/null
kill "$QS" "$BG" 2>/dev/null

python3 - "$OUT/popups.png" <<'PY'
import sys
from PIL import Image

im = Image.open(sys.argv[1]).convert("RGB")
px = im.load()
W, H = im.size


def panels(row):
    """The two popup panels, found rather than assumed: on a row where the
    wallpaper stripe is white, any column a panel covers is darker."""
    out, s = [], None
    for x in range(W):
        covered = sum(px[x, row]) / 3 < 200
        if covered and s is None:
            s = x
        elif not covered and s is not None:
            if x - s > 60:
                out.append((s, x - 1))
            s = None
    return out


def contrast(x0, x1, y0, y1):
    """Peak-to-trough down each column: high means the stripes survived."""
    worst = 0
    for x in range(x0, x1):
        col = [sum(px[x, y]) for y in range(y0, y1)]
        worst = max(worst, max(col) - min(col))
    return worst


# y=100 is inside both panels and on a white band of the wallpaper.
found = panels(100)
if len(found) < 2:
    print(f"FAIL both popups are on screen (found {len(found)})")
    raise SystemExit(1)

(ba, bb), (ca, cb) = found[0], found[1]
blurred = contrast(ba + 12, bb - 12, 95, 175)
control = contrast(ca + 12, cb - 12, 95, 175)
print(f"asked-for-blur {ba}..{bb}: stripe contrast {blurred}")
print(f"control        {ca}..{cb}: stripe contrast {control}")

# Every FAIL below must reach the exit status. This script used to print them
# and exit 0 -- the same defect avk-suite.sh's third check was written for,
# arriving by a different route: a python heredoc that falls off its end.
bad = 0

if blurred < control / 2:
    print("PASS a popup that asks for blur gets it")
else:
    print("FAIL a popup that asks for blur gets it")
    bad += 1

if control > 150:
    print("PASS a popup that does not ask keeps its background sharp")
else:
    print("FAIL a popup that does not ask keeps its background sharp")
    bad += 1

raise SystemExit(1 if bad else 0)
PY
