#!/usr/bin/env python3
"""The defaults in the docs tables are the ones the compositor actually uses.

Every `| Setting | Default | Description |` table in docs/ is hand-maintained,
and the default column is the one cell in it that is mechanically knowable. It
had drifted: eight rows disagreed with the running code, and the two directions
of the drift are worth naming because they are different mistakes.

  - `animation_duration_focus` was documented as `0`, the value assigned in
    set_value_default -- but override_config clamps it to [1, 50000] on the very
    next line, so `0` is a number that has never once taken effect. Reading the
    assignment and stopping there is the obvious way to write that cell by hand.

  - three `theme_*` colours were simply someone's palette, pasted years ago and
    never revisited.

Both are invisible to a reader, who has no way to tell a stale cell from a live
one, and both are invisible to `-S`, which checks the schema against the code and
has no idea the docs exist.

WHY THIS CHECKS RATHER THAN GENERATES
-------------------------------------
The plan called for generating these tables from the schema between marker
comments. Doing it would have deleted the descriptions. The schema's `desc` is a
one-line tooltip sized for a settings panel; the docs descriptions carry markdown
links, KDL snippets and bitmask tables (`border_radius_location_default` spends
four clauses on the bit values, `float_click_to_focus` explains its interaction
with `sloppyfocus` and gives the KDL spelling). Generation would replace all of
that with the tooltip, which is a straight downgrade of the thing the docs are
for, and the plan says in as many words that the surrounding prose is the
valuable part.

So the schema owns the default and the writer owns the prose, and this asserts
the one against the other. `--fix` rewrites default cells and touches nothing
else.

VALUE, NOT SPELLING
-------------------
Defaults compare numerically for numeric types and case-insensitively for
colours, so `1.0` may document a default of `1` and `0x8BAA9Bff` may document
`0x8baa9bff`. A checker that demanded byte equality would force `blur_params_radius`
to read `5.0`, which is worse prose in service of nothing: the question a reader
has is what value takes effect, and that is what gets compared.

Usage: check-config-docs.py [path-to-asteroidz-binary] [--fix]
"""

import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DOCS = os.path.join(ROOT, "docs")
EXEMPT = os.path.join(ROOT, "tests", "docs-exempt.txt")

NUMERIC = {"int", "float", "double", "bool"}


def load_schema(binary):
    """key -> record, from `asteroidz -L`.

    From the binary, not by regexing config-schema.h, for the same reason
    check-config-schema.py does it this way: a pattern that stops matching is a
    checker that silently stops checking.
    """
    try:
        out = subprocess.run(
            [binary, "-L"], capture_output=True, text=True, check=True
        ).stdout
    except (OSError, subprocess.CalledProcessError) as e:
        sys.exit(f"check-config-docs: could not run `{binary} -L`: {e}")

    schema = {}
    for line in out.splitlines():
        if line.startswith("#") or not line.strip():
            continue
        f = line.split("\t")
        if len(f) < 8:
            continue
        schema[f[0]] = {
            "key": f[0],
            "path": f[1],
            "type": f[4],
            "default": f[7],
        }
    if not schema:
        sys.exit("check-config-docs: `-L` printed no options")
    return schema


def build_index(schema):
    """Every spelling a doc may legitimately use for an option.

    Three, because the docs use all three: the internal key (`borderpx`), the
    full KDL path (`layout/border/width`), and the KDL leaf (`focus-color`).
    Leaves are only indexed when unambiguous -- `color` is a leaf of four
    different paths, and resolving it to whichever came first would attach a
    default to the wrong row.
    """
    index = {}
    leaves = {}
    for o in schema.values():
        index[o["key"]] = o
        if o["path"]:
            index[o["path"]] = o
            leaves.setdefault(o["path"].split("/")[-1], []).append(o)
    for leaf, opts in leaves.items():
        if len(opts) == 1 and leaf not in index:
            index[leaf] = opts[0]
    return index


def same_value(doc, want, type_):
    """Does the documented default mean the same as the schema's?"""
    doc, want = doc.strip(), want.strip()
    if doc == want:
        return True
    if type_ == "color":
        return doc.lower() == want.lower()
    if type_ in NUMERIC:
        try:
            return float(doc) == float(want)
        except ValueError:
            return False
    return False


CELL = re.compile(r"^\s*\|(.*)\|\s*$")
SEP = re.compile(r"^[\s:|-]+$")


def split_row(line):
    m = CELL.match(line.rstrip())
    return [c.strip() for c in m.group(1).split("|")] if m else None


def tables(lines):
    """Yield (header, [(lineno, cells)]) for each markdown table."""
    i = 0
    while i < len(lines):
        head = split_row(lines[i])
        if head is None or i + 1 >= len(lines) or not SEP.match(lines[i + 1]):
            i += 1
            continue
        rows = []
        j = i + 2
        while j < len(lines):
            cells = split_row(lines[j])
            if cells is None:
                break
            rows.append((j, cells))
            j += 1
        yield [h.strip().lower() for h in head], rows
        i = j


