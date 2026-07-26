#!/usr/bin/env python3
"""Normalise the bar's SVG art to one canvas, so every icon reads at the same
visual size.

The artwork comes from several waybar plugin repos that never had to agree
with each other: viewBoxes of 24x24 and 100x100, ink filling the canvas edge
to edge in one file and floating in the middle of it in another. The bar draws
each icon into an identical square box, so those differences show up directly
as icons that look bigger or smaller than their neighbours in the same pill
row.

This rewrites each file to a 24x24 viewBox with its ink scaled and centred
into a fixed content square, preserving aspect ratio. The original drawing is
untouched -- it is wrapped in one transform group -- so re-running this is
idempotent in appearance and the upstream art stays recognisable.

Ink bounds are measured by rasterising, not by parsing paths: these files use
strokes, masks and gradients, and only the renderer knows what those actually
cover.

Usage: contrib/normalize-bar-icons.py assets/bar-icons [--check]
"""
import os
import re
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

SVG_NS = "http://www.w3.org/2000/svg"
CANVAS = 24.0
# Ink FILLS the canvas. It was inset to 20-of-24 to keep a round icon from
# looking heavier than a square one, and that inset turned out to be a layout
# bug in disguise: the bar spaces icon BOXES evenly, so 2px of transparency
# per side became 4px of extra apparent gap between every pair of icon-only
# pills, while text pills sat at the configured distance. The same inset made
# vendored icons render smaller than tray icons (which are cropped to their
# ink), so the two never lined up vertically either.
#
# Filling the canvas makes the drawn ink the icon's real extent, which is what
# both the spacing and the vertical centring already assume.
CONTENT = 24.0
RASTER = 256


def ink_bbox_png(path):
    from PIL import Image

    im = Image.open(path).convert("RGBA")
    bbox = im.getbbox()  # alpha-aware
    return bbox, im.size


def viewbox_of(root):
    vb = root.get("viewBox")
    if vb:
        parts = [float(v) for v in re.split(r"[ ,]+", vb.strip())]
        if len(parts) == 4:
            return parts
    w = root.get("width")
    h = root.get("height")
    if w and h:
        return [0.0, 0.0, float(re.sub(r"[^0-9.]", "", w)),
                float(re.sub(r"[^0-9.]", "", h))]
    return [0.0, 0.0, CANVAS, CANVAS]


def normalize(path, check=False):
    ET.register_namespace("", SVG_NS)
    tree = ET.parse(path)
    root = tree.getroot()
    vx, vy, vw, vh = viewbox_of(root)
    if vw <= 0 or vh <= 0:
        return "skip (no viewBox)"

    with tempfile.NamedTemporaryFile(suffix=".png", delete=False) as tmp:
        png = tmp.name
    try:
        subprocess.run(
            ["rsvg-convert", "-w", str(RASTER), "-h", str(RASTER), path,
             "-o", png],
            check=True, capture_output=True)
        bbox, (pw, ph) = ink_bbox_png(png)
    finally:
        os.unlink(png)
    if not bbox:
        return "skip (no ink)"

    # raster pixels -> user units of the ORIGINAL viewBox
    x0 = vx + bbox[0] / pw * vw
    y0 = vy + bbox[1] / ph * vh
    x1 = vx + bbox[2] / pw * vw
    y1 = vy + bbox[3] / ph * vh
    iw, ih = x1 - x0, y1 - y0
    if iw <= 0 or ih <= 0:
        return "skip (degenerate ink)"

    s = min(CONTENT / iw, CONTENT / ih)
    tx = CANVAS / 2.0 - s * (x0 + iw / 2.0)
    ty = CANVAS / 2.0 - s * (y0 + ih / 2.0)

    if check:
        # already normalised? ink centred and filling CONTENT on its long axis
        long_axis = max(iw, ih)
        centred = (abs((x0 + iw / 2) - CANVAS / 2) < 0.6 and
                   abs((y0 + ih / 2) - CANVAS / 2) < 0.6)
        sized = abs(long_axis - CONTENT) < 0.6 and abs(vw - CANVAS) < 0.01
        return "ok" if (centred and sized) else "NEEDS NORMALISING"

    children = list(root)
    for c in children:
        root.remove(c)
    g = ET.SubElement(root, "{%s}g" % SVG_NS)
    g.set("transform", "translate(%.4f %.4f) scale(%.6f)" % (tx, ty, s))
    for c in children:
        g.append(c)

    root.set("width", "24")
    root.set("height", "24")
    root.set("viewBox", "0 0 24 24")
    tree.write(path, encoding="UTF-8", xml_declaration=True)
    return "normalised (scale %.3f)" % s


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("-")]
    check = "--check" in sys.argv
    rootdir = args[0] if args else "assets/bar-icons"
    bad = 0
    for dirpath, _, files in os.walk(rootdir):
        for f in sorted(files):
            if not f.endswith(".svg"):
                continue
            p = os.path.join(dirpath, f)
            r = normalize(p, check)
            if "NEEDS" in r:
                bad += 1
            print("%-56s %s" % (os.path.relpath(p, rootdir), r))
    if check and bad:
        print("\n%d icon(s) not normalised" % bad)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
