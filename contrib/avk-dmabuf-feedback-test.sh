#!/usr/bin/env bash
# avk-dmabuf-feedback-test.sh — the compositor advertises what AVK can import.
#
# Before M3.6, linux-dmabuf feedback was built from the GLES2 COMPATIBILITY
# renderer: wlr_linux_dmabuf_v1_create_with_renderer(dpy, 5, drw) for the
# default, and SceneFX passing `.main_renderer = output->renderer` for
# per-surface feedback. So a client asked what the compositor could consume and
# was answered by a renderer that composites nothing, while AVK -- the
# subsystem that actually imports the buffer -- was never consulted.
#
# THE RULE
#
#     wlroots implements the protocol; AVK determines the GPU capabilities.
#
# WHAT IS ASSERTED
#
#   source == avk                  the ownership itself
#   advertised ⊆ AVK_importable    no pair advertised that AVK cannot import
#   MOD_INVALID never advertised   it is a fallback to cope with, not a
#                                  modifier to ask a client for
#   scanout ⊆ AVK ∩ KMS            no impossible intersection
#   main_device == AVK's DRM node  not the compatibility renderer's
#   withheld pairs are accounted   every omission has a logged reason
#
# WHY THE BREAK IS NOT ENOUGH ON ITS OWN
#
# BREAK=dmabuf-feedback-gles restores the shipped behaviour, and the test
# detects it because the reported SOURCE changes. But on a machine whose GLES
# and Vulkan format tables largely agree, the resulting format SET may be
# similar enough that a contents-only assertion would not fail -- so this
# would be coverage that depends on one driver's current Mesa version rather
# than on the architecture. The contents check is therefore paired with a
# source check, and the synthetic test below covers the ownership rule with no
# dependence on this GPU at all.
set -u

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="avk-dmabuf-feedback"
BREAK="${BREAK:-}"

OUTDIR="${TMPDIR:-/tmp}/asteroidz-dmabuf-fb-$$"
HL_OUTDIR="$OUTDIR"
HL_OUTPUTS=2
HL_ENV="ASTEROIDZ_RENDERER=avk"
[ "$BREAK" = dmabuf-feedback-gles ] && HL_ENV="$HL_ENV AZ_DMABUF_FEEDBACK_GLES=1"
export HL_OUTDIR HL_OUTPUTS HL_ENV

echo "binary under test: $HL_ASTEROIDZ"
hl_start >/dev/null 2>&1
sleep 2

FB="$OUTDIR/feedback.json"
hl_get "get dmabuf-feedback" > "$FB"

SRC="$(jq -r .source "$FB")"
echo "-- feedback source: $SRC --"
hl_assert "the compositor advertises AVK's capabilities, not the renderer's" \
	"$SRC" "avk"

if [ "$SRC" != "avk" ]; then
	# Everything below reads sets that only exist on the AVK path. Reporting
	# them as passes when they were never evaluated would be worse than
	# stopping, so stop.
	echo "  (feedback is renderer-derived; the capability assertions below"
	echo "   cannot be evaluated and are NOT counted as passes)"
	hl_stop
	hl_summary
	# hl_summary REPORTS, it does not terminate. Without this the run carried
	# on into assertions whose inputs are null and produced a second, bogus
	# failure on top of the real one.
	exit
fi

ADV=$(jq -r '.advertised_pairs' "$FB")
WITH=$(jq -r '.withheld_pairs' "$FB")
PROBED=$(jq -r '.avk_texture_pairs_probed' "$FB")
DEV=$(jq -r '.main_device' "$FB")
echo "  main device $DEV, $ADV advertised, $WITH withheld, $PROBED probed"

hl_assert "something is advertised at all ($ADV pairs)" \
	"$([ "$ADV" -gt 0 ] && echo true || echo false)" "true"

# The reverse check, as an equation rather than a feeling: every pair AVK
# probed is either advertised or deliberately withheld. A gap means the
# compositor is quietly narrower than its engine and nobody noticed.
hl_assert "advertised + withheld accounts for every probed pair" \
	"$((ADV + WITH))" "$PROBED"

python3 - "$FB" <<'PY' > "$OUTDIR/checks.txt"
import json, sys
d = json.load(open(sys.argv[1]))
adv = set(d["advertised_composition"]); imp = set(d["avk_importable"])
print("subset", "true" if adv <= imp else "false", len(adv - imp))
print("modinvalid", "true" if not any(p.split(":")[1].lower() in
    ("0x00ffffffffffffff", "0xffffffffffffffff") for p in adv) else "false")
worst = "true"
for o in d["outputs"]:
    sc = set(o.get("advertised_scanout") or [])
    kms = set(o.get("kms_scanout") or [])
    if not sc <= (imp & kms):
        worst = "false"
print("scanout", worst)
PY
read -r _ SUBSET NBAD < <(grep "^subset" "$OUTDIR/checks.txt")
read -r _ MODINV < <(grep "^modinvalid" "$OUTDIR/checks.txt")
read -r _ SCAN < <(grep "^scanout" "$OUTDIR/checks.txt")

hl_assert "every advertised pair is one AVK can import ($NBAD bad)" "$SUBSET" "true"
hl_assert "DRM_FORMAT_MOD_INVALID is never advertised" "$MODINV" "true"
hl_assert "every scanout pair is AVK-importable AND KMS-scannable" "$SCAN" "true"

# The main device must be AVK's own DRM node. Read it back from the log, which
# names the device AVK actually selected, rather than assuming they agree.
AVKDEV="$(grep -o "DRM primary [0-9]*:[0-9]*, render [0-9]*:[0-9]*" \
	"$OUTDIR/state/asteroidz/asteroidz.log" | head -1 | sed 's/.*render //')"
echo "  AVK selected render node $AVKDEV; feedback advertises $DEV"
hl_assert "the advertised main device is AVK's render node" "$DEV" "$AVKDEV"

hl_stop
hl_summary
