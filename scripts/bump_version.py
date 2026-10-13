#!/usr/bin/env python3
"""The engine version, kept in one place and copied everywhere else (docs/VERSIONING.md).

    scripts/bump_version.py 0.10.0   # set the version, open its CHANGELOG section
    scripts/bump_version.py --check  # exit 1 if any copy disagrees with CMakeLists.txt

The source of truth is `project(MeshControlEngine VERSION x.y.z)` in CMakeLists.txt. Its copies:
the `engine_version` of every projects/*/*.mceproj, the example in docs/user-guide/02-projects.md,
and the newest `## x.y.z` heading in CHANGELOG.md. The template's mceproj takes the version from
new_project.py at generation time, so it is not a copy.

Bumping renames CHANGELOG.md's `## Unreleased` section to `## x.y.z (date)` and opens an empty
`## Unreleased` above it. It does not commit or tag: the tag `vx.y.z` goes on dev_claude after
the bump's PR merges.
"""

import datetime
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
CMAKE = ROOT / "CMakeLists.txt"
CHANGELOG = ROOT / "CHANGELOG.md"
GUIDE = ROOT / "docs/user-guide/02-projects.md"

CMAKE_RE = re.compile(r"(project\(MeshControlEngine\s+VERSION\s+)([0-9]+\.[0-9]+\.[0-9]+)")
MCEPROJ_RE = re.compile(r'("engine_version"\s*:\s*")([^"]*)(")')
RELEASE_RE = re.compile(r"^## ([0-9]+\.[0-9]+\.[0-9]+)\b", re.M)
UNRELEASED = "## Unreleased"


def rel(path):
    return path.relative_to(ROOT).as_posix()


def cmake_version():
    m = CMAKE_RE.search(CMAKE.read_text(encoding="utf-8"))
    if not m:
        sys.exit(f"{rel(CMAKE)}: no `project(MeshControlEngine VERSION x.y.z)`")
    return m.group(2)


def copies():
    """Every file that repeats the version as an engine_version field."""
    return sorted(ROOT.glob("projects/*/*.mceproj")) + [GUIDE]


def check():
    version = cmake_version()
    errors = []
    for path in copies():
        found = MCEPROJ_RE.findall(path.read_text(encoding="utf-8"))
        if not found:
            errors.append(f"{rel(path)}: no engine_version")
        errors += [f"{rel(path)}: engine_version {v!r}, expected {version!r}" for _, v, _ in found if v != version]
    if not CHANGELOG.is_file():
        errors.append(f"{rel(CHANGELOG)}: missing")
    else:
        text = CHANGELOG.read_text(encoding="utf-8")
        if UNRELEASED not in text:
            errors.append(f"{rel(CHANGELOG)}: no `{UNRELEASED}` section")
        m = RELEASE_RE.search(text)
        if not m or m.group(1) != version:
            errors.append(f"{rel(CHANGELOG)}: newest release is {m.group(1) if m else 'none'}, expected {version}")
    for e in errors:
        print(f"version check: {e}")
    if not errors:
        print(f"version check: {version} everywhere")
    return 1 if errors else 0


def bump(version):
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version):
        sys.exit(f"not a version: {version!r} (want x.y.z)")
    old = cmake_version()
    if tuple(map(int, version.split("."))) <= tuple(map(int, old.split("."))):
        sys.exit(f"{version} is not newer than {old}")

    text = CHANGELOG.read_text(encoding="utf-8")
    if UNRELEASED not in text:
        sys.exit(f"{rel(CHANGELOG)}: no `{UNRELEASED}` section to release")
    today = datetime.date.today().isoformat()
    CHANGELOG.write_text(text.replace(UNRELEASED, f"{UNRELEASED}\n\n## {version} ({today})", 1), encoding="utf-8")

    CMAKE.write_text(CMAKE_RE.sub(rf"\g<1>{version}", CMAKE.read_text(encoding="utf-8"), count=1), encoding="utf-8")
    for path in copies():
        path.write_text(MCEPROJ_RE.sub(rf"\g<1>{version}\g<3>", path.read_text(encoding="utf-8")), encoding="utf-8")
    print(f"{old} -> {version}. After the PR merges: git tag v{version} origin/dev_claude && git push origin v{version}")
    return check()


def main(argv):
    if argv == ["--check"]:
        return check()
    if len(argv) == 1 and not argv[0].startswith("-"):
        return bump(argv[0])
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
