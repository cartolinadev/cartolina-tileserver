#!/usr/bin/env python3
"""
Resolve symlinks in compile_commands.json so clangd can match source files
opened via their real paths (externals/...) instead of the symlinked paths
(mapproxy/src/...).

Usage:
    fix-compile-commands.py <src> <dst>
"""
import json
import pathlib
import sys

if len(sys.argv) != 3:
    sys.exit(f"Usage: {sys.argv[0]} <src> <dst>")

src, dst = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])

if not src.exists():
    sys.exit(f"Source not found: {src}")

with open(src) as f:
    entries = json.load(f)

for entry in entries:
    real = pathlib.Path(entry["file"]).resolve()
    if real.exists():
        entry["file"] = str(real)

with open(dst, "w") as f:
    json.dump(entries, f, indent=2)
