#!/usr/bin/env python3
"""Every vendored bar icon parses, and draws something.

This exists because of a real failure. The bar's artwork was moved out of the
waybar plugin repos and into assets/bar-icons so the bar would stop depending
on which plugins happened to be installed. The ship logo and the four layout
icons were missed, and nothing noticed: the plugin packages were deleted, the
files survived only as untracked orphans under ~/.local/share, and the bar drew
empty pills where the logo and the layout indicator should have been.

Then, vendoring them, a comment containing a double hyphen went into one of the
SVGs. That is not legal XML. librsvg rejected the entire document, so the icon
did not render *wrong*, it did not render at all -- the same symptom as the
missing file, from a completely different cause.

Both failures are silent at the only place they matter (a pill is simply
blank), and both are caught here in a few milliseconds:

  * XML well-formedness, which is what the double hyphen broke
  * a non-empty ink bounding box, which is what a missing or blank file breaks,
    checked by actually rasterising when a renderer is available

A third check, from a third failure: a pill that toggles between two icons must
not change size when it does. The idle cup's crossed variant carried its slash
corner to corner, so it drew 92 pixels of ink to the plain cup's 89 and the pill
visibly grew on click. Nothing about that is a rendering fault -- both files are
valid, both draw something, both declare the same 24x24 box -- it is only wrong
relative to the file it is swapped with, which is why it needs a check that
knows the two are a pair.

The ink check needs rsvg-convert and Pillow. They are test-time conveniences,
not build dependencies, so their absence skips that half rather than failing.
"""
import importlib.util
import os
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

ROOT = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "assets", "bar-icons")


def svgs():
    for dirpath, _, names in os.walk(ROOT):
        for n in sorted(names):
            if n.endswith(".svg"):
                yield os.path.join(dirpath, n)


def have(cmd):
    return subprocess.run(["sh", "-c", "command -v " + cmd],
                          capture_output=True).returncode == 0


def ink_box(path, png, height=64):
    """Rasterise and return the bounding box of everything non-transparent."""
    subprocess.run(["rsvg-convert", "-h", str(height), path, "-o", png],
                   check=True, capture_output=True)
    from PIL import Image
    return Image.open(png).convert("RGBA").getbbox()


# Icons one pill swaps between, so a click must not resize it. Only pairs a
# module actually toggles: two icons that merely sit near each other have no
# reason to match, and demanding it would be a rule nobody could satisfy while
# drawing anything interesting.
TOGGLE_PAIRS = [
    ("asteroidz-bar/idle-on.svg", "asteroidz-bar/idle-off.svg"),
]

# Rendered LARGE for this comparison, not at the 64px the checks above use.
#
# The mismatch that prompted this was 3 pixels of 96 -- which at 64 is 2, the
# same as the antialiasing wobble at the ends of a diagonal, so a tolerance
# loose enough to be safe there is loose enough to accept the bug. At 256 the
# same mismatch is 8 pixels and the wobble is still about 2, and the two stop
# overlapping. The tolerance has to be a fraction of the size, not a constant.
PAIR_HEIGHT = 256
INK_TOLERANCE = 4


def check_toggle_pairs(tmp):
    failures = 0
    for a, b in TOGGLE_PAIRS:
        pa, pb = os.path.join(ROOT, a), os.path.join(ROOT, b)
        if not (os.path.exists(pa) and os.path.exists(pb)):
            print("FAIL %s / %s: toggle pair is missing a half" % (a, b))
            failures += 1
            continue
        box_a = ink_box(pa, os.path.join(tmp, "pair_a.png"), PAIR_HEIGHT)
        box_b = ink_box(pb, os.path.join(tmp, "pair_b.png"), PAIR_HEIGHT)
        if box_a is None or box_b is None:
            continue  # the empty-render check above already reported it
        wa, wb = box_a[2] - box_a[0], box_b[2] - box_b[0]
        if abs(wa - wb) > INK_TOLERANCE:
            print("FAIL %s (%dpx of ink) and %s (%dpx) are swapped in one "
                  "pill and must be the same width" % (a, wa, b, wb))
            failures += 1
    return failures


def main():
    files = list(svgs())
    if not files:
        print("FAIL: no SVGs found under", ROOT)
        return 1

    can_render = (have("rsvg-convert")
                  and importlib.util.find_spec("PIL") is not None)

    failures = 0
    tmp = tempfile.mkdtemp(prefix="bar-icons-")
    for path in files:
        rel = os.path.relpath(path, ROOT)

        try:
            ET.parse(path)
        except ET.ParseError as e:
            # The double-hyphen case lands here.
            print("FAIL %s: not well-formed XML: %s" % (rel, e))
            failures += 1
            continue

        if not can_render:
            continue

        png = os.path.join(tmp, rel.replace(os.sep, "_") + ".png")
        try:
            box = ink_box(path, png)
        except subprocess.CalledProcessError as e:
            print("FAIL %s: renderer rejected it: %s"
                  % (rel, e.stderr.decode(errors="replace").strip()))
            failures += 1
            continue

        if box is None:
            print("FAIL %s: renders completely empty" % rel)
            failures += 1

    if can_render:
        failures += check_toggle_pairs(tmp)

    checked = "parse + ink" if can_render else "parse only (no rsvg-convert/Pillow)"
    print("%d/%d bar icons OK (%s)" % (len(files) - failures, len(files), checked))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