def check_defaults(index, fix):
    """Compare every documented default against the schema."""
    problems = []
    fixed = 0

    for dirpath, _, names in os.walk(DOCS):
        for name in sorted(names):
            if not name.endswith((".md", ".mdx")):
                continue
            path = os.path.join(dirpath, name)
            rel = os.path.relpath(path, ROOT)
            raw = open(path, encoding="utf-8").read()
            lines = raw.splitlines()
            # Preserved, because several docs files end without one and adding
            # it would put an unrelated hunk in the diff of every --fix run.
            trailer = "\n" if raw.endswith("\n") else ""
            dirty = False

            for header, rows in tables(lines):
                if "default" not in header:
                    continue
                col = header.index("default")
                for lineno, cells in rows:
                    if col >= len(cells) or not cells:
                        continue
                    key = cells[0].strip().strip("`").strip()
                    opt = index.get(key)
                    if opt is None:
                        continue
                    doc = cells[col].strip().strip("`").strip()
                    if not doc:
                        continue
                    if same_value(doc, opt["default"], opt["type"]):
                        continue
                    if fix:
                        cells[col] = f"`{opt['default']}`"
                        lines[lineno] = "| " + " | ".join(cells) + " |"
                        dirty = True
                        fixed += 1
                    else:
                        problems.append(
                            f"{rel}:{lineno + 1}: {key} documented as "
                            f"{doc!r}, schema says {opt['default']!r}"
                        )
            if dirty:
                with open(path, "w", encoding="utf-8") as fh:
                    fh.write("\n".join(lines) + trailer)

    return problems, fixed


def load_exempt():
    """Options deliberately absent from the docs, each with a stated reason.

    Same shape as tests/schema-exempt.txt: an escape hatch nobody has to justify
    stops being one.
    """
    if not os.path.exists(EXEMPT):
        return {}
    out = {}
    for raw in open(EXEMPT, encoding="utf-8"):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        key, _, why = line.partition(":")
        out[key.strip()] = why.strip()
    return out


def check_coverage(schema, exempt):
    """Is each option findable in the docs, and is it actually named?

    Two tiers, because "documented" and "mentioned" are different states and
    only one of them is a build failure.

      NAMED    the key, its full KDL path, or its KDL leaf appears in backticks.
               A reader searching for it finds it.

      SHOWN    the leaf appears as a bare word somewhere -- almost always inside
               a ```kdl example. `border_gradient_angle` is like this: the
               Borders section explains the gradient and prints `angle 45` in a
               config block, but never writes the word `angle` as a term. That
               is real documentation and failing on it would only teach people
               to write exemptions.

    Only NEITHER fails. SHOWN options are printed as a to-do, so the gap stays
    visible without blocking anything.
    """
    ticked = set()
    words = set()
    for dirpath, _, names in os.walk(DOCS):
        for name in names:
            if not name.endswith((".md", ".mdx")):
                continue
            text = open(os.path.join(dirpath, name), encoding="utf-8").read()
            ticked.update(re.findall(r"`([^`\n]+)`", text))
            words.update(re.findall(r"[A-Za-z][A-Za-z0-9_-]*", text))

    missing, shown = [], []
    for key, o in sorted(schema.items()):
        leaf = o["path"].split("/")[-1] if o["path"] else key
        spellings = {key, o["path"], leaf} - {""}
        if spellings & ticked:
            continue
        if key in exempt:
            continue
        (shown if (leaf in words or key in words) else missing).append(key)
    stale = sorted(k for k in exempt if k not in schema)
    return missing, shown, stale


def main():
    args = [a for a in sys.argv[1:] if a != "--fix"]
    fix = "--fix" in sys.argv[1:]
    binary = args[0] if args else os.path.join(ROOT, "build", "asteroidz")

    schema = load_schema(binary)
    index = build_index(schema)
    exempt = load_exempt()

    problems, fixed = check_defaults(index, fix)
    missing, shown, stale = check_coverage(schema, exempt)

    if fix:
        print(f"check-config-docs: rewrote {fixed} default cell(s)")

    bad = False
    if problems:
        bad = True
        print(f"check-config-docs: {len(problems)} default(s) disagree with the schema:")
        for p in problems:
            print(f"  {p}")
        print("\n  Run tests/check-config-docs.py --fix to update them.")
    if missing:
        bad = True
        print(f"\ncheck-config-docs: {len(missing)} option(s) appear nowhere in docs/:")
        for k in missing:
            o = schema[k]
            print(f"  {k}  (KDL: {o['path'] or k})")
        print(
            "\n  Document them, or add them to tests/docs-exempt.txt with a reason."
        )
    if shown:
        print(
            f"\ncheck-config-docs: note -- {len(shown)} option(s) appear only in an "
            "example, never named:"
        )
        for k in shown:
            o = schema[k]
            print(f"  {k}  (KDL: {o['path'] or k})")
        print("  Not a failure. Backtick the name where it is explained.")
    if stale:
        bad = True
        print(f"\ncheck-config-docs: {len(stale)} exempt entr(ies) name a dead option:")
        for k in stale:
            print(f"  {k}")

    if bad:
        return 1
    print(f"check-config-docs: {len(schema)} options, defaults and coverage OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
