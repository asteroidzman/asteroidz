#!/usr/bin/env python3
"""Every dispatchable action is described, and every described one is real.

`dispatch_actions[]` in src/ipc/ipc-config.h tells a settings UI what actions
exist and what arguments each takes, so it can offer a keybind editor with
pickers instead of a text box. It is hand-written beside `parse_func_name`, for
the same reason `config_schema[]` is hand-written beside `parse_option`: the
description and the argument KINDS cannot be extracted from the code, because
the code does not distinguish a tag index from an integer.

Which means it can drift, in both directions, and both are bad:

  * an action in parse_func_name and not in the table never appears in the UI,
    with nothing to say so;
  * an action in the table and not in parse_func_name is a control that answers
    `{"error":"unknown function"}` when clicked.

Same split of sources as check-config-schema.py, for the same reasons: the
described actions come from the BINARY (`asteroidz -D`), the real ones by parsing
`parse_func_name`, because that is the only place they exist.

Usage: check-dispatch-actions.py [path-to-asteroidz-binary]
"""

import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PARSE_CONFIG = os.path.join(ROOT, "src", "config", "parse_config.h")

# Selected by argument VALUE rather than by name -- `toggle_overview jump` picks
# a different function than `toggle_overview` -- so parse_func_name matches these
# against func_name while they are not separately dispatchable names.
SUBMODE_NAMES = {"togglejump", "ufo"}

KINDS = {
    "int", "float", "string", "bool", "tag-index", "direction",
    "circle-direction", "layout-name", "option-key", "option-value",
}


def function_body(src, signature_re):
    m = re.search(signature_re, src)
    if not m:
        sys.exit(f"check-dispatch-actions: could not find {signature_re}")
    i = m.end()
    depth = 1
    while depth > 0 and i < len(src):
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
        i += 1
    return src[m.end() : i]


def real_actions(body):
    """Names compared against `func_name` at the top level of the chain."""
    names = []
    depth = 0
    for m in re.finditer(r'[{}]|strcmp\(func_name,\s*"([^"]+)"', body):
        tok = m.group(0)
        if tok == "{":
            depth += 1
        elif tok == "}":
            depth -= 1
        elif depth == 0:
            names.append(m.group(1))
    out = []
    for n in names:
        if n not in out and n not in SUBMODE_NAMES:
            out.append(n)
    return out


def described_actions(binary):
    try:
        out = subprocess.run(
            [binary, "-D"], capture_output=True, text=True, check=True
        ).stdout
    except (OSError, subprocess.CalledProcessError) as e:
        sys.exit(f"check-dispatch-actions: could not run `{binary} -D`: {e}")
    rows = {}
    for line in out.splitlines():
        if not line or line.startswith("#"):
            continue
        parts = line.split("\t")
        rows[parts[0]] = parts[1] if len(parts) > 1 else ""
    if not rows:
        sys.exit("check-dispatch-actions: `-D` printed no actions")
    return rows


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        ROOT, "build", "asteroidz"
    )
    src = open(PARSE_CONFIG).read()
    real = real_actions(function_body(src, r"parse_func_name\s*\([^)]*\)\s*\{"))
    described = described_actions(binary)

    failures = []
    missing = [n for n in real if n not in described]

    for name in missing:
        failures.append(
            f"`{name}` is dispatchable but not in dispatch_actions[] -- a "
            f"settings UI will never offer it"
        )
    for name in described:
        if name not in real:
            failures.append(
                f"`{name}` is in dispatch_actions[] but parse_func_name does not "
                f"accept it -- the UI would offer a control that errors"
            )
    # An unknown kind is a UI falling back to a text box without knowing it did.
    for name, args in described.items():
        for kind in args.split():
            if kind not in KINDS:
                failures.append(
                    f"`{name}` declares argument kind `{kind}`, which is not one "
                    f"of: {', '.join(sorted(KINDS))}"
                )

    print(
        f"parse_func_name accepts {len(real)} actions: "
        f"{len(described)} described, {len(missing)} missing"
    )
    if not failures:
        print("ok")
        return 0
    for f in failures:
        print(f"FAIL {f}", file=sys.stderr)
    print(f"\n{len(failures)} problems", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
