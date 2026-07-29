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


def ink_box(path, png):
    """Rasterise and return the bounding box of everything non-transparent."""
    subprocess.run(["rsvg-convert", "-h", "64", path, "-o", png], check=True,
                   capture_output=True)
    from PIL import Image
    return Image.open(png).convert("RGBA").getbbox()


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

    checked = "parse + ink" if can_render else "parse only (no rsvg-convert/Pillow)"
    print("%d/%d bar icons OK (%s)" % (len(files) - failures, len(files), checked))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
