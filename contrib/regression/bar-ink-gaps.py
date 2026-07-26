#!/usr/bin/env python3
"""Measure the gaps between what you can SEE in a bar screenshot.

The bar spaces pills by their boxes, but a box is only as honest as the
content inside it: transparent margin in an icon, or a pinned width reserving
room for a label that is not being shown, both render as spacing that no
config value asked for. This walks a horizontal band of the screenshot,
finds the runs of columns containing anything other than the panel
background, and reports the gaps between consecutive runs -- which is the
separation a person actually perceives.

Usage: bar-ink-gaps.py PNG Y0 Y1 X0 X1
Prints one line: "gaps: 12 12 12 13 12  min=12 max=13 spread=1"
Exits non-zero if the band contains fewer than two runs (nothing to measure).
"""
import sys
from collections import Counter

try:
    from PIL import Image
except ImportError:
    print("skip: python3 PIL not available")
    sys.exit(77)

png, y0, y1, x0, x1 = (sys.argv[1], int(sys.argv[2]), int(sys.argv[3]),
                       int(sys.argv[4]), int(sys.argv[5]))
im = Image.open(png).convert("RGB")
W, H = im.size
px = im.load()
x1, y1 = min(x1, W), min(y1, H)

# Background is every colour covering a large share of the band -- normally
# two of them, the panel and the wallpaper it is drawn on, because a rounded
# panel corner clipping into the band shows both. Counting only the panel
# would make that corner read as artwork and report the panel's own padding as
# a gap between modules.
counts = Counter(px[x, y] for x in range(x0, x1) for y in range(y0, y1))
total = sum(counts.values())
bgs = [c for c, n in counts.most_common(4) if n >= total * 0.05] or \
      [counts.most_common(1)[0][0]]

def ink(p):
    return all(abs(p[0] - b[0]) + abs(p[1] - b[1]) + abs(p[2] - b[2]) > 40
               for b in bgs)

runs, start = [], None
for x in range(x0, x1):
    hit = any(ink(px[x, y]) for y in range(y0, y1))
    if hit and start is None:
        start = x
    elif not hit and start is not None:
        runs.append((start, x - 1))
        start = None
if start is not None:
    runs.append((start, x1 - 1))

# Antialiasing can drop a column inside one glyph; anything closer than 4px is
# the same piece of artwork, not two modules.
merged = []
for r in runs:
    if merged and r[0] - merged[-1][1] <= 4:
        merged[-1] = (merged[-1][0], r[1])
    else:
        merged.append(list(r))

# A rounded panel corner clipping into the band is a one-pixel sliver, not a
# module; measuring to it would report the panel padding as a gap.
merged = [r for r in merged if r[1] - r[0] + 1 >= 3]

if len(merged) < 2:
    print("error: %d run(s) in the band -- nothing to measure" % len(merged))
    sys.exit(1)

gaps = [merged[i + 1][0] - merged[i][1] - 1 for i in range(len(merged) - 1)]
print("gaps: %s  min=%d max=%d spread=%d" %
      (" ".join(str(g) for g in gaps), min(gaps), max(gaps),
       max(gaps) - min(gaps)))
