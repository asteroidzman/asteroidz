#!/usr/bin/env python3
"""Keep EGL, GLES and wlroots' renderer out of the Vulkan engine.

docs/vulkan-native-architecture.md says that nothing under src/render/vulkan/
may depend on the abstractions this work exists to stop being constrained by.
That is easy to state and easy to violate by accident -- one `#include
<wlr/render/wlr_renderer.h>` to reach for a type that already exists, and the
new architecture is quietly shaped by the old one again.

So it is checked. A build fails rather than a reviewer noticing.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VULKAN_DIR = ROOT / "src" / "render" / "vulkan"

# Substrings that must not appear in an #include under src/render/vulkan/.
# Matched against the header path, so <wlr/render/wlr_renderer.h> and
# "render/egl.h" are both caught.
FORBIDDEN = [
    ("EGL/", "EGL"),
    ("egl.h", "EGL"),
    ("GLES2/", "OpenGL ES"),
    ("GLES3/", "OpenGL ES"),
    ("GL/gl", "OpenGL"),
    ("wlr/render/", "wlroots renderer"),
    ("wlr_renderer", "wlroots renderer"),
    ("wlr_texture", "wlroots renderer"),
    ("wlr_allocator", "wlroots renderer"),
    ("scenefx/", "SceneFX"),
    ("fx_renderer", "SceneFX renderer"),
]

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]')


def main() -> int:
    if not VULKAN_DIR.is_dir():
        print(f"no {VULKAN_DIR}, nothing to check")
        return 0

    problems = []
    checked = 0

    for path in sorted(VULKAN_DIR.rglob("*")):
        if path.suffix not in (".c", ".h"):
            continue
        checked += 1
        for lineno, line in enumerate(
            path.read_text(encoding="utf-8").splitlines(), 1
        ):
            match = INCLUDE_RE.match(line)
            if not match:
                continue
            header = match.group(1)
            for needle, what in FORBIDDEN:
                if needle in header:
                    problems.append(
                        f"{path.relative_to(ROOT)}:{lineno}: includes "
                        f"<{header}> ({what})"
                    )
                    break

    if problems:
        print("The Vulkan engine must not depend on the renderer abstractions")
        print("it replaces (docs/vulkan-native-architecture.md §5.1):")
        print()
        for problem in problems:
            print(f"  {problem}")
        return 1

    print(f"checked {checked} files under src/render/vulkan: no EGL, no GLES, "
          f"no wlr_renderer, no SceneFX")
    return 0


if __name__ == "__main__":
    sys.exit(main())
