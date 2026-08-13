#!/usr/bin/env python3
"""Binary-PPM tools for the deterministic transform oracle.

`amsg dispatch capture_output` writes the Vulkan attachment itself, in the
ATTACHMENT's own orientation and extent. Comparing two rotations therefore means
mapping each capture back to a canonical orientation first -- comparing raw
rotated buffers is comparing different pictures.

PPM rather than PNG on purpose. A three-line ASCII header and raw RGB needs no
decoder, no zlib and no filter reconstruction; the harness has already had one
hand-written PNG reader produce an IndexError that read exactly like "the two
captures are different sizes".

Commands
    info    FILE                          w h
    diff    A B                           differing_pixels worst_channel_error
    bbox    A B                           x0 y0 x1 y1   (half-open x1/y1)
    px      FILE X Y                      r g b
    edgecol FILE Y                        the column of the first colour
                                          transition in row Y, or -1
    edgespan FILE                         rows first_y last_y col_min col_max
                                          over every row that has one
    canon   IN OUT TRANSFORM              map an attachment into presentation
                                          orientation
    verify  IN REF TRANSFORM              canonicalise IN through all eight
                                          candidate transforms and print the
                                          differing-pixel count of each against
                                          REF; the expected one must be the
                                          unique zero
    marks   FILE                          the four corner marker colours
    report  A B [DIFF_IMAGE]              extents, hashes, wrong-pixel count,
                                          first mismatch, bbox; optionally
                                          writes a difference image
    grid    A B [N]                       an NxN coarse map of where they
                                          differ: '.' none, digits = log10 of
                                          the count, '#' = every pixel

TRANSFORM is the wl_output_transform the output was rendered with: 0-7, for
normal / 90 / 180 / 270 / flipped / flipped-90 / flipped-180 / flipped-270.
"""

import sys


def read(path):
    with open(path, 'rb') as f:
        data = f.read()
    if data[:2] != b'P6':
        raise ValueError(path + ' is not a binary PPM')
    # Header: P6 <ws> W <ws> H <ws> MAX <single ws> then raw bytes. Comments are
    # legal in a PPM header and the writer never emits one, but a reader that
    # ignores them is a reader that silently misparses somebody else's file.
    fields, i = [], 2
    while len(fields) < 3:
        while i < len(data) and data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b'#':
            while i < len(data) and data[i] != 0x0A:
                i += 1
            continue
        j = i
        while j < len(data) and not data[j:j + 1].isspace():
            j += 1
        fields.append(int(data[i:j]))
        i = j
    i += 1
    w, h, maxval = fields
    if maxval != 255:
        raise ValueError('only 8-bit PPM is supported')
    px = data[i:i + w * h * 3]
    if len(px) != w * h * 3:
        raise ValueError('%s is truncated: %d bytes for %dx%d'
                         % (path, len(px), w, h))
    return w, h, px


def write(path, w, h, px):
    with open(path, 'wb') as f:
        f.write(b'P6\n%d %d\n255\n' % (w, h))
        f.write(bytes(px))


def transform_point(x, y, t, w, h):
    """wlr_box_transform() for a 1x1 box, written out from the definition.

    An INDEPENDENT implementation, deliberately: the oracle must not share a
    transform helper with the renderer, or one wrong transform can make both
    agree. This is the eight-case switch from wlroots' util/box.c specialised to
    a unit box, and the direction it maps is additionally checked by data --
    `verify` below canonicalises against all eight candidates and requires the
    expected one to be the unique winner.

    (w, h) is the SOURCE space. For an odd transform the destination space is
    (h, w).
    """
    if t == 0:                      # normal
        return x, y
    if t == 1:                      # 90
        return h - y - 1, x
    if t == 2:                      # 180
        return w - x - 1, h - y - 1
    if t == 3:                      # 270
        return y, w - x - 1
    if t == 4:                      # flipped
        return w - x - 1, y
    if t == 5:                      # flipped-90
        return y, x
    if t == 6:                      # flipped-180
        return x, h - y - 1
    if t == 7:                      # flipped-270
        return h - y - 1, w - x - 1
    raise ValueError('transform %r is not a wl_output_transform' % (t,))


def invert(t):
    """wlr_output_transform_invert(): the flipped transforms are involutions,
    90 and 270 swap. Written out rather than imported, for the same reason."""
    if (t & 1) and not (t & 4):
        return t ^ 2
    return t


