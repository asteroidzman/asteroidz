#!/usr/bin/env python3
"""M6A/ADR-606 falsifier I10: animation PROGRESS must not be sampled from a clock.

The frame's sample instant is decided once per output pass by the presenter and
threaded down as a parameter. Before M6A, five separate places under
src/animation/ called clock_gettime() for themselves, so two windows animating
in the same frame sampled at instants separated by however long the walk
between them took -- invisible on a still desktop, and not a rounding detail:
two objects disagreeing about what time the frame is.

Threading fixes that only for as long as nobody adds a sixth, and a clock read
is a one-line change that compiles, runs, looks right and silently
reintroduces the defect. It has no runtime symptom to assert on, so the check
is static.

WHAT IS *NOT* BANNED, and getting this wrong is how a check gets switched off:

    c->animation.time_started = get_now_in_ms();

is a SEMANTIC event -- "this animation began now" -- and ADR-611 puts semantic
state on the CPU by design. So is a grace-period deadline. Reading the clock to
decide WHEN SOMETHING HAPPENED is correct; reading it to decide WHAT A FRAME
SHOWS is the defect. Only the functions that evaluate progress for a frame are
in scope, and they are named below.
"""
import re
import sys
from pathlib import Path

WATCHED = [
    "src/animation/client.h",
    "src/animation/layer.h",
    "src/animation/tag.h",
    "src/animation/common.h",
]

# The functions that turn elapsed time into a frame's geometry/opacity. These
# receive `sample_ns` and must use it.
SAMPLING = {
    "client_animation_next_tick",
    "fadeout_client_animation_next_tick",
    "layer_animation_next_tick",
    "fadeout_layer_animation_next_tick",
    "client_apply_focus_opacity",
    "client_draw_frame",
    "client_draw_fadeout_frame",
    "layer_draw_frame",
    "layer_draw_fadeout_frame",
}

BANNED = re.compile(r"\b(clock_gettime|get_now_in_ms|gettimeofday)\b")
# A definition at column 0: `type name(args) {`
DEFN = re.compile(r"^[A-Za-z_][A-Za-z0-9_ *]*\b([a-z_][a-z0-9_]*)\s*\([^;]*\)\s*\{")

root = Path(__file__).resolve().parent.parent
failures = []
scanned = []

for rel in WATCHED:
    path = root / rel
    if not path.exists():
        continue
    fn = None
    depth = 0
    for n, line in enumerate(path.read_text().splitlines(), 1):
        stripped = line.lstrip()
        if depth == 0:
            m = DEFN.match(line)
            if m:
                fn = m.group(1)
                if fn in SAMPLING:
                    scanned.append(f"{rel}:{fn}")
        if fn in SAMPLING and not stripped.startswith(("*", "/*", "//")):
            hit = BANNED.search(line)
            if hit:
                failures.append(
                    f"{rel}:{n}: {hit.group(1)} inside {fn}()\n      {stripped}")
        depth += line.count("{") - line.count("}")
        if depth <= 0:
            depth = 0
            fn = None

if len(scanned) < len(SAMPLING):
    # The check silently passing because it matched nothing is the failure mode
    # this project has been bitten by repeatedly. Assert the premise.
    missing = SAMPLING - {s.split(":")[1] for s in scanned}
    print(f"FAIL: premise -- only found {len(scanned)} of {len(SAMPLING)} "
          f"sampling functions; the parser is not looking at: {sorted(missing)}")
    sys.exit(1)

print(f"checked {len(scanned)} sampling functions in {len(WATCHED)} files")
if failures:
    print("\nFAIL: a frame's animation progress must come from the instant it")
    print("was HANDED (sample_ns), not from a clock read here. See ADR-606.\n")
    for f in failures:
        print("  " + f)
    sys.exit(1)
print("ok - every sampling function uses the threaded instant")
