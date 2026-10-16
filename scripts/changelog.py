#!/usr/bin/env python3
"""CHANGELOG fragments (towncrier model): one file per PR in changelog.d/, merged into CHANGELOG.md later.

    scripts/changelog.py --check      # walls stage: every fragment is well formed (exit 1 otherwise)
    scripts/changelog.py              # collect: append the fragments to `## Unreleased`, delete them
    scripts/changelog.py --dry-run    # print what collect would append

A fragment is changelog.d/<ticket>.md (SBA-127.md, SBA-127-polish.md): one or more `- ` bullets, continuation
lines indented. README.md in the directory is documentation, not a fragment. scripts/bump_version.py collects
before it cuts a release. Entries are appended in ticket-number order (then name), after the entries already
under `## Unreleased`.
"""

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
DIR = ROOT / "changelog.d"
CHANGELOG = ROOT / "CHANGELOG.md"
UNRELEASED = "## Unreleased"
NAME_RE = re.compile(r"^[A-Za-z][A-Za-z0-9]*-[0-9]+(-[a-z0-9][a-z0-9-]*)?\.md$")


def fragments():
    return sorted((p for p in DIR.glob("*.md") if p.name != "README.md"), key=sort_key) if DIR.is_dir() else []


def sort_key(path):
    m = re.match(r"^([A-Za-z]+)-([0-9]+)(.*)\.md$", path.name)
    return (m.group(1), int(m.group(2)), m.group(3)) if m else ("", 0, path.name)


def problems(path):
    out = []
    if not NAME_RE.match(path.name):
        out.append("name must be <ticket>.md, e.g. SBA-127.md or SBA-127-polish.md")
    text = path.read_text(encoding="utf-8")
    lines = text.rstrip("\n").splitlines()
    if not lines:
        return out + ["empty"]
    if not lines[0].startswith("- "):
        out.append("first line must start with `- `")
    for no, line in enumerate(lines, 1):
        if line.strip() and not (line.startswith("- ") or line.startswith("  ")):
            out.append(f"line {no}: continuation lines are indented by two spaces")
        if line.rstrip() != line:
            out.append(f"line {no}: trailing whitespace")
    return out


def check():
    bad = 0
    files = fragments()
    for p in files:
        for e in problems(p):
            print(f"changelog.d/{p.name}: {e}")
            bad += 1
    if not bad:
        print(f"changelog fragments: {len(files)} well formed")
    return 1 if bad else 0


def collect(dry):
    files = fragments()
    if not files:
        print("changelog.d: no fragments")
        return 0
    if check():
        return 1
    entries = "".join(p.read_text(encoding="utf-8").rstrip("\n") + "\n" for p in files)
    if dry:
        sys.stdout.write(entries)
        return 0
    text = CHANGELOG.read_text(encoding="utf-8")
    start = text.find(UNRELEASED)
    if start < 0:
        print(f"CHANGELOG.md: no `{UNRELEASED}` section")
        return 1
    nxt = re.compile(r"^## ", re.M).search(text, start + len(UNRELEASED))
    end = nxt.start() if nxt else len(text)
    section = text[start:end].rstrip("\n") + "\n" + entries + ("\n" if nxt else "")
    CHANGELOG.write_text(text[:start] + section + text[end:], encoding="utf-8")
    for p in files:
        p.unlink()
    print(f"changelog: {len(files)} fragment(s) appended to {UNRELEASED}")
    return 0


def main(argv):
    if argv == ["--check"]:
        return check()
    if argv in ([], ["--dry-run"]):
        return collect(bool(argv))
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
