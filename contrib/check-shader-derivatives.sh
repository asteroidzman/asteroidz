#!/usr/bin/env sh
# check-shader-derivatives.sh — the one shader rule that nothing else catches.
#
# A fragment derivative (fwidth/dFdx/dFdy) is computed across the 2x2 quad the
# GPU shades together. If any invocation of that quad took a different branch
# and never produced the value, what the survivors read is UNDEFINED.
#
# This is not theoretical. az_rounded_coverage() evaluated fwidth() after a
# per-pixel early return for the whole of M4A and rendered correctly the entire
# time; it only broke when M4B called the helper a second time and the compiler
# laid the shader out differently, painting a one-pixel column of border down
# the outside of a corner and varying run to run on identical input.
#
# Vulkan validation does NOT catch it -- there is no layer for undefined
# derivatives -- and no pixel test does either, because the output is correct
# until it isn't. Structure is the only guard.
#
# COMMENTS ARE STRIPPED FIRST, and that is not tidiness. The first version of
# this check matched the word "fwidth()" in the explanatory comment ABOVE the
# code, decided the derivative had already been seen, and then passed against a
# deliberately reintroduced defect. A guard that reads its own documentation as
# code is worse than none.
set -u
DIR="$(dirname "$0")/../src/render/vulkan/shader/src"
python3 - "$DIR" <<'PY'
import glob, os, re, sys

d = sys.argv[1]
bad = []
for path in sorted(glob.glob(os.path.join(d, "*.glsl")) +
                   glob.glob(os.path.join(d, "*.frag")) +
                   glob.glob(os.path.join(d, "*.vert"))):
    src = open(path).read()
    # strip block and line comments, keeping line numbering intact
    src = re.sub(r'/\*.*?\*/', lambda m: "\n" * m.group(0).count("\n"),
                 src, flags=re.S)
    src = re.sub(r'//[^\n]*', '', src)

    infn = False
    seen = False
    risky = None
    for n, line in enumerate(src.splitlines(), 1):
        if re.match(r'^[A-Za-z_][\w \t\*]*\([^;]*\)\s*\{', line):
            infn, seen, risky = True, False, None
            continue
        if line.startswith('}'):
            infn = False
            continue
        if not infn:
            continue
        if not seen and re.search(r'\b(fwidth|dFdx|dFdy)\s*\(', line):
            seen = True
            if risky:
                bad.append(f"{path}:{n}: derivative reached after a "
                           f"per-fragment branch at line {risky[0]}:"
                           f"{risky[1].strip()}")
            continue
        if not seen and re.search(r'\b(if|for|while|discard)\b', line):
            # Per-fragment: anything derived from gl_FragCoord or a varying.
            # A branch on push-constant radii is uniform and cannot split a
            # quad, so it is deliberately allowed.
            if re.search(r'gl_FragCoord|(?<![A-Za-z_])p\s*\.\s*[xy]|v_uv', line):
                risky = (n, line)

for b in bad:
    print(b, file=sys.stderr)
if bad:
    print("FAIL: a fragment derivative is reachable through non-uniform "
          "control flow.", file=sys.stderr)
    print("Compute the value and its derivative unconditionally, then branch.",
          file=sys.stderr)
    sys.exit(1)
print("ok - no derivative is evaluated after a per-fragment branch")
PY