def canonicalise(w, h, px, transform):
    """An ATTACHMENT mapped back into PRESENTATION orientation.

    The renderer maps a presentation-orientation raster into the attachment with
    wlr_box_transform(invert(T)) over the presentation extent. This walks the
    presentation raster and gathers from the attachment through exactly that
    map, so the result of every transform is directly comparable with the
    transform-0 result of the same logical desktop.

    Half-open throughout: a pixel at presentation (x, y) of a WxH raster is the
    sample at index (x, y), and the far edge is W, never W-1 plus a fudge.
    """
    transform = int(transform)
    if transform == 0:
        return w, h, px
    # The presentation extent: odd transforms transpose the attachment.
    ow, oh = (h, w) if (transform & 1) else (w, h)
    inv = invert(transform)
    out = bytearray(ow * oh * 3)
    for y in range(oh):
        d = y * ow * 3
        for x in range(ow):
            sx, sy = transform_point(x, y, inv, ow, oh)
            s = (sy * w + sx) * 3
            out[d:d + 3] = px[s:s + 3]
            d += 3
    return ow, oh, bytes(out)


def fnv(data):
    """FNV-1a over the pixel bytes. Not a checksum with any security
    property -- an identity for a capture, so that two runs of the same fixture
    can be stated to have produced the same image in one field."""
    h = 0x811c9dc5
    for b in data:
        h = ((h ^ b) * 0x01000193) & 0xffffffff
    return h


def _pair(a, b):
    wa, ha, pa = read(a)
    wb, hb, pb = read(b)
    if (wa, ha) != (wb, hb):
        return None
    return wa, ha, pa, pb


