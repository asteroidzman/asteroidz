#!/usr/bin/env python3
"""Every window-rule key the parser handles is described in rule_schema[].

`asteroidz -S` checks the rule table against the code in one direction: for each
entry, write it through the real windowrule parser and read it back through its
own offset. It cannot see the other direction -- a key that exists in the parser
and is simply MISSING from the table has no entry to run a check against, so it
is invisible there and invisible in the rule editor, with nothing to say so.

That direction is not hypothetical here. Writing the table turned up six keys the
parser has always accepted and no documentation mentioned (force_hdr,
isnotitlebar, nofocus, noscanout, shield_when_capture, vrr_only_fullscreen), plus
`single_scratchpad`, documented as a window rule the parser has never accepted.
This is what stops that happening again.

Two sources, deliberately different, for the same reason check-config-schema.py
uses two:

  - the DESCRIBED keys come from the binary, via `asteroidz -R`. Extracting them
    from the C table by regex is the fragile step that makes a checker quietly
    stop checking.
  - the HANDLED keys have to come from parsing the windowrule branch of
    parse_option, because there is nowhere else that list exists.

Unlike the option schema there is no exempt list, and there should not be one.
Fifty-three fields is a size a person can finish, the table is new, and an escape
hatch added before anything needs it is an invitation to leave the next one out.

Usage: check-rule-schema.py [path-to-asteroidz-binary]
"""

import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PARSE_CONFIG = os.path.join(ROOT, "src", "config", "parse_config.h")


def windowrule_branch(src):
    """The body of parse_option's `windowrule` branch.

    Delimited by the two `else if` lines around it rather than by brace
    counting: the branch is a flat if/else chain over strtok'd tokens with no
    nested key handling, so the boundaries are unambiguous and a brace counter
    would only be a second thing to get wrong.
    """
    start = src.find('} else if (strcmp(key, "windowrule") == 0) {')
    if start < 0:
        sys.exit("check-rule-schema: could not find the windowrule branch")
    end = src.find('} else if (strncmp(key, "env", 3) == 0) {', start)
    if end < 0:
        sys.exit("check-rule-schema: could not find the end of the branch")
    return src[start:end]


def handled_keys(src):
    branch = windowrule_branch(src)
    keys = set(re.findall(r'strcmp\(key, "([a-z_0-9]+)"\) == 0', branch))
    # The branch's own key, which is the option, not a field of it.
    keys.discard("windowrule")
    return keys


def described(binary):
    out = subprocess.run([binary, "-R"], capture_output=True, text=True)
    if out.returncode != 0:
        sys.exit(f"check-rule-schema: {binary} -R failed:\n{out.stderr}")
    keys = set()
    nice = {}
    for line in out.stdout.splitlines():
        if not line or line.startswith("#"):
            continue
        parts = line.split("\t")
        if len(parts) < 2:
            sys.exit(f"check-rule-schema: unparseable -R line: {line!r}")
        keys.add(parts[0])
        nice[parts[0]] = parts[1]
    return keys, nice


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        ROOT, "build", "asteroidz")
    if not os.path.exists(binary):
        sys.exit(f"check-rule-schema: no binary at {binary}")

    src = open(PARSE_CONFIG, encoding="utf-8").read()
    handled = handled_keys(src)
    desc, nice = described(binary)

    if len(handled) < 40:
        # The extractor matched almost nothing, which means the branch moved or
        # was reshaped. Failing here is the point: a checker that silently finds
        # zero keys reports success forever.
        sys.exit(f"check-rule-schema: only found {len(handled)} keys in the "
                 "windowrule branch -- the extractor is broken, not the schema")

    missing = sorted(handled - desc)
    extra = sorted(desc - handled)
    rc = 0

    if missing:
        print(f"{len(missing)} window-rule key(s) the parser handles but "
              "rule_schema[] does not describe:")
        for k in missing:
            print(f"  {k}")
        print()
        print("Add them to src/config/rule-schema.h with a label, a group and a")
        print("sentence saying what they do. They are invisible in the rule")
        print("editor until you do.")
        rc = 1

    if extra:
        print(f"{len(extra)} key(s) described but not handled by the parser:")
        for k in extra:
            print(f"  {k}")
        print()
        print("A rule the editor offers and the compositor rejects.")
        rc = 1

    # A nice name that is also a different field's key resolves to the wrong
    # branch, because rule_field_key_for_nice returns its argument unchanged when
    # nothing matches. `-S` checks this too; it is cheap and the failure is
    # silent, so it is worth having in both.
    for key, n in nice.items():
        if n != key and n in desc:
            print(f"KDL name {n!r} for {key!r} is another field's key")
            rc = 1

    if rc == 0:
        print(f"{len(desc)} window-rule fields, all described, "
              f"{len(handled)} handled")
    return rc


if __name__ == "__main__":
    sys.exit(main())
