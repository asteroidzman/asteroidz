#!/usr/bin/env python3
"""The amsg man page's dispatch list, against the compositor's own.

A man page listing 94 action names is a second copy of a table, and a second
copy drifts: the first action added after it was written would be missing, and
nothing would say so. So the list is generated from `asteroidz -D` and this
checks it still matches -- names, order and argument kinds.

Run with --fix to regenerate the block in place. Everything outside the
BEGIN/END markers is hand-written prose and is never touched.

Usage: check-amsg-man.py [--fix] [--asteroidz PATH] [--man PATH]
"""

import argparse
import os
import re
import subprocess
import sys

BEGIN = '.\\" BEGIN GENERATED DISPATCHES\n'
END = '.\\" END GENERATED DISPATCHES\n'


def dispatches(binary):
    out = subprocess.run([binary, "-D"], capture_output=True, text=True,
                         check=True).stdout
    rows = []
    for line in out.splitlines():
        if not line or line.startswith("#"):
            continue
        parts = line.split("\t")
        rows.append((parts[0], (parts[1] if len(parts) > 1 else "").strip()))
    return rows


def render(rows):
    out = []
    for name, args in rows:
        if args:
            out.append(".TP\n\\fB%s\\fR,<%s>" % (name, ">,<".join(args.split())))
        else:
            out.append(".TP\n\\fB%s\\fR" % name)
    return "\n".join(out) + "\n"


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(here)
    ap = argparse.ArgumentParser()
    ap.add_argument("--fix", action="store_true")
    ap.add_argument("--asteroidz", default=os.path.join(root, "build", "asteroidz"))
    ap.add_argument("--man", default=os.path.join(root, "amsg", "amsg.1"))
    a = ap.parse_args()

    if not os.path.exists(a.asteroidz):
        print("check-amsg-man: no binary at %s; build first" % a.asteroidz,
              file=sys.stderr)
        return 77  # meson's "skipped"

    text = open(a.man).read()
    if BEGIN not in text or END not in text:
        print("check-amsg-man: the generated block's markers are missing from %s"
              % a.man, file=sys.stderr)
        return 1

    i = text.index(BEGIN) + len(BEGIN)
    j = text.index(END)
    want = render(dispatches(a.asteroidz))

    if text[i:j] == want:
        print("check-amsg-man: %d dispatches, man page matches"
              % len(dispatches(a.asteroidz)))
        return 0

    if a.fix:
        open(a.man, "w").write(text[:i] + want + text[j:])
        print("check-amsg-man: regenerated %s" % a.man)
        return 0

    # Say WHICH, not just "differs" -- the whole point is that somebody added an
    # action and did not know this file existed.
    # `\fBname\fR` or `\fBname\fR,<kind>` -- taken with a pattern rather than
    # by slicing off fixed widths, which is how an earlier version of this
    # reported a missing action called "oo".
    def names(block):
        return {m.group(1)
                for m in re.finditer(r"^\\fB([a-z_0-9]+)\\fR", block, re.M)}

    have = names(text[i:j])
    now = names(want)
    for n in sorted(now - have):
        print("  missing from the man page: %s" % n, file=sys.stderr)
    for n in sorted(have - now):
        print("  in the man page but not in the compositor: %s" % n,
              file=sys.stderr)
    if now == have:
        print("  the same actions, but the order or argument kinds changed",
              file=sys.stderr)
    print("check-amsg-man: FAILED -- rerun with --fix", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