def main():
    mode = sys.argv[1]
    if mode == 'info':
        w, h, _ = read(sys.argv[2])
        print(w, h)
    elif mode == 'diff':
        got = _pair(sys.argv[2], sys.argv[3])
        if got is None:
            print(-1, -1)
            return
        w, h, pa, pb = got
        differ = worst = 0
        for i in range(0, len(pa), 3):
            if pa[i:i + 3] != pb[i:i + 3]:
                differ += 1
                for k in range(3):
                    d = abs(pa[i + k] - pb[i + k])
                    if d > worst:
                        worst = d
        print(differ, worst)
    elif mode == 'bbox':
        got = _pair(sys.argv[2], sys.argv[3])
        if got is None:
            print(-1, -1, -1, -1)
            return
        w, h, pa, pb = got
        x0 = y0 = 1 << 30
        x1 = y1 = -1
        for y in range(h):
            row = y * w * 3
            for x in range(w):
                i = row + x * 3
                if pa[i:i + 3] != pb[i:i + 3]:
                    x0 = min(x0, x); y0 = min(y0, y)
                    x1 = max(x1, x + 1); y1 = max(y1, y + 1)
        print(x0, y0, x1, y1)
    elif mode == 'px':
        w, h, px = read(sys.argv[2])
        x, y = int(sys.argv[3]), int(sys.argv[4])
        if x < 0:
            x += w
        if y < 0:
            y += h
        i = (y * w + x) * 3
        print(px[i], px[i + 1], px[i + 2])
    elif mode == 'canon':
        w, h, px = read(sys.argv[2])
        ow, oh, out = canonicalise(w, h, px, int(sys.argv[4]))
        write(sys.argv[3], ow, oh, out)
        print(ow, oh)
    elif mode == 'edgecol':
        # THE COLUMN OF THE FIRST COLOUR TRANSITION in one row, or -1.
        #
        # A readback with a wrong row pitch does not corrupt the picture, it
        # SHEARS it: every row is displaced from the one above by a constant,
        # every pixel is a real pixel and every colour is right. Extents cannot
        # see that and neither can a histogram. A straight vertical edge can:
        # if this returns the same column at the top, the middle and the bottom
        # of the frame, the rows are where they claim to be.
        w, h, px = read(sys.argv[2])
        y = int(sys.argv[3])
        if y < 0 or y >= h:
            print(-1)
            return
        base = y * w * 3
        first = px[base:base + 3]
        for x in range(1, w):
            i = base + x * 3
            if px[i:i + 3] != first:
                print(x)
                return
        print(-1)
    elif mode == 'edgespan':
        # EVERY row that has a colour transition, and the columns they are at.
        #
        # Prints: rows first_y last_y col_min col_max
        #
        # A row-pitch error displaces each row from the one above by a
        # constant, so a straight vertical edge becomes a diagonal and col_min
        # and col_max separate by roughly the number of rows measured. A
        # correct readback gives col_min == col_max over hundreds of rows.
        #
        # Self-calibrating on purpose: a fixture's windows do not reach the top
        # and bottom of every output shape, so a test that sampled three FIXED
        # rows measured uniform background at two of them and reported no edge
        # at all -- which is a premise failure, not a result.
        w, h, px = read(sys.argv[2])
        rows = 0
        first = last = -1
        cmin, cmax = 1 << 30, -1
        for y in range(h):
            base = y * w * 3
            head = px[base:base + 3]
            col = -1
            for x in range(1, w):
                i = base + x * 3
                if px[i:i + 3] != head:
                    col = x
                    break
            if col < 0:
                continue
            rows += 1
            if first < 0:
                first = y
            last = y
            cmin = min(cmin, col)
            cmax = max(cmax, col)
        if rows == 0:
            print(0, -1, -1, -1, -1)
        else:
            print(rows, first, last, cmin, cmax)
    elif mode == 'report':
        # ONE LINE PER FACT, for a capture and the reference it was compared
        # against. This is the reporting surface M4F.2C.4e asks the readback to
        # be: a mismatch should say what was compared, how much differed, and
        # where the first one is -- not just "0" or "not 0".
        #
        #     report A B [DIFF_IMAGE]
        #
        # With a third argument it also writes a difference image: every
        # matching pixel black, every differing one the absolute per-channel
        # difference amplified to be visible. Debug only, and never in a hot
        # path -- nothing in the renderer calls this.
        wa, ha, pa = read(sys.argv[2])
        wb, hb, pb = read(sys.argv[3])
        print('a_extent %dx%d' % (wa, ha))
        print('b_extent %dx%d' % (wb, hb))
        print('a_hash %08x' % fnv(pa))
        print('b_hash %08x' % fnv(pb))
        if (wa, ha) != (wb, hb):
            print('wrong -1   (different extents, nothing else is comparable)')
            return
        wrong = worst = 0
        fx = fy = -1
        x0 = y0 = 1 << 30
        x1 = y1 = -1
        out = bytearray(wa * ha * 3) if len(sys.argv) > 4 else None
        for y in range(ha):
            row = y * wa * 3
            for x in range(wa):
                i = row + x * 3
                if pa[i:i + 3] == pb[i:i + 3]:
                    continue
                wrong += 1
                if fx < 0:
                    fx, fy = x, y
                x0 = min(x0, x); y0 = min(y0, y)
                x1 = max(x1, x + 1); y1 = max(y1, y + 1)
                for k in range(3):
                    d = abs(pa[i + k] - pb[i + k])
                    if d > worst:
                        worst = d
                    if out is not None:
                        out[i + k] = min(255, d * 16)
        print('wrong %d' % wrong)
        print('worst_channel %d' % worst)
        print('first %d %d' % (fx, fy))
        if x1 < 0:
            print('bbox - - - -')
        else:
            print('bbox %d %d %d %d' % (x0, y0, x1, y1))
        if out is not None:
            write(sys.argv[4], wa, ha, bytes(out))
            print('diff_image %s' % sys.argv[4])
    elif mode == 'verify':
        # THE CANONICALISATION, CHECKED BY DATA RATHER THAN BY CONVENTION.
        #
        # transform_point() is written from the wl_output_transform definition,
        # and a definition can be transcribed backwards. So canonicalise the
        # same capture through every candidate and print what each one costs
        # against an independently rendered reference: the right one is 0 and
        # the seven wrong ones are enormous. A caller asserts both halves --
        # that the expected candidate wins, and that it wins ALONE, which is
        # what rules out a symmetric fixture that cannot tell two of them apart.
        w, h, px = read(sys.argv[2])
        rw, rh, rpx = read(sys.argv[3])
        want = int(sys.argv[4])
        for cand in range(8):
            ow, oh, out = canonicalise(w, h, px, cand)
            if (ow, oh) != (rw, rh):
                print(cand, 'shape', '%dx%d' % (ow, oh))
                continue
            differ = 0
            for i in range(0, len(out), 3):
                if out[i:i + 3] != rpx[i:i + 3]:
                    differ += 1
            print(cand, 'diff', differ, 'EXPECTED' if cand == want else '')
    elif mode == 'grid':
        # WHERE two images differ, at a glance. A pixel count and a bounding box
        # cannot tell a uniform one-code drift from a displaced window, and
        # those call for completely different investigations.
        got = _pair(sys.argv[2], sys.argv[3])
        if got is None:
            print('different sizes')
            return
        w, h, pa, pb = got
        n = int(sys.argv[4]) if len(sys.argv) > 4 else 24
        cw, ch = max(1, w // n), max(1, h // n)
        for gy in range(0, h, ch):
            row = ''
            for gx in range(0, w, cw):
                cnt = 0
                for y in range(gy, min(gy + ch, h)):
                    base = y * w * 3
                    for x in range(gx, min(gx + cw, w)):
                        i = base + x * 3
                        if pa[i:i + 3] != pb[i:i + 3]:
                            cnt += 1
                total = (min(gy + ch, h) - gy) * (min(gx + cw, w) - gx)
                if cnt == 0:
                    row += '.'
                elif cnt == total:
                    row += '#'
                else:
                    row += str(min(9, len(str(cnt)) ))
            print(row)
    elif mode == 'marks':
        w, h, px = read(sys.argv[2])
        for name, (x, y) in (('TL', (2, 2)), ('TR', (w - 3, 2)),
                             ('BL', (2, h - 3)), ('BR', (w - 3, h - 3))):
            i = (y * w + x) * 3
            print(name, px[i], px[i + 1], px[i + 2])
    else:
        raise SystemExit('unknown mode ' + mode)


main()
