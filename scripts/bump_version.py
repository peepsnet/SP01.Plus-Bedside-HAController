#!/usr/bin/env python3
"""Bump the version in /VERSION. Usage: bump_version.py patch|minor|major  (prints the new version)."""
import pathlib, sys

path = pathlib.Path(__file__).resolve().parent.parent / "VERSION"
major, minor, patch = (int(x) for x in path.read_text().strip().split("."))
kind = sys.argv[1] if len(sys.argv) > 1 else "patch"
if kind == "major":
    major, minor, patch = major + 1, 0, 0
elif kind == "minor":
    minor, patch = minor + 1, 0
elif kind == "patch":
    patch += 1
else:
    sys.exit("usage: bump_version.py patch|minor|major")
new = f"{major}.{minor}.{patch}"
path.write_text(new + "\n")
print(new)
