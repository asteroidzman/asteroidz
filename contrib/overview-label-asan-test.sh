#!/usr/bin/env bash
# overview-label-asan-test.sh — the overview tag labels, under ASAN.
#
# asteroidz_jump_label_node_set_focus() re-renders by calling
# ..._node_update(node, node->cached_text, ...) -- passing the cache AS the
# text argument. update() then replaces the cache:
#
#     char *new_cached_text = g_strdup(text);
#     g_free(node->cached_text);      <- frees the buffer `text` aliases
#     node->cached_text = new_cached_text;
#
# An earlier fix ordered the g_strdup before the g_free so the COPY was safe,
# and stopped there. Every later read of `text` was still dangling, including
# the pango_layout_set_text() that draws the label -- so the pill rendered
# whatever the allocator had since put in that block: tofu and stray letters.
#
# Why a screenshot test is not enough, and this exists instead: a freed block
# usually keeps its contents until something reuses it, so a quiet headless
# instance with two windows renders the label CORRECTLY on the broken build.
# It reproduced on a real desktop and not here, twice. ASAN does not care
# whether the contents happened to survive:
#
#     ERROR: AddressSanitizer: heap-use-after-free   READ of size 3
#       #1 pango_layout_set_text
#       #2 get_text_pixel_size               text-node.c:746
#       #3 asteroidz_jump_label_node_update  text-node.c:886
#       #4 ..._set_focus                     text-node.c:1074
#       #5 overview_draw_cell_label          overview.h:624
#
# 2 reports before the fix, 0 after.
#
# Needs an ASAN build: meson setup build-asan -Dasan=true && ninja -C build-asan
# Exit: 0 clean, 1 sanitizer reports found, 2 no ASAN build
# The overview label UAF needs a cell whose FOCUS STATE CHANGES between opens.
# A first open leaves cached_focused matching (calloc'd false, cell not
# current) so update() returns early and never takes the aliasing path -- which
# is why a single open looked fine. Switch the current tag and open again and
# the previously-current cell flips to non-current, which is the path that
# freed the buffer it was about to render from.
set -u
REPO="${ASTEROIDZ_REPO:-$HOME/asteroidz}"
export HL_RENDERER=gles2
export ASTEROIDZ="$REPO/build-asan/asteroidz"
if [ ! -x "$REPO/build-asan/asteroidz" ]; then
	echo "  --   no ASAN build (meson setup build-asan -Dasan=true); INCONCLUSIVE"
	exit 2
fi
export HL_ENV="ASAN_OPTIONS=detect_leaks=0:abort_on_error=0:log_path=stderr" HL_WIDTH=1920 HL_HEIGHT=1080
export HL_OUTDIR="/tmp/asteroidz-ov3-$$"
. "$REPO/contrib/lib/headless.sh"
hl_start
trap 'hl_stop' EXIT
WORK="$HL_OUTDIR"
cat >> "$HL_CONFIG" <<'EOF'
source "/home/ralf/.config/asteroidz/colors.kdl"
effects { blend-space "srgb" }
EOF
hl_dispatch "reload_config" 2

kitty --title OVA -o background_opacity=1.0 -o background='#203040' \
  sh -c 'echo OVA; exec sleep 600' > "$WORK/a.log" 2>&1 &
sleep 4
hl_dispatch "view,6" 1
kitty --title OVB -o background_opacity=1.0 -o background='#403020' \
  sh -c 'echo OVB; exec sleep 600' > "$WORK/b.log" 2>&1 &
sleep 4

hl_dispatch "toggle_overview" 2     # first open: caches focus state
hl_dispatch "toggle_overview" 1     # close
hl_dispatch "view,1" 1              # current tag moves -> focus states flip
hl_dispatch "toggle_overview" 3     # second open: the aliasing path
# The label pixels are deliberately NOT asserted -- see the header. What is
# asserted is that the sanitizer stayed quiet.
LOG="$WORK/state/asteroidz/asteroidz.log"
N="$(grep -c "ERROR: AddressSanitizer" "$LOG" 2>/dev/null)"
echo "AddressSanitizer reports: ${N:-0}"
if [ "${N:-0}" -gt 0 ]; then
	grep -m1 -A10 "ERROR: AddressSanitizer" "$LOG" | sed 's/^/  /'
	exit 1
fi
echo "  ok   no sanitizer reports across an overview refocus"
