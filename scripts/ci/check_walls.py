#!/usr/bin/env python3
"""Include/symbol grep walls (ARCHITECTURE §1.2). Exit code 1 on any violation.

Each wall: a regex that may only appear under the allowed path prefixes. A line containing
`mce-allow` is exempt (use sparingly, with a reason in the same comment).
"""

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
SCAN_DIRS = ["src", "samples", "games", "tests", "tools"]
EXTS = {".h", ".hpp", ".inl", ".c", ".cc", ".cpp"}

# (name, pattern, allowed path prefixes, scanned path prefixes)
WALLS = [
    ("GL wall", re.compile(r"\bgl[A-Z]\w*\s*\("), ["src/engine/rhi_gl/"], ["src/", "samples/", "games/"]),
    ("SDL wall", re.compile(r"\bSDL_Event\b"), ["src/engine/platform/"], ["src/", "samples/", "games/"]),
    # SDL headers: platform, plus the sanctioned native_handle() consumers (GL context, ImGui backend).
    (
        "SDL include wall",
        re.compile(r"[<\"]SDL3/"),
        ["src/engine/platform/", "src/engine/rhi_gl/", "src/editor/layer/"],
        ["src/", "samples/", "games/"],
    ),
    ("glad wall", re.compile(r"[<\"]glad/"), ["src/engine/rhi_gl/"], ["src/", "samples/", "games/", "tests/"]),
    ("ImGui wall", re.compile(r"\bImGui(::|zmo)|[<\"]imgui"), ["src/editor/layer/"], ["src/", "samples/", "games/"]),
    ("JPH wall", re.compile(r"\bJPH::|[<\"]Jolt/"), ["src/engine/physics3d/"], ["src/", "samples/", "games/", "tests/"]),
    ("Box2D wall", re.compile(r"\bb2[A-Z]\w*|[<\"]box2d/"), ["tests/test_physics2d/"], ["src/", "samples/", "games/", "tests/", "tools/"]),
    (
        "engine container ban",
        re.compile(r"std::(vector|unordered_map|unordered_set|string\b|list\b|map\b)|#include\s*<(iostream|sstream|fstream)>"),
        [],
        ["src/engine/"],
    ),
]


def main() -> int:
    violations = []
    for d in SCAN_DIRS:
        base = ROOT / d
        if not base.exists():
            continue
        for path in base.rglob("*"):
            if path.suffix not in EXTS or not path.is_file():
                continue
            rel = path.relative_to(ROOT).as_posix()
            lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
            for name, pattern, allowed, scanned in WALLS:
                if not any(rel.startswith(s) for s in scanned):
                    continue
                if any(rel.startswith(a) for a in allowed):
                    continue
                for no, line in enumerate(lines, 1):
                    if "mce-allow" in line:
                        continue
                    if pattern.search(line):
                        violations.append(f"{rel}:{no}: {name}: {line.strip()}")
    if violations:
        print("Wall violations:")
        for v in violations:
            print("  " + v)
        return 1
    print("All walls hold.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
