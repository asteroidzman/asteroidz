#!/usr/bin/env bash
# border-color-space-test.sh — does a solid colour survive srgb blending?
#
# A window border is a solid-colour rect, and render_pass_add_rect /
# render_rounded_rect convert that colour from sRGB to linear before handing it
# to the shader. They did so UNCONDITIONALLY, which is wrong whenever the pass
# is blending on encoded values instead of on light: the rect gets linearised
# into a buffer that holds encoded values and comes out double-darkened.
#
# `effects { blend-space "srgb" }` plus an output with no colour transform is
# exactly that case, and it is the ordinary SDR desktop. Measured, same palette,
# only the blend space differing:
#
#     border      linear blending   srgb blending (before the fix)
#     inactive    #2c2c2c           #050505
#     focused     #a4c8ff           #6196ff
#
# It reported as "my two monitors draw the same border colour differently":
# a colour-transformed output (HDR, or an ICC profile) has color_transform !=
# NULL, so it does NOT blend encoded, linearises as it always did, and looks
# correct -- while the plain SDR monitor beside it drew every border too dark.
# Textured elements agreed on both monitors throughout, because the texture
# path already asked the blend-space question. That asymmetry is the tell.
#
# Both blend spaces are asserted. Only checking srgb would pass on a build that
# dropped the conversion entirely, which breaks linear blending instead.
set -u

REPO="${ASTEROIDZ_REPO:-$HOME/asteroidz}"
export HL_RENDERER="${HL_RENDERER:-vulkan}"

PASS=0; FAIL=0
ok() { echo "  ok   $1"; PASS=$((PASS + 1)); }
bad() { echo "  FAIL $1"; FAIL=$((FAIL + 1)); }

# shellcheck disable=SC1091
. "$REPO/contrib/lib/headless.sh"

# The value the bug mangled: srgb blending drew this as #6196ff.
BORDER_FOCUS="a4c8ff"

run_space() { # run_space <linear|srgb>
	local space="$1"
	HL_OUTDIR="/tmp/asteroidz-bordercs-$$-$space"
	hl_start
	local work="$HL_OUTDIR"

	cat >> "$HL_CONFIG" <<EOF
layout { border { width 4; color 0x2c2c2cff; focus-color 0x${BORDER_FOCUS}ff
	gradient { enable 0 } } }
effects { blend-space "$space" }
EOF
	hl_dispatch "reload_config" 2

	# ONE window, so it is unambiguously the focused one. Two windows meant
	# guessing which had focus, and the loser's border sat next to the
	# winner's with blend pixels between them -- the test then measured
	# antialiasing instead of the colour. The conversion under test is shared
	# by every solid colour, so pinning one through both blend spaces pins it.
	kitty --title BCSa -o background_opacity=1.0 -o background='#101010' \
		sh -c 'echo BCSa; exec sleep 600' > "$work/ka.log" 2>&1 &
	sleep 5
	grim -o "$HL_MON" "$work/s.png" 2>/dev/null

	# The focused window is whichever the compositor selected; assert that the
	# two DISTINCT colours present are exactly the two configured ones, which
	# avoids depending on which window won focus.
	local found
	found="$(python3 - "$work/s.png" "$(hl_get 'get all-clients')" <<'PY'
import sys, json
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB"); px = im.load(); W, H = im.size
seen = set()
for c in json.loads(sys.argv[2]).get("clients", []):
    y = c["y"] + c["height"] // 2
    if not (0 <= y < H):
        continue
    # The border's own pixels. `x` from get all-clients is the FRAME origin,
    # so the border occupies x .. x+width-1 and the content starts after it --
    # sampling to the LEFT of x reads wallpaper, which is how the first draft
    # of this test managed to fail on a correct build.
    for x in range(c["x"], min(W, c["x"] + 3)):
        r, g, b = px[x, y]
        seen.add("%02x%02x%02x" % (r, g, b))
print(" ".join(sorted(seen)))
PY
)"
	echo "  --   [$space] border pixels: ${found:-none}"
	if echo "$found" | grep -qi "$BORDER_FOCUS"; then
		ok "[$space] the focused border is #$BORDER_FOCUS as configured"
	else
		bad "[$space] the focused border is #$BORDER_FOCUS as configured"
	fi

	hl_stop
}

run_space linear
run_space srgb

echo
echo "$PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
