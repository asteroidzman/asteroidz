#!/usr/bin/env python3
"""Every key parse_option handles is either described or deliberately exempt.

`asteroidz -S` checks the schema against the code in one direction: for each
entry in the table, is the default right, is the clamp right, does parse_option
reach it. It cannot check the other direction -- a key that exists in
parse_option and is simply MISSING from the table is invisible to it, because
there is no entry to run a check against. That is what this script is for, and
it is the direction that actually matters as the parser grows: a new option added
to parse_option and forgotten here would never appear in the settings UI, with
nothing to say so.

Two sources, deliberately different:

  - the DESCRIBED keys come from the binary, via `asteroidz -L`. Extracting them
    from the C table by regex is the kind of fragile step that makes a checker
    quietly stop checking; an over-matching pattern reported 100 keys where
    there were 95, which would have hidden five missing entries.
  - the HANDLED keys have to come from parsing parse_option, because there is
    nowhere else that list exists. Matched at brace depth 1 of the function
    body, so the sub-keys nested inside the windowrule/monitorrule/tagrule
    branches are not mistaken for standalone options.

Exemptions live in tests/schema-exempt.txt and must sit under a `## reason:`
heading. A bare list of exempt keys is an escape hatch nobody has to justify,
which is not an escape hatch at all.

Usage: check-config-schema.py [path-to-asteroidz-binary]
"""

import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PARSE_CONFIG = os.path.join(ROOT, "src", "config", "parse_config.h")
EXEMPT = os.path.join(ROOT, "tests", "schema-exempt.txt")


def function_body(src, signature_re):
    """The braced body of the first function matching signature_re."""
    m = re.search(signature_re, src)
    if not m:
        sys.exit(f"check-config-schema: could not find {signature_re}")
    i = m.end()
    depth = 1
    while depth > 0 and i < len(src):
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
        i += 1
    return src[m.end() : i]


def handled_keys(body):
    """Keys compared against `key` at the top level of the if/else chain.

    Depth matters: the windowrule, monitorrule, tagrule and layerrule branches
    each contain dozens of strcmp calls against their OWN sub-keys (isfloating,
    nmaster, hdr_max_fall). Those are fields of a rule, not standalone options,
    and counting them would demand schema entries for things a settings UI
    reaches through a rule editor instead.
    """
    keys = []
    depth = 0
    pattern = re.compile(r'[{}]|str(?:n)?cmp\(key,\s*"([^"]+)"')
    for m in pattern.finditer(body):
        tok = m.group(0)
        if tok == "{":
            depth += 1
        elif tok == "}":
            depth -= 1
        elif depth == 0:
            keys.append(m.group(1))
    seen = []
    for k in keys:
        if k not in seen:
            seen.append(k)
    return seen


def read_exempt(path):
    """{key: reason}. Keys outside a `## reason:` heading are an error."""
    exempt = {}
    orphans = []
    reason = None
    with open(path) as f:
        for lineno, raw in enumerate(f, 1):
            line = raw.strip()
            if line.startswith("## reason:"):
                reason = line[len("## reason:") :].strip()
                continue
            if not line or line.startswith("#"):
                continue
            if reason is None:
                orphans.append((lineno, line))
                continue
            exempt[line] = reason
    return exempt, orphans


def described_keys(binary):
    try:
        out = subprocess.run(
            [binary, "-L"], capture_output=True, text=True, check=True
        ).stdout
    except (OSError, subprocess.CalledProcessError) as e:
        sys.exit(f"check-config-schema: could not run `{binary} -L`: {e}")
    keys = []
    for line in out.splitlines():
        if not line or line.startswith("#"):
            continue
        keys.append(line.split("\t")[0])
    if not keys:
        sys.exit("check-config-schema: `-L` printed no options")
    return keys


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        ROOT, "build", "asteroidz"
    )

    src = open(PARSE_CONFIG).read()
    handled = handled_keys(function_body(src, r"parse_option\s*\([^)]*\)\s*\{"))
    described = described_keys(binary)
    exempt, orphans = read_exempt(EXEMPT)

    failures = []

    for lineno, line in orphans:
        failures.append(
            f"schema-exempt.txt:{lineno}: `{line}` is not under a `## reason:` "
            f"heading -- an exemption has to say why"
        )

    missing = [k for k in handled if k not in described and k not in exempt]
    for k in missing:
        failures.append(
            f"`{k}` is handled by parse_option but is neither in the schema nor "
            f"exempt -- add it to config-schema.h, or to tests/schema-exempt.txt "
            f"under a reason"
        )

    # An exemption for a key that no longer exists is stale, and a key that is
    # BOTH described and exempt means someone did the work and forgot to delete
    # the line -- which would let the next real omission hide behind it.
    for k in sorted(exempt):
        if k in described:
            failures.append(
                f"`{k}` is in the schema AND exempt -- delete its line from "
                f"tests/schema-exempt.txt"
            )
        elif k not in handled:
            failures.append(
                f"`{k}` is exempt but parse_option no longer handles it -- "
                f"delete its stale line from tests/schema-exempt.txt"
            )

    for k in described:
        if k not in handled:
            failures.append(
                f"`{k}` is in the schema but parse_option does not handle it "
                f"(`asteroidz -S` should also catch this)"
            )

    print(
        f"parse_option handles {len(handled)} keys: "
        f"{len(described)} described, {len(exempt)} exempt, {len(missing)} missing"
    )
    if not failures:
        by_reason = {}
        for k, r in exempt.items():
            by_reason[r] = by_reason.get(r, 0) + 1
        for reason, n in sorted(by_reason.items(), key=lambda kv: -kv[1]):
            print(f"  {n:4d} exempt: {reason.splitlines()[0]}")
        print("ok")
        return 0

    for f in failures:
        print(f"FAIL {f}", file=sys.stderr)
    print(f"\n{len(failures)} problems", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
