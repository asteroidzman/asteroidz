#!/usr/bin/env python3
"""Turn a .spv blob into a C array.

SPIR-V is loaded from memory at pipeline creation, so the alternative is
shipping .spv files next to the binary and finding them at runtime -- a search
path, an install rule, and a way for the compositor to fail to start because a
shader moved. Embedding is one less thing that can be missing.
"""
import sys
from pathlib import Path

src, dst, name = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3]
data = src.read_bytes()
if len(data) % 4:
    sys.exit(f"{src}: SPIR-V must be a whole number of 32-bit words")

words = [int.from_bytes(data[i:i + 4], "little") for i in range(0, len(data), 4)]
out = [f"/* generated from {src.name} -- do not edit */",
       "#pragma once", "#include <stdint.h>", "",
       f"static const uint32_t {name}[] = {{"]
for i in range(0, len(words), 8):
    out.append("\t" + " ".join(f"0x{w:08x}," for w in words[i:i + 8]))
out += ["};", ""]
dst.write_text("\n".join(out))
