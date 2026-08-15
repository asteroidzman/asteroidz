#!/usr/bin/env python3
"""Measure whether a captured region carries a 1:1 one-pixel checkerboard.

The client half is contrib/x11check/x11check.c, which fills its window with
white/black pixels alternating on (x+y). This is the reader.

── WHY THESE THREE NUMBERS ──────────────────────────────────────────────

A magnified checkerboard is not "a slightly different checkerboard". Which of
the two symptoms shows up depends only on the sampler:

    grey     a bilinear tap between a black and a white texel is grey. Any
             grey pixel at all means the image was resampled, because the
             source contains exactly two colours and nothing between them.
    dup      a nearest tap over a magnified grid repeats a source texel, so
             two horizontally (or vertically) adjacent output pixels come out
             equal. At 1:1 that can never happen: every neighbour differs by
             construction.

So `grey == 0 and dup_h == 0 and dup_v == 0` is native presentation, and each
of the two upscaling paths trips a different one of them. Counting only greys
would call a NEAREST-filtered 1.25x upscale "native"; counting only duplicates
would call a bilinear one "native". Both mistakes are silent.

── WHY IT IS COUNTED THIS WAY ───────────────────────────────────────────

Run lengths are the natural way to say this and the wrong way to compute it:
an explicit run-length pass has to decide what to do with the runs the crop
cuts in half at the left and right edges, and getting that wrong shifts the
count by one per row in a way that looks like a real difference. "How many
horizontally adjacent pairs are equal" says exactly the same thing -- it is
zero if and only if every run is length 1 -- with no edge case at all.

Commands
    measure PNG X0 Y0 X1 Y1     grey=N dup_h=N dup_v=N px=N  (half-open x1/y1)
    verdict PNG X0 Y0 X1 Y1     `native` or `scaled`, plus the same counts
    colors  PNG X0 Y0 X1 Y1     the distinct colours in the region, most
                                common first -- for working out what a region
                                actually contains when a verdict surprises you
    count   PNG X0 Y0 X1 Y1 RRGGBB
                                how many pixels in the region are exactly that
                                colour

── WHY `count` EXISTS ───────────────────────────────────────────────────

`native` validates itself: a region with no greys AND no equal neighbours is
a checkerboard and can be nothing else. `scaled` does not -- ANY content that
is not a 1:1 checkerboard reports `scaled`, including a region that missed the
window altogether and sampled the desktop behind it. Worse, a bilinearly
magnified checkerboard's most common colour is mid-grey, which is exactly what
the harness's default wallpaper is.

So a fixture asserting `scaled` has to show its region contains client pixels
at all. Painting the wallpaper a colour the client never emits and asserting
zero of them inside the region is the cheapest way to say that.
"""

import sys

import numpy as np
from PIL import Image

# How far from pure black / pure white a pixel may sit and still count as one
# of the two. Not zero: the capture goes through the compositor's own encode,
# and a dither amplitude or a rounding step of a code value or two is not
# resampling. A genuine bilinear tap between black and white lands near 128,
# which is nowhere near this.
TOL = 8


def region(path, x0, y0, x1, y1):
    img = Image.open(path).convert("RGB")
    a = np.asarray(img, dtype=np.int16)
    if y1 > a.shape[0] or x1 > a.shape[1] or x0 < 0 or y0 < 0 or x1 <= x0 or y1 <= y0:
        sys.exit(
            f"checker: region {x0},{y0}..{x1},{y1} is not inside "
            f"{a.shape[1]}x{a.shape[0]} ({path})"
        )
    return a[y0:y1, x0:x1, :]


def measure(a):
    # One channel is enough: the pattern is achromatic, and using the mean of
    # the three would turn a single off-by-one channel into a fractional value
    # that is neither black nor white.
    v = a[:, :, 0]
    grey = int(np.count_nonzero((v > TOL) & (v < 255 - TOL)))
    # Quantise before comparing neighbours, so a black pixel at code 1 and one
    # at code 0 still count as the same colour. Greys are already counted.
    q = (v >= 128).astype(np.int8)
    dup_h = int(np.count_nonzero(q[:, 1:] == q[:, :-1]))
    dup_v = int(np.count_nonzero(q[1:, :] == q[:-1, :]))
    return grey, dup_h, dup_v, int(v.size)


def main():
    if len(sys.argv) < 7:
        sys.exit(__doc__)
    cmd, path = sys.argv[1], sys.argv[2]
    x0, y0, x1, y1 = (int(v) for v in sys.argv[3:7])
    a = region(path, x0, y0, x1, y1)

    if cmd == "count":
        if len(sys.argv) < 8:
            sys.exit("checker: count needs a RRGGBB colour")
        hexv = sys.argv[7].lstrip("#")
        want = np.array(
            [int(hexv[0:2], 16), int(hexv[2:4], 16), int(hexv[4:6], 16)],
            dtype=np.int16,
        )
        print(int(np.count_nonzero((a == want).all(axis=2))))
        return

    if cmd == "colors":
        flat = a.reshape(-1, 3)
        vals, counts = np.unique(flat, axis=0, return_counts=True)
        order = np.argsort(-counts)
        for i in order[:12]:
            r, g, b = vals[i]
            print(f"{counts[i]:9d}  #{r:02x}{g:02x}{b:02x}")
        return

    grey, dup_h, dup_v, px = measure(a)
    line = f"grey={grey} dup_h={dup_h} dup_v={dup_v} px={px}"
    if cmd == "measure":
        print(line)
    elif cmd == "verdict":
        native = grey == 0 and dup_h == 0 and dup_v == 0
        print(f"{'native' if native else 'scaled'} {line}")
    else:
        sys.exit(f"checker: unknown command {cmd}")


if __name__ == "__main__":
    main()
